#pragma once

struct RTGIConfig
{
    // Toggle
    bool enabled = true;

    // Horizon trace (visibility-bitmask GI)
    float rayLength     = 0.1f;    // slice march reach; UV reach = rayLength*0.5/tanHalfFov (depth-independent)
    int   raySteps      = 16;      // samples per slice
    int   raysPerPixel  = 3;       // horizon slices per pixel (min 2)
    float thickness     = 0.1f;    // view-space occluder thickness for the back horizon
    float thicknessCurve = 0.8f;   // depth exponent (1 = linear, <1 = thinner far away)
    float fadeDistance   = 0.15f;   // depth at which GI fades out
    float normalDetail   = 0.5f;   // blend between geometric (0) and gbuffer normals (1)

    // Lighting
    float giIntensity   = 3.0f;    // final indirect light brightness (applied at composite)
    float aoIntensity   = 1.0f;    // ambient occlusion strength
    float shadowContrast = 2.0f;   // power curve on the directional AO at composite —
                                   // >1 deepens occluded contacts into GI shadows while
                                   // leaving open surfaces (ao=1) untouched
    float bounceIntensity = 0.3f;  // multi-bounce feedback (0 = single bounce only)
    float saturation    = 1.0f;    // color saturation of indirect light

    // Denoise (SVGF)
    int   denoiseSteps  = 4;       // a-trous filter iterations (1-5)
    float depthSigma    = 2.0f;    // depth weight sensitivity (higher = more permissive on smooth surfaces)
    float phiColor      = 4.0f;    // luminance sensitivity (variance-guided, higher = smoother)

    // Quality — render resolution as percentage of native (25-100).
    int   resolutionScale = 50;

    // Camera (hidden)
    float tanHalfFov    = 0.5218f;

    // Debug
    int   debugView     = 0;       // 0=off, 1=GI, 2=AO, 3=combined, 4=world normals, 5=view normals, 6=depth
};

extern RTGIConfig gRTGIConfig;
