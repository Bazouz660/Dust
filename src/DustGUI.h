#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace DustGUI
{
    // Called once from the Present hook on first invocation.
    bool Init(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context);

    // Called every frame from the Present hook.
    void Render();

    // Cleanup.
    void Shutdown();

    // Whether the overlay is currently visible.
    bool IsVisible();
}
