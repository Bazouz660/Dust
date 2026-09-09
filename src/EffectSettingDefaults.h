#pragma once
#include "DustAPI.h"
#include <array>
#include <cstring>
#include <vector>

// Capture before loading the active INI, while the DLL still holds defaults.
class EffectSettingDefaults {
    std::vector<std::array<unsigned char, 12>> values_;
    static size_t Size(DustSettingType type) {
        switch (type) {
        case DUST_SETTING_BOOL: case DUST_SETTING_HIDDEN_BOOL: return sizeof(bool);
        case DUST_SETTING_FLOAT: case DUST_SETTING_HIDDEN_FLOAT: return sizeof(float);
        case DUST_SETTING_INT: case DUST_SETTING_HIDDEN_INT: case DUST_SETTING_ENUM: return sizeof(int);
        case DUST_SETTING_COLOR3: return 3 * sizeof(float);
        default: return 0;
        }
    }
public:
    void Capture(const DustEffectDesc& desc) {
        values_.resize(desc.settingCount);
        for (uint32_t i = 0; desc.settings && i < desc.settingCount; ++i)
            if (desc.settings[i].valuePtr)
                std::memcpy(values_[i].data(), desc.settings[i].valuePtr, Size(desc.settings[i].type));
    }
    void RestoreMissing(const DustSettingDesc& setting, uint32_t index) const {
        if ((setting.settingFlags & DUST_SETTING_FLAG_PRESET_DEFAULT) &&
            !(setting.settingFlags & DUST_SETTING_FLAG_PRESET_OPTIONAL) &&
            setting.valuePtr && index < values_.size())
            std::memcpy(setting.valuePtr, values_[index].data(), Size(setting.type));
    }
};
