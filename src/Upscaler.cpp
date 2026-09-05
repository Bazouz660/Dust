#include "Upscaler.h"
#include "DustLog.h"

// DUST_HAVE_DLSS is defined by the build only when external/DLSS (the NGX SDK) is present. Without it
// the whole backend compiles to no-op stubs (see the #else at the bottom) so the repo still builds.
#ifdef DUST_HAVE_DLSS

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <string>

namespace Upscaler
{
static bool                 sInited     = false;
static bool                 sAvailable  = false;
static NVSDK_NGX_Parameter* sParams     = nullptr;   // capability params (also carry the optimal-settings callback)
static NVSDK_NGX_Handle*    sFeature    = nullptr;
static ID3D11Device*        sDevice     = nullptr;
static uint32_t             sRenderW = 0, sRenderH = 0, sDisplayW = 0, sDisplayH = 0;

// A stable project GUID + CUSTOM engine — the documented path for apps without an NVIDIA-issued
// Application ID. DLSS still runs; it just forgoes per-title driver tuning.
static const char* kProjectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";

bool Init(ID3D11Device* device, const wchar_t* dllSearchPath, const wchar_t* appDataPath)
{
    if (sInited) return sAvailable;
    sInited = true;
    sDevice = device;
    if (!device) return false;

    // Point NGX at our shipped nvngx_dlss.dll so DLSS loads without the app being allowlisted.
    static std::wstring sPath = dllSearchPath ? dllSearchPath : L"";
    const wchar_t* paths[1] = { sPath.c_str() };
    NVSDK_NGX_FeatureCommonInfo commonInfo = {};
    commonInfo.PathListInfo.Path   = paths;
    commonInfo.PathListInfo.Length = sPath.empty() ? 0 : 1;

    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0.0",
        appDataPath ? appDataPath : L".", device, &commonInfo);
    if (NVSDK_NGX_FAILED(r))
    {
        Log("Upscaler[DLSS]: NGX init failed (0x%08x) — DLSS unavailable (non-NVIDIA / driver / missing dll)", r);
        return false;
    }

    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&sParams);
    if (NVSDK_NGX_FAILED(r) || !sParams)
    {
        Log("Upscaler[DLSS]: GetCapabilityParameters failed (0x%08x)", r);
        NVSDK_NGX_D3D11_Shutdown1(device);
        return false;
    }

    int available = 0;
    NVSDK_NGX_Parameter_GetI(sParams, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available)
    {
        int initResult = 0, needsDriver = 0;
        NVSDK_NGX_Parameter_GetI(sParams, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
        NVSDK_NGX_Parameter_GetI(sParams, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
        Log("Upscaler[DLSS]: DLSS-SR not available (initResult=0x%08x needsUpdatedDriver=%d)", initResult, needsDriver);
        // Same unwind as the GetCapabilityParameters failure above.
        NVSDK_NGX_D3D11_DestroyParameters(sParams); sParams = nullptr;
        NVSDK_NGX_D3D11_Shutdown1(device);
        return false;
    }

    sAvailable = true;
    Log("Upscaler[DLSS]: NGX initialized; DLSS super-resolution AVAILABLE");
    return true;
}

bool IsAvailable() { return sAvailable; }

static const char* kPresetLabels[] = { "Default", "K (transformer, blurs when static)", "J", "F (CNN, recommended)", "E (CNN)" };
const char* const* PresetLabels() { return kPresetLabels; }

static unsigned int PresetToNgx(int p)
{
    switch (p)
    {
        case PRESET_K: return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
        case PRESET_J: return NVSDK_NGX_DLSS_Hint_Render_Preset_J;
        case PRESET_F: return NVSDK_NGX_DLSS_Hint_Render_Preset_F;
        case PRESET_E: return NVSDK_NGX_DLSS_Hint_Render_Preset_E;
        default:       return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    }
}

bool CreateFeature(ID3D11DeviceContext* ctx, uint32_t renderW, uint32_t renderH,
                   uint32_t displayW, uint32_t displayH, bool isHDR, bool depthInverted, int preset)
{
    if (!sAvailable || !ctx || !sParams) return false;
    if (sFeature) { NVSDK_NGX_D3D11_ReleaseFeature(sFeature); sFeature = nullptr; }

    // Query optimal render size for the chosen quality. For DLAA it returns render == display.
    unsigned int optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0; float sharpness = 0.0f;
    NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        sParams, displayW, displayH, NVSDK_NGX_PerfQuality_Value_DLAA,
        &optW, &optH, &maxW, &maxH, &minW, &minH, &sharpness);
    if (NVSDK_NGX_FAILED(r)) { Log("Upscaler[DLSS]: GetOptimalSettings failed (0x%08x)", r); return false; }

    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;         // MVs supplied at (low) render res
    if (isHDR)         flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (depthInverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

    // Set the render preset before feature create (recreate to change). Default is F (CNN DLAA): the
    // transformer presets K/J blur the whole image when the scene is static in this D3D11 NGX integration.
    NVSDK_NGX_Parameter_SetUI(sParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, PresetToNgx(preset));

    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth            = renderW;
    cp.Feature.InHeight           = renderH;
    cp.Feature.InTargetWidth      = displayW;
    cp.Feature.InTargetHeight     = displayH;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    cp.InFeatureCreateFlags       = flags;

    r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &sFeature, sParams, &cp);
    if (NVSDK_NGX_FAILED(r) || !sFeature)
    {
        Log("Upscaler[DLSS]: CreateFeature failed (0x%08x)", r);
        sFeature = nullptr;
        return false;
    }
    sRenderW = renderW; sRenderH = renderH; sDisplayW = displayW; sDisplayH = displayH;
    Log("Upscaler[DLSS]: DLAA feature created %ux%u -> %ux%u (isHDR=%d depthInv=%d preset=%d)",
        renderW, renderH, displayW, displayH, (int)isHDR, (int)depthInverted, preset);
    return true;
}

bool Evaluate(ID3D11DeviceContext* ctx,
              ID3D11Resource* color, ID3D11Resource* depth, ID3D11Resource* motionVectors,
              ID3D11Resource* output, float jitterX, float jitterY,
              float mvScaleX, float mvScaleY, float sharpness, bool reset,
              ID3D11Resource* reactiveMask)
{
    if (!sAvailable || !sFeature || !ctx) return false;
    if (!color || !depth || !motionVectors || !output) return false;

    NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor                = color;
    ep.Feature.pInOutput               = output;
    ep.Feature.InSharpness             = sharpness;
    ep.pInDepth                        = depth;
    ep.pInMotionVectors               = motionVectors;
    ep.pInBiasCurrentColorMask        = reactiveMask;   // per-pixel: favour current frame (kills fog ghosting)
    ep.InJitterOffsetX                = jitterX;
    ep.InJitterOffsetY                = jitterY;
    ep.InMVScaleX                     = mvScaleX;
    ep.InMVScaleY                     = mvScaleY;
    ep.InReset                        = reset ? 1 : 0;
    ep.InRenderSubrectDimensions.Width  = sRenderW;
    ep.InRenderSubrectDimensions.Height = sRenderH;

    NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, sFeature, sParams, &ep);
    if (NVSDK_NGX_FAILED(r)) { Log("Upscaler[DLSS]: Evaluate failed (0x%08x)", r); return false; }
    return true;
}

void Shutdown()
{
    if (sFeature)          { NVSDK_NGX_D3D11_ReleaseFeature(sFeature); sFeature = nullptr; }
    if (sParams)           { NVSDK_NGX_D3D11_DestroyParameters(sParams); sParams = nullptr; }
    if (sInited && sDevice) NVSDK_NGX_D3D11_Shutdown1(sDevice);
    sInited = false; sAvailable = false; sDevice = nullptr;
}
}

#else  // !DUST_HAVE_DLSS — NGX SDK not vendored: safe stubs so the build works without external/DLSS.

namespace Upscaler
{
static bool sWarned = false;
bool Init(ID3D11Device*, const wchar_t*, const wchar_t*)
{
    if (!sWarned) { sWarned = true; Log("Upscaler[DLSS]: built WITHOUT the NGX SDK (external/DLSS absent) — DLSS unavailable"); }
    return false;
}
bool IsAvailable() { return false; }
static const char* kPresetLabels[] = { "Default", "K (transformer, blurs when static)", "J", "F (CNN, recommended)", "E (CNN)" };
const char* const* PresetLabels() { return kPresetLabels; }
bool CreateFeature(ID3D11DeviceContext*, uint32_t, uint32_t, uint32_t, uint32_t, bool, bool, int) { return false; }
bool Evaluate(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*, ID3D11Resource*,
              ID3D11Resource*, float, float, float, float, float, bool, ID3D11Resource*) { return false; }
void Shutdown() {}
}

#endif  // DUST_HAVE_DLSS
