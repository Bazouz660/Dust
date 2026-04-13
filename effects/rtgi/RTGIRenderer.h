#pragma once

#include "../../src/DustAPI.h"
#include <d3d11.h>

namespace RTGIRenderer
{
    bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir);
    void Shutdown();

    // Called at PRE timing of POST_LIGHTING to extract camera data from the game's CB.
    void ExtractCameraData(ID3D11DeviceContext* ctx);

    // Main render: ray trace -> temporal -> denoise. Returns final GI SRV (RGBA16F).
    // RGB = indirect light (physically-based, before intensity scaling), A = occlusion.
    ID3D11ShaderResourceView* RenderGI(ID3D11DeviceContext* ctx,
                                        ID3D11ShaderResourceView* depthSRV,
                                        ID3D11ShaderResourceView* sceneSRV,
                                        ID3D11ShaderResourceView* normalsSRV);

    void RenderDebugOverlay(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* hdrRTV);

    void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight);
    bool IsInitialized();
    bool HasValidCameraData();
}
