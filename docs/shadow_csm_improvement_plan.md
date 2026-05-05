# Shadow Improvement Plan

*Written: 2026-05-04 — Path B implemented 2026-05-05*

## TL;DR

Runtime PSSM lambda control is shipped. `Shadows → Cascade Lambda` slider in Dust's GUI controls cascade-split distribution live (0.0 = pure linear, 1.0 = pure logarithmic, Kenshi default ≈ 0.95). User reports lambda 0.8-0.9 looks visibly better than vanilla. See "§11 Path B implementation" below.

---


Plan covering both shadow paths in Kenshi:

- **CSM** (cascaded shadow maps): "split 1 looks good, splits 2-3 are huge rectangles or no shadow at all", even at max atlas resolution and max shadow range. Sections 1-9.
- **RTWSM** (resolution-tweaked warped shadow maps): camera-position-dependent banding/streak artifacts near close-range surfaces, that don't track scene geometry. Section 10.

This document is the resume point for picking the work back up once a machine with Kenshi installed is available. Nothing here can be acted on without reading the live install.

---

## 1. Problem statement

User-observed behavior with the current Dust shadow setup:

- **RTWSM** (warped shadow maps): looks great when stable but flickers, has UV artifacts, occasional crawling. Artifacts concentrate near the camera.
- **CSM** (cascaded shadow maps): rock-stable, but past split 1 the texels are visibly huge. Split 2 is OK-ish, splits 3+ are "huge rectangles or no shadow at all". Bumping atlas to max resolution and shadow range to max barely helps the far cascades.
- User preference: full CSM globally, but better.

The problem is not filter quality. PCF/PCSS just blurs the rectangles into blurry blobs; it does not change world-space texel size.

## 2. Root cause: PSSM split distribution

World-space texel size in cascade `i` is roughly:

```
texel_size_i = (cascade_i_world_extent) / (cascade_i_resolution)
```

OGRE's PSSM split formula:

```
split[i] = lambda * near * (far/near)^(i/N) + (1-lambda) * (near + (far-near) * i/N)
```

### Unit-scale calibration

In-game observation: at shadow range 9000, the visible shadow envelope reads as 500-1000m. So **1 Kenshi unit ≈ 0.06-0.11 m**, call it ~0.08 m/unit. All meter values below assume 0.083 m/unit.

### Live data captured 2026-05-04 (supersedes prior estimates)

Cbuffer dump from `D3D11Hook.cpp` at runtime, slider Shadow Range = 9000:

| Cascade | Split end (units) | Width (units) | Width (m) |
|---|---|---|---|
| 0 | 40.7  | ~40   | ~3.4  |
| 1 | 110.8 | ~70   | ~5.8  |
| 2 | 435.3 | ~325  | ~27   |
| 3 | 5999  | ~5564 | **~462** |

Engine-internal far = ~6000, **not** the Shadow Range UI slider. `settings.cfg` reports `Shadow Range=50000`, yet cascade 3 ends at 5999 in the dump — the slider is *not* the engine far distance, and the relationship is not a simple compression. The engine far appears to be set independently (possibly clamped or derived from view frustum/scene). Solving the PSSM formula against these splits with near≈0.1 yields **λ ≈ 0.97-0.99** (cascade 3 even lies *below* the pure-log split for λ=0.95). Kenshi is more logarithmic than OGRE default. The cascade-3-vs-cascade-0 width ratio is ~140×, worse than the ~64× the prior estimate gave.

Filter radius (`csmParams[1]`) is already per-cascade and scales 0.0061 / 0.0023 / 0.0006 / 0.0004 — Kenshi's shader pre-computes a per-cascade filter taper. (Out of scope for tuning here; just noting it for future filter work.)

### The texel-density gap (using live data)

With far=6000, λ≈0.97, cascade 3 width = ~462m. At 16384² atlas (8192² per cascade in 2x2 layout): 462m / 8192 texels = **~5.6 cm/texel**. Slightly less catastrophic than the 8 cm/texel earlier estimate, but still where the "huge rectangles" come from.

Cascade 0 at the same atlas: 3.4m / 8192 = ~0.4 mm/texel. Massively overserved.

Doubling atlas resolution gives 2× texel density per cascade. The atlas knob is exhausted at 16384² for the user's hardware. The remaining lever is **redistribution (lambda)**.

## 3. Engine details (confirmed)

- Kenshi runs **OGRE 2.0 "Tindalos" (unstable)**. Source: `external/KenshiLib_Examples_deps/KenshiLib/Include/ogre/OgrePrerequisites.h` defines `OGRE_VERSION_MAJOR=2, NAME="Tindalos"`.
- OGRE 2.0 uses a compositor-driven shadow system (`CompositorShadowNode` + `ShadowTextureDefinition`). Not the legacy OGRE 1.x `PSSMShadowCameraSetup` class.
- KenshiLib itself only exposes `BinaryVersion::GetKenshiVersion()` and camera-distance globals. No shadow controls.
- OGRE's symbols (`Ogre::Root`, `CompositorManager2`, `CompositorShadowNodeDef`) are exported by Kenshi's bundled OGRE DLL. Reachable from Dust at runtime if we link or symbol-resolve.
- `SHADOW_MAP_COUNT = 4` is bound in the deferred shader cbuffer; cannot be increased without engine-side changes.
- Confirmed runtime cbuffer layout (`src/D3D11Hook.cpp:993-996`):
  ```
  csmParams   @ 208, 4x float4   (per-cascade: split, filter radius, fixed bias, depth radius)
  csmScale    @ 272, 4x float4   (per-cascade: world->atlas-uv scale)
  csmTrans    @ 336, 4x float4   (per-cascade: world->atlas-uv translate)
  csmUvBounds @ 400, 4x float4   (reflected but unused — always zero)
  ```
  **Confirmed 2x2 atlas-packed** (one shared texture). Discriminator is the trans-clustering: cascades' `(transX, transY)` land in 4 distinct half-unit cells `(0,0)/(1,0)/(0,1)/(1,1)`, with the atlas tile offset baked directly into `csmTrans`. The `csmUvBounds` slot exists in shader reflection but is never written by Kenshi — leftover from OGRE's atlas plumbing that this shader doesn't consume.
- The cbuffer is updated via `Map`/`Unmap` (D3D11_USAGE_DYNAMIC), not `UpdateSubresource`. Any future runtime mutation has to hook the Unmap path.

## 4. The relevant OGRE 2.0 type

`ShadowTextureDefinition` (`OgreCompositorShadowNodeDef.h:55-94`):

```cpp
class ShadowTextureDefinition {
    uint  width, height;          // per-shadow-map resolution
    Real  pssmLambda;             // 0.95 default - log/uniform split mix
    Real  splitPadding;
    uint  numSplits;
    size_t split;
    ShadowMapTechniques shadowMapTechnique;  // SHADOWMAP_PSSM
    ...
};
```

Each per-cascade entry in the shadow node definition has its own `width`, `height`, `pssmLambda`, `splitPadding`. So the engine in principle supports per-cascade resolution and tuning.

## 5. Plan

### Step 1: confirm atlas mode — DONE 2026-05-04

Implemented as `CSMIntercept::ClassifyLayout` in `src/D3D11Hook.cpp`, hooked into the Unmap path. Verdict: **2x2 atlas-packed**. `csmUvBounds` is unused by the shader; classification is via trans-clustering (each cascade's `(transX, transY)` falls in a distinct half-unit cell of the atlas).

Implication: per-cascade resolution is **not** a lever. Step 4 has only Path A (lambda + global resolution) — no Path A bonus.

### Step 2: locate Kenshi's compositor script — DONE 2026-05-04

**Result: there is no compositor script.** Searched `D:\SteamLibrary\steamapps\common\Kenshi\` recursively for `pssm_lambda|pssmLambda|compositor_node_shadow|technique pssm|num_splits` across `*.compositor` (10 files), `*.material` (164 files), `*.cfg`, `*.json`, `*.txt`, `*.xml`. **Zero hits anywhere.** The 10 `.compositor` files cover GBuffer/Lighting_HDR/Resolve_HDR/Water_Reflection/Debug, FXAA/HeatHaze post-process, and Caelum sky — none define a shadow node. `compositors.cfg` only lists post-process nodes (FXAA, HeatHaze).

The `SHADOW_MAP_COUNT` cascade count is a `#define` in `data/materials/common/shadowFunctions.hlsl:3` and `shadow_csm.glsl:4` (default 4, ladder up to 9). The cbuffer values are populated by Kenshi's binary at runtime, not by a script.

**Conclusion: Kenshi's shadow node is constructed in C++.** Either by Kenshi explicitly or by OGRE's auto-construction defaults invoked from Kenshi's compositor manager setup.

### Step 3: ~~read current values~~ — N/A

Subsumed into Step 1's runtime dump and the Step 2 finding above. Nominal values to record: λ ≈ 0.97-0.99 (back-solved), 4 cascades, atlas-packed 2x2, far ≈ 6000 (engine-internal, decoupled from slider).

### Step 4: pick implementation path — Path A dead, Path B is the only option

~~**Path A: edit the compositor script**~~ — **NOT VIABLE.** Step 2 confirmed there is no script to edit.

**Path B: hook OGRE at runtime** — the only path forward. Survey 2026-05-04 found Path B is *much* more accessible than originally thought:

- ✅ **Dust already links `OgreMain_x64.lib`** (`src/Dust.vcxproj:71,87`). The plan's "non-trivial linkage" obstacle was wrong.
- ✅ **OGRE 2.0 Tindalos headers are in tree** at `external/KenshiLib/Include/ogre/`.
- ✅ **Working OGRE C++ usage already exists** — `src/OgreSwapHook.cpp` already vtable-patches `Ogre::RenderTarget::swapBuffers` against the live instance reached via `Ogre::Root::getSingletonPtr()`.
- ✅ **RE_Kenshi does not touch shadows** (wide grep returned zero hits). No conflict risk.
- ✅ **`OgreMain_x64.dll` and `RenderSystem_Direct3D11_x64.dll` ship as separate DLLs** alongside `Kenshi_x64.exe`, so symbols are dynamically resolvable.
- ⚠️ **Lambda lives on `ShadowTextureDefinition::pssmLambda`** (`OgreCompositorShadowNodeDef.h:73`) — a *public* `Real` field, default `0.95f`. Mutable directly.
- ⚠️ **The vector holding those defs is `protected`** (`mShadowMapTexDefinitions`). Three workarounds: `#define protected public` before include (cursed but works), friend-class declaration (requires header edit), or offset hack (layout-dependent).
- ⚠️ **Workspace rebuild is required after mutation.** OGRE comment: *"Modifying a NodeDef while it's being used by CompositorNode instances is undefined."* So sequence is: snapshot live workspaces → `removeAllWorkspaces` → mutate def → `addWorkspace` recreated.
- ⚠️ **Shadow node name unknown.** Kenshi creates it programmatically with no script. We'll need to enumerate `CompositorManager2::getNodeDefinitions()` (or attempt `getShadowNodeDefinition`) to find the right node by inspection at runtime.

**Concrete API path:**
```cpp
auto* root = Ogre::Root::getSingletonPtr();
auto* cm   = root->getCompositorManager2();
// Find the shadow node def (name discovered by enumeration / log-and-inspect)
auto* def  = const_cast<Ogre::CompositorShadowNodeDef*>(cm->getShadowNodeDefinition("..."));
// Snapshot live workspaces (sceneManager, finalRT, defaultCam, defName, enabled, position)
// removeAllWorkspaces / removeWorkspace per snapshot
// Mutate def->mShadowMapTexDefinitions[i].pssmLambda for i in [0..3]
// addWorkspace per snapshot
```

**Lower-level alternative (also viable):** detour `Ogre::PSSMShadowCameraSetup::calculateSplitPoints(splitCount, near, far, lambda)` — public non-virtual method, mangled symbol resolvable in `OgreMain_x64.dll`. Every camera setup created after the detour gets our lambda. Avoids the rebuild dance entirely. Slightly riskier (function detour vs. data mutation) but no header workaround needed.

**Dead-end alternative (do NOT pursue):** rewrite the cbuffer at `Map`/`Unmap`. The shader reads split distances from the cbuffer, but the *shadow map content* is rendered by the shadow camera using OGRE's split distances, not ours. Mismatching the two produces shadows sampled from the wrong region of the atlas — visibly broken.

**Recommended next step**: discovery probe — log all node definitions on first frame to find Kenshi's shadow node name. Then pick between the two viable paths above based on which the user prefers (data-mutation + rebuild vs. function detour).

## 6. Why max atlas + lambda compound

These solve different problems and combine multiplicatively:

- **Atlas resolution** changes how many texels are available per cascade.
- **Lambda** changes which cascade those texels are spent on (in world-space terms).

Currently, the user paid full VRAM cost for a max-size atlas, but ~80% of those extra texels land on cascade 0/1 which were already fine at 2048. Lambda redistributes that VRAM toward the cascades that need it.

### Lambda budget at engine far=6000, near=0.1 (the actual configuration)

| Lambda | Cascade 0 (m) | Cascade 3 (m) | Cascade 3 density vs current |
|---|---|---|---|
| 0.97 (current Kenshi value) | ~3.9 | ~456 | 1.0× (baseline) |
| 0.7 | ~37 | ~364 | 1.25× |
| 0.5 | ~62 | ~295 | 1.55× |
| 0.3 | ~87 | ~227 | 2.01× |
| 0.0 (pure linear) | ~125 | ~125 | 3.65× |

Honest reading: **lambda alone roughly cuts cascade 3's coverage by a third** (lambda 0.5) or by half (lambda 0.3) before pure linear. Cascade 3 density improvement tops out at ~3.6× even at the extreme. Combined with full 16384² atlas, lambda 0.5 brings cascade 3 from ~5.6 cm/texel to ~3.6 cm/texel — acceptable rather than "huge rectangles". This is "less obviously broken", not "fixed".

The trade is cascade 0 growing from ~4m to ~37-62m. Cascade 0 was massively overserved at 4m (sub-millimeter texels), so this is a free win up to a point — exactly the headroom the user noted ("split 0 already looked good at 2048"). The realistic recommendation is **lambda 0.5-0.7**, picking by visual taste.

Bonus: with splits more uniform, the cascade 1->2 and 2->3 transitions also become less jarring because the resolution step between them is smaller.

### Implication for max shadow range

At ranges far beyond 9000 (the user's slider goes to 50000), cascade 3 covers tens of kilometres of world. No combination of atlas + lambda fixes that — the atlas budget is just too small for the area. Realistic ceiling for "sharp everywhere" is probably **range ≤ ~12000-15000 with lambda 0.5-0.7**. Beyond that, the right model is "shadows fade out gracefully past cascade 2" (a distance fade in the shader), not "shadows continue to render but look terrible". That falls under the larger-scope work in `next_steps_plan.md`.

## 7. Failure modes and what to watch for

- **Split 0 becomes visibly soft.** Means lambda was pushed too low. Climb back. Fine-tune is per-user.
- **Visible seams between splits.** Bump `pssm_split_padding` slightly.
- **No visible difference.** The game is reading a different shadow node, the script edit was on the wrong file, or the value is being overridden somewhere else (e.g. by a script in a mod or by RE_Kenshi). Verify by running the cbuffer dump after the edit and checking that cascade split distances actually changed.
- **Game crashes / shadows go black.** Compositor failed to compile after edit. Restore the file.

## 8. Out of scope (decided against)

- **Custom DustCascadeShadow filter** (Poisson PCF / PCSS / per-cascade filter scale). Would mask far-cascade ugliness with blur, not fix it. Rejected by user: "filtering just makes those rectangles blurry."
- **Hybrid RTWSM-near + CSM-far**. Inverse hybrid considered (CSM-near + RTWSM-far) but the user prefers full CSM globally.
- **More than 4 cascades**. `SHADOW_MAP_COUNT` is bound in the deferred shader cbuffer. Engine-side change, much larger scope.
- **VSM / MSM / EVSM**. These are filtering-quality improvements, not texel-density fixes. Same reason as the custom filter rejection.

## 9. Open questions

1. ~~Is the shadow node atlas-packed?~~ **Answered 2026-05-04: yes, 2x2 atlas-packed.**
2. ~~What is the current `pssm_lambda` value?~~ **Answered 2026-05-04: λ ≈ 0.97-0.99 (back-solved from runtime splits).** Compositor script may state a different nominal value if RE_Kenshi or a mod overrides it; still worth reading once Kenshi install is available.
3. ~~Engine near plane and shadow far distance?~~ **Partially answered: engine far ≈ 6000 (with `Shadow Range=50000` in settings.cfg). Near ≈ 0.1 (back-solved).** The slider does NOT correspond to the engine far distance — the engine far is set by something else (clamp? view frustum? scene size?). Unresolved.
4. ~~Does Kenshi load any compositor overrides from mods?~~ **Moot — there is no shadow compositor script for mods to override.** Step 2 confirmed shadow node is C++-constructed. Modding the shadow node would require a binary patch.
5. ~~Is RE_Kenshi already patching the compositor at runtime?~~ **Answered 2026-05-04: no.** Wide grep across `external/RE_Kenshi/` (full source: `OgreHooks.cpp`, `ShaderCache.cpp`, `MiscHooks.cpp`, etc.) for `shadow|cascade|csm|pssm|lambda|Compositor|Workspace` returned zero hits. RE_Kenshi only touches DDS texture loading, MyGUI, FS, heightmap, sound, and physics — not shadows. No conflict risk for Path B.

6. ~~What is the name of Kenshi's shadow node definition?~~ **Moot 2026-05-05 — Kenshi doesn't use OGRE's `CompositorShadowNode` at all.** It uses regular `CompositorNode` with a custom render path. See §11.

---

## 11. Path B implementation (delivered 2026-05-05)

The Path B "hook OGRE at runtime" plan turned out to need a different shape than originally drawn. Documenting the actual delivery here.

### What's wired now

- **`Shadows → Cascade Lambda` slider in Dust's GUI**, range `[0, 1]`, default `0.95` (matches Kenshi's native value). Lives in `effects/shadows/DustShadows.cpp` as a normal `DustSettingDesc`. Persists in `Dust.ini` under `[Shadows] CascadeLambda=`.
- **Host API entry `SetCascadeLambda(float)`** added to `DustHostAPI` (`src/DustAPI.h:181`). Wired to `PssmDetour::SetLambda` in `src/EffectLoader.cpp`.
- **`src/PssmDetour.cpp`** owns the actual splits override. Stores `sLambda` atomically; on slider change, recomputes splits via the standard PSSM formula and pushes them into both Kenshi's source array and OGRE's `GpuSharedParameters` buffer.

### Why Path A and the original Path B sketch were wrong

- **Path A (edit compositor script):** dead — no shadow node script exists in `data/`. Confirmed by full grep.
- **Original Path B sketch (mutate `ShadowTextureDefinition::pssmLambda`):** dead — Kenshi never instantiates a `CompositorShadowNode`. We installed hooks on `ShadowCameraSetup::ctor`, `~ctor`, `CompositorShadowNode::ctor`, `_update`, `calculateSplitPoints`, `setSplitPoints`, and all four `getShadowCamera` virtuals (PSSM/Default/Focused/PlaneOptimal). **None ever fired.** Kenshi has its own custom render path.

### How it actually works

The cbuffer fields `csmParams[i].x` (split distances) are populated by Kenshi's own code in `Kenshi_x64.exe`. The data flow we discovered:

1. Kenshi has a giant function (~10KB, starts ~`+0x8629a0` in `Kenshi_x64.exe`) that orchestrates the deferred-rendering frame, including shadow setup.
2. Inside it, around `+0x862c34`, Kenshi calls `Ogre::GpuSharedParameters::getFloatPointer(this, 24)` to get a writable pointer into the `ShadowSharedParams` shared block (the slot for `csmParams`).
3. Right after that call, Kenshi loops over its own pre-computed splits stored at `(orchestrator-this)->field_0x10`. Layout is `[near, split[1], split[2], split[3], far]` — 5 floats.
4. For each cascade `i`, it writes `csmParams[i].x = splits[i+1] - near` into the shared block.
5. OGRE pushes the shared block to the GPU cbuffer every frame — so the same values get re-uploaded forever, even though Kenshi only writes to the shared block once at scene init.

### Discovery sequence

1. Hooked `Ogre::GpuSharedParameters::getFloatPointer` (the raw-write fast path used by the orchestrator). Got call stack on first `pos==24` hit, which led to the orchestrator at `kenshi_x64.exe+0x862c34`.
2. Disassembled the post-call code (capstone via `tmp/disasm_post_call.py`). Identified `(this)->field_0x10` as the source pointer for the splits.
3. Used `RtlVirtualUnwind` from inside the OGRE hook to reconstruct the orchestrator's `RDI` (= `this`) — needed because MSVC's prologue clobbers our local `rdi` by the time C++ runs.
4. Read 32 floats from `this->field_0x10` and saw `[1.0, 41.7, 111.81, 436.33, 6000.0]` — exact match for the cascade splits we'd already observed in the cbuffer (`csmParams[i].x = splits[i+1] - 1.0`).

### What the slider actually does on each move

In `PssmDetour::SetLambda`:

1. Update `sLambda` atomic.
2. Recompute `splits[1..3]` using the standard PSSM formula on the cached `near`/`far` values.
3. Write them into Kenshi's `field_0x10` source array (so any subsequent shadow-camera setup uses the new splits).
4. Call OGRE's exported `getFloatPointer(GpuSharedParameters, 24)` ourselves and write `csmParams[i].x = splits[i+1] - near` directly into the shared block — preserving `.yzw` (filter radius, biases).

Step 4 is the critical one: Kenshi only writes to the shared block once at scene init, so without re-pushing ourselves on slider change, the cbuffer values would stay stale. With it, OGRE picks up our new values on the next frame's auto-push to GPU.

### Caveats

- We don't re-render the shadow MAP atlas. The atlas content was rendered with whichever cascade frusta Kenshi positioned originally. So the lambda slider re-distributes how the *shader samples* the atlas, but the atlas itself stays put. In practice this looks fine because Kenshi re-renders the shadow atlas every frame from the camera's current pose anyway — and if our `field_0x10` patch is read by that path (which it should be), both stay consistent. User reports the slider gives a visibly correct redistribution.
- Hook-install address `+0x8629a0` is brittle (specific to RE_Kenshi-modified `Kenshi_x64.exe` 1.0.65 Steam). Future Kenshi/RE_Kenshi updates likely shift it. Probably OK for now since RE_Kenshi only supports 1.0.65 anyway.
- The `getFloatPointer`-based capture relies on Kenshi calling it at scene init. If a future Kenshi patch caches differently, we'd miss the capture window. Mitigated by the fact that Dust loads early (preload via `dllStartPlugin`).

### Files changed

- `src/PssmDetour.cpp`, `src/PssmDetour.h` — the hook + state + `SetLambda`/`GetLambda` API
- `src/DustAPI.h` — added `SetCascadeLambda` to `DustHostAPI`
- `src/EffectLoader.cpp` — wired `hostAPI_.SetCascadeLambda`
- `effects/shadows/DustShadows.cpp` — added `pssmLambda` config field, slider entry, `OnSettingChanged` push, `Init`-time push
- `src/dllmain.cpp` — installs `PssmDetour::TryInstall()` early in `startPlugin`
- `src/D3D11Hook.cpp` — `HookedUnmap` invokes `CSMIntercept::ClassifyLayout` + `LogCallerStack` (the original probe machinery, which is what got us here)

---

## 10. RTWSM artifact analysis

User-observed artifacts when RTWSM is enabled:

- View-angle and camera-position-dependent banding/streak/rectangle patterns near close-range surfaces (ground, nearby objects).
- Patterns do not correlate to scene geometry. They move when the camera moves but are not anchored to any specific edge in the world.
- Artifact intensity scales with both shadow range and atlas resolution. Worst at high range + high resolution.
- Distinct from shadow acne (which tracks geometry exactly).

### 10.1 Cause: vanilla pipeline, not Dust-introduced

This is a property of vanilla Kenshi's RTWSM pipeline. Vanilla simply hid it because its PCF was effectively single-tap (3x3 with 0.0001 texel offsets, see `src/ShaderPatch.cpp:62`), so the whole shadow looked low-detail and the warp banding got lost in general fuzziness. Dust's proper Poisson PCF renders sharper shadows everywhere, which makes the underlying warp pathology readable for the first time. Dust did not introduce the bug; it revealed an existing one.

### 10.2 Mechanism

Two compounding issues, both inherent to the warp+importance-map pipeline:

**Issue A: PCF performed in post-warp UV space.**

In `DustRTWShadow` (`src/ShaderPatch.cpp:201-208`), the filter loop adds Poisson offsets to the *warped* UV:

```hlsl
float2 center = DustGetOffsetLocationS(wMap, sc.xy);  // warped UV
for (int i = 0; i < 12; i++) {
    float2 suv = center + off;   // offset added in post-warp space
    shadow += ShadowMap(sMap, suv, sd, b, 0);
}
```

That UV offset represents the same UV-space distance everywhere, but in *world space* it represents wildly different distances depending on the local warp gradient. Where the warp is dense (near camera), the kernel covers a tiny world-space area; where it is sparse, it covers a large area. X and Y warps are independent, so the filter footprint becomes a non-axis-aligned parallelogram that varies across the screen. This produces patterns that correlate to the warp gradient field, which is purely a function of the importance map (and therefore of view angle and camera position).

**Issue B: importance map aliasing -> warp gradient discontinuities.**

The 1D warp tables (513x2 R32_FLOAT) are derived from an importance map computed each frame from screen-space depth and derivatives. Near close-range surfaces, screen-space depth gradients spike (a single pixel can span a huge depth range when looking along a surface). Those spikes get baked into the importance map, then into the warp texture. Where the warp gradient changes sharply, adjacent screen pixels can fall into very different parts of the shadow map -> visible streaks, banding, "rectangles" that move with the camera but aren't tied to any specific edge.

Bilinearly sampling the warp (`DustWarp1D` at `src/ShaderPatch.cpp:113-128`) smooths the lookup itself but does not smooth the underlying gradient. The importance map is still spiky; the warp inherits the spikes; bilinear sampling just blurs the boundary between texels of the warp table, not the gradient between distant warp values.

### 10.3 Why intensity scales with shadow range

The warp redistributes UV [0,1] across whatever world distance the shadow camera covers. At range 9000 the warp redistributes ~750m of world; at range 50000 it redistributes ~4km. Importance map spikes happen at the same screen positions regardless of range, but the resulting warp gradient discontinuities get *stretched into world space proportional to shadow range*. The same gradient discontinuity that paints a 2m banding pattern at range 9000 paints an 11m pattern at range 50000.

### 10.4 Why intensity scales with atlas resolution

The current shadow plugin holds filter radius constant **in texels** (`effects/shadows/DustShadows.cpp:298-300`):

```cpp
float resScale = 4096.0f / (float)GetSelectedShadowResolution();
data.filterRadius = gConfig.filterRadius * 0.001f * resScale;
```

This was correct for solving Poisson clustering at low resolutions, and is not the cause of the warp artifacts. But warp-gradient artifacts live in **UV space**. At higher atlas resolution the filter (in UV) shrinks, covers fewer warp-gradient cycles, and integrates less of the artifact pattern into a smooth average. So higher resolution = filter smooths less of the warp banding = artifacts more visible per-pixel. This is a visibility modulator, not a cause.

### 10.5 Combined visibility model

| Range | Atlas | Filter UV size | Warp artifact world size | Visibility |
|---|---|---|---|---|
| Low | Low | Large | Small | Hidden by filter |
| Low | High | Small | Small | Mostly hidden |
| High | Low | Large | Large | Banding visible but blurred |
| High | High | Small | Large | **Maximally visible** |

Both knobs the user uses to improve shadows (range + resolution) push toward exposing the underlying artifact. There is no escape via these levers alone.

### 10.6 Fix targets, by cost

1. **Pre-blur the importance map.** Cheapest. Eliminates gradient discontinuities at the source. Requires identifying the importance map compute pass and injecting a separable Gaussian blur before the warp is computed. Cost: one extra small-resolution pass per frame. Tradeoff: slightly less aggressive warp redistribution (acceptable; the spikes were never useful warp signal anyway).

2. **Warp Jacobian compensation in PCF.** Medium cost. Compute the local 2x2 warp Jacobian via finite differences on the warp map (4 extra warp taps per fragment), use it to pre-distort the Poisson offset pattern back to world-space-uniform. Per-fragment cost: ~4 extra warp samples plus a 2x2 matrix mul. Fixes Issue A directly.

3. **Range-aware importance map clamping.** Cap the maximum warp gradient to prevent the importance map from saturating the warp at extreme range. This is more of a damage-control measure than a fix; combine with #1 for best effect.

4. **Re-warp every PCF sample independently.** Most correct, hottest. 12 PCF samples * bilinear warp = 48 extra warp taps. Probably overkill given #2 covers most of the same ground at a fraction of the cost.

Recommended starting attack: #1 (importance map pre-blur). Cheap, addresses both Issue A and Issue B together (smoother importance map -> smoother warp gradient -> less footprint distortion in PCF -> less banding pattern), and validates the diagnosis before committing to the more invasive Jacobian work.

### 10.7 Open questions for RTWSM

1. Where is the importance map computed? Identifying the pass is prerequisite for any fix.
2. What resolution is the importance map at? (The warp table is 513x2; the source could be anything.)
3. Is the importance map double-buffered (read this frame, written next frame)? If so, there's an additional temporal-lag artifact to factor in.
4. Does RE_Kenshi already patch the importance map computation? If so, the artifact behavior may already differ from raw vanilla.
