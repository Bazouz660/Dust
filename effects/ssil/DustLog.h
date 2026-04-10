// DustLog.h - Plugin-side logging via host callback
// Routes Log() calls through a function pointer set during Init.
#pragma once

#include <cstdio>
#include <cstdarg>

// Set by DustSSIL.cpp during Init, points to host->Log
typedef void (*DustLogFn)(const char* fmt, ...);
extern DustLogFn gLogFn;

inline void DustLog(const char* fmt, ...)
{
    if (!gLogFn) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    gLogFn("%s", buf);
}

// Use DustLog as the Log function in all SSIL source files.
// NOTE: Do NOT use this macro when accessing DustHostAPI.Log member.
#define Log DustLog
