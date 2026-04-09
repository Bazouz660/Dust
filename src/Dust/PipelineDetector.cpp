#include "PipelineDetector.h"
#include "ResourceRegistry.h"
#include "DustLog.h"
#include <dxgi.h>

PipelineDetector gPipelineDetector;

static bool sFirstDetectLogged = false;

PipelineDetector::DetectionResult PipelineDetector::OnFullscreenDraw(ID3D11DeviceContext* ctx)
{
    DetectionResult result;

    // Detect passes in pipeline order
    if (!lightingDetected_)
    {
        if (IsLightingPass(ctx))
        {
            CaptureLightingResources(ctx);
            lightingDetected_ = true;
            result.detected = true;
            result.point = InjectionPoint::POST_LIGHTING;
            return result;
        }
    }

    // Future: fog, tonemap detection here
    // if (lightingDetected_ && !fogDetected_) { ... }
    // if (fogDetected_ && !tonemapDetected_) { ... }

    return result;
}

void PipelineDetector::ResetFrame()
{
    lightingDetected_ = false;
    fogDetected_      = false;
    tonemapDetected_  = false;
}

// Detect lighting pass by pipeline state (hash-free).
// The lighting pass is the only fullscreen draw where:
//   1. RT format = R11G11B10_FLOAT (HDR scene target)
//   2. SRV slot 2 = R32_FLOAT (depth buffer)
//   3. SRV slot 0 is bound (GBuffer albedo)
bool PipelineDetector::IsLightingPass(ID3D11DeviceContext* ctx)
{
    // Check RT format
    ID3D11RenderTargetView* rt = nullptr;
    ctx->OMGetRenderTargets(1, &rt, nullptr);
    if (!rt)
        return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtDesc;
    rt->GetDesc(&rtDesc);
    rt->Release();

    if (rtDesc.Format != DXGI_FORMAT_R11G11B10_FLOAT)
        return false;

    // Check SRV[2] is R32_FLOAT (depth)
    ID3D11ShaderResourceView* srv2 = nullptr;
    ctx->PSGetShaderResources(2, 1, &srv2);
    if (!srv2)
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    srv2->GetDesc(&srvDesc);
    srv2->Release();

    if (srvDesc.Format != DXGI_FORMAT_R32_FLOAT)
        return false;

    // Check SRV[0] is bound (GBuffer albedo)
    ID3D11ShaderResourceView* srv0 = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv0);
    if (!srv0)
        return false;
    srv0->Release();

    return true;
}

void PipelineDetector::CaptureLightingResources(ID3D11DeviceContext* ctx)
{
    // Capture HDR RTV
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv)
    {
        gResourceRegistry.SetRTV(ResourceName::HDR_RTV, rtv);
        rtv->Release();
    }

    // Capture depth SRV (slot 2)
    ID3D11ShaderResourceView* depthSRV = nullptr;
    ctx->PSGetShaderResources(2, 1, &depthSRV);
    if (depthSRV)
    {
        gResourceRegistry.SetSRV(ResourceName::DEPTH_SRV, depthSRV);
        depthSRV->Release();
    }

    if (!sFirstDetectLogged)
    {
        Log("Pipeline: Lighting pass detected (depth=%p, hdrRT=%p)",
            gResourceRegistry.GetSRV(ResourceName::DEPTH_SRV),
            gResourceRegistry.GetRTV(ResourceName::HDR_RTV));
        sFirstDetectLogged = true;
    }
}
