#pragma once

#include <d3d11.h>
#include <cstdint>
#include <vector>
#include "ShaderMetadata.h"

// Kenshi GBuffer RT formats: albedo, normals, depth
static const DXGI_FORMAT GBUFFER_COLOR_FORMATS[3] = {
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_R32_FLOAT
};

struct CapturedDraw
{
    // Draw call parameters
    UINT indexCount            = 0;
    UINT startIndexLocation    = 0;
    INT  baseVertexLocation    = 0;
    UINT instanceCount         = 1;
    UINT startInstanceLocation = 0;

    // Input Assembler — slot 0 is geometry, slot 1 is instance data (if instanced)
    static const UINT           MAX_VB_SLOTS  = 2;
    ID3D11Buffer*               vertexBuffers[MAX_VB_SLOTS] = {};
    UINT                        vbStrides[MAX_VB_SLOTS]     = {};
    UINT                        vbOffsets[MAX_VB_SLOTS]     = {};
    ID3D11Buffer*               indexBuffer   = nullptr;
    DXGI_FORMAT                 indexFormat    = DXGI_FORMAT_R16_UINT;
    UINT                        ibOffset      = 0;
    ID3D11InputLayout*          inputLayout   = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY    topology       = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // Vertex Shader
    ID3D11VertexShader*         vs            = nullptr;
    static const UINT           MAX_VS_CBS    = 4;
    ID3D11Buffer*               vsCBs[MAX_VS_CBS] = {};

    // Pixel Shader
    ID3D11PixelShader*          ps            = nullptr;
    static const UINT           MAX_PS_CBS    = 4;
    ID3D11Buffer*               psCBs[MAX_PS_CBS] = {};
    static const UINT           MAX_PS_SRVS   = 8;
    ID3D11ShaderResourceView*   psSRVs[MAX_PS_SRVS] = {};
    static const UINT           MAX_PS_SAMPLERS = 4;
    ID3D11SamplerState*         psSamplers[MAX_PS_SAMPLERS] = {};

    // Shader metadata (non-owning pointer, valid for lifetime of ShaderMetadata registry)
    const VSConstantBufferInfo* vsMetadata    = nullptr;

    // Staging copy of the VS CB containing the clip/world matrices.
    // Populated at capture time via CopyResource. By replay time (POST_LIGHTING),
    // the GPU has finished the copy so Map won't stall.
    // Null for unclassified draws (UNKNOWN transform type).
    ID3D11Buffer* cbStagingCopy = nullptr;
    uint32_t      cbStagingSize = 0;
};

namespace GeometryCapture
{
    // Called from HookedOMSetRenderTargets to detect GBuffer pass start/end.
    // Must be called AFTER the original OMSetRenderTargets so the state is committed.
    void OnOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT numViews,
                              ID3D11RenderTargetView* const* ppRTVs,
                              ID3D11DepthStencilView* pDSV);

    // Fast path: update GBuffer tracking with a precomputed CheckGBufferConfig result.
    void OnOMSetRenderTargetsWithResult(bool isGBuffer);

    // Called from HookedDrawIndexed when a GBuffer draw is detected.
    void OnDrawIndexed(ID3D11DeviceContext* ctx, UINT indexCount,
                       UINT startIndexLocation, INT baseVertexLocation);

    // Called from HookedDrawIndexedInstanced when a GBuffer instanced draw is detected.
    void OnDrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT indexCountPerInstance,
                                UINT instanceCount, UINT startIndexLocation,
                                INT baseVertexLocation, UINT startInstanceLocation);

    // Per-frame lifecycle
    void ResetFrame();
    void Shutdown();

    // Access captured draws (valid from end of GBuffer pass until ResetFrame)
    const std::vector<CapturedDraw>& GetCaptures();
    uint32_t GetCaptureCount();

    // Whether we're currently inside a detected GBuffer pass
    bool IsInGBufferPass();

    // Check if the given RT config matches the GBuffer (does NOT update internal state).
    bool CheckGBufferConfig(UINT numViews, ID3D11RenderTargetView* const* ppRTVs,
                            ID3D11DepthStencilView* pDSV);

    // Set the expected GBuffer resolution (called when resolution is detected/changes)
    void SetResolution(UINT width, UINT height);

    // Cache the device pointer (called once from TryCaptureDevice)
    void SetDevice(ID3D11Device* device);

    // Capture flags — controls what per-draw state is captured.
    // Default 0 = lean (IA + VS + PS pointer). DUST_CAPTURE_PS_RESOURCES = full PS state.
    void SetCaptureFlags(uint32_t flags);
    uint32_t GetCaptureFlags();
}
