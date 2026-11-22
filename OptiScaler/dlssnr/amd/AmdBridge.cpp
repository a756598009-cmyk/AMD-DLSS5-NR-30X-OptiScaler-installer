#include "pch.h"
#include "AmdBridge.h"
#include "AwaitingListTracker.h"
#include "../submission/SubmissionTls.h"
#include "AmdPreSr.h"
#include "StallWatch.h"
#include "RenoComposition.h"
#include "Foveation.h"
#include "ResidualStabilizer.h"
#include "DynamicScale.h"
#include "PresentExperimental.h"
#include "../person/PersonPartition.h"
#include "../backend/DanielBackend.h"
#include "../backend/LmxxfBackend.h"
#include "../backend/Selector.h"
#include "../backend/LmxxfEvaluateCut.h"
#include "../backend/LmxxfGenerationObserver.h"
#include "../submission/SubmissionHooks.h"
#include <State.h>
#include <Util.h>
#include <misc/SkipSpoof.h>
#include <misc/IdentifyGpu.h>
#include <with_dx12/with_dx12.h>
#include <gpu_time/GpuTime_Dx12.h>
#include <detours/detours.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>

namespace DlssNr::AmdBridge
{
namespace
{
// Why NR was switched off by the stall watch, until it is switched on again.
std::mutex standDownMutex;
std::string standDown;
std::atomic<DlssNr::Backend::Host*> backend { nullptr };
// danielblnc 0.8.0's host loaded only as the NGX DLSS-SR/DLSS-RR provider, when NrBackend is another runtime (lmxxf,
// mochizuki) or off. It never records NR; the bridge only forwards the host's NGX lists to it. Process lifetime.
std::atomic<AmdPreSr::Backend*> ngxHostOnly { nullptr };
std::atomic<bool> ngxHostDeferred { false };
// COM identity and adapter of the device the backend was built on (guarded by initMutex). The backend lives for
// the process, so this reference is never released either.
IUnknown* backendDevice = nullptr;
LUID backendAdapter {};
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ExitFn = void(NTAPI*)(LONG);
ExecuteFn executeOriginal = nullptr;
ExitFn exitOriginal = nullptr;
bool submissionHookReady = false; // guarded by initMutex
std::string message = "AMD pre-SR: waiting for an upscaler frame";
std::mutex messageMutex;
std::mutex initMutex;
std::mutex observedMutex;
std::unordered_set<ID3D12CommandList*> observedLists;
void Message(const char* s)
{
    std::lock_guard l(messageMutex);
    if (*s && message != s)
    {
        std::ofstream log(Util::DllPath().parent_path() / L"amd_bridge.log", std::ios::app);
        log << GetTickCount64() << " thread=" << GetCurrentThreadId() << " " << s << '\n';
    }
    message = s;
}
thread_local NVSDK_NGX_Parameter* replacedParams = nullptr;
thread_local ID3D12Resource* originalColour = nullptr;
struct FrameIdentity
{
    ID3D12Resource* colour = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* depth = nullptr;
    UINT width = 0, height = 0;
};
std::mutex frameMutex;
FrameIdentity lastFrame {};
UINT stableFrames = 0;
uint64_t runSerial = 0;

// Dynamic NR resolution. The controller is stepped under frameMutex; the menu reads the atomics,
// which stay zero while it is off.
AmdPreSr::DynamicScale dynamicScale;
std::atomic<float> dynamicScaleNow { 0 }, dynamicFpsNow { 0 };
std::atomic<int> dynamicChangesNow { 0 };

// GPU time of everything Record puts on the game's list: the input copies, the wait for the model
// and the applied result, every pass together. Used under frameMutex; the menu reads neuralMsNow.
std::unique_ptr<GpuTime_Dx12> neuralTimer;
std::atomic<float> neuralMsNow { 0 };
// The last reading as it was, and when, for the FPS overlay's line and graph; and the runtime the
// backend was built for, named once (the overlay draws every frame).
std::atomic<float> neuralMsLast { 0 };
std::atomic<UINT64> neuralAt { 0 };
std::mutex runtimeLabelMutex;
std::string runtimeLabel;

// The residual stabilizer between the runtime and SR (pre-SR, danielblnc and mochizuki), made with the backend; the
// last Run it filtered and when. Used under frameMutex, except Invalidate.
std::unique_ptr<AmdPreSr::ResidualStabilizer> stabilizer;
uint64_t lastStabilizedSerial = 0;
ULONGLONG lastStabilizedTick = 0;
bool stabilizerFailed = false;
// RenoDX's composition after the runtime and the stabilizer (pre-SR, danielblnc and lmxxf; mochizuki runs inside
// upstream's RenoDX stage in its runtime), made with the backend; the last Run it composed and when, for its
// divisor's rate limits. Used under frameMutex.
std::unique_ptr<AmdPreSr::RenoComposition> composition;
uint64_t lastComposedSerial = 0;
ULONGLONG lastComposedTick = 0;
bool compositionFailed = false;
// Person protection (NrPersonPartition) between the runtime and the stabilizer, lmxxf and mochizuki before SR, made
// with the backend; the last Run it composed and when. Record and Idle under frameMutex; Submitted on the submitting
// thread.
std::unique_ptr<DlssNr::Person::Partition> person;
uint64_t lastPersonSerial = 0;
ULONGLONG lastPersonTick = 0;
// Foveated NR before SR ([DlssNr] Foveation, Foveation.h), mochizuki: the runtime gets a warped copy of the render
// frame and its change goes back on the render grid before the stabilizer and SR. The grid at the fixed extent (the
// colour allocation under dynamic resolution) decides the textures, the log and the history; the jitter step is
// folded into the warped vectors with MochizukiJitterHistory. Made with the backend, used under frameMutex.
std::unique_ptr<AmdPreSr::Foveation::Passes> foveation;
AmdPreSr::Foveation::Grid foveationGrid;
float foveationJitterX = 0, foveationJitterY = 0;
bool foveationJitter = false, foveationFailed = false;

thread_local uint64_t submitOrdinal = 0; // Monotonic per-thread submission counter.

class ConfirmedQueueHolder
{
    mutable std::mutex mu_;
    ID3D12CommandQueue* queue_ = nullptr;

  public:
    ~ConfirmedQueueHolder() { Clear(); }
    void Clear()
    {
        std::lock_guard lock(mu_);
        if (queue_)
        {
            queue_->Release();
            queue_ = nullptr;
        }
    }
    void Set(ID3D12CommandQueue* q)
    {
        std::lock_guard lock(mu_);
        if (queue_ == q)
            return;
        if (q)
            q->AddRef();
        if (queue_)
            queue_->Release();
        queue_ = q;
    }
    ID3D12CommandQueue* Get() const
    {
        std::lock_guard lock(mu_);
        if (queue_)
            queue_->AddRef();
        return queue_; // Caller must Release()
    }
    bool Matches(ID3D12CommandQueue* q) const
    {
        std::lock_guard lock(mu_);
        if (!queue_ || !q)
            return queue_ == q;
        if (queue_ == q)
            return true;
        IUnknown* id1 = nullptr;
        IUnknown* id2 = nullptr;
        queue_->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&id1));
        q->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&id2));
        const bool same = (id1 && id2 && id1 == id2);
        if (id1)
            id1->Release();
        if (id2)
            id2->Release();
        return same;
    }
};

static ConfirmedQueueHolder s_confirmedRenderQueue;
static AwaitingListTracker s_awaitingTracker;

void SetConfirmedRenderQueueInternal(ID3D12CommandQueue* q) { s_confirmedRenderQueue.Set(q); }

void ExecuteBatch(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* c)
{
    if (q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        ID3D12GraphicsCommandList* matched = s_awaitingTracker.MatchAndRemove(n, c);
        if (matched)
        {
            s_confirmedRenderQueue.Set(q);
            LOG_INFO("AMD pre-SR: confirmed execution queue {:p} for target list {:p}", reinterpret_cast<void*>(q),
                     reinterpret_cast<void*>(matched));
        }
    }
    auto b = backend.load();
    if (b)
        b->Submitting(q, n, c);
    // Execute every game list exactly once. Private runtime Notify callbacks
    // publish HIP jobs afterwards and have their internal ECL call neutralized.
    // When lmxxf submission expand is armed, unwrap CommandListProxy (between = HIP slot).
    // Expand runs for every ExecuteBatch when ExpandEnabled ? independent of PendingListIndex
    // (Daniel-only isolation of a private neural list).
    if (DlssNr::Submission::Hooks::ExpandEnabled())
    {
        // Past the exit hook nothing is enqueued between a list's halves; both still run, in order.
        const auto between =
            Exiting() ? DlssNr::Submission::Hooks::BetweenState {} : DlssNr::Submission::Hooks::GetBetween();
        DlssNr::Submission::Hooks::ExecuteExpanded(q, n, c, between.fn, between.ctx, executeOriginal);
    }
    else
        executeOriginal(q, n, c);
    // Published before the backend: a capture person protection recorded on one of these lists reads back after this.
    if (b && person)
        person->Submitted(q, n, c);
    {
        std::lock_guard guard(observedMutex);
        if (observedLists.size() > 256)
            observedLists.clear();
        for (UINT i = 0; i < n; ++i)
            observedLists.insert(c[i]);
    }
    if (b)
        b->Submitted(q, n, c);
    else
        AmdPreSr::NgxSubmitted(q, n, c);
    // Another runtime's backend does not know the NGX host's lists: notify them here.
    if (b && ngxHostOnly.load(std::memory_order_acquire))
        AmdPreSr::NgxSubmitted(q, n, c);
}
void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* c)
{
    auto b = backend.load();
    int index = b ? b->PendingListIndex(n, c) : AmdPreSr::NgxPendingListIndex(n, c);
    if (index < 0 && b && ngxHostOnly.load(std::memory_order_acquire))
        index = AmdPreSr::NgxPendingListIndex(n, c);
    if (n > 1 && index >= 0)
    {
        // Separate Execute calls establish an execution boundary around the
        // interop list. Preserve list order and execute each list exactly once.
        // Once: Run clears the message every frame, so a note set every frame was a line each frame (29 MB of
        // amd_bridge.log in one Cyberpunk report).
        static std::atomic<bool> noted { false };
        if (!noted.exchange(true))
            Message("AMD isolated neural command list from a render batch");
        if (index)
            ExecuteBatch(q, static_cast<UINT>(index), c);
        ExecuteBatch(q, 1, c + index);
        auto remaining = n - static_cast<UINT>(index) - 1;
        if (remaining)
            ExecuteBatch(q, remaining, c + index + 1);
        return;
    }
    ExecuteBatch(q, n, c);
}
void NTAPI Exit(LONG code)
{
    // The game's other threads still run until exitOriginal terminates them. From here on Run passes the
    // original colour through and nothing new enters the runtime; the backend decides what it can tear down.
    DlssNr::Backend::LmxxfCut::g_exiting.store(true, std::memory_order_release);
    s_confirmedRenderQueue.Clear();
    s_awaitingTracker.Clear();
    if (auto b = backend.load())
        b->OnProcessExit();
    if (auto h = ngxHostOnly.load(std::memory_order_acquire))
        h->Shutdown();
    exitOriginal(code);
}
// Caller holds initMutex. Install once, before any proxy can escape into game submission.
bool InstallSubmissionHook(ID3D12Device* device, ID3D12CommandQueue* q)
{
    if (submissionHookReady)
        return true;
    executeOriginal = reinterpret_cast<ExecuteFn>((*reinterpret_cast<void***>(q))[10]);
    // FG can expose a proxy present queue. Hook the device's execution
    // implementation so actual render submissions are still observed.
    ID3D12CommandQueue* probe = nullptr;
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    if (SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&probe))))
    {
        executeOriginal = reinterpret_cast<ExecuteFn>((*reinterpret_cast<void***>(probe))[10]);
        probe->Release();
    }
    exitOriginal = reinterpret_cast<ExitFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlExitUserProcess"));
    LONG err = DetourTransactionBegin();
    if (err == NO_ERROR)
        err = DetourUpdateThread(GetCurrentThread());
    if (err == NO_ERROR)
        err = DetourAttach(reinterpret_cast<PVOID*>(&executeOriginal), Execute);
    if (err == NO_ERROR && exitOriginal)
        err = DetourAttach(reinterpret_cast<PVOID*>(&exitOriginal), Exit);
    if (err == NO_ERROR)
        err = DetourTransactionCommit();
    else
        DetourTransactionAbort();
    if (err != NO_ERROR)
    {
        Message("AMD pre-SR: could not install submission notification");
        return false;
    }
    DlssNr::Submission::NoteRawExecuteCommandLists(executeOriginal);
    submissionHookReady = true;
    return true;
}
std::filesystem::path Directory() { return Util::DllPath().parent_path(); }
ID3D12Resource* Resource(NVSDK_NGX_Parameter* p, const char* name)
{
    ID3D12Resource* r = nullptr;
    if (p->Get(name, &r) != NVSDK_NGX_Result_Success)
        p->Get(name, reinterpret_cast<void**>(&r));
    return r;
}
// Calls use with the physical adapter behind luid, past the vendor spoof.
template <class Use> void WithPhysicalAdapter(LUID luid, Use&& use)
{
    struct PhysicalAdapterScope
    {
        uint64_t id = SkipSpoof::AddEntry(SkipSpoofType::Thread);
        ~PhysicalAdapterScope() { SkipSpoof::RemoveEntry(id); }
    } physicalAdapterScope;
    IDXGIFactory4* f = nullptr;
    IDXGIAdapter1* a = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))
    {
        if (SUCCEEDED(f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&a))))
        {
            use(a);
            a->Release();
        }
        f->Release();
    }
}
bool IsAmd(ID3D12Device* d)
{
    bool amd = false;
    WithPhysicalAdapter(d->GetAdapterLuid(),
                        [&](IDXGIAdapter1* a)
                        {
                            DXGI_ADAPTER_DESC1 desc {};
                            amd = SUCCEEDED(a->GetDesc1(&desc)) && desc.VendorId == 0x1002;
                            LOG_INFO("AMD pre-SR physical adapter vendor: {:04X}, AMD: {}", desc.VendorId, amd);
                        });
    return amd;
}
// This process's share of the adapter's local memory. Logged at every resolution change, because
// the runtime keeps a little more after each rebuild and this is how much.
std::string VramUsage(LUID luid)
{
    std::string text = "vram unknown";
    WithPhysicalAdapter(luid,
                        [&](IDXGIAdapter1* a)
                        {
                            IDXGIAdapter3* a3 = nullptr;
                            DXGI_QUERY_VIDEO_MEMORY_INFO info {};
                            if (FAILED(a->QueryInterface(IID_PPV_ARGS(&a3))))
                                return;
                            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
                                text = "vram " + std::to_string(info.CurrentUsage >> 20) + "/" +
                                       std::to_string(info.Budget >> 20) + " MB";
                            a3->Release();
                        });
    return text;
}
// "14.4/12.5 GB" while this process uses more of the adapter's local memory than its budget, else empty.
std::string VramOverBudget(LUID luid)
{
    std::string text;
    WithPhysicalAdapter(luid,
                        [&](IDXGIAdapter1* a)
                        {
                            IDXGIAdapter3* a3 = nullptr;
                            DXGI_QUERY_VIDEO_MEMORY_INFO info {};
                            if (FAILED(a->QueryInterface(IID_PPV_ARGS(&a3))))
                                return;
                            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) &&
                                info.Budget && info.CurrentUsage > info.Budget)
                            {
                                char gb[48];
                                std::snprintf(gb, sizeof(gb), "%.1f/%.1f GB", info.CurrentUsage / 1073741824.0,
                                              info.Budget / 1073741824.0);
                                text = gb;
                            }
                            a3->Release();
                        });
    return text;
}
} // namespace
bool Exiting() { return DlssNr::Backend::LmxxfCut::g_exiting.load(std::memory_order_acquire); }
void UpdateConfirmedRenderQueue(ID3D12CommandQueue* q) { SetConfirmedRenderQueueInternal(q); }
bool EnsureSubmissionHook(ID3D12CommandQueue* q)
{
    if (!q)
        return false;
    std::lock_guard lock(initMutex);
    if (submissionHookReady)
        return true;
    ID3D12Device* device = nullptr;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    const bool ready = InstallSubmissionHook(device, q);
    device->Release();
    return ready;
}
AmdPreSr::Settings SettingsFromConfig(const Config& cfg, float modelScale)
{
    AmdPreSr::Settings s {};
    s.rtgi.enabled = cfg.AmdRtgiEnabled.value_or_default();
    s.rtgi.quality = cfg.AmdRtgiQuality.value_or_default();
    s.rtgi.denoiser = cfg.AmdRtgiDenoiser.value_or_default();
    s.rtgi.inspect = cfg.AmdRtgiInspect.value_or_default();
    s.rtgi.contact = cfg.AmdRtgiContact.value_or_default();
    s.rtgi.saturation = cfg.AmdRtgiSaturation.value_or_default();
    s.rtgi.radius = cfg.AmdRtgiRadius.value_or_default();
    s.rtgi.mix = cfg.AmdRtgiMix.value_or_default();
    s.rtgi.lighting = cfg.AmdRtgiLighting.value_or_default();
    s.rtgi.occlusion = cfg.AmdRtgiOcclusion.value_or_default();
    s.rtgi.ambient = cfg.AmdRtgiAmbient.value_or_default();
    s.rtgi.thickness = cfg.AmdRtgiThickness.value_or_default();
    s.rtgi.smoothness = cfg.AmdRtgiSmoothness.value_or_default();
    s.rtgi.fade = cfg.AmdRtgiFade.value_or_default();
    s.rtgi.fov = cfg.AmdRtgiFov.value_or_default();
    s.rtgi.farPlane = cfg.AmdRtgiFarPlane.value_or_default();
    s.look.enabled = cfg.AmdLookEnabled.value_or_default();
    s.look.appearance = 2; // Single default profile; ignore legacy preset selections.
    s.look.mix = cfg.AmdLookMix.value_or_default();
    s.look.materialDetail = cfg.AmdLookMaterialDetail.value_or_default();
    s.look.shapeDefinition = cfg.AmdLookShapeDefinition.value_or_default();
    s.look.localLighting = cfg.AmdLookLocalLighting.value_or_default();
    s.look.skinDetail = cfg.AmdLookSkinDetail.value_or_default();
    s.look.skinSoftness = cfg.AmdLookSkinSoftness.value_or_default();
    s.look.detectSkin = cfg.AmdLookDetectSkin.value_or_default();
    s.look.specularControl = cfg.AmdLookSpecularControl.value_or_default();
    s.look.highlightRollOff = cfg.AmdLookHighlightRollOff.value_or_default();
    s.look.colourSeparation = cfg.AmdLookColourSeparation.value_or_default();
    s.look.shadowDepth = cfg.AmdLookShadowDepth.value_or_default();
    s.look.antiHalo = cfg.AmdLookAntiHalo.value_or_default();
    s.look.flatAreaProtection = cfg.AmdLookFlatAreaProtection.value_or_default();
    s.look.inspect = cfg.AmdLookInspect.value_or_default();
    s.look.tone = cfg.AmdLookTone.value_or_default();
    s.look.exposureEV = cfg.AmdLookExposureEV.value_or_default();
    s.look.contrast = cfg.AmdLookContrast.value_or_default();
    s.look.saturation = cfg.AmdLookSaturation.value_or_default();
    s.look.highlightCompression = cfg.AmdLookHighlightCompression.value_or_default();
    s.modelScale = modelScale;
    // One pass chain whatever the placement. The AMD backend runs a separate runtime module per
    // pass (dlssnr_amd_pass1-3.dll), not an NGX feature chain, and a game only ever reaches one of
    // the two placements anyway -- a title driving Ray Reconstruction is always post-upscale.
    // Reading the after-RR count here left the Passes control doing nothing in exactly that case.
    s.passes = cfg.DlssNrPasses.value_or_default();
    s.everyFrame = cfg.AmdEveryFrame.value_or_default();
    s.slots = std::clamp(cfg.AmdSlots.value_or_default(), 1, 5);
    // AmdGraphicsWait=1 requests the 1-pixel draw wait of 0.3.1 and later (this project's New wait).
    // InitPass/Record still force SpinDraw=0 unless a freeze+restore plan armed.
    s.spinDraw = cfg.AmdGraphicsWait.value_or_default() ? 1 : 0;
    // The pinned AMD binary explicitly disables the broad lighting/colour
    // channels. Its embedded UI warns that nonzero tone mostly darkens frames.
    // An old INI's 0 (the former Auto, which converted nothing) reads as Linear, the default.
    const int encoding = cfg.AmdEncoding.value_or_default();
    s.encoding = encoding == 0 ? 1 : std::clamp(encoding, 1, 3);
    // tone must be 0 whenever toneChannels is: 0.3.1 zeroed it itself, 0.4.x passes it through.
    s.toneChannels = cfg.AmdNeuralLightingStrength.value_or_default() > 0;
    s.tone = s.toneChannels ? std::clamp(cfg.AmdNeuralLightingStrength.value_or_default(), 0.f, 1.f) : 0.f;
    s.structure = cfg.DlssNrLocalStructure.value_or_default();
    s.skin = cfg.DlssNrSkinStructure.value_or_default();
    if (s.skin < 0)
        s.skin = s.structure;
    // One Effect strength on danielblnc: with the RenoDX composition the runtime keeps its default and the strength is
    // the composition's intensity, which moves without restarting the model's history.
    s.strength = cfg.DlssNrRenoComposition.value_or_default()
                     ? 1.f
                     : std::clamp(cfg.AmdEffectStrength.value_or_default(), 0.f, 2.f);
    s.fast = cfg.AmdQuality.value_or_default() == 0;
    s.toneCurve = UINT(std::clamp(cfg.AmdToneCurve.value_or_default(), 0, 1));
    s.toneLift = std::clamp(cfg.AmdToneLift.value_or_default(), 0.f, .25f);
    s.gameExposure = cfg.AmdUseGameExposure.value_or_default();
    s.style = UINT(std::clamp(cfg.AmdStyle.value_or_default(), 0, 2));
    return s;
}

std::filesystem::path LibraryFolder()
{
    // Before DllMain resolves OptiDllPath (the INI's first read) the default stands in for it.
    if (Config::Instance()->MainDllPath.has_value())
    {
        std::filesystem::path folder(Config::Instance()->MainDllPath.value());
        return folder.is_relative() ? Directory() / folder : folder;
    }
    return Directory() / L"OptiScaler";
}
bool HostKeptOnHip(const std::string& backend) { return _stricmp(backend.c_str(), "HIP") == 0; }
bool HostKeptOnHip() { return HostKeptOnHip(Config::Instance()->HostComputeBackend.value_or_default()); }
bool NeuralHubFgInstalled(const std::filesystem::path& library, const std::string& backend, bool dlssgOutput,
                          bool otherFgOutput, bool danielNr)
{
    // Another FG picked as the output (XeFG, FSR FG): NeuralHub's DLSS-FG would take the game's frames from it
    if (HostKeptOnHip(backend) || otherFgOutput)
        return false;
    // With danielblnc's NR the game's own nvngx_dlssg.dll does not count: the FG would take its NR away.
    if (danielNr && !dlssgOutput && _stricmp(backend.c_str(), "D3D12") != 0)
        return false;
    std::error_code ec;
    const auto dir = Directory();
    const auto host = dir / L"dlssnr_amd_host";
    // dlssnr_amd_host\ is made when the host first loads: before that, danielblnc 0.8.0 beside OptiScaler is the host
    const char* build = std::filesystem::exists(host / L"neuralhub_host.dll", ec) ? "0.8.0" : RuntimeName();
    return build && std::strcmp(build, "0.8.0") == 0 &&
           (std::filesystem::exists(host / L"nvngx_dlssg.dll", ec) ||
            std::filesystem::exists(library / L"nvngx_dlssg.dll", ec) ||
            std::filesystem::exists(dir / L"nvngx_dlssg.dll", ec));
}
bool HostD3d12Supported(const LUID* luid)
{
    auto gen = IdentifyGpu::getPrimaryGpu().amdHwGeneration;
    if (luid)
        for (const auto& gpu : IdentifyGpu::getAllGpus())
            if (IsEqualLUID(gpu.luid, *luid))
                gen = gpu.amdHwGeneration;
    return gen == device_info::HwGeneration::kUndefinedGeneration || gen >= device_info::HwGeneration::kGfx11;
}
bool NeuralHubFgFilesInstalled()
{
    // The INI's own FGOutput, read once: a volatile override or a menu change mid-game does not move the host.
    static const auto output = Config::Instance()->FGOutput.value_for_config_ignore_default();
    static const bool dlssgOutput = output == FGOutput::DLSSG;
    static const bool otherFgOutput = output.has_value() && *output != FGOutput::DLSSG && *output != FGOutput::NoFG;
    return NeuralHubFgInstalled(LibraryFolder(), Config::Instance()->HostComputeBackend.value_or_default(),
                                dlssgOutput, otherFgOutput,
                                DlssNr::Backend::ActiveKindFromConfig() == DlssNr::Backend::Kind::Daniel);
}
bool NeuralHubFgInstalled() { return NeuralHubFgFilesInstalled() && HostD3d12Supported(); }
bool NeuralHubFgServed()
{
    return (State::Instance().NeuralHubArmOk && State::Instance().NVNGX_DLSSG_Path.has_value() && !HostKeptOnHip()) ||
           NeuralHubFgInstalled();
}
bool HasFiles()
{
    // Proxy names such as winmm.dll can load before Util::DllPath is finalized.
    // A negative result cached at that point disabled the AMD backend for the
    // rest of the process and left the menu at "waiting for a DirectX 12 SR
    // frame". Recheck until the package path becomes available.
    std::error_code ec;
    const auto dir = Directory();
    const auto active = DlssNr::Backend::ActiveKindFromConfig();
    if (active == DlssNr::Backend::Kind::Lmxxf)
        return std::filesystem::exists(dir / DlssNr::Backend::LmxxfRuntimeFile(), ec);
    if (active == DlssNr::Backend::Kind::Mochizuki)
        return std::filesystem::exists(dir / L"MochizukiNrRuntime.dll", ec);
    if (active == DlssNr::Backend::Kind::Daniel)
        return std::filesystem::exists(dir / L"dlssnr_amd_pass1.dll", ec);
    return std::filesystem::exists(dir / L"LmxxfNrRuntime.dll", ec) ||
           std::filesystem::exists(dir / L"dlssnr_amd_pass1.dll", ec);
}
const char* RuntimeName()
{
    // The menu queries this every frame. Cache both known and unknown hashes,
    // but recheck the path and metadata so an early proxy-path query or a DLL
    // replacement does not leave a stale display for the rest of the process.
    // Runtime loading still performs its own full SHA validation.
    static std::mutex cacheMutex;
    static std::filesystem::path cachedPath;
    static std::uintmax_t cachedSize = 0;
    static std::filesystem::file_time_type cachedWriteTime {};
    static const char* cachedName = nullptr;
    static bool cached = false;
    std::lock_guard lock(cacheMutex);
    std::error_code ec;
    const auto path = Directory() / L"dlssnr_amd_pass1.dll";
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        cached = false;
        return nullptr;
    }
    const auto writeTime = std::filesystem::last_write_time(path, ec);
    if (ec)
    {
        cached = false;
        return nullptr;
    }
    if (!cached || path != cachedPath || size != cachedSize || writeTime != cachedWriteTime)
    {
        cachedName = AmdPreSr::IdentifyRuntimeName(path);
        cachedPath = path;
        cachedSize = size;
        cachedWriteTime = writeTime;
        cached = true;
    }
    return cachedName;
}
bool NgxHostDeferred() { return ngxHostDeferred.load(std::memory_order_relaxed) && !State::Instance().NeuralHubArmOk; }
bool EnsureNgxHost(ID3D12Device* device, ID3D12CommandQueue* q, bool wanted)
{
    if (State::Instance().NeuralHubArmOk)
        return true;
    if (Exiting() || !device)
        return false;
    const auto active = DlssNr::Backend::ActiveKindFromConfig();
    // Another NR runtime (or none) is selected: danielblnc 0.8.0's host still provides DLSS-SR/DLSS-RR on its own,
    // without its NR, as long as its pass file is installed.
    const bool hostOnly = active != DlssNr::Backend::Kind::Daniel;
    if (hostOnly)
    {
        std::error_code ec;
        if (!std::filesystem::exists(Directory() / L"dlssnr_amd_pass1.dll", ec))
            return false;
    }
    else if (!HasFiles())
        return false;
    const char* build = RuntimeName();
    if (!build || std::strcmp(build, "0.8.0") != 0)
        return false;
    if (!IsAmd(device))
        return false;
    if (hostOnly && !wanted)
    {
        const auto upscaler = Config::Instance()->Dx12Upscaler.value_for_config();
        if (!(upscaler == Upscaler::DLSS || upscaler == Upscaler::DLSSD) && !NeuralHubFgInstalled())
        {
            if (!ngxHostDeferred.exchange(true, std::memory_order_relaxed))
                LOG_INFO("AMD neural: NrBackend={}; danielblnc {} host not loaded, neither its DLSS nor its DLSS-FG was "
                         "asked for (picking DLSS in the menu loads it)",
                         Config::Instance()->NrBackend.value_or_default(), build);
            return false;
        }
    }

    ID3D12CommandQueue* confirmedQ = nullptr;
    if (!q)
    {
        confirmedQ = s_confirmedRenderQueue.Get();
        q = confirmedQ;
    }
    if (!q)
        q = reinterpret_cast<ID3D12CommandQueue*>(State::Instance().currentCommandQueue);

    ID3D12CommandQueue* fallbackQ = nullptr;
    if (!q)
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        if (SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&fallbackQ))))
            q = fallbackQ;
    }
    if (!q)
    {
        if (confirmedQ)
            confirmedQ->Release();
        return false;
    }

    std::lock_guard initGuard(initMutex);
    if (hostOnly)
    {
        // Once per process: a host that failed to load or arm stays loaded and cannot be loaded again.
        static bool attempted = false;
        if (!attempted && InstallSubmissionHook(device, q))
        {
            attempted = true;
            if (DlssNr::Backend::SubmissionHooksWanted() && DlssNr::Submission::Hooks::IsArmed())
                DlssNr::Submission::Hooks::SetProxyWrap(true);
            auto* host = new AmdPreSr::Backend(device, q, Directory());
            ngxHostOnly.store(host, std::memory_order_release);
            const auto raw = Config::Instance()->NrBackend.value_or_default();
            LOG_INFO("AMD neural: NrBackend={}; danielblnc {} host loaded for NGX DLSS-SR/DLSS-RR only (armed: {})",
                     raw, build, State::Instance().NeuralHubArmOk);
            if (!State::Instance().NeuralHubArmOk)
                LOG_WARN("AMD neural: danielblnc host for NGX: {}", host->Status());
        }
        if (fallbackQ)
            fallbackQ->Release();
        if (confirmedQ)
            confirmedQ->Release();
        return State::Instance().NeuralHubArmOk;
    }
    auto b = backend.load();
    if (!b)
    {
        if (!InstallSubmissionHook(device, q))
        {
            if (fallbackQ)
                fallbackQ->Release();
            if (confirmedQ)
                confirmedQ->Release();
            return false;
        }
        if (DlssNr::Backend::SubmissionHooksWanted() && DlssNr::Submission::Hooks::IsArmed())
            DlssNr::Submission::Hooks::SetProxyWrap(true);
        b = new DlssNr::Backend::DanielBackend(device, q, Directory());
        std::string label = build ? std::string("danielblnc ") + build : std::string("danielblnc");
        {
            const auto raw = Config::Instance()->NrBackend.value_or_default();
            LOG_INFO("AMD neural: NrBackend={} runs {} (pre-armed for NGX DLSS-SR/DLSS-RR)", raw, label);
        }
        {
            std::lock_guard guard(runtimeLabelMutex);
            runtimeLabel = std::move(label);
        }
        const auto adapter = device->GetAdapterLuid();
        device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&backendDevice));
        backendAdapter = adapter;
        stabilizer = std::make_unique<AmdPreSr::ResidualStabilizer>(device);
        composition = std::make_unique<AmdPreSr::RenoComposition>(device);
        person = std::make_unique<DlssNr::Person::Partition>(device);
        foveation = std::make_unique<AmdPreSr::Foveation::Passes>(device);
        backend.store(b);
        neuralTimer = std::make_unique<GpuTime_Dx12>(device);
    }

    if (fallbackQ)
        fallbackQ->Release();
    if (confirmedQ)
        confirmedQ->Release();

    return State::Instance().NeuralHubArmOk;
}
bool NgxHostInstalled()
{
    if (State::Instance().NeuralHubArmOk)
        return true;
    if (IdentifyGpu::getPrimaryGpu().vendorId != VendorId::AMD)
        return false;
    const char* build = RuntimeName();
    return build && std::strcmp(build, "0.8.0") == 0;
}
bool EnsureNgxHostForInterop()
{
    if (State::Instance().NeuralHubArmOk)
        return true;
    if (Exiting() || !NgxHostInstalled())
        return false;
    ID3D12Device* device = nullptr;
    {
        ScopedSkipSpoofingGlobal skipSpoofing {};
        ScopedSkipVulkanHooks skipVulkanHooks {};
        device = WithDx12::RequestD3D12Device(D3D_FEATURE_LEVEL_12_0, nullptr);
    }
    const bool armed = device && EnsureNgxHost(device, nullptr, true);
    LOG_INFO("AMD neural: NeuralHub host for a w/Dx12 DLSS feature: {}", armed ? "armed" : "not armed");
    return armed;
}
static bool Run(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params, ID3D12CommandQueue* q, bool afterUpscale,
                ID3D12Resource** outResult)
{
    if (outResult)
        *outResult = nullptr;
    // A layer that wraps the game's lists, ReShade loaded as dxgi.dll, hands NGX its wrapper and gives
    // ExecuteCommandLists ours from under it. Work with ours, so the list and its device are the ones
    // the queue sees; otherwise the list is never observed and NR stays off.
    DlssNr::Submission::ILogicalCommandList* logical = nullptr;
    if (cmd &&
        SUCCEEDED(cmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                      reinterpret_cast<void**>(&logical))) &&
        logical)
    {
        ID3D12GraphicsCommandList* ours = nullptr;
        if (SUCCEEDED(logical->QueryInterface(IID_PPV_ARGS(&ours))) && ours)
        {
            cmd = ours;
            ours->Release(); // the game holds the list for this evaluate
        }
        logical->Release();
    }
    // Process exit has begun: the original colour goes to SR.
    if (Exiting())
        return true;
    // This evaluate's colour is already the model's answer (DlssNrPreUpscale or DualFeature offering the pre-SR
    // seam a second time). Running again would only be refused, and would cost the model its history.
    if (!afterUpscale && HasReplacement(params))
        return true;
    // A single backend consumes one SR stream even if the engine rotates worker threads.
    // Serialize shared settling/identity state; thread-local replacement ownership stays unchanged.
    std::lock_guard frameGuard(frameMutex);
    ++runSerial;
    DlssNr::Backend::LmxxfProbe::CurrentEvidence() = {};
    if (!HasFiles())
        return false;
    const auto requested = DlssNr::Backend::RequestedKind();
    const auto active = DlssNr::Backend::ActiveKindFromConfig();
    if (active == DlssNr::Backend::Kind::Off)
        return true;
    if (requested == DlssNr::Backend::Kind::Lmxxf && !DlssNr::Backend::LmxxfWired())
    {
        static bool loggedLmxxfFallback = false;
        if (!loggedLmxxfFallback)
        {
            loggedLmxxfFallback = true;
            Message("AMD pre-SR: NrBackend=lmxxf is not wired; using daniel");
        }
    }
    ID3D12Device* device = nullptr;
    if (!cmd || !params || FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))))
        return true;
    thread_local LUID checkedAdapter {};
    thread_local bool checked = false, amd = false;
    const auto adapter = device->GetAdapterLuid();
    if (!checked || adapter.HighPart != checkedAdapter.HighPart || adapter.LowPart != checkedAdapter.LowPart)
    {
        amd = IsAmd(device);
        checkedAdapter = adapter;
        checked = true;
    }
    if (!amd)
    {
        device->Release();
        return false;
    }
    ID3D12CommandQueue* confirmedQ = nullptr;
    if (!q)
    {
        confirmedQ = s_confirmedRenderQueue.Get();
        if (confirmedQ)
        {
            ID3D12Device* qDev = nullptr;
            if (SUCCEEDED(confirmedQ->GetDevice(IID_PPV_ARGS(&qDev))))
            {
                IUnknown* devId1 = nullptr;
                IUnknown* devId2 = nullptr;
                device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&devId1));
                qDev->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&devId2));
                if (devId1 && devId2 && devId1 == devId2)
                {
                    q = confirmedQ;
                }
                if (devId1)
                    devId1->Release();
                if (devId2)
                    devId2->Release();
                qDev->Release();
            }
            if (!q)
            {
                confirmedQ->Release();
                confirmedQ = nullptr;
            }
        }
    }
    if (!q)
    {
        // Target list not yet observed on any execution queue.
        // Register cmd as awaiting observation, and bypass NR this frame (original Color to SR).
        s_awaitingTracker.Add(cmd);
        if (!submissionHookReady)
        {
            auto* fallback = reinterpret_cast<ID3D12CommandQueue*>(State::Instance().currentCommandQueue);
            if (fallback)
                InstallSubmissionHook(device, fallback);
        }
        device->Release();
        static bool s_loggedWait = false;
        if (!s_loggedWait)
        {
            s_loggedWait = true;
            LOG_INFO("AMD pre-SR: awaiting execution queue observation for target command list {:p}; bypassing NR this "
                     "frame",
                     reinterpret_cast<void*>(cmd));
        }
        return true;
    }
    std::lock_guard initGuard(initMutex);
    auto b = backend.load();
    if (!b)
    {
        if (!InstallSubmissionHook(device, q))
        {
            device->Release();
            if (confirmedQ)
                confirmedQ->Release();
            return true;
        }
        if (DlssNr::Backend::SubmissionHooksWanted() && DlssNr::Submission::Hooks::IsArmed())
            DlssNr::Submission::Hooks::SetProxyWrap(true);
        std::string label;
        if (active == DlssNr::Backend::Kind::Lmxxf)
        {
            const wchar_t* file = DlssNr::Backend::LmxxfRuntimeFile();
            b = new DlssNr::Backend::LmxxfBackend(device, q, Directory(), file);
            label = wcscmp(file, L"LmxxfNrRuntime.dll") == 0 ? "lmxxf" : "rdna2 (Rdna2NrRuntime.dll)";
        }
        else if (active == DlssNr::Backend::Kind::Mochizuki)
        {
            b = new DlssNr::Backend::LmxxfBackend(device, q, Directory(), L"MochizukiNrRuntime.dll");
            label = "mochizuki";
        }
        else
        {
            b = new DlssNr::Backend::DanielBackend(device, q, Directory());
            const char* build = RuntimeName();
            label = build ? std::string("danielblnc ") + build : std::string("danielblnc");
        }
        // Which runtime the INI's NrBackend got, once: a name that is none of them runs danielblnc (Kind.h). lmxxf
        // and mochizuki refuse a GPU before the RX 9000 series in their own logs.
        {
            const auto raw = Config::Instance()->NrBackend.value_or_default();
            const bool named = DlssNr::Backend::ParseKind(raw) != DlssNr::Backend::Kind::Daniel || raw.empty() ||
                               _stricmp(raw.c_str(), "auto") == 0 || _stricmp(raw.c_str(), "daniel") == 0;
            if (!named)
                LOG_WARN("AMD neural: NrBackend={} is not a runtime (daniel, lmxxf, rdna2, mochizuki, auto or off); "
                         "danielblnc runs instead",
                         raw);
            LOG_INFO("AMD neural: NrBackend={} runs {}", raw, label);
        }
        {
            std::lock_guard guard(runtimeLabelMutex);
            runtimeLabel = std::move(label);
        }
        device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&backendDevice));
        backendAdapter = adapter;
        // Before the backend is published: InvalidateHistory reaches the stabilizer through it.
        stabilizer = std::make_unique<AmdPreSr::ResidualStabilizer>(device);
        composition = std::make_unique<AmdPreSr::RenoComposition>(device);
        person = std::make_unique<DlssNr::Person::Partition>(device);
        foveation = std::make_unique<AmdPreSr::Foveation::Passes>(device);
        backend.store(b);
        neuralTimer = std::make_unique<GpuTime_Dx12>(device);
    }
    // The backend and its runtime session belong to the device they were built on. A frame recorded on another
    // one (a second adapter) goes to SR untouched rather than reach resources that device cannot use. A different
    // COM identity on the same adapter is the same device behind a wrapper: D3D12 has one device per adapter.
    {
        IUnknown* deviceId = nullptr;
        device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&deviceId));
        const bool sameDevice = deviceId == backendDevice || (adapter.HighPart == backendAdapter.HighPart &&
                                                              adapter.LowPart == backendAdapter.LowPart);
        if (deviceId)
            deviceId->Release();
        if (!sameDevice)
        {
            static bool loggedForeignDevice = false;
            if (!loggedForeignDevice)
            {
                loggedForeignDevice = true;
                LOG_WARN("AMD pre-SR: frame recorded on device {:p} (adapter {:08X}{:08X}), not the backend's {:p}; "
                         "passing it through without NR",
                         reinterpret_cast<void*>(deviceId), static_cast<uint32_t>(adapter.HighPart),
                         static_cast<uint32_t>(adapter.LowPart), reinterpret_cast<void*>(backendDevice));
            }
            device->Release();
            if (confirmedQ)
                confirmedQ->Release();
            return true;
        }
    }
    device->Release();
    if (confirmedQ)
    {
        confirmedQ->Release();
        confirmedQ = nullptr;
    }
    // The hook observes this list when the current frame is submitted and then
    // binds the actual queue before waking HIP. Engines that rotate command-list
    // objects may never submit the same object twice, so do not require a prior
    // observation here.
    Message("");
    // The swapchain's present queue can change when FG is enabled. It is
    // only a bootstrap hint; Submitted identifies the queue executing our list.
    AmdPreSr::Frame f {};
    f.afterUpscale = afterUpscale;
    f.colour =
        afterUpscale ? Resource(params, NVSDK_NGX_Parameter_Output) : Resource(params, NVSDK_NGX_Parameter_Color);
    f.motion = Resource(params, NVSDK_NGX_Parameter_MotionVectors);
    f.depth = Resource(params, NVSDK_NGX_Parameter_Depth);
    f.exposure = Resource(params, NVSDK_NGX_Parameter_ExposureTexture);
    params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &f.preExposure);
    params->Get(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, &f.exposureScale);
    // The render grid: the colour's extent before the upscale, and the guides' extent always.
    UINT renderWidth = 0, renderHeight = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &renderWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &renderHeight);
    if (afterUpscale)
    {
        params->Get(NVSDK_NGX_Parameter_OutWidth, &f.width);
        params->Get(NVSDK_NGX_Parameter_OutHeight, &f.height);
        f.guideWidth = renderWidth;
        f.guideHeight = renderHeight;
    }
    else
    {
        f.width = renderWidth;
        f.height = renderHeight;
    }
    if (f.colour)
    {
        const auto extent = f.colour->GetDesc();
        if (!f.width)
            f.width = static_cast<UINT>(extent.Width);
        if (!f.height)
            f.height = extent.Height;
    }
    if (afterUpscale && (!f.guideWidth || !f.guideHeight) && f.depth)
    {
        const auto guideExtent = f.depth->GetDesc();
        f.guideWidth = static_cast<UINT>(guideExtent.Width);
        f.guideHeight = guideExtent.Height;
    }
    UINT x = 0, y = 0, flags = 0, reset = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &x);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &y);
    if (x || y)
    {
        Message("AMD pre-SR: nonzero colour subrect origin unsupported");
        return true;
    }
    auto haveFlags = params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags) == NVSDK_NGX_Result_Success;
    if (!haveFlags && State::Instance().currentFeature != nullptr)
    {
        flags = static_cast<UINT>(State::Instance().currentFeature->GetFeatureFlags());
        haveFlags = true;
    }
    // NGX applies the exposure texture to HDR input only. An SDR colour (no IsHDR, or an 8- or 10-bit UNORM one) is
    // already exposed: Watch Dogs: Legion hands an R8G8B8A8 colour with an exposure that grows huge in the dark, and
    // applied again it overflowed the network into a green frame.
    if (f.exposure && f.colour)
    {
        switch (AmdPreSr::ReadFormat(f.colour->GetDesc().Format))
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            f.exposure = nullptr;
            break;
        default:
            if (haveFlags && !(flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR))
                f.exposure = nullptr;
        }
    }
    if (haveFlags && !(flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) && f.motion)
    {
        params->Get(NVSDK_NGX_Parameter_OutWidth, &f.motionWidth);
        params->Get(NVSDK_NGX_Parameter_OutHeight, &f.motionHeight);
        if (!f.motionWidth)
            f.motionWidth = static_cast<UINT>(f.motion->GetDesc().Width);
        if (!f.motionHeight)
            f.motionHeight = f.motion->GetDesc().Height;
    }
    // Let SR finish its reconfiguration before rebuilding the private HIP model.
    // Do not retain or replay the old image while input sizes are settling.
    static UINT settlingWidth = 0, settlingHeight = 0;
    static DXGI_FORMAT settlingFormat = DXGI_FORMAT_UNKNOWN;
    static float settlingScale = 1.f;
    static ULONGLONG settlingSince = 0;
    // mochizuki builds its network from its own keys and swaps a rebuilt one in while the frames keep the old, so
    // danielblnc's NR resolution (AmdNrScale, INI AmdModelScale) and its dynamic steps (AmdDynamicScale) neither size
    // it nor settle it: the controller stays off and the gate's scale is constant. With dynamic resolution
    // (MochizukiDynamicResolution other than exact) it also keeps its network while the render subrect moves inside
    // one colour allocation, so the gate settles and warms up on that allocation (and its format) instead, and a
    // subrect change only restarts the history.
    const bool mochizuki = active == DlssNr::Backend::Kind::Mochizuki;
    // After the upscale the vectors are still on the render grid; the runtime would take them for the frame's extent.
    if (mochizuki && afterUpscale && f.motion && !f.motionWidth)
    {
        f.motionWidth = f.guideWidth;
        f.motionHeight = f.guideHeight;
    }
    const bool dynamicResolution =
        mochizuki && Config::Instance()->MochizukiDynamicResolution.value_or_default() != "exact";
    // Post-upscale answers to the "after RR" controls, so the two placements can be tuned apart.
    // The AMD backend never supersamples, so its scale tops out at 1.0 (the frame's own size).
    const bool dynamicOn = !mochizuki && Config::Instance()->AmdDynamicScale.value_or_default();
    const float sessionScale = dynamicScale.Step(
        std::chrono::steady_clock::now(), dynamicOn, Config::Instance()->AmdDynamicTargetFps.value_or_default(),
        afterUpscale ? std::clamp(Config::Instance()->DlssNrRRWorkingScale.value_or_default(), .25f, 1.f)
                     : Config::Instance()->AmdNrScale.value_or_default());
    dynamicScaleNow = dynamicOn ? sessionScale : 0.f;
    dynamicFpsNow = dynamicOn ? static_cast<float>(dynamicScale.Fps()) : 0.f;
    dynamicChangesNow = dynamicScale.changes;
    const float requestedScale = mochizuki ? 1.f : sessionScale;
    // What the gate settles on: the render subrect, or with mochizuki's dynamic resolution the colour allocation.
    UINT gateWidth = f.width, gateHeight = f.height;
    DXGI_FORMAT gateFormat = DXGI_FORMAT_UNKNOWN;
    if (dynamicResolution && f.colour)
    {
        const auto allocation = f.colour->GetDesc();
        gateWidth = static_cast<UINT>(allocation.Width);
        gateHeight = allocation.Height;
        gateFormat = allocation.Format;
    }
    // danielblnc and lmxxf under a game's dynamic resolution (The Witcher 3 reallocates its frame at every step): the
    // model keeps the largest frame seen at this format and aspect, so a step neither settles nor warms up. danielblnc
    // resamples a smaller frame up to it; lmxxf's runtime fits it into the network that size chose and rebuilds only
    // its codec. A larger frame, another format or another aspect settles as before and becomes the new extent.
    // mochizuki's dynamic resolution, likewise, on the colour allocation (Cyberpunk 2077 makes a new one at every
    // step): its runtime holds network and buffers at the largest of them (Session::DrsExtent).
    static UINT drsWidth = 0, drsHeight = 0;
    static DXGI_FORMAT drsFormat = DXGI_FORMAT_UNKNOWN;
    if ((active == DlssNr::Backend::Kind::Daniel || active == DlssNr::Backend::Kind::Lmxxf || dynamicResolution) &&
        f.colour && !afterUpscale)
    {
        const auto format = f.colour->GetDesc().Format;
        const bool sameAspect =
            drsHeight && gateHeight && std::abs(double(gateWidth) / gateHeight - double(drsWidth) / drsHeight) < .02;
        if (format == drsFormat && sameAspect && gateWidth <= drsWidth && gateHeight <= drsHeight)
        {
            gateWidth = drsWidth;
            gateHeight = drsHeight;
            if (!mochizuki)
            {
                f.fixedWidth = drsWidth;
                f.fixedHeight = drsHeight;
            }
        }
        else
        {
            drsWidth = gateWidth;
            drsHeight = gateHeight;
            drsFormat = format;
        }
    }
    const auto now = GetTickCount64();
    // mochizuki: a game that renders into two colour textures in turn (Forza Horizon 6) changes the gate's key every
    // frame, A B A B, and the wait never ended. Back to the key before the last is a flip, not a change: the wait and
    // the history stay. Anything else is a change as before.
    static UINT altWidth = 0, altHeight = 0;
    static DXGI_FORMAT altFormat = DXGI_FORMAT_UNKNOWN;
    static bool altValid = false, altLogged = false;
    const bool keyChanged = settlingWidth != gateWidth || settlingHeight != gateHeight ||
                            settlingFormat != gateFormat || settlingScale != requestedScale;
    const bool flip = keyChanged && mochizuki && altValid && altWidth == gateWidth && altHeight == gateHeight &&
                      altFormat == gateFormat && settlingScale == requestedScale;
    if (flip)
    {
        if (!std::exchange(altLogged, true))
            b->TraceBoundary("the game alternates two colour textures: " + std::to_string(settlingWidth) + "x" +
                             std::to_string(settlingHeight) + " and " + std::to_string(gateWidth) + "x" +
                             std::to_string(gateHeight) + "; NR runs on both");
        std::swap(altWidth, settlingWidth);
        std::swap(altHeight, settlingHeight);
        std::swap(altFormat, settlingFormat);
    }
    else if (keyChanged)
    {
        altWidth = settlingWidth;
        altHeight = settlingHeight;
        altFormat = settlingFormat;
        altValid = settlingWidth != 0;
        b->TraceBoundary("settings change: input " + std::to_string(settlingWidth) + "x" +
                         std::to_string(settlingHeight) + " -> " + std::to_string(gateWidth) + "x" +
                         std::to_string(gateHeight) +
                         (dynamicResolution ? " (colour allocation, DXGI " + std::to_string(UINT(gateFormat)) + ")"
                                            : std::string()) +
                         "; NR scale " + std::to_string(settlingScale) + " -> " + std::to_string(requestedScale) +
                         "; " + VramUsage(adapter));
        settlingWidth = gateWidth;
        settlingHeight = gateHeight;
        settlingFormat = gateFormat;
        settlingScale = requestedScale;
        settlingSince = now;
        b->InvalidateHistory();
    }
    if (now - settlingSince < 300)
    {
        Message("AMD neural: waiting for resolution settings to settle");
        return true;
    }
    const FrameIdentity current { f.colour, f.motion, f.depth, gateWidth, gateHeight };
    // Resource addresses rotate in Unreal's frame buffers. Only an extent
    // change requires warm-up; pointer equality can suppress every frame.
    // The other of mochizuki's two alternating textures (above) is the same frame too.
    const bool sameFrame = (current.width == lastFrame.width && current.height == lastFrame.height) ||
                           (flip && lastFrame.width == altWidth && lastFrame.height == altHeight);
    if (sameFrame && flip)
        lastFrame = current;
    if (!sameFrame)
    {
        lastFrame = current;
        stableFrames = 0;
        b->InvalidateHistory();
        Message("AMD pre-SR: warming up after an upscaler/resource change");
        return true;
    }
    if (stableFrames < 2 && ++stableFrames < 2)
    {
        Message("AMD pre-SR: warming up after an upscaler/resource change");
        return true;
    }
    // mochizuki's dynamic resolution: a render subrect change inside the allocation needs no settling, only a new
    // history (the runtime restarts it too).
    static UINT subrectWidth = 0, subrectHeight = 0;
    if (dynamicResolution && (subrectWidth != f.width || subrectHeight != f.height))
        b->InvalidateHistory();
    subrectWidth = f.width;
    subrectHeight = f.height;
    f.depthInverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    params->Get(NVSDK_NGX_Parameter_Reset, &reset);
    f.reset = reset != 0;
    params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &f.motionScaleX);
    params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &f.motionScaleY);
    const auto& cfg = *Config::Instance();
    const bool jitteredMotion = cfg.JitterCancellation.has_value()
                                    ? cfg.JitterCancellation.value()
                                    : haveFlags && (flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
    if (!jitteredMotion)
    {
        params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &f.jitterX);
        params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &f.jitterY);
    }
    if (afterUpscale)
        f.colourState = cfg.OutputResourceBarrier.has_value()
                            ? static_cast<D3D12_RESOURCE_STATES>(cfg.OutputResourceBarrier.value())
                            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    else if (cfg.ColorResourceBarrier.has_value())
        f.colourState = static_cast<D3D12_RESOURCE_STATES>(cfg.ColorResourceBarrier.value());
    if (cfg.MVResourceBarrier.has_value())
        f.motionState = static_cast<D3D12_RESOURCE_STATES>(cfg.MVResourceBarrier.value());
    if (cfg.DepthResourceBarrier.has_value())
        f.depthState = static_cast<D3D12_RESOURCE_STATES>(cfg.DepthResourceBarrier.value());
    if (cfg.ExposureResourceBarrier.has_value())
        f.exposureState = static_cast<D3D12_RESOURCE_STATES>(cfg.ExposureResourceBarrier.value());
    auto s = SettingsFromConfig(cfg, sessionScale);
    s.fixedSeed = cfg.AmdFixedSeed.value_or_default();
    // Evaluate cut: Split proxy + SetBetween(EnqueueHip). Live only when SubmissionHooksWanted() (NrBackend=lmxxf
    // or mochizuki).
    DlssNr::Backend::LmxxfCut::OnEvaluateBeforeRecord(cmd);
    // The original colour goes to SR.
    if (StandDownIfTripped())
        return true;
    // Person protection (NrPersonPartition, from TheAutomatic/dlss-5-amd-project): lmxxf and mochizuki before SR. With
    // more than one pass the runtime also hands over its first pass (mochizuki: the RenoDX stage's, in the frame's
    // encoding); in single pass it does something only with Strength or DetailGain below 1, as theirs.
    const auto personSettings =
        DlssNr::Person::Settings { cfg.NrPersonStrength.value_or_default(), cfg.NrPersonDetailGain.value_or_default(),
                                   cfg.NrPersonDebugMask.value_or_default() }
            .Bounded();
    const UINT personPasses = mochizuki ? std::clamp(cfg.MochizukiRenoPasses.value_or_default(), 1u,
                                                     cfg.MochizukiRenoUnlockPasses.value_or_default() ? 30u : 4u)
                                        : s.passes;
    const bool personWanted = cfg.NrPersonPartition.value_or_default() && !afterUpscale &&
                              active != DlssNr::Backend::Kind::Daniel &&
                              (personPasses > 1 || personSettings.ChangesSinglePass());
    const int personModel = std::clamp(cfg.NrPersonModel.value_or_default(), 0, 2);
    const unsigned faceSize = DlssNr::Person::Face::BoundedSize(cfg.NrFaceInputSize.value_or_default());
    {
        // Another model or setting: the masks and the stabilizer's history of the old composite go.
        static DlssNr::Person::Settings lastSettings;
        static int lastModel = -1;
        static unsigned lastFaceSize = 320;
        if (!(lastSettings == personSettings) || lastModel != personModel || lastFaceSize != faceSize)
        {
            person->Invalidate();
            stabilizer->Invalidate();
        }
        lastSettings = personSettings;
        lastModel = personModel;
        lastFaceSize = faceSize;
    }
    const bool personReady = DlssNr::Person::Prepare(personWanted, Directory(), personModel, faceSize);
    s.firstPass = personWanted && personPasses > 1;
    // Foveated NR: this frame's grid warps it, the fixed extent's sizes the textures and is the one logged; a new one
    // restarts the history (a dynamic resolution step restarts it anyway).
    AmdPreSr::Foveation::Grid grid { f.width, f.height, f.width, f.height }, fixedGrid = grid;
    if (const int mode = std::clamp(cfg.NrFoveation.value_or_default(), 0, 4);
        mode > 0 && !afterUpscale && !foveationFailed)
    {
        static const char* loggedOff = nullptr;
        const char* off = !mochizuki      ? "runs on mochizuki only; this runtime gets the whole frame"
                          : personWanted ? "is off while person protection runs"
                                         : nullptr;
        if (off && loggedOff != off)
            LOG_INFO("AMD pre-SR: foveated NR {}", off);
        loggedOff = off;
        if (!off)
        {
            auto params = AmdPreSr::Foveation::Preset(mode);
            if (mode >= 4)
                params = { cfg.NrFoveationCentreWidth.value_or_default(), cfg.NrFoveationCentreHeight.value_or_default(),
                           cfg.NrFoveationFalloff.value_or_default(), cfg.NrFoveationEdge.value_or_default() };
            grid = AmdPreSr::Foveation::MakeGrid(f.width, f.height, params);
            fixedGrid = AmdPreSr::Foveation::MakeGrid(gateWidth, gateHeight, params);
        }
    }
    const bool foveated = grid.Active();
    if (const auto key = foveated ? fixedGrid : AmdPreSr::Foveation::Grid {}; !(key == foveationGrid))
    {
        if (foveated)
            LOG_INFO("AMD pre-SR: foveated NR, the network at {}x{} for {}x{} ({:.0f}% of the pixels; edge density "
                     "{:.2f} x {:.2f})",
                     fixedGrid.workWidth, fixedGrid.workHeight, gateWidth, gateHeight, 100 * fixedGrid.Share(),
                     fixedGrid.x.e, fixedGrid.y.e);
        else if (foveationGrid.Active())
            LOG_INFO("AMD pre-SR: foveated NR off at {}x{}", f.width, f.height);
        foveationGrid = key;
        foveationJitter = false;
        b->InvalidateHistory();
        stabilizer->Invalidate();
    }
    if (neuralTimer)
        neuralTimer->Start(cmd);
    // The warp reads the game's colour and vectors; mochizuki reads no depth before SR.
    AmdPreSr::Frame in = f, source = f;
    source.depth = nullptr;
    if (foveated)
    {
        AmdPreSr::Foveation::Passes::Options o;
        o.allocWidth = fixedGrid.workWidth;
        o.allocHeight = fixedGrid.workHeight;
        // The jitter step goes through the warp with the vectors: in render pixels it is only right in the centre.
        const bool fold = cfg.MochizukiJitterHistory.value_or_default() && f.motion;
        if (fold)
        {
            if (foveationJitter && !f.reset)
            {
                o.jitterStepX = foveationJitterX - f.jitterX;
                o.jitterStepY = foveationJitterY - f.jitterY;
            }
            foveationJitterX = f.jitterX;
            foveationJitterY = f.jitterY;
            foveationJitter = true;
        }
        try
        {
            in = foveation->Warp(cmd, source, grid, o);
            if (fold)
                in.jitterX = in.jitterY = 0;
        }
        catch (const std::exception& e)
        {
            foveationFailed = true;
            in = f;
            LOG_ERROR("AMD pre-SR: foveated NR off for this session: {}", e.what());
        }
    }
    else
        foveation->Idle();
    auto replacement = b->Record(cmd, in, s);
    if (replacement && in.colour != f.colour)
    {
        try
        {
            const auto allocation = f.colour->GetDesc();
            replacement = foveation->Unwarp(cmd, source, replacement, UINT(allocation.Width), allocation.Height);
        }
        catch (const std::exception& e)
        {
            foveationFailed = true;
            replacement = nullptr;
            LOG_ERROR("AMD pre-SR: foveated NR off for this session: {}", e.what());
        }
    }
    if (replacement && personWanted && personReady)
    {
        ID3D12Resource* first = personPasses > 1 ? b->FirstPass() : replacement;
        const bool continuous = lastPersonSerial == runSerial - 1 && now - lastPersonTick < 250;
        replacement = person->Record(cmd, f, first, replacement, jitteredMotion, continuous,
                                     DlssNr::Person::Worker().Latest(), personSettings);
        lastPersonSerial = runSerial;
        lastPersonTick = now;
        if (const auto report = person->DiagnosticReport(); !report.empty())
            LOG_INFO("{}", report);
    }
    else
        person->Idle();
    // lmxxf smooths inside its runtime and has no stabilizer key.
    float stabilizerStrength = 0, stabilizerThreshold = 2;
    if (mochizuki)
    {
        stabilizerStrength = cfg.MochizukiStabilizerStrength.value_or_default();
        stabilizerThreshold = cfg.MochizukiStabilizerThreshold.value_or_default();
    }
    else if (active != DlssNr::Backend::Kind::Lmxxf)
    {
        stabilizerStrength = cfg.AmdStabilizerStrength.value_or_default();
        stabilizerThreshold = cfg.AmdStabilizerThreshold.value_or_default();
    }
    if (replacement && !afterUpscale && stabilizerStrength > 0 && !stabilizerFailed)
    {
        // The history bridges the runtime's own restarts. A Run that returned early, a reset or a 250 ms gap drops it.
        const bool continuous = lastStabilizedSerial == runSerial - 1 && now - lastStabilizedTick < 250 && !f.reset;
        try
        {
            const auto stabilized =
                stabilizer->Record(cmd, f, replacement, continuous, std::min(stabilizerStrength, 1.f),
                                   std::clamp(stabilizerThreshold, .5f, 8.f));
            if (stabilized != replacement)
            {
                replacement = stabilized;
                lastStabilizedSerial = runSerial;
                lastStabilizedTick = now;
            }
            else
            {
                static bool loggedDepth = false;
                if (!loggedDepth)
                {
                    loggedDepth = true;
                    LOG_INFO("AMD pre-SR: residual stabilizer skipped: the game's depth is not shader readable");
                }
            }
        }
        catch (const std::exception& e)
        {
            stabilizerFailed = true;
            LOG_ERROR("AMD pre-SR: residual stabilizer off for this session: {}", e.what());
        }
    }
    else
        stabilizer->Idle();
    if (replacement && !afterUpscale && active != DlssNr::Backend::Kind::Mochizuki &&
        cfg.DlssNrRenoComposition.value_or_default() && !compositionFailed)
    {
        // The divisor moves at a rate per second; a gap, a reset or the first frame takes this frame's estimate.
        const bool continuous = lastComposedSerial == runSerial - 1 && now - lastComposedTick < 250 && !f.reset;
        try
        {
            // danielblnc's answer is in the frame's own transfer (AmdEncoding); lmxxf and mochizuki take and give
            // the colour as it is.
            replacement = composition->Record(
                cmd, f, replacement, active == DlssNr::Backend::Kind::Daniel ? s.encoding : 1u, !continuous,
                continuous ? float(now - lastComposedTick) / 1000.f : 0.f,
                active == DlssNr::Backend::Kind::Daniel ? cfg.AmdEffectStrength.value_or_default()
                                                        : cfg.DlssNrRenoIntensity.value_or_default(),
                cfg.DlssNrRenoGuard.value_or_default(), cfg.DlssNrRenoColour.value_or_default(),
                cfg.DlssNrRenoChromaClamp.value_or_default(), cfg.DlssNrRenoPedestal.value_or_default());
            lastComposedSerial = runSerial;
            lastComposedTick = now;
        }
        catch (const std::exception& e)
        {
            compositionFailed = true;
            LOG_ERROR("AMD pre-SR: RenoDX composition off for this session: {}", e.what());
        }
    }
    else
        composition->Idle();
    if (neuralTimer)
    {
        neuralTimer->End(cmd);
        // A refused Record puts nothing between the timestamps: that frame ran no model.
        if (auto ms = neuralTimer->ReadGpuTime(q); ms.has_value() && ms.value() > .1)
        {
            const float now = static_cast<float>(ms.value()), shown = neuralMsNow.load();
            neuralMsNow = shown > 0 ? shown * .9f + now * .1f : now;
            neuralMsLast = now;
            neuralAt = GetTickCount64();
        }
    }
    if (replacement && afterUpscale)
    {
        // Nothing is substituted here: the upscaler has already run. The caller writes this into
        // the frame, which is the only place that knows the surface's format and resting state.
        if (outResult)
            *outResult = replacement;
    }
    else if (replacement)
    {
        originalColour = f.colour;
        replacedParams = params;
        params->Set(NVSDK_NGX_Parameter_Color, replacement);
    }
    return true;
}

bool Before(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params, ID3D12CommandQueue* q)
{
    return Run(cmd, params, q, false, nullptr);
}

bool After(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params, ID3D12CommandQueue* q,
           ID3D12Resource** outResult)
{
    return Run(cmd, params, q, true, outResult);
}
bool HasReplacement(NVSDK_NGX_Parameter* params) { return params && replacedParams == params && originalColour; }
void Restore(NVSDK_NGX_Parameter* params)
{
    if (params && replacedParams == params)
    {
        params->Set(NVSDK_NGX_Parameter_Color, originalColour);
        replacedParams = nullptr;
        originalColour = nullptr;
    }
}
void InvalidateHistory()
{
    if (auto b = backend.load())
    {
        b->InvalidateHistory();
        stabilizer->Invalidate();
        person->Invalidate();
    }
}
std::string PersonStatus()
{
    std::string text;
    if (backend.load())
        text = person->Status();
    const std::string worker = DlssNr::Person::Worker().Status();
    return text.empty() ? worker : text + " | " + worker;
}
void TraceContextRelease(unsigned int handle, bool after)
{
    if (auto b = backend.load())
        b->TraceBoundary(std::string(after ? "after" : "before") +
                         " SR context release handle=" + std::to_string(handle));
}
float NeuralMs() { return neuralMsNow.load(); }
float NeuralMsLast() { return GetTickCount64() - neuralAt.load() < 1000 ? neuralMsLast.load() : 0.f; }
void ReportNeuralMs(float ms)
{
    const float shown = neuralMsNow.load();
    neuralMsNow = shown > 0 ? shown * .9f + ms * .1f : ms;
    neuralMsLast = ms;
    neuralAt = GetTickCount64();
    static std::atomic<bool> logged { false };
    if (!logged.exchange(true))
        LOG_INFO("Final-image NR: first GPU time of the model, {:.2f} ms", ms);
    // No upscaler backend named the final image's runtime: danielblnc or mochizuki.
    std::lock_guard guard(runtimeLabelMutex);
    if (runtimeLabel.empty() && DlssNr::Backend::ActiveKindFromConfig() == DlssNr::Backend::Kind::Mochizuki)
        runtimeLabel = "mochizuki";
    else if (runtimeLabel.empty())
    {
        const char* build = RuntimeName();
        runtimeLabel = build ? std::string("danielblnc ") + build : std::string("danielblnc");
    }
}
std::string NeuralRuntime()
{
    std::lock_guard guard(runtimeLabelMutex);
    return runtimeLabel;
}
std::string DynamicStatus()
{
    const float scale = dynamicScaleNow.load();
    if (scale <= 0)
        return {};
    const float fps = dynamicFpsNow.load();
    const int changes = dynamicChangesNow.load();
    char text[160] {};
    int n = fps > 0 ? std::snprintf(text, sizeof(text), "Now at %.0f%%, %.0f fps rendered", scale * 100.f, fps)
                    : std::snprintf(text, sizeof(text), "Now at %.0f%%, measuring", scale * 100.f);
    if (changes >= AmdPreSr::DynamicScale::kMaxChanges)
        std::snprintf(text + n, sizeof(text) - n, "\nChange limit reached (%d): it stays here until turned off and on",
                      changes);
    else
        std::snprintf(text + n, sizeof(text) - n, ", %d of %d changes used", changes,
                      AmdPreSr::DynamicScale::kMaxChanges);
    return text;
}
bool GraphicsRestartNeeded(UINT activePasses)
{
    if (auto b = backend.load())
        return b->GraphicsRestartNeeded(activePasses);
    return false;
}

bool StandDownIfTripped()
{
    // One job the GPU held for seconds is the warning before the one that locks the PC (StallWatch.h):
    // NR switches off for this session without touching the INI, and the menu says why. Switched on
    // again (menu or the NR key), it starts with a fresh watch.
    auto& stall = stallwatch::Watch::Get();
    stall.limitMs.store(static_cast<UINT64>(std::max(0, Config::Instance()->AmdStallStandDownMs.value_or_default())));
    std::lock_guard lock(standDownMutex);
    const UINT64 held = stall.Tripped();
    if (!held)
    {
        standDown.clear();
        return false;
    }
    char seconds[16];
    std::snprintf(seconds, sizeof(seconds), "%.1f", static_cast<double>(held) / 1000.0);
    const std::string over = VramOverBudget(backendAdapter);
    standDown = std::string("Turned off: the GPU held one NR job for ") + seconds + " s" +
                (over.empty() ? std::string() : "; VRAM was over budget (" + over + ")") +
                ", the warning before a freeze that can lock the PC. Turn NR on again to retry, or "
                "try another runtime. AmdStallStandDownMs=0 turns this check off.";
    LOG_WARN("AMD neural: {}", standDown);
    Message(standDown.c_str());
    stall.Clear();
    Config::Instance()->DlssNrEnabled.set_volatile_value(false);
    return true;
}

std::string StandDownNote()
{
    std::lock_guard lock(standDownMutex);
    return standDown;
}

std::string Status()
{
    // The final image's status only while it is the path that runs: danielblnc or mochizuki, no upscaler, or
    // mochizuki's Present placement.
    if ((AmdPresentExperimental::IsTarget() && !State::Instance().currentFeature &&
         DlssNr::Backend::PresentRuntimeSupported()) ||
        DlssNr::Backend::MochizukiPlacementNow() == DlssNr::Backend::MochizukiPlace::Present)
        return AmdPresentExperimental::Status();
    {
        std::lock_guard l(messageMutex);
        if (!message.empty())
            return message;
    }
    if (auto b = backend.load())
        return b->Status();
    return "AMD pre-SR: idle";
}
void Note(const std::string& text) { Message(text.c_str()); }
std::string RuntimeStatus()
{
    if (auto b = backend.load())
        return b->RuntimeStatus();
    return {};
}
} // namespace DlssNr::AmdBridge
