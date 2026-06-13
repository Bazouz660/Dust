#pragma once

#include "../../src/DustAPI.h"
#include <d3d11.h>

namespace SSSRenderer
{
    bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir);
    void Shutdown();

    // Apply subsurface scattering to skin-tagged pixels of the lit HDR scene.
    //   sceneCopySRV : a copy of the current HDR scene (read source)
    //   normalsSRV   : GBuffer normals (.a = skin tag)
    //   depthSRV     : GBuffer depth
    //   hdrRTV       : the live HDR render target (write destination)
    void Render(ID3D11DeviceContext* ctx,
                ID3D11ShaderResourceView* sceneCopySRV,
                ID3D11ShaderResourceView* normalsSRV,
                ID3D11ShaderResourceView* depthSRV,
                ID3D11RenderTargetView* hdrRTV,
                UINT width, UINT height);

    void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight);
    bool IsInitialized();
}
