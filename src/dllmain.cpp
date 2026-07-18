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
#include "PssmDetour.h"
#include "CameraAccess.h"

static HMODULE gDllModule = nullptr;

// ==================== Shutdown exception filter ====================
// Once our DllMain DETACH has run (gShutdownSignaled), any further unhandled
// exception is post-cleanup noise — typically RE_Kenshi/Kenshi shutdown bugs
// that surface now that our hook trampolines pass through cleanly instead of
// crashing first. Swallow these to avoid the OS error dialog. Real bugs
// during gameplay still go through to the previous filter (RE_Kenshi's).

static LPTOP_LEVEL_EXCEPTION_FILTER gPreviousExceptionFilter = nullptr;

static LONG WINAPI DustShutdownExceptionFilter(EXCEPTION_POINTERS* ep)
{
    if (D3D11Hook::IsShutdownSignaled())
        TerminateProcess(GetCurrentProcess(), 0);
    return gPreviousExceptionFilter ? gPreviousExceptionFilter(ep)
                                    : EXCEPTION_CONTINUE_SEARCH;
}

// ==================== Utility ====================

static std::string GetModuleDir(HMODULE hModule)
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos + 1) : s;
}

static bool FileExists(const std::string& path)
{
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Resolve the game install dir from the HOST PROCESS EXE, not the DLL location. Walking up
// from the DLL is wrong for Steam Workshop installs (the DLL lives in
// steamapps/workshop/content/233860/<id>/, so "up 2 dirs" lands in workshop/content/): the
// shader-cache stamp was read+written in a phantom RE_Kenshi dir there, always matched
// itself, and the REAL RE_Kenshi/shader_cache.sc was never invalidated. Stale bytecode
// compiled before the MV injection then loaded from cache and paired MV-less vertex shaders
// with MV pixel shaders — the injected TEXCOORD12/13 inputs read undefined interpolants and
// the velocity became a function of screen position (the quadrant-coloured trees/foliage in
// the MV debug view). Same derivation as BugReport's GetGameDir.
static std::string GetGameDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path);
    auto pos = dir.find_last_of("\\/");
    dir = (pos != std::string::npos) ? dir.substr(0, pos + 1) : dir;

    // The exe dir is not necessarily the game root: RE_Kenshi launches a patched copy of
    // kenshi_x64.exe from <game>/RE_Kenshi/. Walk up until the game's marker files appear.
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

    // Bump this suffix when ShaderPatch HLSL injection changes, so RE_Kenshi
    // discards cached bytecode that was compiled with an older injection.
    // mvend: the injected MV interpolants are now APPENDED to each entry function's parameter list
    // instead of inserted after a TEXCOORDn anchor. Mid-list insertion shifted every later parameter
    // down a register — with COLOURING it moved the vertex COLOR0 from reg7 to reg9 while Kenshi's
    // un-injected forward icon shader (rtticons.hlsl, backwards-compat => links by REGISTER) kept
    // reading reg7 and got the clip position as the item's colour. Every cached shader from before
    // this change has the old register layout, so the bump is required.
    // mvsgv: the injected PS inputs are now placed BEFORE any SV_IsFrontFace parameter — appending
    // after it is an fxc error (X4576) that silently dropped the whole MV injection from every
    // DOUBLESIDED variant via the original-source compile fallback.
    stamp += "|patch=shadow-b7-csm-r9-mvsgv";
    return stamp;
}

static void ManageShaderCache(const std::string& gameDir, const std::string& modDir)
{
    std::string cachePath = gameDir + "RE_Kenshi\\shader_cache.sc";
    std::string stampPath = gameDir + "RE_Kenshi\\dust_cache_stamp.txt";

    // One-time cleanup of the phantom RE_Kenshi dir older builds created by deriving the
    // game dir from the DLL path (steamapps/workshop/content/ for Workshop installs).
    {
        std::string legacy;
        auto p1 = modDir.find_last_of("\\/", modDir.size() - 2);
        if (p1 != std::string::npos)
        {
            std::string up1 = modDir.substr(0, p1);
            auto p2 = up1.find_last_of("\\/");
            if (p2 != std::string::npos) legacy = up1.substr(0, p2 + 1);
        }
        if (!legacy.empty() && legacy != gameDir &&
            DeleteFileA((legacy + "RE_Kenshi\\dust_cache_stamp.txt").c_str()))
        {
            RemoveDirectoryA((legacy + "RE_Kenshi").c_str());   // no-op unless now empty
            Log("Removed stale cache stamp at legacy path %sRE_Kenshi\\", legacy.c_str());
        }
    }

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

    // Hand the GameWorld* to the camera-access helper so the MV pass can read the OGRE
    // camera's exact view-projection this frame (same thread as the D3D draw hooks).
    CameraAccess_SetGameWorld(thisptr);

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
    // Install our shutdown exception filter as early as possible. Chains to
    // RE_Kenshi's filter (installed earlier) so legitimate gameplay crashes
    // still surface their dialog.
    gPreviousExceptionFilter = SetUnhandledExceptionFilter(DustShutdownExceptionFilter);

    // Init logging (on by default; FileLogging=0 in Dust.ini opts out) and
    // rotate old logs so at most MaxLogFiles (default 10) sessions are kept
    DustLogInit(gDllModule);

#define DUST_STR2(x) #x
#define DUST_STR(x) DUST_STR2(x)
#ifdef DUST_VERSION
    Log("Dust v" DUST_STR(DUST_VERSION) " loading...");
#else
    Log("Dust (dev) loading...");
#endif

    // Manage shader cache: only invalidate when Dust version or config changes
    std::string gameDir = GetGameDir();
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

    // PSSM cascade-splits detour (live Cascade Lambda for CSM shadows).
    // Installs here (early in startPlugin) so it's in place before Kenshi's
    // shadow node init, which is when the splits are written. Late-stage
    // installs (post-gGameAlive) miss the capture.
    PssmDetour::TryInstall();

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
        // GET_MODULE_HANDLE_EX_FLAG_PIN pins permanently, with no path lookup (the
        // old GetModuleFileNameA + LoadLibraryA pin could silently fail on MAX_PATH
        // truncation, leaving the DLL unloadable with hooks live).
        {
            HMODULE hPin = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN |
                                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                    (LPCSTR)hModule, &hPin))
            {
                // Fallback: refcount bump via path, checked this time.
                char selfPath[MAX_PATH];
                if (!GetModuleFileNameA(hModule, selfPath, MAX_PATH) ||
                    !LoadLibraryA(selfPath))
                    OutputDebugStringA("[Dust] FATAL: could not pin module — hooks may dangle on unload\n");
            }
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
