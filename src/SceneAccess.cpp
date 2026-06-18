#include "SceneAccess.h"
#include "DustLog.h"

#include <core/Functions.h>      // KenshiLib::AddHook
#include <ogre/OgreLight.h>      // Ogre::Light + math types

#include <windows.h>
#include <vector>

namespace SceneAccess
{

// ---- live light set (guarded by a critical section; createLight may fire on a
//      loader thread while the render thread snapshots) ----
static CRITICAL_SECTION   sLock;
static bool               sLockInit = false;
static std::vector<void*> sLights;
static void*              sWorldSceneMgr = nullptr;
static long               sCreatedTotal  = 0;

static void InitLock()
{
    if (!sLockInit) { InitializeCriticalSection(&sLock); sLockInit = true; }
}

// ---- createLight / destroyLight hooks ----
typedef void* (*CreateLightFn)(void* self);
typedef void  (*DestroyLightFn)(void* self, void* light);
static CreateLightFn  sOrigCreateLight  = nullptr;
static DestroyLightFn sOrigDestroyLight = nullptr;

static void* Hook_createLight(void* self)
{
    void* l = sOrigCreateLight(self);
    if (l)
    {
        EnterCriticalSection(&sLock);
        sWorldSceneMgr = self;
        sLights.push_back(l);
        ++sCreatedTotal;
        LeaveCriticalSection(&sLock);
    }
    return l;
}

static void Hook_destroyLight(void* self, void* light)
{
    EnterCriticalSection(&sLock);
    for (size_t i = 0; i < sLights.size(); ++i)
        if (sLights[i] == light) { sLights[i] = sLights.back(); sLights.pop_back(); break; }
    LeaveCriticalSection(&sLock);
    sOrigDestroyLight(self, light);
}

void EnsureHooks()
{
    static bool tried = false;
    if (tried) return;

    HMODULE ogre = GetModuleHandleA("OgreMain_x64.dll");
    if (!ogre) return;   // OGRE not loaded yet — a later call will install
    tried = true;

    InitLock();

    void* cl = (void*)GetProcAddress(ogre, "?createLight@SceneManager@Ogre@@UEAAPEAVLight@2@XZ");
    void* dl = (void*)GetProcAddress(ogre, "?destroyLight@SceneManager@Ogre@@UEAAXPEAVLight@2@@Z");

    if (cl)
    {
        KenshiLib::HookStatus s = KenshiLib::AddHook(cl, &Hook_createLight, (void**)&sOrigCreateLight);
        Log("SceneAccess: createLight hook %s", (s == KenshiLib::SUCCESS) ? "OK" : "FAIL");
    }
    else Log("SceneAccess: createLight export missing");

    if (dl)
    {
        KenshiLib::HookStatus s = KenshiLib::AddHook(dl, &Hook_destroyLight, (void**)&sOrigDestroyLight);
        Log("SceneAccess: destroyLight hook %s", (s == KenshiLib::SUCCESS) ? "OK" : "FAIL");
    }
    else Log("SceneAccess: destroyLight export missing");
}

static int ReadLightsLocked(Light* out, int maxOut)
{
    int n = 0;
    for (size_t i = 0; i < sLights.size() && n < maxOut; ++i)
    {
        Ogre::Light* l = (Ogre::Light*)sLights[i];
        if (!l) continue;

        Light& o = out[n];
        o.ogreLight = l;
        o.type      = (int)l->getType();

        Ogre::Vector4 p = l->getAs4DVector();   // pos (w=1) for point/spot
        o.pos[0] = p.x; o.pos[1] = p.y; o.pos[2] = p.z;

        Ogre::Vector3 d = l->getDirection();
        o.dir[0] = d.x; o.dir[1] = d.y; o.dir[2] = d.z;

        const Ogre::ColourValue& c = l->getSpecularColour();   // Kenshi stores color here
        o.color[0] = c.r; o.color[1] = c.g; o.color[2] = c.b;

        o.intensity = l->getPowerScale();

        // Range: Kenshi doesn't reliably set OGRE attenuation, so fall back to a
        // power-derived estimate when the engine value is unset. TODO: source the
        // true reach from Kenshi's light-volume radius / LightEnt.
        float r = l->getAttenuationRange();
        if (r <= 0.01f) r = 8.0f + 0.15f * o.intensity;
        o.range = r;

        ++n;
    }
    return n;
}

int GetLights(Light* out, int maxOut)
{
    EnsureHooks();
    if (!sLockInit || !out || maxOut <= 0) return 0;
    EnterCriticalSection(&sLock);
    int n = ReadLightsLocked(out, maxOut);
    LeaveCriticalSection(&sLock);
    return n;
}

int GetLightCount()
{
    if (!sLockInit) return 0;
    EnterCriticalSection(&sLock);
    int n = (int)sLights.size();
    LeaveCriticalSection(&sLock);
    return n;
}

void* GetWorldSceneManager() { return sWorldSceneMgr; }

void DebugDumpOnce()
{
    static bool done = false;
    if (done) return;

    EnsureHooks();
    int count = GetLightCount();
    if (count == 0)
    {
        static int sw = 0;
        if ((sw++ % 180) == 0)
            Log("SceneAccess: no lights yet (createLight fired %ld) — load into a lit area", sCreatedTotal);
        return;
    }

    static std::vector<Light> buf;
    buf.resize(count);
    int n = GetLights(&buf[0], count);

    int nPoint = 0, nDir = 0, nSpot = 0;
    for (int i = 0; i < n; ++i)
    {
        if      (buf[i].type == 0) nPoint++;
        else if (buf[i].type == 1) nDir++;
        else if (buf[i].type == 2) nSpot++;
    }
    Log("SceneAccess: %d live lights (point=%d dir=%d spot=%d) worldSM=%p",
        n, nPoint, nDir, nSpot, sWorldSceneMgr);

    int dump = n < 6 ? n : 6;
    for (int i = 0; i < dump; ++i)
    {
        const Light& L = buf[i];
        Log("  [%d] type=%d pos=(%.0f,%.0f,%.0f) col=(%.2f,%.2f,%.2f) int=%.1f rng=%.1f",
            i, L.type, L.pos[0], L.pos[1], L.pos[2],
            L.color[0], L.color[1], L.color[2], L.intensity, L.range);
    }

    done = true;
}

} // namespace SceneAccess
