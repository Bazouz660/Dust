// DustRTGI.cpp - Screen-Space Ray-Traced Global Illumination effect plugin for Dust (API v3)
// Traces rays through the depth buffer, samples lit scene radiance at hit points for
// indirect bounce lighting, and derives ambient occlusion from ray miss/hit ratio.
// Uses temporal accumulation and a-trous wavelet denoising for stable output.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "RTGIRenderer.h"
#include "RTGIConfig.h"

#include <d3d11.h>
#include <cstring>
#include <string>

static const DustHostAPI* gHost = nullptr;

DustLogFn gLogFn = nullptr;

RTGIConfig gRTGIConfig;

static HMODULE gPluginModule = nullptr;
static ID3D11Device* gDevice = nullptr;

// Composite resources
static ID3D11PixelShader*  gCompositePS = nullptr;
static ID3D11PixelShader*  gAOCompositePS = nullptr;
static ID3D11BlendState*   gAdditiveBlend = nullptr;
static ID3D11BlendState*   gMultiplyBlend = nullptr;
static ID3D11SamplerState* gLinearSampler = nullptr;
static ID3D11SamplerState* gPointSampler = nullptr;
static ID3D11Buffer*       gCompositeCB = nullptr;

struct CompositeCBData
{
    float viewportSize[2];
    float invViewportSize[2];
    float giIntensity;
    float saturation;
    float giTexSize[2];
};

// Track the last GI SRV for compositing
static ID3D11ShaderResourceView* gLastGISRV = nullptr;

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

// PRE callback: extract camera data from the game's bound constant buffer
static void RTGIPreExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gRTGIConfig.enabled || !RTGIRenderer::IsInitialized())
        return;

    RTGIRenderer::ExtractCameraData(ctx->context);
}

// POST callback: render GI and composite onto HDR scene
static void RTGIPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gRTGIConfig.enabled || !RTGIRenderer::IsInitialized())
        return;

    ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
    if (!depthSRV)
        return;

    ID3D11ShaderResourceView* normalsSRV = host->GetSRV(DUST_RESOURCE_NORMALS);

    // Lit HDR scene — Reinhard normalization preserves hue at hit points
    ID3D11ShaderResourceView* sceneSRV = host->GetSceneCopy(ctx->context, DUST_RESOURCE_HDR_RT);
    if (!sceneSRV)
        return;

    // Render the GI (scene color with Reinhard normalization for colored bounce lighting)
    ID3D11ShaderResourceView* giSRV = RTGIRenderer::RenderGI(ctx->context, depthSRV, sceneSRV, normalsSRV);
    if (!giSRV)
        return;

    gLastGISRV = giSRV;

    // Composite onto HDR scene
    ID3D11RenderTargetView* hdrRTV = host->GetRTV(DUST_RESOURCE_HDR_RT);
    if (!hdrRTV)
        return;

    ID3D11DeviceContext* dc = ctx->context;
    host->SaveState(dc);

    D3D11_VIEWPORT vp = { 0, 0, (float)ctx->width, (float)ctx->height, 0, 1 };
    dc->RSSetViewports(1, &vp);

    // Composite cbuffer (shared by both passes)
    UINT giW, giH;
    RTGIRenderer::GetRenderSize(&giW, &giH);

    CompositeCBData ccb = {};
    ccb.viewportSize[0] = (float)ctx->width;
    ccb.viewportSize[1] = (float)ctx->height;
    ccb.invViewportSize[0] = 1.0f / (float)ctx->width;
    ccb.invViewportSize[1] = 1.0f / (float)ctx->height;
    ccb.giIntensity = gRTGIConfig.giIntensity;
    ccb.saturation = gRTGIConfig.saturation;
    ccb.giTexSize[0] = (float)giW;
    ccb.giTexSize[1] = (float)giH;
    host->UpdateConstantBuffer(dc, gCompositeCB, &ccb, sizeof(ccb));

    ID3D11SamplerState* samplers[2] = { gLinearSampler, gPointSampler };

    // Step 1: Add indirect light (additive blend) with intensity + saturation applied in shader
    if (gRTGIConfig.giIntensity > 0.0f)
    {
        dc->OMSetRenderTargets(1, &hdrRTV, nullptr);
        float bf[4] = { 0, 0, 0, 0 };
        dc->OMSetBlendState(gAdditiveBlend, bf, 0xFFFFFFFF);
        ID3D11ShaderResourceView* srvs[2] = { giSRV, depthSRV };
        dc->PSSetShaderResources(0, 2, srvs);
        dc->PSSetSamplers(0, 2, samplers);
        dc->PSSetConstantBuffers(0, 1, &gCompositeCB);
        host->DrawFullscreenTriangle(dc, gCompositePS);
        ID3D11ShaderResourceView* nullSRVs[2] = {};
        dc->PSSetShaderResources(0, 2, nullSRVs);
    }

    // Step 2: Apply AO (multiply blend)
    if (gRTGIConfig.aoIntensity > 0.0f)
    {
        dc->OMSetRenderTargets(1, &hdrRTV, nullptr);
        float bf[4] = { 0, 0, 0, 0 };
        dc->OMSetBlendState(gMultiplyBlend, bf, 0xFFFFFFFF);
        ID3D11ShaderResourceView* srvs[2] = { giSRV, depthSRV };
        dc->PSSetShaderResources(0, 2, srvs);
        dc->PSSetSamplers(0, 2, samplers);
        dc->PSSetConstantBuffers(0, 1, &gCompositeCB);
        host->DrawFullscreenTriangle(dc, gAOCompositePS);
        ID3D11ShaderResourceView* nullSRVs[2] = {};
        dc->PSSetShaderResources(0, 2, nullSRVs);
    }

    host->RestoreState(dc);

    // Debug overlay (replaces scene if active)
    if (gRTGIConfig.debugView != 0)
    {
        RTGIRenderer::RenderDebugOverlay(dc, hdrRTV);
    }
}

static int RTGIInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
    gHost = host;
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gDevice = device;

    std::string pluginDir = GetPluginDir();
    std::string shaderDir = pluginDir + "\\shaders\\";

    if (!RTGIRenderer::Init(device, width, height, host, pluginDir.c_str()))
        return -1;

    // Compile composite shaders
    ID3DBlob* compBlob = host->CompileShaderFromFile((shaderDir + "rtgi_composite_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!compBlob) return -2;
    HRESULT hr = device->CreatePixelShader(compBlob->GetBufferPointer(), compBlob->GetBufferSize(), nullptr, &gCompositePS);
    compBlob->Release();
    if (FAILED(hr)) return -3;

    ID3DBlob* aoBlob = host->CompileShaderFromFile((shaderDir + "rtgi_ao_composite_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!aoBlob) return -4;
    hr = device->CreatePixelShader(aoBlob->GetBufferPointer(), aoBlob->GetBufferSize(), nullptr, &gAOCompositePS);
    aoBlob->Release();
    if (FAILED(hr)) return -5;

    // Additive blend
    {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&bd, &gAdditiveBlend);
        if (FAILED(hr)) return -6;
    }

    // Multiply blend (dest = dest * src)
    {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&bd, &gMultiplyBlend);
        if (FAILED(hr)) return -7;
    }

    // Linear sampler
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&sd, &gLinearSampler);
        if (FAILED(hr)) return -8;
    }

    // Point clamp sampler (for bilateral upscale)
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&sd, &gPointSampler);
        if (FAILED(hr)) return -8;
    }

    // Composite constant buffer
    gCompositeCB = host->CreateConstantBuffer(device, sizeof(CompositeCBData));
    if (!gCompositeCB) return -9;

    Log("RTGI: Initialized (%ux%u)", width, height);
    return 0;
}

static void RTGIShutdown()
{
    RTGIRenderer::Shutdown();

#define SR(p) if (p) { (p)->Release(); (p) = nullptr; }
    SR(gCompositePS); SR(gAOCompositePS);
    SR(gAdditiveBlend); SR(gMultiplyBlend); SR(gLinearSampler); SR(gPointSampler);
    SR(gCompositeCB);
#undef SR

    gDevice = nullptr;
    gLastGISRV = nullptr;
    Log("RTGI: Shut down");
}

static void RTGIOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    RTGIRenderer::OnResolutionChanged(device, w, h);
    Log("RTGI: Resolution changed to %ux%u", w, h);
}

static int RTGIIsEnabled()
{
    return 1; // Always dispatch so we can extract camera data in preExecute
}

// ==================== Settings ====================

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",            DUST_SETTING_BOOL,  &gRTGIConfig.enabled,         0.0f,   1.0f,   "Enabled" },
    { "GI Intensity",       DUST_SETTING_FLOAT, &gRTGIConfig.giIntensity,     0.0f,   5.0f,   "GIIntensity" },
    { "AO Intensity",       DUST_SETTING_FLOAT, &gRTGIConfig.aoIntensity,     0.0f,   2.0f,   "AOIntensity" },
    { "Ray Length",         DUST_SETTING_FLOAT, &gRTGIConfig.rayLength,       0.05f,  1.0f,   "RayLength" },
    { "Ray Steps",          DUST_SETTING_INT,   &gRTGIConfig.raySteps,        8.0f,   64.0f,  "RaySteps" },
    { "Rays Per Pixel",     DUST_SETTING_INT,   &gRTGIConfig.raysPerPixel,    1.0f,   16.0f,  "RaysPerPixel" },
    { "Thickness",          DUST_SETTING_FLOAT, &gRTGIConfig.thickness,       0.005f, 0.5f,   "Thickness" },
    { "Thickness Curve",    DUST_SETTING_FLOAT, &gRTGIConfig.thicknessCurve,  0.3f,   1.5f,   "ThicknessCurve" },
    { "Fade Distance",      DUST_SETTING_FLOAT, &gRTGIConfig.fadeDistance,     0.01f,  1.0f,   "FadeDistance" },
    { "Bounce Intensity",   DUST_SETTING_FLOAT, &gRTGIConfig.bounceIntensity, 0.0f,   2.0f,   "BounceIntensity" },
    { "Saturation",         DUST_SETTING_FLOAT, &gRTGIConfig.saturation,      0.0f,   2.0f,   "Saturation" },
    { "Temporal Blend",     DUST_SETTING_FLOAT, &gRTGIConfig.temporalBlend,   0.5f,   0.99f,  "TemporalBlend" },
    { "Denoise Steps",      DUST_SETTING_INT,   &gRTGIConfig.denoiseSteps,    0.0f,   5.0f,   "DenoiseSteps" },
    { "Depth Sigma",        DUST_SETTING_FLOAT, &gRTGIConfig.depthSigma,      0.1f,   5.0f,   "DepthSigma" },
    { "Color Phi",          DUST_SETTING_FLOAT, &gRTGIConfig.phiColor,        1.0f,   10.0f,  "PhiColor" },
    { "Half Resolution",    DUST_SETTING_BOOL,  &gRTGIConfig.halfResolution,  0.0f,   1.0f,   "HalfResolution" },
    { "Debug View",         DUST_SETTING_INT,   &gRTGIConfig.debugView,       0.0f,   3.0f,   "DebugView" },
    // Hidden
    { "Tan Half FOV",       DUST_SETTING_HIDDEN_FLOAT, &gRTGIConfig.tanHalfFov, 0.1f, 2.0f,   "TanHalfFov" },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "RTGI";
    desc->injectionPoint    = DUST_INJECT_POST_LIGHTING;
    desc->Init              = RTGIInit;
    desc->Shutdown          = RTGIShutdown;
    desc->OnResolutionChanged = RTGIOnResolutionChanged;
    desc->preExecute        = RTGIPreExecute;
    desc->postExecute       = RTGIPostExecute;
    desc->IsEnabled         = RTGIIsEnabled;
    desc->settings          = gSettingsArray;
    desc->settingCount      = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->OnSettingChanged  = nullptr;

    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "RTGI";
    desc->priority          = 20; // After SSAO (0) and SSIL (10)

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        gPluginModule = hModule;
    }
    return TRUE;
}
