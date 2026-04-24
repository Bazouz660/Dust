# Subsurface Scattering Research for Kenshi's Renderer

## 1. What is Subsurface Scattering (SSS)

Subsurface scattering is the physical phenomenon where light penetrates a translucent surface, scatters inside the material, and exits at a different point. In skin, light enters, bounces between tissue layers (epidermis, dermis, subcutaneous fat), gets partially absorbed, and re-emerges. This creates several visible effects:

- **Soft light falloff at terminators** — the shadow-to-light transition on skin is much softer and warmer than on hard surfaces. Light "wraps around" the form.
- **Color shift** — scattered light shifts red because hemoglobin and melanin absorb blue/green wavelengths more than red. The deeper light travels, the redder it gets.
- **High-frequency detail softening** — fine surface bumps (pores, wrinkles) appear softer in the diffuse component because the scattering blurs them out. Specular highlights remain sharp (specular reflection happens at the surface).
- **Translucency / back-lighting** — thin regions (ears, nostrils, fingers against light) glow red/orange as light passes entirely through.

Without SSS, skin looks like plastic or painted clay — hard lighting transitions, no warmth, no translucency.

---

## 2. Kenshi's Current Skin Rendering

### GBuffer Layout

| MRT   | Format            | Content |
|-------|-------------------|---------|
| RT0   | B8G8R8A8_UNORM    | R=Luma(Y), G=Chroma(Co/Cg checkerboard), B=Metalness, A=Gloss |
| RT1   | B8G8R8A8_UNORM    | RGB=Normal (n*0.5+0.5), A=Emissive/Translucency |
| RT2   | R32_FLOAT         | Linear depth (distance / farClip) |
| DS    | D24_UNORM_S8_UINT | Hardware depth + stencil |

### Skin Material Shaders

**`skin.hlsl`** — 292 draws/frame, 16 permutations. Used for skinned body parts (limbs, torso). Features:
- 60-bone hardware skinning, 3 blend weights
- Blood system via atan2 cylinder projection
- Part masking (hiddenMask) for armor/clothing
- Wetness blending
- DARKEN=0.8 multiplier on final albedo
- Textures: diffuse (s0), normal (s1), colorMap with metalness in blue channel (s2)

**`character.hlsl`** — 131 draws/frame, 12 permutations. Full body + hair, 13 texture inputs. Features:
- Skin tone variation via mask subtraction
- Hair/beard color channel blending
- Dismemberment (severed_limb_fs)
- DARKEN=0.7 multiplier
- Separate body, head, hair, beard, vest, blood textures

### Current Translucency Handling

RT1.A dual-encodes translucency (0.0-0.5 → 0.0-1.0 translucency) and emissive (0.5-1.0 → 0.0-3.2 emissive). The deferred shader applies simple wrap lighting:

```hlsl
if (dotNL < 0) dotNL = lerp(0.0, -dotNL, translucency);
```

This is a crude approximation — it turns back-facing surfaces "partially lit" but without the color shift, depth-dependence, or diffusion characteristics of real SSS.

### Deferred Lighting Output

Single **R11G11B10_FLOAT** HDR target. No separate diffuse/specular channels. This is the most significant constraint for implementing screen-space SSS techniques.

### What the Existing "SSS" Plugin Actually Does

The `effects/sss/` directory contains **Screen Space Shadows** (contact shadows via depth-buffer ray marching toward the sun). It is NOT subsurface scattering. It produces a shadow mask (R8_UNORM) applied multiplicatively to the HDR scene. The naming is a coincidence.

---

## 3. SSS Techniques — Survey

### 3.1 Texture-Space Diffusion (d'Eon & Luebke, GPU Gems 3, 2007)

**How it works:** Render irradiance into texture space (using the mesh's UV layout), blur the irradiance texture with multiple Gaussian kernels at different radii (one per diffusion profile component), then sample the blurred irradiance during the final render.

**Diffusion profile (sum of 6 Gaussians for skin):**
```
R(r) = w1*G(v1,r) + w2*G(v2,r) + w3*G(v3,r) + w4*G(v4,r) + w5*G(v5,r) + w6*G(v6,r)
```
Each Gaussian has per-channel (RGB) variance, modeling the different scattering distances for red/green/blue light.

**Pros:** Physically accurate, works regardless of screen resolution, handles self-shadowing and concavities correctly.

**Cons:** Requires UV access for each mesh, texture-space rendering is invasive (needs geometry access), stretch correction for non-uniform UV density, up to 6 separate blur passes. **Not feasible in Dust's post-processing architecture.**

### 3.2 Screen-Space Subsurface Scattering (Jimenez et al., 2009)

**How it works:** Instead of texture-space diffusion, blur the lit irradiance directly in screen space. The depth buffer is used to modulate the blur radius so that the convolution follows the surface shape rather than screen-space distance.

**Key insight:** Subsurface scattering only affects diffuse lighting. Specular reflections happen at the surface and must NOT be blurred. This requires **separate diffuse and specular HDR buffers**.

**Implementation:**
1. Render diffuse and specular lighting into separate targets
2. Mark skin pixels (stencil buffer or alpha channel flag)
3. Apply the sum-of-Gaussians diffusion profile as multiple separable blur passes on the diffuse buffer only
4. Recombine blurred diffuse + sharp specular

**Diffusion kernel (6 Gaussians × 2 passes each = 12 total 1D passes):**
Each Gaussian has different per-channel weights → red blurs wider than green, green wider than blue.

**Pros:** Good quality, works in screen space (no geometry access needed for the blur itself).

**Cons:** Requires separate diffuse/specular buffers (Kenshi has only one HDR RT), 12 blur passes is expensive, needs material masking. Edge artifacts at silhouettes.

### 3.3 Separable Subsurface Scattering (Jimenez et al., 2015) — "SSSS" or "4S"

**How it works:** The key innovation — approximate the full 2D diffusion profile with a **single separable kernel** instead of a sum of separable Gaussians. This reduces the cost from 12 passes to just **2 passes** (horizontal + vertical).

**Kernel generation (CPU-side):**
```cpp
// Sum-of-Gaussians skin profile
D3DXVECTOR3 profile(float r) {
    return 0.100f * gaussian(0.0484f, r)
         + 0.118f * gaussian(0.187f, r)
         + 0.113f * gaussian(0.567f, r)
         + 0.358f * gaussian(1.99f, r)
         + 0.078f * gaussian(7.41f, r);
}

// Per-channel Gaussian with per-channel falloff
D3DXVECTOR3 gaussian(float variance, float r) {
    for (int i = 0; i < 3; i++) {
        float rr = r / (0.001f + falloff[i]); // falloff = (1.0, 0.37, 0.3)
        g[i] = exp(-(rr*rr) / (2.0f * variance)) / (2.0f * PI * variance);
    }
}
```

**Blur shader (HLSL, per pass):**
```hlsl
// For each sample in the kernel (11-25 samples):
float2 offset = kernel[i].a * dir;  // dir = (1,0) for H, (0,1) for V
float2 sampleUV = uv + offset * sssWidth * correction;
// correction = distanceToProjectionWindow / depth → converts screen to world space
float3 sampleColor = colorTex.Sample(samp, sampleUV).rgb;
float sampleDepth = depthTex.Sample(samp, sampleUV).r;

// Depth-based rejection: don't blur across depth discontinuities
float s = saturate(300.0 * distanceToProjectionWindow * sssWidth *
          abs(centerDepth - sampleDepth));
color += lerp(sampleColor, centerColor, s) * kernel[i].rgb;
```

**Quality levels:**
- Low: 11 samples per pass (22 total)
- Medium: 17 samples (34 total)
- High: 25 samples (50 total)

**Default skin profile parameters:**
- Falloff: (1.0, 0.37, 0.3) — red scatters 3x farther than blue
- Strength: (0.48, 0.41, 0.28)
- sssWidth: world-space SSS radius (e.g. 0.012 = 12mm for human skin)

**Performance:** Under 0.5ms for both passes at 1080p.

**Material masking:** Original implementation uses the alpha channel of the HDR target to store SSS width per pixel. Where alpha=0, no blur is applied. Alternative: stencil buffer.

**Pros:** Best quality-to-cost ratio. Only 2 passes. Industry standard (used in Unreal Engine, many AAA games). Well-documented reference implementation.

**Cons:** Still requires separate diffuse/specular for correctness. Kenshi's R11G11B10_FLOAT has no alpha channel for masking. Separable approximation introduces slight cross-pattern artifacts (barely visible).

### 3.4 Pre-Integrated Skin Shading (Penner, 2011)

**How it works:** Instead of blurring lighting in screen space, pre-compute the effect of subsurface scattering on the diffuse BRDF as a 2D lookup texture. The LUT is parameterized by:
- **X axis:** N·L (remapped to 0-1 via `NdotL * 0.5 + 0.5`)
- **Y axis:** 1/curvature (inverse surface curvature)

For each (NdotL, curvature) pair, the LUT stores the integrated scattering result:
```
D(θ, r) = ∫cos(θ+x) · R(2r·sin(x/2)) dx / ∫R(2r·sin(x/2)) dx
```
where R(r) is the diffusion profile and r is the radius of curvature.

**LUT generation (offline, per-channel):**
```
For each curvature c (y axis):
  For each theta (x axis, mapped to NdotL):
    Integrate the diffusion profile R(r) weighted by cos(theta+x)
    over a ring of radius 1/c
    Store the RGB result
```

**Shader usage:**
```hlsl
// Approximate curvature from screen-space derivatives
float curvature = length(fwidth(worldNormal)) / length(fwidth(worldPos));

// Sample the pre-integrated BRDF
float3 brdf = skinLUT.Sample(linearClamp,
    float2(NdotL * 0.5 + 0.5, curvature * curvatureScale));

// Replace Lambert diffuse with pre-integrated diffuse
float3 diffuse = brdf * lightColor * albedo;
```

**Pros:**
- **Zero extra passes** — just a texture lookup replacing the Lambert term
- **No separate diffuse/specular buffers needed**
- **No material masking needed in a post-pass** — applied per-pixel in the deferred shader
- Extremely cheap (~0.0ms extra cost, single texture sample)
- Handles the soft terminator and color shift naturally
- Can be patched into Kenshi's deferred shader via D3DCompile hook

**Cons:**
- Lower quality than Jimenez blur — doesn't capture fine-scale diffusion (pore softening)
- Curvature approximation from screen-space derivatives can be noisy
- Doesn't handle transmittance / back-lighting
- Assumes locally spherical geometry (the ring integration)
- Needs material masking to avoid applying to non-skin surfaces

### 3.5 Burley Normalized Diffusion (Christensen & Burley, 2015)

**How it works:** An empirical fit to the full diffusion profile that's simpler than sum-of-Gaussians:
```
R(r) = A * s/(8π) * (e^(-s*r) + e^(-s*r/3)) / r
```
where `s` is a shape parameter derived from surface albedo and `A` is the surface albedo.

**Advantages over sum-of-Gaussians:**
- Two exponential terms instead of six Gaussians
- More physically accurate (matches Monte Carlo references better)
- Easier to importance-sample (analytic CDF exists)
- Used in Unity HDRP and Disney/Pixar production rendering

**For screen-space evaluation (Golubev, SIGGRAPH 2018):**
Instead of separable Gaussian blur, use importance sampling of the Burley profile. For each pixel, take N samples in a disk around the center pixel, with sample positions drawn from the Burley profile's CDF. This is non-separable but can be efficient with enough samples.

**Pros:** Most physically accurate profile. Elegant parameterization.

**Cons:** Non-separable → more expensive than Jimenez 4S. Importance sampling needs enough samples for clean results without temporal accumulation (our constraint). Better suited as the profile inside a pre-integrated LUT than as a screen-space kernel.

### 3.6 Transmittance / Back-Lighting (Jimenez, Green, GPU Pro 2)

**How it works:** Estimate the thickness of the object at each pixel using the shadow map. The distance light travels through the object is:
```
thickness = d_exit - d_enter
```
where `d_enter` is the shadow map depth (where light first hits the surface) and `d_exit` is the current pixel's depth from the light's perspective.

Apply the diffusion profile to the thickness to get the transmitted light color:
```hlsl
float3 transmittance(float3 sssColor, float thickness) {
    // Same sum-of-Gaussians profile used for diffusion
    float3 t = 0.233f * exp(-thickness*thickness / (2*0.0064f))
             + 0.100f * exp(-thickness*thickness / (2*0.0484f))
             + 0.118f * exp(-thickness*thickness / (2*0.187f))
             + 0.113f * exp(-thickness*thickness / (2*0.567f))
             + 0.358f * exp(-thickness*thickness / (2*1.99f))
             + 0.078f * exp(-thickness*thickness / (2*7.41f));
    return t * sssColor;
}
```

**Pros:** Creates the dramatic back-lit ears/fingers effect. Uses existing shadow map.

**Cons:** Requires shadow map access (we have it — slot 5 during deferred). Thickness estimation assumes convex geometry. Requires light-space projection of pixel position.

---

## 4. Constraints Analysis for Dust

### Hard Constraints

| Constraint | Impact |
|-----------|--------|
| **Single R11G11B10_FLOAT HDR output** | Cannot do pure Jimenez SSS (needs separate diffuse/specular). No alpha channel for per-pixel SSS width. |
| **No temporal techniques** | Cannot amortize expensive per-pixel sampling (Burley importance sampling) across frames. Must produce clean output in a single frame. |
| **Post-processing architecture** | Cannot modify the game's GBuffer shaders directly. Can only patch via D3DCompile hook or inject effects after the fact. |
| **D3D11 Feature Level 11_0** | No DXR, no mesh shaders. Compute shaders available (used by RTGI). |
| **YCoCg albedo encoding** | Albedo reconstruction requires the edge-directed chroma decode. Can use the repacked R16G16B16A16_FLOAT buffer instead. |

### Soft Constraints (Workarounds Exist)

| Constraint | Workaround |
|-----------|-----------|
| **No material ID in GBuffer** | Use RT1.A translucency as skin proxy. Skin surfaces write non-zero translucency; most hard surfaces write zero. Not perfect but practical. |
| **No separate diffuse buffer** | Option A: Approximate by blurring full HDR (specular smearing accepted as minor artifact). Option B: Reconstruct diffuse from GBuffer in a separate pass. Option C: Use pre-integrated approach (no blur needed). |
| **No stencil access for masking** | Can branch on a per-pixel value (translucency) instead of stencil testing. Slightly less efficient but functional. |
| **No geometry access yet** | Pre-integrated and screen-space approaches work without geometry. Transmittance needs shadow map (already available). |

### Available Resources at POST_LIGHTING

| Resource | Format | How to Access |
|----------|--------|---------------|
| GBuffer RT0 | B8G8R8A8_UNORM | `host->GetSRV(DUST_RESOURCE_ALBEDO)` |
| GBuffer RT1 | B8G8R8A8_UNORM | `host->GetSRV(DUST_RESOURCE_NORMALS)` |
| GBuffer RT2 | R32_FLOAT | `host->GetSRV(DUST_RESOURCE_DEPTH)` |
| Repacked albedo | R16G16B16A16_FLOAT | Available after repack pass |
| HDR scene | R11G11B10_FLOAT | `host->GetRTV(DUST_RESOURCE_HDR_RT)` |
| Shadow map | R32_FLOAT 4096x4096 | PS SRV slot 5 during deferred |
| Sun direction | float3 | CB0 offset c0 (already extracted by SSS/RTGI) |
| Inverse view matrix | float4x4 | CB0 offset c8 (already extracted) |
| Camera data | DustCameraData | `ctx->camera` |

---

## 5. Recommended Approach: Two-Tier Implementation

### Tier 1: Pre-Integrated Skin + Approximate Screen-Space Blur (Immediate)

This can be built with the existing post-processing architecture and provides the two most impactful visual improvements: soft terminators and subtle diffusion softening.

#### Component A: Pre-Integrated Skin Diffuse (Deferred Shader Patch)

**What:** Replace the Lambert diffuse term with a pre-integrated skin BRDF lookup for pixels identified as skin.

**How:** Patch the deferred shader source via the existing D3DCompile hook. When the game compiles `deferred.hlsl`, inject:

1. A skin diffusion LUT (Texture2D, bound to an unused sampler register)
2. Curvature computation from screen-space derivatives of normal and position
3. Skin identification via the translucency value in RT1.A
4. LUT sampling to replace the Lambert `dotNL * lightColor` term

```hlsl
// In the patched deferred shader:
float translucency = DecodeTranslucency(gBuf1.a);  // existing decode
float isSkin = step(0.01, translucency) * step(translucency, 0.99);

if (isSkin > 0.5) {
    // Approximate curvature from screen-space derivatives
    float3 worldN = normalize(gBuf1.rgb * 2.0 - 1.0);
    float3 worldPos = ReconstructWorldPos(uv, depth);
    float curvature = length(fwidth(worldN)) / max(length(fwidth(worldPos)), 0.0001);

    // Sample pre-integrated diffuse BRDF
    float2 brdfUV = float2(dotNL * 0.5 + 0.5, saturate(curvature * curvatureScale));
    float3 skinDiffuse = skinLUT.Sample(linearClamp, brdfUV).rgb;

    // Replace Lambert with pre-integrated result
    ld.diffuse = skinDiffuse * lightColor * albedo;
} else {
    // Standard Lambert (or Disney/Burley) for non-skin
    ld.diffuse = PI * dotNL * lightColor * FresnelDiffuse(specColor);
}
```

**LUT generation (offline, baked into a texture file):**

The 256x256 LUT is generated by integrating the diffusion profile over a ring for each (NdotL, curvature) pair:

```python
# Pseudocode for LUT generation
for y in range(256):  # curvature axis
    r = map_to_curvature_range(y)  # inverse curvature (radius)
    for x in range(256):  # NdotL axis
        theta = map_to_angle(x)  # incident angle
        result = float3(0, 0, 0)
        norm = 0
        for sample in integration_steps:
            angle = sample * PI  # -PI to PI
            cos_term = max(0, cos(theta + angle))
            d = 2 * r * abs(sin(angle / 2))  # chord distance on ring
            weight = diffusion_profile(d)  # R(d) evaluated per-channel
            result += cos_term * weight
            norm += weight
        lut[x, y] = result / norm
```

The diffusion profile R(d) should use the Burley normalized diffusion for best accuracy:
```
R(r) = s/(8*PI) * (exp(-s*r) + exp(-s*r/3)) / max(r, epsilon)
```

**Cost:** Essentially free — one extra texture sample per skin pixel in the deferred shader.

#### Component B: Lightweight Screen-Space Diffusion Blur (New Plugin)

**What:** A subtle, depth-aware Gaussian blur applied only to skin pixels in the HDR buffer. This adds the characteristic "pore softening" that pre-integrated skin alone doesn't capture.

**Why blur the full HDR instead of diffuse-only:** Without separate diffuse/specular buffers, we blur everything. The trick is to keep the blur radius small enough that specular smearing is imperceptible. Skin specular is typically broad (high roughness / low gloss) so a subtle blur doesn't noticeably change it.

**Implementation:**

```hlsl
// Separable blur kernel for skin (simplified Jimenez, 7-11 samples)
// Smaller kernel than full Jimenez because we're blurring full HDR
static const int NUM_SAMPLES = 7;
static const float4 kernel[NUM_SAMPLES] = {
    // .rgb = per-channel weight, .a = offset
    float4(0.560, 0.560, 0.560, 0.000),  // center
    float4(0.098, 0.078, 0.042, 0.080),  // ±1
    float4(0.065, 0.043, 0.018, 0.200),  // ±2
    float4(0.028, 0.011, 0.004, 0.400),  // ±3
    ... // Additional samples
};

float4 SSSBlurPS(float2 uv, float2 dir) {
    float depth = depthTex.Sample(pointClamp, uv);
    float translucency = DecodeSkinMask(normalsTex.Sample(pointClamp, uv).a);

    // Skip non-skin pixels
    if (translucency < 0.01 || depth > maxSkinDepth)
        return hdrTex.Sample(pointClamp, uv);

    // World-space correction: blur radius is constant in world space
    float correction = distanceToProjectionWindow / depth;
    float blurScale = sssWidth * correction * translucency;

    float3 color = hdrTex.Sample(pointClamp, uv).rgb * kernel[0].rgb;

    [unroll]
    for (int i = 1; i < NUM_SAMPLES; i++) {
        float2 offset = kernel[i].a * blurScale * dir;

        float2 uv1 = uv + offset;
        float2 uv2 = uv - offset;

        float3 c1 = hdrTex.Sample(linearClamp, uv1).rgb;
        float3 c2 = hdrTex.Sample(linearClamp, uv2).rgb;
        float d1 = depthTex.Sample(pointClamp, uv1);
        float d2 = depthTex.Sample(pointClamp, uv2);

        // Depth-based rejection: snap to center color at depth edges
        float s1 = saturate(depthThreshold * abs(depth - d1) * correction);
        float s2 = saturate(depthThreshold * abs(depth - d2) * correction);
        c1 = lerp(c1, color / max(kernel[0].rgb, 0.001), s1);
        c2 = lerp(c2, color / max(kernel[0].rgb, 0.001), s2);

        color += c1 * kernel[i].rgb + c2 * kernel[i].rgb;
    }

    return float4(color, 1);
}
```

**Two passes:** Horizontal (dir = (1,0)) then Vertical (dir = (0,1)).

**Cost:** ~0.2-0.4ms at 1440p with 7 samples per pass (only processing skin pixels).

#### Component C: Transmittance / Back-Lighting (Deferred Shader Patch)

**What:** Light transmission through thin skin regions using the shadow map for thickness estimation.

**How:** In the deferred shader, for skin pixels facing away from the light (dotNL < 0), estimate the thickness by looking up the shadow map depth and comparing with the pixel's depth from the light's perspective.

```hlsl
// In patched deferred shader, for skin pixels:
if (isSkin > 0.5 && dotNL < 0.0) {
    // Project pixel to shadow map space
    float4 shadowCoord = mul(float4(worldPos, 1.0), shadowViewProj);
    shadowCoord.xyz /= shadowCoord.w;
    float2 shadowUV = shadowCoord.xy * 0.5 + 0.5;

    // Thickness = distance light travels through the object
    float shadowDepth = shadowMap.Sample(linearClamp, shadowUV);
    float thickness = abs(shadowCoord.z - shadowDepth);

    // Apply diffusion profile to thickness
    float3 transmit = exp(-thickness * thickness * float3(1.0, 3.5, 7.0));
    // Red transmits most, blue least — matches skin scattering

    float3 backLight = transmit * sunColour * translucency * albedo;
    ld.diffuse += backLight * saturate(-dotNL);
}
```

**Cost:** Minimal — one shadow map lookup + a few ALU ops for skin pixels only.

**Note:** This requires access to the shadow view-projection matrix, which needs to be extracted from the game's constant buffer. The shadow matrix parameters are at registers c12-c15 in the deferred PS CB0 (csmParams, csmScale, csmTrans, csmUvBounds).

### Tier 2: Full Separable SSS with Material System (Future, requires geometry capture)

Once the geometry capture system (Phase 4 of the modernization plan) is implemented:

1. **Stencil-based material masking** — During GBuffer fill, intercept DrawIndexed for skin/character draws (identified by PS hash) and set a custom depth-stencil state that writes a material ID into the stencil buffer.

2. **Separate diffuse/specular RT** — Create an additional R11G11B10_FLOAT render target. Patch the deferred shader to output diffuse lighting to one RT and specular to another. This enables full-quality Jimenez separable SSS.

3. **Per-material SSS profiles** — Store a profile ID in the stencil (different skin types, foliage translucency, wax, marble, etc.). Use a structured buffer with per-profile kernel weights.

4. **Full Jimenez 4S blur** — 25-sample separable blur on the diffuse-only buffer with the complete skin diffusion kernel. Recombine with unblurred specular.

5. **Multi-light transmittance** — With custom shadow maps for point/spot lights, apply transmittance from all shadow-casting light sources.

---

## 6. Skin Identification Strategy

### Using Translucency as a Skin Proxy

The RT1.A channel encodes translucency (0.0-0.5 range) for GBuffer materials. Skin materials in Kenshi are expected to write a non-zero translucency value (since skin IS translucent). Most hard surfaces (metal, stone, wood) write zero translucency.

**Detection logic:**
```hlsl
float rawA = normalsTex.Sample(samp, uv).a;
float translucency = (rawA <= 0.5) ? rawA * 2.0 : 0.0;  // 0 if emissive
float isSkin = smoothstep(0.01, 0.1, translucency);        // soft threshold
```

**Limitations:**
- Foliage may also have translucency → could get SSS applied (fixable by checking roughness/metalness — foliage tends to have different material properties)
- Some skin pixels might have zero translucency
- The translucency value might vary between skin.hlsl and character.hlsl permutations

**Mitigation:** Add additional heuristics:
```hlsl
// Skin tends to: low metalness, moderate roughness, warm albedo
float metalness = gBuf0.b;
float gloss = gBuf0.a;
bool likelySkin = translucency > 0.01
               && metalness < 0.3      // skin isn't metallic
               && gloss < 0.8;         // skin isn't mirror-smooth
```

### Alternative: Depth/Position Heuristic

Characters in Kenshi are typically rendered at specific depth ranges (close to camera). Combined with translucency, a depth threshold could help isolate skin from distant translucent surfaces. This is fragile but could be a useful secondary filter.

### Future: Stencil-Based Material ID

The correct long-term solution. Requires geometry capture (Phase 4) to intercept skin draws and write stencil values.

---

## 7. Implementation Plan

### Phase 1: Pre-Integrated Skin LUT (Deferred Shader Patch)

**Files to modify:**
- `src/D3D11Hook.cpp` — Add skin LUT texture binding in the D3DCompile hook
- Deferred shader patch code — Inject pre-integrated skin BRDF sampling

**New files:**
- `assets/skin_lut.dds` — Pre-computed 256x256 skin BRDF lookup texture (generated offline)
- LUT generation tool (Python/C++ offline script)

**Steps:**
1. Generate the skin LUT offline using the Burley normalized diffusion profile
2. Load the LUT as a DDS texture at framework initialization
3. Bind it to an unused sampler register during the deferred pass
4. Patch the deferred shader to sample the LUT for pixels with non-zero translucency
5. Add GUI controls: SSS strength, curvature scale, skin color tint

### Phase 2: Screen-Space Diffusion Blur (New Plugin: DustSubsurface)

**New files:**
- `effects/subsurface/` — New effect plugin directory
- `DustSubsurface.cpp` — Plugin entry point, settings
- `SubsurfaceRenderer.h/cpp` — Blur passes, compositing
- `SubsurfaceConfig.h` — Configuration struct
- `shaders/subsurface_blur_h_ps.hlsl` — Horizontal blur
- `shaders/subsurface_blur_v_ps.hlsl` — Vertical blur
- `shaders/subsurface_composite_ps.hlsl` — Final composite
- `shaders/subsurface_debug_ps.hlsl` — Debug visualization

**Injection:** POST_LIGHTING, after SSAO/SSIL/contact shadows but before fog.

**Pipeline:**
1. Read HDR scene + depth + normals (RT1 for translucency/skin mask)
2. Horizontal blur pass → temp RT (R11G11B10_FLOAT or R16G16B16A16_FLOAT)
3. Vertical blur pass → back to HDR
4. Only process pixels where translucency indicates skin

**Settings:**
- Enabled (bool)
- SSS Width (float, 0.005-0.05, default 0.012)
- Strength (float, 0-1, default 0.5)
- Max Depth (float, 0.01-0.5, default 0.15)
- Sample Count (int, 7/11/17, default 11)
- Depth Threshold (float, 50-500, default 200)
- Debug View (bool — show skin mask, blur result, etc.)

### Phase 3: Transmittance (Deferred Shader Patch Enhancement)

Requires extracting the shadow projection matrix from the game's CB. The shadow matrix parameters are in the deferred PS CB0 at c12+ (csmParams, csmScale, csmTrans, csmUvBounds). This is already partially understood from the shadow system documentation.

**Steps:**
1. Extract shadow matrix from CB0
2. For back-lit skin pixels, project to shadow space and sample shadow depth
3. Compute thickness and apply diffusion profile for transmitted light color
4. Add as warm back-lighting contribution

---

## 8. Diffusion Kernel Reference

### Sum-of-Gaussians Skin Profile (Jimenez 2015)

| Weight | Variance | Description |
|--------|----------|-------------|
| 0.100  | 0.0484   | Very tight — pore-level detail |
| 0.118  | 0.187    | Fine detail — wrinkles, bumps |
| 0.113  | 0.567    | Medium — local curvature effect |
| 0.358  | 1.99     | Broad — overall form softening |
| 0.078  | 7.41     | Very broad — global color bleed |

Per-channel falloff: **(R=1.0, G=0.37, B=0.3)** — red scatters ~3x farther than blue.

Default strength: **(R=0.48, G=0.41, B=0.28)**

### Burley Normalized Diffusion Profile

```
R(r) = s/(8*PI) * (exp(-s*r) + exp(-s*r/3)) / r
```

The shape parameter `s` is derived from surface albedo `A`:
```
s = 1.9 - A + 3.5 * (A - 0.8)^2
```

For typical skin (A ≈ 0.5-0.7):
- Fair skin: s ≈ 1.5
- Medium skin: s ≈ 1.3
- Dark skin: s ≈ 1.1

### Pre-Integrated LUT Dimensions

- **X axis (NdotL):** 256 texels, range [-1, 1] mapped to [0, 1] via `NdotL * 0.5 + 0.5`
- **Y axis (curvature):** 256 texels, range [0, 1] representing inverse curvature (1/r). Low values = flat surfaces (wide scattering visible), high values = sharp curves (narrow scattering).
- **Format:** R16G16B16A16_FLOAT or R8G8B8A8_UNORM (8-bit is sufficient for the LUT)

---

## 9. Performance Budget

| Component | Estimated Cost | Notes |
|-----------|---------------|-------|
| Pre-integrated LUT sampling | ~0.0ms | Single texture fetch per pixel, ALU negligible |
| Curvature computation | ~0.05ms | fwidth() calls on normal and position |
| Skin mask generation | ~0.0ms | Branch on existing GBuffer data |
| Screen-space blur (7 samples) | ~0.2-0.4ms | Two passes, skin pixels only |
| Screen-space blur (11 samples) | ~0.3-0.6ms | Higher quality |
| Transmittance | ~0.05ms | Shadow map lookup for back-lit pixels only |
| **Total (Tier 1)** | **~0.3-0.7ms** | At 2560x1440 |

For comparison: SSAO costs 1-3ms, SSIL costs 2-5ms, RTGI costs 3-8ms. SSS is one of the cheaper effects.

---

## 10. Key References

### Papers & Presentations
- [Separable Subsurface Scattering (Jimenez et al., 2015)](https://www.iryoku.com/separable-sss/) — The industry-standard screen-space SSS technique
- [Screen-Space Subsurface Scattering (Jimenez et al., 2009)](https://www.iryoku.com/screen-space-subsurface-scattering/) — Original screen-space approach
- [Pre-Integrated Skin Shading (Penner, 2011)](https://www.slideshare.net/slideshow/penner-preintegrated-skin-rendering-siggraph-2011-advances-in-realtime-rendering-course/13966747) — LUT-based approach, no blur passes needed
- [Efficient Screen-Space SSS (Golubev, SIGGRAPH 2018)](https://advances.realtimerendering.com/s2018/Efficient%20screen%20space%20subsurface%20scattering%20Siggraph%202018.pdf) — Burley profile for Unity HDRP
- [Approximate Reflectance Profiles (Christensen & Burley, 2015)](https://graphics.pixar.com/library/ApproxBSSRDF/paper.pdf) — Normalized diffusion profile
- [Advanced Skin Rendering (d'Eon & Luebke, GPU Gems 3)](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-14-advanced-techniques-realistic-real-time-skin) — Texture-space diffusion
- [Real-Time Approximations to SSS (GPU Gems 1, Ch. 16)](https://developer.nvidia.com/gpugems/gpugems/part-iii-materials/chapter-16-real-time-approximations-subsurface-scattering) — Foundational depth-based techniques

### Reference Implementations
- [iryoku/separable-sss (GitHub)](https://github.com/iryoku/separable-sss) — Jimenez's reference DX10 implementation with SeparableSSS.h shader and kernel calculation code
- [Skyrim Community Shaders SSS](https://www.nexusmods.com/skyrimspecialedition/mods/114114) — Similar modding context (injecting SSS into an existing deferred renderer)
- [Pre-Integrated Skin Shader (Unity)](https://farfarer.com/blog/2013/02/11/pre-integrated-skin-shader-unity-3d/) — Practical implementation of Penner's technique
- [NVIDIA FaceWorks](https://github.com/NVIDIAGameWorks/FaceWorks) — Production skin rendering library

### Blog Posts & Tutorials
- [Pre-Integrated Skin Shading (Simon's Tech Blog)](http://simonstechblog.blogspot.com/2015/02/pre-integrated-skin-shading.html) — Step-by-step LUT generation
- [Skin Shading Model Journey (GraphicsNerd)](http://grephicsnerd.blogspot.com/2017/06/skin-shading-model.html) — Practical implementation walkthrough
- [Deferred SSS Using Compute Shaders (Der Schmale)](https://www.derschmale.com/2014/06/02/deferred-subsurface-scattering-using-compute-shaders/) — Compute shader approach
- [Fast SSS for Unity URP (John Austin)](https://johnaustin.io/articles/2020/fast-subsurface-scattering-for-the-unity-urp) — Simplified approach without separate buffers

---

## 11. Summary & Recommendation

**Start with Tier 1** — the combination of pre-integrated skin shading (in the deferred shader patch) and a lightweight screen-space blur (as a new plugin) provides 80% of the visual improvement at minimal cost and no architectural changes.

The pre-integrated LUT handles the most visually important aspect: soft, warm light falloff on skin at shadow terminators. The screen-space blur adds the secondary effect of fine detail softening. Together they make skin look like skin rather than painted plastic.

**Key decision:** The skin identification strategy (translucency-based proxy) is the weakest link. It should work for most cases but may misidentify some foliage or other translucent surfaces. The long-term solution is stencil-based material masking from Phase 4 of the modernization plan.

**What NOT to do:**
- Don't attempt full Jimenez 4S without separate diffuse/specular buffers — blurring specular will look wrong
- Don't use temporal accumulation for denoising the blur — spatial-only per the quality guidelines
- Don't try to modify skin.hlsl/character.hlsl directly — use the D3DCompile hook to patch the deferred shader where all skin pixels are processed uniformly
