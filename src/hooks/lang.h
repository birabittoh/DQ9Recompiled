// Text the hooks add to the game, from res/lang/*.toml (embedded by
// scripts/gen-lang.py into lang.generated.h).
#pragma once

#include <cstdint>
#include <cstring>

#include "lang.generated.h"

namespace dq9 {

// The language DQ9 last loaded text in (the game context, 0x0200F398, +5;
// DS firmware codes, 1 English .. 5 Spanish). See language.cpp.
constexpr uint32_t kGameTextLanguage = 0x020F33D8u + 5u;

// The language the hooks' text is shown in: the game's, or 6 (Japanese,
// Shift JIS text) once the Japanese fonts are loaded (ja_font.cpp).
uint32_t TextLanguage();

// The title screen is still loading its menu text in a newly chosen
// language (language.cpp): the main menu would show the old one.
bool TitleTextPending();

// The text for [section] key in DS language `language`, English when that
// language lacks it or is out of range, "" when English lacks it too.
inline const char* Tr(const char* section, const char* key, uint32_t language) {
    for (const LangText& t : kLangTexts) {
        if (std::strcmp(t.section, section) != 0 || std::strcmp(t.key, key) != 0)
            continue;
        if (language >= 1u && language <= 6u && t.text[language - 1u])
            return t.text[language - 1u];
        return t.text[0] ? t.text[0] : "";
    }
    return "";
}

}  // namespace dq9
