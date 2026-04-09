#pragma once

#include <windows.h>
#include <string>

struct SSAOConfig
{
    // Toggle
    bool enabled = true;

    // AO generation
    float aoRadius = 0.002f;
    float aoStrength = 3.0f;
    float aoBias = 0.05f;
    float aoMaxDepth = 0.1f;
    float filterRadius = 0.15f;
    float foregroundFade = 50.0f;
    float falloffPower = 2.0f;
    float maxScreenRadius = 0.03f;
    float minScreenRadius = 0.001f;
    float depthFadeStart = 0.0f;

    // Blur
    float blurSharpness = 0.01f;

    // Exposure compensation — boosts AO in dark scenes to counteract auto-exposure
    float nightCompensation = 10.0f;

    // Camera
    float tanHalfFov = 0.5218f;

    // Debug
    bool debugView = false;

    // Initialize from DLL module handle — finds .ini next to the DLL
    void Init(HMODULE hModule);

    // Load settings from the .ini file. Creates the file with defaults if missing.
    void Load();

    // Check file modification time and reload if changed. Call once per frame.
    void CheckHotReload();

    // Write current values back to the .ini file.
    void Save();

private:
    std::string filePath;
    FILETIME lastWriteTime = {};

    void WriteDefaults();
    float ReadFloat(const char* key, float def);
    int ReadInt(const char* key, int def);
};

extern SSAOConfig gSSAOConfig;
