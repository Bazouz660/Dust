// DustClarity.cpp - Clarity / Local Contrast effect plugin for Dust (API v3)
// Enhances midtone detail by extracting and amplifying local contrast.
// Exports DustEffectCreate per the Dust plugin API.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "ClarityRenderer.h"
#include "ClarityConfig.h"

#include <d3d11.h>
#include <cstring>
#include <string>

static const DustHostAPI* gHost = nullptr;

DustLogFn gLogFn = nullptr;

ClarityConfig gClarityConfig;

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
static void ClarityPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!ClarityRenderer::IsInitialized() || !gClarityConfig.enabled)
        return;

    // Get a copy of the current LDR scene (after LUT has run)
    ID3D11ShaderResourceView* sceneCopy = host->GetSceneCopy(ctx->context, DUST_RESOURCE_LDR_RT);
    if (!sceneCopy)
        return;

    ID3D11RenderTargetView* ldrRTV = host->GetRTV(DUST_RESOURCE_LDR_RT);
    if (!ldrRTV)
        return;

    if (gClarityConfig.debugView)
    {
        ClarityRenderer::RenderDebugOverlay(ctx->context, sceneCopy, ldrRTV);
        return;
    }

    ClarityRenderer::Render(ctx->context, sceneCopy, ldrRTV);
}

static int ClarityInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
    gHost = host;
#undef Log
    gLogFn = host->Log;
#define Log DustLog

    std::string pluginDir = GetPluginDir();
    if (!ClarityRenderer::Init(device, width, height, host, pluginDir.c_str()))
        return -1;

    Log("Clarity: Initialized (%ux%u)", width, height);
    return 0;
}

static void ClarityShutdown()
{
    ClarityRenderer::Shutdown();
    Log("Clarity: Shut down");
}

static void ClarityOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    ClarityRenderer::OnResolutionChanged(device, w, h);
    Log("Clarity: Resolution changed to %ux%u", w, h);
}

static int ClarityIsEnabled()
{
    return gClarityConfig.enabled ? 1 : 0;
}

// ==================== GUI Settings ====================

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",          DUST_SETTING_BOOL,  &gClarityConfig.enabled,        0.0f, 1.0f,  "Enabled",        nullptr, "Enable or disable the clarity effect" },
    { "Strength",         DUST_SETTING_FLOAT, &gClarityConfig.strength,       0.0f, 2.0f,  "Strength",       nullptr, "Intensity of local contrast enhancement" },
    { "Midtone Protect",  DUST_SETTING_FLOAT, &gClarityConfig.midtoneProtect, 0.0f, 1.0f,  "MidtoneProtect", nullptr, "Protect midtones from over-sharpening (0 = no protection)" },
    { "Blur Radius",      DUST_SETTING_FLOAT, &gClarityConfig.blurRadius,     1.0f, 32.0f, "BlurRadius",     nullptr, "Size of the detail extraction blur" },
    { "Debug View",       DUST_SETTING_BOOL,  &gClarityConfig.debugView,      0.0f, 1.0f,  "DebugView",      nullptr, "Show the extracted detail layer" },
};

// Plugin entry point
extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "Clarity";
    desc->injectionPoint    = DUST_INJECT_POST_TONEMAP;
    desc->Init              = ClarityInit;
    desc->Shutdown          = ClarityShutdown;
    desc->OnResolutionChanged = ClarityOnResolutionChanged;
    desc->preExecute        = nullptr;
    desc->postExecute       = ClarityPostExecute;
    desc->IsEnabled         = ClarityIsEnabled;
    desc->settings          = gSettingsArray;
    desc->settingCount      = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->OnSettingChanged  = nullptr;

    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "Clarity";

    // After LUT (0), before Bloom (100)
    desc->priority          = 50;

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
