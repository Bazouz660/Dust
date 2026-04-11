// DustLUT.cpp - HDR color pipeline for Dust (API v3)
// Captures HDR scene before game tonemaps, then applies:
//   exposure -> ACES tonemap -> LUT color grading -> dither
// in a single full-precision pass. Only quantization is the final 8-bit write.

#include "../../src/DustAPI.h"
#include "DustLog.h"

#include <d3d11.h>
#include <cstring>
#include <cmath>
#include <algorithm>
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

struct LUTConfig {
    bool enabled       = true;
    float intensity    = 1.0f;
    float exposure     = 0.692f;  // EV stops (-3 to 3), acts as compensation when auto-exposure is on

    // Auto-exposure
    bool autoExposure       = true;
    float adaptationSpeed   = 10.0f;  // How fast exposure adapts (higher = faster)
    float minAutoExposure   = -2.5f;  // Minimum auto EV
    float maxAutoExposure   = 1.106f; // Maximum auto EV

    // Lift / Gamma / Gain (applied in log space)
    float lift         = 0.0f;    // Shadows offset (-0.1 to 0.1)
    float gamma        = 0.9f;    // Midtone power (0.8 to 1.2)
    float gain         = 0.985f;  // Highlight multiplier (0.8 to 1.2)

    // Color balance
    float contrast     = 0.934f;  // 0.5 to 1.5
    float saturation   = 1.05f;   // 0.0 to 2.0
    float temperature  = 0.085f;  // -1.0 (cool) to 1.0 (warm)
    float tint         = -0.005f; // -1.0 (green) to 1.0 (magenta)

    // Split toning
    float shadowR      = -0.01f;  // Shadow color offset (-0.1 to 0.1)
    float shadowG      = 0.0f;
    float shadowB      = 0.0f;
    float highlightR   = 0.01f;   // Highlight color offset
    float highlightG   = 0.0f;
    float highlightB   = 0.0f;
};

static LUTConfig gConfig;

// ==================== LUT Generation ====================

static float Clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

static void GradeColor(float r, float g, float b, float& outR, float& outG, float& outB)
{
    // Temperature / tint (simple approximation)
    r += gConfig.temperature * 0.1f;
    b -= gConfig.temperature * 0.1f;
    g += gConfig.tint * 0.05f;

    // Contrast (pivot at 0.5)
    r = ((r - 0.5f) * gConfig.contrast) + 0.5f;
    g = ((g - 0.5f) * gConfig.contrast) + 0.5f;
    b = ((b - 0.5f) * gConfig.contrast) + 0.5f;

    // Lift / Gamma / Gain
    r = (r * gConfig.gain + gConfig.lift);
    g = (g * gConfig.gain + gConfig.lift);
    b = (b * gConfig.gain + gConfig.lift);

    r = r > 0.0f ? powf(r, 1.0f / gConfig.gamma) : 0.0f;
    g = g > 0.0f ? powf(g, 1.0f / gConfig.gamma) : 0.0f;
    b = b > 0.0f ? powf(b, 1.0f / gConfig.gamma) : 0.0f;

    // Saturation
    float luma = r * 0.2126f + g * 0.7152f + b * 0.0722f;
    r = luma + (r - luma) * gConfig.saturation;
    g = luma + (g - luma) * gConfig.saturation;
    b = luma + (b - luma) * gConfig.saturation;

    // Split toning — blend shadow/highlight colors based on luminance
    float shadowWeight = 1.0f - Clamp01(luma * 2.0f);  // strong in darks
    float highlightWeight = Clamp01(luma * 2.0f - 1.0f); // strong in brights

    r += gConfig.shadowR * shadowWeight + gConfig.highlightR * highlightWeight;
    g += gConfig.shadowG * shadowWeight + gConfig.highlightG * highlightWeight;
    b += gConfig.shadowB * shadowWeight + gConfig.highlightB * highlightWeight;

    outR = Clamp01(r);
    outG = Clamp01(g);
    outB = Clamp01(b);
}

static const UINT LUT_SIZE = 32;
static ID3D11Texture2D* gLutTex = nullptr;
static ID3D11ShaderResourceView* gLutSRV = nullptr;

static void ReleaseLUT()
{
    if (gLutSRV) { gLutSRV->Release(); gLutSRV = nullptr; }
    if (gLutTex) { gLutTex->Release(); gLutTex = nullptr; }
}

static bool GenerateLUT(ID3D11Device* device)
{
    ReleaseLUT();

    const UINT stripWidth = LUT_SIZE * LUT_SIZE;
    const UINT stripHeight = LUT_SIZE;

    // Full float precision — no 8-bit quantization in the LUT itself
    float* pixels = new float[stripWidth * stripHeight * 4];

    for (UINT bIdx = 0; bIdx < LUT_SIZE; bIdx++)
    {
        for (UINT gIdx = 0; gIdx < LUT_SIZE; gIdx++)
        {
            for (UINT rIdx = 0; rIdx < LUT_SIZE; rIdx++)
            {
                float r = (float)rIdx / (LUT_SIZE - 1);
                float g = (float)gIdx / (LUT_SIZE - 1);
                float b = (float)bIdx / (LUT_SIZE - 1);

                float outR, outG, outB;
                GradeColor(r, g, b, outR, outG, outB);

                UINT x = bIdx * LUT_SIZE + rIdx;
                UINT y = gIdx;
                UINT idx = (y * stripWidth + x) * 4;
                pixels[idx + 0] = outR;
                pixels[idx + 1] = outG;
                pixels[idx + 2] = outB;
                pixels[idx + 3] = 1.0f;
            }
        }
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = stripWidth;
    desc.Height = stripHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = pixels;
    init.SysMemPitch = stripWidth * 16;  // 4 floats * 4 bytes

    HRESULT hr = device->CreateTexture2D(&desc, &init, &gLutTex);
    delete[] pixels;

    if (FAILED(hr))
    {
        Log("LUT: Failed to create LUT texture: 0x%08X", hr);
        return false;
    }

    hr = device->CreateShaderResourceView(gLutTex, nullptr, &gLutSRV);
    if (FAILED(hr))
    {
        Log("LUT: Failed to create LUT SRV: 0x%08X", hr);
        gLutTex->Release(); gLutTex = nullptr;
        return false;
    }

    return true;
}

// ==================== GPU Resources ====================

static ID3D11PixelShader* gPS = nullptr;
static ID3D11Buffer* gCB = nullptr;
static ID3D11SamplerState* gPointSampler = nullptr;
static ID3D11SamplerState* gLinearSampler = nullptr;
static ID3D11BlendState* gNoBlend = nullptr;
static ID3D11DepthStencilState* gNoDepth = nullptr;
static ID3D11RasterizerState* gRasterState = nullptr;

struct LUTParams {
    float intensity;
    float lutSize;
    float exposure;
    float pad;
};

// HDR scene captured in preExecute, used in postExecute
static ID3D11ShaderResourceView* gHdrCopySRV = nullptr;

// ==================== Auto-Exposure ====================

static ID3D11PixelShader* gLumPS = nullptr;          // Log-luminance extraction shader
static ID3D11Texture2D* gLumTex = nullptr;            // Mipmap chain for luminance averaging
static ID3D11ShaderResourceView* gLumSRV = nullptr;
static ID3D11RenderTargetView* gLumRTV = nullptr;     // RTV for mip 0
static ID3D11Texture2D* gLumStagingTex = nullptr;     // 1x1 staging for CPU readback
static uint32_t gLumMipCount = 0;
static float gAdaptedExposure = -0.7f;                // Current smoothed auto-exposure EV
static LARGE_INTEGER gLastFrameTime = {};
static LARGE_INTEGER gPerfFreq = {};

static void ReleaseAutoExposure()
{
    if (gLumRTV)        { gLumRTV->Release();        gLumRTV = nullptr; }
    if (gLumSRV)        { gLumSRV->Release();        gLumSRV = nullptr; }
    if (gLumTex)        { gLumTex->Release();         gLumTex = nullptr; }
    if (gLumStagingTex) { gLumStagingTex->Release();  gLumStagingTex = nullptr; }
}

static uint32_t CalcMipCount(uint32_t w, uint32_t h)
{
    uint32_t dim = w > h ? w : h;
    uint32_t mips = 1;
    while (dim > 1) { dim >>= 1; mips++; }
    return mips;
}

static bool CreateLuminanceResources(ID3D11Device* device, uint32_t width, uint32_t height)
{
    ReleaseAutoExposure();

    gLumMipCount = CalcMipCount(width, height);

    // Luminance texture with full mip chain
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = gLumMipCount;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &gLumTex);
    if (FAILED(hr)) { Log("LUT: Failed to create luminance texture: 0x%08X", hr); return false; }

    // SRV for full mip chain (needed by GenerateMips)
    hr = device->CreateShaderResourceView(gLumTex, nullptr, &gLumSRV);
    if (FAILED(hr)) { Log("LUT: Failed to create luminance SRV: 0x%08X", hr); return false; }

    // RTV for mip 0 only
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;
    hr = device->CreateRenderTargetView(gLumTex, &rtvDesc, &gLumRTV);
    if (FAILED(hr)) { Log("LUT: Failed to create luminance RTV: 0x%08X", hr); return false; }

    // 1x1 staging texture for CPU readback
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = 1;
    stagingDesc.Height = 1;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_R32_FLOAT;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = device->CreateTexture2D(&stagingDesc, nullptr, &gLumStagingTex);
    if (FAILED(hr)) { Log("LUT: Failed to create luminance staging texture: 0x%08X", hr); return false; }

    return true;
}

// ==================== Init / Shutdown ====================

static int LUTInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
    gHost = host;
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gDevice = device;

    // Compile pixel shader from file
    std::string shaderPath = GetPluginDir() + "\\shaders\\lut_ps.hlsl";
    ID3DBlob* psBlob = host->CompileShaderFromFile(shaderPath.c_str(), "main", "ps_5_0");
    if (!psBlob) return -1;

    HRESULT hr = device->CreatePixelShader(psBlob->GetBufferPointer(),
                                            psBlob->GetBufferSize(), nullptr, &gPS);
    psBlob->Release();
    if (FAILED(hr)) return -2;

    // Constant buffer via framework
    gCB = host->CreateConstantBuffer(device, sizeof(LUTParams));
    if (!gCB) return -3;

    // Samplers
    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = device->CreateSamplerState(&sd, &gPointSampler);
    if (FAILED(hr)) return -4;

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    hr = device->CreateSamplerState(&sd, &gLinearSampler);
    if (FAILED(hr)) return -5;

    // Blend state (no blending)
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&bd, &gNoBlend);
    if (FAILED(hr)) return -6;

    // Depth stencil (disabled)
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    hr = device->CreateDepthStencilState(&dsd, &gNoDepth);
    if (FAILED(hr)) return -7;

    // Rasterizer
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    hr = device->CreateRasterizerState(&rd, &gRasterState);
    if (FAILED(hr)) return -8;

    // Compile log-luminance shader for auto-exposure
    std::string lumShaderPath = GetPluginDir() + "\\shaders\\lum_avg_ps.hlsl";
    ID3DBlob* lumBlob = host->CompileShaderFromFile(lumShaderPath.c_str(), "main", "ps_5_0");
    if (!lumBlob) { Log("LUT: Failed to compile lum_avg_ps.hlsl"); return -9; }

    hr = device->CreatePixelShader(lumBlob->GetBufferPointer(),
                                    lumBlob->GetBufferSize(), nullptr, &gLumPS);
    lumBlob->Release();
    if (FAILED(hr)) return -10;

    // Create luminance reduction resources
    if (!CreateLuminanceResources(device, width, height))
        return -11;

    // Init timing for adaptation
    QueryPerformanceFrequency(&gPerfFreq);
    QueryPerformanceCounter(&gLastFrameTime);
    gAdaptedExposure = gConfig.exposure;

    // Generate LUT from config settings
    if (!GenerateLUT(device))
        return -12;

    Log("LUT: Initialized (%ux%u)", width, height);
    return 0;
}

static void LUTShutdown()
{
    ReleaseLUT();
    ReleaseAutoExposure();
    if (gLumPS)        { gLumPS->Release();          gLumPS = nullptr; }
    if (gRasterState)  { gRasterState->Release();   gRasterState = nullptr; }
    if (gNoDepth)      { gNoDepth->Release();       gNoDepth = nullptr; }
    if (gNoBlend)      { gNoBlend->Release();       gNoBlend = nullptr; }
    if (gLinearSampler){ gLinearSampler->Release();  gLinearSampler = nullptr; }
    if (gPointSampler) { gPointSampler->Release();   gPointSampler = nullptr; }
    if (gCB)           { gCB->Release();             gCB = nullptr; }
    if (gPS)           { gPS->Release();             gPS = nullptr; }
    gDevice = nullptr;
    Log("LUT: Shut down");
}

static void LUTOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    CreateLuminanceResources(device, w, h);
    Log("LUT: Resolution changed to %ux%u", w, h);
}

// ==================== Per-frame ====================

// preExecute: fires BEFORE the game's tonemap draw.
// HDR RT is still intact — capture it for our own tonemap pass.
// Also computes average luminance for auto-exposure.
static void LUTPreExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gConfig.enabled)
        return;

    gHdrCopySRV = host->GetSceneCopy(ctx->context, DUST_RESOURCE_HDR_RT);

    // Compute average scene luminance for auto-exposure
    if (gConfig.autoExposure && gHdrCopySRV && gLumPS && gLumTex)
    {
        ID3D11DeviceContext* dc = ctx->context;
        host->SaveState(dc);

        // Render log-luminance to mip 0 of luminance texture
        dc->OMSetRenderTargets(1, &gLumRTV, nullptr);
        dc->OMSetBlendState(gNoBlend, nullptr, 0xFFFFFFFF);
        dc->OMSetDepthStencilState(gNoDepth, 0);
        dc->RSSetState(gRasterState);

        D3D11_VIEWPORT vp = {};
        vp.Width = (float)ctx->width;
        vp.Height = (float)ctx->height;
        vp.MaxDepth = 1.0f;
        dc->RSSetViewports(1, &vp);

        dc->PSSetShaderResources(0, 1, &gHdrCopySRV);
        dc->PSSetSamplers(0, 1, &gPointSampler);
        host->DrawFullscreenTriangle(dc, gLumPS);

        // Unbind RTV before GenerateMips (it needs SRV access to all mips)
        ID3D11RenderTargetView* nullRTV = nullptr;
        dc->OMSetRenderTargets(1, &nullRTV, nullptr);

        // Average via mipmap generation (bilinear on log values = geometric mean)
        dc->GenerateMips(gLumSRV);

        // Copy 1x1 mip (lowest) to staging for CPU readback
        D3D11_BOX srcBox = { 0, 0, 0, 1, 1, 1 };
        dc->CopySubresourceRegion(gLumStagingTex, 0, 0, 0, 0,
                                   gLumTex, gLumMipCount - 1, &srcBox);

        // Read average log-luminance
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = dc->Map(gLumStagingTex, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            float avgLogLum = *(float*)mapped.pData;
            dc->Unmap(gLumStagingTex, 0);

            // Convert back from log space
            float avgLum = exp2f(avgLogLum);

            // Auto-exposure adaptation
            if (gConfig.autoExposure)
            {
                // Compute target exposure: how many EV stops to reach 18% gray
                float targetEV = log2f(0.18f / (avgLum + 0.001f)) + gConfig.exposure;

                // Clamp to user-defined range
                if (targetEV < gConfig.minAutoExposure) targetEV = gConfig.minAutoExposure;
                if (targetEV > gConfig.maxAutoExposure) targetEV = gConfig.maxAutoExposure;

                // Temporal smoothing
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                float dt = (float)(now.QuadPart - gLastFrameTime.QuadPart) / (float)gPerfFreq.QuadPart;
                gLastFrameTime = now;
                dt = dt < 0.001f ? 0.001f : (dt > 0.5f ? 0.5f : dt);

                float alpha = 1.0f - expf(-dt * gConfig.adaptationSpeed);
                gAdaptedExposure += (targetEV - gAdaptedExposure) * alpha;
            }
        }

        host->RestoreState(dc);
    }
}

// postExecute: fires AFTER the game's tonemap draw.
// Overwrite the LDR target with our HDR -> tonemap -> LUT -> dither pipeline.
static void LUTPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gConfig.enabled || !gHdrCopySRV || !gLutSRV)
    {
        gHdrCopySRV = nullptr;
        return;
    }

    ID3D11RenderTargetView* ldrRTV = host->GetRTV(DUST_RESOURCE_LDR_RT);
    if (!ldrRTV)
    {
        gHdrCopySRV = nullptr;
        return;
    }

    ID3D11DeviceContext* dc = ctx->context;

    host->SaveState(dc);

    float finalExposure = gConfig.autoExposure ? gAdaptedExposure : gConfig.exposure;
    LUTParams params = { gConfig.intensity, (float)LUT_SIZE, finalExposure, 0.0f };
    host->UpdateConstantBuffer(dc, gCB, &params, sizeof(params));

    dc->PSSetConstantBuffers(0, 1, &gCB);

    // t0 = HDR scene copy, t1 = float LUT
    ID3D11ShaderResourceView* srvs[] = { gHdrCopySRV, gLutSRV };
    dc->PSSetShaderResources(0, 2, srvs);

    ID3D11SamplerState* samplers[] = { gPointSampler, gLinearSampler };
    dc->PSSetSamplers(0, 2, samplers);

    dc->OMSetRenderTargets(1, &ldrRTV, nullptr);
    dc->OMSetBlendState(gNoBlend, nullptr, 0xFFFFFFFF);
    dc->OMSetDepthStencilState(gNoDepth, 0);
    dc->RSSetState(gRasterState);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)ctx->width;
    vp.Height = (float)ctx->height;
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    host->DrawFullscreenTriangle(dc, gPS);

    host->RestoreState(dc);
    gHdrCopySRV = nullptr;
}

static int LUTIsEnabled()
{
    // Always return 1 so the framework always calls postExecute.
    // When disabled, postExecute returns early.
    return 1;
}

// ==================== GUI Settings ====================

static DustSettingDesc gLUTSettingsArray[] = {
    { "Enabled",          DUST_SETTING_BOOL,  &gConfig.enabled,     0.0f,  1.0f, "Enabled" },
    { "Exposure",         DUST_SETTING_FLOAT, &gConfig.exposure,   -3.0f,  3.0f, "Exposure" },
    { "Auto Exposure",    DUST_SETTING_BOOL,  &gConfig.autoExposure, 0.0f, 1.0f, "AutoExposure" },
    { "Adaptation Speed", DUST_SETTING_FLOAT, &gConfig.adaptationSpeed, 0.1f, 10.0f, "AdaptationSpeed" },
    { "Min Auto EV",      DUST_SETTING_FLOAT, &gConfig.minAutoExposure, -5.0f, 0.0f, "MinAutoExposure" },
    { "Max Auto EV",      DUST_SETTING_FLOAT, &gConfig.maxAutoExposure,  0.0f, 5.0f, "MaxAutoExposure" },
    { "Intensity",        DUST_SETTING_FLOAT, &gConfig.intensity,   0.0f,  1.0f, "Intensity" },
    { "Lift (Shadows)",   DUST_SETTING_FLOAT, &gConfig.lift,       -0.1f,  0.1f, "Lift" },
    { "Gamma (Midtones)", DUST_SETTING_FLOAT, &gConfig.gamma,       0.8f,  1.2f, "Gamma" },
    { "Gain (Highlights)",DUST_SETTING_FLOAT, &gConfig.gain,        0.8f,  1.2f, "Gain" },
    { "Contrast",         DUST_SETTING_FLOAT, &gConfig.contrast,    0.5f,  1.5f, "Contrast" },
    { "Saturation",       DUST_SETTING_FLOAT, &gConfig.saturation,  0.0f,  2.0f, "Saturation" },
    { "Temperature",      DUST_SETTING_FLOAT, &gConfig.temperature,-1.0f,  1.0f, "Temperature" },
    { "Tint",             DUST_SETTING_FLOAT, &gConfig.tint,       -1.0f,  1.0f, "Tint" },
    { "Shadow Red",       DUST_SETTING_FLOAT, &gConfig.shadowR,    -0.1f,  0.1f, "ShadowR" },
    { "Shadow Green",     DUST_SETTING_FLOAT, &gConfig.shadowG,    -0.1f,  0.1f, "ShadowG" },
    { "Shadow Blue",      DUST_SETTING_FLOAT, &gConfig.shadowB,    -0.1f,  0.1f, "ShadowB" },
    { "Highlight Red",    DUST_SETTING_FLOAT, &gConfig.highlightR, -0.1f,  0.1f, "HighlightR" },
    { "Highlight Green",  DUST_SETTING_FLOAT, &gConfig.highlightG, -0.1f,  0.1f, "HighlightG" },
    { "Highlight Blue",   DUST_SETTING_FLOAT, &gConfig.highlightB, -0.1f,  0.1f, "HighlightB" },
};

static void LUTOnSettingChanged()
{
    // Regenerate LUT from the updated parameters
    if (gDevice)
        GenerateLUT(gDevice);
}

// ==================== Plugin entry ====================

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "LUT";
    desc->injectionPoint    = DUST_INJECT_POST_TONEMAP;
    desc->Init              = LUTInit;
    desc->Shutdown          = LUTShutdown;
    desc->OnResolutionChanged = LUTOnResolutionChanged;
    desc->preExecute        = LUTPreExecute;
    desc->postExecute       = LUTPostExecute;
    desc->IsEnabled         = LUTIsEnabled;
    desc->settings          = gLUTSettingsArray;
    desc->settingCount      = sizeof(gLUTSettingsArray) / sizeof(gLUTSettingsArray[0]);
    desc->OnSettingChanged  = LUTOnSettingChanged;

    // v3: Framework handles config I/O and GPU timing
    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "LUT";

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
