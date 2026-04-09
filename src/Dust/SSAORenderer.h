#pragma once

#include <d3d11.h>

namespace SSAORenderer
{
    bool Init(ID3D11Device* device, UINT width, UINT height);
    void Shutdown();

    // Generate AO texture (gen + blur only). Saves/restores GPU state.
    // Returns the AO SRV (R8_UNORM, white=unoccluded, dark=occluded).
    ID3D11ShaderResourceView* RenderAO(ID3D11DeviceContext* ctx,
                                        ID3D11ShaderResourceView* depthSRV);

    // Render debug overlay onto HDR target. Called after the lighting draw.
    void RenderDebugOverlay(ID3D11DeviceContext* ctx,
                            ID3D11RenderTargetView* hdrRTV);

    void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight);
    bool IsInitialized();
    ID3D11ShaderResourceView* GetAoSRV();
}
