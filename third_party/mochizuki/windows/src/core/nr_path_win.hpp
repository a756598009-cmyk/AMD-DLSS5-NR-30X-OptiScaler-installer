#pragma once
// Windows' file functions with UTF-8 paths (nr_path_u8.hpp: why): each calls the wide ("W") function. The Windows
// builds only (the .asi, the ReShade add-on, the NGX DLLs).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <string>

#include "nr_path_u8.hpp"

namespace nr::u8 {

// GetModuleFileNameA's shape: the module's file, UTF-8, into `out` (`size` bytes with the NUL); its length, 0 when
// it failed or does not fit. No heap use: the crash handlers call it.
inline DWORD module_file(HMODULE m, char* out, DWORD size) {
    if (size) out[0] = 0;
    wchar_t w[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(m, w, MAX_PATH * 4);
    if (!n || n >= MAX_PATH * 4 || !size) return 0;
    const int got = WideCharToMultiByte(CP_UTF8, 0, w, int(n), out, int(size) - 1, nullptr, nullptr);
    if (got <= 0) { out[0] = 0; return 0; }
    out[got] = 0;
    return DWORD(got);
}
inline std::string module_file(HMODULE m) {
    std::wstring w(32768, L'\0');
    const DWORD n = GetModuleFileNameW(m, w.data(), DWORD(w.size()));
    if (!n || n >= w.size()) return {};
    w.resize(n);
    return narrow(w);
}

// Each leaves the call's GetLastError for its caller: the wide strings are freed first, then the error put back.
inline DWORD attributes(const std::string& p) {
    DWORD r, e;
    { const std::wstring w = wide(p); r = GetFileAttributesW(w.c_str()); e = GetLastError(); }
    SetLastError(e);
    return r;
}
inline BOOL attributes_ex(const std::string& p, WIN32_FILE_ATTRIBUTE_DATA* d) {
    BOOL r; DWORD e;
    { const std::wstring w = wide(p); r = GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, d); e = GetLastError(); }
    SetLastError(e);
    return r;
}
inline BOOL copy_file(const std::string& from, const std::string& to, BOOL fail_if_exists) {
    BOOL r; DWORD e;
    { const std::wstring a = wide(from), b = wide(to); r = CopyFileW(a.c_str(), b.c_str(), fail_if_exists); e = GetLastError(); }
    SetLastError(e);
    return r;
}
inline BOOL make_dir(const std::string& p, SECURITY_ATTRIBUTES* sa = nullptr) {
    BOOL r; DWORD e;
    { const std::wstring w = wide(p); r = CreateDirectoryW(w.c_str(), sa); e = GetLastError(); }
    SetLastError(e);
    return r;
}
inline HANDLE create_file(const std::string& p, DWORD access, DWORD share, SECURITY_ATTRIBUTES* sa, DWORD disposition,
                          DWORD flags, HANDLE templ) {
    HANDLE r; DWORD e;
    { const std::wstring w = wide(p); r = CreateFileW(w.c_str(), access, share, sa, disposition, flags, templ); e = GetLastError(); }
    SetLastError(e);
    return r;
}

// A folder's entries for a pattern ("C:\\x\\*.txt"): f(name in UTF-8, attributes) for each
template <class F> inline void list(const std::string& pattern, F f) {
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW(wide(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do f(narrow(fd.cFileName), fd.dwFileAttributes);
    while (FindNextFileW(h, &fd));
    FindClose(h);
}

// GetPrivateProfileStringA's shape for our ini files in UTF-8 (GetPrivateProfileString reads a file without a BOM in
// the ANSI code page), a UTF-16 one (saved as "Unicode" by Notepad) read too: the key's value from the first matching
// section, names compared without case, the value trimmed and one pair of quotes around it taken off; `def` when there
// is none. Into `out` (`size` bytes); its length.
inline DWORD profile(const char* section, const char* key, const char* def, char* out, DWORD size, const char* file) {
    const auto lower = [](std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    const auto trim = [](std::string s) {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
    };
    std::string value = def ? def : "";
    std::string text;
    if (FILE* f = fopen(file, "rb")) {
        char buf[4096];
        for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, got);
        std::fclose(f);
    }
    if (text.size() >= 2 && uint8_t(text[0]) == 0xFF && uint8_t(text[1]) == 0xFE) {   // UTF-16 LE
        std::wstring w((text.size() - 2) / 2, L'\0');
        for (size_t i = 0; i < w.size(); ++i) w[i] = wchar_t(uint8_t(text[2 + 2 * i]) | (uint8_t(text[3 + 2 * i]) << 8));
        text = narrow(w);
    } else if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        text.erase(0, 3);
    }
    const std::string want_sec = lower(section ? section : ""), want_key = lower(key ? key : "");
    bool in = false;
    for (size_t at = 0; at < text.size();) {
        const size_t eol = text.find('\n', at);
        std::string line = trim(text.substr(at, eol == std::string::npos ? std::string::npos : eol - at));
        at = eol == std::string::npos ? text.size() : eol + 1;
        if (line.empty()) continue;
        if (line.front() == '[') {
            const size_t close = line.find(']');
            const bool was = in;
            in = lower(trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1))) == want_sec;
            if (was && !in) break;   // the first matching section only
            continue;
        }
        const size_t eq = line.find('=');
        if (!in || eq == std::string::npos || lower(trim(line.substr(0, eq))) != want_key) continue;
        value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        break;
    }
    if (!size) return 0;
    const size_t n = std::min<size_t>(value.size(), size - 1);
    std::memcpy(out, value.data(), n);
    out[n] = 0;
    return DWORD(n);
}
inline DWORD profile(const char* section, const char* key, const char* def, char* out, DWORD size, const std::string& file) {
    return profile(section, key, def, out, size, file.c_str());
}

}  // namespace nr::u8
