#include "SSSRenderer.h"
#include "SSSConfig.h"
#include "DustLog.h"
#include <cstring>
#include <cmath>
#include <string>

namespace SSSRenderer
{

static bool gInitialized = false;
static UINT gWidth = 0;
static UINT gHeight = 0;
static const DustHostAPI* gHost = nullptr;
static std::string gShaderDir;

// SSS textures (ping-pong for gen + blur)
static ID3D11Texture2D*          gSssTex = nullptr;
static ID3D11RenderTargetView*   gSssRTV = nullptr;
static ID3D11ShaderResourceView* gSssSRV = nullptr;

static ID3D11Texture2D*          gSssBlurTex = nullptr;
static ID3D11RenderTargetView*   gSssBlurRTV = nullptr;
static ID3D11ShaderResourceView* gSssBlurSRV = nullptr;

// Shaders
static ID3D11VertexShader* gFullscreenVS = nullptr;
static ID3D11PixelShader*  gSSSGenPS = nullptr;
static ID3D11PixelShader*  gSSSBlurHPS = nullptr;
static ID3D11PixelShader*  gSSSBlurVPS = nullptr;
static ID3D11PixelShader*  gSSSCompositePS = nullptr;
static ID3D11PixelShader*  gSSSDebugPS = nullptr;

// Pipeline states
static ID3D11BlendState*        gNoBlend = nullptr;
static ID3D11BlendState*        gMultiplyBlend = nullptr;
static ID3D11DepthStencilState* gNoDepthDSS = nullptr;
static ID3D11RasterizerState*   gNoCullRS = nullptr;
static ID3D11SamplerState*      gPointClampSampler = nullptr;
static ID3D11SamplerState*      gLinearClampSampler = nullptr;

// Constant buffer
struct SSSCBData
{
    float viewportSize[2];
    float invViewportSize[2];
    float tanHalfFov;
    float aspectRatio;
    float maxDistance;
    float thickness;
    float sunDirView[3];
    float strength;
    float stepCount;
    float maxDepth;
    float depthBias;
    float blurSharpness;
};
static ID3D11Buffer* gSSSCB = nullptr;

// Light data extracted from game's constant buffer
static float gCachedSunDir[3] = {0.0f, 1.0f, 0.0f};        // World-space sun direction (default: up)
static float gCachedInverseView[16] = {                       // Identity matrix
    1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
};
static float gSunDirView[3] = {0.0f, 1.0f, 0.0f};           // View-space sun direction
static bool  gHasValidLightData = false;

// Staging buffer for reading game's constant buffer
static ID3D11Buffer* gStagingCB = nullptr;

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
    if (FAILED(hr)) { Log("SSS: Failed to create R8 texture (%ux%u): 0x%08X", width, height, hr); return false; }

    hr = device->CreateRenderTargetView(*outTex, nullptr, outRTV);
    if (FAILED(hr)) { Log("SSS: Failed to create R8 RTV: 0x%08X", hr); return false; }

    hr = device->CreateShaderResourceView(*outTex, nullptr, outSRV);
    if (FAILED(hr)) { Log("SSS: Failed to create R8 SRV: 0x%08X", hr); return false; }

    return true;
}

static void ComputeViewSpaceSunDir()
{
    // inverseView is view-to-world transform (row-major in CB).
    // To transform a direction from world to view space:
    // viewDir = transpose(rotation_part(inverseView)) * worldDir
    //
    // For row-major storage: m[row*4 + col]
    //   row 0: m[0], m[1], m[2], m[3]
    //   row 1: m[4], m[5], m[6], m[7]
    //   row 2: m[8], m[9], m[10], m[11]
    //
    // transpose(M3x3) * v =
    //   [m[0] m[4] m[8] ] [v0]
    //   [m[1] m[5] m[9] ] [v1]
    //   [m[2] m[6] m[10]] [v2]

    float* m = gCachedInverseView;
    float* s = gCachedSunDir;

    gSunDirView[0] = m[0]*s[0] + m[4]*s[1] + m[8]*s[2];
    gSunDirView[1] = m[1]*s[0] + m[5]*s[1] + m[9]*s[2];
    gSunDirView[2] = m[2]*s[0] + m[6]*s[1] + m[10]*s[2];

    float len = sqrtf(gSunDirView[0]*gSunDirView[0] +
                      gSunDirView[1]*gSunDirView[1] +
                      gSunDirView[2]*gSunDirView[2]);
    if (len > 0.0001f)
    {
        gSunDirView[0] /= len;
        gSunDirView[1] /= len;
        gSunDirView[2] /= len;
    }
}

// ==================== Public API ====================

bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir)
{
    if (gInitialized)
        return true;

    gHost = host;
    gShaderDir = std::string(effectDir) + "\\shaders\\";
    Log("SSS: Initializing SSSRenderer (%ux%u), shaders: %s", width, height, gShaderDir.c_str());
    gWidth = width;
    gHeight = height;

    // Compile shaders
    ID3DBlob* vsBlob = host->CompileShaderFromFile((gShaderDir + "fullscreen_vs.hlsl").c_str(), "main", "vs_5_0");
    if (!vsBlob) return false;

    ID3DBlob* genBlob = host->CompileShaderFromFile((gShaderDir + "sss_gen_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!genBlob) { vsBlob->Release(); return false; }

    ID3DBlob* blurHBlob = host->CompileShaderFromFile((gShaderDir + "sss_blur_h_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!blurHBlob) { vsBlob->Release(); genBlob->Release(); return false; }

    ID3DBlob* blurVBlob = host->CompileShaderFromFile((gShaderDir + "sss_blur_v_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!blurVBlob) { vsBlob->Release(); genBlob->Release(); blurHBlob->Release(); return false; }

    ID3DBlob* compositeBlob = host->CompileShaderFromFile((gShaderDir + "sss_composite_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!compositeBlob) { vsBlob->Release(); genBlob->Release(); blurHBlob->Release(); blurVBlob->Release(); return false; }

    ID3DBlob* debugBlob = host->CompileShaderFromFile((gShaderDir + "sss_debug_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!debugBlob) { vsBlob->Release(); genBlob->Release(); blurHBlob->Release(); blurVBlob->Release(); compositeBlob->Release(); return false; }

    HRESULT hr;

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &gFullscreenVS);
    vsBlob->Release();
    if (FAILED(hr)) { Log("SSS: Failed to create VS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(genBlob->GetBufferPointer(), genBlob->GetBufferSize(), nullptr, &gSSSGenPS);
    genBlob->Release();
    if (FAILED(hr)) { Log("SSS: Failed to create gen PS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(blurHBlob->GetBufferPointer(), blurHBlob->GetBufferSize(), nullptr, &gSSSBlurHPS);
    blurHBlob->Release();
    if (FAILED(hr)) { Log("SSS: Failed to create blur H PS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(blurVBlob->GetBufferPointer(), blurVBlob->GetBufferSize(), nullptr, &gSSSBlurVPS);
    blurVBlob->Release();
    if (FAILED(hr)) { Log("SSS: Failed to create blur V PS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(compositeBlob->GetBufferPointer(), compositeBlob->GetBufferSize(), nullptr, &gSSSCompositePS);
    compositeBlob->Release();
    if (FAILED(hr)) { Log("SSS: Failed to create composite PS: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(debugBlob->GetBufferPointer(), debugBlob->GetBufferSize(), nullptr, &gSSSDebugPS);
    debugBlob->Release();
    if (FAILED(hr)) { Log("SSS: Failed to create debug PS: 0x%08X", hr); return false; }

    // Textures
    if (!CreateR8Texture(device, width, height, &gSssTex, &gSssRTV, &gSssSRV))
        return false;
    if (!CreateR8Texture(device, width, height, &gSssBlurTex, &gSssBlurRTV, &gSssBlurSRV))
        return false;

    // No-blend state
    {
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].BlendEnable = FALSE;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &gNoBlend);
        if (FAILED(hr)) return false;
    }

    // Multiply blend state (DstColor * SrcColor)
    {
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].BlendEnable = TRUE;
        desc.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;
        desc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &gMultiplyBlend);
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
    gSSSCB = host->CreateConstantBuffer(device, sizeof(SSSCBData));
    if (!gSSSCB) return false;

    gInitialized = true;
    Log("SSS: SSSRenderer initialized successfully");
    return true;
}

void Shutdown()
{
#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gSssTex);     SAFE_RELEASE(gSssRTV);     SAFE_RELEASE(gSssSRV);
    SAFE_RELEASE(gSssBlurTex); SAFE_RELEASE(gSssBlurRTV); SAFE_RELEASE(gSssBlurSRV);
    SAFE_RELEASE(gFullscreenVS);
    SAFE_RELEASE(gSSSGenPS);    SAFE_RELEASE(gSSSBlurHPS);
    SAFE_RELEASE(gSSSBlurVPS);  SAFE_RELEASE(gSSSCompositePS); SAFE_RELEASE(gSSSDebugPS);
    SAFE_RELEASE(gNoBlend);     SAFE_RELEASE(gMultiplyBlend);
    SAFE_RELEASE(gNoDepthDSS);  SAFE_RELEASE(gNoCullRS);
    SAFE_RELEASE(gPointClampSampler); SAFE_RELEASE(gLinearClampSampler);
    SAFE_RELEASE(gSSSCB);
    SAFE_RELEASE(gStagingCB);
#undef SAFE_RELEASE
    gInitialized = false;
    gHasValidLightData = false;
    gHost = nullptr;
    Log("SSS: SSSRenderer shut down");
}

void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight)
{
    if (newWidth == gWidth && newHeight == gHeight)
        return;

    Log("SSS: Resolution changed: %ux%u -> %ux%u", gWidth, gHeight, newWidth, newHeight);

#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gSssTex);     SAFE_RELEASE(gSssRTV);     SAFE_RELEASE(gSssSRV);
    SAFE_RELEASE(gSssBlurTex); SAFE_RELEASE(gSssBlurRTV); SAFE_RELEASE(gSssBlurSRV);
#undef SAFE_RELEASE

    gWidth = newWidth;
    gHeight = newHeight;
    if (!CreateR8Texture(device, newWidth, newHeight, &gSssTex, &gSssRTV, &gSssSRV)
        || !CreateR8Texture(device, newWidth, newHeight, &gSssBlurTex, &gSssBlurRTV, &gSssBlurSRV))
    {
        Log("SSS: WARNING: Failed to recreate textures after resolution change");
        gInitialized = false;
    }
}

bool IsInitialized()
{
    return gInitialized;
}

bool HasValidLightData()
{
    return gHasValidLightData;
}

void ExtractLightData(ID3D11DeviceContext* ctx)
{
    // Read the PS constant buffer that the game has set for the deferred lighting draw.
    // Layout (based on deferred.hlsl parameter order):
    //   c0  (offset 0):   float3 sunDirection
    //   c8  (offset 128): float4x4 inverseView
    ID3D11Buffer* psCB = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &psCB);
    if (!psCB) return;

    D3D11_BUFFER_DESC cbDesc;
    psCB->GetDesc(&cbDesc);

    // Need at least 192 bytes to read sunDirection (0-12) and inverseView (128-192)
    if (cbDesc.ByteWidth < 192)
    {
        psCB->Release();
        return;
    }

    // Create or recreate staging buffer if needed
    if (!gStagingCB)
    {
        D3D11_BUFFER_DESC stagingDesc = cbDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.BindFlags = 0;
        stagingDesc.MiscFlags = 0;

        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        if (device)
        {
            device->CreateBuffer(&stagingDesc, nullptr, &gStagingCB);
            device->Release();
        }
    }

    if (!gStagingCB)
    {
        psCB->Release();
        return;
    }

    ctx->CopyResource(gStagingCB, psCB);
    psCB->Release();

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx->Map(gStagingCB, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        float* data = (float*)mapped.pData;

        // sunDirection at c0 (offset 0, 3 floats)
        gCachedSunDir[0] = data[0];
        gCachedSunDir[1] = data[1];
        gCachedSunDir[2] = data[2];

        // inverseView at c8 (offset 32 floats = 128 bytes, 16 floats)
        memcpy(gCachedInverseView, data + 32, 64);

        ctx->Unmap(gStagingCB, 0);

        // Validate — sun direction should be a reasonable unit vector
        float len = sqrtf(gCachedSunDir[0]*gCachedSunDir[0] +
                          gCachedSunDir[1]*gCachedSunDir[1] +
                          gCachedSunDir[2]*gCachedSunDir[2]);
        if (len > 0.1f && len < 10.0f)
        {
            ComputeViewSpaceSunDir();
            gHasValidLightData = true;
        }
    }
}

ID3D11ShaderResourceView* RenderSSS(ID3D11DeviceContext* ctx,
                                     ID3D11ShaderResourceView* depthSRV)
{
    if (!gInitialized || !ctx || !depthSRV || !gHost || !gHasValidLightData)
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
        SSSCBData cb = {};
        cb.viewportSize[0] = (float)gWidth;
        cb.viewportSize[1] = (float)gHeight;
        cb.invViewportSize[0] = 1.0f / (float)gWidth;
        cb.invViewportSize[1] = 1.0f / (float)gHeight;
        cb.tanHalfFov = gSSSConfig.tanHalfFov;
        cb.aspectRatio = (float)gWidth / (float)gHeight;
        cb.maxDistance = gSSSConfig.maxDistance;
        cb.thickness = gSSSConfig.thickness;
        cb.sunDirView[0] = gSunDirView[0];
        cb.sunDirView[1] = gSunDirView[1];
        cb.sunDirView[2] = gSunDirView[2];
        cb.strength = gSSSConfig.strength;
        cb.stepCount = gSSSConfig.stepCount;
        cb.maxDepth = gSSSConfig.maxDepth;
        cb.depthBias = gSSSConfig.depthBias;
        cb.blurSharpness = gSSSConfig.blurSharpness;
        gHost->UpdateConstantBuffer(ctx, gSSSCB, &cb, sizeof(cb));
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
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };

    // Pass 1: Generate raw SSS shadow mask
    {
        float clearColor[4] = { 1, 1, 1, 1 };
        ctx->ClearRenderTargetView(gSssRTV, clearColor);
        ctx->OMSetRenderTargets(1, &gSssRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gSSSGenPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[1] = { depthSRV };
        ctx->PSSetShaderResources(0, 1, srvs);
        ctx->PSSetSamplers(0, 1, &gPointClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gSSSCB);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, (ID3D11ShaderResourceView**)nullSRVs);
    }

    // Pass 2: Horizontal blur
    {
        ctx->OMSetRenderTargets(1, &gSssBlurRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gSSSBlurHPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { gSssSRV, depthSRV };
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 1, &gPointClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gSSSCB);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 2, nullSRVs);
    }

    // Pass 3: Vertical blur
    {
        ctx->OMSetRenderTargets(1, &gSssRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gSSSBlurVPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { gSssBlurSRV, depthSRV };
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 1, &gPointClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gSSSCB);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 2, nullSRVs);
    }

    gHost->RestoreState(ctx);

    return gSssSRV;
}

void Composite(ID3D11DeviceContext* ctx,
               ID3D11RenderTargetView* hdrRTV)
{
    if (!gInitialized || !ctx || !hdrRTV || !gHost || !gSssSRV)
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
    ctx->OMSetBlendState(gMultiplyBlend, blendFactor, 0xFFFFFFFF);
    ctx->PSSetShader(gSSSCompositePS, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &gSssSRV);
    ctx->PSSetSamplers(0, 1, &gLinearClampSampler);
    ctx->PSSetConstantBuffers(0, 1, &gSSSCB);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);

    gHost->RestoreState(ctx);
}

void RenderDebugOverlay(ID3D11DeviceContext* ctx,
                        ID3D11RenderTargetView* hdrRTV)
{
    if (!gInitialized || !ctx || !hdrRTV || !gSSSConfig.debugView || !gHost)
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
    ctx->PSSetShader(gSSSDebugPS, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &gSssSRV);
    ctx->PSSetSamplers(0, 1, &gPointClampSampler);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);

    gHost->RestoreState(ctx);
}

} // namespace SSSRenderer
