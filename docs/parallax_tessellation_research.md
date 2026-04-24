# Parallax Occlusion Mapping & Tessellation Research

*Date: 2026-04-24*

Deep research on implementing parallax occlusion mapping (POM) and hardware tessellation in Kenshi's render pipeline via the Dust framework.

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Current Pipeline State](#2-current-pipeline-state)
3. [Technique Overview: POM vs Tessellation](#3-technique-overview-pom-vs-tessellation)
4. [Approach A: POM-Only (Pixel Shader)](#4-approach-a-pom-only-pixel-shader)
5. [Approach B: Tessellation + Displacement](#5-approach-b-tessellation--displacement)
6. [Approach C: Combined POM + Tessellation](#6-approach-c-combined-pom--tessellation)
7. [Per-Material Analysis](#7-per-material-analysis)
8. [Framework Changes Required](#8-framework-changes-required)
9. [Height Map Sourcing](#9-height-map-sourcing)
10. [GBuffer Depth Correction](#10-gbuffer-depth-correction)
11. [Self-Shadowing](#11-self-shadowing)
12. [Performance Analysis](#12-performance-analysis)
13. [Recommended Implementation Plan](#13-recommended-implementation-plan)
14. [References](#14-references)

---

## 1. Executive Summary

Three approaches exist for adding surface depth detail to Kenshi's materials:

| Approach | Visual Quality | Silhouettes | Perf Cost | Complexity |
|----------|---------------|-------------|-----------|------------|
| **A: POM-only** | Good (flat surfaces) | Incorrect | Medium | Low |
| **B: Tessellation + displacement** | Excellent | Correct | Medium-High | High |
| **C: POM + tessellation** | Best | Correct | Highest | Highest |

**Recommendation:** Start with **Approach A (POM-only)** for terrain and objects. It delivers 80% of the visual improvement with the least framework complexity. Tessellation can be added later as a second phase, building on existing shadow tessellation infrastructure (`rtwtessellator.hlsl`).

**Key finding:** Kenshi already uses SM5.0 tessellation for 52% of shadow draws via `rtwtessellator.hlsl` with `PATCHLIST_3_CP` topology. This proves the engine's D3D11 path supports tessellation — the HS/DS pipeline works. The challenge is extending this from the shadow pass to the GBuffer pass.

---

## 2. Current Pipeline State

### What exists today

**Tessellation in shadow pass (proven working):**
- Source: `rtwtessellator.hlsl` (~148 lines)
- Topology: `PATCHLIST_3_CP` (3 control points per patch = triangle patches)
- 548 tessellated draws out of 1,051 shadow draws (52.1%)
- Screen-space edge length tessellation factor
- Single HS address `0x66B35DB8`, single DS address `0x66B366F8`

**GBuffer materials (no tessellation today):**
| Material | Source | GBuffer Draws | Topology |
|----------|--------|--------------|----------|
| Terrain | `terrainfp4.hlsl` | 120 (10.2%) | TRIANGLESTRIP (100) / TRIANGLELIST (20) |
| Objects | `objects.hlsl` | 455 (38.7%) | TRIANGLELIST (451) / TRIANGLESTRIP (4) |
| Triplanar | `triplanar.hlsl` | 3 (0.3%) | TRIANGLELIST |
| Skin | `skin.hlsl` | 292 (24.8%) | TRIANGLELIST |

**Texture slots per material (available for height data):**
| Material | s0 | s1 | s2 | s3 | s4 |
|----------|----|----|----|----|-----|
| Objects | diffuse+alpha | normal | colorMap(B=metal) | dustMap | secondaryDiffuse |
| Terrain | Texture2DArray (6 layers per biome) | — | — | — | — |
| Triplanar | diffuse | normal | — | — | — |

**Draw call interception:** `HookedDrawIndexed` and `HookedDrawIndexedInstanced` capture GBuffer draws. GeometryCapture stores full IA/VS/PS state. GeometryReplay re-issues captured draws with modified constant buffers.

**Shader compilation interception:** `D3DCompile` hook allows runtime HLSL patching (used today for shadow filtering injection into `deferred.hlsl`).

---

## 3. Technique Overview: POM vs Tessellation

### Parallax Occlusion Mapping (POM)

A pixel shader technique that ray-marches through a height field in tangent space to find the correct texture coordinate for each pixel. No geometry is added.

**Algorithm (per pixel):**
1. Compute view direction in tangent space
2. Step along the view ray through the height field (min/max samples based on view angle)
3. Find intersection with height profile (piecewise-linear interpolation)
4. Offset texture coordinates to the intersection point
5. Sample albedo, normal, etc. at the offset coordinates
6. Optionally: second ray march toward light for self-shadowing

**Strengths:**
- Pure pixel shader — no topology or pipeline changes
- Works with any existing geometry
- Fine detail without extra triangles
- Widely understood, battle-tested technique

**Weaknesses:**
- Silhouettes remain flat (geometry isn't modified)
- At steep viewing angles, stepping artifacts appear
- Performance scales with screen coverage, not geometry complexity
- Depth buffer mismatch in deferred rendering (requires correction)

### Hardware Tessellation (DX11)

The DX11 tessellation pipeline adds two programmable shader stages (Hull Shader, Domain Shader) and one fixed-function stage (Tessellator) between the vertex and pixel shaders:

```
VS → HS → Tessellator (fixed) → DS → PS
```

**Hull Shader (HS):** Runs per-patch. Two functions:
1. Main function: processes control points (runs per output control point)
2. Patch constant function: calculates tessellation factors (runs once per patch)

**Tessellator:** Fixed-function stage. Subdivides patches based on tessellation factors. Outputs barycentric coordinates (u,v) for the Domain Shader.

**Domain Shader (DS):** Runs per generated vertex. Interpolates control point data using barycentric coordinates, then applies displacement from a height map.

**Key DX11 requirements:**
- Input topology must be `D3D11_PRIMITIVE_TOPOLOGY_N_CONTROL_POINT_PATCHLIST`
- For triangles: `PATCHLIST_3_CP` (same as Kenshi's shadow tessellation)
- HS attributes: `[domain("tri")]`, `[partitioning("fractional_odd")]`, `[outputtopology("triangle_cw")]`, `[outputcontrolpoints(3)]`
- DS receives: tessellation factors + control points + barycentric UV
- Max tessellation factor: 64

**Strengths:**
- True geometry modification — correct silhouettes
- Correct depth buffer (actual displaced geometry)
- Correct shadow casting (displaced geometry casts real shadows)
- Efficient GPU-side subdivision (no CPU geometry processing)

**Weaknesses:**
- Requires topology change (TRIANGLELIST → PATCHLIST_3_CP)
- Needs HS/DS shader stages bound to pipeline
- Over-tessellation can reduce performance (need adaptive LOD)
- Height maps required

---

## 4. Approach A: POM-Only (Pixel Shader)

### Implementation via D3DCompile Patching

The cleanest approach is to patch material pixel shaders at compile time via the existing `D3DCompile` hook. When `terrainfp4.hlsl` or `objects.hlsl` is compiled, inject POM code before the texture sampling.

### POM Algorithm for Kenshi Materials

```hlsl
// POM parameters (could be per-material via CB)
static const float  g_heightScale    = 0.04;  // displacement magnitude in world units
static const int    g_minSamples     = 8;     // samples at perpendicular view
static const int    g_maxSamples     = 32;    // samples at grazing angles

float2 ParallaxOcclusionMapping(float2 texCoords, float3 viewDirTS, Texture2D heightMap,
                                 SamplerState samp)
{
    // Dynamic step count based on view angle
    float numSteps = lerp(g_maxSamples, g_minSamples, abs(viewDirTS.z));
    float stepSize = 1.0 / numSteps;

    // Step direction along view ray projected onto surface
    float2 deltaUV = viewDirTS.xy * g_heightScale / (viewDirTS.z * numSteps);

    float2 currUV     = texCoords;
    float  currHeight = heightMap.SampleLevel(samp, currUV, 0).r;
    float  currLayer  = 0.0;

    // Ray march through height field
    [loop]
    while (currLayer < currHeight)
    {
        currUV    -= deltaUV;
        currHeight = heightMap.SampleLevel(samp, currUV, 0).r;
        currLayer += stepSize;
    }

    // Linear interpolation between last two samples for precision
    float2 prevUV    = currUV + deltaUV;
    float  prevLayer = currLayer - stepSize;
    float  prevHeight = heightMap.SampleLevel(samp, prevUV, 0).r;

    float afterDepth  = currHeight - currLayer;
    float beforeDepth = prevHeight - prevLayer;
    float weight = afterDepth / (afterDepth - beforeDepth);

    return lerp(currUV, prevUV, weight);
}
```

### Required Shader Modifications

**For `objects.hlsl`:**
1. Compute tangent-space view direction from existing tangent/bitangent/normal
2. Sample height from `normalMap.a` (height packed in normal map alpha — standard practice)
3. Call POM to get offset UV before all texture sampling
4. Write corrected depth to GBuffer RT2

**For `terrainfp4.hlsl`:**
1. More complex — terrain uses `Texture2DArray` with 6 layers per biome
2. POM needs to be applied per-layer before blending
3. Performance concern: 6 layers × N samples per ray = expensive
4. Alternative: POM on the final blended height only (one ray march using blended height)
5. Height data: likely the alpha channel of diffuse layers, or could generate from normal maps

**For `triplanar.hlsl`:**
1. World-space UV projection complicates tangent-space POM
2. Need per-axis POM with axis-aligned tangent frames
3. Only 3 draws per frame — low priority

### Tangent-Space View Direction

Objects in Kenshi have tangent-space data in their vertex format (normal maps require it). The view direction in tangent space:

```hlsl
// In VS or PS (if interpolated from VS):
float3 T = normalize(input.tangent);
float3 B = normalize(input.bitangent);
float3 N = normalize(input.normal);
float3 viewDir = normalize(cameraPos - worldPos);

// Transform to tangent space
float3 viewDirTS;
viewDirTS.x = dot(viewDir, T);
viewDirTS.y = dot(viewDir, B);
viewDirTS.z = dot(viewDir, N);
```

### Terrain-Specific Considerations

Terrain is the highest-impact target for POM. Key challenges:

1. **Texture2DArray sampling:** Each biome uses a Texture2DArray with 6 terrain layers. POM needs to ray-march through a per-layer height field. Two strategies:
   - **Per-layer POM:** Ray-march each active layer independently, blend results. Expensive but accurate — each material layer has its own depth profile.
   - **Blended-height POM:** Compute a single blended height from all active layers' weight maps, ray-march once. Cheaper but loses per-layer depth variation.

2. **Height-blend interaction:** Terrain already uses height-blend between layers (`BLEND1`-`BLEND3` defines). POM height data should be the same height field used for height-blending — this creates consistency where the visually "higher" material is also the one that protrudes in parallax.

3. **LOD:** Terrain tiles at distance should skip POM (fade out based on distance or pixel density). The terrain shader already has `distance fade` logic that can gate POM.

4. **TRIANGLESTRIP topology:** 100 of 120 terrain draws use TRIANGLESTRIP. POM doesn't care about topology (it's PS-only), so this isn't a problem.

---

## 5. Approach B: Tessellation + Displacement

### Leveraging Existing Shadow Tessellation

Kenshi's shadow pass already implements the full tessellation pipeline via `rtwtessellator.hlsl`. The exact same pattern can be adapted for GBuffer draws:

**Shadow tessellation (what exists):**
- Topology: `PATCHLIST_3_CP`
- HS: Screen-space edge length tessellation factor calculation
- DS: Barycentric interpolation of control point positions
- 548 draws per frame at `0x66B35DB8` (HS) / `0x66B366F8` (DS)

**GBuffer tessellation (what we'd build):**
- Same topology change: TRIANGLELIST → PATCHLIST_3_CP
- HS: Similar screen-space adaptive tessellation factors + distance-based LOD
- DS: Interpolate vertex attributes (position, normal, UV, tangent) + sample height map + displace along normal
- PS: Existing material PS unchanged (receives displaced geometry)

### Hull Shader Design

```hlsl
// For triangle domain (matching existing shadow tessellation)
struct HullInput
{
    float4 position : SV_Position;   // world-space position from VS
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float3 tangent  : TANGENT;
    // ... other VS outputs
};

struct PatchConstants
{
    float edges[3]  : SV_TessFactor;
    float inside    : SV_InsideTessFactor;
};

// Adaptive tessellation factor based on screen-space edge length
float ComputeEdgeTessFactor(float3 p0, float3 p1, float4x4 viewProj,
                             float2 screenSize, float targetEdgeLength)
{
    float4 c0 = mul(float4(p0, 1), viewProj);
    float4 c1 = mul(float4(p1, 1), viewProj);
    float2 s0 = (c0.xy / c0.w) * 0.5 * screenSize;
    float2 s1 = (c1.xy / c1.w) * 0.5 * screenSize;
    float edgeLength = distance(s0, s1);
    return clamp(edgeLength / targetEdgeLength, 1.0, 64.0);
}

// Distance-based falloff to avoid over-tessellation at distance
float DistanceFade(float3 patchCenter, float3 cameraPos, float fadeStart, float fadeEnd)
{
    float dist = distance(patchCenter, cameraPos);
    return 1.0 - saturate((dist - fadeStart) / (fadeEnd - fadeStart));
}

PatchConstants PatchConstantFunc(InputPatch<HullInput, 3> patch, uint patchId : SV_PrimitiveID)
{
    PatchConstants output;

    float3 center = (patch[0].position.xyz + patch[1].position.xyz + patch[2].position.xyz) / 3.0;
    float fade = DistanceFade(center, cameraPos, 50.0, 200.0);

    if (fade <= 0.0)
    {
        // Beyond fade range — no tessellation
        output.edges[0] = output.edges[1] = output.edges[2] = 1.0;
        output.inside = 1.0;
        return output;
    }

    float e0 = ComputeEdgeTessFactor(patch[1].position.xyz, patch[2].position.xyz,
                                      viewProj, screenSize, targetEdgePixels);
    float e1 = ComputeEdgeTessFactor(patch[2].position.xyz, patch[0].position.xyz,
                                      viewProj, screenSize, targetEdgePixels);
    float e2 = ComputeEdgeTessFactor(patch[0].position.xyz, patch[1].position.xyz,
                                      viewProj, screenSize, targetEdgePixels);

    output.edges[0] = max(1.0, e0 * fade);
    output.edges[1] = max(1.0, e1 * fade);
    output.edges[2] = max(1.0, e2 * fade);
    output.inside   = max(1.0, (e0 + e1 + e2) / 3.0 * fade);

    return output;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstantFunc")]
HullInput HullMain(InputPatch<HullInput, 3> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];  // Pass-through for simple displacement
}
```

### Domain Shader Design

```hlsl
Texture2D    heightMap : register(t0);   // bound to tessellation texture slot
SamplerState heightSamp : register(s0);

[domain("tri")]
DomainOutput DomainMain(PatchConstants patchData,
                         float3 bary : SV_DomainLocation,
                         const OutputPatch<HullInput, 3> patch)
{
    DomainOutput output;

    // Barycentric interpolation of all vertex attributes
    float3 worldPos = bary.x * patch[0].position.xyz
                    + bary.y * patch[1].position.xyz
                    + bary.z * patch[2].position.xyz;

    float3 normal = normalize(bary.x * patch[0].normal
                            + bary.y * patch[1].normal
                            + bary.z * patch[2].normal);

    float2 uv = bary.x * patch[0].uv
              + bary.y * patch[1].uv
              + bary.z * patch[2].uv;

    // Sample height map and displace along normal
    float height = heightMap.SampleLevel(heightSamp, uv, 0).r;
    worldPos += normal * height * heightScale;

    // Transform displaced position to clip space
    output.position = mul(float4(worldPos, 1), viewProj);
    output.worldPos = worldPos;
    output.normal   = normal;
    output.uv       = uv;
    // ... pass through other attributes

    return output;
}
```

### Draw Call Modification

For tessellation, each GBuffer draw call must be modified:

1. **Topology:** Change from `TRIANGLELIST`/`TRIANGLESTRIP` to `PATCHLIST_3_CP`
   - TRIANGLESTRIP requires conversion: each strip generates individual triangle patches
   - This is a problem for terrain (100/120 draws are TRIANGLESTRIP)
   - Options: (a) convert strips to lists at capture time, or (b) skip strip draws for tessellation

2. **Bind HS/DS:** Set hull shader and domain shader before each draw

3. **Bind height map:** Need a free texture slot for the height map SRV in the DS stage
   - DS has its own texture register space (separate from PS)
   - Ogre3D's D3D11 render system supports texture units in tessellation stages

4. **Constant buffer:** DS needs viewProj, camera position, height scale, screen size
   - Can reuse the VS constant buffer (already contains viewProj at `clipMatrixOffset`)
   - Or bind a separate CB to the DS

### TRIANGLESTRIP Problem

100 of 120 terrain draws use TRIANGLESTRIP topology. Tessellation requires PATCHLIST topology, which has no strip equivalent. Options:

1. **Skip TRIANGLESTRIP draws for tessellation:** Only tessellate the 20 TRIANGLELIST draws. Misses 83% of terrain draws. Not viable.

2. **Convert to TRIANGLELIST at capture time:** When GeometryCapture records a TRIANGLESTRIP draw, expand the index buffer into individual triangles and store as TRIANGLELIST. Then change topology to PATCHLIST_3_CP for tessellated replay.
   - Requires reading the index buffer on CPU (or compute shader)
   - Increases index count (~2x for strips)
   - One-time cost per unique mesh (cache the converted buffer)

3. **Intercept before the original draw, not via replay:** Instead of post-hoc replay, intercept each GBuffer draw in `HookedDrawIndexed` and modify the pipeline state in-place before calling the original. This avoids the replay system entirely.
   - Change topology in-place
   - Bind HS/DS
   - After original draw, unbind HS/DS and restore topology
   - Most flexible but most invasive

4. **Use geometry shader to expand strips:** A GS could consume strip-topology triangles and output individual triangles. But GS is slower than tessellation for this purpose — defeats the goal.

**Recommended:** Option 3 (in-place interception) for the tessellation phase. It sidesteps the strip conversion problem entirely because the IA topology change from TRIANGLELIST/STRIP to PATCHLIST_3_CP happens before the draw call, and the tessellator handles the rest.

Wait — actually, PATCHLIST_3_CP requires that the index buffer contains groups of 3 indices (like TRIANGLELIST), not strips. So TRIANGLESTRIP topology fundamentally cannot be reinterpreted as PATCHLIST_3_CP without index buffer expansion. **Option 2 is necessary for strip draws.**

---

## 6. Approach C: Combined POM + Tessellation

The DX SDK `DetailTessellation11` sample demonstrates the gold-standard combined approach:

1. **Tessellation** handles macro-level displacement (coarse geometry correction)
   - Adaptive tessellation factors based on screen-space edge length
   - Domain shader displaces vertices along normals using a low-frequency height map
   - Corrects silhouettes, depth buffer, shadow casting

2. **POM** handles micro-level detail (fine surface depth)
   - Applied in the pixel shader after tessellation
   - Uses a high-frequency height map (can be the same texture at higher mip levels)
   - Provides sub-triangle detail without excessive tessellation
   - Self-shadowing via second ray march toward light

**Advantage:** The tessellation provides enough geometric correction that POM only needs to handle small-scale detail, reducing POM artifacts (silhouette issues are gone because tessellation handles silhouettes, POM just adds bump detail).

**When to use:** When the height displacement is large enough to be visible at silhouettes (rocky terrain, brick walls, cobblestones). For subtle surface detail (wood grain, fabric weave), POM alone is sufficient.

---

## 7. Per-Material Analysis

### Terrain (`terrainfp4.hlsl`) — HIGHEST PRIORITY

**Impact:** Terrain is always visible, covers most of the screen. Desert sand dunes, rocky outcrops, fertile ground — all benefit enormously from surface depth.

**POM approach:**
- Height data: alpha channel of terrain diffuse layers in the Texture2DArray
- One POM ray march per active terrain layer, or one march on blended height
- LOD: fade POM out at distance (terrain already has distance fade)
- Performance: terrain fills large screen areas — POM cost is proportional to fill rate

**Tessellation approach:**
- TRIANGLESTRIP → PATCHLIST conversion required (100/120 draws)
- Terrain mesh is already subdivided (8,572 indices per draw) — moderate base density
- Displacement: sample height from terrain layers, blend per biome weights
- LOD: distance-based tessellation factor fade-out

**Recommendation:** Start with POM. Terrain is rendered at large scale where silhouettes are less noticeable (RTS camera, rarely at steep angles to terrain). POM on terrain gives 90% of the visual win. Tessellation can be added later for close-up views.

### Objects (`objects.hlsl`) — HIGH PRIORITY

**Impact:** 455 draws (38.7% of GBuffer). Buildings, ruins, props, furniture.

**POM approach:**
- Height data: pack into `normalMap.a` (s1 alpha channel). Many game normal maps already have height in alpha.
- Standard tangent-space POM — objects have tangent/bitangent data
- LOD: fade based on distance or screen-space size
- Variant handling: 91 permutations via `#ifdef` — POM injection must work with all variants

**Tessellation approach:**
- TRIANGLELIST topology (451/455 draws) — directly compatible with PATCHLIST_3_CP
- Only 4 TRIANGLESTRIP draws to handle
- Variable vertex strides (44-96 bytes) — HS/DS must handle all stride variants
- Instanced draws: instance data in VB slot 1 — tessellation must preserve instancing

**Recommendation:** POM first. Objects have complex vertex formats and 91 shader variants — tessellation requires handling every permutation. POM is a PS-only change that works across all variants.

### Triplanar (`triplanar.hlsl`) — MEDIUM PRIORITY

**Impact:** Only 3 draws (0.3%), but these are large rock/cliff surfaces that dominate close-up views.

**POM approach:**
- World-space UV complicates tangent-space POM
- Need per-projection-axis POM with world-aligned tangent frames
- Blend POM results across axes like the existing triplanar normal blending

**Tessellation approach:**
- TRIANGLELIST only — directly compatible
- 28-byte vertex stride
- Few draws → low overhead even with high tessellation factors

**Recommendation:** POM with world-space adapted tangent frames. Low draw count means either technique is affordable.

### Skin/Character (`skin.hlsl`, `character.hlsl`) — LOW PRIORITY

**Impact:** Characters are small on screen (RTS camera). Skin pores, leather wrinkles at this scale are invisible.

**Recommendation:** Skip for now. If added later, POM on armor/clothing detail only. Skinned meshes with bone matrices add complexity for tessellation (DS must apply bone transforms before displacement).

### Foliage (`foliage.hlsl`) — NOT APPLICABLE

Foliage is billboard grass and alpha-tested leaves. POM/tessellation are not appropriate.

---

## 8. Framework Changes Required

### For POM-Only (Approach A)

**Minimal framework changes needed.** POM is purely a pixel shader modification.

1. **D3DCompile hook enhancement:** The existing hook intercepts `D3DCompile` calls. To inject POM, detect when `terrainfp4.hlsl` or `objects.hlsl` is being compiled (by source content hashing or entry point name), and prepend/append POM code before compilation.

2. **Height map binding:** Need to bind height map textures to PS slots. Options:
   - Use an existing unused slot (e.g., `s5` for objects without `DUAL_TEXTURE`)
   - Or pack height into normal map alpha (no extra slot needed — height is in the same texture as normals at `s1`)

3. **Per-material CB for POM parameters:** Height scale, min/max samples, LOD distances. Could be added to the existing VS CB or a new PS CB slot.

**If height is packed in normal map alpha:** Zero framework changes needed. The D3DCompile hook patches the PS source, and height data comes from the already-bound normal map at `s1`. This is the cleanest path.

### For Tessellation (Approach B)

**Significant framework changes needed:**

1. **Draw call modification in `HookedDrawIndexed`:**
   ```
   Before original draw:
   - Detect if draw is a tessellation candidate (by shader category, from ShaderMetadata)
   - If yes: change topology to PATCHLIST_3_CP
   - Bind compiled HS and DS shaders
   - Bind height map to DS texture slot
   - Bind tessellation CB (viewProj, camera pos, screen size, params)
   
   After original draw:
   - Unbind HS/DS (set to nullptr)
   - Restore original topology
   ```

2. **TRIANGLESTRIP conversion:** For terrain strips, need an index buffer expansion system:
   - On first encounter of a strip-topology draw, copy and expand the index buffer
   - Cache the expanded buffer keyed by original IB pointer + offset + count
   - Use the expanded buffer with PATCHLIST_3_CP topology

3. **HS/DS shader compilation and caching:** Compile HS/DS variants for each vertex format (stride, layout). Cache compiled shaders by input layout signature.

4. **New DustAPI functions (optional for effect plugins):**
   ```c
   // Enable tessellation for specific shader categories
   void (*SetTessellation)(DustShaderCategory category, int enabled);
   void (*SetTessellationParams)(DustShaderCategory category, float heightScale,
                                  float maxDistance, float targetEdgePixels);
   ```

5. **Geometry capture expansion:** `CapturedDraw` struct needs HS/DS fields if replay should preserve tessellation state. Currently only captures IA/VS/PS.

### For Combined (Approach C)

All changes from both A and B, plus coordination between DS displacement and PS POM to avoid double-displacement.

---

## 9. Height Map Sourcing

### Option 1: Normal Map Alpha (Recommended for objects)

Standard industry practice. Height is stored in the alpha channel of the tangent-space normal map. Many game assets ship with height in normal map alpha already.

**Kenshi's normal maps:** Format is BC3_UNORM (DXT5) or BC1_UNORM (DXT1). BC3 has an alpha channel that could contain height. BC1 has only 1-bit alpha — height cannot be stored here.

From the texture format stats:
- Objects: BC1_UNORM (1928 binds), BC3_UNORM (1389 binds)
- Some normal maps are BC3 (have alpha for height), some are BC1 (no alpha for height)

**Implication:** POM can be applied to objects whose normal maps are BC3 (have height in alpha) but must be skipped for BC1 normal maps. This can be detected at runtime by checking the texture format.

### Option 2: Separate Height Map Texture

Bind a separate height map at an unused texture slot. Requires:
- Generating or sourcing height maps for all materials
- Additional VRAM usage
- An extra SRV bind per draw

### Option 3: Derive Height from Normal Map (Fallback)

If no explicit height data exists, height can be approximated by integrating the normal map (Poisson reconstruction). This is an offline preprocessing step, not real-time. Quality is mediocre but better than nothing.

### Option 4: Terrain Layer Heights

Terrain `Texture2DArray` layers likely include height data (the terrain system uses height-blend, which implies per-layer heights exist). Need to verify by inspecting the actual terrain texture arrays in-game:
- Check if Texture2DArray layers have alpha channels with height data
- Check the terrain height-blend code path in `computeBiome()` for where it reads height

### Recommended Strategy

1. **Objects:** Check normal map format at draw time. If BC3, use alpha as height. If BC1, skip POM.
2. **Terrain:** Investigate Texture2DArray layer format. If height data exists, use it. If not, derive from normal maps offline.
3. **Future:** Ship custom height-enhanced texture packs for maximum quality.

---

## 10. GBuffer Depth Correction

### The Problem

In deferred rendering, POM offsets texture coordinates in the pixel shader, but the depth buffer still contains the depth of the flat polygon. This causes:
1. **SSAO/SSIL errors:** Screen-space effects read depth from GBuffer RT2 — if depth doesn't match the apparent surface, AO halos appear at depth discontinuities
2. **Depth testing artifacts:** Objects behind POM'd surfaces may incorrectly show through
3. **Shadow map mismatch:** CSM shadows are cast from flat geometry, not POM'd surface

### Solution: Write Corrected Depth

After POM computes the displaced position, calculate the corrected depth and write it:

```hlsl
// After POM ray march, we know the intersection depth along the view ray
float pomDepth = currLayer;  // 0..1 depth within height field

// Convert to world-space displacement along view direction
float3 displacedWorldPos = worldPos - viewDir * pomDepth * heightScale / dot(viewDir, normal);

// Write corrected linear depth to GBuffer RT2
float correctedLinearDepth = length(displacedWorldPos - cameraPos) / farClip;
output.depth = correctedLinearDepth;  // RT2
```

For hardware depth buffer correction, use `SV_Depth` output semantic:

```hlsl
// In PS output struct:
float depth : SV_Depth;

// Calculate clip-space depth of displaced position
float4 clipPos = mul(float4(displacedWorldPos, 1), viewProj);
output.depth = clipPos.z / clipPos.w;
```

**Performance note:** Writing `SV_Depth` disables early-Z/Hi-Z optimizations because the GPU can no longer assume the PS won't modify depth. This is a measurable performance cost.

**Mitigation:** Only write `SV_Depth` when the POM displacement is significant. For distant objects or very small displacements, skip depth correction (the error is negligible).

### Conservative Depth

DX11.1+ supports `SV_DepthGreaterEqual` / `SV_DepthLessEqual` semantics that preserve early-Z optimizations when the shader guarantees the depth output is always greater than (or less than) the interpolated depth. Since POM displaces the surface *inward* (the displaced surface is always closer to or at the original surface), use `SV_DepthGreaterEqual`:

```hlsl
// POM pulls the surface inward → depth is always ≥ original
float depth : SV_DepthGreaterEqual;
```

This preserves most of the early-Z benefit. D3D11 Feature Level 11_0 supports this.

---

## 11. Self-Shadowing

### Technique

After finding the POM intersection point, cast a second ray from the intersection toward the light source. If the ray intersects the height field before exiting, the point is in shadow.

```hlsl
float POMSelfShadow(float2 texCoords, float3 lightDirTS, float currentHeight,
                     Texture2D heightMap, SamplerState samp)
{
    if (lightDirTS.z <= 0.0)
        return 0.0;  // light below surface

    int numShadowSteps = 16;
    float2 deltaUV = lightDirTS.xy * g_heightScale / (lightDirTS.z * numShadowSteps);
    float stepHeight = (1.0 - currentHeight) / numShadowSteps;

    float2 currUV = texCoords;
    float currShadowLayer = currentHeight;
    float shadow = 1.0;

    [loop]
    for (int i = 0; i < numShadowSteps; i++)
    {
        currUV += deltaUV;
        currShadowLayer += stepHeight;
        float sampledHeight = heightMap.SampleLevel(samp, currUV, 0).r;

        if (sampledHeight > currShadowLayer)
        {
            shadow = 0.0;
            break;
        }
    }

    return shadow;
}
```

### Integration with Kenshi's Deferred Lighting

Self-shadowing is computed in the GBuffer PS, independent of the deferred lighting pass. It can be:
1. **Stored in GBuffer:** Use a free bit/channel to pass the self-shadow factor to the deferred pass. Problem: no free channels in Kenshi's packed GBuffer.
2. **Baked into albedo:** Multiply albedo by the self-shadow factor before writing to GBuffer. Simple but loses the ability to have specular highlights in self-shadowed areas.
3. **Separate RT:** Write self-shadow to a separate render target. Requires adding a 4th MRT. Kenshi's GBuffer pass uses 3 MRTs — D3D11 supports up to 8, so a 4th is feasible but requires framework support.
4. **Skip self-shadowing:** Rely on SSAO and screen-space shadows for contact darkening. These already handle cavity occlusion well.

**Recommendation:** Start without self-shadowing. SSAO (already implemented in Dust) provides contact shadows that approximate the visual effect. Self-shadowing can be added later via option 3 if the visual gap is noticeable.

---

## 12. Performance Analysis

### POM Cost

**Per-pixel cost:** Each POM ray march executes 8-32 texture samples per pixel (view-angle dependent). Self-shadowing adds another 16 samples.

**Fill rate impact:** Terrain covers ~40-60% of screen area. At 1440p (2560×1440 = 3.7M pixels), POM processes 1.5M-2.2M pixels:
- At 16 average samples/pixel: 24M-35M extra texture samples for terrain alone
- At 32 samples (grazing): 48M-70M extra texture samples

**Comparison:** Modern GPUs handle 10B+ texture samples/second. 35M extra samples is negligible (0.35% of a GPU running at 10GT/s). The real cost is ALU (the ray march loop) and cache pressure (random-access height map reads).

**Expected cost:** 0.3-0.8ms at 1440p for terrain POM on mid-range GPUs (RTX 3060 class). Higher for objects (more draws, smaller surfaces = less cache coherence).

### Tessellation Cost

**Per-draw cost:** Tessellation adds HS/DS execution + fixed-function subdivision. For ~120 terrain draws:
- With average tessellation factor ~4x: triangle count increases 16x per draw
- Original: ~8572 indices × 120 draws = ~1M indices → ~340K triangles
- Tessellated: ~340K × 16 = ~5.5M triangles (terrain only)

**Expected cost:** 0.5-1.5ms at 1440p. Dominated by VS/DS execution and rasterization of additional triangles.

### Combined Cost

POM + tessellation: 0.8-2.0ms total. Tessellation reduces POM sample count needed (geometry is closer to the displaced surface, so POM corrections are smaller = fewer steps).

### LOD Strategies

1. **Distance-based fade:** Reduce tessellation factors and POM sample counts with distance
2. **Screen-space coverage:** Skip POM for triangles covering < N pixels
3. **View angle:** Reduce samples when viewing surface head-on (less parallax needed)
4. **Material-based:** Skip POM for flat materials (sand, water, roads)

---

## 13. Recommended Implementation Plan

### Phase 1: POM for Objects (Lowest Risk, Quick Win)

1. **Modify D3DCompile hook** to detect `objects.hlsl` compilation
2. **Inject POM code** that reads height from `normalMap.a` (slot s1)
3. **Add runtime format check:** Only apply POM when normal map is BC3_UNORM (has alpha)
4. **Add GBuffer depth correction** using `SV_DepthGreaterEqual`
5. **Add distance-based LOD** fade for POM
6. **Add INI settings:** heightScale, minSamples, maxSamples, maxDistance, enabled

**Deliverable:** Objects (buildings, ruins, props) gain visible surface depth. No framework API changes needed.

### Phase 2: POM for Terrain

1. **Investigate terrain Texture2DArray** for height data availability
2. **Modify D3DCompile hook** to detect `terrainfp4.hlsl` compilation
3. **Inject terrain-specific POM** that handles Texture2DArray sampling
4. **Handle height-blend interaction** (POM height should match blend weights)
5. **Tune LOD aggressively** — terrain fills massive screen area, needs fast fade-out

**Deliverable:** Terrain gains depth detail (sand ripples, rock crevices, soil texture).

### Phase 3: POM for Triplanar

1. **Adapt POM** for world-space UV projection
2. **Per-axis POM** with world-aligned tangent frames matching triplanar projection
3. **Blend POM results** across axes like existing normal blending

**Deliverable:** Rock and cliff surfaces gain depth detail.

### Phase 4: Tessellation (Future)

1. **Extend HookedDrawIndexed** with pre-draw callback for pipeline state modification
2. **Implement TRIANGLESTRIP → TRIANGLELIST conversion** for terrain index buffers
3. **Compile adaptive HS/DS** matching existing shadow tessellation pattern
4. **Add height map binding** to DS texture slots
5. **Distance-based tessellation factor** with screen-space edge length adaptive LOD
6. **Combine with POM** — tessellation for macro displacement, POM for micro detail

**Deliverable:** True geometric displacement with correct silhouettes, depth, and shadows. The ultimate quality target.

---

## 14. References

### Techniques

- [LearnOpenGL - Parallax Mapping](https://learnopengl.com/Advanced-Lighting/Parallax-Mapping) — comprehensive POM tutorial with GLSL code
- [Practical Parallax Occlusion Mapping (Tatarchuk, ATI)](https://web.engr.oregonstate.edu/~mjb/cs519/Projects/Papers/Parallax_Occlusion_Mapping.pdf) — the original AMD/ATI paper on POM
- [A Closer Look at Parallax Occlusion Mapping (GameDev.net)](https://gamedev.net/articles/programming/graphics/a-closer-look-at-parallax-occlusion-mapping-r3262/) — detailed implementation walkthrough
- [DX SDK DetailTessellation11 POM.hlsl](https://github.com/tgjones/slimshader-cpp/blob/master/src/Shaders/Sdk/Direct3D11/DetailTessellation11/POM.hlsl) — Microsoft's reference combined POM + tessellation

### DX11 Tessellation

- [Microsoft: Tessellation Stages](https://learn.microsoft.com/en-us/windows/win32/direct3d11/direct3d-11-advanced-stages-tessellation) — official pipeline documentation
- [Microsoft: How to Design a Hull Shader](https://learn.microsoft.com/en-us/windows/win32/direct3d11/direct3d-11-advanced-stages-hull-shader-design) — HS design patterns
- [D3D11 Tessellation In Depth (GameDev.net)](https://gamedev.net/tutorials/programming/graphics/d3d11-tessellation-in-depth-r3059) — practical tutorial
- [Tessellation on D3D11 (A Graphics Guy's Notes)](https://agraphicsguynotes.com/posts/tessellation_on_d3d11/) — implementation walkthrough
- [Rendering Terrain Part 8 – Adding Tessellation](https://thedemonthrone.ca/projects/rendering-terrain/rendering-terrain-part-8-adding-tessellation/) — terrain-specific tessellation

### Ogre3D Tessellation

- [Ogre3D: GPU Program Scripts](https://ogrecave.github.io/ogre/api/latest/_high-level-_programs.html) — tessellation_hull_program / tessellation_domain_program syntax
- [GSoC 2013: DirectX 11 Tessellation Samples](https://forums.ogre3d.org/viewtopic.php?t=77133) — Ogre's tessellation implementation history
- [Ogre Wiki: SoC2013 DirectX11 & Tesselation Sample](https://wiki.ogre3d.org/SoC2013+DirectX11+&+Tesselation+Sample)

### Deferred Rendering + POM

- [Parallax Occlusion with Deferred Rendering (GameDev.net)](https://www.gamedev.net/forums/topic/572369-parallax-occlusion-with-deferred-rendering/) — depth correction discussion
- [Parallax mapping with deferred shading (Ogre Forums)](https://forums.ogre3d.org/viewtopic.php?t=47474) — Ogre-specific considerations
- [SPOM with Horizon Detection + Self-Shadowing (Godot)](https://godotshaders.com/shader/spom-with-horizon-detection-self-shading-silhouette-clipping-parallax-occlusion-mapping-self-shading-horizon-trimming-erosion/) — advanced POM with silhouette correction

### Terrain POM

- [Improved Terrain Splatting and Parallax Mapping (jMonkeyEngine)](https://hub.jmonkeyengine.org/t/improved-terrain-splatting-and-parallax-mapping/27844) — multi-layer terrain POM
- [SpaceEngine: Terrain Engine Upgrade](https://spaceengine.org/news/blog180323/) — tessellation + POM for planetary terrain
- [Skyrim SE Parallax Occlusion Mapping (Nexus)](https://www.nexusmods.com/skyrimspecialedition/mods/78976) — game mod POM (comparable use case)
