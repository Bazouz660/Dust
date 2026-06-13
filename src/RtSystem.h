#pragma once

// RtSystem — frame orchestration for the DustRT path-traced lighting sidecar.
//
// Per frame, both hooks fire around the deferred lighting (main_fs) fullscreen
// draw — the moment PipelineDetector reports POST_LIGHTING (the GBuffer is
// complete and its SRVs were just captured into the registry):
//   OnLightingPre (before the lighting draw): copy depth/normals into shared
//     textures, signal the shared fence, submit the D3D12 frame (BLAS/TLAS
//     maintenance, sun shadow + path-traced GI trace, temporal accumulation,
//     a-trous filter). All sync is GPU-side; the D3D11 timeline never
//     CPU-blocks, and the trace overlaps the lighting draw + point-shadow work.
//   OnLightingPost (after the lighting draw, before the additive light
//     volumes): queue a GPU wait for the trace, additively composite GI
//     (albedo * irradiance) into the currently-bound HDR target, draw debug
//     overlays, then harvest this frame's captures for the next frame's TLAS.
//
// Fail-closed: any init failure disables the system permanently (status shown
// in the GUI); every entry point no-ops when disabled.

#include <d3d11.h>
#include <cstdint>

namespace RtSystem
{
    struct Settings
    {
        bool  enabled            = true;
        bool  sunShadows         = true;
        bool  gi                 = true;
        int   giRaysPerPixel     = 1;
        int   giBounces          = 1;     // 1 = single bounce + sky; up to 3
        int   shadowRaysPerPixel = 2;
        float sunAngularRadiusDeg = 0.8f; // visually-soft default; 0.27 = physical
        float giIntensity        = 1.0f;
        float ambientReplace     = 0.85f;  // fraction of the game's flat IBL ambient replaced by RT GI
        float sunShadowStrength  = 1.0f;   // RT sun-visibility multiply on the sun term
        float vanillaShadowRemove = 0.0f;  // 1 = drop the game's RTW/CSM sun shadow (RT becomes the only
                                           // sun shadow — terrain/characters stop casting until they're in the TLAS)
        float bounceAlbedo       = 0.55f;
        float maxRayDist         = 3000.0f;
        float normalBias         = 0.05f;
        float temporalAlpha      = 0.08f; // EMA weight of the CURRENT frame
        int   atrousIterations   = 3;     // 1..4
        float sigmaDepth         = 0.03f;
        float sigmaNormal        = 32.0f;
        float sunColor[3]        = { 1.0f, 0.95f, 0.85f };
        float sunIntensity       = 3.0f;
        float skyHorizon[3]      = { 0.50f, 0.58f, 0.70f };
        float skyZenith[3]       = { 0.16f, 0.30f, 0.55f };
        float skyIntensity       = 1.0f;
        bool  flipSunDir         = false; // CSM sunDirection sign is convention-checked in game
        int   debugMode          = 0;     // 0=off 1=clay TLAS view 2=sun shadow mask 3=GI radiance
    };
    Settings& GetSettings();

    void Shutdown();
    bool IsActive();                 // init succeeded and master toggle on
    const char* StatusString();

    void OnLightingPre(ID3D11DeviceContext* ctx, ID3D11Device* device,
                       ID3D11ShaderResourceView* depthSRV,
                       ID3D11ShaderResourceView* normalsSRV,
                       uint32_t width, uint32_t height, uint64_t frameIndex);

    // Immediately before the lighting draw (after effect DispatchPre): queues
    // the GPU wait for this frame's trace and binds t20/b9 for the patched
    // main_fs. Binds zeroed params (vanilla lighting) when inactive.
    void BindForLightingDraw(ID3D11DeviceContext* ctx);

    // Composites into whatever RTV is bound (the HDR scene target at this
    // point in the frame).
    void OnLightingPost(ID3D11DeviceContext* ctx, ID3D11Device* device,
                        ID3D11ShaderResourceView* albedoSRV,
                        uint32_t width, uint32_t height);

    // GUI helpers
    void RequestFlush();             // drop + rebuild all captured geometry
    void RequestDump();              // write the offline-replay snapshot (rt_dump/)
    struct FrameStats
    {
        uint32_t instances = 0;
        uint32_t meshes = 0;
        uint32_t blasBuilt = 0;
        uint32_t pendingPairs = 0;
        uint32_t skippedInstanced = 0;
        uint32_t parseFailures = 0;
        uint64_t poolVerts = 0;
        uint64_t poolIdx = 0;
        bool     traced = false;
        bool     sunValid = false;
        int      sunSource = 0;    // 0 = fallback, 1 = CSM snapshot, 2 = scene light
        bool     mainFsPatched = false;
        float    tlasAlignPct = -1.0f;  // % of GBuffer pixels the TLAS matches (clay mode only)
    };
    FrameStats GetFrameStats();
}
