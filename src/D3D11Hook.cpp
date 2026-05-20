#include "D3D11Hook.h"
#include "DustGUI.h"
#include "PipelineDetector.h"
#include "EffectLoader.h"
#include "ResourceRegistry.h"
#include "Survey.h"
#include "SurveyRecorder.h"
#include "SurveyWriter.h"
#include "ShaderPatch.h"
#include "OgreSwapHook.h"
#include "ShadowProbe.h"
#include "PssmDetour.h"
#include "ShaderMetadata.h"
#include "ShaderDatabase.h"
#include "GeometryCapture.h"
#include "POMState.h"
#include "TerrainTess.h"
#include "CSMCapture.h"
#include "DustLog.h"
#include <tracy/Tracy.hpp>
#ifdef TRACY_ENABLE
#include <tracy/TracyD3D11.hpp>
#endif
#include <core/Functions.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <string>
#include <cstring>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <atomic>

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

// Camera extraction from the game's deferred lighting CB
// (staging CBs are in gCameraStagingCB[2], declared near ExtractCameraData)
static DustCameraData gCameraData = {};
static bool gCameraDataExtracted = false; // per-frame flag
static bool gCameraDataEverValid  = false; // sticky once first extraction succeeds

// VTable indices — swap chain
static const int VTIDX_SC_Present        = 8;
static const int VTIDX_SC_ResizeBuffers  = 13;
static const int VTIDX_SC1_Present1      = 22;

// VTable indices — device (used by Install() for Detours hooks)
static const int VTIDX_DEVICE_CreateBuffer          = 3;
static const int VTIDX_DEVICE_CreateTexture2D       = 5;
static const int VTIDX_DEVICE_CreateVertexShader    = 12;
static const int VTIDX_DEVICE_CreatePixelShader     = 15;

// VTable indices — context (used by VTable hook infrastructure)
static const int VTIDX_CTX_PSSetShaderResources     = 8;
static const int VTIDX_CTX_PSSetShader              = 9;
static const int VTIDX_CTX_VSSetShader              = 11;
static const int VTIDX_CTX_DrawIndexed              = 12;
static const int VTIDX_CTX_Draw                     = 13;
static const int VTIDX_CTX_Map                      = 14;
static const int VTIDX_CTX_Unmap                    = 15;
static const int VTIDX_CTX_DrawIndexedInstanced     = 20;
static const int VTIDX_CTX_OMSetRenderTargets       = 33;
static const int VTIDX_CTX_OMSetRenderTargetsAndUAV = 34;
static const int VTIDX_CTX_RSSetViewports           = 44;
static const int VTIDX_CTX_CopyResource             = 47;
static const int VTIDX_CTX_UpdateSubresource        = 48;

// Deferred Present hooking: addresses saved from temp device, installed later
static void* sSavedAddrPresent   = nullptr;
static void* sSavedAddrPresent1  = nullptr;
static void* sSavedAddrResizeBuf = nullptr;
static bool  sSwapChainHooked    = false;

// Survey: collected frame data for writing after all frames captured
static std::vector<SurveyFrameData> sSurveyFrames;

static bool gDeviceRemovedThisFrame = false;
static volatile bool gShutdownSignaled = false;

// True once the game has progressed past loader/splash. Set by SignalGameAlive
// from either TitleScreen::show(true) (main menu reached) or GameWorld::mainLoop
// (in-game reached). Anything Presenting before this is loader/splash code
// (Havok loader window, etc.) — initializing ImGui against a window that's
// about to be destroyed is what crashes some users on startup.
static volatile bool gGameAlive = false;

// Latched on the first Present we accept after gGameAlive flips. VTable
// hooks fire on every swap chain that shares the vtable; we only want to act
// on the game's main one.
static IDXGISwapChain* gCanonicalSwapChain = nullptr;

// Plugin-supplied override for the shadow atlas dimension (square). 0 = no
// override; HookedCreateTexture2D rewrites the desc when this is non-zero
// and the descriptor matches the shadow atlas / depth signature.
static UINT gShadowAtlasOverride = 0;
static constexpr size_t kMaxShadowIdentities = 8;

// Forward declaration — defined with the other hook function pointers below.
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateTexture2D)(
    ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
    const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static PFN_CreateTexture2D oCreateTexture2D;

UINT GetShadowAtlasResolution()          { return gShadowAtlasOverride; }

// Runtime shadow atlas resize state. Each entry tracks one shadow atlas
// texture (color or depth) created by the game, plus an optional replacement
// texture/views at the user's chosen resolution. Swap happens at bind time.
struct ShadowAtlasEntry {
    ID3D11Texture2D*          tex;          // AddRef'd original
    IUnknown*                 identity;     // QI(IID_IUnknown) for matching
    D3D11_TEXTURE2D_DESC      desc;         // post-override desc (actual size OGRE sees)
    bool                      isDepth;      // DSV-bound depth vs RTV+SRV color
    ID3D11Texture2D*          newTex;       // replacement (null if no resize active)
    ID3D11DepthStencilView*   newDSV;
    ID3D11RenderTargetView*   newRTV;
    ID3D11ShaderResourceView* newSRV;
};
static ShadowAtlasEntry gShadowEntries[kMaxShadowIdentities] = {};
static std::atomic<size_t> gShadowEntryCount{0};
static UINT  gShadowBaseSize = 0;              // size OGRE thinks the atlas is
static bool  gShadowSwapActive = false;        // fast-path flag
static float gShadowViewportScale = 1.0f;      // newSize / baseSize
static std::atomic<bool> gShadowResizePending{false};
static UINT  gShadowResizeTarget = 0;

void SetShadowAtlasResolution(UINT size)
{
    gShadowAtlasOverride = size;
    if (gShadowEntryCount.load(std::memory_order_acquire) > 0 && size != 0 &&
        size != gShadowResizeTarget)
    {
        gShadowResizeTarget = size;
        gShadowResizePending.store(true, std::memory_order_release);
    }
    RefreshContextHooks();
}

// Shadow-pass profiling state. gShadowAtlasIdentity holds the IUnknown
// identity pointer (QueryInterface(IID_IUnknown)) of the atlas Texture2D —
// COM identity rule guarantees this matches across any interface query, even
// if a wrapper layer (RE_Kenshi, debug layer) sits between us and the real
// texture. gInShadowPass flips in HookedOMSetRenderTargets when the bound
// DSV's resource resolves to the same IUnknown identity. Weak pointer — we
// don't AddRef and tolerate the texture outliving us.
// Up to 8 atlas identities tracked simultaneously. Kenshi can create multiple
// DSV-bound atlas textures (RTW + CSM modes, workspace recreate on resolution
// change, etc.) and a single-slot identity caused us to lose track every time
// a new one appeared. Slot 0 is the most recently captured (just informational
// — the compare scans all slots). Atomic so OMSet hooks read without a lock.
static IUnknown* gShadowAtlasIdentities[kMaxShadowIdentities] = {};
static std::atomic<size_t> gShadowAtlasIdentityCount{0};
static bool      gInShadowPass        = false;

// Deferred-lighting PS detection. The shader patch fires for every permutation
// of the deferred main_fs (shadow on/off, RTW/CSM, cascade counts, ...), so we
// have to track all of them — capturing only the first one means most binds
// don't match. Set in HookedCreatePixelShader when bytecode contains
// "DustShadowParams" (our b7 cbuffer, present only in patched variants).
// Flipped in HookedPSSetShader when any of these PSes binds.
static constexpr size_t kMaxDeferredPSes = 16;
static ID3D11PixelShader* gDeferredShadowPSes[kMaxDeferredPSes] = {};
static std::atomic<size_t> gDeferredShadowPSCount{0};
static bool gInDeferredShadowPass = false;
// Diagnostic counters — surface in HookedPresent so we can read them
// without a debugger. gShadowMatchCount = times the DSV bind compare
// succeeded; gShadowDrawCount = times a draw fired while gInShadowPass
// was true. If matches >> draws, our flag is stale by draw time and we
// need a different detection strategy (e.g., check bound DSV at draw).
static std::atomic<uint64_t> gShadowMatchCount{0};
static std::atomic<uint64_t> gShadowDrawCount{0};

#ifdef TRACY_ENABLE
// Tracy GPU context. Created after gContext is captured; one per immediate
// context. Tracy handles per-frame Collect + query pool; we just emit zones.
static TracyD3D11Ctx gTracyGpuCtx = nullptr;
#endif

bool GetCameraWorldPos(float outXYZ[3])
{
    if (!gCameraDataEverValid || !outXYZ) return false;
    outXYZ[0] = gCameraData.camPosition[0];
    outXYZ[1] = gCameraData.camPosition[1];
    outXYZ[2] = gCameraData.camPosition[2];
    return true;
}

void SignalGameAlive(const char* via)
{
    if (!gGameAlive)
    {
        gGameAlive = true;
        Log("Game alive (via %s) — splash/loader phase complete", via ? via : "?");
    }
}

// Find the ShadowAtlasEntry whose original texture matches this resource's
// IUnknown identity. Returns index or -1. Caller must Release res afterwards
// (this function does NOT consume the ref).
static int FindShadowEntry(ID3D11Resource* res)
{
    ZoneScopedN("FindShadowEntry");
    if (!res) return -1;
    IUnknown* unk = nullptr;
    res->QueryInterface(IID_IUnknown, (void**)&unk);
    if (!unk) return -1;
    size_t count = gShadowEntryCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; i++)
    {
        if (gShadowEntries[i].identity == unk) { unk->Release(); return (int)i; }
    }
    unk->Release();
    return -1;
}

static void ReleaseShadowReplacement(ShadowAtlasEntry& e)
{
    if (e.newDSV)  { e.newDSV->Release();  e.newDSV = nullptr; }
    if (e.newRTV)  { e.newRTV->Release();  e.newRTV = nullptr; }
    if (e.newSRV)  { e.newSRV->Release();  e.newSRV = nullptr; }
    if (e.newTex)  { e.newTex->Release();  e.newTex = nullptr; }
}

static void ApplyPendingShadowResize()
{
    if (!gShadowResizePending.load(std::memory_order_acquire))
        return;
    gShadowResizePending.store(false, std::memory_order_relaxed);

    UINT newSize = gShadowResizeTarget;
    size_t count = gShadowEntryCount.load(std::memory_order_acquire);
    if (count == 0 || !gDevice) return;

    // If matching base size, disable swapping (original textures are correct)
    if (newSize == gShadowBaseSize)
    {
        for (size_t i = 0; i < count; i++)
            ReleaseShadowReplacement(gShadowEntries[i]);
        gShadowSwapActive = false;
        gShadowViewportScale = 1.0f;
        Log("Shadow atlas resize: back to base %u — swap disabled", newSize);
        RefreshContextHooks();
        return;
    }

    Log("Shadow atlas runtime resize: %u -> %u (%zu entries)",
        gShadowBaseSize, newSize, count);

    for (size_t i = 0; i < count; i++)
    {
        ShadowAtlasEntry& e = gShadowEntries[i];
        ReleaseShadowReplacement(e);

        D3D11_TEXTURE2D_DESC d = e.desc;
        d.Width  = newSize;
        d.Height = newSize;

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

            // Register new depth identity for shadow-pass detection
            IUnknown* nid = nullptr;
            e.newTex->QueryInterface(IID_IUnknown, (void**)&nid);
            if (nid)
            {
                size_t sidx = gShadowAtlasIdentityCount.load(std::memory_order_relaxed);
                if (sidx < kMaxShadowIdentities)
                {
                    gShadowAtlasIdentities[sidx] = nid;
                    gShadowAtlasIdentityCount.store(sidx + 1, std::memory_order_release);
                }
                nid->Release();
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

        Log("  entry %zu (%s): tex=%p DSV=%p RTV=%p SRV=%p",
            i, e.isDepth ? "depth" : "color",
            e.newTex, e.newDSV, e.newRTV, e.newSRV);
    }

    gShadowViewportScale = (float)newSize / (float)gShadowBaseSize;
    gShadowSwapActive = true;
    Log("Shadow atlas swap active, viewport scale = %.3f", gShadowViewportScale);
    RefreshContextHooks();
}

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
    }
}

void ResetFrameState()
{
    ApplyPendingShadowResize();
    ClearShadowReplacements();
    SignalGameAlive("GameWorld::mainLoop");
    GeometryCapture::ResetFrame();
    gPipelineDetector.ResetFrame();
    gResourceRegistry.ResetFrame();
    TerrainTess::OnFrameEnd();
    gDispatchedThisFrame = false;
    gCameraDataExtracted = false;
    gDeviceRemovedThisFrame = false;
    gFrameIndex++;
}

// Extract camera basis vectors from the game's deferred lighting PS constant buffer.
// The inverse view matrix sits at register c8 (offset 128 bytes / 32 floats).
// Camera axes are the COLUMNS of the inverse view matrix (= rows of the view matrix).
// Double-buffered staging: CopyResource this frame, Map+read LAST frame's copy
// with DO_NOT_WAIT (zero GPU stall). In practice the previous frame's copy is
// always complete by the time we read it — one full frame of GPU work has passed.
static ID3D11Buffer* gCameraStagingCB[2] = { nullptr, nullptr };
static int           gCameraStagingIdx   = 0;
static bool          gCameraStagingReady[2] = { false, false };

static void ExtractCameraData(ID3D11DeviceContext* ctx)
{
    ZoneScopedN("ExtractCameraData");
    // Per-period instrumentation. Counts each failure mode + success so we
    // can see (from the log) why gCameraData isn't updating reliably.
    static uint32_t sCallCount         = 0;
    static uint32_t sAlreadyExtracted  = 0;
    static uint32_t sNoBindCount       = 0;
    static uint32_t sSizeFailCount     = 0;
    static uint32_t sStagingCreateFail = 0;
    static uint32_t sMapFailCount      = 0;
    static uint32_t sValidateFailCount = 0;
    static uint32_t sSuccessCount      = 0;

    // CB-content stability tracker. Hash the 64 matrix bytes each successful
    // extraction; compare to the previous frame's hash. If they match on
    // frames where the game is rendering motion, Kenshi's CB at c8 didn't
    // refresh this frame. Definitively proves whether the strobing is
    // game-side or downstream.
    static uint64_t sLastMatrixHash    = 0;
    static uint32_t sCBSameCount       = 0;   // frames where matrix == prev
    static uint32_t sCBDiffCount       = 0;   // frames where matrix changed
    sCallCount++;

    auto logIfTime = []() {
        if (gFrameIndex > 0 && (gFrameIndex % 120) == 0)
        {
            Log("[CamExtract@frame %llu over last 120 frames]: calls=%u alreadyExt=%u "
                "noBind=%u sizeFail=%u stagingFail=%u mapFail=%u validateFail=%u success=%u | "
                "CB-same=%u CB-diff=%u",
                (unsigned long long)gFrameIndex, sCallCount, sAlreadyExtracted,
                sNoBindCount, sSizeFailCount, sStagingCreateFail,
                sMapFailCount, sValidateFailCount, sSuccessCount,
                sCBSameCount, sCBDiffCount);
            sCallCount = sAlreadyExtracted = sNoBindCount = sSizeFailCount = 0;
            sStagingCreateFail = sMapFailCount = 0;
            sValidateFailCount = sSuccessCount = 0;
            sCBSameCount = sCBDiffCount = 0;
        }
    };

    if (gCameraDataExtracted) { sAlreadyExtracted++; logIfTime(); return; }

    ID3D11Buffer* psCB = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &psCB);
    if (!psCB) { sNoBindCount++; logIfTime(); return; }

    D3D11_BUFFER_DESC cbDesc;
    psCB->GetDesc(&cbDesc);
    if (cbDesc.ByteWidth < 192) { sSizeFailCount++; psCB->Release(); logIfTime(); return; }

    int writeIdx = gCameraStagingIdx;
    int readIdx  = 1 - writeIdx;

    // Ensure both staging buffers match the CB size
    for (int i = 0; i < 2; i++)
    {
        if (gCameraStagingCB[i])
        {
            D3D11_BUFFER_DESC sd;
            gCameraStagingCB[i]->GetDesc(&sd);
            if (sd.ByteWidth != cbDesc.ByteWidth)
            {
                gCameraStagingCB[i]->Release();
                gCameraStagingCB[i] = nullptr;
                gCameraStagingReady[i] = false;
            }
        }
        if (!gCameraStagingCB[i])
        {
            D3D11_BUFFER_DESC sd = cbDesc;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.BindFlags = 0;
            sd.MiscFlags = 0;
            gDevice->CreateBuffer(&sd, nullptr, &gCameraStagingCB[i]);
            gCameraStagingReady[i] = false;
        }
    }
    if (!gCameraStagingCB[writeIdx])
    {
        sStagingCreateFail++; psCB->Release(); logIfTime(); return;
    }

    // Issue GPU copy for THIS frame (non-blocking)
    ctx->CopyResource(gCameraStagingCB[writeIdx], psCB);
    psCB->Release();
    gCameraStagingReady[writeIdx] = true;
    gCameraStagingIdx = 1 - writeIdx;

    // Read LAST frame's copy (should be complete — one full frame of GPU work has passed)
    if (gCameraStagingReady[readIdx] && gCameraStagingCB[readIdx])
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(gCameraStagingCB[readIdx], 0, D3D11_MAP_READ,
                               D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
        {
            // One-shot CB audit on first successful map. Scans the whole CB
            // for matrices identifiable by mathematical properties — gives
            // us programmatic ground truth for projection FOV, near/far,
            // and confirms the inverse-view location.
            static bool sAuditDone = false;
            if (!sAuditDone)
            {
                D3D11_BUFFER_DESC stagingDesc;
                gCameraStagingCB[readIdx]->GetDesc(&stagingDesc);
                int numFloats = (int)(stagingDesc.ByteWidth / 4);
                const float* cb = (const float*)mapped.pData;
                Log("[CBAudit] === Begin one-shot CB scan: %d floats (%d bytes) ===",
                    numFloats, (int)stagingDesc.ByteWidth);

                for (int off = 0; off + 16 <= numFloats; off += 4)
                {
                    const float* M = cb + off;

                    // Count near-zeros for projection detection.
                    int zeros = 0;
                    bool allFinite = true;
                    for (int i = 0; i < 16; i++)
                    {
                        if (!isfinite(M[i])) { allFinite = false; break; }
                        if (fabsf(M[i]) < 1e-5f) zeros++;
                    }
                    if (!allFinite) continue;

                    // Test ROW-MAJOR columns for orthonormality (basis vectors
                    // stored at strided positions m[0,4,8], m[1,5,9], m[2,6,10]).
                    float c0[3] = {M[0], M[4], M[8]};
                    float c1[3] = {M[1], M[5], M[9]};
                    float c2[3] = {M[2], M[6], M[10]};
                    float lc0 = sqrtf(c0[0]*c0[0]+c0[1]*c0[1]+c0[2]*c0[2]);
                    float lc1 = sqrtf(c1[0]*c1[0]+c1[1]*c1[1]+c1[2]*c1[2]);
                    float lc2 = sqrtf(c2[0]*c2[0]+c2[1]*c2[1]+c2[2]*c2[2]);
                    bool unitCols = (fabsf(lc0-1.0f) < 0.05f && fabsf(lc1-1.0f) < 0.05f && fabsf(lc2-1.0f) < 0.05f);
                    float dotc01 = c0[0]*c1[0]+c0[1]*c1[1]+c0[2]*c1[2];
                    float dotc02 = c0[0]*c2[0]+c0[1]*c2[1]+c0[2]*c2[2];
                    bool orthCols = (fabsf(dotc01) < 0.05f && fabsf(dotc02) < 0.05f);

                    // Test ROW-MAJOR rows for orthonormality (basis vectors
                    // stored contiguously: m[0..2], m[4..6], m[8..10]).
                    float r0[3] = {M[0], M[1], M[2]};
                    float r1[3] = {M[4], M[5], M[6]};
                    float r2[3] = {M[8], M[9], M[10]};
                    float lr0 = sqrtf(r0[0]*r0[0]+r0[1]*r0[1]+r0[2]*r0[2]);
                    float lr1 = sqrtf(r1[0]*r1[0]+r1[1]*r1[1]+r1[2]*r1[2]);
                    float lr2 = sqrtf(r2[0]*r2[0]+r2[1]*r2[1]+r2[2]*r2[2]);
                    bool unitRows = (fabsf(lr0-1.0f) < 0.05f && fabsf(lr1-1.0f) < 0.05f && fabsf(lr2-1.0f) < 0.05f);
                    float dotr01 = r0[0]*r1[0]+r0[1]*r1[1]+r0[2]*r1[2];
                    float dotr02 = r0[0]*r2[0]+r0[1]*r2[1]+r0[2]*r2[2];
                    bool orthRows = (fabsf(dotr01) < 0.05f && fabsf(dotr02) < 0.05f);

                    if (unitCols && orthCols)
                    {
                        Log("[CBAudit] c%d (byte %d): orthonormal COLS (rowmajor), "
                            "row3=(%.3f,%.3f,%.3f,%.3f) col3=(%.3f,%.3f,%.3f,%.3f)",
                            off/4, off*4,
                            M[12], M[13], M[14], M[15],
                            M[3], M[7], M[11], M[15]);
                    }
                    if (unitRows && orthRows)
                    {
                        Log("[CBAudit] c%d (byte %d): orthonormal ROWS (colmajor), "
                            "row3=(%.3f,%.3f,%.3f,%.3f) col3=(%.3f,%.3f,%.3f,%.3f)",
                            off/4, off*4,
                            M[12], M[13], M[14], M[15],
                            M[3], M[7], M[11], M[15]);
                    }

                    // Projection-shaped: many zeros + nonzero [1][1] (which is
                    // 1/tan(fov_y/2) for a standard perspective matrix).
                    if (zeros >= 10 && fabsf(M[5]) > 0.1f)
                    {
                        float tanY = 1.0f / M[5];
                        float fovYdeg = 2.0f * atanf(tanY) * 57.2957795f;
                        // [2][2] and [3][2] encode near/far in row-major LH:
                        //   [2][2] = far/(far-near),  [3][2] = -near*far/(far-near)
                        // → near = -[3][2] / ([2][2]-1),  far = [3][2] / (1-[2][2]) ... varies by convention
                        Log("[CBAudit] c%d (byte %d): %d zeros, diag=(%.4f,%.4f,%.4f,%.4f) → "
                            "if perspective: tan(fovY/2)=%.4f, fovY=%.2fdeg",
                            off/4, off*4, zeros,
                            M[0], M[5], M[10], M[15],
                            tanY, fovYdeg);
                        Log("[CBAudit]    proj rows: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] "
                            "[%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]",
                            M[0],M[1],M[2],M[3], M[4],M[5],M[6],M[7],
                            M[8],M[9],M[10],M[11], M[12],M[13],M[14],M[15]);
                    }
                }
                Log("[CBAudit] === End scan ===");
                sAuditDone = true;
            }

            float m[16];
            memcpy(m, (float*)mapped.pData + 32, 64); // c8 offset (inverse view)

            float proj[16] = {};
            int numFloats = (int)(cbDesc.ByteWidth / 4);
            if (numFloats >= 136 + 16)
                memcpy(proj, (float*)mapped.pData + 136, 64); // c34 offset (projection)

            ctx->Unmap(gCameraStagingCB[readIdx], 0);

            bool valid = true;
            for (int i = 0; i < 16; i++)
                if (!isfinite(m[i])) { valid = false; break; }

            if (valid)
            {
                // FNV-1a hash of the 64 raw matrix bytes — fast, byte-exact
                // comparison so we can tell if Kenshi wrote the same CB content
                // as last frame.
                uint64_t h = 0xcbf29ce484222325ull;
                const uint8_t* bytes = (const uint8_t*)m;
                for (int i = 0; i < 64; ++i) { h ^= bytes[i]; h *= 0x100000001b3ull; }
                if (h == sLastMatrixHash) sCBSameCount++;
                else                      sCBDiffCount++;
                sLastMatrixHash = h;

                memcpy(gCameraData.inverseView, m, 64);
                // Kenshi/OGRE stores matrices column-major in memory. The inverse
                // view matrix has columns = basis vectors in world. In column-major
                // memory, column N occupies m[N*4 .. N*4+2] (consecutive). The
                // previous strided extraction (m[0],m[4],m[8]) was reading rows
                // of the math matrix — orthonormal but NOT the basis vectors,
                // which scrambled all temporal reprojection.
                gCameraData.camRight[0]    = m[0];  gCameraData.camRight[1]    = m[1];  gCameraData.camRight[2]    = m[2];
                gCameraData.camUp[0]       = m[4];  gCameraData.camUp[1]       = m[5];  gCameraData.camUp[2]       = m[6];
                gCameraData.camForward[0]  = m[8];  gCameraData.camForward[1]  = m[9];  gCameraData.camForward[2]  = m[10];
                gCameraData.camPosition[0] = m[12]; gCameraData.camPosition[1] = m[13]; gCameraData.camPosition[2] = m[14];
                gCameraData.valid = 1;
                memcpy(gCameraData.projMatrix, proj, 64);

                gCameraDataExtracted = true;
                gCameraDataEverValid = true;
                sSuccessCount++;
            }
            else
            {
                sValidateFailCount++;
            }
        }
        else
        {
            sMapFailCount++;
        }
    }

    logIfTime();
}

// ==================== Original function pointers ====================
// (PFN_CreateTexture2D forward-declared above shadow resize state)

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

typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(
    ID3D11DeviceContext* pThis, UINT NumViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView);

typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargetsAndUAV)(
    ID3D11DeviceContext* pThis,
    UINT NumRTVs,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
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

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(
    IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags);

typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(
    IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags);

static PFN_CreatePixelShader        oCreatePixelShader = nullptr;
static PFN_Draw                     oDraw = nullptr;
static PFN_DrawIndexed              oDrawIndexed = nullptr;
static PFN_DrawIndexedInstanced     oDrawIndexedInstanced = nullptr;
static PFN_OMSetRenderTargets       oOMSetRenderTargets = nullptr;
static PFN_OMSetRenderTargetsAndUAV oOMSetRenderTargetsAndUAV = nullptr;
static PFN_PSSetShaderResources     oPSSetShaderResources = nullptr;
static PFN_RSSetViewports           oRSSetViewports = nullptr;
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
    *original = vtable[vtableIndex];

    DWORD oldProtect;
    if (!VirtualProtect(&vtable[vtableIndex], sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
    vtable[vtableIndex] = detour;
    VirtualProtect(&vtable[vtableIndex], sizeof(void*), oldProtect, &oldProtect);
    return true;
}

// ==================== Function pointer types for VTable-managed methods =====
// Moved here from their original locations so the VTable infrastructure and
// TryCaptureDevice can reference them. The hook implementations still live
// further down in the file.

typedef void (STDMETHODCALLTYPE* PFN_PSSetShader)(
    ID3D11DeviceContext* pThis, ID3D11PixelShader* pPixelShader,
    ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
typedef void (STDMETHODCALLTYPE* PFN_VSSetShader)(
    ID3D11DeviceContext* pThis, ID3D11VertexShader* pVertexShader,
    ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Map)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT,
    D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void (STDMETHODCALLTYPE* PFN_Unmap)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void (STDMETHODCALLTYPE* PFN_CopyResource)(
    ID3D11DeviceContext* pThis, ID3D11Resource* pDstResource,
    ID3D11Resource* pSrcResource);
typedef void (STDMETHODCALLTYPE* PFN_UpdateSubresource)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
    const void*, UINT, UINT);

static PFN_PSSetShader       oPSSetShader       = nullptr;
static PFN_VSSetShader       oVSSetShader       = nullptr;
static PFN_Map               oMap               = nullptr;
static PFN_Unmap             oUnmap             = nullptr;
static PFN_CopyResource      oCopyResource      = nullptr;
static PFN_UpdateSubresource oUpdateSubresource = nullptr;

// ==================== Context VTable hook infrastructure ====================
//
// Hot-path ID3D11DeviceContext methods (Map, Unmap, DrawIndexed, etc.) are
// hooked via vtable pointer replacement instead of Detours.  Benefits:
//   1. Zero trampoline overhead — vtable dispatch is the same mechanism D3D11
//      already uses, so hooked calls cost the same as unhooked calls.
//   2. Atomically removable — restoring the original pointer = zero overhead
//      when no Dust feature needs interception.
//
// The o* function pointers (oMap, oUnmap, etc.) always hold the REAL original
// function addresses (not trampolines).  They are set from the temp device in
// Install() and overwritten with the real device's vtable entries in
// TryCaptureDevice().

static void**  gCtxVtable = nullptr;           // real context's vtable base
static void*   gCtxOriginals[64] = {};          // saved original entries
static bool    gVTableHooksActive = false;      // master install state

struct CtxHookEntry {
    int   vtableIndex;
    void* hookFunc;
};

// Forward-declare hook functions (defined later in the file).
static HRESULT STDMETHODCALLTYPE HookedMap(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
static void    STDMETHODCALLTYPE HookedUnmap(ID3D11DeviceContext*, ID3D11Resource*, UINT);
static void    STDMETHODCALLTYPE HookedDrawIndexed(ID3D11DeviceContext*, UINT, UINT, INT);
static void    STDMETHODCALLTYPE HookedDrawIndexedInstanced(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
static void    STDMETHODCALLTYPE HookedPSSetShader(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
static void    STDMETHODCALLTYPE HookedVSSetShader(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
static void    STDMETHODCALLTYPE HookedOMSetRenderTargets(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
static void    STDMETHODCALLTYPE HookedOMSetRenderTargetsAndUAV(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
static void    STDMETHODCALLTYPE HookedPSSetShaderResources(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
static void    STDMETHODCALLTYPE HookedRSSetViewports(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
static void    STDMETHODCALLTYPE HookedCopyResource(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
static void    STDMETHODCALLTYPE HookedUpdateSubresource(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);

static const CtxHookEntry kCtxHooks[] = {
    { VTIDX_CTX_Map,                      (void*)HookedMap },
    { VTIDX_CTX_Unmap,                    (void*)HookedUnmap },
    { VTIDX_CTX_DrawIndexed,              (void*)HookedDrawIndexed },
    { VTIDX_CTX_DrawIndexedInstanced,     (void*)HookedDrawIndexedInstanced },
    { VTIDX_CTX_PSSetShader,              (void*)HookedPSSetShader },
    { VTIDX_CTX_VSSetShader,              (void*)HookedVSSetShader },
    { VTIDX_CTX_OMSetRenderTargets,       (void*)HookedOMSetRenderTargets },
    { VTIDX_CTX_OMSetRenderTargetsAndUAV, (void*)HookedOMSetRenderTargetsAndUAV },
    { VTIDX_CTX_PSSetShaderResources,     (void*)HookedPSSetShaderResources },
    { VTIDX_CTX_RSSetViewports,           (void*)HookedRSSetViewports },
    { VTIDX_CTX_CopyResource,             (void*)HookedCopyResource },
    { VTIDX_CTX_UpdateSubresource,        (void*)HookedUpdateSubresource },
};

static void WriteCtxVTableEntry(int index, void* func)
{
    DWORD oldProtect;
    if (VirtualProtect(&gCtxVtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        gCtxVtable[index] = func;
        VirtualProtect(&gCtxVtable[index], sizeof(void*), oldProtect, &oldProtect);
    }
}

static void InstallContextHooks()
{
    if (gVTableHooksActive || !gCtxVtable) return;
    for (const auto& h : kCtxHooks)
        WriteCtxVTableEntry(h.vtableIndex, h.hookFunc);
    gVTableHooksActive = true;
    Log("Context VTable hooks INSTALLED (%zu methods)", std::size(kCtxHooks));
}

static void RemoveContextHooks()
{
    if (!gVTableHooksActive || !gCtxVtable) return;
    for (const auto& h : kCtxHooks)
    {
        if (gCtxVtable[h.vtableIndex] == h.hookFunc)
            WriteCtxVTableEntry(h.vtableIndex, gCtxOriginals[h.vtableIndex]);
    }
    gVTableHooksActive = false;
    Log("Context VTable hooks REMOVED (zero overhead)");
}

static bool AnyFeatureNeedsHooks()
{
    if (TerrainTess::GetEnabled()) return true;
    if (POMState::GetEnabled()) return true;
    if (gShadowSwapActive) return true;
    if (gShadowAtlasOverride != 0) return true;
    if (Survey::IsActive()) return true;
    if (GeometryCapture::detail::sCaptureFlags != 0) return true;
    return false;
}

void RefreshContextHooks()
{
    if (!gCtxVtable) return;
    bool needed = AnyFeatureNeedsHooks();
    if (needed && !gVTableHooksActive)
        InstallContextHooks();
    else if (!needed && gVTableHooksActive)
        RemoveContextHooks();
}

// The swap chain pointer we vtable-hooked — needed for recovery verification
static IDXGISwapChain* gHookedSwapChain = nullptr;

static void TryInstallSwapChainHooks(ID3D11DeviceContext* drawCtx = nullptr)
{
    if (sSwapChainHooked)
        return;

    // Defer Present/ResizeBuffers vtable patching until the splash/loader
    // phase is over. Some users have an overlay or driver shim with an inline
    // hook on Present that crashes when called against a transient loader
    // swap chain. By holding off the patch until gGameAlive flips
    // (TitleScreen::show or first GameWorld::mainLoop), the splash Present
    // runs on the un-patched DXGI path with zero Dust code in its chain.
    if (!gGameAlive)
        return;

    // Preferred GUI tick site: OGRE's RenderWindow::swapBuffers. Lives in the
    // game's own render path, never fires for the loader's transient swap
    // chain. If this succeeds, the DXGI Present hook below still installs
    // (we need ResizeBuffers anyway) but TickGuiOnPresent skips its render
    // step to avoid double-rendering.
    OgreSwapHook::TryInstall();

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
    GeometryCapture::SetDevice(device);
    POMState::SetDevice(device);
    TerrainTess::Init(device);

    Log("Captured real D3D11 device=%p, context=%p", gDevice, gContext);

    // Initialize VTable hook infrastructure: save originals from the real
    // context's vtable and overwrite the o* pointers (which currently hold
    // temp-device addresses from Install).
    gCtxVtable = *reinterpret_cast<void***>(gContext);
    for (const auto& h : kCtxHooks)
        gCtxOriginals[h.vtableIndex] = gCtxVtable[h.vtableIndex];
    oMap                      = (PFN_Map)gCtxOriginals[VTIDX_CTX_Map];
    oUnmap                    = (PFN_Unmap)gCtxOriginals[VTIDX_CTX_Unmap];
    oDrawIndexed              = (PFN_DrawIndexed)gCtxOriginals[VTIDX_CTX_DrawIndexed];
    oDrawIndexedInstanced     = (PFN_DrawIndexedInstanced)gCtxOriginals[VTIDX_CTX_DrawIndexedInstanced];
    oPSSetShader              = (PFN_PSSetShader)gCtxOriginals[VTIDX_CTX_PSSetShader];
    oVSSetShader              = (PFN_VSSetShader)gCtxOriginals[VTIDX_CTX_VSSetShader];
    oOMSetRenderTargets       = (PFN_OMSetRenderTargets)gCtxOriginals[VTIDX_CTX_OMSetRenderTargets];
    oOMSetRenderTargetsAndUAV = (PFN_OMSetRenderTargetsAndUAV)gCtxOriginals[VTIDX_CTX_OMSetRenderTargetsAndUAV];
    oPSSetShaderResources     = (PFN_PSSetShaderResources)gCtxOriginals[VTIDX_CTX_PSSetShaderResources];
    oRSSetViewports           = (PFN_RSSetViewports)gCtxOriginals[VTIDX_CTX_RSSetViewports];
    oCopyResource             = (PFN_CopyResource)gCtxOriginals[VTIDX_CTX_CopyResource];
    oUpdateSubresource        = (PFN_UpdateSubresource)gCtxOriginals[VTIDX_CTX_UpdateSubresource];
    Log("VTable originals saved from real context (12 methods)");

    RefreshContextHooks();

#ifdef TRACY_ENABLE
    // Tracy GPU profiling — must run after gContext is valid. The context
    // takes its own ref on device + immediate context; safe to leak at exit
    // since Tracy tears down with the process.
    gTracyGpuCtx = TracyD3D11Context(gDevice, gContext);
    TracyD3D11ContextName(gTracyGpuCtx, "Kenshi", 6);
    Log("Tracy GPU context created");
#endif

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
        GeometryCapture::SetResolution(gWidth, gHeight);
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

// Detects the shadow atlas pair: R32_FLOAT color (RTV+SRV) and
// D32_FLOAT/R32_TYPELESS depth (DSV, optionally SRV). The game's "shadow
// resolution" setting actually picks 1024/2048/4096 — we accept any square
// power-of-two in that range. The 512^2 RTW intermediate is excluded by
// the 1024 floor; no other pipeline output is square R32_FLOAT/D32 in this
// size range (see docs/pipeline/00_overview.md).
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
    if ((d->Format == DXGI_FORMAT_D32_FLOAT ||
         d->Format == DXGI_FORMAT_R32_TYPELESS) && hasDSV) return true;
    return false;
}

static HRESULT STDMETHODCALLTYPE HookedCreateTexture2D(
    ID3D11Device* pThis, const D3D11_TEXTURE2D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D)
{
    ZoneScopedN("HookedCreateTexture2D");
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

    UINT override = gShadowAtlasOverride;
    const D3D11_TEXTURE2D_DESC* finalDesc = pDesc;
    D3D11_TEXTURE2D_DESC modDesc;
    if (override != 0 && IsShadowAtlasDesc(pDesc) && pDesc->Width != override)
    {
        modDesc = *pDesc;
        modDesc.Width  = override;
        modDesc.Height = override;
        Log("Shadow atlas override: %ux%u %s -> %ux%u",
            pDesc->Width, pDesc->Height,
            FormatName(pDesc->Format) ? FormatName(pDesc->Format) : "?",
            override, override);
        finalDesc = &modDesc;
    }

    HRESULT hr = oCreateTexture2D(pThis, finalDesc, pInitialData, ppTexture2D);

    // Track every shadow-atlas-matching Texture2D (color AND depth) for
    // runtime resize and shadow-pass detection.
    if (SUCCEEDED(hr) && pDesc && ppTexture2D && *ppTexture2D &&
        IsShadowAtlasDesc(pDesc))
    {
        bool isDepth = (pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
        IUnknown* unk = nullptr;
        (*ppTexture2D)->QueryInterface(IID_IUnknown, (void**)&unk);

        // DSV identity tracking (existing — for shadow-pass detection)
        if (unk && isDepth)
        {
            size_t idx = gShadowAtlasIdentityCount.load(std::memory_order_relaxed);
            if (idx < kMaxShadowIdentities)
            {
                gShadowAtlasIdentities[idx] = unk;
                gShadowAtlasIdentityCount.store(idx + 1, std::memory_order_release);
            }
        }

        // Entry tracking (new — for runtime resize swapping)
        if (unk)
        {
            size_t eidx = gShadowEntryCount.load(std::memory_order_relaxed);
            if (eidx < kMaxShadowIdentities)
            {
                ShadowAtlasEntry& e = gShadowEntries[eidx];
                e = {};
                e.tex = *ppTexture2D;
                e.tex->AddRef();
                e.identity = unk;  // weak ref (matches gShadowAtlasIdentities convention)
                e.desc = *finalDesc;
                e.isDepth = isDepth;
                if (gShadowBaseSize == 0)
                    gShadowBaseSize = finalDesc->Width;
                gShadowEntryCount.store(eidx + 1, std::memory_order_release);
            }
            unk->Release();
        }

        Log("Shadow atlas %s texture captured: tex=%p identity=%p (%ux%u) entry=%zu",
            isDepth ? "DSV" : "color", *ppTexture2D, unk,
            finalDesc->Width, finalDesc->Height,
            gShadowEntryCount.load(std::memory_order_relaxed) - 1);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreatePixelShader(
    ID3D11Device* pThis, const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader)
{
    ZoneScopedN("HookedCreatePixelShader");
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
        ShaderDatabase::OnPixelShaderCreated(*ppPixelShader);
        TerrainTess::OnPixelShaderCreated(pShaderBytecode, BytecodeLength, *ppPixelShader);

        // Detect the patched deferred main_fs by the b7 cbuffer name our
        // shader patch injects. DXBC stores cbuffer names in the RDEF chunk
        // as plain ASCII, so a linear byte scan is enough. Append every
        // variant — there are multiple permutations (RTW/CSM, shadow on/off,
        // ...) and capturing just one misses the rest.
        if (pShaderBytecode && BytecodeLength >= 16)
        {
            static const char kNeedle[] = "DustShadowParams";
            const size_t needleLen = sizeof(kNeedle) - 1;
            const char* hay = (const char*)pShaderBytecode;
            for (size_t i = 0; i + needleLen <= BytecodeLength; i++)
            {
                if (memcmp(hay + i, kNeedle, needleLen) == 0)
                {
                    size_t idx = gDeferredShadowPSCount.load(std::memory_order_relaxed);
                    if (idx < kMaxDeferredPSes)
                    {
                        gDeferredShadowPSes[idx] = *ppPixelShader;
                        gDeferredShadowPSCount.store(idx + 1, std::memory_order_release);
                        Log("Captured patched deferred PS: %p (slot=%zu)",
                            *ppPixelShader, idx);
                    }
                    break;
                }
            }
        }
    }
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
    ZoneScopedN("HookedCreateVertexShader");
    if (gShutdownSignaled)
        return oCreateVertexShader(pThis, pShaderBytecode, BytecodeLength,
                                    pClassLinkage, ppVertexShader);

    HRESULT hr = oCreateVertexShader(pThis, pShaderBytecode, BytecodeLength,
                                      pClassLinkage, ppVertexShader);
    if (SUCCEEDED(hr) && ppVertexShader && *ppVertexShader)
    {
        SurveyRecorder::OnVertexShaderCreated(pShaderBytecode, BytecodeLength, *ppVertexShader);
        ShaderMetadata::OnVertexShaderCreated(pShaderBytecode, BytecodeLength, *ppVertexShader);
        ShaderDatabase::OnVertexShaderCreated(*ppVertexShader);
        TerrainTess::OnVertexShaderCreated(pShaderBytecode, BytecodeLength, *ppVertexShader);
    }
    return hr;
}

static void STDMETHODCALLTYPE HookedDraw(
    ID3D11DeviceContext* pThis, UINT VertexCount, UINT StartVertexLocation)
{
    if (gShutdownSignaled) { oDraw(pThis, VertexCount, StartVertexLocation); return; }

    ZoneScoped;
    if (gInDeferredShadowPass) { ZoneName("ShadowSample", 12); }
#ifdef TRACY_ENABLE
    TracyD3D11NamedZone(gTracyGpuCtx, _gpuSampleZoneDraw, "ShadowSample", gInDeferredShadowPass);
#endif

    // Try to install swap chain hooks early — DustBoot may already have captured the
    // swap chain, and we don't need device capture for that path. Pass pThis so
    // runtime discovery can also work from any draw call (not just fullscreen ones).
    if (!sSwapChainHooked)
        TryInstallSwapChainHooks(pThis);

    // Survey: record ALL draws (before fullscreen filter)
    if (Survey::IsActive())
        SurveyRecorder::OnDraw(pThis, VertexCount, StartVertexLocation);

    if (VertexCount != 3 && VertexCount != 4)
    {
        oDraw(pThis, VertexCount, StartVertexLocation);
        return;
    }

    // Capture device from the rendering context on first fullscreen draw.
    // This is the actual game device, not a temporary enumeration device.
    if (!gDeviceCaptured)
    {
        ID3D11Device* ctxDevice = nullptr;
        pThis->GetDevice(&ctxDevice);
        if (ctxDevice)
        {
            TryCaptureDevice(ctxDevice);
            ctxDevice->Release();
        }

        if (!gDeviceCaptured)
        {
            oDraw(pThis, VertexCount, StartVertexLocation);
            return;
        }
    }

    // Check device health once per frame, not per draw call.
    // GetDeviceRemovedReason can cause driver synchronization on some hardware.
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

    // Detect render pass from GPU state
    auto result = gPipelineDetector.OnFullscreenDraw(pThis);

    if (result.detected)
    {
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
                            GeometryCapture::SetResolution(gWidth, gHeight);
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
            ExtractCameraData(pThis);

        DustFrameContext fctx = {};
        fctx.device = gDevice;
        fctx.context = pThis;
        fctx.point = dip;
        fctx.width = gWidth;
        fctx.height = gHeight;
        fctx.frameIndex = gFrameIndex;
        fctx.camera = gCameraData;

        // PRE: effects bind resources before the game's draw
        fctx.timing = DUST_TIMING_PRE;
        gEffectLoader.DispatchPre(dip, &fctx);

        // Execute the game's original draw call
        oDraw(pThis, VertexCount, StartVertexLocation);

        // POST: effects that operate after the draw
        fctx.timing = DUST_TIMING_POST;
        gEffectLoader.DispatchPost(dip, &fctx);
    }
    else
    {
        oDraw(pThis, VertexCount, StartVertexLocation);
    }
}

// ==================== PSSetShader / VSSetShader hooks ====================
// Maintain TerrainTess's cached "is the current VS+PS a terrain pair" bool
// so the per-DrawIndexed check becomes a single bool load. Without this,
// every non-terrain draw (UI, characters, props) eats two COM AddRef/Release
// pairs + several map lookups just to confirm "not terrain".

// PFN_PSSetShader/PFN_VSSetShader typedefs + o* declarations moved to VTable infrastructure section

static void STDMETHODCALLTYPE HookedPSSetShader(
    ID3D11DeviceContext* pThis, ID3D11PixelShader* pPixelShader,
    ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances)
{
    oPSSetShader(pThis, pPixelShader, ppClassInstances, NumClassInstances);
    // Flag deferred-lighting draws so the Draw hook can bracket them as
    // ShadowSample (PCSS + cascade-blend cost). Scan the captured set of
    // patched deferred main_fs PSes — multiple permutations exist.
    bool match = false;
    if (pPixelShader)
    {
        size_t count = gDeferredShadowPSCount.load(std::memory_order_acquire);
        for (size_t i = 0; i < count; i++)
        {
            if (gDeferredShadowPSes[i] == pPixelShader) { match = true; break; }
        }
    }
    gInDeferredShadowPass = match;
    // Skip bookkeeping when tess is off — nothing reads gIsTerrainBoundFlag.
    // When tess flips on later, the next shader bind repopulates the cache.
    if (!gShutdownSignaled && TerrainTess::GetEnabled())
        TerrainTess::OnPsBound(pPixelShader);
}

static void STDMETHODCALLTYPE HookedVSSetShader(
    ID3D11DeviceContext* pThis, ID3D11VertexShader* pVertexShader,
    ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances)
{
    oVSSetShader(pThis, pVertexShader, ppClassInstances, NumClassInstances);
    if (!gShutdownSignaled && TerrainTess::GetEnabled())
        TerrainTess::OnVsBound(pVertexShader);
}

static void STDMETHODCALLTYPE HookedDrawIndexed(
    ID3D11DeviceContext* pThis, UINT IndexCount, UINT StartIndexLocation,
    INT BaseVertexLocation)
{
    if (gShutdownSignaled) { oDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation); return; }

    // CPU zone only for shadow passes (~5% of draws). The ~95% non-shadow
    // draws skip the ~40ns zone overhead entirely; inner zones
    // (TryDrawTessellated etc.) still capture tessellation work.
    ZoneNamed(___tracy_scoped_zone, gInShadowPass || gInDeferredShadowPass);
    if (gInShadowPass) {
        ZoneName("ShadowCast", 10);
        gShadowDrawCount.fetch_add(1, std::memory_order_relaxed);
    }
    else if (gInDeferredShadowPass) {
        ZoneName("ShadowSample", 12);
    }
#ifdef TRACY_ENABLE
    TracyD3D11NamedZone(gTracyGpuCtx, _gpuShadowZone, "ShadowCast", gInShadowPass);
    TracyD3D11NamedZone(gTracyGpuCtx, _gpuSampleZone, "ShadowSample", gInDeferredShadowPass);
#endif

    if (Survey::IsActive())
        SurveyRecorder::OnDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);

    // Inline early-out: skip the call entirely when no capture session is
    // active. Saves the function-call overhead on ~2000 draws/frame.
    if (GeometryCapture::HasActiveCapture())
        GeometryCapture::OnDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);

    // All three checks are inline bool loads. For non-terrain/non-blood draws
    // (the vast majority — ~5000/frame in Kenshi) we never enter the function
    // body of TryDrawTessellated at all, saving the call overhead.
    if (TerrainTess::GetEnabled() &&
        (TerrainTess::IsTerrainBound() || TerrainTess::IsBloodBound()) &&
        TerrainTess::TryDrawTessellated(pThis, IndexCount, StartIndexLocation,
                                         BaseVertexLocation, oDrawIndexed))
    {
        return;
    }
    if (GeometryCapture::IsInGBufferPass())
        POMState::BindPerDraw(pThis);
    oDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);
}

static void STDMETHODCALLTYPE HookedDrawIndexedInstanced(
    ID3D11DeviceContext* pThis, UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
    if (gShutdownSignaled) { oDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation); return; }

    ZoneNamed(___tracy_scoped_zone, gInShadowPass || gInDeferredShadowPass);
    if (gInShadowPass) {
        ZoneName("ShadowCast", 10);
        gShadowDrawCount.fetch_add(1, std::memory_order_relaxed);
    }
    else if (gInDeferredShadowPass) {
        ZoneName("ShadowSample", 12);
    }
#ifdef TRACY_ENABLE
    TracyD3D11NamedZone(gTracyGpuCtx, _gpuShadowZone, "ShadowCast", gInShadowPass);
    TracyD3D11NamedZone(gTracyGpuCtx, _gpuSampleZone, "ShadowSample", gInDeferredShadowPass);
#endif

    if (Survey::IsActive())
        SurveyRecorder::OnDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                                                StartIndexLocation, BaseVertexLocation,
                                                StartInstanceLocation);

    if (GeometryCapture::HasActiveCapture())
        GeometryCapture::OnDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                                                StartIndexLocation, BaseVertexLocation,
                                                StartInstanceLocation);

    // Instanced terrain draws aren't typical (terrain isn't instanced), so
    // skip tessellation routing here for now.
    if (GeometryCapture::IsInGBufferPass())
        POMState::BindPerDraw(pThis);
    oDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                          StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

// Check if the bound DSV refers to one of the captured shadow atlas
// textures. Resolves via IUnknown identity (the only pointer COM guarantees
// stable across interface queries). Scans the identity set — typically 1-4
// entries — so the cost is a couple virtual calls plus a tiny linear search.
static bool ResolveIsShadowDsv(ID3D11DepthStencilView* dsv)
{
    ZoneScopedN("ResolveIsShadowDsv");
    if (!dsv) return false;
    size_t count = gShadowAtlasIdentityCount.load(std::memory_order_acquire);
    if (count == 0) return false;
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    if (!res) return false;
    IUnknown* unk = nullptr;
    res->QueryInterface(IID_IUnknown, (void**)&unk);
    res->Release();
    bool match = false;
    for (size_t i = 0; i < count; i++)
    {
        if (gShadowAtlasIdentities[i] == unk) { match = true; break; }
    }
    if (unk) unk->Release();
    return match;
}

// Try to swap a DSV or RTV to its shadow atlas replacement. Returns the
// replacement view, or the original if no swap is needed.
static ID3D11DepthStencilView* MaybeSwapShadowDSV(ID3D11DepthStencilView* dsv)
{
    ZoneScopedN("MaybeSwapShadowDSV");
    if (!dsv) return dsv;
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    int idx = FindShadowEntry(res);
    if (res) res->Release();
    if (idx >= 0 && gShadowEntries[idx].isDepth && gShadowEntries[idx].newDSV)
        return gShadowEntries[idx].newDSV;
    return dsv;
}

static ID3D11RenderTargetView* MaybeSwapShadowRTV(ID3D11RenderTargetView* rtv)
{
    ZoneScopedN("MaybeSwapShadowRTV");
    if (!rtv) return rtv;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    int idx = FindShadowEntry(res);
    if (res) res->Release();
    if (idx >= 0 && !gShadowEntries[idx].isDepth && gShadowEntries[idx].newRTV)
        return gShadowEntries[idx].newRTV;
    return rtv;
}

static void STDMETHODCALLTYPE HookedOMSetRenderTargets(
    ID3D11DeviceContext* pThis, UINT NumViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView)
{
    if (gShutdownSignaled) { oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView); return; }

    bool wasInGBuffer = GeometryCapture::IsInGBufferPass();
    bool isGBuffer    = GeometryCapture::CheckGBufferConfig(NumViews, ppRenderTargetViews, pDepthStencilView);

    bool nowInShadowPass = ResolveIsShadowDsv(pDepthStencilView);
    if (nowInShadowPass) gShadowMatchCount.fetch_add(1, std::memory_order_relaxed);

    if (gShadowSwapActive)
    {
        ID3D11DepthStencilView* swapDSV = MaybeSwapShadowDSV(pDepthStencilView);
        ID3D11RenderTargetView* swapRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        ID3D11RenderTargetView* const* finalRTVs = ppRenderTargetViews;
        bool anyRTVSwapped = false;
        if (ppRenderTargetViews && NumViews > 0)
        {
            for (UINT i = 0; i < NumViews && i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
            {
                swapRTVs[i] = MaybeSwapShadowRTV(ppRenderTargetViews[i]);
                if (swapRTVs[i] != ppRenderTargetViews[i]) anyRTVSwapped = true;
            }
            if (anyRTVSwapped) finalRTVs = swapRTVs;
        }
        if (swapDSV != pDepthStencilView || anyRTVSwapped)
            nowInShadowPass = true;
        oOMSetRenderTargets(pThis, NumViews, finalRTVs, swapDSV);

        // OGRE sets RSSetViewports BEFORE OMSetRenderTargets, so the viewport
        // hook (gated on gInShadowPass) misses it. On shadow-pass entry, query
        // the already-set viewport and scale it to match the replacement atlas.
        if (nowInShadowPass && !gInShadowPass)
        {
            UINT nVP = 0;
            pThis->RSGetViewports(&nVP, nullptr);
            if (nVP > 0 && nVP <= D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
            {
                D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
                pThis->RSGetViewports(&nVP, vps);
                float s = gShadowViewportScale;
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
    }
    gInShadowPass = nowInShadowPass;
    GeometryCapture::OnOMSetRenderTargetsWithResult(isGBuffer);

    if (!wasInGBuffer && isGBuffer)
        POMState::OnGBufferEnter(pThis);
    else if (wasInGBuffer && !isGBuffer)
        POMState::OnGBufferLeave(pThis);
}

// OGRE 2.0 binds RTV/DSV through this combined call rather than the plain
// OMSetRenderTargets — so the shadow caster pass is invisible to slot-33
// hooks alone. Mirror the DSV-identity check here so gInShadowPass tracks
// shadow binds regardless of which API the engine used.
static void STDMETHODCALLTYPE HookedOMSetRenderTargetsAndUAV(
    ID3D11DeviceContext* pThis,
    UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts)
{
    if (gShutdownSignaled) {
        oOMSetRenderTargetsAndUAV(pThis, NumRTVs, ppRenderTargetViews,
            pDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews,
            pUAVInitialCounts);
        return;
    }

    bool nowInShadowPass = ResolveIsShadowDsv(pDepthStencilView);
    if (nowInShadowPass) gShadowMatchCount.fetch_add(1, std::memory_order_relaxed);

    if (gShadowSwapActive)
    {
        ID3D11DepthStencilView* swapDSV = MaybeSwapShadowDSV(pDepthStencilView);
        ID3D11RenderTargetView* swapRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        ID3D11RenderTargetView* const* finalRTVs = ppRenderTargetViews;
        bool anyRTVSwapped = false;
        if (ppRenderTargetViews && NumRTVs > 0)
        {
            for (UINT i = 0; i < NumRTVs && i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
            {
                swapRTVs[i] = MaybeSwapShadowRTV(ppRenderTargetViews[i]);
                if (swapRTVs[i] != ppRenderTargetViews[i]) anyRTVSwapped = true;
            }
            if (anyRTVSwapped) finalRTVs = swapRTVs;
        }
        if (swapDSV != pDepthStencilView || anyRTVSwapped)
            nowInShadowPass = true;
        oOMSetRenderTargetsAndUAV(pThis, NumRTVs, finalRTVs,
            swapDSV, UAVStartSlot, NumUAVs, ppUnorderedAccessViews,
            pUAVInitialCounts);

        if (nowInShadowPass && !gInShadowPass)
        {
            UINT nVP = 0;
            pThis->RSGetViewports(&nVP, nullptr);
            if (nVP > 0 && nVP <= D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
            {
                D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
                pThis->RSGetViewports(&nVP, vps);
                float s = gShadowViewportScale;
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
    }
    gInShadowPass = nowInShadowPass;
}

// ==================== Shadow atlas SRV / viewport swap hooks ====================

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

static void STDMETHODCALLTYPE HookedRSSetViewports(
    ID3D11DeviceContext* pThis, UINT NumViewports,
    const D3D11_VIEWPORT* pViewports)
{
    if (!gInShadowPass || !gShadowSwapActive || gShutdownSignaled ||
        !pViewports || NumViewports == 0)
    {
        oRSSetViewports(pThis, NumViewports, pViewports);
        return;
    }

    D3D11_VIEWPORT scaled[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT n = (NumViewports <= D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
           ? NumViewports : D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    float s = gShadowViewportScale;
    for (UINT i = 0; i < n; i++)
    {
        scaled[i] = pViewports[i];
        scaled[i].TopLeftX *= s;
        scaled[i].TopLeftY *= s;
        scaled[i].Width    *= s;
        scaled[i].Height   *= s;
    }
    oRSSetViewports(pThis, n, scaled);
}

// ==================== Swap chain hooks (ImGui) ====================

static void TickGuiOnPresent(IDXGISwapChain* swapChain, const char* via)
{
    ++gPresentHookCallCount;

    // One-shot probe to discover Kenshi's shadow node name and lambda value.
    // Cheap after the first success (early-returns on sProbed). Lives here
    // rather than TryInstallSwapChainHooks so it can keep retrying until
    // CompositorManager2 is alive — the swap-chain installer fires only once.
    ShadowProbe::TryProbe();

    // Periodic diagnostic: confirm Present is firing and show init state
    if (gPresentHookCallCount <= 5 ||
        (gPresentHookCallCount <= 600 && (gPresentHookCallCount % 60) == 0))
    {
        Log("Present #%llu via %s: sc=%p captured=%d guiDone=%d boot=%d draws=%llu",
            (unsigned long long)gPresentHookCallCount, via, swapChain,
            (int)gDeviceCaptured,
            (int)gGuiInitDone,
            (int)sTryCaptureFromBoot,
            (unsigned long long)gDrawHookCallCount);
    }

    // Splash/loader filter. Slow-startup machines (e.g. Iblis: Havok loader
    // takes seconds) Present a transient splash swap chain before the main
    // game one — initializing ImGui against a window that's about to be
    // destroyed crashes when the loader exits.
    //
    // Two-stage gate by Kenshi-side lifecycle events, not pointer heuristics:
    //   1. Skip everything until SignalGameAlive() fires (either the title
    //      screen has become visible, or the in-game loop has started). By
    //      definition, anything Presenting before that is pre-game.
    //   2. After the game is alive, latch the first Present we see. VTable
    //      hooks fire on every swap chain sharing the vtable, so we filter
    //      later Presents to that single swap chain.
    if (!gGameAlive)
    {
        static int sLogCount = 0;
        if (sLogCount < 3)
        {
            Log("Skipping pre-game Present on swap chain %p (loader/splash phase)",
                swapChain);
            ++sLogCount;
        }
        return;
    }

    if (!gCanonicalSwapChain)
    {
        gCanonicalSwapChain = swapChain;
        Log("Canonical game swap chain latched: %p (after game loop alive)", swapChain);
    }
    else if (swapChain != gCanonicalSwapChain)
    {
        static int sLogCount = 0;
        if (sLogCount < 3)
        {
            Log("Ignoring Present on non-canonical swap chain %p (canonical=%p)",
                swapChain, gCanonicalSwapChain);
            ++sLogCount;
        }
        return;
    }

    // Survey: finalize frame at Present boundary
    if (Survey::IsActive())
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

    if (!gGuiInitDone)
    {
        if (DustGUI::Init(swapChain, gDevice, gContext))
        {
            gGuiInitDone = true;
            Log("GUI initialized successfully (swap chain via %s)",
                sTryCaptureFromBoot ? "DustBoot preload" : "runtime discovery");
        }
        else
        {
            static int sRetryCount = 0;
            if (sRetryCount < 3)
                Log("GUI init failed, will retry (attempt %d)", ++sRetryCount);
        }
    }

    // OGRE's swapBuffers hook (if installed) is what actually renders the GUI
    // — see OgreSwapHook. Initialization stays here because this is the path
    // we have the DXGI swap chain on. Render is skipped to avoid double-draw.
    if (OgreSwapHook::IsInstalled())
        return;

    DustGUI::Render();
}

static HRESULT STDMETHODCALLTYPE HookedPresent(
    IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags)
{
    ZoneScoped;
    if (!gShutdownSignaled) TickGuiOnPresent(pThis, "Present");
#ifdef TRACY_ENABLE
    // Harvest finished GPU timestamps for this frame. Must run on the same
    // context that issued the zones. Cheap when there are no completed
    // queries; Tracy polls non-blocking.
    if (gTracyGpuCtx) TracyD3D11Collect(gTracyGpuCtx);
#endif
    // Periodic diagnostic for shadow-pass detection. Every 120 frames
    // (~1-2s) log "matches=X draws=Y" so we can tell whether our DSV
    // compare ever lands (matches > 0) and whether draws actually fire
    // with gInShadowPass=true (draws > 0). The pair tells us where the
    // shadow profiling is breaking.
    if ((gPresentHookCallCount % 120) == 0)
    {
        static uint64_t sPrevMatches = 0;
        static uint64_t sPrevDraws   = 0;
        uint64_t m = gShadowMatchCount.load(std::memory_order_relaxed);
        uint64_t d = gShadowDrawCount.load(std::memory_order_relaxed);
        Log("Shadow diag: matches=%llu (+%llu) draws=%llu (+%llu)",
            (unsigned long long)m, (unsigned long long)(m - sPrevMatches),
            (unsigned long long)d, (unsigned long long)(d - sPrevDraws));
        sPrevMatches = m;
        sPrevDraws   = d;
    }
    HRESULT hr = oPresent(pThis, SyncInterval, Flags);
    FrameMark;
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedPresent1(
    IDXGISwapChain1* pThis, UINT SyncInterval, UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    ZoneScoped;
    if (!gShutdownSignaled) TickGuiOnPresent(pThis, "Present1");
#ifdef TRACY_ENABLE
    if (gTracyGpuCtx) TracyD3D11Collect(gTracyGpuCtx);
#endif
    HRESULT hr = oPresent1(pThis, SyncInterval, PresentFlags, pPresentParameters);
    FrameMark;
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    ZoneScopedN("HookedResizeBuffers");
    if (gShutdownSignaled) return oResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);

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

// ==================== CSM cascade matrix interception ====================
// Phase 1: instrumentation only — track CSM-mode deferred $Params cbuffers
// (608 bytes per D3DReflect) and log their cascade matrix contents. This is
// the source-of-truth dump we need before deciding how to rewrite values.
//
// Cbuffer layout (from logged D3DReflect output, $Params for CSM main_fs):
//   csmParams   @ 208, 4x float4   (per-cascade: split, filter radius, fixed bias, depth radius)
//   csmScale    @ 272, 4x float4   (per-cascade: world->atlas-uv scale)
//   csmTrans    @ 336, 4x float4   (per-cascade: world->atlas-uv translate)
//   csmUvBounds @ 400, 4x float4   (per-cascade: atlas tile UV bounds)

namespace CSMIntercept
{
    static const UINT kCbSize           = 608;
    static const UINT kCsmParamsOffset  = 208;
    static const UINT kCsmScaleOffset   = 272;
    static const UINT kCsmTransOffset   = 336;
    static const UINT kCsmUvBoundsOffset = 400;
    static const UINT kCsmCount         = 4;

    static std::unordered_set<ID3D11Buffer*>           sTracked;
    static std::unordered_map<ID3D11Buffer*, void*>    sMapped;  // resource -> mapped pointer
    static std::mutex                                  sMutex;
    // Lock-free fast path: linear scan of a small pointer array avoids
    // the mutex + hash lookup on the ~14M Map/Unmap calls per capture
    // that aren't CSM buffers. Only actual CSM buffer matches take the mutex.
    static constexpr int kMaxFastPtrs = 8;
    static void* sFastPtrs[kMaxFastPtrs] = {};
    static std::atomic<int> sFastCount{0};
    static bool FastCheck(void* p) {
        int n = sFastCount.load(std::memory_order_acquire);
        for (int i = 0; i < n && i < kMaxFastPtrs; i++)
            if (sFastPtrs[i] == p) return true;
        return false;
    }
    static std::atomic<int>                            sTrackedSize{0};
    static std::atomic<int>                            sUpdateCounter{0};
    static std::atomic<int>                            sUnmapCounter{0};
    static std::atomic<bool>                           sLayoutLogged{false};
    static std::atomic<bool>                           sStackLogged{false};

    // One-shot caller-stack capture, fired the first time real CSM data is
    // unmapped. Tells us which DLL/EXE writes the cbuffer (Kenshi_x64.exe,
    // OgreMain_x64.dll, a plugin, etc.) and the per-frame RVAs, so we can
    // pick a higher-level hook point now that OGRE's shadow path is
    // confirmed unused.
    static void LogCallerStack(const void* pSrcData, const char* tag)
    {
        if (sStackLogged.load()) return;

        // Same liveness gate as ClassifyLayout — wait until the engine has
        // populated real cascade data, not the zero-init pass.
        const float* params = (const float*)((const char*)pSrcData + kCsmParamsOffset);
        bool live = false;
        for (UINT i = 0; i < kCsmCount; i++)
            if (params[i*4] > 1e-4f || params[i*4] < -1e-4f) { live = true; break; }
        if (!live) return;

        if (sStackLogged.exchange(true)) return;

        void* frames[24] = {0};
        USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        Log("CSMIntercept: caller stack at %s (%u frames):", tag, (unsigned)n);

        for (USHORT i = 0; i < n; i++)
        {
            HMODULE mod = nullptr;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)frames[i], &mod) && mod)
            {
                char modPath[MAX_PATH];
                GetModuleFileNameA(mod, modPath, MAX_PATH);
                const char* base = strrchr(modPath, '\\');
                base = base ? base + 1 : modPath;

                uintptr_t rva = (uintptr_t)frames[i] - (uintptr_t)mod;
                Log("  [%2u] %p  %s+0x%llx",
                    (unsigned)i, frames[i], base, (unsigned long long)rva);
            }
            else
            {
                Log("  [%2u] %p  (unmapped)", (unsigned)i, frames[i]);
            }
        }
    }

    // One-shot classifier of the cascade texture layout. Verdict gates Step 4
    // of docs/shadow_csm_improvement_plan.md — atlas-packed means lambda +
    // global atlas resolution are the only levers; separate textures would
    // additionally allow per-cascade width/height tuning.
    //
    // Discriminator: csmTrans is the world->atlas-UV translation for each
    // cascade. In atlas-packed mode it bakes in the tile offset, so the four
    // cascades' trans (X,Y) land in distinct atlas cells. In separate-texture
    // mode (each cascade owns full UV) all four trans values cluster together.
    // We bucket into half-unit cells: distinct cells -> atlas-packed.
    //
    // (The reflected csmUvBounds slot turned out to be unused by Kenshi's
    // shader — always zero — so it cannot serve as the discriminator.)
    static void ClassifyLayout(const void* pSrcData)
    {
        if (sLayoutLogged.load()) return;

        const float* params = (const float*)((const char*)pSrcData + kCsmParamsOffset);
        const float* trans  = (const float*)((const char*)pSrcData + kCsmTransOffset);
        auto isClose = [](float a, float b) { float d = a - b; return d > -1e-4f && d < 1e-4f; };

        // Wait until the engine has populated real cascade data — the first
        // Unmap is sometimes a zero-init pass. Cascade split distance (params[0]
        // of each entry) is the liveness signal.
        bool live = false;
        for (UINT i = 0; i < kCsmCount; i++)
            if (!isClose(params[i*4], 0.0f)) { live = true; break; }
        if (!live) return;

        if (sLayoutLogged.exchange(true)) return;

        int cellX[kCsmCount], cellY[kCsmCount];
        for (UINT i = 0; i < kCsmCount; i++)
        {
            const float* t = trans + i*4;
            cellX[i] = (int)(t[0] * 2.0f);
            cellY[i] = (int)(t[1] * 2.0f);
        }
        bool atlasPacked = false;
        for (UINT i = 1; i < kCsmCount; i++)
            if (cellX[i] != cellX[0] || cellY[i] != cellY[0]) { atlasPacked = true; break; }

        if (atlasPacked)
        {
            Log("CSM layout verdict: ATLAS-PACKED (cascades share one texture)");
            Log("  -> only lambda + global atlas resolution are tuning levers");
            for (UINT i = 0; i < kCsmCount; i++)
            {
                const float* t = trans + i*4;
                Log("  cascade %u atlas-cell=(%d,%d) trans=(%.3f, %.3f)",
                    i, cellX[i], cellY[i], t[0], t[1]);
            }
        }
        else
        {
            Log("CSM layout verdict: SEPARATE TEXTURES (cascades cluster in same UV cell)");
            Log("  -> per-cascade width/height is a tuning lever (Step 4 Path A bonus)");
        }
    }

    static void DumpCascades(const void* pSrcData)
    {
        const float* params = (const float*)((const char*)pSrcData + kCsmParamsOffset);
        const float* scale  = (const float*)((const char*)pSrcData + kCsmScaleOffset);
        const float* trans  = (const float*)((const char*)pSrcData + kCsmTransOffset);
        const float* bounds = (const float*)((const char*)pSrcData + kCsmUvBoundsOffset);
        for (UINT i = 0; i < kCsmCount; i++)
        {
            const float* p = params + i*4;
            const float* s = scale  + i*4;
            const float* t = trans  + i*4;
            const float* b = bounds + i*4;
            Log("CSM[%u]: params=(%.3f, %.4f, %.5f, %.4f)", i, p[0], p[1], p[2], p[3]);
            Log("       scale=(%.5f, %.5f, %.5f) trans=(%.5f, %.5f, %.5f)",
                s[0], s[1], s[2], t[0], t[1], t[2]);
            Log("       uvBounds=(%.3f, %.3f, %.3f, %.3f)", b[0], b[1], b[2], b[3]);
        }
    }
}

// PFN_Map/PFN_Unmap/PFN_UpdateSubresource typedefs + o* declarations moved to VTable infrastructure section

typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateBuffer)(
    ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
static PFN_CreateBuffer oCreateBuffer = nullptr;

static HRESULT STDMETHODCALLTYPE HookedCreateBuffer(
    ID3D11Device* pThis, const D3D11_BUFFER_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer)
{
    ZoneScopedN("HookedCreateBuffer");
    if (gShutdownSignaled) return oCreateBuffer(pThis, pDesc, pInitialData, ppBuffer);

    HRESULT hr = oCreateBuffer(pThis, pDesc, pInitialData, ppBuffer);
    if (SUCCEEDED(hr) && pDesc && ppBuffer && *ppBuffer &&
        (pDesc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) &&
        pDesc->ByteWidth == CSMIntercept::kCbSize)
    {
        std::lock_guard<std::mutex> lock(CSMIntercept::sMutex);
        CSMIntercept::sTracked.insert(*ppBuffer);
        CSMIntercept::sTrackedSize.store((int)CSMIntercept::sTracked.size(),
                                          std::memory_order_release);
        int idx = CSMIntercept::sFastCount.load(std::memory_order_relaxed);
        if (idx < CSMIntercept::kMaxFastPtrs)
        {
            CSMIntercept::sFastPtrs[idx] = *ppBuffer;
            CSMIntercept::sFastCount.store(idx + 1, std::memory_order_release);
        }
        Log("CSMIntercept: tracked cbuffer %p (size=%u)", *ppBuffer, pDesc->ByteWidth);
    }

    // TerrainTess CPU-side skip: cache first-vertex position for every VB
    // created with initial data. Kenshi terrain VBs are static (uploaded
    // once at level load), so this gets us 100% cache-hit at draw time —
    // no per-draw CopyResource+Map(READ) stalls. Previously, every fresh
    // chunk caused a sync stall on first sighting; rotating the camera
    // (which exposed new chunks all at once) caused severe lag spikes.
    if (SUCCEEDED(hr) && pDesc && ppBuffer && *ppBuffer &&
        (pDesc->BindFlags & D3D11_BIND_VERTEX_BUFFER) &&
        pInitialData && pInitialData->pSysMem &&
        pDesc->ByteWidth >= 12)
    {
        TerrainTess::OnVertexBufferCreated(*ppBuffer, pInitialData->pSysMem);
    }

    return hr;
}

// ==================== CopyResource hook ====================
// PFN_CopyResource typedef + o* declaration moved to VTable infrastructure section

static void STDMETHODCALLTYPE HookedCopyResource(
    ID3D11DeviceContext* pThis, ID3D11Resource* pDstResource,
    ID3D11Resource* pSrcResource)
{
    if (gShutdownSignaled)
    {
        oCopyResource(pThis, pDstResource, pSrcResource);
        return;
    }

    oCopyResource(pThis, pDstResource, pSrcResource);
}

static void STDMETHODCALLTYPE HookedUpdateSubresource(
    ID3D11DeviceContext* pThis, ID3D11Resource* pDstResource, UINT DstSubresource,
    const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch)
{
    if (gShutdownSignaled)
    {
        oUpdateSubresource(pThis, pDstResource, DstSubresource, pDstBox,
                           pSrcData, SrcRowPitch, SrcDepthPitch);
        return;
    }

    bool tracked = false;
    if (pDstResource && pSrcData &&
        CSMIntercept::sFastCount.load(std::memory_order_acquire) > 0 &&
        CSMIntercept::FastCheck(pDstResource))
    {
        tracked = true;
    }

    if (tracked)
    {
        CSMIntercept::ClassifyLayout(pSrcData);  // one-shot atlas-vs-separate verdict

        // Throttle: log first 3 calls, then once every ~600 calls (~10s @ 60fps)
        int n = CSMIntercept::sUpdateCounter.fetch_add(1);
        if (n < 3 || (n % 600) == 0)
        {
            Log("CSMIntercept: UpdateSubresource on %p (call #%d)", pDstResource, n);
            CSMIntercept::DumpCascades(pSrcData);
        }
    }

    oUpdateSubresource(pThis, pDstResource, DstSubresource, pDstBox,
                       pSrcData, SrcRowPitch, SrcDepthPitch);
}

static HRESULT STDMETHODCALLTYPE HookedMap(
    ID3D11DeviceContext* pThis, ID3D11Resource* pResource, UINT Subresource,
    D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource)
{
    if (gShutdownSignaled)
        return oMap(pThis, pResource, Subresource, MapType, MapFlags, pMappedResource);

    HRESULT hr = oMap(pThis, pResource, Subresource, MapType, MapFlags, pMappedResource);

    // Lock-free pointer check skips the mutex for the ~99.99% of Map calls
    // that aren't CSM buffers. Only actual matches take the lock.
    if (SUCCEEDED(hr) && pResource && pMappedResource && pMappedResource->pData &&
        CSMIntercept::sFastCount.load(std::memory_order_acquire) > 0 &&
        CSMIntercept::FastCheck(pResource))
    {
        std::lock_guard<std::mutex> lock(CSMIntercept::sMutex);
        CSMIntercept::sMapped[(ID3D11Buffer*)pResource] = pMappedResource->pData;
    }

    // Same idea for the terrain tessellation CPU-side far-skip — shadow the
    // VS cbuffer that holds worldViewProjMatrix so TryDrawTessellated can read
    // the chunk's clip-space distance without a GPU readback. The bloom test
    // is inline (no function call) so >99% of Maps bail right here.
    if (SUCCEEDED(hr) && pResource && pMappedResource && pMappedResource->pData &&
        TerrainTess::GetEnabled() && TerrainTess::IsResourceTracked(pResource))
        TerrainTess::OnContextMap(pResource, pMappedResource->pData);

    return hr;
}

static void STDMETHODCALLTYPE HookedUnmap(
    ID3D11DeviceContext* pThis, ID3D11Resource* pResource, UINT Subresource)
{
    if (gShutdownSignaled) { oUnmap(pThis, pResource, Subresource); return; }

    void* mappedData = nullptr;
    if (pResource && CSMIntercept::sFastCount.load(std::memory_order_acquire) > 0 &&
        CSMIntercept::FastCheck(pResource))
    {
        std::lock_guard<std::mutex> lock(CSMIntercept::sMutex);
        auto it = CSMIntercept::sMapped.find((ID3D11Buffer*)pResource);
        if (it != CSMIntercept::sMapped.end())
        {
            mappedData = it->second;
            CSMIntercept::sMapped.erase(it);
        }
    }
    // (mappedData is a hook point for future cbuffer modification — read/write
    // before the original Unmap commits the data to the GPU.)
    if (mappedData)
    {
        CSMIntercept::ClassifyLayout(mappedData);  // one-shot atlas-vs-separate verdict
        CSMIntercept::LogCallerStack(mappedData, "HookedUnmap"); // one-shot stack dump

        // Apply user's per-cascade filter scales to csmParams[i].y in-place.
        // Map(WRITE_DISCARD) means OGRE writes fresh values each frame, so we
        // multiply once per commit — no cumulative drift.
        PssmDetour::ApplyFilterScalesToCbuffer(mappedData);

        // Snapshot the cbuffer fields for plugins that need shadow-space
        // sampling (e.g. volumetric fog). Reads at offsets discovered via
        // D3DReflect on the patched deferred PS at compile time. No-op until
        // those offsets are known.
        CSMCapture::OnUnmap(mappedData, CSMIntercept::kCbSize);

        // Throttled raw dump on the Unmap path — diagnostic for figuring out
        // when (if ever) real cascade data lands. First 3 calls + every 600.
        int n = CSMIntercept::sUnmapCounter.fetch_add(1);
        if (n < 3 || (n % 600) == 0)
        {
            Log("CSMIntercept: Unmap on %p (call #%d)", pResource, n);
            CSMIntercept::DumpCascades(mappedData);
        }
    }

    // Terrain tess shadow: commit the latest cb content to our CPU copy
    // BEFORE the original Unmap runs (mappedData is still valid pre-Unmap).
    if (TerrainTess::GetEnabled() && pResource && TerrainTess::IsResourceTracked(pResource))
        TerrainTess::OnContextUnmap(pResource);

    oUnmap(pThis, pResource, Subresource);
}

// ==================== Install ====================

// VTIDX_* constants moved to top of file (needed by VTable hook infrastructure)

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

    void* addrCreateBuffer = devVtable[VTIDX_DEVICE_CreateBuffer];
    void* addrCreateTex2D  = devVtable[VTIDX_DEVICE_CreateTexture2D];
    void* addrCreateVS     = devVtable[VTIDX_DEVICE_CreateVertexShader];
    void* addrCreatePS     = devVtable[VTIDX_DEVICE_CreatePixelShader];
    void* addrPSSetShader  = ctxVtable[VTIDX_CTX_PSSetShader];
    void* addrVSSetShader  = ctxVtable[VTIDX_CTX_VSSetShader];
    void* addrDraw         = ctxVtable[VTIDX_CTX_Draw];
    void* addrDrawIndexed  = ctxVtable[VTIDX_CTX_DrawIndexed];
    void* addrDrawIdxInst  = ctxVtable[VTIDX_CTX_DrawIndexedInstanced];
    void* addrOMSetRT      = ctxVtable[VTIDX_CTX_OMSetRenderTargets];
    void* addrOMSetRTUAV   = ctxVtable[VTIDX_CTX_OMSetRenderTargetsAndUAV];
    void* addrPSSetSRV     = ctxVtable[VTIDX_CTX_PSSetShaderResources];
    void* addrRSSetVP      = ctxVtable[VTIDX_CTX_RSSetViewports];
    void* addrCopyRes      = ctxVtable[VTIDX_CTX_CopyResource];
    void* addrUpdateSubres = ctxVtable[VTIDX_CTX_UpdateSubresource];
    void* addrMap          = ctxVtable[VTIDX_CTX_Map];
    void* addrUnmap        = ctxVtable[VTIDX_CTX_Unmap];
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
    Log("  OMSetRenderTargets    = %p", addrOMSetRT);
    Log("  OMSetRTAndUAV         = %p", addrOMSetRTUAV);
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

    // --- Detours hooks: device methods + Draw (low frequency, needed before device capture) ---

    if (KenshiLib::AddHook(addrCreateBuffer, (void*)HookedCreateBuffer,
                           (void**)&oCreateBuffer) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook CreateBuffer (CSM cbuffer tracking disabled)"); }

    if (KenshiLib::AddHook(addrCreateTex2D, (void*)HookedCreateTexture2D,
                           (void**)&oCreateTexture2D) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook CreateTexture2D"); ok = false; }

    if (KenshiLib::AddHook(addrCreateVS, (void*)HookedCreateVertexShader,
                           (void**)&oCreateVertexShader) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook CreateVertexShader (shader source tracking for VS disabled)"); }

    if (KenshiLib::AddHook(addrCreatePS, (void*)HookedCreatePixelShader,
                           (void**)&oCreatePixelShader) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook CreatePixelShader"); ok = false; }

    if (KenshiLib::AddHook(addrDraw, (void*)HookedDraw,
                           (void**)&oDraw) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook Draw"); ok = false; }

    // --- VTable-managed context methods: set o* from temp device as defaults. ---
    // The real originals are saved from the game device in TryCaptureDevice().
    // Until then, these pointers are valid (same D3D11 runtime code).
    oMap                     = (PFN_Map)addrMap;
    oUnmap                   = (PFN_Unmap)addrUnmap;
    oDrawIndexed             = (PFN_DrawIndexed)addrDrawIndexed;
    oDrawIndexedInstanced    = (PFN_DrawIndexedInstanced)addrDrawIdxInst;
    oPSSetShader             = (PFN_PSSetShader)addrPSSetShader;
    oVSSetShader             = (PFN_VSSetShader)addrVSSetShader;
    oOMSetRenderTargets      = (PFN_OMSetRenderTargets)addrOMSetRT;
    oOMSetRenderTargetsAndUAV = (PFN_OMSetRenderTargetsAndUAV)addrOMSetRTUAV;
    oPSSetShaderResources    = (PFN_PSSetShaderResources)addrPSSetSRV;
    oRSSetViewports          = (PFN_RSSetViewports)addrRSSetVP;
    oCopyResource            = (PFN_CopyResource)addrCopyRes;
    oUpdateSubresource       = (PFN_UpdateSubresource)addrUpdateSubres;
    Log("  12 context methods set for VTable hooking (deferred to device capture)");

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
        Log("Detours hooks installed, VTable context hooks + swap chain hooks deferred");

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

} // namespace D3D11Hook
