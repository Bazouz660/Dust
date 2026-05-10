// DustTerrainTess.cpp - Terrain tessellation settings plugin for Dust.
// Owns the GUI / INI persistence for the tessellation runtime cbuffer.
// Pushes values to the host (TerrainTess) via DustHostAPI; the host owns
// the HS/DS shaders and the per-PS BLEND-mask routing for displacement =
// PS visible luminance.

#include "../../src/DustAPI.h"
#include "DustLog.h"

#include <d3d11.h>
#include <windows.h>
#include <cstring>

DustLogFn gLogFn = nullptr;

struct TerrainTessConfig
{
    bool  enabled          = true;

    // Tessellation factor + LOD fade.
    int   maxFactor        = 32;
    float factFadeStart    = 20.0f;
    float factFadeEnd      = 150.0f;
    int   factorSnapStep   = 4;

    // Displacement (bandpass + soft saturation).
    float amplitude        = 1.0f;
    float displacementBias = 0.0f;
    float sharpMip         = 1.0f;    // sharp tap mip; mid at +2, blurry at +4
    float scale            = 0.05f;   // saturation knee
    float hfWeight         = 0.5f;    // high-freq slice gain (2-point falloff curve)
    bool  ampFadeEnabled   = true;
    float ampFadeStart     = 80.0f;
    float ampFadeEnd       = 150.0f;
    float dispDirWorldUp   = 0.0f;

    // Debug.
    int   debugViewMode    = 0;       // 0=off, 1=PS visible lum, 2=DS-replica diff
    int   wireframe        = 0;       // 0=off, 1=tess wf, 2=vanilla wf, 3=IB-conv wf
};

static TerrainTessConfig gConfig;
static const DustHostAPI* gHost = nullptr;

// Per-frame GPU time read from the host. Pointer published in DustEffectDesc
// for the framework's perf display. Updated each frame by the preExecute
// hook (which the framework calls regardless of injection point).
static float sGpuTimeMs = 0.0f;

// Mirror TerrainTess::Controls layout: 15 floats matching the host's HLSL
// cbuffer field order. Booleans/ints converted to float here.
static void PushRuntime()
{
    if (!gHost || !gHost->SetTerrainTessControls) return;
    float buf[15] = {};
    buf[0]  = (float)gConfig.maxFactor;
    buf[1]  = gConfig.factFadeStart;
    buf[2]  = gConfig.factFadeEnd;
    buf[3]  = gConfig.amplitude;
    buf[4]  = gConfig.ampFadeStart;
    buf[5]  = gConfig.ampFadeEnd;
    buf[6]  = gConfig.ampFadeEnabled ? 1.0f : 0.0f;
    buf[7]  = (float)gConfig.debugViewMode;
    buf[8]  = gConfig.displacementBias;
    buf[9]  = (float)gConfig.factorSnapStep;
    buf[10] = gConfig.dispDirWorldUp;
    buf[11] = (float)gConfig.wireframe;
    buf[12] = gConfig.sharpMip;
    buf[13] = gConfig.scale;
    buf[14] = gConfig.hfWeight;
    gHost->SetTerrainTessControls(buf);
}

static void PushEnabled()
{
    if (!gHost || !gHost->SetTerrainTessEnabled) return;
    gHost->SetTerrainTessEnabled(gConfig.enabled ? 1 : 0);
}

static int TerrainTessInit(ID3D11Device* /*device*/, uint32_t /*w*/, uint32_t /*h*/, const DustHostAPI* host)
{
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gHost = host;
    PushEnabled();
    PushRuntime();
    Log("TerrainTess: initialized (enabled=%d, amplitude=%.2f, snap=%d)",
        (int)gConfig.enabled, gConfig.amplitude, gConfig.factorSnapStep);
    return 0;
}

static void TerrainTessShutdown()
{
    Log("TerrainTess: shut down");
}

static int TerrainTessIsEnabled() { return gConfig.enabled ? 1 : 0; }

static void TerrainTessPreExecute(const DustFrameContext* /*ctx*/, const DustHostAPI* host)
{
    if (host && host->GetTerrainTessGpuTimeMs)
        sGpuTimeMs = host->GetTerrainTessGpuTimeMs();
}

static void TerrainTessOnSettingChanged()
{
    PushEnabled();
    PushRuntime();
}

static DustSettingDesc gSettings[] = {
    { "Enabled",           DUST_SETTING_BOOL,  &gConfig.enabled,         0.0f,  1.0f,  "Enabled",         nullptr, "Master enable. When off, terrain renders flat with no HS/DS routing.", DUST_PERF_HIGH },

    // Tessellation
    { "Max Factor",        DUST_SETTING_INT,   &gConfig.maxFactor,       1.0f,  64.0f, "MaxFactor",       nullptr, "Maximum tess subdivision factor at near range. Higher = denser mesh, more displacement detail.", DUST_PERF_HIGH },
    { "Factor Fade Start", DUST_SETTING_FLOAT, &gConfig.factFadeStart,   0.0f,  500.0f,"FactFadeStart",   nullptr, "Distance (clip-space depth) at which tessellation begins fading out toward 1.", DUST_PERF_NONE },
    { "Factor Fade End",   DUST_SETTING_FLOAT, &gConfig.factFadeEnd,    10.0f,  500.0f,"FactFadeEnd",     nullptr, "Distance at which tessellation fully drops to factor 1 (no subdivision).", DUST_PERF_NONE },
    { "Factor Snap Step",  DUST_SETTING_INT,   &gConfig.factorSnapStep,  1.0f,  16.0f, "FactorSnapStep",  nullptr, "Quantize tess factor to multiples of this for stable LOD. Bigger = more stable, coarser steps.", DUST_PERF_NONE },

    // Displacement (bandpass + soft saturation)
    { "Amplitude",         DUST_SETTING_FLOAT, &gConfig.amplitude,       0.0f, 20.0f,  "Amplitude",       nullptr, "Final vertex displacement magnitude in world units. The shaped signal is bounded to ~±1, so output range is ~±amp.", DUST_PERF_NONE },
    { "Displacement Bias", DUST_SETTING_FLOAT, &gConfig.displacementBias,-0.5f, 0.5f,  "DisplacementBias",nullptr, "Pure additive output offset (independent of amp). Negative shifts terrain down, positive shifts it up.", DUST_PERF_NONE },
    { "Smoothness",        DUST_SETTING_FLOAT, &gConfig.sharpMip,        0.0f,  4.0f,  "SharpMip",        nullptr, "Mip level for the sharp bandpass tap (blurry tap is fixed at +4). Higher = blurrier sharp tap → fewer high-freq spikes, but also less fine detail.", DUST_PERF_NONE },
    { "Detail Scale",      DUST_SETTING_FLOAT, &gConfig.scale,           0.01f, 0.5f,  "Scale",           nullptr, "Saturation knee. Smaller = more equalized magnitude across textures (subtle and bumpy textures both produce similar displacement). Larger = more dynamic range preserved.", DUST_PERF_NONE },
    { "HF Weight",         DUST_SETTING_FLOAT, &gConfig.hfWeight,        0.0f,  1.0f,  "HfWeight",        nullptr, "High-frequency bump amplitude relative to mid-frequency. 1.0 = flat response (high-freq bumps full amplitude). 0 = mid-band only (high-freq bumps killed). Forms a 2-point frequency falloff curve.", DUST_PERF_NONE },
    { "Disp Dir World-Up", DUST_SETTING_FLOAT, &gConfig.dispDirWorldUp,  0.0f,  1.0f,  "DispDirWorldUp",  nullptr, "Blend between per-vertex normal (0) and world-up (1) for displacement direction. 1 fixes seams from boundary normal mismatches.", DUST_PERF_NONE },
    { "Amp Fade Enabled",  DUST_SETTING_BOOL,  &gConfig.ampFadeEnabled,  0.0f,  1.0f,  "AmpFadeEnabled",  nullptr, "Fade displacement amplitude with distance.", DUST_PERF_NONE },
    { "Amp Fade Start",    DUST_SETTING_FLOAT, &gConfig.ampFadeStart,    0.0f,  500.0f,"AmpFadeStart",    nullptr, "Distance at which amplitude begins fading toward zero.", DUST_PERF_NONE },
    { "Amp Fade End",      DUST_SETTING_FLOAT, &gConfig.ampFadeEnd,     10.0f,  500.0f,"AmpFadeEnd",      nullptr, "Distance at which amplitude reaches zero.", DUST_PERF_NONE },

    // Debug
    { "Debug: View Mode",  DUST_SETTING_INT,   &gConfig.debugViewMode,   0.0f,  2.0f,  "DebugViewMode",  nullptr, "0=off, 1=PS visible luminance (grayscale), 2=DS-replica diff overlay (bright = where the DS displacement formula diverges from the PS visible; black = pixel-exact match).", DUST_PERF_NONE },
    { "Debug: Wireframe",  DUST_SETTING_INT,   &gConfig.wireframe,       0.0f,  3.0f,  "Wireframe",      nullptr, "0=off, 1=tess wireframe, 2=vanilla mesh wireframe (no tess), 3=strip→list IB conversion + TRIANGLELIST + no tess (isolates IB conversion).", DUST_PERF_NONE },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;
    memset(desc, 0, sizeof(*desc));

    desc->apiVersion       = DUST_API_VERSION;
    desc->name             = "Terrain Tessellation";
    desc->injectionPoint   = DUST_INJECT_POST_GBUFFER;
    desc->Init             = TerrainTessInit;
    desc->Shutdown         = TerrainTessShutdown;
    desc->IsEnabled        = TerrainTessIsEnabled;
    desc->preExecute       = TerrainTessPreExecute;
    desc->settings         = gSettings;
    desc->settingCount     = sizeof(gSettings) / sizeof(gSettings[0]);
    desc->OnSettingChanged = TerrainTessOnSettingChanged;
    desc->flags            = DUST_FLAG_FRAMEWORK_CONFIG;
    desc->configSection    = "TerrainTess";
    desc->gpuTimeMsPtr     = &sGpuTimeMs;
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);
    return TRUE;
}
