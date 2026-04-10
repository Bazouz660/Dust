#pragma once

#include "DustAPI.h"
#include <vector>
#include <string>
#include <windows.h>

struct LoadedEffect {
    HMODULE         hModule;
    DustEffectDesc  desc;
    bool            initialized;

    // v3: Framework-managed GPU timing
    ID3D11Query*    tsDisjoint  = nullptr;
    ID3D11Query*    tsBegin     = nullptr;
    ID3D11Query*    tsEnd       = nullptr;
    bool            timingActive = false;
    float           gpuTimeMs   = 0.0f;

    // v3: Framework-managed config
    std::string     effectDir;      // directory containing the DLL
    std::string     configPath;     // full path to .ini
    FILETIME        configMtime;    // for hot-reload
};

class EffectLoader {
public:
    // Scan effects/ folder and load all plugin DLLs
    int LoadAll(const char* effectsDir);

    // Initialize all loaded plugins (call after device capture)
    bool InitAll(ID3D11Device* device, uint32_t w, uint32_t h);

    // Dispatch callbacks for a given injection point
    void DispatchPre(DustInjectionPoint point, const DustFrameContext* ctx);
    void DispatchPost(DustInjectionPoint point, const DustFrameContext* ctx);

    void OnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h);
    void ShutdownAll();

    size_t Count() const { return effects_.size(); }
    const LoadedEffect& GetEffect(size_t index) const { return effects_[index]; }

    // v3: Framework config helpers (called by DustGUI)
    void SaveEffectConfig(size_t index);
    void LoadEffectConfig(size_t index);

    // v3: GPU timing access (for DustGUI)
    float GetEffectGpuTime(size_t index) const;

private:
    std::vector<LoadedEffect> effects_;
    DustHostAPI hostAPI_ = {};

    void BuildHostAPI();

    // v3: Config I/O helpers
    static void EffectConfigLoad(LoadedEffect& le);
    static void EffectConfigSave(LoadedEffect& le);
    static void EffectConfigWriteDefaults(LoadedEffect& le);
    static void EffectConfigCheckHotReload(LoadedEffect& le);

    // v3: GPU timing helpers
    void CollectTiming(LoadedEffect& le, ID3D11DeviceContext* ctx);
    void BeginTiming(LoadedEffect& le, ID3D11DeviceContext* ctx);
    void EndTiming(LoadedEffect& le, ID3D11DeviceContext* ctx);
};

extern EffectLoader gEffectLoader;
