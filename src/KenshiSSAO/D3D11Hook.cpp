#include "D3D11Hook.h"
#include "SSAORenderer.h"
#include "SSAOConfig.h"
#include <core/Functions.h>
#include <Debug.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <windows.h>

static void Log(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA("[KenshiSSAO] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    char fullBuf[600];
    snprintf(fullBuf, sizeof(fullBuf), "[KenshiSSAO] %s", buf);
    ::DebugLog(fullBuf);
}

namespace D3D11Hook
{

// ==================== Global state ====================

ID3D11ShaderResourceView* gDepthSRV = nullptr;
ID3D11ShaderResourceView* gNormalsSRV = nullptr;
ID3D11RenderTargetView* gHdrRTV = nullptr;
bool gAoInjectedThisFrame = false;

ID3D11Device* gDevice = nullptr;
ID3D11DeviceContext* gContext = nullptr;
bool gDeviceCaptured = false;

static bool gResetPending = false;

void ResetFrameState()
{
    gAoInjectedThisFrame = false;
    gDepthSRV = nullptr;
    gNormalsSRV = nullptr;
    gHdrRTV = nullptr;
    gResetPending = true;
}

// ==================== Original function pointers ====================

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreatePixelShader)(
    ID3D11Device* pThis, const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader);

typedef void(STDMETHODCALLTYPE* PFN_Draw)(
    ID3D11DeviceContext* pThis, UINT VertexCount, UINT StartVertexLocation);

typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(
    ID3D11DeviceContext* pThis, UINT IndexCount, UINT StartIndexLocation,
    INT BaseVertexLocation);

typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(
    ID3D11DeviceContext* pThis, UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation);

typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(
    ID3D11DeviceContext* pThis, UINT NumViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView);

static PFN_CreatePixelShader        oCreatePixelShader = nullptr;
static PFN_Draw                     oDraw = nullptr;
static PFN_DrawIndexed              oDrawIndexed = nullptr;
static PFN_DrawIndexedInstanced     oDrawIndexedInstanced = nullptr;
static PFN_OMSetRenderTargets       oOMSetRenderTargets = nullptr;

// ==================== Device capture ====================

static void TryCaptureDevice(ID3D11Device* device)
{
    if (gDeviceCaptured)
        return;

    gDeviceCaptured = true;

    gDevice = device;
    device->GetImmediateContext(&gContext);

    Log("Captured real D3D11 device=%p, context=%p", gDevice, gContext);

    UINT width = 0, height = 0;
    {
        ID3D11RenderTargetView* rtv = nullptr;
        gContext->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv)
        {
            ID3D11Resource* res = nullptr;
            rtv->GetResource(&res);
            if (res)
            {
                ID3D11Texture2D* tex = nullptr;
                res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
                if (tex)
                {
                    D3D11_TEXTURE2D_DESC desc;
                    tex->GetDesc(&desc);
                    width = desc.Width;
                    height = desc.Height;
                    tex->Release();
                }
                res->Release();
            }
            rtv->Release();
        }
    }

    if (width == 0 || height == 0)
    {
        width = 1;
        height = 1;
        Log("Resolution not yet available, will detect from HDR RT on first frame");
    }
    else
    {
        Log("Detected resolution: %ux%u", width, height);
    }

    if (!SSAORenderer::Init(gDevice, width, height))
    {
        Log("ERROR: SSAORenderer::Init failed");
        gDeviceCaptured = false;
        return;
    }
    Log("KenshiSSAO fully initialized and active");
}

// ==================== Lighting pass detection ====================
// Detect by pipeline state, not shader hash. Works with any settings.
// The lighting pass is the only fullscreen draw where:
//   1. RT format = R11G11B10_FLOAT (HDR scene target)
//   2. SRV slot 2 = R32_FLOAT (depth buffer)
//   3. SRV slot 0 is bound (GBuffer)

static bool IsLightingPass(ID3D11DeviceContext* ctx)
{
    // Check RT format
    ID3D11RenderTargetView* rt = nullptr;
    ctx->OMGetRenderTargets(1, &rt, nullptr);
    if (!rt)
        return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtDesc;
    rt->GetDesc(&rtDesc);
    rt->Release();

    if (rtDesc.Format != DXGI_FORMAT_R11G11B10_FLOAT)
        return false;

    // Check SRV[2] is R32_FLOAT (depth)
    ID3D11ShaderResourceView* srv2 = nullptr;
    ctx->PSGetShaderResources(2, 1, &srv2);
    if (!srv2)
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    srv2->GetDesc(&srvDesc);
    srv2->Release();

    if (srvDesc.Format != DXGI_FORMAT_R32_FLOAT)
        return false;

    // Check SRV[0] is bound (GBuffer albedo — confirms this is lighting, not some other pass)
    ID3D11ShaderResourceView* srv0 = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv0);
    if (!srv0)
        return false;
    srv0->Release();

    return true;
}

// ==================== Hook implementations ====================

static HRESULT STDMETHODCALLTYPE HookedCreatePixelShader(
    ID3D11Device* pThis, const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader)
{
    if (!gDeviceCaptured)
        TryCaptureDevice(pThis);

    return oCreatePixelShader(pThis, pShaderBytecode, BytecodeLength,
                               pClassLinkage, ppPixelShader);
}

static bool gFirstInjectLogged = false;

// Pipeline survey: log all fullscreen draws for the first few frames
static int gSurveyFrameCount = 0;
static int gSurveyDrawIndex = 0;
static const int SURVEY_FRAMES = 5;

static void SurveyFullscreenDraw(ID3D11DeviceContext* ctx, UINT vertexCount)
{
    if (gSurveyFrameCount >= SURVEY_FRAMES)
        return;

    // Handle frame boundary
    if (gResetPending)
    {
        if (gSurveyDrawIndex > 0)
        {
            Log("SURVEY: Frame %d had %d fullscreen draws", gSurveyFrameCount, gSurveyDrawIndex);
            gSurveyFrameCount++;
        }
        gSurveyDrawIndex = 0;
        gResetPending = false;
        if (gSurveyFrameCount >= SURVEY_FRAMES)
            return;
    }

    // RT info
    ID3D11RenderTargetView* rtvs[4] = {};
    ctx->OMGetRenderTargets(4, rtvs, nullptr);
    int numRTs = 0;
    DXGI_FORMAT rtFormats[4] = {};
    UINT rtWidth = 0, rtHeight = 0;
    for (int i = 0; i < 4; i++)
    {
        if (rtvs[i])
        {
            numRTs = i + 1;
            D3D11_RENDER_TARGET_VIEW_DESC desc;
            rtvs[i]->GetDesc(&desc);
            rtFormats[i] = desc.Format;
            if (i == 0)
            {
                ID3D11Resource* res = nullptr;
                rtvs[i]->GetResource(&res);
                if (res)
                {
                    ID3D11Texture2D* tex = nullptr;
                    res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
                    if (tex)
                    {
                        D3D11_TEXTURE2D_DESC td;
                        tex->GetDesc(&td);
                        rtWidth = td.Width;
                        rtHeight = td.Height;
                        tex->Release();
                    }
                    res->Release();
                }
            }
            rtvs[i]->Release();
        }
    }

    // SRV info (slots 0-7)
    ID3D11ShaderResourceView* srvs[8] = {};
    ctx->PSGetShaderResources(0, 8, srvs);
    char srvInfo[256] = {};
    int pos = 0;
    for (int i = 0; i < 8; i++)
    {
        if (srvs[i])
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC desc;
            srvs[i]->GetDesc(&desc);
            pos += snprintf(srvInfo + pos, sizeof(srvInfo) - pos, " t%d=%u", i, desc.Format);
            srvs[i]->Release();
        }
    }

    // Pixel shader info
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    void* psAddr = ps;
    if (ps) ps->Release();

    Log("SURVEY F%d D%02d: verts=%u RTs=%d rt0fmt=%u %ux%u PS=%p SRVs:[%s]",
        gSurveyFrameCount, gSurveyDrawIndex, vertexCount,
        numRTs, rtFormats[0], rtWidth, rtHeight, psAddr, srvInfo);

    gSurveyDrawIndex++;
}

static void InjectAOAfterLighting(ID3D11DeviceContext* pThis)
{
    // Query current RT (HDR, still bound from lighting draw)
    ID3D11RenderTargetView* currentRT = nullptr;
    pThis->OMGetRenderTargets(1, &currentRT, nullptr);
    if (currentRT)
    {
        gHdrRTV = currentRT;
        currentRT->Release();
    }

    // Query depth SRV from slot 2
    ID3D11ShaderResourceView* depthSRV = nullptr;
    pThis->PSGetShaderResources(2, 1, &depthSRV);
    if (depthSRV)
    {
        gDepthSRV = depthSRV;
        depthSRV->Release();
    }

    if (!gFirstInjectLogged)
    {
        Log("InjectAO (post-lighting, hash-free): depth=%p, hdrRT=%p",
            gDepthSRV, gHdrRTV);
        gFirstInjectLogged = true;
    }

    if (!gDepthSRV || !gHdrRTV)
        return;

    gAoInjectedThisFrame = true;

    SSAORenderer::Inject(pThis, gDepthSRV, gNormalsSRV, gHdrRTV);
}

static void STDMETHODCALLTYPE HookedDraw(
    ID3D11DeviceContext* pThis, UINT VertexCount, UINT StartVertexLocation)
{
    // Execute the original draw first
    oDraw(pThis, VertexCount, StartVertexLocation);

    // Survey all fullscreen draws for pipeline analysis
    if (gDeviceCaptured && (VertexCount == 3 || VertexCount == 4))
        SurveyFullscreenDraw(pThis, VertexCount);

    // Only check fullscreen draws (3 or 4 verts), only once per frame
    if (gAoInjectedThisFrame || !gDeviceCaptured || !gSSAOConfig.enabled)
        return;
    if (VertexCount != 3 && VertexCount != 4)
        return;

    // Detect lighting pass by pipeline state, inject AO right after
    if (IsLightingPass(pThis))
        InjectAOAfterLighting(pThis);
}

static void STDMETHODCALLTYPE HookedDrawIndexed(
    ID3D11DeviceContext* pThis, UINT IndexCount, UINT StartIndexLocation,
    INT BaseVertexLocation)
{
    oDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);
}

static void STDMETHODCALLTYPE HookedDrawIndexedInstanced(
    ID3D11DeviceContext* pThis, UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
    oDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                          StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

static void STDMETHODCALLTYPE HookedOMSetRenderTargets(
    ID3D11DeviceContext* pThis, UINT NumViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView)
{
    oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView);
}

// ==================== Install ====================

static const int VTIDX_DEVICE_CreatePixelShader     = 15;
static const int VTIDX_CTX_DrawIndexed              = 12;
static const int VTIDX_CTX_Draw                     = 13;
static const int VTIDX_CTX_DrawIndexedInstanced     = 20;
static const int VTIDX_CTX_OMSetRenderTargets       = 33;

bool Install()
{
    Log("Creating temporary D3D11 device to discover function addresses...");

    ID3D11Device* tmpDevice = nullptr;
    ID3D11DeviceContext* tmpContext = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &tmpDevice, &featureLevel, &tmpContext);

    if (FAILED(hr))
    {
        Log("ERROR: Failed to create temporary D3D11 device: 0x%08X", hr);
        return false;
    }

    void** devVtable = *reinterpret_cast<void***>(tmpDevice);
    void** ctxVtable = *reinterpret_cast<void***>(tmpContext);

    void* addrCreatePS     = devVtable[VTIDX_DEVICE_CreatePixelShader];
    void* addrDraw         = ctxVtable[VTIDX_CTX_Draw];
    void* addrDrawIndexed  = ctxVtable[VTIDX_CTX_DrawIndexed];
    void* addrDrawIdxInst  = ctxVtable[VTIDX_CTX_DrawIndexedInstanced];
    void* addrOMSetRT      = ctxVtable[VTIDX_CTX_OMSetRenderTargets];

    Log("Function addresses discovered:");
    Log("  CreatePixelShader     = %p", addrCreatePS);
    Log("  Draw                  = %p", addrDraw);
    Log("  DrawIndexed           = %p", addrDrawIndexed);
    Log("  DrawIndexedInstanced  = %p", addrDrawIdxInst);
    Log("  OMSetRenderTargets    = %p", addrOMSetRT);

    tmpContext->Release();
    tmpDevice->Release();

    Log("Installing D3D11 function hooks...");

    bool ok = true;

    if (KenshiLib::AddHook(addrCreatePS, (void*)HookedCreatePixelShader,
                           (void**)&oCreatePixelShader) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook CreatePixelShader"); ok = false; }

    if (KenshiLib::AddHook(addrDraw, (void*)HookedDraw,
                           (void**)&oDraw) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook Draw"); ok = false; }

    if (KenshiLib::AddHook(addrDrawIndexed, (void*)HookedDrawIndexed,
                           (void**)&oDrawIndexed) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook DrawIndexed"); ok = false; }

    if (KenshiLib::AddHook(addrDrawIdxInst, (void*)HookedDrawIndexedInstanced,
                           (void**)&oDrawIndexedInstanced) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook DrawIndexedInstanced"); ok = false; }

    if (KenshiLib::AddHook(addrOMSetRT, (void*)HookedOMSetRenderTargets,
                           (void**)&oOMSetRenderTargets) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook OMSetRenderTargets"); ok = false; }

    if (ok)
        Log("All D3D11 hooks installed successfully (hash-free detection)");

    return ok;
}

} // namespace D3D11Hook
