#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <Debug.h>

// Global log toggle — set by DustLogInit()
inline bool& DustLogEnabled()
{
    static bool enabled = false;
    return enabled;
}

// Call once at startup with the DLL module handle.
// Reads Logging=1/0 from [Dust] section in Dust.ini next to the DLL.
inline void DustLogInit(HMODULE hModule)
{
    char path[MAX_PATH];
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    if (pos != std::string::npos)
        s = s.substr(0, pos + 1);
    s += "Dust.ini";

    DustLogEnabled() = GetPrivateProfileIntA("Dust", "Logging", 0, s.c_str()) != 0;
}

// File log — opens on first use next to the game exe
inline FILE* DustLogFile()
{
    static FILE* f = nullptr;
    if (!f)
    {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string s(path);
        auto pos = s.find_last_of("\\/");
        if (pos != std::string::npos)
            s = s.substr(0, pos + 1);
        s += "Dust.log";
        f = fopen(s.c_str(), "w");
    }
    return f;
}

inline void DustLog(const char* fmt, ...)
{
    if (!DustLogEnabled())
        return;

    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    OutputDebugStringA("[Dust] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    char fullBuf[600];
    snprintf(fullBuf, sizeof(fullBuf), "[Dust] %s", buf);
    ::DebugLog(fullBuf);

    FILE* f = DustLogFile();
    if (f)
    {
        fprintf(f, "[Dust] %s\n", buf);
        fflush(f);
    }
}

#define Log DustLog
