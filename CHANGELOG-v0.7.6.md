## v0.7.6

**RTWSM shadows**
- Fixed shadows being cut off near the camera when zoomed in. Two defects in the game's own warp shaders, both most visible up close: the warp builder pushed the end of the important region off the shadow map (a straight band with no shadows at all), and every warp lookup read the 513 knots half a texel away from where the builder places them, which left the last strip of the map unmagnified (shadows fading out along a straight line, with blotchy terrain)
- PCSS softness no longer changes with the camera. The game writes shadow depth clamped to the shadow camera's near plane, and that plane moves with the camera, so the same shadow could be razor sharp from one angle and a wide smear from the next. Casters now store their true depth
- PCSS no longer blurs contact shadows next to tall or distant geometry: a surface only counts as a blocker if its light cone actually reaches the pixel, in the blocker search and in every filter tap
- A near object keeps its crisp shadow next to, or in the outer part of, a far caster's soft shadow: near and far blockers are filtered as separate layers with their own penumbrae. The lit outline this first produced around near shadows is fixed
- Penumbrae are smooth without temporal AA: the blocker search uses fixed directions with a refinement probe (no more hatching or hard rim on the lit side), the filter is a 24-tap Vogel disk in wide penumbrae, and the tap rotation no longer alternates between neighbouring pixels. Under temporal AA the pattern also advances per frame
- New **Max Penumbra** setting (world units, default 2): the widest a soft edge may get. A shadow map holds one surface per texel, so inside a tall caster's soft edge the shadows of things standing under it are missing; a narrow cap keeps that zone a thin strip. Raise it for softer large shadows
- **Light Size** now defaults to 1.0 (was 3.0, about six times the sun). Dust's own presets follow; your saved settings are not changed
- The game's caster tessellation is sized by warp cells crossed instead of by the distance between already-warped end points: less misplaced geometry in the shadow map, and fewer triangles than vanilla
- Caster bias is preserved when the shadow atlas is enlarged, and atlas bindings stay coherent across shadow mode switches
- Receiver slope is accounted for at each filter tap

**Temporal anti-aliasing**
- Fixed shadow acne with temporal AA on: the game rebuilt world positions from unjittered view rays while the G-buffer is rendered with sub-pixel jitter
- Fixed water borders flickering against distant terrain: every water draw now shares the G-buffer's jitter, not only the one plane inside the game's occlusion query
- Temporal reset is held until the upscaler has actually evaluated a frame; motion reprojection is computed in double precision; interop resources are retained until the GPU is proven done with them

**RTGI**
- Bounce history survives FOV changes; cameras stay on the GPU and lighting history is reprojected; motion estimates and temporal history are stabilised

**Framework**
- Automatic crash reports are captured before save loading
- Shader patches are fingerprinted and cache stamps published atomically, so a Dust update always recompiles the shaders it patches
- Shader includes resolve correctly in the in-game effect loader
- Large effect render targets are allocated on demand; GPU timings are collected without flushing
- Outline: creases are detected independently of normal length

**Known limits**
- A near object's crisp shadow is still missing in the inner part of a far caster's soft edge (the shadow map holds only the far caster there). Max Penumbra keeps that zone narrow
- NVIDIA Smooth Motion / driver frame generation remains unsupported (#17, #20)

https://github.com/Bazouz660/Dust/compare/v0.7.5.3...v0.7.6
