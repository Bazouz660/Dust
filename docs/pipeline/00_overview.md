# Kenshi Rendering Pipeline — Overview

Verified via Dust Pipeline Survey system across 33 captures, 32 locations, day/night.
Reference capture: `2026-04-20_193729_shark` (3763 draws, densest scene).
Date: 2026-04-20.

## Engine & API

| Property         | Value                                              |
|------------------|----------------------------------------------------|
| Graphics API     | Direct3D 11, Feature Level 11_0                    |
| Engine           | OGRE 2.0 (Object-Oriented Graphics Rendering Engine) |
| Rendering model  | Deferred shading + forward light volumes + forward transparent |
| Resolution       | Native (tested at 2560x1440)                       |
| Depth format     | D24_UNORM_S8_UINT (hardware), R32_FLOAT (linear GBuffer) |
| Shadow format    | R32_FLOAT 4096x4096, D32_FLOAT depth               |
| HDR format       | R11G11B10_FLOAT                                    |
| BRDF             | GGX specular + Lambert diffuse with Fresnel energy conservation |
| Shader model     | SM5.0 (tessellation used in shadow pass)           |
| Shader sources   | 50 HLSL source files, 492 unique compiled permutations |

## Frame Structure

Complete pipeline order. Draw indices from Shark capture (3763 draws). Every shader identified.

```
Pass                        Draws     Range          Shader Sources                     Render Target
─────────────────────────────────────────────────────────────────────────────────────────────────────────
Heat haze (prev frame)          7     0-6            heathaze.hlsl                      R8G8B8A8_UNORM native + depth
Cubemap / reflection probe   1400     7-1406         (same as GBuffer materials)        B8G8R8A8_UNORM 512x512 + D32_FLOAT
GBuffer fill (geometry)      1177     1407-2583      objects/terrain/skin/char/foliage   3 MRT + D24_UNORM_S8_UINT
Sky rendering                   2     2584-2585      SkyX_Skydome/Clouds.hlsl           R11G11B10_FLOAT native + depth
RTW warp map generation         4     2586-2589      rtwshadows/backward.hlsl           R32_FLOAT 512x512 -> 513x2
Shadow map (CSM)             1051     2590-3640      shadowcaster.hlsl + tess            R32_FLOAT 4096x4096 + D32_FLOAT
Deferred pre-pass               1     3641           deferred.hlsl (fullscreen)         R11G11B10_FLOAT native
Stencil mask                    3     3642-3644      stencil_mask.hlsl                  R8_UNORM native
GBuffer repack                  3     3645-3647      gbuffer_repack.hlsl                R16G16B16A16_FLOAT native
Deferred lighting (sun)         1     3648           deferred.hlsl main_fs              R11G11B10_FLOAT native
Light volumes                   1     3649           deferred.hlsl light_fs             R11G11B10_FLOAT native
Water surface                   1     3650           water.hlsl                         R11G11B10_FLOAT native + depth
Water decoration               16     3651-3666      objects.hlsl PS + water.hlsl VS    R11G11B10_FLOAT native + depth
Atmosphere fog (fullscreen)     1     3667           fog.hlsl atmosphere_fog_fs         R11G11B10_FLOAT native + depth
Fog volumes (planes)           11     3668-3678      fog.hlsl fog_planes_fs             R11G11B10_FLOAT native + depth
Forward basic geometry         25     3679-3708      basic.hlsl                         R11G11B10_FLOAT native + depth
Luminance chain                 7     3709-3715      hdrfp4.hlsl                        R32_FLOAT 128 -> 64 -> 16 -> 4 -> 1
Bloom extract + blur            2     3716-3717      hdrfp4.hlsl                        R11G11B10_FLOAT 640x360
Tonemapping / composite         1     3718           hdrfp4.hlsl Composite_fp4          B8G8R8A8_UNORM native
Luminance adapt (next frame)    1     3719           hdrfp4.hlsl AdaptLum               R32_FLOAT 256x256
LDR post-processing (4x)       4     3720-3723      post_ldr.hlsl                      B8G8R8A8_UNORM native
DOF CoC calculation             1     3724           DepthBlur.hlsl                     R16_FLOAT native
Half-res blur (3 passes)        3     3725-3727      hdrfp4.hlsl                        B8G8R8A8_UNORM 1280x720
LDR composite                   1     3728           post_ldr.hlsl                      B8G8R8A8_UNORM native
Bloom (down 5 + up 4)          9     3729-3737      hdrfp4.hlsl                        R11G11B10_FLOAT 1280->80->1280
LDR final composite             1     3738           post_ldr.hlsl                      B8G8R8A8_UNORM native
FXAA                            1     3739           FXAA.hlsl                          B8G8R8A8_UNORM native + depth
Heat haze                       1     3740           heathaze.hlsl                      R8G8B8A8_UNORM native + depth
FXAA passes (22 draws)         22     3741-3762      FXAA.hlsl                          R8G8B8A8_UNORM native + depth
```

## Draw Count Statistics

Draw counts vary significantly by scene complexity:

| Metric            | Sparse (Ashlands) | Average (33 captures) | Dense (Shark) |
|-------------------|------------------:|----------------------:|--------------:|
| Total draws       |               582 |                 1,545 |         3,763 |
| GBuffer draws     |               238 |                   545 |         1,177 |
| Shadow draws      |               196 |                   579 |         1,051 |
| Cubemap draws     |                 0 |                  ~700 |         1,400 |
| Unique PS/frame   |                61 |                   ~74 |            85 |
| Unique VS/frame   |                32 |                   ~36 |            41 |

## Passes Present Across Captures

**Always present** (in all 33 captures):
GBUFFER, SHADOW, DEFERRED_SUN, SKY, FOG, WATER, BLOOM, LUMINANCE, TONEMAP_COMPOSITE,
LDR_POST, DOF_COC, HALF_RES_POST, FXAA, HEAT_HAZE, STENCIL_MASK, GBUFFER_REPACK

**Sometimes present** (depends on scene):
CUBEMAP, LIGHT_VOLUME, WATER_DECOR, FORWARD_PARTICLES, FULLSCREEN_HDR

## State Change Analysis (Shark, 3763 draws)

| Metric              | Count | % of transitions |
|---------------------|------:|-----------------:|
| PS changes          |   369 |             9.8% |
| VS changes          |   301 |             8.0% |
| RT changes          |    36 |             1.0% |
| SRV changes         |   474 |            12.6% |
| Redundant PS binds  | 3,393 |            90.2% |
| Redundant VS binds  | 3,461 |            92.0% |

OGRE sorts draws to minimize state changes. Within the GBuffer pass (1177 draws), only
115 PS changes and 89 VS changes — the engine batches heavily by material.

## Source File Locations

| File                      | Location                            |
|---------------------------|-------------------------------------|
| `gbuffer.hlsl`            | `data/materials/deferred/`          |
| `lightingFunctions.hlsl`  | `data/materials/common/`            |
| `shadowFunctions.hlsl`    | `data/materials/common/`            |
| `deferred_vanilla.hlsl`   | Dust mod directory                  |
| All material shaders      | `data/materials/` (game directory)  |

## Pipeline Surprises

1. **YCoCg chroma subsampling** in GBuffer — not standard RGB albedo
2. **Shadow map rendered AFTER GBuffer** — not before (unusual ordering)
3. **52% of shadow draws are tessellated** — SM5.0 HS/DS in shadow pass
4. **VS reads GBuffer** in deferred pass — OGRE auto-parameter pattern
5. **BRDF is already PBR-quality GGX** — not Blinn-Phong as might be assumed
6. **Custom cubic attenuation** — not inverse-square falloff for point/spot lights
7. **Disney/Burley diffuse implemented but commented out** in lightingFunctions.hlsl
