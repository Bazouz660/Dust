#pragma once

#include "../../src/DustAPI.h"
#include <d3d11.h>

namespace SSAORenderer
{
    bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir);
    void Shutdown();

    // Generate AO texture (gen + blur only). Saves/restores GPU state via host API.
    // Returns the AO SRV (R8_UNORM, white=unoccluded, dark=occluded).
    ID3D11ShaderResourceView* RenderAO(ID3D11DeviceContext* ctx,
                                        ID3D11ShaderResourceView* depthSRV,
                                        const DustCameraData* camera);

    // Render debug overlay onto HDR target. Called after the lighting draw.
    // depthSRV is passed through so debug modes can visualize depth-derived
    // intermediates independently of the AO pipeline.
    void RenderDebugOverlay(ID3D11DeviceContext* ctx,
                            ID3D11RenderTargetView* hdrRTV,
                            ID3D11ShaderResourceView* depthSRV);

    void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight);
    bool IsInitialized();
    ID3D11ShaderResourceView* GetAoSRV();
    float GetLastGpuTimeMs();
}
