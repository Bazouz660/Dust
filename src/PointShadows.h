#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

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

    // OGRE->shader-space translation R (3 floats) for matching cube centers to the
    // light_fs `position` uniform (rebased space). Call before RenderFrame each frame.
    void SetLightSpaceR(const float* r3, bool valid);

    // Read back the current OGRE->shader translation R (diagnostic).
    void GetLightSpaceR(float* out3, bool* valid);

    // Render-world camera position (from light_fs view matrix, set by D3D11Hook). Used to
    // reconcile the OGRE frame with Kenshi's render-world for selection + cube placement.
    void SetRenderCamPos(const float* p3, bool valid);

    // The point lights the deferred pass actually drew this frame, in RENDER-WORLD space
    // (light_fs position[12..14]). These are the ground-truth cube light positions — used
    // directly for cube center + replay POV + the light_fs match, with no R conversion.
    void SetDrawnLights(const float (*positions)[3], const float* radii, int count, const float* renderCamPos);

    // Rebuild the b8 light-table CB from the current active lights + R. Called mid
    // light-volume pass (from the probe) with the current frame's R to remove lag.
    void UpdateLightTableCB(ID3D11DeviceContext* ctx);

    // Called per frame at POST_LIGHTING. camPos3 = camera world position (3 floats).
    // Renders cubes for the N nearest point lights and updates the light-table CB.
    void RenderFrame(ID3D11DeviceContext* ctx, ID3D11Device* device, const float* camPos3);

    // Bind the cube SRVs (t11..t14), sampler (s11) and light-table CB (b8) to the PS
    // stage for the additive light-volume (light_fs) draws. Call AFTER the POST_LIGHTING
    // effect dispatch so nothing clobbers the high slots before the volumes draw; left
    // bound (not unbound), mirroring the contact-shadow b7/t10 keep-bound pattern.
    void BindForLightFs(ID3D11DeviceContext* ctx);

    // Max cube count (N). The OGRE depth renderer fills up to this many cubes.
    int GetMaxCubes();

    // Copy an OGRE-rendered R32_FLOAT depth texture into cube `cube` face `face`
    // (the TextureCubeArray slice cube*6+face). Used by PointShadowsOgre.
    void CopyOgreFaceDepth(ID3D11DeviceContext* ctx, int cube, int face, ID3D11Texture2D* src);

    void Shutdown();

    // Depth SRV of the most recent shadow render (for optional debug viz). May be NULL.
    void* GetDebugDepthSRV();
}
