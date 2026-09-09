#include "../src/EffectSettingDefaults.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

struct LoadedEffect { DustEffectDesc desc = {}; EffectSettingDefaults settingDefaults; };
class EffectLoader {
public:
    static void EffectConfigLoadFrom(LoadedEffect&, const std::string&);
    static void EffectConfigSaveTo(LoadedEffect&, const std::string&);
};
#include "build/EffectPresetIO.generated.h"

int main() {
    const auto root = std::filesystem::absolute(std::filesystem::path("build") / ("preset-probe-" + std::to_string(GetCurrentProcessId())));
    const auto modern = root / "modern", old = root / "old", missing = root / "missing";
    std::filesystem::create_directories(modern); std::filesystem::create_directories(old);
    std::filesystem::create_directories(missing);
    bool enabled = true, depth = false;
    int stage = 0, order = 80, playerSetting = 4096;
    float strength = 1, luminanceProtect = 0;
    const char* const stages[] = {"HDR", "LDR", nullptr};
    DustSettingDesc settings[] = {
        {"Enabled", DUST_SETTING_BOOL, &enabled, 0, 1, "Enabled"},
        {"Strength", DUST_SETTING_FLOAT, &strength, 0, 1, "Strength"},
        {"Depth", DUST_SETTING_BOOL, &depth, 0, 1, "Depth", nullptr, nullptr, 0, DUST_SETTING_FLAG_PRESET_DEFAULT},
        {"Luminance", DUST_SETTING_FLOAT, &luminanceProtect, 0, 1, "Luminance", nullptr, nullptr, 0, DUST_SETTING_FLAG_PRESET_DEFAULT},
        {"Stage", DUST_SETTING_ENUM, &stage, 0, 1, "Stage", stages, nullptr, 0, DUST_SETTING_FLAG_POST_STAGE | DUST_SETTING_FLAG_PRESET_DEFAULT},
        {"Order", DUST_SETTING_HIDDEN_INT, &order, -10000, 10000, "Order", nullptr, nullptr, 0, DUST_SETTING_FLAG_POST_ORDER_LDR | DUST_SETTING_FLAG_PRESET_DEFAULT},
        {"Player", DUST_SETTING_INT, &playerSetting, 512, 16384, "Player", nullptr, nullptr, 0, DUST_SETTING_FLAG_PRESET_OPTIONAL}
    };
    LoadedEffect le; le.desc.name = "Fixture"; le.desc.settings = settings; le.desc.settingCount = 7;
    le.settingDefaults.Capture(le.desc);
    depth = true; stage = 1; order = 17; strength = .25f; luminanceProtect = .75f;
    EffectLoader::EffectConfigSaveTo(le, modern.string());
    const auto ini = (modern / "Fixture.ini").string();
    assert(GetPrivateProfileIntA("Fixture", "Order", -1, ini.c_str()) == 17);
    assert(GetPrivateProfileIntA("Fixture", "Player", -1, ini.c_str()) == -1);
    depth = false; stage = 0; order = 80; strength = 1; luminanceProtect = 0;
    EffectLoader::EffectConfigLoadFrom(le, modern.string());
    assert(depth && stage == 1 && order == 17 && strength == .25f && luminanceProtect == .75f);
    assert(WritePrivateProfileStringA("Fixture", "Strength", "0.6", (old / "Fixture.ini").string().c_str()));
    EffectLoader::EffectConfigLoadFrom(le, old.string());
    assert(!depth && stage == 0 && order == 80 && strength == .6f && luminanceProtect == 0);
    assert(playerSetting == 4096);
    EffectLoader::EffectConfigLoadFrom(le, modern.string());
    EffectLoader::EffectConfigLoadFrom(le, missing.string());
    assert(!enabled && !depth && stage == 0 && order == 80 && luminanceProtect == 0);
    std::puts("Production preset INI round-trip, legacy reset and player-owned setting preservation passed");
}
