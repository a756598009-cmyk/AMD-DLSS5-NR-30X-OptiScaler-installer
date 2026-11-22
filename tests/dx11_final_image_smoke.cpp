// A D3D11 game stand-in for final-image NR and the D3D11->D3D12 XeFG bridge.
//
// Loads OptiScaler as dxgi.dll from its own folder (beside it: OptiScaler.pdb, the danielblnc pass
// runtime, its weights and the OptiScaler\ folder of a release), renders a panning scene with depth,
// and drives OptiScaler the way a player does: menu, NR and FG keys with real key input. Settings
// without a key (NR scale, passes, menu section) are written into OptiScaler's own Config through
// the PDB, the same fields the menu writes. Checks read OptiScaler.log, amd_presr.log, the DXGI
// present count, the RivaTuner Statistics Server shared memory when RTSS runs, and the screen (Desktop Duplication
// counts what the compositor shows).
//
//   dx11_final_image_smoke.exe fg|nr|keys|focus|fgoff|resize|recreate|menu|pace|artefacts|bars
//   [seconds per phase] [log level] [Section/Key=Value ...] [sync=1] [bgra=1] [tearing=1] [full=1] [blt=1] [srgb=1]
//   [r10=1]
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dbghelp.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "present_look.h"
#include "screen_look.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static fs::path dir;
static int failures = 0;
static std::atomic<unsigned> progress { 0 };
static std::atomic<const char*> phaseName { "start" };
static UINT syncInterval = 0; // sync=1 on the command line presents with vsync
static bool bgra = false;     // bgra=1: a B8G8R8A8 back buffer, as the 32-bit D3D9 host has
static bool tearing = false; // tearing=1: a swapchain that allows tearing, presented with it at interval 0, as the host
static bool full = false;    // full=1: a borderless window over the whole primary screen, as a game in fullscreen
static bool blt = false;     // blt=1: one back buffer and the bit-block transfer model, as older D3D11 games
static bool srgb = false;    // srgb=1: an R8G8B8A8_UNORM_SRGB back buffer, resized with that format again, as Unity does
static bool r10 = false;     // r10=1: an R10G10B10A2 back buffer in the default sRGB colour space, as Unreal games have

static void check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char text[160];
        std::snprintf(text, sizeof(text), "%s failed: 0x%08X", what, (unsigned) hr);
        throw std::runtime_error(text);
    }
}
static void expect(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

// ---- OptiScaler.ini ---------------------------------------------------------------------------
static void SetIni(std::vector<std::string>& lines, const std::string& section, const std::string& key,
                   const std::string& value)
{
    bool inside = false;
    size_t header = lines.size();
    for (size_t i = 0; i < lines.size(); ++i)
    {
        auto& line = lines[i];
        if (!line.empty() && line[0] == '[')
        {
            inside = line == "[" + section + "]";
            if (inside)
                header = i;
        }
        else if (inside && line.rfind(key + "=", 0) == 0)
        {
            line = key + "=" + value;
            return;
        }
    }
    if (header == lines.size())
        throw std::runtime_error("INI section not found: " + section);
    lines.insert(lines.begin() + header + 1, key + "=" + value);
}
static void WriteIni(bool fg, const wchar_t* logLevel, const std::vector<std::wstring>& extra)
{
    std::ifstream in(dir / "OptiScaler.template.ini");
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);)
        lines.push_back(line.size() && line.back() == '\r' ? line.substr(0, line.size() - 1) : line);
    if (lines.empty())
        throw std::runtime_error("OptiScaler.template.ini missing");
    SetIni(lines, "DlssNr", "Enabled", "true");
    SetIni(lines, "DlssNr", "PresentWithoutUpscaler", "true");
    SetIni(lines, "DlssNr", "NrBackend", "daniel");
    SetIni(lines, "FrameGen", "Enabled", fg ? "true" : "false");
    SetIni(lines, "FrameGen", "FGOutput", fg ? "xefg" : "nofg");
    SetIni(lines, "XeFG", "InterpolationCount", "2");
    SetIni(lines, "Log", "LogToFile", "true");
    SetIni(lines, "Log", "LogLevel", logLevel ? std::string(logLevel, logLevel + wcslen(logLevel)) : "2");
    SetIni(lines, "Menu", "ShowFps", "true");
    SetIni(lines, "Menu", "DisableSplash", "true");
    SetIni(lines, "Hotfix", "CheckForUpdate", "false");
    for (const auto& setting : extra) // Section/Key=Value
    {
        const std::string text(setting.begin(), setting.end());
        const size_t slash = text.find('/'), equals = text.find('=');
        if (slash != std::string::npos && equals > slash)
            SetIni(lines, text.substr(0, slash), text.substr(slash + 1, equals - slash - 1), text.substr(equals + 1));
    }
    std::ofstream out(dir / "OptiScaler.ini", std::ios::trunc);
    for (auto& line : lines)
        out << line << "\n";
}

// ---- OptiScaler's settings through its PDB ----------------------------------------------------
struct Opti
{
    HANDLE process = GetCurrentProcess();
    DWORD64 base = 0;
    BYTE* config = nullptr;
    ULONG configType = 0;
    bool* menuVisible = nullptr;
    int* menuSection = nullptr;
    // The NR status the menu shows and the mutex it is read under (same toolset and CRT as OptiScaler).
    const std::string* nrStatus = nullptr;
    std::mutex* nrStatusMutex = nullptr;

    void Load(HMODULE module)
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        check(SymInitializeW(process, dir.wstring().c_str(), FALSE) ? S_OK : E_FAIL, "SymInitialize");
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(module, path, MAX_PATH);
        base = SymLoadModuleExW(process, nullptr, path, nullptr, (DWORD64) module, 0, nullptr, 0);
        if (!base)
            throw std::runtime_error("OptiScaler symbols not loaded (OptiScaler.pdb beside dxgi.dll?)");
        BYTE* configPointer = (BYTE*) Address(L"Config::_config");
        config = *(BYTE**) configPointer;
        menuVisible = (bool*) Address(L"MenuCommon::_isVisible");
        menuSection = (int*) Address(L"selectedTab");
        nrStatus = (const std::string*) Address(L"AmdPresentExperimental::shown");
        nrStatusMutex = (std::mutex*) Address(L"AmdPresentExperimental::shownMutex");
        SYMBOL_INFOW type {};
        type.SizeOfStruct = sizeof(type);
        check(SymGetTypeFromNameW(process, base, L"Config", &type) ? S_OK : E_FAIL, "Config type");
        configType = type.TypeIndex;
        if (!config)
            throw std::runtime_error("Config::_config is null");
    }
    void* Address(const wchar_t* name)
    {
        std::vector<BYTE> storage(sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t));
        auto symbol = (SYMBOL_INFOW*) storage.data();
        symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
        symbol->MaxNameLen = MAX_SYM_NAME;
        if (!SymFromNameW(process, name, symbol))
            throw std::runtime_error("symbol not found");
        return (void*) symbol->Address;
    }
    DWORD Offset(ULONG typeIndex, const wchar_t* name)
    {
        DWORD count = 0;
        SymGetTypeInfo(process, base, typeIndex, TI_GET_CHILDRENCOUNT, &count);
        std::vector<BYTE> storage(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
        auto children = (TI_FINDCHILDREN_PARAMS*) storage.data();
        children->Count = count;
        SymGetTypeInfo(process, base, typeIndex, TI_FINDCHILDREN, children);
        for (DWORD i = 0; i < count; ++i)
        {
            WCHAR* childName = nullptr;
            if (!SymGetTypeInfo(process, base, children->ChildId[i], TI_GET_SYMNAME, &childName))
                continue;
            const bool match = wcscmp(childName, name) == 0;
            LocalFree(childName);
            DWORD offset = 0;
            if (match && SymGetTypeInfo(process, base, children->ChildId[i], TI_GET_OFFSET, &offset))
                return offset;
        }
        throw std::runtime_error("field not found");
    }
    BYTE* Field(const wchar_t* name) { return config + Offset(configType, name); }
    // A member of an OptiScaler object, by its class and member name.
    template <class T> T Member(const void* object, const wchar_t* typeName, const wchar_t* name)
    {
        SYMBOL_INFOW type {};
        type.SizeOfStruct = sizeof(type);
        check(SymGetTypeFromNameW(process, base, typeName, &type) ? S_OK : E_FAIL, "type");
        T value {};
        std::memcpy(&value, (const BYTE*) object + Offset(type.TypeIndex, name), sizeof(T));
        return value;
    }
    // CustomOptional<T> starts with its std::optional<T>: the value, then the engaged flag.
    template <class T> void Set(const wchar_t* name, T value)
    {
        BYTE* field = Field(name);
        std::memcpy(field, &value, sizeof(T));
        field[sizeof(T)] = 1;
    }
    std::string Status() const
    {
        std::lock_guard lock(*nrStatusMutex);
        return *nrStatus;
    }
    // Frames the NR runtime completed, from the status line the Neural section shows.
    unsigned NrFrames() const
    {
        static unsigned last = 0;
        const std::string text = Status();
        const size_t at = text.find("completed frames=");
        if (at != std::string::npos)
            last = (unsigned) std::strtoul(text.c_str() + at + 17, nullptr, 10);
        return last;
    }
    template <class T> T Get(const wchar_t* name)
    {
        T value {};
        std::memcpy(&value, Field(name), sizeof(T));
        return value;
    }
} opti;

static void PrintFrame(DWORD64 address)
{
    std::vector<BYTE> storage(sizeof(SYMBOL_INFO) + 256);
    auto symbol = (SYMBOL_INFO*) storage.data();
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    HMODULE module = nullptr;
    char moduleName[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR) address, &module);
    if (module)
        GetModuleFileNameA(module, moduleName, MAX_PATH);
    const bool named = SymFromAddr(GetCurrentProcess(), address, &displacement, symbol);
    if (!named)
        displacement = address - (DWORD64) module; // module offset when there are no symbols
    std::printf("    %s!%s+0x%llx\n", fs::path(moduleName).filename().string().c_str(), named ? symbol->Name : "",
                (unsigned long long) displacement);
}
// Where a thread is stuck.
static void PrintStack(HANDLE thread)
{
    SuspendThread(thread);
    CONTEXT context {};
    context.ContextFlags = CONTEXT_FULL;
    GetThreadContext(thread, &context);
    STACKFRAME64 frame {};
    frame.AddrPC = { context.Rip, 0, AddrModeFlat };
    frame.AddrFrame = { context.Rbp, 0, AddrModeFlat };
    frame.AddrStack = { context.Rsp, 0, AddrModeFlat };
    for (int i = 0; i < 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), thread, &frame, &context,
                                            nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
         ++i)
        PrintFrame(frame.AddrPC.Offset);
    // Hooks leave frames without unwind data; code addresses found on the stack fill the gap.
    std::printf("  return addresses on the stack:\n");
    MEMORY_BASIC_INFORMATION stackInfo {};
    VirtualQuery((void*) context.Rsp, &stackInfo, sizeof(stackInfo));
    const auto top = (const DWORD64*) ((BYTE*) stackInfo.BaseAddress + stackInfo.RegionSize);
    int shown = 0;
    for (auto p = (const DWORD64*) context.Rsp; p < top && shown < 60; ++p)
    {
        HMODULE module = nullptr;
        if (*p > 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               (LPCSTR) *p, &module))
        {
            MEMORY_BASIC_INFORMATION code {};
            if (VirtualQuery((void*) *p, &code, sizeof(code)) && (code.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE)))
            {
                PrintFrame(*p);
                ++shown;
            }
        }
    }
}

// A crash prints where it happened before the process goes.
static LONG CALLBACK Crash(EXCEPTION_POINTERS* info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    static std::atomic<int> printed { 0 };
    if ((code != 0xC0000005 && code != 0xC0000374 && code != 0xC0000409 && code != 0xC000001D) || printed++ > 2)
        return EXCEPTION_CONTINUE_SEARCH;
    std::printf("exception 0x%08X at %p during \"%s\"\n", (unsigned) code, info->ExceptionRecord->ExceptionAddress,
                phaseName.load());
    PrintFrame((DWORD64) info->ExceptionRecord->ExceptionAddress);
    void* frames[48];
    const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    std::vector<BYTE> storage(sizeof(SYMBOL_INFO) + 256);
    auto symbol = (SYMBOL_INFO*) storage.data();
    for (USHORT i = 0; i < count; ++i)
    {
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        HMODULE module = nullptr;
        char moduleName[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR) frames[i], &module);
        if (module)
            GetModuleFileNameA(module, moduleName, MAX_PATH);
        const bool named = SymFromAddr(GetCurrentProcess(), (DWORD64) frames[i], &displacement, symbol);
        if (!named)
            displacement = (DWORD64) frames[i] - (DWORD64) module;
        std::printf("  %2u %s!%s+0x%llx\n", i, fs::path(moduleName).filename().string().c_str(),
                    named ? symbol->Name : "", (unsigned long long) displacement);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- Logs -------------------------------------------------------------------------------------
static std::string ReadShared(const fs::path& path)
{
    HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size {};
    GetFileSizeEx(file, &size);
    std::string text((size_t) size.QuadPart, '\0');
    DWORD read = 0;
    ReadFile(file, text.data(), (DWORD) text.size(), &read, nullptr);
    CloseHandle(file);
    text.resize(read);
    return text;
}
static size_t Count(const std::string& text, const char* token, size_t from = 0)
{
    size_t n = 0;
    for (size_t at = text.find(token, from); at != std::string::npos; at = text.find(token, at + 1))
        ++n;
    return n;
}
struct Logs
{
    size_t opti = 0, amd = 0;
    void Mark()
    {
        opti = ReadShared(dir / "OptiScaler.log").size();
        amd = ReadShared(dir / "amd_presr.log").size();
    }
    size_t Opti(const char* token) const { return Count(ReadShared(dir / "OptiScaler.log"), token, opti); }
    size_t Amd(const char* token) const { return Count(ReadShared(dir / "amd_presr.log"), token, amd); }
};

// ---- RivaTuner Statistics Server --------------------------------------------------------------
// Framerate RTSS reports for this process (0 when RTSS is not running or has not seen it).
static double RtssFramerate()
{
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, L"RTSSSharedMemoryV2");
    if (!map)
        return 0;
    double fps = 0;
    if (auto memory = (const BYTE*) MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0))
    {
        const DWORD* header = (const DWORD*) memory;
        if (header[0] == 'RTSS' && header[1] >= 0x00020000)
        {
            const DWORD entrySize = header[2], arrayOffset = header[3], arraySize = header[4];
            for (DWORD i = 0; i < arraySize; ++i)
            {
                const BYTE* entry = memory + arrayOffset + i * entrySize;
                if (*(const DWORD*) entry != GetCurrentProcessId())
                    continue;
                const DWORD* timing = (const DWORD*) (entry + 4 + MAX_PATH + 4);
                if (timing[1] > timing[0])
                    fps = 1000.0 * timing[2] / (timing[1] - timing[0]);
            }
        }
        UnmapViewOfFile(memory);
    }
    CloseHandle(map);
    return fps;
}

// ---- Window, input, screen --------------------------------------------------------------------
static HWND window = nullptr, decoy = nullptr;
static UINT width = 1280, height = 720;
static bool resized = false;

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_SIZE && hwnd == window && LOWORD(l) && HIWORD(l))
    {
        width = LOWORD(l);
        height = HIWORD(l);
        resized = true;
    }
    // Like most games: no window menu loop on ALT (the foreground helper taps ALT).
    if (message == WM_CLOSE || (message == WM_SYSCOMMAND && (w & 0xFFF0) == SC_KEYMENU))
        return 0;
    return DefWindowProcW(hwnd, message, w, l);
}
static void Pump()
{
    for (MSG msg; PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
// OptiScaler filters this process's SendInput and PostMessage while its menu is open, as a game's own synthetic input;
// a keyboard does not go through them, so neither do these keys.
static UINT Send(UINT count, INPUT* inputs)
{
    using SendInputFn = UINT(WINAPI*)(UINT, LPINPUT, int);
    static auto send = (SendInputFn) GetProcAddress(LoadLibraryW(L"win32u.dll"), "NtUserSendInput");
    return send ? send(count, inputs, sizeof(INPUT)) : SendInput(count, inputs, sizeof(INPUT));
}
static void Post(UINT message, WPARAM w, LPARAM l)
{
    using PostFn = BOOL(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
    static auto post = (PostFn) GetProcAddress(LoadLibraryW(L"win32u.dll"), "NtUserPostMessage");
    (post ? post : PostMessageW)(window, message, w, l);
}
static void Foreground(HWND hwnd)
{
    if (GetForegroundWindow() == hwnd)
        return;
    // The ALT tap lifts the foreground lock for a process the user did not click.
    INPUT alt[2] {};
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    Send(2, alt);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetFocus(hwnd);
}
static void Key(WORD vk, bool up)
{
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = (WORD) MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) |
                       (vk == VK_INSERT || vk == VK_END ? KEYEVENTF_EXTENDEDKEY : 0);
    // While the menu is open, keys injected into the desktop do not reach this thread's key state on this machine
    // (the window keeps the foreground and the focus). The open menu gets the window message a keyboard produces.
    if (!*opti.menuVisible)
    {
        Send(1, &input);
        return;
    }
    const bool extended = vk == VK_INSERT || vk == VK_END;
    Post(up ? WM_KEYUP : WM_KEYDOWN, vk,
         1 | (LPARAM(input.ki.wScan) << 16) | (extended ? LPARAM(1) << 24 : 0) | (up ? LPARAM(3) << 30 : 0));
}
// A small grey thumbnail of what the screen shows inside the window right now.
static std::vector<BYTE> Screen()
{
    RECT rect;
    GetClientRect(window, &rect);
    POINT origin { 0, 0 };
    ClientToScreen(window, &origin);
    const int w = 96, h = 54;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    BITMAPINFO info {};
    info.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(memory, bitmap);
    SetStretchBltMode(memory, HALFTONE);
    StretchBlt(memory, 0, 0, w, h, screen, origin.x, origin.y, rect.right, rect.bottom, SRCCOPY);
    std::vector<BYTE> grey(w * h);
    for (int i = 0; i < w * h; ++i)
    {
        const BYTE* p = (const BYTE*) bits + i * 4;
        grey[i] = (BYTE) ((p[0] + 2 * p[1] + p[2]) / 4);
    }
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return grey;
}
static double Difference(const std::vector<BYTE>& a, const std::vector<BYTE>& b)
{
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i)
        sum += std::abs(int(a[i]) - int(b[i]));
    return sum / a.size();
}

// ---- The game -----------------------------------------------------------------------------------
static const char* kShader = R"(
cbuffer Frame : register(b0) { float2 pan; float time; float load; float2 size; float2 pad; };
float4 vs(uint id : SV_VertexID) : SV_Position
{ float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }
struct Out { float4 colour : SV_Target; float depth : SV_Depth; };
Out ps(float4 position : SV_Position)
{
    float2 uv = (position.xy + pan) / size.y;
    float2 cell = floor(uv * 8);
    float checker = fmod(abs(cell.x + cell.y), 2);
    float3 colour = lerp(float3(0.20, 0.35, 0.55), float3(0.85, 0.75, 0.55), checker);
    colour *= 0.75 + 0.25 * sin(uv.x * 3.1 + uv.y * 1.7);
    float depth = 0.9 - 0.1 * checker;
    [loop] for (int i = 0; i < 6; ++i)
    {
        float2 centre = size * (0.5 + 0.35 * float2(sin(time * (0.7 + 0.13 * i) + i), cos(time * (0.5 + 0.11 * i) + 2 * i)));
        float d = length(position.xy - centre) / (size.y * (0.05 + 0.015 * i));
        if (d < 1) { colour = lerp(float3(0.9, 0.3 + 0.1 * i, 0.2), float3(1, 1, 1), 1 - d) * (1 - 0.4 * d * d);
                     depth = 0.2 + 0.05 * i + 0.05 * d; }
    }
    float burn = 0;
    [loop] for (int k = 0; k < (int) load; ++k) burn += sin(burn + k * 0.001 + position.x);
    Out o; o.colour = float4(colour + burn * 1e-9, 1); o.depth = depth; return o;
}
)";

struct Game
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11DepthStencilState> depthState;
    float panX = 0, panY = 0, speed = 4, gpuLoad = 64, cpuMs = 2, clock = 1, tilt = 0;
    double time = 0;

    void Create()
    {
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1,
                                D3D11_SDK_VERSION, &device, nullptr, &context),
              "D3D11CreateDevice");
        Swapchain();
        Shaders();
    }
    void Swapchain()
    {
        ComPtr<IDXGIDevice> dxgi;
        check(device.As(&dxgi), "IDXGIDevice");
        ComPtr<IDXGIAdapter> adapter;
        check(dxgi->GetAdapter(&adapter), "GetAdapter");
        ComPtr<IDXGIFactory> factory;
        check(adapter->GetParent(IID_PPV_ARGS(&factory)), "GetParent");
        DXGI_SWAP_CHAIN_DESC desc {};
        desc.BufferDesc.Width = width;
        desc.BufferDesc.Height = height;
        desc.BufferDesc.Format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM
                                 : srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                 : r10  ? DXGI_FORMAT_R10G10B10A2_UNORM
                                        : DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = blt ? 1 : 2;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = blt ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = tearing && !blt ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        check(factory->CreateSwapChain(device.Get(), &desc, &swap), "CreateSwapChain");
    }
    // What a game does on a display mode or graphics settings change in many engines: drop the swapchain, make another.
    void Recreate()
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        target.Reset();
        depthView.Reset();
        context->ClearState();
        context->Flush();
        swap.Reset();
        Swapchain();
        Views();
    }
    void Shaders()
    {
        ComPtr<ID3DBlob> blob, errors;
        check(D3DCompile(kShader, strlen(kShader), "scene", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &blob, &errors), "vs");
        check(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs), "vs");
        check(D3DCompile(kShader, strlen(kShader), "scene", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &blob, &errors), "ps");
        check(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps), "ps");
        D3D11_BUFFER_DESC cb { 32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
        check(device->CreateBuffer(&cb, nullptr, &constants), "constants");
        D3D11_DEPTH_STENCIL_DESC ds {};
        ds.DepthEnable = TRUE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        check(device->CreateDepthStencilState(&ds, &depthState), "depth state");
        Views();
    }
    void Views()
    {
        target.Reset();
        depthView.Reset();
        ComPtr<ID3D11Texture2D> back;
        check(swap->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
        check(device->CreateRenderTargetView(back.Get(), nullptr, &target), "RTV");
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> depth;
        check(device->CreateTexture2D(&desc, nullptr, &depth), "depth");
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        check(device->CreateDepthStencilView(depth.Get(), &dsv, &depthView), "DSV");
    }
    void Resize()
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        target.Reset();
        depthView.Reset();
        context->Flush();
        check(swap->ResizeBuffers(0, width, height, srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_UNKNOWN,
                                  tearing && !blt ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),
              "ResizeBuffers");
        Views();
    }
    HRESULT Frame()
    {
        if (resized)
        {
            resized = false;
            Resize();
        }
        time += clock / 60.0;
        panX += speed;
        // tilt: the view also swings up and down, like a mouse look.
        panY += speed * 0.35f + tilt * (float) std::sin(time * 3.0);
        const float data[8] = { panX, panY, (float) time, gpuLoad, (float) width, (float) height, 0, 0 };
        context->UpdateSubresource(constants.Get(), 0, nullptr, data, 0, 0);
        D3D11_VIEWPORT viewport { 0, 0, (float) width, (float) height, 0, 1 };
        context->RSSetViewports(1, &viewport);
        ID3D11RenderTargetView* targets[] = { target.Get() };
        context->OMSetRenderTargets(1, targets, depthView.Get());
        context->OMSetDepthStencilState(depthState.Get(), 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer* buffers[] = { constants.Get() };
        context->PSSetConstantBuffers(0, 1, buffers);
        context->Draw(3, 0);
        // The game's own CPU work for the frame.
        LARGE_INTEGER f, start, now;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&start);
        do
            QueryPerformanceCounter(&now);
        while ((now.QuadPart - start.QuadPart) * 1000.0 / f.QuadPart < cpuMs);
        // The depth buffer stays bound through Present, as in most D3D11 games.
        return swap->Present(syncInterval, tearing && !blt && !syncInterval ? DXGI_PRESENT_ALLOW_TEARING : 0);
    }
} game;

// ---- Phases -----------------------------------------------------------------------------------
struct Stats
{
    double average = 0, median = 0, p99 = 0, worst = 0, presentsPerFrame = 0, rtss = 0, appFps = 0;
    unsigned frames = 0, nr = 0, slow = 0; // slow: frames over 2.5 times the median
};
static Stats Run(const char* name, double seconds, bool quiet = false)
{
    phaseName = name;
    LARGE_INTEGER f, start, last, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&start);
    last = start;
    UINT presents0 = 0, presents1 = 0;
    game.swap->GetLastPresentCount(&presents0);
    const unsigned nr0 = opti.NrFrames();
    std::vector<double> times;
    double rtss = 0;
    for (;;)
    {
        Pump();
        const HRESULT hr = game.Frame();
        if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED)
            check(hr, "Present");
        ++progress;
        QueryPerformanceCounter(&now);
        times.push_back((now.QuadPart - last.QuadPart) * 1000.0 / f.QuadPart);
        last = now;
        const double elapsed = (now.QuadPart - start.QuadPart) / double(f.QuadPart);
        if (elapsed > seconds * 0.6 && rtss == 0)
            rtss = RtssFramerate();
        if (elapsed >= seconds)
            break;
    }
    game.swap->GetLastPresentCount(&presents1);
    Stats s;
    s.frames = (unsigned) times.size();
    s.nr = opti.NrFrames() - nr0;
    std::vector<double> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    for (double t : times)
        s.average += t / times.size();
    s.median = sorted[sorted.size() / 2];
    s.p99 = sorted[(size_t) (sorted.size() * 0.99)];
    s.worst = sorted.back();
    s.slow = (unsigned) std::count_if(times.begin(), times.end(), [&](double t) { return t > s.median * 2.5; });
    s.presentsPerFrame = double(presents1 - presents0) / times.size();
    s.appFps = 1000.0 / s.average;
    s.rtss = rtss;
    if (!quiet)
        std::printf("  [%-22s] frames %5u  avg %6.2f ms  p99 %6.2f ms  max %7.2f ms  app %6.1f fps  "
                    "presents/frame %4.2f  rtss %6.1f fps  NR %u  slow %u\n",
                    name, s.frames, s.average, s.p99, s.worst, s.appFps, s.presentsPerFrame, s.rtss, s.nr, s.slow);
    return s;
}
static void Press(WORD vk)
{
    Foreground(window);
    Key(vk, false);
    Run("key down", 0.15, true);
    Key(vk, true);
    Run("key up", 0.25, true);
}
// ---- Present cadence ----------------------------------------------------------------------------
// At debug level OptiScaler logs the time since the previous present of the swapchain XeFG presents to ("Frametime:").
// Presents a few ms apart reach a 120 Hz screen as one; generated frames bunched that way are not seen.
struct Cadence
{
    unsigned presents = 0, bunched = 0;
    double p10 = 0, p50 = 0, p90 = 0;
};
static Cadence PresentCadence(const Logs& from)
{
    Cadence c;
    const std::string log = ReadShared(dir / "OptiScaler.log");
    std::vector<double> gaps;
    const char* token = "LocalPresent SyncInterval: ";
    for (size_t at = log.find(token, from.opti); at != std::string::npos; at = log.find(token, at + 1))
    {
        const size_t time = log.find("Frametime: ", at);
        if (time != std::string::npos && time < log.find('\n', at))
            gaps.push_back(std::atof(log.c_str() + time + 11));
    }
    if (gaps.empty())
        return c;
    std::sort(gaps.begin(), gaps.end());
    c.presents = (unsigned) gaps.size();
    c.bunched = (unsigned) std::count_if(gaps.begin(), gaps.end(), [](double g) { return g < 4.0; });
    c.p10 = gaps[gaps.size() / 10];
    c.p50 = gaps[gaps.size() / 2];
    c.p90 = gaps[gaps.size() * 9 / 10];
    return c;
}

// ---- What reaches the screen -------------------------------------------------------------------
// Desktop Duplication hands over one frame per desktop update that touches the window. A present the compositor
// replaces before the next refresh never reaches the screen and is not counted. It runs as a second copy of this exe
// ("screens"), which loads the system dxgi.dll and never OptiScaler. With "look <name> [reference]" it also reads
// every one of those frames back (tests\screen_look.h).
static int CountScreenUpdates(double seconds, RECT area, const wchar_t* name = nullptr,
                              const wchar_t* reference = nullptr)
{
    const ScreenLook l = LookAtScreen(seconds, area, dir.wstring(), name, reference);
    check(l.error.empty() ? S_OK : E_FAIL, l.error.c_str());
    if (!name)
        std::printf("%u\n", l.updates);
    else
        std::printf("%u %u %u %u %u %u %u %.3f %.3f %.3f %.3f\n", l.updates, l.frames, l.black, l.rows, l.run, l.top,
                    l.bottom, l.p10, l.p50, l.p90, l.mean);
    return 0;
}
// Starts counting what reaches the screen inside the window for the given time; Visible() returns frames per second.
// With a name, Seen() returns what the screen showed (see CountScreenUpdates).
struct ScreenCounter
{
    HANDLE process = nullptr, output = nullptr;
    double seconds = 0;
    std::string text;
    void Start(double time, const wchar_t* name = nullptr, const wchar_t* reference = nullptr)
    {
        seconds = time;
        RECT client;
        GetClientRect(window, &client);
        MapWindowPoints(window, nullptr, (POINT*) &client, 2);
        SECURITY_ATTRIBUTES inherit { sizeof(inherit), nullptr, TRUE };
        HANDLE write = nullptr;
        CreatePipe(&output, &write, &inherit, 0);
        SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
        wchar_t self[MAX_PATH], line[600];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::swprintf(line, 600, L"\"%s\" screens %.2f %ld %ld %ld %ld %s %s %s", self, seconds, client.left,
                      client.top, client.right, client.bottom, name ? L"look" : L"", name ? name : L"",
                      reference ? reference : L"");
        STARTUPINFOW si { sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = write;
        PROCESS_INFORMATION pi {};
        if (CreateProcessW(nullptr, line, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            process = pi.hProcess;
            CloseHandle(pi.hThread);
        }
        CloseHandle(write);
    }
    void Wait()
    {
        text.clear();
        if (!process)
            return;
        WaitForSingleObject(process, 15000);
        CloseHandle(process);
        char buffer[512] {};
        DWORD read = 0;
        ReadFile(output, buffer, sizeof(buffer) - 1, &read, nullptr);
        CloseHandle(output);
        process = output = nullptr;
        text = buffer;
    }
    double Visible()
    {
        Wait();
        return std::atof(text.c_str()) / seconds;
    }
    ScreenLook Seen()
    {
        Wait();
        ScreenLook l;
        sscanf_s(text.c_str(), "%u %u %u %u %u %u %u %lf %lf %lf %lf", &l.updates, &l.frames, &l.black, &l.rows, &l.run,
                 &l.top, &l.bottom, &l.p10, &l.p50, &l.p90, &l.mean);
        return l;
    }
};

// The image on screen keeps moving: two looks a little apart differ.
static bool Moving()
{
    Run("settle", 0.3, true);
    const auto a = Screen();
    Run("move", 0.25, true);
    const auto b = Screen();
    const double d = Difference(a, b);
    std::printf("  screen change %.2f\n", d);
    return d > 1.0;
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    try
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        dir = fs::path(self).parent_path();
        const std::wstring mode = argc > 1 ? argv[1] : L"fg";
        const double phase = argc > 2 ? _wtof(argv[2]) : 4.0;
        if (mode == L"screens" && argc > 6)
            return CountScreenUpdates(phase, { _wtol(argv[3]), _wtol(argv[4]), _wtol(argv[5]), _wtol(argv[6]) },
                                      argc > 8 ? argv[8] : nullptr, argc > 9 ? argv[9] : nullptr);
        for (int i = 3; i < argc; ++i)
            if (wcsncmp(argv[i], L"sync=", 5) == 0)
                syncInterval = (UINT) _wtoi(argv[i] + 5);
            else if (wcsncmp(argv[i], L"bgra=", 5) == 0)
                bgra = _wtoi(argv[i] + 5) != 0;
            else if (wcsncmp(argv[i], L"tearing=", 8) == 0)
                tearing = _wtoi(argv[i] + 8) != 0;
            else if (wcsncmp(argv[i], L"full=", 5) == 0)
                full = _wtoi(argv[i] + 5) != 0;
            else if (wcsncmp(argv[i], L"blt=", 4) == 0)
                blt = _wtoi(argv[i] + 4) != 0;
            else if (wcsncmp(argv[i], L"srgb=", 5) == 0)
                srgb = _wtoi(argv[i] + 5) != 0;
            else if (wcsncmp(argv[i], L"r10=", 4) == 0)
                r10 = _wtoi(argv[i] + 4) != 0;
        const bool fg = mode == L"fg" || mode == L"focus" || mode == L"fgoff" || mode == L"resize" || mode == L"menu" ||
                        mode == L"pace" || mode == L"recreate" || mode == L"artefacts" || mode == L"bars";
        fs::remove(dir / "OptiScaler.log");
        fs::remove(dir / "amd_presr.log");
        WriteIni(fg, argc > 3 ? argv[3] : (mode == L"pace" ? L"1" : nullptr),
                 std::vector<std::wstring>(argv + std::min(argc, 4), argv + argc));

        // Nothing must hang: a phase that stops presenting for 30 s fails the run.
        HANDLE mainThread = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &mainThread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        std::thread([mainThread] {
            unsigned seen = 0, still = 0;
            for (;;)
            {
                Sleep(1000);
                const unsigned now = progress;
                still = now == seen ? still + 1 : 0;
                seen = now;
                if (still >= 30)
                {
                    std::printf("FAIL: no frame presented for 30 s during \"%s\"\n", phaseName.load());
                    PrintStack(mainThread);
                    std::fflush(stdout);
                    TerminateProcess(GetCurrentProcess(), 3);
                }
            }
        }).detach();

        HMODULE proxy = LoadLibraryW((dir / L"dxgi.dll").wstring().c_str());
        if (!proxy)
            throw std::runtime_error("dxgi.dll (OptiScaler) did not load");
        opti.Load(proxy);
        AddVectoredExceptionHandler(1, Crash);

        WNDCLASSW wc {};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR) IDC_ARROW);
        wc.lpszClassName = L"OptiDx11FinalImageSmoke";
        RegisterClassW(&wc);
        if (full)
        {
            width = (UINT) GetSystemMetrics(SM_CXSCREEN);
            height = (UINT) GetSystemMetrics(SM_CYSCREEN);
            window = CreateWindowW(wc.lpszClassName, L"OptiScaler D3D11 smoke", WS_POPUP | WS_VISIBLE, 0, 0, width,
                                   height, nullptr, nullptr, wc.hInstance, nullptr);
        }
        else
        {
            RECT rect { 0, 0, (LONG) width, (LONG) height };
            AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
            window =
                CreateWindowW(wc.lpszClassName, L"OptiScaler D3D11 smoke", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
        }
        decoy = CreateWindowW(wc.lpszClassName, L"focus thief", WS_OVERLAPPEDWINDOW, 200, 200, 400, 300, nullptr,
                              nullptr, wc.hInstance, nullptr);
        Foreground(window);
        Pump();
        resized = false;
        game.Create();

        Logs logs;
        if (mode == L"keys")
        {
            // Menu and hotkeys only, quickly.
            Run("warm-up", 4);
            for (int i = 0; i < 3; ++i)
            {
                Press(VK_INSERT);
                expect(*opti.menuVisible, "Insert opens the menu");
                Run("menu open", 1, true);
                Press(VK_INSERT);
                expect(!*opti.menuVisible, "Insert closes the menu");
                Run("menu closed", 1, true);
            }
            std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
            return failures ? 1 : 0;
        }
        Run("warm-up", fg ? 14 : 10);
        expect(logs.Amd("Completed AMD pre-SR") > 0, "NR ran on the final image (amd_presr.log: Completed AMD pre-SR)");
        if (fg)
        {
            expect(logs.Opti("Created Dx11wDx12SC") == 1, "D3D11 swapchain went through the D3D12 bridge");
            expect(logs.Opti("XeFG swapchain created") > 0, "XeFG swapchain created");
        }
        const double expected = fg ? 3.0 : 1.0;
        auto FgShowing = [&](const Stats& s) { return std::abs(s.presentsPerFrame - expected) < 0.5; };
        if (mode == L"pace")
        {
            // How XeFG spaces its presents with FG on, and how many presents a game frame makes with FG off.
            Logs mark;
            mark.Mark();
            ScreenCounter screen;
            screen.Start(phase - 0.5);
            const Stats on = Run("FG on", phase);
            const double visibleOn = screen.Visible();
            const Cadence fgOn = PresentCadence(mark);
            std::printf("  FG on: %u presents for %u game frames, time between presents p10 %.2f p50 %.2f p90 %.2f ms, "
                        "%u under 4 ms; on screen %.1f fps for %.1f game fps\n",
                        fgOn.presents, on.frames, fgOn.p10, fgOn.p50, fgOn.p90, fgOn.bunched, visibleOn, on.appFps);
            expect(fgOn.presents > on.frames * 5 / 2, "FG on: XeFG presents about 3 frames per game frame");
            // With the game at a quarter of the refresh rate, 3X fits on the screen: every frame XeFG presents should
            // be seen.
            MONITORINFOEXW monitor {};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            DEVMODEW display {};
            display.dmSize = sizeof(display);
            EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &display);
            const double refresh = display.dmDisplayFrequency > 1 ? display.dmDisplayFrequency : 60;
            game.cpuMs = float(4000.0 / refresh);
            Run("slow game settle", 1, true);
            screen.Start(phase - 0.5);
            const Stats slow = Run("FG on, slow game", phase);
            const double visibleSlow = screen.Visible();
            game.cpuMs = 2;
            std::printf("  FG on, game at %.1f fps on a %.0f Hz screen: on screen %.1f fps (%.2f per game frame, %.2f "
                        "presents per game frame)\n",
                        slow.appFps, refresh, visibleSlow, visibleSlow / slow.appFps, slow.presentsPerFrame);
            expect(visibleSlow > slow.appFps * 2.5, "FG on: about 3 frames per game frame reach the screen");
            Press(VK_END);
            Run("FG off settle", 1, true);
            mark.Mark();
            screen.Start(phase - 0.5);
            const Stats off = Run("FG off", phase);
            const double visibleOff = screen.Visible();
            const Cadence fgOff = PresentCadence(mark);
            std::printf("  FG off: %u presents for %u game frames; on screen %.1f fps for %.1f game fps\n",
                        fgOff.presents, off.frames, visibleOff, off.appFps);
            expect(fgOff.presents <= off.frames + 2, "FG off: one present per game frame");
            std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
            return failures ? 1 : 0;
        }
        if (mode == L"bars")
        {
            // Every frame XeFG presents, read on its swapchain's queue before the display takes it
            // (tests\present_look.h), so what a fullscreen game with tearing shows is read too: rows of black, which
            // the scene never has, with the menu closed and open and with the game's CPU or the GPU setting the pace;
            // then, on a still scene, how far each present is from the one before it (a generated frame without NR
            // stands out from the real frames as a band of the picture once presents tear).
            // XeFG presents through OptiScaler's wrapper of its DXGI swapchain; the wrapper's first present after this
            // hands over the real swapchain, whose class the probe then patches.
            static void *wrapperPresent = nullptr, *wrapperPresent1 = nullptr;
            static DWORD realAt = 0;
            static DWORD deviceAt = 0;
            static auto findReal = [](IDXGISwapChain* wrapper)
            {
                auto real = *(IDXGISwapChain**) ((BYTE*) wrapper + realAt);
                auto device = *(IUnknown**) ((BYTE*) wrapper + deviceAt);
                ComPtr<ID3D12CommandQueue> queue;
                if (real && device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))))
                    InstallPresentLook(real, queue.Get());
            };
            struct Finder
            {
                static HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* s, UINT i, UINT f)
                {
                    findReal(s);
                    return ((present_look::PresentFn) wrapperPresent)(s, i, f);
                }
                static HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* s, UINT i, UINT f,
                                                          const DXGI_PRESENT_PARAMETERS* p)
                {
                    findReal(s);
                    return ((present_look::Present1Fn) wrapperPresent1)(s, i, f, p);
                }
            };
            {
                SYMBOL_INFOW type {};
                type.SizeOfStruct = sizeof(type);
                check(SymGetTypeFromNameW(opti.process, opti.base, L"WrappedIDXGISwapChain4", &type) ? S_OK : E_FAIL,
                      "WrappedIDXGISwapChain4 type");
                realAt = opti.Offset(type.TypeIndex, L"_real");
                deviceAt = opti.Offset(type.TypeIndex, L"_device");
            }
            auto wrapperTable = (void**) opti.Address(L"WrappedIDXGISwapChain4::`vftable'");
            expect(PatchPresentSlots(wrapperTable, (void*) &Finder::Present, (void*) &Finder::Present1, &wrapperPresent,
                                     &wrapperPresent1),
                   "present probe finds XeFG's swapchain through OptiScaler's wrapper");
            Run("probe settle", 0.5, true);
            expect(present_look::state.table != nullptr, "present probe on the DXGI swapchain class");
            PresentLook total;
            auto Probe = [&](const char* name, const wchar_t* csv, double seconds)
            {
                PresentLook l;
                std::thread reader([&] { l = LookAtPresents(seconds - 0.3, (dir / csv).wstring()); });
                const Stats s = Run(name, seconds);
                reader.join();
                std::printf("  %s: %u frames presented and read, %u with black rows (%u rows, longest run %u), %u "
                            "with partly black rows, %u not read; %.2f presents per game frame; GPU time between "
                            "presents p10 %.2f p50 %.2f p90 %.2f ms, %u bunched; step from the frame before p50 %.2f "
                            "p90 %.2f max %.2f\n",
                            name, l.frames, l.black, l.rows, l.run, l.partial, l.dropped, s.presentsPerFrame, l.gapP10,
                            l.gapP50, l.gapP90, l.bunched, l.stepP50, l.stepP90, l.stepMax);
                expect(l.frames > s.frames * 2, "the probe read the frames XeFG presented");
                total.frames += l.frames;
                total.black += l.black;
                total.rows += l.rows;
                return l;
            };
            const float cpuMs = game.cpuMs;
            game.tilt = 8;
            Probe("FG on, menu closed", L"bars-closed.csv", phase);
            // Which D3D12 queue each part uses: the bridge's copy, XeFG's game queue (NR and XeFG's reads), and the
            // queue XeFG presents its frames on.
            auto fgFeature = opti.Member<void*>(game.swap.Get(), L"Dx11wDx12SC", L"_fg");
            std::printf("  queues: bridge copy %p, XeFG game queue %p, XeFG presents on %p\n",
                        opti.Member<void*>(game.swap.Get(), L"Dx11wDx12SC", L"_dx12CommandQueue"),
                        fgFeature ? opti.Member<void*>(fgFeature, L"IFGFeature_Dx12", L"_gameCommandQueue") : nullptr,
                        present_look::state.queue.load());
            game.speed = 24;
            game.tilt = 30;
            Probe("FG on, fast pan", L"bars-fast.csv", phase);
            Press(VK_INSERT);
            expect(*opti.menuVisible, "Insert opens the menu");
            Probe("FG on, menu open", L"bars-menu.csv", phase);
            game.cpuMs = cpuMs * 4 + 20;
            Probe("FG on, menu open, slow game", L"bars-menu-slow.csv", phase);
            game.cpuMs = cpuMs;
            // The GPU, not the game's CPU, sets the pace: the game's own D3D11 work, NR and XeFG compete for it.
            const float gpuLoad = game.gpuLoad;
            game.gpuLoad = gpuLoad * 512;
            Probe("FG on, menu open, GPU-bound game", L"bars-menu-gpu.csv", phase);
            game.gpuLoad = gpuLoad * 1536;
            Probe("FG on, menu open, heavier GPU-bound game", L"bars-menu-gpu2.csv", phase);
            game.gpuLoad = gpuLoad;
            Press(VK_INSERT);
            expect(!*opti.menuVisible, "Insert closes the menu");
            Probe("FG on, menu closed again", L"bars-after.csv", phase);
            // A still scene: consecutive presents differ only where generated and real frames differ. NR off first,
            // as the floor. The game runs at about 15 fps here, where XeFG has the most time between real frames.
            game.speed = 0;
            game.tilt = 0;
            game.clock = 0;
            game.cpuMs = 60;
            Press(VK_NUMPAD5);
            Run("NR off settle", 2, true);
            const PresentLook plain = Probe("still, NR off, FG on", L"bars-still-plain.csv", phase);
            Press(VK_NUMPAD5);
            Run("NR on settle", 3, true);
            const PresentLook still = Probe("still, NR on, FG on", L"bars-still-nr.csv", phase);
            char text[200];
            std::snprintf(text, sizeof(text), "no presented frame with black rows (%u of %u, %u rows)", total.black,
                          total.frames, total.rows);
            expect(total.frames > 0 && total.black == 0, text);
            std::snprintf(text, sizeof(text),
                          "still scene, NR on: consecutive presents as alike as with NR off (step p90 %.2f against "
                          "%.2f)",
                          still.stepP90, plain.stepP90);
            expect(still.stepP90 < std::max(1.0, plain.stepP90 * 2), text);
            std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
            return failures ? 1 : 0;
        }
        if (mode == L"artefacts")
        {
            // What the screen shows with XeFG on. Moving: rows of black, which the scene never has, while the view
            // pans and swings up and down, at the game's own rate and at a quarter of the refresh rate (where every
            // generated frame is shown). Still: NR's strength in every frame shown, with FG on and with FG off,
            // against the same scene with NR off.
            MONITORINFOEXW monitor {};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            DEVMODEW display {};
            display.dmSize = sizeof(display);
            EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &display);
            const double refresh = display.dmDisplayFrequency > 1 ? display.dmDisplayFrequency : 60;
            auto Watch = [&](const char* phase, const wchar_t* name, double seconds, const wchar_t* reference = nullptr)
            {
                ScreenCounter screen;
                screen.Start(seconds - 0.5, name, reference);
                const Stats s = Run(phase, seconds);
                const ScreenLook l = screen.Seen();
                std::printf(
                    "  %s: %u frames shown, %u with black rows (%u rows, %u above the middle, %u below, longest "
                    "run %u); difference to the reference p10 %.2f p50 %.2f p90 %.2f mean %.2f; %.2f presents "
                    "per game frame\n",
                    phase, l.frames, l.black, l.rows, l.top, l.bottom, l.run, l.p10, l.p50, l.p90, l.mean,
                    s.presentsPerFrame);
                return l;
            };
            const float cpuMs = game.cpuMs, slowMs = float(4000.0 / refresh);
            game.tilt = 8;
            const ScreenLook moving = Watch("FG on, moving", L"moving", phase);
            game.speed = 24;
            game.tilt = 30;
            const ScreenLook fast = Watch("FG on, fast pan", L"fast", phase);
            game.cpuMs = slowMs;
            Run("slow game settle", 1, true);
            const ScreenLook slow = Watch("FG on, fast pan, slow game", L"slow", phase);
            expect(moving.frames > 0 && fast.frames > 0 && slow.frames > 0, "the screen was read back");
            expect(moving.black == 0 && fast.black == 0 && slow.black == 0, "no frame shown with black rows");
            // Still scene, every present shown.
            game.speed = 0;
            game.tilt = 0;
            game.clock = 0;
            Press(VK_NUMPAD5);
            Run("NR off settle", 2, true);
            const ScreenLook plain = Watch("still, NR off, FG on", L"plain", phase);
            Press(VK_NUMPAD5);
            Run("NR on settle", 3, true);
            const ScreenLook nrFg = Watch("still, NR on, FG on", L"nrfg", phase, L"plain.ref");
            Press(VK_END);
            Run("FG off settle", 2, true);
            const ScreenLook nrOnly = Watch("still, NR on, FG off", L"nr", phase, L"plain.ref");
            Press(VK_END);
            Run("FG on settle", 2, true);
            game.cpuMs = cpuMs;
            char text[200];
            std::snprintf(text, sizeof(text),
                          "NR changes the still scene (FG off: mean %.2f from the scene without NR)", nrOnly.mean);
            expect(plain.frames > 0 && nrOnly.p50 > 1.0, text);
            std::snprintf(
                text, sizeof(text),
                "NR as strong in every frame shown with FG on (p10 %.2f, mean %.2f) as with FG off (p50 %.2f)",
                nrFg.p10, nrFg.mean, nrOnly.p50);
            expect(nrFg.p10 > nrOnly.p50 * 0.75 && nrFg.mean > nrOnly.mean * 0.85, text);
            std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
            return failures ? 1 : 0;
        }
        if (mode == L"recreate")
        {
            // The swapchain dropped and made again with FG on, every other time with the menu open.
            for (int round = 0; round < 4; ++round)
            {
                const bool menu = round % 2 == 1;
                if (menu)
                    Press(VK_INSERT);
                game.Recreate();
                Run("recreate settle", 3, true);
                Stats after = Run(menu ? "recreated, menu open" : "recreated", phase);
                expect(after.nr > after.frames * 9 / 10, "NR runs after the swapchain is recreated");
                expect(FgShowing(after), "XeFG presents generated frames after the swapchain is recreated");
                expect(Moving(), "image moves after the swapchain is recreated");
                if (menu)
                {
                    expect(*opti.menuVisible, "menu stays open through the swapchain recreation");
                    Press(VK_INSERT);
                }
            }
            expect(logs.Opti("Dx11wDx12SC 5 created") == 1, "every swapchain went through the D3D12 bridge");
            const auto log = ReadShared(dir / "OptiScaler.log");
            expect(Count(log, "DEVICE_HUNG") == 0 && Count(log, "887A0005") == 0 && Count(log, "Device removed") == 0 &&
                       Count(log, "restart required") == 0 && Count(log, "GPU timeout") == 0,
                   "no device removal or NR stop");
            std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
            return failures ? 1 : 0;
        }
        if (mode == L"resize")
        {
            // ResizeBuffers over and over with FG on, every other one with the menu open.
            static const UINT sizes[][2] = { { 1600, 900 }, { 1280, 720 }, { 1440, 810 } };
            for (int round = 0; round < 9; ++round)
            {
                const bool menu = round % 2 == 1;
                if (menu)
                    Press(VK_INSERT);
                RECT frame { 0, 0, (LONG) sizes[round % 3][0], (LONG) sizes[round % 3][1] };
                AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
                SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                             SWP_NOMOVE | SWP_NOZORDER);
                Run("resize settle", 2, true);
                Stats after = Run(menu ? "resized, menu open" : "resized", phase);
                expect(width == sizes[round % 3][0], "window resized");
                expect(after.nr > after.frames * 9 / 10, "NR runs after ResizeBuffers");
                expect(FgShowing(after), "XeFG presents generated frames after ResizeBuffers");
                if (menu)
                {
                    expect(*opti.menuVisible, "menu stays open through ResizeBuffers");
                    Press(VK_INSERT);
                }
            }
            const auto log = ReadShared(dir / "OptiScaler.log");
            expect(Count(log, "DEVICE_HUNG") == 0 && Count(log, "887A0005") == 0 && Count(log, "Device removed") == 0,
                   "no device removal");
            std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
            return failures ? 1 : 0;
        }
        if (mode == L"fgoff")
        {
            // FG off and on only: what an overlay counts, and NR once per game frame.
            for (int round = 0; round < 2; ++round)
            {
                Stats on = Run("FG on", phase);
                Press(VK_END);
                Run("FG off settle", 1, true);
                Stats off = Run("FG off", phase);
                expect(off.presentsPerFrame < 1.5, "End turns XeFG off (one present per game frame)");
                expect(off.nr > off.frames * 9 / 10 && off.nr <= off.frames + 2, "NR once per frame with FG off");
                if (off.rtss > 0 && on.rtss > 0)
                {
                    char text[160];
                    std::snprintf(text, sizeof(text), "RTSS reports %.1f fps for %.1f game fps with FG off", off.rtss,
                                  off.appFps);
                    expect(off.rtss < off.appFps * 1.3, text);
                    std::snprintf(text, sizeof(text), "RTSS reports %.1f fps for %.1f game fps with FG on", on.rtss,
                                  on.appFps);
                    expect(on.rtss > on.appFps * 2.5, text);
                }
                else
                    std::printf("  RTSS is not running: its counter is not checked\n");
                Press(VK_END);
                Run("FG on settle", 2, true);
            }
            std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
            return failures ? 1 : 0;
        }
        if (mode != L"focus")
        {

        Stats closed = Run("menu closed", phase);
        expect(closed.nr > closed.frames * 9 / 10, "NR on every frame, menu closed");
        expect(closed.nr <= closed.frames + 2, "NR once per frame, menu closed");
        if (fg)
            expect(FgShowing(closed), "XeFG presents generated frames (3 presents per game frame)");
        expect(Moving(), "image moves, menu closed");

        Press(VK_INSERT);
        expect(*opti.menuVisible, "Insert opens the menu");
        static const char* sections[] = { "menu: Neural", "menu: Upscaling", "menu: Frame Gen",
                                          "menu: Image", "menu: Interface", "menu: Advanced" };
        std::vector<Stats> open;
        for (int section = 0; section < 6; ++section)
        {
            *opti.menuSection = section;
            open.push_back(Run(sections[section], phase));
            expect(*opti.menuVisible, "menu stays open");
            expect(open.back().nr > open.back().frames * 9 / 10, "NR keeps running with the menu open");
            if (fg)
                expect(FgShowing(open.back()), "XeFG keeps presenting generated frames with the menu open");
        }
        *opti.menuSection = 0;
        expect(Moving(), "image moves, menu open");
        Press(VK_INSERT);
        expect(!*opti.menuVisible, "Insert closes the menu");
        Stats after = Run("menu closed again", phase);
        expect(after.nr > after.frames * 9 / 10, "NR keeps running after the menu closes");
        if (fg)
            expect(FgShowing(after), "XeFG keeps presenting generated frames after the menu closes");

        // Frame time must not depend on the menu: every section within 25% (and 1.5 ms) of closed.
        for (size_t i = 0; i < open.size(); ++i)
        {
            const double gap = open[i].average - closed.average;
            char text[160];
            std::snprintf(text, sizeof(text), "%s frame time %.2f ms vs %.2f ms closed", sections[i], open[i].average,
                          closed.average);
            expect(gap < std::max(1.5, closed.average * 0.25), text);
            // Stalls hide in an average: the slowest frames of each section against the closed menu, before and after.
            const double reference = std::max(closed.p99, after.p99);
            std::snprintf(text, sizeof(text), "%s p99 %.2f ms vs %.2f ms closed", sections[i], open[i].p99, reference);
            expect(open[i].p99 < std::max(reference * 1.5, reference + 8), text);
        }

        if (mode == L"menu")
        {
            std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
            return failures ? 1 : 0;
        }

        // Camera: a fast pan costs no more than a still one.
        game.speed = 0;
        Stats still = Run("camera still", phase);
        game.speed = 60;
        Stats pan = Run("camera fast pan", phase);
        game.speed = 4;
        {
            char text[160];
            std::snprintf(text, sizeof(text), "fast pan %.2f ms vs still %.2f ms per frame", pan.average, still.average);
            expect(pan.average < still.average * 1.15 + 0.5, text);
        }

        // Resize with everything on.
        SetWindowPos(window, nullptr, 0, 0, 1600 + 16, 900 + 39, SWP_NOMOVE | SWP_NOZORDER);
        Run("resize settle", 3);
        Stats big = Run("after ResizeBuffers", phase);
        expect(width != 1280, "window resized");
        expect(big.nr > big.frames * 9 / 10, "NR runs after ResizeBuffers");
        if (fg)
            expect(FgShowing(big), "XeFG presents generated frames after ResizeBuffers");
        expect(Moving(), "image moves after ResizeBuffers");

        // NR scale and passes.
        const float scale0 = opti.Get<float>(L"AmdNrScale");
        const uint32_t passes0 = opti.Get<uint32_t>(L"DlssNrPasses");
        opti.Set<float>(L"AmdNrScale", 0.5f);
        Run("scale settle", 3);
        Stats half = Run("NR scale 0.5", phase);
        expect(half.nr > half.frames * 9 / 10, "NR runs at scale 0.5");
        if (fg)
            expect(FgShowing(half), "XeFG presents generated frames at NR scale 0.5");
        expect(Moving(), "image moves at NR scale 0.5");
        opti.Set<uint32_t>(L"DlssNrPasses", 2);
        Run("passes settle", 3);
        Stats two = Run("NR 2 passes", phase);
        expect(opti.Status().find("passes=2") != std::string::npos && two.nr > two.frames * 9 / 10, "NR runs 2 passes");
        if (fg)
            expect(FgShowing(two), "XeFG presents generated frames with 2 NR passes");
        expect(Moving(), "image moves with 2 NR passes");
        opti.Set<float>(L"AmdNrScale", scale0);
        opti.Set<uint32_t>(L"DlssNrPasses", passes0);
        Run("restore settle", 3, true);

        // NR off and on, FG off and on: the image never freezes.
        for (int round = 0; round < 2; ++round)
        {
            Press(VK_NUMPAD5);
            Stats off = Run("NR off", phase);
            expect(off.nr == 0, "NR key turns NR off");
            expect(Moving(), "image moves with NR off");
            if (fg)
                expect(FgShowing(off), "XeFG keeps presenting generated frames with NR off");
            Press(VK_NUMPAD5);
            Stats on = Run("NR on", phase);
            expect(on.nr > on.frames * 9 / 10, "NR key turns NR back on");
            expect(Moving(), "image moves after NR comes back");
            if (!fg)
                continue;
            Press(VK_END);
            Stats fgOff = Run("FG off", phase);
            expect(fgOff.presentsPerFrame < 1.5, "End turns XeFG off (one present per game frame)");
            expect(fgOff.nr > fgOff.frames * 9 / 10 && fgOff.nr <= fgOff.frames + 2, "NR once per frame with FG off");
            if (fgOff.rtss > 0)
            {
                char text[160];
                std::snprintf(text, sizeof(text), "RTSS reports %.1f fps for %.1f game fps with FG off", fgOff.rtss,
                              fgOff.appFps);
                expect(fgOff.rtss < fgOff.appFps * 1.3, text);
            }
            expect(Moving(), "image moves with FG off");
            Press(VK_END);
            Run("FG on settle", 2, true);
            Stats fgOn = Run("FG on", phase);
            expect(FgShowing(fgOn), "End turns XeFG back on");
            expect(Moving(), "image moves after FG comes back");
        }

        }

        // Focus loss and regain, then a minimise and restore (what Alt+Tab does to many games).
        Foreground(decoy);
        Run("unfocused", phase);
        expect(GetForegroundWindow() != window, "focus moved to another window");
        Foreground(window);
        Stats back = Run("focus regained", phase);
        expect(back.nr > back.frames * 9 / 10, "NR runs after focus returns");
        if (fg)
            expect(FgShowing(back), "XeFG presents generated frames after focus returns");
        expect(Moving(), "image moves after focus returns");
        ShowWindow(window, SW_MINIMIZE);
        Run("minimised", 2);
        ShowWindow(window, SW_RESTORE);
        Foreground(window);
        Run("restore settle", 2, true);
        Stats restored = Run("restored", phase);
        if (fg)
            expect(FgShowing(restored), "XeFG presents generated frames after a minimise");
        expect(Moving(), "image moves after a minimise");
        Press(VK_INSERT);
        expect(*opti.menuVisible, "menu opens after focus changes");
        Run("menu after focus", 2);
        expect(*opti.menuVisible, "menu stays open after focus changes");
        Press(VK_INSERT);
        expect(!*opti.menuVisible, "menu closes after focus changes");

        const auto log = ReadShared(dir / "OptiScaler.log");
        expect(Count(log, "GPU timeout") == 0 && Count(log, "restart required") == 0, "no NR stop in OptiScaler.log");
        expect(Count(log, "DEVICE_REMOVED") == 0 && Count(log, "DEVICE_HUNG") == 0 && Count(log, "887A0005") == 0 &&
                   Count(log, "Device removed") == 0,
               "no device removal");
        std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
        std::fflush(stdout);
        // Leave like a game that closes its window.
        game.swap.Reset();
        game.context->ClearState();
        game.context->Flush();
        DestroyWindow(window);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: %s (phase %s)\n", e.what(), phaseName.load());
        return 2;
    }
}
