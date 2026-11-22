#include "pch.h"
#include "D3D9_Proxy.h"

#include <Logger.h>
#include <with_dx12/dx9_with_dx12.h>

#include <d3d9.h>
#include <d3d9on12.h>
#include <detours/detours.h>

namespace
{
HMODULE system9 = nullptr;
bool enabled = false;

using CreateDevice9 = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                  IDirect3DDevice9**);
using CreateDevice9Ex = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
using Present9 = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using Present9Ex = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*,
                                               DWORD);
using PresentChain9 = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*,
                                                  DWORD);
using Reset9 = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Reset9Ex = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

CreateDevice9 realCreate = nullptr;
CreateDevice9Ex realCreateEx = nullptr;
Present9 realPresent = nullptr;
Present9Ex realPresentEx = nullptr;
PresentChain9 realPresentChain = nullptr;
Reset9 realReset = nullptr;
Reset9Ex realResetEx = nullptr;

// The runtime's device Present can reach the swapchain Present: only the outer call works on the frame.
thread_local bool insidePresent = false;

template <typename Fn> Fn System(const char* name)
{
    return system9 ? reinterpret_cast<Fn>(GetProcAddress(system9, name)) : nullptr;
}

IDirect3DSwapChain9* ImplicitChain(IDirect3DDevice9* device)
{
    IDirect3DSwapChain9* chain = nullptr;
    device->GetSwapChain(0, &chain);
    return chain;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window,
                                      const RGNDATA* dirty)
{
    if (insidePresent)
        return realPresent(device, src, dst, window, dirty);

    insidePresent = true;
    HRESULT result = D3D_OK;

    if (auto chain = ImplicitChain(device); chain != nullptr)
    {
        const bool presented = Dx9WithDx12::Present(device, chain, window);
        chain->Release();

        if (!presented)
            result = realPresent(device, src, dst, window, dirty);
    }
    else
    {
        result = realPresent(device, src, dst, window, dirty);
    }

    insidePresent = false;
    return result;
}

HRESULT STDMETHODCALLTYPE HookPresentEx(IDirect3DDevice9Ex* device, const RECT* src, const RECT* dst, HWND window,
                                        const RGNDATA* dirty, DWORD flags)
{
    if (insidePresent)
        return realPresentEx(device, src, dst, window, dirty, flags);

    insidePresent = true;
    HRESULT result = D3D_OK;

    if (auto chain = ImplicitChain(device); chain != nullptr)
    {
        const bool presented = Dx9WithDx12::Present(device, chain, window);
        chain->Release();

        // A Present that may return early would run the frame's work again when the game repeats it.
        if (!presented)
            result = realPresentEx(device, src, dst, window, dirty, flags & ~D3DPRESENT_DONOTWAIT);
    }
    else
    {
        result = realPresentEx(device, src, dst, window, dirty, flags);
    }

    insidePresent = false;
    return result;
}

HRESULT STDMETHODCALLTYPE HookPresentChain(IDirect3DSwapChain9* chain, const RECT* src, const RECT* dst, HWND window,
                                           const RGNDATA* dirty, DWORD flags)
{
    if (insidePresent)
        return realPresentChain(chain, src, dst, window, dirty, flags);

    insidePresent = true;
    HRESULT result = D3D_OK;
    IDirect3DDevice9* device = nullptr;

    if (SUCCEEDED(chain->GetDevice(&device)) && device != nullptr)
    {
        const bool presented = Dx9WithDx12::Present(device, chain, window);
        device->Release();

        if (!presented)
            result = realPresentChain(chain, src, dst, window, dirty, flags & ~D3DPRESENT_DONOTWAIT);
    }
    else
    {
        result = realPresentChain(chain, src, dst, window, dirty, flags);
    }

    insidePresent = false;
    return result;
}

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params)
{
    Dx9WithDx12::BeforeReset(device, params);
    const HRESULT result = realReset(device, params);
    LOG_INFO("D3D9 Reset: {:X}, {}x{}, windowed {}", (UINT) result, params ? params->BackBufferWidth : 0,
             params ? params->BackBufferHeight : 0, params ? params->Windowed : 0);
    return result;
}

HRESULT STDMETHODCALLTYPE HookResetEx(IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* mode)
{
    Dx9WithDx12::BeforeReset(device, params);
    const HRESULT result = realResetEx(device, params, mode);
    LOG_INFO("D3D9 ResetEx: {:X}, {}x{}, windowed {}", (UINT) result, params ? params->BackBufferWidth : 0,
             params ? params->BackBufferHeight : 0, params ? params->Windowed : 0);
    return result;
}

void HookDevice(IDirect3DDevice9* device)
{
    auto table = *reinterpret_cast<void***>(device);
    IDirect3DSwapChain9* chain = ImplicitChain(device);
    IDirect3DDevice9Ex* deviceEx = nullptr;
    device->QueryInterface(IID_PPV_ARGS(&deviceEx));

    if (realPresent != nullptr && realPresentChain != nullptr && (deviceEx == nullptr || realPresentEx != nullptr))
    {
        if (chain != nullptr)
            chain->Release();

        if (deviceEx != nullptr)
            deviceEx->Release();

        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (realPresent == nullptr)
    {
        realPresent = reinterpret_cast<Present9>(table[17]);
        DetourAttach(&(PVOID&) realPresent, HookPresent);
        realReset = reinterpret_cast<Reset9>(table[16]);
        DetourAttach(&(PVOID&) realReset, HookReset);
    }

    if (realPresentChain == nullptr && chain != nullptr)
    {
        realPresentChain = reinterpret_cast<PresentChain9>((*reinterpret_cast<void***>(chain))[3]);
        DetourAttach(&(PVOID&) realPresentChain, HookPresentChain);
    }

    if (realPresentEx == nullptr && deviceEx != nullptr)
    {
        auto tableEx = *reinterpret_cast<void***>(deviceEx);
        realPresentEx = reinterpret_cast<Present9Ex>(tableEx[121]);
        DetourAttach(&(PVOID&) realPresentEx, HookPresentEx);
        realResetEx = reinterpret_cast<Reset9Ex>(tableEx[132]);
        DetourAttach(&(PVOID&) realResetEx, HookResetEx);
    }

    const LONG result = DetourTransactionCommit();
    LOG_INFO("D3D9 device hooks installed: {}", result);

    if (chain != nullptr)
        chain->Release();

    if (deviceEx != nullptr)
        deviceEx->Release();
}

void LogDevice(const char* api, HRESULT result, HWND focus, DWORD flags, const D3DPRESENT_PARAMETERS* params)
{
    if (params == nullptr)
        return;

    LOG_INFO("{}: {:X}, {}x{} format {}, {} back buffers, MSAA {}, swap effect {}, windowed {}, interval {:X}, "
             "window {:X}, focus {:X}, flags {:X}",
             api, (UINT) result, params->BackBufferWidth, params->BackBufferHeight, (UINT) params->BackBufferFormat,
             params->BackBufferCount, (UINT) params->MultiSampleType, (UINT) params->SwapEffect, params->Windowed,
             params->PresentationInterval, (size_t) params->hDeviceWindow, (size_t) focus, flags);
}

HRESULT STDMETHODCALLTYPE HookCreate(IDirect3D9* api, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                     D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** out)
{
    const HRESULT result = realCreate(api, adapter, type, window, flags, params, out);
    LogDevice("D3D9 CreateDevice", result, window, flags, params);

    if (SUCCEEDED(result) && out != nullptr && *out != nullptr)
        HookDevice(*out);

    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateEx(IDirect3D9Ex* api, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                       D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** out)
{
    const HRESULT result = realCreateEx(api, adapter, type, window, flags, params, mode, out);
    LogDevice("D3D9 CreateDeviceEx", result, window, flags, params);

    if (SUCCEEDED(result) && out != nullptr && *out != nullptr)
        HookDevice(*out);

    return result;
}

void HookApi(IDirect3D9* api)
{
    auto table = *reinterpret_cast<void***>(api);
    IDirect3D9Ex* apiEx = nullptr;
    api->QueryInterface(IID_PPV_ARGS(&apiEx));

    if (realCreate != nullptr && (apiEx == nullptr || realCreateEx != nullptr))
    {
        if (apiEx != nullptr)
            apiEx->Release();

        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (realCreate == nullptr)
    {
        realCreate = reinterpret_cast<CreateDevice9>(table[16]);
        DetourAttach(&(PVOID&) realCreate, HookCreate);
    }

    if (realCreateEx == nullptr && apiEx != nullptr)
    {
        realCreateEx = reinterpret_cast<CreateDevice9Ex>((*reinterpret_cast<void***>(apiEx))[20]);
        DetourAttach(&(PVOID&) realCreateEx, HookCreateEx);
    }

    DetourTransactionCommit();

    if (apiEx != nullptr)
        apiEx->Release();
}
} // namespace

namespace D3D9Proxy
{
void Init(HMODULE module, bool hook)
{
    system9 = module;
    enabled = hook;
}

HMODULE Module() { return system9; }
} // namespace D3D9Proxy

IDirect3D9* WINAPI _Direct3DCreate9(UINT version)
{
    auto create = System<decltype(&Direct3DCreate9)>("Direct3DCreate9");
    auto api = create ? create(version) : nullptr;

    if (api != nullptr && enabled)
        HookApi(api);

    return api;
}

HRESULT WINAPI _Direct3DCreate9Ex(UINT version, IDirect3D9Ex** out)
{
    auto create = System<decltype(&Direct3DCreate9Ex)>("Direct3DCreate9Ex");
    const HRESULT result = create ? create(version, out) : E_FAIL;

    if (SUCCEEDED(result) && out != nullptr && *out != nullptr && enabled)
        HookApi(*out);

    return result;
}

IDirect3D9* WINAPI _Direct3DCreate9On12(UINT version, D3D9ON12_ARGS* args, UINT count)
{
    auto create = System<decltype(&Direct3DCreate9On12)>("Direct3DCreate9On12");
    auto api = create ? create(version, args, count) : nullptr;

    if (api != nullptr && enabled)
        HookApi(api);

    return api;
}

HRESULT WINAPI _Direct3DCreate9On12Ex(UINT version, D3D9ON12_ARGS* args, UINT count, IDirect3D9Ex** out)
{
    auto create = System<decltype(&Direct3DCreate9On12Ex)>("Direct3DCreate9On12Ex");
    const HRESULT result = create ? create(version, args, count, out) : E_FAIL;

    if (SUCCEEDED(result) && out != nullptr && *out != nullptr && enabled)
        HookApi(*out);

    return result;
}

// The remaining exports of the system d3d9.dll pass straight through. The undocumented ones forward as many
// pointer-sized arguments as they take and whatever they return, which the x64 calling convention carries unchanged
// (DebugSetLevel and DebugSetMute are empty in the system DLL). Its six unnamed ordinals (16-19, 22, 23, settings the
// compatibility shims set) are not exported: they need ordinals below 100, which would renumber the export table
// that every proxy name shares. Looking one up here returns null.
#define D3D9_FORWARD(ret, name, params, args, fallback)                                                                \
    ret WINAPI _##name params                                                                                          \
    {                                                                                                                  \
        auto fn = System<ret(WINAPI*) params>(#name);                                                                  \
        return fn ? fn args : fallback;                                                                                \
    }
#define D3D9_FORWARD_VOID(name, params, args)                                                                          \
    void WINAPI _##name params                                                                                         \
    {                                                                                                                  \
        if (auto fn = System<void(WINAPI*) params>(#name))                                                             \
            fn args;                                                                                                   \
    }

D3D9_FORWARD(int, D3DPERF_BeginEvent, (D3DCOLOR a, LPCWSTR b), (a, b), 0)
D3D9_FORWARD(int, D3DPERF_EndEvent, (), (), 0)
D3D9_FORWARD(DWORD, D3DPERF_GetStatus, (), (), 0)
D3D9_FORWARD(BOOL, D3DPERF_QueryRepeatFrame, (), (), FALSE)
D3D9_FORWARD_VOID(D3DPERF_SetMarker, (D3DCOLOR a, LPCWSTR b), (a, b))
D3D9_FORWARD_VOID(D3DPERF_SetOptions, (DWORD a), (a))
D3D9_FORWARD_VOID(D3DPERF_SetRegion, (D3DCOLOR a, LPCWSTR b), (a, b))
D3D9_FORWARD(void*, Direct3DShaderValidatorCreate9, (), (), nullptr)
D3D9_FORWARD(INT_PTR, DebugSetLevel, (), (), 0)
D3D9_FORWARD(INT_PTR, Direct3D9EnableMaximizedWindowedModeShim, (UINT_PTR a), (a), 0)
D3D9_FORWARD(INT_PTR, PSGPError, (void* a, UINT_PTR b, UINT_PTR c), (a, b, c), 0)
D3D9_FORWARD(INT_PTR, PSGPSampleTexture, (void* a, UINT_PTR b, void* c, UINT_PTR d, void* e), (a, b, c, d, e), 0)

#undef D3D9_FORWARD
#undef D3D9_FORWARD_VOID
