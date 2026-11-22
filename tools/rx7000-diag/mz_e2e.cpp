// mz_e2e <MochizukiNrRuntime.dll> <modules_dir> <image.png> <out.bin> [frames] [passes]
// Runs the runtime on one image for N frames (full sync each frame), prints per-frame wall time, and dumps the
// last output (RGBA16F rows) for comparison.
#define WIN32_LEAN_AND_MEAN
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

int main(int argc, char** argv)
{
    if (argc < 5) { std::fprintf(stderr, "usage\n"); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int frames = argc > 5 ? std::atoi(argv[5]) : 40;
    const unsigned passes = argc > 6 ? unsigned(std::atoi(argv[6])) : 1;
    auto envU = [](const char* k, unsigned d) { const char* v = std::getenv(k); return v ? unsigned(std::strtoul(v, nullptr, 0)) : d; };
    const unsigned flagsA = envU("E2E_FLAGS", 0), flagsB = envU("E2E_FLAGS_B", flagsA);
    const int pan = int(envU("E2E_PAN", 0));
    const float mvSign = std::getenv("E2E_MVSIGN") ? float(std::atof(std::getenv("E2E_MVSIGN"))) : -1.f;
    const bool noMotion = std::getenv("E2E_NOMOTION") != nullptr;
    const bool r10 = std::getenv("E2E_FORMAT") && !strcmp(std::getenv("E2E_FORMAT"), "r10");
    const int styleA = argc > 7 ? std::atoi(argv[7]) : -1, styleB = argc > 8 ? std::atoi(argv[8]) : styleA;
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "com");

    // Image -> RGBA16F pixels.
    IWICImagingFactory* wic = nullptr;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)), "wic");
    IWICBitmapDecoder* dec = nullptr;
    Check(wic->CreateDecoderFromFilename(W(argv[3]).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec), "decode");
    IWICBitmapFrameDecode* fr = nullptr;
    Check(dec->GetFrame(0, &fr), "frame");
    IWICBitmapScaler* scaler = nullptr;
    Check(wic->CreateBitmapScaler(&scaler), "scaler");
    Check(scaler->Initialize(fr, std::getenv("E2E_W") ? UINT(std::atoi(std::getenv("E2E_W"))) : 1920u, std::getenv("E2E_H") ? UINT(std::atoi(std::getenv("E2E_H"))) : 1080u, WICBitmapInterpolationModeFant), "scale");
    IWICFormatConverter* conv = nullptr;
    Check(wic->CreateFormatConverter(&conv), "conv");
    Check(conv->Initialize(scaler, GUID_WICPixelFormat64bppRGBAHalf, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "conv init");
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    std::vector<uint16_t> pixels(size_t(w) * h * 4);
    Check(conv->CopyPixels(nullptr, w * 8, UINT(pixels.size() * 2), reinterpret_cast<BYTE*>(pixels.data())), "copy pixels");
    std::printf("image %ux%u\n", w, h);

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

    D3D12_HEAP_PROPERTIES def { D3D12_HEAP_TYPE_DEFAULT }, up { D3D12_HEAP_TYPE_UPLOAD }, rb { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC td {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = td.MipLevels = 1;
    const bool f32 = std::getenv("E2E_FORMAT") && !strcmp(std::getenv("E2E_FORMAT"), "f32");
    td.Format = r10 ? DXGI_FORMAT_R10G10B10A2_UNORM : f32 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* color = nullptr;
    Check(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&color)), "color");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *upload = nullptr, *readback = nullptr;
    Check(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "upload");
    D3D12_RESOURCE_DESC rbd = bd;
    rbd.Width = UINT64(w) * h * 16 + (UINT64(4) << 20);
    Check(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rbd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "readback");
    auto fill = [&](int shift) {
        uint8_t* m = nullptr;
        Check(upload->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map");
        auto h2f = [](uint16_t x) { uint32_t e = (x >> 10) & 31, mm = x & 1023; float v = e ? std::ldexp(float(mm | 1024), int(e) - 25) : std::ldexp(float(mm), -24); return (x & 0x8000) ? -v : v; };
        for (UINT y = 0; y < h; ++y)
        {
            uint8_t* row = m + fp.Offset + size_t(y) * fp.Footprint.RowPitch;
            if (f32)
            {
                for (UINT x = 0; x < w; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        // between two halves: the f16 value nudged by a third of an ulp, up or down by pixel
                        float v = h2f(pixels[(size_t(y) * w + (x + w - UINT(shift) % w) % w) * 4 + c]);
                        v *= 1.f + float(int((x * 7 + y * 13 + UINT(c)) % 3u) - 1) * 0.0003f;
                        memcpy(row + size_t(x) * 16 + c * 4, &v, 4); if (y == 540 && x == 960 && shift == 0) std::printf("in px %d = %g\n", c, v);
                    }
                continue;
            }
            if (!r10)
            {
                for (UINT x = 0; x < w; ++x)
                    memcpy(row + size_t(x) * 8, pixels.data() + (size_t(y) * w + (x + w - UINT(shift) % w) % w) * 4, 8);
                continue;
            }
            for (UINT x = 0; x < w; ++x)
            {
                const uint16_t* p = &pixels[(size_t(y) * w + x) * 4];
                auto q = [&](int c) { float v = std::clamp(h2f(p[c]), 0.f, 1.f); return uint32_t(v * 1023.f + .5f); };
                const uint32_t v = q(0) | q(1) << 10 | q(2) << 20 | 3u << 30;
                memcpy(row + x * 4, &v, 4);
            }
        }
        upload->Unmap(0, nullptr);
    };
    fill(0);

    ID3D12Resource* motion = nullptr;
    {
        D3D12_RESOURCE_DESC md = td;
        md.Format = DXGI_FORMAT_R16G16_FLOAT;
        md.Flags = D3D12_RESOURCE_FLAG_NONE;
        Check(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &md, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&motion)), "motion");
    }
    ID3D12Resource* mupload = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT mfp {};
    {
        D3D12_RESOURCE_DESC md = motion->GetDesc();
        UINT64 mt = 0;
        device->GetCopyableFootprints(&md, 0, 1, 0, &mfp, nullptr, nullptr, &mt);
        D3D12_RESOURCE_DESC mb = bd;
        mb.Width = mt;
        Check(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &mb, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mupload)), "mupload");
        uint8_t* m = nullptr;
        Check(mupload->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map mv");
        auto f2h = [](float f) { uint32_t b; memcpy(&b, &f, 4); uint32_t sgn = (b >> 16) & 0x8000; int e = int((b >> 23) & 255) - 112; uint32_t mm = (b >> 13) & 1023; if (f == 0.f) return uint16_t(sgn); return uint16_t(sgn | (uint32_t(e) << 10) | mm); };
        const uint16_t vx = f2h(mvSign * float(pan)), vy = 0;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                uint16_t* p = reinterpret_cast<uint16_t*>(m + mfp.Offset + size_t(y) * mfp.Footprint.RowPitch) + x * 2;
                p[0] = vx;
                p[1] = vy;
            }
        mupload->Unmap(0, nullptr);
    }
    ID3D12CommandAllocator* alloc = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
    ID3D12GraphicsCommandList* list = nullptr;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)), "list");
    {
        D3D12_TEXTURE_COPY_LOCATION d { color, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, s { upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        d.SubresourceIndex = 0;
        s.PlacedFootprint = fp;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        Barrier(list, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_TEXTURE_COPY_LOCATION md { motion, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, ms { mupload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        md.SubresourceIndex = 0;
        ms.PlacedFootprint = mfp;
        list->CopyTextureRegion(&md, 0, 0, 0, &ms, nullptr);
        Barrier(list, motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Check(list->Close(), "close");
        ID3D12CommandList* l[] = { list };
        queue->ExecuteCommandLists(1, l);
        wait();
    }

    const std::wstring modules = W(argv[2]);
    LmxxfNrCreateInfo ci {};
    ci.struct_size = sizeof(ci);
    ci.device = device;
    ci.queue = queue;
    ci.assets_directory = modules.c_str();
    ci.flags = envU("E2E_CREATE_FLAGS", 0);   // 2: ACO (our references only)
    void* ctx = nullptr;
    char err[1024] {};
    if (api.Create(&ci, &ctx) != LMXXF_NR_OK || api.PrepareSession(ctx) != LMXXF_NR_OK)
    {
        api.GetLastError(err, sizeof err);
        std::fprintf(stderr, "FAIL: create/prepare: %s\n", err);
        return 1;
    }

    // E2E_PREP: the preprocess on; E2E_RENO_STYLE=n: the RenoDX stage's Style (1 Natural, 2 Cinematic: Style grade).
    if (std::getenv("E2E_PREP") || std::getenv("E2E_RENO_STYLE"))
    {
        auto defs = reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(GetProcAddress(dll, "MochizukiNrGetControlDefaults"));
        auto set = reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(dll, "MochizukiNrSetControls"));
        MochizukiNrControls c {};
        c.struct_size = sizeof(c);
        Require(defs && set && defs(&c) == LMXXF_NR_OK, "control defaults");
        if (std::getenv("E2E_PREP"))
            c.preprocess = 1;
        if (const char* st = std::getenv("E2E_RENO_STYLE"))
            c.reno_style = uint32_t(std::atoi(st));
        Require(set(ctx, &c) == LMXXF_NR_OK, "SetControls");
        std::printf("controls: preprocess %u, RenoDX style %u\n", c.preprocess, c.reno_style);
    }
    std::vector<double> times;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ofp {};
    size_t outBpp = 8;
    for (int f = 0; f < frames; ++f)
    {
        const auto t0 = std::chrono::steady_clock::now();
        Check(alloc->Reset(), "alloc reset");
        Check(list->Reset(alloc, nullptr), "list reset");
        if (pan)
        {
            fill(f * pan);
            Barrier(list, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION d { color, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, s { upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            d.SubresourceIndex = 0;
            s.PlacedFootprint = fp;
            list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            Barrier(list, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        LmxxfNrFrameInfo fi {};
        fi.struct_size = sizeof(fi);
        fi.session_id = 1;
        fi.frame_id = uint64_t(f + 1);
        fi.command_list = list;
        fi.color_width = w;
        fi.color_height = h;
        fi.color = color;
        fi.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        fi.flags = LMXXF_NR_FRAME_FLAG_STRENGTH;
        fi.transfer_strength = std::getenv("E2E_STRENGTH") ? float(std::atof(std::getenv("E2E_STRENGTH"))) : 1.f;
        fi.color_strength = fi.transfer_strength;
        fi.model_scale = 1.f;
        fi.passes = passes;
        if (!noMotion)
        {
            fi.flags |= LMXXF_NR_FRAME_FLAG_TEMPORAL;
            fi.motion = motion;
            fi.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            fi.motion_width = w;
            fi.motion_height = h;
            fi.motion_scale_x = 1.f;
            fi.motion_scale_y = 1.f;
        }
        fi.flags |= f < frames / 2 ? flagsA : flagsB;
        if (const int st = f < frames / 2 ? styleA : styleB; st >= 0)
        {
            fi.flags |= LMXXF_NR_FRAME_FLAG_STYLE;
            fi.style = uint32_t(st);
        }
        LmxxfNrJob job {};
        job.struct_size = sizeof(job);
        for (int tries = 0;; ++tries)
        {
            const int32_t rc = api.PrepareFrame(ctx, &fi, &job);
            if (rc == LMXXF_NR_OK)
                break;
            api.GetLastError(err, sizeof err);
            // Up to 15 min for a cold build; a failed one is not tried again, so stop there.
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
        ID3D12CommandList* l[] = { list };
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
        if (api.RecordOutputs(ctx, job.handle, list) != LMXXF_NR_OK)
        {
            api.GetLastError(err, sizeof err);
            std::fprintf(stderr, "FAIL: RecordOutputs: %s\n", err);
            return 1;
        }
        auto* out = static_cast<ID3D12Resource*>(job.private_output);
        const bool last = f + 1 == frames;
        if (last)
        {
            Barrier(list, out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            const auto od = out->GetDesc();
            Require(od.Width == w && od.Height == h, "output at the input size");
            outBpp = od.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16 : od.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
            if (f + 1 == frames)
                std::printf("output format %d, %zu bytes a pixel\n", int(od.Format), outBpp);
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
        if (last)
        {
            uint8_t* m = nullptr;
            Check(readback->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map rb");
            std::string name = std::string(argv[4]) + (f + 1 == frames ? "" : ".prev");
            FILE* o = std::fopen(name.c_str(), "wb");
            for (UINT y = 0; y < h; ++y)
                std::fwrite(m + ofp.Offset + size_t(y) * ofp.Footprint.RowPitch, 1, size_t(w) * outBpp, o);
            std::fclose(o);
            readback->Unmap(0, nullptr);
        }
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    api.GetStatus(ctx, err, sizeof err);
    std::printf("status: %s\n", err);
    std::vector<double> warm(times.begin() + std::min<size_t>(10, times.size() - 1), times.end());
    std::sort(warm.begin(), warm.end());
    std::printf("frame times ms:");
    for (double t : times) std::printf(" %.2f", t);
    std::printf("\n");
    std::printf("frame ms: first=%.1f median=%.2f min=%.2f p90=%.2f (n=%zu)\n", times[0], warm[warm.size() / 2], warm[0],
                warm[warm.size() * 9 / 10], warm.size());
    api.Destroy(ctx);
    std::printf("ok\n");
    return 0;
}
