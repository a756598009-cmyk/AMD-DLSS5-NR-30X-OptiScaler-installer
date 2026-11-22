// What a game does to its window, against OptiScaler's opengl32.dll route with final-image NR (and
// XeFG when the ini turns it on). Run it beside OptiScaler.dll renamed opengl32.dll.
//
//   recreate: halfway through, the game deletes its context, destroys its window and opens a new one
//             at the same size, as many games do on a fullscreen toggle. The second window has to keep
//             a normal frame time.
//   resize:   the client size changes every 8 frames through 150 sizes, as a drag of the window
//             border does, then holds the last size. The process may not keep a handle per size.
//   two:      one context presents to the game's window and to a second 640x360 window every frame,
//             as an emulator with a second screen does; twosame makes both 1280x720. Insert is posted
//             to the game's window to open the menu and again to close it. The frame time has to stay
//             normal; the runner checks that the menu, NR and XeFG stay on the game's window.
//
//   gl_window_smoke.exe recreate|resize|two|twosame
// Prints the process handle count and frame times. Exit 0 on success, 1 on a failed check, 3 when the
// watchdog fires.

#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int Fail(const char* what)
{
    std::printf("FAIL: %s\n", what);
    std::fflush(stdout);
    return 1;
}

static DWORD WINAPI Watchdog(void*)
{
    Sleep(120000);
    std::printf("FAIL: watchdog, the app did not finish in 120 s\n");
    std::fflush(stdout);
    ExitProcess(3);
}

static LONG CALLBACK ReportFault(EXCEPTION_POINTERS* info)
{
    static LONG reported = 0;
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !InterlockedExchange(&reported, 1))
    {
        void* at = info->ExceptionRecord->ExceptionAddress;
        HMODULE module = nullptr;
        wchar_t name[MAX_PATH] = L"(unknown module)";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(at), &module))
            GetModuleFileNameW(module, name, MAX_PATH);
        std::printf(
            "access violation at %ls+0x%llX on thread %lu\n", name,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(module)),
            GetCurrentThreadId());
        std::fflush(stdout);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_CLOSE)
        return 0;
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static DWORD Handles()
{
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    return handles;
}

static void Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void ClientSize(int width, int height, RECT& outer)
{
    outer = { 0, 0, width, height };
    AdjustWindowRect(&outer, WS_OVERLAPPEDWINDOW, FALSE);
}

// A game is the foreground window, and frame generation pauses when it is not.
static HWND NewWindow(int width = 1280, int height = 720, int x = 40)
{
    RECT outer;
    ClientSize(width, height, outer);
    HWND hwnd = CreateWindowW(L"OptiGlWindowSmoke", L"Opti OpenGL window smoke", WS_OVERLAPPEDWINDOW, x, 40,
                              outer.right - outer.left, outer.bottom - outer.top, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!hwnd)
        return nullptr;
    ShowWindow(hwnd, SW_SHOW);
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    AttachThreadInput(foregroundThread, GetCurrentThreadId(), TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    AttachThreadInput(foregroundThread, GetCurrentThreadId(), FALSE);
    std::printf("window %p in the foreground: %s\n", static_cast<void*>(hwnd),
                GetForegroundWindow() == hwnd ? "yes" : "no");
    return hwnd;
}

static void Rect(float x0, float y0, float x1, float y1)
{
    glBegin(GL_QUADS);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();
}

// Taken just before the resizes start, after NR and XeFG have set up.
static DWORD handlesBefore = 0;

static void Draw(HWND hwnd, int frame)
{
    RECT client {};
    GetClientRect(hwnd, &client);
    glViewport(0, 0, client.right, client.bottom);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, client.right, 0, client.bottom, -1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(1.0f, 0.0f, 0.0f);
    Rect(client.right - 300.0f, client.bottom - 200.0f, client.right - 40.0f, client.bottom - 40.0f);
    for (int i = 0; i < 8; i++)
    {
        const float x = float((frame * 4 + i * 150) % 1200);
        glColor3f(0.1f + 0.1f * i, 0.8f - 0.08f * i, 0.3f + 0.06f * i);
        Rect(x, 260.0f + i * 25, x + 90, 300.0f + i * 25);
    }
}

// Presents to one window, then checks what a game relies on after its SwapBuffers.
static int Present(HDC hdc, HGLRC context)
{
    if (!SwapBuffers(hdc))
        return Fail("SwapBuffers");
    if (wglGetCurrentContext() != context || wglGetCurrentDC() != hdc)
        return Fail("the game's context is not current after the present");
    if (glGetError() != GL_NO_ERROR)
        return Fail("GL error in the game's context");
    return 0;
}

static void PostInsert(HWND hwnd)
{
    PostMessageW(hwnd, WM_KEYDOWN, VK_INSERT, 0x00520001);
    PostMessageW(hwnd, WM_KEYUP, VK_INSERT, 0xC0520001);
}

// Presents at least the given frames on a fresh context; resize changes the client size every 8
// frames for 150 sizes and then holds the last size for 2 s, longer than OptiScaler waits for a size
// to settle. A second window, when given, is presented right after the first with the same context.
// tail is the average frame time in ms over the last 120 frames.
static int Run(HWND hwnd, int frames, bool resize, double& tail, HWND second = nullptr)
{
    HDC hdc = GetDC(hwnd);
    HDC secondDc = second ? GetDC(second) : nullptr;
    PIXELFORMATDESCRIPTOR pfd { sizeof(pfd), 1 };
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    const int format = ChoosePixelFormat(hdc, &pfd);
    if (format == 0 || !SetPixelFormat(hdc, format, &pfd) || (secondDc && !SetPixelFormat(secondDc, format, &pfd)))
        return Fail("pixel format");
    HGLRC context = wglCreateContext(hdc);
    if (!context || !wglMakeCurrent(hdc, context))
        return Fail("context");
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);

    LARGE_INTEGER frequency, stamps[121] {};
    QueryPerformanceFrequency(&frequency);
    ULONGLONG resized = 0;
    int frame = 0;
    for (; frame < frames || GetTickCount64() - resized < 2000; frame++)
    {
        Pump();
        if (frame == 119)
            handlesBefore = Handles();
        if (resize && frame >= 120 && frame < 120 + 8 * 150 && frame % 8 == 0)
        {
            const int i = (frame - 120) / 8;
            RECT outer;
            ClientSize(1280 - (i % 75) * 8, 720 - (i % 75) * 4, outer);
            SetWindowPos(hwnd, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
                         SWP_NOMOVE | SWP_NOZORDER);
            resized = GetTickCount64();
        }

        if (second && (frame == 200 || frame == 260))
            PostInsert(hwnd);

        Draw(hwnd, frame);
        if (const int result = Present(hdc, context))
            return result;
        if (second)
        {
            if (!wglMakeCurrent(secondDc, context))
                return Fail("wglMakeCurrent on the second window");
            Draw(second, frame);
            if (const int result = Present(secondDc, context))
                return result;
            wglMakeCurrent(hdc, context);
        }
        QueryPerformanceCounter(&stamps[frame % 121]);
    }
    const auto last = stamps[(frame - 1) % 121].QuadPart, first = stamps[frame % 121].QuadPart;
    tail = double(last - first) * 1000.0 / double(frequency.QuadPart) / 120.0;

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(hwnd, hdc);
    if (second)
        ReleaseDC(second, secondDc);
    return 0;
}

int main(int argc, char** argv)
{
    const bool recreate = argc > 1 && std::strcmp(argv[1], "recreate") == 0;
    const bool resize = argc > 1 && std::strcmp(argv[1], "resize") == 0;
    const bool twosame = argc > 1 && std::strcmp(argv[1], "twosame") == 0;
    const bool two = twosame || (argc > 1 && std::strcmp(argv[1], "two") == 0);
    if (!recreate && !resize && !two)
        return Fail("mode must be recreate, resize, two or twosame");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    AddVectoredExceptionHandler(1, ReportFault);

    WNDCLASSW type {};
    type.style = CS_OWNDC;
    type.lpfnWndProc = WindowProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, (LPCWSTR) IDC_ARROW);
    type.lpszClassName = L"OptiGlWindowSmoke";
    if (!RegisterClassW(&type))
        return Fail("RegisterClass");

    // The second window opens first, so the game's window ends in the foreground.
    HWND second = two ? NewWindow(twosame ? 1280 : 640, twosame ? 720 : 360, 1360) : nullptr;
    HWND hwnd = NewWindow();
    if (!hwnd || (two && !second))
        return Fail("CreateWindow");
    double tail = 0;
    if (two)
    {
        if (const int result = Run(hwnd, 600, false, tail, second))
            return result;
        std::printf("two windows: %.2f ms per frame over the last 120 frames\n", tail);
        DestroyWindow(second);
        if (tail > 50.0)
            return Fail("presenting to a second window slows the game down");
    }
    else if (recreate)
    {
        if (const int result = Run(hwnd, 600, false, tail))
            return result;
        std::printf("first window: %.2f ms per frame over its last 120 frames\n", tail);
        const double first = tail;
        DestroyWindow(hwnd);
        Pump();
        hwnd = NewWindow();
        if (!hwnd)
            return Fail("CreateWindow");
        if (const int result = Run(hwnd, 600, false, tail))
            return result;
        std::printf("second window: %.2f ms per frame over its last 120 frames\n", tail);
        if (tail > 50.0 || tail > first * 4 + 10.0)
            return Fail("the frame time after the window was recreated is not the frame time before it");
    }
    else
    {
        if (const int result = Run(hwnd, 120 + 8 * 150 + 300, true, tail))
            return result;
        const DWORD after = Handles();
        std::printf(
            "150 sizes applied; handles %lu before them, %lu after (%+ld); %.2f ms per frame at the last size\n",
            handlesBefore, after, long(after) - long(handlesBefore), tail);
        if (long(after) - long(handlesBefore) > 40)
            return Fail("the process kept handles for the sizes it went through");
    }
    DestroyWindow(hwnd);
    std::printf("PASS\n");
    std::fflush(stdout);
    return 0;
}
