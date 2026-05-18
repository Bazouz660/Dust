#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "SMAAAreaTex.h"
#include "SMAASearchTex.h"

#include <d3d11.h>
#include <cstring>
#include <cmath>
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

struct SMAAConfig {
    bool  enabled           = true;
    int   edgeMode          = 0;
    float lumaThreshold     = 0.1f;
    float depthThreshold    = 0.01f;
    bool  showEdges         = false;
    bool  showWeights       = false;
    bool  temporalEnabled   = true;
    float temporalStrength  = 0.5f;
    bool  showMotionVectors = false;
    bool  showTestPattern  = false;
};

static SMAAConfig gConfig;

static ID3D11PixelShader* gEdgeDetectPS   = nullptr;
static ID3D11PixelShader* gBlendWeightPS  = nullptr;
static ID3D11PixelShader* gResolvePS      = nullptr;
static ID3D11PixelShader* gTemporalPS     = nullptr;
static ID3D11PixelShader* gTestPatternPS  = nullptr;

static ID3D11Buffer*             gCB            = nullptr;
static ID3D11SamplerState*       gPointSampler  = nullptr;
static ID3D11SamplerState*       gLinearSampler = nullptr;
static ID3D11BlendState*         gNoBlend       = nullptr;
static ID3D11DepthStencilState*  gNoDepth       = nullptr;
static ID3D11RasterizerState*    gRasterState   = nullptr;

struct SMAATexture {
    ID3D11Texture2D*          tex = nullptr;
    ID3D11RenderTargetView*   rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
};

static SMAATexture gEdgeTex;
static SMAATexture gBlendTex;
static SMAATexture gSMAATex;
static SMAATexture gTestPatternTex;
static SMAATexture gHistory[2];
static int gHistoryWriteIdx = 0;
static uint32_t gWidth = 0, gHeight = 0;

static float gPrevInvView[16] = {};
static bool  gPrevCamValid     = false;
static int   gFrameCounter     = 0;

static ID3D11Texture2D*          gAreaTexture  = nullptr;
static ID3D11ShaderResourceView* gAreaSRV      = nullptr;
static ID3D11Texture2D*          gSearchTexture = nullptr;
static ID3D11ShaderResourceView* gSearchSRV    = nullptr;

struct SMAACB {
    float invWidth, invHeight, width, height;
    float lumaThreshold;
    float depthThreshold;
    int   edgeMode;
    int   temporalEnabled;
    float tanHalfFov;
    float aspectRatio;
    float temporalStrength;
    float reprojDepthScale;
    float reprojDepthOffset;
    int   debugMotionVectors;
    float jitterX, jitterY;
    float reprojRow0[4];
    float reprojRow1[4];
    float reprojRow2[4];
    float reprojRow3[4];
    float invViewRow0[4];
    float invViewRow1[4];
    float invViewRow2[4];
    float invViewRow3[4];
};

static void ReleaseTexture(SMAATexture& t)
{
    if (t.srv) { t.srv->Release(); t.srv = nullptr; }
    if (t.rtv) { t.rtv->Release(); t.rtv = nullptr; }
    if (t.tex) { t.tex->Release(); t.tex = nullptr; }
}

static bool CreateTexture(ID3D11Device* dev, uint32_t w, uint32_t h,
                           DXGI_FORMAT fmt, SMAATexture& out, const char* name)
{
    ReleaseTexture(out);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = w;
    desc.Height           = h;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = fmt;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(dev->CreateTexture2D(&desc, nullptr, &out.tex)))
    { Log("SMAA: Failed to create %s texture", name); return false; }
    if (FAILED(dev->CreateRenderTargetView(out.tex, nullptr, &out.rtv)))
    { Log("SMAA: Failed to create %s RTV", name); return false; }
    if (FAILED(dev->CreateShaderResourceView(out.tex, nullptr, &out.srv)))
    { Log("SMAA: Failed to create %s SRV", name); return false; }

    return true;
}

static bool CreateTextures(ID3D11Device* dev, uint32_t w, uint32_t h)
{
    gWidth = w;
    gHeight = h;
    if (!CreateTexture(dev, w, h, DXGI_FORMAT_R8G8_UNORM, gEdgeTex, "edge"))
        return false;
    if (!CreateTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, gBlendTex, "blend"))
        return false;
    if (!CreateTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, gSMAATex, "smaa_resolve"))
        return false;
    if (!CreateTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, gTestPatternTex, "test_pattern"))
        return false;
    if (!CreateTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, gHistory[0], "history0"))
        return false;
    if (!CreateTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, gHistory[1], "history1"))
        return false;
    gHistoryWriteIdx = 0;
    gPrevCamValid = false;
    return true;
}

static std::string gShaderDir;

static ID3D11PixelShader* CompilePS(const char* filename, const char* label)
{
    std::string path = gShaderDir + filename;
    ID3DBlob* blob = gHost->CompileShaderFromFile(path.c_str(), "main", "ps_5_0");
    if (!blob) { Log("SMAA: Failed to compile %s from %s", label, path.c_str()); return nullptr; }

    ID3D11PixelShader* ps = nullptr;
    HRESULT hr = gDevice->CreatePixelShader(blob->GetBufferPointer(),
                                             blob->GetBufferSize(), nullptr, &ps);
    blob->Release();
    if (FAILED(hr)) { Log("SMAA: Failed to create %s PS: 0x%08X", label, hr); return nullptr; }
    return ps;
}

static void MatIdentity(float* m)
{
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void Mat4Mul(float* out, const float* A, const float* B)
{
    float tmp[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
        {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += A[r * 4 + k] * B[k * 4 + c];
            tmp[r * 4 + c] = s;
        }
    memcpy(out, tmp, 64);
}

// Build view matrix (row-major float[16]) from raw column-major inverse view
// bytes. View space: X right, Y down, Z into scene.
// Raw cols: (camRight, camUp, camForward=behind camera, camPos).
static void NormalizeBasis(float& x, float& y, float& z)
{
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 1e-8f) { float inv = 1.0f / len; x *= inv; y *= inv; z *= inv; }
}

static void BuildViewFromRaw(float* V, const float* m)
{
    float Rx = m[0],  Ry = m[1],  Rz = m[2];
    float Ux = m[4],  Uy = m[5],  Uz = m[6];
    float Fx = m[8],  Fy = m[9],  Fz = m[10];
    float Px = m[12], Py = m[13], Pz = m[14];
    NormalizeBasis(Rx, Ry, Rz);
    NormalizeBasis(Ux, Uy, Uz);
    NormalizeBasis(Fx, Fy, Fz);

    V[0]  =  Rx; V[1]  =  Ry; V[2]  =  Rz; V[3]  = -(Rx*Px + Ry*Py + Rz*Pz);
    V[4]  = -Ux; V[5]  = -Uy; V[6]  = -Uz; V[7]  =  (Ux*Px + Uy*Py + Uz*Pz);
    V[8]  = -Fx; V[9]  = -Fy; V[10] = -Fz; V[11] =  (Fx*Px + Fy*Py + Fz*Pz);
    V[12] =  0;  V[13] =  0;  V[14] =  0;  V[15] =  1;
}

static void BuildInvViewFromRaw(float* I, const float* m)
{
    float Rx = m[0],  Ry = m[1],  Rz = m[2];
    float Ux = m[4],  Uy = m[5],  Uz = m[6];
    float Fx = m[8],  Fy = m[9],  Fz = m[10];
    float Px = m[12], Py = m[13], Pz = m[14];
    NormalizeBasis(Rx, Ry, Rz);
    NormalizeBasis(Ux, Uy, Uz);
    NormalizeBasis(Fx, Fy, Fz);

    I[0]  =  Rx; I[1]  = -Ux; I[2]  = -Fx; I[3]  = Px;
    I[4]  =  Ry; I[5]  = -Uy; I[6]  = -Fy; I[7]  = Py;
    I[8]  =  Rz; I[9]  = -Uz; I[10] = -Fz; I[11] = Pz;
    I[12] =  0;  I[13] =  0;  I[14] =  0;  I[15] =  1;
}

static int SMAAInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
    gHost = host;
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gDevice = device;
    gShaderDir = GetPluginDir() + "\\shaders\\";

    gEdgeDetectPS  = CompilePS("smaa_edge_detect_ps.hlsl",   "edge detect");
    gBlendWeightPS = CompilePS("smaa_blend_weight_ps.hlsl",  "blend weight");
    gResolvePS     = CompilePS("smaa_resolve_ps.hlsl",       "resolve");
    gTemporalPS    = CompilePS("smaa_temporal_ps.hlsl",      "temporal");
    gTestPatternPS = CompilePS("smaa_test_pattern_ps.hlsl",  "test pattern");
    if (!gEdgeDetectPS || !gBlendWeightPS || !gResolvePS || !gTemporalPS) return -1;

    gCB = host->CreateConstantBuffer(device, sizeof(SMAACB));
    if (!gCB) return -2;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &gPointSampler))) return -3;

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(device->CreateSamplerState(&sd, &gLinearSampler))) return -4;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &gNoBlend))) return -5;

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&dsd, &gNoDepth))) return -6;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    if (FAILED(device->CreateRasterizerState(&rd, &gRasterState))) return -7;

    if (!CreateTextures(device, width, height)) return -8;

    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width            = AREATEX_WIDTH;
        td.Height           = AREATEX_HEIGHT;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_R8G8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_IMMUTABLE;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem    = areaTexBytes;
        init.SysMemPitch = AREATEX_PITCH;

        if (FAILED(device->CreateTexture2D(&td, &init, &gAreaTexture)))
        { Log("SMAA: Failed to create area texture"); return -9; }
        if (FAILED(device->CreateShaderResourceView(gAreaTexture, nullptr, &gAreaSRV)))
        { Log("SMAA: Failed to create area SRV"); return -10; }
    }
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width            = SEARCHTEX_WIDTH;
        td.Height           = SEARCHTEX_HEIGHT;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_IMMUTABLE;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem    = searchTexBytes;
        init.SysMemPitch = SEARCHTEX_PITCH;

        if (FAILED(device->CreateTexture2D(&td, &init, &gSearchTexture)))
        { Log("SMAA: Failed to create search texture"); return -11; }
        if (FAILED(device->CreateShaderResourceView(gSearchTexture, nullptr, &gSearchSRV)))
        { Log("SMAA: Failed to create search SRV"); return -12; }
    }

    Log("SMAA: Initialized (%ux%u)", width, height);
    return 0;
}

static void SMAAShutdown()
{
    ReleaseTexture(gEdgeTex);
    ReleaseTexture(gBlendTex);
    ReleaseTexture(gSMAATex);
    ReleaseTexture(gTestPatternTex);
    ReleaseTexture(gHistory[0]);
    ReleaseTexture(gHistory[1]);
    if (gSearchSRV)     { gSearchSRV->Release();      gSearchSRV = nullptr; }
    if (gSearchTexture) { gSearchTexture->Release();   gSearchTexture = nullptr; }
    if (gAreaSRV)       { gAreaSRV->Release();         gAreaSRV = nullptr; }
    if (gAreaTexture)   { gAreaTexture->Release();     gAreaTexture = nullptr; }
    if (gRasterState)   { gRasterState->Release();     gRasterState = nullptr; }
    if (gNoDepth)       { gNoDepth->Release();         gNoDepth = nullptr; }
    if (gNoBlend)       { gNoBlend->Release();         gNoBlend = nullptr; }
    if (gLinearSampler) { gLinearSampler->Release();   gLinearSampler = nullptr; }
    if (gPointSampler)  { gPointSampler->Release();    gPointSampler = nullptr; }
    if (gCB)            { gCB->Release();              gCB = nullptr; }
    if (gTestPatternPS) { gTestPatternPS->Release();    gTestPatternPS = nullptr; }
    if (gTemporalPS)    { gTemporalPS->Release();      gTemporalPS = nullptr; }
    if (gResolvePS)     { gResolvePS->Release();       gResolvePS = nullptr; }
    if (gBlendWeightPS) { gBlendWeightPS->Release();   gBlendWeightPS = nullptr; }
    if (gEdgeDetectPS)  { gEdgeDetectPS->Release();    gEdgeDetectPS = nullptr; }
    gPrevCamValid = false;
    gDevice = nullptr;
    Log("SMAA: Shut down");
}

static void SMAAOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    CreateTextures(device, w, h);
    Log("SMAA: Resolution changed to %ux%u", w, h);
}

static void SMAAPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gConfig.enabled) return;

    ID3D11RenderTargetView* ldrRTV = host->GetRTV(DUST_RESOURCE_LDR_RT);
    if (!ldrRTV) return;

    ID3D11ShaderResourceView* sceneCopy = host->GetSceneCopy(ctx->context, DUST_RESOURCE_LDR_RT);
    if (!sceneCopy) return;

    ID3D11DeviceContext* dc = ctx->context;
    host->SaveState(dc);

    dc->OMSetDepthStencilState(gNoDepth, 0);
    dc->RSSetState(gRasterState);
    dc->OMSetBlendState(gNoBlend, nullptr, 0xFFFFFFFF);
    dc->PSSetConstantBuffers(0, 1, &gCB);

    bool doTemporal = gConfig.temporalEnabled && ctx->camera.valid;

    SMAACB cb = {};
    cb.invWidth       = 1.0f / (float)ctx->width;
    cb.invHeight      = 1.0f / (float)ctx->height;
    cb.width          = (float)ctx->width;
    cb.height         = (float)ctx->height;
    cb.lumaThreshold  = gConfig.lumaThreshold;
    cb.depthThreshold = gConfig.depthThreshold;
    cb.edgeMode       = gConfig.edgeMode;
    cb.temporalEnabled   = doTemporal ? 1 : 0;
    cb.tanHalfFov        = 0.4142f;
    cb.aspectRatio       = (float)ctx->width / (float)ctx->height;
    cb.reprojDepthScale  = 1000.0f;
    cb.reprojDepthOffset = 1.0f;

    if (doTemporal)
    {
        cb.debugMotionVectors = gConfig.showMotionVectors ? 1 : 0;

        if (gPrevCamValid)
        {
            cb.temporalStrength = gConfig.temporalStrength;
            float prevView[16], currInvView[16], mat[16];
            BuildViewFromRaw(prevView, gPrevInvView);
            BuildInvViewFromRaw(currInvView, ctx->camera.inverseView);
            Mat4Mul(mat, prevView, currInvView);
            memcpy(cb.reprojRow0, &mat[0],  16);
            memcpy(cb.reprojRow1, &mat[4],  16);
            memcpy(cb.reprojRow2, &mat[8],  16);
            memcpy(cb.reprojRow3, &mat[12], 16);

            if (gFrameCounter % 60 == 0)
            {
                bool identical = memcmp(gPrevInvView, ctx->camera.inverseView, sizeof(gPrevInvView)) == 0;
                Log("[SMAA-T] frame=%d cam_identical=%d diag=(%.6f,%.6f,%.6f) trans=(%.6f,%.6f,%.6f)",
                    gFrameCounter, identical ? 1 : 0,
                    mat[0], mat[5], mat[10],
                    mat[3], mat[7], mat[11]);
            }
        }
        else
        {
            cb.temporalStrength = 0.0f;
            float id[16];
            MatIdentity(id);
            memcpy(cb.reprojRow0, &id[0],  16);
            memcpy(cb.reprojRow1, &id[4],  16);
            memcpy(cb.reprojRow2, &id[8],  16);
            memcpy(cb.reprojRow3, &id[12], 16);
        }
    }

    // Fill inverse view matrix for world-space test pattern
    {
        float invView[16];
        BuildInvViewFromRaw(invView, ctx->camera.inverseView);
        memcpy(cb.invViewRow0, &invView[0],  16);
        memcpy(cb.invViewRow1, &invView[4],  16);
        memcpy(cb.invViewRow2, &invView[8],  16);
        memcpy(cb.invViewRow3, &invView[12], 16);
    }

    host->UpdateConstantBuffer(dc, gCB, &cb, sizeof(cb));

    D3D11_VIEWPORT vp = { 0, 0, (float)ctx->width, (float)ctx->height, 0, 1 };
    dc->RSSetViewports(1, &vp);

    ID3D11ShaderResourceView* nullSRV = nullptr;

    // --- Test pattern (replaces scene input when enabled) ---
    ID3D11ShaderResourceView* sceneInput = sceneCopy;
    if (gConfig.showTestPattern && gTestPatternPS)
    {
        dc->OMSetRenderTargets(1, &gTestPatternTex.rtv, nullptr);
        ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
        dc->PSSetShaderResources(0, 1, &depthSRV);
        dc->PSSetSamplers(0, 1, &gPointSampler);
        host->DrawFullscreenTriangle(dc, gTestPatternPS);
        dc->PSSetShaderResources(0, 1, &nullSRV);
        sceneInput = gTestPatternTex.srv;
    }

    // --- Pass 1: Edge detection ---
    {
        float clearColor[4] = { 0, 0, 0, 0 };
        dc->ClearRenderTargetView(gEdgeTex.rtv, clearColor);
        dc->OMSetRenderTargets(1, &gEdgeTex.rtv, nullptr);

        dc->PSSetShaderResources(0, 1, &sceneInput);
        ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
        dc->PSSetShaderResources(1, 1, &depthSRV);
        dc->PSSetSamplers(0, 1, &gPointSampler);

        host->DrawFullscreenTriangle(dc, gEdgeDetectPS);

        dc->PSSetShaderResources(0, 1, &nullSRV);
        dc->PSSetShaderResources(1, 1, &nullSRV);
    }

    if (gConfig.showEdges)
    {
        dc->OMSetRenderTargets(1, &ldrRTV, nullptr);
        dc->PSSetShaderResources(0, 1, &gEdgeTex.srv);
        dc->PSSetShaderResources(1, 1, &nullSRV);
        ID3D11SamplerState* samplers[2] = { gLinearSampler, gPointSampler };
        dc->PSSetSamplers(0, 2, samplers);
        host->DrawFullscreenTriangle(dc, gResolvePS);
        dc->PSSetShaderResources(0, 1, &nullSRV);
        host->RestoreState(dc);
        return;
    }

    // --- Pass 2: Blend weight calculation ---
    {
        float clearColor[4] = { 0, 0, 0, 0 };
        dc->ClearRenderTargetView(gBlendTex.rtv, clearColor);
        dc->OMSetRenderTargets(1, &gBlendTex.rtv, nullptr);

        dc->PSSetShaderResources(0, 1, &gEdgeTex.srv);
        dc->PSSetShaderResources(1, 1, &gAreaSRV);
        dc->PSSetShaderResources(2, 1, &gSearchSRV);
        ID3D11SamplerState* bwSamplers[2] = { gPointSampler, gLinearSampler };
        dc->PSSetSamplers(0, 2, bwSamplers);

        host->DrawFullscreenTriangle(dc, gBlendWeightPS);

        dc->PSSetShaderResources(0, 1, &nullSRV);
        dc->PSSetShaderResources(1, 1, &nullSRV);
        dc->PSSetShaderResources(2, 1, &nullSRV);
    }

    if (gConfig.showWeights)
    {
        dc->OMSetRenderTargets(1, &ldrRTV, nullptr);
        dc->PSSetShaderResources(0, 1, &gBlendTex.srv);
        dc->PSSetShaderResources(1, 1, &nullSRV);
        ID3D11SamplerState* samplers[2] = { gLinearSampler, gPointSampler };
        dc->PSSetSamplers(0, 2, samplers);
        host->DrawFullscreenTriangle(dc, gResolvePS);
        dc->PSSetShaderResources(0, 1, &nullSRV);
        host->RestoreState(dc);
        return;
    }

    // --- Pass 3: Neighborhood blending ---
    {
        ID3D11RenderTargetView* resolveTarget = doTemporal ? gSMAATex.rtv : ldrRTV;
        dc->OMSetRenderTargets(1, &resolveTarget, nullptr);

        dc->PSSetShaderResources(0, 1, &sceneInput);
        dc->PSSetShaderResources(1, 1, &gBlendTex.srv);
        ID3D11SamplerState* samplers[2] = { gLinearSampler, gPointSampler };
        dc->PSSetSamplers(0, 2, samplers);

        host->DrawFullscreenTriangle(dc, gResolvePS);

        dc->PSSetShaderResources(0, 1, &nullSRV);
        dc->PSSetShaderResources(1, 1, &nullSRV);
    }

    // --- Pass 4: Temporal resolve ---
    if (doTemporal)
    {
        int readIdx  = 1 - gHistoryWriteIdx;
        int writeIdx = gHistoryWriteIdx;

        ID3D11RenderTargetView* rtvs[2] = { ldrRTV, gHistory[writeIdx].rtv };
        dc->OMSetRenderTargets(2, rtvs, nullptr);

        dc->PSSetShaderResources(0, 1, &gSMAATex.srv);
        dc->PSSetShaderResources(1, 1, &gHistory[readIdx].srv);
        ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
        dc->PSSetShaderResources(2, 1, &depthSRV);
        ID3D11SamplerState* tempSamplers[2] = { gPointSampler, gLinearSampler };
        dc->PSSetSamplers(0, 2, tempSamplers);

        host->DrawFullscreenTriangle(dc, gTemporalPS);

        dc->PSSetShaderResources(0, 1, &nullSRV);
        dc->PSSetShaderResources(1, 1, &nullSRV);
        dc->PSSetShaderResources(2, 1, &nullSRV);

        gHistoryWriteIdx = readIdx;
    }

    if (ctx->camera.valid)
    {
        memcpy(gPrevInvView, ctx->camera.inverseView, sizeof(gPrevInvView));
        gPrevCamValid = true;
    }
    gFrameCounter++;

    host->RestoreState(dc);
}

static int SMAAIsEnabled()
{
    return 1;
}

static const char* const gEdgeModeLabels[] = { "Luma", "Depth", "Luma + Depth", nullptr };

static DustSettingDesc gSettings[] = {
    { "Enabled",            DUST_SETTING_BOOL,    &gConfig.enabled,          0.0f,  1.0f, "Enabled",           nullptr,          "Enable or disable anti-aliasing",                                 DUST_PERF_LOW    },
    { "Mode",               DUST_SETTING_ENUM,    &gConfig.edgeMode,         0.0f,  2.0f, "EdgeMode",          gEdgeModeLabels,  "Edge detection method: Luma, Depth, or both",                     DUST_PERF_LOW    },
    { "Luma Threshold",     DUST_SETTING_FLOAT,   &gConfig.lumaThreshold,    0.05f, 0.5f, "LumaThreshold",     nullptr,          "Sensitivity for luma-based edge detection (lower = more edges)",  DUST_PERF_NONE   },
    { "Depth Threshold",    DUST_SETTING_FLOAT,   &gConfig.depthThreshold,   0.001f,0.1f, "DepthThreshold",    nullptr,          "Sensitivity for depth-based edge detection (lower = more edges)", DUST_PERF_NONE   },
    { "Temporal",           DUST_SETTING_SECTION, nullptr,                   0.0f,  0.0f, nullptr,             nullptr,          nullptr,                                                           DUST_PERF_NONE   },
    { "Temporal AA",        DUST_SETTING_BOOL,    &gConfig.temporalEnabled,  0.0f,  1.0f, "TemporalEnabled",   nullptr,          "Blend with reprojected history for sub-pixel AA",                 DUST_PERF_LOW    },
    { "Temporal Strength",  DUST_SETTING_FLOAT,   &gConfig.temporalStrength, 0.0f,  0.98f,"TemporalStrength",  nullptr,          "History blend weight (higher = smoother but more ghosting)",      DUST_PERF_NONE   },
    { "Debug",              DUST_SETTING_SECTION, nullptr,                   0.0f,  0.0f, nullptr,             nullptr,          nullptr,                                                           DUST_PERF_NONE   },
    { "Show Edges",         DUST_SETTING_BOOL,    &gConfig.showEdges,        0.0f,  1.0f, "ShowEdges",         nullptr,          "Visualize detected edges",                                        DUST_PERF_NONE   },
    { "Show Weights",       DUST_SETTING_BOOL,    &gConfig.showWeights,      0.0f,  1.0f, "ShowWeights",       nullptr,          "Visualize blending weights",                                      DUST_PERF_NONE   },
    { "Show Motion Vectors",DUST_SETTING_BOOL,    &gConfig.showMotionVectors,0.0f,  1.0f, "ShowMotionVectors", nullptr,          "Visualize reprojection motion (gray=still, color=motion)",        DUST_PERF_NONE   },
    { "Show Test Pattern",  DUST_SETTING_BOOL,    &gConfig.showTestPattern,  0.0f,  1.0f, "ShowTestPattern",   nullptr,          "Replace scene with aliased test shapes to evaluate AA quality",   DUST_PERF_NONE   },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion          = DUST_API_VERSION;
    desc->name                = "SMAA";
    desc->injectionPoint      = DUST_INJECT_POST_TONEMAP;
    desc->Init                = SMAAInit;
    desc->Shutdown            = SMAAShutdown;
    desc->OnResolutionChanged = SMAAOnResolutionChanged;
    desc->postExecute         = SMAAPostExecute;
    desc->IsEnabled           = SMAAIsEnabled;
    desc->settings            = gSettings;
    desc->settingCount        = sizeof(gSettings) / sizeof(gSettings[0]);

    desc->flags               = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection       = "SMAA";
    desc->priority            = 250;

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
