# Hybrid Ray Traced Shadows — Research for Dust/Kenshi

## 1. The Paper: "Hybrid Ray-Traced Shadows" (GDC 2015, Jon Story, NVIDIA)

### Core Idea

Replace shadow map artifacts (acne, peter-panning, aliasing) with ray-traced hard shadows near contact surfaces, blended with conventional soft shadows (PCSS) at distance. The key insight: **you don't need to ray trace the entire shadow** — use cheap PCSS for the soft penumbra far from occluders, and expensive but pixel-perfect ray tracing only where it matters (at contact).

### Deep Primitive Map (DPM)

Instead of building a traditional BVH (which requires rebuilding for dynamic objects and has slow tree traversal), the paper stores scene triangles in a 2D map aligned to the shadow map — a "Deep Primitive Map."

**Three GPU resources:**

| Resource | Dimensions | Content |
|----------|-----------|---------|
| Prim Count Map | NxN | Atomic counter per texel: how many triangles overlap this texel |
| Prim Indices Map | NxNxd | Index into the prim buffer for each triangle at this texel |
| Prim Buffer | flat array | World-space triangle vertices (3x float3 per triangle) |

Where N = shadow map resolution (typically 1K), d = max triangles per texel (32-64).

**How it's built:**

1. Render scene from the light's perspective (same as shadow mapping)
2. Geometry Shader outputs 3 world-space vertex positions + `SV_PrimitiveID` per triangle
3. Pixel Shader hashes draw call ID + `SV_PrimitiveID` to get a unique prim index
4. PS writes world-space vertices to the Prim Buffer at that index
5. PS uses `InterlockedAdd` on the Prim Count Map to count triangles per texel
6. PS writes the prim index into the Prim Indices Map at `[texelXY, currentCount]`

**Key code from the paper:**

```hlsl
// Geometry Shader — pass through world-space triangle vertices
[maxvertexcount(3)]
void Primitive_Map_GS(triangle GS_Input IN[3], uint uPrimID : SV_PrimitiveID,
                      inout TriangleStream<PS_Input> Triangles)
{
    PS_Input O;
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        O.f3PositionWS0 = IN[0].f3PositionWS;  // vertex 0
        O.f3PositionWS1 = IN[1].f3PositionWS;  // vertex 1
        O.f3PositionWS2 = IN[2].f3PositionWS;  // vertex 2
        O.f4PositionCS  = IN[i].f4PositionCS;   // SV_Position
        O.uPrimID       = uPrimID;
        Triangles.Append(O);
    }
    Triangles.RestartStrip();
}

// Pixel Shader — store triangle in the deep primitive map
float Primitive_Map_PS(PS_Input IN) : SV_TARGET
{
    uint PrimIndex = g_DrawCallOffset + IN.uPrimID;

    g_PrimBuffer[PrimIndex].f3PositionWS0 = IN.f3PositionWS0;
    g_PrimBuffer[PrimIndex].f3PositionWS1 = IN.f3PositionWS1;
    g_PrimBuffer[PrimIndex].f3PositionWS2 = IN.f3PositionWS2;

    uint CurrentIndexCounter;
    InterlockedAdd(g_IndexCounterMap[uint2(IN.f4PositionCS.xy)], 1, CurrentIndexCounter);
    g_IndexMap[uint3(IN.f4PositionCS.xy, CurrentIndexCounter)] = PrimIndex;

    return 0;
}
```

**Conservative rasterization** is critical — without it, triangles smaller than a texel or barely touching a texel are missed. Options:
- **Hardware CR** (D3D11.3 / `D3D11_RASTERIZER_DESC2`): Tier 1 hardware rasterizes every pixel touched by a triangle. Requires FL 11_1+ and hardware support.
- **Software CR** (GS dilation): Geometry Shader dilates the triangle in clip space by expanding each edge outward by half a texel. Generate AABB in PS to clip the dilated region. See GPU Gems 2, Chapter 42.

### Ray Tracing Pass

For each screen pixel, reconstruct world position, compute shadow map UV (same as standard shadow mapping), then iterate over stored triangles at that texel:

```hlsl
float Ray_Test(float2 MapCoord, float3 f3Origin, float3 f3Dir, out float BlockerDistance)
{
    uint uCounter = tIndexCounterMap.Load(int3(MapCoord, 0)).x;

    [branch]
    if (uCounter > 0)
    {
        for (uint i = 0; i < uCounter; i++)
        {
            uint uPrimIndex = tIndexMap.Load(int4(MapCoord, i, 0)).x;

            float3 v0, v1, v2;
            Load_Prim(uPrimIndex, v0, v1, v2);

            // Möller-Trumbore ray-triangle intersection
            [branch]
            if (Ray_Hit_Triangle(f3Origin, f3Dir, v0, v1, v2, BlockerDistance) != 0.0f)
                return 1.0f;  // shadowed
        }
    }
    return 0.0f;  // lit
}
```

The ray origin is the surface world position. The ray direction points toward the light. `BlockerDistance` records the distance from surface to blocker — this is the key value for the hybrid blend.

### The Hybrid Blend

```
L = saturate(BD / WSS * PHS)

L:   Lerp factor (0 = pure ray-traced, 1 = pure PCSS)
BD:  Blocker distance (from ray origin to hit triangle)
WSS: World space scale (chosen per model / cascade)
PHS: Percentage of hard shadow desired

FS = lerp(RTS, PCSS, L)

FS:  Final shadow
RTS: Ray-traced shadow (binary: 0 or 1)
PCSS: Percentage-Closer Soft Shadow (0.0 to 1.0)
```

When `BD = 0` (surface touches the occluder), the result is pure ray-traced hard shadow — pixel-perfect edges, no filtering artifacts.

When `BD` is large (far from occluder), the result is pure PCSS — natural penumbra softening proportional to distance.

### Shrinking Penumbra Filter

**Critical requirement**: The PCSS filter must use a **shrinking** penumbra, not the standard expanding penumbra.

Standard PCSS: penumbra region EXPANDS outward from the shadow boundary as blocker distance increases. The soft shadow "leaks" beyond the geometric shadow boundary.

Shrinking PCSS: penumbra region CONTRACTS inward — the fully-lit outer edge stays fixed, and the softness grows inward toward the fully-shadowed region.

Why this matters: when lerping between ray-traced (hard) and PCSS (soft), the soft shadow must be fully **contained within** the hard shadow. Otherwise the lerp produces visible artifacts at the transition boundary — soft shadow areas where the ray-traced result says "lit" create bright halos.

```
Standard Filter:     [Fully Lit] --- penumbra --- [Fully Shadowed]
                     ^extends outward

Shrinking Filter:    [Fully Lit] [Fully Shadowed] --- penumbra inward ---
                                 ^fixed edge, softness grows inward
```

### Performance (2015 Hardware)

| Scene | Prims | PM Size | PM Build | Ray Trace | PCSS | Total | GPU |
|-------|-------|---------|----------|-----------|------|-------|-----|
| ~10K  | 1Kx1Kx32 (128MB) | 0.5ms | 0.4ms | 1.3ms | 1.8ms | GTX 980 |
| ~65K  | 1Kx1Kx64 (256MB) | 0.7ms | 0.7ms | 1.3ms | 2.8ms | GTX 980 |
| ~240K | 1Kx1Kx64 (256MB) | 4.1ms | 1.0ms | 1.3ms | 3.4ms* | GTX 980 |

*240K prims used SW conservative rasterization (HW CR was available only on GTX 980 in 2015)

### Limitations (from the paper)

1. **Single light source** — each light needs its own DPM
2. **Doesn't scale to whole scene** — storage becomes the limiter
3. **Ideal for closest cascade** — near-field geometry where contact matters
4. **Memory intensive** — 128-256 MB per DPM

### Anti-Aliasing the Ray-Traced Result

The paper recommends applying a screen-space AA technique (FXAA, MLAA, SMAA) to the shadow buffer rather than shooting additional rays. This is much cheaper and sufficient for shadow edges.

---

## 2. Alternative Approaches Researched

### 2A. CPU-Built BVH + Compute Shader (Interplay of Light, 2018)

Kostas Anagnostou's approach from his "Hybrid Raytraced Shadows" blog series:

**Architecture:**
- Build BVH on CPU from scene triangles (world-space transformed)
- Upload BVH to GPU as a flat buffer (ByteAddressBuffer or float4 typed buffer)
- Compute shader traces one shadow ray per screen pixel through the BVH
- Stackless depth-first traversal using offset-to-sibling encoding

**Key optimizations and their impact (Sponza, ~558K triangles, 1920x1080):**

| Optimization | Before | After | GPU |
|-------------|--------|-------|-----|
| Triangles in leaves (not meshes) | 23ms (quarter) | 10ms | HD 4000 |
| ByteAddressBuffer storage | 16ms (quarter) | 13ms | GTX 970 |
| Surface Area Heuristic BVH | 13ms (quarter) | 3.6ms | GTX 970 |
| Node reordering (largest first) | 3.6ms | 3.3ms | GTX 970 |
| float4 typed buffer (NVIDIA) | 10.5ms (full) | 5.5ms | GTX 970 |
| NdotL backface culling | 5.5ms (full) | 2.5ms | GTX 970 |
| **Combined** | **16ms (quarter)** | **2.5ms (full)** | **GTX 970** |

**Key insight**: NdotL culling is free and halves cost — skip rays for surfaces facing away from the light. Combined with SAH BVH and optimal buffer format, full-res shadows at 2.5ms on 2015 hardware.

**Applicability to Dust:**
- Would require extracting world-space vertex positions from captured draws
- CPU BVH construction adds latency (must be done every frame for dynamic scenes)
- Kenshi's geometry capture only has VB/IB/VS state — would need to read back transformed vertices
- More flexible than DPM (works for any light type, not tied to shadow map UV)
- Higher quality than DPM (true BVH traversal, not limited to texel-aligned lookups)

### 2B. Shadow Map Heightfield Ray Marching

**Technique:** Treat the existing shadow map as a heightfield. For each penumbra pixel, march a ray from the surface toward the light in world space, projecting each step into shadow map space and comparing against the stored depth.

**Advantages:**
- No geometry access needed — works with current Dust infrastructure immediately
- No memory overhead (uses existing shadow map)
- Covers off-screen occluders (shadow map sees them)
- Quick-reject: only trace penumbra pixels (~5-15% of frame)
- Performance estimate: ~0.5-1.5ms for penumbra-only tracing

**Disadvantages:**
- Quality limited by shadow map resolution (tracing through a 2048x2048 heightfield)
- RTWSM warp adds per-step accuracy issues
- Doesn't fix temporal flickering (still depends on game's unstable warp map)
- Misses thin objects (heightfield has one depth per texel)
- Band-aid over a fundamentally broken shadow map

### 2C. AMD FidelityFX Hybrid Shadows

**Technique:** Tile-based shadow classification + DXR ray tracing for edge tiles only.

**Architecture:**
1. Render standard shadow map cascades
2. Classify 8x8 tiles into: fully lit, fully shadowed, penumbra
3. For penumbra tiles only, fire DXR shadow rays
4. Denoise the sparse ray-traced result (FidelityFX Shadow Denoiser)
5. Composite with shadow map result

**Key features:**
- Lit pixel rejection: tiles where all samples pass shadow test → skip
- Shadowed pixel rejection: tiles where all samples fail → skip
- Ray interval reduction: limit ray length based on shadow map depth
- Only 5-15% of tiles typically need rays

**Not applicable to Dust**: Requires DXR 1.1 (D3D12 only). Kenshi runs D3D11 FL 11_0. However, the **tile classification concept** is directly applicable — classify shadow regions to avoid ray tracing fully-lit or fully-shadowed areas.

### 2D. Screen-Space Contact Shadows (Already in Dust as SSS)

The existing SSS effect ray-marches screen-space depth toward the sun. Good for small-scale contact shadows but fundamentally limited by screen-space visibility.

---

## 3. Kenshi-Specific Context

### Current Shadow System

| Property | Value |
|----------|-------|
| Type | Cascaded Shadow Maps (CSM) with RTW warp |
| Atlas | 4096x4096 R32_FLOAT (single atlas for 4 cascades) |
| Effective/cascade | ~2048x2048 |
| Filtering | 12-sample hex PCF with jittered rotation |
| Bias | Fixed + slope-scaled |
| Shadow draws | 196-1051 per frame (avg 579) |
| 52% tessellated | PATCHLIST_3_CP via rtwtessellator.hlsl |

### Existing Infrastructure in Dust

| System | Status | Relevance |
|--------|--------|-----------|
| GeometryCapture | Working | Captures all GBuffer draws with IA/VS/PS state |
| GeometryReplay | Working | Replays draws with replacement VP matrix |
| ShaderMetadata | Working | Classifies VS transforms (STATIC/SKINNED), finds clip matrix offset |
| CB staging copies | Working | Async copy of VS CB at capture time, stall-free read at replay |
| Compute shader dispatch | Working | RTGI uses 8x8 threadgroups, UAV read/write |
| D3DCompile hook | Working | Runtime shader patching of deferred.hlsl |
| Shadow CB injection | Working | DustShadows binds CB at b2 for PCSS params |
| Resource creation | Working | CreateConstantBuffer, managed RT/SRV creation |
| State save/restore | Working | Full D3D11StateBlock or lightweight ReplayStateBlock |
| Sun direction | Available | From deferred PS CB0 register c0 (xyz) |
| Inverse view matrix | Available | From deferred PS CB0 registers c8-c11 |
| Shadow matrices | Available | csmScale[], csmTrans[], shadowViewMat from deferred CB |

### Key Constraint: No Temporal Techniques

All denoising must be spatial-only. This is actually an advantage for the hybrid ray-traced approach because:
- The ray-traced result is binary (hit/miss) — no noise to denoise
- PCSS uses spatial PCF filtering — already spatial-only
- The hybrid blend is per-pixel per-frame — no history dependence
- Anti-aliasing the shadow buffer with SMAA is spatial-only

The hybrid approach is inherently clean per-frame. No temporal accumulation needed.

---

## 4. Implementation Analysis for Dust

### Option A: Deep Primitive Map (Paper's Approach)

**How it maps to Dust's infrastructure:**

1. **DPM Construction**: During GeometryReplay into the shadow map, also populate the DPM
   - Replace the null PS (depth-only) with a GS+PS that writes world-space vertices to UAVs
   - Need a custom VS that outputs world-space positions to the GS
   - OR: use DX11.1's ability to write directly from VS to UAV (avoids GS entirely)

2. **Conservative Rasterization**:
   - **D3D11.3 HW CR**: Requires `ID3D11Device3::CreateRasterizerState2` with `D3D11_RASTERIZER_DESC2`. Available on GPUs that support CR (most post-2015 GPUs). Kenshi's FL 11_0 may limit this — need to check if CR works at FL 11_0 with optional feature support.
   - **Software CR via GS**: Dilate triangle edges in clip space by half a texel width. Clip expanded region in PS via AABB test. ~0.1ms overhead per cascade.

3. **Ray Tracing Pass**: Compute shader at POST_LIGHTING
   - Reconstruct world position from depth buffer
   - Compute shadow map UV from world position (using captured shadow matrices)
   - Look up DPM at that texel
   - Iterate over stored triangles, do Möller-Trumbore intersection
   - Output: shadow mask (R8_UNORM) + blocker distance (R16_FLOAT)

4. **PCSS Pass**: Same compute shader or separate fullscreen PS
   - Standard PCSS with **shrinking** penumbra filter
   - Uses existing shadow map depth data
   - Output: soft shadow value (R8_UNORM)

5. **Hybrid Composite**: Fullscreen PS at POST_LIGHTING
   - `lerp(rayTracedShadow, pcssShadow, saturate(blockerDist / worldScale * hardShadowPct))`
   - Apply SMAA to the shadow buffer for clean edges
   - Multiply into HDR lighting

**Memory Budget (at 1024x1024 DPM, d=32):**

| Resource | Size |
|----------|------|
| Prim Count Map (R32_UINT, 1024x1024) | 4 MB |
| Prim Indices Map (R32_UINT, 1024x1024x32) | 128 MB |
| Prim Buffer (3x float3 per prim, ~50K prims) | ~1.7 MB |
| Shadow mask (R8_UNORM, native res) | ~3.5 MB |
| Blocker distance (R16_FLOAT, native res) | ~7 MB |
| **Total** | **~144 MB** |

This is significant but manageable on modern GPUs (4+ GB VRAM). Could reduce d from 32 to 16 for 72 MB total.

**Estimated Performance:**

| Pass | Estimate | Notes |
|------|----------|-------|
| DPM construction | ~0.5-1ms | During shadow map rendering, adds GS/PS overhead |
| Ray trace (compute) | ~0.5-1ms | Only penumbra pixels, NdotL cull, ~5-15% of screen |
| PCSS | ~1-1.5ms | Existing technique, optimized with tile classification |
| Composite + SMAA | ~0.3ms | Fullscreen blend + edge AA on shadow buffer |
| **Total** | **~2.5-4ms** | On modern hardware (GTX 1060+) |

### Option B: Custom CSM + Enhanced PCSS (No Ray Tracing)

**The simplest path that uses existing infrastructure:**

1. Use GeometryReplay to render proper CSM (3-4 cascades, texel-snapped)
2. Apply PCSS with blocker search on the custom CSM
3. Add cascade blending at split boundaries
4. Combine with existing screen-space contact shadows (SSS effect)

**Advantages:**
- Uses existing GeometryReplay without modification
- No new data structures (no DPM, no BVH)
- Lower memory cost
- Simpler implementation (~2-3 weeks vs ~6-8 weeks for DPM)

**Disadvantages:**
- Still limited by shadow map resolution for contact shadows
- No pixel-perfect hard shadows at contact
- Shadow quality depends on cascade resolution

**Estimated Performance:**

| Pass | Estimate | Notes |
|------|----------|-------|
| CSM rendering (3 cascades) | ~2-4ms | Geometry replay from light perspective |
| PCSS filtering | ~1-1.5ms | Blocker search + variable PCF |
| Cascade blending | ~0.1ms | Cross-fade at boundaries |
| **Total** | **~3-5.5ms** | Geometry cost dominates |

### Option C: Custom CSM + DPM Nearest Cascade (Recommended)

**The hybrid approach — best of both worlds:**

1. Use GeometryReplay to render proper 3-cascade CSM
2. For cascade 0 (nearest) only, build a DPM alongside the shadow rendering
3. Ray trace against the DPM for near-field contact shadow quality
4. Use PCSS on all cascades for soft shadows
5. Hybrid blend: near cascade gets ray-traced contact + PCSS softness; far cascades get PCSS only

**Why this is optimal for Kenshi:**
- Contact shadows are only visually important near the camera (cascade 0)
- Far cascades don't need pixel-perfect edges — PCSS softening hides all aliasing
- DPM memory is smaller (only 1 cascade's worth of geometry)
- DPM build cost is lower (only nearest-cascade draws)
- GeometryReplay already supports per-cascade frustum culling
- The hybrid blend produces clean per-frame output (no temporal dependency)

**Memory: ~72-144 MB** (cascade 0 only, d=16-32)
**Performance: ~3-5ms total** (CSM render + DPM build + ray trace + PCSS)

---

## 5. Deep Primitive Map — Detailed Implementation Plan

### 5.1 DPM Resources

```cpp
// Prim Count Map — one uint per DPM texel
ID3D11Texture2D*          dpmCountTex;     // R32_UINT, dpmSize x dpmSize
ID3D11UnorderedAccessView* dpmCountUAV;
ID3D11ShaderResourceView*  dpmCountSRV;

// Prim Indices Map — d uints per texel (3D texture or structured buffer)
// Option A: Texture3D (NxNxd) — simple addressing but wastes memory
// Option B: StructuredBuffer (NxN*d) — more flexible
ID3D11Buffer*             dpmIndicesBuf;   // StructuredBuffer<uint>, dpmSize*dpmSize*maxDepth
ID3D11UnorderedAccessView* dpmIndicesUAV;
ID3D11ShaderResourceView*  dpmIndicesSRV;

// Prim Buffer — world-space triangle vertices
struct DPMPrimitive {
    float3 v0, v1, v2;  // 36 bytes per triangle
};
ID3D11Buffer*             primBuffer;      // StructuredBuffer<DPMPrimitive>
ID3D11UnorderedAccessView* primBufferUAV;
ID3D11ShaderResourceView*  primBufferSRV;
```

### 5.2 DPM Construction Shaders

**Vertex Shader** — outputs world-space position and clip-space position:

```hlsl
struct VS_Output {
    float4 posCS  : SV_Position;
    float3 posWS  : TEXCOORD0;
};

// Use the captured draw's original VS, but extract world position
// from the VS constant buffer using ShaderMetadata offsets.
// For STATIC transforms: posWS = mul(worldMatrix, posOS)
// For SKINNED transforms: posWS = mul(skinMatrix, posOS) (bones applied)
```

For Dust, the simplest approach: run the original VS (which outputs clip-space position for the light's VP), and compute world-space position in the GS by inverting the light VP transform. OR: use the cbStagingCopy to extract the world matrix and compute world-space positions directly.

**Geometry Shader** — pass through world-space triangle:

```hlsl
struct GS_Output {
    float4 posCS  : SV_Position;
    float3 v0WS   : TEXCOORD0;
    float3 v1WS   : TEXCOORD1;
    float3 v2WS   : TEXCOORD2;
    uint   primID : TEXCOORD3;
};

cbuffer DrawInfo : register(b1) {
    uint drawCallOffset;  // Unique per draw call, ensures global prim ID
};

[maxvertexcount(3)]
void DPM_GS(triangle VS_Output IN[3], uint primID : SV_PrimitiveID,
            inout TriangleStream<GS_Output> stream)
{
    GS_Output o;
    [unroll]
    for (int i = 0; i < 3; i++) {
        o.posCS = IN[i].posCS;
        o.v0WS  = IN[0].posWS;
        o.v1WS  = IN[1].posWS;
        o.v2WS  = IN[2].posWS;
        o.primID = drawCallOffset + primID;
        stream.Append(o);
    }
    stream.RestartStrip();
}
```

**Pixel Shader** — store triangle in DPM:

```hlsl
RWTexture2D<uint>            g_PrimCountMap  : register(u0);
RWStructuredBuffer<uint>     g_PrimIndicesMap : register(u1);
RWStructuredBuffer<DPMPrim>  g_PrimBuffer    : register(u2);

cbuffer DPMParams : register(b2) {
    uint dpmSize;      // DPM resolution (e.g., 1024)
    uint maxDepth;     // Max triangles per texel (e.g., 32)
};

void DPM_PS(GS_Output IN)
{
    uint2 texel = uint2(IN.posCS.xy);
    uint primIdx = IN.primID;

    // Store world-space triangle in prim buffer
    g_PrimBuffer[primIdx].v0 = IN.v0WS;
    g_PrimBuffer[primIdx].v1 = IN.v1WS;
    g_PrimBuffer[primIdx].v2 = IN.v2WS;

    // Atomically increment per-texel counter
    uint slot;
    InterlockedAdd(g_PrimCountMap[texel], 1, slot);

    // Store prim index in the indices map (if within depth budget)
    if (slot < maxDepth) {
        uint flatIndex = (texel.y * dpmSize + texel.x) * maxDepth + slot;
        g_PrimIndicesMap[flatIndex] = primIdx;
    }
}
```

### 5.3 Ray Tracing Compute Shader

```hlsl
Texture2D<float>  depthTex    : register(t0);
Texture2D<float4> normalsTex  : register(t1);
Texture2D<uint>   primCountMap: register(t2);
StructuredBuffer<uint>     primIndicesMap : register(t3);
StructuredBuffer<DPMPrim>  primBuffer    : register(t4);
Texture2D<float>  shadowMap   : register(t5);  // For PCSS pass

RWTexture2D<float>  shadowMask    : register(u0);  // 0=shadowed, 1=lit
RWTexture2D<float>  blockerDist   : register(u1);  // Distance to blocker

cbuffer RayTraceCB : register(b0) {
    float4x4 invViewProj;
    float3   sunDirection;
    float    farClip;
    float4x4 shadowViewProj;  // Light's VP for cascade 0
    uint     dpmSize;
    uint     maxDepth;
    float    rayMaxDist;      // Max trace distance in world space
};

// Möller-Trumbore ray-triangle intersection
bool RayTriangleIntersect(float3 orig, float3 dir,
                          float3 v0, float3 v1, float3 v2,
                          out float t)
{
    float3 edge1 = v1 - v0;
    float3 edge2 = v2 - v0;
    float3 pvec  = cross(dir, edge2);
    float  det   = dot(edge1, pvec);

    if (abs(det) < 1e-8) return false;

    float invDet = 1.0 / det;
    float3 tvec  = orig - v0;
    float  u     = dot(tvec, pvec) * invDet;
    if (u < 0.0 || u > 1.0) return false;

    float3 qvec = cross(tvec, edge1);
    float  v    = dot(dir, qvec) * invDet;
    if (v < 0.0 || u + v > 1.0) return false;

    t = dot(edge2, qvec) * invDet;
    return t > 0.001;  // Small epsilon to avoid self-intersection
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    float depth = depthTex[DTid.xy];
    if (depth >= 1.0) {  // Sky
        shadowMask[DTid.xy] = 1.0;
        blockerDist[DTid.xy] = 0.0;
        return;
    }

    // NdotL backface cull — free, halves cost
    float3 normal = normalsTex[DTid.xy].rgb * 2.0 - 1.0;
    float NdotL = dot(normalize(normal), sunDirection);
    if (NdotL <= 0.0) {
        shadowMask[DTid.xy] = 0.0;  // Self-shadowed
        blockerDist[DTid.xy] = 0.0;
        return;
    }

    // Reconstruct world position from depth
    float2 uv = (float2(DTid.xy) + 0.5) / float2(screenWidth, screenHeight);
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;
    float4 worldPos4 = mul(invViewProj, clipPos);
    float3 worldPos = worldPos4.xyz / worldPos4.w;

    // Project to shadow map / DPM coordinates
    float4 shadowPos = mul(shadowViewProj, float4(worldPos, 1.0));
    shadowPos.xyz /= shadowPos.w;
    float2 shadowUV = shadowPos.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;

    // Check if within cascade 0 bounds
    if (any(shadowUV < 0.0) || any(shadowUV > 1.0)) {
        // Outside DPM coverage — fall back to shadow map
        shadowMask[DTid.xy] = 1.0;  // Will be overwritten by PCSS
        blockerDist[DTid.xy] = 999.0;
        return;
    }

    // Look up DPM at this texel
    uint2 dpmTexel = uint2(shadowUV * float(dpmSize));
    uint primCount = primCountMap[dpmTexel];

    float3 rayDir = sunDirection;
    float closestBlocker = rayMaxDist;
    bool hit = false;

    for (uint i = 0; i < min(primCount, maxDepth); i++) {
        uint flatIdx = (dpmTexel.y * dpmSize + dpmTexel.x) * maxDepth + i;
        uint primIdx = primIndicesMap[flatIdx];

        float3 v0 = primBuffer[primIdx].v0;
        float3 v1 = primBuffer[primIdx].v1;
        float3 v2 = primBuffer[primIdx].v2;

        float t;
        if (RayTriangleIntersect(worldPos, rayDir, v0, v1, v2, t)) {
            hit = true;
            closestBlocker = min(closestBlocker, t);
        }
    }

    shadowMask[DTid.xy] = hit ? 0.0 : 1.0;
    blockerDist[DTid.xy] = hit ? closestBlocker : 0.0;
}
```

### 5.4 PCSS with Shrinking Penumbra

```hlsl
// Standard PCSS blocker search
float SearchBlockers(float2 shadowUV, float receiverDepth, float searchRadius)
{
    float blockerSum = 0.0;
    float blockerCount = 0.0;

    for (int i = 0; i < BLOCKER_SEARCH_SAMPLES; i++) {
        float2 offset = poissonDisk[i] * searchRadius;
        float sampleDepth = shadowMap.SampleLevel(pointSamp, shadowUV + offset, 0);

        if (sampleDepth < receiverDepth) {
            blockerSum += sampleDepth;
            blockerCount += 1.0;
        }
    }

    return (blockerCount > 0) ? blockerSum / blockerCount : -1.0;
}

// Shrinking penumbra PCF — penumbra grows INWARD, not outward
float ShrinkingPCF(float2 shadowUV, float receiverDepth, float penumbraWidth)
{
    float shadow = 0.0;

    for (int i = 0; i < PCF_SAMPLES; i++) {
        float2 offset = poissonDisk[i] * penumbraWidth;
        float sampleDepth = shadowMap.SampleLevel(pointSamp, shadowUV + offset, 0);

        // Key difference: compare with a tightened threshold
        // Standard: sample < receiver → shadowed
        // Shrinking: the comparison radius shrinks with penumbra
        shadow += (sampleDepth < receiverDepth) ? 1.0 : 0.0;
    }

    return shadow / float(PCF_SAMPLES);
}
```

### 5.5 Hybrid Composite

```hlsl
Texture2D<float> rtShadow     : register(t0);  // Ray-traced binary mask
Texture2D<float> pcssShadow   : register(t1);  // PCSS soft shadow
Texture2D<float> blockerDist  : register(t2);  // Distance to blocker

cbuffer HybridCB : register(b0) {
    float worldSpaceScale;     // Tune per scene: controls blend distance
    float hardShadowPercent;   // 0.0-1.0: how much hard shadow to preserve
};

float4 HybridComposite_PS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float rt   = rtShadow.Sample(pointSamp, uv);
    float pcss = pcssShadow.Sample(pointSamp, uv);
    float bd   = blockerDist.Sample(pointSamp, uv);

    // L=0 at contact (pure RT), L=1 at distance (pure PCSS)
    float L = saturate(bd / worldSpaceScale * hardShadowPercent);

    float finalShadow = lerp(rt, pcss, L);
    return float4(finalShadow, finalShadow, finalShadow, 1.0);
}
```

### 5.6 Tile Classification (from AMD approach — optimization)

Before dispatching the ray trace compute shader, classify 8x8 tiles to skip fully-lit and fully-shadowed regions:

```hlsl
// Pre-pass: read shadow map depth at tile corners/center
// If all samples pass (lit): skip tile, write 1.0 to shadow mask
// If all samples fail (shadowed): skip tile, write 0.0
// Otherwise: tile needs ray tracing

// This reduces ray tracing from 100% to ~5-15% of screen pixels
```

---

## 6. Integration with Dust Architecture

### Where Each Pass Fits

```
Game pipeline:
[1407-2583] GBuffer fill → GeometryCapture records all draws
[2590-3640] Game shadow map rendering → we read the shadow map SRV
[3641-3648] Deferred lighting → game uses its shadow map

Dust injection:
POST_GBUFFER or POST_LIGHTING:
  1. GeometryReplay → render custom CSM (3 cascades)
  2. During cascade 0 replay → also populate DPM
  3. Tile classification (compute shader)
  4. Ray trace penumbra tiles against DPM (compute shader)
  5. PCSS with shrinking penumbra (compute shader)
  6. Hybrid composite (fullscreen PS)
  7. Patch deferred shader to read our shadow result instead of game's
```

### Required Framework Changes

1. **New effect plugin**: `DustHybridShadows` (replaces current `DustShadows`)
2. **Extended GeometryReplay**: option to run a GS+PS alongside the depth-only shadow pass for DPM population
3. **Shadow map management**: create 3 cascade depth textures (R32_FLOAT or D32_FLOAT)
4. **Deferred shader patch**: replace shadow sampling with our shadow mask texture lookup
5. **New shaders**: DPM construction GS+PS, ray trace CS, PCSS CS, hybrid composite PS, tile classification CS

### DPM Build During GeometryReplay

The key change to GeometryReplay is adding a GS and PS for DPM population during cascade 0 rendering:

```cpp
// In GeometryReplay::Replay, for cascade 0:
// Bind DPM UAVs alongside the depth RTV
// Set custom GS that outputs world-space triangle vertices
// Set custom PS that writes to DPM buffers via UAV
// (D3D11 supports simultaneous RTV + UAV output)
```

D3D11 supports binding UAVs alongside render targets via `OMSetRenderTargetsAndUnorderedAccessViews`. The PS can write depth (to RTV) and DPM data (to UAV) simultaneously. This avoids a separate pass for DPM construction.

### Skinned Mesh Handling

For skinned meshes (VSTransformType::SKINNED), the world-space positions are computed by the original VS using bone matrices from the constant buffer. The GS receives already-transformed clip-space positions. To get world-space:

Option A: Inverse the light's VP in the GS to get world-space from clip-space
Option B: Have the VS output both clip and world positions (requires custom VS)

Option A is simpler — single `mul(invLightVP, clipPos)` in the GS. Less precise for extreme projections but sufficient for shadow contact.

---

## 7. Performance Budget

Target: under 4ms total for the shadow system on a GTX 1060+ class GPU.

| Component | Budget | Notes |
|-----------|--------|-------|
| CSM rendering (3 cascades) | 2.0ms | GeometryReplay, ~500 draws x 3 cascades (frustum culled) |
| DPM construction (cascade 0) | 0.3ms | GS+PS overhead during cascade 0 rendering |
| Tile classification | 0.1ms | Simple compute pass |
| Ray trace (penumbra tiles) | 0.5ms | ~10% of pixels, NdotL culled |
| PCSS filtering | 0.5ms | Blocker search + shrinking PCF |
| Hybrid composite | 0.1ms | Fullscreen lerp |
| SMAA on shadow buffer | 0.2ms | Edge AA (optional) |
| **Total** | **~3.7ms** | Within budget |

The CSM rendering dominates cost. The ray tracing is cheap because:
1. DPM lookup is O(d) per pixel where d is typically 1-8 (not 32)
2. Tile classification eliminates 85-95% of pixels
3. NdotL backface culling eliminates ~50% of remaining pixels
4. Möller-Trumbore is fast (~10 ALU ops per triangle test)

---

## 8. Comparison of Approaches

| Criterion | Option A: Full DPM | Option B: CSM+PCSS Only | Option C: CSM+DPM Nearest (Recommended) |
|-----------|--------------------|-----------------------|----------------------------------------|
| Contact shadow quality | Perfect (ray traced) | Limited by texel size | Perfect for near-field |
| Soft shadow quality | PCSS (good) | PCSS (good) | PCSS (good) |
| Memory overhead | 144 MB | ~12 MB (3 cascade textures) | ~72-80 MB |
| GPU cost | ~4ms | ~3-4ms | ~3.5-4ms |
| Implementation complexity | High | Medium | High |
| Temporal stability | Perfect (no temporal) | Texel-snapped CSM is stable | Perfect |
| Works with existing infra | Mostly | Fully | Mostly |
| Handles skinned meshes | Yes (via GS inverse) | Yes (GeometryReplay) | Yes |
| Handles alpha-tested foliage | Needs PS alpha test | Needs PS alpha test | Needs PS alpha test |
| Near-field improvement | Dramatic | Moderate | Dramatic |
| Far-field improvement | None (PCSS covers it) | Moderate | None (PCSS covers it) |

---

## 9. Open Questions

1. **Conservative rasterization on FL 11_0**: Can we use `ID3D11Device3` / `ID3D11RasterizerState2` if the hardware supports it even at FL 11_0? Need to test — the feature may be available as an optional feature regardless of feature level.

2. **Prim buffer sizing**: How many triangles does cascade 0 contain? If Kenshi has 500 GBuffer draws and cascade 0 covers the nearest ~30% of geometry, we'd need space for ~100K-150K triangles x 36 bytes = ~3.6-5.4 MB. Much less than the DPM indices map.

3. **Alpha-tested geometry**: Foliage and other alpha-tested materials cast shadows based on the alpha channel. The DPM stores full triangles — do we need to also store UV + alpha threshold for alpha testing during ray-triangle intersection? This would add cost and memory.

4. **DPM texel overflow (d exceeded)**: The paper suggests visualizing occupancy (black=empty, white=full, red=overflow) and tuning d. For Kenshi's near cascade, d=16 may be sufficient. Monitor with a debug visualization.

5. **Game shadow map replacement**: Should we completely replace the game's shadow rendering, or render our CSM in addition? Replacing saves the game's ~2ms shadow render cost. But requires suppressing the game's shadow draws (hook `DrawIndexed` during shadow pass, skip calls).

6. **Cascade 0 size**: What world-space area should cascade 0 cover? Smaller = higher texel density = better DPM quality = fewer triangles. A 50m x 50m near cascade at 2048x2048 gives ~2.4cm per texel — excellent for contact shadows.

---

## 10. References

- Jon Story, "Hybrid Ray-Traced Shadows", GDC 2015, NVIDIA
- Kostas Anagnostou, "Hybrid Raytraced Shadows Part 2: Performance Improvements", Interplay of Light, 2018
- AMD GPUOpen, "FidelityFX Hybrid Shadows"
- Randima Fernando, "Percentage-Closer Soft Shadows", NVIDIA 2005
- Tomas Möller & Ben Trumbore, "Fast, Minimum Storage Ray/Triangle Intersection"
- GPU Gems 2, Chapter 42: "Conservative Rasterization" (Hasselgren, Akenine-Möller, Ohlsson)
- Matt Pettineo (MJP), "A Sampling of Shadow Techniques"
- Microsoft, "Direct3D 11.3 Conservative Rasterization" (learn.microsoft.com)
