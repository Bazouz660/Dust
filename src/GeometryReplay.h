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
    //
    // cullCenter/cullRadius (optional): when cullCenter != nullptr, STATIC draws whose
    // world-matrix translation is farther than cullRadius from cullCenter are skipped.
    // Lets a small point light replay only its nearby occluders (huge perf win, enables
    // many shadow cubes). Instanced / world-less draws are never culled.
    uint32_t Replay(ID3D11DeviceContext* ctx, ID3D11Device* device,
                    const float* replacementVP,
                    const float* cullCenter = nullptr, float cullRadius = 0.0f);

    void Shutdown();
}
