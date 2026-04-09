#include <d3d11.h>
#include <memory>
#include <cstdio>
#include <string>

// KenshiLib headers
#include <kenshi/GameWorld.h>
#include <core/Functions.h>

// Dust framework
#include "DustLog.h"
#include "D3D11Hook.h"
#include "EffectLoader.h"

static HMODULE gDllModule = nullptr;
static std::string gGameDir;

// ==================== Shader file management ====================

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
    // Remove trailing slash, go up once
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

// Install modified deferred.hlsl into the game's shader directory.
// Backs up the original as deferred.hlsl.vanilla if not already backed up.
static bool InstallModifiedShader()
{
    std::string shaderDir = gGameDir + "data\\materials\\deferred\\";
    std::string target = shaderDir + "deferred.hlsl";
    std::string backup = shaderDir + "deferred.hlsl.vanilla";
    std::string modDir = GetModuleDir(gDllModule);
    // The modified shader is shipped alongside the DLL in shaders/
    std::string source = modDir + "shaders\\deferred.hlsl";

    // Check if source exists
    FILE* srcFile = fopen(source.c_str(), "rb");
    if (!srcFile)
    {
        Log("WARNING: Modified shader not found at %s", source.c_str());
        Log("  AO will use post-lighting fallback (no day/night consistency)");
        return false;
    }

    // Read source
    fseek(srcFile, 0, SEEK_END);
    long srcSize = ftell(srcFile);
    fseek(srcFile, 0, SEEK_SET);
    std::string srcContent(srcSize, '\0');
    fread(&srcContent[0], 1, srcSize, srcFile);
    fclose(srcFile);

    // Check if target already has our marker
    FILE* tgtFile = fopen(target.c_str(), "rb");
    if (tgtFile)
    {
        fseek(tgtFile, 0, SEEK_END);
        long tgtSize = ftell(tgtFile);
        fseek(tgtFile, 0, SEEK_SET);
        std::string tgtContent(tgtSize, '\0');
        fread(&tgtContent[0], 1, tgtSize, tgtFile);
        fclose(tgtFile);

        if (tgtContent.find("[Dust]") != std::string::npos)
        {
            Log("Modified deferred.hlsl already installed");
            return true;
        }

        // Back up original (only if no backup exists yet)
        FILE* bkFile = fopen(backup.c_str(), "rb");
        if (bkFile)
        {
            fclose(bkFile);
        }
        else
        {
            if (CopyFileA(target.c_str(), backup.c_str(), TRUE))
                Log("Backed up vanilla deferred.hlsl -> deferred.hlsl.vanilla");
            else
                Log("WARNING: Could not back up vanilla deferred.hlsl");
        }
    }

    // Write modified shader
    FILE* outFile = fopen(target.c_str(), "wb");
    if (!outFile)
    {
        Log("ERROR: Could not write to %s", target.c_str());
        return false;
    }
    fwrite(srcContent.data(), 1, srcContent.size(), outFile);
    fclose(outFile);

    Log("Installed modified deferred.hlsl for ambient-only AO");
    return true;
}

static void RestoreVanillaShader()
{
    std::string shaderDir = gGameDir + "data\\materials\\deferred\\";
    std::string target = shaderDir + "deferred.hlsl";
    std::string backup = shaderDir + "deferred.hlsl.vanilla";

    if (CopyFileA(backup.c_str(), target.c_str(), FALSE))
        Log("Restored vanilla deferred.hlsl");
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
#define DUST_STR2(x) #x
#define DUST_STR(x) DUST_STR2(x)
#ifdef DUST_VERSION
    Log("Dust v" DUST_STR(DUST_VERSION) " loading...");
#else
    Log("Dust (dev) loading...");
#endif

    // Determine game directory from DLL path
    gGameDir = GetGameDir(gDllModule);
    Log("Game directory: %s", gGameDir.c_str());

    // Install modified deferred.hlsl BEFORE OGRE compiles shaders
    InstallModifiedShader();

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
        gEffectLoader.ShutdownAll();
        RestoreVanillaShader();
        break;
    }
    return TRUE;
}
