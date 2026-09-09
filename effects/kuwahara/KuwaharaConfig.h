#pragma once

struct KuwaharaConfig
{
    bool  enabled    = true;
    int   radius     = 3;       // Kernel radius in pixels (sector sampling radius)
    float strength   = 1.0f;    // Blend with original (0 = original, 1 = full filter)
    float sharpness  = 8.0f;    // How aggressively the lowest-variance sector wins
    bool  depthEnabled = false;
    float depthStart = 0.01f;   // Linear distance / far clip, as in DoF
    float depthEnd = 0.10f;
    float nearRadius = 0.0f;    // Radius/strength above are the far endpoints
    float nearStrength = 0.0f;
    bool  debugView  = false;
};

extern KuwaharaConfig gKuwaharaConfig;
