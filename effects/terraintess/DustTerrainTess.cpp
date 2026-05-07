// DustTerrainTess.cpp - Terrain tessellation settings plugin for Dust (API v7).
// Owns the GUI / INI persistence for the tessellation runtime cbuffer and the
// bake-time params. Pushes values to the host (TerrainTess) via DustHostAPI v7
// functions; the host owns the HS/DS shaders, the GPU FFT bake pipeline, and
// the heightArray cache. Bake-time param changes auto-trigger a rebake.

#include "../../src/DustAPI.h"
#include "DustLog.h"

#include <d3d11.h>
#include <windows.h>
#include <cstring>

DustLogFn gLogFn = nullptr;

struct TerrainTessConfig
{
    // Master enable
    bool  enabled          = true;

    // Tessellation factor + LOD fade
    int   maxFactor        = 64;     // integer subdivision count
    float factFadeStart    = 20.0f;
    float factFadeEnd      = 200.0f;
    int   factorSnapStep   = 4;      // factor quantum (integer)

    // Displacement
    float amplitude        = 1.0f;
    float displacementBias = 0.3f;
    bool  ampFadeEnabled   = false;
    float ampFadeStart     = 50.0f;
    float ampFadeEnd       = 200.0f;
    float mipBias          = 0.0f;
    float dispDirWorldUp   = 0.0f;
    float uvTileFactor     = 250.0f;

    // Bake-time (require rebake to apply)
    int   bakeHpCutoff     = 0;      // cutoff in cycles/image (integer)
    float bakeHpStrength   = 1.0f;
    float bakeLumMix       = 0.5f;

    // Debug
    bool  debugViewMode    = false;
    int   debugSlice       = -1;
};

static TerrainTessConfig gConfig;
static const DustHostAPI* gHost = nullptr;

// Snapshot of the bake-time values from the last push, so OnSettingChanged
// can detect when one of them moved and trigger a rebake automatically.
static int   sLastBakeHpCutoff   = 0;
static float sLastBakeHpStrength = 1.0f;
static float sLastBakeLumMix     = 0.5f;

// Per-frame GPU time read from the host. Pointer published in DustEffectDesc
// for the framework's perf display. Updated each frame by the preExecute
// hook (which the framework calls regardless of injection point).
static float sGpuTimeMs = 0.0f;

// Mirror TerrainTess::Controls layout: 16 floats, in the order the host's
// HLSL cbuffer reads them. Booleans/ints are converted to float here.
static void PushRuntime()
{
    if (!gHost || !gHost->SetTerrainTessControls) return;
    float buf[16] = {};
    buf[0]  = (float)gConfig.maxFactor;
    buf[1]  = gConfig.factFadeStart;
    buf[2]  = gConfig.factFadeEnd;
    buf[3]  = gConfig.amplitude;
    buf[4]  = gConfig.ampFadeStart;
    buf[5]  = gConfig.ampFadeEnd;
    buf[6]  = gConfig.ampFadeEnabled ? 1.0f : 0.0f;
    buf[7]  = gConfig.uvTileFactor;
    buf[8]  = (float)gConfig.debugSlice;
    buf[9]  = gConfig.debugViewMode ? 1.0f : 0.0f;
    buf[10] = gConfig.displacementBias;
    buf[11] = (float)gConfig.factorSnapStep;
    buf[12] = gConfig.dispDirWorldUp;
    buf[13] = gConfig.mipBias;
    // buf[14], buf[15] are pad — left zero.
    gHost->SetTerrainTessControls(buf);
}

static void PushBake()
{
    if (!gHost) return;
    if (gHost->SetTerrainTessBakeHighPass)
        gHost->SetTerrainTessBakeHighPass((float)gConfig.bakeHpCutoff, gConfig.bakeHpStrength);
    if (gHost->SetTerrainTessBakeLumMix)
        gHost->SetTerrainTessBakeLumMix(gConfig.bakeLumMix);
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
    PushBake();
    sLastBakeHpCutoff   = gConfig.bakeHpCutoff;
    sLastBakeHpStrength = gConfig.bakeHpStrength;
    sLastBakeLumMix     = gConfig.bakeLumMix;
    Log("TerrainTess: initialized (enabled=%d, amplitude=%.2f, lumMix=%.2f, snap=%d)",
        (int)gConfig.enabled, gConfig.amplitude, gConfig.bakeLumMix, gConfig.factorSnapStep);
    return 0;
}

static void TerrainTessShutdown()
{
    Log("TerrainTess: shut down");
}

static int TerrainTessIsEnabled() { return gConfig.enabled ? 1 : 0; }

// Per-frame: pull the host's measured GPU time into our local float so the
// framework's perf display picks it up via desc->gpuTimeMsPtr.
static void TerrainTessPreExecute(const DustFrameContext* /*ctx*/, const DustHostAPI* host)
{
    if (host && host->GetTerrainTessGpuTimeMs)
        sGpuTimeMs = host->GetTerrainTessGpuTimeMs();
}

static void TerrainTessOnSettingChanged()
{
    // Enabled toggle pushes immediately so the host can short-circuit
    // tessellation on the next draw.
    PushEnabled();

    // Runtime values are cheap to push every change.
    PushRuntime();

    // Bake-time params trigger a rebake when any of them moves. We only push
    // + rebake on actual change (compared against last-pushed values), so
    // wiggling unrelated sliders doesn't kick off a rebake.
    bool bakeChanged = (gConfig.bakeHpCutoff   != sLastBakeHpCutoff)
                    || (gConfig.bakeHpStrength != sLastBakeHpStrength)
                    || (gConfig.bakeLumMix     != sLastBakeLumMix);
    if (bakeChanged)
    {
        PushBake();
        if (gHost && gHost->RebakeTerrainHeightmaps)
            gHost->RebakeTerrainHeightmaps();
        sLastBakeHpCutoff   = gConfig.bakeHpCutoff;
        sLastBakeHpStrength = gConfig.bakeHpStrength;
        sLastBakeLumMix     = gConfig.bakeLumMix;
    }
}

static DustSettingDesc gSettings[] = {
    // Master
    { "Enabled",           DUST_SETTING_BOOL,  &gConfig.enabled,         0.0f,  1.0f,  "Enabled",         nullptr, "Master enable. When off, terrain renders flat with no HS/DS routing or heightmap bake.", DUST_PERF_HIGH },

    // Tessellation
    { "Max Factor",        DUST_SETTING_INT,   &gConfig.maxFactor,       1.0f,  64.0f, "MaxFactor",       nullptr, "Maximum tess subdivision factor at near range. Higher = denser mesh, more displacement detail.", DUST_PERF_HIGH },
    { "Factor Fade Start", DUST_SETTING_FLOAT, &gConfig.factFadeStart,   0.0f,  500.0f,"FactFadeStart",   nullptr, "Distance (clip-space depth) at which tessellation begins fading out toward 1.", DUST_PERF_NONE },
    { "Factor Fade End",   DUST_SETTING_FLOAT, &gConfig.factFadeEnd,    10.0f,  500.0f,"FactFadeEnd",     nullptr, "Distance at which tessellation fully drops to factor 1 (no subdivision).", DUST_PERF_NONE },
    { "Factor Snap Step",  DUST_SETTING_INT,   &gConfig.factorSnapStep,  1.0f,  16.0f, "FactorSnapStep",  nullptr, "Quantize tess factor to multiples of this for stable LOD as the camera moves. Bigger = more stable, coarser steps.", DUST_PERF_NONE },

    // Displacement
    { "Amplitude",         DUST_SETTING_FLOAT, &gConfig.amplitude,       0.0f,  5.0f,  "Amplitude",       nullptr, "Vertex displacement magnitude in world units (signed around the bias point).", DUST_PERF_NONE },
    { "Displacement Bias", DUST_SETTING_FLOAT, &gConfig.displacementBias,0.0f,  0.5f,  "DisplacementBias",nullptr, "Mid-point of the heightmap that maps to ground level. Lower = displacement biased upward, fewer pixels sink.", DUST_PERF_NONE },
    { "Disp Dir World-Up", DUST_SETTING_FLOAT, &gConfig.dispDirWorldUp,  0.0f,  1.0f,  "DispDirWorldUp",  nullptr, "Blend between per-vertex normal (0) and world-up (1) for displacement direction. 1 fixes seams between chunks with mismatched boundary normals.", DUST_PERF_NONE },
    { "Mip Bias",          DUST_SETTING_FLOAT, &gConfig.mipBias,         0.0f,  6.0f,  "MipBias",         nullptr, "Extra blur on the heightmap sampling. Higher = smoother displacement, less spike potential at low tess density.", DUST_PERF_NONE },
    { "Amp Fade Enabled",  DUST_SETTING_BOOL,  &gConfig.ampFadeEnabled,  0.0f,  1.0f,  "AmpFadeEnabled",  nullptr, "Fade displacement amplitude with distance.", DUST_PERF_NONE },
    { "Amp Fade Start",    DUST_SETTING_FLOAT, &gConfig.ampFadeStart,    0.0f,  500.0f,"AmpFadeStart",    nullptr, "Distance at which amplitude begins fading toward zero.", DUST_PERF_NONE },
    { "Amp Fade End",      DUST_SETTING_FLOAT, &gConfig.ampFadeEnd,     10.0f,  500.0f,"AmpFadeEnd",      nullptr, "Distance at which amplitude reaches zero.", DUST_PERF_NONE },

    // Bake-time (auto-rebake on change)
    { "Bake: Lum Mix",         DUST_SETTING_FLOAT, &gConfig.bakeLumMix,     0.0f,  1.0f,  "BakeLumMix",     nullptr, "Bake-time blend between FFT-from-normal (0) and luminance-as-height (1). 0.5 default works well across rocky and dune textures. Auto-rebakes on change.", DUST_PERF_LOW },
    { "Bake: HP Cutoff",       DUST_SETTING_INT,   &gConfig.bakeHpCutoff,   0.0f, 64.0f,  "BakeHpCutoff",   nullptr, "Bake-time Gaussian high-pass sigma in cycles/image. 0 disables. Lower = suppress wider macro features. Auto-rebakes on change.", DUST_PERF_LOW },
    { "Bake: HP Strength",     DUST_SETTING_FLOAT, &gConfig.bakeHpStrength, 0.0f,  1.0f,  "BakeHpStrength", nullptr, "How aggressively low-frequencies are attenuated. Auto-rebakes on change.", DUST_PERF_LOW },

    // Debug
    { "Debug: View Heightmap", DUST_SETTING_BOOL,  &gConfig.debugViewMode,  0.0f,  1.0f,  "DebugViewMode",  nullptr, "Override terrain albedo with the blended heightmap so you can see what's being applied.", DUST_PERF_NONE },
    { "Debug: Force Slice",    DUST_SETTING_INT,   &gConfig.debugSlice,    -1.0f,  9.0f,  "DebugSlice",     nullptr, "-1 = full PS-style blend (real). 0..5 = force a single slice (0 base, 1 slope, 2 cliff, 3 grass, 4 dirt, 5 road). 6..9 = show blend weights.", DUST_PERF_NONE },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;
    memset(desc, 0, sizeof(*desc));

    desc->apiVersion       = DUST_API_VERSION;
    desc->name             = "Terrain Tessellation";
    // No post-process slot — terrain tess runs inside the GBuffer pass via
    // host-side HS/DS injection. POST_GBUFFER is a no-op anchor.
    desc->injectionPoint   = DUST_INJECT_POST_GBUFFER;
    desc->Init             = TerrainTessInit;
    desc->Shutdown         = TerrainTessShutdown;
    desc->IsEnabled        = TerrainTessIsEnabled;
    desc->preExecute       = TerrainTessPreExecute;  // pulls per-frame GPU time
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
