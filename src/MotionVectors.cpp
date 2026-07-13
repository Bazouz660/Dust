#include "MotionVectors.h"
#include "GeometryCapture.h"
#include "ShaderMetadata.h"
#include "D3D11StateBlock.h"
#include "CameraAccess.h"
#include "DustLog.h"
#include <d3dcompiler.h>
#include <vector>
#include <unordered_map>
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
static ID3D11VertexShader*   sVelSkinVS = nullptr;   // A.3b skinned velocity VS
static ID3D11VertexShader*   sVelInstVS = nullptr;   // A.4 instanced (per-instance world matrix) VS
static ID3D11PixelShader*    sFsPS  = nullptr;
static ID3D11VertexShader*   sFsVS  = nullptr;
static ID3D11PixelShader*    sSharpenPS = nullptr;   // RCAS-lite sharpen for the DLSS output
static ID3D11PixelShader*    sSkyMvPS   = nullptr;   // camera-only MV fill for sky/far-depth pixels
static ID3D11Buffer*         sSharpenCB = nullptr;   // { float2 invRes; float amount; float pad; }
static ID3D11Buffer*         sVelCB = nullptr;
static ID3D11Buffer*         sCurBoneCB = nullptr;    // 80 float3x4 bone palette (this frame)
static ID3D11Buffer*         sPrevBoneCB = nullptr;   // matched previous-frame bone palette
static ID3D11Buffer*         sVpCB = nullptr;         // { curVP, prevVP } for skinned
static ID3D11DepthStencilState* sDepthState = nullptr;
static ID3D11DepthStencilState* sDepthOff   = nullptr;   // no hardware depth (manual PS test vs game depth)
static ID3D11ShaderResourceView* sGameDepthSRV = nullptr;   // R24_UNORM_X8 view of the game GBuffer depth
static ID3D11Resource*           sGameDepthTex = nullptr;   // cache key (raw compare only)
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

// Skinned velocity VS (A.3b): skin the vertex with CURRENT bones (this frame's pose+camera)
// and PREVIOUS bones (last frame's matched pose+camera) to get true per-object motion.
// Bones are worldMatrix3x4Array at CB offset 0 in every skinned shader; VP passed separately.
static const char* kVelSkinVS = R"(
// Mirrors the game's skin.hlsl main_vs EXACTLY (the only skinned deferred shader):
//   #define WEIGHTS 3, #define BONES 60; blendPos = sum_{i<3} mul(bone[idx[i]],pos).xyz*wgt[i];
//   oPosition = mul(viewProjectionMatrix, float4(blendPos,1)).
// skin.hlsl is compiled ROW-major (proof: its float3x4 bones are 48 B/bone -> boneCnt*48 lands
// exactly on clipOff; a column-major float3x4 would be 64 B). So EVERY matrix in this shader's
// CB is row-major: bones AND viewProjectionMatrix. Declare both row_major or D3DCompile's
// column-major default reads them transposed (48B bone stride -> garbage skinning; transposed
// VP -> negative/near-zero w -> radial spike blowup). NOTE the static pass reads objects.hlsl's
// worldViewProj COLUMN-major because that's a *separate* compilation (column-major) — the pack
// convention is per-shader, not engine-wide, so static != skinned here.
// CRITICAL: loop i<3 not i<4 — the vertex BLENDWEIGHT/BLENDINDICES stream is 3-component, so a
// 4th read auto-fills w=1.0 (weight) / w=1 (index) => a full bogus bone added to every vertex.
cbuffer CurBones  : register(b0) { row_major float3x4 curBones[80]; };
cbuffer PrevBones : register(b1) { row_major float3x4 prevBones[80]; };
cbuffer VelVP     : register(b2) { row_major float4x4 curVP; row_major float4x4 prevVP; };
struct VOut { float4 pos:SV_Position; float4 cur:TEXCOORD0; float4 prev:TEXCOORD1; };
// Declare the FULL game main_vs input signature (POSITION, TEXCOORD0, NORMAL, TANGENT0,
// BINORMAL0, BLENDINDICES, BLENDWEIGHT) even though we only use position+blend. The captured
// input layout was built against this full signature; pairing it with a REDUCED signature
// misaligns the IA fetch for trailing (blend) elements even when POSITION reads fine.
VOut main(float4 iPos : POSITION, float2 iTex : TEXCOORD0, float3 iNrm : NORMAL,
          float4 iTan : TANGENT0, float3 iBin : BINORMAL0,
          uint4 bidx : BLENDINDICES, float4 bwgt : BLENDWEIGHT) {
    VOut o;
    // blendPos accumulates float4(worldPos,1)*weight so blendPos.w = sum(weights); feed that .w
    // (NOT a forced 1.0) into mul(VP,blendPos), exactly like the game — verts whose weights don't
    // sum to 1 project correctly only with the carried .w.
    float4 cwv = float4(0,0,0,0), pwv = float4(0,0,0,0);
    [unroll] for (int i = 0; i < 3; i++) {
        uint bi = (uint)bidx[i]; bi = (bi < 60u) ? bi : 0u;
        cwv += float4(mul(curBones[bi],  iPos).xyz, 1.0) * bwgt[i];
        pwv += float4(mul(prevBones[bi], iPos).xyz, 1.0) * bwgt[i];
    }
    o.cur  = mul(curVP,  cwv);
    o.prev = mul(prevVP, pwv);
    o.pos  = o.cur;
    return o;
})";

// Instanced velocity VS (A.4): mirrors objects.hlsl INSTANCED main_vs. The per-instance world
// matrix is 3 float4 ROWS at TEXCOORD1/2/3 (VB slot 1, per-instance); worldPos = mul(worldMatrix,
// pos); clip = mul(wvp, worldPos) where wvp = the CB worldViewProjMatrix (= VP, since the batch's
// world is identity for instancing) read COLUMN-major like the static pass. The instance matrix is
// built from vertex inputs so its convention is fixed by the float4x4(rows) constructor. Instanced
// objects are static -> prev = global camera reproj. Declare the FULL input signature so the IA
// fetches the trailing instance rows correctly (same lesson as skinned).
static const char* kVelInstVS = R"(
cbuffer VelCB : register(b0) { float4x4 wvp; float4x4 reproj; };
struct VOut { float4 pos:SV_Position; float4 cur:TEXCOORD0; float4 prev:TEXCOORD1; };
VOut main(float4 pos:POSITION, float4 nrm:NORMAL, float2 uv:TEXCOORD0, float4 tan:TANGENT,
          float4 im0:TEXCOORD1, float4 im1:TEXCOORD2, float4 im2:TEXCOORD3) {
    VOut o;
    float4x4 wm = float4x4(im0, im1, im2, float4(0,0,0,1));
    float4 wp  = mul(wm, pos);
    o.cur  = mul(wvp, wp);
    o.prev = mul(reproj, o.cur);
    o.pos  = o.cur;
    return o;
})";

static const char* kVelPS = R"(
Texture2D<float> gameDepth : register(t0);
struct VOut { float4 pos:SV_Position; float4 cur:TEXCOORD0; float4 prev:TEXCOORD1; };
float2 main(VOut i) : SV_Target {
    // Tolerant depth test vs the game's GBuffer depth (replaces brittle hardware EQUAL, which
    // flickered on opaque geometry because our re-render's depth differs from the game's by a
    // few ULP / a D24 quantum). Keep the pixel only where THIS draw is the visible surface:
    // absorbs that tiny mismatch, yet the gap to an alpha card's background is far larger.
    float gd = gameDepth.Load(int3(int2(i.pos.xy), 0));
    // ONE-SIDED depth test: discard only where this pixel is meaningfully IN FRONT of the game's
    // recorded surface (smaller NDC z) — i.e. an alpha card's transparent part covering background.
    // Occlusion of surfaces BEHIND is handled by our own LESS depth buffer, so we never need the
    // "farther" side. This never discards the true visible surface (it sits AT gd, not in front of
    // it), so opaque geometry is immune to the far-plane depth-precision error that caused flicker.
    // Adaptive margin absorbs that precision error near z->1; the gap to a card's background is far
    // larger, so cutouts still reject correctly.
    float eps = 5e-4 + 8e-3 * i.pos.z;
    if (i.pos.z < gd - eps) discard;
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
    if (mv.x < -100.0) return float4(1.0, 0.0, 0.0, 1.0);   // sentinel => no coverage => REACTIVE (red)
    float s = 40.0;
    float3 col = float3(0.5,0.5,0.5) + float3(mv.x, mv.y, 0.0) * s;
    col.b = saturate(length(mv) * s);
    return float4(saturate(col), 1.0);
})";

// Contrast-limited sharpen (RCAS-lite) for the DLSS output: adds crispness without ringing/halos or the
// local-contrast boost that a plain unsharp/Clarity pass produces. The sharpened value is clamped to the
// 3x3-cross min/max so it can only recover detail, never overshoot => no dark/bright rims.
static const char* kSharpenPS = R"(
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);
cbuffer CB : register(b0) { float2 invRes; float amount; float _pad; };
struct FOut { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
float4 main(FOut i) : SV_Target {
    float3 c = tex.Sample(smp, i.uv).rgb;
    float3 n = tex.Sample(smp, i.uv + float2(0, -invRes.y)).rgb;
    float3 s = tex.Sample(smp, i.uv + float2(0,  invRes.y)).rgb;
    float3 e = tex.Sample(smp, i.uv + float2( invRes.x, 0)).rgb;
    float3 w = tex.Sample(smp, i.uv + float2(-invRes.x, 0)).rgb;
    float3 avg   = (n + s + e + w) * 0.25;
    float3 sharp = c + (c - avg) * (amount * 2.0);
    float3 mn = min(c, min(min(n, s), min(e, w)));
    float3 mx = max(c, max(max(n, s), max(e, w)));
    return float4(clamp(sharp, mn, mx), 1.0);
})";

// Sky / far-background motion vectors. The sky is a forward pass with no MV injection, so its
// velocity stays 0 from the clear -> under camera rotation FSR/DLSS think it's static and pull
// history from the wrong place -> ghosting + object/sky edge shimmer. Here we fill the velocity
// buffer for far-plane pixels ONLY (depth ~= far) with the camera-only reprojection: a point at
// infinity moves purely by camera rotation, so we reproject a far-plane clip point through the SAME
// column-major dust_reproj (prevVP*inv(curVP)) the game shaders use, with the SAME (0.5,-0.5) UV
// convention. Non-sky pixels are discarded so per-object MVs are preserved untouched.
static const char* kSkyMvPS = R"(
Texture2D<float> depthTex : register(t0);
SamplerState smp : register(s0);
cbuffer DustMVCB : register(b0) { column_major float4x4 dust_reproj; };
struct FOut { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
float2 main(FOut i) : SV_Target {
    float d = depthTex.SampleLevel(smp, i.uv, 0);
    if (d < 0.999) discard;                        // covered geometry keeps its per-object MV
    float2 ndc      = float2(i.uv.x*2-1, 1-i.uv.y*2);
    float4 curClip  = float4(ndc, 1.0, 1.0);       // far plane; rotation dominates at infinity
    float4 prevClip = mul(dust_reproj, curClip);
    float2 prevNDC  = prevClip.xy / prevClip.w;
    return (ndc - prevNDC) * float2(0.5, -0.5);
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
    // Invalidate the game-depth SRV cache — on a resize the depth texture is recreated and its
    // pointer could be reused, which would false-hit the raw-pointer cache key.
    if (sGameDepthSRV) { sGameDepthSRV->Release(); sGameDepthSRV = nullptr; }
    sGameDepthTex = nullptr;
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
    if (!sVelSkinVS && !CompileVS(kVelSkinVS, &sVelSkinVS)) return false;
    if (!sVelInstVS && !CompileVS(kVelInstVS, &sVelInstVS)) return false;
    if (!sFsVS  && !CompileVS(kFsVS,  &sFsVS))  return false;
    if (!sFsPS  && !CompilePS(kFsPS,  &sFsPS))  return false;

    if (!sVelCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(VelCB); bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        sDevice->CreateBuffer(&bd, nullptr, &sVelCB);
    }
    if (!sCurBoneCB)   // 80 float3x4 = 3840 bytes; holds any skinned palette
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 80 * 48; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        sDevice->CreateBuffer(&bd, nullptr, &sCurBoneCB);
        sDevice->CreateBuffer(&bd, nullptr, &sPrevBoneCB);
    }
    if (!sVpCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 128; bd.Usage = D3D11_USAGE_DYNAMIC;   // curVP + prevVP
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        sDevice->CreateBuffer(&bd, nullptr, &sVpCB);
    }
    if (!sDepthState)
    {
        D3D11_DEPTH_STENCIL_DESC ds = {};
        ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc = D3D11_COMPARISON_LESS;
        sDevice->CreateDepthStencilState(&ds, &sDepthState);
    }
    if (!sDepthOff)
    {
        // No hardware depth test/write — the velocity PS does a TOLERANT compare against the
        // game's GBuffer depth instead (a draw contributes velocity only at the pixels it owns,
        // alpha cutouts included, opaque geometry covered without the ULP flicker EQUAL had).
        D3D11_DEPTH_STENCIL_DESC ds = {};
        ds.DepthEnable = FALSE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        sDevice->CreateDepthStencilState(&ds, &sDepthOff);
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
// r = m * v  (column-major: element(row,col) = m[col*4+row])
static void MatVec(const float* m, const float* v, float* r)
{
    for (int row = 0; row < 4; row++)
        r[row] = m[0*4+row]*v[0] + m[1*4+row]*v[1] + m[2*4+row]*v[2] + m[3*4+row]*v[3];
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

void SetDebugViz(bool on) { sDebugViz = on ? 1 : 0; }   // runtime toggle from the GUI

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

    // --- A.3b: gather skinned draws with bone palette + skinnedVP, matched to the previous frame
    //     by MESH IDENTITY (ib+idxCount+baseVtx) + nearest ROOT bone, NOT draw order (the game
    //     reorders/culls skinned draws as the camera moves -> order-matching pairs the wrong
    //     character -> huge bogus MV -> flashing). A gate rejects no-confident-prev -> skip. ---
    struct SkinRec { std::vector<uint8_t> bones; float vp[16];
                     ID3D11Buffer* ib; uint32_t idxCount; INT baseVtx; float root[3]; };
    static std::unordered_map<ID3D11VertexShader*, std::vector<SkinRec>> sPrevSkin;
    std::unordered_map<ID3D11VertexShader*, std::vector<SkinRec>> curSkin;
    struct SkinItem { const CapturedDraw* d; ID3D11VertexShader* vs; uint32_t classIdx; };
    std::vector<SkinItem> skinItems;
    uint32_t skinTotal = 0, skinMatched = 0;

    for (const auto& d : caps)
    {
        if (!d.vsMetadata || d.vsMetadata->transformType != VSTransformType::SKINNED) continue;
        if (!d.cbStagingCopy || !d.indexBuffer || !d.inputLayout) continue;
        uint32_t clipOff  = d.vsMetadata->clipMatrixOffset;
        uint32_t boneOff  = d.vsMetadata->boneArrayOffset;
        uint32_t boneCnt  = d.vsMetadata->boneCount;
        if (clipOff + 64 > d.cbStagingSize) continue;
        if (boneCnt == 0 || boneCnt > 80) continue;                 // need reflected bones; cap to CB
        uint32_t bonesLen = boneCnt * 48;
        if (boneOff + bonesLen > d.cbStagingSize) continue;

        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(ctx->Map(d.cbStagingCopy, 0, D3D11_MAP_READ, 0, &ms))) continue;
        SkinRec rec;
        rec.bones.assign((const uint8_t*)ms.pData + boneOff, (const uint8_t*)ms.pData + boneOff + bonesLen);
        memcpy(rec.vp, (const uint8_t*)ms.pData + clipOff, 64);       // skinned viewProjectionMatrix
        const float* bf = (const float*)rec.bones.data();            // bone0 row-major float3x4
        rec.root[0] = bf[3]; rec.root[1] = bf[7]; rec.root[2] = bf[11];   // bone0 translation (rebased world)
        rec.ib = d.indexBuffer; rec.idxCount = d.indexCount; rec.baseVtx = d.baseVertexLocation;
        ctx->Unmap(d.cbStagingCopy, 0);

        uint32_t idx = (uint32_t)curSkin[d.vs].size();
        curSkin[d.vs].push_back(std::move(rec));
        skinItems.push_back({ &d, d.vs, idx });
        skinTotal++;
    }

    // --- A.4: gather INSTANCED static draws (buildings/props/rocks/foliage batches). Static ->
    //     camera reprojection like the static pass, but rendered with the per-instance world
    //     matrix (objects.hlsl INSTANCED). cur = the CB worldViewProjMatrix (= VP for the batch). ---
    struct InstItem { const CapturedDraw* d; Mat4 cur; };
    std::vector<InstItem> instItems;
    uint32_t instSkipLayout = 0;
    for (const auto& d : caps)
    {
        if (!d.vsMetadata || d.vsMetadata->transformType != VSTransformType::STATIC) continue;
        if (d.instanceCount <= 1) continue;
        if (!d.instVBCopy || !d.indexBuffer || !d.inputLayout || !d.cbStagingCopy) continue;
        // Only handle the objects.hlsl INSTANCED layout: per-instance world rows at TEXCOORD1 slot 1.
        const auto* el = ShaderMetadata::GetInputLayoutElements(d.inputLayout);
        bool ok = false;
        if (el) for (const auto& e : *el)
            if (e.semantic == "TEXCOORD" && e.semanticIndex == 1 &&
                e.slotClass == D3D11_INPUT_PER_INSTANCE_DATA && e.inputSlot == 1) { ok = true; break; }
        if (!ok) { instSkipLayout++; continue; }
        Mat4 curM, worldM; bool hasW = false;
        if (!ReadClipWorld(ctx, d, curM, worldM, hasW)) continue;   // curM = worldViewProjMatrix (=VP)
        instItems.push_back({ &d, curM });
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

    // Clear to a sentinel (not 0 — real static velocity is ~0). Any pixel still == sentinel after
    // the pass got NO coverage (foliage/birds/anything we can't reproduce) => reactive.
    float clear[4] = { -1000.0f, -1000.0f, 0, 0 };
    ctx->ClearRenderTargetView(sVelRTV, clear);

    // Build (once, cached) an SRV of the game's GBuffer depth so the velocity PS can do a tolerant
    // depth compare. The depth is R24G8_TYPELESS w/ SHADER_RESOURCE, viewed as R24_UNORM_X8.
    ID3D11DepthStencilView* gbufDSV = GeometryCapture::GetGBufferDSV();
    if (gbufDSV)
    {
        ID3D11Resource* res = nullptr; gbufDSV->GetResource(&res);
        if (res)
        {
            if (res != sGameDepthTex)   // texture changed (first use / resize) -> rebuild SRV
            {
                if (sGameDepthSRV) { sGameDepthSRV->Release(); sGameDepthSRV = nullptr; }
                D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
                sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                if (FAILED(sDevice->CreateShaderResourceView(res, &sd, &sGameDepthSRV)))
                    Log("MotionVectors: game-depth SRV creation FAILED");
                sGameDepthTex = res;   // raw compare key; sGameDepthSRV holds the actual ref
            }
            res->Release();
        }
    }

    // Occlusion is handled by our OWN depth buffer (LESS + write) — self-consistent across our
    // re-render, so no see-through and none of the game-depth precision flicker. The tolerant PS
    // test vs the game depth SRV (t0) is then only used to REJECT alpha-quad-over-background
    // pixels (large depth gap), which a loose tolerance can do without breaking occlusion.
    ctx->ClearDepthStencilView(sVelDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ctx->OMSetRenderTargets(1, &sVelRTV, sVelDSV);
    ctx->OMSetDepthStencilState(sDepthState, 0);
    ctx->PSSetShaderResources(0, 1, &sGameDepthSRV);
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

    // Skinned velocity (A.3b): render each matched character with cur + prev bone pose. Same
    // OM/depth/raster/viewport/PS as the static pass; just swap the VS + its bone/VP CBs.
    ctx->VSSetShader(sVelSkinVS, nullptr, 0);
    uint32_t skinDrawn = 0;
    // Per-prev-vector "already claimed" flags so matching is one-to-one.
    std::unordered_map<ID3D11VertexShader*, std::vector<char>> prevUsed;
    const float kMatchGate2 = 25.0f * 25.0f;   // rebased world units^2; rejects wrong-char / origin-resnap
    for (const auto& si : skinItems)
    {
        auto pit = sPrevSkin.find(si.vs);
        if (pit == sPrevSkin.end()) continue;
        const SkinRec& cur = curSkin[si.vs][si.classIdx];
        auto& prevVec = pit->second;
        auto& usedVec = prevUsed[si.vs];
        if (usedVec.size() != prevVec.size()) usedVec.assign(prevVec.size(), 0);

        // Match to the same-mesh previous draw whose root bone is nearest (characters barely move
        // between frames). Gate rejects "no real prev" -> skip -> no wrong MV (reactive later).
        int best = -1; float bestD = 1e30f;
        for (size_t j = 0; j < prevVec.size(); j++)
        {
            const SkinRec& pr = prevVec[j];
            if (usedVec[j] || pr.ib != cur.ib || pr.idxCount != cur.idxCount || pr.baseVtx != cur.baseVtx) continue;
            float dx = cur.root[0]-pr.root[0], dy = cur.root[1]-pr.root[1], dz = cur.root[2]-pr.root[2];
            float dd = dx*dx + dy*dy + dz*dz;
            if (dd < bestD) { bestD = dd; best = (int)j; }
        }
        if (best < 0 || bestD > kMatchGate2) continue;   // no confident prev -> skip this character
        usedVec[best] = 1; skinMatched++;
        const SkinRec& prev = prevVec[best];
        const CapturedDraw& d = *si.d;

        D3D11_MAPPED_SUBRESOURCE ms;
        // Zero the full 80-slot CB before uploading the (<=60) real bones, so an out-of-range
        // blend index reads mul(0,pos)=0 (benign) instead of stale garbage from a prior draw.
        if (SUCCEEDED(ctx->Map(sCurBoneCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        { memset(ms.pData, 0, 80 * 48); memcpy(ms.pData, cur.bones.data(), cur.bones.size()); ctx->Unmap(sCurBoneCB, 0); }
        if (SUCCEEDED(ctx->Map(sPrevBoneCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        { memset(ms.pData, 0, 80 * 48); memcpy(ms.pData, prev.bones.data(), prev.bones.size()); ctx->Unmap(sPrevBoneCB, 0); }
        if (SUCCEEDED(ctx->Map(sVpCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        { memcpy(ms.pData, cur.vp, 64); memcpy((uint8_t*)ms.pData + 64, prev.vp, 64); ctx->Unmap(sVpCB, 0); }
        ID3D11Buffer* cbs[3] = { sCurBoneCB, sPrevBoneCB, sVpCB };
        ctx->VSSetConstantBuffers(0, 3, cbs);

        ctx->IASetInputLayout(d.inputLayout);
        ctx->IASetPrimitiveTopology(d.topology);
        ctx->IASetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS,
            (ID3D11Buffer* const*)d.vertexBuffers, d.vbStrides, d.vbOffsets);
        ctx->IASetIndexBuffer(d.indexBuffer, d.indexFormat, d.ibOffset);
        ctx->DrawIndexed(d.indexCount, d.startIndexLocation, d.baseVertexLocation);
        skinDrawn++;
    }
    if ((sFrame % 60) == 0)
        Log("MV[A.3b]: skinned=%u matched=%u drawn=%u", skinTotal, skinMatched, skinDrawn);

    // --- A.4: render instanced static draws with the per-instance world matrix + global reproj. ---
    ctx->VSSetShader(sVelInstVS, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &sVelCB);
    uint32_t instDrawn = 0;
    for (const auto& it : instItems)
    {
        const CapturedDraw& d = *it.d;
        VelCB cb; memcpy(cb.cur, it.cur.m, 64); memcpy(cb.reproj, reproj, 64);
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ctx->Map(sVelCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        { memcpy(ms.pData, &cb, sizeof(cb)); ctx->Unmap(sVelCB, 0); }

        // OGRE recycles the per-instance VB (slot 1) per batch -> bind the captured snapshot there.
        ID3D11Buffer* vbs[CapturedDraw::MAX_VB_SLOTS];
        UINT strides[CapturedDraw::MAX_VB_SLOTS], offsets[CapturedDraw::MAX_VB_SLOTS];
        for (int i = 0; i < (int)CapturedDraw::MAX_VB_SLOTS; i++)
        { vbs[i] = d.vertexBuffers[i]; strides[i] = d.vbStrides[i]; offsets[i] = d.vbOffsets[i]; }
        vbs[1] = d.instVBCopy;

        ctx->IASetInputLayout(d.inputLayout);
        ctx->IASetPrimitiveTopology(d.topology);
        ctx->IASetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS, vbs, strides, offsets);
        ctx->IASetIndexBuffer(d.indexBuffer, d.indexFormat, d.ibOffset);
        ctx->DrawIndexedInstanced(d.indexCount, d.instanceCount, d.startIndexLocation,
                                  d.baseVertexLocation, d.startInstanceLocation);
        instDrawn++;
    }
    if ((sFrame % 60) == 0)
        Log("MV[A.4]: instanced=%zu drawn=%u skipLayout=%u", instItems.size(), instDrawn, instSkipLayout);

    if ((sFrame % 120) == 0)   // coverage census: exactly what the velocity pass is NOT rendering
    {
        uint32_t cTot=0,cUnk=0,cStat=0,cInst=0,cNoCB=0,cNoGeom=0,cSkin=0;
        for (const auto& d : caps)
        {
            cTot++;
            if (!d.vsMetadata || d.vsMetadata->transformType == VSTransformType::UNKNOWN) { cUnk++; continue; }
            if (d.vsMetadata->transformType == VSTransformType::SKINNED) { cSkin++; continue; }
            cStat++;
            if (d.instanceCount != 1) cInst++;
            else if (!d.indexBuffer || !d.inputLayout) cNoGeom++;
            else if (!d.cbStagingCopy) cNoCB++;
        }
        Log("MV[cov]: total=%u unknown=%u static=%u (skip: inst=%u noCB=%u noGeom=%u) skinned=%u",
            cTot, cUnk, cStat, cInst, cNoCB, cNoGeom, cSkin);
    }

    if ((sFrame % 300) == 0)   // heavy full-RT readback: rare, so its stall isn't a periodic hitch
        LogStats(ctx, drawn);

    sState.Restore(ctx);
    ctx->IASetVertexBuffers(0, 4, savedVB, savedStr, savedOff);
    ctx->IASetIndexBuffer(savedIB, savedIF, savedIBOff);
    for (int i = 0; i < 4; i++) if (savedVB[i]) savedVB[i]->Release();
    if (savedIB) savedIB->Release();

    sPrevSkin = std::move(curSkin);   // this frame's poses become next frame's "previous"
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

// ===================== Injected per-object velocity (shader-patched GBuffer output) =====================
static ID3D11Texture2D*          sInjVelTex = nullptr;
static ID3D11RenderTargetView*   sInjVelRTV = nullptr;
static ID3D11ShaderResourceView* sInjVelSRV = nullptr;
static uint32_t sInjW = 0, sInjH = 0;
static ID3D11Buffer* sReprojCB = nullptr;                 // b13: float4x4 reproj (column-major)
static float sInjPrevVP[16]; static bool sInjHavePrev = false;
static bool sInjBegun = false;

// --- Injected skinned prev-bones provisioning (b12 = DustPrevCB: float3x4 bones[60] + float4x4 VP) ---
// The patched skin VS skins each vertex twice — current bones (b0, oDustCur) and the PREVIOUS frame's
// bones (b12, oDustPrev) — to get TRUE animation motion (camera reproj alone gives zero MV for a
// character walking while the camera is still). Prev poses are matched SPATIALLY (mesh identity +
// nearest root bone), exactly like the verified replay path: draw-order matching flashes because the
// game reorders/culls the hundreds of skinned draws per frame. A zero b12 => the shader's guard falls
// back to oDustCur (MV 0), so unmatched / first-frame / not-yet-shadowed draws are safe.
static const uint32_t kB12Size = 60 * 48 + 64;            // 2944: float3x4[60] (row-major, 48B) + float4x4
static ID3D11Buffer*  sInjSkinB12 = nullptr;              // re-Map_WRITE_DISCARDed per skinned draw

struct SkinPose {
    std::vector<uint8_t> b12;      // b12 layout: bones @0 (2880B) + viewProjectionMatrix @2880 (64B)
    float root[3] = {0,0,0};       // bone0 translation (rebased world) — the spatial match key
    ID3D11Buffer* ib = nullptr;    // mesh identity (raw ptr; OGRE reuses the same IB per mesh)
    uint32_t idxCount = 0;
    int32_t  baseVtx = 0;
};
static std::vector<SkinPose>   sPrevSkinPoses;            // last frame's skinned poses (match targets)
static std::vector<uint8_t>    sPrevClaimed;              // one-to-one matching: prev pose already taken
static uint32_t sSkinTotalFrame = 0, sSkinMatchedFrame = 0, sSkinShadowMissFrame = 0;   // per-frame diag
static uint32_t sSkinDiagFrame = 0;

// Bone-palette CB shadow: OGRE Map_WRITE_DISCARDs the skin CB (bones + VP) just before each skin draw.
// We snapshot those bytes at Unmap so InjBindSkinPrev can read THIS frame's root bone at draw time
// (needed to spatially match against last frame) without a GPU read-back stall.
static bool sBoneShadowActive = false;                                        // gate the Map/Unmap fast path
static std::unordered_map<const void*, uint32_t>            sKnownBoneCB;      // buffer -> byte size
static std::unordered_map<const void*, void*>              sMapPending;        // buffer -> pData (Map..Unmap)
static std::unordered_map<const void*, std::vector<uint8_t>> sBoneShadow;      // buffer -> last-unmapped bytes

ID3D11RenderTargetView* InjEnsureVelRTV(ID3D11RenderTargetView* refRTV)
{
    if (!sDevice || !refRTV) return sInjVelRTV;
    ID3D11Resource* res = nullptr; refRTV->GetResource(&res);
    if (!res) return sInjVelRTV;
    uint32_t w = 0, h = 0;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex)
    { D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td); w = td.Width; h = td.Height; tex->Release(); }
    res->Release();
    if (w == 0 || h == 0) return sInjVelRTV;
    if (sInjVelRTV && w == sInjW && h == sInjH) return sInjVelRTV;   // already sized

    if (sInjVelSRV) { sInjVelSRV->Release(); sInjVelSRV = nullptr; }
    if (sInjVelRTV) { sInjVelRTV->Release(); sInjVelRTV = nullptr; }
    if (sInjVelTex) { sInjVelTex->Release(); sInjVelTex = nullptr; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16_FLOAT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(sDevice->CreateTexture2D(&td, nullptr, &sInjVelTex)))
    {
        sDevice->CreateRenderTargetView(sInjVelTex, nullptr, &sInjVelRTV);
        sDevice->CreateShaderResourceView(sInjVelTex, nullptr, &sInjVelSRV);
        sInjW = w; sInjH = h;
        Log("MotionVectors[inject]: velocity RT %ux%u created", w, h);
    }
    return sInjVelRTV;
}

ID3D11Buffer* GetInjReprojCB() { return sReprojCB; }

// ---- A.5 rigid moving-object velocity (weapons/tools/doors on animated bones) ---------------------
// objects.hlsl carries geometry whose WORLD transform can change frame-to-frame (a weapon follows an
// animated hand bone). Camera-only reproj then ghosts it. We spatially match each objects.hlsl draw to
// its previous-frame self (mesh identity + nearest rebased world position, one-to-one, gated) and bind
// that draw's PREVIOUS world-view-proj at b12, so the injected VS (InjRigidVS) reprojects by the
// object's own motion. Current WVP is read at draw time from the SAME Map/Unmap CB shadow the skinned
// path uses; the world position is the WVP origin unprojected by inv(curVP). Unmatched -> zero b12 ->
// the shader falls back to camera reproj (no regression: a static object's prev WVP == the camera reproj).
struct RigidXform { float wvp[16]; ID3D11Buffer* mesh; UINT idxCount; INT baseVtx; };
static std::vector<RigidXform>                                  sPrevRigid, sCurRigid;
static std::vector<uint8_t>                                     sPrevRigidClaimed;
static std::unordered_map<ID3D11Buffer*, std::vector<uint32_t>> sPrevRigidByMesh;   // mesh(VB0) -> prev indices
static ID3D11Buffer*  sInjRigidB12  = nullptr;   // per-draw prevWVP CB (64B, column-major)
static ID3D11Buffer*  sRigidZeroB12 = nullptr;   // persistent all-zero b12 -> shader camera-reproj fallback
static float          sFwdReproj[16] = {}; static bool sHaveFwd = false;   // curVP * inv(prevVP): last->this clip
static uint32_t sRigidTotal = 0, sRigidMatched = 0, sRigidShadowMiss = 0, sRigidDiag = 0;

ID3D11Resource* GetInjVelResource() { return sInjVelTex; }

void InjBeginGBuffer(ID3D11DeviceContext* ctx)
{
    if (!ctx || !sDevice || sInjBegun) return;
    sInjBegun = true;
    if (!sReprojCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 80; bd.Usage = D3D11_USAGE_DYNAMIC;   // 64 reproj + float dust_windDelta (pad to 80)
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        sDevice->CreateBuffer(&bd, nullptr, &sReprojCB);
    }
    // reproj = prevVP * inv(curVP) from the OGRE camera — the exact, frame-consistent, rebase-safe
    // reprojection from A.2. Applied to a draw's current clip pos it yields its previous clip pos.
    float curVP[16]; float reproj[16]; Identity(reproj);
    bool haveVP = CameraAccess_GetViewProj(curVP);
    sHaveFwd = false;
    if (haveVP && sInjHavePrev)
    {
        float invCur[16];
        if (Inv(curVP, invCur)) Mul(sInjPrevVP, invCur, reproj);
        // A.5: forward reproj (last-frame clip -> this-frame clip) for predicted-screen-space matching —
        // built from LAST frame's VP (still in sInjPrevVP here) and this frame's curVP.
        float invPrev[16];
        if (Inv(sInjPrevVP, invPrev)) { Mul(curVP, invPrev, sFwdReproj); sHaveFwd = true; }
    }
    if (haveVP) { memcpy(sInjPrevVP, curVP, 64); sInjHavePrev = true; }
    // Wind-MV time delta: grass_vs recomputes its sway at (time - dust_windDelta) for the previous frame.
    // Kenshi's grass `time` is OGRE ACT_TIME (real render seconds), so a real wall-clock frame delta is the
    // matching unit. Clamp out hitches/first-frame so a stall can't fling the grass MV.
    static LARGE_INTEGER sQpcFreq = {}, sQpcPrev = {};
    if (sQpcFreq.QuadPart == 0) QueryPerformanceFrequency(&sQpcFreq);
    LARGE_INTEGER qpcNow; QueryPerformanceCounter(&qpcNow);
    float windDelta = 1.0f / 60.0f;
    if (sQpcPrev.QuadPart != 0 && sQpcFreq.QuadPart != 0)
    {
        double dt = double(qpcNow.QuadPart - sQpcPrev.QuadPart) / double(sQpcFreq.QuadPart);
        if (dt > 0.0 && dt < 0.1) windDelta = (float)dt;   // ignore >100ms hitches
    }
    sQpcPrev = qpcNow;
    if (sReprojCB)
    {
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ctx->Map(sReprojCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        {
            memcpy(ms.pData, reproj, 64);
            memcpy((uint8_t*)ms.pData + 64, &windDelta, sizeof(float));   // dust_windDelta @ 64
            ctx->Unmap(sReprojCB, 0);
        }
    }
    // Clear to ZERO (static), not a sentinel: un-covered pixels (sky, particles, transparents) are read
    // by DLSS as its motion-vector input. A sentinel there becomes a giant bogus MV -> DLSS fetches
    // garbage history -> the sky and object/sky edges shimmer. Zero = "not moving" (correct for a still
    // camera, a safe approximation when panning). The game shaders overwrite covered pixels with real MVs.
    if (sInjVelRTV) { float c[4] = { 0.f, 0.f, 0.f, 0.f }; ctx->ClearRenderTargetView(sInjVelRTV, c); }
}

void InjEndFrame() { sInjBegun = false; }

// Map/Unmap shadow: snapshot the bytes of a known bone-palette CB as OGRE writes them (WRITE_DISCARD
// just before each skin draw), so InjBindSkinPrev can read the current root bone at draw time.
void InjNoteMap(void* resource, void* pData)
{
    if (!sBoneShadowActive || !resource || !pData) return;
    if (sKnownBoneCB.find((const void*)resource) == sKnownBoneCB.end()) return;
    sMapPending[(const void*)resource] = pData;
}
void InjNoteUnmap(void* resource)
{
    if (!sBoneShadowActive || !resource) return;
    auto itp = sMapPending.find((const void*)resource);
    if (itp == sMapPending.end()) return;
    uint32_t sz = sKnownBoneCB[(const void*)resource];
    std::vector<uint8_t>& dst = sBoneShadow[(const void*)resource];
    if (dst.size() != sz) dst.resize(sz);
    if (sz && itp->second) memcpy(dst.data(), itp->second, sz);
    sMapPending.erase(itp);
}

// POST_LIGHTING: snapshot THIS frame's skinned poses (bones+VP repacked to b12 layout, plus the root
// bone and mesh identity) as the SPATIAL match targets for next frame. GeometryCapture's cbStagingCopy
// is resolved by now, so Map(READ) doesn't stall.
void InjFillSkinPrev(ID3D11DeviceContext* ctx)
{
    if (!ctx) return;
    // Per-frame match diagnostic (this frame's draws matched against last frame's poses). A low
    // matched/total ratio => flashing (misses fall back to camera reproj). shadowMiss = warmup/churn.
    if ((sSkinDiagFrame++ % 120) == 0 && sSkinTotalFrame > 0)
        Log("MV[skin]: matched=%u/%u (%.0f%%) shadowMiss=%u prevPoses=%zu",
            sSkinMatchedFrame, sSkinTotalFrame,
            100.0f * sSkinMatchedFrame / sSkinTotalFrame, sSkinShadowMissFrame, sPrevSkinPoses.size());
    sSkinTotalFrame = 0; sSkinMatchedFrame = 0; sSkinShadowMissFrame = 0;

    sPrevSkinPoses.clear();
    const auto& caps = GeometryCapture::GetCaptures();
    for (const auto& d : caps)
    {
        if (!d.vsMetadata || d.vsMetadata->transformType != VSTransformType::SKINNED) continue;
        if (!d.cbStagingCopy || !d.indexBuffer) continue;
        uint32_t clipOff = d.vsMetadata->clipMatrixOffset;
        uint32_t boneOff = d.vsMetadata->boneArrayOffset;
        uint32_t boneCnt = d.vsMetadata->boneCount;
        if (boneCnt == 0) continue;
        if (boneCnt > 60) boneCnt = 60;           // b12 declares [60]; blendIdx never exceeds it
        uint32_t bonesLen = boneCnt * 48;
        if (clipOff + 64 > d.cbStagingSize || boneOff + bonesLen > d.cbStagingSize) continue;

        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(ctx->Map(d.cbStagingCopy, 0, D3D11_MAP_READ, 0, &ms))) continue;
        SkinPose p;
        p.b12.assign(kB12Size, 0);
        memcpy(p.b12.data(),        (const uint8_t*)ms.pData + boneOff, bonesLen);   // bones @ 0
        memcpy(p.b12.data() + 2880, (const uint8_t*)ms.pData + clipOff, 64);         // VP    @ 2880
        const float* bf = (const float*)((const uint8_t*)ms.pData + boneOff);
        p.root[0] = bf[3]; p.root[1] = bf[7]; p.root[2] = bf[11];   // bone0 translation (rebased world)
        ctx->Unmap(d.cbStagingCopy, 0);
        p.ib = d.indexBuffer; p.idxCount = d.indexCount; p.baseVtx = d.baseVertexLocation;
        sPrevSkinPoses.push_back(std::move(p));
    }
    sPrevClaimed.assign(sPrevSkinPoses.size(), 0);
}

// Draw hook (right after GeometryCapture::OnDrawIndexed): if the just-captured draw is skinned, read
// its CURRENT root bone from the CB shadow, spatially match it to a same-mesh previous pose, and bind
// that pose to b12 before the draw. Match by mesh identity (ib+idxCount+baseVtx) + nearest root, with a
// one-to-one claim and a distance gate — the verified A.3b scheme (draw-order flashes at 100s of draws).
void InjBindSkinPrev(ID3D11DeviceContext* ctx)
{
    if (!ctx || !sDevice) return;
    const auto& caps = GeometryCapture::GetCaptures();
    if (caps.empty()) return;
    const CapturedDraw& d = caps.back();
    if (!d.vsMetadata || d.vsMetadata->transformType != VSTransformType::SKINNED) return;
    sSkinTotalFrame++;

    uint32_t boneOff = d.vsMetadata->boneArrayOffset;
    uint32_t cbSlot  = d.vsMetadata->cbSlot;

    // Learn this draw's bone-palette CB so the Map/Unmap shadow starts capturing it next frame.
    ID3D11Buffer* boneCB = nullptr;
    ctx->VSGetConstantBuffers(cbSlot, 1, &boneCB);
    if (!boneCB) return;
    if (sKnownBoneCB.find((const void*)boneCB) == sKnownBoneCB.end())
    {
        if (sKnownBoneCB.size() > 64) { sKnownBoneCB.clear(); sBoneShadow.clear(); sMapPending.clear(); }  // churn backstop
        D3D11_BUFFER_DESC bd; boneCB->GetDesc(&bd);
        sKnownBoneCB[(const void*)boneCB] = bd.ByteWidth;
        sBoneShadowActive = true;
    }
    auto its = sBoneShadow.find((const void*)boneCB);
    boneCB->Release();
    if (its == sBoneShadow.end()) { sSkinShadowMissFrame++; return; }   // not shadowed yet (warmup) -> reproj
    const std::vector<uint8_t>& cur = its->second;
    if ((size_t)boneOff + 48 > cur.size()) return;

    const float* bf = (const float*)(cur.data() + boneOff);
    float cr[3] = { bf[3], bf[7], bf[11] };               // current root (bone0 translation, rebased world)

    // Nearest same-mesh previous pose whose root is closest (characters barely move frame-to-frame).
    const float kGate2 = 25.0f * 25.0f;                   // rebased world units^2; rejects wrong-char / resnap
    int best = -1; float bestD = 1e30f;
    for (size_t j = 0; j < sPrevSkinPoses.size(); j++)
    {
        if (sPrevClaimed[j]) continue;
        const SkinPose& p = sPrevSkinPoses[j];
        if (p.ib != d.indexBuffer || p.idxCount != d.indexCount || p.baseVtx != d.baseVertexLocation) continue;
        float dx = cr[0]-p.root[0], dy = cr[1]-p.root[1], dz = cr[2]-p.root[2];
        float dd = dx*dx + dy*dy + dz*dz;
        if (dd < bestD) { bestD = dd; best = (int)j; }
    }
    if (best < 0 || bestD > kGate2) return;               // no confident prev -> shader guard -> camera reproj
    sPrevClaimed[best] = 1;
    sSkinMatchedFrame++;

    if (!sInjSkinB12)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = kB12Size; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(sDevice->CreateBuffer(&bd, nullptr, &sInjSkinB12))) return;
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx->Map(sInjSkinB12, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        memcpy(ms.pData, sPrevSkinPoses[best].b12.data(), kB12Size);
        ctx->Unmap(sInjSkinB12, 0);
        ctx->VSSetConstantBuffers(12, 1, &sInjSkinB12);
    }
}

// Draw hook (right after GeometryCapture::OnDrawIndexed): for a STATIC (rigid, objects.hlsl-class) draw,
// read its CURRENT WVP from the CB shadow, unproject the origin to a rebased world position, spatially
// match it to a same-mesh previous draw, and bind that draw's PREVIOUS WVP at b12. objects.hlsl's
// InjRigidVS then reprojects the vertex by the object's OWN motion; a miss binds the zero CB and the
// shader falls back to camera reproj. b12 is bound for EVERY static draw so a preceding skinned draw's
// bone CB can never be misread here (and our 64B b12, if a later unmatched skin draw misreads it, reads
// zero past 64B -> that path's own zero-VP fallback). Non-objects static VS (terrain) ignore b12.
void InjBindRigidPrev(ID3D11DeviceContext* ctx)
{
    if (!ctx || !sDevice) return;
    const auto& caps = GeometryCapture::GetCaptures();
    if (caps.empty()) return;
    const CapturedDraw& d = caps.back();
    if (!d.vsMetadata || d.vsMetadata->transformType != VSTransformType::STATIC || !d.indexBuffer) return;

    if (!sRigidZeroB12)
    {
        float zero[16] = {};
        D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = { zero, 0, 0 };
        if (FAILED(sDevice->CreateBuffer(&bd, &sd, &sRigidZeroB12))) return;
    }
    ID3D11Buffer* toBind = sRigidZeroB12;   // default: shader falls back to camera reproj

    // OPT-IN, OFF BY DEFAULT. Cross-frame identity for rigid objects is unreliable enough that wrong
    // matches regress static geometry, so ship it disabled: bind the zero CB (objects.hlsl then falls
    // back to camera reproj = the original, correct-for-static behaviour) and skip the matcher entirely.
    // Set [Upscaling] WeaponMV=1 to try the experimental per-object weapon/rigid velocity.
    static bool sWMVRead = false, sWMV = false;
    if (!sWMVRead)
    {
        sWMVRead = true;
        sWMV = GetPrivateProfileIntA("Upscaling", "WeaponMV", 0, (DustLogDir() + "Dust.ini").c_str()) != 0;
        Log("MotionVectors: weapon/rigid MV %s", sWMV ? "ENABLED ([Upscaling] WeaponMV=1)"
                                                      : "disabled (default) — objects use camera reproj");
    }
    if (!sWMV) { ctx->VSSetConstantBuffers(12, 1, &sRigidZeroB12); return; }

    uint32_t clipOff = d.vsMetadata->clipMatrixOffset;
    uint32_t cbSlot  = d.vsMetadata->cbSlot;

    // Learn this draw's clip-matrix CB so the Map/Unmap shadow captures it (reuses the skin shadow map).
    ID3D11Buffer* clipCB = nullptr;
    ctx->VSGetConstantBuffers(cbSlot, 1, &clipCB);
    if (clipCB)
    {
        if (sKnownBoneCB.find((const void*)clipCB) == sKnownBoneCB.end())
        {
            if (sKnownBoneCB.size() > 96) { sKnownBoneCB.clear(); sBoneShadow.clear(); sMapPending.clear(); }
            D3D11_BUFFER_DESC bd; clipCB->GetDesc(&bd);
            sKnownBoneCB[(const void*)clipCB] = bd.ByteWidth;
            sBoneShadowActive = true;
        }
        auto it = sBoneShadow.find((const void*)clipCB);
        clipCB->Release();
        if (it != sBoneShadow.end() && (size_t)clipOff + 64 <= it->second.size() && sHaveFwd)
        {
            sRigidTotal++;
            const float* wvp = (const float*)(it->second.data() + clipOff);
            float cw = (wvp[15] != 0.0f) ? 1.0f / wvp[15] : 0.0f;
            float curNDC[2] = { wvp[12]*cw, wvp[13]*cw };            // this object's screen position now

            // Match by PREDICTED screen position: forward-reproject each same-mesh previous draw's origin
            // into this frame (camera reproj = sFwdReproj) and take the nearest. A static object's
            // prediction is EXACT (dd~=0) so it matches ITSELF even amid dense identical furniture; a wrong
            // pairing lands at a different screen spot and loses. The gate bounds one-frame self-motion, so
            // it also rejects a truly-moved object whose own prev is missing (-> camera-reproj fallback).
            ID3D11Buffer* meshKey = d.vertexBuffers[0];   // VB0 (position stream) is a stabler mesh id than the reused IB
            const float kGate2 = 0.04f;                    // NDC^2 (~20% screen radius)
            int best = -1; float bestD = 1e30f;
            auto mb = meshKey ? sPrevRigidByMesh.find(meshKey) : sPrevRigidByMesh.end();
            if (meshKey && mb != sPrevRigidByMesh.end())
                for (uint32_t j : mb->second)
                {
                    if (sPrevRigidClaimed[j]) continue;
                    const RigidXform& pr = sPrevRigid[j];
                    if (pr.idxCount != d.indexCount || pr.baseVtx != d.baseVertexLocation) continue;
                    float po[4] = { pr.wvp[12], pr.wvp[13], pr.wvp[14], pr.wvp[15] };
                    float pc[4]; MatVec(sFwdReproj, po, pc);            // predicted THIS-frame clip of the prev origin
                    float pw = (pc[3] != 0.0f) ? 1.0f / pc[3] : 0.0f;
                    float dx = curNDC[0] - pc[0]*pw, dy = curNDC[1] - pc[1]*pw;
                    float dd = dx*dx + dy*dy;
                    if (dd < bestD) { bestD = dd; best = (int)j; }
                }
            if (best >= 0 && bestD <= kGate2)
            {
                sPrevRigidClaimed[best] = 1;
                sRigidMatched++;
                if (!sInjRigidB12)
                {
                    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DYNAMIC;
                    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    sDevice->CreateBuffer(&bd, nullptr, &sInjRigidB12);
                }
                if (sInjRigidB12)
                {
                    D3D11_MAPPED_SUBRESOURCE ms;
                    if (SUCCEEDED(ctx->Map(sInjRigidB12, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
                    {
                        memcpy(ms.pData, sPrevRigid[best].wvp, 64);
                        ctx->Unmap(sInjRigidB12, 0);
                        toBind = sInjRigidB12;
                    }
                }
            }
            // Record THIS draw as a match target for next frame (VB0 mesh id + this frame's WVP/screen pos).
            if (sCurRigid.size() < 8192)
            {
                RigidXform cur; memcpy(cur.wvp, wvp, 64);
                cur.mesh = meshKey; cur.idxCount = d.indexCount; cur.baseVtx = d.baseVertexLocation;
                sCurRigid.push_back(std::move(cur));
            }
        }
        else if (it == sBoneShadow.end()) sRigidShadowMiss++;   // CB not shadowed yet (warmup) -> reproj
    }
    ctx->VSSetConstantBuffers(12, 1, &toBind);
}

// POST_LIGHTING: promote THIS frame's rigid draws to next frame's match targets, and (re)bucket by mesh
// so the per-draw match is O(same-mesh) not O(all). No GPU read (positions/WVPs were gathered at draw time).
void InjFillRigidPrev(ID3D11DeviceContext*)
{
    if ((sRigidDiag++ % 120) == 0 && sRigidTotal > 0)
        Log("MV[rigid]: matched=%u/%u (%.0f%%) shadowMiss=%u prev=%zu",
            sRigidMatched, sRigidTotal, 100.0f * sRigidMatched / (float)sRigidTotal,
            sRigidShadowMiss, sPrevRigid.size());
    sRigidTotal = 0; sRigidMatched = 0; sRigidShadowMiss = 0;

    sPrevRigid.swap(sCurRigid);
    sCurRigid.clear();
    sPrevRigidClaimed.assign(sPrevRigid.size(), 0);
    sPrevRigidByMesh.clear();
    for (uint32_t i = 0; i < sPrevRigid.size(); i++)
        if (sPrevRigid[i].mesh) sPrevRigidByMesh[sPrevRigid[i].mesh].push_back(i);
}

void InjDebugBlit(ID3D11DeviceContext* ctx)
{
    if (!ctx || !DebugVizEnabled() || !sInjVelSRV) return;
    if (!sFsVS && !CompileVS(kFsVS, &sFsVS)) return;   // blit shaders (independent of replay path)
    if (!sFsPS && !CompilePS(kFsPS, &sFsPS)) return;
    if (!sSampler)
    {
        D3D11_SAMPLER_DESC smp = {};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sDevice->CreateSamplerState(&smp, &sSampler);
    }
    sState.Capture(ctx);
    ctx->VSSetShader(sFsVS, nullptr, 0);
    ctx->PSSetShader(sFsPS, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &sInjVelSRV);
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

// Sharpen srcSRV into dstRTV (both display-res) with the contrast-limited sharpen. amount in [0,1];
// amount<=0 is a no-op (the caller does a plain copy instead). src and dst MUST be different resources.
void SharpenBlit(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srcSRV,
                 ID3D11RenderTargetView* dstRTV, float amount, uint32_t w, uint32_t h)
{
    if (!ctx || !srcSRV || !dstRTV || amount <= 0.0f || w == 0 || h == 0) return;
    if (!sFsVS && !CompileVS(kFsVS, &sFsVS)) return;
    if (!sSharpenPS && !CompilePS(kSharpenPS, &sSharpenPS)) return;
    if (!sSampler)
    {
        D3D11_SAMPLER_DESC smp = {};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sDevice->CreateSamplerState(&smp, &sSampler);
    }
    if (!sSharpenCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(sDevice->CreateBuffer(&bd, nullptr, &sSharpenCB))) return;
    }
    struct { float invW, invH, amt, pad; } cb = { 1.0f / w, 1.0f / h, amount, 0.0f };
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx->Map(sSharpenCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    { memcpy(ms.pData, &cb, sizeof(cb)); ctx->Unmap(sSharpenCB, 0); }

    sState.Capture(ctx);
    ID3D11RenderTargetView* rtv = dstRTV;
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->VSSetShader(sFsVS, nullptr, 0);
    ctx->PSSetShader(sSharpenPS, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srcSRV);
    ctx->PSSetSamplers(0, 1, &sSampler);
    ctx->PSSetConstantBuffers(0, 1, &sSharpenCB);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* nullVB = nullptr; UINT z = 0;
    ctx->IASetVertexBuffers(0, 1, &nullVB, &z, &z);
    ctx->Draw(3, 0);
    sState.Restore(ctx);
}

// Fill camera-only MV into the velocity buffer for sky / far-depth pixels (see kSkyMvPS). Runs at
// POST_TONEMAP just before the upscaler reads the velocity buffer, so per-object MVs are already in
// place and only the uncovered sky is filled. depthSRV is the game's scene depth (R32F, 0=near..1=far).
void InjFillSkyMV(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* depthSRV, uint32_t w, uint32_t h)
{
    if (!ctx || !depthSRV || !sInjVelRTV || !sReprojCB || w == 0 || h == 0) return;
    if (!sFsVS && !CompileVS(kFsVS, &sFsVS)) return;
    if (!sSkyMvPS && !CompilePS(kSkyMvPS, &sSkyMvPS)) return;
    if (!sSampler)
    {
        D3D11_SAMPLER_DESC smp = {};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sDevice->CreateSamplerState(&smp, &sSampler);
    }

    sState.Capture(ctx);
    ID3D11RenderTargetView* rtv = sInjVelRTV;
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->VSSetShader(sFsVS, nullptr, 0);
    ctx->PSSetShader(sSkyMvPS, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &depthSRV);
    ctx->PSSetSamplers(0, 1, &sSampler);
    ctx->PSSetConstantBuffers(0, 1, &sReprojCB);
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
    if (sSharpenPS) { sSharpenPS->Release(); sSharpenPS = nullptr; }
    if (sSkyMvPS)   { sSkyMvPS->Release();   sSkyMvPS = nullptr; }
    if (sSharpenCB) { sSharpenCB->Release(); sSharpenCB = nullptr; }
    if (sInjVelSRV) { sInjVelSRV->Release(); sInjVelSRV = nullptr; }
    if (sInjVelRTV) { sInjVelRTV->Release(); sInjVelRTV = nullptr; }
    if (sInjVelTex) { sInjVelTex->Release(); sInjVelTex = nullptr; }
    if (sReprojCB)  { sReprojCB->Release();  sReprojCB = nullptr; }
    if (sInjSkinB12){ sInjSkinB12->Release();sInjSkinB12 = nullptr; }
    if (sInjRigidB12) { sInjRigidB12->Release(); sInjRigidB12 = nullptr; }
    if (sRigidZeroB12){ sRigidZeroB12->Release(); sRigidZeroB12 = nullptr; }
    sPrevRigid.clear(); sCurRigid.clear(); sPrevRigidClaimed.clear(); sPrevRigidByMesh.clear();
    sBoneShadowActive = false;
    sKnownBoneCB.clear(); sMapPending.clear(); sBoneShadow.clear();
    sPrevSkinPoses.clear(); sPrevClaimed.clear();
    if (sVelVS) { sVelVS->Release(); sVelVS = nullptr; }
    if (sVelPS) { sVelPS->Release(); sVelPS = nullptr; }
    if (sVelSkinVS) { sVelSkinVS->Release(); sVelSkinVS = nullptr; }
    if (sVelInstVS) { sVelInstVS->Release(); sVelInstVS = nullptr; }
    if (sFsVS)  { sFsVS->Release();  sFsVS = nullptr; }
    if (sFsPS)  { sFsPS->Release();  sFsPS = nullptr; }
    if (sVelCB) { sVelCB->Release(); sVelCB = nullptr; }
    if (sCurBoneCB)  { sCurBoneCB->Release();  sCurBoneCB = nullptr; }
    if (sPrevBoneCB) { sPrevBoneCB->Release(); sPrevBoneCB = nullptr; }
    if (sVpCB)  { sVpCB->Release();  sVpCB = nullptr; }
    if (sDepthState) { sDepthState->Release(); sDepthState = nullptr; }
    if (sDepthOff)   { sDepthOff->Release();   sDepthOff = nullptr; }
    if (sGameDepthSRV) { sGameDepthSRV->Release(); sGameDepthSRV = nullptr; }
    sGameDepthTex = nullptr;
    if (sRaster) { sRaster->Release(); sRaster = nullptr; }
    if (sBlend) { sBlend->Release(); sBlend = nullptr; }
    if (sSampler) { sSampler->Release(); sSampler = nullptr; }
    sFrame = 0;
}

} // namespace MotionVectors
