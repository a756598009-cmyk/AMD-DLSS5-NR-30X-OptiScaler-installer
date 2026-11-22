#pragma once

// A shortcut binding: a virtual key in the low 16 bits and the modifiers that must be held with it above them.
// A plain virtual key, as older INIs hold, is the key with no modifier. -1 or 0 is unbound.
// No Windows or OptiScaler dependency beyond the VK_ constants, so tests can include it alone.

#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace ShortcutKeys
{
constexpr int Ctrl = 1 << 16;
constexpr int Shift = 1 << 17;
constexpr int Alt = 1 << 18;
constexpr int Tab = 1 << 19;
constexpr int ModifierMask = Ctrl | Shift | Alt | Tab;

inline int Key(int binding) { return binding > 0 ? binding & 0xFFFF : 0; }
inline int Modifiers(int binding) { return binding > 0 ? binding & ModifierMask : 0; }
inline bool Bound(int binding) { return Key(binding) > 0 && Key(binding) < 256; }

// The modifier a key is, or 0.
inline int ModifierOf(int vk)
{
    switch (vk)
    {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return Ctrl;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return Shift;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return Alt;
    case VK_TAB:
        return Tab;
    default:
        return 0;
    }
}

// Fires only when exactly the binding's modifiers are held. A binding whose own key is a modifier
// (Tab alone, say) does not count that modifier.
inline bool Matches(int binding, int vk, int heldModifiers)
{
    if (!Bound(binding) || Key(binding) != vk)
        return false;

    const int own = ModifierOf(vk);
    return (heldModifiers & ModifierMask & ~own) == (Modifiers(binding) & ~own);
}

inline std::string ModifierText(int modifiers)
{
    std::string text;
    if (modifiers & Ctrl)
        text += "Ctrl+";
    if (modifiers & Shift)
        text += "Shift+";
    if (modifiers & Alt)
        text += "Alt+";
    if (modifiers & Tab)
        text += "Tab+";
    return text;
}

struct NamedKey
{
    const char* name;
    int vk;
};

// Names the INI accepts for a key, besides a letter, F1-F24, NumpadN and a number (a digit after a modifier).
inline constexpr NamedKey NamedKeys[] = {
    { "Insert", VK_INSERT },
    { "Ins", VK_INSERT },
    { "Delete", VK_DELETE },
    { "Del", VK_DELETE },
    { "Home", VK_HOME },
    { "End", VK_END },
    { "PageUp", VK_PRIOR },
    { "PgUp", VK_PRIOR },
    { "PageDown", VK_NEXT },
    { "PgDn", VK_NEXT },
    { "PrintScreen", VK_SNAPSHOT },
    { "PrtSc", VK_SNAPSHOT },
    { "Pause", VK_PAUSE },
    { "ScrollLock", VK_SCROLL },
    { "Space", VK_SPACE },
    { "Enter", VK_RETURN },
    { "Backspace", VK_BACK },
    { "Escape", VK_ESCAPE },
    { "Esc", VK_ESCAPE },
    { "Tab", VK_TAB },
    { "Up", VK_UP },
    { "Down", VK_DOWN },
    { "Left", VK_LEFT },
    { "Right", VK_RIGHT },
    { "NumpadAdd", VK_ADD },
    { "NumpadSubtract", VK_SUBTRACT },
    { "NumpadMultiply", VK_MULTIPLY },
    { "NumpadDivide", VK_DIVIDE },
    { "NumpadDecimal", VK_DECIMAL },
    { "Ctrl", VK_CONTROL },
    { "Shift", VK_SHIFT },
    { "Alt", VK_MENU },
};

inline bool SameText(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;

    return true;
}

inline std::optional<int> ParseKey(std::string_view token)
{
    if (token.empty())
        return std::nullopt;

    // A lone digit is a key code, as older INIs wrote them (8 is Backspace); Parse makes it the digit key
    // after a modifier.
    if (token.size() == 1 && std::isalpha(static_cast<unsigned char>(token[0])))
        return std::toupper(static_cast<unsigned char>(token[0]));

    for (const auto& named : NamedKeys)
        if (SameText(token, named.name))
            return named.vk;

    auto number = [](std::string_view digits, int base) -> std::optional<int>
    {
        if (digits.empty())
            return std::nullopt;
        std::string text(digits);
        char* end = nullptr;
        const long value = std::strtol(text.c_str(), &end, base);
        if (end == nullptr || *end != '\0')
            return std::nullopt;
        return static_cast<int>(value);
    };

    if ((token[0] == 'F' || token[0] == 'f') && token.size() <= 3)
        if (auto n = number(token.substr(1), 10); n && *n >= 1 && *n <= 24)
            return VK_F1 + *n - 1;

    if (token.size() == 7 && SameText(token.substr(0, 6), "Numpad") &&
        std::isdigit(static_cast<unsigned char>(token[6])))
        return VK_NUMPAD0 + (token[6] - '0');

    if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
        return number(token.substr(2), 16);

    return number(token, 10);
}

// "Ctrl+Shift+Home", "Alt+0x24", "0x30024", "36" or "-1". Spaces around the parts are allowed.
inline std::optional<int> Parse(std::string_view text)
{
    auto trim = [](std::string_view s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
            s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.remove_suffix(1);
        return s;
    };

    text = trim(text);
    if (text.empty())
        return std::nullopt;

    int modifiers = 0;
    for (;;)
    {
        const auto plus = text.find('+', 1);
        if (plus == std::string_view::npos)
            break;

        const auto part = trim(text.substr(0, plus));
        if (SameText(part, "Ctrl") || SameText(part, "Control"))
            modifiers |= Ctrl;
        else if (SameText(part, "Shift"))
            modifiers |= Shift;
        else if (SameText(part, "Alt"))
            modifiers |= Alt;
        else if (SameText(part, "Tab"))
            modifiers |= Tab;
        else
            return std::nullopt;

        text = trim(text.substr(plus + 1));
    }

    auto key = modifiers != 0 && text.size() == 1 && std::isdigit(static_cast<unsigned char>(text[0]))
                   ? std::optional<int>(text[0])
                   : ParseKey(text);
    if (!key)
        return std::nullopt;

    // A number on its own may carry modifier bits already (the form OptiScaler writes).
    if (*key <= 0)
        return modifiers == 0 ? key : std::nullopt;

    if ((*key & ~(0xFFFF | ModifierMask)) != 0 || (*key & 0xFFFF) == 0 || (*key & 0xFFFF) >= 256)
        return std::nullopt;

    return *key | modifiers;
}
} // namespace ShortcutKeys
