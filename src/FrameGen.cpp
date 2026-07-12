#include "FrameGen.h"
#include "DustLog.h"
#include "D3D12Interop.h"
#include "CameraAccess.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <string>

#ifdef DUST_HAVE_FSR3
#include "ffx_api.h"
#include "dx12/ffx_api_dx12.h"
#include "dx12/ffx_api_framegeneration_dx12.h"
#include "ffx_framegeneration.h"
#endif

// D.3.0 present-takeover spike. All D3D12 work rides D3D12Interop's device/queue/fence, so a present
// issued after SubmitD3D12Work is GPU-ordered behind the copy that filled the backbuffer.

namespace FrameGen
{
namespace
{
    template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

    // DustBoot exports (the redirect lives there). Resolved once from the loaded DustBoot.dll.
    typedef bool (*PFN_FGWanted)();
    typedef HWND (*PFN_LookupHwnd)(IDXGISwapChain*);
    typedef void (*PFN_Suspend)(bool);
    PFN_FGWanted   gBootWanted  = nullptr;
    PFN_LookupHwnd gBootLookup  = nullptr;
    PFN_Suspend    gBootSuspend = nullptr;
    bool           gBootResolved = false;
    bool           gInPresent    = false;   // our gSwap->Present re-enters the hook — don't recurse

    void ResolveBoot()
    {
        if (gBootResolved) return;
        gBootResolved = true;
        HMODULE b = GetModuleHandleA("DustBoot.dll");
        if (!b) { Log("FrameGen: DustBoot.dll not loaded — cannot take over present"); return; }
        gBootWanted  = (PFN_FGWanted)  GetProcAddress(b, "DustBoot_FrameGenWanted");
        gBootLookup  = (PFN_LookupHwnd)GetProcAddress(b, "DustBoot_LookupRealHwnd");
        gBootSuspend = (PFN_Suspend)   GetProcAddress(b, "DustBoot_SuspendCapture");
    }

    IDXGISwapChain3* gSwap = nullptr;   // our present swap chain on the real HWND
    HWND        gHwnd = nullptr;
    UINT        gW = 0, gH = 0;
    DXGI_FORMAT gFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool        gInitTried = false;
    bool        gReady = false;

    ID3D11Texture2D* gShared11 = nullptr;   // game backbuffer is copied here (D3D11) then read on D3D12
    ID3D12Resource*  gShared12 = nullptr;

#ifdef DUST_HAVE_FSR3
    // ffx-api (FidelityFX FG). Loaded at runtime from the mod dir (loader dispatches to the FG provider).
    HMODULE              gFfxLoaderDll = nullptr, gFfxFgDll = nullptr;
    PfnFfxCreateContext  gFfxCreate    = nullptr;
    PfnFfxDestroyContext gFfxDestroy   = nullptr;
    PfnFfxConfigure      gFfxConfigure = nullptr;
    PfnFfxDispatch       gFfxDispatch  = nullptr;
    ffxContext           gSwapChainCtx = nullptr;   // the FG swapchain context (owns the proxy swapchain)
    bool                 gFfxTried = false, gFfxLoaded = false;

    bool LoadFfx()
    {
        if (gFfxTried) return gFfxLoaded;
        gFfxTried = true;
        std::string mod = DustLogDir();
        std::wstring dir(mod.begin(), mod.end());
        gFfxFgDll     = LoadLibraryW((dir + L"amd_fidelityfx_framegeneration_dx12.dll").c_str());
        gFfxLoaderDll = LoadLibraryW((dir + L"amd_fidelityfx_loader_dx12.dll").c_str());
        if (!gFfxFgDll || !gFfxLoaderDll) { Log("FrameGen: FFX FG DLLs not found in mod dir — using plain passthrough"); return false; }
        gFfxCreate    = (PfnFfxCreateContext) GetProcAddress(gFfxLoaderDll, "ffxCreateContext");
        gFfxDestroy   = (PfnFfxDestroyContext)GetProcAddress(gFfxLoaderDll, "ffxDestroyContext");
        gFfxConfigure = (PfnFfxConfigure)     GetProcAddress(gFfxLoaderDll, "ffxConfigure");
        gFfxDispatch  = (PfnFfxDispatch)      GetProcAddress(gFfxLoaderDll, "ffxDispatch");
        if (!gFfxCreate || !gFfxDispatch || !gFfxDestroy) { Log("FrameGen: ffx-api entry points missing"); return false; }
        gFfxLoaded = true;
        Log("FrameGen: ffx-api FG runtime loaded");
        return true;
    }

    // --- FrameGeneration context (interpolation) + per-frame inputs ---
    ffxContext    gFgContext = nullptr;
    bool          gFgTried   = false;
    uint64_t      gFrameID   = 0;
    LARGE_INTEGER gQpcFreq = {}, gQpcPrev = {};
    // Depth + motion vectors marshaled D3D11 -> D3D12 (like the colour), for the FG prepare dispatch.
    ID3D11Texture2D* gShDepth11 = nullptr; ID3D12Resource* gShDepth12 = nullptr;
    ID3D11Texture2D* gShMV11    = nullptr; ID3D12Resource* gShMV12    = nullptr;
    DXGI_FORMAT      gDepthFmt = DXGI_FORMAT_UNKNOWN, gMvFmt = DXGI_FORMAT_UNKNOWN;

    // The FG swapchain calls this during Present to dispatch interpolation on its own command list.
    ffxReturnCode_t FgGenCallback(ffxDispatchDescFrameGeneration* params, void* userCtx)
    {
        ffxReturnCode_t rc = gFfxDispatch((ffxContext*)userCtx, &params->header);
        static int sN = 0;
        if (sN < 3) { sN++; Log("FrameGen: FG generation callback FIRED (numGenerated=%u, dispatch rc=%u)", params->numGeneratedFrames, rc); }
        return rc;
    }

    bool EnsureFgContext(UINT w, UINT h, DXGI_FORMAT fmt)
    {
        if (gFgTried) return gFgContext != nullptr;
        gFgTried = true;
        ID3D12Device* dev = D3D12Interop::GetDevice();
        if (!dev || !gFfxCreate) return false;

        ffxCreateBackendDX12Desc backend = {};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device      = dev;

        ffxCreateContextDescFrameGeneration createFg = {};
        createFg.header.type      = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        createFg.header.pNext     = &backend.header;
        createFg.displaySize      = { w, h };
        createFg.maxRenderSize    = { w, h };   // render == display (Dust's FG doesn't upscale)
        createFg.backBufferFormat = ffxApiGetSurfaceFormatDX12(fmt);
        createFg.flags            = 0;          // Kenshi depth 0=near (not inverted); post-tonemap LDR

        ffxReturnCode_t rc = gFfxCreate(&gFgContext, &createFg.header, nullptr);
        if (rc != FFX_API_RETURN_OK) { Log("FrameGen: FrameGeneration context create failed (%u)", rc); gFgContext = nullptr; return false; }
        Log("FrameGen: FrameGeneration context created (%ux%u) — interpolation ready", w, h);
        return true;
    }

    // FSR debug tear/pacing lines on generated frames — [Upscaling] FrameGenDebug=1 (default off).
    uint32_t gFgFlags = 0; bool gFgFlagsRead = false;
    uint32_t FgDebugFlags()
    {
        if (!gFgFlagsRead)
        {
            gFgFlagsRead = true;
            if (GetPrivateProfileIntA("Upscaling", "FrameGenDebug", 0, (DustLogDir() + "Dust.ini").c_str()))
                gFgFlags = FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES;
        }
        return gFgFlags;
    }

    float FrameDeltaMs()
    {
        if (gQpcFreq.QuadPart == 0) QueryPerformanceFrequency(&gQpcFreq);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float ms = 16.7f;
        if (gQpcPrev.QuadPart && gQpcFreq.QuadPart) ms = (float)((now.QuadPart - gQpcPrev.QuadPart) * 1000.0 / gQpcFreq.QuadPart);
        gQpcPrev = now;
        return (ms < 1.0f) ? 1.0f : (ms > 200.0f ? 200.0f : ms);
    }

    bool TexDesc2D(ID3D11Resource* r, D3D11_TEXTURE2D_DESC& out)
    {
        if (!r) return false;
        ID3D11Texture2D* t = nullptr;
        r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t);
        if (!t) return false;
        t->GetDesc(&out); t->Release();
        return true;
    }

    // Shared depth + MV textures matching the game buffers (created once for the current size/format).
    UINT gMotionW = 0, gMotionH = 0;
    bool EnsureMotionTextures(ID3D11Resource* depth, ID3D11Resource* mv)
    {
        D3D11_TEXTURE2D_DESC dd, md;
        if (!TexDesc2D(depth, dd) || !TexDesc2D(mv, md)) return false;
        if (gShDepth11 && gShMV11 && dd.Format == gDepthFmt && md.Format == gMvFmt &&
            dd.Width == gMotionW && dd.Height == gMotionH) return true;
        SafeRelease(gShDepth11); SafeRelease(gShDepth12); SafeRelease(gShMV11); SafeRelease(gShMV12);
        if (!D3D12Interop::CreateSharedTexture(dd.Width, dd.Height, (uint32_t)dd.Format, &gShDepth11, &gShDepth12)) return false;
        if (!D3D12Interop::CreateSharedTexture(md.Width, md.Height, (uint32_t)md.Format, &gShMV11, &gShMV12))
        { SafeRelease(gShDepth11); SafeRelease(gShDepth12); return false; }
        gDepthFmt = dd.Format; gMvFmt = md.Format; gMotionW = dd.Width; gMotionH = dd.Height;
        Log("FrameGen: depth(%d)+MV(%d) marshaling textures ready (%ux%u)", (int)dd.Format, (int)md.Format, dd.Width, dd.Height);
        return true;
    }
#endif

    void FillSwapDesc(DXGI_SWAP_CHAIN_DESC1& sd, UINT w, UINT h, DXGI_FORMAT fmt)
    {
        sd = {};
        sd.Width            = w;
        sd.Height           = h;
        sd.Format           = fmt;   // flip model supports B8G8R8A8_UNORM
        sd.SampleDesc.Count = 1;
        sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount      = 2;
        sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling          = DXGI_SCALING_STRETCH;
        sd.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;
    }

    bool CreateSwap(HWND hwnd, UINT w, UINT h, DXGI_FORMAT fmt)
    {
        ID3D12CommandQueue* q = D3D12Interop::GetQueue();
        if (!q) { Log("FrameGen: no D3D12 queue"); return false; }

        IDXGIFactory4* factory = nullptr;
        if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void**)&factory)) || !factory)
        { Log("FrameGen: CreateDXGIFactory2 failed"); return false; }

        DXGI_SWAP_CHAIN_DESC1 sd; FillSwapDesc(sd, w, h, fmt);
        bool created = false;

#ifdef DUST_HAVE_FSR3
        // D.3.1a: create the FSR3 frame-generation proxy swapchain (interpolation stays OFF until it's
        // configured in D.3.1b — so this is still passthrough, just through the FG swapchain). It calls
        // CreateSwapChainForHwnd internally on the shared vtable, so suspend DustBoot's capture around it.
        if (LoadFfx())
        {
            IDXGISwapChain4* fgSwap = nullptr;
            ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 scDesc = {};
            scDesc.header.type    = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
            scDesc.swapchain      = &fgSwap;
            scDesc.hwnd           = hwnd;
            scDesc.desc           = &sd;
            scDesc.fullscreenDesc = nullptr;
            scDesc.dxgiFactory    = factory;
            scDesc.gameQueue      = q;
            if (gBootSuspend) gBootSuspend(true);
            ffxReturnCode_t rc = gFfxCreate(&gSwapChainCtx, &scDesc.header, nullptr);
            if (gBootSuspend) gBootSuspend(false);
            if (rc == FFX_API_RETURN_OK && fgSwap)
            {
                gSwap = fgSwap;   // IDXGISwapChain3* <- IDXGISwapChain4* (upcast; present uses inherited methods)
                created = true;
                Log("FrameGen: FSR3-FG swapchain created on HWND %p (%ux%u) — interpolation OFF (passthrough)", hwnd, w, h);
            }
            else Log("FrameGen: ffx FG swapchain create failed (%u) — falling back to plain D3D12", rc);
        }
#endif

        if (!created)   // plain D3D12 passthrough swapchain (D.3.0 path / no SDK / ffx failed)
        {
            IDXGISwapChain1* sc1 = nullptr;
            if (gBootSuspend) gBootSuspend(true);
            HRESULT hr = factory->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr, nullptr, &sc1);
            if (gBootSuspend) gBootSuspend(false);
            if (SUCCEEDED(hr) && sc1)
            {
                hr = sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&gSwap);
                sc1->Release();
                if (SUCCEEDED(hr) && gSwap) { created = true; Log("FrameGen: plain D3D12 present swapchain on HWND %p (%ux%u)", hwnd, w, h); }
            }
            if (!created) Log("FrameGen: CreateSwapChainForHwnd failed 0x%08X", hr);
        }

        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        factory->Release();
        if (created) { gHwnd = hwnd; gW = w; gH = h; gFmt = fmt; }
        return created;
    }
}

bool IsWanted()
{
    ResolveBoot();
    return gBootWanted && gBootWanted();
}

bool InTakeover() { return gInPresent; }

HWND RealHwndFor(IDXGISwapChain* swapChain)
{
    ResolveBoot();
    if (!gBootWanted || !gBootWanted() || !gBootLookup || !swapChain) return nullptr;
    return gBootLookup(swapChain);
}

bool PresentTakeover(IDXGISwapChain* gameSwapChain, uint32_t syncInterval,
                     ID3D11Resource* depth, ID3D11Resource* motionVectors)
{
    ResolveBoot();
    if (gInPresent || !gBootWanted || !gBootWanted() || !gameSwapChain) return false;

    // Only take over a swap chain that DustBoot actually redirected (its real HWND is free for us). Other
    // swap chains (incl. our own gSwap) present normally — never black them out.
    HWND hwnd = gBootLookup ? gBootLookup(gameSwapChain) : nullptr;
    if (!hwnd) return false;

    // From here we own this present; our gSwap->Present below re-enters HookedPresent — the guard above
    // makes that nested call a fast no-op.
    gInPresent = true;
    struct PresentGuard { ~PresentGuard() { gInPresent = false; } } presentGuard;

    // Device + immediate context straight from the swap chain (works at menu time, no capture dependency).
    ID3D11Device* dev = nullptr;
    gameSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&dev);
    if (!dev) return false;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) { dev->Release(); return false; }
    struct Rel { ID3D11Device* d; ID3D11DeviceContext* c; ~Rel() { if (c) c->Release(); if (d) d->Release(); } } rel{ dev, ctx };

    if (!gInitTried)
    {
        gInitTried = true;
        if (!D3D12Interop::IsReady() && !D3D12Interop::Init(dev)) { Log("FrameGen: D3D12 side-device unavailable"); return false; }
        ID3D11Texture2D* bb = nullptr;
        gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
        if (!bb) { Log("FrameGen: GetBuffer(game) failed"); return false; }
        D3D11_TEXTURE2D_DESC bd; bb->GetDesc(&bd); bb->Release();
        if (!CreateSwap(hwnd, bd.Width, bd.Height, bd.Format)) return false;
        gReady = true;
    }
    if (!gReady || !gSwap) return false;

    ID3D11Texture2D* gameBB = nullptr;
    gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&gameBB);
    if (!gameBB) return false;
    if (!gShared11 && !D3D12Interop::CreateSharedTexture(gW, gH, (uint32_t)gFmt, &gShared11, &gShared12))
    { gameBB->Release(); return false; }

    // Whether frame interpolation runs this frame: we have depth + MV and the FG context is up.
    bool fgActive = false;
#ifdef DUST_HAVE_FSR3
    fgActive = depth && motionVectors && EnsureMotionTextures(depth, motionVectors) && EnsureFgContext(gW, gH, gFmt);
#endif

    // D3D11: marshal the game frame (+ depth/MV) into the shared bridge textures.
    ctx->CopyResource(gShared11, gameBB);
    gameBB->Release();
#ifdef DUST_HAVE_FSR3
    if (fgActive) { ctx->CopyResource(gShDepth11, depth); ctx->CopyResource(gShMV11, motionVectors); }
#endif

    ID3D12GraphicsCommandList* list = D3D12Interop::BeginD3D12Work(ctx);
    if (!list) return false;

    // Copy the game frame into the FG swap chain's current backbuffer (the "real" present frame).
    UINT idx = gSwap->GetCurrentBackBufferIndex();
    ID3D12Resource* scBB = nullptr;
    gSwap->GetBuffer(idx, __uuidof(ID3D12Resource), (void**)&scBB);
    if (scBB)
    {
        using D3D12Interop::Transition;
        Transition(gShared12, D3D12_RESOURCE_STATE_COMMON,  D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(scBB,      D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(scBB, gShared12);
        Transition(scBB,      D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_PRESENT);
        Transition(gShared12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

#ifdef DUST_HAVE_FSR3
    // FG prepare: dilate depth + MV (feeds the interpolation the swap chain runs during Present).
    if (fgActive)
    {
        ffxDispatchDescFrameGenerationPrepareV2 prep = {};
        prep.header.type   = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prep.commandList   = list;
        prep.flags         = FgDebugFlags();
        prep.frameID       = gFrameID;
        prep.renderSize    = { gMotionW, gMotionH };
        prep.depth         = ffxApiGetResourceDX12(gShDepth12, FFX_API_RESOURCE_STATE_COMMON);
        prep.motionVectors = ffxApiGetResourceDX12(gShMV12,    FFX_API_RESOURCE_STATE_COMMON);
        prep.jitterOffset      = { 0.0f, 0.0f };                       // Dust's injected MVs are jitter-free
        prep.motionVectorScale = { -(float)gMotionW, -(float)gMotionH };// our MV = curUV-prevUV -> pixels (neg, like DLSS)
        prep.frameTimeDelta    = FrameDeltaMs();
        float camN = 0, camF = 0, camFov = 0; CameraAccess_GetCameraParams(&camN, &camF, &camFov);
        prep.cameraNear = camN; prep.cameraFar = camF; prep.cameraFovAngleVertical = camFov;
        prep.viewSpaceToMetersFactor = 0.0f;
        prep.reset = false;
        ffxReturnCode_t rcPrep = gFfxDispatch(&gFgContext, &prep.header);
        static int sPrepN = 0;
        if (rcPrep != FFX_API_RETURN_OK && sPrepN < 3) { sPrepN++; Log("FrameGen: FG prepare dispatch FAILED (%u)", rcPrep); }
    }
#endif

    D3D12Interop::SubmitD3D12Work(ctx);   // executes copy + FG prepare on the present queue
    SafeRelease(scBB);

#ifdef DUST_HAVE_FSR3
    // Configure FG for this present (the swap chain's Present then generates the in-between frame).
    if (fgActive)
    {
        ffxConfigureDescFrameGeneration cfg = {};
        cfg.header.type                        = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain                          = gSwap;
        cfg.frameGenerationEnabled             = true;
        cfg.frameGenerationCallback            = FgGenCallback;
        cfg.frameGenerationCallbackUserContext = &gFgContext;
        cfg.presentCallback                    = nullptr;   // D.3.1c: UI composition (HUD-less)
        cfg.presentCallbackUserContext         = nullptr;
        cfg.HUDLessColor                       = FfxApiResource{};
        cfg.flags                              = FgDebugFlags();
        cfg.onlyPresentGenerated               = false;
        cfg.generationRect                     = { 0, 0, (int32_t)gW, (int32_t)gH };
        cfg.frameID                            = gFrameID;
        ffxReturnCode_t rcCfg = gFfxConfigure(&gFgContext, &cfg.header);
        static int sCfgN = 0;
        if (sCfgN < 5) { sCfgN++; Log("FrameGen: FG configure rc=%u (swapChain=%p frameID=%llu)", rcCfg, (void*)gSwap, (unsigned long long)gFrameID); }
        gFrameID++;   // increment only on FG frames -> exact +1 per generated frame (FFX requirement)
    }
#endif

    gSwap->Present(syncInterval, 0);      // FG swap chain interpolates + paces (GPU-ordered after the copy)
    return true;
}

void Shutdown()
{
    SafeRelease(gShared11);
    SafeRelease(gShared12);
#ifdef DUST_HAVE_FSR3
    SafeRelease(gShDepth11); SafeRelease(gShDepth12);
    SafeRelease(gShMV11);    SafeRelease(gShMV12);
    if (gFgContext && gFfxDestroy)      { gFfxDestroy(&gFgContext, nullptr);   gFgContext = nullptr; }
    if (gSwapChainCtx && gFfxDestroy)   { gFfxDestroy(&gSwapChainCtx, nullptr); gSwapChainCtx = nullptr; gSwap = nullptr; }
#endif
    SafeRelease(gSwap);   // plain-path swapchain (no-op if the FG path already nulled it)
    gReady = false;
}

} // namespace FrameGen
