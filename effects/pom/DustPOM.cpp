// DustPOM.cpp - Parallax Occlusion Mapping settings plugin for Dust (API v5).
// Owns the GUI / INI persistence for POM parameters. Pushes values to the host
// (POMState) via DustHostAPI v5 functions; the host binds the cbuffer at PS
// slot 8 on GBuffer pass entry, and the patched objects.hlsl reads from there.

#include "../../src/DustAPI.h"
#include "DustLog.h"

#include <d3d11.h>
#include <windows.h>
#include <cstring>

DustLogFn gLogFn = nullptr;

struct POMConfig
{
    bool  enabled        = true;
    float heightScale    = 0.02f;   // UV-space displacement at full depth
    float threshold      = 0.15f;   // luminance below this -> no displacement
    float thresholdWidth = 0.85f;   // saturate((lum - threshold) / width)
    int   minSamples     = 8;       // ray-march samples at head-on view
    int   maxSamples     = 32;      // ray-march samples at grazing view
};

static POMConfig gConfig;
static const DustHostAPI* gHost = nullptr;

static void PushAll()
{
    if (!gHost) return;
    if (gHost->SetPOMEnabled)        gHost->SetPOMEnabled(gConfig.enabled ? 1 : 0);
    if (gHost->SetPOMHeightScale)    gHost->SetPOMHeightScale(gConfig.heightScale);
    if (gHost->SetPOMThreshold)      gHost->SetPOMThreshold(gConfig.threshold);
    if (gHost->SetPOMThresholdWidth) gHost->SetPOMThresholdWidth(gConfig.thresholdWidth);
    if (gHost->SetPOMSamples)        gHost->SetPOMSamples(gConfig.minSamples, gConfig.maxSamples);
}

static int POMInit(ID3D11Device* /*device*/, uint32_t /*w*/, uint32_t /*h*/, const DustHostAPI* host)
{
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gHost = host;
    PushAll();
    Log("POM: initialized (enabled=%d, scale=%.3f, threshold=%.2f)",
        (int)gConfig.enabled, gConfig.heightScale, gConfig.threshold);
    return 0;
}

static void POMShutdown()
{
    Log("POM: shut down");
}

static int POMIsEnabled() { return gConfig.enabled ? 1 : 0; }

static void POMOnSettingChanged()
{
    PushAll();
}

static DustSettingDesc gSettings[] = {
    { "Enabled",         DUST_SETTING_BOOL,  &gConfig.enabled,        0.0f,  1.0f,  "Enabled",        nullptr, "Enable parallax occlusion mapping on world objects (skipped on foliage).", DUST_PERF_MEDIUM },
    { "Height Scale",    DUST_SETTING_FLOAT, &gConfig.heightScale,    0.0f,  0.05f, "HeightScale",    nullptr, "Maximum UV displacement at full depth. Higher = stronger relief; too high causes stretching at grazing angles.", DUST_PERF_NONE },
    { "Threshold",       DUST_SETTING_FLOAT, &gConfig.threshold,      0.0f,  0.5f,  "Threshold",      nullptr, "Luminance below this value produces no displacement. Anchors dark detail and prevents per-pixel drift.", DUST_PERF_NONE },
    { "Threshold Width", DUST_SETTING_FLOAT, &gConfig.thresholdWidth, 0.05f, 1.0f,  "ThresholdWidth", nullptr, "Falloff width of the luminance contrast curve: saturate((lum - threshold) / width).", DUST_PERF_NONE },
    { "Min Samples",     DUST_SETTING_INT,   &gConfig.minSamples,     2.0f,  16.0f, "MinSamples",     nullptr, "Ray-march sample count when viewing the surface head-on (low parallax needed).", DUST_PERF_LOW },
    { "Max Samples",     DUST_SETTING_INT,   &gConfig.maxSamples,     8.0f,  64.0f, "MaxSamples",     nullptr, "Ray-march sample count at grazing angles (more samples = less artifacting at the cost of fill rate).", DUST_PERF_MEDIUM },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;
    memset(desc, 0, sizeof(*desc));

    desc->apiVersion       = DUST_API_VERSION;
    desc->name             = "POM";
    // Injection point is required by the framework but POM itself runs inside
    // the GBuffer pass via shader patching, not at any post-process slot. We
    // pick POST_GBUFFER as a no-op anchor; preExecute/postExecute are unused.
    desc->injectionPoint   = DUST_INJECT_POST_GBUFFER;
    desc->Init             = POMInit;
    desc->Shutdown         = POMShutdown;
    desc->IsEnabled        = POMIsEnabled;
    desc->settings         = gSettings;
    desc->settingCount     = sizeof(gSettings) / sizeof(gSettings[0]);
    desc->OnSettingChanged = POMOnSettingChanged;
    desc->flags            = DUST_FLAG_FRAMEWORK_CONFIG;
    desc->configSection    = "POM";
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);
    return TRUE;
}
