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
    void SetDebugViz(bool on);   // runtime toggle (GUI)

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

    // Runtime disable of the injected-MV feeder (upscaler + debug viz both off): free the velocity RT —
    // the only large allocation — and drop cross-frame match state so a later re-enable starts clean.
    // InjEnsureVelRTV reallocates the RT lazily when the feeder turns back on.
    void ReleaseInjectionTargets();

    // True skinned-character animation MVs: the patched skin VS skins each vertex with current AND
    // previous-frame bones (b12). InjBindSkinPrev identifies a skinned draw directly, binds the matched
    // previous pose to b12 before the draw, and records this draw's pose from the CB shadow;
    // InjFillSkinPrev promotes the recorded poses to next frame's match targets.
    void InjFillSkinPrev(ID3D11DeviceContext* ctx);
    void InjBindSkinPrev(ID3D11DeviceContext* ctx, UINT indexCount, INT baseVertexLocation);

    // DrawIndexedInstanced hook: instanced draws never run the pose matcher (per-instance bone
    // palettes make cross-frame identity ambiguous), but an instanced SKINNED draw's patched VS
    // still reads b12 — left sticky, that's the previous draw's pose (a wrong-character MV flash).
    // Bind the zero CB so it takes the safe camera-reproj fallback instead. No-op unless the
    // currently-bound vertex shader is skinned.
    void InjBindZeroB12(ID3D11DeviceContext* ctx);

    // A.5 rigid moving-object velocity (weapons/tools/props on animated bones, objects.hlsl). Per-draw
    // previous WVP bound at b12, spatially matched cross-frame; camera-reproj fallback on any miss.
    // InjBindRigidPrev = per STATIC GBuffer draw (after GeometryCapture::OnDrawIndexed); InjFillRigidPrev
    // = POST_LIGHTING (promote this frame's draws to next frame's match targets).
    void InjBindRigidPrev(ID3D11DeviceContext* ctx);
    void InjFillRigidPrev(ID3D11DeviceContext* ctx);

    // Bone-palette CB shadow, fed from the Map/Unmap hooks. For known WRITE_DISCARD buffers the hook
    // gives the game cached RAM, then copies that RAM to the real mapped GPU pointer at Unmap. This
    // preserves the current root pose without ever reading slow write-combined upload memory.
    void InjNoteMap(void* resource, D3D11_MAP mapType, D3D11_MAPPED_SUBRESOURCE* mapped);
    void InjNoteUnmap(void* resource);
    // Blit the injected velocity RT as colour over the bound target (debug viz).
    void InjDebugBlit(ID3D11DeviceContext* ctx);

    // The injected velocity texture as a plain resource, for handing to the upscaler (DLSS/FSR) as
    // its motion-vector input. Null until the RT has been created. Also expose its dimensions.
    ID3D11Resource* GetInjVelResource();

    // Contrast-limited sharpen of srcSRV into dstRTV (display res), for the DLSS output. amount in [0,1].
    void SharpenBlit(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srcSRV,
                     ID3D11RenderTargetView* dstRTV, float amount, uint32_t w, uint32_t h);

    // Fill camera-only MV into the velocity buffer for sky / far-depth pixels, so the upscaler can
    // reproject the sky under camera motion (kills sky ghosting + object/sky edge shimmer). Call at
    // POST_TONEMAP after the scene is rendered, just before the upscaler reads the velocity buffer.
    void InjFillSkyMV(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* depthSRV, uint32_t w, uint32_t h);

    // Convert the game's LINEAR depth (Euclidean ray distance / farClip) into the HYPERBOLIC device-Z
    // the upscalers expect. Fed linear depth, FSR2's near/far un-projection reconstructs a wrong view-Z
    // that is hypersensitive at distance -> per-frame history rejection -> shimmer/jitter on far
    // geometry. Returns the converted R32F texture (owned by this module), or null to fall back.
    ID3D11Resource* ConvertDepthForUpscaler(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* depthSRV,
                                            uint32_t w, uint32_t h,
                                            float nearZ, float farZ, float fovYRadians);

    void Shutdown();
}
