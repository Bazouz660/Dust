# RTWSM receiver bias and directional PCSS

The deferred shader now corrects the receiving surface's depth separately for each filter tap. It fits the geometric plane from shadow-position derivatives and includes the actual sampled texel center. This avoids treating the sloped receiver as its own blocker without increasing the global depth bias. Singular receiver planes fall back to the existing bias.

PCSS uses blocker-to-receiver separation in world units. Kenshi's `rtwshadows.hlsl` transforms depth affinely, whereas the point-light formula in the [original PCSS paper](https://developer.download.nvidia.com/shaderlibrary/docs/shadow_PCSS.pdf) uses distances from the light. Dividing by RTWSM's absolute normalized blocker depth made softness depend on the shadow camera's depth origin. The new calculation removes the projection's depth scale, then converts the directional-light penumbra to unwarped shadow UVs.

Each tap follows the warp independently. Taps in the center's linear warp segment reuse its lookup. Blocker searches include the center and six geometric rings of two opposite samples; an empty partial search no longer skips the remaining samples. A sample counts as a blocker only if its light cone reaches the receiver (`separation * lightSize >= ring distance`), and the search is bounded by the reach of a blocker 500 world units away instead of by Shadow Range. With Range 16678 and Light Size 3 the old rings sat 31, 125 and 500 units from the pixel: they averaged unrelated roofs and hills into the blocker distance, which blurred contact shadows and changed with every camera move as the sparse samples hit or missed that geometry. The old normal-dependent radius shrink is removed.

`FilterRadius` retains its texel-based antialiasing footprint. `LightSize` now maps to `tan(angular radius) * 100`, independently of atlas resolution. Existing settings files are preserved, but the same light-size setting can produce different softness because its previous depth-dependent behavior was incorrect.

This adds shader work, especially in fully lit areas where the old three-sample shortcut returned early. It adds no rendering passes or GPU readbacks. The rings remain a finite sampling approximation: thin or unavailable blockers can still be missed. Actual game performance and visuals require an in-game check.

Validation:

- `python tests/run_native_tests.py RTWShadowTests` runs the production injected shader on WARP. It covers sloped receivers, nearby blockers, atlas resolution, affine and nonlinear warps, depth-origin/scale changes, and contact hardening.
- From `tests`, run `build/RTWShadowTests/RTWShadowTests.exe <Kenshi>/data/materials` to compile the installed game's RTW, CSM, and no-shadow variants, with and without the workshop cliff-fix marker. It also verifies the shadow atlas remains in the host's expected resource slot.
- The shader fingerprint includes `effects/shadows/*.h`, so changing the embedded shader invalidates the game's compiled shader cache on the next startup.

## Caster tessellation

Kenshi warps RTWSM casters per vertex and receivers per pixel, so it tessellates its standard casters (`rtwtessellator.hlsl`, 619 of the depth-pass draws in a vanilla capture). Its hull shader sizes each edge as `floor(1 + 10 * distance between the warped end points)`. An edge that crosses the high-resolution zone but ends in the collapsed zones beside it gets almost no subdivision, so a building wall is rasterised as a straight line between two misplaced end points and its shadow is cut. Zooming in shrinks the high-resolution zone to roughly 1000 units, which is when walls straddle it. The same metric gives every triangle an inside factor of at least 2.

`src/RtwTessellation.h` replaces the factors through the D3DCompile hook: `ceil(3 * warp cells crossed)` per edge from the unwarped positions, clamped to 64, and 1 for edges whose warped end points are both beyond the same map border (the warp is monotonic per axis). Only symmetric operations are used, so triangles sharing an edge agree.

Measured on the 5.0M caster triangles and the live warp map of `vanilla_rtwsm_frame11845.rdc` (2048 map): worst edge placement error 39.8 texels vanilla, 4.1 patched; edges off by more than 4 texels 0.62% vanilla, none patched; emitted triangles 15.0M vanilla, 9.4M patched. Terrain, skinned, foliage and construction casters are not tessellated by the game and are unchanged. In-game verification is pending.
