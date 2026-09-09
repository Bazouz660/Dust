#include "../src/DustAPI.h"
#include "../src/ShadowCasterBias.h"
#include <wrl/client.h>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;
static ID3D11Device* gDevice;
static ID3D11DeviceContext* gContext;
static uint64_t gFrameIndex = 0, gVpSetSerial = 0;
static UINT gShadowAtlasOverride = 0;
static constexpr size_t kMaxShadowIdentities = 8;
static bool gShutdownSignaled = false, gFwdScenePass = false, sMvFeederActive = false;
static bool gJitterEnabled = false, gJitterForwardPasses = false;
static float gJitterPxX = 0, gJitterPxY = 0;
static int tSuppressVpJitter = 0;
static const DustHostAPI* gHost = nullptr;
static uint32_t GetSelectedShadowResolution() { return gShadowAtlasOverride; }
static bool IsPowerOf2(UINT value) { return value && !(value & (value - 1)); }
static void Log(const char*, ...) {}
namespace MotionVectors {
    static void InjNoteRenderTargetsRebound() {}
    static void InjBeginGBuffer(ID3D11DeviceContext*) {}
    static ID3D11RenderTargetView* InjEnsureVelRTV(ID3D11RenderTargetView*) { return nullptr; }
    static ID3D11Buffer* GetInjReprojCB() { return nullptr; }
}
namespace GeometryCapture {
    static bool CheckGBufferConfig(UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*) { return false; }
    static void OnOMSetRenderTargetsWithResult(bool) {}
    static ID3D11DepthStencilView* GetGBufferDSV() { return nullptr; }
    static UINT GetCaptureFlags() { return 0; }
    static bool IsInGBufferPass() { return false; }
}

static bool failDepth = false;
static HRESULT oCreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** texture)
{
    if (failDepth && (desc->BindFlags & D3D11_BIND_DEPTH_STENCIL))
        { *texture = nullptr; return E_OUTOFMEMORY; }
    return device->CreateTexture2D(desc, data, texture);
}
static void oOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv)
    { ctx->OMSetRenderTargets(n, rtv, dsv); }
static void oOMSetRenderTargetsAndUAV(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtv,
    ID3D11DepthStencilView* dsv, UINT start, UINT count, ID3D11UnorderedAccessView* const* uav, const UINT* initial)
    { ctx->OMSetRenderTargetsAndUnorderedAccessViews(n, rtv, dsv, start, count, uav, initial); }
static void oPSSetShaderResources(ID3D11DeviceContext* ctx, UINT start, UINT count, ID3D11ShaderResourceView* const* srv)
    { ctx->PSSetShaderResources(start, count, srv); }
static void oRSSetViewports(ID3D11DeviceContext* ctx, UINT count, const D3D11_VIEWPORT* vp)
    { ctx->RSSetViewports(count, vp); }
static void oClearState(ID3D11DeviceContext* ctx) { ctx->ClearState(); }

#include "build/ShadowAtlasHooks.generated.h"

struct Atlas {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_TEXTURE2D_DESC desc = {};
    Atlas(UINT size, bool depth, bool sampleable = true)
    {
        desc.Width = desc.Height = size;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = depth ? (sampleable ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT) : DXGI_FORMAT_R32_FLOAT;
        desc.BindFlags = (depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET) |
            (sampleable ? D3D11_BIND_SHADER_RESOURCE : 0);
        assert(SUCCEEDED(gDevice->CreateTexture2D(&desc, nullptr, &texture)));
        if (depth)
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC view = {};
            view.Format = DXGI_FORMAT_D32_FLOAT; view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            assert(SUCCEEDED(gDevice->CreateDepthStencilView(texture.Get(), &view, &dsv)));
        }
        else assert(SUCCEEDED(gDevice->CreateRenderTargetView(texture.Get(), nullptr, &rtv)));
        if (sampleable)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC view = {};
            view.Format = DXGI_FORMAT_R32_FLOAT; view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
            assert(SUCCEEDED(gDevice->CreateShaderResourceView(texture.Get(), &view, &srv)));
        }
    }
};

static void NextFrame()
{
    HookedClearState(gContext);
    ++gFrameIndex;
    ApplyPendingShadowResize();
    ClearShadowReplacements();
}
static void Caster(Atlas* color, Atlas& depth, bool combined)
{
    HookedClearState(gContext);
    assert(!gInShadowPass && gShadowPassScale == 1);
    D3D11_VIEWPORT vp = {0, 0, (float)depth.desc.Width, (float)depth.desc.Height, 0, 1};
    HookedRSSetViewports(gContext, 1, &vp); // OGRE's viewport-before-OM ordering
    ID3D11RenderTargetView* rtv = color ? color->rtv.Get() : nullptr;
    if (combined)
        HookedOMSetRenderTargetsAndUAV(gContext, color ? 1 : 0, color ? &rtv : nullptr,
            depth.dsv.Get(), color ? 1 : 0, 0, nullptr, nullptr);
    else HookedOMSetRenderTargets(gContext, color ? 1 : 0, color ? &rtv : nullptr, depth.dsv.Get());
    const bool wasShadow = gInShadowPass;
    const float previousScale = gShadowPassScale;
    HookedOMSetRenderTargetsAndUAV(gContext, D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
        nullptr, nullptr, 0, 0, nullptr, nullptr);
    assert(gInShadowPass == wasShadow && gShadowPassScale == previousScale);
    int index = FindShadowEntry(depth.texture.Get());
    if (index >= 0)
    {
        ComPtr<ID3D11DepthStencilView> bound;
        gContext->OMGetRenderTargets(0, nullptr, &bound);
        bool replaced = bound.Get() == gShadowEntries[index].newDSV;
        UINT count = 1;
        gContext->RSGetViewports(&count, &vp);
        assert(vp.Width == (replaced ? gShadowEntries[index].newSize : depth.desc.Width));
    }
}
static UINT Lighting(Atlas& atlas)
{
    HookedClearState(gContext);
    HookedPSSetShaderResources(gContext, 5, 1, atlas.srv.GetAddressOf());
    ObserveLightingShadowAtlas(gContext);
    return GetEffectiveShadowResolution(gContext);
}

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    gDevice = device.Get(); gContext = context.Get();
    Atlas csm(2048, true), rtwColor(1024, false), rtwDepth(1024, true);
    SetShadowAtlasResolution(4096);
    // Depth-only pooled CSM is not adopted by a random OM bind. Its confirmed
    // lighting sample adopts it, with the original atlas used on that frame.
    Caster(nullptr, csm, false);
    assert(gShadowEntryCount == 0);
    assert(Lighting(csm) == 2048);
    assert(gShadowEntryCount == 1);
    NextFrame();
    Caster(nullptr, csm, true);
    assert(Lighting(csm) == 4096);
    assert(GetShadowBaseResolution() == 2048);

    Caster(&rtwColor, rtwDepth, false);
    assert(gShadowEntryCount == 3);
    assert(Lighting(rtwColor) == 1024); // queued replacement is not rendered yet
    NextFrame();
    for (int i = 0; i < 12; ++i)
    {
        Caster(&rtwColor, rtwDepth, (i & 1) != 0);
        assert(Lighting(rtwColor) == 4096);
        assert(GetShadowBaseResolution() == 1024);
        Caster(nullptr, csm, (i & 1) == 0);
        assert(Lighting(csm) == 4096);
        assert(GetShadowBaseResolution() == 2048);
        NextFrame();
    }
    // Non-power-of-two replacements must resolve back to the native identity.
    SetShadowAtlasResolution(3072);
    NextFrame();
    Caster(&rtwColor, rtwDepth, false);
    assert(Lighting(rtwColor) == 3072 && GetShadowBaseResolution() == 1024);
    Caster(nullptr, csm, true);
    assert(Lighting(csm) == 3072 && GetShadowBaseResolution() == 2048);

    ComPtr<ID3D11DeviceContext> otherContext;
    assert(SUCCEEDED(device->CreateDeferredContext(0, &otherContext)));
    Caster(nullptr, csm, false);
    HookedClearState(otherContext.Get());
    assert(gInShadowPass && gShadowPassScale == 1.5f);
    // A newly allocated color target with failed depth must not be sampled
    // while the all-or-nothing caster bind falls back to native resources.
    SetShadowAtlasResolution(512); // small replacement keeps WARP stress cheap
    failDepth = true;
    NextFrame();
    Caster(&rtwColor, rtwDepth, true);
    assert(Lighting(rtwColor) == 1024);
    failDepth = false;
    SetShadowAtlasResolution(512);
    NextFrame();
    Caster(&rtwColor, rtwDepth, false);
    assert(Lighting(rtwColor) == 512);

    // A foreign DSV needs its color's companion even with an old CSM depth
    // in the table. The first bind falls back, then recovers next frame.
    Atlas foreignDepth(1024, true, false);
    Caster(&rtwColor, foreignDepth, true);
    assert(Lighting(rtwColor) == 1024);
    NextFrame();
    Caster(&rtwColor, foreignDepth, false);
    assert(Lighting(rtwColor) == 512);

    // Rapid generations exceed the fixed table before the idle timeout.
    // Reclaim old generations while retaining the pair used this frame.
    for (int i = 0; i < 12; ++i)
    {
        NextFrame();
        Caster(&rtwColor, rtwDepth, true);
        Atlas next(1024, false);
        assert(AdoptShadowTexture(next.texture.Get(), next.desc) >= 0);
        assert(FindShadowEntry(rtwColor.texture.Get()) >= 0);
        assert(FindShadowEntry(rtwDepth.texture.Get()) >= 0);
        assert(gShadowEntryCount <= kMaxShadowIdentities);
    }
    SetShadowAtlasResolution(0);
    NextFrame();
    Caster(&rtwColor, rtwDepth, false);
    assert(Lighting(rtwColor) == 1024);
    HookedClearState(gContext);
    gShadowCasterBias.Reset();
    for (size_t i = 0; i < gShadowEntryCount; ++i)
    {
        ReleaseShadowReplacement(gShadowEntries[i]);
        gShadowEntries[i].tex->Release();
        gShadowEntries[i] = {};
    }
    gShadowEntryCount = 0;
    std::puts("WARP: repeated CSM/RTW switches, native fallback, pooled depth, companion depth, table pressure: PASS");
}
