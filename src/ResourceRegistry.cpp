#include "ResourceRegistry.h"
#include <cstring>

ResourceRegistry gResourceRegistry;

int ResourceRegistry::SlotForName(const char* name)
{
    if (!name) return SLOT_INVALID;
    if (strcmp(name, ResourceName::DEPTH_SRV) == 0)   return SLOT_DEPTH;
    if (strcmp(name, ResourceName::NORMALS_SRV) == 0) return SLOT_NORMALS;
    if (strcmp(name, ResourceName::ALBEDO_SRV) == 0)  return SLOT_ALBEDO;
    if (strcmp(name, ResourceName::HDR_RTV) == 0)     return SLOT_HDR;
    if (strcmp(name, ResourceName::LDR_RTV) == 0)     return SLOT_LDR;
    return SLOT_INVALID;
}

void ResourceRegistry::SetSRV(const char* name, ID3D11ShaderResourceView* srv)
{
    int slot = SlotForName(name);
    if (slot != SLOT_INVALID)
        srvs_[slot] = srv;
}

ID3D11ShaderResourceView* ResourceRegistry::GetSRV(const char* name) const
{
    int slot = SlotForName(name);
    return (slot != SLOT_INVALID) ? srvs_[slot] : nullptr;
}

void ResourceRegistry::SetRTV(const char* name, ID3D11RenderTargetView* rtv)
{
    int slot = SlotForName(name);
    if (slot != SLOT_INVALID)
        rtvs_[slot] = rtv;
}

ID3D11RenderTargetView* ResourceRegistry::GetRTV(const char* name) const
{
    int slot = SlotForName(name);
    return (slot != SLOT_INVALID) ? rtvs_[slot] : nullptr;
}

void ResourceRegistry::ResetFrame()
{
    memset(srvs_, 0, sizeof(srvs_));
    memset(rtvs_, 0, sizeof(rtvs_));
}

void ResourceRegistry::PopulateFrameContext(FrameContext& ctx) const
{
    ctx.depthSRV    = GetSRV(ResourceName::DEPTH_SRV);
    ctx.normalsSRV  = GetSRV(ResourceName::NORMALS_SRV);
    ctx.albedoSRV   = GetSRV(ResourceName::ALBEDO_SRV);
    ctx.hdrRTV      = GetRTV(ResourceName::HDR_RTV);
}
