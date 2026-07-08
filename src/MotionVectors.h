#pragma once

#include <d3d11.h>
#include <cstdint>

// Motion-vector pass (upscaling milestone A.2). Builds a true analytic velocity
// buffer by re-rendering captured GBuffer geometry and emitting curClip-prevClip
// per pixel (no depth reconstruction -> avoids Kenshi's far-clip / view-space
// reprojection fragility). A.2b: STATIC (non-instanced, non-vertex-fetch) draws,
// per-object previous worldViewProj matched by order-within-VS-class.
//
// Debug viz: set [Upscaling] ShowMotionVectors=1 in Dust.ini to blit the velocity
// buffer over the final image (grey = still, colour = motion direction/magnitude).
namespace MotionVectors
{
    void SetDevice(ID3D11Device* device);
    void OnResolution(uint32_t width, uint32_t height);

    // Called once per frame at POST_LIGHTING, while this frame's captures are live.
    // Renders the analytic velocity buffer for STATIC geometry.
    void RenderVelocity(ID3D11DeviceContext* ctx);

    // Called at POST_TONEMAP (final image) when the debug viz is enabled: blits the
    // velocity buffer as colour over the currently-bound render target.
    void DebugBlit(ID3D11DeviceContext* ctx);

    ID3D11ShaderResourceView* GetVelocitySRV();
    bool DebugVizEnabled();

    void Shutdown();
}
