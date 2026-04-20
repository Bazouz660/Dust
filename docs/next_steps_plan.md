# Dust — Next Steps Plan

*Written: 2026-04-20*

This document captures the full context and roadmap for Dust development, from immediate tasks through the long-term vision. It reflects every decision, constraint, and technical detail discussed so far.

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [What Exists Today](#2-what-exists-today)
3. [Immediate: Test & Validate Survey System](#3-immediate-test--validate-survey-system)
4. [Phase 1: Parse Survey Data & Map the Pipeline](#4-phase-1-parse-survey-data--map-the-pipeline)
5. [Phase 2: Stabilize Current Version](#5-phase-2-stabilize-current-version)
6. [Phase 3: New Major Version Branch](#6-phase-3-new-major-version-branch)
7. [Dust API v4: Full Pipeline Modding ABI](#7-dust-api-v4-full-pipeline-modding-abi)
8. [Phase 3A: Quick Wins (Shader-Only, No Geometry)](#8-phase-3a-quick-wins-shader-only-no-geometry)
9. [Phase 3B: Geometry Interception Foundation](#9-phase-3b-geometry-interception-foundation)
10. [Phase 3C: Shadow System Replacement](#10-phase-3c-shadow-system-replacement)
11. [Phase 3D: Per-Material Shading](#11-phase-3d-per-material-shading)
12. [Phase 3E: World-Space GI & Unlimited Light Shadows](#12-phase-3e-world-space-gi--unlimited-light-shadows)
13. [Phase 3F: Performance Optimizations](#13-phase-3f-performance-optimizations)
14. [Phase 3G: DXR Ray Tracing (Endgame)](#14-phase-3g-dxr-ray-tracing-endgame)
15. [Distribution & Compatibility](#15-distribution--compatibility)
16. [Graphics Quality Guidelines Reference](#16-graphics-quality-guidelines-reference)
17. [Open Questions](#17-open-questions)

---

## 1. Project Overview

**Dust** is a D3D11 rendering mod for Kenshi (OGRE 2.0 engine), distributed as a RE_Kenshi plugin via Steam Workshop. It hooks D3D11 functions at the VTable level, patches the deferred lighting shader at runtime via D3DCompile interception, and injects post-processing effects around the game's draw calls.

**Ultimate goal:** Modernize Kenshi's graphics to AAA quality — perfect shadows from all light sources, physically-based materials, world-space global illumination, proper anti-aliasing — all artifact-free, no temporal tricks, native resolution.

**Design philosophy:** Aligned with Threat Interactive's stance on graphics quality. See `docs/graphics_quality_guidelines.md` for the full guidelines. Core rules:
- No temporal accumulation (no TAA, no temporal denoising, no upscaling)
- Native resolution rendering
- Better BRDFs (replace Lambert + Blinn-Phong)
- Spatial-only denoising
- No artificial limits (unlimited shadow-casting lights)
- Clean image every single frame

---

## 2. What Exists Today

### Hooked D3D11 Functions

| Function | VTable | Purpose |
|----------|--------|---------|
| `CreateTexture2D` | Device:5 | Shadow map resolution override, texture detection |
| `CreatePixelShader` | Device:15 | Shader source tracking (bytecode hash → COM pointer) |
| `CreateVertexShader` | Device:12 | Shader source tracking (bytecode hash → COM pointer) |
| `D3DCompile` | d3dcompiler_47.dll | Runtime HLSL patching (AO injection, shadow filtering) + source capture |
| `Draw` | Context:13 | Fullscreen draw interception (pipeline detection + effect dispatch) |
| `DrawIndexed` | Context:12 | Survey recording, geometry counting |
| `DrawIndexedInstanced` | Context:20 | Survey recording |
| `OMSetRenderTargets` | Context:33 | Render target tracking |
| `Present` / `Present1` | SwapChain:8/22 | Frame boundary, ImGui overlay rendering (deferred hook) |
| `ResizeBuffers` | SwapChain:13 | Resolution change handling (deferred hook) |

### Game Loop Hook

`GameWorld::_NV_mainLoop_GPUSensitiveStuff` — per-frame state reset, watchdog for Present hook failure.

### Implemented Effects

| Effect | Technique | Injection Point | Status |
|--------|-----------|-----------------|--------|
| SSAO | GTAO (12 dir, 6 steps) | POST_LIGHTING pri 0 | Stable |
| SSIL | Screen-space radiance (8 dir × 4 steps) | POST_LIGHTING pri 10 | Stable |
| SSS | Depth-buffer ray march toward sun | POST_LIGHTING pri 20 | Stable |
| RTGI | Screen-space ray trace + compute denoiser | POST_LIGHTING pri 5 | Stable (uses temporal accumulation — candidate for replacement in v2) |
| Shadows | PCSS via deferred shader patch (12-sample Poisson) | POST_LIGHTING pri 30 | Stable |
| LUT | 32³ float LUT + ACES tonemapping + dithering | POST_TONEMAP pri 0 | Stable |
| Bloom | Threshold + progressive downsample/upsample | POST_TONEMAP pri 100 | Stable |
| Clarity | Unsharp mask with midtone protection | POST_TONEMAP pri 50 | Stable |
| DOF | Depth-based blur | POST_TONEMAP | Stable |
| Kuwahara | Stylized painterly filter | POST_TONEMAP | Stable |
| Outline | Edge detection | POST_TONEMAP | Stable |

### Survey System (New, Untested)

Files: `Survey.h/.cpp`, `SurveyRecorder.h/.cpp`, `SurveyWriter.h/.cpp`

Captures every Draw/DrawIndexed/DrawIndexedInstanced call across N frames with full pipeline state. 4 detail levels, SEH-wrapped, JSON output + HLSL shader dumps. GUI controls in DustGUI.cpp.

### Recent Fixes (Untested)

**Deferred Present Hook** (`fc22815`): Present/Present1/ResizeBuffers hooks are now deferred to the first Draw call instead of being installed at `Install()` time. This avoids a race with overlay DLLs (Steam, Discord, ReShade) that also hook Present. Includes DXGI surface parent walk to discover the real swap chain and verify the function address. Watchdog in game loop triggers recovery after 120 loops if Present never fires.

### Kenshi's Rendering Pipeline

Deferred shading with forward light volumes and forward sky/water/particles. Full pipeline documented in `docs/pipeline/` (10 files, organized from survey data). Legacy monolith at `docs/kenshi_rendering_pipeline.md`.

```
[Heat haze prev-frame] → [Cubemap 512x512] → GBuffer fill (3 MRT + depth) →
Stencil mask → GBuffer repack (YCoCg→float) → Deferred sun → Light volumes →
Water (forward) → Fog/Atmosphere → Luminance chain → Bloom extract → Tonemapping →
LDR post → DOF → Half-res blur → Bloom pyramid → FXAA → Heat Haze → Present
```

**GBuffer layout (confirmed by survey):**
- RT0: B8G8R8A8_UNORM — YCoCg chroma-subsampled color (NOT separate albedo+roughness)
- RT1: B8G8R8A8_UNORM — Packed normals + material data (emissive/translucency in alpha)
- RT2: R32_FLOAT — Linear depth
- GBuffer is repacked to R16G16B16A16_FLOAT before deferred lighting (draws 3645-3647)

**GBuffer material data:** Roughness and metalness from textures are NOT stored as separate GBuffer channels. The YCoCg encoding uses all channels of RT0 for color. Material properties flow through the existing per-pixel shader math but are not independently recoverable from the GBuffer for deferred lighting — this limits per-material BRDF branching.

**Kenshi's BRDF (confirmed by survey — deferred.hlsl captured):**
- **Specular:** GGX microfacet model — Trowbridge-Reitz NDF, Schlick Fresnel, Kelemen-Szirmay-Kalos visibility term
- **Diffuse:** Lambert with Fresnel energy conservation (Disney/Burley diffuse is present in lightingFunctions.hlsl but commented out)
- **IBL:** Lazarov approximation for specular, cubemap decode `rgb * a * scale`
- NOT Blinn-Phong as previously suspected — Kenshi already has modern PBR

**Kenshi's shadows (confirmed by survey):**
- CSM: 4 cascades in a single 4096x4096 R32_FLOAT atlas
- 52% tessellated shadow casters (PATCHLIST_3_CP topology), 7 unique VS, 4 unique PS
- 12-sample hex PCF filtering in deferred shader
- Draws 2590-3640 (~1050 draw calls in dense scenes)
- Current Dust fix: PCSS via patched deferred shader

**Light volumes:** Point/spot lights rendered as forward geometry (spheres/cones) with additive blending after the deferred sun pass. Each light is a separate draw call with its own constant buffer containing position, color, radius, attenuation.

---

## 3. Immediate: Test & Validate Survey System

**When:** Tonight (2026-04-20)

**Steps:**

1. Build the current `pipeline-survey-system` branch
2. Load into Kenshi with RE_Kenshi
3. Check `Dust.log` for:
   - `"Present hook deferred"` — confirms deferred hook path is active
   - `"All swap chain hooks installed successfully (deferred)"` — confirms Present hook installed
   - `"Present address MISMATCH"` — if it appears, the real swap chain address differs from temp device (valuable diagnostic)
   - Game loop watchdog messages — should NOT appear if Present hook is working
4. Open the Dust GUI (should appear reliably now with deferred Present hook)
5. Navigate to the Pipeline Survey section
6. Set frames = 5, detail = 1 (Standard)
7. Hit "Capture Survey"
8. Check `<mod dir>/survey/` for output:
   - `frame_0000.json` through `frame_0004.json`
   - `summary.json`
   - `shaders/` directory with `.hlsl` files
9. Verify JSON is well-formed and contains expected data
10. Run again with detail = 2 (Deep) to test constant buffer readback
11. Run again with detail = 3 (Full) for complete data

**If it doesn't work:**
- Check `Dust.log` for crash breadcrumbs (the survey logs which query it's about to attempt)
- SEH wrappers should prevent crashes but may produce empty/partial data
- Common issues: COM reference leaks, staging buffer pool exhaustion, JSON formatting bugs
- We tweak and iterate until it works

**If the GUI doesn't appear:**
- Check for watchdog `"WARNING: Present hook has not fired"` in log
- Check if `TryRecoverPresent()` was called and what it logged
- May need to test with/without Steam overlay, ReShade, Discord overlay

---

## 4. Phase 1: Parse Survey Data & Map the Pipeline

**When:** After survey is validated

**Goal:** Build a complete, verified map of everything Kenshi does each frame. This is the foundation for every future feature.

### 4.1 — Survey Analysis Tool

Write a script/tool that reads the survey JSON and produces:

- **Draw call statistics:**
  - Total draws per frame (Draw vs DrawIndexed vs DrawIndexedInstanced)
  - Draws per render pass (grouped by render target configuration)
  - Average, min, max across captured frames

- **Instancing candidates:**
  - Group draws by mesh signature (VB pointer + IB pointer + VS pointer)
  - For each unique mesh: how many times drawn, with what index count
  - Top N most-repeated meshes — these are the instancing candidates
  - Total potential draw call reduction if all repeats were instanced

- **State change analysis:**
  - How many Set* calls are redundant (same state re-bound between consecutive draws)
  - Shader change frequency vs. draw count
  - Texture binding change frequency

- **Render pass identification:**
  - Sequence of RT configurations (each unique RT set = a pass boundary)
  - Draw count per pass
  - Correlation with known pipeline stages (GBuffer, shadow, lighting, post-process)

- **Shader inventory:**
  - All unique VS/PS used, with draw counts
  - Map shader source files to pipeline stages
  - Identify the deferred lighting shader, shadow shaders, GBuffer shaders, post-process shaders

- **Resource inventory:**
  - All textures bound as SRVs, with formats and sizes
  - All render targets used, with formats and sizes
  - Constant buffer sizes and binding frequency

### 4.2 — Deferred Shader BRDF Analysis ✅ COMPLETE

**Findings:** GGX specular (Trowbridge-Reitz NDF, Schlick Fresnel, Kelemen-SzK visibility) + Lambert diffuse with Fresnel energy conservation. Disney/Burley diffuse is present but commented out. IBL uses Lazarov approximation. See `docs/pipeline/04_deferred_lighting.md`.

### 4.3 — GBuffer Alpha Channel Investigation ✅ COMPLETE

**Findings:** GBuffer uses YCoCg chroma subsampling — all channels of RT0 encode color, not separate roughness/metalness. RT1.a encodes emissive/translucency. Roughness is derived from gloss in the material shaders and flows through the per-pixel lighting math, but is NOT stored as a separate GBuffer channel for deferred access. See `docs/pipeline/01_gbuffer.md`.

### 4.4 — Shadow Pass Analysis ✅ COMPLETE

**Findings:** CSM with 4 cascades in a single 4096x4096 R32_FLOAT atlas. 52% of shadow casters are tessellated (PATCHLIST_3_CP). 7 unique VS, 4 unique PS, 1 HS, 1 DS. Shadow VS CB0 is 64 bytes (light view matrix + translation). Shadow PS CB0 is 16 bytes (bias parameters). 12-sample hex PCF in the deferred shader. See `docs/pipeline/03_shadows.md`.

### 4.5 — Light Data Extraction ✅ COMPLETE

**Findings:** Point/spot lights are forward-rendered as light volumes (additive-blended sphere/cone geometry). Each light is a separate draw call after the deferred sun pass. Deferred sun PS CB0 (352 bytes): c0=sunDirection, c1=sunColour, c6=resolution, c8-c11=inverseView matrix. Light volume PS shares the same `light_fs` shader with per-light CB data. See `docs/pipeline/04_deferred_lighting.md`.

### 4.6 — Documentation Update ✅ COMPLETE

Created `docs/pipeline/` with 10 organized files (00_overview through 09_injection_reference). 492 unique shaders mapped to 50 source files, 0 unidentified. All pipeline stages documented with draw indices, shader sources, RT formats, and injection points.

---

## 5. Phase 2: Stabilize Current Version

**When:** After pipeline mapping is complete

**Goal:** Fix all known bugs, clean up code, make the current feature set rock-solid before branching.

### 5.1 — Fix Known Issues

- **GUI not appearing** — the deferred Present hook fix (`fc22815`) should address this. Verify across multiple launches (10+), with and without overlays. If still intermittent, dig deeper using diagnostic logs.
- **Any survey-revealed bugs** — the pipeline mapping may reveal issues in how effects interact with the pipeline (wrong injection timing, missed state, etc.)
- **RTGI temporal artifacts** — RTGI currently uses temporal accumulation. For the stable release, this is acceptable (it's the existing shipped behavior). For v2, it will be replaced with a spatial-only technique.

### 5.2 — Shader Cache Compatibility ✅ COMPLETE

Implemented stamp-based cache management in `src/dllmain.cpp`:
- `BuildCacheStamp()` generates a stamp from Dust version + shadow resolution config
- `ManageShaderCache()` compares stamp to `RE_Kenshi/dust_cache_stamp.txt`
  - Match → keep cache (fast startup, no recompilation)
  - Mismatch → delete `shader_cache.sc`, write new stamp
- Replaces the old `InvalidateShaderCache()` that deleted the cache every launch

### 5.3 — Code Cleanup ✅ COMPLETE

Audited all source files:
- **COM references:** All properly released. SurveyRecorder uses SAFE_RELEASE throughout, staging buffer pool cleaned in Shutdown(). EffectLoader releases scene copies, pre-fog HDR, timing queries, fullscreen VS on shutdown/reinit. D3D11StateBlock releases all captured COM objects on restore/destruction. PipelineDetector releases all Get* results immediately.
- **Dead code removed:** `JsonEscape` in SurveyWriter.cpp (defined but never called), `LUMINANCE_SRV` in ResourceRegistry.h (defined but never used, no corresponding API constant).
- **No dead #ifdef blocks found** — `DUST_VERSION` is the only conditional, correctly used in dllmain.cpp, DustGUI.cpp.
- **Survey system:** Clean. Timestamped output directories, label support, SEH-wrapped state capture. Added to DustGUI.
- Proton/Linux testing: deferred to in-game validation

### 5.4 — Stable Release

- Merge `pipeline-survey-system` branch into `main`
- Tag a release version
- Workshop update
- This becomes the stable baseline for the v2 branch

---

## 6. Phase 3: New Major Version Branch

**When:** After stable release

Create a new branch (e.g., `v2-graphics-overhaul`) from the stable `main`. All major new features go here. This keeps the stable version available on Workshop while development continues.

---

## 7. Dust API v4: Full Pipeline Modding ABI

The v2 major version introduces API v4 — a complete rewrite of the plugin ABI that exposes the full rendering pipeline to modders. The goal: any graphics modder who wants to change anything about Kenshi's rendering can do it through Dust's API, without writing their own hooks.

Dust becomes a **graphics modding platform**, not just a post-processing framework.

### What v3 Exposes (Current)

| Capability | API |
|-----------|-----|
| Device + context | `DustFrameContext.device/context` |
| Named resources (depth, albedo, normals, hdr_rt, ldr_rt) | `GetSRV()`, `GetRTV()` |
| Injection at 5 pipeline points | `DustInjectionPoint` enum |
| Pre/post callbacks around game draws | `preExecute`, `postExecute` |
| State save/restore | `SaveState()`, `RestoreState()` |
| Shader compilation | `CompileShader()`, `CompileShaderFromFile()` |
| Fullscreen triangle | `DrawFullscreenTriangle()` |
| Scene copy | `GetSceneCopy()` |
| Constant buffer helpers | `CreateConstantBuffer()`, `UpdateConstantBuffer()` |
| Pre-fog HDR snapshot | `GetPreFogHDR()` |
| Settings/GUI/config framework | `DustSettingDesc`, flags |

v3 is a post-process API. Plugins can read the GBuffer and draw fullscreen quads. They cannot touch geometry, replace shaders, intercept draws, access lights, or modify the pipeline structure.

### What v4 Must Expose

The v4 ABI is designed so that every feature Dust implements internally (shadows, GI, materials, instancing) could theoretically be implemented as an external plugin instead. If Dust's own code needs access to something, the API should expose it too.

#### 7.1 — Geometry Access

Plugins need to read and replay the scene geometry.

```c
// Query captured draw calls from the current frame's GBuffer pass
uint32_t (*GetCapturedDrawCount)(void);
const DustCapturedDraw* (*GetCapturedDraw)(uint32_t index);

// Replay a captured draw with modified state
// Binds the original VB/IB/InputLayout/VS, but uses the provided CB override
// for the VS constant buffer (e.g., light-space WVP for shadow rendering).
// If ps is NULL, renders depth-only (no pixel shader bound).
void (*ReplayDraw)(ID3D11DeviceContext* ctx, uint32_t drawIndex,
                   ID3D11Buffer* vsConstantBufferOverride,
                   ID3D11PixelShader* ps);

// Replay all captured draws with a single CB override (batch replay)
void (*ReplayAllDraws)(ID3D11DeviceContext* ctx,
                       ID3D11Buffer* vsConstantBufferOverride,
                       ID3D11PixelShader* ps);
```

```c
// Captured draw data (read-only, valid for current frame only)
typedef struct DustCapturedDraw {
    // Geometry
    ID3D11Buffer*       vertexBuffer;
    uint32_t            vbStride;
    uint32_t            vbOffset;
    ID3D11Buffer*       indexBuffer;
    DXGI_FORMAT         indexFormat;
    uint32_t            indexCount;
    uint32_t            startIndex;
    int32_t             baseVertex;

    // Shaders (as bound during GBuffer pass)
    ID3D11VertexShader* vertexShader;
    ID3D11PixelShader*  pixelShader;
    ID3D11InputLayout*  inputLayout;

    // Transform data (extracted from VS constant buffer)
    float               worldMatrix[16];       // per-object
    float               viewProjMatrix[16];    // per-frame (same for all draws)
    float               worldViewProjMatrix[16];

    // Material classification (0 = unclassified)
    uint32_t            materialId;

    // Bounding info (computed from VB, if available)
    float               boundsMin[3];
    float               boundsMax[3];
} DustCapturedDraw;
```

#### 7.2 — Light Access

Plugins need to know what lights exist in the scene.

```c
typedef struct DustLight {
    uint32_t    type;           // DUST_LIGHT_DIRECTIONAL, POINT, SPOT
    float       position[3];    // world space (point/spot)
    float       direction[3];   // world space (directional/spot)
    float       color[3];       // linear RGB
    float       intensity;
    float       radius;         // attenuation range (point/spot)
    float       spotAngle;      // inner cone angle (spot)
    float       spotOuterAngle; // outer cone angle (spot)
} DustLight;

#define DUST_LIGHT_DIRECTIONAL 0
#define DUST_LIGHT_POINT       1
#define DUST_LIGHT_SPOT        2

uint32_t (*GetLightCount)(void);
const DustLight* (*GetLight)(uint32_t index);
const DustLight* (*GetSunLight)(void);  // shortcut for the main directional
```

#### 7.3 — Shadow Map Access

Plugins that implement custom lighting need to read (or provide) shadow data.

```c
// Read Dust's shadow maps (if Dust shadow system is active)
uint32_t (*GetShadowCascadeCount)(void);
ID3D11ShaderResourceView* (*GetShadowCascadeSRV)(uint32_t cascadeIndex);
const float* (*GetShadowMatrix)(uint32_t cascadeIndex);  // 4x4 float

// Register a plugin-provided shadow source
// Dust will use this instead of its own shadow system for the given light
void (*RegisterShadowProvider)(uint32_t lightIndex,
                               ID3D11ShaderResourceView* shadowSRV,
                               const float* shadowMatrix);
```

#### 7.4 — Camera & Scene Data

Plugins need camera and scene parameters without digging through constant buffers.

```c
typedef struct DustCamera {
    float       position[3];        // world space
    float       forward[3];         // view direction
    float       up[3];
    float       right[3];
    float       viewMatrix[16];
    float       projMatrix[16];
    float       viewProjMatrix[16];
    float       invViewMatrix[16];
    float       invProjMatrix[16];
    float       nearPlane;
    float       farPlane;
    float       fovY;               // vertical FOV in radians
    float       aspectRatio;
} DustCamera;

const DustCamera* (*GetCamera)(void);

typedef struct DustSceneInfo {
    uint32_t    width;              // render resolution
    uint32_t    height;
    uint64_t    frameIndex;
    float       deltaTime;          // seconds since last frame
    float       gameTime;           // game clock
    float       sunDirection[3];
    float       sunColor[4];        // RGB + intensity
    float       ambientColor[4];
    float       fogParams[4];       // start, end, density, etc.
    float       fogColor[4];
} DustSceneInfo;

const DustSceneInfo* (*GetSceneInfo)(void);
```

#### 7.5 — Draw Call Interception

Plugins need to filter, modify, or skip specific draw calls. This is how per-material shading, LOD overrides, and draw call optimization work.

```c
// Draw call filter callback — return behavior flags
// Called for every DrawIndexed during the GBuffer pass
typedef uint32_t (*DustDrawFilter)(const DustCapturedDraw* draw, void* userData);

#define DUST_DRAW_CONTINUE    0x00  // execute normally
#define DUST_DRAW_SKIP        0x01  // skip this draw call entirely
#define DUST_DRAW_REPLACE_PS  0x02  // use the PS set via SetDrawOverridePS

// Register a draw filter (called in registration order, first SKIP wins)
void (*RegisterDrawFilter)(DustDrawFilter filter, void* userData);

// Set a replacement pixel shader for draws flagged DUST_DRAW_REPLACE_PS
void (*SetDrawOverridePS)(ID3D11PixelShader* ps);
```

#### 7.6 — Shader Replacement

Plugins need to replace game shaders entirely — not just patch via string insertion.

```c
// Replace a game shader by its bytecode hash
// When the game creates a shader matching this hash, Dust substitutes the replacement.
// The replacement is compiled from the provided HLSL source.
// Returns a handle for later removal, or 0 on failure.
uint64_t (*RegisterShaderReplacement)(
    const char*  bytecodeHashHex,     // e.g., "3b5a62cd-b2ae32e2-ac920923-3005c021"
    const char*  hlslSource,
    const char*  entryPoint,
    const char*  target               // e.g., "ps_5_0"
);
void (*RemoveShaderReplacement)(uint64_t handle);

// Access captured shader source (from D3DCompile hook)
// Returns HLSL source or NULL if not captured
const char* (*GetShaderSource)(ID3D11PixelShader* ps);
const char* (*GetShaderSource)(ID3D11VertexShader* vs);
```

#### 7.7 — Texture Interception

Plugins need to replace or augment textures.

```c
// Replace a texture at creation time, identified by dimensions + format + hash
// Dust calls the callback when CreateTexture2D matches the signature.
// Callback returns a replacement texture or NULL to keep the original.
typedef ID3D11Texture2D* (*DustTextureReplacer)(
    ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC* originalDesc,
    void* userData
);

void (*RegisterTextureReplacer)(uint32_t width, uint32_t height,
                                DXGI_FORMAT format,
                                DustTextureReplacer replacer,
                                void* userData);
```

#### 7.8 — Render Target & Resource Creation

Plugins need to create and manage render targets, structured buffers, UAVs.

```c
// Create a render target with SRV access (framework manages lifetime on resize)
// Returns both the RTV and SRV. Automatically recreated on resolution change.
void (*CreateManagedRT)(const char* name, DXGI_FORMAT format,
                        float widthScale, float heightScale,   // 1.0 = native res
                        ID3D11RenderTargetView** outRTV,
                        ID3D11ShaderResourceView** outSRV);

// Create a depth stencil with SRV access
void (*CreateManagedDepth)(const char* name, DXGI_FORMAT format,
                           float widthScale, float heightScale,
                           ID3D11DepthStencilView** outDSV,
                           ID3D11ShaderResourceView** outSRV);

// Structured buffer + UAV (for compute shaders)
void (*CreateStructuredBuffer)(uint32_t elementSize, uint32_t elementCount,
                               const void* initialData,
                               ID3D11Buffer** outBuffer,
                               ID3D11ShaderResourceView** outSRV,
                               ID3D11UnorderedAccessView** outUAV);

// 3D texture (for voxel grids, LPV, etc.)
void (*CreateTexture3D)(uint32_t width, uint32_t height, uint32_t depth,
                        DXGI_FORMAT format, uint32_t mipLevels,
                        ID3D11Texture3D** outTex,
                        ID3D11ShaderResourceView** outSRV,
                        ID3D11UnorderedAccessView** outUAV);

// Register a named resource so other plugins can find it
void (*RegisterResource)(const char* name, ID3D11ShaderResourceView* srv);
```

#### 7.9 — Compute Shader Dispatch

Plugins need compute shader support for denoising, culling, voxelization, etc.

```c
// Compile and dispatch compute shaders
ID3D11ComputeShader* (*CompileComputeShader)(const char* hlslSource,
                                             const char* entryPoint);
void (*DispatchCompute)(ID3D11DeviceContext* ctx,
                        ID3D11ComputeShader* cs,
                        uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ);
```

#### 7.10 — Custom Injection Points

Plugins need to register new injection points beyond the 5 built-in ones, and register for per-draw callbacks — not just per-pass.

```c
// Register a custom injection point that other plugins can hook
// Returns a handle used to trigger the injection point
uint32_t (*RegisterInjectionPoint)(const char* name);

// Trigger a custom injection point (dispatches all registered callbacks)
void (*TriggerInjectionPoint)(uint32_t handle, const DustFrameContext* ctx);

// Per-draw callback (fires for every DrawIndexed, not just per-pass)
typedef void (*DustPerDrawCallback)(const DustCapturedDraw* draw,
                                    const DustFrameContext* ctx,
                                    void* userData);
void (*RegisterPerDrawCallback)(DustPerDrawCallback cb, void* userData);
```

#### 7.11 — Sampler State Control

```c
// Override sampler state globally (e.g., force aniso x16)
void (*SetGlobalSamplerOverride)(D3D11_FILTER filter,
                                  uint32_t maxAnisotropy,
                                  float mipLODBias);
void (*ClearGlobalSamplerOverride)(void);
```

#### 7.12 — Material System Access

Once per-material classification exists, plugins need to read and contribute to it.

```c
typedef struct DustMaterialInfo {
    uint32_t    id;
    const char* name;          // "skin", "metal", "terrain", etc.
    uint32_t    shaderHash;    // PS hash that identifies this material
} DustMaterialInfo;

uint32_t (*GetMaterialCount)(void);
const DustMaterialInfo* (*GetMaterial)(uint32_t id);

// Register a custom material classification rule
// When a draw matches the given PS hash + texture signature, assign this material ID
void (*RegisterMaterialRule)(uint32_t psHash, uint32_t materialId);
```

#### 7.13 — Voxel Grid Access (When Available)

If VXGI is active, plugins can read/write the voxel grid for custom effects.

```c
// Read the scene voxel grid
ID3D11ShaderResourceView* (*GetVoxelGridSRV)(void);  // 3D texture
uint32_t (*GetVoxelGridResolution)(void);             // e.g., 128 or 256
const float* (*GetVoxelGridBounds)(void);             // world-space AABB (6 floats)

// Write to the voxel grid (for plugins that want to inject custom voxel data)
ID3D11UnorderedAccessView* (*GetVoxelGridUAV)(void);
```

### ABI Design Principles

1. **Pure C ABI** — no C++ types, no virtuals, no STL. Function pointers in a versioned struct. Plugins compiled with any compiler/version can link against it.

2. **Versioned and additive** — `apiVersion` field in `DustHostAPI`. New functions are appended to the end of the struct. Old plugins see a smaller struct and work fine. New plugins check `apiVersion >= N` before calling new functions.

3. **Stable across Dust updates** — plugin DLLs don't need recompilation when Dust updates, as long as the API version is compatible. Struct layout never changes, only grows.

4. **Null-safe** — every function pointer may be NULL if the feature isn't available (e.g., `GetVoxelGridSRV` is NULL if VXGI isn't active). Plugins must check before calling.

5. **Frame-scoped data** — pointers returned by `GetCapturedDraw`, `GetLight`, `GetCamera`, `GetSceneInfo` are valid for the current frame only. Do not cache across frames.

6. **COM-clean** — the API never transfers COM ownership. SRVs/RTVs returned by the API are framework-owned. Plugins must not Release them. Plugins' own resources are their own responsibility.

7. **Thread-safe where needed** — draw filters and per-draw callbacks may fire from the game's render thread. Plugins must not block or allocate heavily in these callbacks.

### Rollout Strategy

The v4 API doesn't need to ship all at once. Each section maps to a development phase:

| API Section | Available After |
|-------------|----------------|
| 7.4 Camera & Scene Data | Phase 3A (extracted from existing CBs) |
| 7.6 Shader Replacement | Phase 3A (extends existing D3DCompile hook) |
| 7.8 Resource Creation | Phase 3A (helpers around existing D3D11 calls) |
| 7.9 Compute Dispatch | Phase 3A (already used internally by RTGI) |
| 7.11 Sampler Control | Phase 3A (new hook, trivial) |
| 7.1 Geometry Access | Phase 3B (geometry capture) |
| 7.5 Draw Interception | Phase 3B (draw call filter) |
| 7.7 Texture Interception | Phase 3B (extends CreateTexture2D hook) |
| 7.2 Light Access | Phase 3C/3E (light extraction) |
| 7.3 Shadow Map Access | Phase 3C (CSM system) |
| 7.10 Custom Injection Points | Phase 3D (extensibility) |
| 7.12 Material System | Phase 3D (material classification) |
| 7.13 Voxel Grid | Phase 3E (VXGI) |

Each phase ships the corresponding API additions. Plugins query `apiVersion` to know what's available.

---

## 8. Phase 3A: Quick Wins (Shader-Only, No Geometry)

These can be done immediately in the v2 branch, before geometry interception is built. All are shader patches or new post-process passes.

### 3A.1 — BRDF Refinement

Kenshi already uses GGX specular + Lambert diffuse (confirmed by survey). The upgrade is incremental, not a wholesale replacement:

1. **Diffuse:** Uncomment Disney/Burley diffuse (already present in `lightingFunctions.hlsl`, just commented out)
2. **Visibility term:** Replace Kelemen-Szirmay-Kalos with Smith-GGX height-correlated (more accurate at grazing angles)
3. **Multi-scatter:** Add Kulla-Conty energy compensation (prevents energy loss at high roughness)
4. **IBL:** Replace Lazarov approximation with Split-Sum (more accurate environment reflections)
5. **Light falloff:** Replace custom cubic attenuation with physical inverse-square falloff for point/spot lights

**Impact:** Incremental quality improvement — the base BRDF is already modern PBR, so gains are in edge cases (grazing angles, high roughness, rough metals). Near-zero performance cost.

### 3A.2 — SMAA Anti-Aliasing

Replace Kenshi's FXAA pass with SMAA 1x (spatial only):

- Intercept the FXAA fullscreen draw (identified by shader hash from survey)
- Skip the game's FXAA draw
- Execute SMAA edge detection → SMAA blending weight → SMAA neighborhood blending
- Requires pre-computed area/search textures (ship as assets or embed in code)

Alternatively, if the FXAA shader is compiled at runtime (through D3DCompile), patch it to use SMAA source directly.

**Impact:** Sharper edges, better subpixel coverage, no smearing.

### 3A.3 — Force Anisotropic Filtering x16

Hook `CreateSamplerState` (Device VTable index 23):
- Intercept every sampler the game creates
- Override `MaxAnisotropy = 16` and `Filter = D3D11_FILTER_ANISOTROPIC`
- Pass through to original function

**Impact:** Immediate improvement to all texture quality at oblique angles. Terrain, roads, walls all look sharper at distance. Trivial to implement.

### 3A.4 — Improved Shadow Filtering

Based on survey shadow analysis, patch the shadow sampling in the deferred shader:
- Better cascade selection (if cascade splits are suboptimal)
- Higher-quality PCF or PCSS with more samples
- Fix bias values if shadow acne/peter-panning is confirmed
- Potentially increase shadow map resolution via `CreateTexture2D` override (already have the hook)

### 3A.5 — RTGI Spatial-Only Mode

Replace RTGI's temporal accumulation with a spatial-only denoiser:
- Increase ray count per pixel (compensate for no temporal amortization)
- Use A-trous wavelet denoiser (already exists as compute shader) without history buffer
- May need wider kernel or more filter passes
- Accept higher GPU cost for clean per-frame output

---

## 9. Phase 3B: Geometry Interception Foundation

This is the critical infrastructure that enables everything from Phase 3C onward.

### 3B.1 — GBuffer Draw Identification

In `HookedDrawIndexed`, detect GBuffer-pass draws:
- Check currently bound render targets via `OMGetRenderTargets`
- GBuffer signature: 3 MRTs (B8G8R8A8 + B8G8R8A8 + R32_FLOAT) + depth stencil
- Log and verify detection is reliable across scenes

### 3B.2 — Draw Call Capture

For each GBuffer draw, capture:
```cpp
struct CapturedDraw {
    ID3D11Buffer*       vertexBuffer;     // AddRef'd
    UINT                vbStride, vbOffset;
    ID3D11Buffer*       indexBuffer;      // AddRef'd
    DXGI_FORMAT         indexFormat;
    UINT                indexCount, startIndex;
    INT                 baseVertex;
    ID3D11VertexShader* vertexShader;     // AddRef'd
    ID3D11InputLayout*  inputLayout;      // AddRef'd
    ID3D11Buffer*       vsConstantBuffers[4]; // AddRef'd
};
```

Store in a per-frame vector, cleared at frame start in `ResetFrameState()`. All COM pointers AddRef'd on capture, Released at clear.

### 3B.3 — VS Constant Buffer Reverse Engineering

Using detail level 2 survey data (CB readback):
- Read back VS constant buffers from multiple GBuffer draws in the same frame
- Identify the World-View-Projection matrix (changes per draw)
- Identify the View-Projection matrix (same for all draws in a frame)
- Identify the World matrix (per-object)
- Map CB offsets for: WVP, World, VP, camera position, any other parameters

This is a one-time reverse engineering task. Once offsets are known, they're constants.

### 3B.4 — Geometry Replay Proof of Concept

After GBuffer pass completes (detected by deferred lighting fullscreen draw):
- Create a simple R32_FLOAT depth-only render target
- Set an orthographic projection from above (bird's eye view)
- For each captured draw: bind original VB, IB, input layout, VS, but with modified CB (our projection)
- Set null pixel shader (depth-only rendering)
- Execute `DrawIndexed`
- Verify output matches expected geometry (screenshot comparison)

If this works, the geometry capture pipeline is proven and everything from Phase 3C onward is unlocked.

---

## 10. Phase 3C: Shadow System Replacement

### 3C.1 — Custom Cascaded Shadow Maps

Using geometry replay:
- Extract sun direction from deferred lighting CB0 (already mapped: offset c0)
- Compute sun view matrix from sun direction
- Choose cascade split distances (logarithmic or practical split scheme)
- For each cascade (3-4):
  - Compute tight orthographic projection around the camera frustum slice
  - Snap projection to shadow texel grid (eliminates shimmer)
  - Replay captured geometry with light-space WVP in the VS constant buffer
  - Render into a per-cascade R32_FLOAT depth texture
  - Frustum cull draws per cascade (only replay geometry within the cascade bounds)

### 3C.2 — CSM Integration

- Bind custom CSM textures to available SRV slots in the deferred pass
- Patch the deferred shader via D3DCompile to:
  - Declare CSM samplers and matrices
  - Replace the existing shadow sampling with CSM cascade selection + PCF/PCSS
  - Cascade blending at split boundaries
  - Normal-offset bias for acne prevention

### 3C.3 — Shadow Quality

- Per-cascade resolution control (e.g., 2048 near, 1024 mid, 512 far)
- Contact-hardening soft shadows (PCSS — penumbra widens with blocker distance)
- Configurable via GUI: cascade distances, resolution, filter quality, bias

---

## 11. Phase 3D: Per-Material Shading

### 3D.1 — Draw Call Classification

Using survey data + geometry capture:
- Fingerprint each draw by: VS hash + PS hash + bound texture set
- Build a material classification table:
  - Texture hash + shader signature → material type (skin, metal, cloth, terrain, foliage, stone, etc.)
  - May require initial manual annotation, with heuristics to automate later
  - Save classifications to file so they persist across launches

### 3D.2 — Material ID in GBuffer

- Extend the GBuffer with a material ID:
  - ~~Option A: use unused bit range in RT0.a or RT1.a~~ — NOT viable. RT0 uses all 4 channels for YCoCg chroma-subsampled color. RT1.a encodes emissive/translucency. No spare bits.
  - Option B: use the stencil buffer (D24_UNORM_S8_UINT) — 8-bit stencil per pixel, supports up to 255 material classes. Patch GBuffer shaders to write stencil value via OMSetDepthStencilState per draw call.
  - Option C: add a separate R8 render target (requires hooking OMSetRenderTargets to add a 4th MRT during GBuffer fill)
- Write material class ID during GBuffer fill based on draw call classification

### 3D.3 — Per-Material BRDF

Patch the deferred lighting shader to branch on material ID:
- **Skin:** Subsurface scattering (screen-space diffusion, depth-based thickness)
- **Hair:** Anisotropic specular (Kajiya-Kay or Marschner)
- **Cloth:** Sheen/velvet model (Ashikhmin or Charlie)
- **Metal:** Full metallic workflow (colored F0, no diffuse, GGX specular)
- **Terrain/Stone:** High roughness, Oren-Nayar diffuse
- **Foliage:** Translucency (back-lighting based on leaf thickness)
- **Default:** Disney/Burley diffuse + GGX specular

---

## 12. Phase 3E: World-Space GI & Unlimited Light Shadows

### 3E.1 — Scene Voxelization

Using geometry replay:
- Voxelize the scene into a 3D texture (128³ or 256³)
- Each voxel stores: albedo, normal, opacity, emissive
- Render captured geometry from 3 orthogonal projections (X, Y, Z) into the voxel grid
- Use conservative rasterization if available (D3D11.3), or geometry shader emulation
- Update every frame (dynamic lights, moving objects)

### 3E.2 — VXGI Cone Tracing

- For each pixel in the deferred pass:
  - Trace multiple cones through the voxel grid in the hemisphere around the normal
  - Sample lower mip levels of the voxel texture for wider cones (approximates area light)
  - Accumulate indirect diffuse and specular lighting
- Produces inherently smooth output — no denoising needed
- Handles multi-bounce (re-inject first-bounce result into voxels, trace again)

### 3E.3 — Voxel-Based Shadows for All Lights

The same voxel grid enables unlimited light shadows:
- For each light source: trace through the voxel grid from surface point toward light
- If a voxel is opaque along the ray, the point is in shadow
- Cost is per-pixel, not per-light — no shadow maps needed
- Naturally produces soft shadows (sample density in the voxel grid)
- Works for sun, point lights, spot lights, area lights — any light type
- Light data extracted from game constant buffers (Phase 1 mapping)

### 3E.4 — Light Tracking

- Each frame, extract all light positions/colors/radii from constant buffers
- Track lights across frames by position + radius + color
- Handle dynamic lights: player-placed torches, campfires, moving lanterns
- No budget, no cap — every light that exists in the game data casts shadows and contributes to GI

### 3E.5 — Alternative: RSM + LPV

If VXGI proves too expensive:
- Reflective Shadow Maps: render scene from each light, store position + normal + flux
- Light Propagation Volumes: inject RSM data into a 3D grid, iteratively propagate
- Lower cost than VXGI, still world-space, still handles multiple lights
- Smooth output, no temporal denoising needed

---

## 13. Phase 3F: Performance Optimizations

### 3F.1 — Draw Call Instancing

Using geometry capture data:
- Identify repeated mesh draws (same VB + IB + VS, different world matrix only)
- Collect world matrices into an instance buffer
- Replace N individual DrawIndexed calls with 1 DrawIndexedInstanced call
- Requires patching the VS to read from an instance buffer (or using a structured buffer + SV_InstanceID)

Expected impact: 10-50x draw call reduction in settlement/city scenes.

### 3F.2 — Redundant State Filtering

- Track currently bound state (shaders, textures, samplers, blend/depth/rasterizer state)
- In hooked Set* calls, compare new state to current — skip if identical
- Zero visual impact, pure CPU savings

### 3F.3 — GPU-Driven Rendering (Advanced)

- Upload all mesh data + transforms to persistent GPU buffers
- Compute shader performs frustum culling on GPU
- Output DrawIndexedInstancedIndirect argument buffer
- One indirect draw call renders everything
- CPU submits a handful of calls regardless of scene complexity

### 3F.4 — Shader Cache Optimization

- After first launch: RE_Kenshi caches our patched bytecode
- Subsequent launches skip D3DCompile entirely
- Only invalidate cache when Dust version or shader config changes
- Stamp file tracks cache validity

---

## 14. Phase 3G: DXR Ray Tracing (Endgame)

**Prerequisite:** Geometry capture pipeline (Phase 3B), RTX-capable GPU

### 3G.1 — D3D12 Interop

- Create D3D12 device alongside D3D11 via `ID3D11On12Device`
- Share resources between APIs (depth buffer, GBuffer textures, output RT)
- Synchronize work between D3D11 and D3D12 contexts

### 3G.2 — BVH Construction

- Build Bottom-Level Acceleration Structures (BLAS) from captured vertex/index buffers
- Build Top-Level Acceleration Structure (TLAS) from per-object transforms
- Update TLAS every frame (transforms change), rebuild BLAS when geometry changes

### 3G.3 — DXR Effects

- **Ray-traced shadows** — one shadow ray per light per pixel. Pixel-perfect, any number of lights, no resolution limits.
- **Ray-traced reflections** — trace reflection ray, hit actual geometry. No screen-space limitations.
- **Ray-traced GI** — multi-bounce path tracing with spatial denoising. Ground-truth quality.
- **Ray-traced AO** — trace short rays in hemisphere, test actual geometry. No screen-space approximation.

Gate behind hardware capability check. Provide VXGI/rasterized fallback for non-RTX hardware.

---

## 15. Distribution & Compatibility

### RE_Kenshi Plugin

Dust continues to load as a RE_Kenshi plugin. The loading mechanism is unchanged:
- RE_Kenshi calls `?startPlugin@@YAXXZ` (C++ mangled export)
- Dust installs D3D11 hooks via KenshiLib
- Steam Workshop subscription is the only install step

No new dependencies, no separate launcher, no manual file placement.

### Compatibility Requirements

- Must work with Steam overlay active
- Must work with Discord overlay active
- Must work with ReShade active (deferred Present hook handles this)
- Must work on Proton/Linux (Steam Deck)
- Must work with any Kenshi mods (the survey system helps verify this)
- Must not require the user to disable other overlays

### Shader Cache Compatibility

After the shader cache fix (Phase 2), RE_Kenshi's cache works WITH Dust instead of being invalidated:
- First launch or version change: cache miss → D3DCompile fires → Dust patches → RE_Kenshi caches patched bytecode
- Subsequent launches: cache hit → patched bytecode loads directly → fast startup

### Workshop Preset Distribution

Presets should be distributable as standalone Workshop items. A user subscribes to a preset mod and it appears in Dust's preset dropdown automatically.

#### Security Model

**Presets are data, never code.** This is the hard rule.

| Source | Can provide | Cannot provide |
|--------|-----------|---------------|
| `mods/Dust/effects/` (Dust's own folder) | Effect DLLs + local presets | — |
| External mod folders (Workshop or manual) | INI preset files only | DLLs, executables, anything that runs code |

Even if someone puts a DLL in their "preset" mod, Dust never loads it. The scan function only reads `.ini` files and the one `.json` marker. No `LoadLibrary`, no `CreateProcess`, no code execution from external sources.

#### Preset Mod Structure

A Workshop preset mod contains:

```
mods/DustPreset_Cinematic/         (or workshop/content/233860/<id>/)
    dust_preset.json               ← marker file (metadata)
    ssao.ini                       ← per-effect settings
    rtgi.ini
    lut.ini
    bloom.ini
    clarity.ini
    ...
```

The marker file:
```json
{
    "type": "dust_preset",
    "name": "Cinematic Desert",
    "author": "SomeModder",
    "description": "Warm tones, heavy bloom, soft shadows",
    "dustVersionMin": "2.0"
}
```

#### Discovery

Mods can be in two locations:
- `<gameDir>/mods/<modname>/` — manual install
- `<steamLibrary>/workshop/content/233860/<workshopid>/` — Workshop subscription

At startup, Dust scans both paths:
1. `<gameDir>/mods/*/dust_preset.json`
2. `<gameDir>/../../workshop/content/233860/*/dust_preset.json`

The second path works because the game is at `<steam>/common/Kenshi/` and Workshop content is at `<steam>/workshop/content/<appid>/` — two levels up reaches the Steam library root. Same relative path structure on Windows, Linux/Proton.

If RE_Kenshi ever exposes a mod list with paths, use that instead of scanning directories — it already resolves both locations.

#### Version Compatibility

The `dustVersionMin` field lets presets declare what version of Dust they target. If a preset references settings from a newer version (unknown effect names, unrecognized INI keys), Dust warns the user via the existing `ValidatePreset()` system rather than silently applying broken values.

#### GUI Integration

External presets appear in the preset dropdown alongside local presets, with an indicator showing the source (local vs. Workshop). Users can:
- Apply any preset (local or external)
- Save current settings over a local preset
- Cannot modify external presets (read-only — the files live in the Workshop folder, which Steam manages)
- "Save As" copies an external preset to local, where it becomes editable

#### Backport Note

Workshop preset discovery, shader cache compatibility (Phase 2), and aniso x16 forcing (Phase 3A.3) are independent of the v4 API and geometry interception. These will be backported to the current stable version as they are completed, rather than waiting for the full v2 release.

---

## 16. Graphics Quality Guidelines Reference

Full document: `docs/graphics_quality_guidelines.md`

Quick reference:

| | Do | Don't |
|---|---|---|
| AA | SMAA 1x, FXAA Quality 39 | TAA, DLAA, any temporal AA |
| Resolution | Native, selective supersampling | Upscaling, DLSS/FSR |
| Denoising | Spatial bilateral/wavelet | Temporal accumulation |
| Textures | Aniso x16, quality mip gen | Default trilinear |
| Lighting | Disney/Oren-Nayar diffuse, GGX specular | Lambert, Blinn-Phong |
| Shadows | Stable projection, PCSS, voxel/SDF for multi-light | Temporal filtering, light budget caps |
| GI | VXGI, RSM+LPV | Screen-space with temporal convergence |

---

## 17. Open Questions — All Answered by Survey

1. **What BRDF does the deferred shader actually use?** ✅ GGX specular (Trowbridge-Reitz NDF, Schlick Fresnel, Kelemen-SzK visibility) + Lambert diffuse with Fresnel energy conservation. Disney/Burley diffuse is present but commented out. NOT Blinn-Phong.

2. **Does roughness/metalness reach the GBuffer?** ✅ No. GBuffer uses YCoCg chroma subsampling — RT0 encodes color data across all 4 channels, not separate albedo+roughness. RT1.a encodes emissive/translucency. Roughness flows through per-pixel shader math but is not recoverable from the GBuffer as a separate channel.

3. **How are point lights passed to the GPU?** ✅ Forward-rendered light volumes. Each point/spot light is drawn as a sphere/cone with additive blending after the deferred sun pass. Separate draw call per light with its own constant buffer. Uses the same `light_fs` pixel shader as the deferred sun but with different CB data.

4. **What's actually wrong with the shadows?** ✅ CSM with 4 cascades in 4096x4096 atlas. 12-sample hex PCF filtering. Issues are: limited resolution per cascade (1024x1024 effective per cascade in a 4096 atlas), no contact-hardening (fixed filter width), possible cascade split distance issues. Shadow VS CB0 is 64 bytes (3x3 light view matrix + translation).

5. **How bad is the draw call overhead?** ✅ 3763 draw calls in dense scenes (Shark capture), 582 in sparse (Ashlands). GBuffer pass: 1177 draws. Shadow pass: ~1050 draws. Cubemap: up to 1400 draws (full scene re-render at 512x512). Average across 33 captures: 1545 draws. Instancing analysis shows significant potential reduction.

6. **What does the VS constant buffer layout look like?** ✅ GBuffer VS CB0 (160 bytes): c0=per-object material params, c1-c4=viewProjection matrix, c5=camera world position, c6-c9=per-object data. Deferred VS CB0 (32 bytes): c0-c1=far frustum corners + near plane.

7. **Are there any surprises in the pipeline?** ✅ Yes — several: (a) YCoCg chroma subsampling in GBuffer (not standard MRT layout), (b) GBuffer repack pass converts to R16G16B16A16_FLOAT before deferred lighting, (c) cubemap pass re-renders the entire scene (1400 draws in dense captures), (d) heat haze renders previous-frame geometry at the START of the frame (draws 0-6), (e) 492 unique shaders mapped to 50 source files with 0 unidentified.
