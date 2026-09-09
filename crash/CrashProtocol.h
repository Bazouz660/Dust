#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")

namespace DustCrash {
constexpr DWORD Protocol = 0x44555331;
struct Request {
    DWORD protocol, processId, threadId;
    EXCEPTION_RECORD exception;
    CONTEXT context;
    wchar_t modDir[1024], gameDir[1024], outputDir[1024];
    char phase[64];
};
inline bool ReportDirectory(wchar_t* path, size_t count) {
    wchar_t base[MAX_PATH] = {};
    return SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, base))
        && swprintf_s(path, count, L"%s\\Dust\\crash_reports", base) > 0;
}
}
