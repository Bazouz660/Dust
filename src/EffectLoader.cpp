#include "EffectLoader.h"
#include "ResourceRegistry.h"
#include "D3D11StateBlock.h"
#include "DustLog.h"
#include <d3dcompiler.h>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
#include <algorithm>

EffectLoader gEffectLoader;

// State block shared by all plugins via the host API
static D3D11StateBlock gSharedStateBlock;

// ==================== v3: Cached fullscreen VS ====================

static ID3D11VertexShader* gFullscreenVS = nullptr;

// Framework shader directory (derived from effects dir parent)
static std::string gFrameworkShaderDir;

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

// ==================== Pre-fog HDR snapshot ====================
// Captured at POST_LIGHTING (before fog), available to effects at any later point.

static SceneCopyEntry gPreFogHDR;

static void ReleasePreFogHDR()
{
    if (gPreFogHDR.srv) { gPreFogHDR.srv->Release(); gPreFogHDR.srv = nullptr; }
    if (gPreFogHDR.tex) { gPreFogHDR.tex->Release(); gPreFogHDR.tex = nullptr; }
    gPreFogHDR.width = gPreFogHDR.height = 0;
    gPreFogHDR.format = DXGI_FORMAT_UNKNOWN;
}

void EffectLoader::CapturePreFogHDR(ID3D11DeviceContext* ctx)
{
    ID3D11RenderTargetView* rtv = gResourceRegistry.GetRTV(DUST_RESOURCE_HDR_RT);
    if (!rtv) return;

    ID3D11Resource* resource = nullptr;
    rtv->GetResource(&resource);
    if (!resource) return;

    ID3D11Texture2D* srcTex = nullptr;
    resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&srcTex);
    resource->Release();
    if (!srcTex) return;

    D3D11_TEXTURE2D_DESC srcDesc;
    srcTex->GetDesc(&srcDesc);

    // Recreate if dimensions or format changed
    if (gPreFogHDR.width != srcDesc.Width || gPreFogHDR.height != srcDesc.Height || gPreFogHDR.format != srcDesc.Format)
    {
        ReleasePreFogHDR();

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
        if (!device) { srcTex->Release(); return; }

        HRESULT hr = device->CreateTexture2D(&copyDesc, nullptr, &gPreFogHDR.tex);
        if (FAILED(hr)) { device->Release(); srcTex->Release(); return; }

        hr = device->CreateShaderResourceView(gPreFogHDR.tex, nullptr, &gPreFogHDR.srv);
        device->Release();
        if (FAILED(hr))
        {
            gPreFogHDR.tex->Release(); gPreFogHDR.tex = nullptr;
            srcTex->Release();
            return;
        }

        gPreFogHDR.width = srcDesc.Width;
        gPreFogHDR.height = srcDesc.Height;
        gPreFogHDR.format = srcDesc.Format;
    }

    ctx->CopyResource(gPreFogHDR.tex, srcTex);
    srcTex->Release();
}

static ID3D11ShaderResourceView* HostGetPreFogHDR()
{
    return gPreFogHDR.srv;
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
    hostAPI_.GetPreFogHDR           = HostGetPreFogHDR;
}

// ==================== v3: Config I/O ====================

void EffectLoader::EffectConfigLoad(LoadedEffect& le)
{
    const char* section = le.desc.configSection ? le.desc.configSection : le.desc.name;
    if (!section) return;

    bool anyMissing = false;

    for (uint32_t i = 0; i < le.desc.settingCount; i++)
    {
        const DustSettingDesc& s = le.desc.settings[i];
        if (!s.valuePtr) continue;

        const char* key = s.iniKey ? s.iniKey : s.name;
        if (!key) continue;

        // Check if key exists in INI (sentinel-based: if we get the sentinel back, key is missing)
        char probe[64];
        const char* sentinel = "\x01\x02MISSING";
        GetPrivateProfileStringA(section, key, sentinel, probe, sizeof(probe), le.configPath.c_str());
        bool missing = (strcmp(probe, sentinel) == 0);
        if (missing) anyMissing = true;

        switch (s.type)
        {
        case DUST_SETTING_BOOL:
        case DUST_SETTING_HIDDEN_BOOL:
        {
            if (!missing)
                *(bool*)s.valuePtr = GetPrivateProfileIntA(section, key, 0, le.configPath.c_str()) != 0;
            // else: keep default
            break;
        }
        case DUST_SETTING_FLOAT:
        case DUST_SETTING_HIDDEN_FLOAT:
        {
            if (!missing)
                *(float*)s.valuePtr = (float)atof(probe);
            // else: keep default
            break;
        }
        case DUST_SETTING_INT:
        case DUST_SETTING_HIDDEN_INT:
        {
            if (!missing)
                *(int*)s.valuePtr = GetPrivateProfileIntA(section, key, 0, le.configPath.c_str());
            // else: keep default
            break;
        }
        }
    }

    // Write missing keys to INI so new settings are persisted immediately
    if (anyMissing)
    {
        Log("Config for '%s' has new settings, updating INI", le.desc.name);
        EffectConfigSave(le);
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

// ==================== Global Preset System ====================
// Each preset is a folder under <effectsDir>/presets/ containing per-effect INI files.
// e.g. effects/presets/dust_high/DOF.ini, effects/presets/dust_high/SSAO.ini, etc.

void EffectLoader::EffectConfigLoadFrom(LoadedEffect& le, const std::string& presetDir)
{
    const char* section = le.desc.configSection ? le.desc.configSection : le.desc.name;
    if (!section) return;

    std::string iniPath = presetDir + "\\" + std::string(section) + ".ini";

    // If this effect has no INI in the preset, disable it
    DWORD attr = GetFileAttributesA(iniPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        for (uint32_t i = 0; i < le.desc.settingCount; i++)
        {
            const DustSettingDesc& s = le.desc.settings[i];
            const char* key = s.iniKey ? s.iniKey : s.name;
            if (s.type == DUST_SETTING_BOOL && s.valuePtr &&
                key && _stricmp(key, "Enabled") == 0)
            {
                *(bool*)s.valuePtr = false;
                break;
            }
        }
        return;
    }

    const char* sentinel = "\x01\x02MISSING";

    for (uint32_t i = 0; i < le.desc.settingCount; i++)
    {
        const DustSettingDesc& s = le.desc.settings[i];
        if (!s.valuePtr) continue;

        const char* key = s.iniKey ? s.iniKey : s.name;
        if (!key) continue;

        char probe[64];
        GetPrivateProfileStringA(section, key, sentinel, probe, sizeof(probe), iniPath.c_str());
        if (strcmp(probe, sentinel) == 0) continue;

        switch (s.type)
        {
        case DUST_SETTING_BOOL:
        case DUST_SETTING_HIDDEN_BOOL:
            *(bool*)s.valuePtr = (atoi(probe) != 0);
            break;
        case DUST_SETTING_FLOAT:
        case DUST_SETTING_HIDDEN_FLOAT:
            *(float*)s.valuePtr = (float)atof(probe);
            break;
        case DUST_SETTING_INT:
        case DUST_SETTING_HIDDEN_INT:
            *(int*)s.valuePtr = atoi(probe);
            break;
        }
    }
}

void EffectLoader::EffectConfigSaveTo(LoadedEffect& le, const std::string& presetDir)
{
    const char* section = le.desc.configSection ? le.desc.configSection : le.desc.name;
    if (!section) return;

    std::string iniPath = presetDir + "\\" + std::string(section) + ".ini";
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
            WritePrivateProfileStringA(section, key, *(bool*)s.valuePtr ? "1" : "0", iniPath.c_str());
            break;
        case DUST_SETTING_FLOAT:
        case DUST_SETTING_HIDDEN_FLOAT:
            snprintf(buf, sizeof(buf), "%g", *(float*)s.valuePtr);
            WritePrivateProfileStringA(section, key, buf, iniPath.c_str());
            break;
        case DUST_SETTING_INT:
        case DUST_SETTING_HIDDEN_INT:
            snprintf(buf, sizeof(buf), "%d", *(int*)s.valuePtr);
            WritePrivateProfileStringA(section, key, buf, iniPath.c_str());
            break;
        }
    }
}

void EffectLoader::ScanPresets()
{
    presets_.clear();
    currentPreset_ = -1;

    if (presetsDir_.empty()) return;

    // Ensure presets root directory exists
    CreateDirectoryA(presetsDir_.c_str(), nullptr);

    // Enumerate subdirectories
    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*", presetsDir_.c_str());

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue; // skip . and ..

        PresetInfo info;
        info.name = fd.cFileName;
        info.path = presetsDir_ + "\\" + fd.cFileName;
        presets_.push_back(std::move(info));
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);

    std::sort(presets_.begin(), presets_.end(),
        [](const PresetInfo& a, const PresetInfo& b) { return a.name < b.name; });

    Log("Found %zu global presets in %s", presets_.size(), presetsDir_.c_str());
}

void EffectLoader::ValidatePreset(int presetIdx)
{
    if (presetIdx < 0 || presetIdx >= (int)presets_.size()) return;

    PresetInfo& preset = presets_[presetIdx];
    preset.warnings.clear();

    for (const auto& le : effects_)
    {
        if (!le.initialized) continue;
        if (le.desc.apiVersion < 3 || !(le.desc.flags & DUST_FLAG_FRAMEWORK_CONFIG)) continue;

        const char* section = le.desc.configSection ? le.desc.configSection : le.desc.name;
        if (!section) continue;

        std::string iniPath = preset.path + "\\" + std::string(section) + ".ini";
        DWORD attr = GetFileAttributesA(iniPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue; // missing INI = disabled, not outdated

        const char* sentinel = "\x01\x02MISSING";

        // Check for missing keys (effect expects them, INI doesn't have them)
        std::string missing;
        std::vector<std::string> expectedKeys;
        for (uint32_t i = 0; i < le.desc.settingCount; i++)
        {
            const DustSettingDesc& s = le.desc.settings[i];
            const char* key = s.iniKey ? s.iniKey : s.name;
            if (!key) continue;
            expectedKeys.push_back(key);

            char probe[64];
            GetPrivateProfileStringA(section, key, sentinel, probe, sizeof(probe), iniPath.c_str());
            if (strcmp(probe, sentinel) == 0)
            {
                if (!missing.empty()) missing += ", ";
                missing += key;
            }
        }

        // Check for unknown keys (INI has them, effect doesn't expect them)
        // Read all keys from the INI section
        std::string unknown;
        char keysBuf[4096] = {};
        DWORD keysLen = GetPrivateProfileStringA(section, nullptr, "", keysBuf, sizeof(keysBuf), iniPath.c_str());
        if (keysLen > 0)
        {
            const char* p = keysBuf;
            while (*p)
            {
                std::string iniKey(p);
                bool found = false;
                for (const auto& ek : expectedKeys)
                {
                    if (_stricmp(ek.c_str(), iniKey.c_str()) == 0)
                    { found = true; break; }
                }
                if (!found)
                {
                    if (!unknown.empty()) unknown += ", ";
                    unknown += iniKey;
                }
                p += iniKey.size() + 1;
            }
        }

        if (!missing.empty() || !unknown.empty())
        {
            if (!preset.warnings.empty()) preset.warnings += "\n";
            preset.warnings += std::string(le.desc.name) + ": ";
            if (!missing.empty()) preset.warnings += "missing [" + missing + "]";
            if (!missing.empty() && !unknown.empty()) preset.warnings += ", ";
            if (!unknown.empty()) preset.warnings += "unknown [" + unknown + "]";
        }
    }

    if (!preset.warnings.empty())
        Log("Preset '%s' is outdated:\n%s", preset.name.c_str(), preset.warnings.c_str());
}

void EffectLoader::LoadPreset(int presetIdx)
{
    if (presetIdx < 0 || presetIdx >= (int)presets_.size()) return;

    const std::string& presetDir = presets_[presetIdx].path;

    for (auto& le : effects_)
    {
        if (!le.initialized) continue;
        if (le.desc.apiVersion < 3 || !(le.desc.flags & DUST_FLAG_FRAMEWORK_CONFIG)) continue;

        EffectConfigLoadFrom(le, presetDir);

        if (le.desc.OnSettingChanged)
            le.desc.OnSettingChanged();
    }

    currentPreset_ = presetIdx;
    Log("Loaded global preset '%s'", presets_[presetIdx].name.c_str());
}

void EffectLoader::SavePreset(int presetIdx)
{
    if (presetIdx < 0 || presetIdx >= (int)presets_.size()) return;

    const std::string& presetDir = presets_[presetIdx].path;

    for (auto& le : effects_)
    {
        if (!le.initialized) continue;
        if (le.desc.apiVersion < 3 || !(le.desc.flags & DUST_FLAG_FRAMEWORK_CONFIG)) continue;

        EffectConfigSaveTo(le, presetDir);
    }

    presets_[presetIdx].warnings.clear(); // INI is now current
    Log("Saved global preset '%s'", presets_[presetIdx].name.c_str());
}

int EffectLoader::SavePresetAs(const char* name)
{
    if (!name || !name[0] || presetsDir_.empty()) return -1;

    // Sanitize folder name
    std::string safeName(name);
    for (char& c : safeName)
    {
        if (c == '\\' || c == '/' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }

    std::string presetDir = presetsDir_ + "\\" + safeName;

    // Create the preset folder
    CreateDirectoryA(presetDir.c_str(), nullptr);

    // Check if already in list
    int existingIdx = -1;
    for (int i = 0; i < (int)presets_.size(); i++)
    {
        if (_stricmp(presets_[i].name.c_str(), safeName.c_str()) == 0)
        {
            existingIdx = i;
            break;
        }
    }

    if (existingIdx >= 0)
    {
        SavePreset(existingIdx);
        currentPreset_ = existingIdx;
        return existingIdx;
    }

    // Add and sort
    PresetInfo info;
    info.name = safeName;
    info.path = presetDir;
    presets_.push_back(info);

    std::sort(presets_.begin(), presets_.end(),
        [](const PresetInfo& a, const PresetInfo& b) { return a.name < b.name; });

    int newIdx = -1;
    for (int i = 0; i < (int)presets_.size(); i++)
    {
        if (presets_[i].name == safeName) { newIdx = i; break; }
    }

    if (newIdx >= 0)
    {
        SavePreset(newIdx);
        currentPreset_ = newIdx;
    }

    return newIdx;
}

void EffectLoader::DeletePreset(int presetIdx)
{
    if (presetIdx < 0 || presetIdx >= (int)presets_.size()) return;

    const std::string& presetDir = presets_[presetIdx].path;

    // Delete all INI files in the preset folder
    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*.ini", presetDir.c_str());

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do {
            std::string filePath = presetDir + "\\" + fd.cFileName;
            DeleteFileA(filePath.c_str());
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    // Remove the directory
    RemoveDirectoryA(presetDir.c_str());

    Log("Deleted global preset '%s'", presets_[presetIdx].name.c_str());

    presets_.erase(presets_.begin() + presetIdx);

    if (currentPreset_ == presetIdx)
        currentPreset_ = -1;
    else if (currentPreset_ > presetIdx)
        currentPreset_--;
}

// ==================== v3: GPU Timing ====================

void EffectLoader::CollectTiming(LoadedEffect& le, ID3D11DeviceContext* ctx, int phase)
{
    if (le.timingWarmup[phase] < 2 || !le.tsDisjoint[phase][0]) return;

    int readSlot = 1 - le.timingSlot[phase];

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
    UINT64 tsBegin, tsEnd;
    if (ctx->GetData(le.tsDisjoint[phase][readSlot], &disjoint, sizeof(disjoint), 0) == S_OK
        && !disjoint.Disjoint
        && ctx->GetData(le.tsBegin[phase][readSlot], &tsBegin, sizeof(tsBegin), 0) == S_OK
        && ctx->GetData(le.tsEnd[phase][readSlot], &tsEnd, sizeof(tsEnd), 0) == S_OK)
    {
        float ms = (float)((double)(tsEnd - tsBegin) / (double)disjoint.Frequency * 1000.0);
        if (phase == 0) le.gpuTimePre = ms;
        else            le.gpuTimePost = ms;
        le.gpuTimeMs = le.gpuTimePre + le.gpuTimePost;
    }
}

void EffectLoader::BeginTiming(LoadedEffect& le, ID3D11DeviceContext* ctx, int phase)
{
    if (!le.tsDisjoint[phase][0]) return;
    int slot = le.timingSlot[phase];
    ctx->Begin(le.tsDisjoint[phase][slot]);
    ctx->End(le.tsBegin[phase][slot]);
}

void EffectLoader::EndTiming(LoadedEffect& le, ID3D11DeviceContext* ctx, int phase)
{
    if (!le.tsDisjoint[phase][0]) return;
    int slot = le.timingSlot[phase];
    ctx->End(le.tsEnd[phase][slot]);
    ctx->End(le.tsDisjoint[phase][slot]);
    le.timingSlot[phase] = 1 - le.timingSlot[phase];
    if (le.timingWarmup[phase] < 2) le.timingWarmup[phase]++;
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

    // Derive framework shader directory: effectsDir is <modDir>/effects,
    // shared shaders (fullscreen_vs.hlsl etc.) live in <effectsDir>/shaders/
    gFrameworkShaderDir = std::string(effectsDir) + "\\shaders\\";

    // Set up global presets directory: <modRoot>/presets/
    {
        std::string eDir(effectsDir);
        size_t pos = eDir.find_last_of("\\/");
        if (pos != std::string::npos)
            presetsDir_ = eDir.substr(0, pos) + "\\presets";
        else
            presetsDir_ = "presets";
    }

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

    // Sort by (injectionPoint, priority) so effects dispatch in the right order
    std::stable_sort(effects_.begin(), effects_.end(),
        [](const LoadedEffect& a, const LoadedEffect& b) {
            if (a.desc.injectionPoint != b.desc.injectionPoint)
                return a.desc.injectionPoint < b.desc.injectionPoint;
            return a.desc.priority < b.desc.priority;
        });

    Log("Loaded %d effect plugin(s)", loaded);
    return loaded;
}

// ==================== InitAll ====================

bool EffectLoader::InitAll(ID3D11Device* device, uint32_t w, uint32_t h)
{
    // Compile and cache fullscreen VS if not already done
    if (!gFullscreenVS)
    {
        std::string vsPath = gFrameworkShaderDir + "fullscreen_vs.hlsl";
        ID3DBlob* vsBlob = HostCompileShaderFromFile(vsPath.c_str(), "main", "vs_5_0");
        if (vsBlob)
        {
            device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                       nullptr, &gFullscreenVS);
            vsBlob->Release();
            if (gFullscreenVS)
                Log("Framework fullscreen VS compiled from %s", vsPath.c_str());
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

        // v3: Create GPU timing queries (2 phases × 2 double-buffer slots)
        if (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_TIMING))
        {
            D3D11_QUERY_DESC qd = {};
            for (int phase = 0; phase < 2; phase++)
            for (int slot = 0; slot < 2; slot++)
            {
                qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
                device->CreateQuery(&qd, &le.tsDisjoint[phase][slot]);
                qd.Query = D3D11_QUERY_TIMESTAMP;
                device->CreateQuery(&qd, &le.tsBegin[phase][slot]);
                device->CreateQuery(&qd, &le.tsEnd[phase][slot]);
            }
        }

        Log("Initialized effect: %s", le.desc.name ? le.desc.name : "unnamed");
    }

    // Scan global presets after all effects are ready
    ScanPresets();
    for (int i = 0; i < (int)presets_.size(); i++)
        ValidatePreset(i);

    return allOk;
}

// ==================== ReinitAll ====================

bool EffectLoader::ReinitAll(ID3D11Device* device, uint32_t w, uint32_t h)
{
    Log("Reinitializing all effects on device=%p at %ux%u", device, w, h);

    for (auto& le : effects_)
    {
        if (le.initialized && le.desc.Shutdown)
            le.desc.Shutdown();

        // Release framework timing queries
        for (int phase = 0; phase < 2; phase++)
        for (int slot = 0; slot < 2; slot++)
        {
            if (le.tsDisjoint[phase][slot]) { le.tsDisjoint[phase][slot]->Release(); le.tsDisjoint[phase][slot] = nullptr; }
            if (le.tsBegin[phase][slot])    { le.tsBegin[phase][slot]->Release();    le.tsBegin[phase][slot] = nullptr; }
            if (le.tsEnd[phase][slot])      { le.tsEnd[phase][slot]->Release();      le.tsEnd[phase][slot] = nullptr; }
        }
        le.timingSlot[0] = le.timingSlot[1] = 0;
        le.timingWarmup[0] = le.timingWarmup[1] = 0;
        le.gpuTimePre = le.gpuTimePost = le.gpuTimeMs = 0.0f;

        le.initialized = false;
    }

    // Release scene copies (they belong to the old device)
    ReleaseSceneCopies();
    ReleasePreFogHDR();

    // Recompile fullscreen VS on the new device
    if (gFullscreenVS) { gFullscreenVS->Release(); gFullscreenVS = nullptr; }
    {
        std::string vsPath = gFrameworkShaderDir + "fullscreen_vs.hlsl";
        ID3DBlob* vsBlob = HostCompileShaderFromFile(vsPath.c_str(), "main", "vs_5_0");
        if (vsBlob)
        {
            device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                        nullptr, &gFullscreenVS);
            vsBlob->Release();
        }
    }

    return InitAll(device, w, h);
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

        // v3: Collect previous frame timing, then start new timing (phase 0 = pre)
        bool frameworkTiming = (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_TIMING));
        if (frameworkTiming)
        {
            CollectTiming(le, ctx->context, 0);
            BeginTiming(le, ctx->context, 0);
        }

        le.desc.preExecute(ctx, &hostAPI_);

        if (frameworkTiming)
            EndTiming(le, ctx->context, 0);
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

        // v3: Time postExecute (phase 1 = post)
        bool frameworkTiming = (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_TIMING));
        if (frameworkTiming)
        {
            CollectTiming(le, ctx->context, 1);
            BeginTiming(le, ctx->context, 1);
        }

        le.desc.postExecute(ctx, &hostAPI_);

        if (frameworkTiming)
            EndTiming(le, ctx->context, 1);
    }
}

// ==================== Lifecycle ====================

void EffectLoader::OnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    // Safety net: verify device is still alive before recreating GPU resources.
    // The primary check is in D3D11Hook, but guard here too in case of other callers.
    HRESULT removeReason = device->GetDeviceRemovedReason();
    if (removeReason != S_OK)
    {
        Log("OnResolutionChanged: device removed (0x%08X), skipping resource recreation", removeReason);
        return;
    }

    // Release scene copies — they'll be recreated on demand
    ReleaseSceneCopies();
    ReleasePreFogHDR();

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
        for (int phase = 0; phase < 2; phase++)
        for (int slot = 0; slot < 2; slot++)
        {
            if (le.tsDisjoint[phase][slot]) { le.tsDisjoint[phase][slot]->Release(); le.tsDisjoint[phase][slot] = nullptr; }
            if (le.tsBegin[phase][slot])    { le.tsBegin[phase][slot]->Release();    le.tsBegin[phase][slot] = nullptr; }
            if (le.tsEnd[phase][slot])      { le.tsEnd[phase][slot]->Release();      le.tsEnd[phase][slot] = nullptr; }
        }

        if (le.hModule)
            FreeLibrary(le.hModule);
    }
    effects_.clear();

    // Release cached resources
    ReleaseSceneCopies();
    ReleasePreFogHDR();
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
