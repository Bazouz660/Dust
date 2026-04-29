#include "EffectLoader.h"
#include "DustLog.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ==================== Global Preset System ====================
// Each preset is a folder under <modRoot>/presets/ containing per-effect INI
// files plus an optional dust_preset.ini metadata file.
// e.g. <modRoot>/presets/dust_high/DOF.ini, SSAO.ini, dust_preset.ini, ...

// Metadata filename — sentinel that marks a folder as a Dust preset.
static const char* kPresetMetaFile = "dust_preset.ini";
static const char* kPresetMetaSection = "Preset";

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
}

void EffectLoader::EffectConfigLoadFrom(LoadedEffect& le, const std::string& presetDir)
{
    const char* section = le.desc.configSection ? le.desc.configSection : le.desc.name;
    if (!section) return;

    std::string iniPath = presetDir + "\\" + std::string(section) + ".ini";

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
            if (sscanf(probe, "%f,%f,%f", &r, &g, &b) == 3)
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

    std::string iniPath = presetDir + "\\" + std::string(section) + ".ini";
    char buf[64];

    WritePrivateProfileStringA(section, nullptr, nullptr, iniPath.c_str());

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

void EffectLoader::ScanPresets()
{
    presets_.clear();
    currentPreset_ = -1;

    if (presetsDir_.empty()) return;

    // Ensure presets root directory exists
    CreateDirectoryA(presetsDir_.c_str(), nullptr);

    // Enumerate subdirectories
    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*", presetsDir_.c_str());

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue; // skip . and ..

        PresetInfo info;
        info.name = fd.cFileName;
        info.path = presetsDir_ + "\\" + fd.cFileName;
        ReadPresetMetadata(info); // empty for pre-feature presets — that's OK
        presets_.push_back(std::move(info));
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);

    std::sort(presets_.begin(), presets_.end(),
        [](const PresetInfo& a, const PresetInfo& b) { return a.name < b.name; });

    Log("Found %zu global presets in %s", presets_.size(), presetsDir_.c_str());
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

        std::string iniPath = preset.path + "\\" + std::string(section) + ".ini";
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

    // Check if already in list
    int existingIdx = -1;
    for (int i = 0; i < (int)presets_.size(); i++)
    {
        if (_stricmp(presets_[i].name.c_str(), safeName.c_str()) == 0)
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
    presets_.push_back(info);

    std::sort(presets_.begin(), presets_.end(),
        [](const PresetInfo& a, const PresetInfo& b) { return a.name < b.name; });

    int newIdx = -1;
    for (int i = 0; i < (int)presets_.size(); i++)
    {
        if (presets_[i].name == safeName) { newIdx = i; break; }
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

    const std::string& presetDir = presets_[presetIdx].path;

    // Delete all INI files in the preset folder
    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*.ini", presetDir.c_str());

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do {
            std::string filePath = presetDir + "\\" + fd.cFileName;
            DeleteFileA(filePath.c_str());
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    // Remove the directory
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

// Copy every regular file from srcDir into dstDir (non-recursive — presets are
// flat). dstDir is created if missing. Returns number of files copied.
static int CopyFlatDirectory(const std::string& srcDir, const std::string& dstDir)
{
    CreateDirectoryA(dstDir.c_str(), nullptr);

    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*", srcDir.c_str());

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int copied = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string src = srcDir + "\\" + fd.cFileName;
        std::string dst = dstDir + "\\" + fd.cFileName;
        if (CopyFileA(src.c_str(), dst.c_str(), FALSE))
            copied++;
        else
            Log("CopyFlatDirectory: failed to copy %s -> %s (err=%lu)",
                src.c_str(), dst.c_str(), GetLastError());
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return copied;
}

// Count INI files in a folder (excluding dust_preset.ini).
static int CountEffectInis(const std::string& dir)
{
    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*.ini", dir.c_str());

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (_stricmp(fd.cFileName, kPresetMetaFile) == 0) continue;
        n++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
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

    // Collision handling
    int existingIdx = -1;
    for (int i = 0; i < (int)presets_.size(); i++)
    {
        if (_stricmp(presets_[i].name.c_str(), targetName.c_str()) == 0)
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

    // If overwriting, wipe destination INIs first so stale entries don't persist.
    if (existingIdx >= 0)
    {
        char searchPath[MAX_PATH];
        snprintf(searchPath, sizeof(searchPath), "%s\\*.ini", destDir.c_str());
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(searchPath, &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string p = destDir + "\\" + fd.cFileName;
                DeleteFileA(p.c_str());
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }

    int copied = CopyFlatDirectory(srcStr, destDir);
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
    std::string prevCurrentName = (prevCurrent >= 0 && prevCurrent < (int)presets_.size())
        ? presets_[prevCurrent].name : "";

    ScanPresets();

    // Restore selection
    if (!prevCurrentName.empty())
    {
        for (int i = 0; i < (int)presets_.size(); i++)
            if (presets_[i].name == prevCurrentName) { currentPreset_ = i; break; }
    }

    // Find new index and validate
    int newIdx = -1;
    for (int i = 0; i < (int)presets_.size(); i++)
        if (presets_[i].name == targetName) { newIdx = i; break; }
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

    int copied = CopyFlatDirectory(preset.path, destDir);
    if (copied <= 0)
    {
        setErr("Failed to copy any files");
        return false;
    }

    Log("Exported preset '%s' (%d file%s) to %s",
        preset.name.c_str(), copied, copied == 1 ? "" : "s", destDir.c_str());
    return true;
}
