// 32-bit dxgi.dll for the D3D10 and D3D11 games OptiScaler.dll (x64 only) cannot load into. It forwards every export
// to the system dxgi.dll and, at each Present of a swap chain on a D3D10 or D3D11 device, hands the back buffer to the
// same 64-bit host as the D3D9 proxy (host.cpp, through bridge.h), which runs OptiScaler on a D3D11 swap chain on a
// child of the game's window. d3d10.dll, d3d10_1.dll and d3d11.dll (D3D11CreateDeviceAndSwapChain too) make their
// swap chains through the dxgi.dll beside the game, so this one proxy covers all three. Swap chains of other devices
// (a D3D12 queue's) are presented natively.
#include <windows.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>

namespace
{
constexpr wchar_t LogName[] = L"dxgi-x86.log", Section[] = L"DxgiX86", SurfaceName[] = L"OptiScalerDxgiX86Surface",
                  PipePrefix[] = L"\\\\.\\pipe\\opti-dxgi-x86-";
}
#include "bridge.h"

namespace
{
HMODULE systemDxgi = nullptr;
SRWLOCK systemLock = SRWLOCK_INIT;
DWORD loadingThread = 0;

// No CRT here: Windows compatibility shims (AcGenral) call the forwarded exports while the process starts, before
// this DLL's CRT is up; a std::wstring there recursed until the stack ran out (Tomb Raider 2013). Loading the system
// DLL makes the shim call this DLL's SetAppCompatStringPointer again on the same thread (it finds the exports by the
// name dxgi.dll): that call sees the load in progress and gets nothing, where INIT_ONCE waited for itself for good.
// A load that fails is tried again on a later call.
HMODULE LoadSystem()
{
    if (const auto loaded = reinterpret_cast<HMODULE>(InterlockedCompareExchangePointer(
            reinterpret_cast<void**>(&systemDxgi), nullptr, nullptr)))
        return loaded;
    if (InterlockedCompareExchange(&loadingThread, 0, 0) == GetCurrentThreadId())
        return nullptr;
    AcquireSRWLockExclusive(&systemLock);
    if (!systemDxgi)
    {
        wchar_t path[MAX_PATH + 16] {};
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n != 0 && n < MAX_PATH)
        {
            lstrcatW(path, L"\\dxgi.dll");
            InterlockedExchange(&loadingThread, GetCurrentThreadId());
            const HMODULE loaded = LoadLibraryW(path);
            InterlockedExchange(&loadingThread, 0);
            InterlockedExchangePointer(reinterpret_cast<void**>(&systemDxgi), loaded);
        }
    }
    const HMODULE loaded = systemDxgi;
    ReleaseSRWLockExclusive(&systemLock);
    return loaded;
}
void ReplayCompat();
FARPROC SystemProc(const char* name)
{
    if (!LoadSystem())
        return nullptr;
    ReplayCompat();
    return GetProcAddress(systemDxgi, name);
}

using CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                           const DXGI_SWAP_CHAIN_DESC1*,
                                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                           IDXGISwapChain1**);
using PresentChain = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Chain = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using SetFullscreen = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using ResizeChain = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using GetFullscreen = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL*, IDXGIOutput**);
using ResizeTarget = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);
CreateSwapChain createSwapChain = nullptr;
CreateSwapChainForHwnd createSwapChainForHwnd = nullptr;
PresentChain present = nullptr;
Present1Chain present1 = nullptr;
SetFullscreen setFullscreen = nullptr;
ResizeChain resizeBuffers = nullptr;
GetFullscreen getFullscreen = nullptr;
ResizeTarget resizeTarget = nullptr;

// Patches a vtable slot once. All objects of a DXGI class share one table; a second table (another class) whose slot
// holds something else than the first one's original is left alone.
void Hook(void** slot, void* replacement, void** original)
{
    if (*original && *slot != *original)
        return;
    Replace(slot, replacement, original);
}

// The UNORM twin of a back buffer format the host's flip swap chain takes; 0 for the formats it does not. A frame
// crosses at four bytes a texel (Build's pitch).
uint32_t Wire(uint32_t format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    return 0;
}

// The DXGI side of the bridge (bridge.h), for D3D10 and D3D11 devices. The swap chain stands for the device: it names
// the window and the device.
struct Dxgi
{
    using Device = IDXGISwapChain;
    using Back = IDXGISurface;
    static constexpr const char* Name = "DXGI";

    static HWND Window(IDXGISwapChain* swap, HWND& focus)
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (FAILED(swap->GetDesc(&desc)))
            return nullptr;
        focus = desc.OutputWindow;
        return desc.OutputWindow;
    }
    static bool Luid(IDXGISwapChain* swap, LUID& luid)
    {
        ComPtr<IDXGIDevice> device;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc {};
        if (FAILED(swap->GetDevice(IID_PPV_ARGS(&device))) || FAILED(device->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&desc)))
            return false;
        luid = desc.AdapterLuid;
        return true;
    }
    // A removed device presents natively, which gives the game the error it recreates its device on.
    static bool Lost(IDXGISwapChain* swap)
    {
        ComPtr<ID3D11Device> d3d11;
        if (SUCCEEDED(swap->GetDevice(IID_PPV_ARGS(&d3d11))))
            return d3d11->GetDeviceRemovedReason() != S_OK;
        ComPtr<ID3D10Device> d3d10;
        return FAILED(swap->GetDevice(IID_PPV_ARGS(&d3d10))) || d3d10->GetDeviceRemovedReason() != S_OK;
    }
    static bool Describe(IDXGISurface* back, UINT& width, UINT& height, uint32_t& format)
    {
        DXGI_SURFACE_DESC desc {};
        if (FAILED(back->GetDesc(&desc)))
            return false;
        width = desc.Width;
        height = desc.Height;
        format = desc.Format;
        return true;
    }

    // The copy of the back buffer the host reads: a texture shared by its handle, or one read back into a section of
    // memory when the device cannot share one. A multisampled back buffer is resolved into it. Made on the game's own
    // device: D3D10, or D3D11 (d3d11 set) through its immediate context.
    struct Stage
    {
        ComPtr<IUnknown> owner;
        ComPtr<ID3D10Device> d3d10;
        ComPtr<ID3D10Texture2D> texture10, readback10;
        ComPtr<ID3D10Query> done10;
        ComPtr<ID3D11Device> d3d11;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11Texture2D> texture11, readback11;
        ComPtr<ID3D11Query> done11;
        HANDLE shared = nullptr;
        Handle section;
        void* view = nullptr;
        UINT width = 0, height = 0;
        uint32_t format = 0, wire = 0;

        void Release()
        {
            if (view)
                UnmapViewOfFile(view);
            view = nullptr;
            section.reset();
            done10.Reset();
            readback10.Reset();
            texture10.Reset();
            d3d10.Reset();
            done11.Reset();
            readback11.Reset();
            texture11.Reset();
            context.Reset();
            d3d11.Reset();
            owner.Reset();
            shared = nullptr;
            width = height = 0;
            format = wire = 0;
        }
        bool Made() const { return texture10 || texture11; }

        bool Section(UINT w, UINT h)
        {
            const DWORD bytes = w * h * 4;
            section.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, nullptr));
            view = section ? MapViewOfFile(section.value, FILE_MAP_WRITE, 0, 0, bytes) : nullptr;
            return view != nullptr;
        }
        bool Make10(UINT w, UINT h, bool cpu)
        {
            D3D10_TEXTURE2D_DESC desc {};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = static_cast<DXGI_FORMAT>(wire);
            desc.Usage = D3D10_USAGE_DEFAULT;
            desc.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D10_RESOURCE_MISC_SHARED;
            ComPtr<IDXGIResource> resource;
            if (cpu || FAILED(d3d10->CreateTexture2D(&desc, nullptr, &texture10)) || FAILED(texture10.As(&resource)) ||
                FAILED(resource->GetSharedHandle(&shared)) || !shared)
            {
                texture10.Reset();
                shared = nullptr;
                desc.MiscFlags = 0;
                if (FAILED(d3d10->CreateTexture2D(&desc, nullptr, &texture10)))
                    return false;
                desc.BindFlags = 0;
                desc.Usage = D3D10_USAGE_STAGING;
                desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
                if (FAILED(d3d10->CreateTexture2D(&desc, nullptr, &readback10)) || !Section(w, h))
                    return false;
            }
            D3D10_QUERY_DESC query { D3D10_QUERY_EVENT, 0 };
            return SUCCEEDED(d3d10->CreateQuery(&query, &done10));
        }
        bool Make11(UINT w, UINT h, bool cpu)
        {
            d3d11->GetImmediateContext(&context);
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = static_cast<DXGI_FORMAT>(wire);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            ComPtr<IDXGIResource> resource;
            if (cpu || FAILED(d3d11->CreateTexture2D(&desc, nullptr, &texture11)) || FAILED(texture11.As(&resource)) ||
                FAILED(resource->GetSharedHandle(&shared)) || !shared)
            {
                texture11.Reset();
                shared = nullptr;
                desc.MiscFlags = 0;
                if (FAILED(d3d11->CreateTexture2D(&desc, nullptr, &texture11)))
                    return false;
                desc.BindFlags = 0;
                desc.Usage = D3D11_USAGE_STAGING;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(d3d11->CreateTexture2D(&desc, nullptr, &readback11)) || !Section(w, h))
                    return false;
            }
            D3D11_QUERY_DESC query { D3D11_QUERY_EVENT, 0 };
            return SUCCEEDED(d3d11->CreateQuery(&query, &done11));
        }
        bool Make(IDXGISwapChain* swap, UINT w, UINT h, uint32_t f, bool cpu)
        {
            wire = Wire(f);
            if (!wire)
            {
                Log("back buffer format %u cannot cross to the host", f);
                return false;
            }
            if (FAILED(swap->GetDevice(IID_PPV_ARGS(&owner))))
                return false;
            if (SUCCEEDED(owner.As(&d3d11)))
                return Make11(w, h, cpu);
            return SUCCEEDED(owner.As(&d3d10)) && Make10(w, h, cpu);
        }

        // The copy into the shared texture done on the GPU, before the host reads it.
        template <class Done> static bool Wait(Done done)
        {
            const ULONGLONG deadline = GetTickCount64() + GpuWaitMs;
            HRESULT hr = S_FALSE;
            while ((hr = done()) == S_FALSE && GetTickCount64() < deadline)
                YieldProcessor();
            return hr == S_OK;
        }
        void Store(const void* from, UINT pitch)
        {
            const UINT row = width * 4;
            for (UINT y = 0; y < height; ++y)
                std::memcpy(static_cast<BYTE*>(view) + size_t(y) * row,
                            static_cast<const BYTE*>(from) + size_t(y) * pitch, row);
        }

        // DXGI presents the whole back buffer: there are no rectangles.
        bool Capture(IDXGISwapChain*, IDXGISurface* back, const RECT*, const RECT*, HWND)
        {
            DXGI_SURFACE_DESC desc {};
            back->GetDesc(&desc);
            const auto resolved = static_cast<DXGI_FORMAT>(wire);
            if (d3d11)
            {
                ComPtr<ID3D11Texture2D> source;
                if (FAILED(back->QueryInterface(IID_PPV_ARGS(&source))))
                    return false;
                if (desc.SampleDesc.Count > 1)
                    context->ResolveSubresource(texture11.Get(), 0, source.Get(), 0, resolved);
                else
                    context->CopyResource(texture11.Get(), source.Get());
                if (shared)
                {
                    context->End(done11.Get());
                    return Wait([&] { return context->GetData(done11.Get(), nullptr, 0, 0); });
                }
                context->CopyResource(readback11.Get(), texture11.Get());
                D3D11_MAPPED_SUBRESOURCE from {};
                if (FAILED(context->Map(readback11.Get(), 0, D3D11_MAP_READ, 0, &from)))
                    return false;
                Store(from.pData, from.RowPitch);
                context->Unmap(readback11.Get(), 0);
                return true;
            }
            ComPtr<ID3D10Texture2D> source;
            if (FAILED(back->QueryInterface(IID_PPV_ARGS(&source))))
                return false;
            if (desc.SampleDesc.Count > 1)
                d3d10->ResolveSubresource(texture10.Get(), 0, source.Get(), 0, resolved);
            else
                d3d10->CopyResource(texture10.Get(), source.Get());
            if (shared)
            {
                done10->End();
                return Wait([&] { return done10->GetData(nullptr, 0, 0); });
            }
            d3d10->CopyResource(readback10.Get(), texture10.Get());
            D3D10_MAPPED_TEXTURE2D from {};
            if (FAILED(readback10->Map(0, D3D10_MAP_READ, 0, &from)))
                return false;
            Store(from.pData, from.RowPitch);
            readback10->Unmap(0);
            return true;
        }
    };
};
// Never destroyed, as the D3D9 proxy's: nothing of it is released from DllMain at process exit.
Bridge<Dxgi>& bridge = *new Bridge<Dxgi>;

thread_local bool inPresent = false;

// Runs the bridge for one Present of a D3D10 or D3D11 swap chain; false means present natively.
bool Bridged(IDXGISwapChain* swap, UINT interval, UINT flags)
{
    if (inPresent || (flags & DXGI_PRESENT_TEST))
        return false;
    std::lock_guard guard(lock);
    ComPtr<IUnknown> device;
    DXGI_SWAP_CHAIN_DESC desc {};
    // A frame latency object is signalled by the swap chain's own Present, which the game would then wait for forever.
    if (FAILED(swap->GetDevice(IID_PPV_ARGS(&device))) || FAILED(swap->GetDesc(&desc)) ||
        (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
        return false;
    // A new swap chain at the address of one the game released: the copy belongs to the old device.
    if (bridge.stage.owner && bridge.stage.owner.Get() != device.Get())
        bridge.Drop();
    ComPtr<IDXGISurface> back;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back))))
        return false;
    bridge.SetInterval(swap, interval);
    inPresent = true;
    const bool done = bridge.Present(swap, back.Get(), nullptr, nullptr);
    inPresent = false;
    if (!done)
        ++status.native;
    return done;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swap, UINT interval, UINT flags)
{
    if (Bridged(swap, interval, flags))
        return S_OK;
    const bool outer = !inPresent;
    inPresent = true;
    const HRESULT result = present(swap, interval, flags);
    inPresent = !outer;
    return result;
}
HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* swap, UINT interval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* parameters)
{
    if (Bridged(swap, interval, flags))
        return S_OK;
    const bool outer = !inPresent;
    inPresent = true;
    const HRESULT result = present1(swap, interval, flags, parameters);
    inPresent = !outer;
    return result;
}
HWND WindowOf(IDXGISwapChain* swap)
{
    HWND focus = nullptr;
    return Dxgi::Window(swap, focus);
}
// A fullscreen swap chain owns the display, and the host's swap chain on its window would never be seen: the swap
// chain stays windowed and its window becomes a borderless one over the monitor (bridge.h, Restyle), which the game
// is told is fullscreen. [DxgiX86] ForceWindowed=0 opts out.
HRESULT STDMETHODCALLTYPE HookSetFullscreen(IDXGISwapChain* swap, BOOL fullscreen, IDXGIOutput* output)
{
    if (!Setting(L"ForceWindowed", true))
        return setFullscreen(swap, fullscreen, output);
    std::lock_guard guard(lock);
    Restyle(WindowOf(swap), nullptr, fullscreen != FALSE);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE HookGetFullscreen(IDXGISwapChain* swap, BOOL* fullscreen, IDXGIOutput** target)
{
    const HRESULT result = getFullscreen(swap, fullscreen, target);
    std::lock_guard guard(lock);
    if (SUCCEEDED(result) && borderlessFor && borderlessFor == WindowOf(swap))
    {
        if (fullscreen)
            *fullscreen = TRUE;
        if (target && !*target)
            swap->GetContainingOutput(target);
    }
    return result;
}
// A mode change of a fullscreen game would size the borderless window down.
HRESULT STDMETHODCALLTYPE HookResizeTarget(IDXGISwapChain* swap, const DXGI_MODE_DESC* mode)
{
    {
        std::lock_guard guard(lock);
        if (borderlessFor && borderlessFor == WindowOf(swap))
            return S_OK;
    }
    return resizeTarget(swap, mode);
}
// The copy follows the back buffer's size on its own; one the host has is dropped before the buffers go.
HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain* swap, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                     UINT flags)
{
    {
        std::lock_guard guard(lock);
        if (swap == bridge.device)
            bridge.Drop();
    }
    return resizeBuffers(swap, count, width, height, format, flags);
}

void HookSwapChain(IDXGISwapChain* swap)
{
    std::lock_guard guard(lock);
    auto table = *reinterpret_cast<void***>(swap);
    Hook(&table[8], reinterpret_cast<void*>(HookPresent), reinterpret_cast<void**>(&present));
    Hook(&table[10], reinterpret_cast<void*>(HookSetFullscreen), reinterpret_cast<void**>(&setFullscreen));
    Hook(&table[11], reinterpret_cast<void*>(HookGetFullscreen), reinterpret_cast<void**>(&getFullscreen));
    Hook(&table[13], reinterpret_cast<void*>(HookResize), reinterpret_cast<void**>(&resizeBuffers));
    Hook(&table[14], reinterpret_cast<void*>(HookResizeTarget), reinterpret_cast<void**>(&resizeTarget));
    ComPtr<IDXGISwapChain1> swap1;
    if (SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&swap1))))
        Hook(&(*reinterpret_cast<void***>(swap1.Get()))[22], reinterpret_cast<void*>(HookPresent1),
             reinterpret_cast<void**>(&present1));
}
// Only swap chains of D3D10 and D3D11 devices; a D3D12 queue's is left as it is.
bool Bridgeable(IUnknown* device)
{
    ComPtr<ID3D10Device> d3d10;
    ComPtr<ID3D11Device> d3d11;
    return device && (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d3d11))) ||
                      SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d3d10))));
}
HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out)
{
    if (!Bridgeable(device) || !desc)
        return createSwapChain(factory, device, desc, out);
    DXGI_SWAP_CHAIN_DESC local = *desc;
    const bool borderless = !local.Windowed && Setting(L"ForceWindowed", true);
    if (borderless)
    {
        local.Windowed = TRUE;
        local.BufferDesc.RefreshRate = {};
        Log("fullscreen %ux%u swap chain requested; windowed and borderless so the host's swap chain is visible",
            local.BufferDesc.Width, local.BufferDesc.Height);
    }
    const HRESULT result = createSwapChain(factory, device, &local, out);
    if (SUCCEEDED(result) && out && *out)
    {
        HookSwapChain(*out);
        if (borderless)
        {
            std::lock_guard guard(lock);
            Restyle(local.OutputWindow, nullptr, true);
        }
    }
    return result;
}
HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                     IDXGIOutput* output, IDXGISwapChain1** out)
{
    if (!Bridgeable(device))
        return createSwapChainForHwnd(factory, device, window, desc, fullscreen, output, out);
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC local {};
    const bool borderless = fullscreen && !fullscreen->Windowed && Setting(L"ForceWindowed", true);
    if (borderless)
    {
        local = *fullscreen;
        local.Windowed = TRUE;
        fullscreen = &local;
        Log("fullscreen swap chain requested; windowed and borderless so the host's swap chain is visible");
    }
    const HRESULT result = createSwapChainForHwnd(factory, device, window, desc, fullscreen, output, out);
    if (SUCCEEDED(result) && out && *out)
    {
        HookSwapChain(*out);
        if (borderless)
        {
            std::lock_guard guard(lock);
            Restyle(window, nullptr, true);
        }
    }
    return result;
}
void HookFactory(void* object)
{
    std::lock_guard guard(lock);
    if (!Setting(L"Enabled", true))
        return;
    auto factory = static_cast<IUnknown*>(object);
    auto table = *reinterpret_cast<void***>(factory);
    Hook(&table[10], reinterpret_cast<void*>(HookCreateSwapChain), reinterpret_cast<void**>(&createSwapChain));
    ComPtr<IDXGIFactory2> factory2;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2))))
        Hook(&(*reinterpret_cast<void***>(factory2.Get()))[15], reinterpret_cast<void*>(HookCreateSwapChainForHwnd),
             reinterpret_cast<void**>(&createSwapChainForHwnd));
}
} // namespace

extern "C" HRESULT WINAPI _CreateDXGIFactory(REFIID riid, void** out)
{
    auto fn = reinterpret_cast<decltype(&CreateDXGIFactory)>(SystemProc("CreateDXGIFactory"));
    const HRESULT result = fn ? fn(riid, out) : E_FAIL;
    if (SUCCEEDED(result) && out && *out)
        HookFactory(*out);
    return result;
}
extern "C" HRESULT WINAPI _CreateDXGIFactory1(REFIID riid, void** out)
{
    auto fn = reinterpret_cast<decltype(&CreateDXGIFactory1)>(SystemProc("CreateDXGIFactory1"));
    const HRESULT result = fn ? fn(riid, out) : E_FAIL;
    if (SUCCEEDED(result) && out && *out)
        HookFactory(*out);
    return result;
}
extern "C" HRESULT WINAPI _CreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
    using Create2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto fn = reinterpret_cast<Create2>(SystemProc("CreateDXGIFactory2"));
    const HRESULT result = fn ? fn(flags, riid, out) : E_FAIL;
    if (SUCCEEDED(result) && out && *out)
        HookFactory(*out);
    return result;
}

// Every other export goes to the system dxgi.dll as it is: a thunk that finds the function on its first call and
// jumps to it with the caller's stack untouched.
namespace
{
const char* const forwarded[] = {
    "ApplyCompatResolutionQuirking",
    "CompatString",
    "CompatValue",
    "DXGID3D10CreateDevice",
    "DXGID3D10CreateLayeredDevice",
    "DXGID3D10GetLayeredDeviceSize",
    "DXGID3D10RegisterLayers",
    "DXGIDeclareAdapterRemovalSupport",
    "DXGIDisableVBlankVirtualization",
    "DXGIDumpJournal",
    "DXGIGetDebugInterface1",
    "DXGIReportAdapterConfiguration",
    "PIXBeginCapture",
    "PIXEndCapture",
    "PIXGetCaptureState",
    "SetAppCompatStringPointer",
    "UpdateHMDEmulationStatus",
};
FARPROC targets[std::size(forwarded)] {};
} // namespace
extern "C" FARPROC __stdcall ForwardTarget(unsigned index)
{
    if (!targets[index])
        targets[index] = SystemProc(forwarded[index]);
    return targets[index];
}
#define FORWARD(index, name)                                                                                           \
    extern "C" __declspec(naked) void Forward_##name() { __asm push index __asm call ForwardTarget __asm jmp eax }
// The compatibility exports the shims call early: without the system DLL yet they return 0 and pop their arguments
// (as many bytes as Windows' own 32-bit dxgi.dll pops) instead of jumping to address 0
#define FORWARD_OR_ZERO(index, name, bytes)                                                                            \
    extern "C" __declspec(naked) void Forward_##name()                                                                 \
    {                                                                                                                  \
        __asm push index __asm call ForwardTarget __asm test eax, eax __asm jz none __asm jmp eax __asm none:          \
        __asm ret bytes                                                                                                \
    }
namespace
{
// SetAppCompatStringPointer(id, string) from a shim before the system DLL could load: handed over once it does
SIZE_T compatId = 0;
const char* compatString = nullptr;
LONG compatPending = 0;
void ReplayCompat()
{
    if (InterlockedExchange(&compatPending, 0) == 0)
        return;
    using Set = void(__stdcall*)(SIZE_T, const char*);
    if (const auto set = reinterpret_cast<Set>(GetProcAddress(systemDxgi, "SetAppCompatStringPointer")))
        set(compatId, compatString);
}
} // namespace
extern "C" void __stdcall Forward_SetAppCompatStringPointer(SIZE_T id, const char* string)
{
    using Set = void(__stdcall*)(SIZE_T, const char*);
    if (const auto set = reinterpret_cast<Set>(ForwardTarget(15)))
    {
        set(id, string);
        return;
    }
    compatId = id;
    compatString = string;
    InterlockedExchange(&compatPending, 1);
}
FORWARD_OR_ZERO(0, ApplyCompatResolutionQuirking, 8)
FORWARD_OR_ZERO(1, CompatString, 16)
FORWARD_OR_ZERO(2, CompatValue, 8)
FORWARD(3, DXGID3D10CreateDevice)
FORWARD(4, DXGID3D10CreateLayeredDevice)
FORWARD(5, DXGID3D10GetLayeredDeviceSize)
FORWARD(6, DXGID3D10RegisterLayers)
FORWARD(7, DXGIDeclareAdapterRemovalSupport)
FORWARD(8, DXGIDisableVBlankVirtualization)
FORWARD(9, DXGIDumpJournal)
FORWARD(10, DXGIGetDebugInterface1)
FORWARD(11, DXGIReportAdapterConfiguration)
FORWARD(12, PIXBeginCapture)
FORWARD(13, PIXEndCapture)
FORWARD(14, PIXGetCaptureState)
FORWARD(16, UpdateHMDEmulationStatus)
