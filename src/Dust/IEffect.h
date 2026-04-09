#pragma once

#include "InjectionPoint.h"

class IEffect
{
public:
    virtual ~IEffect() = default;

    virtual const char* GetName() const = 0;

    virtual InjectionPoint GetInjectionPoint() const = 0;

    virtual bool Init(ID3D11Device* device, uint32_t width, uint32_t height) = 0;

    virtual void Execute(ID3D11DeviceContext* ctx, const FrameContext& frame) = 0;

    virtual void OnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h) = 0;

    virtual void Shutdown() = 0;

    virtual bool IsEnabled() const = 0;
};
