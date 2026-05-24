// DustShadows.cpp - Shadow filtering settings plugin for Dust (API v3)
// Manages runtime parameters for the improved RTWSM shadow filtering
// injected by PatchDeferredShader. Binds a constant buffer at b2 that
// the patched deferred shader reads for filter radius, light size, etc.

#include "../../src/DustAPI.h"
#include "DustLog.h"

#include <d3d11.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

DustLogFn gLogFn = nullptr;

struct ShadowConfig {
    // === Shared ===
    bool  enabled           = true;
    int   resolutionIndex   = 2;      // index into kShadowResolutions (default = 4096)
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
    float pssmLambda        = 0.95f;  // PSSM cascade split distribution. 0.0 =
                                      // pure linear (close shadows extend
                                      // far, but blockier); 1.0 = pure log
                                      // (close shadows tiny+sharp, far blurry)
    float csmFilterRadius   = 1.0f;   // global scale on top of per-cascade radii
    float csmLightSize      = 2.0f;   // PCSS blocker-search/penumbra scale (multiplier on baseRadius)
    bool  csmPcssEnabled    = true;
    bool  csmBlendEnabled   = true;   // smooth blend between adjacent cascades
    float csmBlendWidth     = 0.15f;  // fraction of cascade depth range used as blend band
    float cascade0Filter    = 1.0f;   // per-cascade filter-radius multipliers
    float cascade1Filter    = 1.0f;   // (relative to Kenshi's vanilla per-
    float cascade2Filter    = 1.0f;   // cascade taper; default 1.0 = vanilla)
    float cascade3Filter    = 1.0f;
};

static ShadowConfig gConfig;
static ID3D11Buffer* gCB = nullptr;
static const DustHostAPI* gHost = nullptr;
static int gLastWrittenShadowRange = INT_MIN;  // debounce slider-drag disk writes
static int gVanillaShadowRange = -1;
static bool gWasEnabled = true;

static const uint32_t kShadowResolutions[] = { 1024, 2048, 4096, 6144, 8192, 12288, 16384 };
static const char* const kShadowResolutionLabels[] = {
    "1024", "2048", "4096", "6144", "8192", "12288", "16384", nullptr
};

static uint32_t GetSelectedShadowResolution()
{
    int idx = gConfig.resolutionIndex;
    int n = (int)(sizeof(kShadowResolutions) / sizeof(kShadowResolutions[0]));
    if (idx < 0 || idx >= n) idx = 2;
    return kShadowResolutions[idx];
}

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

// Write our current shadowRange to settings.cfg, deduping repeated writes
// (slider-drag triggers OnSettingChanged on every frame).
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

// b7 cbuffer layout — must match the HLSL declaration injected by
// ShaderPatch::PatchDeferredShader. 12 floats, no explicit padding required
// because HLSL packs floats tightly within 16-byte rows.
struct alignas(16) ShadowCBData {
    // Shared
    float enabled;
    // RTWSM
    float rtwFilterRadius;
    float rtwLightSize;
    float rtwPcssEnabled;
    float rtwBiasScale;
    float rtwCliffFixEnabled;
    float rtwCliffFixDistance;
    float rtwNormalBias;
    float rtwSlopeBias;
    // CSM
    float csmFilterRadius;
    float csmLightSize;
    float csmPcssEnabled;
    float csmBlendEnabled;
    float csmBlendWidth;
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
    if (host->SetShadowAtlasResolution)
        host->SetShadowAtlasResolution(GetSelectedShadowResolution());
    if (host->SetCascadeLambda)
        host->SetCascadeLambda(gConfig.pssmLambda);
    if (host->SetCascadeFilterScale)
    {
        host->SetCascadeFilterScale(0, gConfig.cascade0Filter);
        host->SetCascadeFilterScale(1, gConfig.cascade1Filter);
        host->SetCascadeFilterScale(2, gConfig.cascade2Filter);
        host->SetCascadeFilterScale(3, gConfig.cascade3Filter);
    }
    // Live shadow-range push. PssmDetour may not yet have captured Kenshi's
    // splits source pointer at Init time (scene init runs later) — if so,
    // SetShadowRange returns 0 and we rely on OnSettingChanged or a deferred
    // push to apply the value once the capture lands. Failure is silent and
    // expected on early init; we still wrote settings.cfg in DustEffectCreate
    // so a restart will apply the value regardless.
    if (host->SetShadowRange)
        host->SetShadowRange((float)gConfig.shadowRange);
    // NB: shadow range was already written to settings.cfg from DustEffectCreate,
    // which runs early enough to beat Kenshi's startup read. A write here
    // would land too late and could clobber the early write with a stale
    // per-effect INI value (the framework's INI load runs after our seed).
    Log("Shadows: Initialized (atlas resolution = %u, shadow range = %d)",
        GetSelectedShadowResolution(), gConfig.shadowRange);
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
    data.enabled             = gConfig.enabled ? 1.0f : 0.0f;
    // Scale the UV-space RTW filter radius by texel size so the filter always
    // covers the same number of shadow texels regardless of atlas resolution.
    // The 0.001 factor was tuned for a 4096 atlas; at lower resolutions the
    // Poisson samples cluster on a single texel and produce visible squares.
    // (4096 / atlasRes) preserves existing tuning at 4096.
    float resScale           = 4096.0f / (float)GetSelectedShadowResolution();
    data.rtwFilterRadius     = gConfig.filterRadius * 0.001f * resScale;
    data.rtwLightSize        = gConfig.lightSize * 0.001f * resScale;
    data.rtwPcssEnabled      = gConfig.pcssEnabled ? 1.0f : 0.0f;
    data.rtwBiasScale        = gConfig.biasScale;
    data.rtwCliffFixEnabled  = gConfig.cliffFix ? 1.0f : 0.0f;
    data.rtwCliffFixDistance = gConfig.cliffFixDistance;
    data.rtwNormalBias       = gConfig.normalBias;
    data.rtwSlopeBias        = gConfig.slopeBias * 0.001f;
    // CSM filter radius scales csmParams[i][1] (the per-cascade PCF radius the
    // engine baked into the lighting cbuffer). 1.0 = vanilla. The per-cascade
    // multipliers (cascade0Filter..) are applied separately via PssmDetour
    // writing directly into csmParams[i][1] — they compound with this global.
    data.csmFilterRadius     = gConfig.csmFilterRadius;
    data.csmLightSize        = gConfig.csmLightSize;
    data.csmPcssEnabled      = gConfig.csmPcssEnabled ? 1.0f : 0.0f;
    data.csmBlendEnabled     = gConfig.csmBlendEnabled ? 1.0f : 0.0f;
    data.csmBlendWidth       = gConfig.csmBlendWidth;

    host->UpdateConstantBuffer(ctx->context, gCB, &data, sizeof(data));
    // Bind to b7: b2 collides with CSM's auto-allocated $Globals cbuffer
    // (which holds csmParams arrays). See ShaderPatch.cpp for details.
    ctx->context->PSSetConstantBuffers(7, 1, &gCB);
}

static void ShadowPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    ID3D11Buffer* nullCB = nullptr;
    ctx->context->PSSetConstantBuffers(7, 1, &nullCB);
}

static int ShadowIsEnabled() { return gConfig.enabled ? 1 : 0; }

static void RestoreVanillaShadows()
{
    if (!gHost) return;
    if (gHost->GetShadowBaseResolution && gHost->SetShadowAtlasResolution)
    {
        uint32_t base = gHost->GetShadowBaseResolution();
        if (base > 0)
            gHost->SetShadowAtlasResolution(base);
    }
    if (gHost->SetShadowRange && gVanillaShadowRange > 0)
        gHost->SetShadowRange((float)gVanillaShadowRange);
    if (gHost->SetCascadeLambda)
        gHost->SetCascadeLambda(0.95f);
    if (gHost->SetCascadeFilterScale)
        for (int i = 0; i < 4; i++)
            gHost->SetCascadeFilterScale(i, 1.0f);
}

static void ApplyDustShadows()
{
    if (!gHost) return;
    if (gHost->SetShadowAtlasResolution)
        gHost->SetShadowAtlasResolution(GetSelectedShadowResolution());
    if (gHost->SetCascadeLambda)
        gHost->SetCascadeLambda(gConfig.pssmLambda);
    if (gHost->SetCascadeFilterScale)
    {
        gHost->SetCascadeFilterScale(0, gConfig.cascade0Filter);
        gHost->SetCascadeFilterScale(1, gConfig.cascade1Filter);
        gHost->SetCascadeFilterScale(2, gConfig.cascade2Filter);
        gHost->SetCascadeFilterScale(3, gConfig.cascade3Filter);
    }
    if (gHost->SetShadowRange)
        gHost->SetShadowRange((float)gConfig.shadowRange);
}

static void ShadowOnSettingChanged()
{
    if (gConfig.enabled)
    {
        ApplyDustShadows();
        PushShadowRangeToGame();
    }
    else if (gWasEnabled)
    {
        RestoreVanillaShadows();
    }
    gWasEnabled = gConfig.enabled;
}

static DustSettingDesc gSettings[] = {
    // === Common (affect both RTWSM and CSM) ===
    { "Common",              DUST_SETTING_SECTION, nullptr,                 0.0f, 0.0f,  nullptr,            nullptr, nullptr, DUST_PERF_NONE },
    { "Enabled",             DUST_SETTING_BOOL,  &gConfig.enabled,          0.0f, 1.0f,  "Enabled",          nullptr, "Enable or disable Dust's improved shadow filtering (RTWSM and CSM). When off, vanilla Kenshi filtering is used.",                                                                                  DUST_PERF_LOW    },
    { "Shadow Resolution",   DUST_SETTING_ENUM,  &gConfig.resolutionIndex,  0.0f, 6.0f,  "Resolution",       kShadowResolutionLabels, "Override the shadow atlas resolution. Higher = sharper shadows, more VRAM (16384 ~= 1 GB). Applies next frame.",                                              DUST_PERF_LOW    },
    { "Shadow Range",        DUST_SETTING_INT,   &gConfig.shadowRange,      500.0f, 50000.0f, "Range",        nullptr, "Maximum distance shadows render. Bypasses the in-game UI's 9000 cap. Applies live (cascade splits + shadow camera frusta re-derive next frame). Also written to settings.cfg so the value survives a restart. Touching the in-game Shadow Range slider will overwrite this.", DUST_PERF_MEDIUM },

    // === RTWSM (warped shadow map — Kenshi's default in some configs) ===
    { "RTWSM",               DUST_SETTING_SECTION, nullptr,                 0.0f, 0.0f,  nullptr,            nullptr, nullptr, DUST_PERF_NONE },
    { "Filter Radius",       DUST_SETTING_FLOAT, &gConfig.filterRadius,     0.1f, 5.0f,  "FilterRadius",     nullptr, "Size of the shadow softening filter (RTWSM only).",                                                                                                                                         DUST_PERF_NONE   },
    { "Light Size",          DUST_SETTING_FLOAT, &gConfig.lightSize,        0.5f, 10.0f, "LightSize",        nullptr, "Simulated light source size for contact-hardening shadows (RTWSM PCSS).",                                                                                                                   DUST_PERF_NONE   },
    { "PCSS",                DUST_SETTING_BOOL,  &gConfig.pcssEnabled,      0.0f, 1.0f,  "PCSS",             nullptr, "Enable Percentage-Closer Soft Shadows for RTWSM (distance-based softness).",                                                                                                                DUST_PERF_MEDIUM },
    { "Bias Scale",          DUST_SETTING_FLOAT, &gConfig.biasScale,        0.0f, 3.0f,  "BiasScale",        nullptr, "Shadow bias multiplier to reduce RTWSM acne artifacts.",                                                                                                                                    DUST_PERF_NONE },
    { "Normal Bias",         DUST_SETTING_FLOAT, &gConfig.normalBias,       0.0f, 5.0f,  "NormalBias",       nullptr, "Offsets the shadow lookup along the surface normal to prevent self-shadowing. Higher = fewer shadow acne artifacts on detailed geometry, but shadows detach slightly from contact edges.",  DUST_PERF_NONE },
    { "Slope Bias",          DUST_SETTING_FLOAT, &gConfig.slopeBias,        0.0f, 5.0f,  "SlopeBias",        nullptr, "Extra depth bias on surfaces at grazing angles to the light. Reduces acne on near-vertical faces without affecting flat surfaces.",                                                        DUST_PERF_NONE },
    { "Cliff Shadow Fix",    DUST_SETTING_BOOL,  &gConfig.cliffFix,         0.0f, 1.0f,  "CliffFix",         nullptr, "Reduce shadow acne on steep cliffs and vertical faces (RTWSM only). Can make close-range vertical shadows fade out. Integration of Crunk Aint Dead's Cliff Face Shadow Fix mod.",        DUST_PERF_NONE },
    { "Cliff Fix Distance",  DUST_SETTING_FLOAT, &gConfig.cliffFixDistance, 0.0f, 1.0f,  "CliffFixDistance", nullptr, "Fraction of shadow range where the cliff fix smoothly ramps in (higher = preserves more close-range vertical shadows).",                                                                  DUST_PERF_NONE },

    // === CSM (cascaded shadow maps) ===
    { "CSM",                 DUST_SETTING_SECTION, nullptr,                 0.0f, 0.0f,  nullptr,            nullptr, nullptr, DUST_PERF_NONE },
    { "Cascade Lambda",      DUST_SETTING_FLOAT, &gConfig.pssmLambda,       0.0f, 1.0f,  "CascadeLambda",    nullptr, "PSSM cascade split distribution. 0.0 = pure linear (close shadows extend further but blockier); 1.0 = pure logarithmic (close shadows tiny+sharp, far cascades huge). Kenshi's native is ~0.95.", DUST_PERF_NONE },
    { "CSM Filter Radius",   DUST_SETTING_FLOAT, &gConfig.csmFilterRadius,  0.1f, 5.0f,  "CsmFilterRadius",  nullptr, "Global scale on CSM PCF filter radius. Composes with the per-cascade multipliers below.",                                                                                                   DUST_PERF_NONE },
    { "CSM Light Size",      DUST_SETTING_FLOAT, &gConfig.csmLightSize,     0.5f, 10.0f, "CsmLightSize",     nullptr, "Simulated light source size for CSM PCSS (contact-hardening). Multiplier on the per-cascade filter radius.",                                                                               DUST_PERF_NONE },
    { "CSM PCSS",            DUST_SETTING_BOOL,  &gConfig.csmPcssEnabled,   0.0f, 1.0f,  "CsmPcss",          nullptr, "Enable Percentage-Closer Soft Shadows for CSM. Blocker search + variable penumbra. Significant cost.",                                                                                     DUST_PERF_HIGH },
    { "Cascade Blending",    DUST_SETTING_BOOL,  &gConfig.csmBlendEnabled,  0.0f, 1.0f,  "CascadeBlending",  nullptr, "Smoothly blend between adjacent CSM cascades near their split boundary. Hides the hard resolution step where cascades meet.",                                                            DUST_PERF_MEDIUM },
    { "Cascade Blend Width", DUST_SETTING_FLOAT, &gConfig.csmBlendWidth,    0.0f, 0.5f,  "CascadeBlendWidth", nullptr, "Width of the blend band at each cascade boundary, as a fraction of cascade depth range (0.05 = subtle, 0.25 = wide).",                                                                    DUST_PERF_NONE },
    { "Cascade 0 Filter",    DUST_SETTING_FLOAT, &gConfig.cascade0Filter,   0.0f, 5.0f,  "Cascade0Filter",   nullptr, "Filter-radius multiplier for the closest CSM cascade. 1.0 = Kenshi's vanilla taper, >1.0 = softer.",                                                                                       DUST_PERF_NONE },
    { "Cascade 1 Filter",    DUST_SETTING_FLOAT, &gConfig.cascade1Filter,   0.0f, 5.0f,  "Cascade1Filter",   nullptr, "Filter-radius multiplier for CSM cascade 1 (mid-range shadows).",                                                                                                                            DUST_PERF_NONE },
    { "Cascade 2 Filter",    DUST_SETTING_FLOAT, &gConfig.cascade2Filter,   0.0f, 5.0f,  "Cascade2Filter",   nullptr, "Filter-radius multiplier for CSM cascade 2 (far-mid shadows).",                                                                                                                              DUST_PERF_NONE },
    { "Cascade 3 Filter",    DUST_SETTING_FLOAT, &gConfig.cascade3Filter,   0.0f, 5.0f,  "Cascade3Filter",   nullptr, "Filter-radius multiplier for the farthest CSM cascade (where blockiness shows most). >1.0 softens the far-cascade artifacts.",                                                            DUST_PERF_NONE },
};

// Runs in EffectLoader::LoadAll right after our INI is loaded, BEFORE Init.
// Pushes the atlas-resolution override to the host hook so Kenshi's atlas
// (created between LoadAll and InitAll) picks up the override. Doing this
// from Init is too late — by then the atlas already exists at vanilla size.
static void ShadowEarlyConfigApply(const DustHostAPI* host)
{
    if (host && host->SetShadowAtlasResolution)
        host->SetShadowAtlasResolution(GetSelectedShadowResolution());
}

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

    desc->apiVersion         = DUST_API_VERSION;
    desc->name               = "Shadows";
    desc->injectionPoint     = DUST_INJECT_POST_LIGHTING;
    desc->priority           = -10;
    desc->Init               = ShadowInit;
    desc->Shutdown           = ShadowShutdown;
    desc->preExecute         = ShadowPreExecute;
    desc->postExecute        = ShadowPostExecute;
    desc->IsEnabled          = ShadowIsEnabled;
    desc->settings           = gSettings;
    desc->settingCount       = sizeof(gSettings) / sizeof(gSettings[0]);
    desc->OnSettingChanged   = ShadowOnSettingChanged;
    desc->OnEarlyConfigApply = ShadowEarlyConfigApply;
    desc->flags              = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection      = "Shadows";

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);
    return TRUE;
}
