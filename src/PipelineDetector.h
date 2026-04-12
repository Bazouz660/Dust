#pragma once

#include "InjectionPoint.h"
#include "ResourceRegistry.h"
#include <d3d11.h>

class PipelineDetector
{
public:
    struct DetectionResult
    {
        bool            detected = false;
        InjectionPoint  point    = InjectionPoint::POST_LIGHTING;
    };

    // Call after every fullscreen Draw (3 or 4 verts).
    // Returns which injection point was detected, if any.
    DetectionResult OnFullscreenDraw(ID3D11DeviceContext* ctx);

    void ResetFrame();

private:
    bool lightingDetected_ = false;
    bool fogDetected_      = false;
    bool tonemapDetected_  = false;

    bool IsLightingPass(ID3D11DeviceContext* ctx);
    void CaptureLightingResources(ID3D11DeviceContext* ctx);

    bool IsFogPass(ID3D11DeviceContext* ctx);

    bool IsTonemapPass(ID3D11DeviceContext* ctx);
    void CaptureTonemapResources(ID3D11DeviceContext* ctx);
};

extern PipelineDetector gPipelineDetector;
