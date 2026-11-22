// 32-bit opengl32.dll for the OpenGL games OptiScaler.dll (x64 only) cannot load into. It forwards every export to the
// system opengl32.dll and, at each wglSwapBuffers (gdi32's SwapBuffers comes here too), hands the default framebuffer
// to the same 64-bit host as the D3D9 proxy (host.cpp, through bridge.h), which runs OptiScaler on a D3D11 swap chain
// on a child of the game's window. The frame crosses through a D3D11 texture of a device of ours on the context's GPU,
// shared by handle and imported into the game's context (GL_EXT_memory_object_win32, its KMT handle type: measured on
// an RX 9070 XT, Adrenalin 26.9.2, in a 32-bit process); without that, it is read back into a section of memory.
// Nothing is ever imported from opengl32.dll here: that name is this module.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

namespace
{
constexpr wchar_t LogName[] = L"opengl32-x86.log", Section[] = L"GlX86", SurfaceName[] = L"OptiScalerGlX86Surface",
                  PipePrefix[] = L"\\\\.\\pipe\\opti-gl-x86-";
}
#include "bridge.h"

namespace
{
HMODULE systemGl = nullptr;
INIT_ONCE systemOnce = INIT_ONCE_STATIC_INIT;

// No CRT here: Windows compatibility shims (AcGenral) call the forwarded exports while the process starts, before
// this DLL's CRT is up; a std::wstring there recursed until the stack ran out (Tomb Raider 2013)
BOOL CALLBACK LoadSystem(PINIT_ONCE, PVOID, PVOID*)
{
    wchar_t path[MAX_PATH + 16] {};
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return TRUE;
    lstrcatW(path, L"\\opengl32.dll");
    systemGl = LoadLibraryW(path);
    return TRUE;
}
FARPROC SystemProc(const char* name)
{
    InitOnceExecuteOnce(&systemOnce, LoadSystem, nullptr, nullptr);
    return systemGl ? GetProcAddress(systemGl, name) : nullptr;
}

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLubyte = unsigned char;
using GLuint64 = unsigned long long;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_RGBA = 0x1908, GL_RGBA8 = 0x8058, GL_UNSIGNED_BYTE = 0x1401,
                 GL_SCISSOR_TEST = 0x0C11, GL_FRAMEBUFFER_SRGB = 0x8DB9, GL_READ_BUFFER = 0x0C02, GL_BACK = 0x0405,
                 GL_COLOR_BUFFER_BIT = 0x4000, GL_NEAREST = 0x2600, GL_READ_FRAMEBUFFER = 0x8CA8,
                 GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_READ_FRAMEBUFFER_BINDING = 0x8CAA,
                 GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
                 GL_SAMPLES = 0x80A9, GL_PACK_ALIGNMENT = 0x0D05, GL_PACK_ROW_LENGTH = 0x0D02,
                 GL_PACK_SKIP_ROWS = 0x0D03, GL_PACK_SKIP_PIXELS = 0x0D04, GL_PIXEL_PACK_BUFFER = 0x88EB,
                 GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED, GL_TEXTURE_BINDING_2D = 0x8069,
                 GL_DEDICATED_MEMORY_OBJECT_EXT = 0x9581, GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT = 0x958C,
                 GL_DEVICE_LUID_EXT = 0x9599;

// The GL entry points the bridge uses, from the system opengl32.dll (GL 1.1 and wgl) and from the context.
struct Gl
{
    HGLRC(WINAPI* getCurrentContext)() = nullptr;
    PROC(WINAPI* getProcAddress)(LPCSTR) = nullptr;
    BOOL(WINAPI* swapBuffers)(HDC) = nullptr;
    BOOL(WINAPI* swapLayerBuffers)(HDC, UINT) = nullptr;
    GLenum(APIENTRY* getError)() = nullptr;
    void(APIENTRY* getIntegerv)(GLenum, GLint*) = nullptr;
    unsigned char(APIENTRY* isEnabled)(GLenum) = nullptr;
    void(APIENTRY* enable)(GLenum) = nullptr;
    void(APIENTRY* disable)(GLenum) = nullptr;
    void(APIENTRY* readBuffer)(GLenum) = nullptr;
    void(APIENTRY* pixelStorei)(GLenum, GLint) = nullptr;
    void(APIENTRY* readPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void(APIENTRY* genTextures)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* deleteTextures)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* bindTexture)(GLenum, GLuint) = nullptr;
    void(APIENTRY* finish)() = nullptr;
    // Per context: framebuffers (GL 3.0), the pack buffer (GL 2.1), memory objects (GL_EXT_memory_object_win32) and
    // the swap interval (WGL_EXT_swap_control).
    void(APIENTRY* genFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* deleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* bindFramebuffer)(GLenum, GLuint) = nullptr;
    void(APIENTRY* framebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    GLenum(APIENTRY* checkFramebufferStatus)(GLenum) = nullptr;
    void(APIENTRY* blitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, unsigned, GLenum) = nullptr;
    void(APIENTRY* bindBuffer)(GLenum, GLuint) = nullptr;
    void(APIENTRY* texStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei) = nullptr;
    void(APIENTRY* getUnsignedBytev)(GLenum, GLubyte*) = nullptr;
    void(APIENTRY* createMemoryObjects)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* deleteMemoryObjects)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* memoryObjectParameteriv)(GLuint, GLenum, const GLint*) = nullptr;
    void(APIENTRY* importMemoryWin32Handle)(GLuint, GLuint64, GLenum, void*) = nullptr;
    void(APIENTRY* texStorageMem2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64) = nullptr;
    int(WINAPI* getSwapInterval)() = nullptr;
    HGLRC loadedFor = nullptr;

    template <class T> void System(const char* name, T& out) { out = reinterpret_cast<T>(SystemProc(name)); }
    // wglGetProcAddress may answer 1, 2, 3 or -1 for functions it does not know.
    template <class T> void Context(const char* name, T& out)
    {
        const auto proc = getProcAddress(name);
        const auto value = reinterpret_cast<intptr_t>(proc);
        out = value >= -1 && value <= 3 ? reinterpret_cast<T>(SystemProc(name)) : reinterpret_cast<T>(proc);
    }
    bool Load()
    {
        if (getCurrentContext)
            return true;
        System("wglGetProcAddress", getProcAddress);
        System("wglSwapBuffers", swapBuffers);
        System("wglSwapLayerBuffers", swapLayerBuffers);
        System("glGetError", getError);
        System("glGetIntegerv", getIntegerv);
        System("glIsEnabled", isEnabled);
        System("glEnable", enable);
        System("glDisable", disable);
        System("glReadBuffer", readBuffer);
        System("glPixelStorei", pixelStorei);
        System("glReadPixels", readPixels);
        System("glGenTextures", genTextures);
        System("glDeleteTextures", deleteTextures);
        System("glBindTexture", bindTexture);
        System("glFinish", finish);
        if (!getProcAddress || !swapBuffers || !getError || !getIntegerv || !isEnabled || !enable || !disable ||
            !readBuffer || !pixelStorei || !readPixels || !genTextures || !deleteTextures || !bindTexture || !finish)
            return false;
        System("wglGetCurrentContext", getCurrentContext);
        return getCurrentContext != nullptr;
    }
    // With the context current; a pointer taken from one context is not promised to work in another.
    void LoadFor(HGLRC context)
    {
        if (loadedFor == context)
            return;
        loadedFor = context;
        Context("glGenFramebuffers", genFramebuffers);
        Context("glDeleteFramebuffers", deleteFramebuffers);
        Context("glBindFramebuffer", bindFramebuffer);
        Context("glFramebufferTexture2D", framebufferTexture2D);
        Context("glCheckFramebufferStatus", checkFramebufferStatus);
        Context("glBlitFramebuffer", blitFramebuffer);
        Context("glBindBuffer", bindBuffer);
        Context("glTexStorage2D", texStorage2D);
        Context("glGetUnsignedBytevEXT", getUnsignedBytev);
        Context("glCreateMemoryObjectsEXT", createMemoryObjects);
        Context("glDeleteMemoryObjectsEXT", deleteMemoryObjects);
        Context("glMemoryObjectParameterivEXT", memoryObjectParameteriv);
        Context("glImportMemoryWin32HandleEXT", importMemoryWin32Handle);
        Context("glTexStorageMem2DEXT", texStorageMem2D);
        Context("wglGetSwapIntervalEXT", getSwapInterval);
    }
    bool Framebuffers() const
    {
        return genFramebuffers && deleteFramebuffers && bindFramebuffer && framebufferTexture2D &&
               checkFramebufferStatus && blitFramebuffer;
    }
    bool Memory() const
    {
        return Framebuffers() && createMemoryObjects && deleteMemoryObjects && memoryObjectParameteriv &&
               importMemoryWin32Handle && texStorageMem2D;
    }
    void Drain()
    {
        for (int i = 0; i < 16 && getError(); ++i)
        {
        }
    }
} gl;

// The parts of the context a copy out of the default framebuffer touches, put back after it.
struct Saved
{
    GLint readFbo = 0, drawFbo = 0, readBuffer = GL_BACK, packBuffer = 0, alignment = 4, rowLength = 0, skipRows = 0,
          skipPixels = 0, texture = 0;
    bool scissor = false, srgb = false;

    Saved()
    {
        gl.Drain();
        if (gl.bindFramebuffer)
        {
            gl.getIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
            gl.getIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
            gl.bindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        }
        gl.getIntegerv(GL_READ_BUFFER, &readBuffer);
        gl.getIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        scissor = gl.isEnabled(GL_SCISSOR_TEST) != 0;
        srgb = gl.isEnabled(GL_FRAMEBUFFER_SRGB) != 0;
        gl.disable(GL_SCISSOR_TEST);
        gl.disable(GL_FRAMEBUFFER_SRGB);
        gl.readBuffer(GL_BACK);
        gl.Drain();
    }
    ~Saved()
    {
        if (gl.bindFramebuffer)
            gl.bindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        gl.readBuffer(static_cast<GLenum>(readBuffer));
        if (gl.bindFramebuffer)
        {
            gl.bindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
            gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
        }
        gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
        (scissor ? gl.enable : gl.disable)(GL_SCISSOR_TEST);
        (srgb ? gl.enable : gl.disable)(GL_FRAMEBUFFER_SRGB);
        gl.Drain();
    }
    // glReadPixels into client memory: no pack buffer, tight rows.
    void Pack()
    {
        gl.getIntegerv(GL_PACK_ALIGNMENT, &alignment);
        gl.getIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
        gl.getIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
        gl.getIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
        if (gl.bindBuffer)
        {
            gl.getIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
            gl.bindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
        gl.pixelStorei(GL_PACK_ALIGNMENT, 4);
        gl.pixelStorei(GL_PACK_ROW_LENGTH, 0);
        gl.pixelStorei(GL_PACK_SKIP_ROWS, 0);
        gl.pixelStorei(GL_PACK_SKIP_PIXELS, 0);
        gl.Drain();
    }
    void Unpack()
    {
        gl.pixelStorei(GL_PACK_ALIGNMENT, alignment);
        gl.pixelStorei(GL_PACK_ROW_LENGTH, rowLength);
        gl.pixelStorei(GL_PACK_SKIP_ROWS, skipRows);
        gl.pixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
        if (gl.bindBuffer)
            gl.bindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
    }
};

// The D3D11 device the shared texture is made on, on the context's GPU. Kept for the session: a new size or a new
// stage reuses it. Never destroyed, as the bridge: nothing is released from DllMain at process exit.
ComPtr<ID3D11Device>& d3d11 = *new ComPtr<ID3D11Device>;
LUID d3d11Luid {};

bool AdapterFor(const LUID& luid, ComPtr<IDXGIAdapter1>& adapter)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
    {
        DXGI_ADAPTER_DESC1 desc {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == luid.LowPart &&
            desc.AdapterLuid.HighPart == luid.HighPart)
            return true;
    }
    adapter.Reset();
    return false;
}

// The OpenGL side of the bridge (bridge.h). The window stands for the device: the context presenting to it may change,
// its DC may be a new one each frame.
struct OpenGL
{
    using Device = HWND__;
    using Back = HDC__;
    static constexpr const char* Name = "OpenGL";

    static HWND Window(HWND window, HWND& focus)
    {
        focus = window;
        return window;
    }
    // The GPU the context reports (GL_EXT_memory_object), else the one driving the window's monitor.
    static bool Luid(HWND window, LUID& luid)
    {
        if (gl.getUnsignedBytev)
        {
            GLubyte bytes[8] {};
            gl.Drain();
            gl.getUnsignedBytev(GL_DEVICE_LUID_EXT, bytes);
            if (!gl.getError() && (bytes[0] | bytes[1] | bytes[2] | bytes[3] | bytes[4] | bytes[5] | bytes[6] | bytes[7]))
            {
                std::memcpy(&luid, bytes, sizeof(luid));
                return true;
            }
        }
        const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
        {
            ComPtr<IDXGIOutput> output;
            for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
            {
                DXGI_OUTPUT_DESC desc {};
                DXGI_ADAPTER_DESC1 info {};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor && SUCCEEDED(adapter->GetDesc1(&info)))
                {
                    luid = info.AdapterLuid;
                    return true;
                }
            }
        }
        return false;
    }
    static bool Lost(HWND) { return false; }
    // The default framebuffer is the window's client area.
    static bool Describe(HDC dc, UINT& width, UINT& height, uint32_t& format)
    {
        RECT client {};
        if (!GetClientRect(WindowFromDC(dc), &client))
            return false;
        width = static_cast<UINT>(client.right);
        height = static_cast<UINT>(client.bottom);
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
        return true;
    }

    // The copy of the default framebuffer the host reads, top row first: a D3D11 texture shared by its handle that the
    // context draws into through a memory object, or, without GL_EXT_memory_object_win32, rows read back into a
    // section of memory. GL objects belong to the context they were made in and are deleted only with it current;
    // otherwise they are left to it.
    struct Stage
    {
        HGLRC context = nullptr;
        ComPtr<ID3D11Texture2D> texture;
        GLuint memory = 0, imported = 0, fbo = 0, resolved = 0, resolveFbo = 0;
        GLint samples = 0;
        HANDLE shared = nullptr;
        Handle section;
        void* view = nullptr;
        std::vector<BYTE> rows;
        UINT width = 0, height = 0;
        uint32_t format = 0;
        const uint32_t wire = DXGI_FORMAT_R8G8B8A8_UNORM;

        void Release()
        {
            if (context && gl.getCurrentContext && gl.getCurrentContext() == context)
            {
                for (GLuint* fb : { &fbo, &resolveFbo })
                    if (*fb)
                        gl.deleteFramebuffers(1, fb);
                for (GLuint* t : { &imported, &resolved })
                    if (*t)
                        gl.deleteTextures(1, t);
                if (memory)
                    gl.deleteMemoryObjects(1, &memory);
                gl.Drain();
            }
            memory = imported = fbo = resolved = resolveFbo = 0;
            samples = 0;
            texture.Reset();
            if (view)
                UnmapViewOfFile(view);
            view = nullptr;
            section.reset();
            rows.clear();
            rows.shrink_to_fit();
            context = nullptr;
            shared = nullptr;
            width = height = 0;
            format = 0;
        }
        bool Made() const { return texture || view; }

        bool MakeDevice(HWND window)
        {
            LUID luid {};
            if (!Luid(window, luid))
                return false;
            if (d3d11 && d3d11Luid.LowPart == luid.LowPart && d3d11Luid.HighPart == luid.HighPart &&
                d3d11->GetDeviceRemovedReason() == S_OK)
                return true;
            d3d11.Reset();
            ComPtr<IDXGIAdapter1> adapter;
            const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
            if (!AdapterFor(luid, adapter) ||
                FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2,
                                         D3D11_SDK_VERSION, &d3d11, nullptr, nullptr)))
            {
                Log("no D3D11 device on the context's GPU for the shared texture");
                return false;
            }
            d3d11Luid = luid;
            return true;
        }
        // The shared texture, its memory imported into the context, and a framebuffer over it.
        bool Import(HWND window, UINT w, UINT h)
        {
            if (!gl.Memory() || !MakeDevice(window))
                return false;
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            ComPtr<IDXGIResource> resource;
            if (FAILED(d3d11->CreateTexture2D(&desc, nullptr, &texture)) || FAILED(texture.As(&resource)) ||
                FAILED(resource->GetSharedHandle(&shared)) || !shared)
                return false;
            Saved saved;
            const GLint dedicated = 1;
            gl.createMemoryObjects(1, &memory);
            gl.memoryObjectParameteriv(memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
            gl.importMemoryWin32Handle(memory, GLuint64(w) * h * 4, GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT, shared);
            gl.genTextures(1, &imported);
            gl.bindTexture(GL_TEXTURE_2D, imported);
            gl.texStorageMem2D(GL_TEXTURE_2D, 1, GL_RGBA8, GLsizei(w), GLsizei(h), memory, 0);
            gl.genFramebuffers(1, &fbo);
            gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, imported, 0);
            bool ok = !gl.getError() && gl.checkFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            // A multisampled default framebuffer is resolved first: one blit cannot resolve and flip at once.
            gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            gl.getIntegerv(GL_SAMPLES, &samples);
            if (ok && samples > 1)
            {
                ok = gl.texStorage2D != nullptr;
                if (ok)
                {
                    gl.genTextures(1, &resolved);
                    gl.bindTexture(GL_TEXTURE_2D, resolved);
                    gl.texStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, GLsizei(w), GLsizei(h));
                    gl.genFramebuffers(1, &resolveFbo);
                    gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, resolveFbo);
                    gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolved, 0);
                    ok = gl.checkFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
                }
            }
            gl.Drain();
            return ok;
        }
        bool Make(HWND window, UINT w, UINT h, uint32_t, bool cpu)
        {
            context = gl.getCurrentContext();
            width = w;
            height = h;
            if (!cpu && Import(window, w, h))
                return true;
            if (!cpu)
                Log("the context cannot draw into a shared D3D11 texture; frames cross through memory");
            // Forget the half-made import, keep the context.
            const HGLRC made = context;
            Release();
            context = made;
            width = w;
            height = h;
            const DWORD bytes = w * h * 4;
            section.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, nullptr));
            view = section ? MapViewOfFile(section.value, FILE_MAP_WRITE, 0, 0, bytes) : nullptr;
            rows.resize(bytes);
            return view != nullptr;
        }

        // GL keeps the bottom row first; the host takes the top row first.
        bool Capture(HWND, HDC, const RECT*, const RECT*, HWND)
        {
            Saved saved;
            const GLint w = GLint(width), h = GLint(height);
            if (texture)
            {
                if (samples > 1)
                {
                    gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, resolveFbo);
                    gl.blitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                    gl.bindFramebuffer(GL_READ_FRAMEBUFFER, resolveFbo);
                }
                gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
                gl.blitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                // The host's copy out of the texture follows: the blit must have landed.
                gl.finish();
                return !gl.getError();
            }
            saved.Pack();
            gl.readPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
            saved.Unpack();
            if (gl.getError())
                return false;
            const size_t row = size_t(width) * 4;
            for (UINT y = 0; y < height; ++y)
                std::memcpy(static_cast<BYTE*>(view) + y * row, rows.data() + (height - 1 - y) * row, row);
            return true;
        }
    };
};
// Never destroyed, as the D3D9 proxy's: nothing of it is released from DllMain at process exit.
Bridge<OpenGL>& bridge = *new Bridge<OpenGL>;

thread_local bool inPresent = false;

// Runs the bridge for one swap of the context current on this thread; false means swap natively.
bool Bridged(HDC dc)
{
    static const bool enabled = Setting(L"Enabled", true);
    if (inPresent || !enabled || !dc || !gl.Load())
        return false;
    std::lock_guard guard(lock);
    const HGLRC context = gl.getCurrentContext();
    const HWND window = WindowFromDC(dc);
    if (!context || !window)
        return false;
    gl.LoadFor(context);
    // A stage made in another context: its GL objects are that context's.
    if (bridge.stage.context && bridge.stage.context != context)
        bridge.Drop();
    // Adaptive vsync (a negative interval) is presented at 1.
    const int interval = gl.getSwapInterval ? gl.getSwapInterval() : 1;
    bridge.SetInterval(window, interval < 0 ? 1 : static_cast<UINT>(std::min(interval, 4)));
    inPresent = true;
    const bool done = bridge.Present(window, dc, nullptr, nullptr);
    inPresent = false;
    if (!done)
        ++status.native;
    return done;
}
} // namespace

extern "C" BOOL WINAPI _wglSwapBuffers(HDC dc)
{
    if (Bridged(dc))
        return TRUE;
    const bool outer = !inPresent;
    inPresent = true;
    const BOOL result = gl.Load() ? gl.swapBuffers(dc) : FALSE;
    inPresent = !outer;
    return result;
}
extern "C" BOOL WINAPI _wglSwapLayerBuffers(HDC dc, UINT planes)
{
    if ((planes & WGL_SWAP_MAIN_PLANE) && Bridged(dc))
    {
        planes &= ~WGL_SWAP_MAIN_PLANE;
        if (!planes)
            return TRUE;
    }
    const bool outer = !inPresent;
    inPresent = true;
    const BOOL result = gl.Load() && gl.swapLayerBuffers ? gl.swapLayerBuffers(dc, planes) : FALSE;
    inPresent = !outer;
    return result;
}

// Every other export goes to the system opengl32.dll as it is: a thunk that finds the function on its first call and
// jumps to it with the caller's stack untouched.
namespace
{
const char* const forwarded[] = {
#define GL_EXPORT(index, name) #name,
#include "exports_opengl32.inc"
#undef GL_EXPORT
};
FARPROC targets[std::size(forwarded)] {};
} // namespace
extern "C" FARPROC __stdcall ForwardTarget(unsigned index)
{
    if (!targets[index])
        targets[index] = SystemProc(forwarded[index]);
    return targets[index];
}
#define GL_EXPORT(index, name)                                                                                         \
    extern "C" __declspec(naked) void Forward_##name() { __asm push index __asm call ForwardTarget __asm jmp eax }
#include "exports_opengl32.inc"
#undef GL_EXPORT
