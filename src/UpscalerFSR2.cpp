#include "UpscalerFSR2.h"
#include "DustLog.h"

// DUST_HAVE_FSR2 is defined by the build only when the FSR2-DX11 libs are present. Without it the whole
// backend compiles to no-op stubs (see the #else at the bottom) so the repo still builds.
#ifdef DUST_HAVE_FSR2

#include "ffx_fsr2.h"
#include "dx11/ffx_fsr2_dx11.h"

#include <cstdlib>

namespace UpscalerFSR2
{
static bool             sInited      = false;
static bool             sHaveContext = false;
static ID3D11Device*    sDevice      = nullptr;
static FfxFsr2Context   sContext     = {};
static FfxFsr2ContextDescription sDesc = {};   // holds the interface (points into sScratch) + device
static void*            sScratch     = nullptr;
static uint32_t         sRenderW = 0, sRenderH = 0;

bool Init(ID3D11Device* device)
{
    if (sInited) return sDevice != nullptr;
    sInited = true;
    sDevice = device;
    if (device) Log("Upscaler[FSR2]: initialized (native D3D11 backend)");
    return device != nullptr;   // compute-only — available on any D3D11 GPU
}

bool IsAvailable() { return sInited && sDevice != nullptr; }

bool CreateFeature(ID3D11DeviceContext* ctx, uint32_t renderW, uint32_t renderH,
                   uint32_t displayW, uint32_t displayH, bool isHDR, bool depthInverted, int /*preset*/)
{
    if (!sDevice) return false;
    if (sHaveContext) { ffxFsr2ContextDestroy(&sContext); sHaveContext = false; }
    if (sScratch)     { free(sScratch); sScratch = nullptr; }

    size_t scratchSize = ffxFsr2GetScratchMemorySizeDX11();
    sScratch = malloc(scratchSize);
    if (!sScratch) return false;

    FfxErrorCode err = ffxFsr2GetInterfaceDX11(&sDesc.callbacks, sDevice, sScratch, scratchSize);
    if (err != FFX_OK) { Log("Upscaler[FSR2]: GetInterfaceDX11 failed (%d)", (int)err); free(sScratch); sScratch = nullptr; return false; }

    sDesc.device        = ffxGetDeviceDX11(sDevice);
    sDesc.maxRenderSize = { renderW, renderH };
    sDesc.displaySize   = { displayW, displayH };
    sDesc.flags = FFX_FSR2_ENABLE_AUTO_EXPOSURE;   // we pass no exposure texture
    if (isHDR)         sDesc.flags |= FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;
    if (depthInverted) sDesc.flags |= FFX_FSR2_ENABLE_DEPTH_INVERTED;

    err = ffxFsr2ContextCreate(&sContext, &sDesc);
    if (err != FFX_OK)
    {
        Log("Upscaler[FSR2]: ContextCreate failed (%d)", (int)err);
        free(sScratch); sScratch = nullptr;
        return false;
    }
    sHaveContext = true;
    sRenderW = renderW; sRenderH = renderH;
    Log("Upscaler[FSR2]: context created %ux%u -> %ux%u (isHDR=%d depthInv=%d)",
        renderW, renderH, displayW, displayH, (int)isHDR, (int)depthInverted);
    return true;
}

bool Evaluate(ID3D11DeviceContext* ctx,
              ID3D11Resource* color, ID3D11Resource* depth, ID3D11Resource* motionVectors,
              ID3D11Resource* output, float jitterX, float jitterY,
              float mvScaleX, float mvScaleY, float sharpness, bool reset,
              ID3D11Resource* reactiveMask)
{
    if (!sHaveContext || !ctx || !color || !depth || !motionVectors || !output) return false;

    FfxFsr2DispatchDescription dd = {};
    dd.commandList        = (FfxCommandList)ctx;   // the DX11 backend reinterpret_casts this to the context
    dd.color              = ffxGetResourceDX11(&sContext, color,         L"FSR2_Color",  FFX_RESOURCE_STATE_COMPUTE_READ);
    dd.depth              = ffxGetResourceDX11(&sContext, depth,         L"FSR2_Depth",  FFX_RESOURCE_STATE_COMPUTE_READ);
    dd.motionVectors      = ffxGetResourceDX11(&sContext, motionVectors, L"FSR2_MV",     FFX_RESOURCE_STATE_COMPUTE_READ);
    dd.output             = ffxGetResourceDX11(&sContext, output,        L"FSR2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    if (reactiveMask)
        dd.reactive       = ffxGetResourceDX11(&sContext, reactiveMask,  L"FSR2_React",  FFX_RESOURCE_STATE_COMPUTE_READ);
    dd.jitterOffset       = { jitterX, jitterY };
    dd.motionVectorScale  = { mvScaleX, mvScaleY };
    dd.renderSize         = { sRenderW, sRenderH };
    dd.enableSharpening   = sharpness > 0.0f;
    dd.sharpness          = sharpness;
    dd.frameTimeDelta     = 16.6f;        // ms — a real delta only affects auto-exposure/reactive pacing
    dd.preExposure        = 1.0f;
    dd.reset              = reset;
    dd.cameraNear         = 0.1f;         // TODO: pull real near/far/fov from CameraAccess for best quality
    dd.cameraFar          = 10000.0f;
    dd.cameraFovAngleVertical = 1.0f;     // radians

    FfxErrorCode err = ffxFsr2ContextDispatch(&sContext, &dd);
    if (err != FFX_OK) { Log("Upscaler[FSR2]: Dispatch failed (%d)", (int)err); return false; }
    return true;
}

void Shutdown()
{
    if (sHaveContext) { ffxFsr2ContextDestroy(&sContext); sHaveContext = false; }
    if (sScratch)     { free(sScratch); sScratch = nullptr; }
    sInited = false; sDevice = nullptr;
}
}

#else  // !DUST_HAVE_FSR2 — libs not vendored: safe stubs so the build works without external/FSR2-DX11.

namespace UpscalerFSR2
{
static bool sWarned = false;
bool Init(ID3D11Device*)
{
    if (!sWarned) { sWarned = true; Log("Upscaler[FSR2]: built WITHOUT the FSR2-DX11 libs — FSR2 unavailable"); }
    return false;
}
bool IsAvailable() { return false; }
bool CreateFeature(ID3D11DeviceContext*, uint32_t, uint32_t, uint32_t, uint32_t, bool, bool, int) { return false; }
bool Evaluate(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*, ID3D11Resource*,
              ID3D11Resource*, float, float, float, float, float, bool, ID3D11Resource*) { return false; }
void Shutdown() {}
}

#endif  // DUST_HAVE_FSR2
