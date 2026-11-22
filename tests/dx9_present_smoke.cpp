// A 64-bit D3D9 game in miniature for OptiScaler loaded as d3d9.dll beside it. It renders a scrolling pattern,
// presents through IDirect3DDevice9::Present, PresentEx or IDirect3DSwapChain9::Present, and follows a script of
// key presses posted to its window (the menu, NR and frame-generation keys), device Resets with a new size and screen
// samples. It prints the frame time of each scripted phase and whether sampled frames still change on screen.
//   dx9_present_smoke.exe [--ex] [--flipex] [--chain] [--msaa] [--vsync] [--borderless] [--size WxH]
//                         [--cursor XxY] [--frames N] [--shots folder]
//                         [--script "frame:action,..."]
// --flipex is a D3D9Ex device with D3DSWAPEFFECT_FLIPEX.
// Actions: menu (Insert), nr (Numpad 5), fg (End), still (stops or restarts the scrolling), press and release
// (the left button, where --cursor XxY put the cursor before OptiScaler started), minimize, restore, window2 (presents
// into a new window from then on, as a game that recreates its window), recreate (releases the device and creates
// another one on the same window), cpu=ms (busy CPU work per frame), reset=WxH, fullscreen (a Reset into exclusive
// fullscreen at the desktop size), windowed (a Reset back to the window), remove12=ms (another thread puts the
// process's D3D12 device, the one OptiScaler made, into the removed state after ms), shot=name, motion=name,
// diff=name (compares the screen with an earlier shot), mark=name, dual (a second device on the same window presents
// after the first one every frame from then on), wndproc (prints the module of the window's WndProc), input (sends a
// key press and a mouse move to the window and prints how many of them reached its WndProc), bool=Field=0|1,
// int=Field=N, float=Field=X and unset=Field (write one of OptiScaler's Config fields, as the menu does), tab=N (the
// menu section), hold and unhold (take and drop a reference to the D3D9 bridge's presenter buffer, which makes the
// ResizeBuffers of a plain DXGI presenter fail meanwhile; XeFG's swapchain resizes anyway, and an unhold after that
// crashes). The Config, tab, hold and unhold actions read OptiScaler.pdb beside d3d9.dll.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <dbghelp.h>
#include <dxgi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Create9 = IDirect3D9*(WINAPI*) (UINT);
using Create9Ex = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);

struct Vertex
{
    float x, y, z, rhw;
    D3DCOLOR colour;
};

struct Shot
{
    std::vector<unsigned char> pixels;
    int width = 0, height = 0;
};

HWND window = nullptr;
HWND presentTo = nullptr;
bool quit = false;
bool borderless = false;
int seen = 0;

LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_CLOSE || message == WM_DESTROY)
        quit = true;

    // The messages of the input action.
    if (((message == WM_KEYDOWN || message == WM_KEYUP) && w == 'J') ||
        (message == WM_MOUSEMOVE && l == MAKELPARAM(7, 9)))
        ++seen;

    return DefWindowProcW(hwnd, message, w, l);
}

// OptiScaler's Config and menu section through its PDB, the values the menu writes, and the D3D9 bridge's presenter.
struct Opti
{
    DWORD64 base = 0;
    BYTE* config = nullptr;
    ULONG configType = 0;

    bool Load(HMODULE module)
    {
        if (config != nullptr)
            return true;

        wchar_t path[MAX_PATH] {};
        GetModuleFileNameW(module, path, MAX_PATH);
        std::wstring folder(path);
        folder = folder.substr(0, folder.find_last_of(L"\\/"));
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SYMBOL_INFOW type {};
        type.SizeOfStruct = sizeof(type);

        if (!SymInitializeW(GetCurrentProcess(), folder.c_str(), FALSE) ||
            (base = SymLoadModuleExW(GetCurrentProcess(), nullptr, path, nullptr, (DWORD64) module, 0, nullptr, 0)) ==
                0 ||
            !SymGetTypeFromNameW(GetCurrentProcess(), base, L"Config", &type) || Address(L"Config::_config") == nullptr)
            return false;

        configType = type.TypeIndex;
        config = *static_cast<BYTE**>(Address(L"Config::_config"));
        return config != nullptr;
    }

    void* Address(const wchar_t* name)
    {
        std::vector<BYTE> storage(sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t));
        auto symbol = reinterpret_cast<SYMBOL_INFOW*>(storage.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
        symbol->MaxNameLen = MAX_SYM_NAME;
        return SymFromNameW(GetCurrentProcess(), name, symbol) ? reinterpret_cast<void*>(symbol->Address) : nullptr;
    }

    // A CustomOptional<T> starts with its std::optional<T>: the value, then the engaged flag.
    BYTE* Field(const std::wstring& name) { return Member(config, configType, name); }

    // Bridge::swap in with_dx12\dx9_with_dx12.cpp, a ComPtr, which holds the pointer alone.
    IDXGISwapChain* Presenter()
    {
        auto bridge = static_cast<BYTE**>(Address(L"bridge"));
        SYMBOL_INFOW type {};
        type.SizeOfStruct = sizeof(type);

        if (bridge == nullptr || *bridge == nullptr ||
            !SymGetTypeFromNameW(GetCurrentProcess(), base, L"`anonymous-namespace'::Bridge", &type))
            return nullptr;

        BYTE* swap = Member(*bridge, type.TypeIndex, L"swap");
        return swap != nullptr ? *reinterpret_cast<IDXGISwapChain**>(swap) : nullptr;
    }

    BYTE* Member(BYTE* object, ULONG objectType, const std::wstring& name)
    {
        DWORD count = 0;
        SymGetTypeInfo(GetCurrentProcess(), base, objectType, TI_GET_CHILDRENCOUNT, &count);
        std::vector<BYTE> storage(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
        auto children = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(storage.data());
        children->Count = count;
        SymGetTypeInfo(GetCurrentProcess(), base, objectType, TI_FINDCHILDREN, children);

        for (DWORD i = 0; i < count; ++i)
        {
            WCHAR* childName = nullptr;
            DWORD offset = 0;

            if (!SymGetTypeInfo(GetCurrentProcess(), base, children->ChildId[i], TI_GET_SYMNAME, &childName))
                continue;

            const bool match = name == childName;
            LocalFree(childName);

            if (match && SymGetTypeInfo(GetCurrentProcess(), base, children->ChildId[i], TI_GET_OFFSET, &offset))
                return object + offset;
        }

        return nullptr;
    }
} opti;

// bool=Field=0|1, int=Field=N, float=Field=X, unset=Field (a field holding 4 bytes) or tab=N.
bool SetOpti(HMODULE module, const std::string& action)
{
    if (!opti.Load(module))
        return false;

    const size_t kind = action.find('='), equals = action.find('=', kind + 1);
    const std::string verb = action.substr(0, kind);
    const std::string name =
        action.substr(kind + 1, equals == std::string::npos ? std::string::npos : equals - kind - 1);
    const int value = equals == std::string::npos ? 0 : std::atoi(action.c_str() + equals + 1);

    if (verb == "tab")
    {
        auto tab = static_cast<int*>(opti.Address(L"selectedTab"));

        if (tab != nullptr)
            *tab = std::atoi(name.c_str());

        return tab != nullptr;
    }

    BYTE* field = opti.Field(std::wstring(name.begin(), name.end()));

    if (field == nullptr)
        return false;

    if (verb == "bool")
    {
        field[0] = value != 0;
        field[1] = 1;
    }
    else if (verb == "int")
    {
        std::memcpy(field, &value, sizeof(value));
        field[sizeof(value)] = 1;
    }
    else if (verb == "float")
    {
        const float number = float(std::atof(action.c_str() + equals + 1));
        std::memcpy(field, &number, sizeof(number));
        field[sizeof(number)] = 1;
    }
    else if (verb == "unset")
        field[sizeof(int)] = 0;
    else
        return false;

    return true;
}

// A left-button press or release as a mouse would send it, and whether the desktop took it. OptiScaler drops the
// game's own SendInput mouse events while its menu is open and answers GetAsyncKeyState for it, so both go to win32u
// directly, below those hooks.
bool Click(bool down)
{
    using SendInputFn = UINT(WINAPI*)(UINT, LPINPUT, int);
    using KeyStateFn = SHORT(WINAPI*)(int);
    static const HMODULE win32u = LoadLibraryW(L"win32u.dll");
    static const auto send = reinterpret_cast<SendInputFn>(GetProcAddress(win32u, "NtUserSendInput"));
    static const auto keyState = reinterpret_cast<KeyStateFn>(GetProcAddress(win32u, "NtUserGetAsyncKeyState"));

    INPUT input {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;

    if (send == nullptr || keyState == nullptr || send(1, &input, sizeof(INPUT)) != 1)
        return false;

    // The desktop takes the event on its own thread.
    for (int wait = 0; wait < 10; ++wait)
    {
        if (((keyState(VK_LBUTTON) & 0x8000) != 0) == down)
            return true;

        Sleep(5);
    }

    return false;
}

void Pump()
{
    MSG message;

    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void PostKey(UINT vk)
{
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    const bool extended = vk == VK_INSERT || vk == VK_END || vk == VK_HOME || vk == VK_DELETE;
    const LPARAM base = 1 | (LPARAM(scan) << 16) | (extended ? (1 << 24) : 0);
    PostMessageW(window, WM_KEYDOWN, vk, base);
    PostMessageW(window, WM_KEYUP, vk, base | (LPARAM(1) << 30) | (LPARAM(1) << 31));
}

void ClientSize(int width, int height)
{
    RECT rect { 0, 0, width, height };
    const int origin = borderless ? 0 : 40;

    if (!borderless)
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    SetWindowPos(window, HWND_TOPMOST, origin, origin, rect.right - rect.left, rect.bottom - rect.top, SWP_SHOWWINDOW);
}

Shot Capture()
{
    Shot shot;
    RECT client {};
    GetClientRect(window, &client);
    POINT origin { 0, 0 };
    ClientToScreen(window, &origin);
    shot.width = client.right;
    shot.height = client.bottom;

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = shot.width;
    info.bmiHeader.biHeight = -shot.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto old = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, shot.width, shot.height, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    shot.pixels.assign(static_cast<unsigned char*>(bits),
                       static_cast<unsigned char*>(bits) + size_t(shot.width) * shot.height * 4);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return shot;
}

double Mean(const Shot& shot)
{
    double sum = 0;

    for (size_t i = 0; i + 3 < shot.pixels.size(); i += 4)
        sum += 0.0722 * shot.pixels[i] + 0.7152 * shot.pixels[i + 1] + 0.2126 * shot.pixels[i + 2];

    return shot.pixels.empty() ? 0 : sum / (shot.pixels.size() / 4);
}

double Changed(const Shot& a, const Shot& b)
{
    if (a.pixels.size() != b.pixels.size() || a.pixels.empty())
        return 1.0;

    size_t differ = 0;

    for (size_t i = 0; i < a.pixels.size(); i += 4)
        differ += std::abs(int(a.pixels[i]) - int(b.pixels[i])) +
                      std::abs(int(a.pixels[i + 1]) - int(b.pixels[i + 1])) +
                      std::abs(int(a.pixels[i + 2]) - int(b.pixels[i + 2])) >
                  24;

    return double(differ) / (a.pixels.size() / 4);
}

void Save(const Shot& shot, const std::string& path)
{
    BITMAPFILEHEADER file {};
    BITMAPINFOHEADER header {};
    header.biSize = sizeof(header);
    header.biWidth = shot.width;
    header.biHeight = -shot.height;
    header.biPlanes = 1;
    header.biBitCount = 32;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(header);
    file.bfSize = DWORD(file.bfOffBits + shot.pixels.size());

    if (FILE* out = std::fopen(path.c_str(), "wb"))
    {
        std::fwrite(&file, sizeof(file), 1, out);
        std::fwrite(&header, sizeof(header), 1, out);
        std::fwrite(shot.pixels.data(), 1, shot.pixels.size(), out);
        std::fclose(out);
    }
}

// Checkerboard cells scrolling right with a bar moving down: enough structure for the optical flow.
void Draw(IDirect3DDevice9* device, int frame, int width, int height)
{
    std::vector<Vertex> vertices;
    const int cell = 64;
    const int offset = (frame * 6) % (cell * 2);

    auto quad = [&](float x0, float y0, float x1, float y1, D3DCOLOR colour)
    {
        const Vertex v[6] = { { x0, y0, 0, 1, colour }, { x1, y0, 0, 1, colour }, { x0, y1, 0, 1, colour },
                              { x1, y0, 0, 1, colour }, { x1, y1, 0, 1, colour }, { x0, y1, 0, 1, colour } };
        vertices.insert(vertices.end(), v, v + 6);
    };

    for (int y = 0; y < height; y += cell)
        for (int x = -cell * 2; x < width; x += cell)
            if (((x + cell * 2) / cell + y / cell) % 2 == 0)
                quad(float(x + offset), float(y), float(x + offset + cell), float(y + cell),
                     D3DCOLOR_XRGB(230, 200 - (y * 120 / height), 60 + (x & 127)));

    const float bar = float((frame * 4) % height);
    quad(0, bar, float(width), bar + 24, D3DCOLOR_XRGB(250, 250, 250));

    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(30, 60, 110), 1.0f, 0);
    device->BeginScene();
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(vertices.size() / 3), vertices.data(), sizeof(Vertex));
    device->EndScene();
}
} // namespace

int main(int argc, char** argv)
{
    bool ex = false, flipex = false, chainPresent = false, msaa = false, vsync = false;
    POINT cursor { -1, -1 };
    int width = 1280, height = 720, frames = 300;
    std::string shots = ".", script;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : "";

        if (arg == "--ex")
            ex = true;
        else if (arg == "--flipex")
            ex = flipex = true;
        else if (arg == "--chain")
            chainPresent = true;
        else if (arg == "--msaa")
            msaa = true;
        else if (arg == "--borderless")
            borderless = true;
        else if (arg == "--cursor" && std::sscanf(next, "%ldx%ld", &cursor.x, &cursor.y) == 2)
            ++i;
        else if (arg == "--vsync")
            vsync = true;
        else if (arg == "--size" && std::sscanf(next, "%dx%d", &width, &height) == 2)
            ++i;
        else if (arg == "--frames")
            frames = std::atoi(argv[++i]);
        else if (arg == "--shots")
            shots = argv[++i];
        else if (arg == "--script")
            script = argv[++i];
    }

    std::multimap<int, std::string> actions;

    for (size_t start = 0; start < script.size();)
    {
        size_t end = script.find(',', start);
        const std::string item = script.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const size_t colon = item.find(':');

        if (colon != std::string::npos)
            actions.emplace(std::atoi(item.c_str()), item.substr(colon + 1));

        start = end == std::string::npos ? script.size() : end + 1;
    }

    // The proxy beside this executable, never the system d3d9.dll.
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring proxy(path);
    proxy = proxy.substr(0, proxy.find_last_of(L"\\/") + 1) + L"d3d9.dll";
    HMODULE d3d9 = LoadLibraryW(proxy.c_str());

    if (d3d9 == nullptr || GetProcAddress(d3d9, "GetBehaviorValue") == nullptr)
    {
        std::printf("FAIL: d3d9.dll beside the test is not OptiScaler\n");
        return 2;
    }

    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = Proc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"OptiDx9Smoke";
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&windowClass);
    window = CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName, L"Opti D3D9 smoke",
                             borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW, 40, 40, width, height, nullptr, nullptr,
                             windowClass.hInstance, nullptr);
    ClientSize(width, height);
    SetForegroundWindow(window);
    Pump();

    // Before OptiScaler's menu holds the cursor: this is where the script's clicks land. A process started in the
    // background cannot take the foreground with SetForegroundWindow, and the first click on an inactive window
    // only activates it, so click once here. Another program on the desktop can keep injected input from landing
    // at all (the cursor stays put, the button never reads as down); the runner then skips its click checks.
    if (cursor.x >= 0 && ClientToScreen(window, &cursor))
    {
        SetCursorPos(cursor.x, cursor.y);
        const bool landed = Click(true);
        Click(false);
        Pump();

        if (!landed)
            std::printf("injected mouse input does not reach the desktop\n");
    }

    IDirect3D9* api = nullptr;
    IDirect3D9Ex* apiEx = nullptr;

    if (ex)
    {
        auto create = reinterpret_cast<Create9Ex>(GetProcAddress(d3d9, "Direct3DCreate9Ex"));

        if (create == nullptr || FAILED(create(D3D_SDK_VERSION, &apiEx)))
            return 3;

        api = apiEx;
    }
    else
    {
        auto create = reinterpret_cast<Create9>(GetProcAddress(d3d9, "Direct3DCreate9"));
        api = create ? create(D3D_SDK_VERSION) : nullptr;
    }

    if (api == nullptr)
    {
        std::printf("FAIL: Direct3DCreate9\n");
        return 3;
    }

    D3DPRESENT_PARAMETERS params {};
    params.Windowed = TRUE;
    params.SwapEffect = flipex ? D3DSWAPEFFECT_FLIPEX : D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window;
    params.BackBufferWidth = width;
    params.BackBufferHeight = height;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = flipex ? 2 : 1;
    params.EnableAutoDepthStencil = TRUE;
    params.AutoDepthStencilFormat = D3DFMT_D24S8;
    params.MultiSampleType = msaa ? D3DMULTISAMPLE_4_SAMPLES : D3DMULTISAMPLE_NONE;
    params.PresentationInterval = vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9* device = nullptr;
    IDirect3DDevice9Ex* deviceEx = nullptr;
    IDirect3DDevice9* second = nullptr;
    ID3D12Resource* held = nullptr;
    auto create = [&]
    {
        const HRESULT created =
            ex ? apiEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                       &params, nullptr, &deviceEx)
               : api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                   &params, &device);

        if (ex)
            device = deviceEx;

        return created;
    };
    HRESULT result = create();

    if (FAILED(result))
    {
        std::printf("FAIL: CreateDevice %08X\n", unsigned(result));
        return 4;
    }

    std::printf("device %s, %dx%d, present through %s\n", ex ? "D3D9Ex" : "D3D9", width, height,
                chainPresent ? "swapchain"
                : ex         ? "PresentEx"
                             : "Present");

    std::map<std::string, Shot> pending, taken;
    std::multimap<int, std::string> motions;
    std::string phase = "start";
    LARGE_INTEGER frequency, phaseStart, previous;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&phaseStart);
    previous = phaseStart;
    int phaseFrames = 0, failures = 0, scene = 0;
    bool still = false;
    double cpuWork = 0;
    double worst = 0;

    auto endPhase = [&](const std::string& next)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double ms = 1000.0 * double(now.QuadPart - phaseStart.QuadPart) / double(frequency.QuadPart);

        if (phaseFrames > 0)
            std::printf("phase %s: %d frames, %.2f ms average, %.2f ms worst\n", phase.c_str(), phaseFrames,
                        ms / phaseFrames, worst);

        phase = next;
        phaseStart = now;
        phaseFrames = 0;
        worst = 0;
    };

    for (int frame = 0; frame < frames && !quit; ++frame)
    {
        for (auto [it, end] = actions.equal_range(frame); it != end; ++it)
        {
            const std::string& action = it->second;
            std::printf("frame %d: %s\n", frame, action.c_str());

            if (action == "menu")
                PostKey(VK_INSERT);
            else if (action == "nr")
                PostKey(VK_NUMPAD5);
            else if (action == "fg")
                PostKey(VK_END);
            else if (action.rfind("cpu=", 0) == 0)
                cpuWork = std::atof(action.c_str() + 4);
            else if (action == "still")
                still = !still;
            else if (action == "dual")
            {
                IDirect3DDevice9* keep = device;
                IDirect3DDevice9Ex* keepEx = deviceEx;
                result = create();
                second = device;
                device = keep;
                deviceEx = keepEx;
                std::printf("dual: second device %08X\n", unsigned(result));
            }
            else if (action == "hold" || action == "unhold")
            {
                IDXGISwapChain* presenter = opti.Load(d3d9) ? opti.Presenter() : nullptr;

                if (action == "hold" && (presenter == nullptr || FAILED(presenter->GetBuffer(0, IID_PPV_ARGS(&held)))))
                {
                    std::printf("FAIL: no presenter buffer to hold\n");
                    return 7;
                }

                if (action == "unhold" && held != nullptr)
                {
                    held->Release();
                    held = nullptr;
                }
            }
            else if (action == "wndproc")
            {
                HMODULE owner = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(GetWindowLongPtrW(window, GWLP_WNDPROC)), &owner);
                wchar_t name[MAX_PATH] = L"?";

                if (owner != nullptr)
                    GetModuleFileNameW(owner, name, MAX_PATH);

                std::printf("wndproc: %s (%ls)\n",
                            owner == d3d9                        ? "OptiScaler"
                            : owner == GetModuleHandleW(nullptr) ? "the game"
                                                                 : "another module",
                            name);
            }
            else if (action == "input")
            {
                // Sent, not posted: these reach the window's WndProc chain only. With the menu open and the window
                // focused, OptiScaler's WndProc keeps them from the game.
                const LPARAM scan = LPARAM(MapVirtualKeyW('J', MAPVK_VK_TO_VSC)) << 16;
                seen = 0;
                SendMessageW(window, WM_KEYDOWN, 'J', 1 | scan);
                SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(7, 9));
                SendMessageW(window, WM_KEYUP, 'J', 1 | scan | (LPARAM(3) << 30));
                std::printf("input: the game saw %d of 3 (foreground %s)\n", seen,
                            GetForegroundWindow() == window ? "yes" : "no");
            }
            else if (action.rfind("bool=", 0) == 0 || action.rfind("int=", 0) == 0 || action.rfind("float=", 0) == 0 ||
                     action.rfind("unset=", 0) == 0 || action.rfind("tab=", 0) == 0)
            {
                if (!SetOpti(d3d9, action))
                {
                    std::printf("FAIL: %s (OptiScaler.pdb beside d3d9.dll?)\n", action.c_str());
                    return 7;
                }
            }
            else if (action == "window2")
            {
                presentTo = CreateWindowExW(WS_EX_TOPMOST, L"OptiDx9Smoke", L"Opti D3D9 smoke 2",
                                            borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW, 40, 40, width, height, nullptr,
                                            nullptr, GetModuleHandleW(nullptr), nullptr);
                ShowWindow(window, SW_HIDE);
                window = presentTo;
                ClientSize(width, height);
                SetForegroundWindow(window);
            }
            else if (action == "minimize" || action == "restore")
            {
                ShowWindow(window, action == "minimize" ? SW_MINIMIZE : SW_RESTORE);

                if (action == "restore")
                    SetForegroundWindow(window);
            }
            else if (action == "press" || action == "release")
            {
                if (!Click(action == "press"))
                    std::printf("  the mouse %s did not land (%s has the foreground)\n", action.c_str(),
                                GetForegroundWindow() == window ? "this window" : "another window");
            }
            else if (action.rfind("mark=", 0) == 0)
                endPhase(action.substr(5));
            else if (action.rfind("shot=", 0) == 0)
            {
                const Shot shot = Capture();
                Save(shot, shots + "\\" + action.substr(5) + ".bmp");
                std::printf("shot %s: mean %.1f\n", action.substr(5).c_str(), Mean(shot));
                taken[action.substr(5)] = shot;
            }
            else if (action.rfind("diff=", 0) == 0)
            {
                const Shot& before = taken[action.substr(5)];
                const Shot now = Capture();
                double sum = 0;

                for (size_t i = 0; i < now.pixels.size() && now.pixels.size() == before.pixels.size(); ++i)
                    sum += std::abs(int(now.pixels[i]) - int(before.pixels[i]));

                std::printf("diff %s: %.3f of the pixels changed, mean difference %.2f\n", action.substr(5).c_str(),
                            Changed(before, now), now.pixels.empty() ? 0.0 : sum / now.pixels.size());
            }
            else if (action.rfind("motion=", 0) == 0)
            {
                pending[action.substr(7)] = Capture();
                motions.emplace(frame + 3, action.substr(7));
            }
            else if (action == "recreate")
            {
                const ULONG left = device->Release();
                result = create();
                std::printf("recreate: old device %lu references left, new device %08X\n", left, unsigned(result));

                if (FAILED(result))
                {
                    std::printf("FAIL: CreateDevice\n");
                    return 4;
                }
            }
            else if (action.rfind("remove12=", 0) == 0)
            {
                // A process has one D3D12 device per adapter: this is the one the bridge uses.
                const DWORD delay = DWORD(std::atoi(action.c_str() + 9));
                std::thread(
                    [delay]
                    {
                        using CreateFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
                        Sleep(delay);
                        const auto create12 =
                            reinterpret_cast<CreateFn>(GetProcAddress(LoadLibraryW(L"d3d12.dll"), "D3D12CreateDevice"));
                        ID3D12Device5* device12 = nullptr;
                        const HRESULT made =
                            create12 ? create12(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)) : E_FAIL;

                        if (device12 != nullptr)
                        {
                            device12->RemoveDevice();
                            device12->Release();
                        }

                        std::printf("D3D12 device removed: %08X\n", unsigned(made));
                    })
                    .detach();
            }
            else if (action == "fullscreen" || action == "windowed")
            {
                params.Windowed = action == "windowed";
                params.BackBufferWidth = params.Windowed ? width : GetSystemMetrics(SM_CXSCREEN);
                params.BackBufferHeight = params.Windowed ? height : GetSystemMetrics(SM_CYSCREEN);
                HRESULT reset = ex ? deviceEx->ResetEx(&params, nullptr) : device->Reset(&params);
                std::printf("reset %s %ux%u: %08X\n", action.c_str(), params.BackBufferWidth, params.BackBufferHeight,
                            unsigned(reset));

                // A failed Reset leaves the device lost until a Reset succeeds: back to the window then.
                if (FAILED(reset) && !params.Windowed)
                {
                    params.Windowed = TRUE;
                    params.BackBufferWidth = width;
                    params.BackBufferHeight = height;
                    reset = ex ? deviceEx->ResetEx(&params, nullptr) : device->Reset(&params);
                }

                if (FAILED(reset))
                {
                    std::printf("FAIL: Reset\n");
                    return 5;
                }

                if (params.Windowed)
                    ClientSize(width, height);
            }
            else if (action.rfind("reset=", 0) == 0 && std::sscanf(action.c_str() + 6, "%dx%d", &width, &height) == 2)
            {
                ClientSize(width, height);
                Pump();
                params.BackBufferWidth = width;
                params.BackBufferHeight = height;
                const HRESULT reset = ex ? deviceEx->ResetEx(&params, nullptr) : device->Reset(&params);
                std::printf("reset %dx%d: %08X\n", width, height, unsigned(reset));

                if (FAILED(reset))
                {
                    std::printf("FAIL: Reset\n");
                    return 5;
                }
            }
        }

        for (auto [it, end] = motions.equal_range(frame); it != end; ++it)
        {
            const double changed = Changed(pending[it->second], Capture());
            std::printf("motion %s: %.3f of the pixels changed\n", it->second.c_str(), changed);
        }

        Pump();
        // Game work on the CPU before the frame's draws, as a busy wait.
        LARGE_INTEGER busy, busyNow;
        QueryPerformanceCounter(&busy);

        do
            QueryPerformanceCounter(&busyNow);
        while (1000.0 * double(busyNow.QuadPart - busy.QuadPart) / double(frequency.QuadPart) < cpuWork);

        scene += still ? 0 : 1;
        Draw(device, scene, width, height);

        if (chainPresent)
        {
            IDirect3DSwapChain9* chain = nullptr;
            device->GetSwapChain(0, &chain);
            result = chain->Present(nullptr, nullptr, presentTo, nullptr, 0);
            chain->Release();
        }
        else
        {
            result = ex ? deviceEx->PresentEx(nullptr, nullptr, presentTo, nullptr, 0)
                        : device->Present(nullptr, nullptr, presentTo, nullptr);
        }

        if (FAILED(result) && ++failures < 5)
            std::printf("present %d failed: %08X\n", frame, unsigned(result));

        if (second != nullptr)
        {
            Draw(second, scene + 37, width, height);
            const HRESULT other = second->Present(nullptr, nullptr, presentTo, nullptr);

            if (FAILED(other) && ++failures < 5)
                std::printf("second present %d failed: %08X\n", frame, unsigned(other));
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        worst = std::max(worst, 1000.0 * double(now.QuadPart - previous.QuadPart) / double(frequency.QuadPart));
        previous = now;
        ++phaseFrames;
    }

    endPhase("end");

    if (second != nullptr)
        second->Release();

    device->Release();
    api->Release();
    DestroyWindow(window);
    std::printf("DONE %d present failures\n", failures);
    return failures == 0 ? 0 : 6;
}
