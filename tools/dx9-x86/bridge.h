#pragma once
// What the 32-bit proxies share: the 64-bit host (host.cpp, in the OptiScaler64 folder beside the proxy) they hand
// the game's frames to, the child window it presents into, the game's input held back while OptiScaler's menu is
// open, and the bridge that runs one host per game window. Each proxy brings its API as Bridge's Api (the device,
// its window and adapter, and the copy of the back buffer the host reads) and its names, defined before this is
// included: LogName (beside the host), Section (in OptiScaler.ini), SurfaceName (the child window's class and
// message) and PipePrefix.
#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "dx9_wire.h"

using Microsoft::WRL::ComPtr;
using namespace dx9wire;

namespace
{
HMODULE self = nullptr;
std::recursive_mutex lock;
std::filesystem::path hostFolder;
ProxyStatus status {};
std::atomic_bool menuOpen { false };

void Log(const char* format, ...)
{
    char line[1024] {};
    SYSTEMTIME t {};
    GetLocalTime(&t);
    int used = sprintf_s(line, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, format);
    vsprintf_s(line + used, sizeof(line) - used, format, args);
    va_end(args);
    FILE* file = nullptr;
    if (_wfopen_s(&file, (hostFolder / LogName).c_str(), L"a") == 0 && file)
    {
        fprintf(file, "%s\n", line);
        fclose(file);
    }
}

bool Replace(void** slot, void* replacement, void** original)
{
    DWORD old = 0;
    if (*slot == replacement || !VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
        return false;
    *original = InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), replacement);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// [Section] in OptiScaler.ini beside the host: true, false, 1 or 0; anything else is the default.
bool Setting(const wchar_t* key, bool fallback)
{
    wchar_t value[16] {};
    GetPrivateProfileStringW(Section, key, L"", value, 16, (hostFolder / L"OptiScaler.ini").c_str());
    if (!_wcsicmp(value, L"true") || !wcscmp(value, L"1"))
        return true;
    if (!_wcsicmp(value, L"false") || !wcscmp(value, L"0"))
        return false;
    return fallback;
}

// The host presents into a child window of the game's window, never into the game's window itself.
// A flip swap chain whose process ends without releasing it leaves its window unable to take another
// one (measured: E_ACCESSDENIED ten seconds after the presenting process was ended), and the game's
// own Present no longer shows there either. A lost host then only costs its child window: it is
// destroyed, the game's Present shows again, and the next host gets a new one. The child is made on
// the game window's thread, which pumps its messages; it takes no input (disabled, and transparent to
// hit testing), so the mouse and keys still reach the game's window. It is asked for and ended with
// posted messages: an engine that renders on its own thread may hold its window thread waiting for
// that very thread, so the Present never waits for the window thread.
const wchar_t* const surfaceClass = SurfaceName;
UINT surfaceRequest = 0;
// The child made for the next host, and the one a host presents into.
std::atomic<HWND> madeSurface { nullptr }, takenSurface { nullptr };
// The game's top-level windows (those of its device window and its focus window).
std::atomic<HWND> gameRoot { nullptr }, focusRoot { nullptr };

// Input the game must not see while OptiScaler's menu is open. The host reads the mouse and keys
// itself; the game's queued input messages, its key and cursor polls and its cursor clip are what is
// held back here.
POINT heldCursor {};
decltype(&GetCursorPos) realGetCursorPos = GetCursorPos;
decltype(&SetCursorPos) realSetCursorPos = SetCursorPos;
decltype(&GetAsyncKeyState) realGetAsyncKeyState = GetAsyncKeyState;
decltype(&GetKeyState) realGetKeyState = GetKeyState;
decltype(&GetKeyboardState) realGetKeyboardState = GetKeyboardState;
decltype(&ClipCursor) realClipCursor = ClipCursor;
decltype(&GetClipCursor) realGetClipCursor = GetClipCursor;
// The game's cursor clip while the menu is open (the menu needs the whole screen); put back when it
// closes. clipNone: the game had no clip, or asked for none.
std::mutex clipLock;
RECT gameClip {};
bool clipNone = true;
// Keys and mouse buttons (by virtual key) the game saw go down, from window messages and from raw
// input. Their release still reaches the game while the menu is open, so none stays held in the game.
std::bitset<256> gameDown, rawDown;
// Characters typed while the menu is open, passed to the host with the next frame for the menu's text
// fields.
std::mutex typedLock;
std::wstring typed;

bool HeldInput(UINT message)
{
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_INPUT || message == WM_KEYDOWN ||
           message == WM_KEYUP || message == WM_CHAR || message == WM_DEADCHAR || message == WM_UNICHAR;
}
LRESULT CALLBACK SurfaceProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    return message == WM_NCHITTEST ? HTTRANSPARENT : DefWindowProcW(window, message, w, l);
}
HWND MakeSurface(HWND parent)
{
    static const ATOM type = []
    {
        WNDCLASSW kind {};
        kind.lpfnWndProc = SurfaceProc;
        kind.hInstance = self;
        kind.lpszClassName = surfaceClass;
        return RegisterClassW(&kind);
    }();
    RECT client {};
    GetClientRect(parent, &client);
    return CreateWindowExW(WS_EX_NOPARENTNOTIFY, MAKEINTATOM(type), L"",
                           WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS, 0, 0, client.right, client.bottom,
                           parent, nullptr, self, nullptr);
}

// Fullscreen the host can be seen in. An exclusive fullscreen swap chain owns the display and the host's swap chain on
// its window would never show, so a game that asks for fullscreen stays windowed and its window becomes a borderless
// one over its monitor: it looks like fullscreen, and the game is told it is. Done on the window's own thread, which
// takes the request from its queue (Filter), for the reason the child window is: the Present never waits for it.
UINT restyleRequest = 0;
struct Restyled
{
    HWND window = nullptr;
    LONG_PTR style = 0, exStyle = 0;
    RECT rect {};
} restyled;
void RestyleNow(HWND window, bool borderless)
{
    if (borderless)
    {
        if (restyled.window != window)
        {
            restyled = { window, GetWindowLongPtrW(window, GWL_STYLE), GetWindowLongPtrW(window, GWL_EXSTYLE) };
            GetWindowRect(window, &restyled.rect);
        }
        MONITORINFO monitor { sizeof(monitor) };
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT& r = monitor.rcMonitor;
        SetWindowLongPtrW(window, GWL_STYLE, (restyled.style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(window, GWL_EXSTYLE,
                          restyled.exStyle &
                              ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        Log("window %p is borderless over its monitor (%ldx%ld) for the fullscreen the game asked for", window,
            r.right - r.left, r.bottom - r.top);
    }
    else if (restyled.window == window)
    {
        SetWindowLongPtrW(window, GWL_STYLE, restyled.style);
        SetWindowLongPtrW(window, GWL_EXSTYLE, restyled.exStyle);
        const RECT& r = restyled.rect;
        SetWindowPos(window, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        restyled = {};
        Log("window %p is back to its own style for windowed mode", window);
    }
}

// The virtual key of a key or mouse button message, and whether it presses or releases it; 0 for other
// messages.
int MessageKey(const MSG& msg, bool& down)
{
    down = false;
    switch (msg.message)
    {
    case WM_KEYDOWN:
        down = true;
        [[fallthrough]];
    case WM_KEYUP:
        return static_cast<int>(msg.wParam & 0xFF);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        down = true;
        [[fallthrough]];
    case WM_LBUTTONUP:
        return VK_LBUTTON;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        down = true;
        [[fallthrough]];
    case WM_RBUTTONUP:
        return VK_RBUTTON;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        down = true;
        [[fallthrough]];
    case WM_MBUTTONUP:
        return VK_MBUTTON;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        down = true;
        [[fallthrough]];
    case WM_XBUTTONUP:
        return GET_XBUTTON_WPARAM(msg.wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    }
    return 0;
}

// Follows the keys and buttons a raw input packet presses and releases; true when it releases one the
// game saw go down.
bool RawRelease(LPARAM handle, bool menu)
{
    RAWINPUT data {};
    UINT size = sizeof(data);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(handle), RID_INPUT, &data, &size, sizeof(RAWINPUTHEADER)) ==
        UINT(-1))
        return false;
    bool owed = false;
    const auto follow = [&](UINT vk, bool down, bool up)
    {
        if (up && rawDown[vk])
        {
            rawDown[vk] = false;
            owed = true;
        }
        else if (down && !menu)
            rawDown[vk] = true;
    };
    if (data.header.dwType == RIM_TYPEKEYBOARD && data.data.keyboard.VKey < 256)
    {
        const bool up = (data.data.keyboard.Flags & RI_KEY_BREAK) != 0;
        follow(data.data.keyboard.VKey, !up, up);
    }
    else if (data.header.dwType == RIM_TYPEMOUSE)
    {
        // RI_MOUSE_LEFT_BUTTON_DOWN and _UP, then right, middle, button 4 and button 5, two bits each.
        const UINT buttons[] { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
        const USHORT flags = data.data.mouse.usButtonFlags;
        for (UINT b = 0; b < 5; ++b)
            follow(buttons[b], (flags & (1u << (2 * b))) != 0, (flags & (2u << (2 * b))) != 0);
    }
    return owed;
}

void Consume(MSG& msg)
{
    msg.message = WM_NULL;
    msg.wParam = 0;
    msg.lParam = 0;
}

// Every message the game window's thread takes from its queue passes here first: the child window
// requests, and the game's input while the menu is open. A message hook sees them whatever window
// procedures the game or an overlay install later.
void Filter(MSG& msg)
{
    if (msg.message == surfaceRequest && surfaceRequest)
    {
        // One child per game window: a request that arrives late, after a child was made or taken for
        // this window, makes none.
        const auto ours = [&](HWND child) { return child && IsWindow(child) && GetParent(child) == msg.hwnd; };
        if (msg.wParam)
        {
            if (!ours(madeSurface) && !ours(takenSurface))
                if (const HWND unused = madeSurface.exchange(MakeSurface(msg.hwnd)))
                    DestroyWindow(unused);
        }
        else if (IsWindow(reinterpret_cast<HWND>(msg.lParam)))
            DestroyWindow(reinterpret_cast<HWND>(msg.lParam));
        Consume(msg);
        return;
    }
    if (msg.message == restyleRequest && restyleRequest)
    {
        RestyleNow(msg.hwnd, msg.wParam != 0);
        Consume(msg);
        return;
    }
    if (!HeldInput(msg.message))
        return;
    // Raw input is the game's whichever of its windows it is registered to.
    const HWND root = msg.hwnd ? GetAncestor(msg.hwnd, GA_ROOT) : nullptr;
    if (msg.message != WM_INPUT && (!root || (root != gameRoot && root != focusRoot)))
        return;
    const bool menu = menuOpen;
    bool down = false, owed = false;
    if (const int vk = MessageKey(msg, down); vk && down && !menu)
        gameDown[vk] = true;
    else if (vk && !down && gameDown[vk])
    {
        gameDown[vk] = false;
        owed = true;
    }
    if (msg.message == WM_INPUT)
        owed = RawRelease(msg.lParam, menu);
    if (!menu || owed)
        return;
    if (msg.message == WM_CHAR || msg.message == WM_UNICHAR)
    {
        std::lock_guard guard(typedLock);
        if (msg.wParam >= 0x20 && msg.wParam < 0xFFFF && typed.size() < 256)
            typed.push_back(static_cast<wchar_t>(msg.wParam));
    }
    // The characters of a held key, for the menu (the game's own TranslateMessage gets WM_NULL).
    else if (msg.message == WM_KEYDOWN)
        TranslateMessage(&msg);
    // The clean-up the game's handling of WM_INPUT would have done.
    else if (msg.message == WM_INPUT && GET_RAWINPUT_CODE_WPARAM(msg.wParam) == RIM_INPUT)
        DefWindowProcW(msg.hwnd, msg.message, msg.wParam, msg.lParam);
    ++status.blockedInput;
    Consume(msg);
}
LRESULT CALLBACK MessageHook(int code, WPARAM removal, LPARAM l)
{
    if (code == HC_ACTION && (removal & PM_REMOVE) && l)
        Filter(*reinterpret_cast<MSG*>(l));
    return CallNextHookEx(nullptr, code, removal, l);
}

BOOL WINAPI HeldGetCursorPos(LPPOINT point)
{
    if (menuOpen && point)
    {
        *point = heldCursor;
        return TRUE;
    }
    return realGetCursorPos(point);
}
BOOL WINAPI HeldSetCursorPos(int x, int y) { return menuOpen ? TRUE : realSetCursorPos(x, y); }
// Keys polled while the menu is open read as up, as OptiScaler's own hold does in a game it runs in.
SHORT WINAPI HeldGetAsyncKeyState(int vk) { return menuOpen ? 0 : realGetAsyncKeyState(vk); }
SHORT WINAPI HeldGetKeyState(int vk) { return menuOpen ? 0 : realGetKeyState(vk); }
BOOL WINAPI HeldGetKeyboardState(PBYTE keys)
{
    const BOOL result = realGetKeyboardState(keys);
    if (result && keys && menuOpen)
        std::memset(keys, 0, 256);
    return result;
}
BOOL WINAPI HeldClipCursor(const RECT* rect)
{
    {
        std::lock_guard guard(clipLock);
        if (menuOpen)
        {
            clipNone = !rect;
            if (rect)
                gameClip = *rect;
            return TRUE;
        }
    }
    return realClipCursor(rect);
}
RECT VirtualScreen()
{
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return { x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
}
BOOL WINAPI HeldGetClipCursor(LPRECT rect)
{
    {
        std::lock_guard guard(clipLock);
        if (menuOpen && rect)
        {
            *rect = clipNone ? VirtualScreen() : gameClip;
            return TRUE;
        }
    }
    return realGetClipCursor(rect);
}

void PatchImports(HMODULE module)
{
    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return;
    const auto pair = [](auto real, auto replacement)
    { return std::pair { reinterpret_cast<void*>(real), reinterpret_cast<void*>(replacement) }; };
    const std::pair<void*, void*> held[] {
        pair(realGetCursorPos, HeldGetCursorPos),         pair(realSetCursorPos, HeldSetCursorPos),
        pair(realGetAsyncKeyState, HeldGetAsyncKeyState), pair(realGetKeyState, HeldGetKeyState),
        pair(realGetKeyboardState, HeldGetKeyboardState), pair(realClipCursor, HeldClipCursor),
        pair(realGetClipCursor, HeldGetClipCursor),
    };
    for (auto entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); entry->Name; ++entry)
    {
        if (_stricmp(reinterpret_cast<char*>(base + entry->Name), "user32.dll") != 0)
            continue;
        for (auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->FirstThunk); thunk->u1.Function; ++thunk)
            for (const auto& [real, replacement] : held)
            {
                void* original = nullptr;
                if (thunk->u1.Function == reinterpret_cast<uintptr_t>(real))
                    Replace(reinterpret_cast<void**>(&thunk->u1.Function), replacement, &original);
            }
    }
}
// DirectInput 8 (Just Cause 2 reads the mouse there): while the menu is open a device's state reads as nothing
// pressed and nothing moved, and its buffered data keeps only releases, so no key or button stays held in the game.
// The ANSI and wide devices have tables of their own; every device of a kind shares one.
using DeviceState = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using DeviceData = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
DeviceState realDeviceState[2] {};
DeviceData realDeviceData[2] {};
template <int Kind> HRESULT STDMETHODCALLTYPE HeldDeviceState(IDirectInputDevice8W* device, DWORD size, LPVOID data)
{
    const HRESULT result = realDeviceState[Kind](device, size, data);
    if (menuOpen && SUCCEEDED(result) && data)
        std::memset(data, 0, size);
    return result;
}
template <int Kind>
HRESULT STDMETHODCALLTYPE HeldDeviceData(IDirectInputDevice8W* device, DWORD stride, LPDIDEVICEOBJECTDATA items,
                                         LPDWORD count, DWORD flags)
{
    const HRESULT result = realDeviceData[Kind](device, stride, items, count, flags);
    if (!menuOpen || FAILED(result) || !items || !count || (flags & DIGDD_PEEK) || stride < sizeof(DIDEVICEOBJECTDATA))
        return result;
    DIDEVCAPS caps { sizeof(caps) };
    const bool mouse =
        SUCCEEDED(device->GetCapabilities(&caps)) && GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_MOUSE;
    auto at = [&](DWORD i)
    { return reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<BYTE*>(items) + i * stride); };
    DWORD kept = 0;
    for (DWORD i = 0; i < *count; ++i)
    {
        const DIDEVICEOBJECTDATA* item = at(i);
        const bool button = !mouse || (item->dwOfs >= DIMOFS_BUTTON0 && item->dwOfs <= DIMOFS_BUTTON7);
        if (button && !(item->dwData & 0x80))
            std::memmove(at(kept++), item, stride);
    }
    *count = kept;
    return result;
}
template <int Kind> void HookDirectInput(REFIID api)
{
    using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const HMODULE dinput = GetModuleHandleW(L"dinput8.dll");
    const auto create = dinput ? reinterpret_cast<Create>(GetProcAddress(dinput, "DirectInput8Create")) : nullptr;
    ComPtr<IDirectInput8W> input;
    ComPtr<IDirectInputDevice8W> device;
    if (realDeviceState[Kind] || !create ||
        FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, api,
                      reinterpret_cast<void**>(input.GetAddressOf()), nullptr)) ||
        FAILED(input->CreateDevice(GUID_SysMouse, &device, nullptr)))
        return;
    auto table = *reinterpret_cast<void***>(device.Get());
    Replace(&table[9], reinterpret_cast<void*>(HeldDeviceState<Kind>),
            reinterpret_cast<void**>(&realDeviceState[Kind]));
    Replace(&table[10], reinterpret_cast<void*>(HeldDeviceData<Kind>), reinterpret_cast<void**>(&realDeviceData[Kind]));
    Log("DirectInput %s devices hooked: the menu holds them", Kind ? "wide" : "ANSI");
}

// Every module loaded so far: run at each host start and each time the menu opens, so modules the
// game loads later are covered too.
void PatchInputCalls()
{
    HMODULE modules[1024] {};
    DWORD bytes = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i)
            if (modules[i] != self)
                PatchImports(modules[i]);
    HookDirectInput<0>(IID_IDirectInput8A);
    HookDirectInput<1>(IID_IDirectInput8W);
}

// OptiScaler's menu opened or closed in the host. While it is open the cursor the game reads stays
// where it was and the game's cursor clip is lifted; on closing, the clip the game had, or asked for
// meanwhile, is put back.
void MenuChanged(bool open)
{
    if (open == menuOpen)
        return;
    if (open)
    {
        PatchInputCalls();
        realGetCursorPos(&heldCursor);
    }
    std::lock_guard guard(clipLock);
    if (open)
    {
        const RECT screen = VirtualScreen();
        clipNone = !realGetClipCursor(&gameClip) || EqualRect(&gameClip, &screen);
    }
    menuOpen = open;
    realClipCursor(open || clipNone ? nullptr : &gameClip);
}

// The message hook on the threads of the game's device and focus windows (once per thread), and the
// cursor calls.
void HookGame(HWND window, HWND focus)
{
    static std::vector<DWORD> hooked;
    if (!surfaceRequest)
        surfaceRequest = RegisterWindowMessageW(SurfaceName);
    if (!restyleRequest)
        restyleRequest = RegisterWindowMessageW((std::wstring(SurfaceName) + L"Restyle").c_str());
    gameRoot = GetAncestor(window, GA_ROOT);
    focusRoot = GetAncestor(focus, GA_ROOT);
    for (const HWND target : { window, focus })
    {
        const DWORD thread = GetWindowThreadProcessId(target, nullptr);
        if (!thread || std::find(hooked.begin(), hooked.end(), thread) != hooked.end())
            continue;
        if (SetWindowsHookExW(WH_GETMESSAGE, MessageHook, self, thread))
        {
            hooked.push_back(thread);
            Log("messages of thread %lu hooked: the menu holds the game's input there", thread);
        }
        else
            Log("cannot hook the messages of thread %lu (%lu)", thread, GetLastError());
    }
    PatchInputCalls();
}

// The fullscreen the game asked for, or windowed again: the window's thread restyles it (RestyleNow). borderlessFor is
// the window last asked to be borderless, so a game that asks every frame posts once.
HWND borderlessFor = nullptr;
void Restyle(HWND window, HWND focus, bool borderless)
{
    if (!window || borderless == (borderlessFor == window))
        return;
    HookGame(window, focus ? focus : window);
    borderlessFor = borderless ? window : nullptr;
    PostMessageW(window, restyleRequest, borderless, 0);
}

struct Host
{
    Handle pipe, process, job;
    // An ended host and its job, until it has left: the next host waits for it (Retire).
    Handle retired, retiredJob;

    // Closing the pipe asks the host to release its swap chain and leave.
    void Stop()
    {
        pipe.reset();
        if (process)
        {
            retired.reset(process.value);
            retiredJob.reset(job.value);
            process.value = job.value = nullptr;
        }
    }
    // A host that has not left within five seconds is ended.
    void Retire()
    {
        if (retired && WaitForSingleObject(retired.value, 5000) == WAIT_TIMEOUT)
            TerminateProcess(retired.value, 7);
        retired.reset();
        retiredJob.reset();
    }

    bool Start(const LUID& luid, HWND window)
    {
        const auto exe = hostFolder / L"OptiScalerDx9Host.exe";
        LARGE_INTEGER ticks {};
        QueryPerformanceCounter(&ticks);
        const auto name =
            std::wstring(PipePrefix) + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(ticks.QuadPart);
        pipe.reset(CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr));
        Handle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!pipe || !connected)
            return false;
        OVERLAPPED overlap {};
        overlap.hEvent = connected.value;
        const bool pending = !ConnectNamedPipe(pipe.value, &overlap) && GetLastError() == ERROR_IO_PENDING;
        auto cancel = [&]
        {
            if (pending)
            {
                DWORD n = 0;
                CancelIoEx(pipe.value, &overlap);
                GetOverlappedResult(pipe.value, &overlap, &n, TRUE);
            }
            return false;
        };
        // The host dies with the game: the job closes with this process.
        job.reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return cancel();
        auto command = L"\"" + exe.wstring() + L"\" \"" + name + L"\" " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup { sizeof(startup) };
        PROCESS_INFORMATION info {};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                            nullptr, hostFolder.c_str(), &startup, &info))
        {
            Log("cannot start %ls (%lu)", exe.c_str(), GetLastError());
            return cancel();
        }
        process.reset(info.hProcess);
        Handle thread(info.hThread);
        if (!AssignProcessToJobObject(job.value, process.value) || ResumeThread(thread.value) == DWORD(-1))
        {
            TerminateProcess(process.value, 1);
            return cancel();
        }
        if (pending)
        {
            HANDLE waits[] { connected.value, process.value };
            DWORD n = 0;
            if (WaitForMultipleObjects(2, waits, FALSE, StartupTimeoutMs) != WAIT_OBJECT_0 ||
                !GetOverlappedResult(pipe.value, &overlap, &n, FALSE))
                return cancel();
        }
        ULONG client = 0;
        if (!GetNamedPipeClientProcessId(pipe.value, &client) || client != info.dwProcessId)
            return false;
        dx9wire::Hello hello;
        hello.pid = GetCurrentProcessId();
        hello.window = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window));
        hello.luidLow = luid.LowPart;
        hello.luidHigh = luid.HighPart;
        Ack ack;
        return Request(pipe.value, process.value, Kind::Hello, &hello, sizeof(hello), ack, StartupTimeoutMs) &&
               ack.result == Result::Ready;
    }
};

template <class Api> struct Bridge
{
    using Device = typename Api::Device;
    using Back = typename Api::Back;
    Host host;
    typename Api::Stage stage;
    Device* device = nullptr;
    HWND window = nullptr;
    // The interval each device asked for, which the host presents at (a forced window may have made it
    // one in the game's API).
    std::vector<std::pair<Device*, UINT>> intervals;
    uint64_t generation = 0, frame = 0;
    bool built = false, disabled = false, pipelined = true;
    // A frame posted to the host whose answer is collected at the next Present (or before anything
    // else uses the pipe), so the game renders its next frame while the host presents this one.
    uint64_t pending = 0;
    // The host starts on its own thread while the game presents natively, then takes the window.
    enum class Link
    {
        Down,
        Starting,
        Up
    } link = Link::Down;
    Handle starter;
    LUID startLuid {};
    HWND focus = nullptr;
    // This host's child window, and when one was last asked of the game window's thread.
    HWND surface = nullptr;
    bool surfaceAsked = false;
    ULONGLONG surfaceAskedAt = 0;
    // When the game last presented to the host's window, whether its device was lost then, and when a
    // host was last replaced for a new window.
    ULONGLONG windowPresentedAt = 0, restartedAt = 0;
    bool lost = false;
    // Failures in a row. A host that presented 600 frames before it failed starts the count again, so
    // only a host that keeps failing early leaves the session native.
    uint32_t strikes = 0;
    uint64_t sinceStart = 0;
    ULONGLONG retryAt = 0;

    // Ends the link: closing the pipe asks the host to leave, and its child window is destroyed.
    void Down()
    {
        host.Stop();
        takenSurface = nullptr;
        if (surface && IsWindow(surface))
            PostMessageW(GetAncestor(surface, GA_PARENT), surfaceRequest, 0, LPARAM(surface));
        surface = nullptr;
        surfaceAsked = false;
        link = Link::Down;
        built = false;
        pending = 0;
        MenuChanged(false);
    }

    void Fault(const char* why, bool retryNow = false)
    {
        Log("host lost (%s); this frame is presented natively", why);
        Down();
        ++status.faults;
        strikes = sinceStart >= 600 ? 1 : strikes + 1;
        sinceStart = 0;
        if (strikes > 3)
        {
            disabled = true;
            status.disabled = 1;
            Log("host failed %u times in a row; the game presents natively for the rest of the session", strikes);
        }
        retryAt = GetTickCount64() + (retryNow ? 0 : 1000ull * strikes);
    }

    // The game presents to a new window: a new host for it, straight away. Not a failure, unless the
    // game moved within 10 s of the last move, so a game that keeps moving between windows cannot start
    // hosts without end.
    void Restart(const char* why)
    {
        const ULONGLONG tick = GetTickCount64();
        const bool again = restartedAt && tick - restartedAt < 10000;
        restartedAt = tick;
        if (again)
        {
            Fault(why);
            return;
        }
        Log("host %u ended: %s; a new host starts", status.hostStarts, why);
        Down();
        retryAt = 0;
    }

    void SetInterval(Device* game, UINT interval)
    {
        const auto known = std::find_if(intervals.begin(), intervals.end(), [&](auto& i) { return i.first == game; });
        if (known != intervals.end())
            known->second = interval;
        else
            intervals.push_back({ game, interval });
    }
    UINT Interval(Device* game)
    {
        const auto known = std::find_if(intervals.begin(), intervals.end(), [&](auto& i) { return i.first == game; });
        return known != intervals.end() ? known->second : 1;
    }

    void Drop()
    {
        Answer();
        if (built)
        {
            Ack ack;
            if (!Request(host.pipe.value, host.process.value, Kind::Drop, nullptr, 0, ack))
                Fault("no answer to a drop");
        }
        built = false;
        stage.Release();
    }

    bool Connect(Device* game)
    {
        LUID luid {};
        if (!(window = Api::Window(game, focus)) || !Api::Luid(game, luid))
            return false;
        windowPresentedAt = GetTickCount64();
        HookGame(window, focus);
        startLuid = luid;
        ++status.hostStarts;
        starter.reset(CreateThread(
            nullptr, 0,
            [](LPVOID context) -> DWORD
            {
                auto owner = static_cast<Bridge*>(context);
                owner->host.Retire();
                return owner->host.Start(owner->startLuid, owner->window) ? 1 : 0;
            },
            this, 0, nullptr));
        return static_cast<bool>(starter);
    }

    // Up once the host answered its Hello; until then the game presents natively.
    bool Started()
    {
        if (link == Link::Starting && WaitForSingleObject(starter.value, 0) == WAIT_OBJECT_0)
        {
            DWORD started = 0;
            GetExitCodeThread(starter.value, &started);
            starter.reset();
            if (!started)
            {
                Fault("start");
                return false;
            }
            link = Link::Up;
            sinceStart = 0;
            pipelined = Setting(L"Pipelined", true);
            Log("host %u connected for window %p", status.hostStarts, window);
        }
        return link == Link::Up;
    }

    // The child window comes from the game window's thread; until it has made one, the game presents
    // natively. Asked again every 3 s while none has come.
    bool Surface()
    {
        if (!surface)
        {
            const HWND made = madeSurface.exchange(nullptr);
            if (made && GetAncestor(made, GA_PARENT) == window)
                takenSurface = surface = made;
            else if (made && IsWindow(made))
                PostMessageW(GetAncestor(made, GA_PARENT), surfaceRequest, 0, LPARAM(made));
        }
        if (!surface)
        {
            const ULONGLONG now = GetTickCount64();
            if (surfaceAsked && now - surfaceAskedAt < 3000)
                return false;
            if (surfaceAsked)
                Log("no child window from the game window's thread after 3 s; asking again");
            surfaceAsked = PostMessageW(window, surfaceRequest, 1, 0) != FALSE;
            surfaceAskedAt = now;
            return false;
        }
        surfaceAsked = false;
        // The child follows the game window's client area (not while it is minimized).
        RECT client {}, child {};
        if (!IsIconic(window) && GetClientRect(window, &client) && GetClientRect(surface, &child) &&
            (client.right != child.right || client.bottom != child.bottom))
            SetWindowPos(surface, nullptr, 0, 0, client.right, client.bottom,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_ASYNCWINDOWPOS);
        return true;
    }

    // format is the back buffer's, in the game's API.
    bool Ensure(Device* game, UINT width, UINT height, uint32_t format)
    {
        if (device != game)
        {
            Drop();
            if (link != Link::Up)
                return false;
            device = game;
        }
        if (built && stage.width == width && stage.height == height && stage.format == format)
            return true;
        Drop();
        if (link != Link::Up)
            return false;
        if (stage.width != width || stage.height != height || stage.format != format || !stage.Made())
        {
            stage.Release();
            if (!stage.Make(game, width, height, format, Setting(L"SharedMemory", false)))
                return false;
            stage.width = width;
            stage.height = height;
            stage.format = format;
        }
        dx9wire::Build request;
        request.generation = ++generation;
        request.window = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(surface));
        request.width = stage.width;
        request.height = stage.height;
        request.format = stage.wire;
        request.pitch = stage.width * 4;
        if (stage.shared)
        {
            request.transport = Transport::SharedTexture;
            request.handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stage.shared));
        }
        else
        {
            HANDLE remote = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), stage.section.value, host.process.value, &remote, 0, FALSE,
                                 DUPLICATE_SAME_ACCESS))
                return false;
            request.transport = Transport::SharedMemory;
            request.handle = reinterpret_cast<uintptr_t>(remote);
        }
        status.transport = static_cast<uint32_t>(request.transport);
        status.width = stage.width;
        status.height = stage.height;
        Ack ack;
        // The host makes its swap chain here, and OptiScaler with it the frame generation swap chain.
        if (!Request(host.pipe.value, host.process.value, Kind::Build, &request, sizeof(request), ack, AnswerTimeoutMs))
        {
            Fault("no answer to a build");
            return false;
        }
        if (ack.result == Result::Unhooked)
        {
            ++status.unhooked;
            Fault("OptiScaler did not hook the host's swap chain");
            return false;
        }
        built = ack.result == Result::Ready && ack.generation == generation;
        Log("stage %llu: %ux%u %s format %u through %s: %s", generation, stage.width, stage.height, Api::Name,
            static_cast<unsigned>(format), stage.shared ? "a shared texture" : "shared memory",
            built ? "ready" : "refused");
        return built;
    }

    // True when the host presented this frame and the game's Present must be skipped.
    bool Present(Device* game, Back* back, const RECT* source, const RECT* dest)
    {
        if (disabled || !back)
            return false;
        UINT width = 0, height = 0;
        uint32_t format = 0;
        HWND focusNow = nullptr;
        const HWND now = Api::Window(game, focusNow);
        if (!now || !Api::Describe(back, width, height, format))
            return false;
        // The host serves one window. Another device of the game presenting to another window (a
        // launcher, a video or a tool view) is presented natively while the host's window is still
        // shown and presented to; a game that has moved to a new window gets a new host there (below).
        const ULONGLONG tick = GetTickCount64();
        if (window && now != window && game != device && IsWindow(window) && IsWindowVisible(window) &&
            tick - windowPresentedAt < 1000)
            return false;
        if (now == window)
            windowPresentedAt = tick;
        // A provisional buffer (The Sims 4 shows 16x16 first). Once the host owns the window its last
        // frame stays up rather than the game's API presenting under it.
        if (width < 64 || height < 64)
            return link == Link::Up;
        if (link == Link::Down)
        {
            if (tick < retryAt)
                return false;
            if (!Connect(game))
            {
                Fault("start");
                return false;
            }
            link = Link::Starting;
        }
        if (!Started() || !Answer())
            return false;
        // A game that releases its window and device and makes new ones: the host's swap chain is on a
        // child of the old window, so the new window gets a new host.
        if (now != window)
        {
            Restart("the game presents to a new window");
            return false;
        }
        if (surface && !IsWindow(surface))
        {
            Restart("its child window is gone");
            return false;
        }
        // A lost plain D3D9 device (a lock screen, a display mode change) cannot hand its frame over. It
        // presents natively, which gives the game the D3DERR_DEVICELOST it resets on, and the host is
        // kept.
        if (Api::Lost(game))
        {
            if (!lost)
                Log("the game's device is lost; presenting natively until it is reset");
            lost = true;
            return false;
        }
        lost = false;
        if (!Surface())
            return false;
        if (!Ensure(game, width, height, format))
        {
            // A host that cannot resize its swap chain (XeFG refuses while anything holds a back
            // buffer) is replaced by a new one at the new size straight away.
            if (link == Link::Up)
                Fault("the host could not take the new back buffer", true);
            return false;
        }
        if (!stage.Capture(game, back, source, dest, window))
        {
            Fault("the frame could not be handed over");
            return false;
        }
        dx9wire::Frame request;
        request.generation = generation;
        request.id = ++frame;
        request.syncInterval = Interval(game);
        {
            std::lock_guard guard(typedLock);
            request.textCount = static_cast<uint32_t>(std::min<size_t>(typed.size(), std::size(request.text)));
            std::copy_n(typed.begin(), request.textCount, request.text);
            typed.erase(0, request.textCount);
        }
        status.typed += request.textCount;
        if (!Post(host.pipe.value, host.process.value, Kind::Frame, &request, sizeof(request)))
        {
            Fault("the host stopped reading");
            return false;
        }
        pending = frame;
        return pipelined || Answer();
    }

    // Collects the answer to the frame in flight; false when the host failed it.
    bool Answer()
    {
        if (!pending)
            return true;
        const uint64_t id = pending;
        pending = 0;
        Ack ack;
        if (!Collect(host.pipe.value, host.process.value, Kind::Frame, ack, AnswerTimeoutMs))
        {
            Fault("no answer to a frame");
            return false;
        }
        if (ack.result == Result::DeviceLost)
        {
            Log("host reports its device lost (reason %08X)", ack.error);
            Fault("device lost");
            return false;
        }
        if (ack.result != Result::Presented || ack.frame != id)
        {
            Fault("the host could not present");
            return false;
        }
        status.state = ack.state;
        MenuChanged((ack.state & StateMenu) != 0);
        status.menuFrames += menuOpen ? 1 : 0;
        ++sinceStart;
        if (++status.bridged == 1)
            Log("first frame presented by the host");
        return true;
    }
};

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        self = module;
        DisableThreadLibraryCalls(module);
        wchar_t path[MAX_PATH] {};
        GetModuleFileNameW(module, path, MAX_PATH);
        hostFolder = std::filesystem::path(path).parent_path() / L"OptiScaler64";
    }
    return TRUE;
}

extern "C" void WINAPI _OptiScalerX86Status(ProxyStatus* out)
{
    std::lock_guard guard(lock);
    if (out)
        *out = status;
}
