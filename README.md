# Dust

Dust is a modular rendering framework for Kenshi that hooks into the game's D3D11 pipeline, providing injection points where shader effects can be applied natively. Unlike post-processing injectors (ReShade, ENB), Dust intercepts specific render passes and integrates effects directly into the deferred lighting pipeline, enabling physically correct results that are immune to auto-exposure, fog bleed, and UI interference.

Effects are loaded as separate DLL plugins from an `effects/` folder using a stable C API (`DustAPI.h`). Anyone can develop and distribute custom effects independently of the framework.

## Features

- **Modular plugin system**: Effects are standalone DLLs loaded at runtime via a stable C ABI
- **In-game GUI** (F11): ImGui overlay with per-effect settings, save/reset controls, and performance metrics
- **GPU performance monitoring**: Per-effect GPU timing via D3D11 timestamp queries (handled automatically by the framework)
- **Hot-reloadable configs**: Edit `.ini` files while the game is running; framework reloads them automatically
- **Pipeline detection**: Identifies render passes by GPU state (render target formats, SRV bindings) rather than fragile shader hashes
- **Runtime shader patching**: Modifies the game's deferred lighting shader bytecode in memory at startup — no files replaced on disk
- **State save/restore**: Full D3D11 state capture ensures effects don't interfere with the game's rendering

## Architecture

Dust works by hooking D3D11 at the vtable level and inspecting GPU state on every fullscreen draw call to identify specific render passes:

```
Game Render Pipeline:
  GBuffer Fill -> Deferred Lighting -> Fog -> Post-Processing -> Tonemapping -> Present
                        |                                              |
                  POST_LIGHTING                                  POST_TONEMAP
                  (SSAO, SSIL, SSS)                              (LUT, Bloom)
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
    const char*         _effectDir;      // Set by framework after DustEffectCreate — DLL directory
    int32_t             priority;        // Dispatch order within same injection point (lower = earlier)
} DustEffectDesc;
```

With `DUST_FLAG_FRAMEWORK_CONFIG`, the framework automatically handles INI load/save/hot-reload from the `DustSettingDesc` array — no boilerplate needed. With `DUST_FLAG_FRAMEWORK_TIMING`, GPU timestamp queries are managed by the framework.

The host provides a `DustHostAPI` struct with functions for logging, resource access (`GetSRV`, `GetRTV`), GPU state management (`SaveState`, `RestoreState`), shader compilation (`CompileShader`, `CompileShaderFromFile`), scene copying (`GetSceneCopy`), fullscreen drawing (`DrawFullscreenTriangle`), and constant buffer helpers (`CreateConstantBuffer`, `UpdateConstantBuffer`).

## Current Effects

### SSAO (Screen-Space Ambient Occlusion) — `POST_LIGHTING`, priority 0

- **GTAO algorithm**: 12 directions, 6 steps per pixel
- **Ambient-only AO**: Applied inside the deferred lighting shader to indirect/environment lighting only, not direct sunlight. Consistent between day and night, immune to auto-exposure
- **Depth-aware bilateral blur**: Horizontal and vertical passes preserve hard edges
- **Debug visualization**: Overlay mode to inspect the raw AO buffer

### SSIL (Screen-Space Indirect Lighting) — `POST_LIGHTING`, priority 10

- **Indirect light bounce**: Samples albedo and depth in 8 directions × 4 steps per pixel to approximate one bounce of indirect lighting from nearby surfaces
- **Color bleeding**: Albedo-weighted accumulation produces colored indirect light (red walls cast a red glow, etc.)
- **Additive composite**: IL result is blended additively onto the HDR render target after lighting, before fog
- **Depth-aware bilateral blur**: Horizontal and vertical passes; configurable sharpness
- **Debug visualization**: Overlay mode to inspect the raw IL buffer

### LUT (Color Grading + Tonemapping) — `POST_TONEMAP`, priority 0

- **Full HDR pipeline**: Captures the R11G11B10_FLOAT scene in `preExecute` before the game's tonemapper runs, then completely replaces the LDR output in `postExecute` — only one 8-bit quantization step in the entire chain
- **ACES filmic tonemapper**: Replaces the vanilla linear tonemapper to eliminate banding artifacts, especially in skies and bright outdoor scenes
- **Parametric LUT**: 32×32×32 color grading LUT generated from configurable parameters (stored as R32G32B32A32_FLOAT to avoid LUT quantization)
- **Lift/Gamma/Gain**: Shadow, midtone, and highlight adjustment
- **Color balance**: Contrast, saturation, temperature, and tint controls
- **Split toning**: Independent shadow and highlight color offsets
- **Exposure (EV stops)**: Pre-tonemap exposure adjustment
- **Triangular dithering**: Applied at the final 8-bit write to break up gradient banding
- **Ships with a cinematic desert preset** tuned for Kenshi's aesthetic

### Screen Space Shadows (SSS) — `POST_LIGHTING`, priority 20

- **Contact shadows**: Ray marches the depth buffer toward the sun direction to add sharp, detailed close-range shadows that mask the low-res shadow map
- **Automatic sun tracking**: Extracts the sun direction and view matrix from the game's constant buffer each frame — shadows follow the in-game day/night cycle
- **Quadratic step distribution**: More samples near the surface for fine contact detail, fewer far away
- **Per-pixel jitter**: Interleaved gradient noise breaks up banding artifacts
- **Depth-aware bilateral blur**: Smooths the shadow mask without bleeding across depth edges
- **Debug visualization**: Overlay mode to inspect the raw shadow mask

### Bloom — `POST_TONEMAP`, priority 100

- **Physically-motivated bloom**: Runs after LUT so bloom is applied to graded colors
- **Multi-pass pipeline**: Threshold extract → progressive downsample chain → upsample chain → composite
- **Dual-threshold extraction**: Separate hard/soft thresholds to control bloom onset and spread
- **Configurable scatter and strength**: Controls how broadly light halos spread

## In-Game GUI

Press **F11** to toggle the ImGui overlay. The GUI has two panes:

**Left pane** (always visible):
- **Framework settings**: Logging toggle, startup notification toggle
- **Performance**: FPS, frame time graph, per-effect GPU cost (color-coded), total GPU budget percentage

**Right pane** (collapsible per-effect):
- Auto-generated settings from each plugin's `DustSettingDesc` array
- **Double-click** any slider for precise numeric input
- Per-parameter **Reset** button (yellow "R" when value differs from saved)
- **Save** button to write changes to disk
- **Reset All** button to reload values from disk

A startup toast notification appears for 30 seconds indicating the mod is active and which key toggles the GUI. This can be disabled in the framework settings.

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
           ├── shaders/
           │   └── fullscreen_vs.hlsl
           └── effects/
               ├── DustSSAO.dll
               ├── DustSSIL.dll
               ├── DustSSS.dll
               ├── DustLUT.dll
               ├── DustBloom.dll
               └── shaders/
                   ├── ssao_*.hlsl
                   ├── ssil_*.hlsl
                   ├── sss_*.hlsl
                   ├── lut_ps.hlsl
                   ├── bloom_*.hlsl
                   └── fullscreen_vs.hlsl
   ```
3. Launch the game. Dust patches the deferred lighting shader in memory at startup (no files are modified on disk), generates default `.ini` configs in the `effects/` folder, and begins rendering.

### Uninstallation

Delete the `Dust` folder from `mods/`. The game's shader is only patched in memory — no disk files are modified, so there is nothing to restore.

## Configuration

Each effect has its own `.ini` file generated automatically in the `effects/` folder on first run. All configs support hot-reload (edit while the game is running) and can also be changed via the in-game GUI (F11).

### Dust.ini (Framework)

```ini
[Dust]
Logging=0
StartupMessage=1
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

### SSS.ini

```ini
[SSS]
Enabled=1
Strength=0.7
MaxDistance=0.005
StepCount=16
Thickness=0.001
DepthBias=0.0001
MaxDepth=0.1
BlurSharpness=0.01
DebugView=0
```

### Bloom.ini

```ini
[Bloom]
Enabled=1
Threshold=0.8
SoftKnee=0.5
Strength=0.3
Scatter=0.7
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
msbuild effects\lut\DustLUT.vcxproj   /p:Configuration=Release /p:Platform=x64
msbuild effects\bloom\DustBloom.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild effects\sss\DustSSS.vcxproj   /p:Configuration=Release /p:Platform=x64
```

### Deployment

Copy the following into `<Kenshi>/mods/Dust/`:

| Source | Destination |
|--------|-------------|
| `src/build/Release/Dust.dll` | `Dust.dll` |
| `mod/RE_Kenshi.json` | `RE_Kenshi.json` |
| `mod/Dust.mod` | `Dust.mod` |
| `mod/Dust.ini` | `Dust.ini` |
| `src/shaders/fullscreen_vs.hlsl` | `shaders/fullscreen_vs.hlsl` |
| `effects/ssao/build/Release/DustSSAO.dll` | `effects/DustSSAO.dll` |
| `effects/ssil/build/Release/DustSSIL.dll` | `effects/DustSSIL.dll` |
| `effects/lut/build/Release/DustLUT.dll` | `effects/DustLUT.dll` |
| `effects/bloom/build/Release/DustBloom.dll` | `effects/DustBloom.dll` |
| `effects/sss/build/Release/DustSSS.dll` | `effects/DustSSS.dll` |
| `effects/*/shaders/*.hlsl` | `effects/shaders/` |

### Creating a New Effect Plugin

1. Create a new directory under `effects/` (e.g., `effects/myeffect/`)
2. Add a `shaders/` subdirectory with your HLSL pixel shaders
3. Include `../../src/DustAPI.h` — this is the only header needed
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

| Effect | Passes | GPU Cost |
|--------|--------|----------|
| SSAO   | 3 (generate + blur H + blur V) | ~1–3 ms |
| SSIL   | 3 (generate + blur H + blur V) | ~2–5 ms |
| SSS    | 3 (generate + blur H + blur V) + 1 composite | ~1–3 ms |
| LUT    | 1 (HDR → ACES → LUT → dither) | ~0.1–0.3 ms |
| Bloom  | ~6 (extract + downsample × 2 + upsample × 2 + composite) | ~0.5–1.5 ms |

## Credits

- [**BFrizzleFoShizzle**](https://github.com/BFrizzleFoShizzle) for [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) (plugin loader) and [KenshiLib](https://github.com/KenshiReclaimer/KenshiLib) (game structure library)
- [**disi30**](https://www.nexusmods.com/kenshi/mods/215) for [ShaderSSAO](https://www.nexusmods.com/kenshi/mods/215), the original Kenshi SSAO mod that inspired this project
- [**Dear ImGui**](https://github.com/ocornut/imgui) for the in-game GUI

## License

This project is licensed under the [MIT License](LICENSE).
