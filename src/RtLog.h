#pragma once

// Logging shim so RtCore/RtScene compile both in the host (DustLog) and in the
// standalone RtSelfTest console exe (printf).

#ifdef RT_SELFTEST
#include <cstdio>
#include <cstdarg>
inline void RtTestLog(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("[RtTest] %s\n", buf);
}
#define Log RtTestLog
#else
#include "DustLog.h"
#endif
