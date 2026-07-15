## v0.7.4

**Upscaling & Anti-Aliasing (new)**
- New **Upscaling** feature: vendor-selectable temporal super-resolution and native-resolution anti-aliasing (DLAA). Run native for the cleanest image, or upscale from a lower internal resolution for frames
- Three backends, auto-filtered by the detected GPU: **DLSS** (NVIDIA RTX), **FSR2** (any GPU, native D3D11), and **FSR3 / FSR4** (any DX12 GPU via a D3D12 side-device; FSR4 needs a Radeon RX 9000)
- Driven by **real motion vectors** injected into the game's own G-buffer shaders — static *and* skinned/animated characters — plus a dedicated sky pass so the sky doesn't ghost under camera rotation. Not a camera-only approximation
- DLSS model preset (CNN DLAA by default), RCAS **Sharpness** slider, texture mip-bias, viewport jitter, and a POST_TONEMAP resolve
- **Motion-vector debug overlay** to spot geometry that would ghost (grey = still, colour = motion, red = no MV)
- Release builds bundle the vendor runtime DLLs; on-screen NVIDIA DLSS / AMD FSR attribution

**Improvements**
- Shadows: restored character **self-shadowing** (RTWSM depth-bias rescale) and made **shadow mode-switches** robust — no more stale captures, identity-list leak, or overrides silently dropping after a switch
- Shadows: CSM PCSS tuning pass
- SSAO / RTGI point & spot-light AO hardened — fail-safe `light_fs` patch binding and a direct AO-texture handoff to the host, so per-light contact darkening is more reliable

**GUI & framework**
- New in-GUI **Report a Bug** system — bundles logs, hardware, and live effect state into a `DustReport_*.zip` for easy sharing
- **Smooth Motion / overlay compatibility**: the game's D3D11 device is now captured only on a confirmed pipeline signature. Fixes the `DEVICE_REMOVED` crash (which surfaced as a false "out of VRAM") when NVIDIA Smooth Motion or other overlays spin up a second device
- Silenced the false "preset outdated" warning for Shadows presets

**Presets**
- Added community preset: **Xscreade's Preset**
- RTGI and Shadows presets retuned

**Licensing & build**
- Relicensed from MIT to the **GNU GPL v3 with a Section 7 linking exception** that permits combining/distributing with the proprietary DLSS and FSR SDKs (full text in `LICENSE` / `COPYING`)
- CI now fetches the DLSS and FidelityFX SDKs and bundles their runtime DLLs into release builds
- Docs: README and Steam Workshop description updated for the upscaler and condensed

https://github.com/Bazouz660/Dust/compare/v0.7.3...v0.7.4
