# Dust

Dust is a modular rendering framework for Kenshi that hooks into the game's D3D11 pipeline, providing injection points where shader effects can be applied natively. Unlike post-processing injectors (ReShade, ENB), Dust intercepts specific render passes and integrates effects directly into the deferred lighting pipeline, enabling physically correct results that are immune to auto-exposure, fog bleed, and UI interference.

Effects are loaded as separate DLL plugins from an `effects/` folder using a stable C API (`DustAPI.h`). Anyone can develop and distribute custom effects independently of the framework.

## Features

- **Modular plugin system**: Effects are standalone DLLs loaded at runtime via a stable C ABI
- **In-game GUI** (F11): ImGui overlay with per-effect settings, save/reset controls, and performance metrics
- **GPU performance monitoring**: Per-effect GPU timing via D3D11 timestamp queries
- **Hot-reloadable configs**: Edit `.ini` files while the game is running
- **Pipeline detection**: Identifies render passes by GPU state (render target formats, SRV bindings) rather than fragile shader hashes
- **State save/restore**: Full D3D11 state capture ensures effects don't interfere with the game's rendering

## Architecture

Dust works by hooking D3D11 at the vtable level and inspecting GPU state on every fullscreen draw call to identify specific render passes:

```
Game Render Pipeline:
  GBuffer Fill -> Deferred Lighting -> Fog -> Post-Processing -> Tonemapping -> Present
                        |                          |                  |            |
                  POST_LIGHTING               POST_FOG          POST_TONEMAP  PRE_PRESENT
```

Effects register at specific injection points. When Dust detects a matching render pass, it dispatches registered effects with full access to the relevant GPU resources (depth buffer, HDR target, etc.).

### Plugin API

Every effect DLL exports a single `DustEffectCreate` function that fills a `DustEffectDesc` struct:

```c
typedef struct DustEffectDesc {
    uint32_t            apiVersion;      // DUST_API_VERSION (currently 2)
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
    void (*SaveSettings)(void);          // Write current values to disk
    void (*LoadSettings)(void);          // Reload values from disk

    const float*        gpuTimeMsPtr;    // Pointer to GPU timing (read by host)
} DustEffectDesc;
```

The host provides a `DustHostAPI` struct with functions for logging, resource access (`GetSRV`, `GetRTV`), GPU state management (`SaveState`, `RestoreState`), and shader resource binding (`BindSRV`, `UnbindSRV`).

## Current Effects

### SSAO (Screen-Space Ambient Occlusion)

- **GTAO algorithm**: 12 directions, 6 steps per pixel
- **Ambient-only AO**: Applied inside the deferred lighting shader to indirect/environment lighting only, not direct sunlight. Consistent between day and night, immune to auto-exposure
- **Depth-aware bilateral blur**: Horizontal and vertical passes preserve edges
- **Debug visualization**: Overlay mode to see the raw AO buffer

### LUT (Color Grading)

- **Parametric color grading**: 32x32x32 LUT generated from configurable parameters
- **Lift/Gamma/Gain**: Shadow, midtone, and highlight adjustment
- **Color balance**: Contrast, saturation, temperature, and tint controls
- **Split toning**: Independent shadow and highlight color offsets
- **Ships with a cinematic desert preset** tuned for Kenshi's aesthetic

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
           ├── RE_Kenshi.json
           ├── shaders/
           │   └── deferred.hlsl
           └── effects/
               ├── DustSSAO.dll
               ├── SSAO.ini
               ├── DustLUT.dll
               └── LUT.ini
           
   ```
3. Launch the game. Dust will automatically install the modified lighting shader (backing up the vanilla version), generate default configs, and begin rendering

### Uninstallation

The vanilla shader is automatically restored when the DLL unloads. To fully uninstall, delete the `Dust` folder from `mods/`.

## Configuration

Each effect has its own `.ini` file in the `effects/` folder, created automatically on first run. All configs support hot-reload (edit while the game is running) and can also be changed via the in-game GUI (F11).

### Dust.ini (Framework)

Located in the mod root directory.

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
FilterRadius=0.15
ForegroundFade=26.6
FalloffPower=2.0
MaxScreenRadius=0.1
MinScreenRadius=0.001
BlurSharpness=0.01
NightCompensation=10.0
DebugView=0
```

### LUT.ini

```ini
[LUT]
Enabled=1
Intensity=0.7
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
cd src
msbuild Dust.vcxproj /p:Configuration=Release /p:Platform=x64
```

**Effect plugins:**
```bash
cd effects/ssao
msbuild DustSSAO.vcxproj /p:Configuration=Release /p:Platform=x64

cd effects/lut
msbuild DustLUT.vcxproj /p:Configuration=Release /p:Platform=x64
```

### Deployment

Copy the following to `<Kenshi>/mods/Dust/`:

- `src/build/Release/Dust.dll`
- `mod/RE_Kenshi.json`
- `mod/shaders/deferred.hlsl`
- `effects/ssao/build/Release/DustSSAO.dll` -> `effects/DustSSAO.dll`
- `effects/lut/build/Release/DustLUT.dll` -> `effects/DustLUT.dll`

### Creating a New Effect Plugin

1. Create a new directory under `effects/` (e.g., `effects/myeffect/`)
2. Include `../../src/DustAPI.h` for the plugin API
3. Export a `DustEffectCreate` function that fills `DustEffectDesc`
4. Choose an injection point (`DUST_INJECT_POST_LIGHTING`, `POST_FOG`, `POST_TONEMAP`, etc.)
5. Implement `Init`, `Shutdown`, `preExecute`/`postExecute` callbacks
6. Optionally expose settings via `DustSettingDesc` array for automatic GUI generation
7. Build as a DLL and place it in the `effects/` folder

See `effects/ssao/DustSSAO.cpp` or `effects/lut/DustLUT.cpp` for complete examples.

## Performance

### Framework overhead

The framework itself adds near-zero overhead. Per frame, it:

- Checks vertex count on every `Draw` call (single integer comparison, skips non-fullscreen draws)
- On fullscreen draws (~10-15 per frame), queries RT format and SRV bindings to identify the render pass
- Once the lighting pass is detected, no further detection runs for that frame

With no effects enabled, the framework's cost is unmeasurable.

### Effect costs

GPU costs are measured via D3D11 timestamp queries and displayed in the in-game GUI (F11). Typical costs at 2560x1440:

| Effect | GPU Cost |
|--------|----------|
| SSAO   | ~1-3 ms (3 fullscreen passes: generation + 2 blur passes on R8_UNORM) |
| LUT    | ~0.1-0.3 ms (1 fullscreen pass) |

## Credits

- [**BFrizzleFoShizzle**](https://github.com/BFrizzleFoShizzle) for [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) (plugin loader) and [KenshiLib](https://github.com/KenshiReclaimer/KenshiLib) (game structure library)
- [**disi30**](https://www.nexusmods.com/kenshi/mods/215) for [ShaderSSAO](https://www.nexusmods.com/kenshi/mods/215), the original Kenshi SSAO mod that inspired this project
- [**Dear ImGui**](https://github.com/ocornut/imgui) for the in-game GUI

## License

This project is licensed under the [MIT License](LICENSE).
