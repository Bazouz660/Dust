#pragma once

#include "IEffect.h"
#include <vector>
#include <memory>

class EffectManager
{
public:
    void Register(std::unique_ptr<IEffect> effect);

    bool InitAll(ID3D11Device* device, uint32_t width, uint32_t height);

    void Dispatch(InjectionPoint point, ID3D11DeviceContext* ctx, const FrameContext& frame);

    void OnResolutionChanged(ID3D11Device* device, uint32_t width, uint32_t height);

    void ShutdownAll();

    size_t EffectCount() const { return effects_.size(); }

private:
    std::vector<std::unique_ptr<IEffect>> effects_;
};

extern EffectManager gEffectManager;
