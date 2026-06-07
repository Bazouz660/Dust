#pragma once
#include <d3d11.h>

// GPU frame profiler: brackets the major render passes with GPU timestamps so we
// can see where each frame's GPU time actually goes — including work that Dust
// injects into the GAME's own draws (tessellation, geometry capture, CSM/RTWSM
// filtering, contact shadows), which the per-effect callback timer can't see.
//
// Segments are derived from the pipeline pass boundaries the hooks already
// detect. Double-buffered queries, read one frame late, so nothing stalls.
namespace FrameProfiler
{
    enum Segment {
        SEG_GBUFFER,    // GBuffer fill (geometry, tessellation, geometry capture)
        SEG_SHADOW,     // shadow-map render (tessellated shadow casters)
        SEG_LIGHTING,   // deferred sun + light volumes (CSM/RTWSM, contact shadows) + forward
        SEG_FOG,        // fog + transparents
        SEG_POST,       // tonemap + Dust post chain + UI
        SEG_COUNT
    };

    void Init(ID3D11Device* device);
    void Shutdown();

    bool IsEnabled();
    void SetEnabled(bool enabled);

    // Frame boundary — call from the Present hook.
    void OnPresent(ID3D11DeviceContext* ctx);

    // Pass-boundary marks — call from the context hooks. No-ops when disabled.
    void MarkGBufferBegin(ID3D11DeviceContext* ctx);
    void MarkGBufferEnd(ID3D11DeviceContext* ctx);
    void MarkShadowBegin(ID3D11DeviceContext* ctx);
    void MarkShadowEnd(ID3D11DeviceContext* ctx);
    void MarkLighting(ID3D11DeviceContext* ctx);
    void MarkFog(ID3D11DeviceContext* ctx);
    void MarkTonemap(ID3D11DeviceContext* ctx);

    // Results in milliseconds (GPU), updated each frame. 0 if unavailable.
    float       GetSegmentMs(Segment s);
    const char* GetSegmentName(Segment s);
    float       GetFrameMs();   // GPU busy time = sum of pass spans (not wall time)
}
