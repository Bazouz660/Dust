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
    // Tunable parameters exposed to the GUI. Mirrored into a HLSL cbuffer
    // each Begin() so changes take effect on the next draw.
    struct Controls
    {
        float maxFactor       = 64.0f;
        float factFadeStart   = 20.0f;
        float factFadeEnd     = 200.0f;
        float amplitude       = 1.0f;
        float ampFadeStart    = 50.0f;
        float ampFadeEnd      = 200.0f;
        float ampFadeEnabled  = 0.0f;   // 0 = no fade, 1 = fade with depth
        float uvTileFactor    = 250.0f;
        // Diagnostic: -1 = use full PS-style blend, 0-5 = force display
        // that single slice across all terrain. Helps verify slice mapping
        // (slice 0 = base, 1 = slope, 2 = cliff, 3 = grass, 4 = dirt, 5 = road).
        float debugSlice      = -1.0f;
        // Override terrain albedo with the blended heightmap when > 0.5,
        // so we can SEE the heightmap painted on the terrain in-game.
        // Only takes effect on BLEND0 PS variants (where displacement runs).
        float debugViewMode   = 0.0f;
        // Where in [0..1] the heightmap maps to "ground level". 0.5 = signed
        // around mean (equal up + down). Lower values bias displacement
        // upward — most pixels rise above the original mesh, fewer dip below.
        // Useful to keep characters/objects from sinking into the ground.
        float displacementBias = 0.3f;
        // Quantize the per-edge tess factor to multiples of this for visual
        // stability — small camera moves no longer flip vertices in/out as
        // factors crawl through integer thresholds.
        float factorSnapStep   = 4.0f;
        // Blend factor between per-vertex normal (0) and world-up (1) for
        // the displacement direction. Adjacent chunks have per-vertex
        // normals that differ by tiny amounts at the shared boundary,
        // causing cracks when displaced. World-up is identical across
        // all chunks, so 1.0 eliminates cross-chunk seams entirely.
        float dispDirWorldUp   = 0.0f;
        // Extra mip bias added on top of the auto-computed mip. Positive
        // values force sampling from blurrier mips → smoother displacement
        // at the cost of fine detail. Negative values are clamped to 0
        // (forcing more detail can re-introduce sub-tessellation spikes).
        float mipBias          = 0.0f;
        // Width (in chunk-UV units, 0..1) over which displacement fades
        // to zero at the patch's chunk-UV edges. Adjacent chunks ALSO
        // fade to zero at the shared boundary, so the boundary vertex
        // displaces by 0 from both sides — eliminates cross-region seams.
        // Cost: a narrow valley along every chunk boundary.
        // 0 = disabled.
        float chunkBoundaryFade = 0.01f;
        float _pad[1]          = { 0 };  // align to 64 bytes
    };

    Controls* GetControls();

    // Bake-time high-pass: suppresses low-frequency macro shape so amplitude
    // can be cranked up without characters floating/sinking over hills.
    // cutoff is in cycles/image (Gaussian sigma in freq domain) — lower =
    // suppresses wider features. strength is 0..1. cutoff=0 disables.
    // Changing these requires a rebake to take effect.
    float GetBakeHighPassCutoff();
    float GetBakeHighPassStrength();
    void  SetBakeHighPass(float cutoff, float strength);

    // Bake-time mix between FFT-from-normal (0) and luminance-as-height (1).
    // For textures where the normal map is flat but the diffuse encodes
    // visible depth, raising mix toward 1 recovers that depth signal.
    // Requires rebake to take effect.
    float GetBakeLumMix();
    void  SetBakeLumMix(float mix);

    // Master enable/disable for the whole tessellation pipeline. When off,
    // terrain renders flat — no HS/DS, no bake.
    bool  GetEnabled();
    void  SetEnabled(bool enabled);

    // GPU time consumed by terrain tess draws on the most recent frame
    // (in ms). 0 if no tess this frame, or if the timestamp data isn't
    // ready yet (queries lag by ~1 frame).
    float GetGpuTimeMs();

    // Called by D3D11Hook::ResetFrameState at the start of each frame —
    // closes the previous frame's disjoint timestamp query and harvests
    // the prior frame's results into GpuTimeMs.
    void OnFrameEnd();

    // Free all cached heightmap arrays so the next terrain draw rebakes them
    // with current high-pass settings. Safe to call from the GUI thread —
    // resources only get touched on the render thread.
    void RebakeAll();

    // Compiles the passthrough HS and DS, and loads the heightmap from
    // <modDir>/heightmaps/sharpgravel_HGT.bin. Called once at device capture.
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

    // Called from the CreatePixelShader hook so we can keep the bytecode
    // around for reflection. Used to determine the actual PS cbuffer layout
    // (offsets of scalesA/B/C, slopeMin/Max/Blend) at first terrain draw,
    // since the HLSL source layout may differ from the compiled bytecode
    // due to optimizer-driven uniform stripping.
    void OnPixelShaderCreated(const void* bytecode, size_t size, ID3D11PixelShader* ps);

    // Reflect the terrain VS to find byte offsets of `worldMatrix` and
    // `overlayData` in its $Globals cbuffer — used at draw time to extract
    // chunk world position and chunk size for cross-region neighbor lookup.
    void OnVertexShaderCreated(const void* bytecode, size_t size, ID3D11VertexShader* vs);

    // Called from the Map/Unmap hooks to snoop terrain VS cbuffer writes.
    // OnCbufferMap stashes the mapped pointer; OnCbufferUnmap reads bytes
    // BEFORE the unmap commits to GPU. Both are no-ops for non-tracked
    // buffers and skip a fast atomic check before any work.
    void OnCbufferMap(ID3D11Buffer* buf, void* dataPtr);
    void OnCbufferUnmap(ID3D11Buffer* buf);
}
