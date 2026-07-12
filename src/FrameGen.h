#pragma once

#include <d3d11.h>
#include <cstdint>

struct IDXGISwapChain;

// D.3.0 frame-generation present-takeover — SPIKE (passthrough, no interpolation yet). When
// [Upscaling] FrameGen=1, DustBoot redirects the game's swap chain to a hidden off-screen window (so the
// real HWND is free) and Dust presents the game's frames through OUR D3D12 swap chain on the real HWND,
// bridged via the D.0 interop. This proves we can own presentation before wiring the FSR3 frame-gen swap
// chain. Everything is a safe no-op unless enabled AND the D3D12 side-device comes up.
namespace FrameGen
{
    bool IsWanted();   // [Upscaling] FrameGen=1 (read from DustBoot)

    // Called from the game's Present hook, after the GUI is drawn onto the game's backbuffer. Copies the
    // game's frame across to our D3D12 swap chain on the real HWND and presents it. Returns true once it
    // has taken over presentation (so the caller can present the game's own swap chain harmlessly/fast).
    bool PresentTakeover(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                         IDXGISwapChain* gameSwapChain, uint32_t syncInterval);

    void Shutdown();
}
