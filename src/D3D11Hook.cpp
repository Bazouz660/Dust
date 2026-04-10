#include "D3D11Hook.h"
#include "DustGUI.h"
#include "PipelineDetector.h"
#include "EffectLoader.h"
#include "ResourceRegistry.h"
#include "DustLog.h"
#include <core/Functions.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <string>
#include <cstring>

namespace D3D11Hook
{

// ==================== Global state ====================

ID3D11Device* gDevice = nullptr;
ID3D11DeviceContext* gContext = nullptr;
bool gDeviceCaptured = false;

static UINT gWidth = 0;
static UINT gHeight = 0;
static uint64_t gFrameIndex = 0;
static bool gDispatchedThisFrame = false;

#ifdef DUST_SURVEY
static bool gResetPending = false;
#endif

void ResetFrameState()
{
    gPipelineDetector.ResetFrame();
    gResourceRegistry.ResetFrame();
    gDispatchedThisFrame = false;
    gFrameIndex++;
#ifdef DUST_SURVEY
    gResetPending = true;
#endif
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

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(
    IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags);

typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(
    IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags);

static PFN_CreatePixelShader        oCreatePixelShader = nullptr;
static PFN_Draw                     oDraw = nullptr;
static PFN_DrawIndexed              oDrawIndexed = nullptr;
static PFN_DrawIndexedInstanced     oDrawIndexedInstanced = nullptr;
static PFN_OMSetRenderTargets       oOMSetRenderTargets = nullptr;
static PFN_Present                  oPresent = nullptr;
static PFN_ResizeBuffers            oResizeBuffers = nullptr;

// ==================== D3DCompile hook (runtime shader patching) ====================

typedef HRESULT(WINAPI* PFN_D3DCompileHook)(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget,
    UINT Flags1, UINT Flags2,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);

static PFN_D3DCompileHook oD3DCompile = nullptr;

// Patch vanilla deferred.hlsl source to add AO support.
// Returns the modified source, or the original if patterns weren't found.
static std::string PatchDeferredShader(const std::string& src)
{
    std::string result = src;

    // Injection 1: Add aoMap + aoParams sampler declarations.
    // Anchor: "uniform float4 ambientParams," exists in all variants.
    const char* anchor1 = "uniform float4 ambientParams,";
    size_t pos1 = result.find(anchor1);
    if (pos1 == std::string::npos)
    {
        Log("ShaderPatch: anchor 'ambientParams' not found, skipping");
        return src;
    }

    std::string inject1 =
        "uniform sampler aoMap : register(s8),\n"
        "\tuniform sampler aoParams : register(s9),\n\n\t";
    result.insert(pos1, inject1);

    // Injection 2: Add AO application code.
    // Anchor: "LightingData ld = (LightingData)0.0f;" — right after env light calculation.
    const char* anchor2 = "LightingData ld = (LightingData)0.0f;";
    size_t pos2 = result.find(anchor2);
    if (pos2 == std::string::npos)
    {
        Log("ShaderPatch: anchor 'LightingData ld' not found, skipping");
        return src;
    }

    std::string inject2 =
        "// [Dust] Ambient occlusion\n"
        "\tfloat ao = tex2D(aoMap, texCoord).r;\n"
        "\tfloat directAO = tex2D(aoParams, texCoord).r;\n"
        "\tenvLight.diffuse *= ao;\n"
        "\tenvLight.specular *= ao;\n"
        "\tfloat directFade = lerp(1.0, ao, directAO);\n"
        "\tsunLight.diffuse *= directFade;\n"
        "\tsunLight.specular *= directFade;\n\n\t";
    result.insert(pos2, inject2);

    return result;
}

static HRESULT WINAPI HookedD3DCompile(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget,
    UINT Flags1, UINT Flags2,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs)
{
    // Detect the deferred lighting pixel shader: entry point is "main_fs"
    // and source contains deferred-specific identifiers.
    if (pEntrypoint && pSrcData && SrcDataSize > 0 &&
        strcmp(pEntrypoint, "main_fs") == 0)
    {
        std::string src((const char*)pSrcData, SrcDataSize);
        if (src.find("CalcEnvironmentLight") != std::string::npos &&
            src.find("aoMap") == std::string::npos)  // not already patched
        {
            std::string patched = PatchDeferredShader(src);
            if (patched.size() != src.size())
            {
                Log("ShaderPatch: patched deferred main_fs (%zu -> %zu bytes)",
                    src.size(), patched.size());
                return oD3DCompile(patched.c_str(), patched.size(), pSourceName,
                                    pDefines, pInclude, pEntrypoint, pTarget,
                                    Flags1, Flags2, ppCode, ppErrorMsgs);
            }
        }
    }

    return oD3DCompile(pSrcData, SrcDataSize, pSourceName,
                        pDefines, pInclude, pEntrypoint, pTarget,
                        Flags1, Flags2, ppCode, ppErrorMsgs);
}

// ==================== Device capture ====================

static void TryCaptureDevice(ID3D11Device* device)
{
    if (gDeviceCaptured)
        return;

    gDeviceCaptured = true;

    gDevice = device;
    device->GetImmediateContext(&gContext);

    Log("Captured real D3D11 device=%p, context=%p", gDevice, gContext);

    // Try to get resolution from current RT
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
                    gWidth = desc.Width;
                    gHeight = desc.Height;
                    tex->Release();
                }
                res->Release();
            }
            rtv->Release();
        }
    }

    if (gWidth == 0 || gHeight == 0)
    {
        gWidth = 1;
        gHeight = 1;
        Log("Resolution not yet available, will detect from HDR RT on first frame");
    }
    else
    {
        Log("Detected resolution: %ux%u", gWidth, gHeight);
    }

    // Initialize all loaded effect plugins
    if (!gEffectLoader.InitAll(gDevice, gWidth, gHeight))
        Log("WARNING: One or more effect plugins failed to initialize");

    Log("Dust fully initialized and active");
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

#ifdef DUST_SURVEY
// Pipeline survey: log all fullscreen draws for the first few frames
static int gSurveyFrameCount = 0;
static int gSurveyDrawIndex = 0;
static const int SURVEY_FRAMES = 5;

static void SurveyFullscreenDraw(ID3D11DeviceContext* ctx, UINT vertexCount)
{
    if (gSurveyFrameCount >= SURVEY_FRAMES)
        return;

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

    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    void* psAddr = ps;
    if (ps) ps->Release();

    Log("SURVEY F%d D%02d: verts=%u RTs=%d rt0fmt=%u %ux%u PS=%p SRVs:[%s]",
        gSurveyFrameCount, gSurveyDrawIndex, vertexCount,
        numRTs, rtFormats[0], rtWidth, rtHeight, psAddr, srvInfo);

    gSurveyDrawIndex++;
}
#endif

static void STDMETHODCALLTYPE HookedDraw(
    ID3D11DeviceContext* pThis, UINT VertexCount, UINT StartVertexLocation)
{
    if (!gDeviceCaptured || (VertexCount != 3 && VertexCount != 4))
    {
        oDraw(pThis, VertexCount, StartVertexLocation);
        return;
    }

#ifdef DUST_SURVEY
    SurveyFullscreenDraw(pThis, VertexCount);
#endif

    // Detect render pass from GPU state
    auto result = gPipelineDetector.OnFullscreenDraw(pThis);

    if (result.detected)
    {
        // Detect real resolution from the HDR render target
        ID3D11RenderTargetView* rtv = nullptr;
        pThis->OMGetRenderTargets(1, &rtv, nullptr);
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
                    if (desc.Width != gWidth || desc.Height != gHeight)
                    {
                        Log("Resolution changed: %ux%u -> %ux%u", gWidth, gHeight, desc.Width, desc.Height);
                        gWidth = desc.Width;
                        gHeight = desc.Height;
                        gEffectLoader.OnResolutionChanged(gDevice, gWidth, gHeight);
                    }
                    tex->Release();
                }
                res->Release();
            }
            rtv->Release();
        }

        DustInjectionPoint dip = static_cast<DustInjectionPoint>(result.point);

        DustFrameContext fctx = {};
        fctx.device = gDevice;
        fctx.context = pThis;
        fctx.point = dip;
        fctx.width = gWidth;
        fctx.height = gHeight;
        fctx.frameIndex = gFrameIndex;

        // PRE: effects bind resources before the game's draw
        fctx.timing = DUST_TIMING_PRE;
        gEffectLoader.DispatchPre(dip, &fctx);

        // Execute the game's original draw call
        oDraw(pThis, VertexCount, StartVertexLocation);

        // POST: effects that operate after the draw
        fctx.timing = DUST_TIMING_POST;
        gEffectLoader.DispatchPost(dip, &fctx);
    }
    else
    {
        oDraw(pThis, VertexCount, StartVertexLocation);
    }
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

// ==================== Swap chain hooks (ImGui) ====================

static HRESULT STDMETHODCALLTYPE HookedPresent(
    IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags)
{
    static bool guiInitAttempted = false;
    if (!guiInitAttempted && gDeviceCaptured)
    {
        guiInitAttempted = true;
        DustGUI::Init(pThis, gDevice, gContext);
    }

    DustGUI::Render();

    return oPresent(pThis, SyncInterval, Flags);
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    // ImGui holds a reference to the back buffer RTV — must shut down before resize
    DustGUI::Shutdown();

    HRESULT hr = oResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    // Re-init ImGui with the new back buffer
    if (SUCCEEDED(hr) && gDeviceCaptured)
        DustGUI::Init(pThis, gDevice, gContext);

    return hr;
}

// ==================== Install ====================

static const int VTIDX_DEVICE_CreatePixelShader     = 15;
static const int VTIDX_CTX_DrawIndexed              = 12;
static const int VTIDX_CTX_Draw                     = 13;
static const int VTIDX_CTX_DrawIndexedInstanced     = 20;
static const int VTIDX_CTX_OMSetRenderTargets       = 33;
static const int VTIDX_SC_Present                   = 8;
static const int VTIDX_SC_ResizeBuffers             = 13;

bool Install()
{
    Log("Creating temporary D3D11 device + swap chain to discover function addresses...");

    // Need a dummy window for the swap chain
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.lpszClassName = "DustDummy";
    wc.hInstance = GetModuleHandleA(nullptr);
    RegisterClassExA(&wc);
    HWND dummyWnd = CreateWindowExA(0, "DustDummy", "", WS_OVERLAPPED,
                                     0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width = 1;
    scd.BufferDesc.Height = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummyWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    IDXGISwapChain* tmpSwapChain = nullptr;
    ID3D11Device* tmpDevice = nullptr;
    ID3D11DeviceContext* tmpContext = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &tmpSwapChain, &tmpDevice, nullptr, &tmpContext);

    if (FAILED(hr))
    {
        Log("ERROR: Failed to create temporary D3D11 device: 0x%08X", hr);
        DestroyWindow(dummyWnd);
        UnregisterClassA("DustDummy", wc.hInstance);
        return false;
    }

    void** devVtable = *reinterpret_cast<void***>(tmpDevice);
    void** ctxVtable = *reinterpret_cast<void***>(tmpContext);
    void** scVtable  = *reinterpret_cast<void***>(tmpSwapChain);

    void* addrCreatePS     = devVtable[VTIDX_DEVICE_CreatePixelShader];
    void* addrDraw         = ctxVtable[VTIDX_CTX_Draw];
    void* addrDrawIndexed  = ctxVtable[VTIDX_CTX_DrawIndexed];
    void* addrDrawIdxInst  = ctxVtable[VTIDX_CTX_DrawIndexedInstanced];
    void* addrOMSetRT      = ctxVtable[VTIDX_CTX_OMSetRenderTargets];
    void* addrPresent      = scVtable[VTIDX_SC_Present];
    void* addrResizeBuf    = scVtable[VTIDX_SC_ResizeBuffers];

    Log("Function addresses discovered:");
    Log("  CreatePixelShader     = %p", addrCreatePS);
    Log("  Draw                  = %p", addrDraw);
    Log("  DrawIndexed           = %p", addrDrawIndexed);
    Log("  DrawIndexedInstanced  = %p", addrDrawIdxInst);
    Log("  OMSetRenderTargets    = %p", addrOMSetRT);
    Log("  Present               = %p", addrPresent);
    Log("  ResizeBuffers         = %p", addrResizeBuf);

    tmpSwapChain->Release();
    tmpContext->Release();
    tmpDevice->Release();
    DestroyWindow(dummyWnd);
    UnregisterClassA("DustDummy", wc.hInstance);

    // Hook D3DCompile for runtime shader patching (no disk writes).
    // Kenshi ships D3DCompiler_43.dll (used by OGRE's RenderSystem_Direct3D11).
    // Must hook before OGRE compiles any shaders.
    Log("Installing D3D11 function hooks...");

    bool ok = true;

    {
        const char* compilerDlls[] = { "D3DCompiler_43.dll", "d3dcompiler_47.dll" };
        bool hooked = false;
        for (const char* dllName : compilerDlls)
        {
            // GetModuleHandle first (already loaded by RenderSystem), LoadLibrary as fallback
            HMODULE hD3DCompiler = GetModuleHandleA(dllName);
            if (!hD3DCompiler)
                hD3DCompiler = LoadLibraryA(dllName);
            if (!hD3DCompiler)
                continue;

            void* addrD3DCompile = (void*)GetProcAddress(hD3DCompiler, "D3DCompile");
            if (!addrD3DCompile)
                continue;

            if (KenshiLib::AddHook(addrD3DCompile, (void*)HookedD3DCompile,
                                   (void**)&oD3DCompile) == KenshiLib::SUCCESS)
            {
                Log("  D3DCompile hook installed via %s (runtime shader patching enabled)", dllName);
                hooked = true;
                break;
            }
        }
        if (!hooked)
        { Log("WARNING: Could not hook D3DCompile, shader patching disabled"); }
    }

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

    if (KenshiLib::AddHook(addrPresent, (void*)HookedPresent,
                           (void**)&oPresent) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook Present"); ok = false; }

    if (KenshiLib::AddHook(addrResizeBuf, (void*)HookedResizeBuffers,
                           (void**)&oResizeBuffers) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook ResizeBuffers"); ok = false; }

    if (ok)
        Log("All D3D11 hooks installed successfully (including Present + ResizeBuffers)");

    return ok;
}

} // namespace D3D11Hook
