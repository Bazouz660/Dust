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
    // replacementVPSkin: the same View*Projection but built from the light's REBASED position
    // (sActivePos, no worldOffset). SKINNED draws' bone matrices are in the rebased frame, not
    // the absolute frame static geometry uses, so they need their own VP. Directions/distances
    // are translation-invariant so both project into the same cube consistently. If null, the
    // static VP is used for skin too (legacy).
    uint32_t Replay(ID3D11DeviceContext* ctx, ID3D11Device* device,
                    const float* replacementVP,
                    const float* replacementVPSkin = nullptr,
                    const float* cullCenter = nullptr, float cullRadius = 0.0f);

    // Compute per-draw placement decisions for this frame's captures (majority-vote
    // cameraVP, then PRE-TRANSFORMED / COMPOSE-world / SKIP per draw). Call once per
    // frame before the Replay() face loop — Replay() consults the cached decisions.
    void BeginFrame(ID3D11DeviceContext* ctx);

    // Read-only view of this frame's placement cache (consumed by RtScene for
    // TLAS instancing). Ensures the cache is fresh (rebuilds if stale), so it
    // works whether or not the point-shadow path already ran this frame.
    // Pointers are valid until the next BuildCache/ResetFrame.
    struct ReplayDrawInfo
    {
        uint32_t drawIndex;    // index into GeometryCapture::GetCaptures()
        int      category;     // DustShaderCategory
        int      placement;    // 0 = PRE-TRANSFORMED (world identity), 1 = WORLD-PLACED
        float    worldM[16];   // row-major row-vector world matrix (placement == 1)
    };
    bool GetFrameCache(ID3D11DeviceContext* ctx, const ReplayDrawInfo** outInfos,
                       uint32_t* outCount);

    // The coherent camera view-proj chosen by the placement cache (row-vector
    // convention, same space as the captured GBuffer clip matrices). This is
    // the authoritative matrix for reconstructing the VB-space camera.
    bool GetCameraVP(float out[16]);

    // Debug (RT snapshot): raw clip matrices + placement flags from the cache.
    uint32_t CopyDebugClips(float* outMats, int* outPlacement, uint32_t maxMats);

    void Shutdown();
}
