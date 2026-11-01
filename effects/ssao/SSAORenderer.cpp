#include "../common/LazyResources.h"
#include "SSAORenderer.h"
#include "SSAOConfig.h"
#include "DustLog.h"
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>

// SSAO = XeGTAO (Intel, MIT; shaders/xegtao.hlsli) adapted to Kenshi, as four compute passes:
//   prefilter  Kenshi radial depth01 -> view-space Z, 5-level MIP chain (R32F)
//   main       GTAO slices; smooth normals generated from the corrected depth; normal-map cavity
//   denoise    XeGTAO edge-aware 3x3, ping-pong; the last pass writes the final AO
// The final AO texture is R8_TYPELESS: written through an R8_UINT UAV (XeGTAO's packing), read by
// every consumer (sun pass, light volumes, debug view) through an R8_UNORM SRV - same contract as
// before (white = unoccluded).
//
// Replaced (2026-11) a pixel-shader HBAO-style pass that reconstructed positions with z = radial
// depth (flat surfaces bent by up to ~20 degrees: AO grew toward the screen edges and left a clean
// band down the middle), measured horizons against normal-MAPPED normals (texture-detail haloing),
// and blurred across depth edges (silhouette halos). Offline bench on 6 captures at 2560x1440
// (RTX 4090): 0.31-0.33 ms -> 0.22-0.24 ms.

namespace SSAORenderer
{

static bool gInitialized = false;
static UINT gWidth = 0;
static UINT gHeight = 0;
static const DustHostAPI* gHost = nullptr;
static std::string gShaderDir;
static uint64_t gFrameIndex = 0;

static const UINT kDepthMips = 5;   // XE_GTAO_DEPTH_MIP_LEVELS

// Sized resources
static ID3D11Texture2D*           gDepthMipTex = nullptr;          // view Z, R32F, 5 mips
static ID3D11UnorderedAccessView* gDepthMipUAV[kDepthMips] = {};
static ID3D11ShaderResourceView*  gDepthMipSRV = nullptr;
static ID3D11Texture2D*           gWorkTex[2] = {};                // raw AO term, R8_UINT (ping-pong)
static ID3D11UnorderedAccessView* gWorkUAV[2] = {};
static ID3D11ShaderResourceView*  gWorkSRV[2] = {};
static ID3D11Texture2D*           gEdgesTex = nullptr;             // packed edges, R8_UNORM
static ID3D11UnorderedAccessView* gEdgesUAV = nullptr;
static ID3D11ShaderResourceView*  gEdgesSRV = nullptr;
static ID3D11Texture2D*           gAoTex = nullptr;                // final AO, R8_TYPELESS
static ID3D11UnorderedAccessView* gAoUAV = nullptr;                // R8_UINT
static ID3D11ShaderResourceView*  gAoSRV = nullptr;                // R8_UNORM

// Shaders
static ID3D11ComputeShader* gPrefilterCS = nullptr;
static ID3D11ComputeShader* gMainCS = nullptr;
static ID3D11ComputeShader* gDenoiseCS = nullptr;
static ID3D11ComputeShader* gDenoiseLastCS = nullptr;
static ID3D11VertexShader*  gFullscreenVS = nullptr;   // debug overlay
static ID3D11PixelShader*   gSSAODebugPS = nullptr;

// States
static ID3D11BlendState*        gNoBlend = nullptr;
static ID3D11DepthStencilState* gNoDepthDSS = nullptr;
static ID3D11RasterizerState*   gNoCullRS = nullptr;
static ID3D11SamplerState*      gPointClampSampler = nullptr;

// Mirrors GTAOConstants in shaders/xegtao.h (HLSL packs it identically: float2 pairs, no padding).
struct GTAOConstants
{
    int   ViewportSize[2];
    float ViewportPixelSize[2];
    float DepthUnpackConsts[2];
    float CameraTanHalfFOV[2];
    float NDCToViewMul[2];
    float NDCToViewAdd[2];
    float NDCToViewMul_x_PixelSize[2];
    float EffectRadius;
    float EffectFalloffRange;
    float RadiusMultiplier;
    float Padding0;
    float FinalValuePower;
    float DenoiseBlurBeta;
    float SampleDistributionPower;
    float ThinOccluderCompensation;
    float DepthMIPSamplingOffset;
    int   NoiseIndex;
};
static_assert(sizeof(GTAOConstants) == 96, "GTAOConstants layout");

// Mirrors cbuffer DustAOParams (b1) in shaders/ssao_xegtao_cs.hlsl.
struct DustAOParams
{
    float camRight[4];
    float camUp[4];
    float camForward[4];
    float normalDetail;
    float maxRadial;
    float fadeStart;
    float maxScreenRadiusPx;
    float sliceCount;
    float stepsPerSlice;
    float pad[2];
};
static_assert(sizeof(DustAOParams) == 80, "DustAOParams layout");

static ID3D11Buffer* gGtaoCB = nullptr;
static ID3D11Buffer* gDustCB = nullptr;

// Kenshi world units per metre (a ~1.8 m character is ~20 units tall in the captures).
static const float kUnitsPerMetre = 10.0f;
// XeGTAO tuning that is not exposed (its defaults, see xegtao.h).
static const float kRadiusMultiplier = 1.457f;
static const float kFalloffRange = 0.615f;
static const float kSampleDistributionPower = 2.0f;
static const float kDepthMipSamplingOffset = 3.30f;
static const float kDenoiseBeta = 1.2f;
static const int   kDenoisePasses = 2;   // no TAA: XeGTAO recommends 2+

// ==================== Helpers ====================

#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }

static ID3D11Texture2D* CreateTex(ID3D11Device* device, DXGI_FORMAT fmt, UINT mips, UINT bind)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = gWidth; d.Height = gHeight; d.MipLevels = mips; d.ArraySize = 1; d.Format = fmt;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    ID3D11Texture2D* t = nullptr;
    HRESULT hr = device->CreateTexture2D(&d, nullptr, &t);
    if (FAILED(hr)) { Log("SSAO: CreateTexture2D(fmt %d, %ux%u) failed: 0x%08X", (int)fmt, gWidth, gHeight, hr); return nullptr; }
    return t;
}

static ID3D11UnorderedAccessView* CreateUAV(ID3D11Device* device, ID3D11Texture2D* t, DXGI_FORMAT fmt, UINT mip = 0)
{
    D3D11_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = fmt; d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; d.Texture2D.MipSlice = mip;
    ID3D11UnorderedAccessView* v = nullptr;
    if (FAILED(device->CreateUnorderedAccessView(t, &d, &v))) { Log("SSAO: CreateUAV(fmt %d) failed", (int)fmt); return nullptr; }
    return v;
}

static ID3D11ShaderResourceView* CreateSRV(ID3D11Device* device, ID3D11Texture2D* t, DXGI_FORMAT fmt, UINT mips)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = fmt; d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MostDetailedMip = 0; d.Texture2D.MipLevels = mips;
    ID3D11ShaderResourceView* v = nullptr;
    if (FAILED(device->CreateShaderResourceView(t, &d, &v))) { Log("SSAO: CreateSRV(fmt %d) failed", (int)fmt); return nullptr; }
    return v;
}

static ID3D11ComputeShader* CreateCS(ID3D11Device* device, const char* entry)
{
    ID3DBlob* blob = gHost->CompileShaderFromFile((gShaderDir + "ssao_xegtao_cs.hlsl").c_str(), entry, "cs_5_0");
    if (!blob) { Log("SSAO: failed to compile %s", entry); return nullptr; }
    ID3D11ComputeShader* cs = nullptr;
    HRESULT hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
    blob->Release();
    if (FAILED(hr)) { Log("SSAO: CreateComputeShader(%s) failed: 0x%08X", entry, hr); return nullptr; }
    return cs;
}

// ==================== Sized resources ====================

static LazyResources gTargets;
static void ReleaseSizedTextures()
{
    for (UINT i = 0; i < kDepthMips; i++) SAFE_RELEASE(gDepthMipUAV[i]);
    SAFE_RELEASE(gDepthMipSRV); SAFE_RELEASE(gDepthMipTex);
    for (int i = 0; i < 2; i++) { SAFE_RELEASE(gWorkUAV[i]); SAFE_RELEASE(gWorkSRV[i]); SAFE_RELEASE(gWorkTex[i]); }
    SAFE_RELEASE(gEdgesUAV); SAFE_RELEASE(gEdgesSRV); SAFE_RELEASE(gEdgesTex);
    SAFE_RELEASE(gAoUAV); SAFE_RELEASE(gAoSRV); SAFE_RELEASE(gAoTex);
}

static bool CreateSizedTextures(ID3D11Device* device)
{
    const UINT rw = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    gDepthMipTex = CreateTex(device, DXGI_FORMAT_R32_FLOAT, kDepthMips, rw);
    if (!gDepthMipTex) return false;
    for (UINT i = 0; i < kDepthMips; i++)
        if (!(gDepthMipUAV[i] = CreateUAV(device, gDepthMipTex, DXGI_FORMAT_R32_FLOAT, i))) return false;
    if (!(gDepthMipSRV = CreateSRV(device, gDepthMipTex, DXGI_FORMAT_R32_FLOAT, kDepthMips))) return false;
    for (int i = 0; i < 2; i++)
    {
        if (!(gWorkTex[i] = CreateTex(device, DXGI_FORMAT_R8_UINT, 1, rw))) return false;
        if (!(gWorkUAV[i] = CreateUAV(device, gWorkTex[i], DXGI_FORMAT_R8_UINT))) return false;
        if (!(gWorkSRV[i] = CreateSRV(device, gWorkTex[i], DXGI_FORMAT_R8_UINT, 1))) return false;
    }
    if (!(gEdgesTex = CreateTex(device, DXGI_FORMAT_R8_UNORM, 1, rw))) return false;
    if (!(gEdgesUAV = CreateUAV(device, gEdgesTex, DXGI_FORMAT_R8_UNORM))) return false;
    if (!(gEdgesSRV = CreateSRV(device, gEdgesTex, DXGI_FORMAT_R8_UNORM, 1))) return false;
    if (!(gAoTex = CreateTex(device, DXGI_FORMAT_R8_TYPELESS, 1, rw))) return false;
    if (!(gAoUAV = CreateUAV(device, gAoTex, DXGI_FORMAT_R8_UINT))) return false;
    if (!(gAoSRV = CreateSRV(device, gAoTex, DXGI_FORMAT_R8_UNORM, 1))) return false;
    return true;
}

static bool EnsureSizedTextures(ID3D11DeviceContext* ctx)
{
    if (!gWidth || !gHeight) return false;
    return gTargets.Ensure(GetTickCount64(), [&] {
        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        if (!device) return false;
        const bool ok = CreateSizedTextures(device);
        device->Release();
        return ok;
    }, ReleaseSizedTextures);
}

// ==================== Public API ====================

bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir)
{
    if (gInitialized)
        return true;

    gHost = host;
    gShaderDir = std::string(effectDir) + "\\shaders\\";
    Log("Initializing SSAORenderer (XeGTAO, %ux%u), shaders: %s", width, height, gShaderDir.c_str());
    gWidth = width;
    gHeight = height;

    if (!(gPrefilterCS = CreateCS(device, "CSPrefilterDepths16x16")) || !(gMainCS = CreateCS(device, "CSMain")) ||
        !(gDenoiseCS = CreateCS(device, "CSDenoise")) || !(gDenoiseLastCS = CreateCS(device, "CSDenoiseLast")))
        return false;

    HRESULT hr;
    ID3DBlob* vsBlob = host->CompileShaderFromFile((gShaderDir + "fullscreen_vs.hlsl").c_str(), "main", "vs_5_0");
    if (!vsBlob) { Log("SSAO: failed to compile fullscreen_vs.hlsl"); return false; }
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &gFullscreenVS);
    vsBlob->Release();
    if (FAILED(hr)) { Log("Failed to create VS: 0x%08X", hr); return false; }

    ID3DBlob* debugBlob = host->CompileShaderFromFile((gShaderDir + "ssao_debug_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!debugBlob) { Log("SSAO: failed to compile ssao_debug_ps.hlsl"); return false; }
    hr = device->CreatePixelShader(debugBlob->GetBufferPointer(), debugBlob->GetBufferSize(), nullptr, &gSSAODebugPS);
    debugBlob->Release();
    if (FAILED(hr)) { Log("Failed to create debug PS: 0x%08X", hr); return false; }

    {
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].BlendEnable = FALSE;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&desc, &gNoBlend))) { Log("SSAO: CreateBlendState failed"); return false; }
    }
    {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = FALSE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(device->CreateDepthStencilState(&desc, &gNoDepthDSS))) { Log("SSAO: CreateDepthStencilState failed"); return false; }
    }
    {
        D3D11_RASTERIZER_DESC desc = {};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        desc.DepthClipEnable = FALSE;
        if (FAILED(device->CreateRasterizerState(&desc, &gNoCullRS))) { Log("SSAO: CreateRasterizerState failed"); return false; }
    }
    {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MaxLOD = D3D11_FLOAT32_MAX;   // XeGTAO samples the depth MIP chain
        if (FAILED(device->CreateSamplerState(&desc, &gPointClampSampler))) { Log("SSAO: CreateSamplerState failed"); return false; }
    }

    gGtaoCB = host->CreateConstantBuffer(device, sizeof(GTAOConstants));
    gDustCB = host->CreateConstantBuffer(device, sizeof(DustAOParams));
    if (!gGtaoCB || !gDustCB) { Log("SSAO: constant buffer creation failed"); return false; }

    gInitialized = true;
    Log("SSAORenderer initialized successfully");
    return true;
}

void Shutdown()
{
    gTargets.Reset(ReleaseSizedTextures);
    SAFE_RELEASE(gPrefilterCS); SAFE_RELEASE(gMainCS); SAFE_RELEASE(gDenoiseCS); SAFE_RELEASE(gDenoiseLastCS);
    SAFE_RELEASE(gFullscreenVS); SAFE_RELEASE(gSSAODebugPS);
    SAFE_RELEASE(gNoBlend); SAFE_RELEASE(gNoDepthDSS); SAFE_RELEASE(gNoCullRS);
    SAFE_RELEASE(gPointClampSampler); SAFE_RELEASE(gGtaoCB); SAFE_RELEASE(gDustCB);
    gInitialized = false;
    gFrameIndex = 0;
    gHost = nullptr;
    Log("SSAORenderer shut down");
}

void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight)
{
    if (!newWidth || !newHeight || (newWidth == gWidth && newHeight == gHeight)) return;
    gTargets.Reset(ReleaseSizedTextures);
    gWidth = newWidth;
    gHeight = newHeight;
}

bool IsInitialized()
{
    return gInitialized;
}

ID3D11ShaderResourceView* GetAoSRV()
{
    return gAoSRV;
}

float GetLastGpuTimeMs()
{
    return 0.0f;  // Framework handles timing via DUST_FLAG_FRAMEWORK_TIMING
}

ID3D11ShaderResourceView* RenderAO(ID3D11DeviceContext* ctx,
                                    ID3D11ShaderResourceView* depthSRV,
                                    ID3D11ShaderResourceView* normalsSRV,
                                    const DustCameraData* camera)
{
    if (!gInitialized || !ctx || !depthSRV || !gHost)
        return nullptr;

    // API v8: the real camera FOV and far clip (the G-buffer depth is distance / farClip).
    const bool haveCamera = camera && camera->valid && gHost->apiVersion >= 8;
    if (haveCamera && camera->tanHalfFov > 0.0f)
        gSSAOConfig.tanHalfFov = camera->tanHalfFov;
    const float farClip = (haveCamera && camera->farZ > 1.0f) ? camera->farZ : 120000.0f;

    // Defensive resolution fallback — every 300 frames (~5s) to catch missed OnResolutionChanged.
    if ((gFrameIndex % 300) == 0)
    {
        ID3D11Resource* res = nullptr;
        depthSRV->GetResource(&res);
        if (res)
        {
            ID3D11Texture2D* tex = nullptr;
            res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
            if (tex)
            {
                D3D11_TEXTURE2D_DESC desc;
                tex->GetDesc(&desc);
                if (desc.Width != gWidth || desc.Height != gHeight)
                {
                    ID3D11Device* device = nullptr;
                    ctx->GetDevice(&device);
                    if (device) { OnResolutionChanged(device, desc.Width, desc.Height); device->Release(); }
                }
                tex->Release();
            }
            res->Release();
        }
    }
    gFrameIndex++;

    if (!EnsureSizedTextures(ctx)) return nullptr;

    // Constants
    {
        const float tanY = gSSAOConfig.tanHalfFov;
        const float tanX = tanY * (float)gWidth / (float)gHeight;
        GTAOConstants g = {};
        g.ViewportSize[0] = (int)gWidth; g.ViewportSize[1] = (int)gHeight;
        g.ViewportPixelSize[0] = 1.0f / gWidth; g.ViewportPixelSize[1] = 1.0f / gHeight;
        g.CameraTanHalfFOV[0] = tanX; g.CameraTanHalfFOV[1] = tanY;
        g.NDCToViewMul[0] = tanX * 2.0f; g.NDCToViewMul[1] = tanY * -2.0f;
        g.NDCToViewAdd[0] = -tanX;       g.NDCToViewAdd[1] = tanY;
        g.NDCToViewMul_x_PixelSize[0] = g.NDCToViewMul[0] / gWidth;
        g.NDCToViewMul_x_PixelSize[1] = g.NDCToViewMul[1] / gHeight;
        // View Z is in depth01 units (distance / farClip): convert the world radius the same way.
        g.EffectRadius = (std::max)(gSSAOConfig.radiusMetres, 0.01f) * kUnitsPerMetre / farClip;
        g.EffectFalloffRange = kFalloffRange;
        g.RadiusMultiplier = kRadiusMultiplier;
        g.FinalValuePower = (std::max)(gSSAOConfig.aoStrength, 0.05f) * 1.25f;
        g.DenoiseBlurBeta = kDenoiseBeta;
        g.SampleDistributionPower = kSampleDistributionPower;
        g.ThinOccluderCompensation = (std::min)((std::max)(gSSAOConfig.thickness, 0.0f), 1.0f);
        g.DepthMIPSamplingOffset = kDepthMipSamplingOffset;
        g.NoiseIndex = 0;   // no temporal accumulation of AO
        gHost->UpdateConstantBuffer(ctx, gGtaoCB, &g, sizeof(g));

        DustAOParams d = {};
        if (camera && camera->valid)
        {
            const float* iv = camera->inverseView;
            d.camRight[0] = iv[0];   d.camRight[1] = iv[4];   d.camRight[2] = iv[8];
            d.camUp[0] = iv[1];      d.camUp[1] = iv[5];      d.camUp[2] = iv[9];
            d.camForward[0] = iv[2]; d.camForward[1] = iv[6]; d.camForward[2] = iv[10];
        }
        d.normalDetail = (std::min)((std::max)(gSSAOConfig.normalDetail, 0.0f), 1.0f);
        d.maxRadial = gSSAOConfig.aoMaxDepth;
        d.fadeStart = (std::min)(gSSAOConfig.depthFadeStart, gSSAOConfig.aoMaxDepth);
        d.maxScreenRadiusPx = 1e6f;   // no clamp: the radius is physical now
        d.sliceCount = (float)(std::min)((std::max)(gSSAOConfig.sampleCount, 1), 9);
        d.stepsPerSlice = (float)(std::min)((std::max)(gSSAOConfig.stepCount, 2), 6);
        gHost->UpdateConstantBuffer(ctx, gDustCB, &d, sizeof(d));
    }

    // Compute state is not covered by the host's Save/RestoreState: bind, dispatch, unbind
    // every resource (the game never uses CS; RTGI rebinds its own).
    ID3D11Buffer* cbs[2] = { gGtaoCB, gDustCB };
    ctx->CSSetConstantBuffers(0, 2, cbs);
    ctx->CSSetSamplers(0, 1, &gPointClampSampler);
    ID3D11ShaderResourceView* nullSRVs[3] = {};
    ID3D11UnorderedAccessView* nullUAVs[kDepthMips] = {};

    // 1. Depth prefilter
    ctx->CSSetShader(gPrefilterCS, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &depthSRV);
    ctx->CSSetUnorderedAccessViews(0, kDepthMips, gDepthMipUAV, nullptr);
    ctx->Dispatch((gWidth + 15) / 16, (gHeight + 15) / 16, 1);
    ctx->CSSetUnorderedAccessViews(0, kDepthMips, nullUAVs, nullptr);

    // 2. GTAO
    ctx->CSSetShader(gMainCS, nullptr, 0);
    ID3D11ShaderResourceView* mainSRVs[3] = { gDepthMipSRV, normalsSRV, depthSRV };
    ctx->CSSetShaderResources(0, 3, mainSRVs);
    ID3D11UnorderedAccessView* mainUAVs[2] = { gWorkUAV[0], gEdgesUAV };
    ctx->CSSetUnorderedAccessViews(0, 2, mainUAVs, nullptr);
    ctx->Dispatch((gWidth + 7) / 8, (gHeight + 7) / 8, 1);
    ctx->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
    ctx->CSSetShaderResources(0, 3, nullSRVs);

    // 3. Denoise (ping-pong; the last pass writes the final AO)
    int src = 0;
    for (int p = 0; p < kDenoisePasses; p++)
    {
        const bool last = (p == kDenoisePasses - 1);
        ctx->CSSetShader(last ? gDenoiseLastCS : gDenoiseCS, nullptr, 0);
        ID3D11ShaderResourceView* dnSRVs[2] = { gWorkSRV[src], gEdgesSRV };
        ctx->CSSetShaderResources(0, 2, dnSRVs);
        ID3D11UnorderedAccessView* out = last ? gAoUAV : gWorkUAV[1 - src];
        ctx->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
        ctx->Dispatch((gWidth + 15) / 16, (gHeight + 7) / 8, 1);
        ctx->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
        ctx->CSSetShaderResources(0, 2, nullSRVs);
        src = 1 - src;
    }

    return gAoSRV;
}

void RenderDebugOverlay(ID3D11DeviceContext* ctx,
                        ID3D11RenderTargetView* hdrRTV)
{
    if (!gInitialized || !ctx || !hdrRTV || !gSSAOConfig.debugView || !gHost)
        return;

    if (!gTargets.Ready()) return;

    gHost->SaveState(ctx);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(gFullscreenVS, nullptr, 0);
    ctx->RSSetState(gNoCullRS);
    ctx->OMSetDepthStencilState(gNoDepthDSS, 0);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)gWidth;
    vp.Height = (float)gHeight;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetRenderTargets(1, &hdrRTV, nullptr);
    ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
    ctx->PSSetShader(gSSAODebugPS, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &gAoSRV);
    ctx->PSSetSamplers(0, 1, &gPointClampSampler);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);

    gHost->RestoreState(ctx);
}

} // namespace SSAORenderer
