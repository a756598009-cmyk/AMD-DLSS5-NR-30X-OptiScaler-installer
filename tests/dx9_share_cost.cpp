// Cost of moving a 1920x1080 D3D9Ex backbuffer into a D3D12 texture: the system-memory copy the D3D9 bridge uses
// vs a D3D9Ex shared surface opened by D3D11 and passed to D3D12 through an NT handle. Alternates the two each
// frame and prints their averages. Standalone, without OptiScaler; build from an x64 MSVC prompt with
//   cl /nologo /O2 /EHsc /std:c++20 tests\dx9_share_cost.cpp
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
using Microsoft::WRL::ComPtr;
using clk = std::chrono::steady_clock;
static double ms(clk::time_point a, clk::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main()
{
    const UINT W = 1920, H = 1080;
    HWND w = CreateWindowW(L"STATIC", L"paths", WS_POPUP | WS_VISIBLE, 0, 0, W, H, 0, 0, 0, 0);
    ComPtr<IDirect3D9Ex> api;
    Direct3DCreate9Ex(D3D_SDK_VERSION, &api);
    D3DPRESENT_PARAMETERS pp {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.hDeviceWindow = w;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    ComPtr<IDirect3DDevice9Ex> dev;
    if (FAILED(api->CreateDeviceEx(0, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, nullptr, &dev)))
        return 1;

    ComPtr<ID3D12Device> d12;
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12));
    D3D12_COMMAND_QUEUE_DESC qd {};
    ComPtr<ID3D12CommandQueue> q;
    d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    ComPtr<ID3D12CommandAllocator> alloc;
    d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ComPtr<ID3D12GraphicsCommandList> list;
    d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
    list->Close();
    ComPtr<ID3D12Fence> fence;
    d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 serial = 0;
    HANDLE ev = CreateEventW(0, 0, 0, 0);
    auto wait12 = [&]
    {
        q->Signal(fence.Get(), ++serial);
        if (fence->GetCompletedValue() < serial)
        {
            fence->SetEventOnCompletion(serial, ev);
            WaitForSingleObject(ev, INFINITE);
        }
    };

    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W;
    td.Height = H;
    td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> frame;
    d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                 IID_PPV_ARGS(&frame));
    const UINT pitch = (W * 4 + 255) & ~255u;
    D3D12_HEAP_PROPERTIES up {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = UINT64(pitch) * H;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    d12->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                 IID_PPV_ARGS(&upload));
    BYTE* mapped = nullptr;
    upload->Map(0, nullptr, (void**) &mapped);
    ComPtr<IDirect3DSurface9> sys;
    dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);

    // Shared chain: D3D9Ex texture (legacy handle) -> D3D11 -> NT handle texture -> D3D12.
    ComPtr<IDirect3DTexture9> shared9;
    HANDLE legacy = nullptr;
    dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &shared9, &legacy);
    ComPtr<IDirect3DSurface9> shared9s;
    shared9->GetSurfaceLevel(0, &shared9s);
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> c11;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d11, nullptr,
                      &c11);
    ComPtr<ID3D11Texture2D> from9;
    d11->OpenSharedResource(legacy, IID_PPV_ARGS(&from9));
    D3D11_TEXTURE2D_DESC nd {};
    nd.Width = W;
    nd.Height = H;
    nd.MipLevels = nd.ArraySize = nd.SampleDesc.Count = 1;
    nd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    nd.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ComPtr<ID3D11Texture2D> nt11;
    d11->CreateTexture2D(&nd, nullptr, &nt11);
    ComPtr<IDXGIResource1> res;
    nt11.As(&res);
    HANDLE nt = nullptr;
    res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &nt);
    ComPtr<ID3D12Resource> nt12;
    d12->OpenSharedHandle(nt, IID_PPV_ARGS(&nt12));
    std::printf("shared chain ready: %d %d %d\n", from9 != nullptr, nt11 != nullptr, nt12 != nullptr);
    ComPtr<ID3D12Resource> direct12;
    std::printf("D3D12 opening the D3D9Ex handle directly: %08X\n",
                unsigned(d12->OpenSharedHandle(legacy, IID_PPV_ARGS(&direct12))));

    auto wait9 = [&]
    {
        ComPtr<IDirect3DQuery9> qq;
        dev->CreateQuery(D3DQUERYTYPE_EVENT, &qq);
        qq->Issue(D3DISSUE_END);
        while (qq->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
        {
        }
    };
    auto wait11 = [&]
    {
        D3D11_QUERY_DESC qd2 { D3D11_QUERY_EVENT };
        ComPtr<ID3D11Query> qq;
        d11->CreateQuery(&qd2, &qq);
        c11->End(qq.Get());
        while (c11->GetData(qq.Get(), nullptr, 0, 0) == S_FALSE)
        {
        }
    };

    double cpu = 0, gpu = 0;
    int n = 0;
    for (int i = 0; i < 400; ++i)
    {
        ComPtr<IDirect3DSurface9> back;
        dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
        D3DRECT r { LONG(i * 7 % 1800), 100, LONG(i * 7 % 1800 + 100), 300 };
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(i & 255, 40, 90), 1, 0);
        dev->Clear(1, &r, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 255, 255), 1, 0);
        auto a = clk::now();
        if (i % 2 == 0)
        {
            dev->GetRenderTargetData(back.Get(), sys.Get());
            D3DLOCKED_RECT lr {};
            sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
            for (UINT y = 0; y < H; ++y)
                memcpy(mapped + size_t(y) * pitch, (BYTE*) lr.pBits + size_t(y) * lr.Pitch, W * 4);
            sys->UnlockRect();
            alloc->Reset();
            list->Reset(alloc.Get(), nullptr);
            D3D12_TEXTURE_COPY_LOCATION dst { frame.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            D3D12_TEXTURE_COPY_LOCATION src { upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, W, H, 1, pitch };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            list->Close();
            ID3D12CommandList* l[] = { list.Get() };
            q->ExecuteCommandLists(1, l);
            wait12();
            if (i >= 100)
                cpu += ms(a, clk::now());
        }
        else
        {
            dev->StretchRect(back.Get(), nullptr, shared9s.Get(), nullptr, D3DTEXF_NONE);
            wait9();
            c11->CopyResource(nt11.Get(), from9.Get());
            wait11();
            alloc->Reset();
            list->Reset(alloc.Get(), nullptr);
            list->CopyResource(frame.Get(), nt12.Get());
            list->Close();
            ID3D12CommandList* l[] = { list.Get() };
            q->ExecuteCommandLists(1, l);
            wait12();
            if (i >= 100)
            {
                gpu += ms(a, clk::now());
                ++n;
            }
        }
        dev->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
        MSG m;
        while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE))
            DispatchMessageW(&m);
    }
    std::printf("1920x1080 into a D3D12 texture: system memory %.3f ms, shared surfaces %.3f ms (%d frames each)\n",
                cpu / n, gpu / n, n);
}
