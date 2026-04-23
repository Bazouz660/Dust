// DustOutline.cpp - Edge-detection outline effect plugin for Dust (API v3)
// Detects edges from depth and normal discontinuities and draws outlines
// over the scene for a stylized look.
// Exports DustEffectCreate per the Dust plugin API.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "OutlineRenderer.h"
#include "OutlineConfig.h"

#include <d3d11.h>
#include <cstring>
#include <string>

DustLogFn gLogFn = nullptr;

OutlineConfig gOutlineConfig;

static HMODULE gPluginModule = nullptr;
static const DustHostAPI* gHost = nullptr;

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

// Called AFTER the game's lighting draw (before fog)
static void OutlinePostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!OutlineRenderer::IsInitialized() || !gOutlineConfig.enabled)
        return;

    ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
    if (!depthSRV)
        return;

    ID3D11ShaderResourceView* normalsSRV = host->GetSRV(DUST_RESOURCE_NORMALS);

    ID3D11RenderTargetView* hdrRTV = host->GetRTV(DUST_RESOURCE_HDR_RT);
    if (!hdrRTV)
        return;

    if (gOutlineConfig.debugView)
    {
        OutlineRenderer::RenderDebugOverlay(ctx->context, depthSRV, normalsSRV, hdrRTV);
        return;
    }

    ID3D11ShaderResourceView* sceneCopy = host->GetSceneCopy(ctx->context, DUST_RESOURCE_HDR_RT);
    if (!sceneCopy)
        return;

    OutlineRenderer::Render(ctx->context, sceneCopy, depthSRV, normalsSRV, hdrRTV);
}

static int OutlineInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gHost = host;

    std::string pluginDir = GetPluginDir();
    if (!OutlineRenderer::Init(device, width, height, host, pluginDir.c_str()))
        return -1;

    Log("Outline: Initialized (%ux%u)", width, height);
    return 0;
}

static void OutlineShutdown()
{
    OutlineRenderer::Shutdown();
    Log("Outline: Shut down");
}

static void OutlineOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    OutlineRenderer::OnResolutionChanged(device, w, h);
    Log("Outline: Resolution changed to %ux%u", w, h);
}

static int OutlineIsEnabled()
{
    return gOutlineConfig.enabled ? 1 : 0;
}

// ==================== GUI Settings ====================

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",           DUST_SETTING_BOOL,  &gOutlineConfig.enabled,          0.0f,    1.0f,  "Enabled",         nullptr, "Enable or disable edge outlines" },
    { "Depth Threshold",   DUST_SETTING_FLOAT, &gOutlineConfig.depthThreshold,   0.0001f, 0.01f, "DepthThreshold",  nullptr, "Minimum depth difference to draw an outline" },
    { "Normal Threshold",  DUST_SETTING_FLOAT, &gOutlineConfig.normalThreshold,  0.0f,    1.0f,  "NormalThreshold", nullptr, "Minimum normal difference to draw an outline" },
    { "Thickness",         DUST_SETTING_INT,   &gOutlineConfig.thickness,        1.0f,    5.0f,  "Thickness",       nullptr, "Outline width in pixels" },
    { "Strength",          DUST_SETTING_FLOAT, &gOutlineConfig.strength,         0.0f,    1.0f,  "Strength",        nullptr, "Outline opacity (0 = invisible, 1 = full)" },
    { "Color R",           DUST_SETTING_FLOAT, &gOutlineConfig.colorR,           0.0f,    1.0f,  "ColorR",          nullptr, "Red component of the outline color" },
    { "Color G",           DUST_SETTING_FLOAT, &gOutlineConfig.colorG,           0.0f,    1.0f,  "ColorG",          nullptr, "Green component of the outline color" },
    { "Color B",           DUST_SETTING_FLOAT, &gOutlineConfig.colorB,           0.0f,    1.0f,  "ColorB",          nullptr, "Blue component of the outline color" },
    { "Max Depth",         DUST_SETTING_FLOAT, &gOutlineConfig.maxDepth,         0.01f,   1.0f,  "MaxDepth",        nullptr, "Maximum depth to draw outlines (skip distant objects)" },
    { "Debug View",        DUST_SETTING_BOOL,  &gOutlineConfig.debugView,        0.0f,    1.0f,  "DebugView",       nullptr, "Show raw edge detection result" },
};

// Plugin entry point
extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "Outline";
    desc->injectionPoint    = DUST_INJECT_POST_LIGHTING;
    desc->Init              = OutlineInit;
    desc->Shutdown          = OutlineShutdown;
    desc->OnResolutionChanged = OutlineOnResolutionChanged;
    desc->preExecute        = nullptr;
    desc->postExecute       = OutlinePostExecute;
    desc->IsEnabled         = OutlineIsEnabled;
    desc->settings          = gSettingsArray;
    desc->settingCount      = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->OnSettingChanged  = nullptr;

    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "Outline";

    // Run after SSAO/SSIL at POST_LIGHTING
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
