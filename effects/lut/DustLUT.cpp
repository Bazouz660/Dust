// DustLUT.cpp - Color grading LUT effect plugin for Dust
// Generates a 32^3 LUT from parametric color grading settings,
// then applies it as a post-process on the HDR target.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "LUTShaders.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>

DustLogFn gLogFn = nullptr;
static const DustHostAPI* gHost = nullptr;
static HMODULE gPluginModule = nullptr;
static ID3D11Device* gDevice = nullptr;
static float gGpuTimeMs = 0.0f;

// GPU timing queries
static ID3D11Query* gTimestampDisjoint = nullptr;
static ID3D11Query* gTimestampBegin = nullptr;
static ID3D11Query* gTimestampEnd = nullptr;
static bool gTimingActive = false;

// ==================== Config ====================

struct LUTConfig {
    bool enabled       = true;
    float intensity    = 1.0f;

    // Lift / Gamma / Gain (applied in log space)
    float lift         = 0.0f;    // Shadows offset (-0.1 to 0.1)
    float gamma        = 1.0f;    // Midtone power (0.8 to 1.2)
    float gain         = 1.0f;    // Highlight multiplier (0.8 to 1.2)

    // Color balance
    float contrast     = 1.0f;    // 0.5 to 1.5
    float saturation   = 1.0f;    // 0.0 to 2.0
    float temperature  = 0.0f;    // -1.0 (cool) to 1.0 (warm)
    float tint         = 0.0f;    // -1.0 (green) to 1.0 (magenta)

    // Split toning
    float shadowR      = 0.0f;    // Shadow color offset (-0.1 to 0.1)
    float shadowG      = 0.0f;
    float shadowB      = 0.0f;
    float highlightR   = 0.0f;    // Highlight color offset
    float highlightG   = 0.0f;
    float highlightB   = 0.0f;
};

static LUTConfig gConfig;
static std::string gConfigPath;
static FILETIME gLastWriteTime = {};

static float ReadFloat(const char* key, float def)
{
    char buf[64], defStr[64];
    snprintf(defStr, sizeof(defStr), "%g", def);
    GetPrivateProfileStringA("LUT", key, defStr, buf, sizeof(buf), gConfigPath.c_str());
    return (float)atof(buf);
}

static void LoadConfig()
{
    gConfig.enabled     = GetPrivateProfileIntA("LUT", "Enabled", 1, gConfigPath.c_str()) != 0;
    gConfig.intensity   = ReadFloat("Intensity", 1.0f);
    gConfig.lift        = ReadFloat("Lift", 0.0f);
    gConfig.gamma       = ReadFloat("Gamma", 1.0f);
    gConfig.gain        = ReadFloat("Gain", 1.0f);
    gConfig.contrast    = ReadFloat("Contrast", 1.0f);
    gConfig.saturation  = ReadFloat("Saturation", 1.0f);
    gConfig.temperature = ReadFloat("Temperature", 0.0f);
    gConfig.tint        = ReadFloat("Tint", 0.0f);
    gConfig.shadowR     = ReadFloat("ShadowR", 0.0f);
    gConfig.shadowG     = ReadFloat("ShadowG", 0.0f);
    gConfig.shadowB     = ReadFloat("ShadowB", 0.0f);
    gConfig.highlightR  = ReadFloat("HighlightR", 0.0f);
    gConfig.highlightG  = ReadFloat("HighlightG", 0.0f);
    gConfig.highlightB  = ReadFloat("HighlightB", 0.0f);

    Log("LUT: Config loaded: intensity=%.2f contrast=%.2f sat=%.2f temp=%.2f",
        gConfig.intensity, gConfig.contrast, gConfig.saturation, gConfig.temperature);
}

static void WriteDefaultConfig()
{
    // Cinematic desert preset — warm, slightly desaturated, teal shadows
    WritePrivateProfileStringA("LUT", "Enabled", "1", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "Intensity", "0.7", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "; Lift/Gamma/Gain", nullptr, gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "Lift", "0.02", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "Gamma", "0.97", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "Gain", "1.05", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "; Color balance", nullptr, gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "Contrast", "1.08", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "Saturation", "0.85", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "Temperature", "0.08", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "Tint", "0", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "; Split toning (shadow/highlight color offsets)", nullptr, gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "ShadowR", "-0.02", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "ShadowG", "0.01", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "ShadowB", "0.04", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "HighlightR", "0.03", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "HighlightG", "0.01", gConfigPath.c_str());
    WritePrivateProfileStringA("LUT", "HighlightB", "-0.02", gConfigPath.c_str());
}

static void SaveConfig()
{
    char buf[64];
    WritePrivateProfileStringA("LUT", "Enabled", gConfig.enabled ? "1" : "0", gConfigPath.c_str());

    snprintf(buf, sizeof(buf), "%g", gConfig.intensity);
    WritePrivateProfileStringA("LUT", "Intensity", buf, gConfigPath.c_str());
    snprintf(buf, sizeof(buf), "%g", gConfig.lift);
    WritePrivateProfileStringA("LUT", "Lift", buf, gConfigPath.c_str());
    snprintf(buf, sizeof(buf), "%g", gConfig.gamma);
    WritePrivateProfileStringA("LUT", "Gamma", buf, gConfigPath.c_str());
    snprintf(buf, sizeof(buf), "%g", gConfig.gain);
    WritePrivateProfileStringA("LUT", "Gain", buf, gConfigPath.c_str());
    snprintf(buf, sizeof(buf), "%g", gConfig.contrast);
    WritePrivateProfileStringA("LUT", "Contrast", buf, gConfigPath.c_str());
    snprintf(buf, sizeof(buf), "%g", gConfig.saturation);
    WritePrivateProfileStringA("LUT", "Saturation", buf, gConfigPath.c_str());
    snprintf(buf, sizeof(buf), "%g", gConfig.temperature);
    WritePrivateProfileStringA("LUT", "Temperature", buf, gConfigPath.c_str());
    snprintf(buf, sizeof(buf), "%g", gConfig.tint);
    WritePrivateProfileStringA("LUT", "Tint", buf, gConfigPath.c_str());
    snprintf(buf, sizeof(buf), "%g", gConfig.shadowR);
    WritePrivateProfileStringA("LUT", "ShadowR", buf, gConfigPath.c_str());
    snprintf(buf, sizeof(buf), "%g", gConfig.shadowG);
    WritePrivateProfileStringA("LUT", "ShadowG", buf, gConfigPath.c_str());
    snprintf(buf, sizeof(buf), "%g", gConfig.shadowB);
    WritePrivateProfileStringA("LUT", "ShadowB", buf, gConfigPath.c_str());
    snprintf(buf, sizeof(buf), "%g", gConfig.highlightR);
    WritePrivateProfileStringA("LUT", "HighlightR", buf, gConfigPath.c_str());
    snprintf(buf, sizeof(buf), "%g", gConfig.highlightG);
    WritePrivateProfileStringA("LUT", "HighlightG", buf, gConfigPath.c_str());
    snprintf(buf, sizeof(buf), "%g", gConfig.highlightB);
    WritePrivateProfileStringA("LUT", "HighlightB", buf, gConfigPath.c_str());
}

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

    uint32_t* pixels = new uint32_t[stripWidth * stripHeight];

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
                uint8_t rr = (uint8_t)(outR * 255.0f + 0.5f);
                uint8_t gg = (uint8_t)(outG * 255.0f + 0.5f);
                uint8_t bb = (uint8_t)(outB * 255.0f + 0.5f);
                pixels[y * stripWidth + x] = (255u << 24) | (bb << 16) | (gg << 8) | rr;
            }
        }
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = stripWidth;
    desc.Height = stripHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = pixels;
    init.SysMemPitch = stripWidth * 4;

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

// ==================== Hot Reload ====================

static void CheckHotReload()
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(gConfigPath.c_str(), GetFileExInfoStandard, &fad))
        return;

    if (CompareFileTime(&fad.ftLastWriteTime, &gLastWriteTime) != 0)
    {
        gLastWriteTime = fad.ftLastWriteTime;
        Log("LUT: Config changed, reloading...");
        LoadConfig();
        if (gDevice)
            GenerateLUT(gDevice);
    }
}

// ==================== Scene Copy ====================

static ID3D11Texture2D* gSceneCopy = nullptr;
static ID3D11ShaderResourceView* gSceneCopySRV = nullptr;
static UINT gWidth = 0, gHeight = 0;

static bool CreateSceneCopy(ID3D11Device* device, UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &gSceneCopy);
    if (FAILED(hr)) return false;

    hr = device->CreateShaderResourceView(gSceneCopy, nullptr, &gSceneCopySRV);
    if (FAILED(hr))
    {
        gSceneCopy->Release(); gSceneCopy = nullptr;
        return false;
    }

    gWidth = w;
    gHeight = h;
    return true;
}

static void ReleaseSceneCopy()
{
    if (gSceneCopySRV) { gSceneCopySRV->Release(); gSceneCopySRV = nullptr; }
    if (gSceneCopy)    { gSceneCopy->Release();    gSceneCopy = nullptr; }
}

// ==================== Shader Compilation ====================

static ID3D11VertexShader* gVS = nullptr;
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
    float pad[2];
};

static ID3DBlob* CompileShader(const char* src, const char* target)
{
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &blob, &errors);
    if (FAILED(hr))
    {
        if (errors)
        {
            Log("LUT: Shader compile error (%s): %s", target,
                (const char*)errors->GetBufferPointer());
            errors->Release();
        }
        return nullptr;
    }
    if (errors) errors->Release();
    return blob;
}

// ==================== Init / Shutdown ====================

static int LUTInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
    gHost = host;
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gDevice = device;

    // Find config path next to DLL
    {
        char path[MAX_PATH];
        GetModuleFileNameA(gPluginModule, path, MAX_PATH);
        std::string s(path);
        auto pos = s.find_last_of("\\/");
        if (pos != std::string::npos)
            s = s.substr(0, pos + 1);
        gConfigPath = s + "LUT.ini";

        DWORD attr = GetFileAttributesA(gConfigPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES)
        {
            Log("LUT: Creating default config: %s", gConfigPath.c_str());
            WriteDefaultConfig();
        }
        LoadConfig();

        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(gConfigPath.c_str(), GetFileExInfoStandard, &fad))
            gLastWriteTime = fad.ftLastWriteTime;
    }

    // Compile shaders
    ID3DBlob* vsBlob = CompileShader(LUT_VS, "vs_5_0");
    if (!vsBlob) return -1;

    ID3DBlob* psBlob = CompileShader(LUT_PS, "ps_5_0");
    if (!psBlob) { vsBlob->Release(); return -2; }

    HRESULT hr = device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                             vsBlob->GetBufferSize(), nullptr, &gVS);
    vsBlob->Release();
    if (FAILED(hr)) { psBlob->Release(); return -3; }

    hr = device->CreatePixelShader(psBlob->GetBufferPointer(),
                                    psBlob->GetBufferSize(), nullptr, &gPS);
    psBlob->Release();
    if (FAILED(hr)) return -4;

    // Constant buffer
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(LUTParams);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbd, nullptr, &gCB);
    if (FAILED(hr)) return -5;

    // Samplers
    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = device->CreateSamplerState(&sd, &gPointSampler);
    if (FAILED(hr)) return -6;

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    hr = device->CreateSamplerState(&sd, &gLinearSampler);
    if (FAILED(hr)) return -7;

    // Blend state (no blending)
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&bd, &gNoBlend);
    if (FAILED(hr)) return -8;

    // Depth stencil (disabled)
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    hr = device->CreateDepthStencilState(&dsd, &gNoDepth);
    if (FAILED(hr)) return -9;

    // Rasterizer
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    hr = device->CreateRasterizerState(&rd, &gRasterState);
    if (FAILED(hr)) return -10;

    // Scene copy texture
    if (!CreateSceneCopy(device, width, height))
        return -11;

    // Generate LUT from config settings
    if (!GenerateLUT(device))
        return -12;

    // GPU timing queries
    {
        D3D11_QUERY_DESC qd = {};
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        device->CreateQuery(&qd, &gTimestampDisjoint);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        device->CreateQuery(&qd, &gTimestampBegin);
        device->CreateQuery(&qd, &gTimestampEnd);
    }

    Log("LUT: Initialized (%ux%u)", width, height);
    return 0;
}

static void LUTShutdown()
{
    ReleaseLUT();
    ReleaseSceneCopy();
    if (gRasterState)  { gRasterState->Release();   gRasterState = nullptr; }
    if (gNoDepth)      { gNoDepth->Release();       gNoDepth = nullptr; }
    if (gNoBlend)      { gNoBlend->Release();       gNoBlend = nullptr; }
    if (gLinearSampler){ gLinearSampler->Release();  gLinearSampler = nullptr; }
    if (gPointSampler) { gPointSampler->Release();   gPointSampler = nullptr; }
    if (gCB)           { gCB->Release();             gCB = nullptr; }
    if (gPS)           { gPS->Release();             gPS = nullptr; }
    if (gVS)           { gVS->Release();             gVS = nullptr; }
    if (gTimestampDisjoint) { gTimestampDisjoint->Release(); gTimestampDisjoint = nullptr; }
    if (gTimestampBegin)   { gTimestampBegin->Release();   gTimestampBegin = nullptr; }
    if (gTimestampEnd)     { gTimestampEnd->Release();     gTimestampEnd = nullptr; }
    gDevice = nullptr;
    Log("LUT: Shut down");
}

static void LUTOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    ReleaseSceneCopy();
    CreateSceneCopy(device, w, h);
    Log("LUT: Resolution changed to %ux%u", w, h);
}

// ==================== Per-frame ====================

static void LUTPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    CheckHotReload();

    if (!gConfig.enabled || gConfig.intensity <= 0.0f)
    {
        gGpuTimeMs = 0.0f;
        return;
    }

    ID3D11RenderTargetView* ldrRTV = host->GetRTV("ldr_rt");
    if (!ldrRTV || !gSceneCopy || !gSceneCopySRV || !gLutSRV)
        return;

    ID3D11DeviceContext* dc = ctx->context;

    // Collect previous frame's GPU timing
    if (gTimingActive && gTimestampDisjoint)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
        UINT64 tsBegin, tsEnd;
        if (dc->GetData(gTimestampDisjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && !disjoint.Disjoint
            && dc->GetData(gTimestampBegin, &tsBegin, sizeof(tsBegin), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && dc->GetData(gTimestampEnd, &tsEnd, sizeof(tsEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
        {
            gGpuTimeMs = (float)((double)(tsEnd - tsBegin) / (double)disjoint.Frequency * 1000.0);
        }
        gTimingActive = false;
    }

    // Get the LDR texture from the RTV
    ID3D11Resource* ldrResource = nullptr;
    ldrRTV->GetResource(&ldrResource);
    if (!ldrResource) return;

    // Copy scene to staging texture
    dc->CopyResource(gSceneCopy, ldrResource);
    ldrResource->Release();

    // Save GPU state
    host->SaveState(dc);

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(dc->Map(gCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        LUTParams* p = (LUTParams*)mapped.pData;
        p->intensity = gConfig.intensity;
        p->lutSize = (float)LUT_SIZE;
        dc->Unmap(gCB, 0);
    }

    // Set pipeline state
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->IASetInputLayout(nullptr);

    dc->VSSetShader(gVS, nullptr, 0);
    dc->PSSetShader(gPS, nullptr, 0);
    dc->PSSetConstantBuffers(0, 1, &gCB);

    ID3D11ShaderResourceView* srvs[] = { gSceneCopySRV, gLutSRV };
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

    // Draw fullscreen triangle with GPU timing
    if (gTimestampDisjoint && gTimestampBegin && gTimestampEnd)
    {
        dc->Begin(gTimestampDisjoint);
        dc->End(gTimestampBegin);
    }

    dc->Draw(3, 0);

    if (gTimestampDisjoint && gTimestampBegin && gTimestampEnd)
    {
        dc->End(gTimestampEnd);
        dc->End(gTimestampDisjoint);
        gTimingActive = true;
    }

    // Restore GPU state
    host->RestoreState(dc);
}

static int LUTIsEnabled()
{
    // Always return 1 so the framework always calls postExecute.
    // When disabled, postExecute returns early (and resets GPU timing to 0).
    return 1;
}

// ==================== Plugin entry ====================

// ==================== GUI Settings ====================

static DustSettingDesc gLUTSettingsArray[] = {
    { "Enabled",          DUST_SETTING_BOOL,  &gConfig.enabled,     0.0f,  1.0f },
    { "Intensity",        DUST_SETTING_FLOAT, &gConfig.intensity,   0.0f,  1.0f },
    { "Lift (Shadows)",   DUST_SETTING_FLOAT, &gConfig.lift,       -0.1f,  0.1f },
    { "Gamma (Midtones)", DUST_SETTING_FLOAT, &gConfig.gamma,       0.8f,  1.2f },
    { "Gain (Highlights)",DUST_SETTING_FLOAT, &gConfig.gain,        0.8f,  1.2f },
    { "Contrast",         DUST_SETTING_FLOAT, &gConfig.contrast,    0.5f,  1.5f },
    { "Saturation",       DUST_SETTING_FLOAT, &gConfig.saturation,  0.0f,  2.0f },
    { "Temperature",      DUST_SETTING_FLOAT, &gConfig.temperature,-1.0f,  1.0f },
    { "Tint",             DUST_SETTING_FLOAT, &gConfig.tint,       -1.0f,  1.0f },
    { "Shadow Red",       DUST_SETTING_FLOAT, &gConfig.shadowR,    -0.1f,  0.1f },
    { "Shadow Green",     DUST_SETTING_FLOAT, &gConfig.shadowG,    -0.1f,  0.1f },
    { "Shadow Blue",      DUST_SETTING_FLOAT, &gConfig.shadowB,    -0.1f,  0.1f },
    { "Highlight Red",    DUST_SETTING_FLOAT, &gConfig.highlightR, -0.1f,  0.1f },
    { "Highlight Green",  DUST_SETTING_FLOAT, &gConfig.highlightG, -0.1f,  0.1f },
    { "Highlight Blue",   DUST_SETTING_FLOAT, &gConfig.highlightB, -0.1f,  0.1f },
};

static void LUTOnSettingChanged()
{
    // Regenerate LUT from the updated parameters (no disk save)
    if (gDevice)
        GenerateLUT(gDevice);
}

static void LUTSaveSettings()
{
    SaveConfig();
    Log("LUT: Settings saved to disk");
}

static void LUTLoadSettings()
{
    LoadConfig();
    if (gDevice)
        GenerateLUT(gDevice);
    Log("LUT: Settings reloaded from disk");
}

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
    desc->preExecute        = nullptr;
    desc->postExecute       = LUTPostExecute;
    desc->IsEnabled         = LUTIsEnabled;
    desc->settings          = gLUTSettingsArray;
    desc->settingCount      = sizeof(gLUTSettingsArray) / sizeof(gLUTSettingsArray[0]);
    desc->OnSettingChanged  = LUTOnSettingChanged;
    desc->SaveSettings      = LUTSaveSettings;
    desc->LoadSettings      = LUTLoadSettings;
    desc->gpuTimeMsPtr      = &gGpuTimeMs;

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
