// DustMaterials.cpp - Material / BRDF settings plugin for Dust
// Manages the experimental physically-based BRDF improvements injected into the
// deferred lighting shader (main_fs + light_fs) by ShaderPatch::PatchDeferredShader.
// Binds a constant buffer at b10 (DustBrdfParams) that those injected terms read.
//
// Split out of the Shadows plugin: BRDF used to ride in the Shadows b7 cbuffer;
// it now owns its own b10 cbuffer so the two features are independent.

#include "../../src/DustAPI.h"
#include "DustLog.h"

#include <d3d11.h>
#include <windows.h>
#include <cstring>

DustLogFn gLogFn = nullptr;

struct MaterialConfig {
    // === Material / BRDF (experimental) ===
    // Injected BRDF improvements in deferred.hlsl main_fs + light_fs. All default
    // OFF for in-game A/B; each runs only when brdfEnabled (master) is also on.
    bool  brdfEnabled        = false; // master gate for all four BRDF improvements
    bool  brdfDisneyDiffuse  = false; // swap cheap FresnelDiffuse for Burley/Disney diffuse
    bool  brdfMultiscatter   = false; // multiscatter GGX energy compensation on specular
    bool  brdfSpecOcclusion  = false; // Lagarde specular occlusion from AO on env specular
    bool  brdfSpecAA         = false; // geometric specular AA (roughness from normal variance)
    float brdfStrength       = 1.0f;  // A/B exaggeration multiplier (1=physical, higher=visible)
};

static MaterialConfig gConfig;
static ID3D11Buffer* gCB = nullptr;

// b10 cbuffer layout — must match the HLSL DustBrdfParams declaration injected by
// ShaderPatch::PatchDeferredShader. Floats pack tightly within 16-byte rows.
struct alignas(16) BrdfCBData {
    float brdfEnabled;
    float brdfDisneyDiffuse;
    float brdfMultiscatter;
    float brdfSpecOcclusion;
    float brdfSpecAA;
    float brdfStrength;
};

static int MaterialInit(ID3D11Device* device, uint32_t w, uint32_t h, const DustHostAPI* host)
{
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gCB = host->CreateConstantBuffer(device, sizeof(BrdfCBData));
    if (!gCB)
    {
        Log("Materials: Failed to create constant buffer");
        return -1;
    }
    Log("Materials: Initialized");
    return 0;
}

static void MaterialShutdown()
{
    if (gCB) { gCB->Release(); gCB = nullptr; }
    Log("Materials: Shut down");
}

static void MaterialPreExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gCB) return;

    BrdfCBData data;
    data.brdfEnabled       = gConfig.brdfEnabled ? 1.0f : 0.0f;
    data.brdfDisneyDiffuse = gConfig.brdfDisneyDiffuse ? 1.0f : 0.0f;
    data.brdfMultiscatter  = gConfig.brdfMultiscatter ? 1.0f : 0.0f;
    data.brdfSpecOcclusion = gConfig.brdfSpecOcclusion ? 1.0f : 0.0f;
    data.brdfSpecAA        = gConfig.brdfSpecAA ? 1.0f : 0.0f;
    data.brdfStrength      = gConfig.brdfStrength;

    host->UpdateConstantBuffer(ctx->context, gCB, &data, sizeof(data));
    // Bind to b10: b7 is the Shadows plugin, b9 the SSS plugin. b10 is a fresh
    // deferred-PS slot above Kenshi's own usage. See ShaderPatch.cpp.
    ctx->context->PSSetConstantBuffers(10, 1, &gCB);
}

static void MaterialPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    ID3D11Buffer* nullCB = nullptr;
    ctx->context->PSSetConstantBuffers(10, 1, &nullCB);
}

// Gate for binding the b10 cbuffer. When this returns 0 the framework skips
// preExecute (b10 stays unbound → all injected dust* flags read 0 → vanilla).
// Spec AA is decoupled from the BRDF master so it can be used as a standalone
// anti-aliasing option; the effect runs when either is enabled.
static int MaterialIsEnabled() { return (gConfig.brdfEnabled || gConfig.brdfSpecAA) ? 1 : 0; }

static DustSettingDesc gSettings[] = {
    // Injected into the deferred lighting shader (main_fs + light_fs). All default
    // OFF; the master "BRDF Improvements" toggle gates the other four for A/B testing.
    { "BRDF Improvements",   DUST_SETTING_BOOL,  &gConfig.brdfEnabled,      0.0f, 1.0f,  "BrdfEnabled",      nullptr, "Master switch for the experimental physically-based lighting improvements below. When off, the vanilla Kenshi BRDF is used. Each sub-option also has its own toggle.", DUST_PERF_LOW },
    { "Disney Diffuse",      DUST_SETTING_BOOL,  &gConfig.brdfDisneyDiffuse, 0.0f, 1.0f, "BrdfDisneyDiffuse", nullptr, "Replace the cheap constant diffuse Fresnel with the Burley/Disney diffuse term (view/light-angle-dependent retroreflection and edge darkening). Affects sun and point/spot diffuse.", DUST_PERF_LOW },
    { "Multiscatter Specular", DUST_SETTING_BOOL, &gConfig.brdfMultiscatter, 0.0f, 1.0f, "BrdfMultiscatter", nullptr, "Energy-compensate single-scatter GGX so rough metals don't lose energy (they appear too dark in vanilla). Boosts specular based on the single-scatter directional albedo.", DUST_PERF_LOW },
    { "Specular Occlusion",  DUST_SETTING_BOOL,  &gConfig.brdfSpecOcclusion, 0.0f, 1.0f, "BrdfSpecOcclusion", nullptr, "Apply Lagarde's specular occlusion so ambient specular is dimmed in crevices (vanilla AO only dims diffuse, so specular leaks into cavities).", DUST_PERF_LOW },
    { "Specular Anti-Aliasing", DUST_SETTING_BOOL, &gConfig.brdfSpecAA,     0.0f, 1.0f,  "BrdfSpecAA",       nullptr, "Geometric specular AA (Tokuyoshi/Kaplanyan): widens material roughness from the screen-space variance of the GBuffer normal, taming specular shimmer/sparkle on distant, shiny and normal-mapped surfaces when moving. Affects the sun, point/spot and environment-reflection specular. Works standalone (no need to enable the BRDF master).", DUST_PERF_LOW },
    { "BRDF Strength",       DUST_SETTING_FLOAT, &gConfig.brdfStrength,     0.0f, 10.0f, "BrdfStrength",     nullptr, "Exaggeration multiplier for the BRDF terms above. 1.0 = physically correct (subtle on most Kenshi materials). Crank to 5-8 to clearly SEE the effect for A/B, then dial back toward 1-2 for a natural look.", DUST_PERF_NONE },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;
    memset(desc, 0, sizeof(*desc));

    desc->apiVersion         = DUST_API_VERSION;
    desc->name               = "Materials";
    desc->injectionPoint     = DUST_INJECT_POST_LIGHTING;
    desc->priority           = -9;   // after Shadows (-10); distinct cbuffer regs, order irrelevant
    desc->Init               = MaterialInit;
    desc->Shutdown           = MaterialShutdown;
    desc->preExecute         = MaterialPreExecute;
    desc->postExecute        = MaterialPostExecute;
    desc->IsEnabled          = MaterialIsEnabled;
    desc->settings           = gSettings;
    desc->settingCount       = sizeof(gSettings) / sizeof(gSettings[0]);
    desc->flags              = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection      = "Materials";

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);
    return TRUE;
}
