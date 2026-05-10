#pragma once

#include <d3d11.h>

// Terrain tessellation. Inserts a Hull/Domain shader pair on TEXTURED GBuffer
// terrain draws to subdivide and displace vertices. Displacement = the PS's
// visible luminance (BLEND0+1+2+3 chain), computed in the DS by replicating
// the PS's exact composition — so DS output equals what the PS draws → no
// chunk seams.
namespace TerrainTess
{
    // Tunable parameters exposed to the GUI. Mirrored into a HLSL cbuffer
    // each Begin() so changes take effect on the next draw.
    //
    // Layout:
    //   12 plugin-set floats (sent via SetTerrainTessControls)
    //   12 host-set floats (per-PS BLEND# channel masks)
    // Total 24 floats = 96 bytes (6 float4 rows).
    struct Controls
    {
        float maxFactor       = 64.0f;
        float factFadeStart   = 20.0f;
        float factFadeEnd     = 200.0f;
        float amplitude       = 1.0f;
        float ampFadeStart    = 50.0f;
        float ampFadeEnd      = 200.0f;
        float ampFadeEnabled  = 0.0f;
        // 0 = off, 1 = PS visible-luminance grayscale, 2 = DS-replica diff overlay.
        float debugViewMode   = 0.0f;
        float displacementBias = 0.3f;
        float factorSnapStep   = 4.0f;
        // Blend per-vertex normal (0) → world-up (1) for displacement direction.
        // 1 fixes seams from boundary normal mismatches.
        float dispDirWorldUp   = 0.0f;
        // 0 = off, 1 = tess wireframe, 2 = vanilla mesh wireframe (no tess),
        // 3 = strip→list IB conversion + TRIANGLELIST + no tess (isolates IB conversion).
        float wireframe        = 0.0f;
        // Per-PS BLEND# channel selectors. Each PS variant uses BLEND1/2/3
        // #defines to pick a channel (0..3 = R/G/B/A) of blendMap. Host fills
        // these per-draw based on the PS's captured defines (see
        // OnTerrainPsCompiled). (1,0,0,0) = R, (0,1,0,0) = G, etc.
        // All-zero = layer inactive; the BLEND chain skips it.
        float blend1Mask[4]    = { 0, 0, 0, 0 };
        float blend2Mask[4]    = { 0, 0, 0, 0 };
        float blend3Mask[4]    = { 0, 0, 0, 0 };
    };

    Controls* GetControls();

    // Master enable/disable. When off, IsTerrainShaderBound returns false and
    // the HS/DS path is bypassed entirely.
    bool  GetEnabled();
    void  SetEnabled(bool enabled);

    // GPU time consumed by terrain tess draws on the most recent frame (ms).
    // 0 if no tess this frame, or if the timestamp data isn't ready yet
    // (queries lag by ~1 frame).
    float GetGpuTimeMs();

    // Called by D3D11Hook::ResetFrameState at the start of each frame —
    // closes the previous frame's disjoint timestamp query and harvests
    // the prior frame's results into GpuTimeMs.
    void OnFrameEnd();

    // Compiles the HS/DS. Called once at device capture.
    void Init(ID3D11Device* device);

    // Releases compiled shaders and any cached state.
    void Shutdown();

    // Function-pointer signature matching ID3D11DeviceContext::DrawIndexed.
    typedef void (STDMETHODCALLTYPE* DrawIndexedFn)(
        ID3D11DeviceContext* ctx, UINT indexCount,
        UINT startIndex, INT baseVertex);

    // Try to render a draw call with tessellation. Returns true if the
    // function handled the draw entirely (caller MUST NOT call drawFn again).
    // Returns false for non-terrain draws — caller should do the standard draw.
    //
    // TRIANGLELIST terrain draws: overrides topology to PATCHLIST and routes
    // through HS/DS.
    // TRIANGLESTRIP terrain draws: converts source IB to a list IB, binds it,
    // routes through HS/DS, restores the original IB binding before returning.
    bool TryDrawTessellated(ID3D11DeviceContext* ctx,
                            UINT indexCount, UINT startIndex,
                            INT baseVertex, DrawIndexedFn drawFn);

    // Diagnostic: reflect a shader's I/O signatures and log them. Used to
    // verify VS-HS-DS-PS semantic chains.
    void LogShaderSignature(const void* bytecode, size_t size, const char* tag);

    // Called from CreatePixelShader hook. Looks up the BLEND-define record
    // (stashed earlier by OnTerrainPsCompiled) and associates it with the PS
    // pointer for per-draw mask lookup; also reflects the PS to determine
    // the cb0 BLEND level (number of layers active).
    void OnPixelShaderCreated(const void* bytecode, size_t size, ID3D11PixelShader* ps);

    // Called from the D3DCompile hook for each successful main_fs / mapfeature_fs
    // compile. Captures BLEND1/2/3 #define values (channel indices for blendMap,
    // 0..3) keyed by bytecode hash. OnPixelShaderCreated then moves the entry
    // into a PS*-keyed map for per-draw lookup.
    void OnTerrainPsCompiled(const void* bytecode, size_t size,
                             int blend1, int blend2, int blend3);
}
