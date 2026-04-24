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
    // The context is needed to clear the MSAA targets (OGRE clears via
    // original RTV handles, so our MSAA targets would never get cleared).
    bool OnGBufferEnter(ID3D11DeviceContext* ctx,
                        ID3D11RenderTargetView* const* origRTVs,
                        ID3D11DepthStencilView* origDSV,
                        ID3D11RenderTargetView** outMSAARTVs,
                        ID3D11DepthStencilView** outMSAADSV);

    // Called when GBuffer pass ends — resolves MSAA to original targets.
    // Skips resolve if no draws happened (false-positive GBuffer detection).
    void OnGBufferLeave(ID3D11DeviceContext* ctx);

    // Notify that a draw happened during the redirected pass.
    void OnDraw();

    // Access MSAA GBuffer SRVs for per-sample deferred shading.
    // Valid after OnGBufferLeave until the next OnGBufferEnter.
    ID3D11ShaderResourceView* GetColorSRV(int index);  // 0-2: RT0, RT1, RT2
    ID3D11ShaderResourceView* GetDepthSRV();

    void Shutdown();
}
