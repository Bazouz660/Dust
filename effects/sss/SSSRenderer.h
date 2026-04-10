#pragma once

#include "../../src/DustAPI.h"
#include <d3d11.h>

namespace SSSRenderer
{
    bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir);
    void Shutdown();

    // Extract sun direction and inverse view matrix from the game's PS constant buffer.
    // Call in preExecute when the deferred shader's CBs are bound.
    void ExtractLightData(ID3D11DeviceContext* ctx);

    // Generate SSS shadow mask (ray march + blur). Saves/restores GPU state via host API.
    // Returns the SSS SRV (R8_UNORM, white=lit, dark=shadowed).
    ID3D11ShaderResourceView* RenderSSS(ID3D11DeviceContext* ctx,
                                         ID3D11ShaderResourceView* depthSRV);

    // Composite SSS onto HDR target (multiplicative darkening).
    void Composite(ID3D11DeviceContext* ctx,
                   ID3D11RenderTargetView* hdrRTV);

    // Render debug overlay onto HDR target.
    void RenderDebugOverlay(ID3D11DeviceContext* ctx,
                            ID3D11RenderTargetView* hdrRTV);

    void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight);
    bool IsInitialized();
    bool HasValidLightData();
}
