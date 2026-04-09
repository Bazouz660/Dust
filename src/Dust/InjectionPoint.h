#pragma once

#include <d3d11.h>
#include <cstdint>

enum class InjectionPoint : uint8_t
{
    POST_GBUFFER,
    POST_LIGHTING,
    POST_FOG,
    POST_TONEMAP,
    PRE_PRESENT
};

struct FrameContext
{
    InjectionPoint              point;

    ID3D11Device*               device      = nullptr;
    ID3D11DeviceContext*        context     = nullptr;

    // GBuffer resources (available from POST_GBUFFER onward)
    ID3D11ShaderResourceView*   depthSRV    = nullptr;  // R32_FLOAT
    ID3D11ShaderResourceView*   normalsSRV  = nullptr;  // B8G8R8A8_UNORM
    ID3D11ShaderResourceView*   albedoSRV   = nullptr;  // B8G8R8A8_UNORM

    // HDR scene target (available from POST_LIGHTING onward)
    ID3D11RenderTargetView*     hdrRTV      = nullptr;  // R11G11B10_FLOAT

    // Resolution
    uint32_t                    width       = 0;
    uint32_t                    height      = 0;
    uint64_t                    frameIndex  = 0;
};
