#include "pch.h"
#include "OpenNrRrFeature_Dx12.h"
#include "OpenNrRrOutputNr.h"

#include <Config.h>
#include <Util.h>
#include <NVNGX_Parameter.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <misc/IdentifyGpu.h>
#include <imgui/ImGuiNotify.hpp>
#include <dlssnr/backend/LmxxfEvaluateCut.h>
#include <dlssnr/backend/LmxxfBackend.h>
#include <dlssnr/backend/mochizuki_runtime/MochizukiNrControls.h>
#include <dlssnr/backend/Selector.h>
#include <dlssnr/amd/AmdBridge.h>
#include <dlssnr/amd/AmdPreSr.h>
#include <dlssnr/amd/Foveation.h>
#include <dlssnr/submission/SubmissionTls.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace LmxxfCut = DlssNr::Backend::LmxxfCut;

namespace
{
std::mutex s_mutex;
HMODULE s_module = nullptr;
OnrRrApi s_api {};
// ABI 2 entry points (the denoised-image hop); null with a runtime that offers only ABI 1
OnrRrApi2 s_api2 {};
// ABI 3: the hop's GPU timing; null with an older runtime
decltype(OnrRrApi3::GetHopTiming) s_getHopTiming = nullptr;
// ABI 4: the D3D12 backend (Record / RecordPost into the game's list); null with an older runtime
decltype(OnrRrApi4::GetBackend) s_getBackend = nullptr;
decltype(OnrRrApi4::Record) s_record = nullptr;
decltype(&OnrRrSetDeltaRate) s_setDeltaRate = nullptr;
decltype(&OnrRrSetFastMode) s_setFastMode = nullptr;
decltype(&OnrRrGetFastMode) s_getFastMode = nullptr;
decltype(&OnrRrSetFp8) s_setFp8 = nullptr;
decltype(&OnrRrGetFp8) s_getFp8 = nullptr;
decltype(OnrRrApi4::RecordPost) s_recordPost = nullptr;
// ABI 5: the Vulkan host backend (PrepareVkDevice / FinishVkDevice / RecordVk); null with an older runtime
OnrRrApi5 s_api5 {};
bool s_loadTried = false;
bool s_checked = false;
bool s_usable = false;
LUID s_luid {};
std::string s_reason;
std::string s_blocked;
std::wstring s_dlssdPath;
void* s_infoContext = nullptr;

// Set by the between slot, read on the next evaluate.
std::atomic<int32_t> s_enqueueRc { ONR_RR_OK };

// NGX publishes the depth type only on a creation that carries DLSSD create params; a recreation keeps it.
bool s_ngxDepthTypeSeen = false;
bool s_ngxReportedHWDepth = false;

std::string RuntimeError()
{
    char text[256] {};
    if (s_api.GetLastError)
        s_api.GetLastError(text, sizeof text);
    return text;
}

// mochizuki on the denoised image (NrOnDenoised): a session of its own on OptiScaler's lists, run between the
// network and the post at the list's one split. Process lifetime, so a recreated feature keeps the network built.
struct DenoisedNr
{
    static constexpr int kSlots = 3;
    struct Slot
    {
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* producer = nullptr; // the image into `colour`, then mochizuki's inputs
        ID3D12GraphicsCommandList* consumer = nullptr; // mochizuki's output back into the runtime's buffer
        // a timestamp before EnqueueNetwork and one after EnqueuePost (with the slot's readback), on timed frames
        ID3D12GraphicsCommandList* stampPre = nullptr;
        ID3D12GraphicsCommandList* stampPost = nullptr;
        std::atomic<uint64_t> busy { 0 };              // fence value that frees the allocator
        bool timed = false;
        uint64_t hop = 0; // the runtime's hop number of the timed frame, 0 until the between slot ran it
        std::atomic<bool> ran { false }; // the between slot submitted the lists, so their NR timestamps are real
    };

    // The hop's timeline on the GPU, every kTimedEvery frames, logged every kTimedLog of them: D3D12 timestamps around
    // OptiScaler's lists (queries kStamps per slot: before EnqueueNetwork, producer start / end, consumer start / end,
    // after EnqueuePost) and the runtime's own (ABI 3), all in QueryPerformanceCounter ticks.
    static constexpr int kStamps = 6;
    static constexpr int kTimedEvery = 16;
    static constexpr int kTimedLog = 20;
    struct Sample
    {
        uint64_t hop = 0;
        uint64_t qpc[kStamps] {};
    };

    std::mutex mutex; // the armed job, between the render thread and the between slot
    DlssNr::Backend::LmxxfBackend* mz = nullptr; // never destroyed, as the bridge's backend
    // [DlssNr] Foveation on the denoised image: the same warp as the bridge's, around mz
    std::unique_ptr<AmdPreSr::Foveation::Passes> foveation;
    AmdPreSr::Foveation::Grid foveationGrid;
    float foveationJitterX = 0, foveationJitterY = 0;
    bool foveationJitter = false, foveationFailed = false;
    // [OpenNR-RR] NrImage=output: the network on RR's finished picture instead of the denoised image
    std::unique_ptr<OpenNrRrOutputNr::Passes> outputNr;
    bool outputFailed = false;
    bool lastOnOutput = false;
    uintptr_t outputJobs = 0; // what the between slot knows such a frame by: it has no job of the runtime
    bool broken = false;
    ID3D12Device* device = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    std::atomic<uint64_t> value { 0 };
    Slot slots[kSlots];
    int next = 0;
    ID3D12Resource* colour = nullptr; // RGBA32F, render extent, what mochizuki reads
    UINT width = 0, height = 0;
    std::atomic<ID3D12CommandQueue*> queue { nullptr }; // the game's queue, learnt from the between slot
    void* armedJob = nullptr;
    int armedSlot = -1;
    uint32_t armedFormat = 0;
    std::string status;
    ID3D12QueryHeap* stamps = nullptr;
    ID3D12Resource* readback = nullptr;
    std::atomic<uint64_t> hops { 0 };          // exported hops, counted as the runtime context counts them
    std::atomic<void*> hopContext { nullptr }; // the context they count for
    uint64_t frames = 0;
    std::vector<Sample> pending; // read back, waiting for the runtime's half (render thread)
    double sum[10] {};
    int summed = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // GPU time of the NR part (producer start to consumer end, mochizuki's own work in between), for the overlay
    std::atomic<float> nrMs { 0 };
    std::atomic<ULONGLONG> nrAt { 0 };

    bool Wait(uint64_t v)
    {
        if (v == 0 || fence->GetCompletedValue() >= v)
            return true;
        fence->SetEventOnCompletion(v, event);
        return WaitForSingleObject(event, 2000) == WAIT_OBJECT_0;
    }

    void SetStatus(std::string s)
    {
        std::lock_guard lock(mutex);
        status = std::move(s);
    }
};

DenoisedNr& Denoised()
{
    static auto* d = new DenoisedNr();
    return *d;
}

// NrImage=output for this frame's picture: the frame is recorded whole and the NR's lists come after it, so this needs the
// D3D12 backend, and a picture format the composite can write.
bool OnOutputFromConfig(const OnrRrFrame& frame)
{
    if (_stricmp(Config::Instance()->OpenNrRrNrImage.value_or_default().c_str(), "output") != 0 || Denoised().outputFailed)
        return false;
    const auto format = AmdPreSr::ReadFormat(static_cast<ID3D12Resource*>(frame.output.resource)->GetDesc().Format);
    return OpenNrRrOutputNr::PictureFormat(format);
}

uint32_t EditFromConfig(std::string* why)
{
    const auto& cfg = *Config::Instance();
    const auto mode = cfg.OpenNrRrNrOnDenoised.value_or_default();
    // display only (the config maps an old "loop" to it)
    const uint32_t edit = _stricmp(mode.c_str(), "display") == 0 ? ONR_RR_EDIT_DISPLAY : ONR_RR_EDIT_OFF;
    why->clear();
    if (edit == ONR_RR_EDIT_OFF)
        return edit;
    if (s_api2.PrepareDenoised == nullptr)
        *why = "OpenNrRrRuntime.dll is too old for it (ABI 1)";
    else if (DlssNr::Backend::ActiveKindFromConfig() != DlssNr::Backend::Kind::Mochizuki)
        *why = "needs NrBackend=mochizuki";
    else if (DlssNr::Backend::MochizukiPlacementNow() != DlssNr::Backend::MochizukiPlace::BeforeSr)
        *why = "MochizukiPlacement runs NR elsewhere";
    else if (!cfg.DlssNrEnabled.value_or_default())
        *why = "NR is off";
    else if (DlssNr::Submission::g_rawExecuteCommandLists == nullptr)
        *why = "the submission hooks are not armed";
    return why->empty() ? edit : ONR_RR_EDIT_OFF;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    list->ResourceBarrier(1, &b);
}

UINT TexelBytes(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 8;
    default:
        return 4;
    }
}

// Caller holds s_mutex.
bool Load()
{
    if (s_loadTried)
        return s_module != nullptr;

    s_loadTried = true;

    const auto path = Util::DllPath().parent_path() / L"OpenNrRrRuntime.dll";
    std::error_code ec;

    if (!std::filesystem::exists(path, ec))
    {
        s_reason = "OpenNrRrRuntime.dll is not in OptiScaler's folder";
        LOG_INFO("OpenRR: {} ({})", s_reason, wstring_to_string(path.wstring()));
        return false;
    }

    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (module == nullptr)
    {
        s_reason = std::format("OpenNrRrRuntime.dll did not load (error {})", GetLastError());
        LOG_ERROR("OpenRR: {} ({})", s_reason, wstring_to_string(path.wstring()));
        return false;
    }

    auto getApi = reinterpret_cast<PFN_OnrRrGetApi>(GetProcAddress(module, "OnrRrGetApi"));
    OnrRrApi5 api5 {};
    OnrRrApi4& api4 = api5.v4;
    OnrRrApi3& api3 = api4.v3;
    OnrRrApi2& api2 = api3.v2;
    OnrRrApi& api = api2.v1;
    // ABI 5, 4, 3 or 2 when the runtime has it, otherwise 1 (no NrOnDenoised)
    int32_t rc = getApi != nullptr ? getApi(ONR_RR_ABI_VERSION_5, &api) : ONR_RR_FAILED;
    const bool has5 = rc == ONR_RR_OK && api.abi_version == ONR_RR_ABI_VERSION_5 && api5.PrepareVkDevice && api5.FinishVkDevice &&
                      api5.RecordVk && api4.GetBackend && api4.Record && api4.RecordPost;
    if (!has5)
    {
        api5 = {};
        rc = getApi != nullptr ? getApi(ONR_RR_ABI_VERSION_4, &api) : ONR_RR_FAILED;
    }
    if (!has5 && (rc != ONR_RR_OK || api.abi_version != ONR_RR_ABI_VERSION_4 || !api4.GetBackend || !api4.Record || !api4.RecordPost))
    {
        api5 = {};
        rc = getApi != nullptr ? getApi(ONR_RR_ABI_VERSION_3, &api) : ONR_RR_FAILED;
    }
    if (rc != ONR_RR_OK || api.abi_version < ONR_RR_ABI_VERSION_3 || !api3.GetHopTiming)
    {
        api5 = {};
        rc = getApi != nullptr ? getApi(ONR_RR_ABI_VERSION_2, &api) : ONR_RR_FAILED;
    }
    if (rc != ONR_RR_OK || api.abi_version < ONR_RR_ABI_VERSION_2 || !api2.PrepareDenoised || !api2.EnqueueNetwork ||
        !api2.EnqueuePost)
    {
        api5 = {};
        rc = getApi != nullptr ? getApi(ONR_RR_ABI_VERSION, &api) : ONR_RR_FAILED;
    }

    if (rc != ONR_RR_OK ||
        api.abi_version < ONR_RR_ABI_VERSION || api.abi_version > ONR_RR_ABI_VERSION_5 ||
        !api.Check || !api.Create || !api.Destroy ||
        !api.SetMode || !api.RecordInputs || !api.Enqueue || !api.RecordOutput || !api.Cancel || !api.GetInfo ||
        !api.GetLastError)
    {
        s_reason = std::format("OpenNrRrRuntime.dll does not offer ABI {} (rc {}, abi {})", ONR_RR_ABI_VERSION, rc,
                               api.abi_version);
        LOG_ERROR("OpenRR: {}", s_reason);
        FreeLibrary(module);
        return false;
    }

    s_module = module;
    s_api = api;
    s_api2 = api2;
    s_getHopTiming = api3.GetHopTiming;
    s_getBackend = api4.GetBackend;
    s_record = api4.Record;
    s_setDeltaRate = reinterpret_cast<decltype(&OnrRrSetDeltaRate)>(GetProcAddress(module, "OnrRrSetDeltaRate"));
    s_setFastMode = reinterpret_cast<decltype(&OnrRrSetFastMode)>(GetProcAddress(module, "OnrRrSetFastMode"));
    s_getFastMode = reinterpret_cast<decltype(&OnrRrGetFastMode)>(GetProcAddress(module, "OnrRrGetFastMode"));
    s_setFp8 = reinterpret_cast<decltype(&OnrRrSetFp8)>(GetProcAddress(module, "OnrRrSetFp8"));
    s_getFp8 = reinterpret_cast<decltype(&OnrRrGetFp8)>(GetProcAddress(module, "OnrRrGetFp8"));
    s_recordPost = api4.RecordPost;
    if (has5)
        s_api5 = api5;
    LOG_INFO("OpenRR: runtime loaded from {} (ABI {})", wstring_to_string(path.wstring()), api.abi_version);
    return true;
}

// Where an nvngx_dlssd.dll to build the weights from may be, in order: a copy kept for them alone, OptiScaler's own
// lookup, the one the game loaded, the one beside the game's exe. Usable takes the first the runtime accepts: OptiScaler's
// lookup can name danielblnc's host copy, another version, while the game has the right one.
std::vector<std::pair<const char*, std::wstring>> DlssdCandidates()
{
    std::vector<std::pair<const char*, std::wstring>> found;
    const auto add = [&found](const char* source, const std::wstring& path)
    {
        for (const auto& [_, seen] : found)
            if (_wcsicmp(seen.c_str(), path.c_str()) == 0)
                return;
        found.emplace_back(source, path);
    };

    std::error_code ec;
    if (const auto own = Util::DllPath().parent_path() / L"opennr-rr" / L"nvngx_dlssd.dll"; std::filesystem::exists(own, ec))
        add("opennr-rr folder", own.wstring());

    if (const auto& known = State::Instance().NVNGX_DLSSD_Path; known.has_value())
        add("OptiScaler's nvngx_dlssd.dll path", known.value());

    if (HMODULE loaded = GetModuleHandleW(L"nvngx_dlssd.dll"); loaded != nullptr)
    {
        wchar_t path[MAX_PATH] {};
        if (GetModuleFileNameW(loaded, path, MAX_PATH) != 0)
            add("loaded by the game", path);
    }

    if (const auto beside = Util::ExePath().remove_filename() / L"nvngx_dlssd.dll"; std::filesystem::exists(beside, ec))
        add("beside the game's exe", beside.wstring());
    else if (auto deeper = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssd.dll"); deeper.has_value())
        add("under the game's exe", deeper->wstring());

    return found;
}

enum class DepthKind
{
    Unknown,
    Hardware,
    GameWritten,
};

// Same reading as FSR-RR: a depth-stencil resource cannot hold a linear distance, a render target or UAV the game
// filled may.
DepthKind ClassifyDepth(ID3D12Resource* depth)
{
    if (depth == nullptr)
        return DepthKind::Unknown;

    const D3D12_RESOURCE_DESC desc = depth->GetDesc();

    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0)
        return DepthKind::Hardware;

    switch (desc.Format)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DepthKind::Hardware;
    default:
        break;
    }

    if ((desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) != 0)
        return DepthKind::GameWritten;

    return DepthKind::Unknown;
}

// The matrix exactly as the game passed it; all zero when absent or not finite.
void ReadMatrix(const NVSDK_NGX_Parameter& params, const char* key, float (&out)[16])
{
    float* matrix = nullptr;

    if (params.Get(key, reinterpret_cast<void**>(&matrix)) != NVSDK_NGX_Result_Success || matrix == nullptr)
        return;

    for (int i = 0; i < 16; i++)
    {
        if (!std::isfinite(matrix[i]))
            return;
    }

    memcpy(out, matrix, sizeof(out));
}

// The DLSS-D inputs nvngx_dlssd.dll 310.9.1 reads as textures (its parameter table), for the inventory in the log.
constexpr const char* kNgxTextures[] = {
    "Color", "Output", "Depth", "MotionVectors", "ExposureTexture", "TransparencyMask", "GBuffer.Normals",
    "GBuffer.Roughness", "DLSS.Input.DiffuseAlbedo", "DLSS.Input.SpecularAlbedo", "DLSSD.Alpha", "DLSSD.OutputAlpha",
    "DLSSD.ReflectedAlbedo", "DLSSD.ColorBeforeParticles", "DLSSD.ColorAfterParticles", "DLSSD.ColorBeforeTransparency",
    "DLSSD.ColorAfterTransparency", "DLSSD.ColorBeforeFog", "DLSSD.ColorAfterFog",
    "DLSSD.ScreenSpaceSubsurfaceScatteringGuide", "DLSSD.DepthOfFieldGuide", "DLSSD.DiffuseHitDistance",
    "DLSSD.SpecularHitDistance", "DLSSD.DiffuseRayDirection", "DLSSD.SpecularRayDirection",
    "DLSSD.DiffuseRayDirectionHitDistance", "DLSSD.SpecularRayDirectionHitDistance", "DLSS.TransparencyLayer",
    "DLSS.TransparencyLayerOpacity", "DLSS.TransparencyLayerMvecs", "DLSS.DisocclusionMask", "DLSSD.ResponsivityMask",
    "GBuffer.IndirectAlbedo", "GBuffer.SpecularMvec", "GBuffer.Emissive", "MotionVectors3D", "IsParticleMask",
    "AnimatedTextureMask", "DepthHighRes", "Position.ViewSpace", "RayTracingHitDistance", "MotionVectorsReflection",
    "DLSS.Input.Reduce.Ghost.Mask", "DLSS.Input.Bias.Current.Color.Mask", "GBuffer.Attrib.15",
};

constexpr const char* kNgxScalars[] = {
    "DLSS.Pre.Exposure", "DLSS.Exposure.Scale", "MV.Scale.X", "MV.Scale.Y", "Jitter.Offset.X", "Jitter.Offset.Y",
    "DLSS.Input.Bias.Current.Color.Subrect.Base.X", "DLSS.Input.Bias.Current.Color.Subrect.Base.Y",
    "DLSS.TransparencyLayer.Subrect.Base.X", "DLSS.TransparencyLayer.Subrect.Base.Y", "TonemapperType",
    "DLSSD.ResponsivityMask.Subrect.Base.X", "DLSSD.ResponsivityMask.Subrect.Base.Y",
    "DLSS.TransparencyLayerMvecs.Subrect.Base.X", "DLSS.TransparencyLayerMvecs.Subrect.Base.Y",
};

// Which DLSS-D inputs the game passes: the first frame, then whenever the set changes (at most 20 times), with each
// texture's size and format, and every 3000 frames how often the transparency layer was there.
void LogNgxInputs(const NVSDK_NGX_Parameter& params, ID3D12Resource* layer)
{
    static std::string lastSet;
    static uint32_t logs = 0;
    static uint64_t frames = 0, withLayer = 0;

    ++frames;
    withLayer += layer != nullptr;
    if (frames % 3000 == 0)
        LOG_INFO("OpenRR: transparency layer in {} of the last 3000 frames", std::exchange(withLayer, 0));

    std::string set;
    std::vector<std::string> lines;
    for (const char* key : kNgxTextures)
    {
        ID3D12Resource* resource = nullptr;
        if (!TryGetNGXVoidPointer(params, key, resource) || resource == nullptr)
            continue;
        set += key;
        set += ';';
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        lines.push_back(std::format("{} {:p} {}x{} format {} flags {:#x}", key, (void*) resource, desc.Width,
                                    desc.Height, (int) desc.Format, (uint32_t) desc.Flags));
    }

    if (set == lastSet || logs >= 20)
        return;

    lastSet = set;
    ++logs;
    LOG_INFO("OpenRR: DLSS-D inputs at frame {} ({} textures):", frames, lines.size());
    for (const auto& line : lines)
        LOG_INFO("OpenRR:   {}", line);
    for (const char* key : kNgxScalars)
    {
        if (float value = 0; params.Get(key, &value) == NVSDK_NGX_Result_Success)
            LOG_INFO("OpenRR:   {} = {}", key, value);
        else if (int ivalue = 0; params.Get(key, &ivalue) == NVSDK_NGX_Result_Success)
            LOG_INFO("OpenRR:   {} = {}", key, ivalue);
    }
}

const char* StateName(uint32_t state)
{
    switch (state)
    {
    case ONR_RR_STATE_BUILDING:
        return "building";
    case ONR_RR_STATE_READY:
        return "ready";
    case ONR_RR_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}
} // namespace

namespace OpenNrRr
{
bool Usable(LUID adapter)
{
    std::lock_guard lock(s_mutex);

    if (!s_blocked.empty())
        return false;

    if (s_checked && IsEqualLUID(adapter, s_luid))
        return s_usable;

    s_checked = true;
    s_luid = adapter;
    s_usable = false;

    if (IdentifyGpu::getPrimaryGpu().vendorId != VendorId::AMD)
    {
        s_reason = "needs an AMD RDNA4 GPU";
        return false;
    }

    if (!Load())
        return false;

    const auto candidates = DlssdCandidates();
    int32_t rc = ONR_RR_OK;
    std::string firstReason;
    s_dlssdPath.clear();
    // none found still asks the runtime, for the answer about the GPU
    for (size_t i = 0; i < (std::max)(candidates.size(), size_t(1)); ++i)
    {
        const wchar_t* path = candidates.empty() ? nullptr : candidates[i].second.c_str();
        if (path != nullptr)
            LOG_INFO("OpenRR: nvngx_dlssd.dll {} ({})", candidates[i].first, wstring_to_string(path));
        {
            ScopedSkipVulkanHooks skipVulkanHooks {};
            ScopedCreatingD3DDevice creatingDevice {};
            ScopedSkipSpoofingGlobal skipSpoofing {};
            rc = s_api.Check(&adapter, path);
        }
        if (rc == ONR_RR_OK)
        {
            if (path != nullptr)
                s_dlssdPath = path;
            break;
        }
        auto why = RuntimeError();
        if (why.empty())
            why = std::format("not supported here (rc {})", rc);
        LOG_INFO("OpenRR: Check rc={}: {}", rc, why);
        if (firstReason.empty())
            firstReason = why;
    }

    if (rc != ONR_RR_OK)
    {
        s_reason = firstReason;
        return false;
    }

    if (s_dlssdPath.empty())
    {
        s_reason = "nvngx_dlssd.dll not found, the weights are built from it";
        LOG_INFO("OpenRR: {}", s_reason);
        return false;
    }

    s_reason.clear();
    s_usable = true;
    LOG_INFO("OpenRR: Check passed on adapter {:08X}:{:08X}", (uint32_t) adapter.HighPart, adapter.LowPart);
    return true;
}

bool Checked()
{
    std::lock_guard lock(s_mutex);
    return s_checked || !s_blocked.empty();
}

std::string Reason()
{
    std::lock_guard lock(s_mutex);
    return s_blocked.empty() ? s_reason : s_blocked;
}

void Block(const std::string& why)
{
    std::lock_guard lock(s_mutex);
    if (s_blocked.empty())
        s_blocked = "stopped this session: " + why;
    s_usable = false;
}

bool Info(OnrRrInfo& info)
{
    std::lock_guard lock(s_mutex);

    if (s_infoContext == nullptr)
        return false;

    info = {};
    info.size = sizeof(info);
    return s_api.GetInfo(s_infoContext, &info) == ONR_RR_OK;
}

bool SplitReady()
{
    return DlssNr::Backend::SubmissionHooksWanted() && DlssNr::Submission::Hooks::IsArmed() &&
           DlssNr::Submission::Hooks::ProxyWrapEnabled();
}

bool SplitComing() { return DlssNr::Backend::SubmissionHooksWanted() && DlssNr::Submission::Hooks::IsArmed(); }

std::vector<const char*> NgxTextureKeys() { return { std::begin(kNgxTextures), std::end(kNgxTextures) }; }
std::vector<const char*> NgxScalarKeys() { return { std::begin(kNgxScalars), std::end(kNgxScalars) }; }

const OnrRrApi5* Api5()
{
    std::lock_guard lock(s_mutex);
    return Load() && s_api5.RecordVk != nullptr ? &s_api5 : nullptr;
}

std::wstring DlssdPath()
{
    std::lock_guard lock(s_mutex);
    return s_dlssdPath;
}

std::string LastError() { return RuntimeError(); }

bool DlssdFound()
{
    return !DlssdCandidates().empty();
}

void SetInfoContext(void* context)
{
    std::lock_guard lock(s_mutex);
    s_infoContext = context;
}

void ClearInfoContext(void* context)
{
    std::lock_guard lock(s_mutex);
    if (s_infoContext == context)
        s_infoContext = nullptr;
}

void ApplyFastMode(void* context)
{
    if (s_setFastMode == nullptr || context == nullptr)
        return;
    // 2 is the runtime's experimental level; one without it takes any non-zero value as New
    const std::string mode = Config::Instance()->OpenNrRrMode.value_or_default();
    s_setFastMode(context, mode == "legacy" ? 0 : mode == "fast" ? 2 : 1);

    if (s_setFp8 != nullptr)
        s_setFp8(context, Config::Instance()->OpenNrRrFp8.value_or_default() ? 1 : 0);
}

int FastModeState()
{
    if (s_getFastMode == nullptr)
        return -1;
    return s_getFastMode(s_infoContext);
}

int Fp8State()
{
    if (s_getFp8 == nullptr)
        return -1;
    return s_getFp8(s_infoContext);
}

std::string DenoisedNrStatus()
{
    std::string why;
    const auto mode = Config::Instance()->OpenNrRrNrOnDenoised.value_or_default();
    if (_stricmp(mode.c_str(), "display") != 0)
        return {};
    if (EditFromConfig(&why) == ONR_RR_EDIT_OFF)
        return "not running: " + why;
    auto& d = Denoised();
    std::lock_guard lock(d.mutex);
    return d.status;
}
feature_version RuntimeVersion()
{
    feature_version v;

    if (s_module == nullptr)
        return v;

    HRSRC res = FindResourceW(s_module, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
    HGLOBAL data = res != nullptr ? LoadResource(s_module, res) : nullptr;
    auto bytes = data != nullptr ? static_cast<const uint8_t*>(LockResource(data)) : nullptr;
    const DWORD size = res != nullptr ? SizeofResource(s_module, res) : 0;

    for (DWORD i = 0; bytes != nullptr && i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4)
    {
        auto info = reinterpret_cast<const VS_FIXEDFILEINFO*>(bytes + i);

        if (info->dwSignature != VS_FFI_SIGNATURE)
            continue;

        v = { HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS) };
        break;
    }

    // 0.1.1 shipped saying 0.1.0 there; the fast mode came with it
    if (v.major == 0 && v.minor == 1 && v.patch == 0 && GetProcAddress(s_module, "OnrRrSetFastMode") != nullptr)
        v.patch = 1;

    return v;
}

} // namespace OpenNrRr

OpenNrRrFeatureDx12::OpenNrRrFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters)
{
    // The network writes the display-size frame itself, so the dual-feature split never applies to it.
    MarkEnlargementStage();

    std::lock_guard lock(s_mutex);
    _moduleLoaded = Load();
}

// The runtime's own release once it loaded; before that, the one this build ships with.
feature_version OpenNrRrFeatureDx12::Version()
{
    const auto v = OpenNrRr::RuntimeVersion();
    return v.major != 0 || v.minor != 0 || v.patch != 0 ? v : feature_version { 0, 1, 3 };
}

OpenNrRrFeatureDx12::~OpenNrRrFeatureDx12()
{
    if (State::Instance().isShuttingDown || _context == nullptr)
        return;

    DropStaleJob();

    {
        std::lock_guard lock(s_mutex);
        if (s_infoContext == _context)
            s_infoContext = nullptr;
    }

    s_api.Destroy(_context);
    _context = nullptr;
}

bool OpenNrRrFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (IsInited())
        return true;

    if (!ModuleLoaded() || Device == nullptr)
        return false;

    if (int value; InParameters->Get(NVSDK_NGX_Parameter_Use_HW_Depth, &value) == NVSDK_NGX_Result_Success)
    {
        _hasNGXDepthType = true;
        _ngxHWDepth = value == NVSDK_NGX_DLSS_Depth_Type_HW;
        s_ngxDepthTypeSeen = true;
        s_ngxReportedHWDepth = _ngxHWDepth;
    }
    else if (s_ngxDepthTypeSeen)
    {
        _hasNGXDepthType = true;
        _ngxHWDepth = s_ngxReportedHWDepth;
    }

    // Before SetInitParameters, which stores the depth direction XeFG starts with; each frame refines it
    _fgLinearDepth = Config::Instance()->FfxDenoiserHardwareDepth.has_value()
                         ? !Config::Instance()->FfxDenoiserHardwareDepth.value()
                         : _hasNGXDepthType && !_ngxHWDepth;

    _initParameters = SetInitParameters(InParameters);

    if (!_initParameters)
        return false;

    if (!OpenNrRr::Usable(Device->GetAdapterLuid()))
    {
        LOG_ERROR("OpenRR: not usable: {}", OpenNrRr::Reason());
        return false;
    }

    if (int value; InParameters->Get(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, &value) == NVSDK_NGX_Result_Success)
    {
        if (value == NVSDK_NGX_DLSS_Roughness_Mode_Packed)
            _roughnessPacked = 1;
        else if (value == NVSDK_NGX_DLSS_Roughness_Mode_Unpacked)
            _roughnessPacked = 0;
    }

    _maxRenderWidth = RenderWidth();
    _maxRenderHeight = RenderHeight();

    const auto dataDir = (Util::DllPath().parent_path() / L"opennr-rr").wstring();
    std::wstring dlssd;
    {
        std::lock_guard lock(s_mutex);
        dlssd = s_dlssdPath;
    }

    OnrRrCreateInfo info {};
    info.size = sizeof(info);
    info.mode = ONR_RR_MODE_FAST;
    info.d3d12_device = Device;
    info.max_render_width = _maxRenderWidth;
    info.max_render_height = _maxRenderHeight;
    info.output_width = DisplayWidth();
    info.output_height = DisplayHeight();
    info.dlssd_path = dlssd.c_str();
    info.data_dir = dataDir.c_str();
    const auto backend = Config::Instance()->OpenNrRrBackend.value_or_default();
    info.backend = _stricmp(backend.c_str(), "vulkan") == 0  ? ONR_RR_BACKEND_CHOICE_VULKAN
                   : _stricmp(backend.c_str(), "d3d12") == 0 ? ONR_RR_BACKEND_CHOICE_D3D12
                                                             : ONR_RR_BACKEND_CHOICE_AUTO;

    int32_t rc;
    {
        ScopedSkipVulkanHooks skipVulkanHooks {};
        ScopedCreatingD3DDevice creatingDevice {};
        ScopedSkipSpoofingGlobal skipSpoofing {};
        rc = s_api.Create(&info, &_context);
    }

    if (rc != ONR_RR_OK || _context == nullptr)
    {
        const auto error = RuntimeError();
        LOG_ERROR("OpenRR: Create rc={}: {}", rc, error);
        OpenNrRr::Block(std::format("Create failed ({})", error));
        _context = nullptr;
        return false;
    }

    {
        std::lock_guard lock(s_mutex);
        s_infoContext = _context;
    }

    uint32_t running = ONR_RR_BACKEND_VULKAN;
    if (s_getBackend != nullptr)
        s_getBackend(_context, &running);
    LOG_INFO("OpenRR: created, render {}x{} -> output {}x{}, backend {} (asked: {}), weights from {}",
             _maxRenderWidth, _maxRenderHeight, DisplayWidth(), DisplayHeight(),
             running == ONR_RR_BACKEND_D3D12 ? "D3D12" : "Vulkan", backend, wstring_to_string(dlssd));

    if (!LowResMV())
        LOG_WARN("OpenRR: the game declares display-size motion vectors; the runtime reads them at render size");

    SetInit(true);
    return true;
}

// The previous frame's job is still armed when its list was reset, released or not yet executed. It can no longer be
// enqueued, so the runtime drops it. A job the between slot already took is left alone.
void OpenNrRrFeatureDx12::DropStaleJob()
{
    if (_job == nullptr)
        return;

    bool stillArmed = false;
    {
        auto& pending = LmxxfCut::Pending();
        std::lock_guard lock(pending.mutex);
        if (pending.job == _job && pending.session == this && pending.targetList == _jobList)
        {
            stillArmed = true;
            pending.session = nullptr;
            pending.job = nullptr;
            pending.enqueueHip = nullptr;
            pending.getLastError = nullptr;
            pending.targetList = nullptr;
            pending.expectedQueue = nullptr;
        }
    }

    if (stillArmed)
    {
        if (!_jobIsToken)
            s_api.Cancel(_context, _job);

        auto& d = Denoised();
        bool hop = false;
        {
            std::lock_guard lock(d.mutex);
            if (d.armedJob == _job)
            {
                hop = true;
                d.armedJob = nullptr;
            }
        }
        if (hop)
            d.mz->CancelOwnLists();

        static uint32_t dropped = 0;
        if (++dropped <= 10 || dropped % 300 == 0)
            LOG_WARN("OpenRR: the list of the previous frame never ran its between slot; job cancelled (#{})",
                     dropped);
    }

    _job = nullptr;
    _jobList = nullptr;
}

void OpenNrRrFeatureDx12::FallBack(const std::string& why)
{
    if (_fellBack)
        return;

    _fellBack = true;
    LOG_ERROR("OpenRR: {}; switching Ray Reconstruction back to the previous provider", why);
    // The game often shows its error and exits before the next Present, so say what DRED saw here.
    if (Device != nullptr && FAILED(Device->GetDeviceRemovedReason()))
        Util::GetDeviceRemovedReason(Device);
    ImGui::InsertNotification(
        { ImGuiToastType::Warning, 10000, "OpenRR stopped, Ray Reconstruction goes back to FSR-RR (see the log)" });

    OpenNrRr::Block(why);

    auto& state = State::Instance();
    state.newBackend = Upscaler::FSR_RR;
    state.changeBackend[Handle()->Id] = true;
}

bool OpenNrRrFeatureDx12::FillFrame(const NVSDK_NGX_Parameter& params, OnrRrFrame& frame)
{
    const auto& cfg = *Config::Instance();
    const auto& state = State::Instance();

    frame = {};
    frame.size = sizeof(frame);

    unsigned int width = 0;
    unsigned int height = 0;
    GetRenderResolution(&params, &width, &height);
    frame.render_width = std::min<uint32_t>(width, _maxRenderWidth);
    frame.render_height = std::min<uint32_t>(height, _maxRenderHeight);

    params.Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &frame.jitter_x);
    params.Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &frame.jitter_y);
    if (!std::isfinite(frame.jitter_x))
        frame.jitter_x = 0.0f;
    if (!std::isfinite(frame.jitter_y))
        frame.jitter_y = 0.0f;

    // A missing, non-finite or zero component counts as one, as NGX does; a negative one is the game's axis.
    if (params.Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.mv_scale_x) != NVSDK_NGX_Result_Success ||
        !std::isfinite(frame.mv_scale_x) || frame.mv_scale_x == 0.0f)
        frame.mv_scale_x = 1.0f;
    if (params.Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.mv_scale_y) != NVSDK_NGX_Result_Success ||
        !std::isfinite(frame.mv_scale_y) || frame.mv_scale_y == 0.0f)
        frame.mv_scale_y = 1.0f;

    ReadMatrix(params, NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, frame.world_to_view);
    ReadMatrix(params, NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, frame.view_to_clip);

    // The states FSR-RR assumes: shader readable inputs and a UAV output, or the barrier overrides from the config.
    const bool unreal =
        state.NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL || state.gameQuirks & GameQuirk::ForceUnrealEngine;
    const uint32_t read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const uint32_t colorState = cfg.ColorResourceBarrier.value_or(unreal ? D3D12_RESOURCE_STATE_RENDER_TARGET : read);
    const uint32_t motionState = cfg.MVResourceBarrier.value_or(unreal ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : read);
    const uint32_t depthState = cfg.DepthResourceBarrier.value_or(read);
    const uint32_t outputState = cfg.OutputResourceBarrier.value_or(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const auto texture = [&params](const char* key, const char* baseX, const char* baseY, uint32_t textureState)
    {
        OnrRrTexture t {};
        ID3D12Resource* resource = nullptr;

        if (TryGetNGXVoidPointer(params, key, resource))
        {
            t.resource = resource;
            t.state = textureState;

            if (baseX != nullptr)
            {
                params.Get(baseX, &t.x);
                params.Get(baseY, &t.y);
            }
        }

        return t;
    };

    frame.color = texture(NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                          NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, colorState);
    frame.diffuse_albedo =
        texture(NVSDK_NGX_Parameter_DiffuseAlbedo, NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X,
                NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y, read);
    frame.specular_albedo =
        texture(NVSDK_NGX_Parameter_SpecularAlbedo, NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X,
                NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y, read);
    frame.normals = texture(NVSDK_NGX_Parameter_GBuffer_Normals, NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X,
                            NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y, read);
    frame.roughness =
        texture(NVSDK_NGX_Parameter_GBuffer_Roughness, NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X,
                NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y, read);
    frame.depth = texture(NVSDK_NGX_Parameter_Depth, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                          NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, depthState);
    frame.motion = texture(NVSDK_NGX_Parameter_MotionVectors, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                           NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, motionState);
    frame.specular_hit_distance = texture(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance,
                                          NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X,
                                          NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y, read);
    frame.specular_ray_direction_hit_distance =
        texture(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance,
                NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_X,
                NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_Y, read);
    // Output may be OptiScaler's own buffer ahead of RCAS or output scaling, so it is written from its origin.
    frame.output = texture(NVSDK_NGX_Parameter_Output, nullptr, nullptr, outputState);
    // The transparency layer NGX composites over its output; nvngx_dlssd.dll takes ColorBeforeParticles for it when
    // given (Cyberpunk's particles and other transparent effects), and the layer's subrect for both.
    frame.transparency_layer =
        texture(NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles, "DLSS.TransparencyLayer.Subrect.Base.X",
                "DLSS.TransparencyLayer.Subrect.Base.Y", read);
    if (frame.transparency_layer.resource == nullptr)
        frame.transparency_layer = texture("DLSS.TransparencyLayer", "DLSS.TransparencyLayer.Subrect.Base.X",
                                           "DLSS.TransparencyLayer.Subrect.Base.Y", read);
    frame.transparency_opacity =
        texture("DLSS.TransparencyLayerOpacity", "DLSS.TransparencyLayerOpacity.Subrect.Base.X",
                "DLSS.TransparencyLayerOpacity.Subrect.Base.Y", read);
    // Two more guides nvngx_dlssd.dll 310.9.1 feeds its first encoder (RE Requiem passes both): the disocclusion mask,
    // and the SSS guide, which ColorBeforeFog replaces when given; NGX reads the latter at the colour's subrect.
    frame.disocclusion_mask = texture("DLSS.DisocclusionMask", "DLSS.DisocclusionMask.Subrect.Base.X",
                                      "DLSS.DisocclusionMask.Subrect.Base.Y", read);
    frame.sss_guide = texture("DLSSD.ColorBeforeFog", nullptr, nullptr, read);
    if (frame.sss_guide.resource == nullptr)
        frame.sss_guide = texture("DLSSD.ScreenSpaceSubsurfaceScatteringGuide", nullptr, nullptr, read);
    frame.sss_guide.x = frame.color.x;
    frame.sss_guide.y = frame.color.y;
    // The rest nvngx_dlssd.dll feeds rr2: the responsivity mask (its own subrect), ColorBeforeTransparency and the depth
    // of field guide (at the colour's subrect), the reflections' motion vectors (at the motion vectors' subrect), and
    // the transparency layer's own motion vectors (its own subrect).
    frame.responsivity_mask = texture("DLSSD.ResponsivityMask", "DLSSD.ResponsivityMask.Subrect.Base.X",
                                      "DLSSD.ResponsivityMask.Subrect.Base.Y", read);
    frame.color_before_transparency = texture(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency, nullptr, nullptr, read);
    frame.color_before_transparency.x = frame.color.x;
    frame.color_before_transparency.y = frame.color.y;
    frame.depth_of_field_guide = texture(NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide, nullptr, nullptr, read);
    frame.depth_of_field_guide.x = frame.color.x;
    frame.depth_of_field_guide.y = frame.color.y;
    frame.specular_motion = texture(NVSDK_NGX_Parameter_GBuffer_SpecularMvec, nullptr, nullptr, read);
    frame.specular_motion.x = frame.motion.x;
    frame.specular_motion.y = frame.motion.y;
    frame.transparency_motion = texture("DLSS.TransparencyLayerMvecs", "DLSS.TransparencyLayerMvecs.Subrect.Base.X",
                                        "DLSS.TransparencyLayerMvecs.Subrect.Base.Y", read);
    // diagnostic switches from the menu, applied on this frame
    const bool useMask = cfg.OpenNrRrUseDisocclusionMask.value_or_default();
    const bool useSss = cfg.OpenNrRrUseSssGuide.value_or_default();
    const bool invertMask = cfg.OpenNrRrInvertDisocclusionMask.value_or_default();
    const bool clampMask = cfg.OpenNrRrClampDisocclusionMask.value_or_default();
    const float maskScale = cfg.OpenNrRrDisocclusionMaskScale.value_or_default();
    const bool useResp = cfg.OpenNrRrUseResponsivityMask.value_or_default();
    const bool useCbt = cfg.OpenNrRrUseColorBeforeTransparency.value_or_default();
    const bool useDof = cfg.OpenNrRrUseDepthOfFieldGuide.value_or_default();
    const bool useSmv = cfg.OpenNrRrUseReflectionMotionVectors.value_or_default();
    const bool useLayerExtras = cfg.OpenNrRrUseLayerOpacityAndMotion.value_or_default();
    if (const int switches = int(useMask) | int(useSss) << 1 | int(invertMask) << 2 | int(clampMask) << 3 |
                             int(useResp) << 4 | int(useCbt) << 5 | int(useDof) << 6 | int(useSmv) << 7 |
                             int(useLayerExtras) << 8;
        switches != _loggedSwitches || maskScale != _loggedMaskScale)
    {
        _loggedSwitches = switches;
        _loggedMaskScale = maskScale;
        LOG_INFO("OpenRR: use disocclusion mask {}, scale it by {}, invert it {}, clamp it to 0..1 {}, use SSS guide {}, "
                 "use responsivity mask {}, colour before transparency {}, depth of field guide {}, reflection motion "
                 "vectors {}, transparency layer opacity and motion {}",
                 useMask, maskScale, invertMask, clampMask, useSss, useResp, useCbt, useDof, useSmv, useLayerExtras);
    }
    frame.disocclusion_mask_scale = maskScale > 0.0f ? maskScale : 1.0f;
    if (!useMask)
        frame.disocclusion_mask = {};
    if (!useSss)
        frame.sss_guide = {};
    if (!useResp)
        frame.responsivity_mask = {};
    if (!useCbt)
        frame.color_before_transparency = {};
    if (!useDof)
        frame.depth_of_field_guide = {};
    if (!useSmv)
        frame.specular_motion = {};
    if (!useLayerExtras)
        frame.transparency_opacity = frame.transparency_motion = {};
    LogNgxInputs(params, static_cast<ID3D12Resource*>(frame.transparency_layer.resource));

    if (frame.color.resource == nullptr || frame.depth.resource == nullptr || frame.motion.resource == nullptr ||
        frame.output.resource == nullptr)
    {
        FallBack(std::format("the game gave no {}", frame.color.resource == nullptr    ? "colour"
                                                    : frame.depth.resource == nullptr  ? "depth"
                                                    : frame.motion.resource == nullptr ? "motion vectors"
                                                                                       : "output"));
        return false;
    }

    static bool guidesLogged = false;
    if (!guidesLogged)
    {
        guidesLogged = true;
        LOG_INFO("OpenRR: guides diffuse albedo {}, specular albedo {}, normals {}, roughness {}, specular hit "
                 "distance {}, ray direction + hit distance {} (not read), transparency layer {}, its opacity {}, its "
                 "motion vectors {}, disocclusion mask {}, SSS guide {}, responsivity mask {}, colour before "
                 "transparency {}, depth of field guide {}, reflection motion vectors {}",
                 frame.diffuse_albedo.resource != nullptr, frame.specular_albedo.resource != nullptr,
                 frame.normals.resource != nullptr, frame.roughness.resource != nullptr,
                 frame.specular_hit_distance.resource != nullptr,
                 frame.specular_ray_direction_hit_distance.resource != nullptr,
                 frame.transparency_layer.resource != nullptr, frame.transparency_opacity.resource != nullptr,
                 frame.transparency_motion.resource != nullptr, frame.disocclusion_mask.resource != nullptr,
                 frame.sss_guide.resource != nullptr, frame.responsivity_mask.resource != nullptr,
                 frame.color_before_transparency.resource != nullptr, frame.depth_of_field_guide.resource != nullptr,
                 frame.specular_motion.resource != nullptr);
    }

    // Without the create-time mode, the first frame settles it: no roughness texture means it rides in normals.w.
    if (_roughnessPacked < 0)
    {
        _roughnessPacked = frame.roughness.resource == nullptr ? 1 : 0;
        LOG_INFO("OpenRR: roughness mode not declared, using {}", _roughnessPacked ? "packed" : "separate");
    }

    // Hardware or linear depth, decided as FSR-RR does: its HardwareDepth override, then a depth-stencil resource,
    // then the NGX declaration, then a game-written target as linear, otherwise hardware.
    bool hardwareDepth;
    const DepthKind kind = ClassifyDepth(static_cast<ID3D12Resource*>(frame.depth.resource));

    if (cfg.FfxDenoiserHardwareDepth.has_value())
        hardwareDepth = cfg.FfxDenoiserHardwareDepth.value();
    else if (kind == DepthKind::Hardware)
        hardwareDepth = true;
    else if (_hasNGXDepthType)
        hardwareDepth = _ngxHWDepth;
    else
        hardwareDepth = kind != DepthKind::GameWritten;

    _fgLinearDepth = !hardwareDepth;

    if (static_cast<int>(hardwareDepth) != _loggedHWDepth)
    {
        _loggedHWDepth = hardwareDepth;
        LOG_INFO("OpenRR: depth is {} ({}), {}", hardwareDepth ? "hardware" : "linear",
                 _hasNGXDepthType ? (_ngxHWDepth ? "NGX says hardware" : "NGX says linear") : "NGX says nothing",
                 DepthInverted() ? "reversed Z" : "standard Z");
    }

    if (uint32_t reset = 0; params.Get(NVSDK_NGX_Parameter_Reset, &reset) == NVSDK_NGX_Result_Success && reset == 1)
        frame.flags |= ONR_RR_FRAME_RESET;
    if (hardwareDepth)
        frame.flags |= ONR_RR_FRAME_HW_DEPTH;
    if (DepthInverted())
        frame.flags |= ONR_RR_FRAME_DEPTH_INVERTED;
    if (JitteredMV())
        frame.flags |= ONR_RR_FRAME_MV_JITTERED;
    if (_roughnessPacked == 1)
        frame.flags |= ONR_RR_FRAME_ROUGHNESS_PACKED;
    if (invertMask)
        frame.flags |= ONR_RR_FRAME_DM_INVERT;
    if (clampMask)
        frame.flags |= ONR_RR_FRAME_DM_CLAMP;

    return true;
}

bool OpenNrRrFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (!IsInited() || _context == nullptr || _fellBack)
        return false;

    DropStaleJob();

    if (const int32_t rc = s_enqueueRc.exchange(ONR_RR_OK, std::memory_order_acq_rel); rc != ONR_RR_OK)
    {
        FallBack(std::format("Enqueue rc={}: {}", rc, LmxxfCut::LastEnqueueDiagnostic().error.data()));
        return false;
    }

    if (_frameCount % 30 == 0)
    {
        OnrRrInfo info {};
        info.size = sizeof(info);

        if (s_api.GetInfo(_context, &info) == ONR_RR_OK)
        {
            if (_frameCount % 300 == 0)
                LOG_INFO("OpenRR: {} ({}), {} network frames, GPU {:.2f} ms (avg {:.2f}), {}x{} -> {}x{}",
                         StateName(info.state), info.message, info.frames, info.gpu_ms_last, info.gpu_ms_average,
                         info.render_width, info.render_height, info.output_width, info.output_height);

            if (info.state == ONR_RR_STATE_FAILED)
            {
                FallBack(std::format("the runtime failed: {}", info.message));
                return false;
            }
        }
    }

    // D3D12 backend: the frame goes straight into the game's list, no split (a hop for NrOnDenoised still splits it)
    if (uint32_t backend = ONR_RR_BACKEND_VULKAN; s_getBackend != nullptr &&
                                                  s_getBackend(_context, &backend) == ONR_RR_OK && backend == ONR_RR_BACKEND_D3D12)
        return EvaluateD3d12(InCommandList, InParameters);

    // The proxy under any layer that wraps the game's list (ReShade as dxgi.dll): the split and the between slot
    // know only that one.
    DlssNr::Submission::ILogicalCommandList* logical = nullptr;

    if (FAILED(InCommandList->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                             reinterpret_cast<void**>(&logical))) ||
        logical == nullptr)
    {
        FallBack("the game's command list is not one OptiScaler can split (the submission proxies are off)");
        return false;
    }

    ID3D12GraphicsCommandList* list = nullptr;
    logical->QueryInterface(IID_PPV_ARGS(&list));

    if (list != nullptr)
        list->Release(); // the game holds the list for this evaluate

    if (list == nullptr || logical->IsSplitIneligible())
    {
        FallBack(std::format("the command list cannot be split here ({})", logical->SplitRejectionReason()));
        logical->Release();
        return false;
    }

    OnrRrFrame frame {};

    if (!FillFrame(*InParameters, frame))
    {
        logical->Release();
        return false;
    }

    if (s_setDeltaRate != nullptr)
        s_setDeltaRate(_context, std::clamp(Config::Instance()->OpenNrRrNrDeltaRate.value_or_default(), .01f, 1.f));
    OpenNrRr::ApplyFastMode(_context);

    void* job = nullptr;

    if (const int32_t rc = s_api.RecordInputs(_context, &frame, list, &job); rc != ONR_RR_OK || job == nullptr)
    {
        FallBack(std::format("RecordInputs rc={}: {}", rc, RuntimeError()));
        logical->Release();
        return false;
    }

    if (const HRESULT hr = LmxxfCut::TrySplitAtEvaluate(list); FAILED(hr) || hr == S_FALSE)
    {
        s_api.Cancel(_context, job);
        FallBack(std::format("the split was refused (hr {:#x}, {})", (uint32_t) hr, logical->SplitRejectionReason()));
        logical->Release();
        return false;
    }

    logical->Release();

    if (const int32_t rc = s_api.RecordOutput(_context, job, list); rc != ONR_RR_OK)
    {
        s_api.Cancel(_context, job);
        FallBack(std::format("RecordOutput rc={}: {}", rc, RuntimeError()));
        return false;
    }

    if (s_api2.PrepareDenoised != nullptr)
        RecordDenoisedNr(*InParameters, frame, job, nullptr);

    LmxxfCut::SetPendingEnqueue(this, job, &OpenNrRrFeatureDx12::Between, s_api.GetLastError, list);
    LmxxfCut::ArmBetweenSlot();
    _job = job;
    _jobIsToken = false;
    _jobList = list;

    _hasColor = true;
    _hasDepth = true;
    _hasMV = true;
    _hasOutput = true;
    _frameCount++;
    return true;
}

// D3D12 backend (ABI 4): Record puts the whole frame into the game's list. With NrOnDenoised it stops at the denoised
// image; the list is split there, mochizuki's lists run at the split (BetweenD3d12) and RecordPost puts the rest
// into the second half. Without the split the hop is skipped and the frame is done on the same list.
bool OpenNrRrFeatureDx12::EvaluateD3d12(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    OnrRrFrame frame {};

    if (!FillFrame(*InParameters, frame))
        return false;

    if (s_setDeltaRate != nullptr)
        s_setDeltaRate(_context, std::clamp(Config::Instance()->OpenNrRrNrDeltaRate.value_or_default(), .01f, 1.f));
    OpenNrRr::ApplyFastMode(_context);

    std::string why;
    uint32_t edit = s_api2.PrepareDenoised != nullptr ? EditFromConfig(&why) : ONR_RR_EDIT_OFF;
    ID3D12GraphicsCommandList* list = InCommandList;
    DlssNr::Submission::ILogicalCommandList* logical = nullptr;

    if (edit != ONR_RR_EDIT_OFF)
    {
        // the split and the between slot know only OptiScaler's proxy, under any layer that wraps the list
        ID3D12GraphicsCommandList* proxied = nullptr;
        if (SUCCEEDED(InCommandList->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                                    reinterpret_cast<void**>(&logical))) &&
            logical != nullptr && !logical->IsSplitIneligible() &&
            SUCCEEDED(logical->QueryInterface(IID_PPV_ARGS(&proxied))) && proxied != nullptr)
        {
            proxied->Release(); // the game holds the list for this evaluate
            list = proxied;
        }
        else
        {
            edit = ONR_RR_EDIT_OFF;
        }

        if (logical != nullptr)
            logical->Release();
    }

    OnrRrDenoisedImage image {};
    image.size = sizeof(image);
    // Half floats: half the bytes for every copy of the hop, and a format mochizuki takes as a game's colour.
    image.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    void* job = nullptr;
    // edit is off when the list cannot be split
    const bool onOutput = edit != ONR_RR_EDIT_OFF && OnOutputFromConfig(frame);
    const uint32_t hop = onOutput ? uint32_t(ONR_RR_EDIT_OFF) : edit;

    if (const int32_t rc = s_record(_context, &frame, list, hop, hop != ONR_RR_EDIT_OFF ? &image : nullptr, &job);
        rc != ONR_RR_OK)
    {
        FallBack(std::format("Record rc={}: {}", rc, RuntimeError()));
        return false;
    }

    if (onOutput)
    {
        // The whole frame is in the first half; the second half follows mochizuki's lists, which read and write the
        // output.
        const HRESULT hr = LmxxfCut::TrySplitAtEvaluate(list);

        if (SUCCEEDED(hr) && hr != S_FALSE)
        {
            auto& d = Denoised();
            void* token = reinterpret_cast<void*>(++d.outputJobs);
            RecordDenoisedNr(*InParameters, frame, token, nullptr, true);
            LmxxfCut::SetPendingEnqueue(this, token, &OpenNrRrFeatureDx12::BetweenD3d12, s_api.GetLastError, list);
            LmxxfCut::ArmBetweenSlot();
            _job = token;
            _jobIsToken = true;
            _jobList = list;
        }
        else
            LOG_WARN("OpenRR: the split for the NR on the output was refused (hr {:#x}); frame without NR", (uint32_t) hr);
    }
    else if (job != nullptr)
    {
        uint32_t format = 0;
        const HRESULT hr = LmxxfCut::TrySplitAtEvaluate(list);
        const bool split = SUCCEEDED(hr) && hr != S_FALSE;

        if (split)
            format = RecordDenoisedNr(*InParameters, frame, job, &image);
        else
            LOG_WARN("OpenRR: the split for the denoised image was refused (hr {:#x}); frame without NR", (uint32_t) hr);

        if (const int32_t rc = s_recordPost(_context, job, list, format); rc != ONR_RR_OK)
        {
            FallBack(std::format("RecordPost rc={}: {}", rc, RuntimeError()));
            return false;
        }

        if (split)
        {
            // the between slot runs mochizuki's lists (and learns the game's queue on the first frames)
            LmxxfCut::SetPendingEnqueue(this, job, &OpenNrRrFeatureDx12::BetweenD3d12, s_api.GetLastError, list);
            LmxxfCut::ArmBetweenSlot();
            _job = job;
            _jobIsToken = false;
            _jobList = list;
        }
    }

    _hasColor = true;
    _hasDepth = true;
    _hasMV = true;
    _hasOutput = true;
    _frameCount++;
    return true;
}

namespace
{
// the game's queue, from the first between slot: mochizuki's session for the denoised image is made on it
void LearnQueue(ID3D12CommandQueue* q)
{
    auto& d = Denoised();
    if (d.queue.load(std::memory_order_acquire) == nullptr)
    {
        q->AddRef();
        ID3D12CommandQueue* none = nullptr;
        if (!d.queue.compare_exchange_strong(none, q))
            q->Release();
    }
}
} // namespace

// The between slot of a D3D12 hop (submit thread): only mochizuki's lists, the network and the post are in the
// game's two halves already.
int32_t OpenNrRrFeatureDx12::BetweenD3d12(void* feature, void* job, void* queue)
{
    (void) feature;
    auto* q = static_cast<ID3D12CommandQueue*>(queue);
    LearnQueue(q);

    auto& d = Denoised();
    int slot = -1;
    {
        std::lock_guard lock(d.mutex);
        if (d.armedJob == job)
        {
            slot = d.armedSlot;
            d.armedJob = nullptr;
        }
    }

    if (slot >= 0)
    {
        auto& s = d.slots[slot];
        d.mz->SubmitOwnLists(q, s.producer, s.consumer, DlssNr::Submission::g_rawExecuteCommandLists);
        s.ran.store(true, std::memory_order_release);
        const uint64_t v = d.value.fetch_add(1) + 1;
        q->Signal(d.fence, v);
        s.busy.store(v, std::memory_order_release);
    }

    return ONR_RR_OK;
}

// The between slot (submit thread). With ABI 2 the network may stop at the denoised image: mochizuki's lists run
// there on the raw queue (this is inside the queue hook), then the post.
int32_t OpenNrRrFeatureDx12::Between(void* feature, void* job, void* queue)
{
    auto* self = static_cast<OpenNrRrFeatureDx12*>(feature);
    auto* q = static_cast<ID3D12CommandQueue*>(queue);
    int32_t rc;

    if (s_api2.EnqueueNetwork == nullptr)
    {
        rc = s_api.Enqueue(self->_context, job, queue);
    }
    else
    {
        auto& d = Denoised();
        LearnQueue(q);

        int slot = -1;
        uint32_t format = 0;
        {
            std::lock_guard lock(d.mutex);
            if (d.armedJob == job)
            {
                slot = d.armedSlot;
                format = d.armedFormat;
                d.armedJob = nullptr;
            }
        }

        const auto exec = DlssNr::Submission::g_rawExecuteCommandLists;
        const auto run = [&](ID3D12GraphicsCommandList* list)
        {
            ID3D12CommandList* l = list;
            exec(q, 1, &l);
        };
        auto* s = slot >= 0 ? &d.slots[slot] : nullptr;
        const bool timed = s != nullptr && s->timed;
        if (timed)
            run(s->stampPre);

        uint32_t exported = 0;
        rc = s_api2.EnqueueNetwork(self->_context, job, queue, &exported);
        uint32_t edited = 0;
        if (d.hopContext.exchange(self->_context) != self->_context)
            d.hops = 0;
        if (rc == ONR_RR_OK && exported)
            ++d.hops; // the runtime numbers the hops its EnqueuePost runs the same way

        bool ran = false;
        if (s != nullptr)
        {
            if (rc == ONR_RR_OK && exported)
            {
                d.mz->SubmitOwnLists(q, s->producer, s->consumer, exec);
                s->ran.store(true, std::memory_order_release);
                edited = format;
                ran = true;
            }
            else
            {
                d.mz->CancelOwnLists();
            }
        }

        if (rc == ONR_RR_OK)
            rc = s_api2.EnqueuePost(self->_context, job, queue, edited);

        // the allocator is free once whatever of the slot ran is done
        if (ran || timed)
        {
            if (ran && timed)
            {
                run(s->stampPost);
                s->hop = d.hops;
            }
            const uint64_t v = d.value.fetch_add(1) + 1;
            q->Signal(d.fence, v);
            s->busy.store(v, std::memory_order_release);
        }
    }

    if (rc != ONR_RR_OK)
        s_enqueueRc.store(rc, std::memory_order_release);
    return rc;
}

// The NR span of this slot's last frame, its lists done (render thread): into the NR's GPU time, which the overlay
// shows on its own line and takes out of the upscaler time.
void OpenNrRrFeatureDx12::ReadNrTime(int slotIndex)
{
    auto& d = Denoised();
    auto& slot = d.slots[slotIndex];
    ID3D12CommandQueue* q = d.queue.load(std::memory_order_acquire);
    UINT64 frequency = 0;

    if (!slot.ran.exchange(false, std::memory_order_acq_rel) || q == nullptr || FAILED(q->GetTimestampFrequency(&frequency)) ||
        frequency == 0)
        return;

    const UINT i = UINT(slotIndex * DenoisedNr::kStamps);
    const D3D12_RANGE range { (i + 1) * sizeof(uint64_t), (i + 5) * sizeof(uint64_t) };
    uint64_t* ts = nullptr;
    if (FAILED(d.readback->Map(0, &range, reinterpret_cast<void**>(&ts))))
        return;
    const uint64_t t0 = ts[i + 1], t1 = ts[i + 4];
    const D3D12_RANGE none { 0, 0 };
    d.readback->Unmap(0, &none);

    if (t1 <= t0)
        return;
    const float ms = float(double(t1 - t0) * 1000.0 / double(frequency));
    d.nrMs.store(ms, std::memory_order_relaxed);
    d.nrAt.store(GetTickCount64(), std::memory_order_relaxed);
    DlssNr::AmdBridge::ReportNeuralMs(ms);
}

std::optional<float> OpenNrRrFeatureDx12::NrTime() const
{
    auto& d = Denoised();
    if (GetTickCount64() - d.nrAt.load(std::memory_order_relaxed) > 500)
        return std::nullopt;
    return d.nrMs.load(std::memory_order_relaxed);
}

// The upscaler's timestamps surround the whole evaluate, so they hold the NR between the network and the post too.
std::optional<double> OpenNrRrFeatureDx12::ReadUpscalerTime(void* commandQueue)
{
    auto total = IFeature_Dx12::ReadUpscalerTime(commandQueue);
    if (const auto nr = NrTime(); nr && total && lastUpscalerTime)
    {
        const double cut = std::min(double(*nr), *lastUpscalerTime);
        lastUpscalerTime = *lastUpscalerTime - cut;
        total = *total - cut;
    }
    return total;
}

void OpenNrRrFeatureDx12::ReadDetailedGpuTimes(void* commandQueue, std::vector<DetailedGpuTime>& detailedGpuTimes)
{
    IFeature_Dx12::ReadDetailedGpuTimes(commandQueue, detailedGpuTimes);
    if (const auto nr = NrTime())
        detailedGpuTimes.emplace_back(DetailedGpuTime { "NR (mochizuki)", double(*nr), false });
}

// A timed frame of this slot, its lists done (render thread): its timestamps into QueryPerformanceCounter ticks, then
// with the runtime's half (a few frames later, ABI 3) into the sums; every kTimedLog frames a line in the log.
void OpenNrRrFeatureDx12::LogHopTiming(int slotIndex)
{
    auto& d = Denoised();
    auto& slot = d.slots[slotIndex];
    ID3D12CommandQueue* q = d.queue.load(std::memory_order_acquire);

    if (slot.timed && slot.hop != 0 && q != nullptr)
    {
        UINT64 frequency = 0, gpu = 0, cpu = 0;
        LARGE_INTEGER qpf {};
        uint64_t* ts = nullptr;
        const D3D12_RANGE range { slotIndex * DenoisedNr::kStamps * sizeof(uint64_t),
                                  (slotIndex + 1) * DenoisedNr::kStamps * sizeof(uint64_t) };

        if (SUCCEEDED(q->GetTimestampFrequency(&frequency)) && SUCCEEDED(q->GetClockCalibration(&gpu, &cpu)) &&
            QueryPerformanceFrequency(&qpf) && SUCCEEDED(d.readback->Map(0, &range, reinterpret_cast<void**>(&ts))))
        {
            DenoisedNr::Sample sample { slot.hop };
            const double k = double(qpf.QuadPart) / double(frequency);
            for (int i = 0; i < DenoisedNr::kStamps; ++i)
            {
                const uint64_t t = ts[slotIndex * DenoisedNr::kStamps + i];
                sample.qpc[i] = uint64_t(int64_t(cpu) + int64_t(double(int64_t(t - gpu)) * k));
            }
            const D3D12_RANGE none { 0, 0 };
            d.readback->Unmap(0, &none);
            d.pending.push_back(sample);
        }
    }
    slot.timed = false;
    if (d.hopContext.load() != _context)
        d.pending.clear();

    LARGE_INTEGER qpf {};
    QueryPerformanceFrequency(&qpf);
    const double ms = 1000.0 / double(qpf.QuadPart);
    auto at = [&](uint64_t a, uint64_t b) { return double(int64_t(b - a)) * ms; };

    for (auto it = d.pending.begin(); it != d.pending.end();)
    {
        const uint64_t* t = it->qpc;
        OnrRrHopTiming h {};
        h.size = sizeof(h);
        const bool runtime = s_getHopTiming != nullptr && s_getHopTiming(_context, it->hop, &h) == ONR_RR_OK;

        if (!runtime && s_getHopTiming != nullptr)
        {
            // not read back by the runtime yet; dropped once it never will be
            if (it->hop + 12 > d.hops)
                ++it;
            else
                it = d.pending.erase(it);
            continue;
        }

        if (runtime)
        {
            // queue -> network, network, -> producer, producer, mochizuki, consumer, -> post, post, -> queue
            const double v[9] = { at(t[0], h.net_begin),  at(h.net_begin, h.net_end),   at(h.net_end, t[1]),
                                  at(t[1], t[2]),         at(t[2], t[3]),               at(t[3], t[4]),
                                  at(t[4], h.post_begin), at(h.post_begin, h.post_end), at(h.post_end, t[5]) };
            for (int i = 0; i < 9; ++i)
                d.sum[i] += v[i];
        }
        else
        {
            // without ABI 3: the runtime's halves with their hops, from the D3D12 side
            const double v[9] = { 0, at(t[0], t[1]), 0, at(t[1], t[2]), at(t[2], t[3]), at(t[3], t[4]), 0, at(t[4], t[5]),
                                  0 };
            for (int i = 0; i < 9; ++i)
                d.sum[i] += v[i];
        }
        d.sum[9] += at(t[0], t[5]);
        ++d.summed;
        it = d.pending.erase(it);
    }

    if (d.summed < DenoisedNr::kTimedLog)
        return;

    const double n = d.summed;
    MochizukiNrInfo mz {};
    const bool mzInfo = DlssNr::Backend::LmxxfBackend::MochizukiInfo(mz);
    if (s_getHopTiming != nullptr)
        LOG_INFO("OpenRR: hop timeline on the GPU, mean of {} frames (ms): queue -> network {:.3f} | network {:.3f} | "
                 "-> producer {:.3f} | producer {:.3f} | mochizuki {:.3f} (its network {:.3f}) | consumer {:.3f} | -> post "
                 "{:.3f} | post {:.3f} | -> queue {:.3f} | total {:.3f}",
                 d.summed, d.sum[0] / n, d.sum[1] / n, d.sum[2] / n, d.sum[3] / n, d.sum[4] / n,
                 mzInfo ? mz.gpu_ms_median : 0.f, d.sum[5] / n, d.sum[6] / n, d.sum[7] / n, d.sum[8] / n, d.sum[9] / n);
    else
        LOG_INFO("OpenRR: hop timeline on the GPU, mean of {} frames (ms; a runtime without ABI 3, its halves with "
                 "their hops): network {:.3f} | producer {:.3f} | mochizuki {:.3f} (its network {:.3f}) | consumer {:.3f} "
                 "| post {:.3f} | total {:.3f}",
                 d.summed, d.sum[1] / n, d.sum[3] / n, d.sum[4] / n, mzInfo ? mz.gpu_ms_median : 0.f, d.sum[5] / n,
                 d.sum[7] / n, d.sum[9] / n);
    std::fill(std::begin(d.sum), std::end(d.sum), 0.0);
    d.summed = 0;
}

// NrOnDenoised (render thread): asks the runtime for the denoised image of this job and records mochizuki on it into
// OptiScaler's own lists: the image into a texture, mochizuki's inputs (producer), and its output back into the
// runtime's buffer (consumer). The between slot runs them. Any refusal leaves the frame with the network's image.
// onOutput (NrImage=output, D3D12 backend, the whole frame already recorded): mochizuki reads the output scaled to the
// render size and the consumer puts its change, with a history of its own, back on the output (OpenNrRrOutputNr.h).
uint32_t OpenNrRrFeatureDx12::RecordDenoisedNr(const NVSDK_NGX_Parameter& params, const OnrRrFrame& frame, void* job,
                                                const OnrRrDenoisedImage* given, bool onOutput)
{
    auto& d = Denoised();
    std::string why;
    const uint32_t edit = EditFromConfig(&why);

    static uint32_t loggedEdit = ONR_RR_EDIT_OFF;
    static std::string loggedWhy;
    if (edit != loggedEdit || why != loggedWhy)
    {
        loggedEdit = edit;
        loggedWhy = why;
        if (edit != ONR_RR_EDIT_OFF)
            LOG_INFO("OpenRR: mochizuki on RR's image, display mode");
        else if (!why.empty())
            LOG_INFO("OpenRR: mochizuki on the denoised image is not running: {}", why);
    }

    if (edit == ONR_RR_EDIT_OFF || d.broken)
        return 0;

    if (d.mz == nullptr)
    {
        // Its session is made on the queue that runs the game's list, known after the first between slot.
        ID3D12CommandQueue* q = d.queue.load(std::memory_order_acquire);
        if (q == nullptr)
        {
            d.SetStatus("waiting for the game's queue");
            return 0;
        }

        try
        {
            d.device = Device;
            d.device->AddRef();
            if (FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence))))
                throw std::runtime_error("CreateFence");
            d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

            // Native lists: the between slot executes them on the raw queue.
            DlssNr::Submission::SuppressProxyWrap native;
            // One allocator takes one recording list at a time: the producer is closed before the consumer is made.
            for (auto& s : d.slots)
            {
                if (FAILED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))) ||
                    FAILED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator, nullptr,
                                                     IID_PPV_ARGS(&s.producer))))
                    throw std::runtime_error("command lists");
                s.producer->Close();
                if (FAILED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator, nullptr,
                                                     IID_PPV_ARGS(&s.consumer))))
                    throw std::runtime_error("command lists");
                s.consumer->Close();
                for (auto** l : { &s.stampPre, &s.stampPost })
                {
                    if (FAILED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator, nullptr,
                                                         IID_PPV_ARGS(l))))
                        throw std::runtime_error("command lists");
                    (*l)->Close();
                }
            }

            D3D12_QUERY_HEAP_DESC qd { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, DenoisedNr::kSlots * DenoisedNr::kStamps, 0 };
            D3D12_HEAP_PROPERTIES rh { D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = DenoisedNr::kSlots * DenoisedNr::kStamps * sizeof(uint64_t);
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(Device->CreateQueryHeap(&qd, IID_PPV_ARGS(&d.stamps))) ||
                FAILED(Device->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr, IID_PPV_ARGS(&d.readback))))
                throw std::runtime_error("timestamp queries");
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("OpenRR: mochizuki on the denoised image: {} failed", e.what());
            d.SetStatus(std::string("failed: ") + e.what());
            d.broken = true;
            return 0;
        }

        d.mz = new DlssNr::Backend::LmxxfBackend(d.device, q, Util::DllPath().parent_path(), L"MochizukiNrRuntime.dll",
                                                 true);
        LOG_INFO("OpenRR: mochizuki session for the denoised image on queue {:p}", (void*) q);
    }
    else if (d.device != Device)
    {
        d.SetStatus("the game's device changed; restart the game");
        return 0;
    }

    if (onOutput != d.lastOnOutput)
    {
        LOG_INFO("OpenRR: the NR reads {}", onOutput ? "RR's output" : "the denoised image");
        d.lastOnOutput = onOutput;
        d.mz->InvalidateHistory();
    }

    OnrRrDenoisedImage image {};
    image.size = sizeof(image);
    // Half floats: half the bytes for every copy of the hop, and a format mochizuki takes as a game's colour.
    image.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (given != nullptr)
        image = *given;

    if (onOutput)
    {
        image.width = frame.render_width;
        image.height = frame.render_height;
    }
    else if ((given == nullptr && s_api2.PrepareDenoised(_context, job, edit, &image) != ONR_RR_OK) || image.buffer == nullptr)
    {
        d.SetStatus("waiting for the network");
        return 0;
    }

    auto* buffer = static_cast<ID3D12Resource*>(image.buffer);
    const int slotIndex = d.next;
    auto& slot = d.slots[slotIndex];

    if (!d.Wait(slot.busy.load(std::memory_order_acquire)))
    {
        d.SetStatus("the previous frames' lists are still running; frame skipped");
        return 0;
    }
    ReadNrTime(slotIndex);
    LogHopTiming(slotIndex);

    if (!onOutput &&
        (d.colour == nullptr || d.width != image.width || d.height != image.height || d.format != DXGI_FORMAT(image.format)))
    {
        for (auto& s : d.slots)
        {
            if (!d.Wait(s.busy.load(std::memory_order_acquire)))
            {
                d.SetStatus("the previous frames' lists are still running; frame skipped");
                return 0;
            }
        }

        if (d.colour != nullptr)
            d.colour->Release();
        d.colour = nullptr;

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = image.width;
        desc.Height = image.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT(image.format);
        desc.SampleDesc.Count = 1;

        if (FAILED(Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&d.colour))))
        {
            d.SetStatus("the colour texture could not be made");
            return 0;
        }

        d.width = image.width;
        d.height = image.height;
        d.format = DXGI_FORMAT(image.format);
    }

    slot.allocator->Reset();
    // the hop's timeline needs the runtime's half (ABI 3), which only the Vulkan backend has
    slot.timed = !onOutput && given == nullptr && d.queue.load(std::memory_order_acquire) != nullptr &&
                 ++d.frames % DenoisedNr::kTimedEvery == 0;
    slot.hop = 0;
    const UINT stamp = UINT(slotIndex * DenoisedNr::kStamps);
    slot.producer->Reset(slot.allocator, nullptr);
    slot.producer->EndQuery(d.stamps, D3D12_QUERY_TYPE_TIMESTAMP, stamp + 1);
    // RR's output, as OpenNrRrOutputNr takes it
    AmdPreSr::Frame picture {};
    ID3D12Resource* reduced = nullptr;
    if (onOutput)
    {
        picture.colour = static_cast<ID3D12Resource*>(frame.output.resource);
        picture.colourState = D3D12_RESOURCE_STATES(frame.output.state);
        picture.width = DisplayWidth();
        picture.height = DisplayHeight();
        try
        {
            if (!d.outputNr)
                d.outputNr = std::make_unique<OpenNrRrOutputNr::Passes>(d.device);
            reduced = d.outputNr->Reduce(slot.producer, picture, image.width, image.height);
        }
        catch (const std::exception& e)
        {
            d.outputFailed = true;
            slot.producer->Close();
            LOG_ERROR("OpenRR: NR on the output off for this session: {}", e.what());
            d.SetStatus(std::string("NR on the output failed: ") + e.what());
            return 0;
        }
    }
    else
    {
        D3D12_TEXTURE_COPY_LOCATION bufferLoc { buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
        bufferLoc.PlacedFootprint = { 0, { DXGI_FORMAT(image.format), image.width, image.height, 1, image.row_pitch } };
        D3D12_TEXTURE_COPY_LOCATION colourLoc { d.colour, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
        Transition(slot.producer, buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(slot.producer, d.colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        slot.producer->CopyTextureRegion(&colourLoc, 0, 0, 0, &bufferLoc, nullptr);
        Transition(slot.producer, d.colour, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(slot.producer, buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    // The denoised image stands where the game's colour would before Super Resolution: same grid, same jitter, same
    // colour space; the guides are the game's. The scaled output is the same, without the jitter.
    AmdPreSr::Frame f {};
    f.colour = onOutput ? reduced : d.colour;
    f.colourState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    f.width = image.width;
    f.height = image.height;
    f.motion = static_cast<ID3D12Resource*>(frame.motion.resource);
    f.motionState = D3D12_RESOURCE_STATES(frame.motion.state);
    f.motionScaleX = frame.mv_scale_x;
    f.motionScaleY = frame.mv_scale_y;
    if (!LowResMV())
    {
        f.motionWidth = DisplayWidth();
        f.motionHeight = DisplayHeight();
    }
    f.depth = static_cast<ID3D12Resource*>(frame.depth.resource);
    f.depthState = D3D12_RESOURCE_STATES(frame.depth.state);
    f.depthInverted = DepthInverted();
    f.jitterX = onOutput ? 0.f : frame.jitter_x;
    f.jitterY = onOutput ? 0.f : frame.jitter_y;
    f.reset = (frame.flags & ONR_RR_FRAME_RESET) != 0;

    // NGX applies the exposure texture to HDR input only; the image keeps the game's input's encoding.
    if (IsHdr())
    {
        ID3D12Resource* exposure = nullptr;
        if (TryGetNGXVoidPointer(params, NVSDK_NGX_Parameter_ExposureTexture, exposure))
        {
            f.exposure = exposure;
            const auto& cfg = *Config::Instance();
            if (cfg.ExposureResourceBarrier.has_value())
                f.exposureState = D3D12_RESOURCE_STATES(cfg.ExposureResourceBarrier.value());
            params.Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &f.preExposure);
            params.Get(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, &f.exposureScale);
        }
    }

    const auto& fovCfg = *Config::Instance();
    AmdPreSr::Foveation::Grid grid { f.width, f.height, f.width, f.height };
    if (const int mode = std::clamp(fovCfg.NrFoveation.value_or_default(), 0, 4); mode > 0 && !d.foveationFailed)
    {
        auto fov = AmdPreSr::Foveation::Preset(mode);
        if (mode >= 4)
            fov = { fovCfg.NrFoveationCentreWidth.value_or_default(), fovCfg.NrFoveationCentreHeight.value_or_default(),
                    fovCfg.NrFoveationFalloff.value_or_default(), fovCfg.NrFoveationEdge.value_or_default() };
        grid = AmdPreSr::Foveation::MakeGrid(f.width, f.height, fov);
    }
    const bool foveated = grid.Active();
    if (const auto key = foveated ? grid : AmdPreSr::Foveation::Grid {}; !(key == d.foveationGrid))
    {
        if (foveated)
            LOG_INFO("OpenRR: foveated NR, the network at {}x{} for {}x{} ({:.0f}% of the pixels)", grid.workWidth,
                     grid.workHeight, f.width, f.height, 100 * grid.Share());
        else
            LOG_INFO("OpenRR: foveated NR off at {}x{}", f.width, f.height);
        d.foveationGrid = key;
        d.foveationJitter = false;
        d.mz->InvalidateHistory();
    }
    if (foveated && !d.foveation)
        d.foveation = std::make_unique<AmdPreSr::Foveation::Passes>(d.device);

    // The warp reads the denoised image and the game's vectors; mochizuki reads no depth before SR.
    AmdPreSr::Frame in = f, source = f;
    source.depth = nullptr;
    if (foveated)
    {
        AmdPreSr::Foveation::Passes::Options o;
        // The jitter step goes through the warp with the vectors, as on the bridge.
        const bool fold = fovCfg.MochizukiJitterHistory.value_or_default() && f.motion;
        if (fold)
        {
            if (d.foveationJitter && !f.reset)
            {
                o.jitterStepX = d.foveationJitterX - f.jitterX;
                o.jitterStepY = d.foveationJitterY - f.jitterY;
            }
            d.foveationJitterX = f.jitterX;
            d.foveationJitterY = f.jitterY;
            d.foveationJitter = true;
        }
        try
        {
            in = d.foveation->Warp(slot.producer, source, grid, o);
            if (fold)
                in.jitterX = in.jitterY = 0;
        }
        catch (const std::exception& e)
        {
            d.foveationFailed = true;
            in = f;
            LOG_ERROR("OpenRR: foveated NR off for this session: {}", e.what());
        }
    }
    else if (d.foveation)
        d.foveation->Idle();

    ID3D12Resource* result = d.mz->Record(slot.producer, in, AmdPreSr::Settings {});
    if (slot.timed)
        slot.producer->EndQuery(d.stamps, D3D12_QUERY_TYPE_TIMESTAMP, stamp + 2);
    slot.producer->Close();

    if (result == nullptr)
    {
        if (onOutput)
            d.outputNr->Invalidate();
        d.SetStatus(d.mz->Status());
        return 0;
    }

    slot.consumer->Reset(slot.allocator, nullptr);
    if (slot.timed)
        slot.consumer->EndQuery(d.stamps, D3D12_QUERY_TYPE_TIMESTAMP, stamp + 3);
    if (!d.mz->RecordConsumer(slot.consumer))
    {
        slot.consumer->Close();
        d.SetStatus(d.mz->Status());
        return 0;
    }

    if (in.colour != f.colour)
    {
        try
        {
            result = d.foveation->Unwarp(slot.consumer, source, result);
        }
        catch (const std::exception& e)
        {
            d.foveationFailed = true;
            slot.consumer->Close();
            LOG_ERROR("OpenRR: foveated NR off for this session: {}", e.what());
            d.SetStatus(std::string("foveated NR failed: ") + e.what());
            return 0;
        }
    }

    DXGI_FORMAT format = result->GetDesc().Format;
    if (onOutput)
    {
        // the change on the picture, into the output
        picture.motion = f.motion;
        picture.motionState = f.motionState;
        picture.motionWidth = f.motionWidth;
        picture.motionHeight = f.motionHeight;
        picture.motionScaleX = f.motionScaleX;
        picture.motionScaleY = f.motionScaleY;
        format = AmdPreSr::ReadFormat(picture.colour->GetDesc().Format);
        try
        {
            result = d.outputNr->Compose(slot.consumer, picture, result, format, f.reset);
        }
        catch (const std::exception& e)
        {
            d.outputFailed = true;
            slot.consumer->Close();
            LOG_ERROR("OpenRR: NR on the output off for this session: {}", e.what());
            d.SetStatus(std::string("NR on the output failed: ") + e.what());
            return 0;
        }
        D3D12_TEXTURE_COPY_LOCATION dstLoc { picture.colour, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
        D3D12_TEXTURE_COPY_LOCATION resultLoc { result, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
        const D3D12_BOX box { 0, 0, 0, picture.width, picture.height, 1 };
        Transition(slot.consumer, result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(slot.consumer, picture.colour, picture.colourState, D3D12_RESOURCE_STATE_COPY_DEST);
        slot.consumer->CopyTextureRegion(&dstLoc, 0, 0, 0, &resultLoc, &box);
        Transition(slot.consumer, picture.colour, D3D12_RESOURCE_STATE_COPY_DEST, picture.colourState);
        Transition(slot.consumer, result, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    else
    {
        // mochizuki's output (the colour's format) back into the buffer, at offset 0 with the pitch the runtime expects
        D3D12_TEXTURE_COPY_LOCATION outLoc { buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
        outLoc.PlacedFootprint = { 0,
                                   { format, image.width, image.height, 1, (image.width * TexelBytes(format) + 255) & ~255u } };
        D3D12_TEXTURE_COPY_LOCATION resultLoc { result, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
        const D3D12_BOX box { 0, 0, 0, image.width, image.height, 1 };
        Transition(slot.consumer, result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(slot.consumer, buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        slot.consumer->CopyTextureRegion(&outLoc, 0, 0, 0, &resultLoc, &box);
        Transition(slot.consumer, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Transition(slot.consumer, result, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    slot.consumer->EndQuery(d.stamps, D3D12_QUERY_TYPE_TIMESTAMP, stamp + 4);
    // the NR span's two stamps (a timed frame resolves all of them again after EnqueuePost)
    for (const UINT i : { stamp + 1, stamp + 4 })
        slot.consumer->ResolveQueryData(d.stamps, D3D12_QUERY_TYPE_TIMESTAMP, i, 1, d.readback, i * sizeof(uint64_t));
    slot.consumer->Close();

    if (slot.timed)
    {
        slot.stampPre->Reset(slot.allocator, nullptr);
        slot.stampPre->EndQuery(d.stamps, D3D12_QUERY_TYPE_TIMESTAMP, stamp);
        slot.stampPre->Close();
        slot.stampPost->Reset(slot.allocator, nullptr);
        slot.stampPost->EndQuery(d.stamps, D3D12_QUERY_TYPE_TIMESTAMP, stamp + 5);
        slot.stampPost->ResolveQueryData(d.stamps, D3D12_QUERY_TYPE_TIMESTAMP, stamp, DenoisedNr::kStamps, d.readback,
                                         stamp * sizeof(uint64_t));
        slot.stampPost->Close();
    }

    {
        std::lock_guard lock(d.mutex);
        d.armedJob = job;
        d.armedSlot = slotIndex;
        d.armedFormat = uint32_t(format);
        d.status = d.mz->Status();
    }
    d.next = (d.next + 1) % DenoisedNr::kSlots;
    return uint32_t(format);
}
