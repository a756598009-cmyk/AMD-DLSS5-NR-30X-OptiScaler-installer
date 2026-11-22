#include "pch.h"
#include "LmxxfBackend.h"
#include "LmxxfQueueDrain.h"
#include <cstring>
#include "../submission/SubmissionTls.h"
#include "lmxxf_runtime/LmxxfNrApi.h"
#include "mochizuki_runtime/MochizukiNrControls.h"
#include "MochizukiGuides.h"
#include "../amd/AmdBridge.h"
#include <State.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>

namespace DlssNr::Backend
{
struct LmxxfBackend::Api
{
    LmxxfNrApi table {};
    // mochizuki's own exports (MochizukiNrControls.h); null for lmxxf and older runtimes.
    PFN_MochizukiNrGetFeatures getFeatures = nullptr;
    PFN_MochizukiNrSetControls setControls = nullptr;
    PFN_MochizukiNrGetInfo getInfo = nullptr;
    PFN_MochizukiNrGetControlDefaults getControlDefaults = nullptr;
    PFN_MochizukiNrSetExposure setExposure = nullptr;
    PFN_MochizukiNrDestroyAtExit destroyAtExit = nullptr;
    // The game's exposure texture, copied on each frame's list into one slot of a readback ring and read
    // kExposureSlots - 1 frames later, long after the GPU wrote it. Render thread only.
    static constexpr uint32_t kExposureSlots = 8;
    static constexpr UINT64 kExposureSlotBytes = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    ID3D12Resource* exposureRing = nullptr;
    const uint8_t* exposureMapped = nullptr;
    DXGI_FORMAT exposureFormat[kExposureSlots] {};
    float exposureScale[kExposureSlots] {};
    uint64_t exposureFrames = 0;
    // MOCHIZUKI_NR_FEATURE_ANY_QUEUE: the runtime follows whichever queue executes the list, so a queue change
    // needs no new session.
    bool anyQueue = false;
    // The runtime's defaults, which every frame's controls start from, and what the session last took: SetControls
    // runs only when the controls change or the session is new. Render thread only.
    MochizukiNrControls controlDefaults {};
    MochizukiNrControls controlsSent {};
    bool controlsSentValid = false;
};

namespace
{
std::wstring WidenPath(const std::filesystem::path& p) { return p.wstring(); }

// COM identity: proxies (FG, Streamline) can hand out different pointers for one queue.
bool SameObject(IUnknown* a, IUnknown* b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    IUnknown* id1 = nullptr;
    IUnknown* id2 = nullptr;
    a->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&id1));
    b->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&id2));
    const bool same = id1 && id2 && id1 == id2;
    if (id1)
        id1->Release();
    if (id2)
        id2->Release();
    return same;
}

std::filesystem::path ResolveModulesDir(const std::filesystem::path& directory)
{
    wchar_t env[MAX_PATH] {};
    if (GetEnvironmentVariableW(L"LMXXF_MODULES_DIR", env, MAX_PATH) && env[0])
        return env;
    const auto nextToDll = directory / L"lmxxf-modules";
    if (std::filesystem::exists(nextToDll))
        return nextToDll;
    // Dev layout: repo exports/lmxxf-modules-68dc099 relative to OptiScaler.dll parent is uncommon;
    // prefer env. Fall back to directory itself so Create can still run and fail loudly.
    return directory;
}

// A mochizuki setting as its runtime accepts it: NaN or infinity is the default, anything else is clamped. The
// runtime sanitises again; doing it here keeps what the log shows equal to what runs.
float MzSetting(float v, float lo, float hi, float def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; }

// What mochizuki takes with each frame in LmxxfNrFrameInfo: the RenoDX stage's pass count (which the runtime takes
// from the stage's settings; here for person protection's first pass) and the network's flags.
struct MochizukiFrameSettings
{
    uint32_t passes;
    uint32_t flags; // LMXXF_NR_FRAME_FLAG_INT4, ORIGINAL_MODEL, ...
};

// From mochizuki's own keys only: never the danielblnc, lmxxf or NVIDIA ones, nor AmdNrScale with AmdDynamicScale's
// steps (the bridge's Settings::modelScale), since every distinct scale or pass count rebuilds the network.
MochizukiFrameSettings MochizukiFrame(const Config& cfg)
{
    MochizukiFrameSettings s {};
    s.passes = std::clamp(cfg.MochizukiRenoPasses.value_or_default(), 1u,
                          cfg.MochizukiRenoUnlockPasses.value_or_default() ? 30u : 4u);
    // MochizukiInt4: int4 mixed then, CC_SILVER_AARDWOLD_OPTIMIZED since upstream 0.0.8.3 (at MochizukiOptimizedLayers),
    // as ACO's machine code only: without ACO the runtime would build the default network anyway.
    const bool int4 = cfg.MochizukiInt4.value_or_default() && cfg.MochizukiAco.value_or_default();
    // It is made from the 310.8 model only.
    const bool original = int4 || cfg.MochizukiOriginalModel.value_or_default();
    // The new model's faster variant, likewise ACO only; the runtime checks the card and the files.
    const bool experimental = !original && cfg.MochizukiExperimental.value_or_default() && cfg.MochizukiAco.value_or_default();
    // Cascade detail, anti-halo and the pass 3 estimate do nothing in the RenoDX stage; they go as before.
    s.flags = (int4 ? LMXXF_NR_FRAME_FLAG_INT4 : 0) | (original ? LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL : 0) |
              (experimental ? LMXXF_NR_FRAME_FLAG_EXPERIMENTAL : 0) |
              (cfg.MochizukiCascadeDetail.value_or_default() ? LMXXF_NR_FRAME_FLAG_CASCADE_DETAIL : 0) |
              (cfg.MochizukiAntiHalo.value_or_default() ? LMXXF_NR_FRAME_FLAG_ANTI_HALO : 0) |
              (cfg.MochizukiPassEstimate.value_or_default() ? LMXXF_NR_FRAME_FLAG_PASS3_ESTIMATE : 0);
    return s;
}

// mochizuki's controls from its Mochizuki* keys, into c (the runtime's defaults, for every field not set here). The
// network always runs in the RenoDX stage: the model, pass, resolution and composition controls are its reno_* ones,
// and the native route's fields keep the runtime's defaults.
void FillMochizukiControls(const Config& cfg, MochizukiNrControls& c)
{
    c.history_strength = MzSetting(cfg.MochizukiHistoryStrength.value_or_default(), 0.f, 1.f, 1.f);
    c.apply_model = cfg.MochizukiApplyModel.value_or_default() ? 1u : 0u;
    // Dynamic resolution: 0 exact, 1 auto (the key's default, and anything the key does not name), 2 always. The
    // runtime's own default is exact, for hosts that do not ask.
    const std::string drs = cfg.MochizukiDynamicResolution.value_or_default();
    c.drs_mode = drs == "exact" ? 0u : drs == "always" ? 2u : 1u;
    c.preprocess = cfg.MochizukiPreprocess.value_or_default() ? 1u : 0u;
    c.preprocess_exposure = std::min(cfg.MochizukiPreprocessExposure.value_or_default(), 2u);
    c.preprocess_bias_ev = MzSetting(cfg.MochizukiPreprocessBias.value_or_default(), -8.f, 8.f, 0.f);
    c.preprocess_curve = std::min(cfg.MochizukiPreprocessCurve.value_or_default(), 6u);
    c.preprocess_contrast = MzSetting(cfg.MochizukiPreprocessContrast.value_or_default(), .5f, 2.f, 1.f);
    c.preprocess_saturation = MzSetting(cfg.MochizukiPreprocessSaturation.value_or_default(), .05f, 2.f, 1.f);
    // Strength 0..1: 1 holds a pixel to the first pass's range, 0 lets a quarter past it (no effect in the stage).
    c.anti_halo_margin = .25f * (1.f - MzSetting(cfg.MochizukiAntiHaloStrength.value_or_default(), 0.f, 1.f, .8f));
    c.noise_every_frame = cfg.MochizukiNoiseEveryFrame.value_or_default() ? 1u : 0u;
    c.style_grade = cfg.MochizukiStyleGrade.value_or_default() ? 1u : 0u;
    c.history_precision = cfg.MochizukiHistoryPrecision.value_or_default() == 32 ? 32u : 16u;
    c.optimized_layers = std::min(cfg.MochizukiOptimizedLayers.value_or_default(), 3u);
    c.preprocess_inverse = cfg.MochizukiPreprocessInverse.value_or_default() ? 1u : 0u;
    c.renodx = 1u;
    // RenoDX's NeuralUplift switch: Enable NR already turns the stage off, so it has no key of its own
    c.reno_nr_enabled = 1u;
#define MZR_F(name, def, lo, hi, group, label, member, help)                                                           \
    c.member = MzSetting(cfg.name.value_or_default(), lo, hi, def);
#define MZR_U(name, def, lo, hi, group, label, member, help)                                                           \
    c.member = std::clamp(cfg.name.value_or_default(), uint32_t(lo), uint32_t(hi));
#define MZR_B(name, def, lo, hi, group, label, member, help) c.member = cfg.name.value_or_default() ? 1u : 0u;
#include "MochizukiRenoKeys.inc"
#undef MZR_F
#undef MZR_U
#undef MZR_B
}

// The live mochizuki session's last MochizukiNrGetInfo, for the menu (LmxxfBackend::MochizukiInfo). The product
// runs one backend, so one slot; the render thread fills it with the runtime status and clears it with the session.
struct InfoSlot
{
    std::mutex mutex;
    MochizukiNrInfo info {};
    bool valid = false;
};

InfoSlot& LastInfo()
{
    static InfoSlot slot;
    return slot;
}

// The backend LmxxfBackend::OnListRecycled routes to. The product constructs one and never destroys it.
std::atomic<LmxxfBackend*> g_recycleTarget { nullptr };

} // namespace

// lmxxf takes the FrameInfo sizes it knows, which end before the depth fields; mochizuki takes all of it.
uint32_t LmxxfBackend::FrameInfoSize() const
{
    return Lmxxf() ? uint32_t(offsetof(LmxxfNrFrameInfo, depth)) : uint32_t(sizeof(LmxxfNrFrameInfo));
}

void LmxxfBackend::SetStatus(const char* s)
{
    if (!s)
        return;
    std::string next = s;
    if (!Lmxxf() && next.rfind("lmxxf", 0) == 0)
        next.replace(0, 5, "mochizuki");
    {
        std::lock_guard lock(statusMutex);
        if (status == next)
            return;
        status = next;
    }
    // Surface to OptiScaler.log with progressive rate-limiting on status changes.
    static std::atomic<uint32_t> s_statusLogCount { 0 };
    const uint32_t c = s_statusLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (c <= 10 || (c <= 100 && (c % 20 == 0)) || (c <= 1000 && (c % 100 == 0)) || (c % 1000 == 0))
    {
        LOG_INFO("lmxxf status: {} (status change #{})", next, c);
    }
}

void LmxxfBackend::RefreshRuntimeStatus()
{
    const ULONGLONG now = GetTickCount64();
    if (!session || !api->table.GetStatus || (runtimeStatusTick && now - runtimeStatusTick < 500))
        return;
    runtimeStatusTick = now;
    char st[256] {};
    api->table.GetStatus(session, st, sizeof st);
    st[sizeof st - 1] = 0;
    {
        std::lock_guard lock(statusMutex);
        runtimeStatus = st;
    }
    if (!Lmxxf() && api->getInfo)
    {
        // Zeroed first, so a field past the struct_size the runtime fills reads 0.
        MochizukiNrInfo info {};
        info.struct_size = sizeof info;
        const bool ok = api->getInfo(session, &info) == LMXXF_NR_OK;
        info.last_error[sizeof info.last_error - 1] = 0;
        auto& slot = LastInfo();
        std::lock_guard lock(slot.mutex);
        slot.info = info;
        slot.valid = ok;
    }
}

void LmxxfBackend::ClearRuntimeStatus()
{
    runtimeStatusTick = 0;
    {
        auto& slot = LastInfo();
        std::lock_guard lock(slot.mutex);
        slot.valid = false;
    }
    std::lock_guard lock(statusMutex);
    runtimeStatus.clear();
}

bool LmxxfBackend::MochizukiInfo(MochizukiNrInfo& out)
{
    auto& slot = LastInfo();
    std::lock_guard lock(slot.mutex);
    if (!slot.valid)
        return false;
    out = slot.info;
    return true;
}

// Render thread, under recordMutex, with a session. The runtime applies the controls from the next PrepareFrame and
// never fails the session over them. They go only when they change or the session is new: each call sanitises them
// again, and pass overrides allocate.
void LmxxfBackend::SendControls()
{
    if (!session || !api->setControls)
        return;
    MochizukiNrControls c = api->controlDefaults;
    FillMochizukiControls(*Config::Instance(), c);
    if (api->controlsSentValid && std::memcmp(&c, &api->controlsSent, sizeof c) == 0)
        return;
    const int32_t rc = api->setControls(session, &c);
    if (rc != LMXXF_NR_OK)
    {
        static std::atomic<uint32_t> failures { 0 };
        const uint32_t n = failures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3 || n % 120 == 0)
        {
            char err[256] {};
            if (api->table.GetLastError)
                api->table.GetLastError(err, sizeof err);
            LOG_ERROR("mochizuki: SetControls rc={} err={} (fail#{})", rc, err, n);
        }
        return;
    }
    api->controlsSent = c;
    api->controlsSentValid = true;
    // A slider moves them every frame; the log keeps the first few and then every hundredth.
    static std::atomic<uint32_t> sends { 0 };
    const uint32_t n = sends.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 10 || n % 100 == 0)
        LOG_INFO("mochizuki: controls intensity={:.2f} style={} tone={:.2f} structure={:.2f} skin={:.2f} mask={} "
                 "guard={:.2f} history={:.2f} white={:.2f} apply={} linear={} drs={} pass2={} pass3={} (#{})",
                 c.intensity, c.style, c.local_tone, c.local_structure, c.skin_structure, c.automatic_mask, c.max_ratio,
                 c.history_strength, c.white_point, c.apply_model, c.linear_input, c.drs_mode, c.pass[0].used,
                 c.pass[1].used, n);
}

LmxxfBackend::LmxxfBackend(ID3D12Device* dev, ID3D12CommandQueue* q, const std::filesystem::path& dir,
                           const wchar_t* runtime, bool ownLists)
    : device(dev), queue(q), directory(dir), runtimeFile(runtime), ownLists(ownLists)
{
    if (device)
        device->AddRef();
    if (queue)
        queue->AddRef();
    api = new Api();
    diagnostic = LmxxfProbe::ParseMode(Config::Instance()->LmxxfDiagnostic.value_or_default());
    LOG_INFO("lmxxf diagnostic: mode={} (restart to change; "
             "off/original/copy-current/staging-current/staging-previous/proxy-original/split-original)",
             Config::Instance()->LmxxfDiagnostic.value_or_default());
    {
        const bool fit = Config::Instance()->LmxxfFitLarge.value_or_default();
        _putenv(fit ? "DLSS5_FIT_LARGE=1" : "DLSS5_FIT_LARGE=0");
        LOG_INFO("lmxxf FitLarge={} (DLSS5_FIT_LARGE; restart if changed mid-session)", fit);
    }
    SetStatus("lmxxf: constructed (session not ready)");
    // A backend with its own lists never sees a game list recycled; the upscaler path's keeps the callback.
    if (ownLists)
        return;
    g_recycleTarget.store(this, std::memory_order_release);
    DlssNr::Submission::Hooks::g_onListRecycled.store(&LmxxfBackend::OnListRecycled, std::memory_order_release);
}

// Like Shutdown, only once no other thread can be inside the backend: a ListRecycled already running is not waited
// for. The product never destroys its backend.
LmxxfBackend::~LmxxfBackend()
{
    UnregisterListRecycled();
    Shutdown();
    delete api;
    api = nullptr;
    if (queue)
    {
        queue->Release();
        queue = nullptr;
    }
    if (device)
    {
        device->Release();
        device = nullptr;
    }
}

bool LmxxfBackend::EnsureRuntime()
{
    if (runtimeDll && api && api->table.EnqueueHip)
        return true;
    const auto dllPath = directory / runtimeFile;
    // No "Bad Image" box over the game when Windows refuses the file: the status says why.
    DWORD oldMode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &oldMode);
    runtimeDll = reinterpret_cast<void*>(LoadLibraryW(dllPath.c_str()));
    const DWORD error = runtimeDll ? 0 : GetLastError();
    SetThreadErrorMode(oldMode, nullptr);
    if (!runtimeDll)
    {
        const std::string name = dllPath.filename().string();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_MOD_NOT_FOUND)
            SetStatus(("lmxxf: " + name + " missing next to OptiScaler").c_str());
        else if (error == ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION || error == ERROR_INVALID_IMAGE_HASH ||
                 error == ERROR_ACCESS_DISABLED_BY_POLICY)
            SetStatus(("lmxxf: Windows blocked " + name + " (error " + std::to_string(error) +
                      ": Smart App Control or a code integrity policy); allow it or turn Smart App Control off")
                          .c_str());
        else
            SetStatus(("lmxxf: " + name + " did not load (error " + std::to_string(error) + ")").c_str());
        return false;
    }
    auto getApi = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi*)>(
        GetProcAddress(reinterpret_cast<HMODULE>(runtimeDll), "LmxxfNrGetApi"));
    if (!getApi)
    {
        SetStatus("lmxxf: LmxxfNrGetApi missing");
        return false;
    }
    api->table.struct_size = sizeof(LmxxfNrApi);
    if (getApi(LMXXF_NR_ABI_VERSION, &api->table) != LMXXF_NR_OK)
    {
        SetStatus("lmxxf: GetApi failed");
        return false;
    }
    if (!Lmxxf())
    {
        const auto dll = reinterpret_cast<HMODULE>(runtimeDll);
        api->getFeatures = reinterpret_cast<PFN_MochizukiNrGetFeatures>(GetProcAddress(dll, "MochizukiNrGetFeatures"));
        api->setControls = reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(dll, "MochizukiNrSetControls"));
        api->getInfo = reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(dll, "MochizukiNrGetInfo"));
        api->setExposure = reinterpret_cast<PFN_MochizukiNrSetExposure>(GetProcAddress(dll, "MochizukiNrSetExposure"));
        api->destroyAtExit =
            reinterpret_cast<PFN_MochizukiNrDestroyAtExit>(GetProcAddress(dll, "MochizukiNrDestroyAtExit"));
        api->getControlDefaults =
            reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(GetProcAddress(dll, "MochizukiNrGetControlDefaults"));
        const uint32_t features = api->getFeatures ? api->getFeatures() : 0u;
        api->anyQueue = (features & MOCHIZUKI_NR_FEATURE_ANY_QUEUE) != 0;
        // The runtime's own defaults under the fields the keys fill, so a field this host does not know stays at
        // what the runtime would use without it.
        api->controlDefaults = {};
        api->controlDefaults.struct_size = sizeof(MochizukiNrControls);
        if (api->getControlDefaults && api->getControlDefaults(&api->controlDefaults) != LMXXF_NR_OK)
        {
            api->controlDefaults = {};
            api->controlDefaults.struct_size = sizeof(MochizukiNrControls);
        }
        api->controlsSentValid = false;
        LOG_INFO("mochizuki: runtime features=0x{:X} (export {}); controls {}; info {}", features,
                 api->getFeatures ? "present" : "absent", api->setControls ? "present" : "absent (defaults only)",
                 api->getInfo ? "present" : "absent");
    }
    return true;
}

// mochizuki: every start creates a Vulkan instance, so a failed one is retried after 2 s, doubling up to a minute,
// and one the runtime reports unsupported ("[unsupported] ...") is never retried. Returns false.
bool LmxxfBackend::SessionStartFailed(const char* step, const char* err)
{
    if (Lmxxf() && !Rdna2())
    {
        SetStatus((std::string("lmxxf: ") + step + " failed").c_str());
        return false;
    }
    if (std::strncmp(err, "[unsupported]", 13) == 0)
    {
        sessionUnavailable = true;
        LOG_ERROR("mochizuki: {} reports this GPU or driver unsupported; NR stays off for this process: {}", step, err);
        SetStatus((std::string("lmxxf: ") + err + " (NO NR)").c_str());
        return false;
    }
    ++sessionFailures;
    const ULONGLONG delay = std::min<ULONGLONG>(2000ull << std::min<uint32_t>(sessionFailures - 1, 5u), 60000ull);
    nextSessionRetry = GetTickCount64() + delay;
    char text[128] {};
    std::snprintf(text, sizeof text, "lmxxf: %s failed (attempt %u; retrying in %llu s)", step, sessionFailures,
                  static_cast<unsigned long long>(delay / 1000));
    SetStatus(text);
    return false;
}

bool LmxxfBackend::EnsureSession()
{
    if (sessionReady && session)
        return true;
    // Before anything else, the weights scan and the QueryInterface calls included.
    if ((!Lmxxf() || Rdna2()) && (sessionUnavailable || (sessionFailures && GetTickCount64() < nextSessionRetry)))
        return false;
    if (!EnsureRuntime() || !device || !queue)
        return false;
    // mochizuki logs its first attempt and every tenth; lmxxf logs them all.
    const uint32_t attempt = sessionFailures + 1;
    const bool verbose = Lmxxf() || attempt == 1 || attempt % 10 == 0;

    // Upstream 0.21+: auto picks 720/900/1080 from Color size. Unset defaults to 1080 and blacks 720p Color.
    if (Lmxxf() && !std::getenv("DLSS5_NETWORK_HEIGHT"))
    {
        _putenv("DLSS5_NETWORK_HEIGHT=auto");
        SetEnvironmentVariableA("DLSS5_NETWORK_HEIGHT", "auto");
        LOG_INFO("lmxxf: DLSS5_NETWORK_HEIGHT defaulted to auto");
    }
    // Weights directory detection:
    // 1. Prioritize local folder next to OptiScaler / game (native-game-tiled-assets or lmxxf-weights).
    // 2. Sibling hint file (lmxxf-weights-dir.txt).
    // 3. Fallback to LMXXF_WEIGHTS_DIR environment variable (for development/benchmarks).
    // mochizuki's runtime reads its model from the dlssnr-amd folder beside it.
    if (Lmxxf())
    {
        const auto localWeights = directory / L"native-game-tiled-assets";
        const auto altWeights = directory / L"lmxxf-weights";
        const auto hint = directory / L"lmxxf-weights-dir.txt";

        if (std::filesystem::exists(localWeights) && std::filesystem::is_directory(localWeights))
        {
            SetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", localWeights.c_str());
            LOG_INFO("lmxxf: LMXXF_WEIGHTS_DIR prioritized local dir: {}", localWeights.string());
        }
        else if (std::filesystem::exists(altWeights) && std::filesystem::is_directory(altWeights))
        {
            SetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", altWeights.c_str());
            LOG_INFO("lmxxf: LMXXF_WEIGHTS_DIR prioritized local dir: {}", altWeights.string());
        }
        else if (std::filesystem::exists(hint))
        {
            std::wifstream in(hint);
            std::wstring line;
            if (in && std::getline(in, line) && !line.empty())
            {
                while (!line.empty() && (line.back() == L'\r' || line.back() == L' '))
                    line.pop_back();
                if (std::filesystem::exists(line) && std::filesystem::is_directory(line))
                {
                    SetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", line.c_str());
                    LOG_INFO("lmxxf: LMXXF_WEIGHTS_DIR from hint file: {}", std::filesystem::path(line).string());
                }
            }
        }
        else
        {
            // Fallback: check environment variable for development, but validate that it exists!
            wchar_t env[MAX_PATH] {};
            if (GetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", env, MAX_PATH) && env[0])
            {
                if (std::filesystem::exists(env) && std::filesystem::is_directory(env))
                {
                    LOG_INFO("lmxxf: LMXXF_WEIGHTS_DIR from environment: {}", std::filesystem::path(env).string());
                }
                else
                {
                    LOG_WARN("lmxxf: LMXXF_WEIGHTS_DIR in environment points to non-existent path '{}', ignoring",
                             std::filesystem::path(env).string());
                    SetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", nullptr);
                }
            }
        }

        wchar_t now[MAX_PATH] {};
        if (GetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", now, MAX_PATH) && now[0])
            LOG_INFO("lmxxf: LMXXF_WEIGHTS_DIR={}", std::filesystem::path(now).string());
        else
            LOG_WARN("lmxxf: LMXXF_WEIGHTS_DIR unset (PrepareSession may fail without tiled weights)");
    }

    const auto modules = ResolveModulesDir(directory);
    const std::wstring modulesW = WidenPath(modules);
    if (verbose)
    {
        LOG_INFO("lmxxf: assets/modules dir={}", modules.string());

        static constexpr GUID kStreamlineRiid = {
            0xADEC44E2, 0x61F0, 0x45C3, { 0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF }
        };
        IUnknown* qId = nullptr;
        if (queue)
            queue->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&qId));
        IUnknown* slQueue = nullptr;
        if (queue)
            queue->QueryInterface(kStreamlineRiid, reinterpret_cast<void**>(&slQueue));
        LOG_INFO("lmxxf: session queue={:p} (type {}) id={:p} sl={:p}", reinterpret_cast<void*>(queue),
                 queue ? static_cast<int>(queue->GetDesc().Type) : -1, reinterpret_cast<void*>(qId),
                 reinterpret_cast<void*>(slQueue));
        if (qId)
            qId->Release();
        if (slQueue)
            slQueue->Release();
    }

    LmxxfNrCreateInfo info {};
    info.struct_size = sizeof(info);
    info.device = device;
    info.queue = queue;
    info.assets_directory = modulesW.c_str();
    info.flags = LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK;
    if (!Lmxxf() && Config::Instance()->MochizukiAco.value_or_default())
        info.flags |= LMXXF_NR_CREATE_FLAG_ACO;
    // mochizuki's RX 6000 network in its INT8 form: read by the runtime (and its pipeline check's child process) when it
    // picks the RDNA2 network.
    if (!Lmxxf() && Config::Instance()->MochizukiRdna2Int8.value_or_default())
        SetEnvironmentVariableA("MZ_RDNA2_INT8", "1");
    void* ctx = nullptr;
    int32_t createRc = api->table.Create(&info, &ctx);
    // A runtime from before ACO refuses the flag: it runs without it.
    if (createRc == LMXXF_NR_INVALID_ARGUMENT && (info.flags & LMXXF_NR_CREATE_FLAG_ACO))
    {
        LOG_WARN("mochizuki: this MochizukiNrRuntime.dll has no ACO; MochizukiAco is ignored");
        info.flags &= ~uint32_t(LMXXF_NR_CREATE_FLAG_ACO);
        createRc = api->table.Create(&info, &ctx);
    }
    if (createRc != LMXXF_NR_OK || !ctx)
    {
        char err[256] {};
        if (api->table.GetLastError)
            api->table.GetLastError(err, sizeof err);
        if (Lmxxf() || std::strncmp(err, "[unsupported]", 13) != 0)
            LOG_ERROR("lmxxf: Create rc={} err={}", createRc, err);
        return SessionStartFailed("Create", err);
    }
    if (verbose && api->table.GetStatus)
    {
        char st[256] {};
        api->table.GetStatus(ctx, st, sizeof st);
        LOG_INFO("lmxxf: after Create status={}", st);
    }
    int32_t prepRc;
    {
        // mochizuki's runtime creates its Vulkan instance and device here. They are not the game's, so the
        // Vulkan hooks leave them alone, as they do for the devices DXVK and vkd3d create.
        ScopedSkipVulkanHooks skipVulkanHooks {};
        ScopedCreatingD3DDevice creatingDevice {};
        prepRc = api->table.PrepareSession(ctx);
    }
    if (prepRc != LMXXF_NR_OK)
    {
        char err[256] {};
        if (api->table.GetLastError)
            api->table.GetLastError(err, sizeof err);
        if (Lmxxf() || (verbose && std::strncmp(err, "[unsupported]", 13) != 0))
            LOG_ERROR("lmxxf: PrepareSession rc={} err={} (attempt {})", prepRc, err, attempt);
        api->table.Destroy(ctx);
        return SessionStartFailed("PrepareSession", err);
    }
    session = ctx;
    sessionReady = true;
    sessionFailures = 0;
    api->controlsSentValid = false; // a new session starts from the runtime's defaults
    SetStatus("lmxxf: session ready");
    return true;
}

ID3D12Resource* LmxxfBackend::FinishRecord(ID3D12GraphicsCommandList* recordCmd, void* jobHandle, void* privateOutput)
{
    if (api->table.RecordInputs(session, jobHandle, recordCmd) != LMXXF_NR_OK)
    {
        api->table.CancelUnsubmitted(session, jobHandle);
        SetStatus("lmxxf: RecordInputs failed");
        return nullptr;
    }
    const HRESULT splitHr = LmxxfCut::TrySplitAtEvaluate(recordCmd);
    if (FAILED(splitHr) || splitHr == S_FALSE)
    {
        api->table.CancelUnsubmitted(session, jobHandle);
        SetStatus(FAILED(splitHr) ? "lmxxf: Split failed" : "lmxxf: Split returned S_FALSE");
        return nullptr;
    }
    if (api->table.RecordOutputs(session, jobHandle, recordCmd) != LMXXF_NR_OK)
    {
        api->table.CancelUnsubmitted(session, jobHandle);
        SetStatus("lmxxf: RecordOutputs failed");
        return nullptr;
    }
    LmxxfCut::SetPendingEnqueue(session, jobHandle, api->table.EnqueueHip, api->table.GetLastError, recordCmd, queue);
    LmxxfCut::ArmBetweenSlot();
    {
        std::lock_guard lock(jobMutex);
        pendingJobInfo.job = jobHandle;
        pendingJobInfo.cmd = recordCmd;
        pendingList.store(recordCmd, std::memory_order_release);
    }
    SetStatus("lmxxf: Record ok (pending EnqueueHip)");
    return reinterpret_cast<ID3D12Resource*>(privateOutput);
}

// Own lists (NR without upscaling): the producer list gets the inputs now, the consumer list the outputs later
// (RecordConsumer), and SubmitOwnLists runs the network between them on the queue.
ID3D12Resource* LmxxfBackend::RecordProducer(ID3D12GraphicsCommandList* producer, void* jobHandle, void* privateOutput)
{
    if (api->table.RecordInputs(session, jobHandle, producer) != LMXXF_NR_OK)
    {
        api->table.CancelUnsubmitted(session, jobHandle);
        SetStatus("lmxxf: RecordInputs failed");
        return nullptr;
    }
    std::lock_guard lock(jobMutex);
    pendingJobInfo.job = jobHandle;
    pendingJobInfo.cmd = producer;
    SetStatus("lmxxf: running");
    return reinterpret_cast<ID3D12Resource*>(privateOutput);
}

bool LmxxfBackend::RecordConsumer(ID3D12GraphicsCommandList* consumer)
{
    std::lock_guard lock(jobMutex);
    if (!pendingJobInfo.job)
        return false;
    if (api->table.RecordOutputs(session, pendingJobInfo.job, consumer) == LMXXF_NR_OK)
    {
        pendingJobInfo.cmd = consumer;
        return true;
    }
    api->table.CancelUnsubmitted(session, pendingJobInfo.job);
    pendingJobInfo = {};
    SetStatus("lmxxf: RecordOutputs failed");
    return false;
}

void LmxxfBackend::SubmitOwnLists(ID3D12CommandQueue* q, ID3D12CommandList* producer, ID3D12CommandList* consumer,
                                  ExecFn exec)
{
    std::lock_guard lock(jobMutex);
    const auto run = [&](ID3D12CommandList* list)
    {
        if (exec)
            exec(q, 1, &list);
        else
            q->ExecuteCommandLists(1, &list);
    };
    run(producer);
    if (pendingJobInfo.job)
    {
        // The network waits for the producer on this queue and the consumer waits for the network; a failed enqueue
        // leaves the zeroed output (LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK), which the decoder turns into the frame.
        if (api->table.EnqueueHip(session, pendingJobInfo.job, q) != LMXXF_NR_OK)
        {
            char err[256] {};
            api->table.GetLastError(err, sizeof err);
            SetStatus((std::string("lmxxf: EnqueueHip failed: ") + err).c_str());
        }
    }
    run(consumer);
    if (pendingJobInfo.job)
        api->table.Retire(session, pendingJobInfo.job);
    pendingJobInfo = {};
}

void LmxxfBackend::CancelOwnLists()
{
    std::lock_guard lock(jobMutex);
    if (pendingJobInfo.job && session)
        api->table.CancelUnsubmitted(session, pendingJobInfo.job);
    pendingJobInfo = {};
}

namespace
{
float HalfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h >> 15) << 31, exp = (h >> 10) & 31, man = h & 1023;
    uint32_t bits;
    if (exp == 0)
        return (sign ? -1.f : 1.f) * std::ldexp(float(man), -24);
    if (exp == 31)
        bits = sign | 0x7F800000u | (man << 13);
    else
        bits = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// A typeless exposure as its float format (Dying Light: The Beast's is R32_TYPELESS); any other as it is.
DXGI_FORMAT ExposureFloat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return f;
    }
}

// The exposure formats the danielblnc bridge takes.
bool ExposureFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R32G32_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT ||
           f == DXGI_FORMAT_R16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

// The first channel of a texel in one of those formats, or NaN for any other.
float ExposureTexel(const uint8_t* p, DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    {
        float f;
        std::memcpy(&f, p, sizeof f);
        return f;
    }
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    {
        uint16_t h;
        std::memcpy(&h, p, sizeof h);
        return HalfToFloat(h);
    }
    default:
        return NAN;
    }
}
} // namespace

// The game's exposure as mochizuki takes it: what the frame's linear colour is multiplied by to be shown, the texel
// times the exposure scale over the pre-exposure (as the danielblnc bridge hands it to its runtime). The texel is
// read kExposureSlots - 1 frames late, which an auto exposure that eases over many frames does not notice. It is the
// RenoDX stage's feed (MochizukiRenoFeedMode decides whether the stage takes it). 0 when the game hands no usable
// texture or the ring is not full yet.
float LmxxfBackend::GameExposure(ID3D12GraphicsCommandList* cmd, const AmdPreSr::Frame& frame)
{
    Api& a = *api;
    if (!frame.exposure)
    {
        a.exposureFrames = 0;
        return 0.f;
    }
    const D3D12_RESOURCE_DESC ed = frame.exposure->GetDesc();
    const DXGI_FORMAT format = ExposureFloat(ed.Format);
    if (ed.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || ed.SampleDesc.Count != 1 || !ExposureFormat(format))
    {
        a.exposureFrames = 0;
        return 0.f;
    }
    if (!a.exposureRing)
    {
        ID3D12Device* dev = nullptr;
        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&dev))) || !dev)
            return 0.f;
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = Api::kExposureSlots * Api::kExposureSlotBytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr, IID_PPV_ARGS(&a.exposureRing));
        dev->Release();
        void* mapped = nullptr;
        if (FAILED(hr) || FAILED(a.exposureRing->Map(0, nullptr, &mapped)))
        {
            if (a.exposureRing)
                a.exposureRing->Release();
            a.exposureRing = nullptr;
            return 0.f;
        }
        a.exposureMapped = static_cast<const uint8_t*>(mapped);
    }
    const uint32_t slot = uint32_t(a.exposureFrames % Api::kExposureSlots);
    D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
    src.pResource = frame.exposure;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = a.exposureRing;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = slot * Api::kExposureSlotBytes;
    dst.PlacedFootprint.Footprint = { format, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
    const D3D12_BOX box { 0, 0, 0, 1, 1, 1 };
    const bool move = frame.exposureState != D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = frame.exposure;
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = frame.exposureState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (move)
        cmd->ResourceBarrier(1, &b);
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    if (move)
    {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cmd->ResourceBarrier(1, &b);
    }
    const float pre = std::isfinite(frame.preExposure) && frame.preExposure > 0 ? frame.preExposure : 1.f;
    const float scale = std::isfinite(frame.exposureScale) && frame.exposureScale > 0 ? frame.exposureScale : 1.f;
    a.exposureFormat[slot] = format;
    a.exposureScale[slot] = scale / pre;
    if (++a.exposureFrames < Api::kExposureSlots)
        return 0.f;
    // The oldest slot: the one the next frame writes.
    const uint32_t old = uint32_t(a.exposureFrames % Api::kExposureSlots);
    const float e =
        ExposureTexel(a.exposureMapped + old * Api::kExposureSlotBytes, a.exposureFormat[old]) * a.exposureScale[old];
    return std::isfinite(e) && e > 0.f ? e : 0.f;
}

ID3D12Resource* LmxxfBackend::Record(ID3D12GraphicsCommandList* cmd, const AmdPreSr::Frame& frame,
                                     const AmdPreSr::Settings& settings)
{
    std::lock_guard recordLock(recordMutex);
    firstPass = nullptr;
    if (exiting.load(std::memory_order_acquire))
        return nullptr;
    // A queue change seen by Submitted is acted on here, where no other Record can be inside the session.
    ID3D12CommandQueue* newQueue = nullptr;
    {
        std::lock_guard lock(jobMutex);
        if (migrateTo && !pendingJobInfo.job)
        {
            newQueue = migrateTo;
            migrateTo = nullptr;
        }
    }
    if (newQueue)
        MigrateQueue(newQueue);
    if (historyResetPending.exchange(false, std::memory_order_acq_rel))
        ResetHistoryNow();
    bool previousPending = false;
    bool sameList = false;
    ID3D12CommandList* pendingCmd = nullptr;
    {
        std::lock_guard lock(jobMutex);
        previousPending = pendingJobInfo.job != nullptr;
        pendingCmd = pendingJobInfo.cmd;
        sameList = previousPending && pendingCmd == cmd;
    }
    if (sameList)
    {
        // A second Evaluate on the list that already carries this frame's job (DlssNrPreUpscale, DualFeature).
        // The frame keeps its NR, so neither the frame id nor the history moves.
        return nullptr;
    }
    // Every other decline before PrepareFrame is a frame without NR. frameId does not advance for it, so the history
    // is reset instead: the next frame would otherwise pass the runtime's continuity test and blend history two
    // frames old with one frame of motion.
    const auto declineFrame = [this](const char* why) -> ID3D12Resource*
    {
        if (session && api->table.ResetHistory)
            api->table.ResetHistory(session);
        SetStatus(why);
        return nullptr;
    };
    if (!cmd || !frame.colour)
        return declineFrame("lmxxf: Record missing cmd/colour");
    if (frame.afterUpscale && Lmxxf())
        return declineFrame("lmxxf: runs before Super Resolution only; set ApplyAfterRR=false (NO NR)");
    if (previousPending)
    {
        // The previous Evaluate already returned its output to SR. Cancelling its
        // job here would leave that recorded continuation consuming invalid data.
        // It clears when its list is executed (Submitted), or Reset or released unexecuted (ListRecycled).
        // A second Evaluate of that same frame on another list (NBA 2K27, ~5 ms later) keeps the history: the next
        // frame follows the pending one. Same colour, extent and jitter, and, when the game hands no jitter, well
        // inside one frame interval; anything else may be a new frame and resets as before.
        const LastFrame& lf = lastFrame;
        const bool jittered = frame.jitterX != 0.f || frame.jitterY != 0.f;
        const ULONGLONG elapsed = GetTickCount64() - lf.tick;
        const bool sameFrame = !frame.reset && frame.colour == lf.colour && frame.width == lf.width &&
                               frame.height == lf.height && frame.jitterX == lf.jitterX &&
                               frame.jitterY == lf.jitterY && (jittered || (lf.interval && elapsed * 2 < lf.interval));
        if (sameFrame)
        {
            if (!repeatLogged)
            {
                repeatLogged = true;
                LOG_INFO("lmxxf: second Evaluate of the pending frame on cmd {:X} (pending cmd {:X}, {} ms later); "
                         "declined, history kept (logged once)",
                         (size_t) cmd, (size_t) pendingCmd, elapsed);
            }
            SetStatus("lmxxf: previous frame not submitted (original Color; NO NR)");
            return nullptr;
        }
        return declineFrame("lmxxf: previous frame not submitted (original Color; NO NR)");
    }
    // Crucially before EnsureSession: controls do not load the runtime, prepare HIP,
    // or submit HIP. split-original only cuts the game list for boundary validation.
    if (diagnostic != LmxxfProbe::Mode::Off)
        return RecordDiagnostic(cmd, frame);
    // Never substitute Color from an earlier Evaluate to work around an unsubmitted producer.
    // With its own lists (UseOwnLists) the host submits the two halves itself: no game list to split.
    DlssNr::Submission::ILogicalCommandList* logical = nullptr;
    if (!ownLists && (FAILED(cmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                                 reinterpret_cast<void**>(&logical))) ||
                      !logical))
    {
        if (!boundaryLogged)
        {
            boundaryLogged = true;
            // Whose list it is: the module its methods come from (the driver's for a native list).
            HMODULE owner = nullptr;
            char name[MAX_PATH] {};
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   *reinterpret_cast<LPCSTR*>(cmd), &owner))
                GetModuleFileNameA(owner, name, MAX_PATH);
            LOG_WARN("lmxxf: Evaluate on cmd {:X}, which is no proxy (methods from {}), likely made before the "
                     "swapchain by a module not taken for the game's; frames on it get no NR (logged once)",
                     (size_t) cmd, name);
        }
        return declineFrame("lmxxf: same-frame boundary unavailable (original Color; NO NR)");
    }
    const bool ineligible = logical && logical->IsSplitIneligible();
    char status[160] {};
    if (ineligible)
    {
        const char* reason = logical->SplitRejectionReason();
        std::snprintf(status, sizeof(status), "%s: split ineligible: %s (NO NR)", Lmxxf() ? "lmxxf" : "mochizuki",
                      reason ? reason : "unknown");
    }
    if (logical)
        logical->Release();
    if (ineligible)
    {
        static std::atomic<uint32_t> s_ineligibleCount { 0 };
        const uint32_t c = s_ineligibleCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (c <= 10 || (c <= 100 && (c % 20 == 0)) || (c <= 1000 && (c % 100 == 0)) || (c % 1000 == 0))
        {
            LOG_WARN("{} (frame #{})", status, c);
        }
        return declineFrame(status);
    }
    if (!EnsureSession())
        return nullptr;
    RefreshRuntimeStatus();
    if (!Lmxxf())
    {
        SendControls();
        if (api->setExposure)
            api->setExposure(session, GameExposure(cmd, frame));
    }

    D3D12_RESOURCE_DESC desc = frame.colour->GetDesc();
    LmxxfNrFrameInfo fi {};
    fi.struct_size = FrameInfoSize();
    // Only frames that reach PrepareFrame count, so a duplicate Evaluate cannot break the history's continuity.
    fi.frame_id = ++frameId;
    {
        const ULONGLONG now = GetTickCount64();
        lastFrame = { frame.colour,
                      frame.width,
                      frame.height,
                      frame.jitterX,
                      frame.jitterY,
                      now,
                      lastFrame.tick ? now - lastFrame.tick : 0 };
    }
    fi.command_list = cmd;
    fi.color_width = frame.width ? frame.width : static_cast<uint32_t>(desc.Width);
    fi.color_height = frame.height ? frame.height : static_cast<uint32_t>(desc.Height);
    fi.color = frame.colour;
    fi.color_state = static_cast<uint32_t>(frame.colourState);
    fi.flags = LMXXF_NR_FRAME_FLAG_STRENGTH | LMXXF_NR_FRAME_FLAG_DEBUG_VIEW;
    if (Lmxxf())
    {
        fi.transfer_strength = std::clamp(Config::Instance()->DlssNrTransferStrength.value_or_default(), 0.0f, 2.0f);
        fi.color_strength = std::clamp(Config::Instance()->DlssNrColourStrength.value_or_default(), 0.0f, 2.0f);
        fi.debug_view = Config::Instance()->DlssNrDebugView.value_or_default();
        fi.model_scale = settings.modelScale;
        fi.passes = settings.passes;
        fi.flags |= LMXXF_NR_FRAME_FLAG_STYLE;
        fi.style = std::min(Config::Instance()->LmxxfStyle.value_or_default(), 2u);
        if (Config::Instance()->LmxxfVitReuse.value_or_default())
            fi.flags |= LMXXF_NR_FRAME_FLAG_VIT_REUSE;
        if (Config::Instance()->LmxxfCompact1080.value_or_default())
            fi.flags |= LMXXF_NR_FRAME_FLAG_COMPACT_1080;
        if (Config::Instance()->LmxxfOriginalModel.value_or_default())
            fi.flags |= LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL;
        if (Config::Instance()->LmxxfPassEstimate.value_or_default())
            fi.flags |= LMXXF_NR_FRAME_FLAG_PASS3_ESTIMATE;
        if (Config::Instance()->LmxxfSkinProtect.value_or_default())
            fi.flags |= LMXXF_NR_FRAME_FLAG_SKIN_PROTECT;
        if (Config::Instance()->LmxxfAutoExposure.value_or_default())
            fi.flags |= LMXXF_NR_FRAME_FLAG_AUTO_WHITE;
    }
    else
    {
        // mochizuki's own keys; its network has no debug view, so debug_view stays 0. The RenoDX stage composes with
        // its own strengths and scale: these fields keep the runtime's defaults.
        const MochizukiFrameSettings mz = MochizukiFrame(*Config::Instance());
        fi.transfer_strength = fi.color_strength = fi.model_scale = 1.0f;
        fi.passes = mz.passes;
        fi.flags |= mz.flags;
    }
    // mochizuki has a switch of its own. An INI without MochizukiTemporal keeps what LmxxfTemporal gave it before.
    const bool temporalOn = (!Lmxxf() && Config::Instance()->MochizukiTemporal.has_value())
                                ? Config::Instance()->MochizukiTemporal.value()
                                : Config::Instance()->LmxxfTemporal.value_or_default();
    const bool temporal = temporalOn && frame.motion;
    if (temporal)
    {
        // mochizuki: the vectors with the jitter's step added (MochizukiJitterHistory), on this list
        // (MochizukiGuides).
        ID3D12Resource* motion = frame.motion;
        D3D12_RESOURCE_STATES motionState = frame.motionState;
        if (!Lmxxf())
        {
            // The step from this frame's jitter to the last one's, in render pixels, as the stabilizer takes it; in
            // the vectors' own units it is that over their extent's share of the render grid and their scale.
            const bool jitterOn = Config::Instance()->MochizukiJitterHistory.value_or_default();
            // Every frame goes through the shift while it is on, a zero step included: the vectors change format
            // there, and a frame that skipped it would make the runtime rebuild its buffers and drop the history.
            const bool shift = jitterOn && !frame.afterUpscale && frame.motionScaleX != 0.f &&
                               frame.motionScaleY != 0.f && frame.width && frame.height;
            float offsetX = 0.f, offsetY = 0.f;
            if (shift && haveJitter)
            {
                const float mw = float(frame.motionWidth ? frame.motionWidth : frame.width);
                const float mh = float(frame.motionHeight ? frame.motionHeight : frame.height);
                offsetX = (lastJitterX - frame.jitterX) * (mw / float(frame.width)) / frame.motionScaleX;
                offsetY = (lastJitterY - frame.jitterY) * (mh / float(frame.height)) / frame.motionScaleY;
            }
            lastJitterX = frame.jitterX;
            lastJitterY = frame.jitterY;
            haveJitter = true;
            try
            {
                if (!guides)
                    guides = std::make_unique<MochizukiGuides>(device);
                // No depth reaches the guides or the runtime: the RenoDX stage's network reads the vectors at the
                // pixel, so nothing is copied for it (MochizukiUseDepth is kept for a network outside the stage).
                AmdPreSr::Frame in = frame;
                in.depth = nullptr;
                const MochizukiGuides::Out g = guides->Prepare(cmd, in, shift, offsetX, offsetY);
                motion = g.motion;
                motionState = g.motionState;
            }
            catch (const std::exception& e)
            {
                static bool logged = false;
                if (!std::exchange(logged, true))
                    LOG_WARN("mochizuki: guides left as the game gave them: {}", e.what());
            }
        }
        fi.flags |= LMXXF_NR_FRAME_FLAG_TEMPORAL;
        fi.motion = motion;
        fi.motion_state = static_cast<uint32_t>(motionState);
        fi.motion_width = frame.motionWidth;
        fi.motion_height = frame.motionHeight;
        fi.motion_scale_x = frame.motionScaleX;
        fi.motion_scale_y = frame.motionScaleY;
        fi.reset = frame.reset ? 1u : 0u;
        fi.smooth_threshold =
            std::clamp(Config::Instance()->LmxxfSmoothThreshold.value_or_default(), 0.f, 255.f) / 255.f;
        fi.smooth_strength = std::clamp(Config::Instance()->LmxxfSmoothStrength.value_or_default(), 0.f, 1.f);
        if (Lmxxf() && Config::Instance()->LmxxfSmoothResidual.value_or_default())
            fi.flags |= LMXXF_NR_FRAME_FLAG_SMOOTH_RESIDUAL;
    }

    // Person protection's first pass. A runtime from before it refuses the flag; the frame then goes again without.
    const bool wantFirst = settings.firstPass && fi.passes > 1 && !firstPassRefused;
    if (wantFirst)
        fi.flags |= LMXXF_NR_FRAME_FLAG_FIRST_PASS;
    LmxxfNrJob job {};
    job.struct_size = wantFirst ? uint32_t(sizeof(job)) : uint32_t(offsetof(LmxxfNrJob, first_pass_output));
    int32_t frameRc = api->table.PrepareFrame(session, &fi, &job);
    if (wantFirst && frameRc == LMXXF_NR_INVALID_ARGUMENT)
    {
        firstPassRefused = true;
        LOG_WARN("{}: the runtime gives no first pass (older than this OptiScaler); person protection stays off with "
                 "more than one pass",
                 Lmxxf() ? "lmxxf" : "mochizuki");
        fi.flags &= ~LMXXF_NR_FRAME_FLAG_FIRST_PASS;
        job = {};
        job.struct_size = uint32_t(offsetof(LmxxfNrJob, first_pass_output));
        frameRc = api->table.PrepareFrame(session, &fi, &job);
    }
    if (frameRc == LMXXF_NR_UNAVAILABLE && !Lmxxf())
    {
        // Building its network, or declining this frame; the reason goes to the menu, the details to its own log.
        char err[256] {};
        api->table.GetLastError(err, sizeof err);
        SetStatus((std::string("lmxxf: ") + err).c_str());
        return nullptr;
    }
    if (frameRc != LMXXF_NR_OK || !job.handle || !job.private_output)
    {
        char err[256] {};
        if (api->table.GetLastError)
            api->table.GetLastError(err, sizeof err);
        const auto enqueue = LmxxfCut::LastEnqueueDiagnostic();
        static unsigned prepareFrameFailLogs = 0;
        static unsigned prepareFrameRebuilds = 0;
        static constexpr GUID kStreamlineRiid = {
            0xADEC44E2, 0x61F0, 0x45C3, { 0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF }
        };
        IUnknown* sessId = nullptr;
        if (queue)
            queue->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&sessId));
        IUnknown* sessSl = nullptr;
        if (queue)
            queue->QueryInterface(kStreamlineRiid, reinterpret_cast<void**>(&sessSl));

        IUnknown* execId = nullptr;
        if (enqueue.queue)
            enqueue.queue->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&execId));
        IUnknown* execSl = nullptr;
        if (enqueue.queue)
            enqueue.queue->QueryInterface(kStreamlineRiid, reinterpret_cast<void**>(&execSl));

        if (prepareFrameFailLogs < 3 || (prepareFrameFailLogs % 30) == 0)
            LOG_ERROR("lmxxf: PrepareFrame rc={} handle={} out={} err={} lastEnqueueRc={:X} lastEnqueueErr={} "
                      "sessQ={:p}(t={},id={:p},sl={:p}) execQ={:p}(t={},id={:p},sl={:p}) {}x{} (fail#{})",
                      frameRc, job.handle != nullptr, job.private_output != nullptr, err, enqueue.rc,
                      enqueue.error.data(), reinterpret_cast<void*>(queue),
                      queue ? static_cast<int>(queue->GetDesc().Type) : -1, reinterpret_cast<void*>(sessId),
                      reinterpret_cast<void*>(sessSl), reinterpret_cast<void*>(enqueue.queue),
                      enqueue.queue ? static_cast<int>(enqueue.queue->GetDesc().Type) : -1,
                      reinterpret_cast<void*>(execId), reinterpret_cast<void*>(execSl), fi.color_width, fi.color_height,
                      prepareFrameFailLogs + 1);

        if (sessId)
            sessId->Release();
        if (sessSl)
            sessSl->Release();
        if (execId)
            execId->Release();
        if (execSl)
            execSl->Release();
        ++prepareFrameFailLogs;
        // Menu/resize: runtime drains/rebuilds codec on rebind/geometry; if still failing,
        // drop host session so the next Record EnsureSession starts clean.
        const bool rebindish = (err[0] && (std::strstr(err, "rebind") || std::strstr(err, "geometry")));
        if (rebindish && (prepareFrameFailLogs <= 2 || (prepareFrameFailLogs % 4) == 0))
        {
            LOG_WARN("lmxxf: PrepareFrame fail -> host session rebuild #{}", ++prepareFrameRebuilds);
            if (session && api && api->table.Destroy)
                api->table.Destroy(session);
            session = nullptr;
            sessionReady = false;
            ClearRuntimeStatus();
            SetStatus("lmxxf: session rebuild after PrepareFrame fail");
        }
        else
        {
            SetStatus("lmxxf: PrepareFrame failed");
        }
        return nullptr;
    }
    {
        // Logged again whenever temporal history or the Style is switched, so its state (or why it failed) reaches
        // the log. A new Style rebuilds the network.
        static int loggedTemporal = -1, loggedStyle = -1;
        const int style = (fi.flags & LMXXF_NR_FRAME_FLAG_STYLE) ? int(fi.style) : -1;
        if ((loggedTemporal != int(temporal) || loggedStyle != style) && api->table.GetStatus)
        {
            char st[256] {};
            api->table.GetStatus(session, st, sizeof st);
            LOG_INFO("lmxxf: after PrepareFrame HIP/net geometry status={} style={}", st, style);
            loggedTemporal = int(temporal);
            loggedStyle = style;
        }
    }

    if (ownLists)
        return RecordProducer(cmd, job.handle, job.private_output);
    ID3D12Resource* result = FinishRecord(cmd, job.handle, job.private_output);
    if (result && wantFirst)
        firstPass = static_cast<ID3D12Resource*>(job.first_pass_output);
    static uint64_t recordEvalCount = 0;
    const auto rEval = ++recordEvalCount;
    auto& p = LmxxfCut::Pending();
    const auto lastRc = static_cast<unsigned>(p.lastEnqueueRc.load(std::memory_order_relaxed));
    const auto submitFails = DlssNr::Submission::g_submissionFailures.load(std::memory_order_relaxed);

    if (rEval <= 5)
    {
        LOG_INFO("lmxxf nr: eval={} output={} betweenHits={} enqueueCalls={} skippedHits={} lastEnqueueRc={:X} "
                 "transfer={:.2f} color={:.2f} debugView={} scale={:.2f} producerSubmitted={} continuationSubmitted={} "
                 "submitFailures={}",
                 rEval, static_cast<void*>(result), p.betweenHits.load(std::memory_order_relaxed),
                 p.enqueueCalls.load(std::memory_order_relaxed), p.skippedHits.load(std::memory_order_relaxed), lastRc,
                 fi.transfer_strength, fi.color_strength, fi.debug_view, fi.model_scale,
                 DlssNr::Submission::g_splitSubmissions.load(std::memory_order_relaxed),
                 DlssNr::Submission::g_continuationSubmissions.load(std::memory_order_relaxed), submitFails);
    }
    else if ((lastRc != 0 && lastRc != static_cast<unsigned>(LmxxfCut::kEnqueueSkipped) &&
              lastRc != static_cast<unsigned>(LmxxfCut::kEnqueueQueueMismatch)) ||
             submitFails > 0)
    {
        static uint64_t warnCount = 0;
        if (++warnCount <= 5 || (warnCount % 120 == 0))
        {
            LOG_WARN("lmxxf nr anomaly: eval={} lastEnqueueRc={:X} submitFailures={} skippedHits={}", rEval, lastRc,
                     submitFails, p.skippedHits.load(std::memory_order_relaxed));
        }
    }
    else if (rEval % 120 == 0)
    {
        LOG_DEBUG("lmxxf nr: eval={} output={} betweenHits={} enqueueCalls={} skippedHits={} lastEnqueueRc={:X} "
                  "transfer={:.2f} color={:.2f} debugView={} scale={:.2f} producerSubmitted={} "
                  "continuationSubmitted={} submitFailures={}",
                  rEval, static_cast<void*>(result), p.betweenHits.load(std::memory_order_relaxed),
                  p.enqueueCalls.load(std::memory_order_relaxed), p.skippedHits.load(std::memory_order_relaxed), lastRc,
                  fi.transfer_strength, fi.color_strength, fi.debug_view, fi.model_scale,
                  DlssNr::Submission::g_splitSubmissions.load(std::memory_order_relaxed),
                  DlssNr::Submission::g_continuationSubmissions.load(std::memory_order_relaxed), submitFails);
    }
    return result;
}

ID3D12Resource* LmxxfBackend::RecordDiagnostic(ID3D12GraphicsCommandList* cmd, const AmdPreSr::Frame& frame)
{
    const auto seq = ++evaluateSequence_;
    const auto id = ++probeEvaluateId;
    const bool sampled = id <= 3 || id % 120 == 0;
    const auto d = frame.colour->GetDesc();
    const UINT w = frame.width ? frame.width : static_cast<UINT>(d.Width);
    const UINT h = frame.height ? frame.height : d.Height;
    ID3D12Resource* output = nullptr;
    const char* reason = "original_no_nr";
    LmxxfProbe::Evidence ev {};
    ev.evaluateId = id;
    ev.list = cmd;
    ev.sampled = sampled;
    ev.mode = diagnostic;

    if (diagnostic == LmxxfProbe::Mode::CodecPassthrough)
    {
        DlssNr::Submission::ILogicalCommandList* logical = nullptr;
        const bool isProxy = SUCCEEDED(cmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                                           reinterpret_cast<void**>(&logical))) &&
                             logical;
        if (!isProxy)
        {
            reason = "boundary_not_proxy";
            ++boundaryRejects;
            SetStatus("lmxxf diagnostic: codec-passthrough REJECTED (not proxy; original Color)");
        }
        else
        {
            ++boundaryProxyHits;
            if (logical->IsSplitIneligible())
            {
                reason = logical->SplitRejectionReason();
                ++boundaryRejects;
                char state[256] {};
                snprintf(state, sizeof state, "lmxxf diagnostic: codec-passthrough REJECTED (%s; original Color)",
                         reason);
                SetStatus(state);
            }
            else if (!EnsureSession())
            {
                reason = "ensure_session_failed";
                ++boundaryRejects;
                SetStatus("lmxxf diagnostic: codec-passthrough REJECTED (session failed; original Color)");
            }
            else
            {
                D3D12_RESOURCE_DESC desc = frame.colour->GetDesc();
                LmxxfNrFrameInfo fi {};
                fi.struct_size = FrameInfoSize();
                fi.frame_id = ++frameId; // Record leaves the increment to whichever path calls PrepareFrame.
                fi.command_list = cmd;
                fi.color_width = frame.width ? frame.width : static_cast<uint32_t>(desc.Width);
                fi.color_height = frame.height ? frame.height : static_cast<uint32_t>(desc.Height);
                fi.color = frame.colour;
                fi.color_state = static_cast<uint32_t>(frame.colourState);
                fi.flags = LMXXF_NR_FRAME_FLAG_STRENGTH | LMXXF_NR_FRAME_FLAG_DEBUG_VIEW |
                           LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH;
                if (Lmxxf())
                {
                    fi.transfer_strength =
                        std::clamp(Config::Instance()->DlssNrTransferStrength.value_or_default(), 0.0f, 2.0f);
                    fi.color_strength =
                        std::clamp(Config::Instance()->DlssNrColourStrength.value_or_default(), 0.0f, 2.0f);
                    fi.debug_view = Config::Instance()->DlssNrDebugView.value_or_default();
                    // The same Style as the frames around it: another would rebuild the network.
                    fi.flags |= LMXXF_NR_FRAME_FLAG_STYLE;
                    fi.style = std::min(Config::Instance()->LmxxfStyle.value_or_default(), 2u);
                    if (Config::Instance()->LmxxfCompact1080.value_or_default())
                        fi.flags |= LMXXF_NR_FRAME_FLAG_COMPACT_1080;
                    if (Config::Instance()->LmxxfOriginalModel.value_or_default())
                        fi.flags |= LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL;
                }
                else
                {
                    fi.transfer_strength = fi.color_strength = 1.0f;
                    fi.flags |= MochizukiFrame(*Config::Instance()).flags; // the same network as the frames around it
                }
                fi.model_scale = 1.0f;

                LmxxfNrJob job {};
                job.struct_size = uint32_t(offsetof(LmxxfNrJob, first_pass_output));
                const int32_t frameRc = api->table.PrepareFrame(session, &fi, &job);
                if (frameRc != LMXXF_NR_OK || !job.handle || !job.private_output)
                {
                    char err[256] {};
                    if (api->table.GetLastError)
                        api->table.GetLastError(err, sizeof err);
                    static uint64_t diagPrepareFails = 0;
                    if (++diagPrepareFails <= 5 || (diagPrepareFails % 300 == 0))
                    {
                        LOG_ERROR("lmxxf diagnostic: codec-passthrough PrepareFrame rc={} handle={} out={} err='{}' "
                                  "{}x{} (fail#{})",
                                  frameRc, job.handle != nullptr, job.private_output != nullptr, err, fi.color_width,
                                  fi.color_height, diagPrepareFails);
                    }
                    reason = "prepare_frame_failed";
                    ++boundaryRejects;
                    SetStatus((std::string("lmxxf diagnostic: codec-passthrough PrepareFrame failed: ") + err).c_str());
                }
                else if (api->table.RecordInputs(session, job.handle, cmd) != LMXXF_NR_OK)
                {
                    char err[256] {};
                    if (api->table.GetLastError)
                        api->table.GetLastError(err, sizeof err);
                    LOG_ERROR("lmxxf diagnostic: codec-passthrough RecordInputs failed: {}", err);
                    reason = "record_inputs_failed";
                    ++boundaryRejects;
                    SetStatus("lmxxf diagnostic: codec-passthrough RecordInputs failed");
                }
                else
                {
                    const HRESULT cutHr = logical->SplitSegments();
                    if (cutHr != S_OK)
                    {
                        char err[256] {};
                        if (api->table.GetLastError)
                            api->table.GetLastError(err, sizeof err);
                        LOG_ERROR("lmxxf diagnostic: codec-passthrough Split failed: hr={:X} err='{}'",
                                  static_cast<unsigned>(cutHr), err);
                        reason = "boundary_split_failed";
                        ++boundaryRejects;
                        SetStatus("lmxxf diagnostic: codec-passthrough Split failed");
                    }
                    else
                    {
                        ++boundaryCuts;
                        if (api->table.RecordOutputs(session, job.handle, cmd) != LMXXF_NR_OK)
                        {
                            char err[256] {};
                            if (api->table.GetLastError)
                                api->table.GetLastError(err, sizeof err);
                            LOG_ERROR("lmxxf diagnostic: codec-passthrough RecordOutputs failed: {}", err);
                            reason = "record_outputs_failed";
                            ++boundaryRejects;
                            SetStatus("lmxxf diagnostic: codec-passthrough RecordOutputs failed");
                        }
                        else
                        {
                            output = reinterpret_cast<ID3D12Resource*>(job.private_output);
                            reason = "codec_passthrough_recorded";
                            SetStatus("lmxxf diagnostic: codec-passthrough (NO HIP; encode->decode passthrough)");
                        }
                    }
                }
            }
        }
        if (logical)
            logical->Release();
        if (sampled)
            LOG_INFO("lmxxf boundary: eval={} proxy={} reason={} proxyHits={} cutsRecorded={} rejected={} output={} "
                     "unsplitSubmitted={} producerSubmitted={} continuationSubmitted={} submitFailures={}",
                     id, isProxy, reason, boundaryProxyHits, boundaryCuts, boundaryRejects, static_cast<void*>(output),
                     DlssNr::Submission::g_unsplitProxySubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_splitSubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_continuationSubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_submissionFailures.load(std::memory_order_relaxed));
    }
    else if (LmxxfProbe::NeedsOpenListProxy(diagnostic))
    {
        DlssNr::Submission::ILogicalCommandList* logical = nullptr;
        HRESULT cutHr = S_FALSE;
        const bool isProxy = SUCCEEDED(cmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                                           reinterpret_cast<void**>(&logical))) &&
                             logical;
        if (!isProxy)
            reason = "boundary_not_proxy";
        else
        {
            ++boundaryProxyHits;
            if (diagnostic == LmxxfProbe::Mode::ProxyOriginal)
                reason = "proxy_original_no_nr";
            else if (logical->IsSplitIneligible())
                reason = logical->SplitRejectionReason();
            else
            {
                cutHr = logical->SplitSegments();
                if (cutHr == S_OK)
                {
                    ++boundaryCuts;
                    reason = "split_original_recorded_no_nr";
                }
                else
                    reason = "boundary_split_failed";
            }
        }
        if (logical)
            logical->Release();
        if (!isProxy || (diagnostic == LmxxfProbe::Mode::SplitOriginal && cutHr != S_OK))
            ++boundaryRejects;
        char state[256] {};
        snprintf(state, sizeof state, "lmxxf diagnostic: %s (original Color; NO NR)", reason);
        SetStatus(state);
        if (sampled)
            LOG_INFO("lmxxf boundary: eval={} proxy={} cutHr={:X} proxyHits={} cutsRecorded={} rejected={} "
                     "unsplitSubmitted={} producerSubmitted={} continuationSubmitted={} submitFailures={} (counts are "
                     "NOT GPU completion)",
                     id, isProxy, static_cast<unsigned>(cutHr), boundaryProxyHits, boundaryCuts, boundaryRejects,
                     DlssNr::Submission::g_unsplitProxySubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_splitSubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_continuationSubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_submissionFailures.load(std::memory_order_relaxed));
    }
    else if (diagnostic == LmxxfProbe::Mode::CopyCurrent)
    {
        output = colorProbe.Record(device, cmd, frame.colour, frame.colourState, w, h);
        reason = colorProbe.Reason();
        SetStatus(output ? "lmxxf diagnostic: copy-current (NO NR; recorded, not GPU-complete)"
                         : "lmxxf diagnostic: copy-current REJECTED (original Color; see log)");
    }
    else if (diagnostic == LmxxfProbe::Mode::StagingCurrent || diagnostic == LmxxfProbe::Mode::StagingPrevious)
    {
        auto r = stagingProbe.Record(diagnostic, device, cmd, frame.colour, frame.colourState, w, h, seq);
        output = r.output;
        reason = r.reason;
        ev.epoch = r.epoch;
        ev.sourceSequence = r.sourceSequence;
        ev.age = r.age;
        ev.priming = r.priming;
        const char* modeName =
            (diagnostic == LmxxfProbe::Mode::StagingCurrent) ? "staging-current" : "staging-previous";
        if (r.priming)
        {
            char buf[256];
            snprintf(buf, sizeof buf, "lmxxf diagnostic: %s PRIMING (no previous; original Color)", modeName);
            SetStatus(buf);
        }
        else if (output)
        {
            char buf[256];
            snprintf(buf, sizeof buf, "lmxxf diagnostic: %s (NO NR; age=%u src=%llu)", modeName, r.age,
                     static_cast<unsigned long long>(r.sourceSequence));
            SetStatus(buf);
        }
        else
        {
            char buf[256];
            snprintf(buf, sizeof buf, "lmxxf diagnostic: %s REJECTED (%s)", modeName, reason);
            SetStatus(buf);
        }
    }
    else if (diagnostic == LmxxfProbe::Mode::Original)
        SetStatus("lmxxf diagnostic: original (NO NR, no replacement)");
    else
    {
        reason = "invalid_diagnostic_option";
        SetStatus("lmxxf diagnostic: INVALID option (original Color; no HIP)");
    }

    ev.expectedColor = output ? output : frame.colour;
    ev.copied = output != nullptr;
    LmxxfProbe::CurrentEvidence() = ev;

    if (sampled)
    {
        const auto st = stagingProbe.GetStats();
        LOG_INFO("lmxxf probe: eval={} seq={} list={} color={} output={} mv={} depth={} valid={}x{} allocation={}x{} "
                 "format={} colorState={} preExposure={} exposureScale={} reset={} reason={} mode={} epoch={} age={} "
                 "srcSeq={} priming={} window={} best={} pins={} bytes={} (CPU record only)",
                 id, seq, static_cast<void*>(cmd), static_cast<void*>(frame.colour), static_cast<void*>(output),
                 static_cast<void*>(frame.motion), static_cast<void*>(frame.depth), w, h, d.Width, d.Height,
                 static_cast<unsigned>(d.Format), static_cast<unsigned>(frame.colourState), frame.preExposure,
                 frame.exposureScale, frame.reset, reason, static_cast<int>(diagnostic), ev.epoch, ev.age,
                 ev.sourceSequence, ev.priming, st.currentWindowLength, st.bestWindowLength,
                 stagingProbe.CaptureCount() + stagingProbe.OutputCount() + colorProbe.EntryCount(),
                 stagingProbe.AllocatedBytes() + colorProbe.AllocatedBytes());
    }
    return output;
}

// Daniel uses PendingListIndex to isolate a private neural list from a multi-list batch.
// lmxxf HIP sits in ExecuteExpanded's between-slot on the game proxy itself, so isolation
// is unnecessary: AmdBridge::ExecuteBatch always calls ExecuteExpanded when ExpandEnabled().
int LmxxfBackend::PendingListIndex(UINT, ID3D12CommandList* const*) const { return -1; }

void LmxxfBackend::Submitting(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) {}

// The bridge's settle gate (AmdBridge Run): every change that restarts its wait. A game that never settles shows here
// what keeps changing; the first 30, then every 100th.
void LmxxfBackend::TraceBoundary(const std::string& reason)
{
    static std::atomic<uint32_t> count { 0 };
    const uint32_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 30 || n % 100 == 0)
        LOG_INFO("{}: {} (#{})", Lmxxf() ? "lmxxf" : "mochizuki", reason, n);
}

void LmxxfBackend::Submitted(ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists)
{
    if (exiting.load(std::memory_order_acquire))
        return;
    int32_t retireRc = LMXXF_NR_OK;
    char retireErr[256] {};
    ID3D12CommandQueue* sessionQueue = nullptr;
    bool migrate = false;
    bool keptQueue = false;
    {
        // Retire, releasing the pending job and the queue decision are one step: the next Record cannot prepare a
        // job on this session until the runtime has retired this one.
        std::lock_guard lock(jobMutex);
        bool containsCmd = false;
        if (lists)
        {
            for (UINT i = 0; i < count; ++i)
            {
                if (pendingJobInfo.cmd && lists[i] == pendingJobInfo.cmd)
                {
                    containsCmd = true;
                    break;
                }
            }
        }
        if (containsCmd)
        {
            if (session && pendingJobInfo.job && api && api->table.Retire)
            {
                retireRc = api->table.Retire(session, pendingJobInfo.job);
                if (retireRc != LMXXF_NR_OK && api->table.GetLastError)
                    api->table.GetLastError(retireErr, sizeof retireErr);
            }
            pendingJobInfo = {};
            pendingList.store(nullptr, std::memory_order_release);
            sessionQueue = queue;
            if (q && !SameObject(q, queue))
            {
                if (api && api->anyQueue)
                    keptQueue = true;
                else
                {
                    // Draining and destroying here would pull the session from under a Record on the render
                    // thread; Record migrates it before its next use instead.
                    q->AddRef();
                    if (migrateTo)
                        migrateTo->Release();
                    migrateTo = q;
                    migrate = true;
                }
            }
        }
        // Other UE/FG lists may submit before the list containing this Evaluate.
        // Only that list can retire the pending HIP slot.
        LmxxfCut::ClearPendingEnqueueIfSubmitted(count, lists);
    }
    if (retireRc != LMXXF_NR_OK)
        LOG_ERROR("lmxxf: Retire rc={} err={} submitQ={:p} sessQ={:p}", retireRc, retireErr, reinterpret_cast<void*>(q),
                  reinterpret_cast<void*>(sessionQueue));
    if (migrate)
        LOG_WARN("lmxxf: queue transition detected (current={:p}, actual={:p}); the next Record drains both and "
                 "migrates the session",
                 reinterpret_cast<void*>(sessionQueue), reinterpret_cast<void*>(q));
    if (keptQueue)
    {
        static std::atomic<bool> logged { false };
        if (!logged.exchange(true, std::memory_order_relaxed))
            LOG_INFO("mochizuki: list executed on queue {:p}, not the session's {:p}; the runtime follows any queue, "
                     "so the session stays",
                     reinterpret_cast<void*>(q), reinterpret_cast<void*>(sessionQueue));
    }
}

// Any thread, from inside a proxy's Reset or final Release (the proxy is deleted only after this returns).
void LmxxfBackend::OnListRecycled(ID3D12CommandList* list)
{
    if (auto* backend = g_recycleTarget.load(std::memory_order_acquire))
        backend->ListRecycled(list);
}

// The game Reset or released the list carrying the pending job without executing it: the job's input and output
// copies went with that recording, and Submitted will never retire it. Without this every later Record would decline
// as "previous frame not submitted" until that list pointer is executed again, which a released list never is. Only
// this event proves the list dead; one that is merely late still holds copies into the session's buffers, so nothing
// ages a job out. Submitted runs inside the game's Execute, before the game can Reset the list, so this never races
// a real submit.
void LmxxfBackend::ListRecycled(ID3D12CommandList* list)
{
    if (!list || pendingList.load(std::memory_order_acquire) != list)
        return;
    void* job = nullptr;
    void* jobSession = nullptr;
    int32_t (*cancel)(void*, void*) = nullptr;
    int32_t (*resetHistory)(void*) = nullptr;
    {
        std::lock_guard lock(jobMutex);
        if (exiting.load(std::memory_order_acquire) || !pendingJobInfo.job || pendingJobInfo.cmd != list)
            return;
        // The job stays pending until the runtime has cancelled it: Record keeps declining, so it can neither
        // prepare the next job on this session nor rebuild or migrate the session meanwhile. The session and the
        // table were set before the job was, and stay while it is pending.
        job = pendingJobInfo.job;
        pendingJobInfo.cmd = nullptr;
        pendingList.store(nullptr, std::memory_order_release);
        cancelling = true;
        jobSession = session;
        cancel = api->table.CancelUnsubmitted;
        resetHistory = api->table.ResetHistory;
    }
    if (jobSession && cancel)
        cancel(jobSession, job);
    LmxxfCut::ClearPendingEnqueueIf(list, job);
    // The cancelled frame took a frame id, so the next frame would otherwise pass as continuous with it.
    if (jobSession && resetHistory)
        resetHistory(jobSession);
    // Set while the job is still pending, so the next Record's status replaces this one and not the reverse.
    SetStatus("lmxxf: frame's command list reset or released without being executed; its NR job was cancelled");
    {
        std::lock_guard lock(jobMutex);
        if (pendingJobInfo.job == job && !pendingJobInfo.cmd)
            pendingJobInfo = {};
        cancelling = false;
    }
    static std::atomic<uint32_t> s_cancelCount { 0 };
    const uint32_t c = s_cancelCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (c <= 10 || (c <= 100 && (c % 20 == 0)) || (c <= 1000 && (c % 100 == 0)) || (c % 1000 == 0))
    {
        LOG_WARN("lmxxf: command list {:p} was reset or released without being executed; its NR job was cancelled "
                 "and the history reset (event #{})",
                 reinterpret_cast<void*>(list), c);
    }
}

// Stops OnListRecycled reaching this backend. It does not wait for a call already running.
void LmxxfBackend::UnregisterListRecycled()
{
    LmxxfBackend* self = this;
    if (!g_recycleTarget.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel))
        return;
    auto listener = &LmxxfBackend::OnListRecycled;
    DlssNr::Submission::Hooks::g_onListRecycled.compare_exchange_strong(listener, nullptr, std::memory_order_acq_rel);
}

// Render thread, under recordMutex, with no job pending, so nothing else is inside the session. Takes over the
// reference migrateTo held on q.
void LmxxfBackend::MigrateQueue(ID3D12CommandQueue* q)
{
    LmxxfCut::ClearPendingEnqueue();
    // 1. Drain the actual queue q that executed the last producer and continuation:
    const bool actualDrained = DrainQueue(device, q);

    // 2. Drain the old session queue:
    const bool sessionDrained =
        !session || ((api && api->table.Drain) ? (api->table.Drain(session) == LMXXF_NR_OK) : false);

    if (actualDrained && sessionDrained)
    {
        if (session && api && api->table.Destroy)
            api->table.Destroy(session);
        LOG_INFO("lmxxf: session cleanly destroyed after verified dual queue drain; queue updated to {:p}",
                 reinterpret_cast<void*>(q));
    }
    else
    {
        // CRITICAL SAFETY: If either queue failed to drain, GPU may still be referencing
        // old session resources. We MUST NOT call Destroy(session) to avoid GPU Use-After-Free.
        LOG_ERROR("lmxxf: GPU drain failed during queue transition (actualDrained={}, sessionDrained={}); "
                  "abandoning old session without Destroy to prevent GPU UAF",
                  actualDrained, sessionDrained);
    }
    session = nullptr;
    sessionReady = false;
    ClearRuntimeStatus();
    ID3D12CommandQueue* previous = nullptr;
    {
        std::lock_guard lock(jobMutex);
        previous = queue;
        queue = q;
    }
    if (previous)
        previous->Release();
    DlssNr::AmdBridge::UpdateConfirmedRenderQueue(q);
    SetStatus(actualDrained && sessionDrained
                  ? "lmxxf: session migrated to new render queue"
                  : "lmxxf: queue migration completed with abandoned undrained session (GPU safety fallback)");
}

bool LmxxfBackend::Shutdown()
{
    LmxxfCut::DisarmBetweenSlot();
    {
        std::lock_guard lock(jobMutex);
        pendingJobInfo = {};
        pendingList.store(nullptr, std::memory_order_release);
        if (migrateTo)
        {
            migrateTo->Release();
            migrateTo = nullptr;
        }
    }
    if (session && api && api->table.Destroy)
    {
        api->table.Destroy(session);
        session = nullptr;
    }
    sessionReady = false;
    ClearRuntimeStatus();
    if (runtimeDll)
    {
        FreeLibrary(reinterpret_cast<HMODULE>(runtimeDll));
        runtimeDll = nullptr;
    }
    if (api)
    {
        api->table = {};
        api->getFeatures = nullptr;
        api->setControls = nullptr;
        api->getInfo = nullptr;
        api->getControlDefaults = nullptr;
        api->setExposure = nullptr;
        api->anyQueue = false;
        api->controlsSentValid = false;
        if (api->exposureRing)
        {
            api->exposureRing->Release();
            api->exposureRing = nullptr;
            api->exposureMapped = nullptr;
        }
        api->exposureFrames = 0;
    }
    SetStatus("lmxxf: shutdown");
    return true;
}

// The exit hook runs while the render and submit threads may still be inside the runtime. The session is destroyed
// only if its recording, submission and job locks come free within 2 s and no job is being cancelled; otherwise it is
// left to the OS. mochizuki ends through MochizukiNrDestroyAtExit, which does not wait out a network build (Destroy
// would, up to a minute) and gives a game queue 2 s to drain: a Vulkan device, its implicit layers and the runtime's
// threads left alive into the DLLs' detach are the suspects in Cyberpunk 2077 2.5's access violation box on every
// exit. A runtime without that export is left to the OS as before. Neither DLL is unloaded: FreeLibrary would unmap
// code a thread still running may be in.
void LmxxfBackend::OnProcessExit()
{
    UnregisterListRecycled();
    LmxxfCut::DisarmBetweenSlot();
    {
        std::lock_guard lock(jobMutex);
        pendingJobInfo = {};
        pendingList.store(nullptr, std::memory_order_release);
        exiting.store(true, std::memory_order_release);
    }
    if (!session)
        return;
    if (!Lmxxf() && !(api && api->destroyAtExit))
    {
        SetStatus("lmxxf: process exit (session left to the OS)");
        return;
    }
    const ULONGLONG deadline = GetTickCount64() + 2000;
    const auto acquire = [deadline](auto& lock)
    {
        while (!lock.try_lock())
        {
            if (GetTickCount64() >= deadline)
                return false;
            Sleep(1);
        }
        return true;
    };
    // SubmitOwnLists holds jobMutex while it executes (which takes g_executeMu): jobMutex is only tried, never waited.
    std::unique_lock recordLock(recordMutex, std::defer_lock);
    std::unique_lock executeLock(DlssNr::Submission::Hooks::g_executeMu, std::defer_lock);
    std::unique_lock jobLock(jobMutex, std::defer_lock);
    if (!acquire(recordLock) || !acquire(executeLock) || !acquire(jobLock))
    {
        LOG_WARN("lmxxf: process exit while the runtime is in use; session left to the OS");
        return;
    }
    if (!session)
        return;
    // ListRecycled starts no cancel once exiting is set; one already started may still be inside the session.
    if (cancelling)
    {
        LOG_WARN("lmxxf: process exit while a job is being cancelled; session left to the OS");
        return;
    }
    if (!Lmxxf())
    {
        if (api->destroyAtExit(session) != LMXXF_NR_OK)
        {
            SetStatus("lmxxf: process exit during a network build (session left to the OS)");
            return;
        }
    }
    else if (api && api->table.Destroy)
        api->table.Destroy(session);
    session = nullptr;
    sessionReady = false;
    ClearRuntimeStatus();
    SetStatus("lmxxf: process exit (session ended)");
}

void LmxxfBackend::InvalidateHistory()
{
    if (exiting.load(std::memory_order_acquire))
        return;
    // The menu calls this too and must never wait on a Record: while one runs, the next Record resets instead.
    std::unique_lock recordLock(recordMutex, std::try_to_lock);
    if (!recordLock.owns_lock())
    {
        historyResetPending.store(true, std::memory_order_release);
        return;
    }
    ResetHistoryNow();
}

// Caller holds recordMutex.
void LmxxfBackend::ResetHistoryNow()
{
    if (session && api && api->table.ResetHistory)
    {
        const int32_t resetRc = api->table.ResetHistory(session);
        if (resetRc != LMXXF_NR_OK)
        {
            static std::atomic<uint32_t> resetFailures { 0 };
            const uint32_t n = resetFailures.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 3 || n % 120 == 0)
            {
                char err[256] {};
                if (api->table.GetLastError)
                    api->table.GetLastError(err, sizeof err);
                LOG_ERROR("lmxxf: ResetHistory rc={} err={} (fail#{})", resetRc, err, n);
            }
        }
    }
    stagingProbe.InvalidateEpoch();
}

std::string LmxxfBackend::Status() const
{
    std::lock_guard lock(statusMutex);
    return status;
}

std::string LmxxfBackend::RuntimeStatus() const
{
    std::lock_guard lock(statusMutex);
    return runtimeStatus;
}

bool LmxxfBackend::GraphicsRestartNeeded(UINT) const { return false; }
} // namespace DlssNr::Backend
