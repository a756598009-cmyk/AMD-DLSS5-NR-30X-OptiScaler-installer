// Built with /utf-8, the language names below are UTF-8.
#include <windows.h>

#include "MenuLocale.h"
#include "MenuLocaleTables.h"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <ankerl/unordered_dense.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace MenuLocale
{
namespace
{
enum class Script
{
    Latin, // and Cyrillic, which the bundled Hack and Segoe UI both have
    SimplifiedChinese,
    TraditionalChinese,
    Japanese,
    Korean,
    Thai,
};

struct LanguageRow
{
    LanguageInfo info;
    Script script;
    // A glyph of these missing from the fonts means the language can't be shown.
    const char* sample;
};

// Arabic and Hindi stay out: ImGui has no bidi or shaping, the text would come out wrong.
const LanguageRow kRows[] = {
    { { "en", "English", "English" }, Script::Latin, "" },
    { { "pt-BR", "Portuguese (Brazil)", "Português (Brasil)" }, Script::Latin, "çãõ" },
    { { "es", "Spanish", "Español" }, Script::Latin, "ñóá" },
    { { "ru", "Russian", "Русский" }, Script::Latin, "ЖЯю" },
    { { "zh-CN", "Chinese (Simplified)", "简体中文" }, Script::SimplifiedChinese, "简体设置" },
    { { "de", "German", "Deutsch" }, Script::Latin, "äöüß" },
    { { "fr", "French", "Français" }, Script::Latin, "çéœ" },
    { { "it", "Italian", "Italiano" }, Script::Latin, "àèù" },
    { { "pl", "Polish", "Polski" }, Script::Latin, "ąłż" },
    { { "tr", "Turkish", "Türkçe" }, Script::Latin, "ğışİ" },
    { { "ja", "Japanese", "日本語" }, Script::Japanese, "あア語" },
    { { "ko", "Korean", "한국어" }, Script::Korean, "한글어" },
    { { "uk", "Ukrainian", "Українська" }, Script::Latin, "їєґ" },
    { { "zh-TW", "Chinese (Traditional)", "繁體中文" }, Script::TraditionalChinese, "繁體設定" },
    { { "cs", "Czech", "Čeština" }, Script::Latin, "čřů" },
    { { "nl", "Dutch", "Nederlands" }, Script::Latin, "éë" },
    { { "hu", "Hungarian", "Magyar" }, Script::Latin, "őű" },
    { { "ro", "Romanian", "Română" }, Script::Latin, "ășț" },
    { { "hr", "Croatian", "Hrvatski" }, Script::Latin, "čćđ" },
    { { "lt", "Lithuanian", "Lietuvių" }, Script::Latin, "ąėų" },
    { { "vi", "Vietnamese", "Tiếng Việt" }, Script::Latin, "ệơư" },
    { { "id", "Indonesian", "Bahasa Indonesia" }, Script::Latin, "" },
    { { "th", "Thai", "ไทย" }, Script::Thai, "ไทยก" },
};

struct FontFile
{
    const char* name;
    int face;
};

// The first of these that exists on the machine gets loaded.
std::vector<FontFile> ScriptFonts(Script script)
{
    switch (script)
    {
    case Script::SimplifiedChinese:
        return { { "msyh.ttc", 0 }, { "simsun.ttc", 0 }, { "simhei.ttf", 0 } };
    case Script::TraditionalChinese:
        return { { "msjh.ttc", 0 }, { "mingliu.ttc", 0 } };
    case Script::Japanese:
        return { { "YuGothR.ttc", 0 }, { "meiryo.ttc", 0 }, { "msgothic.ttc", 0 } };
    case Script::Korean:
        return { { "malgun.ttf", 0 }, { "gulim.ttc", 0 } };
    case Script::Thai:
        return { { "LeelawUI.ttf", 0 }, { "leelawad.ttf", 0 } };
    default:
        return {};
    }
}

// Conversions as vsnprintf would read them; %% is not one. With spaced, "% d" counts too, which in a translation
// would read an argument that is not there, and "% s" would crash.
template <typename Out>
void ScanFormat(std::string_view text, bool spaced, Out&& out, size_t* escaped = nullptr, size_t* lone = nullptr)
{
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] != '%')
            continue;

        size_t j = i + 1;
        if (j < text.size() && text[j] == '%')
        {
            if (escaped != nullptr)
                ++*escaped;
            i = j;
            continue;
        }

        while (j < text.size() && (strchr("-+#0", text[j]) != nullptr || (spaced && text[j] == ' ')))
            j++;
        while (j < text.size() && text[j] >= '0' && text[j] <= '9')
            j++;
        if (j < text.size() && text[j] == '.')
        {
            j++;
            while (j < text.size() && text[j] >= '0' && text[j] <= '9')
                j++;
        }
        while (j < text.size() && strchr("hlzjtLI", text[j]) != nullptr)
            j++;
        if (j < text.size() && strchr("diouxXeEfFgGaAcsSpn", text[j]) != nullptr)
        {
            out(text.substr(i, j - i + 1));
            i = j;
        }
        else if (lone != nullptr)
        {
            ++*lone;
        }
    }
}

size_t CountBraces(std::string_view text)
{
    size_t n = 0;
    for (size_t at = text.find("{}"); at != text.npos; at = text.find("{}", at + 2))
        n++;
    return n;
}

struct Item
{
    const char* text;
    const char* label;
};

struct Table
{
    ankerl::unordered_dense::map<std::string_view, Item> items;
    std::deque<std::string> labels;
    std::string code;
};

std::atomic<const Table*> active { nullptr };
std::mutex requestMutex;
std::string requested = "auto";
std::atomic<bool> pending { false };
std::atomic<bool> fontMissing { false };
std::vector<std::string> loadedFonts;

const Table* BuildTable(const char* code)
{
    auto table = new Table();
    table->code = code;
    table->items.reserve(Tables::KeyCount());

    const auto add = [table](const char* key, const char* text)
    {
        std::string& label = table->labels.emplace_back(text);
        label += "###";
        label += key;
        table->items.insert_or_assign(std::string_view(key), Item { text, label.c_str() });
    };

    // English goes in too, so that a widget's ID doesn't depend on the language having a translation for it.
    for (size_t i = 0; i < Tables::KeyCount(); i++)
        add(Tables::Key(i), Tables::Key(i));

    size_t count = 0;
    if (const Tables::Entry* entries = Tables::Find(code, count))
    {
        for (size_t i = 0; i < count; i++)
        {
            if (table->items.contains(entries[i].key) && TranslationIsSafe(entries[i].key, entries[i].text))
                add(entries[i].key, entries[i].text);
        }
    }

    return table;
}

std::string FontsFolder()
{
    wchar_t wide[MAX_PATH] {};
    const UINT length = GetWindowsDirectoryW(wide, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return "C:\\Windows\\Fonts\\";

    char narrow[MAX_PATH * 3] {};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof(narrow), nullptr, nullptr);
    return std::string(narrow) + "\\Fonts\\";
}

bool FontFileExists(const std::string& path)
{
    wchar_t wide[MAX_PATH * 2] {};
    if (MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide, MAX_PATH * 2) == 0)
        return false;

    return GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
}

bool FontCovers(ImFont* font, const char* sample)
{
    for (const char* at = sample; *at != 0;)
    {
        unsigned int c = 0;
        const int bytes = ImTextCharFromUtf8(&c, at, nullptr);
        if (bytes <= 0 || !font->IsGlyphInFont((ImWchar) c))
            return false;

        at += bytes;
    }

    return true;
}

bool LoadFirstOf(ImFontAtlas* atlas, ImFont* font, const std::vector<FontFile>& files)
{
    const std::string folder = FontsFolder();
    const float size = font->Sources.Size > 0 ? font->Sources[0]->SizePixels : 14.0f;

    for (const FontFile& file : files)
    {
        const std::string path = folder + file.name;

        for (const std::string& done : loadedFonts)
        {
            if (done == path)
                return true;
        }

        if (!FontFileExists(path))
            continue;

        // A baked size holds loader data per source of the font: one made before this source would overrun.
        ImFontAtlasFontDiscardBakes(atlas, font, 0);
        font->LastBaked = nullptr;

        ImFontConfig config;
        config.MergeMode = true;
        config.DstFont = font;
        config.FontNo = file.face;
        config.Flags |= ImFontFlags_NoLoadError;
        if (atlas->AddFontFromFileTTF(path.c_str(), size, &config) != nullptr)
        {
            loadedFonts.push_back(path);
            return true;
        }
    }

    return false;
}

// Merges what the language needs into the menu's font. The atlas is dynamic: of a font this size only the glyphs the
// text draws get rasterized.
bool EnsureFonts(ImFontAtlas* atlas, const LanguageRow& row)
{
    if (atlas->Fonts.empty())
        atlas->AddFontDefault();

    ImFont* font = atlas->Fonts[0];

    LoadFirstOf(atlas, font, ScriptFonts(row.script));

    // For what Hack lacks. It's what goes missing outside of Windows, and then the sample tells.
    LoadFirstOf(atlas, font, { { "segoeui.ttf", 0 }, { "tahoma.ttf", 0 } });

    return FontCovers(font, row.sample);
}

const LanguageRow* FindRow(std::string_view code)
{
    for (const LanguageRow& row : kRows)
    {
        if (code == row.info.code && (code == "en" || Tables::Has(row.info.code)))
            return &row;
    }

    return nullptr;
}

bool EqualsNoCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
}
} // namespace

bool TranslationIsSafe(const char* english, const char* translation)
{
    std::vector<std::string_view> a, b, c;
    size_t escapedA = 0, escapedB = 0, loneA = 0, loneB = 0;
    ScanFormat(english, false, [&](std::string_view v) { a.push_back(v); }, &escapedA, &loneA);
    ScanFormat(translation, false, [&](std::string_view v) { b.push_back(v); }, &escapedB, &loneB);
    ScanFormat(translation, true, [&](std::string_view v) { c.push_back(v); });

    // A percent sign that starts no conversion is only fine where the English has one too: that text is drawn as
    // it is. In a format it would be an invalid conversion.
    return a == b && b == c && escapedA == escapedB && (loneB == 0 || loneA > 0) &&
           CountBraces(english) == CountBraces(translation) && strstr(translation, "##") == nullptr;
}

int LanguageCount()
{
    int count = 0;
    for (const LanguageRow& row : kRows)
        count += FindRow(row.info.code) != nullptr ? 1 : 0;
    return count;
}

const LanguageInfo& Language(int index)
{
    for (const LanguageRow& row : kRows)
    {
        if (FindRow(row.info.code) != nullptr && index-- == 0)
            return row.info;
    }

    return kRows[0].info;
}

const char* MatchLanguage(std::string_view locale)
{
    for (const LanguageRow& row : kRows)
    {
        if (EqualsNoCase(locale, row.info.code) && FindRow(row.info.code) != nullptr)
            return row.info.code;
    }

    const std::string_view primary = locale.substr(0, locale.find('-'));

    if (EqualsNoCase(primary, "zh"))
    {
        const std::string_view rest = locale.size() > 3 ? locale.substr(3) : std::string_view();
        const bool traditional = rest.starts_with("Hant") || rest.starts_with("hant") || EqualsNoCase(rest, "TW") ||
                                 EqualsNoCase(rest, "HK") || EqualsNoCase(rest, "MO");
        const char* code = traditional ? "zh-TW" : "zh-CN";
        return FindRow(code) != nullptr ? code : "en";
    }

    for (const LanguageRow& row : kRows)
    {
        const std::string_view code = row.info.code;
        if (EqualsNoCase(primary, code.substr(0, code.find('-'))) && FindRow(code) != nullptr)
            return row.info.code;
    }

    return "en";
}

const char* DetectSystemLanguage()
{
    wchar_t wide[LOCALE_NAME_MAX_LENGTH] {};
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), wide, LOCALE_NAME_MAX_LENGTH, 0) == 0)
        return "en";

    char narrow[LOCALE_NAME_MAX_LENGTH * 3] {};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof(narrow), nullptr, nullptr);
    return MatchLanguage(narrow);
}

void Request(const char* setting)
{
    std::scoped_lock lock(requestMutex);
    requested = (setting == nullptr || setting[0] == 0) ? "auto" : setting;
    pending = true;
}

void ApplyPending(ImFontAtlas* atlas)
{
    if (!pending.exchange(false))
        return;

    std::string setting;
    {
        std::scoped_lock lock(requestMutex);
        setting = requested;
    }

    const char* code = EqualsNoCase(setting, "auto") ? DetectSystemLanguage() : MatchLanguage(setting);

    bool fontOk = true;
    if (strcmp(code, "en") != 0)
    {
        fontOk = EnsureFonts(atlas, *FindRow(code));
        if (!fontOk)
            code = "en";
    }
    fontMissing = !fontOk;

    const Table* current = active.load();
    if (current == nullptr || current->code != code)
        active = BuildTable(code); // Other threads may be reading the old one, it stays.
}

const char* ActiveCode()
{
    const Table* table = active.load();
    return table != nullptr ? table->code.c_str() : "en";
}

bool FontMissing() { return fontMissing; }

const char* TextOrNull(std::string_view english)
{
    const Table* table = active.load(std::memory_order_acquire);
    if (table == nullptr)
        return nullptr;

    const auto item = table->items.find(english);
    return item != table->items.end() ? item->second.text : nullptr;
}

const char* Text(const char* english)
{
    std::string_view view(english);
    if (const size_t id = view.find("##"); id != view.npos)
        view = view.substr(0, id);

    const char* text = view.empty() ? nullptr : TextOrNull(view);
    return text != nullptr ? text : english;
}

std::string Printf(const char* fmt, ...)
{
    fmt = Format(fmt);
    va_list args;
    va_start(args, fmt);
    const int length = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);
    if (length <= 0)
        return {};

    std::string out((size_t) length, 0);
    va_start(args, fmt);
    vsnprintf(out.data(), (size_t) length + 1, fmt, args);
    va_end(args);
    return out;
}

// A format keeps its own ##suffix: Label's Tr###english##suffix would repeat the conversions, and "FSR %s##%d" then
// read the %d argument as a string (DX11 games crashed in the FFX Upscaler list).
const char* Format(const char* english)
{
    const char* id = strstr(english, "##");
    if (id == nullptr || id[2] == '#')
        return id == nullptr ? Text(english) : Label(english);

    const char* text = TextOrNull(std::string_view(english, id - english));
    if (text == nullptr)
        return english;

    static thread_local std::string ring[16];
    static thread_local unsigned next = 0;
    std::string& out = ring[next++ % std::size(ring)];
    out = text;
    out += id;
    return out.c_str();
}

const char* Label(const char* english)
{
    const Table* table = active.load(std::memory_order_acquire);
    if (table == nullptr)
        return english;

    const std::string_view label(english);
    const size_t id = label.find("##");

    if (id == 0)
        return english;

    const auto item = table->items.find(label.substr(0, id));
    if (item == table->items.end())
        return english;

    if (id == label.npos)
        return item->second.label;

    // Foo##bar becomes Tr###Foo##bar and Foo###bar becomes Tr###bar: ImGui hashes from the last ###.
    static thread_local std::string ring[16];
    static thread_local unsigned next = 0;
    std::string& out = ring[next++ % std::size(ring)];

    const bool ownId = label.substr(id).starts_with("###");
    out = item->second.text;
    if (!ownId)
        out += "###";
    out += label.substr(ownId ? id : 0);

    return out.c_str();
}
} // namespace MenuLocale
