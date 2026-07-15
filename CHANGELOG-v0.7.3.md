## v0.7.3

**Improvements**
- RTGI reworked to a visibility-bitmask horizon-GI trace: ray reach is now depth-independent, rays-per-pixel are horizon slices, and sampling uses static blue noise so convergence no longer depends on temporal accumulation (less ghosting, more stable indirect light)
- RTGI: new **Shadow Contrast** control — a power curve on the directional AO at composite that deepens occluded contacts into GI shadows while leaving open surfaces untouched
- SSAO and RTGI ambient occlusion now affect **point and spot lights** (injected per-light-volume in `light_fs`), not just the sun pass — local lights get proper contact darkening
- Shadows: added atlas **resolution override** (1024–16384, or Vanilla = no override / zero overhead)
- Shadows: added **Shadow Range** override that bypasses the in-game UI's 9000 cap, applies live (cascade splits re-derive next frame), and is persisted to `settings.cfg` so it survives a restart
- Shadows: added **Cascade Lambda** override for the PSSM split distribution (0.95 default keeps Kenshi's native splits)
- CSM: new **CSM Filter Radius**, **CSM Light Size**, **CSM PCSS**, **Cascade Blending**, and **Cascade Blend Width** controls
- CSM filter-radius formula corrected (0.75 factor to match vanilla's effective footprint) and penumbra growth capped tighter (2× near / 1.5× far) to remove the smeary far-cascade over-blur

**GUI & framework**
- Overlay now skips the entire ImGui frame (NewFrame/Render, cursor/focus polling, backbuffer bind, device-removed check) when nothing is visible, with a ~6s grace window so the layout `.ini` still autosaves
- Per-effect config INI polling throttled to once every 500 ms instead of every frame
- New `SetLightVolumeAoTexture` host API for publishing per-light AO to the framework
- `ResourceRegistry` switched from string-keyed hash maps to fixed enum slots — per-frame lookups are now allocation- and hash-free

**Presets**
- All RTGI presets retuned for the new trace: lower GI/AO intensity, shorter ray length, fewer ray steps but more rays-per-pixel, and lower temporal blend / denoise steps; added the new `ShadowContrast` key

**Other**
- Optimisation: Clarity blur now uses CPU-precomputed Gaussian weights (saves up to 65 `exp()` per pixel per pass)
- Optimisation: Bloom skips its whole chain (including the scene copy) when intensity is 0 and debug view is off
- Cleanup: removed the dead SSAO half-resolution code path (textures, upsample shader, sampler)
- Fix: SMAA and several effects now correctly report their enabled state (`enabled ? 1 : 0`) instead of always-on
- Build/CI: added **Nexus Deploy** and **Beta Release** GitHub workflows; updated `build.yml` and `release.yml`

https://github.com/Bazouz660/Dust/compare/v0.7.2...v0.7.3
