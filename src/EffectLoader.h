#pragma once

#include "DustAPI.h"
#include <vector>
#include <windows.h>

struct LoadedEffect {
    HMODULE         hModule;
    DustEffectDesc  desc;
    bool            initialized;
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

private:
    std::vector<LoadedEffect> effects_;
    DustHostAPI hostAPI_ = {};

    void BuildHostAPI();
};

extern EffectLoader gEffectLoader;
