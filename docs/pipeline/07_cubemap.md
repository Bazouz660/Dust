# Cubemap / Reflection Probe Pass

Renders the scene into a 512x512 cubemap for environment reflections.
Not always present — depends on whether a reflection probe is near the camera.

## Configuration

| Property        | Value |
|-----------------|-------|
| Resolution      | 512x512 |
| Color format    | B8G8R8A8_UNORM |
| Depth format    | D32_FLOAT |
| Draw range      | 7-1406 (Shark capture) |
| Draws           | 0-1,400 (varies by probe proximity) |
| Present in      | Some captures (depends on scene) |

## Material Breakdown (Shark, 1400 draws)

| Material              | Draws | % of cubemap |
|-----------------------|------:|-----------:|
| GEOMETRY_OBJECT       |   464 |      33.1% |
| GEOMETRY_TERRAIN      |   312 |      22.3% |
| GEOMETRY_SKIN         |   292 |      20.9% |
| GEOMETRY_FOLIAGE      |   153 |      10.9% |
| GEOMETRY_CHARACTER    |   131 |       9.4% |
| GEOMETRY_CREATURE     |    37 |       2.6% |
| GEOMETRY_TRIPLANAR    |     4 |       0.3% |
| GEOMETRY_DISTANT_TOWN |     3 |       0.2% |
| SKY_MOON              |     2 |       0.1% |
| SKY_DOME              |     1 |       0.1% |
| SKY_CLOUDS            |     1 |       0.1% |

## Shader Sharing with GBuffer

**34 of 37** pixel shaders used in the cubemap pass are shared with the GBuffer pass.
The same material shaders render at lower resolution into the cubemap.

### Cubemap-Only Shaders (3)

| Address    | Source              | Entry Point | Purpose |
|------------|---------------------|-------------|---------|
| 0x638501B8 | moon.hlsl           | main_fs     | Moon in reflection |
| 0x63926938 | SkyX_Skydome.hlsl   | main_fp     | Sky dome in reflection |
| 0x63926C78 | SkyX_Clouds.hlsl    | main_fp     | Clouds in reflection |

These are sky elements that don't write to the GBuffer but do appear in cubemap reflections.

## Output Usage

The cubemap output feeds into the deferred lighting pass as IBL (Image-Based Lighting):

| SRV Slot | Format         | Size    | Usage |
|----------|----------------|---------|-------|
| s6       | BC3_UNORM CUBE | 16x16   | Irradiance cubemap (diffuse IBL) |
| s7       | BC3_UNORM CUBE | 256x256 | Specular cubemap (specular IBL) |

The 512x512 rendered cubemap is downsampled and filtered into these two cubemaps:
- **Irradiance** (s6): Heavily filtered to 16x16, sampled at mip 3, decoded `rgb * a * 4.0`
- **Specular** (s7): Mip-mapped to 256x256, mip level = `(1-gloss) * 7.0`, decoded `rgb * a * 10.0`

## Performance Impact

The cubemap pass can be expensive — in Shark (densest capture), it uses 1,400 draws,
which is more than the main GBuffer pass (1,177 draws). The entire scene is re-rendered
at 512x512 for the reflection probe.

When no probe is nearby, the pass is skipped entirely (0 draws in Ashlands capture).

## Draw Order

The cubemap pass renders at the **very start** of the frame (draws 7-1406), before
the main GBuffer fill. This ensures updated reflections are available for the current
frame's deferred lighting.

```
[0-6]      Heat haze (previous frame)
[7-1406]   Cubemap pass         <-- Full scene at 512x512
[1407+]    Main GBuffer fill    <-- Uses cubemap for IBL
```
