#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <mutex>
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

// The Kenshi game root (trailing slash), derived from the HOST EXE, not from the DLL: the DLL may
// live in <game>/mods/Dust or in steamapps/workshop/content/233860/<id>, and only the former sits
// under the game folder. RE_Kenshi launches a patched copy of kenshi_x64.exe from <game>/RE_Kenshi/,
// so walk up from the exe until the game's marker files appear. Falls back to the exe dir.
// (Same derivation as dllmain's and BugReport's static GetGameDir.)
inline std::string DustGameDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path);
    auto pos = dir.find_last_of("\\/");
    dir = (pos != std::string::npos) ? dir.substr(0, pos + 1) : dir;
    auto isFile = [](const std::string& p) {
        DWORD a = GetFileAttributesA(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    };
    std::string probe = dir;
    for (int i = 0; i < 3; i++)
    {
        if (isFile(probe + "settings.cfg") || isFile(probe + "currentVersion.txt"))
            return probe;
        size_t p = probe.find_last_of("\\/", probe.size() - 2);
        if (p == std::string::npos) break;
        probe = probe.substr(0, p + 1);
    }
    return dir;
}

// Serializes DustLog across the render thread, the WndProc thread, and worker threads (the
// first-call fopen in DustLogFile and the sBytesWritten/sCapped cap counters race otherwise).
// Function-local static in an inline function — one instance per module, same pattern as the
// rest of this header.
inline std::mutex& DustLogMutex()
{
    static std::mutex m;
    return m;
}

// Delete the oldest logs so at most maxKeep files remain. Filenames embed the
// timestamp (Dust_YYYY-MM-DD_HH-MM-SS.log), so lexical order == chronological
// order. O(n^2) worst case, but n is capped at MaxLogFiles anyway.
inline void DustLogRotate(const std::string& logsDir, int maxKeep)
{
    for (;;)
    {
        int count = 0;
        std::string oldest;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((logsDir + "\\Dust_*.log").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            count++;
            if (oldest.empty() || strcmp(fd.cFileName, oldest.c_str()) < 0)
                oldest = fd.cFileName;
        } while (FindNextFileA(h, &fd));
        FindClose(h);

        if (count <= maxKeep || oldest.empty()) return;
        if (!DeleteFileA((logsDir + "\\" + oldest).c_str())) return; // locked -> give up
    }
}

// Call once at startup with the DLL module handle.
// File logging is ON by default so bug reports always have logs; opt out with
// FileLogging=0 in [Dust]. (The pre-v0.8 key "Logging" is ignored — old saves
// auto-wrote Logging=0 as a default, which was never a deliberate user choice.)
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
    DustLogEnabled() = GetPrivateProfileIntA("Dust", "FileLogging", 1, ini.c_str()) != 0;

    // Rotate old logs (even when logging is off, keep the folder tidy).
    // This session's new log brings the total back up to MaxLogFiles.
    int maxFiles = GetPrivateProfileIntA("Dust", "MaxLogFiles", 10, ini.c_str());
    if (maxFiles < 1)   maxFiles = 1;
    if (maxFiles > 100) maxFiles = 100;
    DustLogRotate(dir + "logs", maxFiles - 1);
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

// Hard per-session cap on the log file. Logging is event-driven (~30 KB per
// normal session); the cap only exists so a runaway error spam can't fill the
// user's disk now that logging defaults to on.
#define DUST_LOG_MAX_BYTES (20u * 1024u * 1024u)

inline void DustLog(const char* fmt, ...)
{
    if (!DustLogEnabled())
        return;

    std::lock_guard<std::mutex> lock(DustLogMutex());

    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    char fullBuf[600];
    int len = snprintf(fullBuf, sizeof(fullBuf), "[Dust] %s\n", buf);
    OutputDebugStringA(fullBuf);
    if (len > 0 && len < (int)sizeof(fullBuf))
        fullBuf[len - 1] = '\0';   // drop the '\n' for DebugLog / file lines
    ::DebugLog(fullBuf);

    FILE* f = DustLogFile();
    if (f)
    {
        static size_t sBytesWritten = 0;
        static bool sCapped = false;
        if (!sCapped)
        {
            int n = fprintf(f, "[Dust] %s\n", buf);
            if (n > 0) sBytesWritten += (size_t)n;
            if (sBytesWritten > DUST_LOG_MAX_BYTES)
            {
                sCapped = true;
                fprintf(f, "[Dust] Session log reached the %u MB cap - file logging stopped for this session\n",
                        DUST_LOG_MAX_BYTES / (1024u * 1024u));
            }
            fflush(f);
        }
    }
}

#define Log DustLog
