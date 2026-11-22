// 32-bit d3d9.dll for the games OptiScaler.dll (x64 only) cannot load into. It forwards every export
// to the system d3d9.dll and, at each Present of the device's own swap chain, hands the back buffer
// to a 64-bit host (host.cpp, in the OptiScaler64 folder beside this file) that runs OptiScaler on a
// D3D11 swap chain on this game's window. While the host presents, the game's own Present is
// skipped: one presenter owns the window for the whole session, whatever NR, frame generation or
// the menu are doing. A frame the host cannot take is presented natively and the host is started
// again, up to three times; after that the session stays native.
#include <d3d9.h>
#include <d3d9on12.h>

namespace
{
constexpr wchar_t LogName[] = L"dx9-x86.log", Section[] = L"Dx9X86", SurfaceName[] = L"OptiScalerDx9X86Surface",
                  PipePrefix[] = L"\\\\.\\pipe\\opti-dx9-x86-";
}
#include "bridge.h"

namespace
{
HMODULE system9 = nullptr;
INIT_ONCE systemOnce = INIT_ONCE_STATIC_INIT;

using Create9 = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                            IDirect3DDevice9**);
using Create9Ex = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                              D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
using Present9 = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using Present9Ex = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*,
                                               DWORD);
using PresentChain = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*,
                                                 DWORD);
using Reset9 = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Reset9Ex = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
Create9 create9 = nullptr;
Create9Ex create9Ex = nullptr;
Present9 present9 = nullptr;
Present9Ex present9Ex = nullptr;
PresentChain presentChain = nullptr;
Reset9 reset9 = nullptr;
Reset9Ex reset9Ex = nullptr;

// No CRT here: Windows compatibility shims (AcGenral) call the forwarded exports while the process starts, before
// this DLL's CRT is up; a std::wstring there recursed until the stack ran out (Tomb Raider 2013)
BOOL CALLBACK LoadSystem(PINIT_ONCE, PVOID, PVOID*)
{
    wchar_t path[MAX_PATH + 16] {};
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return TRUE;
    lstrcatW(path, L"\\d3d9.dll");
    system9 = LoadLibraryW(path);
    return TRUE;
}
template <class T> T System(const char* name)
{
    InitOnceExecuteOnce(&systemOnce, LoadSystem, nullptr, nullptr);
    return reinterpret_cast<T>(system9 ? GetProcAddress(system9, name) : nullptr);
}

// The D3D9 side of the bridge (bridge.h).
struct D3D9
{
    using Device = IDirect3DDevice9;
    using Back = IDirect3DSurface9;
    static constexpr const char* Name = "D3D9";

    // The window a device presents to, and its focus window.
    static HWND Window(IDirect3DDevice9* game, HWND& focus)
    {
        D3DDEVICE_CREATION_PARAMETERS creation {};
        D3DPRESENT_PARAMETERS params {};
        ComPtr<IDirect3DSwapChain9> chain;
        if (FAILED(game->GetCreationParameters(&creation)) || FAILED(game->GetSwapChain(0, &chain)) ||
            FAILED(chain->GetPresentParameters(&params)))
            return nullptr;
        const HWND window = params.hDeviceWindow ? params.hDeviceWindow : creation.hFocusWindow;
        focus = creation.hFocusWindow ? creation.hFocusWindow : window;
        return window;
    }

    static bool Luid(IDirect3DDevice9* device, LUID& luid)
    {
        D3DDEVICE_CREATION_PARAMETERS creation {};
        ComPtr<IDirect3D9> api;
        if (FAILED(device->GetCreationParameters(&creation)) || FAILED(device->GetDirect3D(&api)))
            return false;
        const auto monitor = api->GetAdapterMonitor(creation.AdapterOrdinal);
        ComPtr<IDXGIFactory1> factory;
        if (!monitor || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
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

    static bool Lost(IDirect3DDevice9* device) { return device->TestCooperativeLevel() != D3D_OK; }
    static bool Describe(IDirect3DSurface9* back, UINT& width, UINT& height, uint32_t& format)
    {
        D3DSURFACE_DESC desc {};
        if (FAILED(back->GetDesc(&desc)))
            return false;
        width = desc.Width;
        height = desc.Height;
        format = desc.Format;
        return true;
    }

    // The copy of the back buffer the host reads: an A8R8G8B8 render target shared by handle, or, when
    // the device cannot share one, a render target read back into a section of memory.
    struct Stage
    {
        ComPtr<IDirect3DTexture9> texture;
        ComPtr<IDirect3DSurface9> surface, readback;
        HANDLE shared = nullptr;
        Handle section;
        void* view = nullptr;
        UINT width = 0, height = 0;
        uint32_t format = 0;
        // The format the host's swap chain takes.
        const uint32_t wire = DXGI_FORMAT_B8G8R8A8_UNORM;

        void Release()
        {
            if (view)
                UnmapViewOfFile(view);
            view = nullptr;
            section.reset();
            readback.Reset();
            surface.Reset();
            texture.Reset();
            shared = nullptr;
            width = height = 0;
            format = 0;
        }
        bool Made() const { return texture != nullptr; }

        bool Make(IDirect3DDevice9* game, UINT w, UINT h, uint32_t, bool cpu)
        {
            if (cpu ||
                FAILED(game->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture,
                                           &shared)) ||
                !shared)
            {
                texture.Reset();
                shared = nullptr;
                const DWORD bytes = w * h * 4;
                if (FAILED(game->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                               &texture, nullptr)) ||
                    FAILED(game->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback,
                                                             nullptr)))
                    return false;
                section.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, nullptr));
                view = section ? MapViewOfFile(section.value, FILE_MAP_WRITE, 0, 0, bytes) : nullptr;
                if (!view)
                    return false;
            }
            return SUCCEEDED(texture->GetSurfaceLevel(0, &surface));
        }

        bool Wait9(IDirect3DDevice9* game)
        {
            ComPtr<IDirect3DQuery9> query;
            if (FAILED(game->CreateQuery(D3DQUERYTYPE_EVENT, &query)) || FAILED(query->Issue(D3DISSUE_END)))
                return false;
            const ULONGLONG deadline = GetTickCount64() + GpuWaitMs;
            HRESULT done = S_FALSE;
            while ((done = query->GetData(nullptr, 0, D3DGETDATA_FLUSH)) == S_FALSE && GetTickCount64() < deadline)
                YieldProcessor();
            return done == S_OK;
        }

        // source and dest are the Present's: the part of the back buffer it shows, and where in the window's
        // client area. The host stretches the copy over the client area, so dest is scaled to the copy; the
        // rest of the copy keeps the frame before, as the window would. When the rectangles cannot be
        // copied (a multisampled back buffer cannot be stretched), the whole back buffer is.
        bool Capture(IDirect3DDevice9* game, IDirect3DSurface9* back, const RECT* source, const RECT* dest, HWND window)
        {
            RECT into {}, client {};
            const RECT whole { 0, 0, LONG(width), LONG(height) };
            const bool scaled = dest && GetClientRect(window, &client) && client.right > 0 && client.bottom > 0;
            if (scaled)
            {
                into = { MulDiv(dest->left, width, client.right), MulDiv(dest->top, height, client.bottom),
                         MulDiv(dest->right, width, client.right), MulDiv(dest->bottom, height, client.bottom) };
                IntersectRect(&into, &into, &whole);
            }
            const RECT shown = source ? *source : whole;
            const RECT& to = scaled ? into : whole;
            const bool same =
                shown.right - shown.left == to.right - to.left && shown.bottom - shown.top == to.bottom - to.top;
            const bool part = source || scaled;
            if (!part ||
                FAILED(game->StretchRect(back, &shown, surface.Get(), &to, same ? D3DTEXF_NONE : D3DTEXF_LINEAR)))
            {
                if (FAILED(game->StretchRect(back, nullptr, surface.Get(), nullptr, D3DTEXF_NONE)))
                    return false;
            }
            if (shared)
                return Wait9(game);
            D3DLOCKED_RECT from {};
            if (FAILED(game->GetRenderTargetData(surface.Get(), readback.Get())) ||
                FAILED(readback->LockRect(&from, nullptr, D3DLOCK_READONLY)))
                return false;
            const UINT row = width * 4;
            for (UINT y = 0; y < height; ++y)
                std::memcpy(static_cast<BYTE*>(view) + size_t(y) * row,
                            static_cast<const BYTE*>(from.pBits) + size_t(y) * from.Pitch, row);
            readback->UnlockRect();
            return true;
        }
    };
};

// Never destroyed: releasing D3D9 objects from DllMain at process exit deadlocks D3D9Ex.
Bridge<D3D9>& bridge = *new Bridge<D3D9>;

thread_local bool inPresent = false;

// Runs the bridge for one Present of the device's own swap chain to its own window; false means
// present natively.
bool Bridged(IDirect3DDevice9* game, IDirect3DSwapChain9* chain, HWND target, const RECT* source, const RECT* dest)
{
    if (inPresent)
        return false;
    std::lock_guard guard(lock);
    ComPtr<IDirect3DSwapChain9> implicit;
    D3DPRESENT_PARAMETERS params {};
    if (FAILED(game->GetSwapChain(0, &implicit)) || (chain && implicit.Get() != chain) ||
        (target && SUCCEEDED(implicit->GetPresentParameters(&params)) && params.hDeviceWindow &&
         target != params.hDeviceWindow))
        return false;
    ComPtr<IDirect3DSurface9> back;
    if (FAILED(game->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
        return false;
    inPresent = true;
    const bool done = bridge.Present(game, back.Get(), source, dest);
    inPresent = false;
    if (!done)
        ++status.native;
    return done;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* device, const RECT* source, const RECT* dest, HWND window,
                                      const RGNDATA* dirty)
{
    if (Bridged(device, nullptr, window, source, dest))
        return S_OK;
    const bool outer = !inPresent;
    inPresent = true;
    const HRESULT result = present9(device, source, dest, window, dirty);
    inPresent = !outer;
    return result;
}
HRESULT STDMETHODCALLTYPE HookPresentEx(IDirect3DDevice9Ex* device, const RECT* source, const RECT* dest, HWND window,
                                        const RGNDATA* dirty, DWORD flags)
{
    if (!(flags & D3DPRESENT_DONOTFLIP) && Bridged(device, nullptr, window, source, dest))
        return S_OK;
    const bool outer = !inPresent;
    inPresent = true;
    const HRESULT result = present9Ex(device, source, dest, window, dirty, flags);
    inPresent = !outer;
    return result;
}
HRESULT STDMETHODCALLTYPE HookChain(IDirect3DSwapChain9* chain, const RECT* source, const RECT* dest, HWND window,
                                    const RGNDATA* dirty, DWORD flags)
{
    ComPtr<IDirect3DDevice9> device;
    if (!inPresent && SUCCEEDED(chain->GetDevice(&device)) && Bridged(device.Get(), chain, window, source, dest))
        return S_OK;
    const bool outer = !inPresent;
    inPresent = true;
    const HRESULT result = presentChain(chain, source, dest, window, dirty, flags);
    inPresent = !outer;
    return result;
}

// A fullscreen D3D9 device owns the display, and the host's swap chain on its window would never
// be seen; Windowed=1 keeps it windowed, and its window becomes a borderless one over the monitor
// (bridge.h, Restyle; borderless says so). [Dx9X86] ForceWindowed=0 opts out.
// The host presents at the interval the game asked for. A windowed D3D9 device takes only the
// default, immediate and one, so a fullscreen request for two, three or four is made one in D3D9.
UINT Prepare(D3DPRESENT_PARAMETERS* params, bool& borderless)
{
    borderless = false;
    if (!params)
        return 1;
    const UINT interval = params->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? 0
                          : params->PresentationInterval == D3DPRESENT_INTERVAL_TWO     ? 2
                          : params->PresentationInterval == D3DPRESENT_INTERVAL_THREE   ? 3
                          : params->PresentationInterval == D3DPRESENT_INTERVAL_FOUR    ? 4
                                                                                        : 1;
    if (!params->Windowed && Setting(L"ForceWindowed", true))
    {
        params->Windowed = TRUE;
        params->FullScreen_RefreshRateInHz = 0;
        if (interval > 1)
            params->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        Log("fullscreen %ux%u at interval %u requested; windowed and borderless so the host's swap chain is visible",
            params->BackBufferWidth, params->BackBufferHeight, interval);
        borderless = true;
    }
    return interval;
}
// The copy of the back buffer belongs to the device the host serves and goes before that device resets.
void PrepareReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params)
{
    std::lock_guard guard(lock);
    if (device == bridge.device)
        bridge.Drop();
    bool borderless = false;
    bridge.SetInterval(device, Prepare(params, borderless));
    HWND focus = nullptr;
    const HWND window = params && params->hDeviceWindow ? params->hDeviceWindow : D3D9::Window(device, focus);
    Restyle(window, focus, borderless);
}
HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params)
{
    PrepareReset(device, params);
    return reset9(device, params);
}
HRESULT STDMETHODCALLTYPE HookResetEx(IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* mode)
{
    PrepareReset(device, params);
    return reset9Ex(device, params, params && params->Windowed ? nullptr : mode);
}

void HookDevice(IDirect3DDevice9* device, UINT interval)
{
    std::lock_guard guard(lock);
    bridge.SetInterval(device, interval);
    auto table = *reinterpret_cast<void***>(device);
    Replace(&table[17], reinterpret_cast<void*>(HookPresent), reinterpret_cast<void**>(&present9));
    Replace(&table[16], reinterpret_cast<void*>(HookReset), reinterpret_cast<void**>(&reset9));
    ComPtr<IDirect3DSwapChain9> chain;
    if (SUCCEEDED(device->GetSwapChain(0, &chain)))
        Replace(&(*reinterpret_cast<void***>(chain.Get()))[3], reinterpret_cast<void*>(HookChain),
                reinterpret_cast<void**>(&presentChain));
    ComPtr<IDirect3DDevice9Ex> ex;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&ex))))
    {
        auto entries = *reinterpret_cast<void***>(ex.Get());
        Replace(&entries[121], reinterpret_cast<void*>(HookPresentEx), reinterpret_cast<void**>(&present9Ex));
        Replace(&entries[132], reinterpret_cast<void*>(HookResetEx), reinterpret_cast<void**>(&reset9Ex));
    }
}
// The window a new device presents to becomes borderless when the game asked for fullscreen.
void Created(IDirect3DDevice9* device, UINT interval, bool borderless, HWND focus, D3DPRESENT_PARAMETERS* params)
{
    HookDevice(device, interval);
    if (!borderless)
        return;
    std::lock_guard guard(lock);
    Restyle(params->hDeviceWindow ? params->hDeviceWindow : focus, focus, true);
}
HRESULT STDMETHODCALLTYPE HookCreate(IDirect3D9* api, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                     D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** out)
{
    bool borderless = false;
    const UINT interval = Prepare(params, borderless);
    const auto result = create9(api, adapter, type, window, flags, params, out);
    if (SUCCEEDED(result) && out && *out)
        Created(*out, interval, borderless, window, params);
    return result;
}
HRESULT STDMETHODCALLTYPE HookCreateEx(IDirect3D9Ex* api, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                       D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** out)
{
    bool borderless = false;
    const UINT interval = Prepare(params, borderless);
    const auto result =
        create9Ex(api, adapter, type, window, flags, params, params && params->Windowed ? nullptr : mode, out);
    if (SUCCEEDED(result) && out && *out)
        Created(*out, interval, borderless, window, params);
    return result;
}
void HookApi(IDirect3D9* api)
{
    std::lock_guard guard(lock);
    if (!Setting(L"Enabled", true))
        return;
    auto table = *reinterpret_cast<void***>(api);
    Replace(&table[16], reinterpret_cast<void*>(HookCreate), reinterpret_cast<void**>(&create9));
    ComPtr<IDirect3D9Ex> ex;
    if (SUCCEEDED(api->QueryInterface(IID_PPV_ARGS(&ex))))
        Replace(&(*reinterpret_cast<void***>(ex.Get()))[20], reinterpret_cast<void*>(HookCreateEx),
                reinterpret_cast<void**>(&create9Ex));
}
} // namespace

extern "C" IDirect3D9* WINAPI _Direct3DCreate9(UINT version)
{
    auto fn = System<decltype(&Direct3DCreate9)>("Direct3DCreate9");
    auto api = fn ? fn(version) : nullptr;
    if (api)
        HookApi(api);
    return api;
}
extern "C" HRESULT WINAPI _Direct3DCreate9Ex(UINT version, IDirect3D9Ex** out)
{
    auto fn = System<decltype(&Direct3DCreate9Ex)>("Direct3DCreate9Ex");
    const auto result = fn ? fn(version, out) : E_FAIL;
    if (SUCCEEDED(result) && out && *out)
        HookApi(*out);
    return result;
}
extern "C" IDirect3D9* WINAPI _Direct3DCreate9On12(UINT version, D3D9ON12_ARGS* args, UINT count)
{
    auto fn = System<decltype(&Direct3DCreate9On12)>("Direct3DCreate9On12");
    auto api = fn ? fn(version, args, count) : nullptr;
    if (api)
        HookApi(api);
    return api;
}
extern "C" HRESULT WINAPI _Direct3DCreate9On12Ex(UINT version, D3D9ON12_ARGS* args, UINT count, IDirect3D9Ex** out)
{
    auto fn = System<decltype(&Direct3DCreate9On12Ex)>("Direct3DCreate9On12Ex");
    const auto result = fn ? fn(version, args, count, out) : E_FAIL;
    if (SUCCEEDED(result) && out && *out)
        HookApi(*out);
    return result;
}
#define FORWARD(returnType, name, args, call, fallback)                                                                \
    extern "C" returnType WINAPI _##name args                                                                          \
    {                                                                                                                  \
        auto fn = System<decltype(&name)>(#name);                                                                      \
        return fn ? fn call : fallback;                                                                                \
    }
#define FORWARD_VOID(name, args, call)                                                                                 \
    extern "C" void WINAPI _##name args                                                                                \
    {                                                                                                                  \
        auto fn = System<decltype(&name)>(#name);                                                                      \
        if (fn)                                                                                                        \
            fn call;                                                                                                   \
    }
FORWARD(int, D3DPERF_BeginEvent, (D3DCOLOR colour, LPCWSTR name), (colour, name), 0)
FORWARD(int, D3DPERF_EndEvent, (), (), 0)
FORWARD(DWORD, D3DPERF_GetStatus, (), (), 0)
FORWARD(BOOL, D3DPERF_QueryRepeatFrame, (), (), FALSE)
FORWARD_VOID(D3DPERF_SetMarker, (D3DCOLOR colour, LPCWSTR name), (colour, name))
FORWARD_VOID(D3DPERF_SetOptions, (DWORD options), (options))
FORWARD_VOID(D3DPERF_SetRegion, (D3DCOLOR colour, LPCWSTR name), (colour, name))
