#pragma once

#include <d3d11.h>
#include <cstdint>

// FSR2 native-D3D11 upscaling backend (optiscaler/FidelityFX-FSR2-DX11) — the universal-GPU alternative
// to DLSS. Signatures mirror the Upscaler (DLSS) namespace so the resolve pipeline can branch on a
// backend enum without threading extra params. Compiles to no-op stubs when the SDK isn't vendored.
namespace UpscalerFSR2
{
    bool Init(ID3D11Device* device);
    bool IsAvailable();   // FSR2 is compute-only — available on any D3D11 device once Init succeeds.

    // preset is ignored (FSR2 has no model presets); kept for signature parity with the DLSS backend.
    bool CreateFeature(ID3D11DeviceContext* ctx, uint32_t renderW, uint32_t renderH,
                       uint32_t displayW, uint32_t displayH, bool isHDR, bool depthInverted, int preset);

    bool Evaluate(ID3D11DeviceContext* ctx,
                  ID3D11Resource* color, ID3D11Resource* depth, ID3D11Resource* motionVectors,
                  ID3D11Resource* output, float jitterX, float jitterY,
                  float mvScaleX, float mvScaleY, float sharpness, bool reset,
                  ID3D11Resource* reactiveMask);

    void Shutdown();
}
