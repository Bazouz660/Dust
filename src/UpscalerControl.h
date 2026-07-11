#pragma once

// Runtime control surface for the temporal upscaler (DLSS), used by the GUI. The state and the resolve
// live in D3D11Hook.cpp; these just read/write the runtime config. The MV debug viz toggle lives in
// MotionVectors (SetDebugViz). Preset changes take effect on the next frame (feature is recreated).
namespace UpscalerControl
{
    bool  Available();            // DLSS reported available on this GPU (known after the first frame)
    bool  GetEnabled();
    void  SetEnabled(bool e);
    int   GetPreset();            // Upscaler::Preset index
    void  SetPreset(int p);
    float GetSharpness();         // [0,1]
    void  SetSharpness(float s);
    int   GetBackend();           // 0 = DLSS, 1 = FSR2
    void  SetBackend(int b);
}
