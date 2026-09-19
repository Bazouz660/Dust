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

Measured on the 5.0M caster triangles and the live warp map of `vanilla_rtwsm_frame11845.rdc` (2048 map): worst edge placement error 39.8 texels vanilla, 4.1 patched; edges off by more than 4 texels 0.62% vanilla, none patched; emitted triangles 15.0M vanilla, 9.4M patched. Terrain, skinned, foliage and construction casters are not tessellated by the game and are unchanged. In-game (2026-09-19) this did not change the artifact it was written for; see the next section.

## Caster depth at the near plane

Kenshi fits the RTWSM shadow camera to the light-space bounding box of the main camera's eight frustum corners, far plane at Shadow Range, with no margin (`Kenshi_x64+0x865640`). Casters sunward of that box would be clipped, so every caster shader clamps `z` to the near plane, and the clamped value is also what it writes to the R32F map. Those casters are therefore stored at the near plane, not where they are. The near plane passes through the frustum point nearest the sun, which is the camera itself when it looks down at the ground, so it moves with every camera move.

Two captures of the same spot show the effect. With the camera 1535 units behind the near plane nothing next to the scene was clamped, PCSS measured the building's real height and 39% of pixels sat at the 15 unit penumbra cap. With the camera on the near plane (camera depth -0.00002) 61% of the map was exactly 0, the ground was only 6 to 255 units behind the plane, PCSS measured that distance instead and no pixel reached the cap: the same shadow turned sharp.

`src/RtwCasterDepth.h` reorders the two statements in all ten RTW caster entry points through the D3DCompile hook: the true depth goes to the map, the clamp only to the rasterised `z` (in the tessellated path the clamp moves from the vertex to the domain shader). Cascade variants of the same files are untouched. Casters clamped to the same `z` no longer depth-sort among themselves, so the stored depth is that of the first one drawn; it is still a real caster depth and no longer depends on the camera.

Wide penumbrae now use the full 12-tap disk regardless of the atlas tier, which allowed only 4 taps at 12288.

## Shadows cut near the camera when zoomed in

`rtw_build` writes 513 warp knots for a 512 texel importance map, so knot `k` is the boundary between importance texels `k-1` and `k`. The knot at the end of the important region (`k == range.y`) is the region's last point and belongs at +1.0, but the game tests `my_u >= range.y` and sends it to the off-map value 1.05 with everything beyond it. The region's last cell is stretched across the map border: in captures it ran from 0.85 to 1.024 in warped UV. Past 1.0 there is no shadow map, so receivers read lit and casters are clipped.

That cell is normally blur padding. It holds visible ground when the important region touches the border of the light-space box, and the camera end of the view frustum is always a corner of that box. Zoomed in close, the visible ground sat in warp cells 508 to 512 and 8% (one capture) and 18% (the other) of the visible pixels fell off the map, as a straight band without shadows. `src/RtwWarpBuild.h` changes the test to `>`: the knot takes the ordinary branch, where the remaining weight is zero, and lands on +1.0. Re-evaluating both captured warp maps with that knot corrected leaves no visible pixel off the map. No other knot moves.

## Shadows fading out near the camera when zoomed in

With the end knot fixed, the same strip of ground stopped losing its shadows and showed the next problem: a straight line beyond which shadows turn faint and blotchy. The warp map has 513 knots and the builder places knot `i` at `u = i / 512`, but every lookup samples it as an ordinary texture, which puts knot `i` at the texel centre `(i + 0.5) / 513`. Half a texel at each end of the map lies outside the knots; there the offset is constant, so nothing is magnified. In a capture zoomed in on one character that half texel (about 13 world units at the camera end of the light-space box) covered the top 30% of the screen: magnification 1x against 70 to 90x in the neighbouring cell, so shadow texels 80 times larger, a 4-texel filter several units wide, and visibility jumping from 0.00 to 0.5 across the cell boundary for the same blocker.

`src/RtwWarpLookup.h` rewrites all eight lookups (`rtwshadows.hlsl`, `shadowcaster.hlsl`, `rtwtessellator.hlsl`) to `texcoord = (u * 512 + 0.5) / 513`, in the compiled source and, through an `ID3DInclude` wrapper, in every file that includes them. Dust's receiver (`DustWarp1D`) and the tessellation metric use the same placement. All lookups have to agree: a caster warped one way and a receiver the other would be misregistered by up to half a cell times the local magnification.

## Temporal AA: acne and checkered penumbrae

Temporal AA renders the G-buffer through a viewport shifted by a sub-pixel jitter (`D3D11Hook.cpp`), so pixel `p` holds the sample the unjittered view would have at `p - jitter`. The game's sun pass rebuilds the world position from the view ray through the centre of `p`, which puts the point off the real surface by the jitter's footprint, differently every frame. On ground seen at a grazing angle that is whole world units (2.5 in the WARP test, at a coarse 160x90), against a shadow bias of 0.00003 of the depth range: acne that flickers with the jitter. `src/DeferredJitter.h` rebuilds along `ray - jitter.x * ddx(ray) - jitter.y * ddy(ray)`; `ray` is a linear screen-space interpolant, so the derivatives are its exact per-pixel step. Only `main_fs` is rewritten.

The PCF taps are rotated by interleaved gradient noise, whose value nearly alternates between neighbouring pixels; static, that reads as a one-pixel checker in the penumbra. While a temporal AA is integrating frames the host advances the noise every frame (64-frame cycle) so the pattern averages out. Without temporal AA it stays static, because a moving pattern would boil.

Both values reach the shader through `DustFrameParams` at PS `b8`, which the host binds for the sun draw only and unbinds afterwards. Unbound it reads as zero: no correction and static noise.

## Penumbra quality without temporal AA

Measured on a full-resolution crop of a captured penumbra (59k penumbra pixels, replica of the shader): residual noise 0.103 and one-pixel checker energy 0.158. Tap count and tap rotation were only part of it. The blocker search rotated its two taps per ring per pixel, so neighbouring pixels disagreed about whether a blocker exists (3.2% of neighbours differed by more than 25% in filter radius), and the radius flipped between the texel floor and the full penumbra: hatching along the lit edge.

- The search uses six rings of four fixed directions, alternate rings turned by 45 degrees. Deterministic, so neighbours agree (0.6%).
- Rings are discrete, so a receiver inside a blocker's cone could fall between the last ring that finds it and the first short enough to pass the cone test; the penumbra then ended in a hard, jagged rim on the lit side. A hit whose reach is shorter than its ring is probed again in the same direction at that reach; still a hit means the receiver is inside the cone. Merely loosening the cone test (tried: 3x) also removed the rim, but let distant casters widen the filter over nearby contact shadows, which the plateau regression test caught.
- The PCF uses a Vogel disk, 24 taps for wide penumbrae (atlas tier otherwise), rotated per pixel by the R2 sequence, which does not alternate between neighbours the way interleaved gradient noise does. Under temporal AA the rotation also advances per frame.

Result on the same crop: residual noise 0.038, checker energy 0.050, and a penumbra that fades out on both sides. Cost: 24 search taps (was 12) for every lit pixel, plus one probe per rejected hit, and 24 PCF taps (was 12) inside wide penumbrae.

## Blocker layers

PCSS estimates one blocker distance per pixel and filters with one kernel. Where a near object's shadow lies next to, or inside, the wide penumbra of a far caster, the search finds both, the average lands between them, and the near shadow is smeared with the far one (WARP test: a low object in a far caster's penumbra came out at visibility 0.38 instead of dark).

The search now keeps every accepted blocker's separation and splits them into a near layer, within 4x of the nearest one, and a far layer. Each layer gets its own penumbra radius and its own Vogel-disk PCF that only counts blockers in its separation range; the result is the product of the two visibilities (independent occluders). With a single layer nothing changes and only one PCF runs. A tall single object spans both layers near its base, which is also right: its lower part casts the crisp contact shadow there and its upper part the soft one further out.

### Lit outline around near shadows, and where the layers split

The first layered version still left near shadows wrong in-game: a lit outline around a character's shadow where it overlapped a far caster's soft shadow. A capture replayed on the CPU showed why. Pixels that found the character in their blocker search separated the layers and gave the far layer about 0.50; their neighbours, which found only the far caster (900 units up, 15 unit kernel), gave 0.25 to 0.46 for the same far penumbra. The neighbours were the wrong ones: the wide far kernel counted the character (12 to 15 units up) as an occluder through every tap that landed on it, up to the whole kernel radius away. That is the original smear; layering had only protected the pixels that detected both layers, which therefore stood out as lit.

Every PCF tap now applies the blocker's own light cone: a blocker occludes through a tap only if the tap is within `separation * lightSize` (x2 tolerance) of the receiver, or inside the antialiasing floor. The far kernel then ignores near blockers everywhere, layered pixel or not, and needs no lower separation bound; the near kernel still leaves the far layer's blockers to the far kernel.

The layer boundary moved too. A fixed 4x ratio from the nearest blocker cut through casters whose own separations span more than that (a tall object seen from near its base). The boundary is now the middle of the widest empty stretch of `log2(separation)`, which must be at least two octaves wide; with no such gap there is one layer.

### What a single-layer shadow map cannot do, and Max Penumbra

With the layers and the cone test in place one defect remained in-game: a crisp shadow still fades out where it enters a far caster's soft edge. A capture showed the near shadows ending exactly where the ground's shadow-map texel holds the FAR caster, i.e. in the inner half of the far penumbra, under the far caster as seen from the sun. A shadow map keeps one depth per texel, the surface nearest the light, so an object standing under the far caster is not in the map there. Between 50% and 0% of the light still reaches that ground, so its crisp shadow should remain visible, but there is nothing to draw it from. The outer half of the penumbra, where the map does hold the near object, is correct. This cannot be fixed in the receiver shader; exact fixes are a second depth layer in the caster pass (another full-size target) or screen-space contact shadows. Pushing the whole penumbra outside the geometric edge was prototyped and rejected: the far shadow swallowed far more of the scene and the near shadows came out fat and hatched.

What shipped PCSS implementations do instead is keep soft edges narrow, so that zone is a thin strip. `Max Penumbra` (world units, default 2) replaces the fixed 500-unit blocker distance as the bound of both the blocker search and the penumbra. On the captured scene the partly-lit ground under the far caster was 26% of the region at Light Size 3 with a 15-unit penumbra, 13% at Light Size 1 (5 units), 5.8% with a 2-unit cap and 3.1% with 1 unit. Narrow caps are also cheaper: fewer pixels take the wide 24-tap kernel.
