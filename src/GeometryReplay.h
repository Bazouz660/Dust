#pragma once

#include <d3d11.h>
#include <cstdint>

struct CapturedDraw;

namespace GeometryReplay
{
    // Replay all classified captured GBuffer draws with a replacement view-projection
    // matrix. IA and VS state is saved/restored internally.
    //
    // Caller must set before calling:
    //   - Render targets / depth-stencil view (OM)
    //   - Pixel shader (or null for depth-only)
    //   - Viewport
    //   - Rasterizer / blend / depth-stencil state
    //
    // replacementVP: row-major float4x4 (16 floats). For shadow mapping this is the
    //                light's View*Projection matrix.
    //
    // Returns the number of draws actually replayed (skips unclassified draws).
    uint32_t Replay(ID3D11DeviceContext* ctx, ID3D11Device* device,
                    const float* replacementVP);

    void Shutdown();
}
