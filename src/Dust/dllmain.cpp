#include <d3d11.h>
#include <memory>

// KenshiLib headers
#include <kenshi/GameWorld.h>
#include <core/Functions.h>

// Dust framework
#include "DustLog.h"
#include "D3D11Hook.h"
#include "EffectManager.h"

// Built-in effects
#include "SSAOEffect.h"
#include "SSAOConfig.h"
#include "SSAOMenu.h"

static HMODULE gDllModule = nullptr;

// ==================== Game loop hook ====================

void (*GameWorld__mainLoop_GPUSensitiveStuff_orig)(GameWorld* thisptr, float time);

void GameWorld__mainLoop_GPUSensitiveStuff_hook(GameWorld* thisptr, float time)
{
    // Reset per-frame state before the game renders
    D3D11Hook::ResetFrameState();

    // Hot-reload config file if it changed on disk
    gSSAOConfig.CheckHotReload();

    // Poll compositor node enabled state (Graphics menu toggle)
    gSSAOConfig.enabled = SSAOMenu::PollCompositorEnabled();

    // Call original game loop
    GameWorld__mainLoop_GPUSensitiveStuff_orig(thisptr, time);
}

// ==================== Plugin entry point ====================

// RE_Kenshi calls GetProcAddress(plugin, "?startPlugin@@YAXXZ") — C++ mangled name.
// Do NOT use extern "C" here.
__declspec(dllexport) void startPlugin()
{
    Log("Dust framework loading...");

    // Load SSAO config (creates defaults if missing)
    gSSAOConfig.Init(gDllModule);

    // Register built-in effects
    gEffectManager.Register(std::make_unique<SSAOEffect>());

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

    Log("All hooks installed, %zu effect(s) registered, waiting for first D3D11 call...",
        gEffectManager.EffectCount());
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
        gEffectManager.ShutdownAll();
        break;
    }
    return TRUE;
}
