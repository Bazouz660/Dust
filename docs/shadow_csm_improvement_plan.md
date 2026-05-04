# Shadow Improvement Plan

*Written: 2026-05-04*

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

### Splits at default lambda=0.95, far=9000, near=1

| Cascade | Range (units) | Width (m) |
|---|---|---|
| 0 | 1 → 122 | ~10 |
| 1 | 122 → 315 | ~16 |
| 2 | 315 → 1215 | ~75 |
| 3 | 1215 → 9000 | **~645** |

This matches the user's in-game observation directly: cascade 0 reads as "extremely small" (~10m) and cascade 3 lands in the user's 500-1000m bracket. Conclusion: **Kenshi is almost certainly running OGRE-default lambda=0.95**, not something exotic. No need to invoke lambda close to 1.0; the defaults already produce what we see.

### The texel-density gap

At far=9000, lambda=0.95, the cascade 3 / cascade 0 world-extent ratio is ~64×. Each cascade gets the same atlas slot, so texels in cascade 3 are ~64× wider than in cascade 0. With a 16384² atlas (8192² per cascade in 2x2 layout), cascade 3 covering 645m at 8192 texels = **~8 cm/texel**. That is exactly where the "huge rectangles" come from.

Doubling atlas resolution gives 2× texel density per cascade. The atlas knob alone is fully exhausted at 16384² for the user's hardware. The remaining lever is **redistribution (lambda)**.

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
  csmUvBounds @ 400, 4x float4   (per-cascade: atlas tile UV bounds)
  ```
  The presence of `csmUvBounds` strongly implies all 4 cascades pack into a single shared atlas texture (OGRE `isAtlas=true` mode), but this is not yet verified.

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

### Step 1: confirm atlas mode (do this even before grabbing the script)

Add a one-shot dump of `csmUvBounds[i]` for all 4 cascades in `src/D3D11Hook.cpp` around the existing `DumpCascades` (line 1013-1030). If the bounds look like `(0, 0, 0.5, 0.5)`, `(0.5, 0, 1.0, 0.5)`, `(0, 0.5, 0.5, 1.0)`, `(0.5, 0.5, 1.0, 1.0)`, it is a 2x2 atlas. If they all read `(0, 0, 1, 1)` then each cascade has its own texture and per-cascade resolution is on the table.

This is the only thing in this plan that does not require Kenshi to be installed; it just needs the dump output.

### Step 2: locate Kenshi's compositor script

On a machine with Kenshi installed (Steam: `<library>/steamapps/common/Kenshi/`), grep the data directory:

```powershell
Get-ChildItem "<Kenshi>\data" -Recurse -Include *.compositor,*.material |
    Select-String -Pattern "pssm_lambda|num_splits|shadow_map.*pssm|compositor_node_shadow"
```

Likely candidate filenames: `Deferred.compositor`, `ShadowMaps.compositor`, anything in a `shaders/` subfolder. The relevant block is `compositor_node_shadow ... { technique pssm; ... }`.

### Step 3: read current values

Record:

- Current `pssm_lambda` (expected ~0.95)
- Current `num_splits` (expected 4)
- Current `pssm_split_padding` (expected ~1.0)
- Each `shadow_map` line: dimensions, format, `atlas <name>` clause if present
- Whether atlas mode is in use (matches step 1's dump)

### Step 4: pick implementation path

**Path A: edit the compositor script** (cheap, static)

- Change `pssm_lambda` from ~0.95 to ~0.6 as the first test. If split 0 still looks fine, push down to 0.5. If split 0 starts looking bad, climb back to 0.7.
- Leave `pssm_split_padding` alone unless a seam appears between splits.
- If step 1 confirms separate per-cascade textures, optionally bump split 2 and 3's `width`/`height` while leaving split 0 alone.
- Pros: no code, no DLL changes, immediate visual confirmation.
- Cons: not live-tunable. Every Kenshi update may overwrite. Distribution requires shipping a modified compositor file, which is friction.

**Path B: hook OGRE at runtime** (proper)

- Walk `Ogre::Root::getSingleton().getCompositorManager2()` -> shadow node def -> `ShadowTextureDefinition` entries.
- Mutate `pssmLambda` (and per-cascade `width`/`height` if per-cascade textures), then force a workspace rebuild.
- Expose as Dust shadow plugin sliders alongside the existing filter radius and resolution controls.
- Pros: live tuning, persists across updates, no game-file modification.
- Cons: Dust currently is a pure D3D11 hook with no OGRE dependency. Adding OGRE linkage is non-trivial. Compositor rebuild may be intrusive (frame hitch or breakage).

**Recommended order**: do Path A first to validate the lambda thesis with real visual feedback. If the result is good, productize as Path B. If Path A is already good enough and the user is happy editing one file once, skip B.

## 6. Why max atlas + lambda compound

These solve different problems and combine multiplicatively:

- **Atlas resolution** changes how many texels are available per cascade.
- **Lambda** changes which cascade those texels are spent on (in world-space terms).

Currently, the user paid full VRAM cost for a max-size atlas, but ~80% of those extra texels land on cascade 0/1 which were already fine at 2048. Lambda redistributes that VRAM toward the cascades that need it.

### Lambda budget at far=9000 (the realistic ceiling)

| Lambda | Cascade 0 (m) | Cascade 3 (m) | Cascade 3 density vs default |
|---|---|---|---|
| 0.95 (OGRE default) | ~10 | ~645 | 1.0× (baseline) |
| 0.7 | ~50 | ~530 | 1.2× |
| 0.5 | ~94 | ~428 | 1.5× |
| 0.3 | ~131 | ~332 | 1.94× |
| 0.0 (pure linear) | ~187 | ~187 | 3.45× |

Honest reading: **lambda alone roughly halves cascade 3's coverage** before pure linear (where all cascades become equally bad). Cascade 3 density improvement tops out at ~3.5× even at the extreme. That is real — combined with full 16384² atlas, lambda 0.5 brings cascade 3 from ~8 cm/texel to ~5 cm/texel, which is acceptable rather than "huge rectangles". But this is "less obviously broken", not "fixed".

The trade is cascade 0 growing from 10m to ~50-95m. Cascade 0 was massively overserved at 10m, so this is a free win up to a point — exactly the headroom the user noted ("split 0 already looked good at 2048"). The realistic recommendation is **lambda 0.5-0.7**, picking by visual taste.

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

## 9. Open questions to answer once Kenshi install is available

1. Is the shadow node atlas-packed (one shared texture) or does each cascade have its own texture?
2. What is the current `pssm_lambda` value?
3. What is Kenshi's near plane and shadow far distance as the engine sees them (not the settings.cfg `Shadow Range` value, which is just a UI cap)?
4. Does Kenshi load any compositor overrides from mods, or is the data file the single source of truth?
5. Is RE_Kenshi already patching the compositor at runtime? If so, our edits may be stomped or already-stomped values may be what we are seeing.

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
