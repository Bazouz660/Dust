## v1.0.0

**Ambient occlusion and GI**
- SSAO is rebuilt on Intel XeGTAO: the radius is in world units, depth is read as the ray distance the game actually stores, and horizons are tested against smooth geometry. No more darkening at the screen edges or halos following texture detail
- SSIL and RTGI rebuild positions from the same ray-distance depth, and SSIL tests its hemisphere against the smooth normal
- Fixed AO, RTGI and shadows going very dark or inverted until a screenshot, an overlay or a VSync change "fixed" them (#21, #24): the camera readback froze whenever the GPU ran behind the CPU, most often at high resolutions
- DOF autofocus no longer stalls the CPU
- Lighting no longer turns pitch black (shadowed areas and characters black, point lights off) when the SSAO effect cannot start or Dust never hooks the renderer: the patched game shaders fall back to the game's own lighting
- SSAO settings whose meaning changed have new names, so presets saved with v0.7.6 load the new defaults for them instead of being reported as outdated
- The shipped presets lower the auto-exposure floor (Min Auto EV -1.006 to -1.2): daylight scenes get back the overall brightness of v0.7.6, whose SSAO darkened open flat ground by mistake. The new SSAO darkens around objects as much as before and leaves flat ground alone
- New SSAO setting Denoise (smoothing passes, default 2): a wide Radius spreads the samples thinner and needs more passes to stay free of grain

**Temporal AA and upscalers**
- Film grain, vignette, chromatic aberration and letterbox run after DLAA/FSR, so temporal AA no longer erases the grain (#19)
- An upscaler that can't run (AMD GPU on DLAA, a GPU hidden behind DXVK or another D3D11 wrapper, a missing backend) no longer applies its mip bias or motion vectors, and the menu says why
- FSR2 is actually included in release builds
- The menu and the Workshop description say what this is: anti-aliasing at native resolution, not an upscaler that raises FPS

**Shadows**
- The RTWSM sun pass runs either Dust's shadow filter or the game's, no longer both on every pixel: same image, 10% cheaper with PCSS and 36% without
- RTW caster tessellation uses one segment per warp cell, never below the game's own factor: close views no longer tessellate everything at the maximum (about 6x fewer domain shader invocations)
- New option Shadows > Half Resolution (RTWSM with PCSS, off by default): the shadow term is computed at half resolution and upsampled per surface. The sun pass costs about half as much (0.61 to 0.29 ms at 1440p on an RTX 4090); shadow edges are slightly softer
- A wide RTWSM penumbra now lies outside the caster's outline and is fully dark under it. The shadow of a character standing under a larger soft shadow no longer stops dead in half-lit ground, and soft shadows read about half as wide as before for the same Light Size

**Menu**
- The menu toggle key still works when its key message never reaches the game window (#26)
- Closing the menu no longer locks the cursor into a quarter of the screen on scaled displays (4K at 200%)
- An empty effect list is explained: effects start in a loaded game, not in the main menu, and a real failure says so (#22)
- Menu text falls back to Tahoma, Arial, then Kenshi's own font, so Cyrillic renders under Proton (#23)
- NVIDIA Smooth Motion is detected, with a warning in the menu (#20)
- A warning is shown when two copies of Dust are installed (Workshop and manual)

**Effects**
- Kuwahara no longer uses a shader function that Proton's compiler lacks
- Film grain and deband no longer turn into a fixed pattern after hours of play
- Without temporal AA, film grain, vignette, chromatic aberration and letterbox run before SMAA, as in v0.7.6

**Bug reports**
- Reports record whether they were taken in a loaded game, the temporal AA status, Smooth Motion and wrapped GPUs

**Known limits**
- NVIDIA Smooth Motion / driver frame generation remains unsupported (#17, #20)

https://github.com/Bazouz660/Dust/compare/v0.7.6.2...v1.0.0
