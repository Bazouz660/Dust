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

// Invalidate RE_Kenshi's shader cache so D3DCompile is called for all shaders,
// allowing our runtime hook to patch the deferred shader. Without this, cached
// bytecode would bypass D3DCompile entirely.
static void InvalidateShaderCache(const std::string& gameDir)
{
    std::string cachePath = gameDir + "RE_Kenshi\\shader_cache.sc";
    if (DeleteFileA(cachePath.c_str()))
        Log("Invalidated RE_Kenshi shader cache (will recompile shaders this launch)");
}

// ==================== Game loop hook ====================

void (*GameWorld__mainLoop_GPUSensitiveStuff_orig)(GameWorld* thisptr, float time);

void GameWorld__mainLoop_GPUSensitiveStuff_hook(GameWorld* thisptr, float time)
{
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

    // Invalidate shader cache so our D3DCompile hook can patch shaders at runtime
    std::string gameDir = GetGameDir(gDllModule);
    InvalidateShaderCache(gameDir);

    // Load effect plugins from effects/ directory next to the DLL
    {
        std::string effectsDir = GetModuleDir(gDllModule) + "effects";
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
        break;
    case DLL_PROCESS_DETACH:
        DustGUI::Shutdown();
        gEffectLoader.ShutdownAll();
        break;
    }
    return TRUE;
}
