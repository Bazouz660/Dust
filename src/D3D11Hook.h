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
    bool IsShutdownSignaled();

    // Signal that the game has progressed past the loader/splash phase.
    // Either the title screen has become visible (TitleScreen::show(true))
    // or the in-game loop has started — both mean it is safe to attach ImGui
    // to whatever swap chain is presenting. Until one of these fires, every
    // Present is treated as splash/loader and ignored.
    void SignalGameAlive(const char* via);

    // Plugin-driven override for the shadow atlas resolution. 0 disables the
    // override (game default 4096 used). Read by HookedCreateTexture2D when
    // the game creates the 4096^2 shadow atlas / depth pair.
    void SetShadowAtlasResolution(UINT size);
    UINT GetShadowAtlasResolution();
}
