// The RenoDX composition stage (OptiScaler/dlssnr/amd/RenoComposition.h) on the GPU, against what it promises:
// the game's colour unchanged where the runtime changed nothing, the runtime's brightening carried over, highlights
// the runtime clipped kept, the +-2 stop luminance bound, colour strength 0 keeping the game's hue, no edit on the
// black floor, the pedestal taken off near-black blocks, sRGB frames round-tripped, and the divisor snapping to an
// over-range frame and holding. Needs a D3D12 device; no runtime.
#define NOMINMAX
#include "../OptiScaler/dlssnr/amd/RenoComposition.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT W = 256, H = 128;
int failures = 0;
void Expect(bool ok, const char* what, double got, double want)
{
    std::printf("%s  %-58s got %.5f want %.5f\n", ok ? "ok  " : "FAIL", what, got, want);
    failures += !ok;
}
void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        std::printf("FAIL %s 0x%08lx\n", what, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}
float Half(uint16_t h)
{
    const uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 1023;
    uint32_t bits;
    if (e == 0)
    {
        const float v = std::ldexp(float(m), -24);
        return s ? -v : v;
    }
    bits = s | ((e == 31 ? 255 : e + 112) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
float Srgb(float v) { return v <= .0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - .055f; }
struct Rgb
{
    float r, g, b;
};
float Y(Rgb c) { return .212639f * c.r + .715169f * c.g + .072192f * c.b; }

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    Gpu()
    {
        Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        D3D12_COMMAND_QUEUE_DESC qd {};
        Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        Check(
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
            "list");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    }
    void Submit()
    {
        Check(list->Close(), "close");
        ID3D12CommandList* l = list.Get();
        queue->ExecuteCommandLists(1, &l);
        Check(queue->Signal(fence.Get(), ++value), "signal");
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(value, e);
        WaitForSingleObject(e, INFINITE);
        CloseHandle(e);
        Check(allocator->Reset(), "allocator reset");
        Check(list->Reset(allocator.Get(), nullptr), "list reset");
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = type;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                              type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                             : D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&r)),
              "buffer");
        return r;
    }
    // An RGBA32F texture filled from pixels, left in NON_PIXEL_SHADER_RESOURCE.
    ComPtr<ID3D12Resource> Texture(const std::vector<Rgb>& pixels)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = W;
        rd.Height = H;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        rd.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> t;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&t)),
              "texture");
        const UINT pitch = W * 16;
        auto up = Buffer(UINT64(pitch) * H, D3D12_HEAP_TYPE_UPLOAD);
        float* p = nullptr;
        up->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (UINT i = 0; i < W * H; ++i)
        {
            p[i * 4] = pixels[i].r;
            p[i * 4 + 1] = pixels[i].g;
            p[i * 4 + 2] = pixels[i].b;
            p[i * 4 + 3] = 1;
        }
        up->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst { t.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
        D3D12_TEXTURE_COPY_LOCATION src { up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, W, H, 1, pitch };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b {};
        b.Transition = { t.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        list->ResourceBarrier(1, &b);
        Submit();
        return t;
    }
    // The FP16 output read back as floats.
    std::vector<Rgb> Read(ID3D12Resource* t)
    {
        const UINT pitch = (W * 8 + 255) & ~255u;
        auto back = Buffer(UINT64(pitch) * H, D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_BARRIER b {};
        b.Transition = { t, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_COPY_SOURCE };
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst { back.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 1, pitch };
        D3D12_TEXTURE_COPY_LOCATION src { t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        Submit();
        std::vector<Rgb> out(W * H);
        uint8_t* p = nullptr;
        back->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (UINT y = 0; y < H; ++y)
            for (UINT x = 0; x < W; ++x)
            {
                auto* h = reinterpret_cast<uint16_t*>(p + y * pitch) + x * 4;
                out[y * W + x] = { Half(h[0]), Half(h[1]), Half(h[2]) };
            }
        back->Unmap(0, nullptr);
        return out;
    }
};

struct Scene
{
    std::vector<Rgb> colour = std::vector<Rgb>(W * H), result = std::vector<Rgb>(W * H);
    // Fills the 32-pixel column band starting at x0.
    void Band(UINT x0, const std::function<void(UINT, UINT, Rgb&, Rgb&)>& fill)
    {
        for (UINT y = 0; y < H; ++y)
            for (UINT x = x0; x < x0 + 32; ++x)
                fill(x, y, colour[y * W + x], result[y * W + x]);
    }
};
} // namespace

int main()
{
    Gpu gpu;
    AmdPreSr::RenoComposition composition(gpu.device.Get());
    auto run =
        [&](const Scene& s, UINT encoding, bool snap, float colour, bool pedestal, float intensity = 1, float guard = 4)
    {
        auto c = gpu.Texture(s.colour), r = gpu.Texture(s.result);
        AmdPreSr::Frame f {};
        f.colour = c.Get();
        f.width = W;
        f.height = H;
        auto out = composition.Record(gpu.list.Get(), f, r.Get(), encoding, snap, .1f, intensity, guard, colour, 1.f,
                                      pedestal);
        gpu.Submit();
        return gpu.Read(out);
    };
    auto at = [](const std::vector<Rgb>& v, UINT x, UINT y) { return v[y * W + x]; };

    Scene s;
    // 0: nothing changed, varied colours from the floor to white.
    s.Band(0, [](UINT x, UINT y, Rgb& o, Rgb& r)
           { o = r = { .02f + .9f * y / H, .01f + .5f * (x % 32) / 32, .3f * ((x + y) % 7) / 7 }; });
    // 32: mid grey brightened one stop.
    s.Band(32, [](UINT, UINT, Rgb& o, Rgb& r) { o = { .18f, .18f, .18f }, r = { .36f, .36f, .36f }; });
    // 64: an HDR highlight the runtime clipped to white.
    s.Band(64, [](UINT, UINT, Rgb& o, Rgb& r) { o = { 4, 3, 2 }, r = { 1, 1, 1 }; });
    // 96: a runtime brightening of four stops.
    s.Band(96, [](UINT, UINT, Rgb& o, Rgb& r) { o = { .02f, .02f, .02f }, r = { .32f, .32f, .32f }; });
    // 128: a colour edit, red pushed.
    s.Band(128, [](UINT, UINT, Rgb& o, Rgb& r) { o = { .2f, .1f, .05f }, r = { .4f, .1f, .05f }; });
    // 160: the black floor lifted.
    s.Band(160, [](UINT, UINT, Rgb& o, Rgb& r) { o = { .0003f, .0003f, .0003f }, r = { .01f, .01f, .01f }; });
    // 192: a near-black block lifted a little.
    s.Band(192, [](UINT, UINT, Rgb& o, Rgb& r) { o = { .004f, .004f, .004f }, r = { .008f, .008f, .008f }; });
    // 224: darkened one stop.
    s.Band(224, [](UINT, UINT, Rgb& o, Rgb& r) { o = { .5f, .4f, .3f }, r = { .25f, .2f, .15f }; });

    auto out = run(s, 1, true, 1, false);
    double worst = 0;
    bool finite = true;
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < 32; ++x)
        {
            const auto o = at(s.colour, x, y), q = at(out, x, y);
            worst = std::max<double>({ worst, std::abs(q.r - o.r) / o.r, std::abs(q.g - o.g) / o.g,
                                       o.b > 0 ? std::abs(q.b - o.b) / std::max(o.b, 1e-3f) : 0.0 });
        }
    for (auto& v : out)
        finite = finite && std::isfinite(v.r) && std::isfinite(v.g) && std::isfinite(v.b);
    Expect(finite, "every output value is finite", finite, 1);
    Expect(worst < 2e-3, "no runtime change: the game's colour, relative error", worst, 0);
    Expect(std::abs(at(out, 40, 60).g - .36f) < .36f * .01f, "mid grey +1 stop is carried over", at(out, 40, 60).g,
           .36);
    const double kept = Y(at(out, 70, 60));
    Expect(kept > 3 * Y({ 1, 1, 1 }), "clipped HDR highlight keeps its range (Y)", kept, Y({ 4, 3, 2 }));
    Expect(std::abs(at(out, 100, 60).g - .08f) < .08f * .01f, "+4 stops is bounded by a 4x guard to +2",
           at(out, 100, 60).g, .08);
    Expect(std::abs(at(out, 170, 60).g - .0003f) < .00005f, "black floor: no edit", at(out, 170, 60).g, .0003);
    Expect(std::abs(at(out, 230, 60).r - .25f) < .25f * .01f, "darkened one stop is carried over", at(out, 230, 60).r,
           .25);
    const auto red = at(out, 140, 60);
    Expect(red.r / red.g > 2.0 * 1.3, "colour strength 1: red edit carried (r/g)", red.r / red.g, 4);

    auto luma = run(s, 1, true, 0, false);
    const auto hue = at(luma, 140, 60);
    Expect(std::abs(hue.r / hue.g - 2.0) < .02, "colour strength 0: the game's hue (r/g)", hue.r / hue.g, 2);
    Expect(std::abs(Y(hue) - Y(red)) < Y(red) * .01, "colour strength 0: same luminance as 1", Y(hue), Y(red));

    auto guarded = run(s, 1, true, 1, false, 1, 2);
    Expect(std::abs(at(guarded, 100, 60).g - .04f) < .04f * .01f, "+4 stops is bounded by a 2x guard to +1",
           at(guarded, 100, 60).g, .04);
    Expect(std::abs(at(guarded, 230, 60).r - .25f) < .25f * .01f, "-1 stop fits a 2x guard", at(guarded, 230, 60).r,
           .25);
    auto none = run(s, 1, true, 1, false, 0);
    Expect(std::abs(at(none, 40, 60).g - .18f) < .18f * .01f, "intensity 0: the game's colour", at(none, 40, 60).g,
           .18);
    auto half = run(s, 1, true, 1, false, .5f);
    Expect(std::abs(at(half, 40, 60).g - .2546f) < .2546f * .01f, "intensity 0.5: half the edit in stops",
           at(half, 40, 60).g, .2546);
    auto twice = run(s, 1, true, 1, false, 2, 8);
    Expect(std::abs(at(twice, 40, 60).g - .72f) < .72f * .01f, "intensity 2: +1 stop becomes +2", at(twice, 40, 60).g,
           .72);
    const auto redTwice = at(twice, 140, 60);
    Expect(std::abs(redTwice.r / redTwice.g - 4.0) < .04, "intensity 2 leaves the colour edit as it was",
           redTwice.r / redTwice.g, 4);

    // A wide near-black area lifted a little, beside a lit one: the blocks interpolate, so the gate needs dark
    // neighbours, as it would in a game.
    Scene d;
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x)
        {
            const bool dark = x < 128;
            d.colour[y * W + x] = dark ? Rgb { .004f, .004f, .004f } : Rgb { .18f, .18f, .18f };
            d.result[y * W + x] = dark ? Rgb { .008f, .008f, .008f } : Rgb { .36f, .36f, .36f };
        }
    auto lifted = run(d, 1, true, 1, false), ped = run(d, 1, true, 1, true);
    Expect(at(lifted, 48, 60).g > .0041f, "near-black lift without pedestal removal", at(lifted, 48, 60).g, .004);
    Expect(std::abs(at(ped, 48, 60).g - .004f) < .0001f, "pedestal removal takes the near-black lift off",
           at(ped, 48, 60).g, .004);
    Expect(std::abs(at(ped, 200, 60).g - at(lifted, 200, 60).g) < 1e-4, "pedestal leaves lit blocks alone",
           at(ped, 200, 60).g, at(lifted, 200, 60).g);

    // sRGB-encoded frame: composed in linear light and encoded back.
    Scene e;
    for (UINT i = 0; i < W * H; ++i)
        e.colour[i] = e.result[i] = { Srgb(.02f + .9f * (i % W) / W), Srgb(.2f), Srgb(.05f + .5f * (i / W) / H) };
    auto srgb = run(e, 2, true, 1, true);
    double srgbWorst = 0;
    for (UINT i = 0; i < W * H; ++i)
        srgbWorst =
            std::max<double>(srgbWorst, std::abs(srgb[i].r - e.colour[i].r) + std::abs(srgb[i].b - e.colour[i].b));
    Expect(srgbWorst < 3e-3, "sRGB frame, no runtime change: round trip", srgbWorst, 0);

    // A frame entirely past the shoulder: the divisor snaps to median / 0.30 = 10, so a runtime doubling survives
    // the shoulder, and the next frame holds it.
    Scene hdr;
    for (UINT i = 0; i < W * H; ++i)
        hdr.colour[i] = { 3, 3, 3 }, hdr.result[i] = { 6, 6, 6 };
    auto snapped = run(hdr, 1, true, 1, false);
    Expect(std::abs(at(snapped, 100, 60).g - 6) < .06, "over-range frame: divisor snaps, x2 carried",
           at(snapped, 100, 60).g, 6);
    auto held = run(hdr, 1, false, 1, false);
    Expect(std::abs(at(held, 100, 60).g - 6) < .06, "next frame holds the divisor", at(held, 100, 60).g, 6);

    std::printf(failures ? "FAIL: %d check(s)\n" : "PASS: RenoDX composition\n", failures);
    return failures ? 1 : 0;
}
