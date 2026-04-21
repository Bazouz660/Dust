# Post-Processing Chain

Every post-processing step identified with exact draw indices, shader sources, and render targets.

## Chain Overview

| Step                  | Draws | Range       | RT Format                    | Source            | Entry Point |
|-----------------------|------:|-------------|------------------------------|-------------------|-------------|
| Stencil mask          |     3 | 3642-3644   | R8_UNORM native              | stencil_mask.hlsl | stencil_ps  |
| GBuffer repack        |     3 | 3645-3647   | R16G16B16A16_FLOAT native    | gbuffer_repack.hlsl | repack_ps |
| Luminance measurement |     7 | 3709-3715   | R32_FLOAT 128->64->16->4->1  | hdrfp4.hlsl       | (multiple)  |
| Bloom extract + blur  |     2 | 3716-3717   | R11G11B10_FLOAT 640x360      | hdrfp4.hlsl       | (multiple)  |
| Tonemapping composite |     1 | 3718        | B8G8R8A8_UNORM native        | hdrfp4.hlsl       | Composite_fp4 |
| Luminance adapt       |     1 | 3719        | R32_FLOAT 256x256            | hdrfp4.hlsl       | luminance_ps |
| LDR post (4 passes)   |     4 | 3720-3723   | B8G8R8A8_UNORM native        | post_ldr.hlsl     | post_ps     |
| DOF CoC               |     1 | 3724        | R16_FLOAT native             | DepthBlur.hlsl    | dof_ps      |
| Half-res blur         |     3 | 3725-3727   | B8G8R8A8_UNORM 1280x720      | hdrfp4.hlsl       | halfres_ps  |
| LDR composite         |     1 | 3728        | B8G8R8A8_UNORM native        | post_ldr.hlsl     | post_ps     |
| Bloom (down 5 + up 4) |     9 | 3729-3737   | R11G11B10_FLOAT 1280->80     | hdrfp4.hlsl       | bloom_ps    |
| LDR final composite   |     1 | 3738        | B8G8R8A8_UNORM native        | post_ldr.hlsl     | post_ps     |
| FXAA                  |     1 | 3739        | B8G8R8A8_UNORM native        | FXAA.hlsl         | main        |
| Heat haze             |     1 | 3740        | R8G8B8A8_UNORM native        | heathaze.hlsl     | main        |
| FXAA (UI/final)       |    22 | 3741-3762   | R8G8B8A8_UNORM native        | FXAA.hlsl         | fxaa_ps     |

---

## Stencil Mask (Draws 3642-3644)

Source: `stencil_mask.hlsl`

Generates a stencil mask for the deferred lighting pass. 3 variants (3 unique PS addresses):

| Draw | PS Address  |
|------|-------------|
| 3642 | 0x6394FA38  |
| 3643 | 0x6394FBB8  |
| 3644 | 0x63950378  |

Output: R8_UNORM native resolution. Pre-compiled shader (not from D3DCompile hook).

---

## GBuffer Repack (Draws 3645-3647)

Source: `gbuffer_repack.hlsl`

Converts the packed YCoCg GBuffer into R16G16B16A16_FLOAT for deferred lighting.
3 variants (3 unique PS addresses):

| Draw | PS Address  |
|------|-------------|
| 3645 | 0x63DB40F8  |
| 3646 | 0x63DB42B8  |
| 3647 | 0x63DB3C78  |

Pre-compiled shader. Always exactly 3 draws in every capture.

---

## Luminance Measurement (Draws 3709-3715)

Source: `hdrfp4.hlsl` (~255 lines)

Progressive downsampling to compute average scene luminance:

| Draw | Entry Point                    | Output RT           |
|------|--------------------------------|---------------------|
| 3709 | `Downsample2x2Luminance_fp4`  | R32_FLOAT 128x128   |
| 3710 | `Downsample3x3_fp4`           | R32_FLOAT 64x64     |
| 3711 | `Downsample3x3_fp4`           | R32_FLOAT 16x16     |
| 3712 | `Downsample3x3_fp4`           | R32_FLOAT 4x4       |
| 3713 | `Downsample3x3_fp4`           | R32_FLOAT 1x1       |
| 3714 | `AdaptLuminance_fp4`          | R32_FLOAT 1x1       |
| 3715 | `Copy_fp4`                    | R32_FLOAT 1x1       |

Chain: Native HDR -> 128 -> 64 -> 16 -> 4 -> 1 pixel.
`AdaptLuminance_fp4` blends current and previous frame for smooth adaptation.

---

## Bloom

### Initial Extract + Blur (Draws 3716-3717)

| Draw | Entry Point                     | Output RT                 |
|------|---------------------------------|---------------------------|
| 3716 | `Downsample3x3Brightpass_fp4`  | R11G11B10_FLOAT 640x360   |
| 3717 | `BloomBlurV_fp4`               | R11G11B10_FLOAT 640x360   |

Bright pass extracts pixels above luminance threshold, then vertical blur.

### Bloom Pyramid (Draws 3729-3737)

Downsample chain followed by upsample chain:

| Draw | Entry Point | Direction | Output RT                 |
|------|-------------|-----------|---------------------------|
| 3729 | `bloom_ps`  | Copy      | R11G11B10_FLOAT 1280x720  |
| 3730 | `bloom_ps`  | Down      | R11G11B10_FLOAT 640x360   |
| 3731 | `bloom_ps`  | Down      | R11G11B10_FLOAT 320x180   |
| 3732 | `bloom_ps`  | Down      | R11G11B10_FLOAT 160x90    |
| 3733 | `bloom_ps`  | Down      | R11G11B10_FLOAT 80x45     |
| 3734 | `bloom_ps`  | Up        | R11G11B10_FLOAT 160x90    |
| 3735 | `bloom_ps`  | Up        | R11G11B10_FLOAT 320x180   |
| 3736 | `bloom_ps`  | Up        | R11G11B10_FLOAT 640x360   |
| 3737 | `bloom_ps`  | Up        | R11G11B10_FLOAT 1280x720  |

5 downsample + 4 upsample steps. Two distinct PS addresses:
- Down: 0x63DB3478 (5 draws)
- Up: 0x63DB59F8 (4 draws)

---

## Tonemapping / Composite (Draw 3718)

| Property     | Value |
|--------------|-------|
| PS           | 0x644474F8, entry: `Composite_fp4` |
| VS           | `quad_vp.hlsl` (fullscreen quad) |
| Output       | B8G8R8A8_UNORM native (LDR) |

Converts HDR (R11G11B10_FLOAT) to LDR (B8G8R8A8_UNORM) using adapted luminance.
This is the HDR->LDR transition point in the pipeline.

SRV inputs: HDR scene, luminance (R32_FLOAT 1x1), bloom (R11G11B10_FLOAT 640x360).

---

## Luminance Adapt for Next Frame (Draw 3719)

| Property     | Value |
|--------------|-------|
| PS           | 0x63DB50B8, entry: `luminance_ps` |
| Output       | R32_FLOAT 256x256 |

Stores adapted luminance for the next frame's auto-exposure.

---

## LDR Post-Processing (6 total draws)

Source: `post_ldr.hlsl` (pre-compiled)

| Draw | PS Address  | Notes |
|------|-------------|-------|
| 3720 | 0x63DB4A78  | LDR post pass 1 |
| 3721 | 0x63DB4BF8  | LDR post pass 2 |
| 3722 | 0x63DB64F8  | LDR post pass 3 |
| 3723 | 0x63DB4D78  | LDR post pass 4 |
| 3728 | 0x63DB32F8  | LDR composite (after DOF) |
| 3738 | 0x63DB3AB8  | LDR final composite (after bloom) |

All output to B8G8R8A8_UNORM native. 6 unique PS addresses — each pass is a different
post-processing operation (color grading, vignette, etc.).

---

## DOF — Circle of Confusion (Draw 3724)

Source: `DepthBlur.hlsl`

| Property     | Value |
|--------------|-------|
| PS           | 0x63DB6CB8, entry: `dof_ps` |
| Output       | R16_FLOAT native (single channel CoC) |

Computes per-pixel circle of confusion from depth buffer.

---

## Half-Resolution Blur (Draws 3725-3727)

Source: `hdrfp4.hlsl`

| Draw | PS Address  | Entry Point |
|------|-------------|-------------|
| 3725 | 0x63DB6378  | halfres_ps  |
| 3726 | 0x63DB5238  | halfres_ps  |
| 3727 | 0x63DB2FF8  | halfres_ps  |

Output: B8G8R8A8_UNORM 1280x720 (half native resolution).
Used for DOF blur application at half resolution for performance.

---

## FXAA (Draws 3739 + 3741-3762)

Source: `FXAA.hlsl`

| Group       | Draws | PS Entry  | Output RT |
|-------------|------:|-----------|-----------|
| Initial     |     1 | `main`    | B8G8R8A8_UNORM native |
| UI/overlays |    22 | `fxaa_ps` | R8G8B8A8_UNORM native |

The initial FXAA pass (draw 3739) uses `main` entry point.
The 22 subsequent passes use `fxaa_ps` entry and a different PS address (0x617F6738).
These appear to be per-UI-element anti-aliasing.

---

## Heat Haze

Source: `heathaze.hlsl`

### Previous Frame Heat Haze (Draws 0-6)

| Draws | PS Address  | Entry Point    | Output RT |
|------:|-------------|----------------|-----------|
|     7 | 0x63DB6038  | heathaze_ps    | R8G8B8A8_UNORM native |

Renders heat distortion meshes from the **previous frame** into a distortion buffer.
These are DrawIndexed calls with varying index counts (geometry-based, not fullscreen).

### Current Frame Heat Haze (Draw 3740)

| Draw | PS Address  | Entry Point | Output RT |
|------|-------------|-------------|-----------|
| 3740 | 0x64447B38  | main        | R8G8B8A8_UNORM native |

Fullscreen composite that applies the heat haze distortion to the final image.

---

## Fullscreen Quad VS

All post-processing passes share `quad_vp.hlsl` as the vertex shader:

| Address      | Draws | Used by |
|--------------|------:|---------|
| 0x63DB45B8   |    17 | Deferred sun, GBuffer repack, stencil mask, light volumes |
| 0x6394F578   |    14 | Bloom, DOF, half-res, LDR post, FXAA, luminance, tonemap |

Two VS variants, both fullscreen quad generation. 12 total permutations of `quad_vp.hlsl`.
