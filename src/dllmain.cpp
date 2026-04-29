#include <d3d11.h>
#include <string>

// KenshiLib headers
#include <kenshi/GameWorld.h>
#include <core/Functions.h>

// Dust framework
#include "DustLog.h"
#include "D3D11Hook.h"
#include "DustGUI.h"
#include "EffectLoader.h"

static HMODULE gDllModule = nullptr;

// ==================== Utility ====================

static std::string GetModuleDir(HMODULE hModule)
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos + 1) : s;
}

static std::string GetGameDir(HMODULE hModule)
{
    // DLL is at <game>/mods/Dust/Dust.dll -- go up 2 dirs
    std::string modDir = GetModuleDir(hModule);
    auto pos = modDir.find_last_of("\\/", modDir.size() - 2);
    if (pos != std::string::npos)
    {
        std::string modsDir = modDir.substr(0, pos);
        pos = modsDir.find_last_of("\\/");
        if (pos != std::string::npos)
            return modsDir.substr(0, pos + 1);
    }
    return modDir;
}

// Build a stamp string that changes when any shader-affecting config changes.
// If the stamp matches the stored one, the cached bytecode already has our
// patches baked in and D3DCompile can be skipped for cached shaders.
static std::string BuildCacheStamp(const std::string& modDir)
{
#ifdef DUST_VERSION
    #define DUST_STAMP_STR2(x) #x
    #define DUST_STAMP_STR(x) DUST_STAMP_STR2(x)
    std::string stamp = "dust|" DUST_STAMP_STR(DUST_VERSION);
#else
    std::string stamp = "dust|dev";
#endif

    return stamp;
}

static void ManageShaderCache(const std::string& gameDir, const std::string& modDir)
{
    std::string cachePath = gameDir + "RE_Kenshi\\shader_cache.sc";
    std::string stampPath = gameDir + "RE_Kenshi\\dust_cache_stamp.txt";

    std::string currentStamp = BuildCacheStamp(modDir);

    // Read existing stamp
    std::string storedStamp;
    {
        FILE* f = fopen(stampPath.c_str(), "r");
        if (f)
        {
            char buf[256] = {};
            if (fgets(buf, sizeof(buf), f))
                storedStamp = buf;
            fclose(f);
            // Strip trailing newline
            while (!storedStamp.empty() &&
                   (storedStamp.back() == '\n' || storedStamp.back() == '\r'))
                storedStamp.pop_back();
        }
    }

    if (storedStamp == currentStamp)
    {
        Log("Shader cache stamp matches (%s), keeping cached bytecode", currentStamp.c_str());
        return;
    }

    // Stamp mismatch — invalidate cache and write new stamp
    if (DeleteFileA(cachePath.c_str()))
        Log("Invalidated RE_Kenshi shader cache (stamp changed: '%s' -> '%s')",
            storedStamp.c_str(), currentStamp.c_str());
    else
        Log("Shader cache not present or already clean (new stamp: %s)", currentStamp.c_str());

    // Ensure RE_Kenshi directory exists
    std::string reDir = gameDir + "RE_Kenshi";
    CreateDirectoryA(reDir.c_str(), nullptr);

    FILE* f = fopen(stampPath.c_str(), "w");
    if (f)
    {
        fprintf(f, "%s\n", currentStamp.c_str());
        fclose(f);
    }
}

// ==================== Game loop hook ====================

void (*GameWorld__mainLoop_GPUSensitiveStuff_orig)(GameWorld* thisptr, float time);

void GameWorld__mainLoop_GPUSensitiveStuff_hook(GameWorld* thisptr, float time)
{
    if (D3D11Hook::IsShutdownSignaled())
    {
        GameWorld__mainLoop_GPUSensitiveStuff_orig(thisptr, time);
        return;
    }

    // Diagnostic: confirm game loop is actually firing. Pairs with Present diagnostic
    // in D3D11Hook so we can tell apart "game frozen" vs "Present hook bypassed".
    static uint64_t sLoopCount = 0;
    ++sLoopCount;
    if (sLoopCount <= 5 || (sLoopCount <= 600 && (sLoopCount % 60) == 0))
        Log("GameLoop #%llu", (unsigned long long)sLoopCount);

    // Watchdog: if Present hasn't fired, periodically retry swap chain discovery.
    // Layer 3 fallback — covers cases where both DustBoot and initial discovery missed.
    // Retries at frame 120, 300, 600, 1200 (then stops — if it hasn't worked by ~20s, give up).
    if (!D3D11Hook::IsPresentHooked() &&
        (sLoopCount == 120 || sLoopCount == 300 || sLoopCount == 600 || sLoopCount == 1200))
    {
        Log("WARNING: Present hook has not fired after %llu game loops — attempting recovery",
            (unsigned long long)sLoopCount);
        D3D11Hook::TryRecoverPresent();
    }

    // Reset per-frame state before the game renders
    D3D11Hook::ResetFrameState();

    // Call original game loop
    GameWorld__mainLoop_GPUSensitiveStuff_orig(thisptr, time);
}

// ==================== Plugin entry point ====================

// RE_Kenshi calls GetProcAddress(plugin, "?startPlugin@@YAXXZ") — C++ mangled name.
// Do NOT use extern "C" here.
__declspec(dllexport) void startPlugin()
{
    // Init logging (reads Logging=1/0 from Dust.ini next to the DLL)
    DustLogInit(gDllModule);

#define DUST_STR2(x) #x
#define DUST_STR(x) DUST_STR2(x)
#ifdef DUST_VERSION
    Log("Dust v" DUST_STR(DUST_VERSION) " loading...");
#else
    Log("Dust (dev) loading...");
#endif

    // Manage shader cache: only invalidate when Dust version or config changes
    std::string gameDir = GetGameDir(gDllModule);
    std::string modDir = GetModuleDir(gDllModule);
    ManageShaderCache(gameDir, modDir);

    // Load effect plugins from effects/ directory next to the DLL
    {
        std::string effectsDir = modDir + "effects";
        int loaded = gEffectLoader.LoadAll(effectsDir.c_str());
        Log("Loaded %d effect plugin(s) from %s", loaded, effectsDir.c_str());
    }

    // Hook game loop for per-frame state reset
    KenshiLib::HookStatus status = KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
        &GameWorld__mainLoop_GPUSensitiveStuff_hook,
        &GameWorld__mainLoop_GPUSensitiveStuff_orig);

    if (status != KenshiLib::SUCCESS)
    {
        Log("ERROR: Failed to hook GameWorld::_NV_mainLoop_GPUSensitiveStuff");
        ErrorLog("Dust: could not install game loop hook!");
        return;
    }

    Log("Game loop hook installed");

    // Install D3D11 hooks — creates a temporary device to discover function
    // addresses, then hooks them via KenshiLib::AddHook. The real device/context
    // are captured from the first hooked call. No OGRE dependency.
    if (!D3D11Hook::Install())
    {
        Log("ERROR: D3D11Hook::Install failed");
        ErrorLog("Dust: could not install D3D11 hooks!");
        return;
    }

    Log("All hooks installed, %zu effect plugin(s) loaded, waiting for first D3D11 call...",
        gEffectLoader.Count());
}

// ==================== DllMain ====================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        gDllModule = hModule;
        // Pin the DLL so FreeLibrary can never unmap it while KenshiLib trampoline
        // hooks are still pointing into our code. The hooks can't be removed, so
        // any unload would leave dangling jumps and crash on the next call.
        {
            char selfPath[MAX_PATH];
            GetModuleFileNameA(hModule, selfPath, MAX_PATH);
            LoadLibraryA(selfPath);
        }
        break;
    case DLL_PROCESS_DETACH:
        // Tell hook trampolines to pass through — any in-flight call from another
        // thread (or DXGI) must skip our logic now that teardown has begun.
        D3D11Hook::SignalShutdown();
        // lpReserved != nullptr means the process is terminating; the OS reclaims
        // everything, and running our cleanup after rekenshi has begun unwinding
        // is what was crashing on exit.
        if (lpReserved) break;
        DustGUI::Shutdown();
        gEffectLoader.ShutdownAll();
        break;
    }
    return TRUE;
}
