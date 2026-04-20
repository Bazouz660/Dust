# Kenshi Rendering Pipeline — Verified Analysis

Verified via Dust Pipeline Survey system across 33 captures, 32 locations, day/night.
Date: 2026-04-20

> This document describes the raw pipeline as confirmed by the survey. For the Dust framework's
> injection point system and plugin API built on top of this analysis, see [`src/DustAPI.h`](../src/DustAPI.h).

## Overview

- **Graphics API**: Direct3D 11, Feature Level 11_0
- **Engine**: OGRE 2.0 (Object-Oriented Graphics Rendering Engine)
- **Rendering**: Deferred shading + forward light volumes + forward transparent
- **Resolution**: Native (tested at 2560x1440)
- **Depth format**: D24_UNORM_S8_UINT (hardware), R32_FLOAT (linear, in GBuffer)
- **Shadow format**: R32_FLOAT 4096x4096, D32_FLOAT depth
- **HDR format**: R11G11B10_FLOAT
- **BRDF**: GGX specular + Lambert diffuse with Fresnel energy conservation

## Frame Structure

Verified pipeline order (draw indices from Shark, 3763 draws, every shader identified):

```
[    0-    6]  Heat haze (prev frame)       heathaze.hlsl              R8G8B8A8_UNORM native + depth
[    7- 1406]  Cubemap / reflection probe   (same shaders as GBuffer)  B8G8R8A8_UNORM 512x512 + D32_FLOAT
[ 1407- 2583]  GBuffer fill (geometry)      objects/terrain/skin/etc   3 MRT + D24_UNORM_S8_UINT
[ 2584- 2585]  Sky rendering                SkyX_Skydome/Clouds.hlsl   R11G11B10_FLOAT native + depth
[ 2586- 2589]  RTW warp map generation      rtwshadows/backward.hlsl   R32_FLOAT 512x512 -> 513x2
[ 2590- 3640]  Shadow map (CSM)             shadowcaster.hlsl + tess   R32_FLOAT 4096x4096 + D32_FLOAT
[ 3641]        Deferred pre-pass            deferred.hlsl (fullscreen) R11G11B10_FLOAT native
[ 3642- 3644]  Stencil mask                 stencil_mask.hlsl          R8_UNORM native
[ 3645- 3647]  GBuffer repack               gbuffer_repack.hlsl        R16G16B16A16_FLOAT native
[ 3648]        Deferred lighting (sun)      deferred.hlsl main_fs      R11G11B10_FLOAT native
[ 3649]        Deferred light volume (1)    deferred.hlsl light_fs     R11G11B10_FLOAT native
[ 3650]        Water surface                water.hlsl                 R11G11B10_FLOAT native + depth
[ 3651- 3666]  Water surface decoration     objects.hlsl + water VS    R11G11B10_FLOAT native + depth
[ 3667]        Atmosphere fog (fullscreen)  fog.hlsl atmosphere_fog    R11G11B10_FLOAT native + depth
[ 3668- 3678]  Fog volumes (11 planes)      fog.hlsl fog_planes        R11G11B10_FLOAT native + depth
[ 3679- 3708]  Forward basic geometry       basic.hlsl                 R11G11B10_FLOAT native + depth
[ 3709- 3715]  Luminance chain              hdrfp4.hlsl                R32_FLOAT 128 -> 64 -> 16 -> 4 -> 1
[ 3716- 3717]  Bloom extract + blur         hdrfp4.hlsl                R11G11B10_FLOAT 640x360
[ 3718]        Tonemapping / composite      hdrfp4.hlsl Composite      B8G8R8A8_UNORM native
[ 3719]        Luminance adapt (next frame) hdrfp4.hlsl AdaptLum       R32_FLOAT 256x256
[ 3720- 3723]  LDR post-processing (4x)    post_ldr.hlsl              B8G8R8A8_UNORM native
[ 3724]        DOF CoC calculation          DepthBlur.hlsl             R16_FLOAT native
[ 3725- 3727]  Half-res blur (3 passes)     hdrfp4.hlsl                B8G8R8A8_UNORM 1280x720
[ 3728]        LDR composite               post_ldr.hlsl              B8G8R8A8_UNORM native
[ 3729- 3737]  Bloom (down 5 + up 4)       hdrfp4.hlsl                R11G11B10_FLOAT 1280->80->1280
[ 3738]        LDR final composite         post_ldr.hlsl              B8G8R8A8_UNORM native
[ 3739]        FXAA                         FXAA.hlsl                  B8G8R8A8_UNORM native + depth
[ 3740]        Heat haze                    heathaze.hlsl              R8G8B8A8_UNORM native + depth
[ 3741- 3762]  FXAA passes (22 draws)       FXAA.hlsl                  R8G8B8A8_UNORM native + depth
```

Draw counts vary by scene complexity:
- Sparse (Ashlands): ~582 total, 238 GBuffer, 196 shadow
- Dense (Shark): ~3763 total, 1177 GBuffer, 1051 shadow
- Average across 33 captures: 1545 total, 545 GBuffer, 579 shadow

Pass types present in ALL captures: GBUFFER_FILL, SHADOW_MAP, HDR_PASS, STENCIL_MASK,
HDR_TEMP, LDR_PASS, LUMINANCE_CHAIN, BLOOM_CHAIN, DOF_PASS, HALF_RES_POST, FINAL_OUTPUT.
Cubemap pass only present in some captures (depends on probe proximity).

---

## GBuffer Layout

Three render targets + hardware depth, all at native resolution.

| MRT Slot | Format            | Content                                            |
|----------|-------------------|----------------------------------------------------|
| RT0      | B8G8R8A8_UNORM    | R=Luma(Y), G=Chroma(Co/Cg checkerboard), B=Metalness, A=Gloss |
| RT1      | B8G8R8A8_UNORM    | RGB=Normal (encoded: n*0.5+0.5), A=Emissive/Translucency |
| RT2      | R32_FLOAT         | Linear depth (distance / farClip)                  |
| DS       | D24_UNORM_S8_UINT | Hardware depth + stencil                           |

### Albedo Encoding: YCoCg Chroma Subsampling

Kenshi uses YCoCg color space with checkerboard chroma subsampling to pack metalness
into the GBuffer without an extra render target:

- RGB albedo is converted to YCoCg: Y (luma) stored at full resolution in R
- Co and Cg chrominance stored at HALF resolution using a checkerboard pattern in G
- On even pixels (x%2 == y%2), G stores Cg; on odd pixels, G stores Co
- Metalness gets the freed B channel at full precision (8-bit)
- Gloss gets the A channel at full precision (8-bit)

Decoding in the deferred pass uses edge-directed chroma reconstruction (`decodePixel`
in `gbuffer.hlsl`) that samples 4 neighbors and uses luminance-weighted averaging to
reconstruct the missing chrominance channel.

Source: `data/materials/deferred/gbuffer.hlsl`

### Emissive / Translucency Encoding (RT1.A)

RT1.A dual-encodes two values:
- Values 0.0-0.5: translucency = value * 2.0 (range 0-1)
- Values 0.5-1.0: emissive = (value - 0.5) * 6.4 (range 0-3.2)

### GBuffer Write Functions

```hlsl
writeAlbedo(buffer, rgb, screenPos)    // -> RT0.RG (YCoCg encoded, checkerboard)
writeMetalness(buffer, value)          // -> RT0.B
writeGloss(buffer, value)             // -> RT0.A
writeNormal(buffer, normal)           // -> RT1.RGB (normal * 0.5 + 0.5)
writeDepth(buffer, dist/farClip)      // -> RT2.R
```

### Material Data Flow (example: skin.hlsl)

- Metalness: `colorMap.b` (blue channel of the color/mask texture)
- Gloss: `diffuseTexture.a * glossMult` (alpha channel of diffuse texture, scaled by uniform)

GBuffer draws: 176-1388 per frame (avg 545). 34 unique PS, 16 unique VS in densest capture.
91 unique GBuffer PS observed across all 33 captures. Vertex strides: 24 (most common), 40, 56, 60, 64 bytes.

---

## Material System — All Shader Sources Identified

Every GBuffer material shader is now mapped to its source file. 50 HLSL source files, 458 unique
shader permutations captured across 33 survey captures. 0 unidentified.

### GBuffer Material Breakdown (Shark capture, 1177 draws)

| Material | Source File | Draws | % | Unique PS | Unique VS | Notes |
|----------|------------|------:|--:|----------:|----------:|-------|
| Objects | `objects.hlsl` | 455 | 38.7% | 19 | 8 | Static/instanced meshes, foliage threshold, dual-texture, dust, wetness |
| Skinned | `skin.hlsl` | 292 | 24.8% | 1 | 1 | Hardware skinning (60 bones), blood, part masking, wetness |
| Foliage | `foliage.hlsl` | 137 | 11.6% | 1 | 1 | Billboard grass (grass_fs), wind animation, distance fade, no normal map |
| Characters | `character.hlsl` | 131 | 11.1% | 2 | 2 | Full body (main_fs) + hair (hair_fs), 13 texture inputs, skin tone |
| Terrain | `terrainfp4.hlsl` + `terrain.hlsl` | 120 | 10.2% | 8+1 | 3 | Texture2DArray, 6 layers/biome, up to 4 biome blends, triplanar cliffs |
| Creatures | `creature.hlsl` | 36 | 3.1% | 1 | 1 | NPC/animal rendering |
| Triplanar | `triplanar.hlsl` | 3 | 0.3% | 1 | 1 | Rocks/cliffs, world-space UV / 5000, axis-weighted blending |
| Distant towns | `distant_town.hlsl` | 3 | 0.3% | 1 | 1 | Settlement LOD rendering |

### Material Shader Details

**objects.hlsl** (91 permutations across all captures, most complex material):
Entry points: `main_vs` / `main_ps`. Compile variants via `#ifdef`:
`INSTANCED`, `CONSTRUCTION`, `COLOURING`, `DUAL_TEXTURE`, `DOUBLESIDED`, `DXT5NORMAL`,
`CLIP_INTERIOR`, `DUST`, `TRANSPARENCY`, `EMISSIVE`, `INTERIOR`.
Features: Bayer 4x4 alpha dither (StabilizeFoliageThreshold), dust accumulation layer,
wetness from water line, construction scaffold blend, DXT5 two-channel normal decode.
Textures: diffuseMap(s0), normalMap(s1), colorMap(s2), optional dustMap, secondaryDiffuse.

**terrainfp4.hlsl** (44 permutations):
Entry points: `main_fs` (full biome blending), `mapfeature_fs` (simplified), `simple_fs`.
Uses `Texture2DArray` for layers. `computeBiome()` handles per-biome sampling with triplanar
cliff projection, overlay color maps, distance fade, height-blend. `BLEND1`-`BLEND3` for
multi-biome transitions. Vertex stride 40 bytes, mostly TRIANGLESTRIP.

**skin.hlsl** (16 permutations):
Entry points: `main_vs`/`main_fs`, `shadow_vs`. 60-bone hardware skinning with 3 blend weights.
Blood system: atan2 cylinder projection, `bloodAmount[2]` uniform array per body part.
Part masking: `hiddenMask` bitfield for armor/clothing visibility.
DARKEN=0.8 multiplier on final albedo.

**character.hlsl** (12 permutations):
Entry points: `main_fs` (full body), `distant_fs` (LOD), `hair_fs`, `zero_fs`, `severed_limb_fs`.
13 texture inputs: body diffuse/normal/color/mask, muscle blend normal, head diffuse/normal/mask,
hair, beard, vest diffuse/normal, blood. Skin tone via mask subtraction, configurable hair/beard
color channels. `headMask.g` controls glowy eyes. DARKEN=0.7.

**foliage.hlsl** (12 permutations):
Entry points: `grass_vs`/`grass_fs` (billboard grass), `foliage_vs`/`foliage_fs` (tree leaves),
`farm_vs`/`farm_shadow_vs` (farm plants). Grass uses sine-wave wind animation, distance fade,
hard alpha clip, writes flat upward normal (no normal map). Farm plants use `plantData[256]`
instance array with per-plant rotation and death color.

**triplanar.hlsl** (20 permutations):
Entry points: `triplanar_vs`/`triplanar_ps`. World-space UV divided by 5000 with `worldOffset`
and `tileScale`. Power-2 axis weights for smooth blending. `sampleTriplanarNormal()` correctly
flips normals for opposite-facing sides.

### Forward Pass Materials (after deferred lighting)

| Material | Source | Draws | Notes |
|----------|--------|------:|-------|
| Water | `water.hlsl` | 1 | Flow-mapped normals, rain ripples, scum, PBR lighting, planar reflection |
| Water decor | `objects.hlsl` PS + `water.hlsl` VS | 16 | Surface decoration quads (24 indices each) |
| Atmosphere fog | `fog.hlsl` (atmosphere_fog_fs) | 1 | Fullscreen, uses depth buffer + atmospheric scattering |
| Fog volumes | `fog.hlsl` (fog_planes_fs) | 11 | Convex volumes (7 clipping planes), ray-plane intersection |
| Basic forward | `basic.hlsl` | 25+ | Simple unlit geometry (indicators, debug, UI overlays) |

### Sky System

| Source | Entry Point | Purpose |
|--------|------------|---------|
| `SkyX_Skydome.hlsl` | `main_vp`/`main_fp` | Rayleigh/Mie atmospheric scattering, HDR/LDR paths, starfield |
| `SkyX_Clouds.hlsl` | cloud shaders | Cloud layer rendering |
| `SkyX_VolClouds.hlsl` | volumetric | Volumetric cloud effects |
| `SkyX_Moon.hlsl` | moon shaders | Moon phases/light |
| `SkyX_Ground.hlsl` | ground horizon | Horizon/ground color |
| `SkyX_Lightning.hlsl` | lightning | Lightning effects |
| `moon.hlsl` | moon mesh | Moon geometry |
| `birds.hlsl` | birds VS | Distant bird LOD |

---

## BRDF — Confirmed PBR (NOT Blinn-Phong)

Source: `data/materials/common/lightingFunctions.hlsl`

### Diffuse: Lambert with Fresnel Energy Conservation

```hlsl
ld.diffuse = PI * dotNL * lightColor * FresnelDiffuse(specColor);
// FresnelDiffuse(specColor) = saturate(1 - avg(specColor))
```

Simple Lambert (N dot L) with energy conservation that reduces diffuse as specular
increases. Disney/Burley diffuse is IMPLEMENTED in the same file but commented out:
```hlsl
//ld.diffuse = lightFactor * Fr_DisneyDiffuse(view, light, normal, roughness);
```

### Specular: GGX Microfacet (Trowbridge-Reitz)

```hlsl
ld.specular = lightColor * LightingFuncGGX_OPT3(N, V, L, roughness, F0) / PI;
```

Implementation details:
- **D (NDF)**: GGX / Trowbridge-Reitz: `alpha^2 / (PI * (dotNH^2 * (alpha^2-1) + 1)^2)`
- **F (Fresnel)**: Schlick, Horner-form approximation: `exp2((-5.55473*dotLH - 6.98316)*dotLH)`
- **V (Visibility)**: Kelemen-Szirmay-Kalos approximation: `1 / (dotLH^2 * (1-k^2) + k^2)` where `k = alpha/2`

### Roughness Mapping

```hlsl
float roughness = 1.0 - gloss * 0.99;  // gloss=0 -> rough=1.0, gloss=1 -> rough=0.01
float alpha = roughness * roughness;     // perceptual roughness to alpha
```

### Metalness Workflow

Standard PBR metalness:
```hlsl
float3 specColor = lerp(dielectric_spec, albedo, metalness);  // F0
albedo = lerp(albedo, 0.0, metalness);                        // metals have no diffuse
```

### Environment Lighting (IBL)

- **Irradiance**: 16x16 cubemap (s6), sampled at mip 3, decoded `rgb * a * 4.0`
- **Specular**: 256x256 cubemap (s7), mip = `(1-gloss) * 7.0`, decoded `rgb * a * 10.0`
- **Dominant direction**: Frostbite approach (`GetSpecularDominantDir`)
- **Environment BRDF**: Lazarov analytic fit (Black Ops 2): `LazarovEnvironmentBRDF(gloss, NdotV, F0)`

### Translucency

Simple wrap lighting for translucent materials:
```hlsl
if (dotNL < 0) dotNL = lerp(0.0, -dotNL, translucency);
```

### BRDF Upgrade Opportunities

1. **Easiest win**: Uncomment Disney/Burley diffuse (already implemented, just commented out)
2. **Smith-GGX correlated**: Replace Kelemen-SzK visibility with height-correlated Smith GGX
3. **Multi-scatter**: Add Kulla-Conty energy compensation for rough metals
4. **Better IBL**: Replace Lazarov with Split-Sum or higher-quality analytic fit

---

## Shadow System — Cascaded Shadow Maps

Source: `data/materials/common/shadowFunctions.hlsl`, `deferred_vanilla.hlsl`

### Configuration

- **Format**: R32_FLOAT 4096x4096 (single atlas) + D32_FLOAT depth
- **Cascades**: `SHADOW_MAP_COUNT = 4` (default, supports up to 9)
- **Resolution**: 4096x4096 (consistent across all 33 captures)
- **Filtering**: 12-sample PCF with hex pattern (`ENABLE_PCF_HEX12`), jittered rotation
- **Bias**: Fixed + slope-scaled, with surface-normal-aware sample projection

### Shadow Map Rendering

- Rendered AFTER GBuffer fill, BEFORE deferred lighting
- 196-1378 draws per frame (avg 579)
- 52% of shadow draws use tessellation (PATCHLIST_3_CP with hull/domain shaders)
- 7 unique VS, 4 unique PS, 1 HS, 1 DS in shadow pass
- Shadow VS are DIFFERENT from GBuffer VS (0 shared)

Shadow caster shaders support:
- `INSTANCED`: Instance matrix from vertex attributes (TEXCOORD0-2)
- `TEXCOORDS` + `ENABLE_ALPHA_TEST`: Alpha-tested shadow casting (foliage)
- Depth bias: fixed + slope-scaled + max-slope-clamped

### CSM Cascade Selection

Cascades selected by screen-space depth (`posSs.z`):
```hlsl
shadowUv = csmTrans[i].xyz + csmScale[i].xyz * posLs;
```
Where `posLs = mul(shadowViewMat, posWs).xyz` transforms to light space.

### Shadow Quality Issues

The 12-sample hex PCF with jitter rotation produces reasonable quality but:
- Single 4096x4096 atlas for 4 cascades = only 2048x2048 effective per cascade
- No cascade blending at split boundaries (hard transitions visible)
- No PCSS (contact-hardening) — fixed filter radius regardless of blocker distance
- Jitter can cause temporal noise

---

## Point / Spot Lights — Forward Light Volumes

Source: `deferred_vanilla.hlsl` (`light_vs` / `light_fs`)

Point and spot lights are rendered as **forward draws with light volume meshes** after
the fullscreen deferred lighting pass. Each light gets its own draw call.

### Light Volume Rendering

- `light_vs`: Transforms a sphere/cone mesh with `worldViewProjMatrix`
- `light_fs`: Reads the GBuffer (same gBuf0/1/2), computes per-pixel lighting for the light volume
- Same GGX specular + metalness workflow as the sun pass
- Additive blending onto the R11G11B10_FLOAT HDR target

### Attenuation Model

Custom cubic falloff (not physically-based inverse-square):
```hlsl
float x = saturate(distance / falloff.w);
float start = pow(1-x, 3) * 0.8 + 0.2;   // near: smooth cubic
float vb = -3*pow(x-0.6, 2) + 0.242;      // mid: parabolic bump
float vc = 3*pow(x-1.0, 2);                // far: quadratic fade
attenuation = x < 0.649? start : (x < 0.8? vb : vc);
```

### Spotlight Extension

Spotlights add angular attenuation:
```hlsl
float spotFactor = pow(saturate((dot(direction, -lightDir) - spot.y) / (spot.x - spot.y)), spot.z);
```

### Light Count

Forward light volume draws: 0-59 per frame depending on nearby light sources.
Each draw = one light. No light budget cap — every visible light gets a draw.

Light uniforms per draw: `diffuseColour`, `specularColour`, `falloff` (constant, linear,
quadratic, radius), `position`, `power`, optional `direction` + `spot` for spotlights.

---

## Cubemap / Reflection Probe Pass

Renders the scene into a 512x512 cubemap for environment reflections.
Not always present — depends on whether a reflection probe is near the camera.

- Resolution: 512x512 B8G8R8A8_UNORM + D32_FLOAT
- Shares 34/37 pixel shaders with GBuffer (same material shaders, lower res)
- 0-1400 draws depending on scene complexity
- Output feeds into the `irradianceCube` (s6) and `specularityCube` (s7) of the deferred pass

---

## Deferred Lighting Pass

### Sun Pass (`main_fs`)

| Property    | Value |
|-------------|-------|
| **Draw**    | Fullscreen triangle (TRIANGLELIST, 3 vertices) |
| **PS**      | 2 variants across captures (day/night or CSM/RTW permutation) |
| **Output**  | R11G11B10_FLOAT native |

**PS SRV Inputs:**
| Slot | Format              | Size       | Content |
|------|---------------------|------------|---------|
| 0    | R16G16B16A16_FLOAT  | native     | GBuffer repack (HDR temp) |
| 1    | R32_FLOAT           | native     | Depth |
| 2    | R32_FLOAT           | native     | Linear depth (GBuffer RT2) |
| 3    | B8G8R8A8_UNORM      | 1024x1024  | Ambient map (world-space tint) |
| 4    | R32_FLOAT           | 513x2      | Shadow jitter / filter kernel |
| 5    | R32_FLOAT           | 4096x4096  | Shadow depth map (CSM atlas) |
| 6    | BC3_UNORM CUBE      | 16x16      | Irradiance cubemap |
| 7    | BC3_UNORM CUBE      | 256x256    | Specular cubemap |

**VS SRV Inputs** (unusual — VS reads GBuffer directly):
| Slot | Format           | Size       | Content |
|------|------------------|------------|---------|
| 0    | B8G8R8A8_UNORM   | native     | GBuffer RT0 (albedo+metalness+gloss) |
| 1    | B8G8R8A8_UNORM   | native     | GBuffer RT1 (normal+emissive) |
| 2    | R32_FLOAT        | native     | GBuffer RT2 (depth) |
| 3    | B8G8R8A8_UNORM   | 1024x1024  | Ambient map |

**PS CB0 (32 bytes):**
| Register | Values (example)     | Meaning |
|----------|---------------------|---------|
| c0       | 2560, 1440, 1/2560, 1/1440 | Resolution + texel size |
| c1       | 1.543, 1.0, 2560, 1440     | tan(fovY/2)*aspect(?), 1.0, resolution |

**VS CB0 (32 bytes):**
| Register | Values (example)      | Meaning |
|----------|-----------------------|---------|
| c0       | 36818, 20710, -50000, -451 | Far frustum corner 1 |
| c1       | -36818, -20710, -50000, 0.00008 | Far frustum corner 2 + near plane |

**Key uniforms (from shader source):**
sunDirection, sunColour, pFogParams, envColour, worldSize, worldOffset,
viewport, offset, inverseView, ambientParams, proj, shadowParams, csmParams[4],
csmScale[4], csmTrans[4], csmUvBounds[4], shadowViewMat

---

## Shader Inventory (Across All Captures)

**492 unique shader addresses mapped to 50 source files, 0 unidentified.**

| Category | Source Files | Shaders | Notes |
|----------|-------------|--------:|-------|
| Objects/static | `objects.hlsl` | 99 | Most complex, 11+ #ifdef variants |
| Terrain | `terrainfp4.hlsl`, `terrain.hlsl` | 49 | Texture2DArray biome system |
| Shadow casters | `shadowcaster.hlsl` | 30 | Depth + alpha test + construction |
| Triplanar | `triplanar.hlsl` | 20 | Rocks/cliffs, world-space projection |
| Fog | `fog.hlsl` | 20 | Atmosphere + volume planes/spheres/beams |
| Basic | `basic.hlsl` | 20 | Simple unlit, forward |
| Deferred lighting | `deferred.hlsl` | 19 | Sun (main_fs) + light volumes (light_fs) |
| HDR post | `hdrfp4.hlsl` | 23 | Bloom, luminance, tonemapping |
| Skinning | `skin.hlsl` | 16 | 60-bone HW skinning |
| Character | `character.hlsl` | 12 | Full body + hair, 13 textures |
| Foliage | `foliage.hlsl` | 12 | Grass, tree leaves, farm plants |
| Quad VP | `quad_vp.hlsl` | 12 | Fullscreen quad VS (shared across all post) |
| Water | `water.hlsl` | 10 | Flow-mapped, PBR, rain ripples |
| Sky (SkyX) | 6 SkyX source files | 38 | Atmospheric scattering, clouds, moon, lightning |
| Shadow tess | `rtwtessellator.hlsl` | 10 | SM5.0 HS/DS for shadow resolution |
| RTW | `rtwshadows.hlsl`, `rtwforward.hlsl`, etc. | 14 | Warp map generation |
| Post-processing | FXAA, CAS, DepthBlur, heathaze, etc. | 22 | Individual effect shaders |
| UI | `basic2D.hlsl`, `rtticons.hlsl`, `rtt.hlsl` | 10 | 2D overlays |
| Other | construction, creature, distant_town, etc. | 20+ | Specialized materials |

Source files located at: `data/materials/` (game directory).
Pre-compiled shaders (not captured by D3DCompile hook): deferred sun/light, stencil mask,
GBuffer repack, all LDR post-processing, FXAA, heat haze. Identified by address cross-reference.

---

## Instancing Potential

From GBuffer analysis (Shark capture, 1177 draws):
- **53% draw call reduction** possible through instancing
- 147 unique mesh signatures (VS + PS + stride + indexCount)
- Top repeater: 44 instances of same mesh (save 43 draws)
- Multiple mesh groups with 20-36 identical draws

Instancing requires: capture per-draw world matrix from VS CB, batch into instance buffer,
replace N DrawIndexed with 1 DrawIndexedInstanced.

---

## VS Constant Buffer Layout (GBuffer)

Single CB at slot 0, 160 bytes (10 float4 registers). From skin.hlsl:

| Register | Size    | Content |
|----------|---------|---------|
| c0       | float4  | (1.0, 1.0, materialParam, 0.0) — per-object |
| c1-c4    | 4xfloat4 | View-related matrix (same across all draws in frame) |
| c5       | float4  | Camera world position + distance |
| c6       | float4  | Material color / per-object parameter |
| c7-c9    | 3xfloat4 | Additional per-object data (zeros for many draws) |

Bone matrices (for skinned meshes) are passed separately, not in this CB.
The viewProjectionMatrix and worldMatrix uniforms declared in skin.hlsl map to these registers
(exact mapping depends on OGRE's auto-parameter binding order).

---

## Key Source Files (Game Directory)

| File | Location | Content |
|------|----------|---------|
| `gbuffer.hlsl` | `data/materials/deferred/` | GBuffer struct, YCoCg encode/decode, write functions |
| `lightingFunctions.hlsl` | `data/materials/common/` | GGX specular, Fresnel, Disney diffuse (commented), IBL |
| `shadowFunctions.hlsl` | `data/materials/common/` | CSM cascade selection, PCF filtering, VSM |
| `deferred_vanilla.hlsl` | (Dust mod directory) | Full deferred lighting shader (sun + point lights) |

---

## State Change Analysis (Shark, 3763 draws)

| Metric | Count | % of transitions |
|--------|------:|----------------:|
| PS changes | 369 | 9.8% |
| VS changes | 301 | 8.0% |
| RT changes | 36 | 1.0% |
| SRV changes | 474 | 12.6% |
| Redundant PS binds | 3393 | 90.2% |
| Redundant VS binds | 3461 | 92.0% |

OGRE sorts draws to minimize state changes. Within the GBuffer pass (1177 draws), only
115 PS changes and 89 VS changes — the engine batches by material.

---

## Implementation Reference — Where to Inject

| Feature | Injection Point | Shaders to Modify |
|---------|----------------|-------------------|
| SSAO/SSIL | After deferred sun (draw 3648), before light volumes | New fullscreen PS |
| SSS (subsurface) | After deferred sun, needs skin mask from GBuffer | `skin.hlsl`, `character.hlsl` GBuffer encoding + new deferred pass |
| Shadow replacement | Replace draws 2590-3640 or intercept shadow RT bind | `shadowcaster.hlsl`, `rtwtessellator.hlsl` |
| RT shadows | After GBuffer (1407-2583), before deferred lighting | New compute/ray dispatch |
| Voxel GI | Re-render GBuffer geometry into voxel grid before deferred | All GBuffer PS/VS (redirect output) |
| Tessellation | Intercept GBuffer draws, add HS/DS | `terrain.hlsl`, `objects.hlsl` (add HS/DS like `rtwtessellator.hlsl`) |
| Material upgrade | Replace GBuffer PS per source file | `objects.hlsl`, `terrainfp4.hlsl`, `skin.hlsl`, `character.hlsl`, etc. |
| Better water | Replace water forward pass | `water.hlsl` |
| Better sky | Replace sky draws | `SkyX_Skydome.hlsl`, `SkyX_Clouds.hlsl` |
| Bloom replacement | Replace bloom chain (draws 3716-3737) | `hdrfp4.hlsl` bloom entry points |
| Tonemapping | Replace composite (draw 3718) | `hdrfp4.hlsl` Composite_fp4 |
| AA replacement | Replace FXAA draws (3739-3762) | `FXAA.hlsl` |

### Key Constant Buffers for Feature Implementation

**Deferred lighting PS CB0 (32 bytes):**
c0 = (resX, resY, 1/resX, 1/resY), c1 = (tan(fovY/2)*aspect, 1.0, resX, resY)

**Deferred lighting VS CB0 (32 bytes):**
c0 = far frustum corner 1, c1 = far frustum corner 2 + near plane

**GBuffer VS CB0 (160 bytes):**
c0 = per-object params, c1-c4 = viewProjection, c5 = camera world pos, c6-c9 = per-object material

---

## Open Questions — RESOLVED

1. **BRDF**: Lambert diffuse + GGX specular (NOT Blinn-Phong). Disney/Burley diffuse is implemented but commented out.
2. **Roughness/metalness in GBuffer**: YES. Metalness in RT0.B, gloss in RT0.A. Albedo sacrifices blue channel precision (YCoCg encoding) to make room.
3. **Point lights**: Forward pass with light volume meshes. Each light = one draw. Same GGX BRDF. 0-59 lights per frame.
4. **Shadow quality**: CSM with 4 cascades in 4096x4096 atlas. 12-sample hex PCF with jitter. Issues: no cascade blending, no PCSS, effective ~2048x2048 per cascade.
5. **Draw call overhead**: 582-3763 draws/frame. 53% instancing potential in GBuffer. Cubemap pass repeats entire scene at 512x512.
6. **VS CB layout**: 160 bytes in slot 0. View matrix + camera position (frame-constant) plus per-object material params. Bone matrices separate.
7. **Pipeline surprises**: (a) YCoCg chroma subsampling in GBuffer, (b) shadow map rendered AFTER GBuffer not before, (c) tessellated shadow casters, (d) VS reads GBuffer in deferred pass (OGRE pattern), (e) BRDF is already PBR-quality GGX not Blinn-Phong as assumed.
8. **Material identification**: ALL 50 shader source files identified, ALL 492 unique shader addresses mapped. Every draw call in the frame classified by purpose and material type.
