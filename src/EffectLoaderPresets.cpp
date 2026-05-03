#include "EffectLoader.h"
#include "DustLog.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ==================== Global Preset System ====================
// A preset is a folder containing dust_preset.ini at its root and effect INIs
// inside an effects/ subfolder, e.g.
//   <presetDir>/dust_preset.ini
//   <presetDir>/effects/SSAO.ini, DOF.ini, ...
//
// Layouts by source:
//   - Local:    <DustMod>/presets/<presetName>/...
//   - External: <modRoot>/...                 (mod folder IS the preset)
//
// For backwards compatibility, EffectConfigLoadFrom also reads <presetDir>/<X>.ini
// when the new path is absent — so flat-layout presets from older Dust versions
// keep working until they're saved (which writes to the new path).

// Metadata filename — sentinel that marks a folder as a Dust preset.
static const char* kPresetMetaFile = "dust_preset.ini";
static const char* kPresetMetaSection = "Preset";
// Subfolder holding effect INIs in the canonical preset layout.
static const char* kEffectsSubdir = "effects";

// Path Dust uses to read effect settings. New canonical location is
// <presetDir>/effects/<section>.ini; falls back to a flat <presetDir>/<section>.ini
// for legacy presets that haven't been re-saved yet.
static std::string EffectIniReadPath(const std::string& presetDir, const std::string& section)
{
    std::string newPath = presetDir + "\\" + kEffectsSubdir + "\\" + section + ".ini";
    if (GetFileAttributesA(newPath.c_str()) != INVALID_FILE_ATTRIBUTES) return newPath;
    return presetDir + "\\" + section + ".ini";
}

// Path Dust writes effect settings to. Always the canonical (new) location.
static std::string EffectIniWritePath(const std::string& presetDir, const std::string& section)
{
    return presetDir + "\\" + kEffectsSubdir + "\\" + section + ".ini";
}

void EffectLoader::ReadPresetMetadata(PresetInfo& info)
{
    info.hasMetadata = false;
    info.metaName.clear();
    info.metaAuthor.clear();
    info.metaDescription.clear();
    info.metaVersion = 0;
    info.metaApiVersion = 0;

    std::string metaPath = info.path + "\\" + kPresetMetaFile;
    DWORD attr = GetFileAttributesA(metaPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;

    info.hasMetadata = true;

    char buf[1024];
    GetPrivateProfileStringA(kPresetMetaSection, "Name",        "", buf, sizeof(buf), metaPath.c_str());
    info.metaName = buf;
    GetPrivateProfileStringA(kPresetMetaSection, "Author",      "", buf, sizeof(buf), metaPath.c_str());
    info.metaAuthor = buf;
    GetPrivateProfileStringA(kPresetMetaSection, "Description", "", buf, sizeof(buf), metaPath.c_str());
    info.metaDescription = buf;
    info.metaVersion    = GetPrivateProfileIntA(kPresetMetaSection, "Version",    0, metaPath.c_str());
    info.metaApiVersion = GetPrivateProfileIntA(kPresetMetaSection, "ApiVersion", 0, metaPath.c_str());
    info.metaIsDefault  = GetPrivateProfileIntA(kPresetMetaSection, "Default",    0, metaPath.c_str()) != 0;
}

void EffectLoader::WritePresetMetadata(const PresetInfo& info)
{
    std::string metaPath = info.path + "\\" + kPresetMetaFile;
    char buf[64];

    const std::string& name = info.metaName.empty() ? info.name : info.metaName;
    WritePrivateProfileStringA(kPresetMetaSection, "Name",        name.c_str(),                metaPath.c_str());
    WritePrivateProfileStringA(kPresetMetaSection, "Author",      info.metaAuthor.c_str(),     metaPath.c_str());
    WritePrivateProfileStringA(kPresetMetaSection, "Description", info.metaDescription.c_str(),metaPath.c_str());

    snprintf(buf, sizeof(buf), "%d", info.metaVersion > 0 ? info.metaVersion : 1);
    WritePrivateProfileStringA(kPresetMetaSection, "Version", buf, metaPath.c_str());

    snprintf(buf, sizeof(buf), "%d", DUST_API_VERSION);
    WritePrivateProfileStringA(kPresetMetaSection, "ApiVersion", buf, metaPath.c_str());

    // Preserve the Default flag — set by mod authors via manual INI editing,
    // not surfaced in the GUI. Only written when set so we don't pollute
    // user-created INIs with Default=0.
    if (info.metaIsDefault)
        WritePrivateProfileStringA(kPresetMetaSection, "Default", "1", metaPath.c_str());
}

void EffectLoader::EffectConfigLoadFrom(LoadedEffect& le, const std::string& presetDir)
{
    const char* section = le.desc.configSection ? le.desc.configSection : le.desc.name;
    if (!section) return;

    std::string iniPath = EffectIniReadPath(presetDir, section);

    // If this effect has no INI in the preset, disable it
    DWORD attr = GetFileAttributesA(iniPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        for (uint32_t i = 0; i < le.desc.settingCount; i++)
        {
            const DustSettingDesc& s = le.desc.settings[i];
            const char* key = s.iniKey ? s.iniKey : s.name;
            if (s.type == DUST_SETTING_BOOL && s.valuePtr &&
                key && _stricmp(key, "Enabled") == 0)
            {
                *(bool*)s.valuePtr = false;
                break;
            }
        }
        return;
    }

    const char* sentinel = "\x01\x02MISSING";

    for (uint32_t i = 0; i < le.desc.settingCount; i++)
    {
        const DustSettingDesc& s = le.desc.settings[i];
        if (s.type == DUST_SETTING_SECTION) continue;
        if (!s.valuePtr) continue;

        const char* key = s.iniKey ? s.iniKey : s.name;
        if (!key) continue;

        char probe[64];
        GetPrivateProfileStringA(section, key, sentinel, probe, sizeof(probe), iniPath.c_str());
        if (strcmp(probe, sentinel) == 0) continue;

        switch (s.type)
        {
        case DUST_SETTING_BOOL:
        case DUST_SETTING_HIDDEN_BOOL:
            *(bool*)s.valuePtr = (atoi(probe) != 0);
            break;
        case DUST_SETTING_FLOAT:
        case DUST_SETTING_HIDDEN_FLOAT:
        {
            float val = (float)atof(probe);
            if (s.minVal < s.maxVal)
                val = (val < s.minVal) ? s.minVal : (val > s.maxVal) ? s.maxVal : val;
            *(float*)s.valuePtr = val;
            break;
        }
        case DUST_SETTING_INT:
        case DUST_SETTING_HIDDEN_INT:
        {
            int val = atoi(probe);
            if (s.minVal < s.maxVal)
                val = (val < (int)s.minVal) ? (int)s.minVal : (val > (int)s.maxVal) ? (int)s.maxVal : val;
            *(int*)s.valuePtr = val;
            break;
        }
        case DUST_SETTING_ENUM:
        {
            int count = 0;
            if (s.enumLabels) while (s.enumLabels[count]) count++;
            int val = atoi(probe);
            if (count > 0)
                val = (val < 0) ? 0 : (val >= count ? count - 1 : val);
            *(int*)s.valuePtr = val;
            break;
        }
        case DUST_SETTING_COLOR3:
        {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (sscanf_s(probe, "%f,%f,%f", &r, &g, &b) == 3)
            {
                if (s.minVal < s.maxVal)
                {
                    r = (r < s.minVal) ? s.minVal : (r > s.maxVal ? s.maxVal : r);
                    g = (g < s.minVal) ? s.minVal : (g > s.maxVal ? s.maxVal : g);
                    b = (b < s.minVal) ? s.minVal : (b > s.maxVal ? s.maxVal : b);
                }
                float* dst = (float*)s.valuePtr;
                dst[0] = r; dst[1] = g; dst[2] = b;
            }
            break;
        }
        }
    }
}

void EffectLoader::EffectConfigSaveTo(LoadedEffect& le, const std::string& presetDir)
{
    const char* section = le.desc.configSection ? le.desc.configSection : le.desc.name;
    if (!section) return;

    // Ensure the canonical effects/ subfolder exists before writing.
    std::string effectsDir = presetDir + "\\" + kEffectsSubdir;
    CreateDirectoryA(effectsDir.c_str(), nullptr);

    std::string iniPath = EffectIniWritePath(presetDir, section);
    char buf[64];

    WritePrivateProfileStringA(section, nullptr, nullptr, iniPath.c_str());

    // Migrate away from legacy flat layout: if a same-named INI exists at the
    // preset root from an older Dust version, drop it so it doesn't shadow
    // the new file via the read fallback.
    std::string legacyPath = presetDir + "\\" + std::string(section) + ".ini";
    if (legacyPath != iniPath)
        DeleteFileA(legacyPath.c_str());

    for (uint32_t i = 0; i < le.desc.settingCount; i++)
    {
        const DustSettingDesc& s = le.desc.settings[i];
        if (s.type == DUST_SETTING_SECTION) continue;
        if (!s.valuePtr) continue;

        const char* key = s.iniKey ? s.iniKey : s.name;
        if (!key) continue;

        switch (s.type)
        {
        case DUST_SETTING_BOOL:
        case DUST_SETTING_HIDDEN_BOOL:
            WritePrivateProfileStringA(section, key, *(bool*)s.valuePtr ? "1" : "0", iniPath.c_str());
            break;
        case DUST_SETTING_FLOAT:
        case DUST_SETTING_HIDDEN_FLOAT:
            snprintf(buf, sizeof(buf), "%g", *(float*)s.valuePtr);
            WritePrivateProfileStringA(section, key, buf, iniPath.c_str());
            break;
        case DUST_SETTING_INT:
        case DUST_SETTING_HIDDEN_INT:
        case DUST_SETTING_ENUM:
            snprintf(buf, sizeof(buf), "%d", *(int*)s.valuePtr);
            WritePrivateProfileStringA(section, key, buf, iniPath.c_str());
            break;
        case DUST_SETTING_COLOR3:
        {
            const float* c = (const float*)s.valuePtr;
            snprintf(buf, sizeof(buf), "%g,%g,%g", c[0], c[1], c[2]);
            WritePrivateProfileStringA(section, key, buf, iniPath.c_str());
            break;
        }
        }
    }
}

// Read <gameDir>/data/mods.cfg into a list of .mod filenames in load order.
// Returns empty on missing/unreadable. Each line is one mod.
static std::vector<std::string> ReadModsCfg(const std::string& gameDir)
{
    std::vector<std::string> order;
    if (gameDir.empty()) return order;

    std::string path = gameDir + "\\data\\mods.cfg";
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "r");
    if (!f)
    {
        Log("ReadModsCfg: not found at '%s'", path.c_str());
        return order;
    }
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t'))
            s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i > 0) s.erase(0, i);
        if (!s.empty()) order.push_back(std::move(s));
    }
    fclose(f);
    return order;
}

// Index of `modFilename` in `loadOrder` (case-insensitive), or -1 if absent.
// An empty loadOrder is treated as "no mods enabled" — everything returns -1.
static int LoadOrderIndex(const std::vector<std::string>& loadOrder,
                          const std::string& modFilename)
{
    if (modFilename.empty()) return -1;
    for (int j = 0; j < (int)loadOrder.size(); j++)
        if (_stricmp(loadOrder[j].c_str(), modFilename.c_str()) == 0) return j;
    return -1;
}

// Enumerate immediate subdirectories of `dir`, calling cb(name, fullPath) for
// each. Skips "."/"..". Returns the number of entries visited (0 if dir is
// missing or empty).
template <typename Fn>
static int ForEachSubdir(const std::string& dir, Fn cb)
{
    char search[MAX_PATH];
    snprintf(search, sizeof(search), "%s\\*", dir.c_str());

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        cb(fd.cFileName, dir + "\\" + fd.cFileName);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

// True if `parent\leaf` exists as a regular file.
static bool FileExistsAt(const std::string& parent, const char* leaf)
{
    std::string p = parent + "\\" + leaf;
    DWORD attr = GetFileAttributesA(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Find the first *.mod file in `dir` and return its basename (e.g. "Foo.mod").
// Empty if no .mod file is present.
static std::string FindModFilename(const std::string& dir)
{
    std::string out;
    char search[MAX_PATH];
    snprintf(search, sizeof(search), "%s\\*.mod", dir.c_str());
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            out = fd.cFileName;
        FindClose(h);
    }
    return out;
}

static void EmitPreset(std::vector<PresetInfo>& out,
                       const std::string& path,
                       const std::string& name,
                       PresetSource source,
                       const std::string& sourceLabel,
                       int loadOrderRank)
{
    PresetInfo info;
    info.name = name;
    info.path = path;
    info.source = source;
    info.sourceLabel = sourceLabel;
    info.loadOrderRank = loadOrderRank;
    EffectLoader::ReadPresetMetadata(info);
    out.push_back(std::move(info));
}

// Scan the local presets dir — every immediate subfolder is treated as a
// preset (back-compat with hand-rolled folders that lack dust_preset.ini).
static void ScanLocalPresets(std::vector<PresetInfo>& out,
                             const std::string& presetRoot)
{
    int n = ForEachSubdir(presetRoot, [&](const char* name, const std::string& path) {
        EmitPreset(out, path, name, PresetSource::Local, "", -1);
    });
    if (n > 0) Log("ScanPresets:   '%s' -> %d preset(s)", presetRoot.c_str(), n);
}

// A mod folder IS the preset when dust_preset.ini sits at its root. The
// preset's display name comes from sourceLabel (the mod folder name); its
// effect INIs live in <modRoot>/effects/.
static void ScanExternalMod(std::vector<PresetInfo>& out,
                            const std::string& modRoot,
                            PresetSource source,
                            const std::string& sourceLabel,
                            int loadOrderRank)
{
    if (!FileExistsAt(modRoot, kPresetMetaFile)) return;
    EmitPreset(out, modRoot, sourceLabel, source, sourceLabel, loadOrderRank);
    Log("ScanPresets:   '%s' -> 1 preset", modRoot.c_str());
}

// Walk every mod under `containerDir` (the game's mods/ folder or the
// workshop content folder). Mods absent from loadOrder are skipped, as are
// Dust's own installs — folders containing Dust.dll — so the framework's
// bundled presets aren't re-listed when Dust is also subscribed via Workshop.
static void ScanContainer(std::vector<PresetInfo>& out,
                          const std::string& containerDir,
                          PresetSource source,
                          const std::vector<std::string>& loadOrder)
{
    Log("ScanPresets: scanning container '%s'", containerDir.c_str());

    ForEachSubdir(containerDir, [&](const char* name, const std::string& entryDir) {
        if (FileExistsAt(entryDir, "Dust.dll"))
        {
            Log("ScanPresets:   skipping '%s' (Dust install)", name);
            return;
        }

        std::string modFilename = FindModFilename(entryDir);
        int rank = LoadOrderIndex(loadOrder, modFilename);
        if (rank < 0)
        {
            Log("ScanPresets:   skipping '%s' (mod '%s' not enabled in mods.cfg)",
                name, modFilename.empty() ? "<no .mod file>" : modFilename.c_str());
            return;
        }

        ScanExternalMod(out, entryDir, source, name, rank);
    });
}

void EffectLoader::ScanPresets()
{
    presets_.clear();
    currentPreset_ = -1;

    if (presetsDir_.empty()) return;

    Log("ScanPresets: presetsDir='%s' gameDir='%s'",
        presetsDir_.c_str(), gameDir_.c_str());

    // mods.cfg drives both the disabled-mod filter and load-order ranking.
    // If missing/empty, no external mods are considered enabled and the
    // dropdown shows only Local presets.
    auto loadOrder = ReadModsCfg(gameDir_);
    Log("ScanPresets: %zu enabled mod(s) per mods.cfg", loadOrder.size());

    CreateDirectoryA(presetsDir_.c_str(), nullptr);

    ScanLocalPresets(presets_, presetsDir_);
    size_t localCount = presets_.size();

    size_t modCount = 0, workshopCount = 0;
    if (!gameDir_.empty())
    {
        ScanContainer(presets_, gameDir_ + "\\mods", PresetSource::Mod, loadOrder);
        modCount = presets_.size() - localCount;

        // 233860 is Kenshi's Steam app id; silently skipped on non-Steam installs.
        ScanContainer(presets_, gameDir_ + "\\..\\..\\workshop\\content\\233860",
                      PresetSource::Workshop, loadOrder);
        workshopCount = presets_.size() - localCount - modCount;
    }

    // Sort by source first (Local → Mod → Workshop) so the user's own
    // presets cluster at the top of the dropdown.
    std::sort(presets_.begin(), presets_.end(),
        [](const PresetInfo& a, const PresetInfo& b) {
            if (a.source != b.source) return (int)a.source < (int)b.source;
            if (a.name   != b.name)   return a.name < b.name;
            return a.sourceLabel < b.sourceLabel;
        });

    Log("Found %zu preset(s): %zu local, %zu mod, %zu workshop",
        presets_.size(), localCount, modCount, workshopCount);
}

int EffectLoader::FindFirstLaunchDefaultIdx() const
{
    // External default with the highest load-order rank wins (Kenshi
    // convention: later mods override earlier).
    int bestIdx = -1, bestRank = -1;
    for (int i = 0; i < (int)presets_.size(); i++)
    {
        const PresetInfo& p = presets_[i];
        if (!p.metaIsDefault) continue;
        if (p.source == PresetSource::Local) continue;
        if (p.loadOrderRank > bestRank) { bestRank = p.loadOrderRank; bestIdx = i; }
    }
    if (bestIdx >= 0)
    {
        Log("FindDefault: external default '%s' from '%s' (load-order rank %d)",
            presets_[bestIdx].name.c_str(),
            presets_[bestIdx].sourceLabel.c_str(), bestRank);
        return bestIdx;
    }

    for (int i = 0; i < (int)presets_.size(); i++)
        if (presets_[i].metaIsDefault && presets_[i].source == PresetSource::Local)
        {
            Log("FindDefault: local default '%s'", presets_[i].name.c_str());
            return i;
        }

    for (int i = 0; i < (int)presets_.size(); i++)
        if (presets_[i].source == PresetSource::Local && presets_[i].name == "dust_high")
        {
            Log("FindDefault: falling back to dust_high");
            return i;
        }

    Log("FindDefault: no default preset available");
    return -1;
}

void EffectLoader::ValidatePreset(int presetIdx)
{
    if (presetIdx < 0 || presetIdx >= (int)presets_.size()) return;

    PresetInfo& preset = presets_[presetIdx];
    preset.warnings.clear();

    for (const auto& le : effects_)
    {
        if (!le.initialized) continue;
        if (le.desc.apiVersion < 3 || !(le.desc.flags & DUST_FLAG_FRAMEWORK_CONFIG)) continue;

        const char* section = le.desc.configSection ? le.desc.configSection : le.desc.name;
        if (!section) continue;

        std::string iniPath = EffectIniReadPath(preset.path, section);
        DWORD attr = GetFileAttributesA(iniPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue; // missing INI = disabled, not outdated

        const char* sentinel = "\x01\x02MISSING";

        // Check for missing keys (effect expects them, INI doesn't have them)
        std::string missing;
        std::vector<std::string> expectedKeys;
        for (uint32_t i = 0; i < le.desc.settingCount; i++)
        {
            const DustSettingDesc& s = le.desc.settings[i];
            if (s.type == DUST_SETTING_SECTION) continue;
            const char* key = s.iniKey ? s.iniKey : s.name;
            if (!key) continue;
            expectedKeys.push_back(key);

            char probe[64];
            GetPrivateProfileStringA(section, key, sentinel, probe, sizeof(probe), iniPath.c_str());
            if (strcmp(probe, sentinel) == 0)
            {
                if (!missing.empty()) missing += ", ";
                missing += key;
            }
        }

        // Check for unknown keys (INI has them, effect doesn't expect them)
        // Read all keys from the INI section
        std::string unknown;
        char keysBuf[4096] = {};
        DWORD keysLen = GetPrivateProfileStringA(section, nullptr, "", keysBuf, sizeof(keysBuf), iniPath.c_str());
        if (keysLen > 0)
        {
            const char* p = keysBuf;
            while (*p)
            {
                std::string iniKey(p);
                bool found = false;
                for (const auto& ek : expectedKeys)
                {
                    if (_stricmp(ek.c_str(), iniKey.c_str()) == 0)
                    { found = true; break; }
                }
                if (!found)
                {
                    if (!unknown.empty()) unknown += ", ";
                    unknown += iniKey;
                }
                p += iniKey.size() + 1;
            }
        }

        if (!missing.empty() || !unknown.empty())
        {
            if (!preset.warnings.empty()) preset.warnings += "\n";
            preset.warnings += std::string(le.desc.name) + ": ";
            if (!missing.empty()) preset.warnings += "missing [" + missing + "]";
            if (!missing.empty() && !unknown.empty()) preset.warnings += ", ";
            if (!unknown.empty()) preset.warnings += "unknown [" + unknown + "]";
        }
    }

    if (!preset.warnings.empty())
        Log("Preset '%s' is outdated:\n%s", preset.name.c_str(), preset.warnings.c_str());
}

void EffectLoader::LoadPreset(int presetIdx)
{
    if (presetIdx < 0 || presetIdx >= (int)presets_.size()) return;

    const std::string& presetDir = presets_[presetIdx].path;

    for (auto& le : effects_)
    {
        if (!le.initialized) continue;
        if (le.desc.apiVersion < 3 || !(le.desc.flags & DUST_FLAG_FRAMEWORK_CONFIG)) continue;

        EffectConfigLoadFrom(le, presetDir);

        if (le.desc.OnSettingChanged)
            le.desc.OnSettingChanged();
    }

    currentPreset_ = presetIdx;
    Log("Loaded global preset '%s'", presets_[presetIdx].name.c_str());
}

void EffectLoader::SavePreset(int presetIdx)
{
    if (presetIdx < 0 || presetIdx >= (int)presets_.size()) return;

    PresetInfo& preset = presets_[presetIdx];
    if (preset.isReadOnly())
    {
        Log("SavePreset: refusing to write to read-only preset '%s' (source=%s)",
            preset.name.c_str(), preset.sourceLabel.c_str());
        return;
    }
    const std::string& presetDir = preset.path;

    for (auto& le : effects_)
    {
        if (!le.initialized) continue;
        if (le.desc.apiVersion < 3 || !(le.desc.flags & DUST_FLAG_FRAMEWORK_CONFIG)) continue;

        EffectConfigSaveTo(le, presetDir);
    }

    // Refresh metadata: preserve author/description/version (re-read from disk
    // in case the user edited dust_preset.ini), refresh Name + ApiVersion. If
    // the file doesn't exist yet (existing pre-feature preset being saved for
    // the first time), this generates fresh metadata.
    PresetInfo prev = preset;
    ReadPresetMetadata(preset); // re-read in case user edited Author/Description on disk
    if (!preset.hasMetadata)
    {
        preset.metaAuthor = prev.metaAuthor;
        preset.metaDescription = prev.metaDescription;
        preset.metaVersion = prev.metaVersion > 0 ? prev.metaVersion : 1;
    }
    preset.metaName = preset.name;
    preset.hasMetadata = true;
    WritePresetMetadata(preset);

    preset.warnings.clear(); // INI is now current
    Log("Saved global preset '%s'", preset.name.c_str());
}

int EffectLoader::SavePresetAs(const char* name)
{
    if (!name || !name[0] || presetsDir_.empty()) return -1;

    // Sanitize folder name
    std::string safeName(name);
    for (char& c : safeName)
    {
        if (c == '\\' || c == '/' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }

    std::string presetDir = presetsDir_ + "\\" + safeName;

    // Create the preset folder
    CreateDirectoryA(presetDir.c_str(), nullptr);

    // Check if a *local* preset with this name already exists. We deliberately
    // ignore Mod/Workshop entries — Save As always targets the local presets
    // folder, so a name collision with a read-only preset is fine (both
    // entries coexist, distinguished by source).
    int existingIdx = -1;
    for (int i = 0; i < (int)presets_.size(); i++)
    {
        if (presets_[i].source == PresetSource::Local &&
            _stricmp(presets_[i].name.c_str(), safeName.c_str()) == 0)
        {
            existingIdx = i;
            break;
        }
    }

    if (existingIdx >= 0)
    {
        SavePreset(existingIdx);
        currentPreset_ = existingIdx;
        return existingIdx;
    }

    // Add and sort
    PresetInfo info;
    info.name = safeName;
    info.path = presetDir;
    info.source = PresetSource::Local;
    presets_.push_back(info);

    std::sort(presets_.begin(), presets_.end(),
        [](const PresetInfo& a, const PresetInfo& b) {
            if (a.source != b.source) return (int)a.source < (int)b.source;
            if (a.name   != b.name)   return a.name < b.name;
            return a.sourceLabel < b.sourceLabel;
        });

    int newIdx = -1;
    for (int i = 0; i < (int)presets_.size(); i++)
    {
        if (presets_[i].source == PresetSource::Local && presets_[i].name == safeName)
        { newIdx = i; break; }
    }

    if (newIdx >= 0)
    {
        SavePreset(newIdx);
        currentPreset_ = newIdx;
    }

    return newIdx;
}

void EffectLoader::DeletePreset(int presetIdx)
{
    if (presetIdx < 0 || presetIdx >= (int)presets_.size()) return;

    if (presets_[presetIdx].isReadOnly())
    {
        Log("DeletePreset: refusing to delete read-only preset '%s' (source=%s)",
            presets_[presetIdx].name.c_str(), presets_[presetIdx].sourceLabel.c_str());
        return;
    }

    const std::string& presetDir = presets_[presetIdx].path;
    auto deleteIniFiles = [](const std::string& dir) {
        char search[MAX_PATH];
        snprintf(search, sizeof(search), "%s\\*.ini", dir.c_str());
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(search, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            DeleteFileA((dir + "\\" + fd.cFileName).c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    };

    // Effect INIs live under effects/ in the canonical layout, but legacy
    // flat-layout presets keep them at the root. Clean both.
    std::string effectsDir = presetDir + "\\" + kEffectsSubdir;
    deleteIniFiles(effectsDir);
    RemoveDirectoryA(effectsDir.c_str());
    deleteIniFiles(presetDir);
    RemoveDirectoryA(presetDir.c_str());

    Log("Deleted global preset '%s'", presets_[presetIdx].name.c_str());

    presets_.erase(presets_.begin() + presetIdx);

    if (currentPreset_ == presetIdx)
        currentPreset_ = -1;
    else if (currentPreset_ > presetIdx)
        currentPreset_--;
}

void EffectLoader::UpdatePresetMetadata(int presetIdx, const char* author, const char* description)
{
    if (presetIdx < 0 || presetIdx >= (int)presets_.size()) return;

    PresetInfo& preset = presets_[presetIdx];
    if (preset.isReadOnly())
    {
        Log("UpdatePresetMetadata: refusing to write to read-only preset '%s' (source=%s)",
            preset.name.c_str(), preset.sourceLabel.c_str());
        return;
    }
    preset.metaAuthor      = author      ? author      : "";
    preset.metaDescription = description ? description : "";
    if (preset.metaName.empty()) preset.metaName = preset.name;
    if (preset.metaVersion <= 0) preset.metaVersion = 1;
    preset.hasMetadata = true;
    WritePresetMetadata(preset);
    Log("Updated metadata for preset '%s'", preset.name.c_str());
}

// ==================== Import / Export ====================

// Sanitize a name into something usable as a directory entry (Windows rules).
static std::string SanitizePresetName(const std::string& name)
{
    std::string out = name;
    for (char& c : out)
    {
        if (c == '\\' || c == '/' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    // Strip leading/trailing whitespace and dots (Windows restrictions)
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
    return out;
}

// Extract the leaf name (last path component) from a path with either separator.
static std::string PathLeaf(const std::string& path)
{
    size_t pos = path.find_last_of("\\/");
    std::string leaf = (pos == std::string::npos) ? path : path.substr(pos + 1);
    // Strip trailing separators left over from "C:\foo\bar\"
    while (!leaf.empty() && (leaf.back() == '\\' || leaf.back() == '/'))
        leaf.pop_back();
    return leaf;
}

// Recursively copy every regular file from srcDir into dstDir, preserving
// subfolder structure. dstDir (and any subfolders encountered) are created
// if missing. Returns the total number of files copied.
static int CopyDirectoryTree(const std::string& srcDir, const std::string& dstDir)
{
    CreateDirectoryA(dstDir.c_str(), nullptr);

    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*", srcDir.c_str());

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int copied = 0;
    do {
        if (fd.cFileName[0] == '.') continue;
        std::string src = srcDir + "\\" + fd.cFileName;
        std::string dst = dstDir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            copied += CopyDirectoryTree(src, dst);
        }
        else if (CopyFileA(src.c_str(), dst.c_str(), FALSE))
        {
            copied++;
        }
        else
        {
            Log("CopyDirectoryTree: failed to copy %s -> %s (err=%lu)",
                src.c_str(), dst.c_str(), GetLastError());
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return copied;
}

// Count effect INIs in a preset folder. Looks at <dir>/effects/*.ini (canonical
// layout) and falls back to flat <dir>/*.ini (legacy) for folders that haven't
// been migrated yet. Excludes dust_preset.ini.
static int CountEffectInis(const std::string& dir)
{
    auto countIniFiles = [](const std::string& d) {
        char search[MAX_PATH];
        snprintf(search, sizeof(search), "%s\\*.ini", d.c_str());
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(search, &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        int n = 0;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (_stricmp(fd.cFileName, kPresetMetaFile) == 0) continue;
            n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return n;
    };

    int n = countIniFiles(dir + "\\" + kEffectsSubdir);
    if (n == 0) n = countIniFiles(dir);
    return n;
}

int EffectLoader::ImportPresetFromFolder(const char* srcDir, bool overwrite, std::string* errorOut)
{
    auto setErr = [&](const char* msg) {
        if (errorOut) *errorOut = msg;
        Log("ImportPreset: %s", msg);
    };

    if (!srcDir || !*srcDir) { setErr("Source path is empty"); return -1; }
    if (presetsDir_.empty()) { setErr("Presets directory is not configured"); return -1; }

    DWORD attr = GetFileAttributesA(srcDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        setErr("Source path is not a folder");
        return -1;
    }

    std::string srcStr = srcDir;
    while (!srcStr.empty() && (srcStr.back() == '\\' || srcStr.back() == '/'))
        srcStr.pop_back();

    // Reject importing a folder that lives inside our presets dir already
    // (would create a recursive copy or no-op).
    {
        std::string srcLower = srcStr;       for (char& c : srcLower) c = (char)tolower((unsigned char)c);
        std::string dstLower = presetsDir_;  for (char& c : dstLower) c = (char)tolower((unsigned char)c);
        if (srcLower.compare(0, dstLower.size(), dstLower) == 0)
        {
            setErr("Folder is already inside the presets directory");
            return -1;
        }
    }

    // Check that the folder looks like a Dust preset:
    //   - has dust_preset.ini (strict)  OR
    //   - has at least one *.ini file   (lenient — pre-feature presets, hand-rolled folders)
    std::string metaPath = srcStr + "\\" + kPresetMetaFile;
    bool hasMeta = (GetFileAttributesA(metaPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    int iniCount = CountEffectInis(srcStr);

    if (!hasMeta && iniCount == 0)
    {
        setErr("Folder contains no INI files — does not look like a Dust preset");
        return -1;
    }

    // Determine target name: prefer metadata Name, fall back to source folder name.
    std::string targetName;
    if (hasMeta)
    {
        char buf[256];
        GetPrivateProfileStringA(kPresetMetaSection, "Name", "", buf, sizeof(buf), metaPath.c_str());
        targetName = buf;
    }
    if (targetName.empty()) targetName = PathLeaf(srcStr);
    targetName = SanitizePresetName(targetName);
    if (targetName.empty()) { setErr("Could not derive a valid preset name"); return -1; }

    std::string destDir = presetsDir_ + "\\" + targetName;

    // Collision handling — only check Local entries; Mod/Workshop presets with
    // the same name are unrelated read-only neighbors and don't conflict.
    int existingIdx = -1;
    for (int i = 0; i < (int)presets_.size(); i++)
    {
        if (presets_[i].source == PresetSource::Local &&
            _stricmp(presets_[i].name.c_str(), targetName.c_str()) == 0)
        {
            existingIdx = i;
            break;
        }
    }
    if (existingIdx >= 0 && !overwrite)
    {
        if (errorOut) *errorOut = "A preset named '" + targetName + "' already exists";
        Log("ImportPreset: name collision on '%s' (overwrite=false)", targetName.c_str());
        return -1;
    }

    // If overwriting, wipe destination INIs first so stale entries don't
    // persist. Cleans both the canonical effects/ subfolder and any legacy
    // root-level INIs from older Dust versions.
    if (existingIdx >= 0)
    {
        auto wipeIniFiles = [](const std::string& d) {
            char search[MAX_PATH];
            snprintf(search, sizeof(search), "%s\\*.ini", d.c_str());
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(search, &fd);
            if (h == INVALID_HANDLE_VALUE) return;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                DeleteFileA((d + "\\" + fd.cFileName).c_str());
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        };
        wipeIniFiles(destDir + "\\" + kEffectsSubdir);
        wipeIniFiles(destDir);
    }

    int copied = CopyDirectoryTree(srcStr, destDir);
    if (copied <= 0)
    {
        setErr("Failed to copy any files from source folder");
        return -1;
    }

    // Backfill dust_preset.ini if the source didn't have one.
    if (!hasMeta)
    {
        PresetInfo tmp;
        tmp.name = targetName;
        tmp.path = destDir;
        tmp.metaName = targetName;
        tmp.metaVersion = 1;
        tmp.hasMetadata = true;
        WritePresetMetadata(tmp);
    }

    // Rebuild list (covers add + reorder)
    int prevCurrent = currentPreset_;
    std::string prevCurrentName, prevCurrentLabel;
    PresetSource prevCurrentSource = PresetSource::Local;
    if (prevCurrent >= 0 && prevCurrent < (int)presets_.size())
    {
        prevCurrentName   = presets_[prevCurrent].name;
        prevCurrentLabel  = presets_[prevCurrent].sourceLabel;
        prevCurrentSource = presets_[prevCurrent].source;
    }

    ScanPresets();

    // Restore selection
    if (!prevCurrentName.empty())
    {
        for (int i = 0; i < (int)presets_.size(); i++)
            if (presets_[i].name == prevCurrentName &&
                presets_[i].source == prevCurrentSource &&
                presets_[i].sourceLabel == prevCurrentLabel)
            { currentPreset_ = i; break; }
    }

    // Find new index and validate (always Local — that's where we just imported)
    int newIdx = -1;
    for (int i = 0; i < (int)presets_.size(); i++)
        if (presets_[i].source == PresetSource::Local && presets_[i].name == targetName)
        { newIdx = i; break; }
    if (newIdx >= 0) ValidatePreset(newIdx);

    Log("Imported preset '%s' (%d file%s) from %s",
        targetName.c_str(), copied, copied == 1 ? "" : "s", srcStr.c_str());
    return newIdx;
}

bool EffectLoader::ExportPreset(int presetIdx, const char* destParentDir, std::string* errorOut)
{
    auto setErr = [&](const char* msg) {
        if (errorOut) *errorOut = msg;
        Log("ExportPreset: %s", msg);
    };

    if (presetIdx < 0 || presetIdx >= (int)presets_.size()) { setErr("Invalid preset index"); return false; }
    if (!destParentDir || !*destParentDir) { setErr("Destination path is empty"); return false; }

    DWORD attr = GetFileAttributesA(destParentDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        setErr("Destination is not a folder");
        return false;
    }

    PresetInfo& preset = presets_[presetIdx];

    // Ensure dust_preset.ini exists for the export — backfill silently if the
    // user is exporting a pre-feature preset.
    if (!preset.hasMetadata)
    {
        if (preset.metaName.empty()) preset.metaName = preset.name;
        if (preset.metaVersion <= 0) preset.metaVersion = 1;
        preset.hasMetadata = true;
        WritePresetMetadata(preset);
    }

    std::string destStr = destParentDir;
    while (!destStr.empty() && (destStr.back() == '\\' || destStr.back() == '/'))
        destStr.pop_back();
    std::string destDir = destStr + "\\" + preset.name;

    // Refuse to export into the source folder itself.
    {
        std::string a = destDir;        for (char& c : a) c = (char)tolower((unsigned char)c);
        std::string b = preset.path;    for (char& c : b) c = (char)tolower((unsigned char)c);
        if (a == b) { setErr("Source and destination are the same folder"); return false; }
    }

    int copied = CopyDirectoryTree(preset.path, destDir);
    if (copied <= 0)
    {
        setErr("Failed to copy any files");
        return false;
    }

    Log("Exported preset '%s' (%d file%s) to %s",
        preset.name.c_str(), copied, copied == 1 ? "" : "s", destDir.c_str());
    return true;
}
