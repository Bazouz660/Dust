# Injection Reference — Where to Modify for Each Feature

## Quick Reference Table

| Feature              | Injection Point                          | Shaders to Modify |
|----------------------|------------------------------------------|--------------------|
| SSAO/SSIL            | After deferred sun (draw 3648), before light volumes | New fullscreen PS |
| SSS (subsurface)     | After deferred sun, needs skin mask from GBuffer | `skin.hlsl`, `character.hlsl` + new deferred pass |
| Shadow replacement   | Replace draws 2590-3640 or intercept shadow RT bind | `shadowcaster.hlsl`, `rtwtessellator.hlsl` |
| RT shadows           | After GBuffer (1407-2583), before deferred lighting | New compute/ray dispatch |
| Voxel GI             | Re-render GBuffer geometry into voxel grid before deferred | All GBuffer PS/VS (redirect output) |
| Tessellation         | Intercept GBuffer draws, add HS/DS | `terrain.hlsl`, `objects.hlsl` (add like `rtwtessellator.hlsl`) |
| Material upgrade     | Replace GBuffer PS per source file | `objects.hlsl`, `terrainfp4.hlsl`, `skin.hlsl`, `character.hlsl`, etc. |
| Better water         | Replace water forward pass (draw 3650) | `water.hlsl` |
| Better sky           | Replace sky draws (2584-2585) | `SkyX_Skydome.hlsl`, `SkyX_Clouds.hlsl` |
| Bloom replacement    | Replace bloom chain (draws 3716-3737) | `hdrfp4.hlsl` bloom entry points |
| Tonemapping          | Replace composite (draw 3718) | `hdrfp4.hlsl` Composite_fp4 |
| AA replacement       | Replace FXAA draws (3739-3762) | `FXAA.hlsl` |
| DOF improvement      | Replace DOF CoC + blur (3724-3728) | `DepthBlur.hlsl`, `hdrfp4.hlsl` halfres |
| Fog improvement      | Replace fog passes (3667-3678) | `fog.hlsl` |
| Light model upgrade  | Modify deferred + light volumes | `deferred.hlsl` (main_fs, light_fs) |

---

## SSAO / SSIL

**Injection**: After deferred sun pass (draw 3648), before light volume draws.

**Required inputs** (all available as SRVs at this point):
- GBuffer RT0 (B8G8R8A8_UNORM native) — for albedo (SSIL)
- GBuffer RT1 (B8G8R8A8_UNORM native) — for normals
- GBuffer RT2 (R32_FLOAT native) — for depth
- R16G16B16A16_FLOAT native — GBuffer repack

**Output**: Multiply AO factor into the HDR lighting buffer, or store in a separate
R8_UNORM texture for the light pass to sample.

**Current state**: Dust already injects SSAO at this point (see `DustAPI.h` effect system).

---

## Subsurface Scattering

**Injection**: After deferred sun pass. Requires material identification in GBuffer.

**Challenge**: RT1.A currently encodes emissive/translucency — need to encode SSS profile ID.

**Shaders to modify**:
- `skin.hlsl` — encode SSS profile in GBuffer (character skin)
- `character.hlsl` — encode SSS profile (full character body, hair excluded)
- `deferred.hlsl` — read SSS profile, apply screen-space subsurface blur

**Approach**: Separable Gaussian blur on the diffuse lighting component, profile-driven
kernel widths per the Jimenez method. The stencil buffer (D24_UNORM_S8_UINT) can mark
SSS pixels to skip non-skin regions.

---

## Shadow Replacement

**Injection**: Replace draws 2590-3640 or intercept the shadow RT bind (R32_FLOAT 4096x4096).

**Current pipeline**:
- 7 unique VS, 4 unique PS, 1 HS, 1 DS in shadow pass
- 52% tessellated (PATCHLIST_3_CP)
- Topologies: PATCHLIST_3_CP (548), TRIANGLELIST (438), TRIANGLESTRIP (65)

**Options**:
1. **Better PCF**: Replace `shadowcaster.hlsl` with PCSS (contact-hardening)
2. **VSM/EVSM**: Output moments instead of depth, use variance-based filtering
3. **Cascade blending**: Modify deferred.hlsl to blend between cascade boundaries
4. **Higher resolution**: Increase atlas from 4096 to 8192 (or use separate textures per cascade)

**Constant buffers**: Shadow VS CB0 is 64 bytes (light view matrix + translation).
Shadow PS CB0 is 16 bytes (bias parameters).

---

## RT Shadows (Ray-Traced)

**Injection**: After GBuffer fill (draws 1407-2583), before deferred lighting.

**Required**:
- Build acceleration structure from GBuffer geometry (or maintain persistent BVH)
- Dispatch rays using GBuffer depth + normals as launch points
- Store shadow mask in R8_UNORM texture
- Modify deferred.hlsl to use RT shadow mask instead of CSM

**Note**: Kenshi uses D3D11 (FL11_0), so DXR is not available. Would need to implement
ray tracing in compute shaders (software RT) or require D3D12 interop.

---

## Voxel GI

**Injection**: Before deferred lighting. Re-render GBuffer geometry into a voxel grid.

**Approach**:
1. Allocate 3D texture (R11G11B10_FLOAT or R16G16B16A16_FLOAT)
2. For each GBuffer draw, project geometry into voxel grid (no rasterization — use GS or compute)
3. Inject direct lighting into voxels
4. Cone-trace from screen pixels in deferred pass

**Shaders to modify**: All GBuffer material PS/VS to output voxel data.
This is the most invasive feature — requires intercepting every geometry draw.

---

## Tessellation / Displacement Mapping

**Injection**: Intercept GBuffer draws, inject HS/DS between VS and PS.

**Existing reference**: `rtwtessellator.hlsl` already implements SM5.0 tessellation for shadows.
Reuse the same HS/DS pattern for GBuffer geometry.

**Priority targets**:
- `terrain.hlsl` / `terrainfp4.hlsl` — terrain displacement (highest visual impact)
- `objects.hlsl` — detailed object displacement
- Requires height maps per material

**Topology change**: TRIANGLELIST -> PATCHLIST_3_CP (same as shadow tessellation).

---

## Material Upgrades

**Injection**: Replace GBuffer pixel shaders per source file.

**Per-material targets**:

| Material | Source | Upgrade |
|----------|--------|---------|
| Objects  | `objects.hlsl` | Better normal mapping (detail normals, parallax occlusion) |
| Terrain  | `terrainfp4.hlsl` | Virtual texturing, better biome transitions |
| Skin     | `skin.hlsl` | SSS encoding, better blood system |
| Character | `character.hlsl` | Hair rendering (anisotropic), eye shading |
| Foliage  | `foliage.hlsl` | Translucent leaf shading, better wind |
| Triplanar | `triplanar.hlsl` | Higher-quality blending, detail layers |

---

## Water Improvement

**Injection**: Replace water forward pass (draw 3650).

**Current**: `water.hlsl` — flow maps, rain ripples, scum, PBR, planar reflection.
Forward pass with own lighting (not deferred).

**Upgrades**: Screen-space reflections, caustics, volumetric underwater, wave simulation.

---

## Sky Improvement

**Injection**: Replace sky draws (2584-2585).

**Current**: `SkyX_Skydome.hlsl` — Rayleigh/Mie scattering, starfield.

**Upgrades**: Volumetric clouds (replace `SkyX_Clouds.hlsl`), better atmospheric scattering,
physically-based sky model (Hillaire).

---

## Bloom Replacement

**Injection**: Replace draws 3716-3717 (extract + initial blur) and 3729-3737 (pyramid).

**Current**: 5 downsample + 4 upsample with `bloom_ps` and `BloomBlurV_fp4`.

**Upgrades**: Dual Kawase blur, energy-conserving bloom, spectral bloom.

---

## Tonemapping

**Injection**: Replace draw 3718 (`Composite_fp4`).

**Current**: `hdrfp4.hlsl` — converts R11G11B10_FLOAT HDR to B8G8R8A8_UNORM LDR.

**Upgrades**: ACES, AgX, Tony McMapface, GT tonemapper. Input: HDR scene + adapted luminance (R32_FLOAT 1x1) + bloom.

---

## BRDF Upgrade

**Injection**: Modify `deferred.hlsl` (main_fs for sun, light_fs for point/spot).

**Current**: GGX specular (Kelemen-SzK visibility) + Lambert diffuse with Fresnel energy conservation.

**Upgrades**:
1. Uncomment Disney/Burley diffuse (already in lightingFunctions.hlsl)
2. Smith-GGX height-correlated visibility (replace Kelemen-SzK)
3. Kulla-Conty multi-scatter energy compensation
4. Better IBL (replace Lazarov with Split-Sum)
5. Physical inverse-square light falloff (replace custom cubic attenuation)

---

## Camera Data (DustCameraData)

The framework extracts camera data from the game's deferred lighting constant buffer
at `POST_LIGHTING` and provides it to all effects via `DustFrameContext::camera`.

### Available fields

| Field | Type | Description |
|-------|------|-------------|
| `valid` | `int` | Non-zero once camera data has been successfully extracted |
| `camRight[3]` | `float` | View X axis (right) in world space |
| `camUp[3]` | `float` | View Y axis (up) in world space |
| `camForward[3]` | `float` | View Z axis in world space — **OGRE right-handed convention: points behind camera** |
| `camPosition[3]` | `float` | Camera position in world space |
| `inverseView[16]` | `float` | Raw 4×4 inverse view matrix (row-major in memory) |

### Coordinate systems

**OGRE view space** (right-handed): X=right, Y=up, Z=behind camera.

**Depth-reconstruction view space** (left-handed): X=right, Y=up, Z=depth (into scene).
This is the convention used by screen-space effects (SSAO, SSIL, RTGI).

### World-to-view normal transform

To convert GBuffer world-space normals to the left-handed view space used by screen-space
ray tracing and depth reconstruction:

```hlsl
float3 worldN = normalize(normalsTex.Sample(samp, uv).rgb * 2.0 - 1.0);
float3 viewN;
viewN.x =  dot(worldN, camRight.xyz);
viewN.y =  dot(worldN, camUp.xyz);
viewN.z = -dot(worldN, camForward.xyz);  // negate: OGRE Z behind camera → our Z into scene
viewN = normalize(viewN);
```

### How the basis vectors are extracted

The camera axes are the **columns** of the inverse view matrix (equivalently, the rows
of the view matrix). The framework reads the raw matrix from the game's PS CB at register
c8 (offset 128 bytes) and extracts columns:

```
camRight   = column 0 = (m[0], m[4], m[8])
camUp      = column 1 = (m[1], m[5], m[9])
camForward = column 2 = (m[2], m[6], m[10])
camPosition = row 3   = (m[12], m[13], m[14])
```

### Availability

Camera data is extracted once per frame at the `POST_LIGHTING` injection point. Effects
at `POST_LIGHTING` and later (`POST_FOG`, `POST_TONEMAP`, `PRE_PRESENT`) will have
`camera.valid == 1`. Effects at `POST_GBUFFER` will see `camera.valid == 0` on the first
frame, but will have the previous frame's data from the second frame onward (the data
persists across frames).

---

## Key Constant Buffers for Implementation

### Deferred Lighting PS CB0 (352 bytes)

Key registers for feature implementation:
- c0: `sunDirection` (xyz)
- c1: `sunColour` (rgb) + intensity
- c6: Resolution (w, h, 1/w, 1/h)
- c8-c11: `inverseView` matrix (world reconstruction)

### Deferred Lighting VS CB0 (32 bytes)

- c0: Far frustum corner 1
- c1: Far frustum corner 2 + near plane distance

### GBuffer VS CB0 (160 bytes)

- c0: Per-object material params
- c1-c4: viewProjection matrix
- c5: Camera world position
- c6-c9: Per-object data (material color, etc.)

### Shadow VS CB0 (64 bytes)

- c0-c2: Light view matrix (3x3)
- c3: Light view translation + w=1

### Shadow PS CB0 (16 bytes)

- c0: Bias parameters (fixed, slope, max, reserved)
