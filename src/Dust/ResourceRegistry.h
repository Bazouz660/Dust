#pragma once

#include "InjectionPoint.h"
#include <d3d11.h>
#include <unordered_map>
#include <string>

namespace ResourceName
{
    constexpr const char* DEPTH_SRV     = "depth";
    constexpr const char* NORMALS_SRV   = "normals";
    constexpr const char* ALBEDO_SRV    = "albedo";
    constexpr const char* HDR_RTV       = "hdr_rt";
    constexpr const char* LDR_RTV       = "ldr_rt";
    constexpr const char* LUMINANCE_SRV = "luminance";
}

class ResourceRegistry
{
public:
    void SetSRV(const char* name, ID3D11ShaderResourceView* srv);
    ID3D11ShaderResourceView* GetSRV(const char* name) const;

    void SetRTV(const char* name, ID3D11RenderTargetView* rtv);
    ID3D11RenderTargetView* GetRTV(const char* name) const;

    void ResetFrame();

    void PopulateFrameContext(FrameContext& ctx) const;

private:
    std::unordered_map<std::string, ID3D11ShaderResourceView*> srvs_;
    std::unordered_map<std::string, ID3D11RenderTargetView*>   rtvs_;
};

extern ResourceRegistry gResourceRegistry;
