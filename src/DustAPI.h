// DustAPI.h - Stable C API for Dust effect plugins.
// This is the only header effect authors need to include.
// Do NOT use C++ types or virtuals here - pure C for ABI stability.

#ifndef DUST_API_H
#define DUST_API_H

#include <d3d11.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DUST_API_VERSION 1

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

// Host API - function pointers provided by Dust to plugins
typedef struct DustHostAPI {
    uint32_t apiVersion;

    // Resource access (names: "depth", "hdr_rt")
    ID3D11ShaderResourceView* (*GetSRV)(const char* name);
    ID3D11RenderTargetView*   (*GetRTV)(const char* name);

    // Logging (printf-style)
    void (*Log)(const char* fmt, ...);

    // GPU state save/restore (use around custom rendering passes)
    void (*SaveState)(ID3D11DeviceContext* ctx);
    void (*RestoreState)(ID3D11DeviceContext* ctx);
} DustHostAPI;

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
} DustEffectDesc;

// Every effect DLL must export this function.
// Returns 0 on success, non-zero on failure.
typedef int (*PFN_DustEffectCreate)(DustEffectDesc* desc);

#ifdef __cplusplus
}
#endif

#endif // DUST_API_H
