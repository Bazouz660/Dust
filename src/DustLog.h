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

// Directory where the DLL lives (logs go into logs/ subfolder)
inline std::string& DustLogDir()
{
    static std::string dir;
    return dir;
}

// Call once at startup with the DLL module handle.
// Reads Logging=1/0 from [Dust] section in Dust.ini next to the DLL.
inline void DustLogInit(HMODULE hModule)
{
    char path[MAX_PATH];
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string dir(path);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
        dir = dir.substr(0, pos + 1);

    DustLogDir() = dir;

    std::string ini = dir + "Dust.ini";
    DustLogEnabled() = GetPrivateProfileIntA("Dust", "Logging", 0, ini.c_str()) != 0;
}

inline FILE* DustLogFile()
{
    static FILE* f = nullptr;
    if (!f && !DustLogDir().empty())
    {
        // Create logs/ subdirectory
        std::string logsDir = DustLogDir() + "logs";
        CreateDirectoryA(logsDir.c_str(), nullptr);

        // Generate timestamped filename: Dust_YYYY-MM-DD_HH-MM-SS.log
        SYSTEMTIME st;
        GetLocalTime(&st);
        char filename[128];
        snprintf(filename, sizeof(filename),
                 "Dust_%04d-%02d-%02d_%02d-%02d-%02d.log",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);

        std::string path = logsDir + "\\" + filename;
        f = fopen(path.c_str(), "w");
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
