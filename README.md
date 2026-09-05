# Dust

Dust is a modular rendering framework for Kenshi that hooks into the game's D3D11 pipeline, providing injection points where shader effects can be applied natively. Unlike post-processing injectors (ReShade, ENB), Dust intercepts specific render passes and integrates effects directly into the deferred lighting pipeline, enabling physically correct results that integrate naturally with the game's auto-exposure, fog, and UI.

Effects are loaded as separate DLL plugins from an `effects/` folder using a stable C API (`DustAPI.h`). Anyone can develop and distribute custom effects independently of the framework.

## Features

- **Modular plugin system**: Effects are standalone DLLs loaded at runtime via a stable C ABI
- **In-game GUI** (F11): ImGui overlay with per-effect settings, save/reset controls, and performance metrics
- **GPU performance monitoring**: Per-effect GPU timing via D3D11 timestamp queries (handled automatically by the framework)
- **Hot-reloadable configs**: Edit `.ini` files while the game is running; framework reloads them automatically
- **Pipeline detection**: Identifies render passes by GPU state (render target formats, SRV bindings) rather than fragile shader hashes
- **Runtime shader patching**: Modifies the game's deferred lighting shader bytecode in memory at startup. No files replaced on disk
- **State save/restore**: Full D3D11 state capture ensures effects don't interfere with the game's rendering
- **Temporal anti-aliasing**: Optional DLAA (DLSS at native resolution) / FSR2 / FSR3-FSR4, driven by real per-object motion vectors injected into the game's own G-buffer shaders. Backend is filtered by detected GPU

## Architecture

Dust works by hooking D3D11 at the vtable level and inspecting GPU state on every fullscreen draw call to identify specific render passes:

```
Game Render Pipeline:
  GBuffer Fill -> Deferred Lighting -> Fog -> Post-Processing -> Tonemapping -> Present
                        |                                              |
                  POST_LIGHTING                                  POST_TONEMAP
        (Shadows, SSAO, SSIL, RTGI,                  (LUT, Clarity, DOF, Bloom, Deband,
         Kuwahara, Outline)                       Chromatic Aberration, Vignette, Film
                                                    Grain, Letterbox, SMAA)
```

Effects register at specific injection points. When Dust detects a matching render pass, it dispatches registered effects (in priority order) with full access to the relevant GPU resources.

### Plugin API (v8)

Every effect DLL exports a single `DustEffectCreate` function that fills a `DustEffectDesc` struct:

```c
typedef struct DustEffectDesc {
    uint32_t            apiVersion;      // DUST_API_VERSION (currently 8)
    const char*         name;            // Display name
    DustInjectionPoint  injectionPoint;  // Where in the pipeline to run

    // Lifecycle
    int  (*Init)(ID3D11Device*, uint32_t w, uint32_t h, const DustHostAPI*);
    void (*Shutdown)(void);
    void (*OnResolutionChanged)(ID3D11Device*, uint32_t w, uint32_t h);

    // Per-frame callbacks
    DustEffectCallback  preExecute;      // Before the game's draw
    DustEffectCallback  postExecute;     // After the game's draw

    int (*IsEnabled)(void);

    // GUI settings (auto-generated UI from descriptors)
    DustSettingDesc*    settings;
    uint32_t            settingCount;
    void (*OnSettingChanged)(void);      // Runtime updates (e.g. LUT regeneration)

    // v3 additions
    uint32_t            flags;           // DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING
    const char*         configSection;   // INI section name (NULL = use effect name)
    const char*         _effectDir;      // Set by framework after DustEffectCreate (DLL directory)
    int32_t             priority;        // Dispatch order within same injection point (lower = earlier)
} DustEffectDesc;
```

With `DUST_FLAG_FRAMEWORK_CONFIG`, the framework automatically handles INI load/save/hot-reload from the `DustSettingDesc` array, with no boilerplate needed. With `DUST_FLAG_FRAMEWORK_TIMING`, GPU timestamp queries are managed by the framework.

The host provides a `DustHostAPI` struct with functions for logging, resource access (`GetSRV`, `GetRTV`), GPU state management (`SaveState`, `RestoreState`), shader compilation (`CompileShader`, `CompileShaderFromFile`), scene copying (`GetSceneCopy`), pre-fog HDR snapshot (`GetPreFogHDR`), fullscreen drawing (`DrawFullscreenTriangle`), and constant buffer helpers (`CreateConstantBuffer`, `UpdateConstantBuffer`).

## Current Effects

Effects run at one of the pipeline injection points, in ascending priority order within each stage. All are toggleable and fully configurable in the F11 GUI (most also have a debug/overlay mode).

### Deferred lighting stage (`POST_LIGHTING`)

| Effect | Prio | Summary |
|---|---|---|
| **Shadows (RTWSM)** | -10 | PCSS soft shadows via the RTWSM warp map; variable penumbra, Poisson disk, optional cliff-acne fix, resolution up to 16384 |
| **SSAO** | 0 | GTAO ambient occlusion applied to indirect/ambient light only — day-night consistent, auto-exposure immune |
| **SSIL** | 10 | One albedo-weighted bounce of colored indirect light, composited before fog |
| **RTGI** | 20 | Per-pixel screen-space ray-traced GI + AO, multi-bounce, SVGF denoise, full compute pipeline |
| **Kuwahara** | 40 | Anisotropic painterly filter that preserves edges |
| **Outline** | 50 | Depth + normal edge detection, depth-limited |

### Post-tonemap stage (`POST_TONEMAP`)

| Effect | Prio | Summary |
|---|---|---|
| **LUT (Color Grading)** | 0 | Full HDR grade before the game's tonemapper: selectable tonemapper (ACES / Reinhard / Uncharted 2 / AgX / Khronos PBR Neutral), lift-gamma-gain, split toning, 32³ float LUT, dithered 8-bit output |
| **Clarity** | 50 | Local-contrast / midtone detail enhancement with midtone protection |
| **Depth of Field** | 75 | Auto-focus camera blur, independent near/far fields, sky-aware |
| **Bloom** | 100 | HDR bloom extracted post-fog before tonemapping, soft-threshold knee |
| **Deband** | 180 | Depth/luminance-aware debanding, optional sky-only mode |
| **Chromatic Aberration** | 190 | Per-channel edge fringing (single Strength control) |
| **Vignette** | 200 | Circular or rectangular edge darkening |
| **Film Grain** | 210 | Animated monochrome or per-channel chromatic grain |
| **Letterbox** | 220 | Cinematic bars at a target aspect ratio |
| **SMAA** | 250 | 3-pass subpixel morphological AA (luma / depth / combined) |

### Presentation (core framework, `PRE_PRESENT`)

**Temporal Anti-Aliasing (DLAA / FSR)** — DLAA (DLSS on NVIDIA RTX), FSR2 (any GPU), or FSR3/FSR4 (DX12 side-device; FSR4 needs a Radeon RX 9000), auto-filtered by the detected GPU. All backends run at native resolution (render size == display size), so this is an image-quality feature, not a performance one; there is no lower-internal-resolution mode. Driven by real motion vectors injected into the game's own G-buffer shaders (static + skinned) plus a sky pass that stops sky ghosting. Exposes a DLAA model preset + sharpness and an MV debug overlay. Release builds bundle the vendor runtime DLLs (`nvngx_dlss.dll`, `amd_fidelityfx_*_dx12.dll`); FSR2 is statically linked. Best used without driver frame generation (NVIDIA Smooth Motion), which can flicker the UI.

## In-Game GUI

Press **F11** (remappable) to toggle the ImGui overlay. When the overlay is open, game input is blocked (both Windows messages and DirectInput8) and an ImGui cursor is shown.

**Left pane** (resizable):
- **Framework settings**: Mod version display, remappable overlay-toggle key, optional remappable hotkey to flip all effects on/off without opening the overlay, theme selector (Kenshi/Dark), logging toggle, startup notification toggle
- **Preset system**: Quick-switch between presets (Low, Medium, High, Ultra, or custom). Selected preset is remembered across game restarts
- **Performance**: FPS, frame time graph, per-effect GPU cost (color-coded), total GPU budget percentage

**Right pane** (collapsible per-effect):
- **Search box** to filter effects by name
- Auto-generated settings from each plugin's `DustSettingDesc` array
- **Double-click** any slider for precise numeric input
- Per-parameter **Reset** button (yellow "R" when value differs from saved)
- **Save** button to write changes to disk
- **Reset All** button to reload values from disk

A startup toast notification appears for 30 seconds indicating the mod version and which key toggles the GUI. When the mod is updated, a "New version installed!" message is shown. The toast can be disabled in the framework settings.

## Installation

### Requirements

- [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847) (plugin loader for Kenshi)

### Steps

1. Download the latest release
2. Extract the `Dust` folder into your Kenshi `mods/` directory:
   ```
   <Kenshi>/
   └── mods/
       └── Dust/
           ├── Dust.dll
           ├── nvngx_dlss.dll                     # DLSS runtime (NVIDIA; RTX only) — release builds only
           ├── amd_fidelityfx_loader_dx12.dll     # FSR3/FSR4 runtime (AMD) — release builds only
           ├── amd_fidelityfx_upscaler_dx12.dll   # FSR3/FSR4 runtime (AMD) — release builds only
           ├── Dust.ini
           ├── Dust.mod
           ├── RE_Kenshi.json
           └── effects/
               ├── DustSSAO.dll
               ├── DustSSIL.dll
               ├── DustRTGI.dll
               ├── DustShadows.dll
               ├── DustClarity.dll
               ├── DustLUT.dll
               ├── DustBloom.dll
               ├── DustDOF.dll
               ├── DustOutline.dll
               ├── DustKuwahara.dll
               ├── DustSMAA.dll
               ├── DustChromaticAberration.dll
               ├── DustDeband.dll
               ├── DustFilmGrain.dll
               ├── DustLetterbox.dll
               ├── DustVignette.dll
               ├── presets/
               │   ├── dust_low/
               │   ├── dust_medium/
               │   ├── dust_high/
               │   ├── dust_ultra/
               │   ├── dust_cinematic/
               │   ├── stylized_medium/
               │   ├── stylized_high/
               │   └── Xscreade's Preset/
               └── shaders/
                   └── *.hlsl
   ```
3. Launch the game. Dust patches the deferred lighting shader in memory at startup (no files are modified on disk), generates default `.ini` configs in the `effects/` folder, and begins rendering.

### Uninstallation

Delete the `Dust` folder from `mods/`. The game's shader is only patched in memory; no disk files are modified, so there is nothing to restore.

## Configuration

Each effect has its own `.ini` file generated automatically in the `effects/` folder on first run. All configs support hot-reload (edit while the game is running) and can also be changed via the in-game GUI (F11).

### Dust.ini (Framework)

```ini
[Dust]
FileLogging=1         # timestamped logs in logs/ next to the DLL (on by default)
MaxLogFiles=10        # log sessions kept before the oldest are deleted (1-100)
StartupMessage=1
Theme=kenshi          # "kenshi" (warm parchment palette) or "dark" (ImGui default)
ToggleKey=122         # VK code for the overlay toggle (122 = VK_F11)
ToggleEffectsKey=0    # VK code for the all-effects on/off hotkey (0 = unbound)

[Shadows]
ShadowResolution=0   # 0=default, or 2048/4096/8192/16384 (requires restart, RTWSM only)
```

### SSAO.ini

```ini
[SSAO]
Enabled=1
Radius=0.003
Strength=2.5
Bias=0.001
MaxDepth=0.1
ForegroundFade=26.6
FalloffPower=2.0
MaxScreenRadius=0.1
MinScreenRadius=0.001
BlurSharpness=0.01
DebugView=0
```

### SSIL.ini

```ini
[SSIL]
Enabled=1
Radius=0.005
Strength=1.0
Bias=0.05
MaxDepth=0.15
ForegroundFade=26.0
FalloffPower=2.0
MaxScreenRadius=0.05
MinScreenRadius=0.001
ColorBleeding=1.0
BlurSharpness=0.01
DebugView=0
```

### LUT.ini

```ini
[LUT]
Enabled=1
Intensity=0.7
Exposure=0.0
Lift=0.02
Gamma=0.97
Gain=1.05
Contrast=1.08
Saturation=0.85
Temperature=0.08
Tint=0
ShadowR=-0.02
ShadowG=0.01
ShadowB=0.04
HighlightR=0.03
HighlightG=0.01
HighlightB=-0.02
```

### RTGI.ini

```ini
[RTGI]
Enabled=1
RayLength=0.3
RaySteps=16
RaysPerPixel=1
Thickness=0.01
ThicknessCurve=0.8
FadeDistance=1.0
BounceIntensity=0.5
AOIntensity=1.5
GIIntensity=1.0
ResolutionMode=1
DenoiseSteps=4
DebugView=0
```

### Shadows.ini

```ini
[Shadows]
Enabled=1
FilterRadius=1.0
LightSize=3.0
PCSS=1
CliffFix=0
CliffFixDistance=0.10
```

### Clarity.ini

```ini
[Clarity]
Enabled=1
Strength=0.4
MidtoneProtect=0.5
BlurRadius=8
DebugView=0
```

### Bloom.ini

```ini
[Bloom]
Enabled=1
Intensity=0.5
Threshold=1.0
Radius=2.814
DebugView=0
```

### DOF.ini

```ini
[DOF]
Enabled=1
AutoFocus=1
AutoFocusSpeed=3
FocusDistance=0.02
NearStart=0.003
NearEnd=0.034
NearStrength=0.5
FarStart=0.01
FarEnd=0.05
FarStrength=1
BlurRadius=2.565
MaxDepth=1
BlurDownscale=2
DebugView=0
```

### Outline.ini

```ini
[Outline]
Enabled=1
DepthThreshold=0.003
NormalThreshold=0.8
Thickness=1
Strength=0.8
ColorR=0
ColorG=0
ColorB=0
MaxDepth=0.5
DebugView=0
```

### Kuwahara.ini

```ini
[Kuwahara]
Enabled=1
Radius=3
Strength=1.0
Sharpness=8.0
DebugView=0
```

### SMAA.ini

```ini
[SMAA]
Enabled=1
EdgeMode=0          # 0=luma, 1=depth, 2=combined
LumaThreshold=0.1
DepthThreshold=0.01
ShowEdges=0
ShowWeights=0
```

### ChromaticAberration.ini

```ini
[ChromaticAberration]
Enabled=1
Strength=0.003
DebugView=0
```

### Deband.ini

```ini
[Deband]
Enabled=1
Threshold=0.02
Range=16.0
Intensity=1.0
SkyOnly=0
SkyDepthThreshold=0.99
DebugView=0
```

### FilmGrain.ini

```ini
[FilmGrain]
Enabled=1
Intensity=0.05
Size=1.6
Colored=0
DebugView=0
```

### Letterbox.ini

```ini
[Letterbox]
Enabled=1
AspectRatio=2.35
ColorR=0.0
ColorG=0.0
ColorB=0.0
Opacity=1.0
DebugView=0
```

### Vignette.ini

```ini
[Vignette]
Enabled=1
Strength=0.3
Radius=0.8
Softness=0.5
Shape=0             # 0=circular, 1=rectangular
AspectRatio=1.0
DebugView=0
```

## Building from Source

### Prerequisites

- **Visual Studio 2022** (v143 toolset)
- **Windows 10 SDK**

### Setup

1. Clone the repository with submodules:
   ```bash
   git clone --recurse-submodules https://github.com/Bazouz660/Dust.git
   ```
2. Extract Boost headers (one-time setup):
   ```bash
   cd external/KenshiLib_Examples_deps/boost_1_60_0
   unzip boost.zip
   ```

All build dependencies ([KenshiLib](https://github.com/KenshiReclaimer/KenshiLib), [KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)) are included as git submodules in the `external/` directory.

#### Side-stepping the deps repo's LFS

`KenshiLib_Examples_deps` stores `KenshiLib.lib` and a Boost archive in Git LFS. Dust only links `KenshiLib.lib` and only needs Boost as headers, so we skip LFS on submodule checkout and fetch what we need from public mirrors:

- `KenshiLib.lib` is downloaded from the matching `KenshiReclaimer/KenshiLib` GitHub release (regular release asset, no LFS).
- Boost 1.60 headers come from `archives.boost.io`.

CI handles this automatically (cached by KenshiLib release tag and Boost version). For a local setup:

```bash
GIT_LFS_SKIP_SMUDGE=1 git submodule update --init --recursive
powershell.exe -ExecutionPolicy Bypass -File ./tools/fetch_external_libs.ps1
```

The script is idempotent; pass `-Force` to redownload. The KenshiLib submodule must be on a tagged release commit (e.g. `v0.2.1`) for the `.lib` URL to resolve.

### Build

**Framework (Dust.dll):**
```bash
msbuild src\Dust.vcxproj /p:Configuration=Release /p:Platform=x64
```

**Effect plugins:**
```bash
msbuild effects\ssao\DustSSAO.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\ssil\DustSSIL.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\rtgi\DustRTGI.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\shadows\DustShadows.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\lut\DustLUT.vcxproj   /p:Configuration=Release /p:Platform=x64
msbuild effects\bloom\DustBloom.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\clarity\DustClarity.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\dof\DustDOF.vcxproj   /p:Configuration=Release /p:Platform=x64
msbuild effects\outline\DustOutline.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\kuwahara\DustKuwahara.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\smaa\DustSMAA.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\chromaticaberration\DustChromaticAberration.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\deband\DustDeband.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\filmgrain\DustFilmGrain.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\letterbox\DustLetterbox.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\vignette\DustVignette.vcxproj /p:Configuration=Release /p:Platform=x64
```

The `build.ps1` script at the repo root builds all of the above in one go. Pass `-Deploy` to also copy the artifacts into `KENSHI_MOD_DIR` (set in `.env`).

### Deployment

Copy the following into `<Kenshi>/mods/Dust/`:

| Source | Destination |
|--------|-------------|
| `src/build/Release/Dust.dll` | `Dust.dll` |
| `mod/RE_Kenshi.json` | `RE_Kenshi.json` |
| `mod/Dust.mod` | `Dust.mod` |
| `mod/Dust.ini` | `Dust.ini` |
| `effects/ssao/build/Release/DustSSAO.dll` | `effects/DustSSAO.dll` |
| `effects/ssil/build/Release/DustSSIL.dll` | `effects/DustSSIL.dll` |
| `effects/rtgi/build/Release/DustRTGI.dll` | `effects/DustRTGI.dll` |
| `effects/shadows/build/Release/DustShadows.dll` | `effects/DustShadows.dll` |
| `effects/lut/build/Release/DustLUT.dll` | `effects/DustLUT.dll` |
| `effects/bloom/build/Release/DustBloom.dll` | `effects/DustBloom.dll` |
| `effects/clarity/build/Release/DustClarity.dll` | `effects/DustClarity.dll` |
| `effects/dof/build/Release/DustDOF.dll` | `effects/DustDOF.dll` |
| `effects/outline/build/Release/DustOutline.dll` | `effects/DustOutline.dll` |
| `effects/kuwahara/build/Release/DustKuwahara.dll` | `effects/DustKuwahara.dll` |
| `effects/smaa/build/Release/DustSMAA.dll` | `effects/DustSMAA.dll` |
| `effects/chromaticaberration/build/Release/DustChromaticAberration.dll` | `effects/DustChromaticAberration.dll` |
| `effects/deband/build/Release/DustDeband.dll` | `effects/DustDeband.dll` |
| `effects/filmgrain/build/Release/DustFilmGrain.dll` | `effects/DustFilmGrain.dll` |
| `effects/letterbox/build/Release/DustLetterbox.dll` | `effects/DustLetterbox.dll` |
| `effects/vignette/build/Release/DustVignette.dll` | `effects/DustVignette.dll` |
| `effects/presets/` | `effects/presets/` |
| `effects/*/shaders/*.hlsl` | `effects/shaders/` |

### Creating a New Effect Plugin

1. Create a new directory under `effects/` (e.g., `effects/myeffect/`)
2. Add a `shaders/` subdirectory with your HLSL pixel shaders
3. Include `../../src/DustAPI.h`. This is the only header needed
4. Export a `DustEffectCreate` function that fills `DustEffectDesc`
5. Set `flags = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING` to let the framework handle INI I/O and GPU timing automatically
6. Use `host->CompileShaderFromFile()` to load shaders at runtime (no `d3dcompiler.lib` needed in the plugin)
7. Use `host->DrawFullscreenTriangle()` for fullscreen passes (no VS needed in the plugin)
8. Use `host->GetSceneCopy()` for read-modify-write operations on render targets
9. Build as a DLL linking only `d3d11.lib` and place it in the `effects/` folder

See `effects/ssao/DustSSAO.cpp` or `effects/lut/DustLUT.cpp` for complete examples. `DustAPI.h` contains a minimal 50-line example in the file header.

## Performance

### Framework overhead

The framework itself adds near-zero overhead. Per frame, it:

- Checks vertex count on every `Draw` call (single integer comparison, skips non-fullscreen draws)
- On fullscreen draws (~10-15 per frame), queries RT format and SRV bindings to identify the render pass
- Once injection points are detected, no further detection runs for that frame

With no effects enabled, the framework's cost is unmeasurable.

### Effect costs

GPU costs are measured via D3D11 timestamp queries and displayed in the in-game GUI (F11). Typical costs at 2560×1440:

| Effect                | Passes | GPU Cost |
|-----------------------|--------|----------|
| SSAO                  | 3 (generate + blur H + blur V) | ~1–3 ms |
| SSIL                  | 3 (generate + blur H + blur V) | ~2–5 ms |
| RTGI                  | 5+ (ray trace + temporal + variance + denoise × N) | ~3–8 ms |
| Shadows               | 0 (inlined in deferred shader) | ~0.5–1.5 ms |
| Outline               | 1 (edge detect + composite) | ~0.2–0.5 ms |
| Clarity               | 3 (blur H + blur V + composite) | ~0.3–1 ms |
| LUT                   | 1 (HDR → ACES → LUT → dither) | ~0.1–0.3 ms |
| Bloom                 | ~8 (extract + downsample × 3 + upsample × 3 + composite) | ~0.5–1.5 ms |
| DOF                   | 4 (CoC + downsample + blur H + blur V) | ~0.5–2 ms |
| Kuwahara              | 1 (anisotropic filter) | ~1–3 ms |
| SMAA                  | 3 (edge detect + blend weights + resolve) | ~0.3–0.8 ms |
| Chromatic Aberration  | 1 (per-channel offset) | <0.1 ms |
| Deband                | 1 (noise-pattern dither) | ~0.1–0.3 ms |
| Film Grain            | 1 (animated noise) | <0.1 ms |
| Letterbox             | 1 (composite over edges) | <0.1 ms |
| Vignette              | 1 (radial falloff) | <0.1 ms |

## Credits

- [**BFrizzleFoShizzle**](https://github.com/BFrizzleFoShizzle) for [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) (plugin loader) and [KenshiLib](https://github.com/KenshiReclaimer/KenshiLib) (game structure library)
- [**disi30**](https://www.nexusmods.com/kenshi/mods/215) for [ShaderSSAO](https://www.nexusmods.com/kenshi/mods/215), the original Kenshi SSAO mod that inspired this project
- [**Dear ImGui**](https://github.com/ocornut/imgui) for the in-game GUI

## License

Dust is licensed under the **GNU General Public License v3.0 (or later)** with an
additional permission (a Section 7 linking exception) that allows it to be
combined and distributed with the proprietary NVIDIA DLSS / NGX SDK and the AMD
FidelityFX Super Resolution (FSR) SDK. See [LICENSE](LICENSE) for the grant and
the exception, and [COPYING](COPYING) for the full GPLv3 text.

Third-party components keep their own licenses, including the vendored GPU
upscaling SDKs: the NVIDIA DLSS / NGX SDK ([NVIDIA RTX SDKs License](external/DLSS/LICENSE.txt))
and AMD FSR ([MIT](external/FSR2-DX11/LICENSE.txt)). Note that the NVIDIA license
also requires attribution (NVIDIA marks in the about/credits screen) and
pre-release notification before public distribution.
