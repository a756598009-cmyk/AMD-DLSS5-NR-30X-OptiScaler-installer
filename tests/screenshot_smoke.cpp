// The screenshot key on each API OptiScaler draws its overlay on: a small game clears its back buffer to two
// colours (top and bottom half), the menu and the FPS overlay are on screen, the key is pressed with real input
// from a second copy of this program, and the PNG (and the JPEG XR on HDR swapchains) written to shots\ must hold
// the two colours only: the frame without OptiScaler's overlays. The screen is read too, to show the menu was there.
// The menu is open, so a second "... (menu).png" of the same frame must be there, with the menu on it.
// With chord, the key is Ctrl+F6: F6 alone must save nothing, Ctrl+F6 must save. With nomenu the INI has
// ScreenshotWithMenu=false: one file. With pictures the INI leaves ScreenshotPath empty: the files go to
// <Pictures>\OptiScaler\screenshot_smoke, and the test deletes the ones it made.
//
//   screenshot_smoke.exe dx11|dx12|dx12-fp16|dx12-hdr10|vk|gl|dx9 [chord|nomenu|pictures]
// Beside it: OptiScaler as dxgi.dll (dx11, dx12), OptiScaler.dll (vk), opengl32.dll (gl) or d3d9.dll (dx9), and an
// OptiScaler.ini with ScreenshotPath=shots (tools\test-screenshot.cmd sets it all up). Exit code: failed checks.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <GL/gl.h>
#include <shlobj.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static int failures = 0;
static HWND window = nullptr;
constexpr UINT W = 640, H = 360;

static void Expect(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    failures += !ok;
}

[[noreturn]] static void Die(const char* what, long code = 0)
{
    std::printf("FAIL setup: %s (%ld)\n", what, code);
    std::fflush(stdout);
    ExitProcess(100);
}

static fs::path Here()
{
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

// ---- the expected picture ---------------------------------------------------------------------------------

struct Rgb8
{
    int r, g, b;
};

// SDR clears, as 8-bit values.
constexpr Rgb8 SdrTop { 200, 40, 90 }, SdrBottom { 20, 160, 230 };
// scRGB clears for the half float swapchain (1.0 = 80 nits).
constexpr float FpTop[3] { 0.5f, 0.25f, 0.125f }, FpBottom[3] { 8.0f, 4.0f, 1.0f };
// PQ code values for the HDR10 swapchain.
constexpr float PqTop[3] { 0.50f, 0.42f, 0.30f }, PqBottom[3] { 0.75f, 0.70f, 0.60f };

static float PqToNits(float v)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float p = std::pow(std::max(v, 0.0f), 1.0f / m2);
    return 10000.0f * std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

static int Srgb8(float l)
{
    l = std::clamp(l, 0.0f, 1.0f);
    const float s = l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    return int(s * 255.0f + 0.5f);
}

// The SDR picture OptiScaler makes of linear Rec.709 nits: 203 nits white, soft roll-off above 0.8.
static Rgb8 ToneMapped(const float nits[3])
{
    float c[3];
    for (int i = 0; i < 3; ++i)
        c[i] = std::max(nits[i] / 203.0f, 0.0f);
    const float peak = std::max({ c[0], c[1], c[2] });
    if (peak > 0.8f)
    {
        const float scale = (0.8f + 0.2f * (1.0f - std::exp(-(peak - 0.8f) / 0.2f))) / peak;
        for (float& v : c)
            v *= scale;
    }
    return { Srgb8(c[0]), Srgb8(c[1]), Srgb8(c[2]) };
}

static void FpNits(const float sc[3], float nits[3])
{
    for (int i = 0; i < 3; ++i)
        nits[i] = sc[i] * 80.0f;
}

static void PqNits(const float pq[3], float nits[3])
{
    float n[3];
    for (int i = 0; i < 3; ++i)
        n[i] = PqToNits(std::round(pq[i] * 1023.0f) / 1023.0f);
    nits[0] = 1.660491f * n[0] - 0.587641f * n[1] - 0.072850f * n[2];
    nits[1] = -0.124550f * n[0] + 1.132900f * n[1] - 0.008349f * n[2];
    nits[2] = -0.018151f * n[0] - 0.100579f * n[1] + 1.118730f * n[2];
}

enum class Kind
{
    Sdr,
    Fp16,
    Hdr10
};
static Kind kind = Kind::Sdr;

static void Expected(Rgb8& top, Rgb8& bottom)
{
    float a[3], b[3];
    switch (kind)
    {
    case Kind::Fp16:
        FpNits(FpTop, a), FpNits(FpBottom, b);
        top = ToneMapped(a), bottom = ToneMapped(b);
        break;
    case Kind::Hdr10:
        PqNits(PqTop, a), PqNits(PqBottom, b);
        top = ToneMapped(a), bottom = ToneMapped(b);
        break;
    default:
        top = SdrTop, bottom = SdrBottom;
        break;
    }
}

// ---- reading the files ------------------------------------------------------------------------------------

static bool Decode(const fs::path& path, REFGUID format, UINT bytesPerPixel, UINT& width, UINT& height,
                   std::vector<uint8_t>& pixels)
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), format, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&width, &height)))
        return false;
    pixels.resize(size_t(width) * height * bytesPerPixel);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * bytesPerPixel, UINT(pixels.size()), pixels.data()));
}

// shots\ beside the test, or with pictures the default folder: <Pictures>\OptiScaler\<exe name>.
static fs::path folder;
// What was in the default folder before the run: not the test's to check or delete.
static std::set<fs::path> before;

static fs::path PicturesFolder()
{
    PWSTR known = nullptr;
    fs::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &known)))
        path = fs::path(known) / L"OptiScaler" / L"screenshot_smoke";
    CoTaskMemFree(known);
    return path;
}

static bool IsMenuCopy(const fs::path& path) { return path.stem().wstring().ends_with(L" (menu)"); }

// The files of this run: the clean ones, or the copies with the menu.
static std::vector<fs::path> Shots(const wchar_t* extension, bool menu = false)
{
    std::vector<fs::path> found;
    std::error_code error;
    for (auto& entry : fs::directory_iterator(folder, error))
        if (entry.path().extension() == extension && IsMenuCopy(entry.path()) == menu && !before.contains(entry.path()))
            found.push_back(entry.path());
    return found;
}

static bool Near(int a, int b, int tolerance) { return std::abs(a - b) <= tolerance; }

// Every pixel of the PNG is one of the two colours, the top one above the middle: nothing of the menu.
static void CheckPng(const fs::path& path)
{
    UINT width = 0, height = 0;
    std::vector<uint8_t> pixels;
    if (!Decode(path, GUID_WICPixelFormat32bppBGRA, 4, width, height, pixels))
        return Expect(false, "the PNG decodes");
    Expect(width == W && height == H, "the PNG has the back buffer's size");

    Rgb8 top, bottom;
    Expected(top, bottom);
    const int tolerance = kind == Kind::Sdr ? 1 : 3;
    size_t wrong = 0, opaque = 0;
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
        {
            const uint8_t* p = &pixels[(size_t(y) * width + x) * 4];
            const Rgb8& want = y < height / 2 ? top : bottom;
            wrong += !(Near(p[2], want.r, tolerance) && Near(p[1], want.g, tolerance) && Near(p[0], want.b, tolerance));
            opaque += p[3] == 255;
        }
    const uint8_t* first = pixels.data();
    std::printf("      PNG top-left %d %d %d (want %d %d %d), %zu of %zu pixels off\n", first[2], first[1], first[0],
                top.r, top.g, top.b, wrong, pixels.size() / 4);
    Expect(wrong == 0, "the PNG is the game's frame only, without the menu or the FPS overlay");
    Expect(opaque == pixels.size() / 4, "the PNG is opaque");
}

// The copy with the menu: the same frame, with the menu covering part of it.
static void CheckMenuPng(const fs::path& path)
{
    UINT width = 0, height = 0;
    std::vector<uint8_t> pixels;
    if (!Decode(path, GUID_WICPixelFormat32bppBGRA, 4, width, height, pixels))
        return Expect(false, "the menu PNG decodes");
    Expect(width == W && height == H, "the menu PNG has the back buffer's size");

    Rgb8 top, bottom;
    Expected(top, bottom);
    const int tolerance = kind == Kind::Sdr ? 1 : 3;
    size_t other = 0, corner = 0;
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
        {
            const uint8_t* p = &pixels[(size_t(y) * width + x) * 4];
            const Rgb8& want = y < height / 2 ? top : bottom;
            const bool off =
                !(Near(p[2], want.r, tolerance) && Near(p[1], want.g, tolerance) && Near(p[0], want.b, tolerance));
            other += off;
            // The bottom-right corner is away from the menu and the FPS overlay: the game's frame there.
            corner += off && x >= width - 16 && y >= height - 16;
        }
    std::printf("      menu PNG: %zu of %zu pixels are not the game's colours, %zu in the far corner\n", other,
                pixels.size() / 4, corner);
    Expect(other > size_t(W) * H / 100, "the menu PNG has the menu on it");
    Expect(corner == 0, "the menu PNG is the same frame where the menu is not");
}

// The JPEG XR holds scRGB: what the half float swapchain was cleared to, or the HDR10 values in scRGB.
static void CheckJxr(const fs::path& path)
{
    UINT width = 0, height = 0;
    std::vector<uint8_t> pixels;
    if (!Decode(path, GUID_WICPixelFormat128bppRGBAFloat, 16, width, height, pixels))
        return Expect(false, "the JPEG XR decodes");
    Expect(width == W && height == H, "the JPEG XR has the back buffer's size");

    float top[3], bottom[3];
    if (kind == Kind::Fp16)
        std::copy(FpTop, FpTop + 3, top), std::copy(FpBottom, FpBottom + 3, bottom);
    else
    {
        PqNits(PqTop, top), PqNits(PqBottom, bottom);
        for (int i = 0; i < 3; ++i)
            top[i] /= 80.0f, bottom[i] /= 80.0f;
    }

    size_t wrong = 0;
    auto f = reinterpret_cast<const float*>(pixels.data());
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
        {
            const float* p = f + (size_t(y) * width + x) * 4;
            const float* want = y < height / 2 ? top : bottom;
            for (int c = 0; c < 3; ++c)
                wrong += std::abs(p[c] - want[c]) > 0.01f * std::max(1.0f, std::abs(want[c]));
        }
    std::printf("      JPEG XR top-left %.4f %.4f %.4f (want %.4f %.4f %.4f), bottom-right %.3f %.3f %.3f (want %.3f "
                "%.3f %.3f)\n",
                f[0], f[1], f[2], top[0], top[1], top[2], f[(size_t(W) * H - 1) * 4], f[(size_t(W) * H - 1) * 4 + 1],
                f[(size_t(W) * H - 1) * 4 + 2], bottom[0], bottom[1], bottom[2]);
    Expect(wrong == 0, "the JPEG XR holds the HDR values");
}

// The screen over the window: the menu has to cover part of it, so the PNG left something out.
static void CheckScreenHasMenu()
{
    if (kind != Kind::Sdr)
        return; // DWM shows HDR swapchains tone mapped to the desktop; nothing to compare with.

    RECT rect {};
    GetClientRect(window, &rect);
    POINT origin {};
    ClientToScreen(window, &origin);
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info {};
    info.bmiHeader = { sizeof(BITMAPINFOHEADER), LONG(W), -LONG(H), 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto old = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, W, H, screen, origin.x, origin.y, SRCCOPY);
    GdiFlush();

    size_t other = 0;
    auto p = static_cast<const uint8_t*>(bits);
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x, p += 4)
        {
            const Rgb8& want = y < H / 2 ? SdrTop : SdrBottom;
            other += !(Near(p[2], want.r, 8) && Near(p[1], want.g, 8) && Near(p[0], want.b, 8));
        }
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    std::printf("      screen: %zu of %u pixels are not the game's colours\n", other, W * H);
    Expect(other > W * H / 100, "the menu is on screen while the screenshot is taken");
}

// ---- input ------------------------------------------------------------------------------------------------

// OptiScaler drops SendInput from the game's process while its menu is open, so a second copy of this program
// types: "input <vk> [ctrl]".
static int Inject(int argc, wchar_t** argv)
{
    const auto vk = static_cast<WORD>(_wtoi(argv[2]));
    const bool ctrl = argc > 3 && !wcscmp(argv[3], L"ctrl");
    auto send = [](WORD key, bool up)
    {
        const bool extended = key == VK_INSERT || key == VK_END || key == VK_HOME || key == VK_PRIOR ||
                              key == VK_NEXT || key == VK_DELETE || key == VK_SNAPSHOT;
        INPUT input {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = key;
        input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
        input.ki.dwFlags = (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
        return SendInput(1, &input, sizeof(input));
    };
    UINT sent = 0;
    if (ctrl)
        sent += send(VK_LCONTROL, false), Sleep(80);
    sent += send(vk, false);
    Sleep(100);
    sent += send(vk, true);
    if (ctrl)
        Sleep(80), sent += send(VK_LCONTROL, true);
    return sent == (ctrl ? 4u : 2u) ? 0 : 1;
}

static bool Foreground()
{
    for (int i = 0; i < 20 && GetForegroundWindow() != window; ++i)
    {
        INPUT alt[2] {};
        alt[0].type = alt[1].type = INPUT_KEYBOARD;
        alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
        alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &alt[0], sizeof(INPUT));
        const DWORD current = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        AttachThreadInput(GetCurrentThreadId(), current, TRUE);
        BringWindowToTop(window);
        SetForegroundWindow(window);
        AttachThreadInput(GetCurrentThreadId(), current, FALSE);
        SendInput(1, &alt[1], sizeof(INPUT));
        if (GetForegroundWindow() != window)
            SwitchToThisWindow(window, TRUE);
        Sleep(150);
    }
    return GetForegroundWindow() == window;
}

static std::function<void()> renderFrame;

static void Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void Frames(DWORD ms)
{
    const auto end = GetTickCount64() + ms;
    while (GetTickCount64() < end)
    {
        Pump();
        renderFrame();
        Sleep(8);
    }
}

// Types the key while frames keep coming, then 1.2 s more: past the menu's shortcut lockout.
static void Key(WORD vk, bool ctrl = false)
{
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(path) + L"\" input " + std::to_wstring(vk) + (ctrl ? L" ctrl" : L"");
    STARTUPINFOW startup { sizeof(startup) };
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        Die("the input process did not start", GetLastError());
    while (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
    {
        Pump();
        renderFrame();
        Sleep(8);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Frames(1200);
}

// ---- the APIs ---------------------------------------------------------------------------------------------

static HMODULE Proxy(const wchar_t* name)
{
    HMODULE module = LoadLibraryW((Here() / name).c_str());
    if (!module)
        Die("the OptiScaler proxy did not load", GetLastError());
    return module;
}

static void SetupDx11()
{
    HMODULE dxgi = Proxy(L"dxgi.dll");
    auto createFactory = (HRESULT(WINAPI*)(UINT, REFIID, void**)) GetProcAddress(dxgi, "CreateDXGIFactory2");
    static ComPtr<IDXGIFactory2> factory;
    static ComPtr<ID3D11Device> device;
    static ComPtr<ID3D11DeviceContext1> context;
    static ComPtr<IDXGISwapChain1> swap;
    static ComPtr<ID3D11RenderTargetView> view;
    if (!createFactory || FAILED(createFactory(0, IID_PPV_ARGS(&factory))))
        Die("CreateDXGIFactory2");
    auto create = (PFN_D3D11_CREATE_DEVICE) GetProcAddress(LoadLibraryW(L"d3d11.dll"), "D3D11CreateDevice");
    ComPtr<ID3D11DeviceContext> plain;
    if (FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                      &plain)) ||
        FAILED(plain.As(&context)))
        Die("D3D11CreateDevice");
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = W, desc.Height = H, desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM, desc.SampleDesc = { 1, 0 };
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT, desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &swap)))
        Die("CreateSwapChainForHwnd");
    ComPtr<ID3D11Texture2D> buffer;
    swap->GetBuffer(0, IID_PPV_ARGS(&buffer));
    device->CreateRenderTargetView(buffer.Get(), nullptr, &view);
    renderFrame = []
    {
        const float top[4] { SdrTop.r / 255.0f, SdrTop.g / 255.0f, SdrTop.b / 255.0f, 1 };
        const float bottom[4] { SdrBottom.r / 255.0f, SdrBottom.g / 255.0f, SdrBottom.b / 255.0f, 1 };
        const D3D11_RECT half { 0, LONG(H / 2), LONG(W), LONG(H) };
        context->ClearRenderTargetView(view.Get(), top);
        context->ClearView(view.Get(), bottom, &half, 1);
        swap->Present(1, 0);
    };
}

static void SetupDx12()
{
    HMODULE dxgi = Proxy(L"dxgi.dll");
    auto createFactory = (HRESULT(WINAPI*)(UINT, REFIID, void**)) GetProcAddress(dxgi, "CreateDXGIFactory2");
    auto createDevice = (PFN_D3D12_CREATE_DEVICE) GetProcAddress(LoadLibraryW(L"d3d12.dll"), "D3D12CreateDevice");
    static ComPtr<IDXGIFactory4> factory;
    static ComPtr<ID3D12Device> device;
    static ComPtr<ID3D12CommandQueue> queue;
    static ComPtr<IDXGISwapChain3> swap;
    static ComPtr<ID3D12DescriptorHeap> heap;
    static ComPtr<ID3D12CommandAllocator> allocator;
    static ComPtr<ID3D12GraphicsCommandList> list;
    static ComPtr<ID3D12Fence> fence;
    static ComPtr<ID3D12Resource> buffers[2];
    static UINT64 fenceValue = 0;
    static UINT rtvSize = 0;
    if (!createFactory || FAILED(createFactory(0, IID_PPV_ARGS(&factory))) ||
        FAILED(createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        Die("D3D12 device");
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));

    const DXGI_FORMAT format = kind == Kind::Fp16    ? DXGI_FORMAT_R16G16B16A16_FLOAT
                               : kind == Kind::Hdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                     : DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = W, desc.Height = H, desc.Format = format, desc.SampleDesc = { 1, 0 };
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT, desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap1;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &swap1)) ||
        FAILED(swap1.As(&swap)))
        Die("CreateSwapChainForHwnd");
    if (kind != Kind::Sdr)
    {
        const auto space = kind == Kind::Fp16 ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                              : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        const HRESULT hr = swap->SetColorSpace1(space);
        std::printf("      SetColorSpace1: %#lx\n", hr);
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
    device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap));
    rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < 2; ++i)
    {
        swap->GetBuffer(i, IID_PPV_ARGS(&buffers[i]));
        auto handle = heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += i * rtvSize;
        device->CreateRenderTargetView(buffers[i].Get(), nullptr, handle);
    }
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    list->Close();
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

    renderFrame = []
    {
        const UINT index = swap->GetCurrentBackBufferIndex();
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Transition.pResource = buffers[index].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        auto handle = heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += index * rtvSize;
        float top[4] { SdrTop.r / 255.0f, SdrTop.g / 255.0f, SdrTop.b / 255.0f, 1 };
        float bottom[4] { SdrBottom.r / 255.0f, SdrBottom.g / 255.0f, SdrBottom.b / 255.0f, 1 };
        const float* topSource = kind == Kind::Fp16 ? FpTop : kind == Kind::Hdr10 ? PqTop : nullptr;
        const float* bottomSource = kind == Kind::Fp16 ? FpBottom : kind == Kind::Hdr10 ? PqBottom : nullptr;
        if (topSource)
            std::copy(topSource, topSource + 3, top), std::copy(bottomSource, bottomSource + 3, bottom);
        const D3D12_RECT half { 0, LONG(H / 2), LONG(W), LONG(H) };
        list->ClearRenderTargetView(handle, top, 0, nullptr);
        list->ClearRenderTargetView(handle, bottom, 1, &half);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList* lists[] { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        swap->Present(1, 0);
        queue->Signal(fence.Get(), ++fenceValue);
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(fenceValue, done);
        WaitForSingleObject(done, 5000);
        CloseHandle(done);
    };
}

static void SetupDx9()
{
    HMODULE d3d9 = Proxy(L"d3d9.dll");
    auto create = (IDirect3D9 * (WINAPI*) (UINT)) GetProcAddress(d3d9, "Direct3DCreate9");
    static ComPtr<IDirect3D9> d3d;
    static ComPtr<IDirect3DDevice9> device;
    if (!create || !(d3d = create(D3D_SDK_VERSION)))
        Die("Direct3DCreate9");
    D3DPRESENT_PARAMETERS params {};
    params.Windowed = TRUE, params.SwapEffect = D3DSWAPEFFECT_DISCARD, params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferWidth = W, params.BackBufferHeight = H, params.hDeviceWindow = window;
    params.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                 &params, &device)))
        Die("CreateDevice");
    renderFrame = []
    {
        const D3DRECT half { 0, LONG(H / 2), LONG(W), LONG(H) };
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(SdrTop.r, SdrTop.g, SdrTop.b), 1, 0);
        device->Clear(1, &half, D3DCLEAR_TARGET, D3DCOLOR_XRGB(SdrBottom.r, SdrBottom.g, SdrBottom.b), 1, 0);
        device->Present(nullptr, nullptr, nullptr, nullptr);
    };
}

static void SetupGl()
{
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                PFD_TYPE_RGBA, 32 };
    pfd.cAlphaBits = 8;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC context = wglCreateContext(dc);
    if (!context || !wglMakeCurrent(dc, context))
        Die("OpenGL context");
    wchar_t loaded[MAX_PATH] {};
    GetModuleFileNameW(GetModuleHandleW(L"opengl32.dll"), loaded, MAX_PATH);
    if (fs::path(loaded).parent_path() != Here())
        Die("opengl32.dll is not the OptiScaler proxy beside the test");
    renderFrame = [dc]
    {
        glDisable(0x0C11); // GL_SCISSOR_TEST
        glClearColor(SdrTop.r / 255.0f, SdrTop.g / 255.0f, SdrTop.b / 255.0f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(0x0C11);
        glScissor(0, 0, W, H / 2); // OpenGL rows run bottom up: this is the bottom half
        glClearColor(SdrBottom.r / 255.0f, SdrBottom.g / 255.0f, SdrBottom.b / 255.0f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(0x0C11);
        SwapBuffers(dc);
    };
}

static void SetupVulkan()
{
    if (!LoadLibraryW(L"vulkan-1.dll"))
        Die("vulkan-1.dll");
    Proxy(L"OptiScaler.dll");

    static VkInstance instance;
    static VkPhysicalDevice physical;
    static VkDevice device;
    static VkQueue queue;
    static VkSurfaceKHR surface;
    static VkSwapchainKHR swapchain;
    static std::vector<VkImage> images;
    static VkCommandPool pool;
    static VkCommandBuffer cmd;
    static VkFence fence;
    static VkSemaphore acquired, rendered;
    static VkBuffer staging;
    static VkDeviceMemory stagingMemory;
    static uint32_t family = 0;

    const char* instanceExtensions[] { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS)
        Die("vkCreateInstance");
    uint32_t count = 1;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    VkWin32SurfaceCreateInfoKHR surfaceInfo { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    surfaceInfo.hinstance = GetModuleHandleW(nullptr);
    surfaceInfo.hwnd = window;
    vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface);

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    for (family = 0; family < count; ++family)
    {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &present);
        if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
            break;
    }
    const float priority = 1;
    VkDeviceQueueCreateInfo queueInfo { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* deviceExtensions[] { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    if (vkCreateDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS)
        Die("vkCreateDevice");
    vkGetDeviceQueue(device, family, 0, &queue);

    VkSurfaceCapabilitiesKHR caps {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
    VkSwapchainCreateInfoKHR swapInfo { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swapInfo.surface = surface;
    swapInfo.minImageCount = std::max(2u, caps.minImageCount);
    swapInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swapInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapInfo.imageExtent = { W, H };
    swapInfo.imageArrayLayers = 1;
    // No transfer source: OptiScaler has to add it for the screenshot.
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapInfo.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(device, &swapInfo, nullptr, &swapchain) != VK_SUCCESS)
        Die("vkCreateSwapchainKHR");
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());

    VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = family;
    vkCreateCommandPool(device, &poolInfo, nullptr, &pool);
    VkCommandBufferAllocateInfo cmdInfo { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdInfo.commandPool = pool, cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cmdInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cmdInfo, &cmd);
    VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(device, &fenceInfo, nullptr, &fence);
    VkSemaphoreCreateInfo semaphoreInfo { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquired);
    vkCreateSemaphore(device, &semaphoreInfo, nullptr, &rendered);

    // The bottom half comes from a buffer: vkCmdClearColorImage has no rectangle.
    VkBufferCreateInfo bufferInfo { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = VkDeviceSize(W) * (H / 2) * 4;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(device, &bufferInfo, nullptr, &staging);
    VkMemoryRequirements requirements {};
    vkGetBufferMemoryRequirements(device, staging, &requirements);
    VkPhysicalDeviceMemoryProperties memory {};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    VkMemoryAllocateInfo allocateInfo { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocateInfo.allocationSize = requirements.size;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
            allocateInfo.memoryTypeIndex = i;
            break;
        }
    vkAllocateMemory(device, &allocateInfo, nullptr, &stagingMemory);
    vkBindBufferMemory(device, staging, stagingMemory, 0);
    void* data = nullptr;
    vkMapMemory(device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &data);
    auto pixels = static_cast<uint8_t*>(data);
    for (size_t i = 0; i < size_t(W) * (H / 2); ++i)
        pixels[i * 4 + 0] = uint8_t(SdrBottom.b), pixels[i * 4 + 1] = uint8_t(SdrBottom.g),
                   pixels[i * 4 + 2] = uint8_t(SdrBottom.r), pixels[i * 4 + 3] = 255;
    vkUnmapMemory(device, stagingMemory);

    renderFrame = []
    {
        uint32_t index = 0;
        if (vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index) != VK_SUCCESS)
            return;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(cmd, &begin);
        VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[index];
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        VkClearColorValue top { { SdrTop.r / 255.0f, SdrTop.g / 255.0f, SdrTop.b / 255.0f, 1 } };
        vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &top, 1,
                             &barrier.subresourceRange);
        VkMemoryBarrier between { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        between.srcAccessMask = between.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &between, 0,
                             nullptr, 0, nullptr);
        VkBufferImageCopy region {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageOffset = { 0, int32_t(H / 2), 0 };
        region.imageExtent = { W, H / 2, 1 };
        vkCmdCopyBufferToImage(cmd, staging, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);
        vkEndCommandBuffer(cmd);
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount = 1, submit.pWaitSemaphores = &acquired, submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1, submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1, submit.pSignalSemaphores = &rendered;
        vkQueueSubmit(queue, 1, &submit, fence);
        VkPresentInfoKHR present { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.waitSemaphoreCount = 1, present.pWaitSemaphores = &rendered;
        present.swapchainCount = 1, present.pSwapchains = &swapchain, present.pImageIndices = &index;
        vkQueuePresentKHR(queue, &present);
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &fence);
    };
}

// ---- the run ----------------------------------------------------------------------------------------------

static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CLOSE)
        return 0;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool WaitForShots(size_t png, size_t jxr, DWORD ms)
{
    const auto end = GetTickCount64() + ms;
    while (GetTickCount64() < end)
    {
        if (Shots(L".png").size() >= png && Shots(L".jxr").size() >= jxr)
        {
            Frames(300); // the writer closes its files
            return true;
        }
        Pump();
        renderFrame();
        Sleep(8);
    }
    return false;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 3 && !wcscmp(argv[1], L"input"))
        return Inject(argc, argv);
    if (argc < 2)
    {
        std::printf("usage: screenshot_smoke.exe dx11|dx12|dx12-fp16|dx12-hdr10|vk|gl|dx9 [chord]\n");
        return 2;
    }
    const std::wstring api = argv[1];
    const std::wstring mode = argc > 2 ? argv[2] : L"";
    const bool chord = mode == L"chord";
    const bool withMenu = mode != L"nomenu";
    kind = api == L"dx12-fp16" ? Kind::Fp16 : api == L"dx12-hdr10" ? Kind::Hdr10 : Kind::Sdr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::error_code error;
    bool madeFolder = false, madeParent = false;
    if (mode == L"pictures")
    {
        folder = PicturesFolder();
        if (folder.empty())
            Die("no Pictures folder");
        madeParent = !fs::exists(folder.parent_path(), error);
        madeFolder = !fs::exists(folder, error);
        for (auto& entry : fs::directory_iterator(folder, error))
            before.insert(entry.path());
        std::printf("      default folder: %ls\n", folder.c_str());
    }
    else
    {
        folder = Here() / L"shots";
        fs::remove_all(folder, error);
    }

    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = Proc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"OptiScreenshotSmoke";
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&windowClass);
    RECT rect { 0, 0, LONG(W), LONG(H) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName, L"Opti screenshot smoke",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, rect.right - rect.left,
                             rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, nullptr);
    Pump();

    if (api == L"dx11")
        SetupDx11();
    else if (api.rfind(L"dx12", 0) == 0)
        SetupDx12();
    else if (api == L"dx9")
        SetupDx9();
    else if (api == L"gl")
        SetupGl();
    else if (api == L"vk")
        SetupVulkan();
    else
        Die("unknown API");

    if (!Foreground())
        Die("the test window cannot take the foreground (locked or busy desktop)");

    Frames(2500);
    Key(VK_INSERT); // the menu opens; the FPS overlay is on from the INI
    Frames(500);
    CheckScreenHasMenu();

    const size_t jxr = kind == Kind::Sdr ? 0 : 1;
    if (chord)
    {
        Key(VK_F6);
        Expect(!WaitForShots(1, 0, 2500), "F6 alone does not fire Ctrl+F6");
        Key(VK_F6, true);
    }
    else
        Key(VK_F6);

    const bool saved = WaitForShots(1, jxr, 10000);
    Expect(saved, kind == Kind::Sdr ? "a PNG was written" : "a PNG and a JPEG XR were written");
    if (saved)
    {
        if (withMenu)
        {
            const auto end = GetTickCount64() + 10000;
            while (GetTickCount64() < end && (Shots(L".png", true).empty() || Shots(L".jxr", true).size() < jxr))
                Frames(100);
            Frames(300);
        }
        else
            Frames(1500); // time enough for a copy with the menu that must not come

        auto png = Shots(L".png");
        auto menuPng = Shots(L".png", true);
        Expect(png.size() == 1, "one press, one PNG without the menu");
        Expect(menuPng.size() == (withMenu ? 1u : 0u),
               withMenu ? "and one PNG with the menu" : "ScreenshotWithMenu=false: no PNG with the menu");
        std::printf("      %ls\n", png[0].filename().c_str());
        CheckPng(png[0]);
        if (jxr)
            CheckJxr(Shots(L".jxr")[0]);

        // The name: "<exe> yyyy-MM-dd HH-mm-ss", and the menu copy "<the same> (menu)".
        const auto stem = png[0].stem().wstring();
        Expect(stem.rfind(L"screenshot_smoke ", 0) == 0 && stem.size() == std::wstring(L"screenshot_smoke ").size() + 19,
               "the file is named after the exe, date and time");
        if (!menuPng.empty())
        {
            std::printf("      %ls\n", menuPng[0].filename().c_str());
            Expect(menuPng[0].stem().wstring() == stem + L" (menu)", "the menu copy has the same name and time");
            Expect(Shots(L".jxr", true).size() == jxr, "the menu copy has its JPEG XR on HDR swapchains");
            CheckMenuPng(menuPng[0]);
        }
    }

    Key(VK_INSERT);

    // Only what this run wrote goes from the default folder.
    if (mode == L"pictures")
    {
        size_t removed = 0;
        for (const wchar_t* extension : { L".png", L".jxr" })
            for (bool menu : { false, true })
                for (auto& path : Shots(extension, menu))
                    removed += fs::remove(path, error);
        if (madeFolder && fs::is_empty(folder, error))
            fs::remove(folder, error);
        if (madeParent && fs::is_empty(folder.parent_path(), error))
            fs::remove(folder.parent_path(), error);
        std::printf("      removed the %zu files of this run\n", removed);
    }
    std::printf("%d failed\n", failures);
    std::fflush(stdout);
    ExitProcess(failures);
}
