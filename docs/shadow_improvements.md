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

## Implemented: Improved RTWSM Shadow Filtering

The deferred lighting shader is patched at D3DCompile time (via `PatchDeferredShader` in `D3D11Hook.cpp`) to replace the vanilla `RTWShadow()` call with `DustRTWShadow()`:

**Vanilla problems fixed:**
- PCF texel size was `0.0001` — for a 2048 shadow map (texel ≈ 0.000488), the 3x3 grid covered less than one texel. Essentially a single sample.
- Regular grid pattern visible as blocky shadow edges.
- No per-pixel variation — identical artifact patterns across the screen.

**DustRTWShadow improvements:**
- **12-sample Poisson disk** — well-distributed samples vs. 3×3 grid (9 samples).
- **Per-pixel rotation** via interleaved gradient noise — eliminates banding, looks smooth.
- **Correct filter radius** — `0.0012` in pre-warp UV (covers ~2.5 texels) vs. `0.0001` (< 1 texel).
- **PCSS blocker search** (12-sample) — estimates distance to occluder, produces variable penumbra. Contact shadows are sharp, distant shadows are soft. Toggled via `DUST_SHADOW_PCSS` define (default on).
- **Compilation fallback** — if the patched shader fails, falls back to vanilla automatically.

**Tunable constants** (in the injected shader source):
- `fr` (filterRadius): base PCF filter size in pre-warp UV. Default `0.0012`.
- `ls` (lightSize): virtual light size for PCSS search radius. Default `0.004`.
- `DUST_SHADOW_PCSS`: set to `0` to disable PCSS and use fixed-radius PCF only.

**Performance:** ~48 texture reads/pixel (PCSS on) vs. 18 (vanilla). ~2.7× shadow sampling cost, acceptable for a graphics mod.

## Remaining Future Work

- **Shadow map resolution override** — intercept `CreateTexture2D` to force higher-res shadow maps.
- **Runtime parameter tuning** — add constant buffer for live adjustment of filter radius / light size via GUI.
