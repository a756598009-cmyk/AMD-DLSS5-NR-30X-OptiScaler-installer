// What tests\dx11_x86_smoke.cpp and tests\gl_x86_smoke.cpp share: the scene, keys sent to their own window, the
// proxy's counters, the host's logs, and what the screen shows over the window (read through GDI, which sees the
// composed desktop, so the host's swap chain on the child window too).
#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../tools/dx9-x86/dx9_wire.h"

namespace x86look
{
inline int failures = 0;
inline void Expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    failures += ok ? 0 : 1;
}
inline std::string Read(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// Soft shapes with grain that pan slowly, as RGBA texels, top row first: something the network visibly works on.
// Still when frame stays the same.
inline void Scene(std::vector<uint32_t>& pixels, UINT w, UINT h, unsigned frame)
{
    pixels.resize(size_t(w) * h);
    const float t = frame * 0.5f;
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            const float fx = (x + t) * 0.02f, fy = y * 0.013f;
            const float v = 0.5f + 0.25f * std::sin(fx) * std::cos(fy) + 0.1f * std::sin(fx * 7.1f + fy * 3.3f);
            const uint32_t n = (x * 73856093u ^ y * 19349663u ^ frame * 83492791u) >> 27;
            // Brighter towards the bottom, so a picture upside down reads far from the scene.
            const float k = 0.6f + 0.4f * float(y) / float(h);
            const auto c = [&](float m) { return uint32_t(std::clamp(v * m * k * 255.f + n, 0.f, 255.f)); };
            pixels[size_t(y) * w + x] = c(1.f) | c(0.8f) << 8 | c(0.6f) << 16 | 0xFF000000u;
        }
}

// The window's client area as the screen shows it, compared with the scene: the mean difference per channel, upright
// and upside down.
struct Look
{
    double upright = -1, flipped = -1;
};
inline Look LookAt(HWND window, const std::vector<uint32_t>& scene, UINT w, UINT h)
{
    Look look;
    RECT client {};
    GetClientRect(window, &client);
    if (UINT(client.right) != w || UINT(client.bottom) != h)
        return look;
    POINT origin {};
    ClientToScreen(window, &origin);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = LONG(w);
    info.bmiHeader.biHeight = -LONG(h);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    const HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    const HGDIOBJ old = SelectObject(memory, bitmap);
    const bool got = BitBlt(memory, 0, 0, int(w), int(h), screen, origin.x, origin.y, SRCCOPY) != FALSE;
    SelectObject(memory, old);
    if (got && bits)
    {
        uint64_t up = 0, down = 0;
        const auto* seen = static_cast<const uint32_t*>(bits);
        for (UINT y = 0; y < h; y += 2)
            for (UINT x = 0; x < w; x += 2)
            {
                const uint32_t s = seen[size_t(y) * w + x];
                const uint32_t a = scene[size_t(y) * w + x], b = scene[size_t(h - 1 - y) * w + x];
                // The screen is BGRA, the scene RGBA.
                for (int c = 0; c < 3; ++c)
                {
                    const int v = int((s >> (8 * (2 - c))) & 255);
                    up += std::abs(v - int((a >> (8 * c)) & 255));
                    down += std::abs(v - int((b >> (8 * c)) & 255));
                }
            }
        const double samples = double((w + 1) / 2) * ((h + 1) / 2) * 3;
        look.upright = double(up) / samples;
        look.flipped = double(down) / samples;
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return look;
}

inline bool Foreground(HWND window)
{
    const HWND current = GetForegroundWindow();
    if (current == window)
        return true;
    if (!current)
        return SetForegroundWindow(window) && GetForegroundWindow() == window;
    const DWORD other = GetWindowThreadProcessId(current, nullptr), me = GetCurrentThreadId();
    AttachThreadInput(me, other, TRUE);
    SetForegroundWindow(window);
    BringWindowToTop(window);
    SetFocus(window);
    AttachThreadInput(me, other, FALSE);
    return GetForegroundWindow() == window;
}
// Keys go out only while this window is in front (it asks to be), so nothing reaches another program.
inline bool Key(HWND window, WORD vk, bool up)
{
    if (!Foreground(window))
        return false;
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (vk == VK_INSERT ? KEYEVENTF_EXTENDEDKEY : 0);
    return SendInput(1, &input, sizeof(input)) == 1;
}

inline dx9wire::ProxyStatus Status(HMODULE proxy)
{
    dx9wire::ProxyStatus state {};
    using ReadStatus = void(WINAPI*)(dx9wire::ProxyStatus*);
    if (auto read = reinterpret_cast<ReadStatus>(GetProcAddress(proxy, "OptiScalerX86Status")))
        read(&state);
    return state;
}

// The run's phases, as fractions of its time, each marked with the first frame that reaches it: the scene pans, holds
// still for 90 frames for the screen to be read, the menu opens and closes, and the window is resized (and, with full,
// made fullscreen, checked and windowed again).
struct Phases
{
    double stillAt, menuAt, resizeAt, fullAt, windowedAt;
    static constexpr unsigned none = ~0u;
    unsigned still = none, look = none, menu = none, resize = none, full = none, check = none, windowed = none,
             last = none;

    bool Next(unsigned frame, DWORD start, int seconds)
    {
        const double t = (GetTickCount() - start) / (1000.0 * seconds);
        const auto mark = [&](unsigned& at, double when)
        {
            if (at == none && t >= when)
                at = frame;
        };
        mark(still, stillAt);
        look = still == none ? none : still + 90;
        mark(menu, menuAt);
        mark(resize, resizeAt);
        mark(full, fullAt);
        mark(check, windowedAt - 0.03);
        mark(windowed, windowedAt);
        mark(last, 0.98);
        return t < 1;
    }
};

struct Run
{
    HWND window = nullptr;
    HMODULE proxy = nullptr;
    unsigned menuSeen = 0, keysSent = 0;
    bool menuWasOpen = false;
    Look still;
    // The menu key: Insert, pressed at open and again at open + 90; the menu must show within those frames.
    void Menu(unsigned frame, unsigned open)
    {
        if (open == Phases::none)
            return;
        if (frame == open || frame == open + 90)
            keysSent += Key(window, VK_INSERT, false);
        if (frame == open + 4 || frame == open + 94)
            keysSent += Key(window, VK_INSERT, true);
        const bool now = (Status(proxy).state & dx9wire::StateMenu) != 0;
        menuSeen += now && frame >= open && frame < open + 90;
        menuWasOpen = menuWasOpen || now;
    }
    void Report(unsigned frames, UINT resizedW, UINT resizedH, const std::filesystem::path& logs, const char* proxyLog)
    {
        const dx9wire::ProxyStatus s = Status(proxy);
        std::printf("bridged %u native %u host starts %u faults %u transport %u %ux%u menu frames %u keys %u\n",
                    s.bridged, s.native, s.hostStarts, s.faults, s.transport, s.width, s.height, s.menuFrames,
                    keysSent);
        std::printf("still scene on screen: difference %.2f upright, %.2f upside down\n", still.upright,
                    still.flipped);
        Expect(s.bridged > frames / 2, "the host presented most frames");
        Expect(s.faults == 0, "no host fault");
        Expect(still.upright >= 0 && still.upright < still.flipped * 0.6,
               "the window shows the game's picture, upright");
        Expect(still.upright > 0.4, "the picture on screen came out of the network");
        Expect(keysSent == 4 && menuSeen > 20, "the menu opened over the game");
        Expect(s.width == resizedW && s.height == resizedH, "the host follows the resized window");
        const std::string log = Read(logs / L"OptiScaler.log") + Read(logs / L"OptiScaler.previous.log");
        Expect(log.find("Final-image NR: first GPU time of the model") != std::string::npos, "the network ran");
        const std::string own = Read(logs / proxyLog);
        Expect(own.find("first frame presented by the host") != std::string::npos, "the proxy logged the host");
    }
};
} // namespace x86look
