#include "BugReport.h"
#include "DustLog.h"
#include "EffectLoader.h"

#include <dxgi.h>
#include <psapi.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../external/miniz/miniz.h"

#pragma comment(lib, "version.lib")

// Version string (DUST_VERSION is injected at build time by CI from git tags)
#define DUST_STR2(x) #x
#define DUST_STR(x) DUST_STR2(x)
#ifdef DUST_VERSION
static const char* DUST_VERSION_STR = "v" DUST_STR(DUST_VERSION);
#else
static const char* DUST_VERSION_STR = "dev";
#endif

namespace BugReport
{

// ==================== State ====================

// One file to copy into the zip. Read on the worker thread so the render
// thread never touches the disk.
struct FileEntry {
    std::string zipName;   // path inside the archive (forward slashes)
    std::string srcPath;   // absolute path on disk
    size_t      tailCap;   // 0 = copy whole file; else keep only the last N bytes
};

struct ReportJob {
    std::string zipPath;
    std::string reportText;             // report.txt body (manifest appended by worker)
    std::vector<FileEntry> files;
};

static std::atomic<int> gStatus{ (int)Status::Idle };
static std::mutex       gResultMutex;
static std::string      gResultPath;
static std::string      gError;
static std::string      gSummary;

// ==================== Small helpers ====================

static void Appendf(std::string& out, const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    out += buf;
}

static std::string WideToUtf8(const wchar_t* w)
{
    if (!w || !*w) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
}

static bool FileExists(const std::string& path)
{
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string GetGameDir()
{
    // Start from the host process EXE. Unlike walking up from the DLL, this
    // is correct for both <game>/mods/Dust and Steam Workshop installs.
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path);
    auto pos = dir.find_last_of("\\/");
    dir = (pos != std::string::npos) ? dir.substr(0, pos + 1) : dir;

    // The exe dir is not necessarily the game root: RE_Kenshi launches a
    // patched copy of kenshi_x64.exe from <game>/RE_Kenshi/. Walk up until a
    // directory contains the game's marker files.
    std::string probe = dir;
    for (int i = 0; i < 3; i++)
    {
        if (FileExists(probe + "settings.cfg") || FileExists(probe + "currentVersion.txt"))
            return probe;
        size_t p = probe.find_last_of("\\/", probe.size() - 2);
        if (p == std::string::npos) break;
        probe = probe.substr(0, p + 1);
    }
    return dir; // no markers found anywhere — fall back to the exe dir
}

static std::string TimestampString()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d-%02d-%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// Read a whole file (or its tail if tailCap > 0 and the file is larger).
// Returns false if the file can't be opened.
static bool ReadFileMaybeTail(const std::string& path, size_t tailCap, std::string& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); return false; }

    long start = 0;
    if (tailCap > 0 && (size_t)size > tailCap)
        start = size - (long)tailCap;

    fseek(f, start, SEEK_SET);
    out.clear();
    if (start > 0)
        Appendf(out, "[... truncated by Dust bug report: showing last %ld of %ld bytes ...]\r\n",
                size - start, size);

    size_t want = (size_t)(size - start);
    size_t off = out.size();
    out.resize(off + want);
    size_t got = fread(&out[off], 1, want, f);
    out.resize(off + got);
    fclose(f);
    return true;
}

// First line of a small text file, trimmed.
static std::string ReadFirstLine(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return std::string();
    char buf[256] = {};
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return std::string(); }
    fclose(f);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// File version resource, e.g. "1.0.2.0". Empty if unavailable.
static std::string GetDllFileVersion(const std::string& path)
{
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeA(path.c_str(), &handle);
    if (!size) return std::string();

    std::vector<char> data(size);
    if (!GetFileVersionInfoA(path.c_str(), 0, size, data.data())) return std::string();

    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (!VerQueryValueA(data.data(), "\\", (void**)&ffi, &ffiLen) || !ffi)
        return std::string();

    char buf[64];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
             HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return buf;
}

// RE_Kenshi.dll ships without a version resource (0.0.0.0), but its log's
// first line is e.g. "0.000 RE_Kenshi: RE_Kenshi 0.3.4" and the log is
// rewritten every session, so it always matches the running version.
static std::string GetReKenshiVersion(const std::string& gameDir)
{
    std::string line = ReadFirstLine(gameDir + "RE_Kenshi_log.txt");
    size_t pos = line.rfind("RE_Kenshi ");
    if (pos != std::string::npos)
    {
        std::string v = line.substr(pos + 10);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        if (!v.empty()) return v;
    }
    std::string dllVer = GetDllFileVersion(gameDir + "RE_Kenshi.dll");
    return (dllVer == "0.0.0.0") ? std::string() : dllVer;
}

// ==================== System info sections ====================

static void AppendOsInfo(std::string& out)
{
    // GetVersionEx lies without a manifest; RtlGetVersion doesn't.
    typedef LONG(WINAPI* PFN_RtlGetVersion)(RTL_OSVERSIONINFOW*);
    RTL_OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    PFN_RtlGetVersion rtlGetVersion = ntdll
        ? (PFN_RtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
    if (rtlGetVersion && rtlGetVersion(&vi) == 0)
    {
        const char* name = "Windows";
        if (vi.dwMajorVersion == 10)
            name = (vi.dwBuildNumber >= 22000) ? "Windows 11" : "Windows 10";
        Appendf(out, "OS: %s (build %u.%u.%u)\r\n",
                name, vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    }
    else
    {
        Appendf(out, "OS: Windows (version query failed)\r\n");
    }
}

static void AppendCpuRamInfo(std::string& out)
{
    // CPU brand string via CPUID
    int regs[4] = {};
    char brand[49] = {};
    __cpuid(regs, 0x80000000);
    if ((unsigned)regs[0] >= 0x80000004)
    {
        __cpuid((int*)&brand[0],  0x80000002);
        __cpuid((int*)&brand[16], 0x80000003);
        __cpuid((int*)&brand[32], 0x80000004);
        brand[48] = '\0';
    }
    // Vendors pad the 48-char brand string with spaces on either side
    char* end = brand + strlen(brand);
    while (end > brand && end[-1] == ' ') *--end = '\0';
    const char* b = brand;
    while (*b == ' ') b++;

    SYSTEM_INFO si = {};
    GetNativeSystemInfo(&si);

    Appendf(out, "CPU: %s (%u logical processors)\r\n",
            *b ? b : "(unknown)", si.dwNumberOfProcessors);

    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        Appendf(out, "RAM: %llu MB total, %llu MB available\r\n",
                ms.ullTotalPhys / (1024ull * 1024ull),
                ms.ullAvailPhys / (1024ull * 1024ull));
}

static const char* GpuVendorName(UINT vendorId)
{
    switch (vendorId)
    {
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    case 0x1414: return "Microsoft (software)";
    default:     return "Unknown vendor";
    }
}

static void AppendGpuInfo(std::string& out, ID3D11Device* device)
{
    // LUID of the adapter the game is actually rendering on
    LUID usedLuid = {};
    bool haveUsedLuid = false;
    if (device)
    {
        IDXGIDevice* dxgiDev = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && dxgiDev)
        {
            IDXGIAdapter* usedAdapter = nullptr;
            if (SUCCEEDED(dxgiDev->GetAdapter(&usedAdapter)) && usedAdapter)
            {
                DXGI_ADAPTER_DESC d = {};
                if (SUCCEEDED(usedAdapter->GetDesc(&d)))
                {
                    usedLuid = d.AdapterLuid;
                    haveUsedLuid = true;
                }
                usedAdapter->Release();
            }
            dxgiDev->Release();
        }

        UINT fl = (UINT)device->GetFeatureLevel();
        Appendf(out, "D3D feature level: %u_%u\r\n", (fl >> 12) & 0xF, (fl >> 8) & 0xF);
    }

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory)
    {
        Appendf(out, "GPU: (DXGI factory creation failed)\r\n");
        return;
    }

    Appendf(out, "GPUs:\r\n");
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
    {
        DXGI_ADAPTER_DESC1 d = {};
        if (SUCCEEDED(adapter->GetDesc1(&d)))
        {
            bool inUse = haveUsedLuid &&
                d.AdapterLuid.LowPart == usedLuid.LowPart &&
                d.AdapterLuid.HighPart == usedLuid.HighPart;

            // UMD driver version (works for all vendors)
            char driverStr[64] = "unknown";
            LARGE_INTEGER umd = {};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                snprintf(driverStr, sizeof(driverStr), "%u.%u.%u.%u",
                         HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                         HIWORD(umd.LowPart),  LOWORD(umd.LowPart));

            Appendf(out, "  - %s | %s | VRAM %llu MB | driver %s | vendor 0x%04X device 0x%04X%s%s\r\n",
                    WideToUtf8(d.Description).c_str(),
                    GpuVendorName(d.VendorId),
                    (unsigned long long)(d.DedicatedVideoMemory / (1024ull * 1024ull)),
                    driverStr, d.VendorId, d.DeviceId,
                    (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " [software]" : "",
                    inUse ? "  <== in use" : "");
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
}

struct MonitorEnumState {
    std::string* out;
    int index;
};

static BOOL CALLBACK MonitorEnumProc(HMONITOR mon, HDC, LPRECT, LPARAM lp)
{
    MonitorEnumState* state = (MonitorEnumState*)lp;

    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(mon, (MONITORINFO*)&mi)) return TRUE;

    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    BOOL haveMode = EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);

    // Effective DPI (per-monitor), loaded dynamically — shcore is Win8.1+
    UINT dpiX = 0, dpiY = 0;
    typedef HRESULT(WINAPI* PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
    static PFN_GetDpiForMonitor sGetDpiForMonitor =
        (PFN_GetDpiForMonitor)GetProcAddress(LoadLibraryA("shcore.dll"), "GetDpiForMonitor");
    if (sGetDpiForMonitor)
        sGetDpiForMonitor(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY);

    Appendf(*state->out, "  - %s: ", mi.szDevice);
    if (haveMode)
        Appendf(*state->out, "%ux%u @ %u Hz, %u bpp",
                dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, dm.dmBitsPerPel);
    else
        Appendf(*state->out, "%ldx%ld",
                mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    Appendf(*state->out, ", pos (%ld,%ld)", mi.rcMonitor.left, mi.rcMonitor.top);
    if (dpiX)
        Appendf(*state->out, ", scale %u%%", dpiX * 100 / 96);
    if (mi.dwFlags & MONITORINFOF_PRIMARY)
        Appendf(*state->out, " [primary]");
    Appendf(*state->out, "\r\n");

    state->index++;
    return TRUE;
}

static void AppendMonitorInfo(std::string& out)
{
    Appendf(out, "Monitors:\r\n");
    MonitorEnumState state = { &out, 0 };
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, (LPARAM)&state);
    if (state.index == 0)
        Appendf(out, "  (none enumerated)\r\n");
}

// ==================== Overlay / injected-module detection ====================

static void AppendModuleScan(std::string& out, const std::string& gameDir, const std::string& modDir)
{
    struct Known { const char* module; const char* label; };
    static const Known kKnown[] = {
        { "gameoverlayrenderer64.dll", "Steam overlay" },
        { "rtsshooks64.dll",           "RivaTuner Statistics Server (RTSS / MSI Afterburner OSD)" },
        { "graphics-hook64.dll",       "OBS Studio game capture" },
        { "discordhook64.dll",         "Discord overlay" },
        { "nvspcap64.dll",             "NVIDIA ShadowPlay / GeForce overlay" },
        { "specialk64.dll",            "Special K" },
        { "reshade64.dll",             "ReShade" },
        { "fraps64.dll",               "Fraps" },
        { "bdcam64.dll",               "Bandicam" },
    };

    HANDLE proc = GetCurrentProcess();
    HMODULE mods[2048];
    DWORD needed = 0;
    if (!EnumProcessModules(proc, mods, sizeof(mods), &needed))
    {
        Appendf(out, "(module enumeration failed)\r\n");
        return;
    }
    int count = (int)(needed / sizeof(HMODULE));
    if (count > 2048) count = 2048;

    // Lowercase copies of dirs for prefix matching
    auto toLower = [](std::string s) {
        for (char& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    std::string gameDirL = toLower(gameDir);
    std::string modDirL  = toLower(modDir);

    std::string detected;
    std::string gameDirModules;

    for (int i = 0; i < count; i++)
    {
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameExA(proc, mods[i], path, MAX_PATH)) continue;
        std::string full = toLower(path);

        std::string base = full;
        auto pos = base.find_last_of("\\/");
        if (pos != std::string::npos) base = base.substr(pos + 1);

        for (const Known& k : kKnown)
            if (base == k.module)
                Appendf(detected, "  - %s (%s)\r\n", k.label, base.c_str());

        // ReShade installs as dxgi.dll/d3d11.dll INSIDE the game dir (the real
        // ones live in System32) — flag any such local system-named DLL.
        if ((base == "dxgi.dll" || base == "d3d11.dll" || base == "d3dcompiler_47.dll")
            && full.compare(0, gameDirL.size(), gameDirL) == 0)
            Appendf(detected, "  - Local %s in game folder (ReShade or another wrapper)\r\n", base.c_str());

        // Everything loaded from the game dir or the Dust mod dir — catches
        // RE_Kenshi plugins and other injected mods.
        if (full.compare(0, gameDirL.size(), gameDirL) == 0 ||
            (!modDirL.empty() && full.compare(0, modDirL.size(), modDirL) == 0))
            Appendf(gameDirModules, "  - %s\r\n", base.c_str());
    }

    Appendf(out, "Overlays / capture software detected:\r\n%s",
            detected.empty() ? "  (none detected)\r\n" : detected.c_str());
    if (FileExists(gameDir + "ReShade.ini"))
        Appendf(out, "  - ReShade.ini present in game folder\r\n");
    Appendf(out, "Modules loaded from game/mod folders:\r\n%s",
            gameDirModules.empty() ? "  (none)\r\n" : gameDirModules.c_str());
}

// ==================== Effects section ====================

static const char* InjectionPointName(DustInjectionPoint p)
{
    switch (p)
    {
    case DUST_INJECT_POST_GBUFFER:  return "POST_GBUFFER";
    case DUST_INJECT_POST_LIGHTING: return "POST_LIGHTING";
    case DUST_INJECT_POST_FOG:      return "POST_FOG";
    case DUST_INJECT_POST_TONEMAP:  return "POST_TONEMAP";
    case DUST_INJECT_PRE_PRESENT:   return "PRE_PRESENT";
    default:                        return "?";
    }
}

// First DUST_SETTING_BOOL = the effect's Enabled flag (same heuristic as DustGUI)
static bool EffectIsEnabled(const LoadedEffect& le)
{
    for (uint32_t i = 0; i < le.desc.settingCount; i++)
    {
        const DustSettingDesc& s = le.desc.settings[i];
        if (s.type == DUST_SETTING_BOOL && s.valuePtr)
            return *(bool*)s.valuePtr;
    }
    return true;
}

static void AppendSettingValue(std::string& out, const DustSettingDesc& s)
{
    switch (s.type)
    {
    case DUST_SETTING_BOOL:
    case DUST_SETTING_HIDDEN_BOOL:
        Appendf(out, "%s", *(bool*)s.valuePtr ? "true" : "false");
        break;
    case DUST_SETTING_FLOAT:
    case DUST_SETTING_HIDDEN_FLOAT:
        Appendf(out, "%.4g", *(float*)s.valuePtr);
        break;
    case DUST_SETTING_INT:
    case DUST_SETTING_HIDDEN_INT:
        Appendf(out, "%d", *(int*)s.valuePtr);
        break;
    case DUST_SETTING_ENUM:
    {
        int v = *(int*)s.valuePtr;
        const char* label = nullptr;
        if (s.enumLabels)
        {
            int n = 0;
            while (s.enumLabels[n]) n++;
            if (v >= 0 && v < n) label = s.enumLabels[v];
        }
        Appendf(out, "%d (%s)", v, label ? label : "?");
        break;
    }
    case DUST_SETTING_COLOR3:
    {
        const float* c = (const float*)s.valuePtr;
        Appendf(out, "(%.3g, %.3g, %.3g)", c[0], c[1], c[2]);
        break;
    }
    default:
        Appendf(out, "?");
        break;
    }
}

static void AppendEffectsSection(std::string& out, std::string& enabledListOut)
{
    size_t count = gEffectLoader.Count();
    size_t initCount = 0;
    for (size_t i = 0; i < count; i++)
        if (gEffectLoader.GetEffect(i).initialized) initCount++;

    Appendf(out, "[Effects]  (%zu loaded, %zu initialized)\r\n", count, initCount);
    for (size_t i = 0; i < count; i++)
    {
        const LoadedEffect& le = gEffectLoader.GetEffect(i);
        const char* name = le.desc.name ? le.desc.name : "Unnamed";
        if (!le.initialized)
        {
            Appendf(out, "  - %-18s INIT FAILED / NOT INITIALIZED\r\n", name);
            continue;
        }
        bool enabled = EffectIsEnabled(le);
        Appendf(out, "  - %-18s api v%u  %-13s %-8s %.2f ms\r\n",
                name, le.desc.apiVersion, InjectionPointName(le.desc.injectionPoint),
                enabled ? "enabled" : "disabled", gEffectLoader.GetEffectGpuTime(i));
        if (enabled)
        {
            if (!enabledListOut.empty()) enabledListOut += ", ";
            enabledListOut += name;
        }
    }

    Appendf(out, "\r\n[Effect settings (live values)]\r\n");
    for (size_t i = 0; i < count; i++)
    {
        const LoadedEffect& le = gEffectLoader.GetEffect(i);
        if (!le.initialized) continue;
        Appendf(out, "--- %s ---\r\n", le.desc.name ? le.desc.name : "Unnamed");
        for (uint32_t j = 0; j < le.desc.settingCount; j++)
        {
            const DustSettingDesc& s = le.desc.settings[j];
            if (!s.name) continue;
            if (s.type == DUST_SETTING_SECTION)
            {
                Appendf(out, "  ## %s\r\n", s.name);
                continue;
            }
            if (!s.valuePtr) continue;
            Appendf(out, "  %s = ", s.name);
            AppendSettingValue(out, s);
            Appendf(out, "\r\n");
        }
    }
}

// ==================== File list assembly ====================

// Add every "*.ini" (or any extension) directly inside dir to the job.
static void AddDirFiles(ReportJob& job, const std::string& dir, const char* pattern,
                        const std::string& zipPrefix, size_t tailCap)
{
    WIN32_FIND_DATAA fd = {};
    std::string search = dir + pattern;
    HANDLE h = FindFirstFileA(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        job.files.push_back({ zipPrefix + fd.cFileName, dir + fd.cFileName, tailCap });
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// Newest N logs matching pattern (timestamp-named, so lexical = chronological).
static void AddNewestLogs(ReportJob& job, const std::string& logsDir, const char* pattern,
                          const char* zipPrefix, int maxLogs, size_t tailCap)
{
    std::vector<std::string> names;
    WIN32_FIND_DATAA fd = {};
    std::string search = logsDir + pattern;
    HANDLE h = FindFirstFileA(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            names.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    std::sort(names.begin(), names.end());
    int start = (int)names.size() > maxLogs ? (int)names.size() - maxLogs : 0;
    for (int i = start; i < (int)names.size(); i++)
        job.files.push_back({ zipPrefix + names[i], logsDir + names[i], tailCap });
}

// ==================== Worker thread ====================

static void WorkerMain(ReportJob* jobPtr)
{
    ReportJob& job = *jobPtr;

    // Read every file first so the manifest can go into report.txt
    struct Loaded { std::string zipName; std::string data; };
    std::vector<Loaded> loaded;
    std::string manifest;
    Appendf(manifest, "\r\n[Bundled files]\r\n");

    for (const FileEntry& fe : job.files)
    {
        std::string data;
        if (ReadFileMaybeTail(fe.srcPath, fe.tailCap, data))
        {
            Appendf(manifest, "  %-40s %zu bytes\r\n", fe.zipName.c_str(), data.size());
            loaded.push_back({ fe.zipName, std::move(data) });
        }
        else
        {
            Appendf(manifest, "  %-40s MISSING (%s)\r\n", fe.zipName.c_str(), fe.srcPath.c_str());
        }
    }

    job.reportText += manifest;

    // Write the zip
    mz_zip_archive zip = {};
    if (!mz_zip_writer_init_file(&zip, job.zipPath.c_str(), 0))
    {
        std::lock_guard<std::mutex> lock(gResultMutex);
        gError = "Could not create zip file at " + job.zipPath;
        gStatus.store((int)Status::Failed);
        Log("BugReport: %s", gError.c_str());
        delete jobPtr;
        return;
    }

    bool ok = mz_zip_writer_add_mem(&zip, "report.txt",
                                    job.reportText.data(), job.reportText.size(),
                                    MZ_DEFAULT_COMPRESSION) != 0;
    for (const Loaded& l : loaded)
        ok = mz_zip_writer_add_mem(&zip, l.zipName.c_str(),
                                   l.data.data(), l.data.size(),
                                   MZ_DEFAULT_COMPRESSION) && ok;

    ok = mz_zip_writer_finalize_archive(&zip) && ok;
    mz_zip_writer_end(&zip);

    {
        std::lock_guard<std::mutex> lock(gResultMutex);
        if (ok)
        {
            gResultPath = job.zipPath;
            gStatus.store((int)Status::Done);
            Log("BugReport: report written to %s (%zu files bundled)",
                job.zipPath.c_str(), loaded.size() + 1);
        }
        else
        {
            gError = "Zip writing failed (disk full or file locked?)";
            gStatus.store((int)Status::Failed);
            Log("BugReport: zip writing failed for %s", job.zipPath.c_str());
        }
    }
    delete jobPtr;
}

// ==================== Report assembly (synchronous part) ====================

bool StartGeneration(const LiveStats& stats)
{
    int expected = (int)Status::Idle;
    if (!gStatus.compare_exchange_strong(expected, (int)Status::Running))
    {
        expected = (int)Status::Done;
        if (!gStatus.compare_exchange_strong(expected, (int)Status::Running))
        {
            expected = (int)Status::Failed;
            if (!gStatus.compare_exchange_strong(expected, (int)Status::Running))
                return false; // still Running
        }
    }

    std::string modDir  = DustLogDir();          // dir of Dust.dll, trailing slash
    std::string gameDir = GetGameDir();          // dir of kenshi_x64.exe, trailing slash
    std::string stamp   = TimestampString();

    ReportJob* job = new ReportJob();

    // ---- Output path ----
    std::string reportDir = modDir + "bug_reports";
    CreateDirectoryA(reportDir.c_str(), nullptr);
    job->zipPath = reportDir + "\\DustReport_" + stamp + ".zip";

    // ---- Versions ----
    std::string kenshiVersion   = ReadFirstLine(gameDir + "currentVersion.txt");
    std::string reKenshiVersion = GetReKenshiVersion(gameDir);

    // ---- Preset ----
    std::string presetName = "(Custom)";
    std::string presetPath;
    {
        int cur = gEffectLoader.GetCurrentPreset();
        const auto& presets = gEffectLoader.GetPresets();
        if (cur >= 0 && cur < (int)presets.size())
        {
            presetName = presets[cur].name;
            presetPath = presets[cur].path;
            if (!presets[cur].warnings.empty())
                presetName += " [OUTDATED]";
        }
    }

    // ---- report.txt ----
    std::string& r = job->reportText;
    Appendf(r, "==================== Dust Bug Report ====================\r\n");
    Appendf(r, "Generated: %s\r\n", stamp.c_str());
    Appendf(r, "This bundle contains your Dust logs/config, Kenshi graphics settings,\r\n");
    Appendf(r, "mod load order and hardware info. Review before sharing if you care\r\n");
    Appendf(r, "about file paths being visible.\r\n\r\n");

    Appendf(r, "[Versions]\r\n");
    Appendf(r, "Dust: %s\r\n", DUST_VERSION_STR);
    Appendf(r, "Kenshi: %s\r\n", kenshiVersion.empty() ? "(unknown)" : kenshiVersion.c_str());
    Appendf(r, "RE_Kenshi: %s\r\n", reKenshiVersion.empty() ? "(unknown)" : reKenshiVersion.c_str());
    {
        FILETIME creation = {}, exit_ = {}, kernel = {}, user = {};
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_, &kernel, &user))
        {
            FILETIME nowFt = {};
            GetSystemTimeAsFileTime(&nowFt);
            ULARGE_INTEGER a, b;
            a.LowPart = creation.dwLowDateTime; a.HighPart = creation.dwHighDateTime;
            b.LowPart = nowFt.dwLowDateTime;    b.HighPart = nowFt.dwHighDateTime;
            Appendf(r, "Session uptime: %llu minutes\r\n", (b.QuadPart - a.QuadPart) / 600000000ull);
        }
    }
    Appendf(r, "\r\n[System]\r\n");
    AppendOsInfo(r);
    AppendCpuRamInfo(r);
    AppendGpuInfo(r, stats.device);
    AppendMonitorInfo(r);

    Appendf(r, "\r\n[Display]\r\n");
    Appendf(r, "Backbuffer: %.0fx%.0f\r\n", stats.displayW, stats.displayH);
    Appendf(r, "FPS at report time: %.1f (%.2f ms)\r\n", stats.fps, stats.frameTimeMs);

    Appendf(r, "\r\n[Dust]\r\n");
    Appendf(r, "Mod dir: %s\r\n", modDir.c_str());
    Appendf(r, "Game dir: %s\r\n", gameDir.c_str());
    Appendf(r, "Logging enabled: %s\r\n", DustLogEnabled() ? "yes" : "no  <-- enable Logging and reproduce the bug for better reports");
    Appendf(r, "Active preset: %s\r\n", presetName.c_str());
    Appendf(r, "\r\n");

    std::string enabledEffects;
    AppendEffectsSection(r, enabledEffects);

    Appendf(r, "\r\n[Environment]\r\n");
    AppendModuleScan(r, gameDir, modDir);

    // ---- Load order summary (file itself is bundled too) ----
    int modCount = -1;
    {
        std::string modsCfg;
        if (ReadFileMaybeTail(gameDir + "data\\mods.cfg", 0, modsCfg))
        {
            modCount = 0;
            for (size_t i = 0; i < modsCfg.size(); i++)
                if (modsCfg[i] == '\n') modCount++;
            if (!modsCfg.empty() && modsCfg.back() != '\n') modCount++;
        }
        Appendf(r, "\r\n[Load order]\r\n");
        if (modCount >= 0)
            Appendf(r, "%d entries in data/mods.cfg (bundled as game/mods.cfg)\r\n", modCount);
        else
            Appendf(r, "data/mods.cfg not found\r\n");
    }

    // ---- Files to bundle ----
    job->files.push_back({ "config/Dust.ini",       modDir + "Dust.ini",           0 });
    job->files.push_back({ "game/RE_Kenshi.json",   modDir + "RE_Kenshi.json",     0 });
    AddDirFiles(*job, modDir + "effects\\", "*.ini", "config/effects/", 0);
    if (!presetPath.empty())
        AddDirFiles(*job, presetPath + "\\", "*.ini", "config/preset/", 0);

    AddNewestLogs(*job, modDir + "logs\\", "Dust_*.log",     "logs/dust/", 3, 2 * 1024 * 1024);
    AddNewestLogs(*job, modDir + "logs\\", "DustBoot_*.log", "logs/dust/", 2, 256 * 1024);

    job->files.push_back({ "game/settings.cfg",     gameDir + "settings.cfg",      0 });
    job->files.push_back({ "game/kenshi.cfg",       gameDir + "kenshi.cfg",        0 });
    job->files.push_back({ "game/mods.cfg",         gameDir + "data\\mods.cfg",    0 });
    job->files.push_back({ "game/RE_Kenshi.ini",    gameDir + "RE_Kenshi.ini",     0 });
    job->files.push_back({ "logs/kenshi.log",       gameDir + "kenshi.log",        512 * 1024 });
    job->files.push_back({ "logs/kenshi_info.log",  gameDir + "kenshi_info.log",   256 * 1024 });
    job->files.push_back({ "logs/RE_Kenshi_log.txt", gameDir + "RE_Kenshi_log.txt", 256 * 1024 });

    // Make the Dust-log situation explicit: with logging disabled the bundle
    // has no log from THIS session, and any bundled logs are from older runs.
    {
        int dustLogs = 0;
        for (const FileEntry& fe : job->files)
            if (fe.zipName.compare(0, 10, "logs/dust/") == 0) dustLogs++;
        Appendf(r, "\r\n[Dust logs]\r\n");
        if (dustLogs == 0)
            Appendf(r, "none bundled%s\r\n",
                    DustLogEnabled() ? " (no log files found)"
                                     : " — logging is disabled, no logs exist");
        else
            Appendf(r, "%d bundled%s\r\n", dustLogs,
                    DustLogEnabled() ? ""
                                     : " — NOTE: logging is disabled this session; these are from OLDER sessions");
    }

    // ---- Short summary (clipboard / GitHub prefill) ----
    {
        std::string s;
        Appendf(s, "Dust: %s | Kenshi: %s | RE_Kenshi: %s\n",
                DUST_VERSION_STR,
                kenshiVersion.empty() ? "?" : kenshiVersion.c_str(),
                reKenshiVersion.empty() ? "?" : reKenshiVersion.c_str());
        AppendOsInfo(s);
        AppendCpuRamInfo(s);
        AppendGpuInfo(s, stats.device);
        Appendf(s, "Backbuffer: %.0fx%.0f | %.1f FPS at report time\n",
                stats.displayW, stats.displayH, stats.fps);
        Appendf(s, "Preset: %s\n", presetName.c_str());
        Appendf(s, "Effects on: %s\n", enabledEffects.empty() ? "(none)" : enabledEffects.c_str());
        if (modCount >= 0)
            Appendf(s, "Mods: %d entries in load order\n", modCount);
        // Sections above use \r\n; normalize for clipboard/URL use
        std::string norm;
        norm.reserve(s.size());
        for (char c : s) if (c != '\r') norm += c;

        std::lock_guard<std::mutex> lock(gResultMutex);
        gSummary = norm;
        gResultPath.clear();
        gError.clear();
    }

    // ---- Hand off to worker ----
    std::thread(WorkerMain, job).detach();
    Log("BugReport: generation started -> %s", job->zipPath.c_str());
    return true;
}

// ==================== Accessors ====================

Status GetStatus()
{
    return (Status)gStatus.load();
}

std::string GetResultPath()
{
    std::lock_guard<std::mutex> lock(gResultMutex);
    return gResultPath;
}

std::string GetError()
{
    std::lock_guard<std::mutex> lock(gResultMutex);
    return gError;
}

std::string GetSummary()
{
    std::lock_guard<std::mutex> lock(gResultMutex);
    return gSummary;
}

std::string BuildGitHubIssueURL()
{
    std::string summary = GetSummary();
    if (summary.size() > 1500)
        summary.resize(1500);

    // Percent-encode for a URL query value
    static const char* hex = "0123456789ABCDEF";
    std::string enc;
    enc.reserve(summary.size() * 3);
    for (unsigned char c : summary)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            enc += (char)c;
        else
        {
            enc += '%';
            enc += hex[c >> 4];
            enc += hex[c & 15];
        }
    }

    // 'sysinfo' matches the field id in .github/ISSUE_TEMPLATE/bug_report.yml
    return "https://github.com/Bazouz660/Dust/issues/new?template=bug_report.yml&sysinfo=" + enc;
}

} // namespace BugReport
