// A D3D10 game stand-in for final-image NR: loads OptiScaler as dxgi.dll from its own folder, presents a moving
// pattern from a D3D10 swapchain (one sequential back buffer, so the frame NR wrote stays readable after Present) and
// checks that the frame changed and that OptiScaler took the D3D10 route. The folder also holds OptiScaler.ini, the
// runtime NrBackend names with its data, and the OptiScaler\ folder of a release.
// Built for x86 beside the 32-bit dxgi.dll proxy (tools\dx9-x86\proxy_dxgi.cpp), whose host in OptiScaler64\ runs
// OptiScaler and presents the frames itself: then it checks the frames the host presented and the host's logs.
//
//   dx10_final_image_smoke.exe [seconds] [d3d10_1|d3d10] [full|r10]
// r10: an R10G10B10A2 back buffer in the default sRGB colour space (64-bit only).
// full: the swap chain is made fullscreen; on x86 the window must turn borderless over its monitor and the game be
// told it is fullscreen. On x86 a DirectInput mouse is made too, which the proxy hooks for the menu.
#define NOMINMAX
#include <windows.h>
#include <d3d10_1.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../tools/dx9-x86/dx9_wire.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static int failures = 0;
static void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += ok ? 0 : 1;
}
static std::string Read(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

int wmain(int argc, wchar_t** argv)
{
    const int seconds = argc > 1 ? _wtoi(argv[1]) : 40;
    const bool ten1 = argc > 2 && !wcscmp(argv[2], L"d3d10_1");
    const bool full = argc > 3 && !wcscmp(argv[3], L"full");
    const bool r10 = argc > 3 && !wcscmp(argv[3], L"r10");
    wchar_t self[MAX_PATH] {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path dir = fs::path(self).parent_path();
    // OptiScaler before d3d10.dll, which then takes dxgi.dll from it as a game's import would.
    const HMODULE proxy = LoadLibraryW((dir / L"dxgi.dll").c_str());
    const bool x86 = fs::exists(dir / L"OptiScaler64");
    const fs::path logs = x86 ? dir / L"OptiScaler64" : dir;
    if (!proxy)
    {
        std::printf("FAIL dxgi.dll (OptiScaler) did not load: %lu\n", GetLastError());
        return 2;
    }
    const UINT w = 1280, h = 720;
    WNDCLASSW type {};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"Dx10FinalImageSmoke";
    RegisterClassW(&type);
    RECT r { 0, 0, LONG(w), LONG(h) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowW(type.lpszClassName, L"D3D10 final image", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, type.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC desc {};
    desc.BufferDesc.Width = w;
    desc.BufferDesc.Height = h;
    desc.BufferDesc.Format = r10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = window;
    desc.Windowed = full ? FALSE : TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
    ComPtr<ID3D10Device> device;
    ComPtr<IDXGISwapChain> swap;
    HRESULT hr;
    if (ten1)
    {
        ComPtr<ID3D10Device1> device1;
        hr = D3D10CreateDeviceAndSwapChain1(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1,
                                            D3D10_1_SDK_VERSION, &desc, &swap, &device1);
        device = device1;
    }
    else
        hr = D3D10CreateDeviceAndSwapChain(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_SDK_VERSION, &desc,
                                           &swap, &device);
    if (FAILED(hr))
    {
        std::printf("FAIL D3D10CreateDeviceAndSwapChain%s: %08lX\n", ten1 ? "1" : "", hr);
        return 2;
    }
    ComPtr<IDirectInput8W> input;
    ComPtr<IDirectInputDevice8W> mouse;
    if (x86 && SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                            reinterpret_cast<void**>(input.GetAddressOf()), nullptr)))
        input->CreateDevice(GUID_SysMouse, &mouse, nullptr);
    ComPtr<ID3D10Texture2D> back, staging, source;
    swap->GetBuffer(0, IID_PPV_ARGS(&back));
    D3D10_TEXTURE2D_DESC td {};
    back->GetDesc(&td);
    td.BindFlags = 0;
    td.Usage = D3D10_USAGE_STAGING;
    td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    device->CreateTexture2D(&td, nullptr, &staging);
    td.Usage = D3D10_USAGE_DEFAULT;
    td.CPUAccessFlags = 0;
    device->CreateTexture2D(&td, nullptr, &source);
    std::vector<uint32_t> pixels(size_t(w) * h);
    const DWORD start = GetTickCount();
    unsigned frames = 0;
    double changed = 0;
    while (GetTickCount() - start < DWORD(seconds) * 1000)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
        // Soft shapes with grain that pan slowly: something the network visibly works on.
        const float t = frames * 0.5f;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                const float fx = (x + t) * 0.02f, fy = y * 0.02f;
                const float v = 0.5f + 0.25f * std::sin(fx) * std::cos(fy) + 0.1f * std::sin(fx * 7.1f + fy * 3.3f);
                const uint32_t n = (x * 73856093u ^ y * 19349663u ^ frames * 83492791u) >> 27;
                const auto c = [&](float k) { return uint32_t(std::clamp(v * k * 255.f + n, 0.f, 255.f)); };
                const auto c10 = [&](float k) { return uint32_t(std::clamp((v * k * 255.f + n) * 4.f, 0.f, 1023.f)); };
                pixels[size_t(y) * w + x] = r10 ? c10(1.f) | c10(.8f) << 10 | c10(.6f) << 20 | 3u << 30
                                                : c(1.f) | c(0.8f) << 8 | c(0.6f) << 16 | 0xFF000000u;
            }
        device->UpdateSubresource(source.Get(), 0, nullptr, pixels.data(), w * 4, 0);
        device->CopyResource(back.Get(), source.Get());
        swap->Present(0, 0);
        ++frames;
        // What Present showed: NR wrote it into the back buffer before the swapchain presented it.
        if (frames % 60 == 0)
        {
            device->CopyResource(staging.Get(), back.Get());
            D3D10_MAPPED_TEXTURE2D map {};
            if (SUCCEEDED(staging->Map(0, D3D10_MAP_READ, 0, &map)))
            {
                uint64_t diff = 0;
                for (UINT y = 0; y < h; ++y)
                    for (UINT x = 0; x < w; ++x)
                    {
                        const uint32_t a = static_cast<const uint32_t*>(map.pData)[size_t(y) * (map.RowPitch / 4) + x];
                        const uint32_t b = pixels[size_t(y) * w + x];
                        if (r10)
                            for (int k = 0; k < 30; k += 10)
                                diff += std::abs(int((a >> k) & 1023) - int((b >> k) & 1023)) / 4;
                        else
                            for (int k = 0; k < 24; k += 8)
                                diff += std::abs(int((a >> k) & 255) - int((b >> k) & 255));
                    }
                staging->Unmap(0);
                changed = double(diff) / (double(w) * h * 3);
                std::printf("frame %u: mean change %.3f\n", frames, changed);
            }
        }
    }
    if (full && x86)
    {
        BOOL fullscreen = FALSE;
        swap->GetFullscreenState(&fullscreen, nullptr);
        RECT rect {};
        GetWindowRect(window, &rect);
        MONITORINFO monitor { sizeof(monitor) };
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        std::printf("window %ldx%ld, monitor %ldx%ld, fullscreen state %d\n", rect.right - rect.left,
                    rect.bottom - rect.top, monitor.rcMonitor.right - monitor.rcMonitor.left,
                    monitor.rcMonitor.bottom - monitor.rcMonitor.top, fullscreen);
        expect(EqualRect(&rect, &monitor.rcMonitor) != FALSE, "the window is borderless over its monitor");
        expect(fullscreen != FALSE, "the game is told it is fullscreen");
    }
    swap.Reset();
    back.Reset();
    std::printf("%u frames in %d s\n", frames, seconds);
    const std::string log = Read(logs / L"OptiScaler.log");
    if (x86)
    {
        dx9wire::ProxyStatus state {};
        using ReadStatus = void(WINAPI*)(dx9wire::ProxyStatus*);
        if (auto read = reinterpret_cast<ReadStatus>(GetProcAddress(proxy, "OptiScalerX86Status")))
            read(&state);
        std::printf("bridged %u native %u host starts %u faults %u transport %u %ux%u\n", state.bridged, state.native,
                    state.hostStarts, state.faults, state.transport, state.width, state.height);
        expect(state.bridged > frames / 2, "the host presented most frames");
        expect(state.faults == 0, "no host fault");
        expect(Read(logs / L"dxgi-x86.log").find("DirectInput wide devices hooked") != std::string::npos,
               "the proxy holds DirectInput for the menu");
    }
    else
    {
        expect(changed > 0.5, "the frames Present showed came out of the network");
        expect(log.find("D3D10 frames cross to a D3D11 device") != std::string::npos,
               "OptiScaler took the D3D10 route");
    }
    expect(log.find("Final-image NR: first GPU time of the model") != std::string::npos, "the network ran");
    expect(log.find("NR without upscaling: D3D10") == std::string::npos || log.find("timed out") == std::string::npos,
           "no D3D10 timeout");
    return failures ? 1 : 0;
}
