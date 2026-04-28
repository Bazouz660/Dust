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
#include <cmath>
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
static ID3D11DepthStencilState* gWireDSSReadOnly= nullptr;   // depth test only (wireframe pass)
static ID3D11BlendState*        gNoColorWrite   = nullptr;   // depth pre-pass: skip color
static ID3D11Texture2D*         gWireDepthTex   = nullptr;   // dedicated DSV for wireframe
static ID3D11DepthStencilView*  gWireDSV        = nullptr;

// Shared pipeline state
static ID3D11RasterizerState*   gSolidRS        = nullptr;   // FILL_SOLID, CULL_NONE
static ID3D11DepthStencilState* gNoDSS          = nullptr;   // no depth test/write
static ID3D11BlendState*        gNoBlend        = nullptr;
static ID3D11SamplerState*      gPointSamp      = nullptr;
static ID3D11SamplerState*      gLinearSamp     = nullptr;

static UINT gWireDepthW = 0, gWireDepthH = 0;

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

// Wireframe PS CB. Two float4s — avoids any ambiguity in HLSL register packing
// for the float/float/float2 mix the previous layout used.
struct alignas(16) WireCBData
{
    float color[4];       // rgba
    float fadeParams[4];  // x = fadeRange (fraction of farClip), y = minBrightness, zw = 1/viewport
};

// ── Settings ───────────────────────────────────────────────────────────────

static int   gViewMode          = 0;
static float gWireColor[3]      = { 0.0f, 1.0f, 0.0f };   // green wire
static float gBgColor[3]        = { 0.05f, 0.05f, 0.05f }; // near-black background
static float gWireFadeRange     = 0.10f;  // fraction of farClip; smaller = fade onset closer
static float gWireMinBrightness = 0.15f;
static bool  gWireSolidOcclusion = false; // two-pass: solid depth pre-pass + wireframe; false = single-pass
static float gDepthMin          = 0.0f;
static float gDepthMax          = 1.0f;   // GBuffer depth = length / farClip, already normalised

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

// ── Wireframe render ────────────────────────────────────────────────────────

static void RenderWireframe(ID3D11DeviceContext* ctx, ID3D11Device* device,
                            ID3D11RenderTargetView* ldrRTV,
                            UINT w, UINT h)
{
    // Recreate depth buffer if viewport changed
    if (w != gWireDepthW || h != gWireDepthH)
        CreateWireDepth(device, w, h);
    if (!gWireDSV) return;

    // Clear LDR to background, clear depth
    ctx->ClearRenderTargetView(ldrRTV, gBgColor);
    ctx->ClearDepthStencilView(gWireDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Update wire color + depth-fade CB
    WireCBData wc = {};
    wc.color[0] = gWireColor[0];
    wc.color[1] = gWireColor[1];
    wc.color[2] = gWireColor[2];
    wc.color[3] = 1.0f;
    wc.fadeParams[0] = gWireFadeRange;
    wc.fadeParams[1] = gWireMinBrightness;
    wc.fadeParams[2] = (w > 0) ? 1.0f / (float)w : 0.0f;
    wc.fadeParams[3] = (h > 0) ? 1.0f / (float)h : 0.0f;
    gHost->UpdateConstantBuffer(ctx, gWireCB, &wc, sizeof(wc));

    // Disable predication — Kenshi may have a residual predicate from hardware
    // occlusion culling that would mask out our replayed draws.
    ctx->SetPredication(nullptr, FALSE);

    // Bind output targets
    ctx->OMSetRenderTargets(1, &ldrRTV, gWireDSV);
    D3D11_VIEWPORT vp11 = { 0, 0, (float)w, (float)h, 0, 1 };
    ctx->RSSetViewports(1, &vp11);

    // Null out tessellation + GS so captured draws rasterise straight from VS
    // without picking up stale state from earlier effects.
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);

    float bf[4] = {};

    // Bind PS state once — both passes use the same PS and CB.
    ctx->PSSetShader(gWirePS, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &gWireCB);
    ID3D11ShaderResourceView* depthSRV = gHost->GetSRV(DUST_RESOURCE_DEPTH);
    ctx->PSSetShaderResources(0, 1, &depthSRV);
    ctx->PSSetSamplers(0, 1, &gPointSamp);

    // Diagnostic: dump captured-draw breakdown by category every ~2s so we can see
    // which categories are being captured at all (e.g. is `skin` a non-zero number?)
    // and inspect any draws that landed as UNKNOWN.
    static uint32_t sDiagFrame = 0;
    if ((sDiagFrame++ & 0x7F) == 0)
    {
        uint32_t captured = gHost->GetGeometryDrawCount();
        uint32_t cats[7] = {0};
        uint32_t classified = 0;
        for (uint32_t i = 0; i < captured; i++)
        {
            DustGeometryDraw d;
            if (gHost->GetGeometryDrawInfo(i, &d) != 0) continue;
            int c = (int)d.vsCategory;
            if (c >= 0 && c < 7) cats[c]++;
            if (d.transformType != DUST_TRANSFORM_UNKNOWN) classified++;
        }
        Log("DebugViews: wireframe captured=%u classified=%u  unknown=%u objects=%u terrain=%u foliage=%u skin=%u triplanar=%u distant_town=%u",
            captured, classified, cats[0], cats[1], cats[2], cats[3], cats[4], cats[5], cats[6]);
    }

    // DIAGNOSTIC pass: single-pass wireframe with per-draw category colouring.
    // The pre-draw callback updates the wireCB with a colour keyed to the draw's
    // shader category, so the user can see which categories produce fragments
    // and which don't. Skin = magenta, Objects = white, Terrain = blue, Foliage
    // = green, Triplanar = yellow, Distant town = cyan, Unknown = red.
    //
    // The "Solid Occlusion" toggle is repurposed as a diagnostic switch: ON renders
    // FILL_SOLID instead of FILL_WIREFRAME, so we can see whether missing geometry
    // is a wireframe-rasterisation quirk (objects appear solid but not wired) or
    // a real transform/state problem (still missing in solid mode).
    ctx->RSSetState(gWireSolidOcclusion ? gSolidRS : gWireRS);
    ctx->OMSetDepthStencilState(gWireDSS, 0);
    ctx->OMSetBlendState(gNoBlend, bf, 0xFFFFFFFF);

    struct DiagCtx { ID3D11Buffer* cb; const DustHostAPI* host; uint32_t* brokenCount; };
    static uint32_t sBroken = 0;
    sBroken = 0;
    DiagCtx diag = { gWireCB, gHost, &sBroken };

    auto preDraw = [](ID3D11DeviceContext* c, uint32_t drawIndex,
                      uint32_t /*priorTriangles*/, void* ud)
    {
        DiagCtx* d = static_cast<DiagCtx*>(ud);
        DustGeometryDraw info;
        if (d->host->GetGeometryDrawInfo(drawIndex, &info) != 0) return;

        // Project the object's origin (0,0,0,1) through captured WVP. If it lands behind
        // the camera (w<=0) or absurdly far outside the clip volume, the captured matrix
        // is suspect and we colour the draw bright YELLOW so the user can see which ones.
        bool broken = false;
        const void* cbData = nullptr;
        uint32_t cbSize = 0;
        if (d->host->GetGeometryDrawConstants(c, drawIndex, &cbData, &cbSize) == 0 &&
            cbData && info.clipMatrixOffset + 64 <= cbSize)
        {
            const float* m = (const float*)((const uint8_t*)cbData + info.clipMatrixOffset);
            // Row-major float4x4: M[i][j] at index i*4+j. WVP × (0,0,0,1) = column 3 of M.
            float cx = m[3], cy = m[7], cz = m[11], cw = m[15];
            // NaN test, behind-camera test, far outside clip volume test.
            if (!(cw == cw) || !(cx == cx))                         broken = true; // NaN
            else if (cw <= 0.0001f)                                  broken = true; // behind / near plane
            else if (fabsf(cx) > 100.0f * cw || fabsf(cy) > 100.0f * cw) broken = true; // way off
        }

        WireCBData wc = {};
        wc.color[3] = 1.0f;
        if (info.instanceCount > 1)
        {
            // DIAG: highlight instanced draws bright ORANGE — if missing buildings
            // appear in this colour, the per-instance VB is being shared/overwritten.
            wc.color[0] = 1; wc.color[1] = 0.5f; wc.color[2] = 0;
        }
        else if (broken)
        {
            wc.color[0] = 1; wc.color[1] = 1; wc.color[2] = 0; // bright yellow = bad WVP
            (*d->brokenCount)++;
        }
        else switch (info.vsCategory)
        {
            case DUST_SHADER_OBJECTS:      wc.color[0]=1; wc.color[1]=1; wc.color[2]=1; break; // white
            case DUST_SHADER_TERRAIN:      wc.color[0]=0; wc.color[1]=0.4f; wc.color[2]=1; break; // blue
            case DUST_SHADER_FOLIAGE:      wc.color[0]=0; wc.color[1]=1; wc.color[2]=0; break; // green
            case DUST_SHADER_SKIN:         wc.color[0]=1; wc.color[1]=0; wc.color[2]=1; break; // magenta
            case DUST_SHADER_TRIPLANAR:    wc.color[0]=0.5f; wc.color[1]=0.5f; wc.color[2]=0; break; // dark olive (yellow reserved)
            case DUST_SHADER_DISTANT_TOWN: wc.color[0]=0; wc.color[1]=1; wc.color[2]=1; break; // cyan
            default:                       wc.color[0]=1; wc.color[1]=0; wc.color[2]=0; break; // red = unknown
        }
        d->host->UpdateConstantBuffer(c, d->cb, &wc, sizeof(wc));
        c->PSSetConstantBuffers(0, 1, &d->cb);
    };

    uint32_t actuallyReplayed = 0;
    if (gHost->ReplayGeometryEx)
        actuallyReplayed = gHost->ReplayGeometryEx(ctx, device, nullptr, preDraw, &diag);
    else
        actuallyReplayed = gHost->ReplayGeometry(ctx, device, nullptr);

    static uint32_t sLastReplayedLog = ~0u;
    static uint32_t sLastBrokenLog   = ~0u;
    if (actuallyReplayed != sLastReplayedLog || sBroken != sLastBrokenLog)
    {
        Log("DebugViews: wireframe replayed %u of %u  broken-WVP draws=%u",
            actuallyReplayed, gHost->GetGeometryDrawCount(), sBroken);
        sLastReplayedLog = actuallyReplayed;
        sLastBrokenLog   = sBroken;
    }

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

    // Wireframe solid-colour PS (mode 1).
    // DIAGNOSTIC: read the colour from CB (per-draw callback overrides it per draw)
    // so we can see WHICH shader categories actually produce rasterised fragments.
    // Categories that don't show up in the output are being killed at the rasteriser
    // (degenerate geometry, all-clipped, stale instance VB, etc.).
    static const char* kWirePSSrc = R"(
        cbuffer WireCB : register(b0) {
            float4 color;
            float4 fadeParams;
        };
        float4 main(float4 pos : SV_Position) : SV_Target
        {
            return color;
        }
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

        D3D11_RASTERIZER_DESC rdSolid = {};
        rdSolid.FillMode = D3D11_FILL_SOLID;
        rdSolid.CullMode = D3D11_CULL_NONE;
        rdSolid.DepthClipEnable = TRUE;
        device->CreateRasterizerState(&rdSolid, &gSolidRS);
    }

    // Depth-stencil states
    {
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable    = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc      = D3D11_COMPARISON_LESS;
        device->CreateDepthStencilState(&dd, &gWireDSS);

        // Depth-test-only variant: wireframe edges read but don't write the pre-pass
        // depth so coplanar edges of overlapping triangles all stay visible.
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
        device->CreateDepthStencilState(&dd, &gWireDSSReadOnly);

        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc      = D3D11_COMPARISON_LESS;
        device->CreateDepthStencilState(&dd, &gNoDSS);
    }

    // Blend
    {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bd, &gNoBlend);

        // Depth-only blend state: write nothing to color (used for the solid depth pre-pass).
        bd.RenderTarget[0].RenderTargetWriteMask = 0;
        device->CreateBlendState(&bd, &gNoColorWrite);
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
    SR(gWireDSS);   SR(gWireDSSReadOnly); SR(gNoDSS);
    SR(gNoBlend);   SR(gNoColorWrite);
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
    { "Fade Range",     DUST_SETTING_FLOAT, &gWireFadeRange, 0.001f, 1.0f, "FadeRange", nullptr,
      "Fraction of the camera's far clip distance over which the wireframe fades to its minimum brightness. Smaller = fade onset closer to camera." },
    { "Min Brightness", DUST_SETTING_FLOAT, &gWireMinBrightness, 0, 1, "MinBrightness", nullptr,
      "Brightness floor for distant geometry. 0 = far edges fade to black, 1 = no fade." },
    { "Solid Mode (diag)", DUST_SETTING_BOOL, &gWireSolidOcclusion, 0, 1, "SolidOcclusion", nullptr,
      "DIAGNOSTIC: render FILL_SOLID instead of FILL_WIREFRAME. Use this to check whether geometry "
      "missing in wireframe is a wireframe-rasterisation quirk (would appear solid) or a real "
      "transform/state issue (still missing as solid)." },

    { "Depth Range",    DUST_SETTING_SECTION, nullptr, 0, 0, nullptr, nullptr, nullptr },
    { "Depth Near",     DUST_SETTING_FLOAT, &gDepthMin, 0, 1,    "DepthMin", nullptr,
      "Normalised depth shown as white (near). GBuffer depth is length(worldPos - cameraPos) / farClip in [0,1]." },
    { "Depth Far",      DUST_SETTING_FLOAT, &gDepthMax, 0, 1,    "DepthMax", nullptr,
      "Normalised depth shown as black (far). Lower this value to bring distant detail into the visible range." },
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
