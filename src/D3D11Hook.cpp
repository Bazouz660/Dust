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
#include "DustLog.h"
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

UINT GetShadowAtlasResolution()          { return gShadowAtlasOverride; }

// Runtime shadow atlas resize state. Each entry tracks one shadow atlas
// texture (color or depth) created by the game, plus an optional replacement
// texture/views at the user's chosen resolution. Swap happens at bind time.
struct ShadowAtlasEntry {
    ID3D11Texture2D*          tex;          // AddRef'd original
    IUnknown*                 identity;     // QI(IID_IUnknown) for matching
    D3D11_TEXTURE2D_DESC      desc;         // original desc (vanilla size)
    bool                      isDepth;      // DSV-bound depth vs RTV+SRV color
    ID3D11Texture2D*          newTex;       // replacement (null if no resize active)
    ID3D11DepthStencilView*   newDSV;
    ID3D11RenderTargetView*   newRTV;
    ID3D11ShaderResourceView* newSRV;
    ID3D11Texture2D*          companionDepthTex;  // standalone depth for color-only entries
    ID3D11DepthStencilView*   companionDSV;       // DSV for companion depth
};
static ShadowAtlasEntry gShadowEntries[kMaxShadowIdentities] = {};
static std::atomic<size_t> gShadowEntryCount{0};
static UINT  gShadowBaseSize = 0;              // actual size of original textures
static UINT  gShadowVanillaSize = 0;           // what Kenshi requested at creation
static bool  gShadowSwapActive = false;        // fast-path flag read by every hook
static float gShadowViewportScale = 1.0f;      // newSize / baseSize
static std::atomic<bool> gShadowResizePending{false};
static UINT  gShadowResizeTarget = 0;
static uint64_t gShadowCreationFrame = ~0ull;

UINT GetShadowBaseResolution()           { return gShadowVanillaSize; }

void SetShadowAtlasResolution(UINT size)
{
    gShadowAtlasOverride = size;
    size_t entries = gShadowEntryCount.load(std::memory_order_acquire);
    if (entries == 0) return;

    // 0 clears the override: if a swap is active, queue a resize back to the
    // vanilla size so it gets dismantled (the back-to-base branch in
    // ApplyPendingShadowResize releases the replacements).
    UINT want;
    if (size != 0)                 want = size;
    else if (gShadowSwapActive)    want = gShadowBaseSize;
    else                           return;

    if (want != 0 && want != gShadowResizeTarget)
    {
        Log("Shadow atlas resize queued: %u -> %u (base=%u, entries=%zu)",
            gShadowResizeTarget, want, gShadowBaseSize, entries);
        gShadowResizeTarget = want;
        gShadowResizePending.store(true, std::memory_order_release);
    }
}

// Shadow-pass detection state. gShadowAtlasIdentities holds the IUnknown
// identity pointers (QueryInterface(IID_IUnknown)) of the depth atlas
// Texture2Ds — COM identity rule guarantees this matches across any interface
// query, even if a wrapper layer (RE_Kenshi, debug layer) sits between us and
// the real texture. gInShadowPass flips in the OMSetRenderTargets hooks when
// the bound DSV's resource resolves to one of these identities. Weak pointers
// — we don't AddRef and tolerate the texture outliving us. Up to 8 identities
// tracked simultaneously: Kenshi can create multiple DSV-bound atlas textures
// (RTW + CSM modes, workspace recreate, etc.). Atomic count so the OMSet
// hooks can read without a lock.
static IUnknown* gShadowAtlasIdentities[kMaxShadowIdentities] = {};
static std::atomic<size_t> gShadowAtlasIdentityCount{0};
static bool      gInShadowPass = false;

// Find the ShadowAtlasEntry whose original texture matches this resource's
// IUnknown identity. Returns index or -1. Caller must Release res afterwards
// (this function does NOT consume the ref).
static int FindShadowEntry(ID3D11Resource* res)
{
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
    gShadowAtlasIdentityCount.store(0, std::memory_order_release);
    gShadowBaseSize = 0;
    gShadowVanillaSize = 0;
    gShadowSwapActive = false;
    gShadowViewportScale = 1.0f;
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
    if (count == 0 || newSize == 0)
    {
        gShadowResizePending.store(false, std::memory_order_relaxed);
        return;
    }
    // The atlas can be created before the first draw captures the device
    // (between LoadAll and InitAll) — keep the request pending until then.
    if (!gDevice)
        return;
    gShadowResizePending.store(false, std::memory_order_relaxed);

    // If matching base size, disable swapping (original textures are correct)
    if (newSize == gShadowBaseSize)
    {
        for (size_t i = 0; i < count; i++)
            ReleaseShadowReplacement(gShadowEntries[i]);
        gShadowSwapActive = false;
        gShadowViewportScale = 1.0f;
        gInShadowPass = false;
        Log("Shadow atlas resize: back to base %u — swap disabled", newSize);
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
            if (e.isDepth || !e.newTex) continue;
            D3D11_TEXTURE2D_DESC dd = {};
            dd.Width = newSize;
            dd.Height = newSize;
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
                    Log("  companion DSV created for color entry %zu (%ux%u)", i, newSize, newSize);
            }
        }
    }

    gShadowViewportScale = (float)newSize / (float)gShadowBaseSize;
    gShadowSwapActive = true;
    Log("Shadow atlas swap active, viewport scale = %.3f", gShadowViewportScale);
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

void ResetFrameState()
{
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

// The swap chain pointer we vtable-hooked — needed for recovery verification
static IDXGISwapChain* gHookedSwapChain = nullptr;

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
    // geometry-capture layer, and opt in only when the user enables it in Dust.ini
    // ([Upscaling] CaptureGeometry=1). Default OFF — capture is a per-frame cost.
    GeometryCapture::SetDevice(gDevice);
    GeometryCapture::SetResolution(gWidth, gHeight);
    {
        std::string iniPath = DustLogDir() + "Dust.ini";
        if (GetPrivateProfileIntA("Upscaling", "CaptureGeometry", 0, iniPath.c_str()))
        {
            GeometryCapture::SetCaptureFlags(DUST_CAPTURE_GEOMETRY);
            Log("Upscaling: geometry capture ENABLED ([Upscaling] CaptureGeometry=1)");
        }
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

        // DSV identity tracking (for shadow-pass detection)
        if (unk && isDepth)
        {
            size_t idx = gShadowAtlasIdentityCount.load(std::memory_order_relaxed);
            if (idx < kMaxShadowIdentities)
            {
                gShadowAtlasIdentities[idx] = unk;
                gShadowAtlasIdentityCount.store(idx + 1, std::memory_order_release);
            }
        }

        // Entry tracking (for runtime resize swapping)
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
                e.desc = *pDesc;
                e.isDepth = isDepth;
                if (gShadowBaseSize == 0)
                    gShadowBaseSize = pDesc->Width;
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
static void FirePostLightVolumes(ID3D11DeviceContext* ctx, const char* where)
{
    gPostLightVolumesFired = true;

    DustFrameContext fctx = {};
    fctx.device = gDevice;
    fctx.context = ctx;
    fctx.point = static_cast<DustInjectionPoint>(InjectionPoint::POST_FOG);
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
    }
    return hr;
}

static void STDMETHODCALLTYPE HookedDraw(
    ID3D11DeviceContext* pThis, UINT VertexCount, UINT StartVertexLocation)
{
    if (gShutdownSignaled) { oDraw(pThis, VertexCount, StartVertexLocation); return; }

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
                            GeometryCapture::SetResolution(gWidth, gHeight);   // keep MV GBuffer detect in sync
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

        // v5 postLightVolumes: fire at the first pass after the point/spot light volumes
        // (fog, or tonemap if no fog this frame), so effects can composite over the fully
        // lit scene incl. local lights.
        if (!gPostLightVolumesFired &&
            (result.point == InjectionPoint::POST_FOG ||
             result.point == InjectionPoint::POST_TONEMAP))
        {
            FirePostLightVolumes(pThis, "post-fog");
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

    if (Survey::IsActive())
        SurveyRecorder::OnDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);

    // Motion-vector pass: snapshot this GBuffer draw's transform state. Fast-path
    // no-op unless capture is enabled AND we're inside the GBuffer pass.
    if (GeometryCapture::HasActiveCapture())
        GeometryCapture::OnDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);

    // Direct-light AO for point/spot lights: for a light_fs draw, swap the AO snapshot
    // into s8/s9 for the draw and restore afterward (scope dtor). gLvAoReady gates the
    // PSGetShader check to the POST_LIGHTING→POST_FOG window.
    bool isLV = gLvAoReady && IsLightVolumeDraw(pThis);
    LightVolumeAoScope lvAo(pThis, isLV);
    oDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);
}

static void STDMETHODCALLTYPE HookedDrawIndexedInstanced(
    ID3D11DeviceContext* pThis, UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
    if (gShutdownSignaled) { oDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation); return; }

    if (Survey::IsActive())
        SurveyRecorder::OnDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                                                StartIndexLocation, BaseVertexLocation,
                                                StartInstanceLocation);

    // Motion-vector pass: snapshot this instanced GBuffer draw (per-instance
    // transform VB is captured too — see GeometryCapture::CaptureDrawState).
    if (GeometryCapture::HasActiveCapture())
        GeometryCapture::OnDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                                                StartIndexLocation, BaseVertexLocation,
                                                StartInstanceLocation);

    // Direct-light AO for point/spot lights (instanced light-volume path — see HookedDrawIndexed).
    bool isLV = gLvAoReady && IsLightVolumeDraw(pThis);
    LightVolumeAoScope lvAo(pThis, isLV);
    oDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                          StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

// ==================== Shadow atlas bind-time swap ====================
// Only active while gShadowSwapActive — every hook below early-outs on a
// single global read otherwise, so the feature costs one predictable branch
// per call when the resolution override is off.

// Check if the bound DSV refers to one of the captured shadow atlas
// textures. Resolves via IUnknown identity (the only pointer COM guarantees
// stable across interface queries). Scans the identity set — typically 1-4
// entries — so the cost is a couple virtual calls plus a tiny linear search.
static bool ResolveIsShadowDsv(ID3D11DepthStencilView* dsv)
{
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
    if (!dsv) return dsv;
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    int idx = FindShadowEntry(res);
    if (res) res->Release();
    if (idx >= 0 && gShadowEntries[idx].isDepth && gShadowEntries[idx].newDSV)
        return gShadowEntries[idx].newDSV;
    return dsv;
}

static bool AnyRtvIsShadow(UINT NumViews, ID3D11RenderTargetView* const* ppRTVs,
                           bool& outAnyBound)
{
    outAnyBound = false;
    if (!ppRTVs) return false;
    for (UINT i = 0; i < NumViews; i++)
    {
        if (!ppRTVs[i]) continue;
        outAnyBound = true;
        ID3D11Resource* res = nullptr;
        ppRTVs[i]->GetResource(&res);
        int idx = FindShadowEntry(res);
        if (res) res->Release();
        if (idx >= 0) return true;
    }
    return false;
}

static ID3D11RenderTargetView* MaybeSwapShadowRTV(ID3D11RenderTargetView* rtv)
{
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
    if (gShutdownSignaled)
    { oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView); return; }

    // Motion-vector pass: track GBuffer-pass entry/exit — independent of the
    // shadow-swap feature, so it must run before the !gShadowSwapActive early-out.
    // No-op (single global read) unless geometry capture is enabled.
    if (GeometryCapture::GetCaptureFlags() != 0)
        GeometryCapture::OnOMSetRenderTargetsWithResult(
            GeometryCapture::CheckGBufferConfig(NumViews, ppRenderTargetViews, pDepthStencilView));

    if (!gShadowSwapActive)
    { oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView); return; }

    // Shadow pass detection: DSV match + (RTV match or depth-only), OR
    // RTV match alone (when OGRE reuses a pooled depth buffer we didn't
    // track). A lone DSV match with an unrelated RTV is NOT a shadow pass
    // — OGRE's depth pool can share the shadow depth with other targets.
    bool dsvIsShadow = ResolveIsShadowDsv(pDepthStencilView);
    bool anyRtvBound = false;
    bool rtvIsShadow = AnyRtvIsShadow(NumViews, ppRenderTargetViews, anyRtvBound);
    bool nowInShadowPass = false;
    if (dsvIsShadow)
        nowInShadowPass = rtvIsShadow || !anyRtvBound;
    else if (rtvIsShadow)
        nowInShadowPass = true;

    if (nowInShadowPass)
    {
        ID3D11DepthStencilView* swapDSV = MaybeSwapShadowDSV(pDepthStencilView);
        // If DSV wasn't tracked, use companion DSV from the color entry
        if (swapDSV == pDepthStencilView && !dsvIsShadow)
        {
            size_t cnt = gShadowEntryCount.load(std::memory_order_acquire);
            for (size_t i = 0; i < cnt; i++)
            {
                if (!gShadowEntries[i].isDepth && gShadowEntries[i].companionDSV)
                    { swapDSV = gShadowEntries[i].companionDSV; break; }
            }
        }
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
        oOMSetRenderTargets(pThis, NumViews, finalRTVs, swapDSV);

        // OGRE sets RSSetViewports BEFORE OMSetRenderTargets, so the viewport
        // hook (gated on gInShadowPass) misses it. On shadow-pass entry, query
        // the already-set viewport and scale it to match the replacement atlas.
        if (!gInShadowPass)
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
    if (GeometryCapture::GetCaptureFlags() != 0)
        GeometryCapture::OnOMSetRenderTargetsWithResult(
            GeometryCapture::CheckGBufferConfig(NumRTVs, ppRenderTargetViews, pDepthStencilView));

    if (!gShadowSwapActive)
    {
        oOMSetRenderTargetsAndUAV(pThis, NumRTVs, ppRenderTargetViews,
            pDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews,
            pUAVInitialCounts);
        return;
    }

    bool dsvIsShadow = ResolveIsShadowDsv(pDepthStencilView);
    bool anyRtvBound = false;
    bool rtvIsShadow = AnyRtvIsShadow(NumRTVs, ppRenderTargetViews, anyRtvBound);
    bool nowInShadowPass = false;
    if (dsvIsShadow)
        nowInShadowPass = rtvIsShadow || !anyRtvBound;
    else if (rtvIsShadow)
        nowInShadowPass = true;

    if (nowInShadowPass)
    {
        ID3D11DepthStencilView* swapDSV = MaybeSwapShadowDSV(pDepthStencilView);
        if (swapDSV == pDepthStencilView && !dsvIsShadow)
        {
            size_t cnt = gShadowEntryCount.load(std::memory_order_acquire);
            for (size_t i = 0; i < cnt; i++)
            {
                if (!gShadowEntries[i].isDepth && gShadowEntries[i].companionDSV)
                    { swapDSV = gShadowEntries[i].companionDSV; break; }
            }
        }
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
        oOMSetRenderTargetsAndUAV(pThis, NumRTVs, finalRTVs,
            swapDSV, UAVStartSlot, NumUAVs, ppUnorderedAccessViews,
            pUAVInitialCounts);

        if (!gInShadowPass)
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

    DustGUI::Render();
}

static HRESULT STDMETHODCALLTYPE HookedPresent(
    IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags)
{
    if (!gShutdownSignaled) TickGuiOnPresent(pThis, "Present");
    return oPresent(pThis, SyncInterval, Flags);
}

static HRESULT STDMETHODCALLTYPE HookedPresent1(
    IDXGISwapChain1* pThis, UINT SyncInterval, UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if (!gShutdownSignaled) TickGuiOnPresent(pThis, "Present1");
    return oPresent1(pThis, SyncInterval, PresentFlags, pPresentParameters);
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
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

// ==================== Install ====================

static const int VTIDX_DEVICE_CreateTexture2D       = 5;
static const int VTIDX_DEVICE_CreateVertexShader    = 12;
static const int VTIDX_DEVICE_CreatePixelShader     = 15;
static const int VTIDX_CTX_PSSetShaderResources     = 8;
static const int VTIDX_CTX_DrawIndexed              = 12;
static const int VTIDX_CTX_Draw                     = 13;
static const int VTIDX_CTX_DrawIndexedInstanced     = 20;
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
    void* addrCreateVS     = devVtable[VTIDX_DEVICE_CreateVertexShader];
    void* addrCreatePS     = devVtable[VTIDX_DEVICE_CreatePixelShader];
    void* addrDraw         = ctxVtable[VTIDX_CTX_Draw];
    void* addrDrawIndexed  = ctxVtable[VTIDX_CTX_DrawIndexed];
    void* addrDrawIdxInst  = ctxVtable[VTIDX_CTX_DrawIndexedInstanced];
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

} // namespace D3D11Hook
