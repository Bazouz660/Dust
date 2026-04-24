# Real Global Illumination with Multi-Light Shadows — Research for Dust/Kenshi

## 1. Problem Statement

### What We Want

1. **Real indirect lighting** — handles off-screen geometry, not screen-space approximations
2. **Shadows from any light source** — point lights, spot lights, sun; no artificial count limit
3. **No temporal techniques** — clean every frame; no accumulation, reprojection, history buffers
4. **No hardware RT** — D3D11 SM5.0 only; no DXR/RTX
5. **Good performance** — practical budget alongside existing effects
6. **Good visual detail** — no blurry low-frequency mush; fine enough to look real

### What We Have

Dust already has a geometry capture/replay system that intercepts every GBuffer draw and can re-render the entire scene from arbitrary viewpoints. This is the critical enabler — it means we have full scene geometry access at runtime without modifying game code.

| System | Capability |
|--------|-----------|
| `BindGeometryDraw` / `IssueGeometryDraw` | Render any captured draw with custom VS/GS/PS |
| `ReplayGeometry(VP)` | Re-render all classified draws with a replacement view-projection matrix |
| `GetGeometryDrawConstants` | Read frozen VS constant buffer data (world matrices, etc.) |
| `GetGeometryDrawBuffers` | Raw VB/IB access for compute shader consumption |
| `ShaderMetadata` | VS transform classification (STATIC vs SKINNED), matrix offsets |
| Compute shader dispatch | Working UAV read/write, 8x8 threadgroups |
| Camera/sun extraction | Sun direction, color, inverse view matrix from game CBs |

### Why Screen-Space Isn't Enough

Current RTGI and SSIL work by ray-marching the depth buffer. They:
- Miss everything behind the camera or outside the frustum
- Cannot shadow from point/spot lights (no light-perspective geometry)
- Pop when objects enter/leave the screen edges
- Cannot produce indirect light from off-screen emitters
- Are limited to the depth buffer's single-layer representation

These are fundamental limitations of operating on 2D images. Real GI requires a 3D scene representation.

---

## 2. Why Voxel Cone Tracing

### Alternatives Considered and Why They Don't Fit

**DDGI (Dynamic Diffuse GI):** Requires temporal accumulation (97% history, 3% new data per frame). Without it, needs ~4000 rays per probe per frame for noise-free results — prohibitive without hardware RT. Fails the no-temporal constraint.

**Light Propagation Volumes (LPV):** Iterative SH propagation through a coarse grid (32^3). Low quality, severe light leaking through thin walls, propagation artifacts. CryEngine abandoned it. Superseded by Radiance Hints.

**Reflective Shadow Maps + Radiance Hints:** Each light renders an RSM; VPL contributions are evaluated at sparse grid nodes. Cheap (~2-4ms for 4 lights), but the 32^3 grid resolution produces very blurry, low-frequency-only indirect lighting. Good as a lightweight fallback tier, not as a primary solution.

**SDF (Signed Distance Field) Shadows:** Best per-light shadow scaling (~0.3ms/light), beautiful penumbra. But building mesh SDFs at runtime from intercepted geometry is impractical — requires offline preprocessing or expensive per-mesh GPU generation. The Dust framework doesn't control asset loading.

**Voxel Cone Tracing (VCT):** The only technique that satisfies all constraints simultaneously. True 3D scene representation, cone tracing is inherently spatial (no temporal), D3D11 compatible (GS voxelization + compute shaders), and the architecture naturally handles unlimited lights.

### The Key Insight: Light the Voxels, Not the Pixels

The naive approach to multi-light shadows is to trace from each screen pixel toward each light. At 1080p that's ~2M pixels × N lights × ~32 ray steps = cost scales linearly with both resolution and light count. This is what makes per-pixel shadow tracing cost 0.5-1ms per light — unacceptable at scale.

The correct architecture inverts this: compute direct lighting (with shadows) **at the voxel level**, then let the per-pixel cone trace read the already-lit voxels. The cone trace doesn't know or care how many lights contributed.

Why this works:
- A 128^3 grid has ~2M voxels, but only ~50-100K are occupied by geometry
- Each occupied voxel only checks lights within its radius (clustered assignment)
- A point light with 10m radius only touches voxels in that 10m sphere — maybe a few hundred
- Shadow rays in the voxel grid are short (voxel-to-voxel DDA, not world-space ray march)
- Total cost for lighting ALL voxels from ALL lights: ~0.5-1ms combined, not per light
- The per-pixel cone trace cost is completely independent of light count

This makes the architecture scale: 1 light or 50 lights, the cone tracing pass costs the same. Only the voxel lighting pass grows, and it grows sub-linearly thanks to spatial locality.

---

## 3. Architecture

### Pipeline

```
Per Frame:

1. VOXELIZE (POST_GBUFFER, ~1-3ms)
   ├─ Clear voxel grid (or dynamic region only)
   ├─ For each captured GBuffer draw:
   │   BindGeometryDraw(i) with custom GS + PS
   │   GS projects along dominant axis
   │   PS atomically writes albedo/normal/opacity to RWTexture3D
   └─ Result: 128^3 voxel grid with scene geometry

2. LIGHT VOXELS (POST_LIGHTING, compute shader, ~0.5-1ms for all lights)
   ├─ For each occupied voxel:
   │   ├─ Sun: NdotL × shadow ray through opacity grid
   │   ├─ Each nearby point/spot light: NdotL × attenuation × shadow ray
   │   └─ Store lit radiance in 6 directional textures (anisotropic)
   └─ Generate anisotropic mipmap chain (~0.3ms)

3. CONE TRACE (POST_LIGHTING, compute shader, ~1.5-2.5ms at half-res)
   ├─ For each screen pixel (at half resolution):
   │   ├─ Reconstruct world position from depth buffer
   │   ├─ Trace 6 wide cones (~60°) across the hemisphere → diffuse GI
   │   ├─ Trace 1 narrow cone along reflection → specular GI
   │   └─ Accumulate front-to-back through mipmapped radiance
   ├─ Bilateral upscale to native resolution
   └─ Output: indirect light (RGB) + voxel AO (alpha)

4. SCREEN-SPACE REFINEMENT (POST_LIGHTING, optional, ~0.3-0.8ms)
   ├─ For each screen pixel:
   │   ├─ Short-range samples from lit HDR scene (4-8 dirs, 2-4 steps)
   │   └─ Compute fine contact color bleeding delta vs. VCT base
   └─ Add delta to cone trace output

5. COMPOSITE (POST_LIGHTING, fullscreen, ~0.2ms)
   ├─ Add indirect diffuse + specular to HDR scene
   └─ Combine voxel AO with GTAO for multi-scale occlusion
```

### Why This Order

Voxelization needs the captured GBuffer draws → must be after POST_GBUFFER. Light injection needs the sun/light data from the deferred pass constant buffers → must be at or after POST_LIGHTING. Cone tracing reads the mipmapped radiance → must follow light injection. Compositing writes to the HDR buffer → must precede tonemapping.

### How It Relates to Existing Effects

| Existing Effect | Relationship |
|----------------|-------------|
| Custom CSM (GeometryReplay) | Complementary — CSM gives pixel-accurate sun shadows; VCT gives indirect light and multi-light shadows |
| SSAO (GTAO) | Complementary — GTAO handles fine contact AO; VCT handles large-scale volumetric AO; multiply both |
| SSIL | Replaced — VCT indirect light is superior (handles off-screen, 3D, multi-bounce) |
| RTGI | Replaced — same reasons; keep as lightweight fallback quality tier |
| Shadows (PCSS patch) | Partially replaced — sun PCSS stays for direct shadow quality; VCT handles point/spot light shadows |

---

## 4. Voxelization

### How It Works

The scene is rendered into a 3D grid using a special shader pipeline. For each triangle:

1. **Vertex Shader** — the game's original VS runs via `BindGeometryDraw()`, outputting world-space position
2. **Geometry Shader** — selects the dominant axis (the axis where the triangle's face normal component is largest) and projects the triangle orthographically along that axis. This maximizes the rasterized footprint and prevents holes from triangles that are edge-on to one axis
3. **Pixel Shader** — computes the 3D voxel coordinate from the world-space position, then atomically writes albedo, normal, and opacity to `RWTexture3D<uint>` UAVs

The viewport is set to the voxel grid face resolution (128x128 for a 128^3 grid). No render target is bound — only UAVs via `OMSetRenderTargetsAndUnorderedAccessViews`.

### D3D11 Constraints

- **UAV typed loads** are limited to R32_UINT/R32_SINT/R32_FLOAT in D3D11.0. Colors must be packed into uint (RGBA8 → R32_UINT) with bit-packing helpers.
- **Atomic operations** — `InterlockedCompareExchange` on R32_UINT enables atomic averaging. Multiple fragments hitting the same voxel race to update it; the CAS loop retries until the update succeeds. Typically converges in 1-3 iterations.
- **No GS UAV writes** in D3D11.0 — only PS and CS can write to UAVs. The GS handles projection; the PS handles storage. This is fine for our pipeline.
- **Conservative rasterization** — software-emulated via GS triangle edge expansion (dilate by half a texel in clip space). Hardware CR requires D3D11.3 (`ID3D11RasterizerState2`), which may not be available at FL 11_0. Software CR adds ~10% GS cost but ensures thin triangles aren't missed.

### Static vs. Dynamic Geometry

Kenshi's `VSTransformType` classification tells us:
- **STATIC** draws (objects, terrain, foliage, triplanar) — world matrix is in the VS constant buffer. These meshes don't move between frames.
- **SKINNED** draws (characters, creatures) — bone matrices handle the world transform. These are dynamic.

For a clipmap, static geometry only needs revoxelization when the grid shifts. Dynamic (skinned) geometry needs revoxelization every frame. Since Kenshi's scene is mostly static (buildings, terrain, ruins), the per-frame voxelization cost is dominated by the small number of skinned draws (~100-400 out of 500-1200 total).

### Albedo Without Texture Access

The voxelization PS needs the surface albedo. Two options:

**Option A — Capture PS textures:** Enable `DUST_CAPTURE_PS_RESOURCES` flag to capture the bound diffuse texture SRV per draw. Bind it during voxelization and sample it in the PS. Adds ~0.4ms/frame to capture cost but gives correct per-texel color.

**Option B — Uniform albedo from CB:** Read the material color uniform from the PS constant buffer (captured via staging copy). Gives per-draw average color, not per-texel detail. Cheaper but less accurate. Acceptable for GI since indirect lighting is inherently low-frequency — the cone trace's wide aperture averages over many voxels anyway.

Option B is the pragmatic starting point. Option A is a quality upgrade for later.

---

## 5. Voxel Lighting and Multi-Light Shadows

### Light Detection

Kenshi renders point/spot lights as forward light volumes after the deferred sun pass. Each light is one draw call with `light_fs`. Per-light data is in the PS constant buffer:

| Field | Content |
|-------|---------|
| `diffuseColour` | Light RGB color |
| `falloff` | (constant, linear, quadratic, radius) |
| `position` | World-space light position |
| `power` | Light intensity multiplier |
| `direction` | Spotlight direction (zero vector for point lights) |
| `spot` | (cosInner, cosOuter, exponent) for spotlights |

Detection: in `HookedDraw`, identify light volume draws by the bound PS pointer. Read the PS CB via staging copy. Build a per-frame light array.

The sun direction and color are already available from `DustFrameContext::camera` (deferred PS CB0 registers c0/c1).

### Voxel-Level Lighting

A compute shader dispatched over the 128^3 grid processes each occupied voxel:

```
for each occupied voxel at world position P, with normal N and albedo A:
    radiance = 0

    // Sun
    NdotL = max(dot(N, sunDir), 0)
    shadow = trace_shadow_ray(P, sunDir, through opacity grid)
    radiance += A * sunColor * NdotL * shadow

    // Point/spot lights (only those whose radius reaches P)
    for each light L where distance(P, L.pos) < L.radius:
        toLight = L.pos - P
        dist = length(toLight)
        dir = toLight / dist
        NdotL = max(dot(N, dir), 0)
        atten = falloff(dist, L.radius)
        shadow = trace_shadow_ray(P, dir, dist, through opacity grid)
        radiance += A * L.color * L.power * NdotL * atten * shadow

    store radiance in 6 directional anisotropic textures
```

### Why This Scales

The shadow ray traces through the **voxel grid**, not through world-space geometry. A 128^3 grid means the maximum ray length is ~221 voxels (diagonal). With early termination on hit, most rays are much shorter. Each ray step is a single 3D texture lookup — one of the cheapest operations a GPU can do.

Crucially, each voxel only evaluates lights within reach. A point light with radius 10m in a 0.5m voxel grid only affects voxels within 20 grid cells. In a scene with 30 point lights scattered across the map, any given voxel might see 2-4 of them. The total work is proportional to `(occupied voxels) × (average lights per voxel) × (average ray length)`, not `(screen pixels) × (total lights) × (ray steps)`.

### Shadow Ray Through Opacity Grid

The shadow ray uses 3D DDA (digital differential analyzer) to step through voxels along the ray direction, checking the opacity texture at each step. Binary result: if any occupied voxel is hit, the source voxel is in shadow.

For soft shadows, instead of binary hit/miss, accumulate opacity along the ray and allow partial transparency. The cone mip trick can also be used: sample higher mip levels at greater distances for soft falloff, similar to the SDF penumbra estimator (`shadow = min(k * opacity_margin / t, 1.0)`).

### Anisotropic Storage

Lit radiance is stored in 6 directional textures (+X, -X, +Y, -Y, +Z, -Z) at half the voxel grid resolution (64^3 for a 128^3 grid). Each texel stores radiance weighted by the dot product of the voxel's normal with that direction.

Why anisotropic:
- **Prevents light leaking** — the dominant artifact in naive VCT. A thin wall stores radiance only on its lit side (the direction facing the light), not both sides. Without directional storage, mipmapping averages the lit and unlit sides, causing light to appear on the wrong side of walls.
- **Better cone tracing quality** — when tracing a cone, the direction of the cone selects the appropriate directional texture, reading radiance that actually faces the cone's approach direction.

The 6 directional textures + mip chain are generated by a compute shader after the lighting pass. Each mip level averages a 2×2×2 block of the previous level, preserving directional weighting.

---

## 6. Cone Tracing

### How It Works

For each screen pixel, reconstruct the world position from the depth buffer and trace cones through the mipmapped radiance grid.

**Diffuse GI:** 6 cones at ~60° aperture distributed around the surface normal hemisphere. As each cone marches away from the surface, its diameter grows. The mip level sampled increases with the cone diameter: `mip = log2(coneDiameter / voxelSize)`. This means nearby voxels are sampled at full resolution (fine detail) and distant voxels at coarse resolution (blurred, approximating integration over the cone's solid angle). Radiance is accumulated front-to-back with alpha compositing.

**Specular GI:** A single cone traced along the reflection direction. The cone aperture is derived from surface roughness: smooth surfaces get narrow cones (sharp reflections), rough surfaces get wide cones (blurry reflections).

**Ambient Occlusion:** The diffuse cones also accumulate opacity. Total accumulated opacity = how much of the hemisphere is blocked by geometry = ambient occlusion.

### Why Cone Tracing Works Without Temporal

Unlike screen-space ray tracing (which produces noisy results that need temporal denoising), cone tracing produces smooth output inherently. Each cone integrates over a wide solid angle — it's effectively averaging over many possible ray directions simultaneously. The hardware trilinear interpolation on the mipmapped 3D textures further smooths the result.

The output may still benefit from a light spatial filter (bilateral blur edge-aware, reusing the existing A-trous pattern from RTGI), but the primary smoothing comes from the cone aperture itself.

### Half-Resolution Tracing

The existing RTGI already implements half-resolution rendering + joint bilateral upscale. The same pattern applies to cone tracing:

1. Trace cones at half resolution (960x540 at 1080p)
2. Bilateral upscale guided by full-resolution depth and normals (3×3 kernel)
3. Cost reduction: ~75% of the cone tracing pass

This is the single largest optimization lever. Half-res cone tracing at ~1-1.5ms is practical for a shipped effect.

### Screen-Space Detail Refinement

VCT's fundamental limitation is voxel resolution. A 0.5m voxel is ~50 pixels wide at typical Kenshi camera distance. The cone trace produces smooth, correct large-scale indirect lighting but lacks the fine contact color bleeding that RTGI delivers at pixel resolution.

Rather than running two full GI systems, a lightweight screen-space refinement pass can add high-frequency detail on top of VCT's low-frequency base. The idea: VCT has already solved the hard problems (off-screen geometry, multi-bounce, multi-light, stability), so the screen-space pass doesn't need to do any of that. It only needs to capture the near-field color bleeding that VCT's grid is too coarse to represent.

**How it works:**

1. VCT produces the indirect lighting base (large-scale bounce, volumetric AO, off-screen contribution)
2. A cheap screen-space pass samples a few nearby pixels from the lit HDR scene (~4-8 directions, 2-4 steps per direction — much less than RTGI's 8+ directions and 6+ steps)
3. At each sample, compute the visibility angle and distance falloff, same as SSIL
4. Subtract the VCT contribution at that sample position to get the **delta** — the fine detail VCT missed
5. Add the delta to the final output

The key differences from running full RTGI:

| | Full RTGI | Screen-Space Refinement |
|---|---|---|
| Ray length | Long (whole screen) | Short (nearby pixels only, ~5-10% of screen extent) |
| Ray count | 8+ directions × 6+ steps | 4-8 directions × 2-4 steps |
| Purpose | All indirect lighting | Only fine contact detail |
| Off-screen handling | None (screen-space blind spot) | Doesn't need it (VCT handles it) |
| Multi-bounce | Temporal feedback (prevGI) | Not needed (VCT handles it) |
| Denoising | A-trous + temporal | Light spatial blur or none (few samples = low noise) |
| Cost | 2-4ms | ~0.3-0.8ms |

**Why this is cheap:** The expensive parts of RTGI are the long rays (many steps to traverse the screen), the denoising (temporal + spatial to clean up noise from sparse sampling), and the multi-bounce feedback. The refinement pass uses short rays (contact detail is local), fewer samples (VCT already provides the baseline so we only need the delta), and no temporal feedback (VCT handles multi-bounce). The result is roughly 5-10× cheaper than full RTGI.

**Why this works visually:** The human eye is sensitive to fine contact darkening and color bleeding at object boundaries — that's what makes a scene feel "grounded." VCT provides the broad ambient context (warm light from a sunlit wall across a room), and the refinement pass adds the tight color bleeding at object contact points (the reddish tint on the floor directly next to a red barrel). Together they cover the full spatial frequency range of indirect lighting.

**When to skip it:** On lower-end GPUs, the refinement pass can be disabled entirely. VCT alone looks good at the macro level — the missing fine detail is only noticeable in close-up comparisons. This makes it a natural quality setting: Low = VCT only, High = VCT + screen-space refinement.

---

## 7. Clipmap Strategy

### Why a Single Grid Isn't Enough

A single 128^3 grid covering 64m means 0.5m voxels. At the RTS camera distance in Kenshi (~15-30m), this gives reasonable detail for nearby buildings and characters. But the game world extends hundreds of meters — distant structures contribute ambient light that a 64m grid misses entirely.

### Two-Level Clipmap

| Level | Resolution | World Coverage | Voxel Size | Purpose |
|-------|-----------|---------------|------------|---------|
| 0 (near) | 128^3 | 64m × 64m × 64m | 0.5m | Detailed GI + light shadows |
| 1 (far) | 128^3 | 256m × 256m × 256m | 2.0m | Coarse ambient fill |

Both levels are centered on the camera. When the camera moves, the grid contents shift: existing voxels that are still within the new bounds are kept, and only the newly-exposed boundary slices are revoxelized. This amortizes the voxelization cost — instead of revoxelizing the entire scene every frame, you revoxelize a thin slice proportional to camera movement speed.

For Kenshi's RTS camera (which moves slowly and smoothly), the per-frame shift is typically 0-2 voxels, meaning 0-2 slices of 128×128 = 0-32K voxels to fill. This is a fraction of the full grid's 2M voxels.

### Cone Tracing Across Cascades

The cone trace starts in cascade 0 (fine detail). When it exits cascade 0's bounds, it seamlessly transitions to cascade 1 (coarser, larger extent). This is analogous to how cascaded shadow maps blend between cascades — the transition should be smooth because the cone diameter at the cascade boundary is large enough that the coarser resolution is acceptable.

---

## 8. Honest Quality Assessment

### What VCT Does Well

- **Large-scale indirect illumination** — sunlight bouncing off a canyon wall and illuminating the opposite side. A warm glow inside a building from the sunlit exterior. These effects are impossible in screen-space and transformative for visual quality.
- **Multi-light shadow coverage** — every point light in a town casts shadows through the voxel grid. Buildings block light from torches on the other side. This works at any light count.
- **Volumetric AO** — rooms feel enclosed, overhangs create natural darkness, large-scale occlusion that GTAO can't reach.
- **Off-screen stability** — no GI pop when objects enter/leave the screen.

### What VCT Does Poorly

- **Fine shadow detail** — a 0.5m voxel cannot represent a fence post's shadow. Characters at 0.5m resolution are 3-4 voxels tall. Their shadows in the voxel grid are blocky. This is why the custom CSM must handle sun shadows — VCT shadows are for fill/ambient, not hero shadows.
- **Thin geometry** — a thin wall (< 1 voxel thick) may have holes in the voxel grid. Anisotropic storage mitigates the light leaking consequence, but the geometry representation itself is imperfect.
- **Indoor scenes** — light leaking is most visible indoors where walls are expected to fully block light. Kenshi is primarily outdoor (RTS camera, desert/wasteland), which is the best-case scenario for VCT.
- **Specular quality** — cone-traced specular reflections are blurry at all but very smooth surfaces. They approximate environment reflections, not mirror-quality reflections. For Kenshi's materials (rough stone, sand, metal, cloth), this is acceptable.
- **Revoxelization cost** — full-scene voxelization every frame is the most expensive pass. Clipmap partial updates reduce this but add implementation complexity.

### Closing the Detail Gap

The screen-space refinement pass (Section 6) addresses VCT's primary visual weakness. Without it, VCT provides correct but blurry indirect lighting — you see the right colors and intensities but the contact-level detail is soft. With it, near-field color bleeding and contact darkening approach RTGI quality at a fraction of the cost, while VCT handles everything RTGI can't (off-screen, multi-bounce, multi-light, stability).

The combined system covers the full spatial frequency range:
- **GTAO** — pixel-level contact AO (crevices, edges)
- **VCT** — large-scale indirect light + volumetric AO + multi-light shadows
- **Screen-space refinement** — fine contact color bleeding (the RTGI-quality detail layer)

### Realistic Expectations

VCT + refinement gives you a complete indirect lighting solution. The broad illumination context comes from the voxel grid (what does light look like after it bounces off the world?), and the fine detail comes from the screen-space pass (what does the nearby surface color bleed onto this pixel?). Neither system alone is sufficient, but together they cover what matters without the cost or limitations of running full RTGI.

---

## 9. Performance Analysis

### Budget Breakdown (GTX 1060+ at 1080p, conservative estimates)

| Pass | Cost | Scales With |
|------|------|-------------|
| Voxel clear (dynamic region) | 0.2ms | Grid resolution |
| Voxelization (full scene) | 1.5-3ms | Draw count (~500 draws), grid resolution |
| Light injection (all lights) | 0.5-1ms | Occupied voxels × average lights per voxel |
| Mipmap generation (6 directions) | 0.3-0.5ms | Grid resolution, 6 textures × ~6 mip levels |
| SS detail refinement (optional) | 0.3-0.8ms | Screen resolution, sample count |
| Cone tracing (half-res + upscale) | 1.5-2.5ms | Screen resolution, cone count, trace distance |
| Spatial filter | 0.2-0.3ms | Screen resolution |
| Composite | 0.1ms | Screen resolution |
| **Total (without refinement)** | **4-8ms** | |
| **Total (with refinement)** | **5-9ms** | |

### What Drives Cost

**Voxelization** dominates when draw count is high. Kenshi's densest scenes have ~1200 GBuffer draws, of which ~800 are classified (have known VS transform). The GS adds overhead per triangle (dominant axis projection + conservative rasterization). Frustum culling against the voxel grid AABB can skip distant draws.

**Light injection** is cheap because only occupied voxels are processed (~50-100K out of 2M), and each voxel only evaluates lights within reach. The shadow ray through the voxel grid is a simple DDA with early termination. Even with 30 lights in the scene, if each voxel only "sees" 2-3 nearby lights, the effective work is modest.

**Cone tracing** is the most GPU-intensive pass per pixel. 6 cones × ~20-40 steps × 6 texture samples per step (anisotropic directions) = ~720-1440 texture samples per pixel at full res. At half res: ~180-360K pixels × 720-1440 samples = 130-520M texture fetches. 3D texture sampling is slower than 2D (worse cache coherence), but the mip chain means most samples hit small, cache-friendly mip levels.

### Optimization Levers (in priority order)

1. **Half-res cone tracing + bilateral upscale** — already described, saves ~60-75% of cone trace cost. This is essentially mandatory.
2. **Stagger static voxelization** — revoxelize static geometry only when the clipmap shifts. Dynamic (skinned) geometry every frame. Reduces voxelization cost by ~60-80% in most frames.
3. **Fewer cones** — 4 instead of 6 diffuse cones with rebalanced weights. ~33% cone trace savings, minor quality loss.
4. **Smaller grid** — 64^3 instead of 128^3. 8× less voxelization and lighting work, 8× less memory, but 2× coarser (1.0m voxels). The quality tradeoff is significant — probably too coarse for near-field.
5. **Skip specular cone** — for non-metallic surfaces (metalness < 0.5), skip the specular cone entirely. Saves ~14% of cone trace work.
6. **Depth-based cone termination** — don't trace cones for distant pixels (> 80% of depth range). They contribute negligible indirect light at the RTS camera distance.
7. **Indirect-only for distant cascades** — cascade 1 doesn't need per-voxel shadow tracing from point lights (too far for shadow detail to matter). Only sun + ambient.

### Reference Points

- **The Tomorrow Children** (PS4, 2016): full VCT GI at 1080p, 128^3 clipmap, ~4ms total on PS4 GPU (roughly GTX 750 Ti class). Production-shipped game.
- **NVIDIA VXGI SDK** (2015-2018): 256^3 clipmap, ~3-6ms on GTX 980 at 1080p. This includes voxelization + cone tracing.
- **CryEngine SVOGI**: 64^3 sparse voxel octree, ~2-4ms on contemporary hardware.

These numbers suggest our 4-8ms target is achievable on GTX 1060+ hardware, especially with half-res tracing and clipmap partial updates.

---

## 10. Memory Budget

| Resource | Format | Resolution | Size |
|----------|--------|-----------|------|
| Voxel albedo (cascade 0) | R32_UINT | 128^3 | 8 MB |
| Voxel normal (cascade 0) | R32_UINT | 128^3 | 8 MB |
| Voxel opacity (cascade 0) | R32_UINT | 128^3 | 8 MB |
| Voxel albedo/normal/opacity (cascade 1) | R32_UINT × 3 | 128^3 | 24 MB |
| Anisotropic radiance (6 dirs, cascade 0, half-res + mips) | RGBA16F | 64^3 × 6 × 1.33 | ~17 MB |
| Anisotropic radiance (6 dirs, cascade 1, half-res + mips) | RGBA16F | 64^3 × 6 × 1.33 | ~17 MB |
| Cone trace output | RGBA16F | half-res (960×540) | ~4 MB |
| Upscaled + filtered output | RGBA16F | native (1920×1080) | ~15 MB |
| **Total** | | | **~101 MB** |

This is significant but within modern GPU VRAM budgets (4+ GB). The two cascades double the voxel cost. If memory is tight, cascade 1 can use 64^3 instead of 128^3 (saves ~30 MB) or be dropped entirely (saves ~41 MB).

For comparison, Kenshi's existing shadow map is 4096×4096 × R32_FLOAT + D32_FLOAT = ~128 MB. A 101 MB GI system is in the same ballpark.

---

## 11. D3D11-Specific Implementation Notes

### UAV Atomics for Voxelization

D3D11 supports `InterlockedCompareExchange`, `InterlockedAdd`, `InterlockedMax` on R32_UINT UAVs. The atomic averaging pattern for overlapping voxel writes:

```
pack RGBA8 color into uint32
CAS loop: read current value, average with new value, attempt write
retry if another thread modified the value between read and write
typically converges in 1-3 iterations
```

### 3D Texture Limits and Formats

- Max 3D texture size: 2048^3 (128^3 is well within limits)
- Hardware trilinear filtering on 3D textures is supported and used for cone tracing mip sampling
- `SampleLevel` with float mip parameter enables fractional mip interpolation (smooth transitions between mip levels)

### Voxelization Without Render Targets

Use `OMSetRenderTargetsAndUnorderedAccessViews` with null RTVs (NumRTVs=0) and UAVs bound starting at UAVStartSlot=0. Set viewport to grid face resolution. D3D11 supports this mode — the rasterizer runs normally, fragments invoke the PS, but there's no color output; the PS writes only to UAVs.

### Compute Shader Dispatch

- **Voxel lighting:** `Dispatch(128/4, 128/4, 128/4)` = 32×32×32 = 32K threadgroups at [4,4,4] threads each
- **Cone tracing (half-res):** `Dispatch(960/8, 540/8, 1)` = 120×68 = 8K threadgroups at [8,8,1] threads each
- **Mip generation:** `Dispatch(res/4, res/4, res/4)` per mip level, per directional texture

### Simultaneous UAV + SRV Binding

D3D11 does not allow the same resource to be bound as both UAV and SRV simultaneously. The voxelization pass writes to UAVs; the lighting pass reads via SRV and writes to different UAVs (radiance textures); the cone trace pass reads all via SRV. Each pass uses different resources for input vs. output, so there's no conflict.

---

## 12. Implementation Phases

### Phase 1: Voxelization Pipeline

Build the scene voxelizer using `BindGeometryDraw` / `IssueGeometryDraw`:
1. Create 128^3 R32_UINT 3D textures (albedo, normal, opacity) with UAV + SRV bind flags
2. Write voxelization GS (dominant axis projection, software conservative rasterization)
3. Write voxelization PS (world-to-voxel coordinate, atomic RGBA8 write)
4. Handle STATIC vs. SKINNED transforms (extract world position from VS CB staging copies)
5. Debug visualization: fullscreen PS that raycasts the voxel grid and renders opacity as a volume

**Deliverable:** Debug view showing the voxelized scene matching the GBuffer geometry.

### Phase 2: Direct Lighting in Voxels

1. Implement light injection compute shader (sun only)
2. Shadow ray through opacity grid (3D DDA, binary hit/miss)
3. Write lit radiance into 6 anisotropic half-res textures
4. Mipmap generation compute shader
5. Debug visualization: render lit voxels, verify sun shadows in the voxel grid

**Deliverable:** Voxels correctly lit and shadowed by the sun. Viewable in debug overlay.

### Phase 3: Cone Tracing + Composite

1. Fullscreen compute shader: 6 diffuse cones, anisotropic radiance sampling, front-to-back compositing
2. Half-resolution rendering + joint bilateral upscale (reuse RTGI pattern)
3. Spatial denoiser (reuse A-trous pattern from RTGI, optional)
4. Additive composite onto HDR scene
5. Combined AO: `finalAO = gtao * vctAO`

**Deliverable:** Working diffuse GI from voxel cone tracing. First real visual result.

### Phase 4: Multi-Light Support

1. Hook light volume draws to extract point/spot light parameters
2. Add per-light contributions to voxel lighting compute shader (clustered: skip lights out of range)
3. Verify: point lights illuminating voxels, casting voxel shadows

**Deliverable:** Indirect lighting influenced by all active point/spot lights in the scene.

### Phase 5: Screen-Space Detail Refinement

1. Short-range screen-space pass: sample nearby lit pixels (4-8 directions, 2-4 steps)
2. Compute delta vs. VCT base (fine detail that VCT missed)
3. Blend onto cone trace output before composite
4. Make it toggleable as a quality tier (Low = VCT only, High = VCT + refinement)

**Deliverable:** Near-RTGI-quality contact color bleeding layered on VCT's stable base.

### Phase 6: Clipmap + Polish

1. Two-level clipmap (128^3 near + 128^3 far)
2. Camera-relative grid scrolling (shift contents, revoxelize boundary slices)
3. Separate static/dynamic voxelization (static revoxelizes on grid shift only)
4. Specular cone tracing
5. Quality presets (resolution, cone count, trace distance, cascade count)
6. Performance presets (ultra/high/medium/low)

**Deliverable:** Production-ready VCT GI system with quality tiers.

---

## 13. Open Questions

1. **Voxelization VS compatibility** — `BindGeometryDraw` replays the game's original VS. Does the game's VS output world-space position that the GS can use directly, or does it output clip-space only? If clip-space only, the GS needs the inverse VP matrix to recover world position, or we need a custom VS that reads the world matrix from the staging CB copy. The `ShaderMetadata` system knows the matrix offsets — this should be solvable.

2. **GS + UAV pipeline state** — OGRE's original draw state may include a hull/domain shader (52% of shadow draws are tessellated). During voxelization replay, we replace VS/GS/PS but need to explicitly unbind HS/DS. Verify that `BindGeometryDraw` handles this.

3. **Voxelization throughput** — the GS is a known bottleneck on some GPU architectures (especially AMD GCN). If voxelization is too slow with GS-based projection, an alternative is 3-pass rasterization (one pass per dominant axis, with the PS handling axis selection). This removes the GS entirely at the cost of 3× draw calls.

4. **Opacity grid precision** — binary opacity (occupied/empty) is coarse. A fractional opacity per voxel (from alpha-tested foliage, semi-transparent materials) would improve shadow quality for vegetation. This requires the voxelization PS to read the game's alpha texture — needs `DUST_CAPTURE_PS_RESOURCES`.

5. **Frame ordering** — can we voxelize at POST_GBUFFER (when geometry capture is available) and then light voxels at POST_LIGHTING (when sun/light data is available)? Or does the geometry capture data persist across injection points within the same frame? The `DustHostAPI` documentation says captures are "valid from end of GBuffer pass until ResetFrame" — so yes, this should work.

6. **Cubemap interaction** — Kenshi renders a 512×512 cubemap for IBL. VCT could eventually replace this with cone-traced environment probes from the voxel grid, producing higher-quality and dynamic IBL. But this is a Phase 5+ goal.

---

## 14. Fallback: RSM + Radiance Hints

For lower-end GPUs where VCT is too expensive, Radiance Hints (Papaioannou 2011) provides a lighter GI tier:

- Generate RSMs from each light via `ReplayGeometry()` (128×128 per light, 4 MRT)
- Evaluate VPL contributions at a 32×8×32 3D grid of SH nodes
- Multi-bounce via ping-pong SH volumes (2 bounces)
- Trilinear SH lookup per pixel

Cost: ~2-4ms for 4 lights. Memory: ~10 MB. Quality: blurry, low-frequency, visible light leaking indoors. But functional and spatially stable. Could serve as the "Low" quality preset.

---

## 15. References

1. Crassin et al., "Interactive Indirect Illumination Using Voxel Cone Tracing" (2011) — the foundational paper
2. NVIDIA VXGI SDK — production implementation with clipmap and anisotropic voxels
3. McLaren, "The Technology of The Tomorrow Children" (GDC 2015) — production VCT on PS4
4. CryEngine SVOGI — sparse voxel octree GI in a shipped engine
5. Papaioannou, "Real-Time Diffuse Global Illumination Using Radiance Hints" (2011) — fallback technique
6. Thiedemann et al., "Voxel-based Global Illumination" (2011) — deferred voxel shading with performance breakdown
7. GPU Gems 2, Chapter 42: "Conservative Rasterization" — software CR via GS edge expansion
8. Crassin, "GigaVoxels: A Voxel-Based Rendering Pipeline" (2011) — hierarchical voxel structures
9. Kaplanyan & Dachsbacher, "Cascaded Light Propagation Volumes" (2010) — LPV reference (superseded by VCT)
10. Quilez, "Soft Shadow Functions" — SDF-style penumbra estimation applicable to opacity tracing
