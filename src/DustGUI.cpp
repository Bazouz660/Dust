#include "DustGUI.h"
#include "DustLog.h"
#include "EffectLoader.h"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdio>

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
static HWND gHWnd = nullptr;
static WNDPROC oWndProc = nullptr;
static ID3D11Device* gDevice = nullptr;
static ID3D11DeviceContext* gContext = nullptr;
static ID3D11RenderTargetView* gBackBufferRTV = nullptr;

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
    switch (s.type) {
    case DUST_SETTING_BOOL:  v.bVal = *(bool*)s.valuePtr; break;
    case DUST_SETTING_FLOAT: v.fVal = *(float*)s.valuePtr; break;
    case DUST_SETTING_INT:   v.iVal = *(int*)s.valuePtr; break;
    }
    return v;
}

static void SetValue(const DustSettingDesc& s, const SavedValue& v)
{
    switch (s.type) {
    case DUST_SETTING_BOOL:  *(bool*)s.valuePtr = v.bVal; break;
    case DUST_SETTING_FLOAT: *(float*)s.valuePtr = v.fVal; break;
    case DUST_SETTING_INT:   *(int*)s.valuePtr = v.iVal; break;
    }
}

static bool IsDirty(const DustSettingDesc& s, const SavedValue& saved)
{
    switch (s.type) {
    case DUST_SETTING_BOOL:  return *(bool*)s.valuePtr != saved.bVal;
    case DUST_SETTING_FLOAT: return *(float*)s.valuePtr != saved.fVal;
    case DUST_SETTING_INT:   return *(int*)s.valuePtr != saved.iVal;
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

static bool IsEffectDirty(size_t idx)
{
    const LoadedEffect& le = gEffectLoader.GetEffect(idx);
    if (idx >= gEffectStates.size() || !gEffectStates[idx].snapshotted)
        return false;
    for (uint32_t i = 0; i < le.desc.settingCount; i++)
        if (IsDirty(le.desc.settings[i], gEffectStates[idx].diskValues[i]))
            return true;
    return false;
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

// ==================== WndProc ====================

static void OnOverlayOpen(HWND hWnd)
{
    gToastActive = false;

    // Save cursor clip rect and release it so the cursor can move freely
    gHadClipRect = (GetClipCursor(&gSavedClipRect) != 0);
    ClipCursor(nullptr);

    // Show system cursor
    while (ShowCursor(TRUE) < 0) {}
}

static void OnOverlayClose()
{
    // Restore cursor clipping (game usually clips cursor to window)
    if (gHadClipRect)
        ClipCursor(&gSavedClipRect);

    // Hide system cursor (game manages its own)
    while (ShowCursor(FALSE) >= 0) {}
}

static LRESULT CALLBACK DustWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Capture key binding when waiting for a new toggle key
    if (gWaitingForKey && msg == WM_KEYDOWN)
    {
        if (wParam != VK_ESCAPE) // Escape cancels
            gFwConfig.toggleKey = (int)wParam;
        gWaitingForKey = false;
        return 0;
    }

    if (msg == WM_KEYDOWN && (int)wParam == gFwConfig.toggleKey && !(lParam & 0x40000000))
    {
        gOverlayVisible = !gOverlayVisible;
        if (gOverlayVisible)
            OnOverlayOpen(hWnd);
        else
            OnOverlayClose();
        return 0;
    }

    if (gOverlayVisible)
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

        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return 0;

        // Block all mouse, keyboard, and raw input from reaching the game
        if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
            (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
            msg == WM_INPUT)
            return 0;
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

    // Auto-load last preset
    if (!gFwConfig.lastPreset.empty())
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
        ImGui::SetTooltip("Write logs to Dust.log next to the DLL");

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
        if (ImGui::Button("Overwrite", ImVec2(0, 0)))
        {
            gEffectLoader.SavePreset(currentPreset);
            SnapshotAllEffects();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Overwrite '%s' with current settings", presets[currentPreset].name.c_str());

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

    // Build header label with enabled status and dirty indicator
    bool dirty = IsEffectDirty(idx);
    bool enabled = IsEffectEnabled(le);
    char headerLabel[256];
    snprintf(headerLabel, sizeof(headerLabel), "%s  %s%s",
             name, enabled ? "[ON]" : "[OFF]", dirty ? "  *" : "");

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
        if (!s.name || !s.valuePtr) continue;

        // Skip hidden settings (persisted in INI but not shown in GUI)
        if (s.type == DUST_SETTING_HIDDEN_FLOAT || s.type == DUST_SETTING_HIDDEN_INT
            || s.type == DUST_SETTING_HIDDEN_BOOL)
            continue;

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

    // Save / Reset All buttons
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool canSave = (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_CONFIG))
                || le.desc.SaveSettings;
    bool canLoad = (le.desc.apiVersion >= 3 && (le.desc.flags & DUST_FLAG_FRAMEWORK_CONFIG))
                || le.desc.LoadSettings;

    if (dirty)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    if (ImGui::Button("Save", ImVec2(80, 0)) && dirty && canSave)
    {
        gEffectLoader.SaveEffectConfig(idx);
        SnapshotEffect(idx); // update disk values
    }
    if (dirty)
        ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
    {
        if (!canSave)
            ImGui::SetTooltip("This effect does not support saving");
        else if (!dirty)
            ImGui::SetTooltip("No changes to save");
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset All", ImVec2(80, 0)) && dirty)
    {
        if (canLoad)
        {
            gEffectLoader.LoadEffectConfig(idx);
            SnapshotEffect(idx);
        }
        else
        {
            // Fallback: restore from snapshot
            for (uint32_t i = 0; i < le.desc.settingCount; i++)
                SetValue(le.desc.settings[i], gEffectStates[idx].diskValues[i]);
            if (le.desc.OnSettingChanged)
                le.desc.OnSettingChanged();
        }
    }

    if (dirty)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "*");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Unsaved changes");
    }
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

// ==================== Init / Shutdown / Render ====================

bool Init(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (gInitialized) return true;  // Already initialized (e.g. ResizeBuffers ran before first Present)

    gDevice = device;
    gContext = context;

    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);
    gHWnd = desc.OutputWindow;
    if (!gHWnd) { Log("GUI: No HWND"); return false; }

    if (!CreateBackBufferRTV(swapChain))
    { Log("GUI: Failed to create back buffer RTV"); return false; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Style
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Alpha = 0.95f;
    style.WindowPadding = ImVec2(10, 10);
    style.ItemSpacing = ImVec2(8, 5);

    ImGui_ImplWin32_Init(gHWnd);
    ImGui_ImplDX11_Init(gDevice, gContext);

    oWndProc = (WNDPROC)SetWindowLongPtrW(gHWnd, GWLP_WNDPROC, (LONG_PTR)DustWndProc);

    LoadFrameworkConfig();

    gInitialized = true;
    Log("GUI: Initialized (F11 to toggle)");
    return true;
}

void Shutdown()
{
    if (!gInitialized) return;

    if (gOverlayVisible)
        OnOverlayClose();

    if (oWndProc && gHWnd)
        SetWindowLongPtrW(gHWnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
    oWndProc = nullptr;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (gBackBufferRTV) { gBackBufferRTV->Release(); gBackBufferRTV = nullptr; }

    gEffectStates.clear();
    gInitialized = false;
}

bool IsVisible()
{
    return gOverlayVisible;
}

void Render()
{
    if (!gInitialized) return;

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
