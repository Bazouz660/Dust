// DustSubsurfaceScattering.cpp - Screen-space subsurface scattering for skin.
//
// Skin pixels are tagged by the host's GBuffer-shader patch (character.hlsl /
// skin.hlsl) into the NORMALS render target alpha. This POST_LIGHTING effect
// reads that tag, the lit HDR scene and depth, runs a separable depth-aware
// skin-diffusion blur over tagged pixels, and writes the result back to the HDR
// RT. Because only the already-lit HDR is available (not a separated diffuse
// buffer), this is an approximation: it blurs the lit skin color rather than
// pre-light irradiance.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "SSSRenderer.h"
#include "SSSConfig.h"

#include <d3d11.h>
#include <cstring>
#include <string>

static const DustHostAPI* gHost = nullptr;

DustLogFn gLogFn = nullptr;
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

// Runs after the deferred lighting pass — the lit HDR scene is ready and the
// skin tag in NORMALS.a is still intact.
static void SSSPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!SSSRenderer::IsInitialized() || !gSSSConfig.enabled)
        return;

    ID3D11ShaderResourceView* normalsSRV = host->GetSRV(DUST_RESOURCE_NORMALS);
    if (!normalsSRV)
        return;

    ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
    if (!depthSRV)
        return;

    ID3D11ShaderResourceView* sceneCopy = host->GetSceneCopy(ctx->context, DUST_RESOURCE_HDR_RT);
    if (!sceneCopy)
        return;

    ID3D11RenderTargetView* hdrRTV = host->GetRTV(DUST_RESOURCE_HDR_RT);
    if (!hdrRTV)
        return;

    SSSRenderer::Render(ctx->context, sceneCopy, normalsSRV, depthSRV, hdrRTV,
                        ctx->width, ctx->height);
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
    return gSSSConfig.enabled ? 1 : 0;
}

// ==================== GUI Settings ====================

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",         DUST_SETTING_BOOL,  &gSSSConfig.enabled,        0.0f,  1.0f,  "Enabled",        nullptr, "Enable or disable subsurface scattering on skin",                  DUST_PERF_MEDIUM },
    { "Strength",        DUST_SETTING_FLOAT, &gSSSConfig.strength,       0.0f,  1.0f,  "Strength",       nullptr, "Blend of blurred skin vs. original (0 = off, 1 = full)",           DUST_PERF_NONE   },
    { "Blur Width",      DUST_SETTING_FLOAT, &gSSSConfig.blurWidth,      0.0f,  24.0f, "BlurWidth",      nullptr, "Base diffusion radius (scaled inversely by depth so it is world-constant)", DUST_PERF_MEDIUM },
    { "Max Radius (px)", DUST_SETTING_FLOAT, &gSSSConfig.maxRadiusPx,    1.0f,  64.0f, "MaxRadiusPx",    nullptr, "Hard cap on per-tap pixel offset (stability for close-up faces)",  DUST_PERF_LOW    },
    { "Depth Threshold", DUST_SETTING_FLOAT, &gSSSConfig.depthThreshold, 0.001f,0.2f,  "DepthThreshold", nullptr, "Reject blur taps whose depth differs more than this fraction",     DUST_PERF_NONE   },
    { "Follow Surface",  DUST_SETTING_FLOAT, &gSSSConfig.followSurface,  0.0f,  1.0f,  "FollowSurface",  nullptr, "How strongly to restrict blur to skin-tagged taps (1 = strict)",   DUST_PERF_NONE   },
    { "Debug View",      DUST_SETTING_BOOL,  &gSSSConfig.debugView,      0.0f,  1.0f,  "DebugView",      nullptr, "Tint skin-tagged pixels magenta to inspect tag coverage",          DUST_PERF_NONE   },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion          = DUST_API_VERSION;
    desc->name                = "Subsurface Scattering";
    desc->injectionPoint      = DUST_INJECT_POST_LIGHTING;
    desc->Init                = SSSInit;
    desc->Shutdown            = SSSShutdown;
    desc->OnResolutionChanged = SSSOnResolutionChanged;
    desc->preExecute          = nullptr;
    desc->postExecute         = SSSPostExecute;
    desc->IsEnabled           = SSSIsEnabled;
    desc->settings            = gSettingsArray;
    desc->settingCount        = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->OnSettingChanged    = nullptr;

    desc->flags               = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection       = "SubsurfaceScattering";

    // Run after RTGI (which composites indirect light) but it is independent;
    // default priority is fine for a self-contained skin pass.
    desc->priority            = 60;

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
