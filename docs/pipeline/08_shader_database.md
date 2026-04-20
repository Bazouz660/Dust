# Complete Shader Database

492 unique shader addresses mapped to 50 source files. 0 unidentified.

## Per-Source Summary (Shark frame: 128 shaders, all mapped)

| Source File           | Type | Shaders | Draws | Passes |
|-----------------------|------|--------:|------:|--------|
| objects.hlsl          | ps   |      20 |   935 | CUBEMAP, GBUFFER, WATER_DECOR |
| objects.hlsl          | vs   |       5 |   882 | CUBEMAP, GBUFFER |
| skin.hlsl             | ps   |       1 |   584 | CUBEMAP, GBUFFER |
| skin.hlsl             | vs   |       4 | 1,348 | CUBEMAP, GBUFFER, SHADOW |
| shadowcaster.hlsl     | ps   |       4 | 1,051 | SHADOW |
| rtwtessellator.hlsl   | vs   |       4 |   548 | SHADOW |
| rtwtessellator.hlsl   | hs   |       1 |   548 | SHADOW |
| rtwtessellator.hlsl   | ds   |       1 |   548 | SHADOW |
| foliage.hlsl          | ps   |       1 |   290 | CUBEMAP, GBUFFER |
| foliage.hlsl          | vs   |       3 |   309 | CUBEMAP, GBUFFER, SHADOW |
| character.hlsl        | ps   |       2 |   262 | CUBEMAP, GBUFFER |
| terrain.hlsl          | ps   |       1 |   230 | CUBEMAP, GBUFFER |
| terrain.hlsl          | vs   |       2 |   398 | CUBEMAP, GBUFFER |
| terrainfp4.hlsl       | ps   |       8 |   220 | CUBEMAP, GBUFFER |
| terrainfp4.hlsl       | vs   |       1 |    65 | SHADOW |
| creature.hlsl         | ps   |       1 |    73 | CUBEMAP, GBUFFER |
| mapfeature.hlsl       | vs   |       1 |    42 | CUBEMAP, GBUFFER |
| basic.hlsl            | ps   |       2 |    30 | FORWARD_BASIC, FULLSCREEN_HDR |
| basic.hlsl            | vs   |       2 |    30 | FORWARD_BASIC, FULLSCREEN_HDR |
| hdrfp4.hlsl           | ps   |      14 |    23 | BLOOM, HALF_RES_POST, LUMINANCE, TONEMAP_COMPOSITE |
| FXAA.hlsl             | ps   |       2 |    23 | FXAA |
| FXAA.hlsl             | vs   |       1 |    22 | FXAA |
| fog.hlsl              | ps   |       2 |    12 | FOG |
| fog.hlsl              | vs   |       1 |    11 | FOG |
| heathaze.hlsl         | ps   |       2 |     8 | HEAT_HAZE |
| heathaze.hlsl         | vs   |       1 |     7 | HEAT_HAZE |
| triplanar.hlsl        | ps   |       1 |     7 | CUBEMAP, GBUFFER |
| triplanar.hlsl        | vs   |       1 |     7 | CUBEMAP, GBUFFER |
| distant_town.hlsl     | ps   |       1 |     6 | CUBEMAP, GBUFFER |
| distant_town.hlsl     | vs   |       1 |     6 | CUBEMAP, GBUFFER |
| post_ldr.hlsl         | ps   |       6 |     6 | LDR_POST |
| post_ldr.hlsl         | vs   |       1 |     3 | LDR_POST |
| gbuffer_repack.hlsl   | ps   |       3 |     3 | GBUFFER_REPACK |
| gbuffer_repack.hlsl   | vs   |       1 |     3 | GBUFFER_REPACK |
| stencil_mask.hlsl     | ps   |       3 |     3 | STENCIL_MASK |
| stencil_mask.hlsl     | vs   |       1 |     3 | STENCIL_MASK |
| deferred.hlsl         | ps   |       3 |     3 | DEFERRED_SUN, LIGHT_VOLUME |
| deferred.hlsl         | vs   |       1 |     2 | DEFERRED_SUN, FOG |
| rtwshadows.hlsl       | ps   |       3 |     3 | LUMINANCE, UNKNOWN |
| rtwshadows.hlsl       | vs   |       1 |     3 | LUMINANCE, UNKNOWN |
| SkyX_Skydome.hlsl     | ps   |       1 |     2 | CUBEMAP, SKY |
| SkyX_Skydome.hlsl     | vs   |       1 |     2 | CUBEMAP, SKY |
| SkyX_Clouds.hlsl      | ps   |       1 |     2 | CUBEMAP, SKY |
| SkyX_Clouds.hlsl      | vs   |       1 |     2 | CUBEMAP, SKY |
| moon.hlsl             | ps   |       1 |     2 | CUBEMAP |
| moon.hlsl             | vs   |       1 |     2 | CUBEMAP |
| quad_vp.hlsl          | vs   |       2 |    31 | BLOOM, DEFERRED_SUN, DOF_COC, FXAA, HALF_RES_POST, HEAT_HAZE, LDR_POST, LIGHT_VOLUME, LUMINANCE, TONEMAP_COMPOSITE |
| rtwbackward.hlsl      | ps   |       1 |     1 | LUMINANCE |
| rtwbackward.hlsl      | vs   |       1 |     1 | LUMINANCE |
| water.hlsl            | ps   |       1 |     1 | WATER |
| water.hlsl            | vs   |       1 |    17 | WATER, WATER_DECOR |
| DepthBlur.hlsl        | ps   |       1 |     1 | DOF_COC |
| birds.hlsl            | vs   |       1 |     1 | CUBEMAP |

## Aggregate by Category

| Category              | Source Files | Total Shaders | Notes |
|-----------------------|-------------|-------------:|-------|
| Objects/static        | objects.hlsl | 99           | 91 permutations across all captures, most complex |
| Terrain               | terrainfp4.hlsl, terrain.hlsl | 49 | Texture2DArray biome system |
| Shadow casters        | shadowcaster.hlsl | 30       | Depth + alpha test + construction |
| Triplanar             | triplanar.hlsl | 20          | Rocks/cliffs, world-space projection |
| Fog                   | fog.hlsl | 20               | Atmosphere + volume planes/spheres/beams |
| Basic                 | basic.hlsl | 20             | Simple unlit, forward |
| Deferred lighting     | deferred.hlsl | 19          | Sun (main_fs) + light volumes (light_fs) |
| HDR post              | hdrfp4.hlsl | 23            | Bloom, luminance, tonemapping |
| Skinning              | skin.hlsl | 16              | 60-bone HW skinning |
| Character             | character.hlsl | 12          | Full body + hair, 13 textures |
| Foliage               | foliage.hlsl | 12           | Grass, tree leaves, farm plants |
| Quad VP               | quad_vp.hlsl | 12           | Fullscreen quad VS (shared all post) |
| Water                 | water.hlsl | 10             | Flow-mapped, PBR, rain ripples |
| Sky (SkyX)            | 6 SkyX files | 38           | Atmospheric scattering, clouds, moon, lightning |
| Shadow tessellation   | rtwtessellator.hlsl | 10     | SM5.0 HS/DS for shadow resolution |
| RTW                   | rtwshadows.hlsl + rtwforward/backward | 14 | Warp map generation |
| Post-processing       | FXAA, CAS, DepthBlur, heathaze, etc. | 22 | Individual effect shaders |
| UI                    | basic2D.hlsl, rtticons.hlsl, rtt.hlsl | 10 | 2D overlays |
| Other                 | construction, creature, distant_town, etc. | 20+ | Specialized materials |

## Pre-Compiled Shaders

These shaders were compiled before the D3DCompile hook and identified by address cross-reference:

| Shader | Source | Address |
|--------|--------|---------|
| Deferred sun PS | deferred.hlsl | 0x6162EFF8 |
| Deferred sun VS | deferred.hlsl | 0x6162EB38 |
| Light volume PS | deferred.hlsl | 0x63DB5878 |
| Light volume VS | quad_vp.hlsl | 0x6394F578 |
| Stencil mask PS (x3) | stencil_mask.hlsl | 0x6394FA38, 0x6394FBB8, 0x63950378 |
| Stencil mask VS | stencil_mask.hlsl | (shared) |
| GBuffer repack PS (x3) | gbuffer_repack.hlsl | 0x63DB40F8, 0x63DB42B8, 0x63DB3C78 |
| GBuffer repack VS | gbuffer_repack.hlsl | (shared) |
| LDR post PS (x6) | post_ldr.hlsl | 0x63DB4A78, 0x63DB4BF8, 0x63DB64F8, 0x63DB4D78, 0x63DB32F8, 0x63DB3AB8 |
| FXAA PS (x2) | FXAA.hlsl | 0x63A4E4F8, 0x617F6738 |
| Heat haze PS (x2) | heathaze.hlsl | 0x63DB6038, 0x64447B38 |
| DOF PS | DepthBlur.hlsl | 0x63DB6CB8 |
| Half-res PS (x3) | hdrfp4.hlsl | 0x63DB6378, 0x63DB5238, 0x63DB2FF8 |
| Bloom PS (x2) | hdrfp4.hlsl | 0x63DB5578, 0x63DB59F8 |
| Luminance PS | hdrfp4.hlsl | 0x63DB50B8 |

## Cross-Capture Shader Counts

| Capture                | Total Draws | Unique PS | Unique VS |
|------------------------|------------:|----------:|----------:|
| Shark (densest)        |       3,763 |        85 |        41 |
| Mongrel                |       3,490 |        81 |        38 |
| Worlds End             |       2,817 |        80 |        36 |
| Hidden Forest          |       2,501 |        82 |        40 |
| Ashlands (sparsest)    |         582 |        61 |        32 |
| Average                |       1,545 |       ~74 |       ~36 |

Total unique shaders across all 33 captures: **492** (mapped to 50 source files).
