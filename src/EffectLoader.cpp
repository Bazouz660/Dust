#include "EffectLoader.h"
#include "ResourceRegistry.h"
#include "D3D11StateBlock.h"
#include "DustLog.h"
#include <d3dcompiler.h>
#include <cstring>
#include <cstdlib>
#include <unordered_map>

EffectLoader gEffectLoader;

// State block shared by all plugins via the host API
static D3D11StateBlock gSharedStateBlock;

// ==================== v3: Cached fullscreen VS ====================

static ID3D11VertexShader* gFullscreenVS = nullptr;

static const char* FULLSCREEN_VS_SOURCE = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

// ==================== v3: Scene copy cache ====================

struct SceneCopyEntry {
    ID3D11Texture2D*            tex = nullptr;
    ID3D11ShaderResourceView*   srv = nullptr;
    UINT                        width = 0;
    UINT                        height = 0;
    DXGI_FORMAT                 format = DXGI_FORMAT_UNKNOWN;
};

static std::unordered_map<std::string, SceneCopyEntry> gSceneCopies;

static void ReleaseSceneCopies()
{
    for (auto& kv : gSceneCopies)
    {
        if (kv.second.srv) kv.second.srv->Release();
        if (kv.second.tex) kv.second.tex->Release();
    }
    gSceneCopies.clear();
}

// ==================== Host API wrappers (v2) ====================

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

// ==================== Host API wrappers (v3) ====================

static ID3DBlob* HostCompileShader(const char* hlslSource, const char* entryPoint, const char* target)
{
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(hlslSource, strlen(hlslSource), nullptr, nullptr, nullptr,
                            entryPoint, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &blob, &errors);
    if (FAILED(hr))
    {
        if (errors)
        {
            Log("Shader compile error (%s/%s): %s", entryPoint, target,
                (const char*)errors->GetBufferPointer());
            errors->Release();
        }
        return nullptr;
    }
    if (errors) errors->Release();
    return blob;
}

static ID3DBlob* HostCompileShaderFromFile(const char* filePath, const char* entryPoint, const char* target)
{
    // Read the file into memory
    HANDLE hFile = CreateFileA(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        Log("Shader file not found: %s (error %lu)", filePath, GetLastError());
        return nullptr;
    }

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0)
    {
        CloseHandle(hFile);
        Log("Shader file empty or invalid: %s", filePath);
        return nullptr;
    }

    char* buffer = new char[fileSize + 1];
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, buffer, fileSize, &bytesRead, nullptr);
    CloseHandle(hFile);

    if (!ok || bytesRead != fileSize)
    {
        delete[] buffer;
        Log("Failed to read shader file: %s", filePath);
        return nullptr;
    }
    buffer[fileSize] = '\0';

    // Compile with filename for error messages
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(buffer, fileSize, filePath, nullptr, nullptr,
                            entryPoint, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &blob, &errors);
    delete[] buffer;

    if (FAILED(hr))
    {
        if (errors)
        {
            Log("Shader compile error (%s: %s/%s): %s", filePath, entryPoint, target,
                (const char*)errors->GetBufferPointer());
            errors->Release();
        }
        return nullptr;
    }
    if (errors) errors->Release();
    return blob;
}

static void HostDrawFullscreenTriangle(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps)
{
    if (!gFullscreenVS || !ps) return;
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(gFullscreenVS, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->Draw(3, 0);
}

static ID3D11ShaderResourceView* HostGetSceneCopy(ID3D11DeviceContext* ctx, const char* rtvName)
{
    ID3D11RenderTargetView* rtv = gResourceRegistry.GetRTV(rtvName);
    if (!rtv) return nullptr;

    ID3D11Resource* resource = nullptr;
    rtv->GetResource(&resource);
    if (!resource) return nullptr;

    ID3D11Texture2D* srcTex = nullptr;
    resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&srcTex);
    resource->Release();
    if (!srcTex) return nullptr;

    D3D11_TEXTURE2D_DESC srcDesc;
    srcTex->GetDesc(&srcDesc);

    std::string key(rtvName);
    auto& entry = gSceneCopies[key];

    // Recreate if dimensions or format changed
    if (entry.width != srcDesc.Width || entry.height != srcDesc.Height || entry.format != srcDesc.Format)
    {
        if (entry.srv) { entry.srv->Release(); entry.srv = nullptr; }
        if (entry.tex) { entry.tex->Release(); entry.tex = nullptr; }

        D3D11_TEXTURE2D_DESC copyDesc = {};
        copyDesc.Width = srcDesc.Width;
        copyDesc.Height = srcDesc.Height;
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
        copyDesc.Format = srcDesc.Format;
        copyDesc.SampleDesc.Count = 1;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        if (!device) { srcTex->Release(); return nullptr; }

        HRESULT hr = device->CreateTexture2D(&copyDesc, nullptr, &entry.tex);
        if (FAILED(hr)) { device->Release(); srcTex->Release(); return nullptr; }

        hr = device->CreateShaderResourceView(entry.tex, nullptr, &entry.srv);
        device->Release();
        if (FAILED(hr))
        {
            entry.tex->Release(); entry.tex = nullptr;
            srcTex->Release();
            return nullptr;
        }

        entry.width = srcDesc.Width;
        entry.height = srcDesc.Height;
        entry.format = srcDesc.Format;
    }

    ctx->CopyResource(entry.tex, srcTex);
    srcTex->Release();
    return entry.srv;
}

static ID3D11Buffer* HostCreateConstantBuffer(ID3D11Device* device, uint32_t sizeBytes)
{
    // Round up to 16-byte alignment (D3D11 requirement)
    sizeBytes = (sizeBytes + 15) & ~15;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeBytes;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ID3D11Buffer* buf = nullptr;
    HRESULT hr = device->CreateBuffer(&desc, nullptr, &buf);
    if (FAILED(hr))
    {
        Log("Failed to create constant buffer (%u bytes): 0x%08X", sizeBytes, hr);
        return nullptr;
    }
    return buf;
}

static void HostUpdateConstantBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* cb,
                                      const void* data, uint32_t sizeBytes)
{
    if (!cb || !data) return;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        memcpy(mapped.pData, data, sizeBytes);
        ctx->Unmap(cb, 0);
    }
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

    // v3 additions
    hostAPI_.CompileShader          = HostCompileShader;
    hostAPI_.CompileShaderFromFile  = HostCompileShaderFromFile;
    hostAPI_.DrawFullscreenTriangle = HostDrawFullscreenTriangle;
    hostAPI_.GetSceneCopy           = HostGetSceneCopy;
    hostAPI_.CreateConstantBuffer   = HostCreateConstantBuffer;
    hostAPI_.UpdateConstantBuffer   = HostUpdateConstantBuffer;
}

// ==================== v3: Config I/O ====================

void EffectLoader::EffectConfigLoad(LoadedEffect& le)
{
    const char* section = le.desc.configSection ? le.desc.configSection : le.desc.name;
    if (!section) return;

    for (uint32_t i = 0; i < le.desc.settingCount; i++)
    {
        const DustSettingDesc& s = le.desc.settings[i];
        if (!s.valuePtr) continue;

        const char* key = s.iniKey ? s.iniKey : s.name;
        if (!key) continue;

        switch (s.type)
        {
        case DUST_SETTING_BOOL:
        case DUST_SETTING_HIDDEN_BOOL:
        {
            int def = *(bool*)s.valuePtr ? 1 : 0;
            *(bool*)s.valuePtr = GetPrivateProfileIntA(section, key, def, le.configPath.c_str()) != 0;
            break;
        }
        case DUST_SETTING_FLOAT:
        case DUST_SETTING_HIDDEN_FLOAT:
        {
            char buf[64], defStr[64];
            snprintf(defStr, sizeof(defStr), "%g", *(float*)s.valuePtr);
            GetPrivateProfileStringA(section, key, defStr, buf, sizeof(buf), le.configPath.c_str());
            *(float*)s.valuePtr = (float)atof(buf);
            break;
        }
        case DUST_SETTING_INT:
        case DUST_SETTING_HIDDEN_INT:
        {
            *(int*)s.valuePtr = GetPrivateProfileIntA(section, key, *(int*)s.valuePtr, le.configPath.c_str());
            break;
        }
        }
    }

    Log("Config loaded for '%s' from %s", le.desc.name, le.configPath.c_str());
}

void EffectLoader::EffectConfigSave(LoadedEffect& le)
{
    const char* section = le.desc.configSection ? le.desc.configSection : le.desc.name;
    if (!section) return;

    char buf[64];
    for (uint32_t i = 0; i < le.desc.settingCount; i++)
    {
        const DustSettingDesc& s = le.desc.settings[i];
        if (!s.valuePtr) continue;

        const char* key = s.iniKey ? s.iniKey : s.name;
        if (!key) continue;

        switch (s.type)
        {
        case DUST_SETTING_BOOL:
        case DUST_SETTING_HIDDEN_BOOL:
            WritePrivateProfileStringA(section, key, *(bool*)s.valuePtr ? "1" : "0", le.configPath.c_str());
            break;
        case DUST_SETTING_FLOAT:
        case DUST_SETTING_HIDDEN_FLOAT:
            snprintf(buf, sizeof(buf), "%g", *(float*)s.valuePtr);
            WritePrivateProfileStringA(section, key, buf, le.configPath.c_str());
            break;
        case DUST_SETTING_INT:
        case DUST_SETTING_HIDDEN_INT:
            snprintf(buf, sizeof(buf), "%d", *(int*)s.valuePtr);
            WritePrivateProfileStringA(section, key, buf, le.configPath.c_str());
            break;
        }
    }

    // Update stored mtime
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(le.configPath.c_str(), GetFileExInfoStandard, &fad))
        le.configMtime = fad.ftLastWriteTime;

    Log("Config saved for '%s' to %s", le.desc.name, le.configPath.c_str());
}

void EffectLoader::EffectConfigWriteDefaults(LoadedEffect& le)
{
    // Current in-memory values are the defaults — just save them
    EffectConfigSave(le);
}

void EffectLoader::EffectConfigCheckHotReload(LoadedEffect& le)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(le.configPath.c_str(), GetFileExInfoStandard, &fad))
        return;

    if (CompareFileTime(&fad.ftLastWriteTime, &le.configMtime) != 0)
    {
        le.configMtime = fad.ftLastWriteTime;
        Log("Config changed for '%s', reloading...", le.desc.name);
        EffectConfigLoad(le);
        if (le.desc.OnSettingChanged)
            le.desc.OnSettingChanged();
    }
}

// ==================== v3: GPU Timing ====================

void EffectLoader::CollectTiming(LoadedEffect& le, ID3D11DeviceContext* ctx)
{
    if (!le.timingActive || !le.tsDisjoint) return;

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
    UINT64 tsBegin, tsEnd;
    if (ctx->GetData(le.tsDisjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
        && !disjoint.Disjoint
        && ctx->GetData(le.tsBegin, &tsBegin, sizeof(tsBegin), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
        && ctx->GetData(le.tsEnd, &tsEnd, sizeof(tsEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
    {
        le.gpuTimeMs = (float)((double)(tsEnd - tsBegin) / (double)disjoint.Frequency * 1000.0);
    }
    le.timingActive = false;
}

void EffectLoader::BeginTiming(LoadedEffect& le, ID3D11DeviceContext* ctx)
{
    if (!le.tsDisjoint) return;
    ctx->Begin(le.tsDisjoint);
    ctx->End(le.tsBegin);
}

void EffectLoader::EndTiming(LoadedEffect& le, ID3D11DeviceContext* ctx)
{
    if (!le.tsDisjoint) return;
    ctx->End(le.tsEnd);
    ctx->End(le.tsDisjoint);
    le.timingActive = true;
}

float EffectLoader::GetEffectGpuTime(size_t index) const
{
    if (index >= effects_.size()) return 0.0f;
    const LoadedEffect& le = effects_[index];

    // v3 framework timing
    if (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_TIMING))
        return le.gpuTimeMs;

    // v2 plugin-managed timing
    if (le.desc.gpuTimeMsPtr)
        return *le.desc.gpuTimeMsPtr;

    return 0.0f;
}

// ==================== LoadAll ====================

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

        // Derive effect directory from DLL path
        std::string dir(fullPath);
        size_t pos = dir.find_last_of("\\/");
        if (pos != std::string::npos)
            dir = dir.substr(0, pos);
        le.effectDir = dir;

        // Set _effectDir for v3 plugins to read
        le.desc._effectDir = le.effectDir.c_str();

        // v3: Framework config — derive INI path and load
        if (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_CONFIG))
        {
            const char* section = le.desc.configSection ? le.desc.configSection : le.desc.name;
            le.configPath = le.effectDir + "\\" + std::string(section ? section : "effect") + ".ini";

            DWORD attr = GetFileAttributesA(le.configPath.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES)
            {
                Log("Creating default config for '%s': %s", le.desc.name, le.configPath.c_str());
                EffectConfigWriteDefaults(le);
            }

            EffectConfigLoad(le);

            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExA(le.configPath.c_str(), GetFileExInfoStandard, &fad))
                le.configMtime = fad.ftLastWriteTime;
        }

        effects_.push_back(std::move(le));

        Log("Loaded effect plugin: %s (%s)", desc.name ? desc.name : "unnamed", fd.cFileName);
        loaded++;

    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    Log("Loaded %d effect plugin(s)", loaded);
    return loaded;
}

// ==================== InitAll ====================

bool EffectLoader::InitAll(ID3D11Device* device, uint32_t w, uint32_t h)
{
    // Compile and cache fullscreen VS if not already done
    if (!gFullscreenVS)
    {
        ID3DBlob* vsBlob = HostCompileShader(FULLSCREEN_VS_SOURCE, "main", "vs_5_0");
        if (vsBlob)
        {
            device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                       nullptr, &gFullscreenVS);
            vsBlob->Release();
            if (gFullscreenVS)
                Log("Framework fullscreen VS compiled and cached");
        }
    }

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

        // v3: Create GPU timing queries
        if (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_TIMING))
        {
            D3D11_QUERY_DESC qd = {};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            device->CreateQuery(&qd, &le.tsDisjoint);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            device->CreateQuery(&qd, &le.tsBegin);
            device->CreateQuery(&qd, &le.tsEnd);
        }

        Log("Initialized effect: %s", le.desc.name ? le.desc.name : "unnamed");
    }
    return allOk;
}

// ==================== Dispatch ====================

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

        // v3: Hot-reload config
        if (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_CONFIG))
            EffectConfigCheckHotReload(le);

        // v3: Collect previous frame timing, then start new timing
        bool frameworkTiming = (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_TIMING));
        if (frameworkTiming)
        {
            CollectTiming(le, ctx->context);
            BeginTiming(le, ctx->context);
        }

        le.desc.preExecute(ctx, &hostAPI_);

        if (frameworkTiming)
            EndTiming(le, ctx->context);
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

        // v3: Hot-reload config (if preExecute didn't already)
        if (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_CONFIG)
            && !le.desc.preExecute)
            EffectConfigCheckHotReload(le);

        // v3: Collect previous frame timing, then start new timing
        bool frameworkTiming = (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_TIMING));
        if (frameworkTiming)
        {
            CollectTiming(le, ctx->context);
            BeginTiming(le, ctx->context);
        }

        le.desc.postExecute(ctx, &hostAPI_);

        if (frameworkTiming)
            EndTiming(le, ctx->context);
    }
}

// ==================== Lifecycle ====================

void EffectLoader::OnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    // Release scene copies — they'll be recreated on demand
    ReleaseSceneCopies();

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

        // Release framework timing queries
        if (le.tsDisjoint) { le.tsDisjoint->Release(); le.tsDisjoint = nullptr; }
        if (le.tsBegin)    { le.tsBegin->Release();    le.tsBegin = nullptr; }
        if (le.tsEnd)      { le.tsEnd->Release();      le.tsEnd = nullptr; }

        if (le.hModule)
            FreeLibrary(le.hModule);
    }
    effects_.clear();

    // Release cached resources
    ReleaseSceneCopies();
    if (gFullscreenVS) { gFullscreenVS->Release(); gFullscreenVS = nullptr; }
}

// ==================== v3: Public config helpers ====================

void EffectLoader::SaveEffectConfig(size_t index)
{
    if (index >= effects_.size()) return;
    LoadedEffect& le = effects_[index];
    if (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_CONFIG))
        EffectConfigSave(le);
    else if (le.desc.SaveSettings)
        le.desc.SaveSettings();
}

void EffectLoader::LoadEffectConfig(size_t index)
{
    if (index >= effects_.size()) return;
    LoadedEffect& le = effects_[index];
    if (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_CONFIG))
    {
        EffectConfigLoad(le);
        if (le.desc.OnSettingChanged)
            le.desc.OnSettingChanged();
    }
    else if (le.desc.LoadSettings)
        le.desc.LoadSettings();
}
