// DustDOF.cpp - Depth of Field effect plugin for Dust (API v3)
// Blurs the scene based on distance from a focus plane, with auto-focus
// and separate near/far blur curves.
// Exports DustEffectCreate per the Dust plugin API.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "DOFRenderer.h"
#include "DOFConfig.h"

#include <d3d11.h>
#include <cstring>
#include <cmath>
#include <string>

DustLogFn gLogFn = nullptr;

DOFConfig gDOFConfig;

static HMODULE gPluginModule = nullptr;
static const DustHostAPI* gHost = nullptr;

// Auto-focus resources (double-buffered to avoid GPU pipeline stalls)
static const int FOCUS_BUFFER_COUNT = 2;
static ID3D11Texture2D* gFocusStagingTex[FOCUS_BUFFER_COUNT] = {};
static int gFocusWriteIdx = 0;   // which staging texture to write into this frame
static bool gFocusHasData[FOCUS_BUFFER_COUNT] = {}; // whether a valid copy exists
static float gCurrentFocusDepth = 0.02f;
static LARGE_INTEGER gLastFrameTime = {};
static LARGE_INTEGER gPerfFreq = {};

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

static bool CreateFocusResources(ID3D11Device* device)
{
    for (int i = 0; i < FOCUS_BUFFER_COUNT; i++)
    {
        if (gFocusStagingTex[i]) { gFocusStagingTex[i]->Release(); gFocusStagingTex[i] = nullptr; }
        gFocusHasData[i] = false;
    }
    gFocusWriteIdx = 0;

    // 1x1 staging textures to read center depth (double-buffered)
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    for (int i = 0; i < FOCUS_BUFFER_COUNT; i++)
    {
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &gFocusStagingTex[i]);
        if (FAILED(hr))
        {
            Log("DOF: Failed to create focus staging texture %d: 0x%08X", i, hr);
            return false;
        }
    }
    return true;
}

// Read the depth at screen center for auto-focus.
// Double-buffered: read from the staging texture written LAST frame (no stall),
// then issue a copy into the other staging texture for next frame to read.
static void UpdateAutoFocus(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gDOFConfig.autoFocus || !gFocusStagingTex[0])
        return;

    // Step 1: Read back from the OTHER buffer (written last frame) — should be ready, no stall
    int readIdx = 1 - gFocusWriteIdx;
    if (gFocusHasData[readIdx])
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = ctx->context->Map(gFocusStagingTex[readIdx], 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            float centerDepth = *(float*)mapped.pData;
            ctx->context->Unmap(gFocusStagingTex[readIdx], 0);

            // Sky/invalid depth: focus toward maxDepth so sky comes into focus
            float targetDepth;
            if (centerDepth > 0.0001f && centerDepth <= gDOFConfig.maxDepth)
                targetDepth = centerDepth;
            else
                targetDepth = gDOFConfig.maxDepth;

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = (float)(now.QuadPart - gLastFrameTime.QuadPart) / (float)gPerfFreq.QuadPart;
            gLastFrameTime = now;
            dt = dt < 0.001f ? 0.001f : (dt > 0.5f ? 0.5f : dt);

            float alpha = 1.0f - expf(-dt * gDOFConfig.autoFocusSpeed);
            gCurrentFocusDepth += (targetDepth - gCurrentFocusDepth) * alpha;
        }
    }

    // Step 2: Issue copy into the WRITE buffer for next frame to read
    ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
    if (!depthSRV)
        return;

    ID3D11Resource* depthResource = nullptr;
    depthSRV->GetResource(&depthResource);
    if (!depthResource)
        return;

    ID3D11Texture2D* depthTex = nullptr;
    HRESULT hr = depthResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&depthTex);
    depthResource->Release();
    if (FAILED(hr) || !depthTex)
        return;

    UINT cx = ctx->width / 2;
    UINT cy = ctx->height / 2;
    D3D11_BOX srcBox = { cx, cy, 0, cx + 1, cy + 1, 1 };
    ctx->context->CopySubresourceRegion(gFocusStagingTex[gFocusWriteIdx], 0, 0, 0, 0,
                                         depthTex, 0, &srcBox);
    depthTex->Release();

    gFocusHasData[gFocusWriteIdx] = true;
    gFocusWriteIdx = 1 - gFocusWriteIdx;  // swap for next frame
}

// Get the resolved focus distance (auto or manual)
float DOFGetResolvedFocusDistance()
{
    return gDOFConfig.autoFocus ? gCurrentFocusDepth : gDOFConfig.focusDistance;
}

// Called BEFORE the game's tonemap draw (for auto-focus sampling)
static void DOFPreExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gDOFConfig.enabled)
        return;

    UpdateAutoFocus(ctx, host);
}

// Called AFTER the game's tonemap draw
static void DOFPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!DOFRenderer::IsInitialized() || !gDOFConfig.enabled)
        return;

    ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
    if (!depthSRV)
        return;

    ID3D11RenderTargetView* ldrRTV = host->GetRTV(DUST_RESOURCE_LDR_RT);
    if (!ldrRTV)
        return;

    if (gDOFConfig.debugView)
    {
        DOFRenderer::RenderDebugOverlay(ctx->context, depthSRV, ldrRTV);
        return;
    }

    ID3D11ShaderResourceView* sceneCopy = host->GetSceneCopy(ctx->context, DUST_RESOURCE_LDR_RT);
    if (!sceneCopy)
        return;

    DOFRenderer::Render(ctx->context, sceneCopy, depthSRV, ldrRTV);
}

static int DOFInit(ID3D11Device* device, uint32_t width, uint32_t height, const DustHostAPI* host)
{
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gHost = host;

    std::string pluginDir = GetPluginDir();
    if (!DOFRenderer::Init(device, width, height, host, pluginDir.c_str()))
        return -1;

    if (!CreateFocusResources(device))
        return -2;

    QueryPerformanceFrequency(&gPerfFreq);
    QueryPerformanceCounter(&gLastFrameTime);
    gCurrentFocusDepth = gDOFConfig.focusDistance;

    Log("DOF: Initialized (%ux%u)", width, height);
    return 0;
}

static void DOFShutdown()
{
    DOFRenderer::Shutdown();
    for (int i = 0; i < FOCUS_BUFFER_COUNT; i++)
    {
        if (gFocusStagingTex[i]) { gFocusStagingTex[i]->Release(); gFocusStagingTex[i] = nullptr; }
        gFocusHasData[i] = false;
    }
    Log("DOF: Shut down");
}

static void DOFOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    DOFRenderer::OnResolutionChanged(device, w, h);
    Log("DOF: Resolution changed to %ux%u", w, h);
}

static int DOFIsEnabled()
{
    return gDOFConfig.enabled ? 1 : 0;
}

// ==================== GUI Settings ====================

static DustSettingDesc gSettingsArray[] = {
    { "Enabled",          DUST_SETTING_BOOL,  &gDOFConfig.enabled,          0.0f,   1.0f,   "Enabled" },
    { "Auto Focus",       DUST_SETTING_BOOL,  &gDOFConfig.autoFocus,        0.0f,   1.0f,   "AutoFocus" },
    { "Focus Speed",      DUST_SETTING_FLOAT, &gDOFConfig.autoFocusSpeed,   0.5f,   20.0f,  "AutoFocusSpeed" },
    { "Focus Distance",   DUST_SETTING_FLOAT, &gDOFConfig.focusDistance,    0.001f, 0.1f,   "FocusDistance" },
    { "Near Start",       DUST_SETTING_FLOAT, &gDOFConfig.nearStart,       0.0001f, 0.05f,  "NearStart" },
    { "Near End",         DUST_SETTING_FLOAT, &gDOFConfig.nearEnd,         0.001f,  0.1f,   "NearEnd" },
    { "Near Strength",    DUST_SETTING_FLOAT, &gDOFConfig.nearStrength,    0.0f,    1.0f,   "NearStrength" },
    { "Far Start",        DUST_SETTING_FLOAT, &gDOFConfig.farStart,        0.0001f, 0.05f,  "FarStart" },
    { "Far End",          DUST_SETTING_FLOAT, &gDOFConfig.farEnd,          0.001f,  0.1f,   "FarEnd" },
    { "Far Strength",     DUST_SETTING_FLOAT, &gDOFConfig.farStrength,     0.0f,    1.0f,   "FarStrength" },
    { "Blur Radius",      DUST_SETTING_FLOAT, &gDOFConfig.blurRadius,      1.0f,    32.0f,  "BlurRadius" },
    { "Blur Downscale",   DUST_SETTING_INT,   &gDOFConfig.blurDownscale,   2.0f,    4.0f,   "BlurDownscale" },
    { "Max Depth",        DUST_SETTING_FLOAT, &gDOFConfig.maxDepth,        0.01f,   1.0f,   "MaxDepth" },
    { "Debug View",       DUST_SETTING_BOOL,  &gDOFConfig.debugView,       0.0f,    1.0f,   "DebugView" },
};

// Plugin entry point
extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion        = DUST_API_VERSION;
    desc->name              = "Depth of Field";
    desc->injectionPoint    = DUST_INJECT_POST_TONEMAP;
    desc->Init              = DOFInit;
    desc->Shutdown          = DOFShutdown;
    desc->OnResolutionChanged = DOFOnResolutionChanged;
    desc->preExecute        = DOFPreExecute;
    desc->postExecute       = DOFPostExecute;
    desc->IsEnabled         = DOFIsEnabled;
    desc->settings          = gSettingsArray;
    desc->settingCount      = sizeof(gSettingsArray) / sizeof(gSettingsArray[0]);
    desc->OnSettingChanged  = nullptr;

    desc->flags             = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection     = "DOF";

    desc->priority          = 75;

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
