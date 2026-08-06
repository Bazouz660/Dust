#pragma once

#include <string>

namespace DustLoc
{
    void Init(const std::string& modDir, const std::string& requestedLanguage);
    const char* T(const char* key);
    const char* Language();
    bool IsChinese();
}
