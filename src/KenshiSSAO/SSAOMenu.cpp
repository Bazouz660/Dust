#include "SSAOMenu.h"
#include "SSAOConfig.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <Debug.h>

static void Log(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA("[KenshiSSAO] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    char fullBuf[600];
    snprintf(fullBuf, sizeof(fullBuf), "[KenshiSSAO] %s", buf);
    ::DebugLog(fullBuf);
}

namespace SSAOMenu
{

static std::string sSettingsPath;
static bool sInitialized = false;
static bool sLastState = true;

static void FindSettingsFile()
{
    // The game exe runs from Kenshi/RE_Kenshi/, but settings.cfg is in Kenshi/
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);

    // Strip exe filename
    std::string dir(exePath);
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
        dir = dir.substr(0, pos);

    // Go up one level (RE_Kenshi -> Kenshi)
    pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
        dir = dir.substr(0, pos + 1);

    sSettingsPath = dir + "settings.cfg";
    sInitialized = true;
    Log("Settings file: %s", sSettingsPath.c_str());
}

// Parse sectionless key=value file for a specific key
static int ReadKeyValue(const char* path, const char* key, int defaultVal)
{
    FILE* f = fopen(path, "r");
    if (!f)
        return defaultVal;

    char line[256];
    size_t keyLen = strlen(key);

    while (fgets(line, sizeof(line), f))
    {
        // Skip whitespace
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;

        // Match "key="
        if (strncmp(p, key, keyLen) == 0 && p[keyLen] == '=')
        {
            int val = atoi(p + keyLen + 1);
            fclose(f);
            return val;
        }
    }

    fclose(f);
    return defaultVal;
}

bool PollCompositorEnabled()
{
    if (!sInitialized)
        FindSettingsFile();

    bool enabled = ReadKeyValue(sSettingsPath.c_str(), "KenshiSSAO", 1) != 0;

    if (enabled != sLastState)
    {
        Log("Graphics toggle: KenshiSSAO=%d", (int)enabled);
        sLastState = enabled;
    }

    return enabled;
}

} // namespace SSAOMenu
