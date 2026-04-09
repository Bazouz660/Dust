#include "EffectManager.h"
#include "DustLog.h"

EffectManager gEffectManager;

void EffectManager::Register(std::unique_ptr<IEffect> effect)
{
    Log("Registered effect: %s (injection point %d)", effect->GetName(), (int)effect->GetInjectionPoint());
    effects_.push_back(std::move(effect));
}

bool EffectManager::InitAll(ID3D11Device* device, uint32_t width, uint32_t height)
{
    bool allOk = true;
    for (auto& fx : effects_)
    {
        if (!fx->Init(device, width, height))
        {
            Log("ERROR: Effect '%s' failed to initialize", fx->GetName());
            allOk = false;
        }
        else
        {
            Log("Effect '%s' initialized (%ux%u)", fx->GetName(), width, height);
        }
    }
    return allOk;
}

void EffectManager::Dispatch(InjectionPoint point, ID3D11DeviceContext* ctx, const FrameContext& frame)
{
    for (auto& fx : effects_)
    {
        if (fx->GetInjectionPoint() == point && fx->IsEnabled())
            fx->Execute(ctx, frame);
    }
}

void EffectManager::OnResolutionChanged(ID3D11Device* device, uint32_t width, uint32_t height)
{
    for (auto& fx : effects_)
        fx->OnResolutionChanged(device, width, height);
}

void EffectManager::ShutdownAll()
{
    for (auto& fx : effects_)
        fx->Shutdown();
    effects_.clear();
}
