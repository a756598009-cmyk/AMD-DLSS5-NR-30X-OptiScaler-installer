// A 32-bit D3D11 game stand-in for the x86 dxgi.dll proxy (tools\dx9-x86\proxy_dxgi.cpp), built into its folder
// beside the proxy and OptiScaler64\ (host, OptiScaler and a runtime). It does not load the proxy itself: d3d11.dll
// takes dxgi.dll from the program's folder, as a game's does. It presents a moving pattern, holds it still while it
// reads the screen over its window, opens and closes the menu, resizes the window and (full) goes fullscreen and back,
// then checks the proxy's counters, the screen and the host's logs (tests\x86_look.h).
//
//   dx11_x86_smoke.exe [seconds] [create|factory|flip] [full]
// create: D3D11CreateDeviceAndSwapChain. factory: D3D11CreateDevice and the adapter's factory's CreateSwapChain.
// flip: CreateSwapChainForHwnd with a flip swap chain of two buffers.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "x86_look.h"

using Microsoft::WRL::ComPtr;
using namespace x86look;
namespace fs = std::filesystem;

int wmain(int argc, wchar_t** argv)
{
    const int seconds = argc > 1 ? _wtoi(argv[1]) : 40;
    const std::wstring mode = argc > 2 ? argv[2] : L"create";
    const bool full = argc > 3 && !wcscmp(argv[3], L"full");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t self[MAX_PATH] {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path dir = fs::path(self).parent_path(), logs = dir / L"OptiScaler64";
    UINT w = 1280, h = 720;
    WNDCLASSW type {};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursor(nullptr, IDC_ARROW);
    type.lpszClassName = L"Dx11X86Smoke";
    RegisterClassW(&type);
    RECT r { 0, 0, LONG(w), LONG(h) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowW(type.lpszClassName, L"D3D11 x86", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, type.hInstance, nullptr);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    DXGI_SWAP_CHAIN_DESC desc {};
    desc.BufferDesc.Width = w;
    desc.BufferDesc.Height = h;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    HRESULT hr = E_FAIL;
    if (mode == L"create")
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                           &desc, &swap, &device, nullptr, &context);
    else
    {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                               nullptr, &context);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (SUCCEEDED(hr))
            hr = device.As(&dxgi);
        if (SUCCEEDED(hr))
            hr = dxgi->GetAdapter(&adapter);
        if (SUCCEEDED(hr))
            hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr) && mode == L"flip")
        {
            DXGI_SWAP_CHAIN_DESC1 flip {};
            flip.Width = w;
            flip.Height = h;
            flip.Format = desc.BufferDesc.Format;
            flip.SampleDesc.Count = 1;
            flip.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            flip.BufferCount = 2;
            flip.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            flip.Flags = desc.Flags;
            ComPtr<IDXGISwapChain1> swap1;
            hr = factory->CreateSwapChainForHwnd(device.Get(), window, &flip, nullptr, nullptr, &swap1);
            swap = swap1;
        }
        else if (SUCCEEDED(hr))
            hr = factory->CreateSwapChain(device.Get(), &desc, &swap);
    }
    if (FAILED(hr))
    {
        std::printf("FAIL the device and swap chain (%ls): %08lX\n", mode.c_str(), hr);
        return 2;
    }
    // The proxy came in through d3d11.dll's own import.
    const HMODULE proxy = GetModuleHandleW(L"dxgi.dll");
    wchar_t loaded[MAX_PATH] {};
    GetModuleFileNameW(proxy, loaded, MAX_PATH);
    std::printf("dxgi.dll: %ls\n", loaded);
    if (!GetProcAddress(proxy, "OptiScalerX86Status"))
    {
        std::printf("FAIL d3d11.dll did not take the proxy as its dxgi.dll\n");
        return 2;
    }
    Run run { window, proxy };
    std::vector<uint32_t> pixels;
    Phases at { 0.30, 0.45, 0.60, 0.72, 0.86 };
    const UINT resizedW = 1024, resizedH = 576;
    bool fullSeen = false, toldFull = false, windowedAgain = false;
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
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(window, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        }
        if (full && (frame == at.full || frame == at.windowed))
            swap->SetFullscreenState(frame == at.full, nullptr);
        // As a game does on WM_SIZE: the buffers follow the client area.
        RECT client {};
        GetClientRect(window, &client);
        if (client.right > 0 && client.bottom > 0 && (UINT(client.right) != w || UINT(client.bottom) != h))
        {
            w = UINT(client.right);
            h = UINT(client.bottom);
            hr = swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, desc.Flags);
            std::printf("frame %u: buffers resized to %ux%u (%08lX)\n", frame, w, h, hr);
        }
        if (full && frame == at.check)
        {
            BOOL state = FALSE;
            swap->GetFullscreenState(&state, nullptr);
            GetWindowRect(window, &client);
            MONITORINFO monitor { sizeof(monitor) };
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            fullSeen = EqualRect(&client, &monitor.rcMonitor) != FALSE;
            toldFull = state != FALSE;
        }
        if (full && frame == at.last)
        {
            GetClientRect(window, &client);
            windowedAgain = UINT(client.right) == resizedW && UINT(client.bottom) == resizedH;
        }
        if (frame < at.still || frame >= at.look)
            ++scene;
        Scene(pixels, w, h, scene);
        ComPtr<ID3D11Texture2D> back;
        swap->GetBuffer(0, IID_PPV_ARGS(&back));
        context->UpdateSubresource(back.Get(), 0, nullptr, pixels.data(), w * 4, 0);
        back.Reset();
        swap->Present(1, 0);
        ++frames;
        if (frame == at.look - 1)
            run.still = LookAt(window, pixels, w, h);
        run.Menu(frame, at.menu);
    }
    if (full)
    {
        Expect(fullSeen, "fullscreen: the window is borderless over its monitor");
        Expect(toldFull, "fullscreen: the game is told it is fullscreen");
        Expect(windowedAgain, "windowed again at its own size");
        swap->SetFullscreenState(FALSE, nullptr);
    }
    std::printf("%u frames in %lu ms\n", frames, GetTickCount() - start);
    const DWORD closing = GetTickCount();
    swap.Reset();
    context.Reset();
    device.Reset();
    DestroyWindow(window);
    std::printf("released in %lu ms\n", GetTickCount() - closing);
    run.Report(frames, resizedW, resizedH, logs, "dxgi-x86.log");
    return failures ? 1 : 0;
}
