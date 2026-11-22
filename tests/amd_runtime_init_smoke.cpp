// Drives a real danielblnc runtime the way AmdPreSr does, without a game: SHA identification,
// bootstrap isolation on the real DllMain, HIP device selection by LUID, the host's field writes,
// init(engine, weights), then frames through Record -> ExecuteCommandLists -> Notify -> fence in
// both wait modes (Dispatch spin, then the 1-pixel draw where the layout maps SpinDraw), and
// shutdown. Needs an AMD GPU with HIP 7, the runtime and dlssnr_on_amd_weights.bin; see
// tools/test-amd-runtime-init.cmd. The inputs are blank textures: this checks the contract and
// times the network, not the picture.
//
//   amd_runtime_init_smoke.exe <version.dll> <dlssnr_on_amd_weights.bin> <work folder> [frames per mode]
//
// The runtime is copied into the work folder as dlssnr_amd_pass1.dll, so what its DllMain reads or
// writes beside itself (dlssnr_on_amd.ini, dlssnr_on_amd.log) stays there.
#include "../OptiScaler/dlssnr/amd/RuntimeHostLoad.h"
#include "../OptiScaler/dlssnr/amd/HipRuntimeLoad.h"
#include "../OptiScaler/dlssnr/amd/RuntimeNotification.h"
#include <bcrypt.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace AmdPreSr;

namespace
{
// Mirrors AmdPreSr.cpp's Packet; its static_asserts pin the same offsets.
struct Packet
{
    ID3D12GraphicsCommandList* list;
    ID3D12Resource* colour;
    UINT colourState, pad14;
    ID3D12Resource* motion;
    UINT motionState, pad24;
    ID3D12Resource* depth;
    UINT depthState, pad34;
    ID3D12Resource* exposure;
    UINT exposureState;
    float scaleX, scaleY;
    uint8_t nativePre;
    uint8_t pad4d[3];
    UINT renderWidth, renderHeight;
    float jitterX, jitterY;
    // Read by danielblnc 0.6.0 (OptiScaler/dlssnr/amd/AmdPreSr.cpp).
    float preExposure = 1.0f;
    UINT pad64;
};
static_assert(sizeof(Packet) == 0x68 && offsetof(Packet, scaleX) == 0x44 && offsetof(Packet, nativePre) == 0x4c);
static_assert(offsetof(Packet, renderWidth) == 0x50 && offsetof(Packet, jitterY) == 0x5c);
static_assert(offsetof(Packet, preExposure) == 0x60);
// Mirrors AmdPreSr.cpp's PacketV2 (abi 2, 0.8.0).
struct ResourceV2
{
    ID3D12Resource* resource;
    UINT api, format, dxgi, state, width, height;
    UINT64 reserved;
};
struct PacketV2
{
    ID3D12GraphicsCommandList* list;
    UINT listApi, listTag;
    ResourceV2 colour, motion, depth, exposure;
    float scaleX, scaleY;
    uint8_t nativePre;
    uint8_t padb9[3];
    UINT renderWidth, renderHeight;
    float jitterX, jitterY;
    float preExposure = 1.0f;
};
static_assert(sizeof(ResourceV2) == 0x28 && sizeof(PacketV2) == 0xd0 && offsetof(PacketV2, nativePre) == 0xb8);
using InitFn = bool(__fastcall*)(void*, const std::string*);
using RecordFn = void(__fastcall*)(Packet*);
using RecordV2Fn = bool(__fastcall*)(PacketV2*);
using FormatEnumFn = UINT(__fastcall*)(DXGI_FORMAT);
using NotifyFn = void(__fastcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

template <class T> T& At(HMODULE h, std::uint32_t rva)
{
    return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(h) + rva);
}

void Expect(bool ok, const std::string& what)
{
    if (!ok)
        throw std::runtime_error(what);
}

const AmdLayout* Identify(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
    BCRYPT_ALG_HANDLE alg {};
    unsigned char digest[32] {};
    Expect(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "BCrypt SHA256");
    const auto hashed = BCryptHash(alg, nullptr, 0, data.data(), static_cast<ULONG>(data.size()), digest, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    Expect(hashed >= 0, "BCryptHash");
    for (auto layout : kAmdLayouts)
        if (data.size() == layout->size && std::memcmp(digest, layout->sha256.bytes, 32) == 0)
            return layout;
    return nullptr;
}

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT w, UINT h, DXGI_FORMAT format)
{
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> texture;
    // The host hands every input over in NON_PIXEL_SHADER_RESOURCE (packet state 4).
    Expect(SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&texture))),
           "CreateCommittedResource");
    return texture;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        Expect(argc == 4 || argc == 5,
               "usage: amd_runtime_init_smoke <version.dll> <weights.bin> <work folder> [frames per mode]");
        const int framesPerMode = argc == 5 ? _wtoi(argv[4]) : 24;
        const std::filesystem::path work = std::filesystem::absolute(argv[3]);
        std::filesystem::create_directories(work);
        auto pass = work / L"dlssnr_amd_pass1.dll";
        const std::filesystem::path weights = std::filesystem::absolute(argv[2]);
        Expect(std::filesystem::exists(weights), "weights file missing");
        std::filesystem::copy_file(argv[1], pass, std::filesystem::copy_options::overwrite_existing);

        const AmdLayout* L = Identify(pass);
        Expect(L != nullptr, "runtime SHA/size is not in kAmdLayouts");
        std::printf("runtime %s identified by SHA\n", L->name);
        const bool host = L->abi >= 2;
        if (host)
        {
            // As AmdPreSr's PrepareHostModule: its own folder, its name, its settings with every add-on idle.
            const auto folder = work / L"dlssnr_amd_host";
            std::filesystem::create_directories(folder);
            const auto module = folder / L"neuralhub_host.dll";
            std::filesystem::copy_file(pass, module, std::filesystem::copy_options::overwrite_existing);
            std::ofstream(folder / L"neuralhub.ini", std::ios::binary | std::ios::trunc)
                << "[Upscaling]\r\nProvider=Passthrough\r\n[RayReconstruction]\r\nProvider=Passthrough\r\n"
                   "[Enhancement]\r\nProvider=Off\r\n[FrameGeneration]\r\nProvider=Off\r\n[Compute]\r\nBackend=HIP\r\n"
                   "[Compatibility]\r\nGpuSpoof=Off\r\nIgnoreHagsForFrameGeneration=Off\r\n";
            pass = module;
        }

        ComPtr<IDXGIFactory6> factory;
        Expect(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))), "CreateDXGIFactory2");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
             ++i)
        {
            DXGI_ADAPTER_DESC1 desc {};
            adapter->GetDesc1(&desc);
            if (desc.VendorId == 0x1002 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            {
                std::wprintf(L"adapter %ls\n", desc.Description);
                break;
            }
            adapter.Reset();
        }
        Expect(adapter != nullptr, "no AMD hardware adapter");
        ComPtr<ID3D12Device> device;
        Expect(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))),
               "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
        ComPtr<ID3D12CommandQueue> queue;
        Expect(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))), "CreateCommandQueue");

        HipRuntimeLoad::WindowsApi api;
        const auto hip = HipRuntimeLoad::Initialize(api, device->GetAdapterLuid(),
                                                    [](const std::string& line) { std::printf("%s\n", line.c_str()); });

        // What the runtime's hooks would change, read again after the load.
        auto kernelBase = GetModuleHandleW(L"kernelbase.dll");
        const void* watched[] { GetProcAddress(kernelBase, "LoadLibraryExW"), GetProcAddress(kernelBase, "GetProcAddress"),
                                GetProcAddress(kernelBase, "GetModuleHandleW"),
                                (*reinterpret_cast<void* const* const*>(queue.Get()))[10] /* ExecuteCommandLists */ };
        unsigned char before[std::size(watched)][16];
        for (size_t i = 0; i < std::size(watched); ++i)
            std::memcpy(before[i], watched[i], 16);
        HMODULE h = host ? RuntimeHostLoad::LoadHost(pass.c_str(), L) : RuntimeHostLoad::Load(pass.c_str(), L);
        if (host)
        {
            int changed = 0;
            for (size_t i = 0; i < std::size(watched); ++i)
                changed += std::memcmp(before[i], watched[i], 16) != 0;
            std::printf("host mode: loaded as neuralhub_host.dll, process init run; %d of %zu watched entry points "
                        "changed (an overlay such as RTSS can account for one)\n",
                        changed, std::size(watched));
        }
        else
            std::printf("bootstrap isolated: one CreateThread suppressed on the real DllMain\n");

        // What DllMain left in the option block, before the host overwrites it.
        std::printf("after DllMain: enabled=%u inline=%u interop=%u spinDraw=%d scale=%g", At<uint8_t>(h, L->enabled),
                    At<uint8_t>(h, L->configuredInline), At<uint8_t>(h, L->interop),
                    L->spinDraw ? At<int>(h, L->spinDraw) : -1, L->scale ? At<float>(h, L->scale) : -1.f);
        if (L->style)
            std::printf(" style=%d toneCurve=%d toneLift=%g useGameExposure=%u", At<int>(h, L->style),
                        At<int>(h, L->toneCurve), At<float>(h, L->toneLift), At<uint8_t>(h, L->useGameExposure));
        std::printf(" initDone=%u nativeFailure=%u\n", At<uint8_t>(h, L->initDone), At<uint8_t>(h, L->nativeFailure));

        // The same writes as InitPass, in the same order.
        At<decltype(&AlreadySubmitted)>(h, L->trampoline) = AlreadySubmitted;
        At<ID3D12Device*>(h, L->device) = device.Get();
        device->AddRef();
        At<ID3D12CommandQueue*>(h, L->queue) = queue.Get();
        queue->AddRef();
        At<int>(h, L->hipOrdinal) = hip.device;
        At<uint8_t>(h, L->configuredInline) = 1;
        At<uint8_t>(h, L->interop) = 1;
        At<uint8_t>(h, L->enabled) = 1;
        if (L->fsrInputs)
            At<uint8_t>(h, L->fsrInputs) = 1;
        At<uint8_t>(h, L->depthPresent) = 1;
        At<int>(h, L->tonemap) = -1;
        if (L->style)
            At<int>(h, L->style) = 0;
        if (L->toneCurve)
            At<int>(h, L->toneCurve) = 0;
        if (L->toneLift)
            At<float>(h, L->toneLift) = 0.f;
        if (L->useGameExposure)
            At<uint8_t>(h, L->useGameExposure) = 1;

        const std::string file = weights.string();
        Expect(hip.setDevice(hip.device) == 0, "hipSetDevice");
        if (L->initCallback)
            At<std::uintptr_t>(h, L->initCallback) = reinterpret_cast<std::uintptr_t>(h) + L->initCallbackFn;
        auto start = std::chrono::steady_clock::now();
        const bool ok = reinterpret_cast<InitFn>(reinterpret_cast<std::uintptr_t>(h) + L->init)(
            reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(h) + L->engine), &file);
        if (host)
            At<uint8_t>(h, L->configuredInline) = 1;
        std::printf("init(engine, weights) = %d in %lld ms; nativeFailure=%u\n", ok ? 1 : 0,
                    static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                            .count()),
                    At<uint8_t>(h, L->nativeFailure));
        Expect(ok, "runtime init returned false");
        Expect(At<uint8_t>(h, L->nativeFailure) == 0, "runtime reports a native failure after init");
        // Set before the first Record, as InitPass does: the runtime builds its 1-pixel-draw PSO with
        // staging, so the draw wait runs first and the live switch goes to the dispatch wait.
        if (L->spinDraw)
            At<int>(h, L->spinDraw) = 1;
        At<uint8_t>(h, L->initDone) = 1;
        Expect(At<ID3D12CommandList*>(h, L->pendingList) == nullptr && At<UINT>(h, L->jobId) == 0 &&
                   At<UINT>(h, L->jobDone) == 0 && At<UINT>(h, L->timeoutCount) == 0,
               "fresh runtime state is not idle (pendingList/jobId/jobDone/timeoutCount)");

        // Frames. 1080p, the render resolution of a 4K output in quality mode.
        constexpr UINT w = 1920, hgt = 1080;
        auto colour = Texture(device.Get(), w, hgt, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto motion = Texture(device.Get(), w, hgt, DXGI_FORMAT_R16G16_FLOAT);
        auto depth = Texture(device.Get(), w, hgt, DXGI_FORMAT_R32_FLOAT);
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        Expect(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))),
               "CreateCommandAllocator");
        Expect(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                   IID_PPV_ARGS(&list))),
               "CreateCommandList");
        list->Close();
        ComPtr<ID3D12Fence> fence;
        Expect(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "CreateFence");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        UINT64 fenceValue = 0;
        const auto base = reinterpret_cast<std::uintptr_t>(h);
        const auto timeoutsBefore = At<UINT>(h, L->timeoutCount);
        bool failed = false;
        const int modes[] = { L->spinDraw ? 1 : 0, 0 };
        for (int step = 0; step < (L->spinDraw ? 2 : 1) && !failed; ++step)
        {
            const int mode = modes[step];
            int accepted = 0, completed = 0, measured = 0;
            double gpuMs = 0;
            for (int frame = 0; frame < framesPerMode; ++frame)
            {
                Expect(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "list reset");
                // Per-Record writes, as Backend::Record makes them.
                if (L->spinDraw)
                    At<int>(h, L->spinDraw) = mode;
                At<uint8_t>(h, L->temporal) = 1;
                if (frame == 0)
                {
                    At<uint8_t>(h, L->historyValid) = 0;
                    At<void*>(h, L->historyView) = nullptr;
                }
                At<UINT>(h, L->depthInverted) = 0;
                At<uint8_t>(h, L->explicitDepth) = 1;
                At<float>(h, L->tone) = 0;
                At<float>(h, L->structure) = 1;
                At<float>(h, L->skin) = 1;
                if (L->scale)
                    At<float>(h, L->scale) = 4.f / 128.f;
                At<UINT>(h, L->toneChannels) = 0;
                At<UINT>(h, L->charMask) = 1;
                // AMD_TEST_PASSES=2/3: the runtime's own multipass (0.8.0), written as Backend::Record writes it.
                if (L->passCount && frame == 0)
                {
                    wchar_t value[8] {};
                    const int passes = GetEnvironmentVariableW(L"AMD_TEST_PASSES", value, 8) ? std::clamp(_wtoi(value), 1, 3) : 1;
                    if (At<int>(h, L->passCount) != passes)
                    {
                        At<int>(h, L->passCount) = passes;
                        At<uint8_t>(h, L->recreate) = 1;
                    }
                    for (UINT k = 0; k < 3; ++k)
                    {
                        At<int>(h, L->passStyle + 4 * k) = 0;
                        At<float>(h, L->passStructure + 4 * k) = 1;
                        At<float>(h, L->passSkin + 4 * k) = 1;
                        At<float>(h, L->passTone + 4 * k) = 0;
                    }
                    if (step == 0)
                        std::printf("runtime passes: %d\n", passes);
                }
                At<UINT>(h, L->watchdog) =
                    static_cast<UINT>(std::clamp<UINT64>(262144 + (UINT64(w) * hgt + 1) / 2, 262144, 2097152));
                Packet packet {};
                packet.list = list.Get();
                packet.colour = colour.Get();
                packet.colourState = 4;
                packet.motion = motion.Get();
                packet.motionState = 4;
                packet.depth = depth.Get();
                packet.depthState = 4;
                packet.exposureState = 4;
                packet.scaleX = packet.scaleY = 1;
                packet.renderWidth = w;
                packet.renderHeight = hgt;
                if (host)
                {
                    const auto format = reinterpret_cast<FormatEnumFn>(base + L->formatEnum);
                    auto describe = [&](ID3D12Resource* r) -> ResourceV2
                    {
                        if (!r)
                            return {};
                        const auto d = r->GetDesc();
                        return { r,      1, format(d.Format), UINT(d.Format), UINT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                 UINT(d.Width), d.Height, 0 };
                    };
                    PacketV2 v2 {};
                    v2.list = list.Get();
                    v2.listApi = 1;
                    v2.listTag = 3;
                    v2.colour = describe(packet.colour);
                    v2.motion = describe(packet.motion);
                    v2.depth = describe(packet.depth);
                    v2.scaleX = v2.scaleY = 1;
                    v2.renderWidth = w;
                    v2.renderHeight = hgt;
                    reinterpret_cast<RecordV2Fn>(base + L->record)(&v2);
                }
                else
                    reinterpret_cast<RecordFn>(base + L->record)(&packet);
                const UINT job = At<UINT>(h, L->jobId);
                const bool recorded = At<ID3D12CommandList*>(h, L->pendingList) == list.Get();
                if (At<uint8_t>(h, L->nativeFailure))
                {
                    std::printf("  mode %d frame %d: native failure\n", mode, frame);
                    failed = true;
                    break;
                }
                Expect(SUCCEEDED(list->Close()), "list close");
                ID3D12CommandList* lists[] { list.Get() };
                const auto submitted = std::chrono::steady_clock::now();
                queue->ExecuteCommandLists(1, lists);
                if (recorded)
                {
                    ++accepted;
                    reinterpret_cast<NotifyFn>(base + L->notify)(queue.Get(), 1, lists);
                    Expect(At<ID3D12CommandList*>(h, L->pendingList) == nullptr,
                           "Notify did not consume the pending list");
                }
                queue->Signal(fence.Get(), ++fenceValue);
                fence->SetEventOnCompletion(fenceValue, event);
                Expect(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU did not finish within 10 s");
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submitted).count();
                if (recorded)
                {
                    // The worker publishes jobDone after its HIP synchronize, just after the GPU wait ends.
                    for (int i = 0; i < 200 && At<volatile UINT>(h, L->jobDone) < job; ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if (At<volatile UINT>(h, L->jobDone) >= job)
                        ++completed;
                    // The first frames build staging and warm the kernels.
                    if (frame >= framesPerMode / 2)
                    {
                        gpuMs += ms;
                        ++measured;
                    }
                }
                if (frame < 3 || !recorded)
                    std::printf("  mode %d frame %d: recorded=%d job=%u done=%u timeouts=%u recreate=%u %.2f ms\n",
                                mode, frame, recorded ? 1 : 0, job, At<UINT>(h, L->jobDone),
                                At<UINT>(h, L->timeoutCount), L->recreate ? At<uint8_t>(h, L->recreate) : 0, ms);
            }
            const bool pso = L->graphicsPso && At<void*>(h, L->graphicsPso) != nullptr;
            const int predicate = L->predicateReady ? At<uint8_t>(h, L->predicateReady) : -1;
            std::printf("mode %d (%s): %d/%d recorded, %d completed, timeouts %u, graphicsPso %d, predicateReady %d, "
                        "%.2f ms per frame (submit to fence, last %d)\n",
                        mode, mode ? "1-pixel draw wait" : "dispatch wait", accepted, framesPerMode, completed,
                        At<UINT>(h, L->timeoutCount) - timeoutsBefore, pso ? 1 : 0, predicate,
                        measured ? gpuMs / measured : 0.0, measured);
            failed |= accepted < framesPerMode - 3 || completed != accepted ||
                      At<UINT>(h, L->timeoutCount) != timeoutsBefore || (mode == 1 && !pso);
        }
        CloseHandle(event);
        Expect(!failed, "frames were refused, did not complete, timed out, or the draw wait had no PSO");

        hip.setDevice(hip.device);
        reinterpret_cast<void (*)()>(base + L->shutdown)();
        std::printf("shutdown returned\n");
        std::printf("PASS: runtime %s loads isolated, initialises, runs frames in every wait mode and shuts down\n",
                    L->name);
        // The runtime stays pinned, as in the host, so its DLL_PROCESS_DETACH runs at process exit
        // the way it does when a game closes. A non-zero exit code after PASS points there.
        std::fflush(stdout);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
