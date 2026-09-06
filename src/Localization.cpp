#include "Localization.h"
#include "DustLog.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    std::unordered_map<std::string, std::string> gTranslations;
    std::unordered_map<std::string, std::string> gLabelCache;
    std::string gLanguage = "en_GB";
    std::string gGlyphText;
    bool gIsChinese = false;

    static std::string Trim(const std::string& s)
    {
        size_t start = 0;
        while (start < s.size() && std::isspace((unsigned char)s[start])) start++;
        size_t end = s.size();
        while (end > start && std::isspace((unsigned char)s[end - 1])) end--;
        return s.substr(start, end - start);
    }

    static std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return (char)std::tolower(c);
        });
        return s;
    }

    static bool FileExists(const std::string& path)
    {
        return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    static std::string JoinPath(const std::string& a, const std::string& b)
    {
        if (a.empty()) return b;
        char last = a[a.size() - 1];
        if (last == '\\' || last == '/') return a + b;
        return a + "\\" + b;
    }

    static std::string NormalizeLanguage(const std::string& raw)
    {
        std::string v = Lower(Trim(raw));
        std::replace(v.begin(), v.end(), '-', '_');

        if (v.empty() || v == "auto") return "auto";
        if (v == "english" || v == "en" || v == "en_us" || v == "en_gb") return "en_GB";
        if (v == "chinese" || v == "schinese" || v == "simp_chinese" ||
            v == "simplified_chinese" || v == "zh" || v == "zh_cn" || v == "zh_hans") return "zh_CN";
        if (v == "tchinese" || v == "trad_chinese" || v == "traditional_chinese" ||
            v == "zh_tw" || v == "zh_hant" || v == "zh_hk") return "zh_TW";
        if (v == "japanese" || v == "ja" || v == "ja_jp") return "ja_JP";
        if (v == "korean" || v == "ko" || v == "ko_kr") return "ko_KR";
        if (v == "russian" || v == "ru" || v == "ru_ru") return "ru_RU";
        if (v == "german" || v == "de" || v == "de_de") return "de_DE";
        if (v == "french" || v == "fr" || v == "fr_fr") return "fr_FR";
        if (v == "spanish" || v == "es" || v == "es_es") return "es_ES";
        if (v == "portuguese" || v == "brazilian" || v == "pt" || v == "pt_br") return "pt_BR";
        return raw;
    }
    static std::string ReadTextFile(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return "";
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    static std::string FindLanguageInText(const std::string& text)
    {
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line))
        {
            std::string trimmed = Trim(line);
            if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;

            size_t sep = trimmed.find_first_of("=:");
            if (sep == std::string::npos) continue;

            std::string key = Lower(Trim(trimmed.substr(0, sep)));
            if (key != "language" && key != "locale" && key != "translation" && key != "lang") continue;

            size_t start = trimmed.find_first_not_of(" \t\"'", sep + 1);
            if (start == std::string::npos) continue;

            size_t end = start;
            while (end < trimmed.size())
            {
                char c = trimmed[end];
                if (std::isspace((unsigned char)c) || c == '\"' || c == '\'' || c == ',' || c == ';') break;
                end++;
            }

            std::string lang = NormalizeLanguage(trimmed.substr(start, end - start));
            if (!lang.empty() && lang != "auto") return lang;
        }
        return "";
    }
    static void AddCandidate(std::vector<std::string>& out, const std::string& path)
    {
        if (!path.empty()) out.push_back(path);
    }

    static std::string DetectGameLanguage(const std::string& modDir)
    {
        std::vector<std::string> candidates;

        // The game root from the host exe FIRST. Deriving it from the mod dir (below) only works for
        // <game>/mods/Dust; a Steam Workshop install lives in steamapps/workshop/content/233860/<id>,
        // whose grandparent has no settings.cfg, and Kenshi keeps settings.cfg in the game root, not
        // under %LOCALAPPDATA% - so Workshop users silently got English (2026-09-05).
        {
            std::string gameRoot = DustGameDir();
            AddCandidate(candidates, JoinPath(gameRoot, "settings.cfg"));
            AddCandidate(candidates, JoinPath(gameRoot, "options.cfg"));
        }

        std::string cleanModDir = modDir;
        while (!cleanModDir.empty() && (cleanModDir.back() == '\\' || cleanModDir.back() == '/'))
            cleanModDir.pop_back();

        size_t modsPos = cleanModDir.find_last_of("\\/");
        if (modsPos != std::string::npos)
        {
            std::string modsDir = cleanModDir.substr(0, modsPos);
            size_t rootPos = modsDir.find_last_of("\\/");
            std::string kenshiRoot = (rootPos == std::string::npos) ? "" : modsDir.substr(0, rootPos);
            if (!kenshiRoot.empty())
            {
                AddCandidate(candidates, JoinPath(kenshiRoot, "settings.cfg"));
                AddCandidate(candidates, JoinPath(kenshiRoot, "options.cfg"));
            }
        }

        char appData[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", appData, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            std::string base = appData;
            AddCandidate(candidates, JoinPath(base, "Kenshi\\settings.cfg"));
            AddCandidate(candidates, JoinPath(base, "Kenshi\\options.cfg"));
        }

        for (const std::string& path : candidates)
        {
            if (!FileExists(path)) continue;
            std::string lang = FindLanguageInText(ReadTextFile(path));
            if (!lang.empty())
            {
                Log("Localization: detected Kenshi language '%s' from %s", lang.c_str(), path.c_str());
                return lang;
            }
        }
        // Say so, and where we looked: a silent fallback is indistinguishable from "working, English".
        std::string looked;
        for (const std::string& c : candidates) { if (!looked.empty()) looked += ", "; looked += c; }
        Log("Localization: no language setting found (looked in: %s) - defaulting to English", looked.c_str());
        return "en_GB";
    }

    static bool LoadIni(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;

        std::string line;
        while (std::getline(f, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string trimmed = Trim(line);
            if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;
            // A "[section]" header carries no '='. Keys such as "[ON]", "[OFF]" and
            // "[!] Preset is outdated" also start with '[' but are real key=value lines.
            if (trimmed[0] == '[' && trimmed.find('=') == std::string::npos) continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            if (!key.empty()) gTranslations[key] = value;
        }
        return true;
    }
}

namespace DustLoc
{
    std::vector<std::string> AvailableLanguages(const std::string& modDir)
    {
        std::vector<std::string> languages = { "auto", "en_GB" };
        WIN32_FIND_DATAA data = {};
        const std::string pattern = JoinPath(JoinPath(modDir, "lang"), "*.ini");
        HANDLE search = FindFirstFileA(pattern.c_str(), &data);
        if (search != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string filename = data.cFileName;
                const size_t dot = filename.find_last_of('.');
                if (dot == std::string::npos || dot == 0) continue;
                std::string locale = filename.substr(0, dot);
                if (std::find(languages.begin(), languages.end(), locale) == languages.end())
                    languages.push_back(locale);
            } while (FindNextFileA(search, &data));
            FindClose(search);
        }
        std::sort(languages.begin() + 2, languages.end());
        return languages;
    }

    void Init(const std::string& modDir, const std::string& requestedLanguage)
    {
        gTranslations.clear();
        gLabelCache.clear();

        std::string lang = NormalizeLanguage(requestedLanguage);
        if (lang.empty() || lang == "auto") lang = DetectGameLanguage(modDir);
        lang = NormalizeLanguage(lang);
        if (lang.empty() || lang == "auto") lang = "en_GB";

        std::string path = JoinPath(JoinPath(modDir, "lang"), lang + ".ini");
        if (lang != "en_GB" && !LoadIni(path))
        {
            Log("Localization: no translation file for '%s' at %s, falling back to English", lang.c_str(), path.c_str());
            lang = "en_GB";
        }
        else if (lang != "en_GB")
        {
            Log("Localization: loaded %zu strings for '%s' from %s", gTranslations.size(), lang.c_str(), path.c_str());
        }
        else
        {
            Log("Localization: language 'en_GB' (requested '%s'), no translation file needed", requestedLanguage.c_str());
        }

        gLanguage = lang;
        gIsChinese = (lang == "zh_CN" || lang == "zh_TW");

        // Concatenate every translated value once, for the GUI's glyph-range builder (see GlyphText).
        gGlyphText.clear();
        for (const auto& kv : gTranslations) { gGlyphText += kv.second; gGlyphText += ' '; }
    }

    const std::string& GlyphText()
    {
        return gGlyphText;
    }

    const char* T(const char* key)
    {
        if (!key || gTranslations.empty()) return key;

        const char* hiddenId = strstr(key, "##");

        auto it = gTranslations.find(key);
        if (it == gTranslations.end() && hiddenId)
        {
            std::string visibleKey(key, hiddenId - key);
            it = gTranslations.find(visibleKey);
        }
        if (it == gTranslations.end()) return key;

        if (!hiddenId || strstr(it->second.c_str(), "##"))
            return it->second.c_str();

        std::string& cached = gLabelCache[key];
        cached = it->second;
        cached += hiddenId;
        return cached.c_str();
    }
    const char* Language()
    {
        return gLanguage.c_str();
    }

    bool IsChinese()
    {
        return gIsChinese;
    }
}
