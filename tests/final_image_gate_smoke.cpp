// A D3D11 program with no upscaler that presents a few hundred frames through whatever dxgi.dll sits beside
// it, at 640x360 or the size given after the frame count. tools\test-final-image-gate.ps1 puts OptiScaler.dll
// there as dxgi.dll with an INI per case and reads OptiScaler.log and amd_presr.log afterwards. Exits 0 once
// every frame was presented.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv)
{
    const int frames = argc > 1 ? _wtoi(argv[1]) : 300;
    const UINT width = argc > 3 ? UINT(_wtoi(argv[2])) : 640, height = argc > 3 ? UINT(_wtoi(argv[3])) : 360;
    // The proxy beside the executable, before d3d11.dll pulls in the system one.
    if (!LoadLibraryW(L"dxgi.dll"))
    {
        std::printf("dxgi.dll did not load: %lu\n", GetLastError());
        return 2;
    }
    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"OptiFinalImageGateSmoke";
    RegisterClassW(&windowClass);
    HWND window = CreateWindowW(windowClass.lpszClassName, L"Final image gate smoke", WS_OVERLAPPEDWINDOW, 0, 0,
                                int(width), int(height), nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window)
        return 3;
    ShowWindow(window, SW_SHOWNOACTIVATE);

    DXGI_SWAP_CHAIN_DESC desc {};
    desc.BufferDesc.Width = width;
    desc.BufferDesc.Height = height;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &desc, &swapchain, &device, nullptr, &context);
    if (FAILED(hr))
    {
        std::printf("D3D11CreateDeviceAndSwapChain failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return 4;
    }
    ComPtr<ID3D11Texture2D> back;
    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) ||
        FAILED(device->CreateRenderTargetView(back.Get(), nullptr, &target)))
        return 5;

    for (int i = 0; i < frames; ++i)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&message);
        // A slow gradient sweep, so consecutive frames differ a little as a game's do.
        const float t = float(i % 120) / 120.f;
        const float colour[4] = { t, 0.5f * (1.f - t), 0.25f + 0.5f * t, 1.f };
        ID3D11RenderTargetView* targets[] = { target.Get() };
        context->OMSetRenderTargets(1, targets, nullptr);
        context->ClearRenderTargetView(target.Get(), colour);
        hr = swapchain->Present(0, 0);
        if (FAILED(hr))
        {
            std::printf("Present %d failed: 0x%08lx\n", i, static_cast<unsigned long>(hr));
            return 6;
        }
    }
    std::printf("presented %d frames\n", frames);
    target.Reset();
    back.Reset();
    swapchain.Reset();
    context.Reset();
    device.Reset();
    DestroyWindow(window);
    return 0;
}
