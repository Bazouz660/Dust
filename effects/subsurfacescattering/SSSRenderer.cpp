#include "SSSRenderer.h"
#include "SSSConfig.h"
#include "DustLog.h"
#include <string>

namespace SSSRenderer
{

static bool gInitialized = false;
static UINT gWidth = 0;
static UINT gHeight = 0;
static const DustHostAPI* gHost = nullptr;
static std::string gShaderDir;

// HDR ping-pong textures for the separable blur (R16G16B16A16_FLOAT to match
// the game's HDR render target).
static ID3D11Texture2D*          gTempTex = nullptr;   // horizontal result
static ID3D11RenderTargetView*   gTempRTV = nullptr;
static ID3D11ShaderResourceView* gTempSRV = nullptr;

static ID3D11Texture2D*          gBlurTex = nullptr;   // vertical result
static ID3D11RenderTargetView*   gBlurRTV = nullptr;
static ID3D11ShaderResourceView* gBlurSRV = nullptr;

// Shaders
static ID3D11VertexShader* gFullscreenVS = nullptr;
static ID3D11PixelShader*  gBlurHPS = nullptr;
static ID3D11PixelShader*  gBlurVPS = nullptr;
static ID3D11PixelShader*  gCompositePS = nullptr;
static ID3D11PixelShader*  gDebugPS = nullptr;

// Pipeline state
static ID3D11BlendState*        gNoBlend = nullptr;
static ID3D11DepthStencilState* gNoDepthDSS = nullptr;
static ID3D11RasterizerState*   gNoCullRS = nullptr;
static ID3D11SamplerState*      gLinearClamp = nullptr;
static ID3D11SamplerState*      gPointClamp = nullptr;

// Constant buffer — layout MUST match SSSParams in sss_common.hlsli.
struct SSSCBData
{
    float viewportSize[2];
    float invViewportSize[2];
    float blurWidth;
    float depthThreshold;
    float followSurface;
    float maxRadiusPx;
    float strength;
    float debugMode;
    float _pad[2];
};
static ID3D11Buffer* gCB = nullptr;

// ==================== Helpers ====================

static bool CreateHDRTexture(ID3D11Device* device, UINT width, UINT height,
                             ID3D11Texture2D** outTex,
                             ID3D11RenderTargetView** outRTV,
                             ID3D11ShaderResourceView** outSRV)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&td, nullptr, outTex);
    if (FAILED(hr)) { Log("SSS: CreateTexture2D failed (%ux%u): 0x%08X", width, height, hr); return false; }
    hr = device->CreateRenderTargetView(*outTex, nullptr, outRTV);
    if (FAILED(hr)) { Log("SSS: CreateRTV failed: 0x%08X", hr); return false; }
    hr = device->CreateShaderResourceView(*outTex, nullptr, outSRV);
    if (FAILED(hr)) { Log("SSS: CreateSRV failed: 0x%08X", hr); return false; }
    return true;
}

// ==================== Public API ====================

bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir)
{
    if (gInitialized) return true;

    gHost = host;
    gShaderDir = std::string(effectDir) + "\\shaders\\";
    gWidth = width;
    gHeight = height;
    Log("SSS: Initializing (%ux%u), shaders: %s", width, height, gShaderDir.c_str());

    ID3DBlob* vsBlob = host->CompileShaderFromFile((gShaderDir + "fullscreen_vs.hlsl").c_str(), "main", "vs_5_0");
    if (!vsBlob) return false;
    ID3DBlob* hBlob = host->CompileShaderFromFile((gShaderDir + "sss_blur_h_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!hBlob) { vsBlob->Release(); return false; }
    ID3DBlob* vBlob = host->CompileShaderFromFile((gShaderDir + "sss_blur_v_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!vBlob) { vsBlob->Release(); hBlob->Release(); return false; }
    ID3DBlob* cBlob = host->CompileShaderFromFile((gShaderDir + "sss_composite_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!cBlob) { vsBlob->Release(); hBlob->Release(); vBlob->Release(); return false; }

    HRESULT hr;
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &gFullscreenVS);
    vsBlob->Release();
    if (FAILED(hr)) { hBlob->Release(); vBlob->Release(); cBlob->Release(); return false; }
    hr = device->CreatePixelShader(hBlob->GetBufferPointer(), hBlob->GetBufferSize(), nullptr, &gBlurHPS);
    hBlob->Release();
    if (FAILED(hr)) { vBlob->Release(); cBlob->Release(); return false; }
    hr = device->CreatePixelShader(vBlob->GetBufferPointer(), vBlob->GetBufferSize(), nullptr, &gBlurVPS);
    vBlob->Release();
    if (FAILED(hr)) { cBlob->Release(); return false; }
    hr = device->CreatePixelShader(cBlob->GetBufferPointer(), cBlob->GetBufferSize(), nullptr, &gCompositePS);
    cBlob->Release();
    if (FAILED(hr)) return false;

    ID3DBlob* dBlob = host->CompileShaderFromFile((gShaderDir + "sss_composite_ps.hlsl").c_str(), "debug_main", "ps_5_0");
    if (dBlob)
    {
        device->CreatePixelShader(dBlob->GetBufferPointer(), dBlob->GetBufferSize(), nullptr, &gDebugPS);
        dBlob->Release();
    }

    if (!CreateHDRTexture(device, width, height, &gTempTex, &gTempRTV, &gTempSRV)) return false;
    if (!CreateHDRTexture(device, width, height, &gBlurTex, &gBlurRTV, &gBlurSRV)) return false;

    {
        D3D11_BLEND_DESC d = {};
        d.RenderTarget[0].BlendEnable = FALSE;
        d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&d, &gNoBlend))) return false;
    }
    {
        D3D11_DEPTH_STENCIL_DESC d = {};
        d.DepthEnable = FALSE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(device->CreateDepthStencilState(&d, &gNoDepthDSS))) return false;
    }
    {
        D3D11_RASTERIZER_DESC d = {};
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_NONE;
        d.DepthClipEnable = FALSE;
        if (FAILED(device->CreateRasterizerState(&d, &gNoCullRS))) return false;
    }
    {
        D3D11_SAMPLER_DESC d = {};
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(device->CreateSamplerState(&d, &gLinearClamp))) return false;
    }
    {
        D3D11_SAMPLER_DESC d = {};
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(device->CreateSamplerState(&d, &gPointClamp))) return false;
    }

    gCB = host->CreateConstantBuffer(device, sizeof(SSSCBData));
    if (!gCB) return false;

    gInitialized = true;
    Log("SSS: Initialized successfully");
    return true;
}

void Shutdown()
{
#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gTempTex); SAFE_RELEASE(gTempRTV); SAFE_RELEASE(gTempSRV);
    SAFE_RELEASE(gBlurTex); SAFE_RELEASE(gBlurRTV); SAFE_RELEASE(gBlurSRV);
    SAFE_RELEASE(gFullscreenVS);
    SAFE_RELEASE(gBlurHPS); SAFE_RELEASE(gBlurVPS); SAFE_RELEASE(gCompositePS); SAFE_RELEASE(gDebugPS);
    SAFE_RELEASE(gNoBlend); SAFE_RELEASE(gNoDepthDSS); SAFE_RELEASE(gNoCullRS);
    SAFE_RELEASE(gLinearClamp); SAFE_RELEASE(gPointClamp);
    SAFE_RELEASE(gCB);
#undef SAFE_RELEASE
    gInitialized = false;
    gHost = nullptr;
    Log("SSS: Shut down");
}

void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight)
{
    if (newWidth == gWidth && newHeight == gHeight) return;
    Log("SSS: Resolution %ux%u -> %ux%u", gWidth, gHeight, newWidth, newHeight);
#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gTempTex); SAFE_RELEASE(gTempRTV); SAFE_RELEASE(gTempSRV);
    SAFE_RELEASE(gBlurTex); SAFE_RELEASE(gBlurRTV); SAFE_RELEASE(gBlurSRV);
#undef SAFE_RELEASE
    gWidth = newWidth;
    gHeight = newHeight;
    if (!CreateHDRTexture(device, newWidth, newHeight, &gTempTex, &gTempRTV, &gTempSRV)
        || !CreateHDRTexture(device, newWidth, newHeight, &gBlurTex, &gBlurRTV, &gBlurSRV))
    {
        Log("SSS: WARNING: failed to recreate textures after resolution change");
        gInitialized = false;
    }
}

bool IsInitialized() { return gInitialized; }

void Render(ID3D11DeviceContext* ctx,
            ID3D11ShaderResourceView* sceneCopySRV,
            ID3D11ShaderResourceView* normalsSRV,
            ID3D11ShaderResourceView* depthSRV,
            ID3D11RenderTargetView* hdrRTV,
            UINT width, UINT height)
{
    if (!gInitialized || !ctx || !sceneCopySRV || !normalsSRV || !depthSRV || !hdrRTV || !gHost)
        return;

    gHost->SaveState(ctx);

    // Update cbuffer
    {
        SSSCBData cb = {};
        cb.viewportSize[0] = (float)width;
        cb.viewportSize[1] = (float)height;
        cb.invViewportSize[0] = 1.0f / (float)width;
        cb.invViewportSize[1] = 1.0f / (float)height;
        cb.blurWidth = gSSSConfig.blurWidth;
        cb.depthThreshold = gSSSConfig.depthThreshold;
        cb.followSurface = gSSSConfig.followSurface;
        cb.maxRadiusPx = gSSSConfig.maxRadiusPx;
        cb.strength = gSSSConfig.strength;
        cb.debugMode = 0.0f;   // unused — debug view is now a host-registered overlay
        gHost->UpdateConstantBuffer(ctx, gCB, &cb, sizeof(cb));
    }

    // Common state
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(gFullscreenVS, nullptr, 0);
    ctx->RSSetState(gNoCullRS);
    ctx->OMSetDepthStencilState(gNoDepthDSS, 0);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    float blendFactor[4] = { 0, 0, 0, 0 };
    ID3D11SamplerState* samplers[2] = { gLinearClamp, gPointClamp };
    ID3D11ShaderResourceView* nullSRV3[3] = { nullptr, nullptr, nullptr };

    // Pass 1: horizontal blur (scene copy + normals + depth -> temp)
    {
        ctx->OMSetRenderTargets(1, &gTempRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gBlurHPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[3] = { sceneCopySRV, normalsSRV, depthSRV };
        ctx->PSSetShaderResources(0, 3, srvs);
        ctx->PSSetSamplers(0, 2, samplers);
        ctx->PSSetConstantBuffers(0, 1, &gCB);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 3, nullSRV3);
    }

    // Pass 2: vertical blur (temp + normals + depth -> blur)
    {
        ctx->OMSetRenderTargets(1, &gBlurRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gBlurVPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[3] = { gTempSRV, normalsSRV, depthSRV };
        ctx->PSSetShaderResources(0, 3, srvs);
        ctx->PSSetSamplers(0, 2, samplers);
        ctx->PSSetConstantBuffers(0, 1, &gCB);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 3, nullSRV3);
    }

    // Pass 3: composite (scene copy + blur + normals -> HDR RT, skin only)
    {
        ctx->OMSetRenderTargets(1, &hdrRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gCompositePS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[3] = { sceneCopySRV, gBlurSRV, normalsSRV };
        ctx->PSSetShaderResources(0, 3, srvs);
        ctx->PSSetSamplers(0, 2, samplers);
        ctx->PSSetConstantBuffers(0, 1, &gCB);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 3, nullSRV3);
    }

    gHost->RestoreState(ctx);
}

// Debug-view overlay: tints skin-tagged pixels magenta onto the final LDR
// target. The host calls this when this view is the active one (after
// tonemapping). Binds the final LDR scene copy (t0) and the GBuffer normals
// whose .a carries the skin tag (t2), then draws gDebugPS fullscreen.
void DebugRender(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* targetRTV, void* /*user*/)
{
    if (!gInitialized || !gSSSConfig.enabled || !gHost || !gDebugPS)
        return;

    ID3D11ShaderResourceView* sceneCopy = gHost->GetSceneCopy(ctx, DUST_RESOURCE_LDR_RT);
    if (!sceneCopy)
        return;

    ID3D11ShaderResourceView* normalsSRV = gHost->GetSRV(DUST_RESOURCE_NORMALS);
    if (!normalsSRV)
        return;

    gHost->SaveState(ctx);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)gWidth;
    vp.Height = (float)gHeight;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(gNoCullRS);
    ctx->OMSetRenderTargets(1, &targetRTV, nullptr);
    ctx->OMSetBlendState(gNoBlend, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(gNoDepthDSS, 0);

    ID3D11SamplerState* samplers[2] = { gLinearClamp, gPointClamp };
    ID3D11ShaderResourceView* srvs[3] = { sceneCopy, nullptr, normalsSRV };
    ctx->PSSetShaderResources(0, 3, srvs);
    ctx->PSSetSamplers(0, 2, samplers);
    ctx->PSSetConstantBuffers(0, 1, &gCB);
    gHost->DrawFullscreenTriangle(ctx, gDebugPS);

    ID3D11ShaderResourceView* nullSRV3[3] = { nullptr, nullptr, nullptr };
    ctx->PSSetShaderResources(0, 3, nullSRV3);

    gHost->RestoreState(ctx);
}

} // namespace SSSRenderer
