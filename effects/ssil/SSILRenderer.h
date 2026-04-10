#pragma once

#include "../../src/DustAPI.h"
#include <d3d11.h>

namespace SSILRenderer
{
    bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir);
    void Shutdown();

    // Generate indirect light texture (gen + blur). Saves/restores GPU state via host API.
    // Returns the IL SRV (R11G11B10_FLOAT, RGB indirect light contribution).
    ID3D11ShaderResourceView* RenderIL(ID3D11DeviceContext* ctx,
                                        ID3D11ShaderResourceView* depthSRV,
                                        ID3D11ShaderResourceView* albedoSRV,
                                        ID3D11ShaderResourceView* normalsSRV);

    // Render debug overlay onto HDR target.
    void RenderDebugOverlay(ID3D11DeviceContext* ctx,
                            ID3D11RenderTargetView* hdrRTV);

    void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight);
    bool IsInitialized();
    ID3D11ShaderResourceView* GetILSRV();
}
