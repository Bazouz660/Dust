#pragma once

struct VignetteConfig
{
    bool enabled = true;
    float strength = 0.3f;
    float radius = 0.8f;
    float softness = 0.5f;
    int shape = 0;
    float aspectRatio = 1.0f;
    bool debugView = false;
};

extern VignetteConfig gVignetteConfig;
