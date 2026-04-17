// DustAPI.h - Stable C API for Dust effect plugins.
// This is the only header effect authors need to include.
// Do NOT use C++ types or virtuals here - pure C for ABI stability.
//
// === Minimal v3 Post-Process Effect Example ===
//
//   #include "DustAPI.h"
//   #include <d3d11.h>
//
//   static const char* PS = R"(
//   Texture2D sceneTex : register(t0);
//   SamplerState samp : register(s0);
//   cbuffer P : register(b0) { float strength; float3 pad; };
//   float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
//       float3 c = sceneTex.SampleLevel(samp, uv, 0).rgb;
//       float luma = dot(c, float3(0.299, 0.587, 0.114));
//       return float4(lerp(c, luma.xxx, strength), 1);
//   })";
//
//   static float gStrength = 0.5f;
//   static bool gEnabled = true;
//   static ID3D11PixelShader* gPS = NULL;
//   static ID3D11Buffer* gCB = NULL;
//
//   static DustSettingDesc gSettings[] = {
//       { "Enabled",  DUST_SETTING_BOOL,  &gEnabled,  0, 1, "Enabled" },
//       { "Strength", DUST_SETTING_FLOAT, &gStrength, 0, 1, "Strength" },
//   };
//
//   static int MyInit(ID3D11Device* dev, uint32_t w, uint32_t h, const DustHostAPI* host) {
//       ID3DBlob* blob = host->CompileShader(PS, "main", "ps_5_0");
//       if (!blob) return -1;
//       dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &gPS);
//       blob->Release();
//       gCB = host->CreateConstantBuffer(dev, 16);
//       return 0;
//   }
//
//   static void MyPost(const DustFrameContext* ctx, const DustHostAPI* host) {
//       if (!gEnabled) return;
//       ID3D11ShaderResourceView* scene = host->GetSceneCopy(ctx->context, "ldr_rt");
//       if (!scene) return;
//       host->SaveState(ctx->context);
//       host->UpdateConstantBuffer(ctx->context, gCB, &gStrength, 4);
//       // ... bind scene SRV, CB, sampler, set RTV + viewport ...
//       host->DrawFullscreenTriangle(ctx->context, gPS);
//       host->RestoreState(ctx->context);
//   }
//
//   extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc) {
//       memset(desc, 0, sizeof(*desc));
//       desc->apiVersion     = DUST_API_VERSION;
//       desc->name           = "Desaturate";
//       desc->injectionPoint = DUST_INJECT_POST_TONEMAP;
//       desc->flags          = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
//       desc->Init           = MyInit;
//       desc->postExecute    = MyPost;
//       desc->IsEnabled      = []() -> int { return 1; };
//       desc->settings       = gSettings;
//       desc->settingCount   = 2;
//       return 0;
//   }

#ifndef DUST_API_H
#define DUST_API_H

#include <d3d11.h>
#include <d3dcommon.h>  // ID3DBlob (ID3D10Blob)
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DUST_API_VERSION 3

// Injection points in the rendering pipeline
typedef enum DustInjectionPoint {
    DUST_INJECT_POST_GBUFFER  = 0,
    DUST_INJECT_POST_LIGHTING = 1,
    DUST_INJECT_POST_FOG      = 2,
    DUST_INJECT_POST_TONEMAP  = 3,
    DUST_INJECT_PRE_PRESENT   = 4
} DustInjectionPoint;

// Callback timing relative to the game's draw call
typedef enum DustCallbackTiming {
    DUST_TIMING_PRE  = 0,   // Before the game's draw call
    DUST_TIMING_POST = 1    // After the game's draw call
} DustCallbackTiming;

// Per-frame context passed to effect callbacks
typedef struct DustFrameContext {
    ID3D11Device*           device;
    ID3D11DeviceContext*    context;
    DustInjectionPoint      point;
    DustCallbackTiming      timing;
    uint32_t                width;
    uint32_t                height;
    uint64_t                frameIndex;
} DustFrameContext;

// Resource name constants for GetSRV / GetRTV / GetSceneCopy
#define DUST_RESOURCE_DEPTH     "depth"
#define DUST_RESOURCE_ALBEDO    "albedo"
#define DUST_RESOURCE_NORMALS   "normals"
#define DUST_RESOURCE_HDR_RT    "hdr_rt"
#define DUST_RESOURCE_LDR_RT    "ldr_rt"

// Host API - function pointers provided by Dust to plugins
typedef struct DustHostAPI {
    uint32_t apiVersion;

    // Resource access (use DUST_RESOURCE_* constants)
    ID3D11ShaderResourceView* (*GetSRV)(const char* name);
    ID3D11RenderTargetView*   (*GetRTV)(const char* name);

    // Logging (printf-style)
    void (*Log)(const char* fmt, ...);

    // GPU state save/restore (use around custom rendering passes)
    void (*SaveState)(ID3D11DeviceContext* ctx);
    void (*RestoreState)(ID3D11DeviceContext* ctx);

    // Bind/unbind an SRV+sampler to a named shader register.
    // The framework resolves the actual D3D11 slot based on the current
    // shader variant (e.g. shadow mode changes register layout).
    // 'baseSlot' is the register declared in the shader (e.g. 8 for register(s8)).
    void (*BindSRV)(ID3D11DeviceContext* ctx, uint32_t baseSlot,
                    ID3D11ShaderResourceView* srv, ID3D11SamplerState* sampler);
    void (*UnbindSRV)(ID3D11DeviceContext* ctx, uint32_t baseSlot);

    // === API v3 additions ===

    // Shader compilation — plugins no longer need to link d3dcompiler.lib.
    // Returns ID3DBlob* on success (caller must Release), NULL on failure.
    ID3DBlob* (*CompileShader)(const char* hlslSource, const char* entryPoint, const char* target);
    ID3DBlob* (*CompileShaderFromFile)(const char* filePath, const char* entryPoint, const char* target);

    // Fullscreen draw helper — framework owns the VS and IA setup.
    // Sets topology, null input layout, framework's fullscreen VS, the given PS, draws 3 vertices.
    void (*DrawFullscreenTriangle)(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps);

    // Scene copy — copies the named RTV to a framework-managed texture.
    // Returns an SRV to the copy. Texture is reused within the same frame.
    // Recreated automatically on resolution change.
    ID3D11ShaderResourceView* (*GetSceneCopy)(ID3D11DeviceContext* ctx, const char* rtvName);

    // Constant buffer helpers
    ID3D11Buffer* (*CreateConstantBuffer)(ID3D11Device* device, uint32_t sizeBytes);
    void (*UpdateConstantBuffer)(ID3D11DeviceContext* ctx, ID3D11Buffer* cb,
                                 const void* data, uint32_t sizeBytes);

    // Pre-fog HDR snapshot — returns the HDR scene as it was at POST_LIGHTING,
    // before fog/atmosphere was drawn. Available from POST_FOG and POST_TONEMAP.
    // Returns NULL if POST_LIGHTING hasn't fired yet this frame.
    ID3D11ShaderResourceView* (*GetPreFogHDR)(void);
} DustHostAPI;

// Setting types for the GUI settings descriptor (API v2+)
typedef enum DustSettingType {
    DUST_SETTING_BOOL  = 0,
    DUST_SETTING_FLOAT = 1,
    DUST_SETTING_INT   = 2,
    // Hidden types (API v3+): persisted in INI but not shown in GUI
    DUST_SETTING_HIDDEN_FLOAT = 3,
    DUST_SETTING_HIDDEN_INT   = 4,
    DUST_SETTING_HIDDEN_BOOL  = 5,
    // v3.1: rich GUI types
    DUST_SETTING_ENUM    = 6,   // int-backed dropdown; uses enumLabels (NULL-terminated)
    DUST_SETTING_COLOR3  = 7,   // float[3]-backed RGB color picker; minVal/maxVal clamp channels
    DUST_SETTING_SECTION = 8    // visual-only collapsing header; name is the title, valuePtr unused
} DustSettingType;

// Describes a single configurable setting exposed to the host GUI
typedef struct DustSettingDesc {
    const char*     name;       // Display name (also the header text for DUST_SETTING_SECTION)
    DustSettingType type;       // DUST_SETTING_BOOL, FLOAT, or INT (or HIDDEN_/ENUM/COLOR3/SECTION)
    void*           valuePtr;   // Pointer to the setting's storage (bool*/float*/int*/float[3]*); NULL for SECTION
    float           minVal;     // Min value (for FLOAT/INT/COLOR3 clamp; ignored otherwise)
    float           maxVal;     // Max value (for FLOAT/INT/COLOR3 clamp; ignored otherwise)
    const char*     iniKey;     // INI key name (API v3+, NULL = use display name; ignored for SECTION)
    // v3.1 additions — zero-initialized for older settings
    const char* const* enumLabels;  // NULL-terminated array of labels (DUST_SETTING_ENUM only)
} DustSettingDesc;

// Effect descriptor flags (API v3+)
#define DUST_FLAG_FRAMEWORK_CONFIG  1   // Framework handles INI load/save/hot-reload from settings array
#define DUST_FLAG_FRAMEWORK_TIMING  2   // Framework handles GPU timestamp queries automatically

// Effect callback signature
typedef void (*DustEffectCallback)(const DustFrameContext* ctx, const DustHostAPI* host);

// Effect descriptor - filled by the plugin's DustEffectCreate function
typedef struct DustEffectDesc {
    uint32_t            apiVersion;
    const char*         name;
    DustInjectionPoint  injectionPoint;

    // Lifecycle
    int  (*Init)(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host);
    void (*Shutdown)(void);
    void (*OnResolutionChanged)(ID3D11Device* device, uint32_t w, uint32_t h);

    // Per-frame callbacks (either or both may be NULL)
    DustEffectCallback  preExecute;     // Before the game's draw
    DustEffectCallback  postExecute;    // After the game's draw

    // Query whether this effect is active
    int (*IsEnabled)(void);

    // GUI settings descriptor (API v2+)
    // Host auto-generates UI widgets from this array.
    DustSettingDesc*    settings;        // Array of setting descriptors (NULL if none)
    uint32_t            settingCount;    // Number of entries in settings array
    void (*OnSettingChanged)(void);      // Called by host after modifying a value via GUI (for runtime updates like LUT regen)
    void (*SaveSettings)(void);          // Called when user clicks Save — write current values to disk
    void (*LoadSettings)(void);          // Called when user clicks Reset All — reload values from disk

    // Performance metrics (API v2+)
    // Plugin sets this to point to its own float that it updates each frame.
    // Host reads through the pointer for the performance display.
    // Ignored for v3 plugins with DUST_FLAG_FRAMEWORK_TIMING.
    const float*        gpuTimeMsPtr;    // Pointer to plugin's GPU time in ms (NULL if not measured)

    // === API v3 additions ===
    uint32_t            flags;           // DUST_FLAG_* bitmask (0 for v2 behavior)
    const char*         configSection;   // INI section name (NULL = use effect name)
    const char*         _effectDir;      // Set by framework after DustEffectCreate — DLL directory (read-only)
    int32_t             priority;        // Dispatch order within same injection point (lower = earlier, default 0)
} DustEffectDesc;

// Every effect DLL must export this function.
// Returns 0 on success, non-zero on failure.
typedef int (*PFN_DustEffectCreate)(DustEffectDesc* desc);

#ifdef __cplusplus
}
#endif

#endif // DUST_API_H
