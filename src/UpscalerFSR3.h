#pragma once

#include <d3d11.h>
#include <cstdint>

// FSR 3.1 / FSR 4 upscaling backend via the FidelityFX ffx-api (DX12). FSR3/FSR4 have no D3D11 backend,
// so this runs the FSR upscaler on the D3D12 side-device (see D3D12Interop): it marshals the game's
// color / depth / motion-vector textures across the shared-texture bridge, dispatches FSR on the D3D12
// queue, and copies the upscaled result back. On RDNA4 the same dispatch auto-upgrades to FSR 4 (D.2).
//
// Signatures mirror the Upscaler (DLSS) / UpscalerFSR2 namespaces so RunUpscaler can branch on a backend
// enum. Compiles to safe no-op stubs (IsAvailable() == false) when the SDK isn't vendored (DUST_HAVE_FSR3
// undefined) or the D3D12 side-device / ffx-api DLLs are unavailable.
namespace UpscalerFSR3
{
    // One-time init: bring up the D3D12 side-device if needed, then LoadLibrary the ffx-api loader +
    // upscaler DLLs from modDir (where build deploys amd_fidelityfx_{loader,upscaler}_dx12.dll). Returns
    // false and leaves IsAvailable() false on any failure. Safe to call repeatedly.
    bool Init(ID3D11Device* device, const wchar_t* modDir);
    bool IsAvailable();

    // (Re)create the FSR upscaler context for a render->display size. renderW==displayW for DLAA. isHDR:
    // color is linear/HDR (pre-tonemap). depthInverted: reversed-Z depth. preset is ignored (FSR has no
    // model presets) — kept for signature parity with the DLSS backend.
    bool CreateFeature(ID3D11DeviceContext* ctx, uint32_t renderW, uint32_t renderH,
                       uint32_t displayW, uint32_t displayH, bool isHDR, bool depthInverted, int preset);

    // Run FSR for one frame. color/depth/motionVectors are render-res game textures; output is the
    // display-res game texture the result is written into. jitterXY = the sub-pixel jitter applied this
    // frame (pixels). mvScaleXY converts the MV texel value into render pixels (sign tuned like DLSS).
    // reset = discard temporal history. reactiveMask is currently ignored.
    bool Evaluate(ID3D11DeviceContext* ctx,
                  ID3D11Resource* color, ID3D11Resource* depth, ID3D11Resource* motionVectors,
                  ID3D11Resource* output, float jitterX, float jitterY,
                  float mvScaleX, float mvScaleY, float sharpness, bool reset,
                  ID3D11Resource* reactiveMask);

    void Shutdown();
}
