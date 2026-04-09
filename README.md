# Dust

Dust is a rendering framework for Kenshi that hooks into the game's D3D11 pipeline, providing injection points where shader effects can be applied natively. Unlike post-processing injectors (ReShade, ENB), Dust intercepts specific render passes and integrates effects directly into the deferred lighting pipeline, enabling physically correct results that are immune to auto-exposure, fog bleed, and UI interference.

The long-term goal is a modular plugin system where effects are loaded as separate modules, allowing anyone to develop and distribute custom effects using a stable API.

## Architecture

Dust works by hooking D3D11 at the vtable level and inspecting GPU state on every fullscreen draw call to identify specific render passes:

```
Game Render Pipeline:
  GBuffer Fill -> Deferred Lighting -> Fog -> Post-Processing -> Tonemapping -> Present
                        |                                             |
                    POST_LIGHTING                                POST_TONEMAP
```

Effects register at specific injection points in the pipeline. When Dust detects a matching render pass, it dispatches registered effects with full access to the relevant GPU resources (depth buffer, HDR target, etc.).

### Core components

- **Pipeline detection**: Identifies render passes by inspecting GPU state (render target formats, shader resource bindings) rather than fragile shader hashes
- **State save/restore**: Full D3D11 pipeline state capture ensures effects don't interfere with the game's rendering
- **Effect dispatch**: Effects register at injection points (`POST_GBUFFER`, `POST_LIGHTING`, `POST_FOG`, `POST_TONEMAP`, `PRE_PRESENT`) and receive a `FrameContext` with device, depth SRV, HDR RTV, resolution, etc.
- **Shader integration**: Effects can bind resources to shader registers before the game's own draw calls execute, enabling modifications to the lighting shader itself

## Current Effects

### SSAO (Screen-Space Ambient Occlusion)

- **GTAO algorithm**: 12 directions, 6 steps per pixel
- **Ambient-only AO**: Applied inside the deferred lighting shader to indirect/environment lighting only, not direct sunlight. Consistent between day and night, immune to auto-exposure
- **Depth-aware bilateral blur**: Horizontal and vertical passes preserve edges
- **Hot-reloadable configuration**: Edit `Dust.ini` while the game is running
- **In-game toggle**: Enable/disable via Kenshi's `settings.cfg` (`Dust_SSAO=1`)
- **Debug visualization**: Overlay mode to see the raw AO buffer

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
3. Launch the game. Dust will automatically install the modified lighting shader (backing up the vanilla version), generate a default `Dust.ini`, and begin rendering

### Uninstallation

The vanilla shader is automatically restored when the DLL unloads. To fully uninstall, delete the `Dust` folder from `mods/`.

## Configuration

### Dust.ini

Located in the mod directory alongside the DLL. Created automatically on first run. Supports hot-reload.

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
3. Copy `.env.template` to `.env` and set your Kenshi install path

All build dependencies ([KenshiLib](https://github.com/KenshiReclaimer/KenshiLib), [KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)) are included as git submodules in the `external/` directory. No environment variables needed.

### Build

1. Open `src/Dust/Dust.vcxproj` in Visual Studio 2022
2. Select **Release | x64**
3. Build the solution

The output `Dust.dll` will be placed in `src/Dust/build/Release/`.

### Deployment

Copy the following to `<Kenshi>/mods/Dust/`:

- `src/Dust/build/Release/Dust.dll`
- `mod/RE_Kenshi.json`
- `mod/shaders/deferred.hlsl`

## Performance

### Framework overhead

The framework itself adds near-zero overhead. Per frame, it:

- Checks vertex count on every `Draw` call (single integer comparison, skips non-fullscreen draws)
- On fullscreen draws (~10-15 per frame), queries RT format and SRV bindings to identify the render pass. These are lightweight D3D11 API calls that read cached driver state
- Once the lighting pass is detected, no further detection runs for that frame

With no effects enabled, the framework's cost is unmeasurable.

### SSAO cost

When SSAO is active, three additional fullscreen passes run before the lighting draw:

1. **AO generation**: GTAO sampling (12 directions x 6 steps) from the depth buffer
2. **Horizontal blur**: Bilateral depth-aware filter
3. **Vertical blur**: Bilateral depth-aware filter

All passes render to R8_UNORM textures (1 byte/pixel), which is significantly cheaper than full RGBA. GPU state is saved and restored around the AO passes to avoid redundant state changes in the game's pipeline.

## Credits

- [**BFrizzleFoShizzle**](https://github.com/BFrizzleFoShizzle) for [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) (plugin loader) and [KenshiLib](https://github.com/KenshiReclaimer/KenshiLib) (game structure library)
- [**disi30**](https://www.nexusmods.com/kenshi/mods/215) for [ShaderSSAO](https://www.nexusmods.com/kenshi/mods/215), the original Kenshi SSAO mod that inspired this project

## License

This project is licensed under the [MIT License](LICENSE).
