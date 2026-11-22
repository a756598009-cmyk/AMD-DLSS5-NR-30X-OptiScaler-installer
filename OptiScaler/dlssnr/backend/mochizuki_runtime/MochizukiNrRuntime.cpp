// MochizukiNrRuntime.dll: mochizuki0323's Vulkan network (third_party/mochizuki) behind the lmxxf C ABI.
//
// The network runs on a Vulkan device of its own, on the game's adapter. Before the cut, the game's colour and
// motion vectors are copied into D3D12 buffers that Vulkan imports. Between the two halves of the game's command
// list, the game queue signals a shared fence (toVk), a Vulkan submit waits on it, runs the network and writes the
// result into a third shared buffer, then signals a second shared fence (fromVk), which the game queue waits on.
// After the cut, the result is copied into the texture handed to Super Resolution. Any game queue may run the list
// (MOCHIZUKI_NR_FEATURE_ANY_QUEUE): a frame from another queue than the last one waits for the last one first. The
// model's controls come from the frame (strengths, model scale, passes) and from MochizukiNrSetControls
// (MochizukiNrControls.h), sanitised before the network sees them.
//
// Networks are built off the render thread, on a second queue of the Vulkan device when its family has one, one core
// build at a time in the process. A build for another model scale, pass count or colour encoding then runs while the
// frames keep the old network, which is swapped for the new one when it is ready; another frame extent or format
// replaces the network and the frame's buffers without waiting for the game's queue. The render thread never waits for
// the GPU there: a network the frames no longer use is freed on another thread once the Vulkan side has finished the
// frames that used it. Destroy during a build stops it between two pipelines and waits for it 2 s at most
// (Session::Abandon).
//
// Dynamic resolution (MochizukiNrControls::drs_mode): by default (exact) the network and the frame's buffers are built
// for the frame's render subrect, so every change of it rebuilds the network. In auto, once a subrect smaller than the
// colour allocation is seen, they are built for a bucket instead (Session::DrsExtent): the largest subrect seen,
// rounded up to 64 within the allocation. The frame's buffers are then keyed on the allocation, each frame copies only
// its subrect, the part of the bucket past it repeats the subrect's edge, the motion vectors are laid over the subrect
// in the bucket's uv, and the history restarts whenever the subrect changes. A subrect equal to the bucket runs as it
// does in exact. A game that reallocates its colour at every step instead (Cyberpunk 2077) engages auto too: allocations
// of one format and aspect share a bucket held at the largest of them, and the frame's buffers and Super Resolution's
// texture stay at that size. Always buckets every frame, also across allocations.
//
// Errors (NrError): only a lost device ends the session. It, and a Vulkan side that stops finishing frames (the
// watchdog), releases every game queue waiting on fromVk. A failed network build or buffer allocation is retried
// when the frame asks for something else, or after a backoff when it ran out of memory; a build or buffers the VRAM
// budget cannot hold are refused before anything is allocated, and a network whose buffers are refused goes with
// them; a frame the network fails on passes its colour through.
#include "LmxxfNrApi.h"
#define MOCHIZUKI_NR_RUNTIME_EXPORTS
#include "MochizukiNrControls.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <winternl.h> // NTSTATUS, for d3dkmthk.h
#include <d3dkmthk.h> // D3DKMTQueryVideoMemoryInfo, for the VRAM check (VideoMemory)
#include <bcrypt.h>   // SHA-256, for MakeExperimentalWeights
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "mz_interpose.h"
#include "mz_rdna3.h"
#include "nr_ini_network.hpp"
#include "nr_log.hpp"
#include "nr_pipeline_binary.hpp"
#include "nr_renodx.hpp"
#include "nr_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <clocale>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
thread_local char g_lastError[256] = {};

void SetError(const char* text)
{
    std::strncpy(g_lastError, text ? text : "", sizeof(g_lastError) - 1);
    g_lastError[sizeof(g_lastError) - 1] = 0;
}

int32_t Fail(int32_t status, const char* text)
{
    SetError(text);
    return status;
}

std::wstring DllDirectory()
{
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LmxxfNrGetApi), &mod);
    if (!mod)
        return {};
    // GetModuleFileNameW truncates to the buffer and returns its size then: grow it until the name fits.
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD n = GetModuleFileNameW(mod, path.data(), DWORD(path.size()));
        if (!n)
            return {};
        if (n < path.size())
        {
            path.resize(n);
            break;
        }
        if (path.size() >= 65536)
            return {};
        path.resize(path.size() * 2);
    }
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    path.resize(slash);
    return path;
}

// Where dlssnr-amd\ (shaders, model, prewarm) and the log are: beside the DLL, or else beside the game's exe. RE
// Engine games load every DLL from a copy in _storage_\ and leave the folders behind.
std::wstring DataDirectory()
{
    static const std::wstring dir = []
    {
        const std::wstring dll = DllDirectory();
        if (GetFileAttributesW((dll + L"\\dlssnr-amd").c_str()) != INVALID_FILE_ATTRIBUTES)
            return dll;
        std::wstring exe(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
        exe.resize(n < exe.size() ? n : 0);
        const size_t slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos &&
            GetFileAttributesW((exe.substr(0, slash) + L"\\dlssnr-amd").c_str()) != INVALID_FILE_ATTRIBUTES)
            return exe.substr(0, slash);
        return dll;
    }();
    return dir;
}

// dlssnr-amd\float-colour-shared: this game's colour has switched between R11G11B10_FLOAT and R16G16B16A16_FLOAT, so
// its float frames run on one RGBA16F network (PrepareFrame). An empty file; the installer takes it on uninstall.
std::wstring FloatSharedMarker() { return DataDirectory() + L"\\dlssnr-amd\\float-colour-shared"; }

bool FloatShared() { return GetFileAttributesW(FloatSharedMarker().c_str()) != INVALID_FILE_ATTRIBUTES; }

void MarkFloatShared()
{
    const HANDLE f = CreateFileW(FloatSharedMarker().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
}

// The runtime's folder as the core's RuntimeConfig::root, in UTF-8: with LC_CTYPE set to UTF-8 (UseUtf8Paths), the
// core's narrow paths reach the file system unchanged, whatever the ANSI code page is.
std::string RootUtf8(const std::wstring& dir)
{
    const int n = dir.empty() ? 0
                              : WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, dir.c_str(), int(dir.size()),
                                                    nullptr, 0, nullptr, nullptr);
    std::string root(size_t(std::max(n, 0)), '\0');
    if (n <= 0 || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, dir.c_str(), int(dir.size()), root.data(), n,
                                      nullptr, nullptr) != n)
        throw std::runtime_error("the runtime's folder name cannot be converted to UTF-8");
    return root;
}

// The core opens its files through narrow paths (std::filesystem::path from std::string, std::ifstream, fopen),
// which the CRT decodes in the LC_CTYPE code page, the ANSI one by default. Only LC_CTYPE, never LC_ALL (number
// formatting stays "C"), and only safe because this DLL is built /MT: the locale belongs to its own CRT, not to the
// game's.
void UseUtf8Paths()
{
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       if (!std::setlocale(LC_CTYPE, ".UTF8"))
                           nr::logf("[mochizuki] setlocale(LC_CTYPE, \".UTF8\") failed; a folder name outside the "
                                    "ANSI code page will fail the network build");
                   });
}

// Fault injection for mz_stress, read once per process at the first Create; nothing happens when unset. N counts a
// session's Enqueue calls (EnqueueHip) from 1.
struct TestHooks
{
    bool failStart = false;      // MZ_TEST_FAIL_START=1: Start throws right after vkCreateInstance
    bool unsupported = false;    // MZ_TEST_UNSUPPORTED=1: Start throws the [unsupported] path after vkCreateInstance
    uint64_t slotTimeoutAt = 0;  // MZ_TEST_SLOT_TIMEOUT_AT=N: Enqueue N finds its slot's wait timed out
    uint64_t enqueueThrowAt = 0; // MZ_TEST_ENQUEUE_THROW_AT=N: Enqueue N throws while recording the network
    uint64_t deviceLostAt = 0;   // MZ_TEST_DEVICE_LOST_AT=N: Enqueue N throws a lost device
    bool buildOomOnce = false;   // MZ_TEST_BUILD_OOM_ONCE=1: the process's first network build runs out of memory
    uint64_t vramBudgetMb = 0;   // MZ_TEST_VRAM_BUDGET_MB=N: the VRAM check sees a budget of N MB
    uint64_t networkMb = 0;      // MZ_TEST_NETWORK_MB=N: the VRAM check estimates every network at N MB
    uint64_t dropSubmitAt = 0;   // MZ_TEST_DROP_SUBMIT_AT=N: Enqueue N skips its Vulkan submit, the game still waits
};

const TestHooks& Hooks()
{
    static const TestHooks hooks = []
    {
        auto on = [](const char* name)
        {
            const char* value = std::getenv(name);
            return value && !std::strcmp(value, "1");
        };
        auto number = [](const char* name) -> uint64_t
        {
            const char* value = std::getenv(name);
            return value ? std::strtoull(value, nullptr, 10) : 0;
        };
        TestHooks h;
        h.failStart = on("MZ_TEST_FAIL_START");
        h.unsupported = on("MZ_TEST_UNSUPPORTED");
        h.slotTimeoutAt = number("MZ_TEST_SLOT_TIMEOUT_AT");
        h.enqueueThrowAt = number("MZ_TEST_ENQUEUE_THROW_AT");
        h.deviceLostAt = number("MZ_TEST_DEVICE_LOST_AT");
        h.buildOomOnce = on("MZ_TEST_BUILD_OOM_ONCE");
        h.vramBudgetMb = number("MZ_TEST_VRAM_BUDGET_MB");
        h.networkMb = number("MZ_TEST_NETWORK_MB");
        h.dropSubmitAt = number("MZ_TEST_DROP_SUBMIT_AT");
        return h;
    }();
    return hooks;
}
std::atomic<bool> g_buildOomInjected { false }; // MZ_TEST_BUILD_OOM_ONCE: once per process

// The FP16 networks' checks (mz_rdna3.h): their verdicts, by device and driver, in dlssnr-amd\rdna3-check.txt or
// rdna2-check.txt. MZ_RDNA3_CHECK=0 (MZ_RDNA2_CHECK=0) skips them, MZ_RDNA3_CHECK=pipelines the RDNA3 layout probes
// only (diagnostics: MZ_FORCE_RDNA3 on an RX 9000 then builds and runs). The RDNA2 network has no layout probes: its
// fragments are arrays in a layout of its own, not the hardware's.
const wchar_t* Fp16Tag(nr::Arch arch)
{
    return arch == nr::Arch::rdna2 ? (nr::rdna2_int8 ? L"rdna2-int8" : L"rdna2") : L"rdna3";
}
std::wstring Fp16CheckFile(nr::Arch arch) { return DataDirectory() + L"\\dlssnr-amd\\" + Fp16Tag(arch) + L"-check.txt"; }

bool Fp16CheckSkips(nr::Arch arch, bool layout)
{
    char v[16] {};
    if (!GetEnvironmentVariableA(arch == nr::Arch::rdna2 ? "MZ_RDNA2_CHECK" : "MZ_RDNA3_CHECK", v, sizeof v))
        return false;
    return !std::strcmp(v, "0") || (layout && !std::strcmp(v, "pipelines"));
}

// A diagnostic switch set to 0 (MZ_CHECK_CACHE=0: the pipeline check keeps no machine code for the build).
bool MzEnvOff(const char* name)
{
    char v[8] {};
    return GetEnvironmentVariableA(name, v, sizeof v) && !std::strcmp(v, "0");
}

// An FP16 network's pipelines (shaders-rdna3 or shaders-rdna2, and the RenoDX stage's in shaders\renodx), and the key
// the pipeline check's verdict is kept under: the device's, and a hash of the files' names and sizes, so other shaders
// (a new release) are checked again.
std::vector<std::wstring> Fp16PipelineFiles(nr::Arch arch, std::string& key)
{
    std::vector<std::wstring> files;
    std::error_code ec;
    for (const std::wstring& dir :
         { std::wstring(L"\\dlssnr-amd\\shaders-") + Fp16Tag(arch), std::wstring(L"\\dlssnr-amd\\shaders\\renodx") })
        for (const auto& e : std::filesystem::recursive_directory_iterator(DataDirectory() + dir, ec))
            if (e.path().extension() == L".spv")
                files.push_back(e.path().wstring());
    std::sort(files.begin(), files.end());
    uint64_t h = 1469598103934665603ull;
    for (const std::wstring& f : files)
    {
        const std::wstring name = std::filesystem::path(f).filename().wstring() + L"|" +
                                  std::to_wstring(std::filesystem::file_size(f, ec));
        for (wchar_t c : name)
            h = (h ^ uint64_t(c)) * 1099511628211ull;
    }
    char text[24];
    std::snprintf(text, sizeof text, "-%016llx", static_cast<unsigned long long>(h));
    key += text;
    return files;
}

// Adapters the network cannot run on: a Start that threw "[unsupported] ..." (a missing extension or feature, or
// no Vulkan device for the adapter). That does not change while the process lives, so a later PrepareSession on the
// same adapter fails at once, without a VkInstance, and the reason is logged once.
std::atomic<bool> g_unsupported { false };
std::mutex g_unsupportedMutex;
std::vector<std::pair<LUID, std::string>> g_unsupportedAdapters;

std::string UnsupportedReason(LUID luid)
{
    if (!g_unsupported.load(std::memory_order_acquire))
        return {};
    std::lock_guard lock(g_unsupportedMutex);
    for (const auto& [adapter, reason] : g_unsupportedAdapters)
        if (!std::memcmp(&adapter, &luid, sizeof luid))
            return reason;
    return {};
}

void RememberUnsupported(LUID luid, const char* reason)
{
    std::lock_guard lock(g_unsupportedMutex);
    for (const auto& known : g_unsupportedAdapters)
        if (!std::memcmp(&known.first, &luid, sizeof luid))
            return;
    g_unsupportedAdapters.emplace_back(luid, reason);
    g_unsupported.store(true, std::memory_order_release);
    nr::logf("[mochizuki] %s; not retried on this adapter", reason);
}

// The runtime's own lines and the network's, in mochizuki_nr.log beside dlssnr-amd\ (DataDirectory).
void LogLine(const char* line)
{
    static std::mutex mutex;
    static const std::wstring path = DataDirectory() + L"\\mochizuki_nr.log";
    std::lock_guard lock(mutex);
    FILE* f = _wfopen(path.c_str(), L"ab");
    if (!f)
        return;
    SYSTEMTIME t {};
    GetLocalTime(&t);
    std::fprintf(f, "%02u:%02u:%02u.%03u %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
    std::fclose(f);
}

// An error, classified for recovery. Only DeviceLost ends the session (Guard); every other kind fails one call, one
// network build (EnsureNetwork's retry policy), the frame's buffers (PrepareFrame) or one frame (Passthrough).
struct NrError : std::runtime_error
{
    enum Kind
    {
        DeviceLost,
        Timeout,
        OutOfMemory,
        Unsupported,
        Invalid,
        Other,
    } kind;
    VkResult vk;
    HRESULT hr;

    NrError(Kind k, const std::string& what, VkResult v = VK_SUCCESS, HRESULT h = S_OK)
        : std::runtime_error(what), kind(k), vk(v), hr(h)
    {
    }
};

const char* KindName(NrError::Kind k)
{
    static const char* const names[] = { "device lost", "timeout", "out of memory", "unsupported", "invalid", "error" };
    return names[k];
}

NrError::Kind KindOf(VkResult r)
{
    switch (r)
    {
    case VK_ERROR_DEVICE_LOST:
        return NrError::DeviceLost;
    case VK_TIMEOUT:
        return NrError::Timeout;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return NrError::OutOfMemory;
    default:
        return NrError::Other;
    }
}

NrError::Kind KindOf(HRESULT hr)
{
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
        return NrError::DeviceLost;
    if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
        return NrError::Timeout;
    if (hr == E_OUTOFMEMORY)
        return NrError::OutOfMemory;
    if (hr == E_INVALIDARG)
        return NrError::Invalid;
    return NrError::Other;
}

// Any exception's kind: an NrError's own; the core's (nrvk::check: "<call>: VkResult=N") by its VkResult;
// bad_alloc and invalid_argument by type. It neither allocates nor throws, so the builder may call it.
NrError::Kind KindOf(const std::exception& e)
{
    if (const auto* error = dynamic_cast<const NrError*>(&e))
        return error->kind;
    if (dynamic_cast<const std::bad_alloc*>(&e))
        return NrError::OutOfMemory;
    if (dynamic_cast<const std::invalid_argument*>(&e))
        return NrError::Invalid;
    const char* what = e.what();
    if (const char* at = std::strstr(what, "VkResult="))
    {
        char* end = nullptr;
        const long r = std::strtol(at + 9, &end, 10);
        if (end != at + 9)
            return KindOf(VkResult(r));
    }
    if (!std::strncmp(what, "[unsupported] ", 14))
        return NrError::Unsupported;
    return NrError::Other;
}

void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char text[160];
        std::snprintf(text, sizeof text, "%s failed, HRESULT 0x%08lX", what, static_cast<unsigned long>(hr));
        throw NrError(KindOf(hr), text, VK_SUCCESS, hr);
    }
}

void VkCheck(VkResult r, const char* what)
{
    if (r != VK_SUCCESS)
        throw NrError(KindOf(r), std::string(what) + " failed, VkResult " + std::to_string(int(r)), r);
}

// This process's use of the adapter's local video memory and its budget, in bytes, through the kernel thunk DXGI's
// QueryVideoMemoryInfo is built on: the same numbers, D3D12 and Vulkan allocations alike. The adapter is opened once,
// in Start (inside the host's device-creation scope), so a check on the render thread creates no DXGI factory or
// adapter, meets none of OptiScaler's DXGI hooks (nor any of its gdi32 ones, which cover D3DKMTQueryAdapterInfo only)
// and the DLL imports no dxgi.dll. VK_EXT_memory_budget cannot stand in: the AMD driver reports this process's usage
// as 0 while the Vulkan device holds no memory of its own, as at the first build.
struct VideoMemory
{
    D3DKMT_HANDLE adapter = 0;
    decltype(&D3DKMTQueryVideoMemoryInfo) query = nullptr;
    decltype(&D3DKMTCloseAdapter) close = nullptr;

    // The adapter behind luid; without it there is no VRAM check.
    void Open(LUID luid)
    {
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi)
            gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); // kept: a system DLL
        if (!gdi)
            return;
        const auto open =
            reinterpret_cast<decltype(&D3DKMTOpenAdapterFromLuid)>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
        query =
            reinterpret_cast<decltype(&D3DKMTQueryVideoMemoryInfo)>(GetProcAddress(gdi, "D3DKMTQueryVideoMemoryInfo"));
        close = reinterpret_cast<decltype(&D3DKMTCloseAdapter)>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
        D3DKMT_OPENADAPTERFROMLUID o {};
        o.AdapterLuid = luid;
        if (open && query && close && open(&o) >= 0)
            adapter = o.hAdapter;
    }

    // False when unknown: no adapter, a failed query, or a budget of 0 (which would refuse everything).
    bool Read(UINT64& usage, UINT64& budget) const
    {
        if (!adapter)
            return false;
        D3DKMT_QUERYVIDEOMEMORYINFO q {}; // hProcess null: this process
        q.hAdapter = adapter;
        q.MemorySegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
        if (query(&q) < 0 || !q.Budget)
            return false;
        usage = q.CurrentUsage;
        budget = q.Budget;
        return true;
    }

    void Close()
    {
        if (adapter)
        {
            D3DKMT_CLOSEADAPTER c {};
            c.hAdapter = adapter;
            close(&c);
        }
        adapter = 0;
    }
};

// A vkEnumerate* list, whole: call(count, items) is the enumeration. A list that grew between the count and the fill
// (VK_INCOMPLETE) is asked for again, one that shrank is cut to what was written. An error throws VkCheck's plain
// message, never "[unsupported] ...": a failed enumeration (an ICD or layer failing once, out of memory) may pass,
// so only a list that was read in full may show that something is absent.
template <typename T, typename Call> std::vector<T> VkEnumerate(const char* what, Call call)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        uint32_t count = 0;
        VkCheck(call(&count, nullptr), what);
        if (!count)
            return {};
        std::vector<T> items(count);
        const VkResult r = call(&count, items.data());
        if (r == VK_INCOMPLETE)
            continue;
        VkCheck(r, what);
        items.resize(count);
        return items;
    }
    throw std::runtime_error(std::string(what) + " kept returning VK_INCOMPLETE");
}

struct Format
{
    VkFormat vk = VK_FORMAT_UNDEFINED;
    UINT bytes = 0;
};

// The Vulkan format with the same bits as the D3D12 one, so a buffer copy carries the texels unchanged.
Format ColourFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return { VK_FORMAT_R16G16B16A16_SFLOAT, 8 };
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return { VK_FORMAT_R32G32B32A32_SFLOAT, 16 };
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return { VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4 };
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4 };
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return { VK_FORMAT_R8G8B8A8_UNORM, 4 };
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return { VK_FORMAT_B8G8R8A8_UNORM, 4 };
    // Upstream 743326d's further colour formats. The runtime blits them, so PrepareFrame takes them only where this GPU
    // can (Blittable). R9G9B9E5 is RE Engine's upscaler colour (Resident Evil Requiem, Monster Hunter Wilds): three
    // 9-bit mantissas sharing a 5-bit exponent, linear like R11G11B10.
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        return { VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4 };
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return { VK_FORMAT_R16G16B16A16_UNORM, 8 };
    case DXGI_FORMAT_R16G16B16A16_SNORM:
        return { VK_FORMAT_R16G16B16A16_SNORM, 8 };
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        return { VK_FORMAT_R8G8B8A8_SNORM, 4 };
    case DXGI_FORMAT_B5G6R5_UNORM:
        return { VK_FORMAT_R5G6B5_UNORM_PACK16, 2 };
    case DXGI_FORMAT_B5G5R5A1_UNORM:
        return { VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2 };
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return { VK_FORMAT_A4R4G4B4_UNORM_PACK16, 2 };
    default:
        return {};
    }
}

// The formats the runtime takes by blit alone (upstream 743326d), which not every GPU can blit.
bool NeedsBlitCheck(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_A4R4G4B4_UNORM_PACK16:
        return true;
    default:
        return false;
    }
}

// The runtime blits the vectors into its own RG32F image, so any float format with at least two channels will do;
// only red and green are read.
Format MotionFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_TYPELESS:
        return { VK_FORMAT_R16G16_SFLOAT, 4 };
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS:
        return { VK_FORMAT_R32G32_SFLOAT, 8 };
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return { VK_FORMAT_R16G16B16A16_SFLOAT, 8 };
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return { VK_FORMAT_R32G32B32A32_SFLOAT, 16 };
    default:
        return {};
    }
}

// The game's depth as the network's temporal path reads it: one float or unorm channel, which the core blits into its
// own R32F image. A depth/stencil format copies its depth plane, as the D3D12 copy footprint names it. D24S8 packs 24
// bits a float image cannot take as they are: none, and the motion vectors are read at the pixel as before.
Format DepthFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return { VK_FORMAT_R32_SFLOAT, 4 };
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return { VK_FORMAT_R16_UNORM, 2 };
    default:
        return {};
    }
}

// Scene-referred float colour: the network sees it through the runtime's sRGB encode.
bool IsLinear(VkFormat f)
{
    return f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
           f == VK_FORMAT_R32G32B32A32_SFLOAT || f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
}

// A versioned struct (FrameInfo, the Mochizuki* ones) holds a field when its struct_size covers all of it: older
// callers send shorter structs.
#define MZ_HOLDS(s, Type, field) ((s)->struct_size >= offsetof(Type, field) + sizeof(Type::field))

// A control as the core's validate() accepts it: NaN or infinity is the default, anything else is clamped. The core
// throws on a value out of range, and Guard then fails the session, so nothing from a host reaches it unsanitised.
float San(float v, float lo, float hi, float def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; }

// FrameInfo::model_scale: 0 (a host that does not set it) is 1. Quantised to 0.05, since every change rebuilds the
// network; 1 stays exactly 1, which keeps the model extent the frame's own.
float ModelScale(float v) { return v == 0 ? 1.f : std::round(San(v, 0.25f, 1.f, 1.f) * 20.f) / 20.f; }

// The most passes a frame can ask for (FrameInfo::passes; MochizukiNrControls::pass holds passes 2 and 3, later ones
// inherit pass 1 with local tone 0), and so the most a network is built for: every pass it is built for costs a
// model-extent RGBA32F history image. The host offers 1..3, and 1..30 with MochizukiUnlockPasses.
constexpr uint32_t kMaxPasses = 30;

// What SetControls last gave, sanitised. Until it is called, the core's defaults: today's output.
struct SessionControls
{
    nr::Controls model;      // detail_strength, colour_strength and passes come with each frame
    float history = 1.f;     // Runtime::set_history_strength
    float white = 1.f;       // Runtime::set_white_point
    uint32_t linearMode = 0; // MochizukiNrControls::linear_input: 0 auto, 1 on, 2 off
    uint32_t maxPasses = 0;  // 0: the frame's passes
    uint32_t drsMode = 0;    // MochizukiNrControls::drs_mode: 0 exact, 1 auto, 2 always (Session::DrsExtent)
    bool renodx = true;      // MochizukiNrControls::renodx: the RenoDX stage, with `reno` in place of `model`
    // The stage's own defaults but SDR black restore and HDR dark pedestal Auto (3), as before its 0.0.8.3 Off.
    nr::renodx::Settings reno = []
    {
        nr::renodx::Settings r;
        r.sdr_black_restore = r.display_pedestal = 3;
        return r;
    }();
    bool styleGrade = true;  // Runtime::set_style_grade
    bool historyF16 = true;  // RuntimeConfig::history_f16 (MochizukiNrControls::history_precision)
    uint32_t optimizedLayers = 3; // RuntimeConfig::layers (MochizukiNrControls::optimized_layers)
};

void DefaultControls(MochizukiNrControls& c)
{
    const nr::Controls model;
    const nr::PassControls pass;
    c = {};
    c.struct_size = sizeof c;
    c.intensity = model.intensity;
    c.style = uint32_t(model.style);
    c.local_tone = model.local_tone;
    c.local_structure = model.local_structure;
    c.skin_structure = model.skin_structure;
    c.automatic_mask = model.automatic_mask;
    c.max_ratio = model.max_ratio;
    c.history_strength = nr::TemporalConfig {}.history_strength;
    c.white_point = nr::RuntimeConfig {}.white_point;
    c.apply_model = model.apply_model;
    c.drs_mode = 0; // exact, as for a zeroed struct: a host must ask for dynamic resolution (its INI default is auto)
    const nr::Preprocess prep;
    c.preprocess = prep.enabled;
    c.preprocess_exposure = uint32_t(prep.exposure);
    c.preprocess_bias_ev = prep.bias_ev;
    c.preprocess_curve = uint32_t(prep.curve);
    c.preprocess_contrast = prep.contrast;
    c.preprocess_saturation = prep.saturation;
    c.anti_halo_margin = model.anti_halo_margin;
    c.enlarge_mode = uint32_t(model.transfer);
    c.classic_scaler = uint32_t(model.classic_scaler);
    c.renodx = 1;
    c.style_grade = 1;
    c.history_precision = 16;
    c.preprocess_inverse = 1;
    c.optimized_layers = 3;
#define RDX_F(member, field, def, low, high) c.member = def;
#define RDX_U RDX_F
#define RDX_B RDX_F
#include "MochizukiRenoOptions.inc"
#undef RDX_F
#undef RDX_U
#undef RDX_B
    for (MochizukiNrPassControls& p : c.pass)
    {
        p.style = uint32_t(pass.style);
        p.intensity = pass.intensity;
        p.local_tone = pass.local_tone;
        p.local_structure = pass.local_structure;
        p.skin_structure = pass.skin_structure;
        p.automatic_mask = pass.automatic_mask;
    }
}

// Every field sanitised to what the core accepts. Allocates only for pass overrides.
SessionControls Sanitise(const MochizukiNrControls& c)
{
    SessionControls out;
    nr::Controls& m = out.model;
    m.intensity = San(c.intensity, 0.f, 2.f, 1.f);
    m.style = int(std::min(c.style, 2u));
    m.local_tone = San(c.local_tone, 0.f, 2.f, 1.f);
    m.local_structure = San(c.local_structure, 0.f, 2.f, 1.f);
    m.skin_structure = San(c.skin_structure, -1.f, 2.f, -1.f);
    m.automatic_mask = c.automatic_mask != 0;
    m.max_ratio = San(c.max_ratio, 1.f, 8.f, 2.f);
    m.apply_model = c.apply_model != 0;
    out.history = San(c.history_strength, 0.f, 1.f, 1.f);
    out.white = San(c.white_point, 0.01f, 100.f, 1.f);
    out.linearMode = c.linear_input <= 2 ? c.linear_input : 0;
    out.maxPasses = std::min(c.max_passes, kMaxPasses);
    out.drsMode = c.drs_mode <= 2 ? c.drs_mode : 0; // a mode this runtime does not know runs exact
    nr::Preprocess& prep = m.preprocess;
    prep.enabled = c.preprocess != 0;
    prep.exposure = int(c.preprocess_exposure <= 2 ? c.preprocess_exposure : 1);
    prep.bias_ev = San(c.preprocess_bias_ev, -8.f, 8.f, 0.f);
    prep.curve = int(c.preprocess_curve <= 6 ? c.preprocess_curve : 3);
    prep.contrast = San(c.preprocess_contrast, .5f, 2.f, 1.f);
    prep.saturation = San(c.preprocess_saturation, .05f, 2.f, 1.f);
    m.anti_halo_margin =
        MZ_HOLDS(&c, MochizukiNrControls, anti_halo_margin) ? San(c.anti_halo_margin, 0.f, .5f, .05f) : .05f;
    // A shorter struct leaves these at DefaultControls' values (SetControls copies over them).
    m.transfer = c.enlarge_mode <= 2 ? int(c.enlarge_mode) : 0;
    m.classic_scaler = c.classic_scaler <= 3 ? int(c.classic_scaler) : 1;
    m.noise_every_frame = c.noise_every_frame != 0;
    out.renodx = c.renodx != 0;
    out.styleGrade = c.style_grade != 0;
    out.historyF16 = c.history_precision != 32;
    out.optimizedLayers = std::min(c.optimized_layers, 3u);
    prep.inverse = c.preprocess_inverse != 0;
#define RDX_F(member, field, def, low, high) out.reno.field = San(c.member, low, high, def);
#define RDX_U(member, field, def, low, high) out.reno.field = std::clamp(c.member, uint32_t(low), uint32_t(high));
#define RDX_B(member, field, def, low, high) out.reno.field = c.member != 0;
#include "MochizukiRenoOptions.inc"
#undef RDX_F
#undef RDX_U
#undef RDX_B
    // The stage's 0.0.8.3 numbering: 0 Off, 1 and 2 as before, 3 Auto; the keys keep 0 Auto (and offer Off as 3).
    for (uint32_t* v : { &out.reno.sdr_black_restore, &out.reno.display_pedestal })
        *v = *v == 0 ? 3u : *v == 3 ? 0u : *v;
    // validate() does not look at these, and the core puts them straight into push constants.
    for (size_t k = std::size(c.pass); k-- > 0;)
    {
        const MochizukiNrPassControls& p = c.pass[k];
        if (!p.used)
            continue;
        if (m.per_pass.size() <= k)
            m.per_pass.resize(k + 1);
        nr::PassControls& o = m.per_pass[k];
        o.used = true;
        o.style = int(std::min(p.style, 2u));
        o.intensity = San(p.intensity, 0.f, 2.f, 1.f);
        o.local_tone = San(p.local_tone, 0.f, 2.f, 0.f);
        o.local_structure = San(p.local_structure, 0.f, 2.f, 1.f);
        o.skin_structure = San(p.skin_structure, -1.f, 2.f, -1.f);
        o.automatic_mask = p.automatic_mask != 0;
    }
    return out;
}

struct Vulkan
{
    VkInstance instance {};
    VkPhysicalDevice physical {};
    VkDevice device {};
    VkQueue queue {};      // the frames'
    VkQueue buildQueue {}; // the network builds': a second queue of the family at a lower priority, else `queue`
    uint32_t family = 0;
    bool bufferDeviceAddress = false; // enabled on `device`; the network reads its arena through it where faster
    // The network (RuntimeConfig::arch): rdna4 the FP8 one; with no FP8 on the device rdna3, on FP16 matrices, or
    // rdna2, with no matrices (its fragments emulated in ALU).
    nr::Arch arch = nr::Arch::rdna4;
    bool rdna3Aco = false; // the RDNA3 network as ACO's gfx11 machine code (Rdna3Aco)
    bool rdna2Aco = false; // the RDNA2 network as ACO's gfx10.3 machine code (Rdna2Aco)
    uint32_t deviceId = 0, driverVersion = 0; // as AMD's driver reports them: the key of the FP16 networks' verdicts
    bool Fp16() const { return nr::fp16_network(arch); }
    VkPhysicalDeviceMemoryProperties memory {};
    PFN_vkGetMemoryWin32HandlePropertiesKHR memoryHandleProperties {};
    PFN_vkImportSemaphoreWin32HandleKHR importSemaphore {};
    std::string name;

    // Throws "[unsupported] ..." for what cannot change while the process lives (no Vulkan 1.3 driver, no device
    // for the adapter, a missing extension or feature), anything else for a failure that may pass. What is absent is
    // decided only from lists that were read in full (VkEnumerate); a failed enumeration throws a plain error.
    void Create(LUID luid, const TestHooks& hooks)
    {
        VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "OptiScaler NR";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ii { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ii.pApplicationInfo = &app;
        VkInstance createdInstance {};
        // ReShade's Vulkan layer, installed system-wide, would load into the game with this instance and hook its
        // D3D12 and DXGI from then on (BeamNG: NR stayed off). The layer's manifest names this variable as its off
        // switch; the loader reads it at vkCreateInstance.
        constexpr wchar_t kNoLayer[] = L"DISABLE_VK_LAYER_reshade_1";
        wchar_t old[8] {};
        const DWORD had = GetEnvironmentVariableW(kNoLayer, old, 8);
        SetEnvironmentVariableW(kNoLayer, L"1");
        const VkResult created = vkCreateInstance(&ii, nullptr, &createdInstance);
        SetEnvironmentVariableW(kNoLayer, had != 0 && had < 8 ? old : nullptr);
        if (created == VK_ERROR_INCOMPATIBLE_DRIVER)
            throw std::runtime_error("[unsupported] no Vulkan 1.3 driver (vkCreateInstance: "
                                     "VK_ERROR_INCOMPATIBLE_DRIVER)");
        VkCheck(created, "vkCreateInstance");
        instance = createdInstance;
        if (hooks.failStart)
            throw std::runtime_error("MZ_TEST_FAIL_START: Start failed after vkCreateInstance (test hook)");

        const std::vector<VkPhysicalDevice> all =
            VkEnumerate<VkPhysicalDevice>("vkEnumeratePhysicalDevices", [this](uint32_t* n, VkPhysicalDevice* p)
                                          { return vkEnumeratePhysicalDevices(instance, n, p); });
        // Every Vulkan device on the game's adapter: a stale second ICD can list the same LUID again (Detroit on an
        // RX 9070: the first one lacked VK_EXT_shader_float8), so each is checked and the first that passes is taken,
        // the newest driver when several do.
        struct Candidate
        {
            VkPhysicalDevice device {};
            std::string name;
            uint32_t driverVersion = 0;
            nr::Arch arch = nr::Arch::rdna4;
        };
        std::vector<Candidate> matches;
        for (VkPhysicalDevice p : all)
        {
            VkPhysicalDeviceDriverProperties driver { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
            VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &driver };
            VkPhysicalDeviceProperties2 props { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
            vkGetPhysicalDeviceProperties2(p, &props);
            if (!id.deviceLUIDValid || std::memcmp(id.deviceLUID, &luid, sizeof luid))
                continue;
            const uint32_t api = props.properties.apiVersion;
            nr::logf("[mochizuki] Vulkan device %zu on the adapter: %s, driver %s (%s), driverVersion 0x%08X, Vulkan "
                     "%u.%u.%u",
                     matches.size(), props.properties.deviceName, driver.driverName, driver.driverInfo,
                     props.properties.driverVersion, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
                     VK_API_VERSION_PATCH(api));
            matches.push_back({ p, props.properties.deviceName, props.properties.driverVersion });
        }
        if (matches.empty())
        {
            char text[112];
            std::snprintf(text, sizeof text,
                          "[unsupported] no Vulkan device matches the game's D3D12 adapter (%zu Vulkan devices)",
                          all.size());
            throw std::runtime_error(text);
        }
        if (hooks.unsupported)
            throw std::runtime_error("[unsupported] MZ_TEST_UNSUPPORTED: the capability check failed (test hook)");

        // What nrvk::Context::create enables, plus the timeline semaphores the game's fences are imported as.
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR
        };
        VkPhysicalDeviceShaderFloat8FeaturesEXT fp8 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT };
        VkPhysicalDeviceVulkan11Features f11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features f12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features f13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wml {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR
        };
        coop.cooperativeMatrix = VK_TRUE;
        fp8.shaderFloat8 = fp8.shaderFloat8CooperativeMatrix = VK_TRUE;
        f11.storageBuffer16BitAccess = VK_TRUE;
        f12.storageBuffer8BitAccess = f12.shaderFloat16 = f12.shaderInt8 = f12.vulkanMemoryModel = VK_TRUE;
        f12.timelineSemaphore = VK_TRUE;
        f13.subgroupSizeControl = f13.synchronization2 = VK_TRUE;
        wml.workgroupMemoryExplicitLayout = wml.workgroupMemoryExplicitLayout8BitAccess =
            wml.workgroupMemoryExplicitLayout16BitAccess = VK_TRUE;
        coop.pNext = &fp8;
        fp8.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        f13.pNext = &wml;
        const char* extensions[] = { VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,
                                     VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME,
                                     VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                     VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
        // A device without FP8 (RX 7000, gfx11) runs the RDNA3 network, whose e4m3 values are FP16 operands of the
        // FP16 x FP16 -> FP32 16x16x16 subgroup matrices; VK_EXT_shader_float8 and its features are then neither
        // required nor enabled. An RX 9000 (device 0x75xx) still needs FP8: the RDNA3 network computes a wrong
        // picture there, as its kernels assume gfx11's fragment layout. MZ_FORCE_RDNA3=1 takes it on any card
        // (a test of the pipelines, builds and frames on RDNA4).
        // A device with neither FP8 nor those matrices (RX 6000, gfx103x) runs the RDNA2 network: the RDNA3 one with
        // its fragments emulated in ALU, so it needs no matrix extension at all and computes the right picture on
        // any GPU. MZ_FORCE_RDNA2=1 takes it on any card (on an RX 9000 the picture is checked against FP8's).
        const auto env1 = [](const char* name)
        {
            char v[4] {};
            return GetEnvironmentVariableA(name, v, sizeof v) == 1 && v[0] == '1';
        };
        const bool forceRdna2 = env1("MZ_FORCE_RDNA2");
        const bool forceRdna3 = !forceRdna2 && env1("MZ_FORCE_RDNA3");
        const auto fp16Matrices = [this](VkPhysicalDevice p)
        {
            auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
                vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
            uint32_t n = 0;
            if (!query || query(p, &n, nullptr) != VK_SUCCESS)
                return false;
            std::vector<VkCooperativeMatrixPropertiesKHR> m(n, { VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR });
            if (query(p, &n, m.data()) != VK_SUCCESS)
                return false;
            return std::any_of(m.begin(), m.begin() + n,
                               [](const VkCooperativeMatrixPropertiesKHR& c)
                               {
                                   return c.MSize == 16 && c.NSize == 16 && c.KSize == 16 &&
                                          c.scope == VK_SCOPE_SUBGROUP_KHR && c.AType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
                                          c.BType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
                                          c.CType == VK_COMPONENT_TYPE_FLOAT32_KHR &&
                                          c.ResultType == VK_COMPONENT_TYPE_FLOAT32_KHR && !c.saturatingAccumulation;
                               });
        };
        nr::Arch candidate = nr::Arch::rdna4; // check's answer for the device it checked
        std::string fp16Why;

        // The capability precheck: what vkCreateDevice would refuse with the chain above, found and named before it
        // is called, as the reason the device fails (empty when it passes). Vulkan 1.3 and the extensions first,
        // since only then may their structures be chained.
        std::vector<VkQueueFamilyProperties> families;
        std::vector<VkExtensionProperties> offered;
        bool deviceAddress = false;
        const auto check = [&](VkPhysicalDevice p) -> std::string
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(p, &count, nullptr);
            families.assign(count, {});
            vkGetPhysicalDeviceQueueFamilyProperties(p, &count, families.data());
            family = count;
            for (uint32_t i = 0; i < count; ++i)
                if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                    (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                {
                    family = i;
                    break;
                }
            if (family == count)
                return "[unsupported] no graphics and compute queue family";
            VkPhysicalDeviceProperties props {};
            vkGetPhysicalDeviceProperties(p, &props);
            if (props.apiVersion < VK_API_VERSION_1_3)
            {
                char text[96];
                std::snprintf(text, sizeof text, "[unsupported] the device offers Vulkan %u.%u; 1.3 is required",
                              VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
                return text;
            }
            offered = VkEnumerate<VkExtensionProperties>(
                "vkEnumerateDeviceExtensionProperties", [this, p](uint32_t* n, VkExtensionProperties* e)
                { return vkEnumerateDeviceExtensionProperties(p, nullptr, n, e); });
            const auto offers = [](const std::vector<VkExtensionProperties>& list, const char* e)
            {
                return std::any_of(list.begin(), list.end(),
                                   [e](const VkExtensionProperties& x) { return !std::strcmp(x.extensionName, e); });
            };
            const bool hasFp8Extension = offers(offered, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
            // The card as AMD's driver reports it: OptiScaler's GPU spoofing can rewrite what the loader answers.
            VkPhysicalDeviceDriverProperties driver { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
            VkPhysicalDeviceProperties2 reported { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver };
            vkGetPhysicalDeviceProperties2(p, &reported);
            if (reported.properties.vendorID != 0x1002 || driver.driverID != VK_DRIVER_ID_AMD_PROPRIETARY)
                nr::driver::amd_reported(p, &reported.properties, &driver);
            const bool amd = reported.properties.vendorID == 0x1002;
            deviceId = reported.properties.deviceID;
            driverVersion = reported.properties.driverVersion;
            const bool rdna4 = amd && (reported.properties.deviceID & 0xFF00) == 0x7500;
            // RDNA2 (gfx103x) by device id: Navi 21/22/23 0x73A0-0x73FF, Navi 24 0x7420-0x743F, and the APUs Van Gogh
            // 0x163F, Rembrandt 0x1681, Raphael 0x164E, Mendocino 0x1506. Its driver branch (9.2.10.342) reports the
            // FP16 16x16x16 subgroup matrices and emulates them itself (phase 0: 256 VGPRs and scratch on the RDNA3
            // kernels), so a missing configuration cannot be the test, and it must never get the RDNA3 network.
            const uint32_t id = reported.properties.deviceID;
            const bool rdna2 = amd && ((id >= 0x73A0 && id <= 0x73FF) || (id >= 0x7420 && id <= 0x743F) ||
                                       id == 0x163F || id == 0x1681 || id == 0x164E || id == 0x1506);
            bool fp16 = forceRdna3 || forceRdna2 || (!hasFp8Extension && amd && !rdna4);
            const bool hasCoopExtension = offers(offered, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
            candidate = nr::Arch::rdna4;
            for (const char* e : extensions)
                if (!(fp16 && !std::strcmp(e, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME)) &&
                    !(fp16 && (!forceRdna3 || rdna2) && !std::strcmp(e, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)) &&
                    !offers(offered, e))
                    return std::string("[unsupported] the driver lacks ") + e;
            VkPhysicalDeviceCooperativeMatrixFeaturesKHR hasCoop {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR
            };
            VkPhysicalDeviceShaderFloat8FeaturesEXT hasFp8 {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT
            };
            VkPhysicalDeviceVulkan11Features has11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
            VkPhysicalDeviceVulkan12Features has12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
            VkPhysicalDeviceVulkan13Features has13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
            VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR hasWml {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR
            };
            hasCoop.pNext = hasFp8Extension ? static_cast<void*>(&hasFp8) : &has11;
            hasFp8.pNext = &has11;
            has11.pNext = &has12;
            has12.pNext = &has13;
            has13.pNext = &hasWml;
            // Without the matrix extension its structure is left out of the chain (an RX 6000).
            VkPhysicalDeviceFeatures2 has { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                            hasCoopExtension ? static_cast<void*>(&hasCoop) : hasCoop.pNext };
            vkGetPhysicalDeviceFeatures2(p, &has);
            if (!fp16 && amd && !rdna4 && (!hasFp8.shaderFloat8 || !hasFp8.shaderFloat8CooperativeMatrix))
                fp16 = true;
            if (fp16)
            {
                fp16Why = forceRdna2          ? "MZ_FORCE_RDNA2=1"
                          : rdna2             ? "an RDNA2 GPU (gfx103x), which has no FP8 and no matrix instructions"
                          : forceRdna3        ? "MZ_FORCE_RDNA3=1"
                          : !hasFp8Extension ? "the driver has no VK_EXT_shader_float8"
                                             : "the driver offers no FP8 matrices";
                if (!forceRdna2 && !rdna2 && hasCoopExtension && hasCoop.cooperativeMatrix && fp16Matrices(p))
                    candidate = nr::Arch::rdna3;
                else if (forceRdna3 && !rdna2)
                    return std::string("[unsupported] ") + fp16Why +
                           " and reports no FP16 x FP16 -> FP32 16x16x16 subgroup matrices";
                else
                {
                    candidate = nr::Arch::rdna2;
                    if (!forceRdna2 && !rdna2)
                        fp16Why += " nor FP16 x FP16 -> FP32 16x16x16 subgroup matrices";
                    // The emulated fragments trade rows between lanes by subgroup shuffles, and every RDNA2 kernel
                    // rounds its f32 -> f16 conversions to nearest even (RoundingModeRTE, rdna2/include/coopmm.glsl).
                    VkPhysicalDeviceVulkan12Properties p12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
                    VkPhysicalDeviceVulkan11Properties p11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
                                                             &p12 };
                    VkPhysicalDeviceProperties2 p2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11 };
                    vkGetPhysicalDeviceProperties2(p, &p2);
                    if (!(p11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
                        !(p11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT))
                        return std::string("[unsupported] ") + fp16Why +
                               ", and the RDNA2 network needs subgroup shuffles in compute shaders";
                    if (!p12.shaderRoundingModeRTEFloat16)
                        return std::string("[unsupported] ") + fp16Why +
                               ", and the RDNA2 network needs shaderRoundingModeRTEFloat16";
                }
            }
            const struct
            {
                VkBool32 wanted, offered;
                const char* name;
            } features[] = {
                { candidate == nr::Arch::rdna2 ? VK_FALSE : coop.cooperativeMatrix, hasCoop.cooperativeMatrix,
                  "cooperativeMatrix" },
                { fp16 ? VK_FALSE : fp8.shaderFloat8, hasFp8.shaderFloat8, "shaderFloat8" },
                { fp16 ? VK_FALSE : fp8.shaderFloat8CooperativeMatrix, hasFp8.shaderFloat8CooperativeMatrix,
                  "shaderFloat8CooperativeMatrix" },
                { f11.storageBuffer16BitAccess, has11.storageBuffer16BitAccess, "storageBuffer16BitAccess" },
                { f12.storageBuffer8BitAccess, has12.storageBuffer8BitAccess, "storageBuffer8BitAccess" },
                { f12.shaderFloat16, has12.shaderFloat16, "shaderFloat16" },
                { f12.shaderInt8, has12.shaderInt8, "shaderInt8" },
                { f12.vulkanMemoryModel, has12.vulkanMemoryModel, "vulkanMemoryModel" },
                { f12.timelineSemaphore, has12.timelineSemaphore, "timelineSemaphore" },
                { f13.subgroupSizeControl, has13.subgroupSizeControl, "subgroupSizeControl" },
                { f13.synchronization2, has13.synchronization2, "synchronization2" },
                { wml.workgroupMemoryExplicitLayout, hasWml.workgroupMemoryExplicitLayout,
                  "workgroupMemoryExplicitLayout" },
                { wml.workgroupMemoryExplicitLayout8BitAccess, hasWml.workgroupMemoryExplicitLayout8BitAccess,
                  "workgroupMemoryExplicitLayout8BitAccess" },
                { wml.workgroupMemoryExplicitLayout16BitAccess, hasWml.workgroupMemoryExplicitLayout16BitAccess,
                  "workgroupMemoryExplicitLayout16BitAccess" },
            };
            for (const auto& f : features)
                if (f.wanted && !f.offered)
                    return std::string("[unsupported] the driver lacks the Vulkan feature ") + f.name;
            deviceAddress = has12.bufferDeviceAddress == VK_TRUE;
            return {};
        };
        std::string refused;
        const Candidate* chosen = nullptr;
        for (Candidate& c : matches)
        {
            const std::string reason = check(c.device);
            c.arch = candidate;
            if (!reason.empty())
            {
                if (matches.size() > 1)
                    nr::logf("[mochizuki] Vulkan device %zu fails the check: %s", size_t(&c - matches.data()),
                             reason.c_str());
                if (refused.empty())
                    refused = reason;
            }
            // FP8 before the RDNA3 network before the RDNA2 one, then the newest driver.
            else if (!chosen || c.arch < chosen->arch ||
                     (chosen->arch == c.arch && c.driverVersion > chosen->driverVersion))
                chosen = &c;
        }
        if (!chosen)
        {
            if (matches.size() > 1)
                refused += " (" + std::to_string(matches.size()) +
                           " Vulkan devices share this adapter, none passes: a duplicate or stale driver may be "
                           "installed)";
            throw std::runtime_error(refused);
        }
        physical = chosen->device;
        name = chosen->name;
        if (matches.size() > 1)
        {
            // The check runs again on the one taken, so the queue families and extensions below are its own.
            nr::logf("[mochizuki] %zu Vulkan devices share this adapter (a duplicate driver); using device %zu",
                     matches.size(), size_t(chosen - matches.data()));
            check(physical);
        }
        arch = chosen->arch;
        // Not offered or not asked for: off, out of the device's chain and extension list (FP8 on both FP16
        // networks, the matrices on RDNA2's).
        void* chain = &coop;
        if (arch == nr::Arch::rdna3)
        {
            coop.pNext = &f11;
            nr::logf("[mochizuki] the RDNA3 network, on FP16 matrices: %s", fp16Why.c_str());
            Rdna3Aco();
        }
        else if (arch == nr::Arch::rdna2)
        {
            chain = &f11;
            // The INT8 network (shaders-rdna2-int8, v_dot4_i32_i8): asked for, installed and offered.
            nr::rdna2_int8 = false;
            if (env1("MZ_RDNA2_INT8"))
            {
                VkPhysicalDeviceShaderIntegerDotProductProperties dp {
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES
                };
                VkPhysicalDeviceProperties2 p2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp };
                vkGetPhysicalDeviceProperties2(physical, &p2);
                VkPhysicalDeviceVulkan13Features has13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
                VkPhysicalDeviceFeatures2 has { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &has13 };
                vkGetPhysicalDeviceFeatures2(physical, &has);
                const bool folder = std::filesystem::is_directory(DataDirectory() + L"\\dlssnr-amd\\shaders-rdna2-int8");
                if (!folder)
                    nr::logf("[mochizuki] MZ_RDNA2_INT8: dlssnr-amd\\shaders-rdna2-int8 is not installed; the FP16 network");
                else if (!has13.shaderIntegerDotProduct)
                    nr::logf("[mochizuki] MZ_RDNA2_INT8: the driver offers no shaderIntegerDotProduct; the FP16 network");
                else
                {
                    // A pipeline check this driver failed on the INT8 shaders (rdna2-int8-check.txt): the FP16 ones.
                    nr::rdna2_int8 = true;
                    std::string key = mzr3::Key(deviceId, driverVersion), why;
                    Fp16PipelineFiles(arch, key);
                    if (!Fp16CheckSkips(arch, false) && mzr3::Cached(Fp16CheckFile(arch), key, "pipelines", why) &&
                        !why.empty())
                    {
                        nr::rdna2_int8 = false;
                        nr::logf("[mochizuki] MZ_RDNA2_INT8: the INT8 network failed its pipeline check on this driver "
                                 "(%s); the FP16 network", why.c_str());
                    }
                    else
                    {
                        f13.shaderIntegerDotProduct = VK_TRUE;
                        nr::logf("[mochizuki] MZ_RDNA2_INT8: the INT8 network (v_dot4), 4x8-bit packed signed dot "
                                 "product %s",
                                 dp.integerDotProduct4x8BitPackedSignedAccelerated ? "accelerated" : "NOT accelerated");
                    }
                }
            }
            nr::logf("[mochizuki] the RDNA2 network, its matrices emulated in ALU (%s products, FP32 sums): %s",
                     nr::rdna2_int8 ? "INT8 and FP16" : "FP16", fp16Why.c_str());
            if (!nr::rdna2_int8)
                Rdna2Aco();
        }
        else
        {
            nr::logf("[mochizuki] the FP8 network (VK_EXT_shader_float8)");
            const std::string asked = nr::binary::directory();
            if (!asked.empty() && std::filesystem::path(asked).filename() != "aco")
                nr::binary::disable("the FP8 network has no ACO records (dlssnr-amd\\aco\\)");
        }
        // Optional: without it the network takes its descriptor reads.
        bufferDeviceAddress = deviceAddress;
        f12.bufferDeviceAddress = bufferDeviceAddress ? VK_TRUE : VK_FALSE;
        // MZ_PROBE_PIPELINE_BINARY=1 only (mz_interpose.h): two more extensions, and their features after wml.
        std::vector<const char*> enabled(std::begin(extensions), std::end(extensions));
        if (Fp16())
            enabled.erase(std::find(enabled.begin(), enabled.end(), VK_EXT_SHADER_FLOAT8_EXTENSION_NAME));
        if (arch == nr::Arch::rdna2)
            enabled.erase(std::find(enabled.begin(), enabled.end(), VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME));
        mzi::BinaryProbe probe;
        // The FP16 networks import the pipeline check's machine code (mz_interpose.h, AdoptBinaries).
        wml.pNext = probe.Prepare(physical, offered, enabled, wml.pNext, Fp16() && !MzEnvOff("MZ_CHECK_CACHE"));

        // A second queue, when the family has one, takes the network builds (the core submits only while it is
        // constructed), so a build never holds the frames' queue; the frames keep the higher priority.
        const float priorities[] = { 1.0f, 0.5f };
        VkDeviceQueueCreateInfo qi { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qi.queueFamilyIndex = family;
        qi.queueCount = std::min<uint32_t>(families[family].queueCount, uint32_t(std::size(priorities)));
        // MZ_ONE_QUEUE=1 (diagnostic, the RX 7000 kit 4): no second queue, the builds share the frames' one.
        if (const char* one = std::getenv("MZ_ONE_QUEUE"); one && std::atoi(one)) qi.queueCount = 1;
        qi.pQueuePriorities = priorities;
        VkDeviceCreateInfo di { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, chain };
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = uint32_t(enabled.size());
        di.ppEnabledExtensionNames = enabled.data();
        // ACO's network (ConfigureAco): pipeline binaries and maintenance5 on this device, when the driver has them.
        nr::binary::Features binaryFeatures;
        binaryFeatures.prepare(physical, di, enabled);
        VkDevice createdDevice {};
        VkCheck(vkCreateDevice(physical, &di, nullptr, &createdDevice), "vkCreateDevice");
        device = createdDevice;
        if (binaryFeatures.active)
            nr::binary::mark(device);
        probe.Report(device, physical);
        vkGetDeviceQueue(device, family, 0, &queue);
        buildQueue = queue;
        if (qi.queueCount > 1)
            vkGetDeviceQueue(device, family, 1, &buildQueue);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        memoryHandleProperties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
            vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandlePropertiesKHR"));
        importSemaphore = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(device, "vkImportSemaphoreWin32HandleKHR"));
        if (!memoryHandleProperties || !importSemaphore)
            throw std::runtime_error("the Win32 external memory entry points are missing");
        if (Fp16())
            Fp16Checks();
    }

    // ACO for the RDNA3 network, when it was asked for (ConfigureAco set dlssnr-amd\aco up): Mesa ACO gfx11 machine code
    // in dlssnr-amd\aco-gfx11\. As shipped (tools\build-mochizuki-runtime.cmd) it holds records\ only for the kernels
    // ACO runs faster on RX 7000 and shaders\ with the ACO SPIR-V of those that differ from shaders-rdna3's
    // (vitattn's matrix body among them; nr::binary::gfx11_slim); a kit's full bundle is laid out as aco\ is. It is taken when
    // its records are there and the PAL adapter takes this GPU's code objects (nr::pal::takes_machine); a pipeline
    // whose record is missing or does not import is compiled by the driver from shaders-rdna3 (nr_pipeline_binary.hpp).
    void Rdna3Aco()
    {
        const std::string asked = nr::binary::directory();
        if (asked.empty())
        {
            nr::binary::disable("the RDNA3 network has no ACO records");
            return;
        }
        const std::string bundle = RootUtf8(DataDirectory()) + "\\dlssnr-amd\\aco-gfx11";
        // gfx1100 (Navi 31), gfx1101 (Navi 32), gfx1102 (Navi 33): their code objects' ELF e_flags.
        const uint32_t mach = (deviceId & 0xFFF0) == 0x7470 ? 0x46 : (deviceId & 0xFFF0) == 0x7480 ? 0x47 : 0x41;
        const char* why = GetFileAttributesA((bundle + "\\records").c_str()) == INVALID_FILE_ATTRIBUTES ||
                                  GetFileAttributesA((bundle + "\\shaders").c_str()) == INVALID_FILE_ATTRIBUTES
                              ? "no gfx11 ACO records (dlssnr-amd\\aco-gfx11\\records\\)"
                          : !nr::pal::takes_machine(mach)
                              ? "the PAL adapter does not take this GPU's code objects"
                              : nullptr;
        if (why)
        {
            nr::logf("[mochizuki] ACO off for the RDNA3 network: %s; the driver compiles it", why);
            nr::binary::disable(std::string("the RDNA3 network: ") + why);
            return;
        }
        nr::binary::configure(bundle, { bundle + "\\shell", RootUtf8(DataDirectory()) + "\\dlssnr-amd\\shaders-rdna3",
                                        RootUtf8(DataDirectory()) + "\\dlssnr-amd\\shaders-rdna3\\temporal" },
                              nr::binary::cache_dir(), {});
        rdna3Aco = true;
        nr::logf("[mochizuki] ACO: the RDNA3 network runs ACO's gfx11 machine code from %s", bundle.c_str());
    }

    // ACO for the RDNA2 network, when it was asked for: the network as Mesa ACO gfx10.3 machine code in
    // dlssnr-amd\aco-gfx103x\ (shaders\ = shaders-rdna2's SPIR-V as it is, records\ named by its hash;
    // tools\aco-records\produce-rdna2.ps1 makes both). One gfx1030 record set serves every RDNA2 GPU: the PAL container
    // is built with the GPU's own machine (nr_pal_binary.hpp synthesize). A pipeline whose record is missing or does
    // not import is compiled by the driver from shaders-rdna2 (nr_pipeline_binary.hpp).
    void Rdna2Aco()
    {
        if (nr::binary::directory().empty())
        {
            nr::binary::disable("the RDNA2 network has no ACO records");
            return;
        }
        const std::string data = RootUtf8(DataDirectory()) + "\\dlssnr-amd";
        const std::string bundle = data + "\\aco-gfx103x";
        if (GetFileAttributesA((bundle + "\\records").c_str()) == INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesA((bundle + "\\shaders").c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            const char* why = "no gfx10.3 ACO records (dlssnr-amd\\aco-gfx103x\\records\\)";
            nr::logf("[mochizuki] ACO off for the RDNA2 network: %s; the driver compiles it", why);
            nr::binary::disable(std::string("the RDNA2 network: ") + why);
            return;
        }
        nr::binary::configure(bundle, { data + "\\shaders-rdna2", data + "\\shaders-rdna2\\temporal" },
                              nr::binary::cache_dir(), {});
        rdna2Aco = true;
        nr::logf("[mochizuki] ACO: the RDNA2 network runs ACO's gfx10.3 machine code from %s", bundle.c_str());
    }

    // An FP16 network's checks: the pipeline check's verdict when it failed before on this driver and, for the RDNA3
    // network, its fragment layout on this device (mzr3::LayoutCheck), once a driver. Either keeps the network off
    // ("[unsupported] ...", the adapter's reason for the process; the menu shows it). The RDNA2 network's fragments
    // are arrays of its own, so it has no layout to check.
    void Fp16Checks() const
    {
        const std::wstring file = Fp16CheckFile(arch);
        const char* tag = nr::arch_name(arch);
        const std::string key = mzr3::Key(deviceId, driverVersion);
        const std::string kept = std::string(" (an earlier check's, kept in dlssnr-amd\\") +
                                 (arch == nr::Arch::rdna2 ? "rdna2" : "rdna3") + "-check.txt)";
        std::string pipelines = key, why;
        Fp16PipelineFiles(arch, pipelines);
        if (!Fp16CheckSkips(arch, false) && mzr3::Cached(file, pipelines, "pipelines", why) && !why.empty())
            why += kept;
        else if (arch == nr::Arch::rdna2)
            return;
        else if (Fp16CheckSkips(arch, true))
        {
            nr::logf("[mochizuki] the RDNA3 layout probes are skipped (MZ_RDNA3_CHECK)");
            return;
        }
        else if (mzr3::Cached(file, key, "layout", why))
        {
            if (!why.empty())
                why += kept;
        }
        else
        {
            const auto t0 = std::chrono::steady_clock::now();
            why = mzr3::LayoutCheck(physical, device, queue, family);
            mzr3::Keep(file, key, "layout", why);
            nr::logf("[mochizuki] RDNA3 layout probes %s in %.0f ms", why.empty() ? "passed" : "failed",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        if (why.empty())
            return;
        nr::logf("[mochizuki] %s network off: %s", tag, why.c_str());
        // The first failure for the menu, whose status line holds 256 characters; the log has them all.
        const size_t more = size_t(std::count(why.begin(), why.end(), ';'));
        if (more)
            why = why.substr(0, why.find(';')) + " (and " + std::to_string(more) + " more, mochizuki_nr.log)";
        throw std::runtime_error(std::string("[unsupported] ") + tag + " network off: " + why);
    }

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const
    {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
                return i;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if (bits & (1u << i))
                return i;
        throw std::runtime_error("no memory type for an image or buffer");
    }

    void Destroy()
    {
        if (device)
        {
            nr::binary::forget(device);
            vkDestroyDevice(device, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
        device = {};
        instance = {};
    }
};

// A D3D12 buffer the Vulkan device reads and writes through an imported handle.
struct SharedBuffer
{
    ID3D12Resource* resource {};
    HANDLE handle {};
    VkBuffer buffer {};
    VkDeviceMemory memory {};

    void Create(ID3D12Device* d, Vulkan& vk, UINT64 bytes)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        // Never below 16 MB. In Cyberpunk 2077 the driver refused the handle (E_INVALIDARG) of buffers of 1.9 to 4.2
        // MB, for seconds to minutes, and never of those of 7.4 MB and more.
        rd.Width = std::max((bytes + 65535) & ~UINT64(65535), UINT64(16) << 20);
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                         IID_PPV_ARGS(&resource)),
              "shared buffer");
        if (const HRESULT hr = d->CreateSharedHandle(resource, nullptr, GENERIC_ALL, nullptr, &handle); FAILED(hr))
        {
            // Seen in a game only (E_INVALIDARG); these say which of its causes it was.
            D3D12_HEAP_PROPERTIES got {};
            D3D12_HEAP_FLAGS flags {};
            const HRESULT query = resource->GetHeapProperties(&got, &flags);
            nr::logf("[mochizuki] shared buffer handle failed (0x%08lX): %llu bytes, heap type %d flags 0x%X (query "
                     "0x%08lX), device 0x%08lX, %d other shared buffers alive",
                     static_cast<unsigned long>(hr), static_cast<unsigned long long>(rd.Width), int(got.Type),
                     unsigned(flags), static_cast<unsigned long>(query),
                     static_cast<unsigned long>(d->GetDeviceRemovedReason()), alive.load());
            Check(hr, "shared buffer handle");
        }
        ++alive;

        VkExternalMemoryBufferCreateInfo external { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external };
        bi.size = rd.Width;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkCheck(vkCreateBuffer(vk.device, &bi, nullptr, &buffer), "vkCreateBuffer (shared)");
        VkMemoryRequirements req {};
        vkGetBufferMemoryRequirements(vk.device, buffer, &req);
        VkMemoryWin32HandlePropertiesKHR props { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
        VkCheck(vk.memoryHandleProperties(vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, &props),
                "vkGetMemoryWin32HandlePropertiesKHR");
        VkMemoryDedicatedAllocateInfo dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.buffer = buffer;
        VkImportMemoryWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated };
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        import.handle = handle;
        VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import };
        ai.allocationSize = std::max<UINT64>(req.size, d->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes);
        ai.memoryTypeIndex =
            vk.MemoryType(req.memoryTypeBits & props.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkCheck(vkAllocateMemory(vk.device, &ai, nullptr, &memory), "import of a D3D12 buffer");
        VkCheck(vkBindBufferMemory(vk.device, buffer, memory, 0), "vkBindBufferMemory (shared)");
    }

    void Release(VkDevice device)
    {
        if (buffer)
            vkDestroyBuffer(device, buffer, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        if (handle)
        {
            CloseHandle(handle);
            --alive;
        }
        if (resource)
            resource->Release();
        *this = {};
    }

    static inline std::atomic<int> alive {};
};

// A D3D12 fence of the game's device that the Vulkan device imports as a timeline semaphore.
struct SharedFence
{
    ID3D12Fence* fence {};
    HANDLE handle {};
    VkSemaphore semaphore {};

    void Create(ID3D12Device* d, Vulkan& vk, const char* name)
    {
        char what[64];
        std::snprintf(what, sizeof what, "shared fence (%s)", name);
        Check(d->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)), what);
        Check(d->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &handle), what);
        VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type };
        VkCheck(vkCreateSemaphore(vk.device, &si, nullptr, &semaphore), "vkCreateSemaphore (timeline)");
        VkImportSemaphoreWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
        import.semaphore = semaphore;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        import.handle = handle;
        std::snprintf(what, sizeof what, "import of the D3D12 fence (%s)", name);
        VkCheck(vk.importSemaphore(vk.device, &import), what);
    }

    // The semaphore only with its device.
    void Release(VkDevice device)
    {
        if (semaphore && device)
            vkDestroySemaphore(device, semaphore, nullptr);
        if (handle)
            CloseHandle(handle);
        if (fence)
            fence->Release();
        *this = {};
    }
};

// A COM object's identity, AddRef'd: its IUnknown, or the object itself if it has none.
IUnknown* Identity(IUnknown* object)
{
    IUnknown* id = nullptr;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&id))) || !id)
    {
        object->AddRef();
        id = object;
    }
    return id;
}

// A game queue frames come from, and its COM identity, both AddRef'd.
struct Producer
{
    ID3D12CommandQueue* queue {};
    IUnknown* id {};

    static Producer Of(ID3D12CommandQueue* q)
    {
        q->AddRef();
        return { q, Identity(q) };
    }

    Producer Copy() const
    {
        queue->AddRef();
        id->AddRef();
        return *this;
    }

    void Release()
    {
        if (queue)
            queue->Release();
        if (id)
            id->Release();
        *this = {};
    }
};

// The frames handed to the Vulkan side, oldest first: each one's produced value (toVk), which its Vulkan submit waits
// for, and its finished value (fromVk), which the game queue waits for. Submitted adds, the watchdog drops what fromVk
// has passed. When full it forgets the oldest, long finished by then: the slots let no more than kSlots network
// submits and kFallbacks passthroughs be in flight.
struct Awaited
{
    struct Frame
    {
        UINT64 produced = 0, finished = 0;
    };
    std::mutex mutex;
    Frame frames[64];
    uint32_t first = 0, count = 0;

    void Add(UINT64 produced, UINT64 finished)
    {
        std::lock_guard lock(mutex);
        if (count == std::size(frames))
        {
            first = (first + 1) % std::size(frames);
            --count;
        }
        frames[(first + count++) % std::size(frames)] = { produced, finished };
    }

    // The oldest frame the Vulkan side has not finished, fromVk being at `done`; false when there is none.
    bool Oldest(UINT64 done, Frame& oldest)
    {
        std::lock_guard lock(mutex);
        while (count && frames[first].finished <= done)
        {
            first = (first + 1) % std::size(frames);
            --count;
        }
        if (count)
            oldest = frames[first];
        return count != 0;
    }
};

// Every image of ours: copied into and out of, and sampled, which lets the core read the frame and the vectors in
// place.
constexpr VkImageUsageFlags kImageUsage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

struct Image
{
    VkImage image {};
    VkDeviceMemory memory {};

    void Create(Vulkan& vk, VkFormat format, uint32_t w, uint32_t h)
    {
        VkImageCreateInfo ci { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = { w, h, 1 };
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = kImageUsage;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkCheck(vkCreateImage(vk.device, &ci, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements req {};
        vkGetImageMemoryRequirements(vk.device, image, &req);
        VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = vk.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkCheck(vkAllocateMemory(vk.device, &ai, nullptr, &memory), "vkAllocateMemory (image)");
        VkCheck(vkBindImageMemory(vk.device, image, memory, 0), "vkBindImageMemory");
    }

    void Release(VkDevice device)
    {
        if (image)
            vkDestroyImage(device, image, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        *this = {};
    }
};

void ImageBarrier(VkCommandBuffer c, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage,
                  VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// A transfer's writes to `image` (GENERAL) made visible to the next transfer.
void TransferBarrier(VkCommandBuffer c, VkImage image)
{
    ImageBarrier(c, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
}

// The rectangle [s0, s1) of `src` onto [d0, d1) of `dst`, both in GENERAL (they may be one image).
void Blit(VkCommandBuffer c, VkImage src, VkImage dst, VkOffset2D s0, VkOffset2D s1, VkOffset2D d0, VkOffset2D d1,
          VkFilter filter)
{
    VkImageBlit b {};
    b.srcSubresource = b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    b.srcOffsets[0] = { s0.x, s0.y, 0 };
    b.srcOffsets[1] = { s1.x, s1.y, 1 };
    b.dstOffsets[0] = { d0.x, d0.y, 0 };
    b.dstOffsets[1] = { d1.x, d1.y, 1 };
    vkCmdBlitImage(c, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &b, filter);
}

// The part of `image` in [0, W) x [0, H) past its valid w x h, filled as the image's own edge is past its border: the
// right strip repeats column w - 1, then the bottom strip, corner included, repeats row h - 1. Exact texel copies, each
// doubling the band that repeats the edge (about log2 of the strip's width of them): a NEAREST blit stretching the
// one-texel edge over the strip is not exact, since near the strip's far end its sampling can land on the texel past
// the edge (a 1706 to 2560 strip did, on an RX 9070 XT with driver 26.8.1: WORK/S5). The image is in GENERAL with its
// last transfer writes visible to transfers (TransferBarrier); the strips' writes are left for the caller's next
// barrier.
void Pad(VkCommandBuffer c, VkImage image, uint32_t w, uint32_t h, uint32_t W, uint32_t H)
{
    bool first = true;
    auto copy = [&](VkOffset3D from, VkOffset3D to, uint32_t cw, uint32_t ch)
    {
        if (!first)
            TransferBarrier(c, image); // each copy reads what the last one wrote
        first = false;
        VkImageCopy r {};
        r.srcSubresource = r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.srcOffset = from;
        r.dstOffset = to;
        r.extent = { cw, ch, 1 };
        vkCmdCopyImage(c, image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    };
    // Columns (rows) [edge, edge + n) hold the edge; the first m of them are copied to edge + n.
    for (uint32_t n = 1, m = 0; w - 1 + n < W; n += m)
    {
        m = std::min(n, W - (w - 1 + n));
        copy({ int32_t(w - 1), 0, 0 }, { int32_t(w - 1 + n), 0, 0 }, m, h);
    }
    for (uint32_t n = 1, m = 0; h - 1 + n < H; n += m)
    {
        m = std::min(n, H - (h - 1 + n));
        copy({ 0, int32_t(h - 1), 0 }, { 0, int32_t(h - 1 + n), 0 }, W, m);
    }
}

void Transition(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
    c->ResourceBarrier(1, &b);
}

// A texture's top-left w x h rows, packed the way both APIs copy buffers. With dynamic resolution a frame copies only
// its subrect, the top-left of this, at the same pitch.
struct Footprint
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT d3d {};
    VkBufferImageCopy vk {};
    UINT64 bytes = 0;

    Footprint() = default;
    Footprint(DXGI_FORMAT format, UINT texelBytes, UINT w, UINT h)
    {
        const UINT pitch =
            (w * texelBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~UINT(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        d3d.Footprint = { format, w, h, 1, pitch };
        vk.bufferRowLength = pitch / texelBytes;
        vk.bufferImageHeight = h;
        vk.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        vk.imageExtent = { w, h, 1 };
        bytes = UINT64(pitch) * h;
    }
};

// The texture's top-left w x h into the buffer laid out as fp.
void CopyToBuffer(ID3D12GraphicsCommandList* c, ID3D12Resource* texture, D3D12_RESOURCE_STATES state,
                  ID3D12Resource* buffer, const Footprint& fp, UINT w, UINT h)
{
    Transition(c, texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(c, buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst { buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
    dst.PlacedFootprint = fp.d3d;
    D3D12_TEXTURE_COPY_LOCATION src { texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
    const D3D12_BOX box { 0, 0, 0, w, h, 1 };
    c->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    Transition(c, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Transition(c, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
}

struct Job
{
    // How the frame's motion vectors reach the network: as uploaded (their extent is the frame's, or the whole frame
    // is the network's); with dynamic resolution, uploaded into the bucket-sized image and padded (they cover the
    // subrect), or rescaled onto the subrect there first (another extent).
    enum MotionPath : uint8_t
    {
        Uploaded,
        Padded,
        Rescaled,
    };
    uint32_t state = LMXXF_NR_JOB_NONE;
    bool enqueued = false; // EnqueueHip ran for it: a second call does nothing
    ID3D12Resource* colour {};
    D3D12_RESOURCE_STATES colourState {};
    ID3D12Resource* motion {};
    D3D12_RESOURCE_STATES motionState {};
    // The game's depth, with motion, at the frame's extent (width x height), padded to the bucket with dynamic
    // resolution as the colour is.
    ID3D12Resource* depth {};
    D3D12_RESOURCE_STATES depthState {};
    bool depthInverted = false;
    // The frame's valid extents, copied from the top-left of its colour and motion: the geometry's own, except with
    // dynamic resolution.
    UINT width = 0, height = 0, motionWidth = 0, motionHeight = 0;
    MotionPath motionPath = Uploaded;
    float motionScaleX = 0, motionScaleY = 0;
    bool reset = false;
    bool firstPass = false;           // the first pass goes out too (LMXXF_NR_FRAME_FLAG_FIRST_PASS), PrepareFrame
    nr::Controls controls;            // the frame's, sanitised: validate() cannot refuse them
    float history = 1.f, white = 1.f; // set on the runtime before the frame is recorded
    nr::renodx::Settings reno;        // for a network in the RenoDX stage (ProductRuntime::reno), in place of controls
    float renoExposure = 0.f;         // the game's exposure (MochizukiNrSetExposure), 0 for none
    bool styleGrade = true;           // set on the runtime before the frame is recorded
};

// The handle PrepareFrame hands out for job generation gen: odd, so never null and never a pointer.
void* JobHandle(uint64_t gen) { return reinterpret_cast<void*>(uintptr_t((gen << 1) | 1)); }

// Everything sized by the frame; the frame's buffers are made again when any of it changes. The extent the network and
// the colour buffers are built for (width, height), the colour allocation, and the motion buffer's extent: in exact,
// the frame's render subrect and the motion's valid extent; with dynamic resolution (bucketed), the bucket and the
// motion allocation, so that a subrect change is no geometry change (the frame's valid extents are the job's).
struct Geometry
{
    UINT width = 0, height = 0;
    DXGI_FORMAT colourFormat = DXGI_FORMAT_UNKNOWN;
    VkFormat netFormat = VK_FORMAT_UNDEFINED; // the network's colour format where it is not the frame's (netImage)
    UINT64 colourWidth = 0;
    UINT colourHeight = 0;
    DXGI_FORMAT motionFormat = DXGI_FORMAT_UNKNOWN;
    UINT motionWidth = 0, motionHeight = 0;
    bool bucketed = false; // the motion buffer is the allocation's, and a bucket-sized motion image is made too
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN; // the game's depth, its buffers at (width, height)
    bool firstPass = false;                        // LMXXF_NR_FRAME_FLAG_FIRST_PASS: pass 1's answer has buffers too
    bool operator==(const Geometry&) const = default;
};

// A file's bytes; empty when it cannot be read.
static std::vector<uint8_t> ReadWhole(const std::filesystem::path& p)
{
    std::vector<uint8_t> bytes;
    if (FILE* f = _wfopen(p.c_str(), L"rb"))
    {
        std::fseek(f, 0, SEEK_END);
        bytes.resize(size_t(_ftelli64(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
            bytes.clear();
        std::fclose(f);
    }
    return bytes;
}

// SHA-256 in lower-case hex; empty when it fails.
static std::string Sha256Hex(const std::vector<uint8_t>& bytes)
{
    uint8_t hash[32] {};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<uint8_t*>(bytes.data()), ULONG(bytes.size()), hash,
                   sizeof hash) != 0)
        return {};
    char text[65] {};
    for (int i = 0; i < 32; ++i)
        std::snprintf(text + 2 * i, 3, "%02x", hash[i]);
    return text;
}

// A network's own weights, the model pack with a difference file XORed in (upstream's experimental_weights.py, its
// install.ps1 New-ExperimentalWeights): each entry of the difference XOR the pack's entry of that name, or, for a name the
// pack does not have ("performance/<name>"), XOR the pack's <name> as it was, added after the pack's own entries. Kept
// in aco-cache once their SHA-256 is the one beside the difference: what the runtime writes there goes when mochizuki is
// uninstalled. Made here, at first use, because the difference is all a release ships: none of NVIDIA's weights. False,
// with the reason logged; the core then builds the network the weights were made from.
static bool MakeWeights(const char* what, const std::filesystem::path& model, const std::filesystem::path& dir,
                        const wchar_t* deltaName, const wchar_t* hashName, const std::filesystem::path& out)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_regular_file(out, ec))
        return true;
    fs::create_directories(out.parent_path(), ec);
    const std::string modelName = model.filename().string();
    const auto fail = [what](const std::string& why)
    {
        nr::logf("[mochizuki] %s's weights: %s", what, why.c_str());
        return false;
    };
    std::vector<uint8_t> pack = ReadWhole(model);
    const std::vector<uint8_t> delta = ReadWhole(dir / deltaName), wanted = ReadWhole(dir / hashName);
    if (pack.size() < 16 || std::memcmp(pack.data(), "NRMODEL1", 8))
        return fail("no " + modelName);
    if (delta.size() < 12 || std::memcmp(delta.data(), "NRXDIFF1", 8) || wanted.size() < 64)
        return fail("aco\\" + dir.filename().string() + " is incomplete");
    const auto u32 = [](const uint8_t* p)
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    };
    const auto u64 = [](const uint8_t* p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    };
    // Both files are read with every offset checked: a short or damaged one fails here and the hash below stays right.
    struct Entry
    {
        uint64_t offset, size;
    };
    std::vector<std::pair<std::string, Entry>> index;
    size_t at = 16;
    const uint32_t packCount = u32(pack.data() + 8);
    for (uint32_t k = 0; k < packCount; ++k)
    {
        if (at + 4 > pack.size() || at + 4 + u32(pack.data() + at) + 16 > pack.size())
            return fail(modelName + " is damaged");
        const uint32_t n = u32(pack.data() + at);
        index.push_back({ std::string(reinterpret_cast<const char*>(pack.data() + at + 4), n),
                          Entry { u64(pack.data() + at + 4 + n), u64(pack.data() + at + 12 + n) } });
        at += 4 + size_t(n) + 16;
    }
    const size_t indexEnd = at;
    std::sort(index.begin(), index.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const auto find = [&index](const std::string& name)
    {
        const auto e = std::lower_bound(index.begin(), index.end(), name,
                                        [](const auto& a, const std::string& b) { return a.first < b; });
        return e != index.end() && e->first == name ? e : index.end();
    };
    const std::string mismatch = modelName + " is not the model its difference file was made from";
    // Two passes: the added entries first, from the pack as it is, then the changed ones in place.
    std::vector<std::pair<std::string, std::vector<uint8_t>>> added;
    for (int pass = 0; pass < 2; ++pass)
    {
        at = 12;
        for (uint32_t k = 0, count = u32(delta.data() + 8); k < count; ++k)
        {
            if (at + 4 > delta.size() || at + 4 + u32(delta.data() + at) + 8 > delta.size())
                return fail("the difference file is damaged");
            const uint32_t n = u32(delta.data() + at);
            const std::string name(reinterpret_cast<const char*>(delta.data() + at + 4), n);
            const uint64_t size = u64(delta.data() + at + 4 + n);
            at += 4 + size_t(n) + 8;
            if (size > delta.size() - at)
                return fail("the difference file is damaged");
            auto e = find(name);
            const bool add = e == index.end();
            if (add)
            {
                const size_t slash = name.find('/');
                e = slash == std::string::npos ? index.end() : find(name.substr(slash + 1));
                for (const auto& a : added)
                    if (pass == 0 && a.first == name)
                        return fail("the difference file is damaged");
            }
            if (e == index.end() || e->second.size != size || e->second.offset + size > pack.size())
                return fail(mismatch);
            if (add && pass == 0)
            {
                std::vector<uint8_t> bytes(pack.begin() + ptrdiff_t(e->second.offset),
                                           pack.begin() + ptrdiff_t(e->second.offset + size));
                for (uint64_t i = 0; i < size; ++i)
                    bytes[i] ^= delta[at + i];
                added.push_back({ name, std::move(bytes) });
            }
            else if (!add && pass == 1)
                for (uint64_t i = 0; i < size; ++i)
                    pack[e->second.offset + i] ^= delta[at + i];
            at += size_t(size);
        }
        if (pass == 0 && at != delta.size())
            return fail("the difference file is damaged");
    }
    if (!added.empty())
    {
        // Index lines for them, every offset moved by those lines, their bytes at the end.
        uint64_t grow = 0;
        for (const auto& a : added)
            grow += 4 + a.first.size() + 16;
        std::vector<uint8_t> grown(pack.begin(), pack.begin() + ptrdiff_t(indexEnd));
        const uint32_t total = packCount + uint32_t(added.size());
        std::memcpy(grown.data() + 8, &total, 4);
        for (size_t p = 16; p < indexEnd;)
        {
            const uint32_t n = u32(grown.data() + p);
            const uint64_t moved = u64(grown.data() + p + 4 + n) + grow;
            std::memcpy(grown.data() + p + 4 + n, &moved, 8);
            p += 4 + size_t(n) + 16;
        }
        const auto put = [&grown](const void* v, size_t bytes)
        { grown.insert(grown.end(), static_cast<const uint8_t*>(v), static_cast<const uint8_t*>(v) + bytes); };
        uint64_t end = pack.size() + grow;
        for (const auto& a : added)
        {
            const uint32_t n = uint32_t(a.first.size());
            const uint64_t size = a.second.size();
            put(&n, 4);
            put(a.first.data(), n);
            put(&end, 8);
            put(&size, 8);
            end += size;
        }
        grown.insert(grown.end(), pack.begin() + ptrdiff_t(indexEnd), pack.end());
        for (const auto& a : added)
            grown.insert(grown.end(), a.second.begin(), a.second.end());
        pack = std::move(grown);
    }
    if (Sha256Hex(pack).compare(0, 64, reinterpret_cast<const char*>(wanted.data()), 64))
        return fail(mismatch + " (SHA-256)");
    const fs::path tmp = out.wstring() + L".tmp"; // a .nrb.tmp, which an uninstall takes too
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    const bool written = f && std::fwrite(pack.data(), 1, pack.size(), f) == pack.size();
    if (f)
        std::fclose(f);
    fs::rename(tmp, out, ec);
    if (!written || ec)
    {
        fs::remove(tmp, ec);
        return fail("could not write them in aco-cache");
    }
    nr::logf("[mochizuki] %s's weights made", what);
    return true;
}

// CC_SMALL_FP8_EXPERIMENTAL's weights: dlssnr-small.bin with aco\experimental's difference.
static bool MakeExperimentalWeights()
{
    const std::filesystem::path data = std::filesystem::path(DataDirectory()) / L"dlssnr-amd";
    return MakeWeights("CC_SMALL_FP8_EXPERIMENTAL", data / L"dlssnr-small.bin", data / L"aco" / L"experimental",
                       L"dlssnr-small-experimental.delta", L"dlssnr-small-experimental.sha256",
                       data / L"aco-cache" / L"dlssnr-small-experimental.nrb");
}

// CC_SILVER_AARDWOLD_OPTIMIZED's weights: dlssnr.bin with aco\optimized's difference (its "performance/" entries added for
// Performance and Ultra Performance). With `int4`, also its int4 layers' weights, made from those by the core's own
// decoder with int4-ultra's tables, which cover every level's (upstream's install.ps1 runs dlssnr-int4-weights.exe the
// same way), checked against the hash in aco\optimized. Seconds of CPU the first time: the builder thread only. False
// when the FP8 weights cannot be made (the core then builds CC_SILVER_AARDWOLD); `int4` turns false when the int4 ones
// cannot (the core then runs Quality).
static bool MakeOptimizedWeights(bool& int4)
{
    namespace fs = std::filesystem;
    const fs::path data = fs::path(DataDirectory()) / L"dlssnr-amd", oroot = data / L"aco" / L"optimized";
    const fs::path pack = data / L"aco-cache" / L"dlssnr-optimized.nrb";
    if (!MakeWeights("CC_SILVER_AARDWOLD_OPTIMIZED", data / L"dlssnr.bin", oroot, L"dlssnr-optimized.delta",
                     L"dlssnr-optimized.sha256", pack))
        return int4 = false;
    const fs::path out = data / L"aco-cache" / L"dlssnr-optimized-int4.nrb";
    std::error_code ec;
    if (!int4 || fs::is_regular_file(out, ec))
        return true;
    const fs::path tmp = out.wstring() + L".tmp";
    const auto t0 = std::chrono::steady_clock::now();
    std::string why;
    if (nr::make_int4_weights(pack, data / L"int4-ultra", tmp, &why))
    {
        const std::vector<uint8_t> made = ReadWhole(tmp), wanted = ReadWhole(oroot / L"dlssnr-optimized-int4.sha256");
        const std::string hash = made.empty() ? std::string() : Sha256Hex(made);
        if (hash.empty())
            why = "could not read them back";
        else if (wanted.size() < 64 || hash.compare(0, 64, reinterpret_cast<const char*>(wanted.data()), 64))
            why = "SHA-256 " + hash + ", not the one in aco\\optimized";
    }
    if (why.empty())
    {
        fs::rename(tmp, out, ec);
        if (ec)
            why = "could not write them in aco-cache";
    }
    if (!why.empty())
    {
        fs::remove(tmp, ec);
        nr::logf("[mochizuki] CC_SILVER_AARDWOLD_OPTIMIZED's int4 weights: %s; its Optimized layers run at Quality",
                 why.c_str());
        int4 = false;
        return true;
    }
    nr::logf("[mochizuki] CC_SILVER_AARDWOLD_OPTIMIZED's int4 weights made in %.1f s",
             std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

// What a network is built for: the frame extent (the bucket, with dynamic resolution), colour format, model scale
// (quantised), pass count and colour encoding.
struct NetworkKey
{
    UINT width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    float scale = 1.f;
    uint32_t maxPasses = 1;
    bool linear = false;
    bool prep = false;      // RuntimeConfig::preprocess
    bool int4 = false;      // CC_SILVER_AARDWOLD_OPTIMIZED, int4 mixed's successor (LMXXF_NR_FRAME_FLAG_INT4)
    bool cascade = false;   // RuntimeConfig::cascade_detail_only (LMXXF_NR_FRAME_FLAG_CASCADE_DETAIL), with passes
    bool original = false;  // RuntimeConfig::network 0, the 310.8 model (LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL)
    bool firstPass = false; // RuntimeConfig::first_pass_output (LMXXF_NR_FRAME_FLAG_FIRST_PASS), with passes
    bool hist16 = true;     // RuntimeConfig::history_f16 (MochizukiNrControls::history_precision)
    bool experimental = false; // RuntimeConfig::network 2 (LMXXF_NR_FRAME_FLAG_EXPERIMENTAL)
    // With int4: CC_SILVER_AARDWOLD_OPTIMIZED (RuntimeConfig::network 3) at this level of its Optimized layers
    // (RuntimeConfig::layers, MochizukiNrControls::optimized_layers), 0 Quality .. 3 Ultra Performance.
    uint32_t layers = 0;
    // MochizukiNrControls::renodx: the network runs inside the RenoDX stage (ProductRuntime::reno), at the stage's
    // extent (renoWidth x renoHeight), one pass at a time, maxPasses being the stage's pass count. The source encoding
    // and the linear unit decide the stage's units for its life.
    bool renodx = false;
    UINT renoWidth = 0, renoHeight = 0;
    uint32_t sourceEncoding = 0;
    float linearNits = 0.f;
    bool operator==(const NetworkKey&) const = default;
};

// A network and, with the RenoDX stage, the stage around it, whose images go with the network's: the stage and its 1x1
// image of the game's exposure (RenoDX's feed), which each frame's recording clears to the value it was given.
struct ProductRuntime : nr::Runtime
{
    std::unique_ptr<nr::renodx::Stage> reno;
    uint32_t renoPasses = 0; // the stage's passes on the last frame: a pass past them drops its history
    Image exposure;
    // With NetworkKey::firstPass: the stage's pass 1 (StageFrame::first), RGBA32F at the frame's extent, as the
    // core's first_pass_output() is; null where the stage cannot write it.
    Image first;
    VkDevice device {};
    ProductRuntime(const nr::HostDevice& host, const nr::RuntimeConfig& config, const nr::TemporalConfig& temporal,
                   const NetworkKey& key, Vulkan& vk)
        : nr::Runtime(host, config, nr::ControlMaskConfig {}, temporal)
    {
        if (!key.renodx)
            return;
        nr::renodx::StageConfig sc;
        sc.shaders = config.root + "/dlssnr-amd/shaders/renodx";
        sc.width = key.width;
        sc.height = key.height;
        sc.nr_width = key.renoWidth;
        sc.nr_height = key.renoHeight;
        sc.frame_format = key.format;
        sc.hdr_flag = key.linear;
        sc.settings.source_encoding = key.sourceEncoding;
        sc.settings.linear_unit_nits = key.linearNits;
        if (key.firstPass)
            sc.first_format = VK_FORMAT_R32G32B32A32_SFLOAT;
        reno = std::make_unique<nr::renodx::Stage>(host, sc);
        device = host.device;
        exposure.Create(vk, VK_FORMAT_R32_SFLOAT, 1, 1);
        if (key.firstPass && reno->exports_first())
            first.Create(vk, VK_FORMAT_R32G32B32A32_SFLOAT, key.width, key.height);
    }
    ~ProductRuntime()
    {
        exposure.Release(device);
        first.Release(device);
    }
};

// Super Resolution's texture for a frame whose colour is `colour`: its extent and format, one mip, UAV. A bucketed
// geometry keeps it at the largest allocation it holds (Geometry::colourWidth), so a game that reallocates its colour
// at every dynamic resolution step gets the same texture; the frame is copied into its top-left.
D3D12_RESOURCE_DESC ResultDesc(ID3D12Resource* colour, const Geometry& g)
{
    D3D12_RESOURCE_DESC td = colour->GetDesc();
    if (g.bucketed)
    {
        td.Width = (std::max) (td.Width, g.colourWidth);
        td.Height = (std::max) (td.Height, g.colourHeight);
    }
    td.MipLevels = 1;
    td.DepthOrArraySize = 1;
    td.SampleDesc = { 1, 0 };
    td.Alignment = 0;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return td;
}

bool SameDesc(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
{
    return a.Dimension == b.Dimension && a.Alignment == b.Alignment && a.Width == b.Width && a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality &&
           a.Layout == b.Layout && a.Flags == b.Flags;
}

// The frame's buffers for one geometry (Session::MakeBuffers): the shared buffers the Vulkan device imports, its
// images, and Super Resolution's texture. Whoever holds a set releases it (Release), once no GPU work uses it.
struct FrameBuffers
{
    Geometry geometry;
    D3D12_RESOURCE_DESC resultDesc {};
    Footprint colourFootprint, motionFootprint, depthFootprint;
    VkFormat colourVk = VK_FORMAT_UNDEFINED, motionVk = VK_FORMAT_UNDEFINED, depthVk = VK_FORMAT_UNDEFINED;
    SharedBuffer input, output, motionBuffer, depthBuffer;
    Image colourImage, motionImage, depthImage;
    Image motionScaled; // bucketed geometry with motion: the vectors laid over the subrect, at the bucket's extent
    Image netImage;     // Geometry::netFormat: the frame converted to the network's format and back
    ID3D12Resource* result {};
    // Geometry::firstPass: the core's RGBA32F first pass, through a buffer of its own into an RGBA32F texture.
    Footprint firstFootprint;
    SharedBuffer firstOutput;
    ID3D12Resource* firstResult {};

    void Release(VkDevice device)
    {
        if (device)
        {
            input.Release(device);
            output.Release(device);
            motionBuffer.Release(device);
            depthBuffer.Release(device);
            colourImage.Release(device);
            netImage.Release(device);
            motionImage.Release(device);
            depthImage.Release(device);
            motionScaled.Release(device);
            firstOutput.Release(device);
        }
        if (result)
            result->Release();
        result = nullptr;
        if (firstResult)
            firstResult->Release();
        firstResult = nullptr;
    }
};

constexpr uint32_t kSlots = 3;
constexpr uint32_t kGpuSamples = 120;    // the frames GetStatus's network median and p95 cover
constexpr uint32_t kHistorySamples = 64; // the frames GetInfo's history_consumed_pct covers, one bit each
// The wait for an earlier frame's slot. Past it the GPU is taken to be stuck: the frame passes through, and after
// kSlotTimeoutsLost such waits in a row the device counts as lost.
constexpr uint64_t kSlotWaitNs = 250'000'000;
constexpr uint32_t kSlotTimeoutsLost = 3;
constexpr uint32_t kFallbacks = 2; // passthrough command buffers, each with its fence
// A drain's wait for the Vulkan side, once the game's queues are idle: fromVk, then the submits' fences, each this
// long at most.
constexpr uint32_t kDrainWaitMs = 2'000;
// Destroy during a network build: the build is told to stop and waited for this long; past it, Destroy returns and the
// build's thread ends the session (Session::Abandon).
constexpr uint32_t kAbandonWaitMs = 2'000;
// The watchdog reads fromVk this often. A frame the game produced that the Vulkan side has not finished through
// kStallMs of reads in a row (above the 2 s TdrDelay, so a TDR comes first) counts as a lost device.
constexpr uint32_t kWatchMs = 250;
constexpr uint32_t kStallMs = 5'000;
// Out of memory: a network build or the frame's buffers are tried again after these, then no more for that key.
constexpr ULONGLONG kOomRetryMs[] = { 5'000, 30'000, 120'000 };
// The frame's buffers after the driver refused a shared handle (E_INVALIDARG; SharedBuffer's 16 MB floor keeps it
// away in Cyberpunk 2077): sooner and more often than after running out of memory.
constexpr ULONGLONG kInvalidRetryMs[] = { 1'000, 2'000, 5'000, 10'000, 30'000, 60'000 };
constexpr ULONGLONG kVramRetryMs = 10'000; // the VRAM check, after it refused
// The VRAM check: what a network costs (its weights and fixed buffers, then per model pixel, times a factor for the
// passes it is built for, plus images at the frame's extent when the model is scaled down), what the frame's buffers
// cost per frame pixel, and a margin; allowed while it all fits in this share of the budget. The network's terms are
// this process's VRAM growth while a network was built on an RX 9070 XT, driver 26.8.1, at 720p to 2160p, 1 to 3
// passes and model scale 0.5 to 1 (WORK/S2/fix1/calib): every estimate is 0.5-4.3% above what was measured. The per
// model pixel term adds 16 bytes to that calibration for the second RGBA32F history image a one-pass network keeps.
constexpr UINT64 kNetworkFixedBytes = 160ull << 20;
constexpr UINT64 kModelPixelBytes = 288;
constexpr double kPassFactor[] = { 1.0, 1.2, 1.3 }; // by the passes built for (max_passes), 1 to 3
// Past three passes, each one more as the step from one to two (TheAutomatic/dlss-5-amd-project's estimate for its
// 30-layer mode): not measured, on the safe side.
double PassMemoryFactor(uint32_t passes)
{
    passes = std::clamp(passes, 1u, kMaxPasses);
    if (passes <= std::size(kPassFactor))
        return kPassFactor[passes - 1];
    return kPassFactor[std::size(kPassFactor) - 1] + 0.2 * double(passes - std::size(kPassFactor));
}
constexpr UINT64 kScaledFramePixelBytes = 36;
constexpr UINT64 kFramePixelBytes = 48;
constexpr UINT64 kVramMarginBytes = 64ull << 20;
constexpr double kVramBudgetShare = 0.9;
constexpr uint32_t kErrorsLogged = 10; // a session's first errors logged in full; then one in 1000
// Dynamic resolution (Session::DrsExtent): the bucket's alignment, and how long every frame must stay this far inside
// it on both axes before it shrinks to what those frames needed (a rebuild).
constexpr UINT kBucketAlign = 64;
constexpr UINT kBucketShrinkMargin = 128;
constexpr ULONGLONG kBucketShrinkMs = 30'000;

// Held by a network build from the prewarm to the end of the core's constructor (Session::Build), so that builds never
// overlap in the process: the core's build writes process-wide state (the model pack's index, which every weight read
// looks up; the arena probe's swap of the log sink; the tile and shape switches), and so do P3's prewarm and its
// pipeline.cache and manifest files. A build Destroy left running (Session::Abandon) holds it until it returns. Never
// destroyed: such a build may still hold it when the process exits.
std::timed_mutex& CoreBuildMutex()
{
    static auto* mutex = new std::timed_mutex;
    return *mutex;
}

// A step that failed for a key (a network build, the frame's buffers) or that the VRAM check refused: not run again
// for that key before `until`, or ever when `again` is false. Any other key runs at once.
template <class Key> struct Hold
{
    bool active = false, again = false;
    Key key {};
    ULONGLONG until = 0;
    uint32_t oomFailures = 0; // out-of-memory failures in a row for this key
    std::string message;      // what PrepareFrame says while it holds

    bool Holds(const Key& k, ULONGLONG now) const { return active && key == k && (!again || now < until); }

    // The step failed for k: out of memory is tried again after kOomRetryMs, and an invalid argument, when
    // `invalidToo`, after kInvalidRetryMs; then no more for k. Any other kind never for k. The delay in ms, 0 for none.
    ULONGLONG Failed(const Key& k, NrError::Kind kind, const char* text, bool invalidToo = false)
    {
        const bool invalid = invalidToo && kind == NrError::Invalid;
        const uint32_t oom = kind == NrError::OutOfMemory || invalid ? (active && key == k ? oomFailures : 0) + 1 : 0;
        const std::span<const ULONGLONG> retry = invalid ? std::span<const ULONGLONG>(kInvalidRetryMs) : kOomRetryMs;
        const ULONGLONG delay = oom && oom <= retry.size() ? retry[oom - 1] : 0;
        Set(k, delay != 0, delay, text);
        oomFailures = oom;
        return delay;
    }

    // The VRAM check refused k: tried again after kVramRetryMs.
    void Refused(const Key& k, const char* text)
    {
        const uint32_t oom = active && key == k ? oomFailures : 0;
        Set(k, true, kVramRetryMs, text);
        oomFailures = oom;
    }

    void Set(const Key& k, bool retry, ULONGLONG ms, const char* text)
    {
        active = true;
        again = retry;
        key = k;
        until = GetTickCount64() + ms;
        message = text;
    }
};

struct Session
{
    ID3D12Device* device {};
    ID3D12CommandQueue* queue {};
    Vulkan vk;
    // The frame's two sync points, one per direction, each with its own counter: the producer queue signals the
    // frame's produced value into toVk, which its Vulkan submit waits for; the submit signals the frame's finished
    // value into fromVk, which the producer queue waits for. Only the Vulkan queue signals fromVk (and Lose and
    // Rerelease, from the CPU, to its maximum), so no game queue can move it backwards, and by default WDDM forces it
    // to its maximum when the Vulkan device is reset, which frees every game queue waiting on it. Game queues signal
    // toVk one frame after another (Follow), and nothing signals it from the CPU: the Vulkan side never runs a frame
    // the game has not produced.
    SharedFence toVk, fromVk;
    UINT64 producedValue = 0, finishedValue = 0; // the last values handed out, under submitMutex
    UINT64 lastSignalled = 0; // the last produced value a game queue signalled into toVk, under submitMutex
    // The last frame handed to the Vulkan side (Submitted), whose finished value fromVk will reach whether or not the
    // game queue's wait went through: Follow's and Drain's waits, the watchdog's log.
    std::atomic<UINT64> lastProduced { 0 }, lastFinished { 0 };
    // The game queues frames come from (EnqueueHip's), under submitMutex: the last frame's, and every one since the
    // last drain, which Drain and ~Session drain besides `queue`.
    Producer lastProducer;
    bool lastProducerListed = false; // it is in producers
    std::vector<Producer> producers;
    uint64_t queueSwitches = 0;
    // The watchdog (Watch), from the first Enqueue until ~Session, and the frames it watches.
    std::thread watchdog;
    bool watchdogStarted = false; // under submitMutex
    std::mutex watchMutex;
    std::condition_variable watchWake;
    bool watchStop = false;
    Awaited awaited;
    std::atomic<bool> released { false }; // Lose's signal of fromVk to its maximum was made
    uint32_t rereleases = 0;              // Rerelease's signals: the watchdog's, then ~Session's
    VkCommandPool pool {};
    VkCommandBuffer commands[kSlots] {};
    VkFence done[kSlots] {};
    bool inFlight[kSlots] {};
    uint32_t nextSlot = 0;
    uint32_t slotTimeouts = 0; // timed-out slot waits in a row
    uint64_t enqueueCalls = 0; // Enqueue calls, for the MZ_TEST_* hooks
    // The passthrough (Passthrough): input copied to output, each command buffer with its own fence, recorded for the
    // current geometry when it is next used (one of a frame before the geometry changed may still run until then).
    VkCommandBuffer fallbackCommands[kFallbacks] {};
    VkFence fallbackDone[kFallbacks] {};
    bool fallbackInFlight[kFallbacks] {};
    bool fallbackCurrent[kFallbacks] {}; // recorded for the current geometry's buffers
    uint32_t nextFallback = 0;
    uint64_t passthroughs = 0; // frames passed through
    // The frames' VkQueue, the slots and fallbacks, and the network the frames record with; a build holds it only when
    // the builds have no queue of their own (vk.buildQueue is vk.queue).
    std::mutex submitMutex;
    // Sticky, and set only with deviceLost (Lose): every other error leaves the session usable.
    std::atomic<bool> failed { false }, deviceLost { false };
    // Set before the first build starts and before every submit. Until then no GPU queue has seen anything of this
    // session, so it can always be freed.
    std::atomic<bool> gpuUsed { false };
    bool aco = false;                // Create's LMXXF_NR_CREATE_FLAG_ACO (ConfigureAco)
    bool atExit = false;             // MochizukiNrDestroyAtExit: a game queue gets kDrainWaitMs to drain, not 30 s
    bool zeroOutputFallback = false; // Create's LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK: frames pass through
    TestHooks hooks;
    std::atomic<uint32_t> errors { 0 }; // failed calls and passed-through frames, for the log's rate limit

    // The network for one extent, colour format, model scale, pass count and colour encoding, built off the render
    // thread: `runtime` is the one the frames record with (for `net`), `built` a finished build's (for buildKey) until
    // EnsureNetwork installs it. With a queue of their own for the builds, a network for another model scale, pass
    // count or colour encoding is built while the frames keep the old one (keep serving); runtime and net change under
    // submitMutex. Only EnsureNetwork and ~Session join the builder, under buildMutex once buildDone is set.
    std::unique_ptr<ProductRuntime> runtime;
    NetworkKey net, buildKey;
    std::thread builder;
    std::mutex buildMutex;
    std::condition_variable buildEnded; // buildDone was set
    bool building = false, buildDone = false;
    // Destroy during a build (Abandon): the build stops between two pipelines, and when it has not ended within
    // kAbandonWaitMs its thread owns the session and ends it (EndAbandoned).
    std::atomic<bool> abandon { false };
    bool builderOwnsSession = false; // under buildMutex
    std::chrono::steady_clock::time_point abandonedAt;
    std::unique_ptr<ProductRuntime> built;
    char buildError[1024] {}; // a fixed buffer, so the builder cannot throw on its way to buildDone
    std::string fp16Off;      // under buildMutex: why an FP16 network stays off for the session (Fp16Pipelines)
    NrError::Kind buildErrorKind = NrError::Other;
    double buildSeconds = 0;
    // Networks the frames no longer use (RetireNetwork), each with the finished value of the last frame handed to the
    // Vulkan side while the frames had it: the releaser thread frees them, oldest first, once fromVk is there
    // (Releaser). What is left when the session ends goes after its drain, or leaks with a lost device.
    struct Retired
    {
        std::unique_ptr<ProductRuntime> network;
        UINT64 finished = 0;
    };
    std::mutex releaseMutex;
    std::condition_variable releaseWake; // a network retired, one freed, releaseStop, or the releaser ended
    std::vector<Retired> retired;        // under releaseMutex
    size_t releasing = 0;                // retired networks not freed yet, under releaseMutex
    bool releaserEnded = false;          // under releaseMutex
    std::atomic<bool> releaseStop { false };
    std::thread releaser; // started by ReleaseLater (render thread), joined by ~Session
    // Why the network for a key, or the buffers for a geometry, are not being made; render thread only.
    Hold<NetworkKey> buildHold;
    Hold<Geometry> geometryHold;
    // The VRAM check's view of the adapter (opened in Start), and what a network was measured to cost: this process's
    // growth while measuredKey was built, or what releasing it gave back, whichever is more. Render thread only.
    VideoMemory vram;
    NetworkKey measuredKey;
    UINT64 measuredBytes = 0;
    UINT64 buildBaseUsage = 0; // this process's VRAM use when the running build was admitted; 0 when unknown
    UINT64 buildGrowth = 0;    // what the finished build's network added to it; 0 when unknown

    // runtime->last_gpu_ms() of the last kGpuSamples frames, for GetStatus. Pushed under submitMutex as well.
    std::mutex statsMutex;
    float gpuMs[kGpuSamples] {};
    uint32_t gpuMsCount = 0, gpuMsNext = 0;

    // SetControls' last word, which PrepareFrame copies into every job. The mutex only guards a host that sets them
    // from another thread than the render thread.
    std::mutex controlsMutex;
    SessionControls controls;
    uint32_t lastPasses = 0; // the previous frame's pass count; a change drops the history

    // What GetInfo reports: atomics and a copy of the last error, never the runtime, since it may ask from any thread.
    std::atomic<bool> infoBuilding { false };
    std::atomic<uint32_t> infoModelWidth { 0 }, infoModelHeight { 0 }, infoFrameWidth { 0 }, infoFrameHeight { 0 },
        infoMaxPasses { 0 }, infoPasses { 1 }, motionRefused { 0 }, dispatches { 0 }, infoReno { 0 }, infoExperimental { 0 }, infoOptimized { 0 };
    std::atomic<float> infoBuildSeconds { 0 };
    std::atomic<float> infoPrepEv { NAN };
    std::atomic<float> infoWhite { 1.f };
    // MochizukiNrSetExposure's last word: the game's exposure, 0 for none.
    std::atomic<float> gameExposure { 0.f };
    // Set by the first frame that asks for the preprocess: from then on every network is built able to run it.
    bool prepWanted = false;
    // The float colour format of the last frame that had one, and, once a frame came in the other one, RGBA16F for
    // every float frame's network (render thread).
    DXGI_FORMAT floatFormat = DXGI_FORMAT_UNKNOWN;
    bool floatShared = false;
    // One bit a frame, newest lowest: its history was consumed. Written under submitMutex only.
    std::atomic<uint64_t> historyBits { 0 };
    std::atomic<uint32_t> historyCount { 0 };
    std::mutex errorMutex;
    char lastError[256] {};

    Geometry geometry;
    Footprint colourFootprint, motionFootprint, depthFootprint;
    VkFormat colourVk = VK_FORMAT_UNDEFINED, motionVk = VK_FORMAT_UNDEFINED, depthVk = VK_FORMAT_UNDEFINED;
    SharedBuffer input, output, motionBuffer, depthBuffer;
    Image colourImage, motionImage, motionScaled, depthImage, netImage;
    std::atomic<uint32_t> depthRefused { 0 }; // the DXGI format of a depth the runtime cannot take, logged once
    ID3D12Resource* result {};                // what Super Resolution gets in place of the game's colour
    // Geometry::firstPass: the core's first pass on its way out, and the texture person protection reads.
    Footprint firstFootprint;
    SharedBuffer firstOutput;
    ID3D12Resource* firstResult {};

    // Dynamic resolution (DrsExtent), render thread only: the bucket, and what it has seen.
    struct Bucket
    {
        UINT64 allocWidth = 0; // the colour allocation auto engaged in
        UINT allocHeight = 0;
        DXGI_FORMAT allocFormat = DXGI_FORMAT_UNKNOWN;
        bool engaged = false;             // auto: a subrect smaller than that allocation came
        UINT maxWidth = 0, maxHeight = 0; // the largest subrect seen
        UINT capWidth = 0, capHeight = 0; // the largest allocation seen (always)
        // Auto: the largest colour allocation of one format and aspect, and whether the game has reallocated it at
        // another size since (Cyberpunk 2077 makes a new colour texture at every step of its dynamic resolution):
        // those steps are then buckets too, held at bigWidth x bigHeight.
        UINT bigWidth = 0, bigHeight = 0;
        DXGI_FORMAT bigFormat = DXGI_FORMAT_UNKNOWN;
        bool reallocated = false;
        // The allocation a bucketed geometry is keyed on (Geometry::colourWidth), and the largest motion allocation it
        // has seen: the frame's buffers stay across the steps.
        UINT limitWidth = 0, limitHeight = 0;
        UINT motionWidth = 0, motionHeight = 0;
        // Bucketed frames of this allocation, all kBucketShrinkMargin inside the bucket, since lowSince (0: the last
        // frame was not one of them, or the allocation changed), and the largest subrect among them.
        ULONGLONG lowSince = 0;
        UINT lowWidth = 0, lowHeight = 0;
        UINT loggedWidth = 0, loggedHeight = 0; // the bucket the log last showed
    } bucket;
    // Colour and motion formats the bucket may blit (Blittable), render thread only.
    std::vector<std::pair<VkFormat, VkFormatFeatureFlags>> formatFeatures;
    bool drsRefusedLogged = false;
    // The last frame's valid extent: a change restarts the history. The atomics are GetStatus's copy.
    UINT validWidth = 0, validHeight = 0;
    std::atomic<uint32_t> infoValidWidth { 0 }, infoValidHeight { 0 };

    // The frame's job, one at a time, handed out as JobHandle(jobGen). jobMutex guards its state and the job
    // bookkeeping below against a Retire, Cancel or Poll from another thread; the rest of the job is the host's to
    // order (PrepareFrame, Record*, EnqueueHip).
    std::mutex jobMutex;
    Job job;
    uint64_t jobGen = 0;
    uint64_t retiredGen = 0; // the last job Retire acted on
    // A job cancelled after its inputs, or outputs, were recorded: the game may still run that list, which copies
    // into the geometry's buffers and result. Until a later job was retired and a drain passed, ReleaseGeometry
    // buries them in the graveyard (D3D12 references only) instead of freeing them.
    bool orphaned = false;
    uint64_t orphanGen = 0;
    std::vector<ID3D12Resource*> graveyard;
    uint64_t graveyardGen = 0; // the newest orphaned job whose buffers are in it
    // The frame's buffers from before the frame changed (RetireGeometry), render thread only. They go once the Vulkan
    // side has finished the last frame handed to it (fromVk at `finished`) and the one producer queue has signalled
    // the next frame's produced value (toVk at `nextProduced`), which it does only after running what it had of the
    // frames before: FreeBuried, from PrepareFrame, or at once after a drain.
    struct Buried
    {
        FrameBuffers buffers;
        UINT64 finished = 0, nextProduced = 0;
    };
    std::vector<Buried> buried;
    // The next frame's buffers, made on the builder thread beside a network built for a frame that had none at its
    // extent and format (builtBuffers), then kept for that frame (prepared, render thread): it takes them over instead
    // of spending the time on the render thread (MakeGeometry).
    std::unique_ptr<FrameBuffers> builtBuffers, prepared;

    uint64_t lastFrameId = 0;
    ULONGLONG lastFrameTick = 0;
    std::atomic<bool> resetPending { true };
    std::atomic<uint64_t> frames { 0 };

    // A game queue idle within 30 s (kDrainWaitMs at process exit): S_OK, HRESULT_FROM_WIN32(ERROR_TIMEOUT) or what
    // failed.
    HRESULT DrainD3D12(ID3D12CommandQueue* q)
    {
        ID3D12Fence* f = nullptr;
        HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
        if (FAILED(hr))
            return hr;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ev)
        {
            f->Release();
            return E_OUTOFMEMORY;
        }
        hr = q->Signal(f, 1);
        if (SUCCEEDED(hr))
            hr = f->SetEventOnCompletion(1, ev);
        if (SUCCEEDED(hr) && WaitForSingleObject(ev, atExit ? kDrainWaitMs : 30000) != WAIT_OBJECT_0)
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        CloseHandle(ev);
        f->Release();
        return hr;
    }

    // Game queues, AddRef'd while it lives.
    struct Queues
    {
        std::vector<ID3D12CommandQueue*> list;

        Queues() = default;
        Queues(const Queues&) = delete;
        Queues& operator=(const Queues&) = delete;
        ~Queues()
        {
            for (ID3D12CommandQueue* q : list)
                q->Release();
        }
    };

    // The session's queue and every queue a frame came from since the last drain, each idle within 30 s: S_OK or
    // the first failure. `queues` gets them all. Never throws (~Session calls it).
    HRESULT DrainGameQueues(Queues& queues)
    {
        {
            std::lock_guard lock(submitMutex);
            try
            {
                queues.list.reserve(producers.size() + 1);
            }
            catch (const std::bad_alloc&)
            {
                return E_OUTOFMEMORY;
            }
            queues.list.push_back(queue);
            for (const Producer& p : producers)
                queues.list.push_back(p.queue);
            for (ID3D12CommandQueue* q : queues.list)
                q->AddRef();
        }
        for (ID3D12CommandQueue* q : queues.list)
            if (const HRESULT hr = DrainD3D12(q); FAILED(hr))
                return hr;
        return S_OK;
    }

    // `queues` were drained: nothing of this session's is left on them. Under submitMutex.
    void ForgetProducers(const Queues& queues)
    {
        size_t kept = 0;
        for (size_t i = 0; i < producers.size(); ++i)
        {
            if (std::find(queues.list.begin(), queues.list.end(), producers[i].queue) != queues.list.end())
                producers[i].Release();
            else
                producers[kept++] = producers[i];
        }
        if (kept != producers.size())
            lastProducerListed = false;
        producers.resize(kept);
    }

    // Both sides idle, so sized resources and the network can go: every game queue a frame came from, then the
    // Vulkan side within kDrainWaitMs a wait (VulkanIdle), whose timeout fails the call only. A game queue that does
    // not drain within 30 s is taken as lost, as before errors were classified: otherwise the render thread would wait
    // on it again every frame.
    void Drain()
    {
        Queues queues;
        if (const HRESULT hr = DrainGameQueues(queues); FAILED(hr))
        {
            char text[96];
            std::snprintf(text, sizeof text, "drain of the game queue failed, HRESULT 0x%08lX",
                          static_cast<unsigned long>(hr));
            throw NrError(hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) ? NrError::DeviceLost : KindOf(hr), text, VK_SUCCESS,
                          hr);
        }
        if (vk.device)
        {
            std::lock_guard lock(submitMutex);
            if (const VkResult r = VulkanIdle(kDrainWaitMs); r != VK_SUCCESS)
            {
                char text[128];
                std::snprintf(text, sizeof text, "drain of the Vulkan side failed: %s (VkResult %d)",
                              r == VK_TIMEOUT ? "its work did not finish within 2 s" : "a wait failed", int(r));
                throw NrError(KindOf(r), text, r);
            }
            // Every slot's fence is now signalled or unused; the next submit on a slot needs it unsignalled.
            VkCheck(vkResetFences(vk.device, kSlots, done), "vkResetFences");
            VkCheck(vkResetFences(vk.device, kFallbacks, fallbackDone), "vkResetFences (passthrough)");
            for (bool& f : inFlight)
                f = false;
            for (bool& f : fallbackInFlight)
                f = false;
            ForgetProducers(queues);
        }
        // A job retired after the orphaned one means the game has run its lists past it; this drain then saw the
        // orphaned list's copies finish, if the game ran it at all.
        std::vector<ID3D12Resource*> released;
        {
            std::lock_guard lock(jobMutex);
            if (orphaned && retiredGen > orphanGen)
                orphaned = false;
            if (!graveyard.empty() && retiredGen > graveyardGen)
                released.swap(graveyard);
        }
        for (ID3D12Resource* r : released)
            r->Release();
        // Both sides idle: every retired set of the frame's buffers can go too.
        FreeBuried(true);
    }

    void FreeGraveyard()
    {
        for (ID3D12Resource* r : graveyard)
            r->Release();
        graveyard.clear();
    }

    // The retired sets of the frame's buffers the GPU is past (Buried); all of them when `all` (both sides idle).
    void FreeBuried(bool all)
    {
        if (buried.empty())
            return;
        UINT64 produced = UINT64_MAX, finished = UINT64_MAX;
        if (!all)
        {
            // Only while the frames still come from one queue: once another one signals toVk, a value there says
            // nothing of the first one's work (a drain frees them then). Left for a later frame while a build holds
            // submitMutex (a build without a queue of its own).
            std::unique_lock lock(submitMutex, std::try_to_lock);
            if (!lock || producers.size() > 1)
                return;
            produced = toVk.fence->GetCompletedValue();
            // fromVk read through Vulkan: the same value, and it also tells a validation layer that the submits which
            // signalled it are done (it cannot see the D3D12 fence read).
            if (vkGetSemaphoreCounterValue(vk.device, fromVk.semaphore, &finished) != VK_SUCCESS)
                return;
        }
        auto gone = [&](Buried& b)
        {
            if (produced < b.nextProduced || finished < b.finished)
                return false;
            b.buffers.Release(vk.device);
            return true;
        };
        buried.erase(std::remove_if(buried.begin(), buried.end(), gone), buried.end());
    }

    void DestroyFences()
    {
        for (VkFence& f : done)
            if (f)
                vkDestroyFence(vk.device, std::exchange(f, VkFence {}), nullptr);
        for (VkFence& f : fallbackDone)
            if (f)
                vkDestroyFence(vk.device, std::exchange(f, VkFence {}), nullptr);
    }

    // Every submit of this session's that may still run, done within `ms`: its slots' and passthroughs' fences.
    VkResult WaitInFlight(uint32_t ms)
    {
        VkFence pending[kSlots + kFallbacks] {};
        uint32_t n = 0;
        for (uint32_t i = 0; i < kSlots; ++i)
            if (inFlight[i])
                pending[n++] = done[i];
        for (uint32_t i = 0; i < kFallbacks; ++i)
            if (fallbackInFlight[i])
                pending[n++] = fallbackDone[i];
        return n ? vkWaitForFences(vk.device, n, pending, VK_TRUE, uint64_t(ms) * 1'000'000) : VK_SUCCESS;
    }

    // Every frame handed to the Vulkan side done within `ms`: fromVk at the last one's finished value, which its submit
    // signals once its work has run. VK_TIMEOUT when it is not done by then. Under submitMutex.
    VkResult FramesDone(uint32_t ms)
    {
        const UINT64 last = lastFinished.load();
        if (!last)
            return VK_SUCCESS;
        VkSemaphoreWaitInfo wi { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores = &fromVk.semaphore;
        wi.pValues = &last;
        return vkWaitSemaphores(vk.device, &wi, uint64_t(ms) * 1'000'000);
    }

    // The Vulkan side idle, each wait within `ms`: fromVk at the last frame a game queue waits for, then every
    // submit's fence. VK_TIMEOUT when either is not done by then. Under submitMutex, or once nothing else runs.
    VkResult VulkanIdle(uint32_t ms)
    {
        if (const VkResult r = FramesDone(ms); r != VK_SUCCESS)
            return r;
        return WaitInFlight(ms);
    }

    // fromVk at `value`, waited for off the render thread in steps of kWatchMs, with no lock (vkWaitSemaphores
    // synchronises nothing of the session's): VK_SUCCESS once it is there, VK_TIMEOUT once stop() says to give up,
    // VK_ERROR_DEVICE_LOST once the device is lost (Lose puts fromVk at its maximum while the GPU may still use
    // everything, so a lost device is never a success), or the wait's own error.
    template <class Stop> VkResult WaitFinished(UINT64 value, Stop&& stop)
    {
        VkSemaphoreWaitInfo wi { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores = &fromVk.semaphore;
        wi.pValues = &value;
        for (;;)
        {
            if (deviceLost)
                return VK_ERROR_DEVICE_LOST;
            const VkResult r = vkWaitSemaphores(vk.device, &wi, uint64_t(kWatchMs) * 1'000'000);
            if (deviceLost)
                return VK_ERROR_DEVICE_LOST;
            if (r != VK_TIMEOUT)
                return r;
            if (stop())
                return VK_TIMEOUT;
        }
    }

    // `network`, which the frames up to `finished` in fromVk may still use, handed to the releaser (Releaser); without
    // memory for that it leaks rather than be freed under the GPU. Any thread.
    void RetireNetwork(std::unique_ptr<ProductRuntime> network, UINT64 finished) noexcept
    {
        try
        {
            std::lock_guard lock(releaseMutex);
            retired.reserve(retired.size() + 1); // the one step that may throw, before the network moves
            retired.push_back({ std::move(network), finished });
            ++releasing;
        }
        catch (...)
        {
            (void) network.release();
            return;
        }
        releaseWake.notify_all();
    }

    // The releaser thread (render thread only: ReleaseLater, ReleasesDone), unless it runs already.
    void StartReleaser() noexcept
    {
        if (releaser.joinable())
            return;
        try
        {
            releaser = std::thread([this] { Releaser(); });
        }
        catch (const std::exception& e)
        {
            nr::logf("[mochizuki] the releaser thread could not start (%s): a network the frames no longer use goes "
                     "when the session ends",
                     e.what());
        }
    }

    // A network the frames no longer use goes on the releaser thread, once the Vulkan side has finished the frames
    // that used it: neither that wait nor the release (about 30 ms at 1080p) is the render thread's. Render thread.
    void ReleaseLater(std::unique_ptr<ProductRuntime> network, UINT64 finished) noexcept
    {
        RetireNetwork(std::move(network), finished);
        StartReleaser();
    }

    // The releaser thread: each retired network, oldest first, freed once fromVk has reached its finished value. It
    // ends at ~Session (releaseStop), or when a wait fails or the device is lost; what it has not freed then stays in
    // `retired` for ~Session.
    void Releaser()
    {
        std::unique_lock lock(releaseMutex);
        for (;;)
        {
            releaseWake.wait(lock, [this] { return releaseStop || !retired.empty(); });
            if (releaseStop)
                break;
            const UINT64 finished = retired.front().finished;
            lock.unlock();
            const VkResult r = WaitFinished(finished, [this] { return releaseStop.load(); });
            lock.lock();
            if (r != VK_SUCCESS)
            {
                if (r != VK_TIMEOUT)
                    nr::logf("[mochizuki] a network the frames no longer use is left to the session's end: %s "
                             "(VkResult %d)",
                             r == VK_ERROR_DEVICE_LOST ? "the device is lost" : "the wait for the Vulkan side failed",
                             int(r));
                break;
            }
            std::unique_ptr<ProductRuntime> network = std::move(retired.front().network);
            retired.erase(retired.begin());
            lock.unlock();
            const auto t0 = std::chrono::steady_clock::now();
            network.reset();
            nr::logf("[mochizuki] the old network released in %.1f ms",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            lock.lock();
            --releasing;
            releaseWake.notify_all();
        }
        releaserEnded = true;
        releaseWake.notify_all();
    }

    // Retired networks not freed yet. Render thread.
    bool Releasing()
    {
        std::lock_guard lock(releaseMutex);
        return releasing != 0;
    }

    // Every retired network freed, within `ms` (or none left to the releaser): after a drain, whose Vulkan side is
    // idle, before the VRAM check looks again. Render thread.
    void ReleasesDone(uint32_t ms)
    {
        if (Releasing())
            StartReleaser();
        std::unique_lock lock(releaseMutex);
        releaseWake.wait_for(lock, std::chrono::milliseconds(ms),
                             [this] { return !releasing || releaserEnded || !releaser.joinable(); });
    }

    // The releaser ended; `retired` is ~Session's.
    void StopReleaser()
    {
        {
            std::lock_guard lock(releaseMutex);
            releaseStop = true;
        }
        releaseWake.notify_all();
        if (releaser.joinable())
            releaser.join();
    }

    // The Vulkan side is lost, or stopped finishing frames (the watchdog): the session fails for good, and fromVk goes
    // to its maximum from the CPU, which frees every game queue's wait on it, queued or still to come. The flags and
    // the error come first, so that whoever sees a game queue freed also sees the session failed, and why. The frames
    // it frees get whatever the output buffer holds. A Vulkan submit still queued then may later signal fromVk lower
    // (VUID-VkSubmitInfo-pSignalSemaphores-03242 forbids it, but a queued submit cannot be taken back): Submit makes
    // no new one, and the watchdog and ~Session put fromVk back (Rerelease). Any thread; the first call records and
    // logs.
    void Lose(const char* why)
    {
        if (deviceLost.exchange(true))
        {
            failed = true;
            return;
        }
        failed = true;
        RememberError(why);
        const HRESULT hr = fromVk.fence ? fromVk.fence->Signal(UINT64_MAX) : S_OK;
        released = true;
        nr::logf("[mochizuki] %s; the session is stopped%s", why,
                 !fromVk.fence ? ""
                 : FAILED(hr)  ? " (releasing the game's queues failed)"
                               : " and the game's queues are released");
    }

    // After the release, fromVk back at its maximum when a Vulkan submit queued before it has since signalled a lower
    // value (the D3D12 fence does fall: RX 9070 XT, driver 26.8.1, WORK/S3/fix1). A game queue's wait queued before
    // the release stays satisfied, but one queued after the fall (an Enqueue that raced Lose) would wait for a value
    // the Vulkan side may never reach. The watchdog every kWatchMs, then ~Session once more; they never run together.
    void Rerelease()
    {
        if (!fromVk.fence)
            return;
        const UINT64 value = fromVk.fence->GetCompletedValue();
        if (value == UINT64_MAX)
            return;
        const HRESULT hr = fromVk.fence->Signal(UINT64_MAX);
        const uint32_t n = ++rereleases;
        if (n <= kErrorsLogged || n % 1000 == 0)
            nr::logf("[mochizuki] fromVk fell back to %llu after the release (a Vulkan submit queued before it): it "
                     "is signalled to its maximum again%s (%u times)",
                     static_cast<unsigned long long>(value), FAILED(hr) ? ", which failed" : "", n);
    }

    // Under submitMutex, before the producer's signal. A frame from another queue than the last frame's (by COM
    // identity) first waits there for the last frame's finished value, so its signal cannot land before the last
    // producer's (toVk would go backwards and meet a Vulkan wait early) and the Vulkan side takes the frames in order.
    // A signal no Vulkan submit took (its frame failed after it and was not passed through) has no finished value to
    // wait for: the new queue then also waits for it in toVk. It depends only on work queued earlier. The new queue's
    // first half has already copied its colour, which may race the last frame's network: both frames may look wrong,
    // and the history restarts on the next frame. The queue is also listed for Drain.
    HRESULT Follow(ID3D12CommandQueue* producer)
    {
        if (producer == lastProducer.queue && lastProducerListed)
            return S_OK;
        Producer p = Producer::Of(producer);
        const UINT64 last = lastFinished.load(std::memory_order_relaxed);
        const UINT64 stray = lastSignalled > lastProduced.load(std::memory_order_relaxed) ? lastSignalled : 0;
        if (lastProducer.id && p.id != lastProducer.id && (last || stray))
        {
            HRESULT hr = last ? producer->Wait(fromVk.fence, last) : S_OK;
            if (SUCCEEDED(hr) && stray)
                hr = producer->Wait(toVk.fence, stray);
            if (FAILED(hr))
            {
                p.Release();
                return hr;
            }
            resetPending = true;
            // Logged like NoteError: a game may switch every frame.
            const uint64_t n = ++queueSwitches;
            if (n <= kErrorsLogged || n % 1000 == 0)
                nr::logf("[mochizuki] a frame from queue %p after one from queue %p (switch %llu of the session%s): it "
                         "waits there for the last frame; this frame may look wrong, and the history restarts",
                         static_cast<void*>(producer), static_cast<void*>(lastProducer.queue),
                         static_cast<unsigned long long>(n),
                         n == kErrorsLogged ? "; from now on one in 1000 is logged" : "");
        }
        if (std::none_of(producers.begin(), producers.end(), [&](const Producer& q) { return q.id == p.id; }))
            producers.push_back(p.Copy());
        lastProducer.Release();
        lastProducer = p;
        lastProducerListed = true;
        return S_OK;
    }

    // A frame is on the Vulkan queue: fromVk will reach its finished value whether or not the game queue's wait for it
    // goes through. The watchdog, Drain and Follow learn of it. Under submitMutex.
    void Submitted(UINT64 produced, UINT64 finished)
    {
        awaited.Add(produced, finished);
        lastProduced.store(produced, std::memory_order_relaxed);
        lastFinished.store(finished, std::memory_order_release);
    }

    // A frame's Vulkan submit: c after the frame's produced value in toVk, then its finished value into fromVk; then
    // Submitted. None once the session is lost (Lose), whose release a later signal would undo.
    VkResult Submit(VkCommandBuffer c, UINT64 produced, UINT64 finished, VkFence signal)
    {
        if (deviceLost)
            return VK_ERROR_DEVICE_LOST;
        VkTimelineSemaphoreSubmitInfo values { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        values.waitSemaphoreValueCount = 1;
        values.pWaitSemaphoreValues = &produced;
        values.signalSemaphoreValueCount = 1;
        values.pSignalSemaphoreValues = &finished;
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO, &values };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &toVk.semaphore;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &c;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &fromVk.semaphore;
        gpuUsed.store(true, std::memory_order_relaxed);
        const VkResult r = vkQueueSubmit(vk.queue, 1, &si, signal);
        if (r == VK_SUCCESS)
            Submitted(produced, finished);
        return r;
    }

    // The watchdog, from the first Enqueue (under submitMutex). Without it (no thread) only the slot waits see a
    // stuck Vulkan side.
    void StartWatchdog()
    {
        if (watchdogStarted)
            return;
        watchdogStarted = true;
        try
        {
            watchdog = std::thread([this] { Watch(); });
        }
        catch (const std::exception& e)
        {
            nr::logf("[mochizuki] the watchdog thread could not start: %s", e.what());
        }
    }

    void StopWatchdog()
    {
        if (!watchdog.joinable())
            return;
        {
            std::lock_guard lock(watchMutex);
            watchStop = true;
        }
        watchWake.notify_all();
        watchdog.join();
    }

    // Every kWatchMs, fromVk's value. A lost device, or the oldest frame handed to the Vulkan side (Awaited) not
    // finished for kStallMs of reads in a row although the game produced it (toVk), means the Vulkan side will not
    // finish it: Lose frees the game's queues. Counted in reads, not in wall time, so a system sleep cannot fake a
    // stall. Once released, it keeps fromVk at its maximum (Rerelease) until ~Session. It never takes submitMutex,
    // which a build without a queue of its own holds for seconds.
    void Watch()
    {
        constexpr uint32_t kStallReads = kStallMs / kWatchMs + 1; // kStallMs between the first and the last
        UINT64 stalledOn = 0;                                     // the stalled frame's finished value
        uint32_t stalledReads = 0;
        std::unique_lock lock(watchMutex);
        while (!watchWake.wait_for(lock, std::chrono::milliseconds(kWatchMs), [this] { return watchStop; }))
        {
            if (released)
            {
                Rerelease();
                continue;
            }
            if (deviceLost)
                continue; // Lose is on its way to the release
            UINT64 reached = 0;
            const VkResult r = vkGetSemaphoreCounterValue(vk.device, fromVk.semaphore, &reached);
            if (r == VK_ERROR_DEVICE_LOST)
            {
                Lose("watchdog: the Vulkan device was lost");
                continue;
            }
            // The frame the Vulkan side has to finish next, if the game has produced it.
            Awaited::Frame oldest;
            const bool waiting = r == VK_SUCCESS && awaited.Oldest(reached, oldest);
            const UINT64 produced = waiting ? toVk.fence->GetCompletedValue() : 0;
            if (!waiting || produced < oldest.produced)
            {
                stalledReads = 0;
                continue;
            }
            if (oldest.finished != stalledOn)
            {
                stalledOn = oldest.finished;
                stalledReads = 0;
            }
            if (++stalledReads < kStallReads)
                continue;
            char text[256];
            std::snprintf(text, sizeof text,
                          "watchdog: the Vulkan side has not finished a frame the game produced %u s ago (fromVk at "
                          "%llu, the frame's value %llu; toVk at %llu, the last frame's value %llu): the device is "
                          "taken to be lost",
                          kStallMs / 1000, static_cast<unsigned long long>(reached),
                          static_cast<unsigned long long>(oldest.finished), static_cast<unsigned long long>(produced),
                          static_cast<unsigned long long>(lastProduced.load()));
            Lose(text);
        }
    }

    // A session error that leaves the session usable: kept for GetInfo and logged, the first kErrorsLogged in full
    // and then one in 1000, since it may come back every frame.
    void NoteError(const char* text)
    {
        RememberError(text);
        const uint32_t n = ++errors;
        if (n < kErrorsLogged)
            nr::logf("[mochizuki] %s", text);
        else if (n == kErrorsLogged || n % 1000 == 0)
            nr::logf("[mochizuki] %s (error %u of the session; from now on one in 1000 is logged)", text, n);
    }

    // A Start that threw has submitted nothing: free what it made, so that a later PrepareSession can try again.
    void AbandonStart()
    {
        if (vk.device)
        {
            DestroyFences();
            if (pool)
                vkDestroyCommandPool(vk.device, pool, nullptr); // frees commands[] and fallbackCommands[] with it
        }
        std::fill(std::begin(commands), std::end(commands), VkCommandBuffer {});
        std::fill(std::begin(fallbackCommands), std::end(fallbackCommands), VkCommandBuffer {});
        pool = {};
        toVk.Release(vk.device);
        fromVk.Release(vk.device);
        vk.Destroy();
        vk = {};
        vram.Close();
    }

    // Median and p95 of the network's GPU time over the last kGpuSamples frames; zero before the first sample.
    void NetworkMs(float& median, float& p95)
    {
        float sorted[kGpuSamples];
        uint32_t n = 0;
        {
            std::lock_guard lock(statsMutex);
            n = gpuMsCount;
            std::copy_n(gpuMs, n, sorted);
        }
        median = p95 = 0;
        if (!n)
            return;
        std::sort(sorted, sorted + n);
        median = n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
        p95 = sorted[(n * 95 + 99) / 100 - 1];
    }

    // The last error, for GetInfo: GetLastError's is the calling thread's.
    void RememberError(const char* text)
    {
        std::lock_guard lock(errorMutex);
        std::strncpy(lastError, text ? text : "", sizeof lastError - 1);
        lastError[sizeof lastError - 1] = 0;
    }

    // The network's time as GetStatus showed it, plus the core's running average, which only the log keeps.
    void LogNetworkTime(const char* when)
    {
        float median = 0, p95 = 0;
        NetworkMs(median, p95);
        nr::logf("[mochizuki] %s: network %ux%u %.2f ms (p95 %.2f, average %.2f), %llu frames in the session", when,
                 net.width, net.height, median, p95, runtime->average_gpu_ms(),
                 static_cast<unsigned long long>(frames.load()));
    }

    // An orphaned job's list may still copy into the geometry's D3D12 buffers, whenever the game runs it: they go to
    // the graveyard as well, AddRef'd, whatever happens to the geometry's own references.
    void KeepOrphaned()
    {
        std::lock_guard lock(jobMutex);
        if (!orphaned)
            return;
        for (ID3D12Resource* r : { input.resource, motionBuffer.resource, output.resource, result })
            if (r)
            {
                r->AddRef();
                graveyard.push_back(r);
            }
        graveyardGen = orphanGen;
        orphaned = false;
    }

    // The geometry's buffers after the frame changed, without waiting for the game's queue: with one producer queue
    // since the last drain, into `buried` until the GPU is past them (FreeBuried); with several, whose order among
    // each other is not known, after a drain, as before. Render thread.
    void RetireGeometry()
    {
        size_t queues = 0;
        UINT64 next = 0;
        {
            std::lock_guard lock(submitMutex);
            queues = producers.size();
            next = producedValue + 1;
        }
        if (!geometry.width || queues > 1)
        {
            if (geometry.width)
                Drain();
            ReleaseGeometry();
            return;
        }
        buried.reserve(buried.size() + 1); // the one step that may throw, before anything moves
        KeepOrphaned();
        buried.push_back({ TakeBuffers(), lastFinished.load(), next });
    }

    // Everything the geometry holds; nothing the GPU may still use (the callers drain first, or never used it). An
    // orphaned job's list may still copy into the D3D12 side, which is buried instead.
    void ReleaseGeometry()
    {
        KeepOrphaned();
        TakeBuffers().Release(vk.device);
    }

    // The geometry's buffers, taken from the frames: no geometry is left.
    FrameBuffers TakeBuffers()
    {
        FrameBuffers b;
        b.geometry = std::exchange(geometry, {});
        b.colourFootprint = colourFootprint;
        b.motionFootprint = motionFootprint;
        b.depthFootprint = depthFootprint;
        b.colourVk = colourVk;
        b.motionVk = motionVk;
        b.depthVk = depthVk;
        b.input = std::exchange(input, {});
        b.output = std::exchange(output, {});
        b.motionBuffer = std::exchange(motionBuffer, {});
        b.depthBuffer = std::exchange(depthBuffer, {});
        b.colourImage = std::exchange(colourImage, {});
        b.netImage = std::exchange(netImage, {});
        b.motionImage = std::exchange(motionImage, {});
        b.depthImage = std::exchange(depthImage, {});
        b.motionScaled = std::exchange(motionScaled, {});
        b.result = std::exchange(result, nullptr);
        b.firstFootprint = firstFootprint;
        b.firstOutput = std::exchange(firstOutput, {});
        b.firstResult = std::exchange(firstResult, nullptr);
        std::fill(std::begin(fallbackCurrent), std::end(fallbackCurrent), false);
        return b;
    }

    // The estimated VRAM cost of the network for `key` (MZ_TEST_NETWORK_MB replaces it).
    UINT64 NetworkEstimate(const NetworkKey& key) const
    {
        if (hooks.networkMb)
            return hooks.networkMb << 20;
        // The model extent as the core pads it, for the activations.
        auto model = [&](UINT v)
        {
            const UINT scaled = key.scale >= 1.f ? v : std::max(8u, UINT(double(v) * key.scale + 0.5));
            return UINT64((scaled + 63) / 64 * 64);
        };
        // The RenoDX stage's network runs one pass at a time at the stage's extent, a history for each pass.
        const double passes = PassMemoryFactor(key.renodx ? 1u : key.maxPasses);
        const UINT64 width = key.renodx ? key.renoWidth : key.width, height = key.renodx ? key.renoHeight : key.height;
        const UINT64 activations = UINT64(double(kModelPixelBytes * model(UINT(width)) * model(UINT(height))) * passes);
        const UINT64 scaled = key.scale < 1.f ? kScaledFramePixelBytes * key.width * key.height : 0;
        // The stage's own images, after TheAutomatic/dlss-5-amd-project's bound: its codec and look intermediates at
        // the frame's extent, and the proxy, the answer and a history per pass at the network's.
        const UINT64 reno =
            key.renodx ? UINT64(key.width) * key.height * 128 + width * height * (64 + 48 * key.maxPasses) : 0;
        const UINT64 first = key.firstPass ? UINT64(16) * key.width * key.height : 0; // the core's first_full
        // An FP16 network's twins (RDNA3, RDNA2): twice the weight arena, and twice the activation arena per model
        // pixel (1080p, MZ_FORCE_RDNA3 on an RX 9070 XT: the process grew by 1877 MB against 799).
        const UINT64 twins = vk.Fp16() ? (336ull << 20) + 384 * model(UINT(width)) * model(UINT(height)) : 0;
        return kNetworkFixedBytes + activations + scaled + reno + first + twins;
    }

    // What the network for `key` costs in VRAM: the estimate, or what building or releasing it measured when that was
    // more.
    UINT64 NetworkBytes(const NetworkKey& key) const
    {
        const UINT64 estimate = NetworkEstimate(key);
        return measuredKey == key ? std::max(estimate, measuredBytes) : estimate;
    }

    // The frame's buffers for g; a bucketed geometry with motion adds its bucket-sized motion image.
    static UINT64 FrameBytes(const Geometry& g)
    {
        const UINT64 scaled = g.bucketed ? MotionFormat(g.motionFormat).bytes : 0;
        const UINT64 first = g.firstPass ? 32 : 0; // its buffer and texture, RGBA32F
        const UINT64 net = g.netFormat ? 8 : 0;     // netImage, RGBA16F
        return (kFramePixelBytes + scaled + first + net + 2 * DepthFormat(g.depthFormat).bytes) * g.width * g.height;
    }

    // A network measured: `bytes` is what it took, or gave back, in this process's VRAM.
    void Measured(const NetworkKey& key, UINT64 bytes)
    {
        measuredBytes = measuredKey == key ? std::max(measuredBytes, bytes) : bytes;
        measuredKey = key;
    }

    // The VRAM check before a network build or the frame's buffers (`what`): this process's use plus `need` must stay
    // within kVramBudgetShare of its budget. Says why not in `why`; lets it through when the budget is unknown. `seen`
    // gets the use it saw, 0 when unknown.
    bool VramAdmits(UINT64 need, UINT w, UINT h, const char* what, std::string& why, UINT64* seen = nullptr)
    {
        UINT64 usage = 0, budget = 0;
        const bool known = vram.Read(usage, budget);
        if (seen)
            *seen = known ? usage : 0;
        if (hooks.vramBudgetMb)
            budget = hooks.vramBudgetMb << 20;
        else if (!known)
        {
            static std::atomic<bool> logged { false };
            if (!logged.exchange(true))
                nr::logf("[mochizuki] the VRAM budget is unknown (D3DKMTQueryVideoMemoryInfo failed or reported no "
                         "budget): no VRAM check");
            return true;
        }
        const UINT64 allowed = UINT64(double(budget) * kVramBudgetShare);
        const UINT64 available = allowed > usage ? allowed - usage : 0;
        auto mb = [](UINT64 bytes) { return static_cast<unsigned long long>(bytes >> 20); };
        if (need <= available)
        {
            nr::logf(
                "[mochizuki] VRAM for %s at %ux%u: about %llu MB needed, %llu MB available (this process uses %llu "
                "MB of a %llu MB budget, %.0f%% of it usable)",
                what, w, h, mb(need), mb(available), mb(usage), mb(budget), kVramBudgetShare * 100);
            return true;
        }
        char text[160];
        std::snprintf(text, sizeof text, "insufficient VRAM for %ux%u (need %llu MB, free %llu MB of a %llu MB budget)",
                      w, h, mb(need), mb(available), mb(budget));
        why = text;
        return false;
    }

    // Passthrough k's copy, input to output, for the current geometry. Under submitMutex (it shares the pool), once
    // k's last submit is done: Passthrough records it when k is first used after the geometry changed, since a
    // passthrough of a frame before the change may run until then (no drain precedes a new geometry).
    void RecordFallback(uint32_t k)
    {
        VkCommandBuffer c = fallbackCommands[k];
        const VkBufferCopy region { 0, 0, colourFootprint.bytes };
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkCheck(vkResetCommandBuffer(c, 0), "vkResetCommandBuffer (passthrough)");
        VkCheck(vkBeginCommandBuffer(c, &bi), "vkBeginCommandBuffer (passthrough)");
        vkCmdCopyBuffer(c, input.buffer, output.buffer, 1, &region);
        VkCheck(vkEndCommandBuffer(c), "vkEndCommandBuffer (passthrough)");
        fallbackCurrent[k] = true;
    }

    // The frame's buffers for g, and Super Resolution's texture as `desc`, into b; on a throw b holds what was made.
    // Any thread: the device calls are free-threaded, and nothing the frames use is touched.
    void MakeBuffers(FrameBuffers& b, const Geometry& g, const D3D12_RESOURCE_DESC& desc)
    {
        const Format cf = ColourFormat(g.colourFormat), mf = MotionFormat(g.motionFormat);
        b.geometry = g;
        b.resultDesc = desc;
        b.colourFootprint = Footprint(g.colourFormat, cf.bytes, g.width, g.height);
        b.colourVk = cf.vk;
        b.input.Create(device, vk, b.colourFootprint.bytes);
        b.output.Create(device, vk, b.colourFootprint.bytes);
        b.colourImage.Create(vk, cf.vk, g.width, g.height);
        if (g.netFormat)
            b.netImage.Create(vk, g.netFormat, g.width, g.height);
        b.motionVk = mf.vk;
        if (mf.vk)
        {
            b.motionFootprint = Footprint(g.motionFormat, mf.bytes, g.motionWidth, g.motionHeight);
            b.motionBuffer.Create(device, vk, b.motionFootprint.bytes);
            b.motionImage.Create(vk, mf.vk, g.motionWidth, g.motionHeight);
            if (g.bucketed)
                b.motionScaled.Create(vk, mf.vk, g.width, g.height);
        }
        const Format df = DepthFormat(g.depthFormat);
        b.depthVk = df.vk;
        if (df.vk)
        {
            // The depth plane's copy format, as D3D12 lays it out in a buffer.
            D3D12_RESOURCE_DESC dd {};
            dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            dd.Width = g.width;
            dd.Height = g.height;
            dd.DepthOrArraySize = 1;
            dd.MipLevels = 1;
            dd.Format = g.depthFormat;
            dd.SampleDesc = { 1, 0 };
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT plane {};
            device->GetCopyableFootprints(&dd, 0, 1, 0, &plane, nullptr, nullptr, nullptr);
            b.depthFootprint = Footprint(plane.Footprint.Format, df.bytes, g.width, g.height);
            b.depthBuffer.Create(device, vk, b.depthFootprint.bytes);
            b.depthImage.Create(vk, df.vk, g.width, g.height);
        }
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                              IID_PPV_ARGS(&b.result)),
              "output texture");
        if (g.firstPass)
        {
            b.firstFootprint = Footprint(DXGI_FORMAT_R32G32B32A32_FLOAT, 16, g.width, g.height);
            b.firstOutput.Create(device, vk, b.firstFootprint.bytes);
            D3D12_RESOURCE_DESC fd = desc;
            fd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &fd,
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                  IID_PPV_ARGS(&b.firstResult)),
                  "first pass texture");
        }
    }

    // The frames take b over (there is no geometry now); its passthrough is recorded when first used. Render thread.
    void UseBuffers(FrameBuffers& b)
    {
        colourFootprint = b.colourFootprint;
        motionFootprint = b.motionFootprint;
        depthFootprint = b.depthFootprint;
        colourVk = b.colourVk;
        motionVk = b.motionVk;
        depthVk = b.depthVk;
        input = std::exchange(b.input, {});
        output = std::exchange(b.output, {});
        motionBuffer = std::exchange(b.motionBuffer, {});
        depthBuffer = std::exchange(b.depthBuffer, {});
        colourImage = std::exchange(b.colourImage, {});
        netImage = std::exchange(b.netImage, {});
        motionImage = std::exchange(b.motionImage, {});
        depthImage = std::exchange(b.depthImage, {});
        motionScaled = std::exchange(b.motionScaled, {});
        result = std::exchange(b.result, nullptr);
        firstFootprint = b.firstFootprint;
        firstOutput = std::exchange(b.firstOutput, {});
        firstResult = std::exchange(b.firstResult, nullptr);
        geometry = b.geometry;
    }

    void CreateGeometry(const Geometry& g, ID3D12Resource* colour)
    {
        FrameBuffers b;
        try
        {
            MakeBuffers(b, g, ResultDesc(colour, g));
        }
        catch (...)
        {
            b.Release(vk.device);
            throw;
        }
        UseBuffers(b);
    }

    // ACO's network, as upstream's [Network] ACO Mode: dlssnr-amd\aco\ holds the Linux network's SPIR-V (shaders\)
    // and ACO's machine code for each pipeline (records\); the driver compiles our own shaders only as templates for
    // the code's container, and the imports are kept in dlssnr-amd\aco-cache\. Once a process, before the first
    // Vulkan device: the core reads it then. Without the folder it stays off and the driver compiles the network.
    static void ConfigureAco()
    {
        static std::once_flag once;
        std::call_once(once,
                       []
                       {
                           const std::string data = RootUtf8(DataDirectory()) + "\\dlssnr-amd";
                           const auto has = [](const std::string& b)
                           {
                               return GetFileAttributesA((b + "\\records").c_str()) != INVALID_FILE_ATTRIBUTES &&
                                      GetFileAttributesA((b + "\\shaders").c_str()) != INVALID_FILE_ATTRIBUTES;
                           };
                           std::string bundle = data + "\\aco";
                           // Without RDNA4's bundle an FP16 network's own one is enough: Rdna2Aco / Rdna3Aco take it
                           // once the device says which network runs (an RDNA4 device then has none: Vulkan::Create).
                           if (!has(bundle))
                               for (const char* own : { "\\aco-gfx103x", "\\aco-gfx11" })
                                   if (has(data + own))
                                   {
                                       bundle = data + own;
                                       break;
                                   }
                           if (!has(bundle))
                           {
                               nr::logf("[mochizuki] ACO asked for, but %s has no records\\ and shaders\\: the "
                                        "driver compiles the network",
                                        bundle.c_str());
                               return;
                           }
                           // "<bundle name> <native name>" a line: a kernel our shaders name otherwise.
                           std::map<std::string, std::string> aliases;
                           if (FILE* f = std::fopen((bundle + "\\shell-aliases.txt").c_str(), "r"))
                           {
                               char a[128], b[128];
                               while (std::fscanf(f, "%127s %127s", a, b) == 2)
                                   aliases[a] = b;
                               std::fclose(f);
                           }
                           const std::string cache = data + "\\aco-cache";
                           CreateDirectoryA(cache.c_str(), nullptr);
                           std::vector<std::string> shells { bundle + "\\shell", data + "\\shaders",
                                                             data + "\\shaders\\temporal" };
                           nr::binary::configure(bundle, std::move(shells), cache, std::move(aliases));
                           if (bundle == data + "\\aco")
                               nr::logf("[mochizuki] ACO: the network runs ACO's machine code from %s", bundle.c_str());
                           else
                               nr::logf("[mochizuki] ACO: no dlssnr-amd\\aco; %s, for the network it is made for only",
                                        bundle.c_str());
                       });
    }

    void Start()
    {
        if (aco)
            ConfigureAco();
        vk.Create(device->GetAdapterLuid(), hooks);
        toVk.Create(device, vk, "toVk");
        fromVk.Create(device, vk, "fromVk");
        VkCommandPoolCreateInfo pi { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.queueFamilyIndex = vk.family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCheck(vkCreateCommandPool(vk.device, &pi, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = kSlots;
        VkCheck(vkAllocateCommandBuffers(vk.device, &ai, commands), "vkAllocateCommandBuffers");
        ai.commandBufferCount = kFallbacks;
        VkCheck(vkAllocateCommandBuffers(vk.device, &ai, fallbackCommands), "vkAllocateCommandBuffers (passthrough)");
        VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        for (VkFence& f : done)
            VkCheck(vkCreateFence(vk.device, &fi, nullptr, &f), "vkCreateFence");
        for (VkFence& f : fallbackDone)
            VkCheck(vkCreateFence(vk.device, &fi, nullptr, &f), "vkCreateFence (passthrough)");
        vram.Open(device->GetAdapterLuid());
        nr::logf("[mochizuki] Vulkan device %s, queue family %u%s", vk.name.c_str(), vk.family,
                 vk.buildQueue != vk.queue ? ", a second queue for the network builds" : "");
        if (DataDirectory() != DllDirectory())
            nr::logf("[mochizuki] loaded from %s; dlssnr-amd is read from %s", RootUtf8(DllDirectory()).c_str(),
                     RootUtf8(DataDirectory()).c_str());
    }

    // A build ended without a network. Its key is not built again, except after running out of memory (kOomRetryMs).
    // The network the frames kept during the build goes (on the releaser thread), and the frame's buffers with it: a
    // retry then has the VRAM to itself, and meanwhile no frame runs, as when nothing was kept. A build that lost the
    // device fails the session like any other lost device.
    void BuildFailed()
    {
        if (buildErrorKind == NrError::DeviceLost)
            throw NrError(NrError::DeviceLost, buildError);
        const ULONGLONG delay = buildHold.Failed(buildKey, buildErrorKind, buildError);
        RememberError(buildError);
        if (delay)
            nr::logf("[mochizuki] out of memory: the network at %ux%u is built again in %llu s", buildKey.width,
                     buildKey.height, static_cast<unsigned long long>(delay / 1000));
        else
            nr::logf("[mochizuki] the network at %ux%u is not built again until the frame's extent, format or settings "
                     "change",
                     buildKey.width, buildKey.height);
        if (runtime)
        {
            std::unique_ptr<ProductRuntime> old;
            UINT64 finished = 0;
            {
                std::lock_guard lock(submitMutex);
                old = TakeNetwork("the network the frames kept goes with the failed build", finished);
            }
            ReleaseLater(std::move(old), finished);
        }
        RetireGeometry();
    }

    // The frames' network, taken from them: no frame records with it after, and `finished` gets the finished value of
    // the last frame handed to the Vulkan side, the last one that may use it. It may go once fromVk is there, which the
    // render thread does not wait for (ReleaseLater, or the builder). `why` goes to the log with the network's time.
    // Under submitMutex.
    std::unique_ptr<ProductRuntime> TakeNetwork(const char* why, UINT64& finished)
    {
        LogNetworkTime(why);
        finished = lastFinished.load();
        return std::move(runtime);
    }

    // The runtime serves a frame of `key`'s extent and colour format: all a frame needs of it while a build for the
    // new model scale, pass count or colour encoding runs (keep serving).
    bool Serves(const NetworkKey& key) const
    {
        return runtime && net.width == key.width && net.height == key.height && net.format == key.format;
    }

    // A finished build's network (built, for buildKey) becomes the frames'. The one they kept meanwhile is taken in the
    // same hold of submitMutex (TakeNetwork), so no frame records between the two, and goes on the releaser thread once
    // the Vulkan side has finished the frames that used it (ReleaseLater): the render thread waits for neither. The
    // history restarts.
    void Install()
    {
        std::unique_ptr<ProductRuntime> old;
        UINT64 oldFinished = 0;
        {
            std::lock_guard lock(submitMutex);
            if (runtime)
                old = TakeNetwork("replacing the network", oldFinished);
            runtime = std::move(built);
            net = buildKey;
            resetPending = true;
            historyBits = 0;
            historyCount = 0;
        }
        // The buffers made beside it wait for the next frame (MakeGeometry).
        if (prepared)
            prepared->Release(vk.device);
        prepared = std::move(builtBuffers);
        const char* const kept = old ? "; the frames ran on the old one meanwhile" : "";
        if (old)
            ReleaseLater(std::move(old), oldFinished);
        buildHold = {};
        {
            std::lock_guard stats(statsMutex);
            gpuMsCount = gpuMsNext = 0;
        }
        infoFrameWidth = net.width;
        infoFrameHeight = net.height;
        infoModelWidth = runtime->model_width();
        infoModelHeight = runtime->model_height();
        infoMaxPasses = net.maxPasses;
        infoReno = runtime->reno != nullptr;
        infoExperimental = runtime->experimental();
        infoOptimized = runtime->optimized() ? uint32_t(1 + runtime->layers()) : 0u;
        infoBuildSeconds = float(buildSeconds);
        if (runtime->model_width() != net.width || runtime->model_height() != net.height || net.maxPasses > 1)
            nr::logf("[mochizuki] network ready at %ux%u (model %ux%u, up to %u passes) in %.1f s%s", net.width,
                     net.height, runtime->model_width(), runtime->model_height(), net.maxPasses, buildSeconds, kept);
        else
            nr::logf("[mochizuki] network ready at %ux%u in %.1f s%s", net.width, net.height, buildSeconds, kept);
        if (buildGrowth)
        {
            Measured(net, buildGrowth);
            nr::logf("[mochizuki] VRAM: the network at %ux%u was estimated at %llu MB; this process grew by %llu MB "
                     "while it was built",
                     net.width, net.height, static_cast<unsigned long long>(NetworkEstimate(net) >> 20),
                     static_cast<unsigned long long>(buildGrowth >> 20));
        }
    }

    // True once a network for `key` is ready, or while one for the frame's extent and colour format serves it during
    // the build for new settings (keep serving: the frame runs at the old ones). Otherwise starts its build, unless one
    // is running, the last one for this key failed (buildHold) or the VRAM check refuses it; `why` then says why there
    // is none. g is the frame's geometry, `colour` its colour: buffers made for another one are retired before a build,
    // and the build makes the new ones beside the network.
    bool EnsureNetwork(const NetworkKey& key, const Geometry& g, ID3D12Resource* colour, std::string& why)
    {
        static const char* const kBuilding = "building the network (the first time takes about half a minute)";
        bool finished = false;
        {
            std::lock_guard lock(buildMutex);
            if (buildDone)
            {
                builder.join();
                buildDone = building = false;
                infoBuilding = false;
                finished = true;
            }
            if (building)
            {
                if (Serves(key))
                    return true;
                why = kBuilding;
                return false;
            }
        }
        if (finished && !built)
            BuildFailed();
        {
            std::lock_guard lock(buildMutex);
            if (!fp16Off.empty())
            {
                why = fp16Off;
                return false;
            }
        }
        if (built)
            Install();
        if (runtime && net == key)
            return true;
        if (buildHold.Holds(key, GetTickCount64()))
        {
            why = buildHold.message;
            return false;
        }
        // With a queue of their own for the builds, the frames keep a network for their extent and format meanwhile.
        const bool serve = Serves(key) && vk.buildQueue != vk.queue;
        // Buffers for another frame go now, unless the frames keep running on them.
        if (!serve && geometry.width && !(geometry == g))
            RetireGeometry();
        // The network, and the frame's buffers unless they are made already, beside what the frames still hold.
        const UINT64 need = NetworkBytes(key) + (geometry == g ? 0 : FrameBytes(g)) + kVramMarginBytes;
        bool admitted = VramAdmits(need, key.width, key.height, "the network", why, &buildBaseUsage);
        if (!admitted && (!buried.empty() || Releasing()))
        {
            // Not beside them: what was retired already goes first, after a drain of both sides (the drain lets the
            // releaser free the networks), then the check again.
            Drain();
            ReleasesDone(kDrainWaitMs);
            admitted = VramAdmits(need, key.width, key.height, "the network", why, &buildBaseUsage);
        }
        const bool freeGeometry = geometry.width && !(geometry == g);
        if (!admitted && (runtime || freeGeometry))
        {
            // Then what the frames hold, but only when freeing it would make room: the game takes back what is
            // freed, so a network given up for one that is then refused leaves nothing, not even the way back (OpenGL
            // games). Refused here, the running network keeps serving and a revert to it is immediate.
            const UINT64 reclaim = (runtime ? NetworkBytes(net) : 0) + (freeGeometry ? FrameBytes(geometry) : 0);
            std::string notEnough;
            if (VramAdmits(need > reclaim ? need - reclaim : 0, key.width, key.height,
                           "the network once the running one is freed", notEnough))
            {
                Drain();
                if (runtime)
                {
                    std::lock_guard lock(submitMutex);
                    LogNetworkTime("replacing the network");
                    runtime.reset();
                }
                if (geometry.width && !(geometry == g))
                    ReleaseGeometry();
                ReleasesDone(kDrainWaitMs);
                admitted = VramAdmits(need, key.width, key.height, "the network", why, &buildBaseUsage);
            }
            else
                nr::logf("[mochizuki] %s, the running network's %llu MB included: it keeps serving", notEnough.c_str(),
                         static_cast<unsigned long long>(reclaim >> 20));
        }
        if (!admitted)
        {
            buildHold.Refused(key, why.c_str());
            NoteError(why.c_str());
            return false;
        }
        const bool keep = serve && runtime;
        // The first ready frame's buffers, when the frame has none, are made beside the network (the frames that keep
        // the old one make their own at once).
        std::unique_ptr<FrameBuffers> buffers;
        if (!keep && !(geometry == g))
        {
            buffers = std::make_unique<FrameBuffers>();
            buffers->geometry = g;
            buffers->resultDesc = ResultDesc(colour, g);
        }
        // A network the frames do not keep goes on the builder thread before the build, once the Vulkan side has
        // finished the frames that used it: the render thread waits for neither that nor its release.
        std::unique_ptr<ProductRuntime> old;
        UINT64 oldFinished = 0;
        if (runtime && !keep)
        {
            std::lock_guard lock(submitMutex);
            old = TakeNetwork("replacing the network", oldFinished);
        }
        buildKey = key;
        StartBuild(std::move(old), oldFinished, std::move(buffers), keep);
        why = kBuilding;
        return keep;
    }

    // The build for buildKey, on the builder thread (Build). `old`, a network no frame uses any more, is freed there
    // first, once fromVk has reached `oldFinished`; `buffers`, when set, asks for the next frame's buffers too
    // (geometry and resultDesc). `keep`: the frames keep their network meanwhile. On a throw, `old` goes to the
    // releaser (ReleaseLater), never here.
    void StartBuild(std::unique_ptr<ProductRuntime> old, UINT64 oldFinished, std::unique_ptr<FrameBuffers> buffers,
                    bool keep)
    {
        if (prepared)
        {
            prepared->Release(vk.device); // made for a frame that did not come
            prepared.reset();
        }
        bool started = false;
        try
        {
            const NetworkKey& key = buildKey;
            nr::RuntimeConfig config;
            config.root = RootUtf8(DataDirectory());
            config.width = key.width;
            config.height = key.height;
            config.colour_format = key.format;
            config.linear_input = key.linear;
            config.model_scale = key.scale;
            config.max_passes = key.maxPasses;
            config.preprocess = key.prep;
            config.arch = vk.arch;
            // The new model goes back to the 310.8 one by itself when dlssnr-small.bin is missing, and logs it.
            config.network = key.original ? 0 : 1;
            if (key.experimental && !vk.Fp16() && MakeExperimentalWeights())
                config.network = 2;
            // int4 mixed's successor (upstream 0.0.8.3): CC_SILVER_AARDWOLD_OPTIMIZED at its Optimized layers level; its
            // weights are made on the builder thread (Build), and the core falls back by itself without them. The FP16
            // networks have neither.
            if (key.int4 && !vk.Fp16())
            {
                config.network = 3;
                config.layers = int(key.layers);
            }
            config.cascade_detail_only = key.cascade;
            config.first_pass_output = key.firstPass;
            config.history_f16 = key.hist16;
            // The linear path's frame is a proxy made with the soft knee, which the preprocess undoes first.
            config.preprocess_unknee = key.linear;
            // The game's vectors at the nearest texel, as NVIDIA's DLL binds them (upstream 0.0.7, its NGX route).
            config.motion_point = true;
            // A network built while the frames keep the current one runs in that one's activation arena when it has
            // the room (upstream 0.0.7's shared activation memory): the two never run in one frame.
            if (keep && runtime)
                config.act_arena = runtime->act_arena();
            // The RenoDX stage hands the network its own RGBA16F proxy at the stage's extent, one pass at a time, and
            // takes the answer as it is (native compose): RenoDX encodes, preprocesses and composes.
            if (key.renodx)
            {
                config.width = key.renoWidth;
                config.height = key.renoHeight;
                config.colour_format = VK_FORMAT_R16G16B16A16_SFLOAT;
                config.linear_input = false;
                config.model_scale = 1.f;
                config.max_passes = 1;
                config.native_compose = true;
                config.preprocess = false;
                config.cascade_detail_only = false;
                config.preprocess_unknee = false;
            }
            nr::TemporalConfig temporal;
            temporal.enable = true;
            nr::HostDevice host;
            host.instance = vk.instance;
            host.physical = vk.physical;
            host.device = vk.device;
            host.queue = vk.buildQueue;
            host.queue_family = vk.family;
            host.buffer_device_address = vk.bufferDeviceAddress;
            char options[96] = {};
            if (key.scale != 1.f || key.maxPasses > 1)
                std::snprintf(options, sizeof options, ", model scale %.2f, up to %u passes%s", key.scale,
                              key.maxPasses, key.cascade ? " (later passes add detail only)" : "");
            if (key.renodx)
                std::snprintf(options, sizeof options, ", in the RenoDX stage at %ux%u, %u pass%s", key.renoWidth,
                              key.renoHeight, key.maxPasses, key.maxPasses == 1 ? "" : "es");
            nr::logf("[mochizuki] building %s at %ux%u, VkFormat %d%s%s%s",
                     nr::network_label(key.int4 ? nr::network_with_layers(nr::kSilverAardwoldOptimized, int(key.layers))
                                       : key.original     ? nr::kSilverAardwold
                                       : key.experimental ? nr::kSmallFp8Experimental
                                                          : nr::kSmallFp8),
                     key.width, key.height, int(key.format), key.linear ? ", linear colour" : "", options,
                     keep ? "; the frames keep the current one meanwhile" : "");
            UseUtf8Paths();
            {
                std::lock_guard lock(buildMutex);
                building = true;
            }
            started = true;
            // The core moves the stage on from 2 and leaves it at 5 after a build: this one starts at planning.
            nr::g_build_pipes_done = 0;
            nr::g_build_pipes_total = 0;
            nr::g_build_stage = 1;
            infoBuilding = true;
            gpuUsed = true;
            // The builder takes `old` over only once its thread exists: a failed start must not free it here.
            ProductRuntime* const handed = old.get();
            builder = std::thread(
                [this, host, config, temporal, key, handed, oldFinished, buffers = std::move(buffers)]() mutable
                {
                    Build(host, config, temporal, key, std::unique_ptr<ProductRuntime>(handed), oldFinished,
                          std::move(buffers));
                });
            (void) old.release();
        }
        catch (...)
        {
            if (started)
            {
                {
                    std::lock_guard lock(buildMutex);
                    building = false;
                }
                infoBuilding = false;
            }
            if (old)
                ReleaseLater(std::move(old), oldFinished);
            throw;
        }
    }

    // The builder thread. Nothing may throw out of it, and nothing from the network's end to buildDone: a fixed
    // buffer, no allocation. First the network the frames no longer use (`old`), once the Vulkan side has finished the
    // frames that used it (fromVk at oldFinished), before the new one takes its VRAM; Destroy or a lost device leaves
    // it to the releaser instead. The build waits for any other in the process (CoreBuildMutex) and stops between two
    // pipelines once `abandon` is set (Destroy; P3's prewarm and the core's pipeline loop check it). Then the next
    // frame's buffers, when asked for: making them takes the render thread longer than a frame. When Destroy has given
    // the session up to this thread (Abandon), it ends it.
    // dlssnr-amd\build.pending while a network build runs. Still there when the next one starts, or renamed
    // build.failed by the ReShade add-on (mochizuki.inc, which then runs danielblnc instead), it means that build
    // never finished: the game crashed or was closed during it. The next build logs every pipeline it compiles. A
    // build that ends, built or failed, takes both away.
    struct BuildMarker
    {
        std::wstring pending = DataDirectory() + L"\\dlssnr-amd\\build.pending";
        std::wstring failed = DataDirectory() + L"\\dlssnr-amd\\build.failed";
        bool lastDied = GetFileAttributesW(pending.c_str()) != INVALID_FILE_ATTRIBUTES ||
                        GetFileAttributesW(failed.c_str()) != INVALID_FILE_ATTRIBUTES;

        BuildMarker()
        {
            if (FILE* f = _wfopen(pending.c_str(), L"wb"))
                std::fclose(f);
        }
        ~BuildMarker()
        {
            DeleteFileW(pending.c_str());
            DeleteFileW(failed.c_str());
        }
        BuildMarker(const BuildMarker&) = delete;
        BuildMarker& operator=(const BuildMarker&) = delete;
    };

    // Before the first FP16 network build (RDNA3, RDNA2) on a driver, on the builder thread and before it takes the
    // build's locks: every pipeline the network needs compiled in a child process (mzr3::PipelineCheck). One AMD's
    // compiler crashes on, hangs on or refuses keeps the network off for the session (fp16Off) and for these shaders on
    // this driver (rdna3-check.txt, rdna2-check.txt).
    void Fp16Pipelines()
    {
        if (Fp16CheckSkips(vk.arch, false))
            return;
        const char* tag = nr::arch_name(vk.arch);
        const std::wstring file = Fp16CheckFile(vk.arch);
        std::string key = mzr3::Key(vk.deviceId, vk.driverVersion), why;
        const std::vector<std::wstring> files = Fp16PipelineFiles(vk.arch, key);
        if (!mzr3::Cached(file, key, "pipelines", why))
        {
            nr::logf("[mochizuki] %s pipeline check: %zu pipelines, each compiled once in a child process first", tag,
                     files.size());
            bool ran = false;
            std::string note;
            nr::g_build_stage = 6;
            const std::wstring data = DataDirectory() + L"\\dlssnr-amd", work = data + L"\\" + Fp16Tag(vk.arch) + L"-check",
                               variant = std::wstring(L"-") + Fp16Tag(vk.arch);
            why = mzr3::PipelineCheck(device->GetAdapterLuid(), files, work, &abandon, ran, note,
                                      vk.arch == nr::Arch::rdna2 ? mzr3::kPipelineTimeoutRdna2Ms
                                                                 : mzr3::kPipelineTimeoutMs,
                                      MzEnvOff("MZ_CHECK_CACHE") ? std::wstring() : data, variant);
            nr::g_build_stage = 1;
            // What the check compiled with the core's create infos, imported by the build below.
            if (ran && why.empty())
                mzi::AdoptBinaries(data, variant, work + L"\\binaries");
            if (abandon)
                throw std::runtime_error("stopped: the session is being destroyed");
            if (!ran)
            {
                nr::logf("[mochizuki] the %s pipeline check could not run (%s); the network builds without it", tag,
                         why.c_str());
                return;
            }
            nr::logf("[mochizuki] %s pipeline check %s: %s", tag, why.empty() ? "passed" : "failed", note.c_str());
            mzr3::Keep(file, key, "pipelines", why);
        }
        if (why.empty())
            return;
        // The RX 6000's INT8 shaders failed: its FP16 ones, checked in turn (the device takes both).
        if (vk.arch == nr::Arch::rdna2 && nr::rdna2_int8)
        {
            nr::logf("[mochizuki] the INT8 network failed its pipeline check (%s); the FP16 network", why.c_str());
            nr::rdna2_int8 = false;
            Fp16Pipelines();
            return;
        }
        const std::string off = std::string(tag) + " network off: " + why;
        nr::logf("[mochizuki] %s", off.c_str());
        {
            std::lock_guard lock(buildMutex);
            fp16Off = off;
        }
        RememberError(off.c_str());
        throw std::runtime_error(off);
    }

    void Build(const nr::HostDevice& host, const nr::RuntimeConfig& config, const nr::TemporalConfig& temporal,
               const NetworkKey& key, std::unique_ptr<ProductRuntime> old, UINT64 oldFinished,
               std::unique_ptr<FrameBuffers> buffers)
    {
        const auto t0 = std::chrono::steady_clock::now();
        if (old)
        {
            if (WaitFinished(oldFinished, [this] { return abandon.load(); }) == VK_SUCCESS)
            {
                old.reset();
                UINT64 usage = 0, budget = 0;
                if (buildBaseUsage && vram.Read(usage, budget))
                    buildBaseUsage = usage; // the growth is measured from after the release
            }
            else
                RetireNetwork(std::move(old), oldFinished);
        }
        std::unique_ptr<ProductRuntime> made;
        char error[sizeof buildError] {};
        NrError::Kind kind = NrError::Other;
        try
        {
            if (vk.Fp16() && !vk.rdna3Aco && !vk.rdna2Aco)
                Fp16Pipelines();
            // One core build at a time in the process, waited for in steps so that Destroy can end the wait.
            std::unique_lock core(CoreBuildMutex(), std::defer_lock);
            while (!core.try_lock_for(std::chrono::milliseconds(kWatchMs)))
                if (abandon)
                    throw std::runtime_error("stopped: the session is being destroyed");
            nr::RuntimeConfig made_config = config;
            if (config.network == 3)
            {
                bool int4 = config.layers > 0;
                if (!MakeOptimizedWeights(int4))
                    made_config.network = 0;
                else if (!int4)
                    made_config.layers = 0;
            }
            // Builds and frames on one queue: the frames wait for the build, as they always did.
            std::unique_lock lock(submitMutex, std::defer_lock);
            if (host.queue == vk.queue)
                lock.lock();
            if (hooks.buildOomOnce && !g_buildOomInjected.exchange(true))
                throw std::runtime_error("vkAllocateMemory (MZ_TEST_BUILD_OOM_ONCE test hook): VkResult=-2");
            // The pipelines an earlier build recorded are compiled on several threads first, into the pipeline.cache
            // the core then loads; the capture records this build's for the next one, and Finish saves what the core
            // compiled after its own save (mz_interpose.h).
            const BuildMarker marker;
            mzi::Capture pipelines(host.device, host.physical, DataDirectory(),
                                   vk.Fp16() ? std::wstring(L"-") + Fp16Tag(vk.arch) : std::wstring());
            if (marker.lastDied)
            {
                nr::logf("[mochizuki] the last network build in this folder never finished (the game crashed or was "
                         "closed during it); this one logs every pipeline it compiles");
                pipelines.Verbose();
            }
            // ACO's network imports its pipelines (dlssnr-amd\aco-cache keeps them): the native list is not for it.
            if (nr::binary::directory().empty() || !nr::binary::enabled(host.device))
                pipelines.Prewarm(&abandon);
            if (abandon)
                throw std::runtime_error("stopped: the session is being destroyed");
            {
                const nr::BuildCancelScope cancel(&abandon);
                try
                {
                    made = std::make_unique<ProductRuntime>(host, made_config, temporal, key, vk);
                }
                catch (const nr::binary::Error& e)
                {
                    // ACO's machine code itself failed (a record, its template, the driver refusing the import, a
                    // user-data layout the adapter does not know): ACO off for the process and the network built
                    // again by the driver's compiler, as upstream's host does. Out of memory and a lost device are
                    // not Error and stay this build's failure.
                    if (abandon)
                        throw;
                    nr::binary::disable(std::string("the ACO network did not build (") + e.what() + ")");
                    nr::logf("[mochizuki] ACO off: %s; building with the driver's compiler",
                             nr::binary::disabled_why().c_str());
                    made = std::make_unique<ProductRuntime>(host, made_config, temporal, key, vk);
                }
            }
            pipelines.Finish();
        }
        catch (const std::exception& e)
        {
            kind = KindOf(e);
            std::snprintf(error, sizeof error, "network build failed at %ux%u: %s", config.width, config.height,
                          e.what());
        }
        catch (...)
        {
            std::snprintf(error, sizeof error, "network build failed at %ux%u: an unknown exception", config.width,
                          config.height);
        }
        if (error[0] && abandon)
            nr::logf("[mochizuki] the network build at %ux%u stopped after %.1f s: the session is being destroyed",
                     config.width, config.height,
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        else if (error[0])
            nr::logf("[mochizuki] %s (%s)", error, KindName(kind));
        // The network's own growth, before the buffers add theirs.
        UINT64 usage = 0, budget = 0;
        const UINT64 grown =
            made && buildBaseUsage && vram.Read(usage, budget) && usage > buildBaseUsage ? usage - buildBaseUsage : 0;
        if (buffers && made && !abandon)
        {
            try
            {
                // The VRAM check the frame makes for them (MakeGeometry), now that the network holds its share. When
                // it refuses, the frame's own check does too, and the network goes (ReleaseNetwork).
                const Geometry& g = buffers->geometry;
                std::string why;
                if (VramAdmits(FrameBytes(g) + kVramMarginBytes, g.width, g.height, "the frame's buffers", why))
                    MakeBuffers(*buffers, g, buffers->resultDesc);
                else
                    buffers.reset();
            }
            catch (const std::exception& e)
            {
                buffers->Release(vk.device);
                buffers.reset();
                nr::logf("[mochizuki] the frame's buffers were not made beside the network (%s); the frame makes them",
                         e.what());
            }
            catch (...)
            {
                buffers->Release(vk.device);
                buffers.reset();
            }
        }
        else if (buffers)
            buffers.reset();
        std::unique_lock lock(buildMutex);
        built = std::move(made);
        builtBuffers = std::move(buffers);
        buildGrowth = grown;
        std::memcpy(buildError, error, sizeof error);
        buildErrorKind = kind;
        buildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        buildDone = true;
        buildEnded.notify_all();
        if (!builderOwnsSession)
            return;
        lock.unlock();
        EndAbandoned(this);
    }

    // Destroy during a network build: the build is told to stop (between two pipelines) and given kAbandonWaitMs to
    // end. True when it has, or none runs: the caller ends the session, and joins the builder. Otherwise the builder
    // thread is detached and owns the session, which it ends once the build returns (EndAbandoned), and this module
    // is pinned, since that thread runs its code after Destroy returned and the host may unload it. The thread then
    // touches nothing of the host's but the device and queues the session holds references to. Until the build
    // returns it holds CoreBuildMutex: a new session's build waits for it (its frames say "building" meanwhile).
    bool Abandon()
    {
        std::unique_lock lock(buildMutex);
        if (!building || buildDone)
            return true;
        abandon = true;
        abandonedAt = std::chrono::steady_clock::now();
        if (buildEnded.wait_for(lock, std::chrono::milliseconds(kAbandonWaitMs), [this] { return buildDone; }))
            return true;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&LmxxfNrGetApi), &self);
        builder.detach();
        builderOwnsSession = true;
        nr::logf("[mochizuki] Destroy during the network build: it did not stop within %u s; its thread ends the "
                 "session once it returns",
                 kAbandonWaitMs / 1000);
        return false;
    }

    // The builder thread of a session Destroy gave up waiting for (Abandon), once the build returned.
    static void EndAbandoned(Session* s)
    {
        const double late = std::chrono::duration<double>(std::chrono::steady_clock::now() - s->abandonedAt).count();
        delete s;
        nr::logf("[mochizuki] the session Destroy left to the network build ended on the build's thread, %.1f s after "
                 "Destroy",
                 late);
    }

    // The builder, once its build ended. Only ~Session calls it; EnsureNetwork joins under buildMutex once buildDone
    // is set. A detached builder (Abandon) is not joinable: this runs on it.
    void JoinBuilder()
    {
        std::unique_lock lock(buildMutex);
        if (!builder.joinable())
            return;
        buildEnded.wait(lock, [this] { return buildDone; });
        builder.join();
    }

    // The network goes because the VRAM check refused the frame's buffers beside it (MakeGeometry): without them it
    // would only hold VRAM. What that gives back is its measured cost, and its key is held like a refused build, so it
    // is built again only once the check admits it together with the buffers (NetworkBytes), not every kVramRetryMs.
    void ReleaseNetwork(const std::string& why)
    {
        Drain();
        UINT64 before = 0, after = 0, budget = 0;
        const bool known = vram.Read(before, budget);
        {
            std::lock_guard lock(submitMutex);
            runtime.reset();
        }
        const UINT64 freed = known && vram.Read(after, budget) && before > after ? before - after : 0;
        if (freed)
            Measured(net, freed);
        buildHold.Refused(net, why.c_str());
        nr::logf("[mochizuki] the network at %ux%u is released (%llu MB given back): the frame's buffers do not fit "
                 "beside it; it is built again once both fit (checked every %llu s)",
                 net.width, net.height, static_cast<unsigned long long>(freed >> 20),
                 static_cast<unsigned long long>(kVramRetryMs / 1000));
    }

    // The frame's buffers for g, unless the last try for g failed (geometryHold) or the VRAM check refuses them;
    // `why` then says why. A refusal also releases the network (ReleaseNetwork). A lost device throws. Buffers made
    // for g beside the network (prepared) are taken over: the builder made them only after this same VRAM check.
    bool MakeGeometry(const Geometry& g, ID3D12Resource* colour, std::string& why)
    {
        std::unique_ptr<FrameBuffers> made = std::move(prepared);
        if (made && !(made->geometry == g && SameDesc(made->resultDesc, ResultDesc(colour, g))))
        {
            made->Release(vk.device); // no GPU work has used them
            made.reset();
        }
        if (!made && geometryHold.Holds(g, GetTickCount64()))
        {
            why = geometryHold.message;
            return false;
        }
        if (!made && !VramAdmits(FrameBytes(g) + kVramMarginBytes, g.width, g.height, "the frame's buffers", why))
        {
            NoteError(why.c_str());
            if (runtime)
                ReleaseNetwork(why);
            else
                geometryHold.Refused(g, why.c_str());
            return false;
        }
        try
        {
            if (made)
                UseBuffers(*made);
            else
                CreateGeometry(g, colour);
        }
        catch (const std::exception& e)
        {
            const NrError::Kind kind = KindOf(e);
            if (kind == NrError::DeviceLost)
                throw;
            ReleaseGeometry(); // what was made of it
            char text[384];
            std::snprintf(text, sizeof text, "the frame's buffers at %ux%u could not be made (%s): %s", g.width,
                          g.height, KindName(kind), e.what());
            // In Cyberpunk 2077 the driver refused a shared buffer's handle (E_INVALIDARG) for a while, then took it:
            // an invalid argument is tried again too.
            const ULONGLONG delay = geometryHold.Failed(g, kind, text, true);
            NoteError(text);
            if (delay)
                nr::logf("[mochizuki] the frame's buffers are made again in %llu s",
                         static_cast<unsigned long long>(delay / 1000));
            why = text;
            return false;
        }
        geometryHold = {};
        return true;
    }

    // The blit a bucket makes on the motion vectors' format f: LINEAR from one image of it into another (the vectors
    // rescaled onto the subrect). The padding copies, which every format takes. Render thread.
    bool Blittable(VkFormat f)
    {
        auto known =
            std::find_if(formatFeatures.begin(), formatFeatures.end(), [f](const auto& p) { return p.first == f; });
        if (known == formatFeatures.end())
        {
            VkFormatProperties p {};
            vkGetPhysicalDeviceFormatProperties(vk.physical, f, &p);
            formatFeatures.emplace_back(f, p.optimalTilingFeatures);
            known = formatFeatures.end() - 1;
        }
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        return (known->second & need) == need;
    }

    // The extent the network and the frame's buffers are built for (W x H), for a frame whose render subrect is w x h
    // in the colour allocation `desc`, under MochizukiNrControls::drs_mode `mode`; `bucketed` when it is the bucket.
    // Exact (0), or motion vectors the bucket cannot blit: the subrect. Auto (1): the subrect too until one smaller
    // than this allocation comes, then the bucket. Always (2): the bucket for every frame. The bucket is, per axis, the
    // largest subrect seen rounded up to kBucketAlign, within the allocation (always: the largest allocation seen), so
    // a subrect equal to it runs as in exact. It only grows, except after kBucketShrinkMs of bucketed frames of one
    // allocation, all kBucketShrinkMargin inside it on both axes: it then shrinks to what they needed. Render thread.
    void DrsExtent(uint32_t mode, UINT w, UINT h, const D3D12_RESOURCE_DESC& desc, bool blittable, UINT& W, UINT& H,
                   bool& bucketed)
    {
        W = w;
        H = h;
        bucketed = false;
        if (!mode || !blittable)
        {
            if (mode && !drsRefusedLogged)
                nr::logf("[mochizuki] dynamic resolution: the motion vectors' format cannot be blitted; the network "
                         "is built for the render subrect (as in exact)");
            drsRefusedLogged |= mode != 0;
            bucket = {};
            return;
        }
        Bucket& b = bucket;
        const UINT allocWidth = UINT(desc.Width), allocHeight = desc.Height;
        if (b.allocWidth != desc.Width || b.allocHeight != desc.Height || b.allocFormat != desc.Format)
        {
            b.allocWidth = desc.Width;
            b.allocHeight = desc.Height;
            b.allocFormat = desc.Format;
            b.engaged = false;
            b.lowSince = 0; // the shrink counts only this allocation's frames
        }
        // Auto: a colour allocation of the same format and aspect at another size is a dynamic resolution step, as a
        // smaller subrect is. Another format or aspect (a new window size, say) starts over.
        if (mode == 1)
        {
            const bool sameKind =
                b.bigFormat == desc.Format && b.bigHeight && allocHeight &&
                std::abs(double(allocWidth) / allocHeight - double(b.bigWidth) / b.bigHeight) < .02;
            if (!sameKind)
            {
                b.bigWidth = allocWidth;
                b.bigHeight = allocHeight;
                b.bigFormat = desc.Format;
                b.reallocated = false;
                b.maxWidth = b.maxHeight = 0;
                b.motionWidth = b.motionHeight = 0;
            }
            else if (allocWidth != b.bigWidth || allocHeight != b.bigHeight)
            {
                if (!b.reallocated)
                    nr::logf("[mochizuki] dynamic resolution (auto): the game reallocates its colour (%ux%u, then "
                             "%ux%u); each size is a step in one bucket",
                             b.bigWidth, b.bigHeight, allocWidth, allocHeight);
                b.reallocated = true;
                b.bigWidth = std::max(b.bigWidth, allocWidth);
                b.bigHeight = std::max(b.bigHeight, allocHeight);
            }
        }
        b.maxWidth = std::max(b.maxWidth, w);
        b.maxHeight = std::max(b.maxHeight, h);
        b.capWidth = std::max(b.capWidth, allocWidth);
        b.capHeight = std::max(b.capHeight, allocHeight);
        b.engaged |= mode == 2 || (mode == 1 && b.reallocated) || w < allocWidth || h < allocHeight;
        if (!b.engaged)
        {
            b.lowSince = 0; // a whole frame is not inside the bucket
            return;
        }
        // What the bucket may grow to: this allocation, or the largest of its kind once it is reallocated (auto), or
        // the largest seen (always).
        const auto limit = [&](UINT cap, UINT big, UINT alloc)
        { return mode == 2 ? cap : b.reallocated ? big : alloc; };
        const auto extent = [&](UINT seen, UINT most)
        { return std::min(most, (seen + kBucketAlign - 1) / kBucketAlign * kBucketAlign); };
        b.limitWidth = limit(b.capWidth, b.bigWidth, allocWidth);
        b.limitHeight = limit(b.capHeight, b.bigHeight, allocHeight);
        W = extent(b.maxWidth, b.limitWidth);
        H = extent(b.maxHeight, b.limitHeight);
        const ULONGLONG now = GetTickCount64();
        if (w + kBucketShrinkMargin > W || h + kBucketShrinkMargin > H)
            b.lowSince = 0;
        else if (!b.lowSince)
        {
            b.lowSince = now;
            b.lowWidth = w;
            b.lowHeight = h;
        }
        else
        {
            b.lowWidth = std::max(b.lowWidth, w);
            b.lowHeight = std::max(b.lowHeight, h);
            if (now - b.lowSince >= kBucketShrinkMs)
            {
                b.maxWidth = b.lowWidth;
                b.maxHeight = b.lowHeight;
                b.capWidth = allocWidth; // the only allocation since lowSince
                b.capHeight = allocHeight;
                if (b.reallocated)
                {
                    b.bigWidth = allocWidth;
                    b.bigHeight = allocHeight;
                }
                b.motionWidth = b.motionHeight = 0;
                b.lowSince = 0;
                b.limitWidth = limit(b.capWidth, b.bigWidth, allocWidth);
                b.limitHeight = limit(b.capHeight, b.bigHeight, allocHeight);
                W = extent(b.maxWidth, b.limitWidth);
                H = extent(b.maxHeight, b.limitHeight);
                nr::logf("[mochizuki] dynamic resolution: the render subrect stayed at most %ux%u for %llu s; the "
                         "bucket shrinks",
                         b.maxWidth, b.maxHeight, static_cast<unsigned long long>(kBucketShrinkMs / 1000));
            }
        }
        bucketed = true;
        if (W != b.loggedWidth || H != b.loggedHeight)
        {
            nr::logf("[mochizuki] dynamic resolution (%s): the network runs at %ux%u for render subrects up to that in "
                     "the %ux%u colour allocation (this frame %ux%u)",
                     mode == 2 ? "always" : "auto", W, H, allocWidth, allocHeight, w, h);
            b.loggedWidth = W;
            b.loggedHeight = H;
        }
    }

    // What a frame has put on the queues so far, for the passthrough to carry on from.
    struct Submission
    {
        UINT64 produced = 0, finished = 0; // the frame's values in toVk and fromVk, around its Vulkan submit
        bool followed = false;             // Follow ran for the producer
        bool signalled = false;            // the producer's Signal(toVk, produced) is on the game queue
        bool submitted = false;            // the network's vkQueueSubmit went through
    };

    // One frame, between the halves of the game's list, from any game queue: the network, or its colour passed
    // through (Passthrough) when anything before the network's submit fails and zeroOutputFallback is set; `why` then
    // says what failed. False for a frame passed through. Throws a lost device, and any error it cannot pass the frame
    // through for.
    bool Enqueue(ID3D12CommandQueue* producer, std::string& why)
    {
        std::lock_guard lock(submitMutex);
        const uint64_t call = ++enqueueCalls;
        StartWatchdog();
        Submission sub;
        try
        {
            RunNetwork(producer, call, sub);
            return true;
        }
        catch (const std::exception& e)
        {
            const NrError::Kind kind = KindOf(e);
            if (!zeroOutputFallback || !input.buffer || kind == NrError::DeviceLost || sub.submitted)
                throw;
            Passthrough(producer, sub);
            char text[384];
            std::snprintf(text, sizeof text,
                          "the network failed on this frame (%s: %s); its colour was passed through (%llu frames so "
                          "far)",
                          KindName(kind), e.what(), static_cast<unsigned long long>(passthroughs));
            why = text;
            return false;
        }
    }

    // The frame's colour as the output: input copied to output on the Vulkan queue, in the network's place between
    // the frame's values (an ExecuteCommandLists from here would re-enter the game's detoured one). Follows and
    // signals `produced` for the producer if the network did not get to it. A failure here leaves the game's queue
    // without the frame's wait, and counts as a lost device; only the copy's recording, the first time the command
    // buffer is used for this geometry, fails before anything of the passthrough is queued, and fails the frame as one
    // without a passthrough. The history restarts on the next frame.
    void Passthrough(ID3D12CommandQueue* producer, Submission& sub)
    {
        auto lost = [](const char* what, VkResult r, HRESULT hr)
        {
            char text[160];
            std::snprintf(text, sizeof text, "passthrough: %s failed (VkResult %d, HRESULT 0x%08lX)", what, int(r),
                          static_cast<unsigned long>(hr));
            return NrError(NrError::DeviceLost, text, r, hr);
        };
        const uint32_t k = nextFallback;
        if (fallbackInFlight[k])
        {
            VkResult r = vkWaitForFences(vk.device, 1, &fallbackDone[k], VK_TRUE, kSlotWaitNs);
            if (r == VK_SUCCESS)
                r = vkResetFences(vk.device, 1, &fallbackDone[k]);
            if (r != VK_SUCCESS)
                throw lost("the wait for an earlier passthrough", r, S_OK);
            fallbackInFlight[k] = false;
        }
        if (!fallbackCurrent[k])
            RecordFallback(k);
        nextFallback = (k + 1) % kFallbacks;
        if (!sub.produced)
        {
            sub.produced = ++producedValue;
            sub.finished = ++finishedValue;
        }
        if (!sub.followed)
        {
            if (const HRESULT hr = Follow(producer); FAILED(hr))
                throw lost("the new queue's wait for the last frame", VK_SUCCESS, hr);
            sub.followed = true;
        }
        if (!sub.signalled)
        {
            if (const HRESULT hr = producer->Signal(toVk.fence, sub.produced); FAILED(hr))
                throw lost("the signal after the producer", VK_SUCCESS, hr);
            sub.signalled = true;
            lastSignalled = sub.produced;
        }
        if (const VkResult r = Submit(fallbackCommands[k], sub.produced, sub.finished, fallbackDone[k]);
            r != VK_SUCCESS)
            throw lost("vkQueueSubmit", r, S_OK);
        fallbackInFlight[k] = true;
        if (const HRESULT hr = producer->Wait(fromVk.fence, sub.finished); FAILED(hr))
            throw lost("the game queue's wait", VK_SUCCESS, hr);
        ++passthroughs;
        resetPending = true;
    }

    // The frame through the RenoDX stage (TheAutomatic/dlss-5-amd-project's MochizukiNrRuntime): RenoDX encodes the
    // frame into the network's proxy and composes each pass's answer back, the network running once per pass of its
    // stack with that pass's own controls and history (feature pass + 1). In place, in the frame's image. Whether a
    // pass used its history.
    bool RecordReno(VkCommandBuffer c, nr::EngineFrame& frame)
    {
        const nr::renodx::Settings& rs = job.reno;
        const uint32_t count = nr::renodx::stack_passes(rs);
        for (uint32_t k = count; k < runtime->renoPasses; ++k)
            runtime->release_feature(k + 1);
        runtime->renoPasses = count;
        runtime->reno->set(rs);
        nr::renodx::StageFrame sf;
        sf.colour = sf.output = frame.colour;
        sf.reset = job.reset;
        sf.after_upscale = false; // this runtime runs before Super Resolution only
        sf.motion = frame.motion;
        frame.motion_scale_x *= rs.mvec_scale_x;
        frame.motion_scale_y *= rs.mvec_scale_y;
        sf.motion_scale_x = frame.motion_scale_x;
        sf.motion_scale_y = frame.motion_scale_y;
        sf.preprocess = job.controls.preprocess;
        if (job.firstPass)
            sf.first = runtime->first.image;
        // The game's exposure, cleared into the image by this recording, so a later value cannot reach a frame already
        // recorded. Without one RenoDX meters the content itself.
        sf.create_flags_known = job.renoExposure > 0.f;
        if (sf.create_flags_known)
        {
            const VkImage image = runtime->exposure.image;
            ImageBarrier(c, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
            VkClearColorValue value {};
            value.float32[0] = job.renoExposure;
            const VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(c, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
            ImageBarrier(c, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            sf.exposure.image = image;
            sf.exposure.format = VK_FORMAT_R32_SFLOAT;
            sf.exposure.width = sf.exposure.height = 1;
            sf.exposure.usage = kImageUsage;
        }
        uint32_t calls = 0;
        bool consumed = false;
        const nr::renodx::Stage::Network network =
            [&](uint32_t pass, const nr::ColourFrame& proxy, const nr::ColourFrame& neural, bool reset)
        {
            // The game's depth is at the frame's extent, not the stage's: the history reads the vectors at the pixel.
            nr::EngineFrame e = frame;
            e.depth = {};
            e.colour = proxy;
            e.target = neural;
            e.reset = reset;
            e.feature = pass + 1;
            nr::Controls ck = job.controls;
            ck.preprocess = {};
            ck.per_pass.clear();
            ck.passes = 1;
            ck.enabled = ck.apply_model = true;
            ck.anti_halo = ck.pass3_estimate = false;
            // Passes 2..4 have their own controls once "Same as pass 1" is off; later ones take pass 4's.
            const uint32_t k = std::min(pass, 3u);
            const bool own = k && !rs.pass_follow[k - 1];
            ck.intensity = k ? rs.pass_intensity[k - 1] : rs.intensity;
            ck.style = int(own ? rs.pass_style[k - 1] : rs.style);
            ck.local_tone = own ? rs.pass_local_tone[k - 1] : rs.local_tone;
            ck.local_structure = own ? rs.pass_local_structure[k - 1] : rs.local_structure;
            ck.skin_structure = own ? rs.pass_skin_structure[k - 1] : rs.skin_structure;
            ck.automatic_mask = own ? rs.pass_auto_mask[k - 1] : rs.auto_mask;
            if (rs.skin_independent && ck.automatic_mask)
            {
                ck.local_structure = std::min(ck.local_structure, 1.f);
                if (ck.skin_structure < 0.f)
                    ck.skin_structure = 1.f;
            }
            if (frame.motion.image)
            {
                const nr::EngineResult r = runtime->record_engine(c, e, ck);
                calls += r.frame.network_dispatches;
                consumed |= r.history_consumed;
                return;
            }
            // No vectors (temporal history off, a menu, a cut): the network without a history, in place on a copy of
            // the proxy, as record_engine would write its answer.
            VkMemoryBarrier mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                                 nullptr, 0, nullptr);
            VkImageCopy region {};
            region.srcSubresource = region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.extent = { proxy.width, proxy.height, 1 };
            vkCmdCopyImage(c, proxy.image, VK_IMAGE_LAYOUT_GENERAL, neural.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                                 nullptr, 0, nullptr);
            calls += runtime->record(c, neural, ck, reset).network_dispatches;
        };
        runtime->reno->record(c, sf, network);
        dispatches.store(calls, std::memory_order_relaxed);
        return consumed;
    }

    void RunNetwork(ID3D12CommandQueue* producer, uint64_t call, Submission& sub)
    {
        if (!runtime)
            throw NrError(NrError::Other, "no network for this frame");
        if (call == hooks.deviceLostAt)
            throw NrError(NrError::DeviceLost, "MZ_TEST_DEVICE_LOST_AT: the device was lost (test hook)",
                          VK_ERROR_DEVICE_LOST);
        // The slot's previous frame must be done before its command buffer is recorded again. The slot moves on only
        // then, so a frame whose wait timed out leaves it to the next frame.
        const uint32_t slot = nextSlot;
        if (inFlight[slot] || call == hooks.slotTimeoutAt)
        {
            const VkResult waited = call == hooks.slotTimeoutAt
                                        ? VK_TIMEOUT
                                        : vkWaitForFences(vk.device, 1, &done[slot], VK_TRUE, kSlotWaitNs);
            if (waited == VK_TIMEOUT)
            {
                if (++slotTimeouts >= kSlotTimeoutsLost)
                    throw NrError(NrError::DeviceLost,
                                  "the wait for an earlier frame timed out 3 times in a row: the GPU is taken to be "
                                  "lost",
                                  VK_TIMEOUT);
                throw NrError(NrError::Timeout, "the wait for an earlier frame timed out (250 ms)", VK_TIMEOUT);
            }
            VkCheck(waited, "wait for an earlier frame");
            VkCheck(vkResetFences(vk.device, 1, &done[slot]), "vkResetFences");
            inFlight[slot] = false;
        }
        slotTimeouts = 0;
        nextSlot = (slot + 1) % kSlots;
        VkCommandBuffer c = commands[slot];
        VkCheck(vkResetCommandBuffer(c, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCheck(vkBeginCommandBuffer(c, &bi), "vkBeginCommandBuffer");

        // The frame's valid extent in the image the network runs on, W x H: the whole of it, except with dynamic
        // resolution, where the rest repeats the frame's edge (Pad).
        const uint32_t w = job.width, h = job.height, W = geometry.width, H = geometry.height;
        const bool padded = w < W || h < H;
        // Buffer to image, the valid w x h only; left in GENERAL for the network, or with `pad` for more transfers.
        auto upload =
            [&](const SharedBuffer& from, const Image& to, const Footprint& fp, uint32_t vw, uint32_t vh, bool pad)
        {
            VkBufferImageCopy region = fp.vk;
            region.imageExtent = { vw, vh, 1 };
            ImageBarrier(c, to.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(c, from.buffer, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            if (pad)
                ImageBarrier(c, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
            else
                ImageBarrier(c, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        };
        // The transfers into `image` (GENERAL) done, for the network.
        auto ready = [&](VkImage image)
        {
            ImageBarrier(c, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        };
        // The frame goes up into our image, the core reads it there (sampled, in place) and writes the answer back
        // into it.
        const Image& colour = colourImage;
        upload(input, colour, colourFootprint, w, h, padded);
        if (padded)
        {
            Pad(c, colour.image, w, h, W, H);
            ready(colour.image);
        }
        // Geometry::netFormat: the network runs on netImage, the frame converted to its format.
        if (netImage.image)
        {
            ImageBarrier(c, netImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
            Blit(c, colour.image, netImage.image, { 0, 0 }, { int32_t(W), int32_t(H) }, { 0, 0 },
                 { int32_t(W), int32_t(H) }, VK_FILTER_NEAREST);
            ready(netImage.image);
        }
        const bool motion = job.motion && motionImage.image;
        // The image the network reads the vectors from, and its extent: the uploaded one, or the bucket-sized one with
        // the vectors laid over the subrect (dynamic resolution).
        VkImage motionSource = motionImage.image;
        uint32_t motionW = job.motionWidth, motionH = job.motionHeight;
        if (motion && job.motionPath == Job::Uploaded)
            upload(motionBuffer, motionImage, motionFootprint, motionW, motionH, false);
        else if (motion && job.motionPath == Job::Padded)
        {
            // They cover the subrect: straight into the bucket-sized image.
            upload(motionBuffer, motionScaled, motionFootprint, motionW, motionH, padded);
            if (padded)
            {
                Pad(c, motionScaled.image, w, h, W, H);
                ready(motionScaled.image);
            }
            motionSource = motionScaled.image;
            motionW = W;
            motionH = H;
        }
        else if (motion)
        {
            // Another extent: uploaded, rescaled onto the subrect (LINEAR, as the network samples them) and padded. A
            // LINEAR tap may reach one texel past the valid extent when the image is larger, which gets its edge too.
            const uint32_t MW = geometry.motionWidth, MH = geometry.motionHeight;
            const bool border = motionW < MW || motionH < MH;
            upload(motionBuffer, motionImage, motionFootprint, motionW, motionH, true);
            if (border)
            {
                Pad(c, motionImage.image, motionW, motionH, std::min(motionW + 1, MW), std::min(motionH + 1, MH));
                TransferBarrier(c, motionImage.image);
            }
            ImageBarrier(c, motionScaled.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
            Blit(c, motionImage.image, motionScaled.image, { 0, 0 }, { int32_t(motionW), int32_t(motionH) }, { 0, 0 },
                 { int32_t(w), int32_t(h) }, VK_FILTER_LINEAR);
            if (padded)
            {
                TransferBarrier(c, motionScaled.image);
                Pad(c, motionScaled.image, w, h, W, H);
            }
            ready(motionScaled.image);
            motionSource = motionScaled.image;
            motionW = W;
            motionH = H;
        }
        const bool depth = motion && job.depth && depthImage.image;
        if (depth)
        {
            upload(depthBuffer, depthImage, depthFootprint, w, h, padded);
            if (padded)
            {
                Pad(c, depthImage.image, w, h, W, H);
                ready(depthImage.image);
            }
        }
        if (call == hooks.enqueueThrowAt)
            throw std::runtime_error("MZ_TEST_ENQUEUE_THROW_AT: recording the network failed (test hook)");

        nr::EngineFrame frame {};
        frame.colour.image = netImage.image ? netImage.image : colour.image;
        frame.colour.format = netImage.image ? geometry.netFormat : colourVk;
        frame.colour.width = W;
        frame.colour.height = H;
        frame.colour.usage = kImageUsage;
        frame.reset = job.reset;
        // Plain fields the core reads while recording; the runtime is used under submitMutex only.
        runtime->set_history_strength(job.history);
        runtime->set_white_point(job.white);
        runtime->set_style_grade(job.styleGrade);
        bool consumed = false;
        if (motion)
        {
            frame.motion = frame.colour;
            frame.motion.image = motionSource;
            frame.motion.format = motionVk;
            frame.motion.width = motionW;
            frame.motion.height = motionH;
            frame.motion.usage = kImageUsage;
            frame.motion_scale_x = job.motionScaleX;
            frame.motion_scale_y = job.motionScaleY;
            // The core reads the vectors at the nearest of five depth taps, so a silhouette takes the foreground's
            // and not the background's (no light rim trailing a moving character).
            if (depth)
            {
                frame.depth = frame.colour;
                frame.depth.image = depthImage.image;
                frame.depth.format = depthVk;
                frame.depth.width = W;
                frame.depth.height = H;
                frame.depth.usage = kImageUsage;
                frame.depth_inverted = job.depthInverted;
            }
        }
        if (runtime->reno)
            consumed = RecordReno(c, frame);
        else if (motion)
        {
            const nr::EngineResult r = runtime->record_engine(c, frame, job.controls);
            dispatches.store(r.frame.network_dispatches, std::memory_order_relaxed);
            consumed = r.history_consumed;
        }
        else
            dispatches.store(runtime->record(c, frame.colour, job.controls).network_dispatches,
                             std::memory_order_relaxed);
        const nr::Preprocess& prep = job.controls.preprocess;
        float prepEv = NAN;
        if (prep.active() && prep.exposure == 1)
            prepEv = runtime->preprocess_meter().first + prep.bias_ev;
        infoPrepEv.store(prepEv, std::memory_order_relaxed);
        historyBits.store((historyBits.load(std::memory_order_relaxed) << 1) | uint64_t(consumed),
                          std::memory_order_relaxed);
        historyCount.store(std::min(historyCount.load(std::memory_order_relaxed) + 1, kHistorySamples),
                           std::memory_order_relaxed);
        if (const float ms = runtime->last_gpu_ms(); ms > 0)
        {
            std::lock_guard stats(statsMutex);
            gpuMs[gpuMsNext] = ms;
            gpuMsNext = (gpuMsNext + 1) % kGpuSamples;
            gpuMsCount = std::min(gpuMsCount + 1, kGpuSamples);
        }

        if (netImage.image)
        {
            // The answer back into the frame's format, where the write-back below takes it.
            ImageBarrier(c, netImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT);
            ImageBarrier(c, colour.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            Blit(c, netImage.image, colour.image, { 0, 0 }, { int32_t(w), int32_t(h) }, { 0, 0 },
                 { int32_t(w), int32_t(h) }, VK_FILTER_NEAREST);
        }
        ImageBarrier(c, colour.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT);
        // The RenoDX stage writes the frame in place whatever apply_model says: without the model the frame goes out
        // as it came, from the upload buffer.
        if (runtime->reno && !job.controls.apply_model)
        {
            const VkBufferCopy whole { 0, 0, colourFootprint.bytes };
            vkCmdCopyBuffer(c, input.buffer, output.buffer, 1, &whole);
        }
        else
        {
            VkBufferImageCopy out = colourFootprint.vk;
            out.imageExtent = { w, h, 1 };
            vkCmdCopyImageToBuffer(c, colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1, &out);
        }
        // Person protection's first pass, when the core or the RenoDX stage wrote it (in GENERAL). The barrier after
        // the copy keeps the next frame's write behind it.
        if (job.firstPass && firstOutput.buffer)
        {
            const VkImage first = runtime->reno ? runtime->first.image : runtime->first_pass_output();
            if (first && (runtime->reno ? runtime->reno->first_written() : runtime->first_pass_written()))
            {
                ImageBarrier(c, first, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                VkBufferImageCopy fc = firstFootprint.vk;
                fc.imageExtent = { w, h, 1 };
                vkCmdCopyImageToBuffer(c, first, VK_IMAGE_LAYOUT_GENERAL, firstOutput.buffer, 1, &fc);
                ImageBarrier(c, first, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT);
            }
        }
        VkCheck(vkEndCommandBuffer(c), "vkEndCommandBuffer");

        // The producer half is already on the game queue: after Follow, its signal releases our submit, ours releases
        // the rest.
        sub.produced = ++producedValue;
        sub.finished = ++finishedValue;
        Check(Follow(producer), "the new queue's wait for the last frame");
        sub.followed = true;
        Check(producer->Signal(toVk.fence, sub.produced), "signal after the producer");
        sub.signalled = true;
        lastSignalled = sub.produced;
        if (call == hooks.dropSubmitAt)
        {
            nr::logf("[mochizuki] MZ_TEST_DROP_SUBMIT_AT: the Vulkan submit of Enqueue %llu is skipped; the game queue "
                     "waits for it all the same (test hook)",
                     static_cast<unsigned long long>(call));
            Submitted(sub.produced, sub.finished); // as if the Vulkan side had lost it
        }
        else
        {
            VkCheck(Submit(c, sub.produced, sub.finished, done[slot]), "vkQueueSubmit");
            inFlight[slot] = true;
        }
        sub.submitted = true;
        Check(producer->Wait(fromVk.fence, sub.finished), "wait for the network");
        ++frames;
    }

    // The game's device, queue and producer queues given back.
    void ReleaseGameQueues()
    {
        lastProducer.Release();
        for (Producer& p : producers)
            p.Release();
        producers.clear();
        if (queue)
            queue->Release();
        if (device)
            device->Release();
    }

    ~Session()
    {
        StopWatchdog();
        JoinBuilder();
        StopReleaser(); // what it has not freed is in `retired`
        vram.Close();   // a kernel handle to the adapter, which no GPU work uses
        // Once the GPU has seen the session, a lost device, a game queue that does not drain within 30 s or Vulkan
        // work that does not finish within kDrainWaitMs a wait (VulkanIdle) means it may still use it all: the game's
        // queues are released (Lose, and Rerelease once more, since the watchdog has stopped), and the Vulkan side and
        // the buffers leak rather than be freed under them; only the game's device and queues are given back.
        // Otherwise everything goes, after a failure too. Before the GPU has seen it (a Start that failed, no build
        // yet) nothing is in use.
        if (gpuUsed && !deviceLost)
        {
            Queues queues;
            if (FAILED(DrainGameQueues(queues)) || (vk.device && VulkanIdle(kDrainWaitMs) != VK_SUCCESS))
                Lose("the session ended with GPU work that did not finish");
        }
        if (gpuUsed && deviceLost)
        {
            Rerelease();
            (void) runtime.release();
            (void) built.release();
            for (Retired& r : retired)
                (void) r.network.release();
            ReleaseGameQueues();
            return;
        }
        if (vk.device)
            vkDeviceWaitIdle(vk.device); // returns at once: all of this session's work has finished
        if (runtime)
            LogNetworkTime("session end");
        runtime.reset();
        built.reset();
        retired.clear();
        ReleaseGeometry();
        FreeBuried(true);
        for (std::unique_ptr<FrameBuffers>* b : { &builtBuffers, &prepared })
            if (*b)
                (*b)->Release(vk.device);
        FreeGraveyard(); // the game queues drained above, an orphaned list with them
        if (vk.device)
        {
            DestroyFences();
            if (pool)
                vkDestroyCommandPool(vk.device, pool, nullptr);
        }
        toVk.Release(vk.device);
        fromVk.Release(vk.device);
        vk.Destroy();
        ReleaseGameQueues();
    }
};

// A call that threw fails with FAILED. A lost device also fails the session for good and releases the game's queues
// (Lose); any other error is the call's alone: recorded for GetInfo from any thread (GetLastError's copy is the calling
// thread's) and logged.
int32_t Failed(Session* s, NrError::Kind kind, const char* what)
{
    char text[320];
    std::snprintf(text, sizeof text, "error (%s): %s", KindName(kind), what);
    if (s && kind == NrError::DeviceLost)
        s->Lose(text);
    else if (s)
        s->NoteError(text);
    else
        nr::logf("[mochizuki] %s", text);
    return Fail(LMXXF_NR_FAILED, what);
}

// whileFailed: the call also runs on a failed session whose device was not lost (Drain, which the host may call
// before Destroy).
template <class Fn> int32_t Guard(Session* s, Fn&& fn, bool whileFailed = false)
{
    if (s && s->failed && (!whileFailed || s->deviceLost))
        return Fail(LMXXF_NR_UNAVAILABLE, "mochizuki session failed earlier; see mochizuki_nr.log");
    try
    {
        return fn();
    }
    catch (const std::exception& e)
    {
        return Failed(s, KindOf(e), e.what());
    }
    catch (...)
    {
        return Failed(s, NrError::Other, "unhandled exception");
    }
}

int32_t QueryCapabilities(LmxxfNrCapabilities* out)
{
    if (!out || out->struct_size != sizeof(LmxxfNrCapabilities))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "QueryCapabilities: struct_size mismatch");
    out->abi_version = LMXXF_NR_ABI_VERSION;
    out->max_input_width = 16384;
    out->max_input_height = 16384;
    out->history_supported = 1;
    out->overlap_supported = out->graph_supported = out->hip_ready = 0;
    out->gfx1201_target = 1;
    return LMXXF_NR_OK;
}

int32_t Create(const LmxxfNrCreateInfo* info, void** context)
{
    if (!info || !context || info->struct_size != sizeof(LmxxfNrCreateInfo))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: struct_size mismatch");
    if (!info->device || !info->queue)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: device and queue required");
    if (info->flags & ~uint32_t(LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK | LMXXF_NR_CREATE_FLAG_ACO))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: unknown flags");
    auto* s = new (std::nothrow) Session;
    if (!s)
        return Fail(LMXXF_NR_FAILED, "Create: out of memory");
    s->zeroOutputFallback = (info->flags & LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK) != 0;
    s->aco = (info->flags & LMXXF_NR_CREATE_FLAG_ACO) != 0;
    s->hooks = Hooks();
    static_cast<IUnknown*>(info->device)->QueryInterface(IID_PPV_ARGS(&s->device));
    static_cast<IUnknown*>(info->queue)->QueryInterface(IID_PPV_ARGS(&s->queue));
    if (!s->device || !s->queue)
    {
        delete s;
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: device or queue is not a D3D12 object");
    }
    *context = s;
    return LMXXF_NR_OK;
}

// During a network build, the build is stopped and waited for at most kAbandonWaitMs; one that takes longer ends the
// session on its own thread (Session::Abandon).
int32_t Destroy(void* context)
{
    auto* s = static_cast<Session*>(context);
    if (s && !s->Abandon())
        return LMXXF_NR_OK;
    delete s;
    return LMXXF_NR_OK;
}

// A Start that fails is undone and reported UNAVAILABLE without failing the session, so the host may try again
// later; one that fails "[unsupported] ..." is remembered for the adapter, and later calls return at once.
int32_t PrepareSession(void* context)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareSession: no session");
    return Guard(s,
                 [&]
                 {
                     if (s->vk.device)
                         return int32_t(LMXXF_NR_OK);
                     const LUID luid = s->device->GetAdapterLuid();
                     if (const std::string reason = UnsupportedReason(luid); !reason.empty())
                     {
                         s->RememberError(reason.c_str());
                         return Fail(LMXXF_NR_UNAVAILABLE, reason.c_str());
                     }
                     try
                     {
                         s->Start();
                     }
                     catch (const std::exception& e)
                     {
                         s->AbandonStart();
                         if (!std::strncmp(e.what(), "[unsupported] ", 14))
                             RememberUnsupported(luid, e.what());
                         else
                             nr::logf("[mochizuki] error: %s (the session start was undone)", e.what());
                         s->RememberError(e.what());
                         return Fail(LMXXF_NR_UNAVAILABLE, e.what());
                     }
                     catch (...)
                     {
                         s->AbandonStart();
                         nr::logf("[mochizuki] error: unhandled exception in Start (the session start was undone)");
                         s->RememberError("PrepareSession: unhandled exception in Start");
                         return Fail(LMXXF_NR_UNAVAILABLE, "PrepareSession: unhandled exception in Start");
                     }
                     s->RememberError(""); // an earlier failed start is over
                     return int32_t(LMXXF_NR_OK);
                 });
}

int32_t PrepareFrame(void* context, const LmxxfNrFrameInfo* info, LmxxfNrJob* job)
{
    auto* s = static_cast<Session*>(context);
    // A host from before LMXXF_NR_FRAME_FLAG_FIRST_PASS sends the job without first_pass_output.
    const bool fullJob = job && job->struct_size == sizeof(LmxxfNrJob);
    if (!s || !info || !job || (!fullJob && job->struct_size != offsetof(LmxxfNrJob, first_pass_output)) ||
        info->struct_size < 64)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: bad arguments");
    job->handle = nullptr;
    job->private_output = nullptr;
    if (fullJob)
        job->first_pass_output = nullptr;
    return Guard(
        s,
        [&]
        {
            if (!s->vk.device)
                return Fail(LMXXF_NR_NOT_IMPLEMENTED, "PrepareFrame: call PrepareSession first");
            s->FreeBuried(false);
            auto* colour = static_cast<ID3D12Resource*>(info->color);
            if (!colour || !info->color_width || !info->color_height)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: colour and its size are required");
            const D3D12_RESOURCE_DESC cd = colour->GetDesc();
            const Format cf = ColourFormat(cd.Format);
            if (!cf.vk || (NeedsBlitCheck(cf.vk) && !s->Blittable(cf.vk)))
            {
                char text[96];
                std::snprintf(text, sizeof text, "colour format %u is not supported", unsigned(cd.Format));
                return Fail(LMXXF_NR_UNAVAILABLE, text);
            }
            // The frame's valid extent: its render subrect, at the top-left of the colour allocation.
            const UINT w = std::min<UINT>(info->color_width, UINT(cd.Width));
            const UINT h = std::min<UINT>(info->color_height, cd.Height);
            Geometry g;
            g.colourFormat = cd.Format;
            // A game that switches its colour between the two float formats (Neverness to Everness, with its phone
            // open) had a network built for each, NR off for seconds at every switch. From the first switch on, float
            // frames share one RGBA16F network: a B10G11R11 frame is converted to it and back (a blit each way, exact
            // on the way in). A game with one format is left as it was. The first switch leaves FloatSharedMarker()
            // behind, so the game's later sessions build the RGBA16F network from their first frame.
            const bool floatColour =
                cd.Format == DXGI_FORMAT_R11G11B10_FLOAT || cd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            if (floatColour && !s->floatShared &&
                (s->floatFormat == DXGI_FORMAT_UNKNOWN ? FloatShared() : s->floatFormat != cd.Format) &&
                s->Blittable(VK_FORMAT_B10G11R11_UFLOAT_PACK32) && s->Blittable(VK_FORMAT_R16G16B16A16_SFLOAT))
            {
                s->floatShared = true;
                if (s->floatFormat == DXGI_FORMAT_UNKNOWN)
                    nr::logf("[mochizuki] the colour switched between float formats in an earlier session: one "
                             "RGBA16F network for both");
                else
                {
                    MarkFloatShared();
                    nr::logf("[mochizuki] the colour switches between float formats: one RGBA16F network for both");
                }
            }
            if (floatColour)
                s->floatFormat = cd.Format;
            const VkFormat netVk = floatColour && s->floatShared ? VK_FORMAT_R16G16B16A16_SFLOAT : cf.vk;
            if (netVk != cf.vk)
                g.netFormat = netVk;
            g.colourWidth = cd.Width;
            g.colourHeight = cd.Height;
            // The motion fields up to reset; a FrameInfo too short for them has no motion.
            const bool hasMotion = info->struct_size >= offsetof(LmxxfNrFrameInfo, smooth_threshold);
            auto* motion = hasMotion ? static_cast<ID3D12Resource*>(info->motion) : nullptr;
            const bool temporal = (info->flags & LMXXF_NR_FRAME_FLAG_TEMPORAL) && motion;
            Format mf;
            D3D12_RESOURCE_DESC md {};
            UINT motionW = 0, motionH = 0; // the motion's valid extent
            if (temporal)
            {
                md = motion->GetDesc();
                mf = MotionFormat(md.Format);
                if (!mf.vk && s->motionRefused.exchange(uint32_t(md.Format)) != uint32_t(md.Format))
                    nr::logf("[mochizuki] motion vectors in DXGI format %u are not supported; running without history",
                             unsigned(md.Format));
                if (mf.vk)
                {
                    s->motionRefused = 0;
                    motionW = std::min<UINT>(info->motion_width ? info->motion_width : w, UINT(md.Width));
                    motionH = std::min<UINT>(info->motion_height ? info->motion_height : h, md.Height);
                }
            }

            // The frame's controls, every one sanitised (see San); a FrameInfo without a field gets its default.
            float detail = 1.f, colourStrength = 1.f;
            if (info->flags & LMXXF_NR_FRAME_FLAG_STRENGTH)
            {
                if (MZ_HOLDS(info, LmxxfNrFrameInfo, transfer_strength))
                    detail = San(info->transfer_strength, 0.f, 2.f, 1.f);
                if (MZ_HOLDS(info, LmxxfNrFrameInfo, color_strength))
                    colourStrength = San(info->color_strength, 0.f, 4.f, 1.f);
            }
            const float scale = MZ_HOLDS(info, LmxxfNrFrameInfo, model_scale) ? ModelScale(info->model_scale) : 1.f;
            const uint32_t framePasses =
                MZ_HOLDS(info, LmxxfNrFrameInfo, passes) ? std::clamp(info->passes, 1u, kMaxPasses) : 1u; // 0 is 1
            nr::Controls model;
            float history = 1.f, white = 1.f;
            uint32_t linearMode = 0, maxPassesSetting = 0, drsMode = 0;
            bool renodx = false, styleGrade = true, historyF16 = true;
            uint32_t optimizedLayers = 3;
            nr::renodx::Settings reno;
            const float exposure = s->gameExposure.load(std::memory_order_relaxed);
            {
                std::lock_guard lock(s->controlsMutex);
                model = s->controls.model;
                history = s->controls.history;
                white = s->controls.white;
                linearMode = s->controls.linearMode;
                if (exposure > 0.f)
                    white = std::clamp(white / exposure, 1e-4f, 1e4f);
                maxPassesSetting = s->controls.maxPasses;
                drsMode = s->controls.drsMode;
                renodx = s->controls.renodx;
                reno = s->controls.reno;
                styleGrade = s->controls.styleGrade;
                historyF16 = s->controls.historyF16;
                optimizedLayers = s->controls.optimizedLayers;
            }
            // The RenoDX stage runs its own stack, whatever the frame asks for.
            const uint32_t passes = renodx ? nr::renodx::stack_passes(reno) : framePasses;
            // By default the network is built for exactly the frame's passes, so one pass costs nothing extra; a
            // larger max_passes lets the pass count change without a rebuild.
            const uint32_t maxPasses = maxPassesSetting ? std::clamp(maxPassesSetting, passes, kMaxPasses) : passes;
            const bool linear = linearMode == 0 ? IsLinear(cf.vk) : linearMode == 1;
            // The extent the network and the frame's buffers are built for: the frame's subrect, or with dynamic
            // resolution the bucket, whose motion buffer is the motion allocation's.
            const bool blittable = !drsMode || !mf.vk || s->Blittable(mf.vk);
            s->DrsExtent(drsMode, w, h, cd, blittable, g.width, g.height, g.bucketed);
            // Exact geometry's buffers are the subrect's: a game that renders it into two colour textures of other
            // sizes in turn (Forza Horizon 6) keeps them.
            if (!g.bucketed)
            {
                g.colourWidth = 0;
                g.colourHeight = 0;
            }
            else
            {
                // Keyed on the largest allocation the bucket holds, not this one: a game that reallocates its colour
                // and vectors at every step (Cyberpunk 2077) keeps the frame's buffers.
                g.colourWidth = std::max<UINT64>(g.colourWidth, s->bucket.limitWidth);
                g.colourHeight = std::max(g.colourHeight, s->bucket.limitHeight);
            }
            if (mf.vk)
            {
                g.motionFormat = md.Format;
                g.motionWidth = motionW;
                g.motionHeight = motionH;
                if (g.bucketed)
                {
                    s->bucket.motionWidth = std::max(s->bucket.motionWidth, UINT(md.Width));
                    s->bucket.motionHeight = std::max(s->bucket.motionHeight, md.Height);
                    g.motionWidth = s->bucket.motionWidth;
                    g.motionHeight = s->bucket.motionHeight;
                }
            }
            // A frame without vectors on a geometry made with them (a menu, a cut) keeps its buffers: it runs without
            // the history, and the next frame with vectors needs no new ones.
            const bool keepMotion = !mf.vk && s->geometry.motionFormat != DXGI_FORMAT_UNKNOWN &&
                                    s->geometry.colourFormat == g.colourFormat && s->geometry.width == g.width &&
                                    s->geometry.height == g.height && s->geometry.bucketed == g.bucketed;
            if (keepMotion)
            {
                g.motionFormat = s->geometry.motionFormat;
                g.motionWidth = s->geometry.motionWidth;
                g.motionHeight = s->geometry.motionHeight;
                g.depthFormat = s->geometry.depthFormat;
            }
            // The game's depth, with its vectors: its top-left w x h, as the colour's.
            ID3D12Resource* depth = nullptr;
            // The RenoDX stage's network reads the vectors at the pixel: no depth is copied for it.
            if (mf.vk && !renodx && MZ_HOLDS(info, LmxxfNrFrameInfo, depth_inverted) && info->depth)
            {
                auto* d = static_cast<ID3D12Resource*>(info->depth);
                const D3D12_RESOURCE_DESC dd = d->GetDesc();
                if (DepthFormat(dd.Format).vk && dd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    dd.SampleDesc.Count == 1 && dd.Width >= w && dd.Height >= h)
                {
                    depth = d;
                    g.depthFormat = dd.Format;
                    s->depthRefused = 0;
                }
                else if (s->depthRefused.exchange(uint32_t(dd.Format)) != uint32_t(dd.Format))
                    nr::logf("[mochizuki] depth in DXGI format %u (%llux%u, %u samples) is not supported; the history "
                             "reads the motion vectors at the pixel",
                             unsigned(dd.Format), static_cast<unsigned long long>(dd.Width), dd.Height,
                             dd.SampleDesc.Count);
            }
            // Likewise a frame without depth keeps the depth buffers of a geometry otherwise its own.
            if (mf.vk && !depth && s->geometry.depthFormat != DXGI_FORMAT_UNKNOWN &&
                s->geometry.motionFormat == g.motionFormat && s->geometry.motionWidth == g.motionWidth &&
                s->geometry.motionHeight == g.motionHeight && s->geometry.colourFormat == g.colourFormat &&
                s->geometry.width == g.width && s->geometry.height == g.height && s->geometry.bucketed == g.bucketed)
                g.depthFormat = s->geometry.depthFormat;
            // Neither a network nor buffers that cannot be had now fail the session: the frame goes without NR, and
            // they are tried again as their holds say.
            s->prepWanted = s->prepWanted || model.preprocess.active();
            const bool int4 = (info->flags & LMXXF_NR_FRAME_FLAG_INT4) != 0;
            // One pass has no later pass to change: the flag alone does not rebuild its network.
            const bool cascade = (info->flags & LMXXF_NR_FRAME_FLAG_CASCADE_DETAIL) != 0 && maxPasses > 1;
            const bool original = (info->flags & LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL) != 0;
            // Person protection's first pass: its own buffers and a network that keeps it, with two or more passes
            // (the RenoDX stage's: its pass 1, StageFrame::first).
            const bool firstPass =
                fullJob && (info->flags & LMXXF_NR_FRAME_FLAG_FIRST_PASS) && (renodx ? passes : maxPasses) > 1;
            g.firstPass = firstPass;
            NetworkKey key { g.width,       g.height, netVk,   scale,    maxPasses, linear,
                             s->prepWanted, int4,     cascade, original, firstPass, historyF16 };
            key.experimental = !original && !int4 && (info->flags & LMXXF_NR_FRAME_FLAG_EXPERIMENTAL) != 0;
            key.layers = int4 ? optimizedLayers : 0;
            // The RenoDX stage: its own extent, pass count, preprocess and units; the model scale and the pass extras
            // are the native route's.
            if (renodx)
            {
                key.renodx = true;
                nr::renodx::working_resolution(reno, g.width, g.height, motionW, motionH, &key.renoWidth,
                                               &key.renoHeight);
                key.sourceEncoding = reno.source_encoding;
                key.linearNits = reno.linear_unit_nits;
                key.maxPasses = nr::renodx::stack_passes(reno);
                key.scale = 1.f;
                key.prep = key.cascade = false;
            }
            std::string why;
            if (!s->EnsureNetwork(key, g, colour, why))
                return Fail(LMXXF_NR_UNAVAILABLE, why.c_str());
            if (!(s->geometry == g))
            {
                s->RetireGeometry();
                if (!s->MakeGeometry(g, colour, why))
                    return Fail(LMXXF_NR_UNAVAILABLE, why.c_str());
                if (g.bucketed)
                    nr::logf("[mochizuki] frame buffers %ux%u for dynamic resolution, colour allocation %llux%u DXGI "
                             "%u, motion allocation %ux%u DXGI %u",
                             g.width, g.height, static_cast<unsigned long long>(g.colourWidth), g.colourHeight,
                             unsigned(g.colourFormat), g.motionWidth, g.motionHeight, unsigned(g.motionFormat));
                else
                    nr::logf("[mochizuki] frame %ux%u colour DXGI %u, motion %ux%u DXGI %u, depth DXGI %u", g.width,
                             g.height, unsigned(g.colourFormat), g.motionWidth, g.motionHeight,
                             unsigned(g.motionFormat), unsigned(g.depthFormat));
            }
            // A new valid extent restarts the history: it holds the frame's content where the old one had it.
            if (w != s->validWidth || h != s->validHeight)
            {
                s->validWidth = w;
                s->validHeight = h;
                s->infoValidWidth = w;
                s->infoValidHeight = h;
                s->resetPending = true;
            }

            // While a build for new settings runs, the frame keeps the network it ran on: no more passes than that one
            // was built for.
            const uint32_t served = std::min(passes, s->net.maxPasses);

            // History carries over only to the frame right after this one.
            const ULONGLONG now = GetTickCount64();
            const bool continuous = info->frame_id == s->lastFrameId + 1 && now - s->lastFrameTick < 250;
            s->lastFrameId = info->frame_id;
            s->lastFrameTick = now;

            // Each pass keeps a history of its own, and one that did not run on the last frame holds a stale one.
            if (served != s->lastPasses)
            {
                if (s->lastPasses)
                    nr::logf("[mochizuki] %u passes (was %u): history reset", served, s->lastPasses);
                s->lastPasses = served;
                s->infoPasses = served;
                s->resetPending = true;
            }

            Job next;
            next.colour = colour;
            next.colourState = static_cast<D3D12_RESOURCE_STATES>(info->color_state);
            next.width = w;
            next.height = h;
            next.controls = std::move(model);
            next.controls.detail_strength = detail;
            next.controls.colour_strength = colourStrength;
            next.controls.passes = int(served);
            next.controls.anti_halo = (info->flags & LMXXF_NR_FRAME_FLAG_ANTI_HALO) != 0;
            next.controls.pass3_estimate = (info->flags & LMXXF_NR_FRAME_FLAG_PASS3_ESTIMATE) != 0;
            next.history = history;
            next.white = white;
            next.reno = reno;
            next.renoExposure = exposure;
            next.styleGrade = styleGrade;
            s->infoWhite.store(white, std::memory_order_relaxed);
            if (mf.vk)
            {
                next.motion = motion;
                next.motionState = static_cast<D3D12_RESOURCE_STATES>(info->motion_state);
                next.motionWidth = motionW;
                next.motionHeight = motionH;
                // value * scale = pixels of the motion extent; the network reads previous uv = uv + mv.
                next.motionScaleX = info->motion_scale_x / float(motionW);
                next.motionScaleY = info->motion_scale_y / float(motionH);
                if (g.depthFormat != DXGI_FORMAT_UNKNOWN)
                {
                    next.depth = depth;
                    next.depthState = static_cast<D3D12_RESOURCE_STATES>(info->depth_state);
                    next.depthInverted = info->depth_inverted != 0;
                }
                if (g.bucketed)
                {
                    // In the bucket, as uploaded when they span the whole of it as in exact; otherwise laid over the
                    // subrect [0, w) x [0, h), so their uv is the bucket's times w / W.
                    const bool whole = w == g.width && h == g.height;
                    next.motionPath = whole && motionW == g.motionWidth && motionH == g.motionHeight ? Job::Uploaded
                                      : motionW == w && motionH == h                                 ? Job::Padded
                                                                                                     : Job::Rescaled;
                    if (next.motionPath != Job::Uploaded)
                    {
                        next.motionScaleX *= float(w) / float(g.width);
                        next.motionScaleY *= float(h) / float(g.height);
                    }
                }
                next.reset = s->resetPending.exchange(false) || info->reset || !continuous;
            }
            else
                s->resetPending = true; // record() neither updates the history nor consumes a reset
            next.state = LMXXF_NR_JOB_PREPARED;
            {
                // A new generation: whatever still holds the last job's handle can no longer reach this one.
                std::lock_guard lock(s->jobMutex);
                s->job = std::move(next);
                job->handle = JobHandle(++s->jobGen);
            }
            job->private_output = s->result;
            // Only where the core writes it this frame (the network running now keeps it, more than one pass, no
            // preprocess, the model applied), or the RenoDX stage does (it can, and the model is applied); else the
            // host composes without it.
            const bool written =
                s->net.renodx ? s->runtime && s->runtime->first.image : !s->job.controls.preprocess.active();
            if (firstPass && s->net.firstPass && served > 1 && written && s->job.controls.apply_model && s->firstResult)
            {
                job->first_pass_output = s->firstResult;
                s->job.firstPass = true;
            }
            SetError("");
            return int32_t(LMXXF_NR_OK);
        });
}

// Record*, EnqueueHip and Poll act only on the current job (the handle of the last PrepareFrame); the job's state
// changes under jobMutex.
int32_t RecordInputs(void* context, void* job, void* command_list)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordInputs: no session");
    return Guard(s,
                 [&]
                 {
                     auto* c = static_cast<ID3D12GraphicsCommandList*>(command_list);
                     std::lock_guard lock(s->jobMutex);
                     Job& j = s->job;
                     if (!c || job != JobHandle(s->jobGen) || j.state != LMXXF_NR_JOB_PREPARED)
                         return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordInputs: job not prepared");
                     CopyToBuffer(c, j.colour, j.colourState, s->input.resource, s->colourFootprint, j.width, j.height);
                     if (j.motion)
                         CopyToBuffer(c, j.motion, j.motionState, s->motionBuffer.resource, s->motionFootprint,
                                      j.motionWidth, j.motionHeight);
                     if (j.depth)
                         CopyToBuffer(c, j.depth, j.depthState, s->depthBuffer.resource, s->depthFootprint, j.width,
                                      j.height);
                     j.state = LMXXF_NR_JOB_PRODUCER_SUBMITTED;
                     return int32_t(LMXXF_NR_OK);
                 });
}

// Runs the job's frame once: a second call for it returns OK and does nothing. A frame the network failed on (with
// ZERO_OUTPUT_FALLBACK) passed its colour through: OK, with the diagnostic in GetLastError.
int32_t EnqueueHip(void* context, void* job, void* command_queue)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "EnqueueHip: no session");
    return Guard(s,
                 [&]
                 {
                     auto* q = static_cast<ID3D12CommandQueue*>(command_queue ? command_queue : s->queue);
                     {
                         std::lock_guard lock(s->jobMutex);
                         Job& j = s->job;
                         if (job != JobHandle(s->jobGen) ||
                             (!j.enqueued && j.state != LMXXF_NR_JOB_PRODUCER_SUBMITTED &&
                              j.state != LMXXF_NR_JOB_CONSUMER_COMPLETE))
                             return Fail(LMXXF_NR_INVALID_ARGUMENT, "EnqueueHip: job not recorded");
                         if (j.enqueued)
                         {
                             SetError("");
                             return int32_t(LMXXF_NR_OK);
                         }
                         j.enqueued = true;
                     }
                     std::string why;
                     const bool ran = s->Enqueue(q, why);
                     {
                         std::lock_guard lock(s->jobMutex);
                         if (job == JobHandle(s->jobGen) && s->job.state == LMXXF_NR_JOB_PRODUCER_SUBMITTED)
                             s->job.state = LMXXF_NR_JOB_NR_COMPLETE;
                     }
                     if (!ran)
                         s->NoteError(why.c_str());
                     SetError(ran ? "" : why.c_str());
                     return int32_t(LMXXF_NR_OK);
                 });
}

int32_t RecordOutputs(void* context, void* job, void* command_list)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordOutputs: no session");
    return Guard(
        s,
        [&]
        {
            auto* c = static_cast<ID3D12GraphicsCommandList*>(command_list);
            std::lock_guard lock(s->jobMutex);
            Job& j = s->job;
            if (!c || job != JobHandle(s->jobGen) ||
                (j.state != LMXXF_NR_JOB_PRODUCER_SUBMITTED && j.state != LMXXF_NR_JOB_NR_COMPLETE))
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordOutputs: inputs not recorded");
            Transition(c, s->output.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(c, s->result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst { s->result, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
            D3D12_TEXTURE_COPY_LOCATION src { s->output.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
            src.PlacedFootprint = s->colourFootprint.d3d;
            // The frame's valid extent into result's top-left: all of the buffer, except with dynamic resolution.
            const D3D12_BOX box { 0, 0, 0, j.width, j.height, 1 };
            const bool whole =
                j.width == src.PlacedFootprint.Footprint.Width && j.height == src.PlacedFootprint.Footprint.Height;
            c->CopyTextureRegion(&dst, 0, 0, 0, &src, whole ? nullptr : &box);
            Transition(c, s->result, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(c, s->output.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (j.firstPass && s->firstResult)
            {
                Transition(c, s->firstOutput.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(c, s->firstResult, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION fdst { s->firstResult, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
                D3D12_TEXTURE_COPY_LOCATION fsrc { s->firstOutput.resource,
                                                   D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                   {} };
                fsrc.PlacedFootprint = s->firstFootprint.d3d;
                c->CopyTextureRegion(&fdst, 0, 0, 0, &fsrc, whole ? nullptr : &box);
                Transition(c, s->firstResult, D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Transition(c, s->firstOutput.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
            if (j.state == LMXXF_NR_JOB_PRODUCER_SUBMITTED)
                j.state = LMXXF_NR_JOB_CONSUMER_COMPLETE;
            return int32_t(LMXXF_NR_OK);
        });
}

// The current job only. One whose inputs or outputs are recorded in a list the game may still run is orphaned: see
// ReleaseGeometry.
int32_t CancelUnsubmitted(void* context, void* job)
{
    if (auto* s = static_cast<Session*>(context))
    {
        std::lock_guard lock(s->jobMutex);
        if (job != JobHandle(s->jobGen))
            return LMXXF_NR_OK;
        if (s->job.state == LMXXF_NR_JOB_PRODUCER_SUBMITTED || s->job.state == LMXXF_NR_JOB_CONSUMER_COMPLETE)
        {
            s->orphaned = true;
            s->orphanGen = s->jobGen;
        }
        s->job.state = LMXXF_NR_JOB_RETIRED;
        s->resetPending = true;
    }
    return LMXXF_NR_OK;
}

int32_t Poll(void* context, void* job, uint32_t* state)
{
    auto* s = static_cast<Session*>(context);
    if (state)
        *state = LMXXF_NR_JOB_NONE;
    if (!s)
        return LMXXF_NR_OK;
    std::lock_guard lock(s->jobMutex);
    if (job != JobHandle(s->jobGen))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Poll: not the current job");
    if (state)
        *state = s->job.state;
    return LMXXF_NR_OK;
}

// The current job only, once its network and outputs are in: a late Retire of an earlier job leaves the next alone.
int32_t Retire(void* context, void* job)
{
    if (auto* s = static_cast<Session*>(context))
    {
        std::lock_guard lock(s->jobMutex);
        if (job == JobHandle(s->jobGen) &&
            (s->job.state == LMXXF_NR_JOB_NR_COMPLETE || s->job.state == LMXXF_NR_JOB_CONSUMER_COMPLETE))
        {
            s->job.state = LMXXF_NR_JOB_RETIRED;
            s->retiredGen = s->jobGen;
        }
    }
    return LMXXF_NR_OK;
}

int32_t ResetHistory(void* context)
{
    if (auto* s = static_cast<Session*>(context))
        s->resetPending = true;
    return LMXXF_NR_OK;
}

// Also on a failed session whose device was not lost.
int32_t Drain(void* context)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Drain: no session");
    return Guard(
        s,
        [&]
        {
            s->Drain();
            return int32_t(LMXXF_NR_OK);
        },
        true);
}

int32_t GetStatus(void* context, char* buf, uint32_t chars)
{
    if (!buf || !chars)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetStatus: empty buffer");
    auto* s = static_cast<Session*>(context);
    char text[256];
    if (!s)
        std::snprintf(text, sizeof text, "no session");
    else if (s->failed)
        std::snprintf(text, sizeof text, "mochizuki failed (see mochizuki_nr.log)");
    else if (s->runtime)
    {
        float median = 0, p95 = 0;
        s->NetworkMs(median, p95);
        // The model extent and the passes, when they are not the frame's own and one.
        char model[64] = {};
        const uint32_t mw = s->infoModelWidth, mh = s->infoModelHeight, passes = s->infoPasses;
        if (mw != s->net.width || mh != s->net.height || passes > 1)
            std::snprintf(model, sizeof model, " (model %ux%u, %u pass%s)", mw, mh, passes, passes > 1 ? "es" : "");
        // With dynamic resolution, the last frame's subrect when it is not the whole network extent.
        char frame[64] = {};
        const uint32_t vw = s->infoValidWidth, vh = s->infoValidHeight;
        if (vw && (vw != s->net.width || vh != s->net.height))
            std::snprintf(frame, sizeof frame, " (dynamic resolution, frame %ux%u)", vw, vh);
        std::snprintf(text, sizeof text, "mochizuki %ux%u%s%s, network %.2f ms (p95 %.2f), %llu frames", s->net.width,
                      s->net.height, model, frame, median, p95, static_cast<unsigned long long>(s->frames.load()));
    }
    else
    {
        bool building = false;
        {
            std::lock_guard lock(s->buildMutex);
            building = s->building;
        }
        std::snprintf(text, sizeof text, "mochizuki %s", building ? "building the network" : "idle");
    }
    std::strncpy(buf, text, chars - 1);
    buf[chars - 1] = 0;
    return LMXXF_NR_OK;
}

int32_t GetLastError(char* buf, uint32_t chars)
{
    if (!buf || !chars)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetLastError: empty buffer");
    std::strncpy(buf, g_lastError, chars - 1);
    buf[chars - 1] = 0;
    return LMXXF_NR_OK;
}

// MochizukiNrControls.h. Every Controls field is 4 bytes and 4-aligned and the struct ends at its last field, so the
// whole fields a caller's struct holds are its first struct_size & ~3 bytes.
static_assert(alignof(MochizukiNrControls) == 4 &&
              sizeof(MochizukiNrControls) == offsetof(MochizukiNrControls, optimized_layers) +
                                                 sizeof(MochizukiNrControls::optimized_layers) &&
              offsetof(MochizukiNrControls, pass) == 72 && offsetof(MochizukiNrControls, preprocess) == 136 &&
              offsetof(MochizukiNrControls, enlarge_mode) == 164 && offsetof(MochizukiNrControls, renodx) == 176 &&
              sizeof(MochizukiNrPassControls) == 32);
// Info has no implicit padding (reserved0 fills the gap before frames), so its offsets do not depend on packing; its
// sizeof does (tail padding after network_dispatches), which is why a filler reports the end of its last field.
static_assert(offsetof(MochizukiNrInfo, reserved0) == 44 && offsetof(MochizukiNrInfo, frames) == 48 &&
              offsetof(MochizukiNrInfo, last_error) == 64 && offsetof(MochizukiNrInfo, network_dispatches) == 320 &&
              offsetof(MochizukiNrInfo, preprocess_ev) == 324 && offsetof(MochizukiNrInfo, white_point) == 328 &&
              offsetof(MochizukiNrInfo, build_stage) == 332 && offsetof(MochizukiNrInfo, build_total) == 340 &&
              offsetof(MochizukiNrInfo, renodx_effective) == 344 && offsetof(MochizukiNrInfo, network) == 348 &&
              offsetof(MochizukiNrInfo, rdna2_int8) == 360 && sizeof(MochizukiNrInfo) == 368);

// The bytes GetInfo writes into a struct of `size` bytes: the whole fields that fit, never part of one, and never
// Info's tail padding. A host that appends a field there (sizeof does not grow) then sees struct_size end before it.
size_t InfoBytes(size_t size)
{
    constexpr size_t ends[] = {
        offsetof(MochizukiNrInfo, rdna2_int8) + sizeof(MochizukiNrInfo::rdna2_int8), // the last field
        offsetof(MochizukiNrInfo, rdna2_int8),
        offsetof(MochizukiNrInfo, optimized),
        offsetof(MochizukiNrInfo, experimental),
        offsetof(MochizukiNrInfo, network),
        offsetof(MochizukiNrInfo, renodx_effective),
        offsetof(MochizukiNrInfo, build_stage),
        offsetof(MochizukiNrInfo, white_point) + sizeof(MochizukiNrInfo::white_point),
        offsetof(MochizukiNrInfo, white_point),
        offsetof(MochizukiNrInfo, preprocess_ev),
        offsetof(MochizukiNrInfo, network_dispatches),
        offsetof(MochizukiNrInfo, last_error),
        offsetof(MochizukiNrInfo, failed),
        offsetof(MochizukiNrInfo, history_consumed_pct),
        offsetof(MochizukiNrInfo, frames),
    };
    for (const size_t end : ends)
        if (size >= end)
            return end;
    return size & ~size_t(3); // below frames every field is 4 bytes
}

// Never fails the session: everything is sanitised, and only the pass overrides allocate.
int32_t SetControls(void* context, const MochizukiNrControls* in)
{
    auto* s = static_cast<Session*>(context);
    if (!s || !in || in->struct_size < sizeof(uint32_t))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "SetControls: bad arguments");
    try
    {
        MochizukiNrControls c;
        DefaultControls(c); // what a shorter struct does not hold
        std::memcpy(&c, in, std::min<size_t>(in->struct_size, sizeof c) & ~size_t(3));
        SessionControls sanitised = Sanitise(c);
        std::lock_guard lock(s->controlsMutex);
        s->controls = std::move(sanitised);
        return int32_t(LMXXF_NR_OK);
    }
    catch (const std::exception& e)
    {
        return Fail(LMXXF_NR_FAILED, e.what());
    }
}

// Atomics and the remembered error only: it may run on any thread, next to a frame or a build.
int32_t GetInfo(void* context, MochizukiNrInfo* out)
{
    auto* s = static_cast<Session*>(context);
    if (!s || !out || out->struct_size < sizeof(uint32_t))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetInfo: bad arguments");
    try
    {
        MochizukiNrInfo info {};
        info.building = s->infoBuilding;
        info.frame_w = s->infoFrameWidth;
        info.frame_h = s->infoFrameHeight;
        info.model_w = s->infoModelWidth;
        info.model_h = s->infoModelHeight;
        info.max_passes = s->infoMaxPasses;
        info.motion_refused_dxgi = s->motionRefused;
        s->NetworkMs(info.gpu_ms_median, info.gpu_ms_p95);
        info.build_seconds = s->infoBuildSeconds;
        info.frames = s->frames;
        const uint32_t n = s->historyCount;
        const uint64_t window = n < kHistorySamples ? (uint64_t(1) << n) - 1 : ~uint64_t(0);
        info.history_consumed_pct = n ? uint32_t(std::popcount(s->historyBits & window)) * 100 / n : 0;
        info.failed = s->failed;
        info.network_dispatches = s->dispatches;
        info.preprocess_ev = s->infoPrepEv;
        info.white_point = s->infoWhite;
        info.renodx_effective = s->infoReno;
        info.network = s->vk.arch == nr::Arch::rdna2   ? 3u
                       : s->vk.arch == nr::Arch::rdna3 ? (s->vk.rdna3Aco ? 2u : 1u)
                                                        : 0u;
        info.experimental = s->infoExperimental;
        info.optimized = s->infoOptimized;
        info.rdna2_int8 = s->vk.arch == nr::Arch::rdna2 && nr::rdna2_int8;
        // The core's own build progress (nr_log.hpp: builds run one at a time), while this session builds.
        if (info.building)
        {
            info.build_stage = uint32_t(nr::g_build_stage.load());
            info.build_done = nr::g_build_pipes_done.load();
            info.build_total = nr::g_build_pipes_total.load();
        }
        {
            std::lock_guard lock(s->errorMutex);
            std::memcpy(info.last_error, s->lastError, sizeof info.last_error);
        }
        const size_t bytes = InfoBytes(out->struct_size);
        info.struct_size = uint32_t(bytes);
        std::memcpy(out, &info, bytes);
        return int32_t(LMXXF_NR_OK);
    }
    catch (const std::exception& e)
    {
        return Fail(LMXXF_NR_FAILED, e.what());
    }
}

int32_t GetControlDefaults(MochizukiNrControls* out)
{
    if (!out || out->struct_size < sizeof(uint32_t))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetControlDefaults: bad arguments");
    MochizukiNrControls c;
    DefaultControls(c);
    const size_t bytes = std::min<size_t>(out->struct_size, sizeof c) & ~size_t(3);
    c.struct_size = uint32_t(bytes);
    std::memcpy(out, &c, bytes);
    return LMXXF_NR_OK;
}
} // namespace

extern "C" __declspec(dllexport) int32_t LmxxfNrGetApi(uint32_t abi_version, LmxxfNrApi* out)
{
    if (!out || out->struct_size != sizeof(LmxxfNrApi))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetApi: struct_size mismatch");
    if (abi_version != LMXXF_NR_ABI_VERSION)
        return Fail(LMXXF_NR_UNSUPPORTED_ABI, "GetApi: unsupported abi_version");
    nr::set_log_sink(LogLine);
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof(LmxxfNrApi);
    out->abi_version = LMXXF_NR_ABI_VERSION;
    out->QueryCapabilities = QueryCapabilities;
    out->Create = Create;
    out->Destroy = Destroy;
    out->PrepareSession = PrepareSession;
    out->PrepareFrame = PrepareFrame;
    out->RecordInputs = RecordInputs;
    out->EnqueueHip = EnqueueHip;
    out->RecordOutputs = RecordOutputs;
    out->ExecuteAfterProducer = EnqueueHip;
    out->CancelUnsubmitted = CancelUnsubmitted;
    out->Poll = Poll;
    out->Retire = Retire;
    out->ResetHistory = ResetHistory;
    out->Drain = Drain;
    out->GetStatus = GetStatus;
    out->GetLastError = GetLastError;
    return LMXXF_NR_OK;
}

// MochizukiNrControls.h: mochizuki's own exports, resolved by the host with GetProcAddress.
extern "C" __declspec(dllexport) int32_t MochizukiNrSetControls(void* context, const MochizukiNrControls* controls)
{
    return SetControls(context, controls);
}

extern "C" __declspec(dllexport) int32_t MochizukiNrGetInfo(void* context, MochizukiNrInfo* info)
{
    return GetInfo(context, info);
}

extern "C" __declspec(dllexport) int32_t MochizukiNrGetControlDefaults(MochizukiNrControls* controls)
{
    return GetControlDefaults(controls);
}

extern "C" __declspec(dllexport) int32_t MochizukiNrSetExposure(void* context, float exposure)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "SetExposure: no session");
    const float e = std::isfinite(exposure) && exposure > 0.f ? exposure : 0.f;
    if ((e > 0.f) != (s->gameExposure.exchange(e, std::memory_order_relaxed) > 0.f))
    {
        if (e > 0.f)
            nr::logf("[mochizuki] white point from the game's exposure (%.4f now)", double(e));
        else
            nr::logf("[mochizuki] the game's exposure is gone: the white point is the setting alone");
    }
    return LMXXF_NR_OK;
}

// Process exit, from the host's exit hook while the loader is not yet shutting down: the session ends here, so no
// Vulkan device or instance (nor the implicit layers loaded into it) and no worker thread is left for the DLLs' detach,
// whose order nobody here controls (Cyberpunk 2077 2.5: an access violation box on every exit). A game queue gets
// kDrainWaitMs to drain; one that does not, or a lost device, leaks the Vulkan side as ~Session does. A network build
// is not waited for: UNAVAILABLE, and the session stays to the OS as before.
extern "C" __declspec(dllexport) int32_t MochizukiNrDestroyAtExit(void* context)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return LMXXF_NR_OK;
    {
        std::lock_guard lock(s->buildMutex);
        if (s->building && !s->buildDone)
            return Fail(LMXXF_NR_UNAVAILABLE, "DestroyAtExit: a network build is running");
    }
    s->atExit = true;
    delete s;
    nr::logf("[mochizuki] process exit: session ended");
    return LMXXF_NR_OK;
}

// ANY_QUEUE: a frame may come from any game queue (Session::Follow), so the host keeps the session on a queue change.
extern "C" __declspec(dllexport) uint32_t MochizukiNrGetFeatures(void) { return MOCHIZUKI_NR_FEATURE_ANY_QUEUE; }
