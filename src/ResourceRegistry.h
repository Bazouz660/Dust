#pragma once

#include "InjectionPoint.h"
#include <d3d11.h>

namespace ResourceName
{
    constexpr const char* DEPTH_SRV     = "depth";
    constexpr const char* NORMALS_SRV   = "normals";
    constexpr const char* ALBEDO_SRV    = "albedo";
    constexpr const char* HDR_RTV       = "hdr_rt";
    constexpr const char* LDR_RTV       = "ldr_rt";
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
    // The resource names form a small closed set — fixed slots instead of
    // string-keyed maps keep the per-frame ResetFrame and the per-effect
    // lookups (dozens per frame) allocation- and hash-free.
    enum SlotId { SLOT_DEPTH, SLOT_NORMALS, SLOT_ALBEDO, SLOT_HDR, SLOT_LDR, SLOT_COUNT, SLOT_INVALID = SLOT_COUNT };
    static int SlotForName(const char* name);

    ID3D11ShaderResourceView* srvs_[SLOT_COUNT] = {};
    ID3D11RenderTargetView*   rtvs_[SLOT_COUNT] = {};
};

extern ResourceRegistry gResourceRegistry;
