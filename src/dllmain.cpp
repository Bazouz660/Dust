#include <d3d11.h>
#include <string>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

// KenshiLib headers
#include <kenshi/GameWorld.h>
#include <core/Functions.h>

// Dust framework
#include "DustLog.h"
#include "D3D11Hook.h"
#include "DustGUI.h"
#include "EffectLoader.h"
#include "ShaderMetadata.h"
#include "ShaderDatabase.h"
#include "ShaderSourceCatalog.h"
#include "GeometryCapture.h"
#include "GeometryReplay.h"
#include "MSAARedirect.h"
#include "DeferredMSAA.h"


static HMODULE gDllModule = nullptr;

// ==================== Crash handler ====================

static LONG WINAPI DustCrashHandler(EXCEPTION_POINTERS* ep)
{
    // Build crash log path next to the DLL
    char logPath[MAX_PATH] = {};
    GetModuleFileNameA(gDllModule, logPath, MAX_PATH);
    char* slash = strrchr(logPath, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(logPath, "dust_crash.log");

    FILE* f = fopen(logPath, "w");
    if (f)
    {
        EXCEPTION_RECORD* rec = ep->ExceptionRecord;
        CONTEXT* ctx = ep->ContextRecord;

        fprintf(f, "Dust Crash Report\n");
        fprintf(f, "Exception code: 0x%08lX\n", rec->ExceptionCode);
        fprintf(f, "Fault address:  0x%p\n", rec->ExceptionAddress);
#ifdef _WIN64
        fprintf(f, "RIP: 0x%016llX  RSP: 0x%016llX\n", ctx->Rip, ctx->Rsp);
#else
        fprintf(f, "EIP: 0x%08lX  ESP: 0x%08lX\n", ctx->Eip, ctx->Esp);
#endif

        // Identify which module the fault is in
        HMODULE hFault = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)rec->ExceptionAddress, &hFault);
        char modName[MAX_PATH] = {};
        if (hFault)
            GetModuleFileNameA(hFault, modName, MAX_PATH);
        fprintf(f, "Faulting module: %s\n", hFault ? modName : "(unknown)");

        // Stack trace via dbghelp
        HANDLE proc = GetCurrentProcess();
        SymInitialize(proc, NULL, TRUE);
        void* stack[64] = {};
        USHORT frames = CaptureStackBackTrace(0, 64, stack, NULL);
        fprintf(f, "\nStack trace (%u frames):\n", frames);
        char symBuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        for (USHORT i = 0; i < frames; i++)
        {
            DWORD64 addr = (DWORD64)stack[i];
            HMODULE hMod = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)stack[i], &hMod);
            char mName[MAX_PATH] = {};
            if (hMod) GetModuleFileNameA(hMod, mName, MAX_PATH);

            if (SymFromAddr(proc, addr, NULL, sym))
                fprintf(f, "  [%2u] 0x%p  %s  (%s)\n", i, stack[i], sym->Name, mName);
            else
                fprintf(f, "  [%2u] 0x%p  (%s)\n", i, stack[i], mName);
        }
        SymCleanup(proc);
        fclose(f);
    }

    return EXCEPTION_CONTINUE_SEARCH;
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
    // Diagnostic: confirm game loop is actually firing. Pairs with Present diagnostic
    // in D3D11Hook so we can tell apart "game frozen" vs "Present hook bypassed".
    static uint64_t sLoopCount = 0;
    ++sLoopCount;
    if (sLoopCount <= 5 || (sLoopCount <= 600 && (sLoopCount % 60) == 0))
        Log("GameLoop #%llu", (unsigned long long)sLoopCount);

    // Watchdog: if Present hasn't fired, periodically retry swap chain discovery.
    // Layer 3 fallback — covers cases where both DustBoot and initial discovery missed.
    // Retries at frame 120, 300, 600, 1200 (then stops — if it hasn't worked by ~20s, give up).
    if (D3D11Hook::gDeviceCaptured && !D3D11Hook::IsPresentHooked() &&
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
    // Install crash handler FIRST so we capture any fault during init
    SetUnhandledExceptionFilter(DustCrashHandler);

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

    // Build the source-level shader catalog by parsing the game's HLSL files
    // in-place at <kenshiInstallDir>/data/materials/. Reading from the game
    // directory avoids deploying duplicate .hlsl files into the mod folder,
    // which OGRE's resource manager auto-scans and would treat as duplicates.
    //
    // GetGameDir() above derives from the DLL location and assumes a mods/Dust
    // layout — that's wrong for Workshop-deployed mods (workshop/content/<id>/),
    // so resolve the install dir from the running executable instead.
    {
        // Resolve the install dir from the running .exe. RE_Kenshi launches
        // its own wrapper from <kenshiInstallDir>/RE_Kenshi/Kenshi_x64.exe,
        // so the exe's parent isn't always the install root — try both.
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string exeDir(exePath);
        auto sep = exeDir.find_last_of("\\/");
        if (sep != std::string::npos) exeDir.resize(sep + 1);

        std::string materialsDir = exeDir + "data\\materials";
        if (GetFileAttributesA(materialsDir.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            // Fall back to one directory up (RE_Kenshi launcher case).
            std::string parent = exeDir;
            if (!parent.empty() && (parent.back() == '\\' || parent.back() == '/'))
                parent.pop_back();
            auto sep2 = parent.find_last_of("\\/");
            if (sep2 != std::string::npos) parent.resize(sep2 + 1);
            materialsDir = parent + "data\\materials";
        }

        size_t parsed = ShaderSourceCatalog::Init(materialsDir);
        if (parsed == 0)
            Log("ShaderSourceCatalog: no files parsed (expected at %s)", materialsDir.c_str());

        // Phase 1.C cross-validation runs D3DReflect on every D3DCompile.
        // Off by default — opt in via [Dust] ValidateCatalog=1.
        std::string ini = modDir + "Dust.ini";
        bool enable = GetPrivateProfileIntA("Dust", "ValidateCatalog", 0, ini.c_str()) != 0;
        ShaderSourceCatalog::SetValidationEnabled(enable);
        if (enable)
            Log("ShaderSourceCatalog: cross-validation ENABLED (per-compile D3DReflect)");
    }

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
        // Pin the DLL so FreeLibrary never unmaps it while hooks are active.
        // KenshiLib trampoline hooks can't be removed, so we must stay loaded.
        {
            char selfPath[MAX_PATH];
            GetModuleFileNameA(hModule, selfPath, MAX_PATH);
            LoadLibraryA(selfPath);
        }
        break;
    case DLL_PROCESS_DETACH:
        D3D11Hook::SignalShutdown();
        if (lpReserved) break; // process terminating — OS reclaims everything
        DustGUI::Shutdown();
        gEffectLoader.ShutdownAll();

        DeferredMSAA::Shutdown();
        MSAARedirect::Shutdown();
        GeometryReplay::Shutdown();
        GeometryCapture::Shutdown();
        ShaderDatabase::Shutdown();
        ShaderMetadata::Shutdown();
        ShaderSourceCatalog::Shutdown();
        break;
    }
    return TRUE;
}
