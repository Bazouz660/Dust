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

    // --- Injected per-object velocity (shader-patched GBuffer SV_Target3 output) ---
    // The robust replacement for the replay pass above. The velocity RT is appended to the live
    // GBuffer bind, and the game's own (patched) shaders write velocity into it.
    //
    // Create/resize the velocity RT to match the GBuffer color target; returns the RTV to bind as
    // the 4th GBuffer render target (null if not ready).
    ID3D11RenderTargetView* InjEnsureVelRTV(ID3D11RenderTargetView* refRTV);
    // The b13 constant buffer holding the camera reprojection (prevVP*inv(curVP)) for the patched VS.
    ID3D11Buffer* GetInjReprojCB();
    // Once per frame, at the start of the GBuffer pass: recompute reproj + upload it, and clear the
    // velocity RT. Safe to call multiple times per frame (only the first does work).
    void InjBeginGBuffer(ID3D11DeviceContext* ctx);
    // Reset the once-per-frame guard (call at frame end / POST_TONEMAP).
    void InjEndFrame();
    // Blit the injected velocity RT as colour over the bound target (debug viz).
    void InjDebugBlit(ID3D11DeviceContext* ctx);

    void Shutdown();
}
