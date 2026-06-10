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

    // Characters (skinned draws): enable/disable replaying them into the cube, and how many
    // were replayed this frame (reset in BeginFrame). Used for the skin diagnostic + dump.
    void SetSkinEnabled(bool on);
    int  GetLastSkinReplayed();

    void Shutdown();
}
