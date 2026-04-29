#include "DustGUI_DInputHook.h"
#include "DustLog.h"
#include "imgui/imgui.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <core/Functions.h>

#include <cstring>

namespace DustGUI
{

// Externs from DustGUI.cpp
extern bool gInitialized;
extern bool gOverlayVisible;

// ==================== DirectInput8 Hook ====================
// Kenshi uses OIS which uses DirectInput8 — WM_ message blocking alone is not enough.

typedef HRESULT(WINAPI* PFN_GetDeviceState)(IDirectInputDevice8W* self, DWORD cbData, LPVOID lpvData);
typedef HRESULT(WINAPI* PFN_GetDeviceData)(IDirectInputDevice8W* self, DWORD cbObjectData,
    LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags);

// Keyboard hooks
static PFN_GetDeviceState oKbGetDeviceState = nullptr;
static PFN_GetDeviceData  oKbGetDeviceData  = nullptr;
// Mouse hooks (only used if vtable differs from keyboard)
static PFN_GetDeviceState oMouseGetDeviceState = nullptr;
static PFN_GetDeviceData  oMouseGetDeviceData  = nullptr;

// Stuck-key fix: track keyboard key state from buffered events
BYTE gKbTrackedState[256] = {};
bool gKbWasBlocking = false;

// Track which keys the game's WndProc has seen as pressed (WM_KEYDOWN reached it).
// Used to: (1) never eat a WM_KEYUP the game needs, (2) send compensating WM_KEYUPs on close.
BYTE gGameKeyHeld[256] = {};

static bool ShouldBlockDInput()
{
    return gInitialized && gOverlayVisible && ImGui::GetIO().WantCaptureMouse;
}

static HRESULT WINAPI HookedKbGetDeviceState(IDirectInputDevice8W* self, DWORD cbData, LPVOID lpvData)
{
    HRESULT hr = oKbGetDeviceState(self, cbData, lpvData);
    if (SUCCEEDED(hr) && lpvData && ShouldBlockDInput())
        memset(lpvData, 0, cbData);
    return hr;
}

static HRESULT WINAPI HookedMouseGetDeviceState(IDirectInputDevice8W* self, DWORD cbData, LPVOID lpvData)
{
    HRESULT hr = oMouseGetDeviceState(self, cbData, lpvData);
    if (SUCCEEDED(hr) && lpvData && ShouldBlockDInput())
        memset(lpvData, 0, cbData);
    return hr;
}

static HRESULT WINAPI HookedKbGetDeviceData(IDirectInputDevice8W* self, DWORD cbObjectData,
    LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags)
{
    DWORD capacity = (pdwInOut && rgdod) ? *pdwInOut : 0;
    HRESULT hr = oKbGetDeviceData(self, cbObjectData, rgdod, pdwInOut, dwFlags);
    if (!SUCCEEDED(hr) || !pdwInOut) return hr;

    // Track keyboard state from buffered events (dwData: 0x80=press, 0x00=release)
    if (rgdod)
    {
        for (DWORD i = 0; i < *pdwInOut; i++)
        {
            if (rgdod[i].dwOfs < 256 &&
                (rgdod[i].dwData == 0x00 || rgdod[i].dwData == 0x80))
                gKbTrackedState[rgdod[i].dwOfs] = (BYTE)rgdod[i].dwData;
        }
    }

    bool blocking = ShouldBlockDInput();

    if (blocking && !gKbWasBlocking)
    {
        // Transition IN: inject key-up events for all held keys
        // Skip if rgdod is NULL (buffer flush) — retry next call
        if (!rgdod || capacity == 0)
            return hr;

        DWORD count = 0;
        for (DWORD i = 0; i < 256 && count < capacity; i++)
        {
            if (gKbTrackedState[i] & 0x80)
            {
                rgdod[count].dwOfs       = i;
                rgdod[count].dwData      = 0; // key up
                rgdod[count].dwTimeStamp = GetTickCount();
                rgdod[count].dwSequence  = 0;
                count++;
            }
        }
        // Don't clear gKbTrackedState here — we still need real state for transition out
        *pdwInOut = count;
        gKbWasBlocking = true;
    }
    else if (!blocking && gKbWasBlocking)
    {
        // Transition OUT: inject key-down events for keys still physically held
        // so the game picks up current state immediately
        if (!rgdod || capacity == 0)
        {
            gKbWasBlocking = false;
            return hr;
        }

        DWORD realCount = *pdwInOut;
        DWORD count = realCount;
        for (DWORD i = 0; i < 256 && count < capacity; i++)
        {
            if (gKbTrackedState[i] & 0x80)
            {
                rgdod[count].dwOfs       = i;
                rgdod[count].dwData      = 0x80; // key down
                rgdod[count].dwTimeStamp = GetTickCount();
                rgdod[count].dwSequence  = 0;
                count++;
            }
        }
        *pdwInOut = count;
        gKbWasBlocking = false;
    }
    else if (blocking)
    {
        *pdwInOut = 0;
    }

    return hr;
}

static HRESULT WINAPI HookedMouseGetDeviceData(IDirectInputDevice8W* self, DWORD cbObjectData,
    LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags)
{
    HRESULT hr = oMouseGetDeviceData(self, cbObjectData, rgdod, pdwInOut, dwFlags);
    if (SUCCEEDED(hr) && pdwInOut && ShouldBlockDInput())
        *pdwInOut = 0;
    return hr;
}

bool InstallDInputHooks()
{
    HMODULE hDInput = GetModuleHandleA("dinput8.dll");
    if (!hDInput)
        hDInput = LoadLibraryA("dinput8.dll");
    if (!hDInput) { Log("GUI: dinput8.dll not found"); return false; }

    typedef HRESULT(WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto pDirectInput8Create = (PFN_DirectInput8Create)GetProcAddress(hDInput, "DirectInput8Create");
    if (!pDirectInput8Create) { Log("GUI: DirectInput8Create not found"); return false; }

    IDirectInput8W* di = nullptr;
    HRESULT hr = pDirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION,
                                      IID_IDirectInput8W, (void**)&di, nullptr);
    if (FAILED(hr) || !di) { Log("GUI: DirectInput8Create failed: 0x%08X", hr); return false; }

    // Get keyboard vtable
    IDirectInputDevice8W* kbDev = nullptr;
    hr = di->CreateDevice(GUID_SysKeyboard, &kbDev, nullptr);
    if (FAILED(hr) || !kbDev) { di->Release(); Log("GUI: CreateDevice(keyboard) failed: 0x%08X", hr); return false; }

    void** kbVtable = *reinterpret_cast<void***>(kbDev);
    void* kbAddrState = kbVtable[9];
    void* kbAddrData  = kbVtable[10];
    kbDev->Release();

    // Get mouse vtable
    IDirectInputDevice8W* mouseDev = nullptr;
    hr = di->CreateDevice(GUID_SysMouse, &mouseDev, nullptr);
    if (FAILED(hr) || !mouseDev) { di->Release(); Log("GUI: CreateDevice(mouse) failed: 0x%08X", hr); return false; }

    void** mouseVtable = *reinterpret_cast<void***>(mouseDev);
    void* mouseAddrState = mouseVtable[9];
    void* mouseAddrData  = mouseVtable[10];
    mouseDev->Release();

    di->Release();

    bool ok = true;

    // Hook keyboard
    if (KenshiLib::AddHook(kbAddrState, (void*)HookedKbGetDeviceState,
                           (void**)&oKbGetDeviceState) != KenshiLib::SUCCESS)
    { Log("GUI: Failed to hook keyboard GetDeviceState"); ok = false; }

    if (KenshiLib::AddHook(kbAddrData, (void*)HookedKbGetDeviceData,
                           (void**)&oKbGetDeviceData) != KenshiLib::SUCCESS)
    { Log("GUI: Failed to hook keyboard GetDeviceData"); ok = false; }

    // Hook mouse (skip if same vtable entries — already covered by keyboard hooks)
    if (mouseAddrState != kbAddrState)
    {
        if (KenshiLib::AddHook(mouseAddrState, (void*)HookedMouseGetDeviceState,
                               (void**)&oMouseGetDeviceState) != KenshiLib::SUCCESS)
        { Log("GUI: Failed to hook mouse GetDeviceState"); ok = false; }
    }
    else
    {
        Log("GUI: Mouse shares keyboard GetDeviceState — already hooked");
    }

    if (mouseAddrData != kbAddrData)
    {
        if (KenshiLib::AddHook(mouseAddrData, (void*)HookedMouseGetDeviceData,
                               (void**)&oMouseGetDeviceData) != KenshiLib::SUCCESS)
        { Log("GUI: Failed to hook mouse GetDeviceData"); ok = false; }
    }
    else
    {
        Log("GUI: Mouse shares keyboard GetDeviceData — already hooked");
    }

    if (ok)
        Log("GUI: DirectInput8 hooks installed (keyboard + mouse input blocking enabled)");
    return ok;
}

} // namespace DustGUI

