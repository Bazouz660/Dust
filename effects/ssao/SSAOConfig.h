#pragma once

// SSAOConfig — plain data struct for SSAO parameters.
// Values are populated by the framework via DustSettingDesc array.

struct SSAOConfig
{
    // Toggle
    bool enabled = true;

    // AO generation (XeGTAO)
    float radiusMetres = 1.0f;       // world-space radius (Kenshi units / 10)
    float aoStrength = 1.2f;         // final visibility power = Strength * 1.25
    float thickness = 0.6f;          // XeGTAO thin-occluder compensation (grass, poles, thin walls)

    // Legacy (pre-XeGTAO) keys: still read from old INIs so they load, no longer used.
    float aoRadius = 0.003f;
    float aoBias = 0.001f;
    float aoMaxDepth = 0.1f;
    float filterRadius = 0.15f;
    float foregroundFade = 26.644f;
    float falloffPower = 2.0f;
    float maxScreenRadius = 0.03f;
    float minScreenRadius = 0.001f;
    float depthFadeStart = 0.0f;
    float normalDetail = 0.5f;       // normal-map cavity term (0 = none, 1 = full)

    // Blur
    float blurSharpness = 0.01f;
    int   legacySamples = 4;         // old "Samples" key (directions); replaced by "Slices"
    float legacyNormalDetail = 1.0f; // old "NormalDetail" key; replaced by "CavityDetail"

    // Camera
    float tanHalfFov = 0.5218f;

    // Direct light occlusion — how much AO affects direct sunlight (0 = ambient only, 1 = full)
    float directLightOcclusion = 0.3f;

    // Performance
    int sampleCount = 3;    // GTAO slices (1-9), each = two opposite directions
    int stepCount = 3;      // Steps per slice side (2-6)

    // Debug
    bool debugView = false;
};

extern SSAOConfig gSSAOConfig;
