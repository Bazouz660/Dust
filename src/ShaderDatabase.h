#pragma once

#include <d3d11.h>
#include <cstdint>
#include "DustAPI.h"

namespace ShaderDatabase
{
    void OnPixelShaderCreated(ID3D11PixelShader* ps);
    void OnVertexShaderCreated(ID3D11VertexShader* vs);

    DustShaderCategory GetPixelShaderCategory(ID3D11PixelShader* ps);
    DustShaderCategory GetVertexShaderCategory(ID3D11VertexShader* vs);

    const char* GetSourceName(uint64_t shaderPtr);

    void Shutdown();
}
