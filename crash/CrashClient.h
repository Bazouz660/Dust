#pragma once
#include "CrashProtocol.h"
#include <string>
#include <vector>

namespace DustCrash {
// One client lives in pinned DustBoot. All allocation and process creation
// happen at startup; the exception path only copies into prepared shared memory.
inline Request* request = nullptr;
inline HANDLE mapping = nullptr, signal = nullptr, done = nullptr, helper = nullptr;
inline volatile LONG reporting = 0, shuttingDown = 0;
inline PVOID volatile nextFilter = nullptr;
inline LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetNext(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    return reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(InterlockedExchangePointer(&nextFilter, reinterpret_cast<PVOID>(filter)));
}
inline LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    if (!InterlockedCompareExchange(&shuttingDown,0,0) && ep && request) {
        if (InterlockedCompareExchange(&reporting,1,0) == 0) {
            request->threadId = GetCurrentThreadId();
            request->exception = *ep->ExceptionRecord;
            request->exception.ExceptionRecord = nullptr;
            request->context = *ep->ContextRecord;
            SetEvent(signal);
        }
        HANDLE waits[] = {done, helper};
        WaitForMultipleObjects(2, waits, FALSE, 30000);
    }
    auto next = reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(InterlockedCompareExchangePointer(&nextFilter,nullptr,nullptr));
    return next && next != Filter ? next(ep) : EXCEPTION_CONTINUE_SEARCH;
}
inline void CloseClient() {
    if (request) UnmapViewOfFile(request);
    request = nullptr;
    for (HANDLE h : {mapping,signal,done,helper}) if (h) CloseHandle(h);
    mapping = signal = done = helper = nullptr;
}
inline bool Start(const wchar_t* mod, const wchar_t* game, const wchar_t* outputOverride = nullptr) {
    if (request) return true;
    std::wstring ini = std::wstring(mod)+L"\\Dust.ini";
    if (!GetPrivateProfileIntW(L"Diagnostics",L"CrashDumps",1,ini.c_str())) return false;
    SECURITY_ATTRIBUTES sa = {sizeof(sa),nullptr,TRUE};
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,sizeof(Request),nullptr);
    signal = CreateEventW(&sa,FALSE,FALSE,nullptr); done = CreateEventW(&sa,TRUE,FALSE,nullptr);
    HANDLE process = nullptr;
    const bool duplicated = DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&process,
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE,TRUE,0) != FALSE;
    if (mapping) request = static_cast<Request*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Request)));
    if (!request || !signal || !done || !duplicated) { if(process) CloseHandle(process); CloseClient(); return false; }
    request->protocol = Protocol; request->processId = GetCurrentProcessId();
    wcscpy_s(request->modDir,mod); wcscpy_s(request->gameDir,game);
    if (outputOverride) wcscpy_s(request->outputDir,outputOverride);
    else if (!ReportDirectory(request->outputDir,1024)) { CloseHandle(process); CloseClient(); return false; }
    strcpy_s(request->phase,"DustBoot startup (before effects)");
    const std::wstring exe = std::wstring(mod)+L"\\DustCrashReporter.exe";
    wchar_t command[4096];
    swprintf_s(command,L"\"%s\" %llu %llu %llu %llu",exe.c_str(),
        (unsigned long long)(uintptr_t)mapping,(unsigned long long)(uintptr_t)signal,
        (unsigned long long)(uintptr_t)done,(unsigned long long)(uintptr_t)process);
    SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
    std::vector<unsigned char> storage(bytes);
    STARTUPINFOEXW startup = {}; startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    HANDLE handles[] = {mapping,signal,done,process};
    bool initialized = InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&bytes) != FALSE;
    bool attributes = initialized && UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles,sizeof(handles),nullptr,nullptr);
    PROCESS_INFORMATION pi = {};
    bool launched = attributes && CreateProcessW(exe.c_str(),command,nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,nullptr,mod,&startup.StartupInfo,&pi);
    if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
    // Do not expose our communication handles to unrelated future child processes.
    for (HANDLE h : handles) SetHandleInformation(h,HANDLE_FLAG_INHERIT,0);
    CloseHandle(process);
    if (!launched) { CloseClient(); return false; }
    CloseHandle(pi.hThread); helper = pi.hProcess;
    HANDLE waits[] = {done,helper};
    if (WaitForMultipleObjects(2,waits,FALSE,5000) != WAIT_OBJECT_0) {
        // Wake a slow helper so it can exit without ever touching the game.
        request->protocol = 0; SetEvent(signal); CloseClient(); return false;
    }
    ResetEvent(done);
    SetNext(SetUnhandledExceptionFilter(Filter));
    ULONG guarantee = 64*1024; SetThreadStackGuarantee(&guarantee);
    return true;
}
}
