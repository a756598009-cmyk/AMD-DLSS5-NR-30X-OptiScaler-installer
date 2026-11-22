// mz_fov <MochizukiNrRuntime.dll> <modules_dir> <image.png> <out prefix> [frames]
// mz_e2e with the foveated grid (OptiScaler/dlssnr/amd/Foveation.h) around the runtime. Environment:
//   E2E_W / E2E_H   frame size (the image is scaled to it), default 1920x1080
//   FOV             0 off, 1 light, 2 medium, 3 strong; FOV_PARAMS=centreW,centreH,falloff,edge overrides
//   FOV_ALIGN       working extent alignment, default 64
//   E2E_PAN         pixels a frame the image moves right (motion vectors follow), 0 = still
//   E2E_DUMP        how many of the last frames to write (<prefix>_fN.bin, RGBA16F rows at the full size), default 1
//   E2E_NOGUIDES    no depth
//   E2E_FORMAT      rgba8: sRGB UNORM colour instead of RGBA16F
//   E2E_JITTER      1: a Halton sub-pixel jitter in the colour each frame (a render frame before SR)
//   E2E_CREATE_FLAGS  2: ACO's network
//   E2E_FEED        the RenoDX stage's HDR divisor feed (MochizukiRenoFeedMode)
// Prints the grid, the runtime's status (network median), and the GPU time of the warp and unwarp passes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "LmxxfNrApi.h"
#include "MochizukiNrControls.h"
#include "../../OptiScaler/dlssnr/amd/Foveation.h"

static void Require(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
static void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr)) { std::fprintf(stderr, "FAIL: %s hr=%08lx\n", what, (unsigned long) hr); std::exit(1); }
}
static std::wstring W(const char* s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring r(n ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, r.data(), n);
    return r;
}
static void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    c->ResourceBarrier(1, &x);
}
static uint16_t F2H(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    const uint32_t s = (b >> 16) & 0x8000;
    const int e = int((b >> 23) & 255) - 112;
    if (f == 0.f || e <= 0) return uint16_t(s);
    return uint16_t(s | (uint32_t(e) << 10) | ((b >> 13) & 1023));
}

int main(int argc, char** argv)
{
    if (argc < 5) { std::fprintf(stderr, "usage\n"); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int frames = argc > 5 ? std::atoi(argv[5]) : 40;
    auto env = [](const char* k) { return std::getenv(k); };
    auto envI = [&](const char* k, int d) { return env(k) ? std::atoi(env(k)) : d; };
    const int pan = envI("E2E_PAN", 0), dumps = envI("E2E_DUMP", 1);
    const UINT w = UINT(envI("E2E_W", 1920)), h = UINT(envI("E2E_H", 1080));
    AmdPreSr::Foveation::Params params = AmdPreSr::Foveation::Preset(envI("FOV", 0));
    if (env("FOV_PARAMS"))
        std::sscanf(env("FOV_PARAMS"), "%f,%f,%f,%f", &params.centreWidth, &params.centreHeight, &params.falloff,
                    &params.edge);
    const bool fov = envI("FOV", 0) != 0 || env("FOV_PARAMS");
    const auto grid = AmdPreSr::Foveation::MakeGrid(w, h, params, UINT(envI("FOV_ALIGN", 64)));
    std::printf("grid %ux%u -> %ux%u (%.1f%% of the pixels), x a=%.3f b=%.3f e=%.3f, y a=%.3f b=%.3f e=%.3f%s\n", w, h,
                grid.workWidth, grid.workHeight, 100 * grid.Share(), grid.x.a, grid.x.b, grid.x.e, grid.y.a, grid.y.b,
                grid.y.e, fov && grid.Active() ? "" : " (off)");
    const bool active = fov && grid.Active();
    // E2E_FORMAT=rgba8: an sRGB UNORM frame (display colour, the runtime's SDR route); the passes then run in every
    // mode, as a copy when off, so each dump is FP16 at the full size.
    const bool unorm = env("E2E_FORMAT") && !strcmp(env("E2E_FORMAT"), "rgba8"), run = active || unorm;
    AmdPreSr::Foveation::Grid use = grid;
    if (!active)
        use = { w, h, w, h };
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "com");

    IWICImagingFactory* wic = nullptr;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)), "wic");
    IWICBitmapDecoder* dec = nullptr;
    Check(wic->CreateDecoderFromFilename(W(argv[3]).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec), "decode");
    IWICBitmapFrameDecode* fr = nullptr;
    Check(dec->GetFrame(0, &fr), "frame");
    IWICBitmapScaler* scaler = nullptr;
    Check(wic->CreateBitmapScaler(&scaler), "scaler");
    Check(scaler->Initialize(fr, w, h, WICBitmapInterpolationModeFant), "scale");
    IWICFormatConverter* conv = nullptr;
    Check(wic->CreateFormatConverter(&conv), "conv");
    Check(conv->Initialize(scaler, GUID_WICPixelFormat64bppRGBAHalf, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "conv init");
    std::vector<uint16_t> pixels(size_t(w) * h * 4);
    Check(conv->CopyPixels(nullptr, w * 8, UINT(pixels.size() * 2), reinterpret_cast<BYTE*>(pixels.data())), "copy pixels");

    HMODULE dll = LoadLibraryW(W(argv[1]).c_str());
    Require(dll, "load runtime");
    auto getApi = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi*)>(GetProcAddress(dll, "LmxxfNrGetApi"));
    LmxxfNrApi api {};
    api.struct_size = sizeof(api);
    Require(getApi && getApi(LMXXF_NR_ABI_VERSION, &api) == LMXXF_NR_OK, "api");

    IDXGIFactory4* factory = nullptr;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d {};
        adapter->GetDesc1(&d);
        if (d.VendorId == 0x1002 && d.DedicatedVideoMemory > (1ull << 30) &&
            SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
            break;
        adapter->Release();
    }
    Require(device, "device");
    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    ID3D12Fence* fence = nullptr;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;
    auto wait = [&] {
        Check(queue->Signal(fence, ++fv), "signal");
        Check(fence->SetEventOnCompletion(fv, ev), "event");
        WaitForSingleObject(ev, 60000);
    };
    UINT64 tsFreq = 0;
    queue->GetTimestampFrequency(&tsFreq);
    ID3D12QueryHeap* qh = nullptr;
    D3D12_QUERY_HEAP_DESC qhd { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0 };
    Check(device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)), "query heap");

    D3D12_HEAP_PROPERTIES def { D3D12_HEAP_TYPE_DEFAULT }, up { D3D12_HEAP_TYPE_UPLOAD }, rb { D3D12_HEAP_TYPE_READBACK };
    auto tex = [&](DXGI_FORMAT f) {
        D3D12_RESOURCE_DESC td {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = td.MipLevels = 1;
        td.Format = f;
        td.SampleDesc.Count = 1;
        ID3D12Resource* r = nullptr;
        Check(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)), "texture");
        return r;
    };
    auto buffer = [&](D3D12_HEAP_PROPERTIES& hp, UINT64 size, D3D12_RESOURCE_STATES s) {
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = size;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* r = nullptr;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, s, nullptr, IID_PPV_ARGS(&r)), "buffer");
        return r;
    };
    struct Upload
    {
        ID3D12Resource *tex = nullptr, *buf = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    };
    auto upload = [&](DXGI_FORMAT f) {
        Upload u;
        u.tex = tex(f);
        auto d = u.tex->GetDesc();
        UINT64 total = 0;
        device->GetCopyableFootprints(&d, 0, 1, 0, &u.fp, nullptr, nullptr, &total);
        u.buf = buffer(up, total, D3D12_RESOURCE_STATE_GENERIC_READ);
        return u;
    };
    Upload colour = upload(unorm ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT),
           motion = upload(DXGI_FORMAT_R16G16_FLOAT), depth = upload(DXGI_FORMAT_R32_FLOAT);
    ID3D12Resource* readback = buffer(rb, UINT64(w) * h * 8 + (UINT64(4) << 20), D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tsRead = buffer(rb, 4 * 8, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint8_t> bytes;
    if (unorm)
    {
        auto h2f = [](uint16_t x) { uint32_t e = (x >> 10) & 31, mm = x & 1023; float v = e ? std::ldexp(float(mm | 1024), int(e) - 25) : std::ldexp(float(mm), -24); return (x & 0x8000) ? -v : v; };
        bytes.resize(pixels.size());
        for (size_t i = 0; i < pixels.size(); ++i)
        {
            float v = std::clamp(h2f(pixels[i]), 0.f, 1.f);
            if (i % 4 != 3)
                v = v <= .0031308f ? 12.92f * v : 1.055f * std::pow(v, 1 / 2.4f) - .055f;
            bytes[i] = uint8_t(v * 255 + .5f);
        }
    }
    // E2E_JITTER: the colour sampled bilinearly at a Halton (2, 3) sub-pixel offset each frame, as a jittered render
    // frame; the motion vectors stay jitter-free (the game's usual case).
    const bool jitter = envI("E2E_JITTER", 0) != 0;
    std::vector<float> linear;
    if (jitter)
    {
        auto h2f = [](uint16_t x) { uint32_t e = (x >> 10) & 31, mm = x & 1023; float v = e ? std::ldexp(float(mm | 1024), int(e) - 25) : std::ldexp(float(mm), -24); return (x & 0x8000) ? -v : v; };
        linear.resize(pixels.size());
        for (size_t i = 0; i < pixels.size(); ++i)
            linear[i] = unorm ? bytes[i] / 255.f : h2f(pixels[i]);
    }
    auto halton = [](int i, int b) { float f = 1, r = 0; for (; i > 0; i /= b) { f /= b; r += f * (i % b); } return r; };
    auto fill = [&](int shift, int frame) {
        uint8_t* m = nullptr;
        Check(colour.buf->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map");
        const float jx = jitter ? halton(frame % 8 + 1, 2) - .5f : 0, jy = jitter ? halton(frame % 8 + 1, 3) - .5f : 0;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                const size_t src = size_t(y) * w + (x + w - UINT(shift) % w) % w;
                uint8_t* dst = m + colour.fp.Offset + size_t(y) * colour.fp.Footprint.RowPitch;
                if (jitter)
                {
                    const float fx = float(x) + jx + 2 * w - float(shift % int(w)), fy = std::clamp(float(y) + jy, 0.f, float(h - 1));
                    const int x0 = int(fx), y0 = int(fy);
                    const float ax = fx - x0, ay = fy - y0;
                    const size_t r0 = size_t(y0) * w, r1 = size_t(std::min<int>(y0 + 1, int(h) - 1)) * w;
                    const size_t c0 = size_t(x0 % w), c1 = size_t((x0 + 1) % w);
                    for (int c = 0; c < 4; ++c)
                    {
                        const float v = (1 - ay) * ((1 - ax) * linear[(r0 + c0) * 4 + c] + ax * linear[(r0 + c1) * 4 + c]) +
                                        ay * ((1 - ax) * linear[(r1 + c0) * 4 + c] + ax * linear[(r1 + c1) * 4 + c]);
                        if (unorm)
                            dst[size_t(x) * 4 + c] = uint8_t(std::clamp(v, 0.f, 1.f) * 255 + .5f);
                        else
                            reinterpret_cast<uint16_t*>(dst)[size_t(x) * 4 + c] = F2H(v);
                    }
                }
                else if (unorm)
                    memcpy(dst + size_t(x) * 4, bytes.data() + src * 4, 4);
                else
                    memcpy(dst + size_t(x) * 8, pixels.data() + src * 4, 8);
            }
        colour.buf->Unmap(0, nullptr);
    };
    fill(0, 0);
    {
        uint8_t* m = nullptr;
        Check(motion.buf->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map mv");
        const uint16_t vx = F2H(-float(pan));
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                uint16_t* p = reinterpret_cast<uint16_t*>(m + motion.fp.Offset + size_t(y) * motion.fp.Footprint.RowPitch) + x * 2;
                p[0] = vx;
                p[1] = 0;
            }
        motion.buf->Unmap(0, nullptr);
        Check(depth.buf->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map depth");
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                const float d = 0.1f + 0.8f * float(y) / h;
                memcpy(m + depth.fp.Offset + size_t(y) * depth.fp.Footprint.RowPitch + x * 4, &d, 4);
            }
        depth.buf->Unmap(0, nullptr);
    }
    ID3D12CommandAllocator* alloc = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
    ID3D12GraphicsCommandList* list = nullptr;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)), "list");
    auto copyUp = [&](Upload& u, D3D12_RESOURCE_STATES from) {
        if (from != D3D12_RESOURCE_STATE_COPY_DEST)
            Barrier(list, u.tex, from, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION d { u.tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, s { u.buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        d.SubresourceIndex = 0;
        s.PlacedFootprint = u.fp;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        Barrier(list, u.tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    };
    copyUp(colour, D3D12_RESOURCE_STATE_COPY_DEST);
    copyUp(motion, D3D12_RESOURCE_STATE_COPY_DEST);
    copyUp(depth, D3D12_RESOURCE_STATE_COPY_DEST);
    Check(list->Close(), "close");
    ID3D12CommandList* l[] = { list };
    queue->ExecuteCommandLists(1, l);
    wait();

    const std::wstring modules = W(argv[2]);
    LmxxfNrCreateInfo ci {};
    ci.struct_size = sizeof(ci);
    ci.device = device;
    ci.queue = queue;
    ci.assets_directory = modules.c_str();
    ci.flags = uint32_t(envI("E2E_CREATE_FLAGS", 0)); // 2: ACO
    void* ctx = nullptr;
    char err[1024] {};
    if (api.Create(&ci, &ctx) != LMXXF_NR_OK || api.PrepareSession(ctx) != LMXXF_NR_OK)
    {
        api.GetLastError(err, sizeof err);
        std::fprintf(stderr, "FAIL: create/prepare: %s\n", err);
        return 1;
    }
    // E2E_FEED=n: the RenoDX stage's HDR divisor feed (0 auto, 1 the content meter, 2 the game's exposure: fixed 1
    // without a texture).
    if (env("E2E_FEED"))
    {
        auto defs = reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(GetProcAddress(dll, "MochizukiNrGetControlDefaults"));
        auto set = reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(dll, "MochizukiNrSetControls"));
        MochizukiNrControls c {};
        c.struct_size = sizeof(c);
        Require(defs && set && defs(&c) == LMXXF_NR_OK, "control defaults");
        c.reno_feed_mode = uint32_t(envI("E2E_FEED", 0));
        Require(set(ctx, &c) == LMXXF_NR_OK, "SetControls");
        std::printf("controls: feed %u\n", c.reno_feed_mode);
    }
    AmdPreSr::Foveation::Passes passes(device);
    std::vector<double> times, warpMs, unwarpMs;
    for (int f = 0; f < frames; ++f)
    {
        const auto t0 = std::chrono::steady_clock::now();
        Check(alloc->Reset(), "alloc reset");
        Check(list->Reset(alloc, nullptr), "list reset");
        if (pan || jitter)
        {
            fill(f * pan, f);
            copyUp(colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        AmdPreSr::Frame full {};
        full.colour = colour.tex;
        full.motion = motion.tex;
        full.depth = env("E2E_NOGUIDES") ? nullptr : depth.tex;
        full.width = w;
        full.height = h;
        AmdPreSr::Frame in = full;
        list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        if (run)
            in = passes.Warp(list, full, use);
        list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        LmxxfNrFrameInfo fi {};
        fi.struct_size = sizeof(fi);
        fi.session_id = 1;
        fi.frame_id = uint64_t(f + 1);
        fi.command_list = list;
        fi.color_width = in.width;
        fi.color_height = in.height;
        fi.color = in.colour;
        fi.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        fi.flags = LMXXF_NR_FRAME_FLAG_STRENGTH | LMXXF_NR_FRAME_FLAG_TEMPORAL;
        fi.transfer_strength = fi.color_strength = 1.f;
        fi.model_scale = 1.f;
        fi.passes = 1;
        fi.motion = in.motion;
        fi.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        fi.motion_width = in.width;
        fi.motion_height = in.height;
        fi.motion_scale_x = in.motionScaleX;
        fi.motion_scale_y = in.motionScaleY;
        fi.depth = in.depth;
        fi.depth_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        LmxxfNrJob job {};
        job.struct_size = sizeof(job);
        for (int tries = 0;; ++tries)
        {
            const int32_t rc = api.PrepareFrame(ctx, &fi, &job);
            if (rc == LMXXF_NR_OK)
                break;
            api.GetLastError(err, sizeof err);
            if (rc == LMXXF_NR_UNAVAILABLE && tries < 9000 && !strstr(err, "build failed") && !strstr(err, "network off"))
            {
                Sleep(100);
                continue;
            }
            std::fprintf(stderr, "FAIL: PrepareFrame rc=%d: %s\n", rc, err);
            return 1;
        }
        if (f == 0)
            std::printf("ready after %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        Require(api.RecordInputs(ctx, job.handle, list) == LMXXF_NR_OK, "RecordInputs");
        Check(list->Close(), "close in");
        queue->ExecuteCommandLists(1, l);
        if (api.EnqueueHip(ctx, job.handle, queue) != LMXXF_NR_OK)
        {
            api.GetLastError(err, sizeof err);
            std::fprintf(stderr, "FAIL: EnqueueHip: %s\n", err);
            return 1;
        }
        wait();
        Check(alloc->Reset(), "alloc reset 2");
        Check(list->Reset(alloc, nullptr), "list reset 2");
        Require(api.RecordOutputs(ctx, job.handle, list) == LMXXF_NR_OK, "RecordOutputs");
        auto* out = static_cast<ID3D12Resource*>(job.private_output);
        list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 2);
        if (run)
            out = passes.Unwarp(list, full, out);
        list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 3);
        list->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, tsRead, 0);
        const bool dump = f >= frames - dumps;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT ofp {};
        if (dump)
        {
            Barrier(list, out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            const auto od = out->GetDesc();
            Require(od.Width >= w && od.Height >= h && od.Format == DXGI_FORMAT_R16G16B16A16_FLOAT, "output at the full size, FP16");
            device->GetCopyableFootprints(&od, 0, 1, 0, &ofp, nullptr, nullptr, nullptr);
            D3D12_TEXTURE_COPY_LOCATION d { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, s { out, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            d.PlacedFootprint = ofp;
            s.SubresourceIndex = 0;
            list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            Barrier(list, out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Check(list->Close(), "close out");
        queue->ExecuteCommandLists(1, l);
        wait();
        Require(api.Retire(ctx, job.handle) == LMXXF_NR_OK, "Retire");
        {
            uint64_t* t = nullptr;
            Check(tsRead->Map(0, nullptr, reinterpret_cast<void**>(&t)), "map ts");
            warpMs.push_back(double(t[1] - t[0]) * 1000.0 / double(tsFreq));
            unwarpMs.push_back(double(t[3] - t[2]) * 1000.0 / double(tsFreq));
            tsRead->Unmap(0, nullptr);
        }
        if (dump)
        {
            uint8_t* m = nullptr;
            Check(readback->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map rb");
            const std::string name = std::string(argv[4]) + "_f" + std::to_string(f) + ".bin";
            FILE* o = std::fopen(name.c_str(), "wb");
            for (UINT y = 0; y < h; ++y)
                std::fwrite(m + ofp.Offset + size_t(y) * ofp.Footprint.RowPitch, 1, size_t(w) * 8, o);
            std::fclose(o);
            readback->Unmap(0, nullptr);
        }
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    api.GetStatus(ctx, err, sizeof err);
    std::printf("status: %s\n", err);
    auto median = [](std::vector<double> v) {
        v.erase(v.begin(), v.begin() + std::min<size_t>(10, v.size() - 1));
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::printf("frame times ms:");
    for (double t : times) std::printf(" %.2f", t);
    std::printf("\n");
    std::printf("frame ms median %.2f, warp %.3f ms, unwarp %.3f ms\n", median(times), median(warpMs), median(unwarpMs));
    api.Destroy(ctx);
    std::printf("ok\n");
    return 0;
}
