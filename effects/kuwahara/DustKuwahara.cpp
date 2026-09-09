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
static void KuwaharaSettingsChanged();

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
    if (!gKuwaharaConfig.debugView && !gKuwaharaConfig.depthPreview && gKuwaharaConfig.strength <= 0.0f &&
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
    KuwaharaSettingsChanged();
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
    { "Near Depth", DUST_SETTING_FLOAT, &gKuwaharaConfig.depthStart, 0, 1, "DepthStart", nullptr, "End of the near region. Depth increases with distance: 0.01 is 1% of the camera far clip. Use Distance Fade Preview to see the transition", DUST_PERF_NONE, DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "Far Depth", DUST_SETTING_FLOAT, &gKuwaharaConfig.depthEnd, 0, 1, "DepthEnd", nullptr, "Beginning of the far region, where Far Radius and Far Strength apply. Near/Far Depth are sorted if reversed", DUST_PERF_NONE, DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "Near Radius", DUST_SETTING_FLOAT, &gKuwaharaConfig.nearRadius, 0, 8, "NearRadius", nullptr, "Filter radius on nearby objects. Zero preserves close-up detail; increasing this filters nearby objects more", DUST_PERF_NONE, DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "Near Strength", DUST_SETTING_FLOAT, &gKuwaharaConfig.nearStrength, 0, 1, "NearStrength", nullptr, "Filter strength on nearby objects. Zero preserves close-up detail; increasing this filters nearby objects more", DUST_PERF_NONE, DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "Distance Fade Preview", DUST_SETTING_BOOL, &gKuwaharaConfig.depthPreview, 0, 1, "DepthPreview", nullptr, "Black = near region, white = far region, gray = transition. Magenta = depth modulation disabled or unavailable. Preview is affected by later effects", DUST_PERF_NONE, DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "Preview Full Strength", DUST_SETTING_BOOL, &gKuwaharaConfig.debugView, 0.0f, 1.0f, "DebugView", nullptr, "Temporarily set near and far strength to 1 while keeping the selected radii. No visible change where strength is already 1. Distance Fade Preview takes precedence", DUST_PERF_NONE },
    { "Render Stage", DUST_SETTING_ENUM, &gKuwaharaConfig.renderStage, 0, 1, "RenderStage", gRenderStages, "Choose LDR to run Kuwahara after DoF, or HDR for the original early stage", DUST_PERF_NONE, DUST_SETTING_FLAG_POST_STAGE | DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "HDR Order", DUST_SETTING_HIDDEN_INT, &gKuwaharaConfig.hdrOrder, -10000, 10000, "HDROrder", nullptr, nullptr, DUST_PERF_NONE, DUST_SETTING_FLAG_POST_ORDER_HDR | DUST_SETTING_FLAG_PRESET_DEFAULT },
    { "LDR Order", DUST_SETTING_HIDDEN_INT, &gKuwaharaConfig.ldrOrder, -10000, 10000, "LDROrder", nullptr, nullptr, DUST_PERF_NONE, DUST_SETTING_FLAG_POST_ORDER_LDR | DUST_SETTING_FLAG_PRESET_DEFAULT },
};

static void KuwaharaSettingsChanged()
{
    for (auto& setting : gSettingsArray)
    {
        if (!strcmp(setting.iniKey, "Radius"))
            setting.name = gKuwaharaConfig.depthEnabled ? "Far Radius" : "Radius";
        if (!strcmp(setting.iniKey, "Strength"))
            setting.name = gKuwaharaConfig.depthEnabled ? "Far Strength" : "Strength";
    }
}

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
    desc->OnSettingChanged  = KuwaharaSettingsChanged;

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
