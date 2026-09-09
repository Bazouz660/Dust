#pragma once
#include <windows.h>
#include <string>

namespace ShaderCacheStamp
{
enum class Result { Unchanged, Updated, InvalidationFailed, StampWriteFailed };

inline Result Update(const std::string& cachePath, const std::string& stampPath,
                     const std::string& stamp, DWORD& error)
{
    error = ERROR_SUCCESS;
    HANDLE file = CreateFileA(stampPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        char bytes[1024]; DWORD count = 0;
        LARGE_INTEGER size = {};
        const bool read = GetFileSizeEx(file, &size) && size.QuadPart < sizeof(bytes) &&
                          ReadFile(file, bytes, sizeof(bytes), &count, nullptr);
        CloseHandle(file);
        if (read)
        {
            std::string previous(bytes, count);
            while (!previous.empty() && (previous.back() == '\r' || previous.back() == '\n'))
                previous.pop_back();
            if (previous == stamp) return Result::Unchanged;
        }
    }
    if (!DeleteFileA(cachePath.c_str()))
    {
        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            return Result::InvalidationFailed; // NEVER certify cached bytecode we could not remove
    }
    error = ERROR_SUCCESS;
    const auto slash = stampPath.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? "." : stampPath.substr(0, slash);
    char temp[MAX_PATH] = {};
    if (!GetTempFileNameA(dir.c_str(), "dst", 0, temp))
    {
        error = GetLastError();
        return Result::StampWriteFailed;
    }
    file = CreateFileA(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = file != INVALID_HANDLE_VALUE;
    if (!ok) error = GetLastError();
    if (ok)
    {
        const std::string text = stamp + "\n";
        DWORD written = 0;
        ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
             written == text.size() && FlushFileBuffers(file);
        if (!ok) error = GetLastError();
        CloseHandle(file);
    }
    if (ok)
    {
        ok = MoveFileExA(temp, stampPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok) error = GetLastError();
    }
    if (!ok) DeleteFileA(temp);
    return ok ? Result::Updated : Result::StampWriteFailed;
}
}
