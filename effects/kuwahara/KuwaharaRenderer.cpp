#include "KuwaharaRenderer.h"
#include "KuwaharaConfig.h"
#include "DustLog.h"
#include <cstring>
#include <string>

namespace KuwaharaRenderer
{

static bool gInitialized = false;
static UINT gWidth = 0;
static UINT gHeight = 0;
static const DustHostAPI* gHost = nullptr;
static std::string gShaderDir;

// Shaders
static ID3D11VertexShader* gFullscreenVS = nullptr;
static ID3D11PixelShader*  gKuwaharaPS = nullptr;

// Pipeline states
static ID3D11BlendState*        gNoBlend = nullptr;
static ID3D11DepthStencilState* gNoDepthDSS = nullptr;
static ID3D11RasterizerState*   gNoCullRS = nullptr;
static ID3D11SamplerState*      gPointClampSampler = nullptr;

// Constant buffer
struct KuwaharaCBData
{
    float texelSize[2];
    int   radius;
    float strength;
    float sharpness;
    float _pad[3];
};
static ID3D11Buffer* gKuwaharaCB = nullptr;

bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir)
{
    if (gInitialized)
        return true;

    gHost = host;
    gShaderDir = std::string(effectDir) + "\\shaders\\";
    gWidth = width;
    gHeight = height;

    ID3DBlob* vsBlob = host->CompileShaderFromFile((gShaderDir + "fullscreen_vs.hlsl").c_str(), "main", "vs_5_0");
    if (!vsBlob) return false;

    ID3DBlob* psBlob = host->CompileShaderFromFile((gShaderDir + "kuwahara_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!psBlob) { vsBlob->Release(); return false; }

    HRESULT hr;
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &gFullscreenVS);
    vsBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &gKuwaharaPS);
    psBlob->Release();
    if (FAILED(hr)) return false;

    // No-blend
    {
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].BlendEnable = FALSE;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&desc, &gNoBlend);
    }
    // No-depth
    {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = FALSE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        device->CreateDepthStencilState(&desc, &gNoDepthDSS);
    }
    // No-cull
    {
        D3D11_RASTERIZER_DESC desc = {};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        desc.DepthClipEnable = FALSE;
        device->CreateRasterizerState(&desc, &gNoCullRS);
    }
    // Point-clamp sampler
    {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        device->CreateSamplerState(&desc, &gPointClampSampler);
    }

    gKuwaharaCB = host->CreateConstantBuffer(device, sizeof(KuwaharaCBData));
    if (!gKuwaharaCB) return false;

    gInitialized = true;
    Log("Kuwahara: Initialized (%ux%u)", width, height);
    return true;
}

void Shutdown()
{
#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gFullscreenVS);
    SAFE_RELEASE(gKuwaharaPS);
    SAFE_RELEASE(gNoBlend);
    SAFE_RELEASE(gNoDepthDSS);
    SAFE_RELEASE(gNoCullRS);
    SAFE_RELEASE(gPointClampSampler);
    SAFE_RELEASE(gKuwaharaCB);
#undef SAFE_RELEASE
    gInitialized = false;
    gHost = nullptr;
}

void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight)
{
    gWidth = newWidth;
    gHeight = newHeight;
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

    {
        KuwaharaCBData cb = {};
        cb.texelSize[0] = 1.0f / (float)gWidth;
        cb.texelSize[1] = 1.0f / (float)gHeight;
        cb.radius = gKuwaharaConfig.radius;
        cb.strength = gKuwaharaConfig.strength;
        cb.sharpness = gKuwaharaConfig.sharpness;
        gHost->UpdateConstantBuffer(ctx, gKuwaharaCB, &cb, sizeof(cb));
    }

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
    ctx->PSSetShader(gKuwaharaPS, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &sceneCopySRV);
    ctx->PSSetSamplers(0, 1, &gPointClampSampler);
    ctx->PSSetConstantBuffers(0, 1, &gKuwaharaCB);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);

    gHost->RestoreState(ctx);
}

} // namespace KuwaharaRenderer
