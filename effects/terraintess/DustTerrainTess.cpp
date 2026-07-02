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
    int   maxFactor        = 16;
    int   factorSnapStep   = 6;

    // Tessellation distance thresholds in world units (Kenshi unit ~cm;
    // 30000 ≈ 300m), all camera-relative. Formerly derived from a single
    // tessRadius knob via fixed ratios; now each is exposed directly so the
    // factor-fade band, amp-fade band, DS LOD cutoffs, and CPU per-chunk
    // skip can be tuned independently. Defaults reproduce the old radius=30000
    // behavior. See "Debug: View Mode" 3 (radius rings) to see these in-world.
    float factFadeStart    = 900.0f;    // mesh density full <here, tapers to 1 by factFadeEnd
    float factFadeEnd      = 3000.0f;
    float ampFadeStart     = 900.0f;    // displacement full <here, tapers to 0 by ampFadeEnd
    float ampFadeEnd       = 3000.0f;
    float farHi            = 450.0f;    // DS drops HF detail bands past here
    float farMid           = 1500.0f;   // DS drops MF detail band past here
    float skipDistance     = 3000.0f;   // CPU bypasses HS/DS entirely past here (0 = never)

    // Displacement (bandpass + soft saturation). Kept as user-tunable
    // because they're quality knobs, not distance knobs.
    float amplitude        = 4.0f;
    float displacementBias = 0.0f;
    float sharpMip         = 0.0f;    // sharp tap mip; mid at +2, blurry at +4
    float scale            = 0.04f;   // saturation knee
    float hfWeight         = 1.0f;    // high-freq slice gain (2-point falloff curve)
    float spikeCap         = 4.0f;    // LF-aware spike cap on the (h_full − h_lf) excess; 0 = off
    float smoothHi         = 6.0f;    // per-slice mip offset for slice_hi tap pair (K..K+1)
    float smoothHiMid      = 6.0f;    // per-slice mip offset for slice_hm tap pair (K+1..K+2)
    float smoothMid        = 0.0f;    // per-slice mip offset for slice_mid tap pair (K+2..K+4)
    float smoothLo         = 2.0f;    // per-slice mip offset for slice_lo tap pair (K+4..K+8)
    bool  ampFadeEnabled   = true;
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

// Mirror TerrainTess::Controls layout: 23 floats matching the host's HLSL
// cbuffer field order. Booleans/ints converted to float here. The host
// memcpy's these directly into the start of Controls (1 pad float and 3
// mask float4s follow, keeping the layout 16-byte aligned).
//
// Distance fields are now exposed directly in the GUI (one slider each)
// rather than derived from a single tessRadius knob — the old ratio coupling
// made them hard to reason about. They map 1:1 onto the host Controls layout.
static void PushRuntime()
{
    if (!gHost || !gHost->SetTerrainTessControls) return;

    float buf[23] = {};
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
    buf[15] = gConfig.spikeCap;
    buf[16] = gConfig.smoothHi;
    buf[17] = gConfig.smoothHiMid;
    buf[18] = gConfig.smoothMid;
    buf[19] = gConfig.smoothLo;
    buf[20] = gConfig.farHi;
    buf[21] = gConfig.farMid;
    buf[22] = gConfig.skipDistance;
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

    // Tessellation distance thresholds (camera-relative, Kenshi units ~cm).
    // Each was formerly derived from one Tess Radius knob via fixed ratios;
    // now tunable independently. Use Debug View Mode 3 (radius rings) to see
    // exactly where each lands on the terrain.
    { "Factor Fade Start", DUST_SETTING_FLOAT, &gConfig.factFadeStart,   0.0f, 3000.0f, "FactFadeStart", nullptr, "Distance where mesh density starts tapering. Closer than this, tess factor is full (Max Factor). RING: green.", DUST_PERF_HIGH },
    { "Factor Fade End",   DUST_SETTING_FLOAT, &gConfig.factFadeEnd,     0.0f, 3000.0f, "FactFadeEnd",   nullptr, "Distance where mesh density reaches factor 1 (no subdivision). Past this the mesh is undisplaced unless still inside the amp fade. RING: magenta.", DUST_PERF_HIGH },
    { "Amp Fade Start",    DUST_SETTING_FLOAT, &gConfig.ampFadeStart,    0.0f, 3000.0f, "AmpFadeStart",  nullptr, "Distance where displacement amplitude starts tapering. Closer than this, displacement is full (Amplitude). Requires Amp Fade Enabled. RING: cyan.", DUST_PERF_NONE },
    { "Amp Fade End",      DUST_SETTING_FLOAT, &gConfig.ampFadeEnd,      0.0f, 3000.0f, "AmpFadeEnd",    nullptr, "Distance where displacement amplitude reaches 0. Past this, vertices are flat. The DS early-outs here, saving texture samples. RING: red.", DUST_PERF_HIGH },
    { "Far Hi (HF cutoff)",DUST_SETTING_FLOAT, &gConfig.farHi,           0.0f, 3000.0f, "FarHi",         nullptr, "Past this distance the DS drops the two high-frequency detail bands (finest rocks) — they aren't visible at range anyway. Lower = fewer texture samples far out = faster. RING: yellow.", DUST_PERF_HIGH },
    { "Far Mid (MF cutoff)",DUST_SETTING_FLOAT,&gConfig.farMid,          0.0f, 3000.0f, "FarMid",        nullptr, "Past this distance the DS additionally drops the mid-frequency band, leaving only the low-frequency dune surface. Lower = faster. Should be >= Far Hi. RING: orange.", DUST_PERF_HIGH },
    { "Skip Distance",     DUST_SETTING_FLOAT, &gConfig.skipDistance,    0.0f, 3000.0f, "SkipDistance",  nullptr, "Past this distance the CPU bypasses the HS/DS pipeline entirely (chunk renders flat via the original draw) — recovers fixed tess-pipeline overhead per distant chunk. 0 = never skip. RING: white.", DUST_PERF_HIGH },

    { "Factor Snap Step",  DUST_SETTING_INT,   &gConfig.factorSnapStep,  1.0f,  16.0f, "FactorSnapStep",  nullptr, "Quantize tess factor to multiples of this for stable LOD. Bigger = more stable, coarser steps.", DUST_PERF_NONE },

    // Displacement (bandpass + soft saturation)
    { "Amplitude",         DUST_SETTING_FLOAT, &gConfig.amplitude,       0.0f, 20.0f,  "Amplitude",       nullptr, "Final vertex displacement magnitude in world units. The shaped signal is bounded to ~±1, so output range is ~±amp.", DUST_PERF_NONE },
    { "Displacement Bias", DUST_SETTING_FLOAT, &gConfig.displacementBias,-0.5f, 0.5f,  "DisplacementBias",nullptr, "Pure additive output offset (independent of amp). Negative shifts terrain down, positive shifts it up.", DUST_PERF_NONE },
    { "Smoothness",        DUST_SETTING_FLOAT, &gConfig.sharpMip,        0.0f,  4.0f,  "SharpMip",        nullptr, "Mip level for the sharp bandpass tap (blurry tap is fixed at +4). Higher = blurrier sharp tap → fewer high-freq spikes, but also less fine detail.", DUST_PERF_NONE },
    { "Detail Scale",      DUST_SETTING_FLOAT, &gConfig.scale,           0.01f, 0.5f,  "Scale",           nullptr, "Saturation knee. Smaller = more equalized magnitude across textures (subtle and bumpy textures both produce similar displacement). Larger = more dynamic range preserved.", DUST_PERF_NONE },
    { "HF Weight",         DUST_SETTING_FLOAT, &gConfig.hfWeight,        0.0f,  1.0f,  "HfWeight",        nullptr, "High-frequency bump amplitude relative to mid-frequency. 1.0 = flat response (high-freq bumps full amplitude). 0 = mid-band only (high-freq bumps killed). Forms a 2-point frequency falloff curve.", DUST_PERF_NONE },
    { "Spike Cap",         DUST_SETTING_FLOAT, &gConfig.spikeCap,        0.0f,  50.0f, "SpikeCap",        nullptr, "LF-aware spike cap. DS computes h with the full pipeline AND with slice_lo only (broad LF surface). Soft-caps the difference (HF/MF excess sitting above the LF surface). Pure-LF features (dunes) have h_full ≈ h_lf → no excess → no cap. HF features (rocks) have a large excess → capped. 0 = off.", DUST_PERF_NONE },
    { "Smooth Hi",         DUST_SETTING_FLOAT, &gConfig.smoothHi,        0.0f,  6.0f,  "SmoothHi",        nullptr, "Per-slice mip offset for the slice_hi tap pair. 0 = standard band at gSharpMip..+1 (finest detail). Raising it blurs the sharpest band only.", DUST_PERF_LOW },
    { "Smooth Hi-Mid",     DUST_SETTING_FLOAT, &gConfig.smoothHiMid,     0.0f,  6.0f,  "SmoothHiMid",     nullptr, "Per-slice mip offset for the slice_hm tap pair. 0 = standard band at gSharpMip+1..+2 (in-between hi and mid). Raising it blurs this intermediate band only.", DUST_PERF_LOW },
    { "Smooth Mid",        DUST_SETTING_FLOAT, &gConfig.smoothMid,       0.0f,  6.0f,  "SmoothMid",       nullptr, "Per-slice mip offset for the slice_mid tap pair. 0 = standard band at gSharpMip+2..+4. Raising it blurs the mid-freq band only.", DUST_PERF_LOW },
    { "Smooth Lo",         DUST_SETTING_FLOAT, &gConfig.smoothLo,        0.0f,  6.0f,  "SmoothLo",        nullptr, "Per-slice mip offset for the slice_lo tap pair. 0 = standard band at gSharpMip+4..+8. Raising it blurs the low-freq band only (dune-scale features get smoother).", DUST_PERF_LOW },
    { "Disp Dir World-Up", DUST_SETTING_FLOAT, &gConfig.dispDirWorldUp,  0.0f,  1.0f,  "DispDirWorldUp",  nullptr, "Blend between per-vertex normal (0) and world-up (1) for displacement direction. 1 fixes seams from boundary normal mismatches.", DUST_PERF_NONE },
    { "Amp Fade Enabled",  DUST_SETTING_BOOL,  &gConfig.ampFadeEnabled,  0.0f,  1.0f,  "AmpFadeEnabled",  nullptr, "Fade displacement amplitude with distance. Disabling makes displacement uniform across the tess radius (the fade band still affects factor but amp stays full).", DUST_PERF_NONE },

    // Debug
    { "Debug: View Mode",  DUST_SETTING_INT,   &gConfig.debugViewMode,   0.0f,  3.0f,  "DebugViewMode",  nullptr, "0=off, 1=PS visible luminance (grayscale), 2=DS-replica diff overlay, 3=radius rings: color-coded contour rings on the terrain at each distance threshold over dimmed terrain. Legend: green=Factor Fade Start, magenta=Factor Fade End, cyan=Amp Fade Start, red=Amp Fade End, yellow=Far Hi, orange=Far Mid, white=Skip Distance.", DUST_PERF_NONE },
    { "Debug: Wireframe",  DUST_SETTING_INT,   &gConfig.wireframe,       0.0f,  4.0f,  "Wireframe",      nullptr, "0=off, 1=tess wireframe, 2=vanilla mesh wireframe (no tess), 3=strip→list IB conversion + TRIANGLELIST + no tess (isolates IB conversion), 4=DIAG: force-skip tess on every terrain draw (no HS/DS — for perf bisection).", DUST_PERF_NONE },
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
