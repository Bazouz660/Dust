#include "PssmDetour.h"
#include "DustLog.h"

#include <core/Functions.h>  // KenshiLib::AddHook
#include <windows.h>
#include <atomic>
#include <cmath>

namespace PssmDetour
{

// Diagnostic detours on multiple OGRE shadow-camera-setup methods. Aim is to
// figure out which path Kenshi uses for split distances and per-frame shadow
// camera placement, so we can pick a viable hook point for live lambda tuning.
//
// Each hook logs early calls and forwards unchanged. No behavior change.

static bool sInstalled = false;

// ===== calculateSplitPoints (already known not to fire — kept as control) =====

typedef void (*CalcSplitFn)(void* self, unsigned int splitCount,
                            float nearDist, float farDist, float lambda);
static CalcSplitFn sOrig_calculateSplitPoints = nullptr;
static int         sCount_calculateSplitPoints = 0;

static void Hook_calculateSplitPoints(void* self, unsigned int splitCount,
                                      float nearDist, float farDist, float lambda)
{
    int n = ++sCount_calculateSplitPoints;
    if (n <= 5 || (n % 600) == 0)
        Log("PssmDetour: calculateSplitPoints #%d self=%p splits=%u "
            "near=%.4f far=%.3f lambda=%.4f",
            n, self, splitCount, (double)nearDist, (double)farDist, (double)lambda);
    sOrig_calculateSplitPoints(self, splitCount, nearDist, farDist, lambda);
}

// ===== setSplitPoints (manual splits API — would explain calc not firing) =====

typedef void (*SetSplitsFn)(void* self, const void* splitVec);
static SetSplitsFn sOrig_setSplitPoints = nullptr;
static int         sCount_setSplitPoints = 0;

static void Hook_setSplitPoints(void* self, const void* splitVec)
{
    int n = ++sCount_setSplitPoints;
    if (n <= 5 || (n % 600) == 0)
    {
        // splitVec is a std::vector<float>; first 3 qwords are
        // (begin, end, capacity_end) for MSVC STL.
        const float* const* pp = (const float* const*)splitVec;
        const float* begin = pp ? pp[0] : nullptr;
        const float* end   = pp ? pp[1] : nullptr;
        size_t count = (begin && end) ? (size_t)(end - begin) : 0;
        Log("PssmDetour: setSplitPoints #%d self=%p count=%zu", n, self, count);
        for (size_t i = 0; i < count && i < 8; i++)
            Log("    [%zu] %.4f", i, (double)begin[i]);
    }
    sOrig_setSplitPoints(self, splitVec);
}

// ===== getShadowCamera virtuals (one per concrete subclass — exactly one
//       should fire per shadow-camera per shadow-pass per frame, telling us
//       which class Kenshi uses) =====

typedef void (*GetShadowCamFn)(const void* self, const void* sm, const void* cam,
                               const void* light, void* texCam, size_t iteration);

#define DEFINE_GSC_HOOK(NAME)                                                \
    static GetShadowCamFn sOrig_getShadowCamera_##NAME = nullptr;            \
    static int            sCount_getShadowCamera_##NAME = 0;                 \
    static void Hook_getShadowCamera_##NAME(                                 \
        const void* self, const void* sm, const void* cam,                   \
        const void* light, void* texCam, size_t iteration)                   \
    {                                                                        \
        int n = ++sCount_getShadowCamera_##NAME;                             \
        if (n <= 3 || (n % 600) == 0)                                        \
            Log("PssmDetour: getShadowCamera." #NAME " #%d self=%p iter=%zu",\
                n, self, iteration);                                         \
        sOrig_getShadowCamera_##NAME(self, sm, cam, light, texCam, iteration);\
    }

DEFINE_GSC_HOOK(Default)
DEFINE_GSC_HOOK(Focused)
DEFINE_GSC_HOOK(PSSM)
DEFINE_GSC_HOOK(PlaneOptimal)

#undef DEFINE_GSC_HOOK

// ===== Base ShadowCameraSetup ctor/dtor (fires for any subclass, including
//       a hypothetical Kenshi-custom one, if it derives from OGRE's base) =====

typedef void (*BaseCtorFn)(void* self);
static BaseCtorFn sOrig_ShadowCameraSetup_Ctor = nullptr;
static int        sCount_ShadowCameraSetup_Ctor = 0;

static void Hook_ShadowCameraSetup_Ctor(void* self)
{
    int n = ++sCount_ShadowCameraSetup_Ctor;
    if (n <= 5)
    {
        // Capture vtable AFTER the base ctor finishes — though derived ctors
        // overwrite it later, so the value here is just the base's.
        sOrig_ShadowCameraSetup_Ctor(self);
        void* vt = *(void**)self;
        Log("PssmDetour: ShadowCameraSetup::ctor #%d self=%p vtable-after-base=%p",
            n, self, vt);
        return;
    }
    sOrig_ShadowCameraSetup_Ctor(self);
}

typedef void (*BaseDtorFn)(void* self);
static BaseDtorFn sOrig_ShadowCameraSetup_Dtor = nullptr;
static int        sCount_ShadowCameraSetup_Dtor = 0;

static void Hook_ShadowCameraSetup_Dtor(void* self)
{
    int n = ++sCount_ShadowCameraSetup_Dtor;
    if (n <= 5)
        Log("PssmDetour: ShadowCameraSetup::dtor #%d self=%p", n, self);
    sOrig_ShadowCameraSetup_Dtor(self);
}

// ===== CompositorShadowNode ctor and per-frame _update =====

typedef void (*CSNCtorFn)(void* self, unsigned __int64 id, const void* def,
                          void* workspace, void* renderSys, const void* renderTarget);
static CSNCtorFn sOrig_CSN_Ctor = nullptr;
static int       sCount_CSN_Ctor = 0;

static void Hook_CSN_Ctor(void* self, unsigned __int64 id, const void* def,
                          void* workspace, void* renderSys, const void* renderTarget)
{
    int n = ++sCount_CSN_Ctor;
    if (n <= 5)
        Log("PssmDetour: CompositorShadowNode::ctor #%d self=%p id=%llu def=%p ws=%p",
            n, self, (unsigned long long)id, def, workspace);
    sOrig_CSN_Ctor(self, id, def, workspace, renderSys, renderTarget);
}

typedef void (*CSNUpdateFn)(void* self, void* camera, const void* lodCamera,
                            void* sceneManager);
static CSNUpdateFn sOrig_CSN_Update = nullptr;
static int         sCount_CSN_Update = 0;

static void Hook_CSN_Update(void* self, void* camera, const void* lodCamera,
                            void* sceneManager)
{
    int n = ++sCount_CSN_Update;
    if (n <= 3 || (n % 600) == 0)
        Log("PssmDetour: CompositorShadowNode::_update #%d self=%p", n, self);
    sOrig_CSN_Update(self, camera, lodCamera, sceneManager);
}

// ===== SceneManager::_injectRenderWithPass — captures the Pass identity
//       and resolves its name + material name via OGRE's exported getters. =====

typedef void (*InjectRenderFn)(void* self, void* pass, void* rend, void* camera,
                               bool firstRenderable, bool casterPass);
static InjectRenderFn sOrig_injectRenderWithPass = nullptr;
static int            sCount_injectRenderWithPass = 0;

// Returns const std::string& — in x64 ABI a reference is a plain pointer in RAX.
typedef const void* (*GetNameFn)(const void* self);   // returns const std::string*
typedef void*       (*GetParentFn)(const void* self); // returns Technique* / Material*
static GetNameFn   sFn_Pass_getName       = nullptr;
static GetNameFn   sFn_Technique_getName  = nullptr;
static GetParentFn sFn_Pass_getParent     = nullptr;
static GetParentFn sFn_Technique_getParent= nullptr;
static GetNameFn   sFn_Material_getName   = nullptr;  // resolved if available

// Decode an MSVC std::string<char> buffer to a null-terminated c-string.
// Layout (stable since VS2015): union(buf[16] | char*) | size_t size | size_t cap.
// SSO when cap <= 15, heap otherwise. Returns "(null)" / "(invalid)" on error.
static const char* DecodeStdString(const void* s)
{
    if (!s) return "(null)";
    __try
    {
        size_t cap = *(const size_t*)((const char*)s + 24);
        if (cap <= 15)
            return (const char*)s;
        const char* p = *(const char* const*)s;
        return p ? p : "(null-heap)";
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return "(invalid)";
    }
}

// ===== Orchestrator function entry (Kenshi+0x8629a0). Captures `this` so
//       we can later inspect the source struct that holds the splits. =====

typedef void* (*OrchestratorFn)(void* self, ...);
static OrchestratorFn sOrig_orchestrator = nullptr;
static void*          sOrchestratorThis  = nullptr;
static int            sCount_orchestrator = 0;

// MSVC x64 ABI: RCX is `this`. We can't know other args without a real
// signature, but we just need this. We declare the hook to take varargs and
// forward via the original trampoline; the trampoline preserves all regs/stack.
static void* Hook_orchestrator_entry(void* self, void* a2, void* a3, void* a4)
{
    sOrchestratorThis = self;
    int n = ++sCount_orchestrator;
    if (n <= 3)
        Log("PssmDetour: orchestrator entry #%d this=%p", n, self);
    // Forward — note: the function's real signature may have more args, but
    // since MSVC x64 passes args 5+ on the stack and we don't touch the stack
    // pointer, the trampoline call still receives them correctly through the
    // unchanged stack frame. The 4 register args we declare are pass-through.
    return ((OrchestratorFn)sOrig_orchestrator)(self, a2, a3, a4);
}

// ===== GpuSharedParameters::getFloatPointer — raw write entry. Kenshi's
//       perf-sensitive shadow code likely writes csmParams via direct float*
//       writes rather than per-element setNamedConstant. Captures stack on
//       the first few calls so we can find Kenshi's shadow pass code. =====

typedef float* (*GetFloatPointerFn)(void* self, size_t pos);
static GetFloatPointerFn sOrig_getFloatPointer = nullptr;
static int               sCount_getFloatPointer = 0;

static bool sSourceDumped = false;

// Runtime-controllable PSSM lambda. Kenshi's native value back-solves to
// roughly 0.95-0.97; default to 0.95 so that with no slider movement the
// patch produces near-identical splits to vanilla.
static std::atomic<float> sLambda{0.95f};

// State captured at first-hook time so SetLambda can push live updates.
// Kenshi only calls getFloatPointer(pos=24) once at scene init — the cbuffer
// values then live in GpuSharedParameters and field_0x10 forever — so we
// have to write to both ourselves whenever lambda changes.
static void*  sSharedParams         = nullptr;  // GpuSharedParameters instance
static float* sSplitsArray          = nullptr;  // orchestrator-this->field_0x10
static float  sCachedNear           = 0.0f;
static float  sCachedFar            = 0.0f;
static std::atomic<float> sCascadeFilterScale[4] = {
    {1.0f}, {1.0f}, {1.0f}, {1.0f}              // per-cascade filter multipliers
};

// Compute and write splits[1..3] into a 5-float [near, s1, s2, s3, far] array
// using the current lambda + cached near/far. Returns true if writes happened.
static bool RecomputeSplits()
{
    float n = sCachedNear, f = sCachedFar;
    if (!(n > 0.0f) || !(f > n) || !sSplitsArray) return false;
    const float lambda = sLambda.load();
    const int N = 4;
    for (int i = 1; i < N; i++)
    {
        float r = (float)i / (float)N;
        float logT = n * powf(f / n, r);
        float linT = n + (f - n) * r;
        sSplitsArray[i] = lambda * logT + (1.0f - lambda) * linT;
    }
    return true;
}

// Push current splits to OGRE's GpuSharedParameters (csmParams[i].x) and to
// Kenshi's source array (field_0x10). Idempotent; safe to call from any
// setter. csmParams[i].y (filter radius) is patched separately at cbuffer
// commit time by ApplyFilterScalesToCbuffer — Kenshi doesn't write .y
// through GpuSharedParameters so we can't reach it from here.
static void RepushAll()
{
    if (!RecomputeSplits()) return;
    if (!sSharedParams || !sOrig_getFloatPointer) return;
    float* dest = sOrig_getFloatPointer(sSharedParams, 24);
    if (!dest) return;
    for (int i = 0; i < 4; i++)
        dest[i*4 + 0] = sSplitsArray[i+1] - sCachedNear;
}

void SetLambda(float lambda)
{
    if (lambda < 0.0f) lambda = 0.0f;
    if (lambda > 1.0f) lambda = 1.0f;
    sLambda.store(lambda);
    RepushAll();
}

float GetLambda() { return sLambda.load(); }

void SetCascadeFilterScale(int idx, float scale)
{
    if (idx < 0 || idx >= 4) return;
    if (scale < 0.0f) scale = 0.0f;
    if (scale > 5.0f) scale = 5.0f;
    sCascadeFilterScale[idx].store(scale);
    // No RepushAll needed — filter scaling lands at cbuffer commit time.
}

float GetCascadeFilterScale(int idx)
{
    if (idx < 0 || idx >= 4) return 1.0f;
    return sCascadeFilterScale[idx].load();
}

void ApplyFilterScalesToCbuffer(void* cbufferData)
{
    if (!cbufferData) return;
    // csmParams is at byte offset 208; per-cascade .y is at +4 within each
    // 16-byte float4. Cascades 0..3 → offsets 212, 228, 244, 260.
    char* base = (char*)cbufferData;
    for (int i = 0; i < 4; i++)
    {
        float* y = (float*)(base + 208 + i*16 + 4);
        *y = (*y) * sCascadeFilterScale[i].load();
    }
}

static float* Hook_getFloatPointer(void* self, size_t pos)
{
    int n = ++sCount_getFloatPointer;


    // When pos==24 (csmParams), unwind the call stack to locate the Kenshi
    // orchestrator frame, recover its RDI (= this), and patch the splits
    // source struct each frame. RtlVirtualUnwind reconstructs callee-saved
    // regs from unwind info; works even though MSVC's prologue clobbers our
    // local rdi by the time C++ code runs.
    if (pos == 24)
    {
        bool firstTime = !sSourceDumped;
        sSourceDumped = true;
        __try
        {
            HMODULE kenshiMod = GetModuleHandleA("Kenshi_x64.exe");
            uintptr_t kenshiBase = (uintptr_t)kenshiMod;

            CONTEXT ctx;
            RtlCaptureContext(&ctx);

            // Unwind up to ~12 frames looking for one whose RIP is in the
            // orchestrator's address range (the function we disasm'd that
            // contains the call site at +0x862c34..+0x864221).
            void* thisPtr = nullptr;
            for (int frame = 0; frame < 16; frame++)
            {
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (!fn) break;
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn,
                                 &ctx, &handlerData, &establisher, nullptr);
                uintptr_t kenshiRva = (kenshiBase && ctx.Rip >= kenshiBase)
                    ? ctx.Rip - kenshiBase : 0;
                if (firstTime)
                    Log("PssmDetour:  unwind frame %d  rip=%p (kenshi+0x%llx) rdi=%p",
                        frame, (void*)ctx.Rip, (unsigned long long)kenshiRva,
                        (void*)ctx.Rdi);
                if (kenshiRva >= 0x862000 && kenshiRva <= 0x866000)
                {
                    thisPtr = (void*)ctx.Rdi;
                    if (firstTime)
                        Log("PssmDetour:  -> matched orchestrator frame, this=%p",
                            thisPtr);
                    break;
                }
            }

            if (thisPtr)
            {
                void** thisQ = (void**)thisPtr;
                void* field0x10 = thisQ[2];
                if (field0x10)
                {
                    float* fp = (float*)field0x10;
                    const float nearD = fp[0];
                    const float farD  = fp[4];
                    const float lambda = sLambda.load();
                    const int N = 4;

                    // Cache pointers + dimensions so SetLambda can push live
                    // updates without waiting for Kenshi to write again.
                    sSharedParams = self;
                    sSplitsArray  = fp;
                    sCachedNear   = nearD;
                    sCachedFar    = farD;
                    // Filter radii (csmParams[i].y) get captured on a later
                    // hook call — see top of Hook_getFloatPointer.

                    if (firstTime)
                        Log("PssmDetour: splits BEFORE=[%g, %g, %g, %g, %g] "
                            "near=%g far=%g lambda=%g",
                            (double)fp[0], (double)fp[1], (double)fp[2],
                            (double)fp[3], (double)fp[4],
                            (double)nearD, (double)farD, (double)lambda);

                    // Standard PSSM split formula (OGRE's calculateSplitPoints):
                    //   split[i] = lambda * near*(far/near)^(i/N)
                    //            + (1-lambda) * (near + (far-near)*i/N)
                    if (nearD > 0.0f && farD > nearD)
                    {
                        for (int i = 1; i < N; i++)
                        {
                            float r = (float)i / (float)N;
                            float logT = nearD * powf(farD / nearD, r);
                            float linT = nearD + (farD - nearD) * r;
                            fp[i] = lambda * logT + (1.0f - lambda) * linT;
                        }
                    }

                    if (firstTime)
                        Log("PssmDetour: splits AFTER =[%g, %g, %g, %g, %g]",
                            (double)fp[0], (double)fp[1], (double)fp[2],
                            (double)fp[3], (double)fp[4]);
                }
            }
            else
            {
                Log("PssmDetour: orchestrator frame not found in unwound stack");
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("PssmDetour: source-dump exception (code 0x%08x)",
                (unsigned)GetExceptionCode());
        }
    }

    if (n <= 6)
    {
        Log("PssmDetour: GpuSharedParameters::getFloatPointer #%d self=%p pos=%zu",
            n, self, pos);
        void* frames[20] = {0};
        USHORT cnt = RtlCaptureStackBackTrace(0, 20, frames, nullptr);
        for (USHORT i = 0; i < cnt; i++)
        {
            HMODULE mod = nullptr;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)frames[i], &mod) && mod)
            {
                char path[MAX_PATH];
                GetModuleFileNameA(mod, path, MAX_PATH);
                const char* base = strrchr(path, '\\');
                base = base ? base + 1 : path;
                Log("    [%2u] %p  %s+0x%llx", (unsigned)i, frames[i], base,
                    (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)mod));
            }
        }
    }
    return sOrig_getFloatPointer(self, pos);
}

// ===== GpuSharedParameters::setNamedConstant(string&, const float*, size_t) —
//       this is what Kenshi's shadow code calls to write csmParams etc. into
//       the ShadowSharedParams shared block. Capturing the call stack here
//       leads straight to Kenshi's split-computing function. =====

typedef void (*SetSharedFloatArrFn)(void* self, const void* name,
                                    const float* val, size_t count);
static SetSharedFloatArrFn sOrig_setSharedFloatArr = nullptr;
static bool sStackOnCsmParamsLogged = false;

static void Hook_setSharedFloatArr(void* self, const void* name,
                                   const float* val, size_t count)
{
    const char* nm = DecodeStdString(name);
    if (nm && strcmp(nm, "csmParams") == 0 && !sStackOnCsmParamsLogged)
    {
        sStackOnCsmParamsLogged = true;
        Log("PssmDetour: setNamedConstant(\"csmParams\", count=%zu) — "
            "first call, capturing stack:", count);
        for (size_t i = 0; i < count && i < 16; i++)
            Log("  [%zu] = %.4f", i, (double)val[i]);
        void* frames[24] = {0};
        USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        for (USHORT i = 0; i < n; i++)
        {
            HMODULE mod = nullptr;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)frames[i], &mod) && mod)
            {
                char path[MAX_PATH];
                GetModuleFileNameA(mod, path, MAX_PATH);
                const char* base = strrchr(path, '\\');
                base = base ? base + 1 : path;
                Log("  [%2u] %p  %s+0x%llx", (unsigned)i, frames[i],
                    base, (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)mod));
            }
            else
            {
                Log("  [%2u] %p  (unmapped)", (unsigned)i, frames[i]);
            }
        }
    }
    sOrig_setSharedFloatArr(self, name, val, count);
}

static void Hook_injectRenderWithPass(void* self, void* pass, void* rend, void* camera,
                                      bool firstRenderable, bool casterPass)
{
    int n = ++sCount_injectRenderWithPass;
    if (n <= 8 || (n % 1200) == 0)
    {
        void* passVtable = pass ? *(void**)pass : nullptr;
        const char* passName     = "(?)";
        const char* materialName = "(?)";

        if (pass && sFn_Pass_getName)
        {
            const void* nm = sFn_Pass_getName(pass);
            passName = DecodeStdString(nm);
        }
        if (pass && sFn_Pass_getParent && sFn_Technique_getParent && sFn_Material_getName)
        {
            void* technique = sFn_Pass_getParent(pass);
            if (technique)
            {
                void* material = sFn_Technique_getParent(technique);
                if (material)
                {
                    const void* nm = sFn_Material_getName(material);
                    materialName = DecodeStdString(nm);
                }
            }
        }

        Log("PssmDetour: _injectRenderWithPass #%d pass=%p (vt=%p) "
            "name=\"%s\" material=\"%s\" caster=%d",
            n, pass, passVtable, passName, materialName, (int)casterPass);
    }
    sOrig_injectRenderWithPass(self, pass, rend, camera, firstRenderable, casterPass);
}

// ===== install =====

static bool InstallOne(HMODULE m, const char* sym, void* detour, void** original)
{
    void* target = (void*)GetProcAddress(m, sym);
    if (!target)
    {
        Log("PssmDetour: symbol not found: %s", sym);
        return false;
    }
    auto status = KenshiLib::AddHook(target, detour, original);
    if (status != KenshiLib::SUCCESS)
    {
        Log("PssmDetour: AddHook FAILED for %s (target=%p)", sym, target);
        return false;
    }
    Log("PssmDetour: installed %s @ %p", sym, target);
    return true;
}

bool IsInstalled() { return sInstalled; }

bool TryInstall()
{
    if (sInstalled) return true;

    HMODULE m = GetModuleHandleA("OgreMain_x64.dll");
    if (!m)
    {
        Log("PssmDetour: OgreMain_x64.dll not loaded yet");
        return false;
    }

    InstallOne(m,
        "?calculateSplitPoints@PSSMShadowCameraSetup@Ogre@@QEAAXIMMM@Z",
        (void*)&Hook_calculateSplitPoints,
        (void**)&sOrig_calculateSplitPoints);

    InstallOne(m,
        "?setSplitPoints@PSSMShadowCameraSetup@Ogre@@QEAAXAEBV?$vector@MV?$STLAllocator@MV?$CategorisedAllocPolicy@$0A@@Ogre@@@Ogre@@@std@@@Z",
        (void*)&Hook_setSplitPoints,
        (void**)&sOrig_setSplitPoints);

    InstallOne(m,
        "?getShadowCamera@DefaultShadowCameraSetup@Ogre@@UEBAXPEBVSceneManager@2@PEBVCamera@2@PEBVLight@2@PEAV42@_K@Z",
        (void*)&Hook_getShadowCamera_Default,
        (void**)&sOrig_getShadowCamera_Default);

    InstallOne(m,
        "?getShadowCamera@FocusedShadowCameraSetup@Ogre@@UEBAXPEBVSceneManager@2@PEBVCamera@2@PEBVLight@2@PEAV42@_K@Z",
        (void*)&Hook_getShadowCamera_Focused,
        (void**)&sOrig_getShadowCamera_Focused);

    InstallOne(m,
        "?getShadowCamera@PSSMShadowCameraSetup@Ogre@@UEBAXPEBVSceneManager@2@PEBVCamera@2@PEBVLight@2@PEAV42@_K@Z",
        (void*)&Hook_getShadowCamera_PSSM,
        (void**)&sOrig_getShadowCamera_PSSM);

    InstallOne(m,
        "?getShadowCamera@PlaneOptimalShadowCameraSetup@Ogre@@UEBAXPEBVSceneManager@2@PEBVCamera@2@PEBVLight@2@PEAV42@_K@Z",
        (void*)&Hook_getShadowCamera_PlaneOptimal,
        (void**)&sOrig_getShadowCamera_PlaneOptimal);

    // Base class: fires for any subclass derived from OGRE's ShadowCameraSetup.
    InstallOne(m,
        "??0ShadowCameraSetup@Ogre@@QEAA@XZ",
        (void*)&Hook_ShadowCameraSetup_Ctor,
        (void**)&sOrig_ShadowCameraSetup_Ctor);

    InstallOne(m,
        "??1ShadowCameraSetup@Ogre@@UEAA@XZ",
        (void*)&Hook_ShadowCameraSetup_Dtor,
        (void**)&sOrig_ShadowCameraSetup_Dtor);

    // CompositorShadowNode: ctor + per-frame _update. If _update fires, Kenshi
    // does use OGRE's compositor shadow path; if not, shadows are entirely
    // outside OGRE and we need a different angle altogether.
    InstallOne(m,
        "??0CompositorShadowNode@Ogre@@QEAA@IPEBVCompositorShadowNodeDef@1@PEAVCompositorWorkspace@1@PEAVRenderSystem@1@PEBVRenderTarget@1@@Z",
        (void*)&Hook_CSN_Ctor,
        (void**)&sOrig_CSN_Ctor);

    InstallOne(m,
        "?_update@CompositorShadowNode@Ogre@@QEAAXPEAVCamera@2@PEBV32@PEAVSceneManager@2@@Z",
        (void*)&Hook_CSN_Update,
        (void**)&sOrig_CSN_Update);

    InstallOne(m,
        "?_injectRenderWithPass@SceneManager@Ogre@@UEAAXPEAVPass@2@PEAVRenderable@2@PEAVCamera@2@_N3@Z",
        (void*)&Hook_injectRenderWithPass,
        (void**)&sOrig_injectRenderWithPass);

    InstallOne(m,
        "?setNamedConstant@GpuSharedParameters@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@PEBM_K@Z",
        (void*)&Hook_setSharedFloatArr,
        (void**)&sOrig_setSharedFloatArr);

    // Non-const getFloatPointer — the raw-write fast path most likely used by
    // Kenshi's shadow code.
    InstallOne(m,
        "?getFloatPointer@GpuSharedParameters@Ogre@@QEAAPEAM_K@Z",
        (void*)&Hook_getFloatPointer,
        (void**)&sOrig_getFloatPointer);

    // Kenshi orchestrator function entry — captures `this` so we can inspect
    // the splits source struct. Address resolved by RVA in the loaded EXE.
    HMODULE kenshiMod = GetModuleHandleA("Kenshi_x64.exe");
    if (kenshiMod)
    {
        void* orchAddr = (char*)kenshiMod + 0x8629a0;
        auto status = KenshiLib::AddHook(orchAddr,
            (void*)&Hook_orchestrator_entry, (void**)&sOrig_orchestrator);
        if (status == KenshiLib::SUCCESS)
            Log("PssmDetour: installed Kenshi+0x8629a0 (orchestrator entry) @ %p",
                orchAddr);
        else
            Log("PssmDetour: AddHook FAILED for orchestrator @ %p", orchAddr);
    }
    else
    {
        Log("PssmDetour: Kenshi_x64.exe not loaded yet at TryInstall");
    }

    // Name-resolution helpers used inside Hook_injectRenderWithPass.
    sFn_Pass_getName = (GetNameFn)GetProcAddress(m,
        "?getName@Pass@Ogre@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ");
    sFn_Pass_getParent = (GetParentFn)GetProcAddress(m,
        "?getParent@Pass@Ogre@@QEBAPEAVTechnique@2@XZ");
    sFn_Technique_getParent = (GetParentFn)GetProcAddress(m,
        "?getParent@Technique@Ogre@@QEBAPEAVMaterial@2@XZ");
    // Material::getName isn't directly exported — Material inherits Resource::getName.
    sFn_Material_getName = (GetNameFn)GetProcAddress(m,
        "?getName@Resource@Ogre@@UEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ");
    Log("PssmDetour: name resolvers Pass::getName=%p Pass::getParent=%p "
        "Technique::getParent=%p Resource::getName=%p",
        sFn_Pass_getName, sFn_Pass_getParent, sFn_Technique_getParent, sFn_Material_getName);

    sInstalled = true;
    return true;
}

}
