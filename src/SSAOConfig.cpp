#include "SSAOConfig.h"
#include "DustLog.h"
#include <cstdlib>

SSAOConfig gSSAOConfig;

static const char* SECTION = "SSAO";

void SSAOConfig::Init(HMODULE hModule)
{
    char dllPath[MAX_PATH];
    GetModuleFileNameA(hModule, dllPath, MAX_PATH);

    // Strip filename to get directory
    std::string dir(dllPath);
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
        dir = dir.substr(0, pos);

    filePath = dir + "\\Dust.ini";

    // Check if file exists
    DWORD attr = GetFileAttributesA(filePath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        Log("Config file not found, creating defaults: %s", filePath.c_str());
        WriteDefaults();
    }

    Load();

    // Store initial write time
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(filePath.c_str(), GetFileExInfoStandard, &fad))
        lastWriteTime = fad.ftLastWriteTime;
}

float SSAOConfig::ReadFloat(const char* key, float def)
{
    char buf[64];
    char defStr[64];
    snprintf(defStr, sizeof(defStr), "%g", def);
    GetPrivateProfileStringA(SECTION, key, defStr, buf, sizeof(buf), filePath.c_str());
    return (float)atof(buf);
}

int SSAOConfig::ReadInt(const char* key, int def)
{
    return GetPrivateProfileIntA(SECTION, key, def, filePath.c_str());
}

void SSAOConfig::Load()
{
    enabled        = ReadInt("Enabled", 1) != 0;
    aoRadius       = ReadFloat("Radius", 0.002f);
    aoStrength     = ReadFloat("Strength", 2.0f);
    aoBias         = ReadFloat("Bias", 0.05f);
    aoMaxDepth     = ReadFloat("MaxDepth", 0.1f);
    filterRadius   = ReadFloat("FilterRadius", 0.15f);
    foregroundFade = ReadFloat("ForegroundFade", 50.0f);
    falloffPower   = ReadFloat("FalloffPower", 2.0f);
    maxScreenRadius = ReadFloat("MaxScreenRadius", 0.1f);
    minScreenRadius = ReadFloat("MinScreenRadius", 0.001f);
    depthFadeStart = ReadFloat("DepthFadeStart", 0.0f);
    blurSharpness  = ReadFloat("BlurSharpness", 0.01f);
    nightCompensation = ReadFloat("NightCompensation", 10.0f);
    tanHalfFov     = ReadFloat("TanHalfFov", 0.5218f);
    debugView      = ReadInt("DebugView", 0) != 0;

    Log("Config loaded: enabled=%d radius=%.4f strength=%.1f bias=%.3f maxDepth=%.2f fgFade=%.1f falloff=%.1f blur=%.3f debug=%d",
        enabled, aoRadius, aoStrength, aoBias, aoMaxDepth, foregroundFade, falloffPower, blurSharpness, debugView);
}

void SSAOConfig::Save()
{
    char buf[64];

    WritePrivateProfileStringA(SECTION, "Enabled", enabled ? "1" : "0", filePath.c_str());

    snprintf(buf, sizeof(buf), "%g", aoRadius);
    WritePrivateProfileStringA(SECTION, "Radius", buf, filePath.c_str());

    snprintf(buf, sizeof(buf), "%g", aoStrength);
    WritePrivateProfileStringA(SECTION, "Strength", buf, filePath.c_str());

    snprintf(buf, sizeof(buf), "%g", aoBias);
    WritePrivateProfileStringA(SECTION, "Bias", buf, filePath.c_str());

    snprintf(buf, sizeof(buf), "%g", aoMaxDepth);
    WritePrivateProfileStringA(SECTION, "MaxDepth", buf, filePath.c_str());

    snprintf(buf, sizeof(buf), "%g", filterRadius);
    WritePrivateProfileStringA(SECTION, "FilterRadius", buf, filePath.c_str());

    snprintf(buf, sizeof(buf), "%g", foregroundFade);
    WritePrivateProfileStringA(SECTION, "ForegroundFade", buf, filePath.c_str());

    snprintf(buf, sizeof(buf), "%g", falloffPower);
    WritePrivateProfileStringA(SECTION, "FalloffPower", buf, filePath.c_str());

    snprintf(buf, sizeof(buf), "%g", maxScreenRadius);
    WritePrivateProfileStringA(SECTION, "MaxScreenRadius", buf, filePath.c_str());

    snprintf(buf, sizeof(buf), "%g", minScreenRadius);
    WritePrivateProfileStringA(SECTION, "MinScreenRadius", buf, filePath.c_str());

    snprintf(buf, sizeof(buf), "%g", depthFadeStart);
    WritePrivateProfileStringA(SECTION, "DepthFadeStart", buf, filePath.c_str());

    snprintf(buf, sizeof(buf), "%g", blurSharpness);
    WritePrivateProfileStringA(SECTION, "BlurSharpness", buf, filePath.c_str());

    snprintf(buf, sizeof(buf), "%g", nightCompensation);
    WritePrivateProfileStringA(SECTION, "NightCompensation", buf, filePath.c_str());

    snprintf(buf, sizeof(buf), "%g", tanHalfFov);
    WritePrivateProfileStringA(SECTION, "TanHalfFov", buf, filePath.c_str());

    WritePrivateProfileStringA(SECTION, "DebugView", debugView ? "1" : "0", filePath.c_str());
}

void SSAOConfig::WriteDefaults()
{
    // Reset to defaults
    *this = SSAOConfig();
    // Keep the filePath
    Save();
}

void SSAOConfig::CheckHotReload()
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(filePath.c_str(), GetFileExInfoStandard, &fad))
        return;

    if (CompareFileTime(&fad.ftLastWriteTime, &lastWriteTime) != 0)
    {
        lastWriteTime = fad.ftLastWriteTime;
        Log("Config file changed, reloading...");
        Load();
    }
}
