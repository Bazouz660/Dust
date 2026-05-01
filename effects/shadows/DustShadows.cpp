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
    bool  enabled           = true;
    float filterRadius      = 1.0f;
    float lightSize         = 3.0f;
    bool  pcssEnabled       = true;
    float biasScale         = 1.0f;
    bool  cliffFix          = false;  // off by default: previous always-on caused
                                      // close-range vertical shadows to disappear
    float cliffFixDistance  = 0.10f;  // fraction of shadow range where the bias
                                      // ramps in; smooth saturate curve, not a hard cutoff
    int   resolutionIndex   = 2;      // index into kShadowResolutions (default = 4096)
    int   shadowRange       = 6000;   // overridden in DustEffectCreate from settings.cfg
                                      // if a value is already present there
};

static ShadowConfig gConfig;
static ID3D11Buffer* gCB = nullptr;
static const DustHostAPI* gHost = nullptr;
static int gLastWrittenShadowRange = INT_MIN;  // debounce slider-drag disk writes

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
    FILE* f = fopen(path.c_str(), "rb");
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

    FILE* f = fopen(path.c_str(), "wb");
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

struct alignas(16) ShadowCBData {
    float enabled;
    float filterRadius;
    float lightSize;
    float pcssEnabled;
    float biasScale;
    float cliffFixEnabled;
    float cliffFixDistance;
    float csmFilterScale;
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
    // NB: shadow range is written to settings.cfg from DustEffectCreate,
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
    data.enabled          = gConfig.enabled ? 1.0f : 0.0f;
    // Scale the UV-space filter radius by texel size so the filter always covers
    // the same number of shadow texels regardless of atlas resolution. The 0.001
    // factor was originally tuned for a 4096 atlas; at lower resolutions the
    // Poisson samples were clustering on a single texel and producing visible
    // squares. Using (4096 / atlasRes) preserves existing tuning at 4096 and
    // widens the kernel proportionally at smaller sizes.
    float resScale        = 4096.0f / (float)GetSelectedShadowResolution();
    data.filterRadius     = gConfig.filterRadius * 0.001f * resScale;
    data.lightSize        = gConfig.lightSize * 0.001f * resScale;
    data.pcssEnabled      = gConfig.pcssEnabled ? 1.0f : 0.0f;
    data.biasScale        = gConfig.biasScale;
    data.cliffFixEnabled  = gConfig.cliffFix ? 1.0f : 0.0f;
    data.cliffFixDistance = gConfig.cliffFixDistance;
    // CSM uses gConfig.filterRadius as a raw multiplier on csmParams[i][1]
    // (vanilla per-cascade PCF radius). RTW uses the same slider value but
    // multiplied by 0.001 above for UV-space. Same slider, two interpretations.
    data.csmFilterScale   = gConfig.filterRadius;

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

static int ShadowIsEnabled() { return 1; }

static void ShadowOnSettingChanged()
{
    if (gHost && gHost->SetShadowAtlasResolution)
        gHost->SetShadowAtlasResolution(GetSelectedShadowResolution());
    PushShadowRangeToGame();
}

static DustSettingDesc gSettings[] = {
    { "Enabled",             DUST_SETTING_BOOL,  &gConfig.enabled,          0.0f, 1.0f,  "Enabled",          nullptr, "Enable or disable shadow filtering",                                                                                                                                          DUST_PERF_LOW    },
    { "Filter Radius",       DUST_SETTING_FLOAT, &gConfig.filterRadius,     0.1f, 5.0f,  "FilterRadius",     nullptr, "Size of the shadow softening filter",                                                                                                                                         DUST_PERF_NONE   },
    { "Light Size",          DUST_SETTING_FLOAT, &gConfig.lightSize,        0.5f, 10.0f, "LightSize",        nullptr, "Simulated light source size for contact-hardening shadows",                                                                                                                   DUST_PERF_NONE   },
    { "PCSS",                DUST_SETTING_BOOL,  &gConfig.pcssEnabled,      0.0f, 1.0f,  "PCSS",             nullptr, "Enable Percentage-Closer Soft Shadows for distance-based softness",                                                                                                           DUST_PERF_MEDIUM },
    { "Bias Scale",          DUST_SETTING_FLOAT, &gConfig.biasScale,        0.0f, 3.0f,  "BiasScale",        nullptr, "Shadow bias multiplier to reduce shadow acne artifacts",                                                                                                                      DUST_PERF_NONE },
    { "Cliff Shadow Fix",    DUST_SETTING_BOOL,  &gConfig.cliffFix,         0.0f, 1.0f,  "CliffFix",         nullptr, "Reduce shadow acne on steep cliffs and vertical faces (can make close-range vertical shadows fade out). Integration of Crunk Aint Dead's Cliff Face Shadow Fix mod.",      DUST_PERF_NONE },
    { "Cliff Fix Distance",  DUST_SETTING_FLOAT, &gConfig.cliffFixDistance, 0.0f, 1.0f,  "CliffFixDistance", nullptr, "Fraction of shadow range where the cliff fix smoothly ramps in (higher = preserves more close-range vertical shadows)",                                                     DUST_PERF_NONE },
    { "Shadow Resolution",   DUST_SETTING_ENUM,  &gConfig.resolutionIndex,  0.0f, 6.0f,  "Resolution",       kShadowResolutionLabels, "Override the shadow atlas resolution. Higher = sharper shadows, more VRAM (16384 ~= 1 GB). Restart the game to apply.",                                              DUST_PERF_LOW    },
    { "Shadow Range",        DUST_SETTING_INT,   &gConfig.shadowRange,      500.0f, 50000.0f, "Range",        nullptr, "Maximum distance shadows render. Bypasses the in-game UI's 9000 cap by writing settings.cfg directly. Restart to apply. Touching the in-game Shadow Range slider will overwrite this.", DUST_PERF_MEDIUM },
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
