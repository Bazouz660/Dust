#include "SSILRenderer.h"
#include "SSILConfig.h"
#include "DustLog.h"
#include <cstring>
#include <string>

namespace SSILRenderer
{

static bool gInitialized = false;
static UINT gWidth = 0;
static UINT gHeight = 0;
static const DustHostAPI* gHost = nullptr;
static std::string gShaderDir;

// IL textures (ping-pong for gen + blur) — RGB indirect light
static ID3D11Texture2D*          gILTex = nullptr;
static ID3D11RenderTargetView*   gILRTV = nullptr;
static ID3D11ShaderResourceView* gILSRV = nullptr;

static ID3D11Texture2D*          gILBlurTex = nullptr;
static ID3D11RenderTargetView*   gILBlurRTV = nullptr;
static ID3D11ShaderResourceView* gILBlurSRV = nullptr;

// Shaders
static ID3D11VertexShader* gFullscreenVS = nullptr;
static ID3D11PixelShader*  gSSILGenPS = nullptr;
static ID3D11PixelShader*  gSSILBlurHPS = nullptr;
static ID3D11PixelShader*  gSSILBlurVPS = nullptr;
static ID3D11PixelShader*  gSSILDebugPS = nullptr;

// Pipeline states
static ID3D11BlendState*        gNoBlend = nullptr;
static ID3D11DepthStencilState* gNoDepthDSS = nullptr;
static ID3D11RasterizerState*   gNoCullRS = nullptr;
static ID3D11SamplerState*      gPointClampSampler = nullptr;

struct SSILCBData
{
    float viewportSize[2];
    float invViewportSize[2];
    float tanHalfFov;
    float aspectRatio;
    float ilRadius;
    float ilStrength;
    float ilBias;
    float ilMaxDepth;
    float foregroundFade;
    float falloffPower;
    float maxScreenRadius;
    float minScreenRadius;
    float depthFadeStart;
    float colorBleeding;
    float debugMode;
    float blurSharpness;
    float numDirections;
    float numSteps;
};
static ID3D11Buffer* gSSILCB = nullptr;

// ==================== Helpers ====================

static bool CreateRGBTexture(ID3D11Device* device, UINT width, UINT height,
                             ID3D11Texture2D** outTex,
                             ID3D11RenderTargetView** outRTV,
                             ID3D11ShaderResourceView** outSRV)
{
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, outTex);
    if (FAILED(hr)) { Log("SSIL: Failed to create RGB texture (%ux%u): 0x%08X", width, height, hr); return false; }

    hr = device->CreateRenderTargetView(*outTex, nullptr, outRTV);
    if (FAILED(hr)) { Log("SSIL: Failed to create RGB RTV: 0x%08X", hr); return false; }

    hr = device->CreateShaderResourceView(*outTex, nullptr, outSRV);
    if (FAILED(hr)) { Log("SSIL: Failed to create RGB SRV: 0x%08X", hr); return false; }

    return true;
}

// ==================== Public API ====================

bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir)
{
    if (gInitialized)
        return true;

    gHost = host;
    gShaderDir = std::string(effectDir) + "\\shaders\\";
    Log("SSIL: Initializing SSILRenderer (%ux%u), shaders: %s", width, height, gShaderDir.c_str());
    gWidth = width;
    gHeight = height;

    // Compile all shaders from .hlsl files
    ID3DBlob* vsBlob = host->CompileShaderFromFile((gShaderDir + "fullscreen_vs.hlsl").c_str(), "main", "vs_5_0");
    if (!vsBlob) return false;

    ID3DBlob* genBlob = host->CompileShaderFromFile((gShaderDir + "ssil_gen_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!genBlob) { vsBlob->Release(); return false; }

    ID3DBlob* blurHBlob = host->CompileShaderFromFile((gShaderDir + "ssil_blur_h_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!blurHBlob) { vsBlob->Release(); genBlob->Release(); return false; }

    ID3DBlob* blurVBlob = host->CompileShaderFromFile((gShaderDir + "ssil_blur_v_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!blurVBlob) { vsBlob->Release(); genBlob->Release(); blurHBlob->Release(); return false; }

    ID3DBlob* debugBlob = host->CompileShaderFromFile((gShaderDir + "ssil_debug_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!debugBlob) { vsBlob->Release(); genBlob->Release(); blurHBlob->Release(); blurVBlob->Release(); return false; }

    HRESULT hr;

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &gFullscreenVS);
    vsBlob->Release();
    if (FAILED(hr)) { Log("SSIL: Failed to create VS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(genBlob->GetBufferPointer(), genBlob->GetBufferSize(), nullptr, &gSSILGenPS);
    genBlob->Release();
    if (FAILED(hr)) { Log("SSIL: Failed to create gen PS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(blurHBlob->GetBufferPointer(), blurHBlob->GetBufferSize(), nullptr, &gSSILBlurHPS);
    blurHBlob->Release();
    if (FAILED(hr)) { Log("SSIL: Failed to create blur H PS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(blurVBlob->GetBufferPointer(), blurVBlob->GetBufferSize(), nullptr, &gSSILBlurVPS);
    blurVBlob->Release();
    if (FAILED(hr)) { Log("SSIL: Failed to create blur V PS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(debugBlob->GetBufferPointer(), debugBlob->GetBufferSize(), nullptr, &gSSILDebugPS);
    debugBlob->Release();
    if (FAILED(hr)) { Log("SSIL: Failed to create debug PS: 0x%08X", hr); return false; }

    // Textures — R11G11B10_FLOAT for indirect light color
    if (!CreateRGBTexture(device, width, height, &gILTex, &gILRTV, &gILSRV))
        return false;
    if (!CreateRGBTexture(device, width, height, &gILBlurTex, &gILBlurRTV, &gILBlurSRV))
        return false;

    // No blend
    {
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].BlendEnable = FALSE;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &gNoBlend);
        if (FAILED(hr)) return false;
    }

    // No-depth DSS
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

    // Point-clamp sampler
    {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = device->CreateSamplerState(&desc, &gPointClampSampler);
        if (FAILED(hr)) return false;
    }

    // Constant buffer
    gSSILCB = host->CreateConstantBuffer(device, sizeof(SSILCBData));
    if (!gSSILCB) return false;

    gInitialized = true;
    Log("SSIL: SSILRenderer initialized successfully");
    return true;
}

void Shutdown()
{
#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gILTex);     SAFE_RELEASE(gILRTV);     SAFE_RELEASE(gILSRV);
    SAFE_RELEASE(gILBlurTex); SAFE_RELEASE(gILBlurRTV); SAFE_RELEASE(gILBlurSRV);
    SAFE_RELEASE(gFullscreenVS);
    SAFE_RELEASE(gSSILGenPS);  SAFE_RELEASE(gSSILBlurHPS);
    SAFE_RELEASE(gSSILBlurVPS); SAFE_RELEASE(gSSILDebugPS);
    SAFE_RELEASE(gNoBlend);
    SAFE_RELEASE(gNoDepthDSS);   SAFE_RELEASE(gNoCullRS);
    SAFE_RELEASE(gPointClampSampler); SAFE_RELEASE(gSSILCB);
#undef SAFE_RELEASE
    gInitialized = false;
    gHost = nullptr;
    Log("SSIL: SSILRenderer shut down");
}

void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight)
{
    if (newWidth == gWidth && newHeight == gHeight)
        return;

    Log("SSIL: Resolution changed: %ux%u -> %ux%u", gWidth, gHeight, newWidth, newHeight);

#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gILTex);     SAFE_RELEASE(gILRTV);     SAFE_RELEASE(gILSRV);
    SAFE_RELEASE(gILBlurTex); SAFE_RELEASE(gILBlurRTV); SAFE_RELEASE(gILBlurSRV);
#undef SAFE_RELEASE

    gWidth = newWidth;
    gHeight = newHeight;
    if (!CreateRGBTexture(device, newWidth, newHeight, &gILTex, &gILRTV, &gILSRV)
        || !CreateRGBTexture(device, newWidth, newHeight, &gILBlurTex, &gILBlurRTV, &gILBlurSRV))
    {
        Log("SSIL: WARNING: Failed to recreate IL textures after resolution change");
        gInitialized = false;
    }
}

bool IsInitialized()
{
    return gInitialized;
}

ID3D11ShaderResourceView* GetILSRV()
{
    return gILSRV;
}

ID3D11ShaderResourceView* RenderIL(ID3D11DeviceContext* ctx,
                                    ID3D11ShaderResourceView* depthSRV,
                                    ID3D11ShaderResourceView* albedoSRV,
                                    ID3D11ShaderResourceView* normalsSRV)
{
    if (!gInitialized || !ctx || !depthSRV || !albedoSRV || !gHost)
        return nullptr;

    // Detect resolution from depth texture
    {
        ID3D11Resource* res = nullptr;
        depthSRV->GetResource(&res);
        if (res)
        {
            ID3D11Texture2D* tex = nullptr;
            res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
            if (tex)
            {
                D3D11_TEXTURE2D_DESC desc;
                tex->GetDesc(&desc);
                if (desc.Width != gWidth || desc.Height != gHeight)
                {
                    ID3D11Device* device = nullptr;
                    ctx->GetDevice(&device);
                    if (device)
                    {
                        OnResolutionChanged(device, desc.Width, desc.Height);
                        device->Release();
                    }
                }
                tex->Release();
            }
            res->Release();
        }
    }

    gHost->SaveState(ctx);

    // Update constant buffer
    {
        SSILCBData cb = {};
        cb.viewportSize[0] = (float)gWidth;
        cb.viewportSize[1] = (float)gHeight;
        cb.invViewportSize[0] = 1.0f / (float)gWidth;
        cb.invViewportSize[1] = 1.0f / (float)gHeight;
        cb.tanHalfFov = gSSILConfig.tanHalfFov;
        cb.aspectRatio = (float)gWidth / (float)gHeight;
        cb.ilRadius = gSSILConfig.ilRadius;
        cb.ilStrength = gSSILConfig.ilStrength;
        cb.ilBias = gSSILConfig.ilBias;
        cb.ilMaxDepth = gSSILConfig.ilMaxDepth;
        cb.foregroundFade = gSSILConfig.foregroundFade;
        cb.falloffPower = gSSILConfig.falloffPower;
        cb.maxScreenRadius = gSSILConfig.maxScreenRadius;
        cb.minScreenRadius = gSSILConfig.minScreenRadius;
        cb.depthFadeStart = gSSILConfig.depthFadeStart;
        cb.colorBleeding = gSSILConfig.colorBleeding;
        cb.debugMode = gSSILConfig.debugView ? 1.0f : 0.0f;
        cb.blurSharpness = gSSILConfig.blurSharpness;
        int numDirs = (int)(gSSILConfig.sampleCount + 0.5f);
        if (numDirs < 1) numDirs = 1;
        cb.numDirections = (float)numDirs;
        cb.numSteps = 4.0f;
        gHost->UpdateConstantBuffer(ctx, gSSILCB, &cb, sizeof(cb));
    }

    // Common state for IL passes
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
    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };

    // Pass 1: Generate raw indirect light
    {
        float clearColor[4] = { 0, 0, 0, 1 };
        ctx->ClearRenderTargetView(gILRTV, clearColor);
        ctx->OMSetRenderTargets(1, &gILRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gSSILGenPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[3] = { depthSRV, albedoSRV, normalsSRV };
        ctx->PSSetShaderResources(0, 3, srvs);
        ctx->PSSetSamplers(0, 1, &gPointClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gSSILCB);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 3, nullSRVs);
    }

    // Pass 2: Horizontal blur
    {
        ctx->OMSetRenderTargets(1, &gILBlurRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gSSILBlurHPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { gILSRV, depthSRV };
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 1, &gPointClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gSSILCB);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* null2[2] = { nullptr, nullptr };
        ctx->PSSetShaderResources(0, 2, null2);
    }

    // Pass 3: Vertical blur
    {
        ctx->OMSetRenderTargets(1, &gILRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gSSILBlurVPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { gILBlurSRV, depthSRV };
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 1, &gPointClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gSSILCB);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* null2[2] = { nullptr, nullptr };
        ctx->PSSetShaderResources(0, 2, null2);
    }

    gHost->RestoreState(ctx);

    return gILSRV;
}

void RenderDebugOverlay(ID3D11DeviceContext* ctx,
                        ID3D11RenderTargetView* hdrRTV)
{
    if (!gInitialized || !ctx || !hdrRTV || !gSSILConfig.debugView || !gHost)
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
    ctx->OMSetRenderTargets(1, &hdrRTV, nullptr);
    ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
    ctx->PSSetShader(gSSILDebugPS, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &gILSRV);
    ctx->PSSetSamplers(0, 1, &gPointClampSampler);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);

    gHost->RestoreState(ctx);
}

} // namespace SSILRenderer
