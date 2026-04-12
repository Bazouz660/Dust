#include "OutlineRenderer.h"
#include "OutlineConfig.h"
#include "DustLog.h"
#include <cstring>
#include <string>

namespace OutlineRenderer
{

static bool gInitialized = false;
static UINT gWidth = 0;
static UINT gHeight = 0;
static const DustHostAPI* gHost = nullptr;
static std::string gShaderDir;

// Shaders
static ID3D11VertexShader* gFullscreenVS = nullptr;
static ID3D11PixelShader*  gOutlinePS = nullptr;
static ID3D11PixelShader*  gDebugPS = nullptr;

// Pipeline states
static ID3D11BlendState*        gNoBlend = nullptr;
static ID3D11DepthStencilState* gNoDepthDSS = nullptr;
static ID3D11RasterizerState*   gNoCullRS = nullptr;
static ID3D11SamplerState*      gPointClampSampler = nullptr;
static ID3D11SamplerState*      gLinearClampSampler = nullptr;

// Constant buffer
struct OutlineCBData
{
    float texelSize[2];
    float depthThreshold;
    float normalThreshold;
    float thickness;
    float strength;
    float maxDepth;
    float _pad0;
    float outlineColor[4]; // rgb + pad
};
static ID3D11Buffer* gOutlineCB = nullptr;

// ==================== Public API ====================

bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir)
{
    if (gInitialized)
        return true;

    gHost = host;
    gShaderDir = std::string(effectDir) + "\\shaders\\";
    Log("Outline: Initializing (%ux%u), shaders: %s", width, height, gShaderDir.c_str());
    gWidth = width;
    gHeight = height;

    // Compile shaders
    ID3DBlob* vsBlob = host->CompileShaderFromFile((gShaderDir + "fullscreen_vs.hlsl").c_str(), "main", "vs_5_0");
    if (!vsBlob) return false;

    ID3DBlob* outlineBlob = host->CompileShaderFromFile((gShaderDir + "outline_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!outlineBlob) { vsBlob->Release(); return false; }

    ID3DBlob* debugBlob = host->CompileShaderFromFile((gShaderDir + "outline_debug_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!debugBlob) { vsBlob->Release(); outlineBlob->Release(); return false; }

    HRESULT hr;

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &gFullscreenVS);
    vsBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(outlineBlob->GetBufferPointer(), outlineBlob->GetBufferSize(), nullptr, &gOutlinePS);
    outlineBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(debugBlob->GetBufferPointer(), debugBlob->GetBufferSize(), nullptr, &gDebugPS);
    debugBlob->Release();
    if (FAILED(hr)) return false;

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

    // Point-clamp sampler (for depth/normals — no filtering)
    {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = device->CreateSamplerState(&desc, &gPointClampSampler);
        if (FAILED(hr)) return false;
    }

    // Linear-clamp sampler (for scene color)
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
    gOutlineCB = host->CreateConstantBuffer(device, sizeof(OutlineCBData));
    if (!gOutlineCB) return false;

    gInitialized = true;
    Log("Outline: Initialized successfully");
    return true;
}

void Shutdown()
{
#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gFullscreenVS);
    SAFE_RELEASE(gOutlinePS);
    SAFE_RELEASE(gDebugPS);
    SAFE_RELEASE(gNoBlend);
    SAFE_RELEASE(gNoDepthDSS);
    SAFE_RELEASE(gNoCullRS);
    SAFE_RELEASE(gPointClampSampler);
    SAFE_RELEASE(gLinearClampSampler);
    SAFE_RELEASE(gOutlineCB);
#undef SAFE_RELEASE
    gInitialized = false;
    gHost = nullptr;
    Log("Outline: Shut down");
}

void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight)
{
    // No resolution-dependent textures — just update dimensions
    gWidth = newWidth;
    gHeight = newHeight;
    Log("Outline: Resolution changed to %ux%u", newWidth, newHeight);
}

bool IsInitialized()
{
    return gInitialized;
}

void Render(ID3D11DeviceContext* ctx,
            ID3D11ShaderResourceView* sceneCopySRV,
            ID3D11ShaderResourceView* depthSRV,
            ID3D11ShaderResourceView* normalsSRV,
            ID3D11RenderTargetView* ldrRTV)
{
    if (!gInitialized || !ctx || !sceneCopySRV || !depthSRV || !ldrRTV || !gHost)
        return;

    gHost->SaveState(ctx);

    // Update constant buffer
    {
        OutlineCBData cb = {};
        cb.texelSize[0] = 1.0f / (float)gWidth;
        cb.texelSize[1] = 1.0f / (float)gHeight;
        cb.depthThreshold = gOutlineConfig.depthThreshold;
        cb.normalThreshold = gOutlineConfig.normalThreshold;
        cb.thickness = (float)gOutlineConfig.thickness;
        cb.strength = gOutlineConfig.strength;
        cb.maxDepth = gOutlineConfig.maxDepth;
        cb.outlineColor[0] = gOutlineConfig.colorR;
        cb.outlineColor[1] = gOutlineConfig.colorG;
        cb.outlineColor[2] = gOutlineConfig.colorB;
        cb.outlineColor[3] = 0.0f;
        gHost->UpdateConstantBuffer(ctx, gOutlineCB, &cb, sizeof(cb));
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

    // Single pass: scene + depth + normals → LDR with outlines
    ctx->OMSetRenderTargets(1, &ldrRTV, nullptr);
    ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
    ctx->PSSetShader(gOutlinePS, nullptr, 0);

    ID3D11ShaderResourceView* srvs[3] = { sceneCopySRV, depthSRV, normalsSRV };
    ctx->PSSetShaderResources(0, 3, srvs);

    ID3D11SamplerState* samplers[2] = { gLinearClampSampler, gPointClampSampler };
    ctx->PSSetSamplers(0, 2, samplers);

    ctx->PSSetConstantBuffers(0, 1, &gOutlineCB);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
    ctx->PSSetShaderResources(0, 3, nullSRVs);

    gHost->RestoreState(ctx);
}

void RenderDebugOverlay(ID3D11DeviceContext* ctx,
                        ID3D11ShaderResourceView* depthSRV,
                        ID3D11ShaderResourceView* normalsSRV,
                        ID3D11RenderTargetView* ldrRTV)
{
    if (!gInitialized || !ctx || !depthSRV || !ldrRTV || !gHost)
        return;

    gHost->SaveState(ctx);

    // Update constant buffer
    {
        OutlineCBData cb = {};
        cb.texelSize[0] = 1.0f / (float)gWidth;
        cb.texelSize[1] = 1.0f / (float)gHeight;
        cb.depthThreshold = gOutlineConfig.depthThreshold;
        cb.normalThreshold = gOutlineConfig.normalThreshold;
        cb.thickness = (float)gOutlineConfig.thickness;
        cb.strength = gOutlineConfig.strength;
        cb.maxDepth = gOutlineConfig.maxDepth;
        cb.outlineColor[0] = gOutlineConfig.colorR;
        cb.outlineColor[1] = gOutlineConfig.colorG;
        cb.outlineColor[2] = gOutlineConfig.colorB;
        cb.outlineColor[3] = 0.0f;
        gHost->UpdateConstantBuffer(ctx, gOutlineCB, &cb, sizeof(cb));
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
    ctx->PSSetShader(gDebugPS, nullptr, 0);

    ID3D11ShaderResourceView* srvs[2] = { depthSRV, normalsSRV };
    ctx->PSSetShaderResources(0, 2, srvs);

    ID3D11SamplerState* samplers[1] = { gPointClampSampler };
    ctx->PSSetSamplers(0, 1, samplers);

    ctx->PSSetConstantBuffers(0, 1, &gOutlineCB);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, nullSRVs);

    gHost->RestoreState(ctx);
}

} // namespace OutlineRenderer
