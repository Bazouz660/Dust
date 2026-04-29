// DustShadows.cpp - Shadow filtering settings plugin for Dust (API v3)
// Manages runtime parameters for the improved RTWSM shadow filtering
// injected by PatchDeferredShader. Binds a constant buffer at b2 that
// the patched deferred shader reads for filter radius, light size, etc.

#include "../../src/DustAPI.h"
#include "DustLog.h"

#include <d3d11.h>
#include <cstring>

DustLogFn gLogFn = nullptr;

struct ShadowConfig {
    bool  enabled           = true;
    float filterRadius      = 1.0f;
    float lightSize         = 3.0f;
    bool  pcssEnabled       = true;
    float biasScale         = 1.0f;
    bool  cliffFix          = false;  // off by default: previous always-on caused
                                      // close-range vertical shadows to disappear
    float cliffFixDistance  = 0.10f;  // fraction of shadow range where the bias
                                      // ramps in; smooth saturate curve, not a hard cutoff
};

static ShadowConfig gConfig;
static ID3D11Buffer* gCB = nullptr;

struct alignas(16) ShadowCBData {
    float enabled;
    float filterRadius;
    float lightSize;
    float pcssEnabled;
    float biasScale;
    float cliffFixEnabled;
    float cliffFixDistance;
    float pad0;
};

static int ShadowInit(ID3D11Device* device, uint32_t w, uint32_t h, const DustHostAPI* host)
{
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gCB = host->CreateConstantBuffer(device, sizeof(ShadowCBData));
    if (!gCB)
    {
        Log("Shadows: Failed to create constant buffer");
        return -1;
    }
    Log("Shadows: Initialized");
    return 0;
}

static void ShadowShutdown()
{
    if (gCB) { gCB->Release(); gCB = nullptr; }
    Log("Shadows: Shut down");
}

static void ShadowPreExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gCB) return;

    ShadowCBData data;
    data.enabled          = gConfig.enabled ? 1.0f : 0.0f;
    data.filterRadius     = gConfig.filterRadius * 0.001f;
    data.lightSize        = gConfig.lightSize * 0.001f;
    data.pcssEnabled      = gConfig.pcssEnabled ? 1.0f : 0.0f;
    data.biasScale        = gConfig.biasScale;
    data.cliffFixEnabled  = gConfig.cliffFix ? 1.0f : 0.0f;
    data.cliffFixDistance = gConfig.cliffFixDistance;
    data.pad0             = 0.0f;

    host->UpdateConstantBuffer(ctx->context, gCB, &data, sizeof(data));
    ctx->context->PSSetConstantBuffers(2, 1, &gCB);
}

static void ShadowPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    ID3D11Buffer* nullCB = nullptr;
    ctx->context->PSSetConstantBuffers(2, 1, &nullCB);
}

static int ShadowIsEnabled() { return 1; }

static DustSettingDesc gSettings[] = {
    { "Enabled",             DUST_SETTING_BOOL,  &gConfig.enabled,          0.0f, 1.0f,  "Enabled",          nullptr, "Enable or disable shadow filtering",                                                                                                                                          DUST_PERF_LOW    },
    { "Filter Radius",       DUST_SETTING_FLOAT, &gConfig.filterRadius,     0.1f, 5.0f,  "FilterRadius",     nullptr, "Size of the shadow softening filter",                                                                                                                                         DUST_PERF_NONE   },
    { "Light Size",          DUST_SETTING_FLOAT, &gConfig.lightSize,        0.5f, 10.0f, "LightSize",        nullptr, "Simulated light source size for contact-hardening shadows",                                                                                                                   DUST_PERF_NONE   },
    { "PCSS",                DUST_SETTING_BOOL,  &gConfig.pcssEnabled,      0.0f, 1.0f,  "PCSS",             nullptr, "Enable Percentage-Closer Soft Shadows for distance-based softness",                                                                                                           DUST_PERF_MEDIUM },
    { "Bias Scale",          DUST_SETTING_FLOAT, &gConfig.biasScale,        0.0f, 3.0f,  "BiasScale",        nullptr, "Shadow bias multiplier to reduce shadow acne artifacts",                                                                                                                      DUST_PERF_NONE },
    { "Cliff Shadow Fix",    DUST_SETTING_BOOL,  &gConfig.cliffFix,         0.0f, 1.0f,  "CliffFix",         nullptr, "Reduce shadow acne on steep cliffs and vertical faces (can make close-range vertical shadows fade out). Integration of Crunk Aint Dead's Cliff Face Shadow Fix mod.",      DUST_PERF_NONE },
    { "Cliff Fix Distance",  DUST_SETTING_FLOAT, &gConfig.cliffFixDistance, 0.0f, 1.0f,  "CliffFixDistance", nullptr, "Fraction of shadow range where the cliff fix smoothly ramps in (higher = preserves more close-range vertical shadows)",                                                     DUST_PERF_NONE },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;
    memset(desc, 0, sizeof(*desc));

    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "Shadows";
    desc->injectionPoint    = DUST_INJECT_POST_LIGHTING;
    desc->priority          = -10;
    desc->Init              = ShadowInit;
    desc->Shutdown          = ShadowShutdown;
    desc->preExecute        = ShadowPreExecute;
    desc->postExecute       = ShadowPostExecute;
    desc->IsEnabled         = ShadowIsEnabled;
    desc->settings          = gSettings;
    desc->settingCount      = sizeof(gSettings) / sizeof(gSettings[0]);
    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "Shadows";

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);
    return TRUE;
}
