#include "MotionVectors.h"
#include "GeometryCapture.h"
#include "ShaderMetadata.h"
#include "D3D11StateBlock.h"
#include "D3D11Hook.h"
#include "CameraAccess.h"
#include "DustLog.h"
#include <d3dcompiler.h>
#include <vector>
#include <cstring>
#include <string>
#include <windows.h>

namespace MotionVectors
{

struct Mat4 { float m[16]; };
struct VelCB { float cur[16]; float reproj[16]; };   // reproj = prevVP * inv(curVP), world-independent

static ID3D11Device*        sDevice = nullptr;
static uint32_t             sW = 0, sH = 0;
static uint32_t             sFrame = 0;
static int                  sDebugViz = -1;   // -1 = not yet read from ini

// Velocity target (RG = screen-space motion in UV units) + own depth for occlusion.
static ID3D11Texture2D*          sVelTex = nullptr;
static ID3D11RenderTargetView*   sVelRTV = nullptr;
static ID3D11ShaderResourceView* sVelSRV = nullptr;
static ID3D11Texture2D*          sVelDepth = nullptr;
static ID3D11DepthStencilView*   sVelDSV = nullptr;
static ID3D11Texture2D*          sVelStaging = nullptr;   // CPU readback for stats

static ID3D11VertexShader*   sVelVS = nullptr;
static ID3D11PixelShader*    sVelPS = nullptr;
static ID3D11VertexShader*   sFsVS  = nullptr;
static ID3D11PixelShader*    sFsPS  = nullptr;
static ID3D11Buffer*         sVelCB = nullptr;
static ID3D11DepthStencilState* sDepthState = nullptr;
static ID3D11RasterizerState*   sRaster = nullptr;
static ID3D11BlendState*        sBlend = nullptr;
static ID3D11SamplerState*      sSampler = nullptr;

static D3D11StateBlock sState;

// ---- shaders -------------------------------------------------------------
static const char* kVelVS = R"(
cbuffer VelCB : register(b0) { float4x4 curWVP; float4x4 reproj; };
struct VOut { float4 pos:SV_Position; float4 cur:TEXCOORD0; float4 prev:TEXCOORD1; };
VOut main(float3 iPos : POSITION) {
    VOut o;
    o.cur  = mul(curWVP, float4(iPos, 1.0));
    o.prev = mul(reproj, o.cur);      // analytic reprojection to previous frame's clip space
    o.pos  = o.cur;
    return o;
})";

static const char* kVelPS = R"(
struct VOut { float4 pos:SV_Position; float4 cur:TEXCOORD0; float4 prev:TEXCOORD1; };
float2 main(VOut i) : SV_Target {
    float2 c = i.cur.xy  / i.cur.w;
    float2 p = i.prev.xy / i.prev.w;
    return (c - p) * float2(0.5, -0.5);   // NDC delta -> UV-space motion
})";

static const char* kFsVS = R"(
struct FOut { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
FOut main(uint id : SV_VertexID) {
    FOut o;
    o.uv  = float2((id<<1)&2, id&2);
    o.pos = float4(o.uv*float2(2,-2)+float2(-1,1), 0, 1);
    return o;
})";

static const char* kFsPS = R"(
Texture2D<float2> velTex : register(t0);
SamplerState smp : register(s0);
struct FOut { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
float4 main(FOut i) : SV_Target {
    float2 mv = velTex.Sample(smp, i.uv);
    float s = 40.0;
    float3 col = float3(0.5,0.5,0.5) + float3(mv.x, mv.y, 0.0) * s;
    col.b = saturate(length(mv) * s);
    return float4(saturate(col), 1.0);
})";

static bool CompileVS(const char* src, ID3D11VertexShader** out)
{
    ID3DBlob* bc = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            "main", "vs_5_0", 0, 0, &bc, &err);
    if (FAILED(hr)) { if (err) { Log("MV VS compile fail: %s", (char*)err->GetBufferPointer()); err->Release(); } return false; }
    hr = sDevice->CreateVertexShader(bc->GetBufferPointer(), bc->GetBufferSize(), nullptr, out);
    bc->Release(); if (err) err->Release();
    return SUCCEEDED(hr);
}
static bool CompilePS(const char* src, ID3D11PixelShader** out)
{
    ID3DBlob* bc = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            "main", "ps_5_0", 0, 0, &bc, &err);
    if (FAILED(hr)) { if (err) { Log("MV PS compile fail: %s", (char*)err->GetBufferPointer()); err->Release(); } return false; }
    hr = sDevice->CreatePixelShader(bc->GetBufferPointer(), bc->GetBufferSize(), nullptr, out);
    bc->Release(); if (err) err->Release();
    return SUCCEEDED(hr);
}

static void ReleaseTargets()
{
    if (sVelRTV) { sVelRTV->Release(); sVelRTV = nullptr; }
    if (sVelSRV) { sVelSRV->Release(); sVelSRV = nullptr; }
    if (sVelTex) { sVelTex->Release(); sVelTex = nullptr; }
    if (sVelDSV) { sVelDSV->Release(); sVelDSV = nullptr; }
    if (sVelDepth) { sVelDepth->Release(); sVelDepth = nullptr; }
    if (sVelStaging) { sVelStaging->Release(); sVelStaging = nullptr; }
}

static bool EnsureResources()
{
    if (!sDevice || sW == 0 || sH == 0) return false;
    if (sVelTex) return true;   // already built at current res

    // Velocity RT: R16G16_FLOAT (signed motion, sub-pixel precision).
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = sW; td.Height = sH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16_FLOAT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(sDevice->CreateTexture2D(&td, nullptr, &sVelTex))) return false;
    sDevice->CreateRenderTargetView(sVelTex, nullptr, &sVelRTV);
    sDevice->CreateShaderResourceView(sVelTex, nullptr, &sVelSRV);

    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(sDevice->CreateTexture2D(&dd, nullptr, &sVelDepth))) { ReleaseTargets(); return false; }
    sDevice->CreateDepthStencilView(sVelDepth, nullptr, &sVelDSV);

    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sDevice->CreateTexture2D(&sd, nullptr, &sVelStaging);

    if (!sVelVS && !CompileVS(kVelVS, &sVelVS)) return false;
    if (!sVelPS && !CompilePS(kVelPS, &sVelPS)) return false;
    if (!sFsVS  && !CompileVS(kFsVS,  &sFsVS))  return false;
    if (!sFsPS  && !CompilePS(kFsPS,  &sFsPS))  return false;

    if (!sVelCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(VelCB); bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        sDevice->CreateBuffer(&bd, nullptr, &sVelCB);
    }
    if (!sDepthState)
    {
        D3D11_DEPTH_STENCIL_DESC ds = {};
        ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc = D3D11_COMPARISON_LESS;
        sDevice->CreateDepthStencilState(&ds, &sDepthState);
    }
    if (!sRaster)
    {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;  // cull-none: safe vs unknown winding
        rd.DepthClipEnable = TRUE;
        sDevice->CreateRasterizerState(&rd, &sRaster);
    }
    if (!sBlend)
    {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        sDevice->CreateBlendState(&bd, &sBlend);
    }
    if (!sSampler)
    {
        D3D11_SAMPLER_DESC smp = {};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sDevice->CreateSamplerState(&smp, &sSampler);
    }
    Log("MotionVectors: velocity resources created (%ux%u)", sW, sH);
    return true;
}

// ---- 4x4 matrix math (column-major, matching HLSL default packing) --------
static void Identity(float* m)
{
    for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}
// r = a*b (column-major: element(row,col) = m[col*4+row])
static void Mul(const float* a, const float* b, float* r)
{
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++)
        {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
}
static bool Inv(const float* m, float* o)
{
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    float det = m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
    if (det == 0.0f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) o[i] = inv[i] * det;
    return true;
}
static float FrobDiff2(const float* a, const float* b)
{
    float s = 0; for (int i = 0; i < 16; i++) { float d = a[i] - b[i]; s += d * d; } return s;
}
static void Transpose(const float* m, float* o)   // column-major
{
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) o[c * 4 + r] = m[r * 4 + c];
}
static void LogMat(const char* name, const float* m)
{
    Log("[MV] %-8s [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
        name, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
        m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

static bool MatClose(const float* a, const float* b, float relEps)
{
    float d = FrobDiff2(a, b), mag = 1e-6f;
    for (int i = 0; i < 16; i++) mag += b[i] * b[i];
    return d / mag < relEps;
}

// Consensus camera view-projection from per-draw candidates (VP = WVP*inv(world),
// a within-frame computation — NO cross-frame matching, so it's immune to frustum
// churn). All correct extractions are ~identical (one camera this frame), scattered
// only by float32 precision (Kenshi's far world coords make inv(world) lossy); a bad
// read (world in a different CB) is a gross outlier. Pick the largest agreeing cluster
// (relaxed threshold to absorb precision scatter) and AVERAGE it for a clean VP.
static bool ConsensusVP(const std::vector<Mat4>& cands, float* out, int& agreeOut)
{
    int n = (int)cands.size();
    agreeOut = 0;
    if (n == 0) return false;
    const float REL = 1e-3f;   // loose enough for float precision, tight vs gross outliers
    int best = -1, bestAgree = -1;
    for (int i = 0; i < n; i++)
    {
        int a = 0;
        for (int j = 0; j < n; j++) if (MatClose(cands[i].m, cands[j].m, REL)) a++;
        if (a > bestAgree) { bestAgree = a; best = i; }
    }
    if (best < 0) return false;
    double acc[16] = { 0 }; int cnt = 0;
    for (int j = 0; j < n; j++)
        if (MatClose(cands[best].m, cands[j].m, REL)) { for (int k = 0; k < 16; k++) acc[k] += cands[j].m[k]; cnt++; }
    for (int k = 0; k < 16; k++) out[k] = (float)(acc[k] / cnt);
    agreeOut = bestAgree;
    return bestAgree * 2 >= n;
}

// Read clip (worldViewProj) and, when present in the same CB, the world matrix, from
// one draw's staging snapshot in a single Map.
static bool ReadClipWorld(ID3D11DeviceContext* ctx, const CapturedDraw& d,
                          Mat4& clip, Mat4& world, bool& hasWorld)
{
    hasWorld = false;
    if (!d.cbStagingCopy || !d.vsMetadata) return false;
    uint32_t co = d.vsMetadata->clipMatrixOffset;
    if (co + 64 > d.cbStagingSize) return false;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ctx->Map(d.cbStagingCopy, 0, D3D11_MAP_READ, 0, &ms))) return false;
    memcpy(clip.m, (const uint8_t*)ms.pData + co, 64);
    uint32_t wo = d.vsMetadata->worldMatrixOffset;
    if (d.vsMetadata->worldMatrixSize >= 64 && wo + 64 <= d.cbStagingSize)
    { memcpy(world.m, (const uint8_t*)ms.pData + wo, 64); hasWorld = true; }
    ctx->Unmap(d.cbStagingCopy, 0);
    return true;
}

void SetDevice(ID3D11Device* device) { sDevice = device; }

void OnResolution(uint32_t width, uint32_t height)
{
    if (width == sW && height == sH) return;
    sW = width; sH = height;
    ReleaseTargets();   // rebuilt lazily at new res
}

bool DebugVizEnabled()
{
    if (sDebugViz < 0)
    {
        std::string ini = DustLogDir() + "Dust.ini";
        sDebugViz = GetPrivateProfileIntA("Upscaling", "ShowMotionVectors", 0, ini.c_str()) ? 1 : 0;
    }
    return sDebugViz == 1;
}

ID3D11ShaderResourceView* GetVelocitySRV() { return sVelSRV; }

static void LogStats(ID3D11DeviceContext* ctx, uint32_t drawn)
{
    // Sparse readback: coverage. Debug-only, every 60 frames.
    if (!sVelStaging) return;
    ctx->CopyResource(sVelStaging, sVelTex);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ctx->Map(sVelStaging, 0, D3D11_MAP_READ, 0, &ms)))
    {
        Log("MV[A.2b]: drawn=%u (readback map failed)", drawn);
        return;
    }
    uint32_t covered = 0, sampled = 0;
    const int STEP = 16;
    for (uint32_t y = 0; y < sH; y += STEP)
    {
        const uint16_t* row = (const uint16_t*)((const uint8_t*)ms.pData + y * ms.RowPitch);
        for (uint32_t x = 0; x < sW; x += STEP)
        {
            // R16G16_FLOAT -> approx via half decode is fiddly; use nonzero test on raw bits
            uint16_t rx = row[x * 2 + 0], ry = row[x * 2 + 1];
            sampled++;
            if (rx != 0 || ry != 0) covered++;
        }
    }
    ctx->Unmap(sVelStaging, 0);
    Log("MV[A.2b]: drawn=%u coverage=%.1f%% (%u/%u samples nonzero)",
        drawn, sampled ? covered * 100.0 / sampled : 0.0, covered, sampled);
}

void RenderVelocity(ID3D11DeviceContext* ctx)
{
    if (!ctx || GeometryCapture::GetCaptureFlags() == 0) return;
    if (!EnsureResources()) return;

    const auto& caps = GeometryCapture::GetCaptures();

    // --- Pass 1: gather STATIC draws + current clip matrices; collect order-matched
    //     (cur,prev) pairs to derive the world-independent global reprojection. ---
    struct Item { const CapturedDraw* d; Mat4 cur; };
    std::vector<Item> items; items.reserve(caps.size());

    // Geometry VP (WVP*inv(world)) from near static objects — the correct, rebase-internal
    // frame. Gather render items too. No engine VP (its deferred frame drifts from the
    // geometry frame under motion), no clamp — measure the raw reproj behaviour.
    struct VpCand { float trans; Mat4 vp; };
    std::vector<VpCand> vpAll;
    for (const auto& d : caps)
    {
        if (!d.vsMetadata || d.vsMetadata->transformType != VSTransformType::STATIC) continue;
        if (d.instanceCount != 1) continue;          // instanced: A.4
        if (!d.indexBuffer || !d.inputLayout || !d.cbStagingCopy) continue;
        Mat4 curM, worldM; bool hasW = false;
        if (!ReadClipWorld(ctx, d, curM, worldM, hasW)) continue;
        items.push_back({ &d, curM });
        if (hasW)
        {
            float invW[16], vp[16];
            if (Inv(worldM.m, invW))
            {
                Mul(curM.m, invW, vp);
                float tr = worldM.m[12]*worldM.m[12] + worldM.m[13]*worldM.m[13] + worldM.m[14]*worldM.m[14];
                VpCand c; c.trans = tr; memcpy(c.vp.m, vp, 64); vpAll.push_back(c);
            }
        }
    }
    // Camera VP straight from the OGRE camera — exact, and consistent (absolute frame) every
    // frame, so reproj = prevVP*inv(curVP) is stable and rebase-safe (clip space is
    // rebase-invariant, so it applies correctly to the rebased-rendered geometry).
    float curVP[16]; bool haveVP = CameraAccess_GetViewProj(curVP);

    // Diagnostic only: geometry VP (rebase-internal frame). Rotation/projection should match
    // the OGRE VP; the translation column differs by the rebase origin.
    float geomVP[16]; int gAgree = 0; bool haveGeom = false;
    {
        std::vector<Mat4> vpUse;
        const int WANT = 24;
        std::vector<bool> used(vpAll.size(), false);
        int take = (int)vpAll.size() < WANT ? (int)vpAll.size() : WANT;
        for (int t = 0; t < take; t++)
        {
            int best = -1; float bd = 1e30f;
            for (size_t i = 0; i < vpAll.size(); i++)
                if (!used[i] && vpAll[i].trans < bd) { bd = vpAll[i].trans; best = (int)i; }
            if (best < 0) break;
            used[best] = true; vpUse.push_back(vpAll[best].vp);
        }
        haveGeom = ConsensusVP(vpUse, geomVP, gAgree);
    }
    float ogreVsGeom = -1.0f;
    if (haveVP && haveGeom)
    {
        float mag = 1e-6f; for (int i = 0; i < 16; i++) mag += geomVP[i] * geomVP[i];
        ogreVsGeom = FrobDiff2(curVP, geomVP) / mag;
    }

    // reproj = prevVP * inv(curVP), used RAW (no clamp/reuse) so we can see the truth.
    static Mat4 sPrevVP; static bool sHaveVP = false;
    static float sLastReproj[16]; static bool sHaveLast = false;
    float reproj[16]; Identity(reproj);
    const char* src = "IDENTITY";
    if (haveVP && sHaveVP)
    {
        float invCur[16];
        if (Inv(curVP, invCur)) { Mul(sPrevVP.m, invCur, reproj); src = "OK"; }
    }
    float rdelta = sHaveLast ? FrobDiff2(reproj, sLastReproj) : 0.0f;
    memcpy(sLastReproj, reproj, sizeof(reproj)); sHaveLast = true;
    if (haveVP) { memcpy(sPrevVP.m, curVP, 64); sHaveVP = true; }
    if ((sFrame % 30) == 0)
        Log("MV[A.2b]: static=%zu vpSrc=%s camStep=%d ogreVsGeom=%.4g reproj=%s rawDelta=%.4g",
            items.size(), haveVP ? "ogre" : "NONE", CameraAccess_Status(), ogreVsGeom, src, rdelta);

    // --- Pass 2: render each static draw with (curWVP, global reproj). ---
    ID3D11Buffer* savedVB[4] = {}; UINT savedStr[4] = {}, savedOff[4] = {};
    ID3D11Buffer* savedIB = nullptr; DXGI_FORMAT savedIF = DXGI_FORMAT_UNKNOWN; UINT savedIBOff = 0;
    ctx->IAGetVertexBuffers(0, 4, savedVB, savedStr, savedOff);
    ctx->IAGetIndexBuffer(&savedIB, &savedIF, &savedIBOff);
    sState.Capture(ctx);

    float clear[4] = { 0, 0, 0, 0 };
    ctx->ClearRenderTargetView(sVelRTV, clear);
    ctx->ClearDepthStencilView(sVelDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ctx->OMSetRenderTargets(1, &sVelRTV, sVelDSV);
    ctx->OMSetDepthStencilState(sDepthState, 0);
    ctx->OMSetBlendState(sBlend, nullptr, 0xffffffff);
    ctx->RSSetState(sRaster);
    D3D11_VIEWPORT vp = { 0, 0, (float)sW, (float)sH, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->VSSetShader(sVelVS, nullptr, 0);
    ctx->PSSetShader(sVelPS, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &sVelCB);

    uint32_t drawn = 0;
    for (const auto& it : items)
    {
        const CapturedDraw& d = *it.d;
        VelCB cb; memcpy(cb.cur, it.cur.m, 64); memcpy(cb.reproj, reproj, 64);
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ctx->Map(sVelCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        { memcpy(ms.pData, &cb, sizeof(cb)); ctx->Unmap(sVelCB, 0); }
        ctx->IASetInputLayout(d.inputLayout);
        ctx->IASetPrimitiveTopology(d.topology);
        ctx->IASetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS,
            (ID3D11Buffer* const*)d.vertexBuffers, d.vbStrides, d.vbOffsets);
        ctx->IASetIndexBuffer(d.indexBuffer, d.indexFormat, d.ibOffset);
        ctx->DrawIndexed(d.indexCount, d.startIndexLocation, d.baseVertexLocation);
        drawn++;
    }

    if ((sFrame % 300) == 0)   // heavy full-RT readback: rare, so its stall isn't a periodic hitch
        LogStats(ctx, drawn);

    sState.Restore(ctx);
    ctx->IASetVertexBuffers(0, 4, savedVB, savedStr, savedOff);
    ctx->IASetIndexBuffer(savedIB, savedIF, savedIBOff);
    for (int i = 0; i < 4; i++) if (savedVB[i]) savedVB[i]->Release();
    if (savedIB) savedIB->Release();

    sFrame++;
}

void DebugBlit(ID3D11DeviceContext* ctx)
{
    if (!ctx || !DebugVizEnabled() || !sVelSRV || !sFsVS || !sFsPS) return;

    sState.Capture(ctx);
    // Draw over the currently-bound RTV (the final image); don't touch OM targets.
    ctx->VSSetShader(sFsVS, nullptr, 0);
    ctx->PSSetShader(sFsPS, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &sVelSRV);
    ctx->PSSetSamplers(0, 1, &sSampler);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* nullVB = nullptr; UINT z = 0;
    ctx->IASetVertexBuffers(0, 1, &nullVB, &z, &z);
    ctx->Draw(3, 0);
    sState.Restore(ctx);
}

void Shutdown()
{
    ReleaseTargets();
    if (sVelVS) { sVelVS->Release(); sVelVS = nullptr; }
    if (sVelPS) { sVelPS->Release(); sVelPS = nullptr; }
    if (sFsVS)  { sFsVS->Release();  sFsVS = nullptr; }
    if (sFsPS)  { sFsPS->Release();  sFsPS = nullptr; }
    if (sVelCB) { sVelCB->Release(); sVelCB = nullptr; }
    if (sDepthState) { sDepthState->Release(); sDepthState = nullptr; }
    if (sRaster) { sRaster->Release(); sRaster = nullptr; }
    if (sBlend) { sBlend->Release(); sBlend = nullptr; }
    if (sSampler) { sSampler->Release(); sSampler = nullptr; }
    sFrame = 0;
}

} // namespace MotionVectors
