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

### Plugin API (v3)

Every effect DLL exports a single `DustEffectCreate` function that fills a `DustEffectDesc` struct:

```c
typedef struct DustEffectDesc {
    uint32_t            apiVersion;      // DUST_API_VERSION (currently 3)
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

### SSAO (Screen-Space Ambient Occlusion) (`POST_LIGHTING`, priority 0)

- **GTAO algorithm**: 12 directions, 6 steps per pixel
- **Ambient-only AO**: Applied inside the deferred lighting shader to indirect/environment lighting only, not direct sunlight. Consistent between day and night, immune to auto-exposure
- **Depth-aware bilateral blur**: Horizontal and vertical passes preserve hard edges
- **Debug visualization**: Overlay mode to inspect the raw AO buffer

### SSIL (Screen-Space Indirect Lighting) (`POST_LIGHTING`, priority 10)

- **Indirect light bounce**: Samples albedo and depth in 8 directions × 4 steps per pixel to approximate one bounce of indirect lighting from nearby surfaces
- **Color bleeding**: Albedo-weighted accumulation produces colored indirect light (red walls cast a red glow, etc.)
- **Additive composite**: IL result is blended additively onto the HDR render target after lighting, before fog
- **Depth-aware bilateral blur**: Horizontal and vertical passes; configurable sharpness
- **Debug visualization**: Overlay mode to inspect the raw IL buffer

### LUT (Color Grading + Tonemapping) (`POST_TONEMAP`, priority 0)

- **Full HDR pipeline**: Captures the R11G11B10_FLOAT scene in `preExecute` before the game's tonemapper runs, then completely replaces the LDR output in `postExecute`, with only one 8-bit quantization step in the entire chain
- **Selectable tonemapper**: ACES (Narkowicz/Hill), Reinhard, Reinhard Extended, Uncharted 2 (Hable), AgX, Khronos PBR Neutral, or linear passthrough
- **Parametric LUT**: 32×32×32 color grading LUT generated from configurable parameters (stored as R32G32B32A32_FLOAT to avoid LUT quantization)
- **Lift/Gamma/Gain**: Shadow, midtone, and highlight adjustment
- **Color balance**: Contrast, saturation, temperature, and tint controls
- **Split toning**: Independent shadow and highlight color offsets
- **Exposure (EV stops)**: Pre-tonemap exposure adjustment
- **Triangular dithering**: Applied at the final 8-bit write to break up gradient banding
- **Ships with a cinematic desert preset** tuned for Kenshi's aesthetic

### Clarity / Local Contrast (`POST_TONEMAP`, priority 50)

- **Midtone detail enhancement**: Extracts and amplifies local contrast by subtracting a large-radius Gaussian blur from the original scene
- **Midtone protection**: Luminance-based mask focuses the effect on midtones, preventing clipping in shadows and highlights
- **Variable blur radius**: Configurable Gaussian kernel radius controls the spatial scale of "local" contrast (small = fine detail, large = broad structure)
- **Debug visualization**: Overlay mode shows the extracted detail layer (gray = neutral, bright = positive detail, dark = negative)

### Bloom (`POST_TONEMAP`, priority 100)

- **HDR bloom pipeline**: Captures HDR scene before tonemapping, extracts bright areas, builds gaussian bloom via progressive downsample/upsample chain, composites additively onto LDR
- **Post-fog extraction**: Blooms from the post-fog HDR scene so distant fogged objects don't bleed through
- **Soft threshold**: Smooth knee curve controls bloom onset, so only genuinely bright features contribute
- **Simplified controls**: Intensity, Threshold, and Radius, with no redundant settings

### Outline (`POST_LIGHTING`, priority 50)

- **Edge detection**: Detects edges from both depth discontinuities (Laplacian) and normal angle differences
- **Configurable appearance**: Adjustable thickness, strength/opacity, and outline color (RGB)
- **Depth-limited**: Max depth parameter prevents outlines on distant objects and sky
- **Debug visualization**: Overlay mode to inspect edge detection output

### RTGI (Ray-Traced Global Illumination) (`POST_LIGHTING`, priority 20)

- **Screen-space indirect lighting**: Casts rays per pixel against the depth buffer and samples lit scene radiance at hit points for physically-based indirect illumination
- **Ambient occlusion**: Occlusion term computed from ray hits, applied alongside indirect light
- **Multi-bounce**: Previous frame's GI is fed back for approximate multi-bounce light transport
- **Temporal accumulation + SVGF denoise**: A-trous wavelet filter with variance-guided edge stopping produces stable, noise-free output
- **Compute shader pipeline**: Ray trace and denoise passes use compute shaders to eliminate pixel shader quad waste at depth discontinuities
- **Configurable**: Ray count, step count, ray length, thickness, resolution mode (full/half/quarter), denoise iterations
- **Debug visualization**: Overlay modes for indirect light, AO, and normals

### Shadows (RTWSM Enhancement) (`POST_LIGHTING`, priority -10)

- **Improved shadow filtering**: Replaces the game's basic shadow sampling with PCSS (Percentage-Closer Soft Shadows) via the RTWSM warp map
- **Variable penumbra**: Light size parameter controls how much shadows soften with distance from the caster
- **12-sample Poisson disk**: Jittered per-pixel rotation for smooth, low-noise shadow edges
- **Configurable**: Filter radius, light size, PCSS toggle, bias scale
- **Cliff Shadow Fix (optional)**: Adds a small steep-surface bias that suppresses shadow acne on cliffs and vertical faces. Off by default since enabling it can fade close-range vertical shadows; the start distance is a smooth ramp controlled by a slider
- **Shadow map resolution override**: Configurable in the GUI (2048–16384), requires restart

### Kuwahara Filter (`POST_LIGHTING`, priority 40)

- **Painterly effect**: Anisotropic Kuwahara filter that smooths flat regions while preserving edges, giving a hand-painted look
- **Configurable radius**: Controls the size of the filter kernel
- **Blend strength**: Smoothly blend between original and filtered result
- **Sharpness**: Controls how aggressively the lowest-variance sector wins

### Depth of Field (`POST_TONEMAP`, priority 75)

- **Auto-focus**: Samples depth at screen center and smoothly tracks focus distance
- **Near and far field blur**: Independent control over near-field and far-field blur strength and range
- **Sky handling**: Sky pixels are clamped to max depth so they follow the natural far-field ramp: sharp when looking at the sky, soft at the horizon
- **Configurable blur**: Adjustable blur radius and downscale factor for performance

### Deband (`POST_TONEMAP`, priority 180)

- **Gradient banding removal**: Adds a small, depth/luminance-aware noise pattern to break up the 8-bit color quantization that produces visible bands in skies and soft gradients
- **Sky-only mode**: Optionally limits debanding to background pixels using a configurable depth threshold
- **Configurable**: Threshold, sample range, intensity

### Chromatic Aberration (`POST_TONEMAP`, priority 190)

- **Lens-style color fringing**: Per-channel UV offset that grows toward the screen edges, mimicking real-lens dispersion
- **Single parameter**: Strength controls the magnitude of the offset

### Vignette (`POST_TONEMAP`, priority 200)

- **Edge darkening**: Smooth radial falloff with configurable radius, softness, and strength
- **Shape modes**: Circular or rectangular falloff with adjustable aspect ratio

### Film Grain (`POST_TONEMAP`, priority 210)

- **Animated noise**: Per-frame grain pattern with adjustable size and intensity
- **Color modes**: Monochrome luminance grain or full per-channel chromatic grain

### Letterbox (`POST_TONEMAP`, priority 220)

- **Cinematic bars**: Constrains the visible image to a target aspect ratio (default 2.35:1)
- **Configurable**: Aspect ratio, bar color, opacity

### SMAA (`POST_TONEMAP`, priority 250)

- **Subpixel Morphological Anti-Aliasing**: 3-pass SMAA (edge detect → blend weights → resolve) cleans up jagged edges left by the game's forward-rendered geometry
- **Edge detection modes**: Luma, depth, or combined
- **Debug visualization**: Show edge map or blend weights for tuning

## In-Game GUI

Press **F11** (remappable) to toggle the ImGui overlay. When the overlay is open, game input is blocked (both Windows messages and DirectInput8) and an ImGui cursor is shown.

**Left pane** (resizable):
- **Framework settings**: Mod version display, remappable toggle key, logging toggle, startup notification toggle
- **Preset system**: Quick-switch between presets (Low, Medium, High, Ultra, or custom). Selected preset is remembered across game restarts
- **Performance**: FPS, frame time graph, per-effect GPU cost (color-coded), total GPU budget percentage

**Right pane** (collapsible per-effect):
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
               │   └── stylized_high/
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
Logging=0
StartupMessage=1
Theme=kenshi        # "kenshi" (warm parchment palette) or "dark" (ImGui default)

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
TemporalBlend=0.95
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
BiasScale=1.0
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

This project is licensed under the [MIT License](LICENSE).
