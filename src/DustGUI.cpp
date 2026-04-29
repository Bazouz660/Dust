#include "DustGUI.h"
#include "DustGUI_DInputHook.h"
#include "DustLog.h"
#include "EffectLoader.h"
#include "FilePicker.h"
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
#include <cctype>

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
    bool showSurvey = false;
    std::string lastPreset;        // name of last selected preset (empty = custom)
    int toggleKey = VK_F11;        // virtual key code for overlay toggle
    int toggleEffectsKey = 0;      // 0 = unbound; else VK code that flips all effects on/off
    std::string theme = "kenshi";  // GUI theme: "kenshi" or "dark"
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
static bool gWaitingForEffectsKey = false;

// ==================== State ====================

bool gInitialized = false;
bool gOverlayVisible = false;
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
static int gForceCollapseState = 0; // 0=none, 1=expand all, -1=collapse all (consumed each frame)

// Preset system GUI state
static char gNewPresetName[64] = {};

// Async picker tracking. The picker runs on a worker thread; we set one of
// these to indicate what the next polled result should be used for.
enum class PickerPurpose { None, Import, Export };
static PickerPurpose gPickerPurpose = PickerPurpose::None;
static int  gPickerExportPresetIdx = -1; // valid only for PickerPurpose::Export
static std::string gPickerError;          // last error message (cleared when popup closes)
static std::string gPickerInfo;           // last info message (e.g. "Imported 'X'")
static int  gPickerInfoFrames = 0;        // countdown for transient info display

// Edit Info popup state
static char gEditInfoAuthor[128] = {};
static char gEditInfoDesc[512]   = {};
static int  gEditInfoPresetIdx   = -1;

// Overwrite-on-import confirmation state
static std::string gPendingImportSrc;     // source folder waiting for user to OK overwrite
static std::string gPendingImportName;    // colliding name

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

// Flip every effect on or off, remembering the per-effect enabled state across
// a disable/enable cycle. Used by the "Toggle Effects" checkbox and hotkey.
static void SetAllEffectsEnabled(bool on)
{
    if (on == gAllEffectsOn) return;
    gAllEffectsOn = on;

    size_t count = gEffectLoader.Count();
    if (!on)
    {
        gEffectWasEnabled.assign(count, false);
        for (size_t i = 0; i < count; i++)
        {
            const LoadedEffect& le = gEffectLoader.GetEffect(i);
            if (!le.initialized) continue;
            bool* p = FindEnabledPtr(le);
            gEffectWasEnabled[i] = p ? *p : false;
            if (p) *p = false;
            if (le.desc.OnSettingChanged) le.desc.OnSettingChanged();
        }
    }
    else
    {
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

// Re-derive the master "all effects on" state from the actual effect Enabled
// flags. Call this after a preset load: presets rewrite each effect's Enabled,
// so the master switch and the remembered pre-disable snapshot would otherwise
// drift out of sync with what's actually rendering.
static void SyncAllEffectsOnState()
{
    size_t count = gEffectLoader.Count();
    bool anyOn = false;
    for (size_t i = 0; i < count; i++)
    {
        const LoadedEffect& le = gEffectLoader.GetEffect(i);
        if (!le.initialized) continue;
        if (IsEffectEnabled(le)) { anyOn = true; break; }
    }
    gAllEffectsOn = anyOn;
    gEffectWasEnabled.clear(); // snapshot belonged to the previous preset
}

// Case-insensitive substring match for the right-pane effect search filter.
static bool EffectNameMatchesFilter(const char* name, const char* filter)
{
    if (!filter || !*filter) return true;
    if (!name) return false;
    size_t nlen = strlen(name);
    size_t flen = strlen(filter);
    if (flen > nlen) return false;
    for (size_t i = 0; i + flen <= nlen; i++)
    {
        bool match = true;
        for (size_t j = 0; j < flen; j++)
        {
            unsigned char a = (unsigned char)name[i + j];
            unsigned char b = (unsigned char)filter[j];
            if (tolower(a) != tolower(b)) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
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
// Implementation moved to DustGUI_DInputHook.{h,cpp}.

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

    // Send WM_KEYUP for every key the game thinks is held, so nothing stays stuck.
    if (oWndProc && gHWnd)
    {
        for (int vk = 0; vk < 256; vk++)
        {
            if (gGameKeyHeld[vk])
            {
                UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
                LPARAM lp = (1) | (scanCode << 16) | (0x3 << 30);
                CallWindowProcW(oWndProc, gHWnd, WM_KEYUP, (WPARAM)vk, lp);
            }
        }
        memset(gGameKeyHeld, 0, sizeof(gGameKeyHeld));
    }

    // Restore cursor clipping (game usually clips cursor to window)
    if (gHadClipRect)
        ClipCursor(&gSavedClipRect);

    // Hide system cursor (game manages its own)
    while (ShowCursor(FALSE) >= 0) {}
}

static LRESULT CALLBACK DustWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Capture key binding when waiting for a new toggle key.
    // WM_SYSKEYDOWN is required for F10 (Windows treats it as a menu-activation key).
    // Escape cancels without rebinding.
    if ((gWaitingForKey || gWaitingForEffectsKey) && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN))
    {
        if (wParam != VK_ESCAPE)
        {
            if (gWaitingForKey)        gFwConfig.toggleKey        = (int)wParam;
            else if (gWaitingForEffectsKey) gFwConfig.toggleEffectsKey = (int)wParam;
        }
        gWaitingForKey = false;
        gWaitingForEffectsKey = false;
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

    // Toggle-all-effects hotkey (skip when unbound, i.e. toggleEffectsKey == 0).
    bool isEffectsToggleMsg = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
                               gFwConfig.toggleEffectsKey != 0 &&
                               (int)wParam == gFwConfig.toggleEffectsKey &&
                               !(lParam & 0x40000000);
    if (isEffectsToggleMsg)
    {
        SetAllEffectsEnabled(!gAllEffectsOn);
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

        // Block game input when the mouse is hovering over the GUI
        bool isMouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
        bool isKeyboard = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST);
        bool eatInput = (isMouse || isKeyboard || msg == WM_INPUT) && ImGui::GetIO().WantCaptureMouse;
        if (eatInput)
        {
            if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
            {
                if (wParam < 256 && gGameKeyHeld[wParam])
                    eatInput = false;
            }
            if (eatInput)
                return 0;
        }
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

    if (wParam < 256)
    {
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
            gGameKeyHeld[wParam] = 1;
        else if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
            gGameKeyHeld[wParam] = 0;
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

// ==================== Theme ====================

// Two themes: "kenshi" (warm parchment / dusty amber, matches the game) and
// "dark" (ImGui default). Applied at init and live-previewed on combo change.
static void ApplyDustTheme(const std::string& name)
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    if (name == "dark")
    {
        ImGui::StyleColorsDark();
        style.WindowRounding = 6.0f;
        style.FrameRounding  = 4.0f;
    }
    else
    {
        // Kenshi: near-black warm backgrounds, cream/parchment text, dusty
        // amber accents on hovered/active widgets. Sharper corners than dark.
        c[ImGuiCol_Text]                  = ImVec4(0.92f, 0.86f, 0.74f, 1.00f);
        c[ImGuiCol_TextDisabled]          = ImVec4(0.55f, 0.50f, 0.42f, 1.00f);
        c[ImGuiCol_WindowBg]              = ImVec4(0.05f, 0.04f, 0.03f, 0.94f);
        c[ImGuiCol_ChildBg]               = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_PopupBg]               = ImVec4(0.06f, 0.05f, 0.04f, 0.96f);
        c[ImGuiCol_Border]                = ImVec4(0.30f, 0.24f, 0.16f, 0.50f);
        c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_FrameBg]               = ImVec4(0.10f, 0.08f, 0.06f, 0.85f);
        c[ImGuiCol_FrameBgHovered]        = ImVec4(0.20f, 0.14f, 0.08f, 0.90f);
        c[ImGuiCol_FrameBgActive]         = ImVec4(0.28f, 0.18f, 0.08f, 0.95f);
        c[ImGuiCol_TitleBg]               = ImVec4(0.04f, 0.03f, 0.02f, 1.00f);
        c[ImGuiCol_TitleBgActive]         = ImVec4(0.08f, 0.06f, 0.04f, 1.00f);
        c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.04f, 0.03f, 0.02f, 0.75f);
        c[ImGuiCol_MenuBarBg]             = ImVec4(0.08f, 0.06f, 0.04f, 1.00f);
        c[ImGuiCol_ScrollbarBg]           = ImVec4(0.04f, 0.03f, 0.02f, 0.50f);
        c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.30f, 0.22f, 0.12f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.45f, 0.32f, 0.16f, 1.00f);
        c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.60f, 0.42f, 0.20f, 1.00f);
        c[ImGuiCol_CheckMark]             = ImVec4(0.85f, 0.62f, 0.30f, 1.00f);
        c[ImGuiCol_SliderGrab]            = ImVec4(0.60f, 0.42f, 0.20f, 1.00f);
        c[ImGuiCol_SliderGrabActive]      = ImVec4(0.85f, 0.62f, 0.30f, 1.00f);
        c[ImGuiCol_Button]                = ImVec4(0.18f, 0.13f, 0.08f, 0.85f);
        c[ImGuiCol_ButtonHovered]         = ImVec4(0.45f, 0.30f, 0.14f, 0.95f);
        c[ImGuiCol_ButtonActive]          = ImVec4(0.65f, 0.42f, 0.18f, 1.00f);
        c[ImGuiCol_Header]                = ImVec4(0.18f, 0.13f, 0.08f, 0.85f);
        c[ImGuiCol_HeaderHovered]         = ImVec4(0.40f, 0.28f, 0.14f, 0.90f);
        c[ImGuiCol_HeaderActive]          = ImVec4(0.55f, 0.36f, 0.16f, 1.00f);
        c[ImGuiCol_Separator]             = ImVec4(0.30f, 0.22f, 0.12f, 0.50f);
        c[ImGuiCol_SeparatorHovered]      = ImVec4(0.50f, 0.34f, 0.16f, 0.80f);
        c[ImGuiCol_SeparatorActive]       = ImVec4(0.70f, 0.46f, 0.20f, 1.00f);
        c[ImGuiCol_ResizeGrip]            = ImVec4(0.30f, 0.22f, 0.12f, 0.40f);
        c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.50f, 0.34f, 0.16f, 0.70f);
        c[ImGuiCol_ResizeGripActive]      = ImVec4(0.70f, 0.46f, 0.20f, 1.00f);
        c[ImGuiCol_Tab]                   = ImVec4(0.12f, 0.09f, 0.06f, 1.00f);
        c[ImGuiCol_TabHovered]            = ImVec4(0.45f, 0.30f, 0.14f, 1.00f);
        c[ImGuiCol_TabActive]             = ImVec4(0.30f, 0.20f, 0.10f, 1.00f);
        c[ImGuiCol_TabUnfocused]          = ImVec4(0.08f, 0.06f, 0.04f, 1.00f);
        c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.18f, 0.13f, 0.08f, 1.00f);
        c[ImGuiCol_PlotLines]             = ImVec4(0.85f, 0.62f, 0.30f, 1.00f);
        c[ImGuiCol_PlotLinesHovered]      = ImVec4(1.00f, 0.75f, 0.40f, 1.00f);
        c[ImGuiCol_PlotHistogram]         = ImVec4(0.85f, 0.62f, 0.30f, 1.00f);
        c[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.00f, 0.75f, 0.40f, 1.00f);
        c[ImGuiCol_TextSelectedBg]        = ImVec4(0.55f, 0.36f, 0.16f, 0.50f);

        style.WindowRounding = 2.0f;
        style.FrameRounding  = 1.0f;
    }

    style.GrabRounding  = style.FrameRounding;
    style.Alpha         = 0.95f;
    style.WindowPadding = ImVec2(10, 10);
    style.ItemSpacing   = ImVec2(8, 5);
}

// ==================== Framework config ====================

static void LoadFrameworkConfig()
{
    gDustIniPath = DustLogDir() + "Dust.ini";
    gFwConfig.logging = GetPrivateProfileIntA("Dust", "Logging", 0, gDustIniPath.c_str()) != 0;
    gFwConfig.showStartupMessage = GetPrivateProfileIntA("Dust", "ShowStartupMessage", 1, gDustIniPath.c_str()) != 0;
    gFwConfig.showSurvey = GetPrivateProfileIntA("Dust", "ShowSurvey", 0, gDustIniPath.c_str()) != 0;

    gFwConfig.toggleKey        = GetPrivateProfileIntA("Dust", "ToggleKey",        VK_F11, gDustIniPath.c_str());
    gFwConfig.toggleEffectsKey = GetPrivateProfileIntA("Dust", "ToggleEffectsKey", 0,      gDustIniPath.c_str());

    char themeBuf[64] = {};
    GetPrivateProfileStringA("Dust", "Theme", "kenshi", themeBuf, sizeof(themeBuf), gDustIniPath.c_str());
    gFwConfig.theme = themeBuf;
    if (gFwConfig.theme != "kenshi" && gFwConfig.theme != "dark")
        gFwConfig.theme = "kenshi";

    char buf[256] = {};
    GetPrivateProfileStringA("Dust", "LastPreset", "", buf, sizeof(buf), gDustIniPath.c_str());
    gFwConfig.lastPreset = buf;

    // Default to dust_high if no preset has been saved yet
    if (gFwConfig.lastPreset.empty())
        gFwConfig.lastPreset = "dust_high";

    // Preset auto-load is deferred to Render() — effects may not be initialized yet
    // when the GUI starts (e.g. GUI inits from DustBoot swap chain before device capture).

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
    WritePrivateProfileStringA("Dust", "ShowSurvey", gFwConfig.showSurvey ? "1" : "0", gDustIniPath.c_str());
    char keyBuf[16];
    snprintf(keyBuf, sizeof(keyBuf), "%d", gFwConfig.toggleKey);
    WritePrivateProfileStringA("Dust", "ToggleKey", keyBuf, gDustIniPath.c_str());
    snprintf(keyBuf, sizeof(keyBuf), "%d", gFwConfig.toggleEffectsKey);
    WritePrivateProfileStringA("Dust", "ToggleEffectsKey", keyBuf, gDustIniPath.c_str());
    WritePrivateProfileStringA("Dust", "Theme", gFwConfig.theme.c_str(), gDustIniPath.c_str());
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
    ApplyDustTheme(gFwConfig.theme);
}

static bool IsFrameworkDirty()
{
    return gFwConfig.logging != gFwDiskConfig.logging ||
           gFwConfig.showStartupMessage != gFwDiskConfig.showStartupMessage ||
           gFwConfig.showSurvey != gFwDiskConfig.showSurvey ||
           gFwConfig.lastPreset != gFwDiskConfig.lastPreset ||
           gFwConfig.toggleKey != gFwDiskConfig.toggleKey ||
           gFwConfig.toggleEffectsKey != gFwDiskConfig.toggleEffectsKey ||
           gFwConfig.theme != gFwDiskConfig.theme;
}

// ==================== Drawing: Framework pane ====================

static void DrawFrameworkSection()
{
    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "Dust");
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", DUST_VERSION_STR);
    ImGui::Separator();
    ImGui::Spacing();

    // Toggle key binding (overlay GUI)
    if (gWaitingForKey)
    {
        ImGui::Button("Press a key...", ImVec2(ImGui::GetContentRegionAvail().x, 0));
    }
    else
    {
        char label[128];
        snprintf(label, sizeof(label), "Toggle: %s", VKKeyName(gFwConfig.toggleKey));
        if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, 0)))
        {
            gWaitingForKey = true;
            gWaitingForEffectsKey = false;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click to rebind the overlay toggle key");
    }

    // Toggle-all-effects key binding (works whether the overlay is open or closed)
    if (gWaitingForEffectsKey)
    {
        ImGui::Button("Press a key...##fx", ImVec2(ImGui::GetContentRegionAvail().x, 0));
    }
    else
    {
        char label[128];
        if (gFwConfig.toggleEffectsKey == 0)
            snprintf(label, sizeof(label), "Toggle Effects: (unbound)");
        else
            snprintf(label, sizeof(label), "Toggle Effects: %s", VKKeyName(gFwConfig.toggleEffectsKey));
        if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, 0)))
        {
            gWaitingForEffectsKey = true;
            gWaitingForKey = false;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click to rebind the hotkey that flips all effects on/off. Works whether the overlay is open or closed. Set ToggleEffectsKey=0 in Dust.ini to unbind.");
    }

    ImGui::Spacing();

    {
        const char* themeLabels[] = { "Kenshi", "Dark" };
        const char* themeKeys[]   = { "kenshi", "dark"  };
        int themeIdx = (gFwConfig.theme == "dark") ? 1 : 0;
        if (ImGui::Combo("Theme", &themeIdx, themeLabels, IM_ARRAYSIZE(themeLabels)))
        {
            gFwConfig.theme = themeKeys[themeIdx];
            ApplyDustTheme(gFwConfig.theme);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("UI theme. Kenshi: warm parchment palette matching the game. Dark: ImGui default.");
    }

    ImGui::Spacing();

    ImGui::Checkbox("Logging", &gFwConfig.logging);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Write timestamped logs to logs/ folder next to the DLL");

    ImGui::Checkbox("Startup Message", &gFwConfig.showStartupMessage);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show notification on game start");

    ImGui::Checkbox("Show Survey", &gFwConfig.showSurvey);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show pipeline survey controls (developer tool)");

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

    // ---- Pipeline Survey (hidden by default, dev tool) ----
    if (gFwConfig.showSurvey)
    {
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

// Tooltip showing preset metadata (author, description, etc.)
static void DrawPresetMetaTooltip(const PresetInfo& p)
{
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(p.name.c_str());
    if (p.hasMetadata)
    {
        if (!p.metaAuthor.empty())
            ImGui::Text("Author: %s", p.metaAuthor.c_str());
        if (!p.metaDescription.empty())
        {
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(p.metaDescription.c_str());
            ImGui::PopTextWrapPos();
        }
        if (p.metaApiVersion > 0)
            ImGui::TextDisabled("API v%d, preset v%d", p.metaApiVersion, p.metaVersion);
    }
    else
    {
        ImGui::TextDisabled("(no metadata)");
    }
    ImGui::EndTooltip();
}

// Lightweight visual-only "disable": grey the button out by pushing alpha.
// This ImGui version doesn't have BeginDisabled/EndDisabled (1.85+) so we
// pair this with manual click gating (`if (clicked && !disabled)`).
static void PushVisualDisabled(bool disabled)
{
    if (disabled)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
}
static void PopVisualDisabled(bool disabled)
{
    if (disabled)
        ImGui::PopStyleVar();
}

// Try to consume an async picker result. Called once per frame.
static void PollFilePicker()
{
    std::string path;
    if (!FilePicker::Poll(path)) return;

    PickerPurpose purpose = gPickerPurpose;
    gPickerPurpose = PickerPurpose::None;

    if (path.empty()) return; // user cancelled

    if (purpose == PickerPurpose::Import)
    {
        std::string err;
        int idx = gEffectLoader.ImportPresetFromFolder(path.c_str(), false, &err);
        if (idx >= 0)
        {
            gEffectLoader.LoadPreset(idx);
            SyncAllEffectsOnState();
            const auto& presets = gEffectLoader.GetPresets();
            if (idx < (int)presets.size())
            {
                gFwConfig.lastPreset = presets[idx].name;
                SaveFrameworkConfig();
            }
            SnapshotAllEffects();
            gPickerInfo = "Imported '" + presets[idx].name + "'";
            gPickerInfoFrames = 240; // ~4 seconds at 60fps
            gPickerError.clear();
        }
        else if (err.find("already exists") != std::string::npos)
        {
            // Defer overwrite confirmation to a popup
            gPendingImportSrc = path;
            // Extract name for display from error message ("preset named 'X' ...")
            size_t a = err.find('\'');
            size_t b = (a != std::string::npos) ? err.find('\'', a + 1) : std::string::npos;
            if (a != std::string::npos && b != std::string::npos)
                gPendingImportName = err.substr(a + 1, b - a - 1);
            else
                gPendingImportName = "<unknown>";
            gPickerError.clear();
        }
        else
        {
            gPickerError = err.empty() ? "Import failed" : err;
        }
    }
    else if (purpose == PickerPurpose::Export)
    {
        std::string err;
        if (gEffectLoader.ExportPreset(gPickerExportPresetIdx, path.c_str(), &err))
        {
            const auto& presets = gEffectLoader.GetPresets();
            const char* name = (gPickerExportPresetIdx >= 0 && gPickerExportPresetIdx < (int)presets.size())
                ? presets[gPickerExportPresetIdx].name.c_str() : "preset";
            gPickerInfo = std::string("Exported '") + name + "' to " + path;
            gPickerInfoFrames = 240;
            gPickerError.clear();
        }
        else
        {
            gPickerError = err.empty() ? "Export failed" : err;
        }
    }
}

static void DrawPresetSection()
{
    PollFilePicker();

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
                SyncAllEffectsOnState();
                gFwConfig.lastPreset = presets[p].name;
                SaveFrameworkConfig();
                SnapshotAllEffects();
            }
            if (ImGui::IsItemHovered())
                DrawPresetMetaTooltip(presets[p]);
        }
        ImGui::EndCombo();
    }
    if (currentPreset >= 0 && currentPreset < (int)presets.size() && ImGui::IsItemHovered())
        DrawPresetMetaTooltip(presets[currentPreset]);

    // Show warnings for outdated presets
    if (currentPreset >= 0 && !presets[currentPreset].warnings.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[!] Preset is outdated");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", presets[currentPreset].warnings.c_str());
    }

    // Picker-busy banner (so the user knows the dialog is open somewhere)
    if (FilePicker::IsBusy())
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Waiting for folder picker...");

    // Transient info / error
    if (gPickerInfoFrames > 0 && !gPickerInfo.empty())
    {
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%s", gPickerInfo.c_str());
        gPickerInfoFrames--;
    }
    if (!gPickerError.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", gPickerError.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss##PickerErr")) gPickerError.clear();
    }

    // ---- Row 1: Save As / Import ----
    bool pickerBusy = FilePicker::IsBusy();

    if (ImGui::Button("Save As...", ImVec2(0, 0)))
        ImGui::OpenPopup("##GlobalSavePresetAs");

    ImGui::SameLine();
    PushVisualDisabled(pickerBusy);
    if (ImGui::Button("Import...") && !pickerBusy)
    {
        gPickerPurpose = PickerPurpose::Import;
        gPickerError.clear();
        if (!FilePicker::StartFolderPicker("Choose a Dust preset folder to import"))
            gPickerPurpose = PickerPurpose::None;
    }
    PopVisualDisabled(pickerBusy);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
            pickerBusy ? "A folder picker is already open"
                       : "Import a preset from a folder on disk");

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

    // ---- Row 2: Save / Export / Edit Info / Delete (only when a preset is selected) ----
    if (currentPreset >= 0)
    {
        if (ImGui::Button("Save", ImVec2(0, 0)))
        {
            gEffectLoader.SavePreset(currentPreset);
            SnapshotAllEffects();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save current settings into '%s'", presets[currentPreset].name.c_str());

        ImGui::SameLine();
        PushVisualDisabled(pickerBusy);
        if (ImGui::Button("Export...") && !pickerBusy)
        {
            gPickerPurpose = PickerPurpose::Export;
            gPickerExportPresetIdx = currentPreset;
            gPickerError.clear();
            if (!FilePicker::StartFolderPicker("Choose a destination folder to export the preset"))
                gPickerPurpose = PickerPurpose::None;
        }
        PopVisualDisabled(pickerBusy);
        if (ImGui::IsItemHovered())
        {
            if (pickerBusy)
                ImGui::SetTooltip("A folder picker is already open");
            else
                ImGui::SetTooltip("Copy '%s' into another folder for sharing",
                                  presets[currentPreset].name.c_str());
        }

        ImGui::SameLine();
        if (ImGui::Button("Edit Info..."))
        {
            gEditInfoPresetIdx = currentPreset;
            const PresetInfo& p = presets[currentPreset];
            // Pre-fill from existing metadata
            strncpy_s(gEditInfoAuthor, sizeof(gEditInfoAuthor),
                      p.metaAuthor.c_str(), _TRUNCATE);
            strncpy_s(gEditInfoDesc, sizeof(gEditInfoDesc),
                      p.metaDescription.c_str(), _TRUNCATE);
            ImGui::OpenPopup("##EditPresetInfo");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Edit author / description metadata");

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

    // ---- Edit Info popup ----
    if (ImGui::BeginPopup("##EditPresetInfo"))
    {
        ImGui::Text("Preset metadata");
        ImGui::Separator();
        ImGui::SetNextItemWidth(280);
        ImGui::InputText("Author##editinfoauthor", gEditInfoAuthor, sizeof(gEditInfoAuthor));
        ImGui::SetNextItemWidth(280);
        ImGui::InputTextMultiline("Description##editinfodesc",
                                  gEditInfoDesc, sizeof(gEditInfoDesc),
                                  ImVec2(280, 80));
        ImGui::Spacing();
        if (ImGui::Button("Save##editinfo", ImVec2(80, 0)))
        {
            if (gEditInfoPresetIdx >= 0 && gEditInfoPresetIdx < (int)presets.size())
                gEffectLoader.UpdatePresetMetadata(gEditInfoPresetIdx,
                                                  gEditInfoAuthor, gEditInfoDesc);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##editinfo", ImVec2(80, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- Overwrite-on-import confirmation ----
    if (!gPendingImportSrc.empty())
        ImGui::OpenPopup("##ConfirmImportOverwrite");
    if (ImGui::BeginPopupModal("##ConfirmImportOverwrite", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("A preset named '%s' already exists.", gPendingImportName.c_str());
        ImGui::Text("Overwrite it?");
        ImGui::Spacing();
        if (ImGui::Button("Overwrite", ImVec2(100, 0)))
        {
            std::string err;
            int idx = gEffectLoader.ImportPresetFromFolder(gPendingImportSrc.c_str(), true, &err);
            if (idx >= 0)
            {
                gEffectLoader.LoadPreset(idx);
                SyncAllEffectsOnState();
                const auto& ps = gEffectLoader.GetPresets();
                if (idx < (int)ps.size())
                {
                    gFwConfig.lastPreset = ps[idx].name;
                    SaveFrameworkConfig();
                }
                SnapshotAllEffects();
                gPickerInfo = "Imported '" + gPendingImportName + "' (overwrote existing)";
                gPickerInfoFrames = 240;
            }
            else
            {
                gPickerError = err.empty() ? "Import failed" : err;
            }
            gPendingImportSrc.clear();
            gPendingImportName.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)))
        {
            gPendingImportSrc.clear();
            gPendingImportName.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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
    if (gForceCollapseState != 0)
        ImGui::SetNextItemOpen(gForceCollapseState > 0);
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

        if (ImGui::IsItemHovered() && s.type != DUST_SETTING_SECTION)
        {
            ImGui::BeginTooltip();
            if (s.description)
                ImGui::TextUnformatted(s.description);

            const char* perfLabel = nullptr;
            ImVec4 perfColor;
            switch (s.perfImpact)
            {
            case DUST_PERF_NONE:
                perfLabel = "None";
                perfColor = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
                break;
            case DUST_PERF_LOW:
                perfLabel = "Low";
                perfColor = ImVec4(0.85f, 0.80f, 0.55f, 1.00f);
                break;
            case DUST_PERF_MEDIUM:
                perfLabel = "Medium";
                perfColor = ImVec4(1.00f, 0.60f, 0.20f, 1.00f);
                break;
            case DUST_PERF_HIGH:
                perfLabel = "High";
                perfColor = ImVec4(1.00f, 0.30f, 0.25f, 1.00f);
                break;
            default:
                break;
            }
            if (perfLabel)
            {
                ImGui::TextUnformatted("Performance impact: ");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(perfColor, "%s", perfLabel);
            }
            ImGui::EndTooltip();
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

    // Per-effect GPU timing (only active effects)
    ImGui::Text("Effect GPU Times:");
    ImGui::Spacing();

    float totalEffectMs = 0.0f;
    size_t count = gEffectLoader.Count();

    int maxNameLen = 0;
    for (size_t i = 0; i < count; i++)
    {
        const LoadedEffect& le = gEffectLoader.GetEffect(i);
        if (!le.initialized || !IsEffectEnabled(le)) continue;
        const char* name = le.desc.name ? le.desc.name : "Unnamed";
        int len = (int)strlen(name);
        if (len > maxNameLen) maxNameLen = len;
    }

    for (size_t i = 0; i < count; i++)
    {
        const LoadedEffect& le = gEffectLoader.GetEffect(i);
        if (!le.initialized || !IsEffectEnabled(le)) continue;

        const char* name = le.desc.name ? le.desc.name : "Unnamed";
        float gpuMs = gEffectLoader.GetEffectGpuTime(i);
        bool hasTiming = (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_TIMING))
                      || le.desc.gpuTimeMsPtr;

        totalEffectMs += gpuMs;

        ImVec4 color = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
        if (gpuMs > 2.0f) color = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);
        if (gpuMs > 5.0f) color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);

        if (hasTiming)
        {
            ImGui::TextColored(color, "  %-*s %.2f ms", maxNameLen, name, gpuMs);
        }
        else
        {
            ImGui::TextDisabled("  %-*s (no timing)", maxNameLen, name);
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

    if (!swapChain)
    { Log("GUI: Init aborted — null swap chain"); return false; }

    // Always use the swap chain's own device — it's the only one that can create
    // views on the swap chain's back buffer. On multi-device systems (e.g. iGPU +
    // discrete GPU) this may differ from the effects pipeline's captured device.
    {
        ID3D11Device* scDevice = nullptr;
        HRESULT hr = swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&scDevice);
        if (FAILED(hr) || !scDevice)
        { Log("GUI: swapChain->GetDevice failed hr=0x%08X", (unsigned)hr); return false; }

        if (device && scDevice != device)
            Log("GUI: note: swapChain device (%p) != captured device (%p)", scDevice, device);

        gDevice = scDevice;
        gDevice->GetImmediateContext(&gContext);
        gContext->Release();
        scDevice->Release();
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

    // Style: apply default theme now; LoadFrameworkConfig below will reapply
    // the user's saved choice once the ini has been read.
    ApplyDustTheme("kenshi");

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
    ApplyDustTheme(gFwConfig.theme);

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

    FilePicker::Shutdown();

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

    // Deferred preset auto-load: GUI can start before effects are initialized
    // (DustBoot provides the swap chain before device capture / effect init).
    // Phase 1: set display name early (presets scanned in LoadAll).
    // Phase 2: apply values once effects are initialized.
    {
        static bool sPresetDisplaySet = false;
        static bool sPresetApplied = false;
        static int  sPendingIdx = -1;

        if (!sPresetApplied && !gFwConfig.lastPreset.empty())
        {
            if (!sPresetDisplaySet)
            {
                const auto& presets = gEffectLoader.GetPresets();
                for (int i = 0; i < (int)presets.size(); i++)
                {
                    if (presets[i].name == gFwConfig.lastPreset)
                    {
                        gEffectLoader.SetCurrentPreset(i);
                        sPendingIdx = i;
                        sPresetDisplaySet = true;
                        break;
                    }
                }
            }

            if (sPresetDisplaySet && sPendingIdx >= 0 && gEffectLoader.IsInitialized())
            {
                gEffectLoader.LoadPreset(sPendingIdx);
                SyncAllEffectsOnState();
                Log("Loaded global preset '%s'", gFwConfig.lastPreset.c_str());
                sPresetApplied = true;
            }
        }
    }

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

        // ---- Left pane: Framework settings + Performance (scrollable) + fixed footer ----
        const float logoSize = 36.0f;
        const float footerH = logoSize + ImGui::GetStyle().FramePadding.y * 2.0f
                             + ImGui::GetStyle().ItemSpacing.y + 4.0f;

        ImGui::BeginChild("##left", ImVec2(leftW, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        // Scrollable content area (everything except footer)
        ImGui::BeginChild("##leftContent", ImVec2(0, -footerH), false);

        // Framework settings
        DrawFrameworkSection();

        ImGui::Spacing();
        ImGui::Spacing();

        // Performance (always visible)
        DrawPerformanceSection();

        ImGui::EndChild(); // ##leftContent

        // Fixed footer — always visible at bottom of left pane
        ImGui::Separator();
        {
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

        ImGui::EndChild(); // ##left

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
        ImGui::BeginChild("##right", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        size_t count = gEffectLoader.Count();

        if (count == 0)
        {
            ImGui::TextDisabled("No effect plugins loaded");
        }
        else
        {
            // Sticky header: preset selector + toggle + expand/collapse
            DrawPresetSection();

            {
                bool desired = gAllEffectsOn;
                if (ImGui::Checkbox("Toggle Effects", &desired))
                    SetAllEffectsEnabled(desired);
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Expand All"))
                gForceCollapseState = 1;
            ImGui::SameLine();
            if (ImGui::SmallButton("Collapse All"))
                gForceCollapseState = -1;

            // Search/filter box: case-insensitive substring match on effect name.
            // Empty filter shows everything; this state is intentionally not persisted.
            static char sFilterBuf[64] = {};
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputTextWithHint("##effectfilter", "Search effects...", sFilterBuf, sizeof(sFilterBuf));

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Scrollable effects area
            ImGui::BeginChild("##effects", ImVec2(0, 0), false);

            int shown = 0;
            for (size_t i = 0; i < count; i++)
            {
                const LoadedEffect& le = gEffectLoader.GetEffect(i);
                if (!le.initialized) continue;

                if (sFilterBuf[0] && !EffectNameMatchesFilter(le.desc.name, sFilterBuf))
                    continue;

                if (shown > 0)
                {
                    ImGui::Spacing();
                    ImGui::Spacing();
                }

                ImGui::PushID((int)i);
                DrawEffectSection(i);
                ImGui::PopID();
                ++shown;
            }
            if (shown == 0 && sFilterBuf[0])
                ImGui::TextDisabled("No effect matches \"%s\"", sFilterBuf);
            gForceCollapseState = 0;

            ImGui::EndChild(); // ##effects
        }

        ImGui::EndChild(); // ##right

        ImGui::End();
    }

    ImGui::Render();
    gContext->OMSetRenderTargets(1, &gBackBufferRTV, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

} // namespace DustGUI
