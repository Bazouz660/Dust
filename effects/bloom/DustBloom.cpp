// DustBloom.cpp - HDR bloom effect plugin for Dust (API v3)
// Extracts bright areas from the HDR scene, builds a gaussian bloom
// via progressive downsample/upsample, then composites additively.

#include "../../src/DustAPI.h"
#include "DustLog.h"

#include <d3d11.h>
#include <cstring>
#include <string>

DustLogFn gLogFn = nullptr;
static const DustHostAPI* gHost = nullptr;
static ID3D11Device* gDevice = nullptr;
static HMODULE gPluginModule = nullptr;

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

// ==================== Config ====================

struct BloomConfig {
    bool  enabled   = true;
    float intensity = 0.65f;
    float threshold = 0.0f;
    float radius    = 1.0f;
    float spread    = 0.5f;   // 0 = tight sharp halo, 1 = wide soft glow
    bool  debugView = false;
};

static BloomConfig gConfig;

// ==================== GPU Resources ====================

static ID3D11PixelShader* gExtractPS         = nullptr;
static ID3D11PixelShader* gDownsampleKarisPS = nullptr;
static ID3D11PixelShader* gDownsamplePS      = nullptr;
static ID3D11PixelShader* gUpsamplePS   = nullptr;
static ID3D11PixelShader* gCompositePS  = nullptr;

static ID3D11Buffer*             gCB             = nullptr;
static ID3D11SamplerState*       gLinearSampler  = nullptr;
static ID3D11BlendState*         gNoBlend        = nullptr;
static ID3D11BlendState*         gAdditiveBlend  = nullptr;
static ID3D11DepthStencilState*  gNoDepth        = nullptr;
static ID3D11RasterizerState*    gRasterState    = nullptr;

// CB layout (must match HLSL)
struct BloomCB {
    float texelSizeX, texelSizeY;
    float threshold;
    float intensity;
    float radius;
    float mipWeight;
    float pad0, pad1;
};

// ==================== Mip Chain ====================

#define BLOOM_MIP_COUNT 8

struct BloomMip {
    ID3D11Texture2D*          tex = nullptr;
    ID3D11RenderTargetView*   rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    uint32_t width = 0, height = 0;
};

static BloomMip gMips[BLOOM_MIP_COUNT];

static void ReleaseMips()
{
    for (int i = 0; i < BLOOM_MIP_COUNT; i++)
    {
        if (gMips[i].srv) { gMips[i].srv->Release(); gMips[i].srv = nullptr; }
        if (gMips[i].rtv) { gMips[i].rtv->Release(); gMips[i].rtv = nullptr; }
        if (gMips[i].tex) { gMips[i].tex->Release(); gMips[i].tex = nullptr; }
        gMips[i].width = gMips[i].height = 0;
    }
}

static bool CreateMips(ID3D11Device* device, uint32_t w, uint32_t h)
{
    ReleaseMips();

    uint32_t mw = w / 2;
    uint32_t mh = h / 2;

    for (int i = 0; i < BLOOM_MIP_COUNT; i++)
    {
        if (mw < 1) mw = 1;
        if (mh < 1) mh = 1;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width            = mw;
        desc.Height           = mh;
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = DXGI_FORMAT_R11G11B10_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DEFAULT;
        desc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &gMips[i].tex);
        if (FAILED(hr)) { Log("Bloom: Failed to create mip %d tex: 0x%08X", i, hr); return false; }

        hr = device->CreateRenderTargetView(gMips[i].tex, nullptr, &gMips[i].rtv);
        if (FAILED(hr)) { Log("Bloom: Failed to create mip %d RTV: 0x%08X", i, hr); return false; }

        hr = device->CreateShaderResourceView(gMips[i].tex, nullptr, &gMips[i].srv);
        if (FAILED(hr)) { Log("Bloom: Failed to create mip %d SRV: 0x%08X", i, hr); return false; }

        gMips[i].width  = mw;
        gMips[i].height = mh;

        mw /= 2;
        mh /= 2;
    }
    return true;
}

// ==================== HDR capture ====================

static ID3D11ShaderResourceView* gHdrCopySRV = nullptr;

// ==================== Init / Shutdown ====================

static std::string gShaderDir;

static ID3D11PixelShader* CompilePS(const char* filename, const char* name)
{
    std::string path = gShaderDir + filename;
    ID3DBlob* blob = gHost->CompileShaderFromFile(path.c_str(), "main", "ps_5_0");
    if (!blob) { Log("Bloom: Failed to compile %s from %s", name, path.c_str()); return nullptr; }

    ID3D11PixelShader* ps = nullptr;
    HRESULT hr = gDevice->CreatePixelShader(blob->GetBufferPointer(),
                                             blob->GetBufferSize(), nullptr, &ps);
    blob->Release();
    if (FAILED(hr)) { Log("Bloom: Failed to create %s PS: 0x%08X", name, hr); return nullptr; }
    return ps;
}

static int BloomInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
    gHost = host;
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gDevice = device;
    gShaderDir = GetPluginDir() + "\\shaders\\";

    // Compile shaders from .hlsl files
    gExtractPS         = CompilePS("bloom_extract_ps.hlsl",          "extract");
    gDownsampleKarisPS = CompilePS("bloom_downsample_karis_ps.hlsl", "downsample_karis");
    gDownsamplePS      = CompilePS("bloom_downsample_ps.hlsl",       "downsample");
    gUpsamplePS        = CompilePS("bloom_upsample_ps.hlsl",         "upsample");
    gCompositePS       = CompilePS("bloom_composite_ps.hlsl",        "composite");
    if (!gExtractPS || !gDownsampleKarisPS || !gDownsamplePS || !gUpsamplePS || !gCompositePS)
        return -1;

    // Constant buffer
    gCB = host->CreateConstantBuffer(device, sizeof(BloomCB));
    if (!gCB) return -2;

    // Linear sampler (clamp)
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &gLinearSampler))) return -3;

    // No-blend state (replace)
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &gNoBlend))) return -4;

    // Additive blend state (ONE + ONE)
    bd.RenderTarget[0].BlendEnable    = TRUE;
    bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend      = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    if (FAILED(device->CreateBlendState(&bd, &gAdditiveBlend))) return -5;

    // Depth stencil (disabled)
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&dsd, &gNoDepth))) return -6;

    // Rasterizer
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    if (FAILED(device->CreateRasterizerState(&rd, &gRasterState))) return -7;

    // Mip chain
    if (!CreateMips(device, width, height)) return -8;

    Log("Bloom: Initialized (%ux%u, %d mip levels)", width, height, BLOOM_MIP_COUNT);
    return 0;
}

static void BloomShutdown()
{
    ReleaseMips();
    if (gRasterState)   { gRasterState->Release();   gRasterState = nullptr; }
    if (gNoDepth)       { gNoDepth->Release();       gNoDepth = nullptr; }
    if (gAdditiveBlend) { gAdditiveBlend->Release(); gAdditiveBlend = nullptr; }
    if (gNoBlend)       { gNoBlend->Release();       gNoBlend = nullptr; }
    if (gLinearSampler) { gLinearSampler->Release();  gLinearSampler = nullptr; }
    if (gCB)            { gCB->Release();             gCB = nullptr; }
    if (gCompositePS)       { gCompositePS->Release();       gCompositePS = nullptr; }
    if (gUpsamplePS)        { gUpsamplePS->Release();        gUpsamplePS = nullptr; }
    if (gDownsamplePS)      { gDownsamplePS->Release();      gDownsamplePS = nullptr; }
    if (gDownsampleKarisPS) { gDownsampleKarisPS->Release(); gDownsampleKarisPS = nullptr; }
    if (gExtractPS)         { gExtractPS->Release();         gExtractPS = nullptr; }
    gDevice = nullptr;
    Log("Bloom: Shut down");
}

static void BloomOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    CreateMips(device, w, h);
    Log("Bloom: Resolution changed to %ux%u", w, h);
}

// ==================== Per-frame ====================

// postExecute: full bloom pipeline (runs at POST_LIGHTING, composites onto HDR)
static void BloomPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gConfig.enabled)
        return;

    // Capture HDR scene and get HDR render target
    gHdrCopySRV = host->GetSceneCopy(ctx->context, DUST_RESOURCE_HDR_RT);
    if (!gHdrCopySRV) return;

    ID3D11RenderTargetView* hdrRTV = host->GetRTV(DUST_RESOURCE_HDR_RT);
    if (!hdrRTV) { gHdrCopySRV = nullptr; return; }

    ID3D11DeviceContext* dc = ctx->context;
    host->SaveState(dc);

    // Common state for all passes
    dc->OMSetDepthStencilState(gNoDepth, 0);
    dc->RSSetState(gRasterState);
    dc->PSSetSamplers(0, 1, &gLinearSampler);
    dc->PSSetConstantBuffers(0, 1, &gCB);

    // --- Pass 1: Karis downsample + prefilter (HDR -> mip[0]) ---
    {
        BloomCB cb = {};
        cb.texelSizeX = 1.0f / (float)ctx->width;
        cb.texelSizeY = 1.0f / (float)ctx->height;
        cb.threshold  = gConfig.threshold;
        host->UpdateConstantBuffer(dc, gCB, &cb, sizeof(cb));

        dc->OMSetBlendState(gNoBlend, nullptr, 0xFFFFFFFF);
        dc->OMSetRenderTargets(1, &gMips[0].rtv, nullptr);

        D3D11_VIEWPORT vp = { 0, 0, (float)gMips[0].width, (float)gMips[0].height, 0, 1 };
        dc->RSSetViewports(1, &vp);

        dc->PSSetShaderResources(0, 1, &gHdrCopySRV);
        host->DrawFullscreenTriangle(dc, gDownsampleKarisPS);
    }

    // --- Passes 2..N: Downsample chain ---
    for (int i = 0; i < BLOOM_MIP_COUNT - 1; i++)
    {
        BloomCB cb = {};
        cb.texelSizeX = 1.0f / (float)gMips[i].width;
        cb.texelSizeY = 1.0f / (float)gMips[i].height;
        host->UpdateConstantBuffer(dc, gCB, &cb, sizeof(cb));

        dc->OMSetRenderTargets(1, &gMips[i + 1].rtv, nullptr);

        D3D11_VIEWPORT vp = { 0, 0, (float)gMips[i + 1].width, (float)gMips[i + 1].height, 0, 1 };
        dc->RSSetViewports(1, &vp);

        dc->PSSetShaderResources(0, 1, &gMips[i].srv);
        host->DrawFullscreenTriangle(dc, gDownsamplePS);
    }

    // --- Passes N+1..2N: Upsample chain (additive onto each larger mip) ---
    dc->OMSetBlendState(gAdditiveBlend, nullptr, 0xFFFFFFFF);

    // Generate mip weights from spread: gaussian curve centered based on spread value
    float mipWeights[BLOOM_MIP_COUNT];
    {
        float center = gConfig.spread * (BLOOM_MIP_COUNT - 1);
        float sigma  = BLOOM_MIP_COUNT * 0.35f;
        for (int m = 0; m < BLOOM_MIP_COUNT; m++)
        {
            float d = (float)m - center;
            mipWeights[m] = expf(-0.5f * (d * d) / (sigma * sigma));
        }
    }

    for (int i = BLOOM_MIP_COUNT - 1; i > 0; i--)
    {
        BloomCB cb = {};
        cb.texelSizeX = 1.0f / (float)gMips[i].width;
        cb.texelSizeY = 1.0f / (float)gMips[i].height;
        cb.radius     = gConfig.radius;
        cb.mipWeight  = mipWeights[i];
        host->UpdateConstantBuffer(dc, gCB, &cb, sizeof(cb));

        dc->OMSetRenderTargets(1, &gMips[i - 1].rtv, nullptr);

        D3D11_VIEWPORT vp = { 0, 0, (float)gMips[i - 1].width, (float)gMips[i - 1].height, 0, 1 };
        dc->RSSetViewports(1, &vp);

        dc->PSSetShaderResources(0, 1, &gMips[i].srv);
        host->DrawFullscreenTriangle(dc, gUpsamplePS);
    }

    // --- Final: Composite bloom additively onto HDR RT ---
    // Tonemapper will naturally compress the bloom, no screen blend needed.
    {
        BloomCB cb = {};
        cb.texelSizeX = 1.0f / (float)gMips[0].width;
        cb.texelSizeY = 1.0f / (float)gMips[0].height;
        cb.intensity  = gConfig.intensity;
        host->UpdateConstantBuffer(dc, gCB, &cb, sizeof(cb));

        if (gConfig.debugView)
            dc->OMSetBlendState(gNoBlend, nullptr, 0xFFFFFFFF);
        // else: still additive from upsample loop

        dc->OMSetRenderTargets(1, &hdrRTV, nullptr);

        D3D11_VIEWPORT vp = { 0, 0, (float)ctx->width, (float)ctx->height, 0, 1 };
        dc->RSSetViewports(1, &vp);

        dc->PSSetShaderResources(0, 1, &gMips[0].srv);
        host->DrawFullscreenTriangle(dc, gCompositePS);
    }

    // Clean up bindings
    ID3D11ShaderResourceView* nullSRV = nullptr;
    dc->PSSetShaderResources(0, 1, &nullSRV);

    host->RestoreState(dc);
    gHdrCopySRV = nullptr;
}

static int BloomIsEnabled()
{
    return 1;
}

// ==================== GUI Settings ====================

static DustSettingDesc gBloomSettingsArray[] = {
    { "Enabled",    DUST_SETTING_BOOL,  &gConfig.enabled,   0.0f, 1.0f, "Enabled" },
    { "Intensity",  DUST_SETTING_FLOAT, &gConfig.intensity, 0.0f, 2.0f, "Intensity" },
    { "Threshold",  DUST_SETTING_FLOAT, &gConfig.threshold, 0.0f, 5.0f, "Threshold" },
    { "Radius",     DUST_SETTING_FLOAT, &gConfig.radius,   0.5f, 3.0f, "Radius" },
    { "Spread",     DUST_SETTING_FLOAT, &gConfig.spread,   0.0f, 1.0f, "Spread" },
    { "Debug View", DUST_SETTING_BOOL,  &gConfig.debugView, 0.0f, 1.0f, "DebugView" },
};

// ==================== Plugin entry ====================

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion          = DUST_API_VERSION;
    desc->name                = "Bloom";
    desc->injectionPoint      = DUST_INJECT_POST_LIGHTING;
    desc->Init                = BloomInit;
    desc->Shutdown            = BloomShutdown;
    desc->OnResolutionChanged = BloomOnResolutionChanged;
    desc->postExecute         = BloomPostExecute;
    desc->IsEnabled           = BloomIsEnabled;
    desc->settings            = gBloomSettingsArray;
    desc->settingCount        = sizeof(gBloomSettingsArray) / sizeof(gBloomSettingsArray[0]);

    desc->flags               = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection       = "Bloom";
    desc->priority            = 60;  // Run after Outline (50) at POST_LIGHTING

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        gPluginModule = hModule;
    }
    return TRUE;
}
