#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>

namespace DustLoadOrder {
inline bool ContainsDust(std::string_view bytes) {
    // mods.cfg normally uses one UTF-8/ANSI filename per line. Accept BOMs
    // from editors too, while keeping the match to a complete filename.
    std::string decoded;
    if (bytes.size() >= 2 && ((unsigned char)bytes[0] == 0xff || (unsigned char)bytes[0] == 0xfe)) {
        bool little = (unsigned char)bytes[0] == 0xff && (unsigned char)bytes[1] == 0xfe;
        bool big = (unsigned char)bytes[0] == 0xfe && (unsigned char)bytes[1] == 0xff;
        if ((!little && !big) || bytes.size()%2) return false;
        for (size_t i = 2; i < bytes.size(); i += 2) {
            unsigned char lo = bytes[i+(big ? 1 : 0)], hi = bytes[i+(little ? 1 : 0)];
            decoded += hi ? '?' : static_cast<char>(lo);
        }
        bytes = decoded;
    } else if (bytes.substr(0,3) == "\xef\xbb\xbf") bytes.remove_prefix(3);

    while (!bytes.empty()) {
        size_t end = bytes.find_first_of("\r\n");
        std::string_view line = bytes.substr(0,end);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        if (line.size() == 8) {
            bool match = true;
            for (size_t i = 0; i < line.size(); ++i) {
                char c = line[i]; if (c >= 'A' && c <= 'Z') c += 'a'-'A';
                if (c != "dust.mod"[i]) { match = false; break; }
            }
            if (match) return true;
        }
        if (end == std::string_view::npos) break;
        bytes.remove_prefix(end+1);
    }
    return false;
}

inline bool EnabledForExecutable(const wchar_t* executable) {
    std::wstring path(executable);
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    path.resize(slash+1); path += L"data\\mods.cfg";
    HANDLE file = CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file,&size) || size.QuadPart <= 0 || size.QuadPart > 4*1024*1024) {
        CloseHandle(file); return false;
    }
    std::vector<char> data(static_cast<size_t>(size.QuadPart)); DWORD read = 0;
    bool ok = ReadFile(file,data.data(),static_cast<DWORD>(data.size()),&read,nullptr) && read == data.size();
    CloseHandle(file);
    return ok && ContainsDust(std::string_view(data.data(),data.size()));
}
}
