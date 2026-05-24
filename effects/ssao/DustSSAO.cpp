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

// White-albedo texture for "true white world" debug visualization.
// Bound at slot t0 (Kenshi's albedo) when DebugView is enabled, replacing
// the GBuffer RT0 so the deferred lighting renders the scene with white
// surfaces. AO still applies via slot 8 for white-world debug visualization.
static ID3D11Texture2D*          gWhiteAlbedoTex = nullptr;
static ID3D11ShaderResourceView* gWhiteAlbedoSRV = nullptr;

// 1x1 dynamic texture for passing directLightOcclusion to the deferred shader (slot t9)
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

// 1x1 B8G8R8A8 encoded to decode as pure white in Kenshi's YCoCg deferred.
// Kenshi RT0 layout: R=Y (luma), G=Cg-or-Co (chroma centered at 0.5),
// B=metalness, A=gloss. White → Y=1, Cg=Co=0, low metalness, low gloss.
// Memory byte order for BGRA8 = (B, G, R, A) = (metal, chroma, luma, gloss).
static bool CreateWhiteAlbedoTexture(ID3D11Device* device)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // BGRA byte order: B=0 (metal=0), G=128 (chroma=0 centered), R=255 (Y=1), A=64 (gloss≈0.25).
    uint8_t pixel[4] = { 0x00, 0x80, 0xFF, 0x40 };
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = pixel;
    init.SysMemPitch = 4;

    HRESULT hr = device->CreateTexture2D(&desc, &init, &gWhiteAlbedoTex);
    if (FAILED(hr)) { Log("SSAO: Failed to create white albedo tex: 0x%08X", hr); return false; }
    hr = device->CreateShaderResourceView(gWhiteAlbedoTex, nullptr, &gWhiteAlbedoSRV);
    if (FAILED(hr)) { Log("SSAO: Failed to create white albedo SRV: 0x%08X", hr); return false; }
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

// Bind AO SRV + sampler at explicit D3D slots t8/s8.
// Uses direct D3D calls (not host->BindSRV) because the shader declares
// explicit register(t8) which must not be remapped by ResolveSlot.
static void BindAoDirect(ID3D11DeviceContext* dc, ID3D11ShaderResourceView* srv)
{
    dc->PSSetShaderResources(8, 1, &srv);
    dc->PSSetSamplers(8, 1, &gAoSampler);
}

// Bind params SRV at explicit D3D slot t9 (shares sampler at s8).
static void BindParamsDirect(ID3D11DeviceContext* dc)
{
    dc->PSSetShaderResources(9, 1, &gParamsSRV);
}

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

    UpdateParamsTexture(ctx->context);
    BindParamsDirect(ctx->context);

    if (!SSAORenderer::IsInitialized())
    {
        static bool sWarnedNoInit = false;
        if (!sWarnedNoInit) { Log("[SSAO] WARNING: Renderer not initialized, binding white fallback"); sWarnedNoInit = true; }
        BindAoDirect(ctx->context, gWhiteSRV);
        return;
    }

    if (!gSSAOConfig.enabled)
    {
        BindAoDirect(ctx->context, gWhiteSRV);
        return;
    }

    // Generate AO (saves/restores GPU state internally)
    ID3D11ShaderResourceView* aoSRV = SSAORenderer::RenderAO(ctx->context, depthSRV, &ctx->camera);
    if (!aoSRV)
    {
        Log("[SSAO] WARNING: RenderAO returned null, binding white fallback");
        aoSRV = gWhiteSRV;
    }

    // Bind AO at t8/s8 — persists through main_fs AND light_fs draws
    BindAoDirect(ctx->context, aoSRV);
}

// Called AFTER the game's main_fs draw.
// Do NOT unbind AO slots here — light_fs (point lights) draws AFTER this
// and needs the AO texture at t8 and params at t9 to still be bound.
static void SSAOPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
}

// Export consumed by the DustSSAODebug companion plugin at PRE_PRESENT.
// Draws the diagnostic overlay onto whatever RTV the caller hands us — the
// LDR target is the intended consumer since it survives until swap.
extern "C" __declspec(dllexport) void Dust_RenderSSAODebugOverlay(
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* targetRTV,
    ID3D11ShaderResourceView* depthSRV)
{
    if (!ctx || !targetRTV || !depthSRV) return;
    if (gSSAOConfig.debugViewMode == 0 || !gSSAOConfig.enabled) return;
    SSAORenderer::RenderDebugOverlay(ctx, targetRTV, depthSRV);
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

    if (!CreateWhiteAlbedoTexture(device))
        return -4;

    if (!CreateParamsTexture(device))
        return -5;

    std::string pluginDir = GetPluginDir();
    if (!SSAORenderer::Init(device, width, height, host, pluginDir.c_str()))
        return -3;

    Log("SSAO: Initialized (%ux%u)", width, height);
    return 0;
}

static void SSAOShutdown()
{
    SSAORenderer::Shutdown();

    if (gAoSampler)       { gAoSampler->Release();      gAoSampler = nullptr; }
    if (gWhiteSRV)        { gWhiteSRV->Release();       gWhiteSRV = nullptr; }
    if (gWhiteTex)        { gWhiteTex->Release();       gWhiteTex = nullptr; }
    if (gWhiteAlbedoSRV)  { gWhiteAlbedoSRV->Release(); gWhiteAlbedoSRV = nullptr; }
    if (gWhiteAlbedoTex)  { gWhiteAlbedoTex->Release(); gWhiteAlbedoTex = nullptr; }
    if (gParamsSRV)       { gParamsSRV->Release();      gParamsSRV = nullptr; }
    if (gParamsTex)       { gParamsTex->Release();      gParamsTex = nullptr; }

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
// AO quality and blending settings exposed in the GUI.

static const char* const kQualityLabels[] = {
    "Low", "Medium", "High", "Very High", "Ultra", "Extreme", "IDGAF", nullptr
};
static const char* const kShadingRateLabels[] = {
    "Full Rate", "Half Rate", "Quarter Rate", nullptr
};
static const char* const kAoTypeLabels[] = {
    "GTAO", "Solid Angle", "Visibility Bitmask", "Bitmask + Solid Angle", nullptr
};
static const char* const kDebugViewLabels[] = {
    "Off",                  // 0
    "Final AO",             // 1
    "Raw Depth",            // 2
    "Linearized Z",         // 3
    "View Pos.z",           // 4
    "Normals from Depth",   // 5
    "Jitter / Blue Noise",  // 6
    "Filtered AO",          // 7
    nullptr
};

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",          DUST_SETTING_BOOL,  &gSSAOConfig.enabled,             0.0f, 1.0f,  "Enabled",     nullptr,             "Enable or disable ambient occlusion.",                                                 DUST_PERF_MEDIUM },

    // --- Global ---
    { "Sample Quality",   DUST_SETTING_ENUM,  &gSSAOConfig.sampleQualityPreset, 0.0f, 6.0f,  "SampleQuality", kQualityLabels,    "Global quality control, main performance knob. Higher radii might require higher quality.", DUST_PERF_HIGH },
    { "Shading Rate",     DUST_SETTING_ENUM,  &gSSAOConfig.shadingRate,         0.0f, 2.0f,  "ShadingRate",   kShadingRateLabels,"0: render all pixels each frame\n1: render only 50% of pixels each frame\n2: render only 25% of pixels each frame", DUST_PERF_HIGH },
    { "Sample Radius",    DUST_SETTING_FLOAT, &gSSAOConfig.sampleRadius,        0.5f, 10.0f, "SampleRadius",  nullptr,           "AO sample radius, higher means more large-scale occlusion with less fine-scale details.", DUST_PERF_NONE },
    { "Increase Radius with Distance", DUST_SETTING_BOOL, &gSSAOConfig.worldspaceEnable, 0.0f, 1.0f, "WorldspaceEnable", nullptr, "When enabled, AO radius scales with depth instead of staying world-space-fixed.", DUST_PERF_NONE },

    // --- Blending ---
    { "Ambient Occlusion Amount", DUST_SETTING_FLOAT, &gSSAOConfig.ssaoAmount,  0.0f, 1.0f,  "SsaoAmount",    nullptr,           "Intensity of AO effect. Can cause pitch black clipping if set too high.", DUST_PERF_NONE },
    { "Fade Out Distance", DUST_SETTING_FLOAT, &gSSAOConfig.fadeDepth,          0.0f, 1.0f,  "FadeDepth",     nullptr,           "Distance at which AO fades out. Higher values extend AO visibility.", DUST_PERF_NONE },
    { "Filter Quality",   DUST_SETTING_INT,   &gSSAOConfig.filterSize,          0.0f, 2.0f,  "FilterSize",    nullptr,           "Number of bilateral filter iterations (0–2).", DUST_PERF_LOW },

    // --- Algorithm variant ---
    { "AO Type",          DUST_SETTING_ENUM,  &gSSAOConfig.aoType,              0.0f, 3.0f,  "AoType",        kAoTypeLabels,     "0: GTAO (high contrast, fast)\n1: Solid Angle (smoother, fastest)\n2: Visibility Bitmask (highest quality)\n3: Bitmask + Solid Angle (smoother bitmask)", DUST_PERF_LOW },

    // --- Debug ---
    { "Debug View",       DUST_SETTING_ENUM,  &gSSAOConfig.debugViewMode,       0.0f, 7.0f, "DebugViewMode", kDebugViewLabels,  "Diagnostic visualizations for each pipeline stage.", DUST_PERF_NONE },

    // --- Dust-specific (lighting integration) ---
    { "Direct Light AO",  DUST_SETTING_FLOAT, &gSSAOConfig.directLightOcclusion, 0.0f, 1.0f, "DirectLightAO", nullptr,           "How much AO affects directly lit areas (0 = ambient only, 1 = full).", DUST_PERF_NONE },

    // FOV is now hardcoded to 0.4142 (45° vertical), measured from Kenshi's
    // projection matrix. Hidden again — not user-tunable.
    { "Tan Half FOV",     DUST_SETTING_HIDDEN_FLOAT, &gSSAOConfig.tanHalfFov,   0.1f, 2.0f,  "TanHalfFov" },
    { "Far Plane",        DUST_SETTING_FLOAT, &gSSAOConfig.farPlane,            1.0f, 10000.0f, "FarPlane", nullptr, "Depth scale: z = depth * FarPlane + 1. High values cause halo artifacts at geometry/sky boundaries. Default 1000.", DUST_PERF_NONE },
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
