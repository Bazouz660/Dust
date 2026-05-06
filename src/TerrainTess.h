#pragma once

#include <d3d11.h>

// Pass-through tessellation spike for terrain. Inserts a Hull Shader and
// Domain Shader that subdivide each triangle to a 1-tri patch (no actual
// subdivision yet). Goal of the spike: prove we can change topology and
// attach HS/DS without breaking Kenshi's rendering.
//
// Once the spike is verified, this module will grow:
//   - Distance-based tessellation factors in the HS
//   - Heightmap-driven displacement in the DS
//   - Multiple HS/DS variants for the different terrain VS variants
//     (TEXTURED GBuffer / non-TEXTURED shadow / cubemap)
//
// For now, only the GBuffer TEXTURED variant is tessellated (where both VS
// and PS classify as DUST_SHADER_TERRAIN).
namespace TerrainTess
{
    // Compiles the passthrough HS and DS. Called once at device capture time.
    void Init(ID3D11Device* device);

    // Releases compiled shaders and any cached converted index buffers.
    void Shutdown();

    // Function-pointer signature matching ID3D11DeviceContext::DrawIndexed.
    typedef void (STDMETHODCALLTYPE* DrawIndexedFn)(
        ID3D11DeviceContext* ctx, UINT indexCount,
        UINT startIndex, INT baseVertex);

    // Try to render a draw call with tessellation. Returns true if the function
    // handled the draw entirely (caller MUST NOT call drawFn again). Returns
    // false for non-terrain draws — caller should do the standard draw.
    //
    // For TRIANGLELIST terrain draws: overrides topology to PATCHLIST and
    // routes through HS/DS.
    // For TRIANGLESTRIP terrain draws: converts source index buffer to a
    // list IB (cached per source-IB), binds the converted IB, then routes
    // through HS/DS. Restores original IB binding before returning.
    bool TryDrawTessellated(ID3D11DeviceContext* ctx,
                            UINT indexCount, UINT startIndex,
                            INT baseVertex, DrawIndexedFn drawFn);

    // Diagnostic: reflect a shader's I/O signatures and log them. Used to
    // compare VS output vs HS input + DS output vs PS input for mismatches
    // that cause silent tessellation pipeline failures.
    void LogShaderSignature(const void* bytecode, size_t size, const char* tag);
}
