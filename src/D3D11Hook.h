#pragma once

#include <d3d11.h>

namespace D3D11Hook
{
    // Discover D3D11 function addresses using a temporary device, then install
    // hooks via KenshiLib::AddHook. The real device/context are captured from
    // the first hooked call's `this` pointer — no OGRE ABI dependency.
    bool Install();

    // The real device/context, captured from hook this pointers
    extern ID3D11Device* gDevice;
    extern ID3D11DeviceContext* gContext;
    extern bool gDeviceCaptured;

    // Reset per-frame state (call at frame start)
    void ResetFrameState();

    // Present hook diagnostics and recovery
    bool IsPresentHooked();     // true if Present hook has actually fired
    void TryRecoverPresent();   // re-attempt swap chain discovery + hook

    // Signal all hooks to pass through to originals (game shutting down).
    // KenshiLib trampolines can't be removed, so any in-flight call after
    // we begin teardown must skip our logic to avoid touching freed state.
    void SignalShutdown();
    bool IsShutdownSignaled();

    // Plugin-driven override for the shadow atlas resolution. 0 disables the
    // override (game default used). The game's atlas is always created at
    // vanilla size; when the override differs, replacement textures are
    // created and their views swapped in at bind time starting next frame.
    void SetShadowAtlasResolution(UINT size);
    UINT GetShadowAtlasResolution();
    // Vanilla atlas size the game requested (0 until the atlas exists).
    UINT GetShadowBaseResolution();

    // Direct-light AO for point/spot lights. ShaderPatch calls this after it compiles a
    // patched light_fs blob, so the CreatePixelShader hook can recognize the resulting
    // pixel shader and feed it the SSAO map at s8/s9 during its (light-volume) draws.
    void NoteLightVolumeShaderBytecode(const void* bytecode, size_t len);

    // RTGI publishes its per-light AO texture here each frame (via the SetLightVolumeAoTexture
    // host API). Bound at t10 for light_fs; null → white fallback (no-op). Cleared per frame.
    void SetLightVolumeAoTexture(ID3D11ShaderResourceView* srv);

    // SSAO publishes its AO map + params here each frame (via SetLightVolumeSsaoAo). Bound at
    // t8/t9 for light_fs, preferred over the deferred-slot snapshot. Cleared per frame.
    void SetLightVolumeSsaoAo(ID3D11ShaderResourceView* ao, ID3D11ShaderResourceView* params);
}
