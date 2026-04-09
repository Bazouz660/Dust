#include "SSAOEffect.h"
#include "SSAORenderer.h"
#include "SSAOConfig.h"
#include "SSAOMenu.h"

bool SSAOEffect::Init(ID3D11Device* device, uint32_t w, uint32_t h)
{
    return SSAORenderer::Init(device, w, h);
}

void SSAOEffect::Execute(ID3D11DeviceContext* ctx, const FrameContext& frame)
{
    SSAORenderer::Inject(ctx, frame.depthSRV, frame.normalsSRV, frame.hdrRTV);
}

void SSAOEffect::OnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    SSAORenderer::OnResolutionChanged(device, w, h);
}

void SSAOEffect::Shutdown()
{
    SSAORenderer::Shutdown();
}

bool SSAOEffect::IsEnabled() const
{
    return gSSAOConfig.enabled;
}
