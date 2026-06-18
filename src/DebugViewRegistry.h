#pragma once

#include "DustAPI.h"
#include <d3d11.h>
#include <vector>
#include <string>

// Centralized registry of debug-view overlays (API v11). Plugins register named
// views that can overwrite the final image; the framework keeps a single active
// selection and draws only that one after tonemapping. This replaces the old
// per-plugin debug enums + DustSSAODebug companion plugin with one unified set.
class DebugViewRegistry
{
public:
    struct View
    {
        int                    handle;
        std::string            label;
        DustDebugViewRenderFn  render;
        void*                  user;
    };

    // Register/unregister a view. Register returns a non-zero handle (0 on
    // failure). Unregistering the active view resets the selection to Off.
    int  Register(const char* label, DustDebugViewRenderFn render, void* user);
    void Unregister(int handle);

    int  GetActiveHandle() const { return activeHandle_; }
    void SetActiveHandle(int handle) { activeHandle_ = handle; }

    // Draw the active view onto targetRTV. No-op if nothing is active, the
    // target is null, or the active handle no longer maps to a view.
    void RenderActive(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* targetRTV);

    const std::vector<View>& Views() const { return views_; }

    // Drop everything (called on full effect shutdown).
    void Clear();

private:
    std::vector<View> views_;
    int activeHandle_ = 0;   // 0 = Off
    int nextHandle_   = 1;
};

extern DebugViewRegistry gDebugViewRegistry;
