#include "../boot/LoadOrder.h"
#include <filesystem>
#include <fstream>
#include <cassert>
#include <cstdio>
namespace fs = std::filesystem;
static bool gBootActive = false, gHooked = false, pinSucceeds = true;
static int reads = 0, pins = 0, logInit = 0, logs = 0, reporters = 0, hooks = 0;
static fs::path executable;
static bool DustEnabledInLoadOrder() { ++reads; return DustLoadOrder::EnabledForExecutable(executable.c_str()); }
static bool PinBootModule() { ++pins; return pinSucceeds; }
static void BootLogInit() { ++logInit; }
static void BootLog(const char*,...) { ++logs; }
static void StartCrashReporting() { ++reporters; }
static bool InstallFactoryHooks() { ++hooks; return true; }
#include "build/BootStartup.generated.h"
static void Reset() { gBootActive = gHooked = false; reads = pins = logInit = logs = reporters = hooks = 0; pinSucceeds = true; }
int main() {
    using DustLoadOrder::ContainsDust;
    for (const char* value : {"Dust.mod", "Other.mod\r\nDust.mod\r\n", "\xef\xbb\xbf" "Dust.mod\n", "\t dUsT.mOd \r\n"}) assert(ContainsDust(value));
    for (const char* value : {"", "Other.mod\n", "#Dust.mod\n", "; Dust.mod\n", "Dust.mod.disabled", "MyDust.mod", "Dust.mod Patch.mod", "Dust.mod # disabled"}) assert(!ContainsDust(value));
    std::string wide("\xff\xfe",2);
    for (char c : std::string("Other.mod\r\nDust.mod\r\n")) { wide += c; wide += '\0'; }
    assert(ContainsDust(wide)); wide.pop_back(); assert(!ContainsDust(wide));
    std::string big("\xfe\xff",2);
    for (char c : std::string("Dust.mod")) { big += '\0'; big += c; }
    assert(ContainsDust(big));
    const fs::path root = fs::absolute(fs::path("build")/(L"load order \u00e9 "+std::to_wstring(GetCurrentProcessId())));
    fs::create_directories(root/L"game/data"); fs::create_directories(root/L"wrong-cwd/data");
    executable = root/L"game/kenshi_x64.exe";
    auto config = root/L"game/data/mods.cfg";
    fs::create_directories(root/L"game/mods/Dust");
    auto modFile = root/L"game/mods/Dust/Dust.mod";
    { std::ofstream physicalMod(modFile); physicalMod << "present on disk, but disabled"; }
    const auto oldCwd = fs::current_path(); fs::current_path(root/L"wrong-cwd");
    { std::ofstream wrong(root/L"wrong-cwd/data/mods.cfg"); wrong << "Dust.mod\n"; }
    auto inactive = [&] {
        Reset(); startPlugin();
        assert(reads == 1 && pins == 0 && logInit == 0 && logs == 0 && reporters == 0 && hooks == 0 && !gBootActive && !gHooked);
    };
    inactive(); // no mods.cfg entry, despite Dust.mod on disk and enabled in a different CWD
    for (const char* value : {"", "Other.mod\r\n", "Dust.mod.disabled\r\n", "#Dust.mod\n"}) {
        { std::ofstream output(config,std::ios::binary); output << value; }
        inactive();
    }
    assert(DeleteFileW(modFile.c_str()));
    assert(!fs::exists(modFile)); // the entry alone must pass the activation guard
    { std::ofstream output(config); output << "Dust.mod\n"; }
    HANDLE locked = CreateFileW(config.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    assert(locked != INVALID_HANDLE_VALUE); inactive(); CloseHandle(locked);
    Reset(); pinSucceeds = false; startPlugin();
    assert(pins == 1 && !gBootActive && !logInit && !reporters && !hooks);
    Reset(); startPlugin(); startPlugin();
    assert(gBootActive && gHooked && reads == 1 && pins == 1 && logInit == 1 && reporters == 1 && hooks == 1);
    fs::current_path(oldCwd);
    // Exercise the actual built preload DLL too. This test executable's folder
    // has no data/mods.cfg: startPlugin must return without pinning the DLL.
    const auto stub = fs::absolute("build/BootDependencyStub/KenshiLib.dll");
    HMODULE dependency = LoadLibraryW(stub.c_str()); assert(dependency);
    const auto dll = fs::absolute("../boot/build/Release/DustBoot.dll");
    HMODULE module = LoadLibraryW(dll.c_str()); assert(module);
    auto start = reinterpret_cast<void(*)()>(GetProcAddress(module,"?startPlugin@@YAXXZ"));
    assert(start); start();
    assert(FreeLibrary(module));
    assert(GetModuleHandleW(L"DustBoot.dll") == nullptr);
    assert(FreeLibrary(dependency));
    std::puts("Disabled/missing/unreadable load order leaves startup inert; exact enabled entry activates once before hooks");
}
