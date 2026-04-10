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

// AO textures (ping-pong for gen + blur)
static ID3D11Texture2D*          gAoTex = nullptr;
static ID3D11RenderTargetView*   gAoRTV = nullptr;
static ID3D11ShaderResourceView* gAoSRV = nullptr;

static ID3D11Texture2D*          gAoBlurTex = nullptr;
static ID3D11RenderTargetView*   gAoBlurRTV = nullptr;
static ID3D11ShaderResourceView* gAoBlurSRV = nullptr;

// Shaders
static ID3D11VertexShader* gFullscreenVS = nullptr;
static ID3D11PixelShader*  gSSAOGenPS = nullptr;
static ID3D11PixelShader*  gSSAOBlurHPS = nullptr;
static ID3D11PixelShader*  gSSAOBlurVPS = nullptr;
static ID3D11PixelShader*  gSSAODebugPS = nullptr;

// Pipeline states
static ID3D11BlendState*        gNoBlend = nullptr;
static ID3D11DepthStencilState* gNoDepthDSS = nullptr;
static ID3D11RasterizerState*   gNoCullRS = nullptr;
static ID3D11SamplerState*      gPointClampSampler = nullptr;

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
    float _pad[1]; // pad to 16-byte alignment
};
static ID3D11Buffer* gSSAOCB = nullptr;

// GPU timing
static ID3D11Query* gTimestampBegin = nullptr;
static ID3D11Query* gTimestampEnd = nullptr;
static ID3D11Query* gTimestampDisjoint = nullptr;
static int gTimingFrameCount = 0;
static double gTimingAccumMs = 0.0;
static const int TIMING_LOG_INTERVAL = 300;
static float gLastGpuTimeMs = 0.0f;
static bool gTimingActive = false;

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

    ID3DBlob* debugBlob = host->CompileShaderFromFile((gShaderDir + "ssao_debug_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!debugBlob) { vsBlob->Release(); genBlob->Release(); blurHBlob->Release(); blurVBlob->Release(); return false; }

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

    hr = device->CreatePixelShader(debugBlob->GetBufferPointer(), debugBlob->GetBufferSize(), nullptr, &gSSAODebugPS);
    debugBlob->Release();
    if (FAILED(hr)) { Log("Failed to create debug PS: 0x%08X", hr); return false; }

    // Textures
    if (!CreateR8Texture(device, width, height, &gAoTex, &gAoRTV, &gAoSRV))
        return false;
    if (!CreateR8Texture(device, width, height, &gAoBlurTex, &gAoBlurRTV, &gAoBlurSRV))
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
    gSSAOCB = host->CreateConstantBuffer(device, sizeof(SSAOCBData));
    if (!gSSAOCB) return false;

    // GPU timestamp queries for internal performance logging
    {
        D3D11_QUERY_DESC qd = {};
        qd.Query = D3D11_QUERY_TIMESTAMP;
        device->CreateQuery(&qd, &gTimestampBegin);
        device->CreateQuery(&qd, &gTimestampEnd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        device->CreateQuery(&qd, &gTimestampDisjoint);
    }

    gInitialized = true;
    Log("SSAORenderer initialized successfully");
    return true;
}

void Shutdown()
{
#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gAoTex);     SAFE_RELEASE(gAoRTV);     SAFE_RELEASE(gAoSRV);
    SAFE_RELEASE(gAoBlurTex); SAFE_RELEASE(gAoBlurRTV); SAFE_RELEASE(gAoBlurSRV);
    SAFE_RELEASE(gFullscreenVS);
    SAFE_RELEASE(gSSAOGenPS);  SAFE_RELEASE(gSSAOBlurHPS);
    SAFE_RELEASE(gSSAOBlurVPS); SAFE_RELEASE(gSSAODebugPS);
    SAFE_RELEASE(gNoBlend);
    SAFE_RELEASE(gNoDepthDSS);   SAFE_RELEASE(gNoCullRS);
    SAFE_RELEASE(gPointClampSampler); SAFE_RELEASE(gSSAOCB);
    SAFE_RELEASE(gTimestampBegin); SAFE_RELEASE(gTimestampEnd); SAFE_RELEASE(gTimestampDisjoint);
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
    SAFE_RELEASE(gAoTex);     SAFE_RELEASE(gAoRTV);     SAFE_RELEASE(gAoSRV);
    SAFE_RELEASE(gAoBlurTex); SAFE_RELEASE(gAoBlurRTV); SAFE_RELEASE(gAoBlurSRV);
#undef SAFE_RELEASE

    gWidth = newWidth;
    gHeight = newHeight;
    if (!CreateR8Texture(device, newWidth, newHeight, &gAoTex, &gAoRTV, &gAoSRV)
        || !CreateR8Texture(device, newWidth, newHeight, &gAoBlurTex, &gAoBlurRTV, &gAoBlurSRV))
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
    return gLastGpuTimeMs;
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
        gHost->UpdateConstantBuffer(ctx, gSSAOCB, &cb, sizeof(cb));
    }

    // Common state for AO passes
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

    // Collect previous frame's GPU timing
    if (gTimingActive && gTimestampDisjoint)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData;
        UINT64 tsBegin = 0, tsEnd = 0;
        if (ctx->GetData(gTimestampDisjoint, &disjointData, sizeof(disjointData), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && !disjointData.Disjoint
            && ctx->GetData(gTimestampBegin, &tsBegin, sizeof(tsBegin), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && ctx->GetData(gTimestampEnd, &tsEnd, sizeof(tsEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
        {
            double ms = (double)(tsEnd - tsBegin) / (double)disjointData.Frequency * 1000.0;
            gLastGpuTimeMs = (float)ms;
            gTimingAccumMs += ms;
            gTimingFrameCount++;

            if (gTimingFrameCount >= TIMING_LOG_INTERVAL)
            {
                double avgMs = gTimingAccumMs / gTimingFrameCount;
                Log("SSAO timing: %.2f ms avg (%d frames, %ux%u)", avgMs, gTimingFrameCount, gWidth, gHeight);
                gTimingAccumMs = 0.0;
                gTimingFrameCount = 0;
            }
        }
        gTimingActive = false;
    }

    // Begin GPU timing
    if (gTimestampDisjoint)
    {
        ctx->Begin(gTimestampDisjoint);
        ctx->End(gTimestampBegin);
    }

    // Pass 1: Generate raw AO
    {
        float clearColor[4] = { 1, 1, 1, 1 };
        ctx->ClearRenderTargetView(gAoRTV, clearColor);
        ctx->OMSetRenderTargets(1, &gAoRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gSSAOGenPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { depthSRV, nullptr };
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 1, &gPointClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gSSAOCB);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 2, nullSRVs);
    }

    // Pass 2: Horizontal blur
    {
        ctx->OMSetRenderTargets(1, &gAoBlurRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gSSAOBlurHPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { gAoSRV, depthSRV };
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 1, &gPointClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gSSAOCB);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 2, nullSRVs);
    }

    // Pass 3: Vertical blur
    {
        ctx->OMSetRenderTargets(1, &gAoRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->PSSetShader(gSSAOBlurVPS, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { gAoBlurSRV, depthSRV };
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 1, &gPointClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gSSAOCB);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 2, nullSRVs);
    }

    // End GPU timing (results collected next frame)
    if (gTimestampDisjoint)
    {
        ctx->End(gTimestampEnd);
        ctx->End(gTimestampDisjoint);
        gTimingActive = true;
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
