#pragma once

// OutlineConfig — plain data struct for edge-detection outline parameters.
// Values are populated by the framework via DustSettingDesc array.

struct OutlineConfig
{
    bool  enabled           = true;

    // Edge detection sensitivity
    float depthThreshold    = 0.003f;   // Depth discontinuity threshold (Laplacian)
    float normalThreshold   = 0.8f;     // Normal angle threshold (higher = only sharp edges)

    // Appearance
    int   thickness         = 1;        // Edge sample offset in pixels
    float strength          = 0.8f;     // Outline opacity (0 = invisible, 1 = full)
    float colorR            = 0.0f;     // Outline color R
    float colorG            = 0.0f;     // Outline color G
    float colorB            = 0.0f;     // Outline color B

    // Limits
    float maxDepth          = 0.5f;     // Max depth to draw outlines (skip distant/sky)

    // Debug
    bool  debugView         = false;
};

extern OutlineConfig gOutlineConfig;
