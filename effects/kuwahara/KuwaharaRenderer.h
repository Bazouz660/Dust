#pragma once

#include "../../src/DustAPI.h"
#include <d3d11.h>

namespace KuwaharaRenderer
{
    bool Init(ID3D11Device* device, UINT width, UINT height,
              const DustHostAPI* host, const char* effectDir);
    void Shutdown();
    void Render(ID3D11DeviceContext* ctx,
                ID3D11ShaderResourceView* sceneCopySRV,
                ID3D11RenderTargetView* ldrRTV);
    void OnResolutionChanged(ID3D11Device* device, UINT w, UINT h);
    bool IsInitialized();
}
