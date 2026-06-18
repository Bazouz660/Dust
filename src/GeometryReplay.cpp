#include "GeometryReplay.h"
#include "GeometryCapture.h"
#include "ShaderDatabase.h"
#include "DustLog.h"
#include "D3D11Hook.h"
#include <tracy/Tracy.hpp>
#include <cstring>
#include <cmath>
#include <d3dcompiler.h>

namespace GeometryReplay
{

static uint32_t sReplaysIssued = 0;

// Characters (SKIN) cast via the captured GBuffer draws — needs 4 VB slots (BLENDINDICES/
// BLENDWEIGHT live in slot 2) and the light VP written TRANSPOSED (the skin VS stores its
// view-proj transposed vs objects). Grass (FOLIAGE) casts via the cameraVP oracle + an alpha
// clip PS + double-sided raster. Both verified 2026-06-11.
// Caster category gates — runtime-settable from the Shadows plugin GUI (productize). The
// cache is rebuilt every frame (BuildCache), so toggling these takes effect next frame.
static bool sBuildingsEnabled = true;   // OBJECTS / DISTANT_TOWN / TRIPLANAR (static world)
static bool sSkinEnabled      = true;   // characters / creatures
static bool sFoliageEnabled   = true;   // grass / vegetation cards

void SetCasterCategories(bool buildings, bool characters, bool foliage)
{
    sBuildingsEnabled = buildings;
    sSkinEnabled      = characters;
    sFoliageEnabled   = foliage;
}

// ---- cameraVP oracle (placement recovery) ------------------------------------------
// Row-major 4x4 multiply: out = a x b (out[r][c] = sum_k a[r][k]*b[k][c]).
static void MatMul(float* out, const float* a, const float* b)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r*4+c] = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c] +
                         a[r*4+2]*b[2*4+c] + a[r*4+3]*b[3*4+c];
}

// Standard row-major 4x4 inverse (double accumulation). Returns false if singular.
static bool Inverse4x4(float* out, const float* m)
{
    double a[16]; for (int i = 0; i < 16; ++i) a[i] = (double)m[i];
    double inv[16];
    inv[0]  =  a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8]  =  a[4]*a[9]*a[15]  - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14]  + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5]  =  a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9]  = -a[0]*a[9]*a[15]  + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] =  a[0]*a[9]*a[14]  - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2]  =  a[1]*a[6]*a[15]  - a[1]*a[7]*a[14]  - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7]  - a[13]*a[3]*a[6];
    inv[6]  = -a[0]*a[6]*a[15]  + a[0]*a[7]*a[14]  + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7]  + a[12]*a[3]*a[6];
    inv[10] =  a[0]*a[5]*a[15]  - a[0]*a[7]*a[13]  - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7]  - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14]  + a[0]*a[6]*a[13]  + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6]  + a[12]*a[2]*a[5];
    inv[3]  = -a[1]*a[6]*a[11]  + a[1]*a[7]*a[10]  + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7]   + a[9]*a[3]*a[6];
    inv[7]  =  a[0]*a[6]*a[11]  - a[0]*a[7]*a[10]  - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7]   - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11]  + a[0]*a[7]*a[9]   + a[4]*a[1]*a[11] - a[4]*a[3]*a[9]  - a[8]*a[1]*a[7]   + a[8]*a[3]*a[5];
    inv[15] =  a[0]*a[5]*a[10]  - a[0]*a[6]*a[9]   - a[4]*a[1]*a[10] + a[4]*a[2]*a[9]  + a[8]*a[1]*a[6]   - a[8]*a[2]*a[5];
    double det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    if (det > -1e-12 && det < 1e-12) return false;
    double idet = 1.0 / det;
    for (int i = 0; i < 16; ++i) out[i] = (float)(inv[i] * idet);
    return true;
}

// Per-frame cameraVP^-1 for placement recovery.
// VALIDATED OFFLINE against bit-exact captured CB bytes (tools/gate*.py, 2026-06-10):
//   cameraVP = inverseView x proj  (plain row-major multiply of the two gCameraData matrices).
// Then per draw, M = capturedClip x cameraVP^-1 is the draw's WORLD matrix:
//   M ~= I        -> PRE-TRANSFORMED (verts already in world space)  -> clip = lightVP
//   M rigid       -> WORLD-PLACED (verts model-local; M places them) -> clip = M x lightVP
//                    (this recovers the cliff/distant meshes that else replay as phantoms)
//   otherwise     -> fall back to PRE (never emit a wild placement).
//
// COHERENCE (kills camera-motion lag): gCameraData is read one frame late, but the captures
// are THIS frame — using the stale VP directly makes buildings drift off identity during a pan
// (misclassified -> jitter). Instead the stale VP is only an ANCHOR: the PRE-TRANSFORMED
// building batch's OWN captured clip IS cameraVP, from the exact same frame. We pick the
// captured clip nearest the anchor (validated margin: building cluster 0.0 vs next 675 units,
// so a frame of motion cannot flip the choice) and use THAT -> buildings are exact identity
// regardless of motion. Falls back to the raw anchor if no captured clip is close (the
// pre-transformed batch isn't in view this frame).
static bool  sCamVPValid = false;
static float sCamVPInv[16];
static float sCamVP[16];      // the chosen coherent camera VP (row-vector convention)

// ---- per-frame replay cache --------------------------------------------------------
// One staging Map + one placement classification per captured draw per FRAME. The
// point-shadow path calls Replay() once per cube face (lights x 6) — without this cache
// every face re-Mapped every staging CB and re-classified every draw (~96x redundant).
// Replay() only patches the 64-byte clip matrix from the cached bytes and draws.
struct CachedEntry
{
    uint32_t           drawIndex;   // index into GeometryCapture::GetCaptures()
    DustShaderCategory cat;
    uint32_t           cbOffset;    // offset of this draw's CB bytes in sCbArena
    uint32_t           cbSize;
    int                place;       // 0 = PRE-TRANSFORMED, 1 = WORLD-PLACED
    float              worldM[16];  // valid when place == 1
};
static std::vector<CachedEntry> sCache;
static std::vector<uint8_t>     sCbArena;
// Public mirror of sCache for RtScene (drawIndex/category/placement/world).
static std::vector<ReplayDrawInfo> sPublicCache;
// Identity stamp of the captures the cache was built from. Replay() rebuilds lazily
// on mismatch — covers the plugin path (HostReplayGeometry) that never calls
// BeginFrame, and frame transitions (generation bumps every ResetFrame).
static const void* sCacheData  = nullptr;
static size_t      sCacheCount = 0;
static uint32_t    sCacheGen   = 0;
static bool        sCacheBuilt = false;

static int ClassifyPlacement(const float* clip, float worldOut[16]);

static void BuildCache(ID3D11DeviceContext* ctx)
{
    ZoneScopedN("GeoReplay.BuildCache");
    const auto& captures = GeometryCapture::GetCaptures();
    sCache.clear();
    sCbArena.clear();
    sCamVPValid = false;
    sCacheData  = captures.data();
    sCacheCount = captures.size();
    sCacheGen   = GeometryCapture::GetGeneration();
    sCacheBuilt = true;
    if (captures.empty()) return;

    float anchor[16];
    bool haveAnchor = false;
    {
        float iv[16], pj[16];
        if (D3D11Hook::GetCameraMatrices(iv, pj)) { MatMul(anchor, iv, pj); haveAnchor = true; }
    }

    // Single Map pass: snapshot each allowed draw's CB bytes into the arena, and track
    // the captured clip nearest the anchor (the coherent same-frame cameraVP — see the
    // COHERENCE comment above). The vote considers ALL classified draws, as before.
    float camVP[16];
    float bestDist = 1e30f;
    for (uint32_t i = 0; i < (uint32_t)captures.size(); ++i)
    {
        const auto& d = captures[i];
        if (!d.vsMetadata || d.vsMetadata->transformType == VSTransformType::UNKNOWN) continue;
        if (!d.cbStagingCopy || d.vsMetadata->clipMatrixOffset + 64 > d.cbStagingSize) continue;

        DustShaderCategory cat = ShaderDatabase::GetVertexShaderCategory(d.vs);
        // OBJECTS/DISTANT_TOWN/TRIPLANAR = static solid occluders. SKIN = characters,
        // FOLIAGE = grass. TERRAIN excluded (vertex-texture fetch; open ground lacks occluders).
        bool isBuilding = cat == DUST_SHADER_OBJECTS || cat == DUST_SHADER_DISTANT_TOWN ||
                          cat == DUST_SHADER_TRIPLANAR;
        bool allow = (isBuilding && sBuildingsEnabled) ||
                     (cat == DUST_SHADER_SKIN && sSkinEnabled) ||
                     (cat == DUST_SHADER_FOLIAGE && sFoliageEnabled);

        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(d.cbStagingCopy, 0, D3D11_MAP_READ, 0, &m))) continue;
        const float* clip = (const float*)((const char*)m.pData + d.vsMetadata->clipMatrixOffset);
        if (haveAnchor)
        {
            float dist = 0.0f;
            for (int k = 0; k < 16; ++k) { float a = clip[k]-anchor[k]; if (a<0) a=-a; if (a>dist) dist=a; }
            if (dist < bestDist) { bestDist = dist; memcpy(camVP, clip, 64); }
        }
        if (allow)
        {
            CachedEntry e = {};
            e.drawIndex = i;
            e.cat      = cat;
            e.cbSize   = d.cbStagingSize;
            e.cbOffset = (uint32_t)sCbArena.size();
            sCbArena.resize(sCbArena.size() + d.cbStagingSize);
            memcpy(sCbArena.data() + e.cbOffset, m.pData, d.cbStagingSize);
            sCache.push_back(e);
        }
        ctx->Unmap(d.cbStagingCopy, 0);
    }

    // No captured clip near the anchor (pre-transformed batch not in view) -> use the raw
    // anchor; placement still correct, just with the minor 1-frame skew.
    if (haveAnchor)
    {
        if (bestDist >= 100.0f) memcpy(camVP, anchor, 64);
        sCamVPValid = Inverse4x4(sCamVPInv, camVP);
        if (sCamVPValid)
            memcpy(sCamVP, camVP, 64);
    }

    // Classify placement once per draw (was: once per draw per cube face). SKIN draws
    // always take the transposed-VP path; "junk" classifications (2) fall back to PRE.
    int nPre = 0, nWorld = 0;
    for (auto& e : sCache)
    {
        if (e.cat == DUST_SHADER_SKIN) { e.place = 0; continue; }
        const float* clip = (const float*)(sCbArena.data() + e.cbOffset +
                                           captures[e.drawIndex].vsMetadata->clipMatrixOffset);
        int place = ClassifyPlacement(clip, e.worldM);
        e.place = (place == 1) ? 1 : 0;
        if (e.place == 1) nWorld++; else nPre++;
    }

    // public mirror for RtScene
    sPublicCache.clear();
    sPublicCache.reserve(sCache.size());
    for (const auto& e : sCache)
    {
        ReplayDrawInfo info;
        info.drawIndex = e.drawIndex;
        info.category  = (int)e.cat;
        info.placement = e.place;
        memcpy(info.worldM, e.worldM, sizeof(info.worldM));
        sPublicCache.push_back(info);
    }

    static int dbg = 0;
    if ((dbg++ % 120) == 0)
        Log("GeoReplay cache: %d/%d draws (PRE=%d WORLD=%d) camVP=%d",
            (int)sCache.size(), (int)captures.size(), nPre, nWorld, (int)sCamVPValid);
}

void BeginFrame(ID3D11DeviceContext* ctx)
{
    BuildCache(ctx);
}

bool GetCameraVP(float out[16])
{
    if (!sCamVPValid || !out) return false;
    memcpy(out, sCamVP, 64);
    return true;
}

// Debug: copy up to maxMats raw clip matrices (+ placement flags) from the
// current frame cache. Returns the number written. For the RT snapshot dump.
uint32_t CopyDebugClips(float* outMats, int* outPlacement, uint32_t maxMats)
{
    uint32_t n = 0;
    for (const auto& e : sCache)
    {
        if (n >= maxMats) break;
        const auto& captures = GeometryCapture::GetCaptures();
        if (e.drawIndex >= captures.size() || !captures[e.drawIndex].vsMetadata) continue;
        const float* clip = (const float*)(sCbArena.data() + e.cbOffset +
                                           captures[e.drawIndex].vsMetadata->clipMatrixOffset);
        memcpy(outMats + n * 16, clip, 64);
        outPlacement[n] = e.place;
        n++;
    }
    return n;
}

bool GetFrameCache(ID3D11DeviceContext* ctx, const ReplayDrawInfo** outInfos,
                   uint32_t* outCount)
{
    *outInfos = nullptr;
    *outCount = 0;
    const auto& captures = GeometryCapture::GetCaptures();
    if (captures.empty())
        return false;
    // same staleness check as Replay() — rebuild covers callers that run before
    // the point-shadow path (or frames where it didn't run at all)
    if (!sCacheBuilt || sCacheData != captures.data() || sCacheCount != captures.size() ||
        sCacheGen != GeometryCapture::GetGeneration())
        BuildCache(ctx);
    *outInfos = sPublicCache.data();
    *outCount = (uint32_t)sPublicCache.size();
    return *outCount > 0;
}

// Classify M = clip x cameraVP^-1. Returns 0=PRE, 1=WORLD, 2=fallback-to-PRE.
static int ClassifyPlacement(const float* clip, float worldOut[16])
{
    if (!sCamVPValid) return 0;   // no oracle -> current behavior (all PRE)
    float M[16];
    MatMul(M, clip, sCamVPInv);
    // Identity test (generous tol absorbs the 1-frame gCameraData/capture skew during motion;
    // PRE vs WORLD margin is huge — world translations are hundreds vs identity's zeros).
    float idres = 0.0f;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        { float t = M[r*4+c] - (r==c ? 1.0f : 0.0f); float a = t<0?-t:t; if (a>idres) idres=a; }
    if (idres < 0.02f) return 0;  // PRE-TRANSFORMED
    // Rigid/affine test: projective column ~0, w ~1, 3x3 rows orthonormal.
    auto af = [](float v){ return v<0?-v:v; };
    if (af(M[3]) > 1e-2f || af(M[7]) > 1e-2f || af(M[11]) > 1e-2f) return 2; // non-affine -> junk
    if (af(M[15]-1.0f) > 1e-2f) return 2;
    for (int r = 0; r < 3; ++r)
    {
        float len2 = M[r*4+0]*M[r*4+0] + M[r*4+1]*M[r*4+1] + M[r*4+2]*M[r*4+2];
        if (af(len2 - 1.0f) > 0.05f) return 2;   // non-unit row -> not a pure rotation
    }
    memcpy(worldOut, M, 64);
    return 1;   // WORLD-PLACED
}

// Alpha-clip PS for FOLIAGE (grass) shadows: samples the grass diffuse (t1) and clips below the
// alpha threshold so leaves cast leaf-shaped depth instead of solid quads. Input signature matches
// grass_vs output (SV_Position/COLOR/TEXCOORD0/TEXCOORD1). Uses an own linear-wrap sampler (s0).
static ID3D11PixelShader*  sAlphaPS    = nullptr;
static ID3D11SamplerState* sAlphaSamp  = nullptr;
static ID3D11RasterizerState* sNoCullRS = nullptr;  // double-sided for grass cards (both faces cast)
static bool                sAlphaFailed = false;

static bool EnsureAlphaPS(ID3D11Device* device)
{
    if (sAlphaPS && sAlphaSamp && sNoCullRS) return true;
    if (sAlphaFailed || !device) return false;
    static const char* kSrc =
        "Texture2D gDiffuse : register(t1);\n"
        "SamplerState gSamp : register(s0);\n"
        "void main(float4 fragCoord : SV_Position, float4 color : COLOR,\n"
        "          float2 texCoord : TEXCOORD0, float4 worldPos : TEXCOORD1) {\n"
        "    clip(gDiffuse.Sample(gSamp, texCoord).a - 0.5);\n"
        "}\n";
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kSrc, strlen(kSrc), "DustGrassAlphaPS", nullptr, nullptr,
                            "main", "ps_5_0", 0, 0, &blob, &err);
    if (FAILED(hr) || !blob)
    {
        if (err) { Log("GrassAlphaPS compile error: %s", (const char*)err->GetBufferPointer()); err->Release(); }
        sAlphaFailed = true; return false;
    }
    if (err) err->Release();
    hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &sAlphaPS);
    blob->Release();
    if (FAILED(hr)) { sAlphaFailed = true; return false; }
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &sAlphaSamp))) { sAlphaFailed = true; return false; }
    D3D11_RASTERIZER_DESC rdsc = {};
    rdsc.FillMode = D3D11_FILL_SOLID; rdsc.CullMode = D3D11_CULL_NONE; rdsc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rdsc, &sNoCullRS))) { sAlphaFailed = true; return false; }
    Log("GrassAlphaPS: compiled foliage alpha-clip shadow PS + double-sided raster state");
    return true;
}

struct ScratchCBEntry
{
    ID3D11Buffer* buffer;
    uint32_t      size;
};
static std::vector<ScratchCBEntry> sScratchCBs;

static ID3D11Buffer* GetScratchCB(ID3D11Device* device, uint32_t requiredSize)
{
    for (auto& entry : sScratchCBs)
    {
        if (entry.size == requiredSize)
            return entry.buffer;
    }

    // DYNAMIC + MAP_WRITE_DISCARD: the same-size scratch CB is rewritten for every
    // replayed draw; DISCARD lets the driver rename the buffer instead of serializing
    // each write against the previous draw's read (UpdateSubresource on DEFAULT did).
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth      = requiredSize;
    desc.Usage           = D3D11_USAGE_DYNAMIC;
    desc.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;

    ID3D11Buffer* buf = nullptr;
    HRESULT hr = device->CreateBuffer(&desc, nullptr, &buf);
    if (FAILED(hr) || !buf)
        return nullptr;

    sScratchCBs.push_back({ buf, requiredSize });
    return buf;
}

// Save/restore IA + VS state around the replay batch.
// Lighter than D3D11StateBlock — only touches what replay modifies.
struct ReplayStateBlock
{
    ID3D11InputLayout*       iaLayout   = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY iaTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer*            vbs[CapturedDraw::MAX_VB_SLOTS] = {};
    UINT                     vbStrides[CapturedDraw::MAX_VB_SLOTS] = {};
    UINT                     vbOffsets[CapturedDraw::MAX_VB_SLOTS] = {};
    ID3D11Buffer*            ib         = nullptr;
    DXGI_FORMAT              ibFormat   = DXGI_FORMAT_R16_UINT;
    UINT                     ibOffset   = 0;
    ID3D11VertexShader*      vs         = nullptr;
    ID3D11Buffer*            vsCBs[CapturedDraw::MAX_VS_CBS] = {};
    ID3D11ShaderResourceView* vsSRVs[CapturedDraw::MAX_VS_SRVS] = {};
    ID3D11SamplerState*      vsSamplers[CapturedDraw::MAX_VS_SAMPLERS] = {};

    void Capture(ID3D11DeviceContext* ctx)
    {
        ctx->IAGetInputLayout(&iaLayout);
        ctx->IAGetPrimitiveTopology(&iaTopology);
        ctx->IAGetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS, vbs, vbStrides, vbOffsets);
        ctx->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->VSGetConstantBuffers(0, CapturedDraw::MAX_VS_CBS, vsCBs);
        ctx->VSGetShaderResources(0, CapturedDraw::MAX_VS_SRVS, vsSRVs);
        ctx->VSGetSamplers(0, CapturedDraw::MAX_VS_SAMPLERS, vsSamplers);
    }

    void Restore(ID3D11DeviceContext* ctx)
    {
        ctx->IASetInputLayout(iaLayout);
        ctx->IASetPrimitiveTopology(iaTopology);
        ctx->IASetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS, vbs, vbStrides, vbOffsets);
        ctx->IASetIndexBuffer(ib, ibFormat, ibOffset);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, CapturedDraw::MAX_VS_CBS, vsCBs);
        ctx->VSSetShaderResources(0, CapturedDraw::MAX_VS_SRVS, vsSRVs);
        ctx->VSSetSamplers(0, CapturedDraw::MAX_VS_SAMPLERS, vsSamplers);
        Release();
    }

    void Release()
    {
        if (iaLayout) { iaLayout->Release(); iaLayout = nullptr; }
        for (UINT i = 0; i < CapturedDraw::MAX_VB_SLOTS; i++)
            if (vbs[i]) { vbs[i]->Release(); vbs[i] = nullptr; }
        if (ib) { ib->Release(); ib = nullptr; }
        if (vs) { vs->Release(); vs = nullptr; }
        for (UINT i = 0; i < CapturedDraw::MAX_VS_CBS; i++)
            if (vsCBs[i]) { vsCBs[i]->Release(); vsCBs[i] = nullptr; }
        for (UINT i = 0; i < CapturedDraw::MAX_VS_SRVS; i++)
            if (vsSRVs[i]) { vsSRVs[i]->Release(); vsSRVs[i] = nullptr; }
        for (UINT i = 0; i < CapturedDraw::MAX_VS_SAMPLERS; i++)
            if (vsSamplers[i]) { vsSamplers[i]->Release(); vsSamplers[i] = nullptr; }
    }
};

uint32_t Replay(ID3D11DeviceContext* ctx, ID3D11Device* device,
                const float* replacementVP,
                const float* replacementVPSkin,
                const float* cullCenter, float cullRadius)
{
    ZoneScopedN("GeoReplay.Replay");
    const auto& captures = GeometryCapture::GetCaptures();
    if (captures.empty())
        return 0;
    if (!replacementVPSkin) replacementVPSkin = replacementVP;

    // Per-light occluder cull (conservative sphere test). When cullCenter != nullptr,
    // a WORLD-PLACED draw (place==1) whose recovered world-matrix translation is farther
    // than cullRadius from cullCenter cannot affect this light, so skip it. worldM is the
    // local->world row-vector matrix (MatMul(wvp, worldM, VP) below), so the draw's world
    // CENTER is the translation row -> worldM[12..14]. cullCenter and worldM live in the
    // SAME render-world-absolute space: the caller builds the face VP via LookAtLH at the
    // light's absPos (= rebased + camera), and the captured GBuffer geometry is render-world
    // absolute too. PRE-TRANSFORMED / SKIN draws (place==0) have no cheap per-draw center,
    // so they are NEVER culled (conservative: a missed cull costs perf; a wrong cull leaks
    // light). cullRadius already includes the caller's object-extent margin.
    const bool  cullActive = (cullCenter != nullptr) && (cullRadius > 0.0f);
    const float cullR2     = cullRadius * cullRadius;
    uint32_t    culledDraws = 0;

    // Cache stale (plugin path without BeginFrame, or captures changed) -> rebuild.
    if (!sCacheBuilt || sCacheData != captures.data() || sCacheCount != captures.size() ||
        sCacheGen != GeometryCapture::GetGeneration())
        BuildCache(ctx);
    if (sCache.empty())
        return 0;

    ReplayStateBlock saved;
    saved.Capture(ctx);
    // The caller's rasterizer state (cull-back) — restore it for non-foliage draws and at the end.
    ID3D11RasterizerState* savedRS = nullptr;
    ctx->RSGetState(&savedRS);

    // SKINNED: clip = viewProjectionMatrix (no world); the bone palette in the same CB
    // transforms local verts -> world. The skin VS stores its VP TRANSPOSED vs the objects
    // VS (measured: skinVP == cameraVP^T) — transpose once per face, not per draw.
    float vpSkinT[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            vpSkinT[r*4+c] = replacementVPSkin[c*4+r];

    uint32_t replayed = 0;

    for (const auto& e : sCache)
    {
        // Conservative per-light cull: only WORLD-PLACED draws have a known center.
        if (cullActive && e.place == 1)
        {
            float dx = e.worldM[12] - cullCenter[0];
            float dy = e.worldM[13] - cullCenter[1];
            float dz = e.worldM[14] - cullCenter[2];
            if (dx*dx + dy*dy + dz*dz > cullR2) { culledDraws++; continue; }
        }

        const CapturedDraw& draw = captures[e.drawIndex];
        const VSConstantBufferInfo& meta = *draw.vsMetadata;
        DustShaderCategory cat = e.cat;

        ID3D11Buffer* scratchCB = GetScratchCB(device, e.cbSize);
        if (!scratchCB)
            continue;

        // Per-face clip matrix: PRE-TRANSFORMED draws take the bare light view-proj;
        // WORLD-PLACED draws compose their cached recovered world matrix with it.
        const float* patch;
        float wvp[16];
        if (cat == DUST_SHADER_SKIN)
            patch = vpSkinT;
        else if (e.place == 1)
        {
            MatMul(wvp, e.worldM, replacementVP);   // local -> world -> light clip
            patch = wvp;
        }
        else
            patch = replacementVP;

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(ctx->Map(scratchCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            continue;
        memcpy(mapped.pData, sCbArena.data() + e.cbOffset, e.cbSize);
        memcpy((char*)mapped.pData + meta.clipMatrixOffset, patch, 64);
        ctx->Unmap(scratchCB, 0);

        // Set IA state (both VB slots — slot 1 has instance data for instanced draws)
        ctx->IASetInputLayout(draw.inputLayout);
        ctx->IASetPrimitiveTopology(draw.topology);
        // Bind ALL captured VB slots (skin uses 4: pos/normal, uv/tangent, BLENDINDICES/WEIGHT,
        // uv2). Slot 1 is per-instance transforms for instanced static draws -> use the per-draw
        // snapshot there (the live buffer is recycled/stale by replay time).
        ID3D11Buffer* vbs[CapturedDraw::MAX_VB_SLOTS];
        for (UINT s = 0; s < CapturedDraw::MAX_VB_SLOTS; ++s) vbs[s] = draw.vertexBuffers[s];
        if (draw.instVBCopy) vbs[1] = draw.instVBCopy;
        ctx->IASetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS, vbs,
                                draw.vbStrides, draw.vbOffsets);
        ctx->IASetIndexBuffer(draw.indexBuffer, draw.indexFormat, draw.ibOffset);

        // Set VS and constant buffers
        ctx->VSSetShader(draw.vs, nullptr, 0);
        for (UINT i = 0; i < CapturedDraw::MAX_VS_CBS; i++)
        {
            if (i == meta.cbSlot)
                ctx->VSSetConstantBuffers(i, 1, &scratchCB);
            else
            {
                // Prefer the per-draw bindable snapshot (skinned bone palette); the live
                // vsCBs pointer is stale by replay time for skinned draws.
                ID3D11Buffer* cb = draw.cbCopies[i] ? draw.cbCopies[i] : draw.vsCBs[i];
                if (cb) ctx->VSSetConstantBuffers(i, 1, &cb);
            }
        }
        // Rebind VS-stage resources so skin/terrain VS fetches (bone palette,
        // heightmap, vertex-fetch) don't read unbound SRVs and GPU-fault.
        ctx->VSSetShaderResources(0, CapturedDraw::MAX_VS_SRVS, draw.vsSRVs);
        ctx->VSSetSamplers(0, CapturedDraw::MAX_VS_SAMPLERS, draw.vsSamplers);

        // FOLIAGE: alpha-clip PS (grass diffuse @ t1) so leaves cast leaf-shaped depth, and render
        // DOUBLE-SIDED so grass cards facing away from the light still cast. All other draws stay
        // depth-only (null PS) with the caller's cull-back state.
        if (cat == DUST_SHADER_FOLIAGE && EnsureAlphaPS(device))
        {
            if (draw.psSRVs[1])
            {
                ctx->PSSetShader(sAlphaPS, nullptr, 0);
                ctx->PSSetShaderResources(1, 1, &draw.psSRVs[1]);
                ctx->PSSetSamplers(0, 1, &sAlphaSamp);
            }
            else
                ctx->PSSetShader(nullptr, nullptr, 0);
            ctx->RSSetState(sNoCullRS);
        }
        else
        {
            ctx->PSSetShader(nullptr, nullptr, 0);
            ctx->RSSetState(savedRS);
        }

        if (draw.instanceCount > 1)
        {
            ctx->DrawIndexedInstanced(draw.indexCount, draw.instanceCount,
                                      draw.startIndexLocation, draw.baseVertexLocation,
                                      draw.startInstanceLocation);
        }
        else
        {
            ctx->DrawIndexed(draw.indexCount, draw.startIndexLocation,
                             draw.baseVertexLocation);
        }

        replayed++;
    }

    // Reset the PS + the foliage diffuse SRV we bound (depth-only default), so the alpha PS and
    // grass texture don't leak into the game's subsequent rendering. Restore the raster state too.
    ctx->PSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
    ctx->PSSetShaderResources(1, 1, nullSRV);
    ctx->RSSetState(savedRS);
    if (savedRS) savedRS->Release();

    saved.Restore(ctx);

    if (sReplaysIssued < 3 && replayed > 0)
    {
        Log("GeometryReplay: replayed %u / %u draws", replayed, (uint32_t)captures.size());
        sReplaysIssued++;
    }

    // Throttled cull report so the user can confirm per-light culling is active.
    // "culled X/Y" = X world-placed draws skipped out of Y total cached this face.
    if (cullActive)
    {
        static int sCullLog = 0;
        if ((sCullLog++ % 240) == 0)
            Log("GeometryReplay: culled %u/%u draws this face (radius=%.0f)",
                culledDraws, (uint32_t)sCache.size(), cullRadius);
    }

    return replayed;
}

void Shutdown()
{
    sCache.clear();
    sCbArena.clear();
    sCacheBuilt = false;
    for (auto& entry : sScratchCBs)
        if (entry.buffer) entry.buffer->Release();
    sScratchCBs.clear();
    if (sAlphaPS)   { sAlphaPS->Release();   sAlphaPS = nullptr; }
    if (sAlphaSamp) { sAlphaSamp->Release(); sAlphaSamp = nullptr; }
    if (sNoCullRS)  { sNoCullRS->Release();  sNoCullRS = nullptr; }
}

} // namespace GeometryReplay
