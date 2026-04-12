#pragma once

struct KuwaharaConfig
{
    bool  enabled    = true;
    int   radius     = 3;       // Kernel radius in pixels (sector sampling radius)
    float strength   = 1.0f;    // Blend with original (0 = original, 1 = full filter)
    float sharpness  = 8.0f;    // How aggressively the lowest-variance sector wins
    bool  debugView  = false;
};

extern KuwaharaConfig gKuwaharaConfig;
