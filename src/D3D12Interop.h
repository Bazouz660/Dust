#pragma once

#include <d3d11.h>
#include <cstdint>

// Forward-declare the D3D12 interfaces at GLOBAL scope so the header doesn't need <d3d12.h> (and so the
// pointer types match ::ID3D12Device etc. — declaring them inside the namespace would make distinct types).
struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;

// D.0 interop spike for FSR 3.1 / FSR 4 (both are DX12-only — there is no D3D11 FSR3 backend).
// Proves the bridge those upscalers need: a background D3D12 device on the game's own adapter that
// receives a D3D11 render target, runs a compute pass on it, and hands the result back — all without
// replacing the game's D3D11 swap chain (present stays on D3D11; only frame-gen needs a DX12 swap chain).
//
// OptiScaler is the prior art for exactly this ("background D3D12 device for DX12-only upscalers in a
// DX11 game, ~10% cost"). The single real risk is Proton: D3D11<->D3D12 cross-layer sharing there is the
// fragile frontier (DXVK<->vkd3d-proton; IDXGIKeyedMutex unsupported), so this spike syncs with a SHARED
// FENCE (not a keyed mutex) and every step degrades to a logged no-op if an interface is missing.
namespace D3D12Interop
{
    // One-time init: create a D3D12 device on the SAME adapter as the game's D3D11 device, a compute
    // queue/allocator/list, a shared fence (opened on the game device as an ID3D11Fence), and the tint
    // compute PSO. Returns false and leaves IsReady() false on any failure (old OS, Proton without the
    // needed interop, non-D3D11.4 device). Safe to call every frame — only the first call does work.
    bool Init(ID3D11Device* d3d11Device);
    bool IsReady();

    // Spike: copy `ldrColor` (a w x h game render target) across to D3D12, run a compute shader that
    // tints the LEFT HALF green, and copy the result back into `ldrColor`. A clean green/normal split
    // in-game == the whole cross-API path works (device + shared textures + fence sync + compute
    // dispatch). `ctx` must be the game's immediate context (called on the render thread at POST_TONEMAP).
    // Returns false (and touches nothing) if not ready or a step fails.
    bool RunTintTest(ID3D11DeviceContext* ctx, ID3D11Resource* ldrColor, uint32_t w, uint32_t h);

    void Shutdown();

    // ---- Reusable primitives (used by the FSR3/FSR4 backend, which runs on this same side-device) ----

    // The D3D12 device on the game's adapter (null until Init succeeds). FFX etc. create contexts on it.
    ID3D12Device* GetDevice();

    // The D3D12 direct queue — the present/execute queue. Frame gen creates its swap chain on this queue,
    // and BeginD3D12Work/SubmitD3D12Work execute on it, so a present issued after Submit is GPU-ordered
    // behind the recorded work.
    struct ID3D12CommandQueue* GetQueue();

    // Create a shared texture pair (a D3D11 texture on the game device + the same memory as a D3D12
    // resource, via one NT handle). SRV + copy usable on both sides (no UAV bind — keep those D3D12-local).
    // Used to marshal game color/depth/MV across and the upscaled result back. Returns false on failure.
    bool CreateSharedTexture(uint32_t w, uint32_t h, uint32_t /*DXGI_FORMAT*/ fmt,
                             ID3D11Texture2D** outD3D11, ID3D12Resource** outD3D12);

    // Fence-synced D3D12 work bracket. Sequence per frame:
    //   1. caller queues D3D11 CopyResource(sharedInput, gameTex) calls on `ctx`
    //   2. list = BeginD3D12Work(ctx)  -> signals inputs-ready, waits on the D3D12 queue, waits the
    //      allocator free, resets and returns a command list (null on failure - then do nothing)
    //   3. caller records D3D12 work into `list` (barriers + ffxDispatch + copy result to shared output)
    //   4. SubmitD3D12Work(ctx)        -> executes, signals done, makes the D3D11 context wait for it
    //   5. caller queues D3D11 CopyResource(gameTex, sharedOutput) on `ctx`
    ID3D12GraphicsCommandList* BeginD3D12Work(ID3D11DeviceContext* ctx);
    bool SubmitD3D12Work(ID3D11DeviceContext* ctx);

    // Record a COMMON<->state transition on the in-flight BeginD3D12Work list (exposes the internal helper
    // so the FSR backend can transition shared/compute resources around its dispatch). fromState/toState
    // are D3D12_RESOURCE_STATES. No-op if there is no open list.
    void Transition(ID3D12Resource* r, uint32_t fromState, uint32_t toState);
}
