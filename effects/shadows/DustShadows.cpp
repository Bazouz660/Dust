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
#include <cmath>
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

    // === Screen-space contact shadows (point/spot lights) ===
    // ENB particle-light style: ray-march the GBuffer depth from each lit
    // surface toward the light inside light_fs. Off by default (opt-in).
    bool  contactEnabled    = false;
    float contactIntensity  = 1.0f;   // 0 = none, 1 = fully dark where occluded
    int   contactSteps      = 12;     // march samples per pixel per light
    float contactRange      = 600.0f; // max march distance (world units)
    float contactThickness  = 300.0f; // occluder thickness window (world units)
    float contactBias       = 8.0f;   // min depth diff to count as occluder (world units)
    bool  contactDebug      = false;  // tint occluded pixels red to verify placement
    bool  contactBackface   = false;  // use real back-face depth for thickness (extra geometry pass)
    int   contactBackfaceScale = 2;   // back-depth resolution divisor (1=full, 2=half, 4=quarter) — perf
    bool  contactBackfaceFlip = false;// flip cull winding if back-depth looks wrong (matches game's front-face convention)
    bool  contactBackfaceTranspose = false;// transpose the replay view-proj matrix (convention probe)
    float contactSoftness   = 0.0f;   // soft-edge penumbra width (world units); 0 = sharp (1 ray), >0 = 7-ray cone
    float contactStepLength = 0.0f;   // march step length (world units); 0 = auto (range / steps)
    float contactLightRadius = 0.0f;  // ignore occluders within this distance of the light (world units); fixes lamp self-shadow
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
// ShaderPatch::PatchDeferredShader. Floats pack tightly within 16-byte rows.
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
    // Quality
    float rtwQuality;
    // Contact shadows (point/spot lights) — order must match the b7
    // declaration appended in ShaderPatch::PatchDeferredShader.
    float contactEnabled;
    float contactIntensity;
    float contactSteps;
    float contactRange;
    float contactThickness;
    float contactBias;
    float contactDebug;
    float contactBackface;
    float contactSoftness;
    float contactStepLength;
    float contactLightRadius;
};

// ============================================================================
// Back-face depth pass — exact contact-shadow thickness.
//
// Re-renders captured GBuffer geometry from the camera with front-face culling
// into an R32_FLOAT target holding euclidean view distance (raw world units).
// light_fs then brackets each ray sample between the front depth (gBuf2) and
// this back depth, giving each occluder its true thickness instead of a flat
// guess. Single-sided geometry (terrain, foliage cards) leaves 0 and light_fs
// falls back to the flat thickness window.
// ============================================================================

// Any nonzero capture flag enables lean geometry capture (IA + VS + CB
// staging) — all that a depth-only replay needs. Bit 0
// (DUST_CAPTURE_PS_RESOURCES) would also capture PS state we don't use, so use
// bit 1 to keep capture cheap.
static const uint32_t kCaptureLean = 2u;

static ID3D11Texture2D*          gBfDepthTex   = nullptr;  // scratch D32 for z-test
static ID3D11DepthStencilView*   gBfDSV        = nullptr;
static ID3D11Texture2D*          gBfEuclidTex  = nullptr;  // R32 euclidean back depth
static ID3D11RenderTargetView*   gBfRTV        = nullptr;
static ID3D11ShaderResourceView* gBfSRV        = nullptr;
static ID3D11PixelShader*        gBfPS         = nullptr;
static ID3D11Buffer*             gBfCB         = nullptr;
static ID3D11RasterizerState*    gBfRaster     = nullptr;  // front-cull (CW front)
static ID3D11RasterizerState*    gBfRasterFlip = nullptr;  // front-cull (CCW front)
static ID3D11DepthStencilState*  gBfDepthState = nullptr;  // LESS + write
static ID3D11BlendState*         gBfBlend      = nullptr;  // opaque
static ID3D11SamplerState*       gBfSampler    = nullptr;  // point clamp (light_fs read)
static uint32_t                  gBfWidth = 0, gBfHeight = 0;
static bool                      gBfCaptureOn = false;
// GPU timestamp profiling for the back-face pass (the "Shadows ms" UI counter
// only times the CPU callback, not this GPU work).
static ID3D11Query*              gBfTsDisjoint = nullptr;
static ID3D11Query*              gBfTsBegin    = nullptr;
static ID3D11Query*              gBfTsEnd      = nullptr;
static bool                      gBfTsPending  = false;
static uint32_t                  gBfLastReplayed = 0;
static float                     gBfGpuMs = 0.0f;   // exposed to the perf panel via gpuTimeMsPtr

struct BackfaceCB {
    float invProj[16];   // inverse projection, row-major (HLSL uses row_major)
    float invW, invH;    // 1/width, 1/height -> pixel coords to UV
    float _pad0, _pad1;
};

// Replacement PS for the back-face replay. Consumes only SV_Position (subset of
// every captured VS output), reconstructs the fragment's euclidean view
// distance from hardware depth, and writes it raw (same euclidean space as
// gBuf2*farClip used in light_fs).
static const char* kBackfacePS =
    "cbuffer BfCB : register(b0) {\n"
    "  row_major float4x4 invProj;\n"
    "  float4 bfParams;\n"   // x=1/width, y=1/height
    "};\n"
    "float4 main(float4 svpos : SV_Position) : SV_Target {\n"
    "  float2 uv  = svpos.xy * bfParams.xy;\n"
    "  float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);\n"
    "  float4 clip = float4(ndc, svpos.z, 1.0);\n"
    "  float4 v = mul(invProj, clip);\n"
    "  v /= v.w;\n"
    "  return length(v.xyz).xxxx;\n"
    "}\n";

static void MatMul4x4(float* out, const float* a, const float* b)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c]
                           + a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

// General 4x4 inverse (Gauss-Jordan). Returns false if singular.
static bool Invert4x4(const float m[16], float inv[16])
{
    float a[4][8];
    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; c < 4; c++) a[r][c] = m[r * 4 + c];
        for (int c = 0; c < 4; c++) a[r][4 + c] = (r == c) ? 1.0f : 0.0f;
    }
    for (int col = 0; col < 4; col++)
    {
        int piv = col;
        for (int r = col + 1; r < 4; r++)
            if (fabsf(a[r][col]) > fabsf(a[piv][col])) piv = r;
        if (fabsf(a[piv][col]) < 1e-12f) return false;
        if (piv != col) for (int c = 0; c < 8; c++) { float t = a[col][c]; a[col][c] = a[piv][c]; a[piv][c] = t; }
        float d = a[col][col];
        for (int c = 0; c < 8; c++) a[col][c] /= d;
        for (int r = 0; r < 4; r++)
        {
            if (r == col) continue;
            float f = a[r][col];
            for (int c = 0; c < 8; c++) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) inv[r * 4 + c] = a[r][4 + c];
    return true;
}

// Build the camera View matrix (world->view) row-major from the camera basis,
// so worldPoint(row) * View = viewPoint(row). Matches the deferred convention.
static void BuildViewRowMajor(const DustCameraData& cam, float v[16])
{
    const float* R = cam.camRight; const float* U = cam.camUp;
    const float* F = cam.camForward; const float* P = cam.camPosition;
    float dR = P[0]*R[0] + P[1]*R[1] + P[2]*R[2];
    float dU = P[0]*U[0] + P[1]*U[1] + P[2]*U[2];
    float dF = P[0]*F[0] + P[1]*F[1] + P[2]*F[2];
    v[0]=R[0]; v[1]=U[0]; v[2]=F[0]; v[3]=0;
    v[4]=R[1]; v[5]=U[1]; v[6]=F[1]; v[7]=0;
    v[8]=R[2]; v[9]=U[2]; v[10]=F[2]; v[11]=0;
    v[12]=-dR; v[13]=-dU; v[14]=-dF; v[15]=1;
}

static void ReleaseBackfaceTextures()
{
    if (gBfSRV)       { gBfSRV->Release();       gBfSRV = nullptr; }
    if (gBfRTV)       { gBfRTV->Release();       gBfRTV = nullptr; }
    if (gBfEuclidTex) { gBfEuclidTex->Release(); gBfEuclidTex = nullptr; }
    if (gBfDSV)       { gBfDSV->Release();       gBfDSV = nullptr; }
    if (gBfDepthTex)  { gBfDepthTex->Release();  gBfDepthTex = nullptr; }
    gBfWidth = gBfHeight = 0;
}

static bool CreateBackfaceTextures(ID3D11Device* device, uint32_t w, uint32_t h)
{
    ReleaseBackfaceTextures();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;

    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &gBfEuclidTex))) return false;
    if (FAILED(device->CreateRenderTargetView(gBfEuclidTex, nullptr, &gBfRTV))) return false;
    if (FAILED(device->CreateShaderResourceView(gBfEuclidTex, nullptr, &gBfSRV))) return false;

    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &gBfDepthTex))) return false;
    if (FAILED(device->CreateDepthStencilView(gBfDepthTex, nullptr, &gBfDSV))) return false;

    gBfWidth = w; gBfHeight = h;
    Log("Shadows backface: back-depth target %ux%u", w, h);
    return true;
}

static bool CreateBackfaceStates(ID3D11Device* device, const DustHostAPI* host)
{
    // Two front-cull rasterizer states differing only in winding, so the user
    // can flip if the game's front-face convention is the opposite.
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_FRONT;
    rd.DepthClipEnable = TRUE;
    rd.FrontCounterClockwise = FALSE;
    if (FAILED(device->CreateRasterizerState(&rd, &gBfRaster))) { Log("Shadows backface: raster create failed"); return false; }
    rd.FrontCounterClockwise = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &gBfRasterFlip))) { Log("Shadows backface: raster(flip) create failed"); return false; }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;  // nearest back-face wins
    if (FAILED(device->CreateDepthStencilState(&dd, &gBfDepthState))) { Log("Shadows backface: depth state create failed"); return false; }

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &gBfBlend))) { Log("Shadows backface: blend state create failed"); return false; }

    // ComparisonFunc and MaxLOD must be valid even for a non-comparison point
    // sampler — a zero-init desc leaves ComparisonFunc=0 (invalid) and
    // MaxLOD=0, which makes CreateSamplerState fail on the debug runtime.
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &gBfSampler))) { Log("Shadows backface: sampler create failed"); return false; }

    // Compile the euclidean-writing replacement PS.
    ID3DBlob* blob = host->CompileShader ? host->CompileShader(kBackfacePS, "main", "ps_5_0") : nullptr;
    if (!blob) { Log("Shadows backface: PS compile failed"); return false; }
    HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &gBfPS);
    blob->Release();
    if (FAILED(hr)) { Log("Shadows backface: CreatePixelShader failed (hr=0x%08X)", (unsigned)hr); return false; }

    gBfCB = host->CreateConstantBuffer(device, sizeof(BackfaceCB));
    if (!gBfCB) { Log("Shadows backface: CB create failed"); return false; }

    // GPU timestamp queries for profiling the pass (non-fatal if unavailable).
    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    device->CreateQuery(&qd, &gBfTsDisjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    device->CreateQuery(&qd, &gBfTsBegin);
    device->CreateQuery(&qd, &gBfTsEnd);

    Log("Shadows backface: resources created OK");
    return true;
}

static void ReleaseBackfaceAll()
{
    ReleaseBackfaceTextures();
    if (gBfTsEnd)      { gBfTsEnd->Release();      gBfTsEnd = nullptr; }
    if (gBfTsBegin)    { gBfTsBegin->Release();    gBfTsBegin = nullptr; }
    if (gBfTsDisjoint) { gBfTsDisjoint->Release(); gBfTsDisjoint = nullptr; }
    if (gBfSampler)    { gBfSampler->Release();    gBfSampler = nullptr; }
    if (gBfBlend)      { gBfBlend->Release();      gBfBlend = nullptr; }
    if (gBfDepthState) { gBfDepthState->Release(); gBfDepthState = nullptr; }
    if (gBfRasterFlip) { gBfRasterFlip->Release(); gBfRasterFlip = nullptr; }
    if (gBfRaster)     { gBfRaster->Release();     gBfRaster = nullptr; }
    if (gBfCB)         { gBfCB->Release();         gBfCB = nullptr; }
    if (gBfPS)         { gBfPS->Release();         gBfPS = nullptr; }
}

// Enable/disable lean geometry capture to match whether the back-face pass is
// active. Only toggles on change (SetGeometryCaptureFlags re-installs hooks).
static void UpdateCaptureState()
{
    if (!gHost) return;
    bool want = gConfig.contactEnabled && gConfig.contactBackface;
    if (want == gBfCaptureOn) return;
    uint32_t f = gHost->GetGeometryCaptureFlags ? gHost->GetGeometryCaptureFlags() : 0;
    if (want) f |= kCaptureLean; else f &= ~kCaptureLean;
    if (gHost->SetGeometryCaptureFlags) gHost->SetGeometryCaptureFlags(f);
    gBfCaptureOn = want;
    Log("Shadows backface: capture %s (flags=%u)", want ? "ENABLED" : "disabled", f);
}

// Render back-face euclidean depth for this frame. Runs in preExecute(POST_
// LIGHTING) before the deferred sun draw, so the result is ready for the
// later point/spot light-volume draws.
static void RunBackfacePass(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gBfPS || !gBfCB || !host->ReplayGeometry || !host->SaveState) return;

    uint32_t drawCount = host->GetGeometryDrawCount ? host->GetGeometryDrawCount() : 0;
    static int sDiagFrames = 0;
    if (sDiagFrames < 5)
    {
        Log("Shadows backface: drawCount=%u camValid=%d res=%ux%u",
            drawCount, ctx->camera.valid, ctx->width, ctx->height);
    }
    if (!ctx->camera.valid) { if (sDiagFrames < 5) sDiagFrames++; return; }
    if (drawCount == 0)     { if (sDiagFrames < 5) sDiagFrames++; return; }

    ID3D11DeviceContext* dc = ctx->context;

    // Back-depth at reduced resolution — thickness is coarse, so half/quarter
    // res cuts this pass's raster cost 4x/16x. This pass is the dominant GPU
    // cost of contact shadows (a full extra geometry pass).
    int _divI = gConfig.contactBackfaceScale; if (_divI < 1) _divI = 1;
    uint32_t _div = (uint32_t)_divI;
    uint32_t _bw = ctx->width / _div;  if (_bw < 1) _bw = 1;
    uint32_t _bh = ctx->height / _div; if (_bh < 1) _bh = 1;
    if (gBfWidth != _bw || gBfHeight != _bh || !gBfEuclidTex)
        if (!CreateBackfaceTextures(ctx->device, _bw, _bh)) return;

    // Camera View*Proj (row-major) for the replay's matrix patch.
    float view[16], vp[16];
    BuildViewRowMajor(ctx->camera, view);
    MatMul4x4(vp, view, ctx->camera.projMatrix);
    if (gConfig.contactBackfaceTranspose)
    {
        for (int r = 0; r < 4; r++)
            for (int c = r + 1; c < 4; c++)
            { float t = vp[r*4+c]; vp[r*4+c] = vp[c*4+r]; vp[c*4+r] = t; }
    }

    // Inverse projection for the PS: invert proj as loaded column-major.
    BackfaceCB cb = {};
    float projHlsl[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            projHlsl[r * 4 + c] = ctx->camera.projMatrix[c * 4 + r];
    if (!Invert4x4(projHlsl, cb.invProj)) return;
    cb.invW = 1.0f / (float)gBfWidth;
    cb.invH = 1.0f / (float)gBfHeight;
    dc->UpdateSubresource(gBfCB, 0, nullptr, &cb, 0, 0);

    // Read back the previous frame's GPU timing (non-blocking) and log it
    // periodically with a per-category draw breakdown, so we can see the real
    // cost of this pass and what geometry dominates it.
    if (gBfTsPending && gBfTsDisjoint)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        if (dc->GetData(gBfTsDisjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
        {
            UINT64 t0 = 0, t1 = 0;
            if (dc->GetData(gBfTsBegin, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                dc->GetData(gBfTsEnd,   &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
            {
                gBfTsPending = false;
                if (!dj.Disjoint && dj.Frequency)
                {
                    double ms = double(t1 - t0) / double(dj.Frequency) * 1000.0;
                    gBfGpuMs = (float)ms;   // live value for the perf panel
                    static int _logN = 0;
                    if ((_logN++ % 30) == 0)
                    {
                        uint32_t cnt[7] = {0};
                        uint32_t total = host->GetGeometryDrawCount ? host->GetGeometryDrawCount() : 0;
                        for (uint32_t i = 0; i < total; i++)
                        {
                            DustGeometryDraw gi;
                            if (host->GetGeometryDrawInfo && host->GetGeometryDrawInfo(i, &gi) == 0)
                            { int c = (int)gi.vsCategory; if (c >= 0 && c < 7) cnt[c]++; }
                        }
                        Log("Shadows backface GPU: %.3f ms | %ux%u | replayed=%u | obj=%u terrain=%u foliage=%u skin=%u tri=%u town=%u unknown=%u",
                            ms, gBfWidth, gBfHeight, gBfLastReplayed,
                            cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6], cnt[0]);
                    }
                }
            }
        }
    }
    bool _doTime = (!gBfTsPending && gBfTsDisjoint && gBfTsBegin && gBfTsEnd);

    host->SaveState(dc);

    if (_doTime) { dc->Begin(gBfTsDisjoint); dc->End(gBfTsBegin); }

    // Clear to a tiny back-distance (1.0 euclidean): pixels with no rendered
    // back face (single-sided geometry like terrain/foliage, or empty) then read
    // ~1.0, which light_fs clamps to the flat min-thickness fallback. Solid
    // objects overwrite this with their real back-face euclidean depth.
    float clearRT[4] = { 1, 1, 1, 1 };
    dc->ClearRenderTargetView(gBfRTV, clearRT);
    dc->ClearDepthStencilView(gBfDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
    dc->OMSetRenderTargets(1, &gBfRTV, gBfDSV);

    D3D11_VIEWPORT vpd = { 0.0f, 0.0f, (float)gBfWidth, (float)gBfHeight, 0.0f, 1.0f };
    dc->RSSetViewports(1, &vpd);
    dc->RSSetState(gConfig.contactBackfaceFlip ? gBfRasterFlip : gBfRaster);
    dc->OMSetDepthStencilState(gBfDepthState, 0);
    float bf[4] = { 0, 0, 0, 0 };
    dc->OMSetBlendState(gBfBlend, bf, 0xffffffff);
    dc->PSSetShader(gBfPS, nullptr, 0);
    dc->PSSetConstantBuffers(0, 1, &gBfCB);

    uint32_t replayed = host->ReplayGeometry(dc, ctx->device, vp);
    gBfLastReplayed = replayed;

    if (_doTime) { dc->End(gBfTsEnd); dc->End(gBfTsDisjoint); gBfTsPending = true; }

    host->RestoreState(dc);

    if (sDiagFrames < 5)
    {
        Log("Shadows backface: replayed=%u draws", replayed);
        sDiagFrames++;
    }
}

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

    // Contact-shadow back-face thickness resources (states + PS + CB; textures
    // are created lazily on first use at the live resolution).
    if (!CreateBackfaceStates(device, host))
        Log("Shadows: back-face resources unavailable; contact thickness will use flat fallback");
    UpdateCaptureState();

    Log("Shadows: Initialized (atlas resolution = %u, shadow range = %d)",
        GetSelectedShadowResolution(), gConfig.shadowRange);
    return 0;
}

static void ShadowShutdown()
{
    if (gCB) { gCB->Release(); gCB = nullptr; }
    ReleaseBackfaceAll();
    Log("Shadows: Shut down");
}

static void ShadowOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    // Drop the back-face textures; RunBackfacePass recreates them at the new
    // resolution on the next frame it runs.
    (void)device; (void)w; (void)h;
    ReleaseBackfaceTextures();
}

static void ShadowPreExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gCB) return;

    // Reconcile geometry-capture state every frame: OnSettingChanged may not
    // fire for the Backface toggle under FRAMEWORK_CONFIG, so don't depend on
    // it. No-op (early return) unless the enabled state actually changed.
    UpdateCaptureState();

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

    float atlasRes = (float)GetSelectedShadowResolution();
    if      (atlasRes >= 12288.0f) data.rtwQuality = 4.0f;
    else if (atlasRes >=  8192.0f) data.rtwQuality = 8.0f;
    else                           data.rtwQuality = 12.0f;

    bool backfaceActive = gConfig.contactEnabled && gConfig.contactBackface && gBfPS;
    data.contactEnabled   = gConfig.contactEnabled ? 1.0f : 0.0f;
    data.contactIntensity = gConfig.contactIntensity;
    data.contactSteps     = (float)gConfig.contactSteps;
    data.contactRange     = gConfig.contactRange;
    data.contactThickness = gConfig.contactThickness;
    data.contactBias      = gConfig.contactBias;
    data.contactDebug     = gConfig.contactDebug ? 1.0f : 0.0f;
    data.contactBackface  = backfaceActive ? 1.0f : 0.0f;
    data.contactSoftness  = gConfig.contactSoftness;
    data.contactStepLength = gConfig.contactStepLength;
    data.contactLightRadius = gConfig.contactLightRadius;

    // Render back-face depth BEFORE binding our CB/SRVs: it does its own
    // SaveState/RestoreState, which would otherwise clobber the bindings below.
    if (backfaceActive)
        RunBackfacePass(ctx, host);
    else
        gBfGpuMs = 0.0f;   // perf panel reads 0 when the pass isn't running

    host->UpdateConstantBuffer(ctx->context, gCB, &data, sizeof(data));
    // Bind to b7: b2 collides with CSM's auto-allocated $Globals cbuffer
    // (which holds csmParams arrays). See ShaderPatch.cpp for details.
    ctx->context->PSSetConstantBuffers(7, 1, &gCB);

    // Bind back-face euclidean depth at t10 (+ point sampler at s10) for
    // light_fs. Like b7, left bound so the later light-volume draws can read
    // it. Only sampled when dustContactBackface > 0.5.
    if (backfaceActive && gBfSRV)
    {
        ctx->context->PSSetShaderResources(10, 1, &gBfSRV);
        ctx->context->PSSetSamplers(10, 1, &gBfSampler);
    }
}

static void ShadowPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    // Intentionally do NOT unbind b7 here. POST_LIGHTING wraps only the
    // fullscreen deferred sun pass (main_fs); the point/spot light volumes
    // draw afterwards as separate (undetected) draws, and light_fs reads the
    // contact-shadow params from this same b7 buffer. Unbinding now would
    // leave those draws with a null CB. Nothing else this frame reads b7, and
    // next frame's preExecute rebinds with fresh data, so leaving it bound is
    // harmless.
    (void)ctx; (void)host;
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
    // Start/stop geometry capture to match the back-face thickness toggle.
    UpdateCaptureState();
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

    // === Contact Shadows (point/spot lights) ===
    { "Contact Shadows",     DUST_SETTING_SECTION, nullptr,                 0.0f, 0.0f,    nullptr,            nullptr, nullptr, DUST_PERF_NONE },
    { "Contact Enabled",     DUST_SETTING_BOOL,  &gConfig.contactEnabled,   0.0f, 1.0f,    "ContactEnabled",   nullptr, "Cast screen-space contact shadows from every point and spot light (ENB particle-light style). Ray-marches the depth buffer per light; only on-screen occluders cast.",                  DUST_PERF_HIGH },
    { "Contact Intensity",   DUST_SETTING_FLOAT, &gConfig.contactIntensity, 0.0f, 1.0f,    "ContactIntensity", nullptr, "How dark the contact shadow gets. 0 = invisible, 1 = fully occluded.",                                                                                                                  DUST_PERF_NONE },
    { "Contact Steps",       DUST_SETTING_INT,   &gConfig.contactSteps,     4.0f, 128.0f,  "ContactSteps",     nullptr, "Ray-march samples per pixel per light. Raise for fine steps over full range (reduces light bleeding through thin walls seen edge-on) without shortening the shadow. Costs scale linearly, multiplied across overlapping lights.",          DUST_PERF_HIGH },
    { "Contact Range",       DUST_SETTING_FLOAT, &gConfig.contactRange,     0.0f, 5000.0f, "ContactRange",     nullptr, "Maximum march distance in world units. Contact shadows are short-range by nature; larger values reach further but need more steps to stay smooth.",                                     DUST_PERF_MEDIUM },
    { "Contact Thickness",   DUST_SETTING_FLOAT, &gConfig.contactThickness, 0.0f, 2000.0f, "ContactThickness", nullptr, "How thick an occluder is assumed to be (world units). Too small misses shadows; too large casts shadows through thin geometry.",                                                       DUST_PERF_NONE },
    { "Contact Bias",        DUST_SETTING_FLOAT, &gConfig.contactBias,      0.0f, 50.0f,   "ContactBias",      nullptr, "How far the ray starts off the surface, along the normal (world units). Raise to kill self-shadowing speckle on bumpy ground; lower for tighter contact.",                              DUST_PERF_NONE },
    { "Contact Softness",    DUST_SETTING_FLOAT, &gConfig.contactSoftness,  0.0f, 50.0f,   "ContactSoftness",  nullptr, "Soft-edge penumbra width in world units. 0 = sharp (1 ray). Above 0 averages a 7-ray cone, so it costs ~7x the march — lower Contact Steps if it's too heavy.",                       DUST_PERF_HIGH },
    { "Contact Step Length", DUST_SETTING_FLOAT, &gConfig.contactStepLength, 0.0f, 50.0f,   "ContactStepLength", nullptr, "March step size in world units. 0 = auto (Range / Steps). Set a small value for fine, gap-free shadows near contacts; the ray then reaches at most StepLength x Steps, so raise Steps for more reach.", DUST_PERF_NONE },
    { "Contact Light Radius", DUST_SETTING_FLOAT, &gConfig.contactLightRadius, 0.0f, 200.0f, "ContactLightRadius", nullptr, "Ignore occluders within this distance of the light (world units). Stops a lamp's own fixture mesh from casting a shadow on the floor when the point light sits inside/above its model. Raise until the fake lamp shadow disappears.", DUST_PERF_NONE },
    { "Contact Debug",       DUST_SETTING_BOOL,  &gConfig.contactDebug,     0.0f, 1.0f,    "ContactDebug",     nullptr, "Tint occluded pixels red to verify where contact shadows trigger. Diagnostic only.",                                                                                                    DUST_PERF_NONE },
    { "Backface Thickness",  DUST_SETTING_BOOL,  &gConfig.contactBackface,  0.0f, 1.0f,    "ContactBackface",  nullptr, "Use real per-object thickness from a back-face depth pass instead of the flat Thickness guess. Removes light leak and grain, but adds an extra depth-only geometry pass each frame. Single-sided geometry (terrain) falls back to flat Thickness.", DUST_PERF_HIGH },
    { "Backface Flip Cull",  DUST_SETTING_BOOL,  &gConfig.contactBackfaceFlip, 0.0f, 1.0f, "ContactBackfaceFlip", nullptr, "Flip the back-face culling winding. Toggle this if Backface Thickness makes shadows worse/invisible (the depth pass is capturing front faces instead of back faces).",             DUST_PERF_NONE },
    { "Backface VP Transpose", DUST_SETTING_BOOL, &gConfig.contactBackfaceTranspose, 0.0f, 1.0f, "ContactBackfaceTranspose", nullptr, "Replay matrix convention probe. If Backface Thickness renders wrong, toggle this (and Flip Cull) until it looks right.", DUST_PERF_NONE },
    { "Backface Resolution",  DUST_SETTING_INT,   &gConfig.contactBackfaceScale, 1.0f, 4.0f, "ContactBackfaceScale", nullptr, "Resolution divisor for the back-face depth pass (1 = full, 2 = half, 4 = quarter). Only helps if the pass is pixel-bound; it's usually geometry-bound, so use Backface Interval instead.", DUST_PERF_MEDIUM },
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
    desc->OnResolutionChanged = ShadowOnResolutionChanged;
    desc->preExecute         = ShadowPreExecute;
    desc->postExecute        = ShadowPostExecute;
    desc->IsEnabled          = ShadowIsEnabled;
    desc->settings           = gSettings;
    desc->settingCount       = sizeof(gSettings) / sizeof(gSettings[0]);
    desc->OnSettingChanged   = ShadowOnSettingChanged;
    desc->OnEarlyConfigApply = ShadowEarlyConfigApply;
    // Plugin-managed GPU timing: the back-face pass runs in preExecute, but the
    // framework's auto-timer wraps that in its own disjoint query, which we'd
    // have to nest a query inside (illegal). So we measure the pass ourselves
    // (gBfGpuMs) and expose it via gpuTimeMsPtr instead of FRAMEWORK_TIMING.
    desc->flags              = DUST_FLAG_FRAMEWORK_CONFIG;
    desc->gpuTimeMsPtr       = &gBfGpuMs;
    desc->configSection      = "Shadows";

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);
    return TRUE;
}
