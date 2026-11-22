#!/usr/bin/env python3
r"""The menu's translation tables.

    i18n.py keys                     rewrite menu/i18n/keys.inl from the sources; prints what changed
    i18n.py check                    exit 1 when keys.inl is stale or a table has a bad entry
    i18n.py status                   keys translated per language
    i18n.py next <lang> [n]          print the next n untranslated keys as "index|||English"
    i18n.py apply <lang> <file>      take "index|||translation" lines, a tab also separates ("=" keeps the English);
                                     bad lines are refused
    i18n.py build                    rewrite languages.inl from the tables that are complete (95% of the keys)

A literal's \n, \" and \\ stay escaped everywhere, in keys and translations alike.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2] / "OptiScaler"
I18N = ROOT / "menu/i18n"
KEYS_FILE = I18N / "keys.inl"
MENU_SOURCES = ["menu/menu_common.cpp", "menu/menu_common.h", "dlssnr/DlssNr_Menu.cpp", "include/imgui/ImGuiNotify.hpp"]
# Elsewhere only toasts show text, so only the literals of a toast count.
TOAST_GLOBS = ["dllmain.cpp", "framegen/**/*.cpp", "hooks/*.cpp", "inputs/*.cpp", "shaders/*.cpp", "upscalers/**/*.cpp"]

# Order of the probable audience; ar and hi stay out, see opt-loop/traducao.md.
LANGUAGES = ["pt-BR", "es", "ru", "zh-CN", "de", "fr", "it", "pl", "tr", "ja", "ko", "uk", "zh-TW", "cs", "nl", "hu",
             "ro", "hr", "lt", "vi", "id", "th"]
WIDE = {"zh-CN", "zh-TW", "ja", "ko"}

# Calls whose literals never reach the screen.
SKIP_CALLS = re.compile(
    r"(LOG_[A-Z_]+|strcmp|_stricmp|stricmp|strstr|wcscmp|GetProcAddress|GetModuleHandle[AW]?|LoadLibrary[AW]?"
    r"|IM_ASSERT|assert|ShellExecute[AW]?|ImGui::PushID|ImGui::GetID|ImGui::BeginChild|ImGui::BeginTable|ScopedCard::Shows"
    r"|ImGui::OpenPopup|ImGui::BeginPopup|ImHashStr|readBool|readString|readInt|readFloat|readUInt|readWString"
    r"|SetValue|GetValue|GetPrivateProfileString[AW]?|WritePrivateProfileString[AW]?|find|contains|ends_with|starts_with|Screenshot::\w+|CheckShortcut)$")


def tokens(src):
    """Yields (kind, value, offset); kind is 'str' or 'code', without comments and preprocessor lines."""
    i, n, line_start = 0, len(src), True
    while i < n:
        c = src[i]
        if c == "\n":
            line_start = True
            i += 1
        elif c in " \t\r":
            i += 1
        elif line_start and c == "#":
            while i < n and src[i] != "\n":
                if src[i] == "\\" and i + 1 < n and src[i + 1] in "\r\n":
                    i += 2
                i += 1
        elif src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
        elif src.startswith("/*", i):
            i = src.find("*/", i) + 2
        elif src.startswith('R"', i):
            line_start = False
            j = src.index("(", i)
            tag = ")" + src[i + 2 : j] + '"'
            end = src.index(tag, j)
            yield "str", src[j + 1 : end].replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n"), i
            i = end + len(tag)
        elif c == '"':
            line_start = False
            j = i + 1
            while src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            yield "str", src[i + 1 : j], i
            i = j + 1
        elif c == "'":
            line_start = False
            j = i + 1
            while src[j] != "'":
                j += 2 if src[j] == "\\" else 1
            i = j + 1
        elif c.isalnum() or c == "_":
            line_start = False
            j = i
            while j < n and (src[j].isalnum() or src[j] in "_:"):
                j += 1
            yield "code", src[i:j], i
            i = j
        else:
            line_start = False
            yield "code", c, i
            i += 1


def literals(src):
    """Yields (literal, offset, open calls) with adjacent literals joined."""
    stack = []
    prev = ""
    cur, cur_off = None, 0
    for kind, value, off in tokens(src):
        if kind == "str":
            if cur is None:
                cur, cur_off = value, off
            else:
                cur += value
            continue
        if cur is not None:
            yield cur, cur_off, list(stack)
            cur = None
        if value in "({[":
            stack.append(prev if value == "(" else value)
        elif value in ")}]" and stack:
            stack.pop()
        prev = value


# Literals that match on status text or that the code draws unchanged.
NOT_KEYS = {"ON {}x", "OpenNR v", "RDNA3 network off: ", "RDNA2 network off: ", " (NO NR)", " (Game DLSS-FG x{})", "Not on the ",
            "Pass ", "NR ", ", ACO", "es", "aco", "dlssnr-amd", "libxess", "daniel", "danielblnc", "lmxxf", "mochizuki",
            # INI values, folders and ID suffixes
            "accent", "bg", "auto", "denoised", "display", "exact", "experimental", "legacy", "new", "optimized",
            "output", "present", "shaders", "vulkan"}


def looks_technical(key):
    """A key name, an enum, a file, a bare format: the same in every language, so no key."""
    if key in NOT_KEYS:
        return True
    key = key.strip()
    if not re.search(r"[A-Za-z]{2}", key):
        return True
    if key.startswith(("http://", "https://", "%s", "D3D", "VK_", "DXGI_", "0x")) and " " not in key:
        return True
    if re.fullmatch(r"[\w.\-/\\:]*\.(dll|ini|exe|log|json|bin|txt|dds|png|cso|hlsl|asi|ttf)", key, re.I):
        return True
    if " " not in key:
        if re.search(r"[a-z][A-Z]", key) or "_" in key or re.search(r"\d", key) or "." in key or "::" in key:
            return True
        if key.isupper():
            return True
    return False


def split_literal(lit):
    """The keys a literal gives: the \\0 items of a combo, each without its ##id."""
    for part in lit.split("\\0"):
        i = part.find("##")
        key = part if i < 0 else part[:i]
        if key and not looks_technical(key):
            yield key


def collect():
    found = {}

    def take(lit, rel, src, off):
        for key in split_literal(lit):
            found.setdefault(key, f"{rel}:{src.count(chr(10), 0, off) + 1}")

    for rel in MENU_SOURCES:
        src = (ROOT / rel).read_text(encoding="utf-8", errors="replace")
        for lit, off, stack in literals(src):
            if not any(SKIP_CALLS.fullmatch(s) for s in stack):
                take(lit, rel, src, off)

    for pat in TOAST_GLOBS:
        for path in sorted(ROOT.glob(pat)):
            src = path.read_text(encoding="utf-8", errors="replace")
            if "InsertNotification" not in src and "ImGuiToast" not in src:
                continue
            rel = path.relative_to(ROOT).as_posix()
            for lit, off, stack in literals(src):
                near = src[max(0, off - 400) : off]
                last = max(near.rfind(w) for w in ("InsertNotification", "setContent", "setTitle", "ImGuiToast"))
                if last >= 0 and ";" not in near[last:] and not any(SKIP_CALLS.fullmatch(s) for s in stack):
                    take(lit, rel, src, off)
    return found


def read_keys():
    if not KEYS_FILE.exists():
        return []
    return [line[1:-2] for line in KEYS_FILE.read_text(encoding="utf-8").splitlines() if line.startswith('"')]


def read_table(lang):
    path = I18N / f"{lang}.inl"
    table = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            m = re.fullmatch(r'\{ "(.*)", "(.*)" \},', line)
            if m:
                table[m.group(1)] = m.group(2)
    return table


def write_table(lang, table):
    lines = [f'{{ "{k}", "{table[k]}" }},\n' for k in sorted(table)]
    (I18N / f"{lang}.inl").write_text("".join(lines), encoding="utf-8", newline="\n")


NARROW = re.compile(r"%(?!%)[-+#0]*\d*(?:\.\d+)?[hlzjtLI]*[diouxXeEfFgGaAcsSpn]")
SPACED = re.compile(r"%(?!%)[-+#0 ]*\d*(?:\.\d+)?[hlzjtLI]*[diouxXeEfFgGaAcsSpn]")
TERMS = ["OptiScaler", "FSR", "DLSS", "XeSS", "XeFG", "OpenRR", "mochizuki", "lmxxf", "danielblnc", "HUDless",
         "Streamline", "Reflex", "NVIDIA", "Nvidia", "AMD", "Intel", "Vulkan", "D3D12", "D3D11", "HDR", "VRAM", "RTSS",
         "OptiScaler.ini", "ReShade", "NGX", "Hudfix", "WrongGPU", "Anti-Lag", "AntiLag", "XeLL", "LatencyFlex", "NRLive"]


def specs(text, pattern):
    # %% pairs go first or %%d would count as a conversion
    return pattern.findall(text.replace("%%", ""))


def lone_percents(text):
    # Percent signs of prose: fine in text drawn as it is, an invalid conversion in a format.
    return NARROW.sub("", text.replace("%%", "")).count("%")


def problems(key, text):
    out = []
    if specs(key, NARROW) != specs(text, NARROW):
        out.append(f"printf conversions differ: {specs(key, NARROW)} vs {specs(text, NARROW)}")
    if specs(text, SPACED) != specs(text, NARROW):
        out.append('a "% " followed by a letter reads as a printf conversion, write the % differently')
    if key.count("%%") != text.count("%%"):
        out.append("%% count differs")
    if lone_percents(text) and not lone_percents(key):
        out.append("a % that starts no conversion where the English has none: in a format it has to be %%")
    if key.count("{}") != text.count("{}"):
        out.append("{} count differs")
    if "##" in text:
        out.append("## in a translation")
    if re.search(r'(?<!\\)(?:\\\\)*"', text):
        out.append('unescaped "')
    if re.search(r"(?<!\\)(?:\\\\)*\\(?![n\"\\t])", text):
        out.append("escape other than \\n, \\\" or \\\\")
    if "\t" in text:
        out.append("raw tab")
    if key.startswith(" ") != text.startswith(" ") or key.endswith(" ") != text.endswith(" "):
        out.append("leading or trailing space differs (the code joins this text to another)")
    if key.startswith("\\n") != text.startswith("\\n") or key.endswith("\\n") != text.endswith("\\n"):
        out.append("leading or trailing \\n differs")
    for term in TERMS:
        if key.count(term) > 0 and text.count(term) == 0:
            out.append(f'keep "{term}" as it is')
            break
    return out


def display_width(text, wide):
    # In columns of the mono font: a CJK or Hangul glyph takes about two.
    return max(sum(2 if ord(c) >= 0x2E80 else 1 for c in line) for line in text.split("\\n"))


def cmd_keys():
    old = set(read_keys())
    found = collect()
    lines = ["// Generated by tools/i18n/i18n.py keys. The menu's English text, also the keys of the tables.\n"]
    lines += [f'"{k}",\n' for k in sorted(found)]
    KEYS_FILE.write_text("".join(lines), encoding="utf-8", newline="\n")
    print(f"{len(found)} keys, {sum(len(k) for k in found)} bytes, {sum(len(k.split()) for k in found)} words; "
          f"{len(set(found) - old)} new, {len(old - set(found))} gone")
    for lang in LANGUAGES:
        table = read_table(lang)
        stale = set(table) - set(found)
        if stale:
            write_table(lang, {k: v for k, v in table.items() if k in found})
            print(f"{lang}: {len(stale)} stale entries dropped")
    return 0


def cmd_check():
    bad = 0
    keys = read_keys()
    if set(keys) != set(collect()):
        print("keys.inl is stale, run i18n.py keys")
        bad += 1
    keyset = set(keys)
    for lang in LANGUAGES:
        for key, text in read_table(lang).items():
            for problem in problems(key, text):
                print(f"{lang}: {problem}: {key[:60]}")
                bad += 1
            if key not in keyset:
                print(f"{lang}: stale key: {key[:60]}")
    print("OK" if not bad else f"{bad} problems")
    return 1 if bad else 0


def cmd_status():
    keys = set(read_keys())
    print(f"{len(keys)} keys")
    for lang in LANGUAGES:
        table = read_table(lang)
        if table:
            have = len(keys & set(table))
            same = sum(1 for k in keys & set(table) if table[k] == k)
            print(f"{lang:6} {have:5} / {len(keys)}  ({same} kept in English)")
    return 0


def cmd_next(lang, count):
    keys = read_keys()
    table = read_table(lang)
    shown = 0
    for i, key in enumerate(keys):
        if key not in table:
            print(f"{i}|||{key}")
            shown += 1
            if shown >= count:
                break
    print(f"# {sum(1 for k in keys if k not in table)} left", file=sys.stderr)
    return 0


def cmd_apply(lang, path):
    keys = read_keys()
    table = read_table(lang)
    taken = refused = 0
    for raw in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        if not raw.strip():
            continue
        parts = raw.split("|||", 1) if "|||" in raw else raw.split("\t", 1)
        if len(parts) != 2 or not parts[0].isdigit() or int(parts[0]) >= len(keys):
            print(f"REFUSED (no index<TAB>text): {raw[:70]}")
            refused += 1
            continue
        key = keys[int(parts[0])]
        text = key if parts[1] == "=" else parts[1]
        # Editors drop trailing spaces: the key's own leading and trailing spaces are what the text gets.
        text = key[: len(key) - len(key.lstrip(" "))] + text.strip(" ") + key[len(key.rstrip(" ")) :]
        trouble = problems(key, text)
        if trouble:
            print(f"REFUSED {parts[0]}: {'; '.join(trouble)}\n  en: {key[:100]}\n  tr: {text[:100]}")
            refused += 1
            continue
        table[key] = text
        taken += 1
    write_table(lang, table)
    print(f"{taken} taken, {refused} refused, {len(keys) - len(set(keys) & set(table))} left")
    return 0


# A language shows in the menu's list once this share of the keys is translated; below it the table stays out of
# the build, a half-translated menu being worse than an English one.
COMPLETE = 0.95


def cmd_build():
    keys = set(read_keys())
    present = [l for l in LANGUAGES if len(keys & set(read_table(l))) >= COMPLETE * len(keys)]
    out = ["// Generated by tools/i18n/i18n.py build.\n"]
    for lang in present:
        out.append(f"const Entry k{lang.replace('-', '')}[] = {{\n#include \"{lang}.inl\"\n}};\n")
    out.append("const Language kLanguages[] = {\n")
    out.append('    { "en", nullptr, 0 },\n')
    for lang in present:
        out.append(f'    {{ "{lang}", k{lang.replace("-", "")}, std::size(k{lang.replace("-", "")}) }},\n')
    out.append("};\n")
    (I18N / "languages.inl").write_text("".join(out), encoding="utf-8", newline="\n")
    print("tables:", " ".join(present))
    return 0


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    if args[0] == "keys":
        return cmd_keys()
    if args[0] == "check":
        return cmd_check()
    if args[0] == "status":
        return cmd_status()
    if args[0] == "next":
        return cmd_next(args[1], int(args[2]) if len(args) > 2 else 200)
    if args[0] == "apply":
        return cmd_apply(args[1], args[2])
    if args[0] == "build":
        return cmd_build()
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
