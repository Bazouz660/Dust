// DustSSAODebug — companion plugin that draws the SSAO diagnostic overlay
// onto the final LDR target at PRE_PRESENT.
//
// The main DustSSAO plugin sits at POST_LIGHTING (required to bind slot 8
// before deferred lighting reads it). Its overlay would be overdrawn by
// fog / tonemap / etc., so this companion defers the overlay until
// PRE_PRESENT and calls back into DustSSAO via a DLL export.

#include "../../src/DustAPI.h"
#include <d3d11.h>
#include <windows.h>
#include <cstring>

typedef void (*PFN_RenderSSAODebugOverlay)(
    ID3D11DeviceContext*, ID3D11RenderTargetView*, ID3D11ShaderResourceView*);

static PFN_RenderSSAODebugOverlay gRenderFn = nullptr;
static const DustHostAPI*         gHost     = nullptr;

static int Init(ID3D11Device* device, uint32_t w, uint32_t h, const DustHostAPI* host)
{
    gHost = host;
    HMODULE m = GetModuleHandleA("DustSSAO.dll");
    if (!m)
    {
        host->Log("[SSAODebug] DustSSAO.dll not loaded — overlay disabled");
        return 0;
    }
    gRenderFn = (PFN_RenderSSAODebugOverlay)GetProcAddress(m, "Dust_RenderSSAODebugOverlay");
    if (!gRenderFn)
    {
        host->Log("[SSAODebug] Dust_RenderSSAODebugOverlay export not found in DustSSAO.dll");
        return 0;
    }
    host->Log("[SSAODebug] Linked to DustSSAO.dll");
    return 0;
}

static void Shutdown(void) { gRenderFn = nullptr; gHost = nullptr; }

static int IsEnabled(void) { return 1; }

static void PostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gRenderFn) return;
    ID3D11RenderTargetView*   rtv   = host->GetRTV(DUST_RESOURCE_LDR_RT);
    ID3D11ShaderResourceView* depth = host->GetSRV(DUST_RESOURCE_DEPTH);
    if (!rtv || !depth) return;
    gRenderFn(ctx->context, rtv, depth);
}

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;
    memset(desc, 0, sizeof(*desc));
    desc->apiVersion     = DUST_API_VERSION;
    desc->name           = "Ambient Occlusion Debug Overlay";
    desc->configSection  = "SSAODebugOverlay";   // keep identity stable after the display-name rename
    // Note: PRE_PRESENT is defined in the enum but PipelineDetector never
    // classifies any draw to it — so plugins there are never dispatched.
    // POST_TONEMAP is the last actually-dispatched point; LDR target is final.
    desc->injectionPoint = DUST_INJECT_POST_TONEMAP;
    desc->priority       = 1000;   // run after FilmGrain (210) etc. so overlay sits on top
    desc->Init           = Init;
    desc->Shutdown       = Shutdown;
    desc->postExecute    = PostExecute;
    desc->IsEnabled      = IsEnabled;
    desc->flags          = DUST_FLAG_FRAMEWORK_CONFIG;
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hModule);
    return TRUE;
}
