#pragma once

#include "../../src/DustAPI.h"
#include <d3d11.h>
#include <cstdint>

namespace DPMRenderer
{
    struct Config
    {
        int  enabled        = 1;
        int  applyToScene   = 1;     // wire shadow mask into deferred shader (Phase 3)
        int  debugView      = 0;     // 0=off, 1=count heatmap, 2=shadow mask
        int  dpmResolution  = 1024;  // square; 1024 or 2048
        int  maxPrimsPerTexel = 32;  // d budget (4..64)
        int  maxPrimBufferSize = 524288; // prim buffer capacity (triangles); 512K = 18 MB
        float frustumExtent = 100.0f; // half-extent of light ortho frustum (world units)
        float frustumDepth  = 200.0f; // half-depth of light ortho frustum
        float tanHalfFov    = 0.5218f; // matches Kenshi default; used for world-pos reconstruction

        // Diagnostics: written by the renderer each frame, exposed via the GUI for live
        // visibility. Users can twiddle them but the renderer overwrites every frame.
        float diagSunX = 0.0f;
        float diagSunY = 0.0f;
        float diagSunZ = 0.0f;
        int   diagSunValid = 0;
        int   diagCapturedDraws = 0;
        int   diagReplayedDraws = 0;
    };

    bool Init(ID3D11Device* device, UINT width, UINT height,
              const DustHostAPI* host, const char* effectDir);
    void Shutdown();
    void OnResolutionChanged(ID3D11Device* device, UINT w, UINT h);

    // Pulled from the game's deferred PS CB0 register c0 (xyz).
    // Must be called when the game's deferred CB is still bound.
    void ExtractSunDirection(ID3D11DeviceContext* ctx);
    bool HasValidSunDirection();

    // Build the DPM by replaying captured geometry from the sun's perspective.
    // Returns the number of draws replayed (0 if disabled or no sun data).
    uint32_t BuildDPM(ID3D11DeviceContext* ctx, ID3D11Device* device,
                      const DustCameraData* camera);

    // Trace shadow rays against the DPM for every lit screen pixel.
    // Output is written to the internal shadow mask texture.
    // Must be called after BuildDPM.
    void RayTrace(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* depthSRV,
                  ID3D11ShaderResourceView* normalsSRV,
                  const DustCameraData* camera,
                  UINT viewportW, UINT viewportH);

    // Debug: render the triangle count heatmap (DPM occupancy).
    void RenderDebugHeatmap(ID3D11DeviceContext* ctx,
                            ID3D11RenderTargetView* dstRTV,
                            UINT viewportW, UINT viewportH);

    // Debug: render the ray-traced shadow mask as a fullscreen overlay.
    void RenderDebugShadowMask(ID3D11DeviceContext* ctx,
                               ID3D11RenderTargetView* dstRTV,
                               UINT viewportW, UINT viewportH);

    // Returns the shadow mask SRV produced by the last RayTrace call.
    // Null until RayTrace has been called at least once.
    ID3D11ShaderResourceView* GetShadowSRV();

    // Returns the point-clamp sampler used for the shadow mask.
    ID3D11SamplerState* GetShadowSampler();

    // True after the first successful RayTrace; use to gate deferred shader binding.
    bool IsShadowMaskReady();

    Config& GetConfig();
}
