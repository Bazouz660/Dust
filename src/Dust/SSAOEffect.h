#pragma once

#include "IEffect.h"

class SSAOEffect : public IEffect
{
public:
    const char* GetName() const override { return "SSAO"; }
    InjectionPoint GetInjectionPoint() const override { return InjectionPoint::POST_LIGHTING; }
    bool Init(ID3D11Device* device, uint32_t width, uint32_t height) override;
    void Execute(ID3D11DeviceContext* ctx, const FrameContext& frame) override;
    void OnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h) override;
    void Shutdown() override;
    bool IsEnabled() const override;
};
