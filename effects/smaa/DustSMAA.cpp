#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "SMAAAreaTex.h"
#include "SMAASearchTex.h"

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

struct SMAAConfig {
    bool  enabled        = true;
    int   edgeMode       = 0;
    float lumaThreshold  = 0.1f;
    float depthThreshold = 0.01f;
    bool  showEdges      = false;
    bool  showWeights    = false;
};

static SMAAConfig gConfig;

static ID3D11PixelShader* gEdgeDetectPS  = nullptr;
static ID3D11PixelShader* gBlendWeightPS = nullptr;
static ID3D11PixelShader* gResolvePS     = nullptr;

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
static uint32_t gWidth = 0, gHeight = 0;

static ID3D11Texture2D*          gAreaTexture  = nullptr;
static ID3D11ShaderResourceView* gAreaSRV      = nullptr;
static ID3D11Texture2D*          gSearchTexture = nullptr;
static ID3D11ShaderResourceView* gSearchSRV    = nullptr;

struct SMAACB {
    float invWidth, invHeight, width, height;
    float lumaThreshold;
    float depthThreshold;
    int   edgeMode;
    float pad;
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
    if (!gEdgeDetectPS || !gBlendWeightPS || !gResolvePS) return -1;

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
    if (gResolvePS)     { gResolvePS->Release();       gResolvePS = nullptr; }
    if (gBlendWeightPS) { gBlendWeightPS->Release();   gBlendWeightPS = nullptr; }
    if (gEdgeDetectPS)  { gEdgeDetectPS->Release();    gEdgeDetectPS = nullptr; }
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

    SMAACB cb = {};
    cb.invWidth       = 1.0f / (float)ctx->width;
    cb.invHeight      = 1.0f / (float)ctx->height;
    cb.width          = (float)ctx->width;
    cb.height         = (float)ctx->height;
    cb.lumaThreshold  = gConfig.lumaThreshold;
    cb.depthThreshold = gConfig.depthThreshold;
    cb.edgeMode       = gConfig.edgeMode;
    host->UpdateConstantBuffer(dc, gCB, &cb, sizeof(cb));

    D3D11_VIEWPORT vp = { 0, 0, (float)ctx->width, (float)ctx->height, 0, 1 };
    dc->RSSetViewports(1, &vp);

    ID3D11ShaderResourceView* nullSRV = nullptr;

    // --- Pass 1: Edge detection ---
    {
        float clearColor[4] = { 0, 0, 0, 0 };
        dc->ClearRenderTargetView(gEdgeTex.rtv, clearColor);
        dc->OMSetRenderTargets(1, &gEdgeTex.rtv, nullptr);

        dc->PSSetShaderResources(0, 1, &sceneCopy);
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
        dc->OMSetRenderTargets(1, &ldrRTV, nullptr);

        dc->PSSetShaderResources(0, 1, &sceneCopy);
        dc->PSSetShaderResources(1, 1, &gBlendTex.srv);
        ID3D11SamplerState* samplers[2] = { gLinearSampler, gPointSampler };
        dc->PSSetSamplers(0, 2, samplers);

        host->DrawFullscreenTriangle(dc, gResolvePS);

        dc->PSSetShaderResources(0, 1, &nullSRV);
        dc->PSSetShaderResources(1, 1, &nullSRV);
    }

    host->RestoreState(dc);
}

static int SMAAIsEnabled()
{
    return 1;
}

static const char* const gEdgeModeLabels[] = { "Luma", "Depth", "Luma + Depth", nullptr };

static DustSettingDesc gSettings[] = {
    { "Enabled",         DUST_SETTING_BOOL,    &gConfig.enabled,        0.0f,  1.0f, "Enabled",        nullptr,          "Enable or disable anti-aliasing",                                 DUST_PERF_LOW  },
    { "Mode",            DUST_SETTING_ENUM,    &gConfig.edgeMode,       0.0f,  2.0f, "EdgeMode",       gEdgeModeLabels,  "Edge detection method: Luma, Depth, or both",                     DUST_PERF_LOW  },
    { "Luma Threshold",  DUST_SETTING_FLOAT,   &gConfig.lumaThreshold,  0.05f, 0.5f, "LumaThreshold",  nullptr,          "Sensitivity for luma-based edge detection (lower = more edges)",  DUST_PERF_NONE },
    { "Depth Threshold", DUST_SETTING_FLOAT,   &gConfig.depthThreshold, 0.001f,0.1f, "DepthThreshold", nullptr,          "Sensitivity for depth-based edge detection (lower = more edges)", DUST_PERF_NONE },
    { "Show Edges",      DUST_SETTING_BOOL,    &gConfig.showEdges,      0.0f,  1.0f, "ShowEdges",      nullptr,          "Visualize detected edges",                                        DUST_PERF_NONE },
    { "Show Weights",    DUST_SETTING_BOOL,    &gConfig.showWeights,    0.0f,  1.0f, "ShowWeights",    nullptr,          "Visualize blending weights",                                      DUST_PERF_NONE },
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
