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

    // Swap the lighting RT to an MSAA version for per-sample shading.
    // Returns true if the swap was successful and EndPerSampleDraw must be called.
    bool BeginPerSampleDraw(ID3D11DeviceContext* ctx);

    // Resolve the MSAA lighting RT back to the original single-sample RT.
    void EndPerSampleDraw(ID3D11DeviceContext* ctx);

    void SetDebugMode(int mode);
    int GetDebugMode();

    uint32_t GetSampleCount();

    void Shutdown();
}
