#pragma once

#include <d3d11.h>
#include <cstdint>

namespace DeferredMSAA
{
    void SetDevice(ID3D11Device* device);

    // Bind MSAA GBuffer SRVs at t10-t12 and config CB at b3 before the deferred draw.
    void BindForLighting(ID3D11DeviceContext* ctx);

    // Unbind MSAA SRVs after the deferred draw.
    void UnbindAfterLighting(ID3D11DeviceContext* ctx);

    void SetEdgeThreshold(float threshold);
    float GetEdgeThreshold();

    void SetDebugMode(int mode);
    int GetDebugMode();

    uint32_t GetSampleCount();

    void Shutdown();
}
