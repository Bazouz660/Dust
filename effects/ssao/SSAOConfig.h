#pragma once

// SSAOConfig — runtime settings for the SSAO effect.

struct SSAOConfig
{
    // Toggle
    bool enabled = true;

    // === AO quality / sampling ===

    // Sample quality preset
    // 0=Low (8), 1=Medium (16), 2=High (40), 3=Very High (72),
    // 4=Ultra (112), 5=Extreme (192), 6=IDGAF (320)
    int sampleQualityPreset = 1;

    // SHADING_RATE: 0=Full, 1=Half, 2=Quarter
    int shadingRate = 0;

    // AO sample radius: 0.5–10.0, default 2.5
    float sampleRadius = 2.5f;

    // Worldspace radius enable
    bool worldspaceEnable = false;

    // AO intensity: 0–1 (allows >1 for gamma-boosted intensity)
    float ssaoAmount = 0.8f;

    // Fade depth: 0–1
    float fadeDepth = 0.25f;

    // Filter size: 0–2
    int filterSize = 1;

    // === Algorithm variant ===

    // AO type: 0=GTAO (high contrast), 1=Solid Angle (smooth, fastest),
    // 2=Visibility Bitmask (physically correct), 3=Bitmask+Solid Angle.
    int aoType = 2;

    // === Dust-specific (camera, deferred binding) ===

    // tan(fovY/2). Kenshi uses a 45° vertical FOV — measured directly from
    // the projection matrix at c34/byte 544 in the deferred lighting CB:
    // matrix[1][1] = 2.414 → tan(fovY/2) = 1/2.414 = 0.4142.
    float tanHalfFov = 0.4142f;

    // Far plane reference. The depth_to_z formula is `d * FarPlane + 1`
    // and assumes d in [0,1] is linearized depth. Kenshi's depth from t0 is
    // linear view-Z in scene units; treating it as already-linearized [0,1]
    // depth with FarPlane=8000 puts z in a comparable scale.
    float farPlane = 8000.0f;

    // How much AO affects directly lit areas (0 = ambient only, 1 = full).
    // Bound at slot 9 for the deferred shader to consume.
    float directLightOcclusion = 0.3f;

};

extern SSAOConfig gSSAOConfig;
