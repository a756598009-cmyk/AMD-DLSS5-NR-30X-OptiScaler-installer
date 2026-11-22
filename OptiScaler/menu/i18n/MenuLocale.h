#pragma once

#include <string>
#include <string_view>

struct ImFontAtlas;

// The menu's translations. The English text is the key, so a string missing from a table, or new since the last
// translation round, just shows in English.
namespace MenuLocale
{
struct LanguageInfo
{
    const char* code;    // what [Menu] Language takes
    const char* english; // name in English, always drawable
    const char* native;  // name in its own script, drawable once its font is in the atlas
};

int LanguageCount();
const LanguageInfo& Language(int index);

// "auto" or empty follows Windows. Takes effect on the next ApplyPending.
void Request(const char* setting);

// Between frames, before NewFrame: adds the fonts the requested language needs and switches to it. Without a font
// that can draw it the language stays English. Does nothing when no request is waiting.
void ApplyPending(ImFontAtlas* atlas);

const char* ActiveCode();

// Whether the last request ended in English for lack of a font.
bool FontMissing();

// Language code Windows' UI language maps to, "en" when there is no table for it.
const char* DetectSystemLanguage();
const char* MatchLanguage(std::string_view locale);

// The translation for the display, the text without any ##id. Unknown text comes back as it is.
const char* Text(const char* english);

// For a printf format that may build a widget label: a ##id in it stays out of the translation.
const char* Format(const char* english);

// printf with the translation of fmt, for text the code builds.
std::string Printf(const char* fmt, ...);

// As Text for a slice; nullptr when there is no translation.
const char* TextOrNull(std::string_view english);

// For a widget: the translated display text with an ID that is the same in every language.
// The pointer is good until 16 more calls of Label with a ##id in them.
const char* Label(const char* english);
} // namespace MenuLocale
