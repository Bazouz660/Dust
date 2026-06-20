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

    // Temporal stabilization (opt-in). Reprojects the previous resolved frame
    // and blends it in to suppress edge crawl / aliasing flicker under camera
    // motion. Kept sharp via Catmull-Rom history + neighborhood clipping.
    bool  temporalEnabled   = false;
    float temporalStrength  = 0.85f;
    float tanHalfFov        = 0.4142f;   // Kenshi: projMatrix[1][1]=2.414 → 1/2.414
    // True view far clip. Kenshi stores depth as linear viewZ/farClip, so the
    // reprojection world-unit scale is depth*farPlane. Measured ~50000 from the
    // deferred far frustum corner; tunable if a different view distance is used.
    float farPlane          = 50000.0f;
};

static SMAAConfig gConfig;

static ID3D11PixelShader* gEdgeDetectPS   = nullptr;
static ID3D11PixelShader* gBlendWeightPS  = nullptr;
static ID3D11PixelShader* gResolvePS      = nullptr;
static ID3D11PixelShader* gTemporalPS     = nullptr;

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

// Temporal resources: current SMAA-1x output + ping-pong history.
static SMAATexture gCurrentTex;
static SMAATexture gHistoryTex[2];
static int         gHistoryIdx = 0;
static bool        gHasHistory = false;
static bool        gPrevTemporalEnabled = false;
static float       gInverseView[16] = {};
static float       gPrevInverseView[16] = {};

static ID3D11Texture2D*          gAreaTexture  = nullptr;
static ID3D11ShaderResourceView* gAreaSRV      = nullptr;
static ID3D11Texture2D*          gSearchTexture = nullptr;
static ID3D11ShaderResourceView* gSearchSRV    = nullptr;

struct SMAACB {
    // First 32 bytes are byte-identical to the rtMetrics layout the edge /
    // blend-weight / resolve shaders expect — do not reorder.
    float invWidth, invHeight, width, height;
    float lumaThreshold;
    float depthThreshold;
    int   edgeMode;
    int   temporalEnabled;       // was 'pad'; ignored by the non-temporal passes

    // Temporal-only fields (read by smaa_temporal_ps.hlsl).
    float tanHalfFov;
    float aspectRatio;
    float temporalStrength;
    float farPlane;

    float reproj[16];            // row-major, currentInvView * prevView

    float frameIndex;
    float hasHistory;
    float pad0, pad1;
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
    if (!CreateTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, gCurrentTex, "current"))
        return false;
    if (!CreateTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, gHistoryTex[0], "history0"))
        return false;
    if (!CreateTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, gHistoryTex[1], "history1"))
        return false;
    gHasHistory = false;   // history is stale after a (re)allocation
    return true;
}

// Compute reproj = currentInvView * prevView (row-major), used in the temporal
// shader as mul(float4(viewPos,1), reproj). Ported verbatim from the proven
// RTGI pipeline — keeps the convention identical so reprojection stays exact.
static void ComputeReprojectionMatrix(const float* curInv, const float* prevInv, float* out)
{
    // prevView = inverse(prevInvView): transpose the 3x3 rotation, derive translation.
    float pv[16];
    pv[0]  = prevInv[0]; pv[1]  = prevInv[4]; pv[2]  = prevInv[8];  pv[3]  = 0;
    pv[4]  = prevInv[1]; pv[5]  = prevInv[5]; pv[6]  = prevInv[9];  pv[7]  = 0;
    pv[8]  = prevInv[2]; pv[9]  = prevInv[6]; pv[10] = prevInv[10]; pv[11] = 0;
    float px = prevInv[12], py = prevInv[13], pz = prevInv[14];
    pv[12] = -(px * pv[0] + py * pv[4] + pz * pv[8]);
    pv[13] = -(px * pv[1] + py * pv[5] + pz * pv[9]);
    pv[14] = -(px * pv[2] + py * pv[6] + pz * pv[10]);
    pv[15] = 1;

    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = curInv[i * 4 + 0] * pv[0 * 4 + j]
                           + curInv[i * 4 + 1] * pv[1 * 4 + j]
                           + curInv[i * 4 + 2] * pv[2 * 4 + j]
                           + curInv[i * 4 + 3] * pv[3 * 4 + j];
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
    gTemporalPS    = CompilePS("smaa_temporal_ps.hlsl",      "temporal");
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
    ReleaseTexture(gCurrentTex);
    ReleaseTexture(gHistoryTex[0]);
    ReleaseTexture(gHistoryTex[1]);
    if (gTemporalPS)    { gTemporalPS->Release();      gTemporalPS = nullptr; }
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

    // Temporal needs camera data; fall back to plain SMAA 1x if it's missing.
    bool temporal = gConfig.temporalEnabled && ctx->camera.valid;

    // Toggling temporal invalidates the accumulated history.
    if (gConfig.temporalEnabled != gPrevTemporalEnabled) {
        gHasHistory = false;
        gPrevTemporalEnabled = gConfig.temporalEnabled;
    }

    // Shift the camera matrices every frame so prev/cur stay adjacent even
    // across frames where temporal was disabled.
    if (ctx->camera.valid) {
        memcpy(gPrevInverseView, gInverseView, sizeof(gInverseView));
        memcpy(gInverseView, ctx->camera.inverseView, sizeof(gInverseView));
    }

    SMAACB cb = {};
    cb.invWidth       = 1.0f / (float)ctx->width;
    cb.invHeight      = 1.0f / (float)ctx->height;
    cb.width          = (float)ctx->width;
    cb.height         = (float)ctx->height;
    cb.lumaThreshold  = gConfig.lumaThreshold;
    cb.depthThreshold = gConfig.depthThreshold;
    cb.edgeMode       = gConfig.edgeMode;

    cb.temporalEnabled  = temporal ? 1 : 0;
    cb.tanHalfFov       = gConfig.tanHalfFov;
    cb.aspectRatio      = (float)ctx->width / (float)ctx->height;
    cb.temporalStrength = gConfig.temporalStrength;
    cb.farPlane         = gConfig.farPlane;
    cb.frameIndex       = (float)ctx->frameIndex;
    cb.hasHistory       = (temporal && gHasHistory) ? 1.0f : 0.0f;
    cb.reproj[0] = cb.reproj[5] = cb.reproj[10] = cb.reproj[15] = 1.0f; // identity
    if (temporal && gHasHistory)
        ComputeReprojectionMatrix(gInverseView, gPrevInverseView, cb.reproj);

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

    // --- Pass 3: Neighborhood blending ---
    // Writes the final image to ldr_rt for plain SMAA, or to the "current"
    // buffer when the temporal pass will consume it next.
    {
        ID3D11RenderTargetView* resolveRTV = temporal ? gCurrentTex.rtv : ldrRTV;
        dc->OMSetRenderTargets(1, &resolveRTV, nullptr);

        dc->PSSetShaderResources(0, 1, &sceneCopy);
        dc->PSSetShaderResources(1, 1, &gBlendTex.srv);
        ID3D11SamplerState* samplers[2] = { gLinearSampler, gPointSampler };
        dc->PSSetSamplers(0, 2, samplers);

        host->DrawFullscreenTriangle(dc, gResolvePS);

        dc->PSSetShaderResources(0, 1, &nullSRV);
        dc->PSSetShaderResources(1, 1, &nullSRV);
    }

    // --- Pass 4: Temporal resolve (optional) ---
    // Blends the reprojected, neighborhood-clipped history into the current
    // SMAA output. Writes the display image to ldr_rt and the new history
    // (ping-pong) in a single MRT draw.
    if (temporal)
    {
        int prevIdx = gHistoryIdx;
        int currIdx = gHistoryIdx ^ 1;

        ID3D11RenderTargetView* rtvs[2] = { ldrRTV, gHistoryTex[currIdx].rtv };
        dc->OMSetRenderTargets(2, rtvs, nullptr);

        dc->PSSetShaderResources(0, 1, &gCurrentTex.srv);
        dc->PSSetShaderResources(1, 1, &gHistoryTex[prevIdx].srv);
        ID3D11ShaderResourceView* depthSRV = host->GetSRV(DUST_RESOURCE_DEPTH);
        dc->PSSetShaderResources(2, 1, &depthSRV);
        ID3D11SamplerState* samplers[2] = { gPointSampler, gLinearSampler };
        dc->PSSetSamplers(0, 2, samplers);

        host->DrawFullscreenTriangle(dc, gTemporalPS);

        dc->PSSetShaderResources(0, 1, &nullSRV);
        dc->PSSetShaderResources(1, 1, &nullSRV);
        dc->PSSetShaderResources(2, 1, &nullSRV);
        ID3D11RenderTargetView* nullRTVs[2] = { nullptr, nullptr };
        dc->OMSetRenderTargets(2, nullRTVs, nullptr);

        gHistoryIdx = currIdx;
        gHasHistory = true;
    }

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
    { "Temporal Stabilize", DUST_SETTING_BOOL,    &gConfig.temporalEnabled,  0.0f,  1.0f, "Temporal",          nullptr,          "Reproject and blend the previous frame to suppress edge crawl / aliasing flicker while the camera moves. Stays sharp via Catmull-Rom history and neighborhood clipping.", DUST_PERF_MEDIUM },
    { "Temporal Strength",  DUST_SETTING_FLOAT,   &gConfig.temporalStrength, 0.0f,  0.95f,"TemporalStrength",  nullptr,          "Max history blend weight. Higher = steadier but softer; lower = sharper but more flicker. 0.85 is a good balance.", DUST_PERF_NONE },

    // Reprojection tuning — hidden, but persisted so it can be adjusted in the INI.
    { "Tan Half FOV",       DUST_SETTING_HIDDEN_FLOAT, &gConfig.tanHalfFov,  0.1f,  2.0f,     "TanHalfFov" },
    { "Far Plane",          DUST_SETTING_HIDDEN_FLOAT, &gConfig.farPlane,    1000.0f, 100000.0f, "ReprojFarClip" },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;

    memset(desc, 0, sizeof(*desc));
    desc->apiVersion          = DUST_API_VERSION;
    desc->name                = "Anti-Aliasing";
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
