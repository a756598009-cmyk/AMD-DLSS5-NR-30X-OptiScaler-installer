// Early list wrap behind Streamline's interposer (Forza Horizon 6): before the swapchain, a list the exe makes through
// sl.interposer.dll is wrapped like one it makes itself, and one the interposer makes on its own thread is not. The
// same fixture loaded as sl.common.dll stands for a Streamline plugin: lists it makes stay native even when the exe
// called it. Built with tools\test-lmxxf-interposer-wrap.cmd.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#define LOG_WARN(...) ((void) 0)
#include "../OptiScaler/dlssnr/submission/SubmissionHooks.h"
#include <cstdio>
#include <cstdlib>

using CreateList = HRESULT (*)(ID3D12Device*, ID3D12CommandAllocator*, void**);
using CreateList1 = HRESULT (*)(ID3D12Device*, void**);
using Call = void* (*) (void* (*) ());

static void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        std::fprintf(stderr, "FAIL: %s hr=%08lx\n", what, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

static void Require(bool ok, const char* what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
    std::printf("ok  : %s\n", what);
}

static ID3D12Device* MakeDevice()
{
    IDXGIFactory4* factory = nullptr;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ID3D12Device* device = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !device && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        adapter->Release();
    }
    factory->Release();
    return device;
}

// Whether a list is a proxy; releases it.
static bool Wrapped(void* list)
{
    auto* unknown = static_cast<IUnknown*>(list);
    DlssNr::Submission::ILogicalCommandList* logical = nullptr;
    const bool wrapped =
        SUCCEEDED(unknown->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                          reinterpret_cast<void**>(&logical))) &&
        logical;
    if (logical)
        logical->Release();
    unknown->Release();
    return wrapped;
}

static __declspec(noinline) void* WhoCalls()
{
    return DlssNr::Submission::Hooks::CallerModule(_ReturnAddress());
}

static HMODULE LoadFixture(const wchar_t* name)
{
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wcscpy_s(wcsrchr(path, L'\\') + 1, MAX_PATH - (wcsrchr(path, L'\\') + 1 - path), name);
    const HMODULE module = LoadLibraryW(path);
    Require(module != nullptr, "fixture loads");
    return module;
}

int main()
{
    namespace Hooks = DlssNr::Submission::Hooks;
    ID3D12Device* device = MakeDevice();
    Require(device != nullptr, "D3D12 device");
    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    ID3D12CommandAllocator* alloc = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");

    const HMODULE interposer = LoadFixture(L"sl.interposer.dll");
    const HMODULE plugin = LoadFixture(L"sl.common.dll");
    const auto viaInterposer = reinterpret_cast<CreateList>(GetProcAddress(interposer, "FixtureCreateList"));
    const auto viaInterposer1 = reinterpret_cast<CreateList1>(GetProcAddress(interposer, "FixtureCreateList1"));
    const auto interposerOwn = reinterpret_cast<CreateList>(GetProcAddress(interposer, "FixtureCreateOwnList"));
    const auto viaPlugin = reinterpret_cast<CreateList>(GetProcAddress(plugin, "FixtureCreateList"));
    const auto callInterposer = reinterpret_cast<Call>(GetProcAddress(interposer, "FixtureCall"));
    const auto callPlugin = reinterpret_cast<Call>(GetProcAddress(plugin, "FixtureCall"));
    Require(viaInterposer && viaInterposer1 && interposerOwn && viaPlugin && callInterposer && callPlugin,
            "fixture exports");

    Require(callInterposer(WhoCalls) == GetModuleHandleW(nullptr), "a call through the interposer is the exe's");
    Require(callPlugin(WhoCalls) == plugin, "a call from a plugin is the plugin's");

    Check(Hooks::Arm(device, queue), "arm hooks");
    Hooks::SetWrapOpenLists(true);
    Hooks::SetEarlyExeWrap(true);
    Require(!Hooks::ProxyWrapEnabled(), "before the swapchain");

    void* list = nullptr;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                    __uuidof(ID3D12GraphicsCommandList), &list),
          "exe list");
    static_cast<ID3D12GraphicsCommandList*>(list)->Close();
    Require(Wrapped(list), "the exe's own list is wrapped");

    Check(viaInterposer(device, alloc, &list), "list through the interposer");
    static_cast<ID3D12GraphicsCommandList*>(list)->Close();
    Require(Wrapped(list), "the exe's list through the interposer is wrapped");

    Check(viaInterposer1(device, &list), "CreateCommandList1 through the interposer");
    Require(Wrapped(list), "the exe's CreateCommandList1 list through the interposer is wrapped");

    Check(interposerOwn(device, alloc, &list), "the interposer's own list");
    static_cast<ID3D12GraphicsCommandList*>(list)->Close();
    Require(!Wrapped(list), "the interposer's own list stays native");

    Check(viaPlugin(device, alloc, &list), "a plugin's list");
    static_cast<ID3D12GraphicsCommandList*>(list)->Close();
    Require(!Wrapped(list), "a plugin's list stays native though the exe called it");

    Hooks::Disarm();
    alloc->Release();
    queue->Release();
    device->Release();
    std::printf("lmxxf_interposer_wrap: ok (early lists wrapped %u)\n", Hooks::g_earlyWrappedLists.load());
    return 0;
}
