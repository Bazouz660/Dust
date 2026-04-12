// DustKuwahara.cpp - Kuwahara filter effect plugin for Dust (API v3)
// Applies a painterly smoothing filter that preserves edges.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "KuwaharaRenderer.h"
#include "KuwaharaConfig.h"

#include <d3d11.h>
#include <cstring>
#include <string>

DustLogFn gLogFn = nullptr;

KuwaharaConfig gKuwaharaConfig;

static HMODULE gPluginModule = nullptr;

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

static void KuwaharaPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!KuwaharaRenderer::IsInitialized() || !gKuwaharaConfig.enabled)
        return;

    ID3D11RenderTargetView* hdrRTV = host->GetRTV(DUST_RESOURCE_HDR_RT);
    if (!hdrRTV)
        return;

    ID3D11ShaderResourceView* sceneCopy = host->GetSceneCopy(ctx->context, DUST_RESOURCE_HDR_RT);
    if (!sceneCopy)
        return;

    KuwaharaRenderer::Render(ctx->context, sceneCopy, hdrRTV);
}

static int KuwaharaInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
#undef Log
    gLogFn = host->Log;
#define Log DustLog

    std::string pluginDir = GetPluginDir();
    if (!KuwaharaRenderer::Init(device, width, height, host, pluginDir.c_str()))
        return -1;

    Log("Kuwahara: Initialized (%ux%u)", width, height);
    return 0;
}

static void KuwaharaShutdown()
{
    KuwaharaRenderer::Shutdown();
    Log("Kuwahara: Shut down");
}

static void KuwaharaOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    KuwaharaRenderer::OnResolutionChanged(device, w, h);
}

static int KuwaharaIsEnabled()
{
    return gKuwaharaConfig.enabled ? 1 : 0;
}

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",    DUST_SETTING_BOOL,  &gKuwaharaConfig.enabled,    0.0f,  1.0f,  "Enabled" },
    { "Radius",     DUST_SETTING_INT,   &gKuwaharaConfig.radius,     2.0f,  8.0f,  "Radius" },
    { "Strength",   DUST_SETTING_FLOAT, &gKuwaharaConfig.strength,   0.0f,  1.0f,  "Strength" },
    { "Sharpness",  DUST_SETTING_FLOAT, &gKuwaharaConfig.sharpness,  1.0f,  16.0f, "Sharpness" },
    { "Debug View", DUST_SETTING_BOOL,  &gKuwaharaConfig.debugView,  0.0f,  1.0f,  "DebugView" },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "Kuwahara";
    desc->injectionPoint    = DUST_INJECT_POST_LIGHTING;
    desc->Init              = KuwaharaInit;
    desc->Shutdown          = KuwaharaShutdown;
    desc->OnResolutionChanged = KuwaharaOnResolutionChanged;
    desc->preExecute        = nullptr;
    desc->postExecute       = KuwaharaPostExecute;
    desc->IsEnabled         = KuwaharaIsEnabled;
    desc->settings          = gSettingsArray;
    desc->settingCount      = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->OnSettingChanged  = nullptr;

    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "Kuwahara";

    // Run before outline (50) in post-lighting
    desc->priority          = 40;

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
