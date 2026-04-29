#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "DebandConfig.h"

#include <d3d11.h>
#include <cstring>
#include <string>

DustLogFn gLogFn = nullptr;
DebandConfig gDebandConfig;

static HMODULE gPluginModule = nullptr;
static const DustHostAPI* gHost = nullptr;
static bool gInitialized = false;

static ID3D11PixelShader* gPS = nullptr;
static ID3D11Buffer* gCB = nullptr;
static ID3D11SamplerState* gSampler = nullptr;
static ID3D11BlendState* gNoBlend = nullptr;
static ID3D11DepthStencilState* gNoDepth = nullptr;
static ID3D11RasterizerState* gNoCull = nullptr;
static UINT gWidth = 0, gHeight = 0;

struct DebandCBData
{
    float    viewportSize[2];
    float    invViewportSize[2];
    float    threshold;
    float    range;
    float    intensity;
    float    skyDepthThreshold;
    uint32_t frameIndex;
    int      debugView;
    int      skyOnly;
    float    _pad;
};

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

static int DebandInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gHost = host;
    gWidth = width;
    gHeight = height;

    std::string shaderDir = GetPluginDir() + "\\shaders\\";

    ID3DBlob* blob = host->CompileShaderFromFile((shaderDir + "deband_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!blob) { Log("Deband: Failed to compile shader"); return -1; }
    HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &gPS);
    blob->Release();
    if (FAILED(hr)) return -1;

    gCB = host->CreateConstantBuffer(device, sizeof(DebandCBData));
    if (!gCB) return -1;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sd, &gSampler);

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &gNoBlend);

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    device->CreateDepthStencilState(&dsd, &gNoDepth);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    device->CreateRasterizerState(&rd, &gNoCull);

    gInitialized = true;
    Log("Deband: Initialized (%ux%u)", width, height);
    return 0;
}

static void DebandShutdown()
{
    if (gPS) { gPS->Release(); gPS = nullptr; }
    if (gCB) { gCB->Release(); gCB = nullptr; }
    if (gSampler) { gSampler->Release(); gSampler = nullptr; }
    if (gNoBlend) { gNoBlend->Release(); gNoBlend = nullptr; }
    if (gNoDepth) { gNoDepth->Release(); gNoDepth = nullptr; }
    if (gNoCull) { gNoCull->Release(); gNoCull = nullptr; }
    gInitialized = false;
    Log("Deband: Shut down");
}

static void DebandOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    gWidth = w;
    gHeight = h;
    Log("Deband: Resolution changed to %ux%u", w, h);
}

static void DebandPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gInitialized || !gDebandConfig.enabled)
        return;

    ID3D11ShaderResourceView* sceneCopy = host->GetSceneCopy(ctx->context, DUST_RESOURCE_LDR_RT);
    if (!sceneCopy) return;

    ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
    if (!depthSRV) return;

    ID3D11RenderTargetView* ldrRTV = host->GetRTV(DUST_RESOURCE_LDR_RT);
    if (!ldrRTV) return;

    host->SaveState(ctx->context);

    DebandCBData cb = {};
    cb.viewportSize[0] = (float)gWidth;
    cb.viewportSize[1] = (float)gHeight;
    cb.invViewportSize[0] = 1.0f / (float)gWidth;
    cb.invViewportSize[1] = 1.0f / (float)gHeight;
    cb.threshold = gDebandConfig.threshold;
    cb.range = gDebandConfig.range;
    cb.intensity = gDebandConfig.intensity;
    cb.skyDepthThreshold = gDebandConfig.skyDepthThreshold;
    cb.frameIndex = (uint32_t)(ctx->frameIndex & 0xFFFFFFFF);
    cb.debugView = gDebandConfig.debugView ? 1 : 0;
    cb.skyOnly = gDebandConfig.skyOnly ? 1 : 0;
    host->UpdateConstantBuffer(ctx->context, gCB, &cb, sizeof(cb));

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)gWidth;
    vp.Height = (float)gHeight;
    vp.MaxDepth = 1.0f;
    ctx->context->RSSetViewports(1, &vp);
    ctx->context->RSSetState(gNoCull);
    ctx->context->OMSetRenderTargets(1, &ldrRTV, nullptr);
    ctx->context->OMSetBlendState(gNoBlend, nullptr, 0xFFFFFFFF);
    ctx->context->OMSetDepthStencilState(gNoDepth, 0);

    ID3D11ShaderResourceView* srvs[2] = { sceneCopy, depthSRV };
    ctx->context->PSSetShaderResources(0, 2, srvs);
    ctx->context->PSSetSamplers(0, 1, &gSampler);
    ctx->context->PSSetConstantBuffers(0, 1, &gCB);
    host->DrawFullscreenTriangle(ctx->context, gPS);

    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    ctx->context->PSSetShaderResources(0, 2, nullSRVs);

    host->RestoreState(ctx->context);
}

static int DebandIsEnabled()
{
    return gDebandConfig.enabled ? 1 : 0;
}

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",        DUST_SETTING_BOOL,  &gDebandConfig.enabled,          0.0f,  1.0f,  "Enabled",          nullptr, "Enable or disable debanding",                       DUST_PERF_LOW  },
    { "Threshold",      DUST_SETTING_FLOAT, &gDebandConfig.threshold,        0.001f,0.1f,  "Threshold",        nullptr, "Maximum color difference to treat as banding",      DUST_PERF_NONE },
    { "Range",          DUST_SETTING_FLOAT, &gDebandConfig.range,            4.0f,  64.0f, "Range",            nullptr, "Search radius in pixels for averaging",             DUST_PERF_NONE },
    { "Intensity",      DUST_SETTING_FLOAT, &gDebandConfig.intensity,        0.0f,  1.0f,  "Intensity",        nullptr, "Blend strength of the debanded result",             DUST_PERF_NONE },
    { "Sky Only",       DUST_SETTING_BOOL,  &gDebandConfig.skyOnly,          0.0f,  1.0f,  "SkyOnly",          nullptr, "Only apply debanding to sky pixels",                DUST_PERF_NONE },
    { "Sky Threshold",  DUST_SETTING_FLOAT, &gDebandConfig.skyDepthThreshold,0.9f,  1.0f,  "SkyDepthThreshold",nullptr, "Depth value above which pixels are treated as sky", DUST_PERF_NONE },
    { "Debug View",     DUST_SETTING_BOOL,  &gDebandConfig.debugView,        0.0f,  1.0f,  "DebugView",        nullptr, "Highlight regions affected by debanding",           DUST_PERF_NONE },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;
    memset(desc, 0, sizeof(*desc));

    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "Deband";
    desc->injectionPoint    = DUST_INJECT_POST_TONEMAP;
    desc->Init              = DebandInit;
    desc->Shutdown          = DebandShutdown;
    desc->OnResolutionChanged = DebandOnResolutionChanged;
    desc->postExecute       = DebandPostExecute;
    desc->IsEnabled         = DebandIsEnabled;
    desc->settings          = gSettingsArray;
    desc->settingCount      = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "Deband";
    desc->priority          = 180;

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
