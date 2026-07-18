#include "UpscalerFSR3.h"
#include "DustLog.h"

#ifndef DUST_HAVE_FSR3
// ---- Stub build (SDK not vendored): every entry point is a safe no-op. ----
namespace UpscalerFSR3
{
    bool Init(ID3D11Device*, const wchar_t*) { return false; }
    bool IsAvailable() { return false; }
    bool CreateFeature(ID3D11DeviceContext*, uint32_t, uint32_t, uint32_t, uint32_t, bool, bool, int) { return false; }
    bool Evaluate(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*, ID3D11Resource*, ID3D11Resource*,
                  float, float, float, float, float, bool, ID3D11Resource*) { return false; }
    void Shutdown() {}
}
#else

#include "D3D12Interop.h"
#include "CameraAccess.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <string>

// ffx-api (FidelityFX SDK). Loaded at runtime via LoadLibrary; we resolve the 5 core entry points with
// GetProcAddress (so no import lib is linked). The DX12 header pulls in d3d12/dxgi and the inline resource
// wrapper ffxApiGetResourceDX12.
#include "ffx_api.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_upscale.h"

namespace UpscalerFSR3
{
namespace
{
    template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

    // --- ffx-api runtime linkage ---
    HMODULE               gProviderDll = nullptr;   // amd_fidelityfx_upscaler_dx12.dll (the FSR backend)
    HMODULE               gLoaderDll   = nullptr;   // amd_fidelityfx_loader_dx12.dll (dispatch shim)
    PfnFfxCreateContext   gCreateContext  = nullptr;
    PfnFfxDestroyContext  gDestroyContext = nullptr;
    PfnFfxConfigure       gConfigure      = nullptr;
    PfnFfxQuery           gQuery          = nullptr;
    PfnFfxDispatch        gDispatch       = nullptr;
    bool                  gLoaded = false;

    // --- FSR context ---
    ffxContext gContext      = nullptr;
    bool       gContextValid = false;
    uint32_t   gRenderW = 0, gRenderH = 0, gDisplayW = 0, gDisplayH = 0;

    // --- marshaling resources (shared inputs game->D3D12, shared output D3D12->game, plus the FFX UAV) ---
    ID3D11Texture2D* gShColor11 = nullptr; ID3D12Resource* gShColor12 = nullptr;
    ID3D11Texture2D* gShDepth11 = nullptr; ID3D12Resource* gShDepth12 = nullptr;
    ID3D11Texture2D* gShMV11    = nullptr; ID3D12Resource* gShMV12    = nullptr;
    ID3D11Texture2D* gShOut11   = nullptr; ID3D12Resource* gShOut12   = nullptr;
    ID3D12Resource*  gFsrOut12  = nullptr;   // D3D12-local UAV: FFX writes here, then copied to gShOut12
    uint32_t         gTexRW = 0, gTexRH = 0, gTexDW = 0, gTexDH = 0;
    DXGI_FORMAT      gColorFmt = DXGI_FORMAT_UNKNOWN, gDepthFmt = DXGI_FORMAT_UNKNOWN,
                     gMVFmt = DXGI_FORMAT_UNKNOWN, gOutFmt = DXGI_FORMAT_UNKNOWN;

    LARGE_INTEGER gQpcFreq = {}, gQpcPrev = {};

    float FrameDeltaMs()
    {
        if (gQpcFreq.QuadPart == 0) QueryPerformanceFrequency(&gQpcFreq);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float ms = 16.7f;
        if (gQpcPrev.QuadPart != 0 && gQpcFreq.QuadPart != 0)
            ms = (float)((now.QuadPart - gQpcPrev.QuadPart) * 1000.0 / gQpcFreq.QuadPart);
        gQpcPrev = now;
        if (ms < 1.0f)   ms = 1.0f;
        if (ms > 200.0f) ms = 200.0f;   // clamp pauses / hitches
        return ms;
    }

    DXGI_FORMAT FormatOf(ID3D11Resource* r)
    {
        ID3D11Texture2D* t = nullptr;
        if (!r) return DXGI_FORMAT_UNKNOWN;
        r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t);
        if (!t) return DXGI_FORMAT_UNKNOWN;
        D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); t->Release();
        return d.Format;
    }

    void ReleaseTextures()
    {
        D3D12Interop::WaitForGpuIdle();   // the D3D12 resources must outlive in-flight GPU work
        SafeRelease(gShColor11); SafeRelease(gShColor12);
        SafeRelease(gShDepth11); SafeRelease(gShDepth12);
        SafeRelease(gShMV11);    SafeRelease(gShMV12);
        SafeRelease(gShOut11);   SafeRelease(gShOut12);
        SafeRelease(gFsrOut12);
        gTexRW = gTexRH = gTexDW = gTexDH = 0;
    }

    // D3D12-local UAV texture that FFX writes its result into (kept off the shared textures, which carry no
    // UAV bind). Copied into the shared output afterwards.
    bool CreateLocalUav(uint32_t w, uint32_t h, DXGI_FORMAT fmt)
    {
        ID3D12Device* dev = D3D12Interop::GetDevice();
        if (!dev) return false;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = w;
        rd.Height           = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = fmt;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                        D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)&gFsrOut12);
        if (FAILED(hr)) { Log("FSR3: CreateCommittedResource(fsrOut) failed 0x%08X", hr); return false; }
        return true;
    }

    bool EnsureTextures(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh,
                        DXGI_FORMAT colorFmt, DXGI_FORMAT depthFmt, DXGI_FORMAT mvFmt, DXGI_FORMAT outFmt)
    {
        if (gShColor11 && rw == gTexRW && rh == gTexRH && dw == gTexDW && dh == gTexDH &&
            colorFmt == gColorFmt && depthFmt == gDepthFmt && mvFmt == gMVFmt && outFmt == gOutFmt)
            return true;
        ReleaseTextures();

        bool ok =
            D3D12Interop::CreateSharedTexture(rw, rh, colorFmt, &gShColor11, &gShColor12) &&
            D3D12Interop::CreateSharedTexture(rw, rh, depthFmt, &gShDepth11, &gShDepth12) &&
            D3D12Interop::CreateSharedTexture(rw, rh, mvFmt,    &gShMV11,    &gShMV12)    &&
            D3D12Interop::CreateSharedTexture(dw, dh, outFmt,   &gShOut11,   &gShOut12)   &&
            CreateLocalUav(dw, dh, outFmt);
        if (!ok) { ReleaseTextures(); return false; }

        gTexRW = rw; gTexRH = rh; gTexDW = dw; gTexDH = dh;
        gColorFmt = colorFmt; gDepthFmt = depthFmt; gMVFmt = mvFmt; gOutFmt = outFmt;
        Log("FSR3: marshaling textures ready (render %ux%u -> display %ux%u)", rw, rh, dw, dh);
        return true;
    }
}

bool IsAvailable() { return gLoaded; }

bool Init(ID3D11Device* device, const wchar_t* modDir)
{
    if (gLoaded) return true;
    if (!device || !modDir) return false;

    // FSR3/FSR4 run on the D3D12 side-device — bring it up if it isn't already.
    if (!D3D12Interop::IsReady() && !D3D12Interop::Init(device))
    {
        Log("FSR3: D3D12 side-device unavailable — FSR3 disabled");
        return false;
    }

    std::wstring dir(modDir);
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';

    // Preload the provider by FULL PATH so the loader's bare LoadLibrary("amd_fidelityfx_upscaler_dx12.dll")
    // resolves to our copy (the mod dir isn't on the default DLL search path; the game exe runs elsewhere).
    gProviderDll = LoadLibraryW((dir + L"amd_fidelityfx_upscaler_dx12.dll").c_str());
    if (!gProviderDll) { Log("FSR3: LoadLibrary amd_fidelityfx_upscaler_dx12.dll failed (err %lu)", GetLastError()); return false; }
    gLoaderDll = LoadLibraryW((dir + L"amd_fidelityfx_loader_dx12.dll").c_str());
    if (!gLoaderDll)
    {
        Log("FSR3: LoadLibrary amd_fidelityfx_loader_dx12.dll failed (err %lu)", GetLastError());
        FreeLibrary(gProviderDll); gProviderDll = nullptr;   // unwind so an Init retry starts clean
        return false;
    }

    gCreateContext  = (PfnFfxCreateContext) GetProcAddress(gLoaderDll, "ffxCreateContext");
    gDestroyContext = (PfnFfxDestroyContext)GetProcAddress(gLoaderDll, "ffxDestroyContext");
    gConfigure      = (PfnFfxConfigure)     GetProcAddress(gLoaderDll, "ffxConfigure");
    gQuery          = (PfnFfxQuery)         GetProcAddress(gLoaderDll, "ffxQuery");
    gDispatch       = (PfnFfxDispatch)      GetProcAddress(gLoaderDll, "ffxDispatch");
    if (!gCreateContext || !gDestroyContext || !gDispatch)
    {
        Log("FSR3: ffx-api entry points missing from loader DLL");
        FreeLibrary(gLoaderDll);   gLoaderDll   = nullptr;   // unwind both loads (see above)
        FreeLibrary(gProviderDll); gProviderDll = nullptr;
        gCreateContext = nullptr; gDestroyContext = nullptr; gConfigure = nullptr; gQuery = nullptr; gDispatch = nullptr;
        return false;
    }

    gLoaded = true;
    Log("FSR3: ffx-api loaded (loader + upscaler DLLs from mod dir)");
    return true;
}

bool CreateFeature(ID3D11DeviceContext* /*ctx*/, uint32_t renderW, uint32_t renderH,
                   uint32_t displayW, uint32_t displayH, bool isHDR, bool depthInverted, int /*preset*/)
{
    if (!gLoaded) return false;
    ID3D12Device* dev = D3D12Interop::GetDevice();
    if (!dev) return false;

    if (gContextValid) { D3D12Interop::WaitForGpuIdle(); gDestroyContext(&gContext, nullptr); gContextValid = false; gContext = nullptr; }

    // Chain: upscale-create desc -> DX12 backend desc (device).
    ffxCreateBackendDX12Desc backendDesc = {};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.device      = dev;

    ffxCreateContextDescUpscale createDesc = {};
    createDesc.header.type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    createDesc.header.pNext = &backendDesc.header;
    createDesc.flags        = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;   // no exposure texture supplied
    if (isHDR)         createDesc.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (depthInverted) createDesc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    createDesc.maxRenderSize  = { renderW, renderH };
    createDesc.maxUpscaleSize = { displayW, displayH };
    createDesc.fpMessage      = nullptr;

    ffxReturnCode_t rc = gCreateContext(&gContext, &createDesc.header, nullptr);
    if (rc != FFX_API_RETURN_OK) { Log("FSR3: ffxCreateContext failed (%u)", rc); return false; }

    gContextValid = true;
    gRenderW = renderW; gRenderH = renderH; gDisplayW = displayW; gDisplayH = displayH;
    Log("FSR3: context created %ux%u -> %ux%u (isHDR=%d depthInv=%d)", renderW, renderH, displayW, displayH, (int)isHDR, (int)depthInverted);
    return true;
}

bool Evaluate(ID3D11DeviceContext* ctx,
              ID3D11Resource* color, ID3D11Resource* depth, ID3D11Resource* motionVectors,
              ID3D11Resource* output, float jitterX, float jitterY,
              float mvScaleX, float mvScaleY, float sharpness, bool reset,
              ID3D11Resource* /*reactiveMask*/)
{
    if (!gLoaded || !gContextValid || !ctx || !color || !depth || !motionVectors || !output) return false;

    DXGI_FORMAT colorFmt = FormatOf(color), depthFmt = FormatOf(depth),
                mvFmt = FormatOf(motionVectors), outFmt = FormatOf(output);
    if (!EnsureTextures(gRenderW, gRenderH, gDisplayW, gDisplayH, colorFmt, depthFmt, mvFmt, outFmt))
        return false;

    // Marshal the game inputs into the shared textures, then run the fence-synced D3D12 bracket.
    ctx->CopyResource(gShColor11, color);
    ctx->CopyResource(gShDepth11, depth);
    ctx->CopyResource(gShMV11,    motionVectors);

    ID3D12GraphicsCommandList* list = D3D12Interop::BeginD3D12Work(ctx);
    if (!list) return false;

    using D3D12Interop::Transition;
    // The shared textures arrive in COMMON (the cross-API handoff state) and gFsrOut12 is left in COMMON
    // too. Declare COMMON to FFX and let it do its own barriers, restoring everything to COMMON on exit —
    // this avoids guessing FFX's internal states and the resulting D3D12 debug-layer mismatches.
    ffxDispatchDescUpscale dd = {};
    dd.header.type    = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList    = list;
    dd.color          = ffxApiGetResourceDX12(gShColor12, FFX_API_RESOURCE_STATE_COMMON);
    dd.depth          = ffxApiGetResourceDX12(gShDepth12, FFX_API_RESOURCE_STATE_COMMON);
    dd.motionVectors  = ffxApiGetResourceDX12(gShMV12,    FFX_API_RESOURCE_STATE_COMMON);
    dd.output         = ffxApiGetResourceDX12(gFsrOut12,  FFX_API_RESOURCE_STATE_COMMON);
    dd.jitterOffset       = { jitterX, jitterY };
    dd.motionVectorScale  = { mvScaleX, mvScaleY };
    dd.renderSize         = { gRenderW, gRenderH };
    dd.upscaleSize        = { gDisplayW, gDisplayH };
    dd.enableSharpening   = sharpness > 0.0f;
    dd.sharpness          = sharpness;
    dd.frameTimeDelta     = FrameDeltaMs();
    dd.preExposure        = 1.0f;
    dd.reset              = reset;
    // Real camera near/far/fov feed FFX's disocclusion math; CameraAccess_GetCameraParams leaves
    // these untouched when the camera isn't reachable (launch/loading screens), so seed the same
    // safe defaults FSR2 uses — zero near/far/fov produces garbage frames.
    float camN = 0.1f, camF = 10000.0f, camFov = 1.0f;
    CameraAccess_GetCameraParams(&camN, &camF, &camFov);
    dd.cameraNear              = camN;
    dd.cameraFar               = camF;
    dd.cameraFovAngleVertical  = camFov;
    dd.viewSpaceToMetersFactor = 0.0f;
    dd.flags                   = 0;

    ffxReturnCode_t rc = gDispatch(&gContext, &dd.header);

    // FFX restored all resources to COMMON. Copy its output into the shared output for the game, then
    // return both to COMMON for the next frame's cross-API handoff.
    Transition(gFsrOut12, D3D12_RESOURCE_STATE_COMMON,      D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(gShOut12,  D3D12_RESOURCE_STATE_COMMON,      D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(gShOut12, gFsrOut12);
    Transition(gFsrOut12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Transition(gShOut12,  D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_COMMON);

    if (!D3D12Interop::SubmitD3D12Work(ctx)) return false;

    if (rc != FFX_API_RETURN_OK)
    {
        static bool logged = false;
        if (!logged) { logged = true; Log("FSR3: ffxDispatch failed (%u)", rc); }
        return false;
    }

    ctx->CopyResource(output, gShOut11);   // upscaled result back into the game's output texture
    return true;
}

void Shutdown()
{
    D3D12Interop::WaitForGpuIdle();   // same in-flight-work hazard as the context recreate above
    if (gContextValid && gDestroyContext) { gDestroyContext(&gContext, nullptr); }
    gContextValid = false; gContext = nullptr;
    ReleaseTextures();
    if (gLoaderDll)   { FreeLibrary(gLoaderDll);   gLoaderDll = nullptr; }
    if (gProviderDll) { FreeLibrary(gProviderDll); gProviderDll = nullptr; }
    gCreateContext = nullptr; gDestroyContext = nullptr; gConfigure = nullptr; gQuery = nullptr; gDispatch = nullptr;
    gLoaded = false;
}

} // namespace UpscalerFSR3

#endif // DUST_HAVE_FSR3
