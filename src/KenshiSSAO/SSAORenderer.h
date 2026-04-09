#pragma once

#include <d3d11.h>

namespace SSAORenderer
{
    // Initialize AO resources (shaders, textures, states).
    // Must be called once after obtaining the D3D11 device.
    bool Init(ID3D11Device* device, UINT width, UINT height);

    // Release all AO resources.
    void Shutdown();

    // Inject the AO pass: generate AO from depth/normals, then multiply onto HDR target.
    // Called from the Draw hook just before the fog pass.
    void Inject(ID3D11DeviceContext* ctx,
                ID3D11ShaderResourceView* depthSRV,
                ID3D11ShaderResourceView* normalsSRV,
                ID3D11RenderTargetView* hdrRTV);

    // Check if resources need recreation (e.g., resolution change).
    void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight);

    // Whether the renderer has been initialized.
    bool IsInitialized();
}
