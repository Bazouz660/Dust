#include "SSAORenderer.h"
#include "SSAOConfig.h"
#include "DustLog.h"
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <random>

// SSAO 6-pass pipeline:
//   1. Deinterleave depth   -> ZSrc          (R32F, screen-size)
//   2. Occlusion pass       -> OccRaw        (RGBA16F, deinterleaved layout)
//   3. Reinterleave         -> AoTex1        (RGBA16F)
//   4. Filter 1 (optional)  -> AoTex2        (RGBA16F)
//   5. Filter 2 (final)     -> gFilteredAOTex (RGBA16F)
//   6. Resolve              -> gAoTex        (R8_UNORM, bound to deferred slot 8)

namespace SSAORenderer
{

static bool gInitialized = false;
static UINT gWidth = 0;
static UINT gHeight = 0;
static const DustHostAPI* gHost = nullptr;
static std::string gShaderDir;
static uint64_t gFrameIndex = 0;

// Tile count for deinterleaving (4 if width%4==0 else 5)
static int gDeinterleaveTileCount = 4;
static int gDeinterleaveHigh = 0;

// Final R8 AO output — what gets bound to deferred slot 8
static ID3D11Texture2D*          gAoTex = nullptr;
static ID3D11RenderTargetView*   gAoRTV = nullptr;
static ID3D11ShaderResourceView* gAoSRV = nullptr;

// Pipeline intermediate textures (all screen-size, persist across frames so
// shading-rate skipping retains previous-frame values)
static ID3D11Texture2D*          gZSrcTex = nullptr;
static ID3D11RenderTargetView*   gZSrcRTV = nullptr;
static ID3D11ShaderResourceView* gZSrcSRV = nullptr;

static ID3D11Texture2D*          gOccRawTex = nullptr;
static ID3D11RenderTargetView*   gOccRawRTV = nullptr;
static ID3D11ShaderResourceView* gOccRawSRV = nullptr;

static ID3D11Texture2D*          gAoTex1 = nullptr;
static ID3D11RenderTargetView*   gAoTex1RTV = nullptr;
static ID3D11ShaderResourceView* gAoTex1SRV = nullptr;

static ID3D11Texture2D*          gAoTex2 = nullptr;
static ID3D11RenderTargetView*   gAoTex2RTV = nullptr;
static ID3D11ShaderResourceView* gAoTex2SRV = nullptr;

// Filter2 output (RGBA16F: .x=AO, .y=depth marker). Always feeds the resolve pass.
static ID3D11Texture2D*          gFilteredAOTex = nullptr;
static ID3D11RenderTargetView*   gFilteredAORTV = nullptr;
static ID3D11ShaderResourceView* gFilteredAOSRV = nullptr;

static ID3D11ShaderResourceView* gLastDepthSRV = nullptr;  // depthSRV from RenderAO, reused by debug overlay

// Procedural 64x64 blue noise tile — generated once at init via Ulichney's
// void-and-cluster method. Bound at t2 of the occlusion pass; sampled by
// get_jitter with a per-frame 2D offset for per-pixel-per-frame variation.
static ID3D11Texture2D*          gBlueNoiseTex = nullptr;
static ID3D11ShaderResourceView* gBlueNoiseSRV = nullptr;

// Shaders
static ID3D11VertexShader* gFullscreenVS    = nullptr;
static ID3D11PixelShader*  gDeinterleavePS = nullptr;
static ID3D11PixelShader*  gOcclusionPS   = nullptr;
static ID3D11PixelShader*  gReinterleavePS   = nullptr;
static ID3D11PixelShader*  gFilter1PS      = nullptr;
static ID3D11PixelShader*  gFilter2PS      = nullptr;
static ID3D11PixelShader*  gResolvePS      = nullptr;
static ID3D11PixelShader*  gSSAODebugPS        = nullptr;

// Pipeline states
static ID3D11BlendState*        gNoBlend = nullptr;
static ID3D11DepthStencilState* gNoDepthDSS = nullptr;
static ID3D11RasterizerState*   gNoCullRS = nullptr;
static ID3D11SamplerState*      gPointClampSampler = nullptr;

struct AOPassCB
{
    float bufferPixelSize[2];     // (1/w, 1/h)
    float bufferScreenSize[2];    // (w, h)

    float bufferAspectRatio[2];   // (1, w/h)
    float tanHalfFov;
    float farPlane;

    float sampleRadius;
    float ssaoAmount;
    float fadeDepth;
    float worldspaceEnable;

    int   sampleQualityPreset;
    int   shadingRate;
    int   filterSize;
    int   aoType;

    int   deinterleaveTileCount;
    int   deinterleaveHigh;
    uint32_t frameCount;
    float debugView;

};
static ID3D11Buffer* gPassCB = nullptr;

// ==================== Helpers ====================

// Generate a 64x64 R8 blue noise tile via the void-and-cluster method
// (Ulichney 1993). Each output value is its dither rank in [0,255], so the
// final tile is spatially decorrelated at all thresholds — the property that
// makes it useful as a jitter source for shading rate dithering.
static void GenerateBlueNoise64(uint8_t* out)
{
    constexpr int SIZE = 64;
    constexpr int N = SIZE * SIZE;
    constexpr int R = 4;
    constexpr float SIGMA2 = 1.9f * 1.9f;
    constexpr int INITIAL_ONES = N / 10;

    float kernel[2*R+1][2*R+1];
    for (int dy = -R; dy <= R; ++dy)
        for (int dx = -R; dx <= R; ++dx)
            kernel[dy+R][dx+R] = expf(-(float)(dx*dx + dy*dy) / (2.0f * SIGMA2));

    auto wrap = [&](int x) { return ((x % SIZE) + SIZE) % SIZE; };

    std::vector<float>   filtered(N, 0.0f);
    std::vector<uint8_t> pattern(N, 0);

    auto apply = [&](std::vector<float>& f, int idx, float sign) {
        int x = idx % SIZE, y = idx / SIZE;
        for (int dy = -R; dy <= R; ++dy)
            for (int dx = -R; dx <= R; ++dx)
                f[wrap(y+dy)*SIZE + wrap(x+dx)] += sign * kernel[dy+R][dx+R];
    };

    std::mt19937 rng(0xB100Du);
    int placed = 0;
    while (placed < INITIAL_ONES) {
        int idx = (int)(rng() % (uint32_t)N);
        if (!pattern[idx]) { pattern[idx] = 1; apply(filtered, idx, 1.0f); ++placed; }
    }

    // Phase 1: relax the initial random pattern by repeatedly moving the
    // tightest cluster to the largest void until they coincide.
    for (;;) {
        int maxIdx = -1; float maxVal = -1e30f;
        for (int i = 0; i < N; ++i)
            if (pattern[i] && filtered[i] > maxVal) { maxVal = filtered[i]; maxIdx = i; }
        pattern[maxIdx] = 0; apply(filtered, maxIdx, -1.0f);
        int minIdx = -1; float minVal = 1e30f;
        for (int i = 0; i < N; ++i)
            if (!pattern[i] && filtered[i] < minVal) { minVal = filtered[i]; minIdx = i; }
        if (minIdx == maxIdx) { pattern[maxIdx] = 1; apply(filtered, maxIdx, 1.0f); break; }
        pattern[minIdx] = 1; apply(filtered, minIdx, 1.0f);
    }

    std::vector<int> rank(N, -1);

    // Phase 2: rank prototype pixels (ranks INITIAL_ONES-1 → 0) by repeatedly
    // pulling out the tightest cluster.
    {
        std::vector<uint8_t> p = pattern;
        std::vector<float>   f = filtered;
        for (int r = INITIAL_ONES - 1; r >= 0; --r) {
            int maxIdx = -1; float maxVal = -1e30f;
            for (int i = 0; i < N; ++i)
                if (p[i] && f[i] > maxVal) { maxVal = f[i]; maxIdx = i; }
            rank[maxIdx] = r;
            p[maxIdx] = 0;
            apply(f, maxIdx, -1.0f);
        }
    }

    // Phase 3: rank remaining pixels (ranks INITIAL_ONES → N-1) by repeatedly
    // filling the largest void.
    {
        std::vector<uint8_t> p = pattern;
        std::vector<float>   f = filtered;
        for (int r = INITIAL_ONES; r < N; ++r) {
            int minIdx = -1; float minVal = 1e30f;
            for (int i = 0; i < N; ++i)
                if (!p[i] && f[i] < minVal) { minVal = f[i]; minIdx = i; }
            rank[minIdx] = r;
            p[minIdx] = 1;
            apply(f, minIdx, 1.0f);
        }
    }

    for (int i = 0; i < N; ++i)
        out[i] = (uint8_t)((rank[i] * 256) / N);
}

static bool CreateBlueNoiseTexture(ID3D11Device* device)
{
    constexpr int SIZE = 64;
    std::vector<uint8_t> noise(SIZE * SIZE);
    GenerateBlueNoise64(noise.data());

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = SIZE;
    desc.Height = SIZE;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = noise.data();
    init.SysMemPitch = SIZE;

    HRESULT hr = device->CreateTexture2D(&desc, &init, &gBlueNoiseTex);
    if (FAILED(hr)) { Log("Failed to create blue noise texture: 0x%08X", hr); return false; }
    hr = device->CreateShaderResourceView(gBlueNoiseTex, nullptr, &gBlueNoiseSRV);
    if (FAILED(hr)) { Log("Failed to create blue noise SRV: 0x%08X", hr); return false; }
    return true;
}

static bool CreateTexture(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format,
                          ID3D11Texture2D** outTex,
                          ID3D11RenderTargetView** outRTV,
                          ID3D11ShaderResourceView** outSRV)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, outTex);
    if (FAILED(hr)) { Log("Failed to create texture (%ux%u fmt=%d): 0x%08X", w, h, format, hr); return false; }

    hr = device->CreateRenderTargetView(*outTex, nullptr, outRTV);
    if (FAILED(hr)) { Log("Failed to create RTV: 0x%08X", hr); return false; }

    hr = device->CreateShaderResourceView(*outTex, nullptr, outSRV);
    if (FAILED(hr)) { Log("Failed to create SRV: 0x%08X", hr); return false; }

    return true;
}

static void ReleasePipelineTextures()
{
#define SR(p) if (p) { (p)->Release(); (p) = nullptr; }
    SR(gAoTex);      SR(gAoRTV);      SR(gAoSRV);
    SR(gZSrcTex);    SR(gZSrcRTV);    SR(gZSrcSRV);
    SR(gOccRawTex); SR(gOccRawRTV); SR(gOccRawSRV);
    SR(gAoTex1); SR(gAoTex1RTV); SR(gAoTex1SRV);
    SR(gAoTex2); SR(gAoTex2RTV); SR(gAoTex2SRV);
    SR(gFilteredAOTex); SR(gFilteredAORTV); SR(gFilteredAOSRV);
#undef SR
}

static bool CreatePipelineTextures(ID3D11Device* device, UINT w, UINT h)
{
    if (!CreateTexture(device, w, h, DXGI_FORMAT_R8_UNORM,        &gAoTex,     &gAoRTV,     &gAoSRV)) return false;
    // R32F for deinterleaved depth — z = d*1000+1 where d ∈ [0,1];
    // R16F precision at z~1000 is ~1.0 unit, creating visible iso-depth
    // banding in the bitmask sector mapping. R32F gives ~1e-4 precision at
    // the same range, eliminating the banding.
    if (!CreateTexture(device, w, h, DXGI_FORMAT_R32_FLOAT,       &gZSrcTex,   &gZSrcRTV,   &gZSrcSRV)) return false;
    if (!CreateTexture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, &gOccRawTex,  &gOccRawRTV,  &gOccRawSRV)) return false;
    if (!CreateTexture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, &gAoTex1, &gAoTex1RTV, &gAoTex1SRV)) return false;
    if (!CreateTexture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, &gAoTex2, &gAoTex2RTV, &gAoTex2SRV)) return false;
    if (!CreateTexture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, &gFilteredAOTex, &gFilteredAORTV, &gFilteredAOSRV)) return false;

    // Clear once on creation so shading-rate-skipped pixels don't read garbage.
    return true;
}

static void ClearPipelineTextures(ID3D11DeviceContext* ctx)
{
    float clr[4] = { 1, 0, 1, 0 }; // AO=1 (no occlusion), depth=0
    if (gOccRawRTV) ctx->ClearRenderTargetView(gOccRawRTV, clr);
    if (gAoTex1RTV) ctx->ClearRenderTargetView(gAoTex1RTV, clr);
    if (gAoTex2RTV) ctx->ClearRenderTargetView(gAoTex2RTV, clr);
    if (gFilteredAORTV) ctx->ClearRenderTargetView(gFilteredAORTV, clr);
    float white[4] = { 1, 1, 1, 1 };
    if (gAoRTV) ctx->ClearRenderTargetView(gAoRTV, white);
}

// ==================== Public API ====================

bool Init(ID3D11Device* device, UINT width, UINT height, const DustHostAPI* host, const char* effectDir)
{
    if (gInitialized) return true;

    gHost = host;
    gShaderDir = std::string(effectDir) + "\\shaders\\";
    Log("Initializing SSAO renderer (%ux%u), shaders: %s", width, height, gShaderDir.c_str());
    gWidth = width;
    gHeight = height;
    gDeinterleaveTileCount = ((width / 4) * 4 == width) ? 4 : 5;
    gDeinterleaveHigh      = (gDeinterleaveTileCount == 5) ? 1 : 0;

    // Compile shaders
    auto compile = [&](const char* name, const char* target) -> ID3DBlob* {
        return host->CompileShaderFromFile((gShaderDir + name).c_str(), "main", target);
    };

    ID3DBlob* vsBlob = compile("fullscreen_vs.hlsl", "vs_5_0");
    if (!vsBlob) return false;
    ID3DBlob* deinterBlob = compile("ao_deinterleave_ps.hlsl", "ps_5_0");
    ID3DBlob* occ1Blob    = compile("ao_occlusion_ps.hlsl",   "ps_5_0");
    ID3DBlob* occ2Blob    = compile("ao_reinterleave_ps.hlsl",   "ps_5_0");
    ID3DBlob* filt1Blob   = compile("ao_filter1_ps.hlsl",      "ps_5_0");
    ID3DBlob* filt2Blob   = compile("ao_filter2_ps.hlsl",      "ps_5_0");
    ID3DBlob* resolveBlob = compile("ao_resolve_ps.hlsl",      "ps_5_0");
    ID3DBlob* debugBlob   = compile("ssao_debug_ps.hlsl",        "ps_5_0");

    if (!deinterBlob || !occ1Blob || !occ2Blob || !filt1Blob || !filt2Blob || !resolveBlob || !debugBlob)
    {
        Log("Failed to compile AO shaders");
        if (vsBlob) vsBlob->Release();
        if (deinterBlob) deinterBlob->Release();
        if (occ1Blob) occ1Blob->Release();
        if (occ2Blob) occ2Blob->Release();
        if (filt1Blob) filt1Blob->Release();
        if (filt2Blob) filt2Blob->Release();
        if (resolveBlob) resolveBlob->Release();
        if (debugBlob) debugBlob->Release();
        return false;
    }

    HRESULT hr;
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &gFullscreenVS);
    vsBlob->Release();
    if (FAILED(hr)) { Log("CreateVertexShader failed: 0x%08X", hr); return false; }

    hr = device->CreatePixelShader(deinterBlob->GetBufferPointer(), deinterBlob->GetBufferSize(), nullptr, &gDeinterleavePS);
    deinterBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(occ1Blob->GetBufferPointer(), occ1Blob->GetBufferSize(), nullptr, &gOcclusionPS);
    occ1Blob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(occ2Blob->GetBufferPointer(), occ2Blob->GetBufferSize(), nullptr, &gReinterleavePS);
    occ2Blob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(filt1Blob->GetBufferPointer(), filt1Blob->GetBufferSize(), nullptr, &gFilter1PS);
    filt1Blob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(filt2Blob->GetBufferPointer(), filt2Blob->GetBufferSize(), nullptr, &gFilter2PS);
    filt2Blob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(resolveBlob->GetBufferPointer(), resolveBlob->GetBufferSize(), nullptr, &gResolvePS);
    resolveBlob->Release();
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(debugBlob->GetBufferPointer(), debugBlob->GetBufferSize(), nullptr, &gSSAODebugPS);
    debugBlob->Release();
    if (FAILED(hr)) return false;

    if (!CreatePipelineTextures(device, width, height)) return false;
    if (!CreateBlueNoiseTexture(device)) return false;

    // States
    {
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].BlendEnable = FALSE;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &gNoBlend); if (FAILED(hr)) return false;
    }
    {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = FALSE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        hr = device->CreateDepthStencilState(&desc, &gNoDepthDSS); if (FAILED(hr)) return false;
    }
    {
        D3D11_RASTERIZER_DESC desc = {};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        desc.DepthClipEnable = FALSE;
        hr = device->CreateRasterizerState(&desc, &gNoCullRS); if (FAILED(hr)) return false;
    }
    {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = device->CreateSamplerState(&desc, &gPointClampSampler); if (FAILED(hr)) return false;
    }

    gPassCB = host->CreateConstantBuffer(device, sizeof(AOPassCB));
    if (!gPassCB) return false;

    gInitialized = true;
    Log("SSAO renderer initialized (tile=%d, high=%d)", gDeinterleaveTileCount, gDeinterleaveHigh);

    return true;
}

void Shutdown()
{
#define SR(p) if (p) { (p)->Release(); (p) = nullptr; }
    ReleasePipelineTextures();
    SR(gBlueNoiseSRV); SR(gBlueNoiseTex);
    SR(gFullscreenVS);
    SR(gDeinterleavePS); SR(gOcclusionPS); SR(gReinterleavePS);
    SR(gFilter1PS);      SR(gFilter2PS);
    SR(gResolvePS);      SR(gSSAODebugPS);
    SR(gNoBlend); SR(gNoDepthDSS); SR(gNoCullRS); SR(gPointClampSampler);
    SR(gPassCB);
#undef SR
    gInitialized = false;
    gFrameIndex = 0;
    gHost = nullptr;
    Log("SSAO renderer shut down");
}

void OnResolutionChanged(ID3D11Device* device, UINT newW, UINT newH)
{
    if (newW == gWidth && newH == gHeight) return;
    Log("SSAO resolution change: %ux%u -> %ux%u", gWidth, gHeight, newW, newH);
    ReleasePipelineTextures();
    gWidth = newW;
    gHeight = newH;
    gDeinterleaveTileCount = ((newW / 4) * 4 == newW) ? 4 : 5;
    gDeinterleaveHigh      = (gDeinterleaveTileCount == 5) ? 1 : 0;
    if (!CreatePipelineTextures(device, newW, newH))
    {
        Log("WARNING: failed to recreate AO textures after resolution change");
        gInitialized = false;
    }
}

bool IsInitialized() { return gInitialized; }
ID3D11ShaderResourceView* GetAoSRV() { return gAoSRV; }
float GetLastGpuTimeMs() { return 0.0f; }

ID3D11ShaderResourceView* RenderAO(ID3D11DeviceContext* ctx,
                                    ID3D11ShaderResourceView* depthSRV,
                                    const DustCameraData* camera)
{
    if (!gInitialized || !ctx || !depthSRV || !gHost)
        return nullptr;

    gLastDepthSRV = depthSRV;

    // Defensive resolution check every 300 frames
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
                    if (device) { OnResolutionChanged(device, desc.Width, desc.Height); device->Release(); }
                }
                tex->Release();
            }
            res->Release();
        }
    }

    // First frame: clear so the persisted-state textures start with no AO.
    if (gFrameIndex == 0)
        ClearPipelineTextures(ctx);

    gFrameIndex++;

    gHost->SaveState(ctx);

    // Pack CB
    {
        AOPassCB cb = {};
        cb.bufferPixelSize[0]  = 1.0f / (float)gWidth;
        cb.bufferPixelSize[1]  = 1.0f / (float)gHeight;
        cb.bufferScreenSize[0] = (float)gWidth;
        cb.bufferScreenSize[1] = (float)gHeight;
        cb.bufferAspectRatio[0] = 1.0f;
        cb.bufferAspectRatio[1] = (float)gWidth / (float)gHeight;
        cb.tanHalfFov          = gSSAOConfig.tanHalfFov;
        cb.farPlane            = gSSAOConfig.farPlane;
        cb.sampleRadius        = gSSAOConfig.sampleRadius;
        cb.ssaoAmount          = gSSAOConfig.ssaoAmount;
        cb.fadeDepth           = gSSAOConfig.fadeDepth;
        cb.worldspaceEnable    = gSSAOConfig.worldspaceEnable ? 1.0f : 0.0f;
        cb.sampleQualityPreset = gSSAOConfig.sampleQualityPreset;
        cb.shadingRate         = gSSAOConfig.shadingRate;
        cb.filterSize          = gSSAOConfig.filterSize;
        cb.aoType              = gSSAOConfig.aoType;
        cb.deinterleaveTileCount = gDeinterleaveTileCount;
        cb.deinterleaveHigh    = gDeinterleaveHigh;
        cb.frameCount          = (uint32_t)gFrameIndex;
        cb.debugView           = (float)gSSAOConfig.debugViewMode;
        gHost->UpdateConstantBuffer(ctx, gPassCB, &cb, sizeof(cb));
    }

    // Common state
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(gFullscreenVS, nullptr, 0);
    ctx->RSSetState(gNoCullRS);
    ctx->OMSetDepthStencilState(gNoDepthDSS, 0);
    float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
    ctx->PSSetSamplers(0, 1, &gPointClampSampler);
    ctx->PSSetConstantBuffers(0, 1, &gPassCB);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)gWidth;
    vp.Height = (float)gHeight;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };

    // Pass 1: deinterleave depth -> ZSrc
    {
        ctx->OMSetRenderTargets(1, &gZSrcRTV, nullptr);
        ctx->PSSetShader(gDeinterleavePS, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &depthSRV);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, nullSRVs);
    }

    // Pass 2: occlusion -> OccRaw (discards skipped tiles, preserves prev frame)
    // t0=ZSrc (deinterleaved depth for AO sampling), t1=screen-layout depth
    // (for tile-coherent normals via get_normals), t2=blue noise tile.
    {
        ctx->OMSetRenderTargets(1, &gOccRawRTV, nullptr);
        ctx->PSSetShader(gOcclusionPS, nullptr, 0);
        ID3D11ShaderResourceView* occ1In[3] = { gZSrcSRV, depthSRV, gBlueNoiseSRV };
        ctx->PSSetShaderResources(0, 3, occ1In);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 3, nullSRVs);
    }

    // Pass 3: reinterleave -> AoTex1
    {
        ctx->OMSetRenderTargets(1, &gAoTex1RTV, nullptr);
        ctx->PSSetShader(gReinterleavePS, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &gOccRawSRV);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, nullSRVs);
    }

    // Pass 4 (optional): Filter1 -> AoTex2 (only if FilterSize >= 2)
    if (gSSAOConfig.filterSize >= 2)
    {
        ctx->OMSetRenderTargets(1, &gAoTex2RTV, nullptr);
        ctx->PSSetShader(gFilter1PS, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &gAoTex1SRV);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, nullSRVs);
    }

    // Pass 5: Filter2 -> gFilteredAOTex
    {
        ctx->OMSetRenderTargets(1, &gFilteredAORTV, nullptr);
        ctx->PSSetShader(gFilter2PS, nullptr, 0);
        ID3D11ShaderResourceView* filtIn[2] = { gAoTex1SRV, gAoTex2SRV };
        ctx->PSSetShaderResources(0, 2, filtIn);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 2, nullSRVs);
    }

    // Pass 6: Resolve RGBA16F → gAoTex (R8) for the deferred shader's slot 8.
    {
        ctx->OMSetRenderTargets(1, &gAoRTV, nullptr);
        ctx->PSSetShader(gResolvePS, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &gFilteredAOSRV);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, nullSRVs);
    }

    gHost->RestoreState(ctx);

    return gAoSRV;
}

void RenderDebugOverlay(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* hdrRTV,
                        ID3D11ShaderResourceView* depthSRV)
{
    if (!gInitialized || !ctx || !hdrRTV || gSSAOConfig.debugViewMode == 0 || !gHost)
        return;

    // Use the same depthSRV that RenderAO used so debug views see identical
    // depth values even if the game modified the depth buffer between passes.
    if (gLastDepthSRV)
        depthSRV = gLastDepthSRV;

    gHost->SaveState(ctx);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(gFullscreenVS, nullptr, 0);
    ctx->RSSetState(gNoCullRS);
    ctx->OMSetDepthStencilState(gNoDepthDSS, 0);
    ctx->PSSetConstantBuffers(0, 1, &gPassCB);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)gWidth;
    vp.Height = (float)gHeight;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetRenderTargets(1, &hdrRTV, nullptr);
    ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);
    ctx->PSSetShader(gSSAODebugPS, nullptr, 0);

    // Bind intermediates the debug shader might want to visualize.
    ID3D11ShaderResourceView* srvs[4] = {
        depthSRV,           // t0: raw scene depth
        gFilteredAOSRV,     // t1: filtered AO
        nullptr,            // t2: (unused)
        gAoSRV              // t3: final R8 AO
    };
    ctx->PSSetShaderResources(0, 4, srvs);

    ctx->PSSetSamplers(0, 1, &gPointClampSampler);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
    ctx->PSSetShaderResources(0, 4, nullSRVs);

    gHost->RestoreState(ctx);
}

} // namespace SSAORenderer
