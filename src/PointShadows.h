#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;

// PointShadows — custom per-light shadow maps for Kenshi's point lights.
//
// Stage 1 (current): render the nearest point light's depth from its POV into a
// Dust-owned depth target by replaying captured static geometry (GeometryReplay),
// and verify via a periodic depth readback (caster count + depth min/max/coverage).
// This proves the light-POV replay pipeline before wiring shadows into light_fs.
namespace PointShadows
{
    // Enable geometry capture so the replay has casters. Call once at startup.
    void Init();

    // Called per frame at POST_LIGHTING. camPos3 = camera world position (3 floats).
    void RenderFrame(ID3D11DeviceContext* ctx, ID3D11Device* device, const float* camPos3);

    void Shutdown();

    // Depth SRV of the most recent shadow render (for optional debug viz). May be NULL.
    void* GetDebugDepthSRV();
}
