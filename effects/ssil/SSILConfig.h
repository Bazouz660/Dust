#pragma once

// SSILConfig — plain data struct for SSIL parameters.
// Values are populated by the framework via DustSettingDesc array.

struct SSILConfig
{
    // Toggle
    bool enabled = true;

    // IL generation
    float ilRadius = 0.005f;
    float ilStrength = 1.0f;
    float ilBias = 0.05f;
    float ilMaxDepth = 0.15f;
    float foregroundFade = 26.0f;
    float falloffPower = 2.0f;
    float maxScreenRadius = 0.05f;
    float minScreenRadius = 0.001f;
    float depthFadeStart = 0.0f;
    float colorBleeding = 1.0f;

    // Sampling
    int sampleCount = 8;

    // Blur
    float blurSharpness = 0.01f;

    // Camera
    float tanHalfFov = 0.5218f;

    // Debug
    bool debugView = false;
};

extern SSILConfig gSSILConfig;
