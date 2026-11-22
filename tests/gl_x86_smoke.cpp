// A 32-bit OpenGL game stand-in for the x86 opengl32.dll proxy (tools\dx9-x86\proxy_gl.cpp), built into its folder
// beside the proxy and OptiScaler64\ (host, OptiScaler and a runtime). It imports opengl32.dll as a game does, so the
// loader takes the proxy from the program's folder. It draws a moving pattern (glDrawPixels on a GL 1.1 pixel
// format), swaps through gdi32's SwapBuffers (wgl: wglSwapBuffers), holds the scene still while it reads the screen
// over its window, opens and closes the menu, resizes the window and (full) makes it a borderless window over its
// monitor and back, as GL games go fullscreen, then checks the proxy's counters, the screen and the host's logs
// (tests\x86_look.h). msaa: a pixel format with 4 samples (WGL_ARB_pixel_format), which the proxy resolves.
//
//   gl_x86_smoke.exe [seconds] [wgl] [full] [msaa]
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#include "x86_look.h"

// Exported by opengl32.dll, declared by no header.
extern "C" __declspec(dllimport) BOOL WINAPI wglSwapBuffers(HDC);

using namespace x86look;
namespace fs = std::filesystem;

// A double-buffered RGBA8 pixel format with 4 samples, found through a context on a window of its own (a window's
// pixel format is set once); 0 when the driver has none.
int MultisampledFormat(const WNDCLASSW& type)
{
    const HWND window = CreateWindowW(type.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                      type.hInstance, nullptr);
    const HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                PFD_TYPE_RGBA, 32 };
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    const HGLRC context = wglCreateContext(dc);
    wglMakeCurrent(dc, context);
    using Choose = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
    const auto choose = reinterpret_cast<Choose>(wglGetProcAddress("wglChoosePixelFormatARB"));
    // WGL_DRAW_TO_WINDOW, SUPPORT_OPENGL, DOUBLE_BUFFER, PIXEL_TYPE (RGBA), COLOR_BITS, ALPHA_BITS, DEPTH_BITS,
    // SAMPLE_BUFFERS, SAMPLES.
    const int wanted[] { 0x2001, 1, 0x2010, 1, 0x2011, 1, 0x2013, 0x202B, 0x2014, 24,
                         0x201B, 8, 0x2022, 24, 0x2041, 1, 0x2042, 4, 0 };
    int format = 0;
    UINT count = 0;
    if (!choose || !choose(dc, wanted, nullptr, 1, &format, &count) || !count)
        format = 0;
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    return format;
}

int wmain(int argc, wchar_t** argv)
{
    const int seconds = argc > 1 ? _wtoi(argv[1]) : 40;
    bool wgl = false, full = false, msaa = false;
    for (int i = 2; i < argc; ++i)
    {
        wgl = wgl || !wcscmp(argv[i], L"wgl");
        full = full || !wcscmp(argv[i], L"full");
        msaa = msaa || !wcscmp(argv[i], L"msaa");
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t self[MAX_PATH] {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path dir = fs::path(self).parent_path(), logs = dir / L"OptiScaler64";
    UINT w = 1280, h = 720;
    WNDCLASSW type {};
    type.style = CS_OWNDC;
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursor(nullptr, IDC_ARROW);
    type.lpszClassName = L"GlX86Smoke";
    RegisterClassW(&type);
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    RECT r { 0, 0, LONG(w), LONG(h) };
    AdjustWindowRect(&r, style, FALSE);
    HWND window = CreateWindowW(type.lpszClassName, L"OpenGL x86", style | WS_VISIBLE, 100, 100, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, type.hInstance, nullptr);
    const int multisampled = msaa ? MultisampledFormat(type) : 0;
    if (msaa && !multisampled)
    {
        std::printf("FAIL no pixel format with 4 samples\n");
        return 2;
    }
    const HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24;
    const int format = multisampled ? multisampled : ChoosePixelFormat(dc, &pfd);
    DescribePixelFormat(dc, format, sizeof(pfd), &pfd);
    if (!SetPixelFormat(dc, format, &pfd))
    {
        std::printf("FAIL SetPixelFormat: %lu\n", GetLastError());
        return 2;
    }
    const HGLRC context = wglCreateContext(dc);
    if (!context || !wglMakeCurrent(dc, context))
    {
        std::printf("FAIL no OpenGL context: %lu\n", GetLastError());
        return 2;
    }
    // GL_SAMPLES
    GLint samples = 0;
    glGetIntegerv(0x80A9, &samples);
    std::printf("GL %s on %s, %d samples\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)), samples);
    using SwapInterval = BOOL(WINAPI*)(int);
    if (auto interval = reinterpret_cast<SwapInterval>(wglGetProcAddress("wglSwapIntervalEXT")))
        interval(1);
    const HMODULE proxy = GetModuleHandleW(L"opengl32.dll");
    wchar_t loaded[MAX_PATH] {};
    GetModuleFileNameW(proxy, loaded, MAX_PATH);
    std::printf("opengl32.dll: %ls\n", loaded);
    if (!GetProcAddress(proxy, "OptiScalerX86Status"))
    {
        std::printf("FAIL the program did not load the proxy as its opengl32.dll\n");
        return 2;
    }
    Run run { window, proxy };
    std::vector<uint32_t> pixels, rows;
    Phases at { 0.30, 0.45, 0.60, 0.72, 0.86 };
    const UINT resizedW = 1024, resizedH = 576;
    RECT before {};
    bool fullSeen = false, windowedAgain = false;
    unsigned frames = 0, scene = 0;
    const DWORD start = GetTickCount();
    for (unsigned frame = 0; at.Next(frame, start, seconds); ++frame)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (frame == at.resize)
        {
            r = { 0, 0, LONG(resizedW), LONG(resizedH) };
            AdjustWindowRect(&r, style, FALSE);
            SetWindowPos(window, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        }
        // Fullscreen as a GL game makes it without a mode change: a popup over the monitor.
        if (full && frame == at.full)
        {
            GetWindowRect(window, &before);
            MONITORINFO monitor { sizeof(monitor) };
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            const RECT& m = monitor.rcMonitor;
            SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
            SetWindowPos(window, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED);
        }
        if (full && frame == at.windowed)
        {
            SetWindowLongPtrW(window, GWL_STYLE, style | WS_VISIBLE);
            SetWindowPos(window, nullptr, before.left, before.top, before.right - before.left,
                         before.bottom - before.top, SWP_FRAMECHANGED | SWP_NOZORDER);
        }
        RECT client {};
        GetClientRect(window, &client);
        if (client.right > 0 && client.bottom > 0 && (UINT(client.right) != w || UINT(client.bottom) != h))
        {
            w = UINT(client.right);
            h = UINT(client.bottom);
            glViewport(0, 0, GLsizei(w), GLsizei(h));
            std::printf("frame %u: window now %ux%u\n", frame, w, h);
        }
        if (full && frame == at.check)
        {
            const dx9wire::ProxyStatus s = Status(proxy);
            MONITORINFO monitor { sizeof(monitor) };
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            fullSeen = LONG(s.width) == monitor.rcMonitor.right - monitor.rcMonitor.left &&
                       LONG(s.height) == monitor.rcMonitor.bottom - monitor.rcMonitor.top;
        }
        if (full && frame == at.last)
            windowedAgain = w == resizedW && h == resizedH;
        if (frame < at.still || frame >= at.look)
            ++scene;
        Scene(pixels, w, h, scene);
        // GL's rows go bottom first.
        rows.resize(pixels.size());
        for (UINT y = 0; y < h; ++y)
            std::copy_n(&pixels[size_t(y) * w], w, &rows[size_t(h - 1 - y) * w]);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glRasterPos2f(-1.f, -1.f);
        glDrawPixels(GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
        if (wgl)
            wglSwapBuffers(dc);
        else
            SwapBuffers(dc);
        ++frames;
        if (frame == at.look - 1)
            run.still = LookAt(window, pixels, w, h);
        run.Menu(frame, at.menu);
    }
    if (full)
    {
        Expect(fullSeen, "fullscreen: the host follows the window over its monitor");
        Expect(windowedAgain, "windowed again at its own size");
    }
    std::printf("%u frames in %lu ms\n", frames, GetTickCount() - start);
    const DWORD closing = GetTickCount();
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    std::printf("released in %lu ms\n", GetTickCount() - closing);
    run.Report(frames, resizedW, resizedH, logs, "opengl32-x86.log");
    return failures ? 1 : 0;
}
