#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <Debug.h>

inline void DustLog(const char* fmt, ...)
{
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
}

#define Log DustLog
