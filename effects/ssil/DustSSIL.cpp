// DustSSIL.cpp - Screen-Space Indirect Lighting effect plugin for Dust (API v3)
// Computes indirect light bounce from nearby surfaces and composites additively onto HDR.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "SSILRenderer.h"
#include "SSILConfig.h"

#include <d3d11.h>
#include <cstring>
#include <string>

static const DustHostAPI* gHost = nullptr;

// Log function pointer used by DustLog.h in other translation units
DustLogFn gLogFn = nullptr;

// Global config instance (framework populates via settings array)
SSILConfig gSSILConfig;

static HMODULE gPluginModule = nullptr;
static ID3D11Device* gDevice = nullptr;

// Composite shader + additive blend for applying IL to HDR RT
static ID3D11PixelShader*  gCompositePS = nullptr;
static ID3D11BlendState*   gAdditiveBlend = nullptr;
static ID3D11SamplerState* gLinearSampler = nullptr;

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

// preExecute — nothing to do; IL generation happens in postExecute after lighting
static void SSILPreExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    (void)ctx; (void)host;
}

// Called AFTER the game's lighting draw — generate IL from the lit scene, then composite
static void SSILPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gSSILConfig.enabled || !SSILRenderer::IsInitialized())
        return;

    ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
    if (!depthSRV)
        return;

    // Snapshot the lit HDR scene — this contains actual surface radiance
    // (direct lighting, shadows, AO, specular) not just flat albedo.
    ID3D11ShaderResourceView* sceneSRV = host->GetSceneCopy(ctx->context, DUST_RESOURCE_HDR_RT);
    if (!sceneSRV)
        return;

    ID3D11ShaderResourceView* normalsSRV = host->GetSRV(DUST_RESOURCE_NORMALS);

    // Generate IL texture from the lit scene
    ID3D11ShaderResourceView* ilSRV = SSILRenderer::RenderIL(ctx->context, depthSRV, sceneSRV, normalsSRV, &ctx->camera);
    if (!ilSRV)
        return;

    // Composite IL additively onto HDR RT
    ID3D11RenderTargetView* hdrRTV = host->GetRTV(DUST_RESOURCE_HDR_RT);
    if (!hdrRTV)
        return;

    ID3D11DeviceContext* dc = ctx->context;
    host->SaveState(dc);

    // Set additive blend and draw IL onto HDR
    dc->OMSetRenderTargets(1, &hdrRTV, nullptr);
    float blendFactor[4] = { 0, 0, 0, 0 };
    dc->OMSetBlendState(gAdditiveBlend, blendFactor, 0xFFFFFFFF);
    dc->PSSetShaderResources(0, 1, &ilSRV);
    dc->PSSetSamplers(0, 1, &gLinearSampler);

    D3D11_VIEWPORT vp = { 0, 0, (float)ctx->width, (float)ctx->height, 0, 1 };
    dc->RSSetViewports(1, &vp);

    host->DrawFullscreenTriangle(dc, gCompositePS);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    dc->PSSetShaderResources(0, 1, &nullSRV);

    host->RestoreState(dc);

    // Debug overlay
    if (gSSILConfig.debugView)
    {
        SSILRenderer::RenderDebugOverlay(dc, hdrRTV);
    }
}

static int SSILInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
    gHost = host;
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gDevice = device;

    std::string pluginDir = GetPluginDir();
    std::string shaderDir = pluginDir + "\\shaders\\";

    if (!SSILRenderer::Init(device, width, height, host, pluginDir.c_str()))
        return -1;

    // Compile composite shader
    ID3DBlob* blob = host->CompileShaderFromFile((shaderDir + "ssil_composite_ps.hlsl").c_str(), "main", "ps_5_0");
    if (!blob) return -2;

    HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &gCompositePS);
    blob->Release();
    if (FAILED(hr)) return -3;

    // Additive blend state (ONE + ONE)
    {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&bd, &gAdditiveBlend);
        if (FAILED(hr)) return -4;
    }

    // Linear sampler
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&sd, &gLinearSampler);
        if (FAILED(hr)) return -5;
    }

    Log("SSIL: Initialized (%ux%u)", width, height);
    return 0;
}

static void SSILShutdown()
{
    SSILRenderer::Shutdown();

    if (gCompositePS)   { gCompositePS->Release();   gCompositePS = nullptr; }
    if (gAdditiveBlend) { gAdditiveBlend->Release(); gAdditiveBlend = nullptr; }
    if (gLinearSampler) { gLinearSampler->Release();  gLinearSampler = nullptr; }

    gDevice = nullptr;
    Log("SSIL: Shut down");
}

static void SSILOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    SSILRenderer::OnResolutionChanged(device, w, h);
    Log("SSIL: Resolution changed to %ux%u", w, h);
}

static int SSILIsEnabled()
{
    // Always return 1 so the framework always calls preExecute/postExecute.
    return 1;
}

// ==================== GUI Settings ====================

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",            DUST_SETTING_BOOL,  &gSSILConfig.enabled,          0.0f,    1.0f,   "Enabled" },
    { "Radius",             DUST_SETTING_FLOAT, &gSSILConfig.ilRadius,         0.0005f, 0.02f,  "Radius" },
    { "Strength",           DUST_SETTING_FLOAT, &gSSILConfig.ilStrength,       0.1f,    10.0f,  "Strength" },
    { "Bias",               DUST_SETTING_FLOAT, &gSSILConfig.ilBias,           0.0f,    0.3f,   "Bias" },
    { "Max Depth",          DUST_SETTING_FLOAT, &gSSILConfig.ilMaxDepth,       0.01f,   1.0f,   "MaxDepth" },
    { "Foreground Fade",    DUST_SETTING_FLOAT, &gSSILConfig.foregroundFade,   1.0f,    200.0f, "ForegroundFade" },
    { "Falloff Power",      DUST_SETTING_FLOAT, &gSSILConfig.falloffPower,     0.5f,    5.0f,   "FalloffPower" },
    { "Max Screen Radius",  DUST_SETTING_FLOAT, &gSSILConfig.maxScreenRadius,  0.005f,  0.2f,   "MaxScreenRadius" },
    { "Min Screen Radius",  DUST_SETTING_FLOAT, &gSSILConfig.minScreenRadius,  0.0001f, 0.01f,  "MinScreenRadius" },
    { "Color Bleeding",     DUST_SETTING_FLOAT, &gSSILConfig.colorBleeding,    0.0f,    2.0f,   "ColorBleeding" },
    { "Directions",         DUST_SETTING_INT,   &gSSILConfig.sampleCount,      2.0f,    12.0f,  "Directions" },
    { "Steps",              DUST_SETTING_INT,   &gSSILConfig.stepCount,        1.0f,    6.0f,   "Steps" },
    { "Blur Sharpness",     DUST_SETTING_FLOAT, &gSSILConfig.blurSharpness,    0.0f,    0.1f,   "BlurSharpness" },
    { "Debug View",         DUST_SETTING_BOOL,  &gSSILConfig.debugView,        0.0f,    1.0f,   "DebugView" },
    // Hidden settings
    { "Depth Fade Start",   DUST_SETTING_HIDDEN_FLOAT, &gSSILConfig.depthFadeStart, 0.0f, 1.0f,   "DepthFadeStart" },
    { "Tan Half FOV",       DUST_SETTING_HIDDEN_FLOAT, &gSSILConfig.tanHalfFov,     0.1f, 2.0f,   "TanHalfFov" },
};

// Plugin entry point
extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "SSIL";
    desc->injectionPoint    = DUST_INJECT_POST_LIGHTING;
    desc->Init              = SSILInit;
    desc->Shutdown          = SSILShutdown;
    desc->OnResolutionChanged = SSILOnResolutionChanged;
    desc->preExecute        = SSILPreExecute;
    desc->postExecute       = SSILPostExecute;
    desc->IsEnabled         = SSILIsEnabled;
    desc->settings          = gSettingsArray;
    desc->settingCount      = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->OnSettingChanged  = nullptr;

    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "SSIL";
    desc->priority          = 10;   // Run after SSAO (priority 0) at POST_LIGHTING

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
