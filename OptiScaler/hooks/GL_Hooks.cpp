#include "pch.h"

#include "GL_Hooks.h"

#include <Config.h>
#include <Logger.h>
#include <State.h>

#include <menu/menu_overlay_gl.h>
#include <dlssnr/amd/GLPresentExperimental.h>
#include <misc/FrameLimit.h>
#include <misc/Screenshot.h>

#include <detours/detours.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

#include "Hook_Utils.h"

typedef BOOL(WINAPI* PFN_wglSwapBuffers)(HDC);
typedef BOOL(WINAPI* PFN_wglSwapLayerBuffers)(HDC, UINT);
typedef HGLRC(WINAPI* PFN_wglCreateContext)(HDC);
typedef BOOL(WINAPI* PFN_wglMakeCurrent)(HDC, HGLRC);
typedef HGLRC(WINAPI* PFN_wglGetCurrentContext)();
typedef HDC(WINAPI* PFN_wglGetCurrentDC)();
typedef PROC(WINAPI* PFN_wglGetProcAddress)(LPCSTR);
typedef BOOL(WINAPI* PFN_wglSwapIntervalEXT)(int);
typedef int(WINAPI* PFN_wglGetSwapIntervalEXT)();

static HMODULE _module = nullptr;
static PFN_wglSwapBuffers o_wglSwapBuffers = nullptr;
static PFN_wglSwapLayerBuffers o_wglSwapLayerBuffers = nullptr;
static PFN_wglCreateContext o_wglCreateContext = nullptr;
static PFN_wglMakeCurrent o_wglMakeCurrent = nullptr;
static PFN_wglGetCurrentContext o_wglGetCurrentContext = nullptr;
static PFN_wglGetCurrentDC o_wglGetCurrentDC = nullptr;
static PFN_wglGetProcAddress o_wglGetProcAddress = nullptr;

// OptiScaler draws in a context of its own, created on the game's device context. The default
// framebuffer belongs to the window, so both contexts see the same back buffer, while the game's
// bindings and state stay untouched and nothing OptiScaler creates dies with a context the game
// deletes. A device context with another pixel format gets a new context; the old one is left
// alive, and generation tells the menu and the NR route to rebuild their objects.
struct OverlayContext
{
    HGLRC context = nullptr;
    HGLRC gameContext = nullptr;
    HDC gameDc = nullptr;
    uint32_t generation = 0;
    bool current = false;
    bool failed = false;

    bool Enter(HDC hdc)
    {
        if (current)
            return true;

        if (failed)
            return false;

        gameContext = o_wglGetCurrentContext();
        gameDc = o_wglGetCurrentDC();

        if (context == nullptr || !o_wglMakeCurrent(hdc, context))
        {
            auto created = o_wglCreateContext(hdc);
            if (created == nullptr || !o_wglMakeCurrent(hdc, created))
            {
                // Without a context of its own OptiScaler stays out of OpenGL for the session.
                LOG_ERROR("OpenGL: OptiScaler context unavailable ({}); menu and NR are off", GetLastError());
                o_wglMakeCurrent(gameDc, gameContext);
                failed = true;
                return false;
            }

            context = created;
            generation++;
            LOG_INFO("OpenGL: OptiScaler context {:X} created on the game's device context", (size_t) context);
        }

        current = true;
        return true;
    }

    void Leave()
    {
        if (!current)
            return;

        o_wglMakeCurrent(gameDc, gameContext);
        current = false;
    }
};

static OverlayContext _overlay;
static thread_local bool _inPresent = false;
static std::atomic<HWND> _gameWindow = nullptr;

// The menu, NR, XeFG and the forced swap interval follow one window: the first that presents, until it
// is destroyed or hidden. Presents to other windows of the game (a second view, a debug window) pass
// through untouched.
static bool IsGameWindow(HWND hwnd)
{
    HWND bound = _gameWindow.load();
    if (bound == hwnd)
        return true;

    if (bound != nullptr && IsWindow(bound) && IsWindowVisible(bound))
        return false;

    if (!_gameWindow.compare_exchange_strong(bound, hwnd))
        return false;

    LOG_INFO("OpenGL: OptiScaler follows the presents to window {:X}; other windows pass through", (size_t) hwnd);
    return true;
}

// The menu's V-Sync settings, as on DXGI: off is interval 0, on is the chosen interval, at least 1.
// The swap interval belongs to the game's context, which is current here. The game's own interval is
// kept from the first time it is forced and given back once the setting is reset.
static void ApplyVsync(HDC hdc)
{
    auto config = Config::Instance();
    static int gameInterval = -1;
    if ((!config->ForceVsync.has_value() && gameInterval < 0) || o_wglGetCurrentDC() != hdc)
        return;

    static auto setInterval = (PFN_wglSwapIntervalEXT) GLHooks::GetProc("wglSwapIntervalEXT");
    static auto getInterval = (PFN_wglGetSwapIntervalEXT) GLHooks::GetProc("wglGetSwapIntervalEXT");
    if (setInterval == nullptr || getInterval == nullptr)
        return;

    if (!config->ForceVsync.has_value())
    {
        if (setInterval(gameInterval))
            LOG_INFO("OpenGL: swap interval back to the game's {}", gameInterval);
        gameInterval = -1;
        return;
    }

    if (gameInterval < 0)
        gameInterval = getInterval();

    const int interval =
        config->ForceVsync.value() ? std::max(1, static_cast<int>(config->VsyncInterval.value_or_default())) : 0;
    static int logged = -1;
    if (getInterval() != interval && setInterval(interval) && logged != interval)
    {
        logged = interval;
        LOG_INFO("OpenGL: swap interval set to {}", interval);
    }
}

// True for a present to the game's window, the one OptiScaler follows.
static bool Present(HDC hdc)
{
    if (_inPresent)
        return false;

    HWND hwnd = WindowFromDC(hdc);
    if (hwnd == nullptr || !IsGameWindow(hwnd))
        return false;

    RECT rect {};
    if (IsIconic(hwnd) || !GetClientRect(hwnd, &rect) || rect.right <= 0 || rect.bottom <= 0)
        return true;

    _inPresent = true;
    State::Instance().swapchainApi = OpenGL;

    const auto width = static_cast<uint32_t>(rect.right);
    const auto height = static_cast<uint32_t>(rect.bottom);
    const bool nr = AmdGlPresent::Wanted();
    const bool setup = MenuOverlayGl::NeedsContext(hwnd);

    if ((nr || setup) && _overlay.Enter(hdc))
    {
        if (nr)
            AmdGlPresent::Process(hwnd, width, height, _overlay.generation);

        if (setup)
            MenuOverlayGl::Setup(hwnd, _overlay.generation);
    }

    // After NR, before the menu: the screenshot copy, or reading back the one made on an earlier frame.
    if (Screenshot::GlWanted() && _overlay.Enter(hdc))
        Screenshot::Gl(width, height, _overlay.generation);

    // The menu frame is built on the CPU; the overlay context is needed only to draw it.
    if (MenuOverlayGl::Frame() && _overlay.Enter(hdc))
        MenuOverlayGl::Draw(_overlay.generation);

    // The same frame with the menu on it, when the screenshot asked for that copy.
    if (Screenshot::GlMenuWanted() && _overlay.Enter(hdc))
        Screenshot::GlMenu(width, height, _overlay.generation);

    // XeFG gets the back buffer with the menu on it, so the menu shows over the generated frames.
    if (nr && _overlay.Enter(hdc))
        AmdGlPresent::PresentGenerated(hwnd);

    _overlay.Leave();
    ApplyVsync(hdc);
    _inPresent = false;
    return true;
}

static void AfterPresent()
{
    if (!State::Instance().reflexLimitsFps)
        FrameLimit::sleep(false);
}

VALIDATE_HOOK(hkwglSwapBuffers, PFN_wglSwapBuffers)
static BOOL WINAPI hkwglSwapBuffers(HDC hdc)
{
    const bool game = Present(hdc);
    auto result = o_wglSwapBuffers(hdc);
    if (game)
        AfterPresent();
    return result;
}

VALIDATE_HOOK(hkwglSwapLayerBuffers, PFN_wglSwapLayerBuffers)
static BOOL WINAPI hkwglSwapLayerBuffers(HDC hdc, UINT planes)
{
    const bool game = (planes & WGL_SWAP_MAIN_PLANE) && Present(hdc);
    auto result = o_wglSwapLayerBuffers(hdc, planes);
    if (game)
        AfterPresent();

    return result;
}

PROC GLHooks::GetProc(const char* name)
{
    PROC proc = o_wglGetProcAddress != nullptr ? o_wglGetProcAddress(name) : nullptr;

    // wglGetProcAddress may answer 1, 2, 3 or -1 for functions it does not know
    const auto value = reinterpret_cast<intptr_t>(proc);
    if (value >= -1 && value <= 3)
        proc = _module != nullptr ? GetProcAddress(_module, name) : nullptr;

    return proc;
}

static std::atomic<bool> _driverPresents = false;

bool GLHooks::DriverPresentsThroughDxgi() { return _driverPresents; }

bool GLHooks::CalledFromDriver()
{
    // Nothing to look for in a process without OpenGL.
    if (_module == nullptr)
        return false;

    static std::mutex mutex;
    static std::unordered_map<HMODULE, bool> drivers;

    void* frames[48];
    const USHORT count = CaptureStackBackTrace(1, 48, frames, nullptr);
    std::lock_guard lock(mutex);
    for (USHORT i = 0; i < count; ++i)
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(frames[i]), &module))
            continue;
        auto known = drivers.find(module);
        if (known == drivers.end())
            known = drivers.emplace(module, GetProcAddress(module, "DrvSwapBuffers") != nullptr).first;
        if (known->second)
        {
            _driverPresents = true;
            return true;
        }
    }
    return false;
}

void GLHooks::Hook(HMODULE opengl32)
{
    if (o_wglSwapBuffers != nullptr)
        return;

    _module = opengl32;
    o_wglSwapBuffers = (PFN_wglSwapBuffers) GetProcAddress(opengl32, "wglSwapBuffers");
    o_wglSwapLayerBuffers = (PFN_wglSwapLayerBuffers) GetProcAddress(opengl32, "wglSwapLayerBuffers");
    o_wglCreateContext = (PFN_wglCreateContext) GetProcAddress(opengl32, "wglCreateContext");
    o_wglMakeCurrent = (PFN_wglMakeCurrent) GetProcAddress(opengl32, "wglMakeCurrent");
    o_wglGetCurrentContext = (PFN_wglGetCurrentContext) GetProcAddress(opengl32, "wglGetCurrentContext");
    o_wglGetCurrentDC = (PFN_wglGetCurrentDC) GetProcAddress(opengl32, "wglGetCurrentDC");
    o_wglGetProcAddress = (PFN_wglGetProcAddress) GetProcAddress(opengl32, "wglGetProcAddress");

    if (o_wglSwapBuffers == nullptr || o_wglCreateContext == nullptr || o_wglMakeCurrent == nullptr ||
        o_wglGetCurrentContext == nullptr || o_wglGetCurrentDC == nullptr || o_wglGetProcAddress == nullptr)
    {
        LOG_ERROR("OpenGL: opengl32.dll is missing wgl entry points");
        o_wglSwapBuffers = nullptr;
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    DetourAttach(&(PVOID&) o_wglSwapBuffers, hkwglSwapBuffers);

    if (o_wglSwapLayerBuffers != nullptr)
        DetourAttach(&(PVOID&) o_wglSwapLayerBuffers, hkwglSwapLayerBuffers);

    auto result = DetourTransactionCommit();
    if (result != NO_ERROR)
        LOG_ERROR("OpenGL: hooking wglSwapBuffers failed ({:X})", result);
    else
        LOG_INFO("OpenGL: wglSwapBuffers hooked");
}
