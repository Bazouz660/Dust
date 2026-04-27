// DustDebugViews.cpp — Global GBuffer debug visualisations for Dust/Kenshi.
//
// Injection: POST_TONEMAP priority 9999 — last effect before the game draws its UI.
// The active mode fully replaces the LDR output; the game then draws UI on top.
//
// Mode 1  Wireframe      — geometry replay with D3D11_FILL_WIREFRAME, correct depth occlusion.
//                          The captured VP is extracted from any SKINNED draw (where it is
//                          stored directly); falls back to a cached value if none visible.
// Mode 2  World Normals  — GBuffer1 normals decoded to world-space, shown as RGB.
// Mode 3  View Normals   — same transformed to camera space via DustCameraData.inverseView.
// Mode 4  Depth          — GBuffer2 linear view-Z, power-remapped to [depthMin..depthMax].
// Mode 5  Luma           — GBuffer0.r = Y channel of YCoCg albedo encoding.
// Mode 6  Roughness      — 1 − GBuffer0.a (gloss), colour-coded blue=smooth → red=rough.
// Mode 7  Metalness      — GBuffer0.b, colour-coded grey=dielectric → gold=metal.
// Mode 8  Lighting       — pre-fog HDR, Reinhard-tonemapped for display.

#include "../../src/DustAPI.h"
#include "DustLog.h"

#include <d3d11.h>
#include <cstring>
#include <string>

DustLogFn gLogFn = nullptr;
static HMODULE gPluginModule = nullptr;
static const DustHostAPI* gHost = nullptr;

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

// ── GPU resources ──────────────────────────────────────────────────────────

// Fullscreen PS for GBuffer modes 2-8
static ID3D11PixelShader*       gGBufferPS      = nullptr;
static ID3D11Buffer*            gGBufferCB      = nullptr;

// Wireframe mode (mode 1)
static ID3D11PixelShader*       gWirePS         = nullptr;
static ID3D11Buffer*            gWireCB         = nullptr;   // float4 wireColor
static ID3D11RasterizerState*   gWireRS         = nullptr;   // FILL_WIREFRAME, CULL_NONE
static ID3D11DepthStencilState* gWireDSS        = nullptr;   // depth test + write
static ID3D11Texture2D*         gWireDepthTex   = nullptr;   // dedicated DSV for wireframe
static ID3D11DepthStencilView*  gWireDSV        = nullptr;

// Shared pipeline state
static ID3D11RasterizerState*   gSolidRS        = nullptr;   // FILL_SOLID, CULL_NONE
static ID3D11DepthStencilState* gNoDSS          = nullptr;   // no depth test/write
static ID3D11BlendState*        gNoBlend        = nullptr;
static ID3D11SamplerState*      gPointSamp      = nullptr;
static ID3D11SamplerState*      gLinearSamp     = nullptr;

static UINT gWireDepthW = 0, gWireDepthH = 0;

// Camera VP extracted from the last SKINNED draw — reused across frames when
// no SKINNED draws are visible (e.g. empty area with only static geometry).
static float gCachedVP[16]  = {};
static bool  gHasCachedVP   = false;

// ── CB layouts ─────────────────────────────────────────────────────────────

// GBuffer PS CB (must match debug_views_ps.hlsl exactly)
struct alignas(16) GBufferCBData
{
    int32_t viewMode;
    float   depthMin;
    float   depthMax;
    float   _pad0;
    float   inverseView[16]; // row-major world←view
    float   viewportW;
    float   viewportH;
    float   _pad1[2];
};
static_assert(sizeof(GBufferCBData) == 96, "GBufferCBData size mismatch");

// Wireframe PS CB
struct alignas(16) WireCBData
{
    float r, g, b, a;
};

// ── Settings ───────────────────────────────────────────────────────────────

static int   gViewMode      = 0;
static float gWireColor[3]  = { 0.0f, 1.0f, 0.0f };   // green wire
static float gBgColor[3]    = { 0.05f, 0.05f, 0.05f }; // near-black background
static float gDepthMin      = 0.0f;
static float gDepthMax      = 200.0f;

// ── Helper: create / recreate the wireframe depth buffer ───────────────────

static bool CreateWireDepth(ID3D11Device* device, UINT w, UINT h)
{
    if (gWireDepthTex) { gWireDepthTex->Release(); gWireDepthTex = nullptr; }
    if (gWireDSV)      { gWireDSV->Release();      gWireDSV      = nullptr; }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &gWireDepthTex))) return false;
    if (FAILED(device->CreateDepthStencilView(gWireDepthTex, nullptr, &gWireDSV)))
        return false;

    gWireDepthW = w;
    gWireDepthH = h;
    return true;
}

// ── Helper: extract camera VP from a SKINNED draw's frozen CB ──────────────

static bool ExtractCameraVP(ID3D11DeviceContext* ctx, float* outVP)
{
    uint32_t count = gHost->GetGeometryDrawCount();
    for (uint32_t i = 0; i < count; i++)
    {
        DustGeometryDraw info;
        if (gHost->GetGeometryDrawInfo(i, &info) != 0) continue;
        // SKINNED draws store viewProjectionMatrix directly at clipMatrixOffset
        if (info.transformType != DUST_TRANSFORM_SKINNED) continue;

        const void* data;
        uint32_t size;
        if (gHost->GetGeometryDrawConstants(ctx, i, &data, &size) != 0) continue;
        if (info.clipMatrixOffset + 64 > size) continue;

        memcpy(outVP, (const uint8_t*)data + info.clipMatrixOffset, 64);
        return true;
    }
    return false;
}

// ── Wireframe render ────────────────────────────────────────────────────────

static void RenderWireframe(ID3D11DeviceContext* ctx, ID3D11Device* device,
                            ID3D11RenderTargetView* ldrRTV,
                            UINT w, UINT h)
{
    // Recreate depth buffer if viewport changed
    if (w != gWireDepthW || h != gWireDepthH)
        CreateWireDepth(device, w, h);
    if (!gWireDSV) return;

    // Extract camera VP (prefer fresh; fall back to cached if no SKINNED draws visible)
    float vp[16];
    if (ExtractCameraVP(ctx, vp))
    {
        memcpy(gCachedVP, vp, 64);
        gHasCachedVP = true;
    }
    else if (gHasCachedVP)
    {
        memcpy(vp, gCachedVP, 64);
    }
    else
    {
        // No VP available yet — skip this frame
        return;
    }

    // Clear LDR to background, clear depth
    ctx->ClearRenderTargetView(ldrRTV, gBgColor);
    ctx->ClearDepthStencilView(gWireDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Update wire color CB
    WireCBData wc = { gWireColor[0], gWireColor[1], gWireColor[2], 1.0f };
    gHost->UpdateConstantBuffer(ctx, gWireCB, &wc, sizeof(wc));

    // Bind output targets
    ctx->OMSetRenderTargets(1, &ldrRTV, gWireDSV);
    D3D11_VIEWPORT vp11 = { 0, 0, (float)w, (float)h, 0, 1 };
    ctx->RSSetViewports(1, &vp11);

    // Wireframe pipeline state
    ctx->RSSetState(gWireRS);
    ctx->OMSetDepthStencilState(gWireDSS, 0);
    float bf[4] = {};
    ctx->OMSetBlendState(gNoBlend, bf, 0xFFFFFFFF);

    ctx->GSSetShader(nullptr, nullptr, 0); // ensure no leftover GS from other effects
    ctx->PSSetShader(gWirePS, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &gWireCB);

    // Replay all captured GBuffer draws from the camera's perspective.
    // ReplayGeometry handles STATIC (world * VP) and SKINNED (VP only) correctly.
    gHost->ReplayGeometry(ctx, device, vp);

    // Leave RTV bound — PostExecute's RestoreState will clean up
}

// ── GBuffer fullscreen modes 2-8 ───────────────────────────────────────────

static void RenderGBufferMode(ID3D11DeviceContext* ctx,
                              ID3D11RenderTargetView* ldrRTV,
                              const DustFrameContext* fctx,
                              const DustHostAPI* host)
{
    GBufferCBData cb = {};
    cb.viewMode  = gViewMode;
    cb.depthMin  = gDepthMin;
    cb.depthMax  = gDepthMax;
    cb.viewportW = (float)fctx->width;
    cb.viewportH = (float)fctx->height;
    if (fctx->camera.valid)
        memcpy(cb.inverseView, fctx->camera.inverseView, 64);
    else
    {
        float id[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        memcpy(cb.inverseView, id, 64);
    }
    host->UpdateConstantBuffer(ctx, gGBufferCB, &cb, sizeof(cb));

    ctx->OMSetRenderTargets(1, &ldrRTV, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)fctx->width, (float)fctx->height, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(gSolidRS);
    ctx->OMSetDepthStencilState(gNoDSS, 0);
    float bf[4] = {};
    ctx->OMSetBlendState(gNoBlend, bf, 0xFFFFFFFF);

    ctx->PSSetConstantBuffers(0, 1, &gGBufferCB);

    // t0=depth, t1=normals, t2=albedo, t3=hdr
    ID3D11ShaderResourceView* srvs[4] = {
        host->GetSRV(DUST_RESOURCE_DEPTH),
        host->GetSRV(DUST_RESOURCE_NORMALS),
        host->GetSRV(DUST_RESOURCE_ALBEDO),
        host->GetPreFogHDR(),
    };
    ctx->PSSetShaderResources(0, 4, srvs);

    ID3D11SamplerState* sampers[2] = { gPointSamp, gLinearSamp };
    ctx->PSSetSamplers(0, 2, sampers);

    host->DrawFullscreenTriangle(ctx, gGBufferPS);

    ID3D11ShaderResourceView* nulls[4] = {};
    ctx->PSSetShaderResources(0, 4, nulls);
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

static int DebugViewsInit(ID3D11Device* device, uint32_t w, uint32_t h,
                          const DustHostAPI* host)
{
    gHost = host;
#undef Log
    gLogFn = host->Log;
#define Log DustLog

    // GBuffer fullscreen PS (modes 2-8)
    std::string psPath = GetPluginDir() + "\\shaders\\debug_views_ps.hlsl";
    {
        ID3DBlob* blob = host->CompileShaderFromFile(psPath.c_str(), "main", "ps_5_0");
        if (!blob) { Log("DebugViews: failed to compile GBuffer PS"); return -1; }
        HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(),
                                                blob->GetBufferSize(),
                                                nullptr, &gGBufferPS);
        blob->Release();
        if (FAILED(hr)) { Log("DebugViews: CreatePixelShader failed (0x%08X)", hr); return -1; }
    }
    gGBufferCB = host->CreateConstantBuffer(device, sizeof(GBufferCBData));
    if (!gGBufferCB) { Log("DebugViews: GBuffer CB creation failed"); return -1; }

    // Wireframe solid-colour PS (mode 1)
    static const char* kWirePSSrc = R"(
        cbuffer WireCB : register(b0) { float4 color; };
        float4 main() : SV_Target { return color; }
    )";
    {
        ID3DBlob* blob = host->CompileShader(kWirePSSrc, "main", "ps_5_0");
        if (!blob) { Log("DebugViews: failed to compile wireframe PS"); return -1; }
        HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(),
                                                blob->GetBufferSize(),
                                                nullptr, &gWirePS);
        blob->Release();
        if (FAILED(hr)) { Log("DebugViews: wireframe CreatePixelShader failed (0x%08X)", hr); return -1; }
    }
    gWireCB = host->CreateConstantBuffer(device, sizeof(WireCBData));
    if (!gWireCB) { Log("DebugViews: wireframe CB creation failed"); return -1; }

    // Rasterizer states
    {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_WIREFRAME;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        device->CreateRasterizerState(&rd, &gWireRS);

        rd.FillMode = D3D11_FILL_SOLID;
        device->CreateRasterizerState(&rd, &gSolidRS);
    }

    // Depth-stencil states
    {
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable    = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc      = D3D11_COMPARISON_LESS;
        device->CreateDepthStencilState(&dd, &gWireDSS);

        dd.DepthEnable = FALSE;
        device->CreateDepthStencilState(&dd, &gNoDSS);
    }

    // Blend
    {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bd, &gNoBlend);
    }

    // Samplers
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&sd, &gPointSamp);

        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        device->CreateSamplerState(&sd, &gLinearSamp);
    }

    // Wireframe depth buffer
    if (!CreateWireDepth(device, w, h))
        Log("DebugViews: WARNING — wireframe depth buffer creation failed");

    Log("DebugViews: initialized (%ux%u)", w, h);
    return 0;
}

static void DebugViewsShutdown()
{
#define SR(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while(0)
    SR(gGBufferPS); SR(gGBufferCB);
    SR(gWirePS);    SR(gWireCB);
    SR(gWireRS);    SR(gSolidRS);
    SR(gWireDSS);   SR(gNoDSS);
    SR(gNoBlend);
    SR(gPointSamp); SR(gLinearSamp);
    SR(gWireDSV);   SR(gWireDepthTex);
#undef SR
    gWireDepthW = gWireDepthH = 0;
}

static void DebugViewsOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    if (device) CreateWireDepth(device, w, h);
}

// ── Per-frame ───────────────────────────────────────────────────────────────

static void DebugViewsPost(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (gViewMode == 0) return;

    ID3D11RenderTargetView* ldrRTV = host->GetRTV(DUST_RESOURCE_LDR_RT);
    if (!ldrRTV) return;

    host->SaveState(ctx->context);

    if (gViewMode == 1)
        RenderWireframe(ctx->context, ctx->device, ldrRTV, ctx->width, ctx->height);
    else
        RenderGBufferMode(ctx->context, ldrRTV, ctx, host);

    host->RestoreState(ctx->context);
}

static int DebugViewsIsEnabled() { return 1; }

// ── Settings ────────────────────────────────────────────────────────────────

static const char* kModeLabels[] = {
    "Off",
    "Wireframe",
    "World Normals", "View Normals",
    "Depth",
    "Luma (Albedo Y)", "Roughness", "Metalness",
    "Lighting (HDR)",
    nullptr
};

static DustSettingDesc gSettings[] = {
    { "View Mode",      DUST_SETTING_ENUM,  &gViewMode, 0, 8, "ViewMode", kModeLabels,
      "Active debug visualisation. Off = no cost. All modes fully replace the scene; UI renders on top." },

    { "Wireframe",      DUST_SETTING_SECTION, nullptr, 0, 0, nullptr, nullptr, nullptr },
    { "Wire Color",     DUST_SETTING_COLOR3, gWireColor, 0, 1, "WireColor", nullptr,
      "Colour of the wireframe triangle edges" },
    { "Background",     DUST_SETTING_COLOR3, gBgColor,   0, 1, "BgColor",   nullptr,
      "Background colour behind the wireframe (cleared before geometry replay)" },

    { "Depth Range",    DUST_SETTING_SECTION, nullptr, 0, 0, nullptr, nullptr, nullptr },
    { "Depth Near",     DUST_SETTING_FLOAT, &gDepthMin, 0, 500,  "DepthMin", nullptr,
      "Linear depth value shown as black in the Depth mode" },
    { "Depth Far",      DUST_SETTING_FLOAT, &gDepthMax, 1, 2000, "DepthMax", nullptr,
      "Linear depth value shown as white in the Depth mode" },
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;
    memset(desc, 0, sizeof(*desc));

    desc->apiVersion          = DUST_API_VERSION;
    desc->name                = "Debug Views";
    desc->injectionPoint      = DUST_INJECT_POST_TONEMAP; // before UI, after all scene effects
    desc->priority            = 9999;                     // absolutely last at POST_TONEMAP
    desc->Init                = DebugViewsInit;
    desc->Shutdown            = DebugViewsShutdown;
    desc->OnResolutionChanged = DebugViewsOnResolutionChanged;
    desc->postExecute         = DebugViewsPost;
    desc->IsEnabled           = DebugViewsIsEnabled;
    desc->settings            = gSettings;
    desc->settingCount        = sizeof(gSettings) / sizeof(gSettings[0]);
    desc->flags               = DUST_FLAG_FRAMEWORK_CONFIG | DUST_FLAG_FRAMEWORK_TIMING;
    desc->configSection       = "DebugViews";

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        gPluginModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
