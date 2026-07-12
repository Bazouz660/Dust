#include "FrameGen.h"
#include "DustLog.h"
#include "D3D12Interop.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

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

    bool CreateSwap(HWND hwnd, UINT w, UINT h, DXGI_FORMAT fmt)
    {
        ID3D12CommandQueue* q = D3D12Interop::GetQueue();
        if (!q) { Log("FrameGen: no D3D12 queue"); return false; }

        IDXGIFactory4* factory = nullptr;
        if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void**)&factory)) || !factory)
        { Log("FrameGen: CreateDXGIFactory2 failed"); return false; }

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width            = w;
        sd.Height           = h;
        sd.Format           = fmt;   // flip model supports B8G8R8A8_UNORM
        sd.SampleDesc.Count = 1;
        sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount      = 2;
        sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling          = DXGI_SCALING_STRETCH;
        sd.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;

        // Suspend DustBoot's capture: CreateSwapChainForHwnd is hooked on the shared DXGI vtable, so
        // without this DustBoot would redirect/AddRef our own swap chain as if it were the game's (crash).
        IDXGISwapChain1* sc1 = nullptr;
        if (gBootSuspend) gBootSuspend(true);
        HRESULT hr = factory->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr, nullptr, &sc1);
        if (gBootSuspend) gBootSuspend(false);
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        factory->Release();
        if (FAILED(hr) || !sc1) { Log("FrameGen: CreateSwapChainForHwnd failed 0x%08X", hr); return false; }

        hr = sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&gSwap);
        sc1->Release();
        if (FAILED(hr) || !gSwap) { Log("FrameGen: QI IDXGISwapChain3 failed 0x%08X", hr); return false; }

        gHwnd = hwnd; gW = w; gH = h; gFmt = fmt;
        Log("FrameGen: D3D12 present swap chain on HWND %p (%ux%u fmt=%d) — takeover active", hwnd, w, h, (int)fmt);
        return true;
    }
}

bool IsWanted()
{
    ResolveBoot();
    return gBootWanted && gBootWanted();
}

bool PresentTakeover(ID3D11Device* /*devIgnored*/, ID3D11DeviceContext* /*ctxIgnored*/,
                     IDXGISwapChain* gameSwapChain, uint32_t syncInterval)
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

    // Get the device + immediate context straight from the swap chain — do NOT depend on Dust's POST_LIGHTING
    // device capture, which hasn't happened yet at menu time (the earlier chicken-and-egg black screen).
    ID3D11Device* dev = nullptr;
    gameSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&dev);
    if (!dev) return false;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) { dev->Release(); return false; }
    // Release dev+ctx (both AddRef'd above) on every exit path.
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

    // The game's just-rendered backbuffer (the GUI was drawn onto it right before this present).
    ID3D11Texture2D* gameBB = nullptr;
    gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&gameBB);
    if (!gameBB) return false;

    if (!gShared11 && !D3D12Interop::CreateSharedTexture(gW, gH, (uint32_t)gFmt, &gShared11, &gShared12))
    { gameBB->Release(); return false; }

    // D3D11: copy the game frame into the shared bridge texture, then run the fence-synced D3D12 work.
    ctx->CopyResource(gShared11, gameBB);
    gameBB->Release();

    ID3D12GraphicsCommandList* list = D3D12Interop::BeginD3D12Work(ctx);
    if (!list) return false;

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

    D3D12Interop::SubmitD3D12Work(ctx);   // executes the copy on the present queue
    SafeRelease(scBB);

    gSwap->Present(syncInterval, 0);      // GPU-ordered after the copy on the same queue
    return true;
}

void Shutdown()
{
    SafeRelease(gShared11);
    SafeRelease(gShared12);
    SafeRelease(gSwap);
    gReady = false;
}

} // namespace FrameGen
