#include "EffectLoader.h"
#include "ResourceRegistry.h"
#include "D3D11StateBlock.h"
#include "DustLog.h"
#include <cstring>

EffectLoader gEffectLoader;

// State block shared by all plugins via the host API
static D3D11StateBlock gSharedStateBlock;

// ==================== Host API wrappers ====================

static ID3D11ShaderResourceView* HostGetSRV(const char* name)
{
    return gResourceRegistry.GetSRV(name);
}

static ID3D11RenderTargetView* HostGetRTV(const char* name)
{
    return gResourceRegistry.GetRTV(name);
}

static void HostLog(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log("%s", buf);
}

static void HostSaveState(ID3D11DeviceContext* ctx)
{
    gSharedStateBlock.Capture(ctx);
}

static void HostRestoreState(ID3D11DeviceContext* ctx)
{
    gSharedStateBlock.Restore(ctx);
}

// Resolve the actual D3D11 sampler slot for a given base register.
// Kenshi's deferred shader has different sampler layouts per variant:
//   CSM/RTW (shadows on):  s0-s7 used by game, custom registers start at s8+
//   No shadows:            s0-s5 used by game, compiler remaps higher registers down
// We detect the variant by checking whether slot 6 has a game texture bound.
static UINT ResolveSlot(ID3D11DeviceContext* ctx, uint32_t baseSlot)
{
    // Only remap slots above the shadow-dependent range
    if (baseSlot <= 5)
        return baseSlot;

    ID3D11ShaderResourceView* srv6 = nullptr;
    ctx->PSGetShaderResources(6, 1, &srv6);
    if (srv6)
    {
        srv6->Release();
        return baseSlot; // Shadow mode: registers are as declared
    }

    // No-shadow mode: registers above s5 are shifted down by the gap (s6-s7 absent)
    // e.g. register(s8) becomes actual slot 6
    return baseSlot - 2;
}

static void HostBindSRV(ID3D11DeviceContext* ctx, uint32_t baseSlot,
                         ID3D11ShaderResourceView* srv, ID3D11SamplerState* sampler)
{
    UINT slot = ResolveSlot(ctx, baseSlot);
    ctx->PSSetShaderResources(slot, 1, &srv);
    if (sampler)
        ctx->PSSetSamplers(slot, 1, &sampler);
}

static void HostUnbindSRV(ID3D11DeviceContext* ctx, uint32_t baseSlot)
{
    UINT slot = ResolveSlot(ctx, baseSlot);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ID3D11SamplerState* nullSampler = nullptr;
    ctx->PSSetShaderResources(slot, 1, &nullSRV);
    ctx->PSSetSamplers(slot, 1, &nullSampler);
}

// ==================== EffectLoader ====================

void EffectLoader::BuildHostAPI()
{
    hostAPI_.apiVersion   = DUST_API_VERSION;
    hostAPI_.GetSRV       = HostGetSRV;
    hostAPI_.GetRTV       = HostGetRTV;
#undef Log
    hostAPI_.Log          = HostLog;
#define Log DustLog
    hostAPI_.SaveState    = HostSaveState;
    hostAPI_.RestoreState = HostRestoreState;
    hostAPI_.BindSRV      = HostBindSRV;
    hostAPI_.UnbindSRV    = HostUnbindSRV;
}

int EffectLoader::LoadAll(const char* effectsDir)
{
    BuildHostAPI();

    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*.dll", effectsDir);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        Log("No effect plugins found in %s", effectsDir);
        return 0;
    }

    int loaded = 0;
    do
    {
        char fullPath[MAX_PATH];
        snprintf(fullPath, sizeof(fullPath), "%s\\%s", effectsDir, fd.cFileName);

        HMODULE hMod = LoadLibraryA(fullPath);
        if (!hMod)
        {
            Log("WARNING: Failed to load effect DLL: %s (error %lu)", fd.cFileName, GetLastError());
            continue;
        }

        auto createFn = (PFN_DustEffectCreate)GetProcAddress(hMod, "DustEffectCreate");
        if (!createFn)
        {
            Log("WARNING: %s has no DustEffectCreate export, skipping", fd.cFileName);
            FreeLibrary(hMod);
            continue;
        }

        DustEffectDesc desc = {};
        int result = createFn(&desc);
        if (result != 0)
        {
            Log("WARNING: DustEffectCreate failed for %s (code %d)", fd.cFileName, result);
            FreeLibrary(hMod);
            continue;
        }

        if (desc.apiVersion > DUST_API_VERSION)
        {
            Log("WARNING: %s requires API v%u but host is v%u, skipping",
                desc.name ? desc.name : fd.cFileName, desc.apiVersion, DUST_API_VERSION);
            FreeLibrary(hMod);
            continue;
        }

        LoadedEffect le = {};
        le.hModule = hMod;
        le.desc = desc;
        le.initialized = false;
        effects_.push_back(le);

        Log("Loaded effect plugin: %s (%s)", desc.name ? desc.name : "unnamed", fd.cFileName);
        loaded++;

    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    Log("Loaded %d effect plugin(s)", loaded);
    return loaded;
}

bool EffectLoader::InitAll(ID3D11Device* device, uint32_t w, uint32_t h)
{
    bool allOk = true;
    for (auto& le : effects_)
    {
        if (le.initialized || !le.desc.Init)
            continue;

        int result = le.desc.Init(device, w, h, &hostAPI_);
        if (result != 0)
        {
            Log("WARNING: Effect '%s' failed to initialize (code %d)",
                le.desc.name ? le.desc.name : "unnamed", result);
            allOk = false;
            continue;
        }

        le.initialized = true;
        Log("Initialized effect: %s", le.desc.name ? le.desc.name : "unnamed");
    }
    return allOk;
}

void EffectLoader::DispatchPre(DustInjectionPoint point, const DustFrameContext* ctx)
{
    for (auto& le : effects_)
    {
        if (!le.initialized || !le.desc.preExecute)
            continue;
        if (le.desc.injectionPoint != point)
            continue;
        if (le.desc.IsEnabled && !le.desc.IsEnabled())
            continue;

        le.desc.preExecute(ctx, &hostAPI_);
    }
}

void EffectLoader::DispatchPost(DustInjectionPoint point, const DustFrameContext* ctx)
{
    for (auto& le : effects_)
    {
        if (!le.initialized || !le.desc.postExecute)
            continue;
        if (le.desc.injectionPoint != point)
            continue;
        if (le.desc.IsEnabled && !le.desc.IsEnabled())
            continue;

        le.desc.postExecute(ctx, &hostAPI_);
    }
}

void EffectLoader::OnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    for (auto& le : effects_)
    {
        if (le.initialized && le.desc.OnResolutionChanged)
            le.desc.OnResolutionChanged(device, w, h);
    }
}

void EffectLoader::ShutdownAll()
{
    for (auto& le : effects_)
    {
        if (le.initialized && le.desc.Shutdown)
        {
            le.desc.Shutdown();
            Log("Shut down effect: %s", le.desc.name ? le.desc.name : "unnamed");
        }
        if (le.hModule)
            FreeLibrary(le.hModule);
    }
    effects_.clear();
}
