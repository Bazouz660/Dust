#pragma once

#include <d3d11.h>
#include <cstdint>

namespace DeferredMSAA
{
    void SetDevice(ID3D11Device* device);
    void SetResolution(UINT width, UINT height);

    // Capture deferred lighting CB data (call at POST_LIGHTING, before the draw).
    void CaptureLightingCB(ID3D11DeviceContext* ctx);

    // Run edge correction pass (call after vanilla deferred lighting draw).
    void Execute(ID3D11DeviceContext* ctx);

    void SetEdgeThreshold(float threshold);
    float GetEdgeThreshold();

    // Debug modes: 0=normal, 1=skip (passthrough), 2=green edges, 3=albedo at edges, 4=ratio vis
    void SetDebugMode(int mode);
    int GetDebugMode();

    void Shutdown();
}
