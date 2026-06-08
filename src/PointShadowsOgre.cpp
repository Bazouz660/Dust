#include "PointShadowsOgre.h"
#include "PointShadows.h"
#include "DustLog.h"
#include "SceneAccess.h"
#include "D3D11Hook.h"

#include <d3d11.h>
#include <exception>
#include <cstring>
#include <vector>
#include <fstream>

#include <ogre/OgreColourValue.h>
#include <ogre/OgreRoot.h>
#include <ogre/OgreSceneManager.h>
#include <ogre/OgreSceneNode.h>
#include <ogre/OgreCamera.h>
#include <ogre/OgreTextureManager.h>
#include <ogre/OgreTexture.h>
#include <ogre/OgreHardwarePixelBuffer.h>
#include <ogre/OgreRenderTexture.h>
#include <ogre/OgreRenderSystem.h>
#include <ogre/OgreViewport.h>

// SAFETY: KenshiLib's OGRE headers do NOT perfectly match the modified Tindalos binary
// layout (ShadowProbe found cmInline != cmExported). So we only CALL real/virtual exported
// functions (which execute the binary's own code) and never rely on inline field accessors.

namespace PointShadowsOgre
{

static bool sInit = false, sFailed = false, sSpikeDone = false;
static Ogre::Root*          sRoot = nullptr;
static Ogre::RenderSystem*  sRS   = nullptr;
static Ogre::SceneManager*  sSM   = nullptr;
static Ogre::TexturePtr     sTex;
static Ogre::RenderTexture* sRT   = nullptr;
static Ogre::Viewport*      sVP   = nullptr;
static Ogre::Camera*        sCam  = nullptr;

// _setCurrentRenderStage/_getCurrentRenderStage are INLINE in KenshiLib's header (they
// write/read mIlluminationStage at the HEADER's offset, which differs from the Tindalos
// binary -> the inline write hit the wrong field and IRS_RENDER_TO_TEXTURE never took).
// The binary EXPORTS the real member functions, so we resolve + call those (correct
// offset). x64 thiscall: `this` in RCX maps to the first free-function arg.
typedef void (WINAPI* SetStageFn)(void* sm, int stage);
typedef int  (WINAPI* GetStageFn)(void* sm);
static SetStageFn pSetStage = nullptr;
static GetStageFn pGetStage = nullptr;

// Exported SceneManager::getCameras() — same inline-vs-exported trap as the stage fns.
// Used to find Kenshi's main camera (the one that isn't ours) for the LOD camera, so our
// light-POV render doesn't drag Kenshi's terrain LOD/streaming to the light position.
typedef const std::vector<Ogre::Camera*>& (WINAPI* GetCamsFn)(void* sm);
static GetCamsFn pGetCams = nullptr;

// Exported Camera::getDerivedPosition() — to test whether Kenshi's rebase R equals the
// camera world position (which would give a reliable R without the vote estimator).
typedef const Ogre::Vector3* (WINAPI* GetPosFn)(void* cam);
static GetPosFn pGetDerivedPos = nullptr;

// Exported Camera::getDerivedOrientation() — to verify our per-face setOrientation took.
typedef const Ogre::Quaternion* (WINAPI* GetOrientFn)(void* cam);
static GetOrientFn pGetDerivedOrient = nullptr;
static int sOrientLog = 6;   // log the first 6 face directions once

// Exported SceneManager::getCameraInProgress() — the actual MAIN render camera (valid
// at swapBuffers from Kenshi's last render, before our own cull overwrites it). The
// first non-dust camera in getCameras() was NOT the main camera.
typedef void* (WINAPI* GetCamInProgressFn)(void* sm);
static GetCamInProgressFn pGetCamInProgress = nullptr;

// Light's position in the OGRE GEOMETRY frame (same frame as Camera::getDerivedPosition):
// light->getParentNode()->_getDerivedPosition(). SceneAccess's getAs4DVector gives the
// DEFERRED-frame position (matches light_fs) which is a different frame than the geometry,
// so we must render the cube from this OGRE-frame position instead. Node::_getDerivedPosition
// returns Vector3 BY VALUE -> x64 sret: hidden return ptr in RCX, `this` in RDX.
typedef void* (WINAPI* GetParentNodeFn)(void* movable);
typedef void  (WINAPI* NodeGetPosFn)(Ogre::Vector3* ret, void* node);
static GetParentNodeFn pGetParentNode = nullptr;
static NodeGetPosFn    pNodeGetDerivedPos = nullptr;

static bool LightOgrePos(void* light, float* out3)
{
    // DISABLED: Node::_getDerivedPosition access-violates on Kenshi's light nodes (Tindalos
    // node layout hazard). Returns false -> caller falls back to the deferred light - R.
    (void)light; (void)out3;
    return false;
}

// ---- Crash diagnostic: a VEH active only during the spike render, to capture the
// faulting instruction + callstack (mapped to module+offset) of the cull access
// violation, so we can pinpoint it in the OGRE source instead of guessing.
static bool  gSpikeActive = false;
static PVOID gVEH         = nullptr;

extern "C" USHORT WINAPI RtlCaptureStackBackTrace(ULONG, ULONG, PVOID*, PULONG);

static void ModOff(void* addr, char* out, size_t n)
{
    HMODULE h = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &h) && h)
    {
        char path[MAX_PATH] = {}; GetModuleFileNameA(h, path, MAX_PATH);
        const char* b = strrchr(path, '\\'); b = b ? b + 1 : path;
        snprintf(out, n, "%s+0x%llX", b, (unsigned long long)((ULONG_PTR)addr - (ULONG_PTR)h));
    }
    else
        snprintf(out, n, "0x%llX(?)", (unsigned long long)(ULONG_PTR)addr);
}

static LONG CALLBACK SpikeVEH(EXCEPTION_POINTERS* ep)
{
    if (!gSpikeActive) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    gSpikeActive = false;   // log once

    ULONG_PTR rw  = ep->ExceptionRecord->NumberParameters >= 1 ? ep->ExceptionRecord->ExceptionInformation[0] : 0;
    ULONG_PTR acc = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    char at[256]; ModOff(ep->ExceptionRecord->ExceptionAddress, at, sizeof(at));
    Log("PSOgre CRASH: ACCESS_VIOLATION %s accessing 0x%llX at %s",
        rw == 1 ? "WRITE" : "READ", (unsigned long long)acc, at);

    PVOID frames[48] = {};
    USHORT nf = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < nf; ++i)
    {
        char fb[256]; ModOff(frames[i], fb, sizeof(fb));
        Log("  PSOgre stk[%02d] %s", (int)i, fb);
    }
    return EXCEPTION_CONTINUE_SEARCH;   // logged the location; let it crash as before
}

static bool EnsureInit()
{
    if (sInit)   return true;
    if (sFailed) return false;

    if (!gVEH) gVEH = AddVectoredExceptionHandler(1, SpikeVEH);

    if (!pSetStage)
    {
        HMODULE og = GetModuleHandleA("OgreMain_x64.dll");
        if (og)
        {
            pSetStage = (SetStageFn)GetProcAddress(og, "?_setCurrentRenderStage@SceneManager@Ogre@@QEAAXW4IlluminationRenderStage@12@@Z");
            pGetStage = (GetStageFn)GetProcAddress(og, "?_getCurrentRenderStage@SceneManager@Ogre@@QEBA?AW4IlluminationRenderStage@12@@Z");
            pGetCams  = (GetCamsFn)GetProcAddress(og, "?getCameras@SceneManager@Ogre@@QEBAAEBV?$vector@PEAVCamera@Ogre@@V?$STLAllocator@PEAVCamera@Ogre@@V?$CategorisedAllocPolicy@$0A@@2@@2@@std@@XZ");
            pGetDerivedPos = (GetPosFn)GetProcAddress(og, "?getDerivedPosition@Camera@Ogre@@QEBAAEBVVector3@2@XZ");
            pGetDerivedOrient = (GetOrientFn)GetProcAddress(og, "?getDerivedOrientation@Camera@Ogre@@QEBAAEBVQuaternion@2@XZ");
            pGetCamInProgress = (GetCamInProgressFn)GetProcAddress(og, "?getCameraInProgress@SceneManager@Ogre@@QEBAPEAVCamera@2@XZ");
            pGetParentNode    = (GetParentNodeFn)GetProcAddress(og, "?getParentNode@MovableObject@Ogre@@QEBAPEAVNode@2@XZ");
            pNodeGetDerivedPos= (NodeGetPosFn)GetProcAddress(og, "?_getDerivedPosition@Node@Ogre@@QEBA?AVVector3@2@XZ");
            Log("PSOgre: exported fns setStage=%p getStage=%p getCams=%p getPos=%p getOri=%p camInProg=%p parentNode=%p nodePos=%p", pSetStage, pGetStage, pGetCams, pGetDerivedPos, pGetDerivedOrient, pGetCamInProgress, pGetParentNode, pNodeGetDerivedPos);
        }
    }

    sRoot = Ogre::Root::getSingletonPtr();
    if (!sRoot) return false;                 // OGRE not up yet
    void* smv = SceneAccess::GetWorldSceneManager();
    if (!smv) return false;                   // no scene yet
    sSM = reinterpret_cast<Ogre::SceneManager*>(smv);

    sRS = sRoot->getRenderSystem();
    Ogre::TextureManager* tm = sRoot->getTextureManager();
    if (!sRS || !tm) { sFailed = true; Log("PSOgre: rs/tm null (rs=%p tm=%p)", sRS, tm); return false; }
    Log("PSOgre: root=%p rs=%p sm=%p tm=%p", sRoot, sRS, sSM, tm);

    // OGRE-owned float-R render target. Wrap creation in try/catch: OGRE throws on any
    // error (e.g. duplicate resource name), and an uncaught throw mid-init would leak
    // the texture and re-throw on the duplicate name every frame (the looping bug).
    try
    {
        sTex = tm->createManual("dustPtOgreRT", "General", Ogre::TEX_TYPE_2D,
                                512, 512, 0, Ogre::PF_FLOAT32_R, Ogre::TU_RENDERTARGET);
        if (sTex.isNull()) { sFailed = true; Log("PSOgre: createManual null"); return false; }
        sRT = sTex->getBuffer()->getRenderTarget();
        if (!sRT) { sFailed = true; Log("PSOgre: getRenderTarget null"); return false; }
        sVP = sRT->addViewport(0.0f, 0.0f, 1.0f, 1.0f);

        // createCamera auto-attaches the camera to a node in this OGRE 2.0 build, so we
        // do NOT attachObject (it throws "already attached"). Position via the camera.
        sCam = sSM->createCamera("dustPtOgreCam");
        sCam->setNearClipDistance(1.0f);
        sCam->setFarClipDistance(8000.0f);
        sCam->setAspectRatio(1.0f);
        sCam->setFOVy(Ogre::Radian(1.5708f));     // 90 deg
    }
    catch (const std::exception& e) { sFailed = true; Log("PSOgre: EnsureInit EXCEPTION: %s", e.what()); return false; }
    catch (...)                     { sFailed = true; Log("PSOgre: EnsureInit unknown exception"); return false; }

    Log("PSOgre: created RT=%p vp=%p cam=%p", sRT, sVP, sCam);
    sInit = true;
    return true;
}

// 6 cube-face camera basis vectors (D3D TextureCubeArray convention: +X,-X,+Y,-Y,+Z,-Z).
static const float kFaceDir[6][3] = {
    { 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1}
};
static const float kFaceUp[6][3] = {
    { 0, 1, 0}, { 0, 1, 0}, { 0, 0,-1}, { 0, 0, 1}, { 0, 1, 0}, { 0, 1, 0}
};

static float sLights[64][3] = {};
static void* sLightPtrs[64] = {};
static float sCamPos[3] = {0,0,0};   // render-world camera position (from PointShadows)
static int   sLightCount = 0;
static bool  sPending = false;

// Dirty-cache: the position last rendered into each cube slot, so static lights are
// rendered once and then skipped (which also stops the continuous terrain-streaming
// disturbance — a one-shot render recovers, a per-frame one does not).
static float sRenderedPos[64][3] = {};
static bool  sRenderedValid[64]  = {};

void SetPendingLights(void* const* lights, const float (*pos)[3], const float* renderCamPos, int count)
{
    if (!lights || !pos) { sLightCount = 0; return; }
    int maxc = PointShadows::GetMaxCubes();
    if (count > maxc) count = maxc;
    if (count > 64)   count = 64;
    for (int c = 0; c < count; ++c)
    {
        sLightPtrs[c] = lights[c];
        sLights[c][0] = pos[c][0]; sLights[c][1] = pos[c][1]; sLights[c][2] = pos[c][2];
    }
    if (renderCamPos) { sCamPos[0]=renderCamPos[0]; sCamPos[1]=renderCamPos[1]; sCamPos[2]=renderCamPos[2]; }
    sLightCount = count;
    sPending = true;
}

// Kenshi's main camera (the first SceneManager camera that isn't ours). Used as the LOD
// camera so our light-POV cull keeps terrain LOD/streaming at the player's position.
static Ogre::Camera* GetKenshiLodCamera()
{
    // Prefer the actual main render camera (getCameraInProgress). It's only valid before
    // our own cull sets mCameraInProgress to sCam, so callers grab it at the very start.
    if (pGetCamInProgress && sSM)
    {
        Ogre::Camera* c = (Ogre::Camera*)pGetCamInProgress(sSM);
        if (c && c != sCam) return c;
    }
    if (!pGetCams) return sCam;
    const std::vector<Ogre::Camera*>& cams = pGetCams(sSM);
    for (size_t i = 0; i < cams.size(); ++i)
        if (cams[i] && cams[i] != sCam) return cams[i];
    return sCam;
}

bool GetKenshiRebaseR(float* out3)
{
    if (!out3) return false;
    // Resolve just the bits we need (independent of the RT/camera created in EnsureInit),
    // so this works during Kenshi's light pass before our swapBuffers render has run.
    if (!pGetCams || !pGetDerivedPos || !pGetCamInProgress)
    {
        HMODULE og = GetModuleHandleA("OgreMain_x64.dll");
        if (!og) return false;
        if (!pGetCams)       pGetCams       = (GetCamsFn)GetProcAddress(og, "?getCameras@SceneManager@Ogre@@QEBAAEBV?$vector@PEAVCamera@Ogre@@V?$STLAllocator@PEAVCamera@Ogre@@V?$CategorisedAllocPolicy@$0A@@2@@2@@std@@XZ");
        if (!pGetDerivedPos) pGetDerivedPos = (GetPosFn)GetProcAddress(og, "?getDerivedPosition@Camera@Ogre@@QEBAAEBVVector3@2@XZ");
        if (!pGetCamInProgress) pGetCamInProgress = (GetCamInProgressFn)GetProcAddress(og, "?getCameraInProgress@SceneManager@Ogre@@QEBAPEAVCamera@2@XZ");
    }
    if (!sSM) sSM = reinterpret_cast<Ogre::SceneManager*>(SceneAccess::GetWorldSceneManager());
    if (!pGetCams || !pGetDerivedPos || !sSM) return false;
    Ogre::Camera* cam = GetKenshiLodCamera();
    if (!cam) return false;
    const Ogre::Vector3* p = pGetDerivedPos(cam);
    if (!p) return false;
    out3[0] = p->x; out3[1] = p->y; out3[2] = p->z;
    return true;
}

// One-shot: dump a 512x512 R32_FLOAT depth texture to a grayscale BMP for direct inspection.
static void DumpDepthBMP(ID3D11Texture2D* tex, const char* path)
{
    if (!tex || !D3D11Hook::gDevice || !D3D11Hook::gContext) return;
    ID3D11DeviceContext* ctx = D3D11Hook::gContext;
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* stg = nullptr;
    if (FAILED(D3D11Hook::gDevice->CreateTexture2D(&sd, nullptr, &stg)) || !stg) return;
    ctx->CopyResource(stg, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m)))
    {
        const UINT W = d.Width, H = d.Height;
        float mn = 1e30f, mx = -1e30f;
        for (UINT y = 0; y < H; ++y) { const float* r = (const float*)((const char*)m.pData + y * m.RowPitch);
            for (UINT x = 0; x < W; ++x) { float v = r[x]; if (v > 0.0f && v < 1e7f) { if (v < mn) mn = v; if (v > mx) mx = v; } } }
        if (mx <= mn) mx = mn + 1.0f;
        const UINT rowBytes = W * 3, imgSize = rowBytes * H, fileSize = 54 + imgSize;
        unsigned char hdr[54] = {}; hdr[0]='B'; hdr[1]='M';
        *(UINT*)&hdr[2]=fileSize; *(UINT*)&hdr[10]=54; *(UINT*)&hdr[14]=40;
        *(int*)&hdr[18]=(int)W; *(int*)&hdr[22]=(int)H;
        *(unsigned short*)&hdr[26]=1; *(unsigned short*)&hdr[28]=24; *(UINT*)&hdr[34]=imgSize;
        std::ofstream f(path, std::ios::binary);
        if (f)
        {
            f.write((const char*)hdr, 54);
            std::vector<unsigned char> row(rowBytes);
            for (int y = (int)H - 1; y >= 0; --y) {   // BMP is bottom-up
                const float* r = (const float*)((const char*)m.pData + y * m.RowPitch);
                for (UINT x = 0; x < W; ++x) { float v = r[x];
                    unsigned char g = (v > 0.0f && v < 1e7f) ? (unsigned char)(255.0f * (v - mn) / (mx - mn)) : 0;
                    row[x*3]=g; row[x*3+1]=g; row[x*3+2]=g; }
                f.write((const char*)row.data(), rowBytes);
            }
            Log("PSOgre: dumped face BMP %s (min=%.1f max=%.1f)", path, mn, mx);
        }
        ctx->Unmap(stg, 0);
    }
    stg->Release();
}

// Orient the OGRE camera (which looks down its local -Z) to view along `dir` with `up`.
static void OrientCamera(const float* dir, const float* up)
{
    Ogre::Vector3 f(dir[0], dir[1], dir[2]); f.normalise();
    Ogre::Vector3 u(up[0], up[1], up[2]);    u.normalise();
    Ogre::Vector3 z = -f;                    // camera local +Z = -view direction
    Ogre::Vector3 x = u.crossProduct(z);     x.normalise();
    Ogre::Vector3 y = z.crossProduct(x);     // re-orthonormalize up
    sCam->setOrientation(Ogre::Quaternion(x, y, z));
}

// At the OGRE swapBuffers frame boundary (cull worker threads idle), render each pending
// light's 6 cube faces — scene DEPTH from the light POV via OGRE's shadow-caster pass —
// and copy each into our cube array. Replaces GeometryReplay as the occluder source.
void RenderPendingAtFrameBoundary()
{
    if (!sPending || sLightCount <= 0) return;
    ID3D11DeviceContext* ctx = D3D11Hook::gContext;
    if (!ctx) return;
    if (!EnsureInit()) return;

    // Save Kenshi's bound D3D11 RTs + viewport (we rebind the immediate context).
    ID3D11RenderTargetView* savedRTV[8] = {};
    ID3D11DepthStencilView* savedDSV = nullptr;
    ctx->OMGetRenderTargets(8, savedRTV, &savedDSV);
    UINT nVP = 16; D3D11_VIEWPORT savedVP[16] = {};
    ctx->RSGetViewports(&nVP, savedVP);

    // IRS_RENDER_TO_TEXTURE -> _setPass derives each material's shadow_caster_material
    // (depth-only, no compositor textures). Via the EXPORTED setter (correct offset).
    int savedStage = pGetStage ? pGetStage(sSM) : 0;
    if (pSetStage) pSetStage(sSM, 1);

    gSpikeActive = true;   // arm the crash VEH (logs faulting addr+callstack on AV)
    int faces = 0, rendered = 0;
    // LOD camera = Kenshi's MAIN camera (getCameraInProgress); grab it BEFORE our cull
    // overwrites mCameraInProgress. R = the main-camera position captured by D3D11Hook
    // DURING the deferred light pass (getCameraInProgress at swapBuffers is a different,
    // non-main camera -> the ground-truth test showed it 882u from the geometry).
    Ogre::Camera* lodCam = GetKenshiLodCamera();
    float R[3] = {0, 0, 0};
    bool rValid = false;
    PointShadows::GetLightSpaceR(R, &rValid);

    // Diagnostic: does Kenshi's rebase R == its camera world position?
    {
        static int dbgf = 0;
        if (pGetDerivedPos && lodCam && ((dbgf++) % 60) == 0)
        {
            const Ogre::Vector3* cp = pGetDerivedPos(lodCam);
            float R[3] = {}; bool rv = false; PointShadows::GetLightSpaceR(R, &rv);
            if (cp) Log("PSOgre: kenshiCamPos=(%.1f,%.1f,%.1f) votedR=(%.1f,%.1f,%.1f) valid=%d",
                        cp->x, cp->y, cp->z, R[0], R[1], R[2], (int)rv);
        }
    }
    try
    {
        for (int c = 0; c < sLightCount; ++c)
        {
            // Dirty-cache: skip a slot whose light hasn't moved (cube still valid).
            // TEMP: forced off for the RenderDoc capture so cube draws are in every frame.
            static const bool kForceRenderForCapture = true;
            float dx = sLights[c][0] - sRenderedPos[c][0];
            float dy = sLights[c][1] - sRenderedPos[c][1];
            float dz = sLights[c][2] - sRenderedPos[c][2];
            if (!kForceRenderForCapture && sRenderedValid[c] && (dx*dx + dy*dy + dz*dz) < 1.0f) continue;

            // GROUND-TRUTH TEST: render the cube from R (the main camera's OGRE position).
            // The player camera is among the geometry, so the dump MUST show geometry only a
            // few units away (min ~tens). If min is ~1000+, R is NOT in the geometry frame.
            float tgt[3] = { R[0], R[1], R[2] };
            (void)sCamPos; (void)sLights;
            sCam->setPosition(Ogre::Vector3(tgt[0], tgt[1], tgt[2]));
            sSM->updateAllTransforms();
            if (pGetDerivedPos)
            {
                const Ogre::Vector3* d = pGetDerivedPos(sCam);
                if (d) sCam->setPosition(Ogre::Vector3(2.0f * tgt[0] - d->x,
                                                       2.0f * tgt[1] - d->y,
                                                       2.0f * tgt[2] - d->z));
            }
            for (int fce = 0; fce < 6; ++fce)
            {
                OrientCamera(kFaceDir[fce], kFaceUp[fce]);
                sSM->updateAllTransforms();
                if (sOrientLog > 0 && pGetDerivedOrient)
                {
                    const Ogre::Quaternion* q = pGetDerivedOrient(sCam);
                    Ogre::Vector3 d = (*q) * Ogre::Vector3(0.0f, 0.0f, -1.0f);
                    Log("PSOgre: face %d want(%.2f,%.2f,%.2f) got(%.2f,%.2f,%.2f)",
                        fce, kFaceDir[fce][0], kFaceDir[fce][1], kFaceDir[fce][2], d.x, d.y, d.z);
                    sOrientLog--;
                }
                sCam->_notifyViewport(sVP);
                sSM->_setCurrentShadowNode(nullptr);
                sVP->_setVisibilityMask(0xFFFFFFFFu);
                sCam->_resetRenderedRqs(256);
                sRT->_beginUpdate();
                sRT->_updateViewportCullPhase01(sVP, sCam, lodCam, 0, 255);
                sRT->_updateViewportRenderPhase02(sVP, sCam, lodCam, 0, 255, false);
                sRT->_endUpdate();

                ID3D11Texture2D* ogreTex = nullptr;
                sRT->getCustomAttribute("ID3D11Texture2D", &ogreTex);
                if (ogreTex) { PointShadows::CopyOgreFaceDepth(ctx, c, fce, ogreTex); ++faces; }
                static int sDumpFaces = 0;
                if (sDumpFaces < 6 && c == 0 && ogreTex)   // dump all 6 faces of light 0 once
                {
                    if (sDumpFaces == 0 && pGetDerivedPos)
                    {
                        const Ogre::Vector3* p = pGetDerivedPos(sCam);
                        if (p) Log("PSOgre: camPos got(%.0f,%.0f,%.0f) want(%.0f,%.0f,%.0f)",
                                   p->x, p->y, p->z, tgt[0], tgt[1], tgt[2]);
                    }
                    char path[160];
                    snprintf(path, sizeof(path), "D:\\Github\\Dust-main\\dustpt_face%d.bmp", fce);
                    DumpDepthBMP(ogreTex, path);
                    sDumpFaces++;
                }
            }
            sRenderedPos[c][0] = sLights[c][0];
            sRenderedPos[c][1] = sLights[c][1];
            sRenderedPos[c][2] = sLights[c][2];
            sRenderedValid[c] = true;
            ++rendered;
        }
    }
    catch (const std::exception& e) { Log("PSOgre: render EXCEPTION: %s", e.what()); }
    catch (...)                     { Log("PSOgre: render unknown exception"); }
    gSpikeActive = false;
    if (pSetStage) pSetStage(sSM, savedStage);

    // Restore Kenshi's RT + viewport.
    ctx->OMSetRenderTargets(8, savedRTV, savedDSV);
    if (nVP > 0) ctx->RSSetViewports(nVP, savedVP);
    for (int i = 0; i < 8; ++i) if (savedRTV[i]) savedRTV[i]->Release();
    if (savedDSV) savedDSV->Release();

    if (rendered > 0)
        Log("PSOgre: (re)rendered %d/%d light cubes (%d faces)", rendered, sLightCount, faces);
}

}
