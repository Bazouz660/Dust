#include "RtCore.h"
#include "RtLog.h"

#include <dxgi1_4.h>
#include <dxcapi.h>
#include <vector>
#include <string>
#include <cstring>

#pragma comment(lib, "d3d12.lib")

namespace RtCore
{

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static ID3D12Device5*           sDevice  = nullptr;
static ID3D12CommandQueue*      sQueue   = nullptr;
static ID3D12Fence*             sFence   = nullptr;          // shared with D3D11
static HANDLE                   sFenceEvent = nullptr;
static uint64_t                 sFenceCounter = 0;           // last value handed out
static ID3D11Fence*             sFence11 = nullptr;          // 11-side view of sFence
static ID3D11Device5*           sDevice11_5 = nullptr;
static ID3D11Device*            sDevice11 = nullptr;
static ID3D11DeviceContext4*    sCachedCtx4 = nullptr;       // QI'd once from the immediate ctx
static ID3D11DeviceContext*     sCachedCtxRaw = nullptr;

static const uint32_t           kRingSize = 3;
static ID3D12CommandAllocator*  sAllocators[kRingSize] = {};
static uint64_t                 sAllocFence[kRingSize] = {};
static ID3D12GraphicsCommandList4* sCmdList = nullptr;
static uint32_t                 sRingSlot = 0;
static bool                     sRecording = false;

// shader-visible descriptor heap, segmented per ring slot
static const uint32_t           kDescPerSlot = 1024;
static ID3D12DescriptorHeap*    sDescHeap = nullptr;
static uint32_t                 sDescStride = 0;
static uint32_t                 sDescUsed = 0;               // within current slot

// per-dispatch CB upload ring, segmented per ring slot
static const uint32_t           kCbBytesPerSlot = 64 * 1024;
static ID3D12Resource*          sCbRing = nullptr;
static uint8_t*                 sCbMapped = nullptr;
static uint32_t                 sCbUsed = 0;

// deferred releases: {fence value, object}
struct DeferredRelease { uint64_t fence; IUnknown* obj; };
static std::vector<DeferredRelease> sDeferred;
static uint64_t                 sLastSubmitFence = 0;

// DXC
static HMODULE                  sDxilMod = nullptr;
static HMODULE                  sDxcMod = nullptr;
static IDxcCompiler3*           sCompiler = nullptr;
static IDxcUtils*               sUtils = nullptr;
static IDxcIncludeHandler*      sIncludeHandler = nullptr;

static char                     sStatus[256] = "not initialized";
static bool                     sAvailable = false;

static void SetStatus(const char* msg)
{
    strncpy_s(sStatus, msg, _TRUNCATE);
    Log("RtCore: %s", msg);
}

const char* StatusString() { return sStatus; }
bool IsAvailable() { return sAvailable; }
ID3D12Device5* Device() { return sDevice; }
ID3D12CommandQueue* Queue() { return sQueue; }

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
bool Init(ID3D11Device* gameDevice)
{
    if (sAvailable) return true;

    // Pick the adapter: the game device's LUID when present, else the first
    // hardware adapter that yields a DXR-capable D3D12 device.
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
    {
        SetStatus("CreateDXGIFactory2 failed");
        return false;
    }

    IDXGIAdapter1* adapter = nullptr;
    if (gameDevice)
    {
        IDXGIDevice* dxgiDev = nullptr;
        if (SUCCEEDED(gameDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev))))
        {
            IDXGIAdapter* a = nullptr;
            if (SUCCEEDED(dxgiDev->GetAdapter(&a)))
            {
                DXGI_ADAPTER_DESC desc = {};
                a->GetDesc(&desc);
                factory->EnumAdapterByLuid(desc.AdapterLuid, IID_PPV_ARGS(&adapter));
                a->Release();
            }
            dxgiDev->Release();
        }
        if (!adapter)
        {
            factory->Release();
            SetStatus("could not resolve the game adapter");
            return false;
        }
    }

    // Optional debug layer (Dust.ini [Dust] RtDebugLayer=1) — only meaningful
    // when launched under a debugger / with debug runtime installed.
#ifndef RT_SELFTEST
    {
        std::string ini = DustLogDir() + "Dust.ini";
        if (GetPrivateProfileIntA("Dust", "RtDebugLayer", 0, ini.c_str()))
        {
            ID3D12Debug* dbg = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
            {
                dbg->EnableDebugLayer();
                dbg->Release();
                Log("RtCore: D3D12 debug layer enabled");
            }
        }
    }
#endif

    HRESULT hr = E_FAIL;
    if (adapter)
    {
        hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&sDevice));
        adapter->Release();
    }
    else
    {
        // self-test: walk hardware adapters until one creates
        for (UINT i = 0; ; ++i)
        {
            IDXGIAdapter1* a = nullptr;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d = {};
            a->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            {
                hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&sDevice));
                if (SUCCEEDED(hr))
                {
                    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
                    if (SUCCEEDED(sDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))) &&
                        o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1)
                    {
                        a->Release();
                        break;   // keep this device
                    }
                    sDevice->Release();
                    sDevice = nullptr;
                    hr = E_FAIL;
                }
            }
            a->Release();
        }
    }
    factory->Release();

    if (FAILED(hr) || !sDevice)
    {
        SetStatus("D3D12CreateDevice failed");
        return false;
    }

    // DXR tier check — inline RayQuery needs tier 1.1.
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5 = {};
    if (FAILED(sDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5))) ||
        opt5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
    {
        sDevice->Release(); sDevice = nullptr;
        SetStatus("GPU lacks DXR tier 1.1 (inline ray tracing)");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(sDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&sQueue))))
    {
        SetStatus("CreateCommandQueue failed");
        Shutdown();
        return false;
    }

    // Shared fence (12-created; 11 opens it when a game device exists)
    if (FAILED(sDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sFence))))
    {
        SetStatus("CreateFence(SHARED) failed");
        Shutdown();
        return false;
    }
    sFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (gameDevice)
    {
        sDevice11 = gameDevice;
        if (FAILED(gameDevice->QueryInterface(IID_PPV_ARGS(&sDevice11_5))))
        {
            SetStatus("game device lacks ID3D11Device5 (Win10 1703+ runtime needed)");
            Shutdown();
            return false;
        }
        HANDLE h = nullptr;
        if (FAILED(sDevice->CreateSharedHandle(sFence, nullptr, GENERIC_ALL, nullptr, &h)) ||
            FAILED(sDevice11_5->OpenSharedFence(h, IID_PPV_ARGS(&sFence11))))
        {
            if (h) CloseHandle(h);
            SetStatus("shared fence interop failed");
            Shutdown();
            return false;
        }
        CloseHandle(h);
    }

    for (uint32_t i = 0; i < kRingSize; ++i)
    {
        if (FAILED(sDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&sAllocators[i]))))
        {
            SetStatus("CreateCommandAllocator failed");
            Shutdown();
            return false;
        }
    }
    if (FAILED(sDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, sAllocators[0],
                                          nullptr, IID_PPV_ARGS(&sCmdList))))
    {
        SetStatus("CreateCommandList failed");
        Shutdown();
        return false;
    }
    sCmdList->Close();

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kDescPerSlot * kRingSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(sDevice->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&sDescHeap))))
    {
        SetStatus("CreateDescriptorHeap failed");
        Shutdown();
        return false;
    }
    sDescStride = sDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    sCbRing = CreateBuffer(kCbBytesPerSlot * kRingSize, D3D12_HEAP_TYPE_UPLOAD,
                           D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!sCbRing || FAILED(sCbRing->Map(0, nullptr, (void**)&sCbMapped)))
    {
        SetStatus("CB upload ring creation failed");
        Shutdown();
        return false;
    }

    sAvailable = true;
    SetStatus("D3D12 device + DXR tier 1.1 ready");
    return true;
}

void Shutdown()
{
    if (sQueue && sFence)
    {
        // drain the GPU so nothing references resources we are about to free
        uint64_t v = ++sFenceCounter;
        sQueue->Signal(sFence, v);
        CpuWait(v);
    }
    for (auto& d : sDeferred) if (d.obj) d.obj->Release();
    sDeferred.clear();

    if (sCbRing && sCbMapped) sCbRing->Unmap(0, nullptr);
    sCbMapped = nullptr;
    if (sCbRing)    { sCbRing->Release();    sCbRing = nullptr; }
    if (sDescHeap)  { sDescHeap->Release();  sDescHeap = nullptr; }
    if (sCmdList)   { sCmdList->Release();   sCmdList = nullptr; }
    for (uint32_t i = 0; i < kRingSize; ++i)
        if (sAllocators[i]) { sAllocators[i]->Release(); sAllocators[i] = nullptr; }
    if (sFence11)   { sFence11->Release();   sFence11 = nullptr; }
    if (sCachedCtx4){ sCachedCtx4->Release(); sCachedCtx4 = nullptr; }
    sCachedCtxRaw = nullptr;
    if (sDevice11_5){ sDevice11_5->Release(); sDevice11_5 = nullptr; }
    sDevice11 = nullptr;
    if (sFenceEvent){ CloseHandle(sFenceEvent); sFenceEvent = nullptr; }
    if (sFence)     { sFence->Release();     sFence = nullptr; }
    if (sQueue)     { sQueue->Release();     sQueue = nullptr; }
    if (sIncludeHandler) { sIncludeHandler->Release(); sIncludeHandler = nullptr; }
    if (sUtils)     { sUtils->Release();     sUtils = nullptr; }
    if (sCompiler)  { sCompiler->Release();  sCompiler = nullptr; }
    if (sDevice)    { sDevice->Release();    sDevice = nullptr; }
    sAvailable = false;
}

// ---------------------------------------------------------------------------
// fence
// ---------------------------------------------------------------------------
uint64_t NextFenceValue() { return ++sFenceCounter; }
uint64_t CompletedValue() { return sFence ? sFence->GetCompletedValue() : 0; }

static ID3D11DeviceContext4* Ctx4(ID3D11DeviceContext* ctx)
{
    if (!ctx) return nullptr;
    if (ctx == sCachedCtxRaw) return sCachedCtx4;
    ID3D11DeviceContext4* c4 = nullptr;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&c4))))
        return nullptr;
    if (sCachedCtx4) sCachedCtx4->Release();
    sCachedCtx4 = c4;          // keep the ref
    sCachedCtxRaw = ctx;
    return c4;
}

bool Signal11(ID3D11DeviceContext* ctx, uint64_t value)
{
    ID3D11DeviceContext4* c4 = Ctx4(ctx);
    if (!c4 || !sFence11) return false;
    return SUCCEEDED(c4->Signal(sFence11, value));
}

bool Wait11(ID3D11DeviceContext* ctx, uint64_t value)
{
    ID3D11DeviceContext4* c4 = Ctx4(ctx);
    if (!c4 || !sFence11) return false;
    return SUCCEEDED(c4->Wait(sFence11, value));
}

bool Signal12(uint64_t value)
{
    return sQueue && SUCCEEDED(sQueue->Signal(sFence, value));
}

bool Wait12(uint64_t value)
{
    return sQueue && SUCCEEDED(sQueue->Wait(sFence, value));
}

void CpuWait(uint64_t value)
{
    if (!sFence) return;
    if (sFence->GetCompletedValue() >= value) return;
    if (SUCCEEDED(sFence->SetEventOnCompletion(value, sFenceEvent)))
        WaitForSingleObject(sFenceEvent, 10000);
}

// ---------------------------------------------------------------------------
// shared textures
// ---------------------------------------------------------------------------
void SharedTexture::Release()
{
    if (tex12) { tex12->Release(); tex12 = nullptr; }
    if (tex11) { tex11->Release(); tex11 = nullptr; }
    width = height = 0;
}

bool CreateSharedTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, uint32_t bindFlags,
                         SharedTexture& out)
{
    out.Release();
    if (!sDevice11 || !sDevice) return false;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bindFlags;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT hr = sDevice11->CreateTexture2D(&td, nullptr, &out.tex11);
    if (FAILED(hr))
    {
        Log("RtCore: shared texture create (11) failed fmt=%d hr=0x%08X", (int)fmt, (unsigned)hr);
        return false;
    }

    IDXGIResource1* dxgiRes = nullptr;
    HANDLE handle = nullptr;
    hr = out.tex11->QueryInterface(IID_PPV_ARGS(&dxgiRes));
    if (SUCCEEDED(hr))
    {
        hr = dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                         nullptr, &handle);
        dxgiRes->Release();
    }
    if (SUCCEEDED(hr))
        hr = sDevice->OpenSharedHandle(handle, IID_PPV_ARGS(&out.tex12));
    if (handle) CloseHandle(handle);

    if (FAILED(hr))
    {
        Log("RtCore: shared texture open (12) failed hr=0x%08X", (unsigned)hr);
        out.Release();
        return false;
    }
    out.format = fmt; out.width = w; out.height = h;
    return true;
}

// ---------------------------------------------------------------------------
// resources
// ---------------------------------------------------------------------------
ID3D12Resource* CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heap,
                             D3D12_RESOURCE_STATES initState, D3D12_RESOURCE_FLAGS flags)
{
    if (!sDevice || size == 0) return nullptr;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ID3D12Resource* res = nullptr;
    HRESULT hr = sDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initState,
                                                  nullptr, IID_PPV_ARGS(&res));
    if (FAILED(hr))
    {
        Log("RtCore: CreateBuffer(%llu) failed hr=0x%08X", (unsigned long long)size, (unsigned)hr);
        return nullptr;
    }
    return res;
}

ID3D12Resource* CreateUploadBuffer(const void* data, uint64_t size)
{
    ID3D12Resource* res = CreateBuffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!res) return nullptr;
    void* mapped = nullptr;
    if (FAILED(res->Map(0, nullptr, &mapped)))
    {
        res->Release();
        return nullptr;
    }
    memcpy(mapped, data, size);
    res->Unmap(0, nullptr);
    return res;
}

ID3D12Resource* CreateUavTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
    if (!sDevice) return nullptr;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // SIMULTANEOUS_ACCESS: state decays to COMMON after every ECL and promotes
    // per use -> the frame loop needs only UAV barriers, no transitions.
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    ID3D12Resource* res = nullptr;
    HRESULT hr = sDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res));
    if (FAILED(hr))
    {
        Log("RtCore: CreateUavTexture %ux%u fmt=%d failed hr=0x%08X", w, h, (int)fmt, (unsigned)hr);
        return nullptr;
    }
    return res;
}

void DeferRelease(IUnknown* obj)
{
    if (!obj) return;
    // released once the NEXT submit's fence completes (conservative: +1 covers
    // "deferred during recording, before Submit assigns the value")
    sDeferred.push_back({ sFenceCounter + 1, obj });
}

// ---------------------------------------------------------------------------
// commands / descriptors / CBs
// ---------------------------------------------------------------------------
ID3D12GraphicsCommandList4* Begin()
{
    if (!sAvailable || sRecording) return nullptr;
    sRingSlot = (sRingSlot + 1) % kRingSize;
    if (sAllocFence[sRingSlot])
        CpuWait(sAllocFence[sRingSlot]);   // ring slot still in flight (rare)

    // retire completed deferred releases
    uint64_t done = sFence->GetCompletedValue();
    size_t kept = 0;
    for (size_t i = 0; i < sDeferred.size(); ++i)
    {
        if (sDeferred[i].fence <= done) sDeferred[i].obj->Release();
        else sDeferred[kept++] = sDeferred[i];
    }
    sDeferred.resize(kept);

    if (FAILED(sAllocators[sRingSlot]->Reset())) return nullptr;
    if (FAILED(sCmdList->Reset(sAllocators[sRingSlot], nullptr))) return nullptr;
    sDescUsed = 0;
    sCbUsed = 0;
    ID3D12DescriptorHeap* heaps[1] = { sDescHeap };
    sCmdList->SetDescriptorHeaps(1, heaps);
    sRecording = true;
    return sCmdList;
}

uint64_t Submit()
{
    if (!sRecording) return 0;
    sRecording = false;
    if (FAILED(sCmdList->Close())) return 0;
    ID3D12CommandList* lists[1] = { sCmdList };
    sQueue->ExecuteCommandLists(1, lists);
    uint64_t v = ++sFenceCounter;
    sQueue->Signal(sFence, v);
    sAllocFence[sRingSlot] = v;
    sLastSubmitFence = v;
    return v;
}

ID3D12DescriptorHeap* DescHeap() { return sDescHeap; }

Descs AllocDescs(uint32_t count)
{
    Descs d = {};
    if (!sDescHeap || sDescUsed + count > kDescPerSlot)
        return d;
    uint32_t base = sRingSlot * kDescPerSlot + sDescUsed;
    sDescUsed += count;
    d.cpu = sDescHeap->GetCPUDescriptorHandleForHeapStart();
    d.gpu = sDescHeap->GetGPUDescriptorHandleForHeapStart();
    d.cpu.ptr += (uint64_t)base * sDescStride;
    d.gpu.ptr += (uint64_t)base * sDescStride;
    d.stride = sDescStride;
    return d;
}

D3D12_GPU_VIRTUAL_ADDRESS AllocCB(const void* data, uint32_t size)
{
    uint32_t aligned = (size + 255) & ~255u;
    if (!sCbMapped || sCbUsed + aligned > kCbBytesPerSlot) return 0;
    uint32_t offset = sRingSlot * kCbBytesPerSlot + sCbUsed;
    memcpy(sCbMapped + offset, data, size);
    sCbUsed += aligned;
    return sCbRing->GetGPUVirtualAddress() + offset;
}

// ---------------------------------------------------------------------------
// DXC
// ---------------------------------------------------------------------------
bool InitShaderCompiler(const wchar_t* dxcDir)
{
    if (sCompiler) return true;
    std::wstring dir = dxcDir ? dxcDir : L"";
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';

    // dxil.dll first so the validator/signer resolves; unsigned DXIL is
    // rejected by the runtime outside developer mode.
    std::wstring dxil = dir + L"dxil.dll";
    std::wstring dxc  = dir + L"dxcompiler.dll";
    sDxilMod = LoadLibraryW(dxil.c_str());
    sDxcMod  = LoadLibraryW(dxc.c_str());
    if (!sDxcMod)
    {
        Log("RtCore: dxcompiler.dll not found at %S", dir.c_str());
        return false;
    }
    if (!sDxilMod)
        Log("RtCore: WARNING dxil.dll missing — DXIL will be unsigned");

    auto create = (HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*))GetProcAddress(sDxcMod, "DxcCreateInstance");
    if (!create)
    {
        Log("RtCore: DxcCreateInstance not exported");
        return false;
    }
    if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&sCompiler))) ||
        FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&sUtils))) ||
        FAILED(sUtils->CreateDefaultIncludeHandler(&sIncludeHandler)))
    {
        Log("RtCore: DXC instance creation failed");
        return false;
    }
    Log("RtCore: DXC loaded from %S", dir.c_str());
    return true;
}

IUnknown* CompileCS(const wchar_t* hlslPath, const wchar_t* includeDir,
                    const void** outBytecode, size_t* outBytecodeSize)
{
    *outBytecode = nullptr;
    *outBytecodeSize = 0;
    if (!sCompiler || !sUtils) return nullptr;

    IDxcBlobEncoding* src = nullptr;
    if (FAILED(sUtils->LoadFile(hlslPath, nullptr, &src)))
    {
        Log("RtCore: cannot read shader %S", hlslPath);
        return nullptr;
    }

    std::wstring inc = L"-I";
    const wchar_t* args[8];
    uint32_t argc = 0;
    args[argc++] = L"-T"; args[argc++] = L"cs_6_5";
    args[argc++] = L"-E"; args[argc++] = L"main";
    std::wstring incDir = includeDir ? includeDir : L"";
    if (!incDir.empty()) { args[argc++] = L"-I"; args[argc++] = incDir.c_str(); }

    DxcBuffer buf = {};
    buf.Ptr = src->GetBufferPointer();
    buf.Size = src->GetBufferSize();
    buf.Encoding = DXC_CP_ACP;

    IDxcResult* result = nullptr;
    HRESULT hr = sCompiler->Compile(&buf, args, argc, sIncludeHandler, IID_PPV_ARGS(&result));
    src->Release();
    if (FAILED(hr) || !result)
    {
        Log("RtCore: DXC Compile() call failed for %S", hlslPath);
        return nullptr;
    }

    IDxcBlobUtf8* errors = nullptr;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (errors && errors->GetStringLength() > 0)
        Log("RtCore: DXC %s for %S:\n%s", FAILED(status) ? "ERRORS" : "warnings",
            hlslPath, errors->GetStringPointer());
    if (errors) errors->Release();

    if (FAILED(status))
    {
        result->Release();
        return nullptr;
    }

    IDxcBlob* obj = nullptr;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr);
    result->Release();
    if (!obj || obj->GetBufferSize() == 0)
    {
        if (obj) obj->Release();
        Log("RtCore: DXC produced no object for %S", hlslPath);
        return nullptr;
    }
    *outBytecode = obj->GetBufferPointer();
    *outBytecodeSize = obj->GetBufferSize();
    return obj;   // caller keeps it alive while bytecode is in use, then Release()s
}

// ---------------------------------------------------------------------------
// pipeline
// ---------------------------------------------------------------------------
ID3D12RootSignature* CreateCommonRootSig(uint32_t numSRVs, uint32_t numUAVs)
{
    if (!sDevice) return nullptr;

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = numSRVs;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = numUAVs;
    ranges[1].BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 3;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 2;
    rsd.pStaticSamplers = samplers;

    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
    {
        if (err) { Log("RtCore: root sig serialize: %s", (const char*)err->GetBufferPointer()); err->Release(); }
        return nullptr;
    }
    if (err) err->Release();
    ID3D12RootSignature* rs = nullptr;
    HRESULT hr = sDevice->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(&rs));
    blob->Release();
    if (FAILED(hr))
    {
        Log("RtCore: CreateRootSignature failed hr=0x%08X", (unsigned)hr);
        return nullptr;
    }
    return rs;
}

ID3D12PipelineState* CreateComputePSO(ID3D12RootSignature* rs,
                                      const void* bytecode, size_t bytecodeSize)
{
    if (!sDevice || !rs || !bytecode) return nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.CS.pShaderBytecode = bytecode;
    pd.CS.BytecodeLength = bytecodeSize;
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = sDevice->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    if (FAILED(hr))
    {
        Log("RtCore: CreateComputePipelineState failed hr=0x%08X", (unsigned)hr);
        return nullptr;
    }
    return pso;
}

// ---------------------------------------------------------------------------
// acceleration structures
// ---------------------------------------------------------------------------
ID3D12Resource* BuildBLAS(ID3D12GraphicsCommandList4* cmd,
                          D3D12_GPU_VIRTUAL_ADDRESS vbVA, uint32_t vertexCount, uint32_t vbStride,
                          D3D12_GPU_VIRTUAL_ADDRESS ibVA, uint32_t indexCount, DXGI_FORMAT ibFmt)
{
    if (!sDevice || !cmd || vertexCount == 0 || indexCount == 0) return nullptr;

    D3D12_RAYTRACING_GEOMETRY_DESC geom = {};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer.StartAddress = vbVA;
    geom.Triangles.VertexBuffer.StrideInBytes = vbStride;
    geom.Triangles.VertexCount = vertexCount;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.IndexBuffer = ibVA;
    geom.Triangles.IndexCount = indexCount;
    geom.Triangles.IndexFormat = ibFmt;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = 1;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = &geom;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    sDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0) return nullptr;

    ID3D12Resource* result = CreateBuffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12Resource* scratch = CreateBuffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                           D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!result || !scratch)
    {
        if (result) result->Release();
        if (scratch) scratch->Release();
        return nullptr;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
    bd.Inputs = inputs;
    bd.DestAccelerationStructureData = result->GetGPUVirtualAddress();
    bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    cmd->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);

    DeferRelease(scratch);
    return result;
}

bool Tlas::Create(uint32_t maxInstances)
{
    Release();
    if (!sDevice || maxInstances == 0) return false;
    capacity = maxInstances;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = maxInstances;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    sDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0) return false;

    result = CreateBuffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    scratch = CreateBuffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    instances = CreateBuffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * (uint64_t)maxInstances,
                             D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!result || !scratch || !instances ||
        FAILED(instances->Map(0, nullptr, (void**)&mapped)))
    {
        Release();
        return false;
    }
    return true;
}

void Tlas::Build(ID3D12GraphicsCommandList4* cmd, uint32_t instanceCount)
{
    if (!cmd || !result || instanceCount == 0) return;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
    bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    bd.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    bd.Inputs.NumDescs = instanceCount;
    bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bd.Inputs.InstanceDescs = instances->GetGPUVirtualAddress();
    bd.DestAccelerationStructureData = result->GetGPUVirtualAddress();
    bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    cmd->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);

    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = result;
    cmd->ResourceBarrier(1, &uav);
}

void Tlas::Release()
{
    if (instances && mapped) instances->Unmap(0, nullptr);
    mapped = nullptr;
    if (instances) { instances->Release(); instances = nullptr; }
    if (scratch)   { scratch->Release();   scratch = nullptr; }
    if (result)    { result->Release();    result = nullptr; }
    capacity = 0;
}

} // namespace RtCore
