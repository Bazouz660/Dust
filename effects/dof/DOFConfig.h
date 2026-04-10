#pragma once

// DOFConfig — plain data struct for Depth of Field parameters.
// Values are populated by the framework via DustSettingDesc array.

struct DOFConfig
{
    // Toggle
    bool enabled = true;

    // Focus
    float focusDistance  = 0.01f;   // Focus distance in depth buffer units (linear)
    float focusRange    = 0.005f;   // Transition zone width (sharp → blurred)

    // Blur
    float blurStrength  = 1.0f;    // Overall blur intensity (0 = off, 1 = full)
    float blurRadius    = 8.0f;    // Gaussian blur radius at half resolution
    float maxDepth      = 0.1f;    // Max depth to process (skip sky)

    // Debug
    bool debugView = false;
};

extern DOFConfig gDOFConfig;
