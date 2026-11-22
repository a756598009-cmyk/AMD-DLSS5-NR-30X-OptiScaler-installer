// What the screen shows inside a window, read back through Desktop Duplication, for the D3D11 and the 32-bit D3D9
// smoke programs. Every desktop update that touches the area is read: rows that are mostly black (the test scenes
// have none, so each one is an artefact), and how far each frame is from a reference image of the same still scene.
// Without a reference it writes the mean of the frames it saw as <name>.ref; a few frames are kept as <name>-*.bmp.
// It must run in a process where dxgi.dll is the system one, never OptiScaler.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

struct ScreenLook
{
    unsigned updates = 0, frames = 0, black = 0, rows = 0, run = 0, top = 0, bottom = 0;
    double p10 = 0, p50 = 0, p90 = 0, mean = 0;
    std::string error;
};

namespace screen_look
{
template <class T> struct Ref
{
    T* p = nullptr;
    ~Ref()
    {
        if (p)
            p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};
inline void SaveBmp(const std::wstring& path, const BYTE* bits, UINT pitch, UINT w, UINT h)
{
    BITMAPFILEHEADER file {};
    file.bfType = 0x4D42;
    BITMAPINFOHEADER info {};
    info.biSize = sizeof(info);
    info.biWidth = LONG(w);
    info.biHeight = -LONG(h);
    info.biPlanes = 1;
    info.biBitCount = 32;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + w * h * 4;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (UINT y = 0; y < h; ++y)
        out.write(reinterpret_cast<const char*>(bits) + size_t(y) * pitch, std::streamsize(w) * 4);
}
} // namespace screen_look

// area: the window's client rectangle in desktop coordinates. folder: where <name>.ref and the BMPs go.
inline ScreenLook LookAtScreen(double seconds, RECT area, const std::wstring& folder, const wchar_t* name,
                               const wchar_t* reference)
{
    using namespace screen_look;
    ScreenLook look;
    const auto fail = [&](const char* what)
    {
        look.error = what;
        return look;
    };
    using FactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create = dxgi ? reinterpret_cast<FactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    Ref<IDXGIFactory1> factory;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return fail("CreateDXGIFactory1");
    IDXGIAdapter1* found = nullptr;
    IDXGIOutput1* output = nullptr;
    DXGI_OUTPUT_DESC where {};
    const POINT centre { (area.left + area.right) / 2, (area.top + area.bottom) / 2 };
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
    {
        IDXGIOutput* o = nullptr;
        for (UINT i = 0; !output && adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            if (SUCCEEDED(o->GetDesc(&where)) && PtInRect(&where.DesktopCoordinates, centre))
                o->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output));
            o->Release();
        }
        if (output)
            found = adapter;
        else
            adapter->Release();
    }
    Ref<IDXGIAdapter1> held;
    held.p = found;
    Ref<IDXGIOutput1> out;
    out.p = output;
    if (!output)
        return fail("no output holds the window");
    using CreateFn = decltype(&D3D11CreateDevice);
    const HMODULE d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto createDevice = d3d11 ? reinterpret_cast<CreateFn>(GetProcAddress(d3d11, "D3D11CreateDevice")) : nullptr;
    Ref<ID3D11Device> device;
    Ref<ID3D11DeviceContext> context;
    if (!createDevice || FAILED(createDevice(found, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                             &device, nullptr, &context)))
        return fail("D3D11CreateDevice");
    Ref<IDXGIOutputDuplication> duplication;
    if (FAILED(output->DuplicateOutput(device.p, &duplication)))
        return fail("DuplicateOutput");
    OffsetRect(&area, -where.DesktopCoordinates.left, -where.DesktopCoordinates.top);
    const UINT w = UINT(area.right - area.left), h = UINT(area.bottom - area.top);
    // The difference is taken on a grid of every fourth pixel below the top fifth, where an FPS overlay sits.
    const UINT gw = w / 4, gh = h / 4, skip = gh / 5;
    std::vector<float> ref, sum(size_t(gw) * gh * 3);
    if (reference)
    {
        ref.resize(sum.size());
        std::ifstream in(folder + L"\\" + reference, std::ios::binary);
        in.read(reinterpret_cast<char*>(ref.data()), std::streamsize(ref.size() * sizeof(float)));
        if (!in)
            ref.clear();
    }
    Ref<ID3D11Texture2D> staging;
    std::vector<double> diffs;
    // One line per frame read back (<name>.csv): when it was presented, its rows of black and its difference.
    std::string lines = "present_ms,black_rows,difference\n";
    LONGLONG firstPresent = 0;
    LARGE_INTEGER rate {};
    QueryPerformanceFrequency(&rate);
    std::vector<BYTE> rects;
    unsigned saved = 0;
    const ULONGLONG end = GetTickCount64() + ULONGLONG(seconds * 1000);
    while (GetTickCount64() < end)
    {
        DXGI_OUTDUPL_FRAME_INFO info {};
        Ref<IDXGIResource> image;
        const HRESULT hr = duplication->AcquireNextFrame(50, &info, &image);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            continue;
        if (FAILED(hr))
            return fail("AcquireNextFrame");
        bool window = false;
        if (info.LastPresentTime.QuadPart && info.TotalMetadataBufferSize)
        {
            rects.resize(info.TotalMetadataBufferSize);
            UINT size = 0;
            if (SUCCEEDED(
                    duplication->GetFrameDirtyRects(UINT(rects.size()), reinterpret_cast<RECT*>(rects.data()), &size)))
                for (UINT i = 0; i < size / sizeof(RECT) && !window; ++i)
                {
                    RECT overlap;
                    window = IntersectRect(&overlap, reinterpret_cast<RECT*>(rects.data()) + i, &area) != FALSE;
                }
        }
        look.updates += window;
        Ref<ID3D11Texture2D> desktop;
        if (window && name &&
            SUCCEEDED(image->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&desktop))))
        {
            if (!staging.p)
            {
                D3D11_TEXTURE2D_DESC desc {};
                desktop->GetDesc(&desc);
                desc.Width = w;
                desc.Height = h;
                desc.MipLevels = desc.ArraySize = 1;
                desc.Usage = D3D11_USAGE_STAGING;
                desc.BindFlags = desc.MiscFlags = 0;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
                    return fail("staging texture");
            }
            const D3D11_BOX box { UINT(area.left), UINT(area.top), 0, UINT(area.right), UINT(area.bottom), 1 };
            context->CopySubresourceRegion(staging.p, 0, 0, 0, 0, desktop.p, 0, &box);
            D3D11_MAPPED_SUBRESOURCE map {};
            if (SUCCEEDED(context->Map(staging.p, 0, D3D11_MAP_READ, 0, &map)))
            {
                const BYTE* bits = static_cast<const BYTE*>(map.pData);
                unsigned rows = 0, run = 0, longest = 0;
                for (UINT y = 0; y < h; ++y)
                {
                    const BYTE* p = bits + size_t(y) * map.RowPitch;
                    UINT dark = 0;
                    for (UINT x = 0; x < w; ++x, p += 4)
                        dark += std::max({ p[0], p[1], p[2] }) <= 16;
                    const bool black = dark * 2 >= w;
                    rows += black;
                    run = black ? run + 1 : 0;
                    longest = std::max(longest, run);
                    if (black)
                        ++(y < h / 2 ? look.top : look.bottom);
                }
                ++look.frames;
                look.black += rows > 0;
                look.rows += rows;
                look.run = std::max(look.run, longest);
                double diff = 0;
                for (UINT gy = skip; gy < gh; ++gy)
                    for (UINT gx = 0; gx < gw; ++gx)
                    {
                        const BYTE* p = bits + size_t(gy) * 4 * map.RowPitch + size_t(gx) * 16;
                        float* s = &sum[(size_t(gy) * gw + gx) * 3];
                        for (int c = 0; c < 3; ++c)
                        {
                            s[c] += p[c];
                            if (!ref.empty())
                                diff += std::abs(p[c] - ref[(size_t(gy) * gw + gx) * 3 + c]);
                        }
                    }
                if (!ref.empty())
                    diffs.push_back(diff / (double(gh - skip) * gw * 3));
                if (!firstPresent)
                    firstPresent = info.LastPresentTime.QuadPart;
                char line[96];
                sprintf_s(line, "%.3f,%u,%.3f\n",
                          double(info.LastPresentTime.QuadPart - firstPresent) * 1000.0 / double(rate.QuadPart), rows,
                          ref.empty() ? 0.0 : diffs.back());
                lines += line;
                if ((rows && saved < 3) || look.frames == 30)
                {
                    wchar_t file[96];
                    swprintf_s(file, L"\\%s-%s%u.bmp", name, rows ? L"black" : L"frame", rows ? saved : 30u);
                    SaveBmp(folder + file, bits, map.RowPitch, w, h);
                    saved += rows > 0;
                }
                context->Unmap(staging.p, 0);
            }
        }
        duplication->ReleaseFrame();
    }
    if (name)
        std::ofstream(folder + L"\\" + name + L".csv") << lines;
    if (name && !reference && look.frames)
    {
        for (auto& v : sum)
            v /= float(look.frames);
        std::ofstream file(folder + L"\\" + name + L".ref", std::ios::binary);
        file.write(reinterpret_cast<const char*>(sum.data()), std::streamsize(sum.size() * sizeof(float)));
    }
    if (!diffs.empty())
    {
        for (double d : diffs)
            look.mean += d / double(diffs.size());
        std::sort(diffs.begin(), diffs.end());
        look.p10 = diffs[diffs.size() / 10];
        look.p50 = diffs[diffs.size() / 2];
        look.p90 = diffs[diffs.size() * 9 / 10];
    }
    return look;
}
