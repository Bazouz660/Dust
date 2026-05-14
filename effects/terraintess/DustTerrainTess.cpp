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

    // Tessellation radius in world units (Kenshi unit ~cm; 30000 ≈ 300m).
    // Single user-facing knob that drives every distance threshold:
    //   - factor fade band:  radius * 0.3  -> radius
    //   - amp fade band:     radius * 0.3  -> radius
    //   - DS HF tap cutoff:  radius * 0.15 (far-hi)
    //   - DS mid tap cutoff: radius * 0.5  (far-mid)
    //   - CPU per-chunk skip past:  radius * 1.05
    // PushRuntime() derives all of these and ships them to the host. The
    // user only ever touches Tess Radius.
    float tessRadius       = 30000.0f;

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
// Distance fields (factFadeStart, ampFadeStart, farHi, farMid, skipDistance)
// are derived from a single user-facing `tessRadius` here — never exposed
// directly in the GUI. The ratios pick a sensible band where chunks fade
// to factor=1 by the radius and CPU-side skip kicks in just past it.
static void PushRuntime()
{
    if (!gHost || !gHost->SetTerrainTessControls) return;

    const float r = gConfig.tessRadius;
    const float factFadeStart = r * 0.3f;
    const float factFadeEnd   = r;
    const float ampFadeStart  = r * 0.3f;
    const float ampFadeEnd    = r;
    const float farHi         = r * 0.15f;
    const float farMid        = r * 0.5f;
    const float skipDistance  = r * 1.05f;

    float buf[23] = {};
    buf[0]  = (float)gConfig.maxFactor;
    buf[1]  = factFadeStart;
    buf[2]  = factFadeEnd;
    buf[3]  = gConfig.amplitude;
    buf[4]  = ampFadeStart;
    buf[5]  = ampFadeEnd;
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
    buf[20] = farHi;
    buf[21] = farMid;
    buf[22] = skipDistance;
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
    { "Tess Radius",       DUST_SETTING_FLOAT, &gConfig.tessRadius,      0.0f, 200000.0f, "TessRadius",  nullptr, "World-space radius (camera-relative, in Kenshi units ~cm) of the tessellated zone. Past this, terrain is rendered without tessellation entirely. Drives all distance thresholds — factor fade, amp fade, DS LOD cutoffs, and CPU per-chunk skip — via fixed ratios. Lower = bigger fps win, smaller visible-displacement zone. HIGH perf impact.", DUST_PERF_HIGH },
    { "Max Factor",        DUST_SETTING_INT,   &gConfig.maxFactor,       1.0f,  64.0f, "MaxFactor",       nullptr, "Maximum tess subdivision factor at near range. Higher = denser mesh, more displacement detail.", DUST_PERF_HIGH },
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
    { "Debug: View Mode",  DUST_SETTING_INT,   &gConfig.debugViewMode,   0.0f,  3.0f,  "DebugViewMode",  nullptr, "0=off, 1=PS visible luminance (grayscale), 2=DS-replica diff overlay, 3=chunk-distance heatmap (green=close/kept by CPU-skip, red=far/would-be-skipped — mode 3 disables actual skipping so every chunk shows its color).", DUST_PERF_NONE },
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
