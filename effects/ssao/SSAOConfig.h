#pragma once

// SSAOConfig — plain data struct for SSAO parameters.
// Values are populated by the framework via DustSettingDesc array.

struct SSAOConfig
{
    // Toggle
    bool enabled = true;

    // AO generation
    float aoRadius = 0.003f;
    float aoStrength = 2.537f;
    float aoBias = 0.001f;
    float aoMaxDepth = 0.1f;
    float filterRadius = 0.15f;
    float foregroundFade = 26.644f;
    float falloffPower = 2.0f;
    float maxScreenRadius = 0.03f;
    float minScreenRadius = 0.001f;
    float depthFadeStart = 0.0f;
    float normalDetail = 0.5f;

    // Blur
    float blurSharpness = 0.01f;

    // Camera
    float tanHalfFov = 0.5218f;

    // Direct light occlusion — how much AO affects direct sunlight (0 = ambient only, 1 = full)
    float directLightOcclusion = 0.3f;

    // Performance
    int sampleCount = 4;    // Directions (4–12)
    int stepCount = 4;      // Steps per direction (2–6)

    // Debug
    bool debugView = false;
};

extern SSAOConfig gSSAOConfig;
