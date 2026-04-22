#pragma once

#include <d3d11.h>
#include <cstdint>

namespace MSAARedirect
{
    void SetEnabled(uint32_t sampleCount);
    uint32_t GetSampleCount();
    bool IsActive();

    void SetDevice(ID3D11Device* device);
    void SetResolution(UINT width, UINT height);

    // Called from OMSetRenderTargets hook when entering GBuffer pass.
    // Stores originals, returns true if MSAA targets should replace them.
    bool OnGBufferEnter(ID3D11RenderTargetView* const* origRTVs,
                        ID3D11DepthStencilView* origDSV,
                        ID3D11RenderTargetView** outMSAARTVs,
                        ID3D11DepthStencilView** outMSAADSV);

    // Called when GBuffer pass ends — resolves MSAA to original targets.
    void OnGBufferLeave(ID3D11DeviceContext* ctx);

    void Shutdown();
}
