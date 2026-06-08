#pragma once

struct ID3D11DeviceContext;

// PointShadowsOgre — spike for the OGRE-re-render occluder source. Replaces the
// camera-GBuffer GeometryReplay (which only captures static camera-visible geometry)
// with OGRE rendering the scene from a light POV (instancing/skinning/terrain for free,
// camera-independent). See docs/point_shadow_ogre_rerender_plan.md.
namespace PointShadowsOgre
{
    // Cache the lights to shadow. `ogreLights` = the Ogre::Light* handles (used to read each
    // light's OGRE-geometry-frame position for the cube render); `deferredPos` = the deferred/
    // shader-frame positions (fallback). Call at POST_LIGHTING.
    void SetPendingLights(void* const* ogreLights, const float (*sceneAccessPos)[3], const float* renderCamPos, int count);

    // Render each pending light's 6 cube faces via OGRE and copy them into the cube array.
    // Call from the OGRE swapBuffers hook (cull worker threads idle).
    void RenderPendingAtFrameBoundary();

    // Kenshi's per-frame rebase translation R = its main camera's world position (verified
    // == the voted R). Fills out3 + returns true when available (OGRE up). Reliable every
    // frame regardless of how many lights are drawn, unlike the vote estimator.
    bool GetKenshiRebaseR(float* out3);
}
