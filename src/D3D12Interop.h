#pragma once

#include <d3d11.h>
#include <cstdint>

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
}
