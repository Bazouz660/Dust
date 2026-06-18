#pragma once

#include <cstdint>

// SceneAccess — the validated "semantic scene" master key.
//
// Kenshi's lights are NOT in OGRE's culled global light list; instead every
// light is an Ogre::Light created via SceneManager::createLight. We hook the
// exported createLight/destroyLight to maintain the authoritative live light
// set (cull-independent), and read each light through layout-safe getters.
//
// Validated 2026-06-08: 143 point lights in a night city. Position/type/color
// (= specular)/intensity (= powerScale) read correctly. Range is driven by
// Kenshi's own light system, so it is estimated here (see GetLights).
namespace SceneAccess
{
    enum LightType { LIGHT_POINT = 0, LIGHT_DIRECTIONAL = 1, LIGHT_SPOT = 2 };

    struct Light
    {
        void*   ogreLight;     // Ogre::Light* (stable handle for this frame)
        int     type;          // LightType
        float   pos[3];        // world space
        float   dir[3];        // world space (dir/spot)
        float   color[3];      // linear RGB (from specular)
        float   intensity;     // powerScale
        float   range;         // reach estimate (see note above)
    };

    // Install the createLight/destroyLight hooks. Idempotent and safe to call
    // before OGRE is loaded (it will no-op and a later call installs).
    void EnsureHooks();

    // Snapshot the current live lights into out[0..maxOut). Returns the count
    // written. Thread-safe. Reads each Ogre::Light's current properties.
    int GetLights(Light* out, int maxOut);

    // Total live light count (cheap).
    int GetLightCount();

    // The SceneManager that owns the lights (== the world SM). NULL until the
    // first createLight fires.
    void* GetWorldSceneManager();

    // One-shot diagnostic dump to the log (no-op after the first successful dump).
    void DebugDumpOnce();
}
