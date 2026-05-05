#pragma once

#include <d3d11.h>

// Owns the POM parameter constant buffer bound at b8 in the patched objects PS.
// The host (Dust.dll) manages the cbuffer and binds it on GBuffer pass entry —
// the POM injection in ShaderPatch reads parameters from this slot. Plugins
// (effects/pom) push parameter values via the host API.
namespace POMState
{
    // Cache the device pointer (called from TryCaptureDevice). The cbuffer is
    // created lazily on first OnGBufferEnter so we don't depend on init order.
    void SetDevice(ID3D11Device* device);

    // Release the cbuffer.
    void Shutdown();

    // Setters — exposed to plugins via DustHostAPI v5.
    void SetEnabled(bool enabled);
    void SetHeightScale(float scale);
    void SetThreshold(float threshold);
    void SetThresholdWidth(float width);
    void SetMinSamples(int n);
    void SetMaxSamples(int n);

    // Hooks — called from D3D11Hook on GBuffer pass transitions detected in
    // HookedOMSetRenderTargets. Bind/unbind the cbuffer at PS slot 8.
    void OnGBufferEnter(ID3D11DeviceContext* ctx);
    void OnGBufferLeave(ID3D11DeviceContext* ctx);
}
