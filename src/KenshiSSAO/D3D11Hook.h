#pragma once

#include <d3d11.h>

namespace D3D11Hook
{
    // Discover D3D11 function addresses using a temporary device, then install
    // hooks via KenshiLib::AddHook. The real device/context are captured from
    // the first hooked call's `this` pointer — no OGRE ABI dependency.
    bool Install();

    // Captured SRVs from the lighting pass
    extern ID3D11ShaderResourceView* gDepthSRV;    // Slot 2 during lighting
    extern ID3D11ShaderResourceView* gNormalsSRV;   // Slot 1 during lighting

    // Captured render target (HDR scene, R11G11B10_FLOAT)
    extern ID3D11RenderTargetView* gHdrRTV;

    // Per-frame flag
    extern bool gAoInjectedThisFrame;

    // The real device/context, captured from hook this pointers
    extern ID3D11Device* gDevice;
    extern ID3D11DeviceContext* gContext;
    extern bool gDeviceCaptured;

    // Reset per-frame state (call at frame start)
    void ResetFrameState();
}
