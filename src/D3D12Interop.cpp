#include "D3D12Interop.h"
#include "DustLog.h"

#include <d3d11_4.h>   // ID3D11Device5 / ID3D11DeviceContext4 / ID3D11Fence (shared-fence sync)
#include <d3d12.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

// D.0 spike. Raw COM + manual Release to match the rest of the codebase. Every failure logs and leaves
// the module in a not-ready state; RunTintTest then does nothing, so a broken bridge degrades the frame
// to "unchanged image", never a crash.

namespace D3D12Interop
{
namespace
{
    template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

    // --- Persistent D3D12 side-device objects (created once in Init) ---
    ID3D11Device*              gGameDev   = nullptr;   // the game's D3D11 device (AddRef'd)
    ID3D12Device*              gDev       = nullptr;
    ID3D12CommandQueue*        gQueue     = nullptr;
    ID3D12CommandAllocator*    gAlloc     = nullptr;
    ID3D12GraphicsCommandList* gList      = nullptr;
    ID3D12RootSignature*       gRootSig   = nullptr;
    ID3D12PipelineState*       gPso       = nullptr;
    ID3D12DescriptorHeap*      gHeap      = nullptr;   // shader-visible: [0]=SRV(in) [1]=UAV(out)
    UINT                       gDescInc   = 0;

    // Cross-API shared fence: one GPU fence seen as ID3D12Fence on our queue and ID3D11Fence on the game
    // device. Producer/consumer handoff without CPU stalls or a keyed mutex (keyed mutex is unsupported
    // under Proton, which is the whole reason we sync this way).
    ID3D12Fence*               gFence     = nullptr;
    ID3D11Fence*               gFence11   = nullptr;
    UINT64                     gFenceVal  = 0;
    HANDLE                     gFenceEvent = nullptr;  // CPU wait for allocator-reset safety
    UINT64                     gLastDone   = 0;        // fence value the previous frame's D3D12 work signals
    ID3D11DeviceContext4*      gCtx4Frame  = nullptr;  // held between BeginD3D12Work and SubmitD3D12Work

    // Shared textures (lazily (re)created for the current ldr size/format). Each is a D3D11 texture on the
    // game device AND, opened from the same NT handle, a D3D12 resource on our side. They are plain
    // SRV/copy transfer buffers — NOT UAVs (a shared texture carrying a UAV bind is dicey; instead the
    // compute writes a D3D12-LOCAL UAV texture (gComputeOut) which is then copied into gSharedOut).
    ID3D12Resource*            gSharedIn  = nullptr;   // D3D11 copies game color in -> D3D12 reads as SRV
    ID3D12Resource*            gSharedOut = nullptr;   // D3D12 copies result in -> D3D11 reads it back
    ID3D11Texture2D*           gShared11In  = nullptr;
    ID3D11Texture2D*           gShared11Out = nullptr;
    ID3D12Resource*            gComputeOut  = nullptr; // D3D12-local UAV target the tint CS writes
    UINT                       gTexW = 0, gTexH = 0;
    DXGI_FORMAT                gTexFmt = DXGI_FORMAT_UNKNOWN;

    bool gReady    = false;
    bool gInitDone = false;

    // Tints the LEFT HALF green (rgb *= .3,1,.3 for x < split); right half is an identity passthrough.
    // A clean split in-game proves the copy-in, the compute dispatch, and the copy-back all landed.
    const char* kTintCS =
        "Texture2D<float4>   gIn  : register(t0);\n"
        "RWTexture2D<float4> gOut : register(u0);\n"
        "cbuffer P : register(b0) { float4 gTint; }\n"   // xyz = tint, w = split X (pixels)
        "[numthreads(8,8,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID)\n"
        "{\n"
        "    float4 c = gIn.Load(int3(id.xy, 0));\n"
        "    float3 rgb = (id.x < (uint)gTint.w) ? c.rgb * gTint.rgb : c.rgb;\n"
        "    gOut[id.xy] = float4(rgb, c.a);\n"
        "}\n";

    void ReleaseSharedTextures()
    {
        SafeRelease(gShared11In);
        SafeRelease(gShared11Out);
        SafeRelease(gSharedIn);
        SafeRelease(gSharedOut);
        SafeRelease(gComputeOut);
        gTexW = gTexH = 0;
        gTexFmt = DXGI_FORMAT_UNKNOWN;
    }

    // Create the shared texture on the GAME's D3D11 device and open it on our D3D12 device from the same
    // NT handle. This direction (D3D11-create -> D3D12-open via OpenSharedHandle) is the documented,
    // reliable one; the reverse (D3D12-create -> D3D11 OpenSharedResource1) returns E_INVALIDARG.
    bool CreateSharedTex(UINT w, UINT h, DXGI_FORMAT fmt,
                         ID3D12Resource** out12, ID3D11Texture2D** out11)
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width            = w;
        td.Height           = h;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = fmt;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        // NThandle needs a companion flag; the legacy SHARED one keeps it fence-syncable (KEYEDMUTEX would
        // force keyed-mutex sync, which D3D12 can't drive). This is the OptiScaler-proven combo.
        td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        HRESULT hr = gGameDev->CreateTexture2D(&td, nullptr, out11);
        if (FAILED(hr) || !*out11) { Log("D3D12Interop: D3D11 CreateTexture2D(shared) failed 0x%08X", hr); return false; }

        IDXGIResource1* dxgiRes = nullptr;
        (*out11)->QueryInterface(__uuidof(IDXGIResource1), (void**)&dxgiRes);
        if (!dxgiRes) { Log("D3D12Interop: IDXGIResource1 QI failed"); return false; }
        HANDLE sh = nullptr;
        hr = dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sh);
        dxgiRes->Release();
        if (FAILED(hr) || !sh) { Log("D3D12Interop: CreateSharedHandle(tex) failed 0x%08X", hr); return false; }

        hr = gDev->OpenSharedHandle(sh, __uuidof(ID3D12Resource), (void**)out12);
        CloseHandle(sh);   // both APIs now hold a reference; the NT handle is no longer needed
        if (FAILED(hr) || !*out12) { Log("D3D12Interop: D3D12 OpenSharedHandle(tex) failed 0x%08X", hr); return false; }
        return true;
    }

    bool EnsureSharedTextures(UINT w, UINT h, DXGI_FORMAT fmt)
    {
        if (gShared11In && gShared11Out && w == gTexW && h == gTexH && fmt == gTexFmt) return true;
        ReleaseSharedTextures();

        // Diagnostic: the compute pass writes the output via a typed UAV store, which BGRA (fmt 87, Kenshi's
        // ldr) does not guarantee. If this logs "NOT supported" and the tint doesn't appear, that's why —
        // switch the output to an RTV/graphics pass. (On the dev RTX 4090 BGRA UAV store is supported.)
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fsup = {}; fsup.Format = fmt;
        if (SUCCEEDED(gDev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fsup, sizeof(fsup))))
            Log("D3D12Interop: fmt=%d typed-UAV-store %s", (int)fmt,
                (fsup.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) ? "supported" : "NOT supported");

        if (!CreateSharedTex(w, h, fmt, &gSharedIn,  &gShared11In))  { ReleaseSharedTextures(); return false; }
        if (!CreateSharedTex(w, h, fmt, &gSharedOut, &gShared11Out)) { ReleaseSharedTextures(); return false; }

        // D3D12-local UAV target the tint CS writes (kept off the shared textures on purpose).
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = w;
        rd.Height           = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = fmt;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT hr = gDev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                        D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)&gComputeOut);
        if (FAILED(hr) || !gComputeOut) { Log("D3D12Interop: CreateCommittedResource(computeOut) failed 0x%08X", hr); ReleaseSharedTextures(); return false; }

        // Descriptor views into the shader-visible heap: SRV(gSharedIn) at [0], UAV(gComputeOut) at [1].
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = gHeap->GetCPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format                  = fmt;
        srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels     = 1;
        gDev->CreateShaderResourceView(gSharedIn, &srv, cpu);

        cpu.ptr += gDescInc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format        = fmt;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        gDev->CreateUnorderedAccessView(gComputeOut, nullptr, &uav, cpu);

        gTexW = w; gTexH = h; gTexFmt = fmt;
        Log("D3D12Interop: shared textures ready (%ux%u fmt=%d)", w, h, (int)fmt);
        return true;
    }

    void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter  = to;
        gList->ResourceBarrier(1, &b);
    }
}

bool IsReady() { return gReady; }

bool Init(ID3D11Device* d3d11Device)
{
    if (gInitDone) return gReady;
    gInitDone = true;
    if (!d3d11Device) return false;

    // 1. D3D12 device on the SAME adapter the game's D3D11 device runs on.
    IDXGIDevice* dxgiDev = nullptr;
    d3d11Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
    IDXGIAdapter* adapter = nullptr;
    if (dxgiDev) { dxgiDev->GetAdapter(&adapter); dxgiDev->Release(); }
    if (!adapter) { Log("D3D12Interop: could not get game DXGI adapter"); return false; }

    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&gDev);
    adapter->Release();
    if (FAILED(hr) || !gDev) { Log("D3D12Interop: D3D12CreateDevice failed 0x%08X (no DX12 / Proton?)", hr); return false; }

    // 2. Compute queue + allocator + list.
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;   // DIRECT can run compute + copy; simplest for a spike
    hr = gDev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&gQueue);
    if (FAILED(hr)) { Log("D3D12Interop: CreateCommandQueue failed 0x%08X", hr); Shutdown(); return false; }

    hr = gDev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&gAlloc);
    if (FAILED(hr)) { Log("D3D12Interop: CreateCommandAllocator failed 0x%08X", hr); Shutdown(); return false; }

    hr = gDev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gAlloc, nullptr,
                                 __uuidof(ID3D12GraphicsCommandList), (void**)&gList);
    if (FAILED(hr)) { Log("D3D12Interop: CreateCommandList failed 0x%08X", hr); Shutdown(); return false; }
    gList->Close();   // created open; reset per-use in RunTintTest

    // 3. Shared fence — the cross-API sync primitive. If the game device isn't D3D11.4 or the fence can't
    //    be opened (a real possibility under Proton), we bail here and the spike stays a no-op.
    hr = gDev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), (void**)&gFence);
    if (FAILED(hr)) { Log("D3D12Interop: CreateFence(SHARED) failed 0x%08X", hr); Shutdown(); return false; }

    HANDLE fh = nullptr;
    hr = gDev->CreateSharedHandle(gFence, nullptr, GENERIC_ALL, nullptr, &fh);
    if (FAILED(hr) || !fh) { Log("D3D12Interop: CreateSharedHandle(fence) failed 0x%08X", hr); Shutdown(); return false; }

    ID3D11Device5* dev5 = nullptr;
    d3d11Device->QueryInterface(__uuidof(ID3D11Device5), (void**)&dev5);
    if (!dev5) { CloseHandle(fh); Log("D3D12Interop: ID3D11Device5 unavailable (no D3D11.4 fence support)"); Shutdown(); return false; }
    hr = dev5->OpenSharedFence(fh, __uuidof(ID3D11Fence), (void**)&gFence11);
    dev5->Release();
    CloseHandle(fh);
    if (FAILED(hr) || !gFence11) { Log("D3D12Interop: OpenSharedFence failed 0x%08X (Proton limitation?)", hr); Shutdown(); return false; }

    gFenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!gFenceEvent) { Log("D3D12Interop: CreateEvent failed"); Shutdown(); return false; }

    // 4. Root signature: b0 = 4 float32 constants (tint + split), table = SRV(t0) + UAV(u0).
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors     = 1;
    ranges[0].BaseShaderRegister = 0;   // t0
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors     = 1;
    ranges[1].BaseShaderRegister = 0;   // u0
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges   = ranges;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters   = params;
    rs.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* rsBlob = nullptr; ID3DBlob* rsErr = nullptr;
    hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr);
    if (FAILED(hr)) { Log("D3D12Interop: SerializeRootSignature failed 0x%08X %s", hr, rsErr ? (const char*)rsErr->GetBufferPointer() : ""); SafeRelease(rsErr); Shutdown(); return false; }
    hr = gDev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                   __uuidof(ID3D12RootSignature), (void**)&gRootSig);
    SafeRelease(rsBlob); SafeRelease(rsErr);
    if (FAILED(hr)) { Log("D3D12Interop: CreateRootSignature failed 0x%08X", hr); Shutdown(); return false; }

    // 5. Compile the tint CS (D3DCompile -> DXBC, which D3D12 accepts for SM5) + PSO.
    ID3DBlob* cs = nullptr; ID3DBlob* csErr = nullptr;
    hr = D3DCompile(kTintCS, strlen(kTintCS), "tint_cs", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &csErr);
    if (FAILED(hr) || !cs) { Log("D3D12Interop: tint CS compile failed 0x%08X %s", hr, csErr ? (const char*)csErr->GetBufferPointer() : ""); SafeRelease(csErr); Shutdown(); return false; }
    SafeRelease(csErr);

    D3D12_COMPUTE_PIPELINE_STATE_DESC psd = {};
    psd.pRootSignature = gRootSig;
    psd.CS.pShaderBytecode = cs->GetBufferPointer();
    psd.CS.BytecodeLength  = cs->GetBufferSize();
    hr = gDev->CreateComputePipelineState(&psd, __uuidof(ID3D12PipelineState), (void**)&gPso);
    SafeRelease(cs);
    if (FAILED(hr)) { Log("D3D12Interop: CreateComputePipelineState failed 0x%08X", hr); Shutdown(); return false; }

    // 6. Shader-visible descriptor heap (2 slots: SRV + UAV).
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = gDev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&gHeap);
    if (FAILED(hr)) { Log("D3D12Interop: CreateDescriptorHeap failed 0x%08X", hr); Shutdown(); return false; }
    gDescInc = gDev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    gGameDev = d3d11Device;
    gGameDev->AddRef();
    gReady = true;
    Log("D3D12Interop: side-device READY (D3D12 device + shared fence + compute PSO on the game adapter)");
    return true;
}

bool RunTintTest(ID3D11DeviceContext* ctx, ID3D11Resource* ldrColor, uint32_t w, uint32_t h)
{
    if (!gReady || !ctx || !ldrColor || w == 0 || h == 0) return false;

    // Format AND dims must match for CopyResource; read them off the game texture rather than trusting the
    // passed display size (they can disagree for a frame mid pipeline-rebuild — a mismatch fails the copy).
    ID3D11Texture2D* srcTex = nullptr;
    ldrColor->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&srcTex);
    if (!srcTex) return false;
    D3D11_TEXTURE2D_DESC sd; srcTex->GetDesc(&sd);
    srcTex->Release();
    w = sd.Width; h = sd.Height;
    if (w == 0 || h == 0) return false;

    if (!EnsureSharedTextures(w, h, sd.Format)) return false;

    // Copy the game color into the shared input, then run the fence-synced D3D12 bracket.
    ctx->CopyResource(gShared11In, ldrColor);
    ID3D12GraphicsCommandList* list = BeginD3D12Work(ctx);
    if (!list) return false;

    // Read gSharedIn (SRV), tint into gComputeOut (UAV), then copy gComputeOut -> gSharedOut for D3D11.
    Barrier(gSharedIn,   D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(gComputeOut, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    list->SetPipelineState(gPso);
    ID3D12DescriptorHeap* heaps[] = { gHeap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(gRootSig);
    float tint[4] = { 0.3f, 1.0f, 0.3f, (float)w * 0.5f };   // xyz tint, w = split X
    list->SetComputeRoot32BitConstants(0, 4, tint, 0);
    list->SetComputeRootDescriptorTable(1, gHeap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    Barrier(gComputeOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,         D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(gSharedOut,  D3D12_RESOURCE_STATE_COMMON,                   D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(gSharedOut, gComputeOut);

    Barrier(gSharedIn,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(gComputeOut, D3D12_RESOURCE_STATE_COPY_SOURCE,               D3D12_RESOURCE_STATE_COMMON);
    Barrier(gSharedOut,  D3D12_RESOURCE_STATE_COPY_DEST,                 D3D12_RESOURCE_STATE_COMMON);

    if (!SubmitD3D12Work(ctx)) return false;
    ctx->CopyResource(ldrColor, gShared11Out);   // tinted result back into the scene color
    return true;
}

// ---- Reusable primitives ----

ID3D12Device* GetDevice() { return gDev; }

bool CreateSharedTexture(uint32_t w, uint32_t h, uint32_t fmt,
                         ID3D11Texture2D** outD3D11, ID3D12Resource** outD3D12)
{
    if (!gReady || !outD3D11 || !outD3D12) return false;
    return CreateSharedTex(w, h, (DXGI_FORMAT)fmt, outD3D12, outD3D11);
}

void Transition(ID3D12Resource* r, uint32_t fromState, uint32_t toState)
{
    if (r && gList) Barrier(r, (D3D12_RESOURCE_STATES)fromState, (D3D12_RESOURCE_STATES)toState);
}

ID3D12GraphicsCommandList* BeginD3D12Work(ID3D11DeviceContext* ctx)
{
    if (!gReady || !ctx || gCtx4Frame) return nullptr;   // gCtx4Frame set => a bracket is already open

    ID3D11DeviceContext4* ctx4 = nullptr;
    ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&ctx4);
    if (!ctx4) { Log("D3D12Interop: ID3D11DeviceContext4 unavailable (no D3D11.4 Signal/Wait)"); gReady = false; return nullptr; }

    // D3D11 signals that the caller's copies into the shared inputs are done; the D3D12 queue waits for it.
    UINT64 vReady = ++gFenceVal;
    ctx4->Signal(gFence11, vReady);
    gQueue->Wait(gFence, vReady);

    // Allocator-reset safety: the GPU must have finished LAST frame's list before Reset(). Bounded CPU wait
    // (the prior frame's Present has already flushed its D3D11 signal, so this returns fast in practice).
    if (gLastDone && gFence->GetCompletedValue() < gLastDone)
    {
        gFence->SetEventOnCompletion(gLastDone, gFenceEvent);
        if (WaitForSingleObject(gFenceEvent, 1000) != WAIT_OBJECT_0)
        {
            Log("D3D12Interop: fence wait timed out — skipping frame (allocator busy)");
            ctx4->Release();
            return nullptr;   // don't Reset a possibly in-flight allocator
        }
    }

    gAlloc->Reset();
    gList->Reset(gAlloc, nullptr);   // caller sets its own PSO
    gCtx4Frame = ctx4;               // released in SubmitD3D12Work
    return gList;
}

bool SubmitD3D12Work(ID3D11DeviceContext* ctx)
{
    (void)ctx;
    if (!gReady || !gCtx4Frame) return false;

    gList->Close();
    ID3D12CommandList* lists[] = { gList };
    gQueue->ExecuteCommandLists(1, lists);
    UINT64 vDone = ++gFenceVal;
    gQueue->Signal(gFence, vDone);
    gLastDone = vDone;               // next frame's allocator reset waits on this

    gCtx4Frame->Wait(gFence11, vDone);   // D3D11 waits for the D3D12 work before the caller reads the output
    gCtx4Frame->Release();
    gCtx4Frame = nullptr;
    return true;
}

void Shutdown()
{
    SafeRelease(gCtx4Frame);
    ReleaseSharedTextures();
    SafeRelease(gFence11);
    SafeRelease(gFence);
    SafeRelease(gHeap);
    SafeRelease(gPso);
    SafeRelease(gRootSig);
    SafeRelease(gList);
    SafeRelease(gAlloc);
    SafeRelease(gQueue);
    SafeRelease(gDev);
    SafeRelease(gGameDev);
    if (gFenceEvent) { CloseHandle(gFenceEvent); gFenceEvent = nullptr; }
    gReady = false;
    gFenceVal = 0;
    gLastDone = 0;
    // gInitDone stays true so a failed init doesn't retry every frame; Shutdown on real teardown is final.
}

} // namespace D3D12Interop
