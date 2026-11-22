// Parsing and matching of shortcut bindings (OptiScaler/menu/input/shortcut_keys.h): plain keys as older INIs
// hold them, chords with Ctrl, Shift, Alt and Tab, the readable INI form, and which held modifiers fire which key.
//   cl /nologo /std:c++20 /EHsc /W4 tests\shortcut_keys_test.cpp && shortcut_keys_test.exe
#include "../OptiScaler/menu/input/shortcut_keys.h"

#include <cstdio>

static int failures = 0;

static void Expect(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

static bool Parses(const char* text, int expected)
{
    auto value = ShortcutKeys::Parse(text);
    return value.has_value() && *value == expected;
}

int main()
{
    using namespace ShortcutKeys;

    // Old INI values keep their meaning.
    Expect(Parses("0x2D", VK_INSERT), "hex key code");
    Expect(Parses("45", VK_INSERT), "decimal key code");
    Expect(Parses("-1", -1), "unbound");
    Expect(Parses("0x30024", VK_HOME | Ctrl | Shift), "number with modifier bits");

    // Readable form.
    Expect(Parses("Ctrl+Shift+Home", VK_HOME | Ctrl | Shift), "Ctrl+Shift+Home");
    Expect(Parses(" alt + f10 ", VK_F10 | Alt), "case and spaces");
    Expect(Parses("Tab+Q", 'Q' | Tab), "Tab as modifier");
    Expect(Parses("Tab", VK_TAB), "Tab alone");
    Expect(Parses("Control+0x2D", VK_INSERT | Ctrl), "modifier with hex key");
    Expect(Parses("Numpad5", VK_NUMPAD5), "Numpad5");
    Expect(Parses("PrintScreen", VK_SNAPSHOT), "PrintScreen");
    Expect(Parses("PageUp", VK_PRIOR), "PageUp");
    Expect(Parses("F24", VK_F24), "F24");
    Expect(Parses("8", VK_BACK), "a lone digit is a key code, as older INIs wrote it");
    Expect(Parses("Ctrl+7", '7' | Ctrl), "a digit after a modifier is the digit key");
    Expect(Parses("q", 'Q'), "a letter");
    Expect(!Parse("Ctrl+").has_value(), "modifier without key");
    Expect(!Parse("Hyper+A").has_value(), "unknown modifier");
    Expect(!Parse("Ctrl+-1").has_value(), "modifier on unbound");
    Expect(!Parse("0x1000000").has_value(), "bits outside the format");
    Expect(!Parse("F25").has_value(), "F25");
    Expect(!Parse("auto").has_value(), "auto is not a key");

    // Matching.
    const int home = VK_HOME, ctrlHome = VK_HOME | Ctrl;
    Expect(Matches(home, VK_HOME, 0), "plain key, nothing held");
    Expect(!Matches(home, VK_HOME, Ctrl), "plain key does not fire with Ctrl held");
    Expect(Matches(ctrlHome, VK_HOME, Ctrl), "chord with its modifier");
    Expect(!Matches(ctrlHome, VK_HOME, 0), "chord without its modifier");
    Expect(!Matches(ctrlHome, VK_HOME, Ctrl | Shift), "chord with an extra modifier");
    Expect(!Matches(ctrlHome, VK_END, Ctrl), "other key");
    Expect(Matches(VK_TAB, VK_TAB, Tab), "Tab bound alone counts its own press");
    Expect(Matches(VK_LCONTROL, VK_LCONTROL, Ctrl), "left Ctrl bound alone");
    Expect(!Matches(-1, VK_HOME, 0) && !Matches(0, 0, 0), "unbound never fires");
    Expect(Key(ctrlHome) == VK_HOME && Modifiers(ctrlHome) == Ctrl, "key and modifiers split");
    Expect(ModifierText(Ctrl | Shift | Alt | Tab) == "Ctrl+Shift+Alt+Tab+", "modifier text");

    std::printf("%d failed\n", failures);
    return failures;
}
