// DustShadows.cpp - Shadow filtering settings plugin for Dust (API v3)
// Manages runtime parameters for the improved RTWSM shadow filtering
// injected by PatchDeferredShader. Binds a constant buffer at b2 that
// the patched deferred shader reads for filter radius, light size, etc.

#include "../../src/DustAPI.h"
#include "DustLog.h"

#include <d3d11.h>
#include <windows.h>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

DustLogFn gLogFn = nullptr;

struct ShadowConfig {
    // === Shared ===
    bool  enabled           = true;
    int   resolutionIndex   = 7;      // index into kShadowResolutions; default =
                                      // "Vanilla" (0 = no override, zero overhead)
    int   shadowRange       = 6000;   // overridden in DustEffectCreate from settings.cfg
                                      // if a value is already present there

    // === RTWSM ===
    float filterRadius      = 1.0f;   // RTW UV-space filter radius (scaled by 0.001 * resScale)
    float lightSize         = 3.0f;   // RTW PCSS light size
    bool  pcssEnabled       = true;   // RTW PCSS toggle
    float biasScale         = 1.0f;
    float normalBias        = 1.5f;   // world-space offset along surface normal before
                                      // shadow lookup; prevents self-shadowing (acne)
    float slopeBias         = 1.0f;   // extra depth bias on surfaces at grazing angles
    bool  cliffFix          = false;  // off by default: previous always-on caused
                                      // close-range vertical shadows to disappear
    float cliffFixDistance  = 0.10f;  // fraction of shadow range where the bias
                                      // ramps in; smooth saturate curve, not a hard cutoff

    // === CSM ===
    float pssmLambda        = 0.95f;  // PSSM cascade split distribution. At exactly
                                      // 0.95 Kenshi's native splits are kept (they
                                      // don't fit the PSSM formula at any lambda);
                                      // any other value activates the override.
    float csmFilterRadius   = 1.0f;   // global scale on the per-cascade PCF radius
    float csmLightSize      = 2.0f;   // PCSS blocker-search/penumbra scale
    bool  csmPcssEnabled    = true;
    bool  csmBlendEnabled   = true;   // smooth blend between adjacent cascades
    float csmBlendWidth     = 0.15f;  // fraction of cascade depth range used as blend band
};

static ShadowConfig gConfig;
static ID3D11Buffer* gCB = nullptr;
static const DustHostAPI* gHost = nullptr;
static bool gWasEnabled = true;
static int gVanillaShadowRange = -1;       // settings.cfg value at boot
static int gLastWrittenShadowRange = INT_MIN;  // debounce slider-drag disk writes

// "Vanilla" (0) is appended, not prepended, so indices saved by older configs
// keep their meaning. 0 = no override: the game's own atlas is used untouched
// and the whole swap machinery stays dormant (zero per-frame overhead).
static const uint32_t kShadowResolutions[] = { 1024, 2048, 4096, 6144, 8192, 12288, 16384, 0 };
static const char* const kShadowResolutionLabels[] = {
    "1024", "2048", "4096", "6144", "8192", "12288", "16384", "Vanilla (no override)", nullptr
};

static uint32_t GetSelectedShadowResolution()
{
    int idx = gConfig.resolutionIndex;
    int n = (int)(sizeof(kShadowResolutions) / sizeof(kShadowResolutions[0]));
    if (idx < 0 || idx >= n) idx = n - 1;
    return kShadowResolutions[idx];
}

// Atlas size actually in effect — for texel-size compensation when the
// override is off ("Vanilla").
static uint32_t GetEffectiveShadowResolution()
{
    uint32_t sel = GetSelectedShadowResolution();
    if (sel == 0 && gHost && gHost->GetShadowBaseResolution)
        sel = gHost->GetShadowBaseResolution();
    if (sel == 0)
        sel = 4096;
    return sel;
}

static void ApplyDustShadows();

// ==================== settings.cfg I/O ====================
// settings.cfg lives in the Kenshi game root directory. It's a flat
// "key=value" file with no [section] header, so WritePrivateProfileString
// can't be used — we manually read, splice the line, and write back.

static std::string GetGameDir()
{
    // Use the running process EXE path. Walking up from the DLL doesn't work
    // for Workshop installs — the DLL lives under steamapps/workshop/content/.
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return "";
    std::string s(path);
    size_t pos = s.find_last_of("\\/");
    if (pos == std::string::npos) return "";
    s.resize(pos);

    // RE_Kenshi launches Kenshi from <game>\RE_Kenshi\Kenshi_x64.exe, so the
    // running EXE's directory is <game>\RE_Kenshi — settings.cfg lives one
    // level up. Detect by directory basename rather than probing for the file
    // (we may already have polluted this dir with a stray settings.cfg from
    // an earlier broken-path run).
    size_t lastSep = s.find_last_of("\\/");
    if (lastSep != std::string::npos)
    {
        std::string base = s.substr(lastSep + 1);
        if (_stricmp(base.c_str(), "RE_Kenshi") == 0)
            s.resize(lastSep);
    }
    return s;
}

static std::string GetSettingsCfgPath()
{
    std::string dir = GetGameDir();
    if (dir.empty()) return "";
    return dir + "\\settings.cfg";
}

// Locate this DLL's directory and the Dust install root. The plugin DLL is at
// <install>/effects/DustShadows.dll, so the install root is one dir up. Used
// to peek at our INI and the auto-load preset before the framework loads them.
static std::string GetThisDllPath()
{
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&GetThisDllPath, &hMod)) return "";
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(hMod, path, MAX_PATH)) return "";
    return std::string(path);
}

static std::string GetDustInstallDir()
{
    std::string p = GetThisDllPath();
    if (p.empty()) return "";
    // <install>\effects\DustShadows.dll — strip filename, then one more level.
    size_t pos = p.find_last_of("\\/");
    if (pos == std::string::npos) return "";
    p.resize(pos);
    pos = p.find_last_of("\\/");
    if (pos == std::string::npos) return "";
    p.resize(pos);
    return p;
}

// Resolve the user's intended shadow range BEFORE the framework loads our INI.
// Priority: auto-load preset's Shadows.ini (if any) > per-effect Shadows.ini >
// fallback. This needs to fire from DustEffectCreate so the value can be
// written to settings.cfg before Kenshi reads it; Kenshi caches the value at
// startup and writes back on exit, so any later write is futile.
static int ResolveUserShadowRange(int fallback)
{
    std::string installDir = GetDustInstallDir();
    if (installDir.empty()) return fallback;

    int perEffect = GetPrivateProfileIntA("Shadows", "Range", -1,
        (installDir + "\\effects\\Shadows.ini").c_str());

    int presetVal = -1;
    char presetName[256] = {};
    GetPrivateProfileStringA("Dust", "LastPreset", "", presetName, sizeof(presetName),
        (installDir + "\\Dust.ini").c_str());
    if (presetName[0])
    {
        std::string presetIni = installDir + "\\presets\\" + presetName + "\\Shadows.ini";
        presetVal = GetPrivateProfileIntA("Shadows", "Range", -1, presetIni.c_str());
    }

    if (presetVal > 0) return presetVal;
    if (perEffect > 0) return perEffect;
    return fallback;
}

static std::string ReadFileContents(const std::string& path)
{
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string content;
    if (sz > 0)
    {
        content.resize(sz);
        size_t got = fread(&content[0], 1, sz, f);
        content.resize(got);
    }
    fclose(f);
    return content;
}

// Find "key=" anchored at start of a line (no leading whitespace tolerance —
// matches Kenshi's exact write format). Returns std::string::npos on miss.
static size_t FindKeyLine(const std::string& content, const std::string& key)
{
    std::string needle = key + "=";
    size_t pos = 0;
    while (pos < content.size())
    {
        if ((pos == 0 || content[pos - 1] == '\n' || content[pos - 1] == '\r') &&
            content.compare(pos, needle.size(), needle) == 0)
            return pos;
        size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return std::string::npos;
}

static int ReadSettingsCfgInt(const std::string& key, int defaultValue)
{
    std::string path = GetSettingsCfgPath();
    if (path.empty()) return defaultValue;
    std::string content = ReadFileContents(path);
    if (content.empty()) return defaultValue;

    size_t keyPos = FindKeyLine(content, key);
    if (keyPos == std::string::npos) return defaultValue;

    size_t valStart = keyPos + key.size() + 1; // past '='
    size_t valEnd = content.find_first_of("\r\n", valStart);
    if (valEnd == std::string::npos) valEnd = content.size();
    std::string val = content.substr(valStart, valEnd - valStart);
    return atoi(val.c_str());
}

static bool WriteSettingsCfgInt(const std::string& key, int value)
{
    std::string path = GetSettingsCfgPath();
    if (path.empty()) return false;
    std::string content = ReadFileContents(path);

    char valBuf[32];
    snprintf(valBuf, sizeof(valBuf), "%d", value);

    size_t keyPos = FindKeyLine(content, key);
    if (keyPos != std::string::npos)
    {
        size_t valStart = keyPos + key.size() + 1;
        size_t valEnd = content.find_first_of("\r\n", valStart);
        if (valEnd == std::string::npos) valEnd = content.size();
        content.replace(valStart, valEnd - valStart, valBuf);
    }
    else
    {
        if (!content.empty() && content.back() != '\n') content += "\n";
        content += key + "=" + valBuf + "\n";
    }

    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
    fclose(f);
    return ok;
}

// Write our current shadowRange to settings.cfg, deduping repeated writes.
static void PushShadowRangeToGame()
{
    if (gConfig.shadowRange == gLastWrittenShadowRange) return;
    if (WriteSettingsCfgInt("Shadow Range", gConfig.shadowRange))
    {
        gLastWrittenShadowRange = gConfig.shadowRange;
        Log("Shadows: wrote Shadow Range=%d to settings.cfg", gConfig.shadowRange);
    }
    else
    {
        Log("Shadows: failed to write Shadow Range to settings.cfg");
    }
}

// Slider drags fire OnSettingChanged on every mouse-move tick; a synchronous
// settings.cfg rewrite per tick is a visible frame hitch. Queue the write and
// flush from preExecute once the value has been stable for half a second.
// (The live range is pushed to the engine immediately either way — only the
// disk persistence is deferred.)
static int   gPendingShadowRangeWrite = INT_MIN;
static DWORD gShadowRangeChangeTick   = 0;

static void QueueShadowRangeWrite()
{
    if (gConfig.shadowRange == gLastWrittenShadowRange)
    {
        gPendingShadowRangeWrite = INT_MIN;
        return;
    }
    gPendingShadowRangeWrite = gConfig.shadowRange;
    gShadowRangeChangeTick   = GetTickCount();
}

static void FlushPendingShadowRangeWrite()
{
    if (gPendingShadowRangeWrite == INT_MIN) return;
    if (GetTickCount() - gShadowRangeChangeTick < 500) return;
    gPendingShadowRangeWrite = INT_MIN;
    PushShadowRangeToGame();
}

// Layout must match the DustShadowParams cbuffer injected by
// ShaderPatch::PatchDeferredShader (register b7).
struct alignas(16) ShadowCBData {
    float enabled;
    float rtwFilterRadius;
    float rtwLightSize;
    float rtwPcssEnabled;
    float rtwBiasScale;
    float rtwCliffFixEnabled;
    float rtwCliffFixDistance;
    float rtwNormalBias;
    float rtwSlopeBias;
    float csmFilterRadius;
    float csmLightSize;
    float csmPcssEnabled;
    float csmBlendEnabled;
    float csmBlendWidth;
    float rtwQuality;
    float pad0;
};

static int ShadowInit(ID3D11Device* device, uint32_t w, uint32_t h, const DustHostAPI* host)
{
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gHost = host;
    gCB = host->CreateConstantBuffer(device, sizeof(ShadowCBData));
    if (!gCB)
    {
        Log("Shadows: Failed to create constant buffer");
        return -1;
    }
    // Re-push the config-derived overrides (OnEarlyConfigApply already did,
    // but Init also runs on ReinitAll where the early hook doesn't fire).
    if (gConfig.enabled)
        ApplyDustShadows();
    gWasEnabled = gConfig.enabled;
    Log("Shadows: Initialized (atlas resolution = %u, cascade lambda = %.3f, shadow range = %d)",
        GetSelectedShadowResolution(), gConfig.pssmLambda, gConfig.shadowRange);
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

    // Commit a debounced Shadow Range disk write once the slider has settled.
    FlushPendingShadowRangeWrite();

    ShadowCBData data = {};
    data.enabled             = gConfig.enabled ? 1.0f : 0.0f;
    // Scale the UV-space RTW filter radius by texel size so the filter always
    // covers the same number of shadow texels regardless of atlas resolution.
    // The 0.001 factor was tuned for a 4096 atlas; at lower resolutions the
    // Poisson samples cluster on a single texel and produce visible squares.
    // (4096 / atlasRes) preserves the existing tuning at 4096.
    float resScale           = 4096.0f / (float)GetEffectiveShadowResolution();
    data.rtwFilterRadius     = gConfig.filterRadius * 0.001f * resScale;
    data.rtwLightSize        = gConfig.lightSize * 0.001f * resScale;
    data.rtwPcssEnabled      = gConfig.pcssEnabled ? 1.0f : 0.0f;
    data.rtwBiasScale        = gConfig.biasScale;
    data.rtwCliffFixEnabled  = gConfig.cliffFix ? 1.0f : 0.0f;
    data.rtwCliffFixDistance = gConfig.cliffFixDistance;
    data.rtwNormalBias       = gConfig.normalBias;
    data.rtwSlopeBias        = gConfig.slopeBias * 0.001f;
    // CSM filter radius scales csmParams[i][1] (the per-cascade PCF radius the
    // engine baked into the lighting cbuffer). 1.0 = vanilla taper.
    data.csmFilterRadius     = gConfig.csmFilterRadius;
    data.csmLightSize        = gConfig.csmLightSize;
    data.csmPcssEnabled      = gConfig.csmPcssEnabled ? 1.0f : 0.0f;
    data.csmBlendEnabled     = gConfig.csmBlendEnabled ? 1.0f : 0.0f;
    data.csmBlendWidth       = gConfig.csmBlendWidth;

    // Resolution-tiered RTW PCF tap count: bigger atlases need fewer taps
    // for the same visual softness.
    float atlasRes = (float)GetEffectiveShadowResolution();
    if      (atlasRes >= 12288.0f) data.rtwQuality = 4.0f;
    else if (atlasRes >=  8192.0f) data.rtwQuality = 8.0f;
    else                           data.rtwQuality = 12.0f;

    host->UpdateConstantBuffer(ctx->context, gCB, &data, sizeof(data));
    // Bind to b7: b2 collides with CSM's auto-allocated $Globals cbuffer
    // (which holds the csmParams arrays). See ShaderPatch.cpp for details.
    ctx->context->PSSetConstantBuffers(7, 1, &gCB);
}

static void ShadowPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    ID3D11Buffer* nullCB = nullptr;
    ctx->context->PSSetConstantBuffers(7, 1, &nullCB);
}

static int ShadowIsEnabled() { return 1; }

static void ApplyDustShadows()
{
    if (!gHost) return;
    if (gHost->SetShadowAtlasResolution)
        gHost->SetShadowAtlasResolution(GetSelectedShadowResolution());
    // Kenshi's native splits don't fit the PSSM formula at any lambda, so
    // the slider default (0.95) means "keep native"; any other value
    // activates the formula override. Negative clears it host-side.
    if (gHost->SetCascadeLambda)
        gHost->SetCascadeLambda(gConfig.pssmLambda == 0.95f ? -1.0f
                                                            : gConfig.pssmLambda);
    // Live shadow-range push, only when the slider differs from the value
    // Kenshi booted with — the boot value already IS the live range, and an
    // unnecessary override would formula-rewrite the native CSM splits.
    // Negative clears a previously latched override. The host may not have
    // captured Kenshi's splits source yet at Init time — failure is silent
    // and expected; the request is remembered and applied at capture. We
    // also wrote settings.cfg in DustEffectCreate so a restart applies the
    // value regardless.
    if (gHost->SetShadowRange)
        gHost->SetShadowRange(gConfig.shadowRange != gVanillaShadowRange
                              ? (float)gConfig.shadowRange : -1.0f);
}

static void RestoreVanillaShadows()
{
    if (!gHost) return;
    if (gHost->SetShadowAtlasResolution && gHost->GetShadowBaseResolution)
    {
        uint32_t base = gHost->GetShadowBaseResolution();
        if (base > 0)
            gHost->SetShadowAtlasResolution(base);
    }
    if (gHost->SetCascadeLambda)
        gHost->SetCascadeLambda(-1.0f);
    if (gHost->SetShadowRange)
        gHost->SetShadowRange(-1.0f);
}

static void ShadowOnSettingChanged()
{
    if (gConfig.enabled)
    {
        ApplyDustShadows();
        // Defer the settings.cfg disk write — a slider drag fires this every
        // frame and a synchronous file rewrite per tick is a frame hitch.
        // (The live range already went to the engine in ApplyDustShadows.)
        QueueShadowRangeWrite();
    }
    else if (gWasEnabled)
    {
        RestoreVanillaShadows();
    }
    gWasEnabled = gConfig.enabled;
}

// Runs in EffectLoader::LoadAll right after our INI is loaded, BEFORE Init.
// Pushes the atlas-resolution override and cascade lambda to the host hooks
// so Kenshi's atlas + shadow node init (between LoadAll and InitAll) pick
// them up immediately.
static void ShadowEarlyConfigApply(const DustHostAPI* host)
{
    if (!gConfig.enabled || !host) return;
    if (host->SetShadowAtlasResolution)
        host->SetShadowAtlasResolution(GetSelectedShadowResolution());
    if (host->SetCascadeLambda && gConfig.pssmLambda != 0.95f)
        host->SetCascadeLambda(gConfig.pssmLambda);
}

static DustSettingDesc gSettings[] = {
    // === Common (affect both RTWSM and CSM) ===
    { "Common",              DUST_SETTING_SECTION, nullptr,                 0.0f, 0.0f,  nullptr,            nullptr, nullptr, DUST_PERF_NONE },
    { "Enabled",             DUST_SETTING_BOOL,  &gConfig.enabled,          0.0f, 1.0f,  "Enabled",          nullptr, "Enable or disable Dust's improved shadow filtering (RTWSM and CSM). When off, vanilla Kenshi filtering is used.",                                                              DUST_PERF_LOW    },
    // Resolution and Range bridge to player-owned engine/game state (atlas size,
    // and a Shadow Range that persists to settings.cfg and is overwritten by the
    // in-game slider). They are PRESET_OPTIONAL: never baked into presets, and a
    // preset that omits them is not "outdated".
    { "Shadow Resolution",   DUST_SETTING_ENUM,  &gConfig.resolutionIndex,  0.0f, 7.0f,  "Resolution",       kShadowResolutionLabels, "Override the shadow atlas resolution. Vanilla = no override (zero overhead). Higher = sharper shadows but the shadow pass costs proportionally more GPU time and VRAM (16384 ~= 1 GB) — 8192+ can visibly destabilize the framerate. Applies next frame.", DUST_PERF_HIGH, DUST_SETTING_FLAG_PRESET_OPTIONAL  },
    { "Shadow Range",        DUST_SETTING_INT,   &gConfig.shadowRange,      500.0f, 50000.0f, "Range",        nullptr, "Maximum distance shadows render. Bypasses the in-game UI's 9000 cap. Applies live (cascade splits + shadow camera re-derive next frame). Also written to settings.cfg so the value survives a restart. Touching the in-game Shadow Range slider will overwrite this.", DUST_PERF_MEDIUM, DUST_SETTING_FLAG_PRESET_OPTIONAL },

    // === RTWSM (warped shadow map) ===
    { "RTWSM",               DUST_SETTING_SECTION, nullptr,                 0.0f, 0.0f,  nullptr,            nullptr, nullptr, DUST_PERF_NONE },
    { "Filter Radius",       DUST_SETTING_FLOAT, &gConfig.filterRadius,     0.1f, 5.0f,  "FilterRadius",     nullptr, "Size of the shadow softening filter (RTWSM only).",                                                                                                                           DUST_PERF_NONE   },
    { "Light Size",          DUST_SETTING_FLOAT, &gConfig.lightSize,        0.5f, 10.0f, "LightSize",        nullptr, "Simulated light source size for contact-hardening shadows (RTWSM PCSS).",                                                                                                     DUST_PERF_NONE   },
    { "PCSS",                DUST_SETTING_BOOL,  &gConfig.pcssEnabled,      0.0f, 1.0f,  "PCSS",             nullptr, "Enable Percentage-Closer Soft Shadows for RTWSM (distance-based softness).",                                                                                                  DUST_PERF_MEDIUM },
    { "Bias Scale",          DUST_SETTING_FLOAT, &gConfig.biasScale,        0.0f, 3.0f,  "BiasScale",        nullptr, "Shadow bias multiplier to reduce RTWSM acne artifacts.",                                                                                                                      DUST_PERF_NONE },
    { "Normal Bias",         DUST_SETTING_FLOAT, &gConfig.normalBias,       0.0f, 5.0f,  "NormalBias",       nullptr, "Offsets the shadow lookup along the surface normal to prevent self-shadowing. Higher = fewer shadow acne artifacts on detailed geometry, but shadows detach slightly from contact edges.", DUST_PERF_NONE },
    { "Slope Bias",          DUST_SETTING_FLOAT, &gConfig.slopeBias,        0.0f, 5.0f,  "SlopeBias",        nullptr, "Extra depth bias on surfaces at grazing angles to the light. Reduces acne on near-vertical faces without affecting flat surfaces.",                                          DUST_PERF_NONE },
    { "Cliff Shadow Fix",    DUST_SETTING_BOOL,  &gConfig.cliffFix,         0.0f, 1.0f,  "CliffFix",         nullptr, "Reduce shadow acne on steep cliffs and vertical faces (RTWSM only). Can make close-range vertical shadows fade out. Integration of Crunk Aint Dead's Cliff Face Shadow Fix mod.", DUST_PERF_NONE },
    { "Cliff Fix Distance",  DUST_SETTING_FLOAT, &gConfig.cliffFixDistance, 0.0f, 1.0f,  "CliffFixDistance", nullptr, "Fraction of shadow range where the cliff fix smoothly ramps in (higher = preserves more close-range vertical shadows).",                                                    DUST_PERF_NONE },

    // === CSM (cascaded shadow maps) ===
    { "CSM",                 DUST_SETTING_SECTION, nullptr,                 0.0f, 0.0f,  nullptr,            nullptr, nullptr, DUST_PERF_NONE },
    { "Cascade Lambda",      DUST_SETTING_FLOAT, &gConfig.pssmLambda,       0.0f, 1.0f,  "CascadeLambda",    nullptr, "PSSM cascade split distribution. At 0.95 (default) Kenshi's native splits are kept. Other values override: 0.0 = pure linear (close shadows extend further but blockier); 1.0 = pure logarithmic (close shadows tiny+sharp, far cascades huge).", DUST_PERF_NONE },
    { "CSM Filter Radius",   DUST_SETTING_FLOAT, &gConfig.csmFilterRadius,  0.1f, 5.0f,  "CsmFilterRadius",  nullptr, "Global scale on the CSM PCF filter radius (on top of Kenshi's vanilla per-cascade taper).",                                                                                   DUST_PERF_NONE },
    { "CSM Light Size",      DUST_SETTING_FLOAT, &gConfig.csmLightSize,     0.5f, 10.0f, "CsmLightSize",     nullptr, "Simulated light source size for CSM PCSS (contact-hardening). Multiplier on the per-cascade filter radius.",                                                                 DUST_PERF_NONE },
    { "CSM PCSS",            DUST_SETTING_BOOL,  &gConfig.csmPcssEnabled,   0.0f, 1.0f,  "CsmPcss",          nullptr, "Enable Percentage-Closer Soft Shadows for CSM. Blocker search + variable penumbra. Significant cost.",                                                                       DUST_PERF_HIGH },
    { "Cascade Blending",    DUST_SETTING_BOOL,  &gConfig.csmBlendEnabled,  0.0f, 1.0f,  "CascadeBlending",  nullptr, "Smoothly blend between adjacent CSM cascades near their split boundary. Hides the hard resolution step where cascades meet.",                                              DUST_PERF_MEDIUM },
    { "Cascade Blend Width", DUST_SETTING_FLOAT, &gConfig.csmBlendWidth,    0.0f, 0.5f,  "CascadeBlendWidth", nullptr, "Width of the blend band at each cascade boundary, as a fraction of cascade depth range (0.05 = subtle, 0.25 = wide).",                                                      DUST_PERF_NONE },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;
    memset(desc, 0, sizeof(*desc));

    // Resolve the user's intended shadow range (preset > our INI > game's
    // current settings.cfg) and write it to settings.cfg NOW, before Kenshi
    // reads the file at startup. Writing later (Init or OnSettingChanged) is
    // futile — Kenshi caches the value early and stomps the file on exit.
    int currentGame = ReadSettingsCfgInt("Shadow Range", -1);
    if (currentGame > 0)
        gVanillaShadowRange = currentGame;
    int resolved = ResolveUserShadowRange(currentGame > 0 ? currentGame : gConfig.shadowRange);
    gConfig.shadowRange = resolved;
    PushShadowRangeToGame();

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
    desc->OnSettingChanged  = ShadowOnSettingChanged;
    desc->OnEarlyConfigApply = ShadowEarlyConfigApply;
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
