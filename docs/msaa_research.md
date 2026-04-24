# True MSAA in Kenshi's Deferred Renderer — Research

## 1. Where We Stand Right Now

### Current MSAARedirect implementation (`MSAARedirect.cpp`)

The existing code does **GBuffer-only MSAA**:

1. **On GBuffer enter**: Creates MSAA versions of all 3 GBuffer RTs (B8G8R8A8, B8G8R8A8, R32_FLOAT) + MSAA depth (D24_S8). Swaps the game's RTVs/DSV for MSAA versions via `HookedOMSetRenderTargets`.
2. **During GBuffer**: All geometry draws rasterize into MSAA targets. The hardware produces per-sample coverage and depth correctly — triangle edges get subsamples from different triangles.
3. **On GBuffer leave**: Resolves color via `ResolveSubresource()` (hardware box filter) and depth via custom min-depth shader.
4. **After resolve**: The pipeline continues with single-sample textures — stencil mask, GBuffer repack, deferred lighting, everything sees 1x data.

**What this gives us**: Anti-aliased GBuffer attributes. Albedo edges are smooth, normal edges are smooth, depth edges are resolved.

**What this does NOT give us**: Per-sample lighting. The deferred pass reads the resolved GBuffer and computes lighting once per pixel. Specular highlights, shadow boundaries, and lighting discontinuities at geometric edges remain aliased. We're paying the memory/bandwidth cost of MSAA but getting minimal visible anti-aliasing because the lighting pass (where the visual result is determined) operates on resolved data.

### The critical pipeline stages between GBuffer and lighting

From actual code (`PipelineDetector.cpp`, `D3D11Hook.cpp`) cross-referenced with pipeline docs:

```
GBuffer fill (MSAA) → [MSAA RESOLVE happens here currently] → Sky → Shadow map →
Deferred pre-pass → Stencil mask → GBuffer repack (YCoCg→R16G16B16A16) →
Deferred lighting (sun) → Light volumes → Water → Fog → ...
```

The **GBuffer repack** (draws 3645-3647) is a critical stage. It converts the packed YCoCg B8G8R8A8 GBuffer into R16G16B16A16_FLOAT for the deferred lighting shader. This is where the chroma reconstruction happens — the edge-directed `decodePixel` function samples 4 neighbors to reconstruct the missing Co/Cg channel.

---

## 2. Why Current MSAA Doesn't Work

The Threat Interactive video on Crysis 3 MSAA identified three categories of failure. Here's how each maps to Kenshi.

### Problem 1: Resolve happens too early

The current resolve (`MSAARedirect::OnGBufferLeave`) fires as soon as `OMSetRenderTargets` detects the GBuffer pass ended. The MSAA data is destroyed before:
- The GBuffer repack (which reconstructs YCoCg albedo)
- The deferred lighting pass (which computes the visible result)
- Any screen-space effects (SSAO, SSIL)

For MSAA to produce visible anti-aliasing, the per-sample data must survive through lighting and be resolved **after** the lit color is computed.

### Problem 2: YCoCg chroma subsampling at MSAA edges

Kenshi's GBuffer uses YCoCg with checkerboard chroma subsampling — even pixels store Cg in the G channel, odd pixels store Co. The repack pass reconstructs full RGB.

At MSAA edges, two subsamples within one pixel may come from triangles with very different albedo. If we resolve (average) the YCoCg-encoded values, we're averaging:
- Subsample 0: Y=0.8, Co=0.3 (from a brown rock)
- Subsample 1: Y=0.2, Cg=0.6 (from green grass)

The box-filter average produces Y=0.5, Co/Cg=0.45 — but the chroma channel is checkerboard-multiplexed, so the result is a single chroma value that doesn't meaningfully represent either surface. The repack pass then tries to reconstruct this, potentially producing color artifacts.

This is analogous to the green/purple tinting Threat Interactive documented in Crysis 3, though Kenshi uses 8-bit per channel (not 16-bit compressed), so precision isn't the problem — the checkerboard chroma subsampling itself is.

### Problem 3: Screen-space effects destroy subsamples

Even if we kept MSAA through lighting, the current SSAO/SSIL effects operate at pixel frequency. If we composite AO at pixel frequency onto an MSAA lit buffer, we destroy the per-sample variation at edges — exactly what happened in Crysis 3.

---

## 3. Background: How MSAA Works in Deferred Renderers

### The fundamental problem

In **forward rendering**, MSAA is efficient: the rasterizer generates per-sample coverage and depth, but the pixel shader runs once per pixel. Per-sample coverage is applied after shading to weight the result into the correct subsamples. You only pay additional rasterizer and ROP cost.

In **deferred rendering**, this breaks because there are two geometry passes:

1. **GBuffer pass**: Geometry rasterizes with per-sample coverage. Material attributes are written per-sample at edges. Works correctly.
2. **Lighting pass**: A fullscreen quad reads GBuffer and computes lighting. The quad has no geometric information — the hardware cannot know which pixels were triangle edges. The rasterizer sees the fullscreen quad as covering every sample uniformly.

The lighting pass must shade at pixel frequency for performance, but at sample frequency for correctness at edges. Full per-sample shading = full supersampling cost. With 4x MSAA where only 5-15% of pixels are edges, 85-95% of per-sample work would be wasted.

### Per-pixel vs per-sample shading

- **Per-pixel** (default): Lighting reads GBuffer sample 0, computes once. Result is written to all coverage samples. Correct for interior pixels, aliased at edges where subsamples came from different triangles.
- **Per-sample**: Lighting is invoked once per subsample (using `SV_SampleIndex`), reading each subsample's unique material data. Correct anti-aliased edges but Nx the shading cost.

### Edge detection methods

**Method 1: SV_Coverage in GBuffer pass**
`SV_Coverage` tells which samples were covered by the current triangle. If any sample has coverage != all-bits-set, that pixel is at an edge. Write this to stencil during the GBuffer pass. Cost: free (already running GBuffer shader).

**Method 2: Post-GBuffer depth/normal discontinuity**
Fullscreen pass comparing depth (and optionally normal) values between subsamples within each pixel. If subsamples differ beyond a threshold, mark the pixel as complex. More reliable — catches all discontinuities.

**Method 3: Per-sample depth comparison**
Load all N depth subsamples, compare. If `max(depths) - min(depths) > threshold`, pixel is complex. Simpler but can miss edges where depth is continuous but normals differ.

### The stencil optimization (CryEngine approach)

1. **GBuffer pass**: Render to MSAA GBuffer textures.
2. **Resolve + stencil marking**: Copy sample 0 to non-MSAA textures. Compare subsamples — if different, set stencil bit. Tag at quad granularity (2x2) for GPU stencil culling efficiency.
3. **Lighting pass 1 (pixel-freq)**: Stencil rejects edge pixels. ~90-95% of pixels at pixel cost.
4. **Lighting pass 2 (sample-freq)**: Stencil only passes edge pixels. Uses `SV_SampleIndex` to iterate subsamples.
5. **Resolve**: Custom HDR-aware resolve to non-MSAA backbuffer.

### Crysis 3's failures (documented by Threat Interactive)

- **Stencil mask too conservative**: Missed real edges → those pixels got pixel-frequency lighting with sample-0 data → visible aliasing persisted even with MSAA enabled.
- **16-bit compressed albedo**: YCoCg packing into 16-bit RTs caused quantization. Averaging quantized YCoCg values produced wrong chrominance at edges → green/purple tinting.
- **AO computed without subsampling**: SSAO ran at pixel frequency, composited multiplicatively at pixel frequency. Destroyed subsample lighting at edges. Even correct per-sample lighting was overwritten by single-value AO.
- **Stencil bug in rain scene**: Stencil got corrupted, causing the entire lighting pass to skip sample-frequency shading. MSAA was effectively disabled for hundreds of light draws.

### Half-Life Alyx comparison

Alyx uses **forward rendering** (Source 2 forward+). MSAA works natively:
- No GBuffer, no stencil edge detection, no encoding artifacts
- Pixel shader outputs final lit color, which is what gets multisampled
- No screen-space effect interaction problems
- 4x MSAA by default with no per-sample lighting overhead

The lesson: forward rendering with MSAA is inherently simpler and higher quality. Deferred MSAA is an approximation that tries to reconstruct what forward rendering gets natively.

---

## 4. Architecture for True MSAA in Kenshi

### Target: 2x MSAA + SMAA (Modernized SMAA S2x)

Based on Threat Interactive's analysis:

- **2x MSAA** provides real geometric subsampling — the rasterizer produces correct coverage, which no post-process AA can replicate
- **SMAA** adds morphological smoothing on top, handling subpixel features and shader aliasing that MSAA doesn't touch
- At 1080p: 2x MSAA + SMAA approaches 4x MSAA quality ("after 4x MSAA at 1080p, diminishing returns; cheap morphological solutions combined with 4x have already surpassed" — Threat Interactive)
- At 1440p: 2x MSAA is the sweet spot (50% more density than 1080p, halve the multi-sampling)
- Memory: 2x GBuffer (~64MB at 1080p) vs 4x (~128MB) — manageable

### Full pipeline

```
PHASE 1: GBuffer Fill (MSAA 2x)

  RT0 (B8G8R8A8, 2x): YCoCg albedo + metalness
  RT1 (B8G8R8A8, 2x): Normals + emissive
  RT2 (R32_FLOAT, 2x): Linear depth
  DS  (D24_S8, 2x): Hardware depth + stencil

  During GBuffer writes:
  - Use centroid interpolation on UVs to prevent off-triangle extrapolation
  - SV_Coverage marks partial coverage pixels

PHASE 2: Edge Detection + Stencil Marking

  Fullscreen pass reads Texture2DMS<float> depth:
  - Compare sample 0 vs sample 1 depth per pixel
  - Also compare normals between subsamples
  - If different → mark edge pixel
  - Mark at quad granularity (2x2) for GPU stencil culling efficiency

  Simultaneously: copy sample-0 data into non-MSAA GBuffer copies
  for the pixel-frequency path

PHASE 3: GBuffer Repack (game's native pass)

  The game's repack reads the resolved (sample-0) non-MSAA GBuffer.
  For the per-sample path, we do our own YCoCg decode.

  Option A: Patch gbuffer_repack.hlsl to handle Texture2DMS
            (unlikely — precompiled shader)
  Option B: Write our own MSAA-aware repack shader that
            decodes YCoCg per-sample
  Option C: Feed resolved GBuffer to game's repack, do per-sample
            YCoCg decode in our own lighting path for edge pixels only

PHASE 4: Deferred Lighting

  Pass 4a — Pixel frequency (non-edge pixels):
    Standard game lighting on resolved GBuffer.
    ~90-95% of pixels. Normal cost.
    Output → MSAA RT (write to both samples)

  Pass 4b — Sample frequency (edge pixels only):
    Custom lighting shader with SV_SampleIndex.
    Reads Texture2DMS GBuffer per subsample.
    Decodes YCoCg per-sample.
    Computes full BRDF per subsample.
    ~5-10% of pixels. 2x cost for these pixels only.
    Output → MSAA RT (write to correct sample)

PHASE 5: Post-Lighting (forward passes, fog, etc.)

  These render into the MSAA HDR RT.
  Forward draws (water, fog, basic geometry) get natural MSAA
  because they rasterize geometry into the MSAA target.

PHASE 6: Custom HDR-Aware Resolve

  AMD reversible tonemapper:
  for each pixel:
    for each sample i:
      w_i = 1 / (1 + max(r,g,b))
      result += sample_i * w_i
      totalW += w_i
    result /= totalW

  Output: non-MSAA R11G11B10_FLOAT HDR

PHASE 7: Screen-Space Effects

  SSAO, SSIL, Clarity, etc. operate on the resolved (1x) HDR image.
  Applied after resolve. No MSAA interaction problems.

PHASE 8: Tonemapping → SMAA → Present

  Tonemap HDR→LDR (standard path)
  SMAA with subsampleIndices for 2x MSAA pattern
  Clean, sharp, geometrically anti-aliased output
```

---

## 5. The Hard Technical Problems

### 5.1. Intercepting the deferred lighting pass

The game's deferred lighting is compiled at runtime via D3DCompile — we already intercept this in `HookedD3DCompile` to patch in AO support and shadow filtering. This is our path for per-sample lighting.

Two approaches:

**Approach A: Two compiled variants**
Patch the deferred shader source to compile two versions — one pixel-frequency, one sample-frequency with `SV_SampleIndex` and `Texture2DMS` reads. Dispatch them with the edge mask (stencil or texture). Requires replacing the game's single lighting draw with two draws.

**Approach B: Overlay correction pass**
Let the game's lighting draw run normally on resolved data. Then at POST_LIGHTING, run a custom pass that re-lights only edge pixels per-sample and overwrites those pixels in the HDR target. Simpler — doesn't modify the game's deferred pass at all.

Approach B is recommended for initial implementation. The game does its normal lighting, then we correct the edge pixels.

### 5.2. The GBuffer repack problem

The game's GBuffer repack (draws 3645-3647) converts packed YCoCg into R16G16B16A16_FLOAT. This is a pre-compiled shader — we can't patch it. The deferred lighting reads from this repacked buffer.

For the per-sample path, we must decode YCoCg per-sample ourselves. This means implementing the `decodePixel` function (edge-directed chroma reconstruction) in our per-sample lighting shader, reading from the raw MSAA GBuffer rather than the repacked version.

The chroma reconstruction samples 4 neighbors for the missing chroma channel. In the per-sample path, each neighbor also needs to be read at sample frequency. Since we only do this for edge pixels (~5-10%) with 2 samples each, cost is manageable.

### 5.3. Keeping MSAA RTs alive through the pipeline

Currently `OnGBufferLeave` resolves and releases references. For true MSAA, the MSAA GBuffer must survive through lighting:

- Don't resolve color at GBuffer exit — only resolve depth (shadow pass needs single-sample depth)
- Keep MSAA color RT references alive
- Create SRVs for MSAA textures (need `D3D11_BIND_SHADER_RESOURCE` flag on creation)
- Resolve color **after** the per-sample lighting correction pass (at POST_LIGHTING)

### 5.4. Stencil management

Kenshi uses D24_UNORM_S8_UINT with stencil for its own purposes (stencil mask pass, draws 3642-3644). We need to coexist.

Options:
- Use bit 7 of the stencil (game likely uses lower bits)
- Use a separate R8_UNORM texture as "MSAA edge mask" (avoids stencil bit conflicts)
- Use a compute shader that writes the edge mask to a UAV

The separate texture approach is simplest — avoids stencil bit conflicts entirely. A fullscreen R8_UNORM costs negligible memory.

### 5.5. MSAA and forward passes

Water, fog, forward basic geometry render after deferred lighting into the HDR RT. If the HDR RT is MSAA, these passes get native MSAA for free. Water edges and fog volume boundaries become anti-aliased.

The sky renders between GBuffer and shadow map (draws 2584-2585). It would need to target the MSAA HDR RT too.

This requires creating an MSAA version of the R11G11B10 HDR RT and redirecting forward draws to it — similar to how we redirect GBuffer draws now.

---

## 6. Implementation Strategy

### Phase A: Fix the resolve timing (minimum viable improvement)

Move the MSAA resolve from GBuffer exit to after deferred lighting.

1. `OnGBufferLeave`: Only resolve depth. Keep MSAA color RTs alive.
2. After deferred lighting (POST_LIGHTING): Custom HDR-aware resolve of MSAA color to the game's HDR target.

**Limitation**: The game's deferred pass still reads from resolved (sample-0) repacked GBuffer, so lighting is still pixel-frequency. But the resolve now happens on a later stage, which is a necessary structural change for the later phases.

**Problem**: The game writes the lit result to a single-sample R11G11B10 HDR RT. We can't retroactively make that output MSAA. The MSAA color RTs contain GBuffer data, not lit data. This phase alone doesn't achieve per-sample lighting — it's a structural refactor that enables Phase B.

### Phase B: Per-sample lighting correction (recommended approach)

Game does its normal lighting on resolved data. A custom pass re-lights edge pixels per-sample.

1. **GBuffer pass**: MSAA as current. Keep MSAA RTs alive after depth-only resolve.
2. **Game's pipeline runs normally**: Repack, stencil, deferred lighting on resolved data. Game produces its standard lit R11G11B10 HDR result.
3. **POST_LIGHTING injection**:
   - a) Edge detection: compare MSAA GBuffer depth/normal subsamples → write edge mask to R8 texture.
   - b) For edge pixels only: decode YCoCg per-sample from MSAA GBuffer, compute BRDF per-sample, write per-sample lit result into a separate MSAA R11G11B10 RT.
   - c) Custom HDR-aware resolve of the MSAA edge pixels.
   - d) Composite: replace edge pixels in the game's HDR RT with the resolved MSAA result.

**Advantage**: Doesn't require modifying the game's deferred pass. The game runs its standard pipeline, and we overlay corrected edge pixels on top.

**Cost**: For edge pixels (~5-10% of screen), we compute the full BRDF with 2 evaluations per edge pixel. At 1080p with 8% edge pixels = ~166K pixels x 2 samples = 332K BRDF evaluations. Trivial for a modern GPU.

### Phase C: Full integration (maximum quality)

Patch the deferred shader to support per-sample mode. Two compiled variants. The game's single lighting draw becomes two draws (pixel-freq + sample-freq with edge mask). Gives perfect per-sample lighting using the game's full BRDF, shadows, environment lighting — not a custom reimplementation.

This requires:
- Compiling `main_fs` twice (pixel-freq variant, sample-freq variant with `SV_SampleIndex`)
- Hooking the deferred lighting draw to dispatch two passes with edge mask
- Providing MSAA GBuffer SRVs to the sample-freq variant
- Managing the MSAA HDR output RT

---

## 7. Key Shader Code

### HDR-Aware Resolve (AMD Reversible Tonemapper)

```hlsl
Texture2DMS<float4, 2> msaaHDR : register(t0);

float4 main(float4 pos : SV_Position) : SV_Target {
    int2 coord = int2(pos.xy);

    float3 s0 = msaaHDR.Load(coord, 0).rgb;
    float3 s1 = msaaHDR.Load(coord, 1).rgb;

    float w0 = 1.0 / (1.0 + max(s0.r, max(s0.g, s0.b)));
    float w1 = 1.0 / (1.0 + max(s1.r, max(s1.g, s1.b)));

    float3 result = (s0 * w0 + s1 * w1) / (w0 + w1);
    return float4(result, 1);
}
```

Standard box-filter resolve destroys specular highlights in HDR. A pixel at a triangle edge where one subsample is background (0.1) and the other is a specular highlight (50.0) produces `(0.1 + 50.0) / 2 = 25.05` — dominated by the single bright sample, creating bright fringing. The luminance-weighted resolve suppresses this.

### Edge Detection

```hlsl
Texture2DMS<float, 2> msaaDepth : register(t0);
Texture2DMS<float4, 2> msaaNormals : register(t1);

float main(float4 pos : SV_Position) : SV_Target {
    int2 coord = int2(pos.xy);

    float d0 = msaaDepth.Load(coord, 0);
    float d1 = msaaDepth.Load(coord, 1);

    float3 n0 = msaaNormals.Load(coord, 0).rgb * 2.0 - 1.0;
    float3 n1 = msaaNormals.Load(coord, 1).rgb * 2.0 - 1.0;

    float depthDiff = abs(d0 - d1) / max(min(d0, d1), 0.0001);
    float normalDiff = 1.0 - dot(n0, n1);

    bool isEdge = depthDiff > 0.02 || normalDiff > 0.1;
    return isEdge ? 1.0 : 0.0;
}
```

### YCoCg Per-Sample Decode

```hlsl
float3 DecodeAlbedoMSAA(Texture2DMS<float4, 2> gbuf0MS, int2 coord, uint sampleIdx) {
    float4 g = gbuf0MS.Load(coord, sampleIdx);
    float Y = g.r;

    // Checkerboard: even pixels have Cg, odd have Co
    bool even = ((coord.x + coord.y) % 2) == 0;
    float stored = g.g;

    // Reconstruct missing chroma from neighbors (same sample index)
    // Simplified — full implementation should use luminance-weighted
    // 4-neighbor averaging as in the game's decodePixel
    float neighbor = gbuf0MS.Load(coord + int2(1, 0), sampleIdx).g;

    float Co, Cg;
    if (even) {
        Cg = stored;
        Co = neighbor;
    } else {
        Co = stored;
        Cg = neighbor;
    }

    // YCoCg -> RGB
    float tmp = Y - Cg;
    float R = tmp + Co;
    float G = Y + Cg;
    float B = tmp - Co;
    return float3(R, G, B);
}
```

### Per-Sample Lighting (Phase B overlay approach)

```hlsl
Texture2DMS<float4, 2> gbuf0MS : register(t0); // albedo+metalness+gloss
Texture2DMS<float4, 2> gbuf1MS : register(t1); // normals+emissive
Texture2DMS<float, 2>  gbuf2MS : register(t2); // linear depth
Texture2D<float>       edgeMask : register(t3); // 1.0 at edge pixels

// Sun/light uniforms from game CB
cbuffer LightParams : register(b0) {
    float3 sunDirection;
    float3 sunColour;
    // ... remaining deferred uniforms
};

struct PSOutput {
    float4 color : SV_Target;
};

PSOutput main(float4 pos : SV_Position, uint sampleIdx : SV_SampleIndex) {
    PSOutput o;
    int2 coord = int2(pos.xy);

    // Only process edge pixels
    if (edgeMask.Load(int3(coord, 0)).r < 0.5) {
        discard;
    }

    // Read per-sample GBuffer data
    float3 albedo = DecodeAlbedoMSAA(gbuf0MS, coord, sampleIdx);
    float metalness = gbuf0MS.Load(coord, sampleIdx).b;
    float gloss = gbuf0MS.Load(coord, sampleIdx).a;

    float3 normal = gbuf1MS.Load(coord, sampleIdx).rgb * 2.0 - 1.0;
    normal = normalize(normal);

    float depth = gbuf2MS.Load(coord, sampleIdx);

    // Reconstruct world position from depth
    // ... (using inverse view/proj from game CB)

    // Compute BRDF (matching game's GGX + Lambert)
    float roughness = 1.0 - gloss * 0.99;
    float alpha = roughness * roughness;
    float3 specColor = lerp(0.04, albedo, metalness);
    albedo = lerp(albedo, 0.0, metalness);

    // ... GGX specular + Lambert diffuse + environment IBL
    // Must match the game's deferred.hlsl BRDF exactly

    o.color = float4(litResult, 1);
    return o;
}
```

### D3D11 Resource Setup

```cpp
// Add SRV bind flag to MSAA RTs (currently only BIND_RENDER_TARGET)
desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

// Create Texture2DMS SRVs for per-sample reads
D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
device->CreateShaderResourceView(sMSAARTs[0], &srvDesc, &sMSAAGBufSRVs[0]);
```

---

## 8. D3D11 MSAA API Reference

### Texture2DMS

Multisampled texture type in HLSL. Only supports `.Load(int2 coord, int sampleIndex)` — no filtering, no `Sample()`.

```hlsl
Texture2DMS<float4, 4> gbufferAlbedoMS : register(t0);
float4 s0 = gbufferAlbedoMS.Load(pixelCoord, 0);
float4 s1 = gbufferAlbedoMS.Load(pixelCoord, 1);
```

### SV_SampleIndex

When declared as a pixel shader input, forces the shader to run at **sample frequency** — once per covered subsample. The GPU cannot selectively run some pixels at pixel frequency and others at sample frequency within a single draw call. This is why the two-pass approach (pixel-freq pass + sample-freq pass with masking) is necessary.

```hlsl
float4 PSMain(float4 pos : SV_Position, uint sampleIdx : SV_SampleIndex) : SV_Target {
    float4 gbData = gbufferMS.Load(int2(pos.xy), sampleIdx);
    return ComputeLighting(gbData);
}
```

### SV_Coverage

Pixel shader input: bitmask of which samples are covered by the current triangle. Can be used during GBuffer pass to detect edge pixels for free.

```hlsl
float4 PSMain(float4 pos : SV_Position, uint coverage : SV_Coverage) : SV_Target {
    bool isEdge = (coverage != 0x3); // For 2x MSAA, full coverage = 0x3
}
```

As a pixel shader output: lets you reduce coverage (mask off samples) but cannot add coverage.

### EvaluateAttributeAtSample

Interpolates a pixel shader input at a specific subsample's position, rather than pixel center. Critical for getting correct texture coordinates at subsample positions.

```hlsl
float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0,
              uint sampleIdx : SV_SampleIndex) : SV_Target {
    float2 sampleUV = EvaluateAttributeAtSample(uv, sampleIdx);
    return texAlbedo.Sample(samLinear, sampleUV);
}
```

### centroid interpolation

Clamps interpolation point to the nearest covered sample, preventing extrapolation artifacts at triangle edges where a subsample position falls outside the triangle.

```hlsl
struct PSInput {
    float4 pos : SV_Position;
    centroid float2 uv : TEXCOORD0;
    centroid float3 normal : NORMAL;
};
```

### Creating MSAA resources

```cpp
// Check support
UINT qualityLevels = 0;
device->CheckMultisampleQualityLevels(format, sampleCount, &qualityLevels);

// Create texture
D3D11_TEXTURE2D_DESC desc = {};
desc.SampleDesc.Count = sampleCount;
desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

// SRV for Texture2DMS reads
D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;

// DSV for MSAA depth
D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;

// ResolveSubresource works for color (box filter), NOT for depth
context->ResolveSubresource(dstTex, 0, srcMSAATex, 0, format);
```

---

## 9. SMAA S2x Integration

### How SMAA multisampling modes work

**SMAA 1x**: Pure post-process morphological AA. No MSAA. Three passes: edge detection, blend weight calculation, neighborhood blending. This is what our current `DustSMAA.cpp` implements.

**SMAA S2x (Spatial Multisampling)**: Combines SMAA 1x with 2x MSAA.
1. Render scene with 2x MSAA.
2. Separate the two subsamples into two distinct buffers.
3. Run SMAA 1x independently on each subsample buffer.
4. Average the two SMAA'd results.

Each MSAA subsample is at a different subpixel position. SMAA's area texture lookup uses `subsampleIndices` to account for these subpixel offsets, producing morphological corrections aware of the MSAA sample pattern.

**SMAA T2x / SMAA 4x**: Temporal. Not applicable (no temporal techniques).

### Implementation

For 2x MSAA with D3D11 standard sample positions:
- Sample 0: (+0.25, +0.25) from pixel center
- Sample 1: (-0.25, -0.25) from pixel center

The `subsampleIndices` parameter in `SMAABlendingWeightCalculationPS` encodes which subsample is being processed, allowing the area texture lookup to account for the known subpixel offset.

Simplest integration: run SMAA on the resolved (post-HDR-resolve) image with standard `subsampleIndices = float4(0, 0, 0, 0)`. This still benefits from the resolved MSAA data — edges are already partially smoothed by the 2x resolve, and SMAA cleans up the remaining stairstepping.

Full S2x: resolve each subsample separately, SMAA each with correct subsample indices, average. Higher quality but 2x the SMAA cost.

---

## 10. The GBuffer Size Problem

### Memory cost

With 2x MSAA at 1080p:

| RT | Format | 1x Size | 2x MSAA Size |
|----|--------|---------|--------------|
| RT0 (Albedo) | B8G8R8A8_UNORM | 7.9 MB | 15.8 MB |
| RT1 (Normals) | B8G8R8A8_UNORM | 7.9 MB | 15.8 MB |
| RT2 (Depth) | R32_FLOAT | 7.9 MB | 15.8 MB |
| DS (D24_S8) | D24_UNORM_S8_UINT | 7.9 MB | 15.8 MB |
| **Total** | | **31.6 MB** | **63.2 MB** |

At 1440p with 2x MSAA: ~112 MB. At 4K with 2x MSAA: ~252 MB.

For comparison, 4x MSAA doubles these numbers again.

### Bandwidth

GBuffer writes touch 2x the memory with 2x MSAA. Most GBuffer bandwidth is texture reads (material textures), not ROP writes, so the impact is moderate — typically 15-25% increase on the GBuffer pass.

---

## 11. Performance Expectations

For 2x MSAA at 1080p:

| Cost | Estimate | Notes |
|------|----------|-------|
| GBuffer memory | +32 MB | 2x the GBuffer RTs + depth |
| GBuffer bandwidth | +15-25% | More ROP traffic |
| GBuffer raster cost | ~0% | Rasterizer handles MSAA natively; VS/PS still run once per pixel |
| Edge detection pass | ~0.1 ms | Simple fullscreen compare of 2 samples |
| Per-sample lighting | ~0.1-0.3 ms | Only ~5-10% of pixels, 2 BRDF evals each |
| HDR resolve | ~0.1 ms | Fullscreen, 2 texture loads + weighted average |
| SMAA (unchanged) | ~0.5-1 ms | Same as current |
| **Total overhead** | **~0.5-1.5 ms** | At 1080p on a modern GPU |

Threat Interactive's analysis: at 1080p, 2x MSAA + morphological AA produces quality approaching 4x MSAA. The combination is significantly cheaper than 4x MSAA alone, where the GBuffer would cost 4x memory and the per-sample lighting pass would have 4 evaluations per edge pixel instead of 2.

---

## 12. What NOT to Do (Lessons from Crysis 3)

1. **Don't resolve before lighting** — this is what the current code does and why MSAA produces no visible improvement.
2. **Don't use a faulty stencil mask** — missing edges means wasted per-sample compute on wrong pixels, visible aliasing on missed edges. Use depth + normal comparison, not just depth.
3. **Don't composite SSAO at pixel frequency onto MSAA lit data** — apply AO after resolve instead. AO is low-frequency and spatially filtered; per-sample error at edges is below perceptual threshold.
4. **Don't use hardware box-filter resolve on HDR data** — specular highlights create bright fringing. Use the AMD reversible tonemapper.
5. **Don't go above 2x MSAA at 1080p+ without proving you need it** — diminishing returns, and SMAA covers the gap.
6. **Don't use temporal MSAA (MFAA, persistence-based, alternating sample patterns)** — fundamentally temporal, produces ghosting and flickering.

---

## 13. Open Questions

1. **Can we add SRV bind flags to the MSAA RTs?** Currently `sMSAARTs[]` are created with `D3D11_BIND_RENDER_TARGET` only. For the per-sample lighting path, we need `D3D11_BIND_SHADER_RESOURCE` too. Easy fix — add the bind flag in `CreateResources()`.

2. **What does the game's stencil mask (draws 3642-3644) actually do?** The docs say it generates a stencil mask for the deferred lighting pass, but exact stencil bit usage needs verification. If the game uses specific bits, we need non-conflicting bits for our edge mask — or use a separate R8 texture.

3. **Can the game's GBuffer repack handle MSAA input?** The repack shader is pre-compiled. If it reads from `Texture2D` (not `Texture2DMS`), it can only see sample 0 of the resolved data. Our per-sample path must do its own YCoCg decode.

4. **How does the cubemap pass interact?** The cubemap renders at 512x512. If we're redirecting GBuffer RTVs to MSAA, we must only redirect the main GBuffer (native resolution), not the cubemap GBuffer. The resolution check in `GeometryCapture::IsGBufferConfig` already handles this.

5. **How do we handle the MSAA HDR RT for forward passes?** If we want water/fog to benefit from MSAA, we need an MSAA version of the R11G11B10 HDR RT and redirect forward draws to it. Otherwise forward passes remain pixel-frequency and only deferred-lit geometry gets MSAA.

6. **Can we match the game's BRDF exactly in the overlay pass?** The per-sample lighting correction (Phase B) needs to produce identical results to the game's `main_fs` for interior pixels. Any BRDF mismatch will create visible seams between game-lit pixels and our re-lit edge pixels. We have the deferred shader source (captured via D3DCompile hook), so we can replicate it — but must verify.

7. **How do we handle the linear blending problem?** Threat Interactive mentioned that most games suffer from dark highlights between MSAA edges, fixable with linear blending. Need to verify whether our resolve produces this artifact and if the HDR-aware resolve already addresses it.

---

## 14. References

- Threat Interactive — "Why MSAA Should Be In EVERY Deferred Renderer" (YouTube)
- AMD GPUOpen — "Optimized Reversible Tonemapper for Resolve"
- MJP — "Deferred MSAA" (therealmjp.github.io)
- MJP — "Experimenting with Reconstruction Filters for MSAA Resolve"
- MJP — "A Quick Overview of MSAA"
- NVIDIA — "Antialiased Deferred Rendering" sample
- Guerrilla Games — "Deferred Rendering in Killzone 2" (GDC 2009)
- CryEngine — "Graphics Gems from CryENGINE 3" (SIGGRAPH 2013)
- CryEngine — "Anti-Aliasing Methods in CryENGINE 3"
- CryEngine — "Rendering Technologies from Crysis 3" (GDC 2013)
- Brian Karis — "Tone Mapping" (Graphics Rants)
- TheAgentD — "HDR Inverse Tone Mapping MSAA Resolve"
- Filmic Worlds — "Decoupled Visibility Multisampling"
- SMAA — "Enhanced Subpixel Morphological Antialiasing" (iryoku.com)
- NVIDIA Research — "Subpixel Reconstruction Antialiasing for Deferred Shading" (2011)
- Diary of a Graphics Programmer — "MSAA on PS3 with Deferred Lighting"
