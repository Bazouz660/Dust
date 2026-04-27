// DustShadowsRT.cpp — pixel-perfect ray-traced shadows for Kenshi (Phase 1-3)
//
// Phase 1: Deep Primitive Map construction + triangle count heatmap
// Phase 2: Möller-Trumbore ray test, binary shadow mask computed each frame
// Phase 3: Deferred shader integration — shadow mask read at s13/b4, replaces RTWShadow
//
// Injection point: POST_LIGHTING
//   PreExecute  (before the game's deferred draw): bind shadow mask SRV + CB so the
//               game's (patched) deferred PS reads it during the draw.
//   PostExecute (after the game's deferred draw):  unbind SRV (prevents UAV hazard),
//               build DPM, ray trace into a new shadow mask for the next frame.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "DPMRenderer.h"

#include <d3d11.h>
#include <cstring>
#include <string>

DustLogFn gLogFn = nullptr;
static const DustHostAPI* gHost = nullptr;
static HMODULE gPluginModule = nullptr;

// Phase 3: resources for binding the shadow mask into the deferred shader
static ID3D11Buffer*       gRTShadowCB      = nullptr; // b4: float dustRTShadowEnabled
static ID3D11SamplerState* gRTShadowSampler = nullptr; // s13: point-clamp sampler

struct alignas(16) RTShadowCBData
{
    float enabled;
    float pad[3];
};

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

// PRE: runs before the game's deferred draw.
// Extracts sun direction and binds the shadow mask into the deferred PS so it can
// sample it during the draw. Also disables the RT override if the mask isn't ready yet.
static void RTShadowsPreExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    auto& cfg = DPMRenderer::GetConfig();
    if (!cfg.enabled) return;

    // Extract sun direction while the deferred CB is still bound
    DPMRenderer::ExtractSunDirection(ctx->context);

    // Determine if we should override the deferred shadow this frame
    bool shouldApply = cfg.applyToScene
                    && DPMRenderer::IsShadowMaskReady()
                    && DPMRenderer::HasValidSunDirection();

    RTShadowCBData cbData = { shouldApply ? 1.0f : 0.0f, { 0.0f, 0.0f, 0.0f } };
    if (gRTShadowCB)
    {
        host->UpdateConstantBuffer(ctx->context, gRTShadowCB, &cbData, sizeof(cbData));
        ctx->context->PSSetConstantBuffers(4, 1, &gRTShadowCB);
    }

    if (shouldApply)
    {
        // Bind shadow mask SRV and sampler at slot 13 for the deferred PS.
        // The deferred PS (patched in D3D11Hook) samples dustShadowMask : register(s13).
        ID3D11ShaderResourceView* shadowSRV = DPMRenderer::GetShadowSRV();
        ID3D11SamplerState*       shadowSamp = DPMRenderer::GetShadowSampler();
        ctx->context->PSSetShaderResources(13, 1, &shadowSRV);
        ctx->context->PSSetSamplers(13, 1, &shadowSamp);
    }
}

// POST: runs after the game's deferred draw.
// Unbinds the shadow SRV from PS (prevents D3D11 SRV/UAV hazard when RayTrace writes to
// the same texture). Then rebuilds DPM + ray traces a new mask for next frame.
static void RTShadowsPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    auto& cfg = DPMRenderer::GetConfig();
    if (!cfg.enabled) return;
    if (!DPMRenderer::HasValidSunDirection()) return;

    // Unbind PS slot 13 before RayTrace writes to gShadowTex via UAV.
    // D3D11 debug layer flags simultaneous SRV (PS) + UAV (CS) bindings of the same texture.
    {
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ID3D11SamplerState*       nullSamp = nullptr;
        ctx->context->PSSetShaderResources(13, 1, &nullSRV);
        ctx->context->PSSetSamplers(13, 1, &nullSamp);
        // Also unbind b4 — it's no longer needed until next frame's PreExecute
        ID3D11Buffer* nullCB = nullptr;
        ctx->context->PSSetConstantBuffers(4, 1, &nullCB);
    }

    DPMRenderer::BuildDPM(ctx->context, ctx->device, &ctx->camera);

    ID3D11ShaderResourceView* depthSRV   = host->GetSRV(DUST_RESOURCE_DEPTH);
    ID3D11ShaderResourceView* normalsSRV = host->GetSRV(DUST_RESOURCE_NORMALS);
    DPMRenderer::RayTrace(ctx->context, depthSRV, normalsSRV,
                          &ctx->camera, ctx->width, ctx->height);

    // Debug overlays (replace scene with diagnostic view — non-destructive to shadow mask)
    if (cfg.debugView != 0)
    {
        ID3D11RenderTargetView* hdrRTV = host->GetRTV(DUST_RESOURCE_HDR_RT);
        if (!hdrRTV) return;
        if (cfg.debugView == 1)
            DPMRenderer::RenderDebugHeatmap(ctx->context, hdrRTV, ctx->width, ctx->height);
        else if (cfg.debugView == 2)
            DPMRenderer::RenderDebugShadowMask(ctx->context, hdrRTV, ctx->width, ctx->height);
    }
}

static int RTShadowsInit(ID3D11Device* device, uint32_t w, uint32_t h, const DustHostAPI* host)
{
    gHost = host;
#undef Log
    gLogFn = host->Log;
#define Log DustLog

    std::string dir = GetPluginDir();
    if (!DPMRenderer::Init(device, w, h, host, dir.c_str()))
    {
        Log("DustShadowsRT: DPMRenderer init failed");
        return -1;
    }

    // Phase 3 CB: dustRTShadowEnabled at b4
    gRTShadowCB = host->CreateConstantBuffer(device, sizeof(RTShadowCBData));
    if (!gRTShadowCB)
    {
        Log("DustShadowsRT: failed to create RT shadow CB");
        return -1;
    }

    // Point-clamp sampler for shadow mask sampling in the deferred PS
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    HRESULT hr = device->CreateSamplerState(&sd, &gRTShadowSampler);
    if (FAILED(hr))
    {
        Log("DustShadowsRT: failed to create shadow sampler (0x%08X)", hr);
        return -1;
    }

    Log("DustShadowsRT: ready (%ux%u, Phase 3)", w, h);
    return 0;
}

static void RTShadowsShutdown()
{
    if (gRTShadowCB)      { gRTShadowCB->Release();      gRTShadowCB = nullptr; }
    if (gRTShadowSampler) { gRTShadowSampler->Release();  gRTShadowSampler = nullptr; }
    DPMRenderer::Shutdown();
}

static void RTShadowsOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    DPMRenderer::OnResolutionChanged(device, w, h);
}

static int RTShadowsIsEnabled() { return DPMRenderer::GetConfig().enabled ? 1 : 0; }

static const char* kDebugLabels[] = { "Off", "Triangle Count Heatmap", "Shadow Mask", nullptr };

static DustSettingDesc gSettings[] = {
    { "Enabled",        DUST_SETTING_BOOL,  &DPMRenderer::GetConfig().enabled,
      0, 1, "Enabled", nullptr,
      "Master enable: build DPM + ray trace each frame" },
    { "Apply to Scene", DUST_SETTING_BOOL,  &DPMRenderer::GetConfig().applyToScene,
      0, 1, "ApplyToScene", nullptr,
      "Wire the RT shadow mask into the deferred lighting shader (Phase 3). "
      "Requires Enabled. Turn off to run DPM/ray trace in background for debugging." },
    { "Debug View",     DUST_SETTING_ENUM,  &DPMRenderer::GetConfig().debugView,
      0, 2, "DebugView", kDebugLabels,
      "Overlay DPM diagnostic on the scene. Does NOT affect the shadow mask." },
    { "DPM Resolution", DUST_SETTING_INT,   &DPMRenderer::GetConfig().dpmResolution,
      512, 2048, "DpmResolution", nullptr,
      "Shadow map resolution (square). 1024 = 128 MB indices. Restart required." },
    { "Frustum Extent", DUST_SETTING_FLOAT, &DPMRenderer::GetConfig().frustumExtent,
      20.0f, 500.0f, "FrustumExtent", nullptr,
      "Half-width of the sun ortho frustum around the camera (world units). "
      "Too small = shadows missing at edge of view; too large = lower texel density." },
    { "Frustum Depth",  DUST_SETTING_FLOAT, &DPMRenderer::GetConfig().frustumDepth,
      50.0f, 1000.0f, "FrustumDepth", nullptr,
      "Half-depth of the sun ortho frustum along the sun ray (world units). "
      "Must cover the tallest occluders above and below camera." },
    { "Tan Half FOV",   DUST_SETTING_HIDDEN_FLOAT, &DPMRenderer::GetConfig().tanHalfFov,
      0.1f, 2.0f, "TanHalfFov", nullptr,
      "tan(verticalFOV/2). Default 0.5218 matches Kenshi. Tweak if world-pos feels shifted." },

    // --- Diagnostics: live-updated by renderer, read-only in practice ---
    { "Diagnostics",      DUST_SETTING_SECTION, nullptr, 0, 0, nullptr, nullptr, nullptr },
    { "Sun X",            DUST_SETTING_FLOAT, &DPMRenderer::GetConfig().diagSunX,
      -1.0f, 1.0f, "DiagSunX", nullptr, "Live: world-space sun direction X" },
    { "Sun Y",            DUST_SETTING_FLOAT, &DPMRenderer::GetConfig().diagSunY,
      -1.0f, 1.0f, "DiagSunY", nullptr, "Live: world-space sun direction Y" },
    { "Sun Z",            DUST_SETTING_FLOAT, &DPMRenderer::GetConfig().diagSunZ,
      -1.0f, 1.0f, "DiagSunZ", nullptr, "Live: world-space sun direction Z" },
    { "Sun Valid",        DUST_SETTING_BOOL,  &DPMRenderer::GetConfig().diagSunValid,
      0, 1, "DiagSunValid", nullptr, "Live: 1 = sun direction extracted successfully" },
    { "Captured Draws",   DUST_SETTING_INT,   &DPMRenderer::GetConfig().diagCapturedDraws,
      0, 100000, "DiagCapturedDraws", nullptr, "Live: GBuffer draws captured this frame" },
    { "Replayed Draws",   DUST_SETTING_INT,   &DPMRenderer::GetConfig().diagReplayedDraws,
      0, 100000, "DiagReplayedDraws", nullptr,
      "Live: draws replayed into DPM. Less than captured = terrain/unknown skipped." },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;
    memset(desc, 0, sizeof(*desc));

    desc->apiVersion          = DUST_API_VERSION;
    desc->name                = "Shadows (RT)";
    desc->injectionPoint      = DUST_INJECT_POST_LIGHTING;
    desc->priority            = -20; // run before the existing PCF Shadows plugin
    desc->Init                = RTShadowsInit;
    desc->Shutdown            = RTShadowsShutdown;
    desc->OnResolutionChanged = RTShadowsOnResolutionChanged;
    desc->preExecute          = RTShadowsPreExecute;
    desc->postExecute         = RTShadowsPostExecute;
    desc->IsEnabled           = RTShadowsIsEnabled;
    desc->settings            = gSettings;
    desc->settingCount        = sizeof(gSettings) / sizeof(gSettings[0]);
    desc->flags               = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection       = "ShadowsRT";

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        gPluginModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
