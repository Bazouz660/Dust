#include "DOFRenderer.h"
#include "DOFConfig.h"
#include "DustLog.h"
#include <cstring>
#include <string>

namespace DOFRenderer
{

static bool gInitialized = false;
static UINT gWidth = 0;
static UINT gHeight = 0;
static const DustHostAPI* gHost = nullptr;
static std::string gShaderDir;

// CoC texture (full resolution)
static ID3D11Texture2D*          gCocTex = nullptr;
static ID3D11RenderTargetView*   gCocRTV = nullptr;
static ID3D11ShaderResourceView* gCocSRV = nullptr;

// Half-res scene (downsampled)
static ID3D11Texture2D*          gHalfTex = nullptr;
static ID3D11RenderTargetView*   gHalfRTV = nullptr;
static ID3D11ShaderResourceView* gHalfSRV = nullptr;

// Half-res blur temp (ping-pong)
static ID3D11Texture2D*          gBlurTempTex = nullptr;
static ID3D11RenderTargetView*   gBlurTempRTV = nullptr;
static ID3D11ShaderResourceView* gBlurTempSRV = nullptr;

// Pixel shaders (VS provided by host->DrawFullscreenTriangle)
static ID3D11PixelShader* gCocPS = nullptr;
static ID3D11PixelShader* gDownsamplePS = nullptr;
static ID3D11PixelShader* gBlurHPS = nullptr;
static ID3D11PixelShader* gBlurVPS = nullptr;
static ID3D11PixelShader* gCompositePS = nullptr;
static ID3D11PixelShader* gDebugPS = nullptr;

// Pipeline states
static ID3D11BlendState*        gNoBlend = nullptr;
static ID3D11DepthStencilState* gNoDepthDSS = nullptr;
static ID3D11RasterizerState*   gNoCullRS = nullptr;
static ID3D11SamplerState*      gPointClampSampler = nullptr;
static ID3D11SamplerState*      gLinearClampSampler = nullptr;

// Constant buffer
struct DOFCBData
{
    float texelSize[2];
    float focusDistance;
    float focusRange;
    float blurStrength;
    float blurRadius;
    float maxDepth;
    float _pad;
};
static ID3D11Buffer* gDOFCB = nullptr;

// ==================== Helpers ====================

static bool CreateTexture(ID3D11Device* device, UINT width, UINT height,
                          DXGI_FORMAT format,
                          ID3D11Texture2D** outTex,
                          ID3D11RenderTargetView** outRTV,
                          ID3D11ShaderResourceView** outSRV)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, outTex);
    if (FAILED(hr)) { Log("DOF: Failed to create texture (%ux%u): 0x%08X", width, height, hr); return false; }

    hr = device->CreateRenderTargetView(*outTex, nullptr, outRTV);
    if (FAILED(hr)) { Log("DOF: Failed to create RTV: 0x%08X", hr); return false; }

    hr = device->CreateShaderResourceView(*outTex, nullptr, outSRV);
    if (FAILED(hr)) { Log("DOF: Failed to create SRV: 0x%08X", hr); return false; }

    return true;
}

static void ReleaseTextures()
{
#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gCocTex);      SAFE_RELEASE(gCocRTV);      SAFE_RELEASE(gCocSRV);
    SAFE_RELEASE(gHalfTex);     SAFE_RELEASE(gHalfRTV);     SAFE_RELEASE(gHalfSRV);
    SAFE_RELEASE(gBlurTempTex); SAFE_RELEASE(gBlurTempRTV); SAFE_RELEASE(gBlurTempSRV);
#undef SAFE_RELEASE
}

static bool CreateSizedResources(ID3D11Device* device, UINT w, UINT h)
{
    UINT hw = w / 2;
    UINT hh = h / 2;
    if (hw < 1) hw = 1;
    if (hh < 1) hh = 1;

    // CoC map at full resolution (R16_FLOAT for smooth gradients)
    if (!CreateTexture(device, w, h, DXGI_FORMAT_R16_FLOAT,
                       &gCocTex, &gCocRTV, &gCocSRV))
        return false;

    // Half-res scene + blur temp (B8G8R8A8 to match LDR scene)
    if (!CreateTexture(device, hw, hh, DXGI_FORMAT_B8G8R8A8_UNORM,
                       &gHalfTex, &gHalfRTV, &gHalfSRV))
        return false;

    if (!CreateTexture(device, hw, hh, DXGI_FORMAT_B8G8R8A8_UNORM,
                       &gBlurTempTex, &gBlurTempRTV, &gBlurTempSRV))
        return false;

    return true;
}

// ==================== Public API ====================

bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir)
{
    if (gInitialized)
        return true;

    gHost = host;
    gShaderDir = std::string(effectDir) + "\\shaders\\";
    Log("DOF: Initializing (%ux%u), shaders: %s", width, height, gShaderDir.c_str());
    gWidth = width;
    gHeight = height;

    // Compile pixel shaders
    struct ShaderDef { const char* file; ID3D11PixelShader** ps; };
    ShaderDef shaders[] = {
        { "dof_coc_ps.hlsl",        &gCocPS },
        { "dof_downsample_ps.hlsl",  &gDownsamplePS },
        { "dof_blur_h_ps.hlsl",      &gBlurHPS },
        { "dof_blur_v_ps.hlsl",      &gBlurVPS },
        { "dof_composite_ps.hlsl",   &gCompositePS },
        { "dof_debug_ps.hlsl",       &gDebugPS },
    };

    for (auto& s : shaders)
    {
        std::string path = gShaderDir + s.file;
        ID3DBlob* blob = host->CompileShaderFromFile(path.c_str(), "main", "ps_5_0");
        if (!blob) { Log("DOF: Failed to compile %s", s.file); return false; }

        HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, s.ps);
        blob->Release();
        if (FAILED(hr)) { Log("DOF: Failed to create PS from %s: 0x%08X", s.file, hr); return false; }
    }

    // Textures
    if (!CreateSizedResources(device, width, height))
        return false;

    HRESULT hr;

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
    gDOFCB = host->CreateConstantBuffer(device, sizeof(DOFCBData));
    if (!gDOFCB) return false;

    gInitialized = true;
    Log("DOF: Initialized successfully");
    return true;
}

void Shutdown()
{
    ReleaseTextures();
#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }
    SAFE_RELEASE(gCocPS);        SAFE_RELEASE(gDownsamplePS);
    SAFE_RELEASE(gBlurHPS);      SAFE_RELEASE(gBlurVPS);
    SAFE_RELEASE(gCompositePS);  SAFE_RELEASE(gDebugPS);
    SAFE_RELEASE(gNoBlend);
    SAFE_RELEASE(gNoDepthDSS);   SAFE_RELEASE(gNoCullRS);
    SAFE_RELEASE(gPointClampSampler); SAFE_RELEASE(gLinearClampSampler);
    SAFE_RELEASE(gDOFCB);
#undef SAFE_RELEASE
    gInitialized = false;
    gHost = nullptr;
    Log("DOF: Shut down");
}

void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight)
{
    if (newWidth == gWidth && newHeight == gHeight)
        return;

    Log("DOF: Resolution changed: %ux%u -> %ux%u", gWidth, gHeight, newWidth, newHeight);

    ReleaseTextures();
    gWidth = newWidth;
    gHeight = newHeight;

    if (!CreateSizedResources(device, newWidth, newHeight))
    {
        Log("DOF: WARNING: Failed to recreate textures");
        gInitialized = false;
    }
}

bool IsInitialized()
{
    return gInitialized;
}

void Render(ID3D11DeviceContext* ctx,
            ID3D11ShaderResourceView* sceneCopySRV,
            ID3D11ShaderResourceView* depthSRV,
            ID3D11RenderTargetView* ldrRTV)
{
    if (!gInitialized || !ctx || !sceneCopySRV || !depthSRV || !ldrRTV || !gHost)
        return;

    gHost->SaveState(ctx);

    UINT hw = gWidth / 2;
    UINT hh = gHeight / 2;

    // Common state
    ctx->OMSetDepthStencilState(gNoDepthDSS, 0);
    ctx->RSSetState(gNoCullRS);
    ctx->OMSetBlendState(gNoBlend, nullptr, 0xFFFFFFFF);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };

    // --- Pass 1: Generate CoC map from depth ---
    {
        DOFCBData cb = {};
        cb.texelSize[0] = 1.0f / (float)gWidth;
        cb.texelSize[1] = 1.0f / (float)gHeight;
        cb.focusDistance = gDOFConfig.focusDistance;
        cb.focusRange = gDOFConfig.focusRange;
        cb.blurStrength = gDOFConfig.blurStrength;
        cb.blurRadius = gDOFConfig.blurRadius;
        cb.maxDepth = gDOFConfig.maxDepth;
        gHost->UpdateConstantBuffer(ctx, gDOFCB, &cb, sizeof(cb));

        ctx->OMSetRenderTargets(1, &gCocRTV, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, (float)gWidth, (float)gHeight, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShaderResources(0, 1, &depthSRV);
        ctx->PSSetSamplers(0, 1, &gPointClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gDOFCB);
        gHost->DrawFullscreenTriangle(ctx, gCocPS);
        ctx->PSSetShaderResources(0, 1, &nullSRV);
    }

    // --- Pass 2: Downsample scene to half resolution ---
    // Reuses CB from pass 1 (full-res texelSize is correct for source sampling)
    {
        ctx->OMSetRenderTargets(1, &gHalfRTV, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, (float)hw, (float)hh, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShaderResources(0, 1, &sceneCopySRV);
        ctx->PSSetSamplers(0, 1, &gLinearClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gDOFCB);
        gHost->DrawFullscreenTriangle(ctx, gDownsamplePS);
        ctx->PSSetShaderResources(0, 1, &nullSRV);
    }

    // --- Pass 3: Horizontal blur at half resolution ---
    {
        DOFCBData cb = {};
        cb.texelSize[0] = 1.0f / (float)hw;
        cb.texelSize[1] = 1.0f / (float)hh;
        cb.blurRadius = gDOFConfig.blurRadius;
        gHost->UpdateConstantBuffer(ctx, gDOFCB, &cb, sizeof(cb));

        ctx->OMSetRenderTargets(1, &gBlurTempRTV, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, (float)hw, (float)hh, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShaderResources(0, 1, &gHalfSRV);
        ctx->PSSetSamplers(0, 1, &gLinearClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gDOFCB);
        gHost->DrawFullscreenTriangle(ctx, gBlurHPS);
        ctx->PSSetShaderResources(0, 1, &nullSRV);
    }

    // --- Pass 4: Vertical blur at half resolution ---
    {
        ctx->OMSetRenderTargets(1, &gHalfRTV, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, (float)hw, (float)hh, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShaderResources(0, 1, &gBlurTempSRV);
        ctx->PSSetSamplers(0, 1, &gLinearClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gDOFCB);
        gHost->DrawFullscreenTriangle(ctx, gBlurVPS);
        ctx->PSSetShaderResources(0, 1, &nullSRV);
    }

    // --- Pass 5: Composite (sharp + blurred + CoC → LDR) ---
    // Restore full-res texelSize (changed in pass 3 to half-res)
    {
        DOFCBData cb = {};
        cb.texelSize[0] = 1.0f / (float)gWidth;
        cb.texelSize[1] = 1.0f / (float)gHeight;
        gHost->UpdateConstantBuffer(ctx, gDOFCB, &cb, sizeof(cb));

        ctx->OMSetRenderTargets(1, &ldrRTV, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, (float)gWidth, (float)gHeight, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ID3D11ShaderResourceView* srvs[3] = { sceneCopySRV, gHalfSRV, gCocSRV };
        ctx->PSSetShaderResources(0, 3, srvs);
        ctx->PSSetSamplers(0, 1, &gLinearClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gDOFCB);
        gHost->DrawFullscreenTriangle(ctx, gCompositePS);
        ctx->PSSetShaderResources(0, 3, nullSRVs);
    }

    gHost->RestoreState(ctx);
}

void RenderDebugOverlay(ID3D11DeviceContext* ctx,
                        ID3D11ShaderResourceView* depthSRV,
                        ID3D11RenderTargetView* ldrRTV)
{
    if (!gInitialized || !ctx || !depthSRV || !ldrRTV || !gHost)
        return;

    gHost->SaveState(ctx);

    // Generate CoC first
    {
        DOFCBData cb = {};
        cb.texelSize[0] = 1.0f / (float)gWidth;
        cb.texelSize[1] = 1.0f / (float)gHeight;
        cb.focusDistance = gDOFConfig.focusDistance;
        cb.focusRange = gDOFConfig.focusRange;
        cb.blurStrength = gDOFConfig.blurStrength;
        cb.maxDepth = gDOFConfig.maxDepth;
        gHost->UpdateConstantBuffer(ctx, gDOFCB, &cb, sizeof(cb));

        ctx->OMSetDepthStencilState(gNoDepthDSS, 0);
        ctx->RSSetState(gNoCullRS);
        ctx->OMSetBlendState(gNoBlend, nullptr, 0xFFFFFFFF);

        ctx->OMSetRenderTargets(1, &gCocRTV, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, (float)gWidth, (float)gHeight, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShaderResources(0, 1, &depthSRV);
        ctx->PSSetSamplers(0, 1, &gPointClampSampler);
        ctx->PSSetConstantBuffers(0, 1, &gDOFCB);
        gHost->DrawFullscreenTriangle(ctx, gCocPS);
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSRV);
    }

    // Draw CoC visualization
    {
        ctx->OMSetRenderTargets(1, &ldrRTV, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, (float)gWidth, (float)gHeight, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShaderResources(0, 1, &gCocSRV);
        ctx->PSSetSamplers(0, 1, &gPointClampSampler);
        gHost->DrawFullscreenTriangle(ctx, gDebugPS);
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSRV);
    }

    gHost->RestoreState(ctx);
}

} // namespace DOFRenderer
