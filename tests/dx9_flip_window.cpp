// What a window shows after a DXGI flip-model swapchain presented to it, the case of the D3D9 bridge's XeFG
// presenter: a classic D3D9 device presents red, a D3D12 flip swapchain presents green and is released, D3D9
// presents blue again, then a second flip swapchain on the same window presents cyan. It prints the window's mean
// colour after each step. With --no-flip-present the first swapchain is created and released without presenting.
// Standalone, without OptiScaler; build from an x64 MSVC prompt with
//   cl /nologo /O2 /EHsc /std:c++20 tests\dx9_flip_window.cpp
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <utility>
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

namespace
{
HWND window = nullptr;

void Pump()
{
    MSG message;

    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void Sample(const char* step)
{
    RECT client {};
    GetClientRect(window, &client);
    POINT origin { 0, 0 };
    ClientToScreen(window, &origin);
    HDC screen = GetDC(nullptr);
    long r = 0, g = 0, b = 0, n = 0;

    for (int y = 20; y < client.bottom; y += 40)
        for (int x = 20; x < client.right; x += 40, ++n)
        {
            const COLORREF pixel = GetPixel(screen, origin.x + x, origin.y + y);
            r += GetRValue(pixel);
            g += GetGValue(pixel);
            b += GetBValue(pixel);
        }

    ReleaseDC(nullptr, screen);
    std::printf("%-48s r %3ld g %3ld b %3ld\n", step, r / n, g / n, b / n);
}

void Present9(IDirect3DDevice9* device, D3DCOLOR colour)
{
    for (int i = 0; i < 60; ++i)
    {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, colour, 1, 0);
        device->Present(nullptr, nullptr, nullptr, nullptr);
        Pump();
        Sleep(10);
    }
}

// A flip-model swapchain on the window, cleared to a colour and presented, then released with its device objects.
void PresentFlip(int presents, float blue, const char* step)
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGIFactory4> factory;
    D3D12_COMMAND_QUEUE_DESC queueDesc {};

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) ||
        FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return;

    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> created;
    ComPtr<IDXGISwapChain3> swap;
    const HRESULT result = factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &created);
    std::printf("CreateSwapChainForHwnd: %08X\n", unsigned(result));

    if (FAILED(result) || FAILED(created.As(&swap)))
        return;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap));
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    list->Close();
    UINT64 serial = 0;

    for (int i = 0; i < presents; ++i)
    {
        ComPtr<ID3D12Resource> back;
        swap->GetBuffer(swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
        device->CreateRenderTargetView(back.Get(), nullptr, heap->GetCPUDescriptorHandleForHeapStart());
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Transition.pResource = back.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        const float colour[4] = { 0, 1, blue, 1 };
        list->ClearRenderTargetView(heap->GetCPUDescriptorHandleForHeapStart(), colour, 0, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        swap->Present(0, 0);
        queue->Signal(fence.Get(), ++serial);

        while (fence->GetCompletedValue() < serial)
            Sleep(1);

        Pump();
        Sleep(10);
    }

    Sample(step);
    created.Reset();
    std::printf("swapchain released, %lu references left\n", swap.Reset());
}
} // namespace

int main(int argc, char** argv)
{
    const bool flipPresents = !(argc > 1 && std::strcmp(argv[1], "--no-flip-present") == 0);
    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"Dx9FlipWindow";
    RegisterClassW(&windowClass);
    window = CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName, L"D3D9 flip window", WS_OVERLAPPEDWINDOW, 60, 60,
                             640, 480, nullptr, nullptr, windowClass.hInstance, nullptr);
    ShowWindow(window, SW_SHOW);
    Pump();

    IDirect3D9* api = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS params {};
    params.Windowed = TRUE;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* device = nullptr;

    if (api == nullptr ||
        FAILED(api->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device)))
        return 2;

    Present9(device, D3DCOLOR_XRGB(255, 0, 0));
    Sample("D3D9, red");
    PresentFlip(flipPresents ? 30 : 0, 0.0f, "flip swapchain, green");
    Present9(device, D3DCOLOR_XRGB(0, 0, 255));
    Sample("D3D9 after the flip swapchain, blue");
    std::printf("D3D9 Reset: %08X\n", unsigned(device->Reset(&params)));
    Present9(device, D3DCOLOR_XRGB(0, 0, 255));
    Sample("D3D9 after a Reset, blue");
    PresentFlip(30, 1.0f, "second flip swapchain, cyan");
    device->Release();
    api->Release();
    DestroyWindow(window);
    return 0;
}
