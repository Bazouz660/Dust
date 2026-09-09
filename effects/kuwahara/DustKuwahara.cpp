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

    // strength 0 => output is `lerp(original, filtered, 0)` — identical,
    // skip the scene copy and the (expensive) filter pass entirely
    if (!gKuwaharaConfig.debugView && gKuwaharaConfig.strength <= 0.0f &&
        (!gKuwaharaConfig.depthEnabled || gKuwaharaConfig.nearStrength <= 0.0f))
        return;

    const char* target = ctx->point == DUST_INJECT_POST_TONEMAP ? DUST_RESOURCE_LDR_RT : DUST_RESOURCE_HDR_RT;
    ID3D11RenderTargetView* hdrRTV = host->GetRTV(target);
    if (!hdrRTV)
        return;

    ID3D11ShaderResourceView* sceneCopy = host->GetSceneCopy(ctx->context, target);
    if (!sceneCopy)
        return;

    KuwaharaRenderer::Render(ctx->context, sceneCopy, host->GetSRV(DUST_RESOURCE_DEPTH), hdrRTV);
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

static const char* const gRenderStages[] = { "Before tonemapping (HDR)", "After tonemapping (LDR)", nullptr };
static DustSettingDesc gSettingsArray[] = {
    { "Enabled",    DUST_SETTING_BOOL,  &gKuwaharaConfig.enabled,    0.0f,  1.0f,  "Enabled",   nullptr, "Enable or disable the Kuwahara filter",                     DUST_PERF_MEDIUM },
    { "Radius",     DUST_SETTING_INT,   &gKuwaharaConfig.radius,     0.0f,  8.0f,  "Radius",    nullptr, "Filter radius in pixels; the far radius when Depth Dependent is enabled", DUST_PERF_HIGH },
    { "Strength",   DUST_SETTING_FLOAT, &gKuwaharaConfig.strength,   0.0f,  1.0f,  "Strength",  nullptr, "Blend factor between original and filtered image",          DUST_PERF_NONE },
    { "Sharpness",  DUST_SETTING_FLOAT, &gKuwaharaConfig.sharpness,  1.0f,  16.0f, "Sharpness", nullptr, "Edge preservation sharpness (higher = crisper boundaries)", DUST_PERF_NONE },
    { "Depth Dependent", DUST_SETTING_BOOL, &gKuwaharaConfig.depthEnabled, 0, 1, "DepthEnabled", nullptr, "Vary radius and strength with camera distance", DUST_PERF_LOW, DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "Depth Start", DUST_SETTING_FLOAT, &gKuwaharaConfig.depthStart, 0, 1, "DepthStart", nullptr, "Near endpoint: linear distance divided by far clip, using the same units as DoF", DUST_PERF_NONE, DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "Depth End", DUST_SETTING_FLOAT, &gKuwaharaConfig.depthEnd, 0, 1, "DepthEnd", nullptr, "Far endpoint: reaches Radius and Strength here; endpoints are sorted if reversed", DUST_PERF_NONE, DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "Near Radius", DUST_SETTING_FLOAT, &gKuwaharaConfig.nearRadius, 0, 8, "NearRadius", nullptr, "Radius at and before Depth Start; zero preserves close-up detail", DUST_PERF_NONE, DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "Near Strength", DUST_SETTING_FLOAT, &gKuwaharaConfig.nearStrength, 0, 1, "NearStrength", nullptr, "Blend strength at and before Depth Start; zero leaves nearby pixels unchanged", DUST_PERF_NONE, DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "Debug View", DUST_SETTING_BOOL,  &gKuwaharaConfig.debugView,  0.0f,  1.0f,  "DebugView", nullptr, "Show the filtered result without blending",                 DUST_PERF_NONE },
    { "Render Stage", DUST_SETTING_ENUM, &gKuwaharaConfig.renderStage, 0, 1, "RenderStage", gRenderStages, "Choose LDR to filter the DoF result; drag the effect header to adjust its position in that group", DUST_PERF_NONE, DUST_SETTING_FLAG_POST_STAGE | DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "HDR Order", DUST_SETTING_HIDDEN_INT, &gKuwaharaConfig.hdrOrder, -10000, 10000, "HDROrder", nullptr, nullptr, DUST_PERF_NONE, DUST_SETTING_FLAG_POST_ORDER_HDR | DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "LDR Order", DUST_SETTING_HIDDEN_INT, &gKuwaharaConfig.ldrOrder, -10000, 10000, "LDROrder", nullptr, nullptr, DUST_PERF_NONE, DUST_SETTING_FLAG_POST_ORDER_LDR | DUST_SETTING_FLAG_PRESET_DEFAULT },
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
