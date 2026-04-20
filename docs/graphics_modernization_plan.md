# Dust Graphics Modernization Plan

## 1. Current State

### What the Framework Does

Dust is a D3D11 rendering mod for Kenshi (OGRE engine) that hooks into the game's D3D11 pipeline via KenshiLib. It loads as a RE_Kenshi plugin, installs VTable-level hooks on D3D11 device/context functions, and patches the game's deferred lighting shader at runtime via a D3DCompile hook.

**Hooked D3D11 Functions:**
- `CreateTexture2D` — shadow map resolution override, texture detection
- `CreatePixelShader` — reserved (passthrough)
- `Draw` — fullscreen draw interception (pipeline detection + effect dispatch)
- `DrawIndexed` — passthrough (diagnostic counting only)
- `DrawIndexedInstanced` — passthrough
- `OMSetRenderTargets` — render target tracking
- `D3DCompile` — runtime shader source patching (AO injection, shadow filtering)
- `Present` / `Present1` — frame boundary, ImGui rendering
- `ResizeBuffers` — resolution change handling

**Game Loop Hook:**
- `GameWorld::_NV_mainLoop_GPUSensitiveStuff` — per-frame state reset before the game renders

### Implemented Effects

| Effect | Technique | Injection Point | What It Does |
|--------|-----------|-----------------|--------------|
| **SSAO** | GTAO (12 dir, 6 steps) | POST_LIGHTING (pri 0) | Ambient occlusion from depth+normals, applied to indirect light only via deferred shader patch |
| **SSIL** | Screen-space radiance sampling (8 dir x 4 steps) | POST_LIGHTING (pri 10) | Colored indirect light bounce from lit surfaces, additive composite |
| **SSS** | Depth-buffer ray march toward sun | POST_LIGHTING (pri 20) | Screen-space contact shadows, multiplicative composite |
| **RTGI** | Depth-buffer ray trace + temporal accumulation + A-trous wavelet denoise (compute shader) | POST_LIGHTING (pri 5) | Screen-space global illumination with multi-bounce feedback |
| **Shadows** | PCSS via deferred shader patch (12-sample Poisson disk, per-pixel rotation, blocker search) | POST_LIGHTING (pri 30) | Improved filtering of game's RTWSM shadow map |
| **LUT** | 32x32x32 float LUT + ACES tonemapping + dithering | POST_TONEMAP (pri 0) | Replaces game's tonemapper entirely, eliminates double-quantization banding |
| **Bloom** | Threshold extract + progressive downsample/upsample | POST_TONEMAP (pri 100) | HDR bloom with curve control |
| **Clarity** | Unsharp mask with midtone protection | POST_TONEMAP (pri 50) | Local contrast enhancement |
| **DOF** | Depth-based blur | POST_TONEMAP | Depth of field |

### Available Resources

| Resource | Format | Access | Source |
|----------|--------|--------|--------|
| Depth | R32_FLOAT | SRV (slot 2 during lighting) | GBuffer |
| Normals | B8G8R8A8_UNORM | SRV (slot 1 during lighting) | GBuffer |
| Albedo | B8G8R8A8_UNORM | SRV (slot 0 during lighting) | GBuffer |
| HDR Scene | R11G11B10_FLOAT | RTV (lighting output) | Deferred pass |
| LDR Scene | B8G8R8A8_UNORM | RTV (tonemap output) | Post-process |
| Sun Direction | float3 | CB0 offset c0 | Game constant buffer |
| Sun Color | float4 | CB0 offset c0+16 | Game constant buffer |
| Inverse View Matrix | float4x4 | CB0 offset c8 | Game constant buffer |
| Projection Matrix | float4x4 | CB0 (further offset) | Game constant buffer |
| Shadow Map | R32_FLOAT | PS SRV during lighting | Game RTWSM |
| Warp Map | — | PS SRV during lighting | Game RTWSM |

### Proven Infrastructure

These systems exist and are production-tested:

- **Compute shader dispatch** — 8x8 threadgroups, UAV read/write (RTGI denoiser)
- **Temporal accumulation** — camera reprojection via inverse view matrix, motion-adaptive blending, previous-frame depth tracking (RTGI)
- **A-trous wavelet denoising** — depth/normal edge-stopping, variance-guided filtering (RTGI)
- **Constant buffer readback** — async staging buffer, no GPU stall (RTGI camera data)
- **Runtime shader compilation** — host API CompileShader/CompileShaderFromFile
- **GPU state save/restore** — full D3D11StateBlock capture
- **Resource management** — ResourceRegistry with named SRV/RTV tracking
- **Pipeline detection** — state-based pass identification (no shader hash dependency)
- **Hot-reload config** — INI polling, preset system, per-setting GUI

---

## 2. The Limitation

All current effects are **screen-space post-processing**. They operate on 2D images (depth, normals, albedo, lit scene) produced by the game's renderer. They cannot:

- **See off-screen geometry** — RTGI, SSIL, SSS all miss objects behind the camera or outside the frustum
- **Access scene geometry** — no vertex data, no triangle data, no mesh information
- **Render from arbitrary viewpoints** — can't create depth/color from the sun's perspective, a reflection camera, or a light probe position
- **Replace the shadow system** — the RTWSM is computed by the engine on the CPU; the warp tree, shadow matrix, and cascade setup are all inaccessible
- **Modify per-object rendering** — can't apply different shaders to skin vs. metal vs. foliage
- **Cast shadows from arbitrary light sources** — only the game's single sun shadow map exists

These limitations are inherent to the post-processing approach. No amount of clever shader work can overcome the fundamental lack of scene geometry access.

---

## 3. The Shadow Problem in Detail

### Why Kenshi's Shadows Look Bad

Kenshi uses RTWSM (Resolution Tree Warped Shadow Maps) — a single shadow map with non-linear UV warping to concentrate resolution near the camera.

**Problems we cannot fix from the GPU side:**

| Problem | Root Cause | Why We Can't Fix It |
|---------|-----------|---------------------|
| Temporal flickering | CPU-side warp tree recomputation shifts texels frame-to-frame | Warp is computed before any D3D11 call we can intercept |
| Limited resolution | Single 2048x2048 map covers the entire shadow range | Intercepting CreateTexture2D can upscale, but the warp still starves far regions |
| No cascades | Engine renders one warped shadow map, not CSM splits | Would require engine-level light matrix setup |
| Warp artifacts | Extreme compression ratios create discontinuities | The warp function is a black box from the GPU |

**Problems we have partially fixed:**

| Problem | Fix | Remaining Issues |
|---------|-----|-----------------|
| Blocky shadow edges | 12-sample Poisson disk with per-pixel rotation (DustRTWShadow) | Still limited by shadow map texel density |
| No penumbra variation | PCSS blocker search for distance-based softening | Penumbra accuracy limited by shadow map resolution |
| Missing contact shadows | Screen-space ray march toward sun (SSS effect) | Screen-space only — misses off-screen occluders |

### What We Actually Need

Custom Cascaded Shadow Maps rendered from the sun's perspective. This requires:
1. Knowledge of what geometry exists in the scene
2. The ability to render that geometry from the light's viewpoint
3. Control over cascade split distances and shadow map resolution

This is not possible with post-processing. It requires geometry access.

---

## 4. Approaches Considered

### A. NVIDIA Hybrid Ray Traced Shadows (GDC 2015 Paper)

**Technique:** Build a "Deep Primitive Map" — a 3D buffer storing actual triangles per shadow map texel. For each screen pixel, do ray-triangle intersection against stored primitives. Blend hard ray-traced result with PCSS using blocker distance.

**Requirements:**
- Geometry Shader to emit world-space triangle vertices + SV_PrimitiveID into a structured buffer
- Conservative rasterization (DX11.3 HW or software GS-based emulation)
- Re-rendering scene from the light's perspective into the primitive map
- Knowledge of vertex formats and transforms

**Verdict:** The core idea (ray tracing against scene geometry for shadows) is sound, but the specific Deep Primitive Map technique requires too much geometry pipeline knowledge. The per-texel triangle storage also scales poorly with scene complexity (128-256 MB for moderate scenes). The hybrid blending approach (hard ray-traced shadows near contact, soft PCSS far from occluder) is a valuable concept we should adopt regardless of technique.

### B. Shadow Map Heightfield Ray Tracing

**Technique:** Treat the game's shadow map as a heightfield. For each pixel, march a ray from the surface toward the light in world space, projecting each step into shadow map space and comparing against the stored depth.

**Requirements:**
- Capture the shadow map SRV and warp map SRV during the deferred pass
- Extract the shadow matrix from the game's constant buffer
- Compute shader for the ray march
- Temporal accumulation + denoising (reuse from RTGI)

**Pros:**
- No geometry access needed — works with current framework
- Covers off-screen occluders (shadow map sees them)
- Quick-reject optimization: only trace penumbra pixels (~5-15% of frame)
- Reasonable performance (~1-1.5ms)

**Cons:**
- Quality limited by shadow map resolution (tracing through a low-res heightfield)
- RTWSM warp adds per-step cost and potential accuracy issues
- Doesn't fix temporal flickering (still depends on the game's unstable shadow map)
- Can miss thin objects (heightfield has one depth per texel)
- Band-aid over a fundamentally broken shadow map

**Verdict:** Moderate improvement over current PCSS filtering, but doesn't solve the root problem. Still dependent on the game's RTWSM with all its limitations. Worth considering as a short-term improvement but not the long-term solution.

### C. Screen-Space Enhancements (Extend Current Approach)

Additional effects that stay within the post-processing paradigm:

| Effect | Description | Impact |
|--------|-------------|--------|
| **SSR** | Screen-space reflections via depth-buffer ray march | High — no reflections currently exist |
| **SMAA** | Subpixel morphological AA (spatial only, no temporal) | High — replaces FXAA, sharper edges without smearing |
| **Volumetric Lighting** | Ray march depth buffer with scattering model for god rays / atmospheric dust | High — fits Kenshi's desert aesthetic |
| **PBR Deferred Upgrade** | Patch deferred shader to use Cook-Torrance GGX BRDF | Medium — better material response |

**Verdict:** All valuable and should be built regardless. But they don't solve the geometry access problem. They're complementary to, not a replacement for, the geometry capture approach.

### D. Geometry Interception via DrawIndexed Hook (Recommended)

**Technique:** Intercept DrawIndexed calls during the GBuffer pass. Capture draw state (VB, IB, VS, CB, input layout). After the GBuffer pass, replay captured draws from the sun's perspective into custom cascaded shadow maps.

**Requirements:**
- Activate the existing DrawIndexed hook for state inspection
- Identify GBuffer-pass draws by render target signature (3 MRT + depth)
- Map OGRE's VS constant buffer layout to find the view-projection matrix
- Create custom shadow map render targets (depth-only, multiple cascades)
- Replay draws with swapped constant buffer (light-space VP)
- Patch deferred shader to read custom CSM

**Pros:**
- Solves shadows at the root — proper CSM with stable cascades
- No temporal flickering (texel-aligned cascade snapping)
- Any shadow distance (configurable cascade splits)
- Any filter technique (PCSS, VSM, ESM on proper CSM)
- Foundation for all future geometry-aware features
- Uses existing hooks — no new API interception needed

**Cons:**
- Requires reverse-engineering OGRE's VS constant buffer layout (one-time effort)
- Doubles geometry rendering cost (replay from light's perspective)
- Need to handle edge cases (skinned meshes, instancing, LOD transitions)

**Verdict:** This is the correct long-term path. It solves the root problem and enables everything else.

### E. D3D12 Interop + DXR Hardware Ray Tracing

**Technique:** Create a D3D12 device alongside D3D11 via `ID3D11On12Device`. Share resources between APIs. Build DXR acceleration structures (BLAS/TLAS) from captured geometry. Dispatch hardware ray tracing for shadows, reflections, GI.

**Requirements:**
- Everything from approach D (geometry capture)
- D3D12 device creation and management
- Resource sharing/synchronization between D3D11 and D3D12
- BVH construction from vertex data
- DXR shader pipeline (ray generation, closest hit, miss shaders)
- DXR-capable GPU (NVIDIA RTX 20xx+ / AMD RX 6xxx+)

**Pros:**
- True hardware-accelerated ray tracing
- Perfect shadows, reflections, GI with actual geometry intersection
- Modern AAA-quality rendering
- Essentially what NVIDIA RTX Remix does for legacy games

**Cons:**
- Massive implementation effort (months of work)
- Requires managing two graphics APIs simultaneously
- Limits audience to DXR-capable GPUs
- Complex vertex format parsing for BVH construction
- Synchronization between D3D11 and D3D12 contexts

**Verdict:** The ultimate endgame for "no compromises" graphics. But it requires approach D as a prerequisite (you need geometry capture before you can build acceleration structures). Consider this a Phase 2 goal after CSM is working.

---

## 5. Recommended Roadmap

### Phase 1: Geometry Capture Foundation

**Goal:** Intercept and catalog all GBuffer-pass DrawIndexed calls.

**Step 1.1 — GBuffer Draw Survey**
- In `HookedDrawIndexed`, detect when the game is rendering into the GBuffer (3 MRT + depth bound)
- For each GBuffer draw, log: index count, vertex buffer stride/offset, input layout pointer, VS pointer, VS constant buffer contents, PS pointer, bound textures
- Run for a few frames to map out OGRE's patterns
- Identify the VS constant buffer offset that contains the World-View-Projection matrix

**Step 1.2 — Draw Call Capture**
- During the GBuffer pass, save COM references (AddRef) to each draw call's state: VB, IB, input layout, VS, VS CBs
- Store in a per-frame vector, cleared at frame start (in `ResetFrameState`)
- No replaying yet — just capture and verify the list is complete and stable

**Step 1.3 — Geometry Replay Proof of Concept**
- After the GBuffer pass (detected when the deferred lighting fullscreen draw fires), replay captured draws into a simple R32_FLOAT depth-only render target
- Use the game's original VS but with a modified constant buffer (orthographic projection from above, for example)
- Verify the output matches expected geometry

### Phase 2: Custom Shadow Maps

**Goal:** Replace the game's RTWSM with proper Cascaded Shadow Maps.

**Step 2.1 — Light-Space Rendering**
- Compute a sun view matrix from the sun direction (extracted from CB0 c0)
- Compute cascade split distances (logarithmic or practical split scheme)
- For each cascade: compute tight orthographic projection, snap to texel grid for stability
- Replay captured geometry into each cascade's depth buffer

**Step 2.2 — CSM Integration**
- Bind custom CSM textures to available shader registers in the deferred pass
- Patch the deferred shader to sample from custom CSM instead of RTWSM
- Implement PCF or PCSS filtering on the CSM
- Add cascade blending at split boundaries

**Step 2.3 — Shadow Quality Polish**
- Per-cascade resolution control (e.g., 2048 near, 1024 mid, 512 far)
- Cascade stabilization (texel snapping, bounding sphere fitting)
- Normal-offset bias to eliminate acne without peter-panning
- Optional VSM/ESM for softer filtering

### Phase 3: Multi-Light Shadows

**Goal:** Support shadow-casting point lights and spot lights beyond the sun.

**Step 3.1 — Light Detection**
- Identify light sources from the game's constant buffers or forward-pass draw calls
- Extract light position, direction, color, attenuation for each light

**Step 3.2 — Per-Light Shadow Maps**
- For point lights: render to a shadow cubemap (6 faces)
- For spot lights: render to a single shadow map with perspective projection
- Budget: shadow maps for N closest/brightest lights per frame

**Step 3.3 — Deferred Shader Multi-Light Integration**
- Patch deferred shader to accumulate light contributions from all shadow-mapped lights
- Physically-based attenuation (inverse square falloff)
- Shadow map filtering per light

### Phase 4: Material Classification & Per-Object Shading

**Goal:** Apply different shading models to different surface types.

**Step 4.1 — Draw Call Classification**
- During geometry capture, hash the pixel shader + bound textures to identify material classes
- Build a material database: which PS hash = skin, foliage, metal, terrain, etc.
- May require manual annotation initially, with heuristics to automate later

**Step 4.2 — Material-Specific GBuffer Extension**
- Add a material ID to the GBuffer (could use alpha channel of normals, or a separate R8 target)
- During geometry replay or via PS patch, write the material class ID
- Deferred shader reads material ID to select BRDF

**Step 4.3 — Custom Material Shaders**
- Subsurface scattering for skin (screen-space diffusion based on depth + material mask)
- Translucency for foliage (back-lighting based on light direction + thickness)
- Enhanced PBR for metals (GGX specular, Fresnel, roughness)
- Terrain detail (parallax mapping, detail normal blending)

### Phase 5: Full-Scene Global Illumination

**Goal:** Replace screen-space RTGI with geometry-aware GI.

**Step 5.1 — Reflective Shadow Maps (RSM)**
- During shadow map rendering, also output world-space normals and flux (albedo * light)
- Each shadow map texel becomes a Virtual Point Light (VPL)
- Scatter VPL contributions to nearby surfaces in the deferred pass

**Step 5.2 — Light Probe Grid**
- Place a sparse 3D grid of light probes in the scene
- Update probes by rendering low-res cubemaps from probe positions (using geometry replay)
- Spherical harmonics encoding for efficient storage and lookup
- Interpolate probe data per-pixel in the deferred shader

**Step 5.3 — (Optional) DXR Path Tracing**
- If D3D12 interop is implemented, replace probes/RSM with hardware ray-traced GI
- Multi-bounce path tracing with denoising
- This is the Tier 3 endgame from the approach analysis

---

## 6. Technical Details: Geometry Capture

### How GBuffer Draws Are Identified

During `HookedDrawIndexed`, check the currently bound render targets:

```
If OMGetRenderTargets returns 3+ RTVs:
  RT0 = B8G8R8A8_UNORM (albedo)
  RT1 = B8G8R8A8_UNORM (normals)
  RT2 = R32_FLOAT (depth)
  + DepthStencilView bound (D24_UNORM_S8_UINT)
Then: this is a GBuffer geometry draw
```

This matches the GBuffer layout documented in `kenshi_rendering_pipeline.md`.

### What State to Capture Per Draw Call

```
struct CapturedDraw {
    // Geometry
    ID3D11Buffer*       vertexBuffer;      // AddRef'd
    UINT                vbStride;
    UINT                vbOffset;
    ID3D11Buffer*       indexBuffer;        // AddRef'd
    DXGI_FORMAT         indexFormat;
    UINT                indexCount;
    UINT                startIndex;
    INT                 baseVertex;

    // Shaders
    ID3D11VertexShader* vertexShader;       // AddRef'd
    ID3D11InputLayout*  inputLayout;        // AddRef'd

    // VS Constant Buffers (need the WVP matrix)
    ID3D11Buffer*       vsConstantBuffers[4]; // AddRef'd, typically only 1-2 used
};
```

### Finding the View-Projection Matrix

OGRE's D3D11 render system typically passes auto-parameters in constant buffers:

1. **World matrix** — per-object transform (position, rotation, scale)
2. **View-Projection matrix** — camera transform (same for all objects in a pass)
3. **World-View-Projection matrix** — combined (most common in OGRE)

Strategy to find it:
- During survey, read back the VS constant buffer for multiple draw calls
- The VP matrix will be identical across all draws in the same frame (same camera)
- The World matrix will differ per draw (different objects)
- WVP = World * VP, so if we find two draws where WVP differs but a 4x4 sub-block is consistent, that's likely VP or its components

Once identified, to render from the light's perspective:
- Compute `lightVP = lightView * lightProjection`
- For each replayed draw, compute `lightWVP = World * lightVP`
- Write `lightWVP` into the constant buffer at the same offset the VS expects

### Replaying Draws

```
For each cascade (0..N):
    Bind cascade shadow map (depth-only RTV)
    Set viewport to cascade resolution
    Set depth bias state (slope-scaled bias for shadow acne prevention)

    For each captured draw:
        Bind original VB, IB, input layout
        Bind original VS
        Bind modified CB with light-space WVP
        Set null PS (depth-only rendering)
        DrawIndexed(indexCount, startIndex, baseVertex)
```

### Performance Considerations

- **Geometry cost doubles** — every mesh is rendered twice (camera + light)
- **Shadow map cost scales with cascades** — 3 cascades = 3x geometry pass
- **Mitigation:** frustum cull per cascade (only render geometry within cascade frustum)
- **Mitigation:** LOD-based cascade rendering (lower detail in far cascades)
- **Mitigation:** Render shadow maps at lower resolution (2048/1024/512 per cascade)
- **Typical budget:** 2-4ms for 3 cascades at 2048x2048 (geometry-dependent)

---

## 7. Framework Changes Required

### DrawIndexed Hook Activation

`HookedDrawIndexed` in `D3D11Hook.cpp` needs to:
1. Detect GBuffer-pass draws by checking bound render targets
2. Capture draw state into a per-frame list
3. After the GBuffer pass ends (detected by lighting fullscreen draw), trigger geometry replay

### New Framework Components

| Component | Purpose |
|-----------|---------|
| `GeometryCapture.h/cpp` | Per-frame draw call capture, state management, COM ref tracking |
| `ShadowRenderer.h/cpp` | CSM cascade setup, geometry replay, shadow map management |
| `LightExtractor.h/cpp` | Extract light data from game constant buffers |

### ResourceRegistry Extensions

```
New resource names:
  "shadow_csm_0"  — cascade 0 depth SRV
  "shadow_csm_1"  — cascade 1 depth SRV
  "shadow_csm_2"  — cascade 2 depth SRV
  "shadow_matrix_0" — cascade 0 VP matrix (accessible to effects)
```

### DustHostAPI Extensions

```c
// Geometry access (API v4)
uint32_t (*GetCapturedDrawCount)(void);
const CapturedDraw* (*GetCapturedDraw)(uint32_t index);

// Shadow access
ID3D11ShaderResourceView* (*GetShadowCascade)(uint32_t cascadeIndex);
const float* (*GetShadowMatrix)(uint32_t cascadeIndex); // 4x4 float
uint32_t (*GetShadowCascadeCount)(void);
```

### Deferred Shader Patch Extensions

New injection in `PatchDeferredShader`:
- Declare CSM samplers (register slots)
- Add CSM sampling function (cascade selection + PCF)
- Replace `RTWShadow` / `DustRTWShadow` call with CSM sample

---

## 8. What Existing Infrastructure Is Reused

| Existing System | Reused For |
|-----------------|------------|
| `HookedDrawIndexed` hook | Geometry capture entry point |
| `PipelineDetector` pattern | GBuffer-pass detection (same state-inspection approach) |
| `CaptureLightingResources` pattern | Capturing VB/IB/VS/CB references |
| RTGI A-trous spatial denoiser | Shadow spatial filtering (if needed) |
| RTGI A-trous denoise | Spatial shadow denoising (if soft shadows need it) |
| RTGI camera data extraction | Light data extraction (same CB readback technique) |
| `D3DCompile` shader patching | CSM integration into deferred shader |
| `DrawFullscreenTriangle` | Shadow composite pass |
| Compute shader dispatch | Shadow filtering, light culling |
| State save/restore | Clean state management during geometry replay |
| Resource creation helpers | Shadow map texture creation |
| Settings GUI + INI system | Shadow quality settings |

---

## 9. Priority Order

1. **DrawIndexed geometry survey** — understand what the game sends us
2. **Geometry capture system** — build the per-frame draw call list
3. **Custom CSM** — proper shadows, the #1 visual issue
4. **SMAA** — subpixel morphological anti-aliasing (spatial only, replaces FXAA)
5. **Volumetric lighting** — atmospheric depth for Kenshi's desert setting
6. **SSR** — screen-space reflections
7. **PBR deferred upgrade** — better material response
8. **Multi-light shadows** — point/spot light shadow maps
9. **Material classification** — per-object shading (SSS, translucency)
10. **Full-scene GI** — RSM or light probes from geometry access
11. **(Optional) D3D12 interop + DXR** — hardware ray tracing endgame

Items 4-7 can be developed in parallel with item 3, as they use the existing post-processing architecture. Items 8-11 depend on item 3 being complete.
