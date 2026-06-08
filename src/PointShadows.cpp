#include "PointShadows.h"
#include "PointShadowsOgre.h"
#include "DustLog.h"
#include "SceneAccess.h"
#include "GeometryReplay.h"
#include "GeometryCapture.h"
#include "ShaderDatabase.h"
#include "D3D11StateBlock.h"

#include <d3d11.h>
#include <math.h>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <cstdint>

namespace PointShadows
{

static const UINT  kSize      = 512;
static const int   kNumLights = 16;      // N nearest point lights to shadow each frame
static const int   kDbgFace   = 3;       // -Y (down): the face that usually sees ground/buildings
static const float kFaceNear  = 1.0f;    // must match the PerspLH below + the light_fs reconstruct
static const float kFaceFar   = 8000.0f;
static const float kCullRadius = 100000.0f; // per-light occluder cull radius (BRUTE FORCE: off)

// Single cube-array: ArraySize = 6 * kNumLights, sampled as a TextureCubeArray in
// light_fs. Avoids hand-writing N TextureCube slots and scales to any N.
static ID3D11Texture2D*          sCubeArrayTex = nullptr;          // R32_TYPELESS, 6*N slices, CUBE
static ID3D11DepthStencilView*   sFaceDSV[kNumLights * 6] = {};    // one per (cube,face)
static ID3D11ShaderResourceView* sCubeArraySRV = nullptr;          // TEXTURECUBEARRAY R32 (t11)
static ID3D11ShaderResourceView* sDebugSRV    = nullptr;           // 2D view of cube 0 face 3 (ImGui preview)
static ID3D11DepthStencilState*  sDepthState  = nullptr;
static ID3D11RasterizerState*    sRasterState = nullptr;
static ID3D11SamplerState*       sPtSampler   = nullptr;   // point-clamp cube sampler (s11)
static ID3D11Buffer*             sPtCB        = nullptr;   // DustPointShadowParams (b8)
static D3D11StateBlock           sStateBlock;
static bool sReady  = false;
static bool sFailed = false;

// Debug: tint point-light-lit pixels (green=lit, red=shadowed, blue=no matching
// cube) so we can see whether the in-shader light->cube match works in-scene.
static bool  sDebugViz    = false;   // green/red/yellow/blue debug tint (off: real hard shadows via color*=0)

// Active light snapshot — filled by RenderFrame, consumed when building the CB.
static int   sActiveCount = 0;
static float sActivePos[kNumLights][3] = {};
static float sActiveRadius[kNumLights] = {};
static void* sActiveLight[kNumLights] = {};   // Ogre::Light* handles for the selected lights

// OGRE->shader-space translation (set per frame from D3D11Hook's ProbeLightCB).
// Cube centers are mapped into shader space (center - R) so the in-shader match
// against light_fs's `position` (rebased space) hits. Sampling is translation-
// invariant so the cube render itself stays in OGRE space.
static float sLightSpaceR[3]   = {0,0,0};
static bool  sLightSpaceRValid = false;

void SetLightSpaceR(const float* r3, bool valid)
{
    if (r3) { sLightSpaceR[0] = r3[0]; sLightSpaceR[1] = r3[1]; sLightSpaceR[2] = r3[2]; }
    sLightSpaceRValid = valid;
}

void GetLightSpaceR(float* out3, bool* valid)
{
    if (out3)  { out3[0] = sLightSpaceR[0]; out3[1] = sLightSpaceR[1]; out3[2] = sLightSpaceR[2]; }
    if (valid) *valid = sLightSpaceRValid;
}

static float sRenderCamPos[3]    = {0,0,0};
static bool  sRenderCamPosValid  = false;

void SetRenderCamPos(const float* p3, bool valid)
{
    if (p3) { sRenderCamPos[0]=p3[0]; sRenderCamPos[1]=p3[1]; sRenderCamPos[2]=p3[2]; }
    sRenderCamPosValid = valid;
}

static float sDrawnLights[256][3] = {};
static int   sDrawnCount = 0;
static float sDrawnCamPos[3] = {0,0,0};
static bool  sDrawnCamPosValid = false;

void SetDrawnLights(const float (*positions)[3], int count, const float* renderCamPos)
{
    if (count > 256) count = 256;
    if (count < 0)   count = 0;
    for (int i = 0; i < count; ++i)
    {
        sDrawnLights[i][0] = positions[i][0];
        sDrawnLights[i][1] = positions[i][1];
        sDrawnLights[i][2] = positions[i][2];
    }
    sDrawnCount = count;
    if (renderCamPos)
    {
        sDrawnCamPos[0]=renderCamPos[0]; sDrawnCamPos[1]=renderCamPos[1]; sDrawnCamPos[2]=renderCamPos[2];
        sDrawnCamPosValid = true;
    }
}

// Rebuild the b8 match CB: posRadius[kNumLights] (cube centers mapped into the
// shader's rebased space, center = OGRE_center - R) then meta. Called from RenderFrame
// (with last frame's R) and again mid-light-pass with the current frame's R so the
// match has no 1-frame lag.
void UpdateLightTableCB(ID3D11DeviceContext* ctx)
{
    if (!ctx || !sPtCB) return;
    float cbData[4 * (kNumLights + 1)] = {};
    for (int c = 0; c < kNumLights; ++c)
    {
        if (c < sActiveCount)
        {
            cbData[c*4+0] = sActivePos[c][0] - (sLightSpaceRValid ? sLightSpaceR[0] : 0.0f);
            cbData[c*4+1] = sActivePos[c][1] - (sLightSpaceRValid ? sLightSpaceR[1] : 0.0f);
            cbData[c*4+2] = sActivePos[c][2] - (sLightSpaceRValid ? sLightSpaceR[2] : 0.0f);
            cbData[c*4+3] = sActiveRadius[c];
        }
    }
    float* meta = &cbData[4 * kNumLights];
    meta[0] = kFaceNear; meta[1] = kFaceFar; meta[2] = (float)sActiveCount;
    meta[3] = sDebugViz ? 1.0f : 0.0f;   // w: shader debug tint (green=lit/red=shadow)
    ctx->UpdateSubresource(sPtCB, 0, nullptr, cbData, 0, 0);
}

void* GetDebugDepthSRV() { return sDebugSRV; }

void Init()
{
    GeometryCapture::SetCaptureFlags(GeometryCapture::GetCaptureFlags() | 0x2u);
    Log("PointShadows: geometry capture enabled (flags=0x%X)", GeometryCapture::GetCaptureFlags());
}

static bool EnsureResources(ID3D11Device* device)
{
    if (sReady)  return true;
    if (sFailed) return false;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kSize; td.Height = kSize; td.MipLevels = 1; td.ArraySize = 6 * kNumLights;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &sCubeArrayTex))) { sFailed = true; Log("PointShadows: cube-array tex failed"); return false; }

    for (int c = 0; c < kNumLights; ++c)
    {
        for (int f = 0; f < 6; ++f)
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
            dvd.Format = DXGI_FORMAT_D32_FLOAT;
            dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            dvd.Texture2DArray.FirstArraySlice = c * 6 + f;
            dvd.Texture2DArray.ArraySize = 1;
            dvd.Texture2DArray.MipSlice = 0;
            if (FAILED(device->CreateDepthStencilView(sCubeArrayTex, &dvd, &sFaceDSV[c*6+f]))) { sFailed = true; Log("PointShadows: cube %d face DSV %d failed", c, f); return false; }
        }
    }

    // Cube-array SRV (sampled in light_fs as TextureCubeArray).
    D3D11_SHADER_RESOURCE_VIEW_DESC cs = {};
    cs.Format = DXGI_FORMAT_R32_FLOAT;
    cs.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    cs.TextureCubeArray.MostDetailedMip = 0;
    cs.TextureCubeArray.MipLevels = 1;
    cs.TextureCubeArray.First2DArrayFace = 0;
    cs.TextureCubeArray.NumCubes = kNumLights;
    device->CreateShaderResourceView(sCubeArrayTex, &cs, &sCubeArraySRV);

    // Debug 2D view of cube 0, face 3 (-Y) for the ImGui preview.
    D3D11_SHADER_RESOURCE_VIEW_DESC ds = {};
    ds.Format = DXGI_FORMAT_R32_FLOAT;
    ds.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    ds.Texture2DArray.MostDetailedMip = 0;
    ds.Texture2DArray.MipLevels = 1;
    ds.Texture2DArray.FirstArraySlice = kDbgFace;
    ds.Texture2DArray.ArraySize = 1;
    device->CreateShaderResourceView(sCubeArrayTex, &ds, &sDebugSRV);

    // Point-clamp sampler for the depth cubes (s11): a hard compare; no filtering.
    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&smp, &sPtSampler);

    // DustPointShadowParams (b8): float4 posRadius[4] + float4 meta = 80 bytes.
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16 * (kNumLights + 1);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device->CreateBuffer(&cbd, nullptr, &sPtCB);

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dsd.DepthFunc = D3D11_COMPARISON_LESS;
    device->CreateDepthStencilState(&dsd, &sDepthState);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_BACK; rd.DepthClipEnable = TRUE;
    rd.DepthBias = 100; rd.SlopeScaledDepthBias = 2.0f;
    device->CreateRasterizerState(&rd, &sRasterState);

    sReady = true;
    Log("PointShadows: cube resources created (%d cubes x 6 x %ux%u)", kNumLights, kSize, kSize);
    return true;
}

// ---- row-major math (D3D LH, depth 0..1, row-vector convention v*M) ----
static void Mul(float* o, const float* a, const float* b)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            o[r*4+c] = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c] + a[r*4+2]*b[2*4+c] + a[r*4+3]*b[3*4+c];
}
static void Norm(float* v) { float l = sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if (l > 1e-6f) { v[0]/=l; v[1]/=l; v[2]/=l; } }
static void Cross(float* o, const float* a, const float* b) { o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0]; }
static float Dot(const float* a, const float* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }

static void LookAtLH(float* m, const float* eye, const float* target, const float* up)
{
    float z[3] = { target[0]-eye[0], target[1]-eye[1], target[2]-eye[2] }; Norm(z);
    float x[3]; Cross(x, up, z); Norm(x);
    float y[3]; Cross(y, z, x);
    m[0]=x[0]; m[1]=y[0]; m[2]=z[0]; m[3]=0;
    m[4]=x[1]; m[5]=y[1]; m[6]=z[1]; m[7]=0;
    m[8]=x[2]; m[9]=y[2]; m[10]=z[2]; m[11]=0;
    m[12]=-Dot(x,eye); m[13]=-Dot(y,eye); m[14]=-Dot(z,eye); m[15]=1;
}
static void PerspLH(float* m, float fovY, float aspect, float zn, float zf)
{
    float ys = 1.0f / tanf(fovY * 0.5f);
    float xs = ys / aspect;
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0]=xs; m[5]=ys; m[10]=zf/(zf-zn); m[11]=1.0f; m[14]=-zn*zf/(zf-zn);
}

// Standard D3D cube-face directions / ups.
static const float kFaceDir[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
static const float kFaceUp [6][3] = { {0,1,0}, {0,1,0}, {0,0,-1}, {0,0,1}, {0,1,0}, {0,1,0} };

// One-shot diagnostic: what's captured, and where is it in world space?
// Category ints: see ShaderDatabase (OBJECTS/TERRAIN/FOLIAGE/SKIN/TRIPLANAR/DISTANT_TOWN/UNKNOWN).
static void DumpCapturesOnce(ID3D11DeviceContext* ctx, const float* camPos3)
{
    static bool done = false;
    if (done) return;
    const auto& caps = GeometryCapture::GetCaptures();
    if (caps.size() < 100) return;   // wait for a representative (loaded) scene
    done = true;

    int hist[8] = {0};
    for (size_t i = 0; i < caps.size(); ++i)
    {
        int ci = (int)ShaderDatabase::GetVertexShaderCategory(caps[i].vs);
        if (ci < 0 || ci > 7) ci = 7;
        hist[ci]++;
    }
    Log("PointShadows DIAG: cam=(%.0f,%.0f,%.0f)  %u captures  cat[0..6,oth]=%d,%d,%d,%d,%d,%d,%d,%d",
        camPos3[0], camPos3[1], camPos3[2], (unsigned)caps.size(),
        hist[0],hist[1],hist[2],hist[3],hist[4],hist[5],hist[6],hist[7]);

    int logged = 0;
    for (size_t i = 0; i < caps.size() && logged < 12; ++i)
    {
        const CapturedDraw& d = caps[i];
        if (!d.vsMetadata || d.vsMetadata->transformType == VSTransformType::UNKNOWN) continue;
        int cat = (int)ShaderDatabase::GetVertexShaderCategory(d.vs);
        float wx = 0, wy = 0, wz = 0; bool gotW = false;
        if (d.cbStagingCopy && d.vsMetadata->worldMatrixOffset + 64 <= d.cbStagingSize)
        {
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(d.cbStagingCopy, 0, D3D11_MAP_READ, 0, &m)))
            {
                const float* w = (const float*)((const char*)m.pData + d.vsMetadata->worldMatrixOffset);
                wx = w[12]; wy = w[13]; wz = w[14]; gotW = true;
                ctx->Unmap(d.cbStagingCopy, 0);
            }
        }
        Log("  cap[%u] cat=%d inst=%u world=(%.0f,%.0f,%.0f)%s",
            (unsigned)i, cat, d.instanceCount, wx, wy, wz, gotW ? "" : " (noCB)");
        ++logged;
    }
}

void RenderFrame(ID3D11DeviceContext* ctx, ID3D11Device* device, const float* camPos3)
{
    if (!ctx || !device || !camPos3) return;
    if (!EnsureResources(device)) return;

    DumpCapturesOnce(ctx, camPos3);

    // Drive the cubes from the point lights the DEFERRED PASS actually drew this frame, in
    // RENDER-WORLD space (light_fs position[12..14]) — the same frame as the GBuffer geometry.
    // No R, no SceneAccess, no conversion: cube center, replay POV, and the light_fs match all
    // use these exact positions. Select the kNumLights nearest to the render-world camera.
    if (sDrawnCount <= 0) return;
    struct Cand { float d; int i; };
    static std::vector<Cand> cand;
    cand.clear();
    for (int i = 0; i < sDrawnCount; ++i)
    {
        float dx = sDrawnLights[i][0]-sDrawnCamPos[0];
        float dy = sDrawnLights[i][1]-sDrawnCamPos[1];
        float dz = sDrawnLights[i][2]-sDrawnCamPos[2];
        cand.push_back({ dx*dx + dy*dy + dz*dz, i });
    }
    int sel = (int)cand.size(); if (sel > kNumLights) sel = kNumLights;
    for (int a = 0; a < sel; ++a)
    {
        int m = a;
        for (int b = a+1; b < (int)cand.size(); ++b) if (cand[b].d < cand[m].d) m = b;
        Cand t = cand[a]; cand[a] = cand[m]; cand[m] = t;
        sActivePos[a][0] = sDrawnLights[cand[a].i][0];
        sActivePos[a][1] = sDrawnLights[cand[a].i][1];
        sActivePos[a][2] = sDrawnLights[cand[a].i][2];
    }
    sActiveCount = sel;
    if (sel <= 0) return;

    // sActivePos is ALREADY render-world -> the cube CENTER (match) stays render-world.
    sLightSpaceR[0] = sLightSpaceR[1] = sLightSpaceR[2] = 0.0f;
    sLightSpaceRValid = false;

    // The replayed GBuffer geometry is in OGRE-ABSOLUTE frame (objects.hlsl: oPosition =
    // worldViewProjMatrix * position, world matrix absolute), while the deferred light_fs
    // (sActivePos = light position[12..14]) is REBASED (render-world). They differ by a constant
    // worldOffset = (absolute) - (rebased), which OGRE camera-relative rendering makes equal to
    // the render-world camera position (confirmed: the SceneAccess-vs-deferred vote == RenderCamPos).
    // So lp_absolute = sActivePos + cameraPos, while the match center stays rebased (sActivePos).
    // Use the stable view-matrix camera position (sDrawnCamPos), not the noisy vote.
    float worldOffset[3] = { sDrawnCamPos[0], sDrawnCamPos[1], sDrawnCamPos[2] };

    // Render each light's 6 cube faces by replaying the captured GBuffer draws from the
    // light's POV (GeometryReplay) with our depth-only output. CRITICAL: the GBuffer geometry
    // is in RENDER-WORLD (worldViewProj uses worldMatrix=identity), so the replay light POV
    // must also be render-world = sActivePos - R (== light_fs `position`, the match center).
    // The old path used sActivePos (wrong frame) -> misplacement; this is the fix.
    float proj[16];
    PerspLH(proj, 1.5708f, 1.0f, kFaceNear, kFaceFar);   // 90 deg per face

    sStateBlock.Capture(ctx);
    ID3D11RenderTargetView* nullRTV[1] = { nullptr };
    D3D11_VIEWPORT port = {}; port.Width = (float)kSize; port.Height = (float)kSize; port.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &port);
    ctx->RSSetState(sRasterState);
    ctx->OMSetDepthStencilState(sDepthState, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);

    uint32_t totalReplayed = 0;
    for (int c = 0; c < sel; ++c)
    {
        // Replay POV in OGRE-ABSOLUTE frame = rebased light + worldOffset (= the SceneAccess
        // absolute light position). The match center (UpdateLightTableCB) stays rebased.
        float lp[3] = { sActivePos[c][0]+worldOffset[0], sActivePos[c][1]+worldOffset[1], sActivePos[c][2]+worldOffset[2] };
        for (int f = 0; f < 6; ++f)
        {
            float target[3] = { lp[0]+kFaceDir[f][0], lp[1]+kFaceDir[f][1], lp[2]+kFaceDir[f][2] };
            float view[16], vp[16];
            LookAtLH(view, lp, target, kFaceUp[f]);
            Mul(vp, view, proj);
            ctx->OMSetRenderTargets(1, nullRTV, sFaceDSV[c*6+f]);
            ctx->ClearDepthStencilView(sFaceDSV[c*6+f], D3D11_CLEAR_DEPTH, 1.0f, 0);
            totalReplayed += GeometryReplay::Replay(ctx, device, vp, lp, kCullRadius);
        }
    }
    sStateBlock.Restore(ctx);

    UpdateLightTableCB(ctx);

    static int frame = 0;
    if (((frame++) % 120) == 0)
        Log("PointShadows: nLights=%d rebasedL0=(%.0f,%.0f,%.0f) worldOffset=(%.0f,%.0f,%.0f) replayed %u",
            sActiveCount, sActivePos[0][0], sActivePos[0][1], sActivePos[0][2],
            worldOffset[0], worldOffset[1], worldOffset[2], totalReplayed);
}

int GetMaxCubes() { return kNumLights; }

void CopyOgreFaceDepth(ID3D11DeviceContext* ctx, int cube, int face, ID3D11Texture2D* src)
{
    if (!ctx || !src || !sCubeArrayTex) return;
    if (cube < 0 || cube >= kNumLights || face < 0 || face >= 6) return;
    // src is R32_FLOAT 512², dst slice is R32_TYPELESS (bit-compatible) cube*6+face.
    UINT dstSub = D3D11CalcSubresource(0, (UINT)(cube * 6 + face), 1);
    ctx->CopySubresourceRegion(sCubeArrayTex, dstSub, 0, 0, 0, src, 0, nullptr);
}

void BindForLightFs(ID3D11DeviceContext* ctx)
{
    if (!ctx || !sReady) return;
    ctx->PSSetShaderResources(11, 1, &sCubeArraySRV);  // t11 (TextureCubeArray)
    ctx->PSSetSamplers(11, 1, &sPtSampler);            // s11
    ctx->PSSetConstantBuffers(8, 1, &sPtCB);           // b8
    // Deliberately not unbound — the additive light-volume (light_fs) draws run after
    // the POST_LIGHTING dispatch and read these. Same rationale as the contact-shadow
    // b7/t10 binds in effects/shadows/DustShadows.cpp.
}

void Shutdown()
{
    for (int i = 0; i < kNumLights * 6; ++i)
        if (sFaceDSV[i]) { sFaceDSV[i]->Release(); sFaceDSV[i] = nullptr; }
    if (sCubeArraySRV) { sCubeArraySRV->Release(); sCubeArraySRV = nullptr; }
    if (sCubeArrayTex) { sCubeArrayTex->Release(); sCubeArrayTex = nullptr; }
    if (sDebugSRV)    { sDebugSRV->Release();    sDebugSRV = nullptr; }
    if (sDepthState)  { sDepthState->Release();  sDepthState = nullptr; }
    if (sRasterState) { sRasterState->Release(); sRasterState = nullptr; }
    if (sPtSampler)   { sPtSampler->Release();   sPtSampler = nullptr; }
    if (sPtCB)        { sPtCB->Release();        sPtCB = nullptr; }
    sReady = false;
}

} // namespace PointShadows
