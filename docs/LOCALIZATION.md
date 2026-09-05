# Dust Localization Support

## Overview

Dust includes a lightweight localization layer for the in-game F11 menu. It
translates menu labels, option names, tooltips, effect names, and effect
descriptions without changing Dust's rendering or configuration logic.

The localization layer is independent from the effect system and only changes
the text returned to the user interface.

## How It Works

UI text is requested through the `DustLoc` function in `Localization.h`:

```cpp
ImGui::Text("%s", DustLoc("Performance Impact").c_str());
```

At runtime, the localization system detects the language selected in Kenshi
(the `language=` line of `settings.cfg` in the game root, which is found from
the host executable so that both `<game>/mods/Dust` and Steam Workshop
installs work), loads the matching file from `mod/lang/`, and looks up the
original English text as the translation key. `Language=auto` in `Dust.ini`
is the default; set it to a code such as `fr_FR` to force a language. The
Dust log reports which language was detected, from which file, and how many
strings were loaded.

The GUI picks a system font per script (Segoe UI for Latin and Cyrillic,
Microsoft YaHei / SimSun for Chinese, Yu Gothic / Meiryo / MS Gothic for
Japanese, Malgun Gothic for Korean) and builds the glyph atlas from the
script's base ranges plus every character the loaded translation uses. When a translation exists, it is returned;
otherwise the original English text is returned.

This fallback behavior ensures that an incomplete translation cannot make a
menu item blank or prevent the menu from being displayed.

## File Layout

```text
mod/
  Dust.ini
  lang/
    de_DE.ini
    es_ES.ini
    fr_FR.ini
    ja_JP.ini
    ko_KR.ini
    pt_BR.ini
    ru_RU.ini
    zh_CN.ini
src/
  Localization.cpp
  Localization.h
```

`Localization.cpp` implements language detection, file loading, key lookup,
and fallback handling. `Localization.h` exposes the interface used by the UI.
One file exists per language Kenshi ships (every folder under the game's
`locale/` directory except `en_GB`, which is the key language). All files
carry the same key set; `zh_CN.ini` is the reference when adding new keys.

Keys may start with `[` (for example `[ON]` or `[!] Preset is outdated`).
The loader only treats a `[...]` line as a section header when it contains no
`=`, so such keys translate normally.

## Adding Another Language

Create a file in `mod/lang/` using the language code reported by Kenshi, for
example `mod/lang/ja_JP.ini`. Copy the English UI keys and translate only the
values. No C++ changes are required for a new language file.

If Kenshi selects a language that does not have a matching file, Dust displays
the original English UI text.

## Translation Rules

Translation files use UTF-8 `key=value` pairs:

```ini
Performance Impact=Translated text
Save=Translated text
Depth of Field=Translated text
```

Keys must remain exactly equal to the original English UI text. Preserve
meaningful spaces and formatting placeholders such as `%s`, `%d`, `%.2f`, and
`%.1f%%`.

Some ImGui labels contain a hidden ID suffix using `###`, for example
`Save###save_button`. The visible text may be translated, but the hidden ID
must remain unchanged so widget state and existing UI behavior continue to
work correctly.

## Compatibility and Scope

The localization changes do not alter rendering effects, shader behavior,
effect parameters, configuration values, hotkeys, input handling, save data,
game data, or the Dust-to-Kenshi interface.

The change is limited to text displayed by Dust's F11 user interface. If a
translation file or entry is missing, the original English text remains
available as the fallback.
