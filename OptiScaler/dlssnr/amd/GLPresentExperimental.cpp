#include "pch.h"

#include "GLPresentExperimental.h"
#include "PresentExperimental.h"
#include "FgChildPresenter.h"
#include "AmdBridge.h"
#include "../backend/Selector.h"

#include <Config.h>
#include <Logger.h>
#include <Util.h>
#include <State.h>
#include <hooks/GL_Hooks.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>
#include <vector>

namespace AmdGlPresent
{
using Microsoft::WRL::ComPtr;

namespace
{
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned long long GLuint64;

constexpr GLenum GL_TEXTURE_2D_ = 0x0DE1;
constexpr GLenum GL_RGBA_ = 0x1908;
constexpr GLenum GL_UNSIGNED_BYTE_ = 0x1401;
constexpr GLenum GL_RGBA8_ = 0x8058;
constexpr GLenum GL_SAMPLES_ = 0x80A9;
constexpr GLenum GL_NEAREST_ = 0x2600;
constexpr GLenum GL_TEXTURE_MIN_FILTER_ = 0x2801;
constexpr GLenum GL_COLOR_BUFFER_BIT_ = 0x4000;
constexpr GLenum GL_FRAMEBUFFER_ = 0x8D40;
constexpr GLenum GL_READ_FRAMEBUFFER_ = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER_ = 0x8CA9;
constexpr GLenum GL_COLOR_ATTACHMENT0_ = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5;
constexpr GLenum GL_TEXTURE_TILING_EXT_ = 0x9580;
constexpr GLenum GL_DEDICATED_MEMORY_OBJECT_EXT_ = 0x9581;
constexpr GLenum GL_OPTIMAL_TILING_EXT_ = 0x9584;
constexpr GLenum GL_HANDLE_TYPE_D3D12_RESOURCE_EXT_ = 0x958A;
constexpr GLenum GL_DEVICE_LUID_EXT_ = 0x9599;

struct Gl
{
    void(WINAPI* finish)();
    GLenum(WINAPI* getError)();
    void(WINAPI* getIntegerv)(GLenum, GLint*);
    void(WINAPI* genTextures)(GLsizei, GLuint*);
    void(WINAPI* deleteTextures)(GLsizei, const GLuint*);
    void(WINAPI* bindTexture)(GLenum, GLuint);
    void(WINAPI* texParameteri)(GLenum, GLenum, GLint);
    void(WINAPI* texImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    void(WINAPI* genFramebuffers)(GLsizei, GLuint*);
    void(WINAPI* deleteFramebuffers)(GLsizei, const GLuint*);
    void(WINAPI* bindFramebuffer)(GLenum, GLuint);
    void(WINAPI* framebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum(WINAPI* checkFramebufferStatus)(GLenum);
    void(WINAPI* blitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void(WINAPI* createMemoryObjects)(GLsizei, GLuint*);
    void(WINAPI* deleteMemoryObjects)(GLsizei, const GLuint*);
    void(WINAPI* memoryObjectParameteriv)(GLuint, GLenum, const GLint*);
    void(WINAPI* importMemoryWin32Handle)(GLuint, GLuint64, GLenum, void*);
    void(WINAPI* texStorageMem2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
    void(WINAPI* getUnsignedBytev)(GLenum, unsigned char*);

    template <typename T> static bool Resolve(T& target, const char* name)
    {
        target = reinterpret_cast<T>(GLHooks::GetProc(name));
        return target != nullptr;
    }

    // getUnsignedBytev is optional: without it the D3D12 device goes on the default adapter.
    bool Load()
    {
        Resolve(getUnsignedBytev, "glGetUnsignedBytevEXT");
        return Resolve(finish, "glFinish") && Resolve(getError, "glGetError") &&
               Resolve(getIntegerv, "glGetIntegerv") && Resolve(genTextures, "glGenTextures") &&
               Resolve(deleteTextures, "glDeleteTextures") && Resolve(bindTexture, "glBindTexture") &&
               Resolve(texParameteri, "glTexParameteri") && Resolve(texImage2D, "glTexImage2D") &&
               Resolve(genFramebuffers, "glGenFramebuffers") && Resolve(deleteFramebuffers, "glDeleteFramebuffers") &&
               Resolve(bindFramebuffer, "glBindFramebuffer") &&
               Resolve(framebufferTexture2D, "glFramebufferTexture2D") &&
               Resolve(checkFramebufferStatus, "glCheckFramebufferStatus") &&
               Resolve(blitFramebuffer, "glBlitFramebuffer") &&
               Resolve(createMemoryObjects, "glCreateMemoryObjectsEXT") &&
               Resolve(deleteMemoryObjects, "glDeleteMemoryObjectsEXT") &&
               Resolve(memoryObjectParameteriv, "glMemoryObjectParameterivEXT") &&
               Resolve(importMemoryWin32Handle, "glImportMemoryWin32HandleEXT") &&
               Resolve(texStorageMem2D, "glTexStorageMem2DEXT");
    }

    bool Clean()
    {
        bool clean = true;
        for (int i = 0; i < 8 && getError() != 0; i++)
            clean = false;

        return clean;
    }
};

// The crossing is created in D3D12 and imported into OpenGL, R8G8B8A8 because the blit writes it
// through OpenGL's RGBA view. Two rules measured by the ReShade add-on's OpenGL route on AMD: the
// memory object is declared dedicated before the import, and the exported NT handle is never
// closed, because closing it faults inside the driver later. That is one handle per size the
// window settles at.
struct Route
{
    Gl gl {};
    uint32_t generation = 0;
    bool stopped = false;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Resource> shared;
    GLuint memory = 0, texture = 0, fbo = 0;
    // The back buffer with OptiScaler's menu and overlays on the NR output, for XeFG. Made when XeFG first runs.
    ComPtr<ID3D12Resource> overlay;
    GLuint overlayMemory = 0, overlayTexture = 0, overlayFbo = 0;
    bool fgPending = false;
    GLuint resolveTexture = 0, resolveFbo = 0;
    GLint samples = 0;
    uint32_t width = 0, height = 0;
    uint32_t seenWidth = 0, seenHeight = 0;
    std::chrono::steady_clock::time_point seenSince {};
    std::vector<HANDLE> keptHandles;
    AmdPresentExperimental::FgPresenter fg { "OpenGL" };
    AmdPresentExperimental::Guides guides;
    bool fgFailed = false;
    // The game's window while it is lifted one row above its monitor (see Lift), its rect before, since when
    // the lift is seen on the window, and how many times in a row the game put it back right away.
    HWND lifted = nullptr;
    RECT liftedFrom {};
    std::chrono::steady_clock::time_point liftedAt {};
    bool liftSeen = false;
    int quickPutBacks = 0;
    bool loggedFirst = false;
    uint64_t totalMicros = 0;
    uint32_t measured = 0;

    void Stop(const std::string& why)
    {
        stopped = true;
        fg.Release();
        AmdPresentExperimental::Report("OpenGL NR: " + why);
        LOG_ERROR("OpenGL final-image NR: {}; the game's image is left alone", why);
    }

    // Objects of an older OptiScaler context are left to it.
    void ReleaseGl(bool sameContext)
    {
        if (sameContext)
        {
            GLuint framebuffers[] { fbo, resolveFbo, overlayFbo };
            GLuint textures[] { texture, resolveTexture, overlayTexture };
            gl.deleteFramebuffers(3, framebuffers);
            gl.deleteTextures(3, textures);
            for (GLuint object : { memory, overlayMemory })
                if (object != 0)
                    gl.deleteMemoryObjects(1, &object);
            gl.Clean();
        }

        memory = texture = fbo = resolveTexture = resolveFbo = 0;
        overlayMemory = overlayTexture = overlayFbo = 0;
        overlay.Reset();
        fgPending = false;
        width = height = 0;
    }

    bool CreateDevice()
    {
        ComPtr<IDXGIAdapter1> adapter;
        unsigned char luid[8] {};
        if (gl.getUnsignedBytev != nullptr)
        {
            gl.getUnsignedBytev(GL_DEVICE_LUID_EXT_, luid);
            ComPtr<IDXGIFactory1> factory;
            if (gl.Clean() && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            {
                for (UINT i = 0;; ++i)
                {
                    ComPtr<IDXGIAdapter1> candidate;
                    if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND)
                        break;

                    DXGI_ADAPTER_DESC1 description {};
                    if (SUCCEEDED(candidate->GetDesc1(&description)) &&
                        std::memcmp(&description.AdapterLuid, luid, sizeof(luid)) == 0)
                    {
                        adapter = candidate;
                        break;
                    }
                }
            }
        }

        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            return false;

        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
            return false;

        LOG_INFO("OpenGL final-image NR: D3D12 device on {} adapter", adapter ? "the context's" : "the default");
        return true;
    }

    // A D3D12 texture with its OpenGL view and a framebuffer on that view. Returns its size in bytes, 0 on failure.
    uint64_t Cross(uint32_t w, uint32_t h, ComPtr<ID3D12Resource>& resource, GLuint& object, GLuint& view,
                   GLuint& framebuffer)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = w;
        description.Height = h;
        description.DepthOrArraySize = description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;

        HANDLE handle = nullptr;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &description,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))) ||
            FAILED(device->CreateSharedHandle(resource.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
        {
            LOG_ERROR("OpenGL final-image NR: shared D3D12 texture {}x{} failed", w, h);
            return 0;
        }

        keptHandles.push_back(handle);
        const auto bytes = device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;

        gl.Clean();
        gl.createMemoryObjects(1, &object);
        const GLint dedicated = 1;
        gl.memoryObjectParameteriv(object, GL_DEDICATED_MEMORY_OBJECT_EXT_, &dedicated);
        gl.importMemoryWin32Handle(object, bytes, GL_HANDLE_TYPE_D3D12_RESOURCE_EXT_, handle);
        if (object == 0 || !gl.Clean())
        {
            LOG_ERROR("OpenGL final-image NR: importing the D3D12 texture was refused");
            return 0;
        }

        gl.genTextures(1, &view);
        gl.bindTexture(GL_TEXTURE_2D_, view);
        gl.texParameteri(GL_TEXTURE_2D_, GL_TEXTURE_TILING_EXT_, static_cast<GLint>(GL_OPTIMAL_TILING_EXT_));
        gl.texStorageMem2D(GL_TEXTURE_2D_, 1, GL_RGBA8_, static_cast<GLsizei>(w), static_cast<GLsizei>(h), object, 0);
        gl.bindTexture(GL_TEXTURE_2D_, 0);
        gl.genFramebuffers(1, &framebuffer);
        gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, framebuffer);
        gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, view, 0);
        const auto status = gl.checkFramebufferStatus(GL_DRAW_FRAMEBUFFER_);
        gl.bindFramebuffer(GL_FRAMEBUFFER_, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE_ || !gl.Clean())
        {
            LOG_ERROR("OpenGL final-image NR: the imported texture cannot be rendered to ({:X})", status);
            return 0;
        }

        return bytes;
    }

    bool Build(uint32_t w, uint32_t h)
    {
        // The D3D12 route sizes its textures on the first frame it sees.
        if (shared)
            AmdPresentExperimental::BeforeResize();

        fg.Release();
        ReleaseGl(true);
        shared.Reset();

        const auto bytes = Cross(w, h, shared, memory, texture, fbo);
        if (bytes == 0)
            return false;

        // A multisampled default framebuffer is resolved first: one blit cannot resolve and flip.
        gl.getIntegerv(GL_SAMPLES_, &samples);
        if (samples > 1)
        {
            gl.genTextures(1, &resolveTexture);
            gl.bindTexture(GL_TEXTURE_2D_, resolveTexture);
            gl.texParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, static_cast<GLint>(GL_NEAREST_));
            gl.texImage2D(GL_TEXTURE_2D_, 0, static_cast<GLint>(GL_RGBA8_), static_cast<GLsizei>(w),
                          static_cast<GLsizei>(h), 0, GL_RGBA_, GL_UNSIGNED_BYTE_, nullptr);
            gl.bindTexture(GL_TEXTURE_2D_, 0);
            gl.genFramebuffers(1, &resolveFbo);
            gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, resolveFbo);
            gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, resolveTexture, 0);
            gl.bindFramebuffer(GL_FRAMEBUFFER_, 0);
            if (!gl.Clean())
            {
                LOG_ERROR("OpenGL final-image NR: the resolve target for {} samples failed", samples);
                return false;
            }
        }

        width = w;
        height = h;
        LOG_INFO("OpenGL final-image NR: back buffer {}x{} ({} samples) crosses as a D3D12 texture of {} bytes; {} "
                 "shared handles kept",
                 w, h, samples, bytes, keptHandles.size());
        return true;
    }

    // Y is flipped both ways: OpenGL's origin is bottom left, the D3D12 route's top left.
    bool BlitIn(GLuint target)
    {
        const auto w = static_cast<GLint>(width), h = static_cast<GLint>(height);
        gl.bindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
        if (resolveFbo != 0)
        {
            gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, resolveFbo);
            gl.blitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
            gl.bindFramebuffer(GL_READ_FRAMEBUFFER_, resolveFbo);
        }

        gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, target);
        gl.blitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
        gl.bindFramebuffer(GL_FRAMEBUFFER_, 0);

        // D3D12 reads the texture next; the CPU wait is the hand-over.
        gl.finish();
        return gl.Clean();
    }

    bool BlitOut()
    {
        const auto w = static_cast<GLint>(width), h = static_cast<GLint>(height);
        gl.bindFramebuffer(GL_READ_FRAMEBUFFER_, fbo);
        gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
        gl.blitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
        gl.bindFramebuffer(GL_FRAMEBUFFER_, 0);
        return gl.Clean();
    }
};

Route route;

void MoveWindow(HWND window, const RECT& rect)
{
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    // The window's own thread may be waiting for this one.
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())
        flags |= SWP_ASYNCWINDOWPOS;
    SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, flags);
}

// AMD's driver flips some games' OpenGL windows that cover the whole monitor straight to the display, past
// DWM (Wolfenstein: The Old Blood, by its profile), and XeFG's child window never reaches the screen. A
// window one row taller, starting one row above the monitor, no longer has the monitor's size: the driver
// leaves it to DWM and the extra row stays off screen. It is lifted while XeFG shows, and put back when
// frame generation stops. Not needed when the driver presents the window through DXGI itself. A game puts
// its window back at the monitor's size now and then (back from alt-tab, a loading screen); it is lifted
// again then, unless the game undoes every lift at once.
void Lift(Route& r, HWND hwnd, bool shown, bool wanted)
{
    RECT now {};
    if (r.lifted && (!IsWindow(r.lifted) || !GetWindowRect(r.lifted, &now)))
        r.lifted = nullptr;

    const auto time = std::chrono::steady_clock::now();
    if (r.lifted && !r.liftSeen)
    {
        // A move from another thread lands later; one that never lands is tried again.
        if (now.top == r.liftedFrom.top - 1 && now.bottom == r.liftedFrom.bottom)
        {
            r.liftSeen = true;
            r.liftedAt = time;
        }
        else if (time - r.liftedAt > std::chrono::seconds(2))
            r.lifted = nullptr;
    }
    else if (r.lifted && EqualRect(&now, &r.liftedFrom))
    {
        r.lifted = nullptr;
        r.quickPutBacks = time - r.liftedAt < std::chrono::seconds(1) ? r.quickPutBacks + 1 : 0;
        if (r.quickPutBacks == 5)
            LOG_WARN("OpenGL XeFG: the game puts its window back at the monitor's size at once; it is left there");
        else
            LOG_INFO("OpenGL XeFG: the game put its window back at the monitor's size");
    }

    if (r.lifted && !wanted)
    {
        if (now.top == r.liftedFrom.top - 1 && now.bottom == r.liftedFrom.bottom)
            MoveWindow(r.lifted, r.liftedFrom);
        LOG_INFO("OpenGL XeFG: the game window is back at its monitor's size");
        r.lifted = nullptr;
        return;
    }

    if (r.lifted || !shown || r.quickPutBacks >= 5 || GLHooks::DriverPresentsThroughDxgi())
        return;

    const HWND root = GetAncestor(hwnd, GA_ROOT);
    MONITORINFO monitor { sizeof(monitor) };
    if (!root || !GetWindowRect(root, &now) ||
        !GetMonitorInfoW(MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST), &monitor) ||
        !EqualRect(&now, &monitor.rcMonitor))
        return;

    RECT taller = now;
    taller.top -= 1;
    MoveWindow(root, taller);
    r.lifted = root;
    r.liftedFrom = now;
    r.liftedAt = time;
    r.liftSeen = false;
    LOG_INFO("OpenGL XeFG: the game window covers its monitor and the driver may flip it past XeFG's window; "
             "lifted one row above the monitor");
}
} // namespace

// XeFG asked for on the OpenGL route. With NR off it keeps running on the optical-flow guides.
bool FgWanted()
{
    return Config::Instance()->FGEnabled.value_or_default() && State::Instance().activeFgInput == FGInput::Upscaler &&
           State::Instance().activeFgOutput == FGOutput::XeFG;
}

bool Wanted()
{
    auto config = Config::Instance();
    const bool neural = config->DlssNrEnabled.value_or_default();
    const bool wanted = (neural || FgWanted()) && config->DlssNrPresent.value_or_default() && !route.stopped &&
                        DlssNr::Backend::PresentRuntimeSupported();

    static bool neuralBefore = true;
    if (config->DlssNrPresent.value_or_default() && neural != neuralBefore)
    {
        neuralBefore = neural;
        LOG_INFO("OpenGL final-image route: NR {}", neural       ? "on"
                                                    : FgWanted() ? "off, XeFG runs on its guides"
                                                                 : "off");
    }

    // Generated frames end with the route; the child window must not keep showing the last one.
    if (!wanted && route.fg.window && (route.fg.enabled || route.fg.visible))
    {
        route.fg.Pause();
        LOG_INFO("OpenGL XeFG paused while final-image NR is off");
    }
    if (!wanted && route.lifted)
        Lift(route, nullptr, false, false);

    return wanted;
}

void Process(HWND hwnd, uint32_t width, uint32_t height, uint32_t generation)
{
    auto& r = route;
    if (r.stopped)
        return;

    if (r.generation != generation)
    {
        if (r.generation != 0)
            r.ReleaseGl(false);

        r.generation = generation;
        if (!r.gl.Load())
        {
            r.Stop("the OpenGL driver has no EXT_memory_object_win32");
            return;
        }
    }

    if (!r.device && !r.CreateDevice())
    {
        r.Stop("D3D12 device unavailable");
        return;
    }

    // A window being resized presents at a new size every few frames. The crossing, and XeFG with it,
    // is built again once the size has held for 300 ms; until then the game's image is left alone.
    const auto now = std::chrono::steady_clock::now();
    if (r.seenWidth != width || r.seenHeight != height)
    {
        r.seenWidth = width;
        r.seenHeight = height;
        r.seenSince = now;
    }

    if (r.fbo != 0 && (r.width != width || r.height != height) && now - r.seenSince < std::chrono::milliseconds(300))
    {
        r.fg.Pause();
        return;
    }

    if ((r.fbo == 0 || r.width != width || r.height != height) && !r.Build(width, height))
    {
        r.Stop("the back buffer could not cross to D3D12");
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    if (!r.BlitIn(r.fbo))
    {
        r.Stop("the blit out of the back buffer failed");
        return;
    }

    auto settings =
        DlssNr::AmdBridge::SettingsFromConfig(*Config::Instance(), Config::Instance()->AmdNrScale.value_or_default());
    settings.spinDraw = 0;
    const bool neural = Config::Instance()->DlssNrEnabled.value_or_default();
    if (!AmdPresentExperimental::RenderResource(r.shared.Get(), r.device.Get(), r.queue.Get(),
                                                Util::DllPath().parent_path(), settings, &r.guides, neural))
    {
        r.Stop(AmdPresentExperimental::Status());
        return;
    }

    // With NR off the game's image is left as it was; only XeFG's guides were made.
    if (neural && !r.BlitOut())
    {
        r.Stop("the blit back into the back buffer failed");
        return;
    }
    r.fgPending = true;

    if (!r.loggedFirst)
    {
        r.loggedFirst = true;
        LOG_INFO("OpenGL final-image NR: first frame processed at {}x{}", r.width, r.height);
    }

    r.totalMicros += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    if (++r.measured == 120)
    {
        LOG_INFO("OpenGL final-image NR: {:.2f} ms average over 120 presents; {}", r.totalMicros / 120000.0,
                 AmdPresentExperimental::Status());
        r.measured = 0;
        r.totalMicros = 0;
    }
}

void PresentGenerated(HWND hwnd)
{
    auto& r = route;
    if (!r.fgPending || r.stopped)
        return;
    r.fgPending = false;

    // The overlay crossing is made the first time XeFG wants a frame at this size.
    const bool wantFg = FgWanted();
    if (wantFg && !r.fgFailed && !r.overlay &&
        r.Cross(r.width, r.height, r.overlay, r.overlayMemory, r.overlayTexture, r.overlayFbo) == 0)
    {
        r.fgFailed = true;
        LOG_ERROR("OpenGL XeFG: the overlay texture failed; NR will continue");
    }
    const bool overlaid = wantFg && !r.fgFailed;
    if (overlaid && !r.BlitIn(r.overlayFbo))
    {
        r.Stop("the blit of the overlay failed");
        return;
    }
    r.fg.Step(hwnd, r.device.Get(), r.queue.Get(), overlaid ? r.overlay.Get() : r.shared.Get(), r.shared.Get(),
              r.guides, r.fgFailed);
    Lift(r, hwnd, r.fg.window && r.fg.visible, wantFg && !r.fgFailed);
}
} // namespace AmdGlPresent
