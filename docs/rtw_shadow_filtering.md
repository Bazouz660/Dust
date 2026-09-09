# RTWSM receiver bias and directional PCSS

The deferred shader now corrects the receiving surface's depth separately for each filter tap. It fits the geometric plane from shadow-position derivatives and includes the actual sampled texel center. This avoids treating the sloped receiver as its own blocker without increasing the global depth bias. Singular receiver planes fall back to the existing bias.

PCSS uses blocker-to-receiver separation in world units. Kenshi's `rtwshadows.hlsl` transforms depth affinely, whereas the point-light formula in the [original PCSS paper](https://developer.download.nvidia.com/shaderlibrary/docs/shadow_PCSS.pdf) uses distances from the light. Dividing by RTWSM's absolute normalized blocker depth made softness depend on the shadow camera's depth origin. The new calculation removes the projection's depth scale, then converts the directional-light penumbra to unwarped shadow UVs.

Each tap follows the warp independently. Taps in the center's linear warp segment reuse its lookup. Blocker searches include the center and three rings of four samples; an empty partial search no longer skips the remaining samples. The old normal-dependent radius shrink is removed.

`FilterRadius` retains its texel-based antialiasing footprint. `LightSize` now maps to `tan(angular radius) * 100`, independently of atlas resolution. Existing settings files are preserved, but the same light-size setting can produce different softness because its previous depth-dependent behavior was incorrect.

This adds shader work, especially in fully lit areas where the old three-sample shortcut returned early. It adds no rendering passes or GPU readbacks. The rings remain a finite sampling approximation: thin or unavailable blockers can still be missed. Actual game performance and visuals require an in-game check.

Validation:

- `python tests/run_native_tests.py RTWShadowTests` runs the production injected shader on WARP. It covers sloped receivers, nearby blockers, atlas resolution, affine and nonlinear warps, depth-origin/scale changes, and contact hardening.
- From `tests`, run `build/RTWShadowTests/RTWShadowTests.exe <Kenshi>/data/materials` to compile the installed game's RTW, CSM, and no-shadow variants, with and without the workshop cliff-fix marker. It also verifies the shadow atlas remains in the host's expected resource slot.
- The shader fingerprint includes `effects/shadows/*.h`, so changing the embedded shader invalidates the game's compiled shader cache on the next startup.
