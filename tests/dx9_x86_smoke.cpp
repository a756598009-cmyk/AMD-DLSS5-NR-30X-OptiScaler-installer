// 32-bit D3D9 program for tools\test-dx9-x86.cmd. It loads the d3d9.dll beside it (the proxy in
// tools\dx9-x86) or, with --native, the system one, renders a moving scene into a real window and
// presents it, and checks what the proxy reports through OptiScalerX86Status: frames the 64-bit
// host presented, host restarts, OptiScaler's menu state and the input held from the game.
//   --ex                 Direct3DCreate9Ex and a D3D9Ex device
//   --frames N           frames to present (default 240)
//   --size WxH           back buffer and client size (default 1280x720)
//   --vsync              present interval one (default immediate)
//   --native             load the system d3d9.dll
//   --window-thread      the window on its own thread, blocked while the main thread renders
//   --expect-bridged     every frame but the first few must be presented by the host
//   --expect-fg          OptiScaler must report frame generation active at the end
//   --menu-at F          press the menu key (Insert) at F, move the mouse over the window, press it again
//   --key-at F VK        press and release a key (hex or decimal virtual key) at frame F
//   --drag-at F X0 Y0 X1 Y1  drag with the left button from X0,Y0 to X1,Y1 (client pixels) from frame F on
//   --pin X Y            from 60 frames before the first drag to the end of the last, keep the cursor on
//                        client point X,Y (a one-pixel clip through the real ClipCursor, as a game that
//                        holds the cursor in place); the drags then reach their start by relative moves only
//   --reset-at F WxH     Reset the device to a new size at frame F
//   --kill-host-at F     end the host process at frame F; the proxy must start another
//   --hold-key F0 F1 VK  press VK at F0 and release it at F1; the game must see the release
//   --ctrl-click-at F X Y  Ctrl + left click at X,Y (client pixels) from frame F on
//   --type-at F TEXT     type TEXT (digits, letters, '.') then Enter from frame F on; the proxy must
//                        pass the characters to the host
//   --recreate-at F      release the device and the window at F and make new ones; the new window
//                        must be presented by a new host, without a failure counted (not with
//                        --window-thread)
//   --minimize-at F0 F1  minimize the window at F0 and restore it at F1
//   --hide-cursor        no cursor over the client area (a game in play hides it)
//   --steer              each drag reaches its start the way a player moves the mouse to the cursor they
//                        see: relative moves only, aimed again from where the cursor is after each one,
//                        after a sweep past the window's top edge
//   --subclass-at F      at F, make the window procedure one that calls nothing installed before it, as
//                        an engine or overlay that replaces it after the device exists would
//   --expect-unhooked    OptiScaler must be reported unhooked in each host, and the session native
//   --fullscreen         ask for a fullscreen device (the proxy makes it windowed); never with --native
//   --interval N         present interval 0 to 4 (default 0, or 1 with --vsync)
//   --second-device      a second window and device, presented after the main one every frame; it must
//                        stay native and cost no host
//   --switch-at F        with --second-device: hide the main window at F and go on with the second device
//                        only; it must get a new host, without a failure counted
//   --flip-every N       with --second-device: every N frames hide the window presented to, show the
//                        other and present only there; the proxy must stop starting hosts for it
//   --late-request F     post the child window request again at F; the game window must still have one
//                        child window at the end
//   --poll-key VK        with --menu-at: hold VK before the menu opens and while it is open; polled with
//                        GetAsyncKeyState, GetKeyState and GetKeyboardState it must read down only while
//                        the menu is closed
//   --clip               clip the cursor to part of the window every frame; with --menu-at the clip must
//                        be lifted while the menu is open, and the game must still read its own
//   --present-rects      a copy swap chain presented with the left half of the back buffer as the source
//                        and the right half of the window as the destination; the right half of the back
//                        buffer is magenta and must not show
//   --lose-device-at F   TestCooperativeLevel reports the device lost for 30 frames from F, then not reset,
//                        and the program resets it; the host must be kept
//   --pan PX             a still textured scene moving PX pixels a frame to the right (0: still)
//   --measure-at F K     capture the window at F and F+K and print how far the picture moved and how much
//                        it changed beyond that move
//   --screen             fail when the window's pixels on screen stop changing
//   --shot-at F FILE     save what the window shows on screen at frame F as a BMP
//   --work MS            spend MS milliseconds of CPU on each frame, as a game would
//   --timeout S          give up after S seconds (default 120)
//   --look-at F S NAME   from frame F, read back for S seconds every frame the screen shows in the window
//                        (tests\screen_look.h) and keep presenting until then; fail on rows of black, which the
//                        scenes never have; without --look-ref write the mean frame as NAME.ref
//   --look-ref FILE      with --look-at: print how far each frame shown is from FILE (a NAME.ref of a still scene)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../tools/dx9-x86/dx9_wire.h"
#include "screen_look.h"

namespace
{
long mouseMessages = 0, keyMessages = 0, charMessages = 0;
// The last key message the game's window procedure saw for watchVk (--hold-key).
UINT watchVk = 0, watchLast = 0;
// The real key and clip calls, past the import table the proxy patches (--poll-key, --clip).
decltype(&GetAsyncKeyState) realAsyncKey = nullptr;
decltype(&GetClipCursor) realGetClip = nullptr;
decltype(&ClipCursor) realClip = nullptr;
decltype(&GetCursorPos) realCursor = nullptr;
// --hide-cursor: no cursor over the client area, as a game that draws its own or none.
bool hideCursor = false;

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_SETCURSOR && hideCursor && LOWORD(l) == HTCLIENT)
    {
        SetCursor(nullptr);
        return TRUE;
    }
    if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
        ++mouseMessages;
    if (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR)
        ++keyMessages;
    if (message == WM_CHAR)
        ++charMessages;
    if ((message == WM_KEYDOWN || message == WM_KEYUP) && watchVk && w == watchVk)
        watchLast = message;
    if (message == WM_CLOSE)
        return 0;
    return DefWindowProcW(window, message, w, l);
}

int Fail(const char* what)
{
    printf("FAIL %s\n", what);
    fflush(stdout);
    return 1;
}

bool Foreground(HWND window)
{
    const HWND current = GetForegroundWindow();
    if (current == window)
        return true;
    // Bringing forward a window of a blocked thread would wait for it.
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())
        return false;
    // With no foreground window at all (one just closed) any process may take the foreground.
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
bool Key(HWND window, WORD vk, bool up)
{
    if (!Foreground(window))
        return false;
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags =
        (up ? KEYEVENTF_KEYUP : 0) |
        (vk == VK_INSERT || vk == VK_END || vk == VK_HOME || vk == VK_PRIOR || vk == VK_NEXT ? KEYEVENTF_EXTENDEDKEY
                                                                                             : 0);
    return SendInput(1, &input, sizeof(input)) == 1;
}

bool MoveMouse(HWND window, int x, int y, DWORD button = 0)
{
    if (!Foreground(window))
        return false;
    POINT point { x, y };
    ClientToScreen(window, &point);
    INPUT input {};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(point.x * 65535.0 / (GetSystemMetrics(SM_CXSCREEN) - 1));
    input.mi.dy = static_cast<LONG>(point.y * 65535.0 / (GetSystemMetrics(SM_CYSCREEN) - 1));
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | button;
    return SendInput(1, &input, sizeof(input)) == 1;
}

// A move as a mouse makes it: relative, which raw input reports as it is.
bool NudgeMouse(HWND window, int dx, int dy, DWORD button = 0)
{
    if (!Foreground(window))
        return false;
    INPUT input {};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | button;
    return SendInput(1, &input, sizeof(input)) == 1;
}

// --steer: pointer speed and acceleration make the cursor's pixels differ from the counts sent, and the
// sweep leaves the cursor at the top of the screen, above the window's top edge. Moves of at most 8
// counts, as a hand slows down near what it aims at (larger ones overshoot under acceleration).
bool SteerMouse(HWND window, int x, int y)
{
    if (!NudgeMouse(window, 0, -4000))
        return false;
    POINT target { x, y };
    ClientToScreen(window, &target);
    POINT at {};
    realCursor(&at);
    for (int i = 0; i < 400; ++i)
    {
        // Wait for the last move to reach the cursor before aiming again.
        for (int wait = 0; wait < 25; ++wait)
        {
            POINT now {};
            realCursor(&now);
            const bool moved = now.x != at.x || now.y != at.y;
            at = now;
            if (moved)
                break;
            Sleep(2);
        }
        if (std::abs(target.x - at.x) <= 1 && std::abs(target.y - at.y) <= 1)
            return true;
        if (!NudgeMouse(window, std::clamp(int(target.x - at.x), -8, 8), std::clamp(int(target.y - at.y), -8, 8)))
            return false;
    }
    printf("steer: the cursor stopped at %ld,%ld on its way to %ld,%ld (screen)\n", at.x, at.y, target.x, target.y);
    return true;
}

COLORREF ScreenPixel(HWND window, int x, int y)
{
    POINT point { x, y };
    ClientToScreen(window, &point);
    // Another program's window over this point would make the sample mean nothing.
    if (WindowFromPoint(point) != window)
        return CLR_INVALID;
    HDC screen = GetDC(nullptr);
    const COLORREF colour = GetPixel(screen, point.x, point.y);
    ReleaseDC(nullptr, screen);
    return colour;
}

// The window's client area as the screen shows it, as a 24-bit BMP.
bool Shot(HWND window, const char* path)
{
    RECT client {};
    GetClientRect(window, &client);
    POINT origin {};
    ClientToScreen(window, &origin);
    BITMAPINFOHEADER header { sizeof(header), client.right, client.bottom, 1, 24, BI_RGB };
    void* bits = nullptr;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap =
        CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    const HGDIOBJ old = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, client.right, client.bottom, screen, origin.x, origin.y, SRCCOPY);
    GdiFlush();
    const DWORD bytes = ((client.right * 3 + 3) & ~3) * client.bottom;
    BITMAPFILEHEADER file { 0x4D42, DWORD(sizeof(file) + sizeof(header) + bytes), 0, 0,
                            DWORD(sizeof(file) + sizeof(header)) };
    FILE* out = nullptr;
    const bool saved = fopen_s(&out, path, "wb") == 0 && out && fwrite(&file, sizeof(file), 1, out) == 1 &&
                       fwrite(&header, sizeof(header), 1, out) == 1 && fwrite(bits, bytes, 1, out) == 1;
    if (out)
        fclose(out);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return saved;
}

HWND MakeWindow(UINT width, UINT height, int x = -1, int y = 40)
{
    WNDCLASSW type {};
    type.lpfnWndProc = Proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursor(nullptr, IDC_ARROW);
    type.lpszClassName = L"OptiDx9X86Smoke";
    RegisterClassW(&type);
    RECT rect { 0, 0, LONG(width), LONG(height) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    const int left = x >= 0 ? x : std::max(0, GetSystemMetrics(SM_CXSCREEN) - int(rect.right - rect.left) - 40);
    HWND window =
        CreateWindowExW(WS_EX_TOPMOST, type.lpszClassName, L"OptiScaler DX9 x86 smoke", WS_OVERLAPPEDWINDOW, left, y,
                        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, type.hInstance, nullptr);
    if (window)
    {
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
        // On the window's own thread: from another one, bringing it forward waits for this thread.
        Foreground(window);
    }
    return window;
}

// --window-thread: the window lives on a thread of its own that pumps its messages once a frame and
// is otherwise blocked waiting for the rendering thread, as an engine with a separate render thread
// can be. Nothing may make the Present wait for that thread.
struct WindowThread
{
    UINT width, height;
    HANDLE go = nullptr, pumped = nullptr;
    HWND window = nullptr;
};
DWORD WINAPI WindowLoop(LPVOID context)
{
    auto pump = static_cast<WindowThread*>(context);
    pump->window = MakeWindow(pump->width, pump->height);
    SetEvent(pump->pumped);
    for (;;)
    {
        WaitForSingleObject(pump->go, INFINITE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        SetEvent(pump->pumped);
    }
}

DWORD HostProcess()
{
    DWORD found = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry { sizeof(entry) };
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
        if (entry.th32ParentProcessID == GetCurrentProcessId() &&
            _wcsicmp(entry.szExeFile, L"OptiScalerDx9Host.exe") == 0)
            found = entry.th32ProcessID;
    CloseHandle(snapshot);
    return found;
}

struct Vertex
{
    float x, y, z, rhw;
    DWORD colour;
};

void Draw(IDirect3DDevice9* device, UINT width, UINT height, int frame)
{
    const float t = frame * 0.05f;
    const auto channel = [&](float phase) { return static_cast<DWORD>(127.5f + 127.5f * std::sin(t + phase)); };
    device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(channel(0), channel(2.1f), channel(4.2f)), 1, 0);
    device->BeginScene();
    const float x = std::fmod(frame * 8.0f, static_cast<float>(width));
    const float w = width / 8.0f;
    const Vertex bar[] {
        { x, 0, 0, 1, 0xffffffff },
        { x + w, 0, 0, 1, 0xffff2020 },
        { x, float(height), 0, 1, 0xff2020ff },
        { x + w, float(height), 0, 1, 0xff20ff20 },
    };
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bar, sizeof(Vertex));
    device->EndScene();
}

DWORD WINAPI Watchdog(LPVOID seconds)
{
    Sleep(static_cast<DWORD>(reinterpret_cast<uintptr_t>(seconds)) * 1000);
    printf("FAIL timeout\n");
    fflush(stdout);
    if (realClip)
        realClip(nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}

// --pan: a still scene of coloured rectangles (fixed seed), large ones under small ones so the whole
// picture has detail, moved right by offset pixels and wrapped.
void DrawPan(IDirect3DDevice9* device, UINT width, UINT height, int offset)
{
    static std::vector<Vertex> quads;
    static UINT madeFor = 0;
    if (madeFor != width * 65536 + height)
    {
        madeFor = width * 65536 + height;
        quads.clear();
        unsigned seed = 12345;
        const auto next = [&] { return (seed = seed * 1103515245u + 12345u) >> 8; };
        for (int i = 0; i < 4400; ++i)
        {
            const UINT size = i < 400 ? 96 : 12;
            const float x = float(next() % width), y = float(next() % height);
            const float w = float(size / 4 + next() % size), h = float(size / 4 + next() % size);
            const DWORD colour = 0xff000000 | (next() & 0xffffff);
            // Twice, one width apart, so the scene wraps when it moves.
            for (const float shift : { 0.0f, -float(width) })
            {
                const Vertex a { x + shift, y, 0, 1, colour }, b { x + shift + w, y, 0, 1, colour },
                    c { x + shift, y + h, 0, 1, colour }, d { x + shift + w, y + h, 0, 1, colour };
                quads.insert(quads.end(), { a, b, c, b, d, c });
            }
        }
    }
    std::vector<Vertex> moved = quads;
    const float dx = float(offset % int(width));
    for (auto& v : moved)
        v.x += dx;
    device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(40, 40, 48), 1, 0);
    device->BeginScene();
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(moved.size() / 3), moved.data(), sizeof(Vertex));
    device->EndScene();
}

// The window's client area as the screen shows it, 32-bit BGRX rows top down.
std::vector<DWORD> Capture(HWND window, int width, int height)
{
    std::vector<DWORD> pixels(size_t(width) * height);
    POINT origin {};
    ClientToScreen(window, &origin);
    BITMAPINFOHEADER header { sizeof(header), width, -height, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap =
        CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    const HGDIOBJ old = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY);
    GdiFlush();
    std::memcpy(pixels.data(), bits, pixels.size() * 4);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return pixels;
}

// How far right the picture moved between two captures (searched up to maxShift pixels), and the mean
// absolute difference per colour channel (0 to 255) that is left once that move is taken out. Measured
// right of the left third (an on-screen display such as RivaTuner's draws there) and in the middle half
// of the rows.
void Compare(const std::vector<DWORD>& a, const std::vector<DWORD>& b, int width, int height, int maxShift, int& shift,
             double& residual)
{
    shift = 0;
    residual = 1e9;
    const int x0 = width / 3, x1 = width - maxShift - 1, y0 = height / 4, y1 = height * 3 / 4;
    for (int s = 0; s <= maxShift; ++s)
    {
        double sum = 0;
        long count = 0;
        for (int y = y0; y < y1; y += 2)
            for (int x = x0; x < x1; x += 2, ++count)
            {
                const DWORD p = a[size_t(y) * width + x], q = b[size_t(y) * width + x + s];
                for (int c = 0; c < 24; c += 8)
                    sum += std::abs(int((p >> c) & 255) - int((q >> c) & 255));
            }
        const double mean = count ? sum / (3.0 * count) : 1e9;
        if (mean < residual)
        {
            residual = mean;
            shift = s;
        }
    }
}

// --look-at: what the screen shows over the window, read on a thread of its own while the frames go on.
struct LookJob
{
    double seconds = 0;
    RECT area {};
    std::wstring folder, name, reference;
    ScreenLook result;
};
DWORD WINAPI LookThread(LPVOID job)
{
    auto& j = *static_cast<LookJob*>(job);
    j.result =
        LookAtScreen(j.seconds, j.area, j.folder, j.name.c_str(), j.reference.empty() ? nullptr : j.reference.c_str());
    return 0;
}

// --lose-device-at: what TestCooperativeLevel reports while a lost device is simulated.
using TestLevel = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
TestLevel realTestLevel = nullptr;
std::atomic<int> lostFrames { 0 };
HRESULT STDMETHODCALLTYPE SimulatedTestLevel(IDirect3DDevice9* device)
{
    const int left = lostFrames;
    return left > 1 ? D3DERR_DEVICELOST : left == 1 ? D3DERR_DEVICENOTRESET : realTestLevel(device);
}
} // namespace

int main(int argc, char** argv)
{
    bool ex = false, native = false, vsync = false, windowThread = false, expectBridged = false, expectFg = false,
         screen = false, expectUnhooked = false, fullscreen = false, secondDevice = false, clip = false,
         presentRects = false;
    int interval = -1, switchAt = -1, flipEvery = 0, flips = 0, lateRequestAt = -1, lostAt = -1, pan = -1,
        measureAt = -1, measureGap = 0;
    WORD pollVk = 0;
    int frames = 240, menuAt = -1, resetAt = -1, killAt = -1, timeout = 120, shotAt = -1;
    int holdDown = -1, holdUp = -1, ctrlAt = -1, ctrlX = 0, ctrlY = 0, typeAt = -1, recreateAt = -1, minimizeAt = -1,
        restoreAt = -1, subclassAt = -1;
    std::string typeText;
    const char* shotPath = nullptr;
    double work = 0;
    int lookAt = -1;
    LookJob lookJob;
    HANDLE look = nullptr;
    UINT width = 1280, height = 720, resetWidth = 0, resetHeight = 0;
    std::vector<std::pair<int, WORD>> keys;
    struct Drag
    {
        int at, x0, y0, x1, y1;
    };
    std::vector<Drag> drags;
    int pinX = -1, pinY = -1;
    bool steer = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? argv[++i] : "0"; };
        if (a == "--ex")
            ex = true;
        else if (a == "--native")
            native = true;
        else if (a == "--window-thread")
            windowThread = true;
        else if (a == "--vsync")
            vsync = true;
        else if (a == "--expect-bridged")
            expectBridged = true;
        else if (a == "--expect-fg")
            expectFg = true;
        else if (a == "--screen")
            screen = true;
        else if (a == "--expect-unhooked")
            expectUnhooked = true;
        else if (a == "--fullscreen")
            fullscreen = true;
        else if (a == "--interval")
            interval = atoi(next());
        else if (a == "--second-device")
            secondDevice = true;
        else if (a == "--switch-at")
            switchAt = atoi(next());
        else if (a == "--flip-every")
            flipEvery = atoi(next());
        else if (a == "--late-request")
            lateRequestAt = atoi(next());
        else if (a == "--poll-key")
            pollVk = static_cast<WORD>(strtoul(next(), nullptr, 0));
        else if (a == "--clip")
            clip = true;
        else if (a == "--present-rects")
            presentRects = true;
        else if (a == "--lose-device-at")
            lostAt = atoi(next());
        else if (a == "--pan")
            pan = atoi(next());
        else if (a == "--measure-at")
        {
            measureAt = atoi(next());
            measureGap = atoi(next());
        }
        else if (a == "--hold-key")
        {
            holdDown = atoi(next());
            holdUp = atoi(next());
            watchVk = static_cast<UINT>(strtoul(next(), nullptr, 0));
        }
        else if (a == "--ctrl-click-at")
        {
            ctrlAt = atoi(next());
            ctrlX = atoi(next());
            ctrlY = atoi(next());
        }
        else if (a == "--type-at")
        {
            typeAt = atoi(next());
            typeText = next();
        }
        else if (a == "--recreate-at")
            recreateAt = atoi(next());
        else if (a == "--subclass-at")
            subclassAt = atoi(next());
        else if (a == "--minimize-at")
        {
            minimizeAt = atoi(next());
            restoreAt = atoi(next());
        }
        else if (a == "--look-at")
        {
            lookAt = atoi(next());
            lookJob.seconds = atof(next());
            const std::string text = next();
            lookJob.name.assign(text.begin(), text.end());
        }
        else if (a == "--look-ref")
        {
            const std::string text = next();
            lookJob.reference.assign(text.begin(), text.end());
        }
        else if (a == "--frames")
            frames = atoi(next());
        else if (a == "--timeout")
            timeout = atoi(next());
        else if (a == "--work")
            work = atof(next());
        else if (a == "--size")
            sscanf_s(next(), "%ux%u", &width, &height);
        else if (a == "--menu-at")
            menuAt = atoi(next());
        else if (a == "--kill-host-at")
            killAt = atoi(next());
        else if (a == "--shot-at")
        {
            shotAt = atoi(next());
            shotPath = next();
        }
        else if (a == "--reset-at")
        {
            resetAt = atoi(next());
            sscanf_s(next(), "%ux%u", &resetWidth, &resetHeight);
        }
        else if (a == "--key-at")
        {
            const int at = atoi(next());
            keys.push_back({ at, static_cast<WORD>(strtoul(next(), nullptr, 0)) });
        }
        else if (a == "--hide-cursor")
            hideCursor = true;
        else if (a == "--steer")
            steer = true;
        else if (a == "--pin")
        {
            pinX = atoi(next());
            pinY = atoi(next());
        }
        else if (a == "--drag-at")
        {
            Drag d {};
            d.at = atoi(next());
            d.x0 = atoi(next());
            d.y0 = atoi(next());
            d.x1 = atoi(next());
            d.y1 = atoi(next());
            drags.push_back(d);
        }
        else
            return Fail(("unknown option " + a).c_str());
    }
    if (fullscreen && native)
        return Fail("--fullscreen with --native would change the display mode");
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    realAsyncKey = reinterpret_cast<decltype(realAsyncKey)>(GetProcAddress(user32, "GetAsyncKeyState"));
    realGetClip = reinterpret_cast<decltype(realGetClip)>(GetProcAddress(user32, "GetClipCursor"));
    realClip = reinterpret_cast<decltype(realClip)>(GetProcAddress(user32, "ClipCursor"));
    realCursor = reinterpret_cast<decltype(realCursor)>(GetProcAddress(user32, "GetCursorPos"));
    CreateThread(nullptr, 0, Watchdog, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(timeout)), 0, nullptr);

    wchar_t path[MAX_PATH] {};
    if (native)
    {
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\d3d9.dll");
    }
    else
    {
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wcscpy_s(wcsrchr(path, L'\\') + 1, MAX_PATH - (wcsrchr(path, L'\\') + 1 - path), L"d3d9.dll");
    }
    const HMODULE d3d9 = LoadLibraryW(path);
    if (!d3d9)
        return Fail("d3d9.dll did not load");
    using StatusFn = void(WINAPI*)(dx9wire::ProxyStatus*);
    const auto readStatus = reinterpret_cast<StatusFn>(GetProcAddress(d3d9, "OptiScalerX86Status"));
    if (!native && !readStatus)
        return Fail("the d3d9.dll beside the program is not the proxy");
    auto Status = [&]
    {
        dx9wire::ProxyStatus s {};
        if (readStatus)
            readStatus(&s);
        return s;
    };

    WindowThread pump { width, height };
    HWND window = nullptr;
    if (windowThread)
    {
        pump.go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        pump.pumped = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CreateThread(nullptr, 0, WindowLoop, &pump, 0, nullptr);
        WaitForSingleObject(pump.pumped, INFINITE);
        window = pump.window;
    }
    else
        window = MakeWindow(width, height);
    if (!window)
        return Fail("window");
    // One turn of the window's messages, on the window's thread when it has its own.
    auto Pump = [&]
    {
        if (windowThread)
        {
            SetEvent(pump.go);
            WaitForSingleObject(pump.pumped, INFINITE);
            return;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    };
    const bool front = GetForegroundWindow() == window;

    IDirect3D9* api = nullptr;
    IDirect3D9Ex* apiEx = nullptr;
    if (ex)
    {
        auto create =
            reinterpret_cast<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>(GetProcAddress(d3d9, "Direct3DCreate9Ex"));
        if (!create || FAILED(create(D3D_SDK_VERSION, &apiEx)))
            return Fail("Direct3DCreate9Ex");
        api = apiEx;
    }
    else
    {
        auto create = reinterpret_cast<IDirect3D9*(WINAPI*) (UINT)>(GetProcAddress(d3d9, "Direct3DCreate9"));
        api = create ? create(D3D_SDK_VERSION) : nullptr;
        if (!api)
            return Fail("Direct3DCreate9");
    }
    D3DPRESENT_PARAMETERS params {};
    params.BackBufferWidth = width;
    params.BackBufferHeight = height;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = 1;
    // Present rectangles need a copy swap chain.
    params.SwapEffect = presentRects ? D3DSWAPEFFECT_COPY : D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window;
    params.Windowed = !fullscreen;
    const DWORD intervals[] { D3DPRESENT_INTERVAL_IMMEDIATE, D3DPRESENT_INTERVAL_ONE, D3DPRESENT_INTERVAL_TWO,
                              D3DPRESENT_INTERVAL_THREE, D3DPRESENT_INTERVAL_FOUR };
    params.PresentationInterval = interval >= 0 && interval <= 4 ? intervals[interval]
                                  : vsync                        ? D3DPRESENT_INTERVAL_ONE
                                                                 : D3DPRESENT_INTERVAL_IMMEDIATE;
    DEVMODEW desktop {};
    desktop.dmSize = sizeof(desktop);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &desktop);
    if (fullscreen)
        params.FullScreen_RefreshRateInHz = desktop.dmDisplayFrequency;
    D3DDISPLAYMODEEX mode { sizeof(mode),    width,
                            height,          desktop.dmDisplayFrequency,
                            D3DFMT_X8R8G8B8, D3DSCANLINEORDERING_PROGRESSIVE };
    IDirect3DDevice9* device = nullptr;
    IDirect3DDevice9Ex* deviceEx = nullptr;
    const DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    auto Make = [&](HWND target, D3DPRESENT_PARAMETERS& with, IDirect3DDevice9*& made, IDirect3DDevice9Ex*& madeEx)
    {
        made = nullptr;
        madeEx = nullptr;
        HRESULT hr = S_OK;
        if (ex)
        {
            hr = apiEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, target, flags, &with,
                                       with.Windowed ? nullptr : &mode, &madeEx);
            made = madeEx;
        }
        else
            hr = api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, target, flags, &with, &made);
        if (FAILED(hr))
            printf("device creation failed %08lX (windowed %d, interval %08lX)\n", hr, with.Windowed,
                   with.PresentationInterval);
        return SUCCEEDED(hr);
    };
    if (!Make(window, params, device, deviceEx))
        return Fail(ex ? "CreateDeviceEx" : "CreateDevice");
    if (fullscreen)
        printf("fullscreen device asked for: created with windowed %d, interval %08lX\n", params.Windowed,
               params.PresentationInterval);
    // --second-device: another window and device of the same program, bottom left.
    HWND window2 = nullptr;
    IDirect3DDevice9* device2 = nullptr;
    IDirect3DDevice9Ex* device2Ex = nullptr;
    D3DPRESENT_PARAMETERS params2 = params;
    UINT width2 = 640, height2 = 360;
    if (secondDevice)
    {
        window2 = MakeWindow(width2, height2, 20, 600);
        Foreground(window);
        params2.BackBufferWidth = width2;
        params2.BackBufferHeight = height2;
        params2.hDeviceWindow = window2;
        if (!window2 || !Make(window2, params2, device2, device2Ex))
            return Fail("the second device");
    }
    // One frame of the main device: the scene (moving bar, or --pan), with the right half magenta for
    // --present-rects; then its Present.
    auto Render = [&](int n)
    {
        if (pan >= 0)
            DrawPan(device, width, height, n * pan);
        else
            Draw(device, width, height, n);
        if (presentRects)
        {
            const D3DRECT right { LONG(width / 2), 0, LONG(width), LONG(height) };
            device->Clear(1, &right, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 255), 1, 0);
        }
    };
    auto Show = [&]
    {
        const RECT source { 0, 0, LONG(width / 2), LONG(height) },
            dest { LONG(width / 2), 0, LONG(width), LONG(height) };
        const RECT* from = presentRects ? &source : nullptr;
        const RECT* to = presentRects ? &dest : nullptr;
        return ex ? deviceEx->PresentEx(from, to, nullptr, nullptr, 0) : device->Present(from, to, nullptr, nullptr);
    };
    auto ShowSecond = [&](int n)
    {
        if (!device2)
            return;
        Draw(device2, width2, height2, n + 40);
        device2->Present(nullptr, nullptr, nullptr, nullptr);
    };
    // After the device or window changed: present until a new host presents, so the frames counted
    // after are its (the old host's last answer may still be counted once the new device presents).
    auto Rewarm = [&](const dx9wire::ProxyStatus& before, unsigned& newHostBridged)
    {
        const ULONGLONG until = GetTickCount64() + 60000;
        int rewarm = 0;
        bool newHost = false;
        for (dx9wire::ProxyStatus now; !(now = Status()).disabled && GetTickCount64() < until;)
        {
            if (!newHost && now.hostStarts > before.hostStarts)
            {
                newHost = true;
                newHostBridged = now.bridged;
            }
            else if (newHost && now.bridged > newHostBridged)
                break;
            Pump();
            Render(rewarm++);
            Show();
        }
        return rewarm;
    };

    COLORREF samples[2] {};
    int sampled = 0;
    unsigned bridgedAtKill = 0, menuSeen = 0, menuGone = 0, inputSent = 0, keysSent = 0, keysSkipped = 0;
    long mouseWhileMenu = 0;
    bool menuOpenNow = false;
    unsigned bridgedAtReset = 0;
    dx9wire::ProxyStatus atRecreate {}, atLost {};
    unsigned newHostBridged = 0;
    // --poll-key: frames the key read down through the program's imports while the menu was closed, and
    // while it was open; frames it was really down while the menu was open.
    unsigned polledClosed = 0, polledOpen = 0, realOpen = 0;
    // --clip: frames with the menu open (or closed) for three frames or more, those where the real clip
    // was lifted (or in place), and those where the program read back its own clip.
    unsigned clipOpenFrames = 0, clipLifted = 0, clipSeen = 0, clipClosedFrames = 0, clipInPlace = 0;
    int menuChangedAt = 0;
    bool menuWasOpen = false, resetAfterLoss = false;
    std::vector<DWORD> measured;
    COLORREF samples2[2] {};
    // The proxy starts its host on a thread of its own and presents natively until the host is up, so
    // present until it is: the frames counted below are then the bridge's.
    dx9wire::ProxyStatus warm {};
    if (!native)
    {
        const ULONGLONG until = GetTickCount64() + 60000;
        int warmFrames = 0;
        while ((warm = Status()).bridged == 0 && !warm.disabled && GetTickCount64() < until)
        {
            Pump();
            Render(warmFrames);
            Show();
            ShowSecond(warmFrames++);
        }
        printf("warm-up: %d frames until the host presented\n", warmFrames);
    }
    LARGE_INTEGER started {}, ended {}, rate {};
    QueryPerformanceFrequency(&rate);
    QueryPerformanceCounter(&started);
    int framesRun = 0;
    POINT pinMoved { pinX, pinY };
    for (int frame = 0; frame < frames || (look && WaitForSingleObject(look, 0) == WAIT_TIMEOUT); ++frame)
    {
        framesRun = frame + 1;
        Pump();
        if (frame == lookAt)
        {
            GetClientRect(window, &lookJob.area);
            MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&lookJob.area), 2);
            wchar_t self[MAX_PATH] {};
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            lookJob.folder = self;
            lookJob.folder.resize(lookJob.folder.find_last_of(L'\\'));
            look = CreateThread(nullptr, 0, LookThread, &lookJob, 0, nullptr);
        }
        // A game that checks its device each frame and resets it once it can.
        if (frame == lostAt)
        {
            auto table = *reinterpret_cast<void***>(device);
            DWORD old = 0;
            VirtualProtect(&table[3], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
            realTestLevel = reinterpret_cast<TestLevel>(table[3]);
            table[3] = reinterpret_cast<void*>(SimulatedTestLevel);
            VirtualProtect(&table[3], sizeof(void*), old, &old);
            atLost = Status();
            lostFrames = 30;
        }
        if (lostAt >= 0 && device->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
        {
            const HRESULT hr = ex ? deviceEx->ResetEx(&params, nullptr) : device->Reset(&params);
            printf("device reset after the simulated loss at frame %d: %08lX\n", frame, hr);
            resetAfterLoss = SUCCEEDED(hr);
            lostFrames = 0;
        }
        const bool menuNow = (Status().state & dx9wire::StateMenu) != 0;
        if (menuNow != menuWasOpen)
            menuChangedAt = frame;
        menuWasOpen = menuNow;
        const bool settled = frame - menuChangedAt >= 3;
        if (pollVk && menuAt >= 0)
        {
            // Down before the menu opens, and again while it is open.
            if (frame == menuAt - 30 || frame == menuAt + 25)
                (Key(window, pollVk, false) ? keysSent : keysSkipped)++;
            if (frame == menuAt - 20 || frame == menuAt + 45)
                (Key(window, pollVk, true) ? keysSent : keysSkipped)++;
            BYTE board[256] {};
            GetKeyboardState(board);
            const bool polled =
                (GetAsyncKeyState(pollVk) & 0x8000) || (GetKeyState(pollVk) & 0x8000) || (board[pollVk] & 0x80);
            polledClosed += polled && !menuNow ? 1 : 0;
            polledOpen += polled && menuNow ? 1 : 0;
            realOpen += (realAsyncKey(pollVk) & 0x8000) && menuNow ? 1 : 0;
        }
        if (clip)
        {
            POINT origin {};
            ClientToScreen(window, &origin);
            const RECT box { origin.x, origin.y, origin.x + 400, origin.y + 300 };
            ClipCursor(&box);
            RECT real {}, seen {};
            realGetClip(&real);
            GetClipCursor(&seen);
            if (settled && menuNow)
            {
                ++clipOpenFrames;
                clipLifted += EqualRect(&real, &box) ? 0 : 1;
                clipSeen += EqualRect(&seen, &box) ? 1 : 0;
            }
            else if (settled)
            {
                ++clipClosedFrames;
                clipInPlace += EqualRect(&real, &box) ? 1 : 0;
            }
        }
        if (frame == lateRequestAt)
            PostMessageW(window, RegisterWindowMessageW(L"OptiScalerDx9X86Surface"), 1, 0);
        if (device2 && flipEvery > 0 && frame > 0 && frame % flipEvery == 0)
        {
            // The other window comes up and the program presents only there.
            ShowWindow(window, SW_HIDE);
            ShowWindow(window2, SW_SHOWNOACTIVATE);
            std::swap(window, window2);
            std::swap(device, device2);
            std::swap(deviceEx, device2Ex);
            std::swap(params, params2);
            std::swap(width, width2);
            std::swap(height, height2);
            ++flips;
        }
        if (frame == switchAt && device2)
        {
            // The main window goes away and the program goes on with its second device only.
            atRecreate = Status();
            ShowWindow(window, SW_HIDE);
            std::swap(window, window2);
            std::swap(device, device2);
            std::swap(deviceEx, device2Ex);
            std::swap(params, params2);
            std::swap(width, width2);
            std::swap(height, height2);
            const int rewarm = Rewarm(atRecreate, newHostBridged);
            printf("switched to the second device at frame %d (bridged %u), %d frames until a new host presented it\n",
                   frame, atRecreate.bridged, rewarm);
        }
        // A drag: the cursor to the start, a relative nudge there and back (a menu reading another
        // process's window follows a mouse by its relative moves, from where the cursor was), button
        // down, twelve relative steps, button up; one event a frame, as OptiScaler reads the mouse once a
        // frame.
        // With --pin the menu can only follow the mouse by its relative moves, from where they left it. The
        // cursor is held from 60 frames before the first drag (before the menu opens in the runner's cases),
        // as a game holds it all along.
        if (pinX >= 0 && !drags.empty() && frame >= drags.front().at - 60 && frame <= drags.back().at + 25)
        {
            POINT point { pinX, pinY };
            ClientToScreen(window, &point);
            const RECT pin { point.x, point.y, point.x + 1, point.y + 1 };
            realClip(frame <= drags.back().at + 24 ? &pin : nullptr);
        }
        for (const auto& d : drags)
        {
            const int step = frame - d.at;
            bool sent = true;
            if (step == 0 && pinX >= 0)
            {
                sent = NudgeMouse(window, d.x0 - pinMoved.x, d.y0 - pinMoved.y);
                pinMoved = { d.x0 + (d.x1 - d.x0) / 12 * 12, d.y0 + (d.y1 - d.y0) / 12 * 12 };
            }
            else if (step == 0 && steer)
                sent = SteerMouse(window, d.x0, d.y0);
            else if (step == 0)
                sent = MoveMouse(window, d.x0, d.y0);
            else if (step == 2)
                sent = NudgeMouse(window, 1, 0);
            else if (step == 4)
                sent = NudgeMouse(window, -1, 0, MOUSEEVENTF_LEFTDOWN);
            else if (step > 7 && step <= 19)
                sent = NudgeMouse(window, (d.x1 - d.x0) / 12, (d.y1 - d.y0) / 12);
            else if (step == 22)
                sent = NudgeMouse(window, 0, 0, MOUSEEVENTF_LEFTUP);
            else
                continue;
            (sent ? keysSent : keysSkipped)++;
        }
        for (const auto& [at, vk] : keys)
        {
            if (frame == at || frame == at + 6)
            {
                const bool sent = Key(window, vk, frame != at);
                (sent ? keysSent : keysSkipped)++;
            }
        }
        if (menuAt >= 0)
        {
            // Open at menuAt, move the mouse over the window while it is open, close 60 frames later.
            if (frame == menuAt || frame == menuAt + 60)
                (Key(window, VK_INSERT, false) ? keysSent : keysSkipped)++;
            if (frame == menuAt + 6 || frame == menuAt + 66)
                (Key(window, VK_INSERT, true) ? keysSent : keysSkipped)++;
            const bool open = (Status().state & dx9wire::StateMenu) != 0;
            if (open && !menuOpenNow)
            {
                menuSeen = frame;
                mouseWhileMenu = mouseMessages;
            }
            if (!open && menuOpenNow)
            {
                menuGone = frame;
                mouseWhileMenu = mouseMessages - mouseWhileMenu;
            }
            menuOpenNow = open;
            if (open && frame >= menuAt + 20 && frame < menuAt + 50)
                inputSent += MoveMouse(window, 100 + (frame % 10) * 20, 100 + (frame % 7) * 15) ? 1 : 0;
        }
        if (frame == holdDown || frame == holdUp)
            (Key(window, static_cast<WORD>(watchVk), frame == holdUp) ? keysSent : keysSkipped)++;
        // Ctrl + click: the cursor there, a relative nudge there and back, Ctrl down, the button down and
        // up, Ctrl up; one event every other frame.
        if (ctrlAt >= 0 && frame >= ctrlAt && frame <= ctrlAt + 14 && !((frame - ctrlAt) & 1))
        {
            const int step = frame - ctrlAt;
            const bool sent = step == 0    ? MoveMouse(window, ctrlX, ctrlY)
                              : step == 2  ? NudgeMouse(window, 1, 0)
                              : step == 4  ? NudgeMouse(window, -1, 0)
                              : step == 6  ? Key(window, VK_CONTROL, false)
                              : step == 8  ? NudgeMouse(window, 0, 0, MOUSEEVENTF_LEFTDOWN)
                              : step == 12 ? NudgeMouse(window, 0, 0, MOUSEEVENTF_LEFTUP)
                              : step == 14 ? Key(window, VK_CONTROL, true)
                                           : true;
            (sent ? keysSent : keysSkipped)++;
        }
        // One character every six frames (down, then up three frames later), then Enter.
        if (typeAt >= 0 && frame >= typeAt && (frame - typeAt) % 3 == 0)
        {
            const size_t index = (frame - typeAt) / 6;
            if (index <= typeText.size())
            {
                const char c = index < typeText.size() ? typeText[index] : '\r';
                const WORD vk = c == '\r'  ? VK_RETURN
                                : c == '.' ? VK_OEM_PERIOD
                                           : static_cast<WORD>(toupper(static_cast<unsigned char>(c)));
                (Key(window, vk, (frame - typeAt) % 6 != 0) ? keysSent : keysSkipped)++;
            }
        }
        if (frame == recreateAt)
        {
            atRecreate = Status();
            device->Release();
            DestroyWindow(window);
            window = MakeWindow(width, height);
            params.hDeviceWindow = window;
            device = nullptr;
            deviceEx = nullptr;
            const HRESULT hr =
                ex ? apiEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, flags, &params, nullptr,
                                           &deviceEx)
                   : api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, flags, &params, &device);
            if (FAILED(hr))
                return Fail("the device could not be made again");
            if (ex)
                device = deviceEx;
            const int rewarm = Rewarm(atRecreate, newHostBridged);
            printf("made a new device and window at frame %d (bridged %u), %d frames until a new host presented it\n",
                   frame, atRecreate.bridged, rewarm);
        }
        if (frame == subclassAt)
            printf("window procedure replaced at frame %d (it was %s)\n", frame,
                   SetWindowLongPtrW(window, GWLP_WNDPROC, LONG_PTR(Proc)) == LONG_PTR(Proc) ? "the program's own"
                                                                                             : "another one");
        if (frame == minimizeAt)
            ShowWindow(window, SW_MINIMIZE);
        if (frame == restoreAt)
        {
            ShowWindow(window, SW_RESTORE);
            Foreground(window);
        }
        if (frame == killAt)
        {
            bridgedAtKill = Status().bridged;
            const DWORD pid = HostProcess();
            HANDLE process = pid ? OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid) : nullptr;
            if (!process)
                return Fail("no host process to end");
            TerminateProcess(process, 9);
            WaitForSingleObject(process, 5000);
            CloseHandle(process);
            printf("ended host %lu at frame %d\n", pid, frame);
        }
        if (frame == resetAt)
        {
            bridgedAtReset = Status().bridged;
            width = resetWidth;
            height = resetHeight;
            RECT r { 0, 0, LONG(width), LONG(height) };
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(window, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                         SWP_NOMOVE | SWP_NOZORDER | (windowThread ? SWP_ASYNCWINDOWPOS : 0));
            params.BackBufferWidth = width;
            params.BackBufferHeight = height;
            const HRESULT hr = ex ? deviceEx->ResetEx(&params, nullptr) : device->Reset(&params);
            if (FAILED(hr))
                return Fail("Reset");
            printf("reset to %ux%u at frame %d\n", width, height, frame);
        }
        Render(frame);
        LARGE_INTEGER spin {}, now {};
        QueryPerformanceCounter(&spin);
        do
            QueryPerformanceCounter(&now);
        while (1000.0 * double(now.QuadPart - spin.QuadPart) / double(rate.QuadPart) < work);
        const HRESULT hr = Show();
        if (FAILED(hr))
        {
            printf("Present failed %08lX at frame %d\n", hr, frame);
            return Fail("Present");
        }
        if (flipEvery <= 0 && (switchAt < 0 || frame < switchAt))
            ShowSecond(frame);
        if (lostFrames > 1)
            --lostFrames;
        if (measureAt >= 0 && (frame == measureAt || frame == measureAt + measureGap))
        {
            Sleep(30);
            auto picture = Capture(window, int(width), int(height));
            if (frame == measureAt)
                measured = std::move(picture);
            else
            {
                const int reach = std::min<int>(std::max(0, (measureGap + 6) * pan), int(width) / 2);
                int moved = 0;
                double residual = 0;
                Compare(measured, picture, int(width), int(height), reach, moved, residual);
                printf("stability: the picture moved %d px in %d frames (the scene moved %d), mean change left %.2f "
                       "of 255\n",
                       moved, measureGap, measureGap * std::max(0, pan), residual);
            }
        }
        if (frame == shotAt)
        {
            Sleep(50);
            printf("shot %s: %s\n", shotPath, Shot(window, shotPath) ? "saved" : "failed");
        }
        // Right of centre: an on-screen display such as RivaTuner's draws over the left side.
        if (frame == frames - 40 || frame == frames - 10)
        {
            Sleep(30);
            if (window2 && (switchAt < 0 || frame < switchAt))
                samples2[sampled] = ScreenPixel(window2, width2 * 3 / 4, height2 / 2);
            samples[sampled++] = ScreenPixel(window, width * 3 / 4, height / 2);
        }
    }

    QueryPerformanceCounter(&ended);
    const double msPerFrame = 1000.0 * double(ended.QuadPart - started.QuadPart) / double(rate.QuadPart) / framesRun;
    const auto s = Status();
    const bool covered = samples[0] == CLR_INVALID || samples[1] == CLR_INVALID;
    const bool screenMoves = !covered && samples[0] != samples[1];
    printf("status: bridged=%u native=%u hostStarts=%u faults=%u disabled=%u transport=%u size=%ux%u state=%u "
           "menuFrames=%u blockedInput=%u typed=%u unhooked=%u\n",
           s.bridged, s.native, s.hostStarts, s.faults, s.disabled, s.transport, s.width, s.height, s.state,
           s.menuFrames, s.blockedInput, s.typed, s.unhooked);
    printf("screen=%s samples=%06lX,%06lX foreground=%d keysSent=%u keysSkipped=%u ms_per_frame=%.2f\n",
           covered       ? "covered"
           : screenMoves ? "ok"
                         : "unchanged",
           samples[0], samples[1], front ? 1 : 0, keysSent, keysSkipped, msPerFrame);

    int failed = 0;
    if (lookAt >= 0 && !look)
        failed = Fail("the screen was not read back: --look-at is past the last frame");
    if (look)
    {
        WaitForSingleObject(look, INFINITE);
        CloseHandle(look);
        const ScreenLook& l = lookJob.result;
        printf(
            "look %ls: %u frames shown, %u with black rows (%u rows, %u above the middle, %u below, longest run %u); "
            "difference to the reference p10 %.2f p50 %.2f p90 %.2f mean %.2f\n",
            lookJob.name.c_str(), l.frames, l.black, l.rows, l.top, l.bottom, l.run, l.p10, l.p50, l.p90, l.mean);
        if (!l.error.empty() || !l.frames)
            failed = Fail(("the screen was not read back: " + l.error).c_str());
        else if (l.black)
            failed = Fail("frames shown with rows of black");
    }
    if (screen && !screenMoves)
        failed = Fail("the window's pixels on screen did not change between samples");
    // A Reset, an ended host, a new window or a lost device may cost frames presented natively.
    const unsigned allowance =
        8 + (killAt >= 0 || resetAt >= 0 || recreateAt >= 0 || switchAt >= 0 || lostAt >= 0 ? 300 : 0);
    if (presentRects && (samples[0] == RGB(255, 0, 255) || samples[1] == RGB(255, 0, 255)))
        failed = Fail("the window shows the part of the back buffer outside the Present source rectangle");
    if (flipEvery > 0)
    {
        printf("flips: %d, host starts %u, faults %u, disabled %u\n", flips, s.hostStarts, s.faults, s.disabled);
        if (!s.disabled || s.hostStarts > 6)
            failed = Fail("a program moving between windows kept starting hosts");
    }
    else if (device2 && switchAt < 0)
    {
        const bool covered2 = samples2[0] == CLR_INVALID || samples2[1] == CLR_INVALID;
        printf("second device: screen=%s samples=%06lX,%06lX\n",
               covered2                     ? "covered"
               : samples2[0] != samples2[1] ? "ok"
                                            : "unchanged",
               samples2[0], samples2[1]);
        if (s.hostStarts != 1 || s.faults || s.native < unsigned(frames))
            failed = Fail("a second device presenting to another window cost a host or was not presented natively");
        if (screen && !covered2 && samples2[0] == samples2[1])
            failed = Fail("the second device's window did not change on screen");
    }
    if (switchAt >= 0)
    {
        printf("switch: bridged before %u, when the new host started %u, at the end %u, starts %u, faults %u\n",
               atRecreate.bridged, newHostBridged, s.bridged, s.hostStarts, s.faults);
        if (s.hostStarts != atRecreate.hostStarts + 1 || s.faults != atRecreate.faults ||
            s.bridged + 8 < newHostBridged + unsigned(frames - switchAt))
            failed = Fail("the second device's window was not presented by a new host");
    }
    if (lateRequestAt >= 0)
    {
        int children = 0;
        for (HWND child = FindWindowExW(window, nullptr, L"OptiScalerDx9X86Surface", nullptr); child;
             child = FindWindowExW(window, child, L"OptiScalerDx9X86Surface", nullptr))
            ++children;
        printf("late request: %d child windows at the end\n", children);
        if (children != 1)
            failed = Fail("the game window does not have exactly one child window");
    }
    if (pollVk)
    {
        printf("polled key 0x%X: read down %u frames with the menu closed, %u with it open (really down %u)\n", pollVk,
               polledClosed, polledOpen, realOpen);
        if (!keysSkipped && (!polledClosed || !realOpen || polledOpen))
            failed = Fail("polled keys were not held while the menu was open, or not seen while it was closed");
    }
    if (clip)
    {
        realClip(nullptr);
        printf("clip: open %u frames, lifted %u, the program read its own %u; closed %u frames, in place %u\n",
               clipOpenFrames, clipLifted, clipSeen, clipClosedFrames, clipInPlace);
        if (menuAt >= 0 && !keysSkipped &&
            (!clipOpenFrames || clipLifted != clipOpenFrames || clipSeen != clipOpenFrames ||
             clipInPlace != clipClosedFrames))
            failed = Fail("the cursor clip was not lifted under the menu, or not in place without it");
    }
    if (lostAt >= 0)
    {
        printf("lost device: native %u -> %u, bridged %u -> %u, starts %u -> %u, faults %u -> %u, reset %s\n",
               atLost.native, s.native, atLost.bridged, s.bridged, atLost.hostStarts, s.hostStarts, atLost.faults,
               s.faults, resetAfterLoss ? "done" : "not done");
        if (!resetAfterLoss || s.hostStarts != atLost.hostStarts || s.faults != atLost.faults ||
            s.native < atLost.native + 25 || s.bridged + 40 < atLost.bridged + unsigned(frames - lostAt))
            failed = Fail("a lost device cost a host, or the bridge did not carry on after the reset");
    }
    if (expectBridged && (s.bridged - warm.bridged + allowance < unsigned(frames) || s.disabled))
        failed = Fail("frames were not presented by the host");
    if (expectFg && !(s.state & dx9wire::StateFrameGen))
        failed = Fail("frame generation was not active at the end");
    if (resetAt >= 0 && (s.width != resetWidth || s.height != resetHeight || s.bridged < bridgedAtReset + 100))
        failed = Fail("the bridge did not carry on at the new size after Reset");
    if (killAt >= 0)
    {
        printf("restart: bridged before end %u, after %u, starts %u, faults %u\n", bridgedAtKill, s.bridged,
               s.hostStarts, s.faults);
        if (s.hostStarts < 2 || s.faults < 1 || s.bridged < bridgedAtKill + 30)
            failed = Fail("the host was not started again after it ended");
    }
    if (watchVk)
    {
        printf("held key 0x%X: the game's last message for it was %s\n", watchVk,
               watchLast == WM_KEYDOWN ? "a key down"
               : watchLast == WM_KEYUP ? "a key up"
                                       : "none");
        if (!keysSkipped && watchLast != WM_KEYUP)
            failed = Fail("a key released while the menu was open stayed down in the game");
    }
    if (typeAt >= 0)
    {
        printf("typed: %u characters passed to the host, %ld reached the game\n", s.typed, charMessages);
        if (!keysSkipped && s.typed < typeText.size())
            failed = Fail("the characters typed into the menu did not reach the host");
    }
    if (recreateAt >= 0)
    {
        printf("recreate: bridged before %u, when the new host started %u, at the end %u, starts %u, faults %u\n",
               atRecreate.bridged, newHostBridged, s.bridged, s.hostStarts, s.faults);
        if (s.hostStarts < atRecreate.hostStarts + 1 || s.faults != atRecreate.faults ||
            s.bridged + 8 < newHostBridged + unsigned(frames - recreateAt))
            failed = Fail("the new window was not presented by a new host");
    }
    if (expectUnhooked && (s.unhooked < 4 || !s.disabled || s.bridged))
        failed = Fail("hosts without OptiScaler's hooks were not detected and the session did not go native");
    if (menuAt >= 0)
    {
        printf("menu: opened at %u, closed at %u, mouse moves sent %u, mouse messages the game saw %ld, held %u\n",
               menuSeen, menuGone, inputSent, mouseWhileMenu, s.blockedInput);
        if (keysSkipped)
            printf("SKIP menu: this window was not in front, no keys were sent\n");
        else if (!menuSeen || !menuGone)
            failed = Fail("the menu did not open and close with Insert");
        else if (inputSent && (mouseWhileMenu != 0 || s.blockedInput == 0))
            failed = Fail("mouse input reached the game while the menu was open");
    }
    if ((!keys.empty() || holdDown >= 0 || ctrlAt >= 0 || typeAt >= 0) && keysSkipped)
        printf("SKIP keys: %u key events not sent (window not in front)\n", keysSkipped);

    device->Release();
    if (device2)
        device2->Release();
    api->Release();
    if (!windowThread)
        DestroyWindow(window);
    if (!failed)
        printf("PASS\n");
    fflush(stdout);
    return failed;
}
