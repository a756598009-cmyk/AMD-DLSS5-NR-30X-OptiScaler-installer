#pragma once
// Paths inside NR are UTF-8 strings, in every file and on every route. std::filesystem::path takes narrow strings as
// UTF-8 under mingw's libstdc++ (and on Linux); Windows' "A" functions and the C runtime's fopen take them in the
// system's ANSI code page instead. A path handed from one to the other broke on any letter outside ASCII - a game in a
// folder with Chinese letters ended (path() threw on the code page's bytes) or found nothing ("????" from
// GetModuleFileNameA) - on Windows and under Proton alike. So: narrow paths are UTF-8, files are opened through
// nr::u8::fopen / nr::u8::path, and Windows is called with wide strings only (nr_path_win.hpp).
// linux/build/check_paths.sh fails the build on the old calls.
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace nr::u8 {

// UTF-8 -> UTF-16 / UTF-32 (wchar_t): a byte that starts no valid sequence becomes U+FFFD
inline std::wstring wide(const char* s) {
    std::wstring w;
    if (!s) return w;
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    while (*p) {
        unsigned c = *p, n = 0;
        if (c < 0x80) n = 0;
        else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
        else { w.push_back(wchar_t(0xFFFD)); ++p; continue; }
        unsigned k = 1;
        for (; k <= n; ++k) {
            if ((p[k] & 0xC0) != 0x80) break;
            c = (c << 6) | (p[k] & 0x3F);
        }
        if (k <= n) { w.push_back(wchar_t(0xFFFD)); p += k; continue; }
        p += n + 1;
        if (sizeof(wchar_t) == 2 && c >= 0x10000) {
            c -= 0x10000;
            w.push_back(wchar_t(0xD800 + (c >> 10)));
            w.push_back(wchar_t(0xDC00 + (c & 0x3FF)));
        } else {
            w.push_back(wchar_t(c));
        }
    }
    return w;
}
inline std::wstring wide(const std::string& s) { return wide(s.c_str()); }

// UTF-16 / UTF-32 (wchar_t) -> UTF-8
inline std::string narrow(const wchar_t* w) {
    std::string s;
    if (!w) return s;
    for (; *w; ++w) {
        unsigned c = unsigned(*w);
        if (sizeof(wchar_t) == 2 && c >= 0xD800 && c < 0xDC00 && unsigned(w[1]) >= 0xDC00 && unsigned(w[1]) < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (unsigned(w[1]) - 0xDC00);
            ++w;
        }
        if (c < 0x80) s.push_back(char(c));
        else if (c < 0x800) { s.push_back(char(0xC0 | (c >> 6))); s.push_back(char(0x80 | (c & 0x3F))); }
        else if (c < 0x10000) {
            s.push_back(char(0xE0 | (c >> 12))); s.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            s.push_back(char(0x80 | (c & 0x3F)));
        } else {
            s.push_back(char(0xF0 | (c >> 18))); s.push_back(char(0x80 | ((c >> 12) & 0x3F)));
            s.push_back(char(0x80 | ((c >> 6) & 0x3F))); s.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return s;
}
inline std::string narrow(const std::wstring& w) { return narrow(w.c_str()); }

// A UTF-8 path as std::filesystem::path (the streams open it with its wide form on Windows; on Linux paths are bytes)
inline std::filesystem::path path(const char* s) {
#ifdef _WIN32
    return std::filesystem::path(wide(s));
#else
    return std::filesystem::path(s ? s : "");
#endif
}
inline std::filesystem::path path(const std::string& s) { return path(s.c_str()); }
inline const std::filesystem::path& path(const std::filesystem::path& p) { return p; }
// and back: the UTF-8 form of a path
inline std::string text(const std::filesystem::path& p) {
#ifdef _WIN32
    return narrow(p.wstring());
#else
    return p.string();
#endif
}

// fopen of a UTF-8 path
inline FILE* fopen(const char* p, const char* mode) {
#ifdef _WIN32
    return ::_wfopen(wide(p).c_str(), wide(mode).c_str());
#else
    return std::fopen(p, mode);
#endif
}
inline FILE* fopen(const std::string& p, const char* mode) { return fopen(p.c_str(), mode); }

// std::remove / std::rename of UTF-8 paths (0: done, as theirs); rename replaces an existing `to`
inline int remove(const std::string& p) {
    std::error_code ec;
    return std::filesystem::remove(path(p), ec) && !ec ? 0 : -1;
}
inline int rename(const std::string& from, const std::string& to) {
    std::error_code ec;
    std::filesystem::rename(path(from), path(to), ec);
    return ec ? -1 : 0;
}

}  // namespace nr::u8
