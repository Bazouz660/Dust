# Kenshi Rendering Pipeline Analysis

Reverse-engineered from RenderDoc captures of Kenshi (D3D11, OGRE engine).
Date: 2026-04-07
Updated: 2026-04-09

> This document describes the raw pipeline as seen in RenderDoc. For the Dust framework's injection point system and plugin API built on top of this analysis, see the main [README](../README.md) and [`src/DustAPI.h`](../src/DustAPI.h).

## Overview

- **Graphics API**: Direct3D 11, Feature Level 11_0
- **Engine**: OGRE (Object-Oriented Graphics Rendering Engine)
- **Rendering**: Deferred shading with forward pass for sky/particles
- **Resolution**: Native (tested at 2560x1440)
- **Depth format**: D24_UNORM_S8_UINT (hardware), R32_FLOAT (linear, in GBuffer)

## Frame Structure

```
EID 0        Frame start
EID 13       ClearRenderTargetView (black)
EID 14       ClearDepthStencilView (1.0, 0)
EID 15-N     Colour Pass #1 — GBuffer fill (3 MRT + Depth)
EID N+1-M    Colour Pass #2 — Deferred composition (1 RT + Depth)
               ├── Sky / atmosphere (DrawIndexed, forward, already fogged)
               ├── Deferred Lighting (fullscreen Draw)
               ├── Forward objects (DrawIndexed — particles, transparent, etc.)
               ├── Fog / Atmospheric scattering (fullscreen Draw)
               ├── Luminance downscale chain (5 passes)
               ├── Auto-exposure adaptation
               ├── Bloom threshold + extract
               ├── Bloom blur (Gaussian)
               ├── Tone mapping + bloom composite
               ├── FXAA
               └── Heat haze / distortion → backbuffer
EID M+1      Present
```

Note: EID numbers shift between frames depending on object count, camera position, LOD, etc.
Passes must be identified by **shader hash**, not by EID.

---

## GBuffer Layout (Colour Pass #1)

Three render targets + hardware depth, all at native resolution.

| MRT Slot | Format            | Resource ID | Content              | Shader Name |
|----------|-------------------|-------------|----------------------|-------------|
| RT0      | B8G8R8A8_UNORM    | 1040        | Albedo / Diffuse     | gBuf0       |
| RT1      | B8G8R8A8_UNORM    | 1043        | Normals              | gBuf1       |
| RT2      | R32_FLOAT         | 1046        | Linear Depth         | gBuf2       |
| DS       | D24_UNORM_S8_UINT | 338         | Hardware Depth       | —           |

Note: Resource IDs are stable within a session (created at init), but may differ between game launches.

---

## Pass Details

### Pass 0: Deferred Lighting

The main deferred lighting composition pass. Reads the full GBuffer and outputs the lit scene **without fog**.

| Property        | Value |
|-----------------|-------|
| **Type**        | Fullscreen quad — Draw(4) |
| **PS Hash**     | `3b5a62cd-b2ae32e2-ac920923-3005c021` |
| **PS Resource** | ResourceId::549 |
| **VS Resource** | ResourceId::543 |
| **Output RT**   | ResourceId::1049 — R11G11B10_FLOAT (HDR) |
| **Depth**       | Test=ON, Write=OFF |
| **Blend**       | OFF |

**Inputs (SRVs):**
| Slot | Resource | Format          | Name            |
|------|----------|-----------------|-----------------|
| 0    | 1040     | B8G8R8A8_UNORM  | gBuf0 (albedo)  |
| 1    | 1043     | B8G8R8A8_UNORM  | gBuf1 (normals) |
| 2    | 1046     | R32_FLOAT       | gBuf2 (depth)   |
| 3    | 2449     | B8G8R8A8_UNORM  | ambientMap (1024x1024) |
| 4    | 1013     | BC3_UNORM       | irradianceCube (16x16) |
| 5    | 2451     | BC3_UNORM       | specularityCube (256x256) |

**Constant Buffer (CB0, 272 bytes):**
| Name          | Value (example)                            | Notes |
|---------------|--------------------------------------------|-------|
| sunDirection  | 0.933, 0.212, 0.292                        | World-space sun vector |
| sunColour     | 1.101, 0.921, 0.789, 0.606                | HDR sun color + intensity |
| pFogParams    | 50000.0, 3000.0, 30000.0, 0.0             | farClip, fogStart(?), fogEnd(?), unused |
| envColour     | 1.0, 1.0, 1.0, 1.0                        | Environment tint |
| worldSize     | 0.000003, 0.000003, 0.500, 0.500          | UV scale / offset |
| worldOffset   | -13223.0, 0.0, -16635.7                   | World position offset |
| viewport      | 2560.0, 1440.0, 0.000391, 0.000694        | Resolution + texel size |
| offset        | 0.0, 0.0, 0.0, 0.0                        | UV offset |
| inverseView   | 4x4 matrix                                 | Inverse view matrix |
| ambientParams | 0.89, 0.66, 0.16, 1.0                     | Ambient light color/intensity |
| proj          | 4x4 matrix                                 | Projection matrix |

---

### Pass 1: Fog / Atmospheric Scattering

Reads depth buffer and applies atmospheric scattering + fog onto the HDR target. Uses Rayleigh/Mie scattering model.

| Property        | Value |
|-----------------|-------|
| **Type**        | Fullscreen — Draw(3) or Draw(4) (varies) |
| **PS Hash**     | `0940e67e-b6ec3a82-91941e1d-a8d81d79` |
| **PS Resource** | ResourceId::625 |
| **VS Resource** | ResourceId::543 |
| **Output RT**   | ResourceId::1049 — R11G11B10_FLOAT (same HDR target) |
| **Depth**       | Test=OFF, Write=TRUE |
| **Blend**       | **ON** — SrcAlpha / InvSrcAlpha / Add (src=6, dst=7, op=0) |

**Inputs (SRVs):**
| Slot | Resource | Format    | Name             |
|------|----------|-----------|------------------|
| 0    | 1046     | R32_FLOAT | gBuf2 (depth)    |

**Constant Buffer (CB0, 448 bytes) — Atmospheric Scattering Parameters:**
| Name                  | Value (example)               | Notes |
|-----------------------|-------------------------------|-------|
| sunDirectionReal      | 0.933, 0.212, 0.292           | |
| pFogParams            | 50000.0, 3000.0, 30000.0, 0.0 | |
| uCameraPos            | 0.0, 9.78, 0.0               | Camera in atmosphere space |
| uInvWaveLength        | 9.47, 18.84, 26.68           | 1/λ^4 for RGB Rayleigh scattering |
| uInnerRadius          | 9.775                         | Planet inner radius (atmosphere units) |
| uExposure             | 1.4                           | HDR exposure for atmosphere |
| uKrESun               | 0.066                         | Rayleigh * sun brightness |
| uKr4PI                | 0.02765                       | Rayleigh * 4π |
| uKm4PI                | 0.00848                       | Mie * 4π |
| uScale                | 1.918                         | 1 / (outerRadius - innerRadius) |
| uScaleDepth           | 0.261                         | Scale height ratio |
| uScaleOverScaleDepth  | 7.360                         | uScale / uScaleDepth |
| uSkydomeRadius        | 70000.0                       | Skydome radius in world units |
| horizonClouds         | 1.183, 1.183, 1.183, 0.0     | |
| mat0/mat1/mat2        | 3x4 matrix rows               | Inverse view matrix |
| camera                | 110.2, 1261.5, -68.9          | Camera world position |
| farClip               | 50000.0                       | |
| fogColour             | 0.914, 0.796, 0.620, 1.0     | Fog color (sandy/brown) |
| fogDensity            | 0.00005                       | Exponential fog density |
| sunColour             | 1.101, 0.921, 0.789, 0.606   | |

**Fog model**: Exponential fog combined with Rayleigh/Mie atmospheric scattering.
The shader reads depth, reconstructs world position, and computes atmospheric scattering along the view ray.

---

### Passes 2-6: Luminance Downscale Chain

Progressive downscale of the HDR scene to compute average luminance for auto-exposure.

| Pass | Output Size | Input         | Shader Hash |
|------|-------------|---------------|-------------|
| #2   | 128x128     | 1049 (HDR)    | `5804af9e` (initial luminance) |
| #3   | 64x64       | 128x128       | `4c9d5661` (3x3 box filter) |
| #4   | 16x16       | 64x64         | `4c9d5661` |
| #5   | 4x4         | 16x16         | `4c9d5661` |
| #6   | 1x1         | 4x4           | `4c9d5661` |

- Format: R32_FLOAT throughout
- Initial pass converts RGB to luminance using `dot(rgb, float3(0.299, 0.587, 0.114))`
- Downscale uses 3x3 box filter (9 samples, multiply by 1/9)

---

### Pass 7: Auto-Exposure Adaptation

Temporal adaptation of exposure based on current and previous frame luminance.

| Property    | Value |
|-------------|-------|
| **PS Hash** | `66085753-0571ed62-f7767360-c93264ae` |
| **Output**  | 1x1 R32_FLOAT (ResourceId::1067) |
| **Inputs**  | Current luminance (1052), Previous adapted luminance (1070) |

**Parameters:**
| Name                      | Value |
|---------------------------|-------|
| frameTime                 | 0.041 (≈24fps) |
| MIN_LUMINANCE             | 0.8   |
| MAX_LUMINANCE             | 1.2   |
| BLOOM_MAGNITUDE           | 0.168 |
| AUTOEXP_ADAPTATION_RATE   | 0.5   |

Formula: `adaptedLum = lerp(prevLum, currentLum, 1 - exp(-dt * rate))`

---

### Pass 8: Luminance Copy

Simple copy of adapted luminance to a second 1x1 texture (for next frame's temporal adaptation).

| Property    | Value |
|-------------|-------|
| **PS Hash** | `d571a71a-e5bdf66f-8bde545f-9f38259f` |
| **Input**   | ResourceId::1067 (adapted luminance) |
| **Output**  | ResourceId::1070 (stored for next frame) |

---

### Pass 9: Bloom Threshold + Extract

Extracts bright areas above threshold from the HDR scene, downscaled to 1/4 resolution.

| Property    | Value |
|-------------|-------|
| **PS Hash** | `cd7b34ff-9c8983c7-ba3e92c1-3e6f7fc5` |
| **Output**  | 640x360 R11G11B10_FLOAT (ResourceId::1073) |
| **Inputs**  | HDR scene (1049), Adapted luminance (1067) |

**Parameters:**
| Name            | Value |
|-----------------|-------|
| BLOOM_THRESHOLD | 6.0   |
| EXPOSURE_KEY    | 0.55  |

---

### Pass 10: Bloom Blur

Gaussian blur of the bloom texture. Single-pass 13-tap vertical blur with σ=0.8.

| Property    | Value |
|-------------|-------|
| **PS Hash** | `a57c90ea-64dd3d95-112a24c8-651500ac` |
| **Output**  | 640x360 R11G11B10_FLOAT (ResourceId::1076) |
| **Input**   | Bloom threshold (1073) |

**Parameters:**
| Name              | Value |
|-------------------|-------|
| BLOOM_BLUR_SIGMA  | 0.8   |

---

### Pass 11: Tone Mapping + Bloom Composite

Combines HDR scene with bloom and applies exposure-based tone mapping. Output is LDR.

| Property    | Value |
|-------------|-------|
| **PS Hash** | `394bc282-2fde4470-2314d37f-784780ba` |
| **Output**  | 2560x1440 B8G8R8A8_UNORM (ResourceId::1085) |
| **Inputs**  | HDR scene (1049), Bloom (1076), Adapted luminance (1067) |

**Parameters:**
| Name            | Value |
|-----------------|-------|
| BLOOM_MAGNITUDE | 0.0 (can be > 0) |
| EXPOSURE_KEY    | 0.55 |

Formula: `output = exposure * hdrColor + bloom * bloomMagnitude`

---

### Pass 12: FXAA

Fast approximate anti-aliasing on the LDR output.

| Property    | Value |
|-------------|-------|
| **PS Hash** | `5fac176d-5ec57a9b-b12eeef1-ae924b3b` |
| **Output**  | 2560x1440 B8G8R8A8_UNORM (ResourceId::1088) |
| **Input**   | Tone-mapped scene (1085) |

Standard FXAA 3.11 implementation (195 lines of shader assembly).

---

### Pass 13: Heat Haze / Distortion → Backbuffer

Final post-process: animated heat haze distortion effect. Writes to the swap chain backbuffer.

| Property    | Value |
|-------------|-------|
| **PS Hash** | `6acb8d7b-771f7455-3209ad05-59009cd6` |
| **Output**  | 2560x1440 R8G8B8A8_UNORM (ResourceId::327 — swap chain) |
| **Inputs**  | Flow map (2975), Perturbation (2977), Depth (1046), FXAA output (1088) |

**Parameters:**
| Name     | Value  | Notes |
|----------|--------|-------|
| gameTime | 0.1146 | Animated distortion |
| heatHaze | 0.0    | Intensity (0 = disabled) |

---

## Injection Points for Graphics Enhancements

Dust defines injection points as `DustInjectionPoint` values in `DustAPI.h`. Effects register at a point and receive pre/post callbacks around the game's draw call. Multiple effects at the same injection point are dispatched in ascending `priority` order.

### Currently implemented

| Injection Point | Pipeline Location              | Available Resources                    | Current Effects             |
|-----------------|--------------------------------|----------------------------------------|-----------------------------|
| `POST_LIGHTING` | Around deferred lighting draw  | Depth SRV, albedo SRV, normals SRV, HDR RTV | SSAO (pri 0), SSIL (pri 10), SSS (pri 20) |
| `POST_TONEMAP`  | Around tone mapping draw       | HDR RTV (pre), LDR RTV (post)          | LUT (pri 0), Bloom (pri 100)|

### SSAO — ambient-only occlusion (`POST_LIGHTING`, priority 0)

Runs in `preExecute`: generates an R8_UNORM AO texture from the depth and normal buffers using GTAO (12 directions, 6 steps), blurs it with a depth-aware bilateral filter, then binds it to shader register 8 before the game's lighting draw executes. The deferred lighting shader is patched in memory at startup to read this AO map and apply it only to indirect/ambient light, not direct sunlight.

### SSIL — screen-space indirect lighting (`POST_LIGHTING`, priority 10)

Runs in `preExecute`: samples albedo and depth in 8 directions × 4 steps per pixel to accumulate albedo-weighted indirect light into an R11G11B10_FLOAT texture, blurs it with a bilateral filter, then stores the result. Runs in `postExecute`: additively blends the IL texture onto the HDR render target after the game's lighting draw. Composited before fog so indirect light is naturally attenuated at distance.

### LUT — color grading + tonemapping (`POST_TONEMAP`, priority 0)

Runs in `preExecute`: captures a copy of the R11G11B10_FLOAT HDR render target before the game's tonemapper executes via `host->GetSceneCopy("hdr_rt")`. Runs in `postExecute`: overwrites the LDR output entirely — applies exposure, ACES filmic tonemapping, a float-precision 32³ LUT, and triangular dithering in a single pass. This is the only 8-bit quantization step in the chain, eliminating the double-quantization banding of the vanilla pipeline.

### SSS — screen-space contact shadows (`POST_LIGHTING`, priority 20)

Runs in `preExecute`: extracts the sun direction and inverse view matrix from the game's PS constant buffer (CB0) — `sunDirection` at register c0, `inverseView` at c8–c11 — and computes a view-space light direction. Runs in `postExecute`: ray marches each pixel toward the sun in view space using the depth buffer with quadratic step distribution and per-pixel jitter, blurs the shadow mask with a bilateral filter, then composites multiplicatively onto the HDR render target. Adds sharp contact shadows that complement the game's low-resolution shadow map.

### Bloom — physically-motivated bloom (`POST_TONEMAP`, priority 100)

Runs after LUT so bloom is applied to graded colors. Threshold-extracts bright areas from the LDR scene, progressively downsamples, upsamples with scatter control, and composites additively.

### Available injection points

| Injection Point | Pipeline Location              | Potential Future Effects               |
|-----------------|--------------------------------|----------------------------------------|
| `POST_GBUFFER`  | After GBuffer fill             | (reserved)                             |
| `POST_FOG`      | After fog/atmosphere           | SSR                                    |
| `PRE_PRESENT`   | After all post-processing      | Debug overlays, capture                |

---

## Identification Strategy

Since EIDs change per frame, passes must be identified by **stable signatures**:

1. **Pixel Shader hash** (most reliable) — compiled shader bytecode hash, never changes
2. **Render target format + resolution** — e.g., R11G11B10_FLOAT at native res
3. **SRV binding pattern** — which textures are bound to which slots
4. **Blend state** — fog uses SrcAlpha/InvSrcAlpha, lighting uses no blend

Recommended: Hook `PSSetShader` to track current PS, then in `Draw`/`DrawIndexed` check if the current PS matches a known hash.
