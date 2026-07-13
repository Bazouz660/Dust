#include "CameraAccess.h"

#include <cstdint>
#include <cmath>
#include <cstring>

// KenshiLib reconstructed structs + OGRE + the resolved-function API.
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/CameraClass.h>
#include <core/Functions.h>     // KenshiLib::GetRealAddress
#include <ogre/OgreCamera.h>

#include <windows.h>

static GameWorld* sGameWorld = nullptr;
static volatile int sStep = 0;   // last step reached; negative = access-violation caught at that step

static unsigned long long sGameLoopTick = 0;

void CameraAccess_SetGameWorld(void* gameWorld)
{
    sGameWorld = reinterpret_cast<GameWorld*>(gameWorld);
    ++sGameLoopTick;   // advances only while the game is actively simulating (stalls during load/menu)
}

unsigned long long CameraAccess_GameLoopTick() { return sGameLoopTick; }

int CameraAccess_Status() { return sStep; }

// IMPORTANT: the KenshiLib header struct *layouts* don't match the game ABI under this
// compiler (v143 vs the game's VS2010 — e.g. std::string is 32B here vs 40B in-game), so
// `obj->member` computes the wrong offset. Walk with RAW documented byte offsets instead.
static inline void* RawPtr(const void* base, size_t off)
{
    return *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(base) + off);
}
static inline bool LooksLikePtr(const void* p)
{
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v > 0x10000 && v < 0x00007FFFFFFFFFFFull;
}

// PlayerInterface::getCamera() — non-virtual, RVA 0x3E6B40. Resolved via KenshiLib (the same
// pattern dllmain uses for hooks); reads +0x30 internally with the correct game ABI, so it's
// robust to the layout drift. x64 has one calling convention, so a free fn taking 'this' works.
typedef CameraClass* (*PfnGetCamera)(void* playerThis);
static PfnGetCamera ResolveGetCamera()
{
    static PfnGetCamera fn = reinterpret_cast<PfnGetCamera>(
        KenshiLib::GetRealAddress(&PlayerInterface::getCamera));
    return fn;
}

// Walk GameWorld -> player -> camera -> Ogre::Camera and read the view + projection matrices as raw
// 16-float row-major arrays (Matrix4 = Real m[4][4], row-major). Shared by the VP and param getters.
static bool GetCameraMatricesRaw(float outView[16], float outProj[16])
{
    sStep = 1;
    if (!LooksLikePtr(sGameWorld)) return false;

    // Hop 1: GameWorld::player — raw +0x580.
    sStep = 2;
    void* player = RawPtr(sGameWorld, 0x580);
    if (!LooksLikePtr(player)) return false;

    // Hop 2: PlayerInterface::camera — prefer the game's getCamera() accessor; raw +0x30 fallback.
    sStep = 3;
    CameraClass* cc = nullptr;
    if (PfnGetCamera getCam = ResolveGetCamera()) cc = getCam(player);
    if (!LooksLikePtr(cc)) cc = reinterpret_cast<CameraClass*>(RawPtr(player, 0x30));
    if (!LooksLikePtr(cc)) return false;

    // Hop 3: CameraClass::camera (Ogre::Camera*) — raw +0x68.
    sStep = 4;
    Ogre::Camera* cam = reinterpret_cast<Ogre::Camera*>(RawPtr(cc, 0x68));
    if (!LooksLikePtr(cam)) return false;

    // Virtual getters (vtable dispatch, offset 0 — ABI-invariant). RSDepth = D3D [0,1] proj.
    sStep = 5;
    const Ogre::Matrix4& viewM = cam->getViewMatrix();
    sStep = 6;
    const Ogre::Matrix4& projM = cam->getProjectionMatrixWithRSDepth();
    sStep = 7;

    memcpy(outView, reinterpret_cast<const float*>(&viewM), 16 * sizeof(float));
    memcpy(outProj, reinterpret_cast<const float*>(&projM), 16 * sizeof(float));
    return true;
}

static bool GetViewProjRaw(float outVP[16])
{
    float V[16], P[16];
    if (!GetCameraMatricesRaw(V, P)) return false;

    // VP = P*V (column-vector), stored TRANSPOSED to column-major. V[row*4+col], P[row*4+col].
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += P[r * 4 + k] * V[k * 4 + c];
            outVP[c * 4 + r] = s;
        }
    sStep = 8;
    return true;
}

// Extract near/far/fovY from the OGRE D3D-RSDepth perspective projection (row-major m[row][col]):
//   m[2][2] = -far/(far-near), m[2][3] = -far*near/(far-near)  =>  near = m23/m22, far = m23/(m22+1)
//   m[1][1] = 1/tan(fovY/2)                                     =>  fovY = 2*atan(1/m11)
static bool GetCameraParamsRaw(float* nearZ, float* farZ, float* fovY)
{
    float V[16], P[16];
    if (!GetCameraMatricesRaw(V, P)) return false;

    float m22 = P[2 * 4 + 2], m23 = P[2 * 4 + 3], m11 = P[1 * 4 + 1];
    if (m22 == 0.0f || (m22 + 1.0f) == 0.0f || m11 == 0.0f) return false;
    float n = m23 / m22;
    float f = m23 / (m22 + 1.0f);
    float fov = 2.0f * atanf(1.0f / m11);
    // Sanity gate — reject nonsense (infinite/reversed-Z or a faulted read).
    if (!(n > 0.0f && f > n && fov > 0.05f && fov < 3.14f)) return false;
    if (nearZ) *nearZ = n;
    if (farZ)  *farZ  = f;
    if (fovY)  *fovY  = fov;
    sStep = 8;
    return true;
}

bool CameraAccess_GetViewProj(float outVP[16])
{
    __try { return GetViewProjRaw(outVP); }
    __except (EXCEPTION_EXECUTE_HANDLER) { sStep = -sStep; return false; }
}

bool CameraAccess_GetCameraParams(float* nearZ, float* farZ, float* fovYRadians)
{
    __try { return GetCameraParamsRaw(nearZ, farZ, fovYRadians); }
    __except (EXCEPTION_EXECUTE_HANDLER) { sStep = -sStep; return false; }
}
