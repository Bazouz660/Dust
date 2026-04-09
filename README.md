# Dust

Dust is a Kenshi rendering framework that hooks into the game's D3D11 render pipeline to provide a way to implement native shader effects like SSAO, DoF, tonemapping, and more, without compromising on performance or relying on limited post-processing techniques that introduce artifacts or bleed through fog, UI, or other effects.

Unlike traditional injectors (ReShade, ENB), Dust intercepts specific render passes and integrates effects directly into the game's deferred lighting pipeline. This means effects like ambient occlusion can be applied to indirect lighting only, making them physically correct and immune to issues like auto-exposure compensation.

## Features

### Screen-Space Ambient Occlusion (SSAO)

- **Ground Truth Ambient Occlusion (GTAO)** algorithm: 12 directions, 6 steps per pixel
- **Ambient-only application**: AO is applied inside the deferred lighting shader to indirect/environment lighting only, not direct sunlight. This keeps AO consistent between day and night
- **Depth-aware bilateral blur**: Horizontal and vertical blur passes preserve edges using depth comparison
- **Hot-reloadable configuration**: Edit `Dust.ini` while the game is running, changes apply instantly
- **In-game toggle**: Enable/disable via Kenshi's settings menu
- **Debug visualization**: Overlay mode to see the raw AO buffer

### Framework Architecture

Dust is built as a modular effect framework:

- **D3D11 vtable hooking**: Hooks `Draw`, `CreateTexture2D`, `Present`, `ClearRenderTargetView`, and `RSSetViewports` to intercept rendering without engine modifications
- **Hash-free pipeline detection**: Identifies render passes by inspecting GPU state (render target formats, shader resource bindings) rather than fragile shader hashes
- **State save/restore**: Full D3D11 pipeline state capture ensures effects don't interfere with the game's rendering
- **Plugin system**: Effects register at specific injection points in the pipeline (`POST_GBUFFER`, `POST_LIGHTING`, `POST_FOG`, `POST_TONEMAP`, `PRE_PRESENT`)

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
           ├── RE_Kenshi.json
           └── shaders/
               └── deferred.hlsl
   ```
3. Launch the game: Dust will automatically:
   - Install the modified lighting shader (backing up the vanilla version)
   - Generate a default `Dust.ini` configuration file
   - Begin rendering SSAO

### Uninstallation

The vanilla `deferred.hlsl` shader is automatically restored when the DLL unloads. To fully uninstall, simply delete the `Dust` folder from `mods/`.

## Configuration

### Dust.ini

Located in the mod directory alongside the DLL. Created automatically on first run with sensible defaults. Supports hot-reload: save the file and changes apply immediately.

```ini
[SSAO]
Enabled=1
Radius=0.002
Strength=2.0
Bias=0.05
MaxDepth=0.1
FilterRadius=0.15
ForegroundFade=50.0
FalloffPower=2.0
MaxScreenRadius=0.1
MinScreenRadius=0.001
DepthFadeStart=0.0
BlurSharpness=0.01
NightCompensation=10.0
TanHalfFov=0.5218
DebugView=0
```

| Parameter | Description |
|-----------|-------------|
| `Enabled` | Master toggle for SSAO (1 = on, 0 = off) |
| `Radius` | World-space AO sampling radius |
| `Strength` | AO intensity multiplier |
| `Bias` | Depth bias to prevent self-occlusion artifacts |
| `MaxDepth` | Maximum depth threshold, fragments beyond this are skipped |
| `FilterRadius` | Bilateral blur radius (higher = smoother, softer AO) |
| `ForegroundFade` | Distance at which AO fades in for close objects |
| `FalloffPower` | Controls how quickly AO fades with distance |
| `MaxScreenRadius` | Upper clamp for screen-space sample radius |
| `MinScreenRadius` | Lower clamp for screen-space sample radius |
| `DepthFadeStart` | Depth at which AO begins fading out |
| `BlurSharpness` | Edge-preserving sharpness for bilateral blur |
| `NightCompensation` | Adjusts AO intensity in low-light conditions |
| `TanHalfFov` | Camera field-of-view parameter (default matches Kenshi's FOV) |
| `DebugView` | Shows raw AO buffer as a screen overlay (1 = on) |

### In-Game Toggle

SSAO can be toggled at runtime through Kenshi's `settings.cfg` using the key `Dust_SSAO` (1 = enabled, 0 = disabled).

## Building from Source

### Prerequisites

- **Visual Studio 2022** (v143 toolset)
- **Windows 10 SDK**
- **KenshiLib**: Set the following environment variables:
  - `KENSHILIB_DIR`: Path to KenshiLib (includes headers for Kenshi internals and OGRE)
  - `KENSHILIB_DEPS_DIR`: Path to KenshiLib dependencies
- **Boost**: Set `BOOST_INCLUDE_PATH` to your Boost headers directory

### Build

1. Open `src/Dust/Dust.vcxproj` in Visual Studio 2022
2. Select **Release | x64**
3. Build the solution

The output `Dust.dll` will be placed in the configured output directory.

### Deployment

Copy the following to `<Kenshi>/mods/Dust/`:

- `Dust.dll`
- `RE_Kenshi.json`
- `shaders/deferred.hlsl`

## How It Works

Dust hooks into D3D11 at the vtable level and inspects GPU state on every fullscreen draw call to identify specific render passes:

```
Game Render Pipeline:
  GBuffer Fill → Deferred Lighting → Fog → Post-Processing → Tonemapping → Present
                        ↑
                    Dust hooks here
```

For SSAO specifically:

1. **Startup**: The DLL installs a modified `deferred.hlsl` that adds an AO texture sampler on register `s8`
2. **Detection**: On each frame, Dust identifies the lighting pass by checking render target format (`R11G11B10_FLOAT`) and bound shader resources (`R32_FLOAT` depth in slot 2)
3. **AO Generation**: Before the lighting draw executes, Dust renders 3 passes: GTAO generation from the depth buffer, then horizontal and vertical bilateral blur
4. **Binding**: The blurred AO texture is bound to shader slot 8. A white 1x1 fallback texture is used when SSAO is disabled
5. **Lighting**: The game's lighting shader executes and reads AO from `s8`, applying it only to ambient/indirect lighting terms
6. **Cleanup**: Slot 8 is unbound, GPU state is fully restored

This approach ensures AO is physically correct (only affects indirect light), immune to auto-exposure compensation, and invisible to fog and UI rendering.

## License

This project is licensed under the [MIT License](LICENSE).
