// Person protection's composition on the GPU (OptiScaler/dlssnr/person/PersonPartition.h): with no mask the runtime's
// result passes, inside a current mask the person takes the first pass (or the input at strength 0), the game's
// alpha stays, a mask from another stream or across a depth jump is refused, the overlay tints, and the texture
// past the render subrect keeps the result. Needs a D3D12 device and no worker or model; with a folder holding
// person-model\ (person-worker.exe and the files it loads) and a still image, also the round trip through the worker.
//   person_partition.exe [folder of person-model\ image.ppm]
#define NOMINMAX
#include "../OptiScaler/dlssnr/person/PersonPartition.h"
#include <dxgi1_6.h>
#include <cstdio>
#include <fstream>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace Person = DlssNr::Person;
namespace
{
constexpr UINT W = 64, H = 48, TW = 72; // the textures are wider than the render subrect
int failures = 0;
void Expect(bool ok, const char* what, double got, double want)
{
    std::printf("%s  %-62s got %.4f want %.4f\n", ok ? "ok  " : "FAIL", what, got, want);
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
    if (e == 0)
    {
        const float v = std::ldexp(float(m), -24);
        return s ? -v : v;
    }
    const uint32_t bits = s | ((e == 31 ? 255 : e + 112) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

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
        // The debug layer, when the SDK's is installed: its errors fail the test (DebugErrors).
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
        Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        D3D12_COMMAND_QUEUE_DESC qd {};
        Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        Check(
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
            "list");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    }
    void Submit(Person::Partition* partition = nullptr)
    {
        Check(list->Close(), "close");
        ID3D12CommandList* l = list.Get();
        queue->ExecuteCommandLists(1, &l);
        if (partition)
            partition->Submitted(queue.Get(), 1, &l);
        Check(queue->Signal(fence.Get(), ++value), "signal");
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(value, e);
        WaitForSingleObject(e, INFINITE);
        CloseHandle(e);
        Check(allocator->Reset(), "allocator reset");
        Check(list->Reset(allocator.Get(), nullptr), "list reset");
    }
    UINT64 DebugErrors()
    {
        ComPtr<ID3D12InfoQueue> info;
        if (FAILED(device.As(&info)))
            return 0;
        UINT64 errors = 0;
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T bytes = 0;
            info->GetMessage(i, nullptr, &bytes);
            std::vector<char> storage(bytes);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(info->GetMessage(i, m, &bytes)) && m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            {
                std::printf("      debug layer: %s\n", m->pDescription);
                ++errors;
            }
        }
        return errors;
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
    // A texture of one value per channel (or one per pixel and channel with perPixel), left in
    // NON_PIXEL_SHADER_RESOURCE.
    ComPtr<ID3D12Resource> Texture(DXGI_FORMAT format, UINT channels, const float* fill, UINT w = TW, UINT h = H,
                                   bool perPixel = false)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = format;
        rd.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> t;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&t)),
              "texture");
        Fill(t.Get(), format, channels, fill, D3D12_RESOURCE_STATE_COPY_DEST, w, h, perPixel);
        return t;
    }
    void Fill(ID3D12Resource* t, DXGI_FORMAT format, UINT channels, const float* fill, D3D12_RESOURCE_STATES state,
              UINT w = TW, UINT h = H, bool perPixel = false)
    {
        const UINT pitch = (w * channels * 4 + 255) & ~255u;
        auto up = Buffer(UINT64(pitch) * h, D3D12_HEAP_TYPE_UPLOAD);
        uint8_t* p = nullptr;
        up->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                std::memcpy(p + y * pitch + x * channels * 4, fill + (perPixel ? (y * w + x) * channels : 0),
                            channels * 4);
        up->Unmap(0, nullptr);
        D3D12_RESOURCE_BARRIER b {};
        b.Transition = { t, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, D3D12_RESOURCE_STATE_COPY_DEST };
        if (state != D3D12_RESOURCE_STATE_COPY_DEST)
            list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst { t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
        D3D12_TEXTURE_COPY_LOCATION src { up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
        src.PlacedFootprint.Footprint = { format, w, h, 1, pitch };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        Submit();
    }
    // The FP16 output: red and alpha at (x, y).
    std::vector<std::array<float, 4>> Read(ID3D12Resource* t, UINT w = TW, UINT h = H)
    {
        const UINT pitch = (w * 8 + 255) & ~255u;
        auto back = Buffer(UINT64(pitch) * h, D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_BARRIER b {};
        b.Transition = { t, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_COPY_SOURCE };
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst { back.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, 1, pitch };
        D3D12_TEXTURE_COPY_LOCATION src { t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        Submit();
        std::vector<std::array<float, 4>> out(w * h);
        uint8_t* p = nullptr;
        back->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                auto* v = reinterpret_cast<uint16_t*>(p + y * pitch) + x * 4;
                out[y * w + x] = { Half(v[0]), Half(v[1]), Half(v[2]), Half(v[3]) };
            }
        back->Unmap(0, nullptr);
        return out;
    }
};

// With a folder holding person-model\ and a still (binary PPM): the frame is captured, read back after the fence,
// segmented by the real worker, and its mask comes back to the composition on a later frame.
void EndToEnd(Gpu& gpu, const std::filesystem::path& dllFolder, const std::wstring& still, int model)
{
    std::ifstream in(still, std::ios::binary);
    std::string magic;
    unsigned w = 0, h = 0, maxValue = 0;
    if (!(in >> magic >> w >> h >> maxValue) || magic != "P6" || maxValue != 255)
    {
        std::printf("FAIL  cannot read %ls\n", still.c_str());
        ++failures;
        return;
    }
    in.get();
    std::vector<uint8_t> bytes(size_t(w) * h * 3);
    in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    // The capture encodes linear light to sRGB, so the still goes in as linear.
    std::vector<float> linear(size_t(w) * h * 4, 1.f);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        for (int c = 0; c < 3; ++c)
        {
            const float s = bytes[i * 3 + c] / 255.f;
            linear[i * 4 + c] = s <= .04045f ? s / 12.92f : std::pow((s + .055f) / 1.055f, 2.4f);
        }
    const float first[4] { .3f, .3f, .3f, 1 }, final[4] { .7f, .7f, .7f, 1 }, zero[2] {}, key[1] { .5f };
    auto colour = gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, 4, linear.data(), w, h, true);
    auto pass1 = gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, 4, first, w, h);
    auto result = gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, 4, final, w, h);
    auto motion = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, 2, zero, w, h);
    auto depth = gpu.Texture(DXGI_FORMAT_R32_FLOAT, 1, key, w, h);
    AmdPreSr::Frame f {};
    f.colour = colour.Get();
    f.motion = motion.Get();
    f.depth = depth.Get();
    f.width = w;
    f.height = h;
    Person::Partition partition(gpu.device.Get());
    ID3D12Resource* out = nullptr;
    bool ready = false, active = false;
    const auto start = GetTickCount64();
    for (int i = 0; GetTickCount64() - start < 15000 && !active; ++i)
    {
        ready = Person::Prepare(true, dllFolder, model, 320);
        out =
            partition.Record(gpu.list.Get(), f, pass1.Get(), result.Get(), false, i > 0, Person::Worker().Latest(), {});
        gpu.Submit(&partition);
        active = partition.Status() == "Person protection active";
        Sleep(16);
    }
    std::printf("      model %d: %s after %llu ms; worker: %s\n", model, partition.Status().c_str(),
                GetTickCount64() - start, Person::Worker().Status().c_str());
    Expect(ready && active, "a capture comes back from the worker as a current mask", active, 1);
    if (!active)
        return;
    const auto px = gpu.Read(out, w, h);
    size_t person = 0;
    for (const auto& v : px)
        person += std::abs(v[0] - .3f) < .01f;
    const double share = double(person) / px.size();
    // Faces only with YuNet: a few hundred pixels in a crowd shot.
    Expect(share > (model == 2 ? .0005 : .02) && share < .95, "part of the frame takes the first pass", share, .5);
    Person::Prepare(false, dllFolder, model, 320);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    Gpu gpu;
    const float base[4] { .2f, .2f, .2f, .37f }, first[4] { .3f, .3f, .3f, .9f }, final[4] { .7f, .7f, .7f, .9f };
    const float zero[2] {}, nearKey[1] { .5f }, farKey[1] { .9f };
    auto colour = gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, 4, base);
    auto pass1 = gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, 4, first);
    auto result = gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, 4, final);
    auto motion = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, 2, zero);
    auto depth = gpu.Texture(DXGI_FORMAT_R32_FLOAT, 1, nearKey);
    AmdPreSr::Frame f {};
    f.colour = colour.Get();
    f.motion = motion.Get();
    f.depth = depth.Get();
    f.width = W;
    f.height = H;
    Person::Partition partition(gpu.device.Get());
    // The mask's left half: the left half of the frame, which fills the letterbox's width.
    auto leftHalf = [&]
    {
        auto m = std::make_shared<Person::Mask>();
        m->epoch = partition.Epoch();
        m->frame = partition.Frame();
        m->tick = GetTickCount64();
        m->width = W;
        m->height = H;
        m->values.assign(160 * 160, 0.f);
        for (UINT y = 0; y < 160; ++y)
            for (UINT x = 0; x < 80; ++x)
                m->values[y * 160 + x] = 1;
        return m;
    };
    bool continuous = false;
    auto run = [&](std::shared_ptr<const Person::Mask> mask, Person::Settings settings = {})
    {
        ID3D12Resource* out =
            partition.Record(gpu.list.Get(), f, pass1.Get(), result.Get(), false, continuous, mask, settings);
        continuous = true;
        gpu.Submit(&partition);
        if (out == result.Get())
        {
            std::printf("FAIL  nothing recorded: %s\n", partition.Status().c_str());
            std::exit(1);
        }
        return gpu.Read(out);
    };
    const UINT left = (H / 2) * TW + W / 4, right = (H / 2) * TW + 3 * W / 4, outside = (H / 2) * TW + W + 4;

    auto px = run(nullptr);
    Expect(std::abs(px[left][0] - .7f) < .002f && std::abs(px[right][0] - .7f) < .002f,
           "no mask: the result everywhere", px[left][0], .7);
    Expect(std::abs(px[left][3] - .37f) < .001f, "the game's alpha", px[left][3], .37);
    Expect(std::abs(px[outside][0] - .7f) < .002f, "past the render subrect: the result", px[outside][0], .7);

    px = run(leftHalf());
    Expect(std::abs(px[left][0] - .3f) < .002f, "inside the mask: the first pass", px[left][0], .3);
    Expect(std::abs(px[right][0] - .7f) < .002f, "outside the mask: the result", px[right][0], .7);

    px = run(leftHalf(), { 0.f, 1.f });
    Expect(std::abs(px[left][0] - .2f) < .002f, "strength 0: the game's colour inside", px[left][0], .2);
    px = run(leftHalf(), { .5f, 1.f });
    Expect(std::abs(px[left][0] - .25f) < .002f, "strength 0.5: halfway to the first pass", px[left][0], .25);

    auto stale = leftHalf();
    stale->epoch += 7;
    px = run(stale);
    Expect(std::abs(px[left][0] - .7f) < .002f, "a mask of another stream is refused", px[left][0], .7);

    px = run(leftHalf(), { 1.f, 1.f, true });
    Expect(px[left][1] > px[left][0] + .3f, "the overlay tints the mask green", px[left][1] - px[left][0], .5);

    // The mask was taken a frame before the depth jumped: the trace back through the guides refuses it.
    auto before = leftHalf();
    run(nullptr);
    gpu.Fill(depth.Get(), DXGI_FORMAT_R32_FLOAT, 1, farKey, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    px = run(before);
    Expect(std::abs(px[left][0] - .7f) < .002f, "across a depth jump the mask is refused", px[left][0], .7);

    partition.Invalidate();
    px = run(before);
    Expect(std::abs(px[left][0] - .7f) < .002f, "after Invalidate an older mask is refused", px[left][0], .7);

    for (int model = 0; argc > 2 && model < 3; ++model)
        EndToEnd(gpu, argv[1], argv[2], model);
    const UINT64 errors = gpu.DebugErrors();
    Expect(errors == 0, "no debug layer errors", double(errors), 0);
    std::printf(failures ? "person partition: %d failed\n" : "person partition: PASS\n", failures);
    return failures ? 1 : 0;
}
