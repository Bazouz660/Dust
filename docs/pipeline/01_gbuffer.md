# GBuffer Layout & Encoding

Three render targets + hardware depth, all at native resolution.

## Render Target Layout

| MRT Slot | Format            | Content                                                     |
|----------|-------------------|-------------------------------------------------------------|
| RT0      | B8G8R8A8_UNORM    | R=Luma(Y), G=Chroma(Co/Cg checkerboard), B=Metalness, A=Gloss |
| RT1      | B8G8R8A8_UNORM    | RGB=Normal (encoded: n*0.5+0.5), A=Emissive/Translucency   |
| RT2      | R32_FLOAT         | Linear depth (distance / farClip)                           |
| DS       | D24_UNORM_S8_UINT | Hardware depth + stencil                                    |

## Albedo Encoding: YCoCg Chroma Subsampling

Kenshi uses YCoCg color space with checkerboard chroma subsampling to pack metalness
into the GBuffer without adding a fourth render target:

- RGB albedo is converted to YCoCg: **Y (luma)** stored at full resolution in **RT0.R**
- Co and Cg chrominance stored at **half resolution** using a checkerboard pattern in **RT0.G**:
  - On even pixels `(x%2 == y%2)`: G stores **Cg**
  - On odd pixels: G stores **Co**
- **Metalness** gets the freed RT0.B channel at full 8-bit precision
- **Gloss** gets the RT0.A channel at full 8-bit precision

Source: `data/materials/deferred/gbuffer.hlsl`

### Decoding

The deferred pass uses edge-directed chroma reconstruction (`decodePixel` in `gbuffer.hlsl`)
that samples 4 neighbors and uses luminance-weighted averaging to reconstruct the missing
chrominance channel. This minimizes color bleeding at sharp edges.

## Normal Encoding (RT1.RGB)

Simple hemisphere encoding:
```hlsl
RT1.rgb = normal * 0.5 + 0.5;
```
Decode: `normal = RT1.rgb * 2.0 - 1.0`

No octahedron or spherical encoding — just scaled world-space normals using the [0,1] range.

## Emissive / Translucency Encoding (RT1.A)

RT1.A dual-encodes two mutually exclusive values:

| RT1.A Range | Interpretation                        | Decoded Range |
|-------------|---------------------------------------|---------------|
| 0.0 - 0.5  | `translucency = value * 2.0`          | 0.0 - 1.0     |
| 0.5 - 1.0  | `emissive = (value - 0.5) * 6.4`     | 0.0 - 3.2     |

## GBuffer Write Functions

```hlsl
writeAlbedo(buffer, rgb, screenPos)    // -> RT0.RG (YCoCg encoded, checkerboard)
writeMetalness(buffer, value)          // -> RT0.B
writeGloss(buffer, value)              // -> RT0.A
writeNormal(buffer, normal)            // -> RT1.RGB (normal * 0.5 + 0.5)
writeDepth(buffer, dist/farClip)       // -> RT2.R
```

## Material Data Flow

How materials write to the GBuffer (example from `skin.hlsl`):

| GBuffer Channel | Material Input                                       |
|-----------------|------------------------------------------------------|
| Albedo (Y/CoCg) | `diffuseTexture.rgb` (after skin tone / blood tint) |
| Metalness        | `colorMap.b` (blue channel of mask texture)         |
| Gloss            | `diffuseTexture.a * glossMult` (alpha * uniform)    |
| Normal           | Tangent-space normal map, transformed to world       |
| Depth            | `length(viewPos) / farClip`                          |

## Draw Statistics

| Metric               | Value                   |
|----------------------|-------------------------|
| Draws per frame       | 176-1,388 (avg 545)   |
| Unique PS in frame    | 34 (Shark), 91 across all captures |
| Unique VS in frame    | 16 (Shark)             |
| Vertex strides        | 24 (most common), 40, 56, 60, 64 bytes |
| Material types        | 8 (objects, skin, foliage, character, terrain, creature, triplanar, distant_town) |

## GBuffer Repack Pass (Draws 3645-3647)

After stencil mask generation, a GBuffer repack pass converts the packed GBuffer into
R16G16B16A16_FLOAT for the deferred lighting pass. This unpacks the YCoCg encoding and
prepares the data for lighting.

| Property      | Value                              |
|---------------|------------------------------------|
| Source         | `gbuffer_repack.hlsl`             |
| Entry point    | `repack_ps` (3 variants)          |
| Output RT      | R16G16B16A16_FLOAT native         |
| Draws          | 3 (always, in all captures)       |

## Instancing Potential

From GBuffer analysis (Shark capture, 1177 draws):
- **53% draw call reduction** possible through instancing
- 147 unique mesh signatures (VS + PS + stride + indexCount)
- Top repeater: 44 instances of same mesh
- Multiple mesh groups with 20-36 identical draws
