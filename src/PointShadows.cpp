#include "PointShadows.h"
#include "DustLog.h"
#include "SceneAccess.h"
#include "GeometryReplay.h"
#include "GeometryCapture.h"
#include "ShaderDatabase.h"
#include "D3D11StateBlock.h"

#include <d3d11.h>
#include <math.h>
#include <vector>

namespace PointShadows
{

static const UINT kSize     = 1024;
static const int  kDbgFace  = 3;     // -Y (down): the face that usually sees ground/buildings

static ID3D11Texture2D*          sCubeTex     = nullptr;   // R32_TYPELESS, ArraySize=6, CUBE
static ID3D11DepthStencilView*   sFaceDSV[6]  = {};        // one per cube face (D32)
static ID3D11ShaderResourceView* sCubeSRV     = nullptr;   // TEXTURECUBE R32 (for sampling later)
static ID3D11ShaderResourceView* sDebugSRV    = nullptr;   // 2D view of one face (ImGui preview)
static ID3D11Texture2D*          sStaging     = nullptr;   // CPU read (all 6 faces)
static ID3D11DepthStencilState*  sDepthState  = nullptr;
static ID3D11RasterizerState*    sRasterState = nullptr;
static D3D11StateBlock           sStateBlock;
static bool sReady  = false;
static bool sFailed = false;

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
    td.Width = kSize; td.Height = kSize; td.MipLevels = 1; td.ArraySize = 6;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &sCubeTex))) { sFailed = true; Log("PointShadows: cube tex failed"); return false; }

    for (int f = 0; f < 6; ++f)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
        dvd.Format = DXGI_FORMAT_D32_FLOAT;
        dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dvd.Texture2DArray.FirstArraySlice = f;
        dvd.Texture2DArray.ArraySize = 1;
        dvd.Texture2DArray.MipSlice = 0;
        if (FAILED(device->CreateDepthStencilView(sCubeTex, &dvd, &sFaceDSV[f]))) { sFailed = true; Log("PointShadows: face DSV %d failed", f); return false; }
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC cs = {};
    cs.Format = DXGI_FORMAT_R32_FLOAT;
    cs.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    cs.TextureCube.MipLevels = 1;
    device->CreateShaderResourceView(sCubeTex, &cs, &sCubeSRV);

    D3D11_SHADER_RESOURCE_VIEW_DESC ds = {};
    ds.Format = DXGI_FORMAT_R32_FLOAT;
    ds.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    ds.Texture2DArray.MostDetailedMip = 0;
    ds.Texture2DArray.MipLevels = 1;
    ds.Texture2DArray.FirstArraySlice = kDbgFace;
    ds.Texture2DArray.ArraySize = 1;
    device->CreateShaderResourceView(sCubeTex, &ds, &sDebugSRV);

    D3D11_TEXTURE2D_DESC sd = td;
    sd.BindFlags = 0; sd.MiscFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    device->CreateTexture2D(&sd, nullptr, &sStaging);

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dsd.DepthFunc = D3D11_COMPARISON_LESS;
    device->CreateDepthStencilState(&dsd, &sDepthState);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_BACK; rd.DepthClipEnable = TRUE;
    rd.DepthBias = 100; rd.SlopeScaledDepthBias = 2.0f;
    device->CreateRasterizerState(&rd, &sRasterState);

    sReady = true;
    Log("PointShadows: cube resources created (6x %ux%u)", kSize, kSize);
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

    static std::vector<SceneAccess::Light> lights;
    int count = SceneAccess::GetLightCount();
    if (count <= 0) return;
    lights.resize(count);
    int n = SceneAccess::GetLights(&lights[0], count);

    int best = -1; float bestD = 1e30f;
    for (int i = 0; i < n; ++i)
    {
        if (lights[i].type != SceneAccess::LIGHT_POINT) continue;
        if (lights[i].intensity > 50000.0f) continue;
        float dx = lights[i].pos[0]-camPos3[0], dy = lights[i].pos[1]-camPos3[1], dz = lights[i].pos[2]-camPos3[2];
        float d = dx*dx + dy*dy + dz*dz;
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) return;
    const SceneAccess::Light& L = lights[best];

    float proj[16];
    PerspLH(proj, 1.5708f, 1.0f, 1.0f, 8000.0f);   // 90 deg per face (large-world scale)

    sStateBlock.Capture(ctx);

    ID3D11RenderTargetView* nullRTV[1] = { nullptr };
    D3D11_VIEWPORT port = {}; port.Width = (float)kSize; port.Height = (float)kSize; port.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &port);
    ctx->RSSetState(sRasterState);
    ctx->OMSetDepthStencilState(sDepthState, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);

    uint32_t totalReplayed = 0;
    for (int f = 0; f < 6; ++f)
    {
        float target[3] = { L.pos[0]+kFaceDir[f][0], L.pos[1]+kFaceDir[f][1], L.pos[2]+kFaceDir[f][2] };
        float view[16], vp[16];
        LookAtLH(view, L.pos, target, kFaceUp[f]);
        Mul(vp, view, proj);

        ctx->OMSetRenderTargets(1, nullRTV, sFaceDSV[f]);
        ctx->ClearDepthStencilView(sFaceDSV[f], D3D11_CLEAR_DEPTH, 1.0f, 0);
        totalReplayed += GeometryReplay::Replay(ctx, device, vp);
    }

    sStateBlock.Restore(ctx);

    // periodic verification readback across all 6 faces
    static int frame = 0;
    if (((frame++) % 120) == 0 && sStaging)
    {
        ctx->CopyResource(sStaging, sCubeTex);
        int totalWritten = 0, totalSampled = 0;
        float mn = 1e30f, mx = -1e30f;
        for (int f = 0; f < 6; ++f)
        {
            D3D11_MAPPED_SUBRESOURCE map;
            if (FAILED(ctx->Map(sStaging, f, D3D11_MAP_READ, 0, &map))) continue;
            const unsigned char* base = (const unsigned char*)map.pData;
            for (UINT y = 0; y < kSize; y += 8)
            {
                const float* row = (const float*)(base + y * map.RowPitch);
                for (UINT x = 0; x < kSize; x += 8)
                {
                    float d = row[x]; ++totalSampled;
                    if (d < 1.0f) { ++totalWritten; if (d < mn) mn = d; if (d > mx) mx = d; }
                }
            }
            ctx->Unmap(sStaging, f);
        }
        // Sample an OBJECTS caster's world translation to compare coordinate spaces.
        float ox = 0, oy = 0, oz = 0; bool gotO = false;
        {
            const auto& caps2 = GeometryCapture::GetCaptures();
            for (size_t i = 0; i < caps2.size(); ++i)
            {
                const CapturedDraw& d = caps2[i];
                if (!d.vsMetadata || d.vsMetadata->transformType == VSTransformType::UNKNOWN) continue;
                if ((int)ShaderDatabase::GetVertexShaderCategory(d.vs) != 1) continue;   // OBJECTS
                if (!d.cbStagingCopy || d.vsMetadata->worldMatrixOffset + 64 > d.cbStagingSize) continue;
                D3D11_MAPPED_SUBRESOURCE mm;
                if (SUCCEEDED(ctx->Map(d.cbStagingCopy, 0, D3D11_MAP_READ, 0, &mm)))
                {
                    const float* w = (const float*)((const char*)mm.pData + d.vsMetadata->worldMatrixOffset);
                    ox = w[12]; oy = w[13]; oz = w[14]; gotO = true;
                    ctx->Unmap(d.cbStagingCopy, 0);
                }
                break;
            }
        }
        Log("PointShadows: light=(%.0f,%.0f,%.0f) cam=(%.0f,%.0f,%.0f) objWorld=(%.0f,%.0f,%.0f)%s replayed %u depth %d/%d min=%.4f",
            L.pos[0], L.pos[1], L.pos[2], camPos3[0], camPos3[1], camPos3[2],
            ox, oy, oz, gotO ? "" : "(none)", totalReplayed, totalWritten, totalSampled, (totalWritten ? mn : 1.0f));
    }
}

void Shutdown()
{
    for (int f = 0; f < 6; ++f) if (sFaceDSV[f]) { sFaceDSV[f]->Release(); sFaceDSV[f] = nullptr; }
    if (sCubeSRV)     { sCubeSRV->Release();     sCubeSRV = nullptr; }
    if (sDebugSRV)    { sDebugSRV->Release();    sDebugSRV = nullptr; }
    if (sCubeTex)     { sCubeTex->Release();     sCubeTex = nullptr; }
    if (sStaging)     { sStaging->Release();     sStaging = nullptr; }
    if (sDepthState)  { sDepthState->Release();  sDepthState = nullptr; }
    if (sRasterState) { sRasterState->Release(); sRasterState = nullptr; }
    sReady = false;
}

} // namespace PointShadows
