#include "D3D11Hook.h"
#include "DustGUI.h"
#include "PipelineDetector.h"
#include "EffectLoader.h"
#include "ResourceRegistry.h"
#include "Survey.h"
#include "SurveyRecorder.h"
#include "SurveyWriter.h"
#include "ShaderPatch.h"
#include "ShaderMetadata.h"
#include "GeometryCapture.h"
#include "MotionVectors.h"
#include "Upscaler.h"
#include "UpscalerFSR2.h"
#include "UpscalerFSR3.h"
#include "D3D12Interop.h"
#include "CameraAccess.h"
#include "DustLog.h"
#include <cmath>
#include <core/Functions.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <string>
#include <cstring>
#include <vector>
#include <atomic>
#include <unordered_set>
#include <mutex>

namespace D3D11Hook
{

// ==================== Global state ====================

ID3D11Device* gDevice = nullptr;
ID3D11DeviceContext* gContext = nullptr;
bool gDeviceCaptured = false;

static UINT gWidth = 0;
static UINT gHeight = 0;
static uint64_t gFrameIndex = 0;
static bool gDispatchedThisFrame = false;
static bool gPostLightVolumesFired = false;  // v5 postLightVolumes: once per frame

// Light-volume direct AO (defined below, near the CreatePixelShader hook).
static void ReleaseLightVolumeAO();
void SetLightVolumeAoTexture(ID3D11ShaderResourceView* srv);
void SetLightVolumeSsaoAo(ID3D11ShaderResourceView* ao, ID3D11ShaderResourceView* params);

// Camera extraction from the game's deferred lighting CB
// (staging CBs are in gCameraStagingCBs[], declared near ExtractCameraData)
static DustCameraData gCameraData = {};
static bool gCameraDataExtracted = false; // per-frame flag

// VTable indices for swap chain methods (used by both Install() and deferred hooking)
static const int VTIDX_SC_Present        = 8;
static const int VTIDX_SC_ResizeBuffers  = 13;
static const int VTIDX_SC1_Present1      = 22;

// Deferred Present hooking: addresses saved from temp device, installed later
static void* sSavedAddrPresent   = nullptr;
static void* sSavedAddrPresent1  = nullptr;
static void* sSavedAddrResizeBuf = nullptr;
static bool  sSwapChainHooked    = false;

// Survey: collected frame data for writing after all frames captured
static std::vector<SurveyFrameData> sSurveyFrames;

static bool gDeviceRemovedThisFrame = false;
static volatile bool gShutdownSignaled = false;

// ==================== Shadow atlas resolution override ====================

// Plugin-supplied override for the shadow atlas dimension (square). 0 = no
// override. The game always creates its atlas at vanilla size; when the
// override differs, replacement textures are created at the requested size
// and swapped in at bind time (OM/PS/RS hooks below). This survives shadow
// type switches: every matching atlas creation re-queues the resize.
static UINT gShadowAtlasOverride = 0;
static constexpr size_t kMaxShadowIdentities = 8;

// Forward declaration — the remaining hook function pointers live below.
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateTexture2D)(
    ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
    const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static PFN_CreateTexture2D oCreateTexture2D = nullptr;

// DLSS texture MIP-LOD bias: DLSS/DLAA supersamples, so NVIDIA's guide prescribes biasing texture
// sampling by log2(render/display) - 1 = -1.0 for DLAA. gDlssWanted (read from the ini at device
// capture) gates a CreateSamplerState hook that adds this to every sampler.
// gMipBiasSharpen ([Upscaling] MipBiasSharpen, default 1) is a kill-switch for that hook — it
// applies the bias to EVERY sampler in the process, including the icon pipeline's (whose materials
// already carry a native -0.5 bias), which is a suspect for icon-texture aliasing.
static bool  gDlssWanted   = false;
static bool  gMipBiasSharpen = true;
static const float kDlssMipBias = -1.0f;
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSamplerState)(
    ID3D11Device*, const D3D11_SAMPLER_DESC*, ID3D11SamplerState**);
static PFN_CreateSamplerState oCreateSamplerState = nullptr;

UINT GetShadowAtlasResolution()          { return gShadowAtlasOverride; }

// Runtime shadow atlas resize state. Each entry tracks one shadow atlas
// texture (color or depth) created by the game, plus an optional replacement
// texture/views at the user's chosen resolution. Swap happens at bind time.
struct ShadowAtlasEntry {
    ID3D11Texture2D*          tex;          // AddRef'd original
    IUnknown*                 identity;     // QI(IID_IUnknown) for matching
    D3D11_TEXTURE2D_DESC      desc;         // original desc (vanilla size)
    bool                      isDepth;      // DSV-bound depth vs RTV+SRV color
    uint64_t                  lastSeen;     // gFrameIndex of the last bind/sample
                                            // that resolved to this entry
    ID3D11Texture2D*          newTex;       // replacement (null if no resize active)
    UINT                      newSize;      // replacement dimension (square)
    float                     vpScale;      // newSize / desc.Width (1.0 = no swap)
    ID3D11DepthStencilView*   newDSV;
    ID3D11RenderTargetView*   newRTV;
    ID3D11ShaderResourceView* newSRV;
    IUnknown*                 newIdentity;  // weak; newTex's identity (backed by the
                                            // newTex ref) — lets the bind classifier
                                            // recognize our own replacements
    ID3D11Texture2D*          companionDepthTex;  // standalone depth for color-only entries
    ID3D11DepthStencilView*   companionDSV;       // DSV for companion depth
};
static ShadowAtlasEntry gShadowEntries[kMaxShadowIdentities] = {};
static std::atomic<size_t> gShadowEntryCount{0};
static UINT  gShadowVanillaSize = 0;           // atlas size of the live generation
static bool  gShadowSwapActive = false;        // fast-path flag: any replacement exists
static float gShadowPassScale = 1.0f;          // viewport scale of the current shadow pass
static std::atomic<bool> gShadowResizePending{false};
static UINT  gShadowResizeTarget = 0;          // 0 = dismantle all replacements
static uint64_t gShadowCreationFrame = ~0ull;
static bool  gInShadowPass = false;

UINT GetShadowBaseResolution()           { return gShadowVanillaSize; }

void SetShadowAtlasResolution(UINT size)
{
    gShadowAtlasOverride = size;
    size_t entries = gShadowEntryCount.load(std::memory_order_acquire);
    if (entries == 0) return;

    // Queue only when some entry's replacement state doesn't match the
    // request (this fires per GUI tick — must not spam the log or the
    // resize path). 0 clears the override: all replacements get dismantled.
    bool change = false;
    for (size_t i = 0; i < entries; i++)
    {
        const ShadowAtlasEntry& e = gShadowEntries[i];
        UINT want = (size != 0 && size != e.desc.Width) ? size : 0;
        UINT have = e.newTex ? e.newSize : 0;
        if (want != have) { change = true; break; }
    }
    if (!change) return;

    Log("Shadow atlas resize queued: target=%u (entries=%zu)", size, entries);
    gShadowResizeTarget = size;
    gShadowResizePending.store(true, std::memory_order_release);
}

// Find the ShadowAtlasEntry whose original texture matches this resource's
// IUnknown identity (the only pointer COM guarantees stable across interface
// queries, even if a wrapper layer sits between us and the real texture).
// Returns index or -1. Caller must Release res afterwards (this function
// does NOT consume the ref). Stamps lastSeen: entries the game stops
// touching go stale and become evictable (see EvictStaleShadowEntries).
static int FindShadowEntry(ID3D11Resource* res)
{
    if (!res) return -1;
    IUnknown* unk = nullptr;
    res->QueryInterface(IID_IUnknown, (void**)&unk);
    if (!unk) return -1;
    size_t count = gShadowEntryCount.load(std::memory_order_acquire);
    int found = -1;
    for (size_t i = 0; i < count; i++)
    {
        if (gShadowEntries[i].identity == unk)
        {
            gShadowEntries[i].lastSeen = gFrameIndex;
            found = (int)i;
            break;
        }
    }
    unk->Release();
    return found;
}

static void ReleaseShadowReplacement(ShadowAtlasEntry& e)
{
    e.newIdentity = nullptr;
    e.newSize = 0;
    e.vpScale = 1.0f;
    if (e.companionDSV)      { e.companionDSV->Release();      e.companionDSV = nullptr; }
    if (e.companionDepthTex) { e.companionDepthTex->Release();  e.companionDepthTex = nullptr; }
    if (e.newDSV)  { e.newDSV->Release();  e.newDSV = nullptr; }
    if (e.newRTV)  { e.newRTV->Release();  e.newRTV = nullptr; }
    if (e.newSRV)  { e.newSRV->Release();  e.newSRV = nullptr; }
    if (e.newTex)  { e.newTex->Release();  e.newTex = nullptr; }
}

static void ResetShadowTracking()
{
    size_t count = gShadowEntryCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; i++)
    {
        ReleaseShadowReplacement(gShadowEntries[i]);
        if (gShadowEntries[i].tex) { gShadowEntries[i].tex->Release(); gShadowEntries[i].tex = nullptr; }
    }
    gShadowEntryCount.store(0, std::memory_order_release);
    gShadowVanillaSize = 0;
    gShadowSwapActive = false;
    gShadowPassScale = 1.0f;
    gShadowResizeTarget = 0;
    gShadowResizePending.store(false, std::memory_order_relaxed);
    gInShadowPass = false;
    Log("Shadow tracking reset (workspace recreate, frame %llu)", (unsigned long long)gFrameIndex);
}

static void ApplyPendingShadowResize()
{
    if (!gShadowResizePending.load(std::memory_order_acquire))
        return;

    UINT newSize = gShadowResizeTarget;
    size_t count = gShadowEntryCount.load(std::memory_order_acquire);
    if (count == 0)
    {
        gShadowResizePending.store(false, std::memory_order_relaxed);
        return;
    }
    // The atlas can be created before the first draw captures the device
    // (between LoadAll and InitAll) — keep the request pending until then.
    if (!gDevice)
        return;
    gShadowResizePending.store(false, std::memory_order_relaxed);

    // Per-entry replacement: the tracked entries can span two atlas
    // generations of DIFFERENT vanilla sizes (RTW 1024 vs CSM 2048, both
    // alive across a shadow-mode switch), so each entry gets its own
    // replacement + viewport scale. An entry already matching the target
    // (either vanilla-sized == target, or an up-to-date replacement) is
    // left alone — re-queues after a mode switch must not churn the live
    // replacements. Target 0 dismantles everything (override cleared).
    for (size_t i = 0; i < count; i++)
    {
        ShadowAtlasEntry& e = gShadowEntries[i];
        UINT want = (newSize != 0 && newSize != e.desc.Width) ? newSize : 0;
        if (want == 0)
        {
            if (e.newTex)
            {
                Log("Shadow atlas entry %zu (%s %u): replacement dismantled",
                    i, e.isDepth ? "depth" : "color", e.desc.Width);
                ReleaseShadowReplacement(e);
            }
            continue;
        }
        if (e.newTex && e.newSize == want)
            continue;  // already correct

        ReleaseShadowReplacement(e);

        D3D11_TEXTURE2D_DESC d = e.desc;
        d.Width  = want;
        d.Height = want;

        HRESULT hr = oCreateTexture2D(gDevice, &d, nullptr, &e.newTex);
        if (FAILED(hr) || !e.newTex)
        {
            Log("  entry %zu: CreateTexture2D FAILED (0x%08X)", i, hr);
            continue;
        }

        if (e.isDepth)
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
            dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dsvd.Format = (d.Format == DXGI_FORMAT_R32_TYPELESS)
                        ? DXGI_FORMAT_D32_FLOAT : d.Format;
            hr = gDevice->CreateDepthStencilView(e.newTex, &dsvd, &e.newDSV);
            if (FAILED(hr))
                Log("  entry %zu: CreateDSV FAILED (0x%08X)", i, hr);

            // R32_TYPELESS can also be sampled (as R32_FLOAT) — the deferred
            // lighting pass binds the depth atlas SRV at slot 5 in CSM mode.
            if (d.Format == DXGI_FORMAT_R32_TYPELESS &&
                (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0)
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
                srvd.Format = DXGI_FORMAT_R32_FLOAT;
                srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvd.Texture2D.MipLevels = 1;
                hr = gDevice->CreateShaderResourceView(e.newTex, &srvd, &e.newSRV);
                if (FAILED(hr))
                    Log("  entry %zu: CreateSRV (depth) FAILED (0x%08X)", i, hr);
            }
        }
        else
        {
            D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
            rtvd.Format = d.Format;
            rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            hr = gDevice->CreateRenderTargetView(e.newTex, &rtvd, &e.newRTV);
            if (FAILED(hr))
                Log("  entry %zu: CreateRTV FAILED (0x%08X)", i, hr);

            D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
            srvd.Format = d.Format;
            srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvd.Texture2D.MipLevels = 1;
            hr = gDevice->CreateShaderResourceView(e.newTex, &srvd, &e.newSRV);
            if (FAILED(hr))
                Log("  entry %zu: CreateSRV FAILED (0x%08X)", i, hr);
        }

        // Remember the replacement's identity (weak, backed by the newTex
        // ref) so the bind classifier recognizes re-binds of our own
        // replacement views and never adopts them as "originals".
        IUnknown* nid = nullptr;
        e.newTex->QueryInterface(IID_IUnknown, (void**)&nid);
        if (nid) { e.newIdentity = nid; nid->Release(); }

        e.newSize = want;
        e.vpScale = (float)want / (float)e.desc.Width;

        Log("  entry %zu (%s %u -> %u, scale %.3f): tex=%p DSV=%p RTV=%p SRV=%p",
            i, e.isDepth ? "depth" : "color", e.desc.Width, want, e.vpScale,
            e.newTex, e.newDSV, e.newRTV, e.newSRV);
    }

    // If no depth entry was tracked (OGRE reused a pooled depth buffer),
    // create a companion depth texture for color entries so the RTV+DSV
    // dimensions match during shadow rendering.
    bool hasDepth = false;
    for (size_t i = 0; i < count; i++)
        if (gShadowEntries[i].isDepth) { hasDepth = true; break; }
    if (!hasDepth)
    {
        for (size_t i = 0; i < count; i++)
        {
            ShadowAtlasEntry& e = gShadowEntries[i];
            if (e.isDepth || !e.newTex || e.companionDSV) continue;
            D3D11_TEXTURE2D_DESC dd = {};
            dd.Width = e.newSize;
            dd.Height = e.newSize;
            dd.MipLevels = 1;
            dd.ArraySize = 1;
            dd.Format = DXGI_FORMAT_D32_FLOAT;
            dd.SampleDesc.Count = 1;
            dd.Usage = D3D11_USAGE_DEFAULT;
            dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            HRESULT hr = oCreateTexture2D(gDevice, &dd, nullptr, &e.companionDepthTex);
            if (SUCCEEDED(hr) && e.companionDepthTex)
            {
                hr = gDevice->CreateDepthStencilView(e.companionDepthTex, nullptr, &e.companionDSV);
                if (FAILED(hr))
                    Log("  companion DSV creation FAILED (0x%08X)", hr);
                else
                    Log("  companion DSV created for color entry %zu (%ux%u)", i, e.newSize, e.newSize);
            }
        }
    }

    bool anyReplacement = false;
    for (size_t i = 0; i < count; i++)
        if (gShadowEntries[i].newTex) { anyReplacement = true; break; }
    if (gShadowSwapActive != anyReplacement)
        Log("Shadow atlas swap %s", anyReplacement ? "active" : "disabled");
    gShadowSwapActive = anyReplacement;
    if (!anyReplacement)
    {
        gShadowPassScale = 1.0f;
        gInShadowPass = false;
    }
}

// The game clears its own (original) atlas views each frame; our replacements
// would accumulate stale contents, so clear them at frame start instead.
static void ClearShadowReplacements()
{
    if (!gShadowSwapActive || !gContext) return;
    size_t count = gShadowEntryCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; i++)
    {
        ShadowAtlasEntry& e = gShadowEntries[i];
        if (e.isDepth && e.newDSV)
            gContext->ClearDepthStencilView(e.newDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
        if (!e.isDepth && e.newRTV)
        {
            float clear[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            gContext->ClearRenderTargetView(e.newRTV, clear);
        }
        if (e.companionDSV)
            gContext->ClearDepthStencilView(e.companionDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
    }
}

// ===================== Temporal sub-pixel jitter (for DLSS/FSR upscaling) =====================
// When enabled, the main GBuffer scene viewport is offset by a Halton(2,3) sub-pixel amount each
// frame so the temporal upscaler accumulates new sub-pixel samples. Applied at the VIEWPORT (post
// clip-space), so our injected motion vectors — computed from clip-space cur/prev in the VS — stay
// jitter-free, which is exactly the upscaler input contract. Recovered from the TSSAA stash@{0}.
static bool  gJitterEnabled = false;
static float gJitterPxX = 0.0f;   // this frame's jitter, in pixels [-0.5, 0.5]
static float gJitterPxY = 0.0f;
static int   sUpsBackend = 0;     // 0 = DLSS, 1 = FSR2 (declared early so the jitter advance can pick FSR2's sequence)

// Forward scene passes (sky/atmosphere, water, transparents, particles) bind the SAME depth buffer as
// the GBuffer but render AFTER it — and were never viewport-jittered. The upscaler jitter-compensates
// the whole frame, so unjittered content wobbles by the jitter amplitude (±0.5px) in the output —
// most visible as "jitter" on thin far silhouettes against the sky/fog. Track those passes (set in the
// OMSet hooks: non-GBuffer bind whose DSV == this frame's GBuffer DSV) and jitter their viewports too.
// Compositor QUADS inside such passes (fog/tonemap; image-space) are un-jittered per-draw in HookedDraw.
static bool  gFwdScenePass = false;
// [Upscaling] JitterForwardPasses — default OFF (2026-07-20): some UI elements render inside a
// pass that binds the scene depth buffer, and the per-draw quad un-jitter doesn't catch them all,
// so the jittered viewport makes the UI visibly wobble. The cost of OFF is the original artifact
// this flag existed to fix: thin far silhouettes against sky/fog wobble by the jitter amplitude.
static bool  gJitterForwardPasses = false;

// Dust-side rendering (effect dispatches, MV/upscaler fullscreen passes, ImGui) sets its own
// viewports through the hooked context. Those must NEVER be jittered — only OGRE's own pass
// viewports carry the sub-pixel offset. Leaking jitter into Dust's POST_* passes half-pixel-shifted
// the sky-MV fill and the depth-convert output every frame -> DLSS/FSR got misaligned inputs ->
// visibly blurry image (in-game 2026-07-14). Counter, not bool: effect fullscreen draws re-enter
// HookedDraw and nest scopes.
static thread_local int tSuppressVpJitter = 0;
struct VpJitterSuppressScope
{
    VpJitterSuppressScope()  { ++tSuppressVpJitter; }
    ~VpJitterSuppressScope() { --tSuppressVpJitter; }
};

static float HaltonSeq(uint32_t i, uint32_t b)
{
    float f = 1.0f, r = 0.0f;
    while (i > 0) { f /= (float)b; r += f * (float)(i % b); i /= b; }
    return r;
}

void SetTemporalJitter(int enabled)
{
    gJitterEnabled = (enabled != 0);
    if (!gJitterEnabled) gJitterPxX = gJitterPxY = 0.0f;
}

// The jitter applied to the frame currently being built (pixels). Read by the upscaler resolve.
void GetTemporalJitter(float& x, float& y) { x = gJitterPxX; y = gJitterPxY; }

static void EvictStaleShadowEntries();  // defined with the bind classifier below

void ResetFrameState()
{
    EvictStaleShadowEntries();
    ApplyPendingShadowResize();
    ClearShadowReplacements();
    gPipelineDetector.ResetFrame();
    GeometryCapture::ResetFrame();   // release last frame's captured draws (MV pass)
    gResourceRegistry.ResetFrame();
    ReleaseLightVolumeAO();
    SetLightVolumeAoTexture(nullptr);        // clear RTGI's per-light AO; republished each frame
    SetLightVolumeSsaoAo(nullptr, nullptr);  // clear SSAO's per-light AO; republished each frame
    gDispatchedThisFrame = false;
    gPostLightVolumesFired = false;
    gCameraDataExtracted = false;
    gDeviceRemovedThisFrame = false;
    gFrameIndex++;

    // Advance the per-frame sub-pixel jitter. FSR2's temporal accumulation is tuned to its own jitter
    // sequence, so use it when FSR2 is the active backend; otherwise Halton(2,3) (fine for DLSS).
    if (gJitterEnabled)
    {
        if (sUpsBackend == 1 && gWidth > 0)
        {
            UpscalerFSR2::ComputeJitter(gFrameIndex, gWidth, gWidth, &gJitterPxX, &gJitterPxY);
        }
        else
        {
            uint32_t i = (uint32_t)(gFrameIndex % 16) + 1;
            gJitterPxX = HaltonSeq(i, 2) - 0.5f;
            gJitterPxY = HaltonSeq(i, 3) - 0.5f;
        }
    }
    else
    {
        gJitterPxX = gJitterPxY = 0.0f;
    }
}

// ===================== DLSS/FSR upscaling resolve (Milestone B: DLAA at native res) =====================
// At POST_TONEMAP (scene tonemapped, before UI) hand DLSS the color + depth + our injected motion-vector
// RT, and copy its anti-aliased output back over the scene color so the UI + present use it. Gated on
// [Upscaling] DLSS=1; degrades to a no-op if NGX/DLSS isn't available (non-NVIDIA / old driver / no dll).
static bool             sUpsConfigRead = false;
static bool             sUpsEnabled    = false;   // runtime DLSS on/off (seeded from [Upscaling] DLSS)
static int              sUpsPresetIdx  = 3;        // Upscaler::PRESET_F (CNN DLAA; K blurs the static image in our D3D11 NGX integration)
static float            sUpsSharpness  = 0.0f;     // [0,1]
static bool             sUpsDepthConvert = true;   // [Upscaling] DepthConvert: linear game depth -> device-Z
static int              sUpsActiveBackend = -1;    // currently-initialized backend (detect GUI switch)
static bool             sUpsInitTried  = false;    // NGX availability probed
static int              sUpsFeaturePreset = -1;    // preset the live feature was created with (-1 = none)
static bool             sUpsJitterOn   = false;    // tracked so we toggle jitter cleanly on enable/disable
// [Upscaling] TemporalJitter (default 1): kill-switch for the per-frame sub-pixel viewport jitter.
// (Added while chasing the quadrant-coloured icons; the jitter was CLEARED — the real culprit was
// the MV-injection gate flipping between sessions, see MvInjectionWanted/BuildCacheStamp. The icon
// workspace GBuffer is icon-sized, so it never classifies as the scene GBuffer and is never
// jittered. Kept as a debug lever for temporal artifacts.)
static bool             gTemporalJitterWanted = true;
static bool             sUpsResetNext  = false;    // discard DLSS history on the next frame (enable/recreate)
static ID3D11Texture2D*          sUpsOut    = nullptr; // DLSS output (UAV); copied/sharpened back to scene color
static ID3D11ShaderResourceView* sUpsOutSRV = nullptr; // SRV of sUpsOut, for the sharpen pass to sample
static uint32_t         sUpsOutW = 0, sUpsOutH = 0;
static DXGI_FORMAT      sUpsOutFmt = DXGI_FORMAT_UNKNOWN;   // format sUpsOut was created with
static const void*               sUpsPrevColorRTV = nullptr; // detect pipeline rebuild (settings change)
static ID3D11Texture2D*          sUpsColorSR  = nullptr; // shader-readable copy of the scene color for NGX
static uint32_t         sUpsColorW = 0, sUpsColorH = 0;
static DXGI_FORMAT      sUpsColorFmt = DXGI_FORMAT_UNKNOWN; // format sUpsColorSR was created with
static bool             sUpsDescLogged = false;

// Runtime gate for the injected motion-vector feeder (velocity RT + patched-shader SV_Target3 bind +
// reproj/clear pass + skin/geometry tracking). The feeder only earns its per-frame cost when something
// consumes velocity: the upscaler resolve, or the MV debug overlay. RunUpscaler recomputes this once
// per frame (POST_TONEMAP); the per-draw hooks (which run earlier, i.e. on the NEXT frame) read it as a
// single cheap bool. So a toggle takes one frame to fully engage/disengage — harmless. When it's off,
// the feeder does nothing and holds no VRAM, so a disabled upscaler is (near) vanilla cost.
static bool             sMvFeederActive = false;
static bool             gCaptureGeometryWanted = true;   // [Upscaling] CaptureGeometry (default ON) — applied only while the feeder is active

// Width/height of a resource (0/false if not a Texture2D). Used to skip the resolve when the color,
// depth and MV inputs momentarily disagree (e.g. mid-pipeline-rebuild) instead of freezing the image.
static bool TexDim(ID3D11Resource* res, uint32_t& w, uint32_t& h)
{
    if (!res) return false;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t) return false;
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); t->Release();
    w = d.Width; h = d.Height; return true;
}

static void ReleaseUpscalerTargets()   // drop derived textures so they rebuild against the new pipeline
{
    if (sUpsOutSRV) { sUpsOutSRV->Release(); sUpsOutSRV = nullptr; }
    if (sUpsOut)    { sUpsOut->Release();    sUpsOut = nullptr; }    sUpsOutW = sUpsOutH = 0; sUpsOutFmt = DXGI_FORMAT_UNKNOWN;
    if (sUpsColorSR){ sUpsColorSR->Release();sUpsColorSR = nullptr; } sUpsColorW = sUpsColorH = 0; sUpsColorFmt = DXGI_FORMAT_UNKNOWN;
}

// One-shot diagnostic: log a resource's format + bind flags (NGX PlatformError is usually a bind-flag gap).
static void LogTexDesc(const char* label, ID3D11Resource* res)
{
    if (!res) { Log("Upscaler[desc] %s = NULL", label); return; }
    ID3D11Texture2D* t = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t)
    {
        D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); t->Release();
        Log("Upscaler[desc] %s: %ux%u fmt=%u bind=0x%x samples=%u", label, d.Width, d.Height,
            (unsigned)d.Format, (unsigned)d.BindFlags, d.SampleDesc.Count);
    }
    else Log("Upscaler[desc] %s = not a Texture2D", label);
}

// Ensure an SHADER_RESOURCE-bindable texture matching src's format/size; returns it after copying src in.
static ID3D11Texture2D* EnsureColorSR(ID3D11DeviceContext* ctx, ID3D11Resource* colorRes)
{
    ID3D11Texture2D* colorTex = nullptr;
    if (FAILED(colorRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&colorTex)) || !colorTex) return nullptr;
    D3D11_TEXTURE2D_DESC cd; colorTex->GetDesc(&cd); colorTex->Release();
    if (!sUpsColorSR || sUpsColorW != cd.Width || sUpsColorH != cd.Height || sUpsColorFmt != cd.Format)
    {
        if (sUpsColorSR) { sUpsColorSR->Release(); sUpsColorSR = nullptr; }
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = cd.Width; sd.Height = cd.Height; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Format = cd.Format; sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_DEFAULT;
        sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(gDevice->CreateTexture2D(&sd, nullptr, &sUpsColorSR))) { sUpsColorSR = nullptr; return nullptr; }
        sUpsColorW = cd.Width; sUpsColorH = cd.Height; sUpsColorFmt = cd.Format;
    }
    ctx->CopyResource(sUpsColorSR, colorRes);   // shader-readable snapshot NGX can bind as its color SRV
    return sUpsColorSR;
}

// Create/resize the DLSS output texture to match the scene color target (display res for DLAA).
static ID3D11Texture2D* EnsureUpscalerOutput(ID3D11Resource* colorRes)
{
    ID3D11Texture2D* colorTex = nullptr;
    if (FAILED(colorRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&colorTex)) || !colorTex) return nullptr;
    D3D11_TEXTURE2D_DESC cd; colorTex->GetDesc(&cd); colorTex->Release();
    if (sUpsOut && sUpsOutW == cd.Width && sUpsOutH == cd.Height && sUpsOutFmt == cd.Format) return sUpsOut;
    if (sUpsOutSRV) { sUpsOutSRV->Release(); sUpsOutSRV = nullptr; }
    if (sUpsOut) { sUpsOut->Release(); sUpsOut = nullptr; }
    D3D11_TEXTURE2D_DESC od = {};
    od.Width = cd.Width; od.Height = cd.Height; od.MipLevels = 1; od.ArraySize = 1;
    od.Format = cd.Format; od.SampleDesc.Count = 1; od.Usage = D3D11_USAGE_DEFAULT;
    od.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;   // DLSS writes via compute UAV
    if (FAILED(gDevice->CreateTexture2D(&od, nullptr, &sUpsOut))) { sUpsOut = nullptr; return nullptr; }
    gDevice->CreateShaderResourceView(sUpsOut, nullptr, &sUpsOutSRV);          // for the sharpen pass
    sUpsOutW = cd.Width; sUpsOutH = cd.Height; sUpsOutFmt = cd.Format;
    Log("Upscaler: output texture %ux%u created", cd.Width, cd.Height);
    return sUpsOut;
}

static void RunUpscaler(ID3D11DeviceContext* ctx)
{
    if (!sUpsConfigRead)
    {
        sUpsConfigRead = true;
        std::string iniPath = DustLogDir() + "Dust.ini";
        sUpsEnabled   = GetPrivateProfileIntA("Upscaling", "DLSS", 0, iniPath.c_str()) != 0;   // "enabled"
        sUpsPresetIdx = GetPrivateProfileIntA("Upscaling", "DLSSPreset", 3, iniPath.c_str());          // 3 = F (K blurs static image)
        sUpsSharpness = GetPrivateProfileIntA("Upscaling", "DLSSSharpness", 0, iniPath.c_str()) / 100.0f;
        sUpsBackend   = GetPrivateProfileIntA("Upscaling", "Backend", 0, iniPath.c_str());     // 0=DLSS 1=FSR2 2=FSR3
        sUpsDepthConvert     = GetPrivateProfileIntA("Upscaling", "DepthConvert", 1, iniPath.c_str()) != 0;
        gJitterForwardPasses = GetPrivateProfileIntA("Upscaling", "JitterForwardPasses", 0, iniPath.c_str()) != 0;
        gTemporalJitterWanted = GetPrivateProfileIntA("Upscaling", "TemporalJitter", 1, iniPath.c_str()) != 0;
    }

    // Decide whether the MV feeder should run this frame, and drive its cost from that. This runs before
    // every early-out below, so the gate stays correct even when the resolve itself is skipped. Applying
    // the capture flag every frame is a trivial static write and survives a device/resolution reinit.
    const bool mvNeeded = sUpsEnabled || MotionVectors::DebugVizEnabled();
    GeometryCapture::SetCaptureFlags(mvNeeded && gCaptureGeometryWanted ? DUST_CAPTURE_GEOMETRY : 0);
    if (mvNeeded != sMvFeederActive)
    {
        sMvFeederActive = mvNeeded;
        if (!mvNeeded) MotionVectors::ReleaseInjectionTargets();   // free the velocity RT + drop stale cross-frame state
        Log("MotionVectors: injected-MV feeder %s (upscaler=%d, debugViz=%d)",
            mvNeeded ? "ON" : "OFF", (int)sUpsEnabled, (int)MotionVectors::DebugVizEnabled());
    }

    if (!gDevice || gWidth == 0 || gHeight == 0) return;

    const int backend = sUpsBackend;   // 0=DLSS 1=FSR2 2=FSR3/FSR4 (D3D12 side-device)

    // Backend switched in the GUI: tear the old one down, force re-init + feature recreate + jitter reset.
    if (sUpsActiveBackend != sUpsBackend)
    {
        Upscaler::Shutdown();
        UpscalerFSR2::Shutdown();
        UpscalerFSR3::Shutdown();
        ReleaseUpscalerTargets();
        sUpsActiveBackend = sUpsBackend;
        sUpsInitTried     = false;
        sUpsFeaturePreset = -1;
        if (sUpsJitterOn) { SetTemporalJitter(0); sUpsJitterOn = false; }
    }

    // Probe/init the active backend once (so the GUI can report availability before it's switched on).
    if (!sUpsInitTried)
    {
        sUpsInitTried = true;
        std::string modDir = DustLogDir(); std::wstring wdir(modDir.begin(), modDir.end());
        if      (backend == 1) UpscalerFSR2::Init(gDevice);
        else if (backend == 2) UpscalerFSR3::Init(gDevice, wdir.c_str());
        else                   Upscaler::Init(gDevice, wdir.c_str(), wdir.c_str());
    }

    const bool avail = backend == 1 ? UpscalerFSR2::IsAvailable()
                     : backend == 2 ? UpscalerFSR3::IsAvailable()
                     :                 Upscaler::IsAvailable();

    // Disabled or unavailable: ensure jitter is off (no shimmer) and skip the resolve.
    if (!sUpsEnabled || !avail)
    {
        if (sUpsJitterOn) { SetTemporalJitter(0); sUpsJitterOn = false; }
        return;
    }

    // (Re)create the feature on first enable or when the preset changed in the GUI.
    if (sUpsFeaturePreset != sUpsPresetIdx)
    {
        bool created = backend == 1 ? UpscalerFSR2::CreateFeature(ctx, gWidth, gHeight, gWidth, gHeight, false, false, sUpsPresetIdx)
                     : backend == 2 ? UpscalerFSR3::CreateFeature(ctx, gWidth, gHeight, gWidth, gHeight, false, false, sUpsPresetIdx)
                     :                 Upscaler::CreateFeature(ctx, gWidth, gHeight, gWidth, gHeight, false, false, sUpsPresetIdx);
        if (created) { sUpsFeaturePreset = sUpsPresetIdx; sUpsResetNext = true; }
        else { sUpsEnabled = false; if (sUpsJitterOn) { SetTemporalJitter(0); sUpsJitterOn = false; } return; }
    }
    if (!sUpsJitterOn) { SetTemporalJitter(gTemporalJitterWanted ? 1 : 0); sUpsJitterOn = true; sUpsResetNext = true; }  // fresh history

    ID3D11RenderTargetView*   colorRTV = gResourceRegistry.GetRTV(ResourceName::LDR_RTV);
    ID3D11ShaderResourceView* depthSRV = gResourceRegistry.GetSRV(ResourceName::DEPTH_SRV);
    ID3D11Resource*           mv       = MotionVectors::GetInjVelResource();
    if (!colorRTV || !depthSRV || !mv) return;

    // Pipeline-rebuild robustness: a vanilla settings change (FXAA, HeatHaze, ...) rebuilds OGRE's
    // compositor and re-registers new render targets. If our color target changed, drop the derived
    // textures (they rebuild against the new one) and reset DLSS history — otherwise we resolve stale
    // content / blend a dead history, which froze the image.
    if ((const void*)colorRTV != sUpsPrevColorRTV)
    {
        sUpsPrevColorRTV = (const void*)colorRTV;
        ReleaseUpscalerTargets();
        sUpsResetNext = true;
        Log("Upscaler: color target changed (pipeline rebuild) — reset DLSS history + textures");
    }

    ID3D11Resource* color = nullptr; colorRTV->GetResource(&color);
    ID3D11Resource* depth = nullptr; depthSRV->GetResource(&depth);

    // Inputs must agree in size; if not (e.g. one frame mid-rebuild), skip so the game's own image shows
    // through instead of freezing on a stale resolve.
    uint32_t cw=0,ch=0, dw=0,dh=0, vw=0,vh=0;
    bool dimsOk = TexDim(color, cw, ch) && TexDim(depth, dw, dh) && TexDim(mv, vw, vh) &&
                  cw == dw && ch == dh && cw == vw && ch == vh;
    if (color && depth && dimsOk)
    {
        ID3D11Texture2D* out   = EnsureUpscalerOutput(color);
        ID3D11Texture2D* colSR = EnsureColorSR(ctx, color);   // NGX needs a shader-readable color input
        if (!sUpsDescLogged)
        {
            sUpsDescLogged = true;
            LogTexDesc("color(ldr_rt)", color);
            LogTexDesc("depth",         depth);
            LogTexDesc("motionVec",     mv);
            LogTexDesc("output",        out);
            LogTexDesc("colorSRcopy",   colSR);
        }
        if (out && colSR)
        {
            // Fill camera-only MV for the sky/far-depth pixels (the sky is a forward pass with no MV
            // injection, so it's velocity 0 -> ghosts under camera motion). Must run here, after the
            // scene + sky are drawn and before the upscaler reads the velocity buffer.
            //
            // The fill keys off DEPTH ALONE (d >= 0.999), and writes a far-plane camera reprojection —
            // a velocity that varies smoothly with screen position. Any geometry whose pixels read as
            // far-depth (very distant meshes, or alpha-BLENDED foliage drawn in the forward pass, which
            // never writes into the velocity RT) therefore gets the sky's position-varying field
            // stamped over it. [Upscaling] SkyMV=0 disables the fill so that can be A/B'd directly.
            static int sSkyMvEnabled = -1;
            if (sSkyMvEnabled < 0)
            {
                sSkyMvEnabled = GetPrivateProfileIntA("Upscaling", "SkyMV", 1,
                                                      (DustLogDir() + "Dust.ini").c_str()) ? 1 : 0;
                Log("MotionVectors: sky MV fill %s ([Upscaling] SkyMV)", sSkyMvEnabled ? "ON" : "OFF");
            }
            if (sSkyMvEnabled)
                MotionVectors::InjFillSkyMV(ctx, depthSRV, gWidth, gHeight);
            // The game's depth is LINEAR (ray distance / farClip); the upscalers expect hyperbolic
            // device-Z and (FSR) un-project it with cameraNear/Far for disocclusion. Feeding linear
            // depth makes the reconstructed view-Z hypersensitive at distance -> history rejection ->
            // shimmer/jitter on far geometry. Convert; fall back to raw depth if the pass fails.
            ID3D11Resource* upsDepth = depth;
            if (sUpsDepthConvert)
            {
                float camN = 0.1f, camF = 10000.0f, camFov = 1.0f;
                CameraAccess_GetCameraParams(&camN, &camF, &camFov);
                ID3D11Resource* conv = MotionVectors::ConvertDepthForUpscaler(
                    ctx, depthSRV, gWidth, gHeight, camN, camF, camFov);
                if (conv) upsDepth = conv;
            }
            float jx, jy; GetTemporalJitter(jx, jy);
            // Our MV texel is (curUV - prevUV) in UV space. The upscaler wants (prevUV - curUV) in render
            // pixels (the vector from a pixel to where it was last frame), so MV_Scale is NEGATIVE display
            // size. Wrong sign => history reprojected backwards => ghosting on camera motion + softness.
            // Sharpness goes to our own RCAS-lite pass, so pass 0 to the upscaler.
            const float mvsx = -(float)gWidth, mvsy = -(float)gHeight;
            bool ok = backend == 1 ? UpscalerFSR2::Evaluate(ctx, colSR, upsDepth, mv, out, jx, jy, mvsx, mvsy, 0.0f, sUpsResetNext, nullptr)
                    : backend == 2 ? UpscalerFSR3::Evaluate(ctx, colSR, upsDepth, mv, out, jx, jy, mvsx, mvsy, 0.0f, sUpsResetNext, nullptr)
                    :                 Upscaler::Evaluate(ctx, colSR, upsDepth, mv, out, jx, jy, mvsx, mvsy, 0.0f, sUpsResetNext, nullptr);
            if (ok)
            {
                if (sUpsSharpness > 0.0f && sUpsOutSRV)
                    MotionVectors::SharpenBlit(ctx, sUpsOutSRV, colorRTV, sUpsSharpness, gWidth, gHeight);
                else
                    ctx->CopyResource(color, out);   // AA'd image back into the scene color for UI + present
            }
            sUpsResetNext = false;
        }
    }
    if (color) color->Release();
    if (depth) depth->Release();
}


// D.0 FSR3/FSR4 interop spike (the de-risking gate). FSR 3.1 / FSR 4 are DX12-only, so they need a
// D3D12 side-device fed by shared textures. Before building any of that, prove the bridge round-trips
// the scene color through a D3D12 compute pass. Gated by [Upscaling] D3D12InteropSpike=1 (off by
// default). When on, the LEFT HALF of the screen turns green; a clean split == device + shared-texture
// + shared-fence sync + compute dispatch all work. If the D3D12 side never inits (old OS / Proton
// without the interop), IsReady() stays false and this is a silent no-op.
static bool sInteropSpikeRead = false;
static bool sInteropSpike     = false;
static bool sInteropInitTried = false;

static void RunInteropSpike(ID3D11DeviceContext* ctx)
{
    if (!sInteropSpikeRead)
    {
        sInteropSpikeRead = true;
        std::string iniPath = DustLogDir() + "Dust.ini";
        sInteropSpike = GetPrivateProfileIntA("Upscaling", "D3D12InteropSpike", 0, iniPath.c_str()) != 0;
    }
    if (!sInteropSpike || !gDevice || gWidth == 0 || gHeight == 0) return;

    if (!sInteropInitTried) { sInteropInitTried = true; D3D12Interop::Init(gDevice); }
    if (!D3D12Interop::IsReady()) return;

    ID3D11RenderTargetView* colorRTV = gResourceRegistry.GetRTV(ResourceName::LDR_RTV);
    if (!colorRTV) return;
    ID3D11Resource* color = nullptr; colorRTV->GetResource(&color);
    if (color) { D3D12Interop::RunTintTest(ctx, color, gWidth, gHeight); color->Release(); }
}


// Extract camera basis vectors from the game's deferred lighting PS constant buffer.
// The inverse view matrix sits at register c8 (offset 128 bytes / 32 floats).
// Camera axes are the COLUMNS of the inverse view matrix (= rows of the view matrix).
// Double-buffered staging: CopyResource into slot N this frame, Map slot N-1
// (which the GPU finished long ago). Eliminates the CPU-GPU sync stall that
// was blocking the pipeline every frame.
static ID3D11Buffer* gCameraStagingCBs[2] = {};
static int gCameraStagingSlot = 0;
static bool gCameraStagingReady = false; // false until first copy has been issued

static void ExtractCameraData(ID3D11DeviceContext* ctx)
{
    if (gCameraDataExtracted) return;

    ID3D11Buffer* psCB = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &psCB);
    if (!psCB) return;

    D3D11_BUFFER_DESC cbDesc;
    psCB->GetDesc(&cbDesc);
    if (cbDesc.ByteWidth < 192) { psCB->Release(); return; }

    if (gCameraStagingCBs[0])
    {
        D3D11_BUFFER_DESC stagingDesc;
        gCameraStagingCBs[0]->GetDesc(&stagingDesc);
        if (stagingDesc.ByteWidth != cbDesc.ByteWidth)
        {
            gCameraStagingCBs[0]->Release();
            gCameraStagingCBs[1]->Release();
            gCameraStagingCBs[0] = nullptr;
            gCameraStagingCBs[1] = nullptr;
            gCameraStagingReady = false;
        }
    }

    if (!gCameraStagingCBs[0])
    {
        D3D11_BUFFER_DESC sd = cbDesc;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.BindFlags = 0;
        sd.MiscFlags = 0;
        gDevice->CreateBuffer(&sd, nullptr, &gCameraStagingCBs[0]);
        gDevice->CreateBuffer(&sd, nullptr, &gCameraStagingCBs[1]);
        if (!gCameraStagingCBs[0] || !gCameraStagingCBs[1])
        {
            // Partial failure: drop BOTH slots so the next frame retries creation from a
            // clean state (a lone slot 0 would gate this branch off forever and leak).
            if (gCameraStagingCBs[0]) { gCameraStagingCBs[0]->Release(); gCameraStagingCBs[0] = nullptr; }
            if (gCameraStagingCBs[1]) { gCameraStagingCBs[1]->Release(); gCameraStagingCBs[1] = nullptr; }
            gCameraStagingReady = false;
            psCB->Release();
            return;
        }
    }
    if (!gCameraStagingCBs[0] || !gCameraStagingCBs[1]) { psCB->Release(); return; }

    int writeSlot = gCameraStagingSlot;
    int readSlot = 1 - writeSlot;

    // Copy this frame's CB into the write slot (async, no stall)
    ctx->CopyResource(gCameraStagingCBs[writeSlot], psCB);
    psCB->Release();

    // Read LAST frame's data from the other slot (GPU finished it long ago)
    if (gCameraStagingReady)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(gCameraStagingCBs[readSlot], 0, D3D11_MAP_READ, 0, &mapped)))
        {
            float m[16];
            memcpy(m, (float*)mapped.pData + 32, 64); // c8 offset
            ctx->Unmap(gCameraStagingCBs[readSlot], 0);

            bool valid = true;
            for (int i = 0; i < 16; i++)
                if (!isfinite(m[i])) { valid = false; break; }

            if (valid)
            {
                memcpy(gCameraData.inverseView, m, 64);
                gCameraData.camRight[0]   = m[0]; gCameraData.camRight[1]   = m[4]; gCameraData.camRight[2]   = m[8];
                gCameraData.camUp[0]      = m[1]; gCameraData.camUp[1]      = m[5]; gCameraData.camUp[2]      = m[9];
                gCameraData.camForward[0] = m[2]; gCameraData.camForward[1] = m[6]; gCameraData.camForward[2] = m[10];
                gCameraData.camPosition[0] = m[12]; gCameraData.camPosition[1] = m[13]; gCameraData.camPosition[2] = m[14];
                // Projection params (API v8) from the OGRE camera — real near/far/fov for effects (RTGI)
                // and the upscaler. Left 0 if the camera walk isn't reachable this frame.
                float camN = 0.0f, camF = 0.0f, camFov = 0.0f;
                if (CameraAccess_GetCameraParams(&camN, &camF, &camFov))
                {
                    gCameraData.nearZ = camN;
                    gCameraData.farZ  = camF;
                    gCameraData.tanHalfFov = tanf(camFov * 0.5f);
                }
                gCameraData.valid = 1;
                gCameraDataExtracted = true;
            }
        }
    }

    gCameraStagingSlot = readSlot;
    gCameraStagingReady = true;
}

// ==================== Original function pointers ====================
// (PFN_CreateTexture2D / oCreateTexture2D are declared earlier — the shadow
// atlas resize machinery needs them to create replacement textures without
// re-entering the hook.)

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreatePixelShader)(
    ID3D11Device* pThis, const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader);

typedef void(STDMETHODCALLTYPE* PFN_Draw)(
    ID3D11DeviceContext* pThis, UINT VertexCount, UINT StartVertexLocation);

typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(
    ID3D11DeviceContext* pThis, UINT IndexCount, UINT StartIndexLocation,
    INT BaseVertexLocation);

typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(
    ID3D11DeviceContext* pThis, UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation);

// Non-indexed instanced. Hooked ONLY so the injected-MV reproj CB stays pinned at b13 for geometry
// that comes through this path — without it the patched VS reads whatever OGRE left at b13 and the
// velocity degenerates into a function of position instead of a frame delta.
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(
    ID3D11DeviceContext* pThis, UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation);

typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(
    ID3D11DeviceContext* pThis, UINT NumViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView);

typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargetsAndUAV)(
    ID3D11DeviceContext* pThis,
    UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts);

typedef void(STDMETHODCALLTYPE* PFN_PSSetShaderResources)(
    ID3D11DeviceContext* pThis, UINT StartSlot, UINT NumViews,
    ID3D11ShaderResourceView* const* ppShaderResourceViews);

typedef void(STDMETHODCALLTYPE* PFN_RSSetViewports)(
    ID3D11DeviceContext* pThis, UINT NumViewports,
    const D3D11_VIEWPORT* pViewports);

typedef HRESULT(STDMETHODCALLTYPE* PFN_Map)(
    ID3D11DeviceContext* pThis, ID3D11Resource* pResource, UINT Subresource,
    D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource);

typedef void(STDMETHODCALLTYPE* PFN_Unmap)(
    ID3D11DeviceContext* pThis, ID3D11Resource* pResource, UINT Subresource);

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(
    IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags);

typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(
    IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags);

static PFN_CreatePixelShader        oCreatePixelShader = nullptr;
static PFN_Draw                     oDraw = nullptr;
static PFN_DrawIndexed              oDrawIndexed = nullptr;
static PFN_DrawIndexedInstanced     oDrawIndexedInstanced = nullptr;
static PFN_DrawInstanced            oDrawInstanced = nullptr;
static PFN_OMSetRenderTargets       oOMSetRenderTargets = nullptr;
static PFN_OMSetRenderTargetsAndUAV oOMSetRenderTargetsAndUAV = nullptr;
static PFN_PSSetShaderResources     oPSSetShaderResources = nullptr;
static PFN_RSSetViewports           oRSSetViewports = nullptr;
static PFN_Map                      oMap = nullptr;
static PFN_Unmap                    oUnmap = nullptr;
static PFN_Present                  oPresent = nullptr;
static PFN_ResizeBuffers            oResizeBuffers = nullptr;

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(
    IDXGISwapChain1* pThis, UINT SyncInterval, UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS* pPresentParameters);
static PFN_Present1                 oPresent1 = nullptr;

// Hook-race diagnostics: if Present is bypassed by an overlay or a Present1 path,
// Draw fires but Present never does. These let us confirm that from the log.
static uint64_t gDrawHookCallCount = 0;
static uint64_t gPresentHookCallCount = 0;
static bool gGuiInitDone = false;

// ==================== D3DCompile hook (runtime shader patching) ====================
// Implementation moved to ShaderPatch.{h,cpp}.

// ==================== DustBoot integration ====================
// DustBoot is a preload plugin that hooks IDXGIFactory::CreateSwapChain before
// the game creates its D3D11 device. If present, it provides the swap chain pointer
// directly — no runtime discovery needed.

typedef IDXGISwapChain* (*PFN_DustBoot_GetSwapChain)();
typedef HWND            (*PFN_DustBoot_GetHWND)();
typedef bool            (*PFN_DustBoot_IsHooked)();

static bool sTryCaptureFromBoot = false; // true if DustBoot provided the swap chain

static IDXGISwapChain* TryGetSwapChainFromBoot()
{
    // Called per-draw — throttle all failure messages to avoid log spam.
    static int sFailLogCount = 0;

    HMODULE boot = GetModuleHandleA("DustBoot.dll");
    if (!boot)
    {
        if (sFailLogCount < 1)
        { Log("DustBoot: not loaded (preload plugin not installed)"); ++sFailLogCount; }
        return nullptr;
    }

    auto isHooked = (PFN_DustBoot_IsHooked)GetProcAddress(boot, "DustBoot_IsHooked");
    if (!isHooked || !isHooked())
    {
        if (sFailLogCount < 3)
        { Log("DustBoot: loaded but factory hooks not active"); ++sFailLogCount; }
        return nullptr;
    }

    auto getSC = (PFN_DustBoot_GetSwapChain)GetProcAddress(boot, "DustBoot_GetSwapChain");
    if (!getSC)
    {
        if (sFailLogCount < 1)
        { Log("DustBoot: export DustBoot_GetSwapChain not found"); ++sFailLogCount; }
        return nullptr;
    }

    IDXGISwapChain* sc = getSC();
    if (!sc)
    {
        if (sFailLogCount < 3)
        { Log("DustBoot: hooked but swap chain not captured yet"); ++sFailLogCount; }
        return nullptr;
    }

    Log("DustBoot: swap chain captured at %p", sc);
    return sc;
}

// ==================== Deferred swap chain hooking ====================

// Forward declarations for hooks defined later (needed by TryInstallSwapChainHooks)
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags);
static HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* pThis, UINT SyncInterval,
    UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* pThis, UINT BufferCount,
    UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

// Try to find the game's real swap chain by walking DXGI from the current
// render target. If the RT is the swap chain's back buffer, IDXGISurface::GetParent
// returns the swap chain. Falls back gracefully if the RT is an intermediate buffer.
// If ctx is null, falls back to gContext.
static bool TryDiscoverSwapChain(IDXGISwapChain** ppSwapChain, ID3D11DeviceContext* ctx = nullptr)
{
    *ppSwapChain = nullptr;

    if (!ctx) ctx = gContext;
    if (!ctx) return false;

    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv)
        return false;

    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    rtv->Release();
    if (!res)
        return false;

    IDXGISurface* surface = nullptr;
    HRESULT hr = res->QueryInterface(__uuidof(IDXGISurface), (void**)&surface);
    res->Release();
    if (FAILED(hr) || !surface)
        return false;

    hr = surface->GetParent(__uuidof(IDXGISwapChain), (void**)ppSwapChain);
    surface->Release();

    if (FAILED(hr) || !*ppSwapChain)
        return false;

    Log("SwapChain discovery: found real swap chain at %p", *ppSwapChain);
    return true;
}

// VTable hook: directly replace function pointer in the COM object's vtable.
// Immune to inline hook conflicts from overlays (Steam, Discord, ReShade).
static bool VTableHook(void* pObject, int vtableIndex, void* detour, void** original)
{
    void** vtable = *reinterpret_cast<void***>(pObject);

    // Already hooked (e.g. TryRecoverPresent re-running TryInstallSwapChainHooks on a chain
    // whose vtable still points at our detours): leave the slot and the saved original alone.
    // Overwriting *original with our own detour here would recurse until stack overflow.
    if (vtable[vtableIndex] == detour)
        return true;

    *original = vtable[vtableIndex];

    DWORD oldProtect;
    if (!VirtualProtect(&vtable[vtableIndex], sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
    vtable[vtableIndex] = detour;
    VirtualProtect(&vtable[vtableIndex], sizeof(void*), oldProtect, &oldProtect);
    return true;
}

// The swap chain pointer we vtable-hooked — needed for recovery verification
static IDXGISwapChain* gHookedSwapChain = nullptr;

// Game-present identification (see the comment block above IsGamePresent, near the Present hooks):
// the game's present = a TOP-LEVEL Present on the game's RENDER THREAD, on the GUI's home chain.
static DWORD gGameRenderThreadId = 0;            // set at confirmed-pipeline device capture (HookedDraw)
static thread_local int tPresentDepth = 0;       // our Present-hook recursion depth on this thread
static IDXGISwapChain* gGuiSwapChain = nullptr;  // chain the GUI renders into (weak, compare-only)

static void TryInstallSwapChainHooks(ID3D11DeviceContext* drawCtx = nullptr)
{
    if (sSwapChainHooked)
        return;

    // Layer 1: Try DustBoot (preload plugin that intercepted CreateSwapChain)
    IDXGISwapChain* realSwapChain = TryGetSwapChainFromBoot();
    if (realSwapChain)
    {
        sTryCaptureFromBoot = true;
        Log("Using swap chain from DustBoot (preload capture)");
    }
    else
    {
        // Layer 2: Fall back to runtime discovery from current render target.
        // Uses drawCtx if provided (any draw call), otherwise needs gContext from device capture.
        ID3D11DeviceContext* ctx = drawCtx ? drawCtx : gContext;
        if (!ctx)
            return;
        if (!TryDiscoverSwapChain(&realSwapChain, ctx) || !realSwapChain)
            return;
        Log("Using swap chain from runtime discovery (fallback)");
    }

    bool ok = true;

    // VTable hook Present (index 8)
    if (!VTableHook(realSwapChain, VTIDX_SC_Present, (void*)HookedPresent, (void**)&oPresent))
    { Log("ERROR: Failed to vtable-hook Present"); ok = false; }
    else
    { Log("  Present vtable-hooked on swap chain %p", realSwapChain); }

    // VTable hook ResizeBuffers (index 13)
    if (!VTableHook(realSwapChain, VTIDX_SC_ResizeBuffers, (void*)HookedResizeBuffers, (void**)&oResizeBuffers))
    { Log("ERROR: Failed to vtable-hook ResizeBuffers"); ok = false; }
    else
    { Log("  ResizeBuffers vtable-hooked on swap chain %p", realSwapChain); }

    // VTable hook Present1 (index 22 on IDXGISwapChain1)
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(realSwapChain->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1)
    {
        if (!VTableHook(sc1, VTIDX_SC1_Present1, (void*)HookedPresent1, (void**)&oPresent1))
        { Log("WARNING: Failed to vtable-hook Present1"); }
        else
        { Log("  Present1 vtable-hooked"); }
        sc1->Release();
    }

    gHookedSwapChain = realSwapChain;
    // Both paths hold a reference: discovery via GetParent AddRef, DustBoot via explicit AddRef.
    realSwapChain->Release();
    sSwapChainHooked = true;

    if (ok)
        Log("All swap chain hooks installed successfully (via %s)",
            sTryCaptureFromBoot ? "DustBoot preload" : "runtime discovery");
    else
        Log("WARNING: Some swap chain hooks failed — GUI may not work");
}

// ==================== Present hook diagnostics ====================

bool IsPresentHooked()
{
    return sSwapChainHooked && gPresentHookCallCount > 0;
}

void TryRecoverPresent()
{
    if (gPresentHookCallCount > 0)
        return; // Already working

    Log("RECOVER: Attempting swap chain capture (DustBoot → discovery → vtable re-patch)...");
    sSwapChainHooked = false;
    TryInstallSwapChainHooks();

    if (!sSwapChainHooked)
    {
        Log("RECOVER: Re-hook failed. GUI will not be available this session. Effects still work.");
    }
}

// ==================== Device capture ====================

static void TryCaptureDevice(ID3D11Device* device)
{
    if (gDeviceCaptured)
        return;

    gDeviceCaptured = true;

    gDevice = device;
    device->GetImmediateContext(&gContext);

    Log("Captured real D3D11 device=%p, context=%p", gDevice, gContext);

    // Try to get resolution from current RT
    {
        ID3D11RenderTargetView* rtv = nullptr;
        gContext->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv)
        {
            ID3D11Resource* res = nullptr;
            rtv->GetResource(&res);
            if (res)
            {
                ID3D11Texture2D* tex = nullptr;
                res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
                if (tex)
                {
                    D3D11_TEXTURE2D_DESC desc;
                    tex->GetDesc(&desc);
                    gWidth = desc.Width;
                    gHeight = desc.Height;
                    tex->Release();
                }
                res->Release();
            }
            rtv->Release();
        }
    }

    if (gWidth == 0 || gHeight == 0)
    {
        gWidth = 1;
        gHeight = 1;
        Log("Resolution not yet available, will detect from HDR RT on first frame");
    }
    else
    {
        Log("Detected resolution: %ux%u", gWidth, gHeight);
    }

    // Motion-vector pass (upscaling): hand the device + GBuffer resolution to the
    // geometry-capture layer. [Upscaling] CaptureGeometry — default ON (2026-07-20): without it,
    // skinned characters get camera-only reprojection under DLSS/FSR (the ghosting the true-MV
    // feature exists to kill) with no UI hint why. The capture flag is driven per-frame by the MV
    // feeder, so it costs nothing unless the upscaler (or MV debug viz) is actually on.
    GeometryCapture::SetDevice(gDevice);
    GeometryCapture::SetResolution(gWidth, gHeight);
    MotionVectors::SetDevice(gDevice);
    MotionVectors::OnResolution(gWidth, gHeight);
    {
        std::string iniPath = DustLogDir() + "Dust.ini";
        gCaptureGeometryWanted = GetPrivateProfileIntA("Upscaling", "CaptureGeometry", 1, iniPath.c_str()) != 0;
        if (gCaptureGeometryWanted)
            Log("Upscaling: geometry capture on ([Upscaling] CaptureGeometry) — active while the MV feeder is on");
        // Read DLSS intent early (before OGRE creates its samplers) so the mip-bias hook can apply.
        gDlssWanted = GetPrivateProfileIntA("Upscaling", "DLSS", 0, iniPath.c_str()) != 0;
        gMipBiasSharpen = GetPrivateProfileIntA("Upscaling", "MipBiasSharpen", 1, iniPath.c_str()) != 0;
        if (gDlssWanted && gMipBiasSharpen) Log("Upscaling: DLSS on -> applying %.1f texture mip bias to new samplers", kDlssMipBias);
        else if (gDlssWanted) Log("Upscaling: DLSS on, mip-bias sharpening disabled (MipBiasSharpen=0)");
    }

    // Initialize all loaded effect plugins
    if (!gEffectLoader.InitAll(gDevice, gWidth, gHeight))
        Log("WARNING: One or more effect plugins failed to initialize");

    // Deferred: install Present/ResizeBuffers hooks now that the real device is captured.
    // By waiting until the first Draw call, we avoid the race with overlay DLLs
    // (Steam, Discord, ReShade) that also hook Present during their initialization.
    TryInstallSwapChainHooks();

    Log("Dust fully initialized and active");
}

// ==================== Hook implementations ====================

static bool IsPowerOf2(UINT v) { return v && !(v & (v - 1)); }

static const char* FormatName(DXGI_FORMAT f)
{
    switch (f) {
        case DXGI_FORMAT_R32_FLOAT:     return "R32_FLOAT";
        case DXGI_FORMAT_R32_TYPELESS:  return "R32_TYPELESS";
        case DXGI_FORMAT_R16_FLOAT:     return "R16_FLOAT";
        case DXGI_FORMAT_R16_UNORM:     return "R16_UNORM";
        case DXGI_FORMAT_R16_TYPELESS:  return "R16_TYPELESS";
        case DXGI_FORMAT_R24G8_TYPELESS:return "R24G8_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT:     return "D32_FLOAT";
        case DXGI_FORMAT_D16_UNORM:     return "D16_UNORM";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
        default: return nullptr;
    }
}

// Matches the two textures Kenshi creates for its shadow atlas: the R32_FLOAT
// color atlas (RTW warped shadow map, RTV+SRV) and the R32_TYPELESS depth
// atlas (DSV+SRV, sampled by deferred lighting in CSM mode). The 1024 floor
// deliberately excludes the 512² RTW warp-map intermediate.
static bool IsShadowAtlasDesc(const D3D11_TEXTURE2D_DESC* d)
{
    if (!d) return false;
    if (d->Width != d->Height) return false;
    if (!IsPowerOf2(d->Width)) return false;
    if (d->Width < 1024 || d->Width > 16384) return false;
    if (d->ArraySize != 1) return false;
    if (d->MipLevels != 1) return false;

    bool hasSRV = (d->BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
    bool hasRTV = (d->BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
    bool hasDSV = (d->BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;

    if (d->Format == DXGI_FORMAT_R32_FLOAT && hasRTV && hasSRV) return true;
    if (d->Format == DXGI_FORMAT_R32_TYPELESS && hasDSV && hasSRV) return true;
    return false;
}

static HRESULT STDMETHODCALLTYPE HookedCreateSamplerState(
    ID3D11Device* pThis, const D3D11_SAMPLER_DESC* pDesc, ID3D11SamplerState** ppSamplerState)
{
    // When DLSS is on, sharpen by exposing ~1 extra mip of texture detail for it to resolve. Harmless
    // on point/no-mip samplers (the driver ignores the bias there), so it's applied unconditionally.
    if (!gShutdownSignaled && gDlssWanted && gMipBiasSharpen && pDesc)
    {
        D3D11_SAMPLER_DESC d = *pDesc;
        d.MipLODBias += kDlssMipBias;
        return oCreateSamplerState(pThis, &d, ppSamplerState);
    }
    return oCreateSamplerState(pThis, pDesc, ppSamplerState);
}

static HRESULT STDMETHODCALLTYPE HookedCreateTexture2D(
    ID3D11Device* pThis, const D3D11_TEXTURE2D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D)
{
    if (gShutdownSignaled) return oCreateTexture2D(pThis, pDesc, pInitialData, ppTexture2D);

    if (pDesc && pDesc->Width == pDesc->Height &&
        IsPowerOf2(pDesc->Width) && pDesc->Width >= 256 && pDesc->Width <= 8192)
    {
        const char* fn = FormatName(pDesc->Format);
        if (fn)
        {
            bool hasSRV = (pDesc->BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
            bool hasRTV = (pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
            bool hasDSV = (pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
            Log("CreateTex2D: %ux%u %s bind=%s%s%s",
                pDesc->Width, pDesc->Height, fn,
                hasSRV ? "SRV " : "", hasRTV ? "RTV " : "", hasDSV ? "DSV " : "");

        }
    }

    HRESULT hr = oCreateTexture2D(pThis, pDesc, pInitialData, ppTexture2D);

    // Track every shadow-atlas-matching Texture2D (color AND depth) for
    // runtime resize and shadow-pass detection.
    if (SUCCEEDED(hr) && pDesc && ppTexture2D && *ppTexture2D &&
        IsShadowAtlasDesc(pDesc))
    {
        // The game recreates its shadow workspace when the user switches
        // shadow type or resolution in the options — drop stale tracking so
        // the new atlas generation starts clean. Same-frame creations are
        // the color+depth pair of one atlas and must accumulate.
        if (gShadowEntryCount.load(std::memory_order_relaxed) > 0 &&
            gFrameIndex != gShadowCreationFrame)
        {
            ResetShadowTracking();
        }
        gShadowCreationFrame = gFrameIndex;

        bool isDepth = (pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
        IUnknown* unk = nullptr;
        (*ppTexture2D)->QueryInterface(IID_IUnknown, (void**)&unk);

        // Entry tracking (shadow-pass detection + runtime resize swapping).
        // NOTE: creation-time tracking alone is incomplete — a shadow-mode
        // switch can rebuild the workspace from OGRE's texture pool with no
        // creation call; those textures are adopted at bind time instead
        // (AdoptShadowTexture).
        if (unk)
        {
            size_t eidx = gShadowEntryCount.load(std::memory_order_relaxed);
            if (eidx < kMaxShadowIdentities)
            {
                ShadowAtlasEntry& e = gShadowEntries[eidx];
                e = {};
                e.tex = *ppTexture2D;
                e.tex->AddRef();
                e.identity = unk;  // weak ref, backed by e.tex
                e.desc = *pDesc;
                e.isDepth = isDepth;
                e.lastSeen = gFrameIndex;
                e.vpScale = 1.0f;
                if (gShadowVanillaSize == 0)
                    gShadowVanillaSize = pDesc->Width;
                gShadowEntryCount.store(eidx + 1, std::memory_order_release);

                // Re-queue a pending resize so an active override survives
                // shadow type switches (the fresh atlas is vanilla-sized).
                UINT wanted = gShadowAtlasOverride;
                if (wanted != 0 && wanted != pDesc->Width &&
                    !gShadowResizePending.load(std::memory_order_relaxed))
                {
                    gShadowResizeTarget = wanted;
                    gShadowResizePending.store(true, std::memory_order_release);
                }
            }
            unk->Release();
        }

        Log("Shadow atlas %s texture captured: tex=%p identity=%p (%ux%u) entry=%zu",
            isDepth ? "DSV" : "color", *ppTexture2D, unk,
            pDesc->Width, pDesc->Height,
            gShadowEntryCount.load(std::memory_order_relaxed) - 1);
    }

    return hr;
}

// ==================== Light-volume direct AO ====================
// Point/spot lights render as additive forward light volumes (light_fs) AFTER the
// fullscreen sun pass (main_fs). SSAO's direct-light AO term is injected into light_fs
// by ShaderPatch, but light_fs samples the AO map at s8/s9 — slots the engine reuses
// between the sun pass and the light volumes. So we can't just leave the sun pass's AO
// binding in place (that painted garbage over the light volumes). Instead we:
//   1. identify the patched light_fs pixel shader(s) by bytecode hash at creation time,
//   2. snapshot the AO/params SRVs the SSAO plugin binds for the sun pass (guaranteed
//      valid at POST_LIGHTING), and
//   3. re-bind them at s8/s9 for exactly the light-volume draws, restoring the engine's
//      previous binding immediately after — so no state leaks into any other pass.
// If ShaderPatch never patches light_fs (e.g. anchors missing), gLightVolumePS stays
// empty and the whole path is a no-op.

static const uint32_t AO_SLOT        = 8;   // register(s8) in deferred.hlsl (aoMap)
static const uint32_t AO_PARAMS_SLOT = 9;   // register(s9) (aoParams)
static const uint32_t RTGI_AO_SLOT   = 10;  // register(t10) — RTGI's per-light AO

static std::mutex gLightVolumeMutex;
static std::unordered_set<uint64_t> gLightVolumeBytecodeHashes;  // patched light_fs blobs
static std::unordered_set<ID3D11PixelShader*> gLightVolumePS;    // created light_fs shaders

// Snapshot of the AO bindings captured at POST_LIGHTING, re-applied per light-volume draw.
static ID3D11ShaderResourceView* gLvAoSRV[2]     = { nullptr, nullptr };
static ID3D11SamplerState*       gLvAoSampler[2] = { nullptr, nullptr };
static bool                      gLvAoReady      = false;

// RTGI publishes its per-light AO texture here each frame (via the SetLightVolumeAoTexture
// host API); light_fs multiplies by it at t10. Null when RTGI isn't applying AO → the
// scope binds a 1x1 white fallback so the multiply is a no-op (never leave t10 unbound —
// an unbound SRV reads 0 and would black out the lights).
static ID3D11ShaderResourceView* gRtgiLvAoSRV = nullptr;   // borrowed ref, refreshed per frame
static ID3D11ShaderResourceView* gLvWhiteSRV  = nullptr;   // host-owned 1x1 R8 white
static ID3D11SamplerState*       gLvDefaultSampler = nullptr;  // fallback when SSAO didn't bind

// SSAO publishes its AO map + params here each frame (v7 SetLightVolumeSsaoAo). These take
// precedence over the deferred-slot snapshot (gLvAoSRV) at t8/t9 — the snapshot comes back
// empty on some configs, dropping AO on local lights. Borrowed refs, cleared per frame.
static ID3D11ShaderResourceView* gSsaoLvAoSRV     = nullptr;
static ID3D11ShaderResourceView* gSsaoLvParamsSRV = nullptr;

static ID3D11ShaderResourceView* GetLvWhiteSRV()
{
    if (gLvWhiteSRV || !gDevice) return gLvWhiteSRV;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    uint8_t val = 255;
    D3D11_SUBRESOURCE_DATA sd = {}; sd.pSysMem = &val; sd.SysMemPitch = 1;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(gDevice->CreateTexture2D(&td, &sd, &tex)) && tex)
    {
        gDevice->CreateShaderResourceView(tex, nullptr, &gLvWhiteSRV);
        tex->Release();
    }
    return gLvWhiteSRV;
}

static ID3D11SamplerState* GetLvDefaultSampler()
{
    if (gLvDefaultSampler || !gDevice) return gLvDefaultSampler;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    gDevice->CreateSamplerState(&sd, &gLvDefaultSampler);
    return gLvDefaultSampler;
}

// Called by RTGI (via host API) each frame it produces a light-volume AO texture.
// nullptr clears it (→ white fallback). Borrowed ref: AddRef for the frame, released on
// the next call or at frame reset.
void SetLightVolumeAoTexture(ID3D11ShaderResourceView* srv)
{
    if (srv == gRtgiLvAoSRV) return;
    if (gRtgiLvAoSRV) gRtgiLvAoSRV->Release();
    gRtgiLvAoSRV = srv;
    if (gRtgiLvAoSRV) gRtgiLvAoSRV->AddRef();
}

// Called by SSAO (via host API) each frame to publish its AO map + params for light volumes.
void SetLightVolumeSsaoAo(ID3D11ShaderResourceView* ao, ID3D11ShaderResourceView* params)
{
    if (ao != gSsaoLvAoSRV)
    {
        if (gSsaoLvAoSRV) gSsaoLvAoSRV->Release();
        gSsaoLvAoSRV = ao;
        if (gSsaoLvAoSRV) gSsaoLvAoSRV->AddRef();
    }
    if (params != gSsaoLvParamsSRV)
    {
        if (gSsaoLvParamsSRV) gSsaoLvParamsSRV->Release();
        gSsaoLvParamsSRV = params;
        if (gSsaoLvParamsSRV) gSsaoLvParamsSRV->AddRef();
    }

    static bool sLogged = false;
    if (!sLogged && ao) { Log("LightVolumeAO: SSAO published its AO for light volumes (t8/t9)"); sLogged = true; }
}

static uint64_t Fnv1a64(const void* data, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

// Called by ShaderPatch after it successfully compiles a PATCHED light_fs blob.
void NoteLightVolumeShaderBytecode(const void* bytecode, size_t len)
{
    if (!bytecode || !len) return;
    uint64_t h = Fnv1a64(bytecode, len);
    std::lock_guard<std::mutex> lock(gLightVolumeMutex);
    gLightVolumeBytecodeHashes.insert(h);
}

static void ReleaseLightVolumeAO()
{
    for (int i = 0; i < 2; ++i)
    {
        if (gLvAoSRV[i])     { gLvAoSRV[i]->Release();     gLvAoSRV[i] = nullptr; }
        if (gLvAoSampler[i]) { gLvAoSampler[i]->Release(); gLvAoSampler[i] = nullptr; }
    }
    gLvAoReady = false;
}

// Mirror of EffectLoader's ResolveSlot: the deferred shaders remap sampler registers by
// shadow variant, so the AO the SSAO plugin bound at baseSlot 8/9 (via host->BindSRV) may
// physically live at 8/9 (shadow mode) or 6/7 (no-shadow mode). We must snapshot from the
// same physical slot it was bound to, else we'd capture an engine texture.
static UINT ResolveAoSlot(ID3D11DeviceContext* ctx, uint32_t baseSlot)
{
    if (baseSlot <= 5) return baseSlot;
    ID3D11ShaderResourceView* srv6 = nullptr;
    ctx->PSGetShaderResources(6, 1, &srv6);
    if (srv6) { srv6->Release(); return baseSlot; }   // shadow mode: as declared
    return baseSlot - 2;                              // no-shadow mode: shifted down
}

// Snapshot whatever the SSAO plugin bound (logically) at s8/s9 for the sun pass. Called
// right after DispatchPre at POST_LIGHTING, while those slots still hold the AO map+params.
static void CaptureLightVolumeAO(ID3D11DeviceContext* ctx)
{
    {
        std::lock_guard<std::mutex> lock(gLightVolumeMutex);
        if (gLightVolumePS.empty()) return;   // nothing patched → nothing to feed
    }
    ReleaseLightVolumeAO();  // drop last frame's snapshot

    UINT aoSlot     = ResolveAoSlot(ctx, AO_SLOT);
    UINT paramsSlot = ResolveAoSlot(ctx, AO_PARAMS_SLOT);
    ctx->PSGetShaderResources(aoSlot,     1, &gLvAoSRV[0]);       // AddRefs
    ctx->PSGetShaderResources(paramsSlot, 1, &gLvAoSRV[1]);
    ctx->PSGetSamplers(aoSlot,     1, &gLvAoSampler[0]);
    ctx->PSGetSamplers(paramsSlot, 1, &gLvAoSampler[1]);

    // Ready = "light_fs is patched and we're in its draw window" — NOT "SSAO bound valid
    // AO". The patched light_fs ALWAYS samples t8/t9/t10, so the scope must run for every
    // light-volume draw and white-fill any missing slot; otherwise unbound slots read 0 and
    // the light multiplies to black. (SSAO removed / not yet bound must not kill lights.)
    gLvAoReady = true;

    static bool sLogged = false;
    if (!sLogged && gLvAoSRV[0])
    {
        // Log the AO texture's dimensions so a bad capture (e.g. a 4096² shadow atlas)
        // is obvious in the log rather than only on screen.
        UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        ID3D11Resource* res = nullptr;
        gLvAoSRV[0]->GetResource(&res);
        if (res)
        {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex)
            {
                D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
                w = d.Width; h = d.Height; fmt = d.Format;
                tex->Release();
            }
            res->Release();
        }
        Log("LightVolumeAO: captured AO snapshot from slots %u/%u (ao=%p %ux%u fmt=%u), %zu light_fs shader(s)",
            aoSlot, paramsSlot, gLvAoSRV[0], w, h, (unsigned)fmt, gLightVolumePS.size());
        sLogged = true;
    }
}

// True if this draw is a point/spot light volume (patched light_fs bound).
static bool IsLightVolumeDraw(ID3D11DeviceContext* ctx)
{
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);  // AddRefs
    if (!ps) return false;

    bool match;
    {
        std::lock_guard<std::mutex> lock(gLightVolumeMutex);
        match = gLightVolumePS.count(ps) != 0;
    }
    ps->Release();
    return match;
}

// Scoped swap of s8/s9 to the AO snapshot for a light-volume draw. Both DrawIndexed and
// DrawIndexedInstanced route through this so the fix holds whichever call the engine uses.
struct LightVolumeAoScope
{
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11ShaderResourceView* savedSRV[3] = { nullptr, nullptr, nullptr };  // t8,t9,t10
    ID3D11SamplerState*       savedSmp[2] = { nullptr, nullptr };            // s8,s9
    bool active = false;

    // isLightVolume is precomputed by the caller (which also needs it for the
    // postLightVolumes fire decision) to avoid a second PSGetShader per draw.
    LightVolumeAoScope(ID3D11DeviceContext* c, bool isLightVolume)
    {
        if (!isLightVolume) return;
        ctx = c;
        // Save t8/t9/t10 + s8/s9, then bind SSAO's AO (t8/t9) and RTGI's AO (t10). Any slot
        // without a real texture gets a 1x1 white fallback (= 1.0, a no-op multiply) — the
        // patched light_fs samples all three unconditionally, so an unbound slot would read
        // 0 and turn the light black. This keeps lights lit even if SSAO/RTGI aren't active.
        ctx->PSGetShaderResources(AO_SLOT, 3, savedSRV);   // AddRefs engine's binding
        ctx->PSGetSamplers(AO_SLOT, 2, savedSmp);
        ID3D11ShaderResourceView* white = GetLvWhiteSRV();
        // t8/t9: prefer SSAO's directly-published textures (robust), then the deferred-slot
        // snapshot, then white. t10: RTGI's published texture, else white.
        ID3D11ShaderResourceView* bind3[3] = {
            gSsaoLvAoSRV     ? gSsaoLvAoSRV     : (gLvAoSRV[0] ? gLvAoSRV[0] : white),
            gSsaoLvParamsSRV ? gSsaoLvParamsSRV : (gLvAoSRV[1] ? gLvAoSRV[1] : white),
            gRtgiLvAoSRV     ? gRtgiLvAoSRV     : white,
        };
        ID3D11SamplerState* def = GetLvDefaultSampler();
        ID3D11SamplerState* smp2[2] = {
            gLvAoSampler[0] ? gLvAoSampler[0] : def,
            gLvAoSampler[1] ? gLvAoSampler[1] : def,
        };
        ctx->PSSetShaderResources(AO_SLOT, 3, bind3);
        ctx->PSSetSamplers(AO_SLOT, 2, smp2);
        active = true;

        static bool sLogged = false;
        if (!sLogged) { Log("LightVolumeAO: applying AO to light-volume draws"); sLogged = true; }
    }

    ~LightVolumeAoScope()
    {
        if (!active) return;
        ctx->PSSetShaderResources(AO_SLOT, 3, savedSRV);   // restore engine's binding
        ctx->PSSetSamplers(AO_SLOT, 2, savedSmp);
        for (int i = 0; i < 3; ++i) if (savedSRV[i]) savedSRV[i]->Release();
        for (int i = 0; i < 2; ++i) if (savedSmp[i]) savedSmp[i]->Release();
    }
};

// Fire the v5 postLightVolumes callbacks once per frame (builds a frame context from the
// captured globals). Fired at POST_FOG/POST_TONEMAP — after the point/spot light volumes.
static void FirePostLightVolumes(ID3D11DeviceContext* ctx, InjectionPoint point, const char* where)
{
    gPostLightVolumesFired = true;

    DustFrameContext fctx = {};
    fctx.device = gDevice;
    fctx.context = ctx;
    fctx.point = static_cast<DustInjectionPoint>(point);
    fctx.timing = DUST_TIMING_PRE;
    fctx.width = gWidth;
    fctx.height = gHeight;
    fctx.frameIndex = gFrameIndex;
    fctx.camera = gCameraData;
    gEffectLoader.DispatchPostLightVolumes(&fctx);

    // Log the first occurrence of each distinct fire path (string literals have stable
    // addresses). "pre-transparent" = fired after light volumes, before water (good);
    // "fog/tonemap fallback" = no light volume this frame, so it fired at POST_FOG,
    // which is after water — surfaces the no-local-light water edge case.
    static const char* sLoggedA = nullptr;
    static const char* sLoggedB = nullptr;
    if (where != sLoggedA && where != sLoggedB)
    {
        Log("PostLightVolumes: dispatched (%s)", where);
        if (!sLoggedA) sLoggedA = where; else sLoggedB = where;
    }
}

// Fwd decls for the [Debug] LogIconDraws diagnostic (defined before HookedDraw below) — the
// create hooks record bytecode for the draw-time disassembly dump.
static bool IconDrawLogEnabled();
static void IconDbgNoteShader(void* obj, const void* bytecode, SIZE_T len);

static HRESULT STDMETHODCALLTYPE HookedCreatePixelShader(
    ID3D11Device* pThis, const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader)
{
    if (gShutdownSignaled)
        return oCreatePixelShader(pThis, pShaderBytecode, BytecodeLength,
                                   pClassLinkage, ppPixelShader);

    // NOTE: Do NOT capture device here.  OGRE (and other middleware) may create
    // temporary enumeration devices that are destroyed before rendering begins.
    // Device capture happens in HookedDraw from the actual rendering context.

    HRESULT hr = oCreatePixelShader(pThis, pShaderBytecode, BytecodeLength,
                                     pClassLinkage, ppPixelShader);
    if (SUCCEEDED(hr) && ppPixelShader && *ppPixelShader)
    {
        SurveyRecorder::OnPixelShaderCreated(pShaderBytecode, BytecodeLength, *ppPixelShader);
        if (IconDrawLogEnabled())
            IconDbgNoteShader(*ppPixelShader, pShaderBytecode, BytecodeLength);

        // Record this shader if it's a patched light_fs. This MUST be reliable: the patched
        // light_fs multiplies each light by t8/t9/t10, so a shader we fail to recognize
        // never gets those slots bound → it reads 0 → the light casts no light.
        if (pShaderBytecode && BytecodeLength)
        {
            // Primary: exact hash of the D3DCompile output (ShaderPatch noted it).
            uint64_t h = Fnv1a64(pShaderBytecode, BytecodeLength);
            bool isLightFs;
            {
                std::lock_guard<std::mutex> lock(gLightVolumeMutex);
                isLightFs = gLightVolumeBytecodeHashes.count(h) != 0;
            }
            // Robust fallback: scan for the unique resource name our patch injects. It lives
            // in the DXBC reflection (RDEF) chunk, which the engine can't strip (it binds
            // params by name) — so this survives even when the bytecode bytes differ from
            // D3DCompile's output (blob re-sign/strip by driver/d3dcompiler) and break the hash.
            if (!isLightFs)
            {
                const char* p = (const char*)pShaderBytecode;
                static const char kMark[] = "dustLvRtgiAoTex";
                const size_t mlen = sizeof(kMark) - 1;
                if (BytecodeLength >= mlen)
                    for (size_t i = 0, n = BytecodeLength - mlen; i <= n; ++i)
                        if (memcmp(p + i, kMark, mlen) == 0) { isLightFs = true; break; }
            }
            if (isLightFs)
            {
                std::lock_guard<std::mutex> lock(gLightVolumeMutex);
                gLightVolumePS.insert(*ppPixelShader);
            }
        }
    }
    return hr;
}

// ==================== CreateInputLayout hook (records vertex declarations) ====================

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateInputLayout)(
    ID3D11Device* pThis, const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements,
    const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength,
    ID3D11InputLayout** ppInputLayout);

static PFN_CreateInputLayout oCreateInputLayout = nullptr;

static HRESULT STDMETHODCALLTYPE HookedCreateInputLayout(
    ID3D11Device* pThis, const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements,
    const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength,
    ID3D11InputLayout** ppInputLayout)
{
    HRESULT hr = oCreateInputLayout(pThis, pInputElementDescs, NumElements,
                                    pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout);
    if (!gShutdownSignaled && SUCCEEDED(hr) && ppInputLayout && *ppInputLayout)
        ShaderMetadata::OnInputLayoutCreated(*ppInputLayout, pInputElementDescs, NumElements);
    return hr;
}

// ==================== CreateVertexShader hook (for shader source tracking) ====================

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateVertexShader)(
    ID3D11Device* pThis, const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader);

static PFN_CreateVertexShader oCreateVertexShader = nullptr;

static HRESULT STDMETHODCALLTYPE HookedCreateVertexShader(
    ID3D11Device* pThis, const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader)
{
    if (gShutdownSignaled)
        return oCreateVertexShader(pThis, pShaderBytecode, BytecodeLength,
                                    pClassLinkage, ppVertexShader);

    HRESULT hr = oCreateVertexShader(pThis, pShaderBytecode, BytecodeLength,
                                      pClassLinkage, ppVertexShader);
    if (SUCCEEDED(hr) && ppVertexShader && *ppVertexShader)
    {
        SurveyRecorder::OnVertexShaderCreated(pShaderBytecode, BytecodeLength, *ppVertexShader);
        // Reflect the VS CB layout so GeometryCapture can classify draws
        // (STATIC vs SKINNED) and locate the clip/world matrix for the MV pass.
        ShaderMetadata::OnVertexShaderCreated(pShaderBytecode, BytecodeLength, *ppVertexShader);
        if (IconDrawLogEnabled())
            IconDbgNoteShader(*ppVertexShader, pShaderBytecode, BytecodeLength);
    }
    return hr;
}

// ==================== [Debug] LogIconDraws diagnostic ====================
// Ground-truth instrument for the quadrant-coloured item icons: logs each DISTINCT VS/PS pair
// drawn into a SMALL offscreen colour RT (<=512px — the item-icon / portrait pipeline) with the
// shaders' compile-time identities from SurveyRecorder. Kenshi builds its icon materials in C++,
// so the .material scripts cannot tell us which pair the icon draw actually binds — this can.
// Also dumps the bound pair's DISASSEMBLY (exact compiled OSGN/ISGN signatures — variant-level
// truth the sources cannot give) plus the PS SRV bindings, to DustLogDir()\icon_shaders\.
// Off by default; zero cost beyond one cached ini read when disabled.
static std::unordered_map<void*, std::vector<uint8_t>> sIconDbgBlob;   // shader object -> bytecode
static std::mutex sIconDbgBlobMutex;
static void IconDbgNoteShader(void* obj, const void* bytecode, SIZE_T len)
{
    if (!obj || !bytecode || !len) return;
    std::lock_guard<std::mutex> lk(sIconDbgBlobMutex);
    sIconDbgBlob[obj].assign((const uint8_t*)bytecode, (const uint8_t*)bytecode + len);
}
static void IconDbgDumpAsm(void* obj, const char* stage, const ShaderSourceInfo* info)
{
    std::vector<uint8_t> blob;
    {
        std::lock_guard<std::mutex> lk(sIconDbgBlobMutex);
        auto it = sIconDbgBlob.find(obj);
        if (it != sIconDbgBlob.end()) blob = it->second;
    }
    if (blob.empty()) { Log("ICONDRAW: no bytecode recorded for %s %p", stage, obj); return; }
    ID3DBlob* txt = nullptr;
    if (FAILED(D3DDisassemble(blob.data(), blob.size(), 0, nullptr, &txt)) || !txt)
    { Log("ICONDRAW: disassembly failed for %s %p", stage, obj); return; }
    std::string dir = DustLogDir() + "icon_shaders";
    CreateDirectoryA(dir.c_str(), nullptr);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s_%p_%s.asm", dir.c_str(), stage, obj,
             info ? info->entryPoint.c_str() : "unknown");
    FILE* f = fopen(path, "wb");
    if (f)
    {
        fwrite(txt->GetBufferPointer(), 1, strlen((const char*)txt->GetBufferPointer()), f);
        fclose(f);
        Log("ICONDRAW: dumped %s", path);
    }
    txt->Release();
}
static bool IconDrawLogEnabled()
{
    static int cached = -1;
    if (cached < 0)
        cached = GetPrivateProfileIntA("Debug", "LogIconDraws", 0,
                                       (DustLogDir() + "Dust.ini").c_str());
    return cached != 0;
}
static void LogIconDraw(ID3D11DeviceContext* ctx, const char* kind, UINT count, UINT instances)
{
    ID3D11RenderTargetView* rtvs[4] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(4, rtvs, &dsv);
    D3D11_TEXTURE2D_DESC desc = {};
    bool iconSized = false;
    if (rtvs[0])
    {
        ID3D11Resource* res = nullptr;
        rtvs[0]->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res) { res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex); res->Release(); }
        if (tex) { tex->GetDesc(&desc); tex->Release(); iconSized = desc.Width <= 512 && desc.Height <= 512; }
    }
    if (iconSized)
    {
        UINT nRt = 0;
        for (UINT i = 0; i < 4; i++) if (rtvs[i]) nRt = i + 1;
        ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        static std::unordered_set<uint64_t> sSeenPairs;
        static std::mutex sSeenMutex;
        bool fresh;
        {
            std::lock_guard<std::mutex> lk(sSeenMutex);
            fresh = sSeenPairs.insert((uint64_t)(uintptr_t)vs ^ ((uint64_t)(uintptr_t)ps << 1)).second;
        }
        if (fresh)
        {
            const ShaderSourceInfo* vi = SurveyRecorder::GetShaderSource((uint64_t)(uintptr_t)vs);
            const ShaderSourceInfo* pi = SurveyRecorder::GetShaderSource((uint64_t)(uintptr_t)ps);
            UINT nvp = 1; D3D11_VIEWPORT vp = {};
            ctx->RSGetViewports(&nvp, &vp);
            Log("ICONDRAW[%s]: rt0=%ux%u fmt=%d nRT=%u dsv=%d vp=%.0fx%.0f n=%u inst=%u VS=%p (%s : %s) PS=%p (%s : %s)",
                kind, desc.Width, desc.Height, (int)desc.Format, nRt, dsv ? 1 : 0,
                vp.Width, vp.Height, count, instances,
                (void*)vs, vi ? vi->sourceName.c_str() : "?", vi ? vi->entryPoint.c_str() : "?",
                (void*)ps, pi ? pi->sourceName.c_str() : "?", pi ? pi->entryPoint.c_str() : "?");
            IconDbgDumpAsm((void*)vs, "vs", vi);
            IconDbgDumpAsm((void*)ps, "ps", pi);
            // PS SRV bindings t0-t7: catches a wrong/mis-dimensioned texture (e.g. a 2D texture
            // where the icon shader expects its reflection CUBE at t3).
            ID3D11ShaderResourceView* srvs[8] = {};
            ctx->PSGetShaderResources(0, 8, srvs);
            for (UINT i = 0; i < 8; i++)
            {
                if (!srvs[i]) continue;
                D3D11_SHADER_RESOURCE_VIEW_DESC sd;
                srvs[i]->GetDesc(&sd);
                ID3D11Resource* sres = nullptr;
                srvs[i]->GetResource(&sres);
                ID3D11Texture2D* stex = nullptr;
                D3D11_TEXTURE2D_DESC td = {};
                if (sres) { sres->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&stex); sres->Release(); }
                if (stex) { stex->GetDesc(&td); stex->Release(); }
                Log("ICONDRAW:   PS t%u dim=%d fmt=%d %ux%u arr=%u", i,
                    (int)sd.ViewDimension, (int)td.Format, td.Width, td.Height, td.ArraySize);
                srvs[i]->Release();
            }
        }
        if (vs) vs->Release();
        if (ps) ps->Release();
    }
    for (UINT i = 0; i < 4; i++) if (rtvs[i]) rtvs[i]->Release();
    if (dsv) dsv->Release();
}

static void STDMETHODCALLTYPE HookedDraw(
    ID3D11DeviceContext* pThis, UINT VertexCount, UINT StartVertexLocation)
{
    if (gShutdownSignaled) { oDraw(pThis, VertexCount, StartVertexLocation); return; }

    ++gDrawHookCallCount;

    // Try to install swap chain hooks early — DustBoot may already have captured the
    // swap chain, and we don't need device capture for that path. Pass pThis so
    // runtime discovery can also work from any draw call (not just fullscreen ones).
    if (!sSwapChainHooked)
        TryInstallSwapChainHooks(pThis);

    // Survey: record ALL draws (before fullscreen filter)
    if (Survey::IsActive())
        SurveyRecorder::OnDraw(pThis, VertexCount, StartVertexLocation);

    if (IconDrawLogEnabled())
        LogIconDraw(pThis, "D", VertexCount, 1);

    if (VertexCount != 3 && VertexCount != 4)
    {
        // Real (non-quad) geometry can also come through the NON-INDEXED path. It still needs the
        // injected-MV reproj CB pinned at b13, exactly like DrawIndexed/DrawIndexedInstanced do:
        // OGRE reflects the injected DustMVCB and rebinds its OWN buffer there per draw, so without
        // this the patched VS computes oDustPrev from whatever OGRE left at b13 and the velocity
        // becomes a function of position instead of a frame delta (shows up in the MV debug view as
        // a position gradient rather than motion).
        if (sMvFeederActive)
        { ID3D11Buffer* rcb = MotionVectors::GetInjReprojCB(); if (rcb) pThis->VSSetConstantBuffers(13, 1, &rcb); }
        oDraw(pThis, VertexCount, StartVertexLocation);
        return;
    }

    // A compositor/fullscreen quad inside a jittered forward scene pass (fog/tonemap and other
    // image-space quads can bind the scene DSV) must NOT be jittered: shifting an image-space quad
    // misaligns its screen-UV reads by the jitter offset and double-jitters the composite. 3D content
    // in those passes comes through DrawIndexed*, so plain 3/4-vertex Draws here are only the quads.
    // "Viewport actually jittered" is detected by its fractional origin (scene viewports are integer);
    // this also self-corrects if OGRE set the pass viewport before the RT bind (then it's unjittered).
    struct QuadUnjitterScope
    {
        ID3D11DeviceContext* ctx = nullptr;
        D3D11_VIEWPORT saved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        UINT n = 0;
        ~QuadUnjitterScope() { if (n) oRSSetViewports(ctx, n, saved); }   // restore the pass's jittered viewport
    } quadVp;
    if (gJitterEnabled && gJitterForwardPasses && gFwdScenePass && !gInShadowPass &&
        (gJitterPxX != 0.0f || gJitterPxY != 0.0f))
    {
        UINT nVP = 0;
        pThis->RSGetViewports(&nVP, nullptr);
        if (nVP > 0 && nVP <= D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
        {
            D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
            pThis->RSGetViewports(&nVP, vps);
            bool vpJittered = (vps[0].TopLeftX != floorf(vps[0].TopLeftX)) ||
                              (vps[0].TopLeftY != floorf(vps[0].TopLeftY));
            if (vpJittered)
            {
                quadVp.ctx = pThis;
                quadVp.n = nVP;
                memcpy(quadVp.saved, vps, nVP * sizeof(D3D11_VIEWPORT));
                for (UINT i = 0; i < nVP; i++)
                {
                    vps[i].TopLeftX -= gJitterPxX;
                    vps[i].TopLeftY -= gJitterPxY;
                }
                oRSSetViewports(pThis, nVP, vps);
            }
        }
    }

    // Detect the render pass from GPU state. This reads ONLY the context's pipeline state (no
    // device needed), so it runs even before we've captured a device — which is exactly how we pick
    // the RIGHT device. Injected overlays (NVIDIA Smooth Motion, App/Freestyle, Steam/Discord, RTSS)
    // spawn a SECOND D3D11 device and also issue fullscreen draws; capturing on the first raw draw
    // could grab that device, and at the world lighting pass the game's real device differs ->
    // cross-device resource binding -> DXGI_ERROR_DEVICE_REMOVED (which Kenshi's catch-all misreports
    // as "ran out of video memory"). Only Kenshi's deferred pipeline produces this GBuffer/lighting
    // signature, so capturing on a CONFIRMED detection is overlay-proof by construction.
    auto result = gPipelineDetector.OnFullscreenDraw(pThis);

    if (result.detected)
    {
        // Everything below is Dust-side work (effect dispatches, upscaler resolve, MV passes) — its
        // viewport sets must not pick up the scene sub-pixel jitter (see VpJitterSuppressScope).
        VpJitterSuppressScope vpSuppress;

        // First confirmed game-pipeline draw: capture the device from THIS context. It just produced
        // Kenshi's own render signature, so it is guaranteed to be the game device, never an overlay's.
        if (!gDeviceCaptured)
        {
            ID3D11Device* ctxDevice = nullptr;
            pThis->GetDevice(&ctxDevice);
            if (ctxDevice)
            {
                Log("Capturing device at CONFIRMED Kenshi pipeline point (%d) — overlay-proof (device=%p)",
                    (int)result.point, ctxDevice);
                TryCaptureDevice(ctxDevice);
                // This thread just ran Kenshi's own pipeline draws -> it IS the game render thread.
                // Present-hook identity (IsGamePresent) keys on it to reject Smooth Motion's
                // worker-thread interpolated presents.
                gGameRenderThreadId = GetCurrentThreadId();
                Log("Game render thread = %u (present-identity anchor)", (unsigned)gGameRenderThreadId);
                ctxDevice->Release();
            }
            if (!gDeviceCaptured)
            {
                oDraw(pThis, VertexCount, StartVertexLocation);
                return;
            }
        }

        // Check device health once per frame (now that gDevice is valid, and only where it matters —
        // effects dispatch below). GetDeviceRemovedReason can force driver sync, so gate it per-frame.
        if (!gDeviceRemovedThisFrame)
        {
            HRESULT removeReason = gDevice->GetDeviceRemovedReason();
            if (removeReason != S_OK)
            {
                Log("Device removed (0x%08X), skipping draw hook entirely", removeReason);
                gDeviceRemovedThisFrame = true;
            }
        }
        if (gDeviceRemovedThisFrame)
        {
            oDraw(pThis, VertexCount, StartVertexLocation);
            return;
        }
        // Verify the context's device matches our captured device (one-time check)
        {
            static bool sDeviceChecked = false;
            if (!sDeviceChecked)
            {
                ID3D11Device* ctxDevice = nullptr;
                pThis->GetDevice(&ctxDevice);
                if (ctxDevice)
                {
                    if (ctxDevice != gDevice)
                        Log("WARNING: Context device=%p differs from captured device=%p!", ctxDevice, gDevice);
                    else
                        Log("Device pointer verified OK (device=%p)", gDevice);
                    ctxDevice->Release();
                }
                sDeviceChecked = true;
            }
        }

        // Detect real resolution from the HDR render target — only on the
        // lighting pass (first detection per frame) to avoid redundant COM calls.
        if (result.point == InjectionPoint::POST_LIGHTING)
        {
            ID3D11RenderTargetView* rtv = nullptr;
            pThis->OMGetRenderTargets(1, &rtv, nullptr);
            if (rtv)
            {
                ID3D11Resource* res = nullptr;
                rtv->GetResource(&res);
                if (res)
                {
                    ID3D11Texture2D* tex = nullptr;
                    res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
                    if (tex)
                    {
                        D3D11_TEXTURE2D_DESC desc;
                        tex->GetDesc(&desc);
                        if (desc.Width != gWidth || desc.Height != gHeight)
                        {
                            Log("Resolution changed: %ux%u -> %ux%u", gWidth, gHeight, desc.Width, desc.Height);
                            gWidth = desc.Width;
                            gHeight = desc.Height;
                            gEffectLoader.OnResolutionChanged(gDevice, gWidth, gHeight);
                            GeometryCapture::SetResolution(gWidth, gHeight);   // keep MV GBuffer detect in sync
                            MotionVectors::OnResolution(gWidth, gHeight);      // resize velocity RT
                        }
                        tex->Release();
                    }
                    res->Release();
                }
                rtv->Release();
            }
        }

        if (gWidth <= 1 || gHeight <= 1)
        {
            Log("Skipping effect dispatch (res=%ux%u)", gWidth, gHeight);
            oDraw(pThis, VertexCount, StartVertexLocation);
            return;
        }

        DustInjectionPoint dip = static_cast<DustInjectionPoint>(result.point);

        // Log first successful dispatch
        {
            static bool sFirstDispatch = true;
            if (sFirstDispatch)
            {
                Log("First effect dispatch: point=%d, res=%ux%u, frame=%llu",
                    (int)dip, gWidth, gHeight, gFrameIndex);
                sFirstDispatch = false;
            }
        }

        // Extract camera data at POST_LIGHTING (deferred CB is bound)
        if (dip == static_cast<DustInjectionPoint>(InjectionPoint::POST_LIGHTING))
        {
            ExtractCameraData(pThis);
            // Repack this frame's skinned poses so next frame's patched skin VS can read them as
            // "previous" from b12 and emit true animation velocity.
            if (sMvFeederActive)
                MotionVectors::InjFillSkinPrev(pThis);
        }

        DustFrameContext fctx = {};
        fctx.device = gDevice;
        fctx.context = pThis;
        fctx.point = dip;
        fctx.width = gWidth;
        fctx.height = gHeight;
        fctx.frameIndex = gFrameIndex;
        fctx.camera = gCameraData;

        // v5 postLightVolumes: fire at the first pass after the point/spot light volumes
        // (fog, or tonemap if no fog this frame), so effects can composite over the fully
        // lit scene incl. local lights.
        if (!gPostLightVolumesFired &&
            (result.point == InjectionPoint::POST_FOG ||
             result.point == InjectionPoint::POST_TONEMAP))
        {
            FirePostLightVolumes(pThis, result.point,
                                 result.point == InjectionPoint::POST_FOG ? "post-fog" : "post-tonemap");
        }

        // PRE: effects bind resources before the game's draw
        fctx.timing = DUST_TIMING_PRE;
        gEffectLoader.DispatchPre(dip, &fctx);

        // At POST_LIGHTING the SSAO plugin has just bound the AO map + params at s8/s9
        // for the sun pass; snapshot them so the point/spot light volumes (which draw
        // after this, through the patched light_fs) can sample the same AO. By POST_FOG
        // all light volumes are done, so stop feeding them.
        if (result.point == InjectionPoint::POST_LIGHTING)
            CaptureLightVolumeAO(pThis);
        else if (result.point == InjectionPoint::POST_FOG)
            gLvAoReady = false;

        // Execute the game's original draw call
        oDraw(pThis, VertexCount, StartVertexLocation);

        // POST: effects that operate after the draw
        fctx.timing = DUST_TIMING_POST;
        gEffectLoader.DispatchPost(dip, &fctx);

        // Shader-injected motion vectors: the velocity RT is filled during the real GBuffer pass
        // (see HookedOMSetRenderTargets + ShaderPatch). Here we only blit the debug viz over the
        // final image at POST_TONEMAP when [Upscaling] ShowMotionVectors=1, then end the frame.
        if (result.point == InjectionPoint::POST_TONEMAP)
        {
            RunUpscaler(pThis);                  // DLSS DLAA resolve (no-op unless [Upscaling] DLSS=1)
            RunInteropSpike(pThis);              // FSR3/4 D3D12 interop spike (no-op unless D3D12InteropSpike=1)
            MotionVectors::InjDebugBlit(pThis);  // MV debug viz (only if ShowMotionVectors=1)
            MotionVectors::InjEndFrame();
        }
    }
    else
    {
        oDraw(pThis, VertexCount, StartVertexLocation);
    }
}

static void STDMETHODCALLTYPE HookedDrawIndexed(
    ID3D11DeviceContext* pThis, UINT IndexCount, UINT StartIndexLocation,
    INT BaseVertexLocation)
{
    if (gShutdownSignaled) { oDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation); return; }

    ++gDrawHookCallCount;

    if (Survey::IsActive())
        SurveyRecorder::OnDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);

    if (IconDrawLogEnabled())
        LogIconDraw(pThis, "DI", IndexCount, 1);

    // The injected path only needs the current VS, skin CB and index buffer. Avoid the legacy full
    // CapturedDraw snapshot, whose replay-only IA/SRV/sampler queries dominated character-heavy scenes.
    if (sMvFeederActive && GeometryCapture::HasActiveCapture())
        MotionVectors::InjBindSkinPrev(pThis, IndexCount, BaseVertexLocation);

    // Direct-light AO for point/spot lights: for a light_fs draw, swap the AO snapshot
    // into s8/s9 for the draw and restore afterward (scope dtor). gLvAoReady gates the
    // PSGetShader check to the POST_LIGHTING→POST_FOG window.
    bool isLV = gLvAoReady && IsLightVolumeDraw(pThis);
    LightVolumeAoScope lvAo(pThis, isLV);
    // Keep the injected-MV reproj CB pinned at b13 for the patched GBuffer VS (bind AFTER OGRE's
    // per-draw constant setup). Gated on the feeder so a disabled upscaler binds nothing extra.
    if (sMvFeederActive)
    { ID3D11Buffer* rcb = MotionVectors::GetInjReprojCB(); if (rcb) pThis->VSSetConstantBuffers(13, 1, &rcb); }
    oDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);
}

static void STDMETHODCALLTYPE HookedDrawIndexedInstanced(
    ID3D11DeviceContext* pThis, UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
    if (gShutdownSignaled) { oDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation); return; }

    ++gDrawHookCallCount;

    if (Survey::IsActive())
        SurveyRecorder::OnDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                                                StartIndexLocation, BaseVertexLocation,
                                                StartInstanceLocation);

    if (IconDrawLogEnabled())
        LogIconDraw(pThis, "DII", IndexCountPerInstance, InstanceCount);

    // Instanced skin draws skip pose matching but must not inherit another character's sticky b12.
    if (sMvFeederActive && GeometryCapture::HasActiveCapture())
        MotionVectors::InjBindZeroB12(pThis);

    // Direct-light AO for point/spot lights (instanced light-volume path — see HookedDrawIndexed).
    bool isLV = gLvAoReady && IsLightVolumeDraw(pThis);
    LightVolumeAoScope lvAo(pThis, isLV);
    if (sMvFeederActive)
    { ID3D11Buffer* rcb = MotionVectors::GetInjReprojCB(); if (rcb) pThis->VSSetConstantBuffers(13, 1, &rcb); }
    oDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                          StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

// NON-INDEXED instanced. Previously unhooked, so geometry drawn this way never got the reproj CB
// pinned at b13 and its injected velocity came out as a function of position (a gradient in the MV
// debug view) instead of a frame delta. Logs once so a capture can confirm whether the game actually
// uses this path — if the line never appears, this was not the culprit.
static void STDMETHODCALLTYPE HookedDrawInstanced(
    ID3D11DeviceContext* pThis, UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation)
{
    if (gShutdownSignaled) { oDrawInstanced(pThis, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation); return; }

    ++gDrawHookCallCount;

    if (sMvFeederActive)
    {
        static bool sLogged = false;
        if (!sLogged) { sLogged = true; Log("DrawInstanced: non-indexed instanced draw seen (verts/inst=%u, inst=%u) — pinning reproj CB at b13", VertexCountPerInstance, InstanceCount); }
        ID3D11Buffer* rcb = MotionVectors::GetInjReprojCB();
        if (rcb) pThis->VSSetConstantBuffers(13, 1, &rcb);
    }
    oDrawInstanced(pThis, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

// Map/Unmap: proxy known skinned bone-palette WRITE_DISCARD buffers through cached CPU RAM. At Unmap
// the cached bytes are written to the real mapped GPU pointer (never read back from write-combined
// upload memory), while remaining available for current-root matching.
static HRESULT STDMETHODCALLTYPE HookedMap(
    ID3D11DeviceContext* pThis, ID3D11Resource* pResource, UINT Subresource,
    D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource)
{
    HRESULT hr = oMap(pThis, pResource, Subresource, MapType, MapFlags, pMappedResource);
    if (!gShutdownSignaled && SUCCEEDED(hr) && Subresource == 0 && pMappedResource && sMvFeederActive)
        MotionVectors::InjNoteMap(pResource, MapType, pMappedResource);
    return hr;
}

static void STDMETHODCALLTYPE HookedUnmap(
    ID3D11DeviceContext* pThis, ID3D11Resource* pResource, UINT Subresource)
{
    if (!gShutdownSignaled && Subresource == 0 && sMvFeederActive)
        MotionVectors::InjNoteUnmap(pResource);   // snapshot bytes BEFORE the unmap invalidates pData
    oUnmap(pThis, pResource, Subresource);
}

// ==================== Shadow atlas bind-time swap ====================
// Only active while gShadowSwapActive — every hook below early-outs on a
// single global read otherwise, so the feature costs one predictable branch
// per call when the resolution override is off.

// Match a resolved identity against the entry table — originals AND our own
// replacement textures (a state save/restore can re-bind the views we
// substituted). Stamps lastSeen so idle entries become evictable.
static int LookupShadowEntry(IUnknown* unk, bool* outIsReplacement)
{
    *outIsReplacement = false;
    if (!unk) return -1;
    size_t count = gShadowEntryCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; i++)
    {
        if (gShadowEntries[i].identity == unk)
        {
            gShadowEntries[i].lastSeen = gFrameIndex;
            return (int)i;
        }
        if (gShadowEntries[i].newIdentity == unk)
        {
            gShadowEntries[i].lastSeen = gFrameIndex;
            *outIsReplacement = true;
            return (int)i;
        }
    }
    return -1;
}

// What one bound RTV/DSV resolves to, for the shadow bind classifier.
struct ShadowViewInfo {
    bool bound = false;                // view was non-null
    IUnknown* unk = nullptr;           // owned identity ref; caller releases
    ID3D11Texture2D* tex = nullptr;    // owned; only set for adoption candidates
    D3D11_TEXTURE2D_DESC desc = {};    // only valid for candidates
    bool candidate = false;            // untracked texture with a shadow-atlas desc
};

static void ReleaseShadowViewInfo(ShadowViewInfo& v)
{
    if (v.unk) { v.unk->Release(); v.unk = nullptr; }
    if (v.tex) { v.tex->Release(); v.tex = nullptr; }
}

static void ResolveShadowView(ID3D11View* view, bool wantDepth, ShadowViewInfo& out)
{
    if (!view) return;
    out.bound = true;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    if (!res) return;
    res->QueryInterface(IID_IUnknown, (void**)&out.unk);

    bool isRepl = false;
    if (out.unk && LookupShadowEntry(out.unk, &isRepl) < 0)
    {
        // Untracked — an atlas-shaped texture here is an adoption candidate
        // (OGRE's texture pool can hand the recreated shadow workspace a
        // texture we never saw created).
        ID3D11Texture2D* tex = nullptr;
        res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
        if (tex)
        {
            D3D11_TEXTURE2D_DESC d = {};
            tex->GetDesc(&d);
            bool isDepthDesc = (d.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
            if (IsShadowAtlasDesc(&d) && isDepthDesc == wantDepth)
            {
                out.candidate = true;
                out.desc = d;
                out.tex = tex;  // ref kept; released by ReleaseShadowViewInfo
            }
            else
            {
                tex->Release();
            }
        }
    }
    res->Release();
}

// Entries the game hasn't bound or sampled for this many frames are pool-idle
// leftovers of a dead workspace generation.
static constexpr uint64_t kShadowEntryIdleFrames = 300;

// Drop idle entries when a new atlas generation is adopted, so replacement
// VRAM (which can be hundreds of MB at high overrides) doesn't accumulate
// across repeated shadow-mode switches. An actively-used entry is stamped
// every frame by the caster bind / lighting sample, so it can never be
// evicted; a mistaken eviction self-heals via re-adoption at the next bind.
static void EvictStaleShadowEntries()
{
    size_t count = gShadowEntryCount.load(std::memory_order_acquire);
    bool evicted = false;
    for (size_t i = 0; i < count; )
    {
        ShadowAtlasEntry& e = gShadowEntries[i];
        if (gFrameIndex - e.lastSeen <= kShadowEntryIdleFrames) { i++; continue; }
        Log("Shadow atlas entry evicted (idle %llu frames): %s %u",
            (unsigned long long)(gFrameIndex - e.lastSeen),
            e.isDepth ? "depth" : "color", e.desc.Width);
        ReleaseShadowReplacement(e);
        if (e.tex) { e.tex->Release(); e.tex = nullptr; }
        e = gShadowEntries[count - 1];
        gShadowEntries[count - 1] = {};
        count--;
        gShadowEntryCount.store(count, std::memory_order_release);
        evicted = true;
    }
    if (evicted)
    {
        bool anyReplacement = false;
        for (size_t i = 0; i < count; i++)
            if (gShadowEntries[i].newTex) { anyReplacement = true; break; }
        gShadowSwapActive = anyReplacement;
        if (!anyReplacement) gShadowPassScale = 1.0f;
    }
}

// Adopt a pool-reused atlas texture first seen at bind time. Creation-time
// tracking alone misses these: on a shadow-mode switch OGRE's texture pool
// can satisfy the recreated workspace with textures it already owns (no
// CreateTexture2D call), which left the real atlas untracked — unswapped,
// unscaled, and sampled at the wrong resolution (the 2026-07 RTWSM->CSM
// "broken cascades" report). Returns the new entry index, or -1.
static int AdoptShadowTexture(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& desc)
{
    EvictStaleShadowEntries();
    size_t eidx = gShadowEntryCount.load(std::memory_order_relaxed);
    if (eidx >= kMaxShadowIdentities)
    {
        static int sFullLogs = 0;
        if (sFullLogs < 4)
        {
            ++sFullLogs;
            Log("Shadow atlas adoption skipped: entry table full (%zu)", eidx);
        }
        return -1;
    }
    IUnknown* unk = nullptr;
    tex->QueryInterface(IID_IUnknown, (void**)&unk);
    if (!unk) return -1;

    ShadowAtlasEntry& e = gShadowEntries[eidx];
    e = {};
    e.tex = tex;
    e.tex->AddRef();
    e.identity = unk;  // weak ref (matches the creation-path convention)
    e.desc = desc;
    e.isDepth = (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
    e.lastSeen = gFrameIndex;
    e.vpScale = 1.0f;
    gShadowVanillaSize = desc.Width;
    gShadowEntryCount.store(eidx + 1, std::memory_order_release);
    unk->Release();

    static int sAdoptLogs = 0;
    if (sAdoptLogs < 8)
    {
        ++sAdoptLogs;
        Log("Shadow atlas %s texture adopted at bind (pool-reused, %ux%u) entry=%zu%s",
            e.isDepth ? "depth" : "color", desc.Width, desc.Height, eidx,
            sAdoptLogs == 8 ? " — further adoptions not logged" : "");
    }

    // The pool-reused atlas is vanilla-sized; re-queue the resize so the
    // override survives the mode switch (applies at next frame start).
    UINT wanted = gShadowAtlasOverride;
    if (wanted != 0 && wanted != desc.Width &&
        !gShadowResizePending.load(std::memory_order_relaxed))
    {
        gShadowResizeTarget = wanted;
        gShadowResizePending.store(true, std::memory_order_release);
    }
    return (int)eidx;
}

// Full classification of one OMSetRenderTargets bind: is it a shadow caster
// pass, which views should be substituted, and at what viewport scale.
struct ShadowBindDecision {
    bool  inShadowPass = false;
    bool  swapped = false;     // rtvs[]/dsv differ from the originals
    float scale = 1.0f;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
};

static void ClassifyShadowBind(UINT NumViews, ID3D11RenderTargetView* const* ppRTVs,
                               ID3D11DepthStencilView* pDSV, ShadowBindDecision& out)
{
    out.dsv = pDSV;
    UINT n = NumViews;
    if (n > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)
        n = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
    for (UINT i = 0; i < n; i++)
        out.rtvs[i] = ppRTVs ? ppRTVs[i] : nullptr;

    ShadowViewInfo dv;
    ResolveShadowView(pDSV, /*wantDepth=*/true, dv);
    ShadowViewInfo rv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    bool anyRtvBound = false;
    bool anyRtvCandidate = false;
    for (UINT i = 0; i < n; i++)
    {
        ResolveShadowView(out.rtvs[i], /*wantDepth=*/false, rv[i]);
        anyRtvBound |= rv[i].bound;
        anyRtvCandidate |= rv[i].candidate;
    }

    // Adoption: strict co-bind rule. An atlas-shaped color RTV and depth DSV
    // of EQUAL size bound together is the shadow caster pass; either half may
    // be a pool-reused texture we never saw created. Depth-only binds never
    // adopt (OGRE's depth pool shares depth textures across unrelated passes
    // — the DSV-only false-positive lesson), and neither do lone RTVs.
    if (dv.candidate || anyRtvCandidate)
    {
        bool dsvRepl = false;
        int dsvEntry = dv.candidate ? -1 : LookupShadowEntry(dv.unk, &dsvRepl);
        UINT dsvWidth = 0;
        if (dv.candidate)
            dsvWidth = dv.desc.Width;
        else if (dsvEntry >= 0 && !dsvRepl)
            dsvWidth = gShadowEntries[dsvEntry].desc.Width;
        if (dsvWidth != 0)
        {
            bool rtvPartner = false;
            for (UINT i = 0; i < n && !rtvPartner; i++)
            {
                if (rv[i].candidate)
                {
                    rtvPartner = (rv[i].desc.Width == dsvWidth);
                }
                else if (rv[i].unk)
                {
                    bool repl = false;
                    int idx = LookupShadowEntry(rv[i].unk, &repl);
                    rtvPartner = (idx >= 0 && !repl &&
                                  gShadowEntries[idx].desc.Width == dsvWidth);
                }
            }
            if (rtvPartner)
            {
                for (UINT i = 0; i < n; i++)
                    if (rv[i].candidate && rv[i].desc.Width == dsvWidth)
                        AdoptShadowTexture(rv[i].tex, rv[i].desc);
                if (dv.candidate)
                    AdoptShadowTexture(dv.tex, dv.desc);
            }
        }
    }

    // Detection (post-adoption). Same semantics as always: DSV match +
    // (RTV match or depth-only), OR RTV match alone. A lone DSV match with
    // an unrelated RTV is NOT a shadow pass (depth pool sharing).
    bool dsvRepl = false;
    int  dsvEntry = LookupShadowEntry(dv.unk, &dsvRepl);
    bool dsvIsShadow = (dsvEntry >= 0);
    int  rtvEntry[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    bool rtvRepl[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    bool rtvIsShadow = false;
    for (UINT i = 0; i < n; i++)
    {
        rtvRepl[i] = false;
        rtvEntry[i] = LookupShadowEntry(rv[i].unk, &rtvRepl[i]);
        rtvIsShadow |= (rtvEntry[i] >= 0);
    }
    out.inShadowPass = dsvIsShadow ? (rtvIsShadow || !anyRtvBound) : rtvIsShadow;

    if (out.inShadowPass)
    {
        // Substitute replacements — ALL or NOTHING. A tracked view without
        // its replacement (adopted this frame; resize lands next frame
        // start, or no override wanted) must leave the whole bind original,
        // or the RTV/DSV dimensions diverge and the pass renders into a
        // different texture than the lighting samples.
        bool allAvailable = true;
        float scale = 0.0f;
        ID3D11DepthStencilView* finalDSV = pDSV;
        bool anyRTVSwapped = false;

        for (UINT i = 0; i < n; i++)
        {
            if (rtvEntry[i] < 0) continue;
            ShadowAtlasEntry& e = gShadowEntries[rtvEntry[i]];
            if (rtvRepl[i])
            {
                if (scale == 0.0f) scale = e.vpScale;
            }
            else if (e.newRTV)
            {
                out.rtvs[i] = e.newRTV;
                anyRTVSwapped = true;
                if (scale == 0.0f) scale = e.vpScale;
            }
            else
            {
                allAvailable = false;
            }
        }

        if (dsvIsShadow)
        {
            ShadowAtlasEntry& e = gShadowEntries[dsvEntry];
            if (dsvRepl)
            {
                if (scale == 0.0f) scale = e.vpScale;
            }
            else if (e.newDSV)
            {
                finalDSV = e.newDSV;
                if (scale == 0.0f) scale = e.vpScale;
            }
            else
            {
                allAvailable = false;
            }
        }
        else if (dv.bound && rtvIsShadow)
        {
            // Foreign (untracked, non-atlas) depth co-bound with the shadow
            // color: substitute a companion depth so the target dimensions
            // match the swapped RTV.
            ID3D11DepthStencilView* comp = nullptr;
            size_t cnt = gShadowEntryCount.load(std::memory_order_acquire);
            for (size_t i = 0; i < cnt; i++)
            {
                if (!gShadowEntries[i].isDepth && gShadowEntries[i].companionDSV)
                    { comp = gShadowEntries[i].companionDSV; break; }
            }
            if (comp) finalDSV = comp;
            else      allAvailable = false;
        }

        if (allAvailable && (anyRTVSwapped || finalDSV != pDSV || scale != 0.0f))
        {
            out.dsv = finalDSV;
            out.swapped = anyRTVSwapped || (finalDSV != pDSV);
            out.scale = (scale != 0.0f) ? scale : 1.0f;
        }
        else
        {
            // Bind everything original at scale 1 — a consistent
            // vanilla-resolution pass until the replacements exist.
            for (UINT i = 0; i < n; i++)
                out.rtvs[i] = ppRTVs ? ppRTVs[i] : nullptr;
            out.dsv = pDSV;
            out.swapped = false;
            out.scale = 1.0f;
        }
    }

    ReleaseShadowViewInfo(dv);
    for (UINT i = 0; i < n; i++)
        ReleaseShadowViewInfo(rv[i]);
}

static void STDMETHODCALLTYPE HookedOMSetRenderTargets(
    ID3D11DeviceContext* pThis, UINT NumViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView)
{
    if (gShutdownSignaled)
    { oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView); return; }

    // Motion-vector pass: track GBuffer-pass entry/exit — independent of the shadow-swap feature,
    // so it must run before the !gShadowSwapActive early-out. Runs unconditionally now (the injected
    // MV path needs it even without geometry capture); CheckGBufferConfig is pointer-cached, cheap.
    bool isGBuf = GeometryCapture::CheckGBufferConfig(NumViews, ppRenderTargetViews, pDepthStencilView);
    GeometryCapture::OnOMSetRenderTargetsWithResult(isGBuf);

    // Temporal-jitter extension: a non-GBuffer pass that binds the scene depth buffer is a forward
    // scene pass (sky/water/transparents/particles) — its viewport must be jittered too (see the
    // gFwdScenePass declaration). GetGBufferDSV() is null until this frame's GBuffer pass, so passes
    // before it can never false-positive.
    gFwdScenePass = !isGBuf && pDepthStencilView &&
                    pDepthStencilView == GeometryCapture::GetGBufferDSV();

    // Shader-injected motion vectors: append the velocity RT (SV_Target3) to the GBuffer bind, do
    // the once-per-frame reproj+clear, and bind the reproj CB at b13 for the patched vertex shaders.
    // Gated on the feeder: when the upscaler (and MV debug viz) are off, OGRE keeps its own 3-RTV bind
    // and the patched shaders' SV_Target3 writes go nowhere — no extra RT, no reproj pass, no clear.
    if (sMvFeederActive && isGBuf && NumViews == 3 && gDevice && ppRenderTargetViews && ppRenderTargetViews[0])
    {
        ID3D11RenderTargetView* velRTV = MotionVectors::InjEnsureVelRTV(ppRenderTargetViews[0]);
        if (velRTV)
        {
            MotionVectors::InjBeginGBuffer(pThis);
            ID3D11Buffer* reprojCB = MotionVectors::GetInjReprojCB();
            if (reprojCB) pThis->VSSetConstantBuffers(13, 1, &reprojCB);
            ID3D11RenderTargetView* rtvs4[4] = {
                ppRenderTargetViews[0], ppRenderTargetViews[1], ppRenderTargetViews[2], velRTV };
            oOMSetRenderTargets(pThis, 4, rtvs4, pDepthStencilView);
            // This early-out skips the shadow-state update below — a GBuffer bind is never
            // a shadow pass, so clear any stale caster state left over from the shadow pass.
            gInShadowPass = false;
            gShadowPassScale = 1.0f;
            return;
        }
    }

    // Classification also runs whenever an override is set, even with no
    // swap live and no entries tracked: bind-time adoption must see the
    // caster pass of a pool-reused atlas (a shadow-mode switch can rebuild
    // the whole workspace from OGRE's texture pool with zero creations).
    if (!gShadowSwapActive && gShadowAtlasOverride == 0)
    {
        oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView);
        // No managed shadow bind can be live while the swap is off: clear stale caster state
        // so a leftover gShadowPassScale never scales a later icon-atlas/UI viewport.
        gInShadowPass = false;
        gShadowPassScale = 1.0f;
        return;
    }

    ShadowBindDecision sb;
    ClassifyShadowBind(NumViews, ppRenderTargetViews, pDepthStencilView, sb);
    if (sb.inShadowPass)
    {
        oOMSetRenderTargets(pThis, NumViews,
            sb.swapped ? sb.rtvs : ppRenderTargetViews, sb.dsv);
        gShadowPassScale = sb.scale;

        // OGRE sets RSSetViewports BEFORE OMSetRenderTargets, so the viewport
        // hook (gated on gInShadowPass) misses it. On shadow-pass entry, query
        // the already-set viewport and scale it to match the replacement atlas.
        if (!gInShadowPass && sb.scale != 1.0f)
        {
            UINT nVP = 0;
            pThis->RSGetViewports(&nVP, nullptr);
            if (nVP > 0 && nVP <= D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
            {
                D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
                pThis->RSGetViewports(&nVP, vps);
                float s = sb.scale;
                for (UINT i = 0; i < nVP; i++)
                {
                    vps[i].TopLeftX *= s;
                    vps[i].TopLeftY *= s;
                    vps[i].Width    *= s;
                    vps[i].Height   *= s;
                }
                oRSSetViewports(pThis, nVP, vps);
            }
        }
    }
    else
    {
        oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView);
        gShadowPassScale = 1.0f;
    }
    gInShadowPass = sb.inShadowPass;
}

// OGRE 2.0 binds RTV/DSV through this combined call rather than the plain
// OMSetRenderTargets — so the shadow caster pass is invisible to the plain
// hook alone. Mirror the same classification + swap logic here so the swap
// works regardless of which API the engine used.
static void STDMETHODCALLTYPE HookedOMSetRenderTargetsAndUAV(
    ID3D11DeviceContext* pThis,
    UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts)
{
    if (gShutdownSignaled)
    {
        oOMSetRenderTargetsAndUAV(pThis, NumRTVs, ppRenderTargetViews,
            pDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews,
            pUAVInitialCounts);
        return;
    }

    // Motion-vector pass: GBuffer-pass tracking (see the plain-OMSet hook). OGRE 2.0
    // binds the GBuffer MRT through this combined call, so detection must live here too.
    bool isGBufU = GeometryCapture::CheckGBufferConfig(NumRTVs, ppRenderTargetViews, pDepthStencilView);
    if (GeometryCapture::GetCaptureFlags() != 0)
        GeometryCapture::OnOMSetRenderTargetsWithResult(isGBufU);

    // Forward-scene-pass tracking for the temporal-jitter extension (see the plain-OMSet hook).
    gFwdScenePass = !isGBufU && pDepthStencilView &&
                    pDepthStencilView == GeometryCapture::GetGBufferDSV();

    // See the plain-OMSet hook: classification also runs whenever an
    // override is set, so bind-time adoption can see pool-reused atlases
    // (OGRE 2.0 binds the caster pass through this combined call).
    if (!gShadowSwapActive && gShadowAtlasOverride == 0)
    {
        oOMSetRenderTargetsAndUAV(pThis, NumRTVs, ppRenderTargetViews,
            pDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews,
            pUAVInitialCounts);
        return;
    }

    ShadowBindDecision sb;
    ClassifyShadowBind(NumRTVs, ppRenderTargetViews, pDepthStencilView, sb);
    if (sb.inShadowPass)
    {
        oOMSetRenderTargetsAndUAV(pThis, NumRTVs,
            sb.swapped ? sb.rtvs : ppRenderTargetViews,
            sb.dsv, UAVStartSlot, NumUAVs, ppUnorderedAccessViews,
            pUAVInitialCounts);
        gShadowPassScale = sb.scale;

        if (!gInShadowPass && sb.scale != 1.0f)
        {
            UINT nVP = 0;
            pThis->RSGetViewports(&nVP, nullptr);
            if (nVP > 0 && nVP <= D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
            {
                D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
                pThis->RSGetViewports(&nVP, vps);
                float s = sb.scale;
                for (UINT i = 0; i < nVP; i++)
                {
                    vps[i].TopLeftX *= s;
                    vps[i].TopLeftY *= s;
                    vps[i].Width    *= s;
                    vps[i].Height   *= s;
                }
                oRSSetViewports(pThis, nVP, vps);
            }
        }
    }
    else
    {
        oOMSetRenderTargetsAndUAV(pThis, NumRTVs, ppRenderTargetViews,
            pDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews,
            pUAVInitialCounts);
        gShadowPassScale = 1.0f;
    }
    gInShadowPass = sb.inShadowPass;
}

// Routes the deferred-lighting (and any other) shadow atlas sampling to the
// resized replacement textures.
static void STDMETHODCALLTYPE HookedPSSetShaderResources(
    ID3D11DeviceContext* pThis, UINT StartSlot, UINT NumViews,
    ID3D11ShaderResourceView* const* ppShaderResourceViews)
{
    if (!gShadowSwapActive || gShutdownSignaled || !ppShaderResourceViews ||
        NumViews == 0 || NumViews > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
    {
        oPSSetShaderResources(pThis, StartSlot, NumViews, ppShaderResourceViews);
        return;
    }

    ID3D11ShaderResourceView* swapped[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    bool anySwap = false;
    for (UINT i = 0; i < NumViews; i++)
    {
        swapped[i] = ppShaderResourceViews[i];
        if (!swapped[i]) continue;
        ID3D11Resource* res = nullptr;
        swapped[i]->GetResource(&res);
        int idx = FindShadowEntry(res);
        if (res) res->Release();
        if (idx >= 0 && gShadowEntries[idx].newSRV)
        {
            swapped[i] = gShadowEntries[idx].newSRV;
            anySwap = true;
        }
    }

    if (anySwap)
        oPSSetShaderResources(pThis, StartSlot, NumViews, swapped);
    else
        oPSSetShaderResources(pThis, StartSlot, NumViews, ppShaderResourceViews);
}

// Scales viewports set DURING the shadow pass (the on-entry catch-up in the
// OM hooks covers the viewport OGRE sets before binding the targets).
static void STDMETHODCALLTYPE HookedRSSetViewports(
    ID3D11DeviceContext* pThis, UINT NumViewports,
    const D3D11_VIEWPORT* pViewports)
{
    // Shadow-atlas resolution override: scale the shadow-pass viewport to the
    // resized atlas. gShadowPassScale is the CURRENT pass's per-entry scale
    // (the two live atlas generations can differ in size across a mode switch).
    if (gInShadowPass && gShadowSwapActive && gShadowPassScale != 1.0f &&
        !gShutdownSignaled && pViewports && NumViewports > 0)
    {
        D3D11_VIEWPORT scaled[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        UINT n = (NumViewports <= D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
               ? NumViewports : D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        float s = gShadowPassScale;
        for (UINT i = 0; i < n; i++)
        {
            scaled[i] = pViewports[i];
            scaled[i].TopLeftX *= s;
            scaled[i].TopLeftY *= s;
            scaled[i].Width    *= s;
            scaled[i].Height   *= s;
        }
        oRSSetViewports(pThis, n, scaled);
        return;
    }

    // Temporal upscaling: offset the scene viewport by this frame's sub-pixel jitter. Applies to the
    // GBuffer pass AND (JitterForwardPasses) the forward scene passes that bind the same depth buffer
    // (sky/water/transparents) — unjittered forward content would wobble by the jitter amplitude once
    // the upscaler jitter-compensates the frame. Shadow/fullscreen/UI viewports are untouched
    // (compositor quads inside a jittered forward pass are un-jittered per-draw in HookedDraw).
    if (gJitterEnabled && !gShutdownSignaled && !gInShadowPass && tSuppressVpJitter == 0 &&
        (gJitterPxX != 0.0f || gJitterPxY != 0.0f) &&
        pViewports && NumViewports > 0 &&
        (GeometryCapture::IsInGBufferPass() || (gJitterForwardPasses && gFwdScenePass)))
    {
        if (!GeometryCapture::IsInGBufferPass())
        {
            static bool sFwdJitterLogged = false;
            if (!sFwdJitterLogged)
            { sFwdJitterLogged = true; Log("Upscaler: forward-scene-pass viewport jitter engaged"); }
        }
        D3D11_VIEWPORT j[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        UINT n = (NumViewports <= D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
               ? NumViewports : D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        for (UINT i = 0; i < n; i++)
        {
            j[i] = pViewports[i];
            j[i].TopLeftX += gJitterPxX;
            j[i].TopLeftY += gJitterPxY;
        }
        oRSSetViewports(pThis, n, j);
        return;
    }

    oRSSetViewports(pThis, NumViewports, pViewports);
}

// ==================== Swap chain hooks (ImGui) ====================

static void TickGuiOnPresent(IDXGISwapChain* swapChain, const char* via)
{
    // ImGui's own viewport set must never pick up the scene jitter either.
    VpJitterSuppressScope vpSuppress;

    ++gPresentHookCallCount;

    // Periodic diagnostic: confirm Present is firing and show init state
    if (gPresentHookCallCount <= 5 ||
        (gPresentHookCallCount <= 600 && (gPresentHookCallCount % 60) == 0))
    {
        Log("Present #%llu via %s: captured=%d guiDone=%d boot=%d draws=%llu",
            (unsigned long long)gPresentHookCallCount, via,
            (int)gDeviceCaptured,
            (int)gGuiInitDone,
            (int)sTryCaptureFromBoot,
            (unsigned long long)gDrawHookCallCount);
    }

    // Survey: clear leftover frames on any capture-state edge. A capture stopped early
    // (Survey::Stop from the GUI) leaves its partial frames in sSurveyFrames — without this
    // they leak into the next capture's WriteSummary, and repeated stop/start cycles grow memory.
    static bool sSurveyWasActive = false;
    const bool surveyActive = Survey::IsActive();
    if (surveyActive != sSurveyWasActive)
    {
        sSurveyFrames.clear();
        sSurveyWasActive = surveyActive;
    }

    // Survey: finalize frame at Present boundary
    if (surveyActive)
    {
        try
        {
            SurveyFrameData frameData = SurveyRecorder::OnEndFrame();
            SurveyWriter::WriteFrame(frameData, Survey::GetOutputDir());
            sSurveyFrames.push_back(std::move(frameData));

            if (Survey::OnFrameEnd())
            {
                // Survey just finished — write shaders and summary
                SurveyWriter::WriteShaders(Survey::GetOutputDir());
                SurveyWriter::WriteSummary(sSurveyFrames.data(), (int)sSurveyFrames.size(),
                                            Survey::GetOutputDir());
                sSurveyFrames.clear();
                SurveyRecorder::Shutdown();
            }
        }
        catch (...)
        {
            // Frames are tens of MB each — a bad_alloc here must never escape into the
            // game's Present (it would terminate the process). Stop the capture instead.
            Log("Survey: exception during capture — stopping survey, accumulated frames dropped");
            sSurveyFrames.clear();
            SurveyRecorder::Shutdown();
            Survey::Stop();
        }
    }

    if (!gGuiInitDone)
    {
        if (DustGUI::Init(swapChain, gDevice, gContext))
        {
            gGuiInitDone = true;
            gGuiSwapChain = swapChain;   // the GUI's home chain (present/resize identity, compare-only)
            Log("GUI initialized successfully (swap chain %p via %s)", (void*)swapChain,
                sTryCaptureFromBoot ? "DustBoot preload" : "runtime discovery");
        }
        else
        {
            static int sRetryCount = 0;
            if (sRetryCount < 3)
                Log("GUI init failed, will retry (attempt %d)", ++sRetryCount);
        }
    }

    DustGUI::Render();
}

// The vtable patch lives on the SHARED IDXGISwapChain vtable, so the Present/ResizeBuffers hooks fire
// for EVERY swapchain in the process — including the second swapchain that driver features (NVIDIA
// Smooth Motion) and overlays create. Ticking the GUI on those foreign presents draws ImGui at
// foreign present timing (often from another thread) -> intermittent GUI flicker with Smooth Motion
// on; worse, a foreign ResizeBuffers would rebind the GUI backbuffer RTV to the WRONG swapchain.
//
// TWO DEAD ENDS, both verified in-game 2026-07-14 — do not retry:
// 1. Device-pointer identity (swapChain->GetDevice() == gDevice): with Smooth Motion the driver WRAPS
//    the app device, so the swapchain and the immediate context return DIFFERENT ID3D11Device
//    pointers for the same logical device -> rejected the GAME's own chain, killed the GUI.
// 2. Render-thread identity: under Smooth Motion the driver takes over presentation COMPLETELY — the
//    game renders into a proxy chain and NO present ever arrives on the game render thread; SM's
//    pacing thread (log: tid 7944 vs renderTid 8416) presents everything, including the frames the
//    GUI must be drawn on to be visible at all. A thread gate can never pass.
// What remains reliable is CHAIN AFFINITY: tick the GUI only on the chain it initialized on (the
// chain that presented during the menu — the visible/presenter chain, under SM too), and never on a
// present nested inside our own hook. That excludes the second chain seen presenting in the wild
// (foreign-chain ticks were the flicker suspect) without any device/thread assumptions.
// (gGameRenderThreadId / tPresentDepth / gGuiSwapChain are declared near gHookedSwapChain — they're
// written from the draw hook and the GUI init path, which live earlier in this file.)
static bool IsGamePresent(IDXGISwapChain* sc, bool topLevel)
{
    if (!sc) return false;
    const char* skip = nullptr;
    if (!topLevel)
        skip = "nested";                                            // driver shim presenting inside our hook
    else if (gGuiSwapChain && sc != gGuiSwapChain)
        skip = "not the GUI chain";                                 // second chain (proxy / overlay)
    if (skip)
    {
        static uint64_t sSkipped = 0;   // throttled: foreign chains can present every frame
        if ((sSkipped++ % 3600) == 0)
            Log("Present skipped (%s): chain=%p tid=%u renderTid=%u depth-top=%d (seen %llu)",
                skip, (void*)sc, (unsigned)GetCurrentThreadId(), (unsigned)gGameRenderThreadId,
                (int)topLevel, (unsigned long long)sSkipped);
        return false;
    }
    return true;
}

static HRESULT STDMETHODCALLTYPE HookedPresent(
    IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags)
{
    ++tPresentDepth;
    if (!gShutdownSignaled && IsGamePresent(pThis, tPresentDepth == 1))
        TickGuiOnPresent(pThis, "Present");
    HRESULT hr = oPresent(pThis, SyncInterval, Flags);
    --tPresentDepth;
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedPresent1(
    IDXGISwapChain1* pThis, UINT SyncInterval, UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    ++tPresentDepth;
    if (!gShutdownSignaled && IsGamePresent(pThis, tPresentDepth == 1))
        TickGuiOnPresent(pThis, "Present1");
    HRESULT hr = oPresent1(pThis, SyncInterval, PresentFlags, pPresentParameters);
    --tPresentDepth;
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    if (gShutdownSignaled) return oResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    // A foreign swapchain (Smooth Motion / overlay) resizing must NOT touch the GUI backbuffer:
    // RecreateBackBuffer(pThis) would rebind the GUI RTV to the wrong swapchain. The GUI's home chain
    // is the one it initialized on (compare-only pointer; null until GUI init -> legacy behavior).
    if (gGuiSwapChain && pThis != gGuiSwapChain)
    {
        Log("Foreign swapchain %p ResizeBuffers (%ux%u) — GUI backbuffer untouched", (void*)pThis, Width, Height);
        return oResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    // Block Render() while we tear down and recreate the back buffer
    DustGUI::SetResizeInProgress(true);

    // Release only the back buffer RTV — keep ImGui context, WndProc, DInput hooks intact
    DustGUI::ReleaseBackBuffer();

    HRESULT hr = oResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    // Recreate back buffer RTV with the new swapchain dimensions
    if (SUCCEEDED(hr) && gGuiInitDone)
        DustGUI::RecreateBackBuffer(pThis);

    DustGUI::SetResizeInProgress(false);
    return hr;
}

// ==================== Install ====================

static const int VTIDX_DEVICE_CreateTexture2D       = 5;
static const int VTIDX_DEVICE_CreateSamplerState    = 23;
static const int VTIDX_DEVICE_CreateInputLayout     = 11;
static const int VTIDX_DEVICE_CreateVertexShader    = 12;
static const int VTIDX_DEVICE_CreatePixelShader     = 15;
static const int VTIDX_CTX_PSSetShaderResources     = 8;
static const int VTIDX_CTX_DrawIndexed              = 12;
static const int VTIDX_CTX_Draw                     = 13;
static const int VTIDX_CTX_Map                      = 14;
static const int VTIDX_CTX_Unmap                    = 15;
static const int VTIDX_CTX_DrawIndexedInstanced     = 20;
static const int VTIDX_CTX_DrawInstanced            = 21;
static const int VTIDX_CTX_OMSetRenderTargets       = 33;
static const int VTIDX_CTX_OMSetRenderTargetsAndUAV = 34;
static const int VTIDX_CTX_RSSetViewports           = 44;
// VTIDX_SC_* constants moved to top of file (needed by deferred hook code)

bool Install()
{
    Log("Creating temporary D3D11 device + swap chain to discover function addresses...");

    // Need a dummy window for the swap chain
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.lpszClassName = "DustDummy";
    wc.hInstance = GetModuleHandleA(nullptr);
    RegisterClassExA(&wc);
    HWND dummyWnd = CreateWindowExA(0, "DustDummy", "", WS_OVERLAPPED,
                                     0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width = 1;
    scd.BufferDesc.Height = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummyWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    IDXGISwapChain* tmpSwapChain = nullptr;
    ID3D11Device* tmpDevice = nullptr;
    ID3D11DeviceContext* tmpContext = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &tmpSwapChain, &tmpDevice, nullptr, &tmpContext);

    if (FAILED(hr))
    {
        Log("ERROR: Failed to create temporary D3D11 device: 0x%08X", hr);
        DestroyWindow(dummyWnd);
        UnregisterClassA("DustDummy", wc.hInstance);
        return false;
    }

    void** devVtable = *reinterpret_cast<void***>(tmpDevice);
    void** ctxVtable = *reinterpret_cast<void***>(tmpContext);
    void** scVtable  = *reinterpret_cast<void***>(tmpSwapChain);

    void* addrCreateTex2D  = devVtable[VTIDX_DEVICE_CreateTexture2D];
    void* addrCreateSampler= devVtable[VTIDX_DEVICE_CreateSamplerState];
    void* addrCreateInpLay = devVtable[VTIDX_DEVICE_CreateInputLayout];
    void* addrCreateVS     = devVtable[VTIDX_DEVICE_CreateVertexShader];
    void* addrCreatePS     = devVtable[VTIDX_DEVICE_CreatePixelShader];
    void* addrDraw         = ctxVtable[VTIDX_CTX_Draw];
    void* addrDrawIndexed  = ctxVtable[VTIDX_CTX_DrawIndexed];
    void* addrMap          = ctxVtable[VTIDX_CTX_Map];
    void* addrUnmap        = ctxVtable[VTIDX_CTX_Unmap];
    void* addrDrawIdxInst  = ctxVtable[VTIDX_CTX_DrawIndexedInstanced];
    void* addrDrawInst     = ctxVtable[VTIDX_CTX_DrawInstanced];
    void* addrOMSetRT      = ctxVtable[VTIDX_CTX_OMSetRenderTargets];
    void* addrOMSetRTUAV   = ctxVtable[VTIDX_CTX_OMSetRenderTargetsAndUAV];
    void* addrPSSetSRVs    = ctxVtable[VTIDX_CTX_PSSetShaderResources];
    void* addrRSSetVPs     = ctxVtable[VTIDX_CTX_RSSetViewports];
    void* addrPresent      = scVtable[VTIDX_SC_Present];
    void* addrResizeBuf    = scVtable[VTIDX_SC_ResizeBuffers];

    // Try to find Present1 (IDXGISwapChain1, DXGI 1.2). Many flip-model
    // swap chains route through this and bypass Present entirely.
    void* addrPresent1 = nullptr;
    {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(tmpSwapChain->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1)
        {
            void** sc1Vtable = *reinterpret_cast<void***>(sc1);
            addrPresent1 = sc1Vtable[VTIDX_SC1_Present1];
            sc1->Release();
        }
    }

    Log("Function addresses discovered:");
    Log("  CreateVertexShader    = %p", addrCreateVS);
    Log("  CreatePixelShader     = %p", addrCreatePS);
    Log("  Draw                  = %p", addrDraw);
    Log("  DrawIndexed           = %p", addrDrawIndexed);
    Log("  DrawIndexedInstanced  = %p", addrDrawIdxInst);
    Log("  DrawInstanced         = %p", addrDrawInst);
    Log("  OMSetRenderTargets    = %p", addrOMSetRT);
    Log("  OMSetRTAndUAV         = %p", addrOMSetRTUAV);
    Log("  PSSetShaderResources  = %p", addrPSSetSRVs);
    Log("  RSSetViewports        = %p", addrRSSetVPs);
    Log("  Present               = %p", addrPresent);
    Log("  Present1              = %p", addrPresent1);
    Log("  ResizeBuffers         = %p", addrResizeBuf);

    tmpSwapChain->Release();
    tmpContext->Release();
    tmpDevice->Release();
    DestroyWindow(dummyWnd);
    UnregisterClassA("DustDummy", wc.hInstance);

    // Hook D3DCompile for runtime shader patching (no disk writes).
    // Kenshi ships D3DCompiler_43.dll (used by OGRE's RenderSystem_Direct3D11).
    // Must hook before OGRE compiles any shaders.
    Log("Installing D3D11 function hooks...");

    bool ok = true;

    {
        const char* compilerDlls[] = { "D3DCompiler_43.dll", "d3dcompiler_47.dll" };
        bool hooked = false;
        for (const char* dllName : compilerDlls)
        {
            // GetModuleHandle first (already loaded by RenderSystem), LoadLibrary as fallback
            HMODULE hD3DCompiler = GetModuleHandleA(dllName);
            if (!hD3DCompiler)
                hD3DCompiler = LoadLibraryA(dllName);
            if (!hD3DCompiler)
                continue;

            void* addrD3DCompile = (void*)GetProcAddress(hD3DCompiler, "D3DCompile");
            if (!addrD3DCompile)
                continue;

            if (KenshiLib::AddHook(addrD3DCompile, (void*)ShaderPatch::HookedD3DCompile,
                                   (void**)&ShaderPatch::oD3DCompile) == KenshiLib::SUCCESS)
            {
                Log("  D3DCompile hook installed via %s (runtime shader patching enabled)", dllName);
                hooked = true;
                break;
            }
        }
        if (!hooked)
        { Log("WARNING: Could not hook D3DCompile, shader patching disabled"); }
    }

    // Init survey defaults from INI
    {
        std::string ini = DustLogDir() + "Dust.ini";
        Survey::InitFromINI(ini.c_str());
    }

    if (KenshiLib::AddHook(addrCreateTex2D, (void*)HookedCreateTexture2D,
                           (void**)&oCreateTexture2D) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook CreateTexture2D"); ok = false; }

    // Non-fatal: only affects the DLSS mip-bias sharpening.
    if (KenshiLib::AddHook(addrCreateSampler, (void*)HookedCreateSamplerState,
                           (void**)&oCreateSamplerState) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook CreateSamplerState (DLSS mip-bias sharpening disabled)"); }

    if (KenshiLib::AddHook(addrCreateInpLay, (void*)HookedCreateInputLayout,
                           (void**)&oCreateInputLayout) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook CreateInputLayout (vertex-declaration RE disabled)"); }

    if (KenshiLib::AddHook(addrCreateVS, (void*)HookedCreateVertexShader,
                           (void**)&oCreateVertexShader) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook CreateVertexShader (shader source tracking for VS disabled)"); }

    if (KenshiLib::AddHook(addrCreatePS, (void*)HookedCreatePixelShader,
                           (void**)&oCreatePixelShader) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook CreatePixelShader"); ok = false; }

    if (KenshiLib::AddHook(addrDraw, (void*)HookedDraw,
                           (void**)&oDraw) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook Draw"); ok = false; }

    if (KenshiLib::AddHook(addrDrawIndexed, (void*)HookedDrawIndexed,
                           (void**)&oDrawIndexed) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook DrawIndexed"); ok = false; }

    if (KenshiLib::AddHook(addrDrawIdxInst, (void*)HookedDrawIndexedInstanced,
                           (void**)&oDrawIndexedInstanced) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook DrawIndexedInstanced"); ok = false; }

    // Non-fatal: only used to pin the MV reproj CB on the non-indexed instanced path.
    if (KenshiLib::AddHook(addrDrawInst, (void*)HookedDrawInstanced,
                           (void**)&oDrawInstanced) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook DrawInstanced (non-indexed instanced MV will be wrong)"); }

    // Map/Unmap: needed to proxy and shadow the skinned bone-palette CB for spatial pose matching.
    // Non-fatal if it fails — skinned animation MVs fall back to camera reprojection.
    if (KenshiLib::AddHook(addrMap, (void*)HookedMap,
                           (void**)&oMap) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook Map (skinned animation motion vectors disabled)"); }

    if (KenshiLib::AddHook(addrUnmap, (void*)HookedUnmap,
                           (void**)&oUnmap) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook Unmap (skinned animation motion vectors disabled)"); }

    if (KenshiLib::AddHook(addrOMSetRT, (void*)HookedOMSetRenderTargets,
                           (void**)&oOMSetRenderTargets) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook OMSetRenderTargets"); ok = false; }

    if (KenshiLib::AddHook(addrOMSetRTUAV, (void*)HookedOMSetRenderTargetsAndUAV,
                           (void**)&oOMSetRenderTargetsAndUAV) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook OMSetRenderTargetsAndUAV"); ok = false; }

    if (KenshiLib::AddHook(addrPSSetSRVs, (void*)HookedPSSetShaderResources,
                           (void**)&oPSSetShaderResources) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook PSSetShaderResources"); ok = false; }

    if (KenshiLib::AddHook(addrRSSetVPs, (void*)HookedRSSetViewports,
                           (void**)&oRSSetViewports) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook RSSetViewports"); ok = false; }

    // Present/Present1/ResizeBuffers hooks are DEFERRED until the first Draw call.
    // This avoids a race with overlay DLLs (Steam, Discord, ReShade) that also hook
    // Present during their initialization. By hooking later, we wrap their hooks
    // and always fire. Addresses are saved and used in TryInstallSwapChainHooks().
    sSavedAddrPresent  = addrPresent;
    sSavedAddrPresent1 = addrPresent1;
    sSavedAddrResizeBuf = addrResizeBuf;
    Log("  Present hooks DEFERRED (Present=%p, Present1=%p, ResizeBuffers=%p)",
        addrPresent, addrPresent1, addrResizeBuf);

    if (ok)
        Log("Device/Context hooks installed, swap chain hooks deferred to first Draw");

    return ok;
}

void SignalShutdown()
{
    gShutdownSignaled = true;
}

bool IsShutdownSignaled()
{
    return gShutdownSignaled;
}

bool MvInjectionWanted()
{
    // Read the intent STRAIGHT FROM Dust.ini, cached for the session.
    //
    // Do NOT gate on gDlssWanted / sUpsEnabled here: both are populated during effect init, which
    // happens AFTER the game has already compiled a chunk of its GBuffer shaders. Gating on them
    // injects MV into the shaders compiled later but not the earlier ones, so the engine ends up
    // pairing an MV-injected VS with an MV-less PS (verified in a capture: objects.hlsl main_ps
    // compiled 10x at 7510->8493 = dither-only BEFORE "DLSS on", then 7510->8676 = dither+MV after).
    // That inconsistency is exactly what this gate exists to avoid, so the decision must be stable
    // from the very first compile. DustLogDir() is set at DLL attach, so the ini is readable here.
    //
    // Consequence (accepted): toggling the upscaler at runtime does not retro-inject already-compiled
    // shaders — it takes effect on the next launch. That was already true; now it is at least
    // all-or-nothing for the whole session instead of split by compile timing.
    static int cached = -1;
    if (cached < 0)
    {
        std::string ini = DustLogDir() + "Dust.ini";
        const bool dlss = GetPrivateProfileIntA("Upscaling", "DLSS", 0, ini.c_str()) != 0;
        const bool viz  = GetPrivateProfileIntA("Upscaling", "ShowMotionVectors", 0, ini.c_str()) != 0;
        cached = (dlss || viz) ? 1 : 0;
    }
    return cached != 0;
}

} // namespace D3D11Hook

// Runtime control surface for the GUI. Global namespace (matches UpscalerControl.h); the resolve state
// lives as file-static in D3D11Hook, reachable by qualified name from this same translation unit.
namespace UpscalerControl
{
    bool  Available()          { return Upscaler::IsAvailable(); }
    bool  GetEnabled()         { return D3D11Hook::sUpsEnabled; }
    void  SetEnabled(bool e)   { D3D11Hook::sUpsEnabled = e; }
    int   GetPreset()          { return D3D11Hook::sUpsPresetIdx; }
    void  SetPreset(int p)     { if (p >= 0 && p < Upscaler::PRESET_COUNT) D3D11Hook::sUpsPresetIdx = p; }
    float GetSharpness()       { return D3D11Hook::sUpsSharpness; }
    void  SetSharpness(float s){ D3D11Hook::sUpsSharpness = s < 0 ? 0 : (s > 1 ? 1 : s); }
    int   GetBackend()         { return D3D11Hook::sUpsBackend; }
    void  SetBackend(int b)    { D3D11Hook::sUpsBackend = (b >= 0 && b <= 2) ? b : 0; }
}
