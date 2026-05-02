#pragma once

#include "DustAPI.h"
#include <vector>
#include <string>
#include <windows.h>

struct LoadedEffect {
    HMODULE         hModule;
    DustEffectDesc  desc;
    bool            initialized;

    // v3: Framework-managed GPU timing
    // Phase 0 = preExecute, phase 1 = postExecute; each double-buffered [slot 0/1]
    ID3D11Query*    tsDisjoint[2][2]  = {};
    ID3D11Query*    tsBegin[2][2]     = {};
    ID3D11Query*    tsEnd[2][2]       = {};
    int             timingSlot[2]     = {};   // per-phase: which buffer to record into
    int             timingWarmup[2]   = {};   // per-phase: counts up to 2 before valid read
    float           gpuTimePre        = 0.0f;
    float           gpuTimePost       = 0.0f;
    float           gpuTimeMs         = 0.0f; // combined

    // v3: Framework-managed config
    std::string     effectDir;      // directory containing the DLL
    std::string     configPath;     // full path to .ini
    FILETIME        configMtime;    // for hot-reload
};

// Where a preset came from. Local presets live under <modDir>/presets/ and are
// fully writable. Mod/Workshop presets are discovered in other mods' folders
// and treated as read-only — the user can load them but not save/delete in
// place (Save As copies them to Local).
enum class PresetSource {
    Local,
    Mod,
    Workshop,
};

// Global preset system — each preset is a folder with per-effect INI files
// plus an optional dust_preset.ini metadata file.
struct PresetInfo {
    std::string name;        // display name (folder name)
    std::string path;        // full path to the preset folder
    std::string warnings;    // non-empty if preset has outdated INI files (missing/unknown fields)

    // Discovery source — Mod/Workshop are read-only.
    PresetSource source = PresetSource::Local;
    std::string  sourceLabel; // mod folder name, workshop ID, or empty for Local

    // Index in <game>/data/mods.cfg (0 = first / lowest priority, last = highest).
    // -1 for Local presets and for external presets whose mod isn't in mods.cfg
    // (those are filtered out of presets_ before reaching here).
    int loadOrderRank = -1;

    // Metadata from dust_preset.ini (empty if file is absent — backwards
    // compatible with pre-metadata presets).
    bool        hasMetadata    = false;
    std::string metaName;        // [Preset] Name (display name; falls back to folder name)
    std::string metaAuthor;      // [Preset] Author
    std::string metaDescription; // [Preset] Description
    int         metaVersion     = 0;  // [Preset] Version (user-defined)
    int         metaApiVersion  = 0;  // [Preset] ApiVersion (DUST_API_VERSION at save time)
    bool        metaIsDefault   = false; // [Preset] Default — modder claims this as the default

    bool isReadOnly() const { return source != PresetSource::Local; }
};

class EffectLoader {
public:
    // Scan effects/ folder and load all plugin DLLs.
    // gameDir is the Kenshi root (used to discover presets shipped by other
    // mods and Steam Workshop items); pass nullptr to skip external discovery.
    int LoadAll(const char* effectsDir, const char* gameDir = nullptr);

    // Initialize all loaded plugins (call after device capture)
    bool InitAll(ID3D11Device* device, uint32_t w, uint32_t h);

    // Shut down and re-initialize all plugins on a (possibly different) device.
    // Used when the captured device turns out to be wrong (multi-monitor).
    bool ReinitAll(ID3D11Device* device, uint32_t w, uint32_t h);

    // Dispatch callbacks for a given injection point
    void DispatchPre(DustInjectionPoint point, const DustFrameContext* ctx);
    void DispatchPost(DustInjectionPoint point, const DustFrameContext* ctx);

    void CapturePreFogHDR(ID3D11DeviceContext* ctx);
    void OnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h);
    void ShutdownAll();

    size_t Count() const { return effects_.size(); }
    const LoadedEffect& GetEffect(size_t index) const { return effects_[index]; }
    bool IsInitialized() const { return initialized_; }

    // v3: Framework config helpers (called by DustGUI)
    void SaveEffectConfig(size_t index);
    void LoadEffectConfig(size_t index);

    // Global preset system
    void ScanPresets();
    void ValidatePreset(int presetIdx);             // check for missing/unknown fields in preset INIs
    void LoadPreset(int presetIdx);                // load all effect configs from preset folder
    void SavePreset(int presetIdx);                // save all effect configs to preset folder
    int  SavePresetAs(const char* name);           // create new preset from current settings, returns index
    void DeletePreset(int presetIdx);

    // Import a preset from an arbitrary folder. Validates source contains
    // INI files (lenient — dust_preset.ini is preferred but not required),
    // copies into <presetsDir>/<name>/, and returns the new index. If a
    // preset with the same name already exists and overwrite is true, it is
    // overwritten; otherwise -1 is returned. errorOut (if non-null) gets a
    // human-readable error message on failure.
    int  ImportPresetFromFolder(const char* srcDir, bool overwrite, std::string* errorOut);

    // Copy <presetsDir>/<presetName>/ into <destParentDir>/<presetName>/.
    // Returns true on success.
    bool ExportPreset(int presetIdx, const char* destParentDir, std::string* errorOut);

    // Update the preset's author/description and rewrite dust_preset.ini.
    void UpdatePresetMetadata(int presetIdx, const char* author, const char* description);

    const std::vector<PresetInfo>& GetPresets() const { return presets_; }
    int  GetCurrentPreset() const { return currentPreset_; }
    void SetCurrentPreset(int idx) { currentPreset_ = idx; }

    // Picks the preset to load on first launch, when the user has no saved
    // choice. Resolution order: external Default=1 with highest mods.cfg
    // load-order index → Local Default=1 → "dust_high" → -1.
    int  FindFirstLaunchDefaultIdx() const;

    // Public so discovery helpers can populate metadata on each found folder.
    static void ReadPresetMetadata(PresetInfo& info);

    // v3: GPU timing access (for DustGUI)
    float GetEffectGpuTime(size_t index) const;

private:
    std::vector<LoadedEffect> effects_;
    DustHostAPI hostAPI_ = {};

    bool initialized_ = false;

    // Preset state
    std::string presetsDir_;            // <modDir>/presets/  (writable, "Local")
    std::string gameDir_;               // Kenshi root, for discovering external presets
    std::vector<PresetInfo> presets_;
    int currentPreset_ = -1;            // -1 = custom

    void BuildHostAPI();

    // v3: Config I/O helpers
    static void EffectConfigLoad(LoadedEffect& le);
    static void EffectConfigSave(LoadedEffect& le);
    static void EffectConfigWriteDefaults(LoadedEffect& le);
    static void EffectConfigCheckHotReload(LoadedEffect& le);

    // Preset I/O helpers
    static void EffectConfigLoadFrom(LoadedEffect& le, const std::string& presetDir);
    static void EffectConfigSaveTo(LoadedEffect& le, const std::string& presetDir);

    // Metadata helpers (Read is public above)
    static void WritePresetMetadata(const PresetInfo& info);

    // v3: GPU timing helpers (phase: 0=pre, 1=post)
    void CollectTiming(LoadedEffect& le, ID3D11DeviceContext* ctx, int phase);
    void BeginTiming(LoadedEffect& le, ID3D11DeviceContext* ctx, int phase);
    void EndTiming(LoadedEffect& le, ID3D11DeviceContext* ctx, int phase);
};

extern EffectLoader gEffectLoader;
