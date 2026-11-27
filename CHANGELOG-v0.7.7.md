## v0.7.7

**Ambient occlusion and GI**
- SSAO is rebuilt on Intel XeGTAO: the radius is in world units, depth is read as the ray distance the game actually stores, and horizons are tested against smooth geometry. No more darkening at the screen edges or halos following texture detail, and it costs about 25% less
- SSIL and RTGI rebuild positions from the same ray-distance depth, and SSIL tests its hemisphere against the smooth normal
- Fixed AO, RTGI and shadows going very dark or inverted until a screenshot, an overlay or a VSync change "fixed" them (#21, #24): the camera readback froze whenever the GPU ran behind the CPU, most often at high resolutions
- DOF autofocus no longer stalls the CPU

**Temporal AA and upscalers**
- Film grain, vignette, chromatic aberration and letterbox run after DLAA/FSR, so temporal AA no longer erases the grain (#19)
- An upscaler that can't run (AMD GPU on DLAA, a GPU hidden behind DXVK or another D3D11 wrapper, a missing backend) no longer applies its mip bias or motion vectors, and the menu says why
- FSR2 is actually included in release builds

**Shadows**
- RTW caster tessellation uses one segment per warp cell, never below the game's own factor: close views no longer tessellate everything at the maximum (about 6x fewer domain shader invocations)

**Menu**
- The menu toggle key still works when its key message never reaches the game window (#26)
- Closing the menu no longer locks the cursor into a quarter of the screen on scaled displays (4K at 200%)
- An empty effect list is explained: effects start in a loaded game, not in the main menu, and a real failure says so (#22)
- Menu text falls back to Tahoma, Arial, then Kenshi's own font, so Cyrillic renders under Proton (#23)
- NVIDIA Smooth Motion is detected, with a warning in the menu (#20)
- A warning is shown when two copies of Dust are installed (Workshop and manual)

**Bug reports**
- Reports record whether they were taken in a loaded game, the temporal AA status, Smooth Motion and wrapped GPUs

**Known limits**
- NVIDIA Smooth Motion / driver frame generation remains unsupported (#17, #20)

https://github.com/Bazouz660/Dust/compare/v0.7.6.2...v0.7.7
