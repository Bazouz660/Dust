#pragma once
#include <cstdio>
#include <cstdarg>

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

#define Log DustLog
