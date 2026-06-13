#pragma once

// RtCore — D3D12/DXR sidecar foundation.
//
// An independent ID3D12Device5 created on the SAME adapter as the game's D3D11
// device (NOT D3D11On12 — the game's device is untouched). Bridges:
//   - shared fence  : D3D12 CreateFence(SHARED) -> ID3D11Device5::OpenSharedFence.
//     All cross-API sync is GPU-side (ctx4->Signal/Wait), never a CPU stall.
//   - shared textures: created on the D3D11 side (MISC_SHARED|SHARED_NTHANDLE),
//     opened on D3D12 via OpenSharedHandle.
//   - DXC           : dxcompiler.dll/dxil.dll loaded from an explicit directory
//     (the mod dir in-game); compiles cs_6_5 inline-RayQuery compute shaders.
//
// Fail-closed: Init() returning false leaves every other call a safe no-op.

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

namespace RtCore
{
    // ---- lifetime -------------------------------------------------------
    // gameDevice may be null in the self-test (no interop; D3D12 only).
    bool Init(ID3D11Device* gameDevice);
    void Shutdown();
    bool IsAvailable();
    const char* StatusString();   // human-readable init/fail status for the GUI

    ID3D12Device5*       Device();
    ID3D12CommandQueue*  Queue();

    // ---- shared fence ---------------------------------------------------
    uint64_t NextFenceValue();                                  // monotonic
    bool Signal11(ID3D11DeviceContext* ctx, uint64_t value);    // 11 GPU signals
    bool Wait11(ID3D11DeviceContext* ctx, uint64_t value);      // 11 GPU waits
    bool Signal12(uint64_t value);                              // 12 queue signals
    bool Wait12(uint64_t value);                                // 12 queue waits
    void CpuWait(uint64_t value);                               // blocking (shutdown/readback)
    uint64_t CompletedValue();

    // ---- shared textures --------------------------------------------------
    struct SharedTexture
    {
        ID3D11Texture2D* tex11 = nullptr;
        ID3D12Resource*  tex12 = nullptr;
        DXGI_FORMAT      format = DXGI_FORMAT_UNKNOWN;
        uint32_t         width = 0, height = 0;
        void Release();
    };
    // bindFlags: D3D11_BIND_* the 11 side needs (SHADER_RESOURCE, RENDER_TARGET,
    // UNORDERED_ACCESS...). The 12 side derives matching capabilities.
    bool CreateSharedTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, uint32_t bindFlags,
                             SharedTexture& out);

    // ---- D3D12 resources ---------------------------------------------------
    ID3D12Resource* CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heap,
                                 D3D12_RESOURCE_STATES initState,
                                 D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* CreateUploadBuffer(const void* data, uint64_t size);
    // UAV texture with ALLOW_SIMULTANEOUS_ACCESS: lives in COMMON, promotes per
    // use and decays every ExecuteCommandLists -> no transition tracking needed.
    ID3D12Resource* CreateUavTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    // Queue a release for after the CURRENT submit's fence completes.
    void DeferRelease(IUnknown* obj);

    // ---- commands ---------------------------------------------------------
    // Single recording list over a ring of allocators; Begin throttles on the
    // ring slot's previous submit. Begin/Submit pairs; no nesting.
    ID3D12GraphicsCommandList4* Begin();
    uint64_t Submit();   // returns the fence value signaled by this submit (0 on failure)

    // ---- descriptors (shader-visible CBV/SRV/UAV heap, per-frame linear) ----
    struct Descs
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
        uint32_t stride = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t i) const { return { cpu.ptr + (uint64_t)i * stride }; }
        D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t i) const { return { gpu.ptr + (uint64_t)i * stride }; }
        bool Valid() const { return cpu.ptr != 0; }
    };
    Descs AllocDescs(uint32_t count);   // valid for the current ring slot only
    ID3D12DescriptorHeap* DescHeap();

    // ---- per-dispatch constant buffers (upload ring, persistently mapped) ----
    // Returns the GPU VA of a 256-aligned copy of `data`. Valid for the current
    // ring slot only.
    D3D12_GPU_VIRTUAL_ADDRESS AllocCB(const void* data, uint32_t size);

    // ---- DXC ----------------------------------------------------------------
    // dxcDir: directory containing dxcompiler.dll + dxil.dll.
    bool InitShaderCompiler(const wchar_t* dxcDir);
    // Compiles `main` of the file as cs_6_5. includeDir is added to the include
    // search path. Returns DXIL bytecode (caller releases via the returned blob's
    // Release through IUnknown) or null (errors logged).
    IUnknown* CompileCS(const wchar_t* hlslPath, const wchar_t* includeDir,
                        const void** outBytecode, size_t* outBytecodeSize);

    // Common root signature for every RT pass:
    //   param 0 : root CBV b0
    //   param 1 : table  SRV t0..t(numSRVs-1)
    //   param 2 : table  UAV u0..u(numUAVs-1)
    //   static  : s0 point-clamp, s1 linear-clamp
    ID3D12RootSignature* CreateCommonRootSig(uint32_t numSRVs, uint32_t numUAVs);
    ID3D12PipelineState* CreateComputePSO(ID3D12RootSignature* rs,
                                          const void* bytecode, size_t bytecodeSize);

    // ---- acceleration structures -------------------------------------------
    // Records a BLAS build (triangles, R32G32B32_FLOAT positions). Allocates the
    // result + scratch; scratch is DeferRelease'd. Returns the result in
    // RAYTRACING_ACCELERATION_STRUCTURE state, or null.
    ID3D12Resource* BuildBLAS(ID3D12GraphicsCommandList4* cmd,
                              D3D12_GPU_VIRTUAL_ADDRESS vbVA, uint32_t vertexCount, uint32_t vbStride,
                              D3D12_GPU_VIRTUAL_ADDRESS ibVA, uint32_t indexCount, DXGI_FORMAT ibFmt);

    // TLAS sized for maxInstances; rebuild each frame from an upload-heap
    // instance array. Caller writes instanceDescs then calls Build (records the
    // build + a UAV barrier on the TLAS).
    struct Tlas
    {
        ID3D12Resource* result = nullptr;
        ID3D12Resource* scratch = nullptr;
        ID3D12Resource* instances = nullptr;     // upload heap, persistently mapped
        D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
        uint32_t capacity = 0;
        bool Create(uint32_t maxInstances);
        void Build(ID3D12GraphicsCommandList4* cmd, uint32_t instanceCount);
        void Release();
    };
}
