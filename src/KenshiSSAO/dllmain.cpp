#include <windows.h>
#include <d3d11.h>
#include <cstdio>

// KenshiLib headers
#include <kenshi/GameWorld.h>
#include <core/Functions.h>
#include <Debug.h>

// Our headers
#include "D3D11Hook.h"
#include "SSAORenderer.h"
#include "SSAOConfig.h"
#include "SSAOMenu.h"

static HMODULE gDllModule = nullptr;

// ==================== Debug logging ====================

static void Log(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA("[KenshiSSAO] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    // Also log to KenshiLib (const char* overload — safe across ABI)
    char fullBuf[600];
    snprintf(fullBuf, sizeof(fullBuf), "[KenshiSSAO] %s", buf);
    ::DebugLog(fullBuf);
}

// ==================== Game loop hook ====================

void (*GameWorld__mainLoop_GPUSensitiveStuff_orig)(GameWorld* thisptr, float time);

void GameWorld__mainLoop_GPUSensitiveStuff_hook(GameWorld* thisptr, float time)
{
    // Reset per-frame state before the game renders
    if (D3D11Hook::gAoInjectedThisFrame)
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
    Log("KenshiSSAO plugin loading...");

    // Load config file (creates defaults if missing)
    gSSAOConfig.Init(gDllModule);

    // Hook game loop for per-frame state reset
    KenshiLib::HookStatus status = KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
        &GameWorld__mainLoop_GPUSensitiveStuff_hook,
        &GameWorld__mainLoop_GPUSensitiveStuff_orig);

    if (status != KenshiLib::SUCCESS)
    {
        Log("ERROR: Failed to hook GameWorld::_NV_mainLoop_GPUSensitiveStuff");
        ErrorLog("KenshiSSAO: could not install game loop hook!");
        return;
    }

    Log("Game loop hook installed");

    // Install D3D11 hooks — creates a temporary device to discover function
    // addresses, then hooks them via KenshiLib::AddHook. The real device/context
    // are captured from the first hooked call. No OGRE dependency.
    if (!D3D11Hook::Install())
    {
        Log("ERROR: D3D11Hook::Install failed");
        ErrorLog("KenshiSSAO: could not install D3D11 hooks!");
        return;
    }

    Log("All hooks installed, waiting for first D3D11 call to capture device...");
    Log("Compositor toggle will be polled from Graphics menu ('KenshiSSAO' node)");
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
        SSAORenderer::Shutdown();
        break;
    }
    return TRUE;
}
