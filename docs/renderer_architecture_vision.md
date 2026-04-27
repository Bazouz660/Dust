# Renderer Architecture Vision — Dust / Kenshi

*A record of the architectural thinking behind where Dust can go, beyond incremental
post-processing into a full renderer replacement.*

---

## 1. What Dust has today

Dust sits *beside* the D3D11 pipeline. It hooks individual API calls
(`DrawIndexed`, `OMSetRenderTargets`, `D3DCompile`), intercepts them, and
optionally injects its own work. The game still drives everything.

Key infrastructure already in place:

| System | What it does |
|---|---|
| `GeometryCapture` | Records every opaque GBuffer draw: VB/IB, VS, frozen CB (world matrix, bones at capture time), input layout, topology |
| `GeometryReplay` | Re-emits captured draws with any replacement VP, any GS/PS the caller binds |
| `ShaderMetadata` | D3DReflect on every VS to classify STATIC vs SKINNED, find clip+world matrix offsets |
| `D3DCompile` hook | Intercepts HLSL compilation — patches `deferred.hlsl` for AO, shadows, MSAA, specular AA |
| `DustShadowsRT` | Deep Primitive Map: captures world-space triangles per shadow texel, Möller-Trumbore ray trace, shadow mask into deferred via s13/b4 |
| `DebugViews` | Wireframe (geometry replay), world/view normals, depth, luma, roughness, metalness, HDR lighting — all at POST_TONEMAP before UI |
| `DustAPI` | Stable C plugin interface: GetSRV/RTV, ReplayGeometry, ReplayGeometryEx (with per-draw callback), GetGeometryDrawConstants, GetGeometryDrawBuffers |

---

## 2. What data we actually have

The first question for any pipeline discussion is: do we have the data?

### Geometry
Every opaque GBuffer draw is captured with full IA state. `GetGeometryDrawBuffers`
exposes the raw VB/IB handles. World-space triangle vertices are recovered in the
DPM build pass by inverting the light VP in the GS (`mul(clip, invLightVP)`).

### Transforms
- **STATIC objects**: world matrix at known CB offset, V*P extracted from SKINNED
  draws or cached. `MatMul4x4(world, VP)` reconstructs `worldViewProj`.
- **SKINNED characters**: `viewProjectionMatrix` stored directly in CB at
  `clipMatrixOffset`. Bone matrix array also in CB — layout fully readable from
  `skin.hlsl` (see §6).

### Materials
`DUST_CAPTURE_PS_RESOURCES` flag captures per-draw PS SRVs and samplers. Combined
with HLSL source (§6), all texture slots are known by name.

### Lighting
Sun direction and inverse view matrix at known offsets in the deferred PS CB0
(pattern established by `SSSRenderer::ExtractLightData`). Point light parameters
extractable from deferred point-light draw CBs once their shader is read.

### Camera
`DustCameraData` provides right/up/forward axes, camera position, and the full
row-major `inverseView[16]`. View-projection reconstructible from any SKINNED draw.

---

## 3. The ceiling of the hook approach

The current approach is reactive: the game submits draws, Dust observes and
augments. Three things remain out of reach:

1. **Geometry beyond the camera frustum.** The game CPU-culls everything not
   potentially visible. World-space techniques (path tracing, full-scene BVH,
   reflection probes off-screen) only have what the game chose to draw.

2. **Transparency and particles.** GeometryCapture deliberately ignores draws
   after the GBuffer pass (alpha-blended geometry, particles, water). They happen
   in separate forward passes that are currently invisible to Dust.

3. **Draw-call CPU overhead.** D3D11 has ~0.01–0.1 ms per draw call. At 1000
   GBuffer draws that's 1–100 ms of pure API overhead on the CPU, which is a
   real contributor to Kenshi's battle performance degradation.

---

## 4. The next level: full D3D11 proxy

### What it is

Place a custom `d3d11.dll` in the Kenshi game directory. Windows resolves DLLs
from the application directory first, so the game loads yours. Your DLL
implements the `ID3D11Device` / `ID3D11DeviceContext` COM interfaces (or the
subset Kenshi actually calls — determinable by logging a session) and forwards
to the real system D3D11 at `C:\Windows\System32\d3d11.dll`.

### What it gives you

Everything the hook approach has, plus:

- **Every resource creation** — textures at upload time with full contents,
  buffers at creation, shaders at compile time. No sampling GBuffer SRVs after
  the fact.
- **All draw types** — transparency, particles, UI, shadow passes, everything.
  Not just the opaque GBuffer window.
- **Constant buffer update stream** — every `UpdateSubresource` and `Map/Unmap`.
  Bone matrix uploads happen here; you can watch them in real time.
- **Full scene graph in CPU memory** — maintain a
  `unordered_map<ID3D11Buffer*, MeshData>` tracking every VB/IB ever created.
  Any draw call's geometry is a lookup away.

### Scope

Kenshi uses roughly 50–70 D3D11 functions. Log one session to get the exact set.
Everything else can be a stub returning `S_OK`. The one genuinely hard piece is
Vulkan-style pipeline state objects (see §5) — Vulkan requires full pipeline state
upfront where D3D11 allows piecemeal changes. Track state, create `VkPipeline`
lazily on first draw for each new state combination. Well-understood; this is the
core of what DXVK implements.

---

## 5. The Vulkan backend

### Why Vulkan specifically

| Capability | D3D11 | Vulkan (via proxy) |
|---|---|---|
| Hardware ray tracing | No (D3D11 predates DXR) | Yes — `VK_KHR_ray_tracing_pipeline` |
| Mesh shaders | No | Yes — `VK_EXT_mesh_shader` |
| Indirect multi-draw | Limited | `vkCmdDrawIndexedIndirect` — all 1000 draws in one call |
| Async compute queues | Implicit only | Explicit separate compute queue |
| Bindless resources | No | `VK_EXT_descriptor_indexing` — all textures in one array |
| Explicit sync | No (driver infers) | Precise pipeline barriers |
| Subgroup operations | No | SPIR-V subgroup intrinsics |

### Shader translation — already mostly solved

The `D3DCompile` hook intercepts HLSL compilation. The game sends HLSL source;
you also pass it to **DXC** (DirectX Shader Compiler) to produce SPIR-V. You
return the D3D11 bytecode to the game (business as usual) and keep the SPIR-V
for Vulkan. No DXBC reverse engineering needed.

```
game calls D3DCompile(hlslSource, "main_fs", "ps_4_0")
  → Dust intercepts
  → patches source (existing shadow/AO/MSAA logic)
  → ALSO: dxc.exe hlslSource → patched_shader.spv
  → returns D3D11 bytecode to game (unchanged behaviour for now)
  → uses .spv for Vulkan rendering path
```

### The draw call overhead fix

Kenshi's CPU bottleneck in large battles is partly the per-draw-call D3D11
driver overhead. With Vulkan indirect rendering:

```
// One-time (or incremental update) setup:
for (auto& draw : capturedDraws)
    indirectBuffer.append(VkDrawIndexedIndirectCommand { draw.indexCount, ... });

// Per frame — one CPU API call handles all 1000 draws:
vkCmdDrawIndexedIndirect(cmd, indirectBuffer, 0, drawCount, stride);
```

Add a compute shader pre-pass that zero-outs invisible draw commands
(GPU-driven frustum culling) and the CPU is entirely out of the geometry
submission loop. Battle scenes stop being a CPU-draw-call problem.

---

## 6. The HLSL source files

Kenshi ships its HLSL shader source files in the game directory. This is the
single most important enabler for everything in this document.

### What it eliminates

**Every item previously labelled "requires reverse engineering":**

- **Bone matrix layout** — `skin.hlsl` declares the CB explicitly: array size,
  which CB slot, how blend weights and indices are stored in vertex attributes.
  Skinned character geometry in a custom compute shader is now straightforward.

- **GBuffer encoding** — `deferred.hlsl` documents the exact YCoCg albedo
  packing, normal encoding convention, which channel carries metalness vs gloss.

- **Material logic per type** — triplanar projection formula (`terrain.hlsl`),
  wind animation displacement (`foliage.hlsl`), alpha threshold handling
  (`objects.hlsl`). Complete reference implementations.

- **Point light data format** — the deferred point-light shader exposes the exact
  CB layout: position, radius, colour, falloff curve. Required for any custom
  light culling system.

- **Vertex semantic mapping** — each VS's `#input` declarations map HLSL
  semantics to physical vertex buffer channels. No guessing which TEXCOORD carries
  blend indices for skinning.

### What it enables

**Proactive shader preparation** instead of reactive patching:

Load the HLSL files from disk at Dust startup. Apply principled modifications
(add `#include "dust_extensions.hlsl"`, inject new function calls at known
anchor points). Compile the modified versions via DXC. Return them when the
game requests compilation. The current string-anchor patching becomes unnecessary.

**Forward+ material shaders** — see §7.

**Richer ShaderMetadata** — currently classifies VSs as STATIC/SKINNED by looking
for matrix names in DXBC reflection. With HLSL source, build a full picture at
startup: texture slot map, CB layout per variable, vertex format semantics per
material category. Everything Dust needs to know about any draw call is available
before the game renders the first frame.

---

## 7. Forward+ rendering

### Why it's interesting over deferred

| Problem | Deferred | Forward+ |
|---|---|---|
| Transparent objects | Separate forward pass (hack) | Unified, same pass |
| MSAA | Requires workarounds (implemented in Dust) | Native, no workarounds |
| Varied material models | GBuffer packing limits you | Each object uses its own shading code |
| Memory (GBuffer) | 4+ large render targets | Depth pre-pass only |
| Many lights | Scales well (deferred tiled) | Scales well (Forward+ tile culling) |

The big win for **this project specifically**: Forward+ allows SSS, hair
(Kajiya-Kay/Marschner), cloth, and eye rendering as first-class citizens — each
with their own shading code — without GBuffer packing gymnastics.

### What building it looks like (with HLSL sources)

```
Depth pre-pass:
  ReplayGeometry() into depth-only DSV  ← already works (DPM does this)

Tile light culling:
  Compute shader: 16x16 tiles, depth bounds per tile, per-tile light list
  Light data: from point-light deferred draw CBs (format known from HLSL source)

Forward shading pass (per material category):
  objects_forward.hlsl:
    VS  = objects.hlsl VS verbatim (world transform, UV output)
    PS  = objects.hlsl material sampling (albedo/normal/roughness decode)
        + tile light list lookup
        + your BRDF (Disney, GGX, Kulla-Conty)
        → output to HDR RT directly

  skin_forward.hlsl:
    VS  = skin.hlsl VS verbatim (bone skinning, correct because source is known)
    PS  = skin material sampling
        + tile light list
        + your BRDF with SSS lookup table

  terrain_forward.hlsl:
    VS  = terrain.hlsl VS verbatim
    PS  = triplanar projection (exact formula from terrain.hlsl)
        + tile light list + BRDF

  (foliage, triplanar, distant_town — same pattern)
```

Material sampling code is paste from the game's PS. Lighting code is new. The
game already wrote the hard parts — you replace only the parts you want to improve.

### The hybrid approach (most practical)

Modern engines (UE5, Unity HDRP, Frostbite) use neither pure deferred nor pure
Forward+:

- **Static world** (buildings, terrain): deferred — many lights, simple materials
- **Characters, foliage, specials**: forward — varied materials, SSS, hair
- **Transparent geometry**: forward — always
- **Lighting**: tiled/clustered for both paths

This is the realistic target. Keep the game's GBuffer pass for the static world
(zero material code to write). Switch characters and foliage to a forward path
using the game's VS verbatim and a new PS with real material models.

---

## 8. The complete picture

What "full pipeline replacement" looks like with everything above in place:

```
Startup:
  Load all HLSL sources from game directory
  Build full material/CB/texture metadata
  Compile modified + forward variants via DXC → SPIR-V
  Initialize Vulkan device alongside D3D11

Per frame:
  D3D11 proxy receives game calls transparently

  GBuffer pass (opaque static):
    Game draws → our GBuffer RTVs (RTV redirection via OMSetRenderTargets hook)
    Material textures bound by game's own PS (or our forward variant)

  Depth pre-pass (forward path objects):
    Extract depth for character + foliage geometry

  Tile light culling (Vulkan compute):
    Point light positions from intercepted CB updates
    Build 16x16 tile light lists

  Forward shading (characters, foliage):
    skin_forward, objects_forward, foliage_forward — game's VS + our PS + tile lights

  Custom deferred lighting (static world):
    Read GBuffer, our BRDF, tile light list, ray-traced shadows from DPM/TLAS

  Hardware ray tracing (Vulkan RT):
    TLAS from captured geometry (near frustum)
    Shadow rays, AO, glossy reflections where applicable

  Post-process stack:
    SSS blur, volumetric froxels, bloom, tonemapping

  UI draw calls forwarded unmodified to D3D11
  Present via Vulkan swapchain (or blit back through D3D11 surface)
```

### What this solves

| Issue | Solution |
|---|---|
| Draw call CPU overhead | Vulkan indirect, GPU-driven culling |
| Shadow quality | Hardware RT (TLAS) + DPM for near |
| GI / bounce light | DDGI probes or hardware RTXGI |
| Character rendering | Forward path: SSS, hair shading per model |
| Transparency | Unified forward pass |
| MSAA | Native in forward path; no workarounds |
| BRDF accuracy | Disney/GGX/Kulla-Conty, no deferred packing limit |
| Specular on metal | Correct energy-conserving specular |

### What it does not solve

- **Asset quality**: geometry density, texture resolution, and UV quality are
  determined by Lo-Fi Games' art. Better lighting reveals rather than hides
  low-poly assets; for Kenshi's aesthetic this is largely a feature, not a bug.

- **Full-scene ray tracing**: the TLAS only contains camera-visible geometry
  (the game culls everything else). Reflections of off-screen geometry and GI
  probes placed away from the camera are limited by what the game drew.

- **Performance at all settings**: hardware RT at 4K native 60fps is not
  achievable today for complex scenes. Hybrid approaches (RT shadows + rasterized
  GI) are the practical target.

---

## 9. Priority order

Given what's already built and what unlocks the most:

1. **Read HLSL sources at startup** — build complete material/CB metadata. Feeds
   into everything else. Low risk, high information gain.

2. **Phase 4 shadow integration** — wire DPM shadow mask into deferred shader.
   Immediately visible quality win, uses existing infrastructure.

3. **PCSS + hybrid blend** (Phase 3/4 of shadow system) — soft shadows at
   penumbra distance.

4. **Deferred pass replacement** — suppress game's deferred draw, run custom
   BRDF. First real lighting quality improvement.

5. **Tiled light culling** — extract point light data from game CB updates, build
   tile lists. Prerequisite for both better deferred and Forward+.

6. **D3D11 proxy skeleton** — transparent forwarding layer. Build incrementally;
   doesn't need to be complete before becoming useful.

7. **Vulkan backend** — enables hardware RT, indirect draw, async compute. The
   largest scope item; everything before it is productive without it.

8. **Forward+ character pass** — once Vulkan backend and HLSL metadata are in
   place, skin_forward.hlsl is the first forward material shader. SSS follows
   immediately.
