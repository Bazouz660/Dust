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

    // Detect tonemap pass (after lighting, writes B8G8R8A8_UNORM with HDR input)
    if (lightingDetected_ && !tonemapDetected_)
    {
        if (IsTonemapPass(ctx))
        {
            CaptureTonemapResources(ctx);
            tonemapDetected_ = true;
            result.detected = true;
            result.point = InjectionPoint::POST_TONEMAP;
            return result;
        }
    }

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

    // Capture GBuffer SRVs (slots 0-2 during lighting pass)
    // slot 0 = albedo (B8G8R8A8_UNORM), slot 1 = normals (B8G8R8A8_UNORM), slot 2 = depth (R32_FLOAT)
    ID3D11ShaderResourceView* albedoSRV = nullptr;
    ctx->PSGetShaderResources(0, 1, &albedoSRV);
    if (albedoSRV)
    {
        gResourceRegistry.SetSRV(ResourceName::ALBEDO_SRV, albedoSRV);
        albedoSRV->Release();
    }

    ID3D11ShaderResourceView* normalsSRV = nullptr;
    ctx->PSGetShaderResources(1, 1, &normalsSRV);
    if (normalsSRV)
    {
        gResourceRegistry.SetSRV(ResourceName::NORMALS_SRV, normalsSRV);
        normalsSRV->Release();
    }

    ID3D11ShaderResourceView* depthSRV = nullptr;
    ctx->PSGetShaderResources(2, 1, &depthSRV);
    if (depthSRV)
    {
        gResourceRegistry.SetSRV(ResourceName::DEPTH_SRV, depthSRV);
        depthSRV->Release();
    }

    if (!sFirstDetectLogged)
    {
        Log("Pipeline: Lighting pass detected (albedo=%p, normals=%p, depth=%p, hdrRT=%p)",
            gResourceRegistry.GetSRV(ResourceName::ALBEDO_SRV),
            gResourceRegistry.GetSRV(ResourceName::NORMALS_SRV),
            gResourceRegistry.GetSRV(ResourceName::DEPTH_SRV),
            gResourceRegistry.GetRTV(ResourceName::HDR_RTV));
        sFirstDetectLogged = true;
    }
}

// Detect tonemap pass: fullscreen draw where:
//   1. RT format = B8G8R8A8_UNORM (LDR output) at full resolution
//   2. SRV[0] = R11G11B10_FLOAT (HDR scene — the same target lighting wrote to)
// This distinguishes tonemap from FXAA/heat haze which read B8G8R8A8_UNORM, not R11G11B10.
bool PipelineDetector::IsTonemapPass(ID3D11DeviceContext* ctx)
{
    // Check RT format is B8G8R8A8_UNORM
    ID3D11RenderTargetView* rt = nullptr;
    ctx->OMGetRenderTargets(1, &rt, nullptr);
    if (!rt)
        return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtDesc;
    rt->GetDesc(&rtDesc);

    // Also check the RT is at full resolution (not a downscale pass)
    ID3D11Resource* rtResource = nullptr;
    rt->GetResource(&rtResource);
    rt->Release();

    if (rtDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        if (rtResource) rtResource->Release();
        return false;
    }

    if (rtResource)
    {
        ID3D11Texture2D* tex = nullptr;
        rtResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
        rtResource->Release();
        if (tex)
        {
            D3D11_TEXTURE2D_DESC texDesc;
            tex->GetDesc(&texDesc);
            tex->Release();

            // Must be at reasonable resolution (not a downscale/bloom pass).
            // Luminance chain goes down to 1x1, bloom is quarter-res.
            // Tonemap is always at native res, so 640 is a safe lower bound.
            if (texDesc.Width < 640 || texDesc.Height < 360)
                return false;
        }
    }

    // Check SRV[0] is R11G11B10_FLOAT (the HDR scene)
    ID3D11ShaderResourceView* srv0 = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv0);
    if (!srv0)
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    srv0->GetDesc(&srvDesc);
    srv0->Release();

    if (srvDesc.Format != DXGI_FORMAT_R11G11B10_FLOAT)
        return false;

    return true;
}

void PipelineDetector::CaptureTonemapResources(ID3D11DeviceContext* ctx)
{
    // Capture LDR RTV (tonemap output)
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv)
    {
        gResourceRegistry.SetRTV(ResourceName::LDR_RTV, rtv);
        rtv->Release();
    }

    static bool sLogged = false;
    if (!sLogged)
    {
        Log("Pipeline: Tonemap pass detected (ldrRT=%p)",
            gResourceRegistry.GetRTV(ResourceName::LDR_RTV));
        sLogged = true;
    }
}
