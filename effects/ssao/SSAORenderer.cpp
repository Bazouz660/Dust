#include "SSAORenderer.h"
#include "SSAOConfig.h"
#include "DustLog.h"
#include <cstring>
#include <string>

namespace SSAORenderer
{

static bool gInitialized = false;
static UINT gWidth = 0;
static UINT gHeight = 0;
static const DustHostAPI* gHost = nullptr;
static std::string gShaderDir;

// Full-res AO textures (ping-pong for gen + blur)
static ID3D11Texture2D*          gAoTex = nullptr;
static ID3D11RenderTargetView*   gAoRTV = nullptr;
static ID3D11ShaderResourceView* gAoSRV = nullptr;

static ID3D11Texture2D*          gAoBlurTex = nullptr;
static ID3D11RenderTargetView*   gAoBlurRTV = nullptr;
static ID3D11ShaderResourceView* gAoBlurSRV = nullptr;

// Half-res AO textures (used when halfResolution is enabled)
static UINT gHalfWidth = 0;
static UINT gHalfHeight = 0;
static ID3D11Texture2D*          gAoHalfTex = nullptr;
static ID3D11RenderTargetView*   gAoHalfRTV = nullptr;
static ID3D11ShaderResourceView* gAoHalfSRV = nullptr;

static ID3D11Texture2D*          gAoHalfBlurTex = nullptr;
static ID3D11RenderTargetView*   gAoHalfBlurRTV = nullptr;
static ID3D11ShaderResourceView* gAoHalfBlurSRV = nullptr;

// Shaders
static ID3D11VertexShader* gFullscreenVS = nullptr;
static ID3D11PixelShader*  gSSAOGenPS = nullptr;
static ID3D11PixelShader*  gSSAOBlurHPS = nullptr;
static ID3D11PixelShader*  gSSAOBlurVPS = nullptr;
static ID3D11PixelShader*  gSSAOUpsamplePS = nullptr;
static ID3D11PixelShader*  gSSAODebugPS = nullptr;

// Pipeline states
static ID3D11BlendState*        gNoBlend = nullptr;
static ID3D11DepthStencilState* gNoDepthDSS = nullptr;
static ID3D11RasterizerState*   gNoCullRS = nullptr;
static ID3D11SamplerState*      gPointClampSampler = nullptr;
static ID3D11SamplerState*      gLinearClampSampler = nullptr;

struct SSAOCBData
{
    float viewportSize[2];
    float invViewportSize[2];
    float tanHalfFov;
    float aspectRatio;
    float filterRadius;
    float debugMode;
    float aoRadius;
    float aoStrength;
    float aoBias;
    float aoMaxDepth;
    float foregroundFade;
    float falloffPower;
    float maxScreenRadius;
    float minScreenRadius;
    float depthFadeStart;
    float blurSharpness;
    float nightCompensation;
    float noiseScale;
    float numDirections;
    float numSteps;
};
static ID3D11Buffer* gSSAOCB = nullptr;

// ==================== Helpers ====================

static bool CreateR8Texture(ID3D11Device* device, UINT width, UINT height,
                            ID3D11Texture2D** outTex,
                            ID3D11RenderTargetView** outRTV,
                            ID3D11ShaderResourceView** outSRV)
{
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, outTex);
    if (FAILED(hr)) { Log("Failed to create R8 texture (%ux%u): 0x%08X", width, height, hr); return false; }

    hr = device->CreateRenderTargetView(*outTex, nullptr, outRTV);
    if (FAILED(hr)) { Log("Failed to create R8 RTV: 0x%08X", hr); return false; }

    hr = device->CreateShaderResourceView(*outTex, nullptr, outSRV);
    if (FAILED(hr)) { Log("Failed to create R8 SRV: 0x%08X", hr); return false; }

    return true;
}

// ==================== Public API ====================

bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir)
{
    if (gInitialized)
        return true;

    gHost = host;
    gShaderDir = std::string(effectDir) + "\\shaders\\";
    Log("Initializing SSAORenderer (%ux%u), shaders: %s", width, height, gShaderDir.c_str());
    gWidth = width;
    gHeight = height;

    // Compile all shaders from .hlsl files
    ID3DBlob* vsBlob = host->CompileShaderFromFile((gShaderDir + "fullscreen_vs.hlsl").c_str(), "main", "vs_5_0");
    if (!vsBlob) return false;

    ID3DBlob* genBlob = host->CompileShaderFromFile((gShaderDir + "ssao_gen_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!genBlob) { vsBlob->Release(); return false; }

    ID3DBlob* blurHBlob = host->CompileShaderFromFile((gShaderDir + "ssao_blur_h_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!blurHBlob) { vsBlob->Release(); genBlob->Release(); return false; }

    ID3DBlob* blurVBlob = host->CompileShaderFromFile((gShaderDir + "ssao_blur_v_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!blurVBlob) { vsBlob->Release(); genBlob->Release(); blurHBlob->Release(); return false; }

    ID3DBlob* upsampleBlob = host->CompileShaderFromFile((gShaderDir + "ssao_upsample_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!upsampleBlob) { vsBlob->Release(); genBlob->Release(); blurHBlob->Release(); blurVBlob->Release(); return false; }

    ID3DBlob* debugBlob = host->CompileShaderFromFile((gShaderDir + "ssao_debug_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!debugBlob) { vsBlob->Release(); genBlob->Release(); blurHBlob->Release(); blurVBlob->Release(); upsampleBlob->Release(); return false; }

    HRESULT hr;

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &gFullscreenVS);
    vsBlob->Release();
    if (FAILED(hr)) { Log("Failed to create VS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(genBlob->GetBufferPointer(), genBlob->GetBufferSize(), nullptr, &gSSAOGenPS);
    genBlob->Release();
    if (FAILED(hr)) { Log("Failed to create gen PS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(blurHBlob->GetBufferPointer(), blurHBlob->GetBufferSize(), nullptr, &gSSAOBlurHPS);
    blurHBlob->Release();
    if (FAILED(hr)) { Log("Failed to create blur H PS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(blurVBlob->GetBufferPointer(), blurVBlob->GetBufferSize(), nullptr, &gSSAOBlurVPS);
    blurVBlob->Release();
    if (FAILED(hr)) { Log("Failed to create blur V PS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(upsampleBlob->GetBufferPointer(), upsampleBlob->GetBufferSize(), nullptr, &gSSAOUpsamplePS);
    upsampleBlob->Release();
    if (FAILED(hr)) { Log("Failed to create upsample PS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(debugBlob->GetBufferPointer(), debugBlob->GetBufferSize(), nullptr, &gSSAODebugPS);
    debugBlob->Release();
    if (FAILED(hr)) { Log("Failed to create debug PS: 0x%08X", hr); return false; }

    // Full-res textures (always needed)
    if (!CreateR8Texture(device, width, height, &gAoTex, &gAoRTV, &gAoSRV))
        return false;
    if (!CreateR8Texture(device, width, height, &gAoBlurTex, &gAoBlurRTV, &gAoBlurSRV))
        return false;

    // Half-res textures (for optional half-res mode)
    gHalfWidth = (width + 1) / 2;
    gHalfHeight = (height + 1) / 2;
    if (!CreateR8Texture(device, gHalfWidth, gHalfHeight, &gAoHalfTex, &gAoHalfRTV, &gAoHalfSRV))
        return false;
    if (!CreateR8Texture(device, gHalfWidth, gHalfHeight, &gAoHalfBlurTex, &gAoHalfBlurRTV, &gAoHalfBlurSRV))
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

    // Linear-clamp sampler (for half-res upsample)
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
    gSSAOCB = host->CreateConstantBuffer(device, sizeof(SSAOCBData));
    if (!gSSAOCB) return false;

    gInitialized = true;
    Log("SSAORenderer initialized successfully");
    return true;
}

void Shutdown()
{
#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gAoTex);         SAFE_RELEASE(gAoRTV);         SAFE_RELEASE(gAoSRV);
    SAFE_RELEASE(gAoBlurTex);     SAFE_RELEASE(gAoBlurRTV);     SAFE_RELEASE(gAoBlurSRV);
    SAFE_RELEASE(gAoHalfTex);     SAFE_RELEASE(gAoHalfRTV);     SAFE_RELEASE(gAoHalfSRV);
    SAFE_RELEASE(gAoHalfBlurTex); SAFE_RELEASE(gAoHalfBlurRTV); SAFE_RELEASE(gAoHalfBlurSRV);
    SAFE_RELEASE(gFullscreenVS);
    SAFE_RELEASE(gSSAOGenPS);  SAFE_RELEASE(gSSAOBlurHPS);
    SAFE_RELEASE(gSSAOBlurVPS); SAFE_RELEASE(gSSAOUpsamplePS); SAFE_RELEASE(gSSAODebugPS);
    SAFE_RELEASE(gNoBlend);
    SAFE_RELEASE(gNoDepthDSS);   SAFE_RELEASE(gNoCullRS);
    SAFE_RELEASE(gPointClampSampler); SAFE_RELEASE(gLinearClampSampler); SAFE_RELEASE(gSSAOCB);
#undef SAFE_RELEASE
    gInitialized = false;
    gHost = nullptr;
    Log("SSAORenderer shut down");
}

void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight)
{
    if (newWidth == gWidth && newHeight == gHeight)
        return;

    Log("Resolution changed: %ux%u -> %ux%u", gWidth, gHeight, newWidth, newHeight);

#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gAoTex);         SAFE_RELEASE(gAoRTV);         SAFE_RELEASE(gAoSRV);
    SAFE_RELEASE(gAoBlurTex);     SAFE_RELEASE(gAoBlurRTV);     SAFE_RELEASE(gAoBlurSRV);
    SAFE_RELEASE(gAoHalfTex);     SAFE_RELEASE(gAoHalfRTV);     SAFE_RELEASE(gAoHalfSRV);
    SAFE_RELEASE(gAoHalfBlurTex); SAFE_RELEASE(gAoHalfBlurRTV); SAFE_RELEASE(gAoHalfBlurSRV);
#undef SAFE_RELEASE

    gWidth = newWidth;
    gHeight = newHeight;
    gHalfWidth = (newWidth + 1) / 2;
    gHalfHeight = (newHeight + 1) / 2;
    if (!CreateR8Texture(device, newWidth, newHeight, &gAoTex, &gAoRTV, &gAoSRV)
        || !CreateR8Texture(device, newWidth, newHeight, &gAoBlurTex, &gAoBlurRTV, &gAoBlurSRV)
        || !CreateR8Texture(device, gHalfWidth, gHalfHeight, &gAoHalfTex, &gAoHalfRTV, &gAoHalfSRV)
        || !CreateR8Texture(device, gHalfWidth, gHalfHeight, &gAoHalfBlurTex, &gAoHalfBlurRTV, &gAoHalfBlurSRV))
    {
        Log("WARNING: Failed to recreate AO textures after resolution change");
        gInitialized = false;
    }
}

bool IsInitialized()
{
    return gInitialized;
}

ID3D11ShaderResourceView* GetAoSRV()
{
    return gAoSRV;
}

float GetLastGpuTimeMs()
{
    return 0.0f;  // Framework handles timing via DUST_FLAG_FRAMEWORK_TIMING
}

ID3D11ShaderResourceView* RenderAO(ID3D11DeviceContext* ctx,
                                    ID3D11ShaderResourceView* depthSRV)
{
    if (!gInitialized || !ctx || !depthSRV || !gHost)
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

    // Directions slider (4–12), steps always 6 for consistent AO intensity
    int numDirs = (int)(gSSAOConfig.sampleCount + 0.5f);
    if (numDirs < 4) numDirs = 4;
    if (numDirs > 12) numDirs = 12;

    // Update constant buffer
    {
        SSAOCBData cb = {};
        cb.viewportSize[0] = (float)gWidth;
        cb.viewportSize[1] = (float)gHeight;
        cb.invViewportSize[0] = 1.0f / (float)gWidth;
        cb.invViewportSize[1] = 1.0f / (float)gHeight;
        cb.tanHalfFov = gSSAOConfig.tanHalfFov;
        cb.aspectRatio = (float)gWidth / (float)gHeight;
        cb.filterRadius = gSSAOConfig.filterRadius;
        cb.debugMode = gSSAOConfig.debugView ? 1.0f : 0.0f;
        cb.aoRadius = gSSAOConfig.aoRadius;
        cb.aoStrength = gSSAOConfig.aoStrength;
        cb.aoBias = gSSAOConfig.aoBias;
        cb.aoMaxDepth = gSSAOConfig.aoMaxDepth;
        cb.foregroundFade = gSSAOConfig.foregroundFade;
        cb.falloffPower = gSSAOConfig.falloffPower;
        cb.maxScreenRadius = gSSAOConfig.maxScreenRadius;
        cb.minScreenRadius = gSSAOConfig.minScreenRadius;
        cb.depthFadeStart = gSSAOConfig.depthFadeStart;
        cb.blurSharpness = gSSAOConfig.blurSharpness;
        cb.nightCompensation = gSSAOConfig.nightCompensation;
        cb.noiseScale = 1.0f;
        cb.numDirections = (float)numDirs;
        cb.numSteps = 6.0f;
        gHost->UpdateConstantBuffer(ctx, gSSAOCB, &cb, sizeof(cb));
    }

    // Common state for all passes (set once)
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(gFullscreenVS, nullptr, 0);
    ctx->RSSetState(gNoCullRS);
    ctx->OMSetDepthStencilState(gNoDepthDSS, 0);

    float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
    ctx->PSSetSamplers(0, 1, &gPointClampSampler);
    ctx->PSSetConstantBuffers(0, 1, &gSSAOCB);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)gWidth;
    vp.Height = (float)gHeight;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };

    // Pass 1: Generate raw AO (fullscreen triangle covers all pixels, no clear needed)
    {
        ctx->OMSetRenderTargets(1, &gAoRTV, nullptr);
        ctx->PSSetShader(gSSAOGenPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { depthSRV, nullptr };
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 2, nullSRVs);
    }

    // Pass 2: Horizontal blur
    {
        ctx->OMSetRenderTargets(1, &gAoBlurRTV, nullptr);
        ctx->PSSetShader(gSSAOBlurHPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { gAoSRV, depthSRV };
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 2, nullSRVs);
    }

    // Pass 3: Vertical blur
    {
        ctx->OMSetRenderTargets(1, &gAoRTV, nullptr);
        ctx->PSSetShader(gSSAOBlurVPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { gAoBlurSRV, depthSRV };
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 2, nullSRVs);
    }

    // Restore state via host API
    gHost->RestoreState(ctx);

    return gAoSRV;
}

void RenderDebugOverlay(ID3D11DeviceContext* ctx,
                        ID3D11RenderTargetView* hdrRTV)
{
    if (!gInitialized || !ctx || !hdrRTV || !gSSAOConfig.debugView || !gHost)
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
    ctx->PSSetShader(gSSAODebugPS, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &gAoSRV);
    ctx->PSSetSamplers(0, 1, &gPointClampSampler);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);

    gHost->RestoreState(ctx);
}

} // namespace SSAORenderer
