#include "PssmDetour.h"
#include "DustLog.h"

#include <core/Functions.h>  // KenshiLib::AddHook
#include <windows.h>
#include <atomic>
#include <cmath>
#include <limits>

namespace PssmDetour
{

// Live PSSM cascade-split lambda override (backported from v0.8.0, lambda
// path only — shadow range and per-cascade filter scales stayed behind).
//
// Kenshi's hand-rolled shadow orchestrator (Kenshi+0x8629a0 region) computes
// the CSM split distances at scene init and stores them in a 5-float
// [near, s1, s2, s3, far] array in its own struct (field_0x10), then pushes
// derived values into OGRE's GpuSharedParameters via the raw
// getFloatPointer(pos=24) fast path every frame. Hooking calculateSplitPoints
// never fires — so instead we hook getFloatPointer, and on a pos==24 call
// unwind the call stack to the orchestrator frame, recover its `this` from
// RDI, and cache the splits array. Lambda changes then recompute splits[1..3]
// in place; Kenshi's own per-frame csmParams writes propagate them.
//
// The unwind only runs when the cache is missing or stale: the pos==24 call
// fires several times per frame in CSM mode, and on the per-frame call path
// the reconstructed RDI is garbage (the frame doesn't restore it), so
// re-running the capture every call meant thousands of caught AVs per minute
// — enough exception + log traffic to visibly destabilize the framerate.
//
// Kenshi's native splits do NOT fit the standard PSSM formula at any single
// lambda (they're tighter than pure-log at the far end), so the override is
// opt-in: until SetLambda is called with a value >= 0, the native splits are
// left untouched, and SetLambda(<0) restores the captured native values.

static bool sInstalled = false;

typedef float* (*GetFloatPointerFn)(void* self, size_t pos);
static GetFloatPointerFn sOrig_getFloatPointer = nullptr;

// Runtime-controllable PSSM lambda. Only applied while sLambdaActive.
static std::atomic<float> sLambda{0.95f};
static std::atomic<bool>  sLambdaActive{false};

// State captured from the orchestrator frame. sOrigSplits/sOrigFar keep
// Kenshi's own values so clearing the overrides restores them exactly.
static void*  sSharedParams = nullptr;  // GpuSharedParameters instance (CSM)
static float* sSplitsArray  = nullptr;  // orchestrator-this->field_0x10
static float  sCachedNear   = 0.0f;
static float  sCachedFar    = 0.0f;
static float  sOrigSplits[3] = {};
static float  sOrigFar      = 0.0f;

// RTWSM shared params: shadow_range is at float[0] of this block. Captured
// from the first getFloatPointer(pos=5) call; Kenshi rewrites the block every
// frame, so a pending override is re-applied on each pos==5 call.
static void*  sRtwsmSharedParams = nullptr;
static float* sRtwsmFloatsBase   = nullptr;
static float  sOrigRtwRange      = 0.0f;   // shadow_range at capture (for revert)

// User-requested shadow far. NaN means "no override" — all paths leave
// Kenshi's native value alone.
static std::atomic<float> sRequestedFar{std::numeric_limits<float>::quiet_NaN()};

// Last values we wrote into the splits array while an override was active.
// Kenshi re-derives the splits on a shadow-mode switch / scene re-init and
// stomps our override; the per-frame pos==24 hook compares the array against
// these (exact bits — anything Kenshi writes differs) and re-applies.
static float sAppliedSplits[3] = {};
static float sAppliedNear = 0.0f;
static float sAppliedFar  = 0.0f;
static bool  sAppliedValid = false;

// GpuSharedParameters destructor hook: deterministic staleness signal. OGRE
// destroys and recreates the shared-params blocks when the user switches
// shadow type in the options; without this, the cached RTWSM float pointer
// dangled and the per-frame range re-apply wrote freed memory. (A read probe
// can't catch this case — freed heap is usually still readable.)
typedef void (*GSPDestructorFn)(void* self);
static GSPDestructorFn sOrig_GSP_Destructor = nullptr;

// Getter intercepts: override the shadow far distance for any OGRE code that
// queries it (shadow camera setup, auto-constants, etc.). Self-compare is the
// NaN check.
typedef float (*GetShadowFarDistFn)(const void* self);
static GetShadowFarDistFn sOrig_SM_getShadowFarDist = nullptr;
static GetShadowFarDistFn sOrig_Light_getShadowFarDist = nullptr;
static GetShadowFarDistFn sOrig_SM_getShadowFarDistSq = nullptr;
static GetShadowFarDistFn sOrig_Light_getShadowFarDistSq = nullptr;

static float Hook_SM_getShadowFarDist(const void* self)
{
    float req = sRequestedFar.load();
    return (req == req) ? req : sOrig_SM_getShadowFarDist(self);
}

static float Hook_SM_getShadowFarDistSq(const void* self)
{
    float req = sRequestedFar.load();
    return (req == req) ? req * req : sOrig_SM_getShadowFarDistSq(self);
}

static float Hook_Light_getShadowFarDist(const void* self)
{
    float req = sRequestedFar.load();
    float orig = sOrig_Light_getShadowFarDist(self);
    return (req == req && orig > 0.0f) ? req : orig;
}

static float Hook_Light_getShadowFarDistSq(const void* self)
{
    float req = sRequestedFar.load();
    float orig = sOrig_Light_getShadowFarDistSq(self);
    return (req == req && orig > 0.0f) ? req * req : orig;
}

// Compute and write splits[1..3] into the 5-float [near, s1, s2, s3, far]
// array using the current lambda + cached near/far. Standard PSSM formula
// (OGRE's calculateSplitPoints):
//   split[i] = lambda * near*(far/near)^(i/N) + (1-lambda) * (near + (far-near)*i/N)
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
        sAppliedSplits[i - 1] = sSplitsArray[i];
    }
    // Record what we applied so the per-frame pos==24 check can detect
    // Kenshi stomping the array (shadow-mode switch re-derives the splits).
    sAppliedNear  = n;
    sAppliedFar   = f;
    sAppliedValid = true;
    return true;
}

// Push the current splits array to OGRE's GpuSharedParameters (csmParams[i].x)
// so a runtime change lands without waiting for Kenshi's next write.
static void PushSplitsToSharedParams()
{
    if (!sSharedParams || !sOrig_getFloatPointer || !sSplitsArray) return;
    float* dest = sOrig_getFloatPointer(sSharedParams, 24);
    if (!dest) return;
    for (int i = 0; i < 4; i++)
        dest[i*4 + 0] = sSplitsArray[i+1] - sCachedNear;
}

static bool ProbeCachedSplits();  // SEH-guarded liveness check, defined below

// Drop the cached splits pointer if it no longer reads as plausible — the
// orchestrator's array can be freed on a scene/shadow rebuild between the
// per-frame hook validations, and SetLambda/SetShadowFar write it directly.
static void ValidateSplitsCache()
{
    if (sSplitsArray && !ProbeCachedSplits())
        sSplitsArray = nullptr;  // pending overrides re-apply at next capture
}

void SetLambda(float lambda)
{
    // Reject non-finite input up front: NaN passes every < / > clamp below and would be
    // written straight into the engine's shadow state (a hand-edited preset can carry `nan`).
    if (!std::isfinite(lambda)) return;
    ValidateSplitsCache();
    if (lambda < 0.0f)
    {
        // Clear the override: restore Kenshi's own splits.
        if (!sLambdaActive.load()) return;
        sLambdaActive.store(false);
        sAppliedValid = false;
        if (sSplitsArray)
        {
            for (int i = 0; i < 3; i++)
                sSplitsArray[i + 1] = sOrigSplits[i];
            PushSplitsToSharedParams();
        }
        return;
    }
    if (lambda > 1.0f) lambda = 1.0f;
    sLambda.store(lambda);
    sLambdaActive.store(true);
    if (RecomputeSplits())
        PushSplitsToSharedParams();
}

float GetLambda() { return sLambda.load(); }

bool SetShadowFar(float farDistance)
{
    // Reject non-finite input up front: NaN passes every < / > clamp below and NaN is also this
    // module's own "no override" sentinel for sRequestedFar — never let it in as a real value.
    if (!std::isfinite(farDistance)) return false;
    ValidateSplitsCache();
    if (farDistance < 0.0f)
    {
        // Clear the override: restore Kenshi's own values.
        float prev = sRequestedFar.load();
        bool hadOverride = (prev == prev);  // non-NaN
        sRequestedFar.store(std::numeric_limits<float>::quiet_NaN());
        if (!hadOverride) return sSplitsArray != nullptr;
        if (sRtwsmFloatsBase && sOrigRtwRange > 0.0f)
            sRtwsmFloatsBase[0] = sOrigRtwRange;
        if (!sSplitsArray || !(sOrigFar > 0.0f)) return sRtwsmFloatsBase != nullptr;
        sSplitsArray[4] = sOrigFar;
        sCachedFar      = sOrigFar;
        if (!sLambdaActive.load())
        {
            sAppliedValid = false;
            for (int i = 0; i < 3; i++)
                sSplitsArray[i + 1] = sOrigSplits[i];
        }
        else
        {
            RecomputeSplits();
        }
        PushSplitsToSharedParams();
        return true;
    }

    if (farDistance < 1.0f)       farDistance = 1.0f;
    if (farDistance > 100000.0f)  farDistance = 100000.0f;
    sRequestedFar.store(farDistance);

    // RTWSM: write shadow range directly to the shared params float array.
    // The per-frame getFloatPointer(pos=5) hook also re-applies a pending
    // override, but this immediate write covers the case where the slider
    // changes between two getFloatPointer calls.
    if (sRtwsmFloatsBase)
        sRtwsmFloatsBase[0] = farDistance;

    if (!sSplitsArray || !(sCachedNear > 0.0f)) return sRtwsmFloatsBase != nullptr;
    float minFar = sCachedNear + 1.0f;
    if (farDistance < minFar) farDistance = minFar;

    // A changed far invalidates the native splits — recompute with the PSSM
    // formula at the current lambda.
    sSplitsArray[4] = farDistance;
    sCachedFar      = farDistance;
    RecomputeSplits();
    PushSplitsToSharedParams();
    return true;
}

float GetShadowFar() { return sCachedFar; }

// Staleness probe + capture, both SEH-guarded (the recovered RDI can be
// garbage on the per-frame call path, and a scene rebuild can free the
// cached splits array). Kept free of C++ objects for __try.
static bool ProbeCachedSplits()
{
    __try
    {
        float n = sSplitsArray[0], f = sSplitsArray[4];
        return (n > 0.0f) && (f > n);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void TryCaptureSplitsSource(void* self)
{
    static int sExceptionLogs = 0;
    static bool sSourceLogged = false;
    bool firstTime = !sSourceLogged;
    sSourceLogged = true;
    __try
    {
        HMODULE kenshiMod = GetModuleHandleA("Kenshi_x64.exe");
        uintptr_t kenshiBase = (uintptr_t)kenshiMod;

        CONTEXT ctx;
        RtlCaptureContext(&ctx);

        // Unwind up to ~16 frames looking for one whose RIP is in the
        // orchestrator's address range (contains the call site at
        // +0x862c34..+0x864221).
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
            if (kenshiRva >= 0x862000 && kenshiRva <= 0x866000)
            {
                thisPtr = (void*)ctx.Rdi;
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
                float nearD = fp[0];
                float farD  = fp[4];

                // Validate BEFORE caching or writing — on the per-frame call
                // path the recovered RDI is not the orchestrator this and
                // these reads are garbage (or fault, caught below).
                if (nearD > 0.0f && farD > nearD && farD < 1.0e9f)
                {
                    sSharedParams = self;
                    sSplitsArray  = fp;
                    sCachedNear   = nearD;
                    sCachedFar    = farD;

                    // Record "native" values only when no override can have
                    // contaminated them: with a far override active Kenshi
                    // derives its splits THROUGH our getter intercepts (so
                    // farD here IS the override), and with a lambda override
                    // active the array may still hold our formula splits. On
                    // a re-capture (shadow-mode switch) keep the clean
                    // boot-time originals so clearing the override later
                    // restores the true values.
                    float pendingFar = sRequestedFar.load();
                    bool overrideActive = sLambdaActive.load() ||
                                          (pendingFar == pendingFar);
                    if (!overrideActive || !(sOrigFar > 0.0f))
                    {
                        sOrigFar = farD;
                        for (int i = 0; i < 3; i++)
                            sOrigSplits[i] = fp[i + 1];
                    }

                    if (firstTime)
                        Log("PssmDetour: splits captured=[%g, %g, %g, %g, %g] "
                            "(override %s, lambda=%g)",
                            (double)fp[0], (double)fp[1], (double)fp[2],
                            (double)fp[3], (double)fp[4],
                            sLambdaActive.load() ? "active" : "inactive",
                            (double)sLambda.load());

                    // Apply a shadow-far override the plugin may have pushed
                    // before this capture happened (e.g. ShadowInit ran
                    // before Kenshi's scene init).
                    bool farOverridden = false;
                    float pending = sRequestedFar.load();
                    if (pending == pending && pending > nearD + 1.0f)
                    {
                        fp[4]      = pending;
                        sCachedFar = pending;
                        farOverridden = true;
                    }

                    if (sLambdaActive.load() || farOverridden)
                        RecomputeSplits();
                }
            }
        }
        else if (firstTime)
        {
            Log("PssmDetour: orchestrator frame not found in unwound stack");
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (sExceptionLogs < 3)
        {
            ++sExceptionLogs;
            Log("PssmDetour: splits-capture exception (code 0x%08x)%s",
                (unsigned)GetExceptionCode(),
                sExceptionLogs == 3 ? " — further occurrences not logged" : "");
        }
    }
}

// Re-assert active lambda/far overrides against the splits array. Kenshi
// re-derives the splits on a shadow-mode switch (RTWSM<->CSM) or scene
// re-init and stomps whatever we wrote; nothing else re-applies (SetLambda /
// SetShadowFar only run on GUI changes). Runs on every validated pos==24
// call, but is exact-bit compares only in steady state — the recompute fires
// solely when the array no longer holds what we last applied.
static void EnforceSplitOverrides()
{
    float pending = sRequestedFar.load();
    bool farActive = (pending == pending);
    if (!farActive && !sLambdaActive.load())
    {
        sAppliedValid = false;
        return;
    }

    // Adopt Kenshi's current near; adopt its far too unless we override it.
    float curNear = sSplitsArray[0];
    float curFar  = sSplitsArray[4];
    if (!(curNear > 0.0f) || !(curFar > curNear)) return;
    float wantFar = curFar;
    if (farActive)
    {
        wantFar = pending;
        float minFar = curNear + 1.0f;
        if (wantFar < minFar) wantFar = minFar;
    }

    bool stomped = !sAppliedValid ||
                   curNear         != sAppliedNear      ||
                   curFar          != sAppliedFar       ||
                   sSplitsArray[1] != sAppliedSplits[0] ||
                   sSplitsArray[2] != sAppliedSplits[1] ||
                   sSplitsArray[3] != sAppliedSplits[2];
    if (!stomped) return;

    sCachedNear     = curNear;
    sCachedFar      = wantFar;
    sSplitsArray[4] = wantFar;
    if (RecomputeSplits())
    {
        PushSplitsToSharedParams();
        // Capped: if an engine path ever rewrites the array per frame this
        // must not become per-frame log traffic (see the r3 AV-spam lesson).
        static int sReapplyLogs = 0;
        if (sReapplyLogs < 8)
        {
            ++sReapplyLogs;
            Log("PssmDetour: splits override re-applied after engine rewrite "
                "(near=%g far=%g lambda=%s)%s",
                (double)curNear, (double)wantFar,
                sLambdaActive.load() ? "active" : "native-formula",
                sReapplyLogs == 8 ? " — further occurrences not logged" : "");
        }
    }
}

// Clears caches when OGRE destroys a shared-params instance we captured —
// the deterministic invalidation for shadow-mode switches. Body must stay
// branch-only: this fires for every GpuSharedParameters destruction.
static void Hook_GSP_Destructor(void* self)
{
    if (self == sRtwsmSharedParams)
    {
        sRtwsmSharedParams = nullptr;
        sRtwsmFloatsBase   = nullptr;
        Log("PssmDetour: RTWSM shared params destroyed — cache cleared");
    }
    if (self == sSharedParams)
    {
        sSharedParams = nullptr;
        sSplitsArray  = nullptr;   // re-captured from the next pos==24 call
        sAppliedValid = false;
        Log("PssmDetour: CSM shared params destroyed — splits cache cleared");
    }
    sOrig_GSP_Destructor(self);
}

static float* Hook_getFloatPointer(void* self, size_t pos)
{
    // pos==5 is the RTWSM shared params (shadow_range at float[0]). Kenshi
    // rewrites the block every frame, so re-apply a pending far override on
    // each call. Capture rules mirror pos==24: first plausible caller wins,
    // a different instance is ignored while the capture is live, and the
    // destructor hook clears the capture when the block is destroyed (mode
    // switch) so the next RTWSM block re-captures cleanly.
    if (pos == 5)
    {
        if (!sRtwsmSharedParams)
        {
            // Never adopt the CSM block, and sanity-check that float[0]
            // looks like a shadow range before trusting a first caller.
            float* base = (self != sSharedParams)
                        ? sOrig_getFloatPointer(self, 0) : nullptr;
            if (base && base[0] > 0.0f && base[0] < 1.0e6f)
            {
                sRtwsmSharedParams = self;
                sRtwsmFloatsBase   = base;
                sOrigRtwRange      = base[0];
                Log("PssmDetour: RTWSM shared params captured, shadow_range=%g",
                    (double)sOrigRtwRange);
            }
        }
        else if (self == sRtwsmSharedParams)
        {
            // Refresh the base pointer — the float array can reallocate
            // while the instance survives (addConstantDefinition).
            sRtwsmFloatsBase = sOrig_getFloatPointer(self, 0);
        }
        if (sRtwsmFloatsBase)
        {
            float pending = sRequestedFar.load();
            if (pending == pending && pending >= 1.0f)
                sRtwsmFloatsBase[0] = pending;
        }
    }

    // pos==24 is the csmParams block. getFloatPointer is a HOT OGRE function
    // (called for many shared-params blocks, several times per frame), so the
    // stack-unwind capture must run at most ONCE per capture epoch — never
    // per frame. Rules:
    //   - No valid capture yet  -> attempt capture from this call.
    //   - Same instance we captured from -> verify the pointer is still live
    //     (two guarded reads); if it went stale (scene rebuild), drop it so
    //     the next call re-captures. While live, re-assert any active
    //     overrides the engine may have stomped (mode switch re-derivation).
    //   - A DIFFERENT instance while we already hold a valid capture -> ignore
    //     entirely. Unwinding here every frame for an unrelated shared-params
    //     block was a periodic frame-time spike.
    if (pos == 24)
    {
        if (!sSplitsArray)
            TryCaptureSplitsSource(self);
        else if (self == sSharedParams)
        {
            if (!ProbeCachedSplits())
                sSplitsArray = nullptr;  // stale; re-capture on a later call
            else
                EnforceSplitOverrides();
        }
    }

    return sOrig_getFloatPointer(self, pos);
}

bool IsInstalled() { return sInstalled; }

static bool InstallOne(HMODULE m, const char* sym, void* detour, void** original)
{
    void* target = (void*)GetProcAddress(m, sym);
    if (!target)
    {
        Log("PssmDetour: symbol not found: %s", sym);
        return false;
    }
    if (KenshiLib::AddHook(target, detour, original) != KenshiLib::SUCCESS)
    {
        Log("PssmDetour: AddHook FAILED for %s (target=%p)", sym, target);
        return false;
    }
    Log("PssmDetour: installed %s @ %p", sym, target);
    return true;
}

bool TryInstall()
{
    if (sInstalled) return true;

    HMODULE m = GetModuleHandleA("OgreMain_x64.dll");
    if (!m)
    {
        Log("PssmDetour: OgreMain_x64.dll not loaded yet");
        return false;
    }

    // Non-const getFloatPointer — the raw-write fast path Kenshi's shadow
    // code uses for csmParams (pos 24) and the RTWSM shared params (pos 5).
    if (!InstallOne(m,
        "?getFloatPointer@GpuSharedParameters@Ogre@@QEAAPEAM_K@Z",
        (void*)&Hook_getFloatPointer,
        (void**)&sOrig_getFloatPointer))
        return false;

    // GpuSharedParameters destructor — deterministic invalidation of the
    // captured RTWSM/CSM blocks when OGRE rebuilds them (shadow-mode switch).
    // Non-fatal if missing: the read-probe staleness paths still apply.
    InstallOne(m,
        "??1GpuSharedParameters@Ogre@@UEAA@XZ",
        (void*)&Hook_GSP_Destructor,
        (void**)&sOrig_GSP_Destructor);

    // Far-distance getter intercepts — override any OGRE code reading the
    // shadow far distance (shadow camera setup, auto-constants). Rare calls
    // (per-frame camera setup), not per-draw.
    InstallOne(m,
        "?getShadowFarDistance@SceneManager@Ogre@@UEBAMXZ",
        (void*)&Hook_SM_getShadowFarDist,
        (void**)&sOrig_SM_getShadowFarDist);
    InstallOne(m,
        "?getShadowFarDistanceSquared@SceneManager@Ogre@@UEBAMXZ",
        (void*)&Hook_SM_getShadowFarDistSq,
        (void**)&sOrig_SM_getShadowFarDistSq);
    InstallOne(m,
        "?getShadowFarDistance@Light@Ogre@@QEBAMXZ",
        (void*)&Hook_Light_getShadowFarDist,
        (void**)&sOrig_Light_getShadowFarDist);
    InstallOne(m,
        "?getShadowFarDistanceSquared@Light@Ogre@@QEBAMXZ",
        (void*)&Hook_Light_getShadowFarDistSq,
        (void**)&sOrig_Light_getShadowFarDistSq);

    sInstalled = true;
    return true;
}

}
