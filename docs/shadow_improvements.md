# Kenshi Shadow Improvements Analysis

## Why Kenshi Shadows Look Bad

Kenshi uses RTWSM (Resolution Tree Warped Shadow Maps), which warps a single shadow map to allocate more texel resolution near the camera. This has fundamental trade-offs:

### Distance vs. Near Quality
The shadow map is a fixed-resolution texture (e.g. 2048x2048). Increasing shadow distance means the same texels cover a larger world area. RTWSM warps resolution toward the camera, but stretching the far range starves the near range, causing blocky/aliased shadows up close.

### Shadow Acne
A depth bias problem. The shadow map stores depths at discrete texel centers. When a surface's actual depth is close to the stored depth, floating-point precision errors cause pixels to incorrectly self-shadow. RTWSM makes this worse because non-uniform texel sizes mean a single bias value can't work everywhere. Too little bias = acne, too much bias = peter-panning (shadows detach from objects).

### Flickering / Temporal Instability
RTWSM recomputes its warping tree every frame based on the current view. Small camera movements cause the warp distribution to shift, meaning shadow map texels snap to different world positions frame-to-frame. This causes edges to shimmer and flicker. Traditional CSM can use stable cascades snapped to texel grids — RTWSM's adaptive warping makes stable snapping much harder.

### Warp Artifacts
The warp function can create discontinuities or extreme compression ratios in certain scenes (e.g. large flat terrain with a few vertical objects), leading to visible seams or sudden quality drops.

---

## What We CAN Fix (D3D11 Hook)

### Better Shadow Filtering
Replace the shadow sampling in the lighting shader with higher-quality PCF, Poisson disk, or PCSS (percentage-closer soft shadows). Smooths blocky edges and hides some acne.

### Bias Adjustment
Tweak the depth bias in the shadow sampling to reduce acne (though can't make it per-cascade since there are no cascades).

### Screen-Space Contact Shadows (Implemented)
~~Add a post-process pass that ray-marches in screen space along the light direction using the depth buffer. Adds sharp, detailed close-range shadows that mask the low-res shadow map quality up close.~~ **Implemented as the SSS (Screen Space Shadows) effect** in `effects/sss/`. Runs at `POST_LIGHTING` (priority 20), extracts the sun direction from the game's constant buffer each frame, ray marches the depth buffer toward the sun with quadratic step distribution and per-pixel jitter, applies bilateral blur, and composites multiplicatively onto the HDR target.

### Shadow Map Resolution Override
Intercept the shadow map render target creation via D3D11 and force a higher resolution texture. Directly improves quality at the cost of GPU performance.

---

## What We CAN'T Fix

### Replace RTWSM with CSM
Would require intercepting the entire shadow rendering pass, changing how the engine sets up light matrices, and managing multiple cascade splits. That's deep engine logic, not just shader replacement.

### Temporal Instability
The flickering comes from the warp tree recomputation on the CPU side, before anything hits the GPU. No way to stabilize the texel grid from a D3D11 hook.

---

## Recommended Approach

**Screen-space contact shadows + better filtering + shadow map resolution bump.** This combo would meaningfully improve the close-range experience without touching the engine's shadow architecture.

Screen-space contact shadows are now implemented — see `effects/sss/`. Better filtering and shadow map resolution override remain as future work.
