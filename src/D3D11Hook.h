#pragma once

#include <d3d11.h>

namespace D3D11Hook
{
    // Discover D3D11 function addresses using a temporary device, then install
    // hooks via KenshiLib::AddHook. The real device/context are captured from
    // the first hooked call's `this` pointer — no OGRE ABI dependency.
    bool Install();

    // The real device/context, captured from hook this pointers
    extern ID3D11Device* gDevice;
    extern ID3D11DeviceContext* gContext;
    extern bool gDeviceCaptured;

    // Reset per-frame state (call at frame start)
    void ResetFrameState();

    // Present hook diagnostics and recovery
    bool IsPresentHooked();     // true if Present hook has actually fired
    void TryRecoverPresent();   // re-attempt swap chain discovery + hook

    // Signal all hooks to pass through to originals (game shutting down).
    // KenshiLib trampolines can't be removed, so any in-flight call after
    // we begin teardown must skip our logic to avoid touching freed state.
    void SignalShutdown();
}
