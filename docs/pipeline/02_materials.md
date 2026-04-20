# Material System — All Shaders Identified

50 HLSL source files produce 492 unique shader permutations across 33 captures. 0 unidentified.

## GBuffer Material Summary (Shark capture, 1177 draws)

| Material      | Source File         | Draws | %     | Unique PS | Unique VS |
|---------------|---------------------|------:|------:|----------:|----------:|
| Objects       | `objects.hlsl`      |   455 | 38.7% |        19 |         8 |
| Skinned       | `skin.hlsl`         |   292 | 24.8% |         1 |         1 |
| Foliage       | `foliage.hlsl`      |   137 | 11.6% |         1 |         1 |
| Characters    | `character.hlsl`    |   131 | 11.1% |         2 |         2 |
| Terrain       | `terrainfp4.hlsl` + `terrain.hlsl` | 120 | 10.2% | 8+1 | 3 |
| Creatures     | `creature.hlsl`     |    36 |  3.1% |         1 |         1 |
| Triplanar     | `triplanar.hlsl`    |     3 |  0.3% |         1 |         1 |
| Distant towns | `distant_town.hlsl` |     3 |  0.3% |         1 |         1 |

GBuffer materials in ALL captures: Objects, Terrain.
GBuffer materials in SOME captures: Skin, Character, Foliage, Creature, Triplanar, Distant_town.

---

## objects.hlsl — Static & Instanced Geometry

**91 permutations** across all captures. Most complex material shader.

| Property      | Value |
|---------------|-------|
| Entry points  | `main_vs` / `main_ps` |
| Lines         | ~330 |
| Vertex strides | 44, 48, 56, 60, 64, 68, 72, 76, 88, 92, 96 bytes |
| Topologies    | TRIANGLELIST (451), TRIANGLESTRIP (4) |

### Compile Variants (#ifdef)

| Define          | Feature |
|-----------------|---------|
| `INSTANCED`     | Instance matrix from vertex TEXCOORD0-2 |
| `CONSTRUCTION`  | Construction scaffold blend |
| `COLOURING`     | Player color tinting |
| `DUAL_TEXTURE`  | Two diffuse textures blended |
| `DOUBLESIDED`   | Two-sided rendering (flips normal) |
| `DXT5NORMAL`    | Two-channel normal decode from DXT5 |
| `CLIP_INTERIOR` | Interior clipping planes |
| `DUST`          | Dust accumulation layer |
| `TRANSPARENCY`  | Alpha dither (Bayer 4x4 StabilizeFoliageThreshold) |
| `EMISSIVE`      | Emissive material output |
| `INTERIOR`      | Interior lighting mode |

### Textures

| Slot | Sampler          | Content |
|------|------------------|---------|
| s0   | diffuseMap       | Albedo + alpha |
| s1   | normalMap        | Tangent-space normal |
| s2   | colorMap         | Mask (R=?, G=?, B=metalness) |
| s3   | dustMap          | Dust accumulation (when `DUST` enabled) |
| s4   | secondaryDiffuse | Second albedo (when `DUAL_TEXTURE` enabled) |

### Texture Formats Used

BC1_UNORM (1928 binds), BC3_UNORM (1389), BC2_UNORM (508), R16G16_FLOAT (317), B8G8R8A8_UNORM (203)

---

## terrainfp4.hlsl — Terrain System

**44 permutations** across all captures. Texture2DArray-based biome blending.

| Property      | Value |
|---------------|-------|
| Entry points  | `main_fs` (full biome), `mapfeature_fs` (simplified), `simple_fs` |
| Lines         | ~368 |
| Vertex stride | 40 (common), 56, 28, 24 bytes |
| Topologies    | TRIANGLESTRIP (100), TRIANGLELIST (20) |
| VS entry      | `main_vs` (78 draws), `feature_vs` (20 draws) from `mapfeature.hlsl` |

### Features

- **Texture2DArray** for terrain layers (6 layers per biome)
- `computeBiome()` handles per-biome sampling with triplanar cliff projection
- **Overlay color maps** for region tinting
- **Distance fade** for LOD transitions
- **Height-blend** between layers
- `BLEND1`-`BLEND3` defines for multi-biome transitions (up to 4 biome blends)

### Texture Formats

BC1_UNORM (402), BC3_UNORM (286), B8G8R8A8_UNORM (258), R16G16_FLOAT (187), R8G8B8A8_UNORM (22)

---

## terrain.hlsl — Terrain (Simple Path)

Used for terrain tiles outside biome-blend regions. Shares VS with `terrainfp4.hlsl`.

| Property      | Value |
|---------------|-------|
| Entry point   | `main_fs` / `main_vs` |
| Topology      | TRIANGLESTRIP, 8572 indices per draw |
| Draws (Shark) | 230 total (cubemap + GBuffer) |

---

## skin.hlsl — Skinned Meshes

**16 permutations** across all captures. Hardware skeletal animation.

| Property      | Value |
|---------------|-------|
| Entry points  | `main_vs` / `main_fs`, `shadow_vs` |
| Lines         | ~160 |
| Skinning      | 60-bone hardware skinning, 3 blend weights per vertex |
| Vertex strides | 24 (193 draws), 56 (96), 60 (3) |

### Features

- **Blood system**: `atan2` cylinder projection, `bloodAmount[2]` uniform array per body part
- **Part masking**: `hiddenMask` bitfield for armor/clothing visibility
- **Wetness**: Water line-based wetness blending
- **DARKEN=0.8** multiplier on final albedo

### Textures

| Slot | Content |
|------|---------|
| s0   | diffuseMap (albedo + alpha for gloss) |
| s1   | normalMap |
| s2   | colorMap (R=?, G=?, B=metalness) |

### Texture Formats

BC3_UNORM (1766), BC1_UNORM (1339), BC2_UNORM (397), B8G8R8A8_UNORM (292), R8G8B8A8_UNORM (2)

---

## character.hlsl — Full Characters

**12 permutations** across all captures. Full body + hair rendering with 13 texture inputs.

| Property      | Value |
|---------------|-------|
| Entry points  | `main_fs` (body), `hair_fs`, `distant_fs` (LOD), `zero_fs`, `severed_limb_fs` |
| Lines         | ~381 |
| Vertex strides | 24 (107 draws), 56 (19), 60 (5) |

### PS breakdown (Shark)

| Address      | Entry    | Draws |
|--------------|----------|------:|
| 0x66B37538   | main_fs  |    78 |
| 0x66B36EF8   | hair_fs  |    53 |

### Texture Inputs (13 total)

| Slot | Content |
|------|---------|
| s0   | Body diffuse |
| s1   | Body normal |
| s2   | Body color/mask |
| s3   | Body mask (glowy eyes in `headMask.g`) |
| s4   | Muscle blend normal |
| s5   | Head diffuse |
| s6   | Head normal |
| s7   | Head mask |
| s8   | Hair texture |
| s9   | Beard texture |
| s10  | Vest diffuse |
| s11  | Vest normal |
| s12  | Blood texture |

### Features

- **Skin tone**: Mask subtraction for racial variation
- **Hair/beard color**: Configurable channel blending
- **Dismemberment**: `severed_limb_fs` entry point
- **DARKEN=0.7** multiplier on final albedo

---

## foliage.hlsl — Grass & Trees

**12 permutations** across all captures. Billboard grass, tree leaves, farm plants.

| Property      | Value |
|---------------|-------|
| Entry points  | `grass_vs`/`grass_fs` (billboard), `foliage_vs`/`foliage_fs` (trees), `farm_vs`/`farm_shadow_vs` (farms) |
| Lines         | ~240 |
| Vertex stride | 24 (all draws) |

### GBuffer PS (Shark): `grass_fs` only (0x6162D878, 137 draws)

### Features

- **Grass**: Sine-wave wind animation, distance fade, hard alpha clip
- **No normal map**: Writes flat upward normal `(0, 1, 0)`
- **Farm plants**: `plantData[256]` instance array with per-plant rotation and death color
- **Tree leaves**: `foliage_fs` with normal mapping and alpha test

### Texture Formats

R16G16_FLOAT (548), BC1_UNORM (411), BC3_UNORM (137)

---

## triplanar.hlsl — Rocks & Cliffs

**20 permutations** across all captures. World-space UV projection.

| Property      | Value |
|---------------|-------|
| Entry points  | `triplanar_vs` / `triplanar_ps` |
| Lines         | ~159 |
| Vertex stride | 28 bytes |

### Features

- World-space UV divided by **5000** with `worldOffset` and `tileScale`
- **Power-2 axis weights** for smooth blending between projection planes
- `sampleTriplanarNormal()` correctly flips normals for opposite-facing sides

### Texture Formats

BC1_UNORM (18), BC3_UNORM (3), R16G16_FLOAT (3)

---

## creature.hlsl — NPCs & Animals

| Property      | Value |
|---------------|-------|
| Entry points  | `main_vs` / `main_fs` |
| Single PS     | 0x6162E9B8 (36 draws in Shark) |
| Vertex stride | 24 bytes |

### Texture Formats

BC3_UNORM (252), BC1_UNORM (144), B8G8R8A8_UNORM (72)

---

## distant_town.hlsl — Settlement LODs

| Property      | Value |
|---------------|-------|
| Entry points  | `main_vs` / `main_fs` |
| Single PS     | 0x6162D3B8 (3 draws in Shark) |
| Vertex stride | 36 bytes |

### Texture Formats

BC3_UNORM (21), BC1_UNORM (9), BC2_UNORM (6), B8G8R8A8_UNORM (3)

---

## Forward Pass Materials

These render after deferred lighting into the HDR buffer (R11G11B10_FLOAT).

| Material    | Source                           | Draws | Entry Points |
|-------------|----------------------------------|------:|--------------|
| Water       | `water.hlsl`                     |     1 | `waterVP` / `waterFP` |
| Water decor | `objects.hlsl` PS + `water.hlsl` VS | 16 | `main_ps` + `waterVP` |
| Fog         | `fog.hlsl`                       |    12 | `atmosphere_fog_fs`, `fog_planes_fs` |
| Basic       | `basic.hlsl`                     |   25+ | `basic_vp` / `basic_fp` |

---

## Construction Material

Source: `construction.hlsl` (~174 lines). Used for buildings under construction.
- Scaffold grid overlay blending
- Shadow caster variant for construction objects
- Shares instancing path with `objects.hlsl`
