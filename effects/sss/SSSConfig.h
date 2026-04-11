#pragma once

// SSSConfig — plain data struct for Screen Space Shadows parameters.
// Values are populated by the framework via DustSettingDesc array.

struct SSSConfig
{
    // Toggle
    bool enabled = true;

    // Ray marching
    float maxDistance   = 0.005f;   // Max march distance in view space
    float thickness     = 0.001f;   // Thickness threshold for occlusion
    int   stepCount     = 16;       // Number of ray march steps
    float depthBias     = 0.0001f;  // Bias to avoid self-shadowing
    float maxDepth      = 0.1f;     // Max depth to process (skip distant pixels)

    // Output
    float strength      = 0.7f;     // Shadow strength (0 = no shadow, 1 = full black)

    // Blur
    float blurSharpness = 0.01f;    // Bilateral blur depth sensitivity

    // Camera (must match SSAO's value)
    float tanHalfFov    = 0.5218f;

    // Debug
    bool debugView      = false;
};

extern SSSConfig gSSSConfig;
