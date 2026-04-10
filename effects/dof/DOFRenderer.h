#pragma once

#include "../../src/DustAPI.h"
#include <d3d11.h>

namespace DOFRenderer
{
    bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir);
    void Shutdown();

    // Render DOF: computes CoC from depth, blurs scene at half res, composites.
    void Render(ID3D11DeviceContext* ctx,
                ID3D11ShaderResourceView* sceneCopySRV,
                ID3D11ShaderResourceView* depthSRV,
                ID3D11RenderTargetView* ldrRTV);

    // Render debug overlay (visualizes CoC map).
    void RenderDebugOverlay(ID3D11DeviceContext* ctx,
                            ID3D11ShaderResourceView* depthSRV,
                            ID3D11RenderTargetView* ldrRTV);

    void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight);
    bool IsInitialized();
}
