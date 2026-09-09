#include "CrashProtocol.h"
#include <dbghelp.h>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <string>
#include <cstdio>
#include "../external/miniz/miniz.h"

namespace fs = std::filesystem;
static std::string Utf8(const wchar_t* text) {
    int size = WideCharToMultiByte(CP_UTF8,0,text,-1,nullptr,0,nullptr,nullptr);
    std::string value(size,0);
    WideCharToMultiByte(CP_UTF8,0,text,-1,value.data(),size,nullptr,nullptr);
    if (!value.empty()) value.pop_back(); return value;
}
static bool AddFile(mz_zip_archive& zip, const fs::path& path, const std::string& name, size_t cap) {
    HANDLE file = CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file == INVALID_HANDLE_VALUE) return true; // optional diagnostic
    LARGE_INTEGER size = {}; GetFileSizeEx(file,&size);
    size_t count = static_cast<size_t>((std::min)(size.QuadPart,static_cast<LONGLONG>(cap)));
    if (size.QuadPart > static_cast<LONGLONG>(count)) {
        LARGE_INTEGER offset; offset.QuadPart = size.QuadPart-count;
        SetFilePointerEx(file,offset,nullptr,FILE_BEGIN);
    }
    std::vector<char> data(count); DWORD read = 0;
    BOOL ok = ReadFile(file,data.data(),static_cast<DWORD>(count),&read,nullptr); CloseHandle(file);
    return !ok || mz_zip_writer_add_mem(&zip,name.c_str(),data.data(),read,MZ_DEFAULT_COMPRESSION);
}
static std::vector<fs::path> Files(const fs::path& dir, const std::wstring& prefix, const std::wstring& ext) {
    std::vector<fs::path> files; std::error_code error;
    for (fs::directory_iterator i(dir,error),end; !error && i != end; i.increment(error)) {
        auto name = i->path().filename().wstring();
        if (i->is_regular_file(error) && name.compare(0,prefix.size(),prefix) == 0 && i->path().extension() == ext)
            files.push_back(i->path());
    }
    std::sort(files.begin(),files.end()); return files;
}
static int Capture(DustCrash::Request& request, HANDLE process, HANDLE done) {
    fs::path output(request.outputDir); std::error_code error;
    fs::create_directories(output,error);
    SYSTEMTIME now; GetSystemTime(&now);
    wchar_t name[128]; swprintf_s(name,L"DustCrash_%04u-%02u-%02u_%02u-%02u-%02u-%03u_%lu",
        now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds,request.processId);
    fs::path stem = output/name, dump = stem; dump += L".dmp";
    HANDLE file = CreateFileW(dump.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    bool written = false; DWORD failure = GetLastError();
    EXCEPTION_POINTERS pointers = {&request.exception,&request.context};
    MINIDUMP_EXCEPTION_INFORMATION exception = {request.threadId,&pointers,FALSE};
    if (file != INVALID_HANDLE_VALUE) {
        const auto flags = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo
            | MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo | MiniDumpIgnoreInaccessibleMemory);
        written = MiniDumpWriteDump(process,request.processId,file,flags,&exception,nullptr,nullptr) != FALSE;
        failure = written ? 0 : GetLastError(); FlushFileBuffers(file); CloseHandle(file);
    }
    // The exception thread may now continue to Kenshi/RE_Kenshi's existing handler.
    SetEvent(done);
    char details[2048];
    snprintf(details,sizeof(details),"Dust automatic crash report\r\nUTC: %04u-%02u-%02u %02u:%02u:%02u\r\n"
        "Process: %lu\r\nFaulting thread: %lu\r\nException: 0x%08lX\r\nAddress: %p\r\nPhase: %.63s\r\n"
        "Minidump: %s (error 0x%08lX)\r\n\r\nThe dump contains thread stacks, modules and memory metadata, not a full heap dump.\r\n"
        "A fault inside a module does not by itself prove that module caused it.\r\n"
        "Saved locally; nothing was uploaded. Review before sharing: dumps may contain paths and memory fragments.\r\n",
        now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,request.processId,request.threadId,
        request.exception.ExceptionCode,request.exception.ExceptionAddress,request.phase,written ? "written" : "FAILED",failure);
    std::string report = details;
    report += "Mod directory: "+Utf8(request.modDir)+"\r\nGame directory: "+Utf8(request.gameDir)+"\r\n";
    fs::path text = stem; text += L".txt";
    if (FILE* f = _wfopen(text.c_str(),L"wb")) { fwrite(report.data(),1,report.size(),f); fclose(f); }
    fs::path temporary = stem; temporary += L".zip.tmp";
    fs::path archive = stem; archive += L".zip";
    FILE* zipFile = _wfopen(temporary.c_str(),L"wb");
    mz_zip_archive zip = {};
    bool ok = zipFile && mz_zip_writer_init_cfile(&zip,zipFile,0);
    if (ok) {
        ok = mz_zip_writer_add_mem(&zip,"report.txt",report.data(),report.size(),MZ_DEFAULT_COMPRESSION) != 0;
        if (written) {
            FILE* input = _wfopen(dump.c_str(),L"rb");
            ok = input && mz_zip_writer_add_cfile(&zip,"crash.dmp",input,fs::file_size(dump),nullptr,nullptr,0,MZ_DEFAULT_COMPRESSION,nullptr,0,nullptr,0) && ok;
            if (input) fclose(input);
        }
        const fs::path mod(request.modDir), game(request.gameDir);
        ok = AddFile(zip,mod/L"Dust.ini","config/Dust.ini",1024*1024) && ok;
        for (const auto& p : Files(mod/L"effects",L"",L".ini"))
            ok = AddFile(zip,p,"config/effects/"+Utf8(p.filename().c_str()),1024*1024) && ok;
        for (const auto& prefix : {L"Dust_",L"DustBoot_"}) {
            auto logs = Files(mod/L"logs",prefix,L".log");
            if (!logs.empty()) ok = AddFile(zip,logs.back(),"logs/"+Utf8(logs.back().filename().c_str()),2*1024*1024) && ok;
        }
        for (const auto& name : {L"kenshi.log",L"kenshi_info.log",L"RE_Kenshi_log.txt",L"settings.cfg",L"currentVersion.txt"})
            ok = AddFile(zip,game/name,"game/"+Utf8(name),512*1024) && ok;
        ok = AddFile(zip,game/L"data/mods.cfg","game/mods.cfg",1024*1024) && ok;
        ok = mz_zip_writer_finalize_archive(&zip) && ok;
        mz_zip_writer_end(&zip);
    }
    if (zipFile) fclose(zipFile);
    if (ok && MoveFileExW(temporary.c_str(),archive.c_str(),MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(dump.c_str()); DeleteFileW(text.c_str());
        // Keep five completed reports; never recurse or touch unrelated files.
        auto old = Files(output,L"DustCrash_",L".zip");
        for (size_t i = 0; i+5 < old.size(); ++i) DeleteFileW(old[i].c_str());
    } else DeleteFileW(temporary.c_str()); // raw dump/text remain usable
    return written ? 0 : 4;
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 5) return 1;
    HANDLE map = reinterpret_cast<HANDLE>(_wcstoui64(argv[1],nullptr,10));
    HANDLE signal = reinterpret_cast<HANDLE>(_wcstoui64(argv[2],nullptr,10));
    HANDLE done = reinterpret_cast<HANDLE>(_wcstoui64(argv[3],nullptr,10));
    HANDLE process = reinterpret_cast<HANDLE>(_wcstoui64(argv[4],nullptr,10));
    auto* request = static_cast<DustCrash::Request*>(MapViewOfFile(map,FILE_MAP_ALL_ACCESS,0,0,sizeof(DustCrash::Request)));
    if (!request || request->protocol != DustCrash::Protocol || GetProcessId(process) != request->processId) return 2;
    SetEvent(done); // ready before the game installs its handler
    HANDLE waits[] = {signal,process};
    DWORD wait = WaitForMultipleObjects(2,waits,FALSE,INFINITE);
    int result = 0;
    if (wait == WAIT_OBJECT_0 && request->protocol == DustCrash::Protocol) {
        try { result = Capture(*request,process,done); }
        catch (...) { SetEvent(done); result = 3; }
    }
    UnmapViewOfFile(request);
    for (HANDLE handle : {map,signal,done,process}) CloseHandle(handle);
    return result;
}
