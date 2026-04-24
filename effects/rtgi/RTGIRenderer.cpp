#include "RTGIRenderer.h"
#include "RTGIConfig.h"
#include "DustLog.h"
#include <cstring>
#include <cmath>
#include <string>

namespace RTGIRenderer
{

static bool gInitialized = false;
static UINT gWidth = 0;
static UINT gHeight = 0;
static const DustHostAPI* gHost = nullptr;
static std::string gShaderDir;

// ==================== Camera Data ====================

static bool    gHasValidCameraData = false;
static float   gInverseView[16] = {};
static float   gPrevInverseView[16] = {};
static bool    gHasPrevFrame = false;
static uint64_t gFrameIndex = 0;
static float   gSmoothedMotion = 0.0f; // EMA of motion magnitude, decays over ~30 frames

// ==================== Textures ====================

// Internal render resolution (supports half-res)
static UINT gRenderWidth = 0;
static UINT gRenderHeight = 0;
static int gLastResScale = 50; // Track resolution scale (25-100%) for runtime texture recreation

// Raw ray trace output (RGBA16F) — UAV for compute shader write
static ID3D11Texture2D*           gRawTex = nullptr;
static ID3D11RenderTargetView*    gRawRTV = nullptr;
static ID3D11ShaderResourceView*  gRawSRV = nullptr;
static ID3D11UnorderedAccessView* gRawUAV = nullptr;

// Temporal accumulation ping-pong (RGBA16F)
static ID3D11Texture2D*          gAccumTexA = nullptr;
static ID3D11RenderTargetView*   gAccumRTVA = nullptr;
static ID3D11ShaderResourceView* gAccumSRVA = nullptr;

static ID3D11Texture2D*          gAccumTexB = nullptr;
static ID3D11RenderTargetView*   gAccumRTVB = nullptr;
static ID3D11ShaderResourceView* gAccumSRVB = nullptr;

static int gAccumWriteIndex = 0; // 0 = write to A (read B as history), 1 = write to B (read A)

// Moments ping-pong for SVGF (RGBA16F: R=m1, G=m2, B=historyLength, A=variance)
static ID3D11Texture2D*          gMomentsTexA = nullptr;
static ID3D11RenderTargetView*   gMomentsRTVA = nullptr;
static ID3D11ShaderResourceView* gMomentsSRVA = nullptr;

static ID3D11Texture2D*          gMomentsTexB = nullptr;
static ID3D11RenderTargetView*   gMomentsRTVB = nullptr;
static ID3D11ShaderResourceView* gMomentsSRVB = nullptr;

// Denoise ping-pong (RGBA16F) — UAV for compute-shader atrous
static ID3D11Texture2D*           gDenoiseTexA = nullptr;
static ID3D11RenderTargetView*    gDenoiseRTVA = nullptr;
static ID3D11ShaderResourceView*  gDenoiseSRVA = nullptr;
static ID3D11UnorderedAccessView* gDenoiseUAVA = nullptr;

static ID3D11Texture2D*           gDenoiseTexB = nullptr;
static ID3D11RenderTargetView*    gDenoiseRTVB = nullptr;
static ID3D11ShaderResourceView*  gDenoiseSRVB = nullptr;
static ID3D11UnorderedAccessView* gDenoiseUAVB = nullptr;

// Previous frame depth — populated at end of each frame via CopyResource
static ID3D11Texture2D*          gPrevDepthTex = nullptr;
static ID3D11ShaderResourceView* gPrevDepthSRV = nullptr;

// Tracks the last final (denoised) GI output for debug overlay
static ID3D11ShaderResourceView* gFinalGISRV = nullptr;

// ==================== Shaders ====================

static ID3D11VertexShader*  gFullscreenVS = nullptr;
static ID3D11ComputeShader* gRayTraceCS = nullptr;
static ID3D11PixelShader*   gTemporalPS = nullptr;
static ID3D11PixelShader*   gVariancePS = nullptr;
static ID3D11ComputeShader* gAtrousCS = nullptr;
static ID3D11PixelShader*   gCompositePS = nullptr;
static ID3D11PixelShader*   gAOCompositePS = nullptr;
static ID3D11PixelShader*   gDebugPS = nullptr;

// ==================== Pipeline States ====================

static ID3D11BlendState*        gNoBlend = nullptr;
static ID3D11BlendState*        gAdditiveBlend = nullptr;
static ID3D11BlendState*        gMultiplyBlend = nullptr;
static ID3D11DepthStencilState* gNoDepthDSS = nullptr;
static ID3D11RasterizerState*   gNoCullRS = nullptr;
static ID3D11SamplerState*      gPointClampSampler = nullptr;
static ID3D11SamplerState*      gLinearClampSampler = nullptr;

// ==================== Constant Buffers ====================

static ID3D11Buffer* gRayTraceCB = nullptr;
static ID3D11Buffer* gTemporalCB = nullptr;
static ID3D11Buffer* gVarianceCB = nullptr;
static ID3D11Buffer* gDenoiseCB = nullptr;
static ID3D11Buffer* gCompositeCB = nullptr;
static ID3D11Buffer* gDebugCB = nullptr;

// ==================== CB Structs ====================

struct RayTraceCBData
{
    float viewportSize[2];
    float invViewportSize[2];
    float tanHalfFov;
    float aspectRatio;
    float rayLength;
    float raySteps;
    float thickness;
    float fadeDistance;
    float bounceIntensity;
    float aoIntensity;
    float frameIndex;
    float raysPerPixel;
    float thicknessCurve;
    float normalDetail;
    float sampleJitter[2];
    float _pad1;
    float _pad2;
    float camRight[4];   // column 0 of inverseView: camera right in world space
    float camUp[4];      // column 1 of inverseView: camera up in world space
    float camForward[4]; // column 2 of inverseView: camera forward in world space
};

struct TemporalCBData
{
    float viewportSize[2];
    float invViewportSize[2];
    float tanHalfFov;
    float aspectRatio;
    float temporalBlend;
    float frameIndex;
    float reprojMatrix[16]; // currentInvView * prevView — direct view-to-prevView transform
    float motionMagnitude;  // length of translation in reprojection matrix (pixels approx)
    float _pad0;
    float _pad1;
    float _pad2;
};

struct DenoiseCBData
{
    float viewportSize[2];
    float invViewportSize[2];
    float tanHalfFov;
    float aspectRatio;
    float stepSize;
    float depthSigma;
    float phiColor;
    float fadeDistance;
    float _pad0;
    float _pad1;
};

struct VarianceCBData
{
    float viewportSize[2];
    float invViewportSize[2];
};

struct CompositeCBData
{
    float viewportSize[2];
    float invViewportSize[2];
    float giIntensity;
    float saturation;
    float giTexSize[2];     // Half-res or full-res GI texture dimensions
};

struct DebugCBData
{
    float debugMode;
    float tanHalfFov;
    float aspectRatio;
    float _pad0;
    float camRight[4];
    float camUp[4];
    float camForward[4];
};


// ==================== Helpers ====================

// Radical-inverse Halton sequence — quasi-random low-discrepancy sampler.
// With b=2 and b=3 on orthogonal axes, gives an evenly-distributed 2D jitter
// grid that doesn't clump. Cycling a 16-sample window is enough to cover the
// 4×4 full-res footprint of a quarter-res pixel.
static float Halton(uint32_t i, uint32_t b)
{
    float f = 1.0f;
    float r = 0.0f;
    while (i > 0)
    {
        f /= (float)b;
        r += f * (float)(i % b);
        i /= b;
    }
    return r;
}

// Compute reprojection matrix: currentInvView * prevView
// Transforms current view-space positions directly to previous view-space,
// avoiding world-space intermediate (which causes catastrophic cancellation
// when camera position has large absolute coordinates).
static void ComputeReprojectionMatrix(const float* curInv, const float* prevInv, float* out)
{
    // Compute prevView = inverse(prevInvView)
    // For row-major layout with mul(v, M):
    //   invView = [R | 0; T | 1] where R is 3x3 rotation, T is translation
    //   view = [R^T | 0; -T*R^T | 1]
    float pv[16];
    // Transpose 3x3 rotation
    pv[0]  = prevInv[0]; pv[1]  = prevInv[4]; pv[2]  = prevInv[8];  pv[3]  = 0;
    pv[4]  = prevInv[1]; pv[5]  = prevInv[5]; pv[6]  = prevInv[9];  pv[7]  = 0;
    pv[8]  = prevInv[2]; pv[9]  = prevInv[6]; pv[10] = prevInv[10]; pv[11] = 0;
    // Translation: -(camPos * R^T)
    float px = prevInv[12], py = prevInv[13], pz = prevInv[14];
    pv[12] = -(px * pv[0] + py * pv[4] + pz * pv[8]);
    pv[13] = -(px * pv[1] + py * pv[5] + pz * pv[9]);
    pv[14] = -(px * pv[2] + py * pv[6] + pz * pv[10]);
    pv[15] = 1;

    // Multiply: out = curInv * pv (row-major 4x4)
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
        {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += curInv[i * 4 + k] * pv[k * 4 + j];
            out[i * 4 + j] = s;
        }
}

static bool CreateRGBA16FTexture(ID3D11Device* device, UINT width, UINT height,
                                  ID3D11Texture2D** outTex,
                                  ID3D11RenderTargetView** outRTV,
                                  ID3D11ShaderResourceView** outSRV,
                                  ID3D11UnorderedAccessView** outUAV = nullptr)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (outUAV) desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, outTex);
    if (FAILED(hr)) { Log("RTGI: Failed to create RGBA16F texture (%ux%u): 0x%08X", width, height, hr); return false; }

    hr = device->CreateRenderTargetView(*outTex, nullptr, outRTV);
    if (FAILED(hr)) { Log("RTGI: Failed to create RGBA16F RTV: 0x%08X", hr); return false; }

    hr = device->CreateShaderResourceView(*outTex, nullptr, outSRV);
    if (FAILED(hr)) { Log("RTGI: Failed to create RGBA16F SRV: 0x%08X", hr); return false; }

    if (outUAV)
    {
        hr = device->CreateUnorderedAccessView(*outTex, nullptr, outUAV);
        if (FAILED(hr)) { Log("RTGI: Failed to create RGBA16F UAV: 0x%08X", hr); return false; }
    }

    return true;
}

static bool CreateR32FTexture(ID3D11Device* device, UINT width, UINT height,
                               ID3D11Texture2D** outTex,
                               ID3D11RenderTargetView** outRTV,
                               ID3D11ShaderResourceView** outSRV)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, outTex);
    if (FAILED(hr)) { Log("RTGI: Failed to create R32F texture (%ux%u): 0x%08X", width, height, hr); return false; }

    hr = device->CreateRenderTargetView(*outTex, nullptr, outRTV);
    if (FAILED(hr)) { Log("RTGI: Failed to create R32F RTV: 0x%08X", hr); return false; }

    hr = device->CreateShaderResourceView(*outTex, nullptr, outSRV);
    if (FAILED(hr)) { Log("RTGI: Failed to create R32F SRV: 0x%08X", hr); return false; }

    return true;
}

#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }

static void ReleaseTextures()
{
    SAFE_RELEASE(gRawTex);     SAFE_RELEASE(gRawRTV);     SAFE_RELEASE(gRawSRV);     SAFE_RELEASE(gRawUAV);
    SAFE_RELEASE(gAccumTexA);  SAFE_RELEASE(gAccumRTVA);  SAFE_RELEASE(gAccumSRVA);
    SAFE_RELEASE(gAccumTexB);  SAFE_RELEASE(gAccumRTVB);  SAFE_RELEASE(gAccumSRVB);
    SAFE_RELEASE(gMomentsTexA);SAFE_RELEASE(gMomentsRTVA);SAFE_RELEASE(gMomentsSRVA);
    SAFE_RELEASE(gMomentsTexB);SAFE_RELEASE(gMomentsRTVB);SAFE_RELEASE(gMomentsSRVB);
    SAFE_RELEASE(gDenoiseTexA);SAFE_RELEASE(gDenoiseRTVA);SAFE_RELEASE(gDenoiseSRVA);SAFE_RELEASE(gDenoiseUAVA);
    SAFE_RELEASE(gDenoiseTexB);SAFE_RELEASE(gDenoiseRTVB);SAFE_RELEASE(gDenoiseSRVB);SAFE_RELEASE(gDenoiseUAVB);
    SAFE_RELEASE(gPrevDepthTex);SAFE_RELEASE(gPrevDepthSRV);
}

static bool CreateTextures(ID3D11Device* device, UINT width, UINT height)
{
    int scale = gRTGIConfig.resolutionScale;
    if (scale < 25) scale = 25;
    if (scale > 100) scale = 100;
    UINT rw = (width * scale + 50) / 100;   // round to nearest
    UINT rh = (height * scale + 50) / 100;
    if (rw == 0) rw = 1;
    if (rh == 0) rh = 1;
    gRenderWidth = rw;
    gRenderHeight = rh;

    if (!CreateRGBA16FTexture(device, rw, rh, &gRawTex, &gRawRTV, &gRawSRV, &gRawUAV)) return false;
    if (!CreateRGBA16FTexture(device, rw, rh, &gAccumTexA, &gAccumRTVA, &gAccumSRVA)) return false;
    if (!CreateRGBA16FTexture(device, rw, rh, &gAccumTexB, &gAccumRTVB, &gAccumSRVB)) return false;
    if (!CreateRGBA16FTexture(device, rw, rh, &gMomentsTexA, &gMomentsRTVA, &gMomentsSRVA)) return false;
    if (!CreateRGBA16FTexture(device, rw, rh, &gMomentsTexB, &gMomentsRTVB, &gMomentsSRVB)) return false;
    if (!CreateRGBA16FTexture(device, rw, rh, &gDenoiseTexA, &gDenoiseRTVA, &gDenoiseSRVA, &gDenoiseUAVA)) return false;
    if (!CreateRGBA16FTexture(device, rw, rh, &gDenoiseTexB, &gDenoiseRTVB, &gDenoiseSRVB, &gDenoiseUAVB)) return false;
    // Previous frame depth — full res, CopyResource target + SRV for temporal pass
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &gPrevDepthTex);
        if (FAILED(hr)) { Log("RTGI: Failed to create prev depth texture: 0x%08X", hr); return false; }

        hr = device->CreateShaderResourceView(gPrevDepthTex, nullptr, &gPrevDepthSRV);
        if (FAILED(hr)) { Log("RTGI: Failed to create prev depth SRV: 0x%08X", hr); return false; }
    }

    return true;
}

// ==================== Public API ====================

bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir)
{
    if (gInitialized) return true;

    gHost = host;
    gShaderDir = std::string(effectDir) + "\\shaders\\";
    Log("RTGI: Initializing RTGIRenderer (%ux%u), shaders: %s", width, height, gShaderDir.c_str());
    gWidth = width;
    gHeight = height;

    // Compile shaders
    ID3DBlob* vsBlob = host->CompileShaderFromFile((gShaderDir + "fullscreen_vs.hlsl").c_str(), "main", "vs_5_0");
    if (!vsBlob) { Log("RTGI: Failed to compile fullscreen VS"); return false; }

    ID3DBlob* rtBlob = host->CompileShaderFromFile((gShaderDir + "rtgi_raytrace_cs.hlsl").c_str(), "main", "cs_5_0");
    if (!rtBlob) { vsBlob->Release(); Log("RTGI: Failed to compile raytrace CS"); return false; }

    ID3DBlob* tempBlob = host->CompileShaderFromFile((gShaderDir + "rtgi_temporal_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!tempBlob) { vsBlob->Release(); rtBlob->Release(); Log("RTGI: Failed to compile temporal PS"); return false; }

    ID3DBlob* varBlob = host->CompileShaderFromFile((gShaderDir + "rtgi_variance_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!varBlob) { vsBlob->Release(); rtBlob->Release(); tempBlob->Release(); Log("RTGI: Failed to compile variance PS"); return false; }

    ID3DBlob* denoiseBlob = host->CompileShaderFromFile((gShaderDir + "rtgi_atrous_cs.hlsl").c_str(), "main", "cs_5_0");
    if (!denoiseBlob) { vsBlob->Release(); rtBlob->Release(); tempBlob->Release(); varBlob->Release(); Log("RTGI: Failed to compile atrous CS"); return false; }

    ID3DBlob* compBlob = host->CompileShaderFromFile((gShaderDir + "rtgi_composite_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!compBlob) { vsBlob->Release(); rtBlob->Release(); tempBlob->Release(); varBlob->Release(); denoiseBlob->Release(); Log("RTGI: Failed to compile composite PS"); return false; }

    ID3DBlob* aoCompBlob = host->CompileShaderFromFile((gShaderDir + "rtgi_ao_composite_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!aoCompBlob) { vsBlob->Release(); rtBlob->Release(); tempBlob->Release(); varBlob->Release(); denoiseBlob->Release(); compBlob->Release(); Log("RTGI: Failed to compile AO composite PS"); return false; }

    ID3DBlob* dbgBlob = host->CompileShaderFromFile((gShaderDir + "rtgi_debug_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!dbgBlob) { vsBlob->Release(); rtBlob->Release(); tempBlob->Release(); varBlob->Release(); denoiseBlob->Release(); compBlob->Release(); aoCompBlob->Release(); Log("RTGI: Failed to compile debug PS"); return false; }

    HRESULT hr;

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &gFullscreenVS);
    vsBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreateComputeShader(rtBlob->GetBufferPointer(), rtBlob->GetBufferSize(), nullptr, &gRayTraceCS);
    rtBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(tempBlob->GetBufferPointer(), tempBlob->GetBufferSize(), nullptr, &gTemporalPS);
    tempBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(varBlob->GetBufferPointer(), varBlob->GetBufferSize(), nullptr, &gVariancePS);
    varBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreateComputeShader(denoiseBlob->GetBufferPointer(), denoiseBlob->GetBufferSize(), nullptr, &gAtrousCS);
    denoiseBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(compBlob->GetBufferPointer(), compBlob->GetBufferSize(), nullptr, &gCompositePS);
    compBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(aoCompBlob->GetBufferPointer(), aoCompBlob->GetBufferSize(), nullptr, &gAOCompositePS);
    aoCompBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(dbgBlob->GetBufferPointer(), dbgBlob->GetBufferSize(), nullptr, &gDebugPS);
    dbgBlob->Release();
    if (FAILED(hr)) return false;

    // Textures
    if (!CreateTextures(device, width, height))
        return false;

    // Pipeline states
    {
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].BlendEnable = FALSE;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &gNoBlend);
        if (FAILED(hr)) return false;
    }
    {
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].BlendEnable = TRUE;
        desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &gAdditiveBlend);
        if (FAILED(hr)) return false;
    }
    {
        // Multiply blend: dest = dest * src
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].BlendEnable = TRUE;
        desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &gMultiplyBlend);
        if (FAILED(hr)) return false;
    }
    {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = FALSE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        hr = device->CreateDepthStencilState(&desc, &gNoDepthDSS);
        if (FAILED(hr)) return false;
    }
    {
        D3D11_RASTERIZER_DESC desc = {};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        desc.DepthClipEnable = FALSE;
        hr = device->CreateRasterizerState(&desc, &gNoCullRS);
        if (FAILED(hr)) return false;
    }
    {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = device->CreateSamplerState(&desc, &gPointClampSampler);
        if (FAILED(hr)) return false;
    }
    {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&desc, &gLinearClampSampler);
        if (FAILED(hr)) return false;
    }

    // Constant buffers
    gRayTraceCB = host->CreateConstantBuffer(device, sizeof(RayTraceCBData));
    gTemporalCB = host->CreateConstantBuffer(device, sizeof(TemporalCBData));
    gVarianceCB = host->CreateConstantBuffer(device, sizeof(VarianceCBData));
    gDenoiseCB = host->CreateConstantBuffer(device, sizeof(DenoiseCBData));
    gCompositeCB = host->CreateConstantBuffer(device, sizeof(CompositeCBData));
    gDebugCB = host->CreateConstantBuffer(device, sizeof(DebugCBData));
    if (!gRayTraceCB || !gTemporalCB || !gVarianceCB || !gDenoiseCB || !gCompositeCB || !gDebugCB)
        return false;

    gLastResScale = gRTGIConfig.resolutionScale;
    gInitialized = true;
    Log("RTGI: Initialized successfully (%ux%u, render %ux%u)", width, height, gRenderWidth, gRenderHeight);
    return true;
}

void Shutdown()
{
    ReleaseTextures();

    SAFE_RELEASE(gFullscreenVS);
    SAFE_RELEASE(gRayTraceCS);    SAFE_RELEASE(gTemporalPS);
    SAFE_RELEASE(gVariancePS);    SAFE_RELEASE(gAtrousCS);
    SAFE_RELEASE(gCompositePS);   SAFE_RELEASE(gAOCompositePS);
    SAFE_RELEASE(gDebugPS);
    SAFE_RELEASE(gNoBlend);       SAFE_RELEASE(gAdditiveBlend);
    SAFE_RELEASE(gMultiplyBlend); SAFE_RELEASE(gNoDepthDSS);
    SAFE_RELEASE(gNoCullRS);      SAFE_RELEASE(gPointClampSampler);
    SAFE_RELEASE(gLinearClampSampler);
    SAFE_RELEASE(gRayTraceCB);    SAFE_RELEASE(gTemporalCB);
    SAFE_RELEASE(gVarianceCB);    SAFE_RELEASE(gDenoiseCB);
    SAFE_RELEASE(gCompositeCB);   SAFE_RELEASE(gDebugCB);
    gInitialized = false;
    gHasValidCameraData = false;
    gHasPrevFrame = false;
    gFrameIndex = 0;
    gFinalGISRV = nullptr;
    gHost = nullptr;
    Log("RTGI: Shut down");
}

void OnResolutionChanged(ID3D11Device* device, UINT newWidth, UINT newHeight)
{
    if (newWidth == gWidth && newHeight == gHeight) return;
    if (newWidth == 0 || newHeight == 0) return;

    Log("RTGI: Resolution changed: %ux%u -> %ux%u", gWidth, gHeight, newWidth, newHeight);
    ReleaseTextures();
    gWidth = newWidth;
    gHeight = newHeight;

    if (!CreateTextures(device, newWidth, newHeight))
    {
        Log("RTGI: WARNING: Failed to recreate textures");
        gInitialized = false;
    }

    // Reset temporal state
    gHasPrevFrame = false;
    gAccumWriteIndex = 0;
}

bool IsInitialized() { return gInitialized; }
bool HasValidCameraData() { return gHasValidCameraData; }
void GetRenderSize(UINT* w, UINT* h) { *w = gRenderWidth; *h = gRenderHeight; }

void UpdateCameraData(const DustCameraData* camera)
{
    if (!camera || !camera->valid) return;

    memcpy(gPrevInverseView, gInverseView, sizeof(gInverseView));
    memcpy(gInverseView, camera->inverseView, sizeof(gInverseView));

    if (gHasValidCameraData)
        gHasPrevFrame = true;
    gHasValidCameraData = true;
}

// ==================== Render Passes ====================

ID3D11ShaderResourceView* RenderGI(ID3D11DeviceContext* ctx,
                                    ID3D11ShaderResourceView* depthSRV,
                                    ID3D11ShaderResourceView* sceneSRV,
                                    ID3D11ShaderResourceView* normalsSRV)
{
    if (!gInitialized || !ctx || !depthSRV || !sceneSRV || !gHost)
        return nullptr;

    // Defensive resolution fallback — in case the framework misses an
    // OnResolutionChanged call. Every 300 frames (~5s at 60fps) is enough
    // to self-correct without paying the QI/GetDesc cost every frame.
    if ((gFrameIndex % 300) == 0)
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
        if (!gInitialized) return nullptr;
    }

    // Detect resolution-scale change at runtime and recreate textures
    if (gRTGIConfig.resolutionScale != gLastResScale)
    {
        Log("RTGI: Resolution scale changed: %d%% -> %d%%, recreating textures", gLastResScale, gRTGIConfig.resolutionScale);
        gLastResScale = gRTGIConfig.resolutionScale;
        ReleaseTextures();
        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        if (device)
        {
            if (!CreateTextures(device, gWidth, gHeight))
            {
                Log("RTGI: WARNING: Failed to recreate textures after resolution-mode change");
                gInitialized = false;
                device->Release();
                return nullptr;
            }
            device->Release();
        }
        gHasPrevFrame = false;
        gAccumWriteIndex = 0;
    }

    gHost->SaveState(ctx);

    // Common state
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(gFullscreenVS, nullptr, 0);
    ctx->RSSetState(gNoCullRS);
    ctx->OMSetDepthStencilState(gNoDepthDSS, 0);

    float blendFactor[4] = { 0, 0, 0, 0 };

    // Internal render viewport (may be half-res)
    D3D11_VIEWPORT renderVP = {};
    renderVP.Width = (float)gRenderWidth;
    renderVP.Height = (float)gRenderHeight;
    renderVP.MaxDepth = 1.0f;

    float aspect = (float)gWidth / (float)gHeight;

    // ---- Pass 1: Ray Trace (compute shader) ----
    {
        // Unbind any render target so the raw texture UAV is not simultaneously bound as RTV
        ID3D11RenderTargetView* nullRTV = nullptr;
        ctx->OMSetRenderTargets(1, &nullRTV, nullptr);

        ctx->CSSetShader(gRayTraceCS, nullptr, 0);

        ID3D11ShaderResourceView* prevGISRV = (gAccumWriteIndex == 0) ? gAccumSRVB : gAccumSRVA;

        // t0=depth, t1=scene, t2=prevGI, t3=normals
        ID3D11ShaderResourceView* srvs[4] = { depthSRV, sceneSRV, prevGISRV, normalsSRV };
        ctx->CSSetShaderResources(0, 4, srvs);
        ID3D11SamplerState* samplers[2] = { gPointClampSampler, gLinearClampSampler };
        ctx->CSSetSamplers(0, 2, samplers);

        RayTraceCBData cb = {};
        cb.viewportSize[0] = (float)gRenderWidth;
        cb.viewportSize[1] = (float)gRenderHeight;
        cb.invViewportSize[0] = 1.0f / (float)gRenderWidth;
        cb.invViewportSize[1] = 1.0f / (float)gRenderHeight;
        cb.tanHalfFov = gRTGIConfig.tanHalfFov;
        cb.aspectRatio = aspect;
        cb.rayLength = gRTGIConfig.rayLength;
        cb.raySteps = (float)gRTGIConfig.raySteps;
        cb.thickness = gRTGIConfig.thickness;
        cb.fadeDistance = gRTGIConfig.fadeDistance;
        cb.bounceIntensity = gRTGIConfig.bounceIntensity;
        cb.aoIntensity = gRTGIConfig.aoIntensity;
        cb.frameIndex = (float)gFrameIndex;
        cb.raysPerPixel = (float)gRTGIConfig.raysPerPixel;
        cb.thicknessCurve = gRTGIConfig.thicknessCurve;
        cb.normalDetail = gRTGIConfig.normalDetail;
        // Sub-pixel jitter: Halton(2,3) indexed by a 16-frame cycle. Skip i=0
        // (which Halton returns as (0,0)) so every frame contributes a unique offset.
        {
            uint32_t jIdx = (uint32_t)(gFrameIndex & 15u) + 1u;
            cb.sampleJitter[0] = Halton(jIdx, 2) - 0.5f;
            cb.sampleJitter[1] = Halton(jIdx, 3) - 0.5f;
        }
        // Extract COLUMNS of inverseView (= rows of view matrix = camera axes in world space)
        cb.camRight[0]   = gInverseView[0]; cb.camRight[1]   = gInverseView[4]; cb.camRight[2]   = gInverseView[8];  cb.camRight[3]   = 0;
        cb.camUp[0]      = gInverseView[1]; cb.camUp[1]      = gInverseView[5]; cb.camUp[2]      = gInverseView[9];  cb.camUp[3]      = 0;
        cb.camForward[0] = gInverseView[2]; cb.camForward[1] = gInverseView[6]; cb.camForward[2] = gInverseView[10]; cb.camForward[3] = 0;
        gHost->UpdateConstantBuffer(ctx, gRayTraceCB, &cb, sizeof(cb));
        ctx->CSSetConstantBuffers(0, 1, &gRayTraceCB);

        UINT initialCount = 0;
        ctx->CSSetUnorderedAccessViews(0, 1, &gRawUAV, &initialCount);
        ctx->Dispatch((gRenderWidth + 7) / 8, (gRenderHeight + 7) / 8, 1);

        // Unbind CS resources
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, &initialCount);
        ID3D11ShaderResourceView* nullSRVs[4] = {};
        ctx->CSSetShaderResources(0, 4, nullSRVs);
    }

    // ---- Pass 2: Temporal Accumulation (single RT: color+AO) ----
    {
        // Write to current accumulation buffer, read history from the other
        ID3D11RenderTargetView* accumWriteRTV = (gAccumWriteIndex == 0) ? gAccumRTVA : gAccumRTVB;
        ID3D11ShaderResourceView* accumHistorySRV = (gAccumWriteIndex == 0) ? gAccumSRVB : gAccumSRVA;

        ctx->OMSetRenderTargets(1, &accumWriteRTV, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->RSSetViewports(1, &renderVP);
        ctx->PSSetShader(gTemporalPS, nullptr, 0);

        // t0=currentGI, t1=historyColor, t2=depth, t3=prevDepth
        ID3D11ShaderResourceView* srvs[4] = { gRawSRV, accumHistorySRV, depthSRV, gPrevDepthSRV };
        ctx->PSSetShaderResources(0, 4, srvs);
        ID3D11SamplerState* samplers[2] = { gPointClampSampler, gLinearClampSampler };
        ctx->PSSetSamplers(0, 2, samplers);

        TemporalCBData cb = {};
        cb.viewportSize[0] = (float)gRenderWidth;
        cb.viewportSize[1] = (float)gRenderHeight;
        cb.invViewportSize[0] = 1.0f / (float)gRenderWidth;
        cb.invViewportSize[1] = 1.0f / (float)gRenderHeight;
        cb.tanHalfFov = gRTGIConfig.tanHalfFov;
        cb.aspectRatio = aspect;
        cb.temporalBlend = gHasPrevFrame ? gRTGIConfig.temporalBlend : 0.0f;
        cb.frameIndex = (float)gFrameIndex;
        if (gHasPrevFrame)
        {
            ComputeReprojectionMatrix(gInverseView, gPrevInverseView, cb.reprojMatrix);
            float tx = cb.reprojMatrix[12], ty = cb.reprojMatrix[13], tz = cb.reprojMatrix[14];
            float instantMotion = sqrtf(tx * tx + ty * ty + tz * tz);
            // EMA smoothing: fast attack, slow decay (~20 frames to halve)
            if (instantMotion > gSmoothedMotion)
                gSmoothedMotion = instantMotion;
            else
                gSmoothedMotion = gSmoothedMotion * 0.75f + instantMotion * 0.25f;
            cb.motionMagnitude = gSmoothedMotion;
        }
        else
        {
            memset(cb.reprojMatrix, 0, 64);
            cb.reprojMatrix[0] = cb.reprojMatrix[5] = cb.reprojMatrix[10] = cb.reprojMatrix[15] = 1.0f;
            cb.motionMagnitude = 0.0f;
            gSmoothedMotion = 0.0f;
        }
        gHost->UpdateConstantBuffer(ctx, gTemporalCB, &cb, sizeof(cb));
        ctx->PSSetConstantBuffers(0, 1, &gTemporalCB);

        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullSRVs[4] = {};
        ctx->PSSetShaderResources(0, 4, nullSRVs);
    }

    // The accumulation result to denoise
    ID3D11ShaderResourceView* accumResultSRV = (gAccumWriteIndex == 0) ? gAccumSRVA : gAccumSRVB;

    // ---- Pass 2b: Variance estimate (3x3 luminance stddev) ----
    // Computed once from the temporal output; atrous reads this value across
    // all denoise iterations instead of recomputing it 4 times.
    {
        ctx->OMSetRenderTargets(1, &gMomentsRTVA, nullptr);
        ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
        ctx->RSSetViewports(1, &renderVP);
        ctx->PSSetShader(gVariancePS, nullptr, 0);

        ctx->PSSetShaderResources(0, 1, &accumResultSRV);
        ctx->PSSetSamplers(0, 1, &gPointClampSampler);

        VarianceCBData cb = {};
        cb.viewportSize[0] = (float)gRenderWidth;
        cb.viewportSize[1] = (float)gRenderHeight;
        cb.invViewportSize[0] = 1.0f / (float)gRenderWidth;
        cb.invViewportSize[1] = 1.0f / (float)gRenderHeight;
        gHost->UpdateConstantBuffer(ctx, gVarianceCB, &cb, sizeof(cb));
        ctx->PSSetConstantBuffers(0, 1, &gVarianceCB);

        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSRV);
    }

    // ---- Pass 3: A-Trous Denoise (compute shader, multiple iterations) ----
    // State that's invariant across iterations is bound once before the loop —
    // trims ~6 redundant D3D11 calls per iteration.
    ID3D11ShaderResourceView* denoiseInputSRV = accumResultSRV;
    ID3D11ShaderResourceView* denoiseOutputSRV = accumResultSRV; // fallback if 0 iterations

    int numDenoiseSteps = gRTGIConfig.denoiseSteps;
    if (numDenoiseSteps < 0) numDenoiseSteps = 0;
    if (numDenoiseSteps > 5) numDenoiseSteps = 5;

    if (numDenoiseSteps > 0)
    {
        // Unbind the temporal RT so its SRV (accumResultSRV) is safe to use as input
        ID3D11RenderTargetView* nullRTV = nullptr;
        ctx->OMSetRenderTargets(1, &nullRTV, nullptr);

        ctx->CSSetShader(gAtrousCS, nullptr, 0);
        ctx->CSSetSamplers(0, 1, &gPointClampSampler);
        ctx->CSSetConstantBuffers(0, 1, &gDenoiseCB);

        // t1=depth, t2=normals, t3=variance — same resources across all iterations
        ID3D11ShaderResourceView* invariantSrvs[3] = { depthSRV, normalsSRV, gMomentsSRVA };
        ctx->CSSetShaderResources(1, 3, invariantSrvs);

        UINT dispatchX = (gRenderWidth + 7) / 8;
        UINT dispatchY = (gRenderHeight + 7) / 8;
        UINT initialCount = 0;

        for (int i = 0; i < numDenoiseSteps; i++)
        {
            ID3D11UnorderedAccessView* writeUAV;
            if (i == 0)
            {
                writeUAV = gDenoiseUAVA;
                denoiseOutputSRV = gDenoiseSRVA;
                denoiseInputSRV = accumResultSRV;
            }
            else if (i % 2 == 1)
            {
                writeUAV = gDenoiseUAVB;
                denoiseOutputSRV = gDenoiseSRVB;
                denoiseInputSRV = gDenoiseSRVA;
            }
            else
            {
                writeUAV = gDenoiseUAVA;
                denoiseOutputSRV = gDenoiseSRVA;
                denoiseInputSRV = gDenoiseSRVB;
            }

            ctx->CSSetShaderResources(0, 1, &denoiseInputSRV);
            ctx->CSSetUnorderedAccessViews(0, 1, &writeUAV, &initialCount);

            DenoiseCBData cb = {};
            cb.viewportSize[0] = (float)gRenderWidth;
            cb.viewportSize[1] = (float)gRenderHeight;
            cb.invViewportSize[0] = 1.0f / (float)gRenderWidth;
            cb.invViewportSize[1] = 1.0f / (float)gRenderHeight;
            cb.tanHalfFov = gRTGIConfig.tanHalfFov;
            cb.aspectRatio = aspect;
            cb.stepSize = (float)(1 << i); // 1, 2, 4, 8, 16
            cb.depthSigma = gRTGIConfig.depthSigma;
            cb.phiColor = gRTGIConfig.phiColor;
            cb.fadeDistance = gRTGIConfig.fadeDistance;
            gHost->UpdateConstantBuffer(ctx, gDenoiseCB, &cb, sizeof(cb));

            ctx->Dispatch(dispatchX, dispatchY, 1);

            // Unbind UAV so the same texture can be bound as SRV next iteration
            ID3D11UnorderedAccessView* nullUAV = nullptr;
            ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, &initialCount);
        }

        // Clear CS SRVs so the bindings don't linger past RenderGI
        ID3D11ShaderResourceView* nullSRVs[4] = {};
        ctx->CSSetShaderResources(0, 4, nullSRVs);
    }

    // Final GI result
    ID3D11ShaderResourceView* finalGI = (numDenoiseSteps > 0) ? denoiseOutputSRV : accumResultSRV;

    // ---- Save depth for next frame's temporal pass ----
    {
        ID3D11Resource* depthResource = nullptr;
        depthSRV->GetResource(&depthResource);
        if (depthResource && gPrevDepthTex)
        {
            ctx->CopyResource(gPrevDepthTex, depthResource);
            depthResource->Release();
        }
    }

    // Swap accumulation buffer index for next frame
    gAccumWriteIndex = 1 - gAccumWriteIndex;
    gFrameIndex++;

    gFinalGISRV = finalGI;

    gHost->RestoreState(ctx);

    return finalGI;
}

void RenderDebugOverlay(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* hdrRTV,
                        ID3D11ShaderResourceView* depthSRV,
                        ID3D11ShaderResourceView* normalsSRV)
{
    if (!gInitialized || !ctx || !hdrRTV || gRTGIConfig.debugView == 0 || !gHost)
        return;

    if (!gFinalGISRV) return;

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
    ctx->PSSetShader(gDebugPS, nullptr, 0);

    // t0=GI, t1=depth, t2=normals
    ID3D11ShaderResourceView* srvs[3] = { gFinalGISRV, depthSRV, normalsSRV };
    ctx->PSSetShaderResources(0, 3, srvs);
    ID3D11SamplerState* samplers[1] = { gPointClampSampler };
    ctx->PSSetSamplers(0, 1, samplers);

    float aspect = (float)gWidth / (float)gHeight;
    DebugCBData cb = {};
    cb.debugMode = (float)gRTGIConfig.debugView;
    cb.tanHalfFov = gRTGIConfig.tanHalfFov;
    cb.aspectRatio = aspect;
    cb.camRight[0]   = gInverseView[0]; cb.camRight[1]   = gInverseView[4]; cb.camRight[2]   = gInverseView[8];  cb.camRight[3]   = 0;
    cb.camUp[0]      = gInverseView[1]; cb.camUp[1]      = gInverseView[5]; cb.camUp[2]      = gInverseView[9];  cb.camUp[3]      = 0;
    cb.camForward[0] = gInverseView[2]; cb.camForward[1] = gInverseView[6]; cb.camForward[2] = gInverseView[10]; cb.camForward[3] = 0;
    gHost->UpdateConstantBuffer(ctx, gDebugCB, &cb, sizeof(cb));
    ctx->PSSetConstantBuffers(0, 1, &gDebugCB);

    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSRVs[3] = {};
    ctx->PSSetShaderResources(0, 3, nullSRVs);

    gHost->RestoreState(ctx);
}

#undef SAFE_RELEASE

} // namespace RTGIRenderer
