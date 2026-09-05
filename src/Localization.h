#pragma once

#include <string>

namespace DustLoc
{
    void Init(const std::string& modDir, const std::string& requestedLanguage);
    const char* T(const char* key);
    const char* Language();
    bool IsChinese();
    // Every translated string of the active language concatenated (empty for English). The GUI feeds
    // it to ImFontGlyphRangesBuilder so the font atlas contains exactly the glyphs the menu will draw
    // (a fixed per-script range table misses e.g. U+2248 '≈', en/em dashes and curly quotes).
    const std::string& GlyphText();
}
