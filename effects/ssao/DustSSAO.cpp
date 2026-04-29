// DustSSAO.cpp - SSAO effect plugin for Dust (API v3)
// Exports DustEffectCreate per the Dust plugin API.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "SSAORenderer.h"
#include "SSAOConfig.h"

#include <d3d11.h>
#include <cstring>
#include <string>

static const DustHostAPI* gHost = nullptr;

// Log function pointer used by DustLog.h in other translation units
DustLogFn gLogFn = nullptr;

// Global config instance (framework populates via settings array)
SSAOConfig gSSAOConfig;

// Sampler + white fallback for binding AO to slot 8
static ID3D11SamplerState* gAoSampler = nullptr;
static ID3D11Texture2D* gWhiteTex = nullptr;
static ID3D11ShaderResourceView* gWhiteSRV = nullptr;

// 1x1 dynamic texture for passing directLightOcclusion to deferred.hlsl (slot 9)
static ID3D11Texture2D* gParamsTex = nullptr;
static ID3D11ShaderResourceView* gParamsSRV = nullptr;

static bool CreateAoSampler(ID3D11Device* device)
{
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr = device->CreateSamplerState(&sd, &gAoSampler);
    if (FAILED(hr))
    {
        Log("SSAO: Failed to create AO sampler: 0x%08X", hr);
        return false;
    }
    return true;
}

static bool CreateWhiteFallback(ID3D11Device* device)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    uint8_t white = 255;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = &white;
    init.SysMemPitch = 1;

    HRESULT hr = device->CreateTexture2D(&desc, &init, &gWhiteTex);
    if (FAILED(hr))
    {
        Log("SSAO: Failed to create white texture: 0x%08X", hr);
        return false;
    }

    hr = device->CreateShaderResourceView(gWhiteTex, nullptr, &gWhiteSRV);
    if (FAILED(hr))
    {
        Log("SSAO: Failed to create white SRV: 0x%08X", hr);
        gWhiteTex->Release();
        gWhiteTex = nullptr;
        return false;
    }
    return true;
}

static bool CreateParamsTexture(ID3D11Device* device)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &gParamsTex);
    if (FAILED(hr))
    {
        Log("SSAO: Failed to create params texture: 0x%08X", hr);
        return false;
    }

    hr = device->CreateShaderResourceView(gParamsTex, nullptr, &gParamsSRV);
    if (FAILED(hr))
    {
        Log("SSAO: Failed to create params SRV: 0x%08X", hr);
        gParamsTex->Release();
        gParamsTex = nullptr;
        return false;
    }
    return true;
}

static void UpdateParamsTexture(ID3D11DeviceContext* ctx)
{
    if (!gParamsTex) return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx->Map(gParamsTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        uint8_t val = (uint8_t)(gSSAOConfig.directLightOcclusion * 255.0f + 0.5f);
        *(uint8_t*)mapped.pData = val;
        ctx->Unmap(gParamsTex, 0);
    }
}

// The register declared in deferred.hlsl for aoMap
static const uint32_t AO_REGISTER = 8;
static const uint32_t AO_PARAMS_REGISTER = 9;

// Called BEFORE the game's lighting draw
static void SSAOPreExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    static bool sFirstRun = true;
    if (sFirstRun)
    {
        Log("[SSAO] First preExecute: res=%ux%u, frame=%llu",
            ctx->width, ctx->height, ctx->frameIndex);
        sFirstRun = false;
    }

    ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
    if (!depthSRV)
    {
        static bool sWarnedNoDepth = false;
        if (!sWarnedNoDepth) { Log("[SSAO] WARNING: No depth SRV available"); sWarnedNoDepth = true; }
        return;
    }

    if (!SSAORenderer::IsInitialized())
    {
        static bool sWarnedNoInit = false;
        if (!sWarnedNoInit) { Log("[SSAO] WARNING: Renderer not initialized, binding white fallback"); sWarnedNoInit = true; }
        host->BindSRV(ctx->context, AO_REGISTER, gWhiteSRV, gAoSampler);
        host->BindSRV(ctx->context, AO_PARAMS_REGISTER, gParamsSRV, gAoSampler);
        return;
    }

    // Update and bind the direct-light occlusion parameter (slot 9)
    UpdateParamsTexture(ctx->context);
    host->BindSRV(ctx->context, AO_PARAMS_REGISTER, gParamsSRV, gAoSampler);

    if (!gSSAOConfig.enabled)
    {
        // Bind white (no occlusion) so deferred.hlsl still reads a valid texture
        host->BindSRV(ctx->context, AO_REGISTER, gWhiteSRV, gAoSampler);
        return;
    }

    // Generate AO (saves/restores GPU state internally)
    ID3D11ShaderResourceView* normalsSRV = host->GetSRV(DUST_RESOURCE_NORMALS);
    ID3D11ShaderResourceView* aoSRV = SSAORenderer::RenderAO(ctx->context, depthSRV, normalsSRV, &ctx->camera);
    if (!aoSRV)
    {
        Log("[SSAO] WARNING: RenderAO returned null, binding white fallback");
        aoSRV = gWhiteSRV;
    }

    // Bind AO for deferred.hlsl to sample
    host->BindSRV(ctx->context, AO_REGISTER, aoSRV, gAoSampler);
}

// Called AFTER the game's lighting draw
static void SSAOPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    // Unbind AO and params slots
    host->UnbindSRV(ctx->context, AO_REGISTER);
    host->UnbindSRV(ctx->context, AO_PARAMS_REGISTER);

    // Debug overlay
    if (gSSAOConfig.debugView && gSSAOConfig.enabled)
    {
        ID3D11RenderTargetView* hdrRTV = host->GetRTV(DUST_RESOURCE_HDR_RT);
        if (hdrRTV)
            SSAORenderer::RenderDebugOverlay(ctx->context, hdrRTV);
    }
}

static HMODULE gPluginModule = nullptr;

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

static int SSAOInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
    gHost = host;
#undef Log
    gLogFn = host->Log;
#define Log DustLog

    if (!CreateAoSampler(device))
        return -1;

    if (!CreateWhiteFallback(device))
        return -2;

    if (!CreateParamsTexture(device))
        return -3;

    std::string pluginDir = GetPluginDir();
    if (!SSAORenderer::Init(device, width, height, host, pluginDir.c_str()))
        return -3;

    Log("SSAO: Initialized (%ux%u)", width, height);
    return 0;
}

static void SSAOShutdown()
{
    SSAORenderer::Shutdown();

    if (gAoSampler)  { gAoSampler->Release();  gAoSampler = nullptr; }
    if (gWhiteSRV)   { gWhiteSRV->Release();  gWhiteSRV = nullptr; }
    if (gWhiteTex)   { gWhiteTex->Release();   gWhiteTex = nullptr; }
    if (gParamsSRV)  { gParamsSRV->Release();  gParamsSRV = nullptr; }
    if (gParamsTex)  { gParamsTex->Release();  gParamsTex = nullptr; }

    Log("SSAO: Shut down");
}

static void SSAOOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    SSAORenderer::OnResolutionChanged(device, w, h);
    Log("SSAO: Resolution changed to %ux%u", w, h);
}

static int SSAOIsEnabled()
{
    // Always return 1 so the framework always calls preExecute/postExecute.
    // When disabled, preExecute binds a white fallback so the shader reads valid data.
    return 1;
}

// ==================== GUI Settings ====================

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",            DUST_SETTING_BOOL,  &gSSAOConfig.enabled,             0.0f,    1.0f,   "Enabled",        nullptr, "Enable or disable ambient occlusion",                          DUST_PERF_MEDIUM },
    { "Radius",             DUST_SETTING_FLOAT, &gSSAOConfig.aoRadius,            0.0005f, 0.01f,  "Radius",         nullptr, "World-space sampling radius for occlusion testing",            DUST_PERF_NONE },
    { "Strength",           DUST_SETTING_FLOAT, &gSSAOConfig.aoStrength,          0.5f,    10.0f,  "Strength",       nullptr, "Intensity of the occlusion darkening",                         DUST_PERF_NONE },
    { "Bias",               DUST_SETTING_FLOAT, &gSSAOConfig.aoBias,              0.0f,    0.2f,   "Bias",           nullptr, "Offset to prevent self-occlusion artifacts",                   DUST_PERF_NONE },
    { "Max Depth",          DUST_SETTING_FLOAT, &gSSAOConfig.aoMaxDepth,          0.01f,   1.0f,   "MaxDepth",       nullptr, "Maximum depth to apply occlusion",                             DUST_PERF_MEDIUM },
    { "Filter Radius",      DUST_SETTING_FLOAT, &gSSAOConfig.filterRadius,        0.01f,   1.0f,   "FilterRadius",   nullptr, "Blur radius for smoothing the AO result",                      DUST_PERF_NONE },
    { "Foreground Fade",    DUST_SETTING_FLOAT, &gSSAOConfig.foregroundFade,       1.0f,    200.0f, "ForegroundFade", nullptr, "Rate at which effect fades for very close objects",          DUST_PERF_NONE },
    { "Falloff Power",      DUST_SETTING_FLOAT, &gSSAOConfig.falloffPower,        0.5f,    5.0f,   "FalloffPower",   nullptr, "How quickly occlusion falls off with sample distance",         DUST_PERF_NONE },
    { "Max Screen Radius",  DUST_SETTING_FLOAT, &gSSAOConfig.maxScreenRadius,     0.005f,  0.2f,   "MaxScreenRadius",nullptr, "Maximum sample radius in screen space",                        DUST_PERF_HIGH },
    { "Min Screen Radius",  DUST_SETTING_FLOAT, &gSSAOConfig.minScreenRadius,     0.0001f, 0.01f,  "MinScreenRadius",nullptr, "Minimum sample radius in screen space",                        DUST_PERF_HIGH },
    { "Blur Sharpness",     DUST_SETTING_FLOAT, &gSSAOConfig.blurSharpness,       0.0f,    0.1f,   "BlurSharpness",  nullptr, "Edge-aware blur sharpness (higher = preserves more detail)",   DUST_PERF_NONE },
    { "Normal Detail",     DUST_SETTING_FLOAT, &gSSAOConfig.normalDetail,        0.0f,    1.0f,   "NormalDetail",   nullptr, "Normal map influence (0 = smooth geometry, 1 = full detail)",   DUST_PERF_LOW },
    { "Direct Light AO",   DUST_SETTING_FLOAT, &gSSAOConfig.directLightOcclusion, 0.0f,    1.0f,   "DirectLightAO",  nullptr, "How much AO affects directly lit areas",                       DUST_PERF_NONE },
    { "Samples",            DUST_SETTING_INT,   &gSSAOConfig.sampleCount,         4.0f,    12.0f,  "Samples",        nullptr, "Number of sampling directions per pixel",                      DUST_PERF_HIGH },
    { "Steps",              DUST_SETTING_INT,   &gSSAOConfig.stepCount,           2.0f,    6.0f,   "Steps",          nullptr, "Number of samples per direction",                              DUST_PERF_HIGH },
    { "Debug View",         DUST_SETTING_BOOL,  &gSSAOConfig.debugView,           0.0f,    1.0f,   "DebugView",      nullptr, "Show raw ambient occlusion before compositing",                DUST_PERF_NONE },
    // Hidden settings: not shown in GUI but persisted in INI
    { "Depth Fade Start",   DUST_SETTING_HIDDEN_FLOAT, &gSSAOConfig.depthFadeStart, 0.0f, 1.0f,   "DepthFadeStart" },
    { "Tan Half FOV",       DUST_SETTING_HIDDEN_FLOAT, &gSSAOConfig.tanHalfFov,     0.1f, 2.0f,   "TanHalfFov" },
};

// No OnSettingChanged needed — values are read live each frame by SSAORenderer

// Plugin entry point
extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "SSAO";
    desc->injectionPoint    = DUST_INJECT_POST_LIGHTING;
    desc->Init              = SSAOInit;
    desc->Shutdown          = SSAOShutdown;
    desc->OnResolutionChanged = SSAOOnResolutionChanged;
    desc->preExecute        = SSAOPreExecute;
    desc->postExecute       = SSAOPostExecute;
    desc->IsEnabled         = SSAOIsEnabled;
    desc->settings          = gSettingsArray;
    desc->settingCount      = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->OnSettingChanged  = nullptr;

    // v3: Framework handles config I/O and GPU timing
    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "SSAO";

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
