# Legacy SSAO Notes

Snapshot of the legacy AO implementation, in case we need to roll back or
reference design choices. The shader files for this pipeline are preserved at
git commit `1fa5bcd` (ADD: CSM shadows PCSS, cascade blending, optimisation).

## Algorithm

**GTAO** — Ground Truth Ambient Occlusion (Jimenez et al. 2016) variant.

Per pixel:
1. Sample center depth, early-out if `depth > aoMaxDepth`.
2. Reconstruct view-space position from `(uv, depth)` using `uvScale = (asp*tanHF, tanHF)`.
3. Reconstruct geometric normal from depth derivatives via **smaller-side picker**:
   compare `|dL - depth|` vs `|dR - depth|`, use the closer side for `ddx`,
   same for `ddy`. Cross product gives geo-normal. Sign-corrected via dot with
   G-buffer normal, then blended toward G-buffer via `normalDetail`.
4. For each direction (4–12):
   - Rotate `direction = (cos θ, sin θ)` by per-pixel blue-noise jitter.
   - For each step (2–6):
     - Sample at `uv + offset` and `uv - offset` (two sides per step).
     - Compute `cosH = dot(diff_normalized, normal)`.
     - Apply `falloff = pow(1 - dist/radius, falloffPower)`.
     - Apply `fgAtten = exp(-(depth - sDepth)/depth * foregroundFade)` to suppress
       halos from foreground samples.
     - `cosH *= falloff * fgAtten`.
     - Track `maxCosPos`/`maxCosNeg` per direction.
   - `ao += saturate(maxCosPos) + saturate(maxCosNeg)`.
5. `ao = (ao / numDirections / 2) * aoStrength`, then `ao = saturate(1 - ao)`.
6. Depth-fade between `depthFadeStart` and `aoMaxDepth`.

## Pipeline

3 fullscreen passes, R8_UNORM intermediate textures:

1. `ssao_gen_ps.hlsl` → write raw AO to `gAoTex`.
2. `ssao_blur_h_ps.hlsl` → gather4 guided filter (positive-shifted 4x4 window) → `gAoBlurTex`.
3. `ssao_blur_v_ps.hlsl` → gather4 guided filter (negative-shifted 4x4 window) → `gAoTex`.

Final `gAoSRV` is bound to register `t8` for Kenshi's deferred shader to multiply
with ambient diffuse.

## Sliders

| Slider | Range | Default |
|--------|-------|---------|
| Radius | 0.0005–0.01 | 0.003 |
| Strength | 0.5–10 | 2.537 |
| Bias | 0–0.2 | 0.001 |
| Max Depth | 0.01–1 | 0.1 |
| Filter Radius | 0.01–1 | 0.15 |
| Foreground Fade | 1–200 | 26.644 |
| Falloff Power | 0.5–5 | 2.0 |
| Max Screen Radius | 0.005–0.2 | 0.03 |
| Min Screen Radius | 0.0001–0.01 | 0.001 |
| Blur Sharpness | 0–0.1 | 0.01 |
| Normal Detail | 0–1 | 0.5 |
| Direct Light AO | 0–1 | 0.3 |
| Samples (directions) | 4–12 | 4 |
| Steps (per direction) | 2–6 | 4 |
| Tan Half FOV (hidden) | 0.1–2 | 0.5218 |

## Files

- `shaders/ssao_gen_ps.hlsl` — main AO pass
- `shaders/ssao_blur_h_ps.hlsl` — horizontal-equivalent gather filter
- `shaders/ssao_blur_v_ps.hlsl` — vertical-equivalent gather filter
- `shaders/ssao_apply_ps.hlsl` — unused (deferred shader applies via slot 8)
- `shaders/ssao_upsample_ps.hlsl` — half-res upsample (renderer doesn't currently route through this)
- `shaders/ssao_debug_ps.hlsl` — raw-AO visualization overlay
- `SSAORenderer.cpp` — 3-pass orchestration, half-res infrastructure present but unused
- `SSAOConfig.h` — settings struct
- `DustSSAO.cpp` — plugin entry, settings array, AO/params binding to deferred slot 8/9
