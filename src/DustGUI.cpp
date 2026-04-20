#include "DustGUI.h"
#include "DustLog.h"
#include "EffectLoader.h"
#include "Survey.h"
#include "SurveyRecorder.h"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "discord_logo.h"
#include "github_logo.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdio>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <shellapi.h>
#include <core/Functions.h>

// Version string (DUST_VERSION is injected at build time by CI from git tags)
#define DUST_STR2(x) #x
#define DUST_STR(x) DUST_STR2(x)
#ifdef DUST_VERSION
static const char* DUST_VERSION_STR = "v" DUST_STR(DUST_VERSION);
#else
static const char* DUST_VERSION_STR = "dev";
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace DustGUI
{

// ==================== Types ====================

union SavedValue {
    bool bVal;
    float fVal;
    int iVal;
    float f3Val[3];   // DUST_SETTING_COLOR3
};

struct EffectGUIState {
    bool snapshotted = false;
    std::vector<SavedValue> diskValues;
};

struct FrameworkConfig {
    bool logging = false;
    bool showStartupMessage = true;
    std::string lastPreset;    // name of last selected preset (empty = custom)
    int toggleKey = VK_F11;    // virtual key code for overlay toggle
};

static const char* VKKeyName(int vk)
{
    static char buf[64];
    UINT scanCode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    // Extended keys need the flag set
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT:
        case VK_UP: case VK_DOWN:
            scanCode |= 0x100;
            break;
    }
    if (GetKeyNameTextA(scanCode << 16, buf, sizeof(buf)) > 0)
        return buf;
    snprintf(buf, sizeof(buf), "Key 0x%02X", vk);
    return buf;
}

static bool gWaitingForKey = false;

// ==================== State ====================

static bool gInitialized = false;
static bool gOverlayVisible = false;
static volatile bool gResizeInProgress = false;
static HWND gHWnd = nullptr;
static WNDPROC oWndProc = nullptr;
static ID3D11Device* gDevice = nullptr;
static ID3D11DeviceContext* gContext = nullptr;
static ID3D11RenderTargetView* gBackBufferRTV = nullptr;
static ID3D11ShaderResourceView* gDiscordLogoSRV = nullptr;
static ID3D11ShaderResourceView* gGithubLogoSRV  = nullptr;

// Per-effect GUI state (saved values for reset)
static std::vector<EffectGUIState> gEffectStates;

// Framework config
static FrameworkConfig gFwConfig;
static FrameworkConfig gFwDiskConfig;
static std::string gDustIniPath;

// Startup toast
static float gToastTimer = 30.0f;
static bool gToastActive = true;
static bool gNewVersionInstalled = false;

// Cursor state when overlay opens
static RECT gSavedClipRect = {};
static bool gHadClipRect = false;

// Performance tracking
static float gFrameTimes[120] = {};
static int gFrameTimeIdx = 0;
static float gFpsAccum = 0.0f;
static int gFpsCount = 0;
static float gDisplayFps = 0.0f;

// Global effects toggle
static bool gAllEffectsOn = true;
static std::vector<bool> gEffectWasEnabled; // remembers which effects were on before global disable

// Preset system GUI state
static char gNewPresetName[64] = {};

// Double-click to input mode
static ImGuiID gInputModeID = 0;
static int gInputModeFrames = 0;

// ==================== Helpers ====================

static SavedValue GetValue(const DustSettingDesc& s)
{
    SavedValue v = {};
    if (!s.valuePtr) return v;
    switch (s.type) {
    case DUST_SETTING_BOOL:   v.bVal = *(bool*)s.valuePtr; break;
    case DUST_SETTING_FLOAT:  v.fVal = *(float*)s.valuePtr; break;
    case DUST_SETTING_INT:
    case DUST_SETTING_ENUM:   v.iVal = *(int*)s.valuePtr; break;
    case DUST_SETTING_COLOR3: {
        const float* src = (const float*)s.valuePtr;
        v.f3Val[0] = src[0]; v.f3Val[1] = src[1]; v.f3Val[2] = src[2];
        break;
    }
    default: break;
    }
    return v;
}

static void SetValue(const DustSettingDesc& s, const SavedValue& v)
{
    if (!s.valuePtr) return;
    switch (s.type) {
    case DUST_SETTING_BOOL:   *(bool*)s.valuePtr = v.bVal; break;
    case DUST_SETTING_FLOAT:  *(float*)s.valuePtr = v.fVal; break;
    case DUST_SETTING_INT:
    case DUST_SETTING_ENUM:   *(int*)s.valuePtr = v.iVal; break;
    case DUST_SETTING_COLOR3: {
        float* dst = (float*)s.valuePtr;
        dst[0] = v.f3Val[0]; dst[1] = v.f3Val[1]; dst[2] = v.f3Val[2];
        break;
    }
    default: break;
    }
}

static bool IsDirty(const DustSettingDesc& s, const SavedValue& saved)
{
    if (!s.valuePtr) return false;
    switch (s.type) {
    case DUST_SETTING_BOOL:   return *(bool*)s.valuePtr != saved.bVal;
    case DUST_SETTING_FLOAT:  return *(float*)s.valuePtr != saved.fVal;
    case DUST_SETTING_INT:
    case DUST_SETTING_ENUM:   return *(int*)s.valuePtr != saved.iVal;
    case DUST_SETTING_COLOR3: {
        const float* cur = (const float*)s.valuePtr;
        return cur[0] != saved.f3Val[0] || cur[1] != saved.f3Val[1] || cur[2] != saved.f3Val[2];
    }
    default: break;
    }
    return false;
}

static void SnapshotEffect(size_t idx)
{
    const LoadedEffect& le = gEffectLoader.GetEffect(idx);
    if (idx >= gEffectStates.size())
        gEffectStates.resize(idx + 1);

    auto& state = gEffectStates[idx];
    state.diskValues.resize(le.desc.settingCount);
    for (uint32_t i = 0; i < le.desc.settingCount; i++)
        state.diskValues[i] = GetValue(le.desc.settings[i]);
    state.snapshotted = true;
}

// ==================== Effect enabled helpers ====================

// Find the "Enabled" bool pointer in an effect's settings array (first DUST_SETTING_BOOL)
static bool* FindEnabledPtr(const LoadedEffect& le)
{
    for (uint32_t i = 0; i < le.desc.settingCount; i++)
    {
        const DustSettingDesc& s = le.desc.settings[i];
        if (s.type == DUST_SETTING_BOOL && s.valuePtr)
            return (bool*)s.valuePtr;
    }
    return nullptr;
}

static bool IsEffectEnabled(const LoadedEffect& le)
{
    bool* p = FindEnabledPtr(le);
    return p ? *p : true; // default to true if no enabled setting found
}

// ==================== Custom slider with double-click-to-input ====================

static bool DustSliderFloat(const char* label, float* v, float minVal, float maxVal)
{
    ImGuiID id = ImGui::GetID(label);

    if (gInputModeID == id)
    {
        if (gInputModeFrames == 0)
            ImGui::SetKeyboardFocusHere();
        gInputModeFrames++;

        ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
        bool changed = ImGui::InputFloat(label, v, 0.0f, 0.0f, "%.4f");

        if (*v < minVal) *v = minVal;
        if (*v > maxVal) *v = maxVal;

        // Exit input mode when focus is lost (skip first 2 frames for activation)
        if (gInputModeFrames > 2 && !ImGui::IsItemActive())
        {
            gInputModeID = 0;
            gInputModeFrames = 0;
        }

        return changed;
    }

    bool changed = ImGui::SliderFloat(label, v, minVal, maxVal);

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        gInputModeID = id;
        gInputModeFrames = 0;
    }

    return changed;
}

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
static BYTE gKbTrackedState[256] = {};
static bool gKbWasBlocking = false;

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

static bool InstallDInputHooks()
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

// ==================== WndProc ====================

static void OnOverlayOpen(HWND hWnd)
{
    gToastActive = false;

    // Save cursor clip rect and release it so the cursor can move freely
    gHadClipRect = (GetClipCursor(&gSavedClipRect) != 0);
    ClipCursor(nullptr);

    // Show system cursor
    while (ShowCursor(TRUE) < 0) {}

    // Enable ImGui software cursor (renders on top of everything)
    ImGui::GetIO().MouseDrawCursor = true;
}

static void OnOverlayClose()
{
    ImGuiIO& io = ImGui::GetIO();

    // Disable ImGui software cursor
    io.MouseDrawCursor = false;

    // Clear all key/mouse state so nothing stays stuck in ImGui
    memset(io.KeysDown, 0, sizeof(io.KeysDown));
    memset(io.MouseDown, 0, sizeof(io.MouseDown));
    io.KeyCtrl = io.KeyShift = io.KeyAlt = io.KeySuper = false;

    // Restore cursor clipping (game usually clips cursor to window)
    if (gHadClipRect)
        ClipCursor(&gSavedClipRect);

    // Hide system cursor (game manages its own)
    while (ShowCursor(FALSE) >= 0) {}
}

static LRESULT CALLBACK DustWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Capture key binding when waiting for a new toggle key
    // WM_SYSKEYDOWN is required for F10 (Windows treats it as a menu-activation key)
    if (gWaitingForKey && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN))
    {
        if (wParam != VK_ESCAPE) // Escape cancels
            gFwConfig.toggleKey = (int)wParam;
        gWaitingForKey = false;
        return 0;
    }

    bool isToggleMsg = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
                       (int)wParam == gFwConfig.toggleKey && !(lParam & 0x40000000);
    if (isToggleMsg)
    {
        gOverlayVisible = !gOverlayVisible;
        if (gOverlayVisible)
            OnOverlayOpen(hWnd);
        else
            OnOverlayClose();
        return 0;
    }

    if (gInitialized && gOverlayVisible)
    {
        // Keep cursor visible — game tries to hide it via WM_SETCURSOR
        if (msg == WM_SETCURSOR)
        {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }

        // Keep cursor unclipped (game may re-clip it each frame)
        if (msg == WM_MOUSEMOVE)
            ClipCursor(nullptr);

        // Let ImGui process the message for its own UI state
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

        // Block all game input when the mouse is hovering over the GUI
        bool isMouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
        bool isKeyboard = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST);
        if ((isMouse || isKeyboard || msg == WM_INPUT) && ImGui::GetIO().WantCaptureMouse)
            return 0;
    }
    else if (gInitialized)
    {
        // Overlay is closed — let ImGui handle only non-input messages
        // (e.g. WM_DISPLAYCHANGE, WM_DEVICECHANGE) without consuming game input
        if (msg != WM_INPUT &&
            !(msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) &&
            !(msg >= WM_KEYFIRST && msg <= WM_KEYLAST))
        {
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        }
    }

    return CallWindowProcW(oWndProc, hWnd, msg, wParam, lParam);
}

// ==================== Back buffer ====================

static bool CreateBackBufferRTV(IDXGISwapChain* swapChain)
{
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) return false;

    hr = gDevice->CreateRenderTargetView(backBuffer, nullptr, &gBackBufferRTV);
    backBuffer->Release();
    return SUCCEEDED(hr);
}

// ==================== Framework config ====================

static void LoadFrameworkConfig()
{
    gDustIniPath = DustLogDir() + "Dust.ini";
    gFwConfig.logging = GetPrivateProfileIntA("Dust", "Logging", 0, gDustIniPath.c_str()) != 0;
    gFwConfig.showStartupMessage = GetPrivateProfileIntA("Dust", "ShowStartupMessage", 1, gDustIniPath.c_str()) != 0;

    gFwConfig.toggleKey = GetPrivateProfileIntA("Dust", "ToggleKey", VK_F11, gDustIniPath.c_str());

    char buf[256] = {};
    GetPrivateProfileStringA("Dust", "LastPreset", "", buf, sizeof(buf), gDustIniPath.c_str());
    gFwConfig.lastPreset = buf;

    // Default to dust_high if no preset has been saved yet
    if (gFwConfig.lastPreset.empty())
        gFwConfig.lastPreset = "dust_high";

    // Auto-load last preset
    {
        const auto& presets = gEffectLoader.GetPresets();
        for (int i = 0; i < (int)presets.size(); i++)
        {
            if (presets[i].name == gFwConfig.lastPreset)
            {
                gEffectLoader.LoadPreset(i);
                break;
            }
        }
    }

    // Detect version change (Steam Workshop overwrites DLL but not Dust.ini)
    char lastVersion[128] = {};
    GetPrivateProfileStringA("Dust", "LastSeenVersion", "", lastVersion, sizeof(lastVersion), gDustIniPath.c_str());
    if (strcmp(lastVersion, DUST_VERSION_STR) != 0)
    {
        // First launch or version changed — update the stored version
        gNewVersionInstalled = (lastVersion[0] != '\0'); // only show "new version" if there was a previous one
        WritePrivateProfileStringA("Dust", "LastSeenVersion", DUST_VERSION_STR, gDustIniPath.c_str());
        if (gNewVersionInstalled)
            gToastActive = true; // always show toast on update, even if disabled
    }

    gFwDiskConfig = gFwConfig;
    if (!gNewVersionInstalled)
        gToastActive = gFwConfig.showStartupMessage;
}

static void SaveFrameworkConfig()
{
    WritePrivateProfileStringA("Dust", "Logging", gFwConfig.logging ? "1" : "0", gDustIniPath.c_str());
    WritePrivateProfileStringA("Dust", "ShowStartupMessage", gFwConfig.showStartupMessage ? "1" : "0", gDustIniPath.c_str());
    char keyBuf[16];
    snprintf(keyBuf, sizeof(keyBuf), "%d", gFwConfig.toggleKey);
    WritePrivateProfileStringA("Dust", "ToggleKey", keyBuf, gDustIniPath.c_str());
    WritePrivateProfileStringA("Dust", "LastPreset", gFwConfig.lastPreset.c_str(), gDustIniPath.c_str());
    gFwDiskConfig = gFwConfig;
    // Apply logging change immediately
    DustLogEnabled() = gFwConfig.logging;
    Log("Framework settings saved");
}

static void ResetFrameworkConfig()
{
    gFwConfig = gFwDiskConfig;
    DustLogEnabled() = gFwConfig.logging;
}

static bool IsFrameworkDirty()
{
    return gFwConfig.logging != gFwDiskConfig.logging ||
           gFwConfig.showStartupMessage != gFwDiskConfig.showStartupMessage ||
           gFwConfig.lastPreset != gFwDiskConfig.lastPreset ||
           gFwConfig.toggleKey != gFwDiskConfig.toggleKey;
}

// ==================== Drawing: Framework pane ====================

static void DrawFrameworkSection()
{
    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "Dust");
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", DUST_VERSION_STR);
    ImGui::Separator();
    ImGui::Spacing();

    // Toggle key binding
    if (gWaitingForKey)
    {
        ImGui::Button("Press a key...", ImVec2(ImGui::GetContentRegionAvail().x, 0));
    }
    else
    {
        char label[128];
        snprintf(label, sizeof(label), "Toggle: %s", VKKeyName(gFwConfig.toggleKey));
        if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, 0)))
            gWaitingForKey = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click to rebind the overlay toggle key");
    }

    ImGui::Spacing();

    ImGui::Checkbox("Logging", &gFwConfig.logging);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Write timestamped logs to logs/ folder next to the DLL");

    ImGui::Checkbox("Startup Message", &gFwConfig.showStartupMessage);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show notification on game start");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool dirty = IsFrameworkDirty();

    if (dirty)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    if (ImGui::Button("Save##fw", ImVec2(80, 0)) && dirty)
        SaveFrameworkConfig();
    if (dirty)
        ImGui::PopStyleColor();
    if (ImGui::IsItemHovered() && !dirty)
        ImGui::SetTooltip("No changes to save");

    ImGui::SameLine();
    if (ImGui::Button("Reset##fw", ImVec2(80, 0)) && dirty)
        ResetFrameworkConfig();

    // ---- Pipeline Survey ----
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Pipeline Survey");

    if (Survey::IsActive())
    {
        ImGui::TextWrapped("Capturing frame %d / %d...",
                           Survey::CurrentFrame() + 1, Survey::TotalFrames());
        if (ImGui::Button("Stop Survey", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
            Survey::Stop();
    }
    else
    {
        static int  surveyFrames = 3;
        static int  surveyDetail = 1;
        static char surveyLabel[64] = "";
        ImGui::InputText("Label##survey", surveyLabel, sizeof(surveyLabel));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Optional label for this capture (e.g. desert_night, hub_city)");
        ImGui::SliderInt("Frames##survey", &surveyFrames, 1, 30);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Number of frames to capture");
        ImGui::SliderInt("Detail##survey", &surveyDetail, 0, 3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0=Minimal, 1=Standard, 2=Deep (CB data), 3=Full (VB/IB)");
        if (ImGui::Button("Capture Survey", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
        {
            SurveyRecorder::Reset();
            Survey::Start(surveyFrames, surveyDetail,
                          surveyLabel[0] ? surveyLabel : nullptr);
        }
    }
}

// ==================== Drawing: Global preset selector ====================

static void SnapshotAllEffects()
{
    size_t count = gEffectLoader.Count();
    for (size_t i = 0; i < count; i++)
    {
        const LoadedEffect& le = gEffectLoader.GetEffect(i);
        if (le.initialized)
            SnapshotEffect(i);
    }
}

static void DrawPresetSection()
{
    const auto& presets = gEffectLoader.GetPresets();
    int currentPreset = gEffectLoader.GetCurrentPreset();

    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "Preset");
    ImGui::Separator();
    ImGui::Spacing();

    const char* previewName = (currentPreset >= 0 && currentPreset < (int)presets.size())
        ? presets[currentPreset].name.c_str() : "(Custom)";

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##Preset", previewName))
    {
        if (ImGui::Selectable("(Custom)", currentPreset < 0))
        {
            gEffectLoader.SetCurrentPreset(-1);
            gFwConfig.lastPreset.clear();
            SaveFrameworkConfig();
        }

        for (int p = 0; p < (int)presets.size(); p++)
        {
            bool selected = (p == currentPreset);
            if (ImGui::Selectable(presets[p].name.c_str(), selected))
            {
                gEffectLoader.LoadPreset(p);
                gFwConfig.lastPreset = presets[p].name;
                SaveFrameworkConfig();
                SnapshotAllEffects();
            }
        }
        ImGui::EndCombo();
    }

    // Show warnings for outdated presets
    if (currentPreset >= 0 && !presets[currentPreset].warnings.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[!] Preset is outdated");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", presets[currentPreset].warnings.c_str());
    }

    // Save As
    if (ImGui::Button("Save As...", ImVec2(0, 0)))
        ImGui::OpenPopup("##GlobalSavePresetAs");

    if (ImGui::BeginPopup("##GlobalSavePresetAs"))
    {
        ImGui::Text("Save all settings as preset:");
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("##presetname", gNewPresetName, sizeof(gNewPresetName));

        bool nameValid = gNewPresetName[0] != '\0';
        if (ImGui::Button("Save", ImVec2(80, 0)) && nameValid)
        {
            gEffectLoader.SavePresetAs(gNewPresetName);
            gNewPresetName[0] = '\0';
            SnapshotAllEffects();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Overwrite / Delete for active preset
    if (currentPreset >= 0)
    {
        ImGui::SameLine();
        if (ImGui::Button("Save", ImVec2(0, 0)))
        {
            gEffectLoader.SavePreset(currentPreset);
            SnapshotAllEffects();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save current settings into '%s'", presets[currentPreset].name.c_str());

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Delete"))
            ImGui::OpenPopup("##ConfirmDeletePreset");
        ImGui::PopStyleColor();

        if (ImGui::BeginPopup("##ConfirmDeletePreset"))
        {
            ImGui::Text("Delete preset '%s'?", presets[currentPreset].name.c_str());
            if (ImGui::Button("Yes", ImVec2(60, 0)))
            {
                gEffectLoader.DeletePreset(currentPreset);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("No", ImVec2(60, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

// ==================== Drawing: Effect settings ====================

static void DrawResetButton(size_t effectIdx, uint32_t settingIdx)
{
    const LoadedEffect& le = gEffectLoader.GetEffect(effectIdx);
    const DustSettingDesc& s = le.desc.settings[settingIdx];
    auto& state = gEffectStates[effectIdx];

    bool dirty = IsDirty(s, state.diskValues[settingIdx]);

    ImGui::SameLine();
    ImGui::PushID((int)settingIdx + 10000);
    if (dirty)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
        if (ImGui::SmallButton("R"))
        {
            SetValue(s, state.diskValues[settingIdx]);
            if (le.desc.OnSettingChanged)
                le.desc.OnSettingChanged();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reset to saved value");
    }
    else
    {
        ImGui::TextDisabled("  ");
    }
    ImGui::PopID();
}

static void DrawEffectSection(size_t idx)
{
    const LoadedEffect& le = gEffectLoader.GetEffect(idx);
    if (!le.initialized) return;

    if (idx >= gEffectStates.size())
        gEffectStates.resize(idx + 1);
    if (!gEffectStates[idx].snapshotted)
        SnapshotEffect(idx);

    const char* name = le.desc.name ? le.desc.name : "Unnamed";

    // Build header label with enabled status
    bool enabled = IsEffectEnabled(le);
    char headerLabel[256];
    snprintf(headerLabel, sizeof(headerLabel), "%s  %s",
             name, enabled ? "[ON]" : "[OFF]");

    // Color the header text
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
    bool open = ImGui::CollapsingHeader(headerLabel, ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor();

    if (!open)
        return;

    ImGui::Spacing();

    if (le.desc.apiVersion < 2 || !le.desc.settings || le.desc.settingCount == 0)
    {
        ImGui::TextDisabled("No configurable settings");
        return;
    }

    bool anyChanged = false;

    for (uint32_t i = 0; i < le.desc.settingCount; i++)
    {
        const DustSettingDesc& s = le.desc.settings[i];
        if (!s.name) continue;

        // Skip hidden settings (persisted in INI but not shown in GUI)
        if (s.type == DUST_SETTING_HIDDEN_FLOAT || s.type == DUST_SETTING_HIDDEN_INT
            || s.type == DUST_SETTING_HIDDEN_BOOL)
            continue;

        // Section: visual-only group header, no value, no reset button
        if (s.type == DUST_SETTING_SECTION)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.45f, 1.0f));
            ImGui::TextUnformatted(s.name);
            ImGui::PopStyleColor();
            ImGui::Separator();
            continue;
        }

        if (!s.valuePtr) continue;

        ImGui::PushID((int)i);

        bool changed = false;
        switch (s.type)
        {
        case DUST_SETTING_BOOL:
            changed = ImGui::Checkbox(s.name, (bool*)s.valuePtr);
            break;
        case DUST_SETTING_FLOAT:
            changed = DustSliderFloat(s.name, (float*)s.valuePtr, s.minVal, s.maxVal);
            break;
        case DUST_SETTING_INT:
            changed = ImGui::SliderInt(s.name, (int*)s.valuePtr, (int)s.minVal, (int)s.maxVal);
            break;
        case DUST_SETTING_ENUM:
        {
            int count = 0;
            if (s.enumLabels)
                while (s.enumLabels[count]) count++;
            int* v = (int*)s.valuePtr;
            if (count > 0)
            {
                if (*v < 0) *v = 0;
                if (*v >= count) *v = count - 1;
                const char* preview = s.enumLabels[*v];
                if (ImGui::BeginCombo(s.name, preview))
                {
                    for (int n = 0; n < count; n++)
                    {
                        bool selected = (*v == n);
                        if (ImGui::Selectable(s.enumLabels[n], selected))
                        {
                            if (*v != n) { *v = n; changed = true; }
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            break;
        }
        case DUST_SETTING_COLOR3:
        {
            float* col = (float*)s.valuePtr;
            // HDR allows out-of-[0,1] values so we can reuse this widget for ASC-CDL-style
            // triplets (lift/gamma/gain/offset/slope) that go negative or above 1.
            changed = ImGui::ColorEdit3(s.name, col,
                ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            if (changed && s.minVal < s.maxVal)
            {
                for (int c = 0; c < 3; c++)
                    col[c] = (col[c] < s.minVal) ? s.minVal : (col[c] > s.maxVal ? s.maxVal : col[c]);
            }
            break;
        }
        default:
            break;
        }

        // Reset button for this parameter
        DrawResetButton(idx, i);

        if (changed) anyChanged = true;
        ImGui::PopID();
    }

    if (anyChanged && le.desc.OnSettingChanged)
        le.desc.OnSettingChanged();
}

// ==================== Drawing: Performance ====================

static void DrawPerformanceSection()
{
    ImGuiIO& io = ImGui::GetIO();

    // Update frame time history
    float frameTime = io.DeltaTime * 1000.0f;
    gFrameTimes[gFrameTimeIdx] = frameTime;
    gFrameTimeIdx = (gFrameTimeIdx + 1) % 120;

    // Smooth FPS (update every 0.5s)
    gFpsAccum += io.DeltaTime;
    gFpsCount++;
    if (gFpsAccum >= 0.5f)
    {
        gDisplayFps = (float)gFpsCount / gFpsAccum;
        gFpsAccum = 0.0f;
        gFpsCount = 0;
    }

    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "Performance");
    ImGui::Separator();
    ImGui::Spacing();

    // FPS and frame time
    ImGui::Text("FPS: %.1f", gDisplayFps);
    ImGui::Text("Frame Time: %.2f ms", frameTime);

    ImGui::Spacing();

    // Frame time graph
    ImGui::Text("Frame Time History:");
    float maxMs = 0.0f;
    for (int i = 0; i < 120; i++)
        if (gFrameTimes[i] > maxMs) maxMs = gFrameTimes[i];
    if (maxMs < 16.67f) maxMs = 16.67f; // at least 60fps scale

    ImGui::PlotLines("##frametime", gFrameTimes, 120, gFrameTimeIdx,
                     nullptr, 0.0f, maxMs * 1.2f, ImVec2(0, 60));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Per-effect GPU timing
    ImGui::Text("Effect GPU Times:");
    ImGui::Spacing();

    float totalEffectMs = 0.0f;
    size_t count = gEffectLoader.Count();
    for (size_t i = 0; i < count; i++)
    {
        const LoadedEffect& le = gEffectLoader.GetEffect(i);
        if (!le.initialized) continue;

        const char* name = le.desc.name ? le.desc.name : "Unnamed";
        float gpuMs = gEffectLoader.GetEffectGpuTime(i);
        bool hasTiming = (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_TIMING))
                      || le.desc.gpuTimeMsPtr;

        totalEffectMs += gpuMs;

        // Color based on cost
        ImVec4 color = ImVec4(0.4f, 1.0f, 0.4f, 1.0f); // green
        if (gpuMs > 2.0f) color = ImVec4(1.0f, 1.0f, 0.4f, 1.0f); // yellow
        if (gpuMs > 5.0f) color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // red

        if (hasTiming)
        {
            ImGui::TextColored(color, "  %-12s %.2f ms", name, gpuMs);
        }
        else
        {
            ImGui::TextDisabled("  %-12s (no timing)", name);
        }
    }

    if (count > 0 && totalEffectMs > 0.0f)
    {
        ImGui::Spacing();
        ImGui::Text("  Total Effects: %.2f ms", totalEffectMs);
        ImGui::Text("  Effect Budget: %.1f%% of frame", totalEffectMs / frameTime * 100.0f);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Resolution info
    ImGui::TextDisabled("Resolution info from last frame context");
    // We don't have direct access to resolution here, but we can show display size
    ImGui::Text("Display: %.0fx%.0f", io.DisplaySize.x, io.DisplaySize.y);
}

// ==================== Startup toast ====================

static void RenderToast()
{
    if (!gToastActive || gToastTimer <= 0.0f || gOverlayVisible)
        return;

    gToastTimer -= ImGui::GetIO().DeltaTime;
    if (gToastTimer <= 0.0f) { gToastActive = false; return; }

    float alpha = gToastTimer < 3.0f ? gToastTimer / 3.0f : 1.0f;

    ImGui::SetNextWindowPos(ImVec2(10, 10));
    ImGui::SetNextWindowBgAlpha(0.75f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

    ImGui::Begin("##DustToast", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "Dust");
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", DUST_VERSION_STR);
    if (gNewVersionInstalled)
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "New version installed!");
    ImGui::Text("Press %s to open settings", VKKeyName(gFwConfig.toggleKey));
    if (!gNewVersionInstalled)
        ImGui::TextDisabled("(disable this message in settings)");

    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ==================== Discord logo texture ====================

static ID3D11ShaderResourceView* LoadTextureFromMemory(const unsigned char* data, int dataSize)
{
    int w, h, n;
    unsigned char* pixels = stbi_load_from_memory(data, dataSize, &w, &h, &n, 4);
    if (!pixels) return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = (UINT)w;
    desc.Height           = (UINT)h;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem     = pixels;
    init.SysMemPitch = (UINT)(w * 4);

    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(gDevice->CreateTexture2D(&desc, &init, &tex)))
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format              = desc.Format;
        srvDesc.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        gDevice->CreateShaderResourceView(tex, &srvDesc, &srv);
        tex->Release();
    }
    stbi_image_free(pixels);
    return srv;
}

static void LoadLogoTextures()
{
    gDiscordLogoSRV = LoadTextureFromMemory(kDiscordLogoPng, (int)kDiscordLogoPngSize);
    gGithubLogoSRV  = LoadTextureFromMemory(kGithubLogoPng,  (int)kGithubLogoPngSize);
}

// ==================== Init / Shutdown / Render ====================

bool Init(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (gInitialized) return true;  // Already initialized (e.g. ResizeBuffers ran before first Present)

    Log("GUI: Init starting (swap=%p, dev=%p, ctx=%p)", swapChain, device, context);

    if (!swapChain || !device || !context)
    { Log("GUI: Init aborted — null swap/device/context"); return false; }

    gDevice = device;
    gContext = context;

    // Sanity: the swap chain must belong to the same device we'll render with,
    // otherwise CreateRenderTargetView on its back buffer fails with E_INVALIDARG.
    // This happens when Kenshi creates extra swap chains on a different device.
    {
        ID3D11Device* scDevice = nullptr;
        HRESULT hr = swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&scDevice);
        if (FAILED(hr) || !scDevice)
        { Log("GUI: swapChain->GetDevice failed hr=0x%08X", (unsigned)hr); if (scDevice) scDevice->Release(); return false; }
        bool match = (scDevice == gDevice);
        scDevice->Release();
        if (!match)
        { Log("GUI: swapChain device (%p) != captured device (%p) — skipping this swap chain", scDevice, gDevice); return false; }
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    HRESULT hr = swapChain->GetDesc(&desc);
    if (FAILED(hr))
    { Log("GUI: swapChain->GetDesc failed hr=0x%08X", (unsigned)hr); return false; }
    gHWnd = desc.OutputWindow;
    Log("GUI: hwnd=%p, size=%ux%u, format=%d", gHWnd, desc.BufferDesc.Width, desc.BufferDesc.Height, (int)desc.BufferDesc.Format);
    if (!gHWnd) { Log("GUI: No HWND"); return false; }
    if (!IsWindow(gHWnd)) { Log("GUI: hwnd %p is not a valid window", gHWnd); return false; }

    if (!CreateBackBufferRTV(swapChain))
    { Log("GUI: Failed to create back buffer RTV"); return false; }
    Log("GUI: back buffer RTV ok");

    IMGUI_CHECKVERSION();
    if (!ImGui::CreateContext())
    { Log("GUI: ImGui::CreateContext failed"); return false; }
    ImGuiIO& io = ImGui::GetIO();
    // NOTE: NavEnableKeyboard intentionally NOT set — it maps Space to
    // "Activate" which can leave io.KeysDown[VK_SPACE] stuck when the
    // overlay closes, breaking the game's pause key.

    // Style
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Alpha = 0.95f;
    style.WindowPadding = ImVec2(10, 10);
    style.ItemSpacing = ImVec2(8, 5);

    if (!ImGui_ImplWin32_Init(gHWnd))
    { Log("GUI: ImGui_ImplWin32_Init failed"); ImGui::DestroyContext(); return false; }
    Log("GUI: Win32 backend ok");
    if (!ImGui_ImplDX11_Init(gDevice, gContext))
    { Log("GUI: ImGui_ImplDX11_Init failed"); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); return false; }
    Log("GUI: DX11 backend ok");
    LoadLogoTextures();

    SetLastError(0);
    oWndProc = (WNDPROC)SetWindowLongPtrW(gHWnd, GWLP_WNDPROC, (LONG_PTR)DustWndProc);
    if (!oWndProc)
    {
        DWORD err = GetLastError();
        if (err != 0)
        { Log("GUI: SetWindowLongPtrW failed err=%lu", err); ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); return false; }
    }
    Log("GUI: WndProc subclassed");

    InstallDInputHooks();
    Log("GUI: DInput hooks installed");

    LoadFrameworkConfig();

    gInitialized = true;
    Log("GUI: Initialized (%s to toggle)", VKKeyName(gFwConfig.toggleKey));
    return true;
}

void Shutdown()
{
    if (!gInitialized) return;

    // Reset input blocking state before teardown
    gKbWasBlocking = false;
    memset(gKbTrackedState, 0, sizeof(gKbTrackedState));

    if (gOverlayVisible)
        OnOverlayClose();

    if (oWndProc && gHWnd)
        SetWindowLongPtrW(gHWnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
    oWndProc = nullptr;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (gBackBufferRTV)  { gBackBufferRTV->Release();  gBackBufferRTV = nullptr; }
    if (gDiscordLogoSRV) { gDiscordLogoSRV->Release(); gDiscordLogoSRV = nullptr; }
    if (gGithubLogoSRV)  { gGithubLogoSRV->Release();  gGithubLogoSRV = nullptr; }

    gEffectStates.clear();
    gInitialized = false;
}

bool IsVisible()
{
    return gOverlayVisible;
}

void SetResizeInProgress(bool inProgress)
{
    gResizeInProgress = inProgress;
}

void ReleaseBackBuffer()
{
    if (!gInitialized) return;
    if (gBackBufferRTV) { gBackBufferRTV->Release(); gBackBufferRTV = nullptr; }
    ImGui_ImplDX11_InvalidateDeviceObjects();
}

bool RecreateBackBuffer(IDXGISwapChain* swapChain)
{
    if (!gInitialized) return false;
    if (!CreateBackBufferRTV(swapChain))
    {
        Log("GUI: Failed to recreate back buffer RTV after resize");
        return false;
    }
    return true;
}

void Render()
{
    if (!gInitialized || gResizeInProgress) return;
    if (!gDevice || !gContext || !gBackBufferRTV) return;

    // Skip rendering if device has been removed (alt-tab, resolution change, etc.)
    HRESULT removeReason = gDevice->GetDeviceRemovedReason();
    if (removeReason != S_OK) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Always render the toast (even when overlay is closed)
    RenderToast();

    if (gOverlayVisible)
    {
        ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
        bool wasVisible = gOverlayVisible;
        ImGui::Begin("Dust Settings", &gOverlayVisible);
        if (wasVisible && !gOverlayVisible)
            OnOverlayClose(); // user clicked the X button

        static float leftW = 220.0f;
        const float minLeftW = 150.0f;
        const float minRightW = 200.0f;
        float availW = ImGui::GetContentRegionAvail().x;

        // ---- Left pane: Framework settings + Performance (always visible) ----
        ImGui::BeginChild("##left", ImVec2(leftW, 0), true);

        // Framework settings
        DrawFrameworkSection();

        ImGui::Spacing();
        ImGui::Spacing();

        // Performance (always visible)
        DrawPerformanceSection();

        // Social logo buttons pinned to bottom of left pane
        {
            const float logoSize = 36.0f;
            const float framePad = ImGui::GetStyle().FramePadding.y;
            const float winPad   = ImGui::GetStyle().WindowPadding.y;
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - logoSize - framePad * 2.0f - winPad);

            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.1f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.2f));

            if (gDiscordLogoSRV)
            {
                ImGui::PushID("discord");
                if (ImGui::ImageButton((ImTextureID)gDiscordLogoSRV, ImVec2(logoSize, logoSize)))
                    ShellExecuteA(nullptr, "open", "https://discord.gg/3fd3c7EFvT", nullptr, nullptr, SW_SHOWNORMAL);
                ImGui::PopID();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Join the Discord server");
            }

            if (gGithubLogoSRV)
            {
                ImGui::SameLine();
                ImGui::PushID("github");
                if (ImGui::ImageButton((ImTextureID)gGithubLogoSRV, ImVec2(logoSize, logoSize)))
                    ShellExecuteA(nullptr, "open", "https://github.com/Bazouz660/Dust", nullptr, nullptr, SW_SHOWNORMAL);
                ImGui::PopID();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("View on GitHub");
            }

            ImGui::PopStyleColor(3);
        }

        ImGui::EndChild();

        // ---- Draggable splitter between panes ----
        ImGui::SameLine();
        ImGui::Button("##splitter", ImVec2(4.0f, ImGui::GetContentRegionAvail().y));
        if (ImGui::IsItemActive())
        {
            float delta = ImGui::GetIO().MouseDelta.x;
            leftW += delta;
            if (leftW < minLeftW) leftW = minLeftW;
            if (leftW > availW - minRightW) leftW = availW - minRightW;
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

        ImGui::SameLine();

        // ---- Right pane: Effect settings ----
        ImGui::BeginChild("##right", ImVec2(0, 0), true);

        size_t count = gEffectLoader.Count();

        if (count == 0)
        {
            ImGui::TextDisabled("No effect plugins loaded");
        }
        else
        {
            // Global preset selector
            DrawPresetSection();

            // Global effects toggle
            {
                bool prev = gAllEffectsOn;
                if (ImGui::Checkbox("Toggle Effects", &gAllEffectsOn))
                {
                    if (!gAllEffectsOn)
                    {
                        // Save which effects are currently enabled, then disable all
                        gEffectWasEnabled.resize(count);
                        for (size_t i = 0; i < count; i++)
                        {
                            const LoadedEffect& le = gEffectLoader.GetEffect(i);
                            if (!le.initialized) { gEffectWasEnabled[i] = false; continue; }
                            bool* p = FindEnabledPtr(le);
                            gEffectWasEnabled[i] = p ? *p : false;
                            if (p) *p = false;
                            if (le.desc.OnSettingChanged) le.desc.OnSettingChanged();
                        }
                    }
                    else
                    {
                        // Restore only effects that were previously enabled
                        for (size_t i = 0; i < count && i < gEffectWasEnabled.size(); i++)
                        {
                            const LoadedEffect& le = gEffectLoader.GetEffect(i);
                            if (!le.initialized) continue;
                            if (!gEffectWasEnabled[i]) continue;
                            bool* p = FindEnabledPtr(le);
                            if (p) *p = true;
                            if (le.desc.OnSettingChanged) le.desc.OnSettingChanged();
                        }
                    }
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            for (size_t i = 0; i < count; i++)
            {
                const LoadedEffect& le = gEffectLoader.GetEffect(i);
                if (!le.initialized) continue;

                ImGui::PushID((int)i);
                DrawEffectSection(i);
                ImGui::PopID();

                // Spacing between effects
                if (i + 1 < count)
                {
                    ImGui::Spacing();
                    ImGui::Spacing();
                }
            }
        }

        ImGui::EndChild();

        ImGui::End();
    }

    ImGui::Render();
    gContext->OMSetRenderTargets(1, &gBackBufferRTV, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

} // namespace DustGUI
