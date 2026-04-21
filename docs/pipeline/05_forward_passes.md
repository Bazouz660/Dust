# Forward Passes — Sky, Water, Fog, Basic Geometry

All forward passes render after deferred lighting into the R11G11B10_FLOAT HDR buffer
(except basic.hlsl which can render to various targets).

---

## Sky System (Draws 2584-2585)

Source: SkyX atmospheric scattering system (OGRE plugin).

### Shaders

| Source                | Entry Points    | Address    | Purpose |
|-----------------------|-----------------|------------|---------|
| `SkyX_Skydome.hlsl`  | `main_vp`/`main_fp` | PS: 0x63926938, VS: 0x63926638 | Rayleigh/Mie scattering, starfield |
| `SkyX_Clouds.hlsl`   | `main_vp`/`main_fp` | PS: 0x63926C78, VS: 0x64446238 | Cloud layer rendering |

### SkyX Source Files (full inventory)

| File                     | Purpose |
|--------------------------|---------|
| `SkyX_Skydome.hlsl`     | Atmospheric scattering (Rayleigh/Mie), HDR/LDR paths, starfield (~185 lines) |
| `SkyX_Clouds.hlsl`      | Cloud layer rendering |
| `SkyX_VolClouds.hlsl`   | Volumetric cloud effects |
| `SkyX_Moon.hlsl`        | Moon phases/light |
| `SkyX_Ground.hlsl`      | Horizon/ground color |
| `SkyX_Lightning.hlsl`   | Lightning effects |
| `moon.hlsl`             | Moon geometry mesh (2 draws in cubemap pass) |
| `birds.hlsl`            | Distant bird LOD (VS only) |

### Features

- Rayleigh scattering for blue sky gradient
- Mie scattering for sun glow / atmospheric haze
- HDR and LDR code paths (HDR active in normal rendering)
- Starfield rendering at night
- 38 total shader permutations across all SkyX files

### Render State

- Output: R11G11B10_FLOAT native + depth
- Always 2 draws: dome + clouds (present in all 33 captures)
- Cubemap pass also includes sky (dome + moon + clouds)

---

## Water Surface (Draw 3650)

Source: `water.hlsl` (~264 lines)

| Property     | Value |
|--------------|-------|
| PS entry     | `waterFP` (0x63A4E1F8) |
| VS entry     | `waterVP` (0x63850FB8) |
| Output       | R11G11B10_FLOAT native + depth |
| Draws        | 1 (always present) |

### Features

- **Flow-mapped normals**: Animated water flow using flow map texture
- **Rain ripples**: Procedural rain disturbance on surface
- **Scum/foam**: Shoreline foam blending
- **PBR lighting**: Full GGX specular + metalness (own lighting, not deferred)
- **Planar reflection**: Reflection probe input for mirror-like reflections
- **Refraction**: Depth-based underwater distortion

### Textures

| Type          | Format / Details |
|---------------|-----------------|
| Flow map      | Directional flow vectors |
| Normal maps   | Two scrolling normal maps for surface detail |
| Reflection    | Planar reflection texture (from cubemap or dedicated pass) |
| Foam/scum     | Shoreline effect texture |

### SRV Binds

BC3_UNORM 512x512 TEXTURECUBE (17), B8G8R8A8_UNORM 512x512 (18), B8G8R8A8_UNORM 128x128 (34)

---

## Water Decoration (Draws 3651-3666)

Surface decoration quads rendered on top of the water surface.

| Property     | Value |
|--------------|-------|
| PS           | `objects.hlsl` main_ps (same as GBuffer objects) |
| VS           | `water.hlsl` waterVP |
| Topology     | TRIANGLELIST, 24 indices per draw |
| Draws        | 16 (in Shark; not always present) |
| Output       | R11G11B10_FLOAT native + depth |

These are small quad meshes (debris, floating objects) positioned on the water surface
using the water VS for wave displacement.

---

## Atmosphere Fog (Draw 3667)

Source: `fog.hlsl` (~361 lines)

### Fullscreen Atmosphere Pass

| Property     | Value |
|--------------|-------|
| PS entry     | `atmosphere_fog_fs` (0x63A4D238) |
| VS           | `deferred.hlsl` main_vs (0x6162EB38) — shared with deferred pass |
| Type         | Fullscreen (reuses deferred VS) |
| Output       | R11G11B10_FLOAT native + depth |
| Draws        | 1 (always present) |

Features:
- Reads depth buffer for distance-based fog
- Atmospheric scattering integration
- Color tinted by time of day / environment

---

## Fog Volumes (Draws 3668-3678)

| Property     | Value |
|--------------|-------|
| PS entry     | `fog_planes_fs` (0x63A4E378) |
| VS entry     | `fog_planes_vs` (0x63A4E038) |
| Draws        | 11 (Shark), variable per scene |
| Output       | R11G11B10_FLOAT native + depth |

### Fog Types (from fog.hlsl, 5 total)

| Type             | Entry Point          | Description |
|------------------|----------------------|-------------|
| Atmosphere       | `atmosphere_fog_fs`  | Fullscreen, depth-based (always 1 draw) |
| Ground planes    | `fog_planes_fs`      | Convex volumes, 7 clipping planes |
| Sphere           | `fog_sphere_fs`      | Spherical fog volumes |
| Beam             | `fog_beam_fs`        | Directional light beam volumes |
| Volume planes    | `fog_planes_fs`      | Layered horizontal fog |

Fog volumes use ray-plane intersection with 7 clipping planes for convex volume shapes.

---

## Forward Basic Geometry (Draws 3679-3708)

Source: `basic.hlsl` (~113 lines)

| Property     | Value |
|--------------|-------|
| PS entry     | `basic_fp` (2 variants: 0x63E70778, 0x63E70F38) |
| VS entry     | `basic_vp` (2 variants: 0x63E70138, 0x63E70BF8) |
| Draws        | 25+ per frame |
| Output       | R11G11B10_FLOAT native + depth |

Simple unlit geometry — no GBuffer output, no lighting calculation.
Used for: indicators, debug visualization, UI overlays, selection markers.

### Fullscreen HDR (Draws 3699-3707)

5 draws using `basic.hlsl` that render fullscreen quads into the HDR buffer.
Same shaders as forward basic but fullscreen geometry. Used for screen-space effects
and buffer copies between post-processing steps.

---

## Forward Pass Summary

| Pass               | Draws | Source          | Always Present |
|--------------------|------:|-----------------|:--------------:|
| Sky                |     2 | SkyX shaders    | Yes            |
| Water              |     1 | water.hlsl      | Yes            |
| Water decoration   |  0-16 | objects+water   | No             |
| Atmosphere fog     |     1 | fog.hlsl        | Yes            |
| Fog volumes        | 0-11+ | fog.hlsl        | Yes (count varies) |
| Forward basic      |  20+  | basic.hlsl      | Yes            |
| Fullscreen HDR     |   0-5 | basic.hlsl      | No             |
