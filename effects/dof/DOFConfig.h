#pragma once

// DOFConfig — plain data struct for Depth of Field parameters.
// Values are populated by the framework via DustSettingDesc array.

struct DOFConfig
{
    // Toggle
    bool enabled = true;

    // Auto-focus (samples depth at screen center)
    bool autoFocus          = true;
    float autoFocusSpeed    = 3.0f;    // Adaptation speed (higher = faster)
    float focusDistance     = 0.02f;   // Manual focus distance (used when autoFocus is off)

    // Near field blur (objects closer than focus)
    float nearStart         = 0.003f;  // Distance from focus where near blur begins
    float nearEnd           = 0.034f;  // Distance from focus where near blur reaches max
    float nearStrength      = 0.5f;    // Near blur intensity (0 = off, 1 = full)

    // Far field blur (objects farther than focus)
    float farStart          = 0.01f;   // Distance from focus where far blur begins
    float farEnd            = 0.05f;   // Distance from focus where far blur reaches max
    float farStrength       = 1.0f;    // Far blur intensity (0 = off, 1 = full)

    // Blur quality
    float blurRadius        = 2.565f;  // Gaussian blur kernel radius
    float maxDepth          = 1.0f;    // Max depth to process (skip sky)
    int   blurDownscale     = 2;       // Blur resolution divisor (2 = half, 4 = quarter)
    int   blurShape         = 0;       // 0=gaussian, 1=disc, 2=hexagonal

    // Debug
    bool debugView = false;

    // CoC mode: false = legacy (near/far ramps), true = physical (thin-lens aperture)
    bool physicalCoC = false;
    float aperture = 0.01f;

    // Highlight preservation: bright pixels bleed through DoF blur
    float highlightThreshold = 1.0f;
    float highlightBoost = 0.0f;
};

extern DOFConfig gDOFConfig;
