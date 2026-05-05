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
#include "DustLog.h"
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

void SetShadowAtlasResolution(UINT size) { gShadowAtlasOverride = size; }
UINT GetShadowAtlasResolution()          { return gShadowAtlasOverride; }

void SignalGameAlive(const char* via)
{
    if (!gGameAlive)
    {
        gGameAlive = true;
        Log("Game alive (via %s) — splash/loader phase complete", via ? via : "?");
    }
}

void ResetFrameState()
{
    SignalGameAlive("GameWorld::mainLoop");
    gPipelineDetector.ResetFrame();
    gResourceRegistry.ResetFrame();
    gDispatchedThisFrame = false;
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

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateTexture2D)(
    ID3D11Device* pThis, const D3D11_TEXTURE2D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D);

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

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(
    IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags);

typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(
    IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags);

static PFN_CreateTexture2D          oCreateTexture2D = nullptr;
static PFN_CreatePixelShader        oCreatePixelShader = nullptr;
static PFN_Draw                     oDraw = nullptr;
static PFN_DrawIndexed              oDrawIndexed = nullptr;
static PFN_DrawIndexedInstanced     oDrawIndexedInstanced = nullptr;
static PFN_OMSetRenderTargets       oOMSetRenderTargets = nullptr;
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
    if (override != 0 && IsShadowAtlasDesc(pDesc) && pDesc->Width != override)
    {
        D3D11_TEXTURE2D_DESC modDesc = *pDesc;
        modDesc.Width  = override;
        modDesc.Height = override;
        Log("Shadow atlas override: %ux%u %s -> %ux%u",
            pDesc->Width, pDesc->Height,
            FormatName(pDesc->Format) ? FormatName(pDesc->Format) : "?",
            override, override);
        return oCreateTexture2D(pThis, &modDesc, pInitialData, ppTexture2D);
    }

    return oCreateTexture2D(pThis, pDesc, pInitialData, ppTexture2D);
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
        SurveyRecorder::OnPixelShaderCreated(pShaderBytecode, BytecodeLength, *ppPixelShader);
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
        SurveyRecorder::OnVertexShaderCreated(pShaderBytecode, BytecodeLength, *ppVertexShader);
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

static void STDMETHODCALLTYPE HookedDrawIndexed(
    ID3D11DeviceContext* pThis, UINT IndexCount, UINT StartIndexLocation,
    INT BaseVertexLocation)
{
    if (gShutdownSignaled) { oDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation); return; }

    if (Survey::IsActive())
        SurveyRecorder::OnDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);

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

    oDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                          StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

static void STDMETHODCALLTYPE HookedOMSetRenderTargets(
    ID3D11DeviceContext* pThis, UINT NumViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView)
{
    if (gShutdownSignaled) { oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView); return; }

    oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView);
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

typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateBuffer)(
    ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
typedef void (STDMETHODCALLTYPE* PFN_UpdateSubresource)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
    const void*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Map)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT,
    D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void (STDMETHODCALLTYPE* PFN_Unmap)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT);

static PFN_CreateBuffer       oCreateBuffer       = nullptr;
static PFN_UpdateSubresource  oUpdateSubresource  = nullptr;
static PFN_Map                oMap                = nullptr;
static PFN_Unmap              oUnmap              = nullptr;

static HRESULT STDMETHODCALLTYPE HookedCreateBuffer(
    ID3D11Device* pThis, const D3D11_BUFFER_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer)
{
    if (gShutdownSignaled) return oCreateBuffer(pThis, pDesc, pInitialData, ppBuffer);

    HRESULT hr = oCreateBuffer(pThis, pDesc, pInitialData, ppBuffer);
    if (SUCCEEDED(hr) && pDesc && ppBuffer && *ppBuffer &&
        (pDesc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) &&
        pDesc->ByteWidth == CSMIntercept::kCbSize)
    {
        std::lock_guard<std::mutex> lock(CSMIntercept::sMutex);
        CSMIntercept::sTracked.insert(*ppBuffer);
        Log("CSMIntercept: tracked cbuffer %p (size=%u)", *ppBuffer, pDesc->ByteWidth);
    }
    return hr;
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
    if (pDstResource && pSrcData)
    {
        // Cheap pointer-only check — UpdateSubresource is hot. Avoid QueryInterface
        // by reinterpreting (cbuffers and other ID3D11Buffer share vtable layout
        // with ID3D11Resource so the pointer compares directly).
        std::lock_guard<std::mutex> lock(CSMIntercept::sMutex);
        if (CSMIntercept::sTracked.count((ID3D11Buffer*)pDstResource))
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

    // Stash the mapped pointer so HookedUnmap can read/modify the data right
    // before commit. Only track if the resource is in our cbuffer set.
    if (SUCCEEDED(hr) && pResource && pMappedResource && pMappedResource->pData)
    {
        std::lock_guard<std::mutex> lock(CSMIntercept::sMutex);
        ID3D11Buffer* buf = (ID3D11Buffer*)pResource;
        if (CSMIntercept::sTracked.count(buf))
            CSMIntercept::sMapped[buf] = pMappedResource->pData;
    }
    return hr;
}

static void STDMETHODCALLTYPE HookedUnmap(
    ID3D11DeviceContext* pThis, ID3D11Resource* pResource, UINT Subresource)
{
    if (gShutdownSignaled) { oUnmap(pThis, pResource, Subresource); return; }

    void* mappedData = nullptr;
    if (pResource)
    {
        std::lock_guard<std::mutex> lock(CSMIntercept::sMutex);
        ID3D11Buffer* buf = (ID3D11Buffer*)pResource;
        auto it = CSMIntercept::sMapped.find(buf);
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

        // Throttled raw dump on the Unmap path — diagnostic for figuring out
        // when (if ever) real cascade data lands. First 3 calls + every 600.
        int n = CSMIntercept::sUnmapCounter.fetch_add(1);
        if (n < 3 || (n % 600) == 0)
        {
            Log("CSMIntercept: Unmap on %p (call #%d)", pResource, n);
            CSMIntercept::DumpCascades(mappedData);
        }
    }

    oUnmap(pThis, pResource, Subresource);
}

// ==================== Install ====================

static const int VTIDX_DEVICE_CreateBuffer          = 3;
static const int VTIDX_DEVICE_CreateTexture2D       = 5;
static const int VTIDX_DEVICE_CreateVertexShader    = 12;
static const int VTIDX_DEVICE_CreatePixelShader     = 15;
static const int VTIDX_CTX_DrawIndexed              = 12;
static const int VTIDX_CTX_Draw                     = 13;
static const int VTIDX_CTX_Map                      = 14;
static const int VTIDX_CTX_Unmap                    = 15;
static const int VTIDX_CTX_DrawIndexedInstanced     = 20;
static const int VTIDX_CTX_OMSetRenderTargets       = 33;
static const int VTIDX_CTX_UpdateSubresource        = 48;
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

    void* addrCreateBuffer = devVtable[VTIDX_DEVICE_CreateBuffer];
    void* addrCreateTex2D  = devVtable[VTIDX_DEVICE_CreateTexture2D];
    void* addrCreateVS     = devVtable[VTIDX_DEVICE_CreateVertexShader];
    void* addrCreatePS     = devVtable[VTIDX_DEVICE_CreatePixelShader];
    void* addrDraw         = ctxVtable[VTIDX_CTX_Draw];
    void* addrDrawIndexed  = ctxVtable[VTIDX_CTX_DrawIndexed];
    void* addrDrawIdxInst  = ctxVtable[VTIDX_CTX_DrawIndexedInstanced];
    void* addrOMSetRT      = ctxVtable[VTIDX_CTX_OMSetRenderTargets];
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

    if (KenshiLib::AddHook(addrCreateBuffer, (void*)HookedCreateBuffer,
                           (void**)&oCreateBuffer) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook CreateBuffer (CSM cbuffer tracking disabled)"); }

    if (KenshiLib::AddHook(addrUpdateSubres, (void*)HookedUpdateSubresource,
                           (void**)&oUpdateSubresource) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook UpdateSubresource (CSM cbuffer tracking disabled)"); }

    if (KenshiLib::AddHook(addrMap, (void*)HookedMap,
                           (void**)&oMap) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook Map (CSM cbuffer tracking disabled)"); }

    if (KenshiLib::AddHook(addrUnmap, (void*)HookedUnmap,
                           (void**)&oUnmap) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook Unmap (CSM cbuffer tracking disabled)"); }

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
