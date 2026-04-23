#pragma once

struct DebandConfig
{
    bool enabled = true;
    float threshold = 0.02f;
    float range = 16.0f;
    float intensity = 1.0f;
    bool skyOnly = false;
    float skyDepthThreshold = 0.99f;
    bool debugView = false;
};

extern DebandConfig gDebandConfig;
