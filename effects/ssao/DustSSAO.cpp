// DustSSAO.cpp - SSAO effect plugin for Dust
// Exports DustEffectCreate per the Dust plugin API.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "SSAORenderer.h"
#include "SSAOConfig.h"
#include "SSAOMenu.h"

#include <d3d11.h>
#include <cstring>

static const DustHostAPI* gHost = nullptr;
static HMODULE gPluginModule = nullptr;
static float gGpuTimeMs = 0.0f;

// Log function pointer used by DustLog.h in other translation units
DustLogFn gLogFn = nullptr;

// Sampler + white fallback for binding AO to slot 8
static ID3D11SamplerState* gAoSampler = nullptr;
static ID3D11Texture2D* gWhiteTex = nullptr;
static ID3D11ShaderResourceView* gWhiteSRV = nullptr;

static bool CreateAoSampler(ID3D11Device* device)
{
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr = device->CreateSamplerState(&sd, &gAoSampler);
    if (FAILED(hr))
    {
        Log("SSAO: Failed to create AO sampler: 0x%08X", hr);
        return false;
    }
    return true;
}

static bool CreateWhiteFallback(ID3D11Device* device)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    uint8_t white = 255;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = &white;
    init.SysMemPitch = 1;

    HRESULT hr = device->CreateTexture2D(&desc, &init, &gWhiteTex);
    if (FAILED(hr))
    {
        Log("SSAO: Failed to create white texture: 0x%08X", hr);
        return false;
    }

    hr = device->CreateShaderResourceView(gWhiteTex, nullptr, &gWhiteSRV);
    if (FAILED(hr))
    {
        Log("SSAO: Failed to create white SRV: 0x%08X", hr);
        gWhiteTex->Release();
        gWhiteTex = nullptr;
        return false;
    }
    return true;
}

// The register declared in deferred.hlsl for aoMap
static const uint32_t AO_REGISTER = 8;

// Called BEFORE the game's lighting draw
static void SSAOPreExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    ID3D11ShaderResourceView* depthSRV = host->GetSRV("depth");
    if (!depthSRV)
        return;

    if (!SSAORenderer::IsInitialized())
        return;

    // Hot-reload config if changed on disk
    gSSAOConfig.CheckHotReload();

    if (!gSSAOConfig.enabled)
    {
        gGpuTimeMs = 0.0f;
        // Bind white (no occlusion) so deferred.hlsl still reads a valid texture
        host->BindSRV(ctx->context, AO_REGISTER, gWhiteSRV, gAoSampler);
        return;
    }

    // Generate AO (saves/restores GPU state internally)
    ID3D11ShaderResourceView* aoSRV = SSAORenderer::RenderAO(ctx->context, depthSRV);
    if (!aoSRV)
        aoSRV = gWhiteSRV;

    // Update GPU timing for performance display
    gGpuTimeMs = SSAORenderer::GetLastGpuTimeMs();

    // Bind AO for deferred.hlsl to sample
    host->BindSRV(ctx->context, AO_REGISTER, aoSRV, gAoSampler);
}

// Called AFTER the game's lighting draw
static void SSAOPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    // Unbind the AO slot
    host->UnbindSRV(ctx->context, AO_REGISTER);

    // Debug overlay
    if (gSSAOConfig.debugView && gSSAOConfig.enabled)
    {
        ID3D11RenderTargetView* hdrRTV = host->GetRTV("hdr_rt");
        if (hdrRTV)
            SSAORenderer::RenderDebugOverlay(ctx->context, hdrRTV);
    }
}

static int SSAOInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
    gHost = host;
#undef Log
    gLogFn = host->Log;
#define Log DustLog

    // Init config (finds/creates .ini next to the plugin DLL)
    gSSAOConfig.Init(gPluginModule);

    if (!CreateAoSampler(device))
        return -1;

    if (!CreateWhiteFallback(device))
        return -2;

    if (!SSAORenderer::Init(device, width, height))
        return -3;

    Log("SSAO: Initialized (%ux%u)", width, height);
    return 0;
}

static void SSAOShutdown()
{
    SSAORenderer::Shutdown();

    if (gAoSampler) { gAoSampler->Release(); gAoSampler = nullptr; }
    if (gWhiteSRV)  { gWhiteSRV->Release();  gWhiteSRV = nullptr; }
    if (gWhiteTex)  { gWhiteTex->Release();   gWhiteTex = nullptr; }

    Log("SSAO: Shut down");
}

static void SSAOOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    SSAORenderer::OnResolutionChanged(device, w, h);
    Log("SSAO: Resolution changed to %ux%u", w, h);
}

static int SSAOIsEnabled()
{
    // Always return 1 so the framework always calls preExecute/postExecute.
    // When disabled, preExecute binds a white fallback so the shader reads valid data.
    return 1;
}

// ==================== GUI Settings ====================

static void SSAOSaveSettings()
{
    gSSAOConfig.Save();
    Log("SSAO: Settings saved to disk");
}

static void SSAOLoadSettings()
{
    gSSAOConfig.Load();
    Log("SSAO: Settings reloaded from disk");
}

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",            DUST_SETTING_BOOL,  &gSSAOConfig.enabled,            0.0f,    1.0f   },
    { "Radius",             DUST_SETTING_FLOAT, &gSSAOConfig.aoRadius,           0.0005f, 0.01f  },
    { "Strength",           DUST_SETTING_FLOAT, &gSSAOConfig.aoStrength,         0.5f,    10.0f  },
    { "Bias",               DUST_SETTING_FLOAT, &gSSAOConfig.aoBias,             0.0f,    0.2f   },
    { "Max Depth",          DUST_SETTING_FLOAT, &gSSAOConfig.aoMaxDepth,         0.01f,   1.0f   },
    { "Filter Radius",      DUST_SETTING_FLOAT, &gSSAOConfig.filterRadius,       0.01f,   1.0f   },
    { "Foreground Fade",    DUST_SETTING_FLOAT, &gSSAOConfig.foregroundFade,      1.0f,    200.0f },
    { "Falloff Power",      DUST_SETTING_FLOAT, &gSSAOConfig.falloffPower,       0.5f,    5.0f   },
    { "Max Screen Radius",  DUST_SETTING_FLOAT, &gSSAOConfig.maxScreenRadius,    0.005f,  0.2f   },
    { "Min Screen Radius",  DUST_SETTING_FLOAT, &gSSAOConfig.minScreenRadius,    0.0001f, 0.01f  },
    { "Blur Sharpness",     DUST_SETTING_FLOAT, &gSSAOConfig.blurSharpness,      0.0f,    0.1f   },
    { "Night Compensation", DUST_SETTING_FLOAT, &gSSAOConfig.nightCompensation,  0.0f,    50.0f  },
    { "Debug View",         DUST_SETTING_BOOL,  &gSSAOConfig.debugView,          0.0f,    1.0f   },
};

// No runtime action needed when settings change — values are read live each frame

// Plugin entry point
extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "SSAO";
    desc->injectionPoint    = DUST_INJECT_POST_LIGHTING;
    desc->Init              = SSAOInit;
    desc->Shutdown          = SSAOShutdown;
    desc->OnResolutionChanged = SSAOOnResolutionChanged;
    desc->preExecute        = SSAOPreExecute;
    desc->postExecute       = SSAOPostExecute;
    desc->IsEnabled         = SSAOIsEnabled;
    desc->settings          = gSettingsArray;
    desc->settingCount      = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->OnSettingChanged  = nullptr;
    desc->SaveSettings      = SSAOSaveSettings;
    desc->LoadSettings      = SSAOLoadSettings;
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
