// What the host's order of calls costs around an lmxxf-ABI runtime (LmxxfNrRuntime.dll, Rdna2NrRuntime.dll), without
// a game: the same sequence LmxxfBackend and the list split give a frame. The game's list up to Evaluate is the producer
// (some GPU work of the game's, then PrepareFrame and RecordInputs), the rest of it the continuation (RecordOutputs at
// its start); at submission the producer runs, EnqueueHip is called between, then the continuation.
//
//   nr_host_cost.exe <runtime.dll> <assets dir> [--w 1706] [--h 960] [--frames 200] [--game-ms 0] [--temporal]
//                    [--late-outputs] [--host-list] [--reno] [--warm 40]
//
// Timestamps on the GPU: T0 producer start, T1 after the game's work (where the host's NR timer starts), T2 producer
// end, T3 continuation start, T4 after RecordOutputs (where the host's NR timer ends with composition off).
//   inputs  = T2 - T1   what RecordInputs recorded
//   between = T3 - T2   the network on HIP and the handoff both ways
//   outputs = T4 - T3   what RecordOutputs recorded
//   menu    = T4 - T1   the host's "NR time"
// --reno adds the host's RenoDX composition after the outputs (its defaults), timed as T5 - T4.
// --late-outputs records RecordOutputs after EnqueueHip, in a list of the host's own executed before the continuation
// (a host order the runtime ABI allows); --host-list keeps the host order but puts RecordOutputs in that list anyway.
// The inputs are a pattern, not a game frame: this times the calls, not the picture.
// Build (x64 developer prompt): cl /nologo /std:c++20 /EHsc /O2 /utf-8 /MT tests\nr_host_cost.cpp /Fe:nr_host_cost.exe
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include "../OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h"
#include "../OptiScaler/dlssnr/amd/RenoComposition.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static void Require(bool ok, const char* what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}
static void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        std::fprintf(stderr, "FAIL: %s hr=%08lx\n", what, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

static LmxxfNrApi g_api {};
static void RuntimeFail(int32_t rc, const char* what)
{
    if (rc == LMXXF_NR_OK)
        return;
    char err[512] {};
    g_api.GetLastError(err, sizeof err);
    std::fprintf(stderr, "FAIL: %s rc=%d err=%s\n", what, rc, err);
    std::exit(1);
}

// The game's work before Evaluate: a compute loop over a buffer, long enough to cover the asked time.
static const char* kBusy = R"(
RWStructuredBuffer<float> b : register(u0);
cbuffer C : register(b0) { uint iters; };
[numthreads(64,1,1)] void main(uint3 id : SV_DispatchThreadID) {
  float v = b[id.x];
  [loop] for (uint i = 0; i < iters; ++i) v = v * 1.0000001f + 0.0000001f;
  b[id.x] = v;
})";

// The colour and the vectors the frame hands the runtime: a gradient with some detail, and a small pan.
static const char* kFill = R"(
RWTexture2D<float4> c : register(u0);
RWTexture2D<float2> m : register(u1);
cbuffer C : register(b0) { uint frame; };
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
  float2 p = float2(id.xy) + float2(frame * 2, 0);
  float s = 0.5f + 0.5f * sin(p.x * 0.05f) * cos(p.y * 0.07f);
  c[id.xy] = float4(s * 0.8f, 0.3f + 0.4f * frac(p.x / 97.f), 0.6f - 0.3f * s, 1);
  m[id.xy] = float2(-2, 0);
})";

struct Shader
{
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
};

static Shader Compute(ID3D12Device* dev, const char* src, UINT uavs, bool table)
{
    ID3DBlob* cs = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err)))
    {
        std::fprintf(stderr, "%s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "compile");
        std::exit(1);
    }
    D3D12_DESCRIPTOR_RANGE range { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, uavs, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, 1 };
    if (table)
    {
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = { 1, &range };
    }
    else
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rd { 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr;
    Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr), "root sig");
    Shader s;
    Check(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s.root)), "root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
    pd.pRootSignature = s.root;
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    Check(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&s.pso)), "pso");
    return s;
}

static ID3D12Resource* Texture(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = f;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    Check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, state, nullptr, IID_PPV_ARGS(&r)), "texture");
    return r;
}

static ID3D12Resource* Buffer(ID3D12Device* dev, UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                              D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES hp { type };
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ID3D12Resource* r = nullptr;
    Check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)), "buffer");
    return r;
}

static double Median(std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
static double P90(std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, v.size() * 9 / 10)];
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: nr_host_cost.exe <runtime.dll> <assets dir> [--w W] [--h H] [--frames N] "
                             "[--game-ms X] [--temporal] [--late-outputs] [--host-list]\n");
        return 2;
    }
    UINT width = 1706, height = 960, frames = 200, warm = 40;
    double gameMs = 0;
    bool temporal = false, lateOutputs = false, hostList = false, reno = false;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? argv[++i] : "0"; };
        if (a == "--w")
            width = std::atoi(next());
        else if (a == "--h")
            height = std::atoi(next());
        else if (a == "--frames")
            frames = std::atoi(next());
        else if (a == "--warm")
            warm = std::atoi(next());
        else if (a == "--game-ms")
            gameMs = std::atof(next());
        else if (a == "--temporal")
            temporal = true;
        else if (a == "--late-outputs")
            lateOutputs = true;
        else if (a == "--host-list")
            hostList = true;
        else if (a == "--reno")
            reno = true;
    }
    const bool ownOutputList = lateOutputs || hostList;

    const std::wstring dllPath(argv[1], argv[1] + std::strlen(argv[1]));
    const std::wstring assets(argv[2], argv[2] + std::strlen(argv[2]));
    HMODULE dll = LoadLibraryW(dllPath.c_str());
    Require(dll != nullptr, "LoadLibraryW");
    auto getApi = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi*)>(GetProcAddress(dll, "LmxxfNrGetApi"));
    Require(getApi != nullptr, "LmxxfNrGetApi");
    g_api.struct_size = sizeof g_api;
    RuntimeFail(getApi(LMXXF_NR_ABI_VERSION, &g_api), "GetApi");

    IDXGIFactory6* factory = nullptr;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    IDXGIAdapter1* adapter = nullptr;
    Check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)), "adapter");
    DXGI_ADAPTER_DESC1 ad {};
    adapter->GetDesc1(&ad);
    ID3D12Device* dev = nullptr;
    Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)), "device");
    std::printf("adapter %ls\n", ad.Description);

    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue* queue = nullptr;
    Check(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    UINT64 freq = 0;
    queue->GetTimestampFrequency(&freq);
    ID3D12CommandAllocator* alloc[3] {};
    ID3D12GraphicsCommandList* list[3] {};
    for (int i = 0; i < 3; ++i)
    {
        Check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i])), "allocator");
        Check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i], nullptr, IID_PPV_ARGS(&list[i])), "list");
        list[i]->Close();
    }
    ID3D12Fence* fence = nullptr;
    Check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    UINT64 fenceValue = 0;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto drain = [&]
    {
        Check(queue->Signal(fence, ++fenceValue), "signal");
        Check(fence->SetEventOnCompletion(fenceValue, ev), "event");
        WaitForSingleObject(ev, 30000);
    };

    D3D12_QUERY_HEAP_DESC qh { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 8 };
    AmdPreSr::RenoComposition composition(dev);
    ID3D12QueryHeap* queries = nullptr;
    Check(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries)), "query heap");
    ID3D12Resource* readback = Buffer(dev, 8 * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    const D3D12_RESOURCE_STATES inputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* colour = Texture(dev, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, inputState);
    ID3D12Resource* motion = Texture(dev, width, height, DXGI_FORMAT_R16G16_FLOAT, inputState);
    const UINT busyCount = 64 * 1024;
    ID3D12Resource* busy = Buffer(dev, busyCount * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const Shader busyShader = Compute(dev, kBusy, 1, false);
    const Shader fillShader = Compute(dev, kFill, 2, true);
    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap* heap = nullptr;
    Check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "heap");
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateUnorderedAccessView(colour, nullptr, nullptr, cpu);
    cpu.ptr += inc;
    dev->CreateUnorderedAccessView(motion, nullptr, nullptr, cpu);

    auto transition = [](ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER br {};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
        l->ResourceBarrier(1, &br);
    };
    // The game's frame: fill the inputs, then the busy loop for the asked time.
    UINT busyIters = 0;
    auto gameWork = [&](ID3D12GraphicsCommandList* l, UINT frame)
    {
        ID3D12DescriptorHeap* heaps[] = { heap };
        l->SetDescriptorHeaps(1, heaps);
        transition(l, colour, inputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(l, motion, inputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        l->SetComputeRootSignature(fillShader.root);
        l->SetPipelineState(fillShader.pso);
        l->SetComputeRoot32BitConstant(0, frame, 0);
        l->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
        l->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        transition(l, colour, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, inputState);
        transition(l, motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, inputState);
        if (busyIters)
        {
            l->SetComputeRootSignature(busyShader.root);
            l->SetPipelineState(busyShader.pso);
            l->SetComputeRoot32BitConstant(0, busyIters, 0);
            l->SetComputeRootUnorderedAccessView(1, busy->GetGPUVirtualAddress());
            l->Dispatch(busyCount / 64, 1, 1);
        }
    };
    auto stamps = [&](UINT count, std::vector<double>& out)
    {
        UINT64* t = nullptr;
        D3D12_RANGE r { 0, count * 8 };
        Check(readback->Map(0, &r, reinterpret_cast<void**>(&t)), "map");
        out.assign(count, 0);
        for (UINT i = 0; i < count; ++i)
            out[i] = double(t[i] - t[0]) * 1000.0 / double(freq);
        readback->Unmap(0, nullptr);
    };
    // Calibrate the busy loop: 4096 iterations, timed, scaled to the asked time.
    if (gameMs > 0)
    {
        busyIters = 4096;
        Check(alloc[0]->Reset(), "reset");
        Check(list[0]->Reset(alloc[0], nullptr), "reset");
        list[0]->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        gameWork(list[0], 0);
        list[0]->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list[0]->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback, 0);
        list[0]->Close();
        ID3D12CommandList* l0[] = { list[0] };
        for (int k = 0; k < 3; ++k)
        {
            queue->ExecuteCommandLists(1, l0);
            drain();
        }
        std::vector<double> t;
        stamps(2, t);
        busyIters = std::max<UINT>(1, UINT(4096.0 * gameMs / std::max(t[1], 0.01)));
    }

    LmxxfNrCreateInfo ci {};
    ci.struct_size = sizeof ci;
    ci.device = dev;
    ci.queue = queue;
    ci.assets_directory = assets.c_str();
    ci.flags = LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK;
    void* ctx = nullptr;
    RuntimeFail(g_api.Create(&ci, &ctx), "Create");
    RuntimeFail(g_api.PrepareSession(ctx), "PrepareSession");

    std::vector<double> renoMs, inputs, between, outputs, menu, game, cpuPrepare, cpuInputs, cpuOutputs, cpuEnqueue, cpuFrame;
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    for (UINT frame = 0; frame < warm + frames; ++frame)
    {
        const auto f0 = Clock::now();
        for (int i = 0; i < 3; ++i)
            Check(alloc[i]->Reset(), "alloc reset");
        Check(list[0]->Reset(alloc[0], nullptr), "producer reset");
        Check(list[1]->Reset(alloc[1], nullptr), "continuation reset");
        list[0]->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        gameWork(list[0], frame);
        list[0]->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 1);

        LmxxfNrFrameInfo fi {};
        fi.struct_size = uint32_t(offsetof(LmxxfNrFrameInfo, depth));
        fi.frame_id = frame + 1;
        fi.command_list = list[0];
        fi.color_width = width;
        fi.color_height = height;
        fi.color = colour;
        fi.color_state = inputState;
        fi.flags = LMXXF_NR_FRAME_FLAG_STRENGTH | LMXXF_NR_FRAME_FLAG_DEBUG_VIEW | LMXXF_NR_FRAME_FLAG_STYLE;
        fi.transfer_strength = fi.color_strength = fi.model_scale = 1.f;
        fi.passes = 1;
        if (temporal)
        {
            fi.flags |= LMXXF_NR_FRAME_FLAG_TEMPORAL;
            fi.motion = motion;
            fi.motion_state = inputState;
            fi.motion_scale_x = fi.motion_scale_y = 1.f;
        }
        LmxxfNrJob job {};
        job.struct_size = uint32_t(offsetof(LmxxfNrJob, first_pass_output));
        auto c0 = Clock::now();
        RuntimeFail(g_api.PrepareFrame(ctx, &fi, &job), "PrepareFrame");
        auto c1 = Clock::now();
        RuntimeFail(g_api.RecordInputs(ctx, job.handle, list[0]), "RecordInputs");
        auto c2 = Clock::now();
        list[0]->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 2);
        Check(list[0]->Close(), "producer close");

        // The continuation: the game's rest of the frame starts with the outputs (host order), or with nothing when
        // they go in a list of their own.
        ID3D12GraphicsCommandList* out = ownOutputList ? list[2] : list[1];
        double cpuOut = 0;
        auto recordOutputs = [&]
        {
            if (ownOutputList)
                Check(list[2]->Reset(alloc[2], nullptr), "outputs reset");
            out->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 3);
            const auto o0 = Clock::now();
            RuntimeFail(g_api.RecordOutputs(ctx, job.handle, out), "RecordOutputs");
            cpuOut = ms(o0, Clock::now());
            out->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 4);
            if (reno)
            {
                AmdPreSr::Frame f {};
                f.colour = colour;
                f.width = width;
                f.height = height;
                composition.Record(out, f, static_cast<ID3D12Resource*>(job.private_output), 1u, frame == 0, 1.f / 60.f,
                                   1.f, 2.f, 1.f, 1.f, false);
            }
            out->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 6);
            if (ownOutputList)
                Check(list[2]->Close(), "outputs close");
        };
        if (!lateOutputs)
            recordOutputs();
        list[1]->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 5);
        list[1]->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 7, readback, 0);
        Check(list[1]->Close(), "continuation close");

        ID3D12CommandList* producer[] = { list[0] };
        ID3D12CommandList* continuation[] = { list[1] };
        ID3D12CommandList* own[] = { list[2] };
        queue->ExecuteCommandLists(1, producer);
        const auto e0 = Clock::now();
        RuntimeFail(g_api.EnqueueHip(ctx, job.handle, queue), "EnqueueHip");
        const auto e1 = Clock::now();
        if (lateOutputs)
            recordOutputs();
        if (ownOutputList)
            queue->ExecuteCommandLists(1, own);
        queue->ExecuteCommandLists(1, continuation);
        drain();
        RuntimeFail(g_api.Retire(ctx, job.handle), "Retire");
        const auto f1 = Clock::now();
        if (frame < warm)
            continue;
        std::vector<double> t;
        stamps(7, t);
        renoMs.push_back(t[6] - t[4]);
        game.push_back(t[1] - t[0]);
        inputs.push_back(t[2] - t[1]);
        between.push_back(t[3] - t[2]);
        outputs.push_back(t[4] - t[3]);
        menu.push_back(t[4] - t[1]);
        cpuPrepare.push_back(ms(c0, c1));
        cpuInputs.push_back(ms(c1, c2));
        cpuOutputs.push_back(cpuOut);
        cpuEnqueue.push_back(ms(e0, e1));
        cpuFrame.push_back(ms(f0, f1));
    }
    std::printf("%ux%u frames=%u game-ms=%.2f temporal=%d order=%s\n", width, height, frames, gameMs, temporal,
                lateOutputs ? "late-outputs" : hostList ? "host+own-list" : "host");
    auto row = [](const char* name, const std::vector<double>& v)
    { std::printf("  %-28s median %7.3f ms   p90 %7.3f ms\n", name, Median(v), P90(v)); };
    row("GPU game work", game);
    row("GPU inputs (T2-T1)", inputs);
    row("GPU between (T3-T2)", between);
    row("GPU outputs (T4-T3)", outputs);
    row("GPU menu NR time (T4-T1)", menu);
    if (reno)
        row("GPU RenoDX composition", renoMs);
    row("CPU PrepareFrame", cpuPrepare);
    row("CPU RecordInputs", cpuInputs);
    row("CPU RecordOutputs", cpuOutputs);
    row("CPU EnqueueHip", cpuEnqueue);
    row("CPU whole frame", cpuFrame);
    char status[512] {};
    g_api.GetStatus(ctx, status, sizeof status);
    std::printf("status: %s\n", status);
    RuntimeFail(g_api.Destroy(ctx), "Destroy");
    return 0;
}
