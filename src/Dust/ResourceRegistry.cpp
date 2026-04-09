#include "ResourceRegistry.h"

ResourceRegistry gResourceRegistry;

void ResourceRegistry::SetSRV(const char* name, ID3D11ShaderResourceView* srv)
{
    srvs_[name] = srv;
}

ID3D11ShaderResourceView* ResourceRegistry::GetSRV(const char* name) const
{
    auto it = srvs_.find(name);
    return (it != srvs_.end()) ? it->second : nullptr;
}

void ResourceRegistry::SetRTV(const char* name, ID3D11RenderTargetView* rtv)
{
    rtvs_[name] = rtv;
}

ID3D11RenderTargetView* ResourceRegistry::GetRTV(const char* name) const
{
    auto it = rtvs_.find(name);
    return (it != rtvs_.end()) ? it->second : nullptr;
}

void ResourceRegistry::ResetFrame()
{
    srvs_.clear();
    rtvs_.clear();
}

void ResourceRegistry::PopulateFrameContext(FrameContext& ctx) const
{
    ctx.depthSRV    = GetSRV(ResourceName::DEPTH_SRV);
    ctx.normalsSRV  = GetSRV(ResourceName::NORMALS_SRV);
    ctx.albedoSRV   = GetSRV(ResourceName::ALBEDO_SRV);
    ctx.hdrRTV      = GetRTV(ResourceName::HDR_RTV);
}
