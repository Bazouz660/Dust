# Shadow System — Cascaded Shadow Maps

Source: `data/materials/common/shadowFunctions.hlsl`, `deferred_vanilla.hlsl`

## Configuration

| Property        | Value |
|-----------------|-------|
| Format          | R32_FLOAT 4096x4096 (single atlas) + D32_FLOAT depth |
| Cascades        | `SHADOW_MAP_COUNT = 4` (default, supports up to 9) |
| Atlas size      | 4096x4096 (consistent across all 33 captures) |
| Effective/cascade | ~2048x2048 (4 cascades in one atlas) |
| Filtering       | 12-sample PCF with hex pattern (`ENABLE_PCF_HEX12`), jittered rotation |
| Bias            | Fixed + slope-scaled, with surface-normal-aware sample projection |
| Jitter texture  | R32_FLOAT 513x2 (SRV slot 4 in deferred pass) |

## Draw Statistics

| Metric             | Sparse (Ashlands) | Average | Dense (Shark) |
|--------------------|------------------:|--------:|--------------:|
| Total shadow draws |               196 |     579 |         1,051 |
| Tessellated draws  |                ~102 |    ~302 |           548 |
| Standard draws     |                ~94 |    ~277 |           503 |

## Pipeline Position

Shadow map renders **after** GBuffer fill, **before** deferred lighting:
- GBuffer: draws 1407-2583
- Shadow: draws 2590-3640
- Deferred sun: draw 3641+

## Shadow Vertex Shaders (7 unique)

| Address      | Source                | Entry Point      | Draws | Notes |
|--------------|-----------------------|------------------|------:|-------|
| 0x63E6F7B8   | skin.hlsl             | shadow_vs        |   429 | Skeletal shadow caster |
| 0x64287878   | rtwtessellator.hlsl   | tessellator_vs   |   323 | Tessellated (main) |
| 0x63E071B8   | rtwtessellator.hlsl   | tessellator_vs   |   143 | Tessellated variant |
| 0x638507F8   | terrainfp4.hlsl       | terrain_shadow_vs|    65 | Terrain shadow |
| 0x63E08F78   | rtwtessellator.hlsl   | tessellator_vs   |    54 | Tessellated variant |
| 0x63E06E78   | rtwtessellator.hlsl   | tessellator_vs   |    28 | Tessellated variant |
| 0x6162D6F8   | foliage.hlsl          | farm_shadow_vs   |     9 | Farm plant shadows |

Shadow VS are **completely separate** from GBuffer VS (0 shared).

## Shadow Pixel Shaders (4 unique)

| Address      | Source            | Entry Point | Draws | Notes |
|--------------|-------------------|-------------|------:|-------|
| 0x642876F8   | shadowcaster.hlsl | shadow_fs   |   442 | Standard depth write |
| 0x64286A78   | shadowcaster.hlsl | shadow_fs   |   429 | Skinned variant |
| 0x63E08138   | shadowcaster.hlsl | shadow_fs   |   171 | Alpha-test variant |
| 0x64286DB8   | shadowcaster.hlsl | shadow_fs   |     9 | Farm plants |

## Tessellation Pipeline (SM5.0)

52.1% of shadow draws use tessellation via `rtwtessellator.hlsl` (~148 lines).

| Shader | Address      | Usage |
|--------|--------------|-------|
| HS     | 0x66B35DB8   | All 548 tessellated draws |
| DS     | 0x66B366F8   | All 548 tessellated draws |

### Tessellation Details

- Topology: `PATCHLIST_3_CP` (3 control points per patch)
- **Screen-space edge length** tessellation factor
- Displaces vertices for higher shadow resolution near camera
- Standard draws use TRIANGLELIST (438) and TRIANGLESTRIP (65)

## Shadow Caster Features (shadowcaster.hlsl, ~166 lines)

| Feature          | Implementation |
|------------------|----------------|
| `INSTANCED`      | Instance matrix from vertex attributes (TEXCOORD0-2) |
| `TEXCOORDS` + `ENABLE_ALPHA_TEST` | Alpha-tested shadow casting (foliage) |
| Depth bias       | Fixed + slope-scaled + max-slope-clamped |
| RTW warp         | Real-Time Warp distortion for cascade optimization |
| VSM              | Variance Shadow Map support (output moments) |

## Cascade Selection

Cascades selected by screen-space depth (`posSs.z`):
```hlsl
shadowUv = csmTrans[i].xyz + csmScale[i].xyz * posLs;
```
Where `posLs = mul(shadowViewMat, posWs).xyz` transforms world to light space.

## Shadow VS Constant Buffer (slot 0, 64 bytes)

| Register | Example Values                              | Meaning |
|----------|---------------------------------------------|---------|
| c0       | (0.0004, 0.0001, 0.0000, 0.0000)           | Light view row 0 |
| c1       | (0.0001, -0.0002, -0.0001, 0.0000)         | Light view row 1 |
| c2       | (-0.0000, 0.0002, -0.0001, 0.0000)         | Light view row 2 |
| c3       | (-1.4010, -0.6295, 0.2616, 1.0000)         | Light view translation |

## Shadow PS Constant Buffer (slot 0, 16 bytes)

| Register | Example Values                              | Meaning |
|----------|---------------------------------------------|---------|
| c0       | (0.0000, 6.0000, 0.0020, 0.0001)           | Bias params (fixed, slope, max, ??) |

## Quality Issues

1. **Effective resolution**: Single 4096x4096 atlas for 4 cascades = only ~2048x2048 per cascade
2. **No cascade blending**: Hard transitions visible at cascade split boundaries
3. **No PCSS**: Fixed filter radius regardless of blocker distance (no contact-hardening)
4. **Temporal noise**: Jitter rotation can cause flickering
5. **No ESM/EVSM**: Only PCF filtering, no exponential variance methods

## RTW Warp Map (Draws 2586-2589)

Before shadow rendering, 4 draws generate an RTW (Real-Time Warp) map:

| Draw | Source            | Entry Point        | Output RT |
|------|-------------------|--------------------|-----------|
| 2586 | rtwbackward.hlsl  | rtw_backward_fs    | R32_FLOAT 512x512 |
| 2587 | rtwshadows.hlsl   | rtw_compact        | R32_FLOAT 512x2 |
| 2588 | rtwshadows.hlsl   | rtw_blur           | R32_FLOAT 512x2 |
| 2589 | rtwshadows.hlsl   | (variant)          | R32_FLOAT 513x2 |

This warp map optimizes shadow cascade distribution, stored as a 513x2 texture
bound to SRV slot 4 in the deferred lighting pass.

## RTWSM bias when overriding atlas resolution

The vanilla caster adds `min(maxSlopeBias, slopeBias * length(ddx/ddy(depth)))`
to its stored depth. Enlarging the atlas shrinks these per-pixel derivatives,
but RTW tessellation uses a fixed clip-space edge threshold and does not become
finer with the atlas. This reduces the bias covering warp interpolation errors
and can expose more acne at higher resolutions.

Dust patches RTW `shadow_fs` variants to multiply the derivative term by
`max(actualAtlasSize / nativeAtlasSize, 1)` before the existing clamp. Both OM
binding hooks supply the successfully bound replacement's scale at PS b13;
an allocation fallback uses the native scale. The small constant buffer is
reused until the scale changes, rebound after pass changes/ClearState, and
removed when leaving the pass. Caster draws rebind it after OGRE's material
setup, which can overwrite reflected cbuffers. This adds no per-draw uploads
or texture taps.

The fixed bias and maximum slope bias remain unchanged. Downsampling never
reduces vanilla bias, and non-RTW shader variants retain their original
calculation. This addresses bias lost through Dust's atlas enlargement; it
does not remove every source of RTW warp or geometry-related self-shadowing.

## Live CSM / RTWSM switches

OGRE can retain both shadow generations in its texture pool. Dust tracks each
identity independently: creation of a new RTW color atlas no longer discards
a still-live CSM depth atlas. Depth-only pool reuse is recognized from the
confirmed deferred lighting pass's `shadowDepthMap` at t5; an arbitrary pooled
DSV bind remains insufficient evidence for adoption.

Replacement SRVs are used only after the matching caster bind actually used
the replacement in the current frame. Incomplete color/depth replacements
fall back together to the original atlas, including its lighting sample.
The Shadows plugin reads the bound atlas size for filtering, rather than
assuming the requested resolution is already available. Companion depth is
associated with the actual color atlas, even when another generation's depth
is still tracked. Rapid switches may evict the oldest entry unused this frame
instead of leaving the fixed tracking table permanently full.

The ClearState hook drops the previous pass's shadow/viewport state before
OGRE sets up the next pass. This prevents the old mode's scale from being
applied to the new mode's viewport before the new targets are bound.
