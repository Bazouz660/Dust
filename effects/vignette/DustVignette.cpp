#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "VignetteConfig.h"

#include <d3d11.h>
#include <cstring>
#include <string>

DustLogFn gLogFn = nullptr;
VignetteConfig gVignetteConfig;

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

struct VignetteCBData
{
    float viewportSize[2];
    float invViewportSize[2];
    float strength;
    float radius;
    float softness;
    int   shape;
    float aspectRatio;
    int   debugView;
    float _pad[2];
};

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

static int VignetteInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gHost = host;
    gWidth = width;
    gHeight = height;

    std::string shaderDir = GetPluginDir() + "\\shaders\\";

    ID3DBlob* blob = host->CompileShaderFromFile((shaderDir + "vignette_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!blob) { Log("Vignette: Failed to compile shader"); return -1; }
    HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &gPS);
    blob->Release();
    if (FAILED(hr)) return -1;

    gCB = host->CreateConstantBuffer(device, sizeof(VignetteCBData));
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
    Log("Vignette: Initialized (%ux%u)", width, height);
    return 0;
}

static void VignetteShutdown()
{
    if (gPS) { gPS->Release(); gPS = nullptr; }
    if (gCB) { gCB->Release(); gCB = nullptr; }
    if (gSampler) { gSampler->Release(); gSampler = nullptr; }
    if (gNoBlend) { gNoBlend->Release(); gNoBlend = nullptr; }
    if (gNoDepth) { gNoDepth->Release(); gNoDepth = nullptr; }
    if (gNoCull) { gNoCull->Release(); gNoCull = nullptr; }
    gInitialized = false;
    Log("Vignette: Shut down");
}

static void VignetteOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    gWidth = w;
    gHeight = h;
    Log("Vignette: Resolution changed to %ux%u", w, h);
}

static void VignettePostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gInitialized || !gVignetteConfig.enabled)
        return;

    ID3D11ShaderResourceView* sceneCopy = host->GetSceneCopy(ctx->context, DUST_RESOURCE_LDR_RT);
    if (!sceneCopy) return;

    ID3D11RenderTargetView* ldrRTV = host->GetRTV(DUST_RESOURCE_LDR_RT);
    if (!ldrRTV) return;

    host->SaveState(ctx->context);

    VignetteCBData cb = {};
    cb.viewportSize[0] = (float)gWidth;
    cb.viewportSize[1] = (float)gHeight;
    cb.invViewportSize[0] = 1.0f / (float)gWidth;
    cb.invViewportSize[1] = 1.0f / (float)gHeight;
    cb.strength = gVignetteConfig.strength;
    cb.radius = gVignetteConfig.radius;
    cb.softness = gVignetteConfig.softness;
    cb.shape = gVignetteConfig.shape;
    cb.aspectRatio = gVignetteConfig.aspectRatio;
    cb.debugView = gVignetteConfig.debugView ? 1 : 0;
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

static int VignetteIsEnabled()
{
    return gVignetteConfig.enabled ? 1 : 0;
}

static const char* const gShapeLabels[] = { "Circle", "Rectangle", "Diamond", nullptr };

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",   DUST_SETTING_BOOL,  &gVignetteConfig.enabled,   0.0f, 1.0f, "Enabled",   nullptr,       "Enable or disable the vignette effect",                    DUST_PERF_NONE },
    { "Strength",  DUST_SETTING_FLOAT, &gVignetteConfig.strength,  0.0f, 1.0f, "Strength",  nullptr,       "How much the edges darken",                                DUST_PERF_NONE },
    { "Radius",    DUST_SETTING_FLOAT, &gVignetteConfig.radius,    0.1f, 2.0f, "Radius",    nullptr,       "Distance from center where darkening begins",              DUST_PERF_NONE },
    { "Softness",  DUST_SETTING_FLOAT, &gVignetteConfig.softness,  0.01f,2.0f, "Softness",  nullptr,       "How gradual the transition from lit to dark",              DUST_PERF_NONE },
    { "Shape",        DUST_SETTING_ENUM,  &gVignetteConfig.shape,       0.0f, 2.0f, "Shape",       gShapeLabels,  "Vignette shape",                                       DUST_PERF_NONE },
    { "Aspect Ratio", DUST_SETTING_FLOAT, &gVignetteConfig.aspectRatio, 0.5f, 2.0f, "AspectRatio", nullptr,       "Stretch the vignette horizontally (>1) or vertically (<1)", DUST_PERF_NONE },
    { "Debug View",   DUST_SETTING_BOOL,  &gVignetteConfig.debugView,   0.0f, 1.0f, "DebugView",   nullptr,       "Show the vignette mask",                                DUST_PERF_NONE },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;
    memset(desc, 0, sizeof(*desc));

    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "Vignette";
    desc->injectionPoint    = DUST_INJECT_POST_TONEMAP;
    desc->Init              = VignetteInit;
    desc->Shutdown          = VignetteShutdown;
    desc->OnResolutionChanged = VignetteOnResolutionChanged;
    desc->postExecute       = VignettePostExecute;
    desc->IsEnabled         = VignetteIsEnabled;
    desc->settings          = gSettingsArray;
    desc->settingCount      = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "Vignette";
    desc->priority          = 200;

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
