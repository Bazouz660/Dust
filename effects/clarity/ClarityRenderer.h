#pragma once

#include "../../src/DustAPI.h"
#include <d3d11.h>

namespace ClarityRenderer
{
    bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir);
    void Shutdown();

    // Render clarity: blurs the scene copy, then composites enhanced detail onto the LDR target.
    void Render(ID3D11DeviceContext* ctx,
                ID3D11ShaderResourceView* sceneCopySRV,
                ID3D11RenderTargetView* ldrRTV);

    // Render debug overlay (shows the extracted detail layer).
    void RenderDebugOverlay(ID3D11DeviceContext* ctx,
                            ID3D11ShaderResourceView* sceneCopySRV,
                            ID3D11RenderTargetView* ldrRTV);

    void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight);
    bool IsInitialized();
}
