// DustBoot — preload plugin that hooks IDXGIFactory::CreateSwapChain
// before the game creates its D3D11 device. Captures the swap chain pointer
// and exposes it to the main Dust.dll plugin via exported getter functions.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdarg>

#include <core/Functions.h>
#include <Debug.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ==================== Logging ====================

static HMODULE gDllModule = nullptr;

static bool& BootLogEnabled()
{
    static bool enabled = false;
    return enabled;
}

static std::string& BootLogDir()
{
    static std::string dir;
    return dir;
}

static FILE* BootLogFile()
{
    static FILE* f = nullptr;
    if (!f && !BootLogDir().empty())
    {
        std::string logsDir = BootLogDir() + "logs";
        CreateDirectoryA(logsDir.c_str(), nullptr);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char filename[128];
        snprintf(filename, sizeof(filename),
                 "DustBoot_%04d-%02d-%02d_%02d-%02d-%02d.log",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);

        std::string path = logsDir + "\\" + filename;
        f = fopen(path.c_str(), "w");
    }
    return f;
}

static void BootLog(const char* fmt, ...)
{
    if (!BootLogEnabled())
        return;

    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    OutputDebugStringA("[DustBoot] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    char fullBuf[600];
    snprintf(fullBuf, sizeof(fullBuf), "[DustBoot] %s", buf);
    ::DebugLog(fullBuf);

    FILE* f = BootLogFile();
    if (f)
    {
        fprintf(f, "[DustBoot] %s\n", buf);
        fflush(f);
    }
}

static void BootLogInit()
{
    char path[MAX_PATH];
    GetModuleFileNameA(gDllModule, path, MAX_PATH);
    std::string dir(path);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
        dir = dir.substr(0, pos + 1);

    BootLogDir() = dir;

    std::string ini = dir + "Dust.ini";
    BootLogEnabled() = GetPrivateProfileIntA("Dust", "Logging", 0, ini.c_str()) != 0;
}

// ==================== Captured state ====================

static IDXGISwapChain* gCapturedSwapChain = nullptr;
static HWND             gCapturedHWND      = nullptr;
static bool             gHooked            = false;

// ==================== Hook trampolines ====================

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(
    IDXGIFactory* pThis, IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(
    IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);

static PFN_CreateSwapChain          oCreateSwapChain          = nullptr;
static PFN_CreateSwapChainForHwnd   oCreateSwapChainForHwnd   = nullptr;

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
    IDXGIFactory* pThis, IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain)
{
    HRESULT hr = oCreateSwapChain(pThis, pDevice, pDesc, ppSwapChain);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain)
    {
        if (gCapturedSwapChain)
            gCapturedSwapChain->Release();
        gCapturedSwapChain = *ppSwapChain;
        gCapturedSwapChain->AddRef();
        gCapturedHWND = pDesc ? pDesc->OutputWindow : nullptr;
        BootLog("Captured swap chain %p (HWND=%p, AddRef'd) via CreateSwapChain",
                gCapturedSwapChain, gCapturedHWND);
    }
    else
    {
        BootLog("CreateSwapChain called but failed or returned null (hr=0x%08X)", (unsigned)hr);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
    IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    HRESULT hr = oCreateSwapChainForHwnd(pThis, pDevice, hWnd,
                                          pDesc, pFullscreenDesc,
                                          pRestrictToOutput, ppSwapChain);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain)
    {
        if (gCapturedSwapChain)
            gCapturedSwapChain->Release();
        gCapturedSwapChain = (IDXGISwapChain*)*ppSwapChain;
        gCapturedSwapChain->AddRef();
        gCapturedHWND = hWnd;
        BootLog("Captured swap chain %p (HWND=%p, AddRef'd) via CreateSwapChainForHwnd",
                gCapturedSwapChain, gCapturedHWND);
    }
    else
    {
        BootLog("CreateSwapChainForHwnd called but failed or returned null (hr=0x%08X)", (unsigned)hr);
    }

    return hr;
}

// ==================== Exported getters ====================

extern "C" __declspec(dllexport) IDXGISwapChain* DustBoot_GetSwapChain()
{
    return gCapturedSwapChain;
}

extern "C" __declspec(dllexport) HWND DustBoot_GetHWND()
{
    return gCapturedHWND;
}

extern "C" __declspec(dllexport) bool DustBoot_IsHooked()
{
    return gHooked;
}

// ==================== Hook installation ====================

static bool InstallFactoryHooks()
{
    // Create a temporary device + swap chain to walk to the DXGI factory
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.lpszClassName = "DustBootDummy";
    wc.hInstance = GetModuleHandleA(nullptr);
    RegisterClassExA(&wc);
    HWND dummyWnd = CreateWindowExA(0, "DustBootDummy", "", WS_OVERLAPPED,
                                     0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummyWnd)
    {
        BootLog("ERROR: Failed to create dummy window (err=%lu)", GetLastError());
        UnregisterClassA("DustBootDummy", wc.hInstance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width = 1;
    scd.BufferDesc.Height = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummyWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    IDXGISwapChain* tmpSC = nullptr;
    ID3D11Device* tmpDev = nullptr;
    ID3D11DeviceContext* tmpCtx = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &tmpSC, &tmpDev, nullptr, &tmpCtx);

    if (FAILED(hr))
    {
        BootLog("ERROR: D3D11CreateDeviceAndSwapChain failed (hr=0x%08X)", (unsigned)hr);
        DestroyWindow(dummyWnd);
        UnregisterClassA("DustBootDummy", wc.hInstance);
        return false;
    }

    // Walk: Device → IDXGIDevice → IDXGIAdapter → IDXGIFactory
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;

    hr = tmpDev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr) || !dxgiDevice)
    {
        BootLog("ERROR: QueryInterface(IDXGIDevice) failed (hr=0x%08X)", (unsigned)hr);
        tmpSC->Release(); tmpCtx->Release(); tmpDev->Release();
        DestroyWindow(dummyWnd);
        UnregisterClassA("DustBootDummy", wc.hInstance);
        return false;
    }

    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter)
    {
        BootLog("ERROR: GetAdapter failed (hr=0x%08X)", (unsigned)hr);
        dxgiDevice->Release();
        tmpSC->Release(); tmpCtx->Release(); tmpDev->Release();
        DestroyWindow(dummyWnd);
        UnregisterClassA("DustBootDummy", wc.hInstance);
        return false;
    }

    hr = adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);
    if (FAILED(hr) || !factory)
    {
        BootLog("ERROR: GetParent(IDXGIFactory) failed (hr=0x%08X)", (unsigned)hr);
        adapter->Release(); dxgiDevice->Release();
        tmpSC->Release(); tmpCtx->Release(); tmpDev->Release();
        DestroyWindow(dummyWnd);
        UnregisterClassA("DustBootDummy", wc.hInstance);
        return false;
    }

    BootLog("Factory discovered at %p", factory);

    // Get CreateSwapChain address from factory vtable (index 10)
    void** factoryVtable = *reinterpret_cast<void***>(factory);
    void* addrCreateSwapChain = factoryVtable[10];
    BootLog("  CreateSwapChain address: %p", addrCreateSwapChain);

    // Get CreateSwapChainForHwnd address from IDXGIFactory2 vtable (index 15)
    void* addrCreateSwapChainForHwnd = nullptr;
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&factory2)) && factory2)
    {
        void** factory2Vtable = *reinterpret_cast<void***>(factory2);
        addrCreateSwapChainForHwnd = factory2Vtable[15];
        BootLog("  CreateSwapChainForHwnd address: %p", addrCreateSwapChainForHwnd);
        factory2->Release();
    }
    else
    {
        BootLog("  IDXGIFactory2 not available (pre-DXGI 1.2), skipping CreateSwapChainForHwnd");
    }

    // Clean up temporary objects before hooking
    factory->Release();
    adapter->Release();
    dxgiDevice->Release();
    tmpSC->Release();
    tmpCtx->Release();
    tmpDev->Release();
    DestroyWindow(dummyWnd);
    UnregisterClassA("DustBootDummy", wc.hInstance);

    // Install inline hooks via KenshiLib
    bool ok = true;

    if (KenshiLib::AddHook(addrCreateSwapChain, (void*)HookedCreateSwapChain,
                           (void**)&oCreateSwapChain) == KenshiLib::SUCCESS)
    {
        BootLog("  CreateSwapChain hook installed");
    }
    else
    {
        BootLog("ERROR: Failed to hook CreateSwapChain");
        ok = false;
    }

    if (addrCreateSwapChainForHwnd)
    {
        if (KenshiLib::AddHook(addrCreateSwapChainForHwnd, (void*)HookedCreateSwapChainForHwnd,
                               (void**)&oCreateSwapChainForHwnd) == KenshiLib::SUCCESS)
        {
            BootLog("  CreateSwapChainForHwnd hook installed");
        }
        else
        {
            BootLog("WARNING: Failed to hook CreateSwapChainForHwnd");
        }
    }

    return ok;
}

// ==================== Plugin entry point ====================

__declspec(dllexport) void startPlugin()
{
    BootLogInit();
    BootLog("DustBoot preload plugin starting...");

    if (InstallFactoryHooks())
    {
        gHooked = true;
        BootLog("Factory hooks installed, waiting for game to create swap chain...");
    }
    else
    {
        BootLog("ERROR: Failed to install factory hooks — Dust will fall back to runtime discovery");
    }
}

// ==================== DllMain ====================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        gDllModule = hModule;
        break;
    }
    return TRUE;
}
