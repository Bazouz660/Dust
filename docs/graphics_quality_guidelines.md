# Dust Graphics Quality Guidelines

## Core Principle

Every pixel must be correct every frame. No borrowing from previous frames, no reconstructing what wasn't rendered, no trading artifacts for performance. If a technique cannot produce a clean result in a single frame, it does not belong in Dust.

---

## No Temporal Accumulation

Temporal techniques reuse data from previous frames to amortize cost. They all share the same fundamental flaws:

- **Ghosting** — stale history trails behind moving objects
- **Smearing** — fast motion blends old and new data into mush
- **Disocclusion noise** — newly revealed pixels have no history, causing flicker or holes
- **Shimmer on first frames** — the image needs multiple frames to "converge," so cuts, camera snaps, and fast pans always look worse

**Banned techniques:**
- TAA (Temporal Anti-Aliasing)
- Temporal reprojection / accumulation for denoising (GI, AO, reflections, shadows)
- TSR, FSR 2, DLSS — temporal upscalers that reconstruct from history
- Any effect that blends current-frame output with a history buffer

**What to use instead:**
- More samples per pixel in a single frame
- Wider spatial filter kernels (bilateral, edge-aware, guided by depth/normals)
- Techniques that produce smooth output inherently (VXGI cone tracing vs. noisy ray marching)
- Accept the GPU cost of doing it right in one pass

This applies to every effect — GI, AO, shadows, reflections, all of it. If the raw output of an effect is too noisy without temporal accumulation, the technique itself is wrong for our use case. Pick a technique that produces clean single-frame output.

---

## Native Resolution Rendering

Render at the display's native resolution. Do not upscale.

- No DLSS, FSR, XeSS, or any reconstruction-based upscaler
- No render-at-half-res-and-upscale tricks
- If performance is a concern, optimize the effect or lower its quality settings — do not reconstruct pixels that were never rendered

Selective supersampling (rendering a specific pass at higher resolution and downsampling) is acceptable — it's the opposite of upscaling. Rendering shadow maps or GBuffer passes at 2x and downsampling produces strictly cleaner results with no artifacts.

---

## Anti-Aliasing

The industry implements post-process AA poorly. Most FXAA implementations use the wrong quality preset, making it blurrier than necessary. TAA smears the image and has been pushed far beyond its original conservative design.

**Preferred approach (in order):**

1. **SMAA (Subpixel Morphological AA)** — best single-frame post-process AA. Morphological edge detection handles both geometric and shading aliasing. Sharper than FXAA, no temporal component. Use the spatial-only mode (SMAA 1x), not the temporal variant (SMAA T2x).

2. **FXAA with correct configuration** — if used, must be set to the highest quality preset (Quality 39 / FXAA_QUALITY__PRESET 39). Lower presets are where the blurriness comes from. Properly configured FXAA is acceptable as a fallback.

3. **Geometry-level fixes** — alpha-to-coverage for vegetation and fences, proper mip chains for thin geometry. Fixing aliasing at the source is always better than fixing it in post.

4. **Selective supersampling** — render the GBuffer at higher resolution and downsample. Expensive but artifact-free. Use for specific problem passes, not the whole frame.

**Never use:**
- TAA in any form
- DLAA (it's temporal)
- Any AA that requires a history buffer

---

## Texture Quality & Filtering

Texture aliasing (shimmer at distance, crawling patterns on surfaces) is often worse than geometric aliasing and harder to fix in post.

**Mandatory:**
- **Anisotropic filtering x16** on all samplers. Override via CreateSamplerState hook or PSSetSamplers. No exceptions — every texture benefits from max aniso.

**Recommended:**
- **Conservative negative mip bias** — a small bias (-0.25 or less) sharpens distant textures. Without TAA to absorb the resulting micro-shimmer, keep it subtle. Make it configurable.
- **Higher quality mip generation** — if the engine uses box-filtered mip chains (common in OGRE), replace with Kaiser or Lanczos downsampling at CreateTexture2D time. Sharper mips with less banding.
- **Texture replacement pipeline** — once textures can be identified by hash at creation time, allow injection of higher-resolution replacements. Community texture packs become trivial.

---

## Lighting & Materials

Modern engines still default to Lambert diffuse — a 264-year-old model that makes everything look flat and plastic. Better models cost virtually nothing on modern hardware.

**BRDF requirements:**
- Replace Lambert diffuse with a physically-based diffuse model. Oren-Nayar for rough surfaces at minimum. Consider the Burley/Disney diffuse model for general use.
- GGX (Trowbridge-Reitz) for specular, with proper Fresnel (Schlick approximation) and geometric attenuation (Smith GGX).
- Per-material BRDF selection where possible — skin gets subsurface scattering, hair gets anisotropic Kajiya-Kay or Marschner, cloth gets sheen/velvet models, terrain gets roughness-driven diffuse.

**Implementation:**
- Patch the deferred lighting shader via D3DCompile hook to replace the BRDF
- Once per-material shading is available (via draw call classification), select BRDF per material ID in the GBuffer
- The BRDF change is essentially free — it's a few extra ALU ops in a shader that's already bound by bandwidth

---

## Shadows

Shadows must be stable, sharp at contact, and soft at distance — with no temporal flickering, no shimmer, and no peter-panning.

**Requirements:**
- Texel-stable projection (snap to shadow texel grid to eliminate sub-pixel jitter between frames)
- Contact-hardening penumbra (PCSS or equivalent — shadow sharpness varies with distance from occluder)
- Proper slope-scaled depth bias (no acne, no peter-panning)
- No temporal shadow filtering — spatial PCF/PCSS only

**For many-light scenarios:**
- No artificial light budget or cap. Every light source should be able to cast shadows.
- Use techniques that scale to unlimited lights: voxel occlusion queries, SDF ray marching, or (with DXR) hardware ray-traced shadow rays.
- One shared spatial data structure (voxel grid or SDF) serving all lights is preferred over per-light shadow maps with a budget.

---

## Global Illumination

GI must work in world space (not screen space) and produce clean output without temporal accumulation.

**Preferred techniques:**
- **VXGI (Voxel Global Illumination)** — voxelize scene geometry once per frame, trace cones through the voxel grid. Produces inherently smooth output. The same voxel grid serves both GI and shadow queries for unlimited lights.
- **RSM (Reflective Shadow Maps) + LPV (Light Propagation Volumes)** — inject light into a 3D grid, propagate. Smooth output, no denoising needed.

**Avoid:**
- Screen-space GI techniques that require temporal accumulation to converge (noisy ray marching with temporal blend)
- Any GI approach that needs 8+ frames to stabilize

If a spatial-only denoiser is needed, use edge-aware bilateral filtering guided by GBuffer depth and normals. The denoiser must not reference any previous frame.

---

## Denoising

When an effect produces noisy output, denoise spatially within the current frame only.

**Acceptable denoisers:**
- Bilateral filter (depth + normal edge-stopping)
- A-trous wavelet filter (multi-scale spatial, depth/normal-guided)
- Gaussian with edge-awareness
- Joint bilateral (guided by auxiliary buffers — depth, normals, albedo)

**Not acceptable:**
- Temporal accumulation with exponential moving average
- History buffer blending
- Motion-vector reprojection of previous results
- Any filter that reads from a buffer written in a previous frame

**Design rule:** if the raw (pre-denoise) output of an effect is so noisy that only temporal accumulation can clean it up, the effect's sample count or technique is wrong. Increase samples or pick a less noisy technique rather than papering over it with temporal blending.

---

## Performance Philosophy

Clean visuals are the priority. Performance is managed through:

1. **Quality settings** — let users choose sample counts, resolution multipliers, effect toggles
2. **Algorithmic efficiency** — better techniques that produce cleaner output with fewer samples, not temporal amortization
3. **Smart budgeting** — skip work that doesn't contribute (frustum culling for shadow geometry, early-out for fully lit/shadowed pixels)
4. **GPU-appropriate features** — more expensive techniques (VXGI, DXR) can be gated behind hardware capability checks

Never trade image quality for performance by adding temporal accumulation. If an effect is too expensive at acceptable quality, make it optional or find a better algorithm — don't make it cheap by making it wrong.

---

## Summary

| Principle | Do | Don't |
|-----------|-----|-------|
| Anti-aliasing | SMAA, properly configured FXAA, selective supersampling | TAA, DLAA, any temporal AA |
| Resolution | Native, optional supersampling for specific passes | Upscaling, reconstruction, DLSS/FSR |
| Denoising | Spatial bilateral/wavelet, edge-aware | Temporal accumulation, history buffers |
| Textures | Aniso x16, quality mip generation | Default trilinear, aggressive negative mip bias |
| Lighting | Oren-Nayar/Disney diffuse, GGX specular, per-material BRDF | Lambert diffuse, uniform shading model |
| Shadows | Stable CSM, PCSS, voxel/SDF for many lights | Temporal shadow filtering, per-light budget caps |
| GI | VXGI, RSM+LPV, world-space techniques | Screen-space with temporal convergence |
| Performance | Quality settings, better algorithms, smart culling | Temporal amortization, upscaling, cutting corners |

*Aligned with [Threat Interactive](https://www.threat-interactive.com)'s advocacy for higher graphics standards — native rendering, correct BRDFs, proper anti-aliasing, and rejecting the industry's over-reliance on temporal reconstruction.*
