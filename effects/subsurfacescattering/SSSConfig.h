#pragma once

// SSSConfig — plain data struct for Subsurface Scattering parameters.
// Values are populated by the framework via the DustSettingDesc array.

struct SSSConfig
{
    // Toggle
    bool enabled = true;

    // Blur shape
    float strength       = 0.6f;   // composite blend 0..1 (0 = off, 1 = full blurred skin)
    float blurWidth      = 6.0f;   // base screen-pixel blur radius at near depth
    float maxRadiusPx    = 24.0f;  // hard cap on per-tap pixel offset (stability up close)

    // Bilateral / surface following
    float depthThreshold = 0.03f;  // reject taps whose depth differs > this fraction
    float followSurface  = 0.85f;  // 0 = ignore tag, 1 = only blur skin-tagged taps

    // Debug
    bool debugView = false;        // tint skin-tagged pixels magenta
};

extern SSSConfig gSSSConfig;
