#include "../src/D3D12Interop.cpp"
#include <dxgi1_4.h>
#include <cassert>

// Exercise the real retirement code against a deliberately blocked WARP queue.
// Including the implementation lets the fixture use a private queue without
// requiring a running game or exporting test hooks from Dust.dll.
int main()
{
    using namespace D3D12Interop;
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter* warp = nullptr;
    assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    assert(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    assert(SUCCEEDED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gDev))));
    warp->Release(); factory->Release();
    D3D12_COMMAND_QUEUE_DESC qd = {};
    assert(SUCCEEDED(gDev->CreateCommandQueue(&qd, IID_PPV_ARGS(&gQueue))));
    assert(SUCCEEDED(gDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gFence))));
    ID3D12Fence* gate = nullptr;
    assert(SUCCEEDED(gDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))));
    gFenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    assert(gFenceEvent);
    gReady = true;
    assert(WaitForGpuIdle() == GpuWaitResult::Completed);
    assert(SUCCEEDED(gQueue->Wait(gate, 1)));
    gLastDone = 1;
    assert(SUCCEEDED(gQueue->Signal(gFence, gLastDone)));
    assert(WaitForGpuIdle() == GpuWaitResult::Pending);
    Shutdown();
    assert(gDev && gQueue && gFence && gFenceEvent); // timeout must retain ownership
    assert(!IsReady());
    assert(SUCCEEDED(gate->Signal(1)));
    assert(WaitForGpuIdle() == GpuWaitResult::Completed);
    gate->Release();

    // A submitted list without a completion signal must acquire one before release.
    gLastDone = 2;
    gUnfencedWork = true;
    assert(WaitForGpuIdle() == GpuWaitResult::Completed);
    assert(!gUnfencedWork);

    ID3D12Device5* dev5 = nullptr;
    assert(SUCCEEDED(gDev->QueryInterface(IID_PPV_ARGS(&dev5))));
    dev5->RemoveDevice();
    assert(WaitForGpuIdle() == GpuWaitResult::DeviceRemoved);
    dev5->Release();
    Shutdown();
    assert(!gDev && !gQueue && !gFence && !gFenceEvent);
}
