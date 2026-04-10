// DustSSS.cpp - Screen Space Shadows effect plugin for Dust (API v3)
// Adds contact shadows by ray marching the depth buffer toward the sun direction.
// Exports DustEffectCreate per the Dust plugin API.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "SSSRenderer.h"
#include "SSSConfig.h"

#include <d3d11.h>
#include <cstring>
#include <string>

static const DustHostAPI* gHost = nullptr;

// Log function pointer used by DustLog.h in other translation units
DustLogFn gLogFn = nullptr;

// Global config instance (framework populates via settings array)
SSSConfig gSSSConfig;

static HMODULE gPluginModule = nullptr;

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

// Called BEFORE the game's lighting draw
static void SSSPreExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!SSSRenderer::IsInitialized())
        return;

    // Extract sun direction and view matrix from the game's bound constant buffer.
    // At this point, the game has set up all uniforms for the deferred lighting draw.
    SSSRenderer::ExtractLightData(ctx->context);
}

// Called AFTER the game's lighting draw
static void SSSPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!SSSRenderer::IsInitialized() || !SSSRenderer::HasValidLightData())
        return;

    if (!gSSSConfig.enabled)
        return;

    ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
    if (!depthSRV)
        return;

    // Generate SSS shadow mask
    ID3D11ShaderResourceView* sssSRV = SSSRenderer::RenderSSS(ctx->context, depthSRV);
    if (!sssSRV)
        return;

    // Debug overlay replaces the scene
    if (gSSSConfig.debugView)
    {
        ID3D11RenderTargetView* hdrRTV = host->GetRTV(DUST_RESOURCE_HDR_RT);
        if (hdrRTV)
            SSSRenderer::RenderDebugOverlay(ctx->context, hdrRTV);
        return;
    }

    // Composite SSS onto HDR (multiplicative darkening)
    ID3D11RenderTargetView* hdrRTV = host->GetRTV(DUST_RESOURCE_HDR_RT);
    if (hdrRTV)
        SSSRenderer::Composite(ctx->context, hdrRTV);
}

static int SSSInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
    gHost = host;
#undef Log
    gLogFn = host->Log;
#define Log DustLog

    std::string pluginDir = GetPluginDir();
    if (!SSSRenderer::Init(device, width, height, host, pluginDir.c_str()))
        return -1;

    Log("SSS: Initialized (%ux%u)", width, height);
    return 0;
}

static void SSSShutdown()
{
    SSSRenderer::Shutdown();
    Log("SSS: Shut down");
}

static void SSSOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    SSSRenderer::OnResolutionChanged(device, w, h);
    Log("SSS: Resolution changed to %ux%u", w, h);
}

static int SSSIsEnabled()
{
    // Always return 1 so preExecute runs to extract light data even when disabled.
    return 1;
}

// ==================== GUI Settings ====================

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",        DUST_SETTING_BOOL,  &gSSSConfig.enabled,        0.0f,    1.0f,     "Enabled" },
    { "Strength",       DUST_SETTING_FLOAT, &gSSSConfig.strength,       0.0f,    1.0f,     "Strength" },
    { "Max Distance",   DUST_SETTING_FLOAT, &gSSSConfig.maxDistance,    0.0005f, 0.05f,    "MaxDistance" },
    { "Step Count",     DUST_SETTING_FLOAT, &gSSSConfig.stepCount,      4.0f,    64.0f,    "StepCount" },
    { "Thickness",      DUST_SETTING_FLOAT, &gSSSConfig.thickness,      0.0001f, 0.01f,    "Thickness" },
    { "Depth Bias",     DUST_SETTING_FLOAT, &gSSSConfig.depthBias,      0.0f,    0.001f,   "DepthBias" },
    { "Max Depth",      DUST_SETTING_FLOAT, &gSSSConfig.maxDepth,       0.01f,   1.0f,     "MaxDepth" },
    { "Blur Sharpness", DUST_SETTING_FLOAT, &gSSSConfig.blurSharpness,  0.0f,    0.1f,     "BlurSharpness" },
    { "Debug View",     DUST_SETTING_BOOL,  &gSSSConfig.debugView,      0.0f,    1.0f,     "DebugView" },
    // Hidden settings
    { "Tan Half FOV",   DUST_SETTING_HIDDEN_FLOAT, &gSSSConfig.tanHalfFov, 0.1f, 2.0f,     "TanHalfFov" },
};

// Plugin entry point
extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "Screen Space Shadows";
    desc->injectionPoint    = DUST_INJECT_POST_LIGHTING;
    desc->Init              = SSSInit;
    desc->Shutdown          = SSSShutdown;
    desc->OnResolutionChanged = SSSOnResolutionChanged;
    desc->preExecute        = SSSPreExecute;
    desc->postExecute       = SSSPostExecute;
    desc->IsEnabled         = SSSIsEnabled;
    desc->settings          = gSettingsArray;
    desc->settingCount      = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->OnSettingChanged  = nullptr;

    // v3: Framework handles config I/O and GPU timing
    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "SSS";

    // Run after SSAO (which has default priority 0)
    desc->priority          = 10;

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
