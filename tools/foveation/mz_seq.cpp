// mz_seq <MochizukiNrRuntime.dll> <modules_dir> <scene dir> <denoised|output> <in prefix> <out prefix> <frames>
// A sequence through mochizuki with OpenRR's two hops (FASE5-NR), on a frame dump of opennr_rr_validate:
//   denoised  in = the hop image (ONR_VALIDATE_HOP_DUMP, rgba32f rows at the render size), jittered; out = mochizuki's
//             answer, rgba16f rows at the render size, for ONR_VALIDATE_EDIT_FILES. As the game with NrImage=denoised.
//   output    in = RR's output (ONR_VALIDATE_DUMP, rgba16f rows at OUT_W x OUT_H, default 1920x1080); it is scaled to the
//             render size, goes through mochizuki with no jitter, and the change comes back on the output in log
//             (Foveation.h). out = that picture. As the game with NrImage=output.
// The vectors and the jitter come from the scene (sequence.json, motion_vectors). Environment:
//   FOV             0 off, 1..3 foveation's presets, as in the game
//   SEQ_EDIT        sharpen=k: no network, x + k (x - box3 x) on the image the network would get, on the CPU
//   SEQ_STYLE       reno_style (0 standard, 1 natural), default 1; SEQ_PASSES reno_passes, default 1
//   SEQ_TEMPORAL    0: no vectors, no history
//   SEQ_LOGK        the change's log2(k x + 1), default 100
//   SEQ_ALPHA       still,move rates of the delta's history; SEQ_CLIP its clip (OpenNrRrOutputNr::Options)
//   SEQ_FLAGS       create flags, default 2 (ACO's network, the game's default)
//   SEQ_FOLD        1: the jitter step folded into the vectors, with FOV only (MochizukiJitterHistory)
//   SEQ_REJECT      where the history starts to go and is gone (OpenNrRrOutputNr::Options)
// Prints the network's status and the GPU time of the scale and composite passes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "LmxxfNrApi.h"
#include "MochizukiNrControls.h"
#include "../../OptiScaler/dlssnr/amd/Foveation.h"
#include "../../OptiScaler/upscalers/opennr_rr/OpenNrRrOutputNr.h"
namespace Fov = AmdPreSr::Foveation;

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
    if (f != f) return uint16_t(s);
    if (f == 0.f || e <= 0) return uint16_t(s);
    if (e >= 31) return uint16_t(s | 0x7bff);
    return uint16_t(s | (uint32_t(e) << 10) | ((b >> 13) & 1023));
}
static float H2F(uint16_t x)
{
    uint32_t e = (x >> 10) & 31, m = x & 1023;
    float v = e ? std::ldexp(float(m | 1024), int(e) - 25) : std::ldexp(float(m), -24);
    return (x & 0x8000) ? -v : v;
}
static std::vector<uint8_t> ReadAll(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) { std::fprintf(stderr, "FAIL: cannot read %s\n", p.c_str()); std::exit(1); }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}
// the n-th frame's "jitter" of sequence.json, and render_size
static std::vector<double> JsonList(const std::string& js, size_t from, const char* key)
{
    std::vector<double> v;
    size_t k = js.find(std::string("\"") + key + "\"", from);
    if (k == std::string::npos) return v;
    size_t p = js.find('[', k) + 1;
    while (js[p] != ']')
    {
        char* e;
        v.push_back(strtod(js.c_str() + p, &e));
        p = size_t(e - js.c_str());
        while (js[p] == ',' || isspace(uint8_t(js[p]))) ++p;
    }
    return v;
}

int main(int argc, char** argv)
{
    if (argc < 8) { std::fprintf(stderr, "usage\n"); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    auto env = [](const char* k) { return std::getenv(k); };
    auto envI = [&](const char* k, int d) { return env(k) ? std::atoi(env(k)) : d; };
    const std::string scene = argv[3], inPrefix = argv[5], outPrefix = argv[6];
    const bool outputMode = !strcmp(argv[4], "output");
    const int frames = std::atoi(argv[7]);
    const std::string js = [&] { auto b = ReadAll(scene + "\\sequence.json"); return std::string(b.begin(), b.end()); }();
    const auto rs = JsonList(js, 0, "render_size");
    const UINT w = UINT(rs[0]), h = UINT(rs[1]);
    const UINT ow = outputMode ? UINT(envI("OUT_W", 1920)) : w, oh = outputMode ? UINT(envI("OUT_H", 1080)) : h;
    const float sharpen = env("SEQ_EDIT") && !strncmp(env("SEQ_EDIT"), "sharpen=", 8) ? float(atof(env("SEQ_EDIT") + 8)) : 0.f;
    const bool temporal = envI("SEQ_TEMPORAL", 1) != 0, fold = envI("SEQ_FOLD", 0) != 0;
    OpenNrRrOutputNr::Options outputOptions;
    if (env("SEQ_LOGK"))
        outputOptions.logScale = float(atof(env("SEQ_LOGK")));
    if (env("SEQ_ALPHA"))
        std::sscanf(env("SEQ_ALPHA"), "%f,%f", &outputOptions.alphaStill, &outputOptions.alphaMove);
    if (env("SEQ_CLIP"))
        outputOptions.clip = float(atof(env("SEQ_CLIP")));
    if (env("SEQ_REJECT"))
        std::sscanf(env("SEQ_REJECT"), "%f,%f", &outputOptions.reject0, &outputOptions.reject1);
    const int fovMode = envI("FOV", 0);
    const auto grid = fovMode ? Fov::MakeGrid(w, h, Fov::Preset(fovMode)) : Fov::Grid { w, h, w, h };

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
    auto tex = [&](UINT tw, UINT th, DXGI_FORMAT f) {
        D3D12_RESOURCE_DESC td {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = tw;
        td.Height = th;
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
    const auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    struct Upload
    {
        ID3D12Resource *tex = nullptr, *buf = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        uint8_t* map = nullptr;
    };
    auto upload = [&](UINT tw, UINT th, DXGI_FORMAT f) {
        Upload u;
        u.tex = tex(tw, th, f);
        auto d = u.tex->GetDesc();
        UINT64 total = 0;
        device->GetCopyableFootprints(&d, 0, 1, 0, &u.fp, nullptr, nullptr, &total);
        u.buf = buffer(up, total, D3D12_RESOURCE_STATE_GENERIC_READ);
        Check(u.buf->Map(0, nullptr, reinterpret_cast<void**>(&u.map)), "map");
        return u;
    };
    // colour: the hop image, or RR's output
    Upload colour = upload(ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT), motion = upload(w, h, DXGI_FORMAT_R16G16_FLOAT);
    Upload edited = upload(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ID3D12Resource* readback = buffer(rb, UINT64(ow) * oh * 8 + (UINT64(8) << 20), D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tsRead = buffer(rb, 4 * 8, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12CommandAllocator* alloc = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
    ID3D12GraphicsCommandList* list = nullptr;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)), "list");
    Check(list->Close(), "close");
    ID3D12CommandList* l[] = { list };
    auto copyUp = [&](Upload& u, D3D12_RESOURCE_STATES from) {
        if (from != D3D12_RESOURCE_STATE_COPY_DEST)
            Barrier(list, u.tex, from, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION d { u.tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, s { u.buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        d.SubresourceIndex = 0;
        s.PlacedFootprint = u.fp;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        Barrier(list, u.tex, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
    };
    D3D12_RESOURCE_STATES colourNow = D3D12_RESOURCE_STATE_COPY_DEST, motionNow = D3D12_RESOURCE_STATE_COPY_DEST, editedNow = D3D12_RESOURCE_STATE_COPY_DEST;

    HMODULE dll = nullptr;
    LmxxfNrApi api {};
    api.struct_size = sizeof(api);
    void* ctx = nullptr;
    char err[1024] {};
    const std::wstring modules = W(argv[2]);
    if (!sharpen)
    {
        dll = LoadLibraryW(W(argv[1]).c_str());
        Require(dll, "load runtime");
        auto getApi = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi*)>(GetProcAddress(dll, "LmxxfNrGetApi"));
        Require(getApi && getApi(LMXXF_NR_ABI_VERSION, &api) == LMXXF_NR_OK, "api");
        LmxxfNrCreateInfo ci {};
        ci.struct_size = sizeof(ci);
        ci.device = device;
        ci.queue = queue;
        ci.assets_directory = modules.c_str();
        ci.flags = uint32_t(envI("SEQ_FLAGS", 2));
        if (api.Create(&ci, &ctx) != LMXXF_NR_OK || api.PrepareSession(ctx) != LMXXF_NR_OK)
        {
            api.GetLastError(err, sizeof err);
            std::fprintf(stderr, "FAIL: create/prepare: %s\n", err);
            return 1;
        }
        auto defs = reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(GetProcAddress(dll, "MochizukiNrGetControlDefaults"));
        auto set = reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(dll, "MochizukiNrSetControls"));
        MochizukiNrControls c {};
        c.struct_size = sizeof(c);
        Require(defs && set && defs(&c) == LMXXF_NR_OK, "control defaults");
        c.renodx = 1;
        c.reno_nr_enabled = 1;
        c.reno_style = uint32_t(envI("SEQ_STYLE", 1));
        c.reno_passes = uint32_t(envI("SEQ_PASSES", 1));
        Require(set(ctx, &c) == LMXXF_NR_OK, "SetControls");
    }

    Fov::Passes foveation(device);
    OpenNrRrOutputNr::Passes scale(device);
    std::vector<double> scaleMs, composeMs, frameMs;
    float lastJx = 0, lastJy = 0;
    size_t at = js.find("\"frames\"");
    uint64_t frameId = 0;
    for (int f = 0; f < frames; ++f)
    {
        const auto t0 = std::chrono::steady_clock::now();
        at = js.find("\"jitter\"", at + 1);
        const auto jit = JsonList(js, at, "jitter");
        const float jx = float(jit[0]), jy = float(jit[1]);
        // the inputs (the validator's first network frame has no hop)
        char name[32];
        std::snprintf(name, sizeof name, "%04d.raw", f);
        if (GetFileAttributesA((inPrefix + name).c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        {
            const auto in = ReadAll(inPrefix + name);
            const auto& fp = colour.fp;
            for (UINT y = 0; y < oh; ++y)
            {
                auto* o = reinterpret_cast<uint16_t*>(colour.map + fp.Offset + size_t(y) * fp.Footprint.RowPitch);
                if (outputMode)
                    memcpy(o, in.data() + size_t(y) * ow * 8, size_t(ow) * 8);
                else
                {
                    auto* src = reinterpret_cast<const float*>(in.data()) + size_t(y) * ow * 4;
                    for (UINT x = 0; x < ow * 4; ++x)
                        o[x] = F2H(src[x]);
                }
            }
            const auto mv = ReadAll(scene + "\\motion_vectors\\" + name);
            for (UINT y = 0; y < h; ++y)
                memcpy(motion.map + motion.fp.Offset + size_t(y) * motion.fp.Footprint.RowPitch, mv.data() + size_t(y) * w * 4, size_t(w) * 4);
        }
        Check(alloc->Reset(), "alloc reset");
        Check(list->Reset(alloc, nullptr), "list reset");
        copyUp(colour, colourNow);
        copyUp(motion, motionNow);
        colourNow = motionNow = SRV;

        AmdPreSr::Frame full {};
        full.colour = colour.tex;
        full.colourState = SRV;
        full.width = ow;
        full.height = oh;
        AmdPreSr::Frame image = full;
        list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        if (outputMode)
        {
            image.colour = scale.Reduce(list, full, w, h);
            image.width = w;
            image.height = h;
        }
        list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        image.motion = temporal ? motion.tex : nullptr;
        image.motionState = SRV;
        image.motionWidth = w;
        image.motionHeight = h;
        image.motionScaleX = image.motionScaleY = 1;
        image.depth = nullptr;

        AmdPreSr::Frame in = image;
        const bool foveated = grid.Active();
        Fov::Passes::Options fovOptions;
        if (fold && !outputMode && f)
            fovOptions.jitterStepX = lastJx - jx, fovOptions.jitterStepY = lastJy - jy;
        if (foveated)
            in = foveation.Warp(list, image, grid, fovOptions);
        lastJx = jx;
        lastJy = jy;

        ID3D12Resource* result = nullptr;
        LmxxfNrJob job {};
        if (sharpen)
        {
            // what the network would get, read back, edited, as its answer
            Require(!foveated, "SEQ_EDIT without foveation");
            Barrier(list, in.colour, SRV, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION d { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, s { in.colour, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            d.PlacedFootprint = edited.fp;
            s.SubresourceIndex = 0;
            list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            Barrier(list, in.colour, D3D12_RESOURCE_STATE_COPY_SOURCE, SRV);
            Check(list->Close(), "close");
            queue->ExecuteCommandLists(1, l);
            wait();
            uint8_t* m = nullptr;
            Check(readback->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map rb");
            const UINT pitch = edited.fp.Footprint.RowPitch;
            auto px = [&](int x, int y) { return reinterpret_cast<const uint16_t*>(m + edited.fp.Offset + size_t(std::clamp(y, 0, int(h) - 1)) * pitch) + 4 * std::clamp(x, 0, int(w) - 1); };
            for (int y = 0; y < int(h); ++y)
            {
                auto* o = reinterpret_cast<uint16_t*>(edited.map + edited.fp.Offset + size_t(y) * pitch);
                for (int x = 0; x < int(w); ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        float sum = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                                sum += H2F(px(x + dx, y + dy)[c]);
                        const float v = H2F(px(x, y)[c]);
                        o[4 * x + c] = F2H(c == 3 ? v : std::max(0.f, v + sharpen * (v - sum / 9.f)));
                    }
            }
            readback->Unmap(0, nullptr);
            Check(alloc->Reset(), "alloc reset 2");
            Check(list->Reset(alloc, nullptr), "list reset 2");
            copyUp(edited, editedNow);
            editedNow = SRV;
            result = edited.tex;
        }
        else
        {
            LmxxfNrFrameInfo fi {};
            fi.struct_size = sizeof(fi);
            fi.session_id = 1;
            fi.frame_id = ++frameId;
            fi.command_list = list;
            fi.color_width = in.width;
            fi.color_height = in.height;
            fi.color = in.colour;
            fi.color_state = SRV;
            fi.flags = LMXXF_NR_FRAME_FLAG_STRENGTH;
            fi.transfer_strength = fi.color_strength = 1.f;
            fi.model_scale = 1.f;
            fi.passes = 1;
            if (temporal)
            {
                fi.flags |= LMXXF_NR_FRAME_FLAG_TEMPORAL;
                fi.motion = in.motion;
                fi.motion_state = SRV;
                fi.motion_width = in.motionWidth ? in.motionWidth : in.width;
                fi.motion_height = in.motionHeight ? in.motionHeight : in.height;
                fi.motion_scale_x = in.motionScaleX;
                fi.motion_scale_y = in.motionScaleY;
                fi.reset = frameId == 1;
            }
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
            result = static_cast<ID3D12Resource*>(job.private_output);
        }
        list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 2);
        if (foveated)
            result = foveation.Unwarp(list, image, result);
        if (outputMode)
        {
            AmdPreSr::Frame guides = full;
            guides.motion = image.motion;
            guides.motionState = SRV;
            guides.motionWidth = w;
            guides.motionHeight = h;
            guides.motionScaleX = guides.motionScaleY = 1;
            result = scale.Compose(list, guides, result, DXGI_FORMAT_R16G16B16A16_FLOAT, frameId <= 1, outputOptions);
        }
        list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 3);
        list->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, tsRead, 0);
        Barrier(list, result, SRV, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const auto od = result->GetDesc();
        Require(od.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && od.Width >= ow && od.Height >= oh, "FP16 at the size");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT ofp {};
        device->GetCopyableFootprints(&od, 0, 1, 0, &ofp, nullptr, nullptr, nullptr);
        D3D12_TEXTURE_COPY_LOCATION d { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, s { result, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        d.PlacedFootprint = ofp;
        s.SubresourceIndex = 0;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        Barrier(list, result, D3D12_RESOURCE_STATE_COPY_SOURCE, SRV);
        Check(list->Close(), "close out");
        queue->ExecuteCommandLists(1, l);
        wait();
        if (!sharpen)
            Require(api.Retire(ctx, job.handle) == LMXXF_NR_OK, "Retire");
        {
            uint64_t* t = nullptr;
            Check(tsRead->Map(0, nullptr, reinterpret_cast<void**>(&t)), "map ts");
            scaleMs.push_back(double(t[1] - t[0]) * 1000.0 / double(tsFreq));
            composeMs.push_back(double(t[3] - t[2]) * 1000.0 / double(tsFreq));
            tsRead->Unmap(0, nullptr);
        }
        {
            uint8_t* m = nullptr;
            Check(readback->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map rb");
            FILE* o = std::fopen((outPrefix + name).c_str(), "wb");
            Require(o, "out file");
            for (UINT y = 0; y < oh; ++y)
                std::fwrite(m + ofp.Offset + size_t(y) * ofp.Footprint.RowPitch, 1, size_t(ow) * 8, o);
            std::fclose(o);
            readback->Unmap(0, nullptr);
        }
        frameMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    if (ctx)
    {
        api.GetStatus(ctx, err, sizeof err);
        std::printf("status: %s\n", err);
    }
    auto median = [](std::vector<double> v) {
        v.erase(v.begin(), v.begin() + std::min<size_t>(4, v.size() - 1));
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::printf("median ms: scale %.3f, composite %.3f\n", median(scaleMs), median(composeMs));
    if (ctx)
        api.Destroy(ctx);
    std::printf("ok\n");
    return 0;
}
