#include "ClarityRenderer.h"
#include "ClarityConfig.h"
#include "DustLog.h"
#include <cstring>
#include <string>

namespace ClarityRenderer
{

static bool gInitialized = false;
static UINT gWidth = 0;
static UINT gHeight = 0;
static const DustHostAPI* gHost = nullptr;
static std::string gShaderDir;

// Blur textures (ping-pong for H + V)
static ID3D11Texture2D*          gBlurTex = nullptr;
static ID3D11RenderTargetView*   gBlurRTV = nullptr;
static ID3D11ShaderResourceView* gBlurSRV = nullptr;

static ID3D11Texture2D*          gBlurTempTex = nullptr;
static ID3D11RenderTargetView*   gBlurTempRTV = nullptr;
static ID3D11ShaderResourceView* gBlurTempSRV = nullptr;

// Shaders
static ID3D11VertexShader* gFullscreenVS = nullptr;
static ID3D11PixelShader*  gBlurHPS = nullptr;
static ID3D11PixelShader*  gBlurVPS = nullptr;
static ID3D11PixelShader*  gCompositePS = nullptr;
static ID3D11PixelShader*  gDebugPS = nullptr;

// Pipeline states
static ID3D11BlendState*        gNoBlend = nullptr;
static ID3D11DepthStencilState* gNoDepthDSS = nullptr;
static ID3D11RasterizerState*   gNoCullRS = nullptr;
static ID3D11SamplerState*      gLinearClampSampler = nullptr;

// Constant buffer
struct ClarityCBData
{
    float viewportSize[2];
    float invViewportSize[2];
    float strength;
    float midtoneProtect;
    float blurRadius;
    float _pad;
};
static ID3D11Buffer* gClarityCB = nullptr;

// ==================== Helpers ====================

static bool CreateLDRTexture(ID3D11Device* device, UINT width, UINT height,
                             ID3D11Texture2D** outTex,
                             ID3D11RenderTargetView** outRTV,
                             ID3D11ShaderResourceView** outSRV)
{
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, outTex);
    if (FAILED(hr)) { Log("Clarity: Failed to create texture (%ux%u): 0x%08X", width, height, hr); return false; }

    hr = device->CreateRenderTargetView(*outTex, nullptr, outRTV);
    if (FAILED(hr)) { Log("Clarity: Failed to create RTV: 0x%08X", hr); return false; }

    hr = device->CreateShaderResourceView(*outTex, nullptr, outSRV);
    if (FAILED(hr)) { Log("Clarity: Failed to create SRV: 0x%08X", hr); return false; }

    return true;
}

// ==================== Public API ====================

bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir)
{
    if (gInitialized)
        return true;

    gHost = host;
    gShaderDir = std::string(effectDir) + "\\shaders\\";
    Log("Clarity: Initializing (%ux%u), shaders: %s", width, height, gShaderDir.c_str());
    gWidth = width;
    gHeight = height;

    // Compile shaders
    ID3DBlob* vsBlob = host->CompileShaderFromFile((gShaderDir + "fullscreen_vs.hlsl").c_str(), "main", "vs_5_0");
    if (!vsBlob) return false;

    ID3DBlob* blurHBlob = host->CompileShaderFromFile((gShaderDir + "clarity_blur_h_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!blurHBlob) { vsBlob->Release(); return false; }

    ID3DBlob* blurVBlob = host->CompileShaderFromFile((gShaderDir + "clarity_blur_v_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!blurVBlob) { vsBlob->Release(); blurHBlob->Release(); return false; }

    ID3DBlob* compositeBlob = host->CompileShaderFromFile((gShaderDir + "clarity_composite_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!compositeBlob) { vsBlob->Release(); blurHBlob->Release(); blurVBlob->Release(); return false; }

    ID3DBlob* debugBlob = host->CompileShaderFromFile((gShaderDir + "clarity_debug_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!debugBlob) { vsBlob->Release(); blurHBlob->Release(); blurVBlob->Release(); compositeBlob->Release(); return false; }

    HRESULT hr;

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &gFullscreenVS);
    vsBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(blurHBlob->GetBufferPointer(), blurHBlob->GetBufferSize(), nullptr, &gBlurHPS);
    blurHBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(blurVBlob->GetBufferPointer(), blurVBlob->GetBufferSize(), nullptr, &gBlurVPS);
    blurVBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(compositeBlob->GetBufferPointer(), compositeBlob->GetBufferSize(), nullptr, &gCompositePS);
    compositeBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(debugBlob->GetBufferPointer(), debugBlob->GetBufferSize(), nullptr, &gDebugPS);
    debugBlob->Release();
    if (FAILED(hr)) return false;

    // Textures
    if (!CreateLDRTexture(device, width, height, &gBlurTex, &gBlurRTV, &gBlurSRV))
        return false;
    if (!CreateLDRTexture(device, width, height, &gBlurTempTex, &gBlurTempRTV, &gBlurTempSRV))
        return false;

    // No-blend
    {
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].BlendEnable = FALSE;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &gNoBlend);
        if (FAILED(hr)) return false;
    }

    // No-depth
    {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = FALSE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        hr = device->CreateDepthStencilState(&desc, &gNoDepthDSS);
        if (FAILED(hr)) return false;
    }

    // No-cull rasterizer
    {
        D3D11_RASTERIZER_DESC desc = {};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        desc.DepthClipEnable = FALSE;
        hr = device->CreateRasterizerState(&desc, &gNoCullRS);
        if (FAILED(hr)) return false;
    }

    // Linear-clamp sampler
    {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = device->CreateSamplerState(&desc, &gLinearClampSampler);
        if (FAILED(hr)) return false;
    }

    // Constant buffer
    gClarityCB = host->CreateConstantBuffer(device, sizeof(ClarityCBData));
    if (!gClarityCB) return false;

    gInitialized = true;
    Log("Clarity: Initialized successfully");
    return true;
}

void Shutdown()
{
#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gBlurTex);     SAFE_RELEASE(gBlurRTV);     SAFE_RELEASE(gBlurSRV);
    SAFE_RELEASE(gBlurTempTex); SAFE_RELEASE(gBlurTempRTV); SAFE_RELEASE(gBlurTempSRV);
    SAFE_RELEASE(gFullscreenVS);
    SAFE_RELEASE(gBlurHPS);     SAFE_RELEASE(gBlurVPS);
    SAFE_RELEASE(gCompositePS); SAFE_RELEASE(gDebugPS);
    SAFE_RELEASE(gNoBlend);
    SAFE_RELEASE(gNoDepthDSS);  SAFE_RELEASE(gNoCullRS);
    SAFE_RELEASE(gLinearClampSampler);
    SAFE_RELEASE(gClarityCB);
#undef SAFE_RELEASE
    gInitialized = false;
    gHost = nullptr;
    Log("Clarity: Shut down");
}

void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight)
{
    if (newWidth == gWidth && newHeight == gHeight)
        return;

    Log("Clarity: Resolution changed: %ux%u -> %ux%u", gWidth, gHeight, newWidth, newHeight);

#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gBlurTex);     SAFE_RELEASE(gBlurRTV);     SAFE_RELEASE(gBlurSRV);
    SAFE_RELEASE(gBlurTempTex); SAFE_RELEASE(gBlurTempRTV); SAFE_RELEASE(gBlurTempSRV);
#undef SAFE_RELEASE

    gWidth = newWidth;
    gHeight = newHeight;
    if (!CreateLDRTexture(device, newWidth, newHeight, &gBlurTex, &gBlurRTV, &gBlurSRV)
        || !CreateLDRTexture(device, newWidth, newHeight, &gBlurTempTex, &gBlurTempRTV, &gBlurTempSRV))
    {
        Log("Clarity: WARNING: Failed to recreate textures after resolution change");
        gInitialized = false;
    }
}

bool IsInitialized()
{
    return gInitialized;
}

void Render(ID3D11DeviceContext* ctx,
            ID3D11ShaderResourceView* sceneCopySRV,
            ID3D11RenderTargetView* ldrRTV)
{
    if (!gInitialized || !ctx || !sceneCopySRV || !ldrRTV || !gHost)
        return;

    gHost->SaveState(ctx);

    // Update constant buffer
    {
        ClarityCBData cb = {};
        cb.viewportSize[0] = (float)gWidth;
        cb.viewportSize[1] = (float)gHeight;
        cb.invViewportSize[0] = 1.0f / (float)gWidth;
        cb.invViewportSize[1] = 1.0f / (float)gHeight;
        cb.strength = gClarityConfig.strength;
        cb.midtoneProtect = gClarityConfig.midtoneProtect;
        cb.blurRadius = gClarityConfig.blurRadius;
        gHost->UpdateConstantBuffer(ctx, gClarityCB, &cb, sizeof(cb));
    }

    // Common state
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(gFullscreenVS, nullptr, 0);
    ctx->RSSetState(gNoCullRS);
    ctx->OMSetDepthStencilState(gNoDepthDSS, 0);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)gWidth;
    vp.Height = (float)gHeight;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    float blendFactor[4] = { 0, 0, 0, 0 };
    ID3D11ShaderResourceView* nullSRV = nullptr;

    // Pass 1: Horizontal blur (scene copy → blur temp)
    {
        ctx->OMSetRenderTargets(1, &gBlurTempRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gBlurHPS, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &sceneCopySRV);
        ctx->PSSetSamplers(0, 1, &gLinearClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gClarityCB);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &nullSRV);
    }

    // Pass 2: Vertical blur (blur temp → blur final)
    {
        ctx->OMSetRenderTargets(1, &gBlurRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gBlurVPS, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &gBlurTempSRV);
        ctx->PSSetSamplers(0, 1, &gLinearClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gClarityCB);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &nullSRV);
    }

    // Pass 3: Composite (scene copy + blurred → LDR target)
    {
        ctx->OMSetRenderTargets(1, &ldrRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gCompositePS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { sceneCopySRV, gBlurSRV };
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 1, &gLinearClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gClarityCB);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        ctx->PSSetShaderResources(0, 2, nullSRVs);
    }

    gHost->RestoreState(ctx);
}

void RenderDebugOverlay(ID3D11DeviceContext* ctx,
                        ID3D11ShaderResourceView* sceneCopySRV,
                        ID3D11RenderTargetView* ldrRTV)
{
    if (!gInitialized || !ctx || !sceneCopySRV || !ldrRTV || !gHost)
        return;

    gHost->SaveState(ctx);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(gFullscreenVS, nullptr, 0);
    ctx->RSSetState(gNoCullRS);
    ctx->OMSetDepthStencilState(gNoDepthDSS, 0);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)gWidth;
    vp.Height = (float)gHeight;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetRenderTargets(1, &ldrRTV, nullptr);
    ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
    ctx->PSSetShader(gDebugPS, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { sceneCopySRV, gBlurSRV };
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, &gLinearClampSampler);
    ctx->PSSetConstantBuffers(0, 1, &gClarityCB);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, nullSRVs);

    gHost->RestoreState(ctx);
}

} // namespace ClarityRenderer
