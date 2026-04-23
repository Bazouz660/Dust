#pragma once

struct RTGIConfig
{
    // Toggle
    bool enabled = true;

    // Ray tracing
    float rayLength     = 0.5f;    // ray reach in view-space (relative to depth)
    int   raySteps      = 16;      // march steps per ray
    int   raysPerPixel  = 1;       // rays per pixel per frame (temporal handles convergence)
    float thickness     = 0.1f;    // depth thickness for hit detection
    float thicknessCurve = 0.8f;   // depth exponent (1 = linear, <1 = thinner far away)
    float fadeDistance   = 0.15f;   // depth at which GI fades out
    float normalDetail   = 0.5f;   // blend between geometric (0) and gbuffer normals (1)

    // Lighting
    float giIntensity   = 3.0f;    // final indirect light brightness (applied at composite)
    float aoIntensity   = 1.0f;    // ambient occlusion strength
    float bounceIntensity = 0.3f;  // multi-bounce feedback (0 = single bounce only)
    float saturation    = 1.0f;    // color saturation of indirect light

    // Temporal
    float temporalBlend = 0.97f;   // accumulation factor (higher = smoother, more ghosting)

    // Denoise (SVGF)
    int   denoiseSteps  = 4;       // a-trous filter iterations (1-5)
    float depthSigma    = 2.0f;    // depth weight sensitivity (higher = more permissive on smooth surfaces)
    float phiColor      = 4.0f;    // luminance sensitivity (variance-guided, higher = smoother)

    // Quality — render resolution as percentage of native (25-100).
    // Sub-pixel temporal jitter (Halton 2,3) reclaims detail at reduced resolution.
    int   resolutionScale = 50;

    // Camera (hidden)
    float tanHalfFov    = 0.5218f;

    // Debug
    int   debugView     = 0;       // 0=off, 1=GI, 2=AO, 3=combined, 4=world normals, 5=view normals, 6=depth
};

extern RTGIConfig gRTGIConfig;
