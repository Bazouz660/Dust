#include "DebugViewRegistry.h"

DebugViewRegistry gDebugViewRegistry;

int DebugViewRegistry::Register(const char* label, DustDebugViewRenderFn render, void* user)
{
    if (!label || !render) return 0;
    int handle = nextHandle_++;
    views_.push_back({ handle, std::string(label), render, user });
    return handle;
}

void DebugViewRegistry::Unregister(int handle)
{
    if (handle == 0) return;
    for (auto it = views_.begin(); it != views_.end(); ++it)
    {
        if (it->handle == handle)
        {
            views_.erase(it);
            break;
        }
    }
    if (activeHandle_ == handle)
        activeHandle_ = 0;
}

void DebugViewRegistry::RenderActive(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* targetRTV)
{
    if (activeHandle_ == 0 || !ctx || !targetRTV) return;

    for (const View& v : views_)
    {
        if (v.handle == activeHandle_)
        {
            if (v.render)
                v.render(ctx, targetRTV, v.user);
            return;
        }
    }

    // Active handle no longer maps to a registered view — reset to Off.
    activeHandle_ = 0;
}

void DebugViewRegistry::Clear()
{
    views_.clear();
    activeHandle_ = 0;
    nextHandle_ = 1;
}
