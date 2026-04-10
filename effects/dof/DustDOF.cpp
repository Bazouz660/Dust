// DustDOF.cpp - Depth of Field effect plugin for Dust (API v3)
// Blurs the scene based on distance from a configurable focus plane.
// Exports DustEffectCreate per the Dust plugin API.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "DOFRenderer.h"
#include "DOFConfig.h"

#include <d3d11.h>
#include <cstring>
#include <string>

DustLogFn gLogFn = nullptr;

DOFConfig gDOFConfig;

static HMODULE gPluginModule = nullptr;

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

// Called AFTER the game's tonemap draw
static void DOFPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!DOFRenderer::IsInitialized() || !gDOFConfig.enabled)
        return;

    ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
    if (!depthSRV)
        return;

    ID3D11RenderTargetView* ldrRTV = host->GetRTV(DUST_RESOURCE_LDR_RT);
    if (!ldrRTV)
        return;

    if (gDOFConfig.debugView)
    {
        DOFRenderer::RenderDebugOverlay(ctx->context, depthSRV, ldrRTV);
        return;
    }

    ID3D11ShaderResourceView* sceneCopy = host->GetSceneCopy(ctx->context, DUST_RESOURCE_LDR_RT);
    if (!sceneCopy)
        return;

    DOFRenderer::Render(ctx->context, sceneCopy, depthSRV, ldrRTV);
}

static int DOFInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
#undef Log
    gLogFn = host->Log;
#define Log DustLog

    std::string pluginDir = GetPluginDir();
    if (!DOFRenderer::Init(device, width, height, host, pluginDir.c_str()))
        return -1;

    Log("DOF: Initialized (%ux%u)", width, height);
    return 0;
}

static void DOFShutdown()
{
    DOFRenderer::Shutdown();
    Log("DOF: Shut down");
}

static void DOFOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    DOFRenderer::OnResolutionChanged(device, w, h);
    Log("DOF: Resolution changed to %ux%u", w, h);
}

static int DOFIsEnabled()
{
    return gDOFConfig.enabled ? 1 : 0;
}

// ==================== GUI Settings ====================

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",        DUST_SETTING_BOOL,  &gDOFConfig.enabled,        0.0f, 1.0f,   "Enabled" },
    { "Focus Distance", DUST_SETTING_FLOAT, &gDOFConfig.focusDistance,  0.001f, 0.1f,  "FocusDistance" },
    { "Focus Range",    DUST_SETTING_FLOAT, &gDOFConfig.focusRange,     0.001f, 0.05f, "FocusRange" },
    { "Blur Strength",  DUST_SETTING_FLOAT, &gDOFConfig.blurStrength,   0.0f, 2.0f,   "BlurStrength" },
    { "Blur Radius",    DUST_SETTING_FLOAT, &gDOFConfig.blurRadius,     1.0f, 32.0f,  "BlurRadius" },
    { "Max Depth",      DUST_SETTING_FLOAT, &gDOFConfig.maxDepth,       0.01f, 1.0f,  "MaxDepth" },
    { "Debug View",     DUST_SETTING_BOOL,  &gDOFConfig.debugView,      0.0f, 1.0f,   "DebugView" },
};

// Plugin entry point
extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "Depth of Field";
    desc->injectionPoint    = DUST_INJECT_POST_TONEMAP;
    desc->Init              = DOFInit;
    desc->Shutdown          = DOFShutdown;
    desc->OnResolutionChanged = DOFOnResolutionChanged;
    desc->preExecute        = nullptr;
    desc->postExecute       = DOFPostExecute;
    desc->IsEnabled         = DOFIsEnabled;
    desc->settings          = gSettingsArray;
    desc->settingCount      = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->OnSettingChanged  = nullptr;

    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "DOF";

    desc->priority          = 75;

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
