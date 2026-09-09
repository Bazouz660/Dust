#include "../crash/CrashClient.h"
#include "../external/miniz/miniz.h"
#include <dbghelp.h>
#include <filesystem>
#include <fstream>
#include <cassert>
#include <cstdlib>
#pragma comment(lib,"dbghelp.lib")
namespace fs = std::filesystem;
static HANDLE marker;
static LPTOP_LEVEL_EXCEPTION_FILTER downstream;
static LONG WINAPI Previous(EXCEPTION_POINTERS*) {
    DWORD written; WriteFile(marker,"chained",7,&written,nullptr);
    if (!DustCrash::shuttingDown) WaitForSingleObject(DustCrash::helper,15000);
    return EXCEPTION_EXECUTE_HANDLER;
}
static LONG WINAPI Later(EXCEPTION_POINTERS* ep) { return downstream(ep); }
__declspec(noinline) static void Fault() { *reinterpret_cast<volatile int*>(1) = 7; }
static void HandledFault() { __try { Fault(); } __except(EXCEPTION_EXECUTE_HANDLER) {} }
__declspec(noinline) static void Overflow(int depth) {
    volatile char buffer[8192]; buffer[0] = static_cast<char>(depth);
    Overflow(depth+1);
    if (buffer[0] == 42) OutputDebugStringA("unreachable");
}
static int Child(const std::wstring& mode, const fs::path& root) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(Previous);
    marker = CreateFileW((root/L"chained.txt").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    bool started = DustCrash::Start((root/L"mod").c_str(),(root/L"game").c_str(),(root/L"reports").c_str());
    if (mode == L"disabled" || mode == L"missing") { assert(!started); return 0; }
    assert(started);
    { std::ofstream pid(root/L"helper.txt"); pid << GetProcessId(DustCrash::helper); }
    assert(DustCrash::request->phase[0]);
    if (mode == L"normal") return 0;
    if (mode == L"handled") { HandledFault(); return 0; }
    if (mode == L"shutdown") InterlockedExchange(&DustCrash::shuttingDown,1);
    if (mode == L"late") { downstream = DustCrash::SetNext(Later); assert(downstream == Previous); }
    if (mode == L"worker" || mode == L"stack") {
        HANDLE thread = CreateThread(nullptr,0,[](void* p)->DWORD {
            if (p) Overflow(0); else Fault(); return 0;
        },mode == L"stack" ? reinterpret_cast<void*>(1) : nullptr,0,nullptr);
        assert(thread); WaitForSingleObject(thread,30000); assert(false);
    }
    Fault(); return 9;
}
static void CheckArchive(const fs::path& path, DWORD pid, DWORD exceptionCode) {
    FILE* file = _wfopen(path.c_str(),L"rb"); assert(file);
    mz_zip_archive zip = {}; assert(mz_zip_reader_init_cfile(&zip,file,0,0));
    size_t bytes = 0; void* data = mz_zip_reader_extract_file_to_heap(&zip,"crash.dmp",&bytes,0);
    assert(data && bytes > sizeof(MINIDUMP_HEADER));
    assert(static_cast<MINIDUMP_HEADER*>(data)->Signature == MINIDUMP_SIGNATURE);
    PMINIDUMP_DIRECTORY directory = nullptr; void* stream = nullptr; ULONG size = 0;
    assert(MiniDumpReadDumpStream(data,ExceptionStream,&directory,&stream,&size));
    auto* exception = static_cast<MINIDUMP_EXCEPTION_STREAM*>(stream);
    assert(exception->ExceptionRecord.ExceptionCode == exceptionCode);
    assert(exception->ExceptionRecord.ExceptionAddress && exception->ThreadId && exception->ThreadContext.DataSize);
    assert(MiniDumpReadDumpStream(data,ThreadListStream,&directory,&stream,&size));
    auto* threads = static_cast<MINIDUMP_THREAD_LIST*>(stream); bool found = false;
    for (ULONG i=0;i<threads->NumberOfThreads;++i) found |= threads->Threads[i].ThreadId == exception->ThreadId;
    assert(found);
    assert(MiniDumpReadDumpStream(data,MiscInfoStream,&directory,&stream,&size));
    assert(static_cast<MINIDUMP_MISC_INFO*>(stream)->ProcessId == pid); // game child, not helper
    assert(MiniDumpReadDumpStream(data,ModuleListStream,&directory,&stream,&size));
    assert(static_cast<MINIDUMP_MODULE_LIST*>(stream)->NumberOfModules > 0);
    mz_free(data);
    data = mz_zip_reader_extract_file_to_heap(&zip,"report.txt",&bytes,0); assert(data);
    std::string report(static_cast<char*>(data),bytes); mz_free(data);
    assert(report.find("before effects") != std::string::npos);
    assert(mz_zip_reader_locate_file(&zip,"config/Dust.ini",nullptr,0) >= 0);
    assert(mz_zip_reader_locate_file(&zip,"game/mods.cfg",nullptr,0) >= 0);
    mz_zip_reader_end(&zip); fclose(file);
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--child") return Child(argv[2],argv[3]);
    wchar_t exe[1024]; assert(GetModuleFileNameW(nullptr,exe,1024));
    fs::path base = fs::absolute(fs::path("build")/(L"crash probes \u00e9 "+std::to_wstring(GetCurrentProcessId())));
    for (const auto& mode : {L"normal",L"handled",L"main",L"worker",L"stack",L"late",L"disabled",L"missing",L"unwritable",L"shutdown"}) {
        fs::path root = base/mode;
        fs::create_directories(root/L"mod"); fs::create_directories(root/L"game/data");
        if (std::wstring(mode) != L"missing") fs::copy_file("../crash/build/Release/DustCrashReporter.exe",root/L"mod/DustCrashReporter.exe");
        { std::ofstream config(root/L"mod/Dust.ini"); config << "[Dust]\nFileLogging=0\n[Diagnostics]\nCrashDumps=" << (std::wstring(mode)==L"disabled" ? 0 : 1) << "\n"; }
        { std::ofstream mods(root/L"game/data/mods.cfg"); mods << "Dust.mod\n"; }
        if (std::wstring(mode) == L"unwritable") { std::ofstream block(root/L"reports"); block << "file, not directory"; }
        std::wstring command = L"\""+std::wstring(exe)+L"\" --child "+mode+L" \""+root.wstring()+L"\"";
        STARTUPINFOW si = {}; si.cb = sizeof(si); PROCESS_INFORMATION pi = {};
        assert(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi));
        CloseHandle(pi.hThread);
        assert(WaitForSingleObject(pi.hProcess,45000) == WAIT_OBJECT_0);
        DWORD code; assert(GetExitCodeProcess(pi.hProcess,&code)); CloseHandle(pi.hProcess);
        const std::wstring m(mode);
        bool crash = m == L"main" || m == L"worker" || m == L"stack" || m == L"late" || m == L"unwritable" || m == L"shutdown";
        assert(code == (crash ? (m == L"stack" ? EXCEPTION_STACK_OVERFLOW : EXCEPTION_ACCESS_VIOLATION) : 0));
        DWORD helperPid = 0; { std::ifstream input(root/L"helper.txt"); input >> helperPid; }
        if (helperPid) if (HANDLE helper = OpenProcess(SYNCHRONIZE,FALSE,helperPid)) {
            assert(WaitForSingleObject(helper,15000) == WAIT_OBJECT_0); CloseHandle(helper);
        }
        size_t archives = 0;
        if (fs::is_directory(root/L"reports")) for (const auto& entry : fs::directory_iterator(root/L"reports")) {
            if (entry.path().extension() == L".zip") { ++archives; CheckArchive(entry.path(),pi.dwProcessId,m == L"stack" ? EXCEPTION_STACK_OVERFLOW : EXCEPTION_ACCESS_VIOLATION); }
        }
        assert(archives == ((crash && m != L"unwritable" && m != L"shutdown") ? 1 : 0));
        if (crash) assert(fs::file_size(root/L"chained.txt") == 7);
        std::printf("Crash recorder %ls: PASS\n",mode); std::fflush(stdout);
    }
}
