#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "FilmGrainConfig.h"

#include <d3d11.h>
#include <cstring>
#include <string>

DustLogFn gLogFn = nullptr;
FilmGrainConfig gFilmGrainConfig;

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

struct FilmGrainCBData
{
    float    viewportSize[2];
    float    invViewportSize[2];
    float    intensity;
    float    grainSize;
    uint32_t frameIndex;
    int      colored;
    int      debugView;
    float    _pad[3];
};

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

static int FilmGrainInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gHost = host;
    gWidth = width;
    gHeight = height;

    std::string shaderDir = GetPluginDir() + "\\shaders\\";

    ID3DBlob* blob = host->CompileShaderFromFile((shaderDir + "filmgrain_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!blob) { Log("FilmGrain: Failed to compile shader"); return -1; }
    HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &gPS);
    blob->Release();
    if (FAILED(hr)) return -1;

    gCB = host->CreateConstantBuffer(device, sizeof(FilmGrainCBData));
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
    Log("FilmGrain: Initialized (%ux%u)", width, height);
    return 0;
}

static void FilmGrainShutdown()
{
    if (gPS) { gPS->Release(); gPS = nullptr; }
    if (gCB) { gCB->Release(); gCB = nullptr; }
    if (gSampler) { gSampler->Release(); gSampler = nullptr; }
    if (gNoBlend) { gNoBlend->Release(); gNoBlend = nullptr; }
    if (gNoDepth) { gNoDepth->Release(); gNoDepth = nullptr; }
    if (gNoCull) { gNoCull->Release(); gNoCull = nullptr; }
    gInitialized = false;
    Log("FilmGrain: Shut down");
}

static void FilmGrainOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    gWidth = w;
    gHeight = h;
    Log("FilmGrain: Resolution changed to %ux%u", w, h);
}

static void FilmGrainPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gInitialized || !gFilmGrainConfig.enabled)
        return;

    ID3D11ShaderResourceView* sceneCopy = host->GetSceneCopy(ctx->context, DUST_RESOURCE_LDR_RT);
    if (!sceneCopy) return;

    ID3D11RenderTargetView* ldrRTV = host->GetRTV(DUST_RESOURCE_LDR_RT);
    if (!ldrRTV) return;

    host->SaveState(ctx->context);

    FilmGrainCBData cb = {};
    cb.viewportSize[0] = (float)gWidth;
    cb.viewportSize[1] = (float)gHeight;
    cb.invViewportSize[0] = 1.0f / (float)gWidth;
    cb.invViewportSize[1] = 1.0f / (float)gHeight;
    cb.intensity = gFilmGrainConfig.intensity;
    cb.grainSize = gFilmGrainConfig.size;
    cb.frameIndex = (uint32_t)(ctx->frameIndex & 0xFFFFFFFF);
    cb.colored = gFilmGrainConfig.colored ? 1 : 0;
    cb.debugView = gFilmGrainConfig.debugView ? 1 : 0;
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
    ctx->context->PSSetShaderResources(0, 1, &sceneCopy);
    ctx->context->PSSetSamplers(0, 1, &gSampler);
    ctx->context->PSSetConstantBuffers(0, 1, &gCB);
    host->DrawFullscreenTriangle(ctx->context, gPS);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->context->PSSetShaderResources(0, 1, &nullSRV);

    host->RestoreState(ctx->context);
}

static int FilmGrainIsEnabled()
{
    return gFilmGrainConfig.enabled ? 1 : 0;
}

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",    DUST_SETTING_BOOL,  &gFilmGrainConfig.enabled,   0.0f,  1.0f, "Enabled",   nullptr, "Enable or disable the film grain effect" },
    { "Intensity",  DUST_SETTING_FLOAT, &gFilmGrainConfig.intensity, 0.0f,  0.3f, "Intensity", nullptr, "Strength of the grain noise" },
    { "Size",       DUST_SETTING_FLOAT, &gFilmGrainConfig.size,      1.0f,  4.0f, "Size",      nullptr, "Grain particle size in pixels" },
    { "Colored",    DUST_SETTING_BOOL,  &gFilmGrainConfig.colored,   0.0f,  1.0f, "Colored",   nullptr, "Use color noise instead of luminance-only" },
    { "Debug View", DUST_SETTING_BOOL,  &gFilmGrainConfig.debugView, 0.0f,  1.0f, "DebugView", nullptr, "Show raw noise pattern" },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;
    memset(desc, 0, sizeof(*desc));

    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "Film Grain";
    desc->injectionPoint    = DUST_INJECT_POST_TONEMAP;
    desc->Init              = FilmGrainInit;
    desc->Shutdown          = FilmGrainShutdown;
    desc->OnResolutionChanged = FilmGrainOnResolutionChanged;
    desc->postExecute       = FilmGrainPostExecute;
    desc->IsEnabled         = FilmGrainIsEnabled;
    desc->settings          = gSettingsArray;
    desc->settingCount      = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "FilmGrain";
    desc->priority          = 210;

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
