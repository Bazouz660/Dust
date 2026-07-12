#pragma once

#include <d3d11.h>
#include <cstdint>

// DLSS (NVIDIA NGX) temporal upscaling / DLAA backend — Milestone B of the upscaling feature.
// First target is native-resolution DLAA (render res == display res); low-res upscaling
// (Milestone C) reuses the same Evaluate with a smaller render size. NVIDIA RTX only — every
// entry point degrades to a safe no-op (IsAvailable() == false) on other adapters / old drivers.
//
// A later refactor abstracts this behind an IUpscaler interface so an FSR2 backend can slot in;
// for now it's a concrete namespace so the pipeline can be brought up against one known-good SDK.
namespace Upscaler
{
    // One-time init: NGX init on the game's D3D11 device + capability query. Returns false (and
    // IsAvailable() stays false) on non-NVIDIA, too-old driver, or missing runtime. dllSearchPath =
    // the folder holding our shipped nvngx_dlss.dll, so DLSS loads without app allowlisting.
    bool Init(ID3D11Device* device, const wchar_t* dllSearchPath, const wchar_t* appDataPath);

    // True once NGX is up and DLSS super-resolution is reported available on this GPU.
    bool IsAvailable();

    // GUI preset choices -> NGX render presets. Default is F (CNN DLAA); K/J are the DLSS-4 transformer
    // (sharpest in theory but blur the static image in this integration).
    enum Preset { PRESET_DEFAULT = 0, PRESET_K, PRESET_J, PRESET_F, PRESET_E, PRESET_COUNT };
    // Labels for a GUI combo (index by Preset).
    const char* const* PresetLabels();

    // (Re)create the DLSS feature for a render->display size. For the DLAA pass renderW==displayW.
    // isHDR: color input is linear/HDR (pre-tonemap). depthInverted: depth is reversed-Z. preset = one
    // of the Preset enum. Recreate to change preset.
    bool CreateFeature(ID3D11DeviceContext* ctx, uint32_t renderW, uint32_t renderH,
                       uint32_t displayW, uint32_t displayH, bool isHDR, bool depthInverted, int preset);

    // Run DLSS for one frame. color/depth/motionVectors are render-res inputs; output is display-res.
    // jitterXY = the sub-pixel jitter applied to this frame (pixels). mvScaleXY converts the MV texel
    // value into render-target pixels. sharpness in [0,1] (only affects older presets; the transformer
    // preset K ignores it). reset = discard temporal history (camera cut / first frame).
    bool Evaluate(ID3D11DeviceContext* ctx,
                  ID3D11Resource* color, ID3D11Resource* depth, ID3D11Resource* motionVectors,
                  ID3D11Resource* output, float jitterX, float jitterY,
                  float mvScaleX, float mvScaleY, float sharpness, bool reset,
                  ID3D11Resource* reactiveMask /* nullable: bias-current-color mask, kills fog ghosting */);

    void Shutdown();
}
