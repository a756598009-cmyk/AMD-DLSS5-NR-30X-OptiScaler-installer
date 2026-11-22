// D3D12 final-image smoke: a real D3D12 app with OptiScaler.dll loaded as dxgi.dll beside it.
// tools\test-dx12-final-image.cmd stages the folder and runs every scenario.
//
//   dx12_final_image_smoke.exe <nr|fg> [--bgra] [--debug-layer] [--lock <dir>] [--timeout <seconds>]
//                              [--log-level <OptiScaler LogLevel, 2 by default>] [--cpu-ms <game CPU ms per frame>]
//                              [--dump] [--lifecycle] [--nr-off-start] [--bad-runtime] [--r10] [--hdr10]
//                              [--still] [--no-probe]
//                              [--set <Section/Key=Value>] [--expect-2x] [--perf] [--gpu-load <iterations>]
//                              [--borderless] [--waitable <max latency>] [--vsync] [--wheel] [--dinput] [--reflex]
//                              [--hitch]
//
// The parent writes OptiScaler.ini from OptiScaler.template.ini, takes the GPU lock, runs itself as a
// child with a timeout and asserts on the child's results and on OptiScaler.log. The child renders a
// moving scene, posts the menu (Insert), frame generation (End) and Neural Rendering (Numpad 5) keys,
// resizes the swapchain and reads back the frame it presented to see whether NR changed it.
// "nr" runs NR alone; "fg" runs NR with XeFG multi frame generation (OptiFG input).
// --lifecycle creates the swapchain again on a new queue and on the same one, opens the menu's Frame Gen
// page (with "fg" it turns FG off and on there), minimizes and restores the window, and with "nr" moves every
// object to WARP and back. Named rdr.exe, the test gets Red Dead Redemption's quirk: a plain first swapchain,
// XeFG's on the next.
// --nr-off-start starts with NR off in the INI and turns it on with Numpad 5.
// --bad-runtime puts another DLL in dlssnr_amd_pass1.dll's place for the run, so the runtime fails to load.
// --r10 presents R10G10B10A2 frames in the default sRGB colour space, which final-image NR takes as it takes 8-bit
// ones. --hdr10 presents them in HDR10 (ST.2084), which it leaves alone and XeFG gets no guides for.
// --still holds the scene on one frame, so --dump writes the same picture in every run (one run against another).
// --expect-2x expects 2X where frame generation runs, for a provider that allows no MFG.
// --perf measures the presented frame rate with NR and XeFG in each on/off combination, without probes, and
// expects XeFG to raise it. --gpu-load gives the game frame that many shader iterations per pixel, --borderless
// covers the screen as a borderless game does, --waitable makes the swapchain with a frame latency waitable object
// the game waits on each frame (as Red Dead Redemption does), --vsync presents with sync interval 1. A thread reads the
// present count every 0.1 ms to see when each present reached DXGI, and how evenly.
// The test process keeps its timer resolution requests honoured while its window cannot be seen (a screen that is
// off): Windows 11 ignores them otherwise, and XeFG's present thread then sleeps a whole 15.6 ms tick.
// --wheel turns the mouse wheel over the open menu and expects its page to scroll. --dinput reads the mouse through
// DirectInput in exclusive mode from before OptiScaler loads, as Red Dead Redemption does, which keeps the system
// from sending the window any mouse message.
// --reflex drives Reflex through nvapi (sleep and latency markers with the game's own frame numbers), as Red Dead
// Redemption does through Streamline. --hitch gives the game six frames of 110 ms in the middle of play and expects
// frame generation to go on after them.
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static fs::path Here()
{
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

static std::FILE* results = nullptr;
static void Out(const char* format, auto... args)
{
    std::printf(format, args...);
    std::printf("\n");
    if (results)
    {
        std::fprintf(results, format, args...);
        std::fprintf(results, "\n");
        std::fflush(results);
    }
    std::fflush(stdout);
}

[[noreturn]] static void Fail(const char* what, HRESULT hr = S_OK)
{
    Out("child-fail %s hr=0x%08X", what, (unsigned) hr);
    ExitProcess(3);
}
static void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        Fail(what, hr);
}

static std::string ReadAll(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

static int Count(const std::string& text, const std::string& needle)
{
    int n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

// Runtime history resets for a settings change after startup, as a Model scale change makes.
static int SettingsResets(const std::string& runtimeLog)
{
    int resets = 0;
    for (size_t at = runtimeLog.find("AMD history reset: frame="); at != std::string::npos;
         at = runtimeLog.find("AMD history reset: frame=", at + 1))
    {
        const auto line = runtimeLog.substr(at, runtimeLog.find("\n", at) - at);
        resets += std::atoi(line.c_str() + 25) > 30 && line.find("settings=1") != std::string::npos;
    }
    return resets;
}

// ---------------------------------------------------------------- child

// Any crash, on any thread: the faulting stack, symbolized when a PDB sits beside the module.
// Every thread's stack of this process, written by a helper process into <file> under <title>: this one may be
// stopped holding any lock.
static void StacksFromOutside(const wchar_t* file, const std::wstring& title)
{
    wchar_t self[MAX_PATH] {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --stacks " + std::to_wstring(GetCurrentProcessId()) +
                           L" " + file + L" " + title;
    STARTUPINFOW startup { sizeof(startup) };
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        return;
    WaitForSingleObject(process.hProcess, 30000);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
}

static LONG WINAPI Crash(EXCEPTION_POINTERS* pointers)
{
    // One thread reports; another that crashes meanwhile waits for the process to end.
    static std::atomic<bool> reporting = false;
    if (reporting.exchange(true))
        Sleep(INFINITE);
    HMODULE faulting = nullptr;
    wchar_t faultingName[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR) pointers->ExceptionRecord->ExceptionAddress, &faulting))
        GetModuleFileNameW(faulting, faultingName, MAX_PATH);
    Out("child-crash code=0x%08X address=%p in %ls+0x%llx thread=%lu",
        (unsigned) pointers->ExceptionRecord->ExceptionCode, pointers->ExceptionRecord->ExceptionAddress,
        fs::path(faultingName).filename().c_str(),
        (unsigned long long) ((DWORD64) pointers->ExceptionRecord->ExceptionAddress - (DWORD64) faulting),
        GetCurrentThreadId());
    // Every thread at the moment of the crash, into crash-stacks.txt, before anything here can block.
    StacksFromOutside(L"crash-stacks.txt", L"crash");
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitializeW(process, Here().c_str(), TRUE);
    CONTEXT context = *pointers->ContextRecord;
    STACKFRAME64 frame {};
    frame.AddrPC = { context.Rip, 0, AddrModeFlat };
    frame.AddrFrame = { context.Rbp, 0, AddrModeFlat };
    frame.AddrStack = { context.Rsp, 0, AddrModeFlat };
    for (int i = 0; i < 48 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,
                                          nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
         ++i)
    {
        const DWORD64 pc = frame.AddrPC.Offset;
        HMODULE module = nullptr;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR) pc, &module))
            GetModuleFileNameW(module, name, MAX_PATH);
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] {};
        auto symbol = (SYMBOL_INFO*) storage;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        const bool named = SymFromAddr(process, pc, &displacement, symbol);
        IMAGEHLP_LINE64 line { sizeof(line) };
        DWORD column = 0;
        const bool lined = SymGetLineFromAddr64(process, pc, &column, &line);
        Out("child-crash frame %d %ls+0x%llx %s+0x%llx %s:%lu", i, fs::path(name).filename().c_str(),
            pc - (DWORD64) module, named ? symbol->Name : "?", displacement, lined ? line.FileName : "?",
            lined ? line.LineNumber : 0);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

struct Options
{
    std::string scenario;
    std::wstring logLevel = L"2";
    std::wstring modelScale = L"auto";  // AmdModelScale at start
    std::vector<std::wstring> settings; // extra INI values, Section/Key=Value
    double cpuMs = 0;                   // simulated game CPU time per frame, before recording
    bool dump = false;                  // writes each phase's first probe frame as <phase>.bmp
    bool bgra = false;
    bool r10 = false;
    bool hdr10 = false;
    bool still = false;
    bool debugLayer = false;
    bool lifecycle = false;   // swapchain on a new queue and again, the FG page, minimize, restore (nr: WARP device)
    bool nrOffStart = false;  // NR disabled in the INI and turned on with Numpad 5 later
    bool badRuntime = false;  // another DLL as dlssnr_amd_pass1.dll: the runtime fails to load
    bool noProbe = false;     // no probe in the measured phases, so nothing but the game and OptiScaler use the frame
    bool stallStacks = false; // every thread's stack into stall-stacks.txt when a frame takes over 120 ms
    bool expect2x = false;    // XeFG may give one generated frame only (UnlockMFG=false): 2X, not 3X
    bool perf = false;        // presented frame rate in every NR/XeFG combination
    int gpuLoad = 0;          // shader iterations per pixel of the game frame
    bool borderless = false;  // a popup window over the whole primary screen
    int waitable = 0;         // maximum frame latency of a waitable swapchain the game waits on, 0 for none
    bool vsync = false;
    bool wheel = false;  // the mouse wheel over the open menu
    bool dinput = false; // the mouse read through DirectInput in exclusive mode
    bool reflex = false; // Reflex sleep and latency markers through nvapi, with the game's own frame numbers
    bool hitch = false;  // a few very long frames while frame generation runs, then whether it still generates
};

// The frame in progress and when it began, for the stall sampler.
static std::atomic<ULONGLONG> frameStarted = 0;
static std::atomic<UINT64> frameNumber = 0;

struct App
{
    Options options;
    HWND window = nullptr;
    PFN_D3D12_CREATE_DEVICE createDevice = nullptr;
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serializeRoot = nullptr;
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12InfoQueue> info;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    static constexpr UINT bufferCount = 3;
    ComPtr<ID3D12CommandAllocator> allocators[bufferCount];
    UINT64 allocatorFence[bufferCount] {};
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> reference, readBack, readReference;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT width = 1280, height = 720;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT swapFlags = 0;
    UINT64 frame = 0;
    // Seconds the game thread spent inside Present, and waiting before it could record, since the phase began.
    double inPresent = 0, inWait = 0;

    void Wait(UINT64 value)
    {
        if (fence->GetCompletedValue() >= value)
            return;
        Check(fence->SetEventOnCompletion(value, fenceEvent), "SetEventOnCompletion");
        if (WaitForSingleObject(fenceEvent, 10000) != WAIT_OBJECT_0)
            Fail("GPU fence wait timed out", device->GetDeviceRemovedReason());
    }
    void Idle()
    {
        Check(queue->Signal(fence.Get(), ++fenceValue), "Signal");
        Wait(fenceValue);
    }

    // Every object on the hardware adapter, or on WARP; called again it builds them anew, as a game that
    // recreates its device.
    void CreateDevice(bool warp = false)
    {
        // Through the proxy: the module named dxgi.dll in this folder is OptiScaler.
        HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        if (!dxgi)
            Fail("dxgi.dll did not load");
        wchar_t loaded[MAX_PATH] {};
        GetModuleFileNameW(dxgi, loaded, MAX_PATH);
        if (fs::path(loaded).parent_path() != Here())
            Fail("dxgi.dll is not the OptiScaler proxy beside the test");
        HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
        if (!d3d12)
            Fail("d3d12.dll did not load");
        // Resolved at run time: a static import would pull the proxy into the parent process too.
        createDevice = (PFN_D3D12_CREATE_DEVICE) GetProcAddress(d3d12, "D3D12CreateDevice");
        serializeRoot = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE) GetProcAddress(d3d12, "D3D12SerializeRootSignature");
        auto getDebug = (PFN_D3D12_GET_DEBUG_INTERFACE) GetProcAddress(d3d12, "D3D12GetDebugInterface");
        auto createFactory = (HRESULT(WINAPI*)(UINT, REFIID, void**)) GetProcAddress(dxgi, "CreateDXGIFactory2");
        if (!createDevice || !serializeRoot || !getDebug || !createFactory)
            Fail("missing D3D12/DXGI entry points");
        if (options.debugLayer && !device)
        {
            ComPtr<ID3D12Debug> debug;
            if (FAILED(getDebug(IID_PPV_ARGS(&debug))))
                Fail("D3D12 debug layer unavailable");
            debug->EnableDebugLayer();
            Out("debug-layer on");
        }
        Check(createFactory(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        ComPtr<IDXGIAdapter1> adapter;
        if (warp)
            Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
        else
            Check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)),
                  "EnumAdapterByGpuPreference");
        DXGI_ADAPTER_DESC1 adapterDesc {};
        adapter->GetDesc1(&adapterDesc);
        Out("adapter %ls", adapterDesc.Description);
        ComPtr<ID3D12Device> created;
        Check(createDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&created)), "D3D12CreateDevice");
        Out("objects device=%p", created.Get());
        device = created;
        if (options.debugLayer && SUCCEEDED(device.As(&info)))
        {
            info->SetMessageCountLimit(4096);
            D3D12_MESSAGE_SEVERITY allow[] = { D3D12_MESSAGE_SEVERITY_CORRUPTION, D3D12_MESSAGE_SEVERITY_ERROR };
            D3D12_INFO_QUEUE_FILTER filter {};
            filter.AllowList.NumSeverities = 2;
            filter.AllowList.pSeverityList = allow;
            info->PushStorageFilter(&filter);
        }
        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

        format = options.r10 || options.hdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM
                 : options.bgra               ? DXGI_FORMAT_B8G8R8A8_UNORM
                                              : DXGI_FORMAT_R8G8B8A8_UNORM;
        BOOL tearing = FALSE;
        factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
        swapFlags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        if (options.waitable)
            swapFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Width = width;
        desc.Height = height;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = bufferCount;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = swapFlags;
        ComPtr<IDXGISwapChain1> swap1;
        Check(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &swap1),
              "CreateSwapChainForHwnd");
        Check(swap1.As(&swapchain), "IDXGISwapChain3");
        if (options.hdr10)
            Check(swapchain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020), "SetColorSpace1 HDR10");
        Waitable();
        Out("objects queue=%p", queue.Get());
        LogBuffers();
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        fenceValue = 0;
        for (auto& value : allocatorFence)
            value = 0;
        if (!fenceEvent)
            fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (auto& allocator : allocators)
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                  "CreateCommandAllocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr,
                                        IID_PPV_ARGS(&list)),
              "CreateCommandList");
        Check(list->Close(), "Close");
        D3D12_DESCRIPTOR_HEAP_DESC heap {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = bufferCount + 1;
        Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap");
        CreatePipeline();
        CreateProbe();
    }

    // Reflex as a game drives it through NvAPI, which Red Dead Redemption does through Streamline's sl.reflex: low
    // latency mode, a sleep at the start of each frame and the frame's latency markers, with the game's own frame
    // numbers. On an AMD GPU OptiScaler answers as nvapi64.dll.
    using NvQuery = void*(__cdecl*) (unsigned);
    int(__cdecl* reflexSleep)(IUnknown*) = nullptr;
    int(__cdecl* reflexMarker)(IUnknown*, void*) = nullptr;
    UINT64 reflexFrame = 0;
    void CreateReflex()
    {
        const HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
        const auto query = nvapi ? (NvQuery) GetProcAddress(nvapi, "nvapi_QueryInterface") : nullptr;
        if (!query)
            Fail("no nvapi64.dll with nvapi_QueryInterface");
        const auto initialize = (int(__cdecl*)()) query(0x0150e828);
        const auto setSleepMode = (int(__cdecl*)(IUnknown*, void*)) query(0xac1ca9e0);
        reflexSleep = (int(__cdecl*)(IUnknown*)) query(0x852cd1d2);
        reflexMarker = (int(__cdecl*)(IUnknown*, void*)) query(0xd9984c05);
        if (!initialize || !setSleepMode || !reflexSleep || !reflexMarker)
            Fail("nvapi64.dll has no Reflex");
        initialize();
        struct
        {
            UINT32 version;
            UINT8 lowLatency, boost;
            UINT32 minimumIntervalUs;
            UINT8 useMarkers, minQueueTime, reserved[30];
        } mode {};
        mode.version = sizeof(mode) | (1 << 16);
        mode.lowLatency = mode.useMarkers = 1;
        wchar_t name[MAX_PATH] {};
        GetModuleFileNameW(nvapi, name, MAX_PATH);
        Out("reflex nvapi=%ls sleep-mode=%d", fs::path(name).filename().c_str(), setSleepMode(device.Get(), &mode));
        // The game counts its frames from its own start, well ahead of anything OptiScaler counts.
        reflexFrame = 100000;
    }
    void ReflexMarker(int type)
    {
        if (!reflexMarker)
            return;
        struct
        {
            UINT32 version;
            UINT64 frameID;
            int markerType;
            UINT64 reserved0;
            UINT8 reserved[56];
        } marker {};
        marker.version = sizeof(marker) | (1 << 16);
        marker.frameID = reflexFrame;
        marker.markerType = type;
        reflexMarker(device.Get(), &marker);
    }

    // The game's own mouse, as Red Dead Redemption reads it: DirectInput, exclusive while in front.
    ComPtr<IDirectInput8W> directInput;
    ComPtr<IDirectInputDevice8W> mouse;
    void CreateMouse()
    {
        Check(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                 (void**) directInput.GetAddressOf(), nullptr),
              "DirectInput8Create");
        Check(directInput->CreateDevice(GUID_SysMouse, &mouse, nullptr), "DirectInput CreateDevice");
        Check(mouse->SetDataFormat(&c_dfDIMouse2), "DirectInput SetDataFormat");
        Check(mouse->SetCooperativeLevel(window, DISCL_EXCLUSIVE | DISCL_FOREGROUND),
              "DirectInput SetCooperativeLevel");
        Out("dinput mouse exclusive");
    }
    void ReadMouse()
    {
        if (!mouse)
            return;
        DIMOUSESTATE2 state {};
        mouse->Acquire();
        mouse->GetDeviceState(sizeof(state), &state);
    }

    HANDLE latency = nullptr;
    void Waitable()
    {
        if (latency)
            CloseHandle(latency);
        latency = nullptr;
        if (!options.waitable)
            return;
        Check(swapchain->SetMaximumFrameLatency(UINT(options.waitable)), "SetMaximumFrameLatency");
        latency = swapchain->GetFrameLatencyWaitableObject();
        if (!latency)
            Fail("no frame latency waitable object");
    }

    // Addresses to tell the app's own objects apart in debug-layer messages.
    void LogBuffers()
    {
        for (UINT i = 0; i < bufferCount; ++i)
        {
            ComPtr<ID3D12Resource> buffer;
            if (SUCCEEDED(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer))))
                Out("objects backbuffer%u=%p", i, buffer.Get());
        }
    }

    void CreatePipeline()
    {
        D3D12_ROOT_PARAMETER param {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.Num32BitValues = 4;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc {};
        rootDesc.NumParameters = 1;
        rootDesc.pParameters = &param;
        ComPtr<ID3DBlob> blob, errors;
        Check(serializeRoot(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "D3D12SerializeRootSignature");
        Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
              "CreateRootSignature");
        static const char vs[] = "float4 main(uint id:SV_VertexID):SV_Position{"
                                 "float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
        // A camera pan over a checkerboard, a disc that moves on its own and a static HUD bar.
        // n iterations per pixel make the frame as heavy as asked; they change it by well under one level.
        static const char ps[] = "cbuffer C:register(b0){float t;float w;float h;float n;};"
                                 "float4 main(float4 p:SV_Position):SV_Target{"
                                 "float2 q=p.xy+float2(t*120.0,t*40.0);"
                                 "float a=0;for(int i=0;i<int(n);i++)a+=sin(q.x*0.001+q.y*0.002+i*0.37);"
                                 "float c=((int(floor(q.x/64.0))+int(floor(q.y/64.0)))&1)?0.8:0.2;"
                                 "float3 col=float3(c,c*0.9+0.1*p.x/w,c*0.8+0.2*p.y/h);"
                                 "float2 m=float2(w*(0.5+0.3*sin(t)),h*(0.5+0.25*cos(t*0.7)));"
                                 "if(length(p.xy-m)<h*0.12)col=float3(0.9,0.3,0.2);"
                                 "if(p.y>h-40.0)col=float3(0.1,0.1,0.12);"
                                 "return float4(col+a*1e-6,1);}";
        ComPtr<ID3DBlob> vertex, pixel;
        Check(D3DCompile(vs, sizeof(vs) - 1, "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vertex, &errors),
              "D3DCompile vs");
        Check(D3DCompile(ps, sizeof(ps) - 1, "ps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pixel, &errors),
              "D3DCompile ps");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso {};
        pso.pRootSignature = root.Get();
        pso.VS = { vertex->GetBufferPointer(), vertex->GetBufferSize() };
        pso.PS = { pixel->GetBufferPointer(), pixel->GetBufferSize() };
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = format;
        pso.SampleDesc.Count = 1;
        Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)), "CreateGraphicsPipelineState");
    }

    // A reference copy of the frame rendered without OptiScaler in between, and two readback buffers.
    void CreateProbe()
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        reference.Reset();
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                              nullptr, IID_PPV_ARGS(&reference)),
              "reference texture");
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        readBack.Reset();
        readReference.Reset();
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&readBack)),
              "readback");
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&readReference)),
              "readback reference");
        auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(bufferCount) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(reference.Get(), nullptr, rtv);
    }

    static void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                        D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER v {};
        v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        v.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
        c->ResourceBarrier(1, &v);
    }

    void Draw(D3D12_CPU_DESCRIPTOR_HANDLE rtv)
    {
        const float constants[4] = { options.still ? 1.0f : float(frame) / 60.0f, float(width), float(height),
                                     float(options.gpuLoad) };
        D3D12_VIEWPORT viewport { 0, 0, float(width), float(height), 0, 1 };
        D3D12_RECT scissor { 0, 0, LONG(width), LONG(height) };
        list->SetGraphicsRootSignature(root.Get());
        list->SetPipelineState(pipeline.Get());
        list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
    }

    struct ProbeResult
    {
        double changed = -1; // fraction of pixels whose colour differs by more than 2 levels
        double mean = 0;     // mean absolute difference per channel, in levels
    };

    // Renders one frame; with probe set, reads back what was presented and the reference.
    HRESULT Frame(ProbeResult* probe)
    {
        frameStarted = GetTickCount64();
        frameNumber = frame;
        const HRESULT presented = RenderFrame(probe);
        frameStarted = 0;
        return presented;
    }

    HRESULT RenderFrame(ProbeResult* probe)
    {
        using Clock = std::chrono::steady_clock;
        const auto waitStart = Clock::now();
        if (latency)
            WaitForSingleObjectEx(latency, 1000, TRUE);
        if (reflexSleep)
        {
            ++reflexFrame;
            reflexSleep(device.Get());
            ReflexMarker(0); // SIMULATION_START
        }
        ReadMouse();
        if (options.cpuMs > 0)
        {
            LARGE_INTEGER now, end, rate;
            QueryPerformanceFrequency(&rate);
            QueryPerformanceCounter(&end);
            end.QuadPart += LONGLONG(options.cpuMs * rate.QuadPart / 1000);
            do
                QueryPerformanceCounter(&now);
            while (now.QuadPart < end.QuadPart);
        }
        ReflexMarker(1); // SIMULATION_END
        const UINT index = swapchain->GetCurrentBackBufferIndex();
        Wait(allocatorFence[index]);
        inWait += std::chrono::duration<double>(Clock::now() - waitStart).count();
        ComPtr<ID3D12Resource> back;
        Check(swapchain->GetBuffer(index, IID_PPV_ARGS(&back)), "GetBuffer");
        auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        const auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto backRtv = rtv;
        backRtv.ptr += SIZE_T(index) * step;
        device->CreateRenderTargetView(back.Get(), nullptr, backRtv);
        Check(allocators[index]->Reset(), "allocator Reset");
        Check(list->Reset(allocators[index].Get(), nullptr), "list Reset");
        Barrier(list.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Draw(backRtv);
        Barrier(list.Get(), back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        if (probe)
        {
            auto referenceRtv = rtv;
            referenceRtv.ptr += SIZE_T(bufferCount) * step;
            Barrier(list.Get(), reference.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Draw(referenceRtv);
            Barrier(list.Get(), reference.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        Check(list->Close(), "list Close");
        ID3D12CommandList* lists[] = { list.Get() };
        ReflexMarker(2); // RENDERSUBMIT_START
        queue->ExecuteCommandLists(1, lists);
        ReflexMarker(3); // RENDERSUBMIT_END
        ReflexMarker(4); // PRESENT_START
        const auto presentStart = Clock::now();
        const HRESULT presented = options.vsync ? swapchain->Present(1, 0)
                                                : swapchain->Present(0, (swapFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
                                                                            ? DXGI_PRESENT_ALLOW_TEARING
                                                                            : 0);
        inPresent += std::chrono::duration<double>(Clock::now() - presentStart).count();
        ReflexMarker(5); // PRESENT_END
        if (probe && SUCCEEDED(presented))
        {
            // Same queue, after Present: sees what the game frame became on its way to the screen.
            Idle();
            Check(allocators[index]->Reset(), "probe allocator Reset");
            Check(list->Reset(allocators[index].Get(), nullptr), "probe list Reset");
            Barrier(list.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst { readBack.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION src { back.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            dst.pResource = readReference.Get();
            src.pResource = reference.Get();
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Barrier(list.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            Check(list->Close(), "probe list Close");
            queue->ExecuteCommandLists(1, lists);
            Idle();
            *probe = Compare();
        }
        Check(queue->Signal(fence.Get(), ++fenceValue), "Signal");
        allocatorFence[index] = fenceValue;
        ++frame;
        return presented;
    }

    // The presented frame as a top-down 32-bit BMP.
    void Dump(const char* phase)
    {
        void* data = nullptr;
        Check(readBack->Map(0, nullptr, &data), "Map dump");
        BITMAPFILEHEADER file {};
        BITMAPINFOHEADER info {};
        info.biSize = sizeof(info);
        info.biWidth = LONG(width);
        info.biHeight = -LONG(height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + width * height * 4;
        std::ofstream out(Here() / (std::string(phase) + ".bmp"), std::ios::binary);
        out.write((const char*) &file, sizeof(file));
        out.write((const char*) &info, sizeof(info));
        std::vector<unsigned char> row(width * 4);
        for (UINT y = 0; y < height; ++y)
        {
            std::memcpy(row.data(), (const unsigned char*) data + UINT64(y) * footprint.Footprint.RowPitch, row.size());
            if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
                for (UINT x = 0; x < width; ++x)
                {
                    const auto rgb = Levels(row.data(), x);
                    row[x * 4] = rgb[2];
                    row[x * 4 + 1] = rgb[1];
                    row[x * 4 + 2] = rgb[0];
                    row[x * 4 + 3] = 255;
                }
            else if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
                for (UINT x = 0; x < width; ++x)
                    std::swap(row[x * 4], row[x * 4 + 2]);
            out.write((const char*) row.data(), row.size());
        }
        readBack->Unmap(0, nullptr);
    }

    // A pixel's three colour channels in 8-bit levels, in the order the format stores them (R, G, B for 10-bit).
    std::array<unsigned char, 3> Levels(const unsigned char* row, UINT x) const
    {
        if (format != DXGI_FORMAT_R10G10B10A2_UNORM)
            return { row[x * 4], row[x * 4 + 1], row[x * 4 + 2] };
        UINT v = 0;
        std::memcpy(&v, row + x * 4, 4);
        const auto level = [](UINT c) { return (unsigned char) ((c * 255 + 511) / 1023); };
        return { level(v & 1023), level(v >> 10 & 1023), level(v >> 20 & 1023) };
    }

    // Over the frame right of 35% of its width: a driver performance overlay may draw on the left.
    ProbeResult Compare()
    {
        const UINT left = width * 35 / 100;
        void* a = nullptr;
        void* b = nullptr;
        Check(readBack->Map(0, nullptr, &a), "Map");
        Check(readReference->Map(0, nullptr, &b), "Map reference");
        UINT64 changed = 0, sum = 0;
        for (UINT y = 0; y < height; ++y)
        {
            auto rowA = (const unsigned char*) a + UINT64(y) * footprint.Footprint.RowPitch;
            auto rowB = (const unsigned char*) b + UINT64(y) * footprint.Footprint.RowPitch;
            for (UINT x = left; x < width; ++x)
            {
                int worst = 0;
                const auto pa = Levels(rowA, x), pb = Levels(rowB, x);
                for (int c = 0; c < 3; ++c)
                {
                    const int d = std::abs(int(pa[c]) - int(pb[c]));
                    worst = std::max(worst, d);
                    sum += d;
                }
                changed += worst > 2;
            }
        }
        readBack->Unmap(0, nullptr);
        readReference->Unmap(0, nullptr);
        const double pixels = double(width - left) * height;
        return { changed / pixels, sum / (pixels * 3) };
    }

    void Resize(UINT w, UINT h)
    {
        Idle();
        RECT rect { 0, 0, LONG(w), LONG(h) };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER);
        Check(swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapFlags), "ResizeBuffers");
        LogBuffers();
        width = w;
        height = h;
        CreateProbe();
        for (auto& value : allocatorFence)
            value = 0;
    }

    // Releases the swapchain and creates another on the same window, on a new queue or the same one, as games
    // do on a settings change. Red Dead Redemption's frame generation swapchain is such a second one.
    void Recreate(bool newQueue)
    {
        Idle();
        swapchain.Reset();
        if (newQueue)
        {
            D3D12_COMMAND_QUEUE_DESC queueDesc {};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            queue.Reset();
            Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue again");
            Out("objects queue=%p", queue.Get());
        }
        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Width = width;
        desc.Height = height;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = bufferCount;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = swapFlags;
        ComPtr<IDXGISwapChain1> swap1;
        Check(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &swap1),
              "CreateSwapChainForHwnd again");
        Check(swap1.As(&swapchain), "IDXGISwapChain3 again");
        Waitable();
        LogBuffers();
        for (auto& value : allocatorFence)
            value = 0;
    }

    void RecreateDevice(bool warp)
    {
        Idle();
        swapchain.Reset();
        CreateDevice(warp);
    }

    // Holds the key for about as long as a person does, rendering meanwhile.
    void PressKey(UINT vk, bool extended, bool (*pump)())
    {
        const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        const LPARAM down = 1 | (LPARAM(scan) << 16) | (extended ? 1 << 24 : 0);
        const LPARAM up = down | (1u << 30) | (1u << 31);
        PostMessageW(window, WM_KEYDOWN, vk, down);
        const auto until = GetTickCount64() + 120;
        while (GetTickCount64() < until)
        {
            pump();
            const HRESULT hr = Frame(nullptr);
            if (FAILED(hr))
                Fail("Present while a key is held", hr);
        }
        PostMessageW(window, WM_KEYUP, vk, up);
    }

    void Render(double seconds, bool (*pump)())
    {
        const auto until = GetTickCount64() + ULONGLONG(seconds * 1000);
        while (GetTickCount64() < until)
        {
            pump();
            const HRESULT hr = Frame(nullptr);
            if (FAILED(hr))
                Fail("Present while driving the menu", hr);
        }
    }

    // A real cursor move and click, as the menu polls both. Places are those of a 1280x720 window at the
    // default menu scale, where the menu opens centred with its pages down the left.
    // A real cursor move and click, made by a helper process: while the menu is open OptiScaler keeps the game's
    // own SetCursorPos and SendInput calls from moving the cursor, as it should. A button acts on release, so the
    // press is held past the menu's longest stall with XeFG (about 210 ms).
    void Click(LONG x, LONG y, bool (*pump)(), double hold = 0.2)
    {
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        POINT point { x, y };
        ClientToScreen(window, &point);
        wchar_t self[MAX_PATH] {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(self) + L"\" --click " + std::to_wstring(point.x) + L" " +
                               std::to_wstring(point.y) + L" " + std::to_wstring(DWORD(hold * 1000));
        STARTUPINFOW startup { sizeof(startup) };
        PROCESS_INFORMATION process {};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
            Fail("click helper did not start");
        while (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
        {
            pump();
            const HRESULT hr = Frame(nullptr);
            if (FAILED(hr))
                Fail("Present while clicking", hr);
        }
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        Render(0.5, pump);
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    // Turns the wheel down over a point of the window, from a helper process as a real mouse does.
    void Wheel(LONG x, LONG y, int notches, bool (*pump)())
    {
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        POINT point { x, y };
        ClientToScreen(window, &point);
        wchar_t self[MAX_PATH] {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(self) + L"\" --wheel " + std::to_wstring(point.x) + L" " +
                               std::to_wstring(point.y) + L" " + std::to_wstring(notches);
        STARTUPINFOW startup { sizeof(startup) };
        PROCESS_INFORMATION process {};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
            Fail("wheel helper did not start");
        while (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
        {
            pump();
            if (FAILED(Frame(nullptr)))
                Fail("Present while turning the wheel");
        }
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        Render(0.5, pump);
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    // Opens the menu, turns the wheel over its page and reports how much of the page moved. A window that is not in
    // front (another program took the foreground) is brought back and the wheel turned again.
    void ScrollMenu(bool (*pump)())
    {
        PressKey(VK_INSERT, true, pump);
        Render(1.0, pump);
        double scrolled = 0;
        for (int attempt = 0; attempt < 3 && scrolled < 0.1; ++attempt)
        {
            // A window of another program may hold the foreground; joining its input queue lets this one take it.
            const DWORD other = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
            AttachThreadInput(other, GetCurrentThreadId(), TRUE);
            SetForegroundWindow(window);
            AttachThreadInput(other, GetCurrentThreadId(), FALSE);
            Render(0.3, pump);
            const bool front = GetForegroundWindow() == window;
            const auto before = Capture("wheel-before", pump);
            Wheel(650, 420, 6, pump);
            const auto after = Capture("wheel-after", pump);
            size_t differ = 0, total = 0;
            for (LONG y = 290; y < 510 && before.size() == after.size() && after.size() >= size_t(width) * 510; ++y)
                for (LONG x = 360; x < 940; ++x, ++total)
                    differ += before[y * width + x] != after[y * width + x];
            scrolled = total ? double(differ) / total : 0.0;
            Out("wheel attempt %d foreground=%d scrolled=%.3f", attempt + 1, front ? 1 : 0, scrolled);
        }
        Out("wheel scrolled=%.3f", scrolled);
        PressKey(VK_INSERT, true, pump);
    }

    // What the window shows on screen, the menu over frame generation's output included, as <name>.bmp with
    // --dump. The window is held in front while it renders a few frames, then its area is read from the screen.
    std::vector<uint32_t> Capture(const char* name, bool (*pump)())
    {
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        Render(0.3, pump);
        RECT client {};
        GetClientRect(window, &client);
        POINT origin {};
        ClientToScreen(window, &origin);
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO info {};
        info.bmiHeader = { sizeof(BITMAPINFOHEADER), client.right, -client.bottom, 1, 32 };
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        auto previous = SelectObject(memory, bitmap);
        BitBlt(memory, 0, 0, client.right, client.bottom, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        std::vector<uint32_t> pixels((const uint32_t*) bits, (const uint32_t*) bits + client.right * client.bottom);
        if (options.dump)
        {
            BITMAPFILEHEADER file { 0x4D42 };
            file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
            std::ofstream out(Here() / (std::string(name) + ".bmp"), std::ios::binary);
            out.write((const char*) &file, sizeof(file));
            out.write((const char*) &info.bmiHeader, sizeof(BITMAPINFOHEADER));
            out.write((const char*) bits, std::streamsize(client.right) * client.bottom * 4);
        }
        SelectObject(memory, previous);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return pixels;
    }

    // Opens the menu, then its Frame Gen page from the side bar, and leaves it open. The page's content area
    // is compared before and after the click to tell that the page changed; a missed click is tried again.
    void OpenFgPage(bool (*pump)())
    {
        PressKey(VK_INSERT, true, pump);
        Render(1.0, pump);
        const auto before = Capture("menu-nr-page", pump);
        double switched = 0;
        for (int attempt = 0; attempt < 3 && switched < 0.1; ++attempt)
        {
            Click(337, 242, pump, 0.6);
            const auto after = Capture("menu-fg-page", pump);
            size_t differ = 0, total = 0;
            for (LONG y = 290; y < 510 && before.size() == after.size() && after.size() >= size_t(width) * 510; ++y)
                for (LONG x = 360; x < 940; ++x, ++total)
                    differ += before[y * width + x] != after[y * width + x];
            switched = total ? double(differ) / total : 0.0;
            Out("fg-page attempt %d switched=%.3f", attempt + 1, switched);
        }
        Out("fg-page switched=%.3f", switched);
    }

    // Back to the NR page, where the menu opens next time, and the menu closed.
    void CloseFgPage(bool (*pump)())
    {
        Click(337, 201, pump, 0.6);
        PressKey(VK_INSERT, true, pump);
    }

    // Sets the Model scale slider on the NR page, where the menu opens, a fifth of the way along (about 0.6)
    // and closes the menu.
    void ChangeModelScale(bool (*pump)())
    {
        // A click the menu misses (the window not yet in front, say) is tried once more.
        const int before = SettingsResets(ReadAll(Here() / L"amd_presr.log"));
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            PressKey(VK_INSERT, true, pump);
            Render(1.0, pump);
            if (options.dump)
                Capture("scale-menu", pump);
            Click(372 + 126 / 5, 395, pump);
            PressKey(VK_INSERT, true, pump);
            Render(0.5, pump);
            if (SettingsResets(ReadAll(Here() / L"amd_presr.log")) > before)
                break;
            Out("scale-click missed, attempt %d", attempt + 1);
        }
    }

    void DebugMessages(const char* phase)
    {
        if (!info)
            return;
        const UINT64 count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
        std::map<std::string, int> unique;
        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::vector<char> storage(size);
            auto message = (D3D12_MESSAGE*) storage.data();
            if (SUCCEEDED(info->GetMessage(i, message, &size)))
                ++unique[std::string(message->pDescription, message->DescriptionByteLength)];
        }
        info->ClearStoredMessages();
        Out("debug %s errors=%llu unique=%zu", phase, count, unique.size());
        int shown = 0;
        for (auto& [text, n] : unique)
            if (shown++ < 12)
                Out("debug-message %s x%d %s", phase, n, text.substr(0, 400).c_str());
    }
};

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CLOSE)
        return 0;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
            return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

static int Child(const Options& options)
{
    results = _wfopen((Here() / L"dx12_smoke_result.txt").c_str(), L"w");
    SetUnhandledExceptionFilter(Crash);
    // Windows 11 honours a process's timer resolution only while its window can be seen; the test machine's screen may
    // be off. This keeps it honoured, as for a game on screen.
    PROCESS_POWER_THROTTLING_STATE throttling { PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                                PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0 };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
    App app;
    app.options = options;
    WNDCLASSW type {};
    type.lpfnWndProc = WindowProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"OptiDx12FinalImageSmoke";
    type.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&type);
    if (options.borderless)
    {
        app.width = UINT(GetSystemMetrics(SM_CXSCREEN));
        app.height = UINT(GetSystemMetrics(SM_CYSCREEN));
    }
    RECT rect { 0, 0, LONG(app.width), LONG(app.height) };
    const DWORD style = options.borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rect, style, FALSE);
    app.window = CreateWindowW(type.lpszClassName, L"Opti D3D12 final-image smoke", style, options.borderless ? 0 : 40,
                               options.borderless ? 0 : 40, rect.right - rect.left, rect.bottom - rect.top, nullptr,
                               nullptr, type.hInstance, nullptr);
    if (!app.window)
        Fail("CreateWindow");
    ShowWindow(app.window, SW_SHOW);
    SetForegroundWindow(app.window);
    // Before the proxy loads, as a game that sets up its input first.
    if (options.dinput)
        app.CreateMouse();
    app.CreateDevice();
    if (options.reflex)
        app.CreateReflex();
    // A helper process writes the stacks: this one may be stalled holding any lock.
    if (options.stallStacks)
        std::thread(
            []
            {
                UINT64 sampled = UINT64_MAX;
                for (int samples = 0; samples < 6;)
                {
                    Sleep(10);
                    const ULONGLONG started = frameStarted;
                    const UINT64 number = frameNumber;
                    if (!started || GetTickCount64() - started < 120 || sampled == number)
                        continue;
                    sampled = number;
                    ++samples;
                    StacksFromOutside(L"stall-stacks.txt", L"stall-at-frame-" + std::to_wstring(number));
                    Out("stall frame=%llu ms=%llu stacks in stall-stacks.txt", number, GetTickCount64() - started);
                }
            })
            .detach();

    using Clock = std::chrono::steady_clock;
    const auto Seconds = [](Clock::time_point since)
    { return std::chrono::duration<double>(Clock::now() - since).count(); };

    // Warm-up: the runtime loads, optical flow settles, XeFG starts. Ends when a probe sees NR, or after a few
    // seconds when NR starts off or cannot load.
    auto start = Clock::now();
    auto lastProbe = start;
    bool warm = false;
    while (Seconds(start) < (options.nrOffStart || options.badRuntime || options.hdr10 ? 6 : 60) && !warm)
    {
        if (!Pump())
            break;
        App::ProbeResult probe;
        const bool probeNow = Seconds(lastProbe) > 1.0;
        const HRESULT hr = app.Frame(probeNow ? &probe : nullptr);
        if (FAILED(hr))
            Fail("Present during warm-up", hr);
        if (probeNow)
        {
            lastProbe = Clock::now();
            warm = probe.changed > 0.3;
        }
    }
    Out("warmup seconds=%.1f nr=%d frames=%llu", Seconds(start), warm ? 1 : 0, app.frame);
    // Let XeFG run a few seconds on top of a working NR before measuring.
    const auto settle = Clock::now();
    while (Seconds(settle) < 3)
    {
        Pump();
        if (FAILED(app.Frame(nullptr)))
            Fail("Present while settling");
    }
    app.DebugMessages("warmup");

    enum Action
    {
        None,
        Resize = -1,
        ModelScale = -2,
        NewQueue = -3,  // swapchain again on a new queue
        SameQueue = -4, // swapchain again on the same queue
        FgPage = -5,    // menu open on the Frame Gen page
        FgPageOut = -6, // back to the NR page, menu closed
        Minimize = -7,
        Restore = -8,
        Warp = -9,      // every object again on WARP
        Hardware = -10, // every object again on the hardware adapter
        FgActive = -11, // the XeFG "Active" box on the Frame Gen page
        Wheel = -12,    // the mouse wheel over the open menu
        Hitch = -13,    // six game frames of 110 ms each, as a game that loads in the middle of play
    };
    struct Step
    {
        const char* phase; // measured after the action, for `seconds`
        int key;           // key to press first, or an Action
        double delay;      // seconds to wait after the action before measuring
        double seconds;
    };
    std::vector<Step> steps;
    if (options.wheel)
        steps = { { "steady", None, 0, 2 }, { "wheel", Wheel, 0.5, 2 } };
    else if (options.hitch)
        steps = { { "steady", None, 3, 4 }, { "after-hitch", Hitch, 1, 5 }, { "later", None, 0, 5 } };
    // Every NR and XeFG combination, from NR with XeFG as the INI starts, once XeFG's first seconds are over.
    else if (options.perf)
        steps = { { "nr-fg", None, 5, 5 },
                  { "nr-only", VK_END, 3, 5 },
                  { "none", VK_NUMPAD5, 3, 5 },
                  { "fg-only", VK_END, 3, 5 },
                  { "nr-fg-again", VK_NUMPAD5, 3, 5 } };
    else if (options.nrOffStart)
        steps = { { "guides-only", None, 0, 4 }, { "nr-late-on", VK_NUMPAD5, 4, 4 } };
    if (options.lifecycle)
    {
        steps.insert(steps.end(), {
                                      { "steady", None, 0, 4 },
                                      { "new-queue", NewQueue, 4, 4 },
                                      { "same-queue", SameQueue, 4, 4 },
                                      { "fg-page", FgPage, 1, 4 },
                                  });
        // FG off and on again from the page's own "Active" box.
        if (options.scenario == "fg")
            steps.insert(steps.end(), { { "fg-page-off", FgActive, 1.5, 3 }, { "fg-page-on", FgActive, 2.5, 4 } });
        steps.insert(steps.end(), {
                                      { "fg-page-closed", FgPageOut, 1, 3 },
                                      { "minimized", Minimize, 1, 3 },
                                      { "restored", Restore, 3, 4 },
                                  });
        if (options.scenario == "nr")
            steps.insert(steps.end(), { { "warp", Warp, 3, 3 }, { "hardware", Hardware, 4, 4 } });
    }
    else if (!options.nrOffStart && !options.perf && !options.wheel && !options.hitch)
        steps = {
            { "steady", None, 0, 4 },           { "menu-open", VK_INSERT, 1, 4 },
            { "menu-closed", VK_INSERT, 1, 3 }, { "fg-off", VK_END, 1.5, 3 },
            { "fg-on", VK_END, 2.5, 4 },        { "nr-off", VK_NUMPAD5, 1.5, 3 },
            { "nr-on", VK_NUMPAD5, 2.5, 4 },    { "scale", ModelScale, 2.5, 4 },
            { "resized", Resize, 3, 4 },        { "menu-resized", VK_INSERT, 1, 3 },
            { "end", VK_INSERT, 1, 2 },
        };
    for (const auto& step : steps)
    {
        if (step.key > 0)
            app.PressKey(step.key, step.key == VK_INSERT || step.key == VK_END, Pump);
        else if (step.key == Resize)
            app.Resize(1600, 900);
        else if (step.key == ModelScale)
            app.ChangeModelScale(Pump);
        else if (step.key == NewQueue || step.key == SameQueue)
            app.Recreate(step.key == NewQueue);
        else if (step.key == FgPage)
            app.OpenFgPage(Pump);
        else if (step.key == FgPageOut)
            app.CloseFgPage(Pump);
        else if (step.key == FgActive)
            app.Click(360, 350, Pump, 0.6);
        else if (step.key == Wheel)
            app.ScrollMenu(Pump);
        else if (step.key == Hitch)
            for (int i = 0; i < 6; ++i)
            {
                Pump();
                Sleep(110);
                if (FAILED(app.Frame(nullptr)))
                    Fail("hitch");
            }
        else if (step.key == Minimize)
            ShowWindow(app.window, SW_MINIMIZE);
        else if (step.key == Restore)
        {
            ShowWindow(app.window, SW_RESTORE);
            SetForegroundWindow(app.window);
        }
        else if (step.key == Warp || step.key == Hardware)
            app.RecreateDevice(step.key == Warp);
        auto begin = Clock::now();
        while (Seconds(begin) < step.delay)
        {
            Pump();
            const HRESULT hr = app.Frame(nullptr);
            if (FAILED(hr))
                Fail(step.phase, hr);
        }
        UINT presentStart = 0, presentEnd = 0;
        app.swapchain->GetLastPresentCount(&presentStart);
        const UINT64 frameStart = app.frame;
        app.inPresent = app.inWait = 0;
        // With --perf, when each present reached DXGI, seen from outside OptiScaler: a thread reads the present count
        // every 0.1 ms. Two presents within one read count as no time apart.
        std::atomic<bool> polling = options.perf;
        std::vector<double> presentTimes;
        std::thread poller;
        if (options.perf)
            poller = std::thread(
                [&]
                {
                    UINT last = presentStart, count = 0;
                    while (polling)
                    {
                        const auto pollStart = Clock::now();
                        if (SUCCEEDED(app.swapchain->GetLastPresentCount(&count)))
                        {
                            const double at =
                                std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
                            for (; last != count; ++last)
                                presentTimes.push_back(at);
                        }
                        while (Clock::now() - pollStart < std::chrono::microseconds(100))
                            YieldProcessor();
                    }
                });
        begin = Clock::now();
        std::vector<App::ProbeResult> probes;
        double nextProbe = step.seconds / 8, worst = 0;
        while (Seconds(begin) < step.seconds)
        {
            Pump();
            App::ProbeResult probe;
            const bool probeNow = !app.options.noProbe && Seconds(begin) >= nextProbe && probes.size() < 6;
            const auto started = Clock::now();
            const HRESULT hr = app.Frame(probeNow ? &probe : nullptr);
            // The longest frame that was not a probe: a stall in Present or in the frame before it.
            if (!probeNow)
                worst = std::max(worst, 1000 * Seconds(started));
            if (FAILED(hr))
                Fail(step.phase, hr);
            if (probeNow)
            {
                if (probes.empty() && app.options.dump)
                    app.Dump(step.phase);
                probes.push_back(probe);
                nextProbe += step.seconds / 8;
            }
        }
        const double elapsed = Seconds(begin);
        app.swapchain->GetLastPresentCount(&presentEnd);
        polling = false;
        if (poller.joinable())
            poller.join();
        // Time between presents: p10, p50, p90, and the share under a quarter of an even spacing.
        std::vector<double> gaps;
        for (size_t i = 1; i < presentTimes.size(); ++i)
            gaps.push_back(presentTimes[i] - presentTimes[i - 1]);
        std::sort(gaps.begin(), gaps.end());
        const double even = presentEnd > presentStart ? 1000 * elapsed / (presentEnd - presentStart) : 0;
        const auto gapAt = [&](double p) { return gaps.empty() ? 0.0 : gaps[size_t(p * (gaps.size() - 1))]; };
        const double bunched =
            gaps.empty()
                ? 0.0
                : 100.0 * std::count_if(gaps.begin(), gaps.end(), [&](double g) { return g < even / 4; }) / gaps.size();
        if (options.perf)
            Out("spacing %s p10=%.2fms p50=%.2fms p90=%.2fms bunched=%.0f%%", step.phase, gapAt(0.1), gapAt(0.5),
                gapAt(0.9), bunched);
        // Probe frames wait for the GPU and count in the rate. least is the probe NR changed least: a frame
        // that missed NR shows there.
        const UINT64 frames = app.frame - frameStart;
        double changed = 0, least = 1, mean = 0;
        for (auto& p : probes)
        {
            changed = std::max(changed, p.changed);
            least = std::min(least, p.changed);
            mean = std::max(mean, p.mean);
        }
        Out("phase %s frames=%llu seconds=%.2f fps=%.1f presents=%u ratio=%.2f changed=%.4f least=%.4f mean=%.3f "
            "size=%ux%u worst=%.1fms pps=%.1f in-present=%.2fms in-wait=%.2fms",
            step.phase, frames, elapsed, frames / elapsed, presentEnd - presentStart,
            frames ? double(presentEnd - presentStart) / frames : 0.0, changed, least, mean, app.width, app.height,
            worst, (presentEnd - presentStart) / elapsed, frames ? 1000 * app.inPresent / frames : 0.0,
            frames ? 1000 * app.inWait / frames : 0.0);
        app.DebugMessages(step.phase);
    }
    app.Idle();
    Out("child-done frames=%llu", app.frame);
    if (results)
        std::fclose(results);
    results = nullptr;
    // Leave through the normal process exit, as a game does.
    return 0;
}

// ---------------------------------------------------------------- parent

struct Lock
{
    std::wstring dir;
    explicit Lock(std::wstring path) : dir(std::move(path))
    {
        if (dir.empty())
            return;
        for (;;)
        {
            if (CreateDirectoryW(dir.c_str(), nullptr))
                return;
            WIN32_FILE_ATTRIBUTE_DATA data {};
            if (GetFileAttributesExW(dir.c_str(), GetFileExInfoStandard, &data))
            {
                FILETIME now;
                GetSystemTimeAsFileTime(&now);
                const auto age =
                    (ULARGE_INTEGER { now.dwLowDateTime, now.dwHighDateTime }.QuadPart -
                     ULARGE_INTEGER { data.ftCreationTime.dwLowDateTime, data.ftCreationTime.dwHighDateTime }
                         .QuadPart) /
                    10000000ull;
                if (age > 20 * 60)
                {
                    RemoveDirectoryW(dir.c_str());
                    continue;
                }
            }
            std::printf("waiting for the GPU lock\n");
            Sleep(20000);
        }
    }
    ~Lock()
    {
        if (!dir.empty())
            RemoveDirectoryW(dir.c_str());
    }
};

struct PhaseResult
{
    double ratio = 0, changed = -1, least = -1, fps = 0, pps = 0, worst = 0;
};

static std::map<std::string, PhaseResult> ParsePhases(const std::string& text)
{
    std::map<std::string, PhaseResult> phases;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);)
    {
        char name[64] {};
        PhaseResult r;
        if (std::sscanf(line.c_str(),
                        "phase %63s frames=%*u seconds=%*f fps=%lf presents=%*u ratio=%lf changed=%lf least=%lf", name,
                        &r.fps, &r.ratio, &r.changed, &r.least) == 5)
        {
            if (const auto at = line.find(" worst="); at != std::string::npos)
                std::sscanf(line.c_str() + at, " worst=%lfms pps=%lf", &r.worst, &r.pps);
            phases[name] = r;
        }
    }
    return phases;
}

// Every thread's stack of another process, symbolized where a PDB sits beside the module, added to a file.
static void WriteStacks(HANDLE process, DWORD pid, const fs::path& file, const char* title)
{
    std::ofstream out(file, std::ios::app);
    out << "== " << title << "\n";
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitializeW(process, file.parent_path().c_str(), TRUE);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry { sizeof(entry) };
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != pid)
            continue;
        HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, entry.th32ThreadID);
        if (!thread)
            continue;
        SuspendThread(thread);
        CONTEXT context {};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context))
        {
            out << "thread " << entry.th32ThreadID << "\n";
            STACKFRAME64 frame {};
            frame.AddrPC = { context.Rip, 0, AddrModeFlat };
            frame.AddrFrame = { context.Rbp, 0, AddrModeFlat };
            frame.AddrStack = { context.Rsp, 0, AddrModeFlat };
            for (int i = 0; i < 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                                                  SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
                 ++i)
            {
                const DWORD64 pc = frame.AddrPC.Offset;
                IMAGEHLP_MODULEW64 module { sizeof(module) };
                const bool known = SymGetModuleInfoW64(process, pc, &module);
                alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] {};
                auto symbol = (SYMBOL_INFO*) storage;
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                DWORD64 displacement = 0;
                IMAGEHLP_LINE64 line { sizeof(line) };
                DWORD column = 0;
                char text[1024];
                std::snprintf(text, sizeof(text), "  %ls+0x%llx %s %s:%lu\n", known ? module.ModuleName : L"?",
                              known ? pc - module.BaseOfImage : pc,
                              SymFromAddr(process, pc, &displacement, symbol) ? symbol->Name : "?",
                              SymGetLineFromAddr64(process, pc, &column, &line) ? line.FileName : "?", line.LineNumber);
                out << text;
            }
        }
        ResumeThread(thread);
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    SymCleanup(process);
}

// Another DLL in the runtime's place for one run; the runtime goes back after it.
struct BadRuntime
{
    fs::path runtime = Here() / L"dlssnr_amd_pass1.dll", kept = Here() / L"dlssnr_amd_pass1.kept";
    bool swapped = false;
    explicit BadRuntime(bool on)
    {
        std::error_code ec;
        if (!on)
            return;
        fs::rename(runtime, kept, ec);
        swapped = !ec;
        if (swapped)
            fs::copy_file(Here() / L"OptiScaler" / L"libxell.dll", runtime, ec);
    }
    ~BadRuntime()
    {
        std::error_code ec;
        if (!swapped)
            return;
        fs::remove(runtime, ec);
        fs::rename(kept, runtime, ec);
    }
};

static int Parent(const Options& options, const std::wstring& lock, DWORD timeoutSeconds, const std::wstring& args)
{
    const auto dir = Here();
    const BadRuntime badRuntime(options.badRuntime);
    std::error_code ec;
    if (!fs::copy_file(dir / L"OptiScaler.template.ini", dir / L"OptiScaler.ini", fs::copy_options::overwrite_existing,
                       ec))
    {
        std::printf("FAIL: OptiScaler.template.ini missing\n");
        return 2;
    }
    const auto ini = (dir / L"OptiScaler.ini").wstring();
    const bool fg = options.scenario == "fg";
    const std::pair<const wchar_t*, std::pair<const wchar_t*, const wchar_t*>> keys[] = {
        { L"Log", { L"LogToFile", L"true" } },
        { L"Log", { L"LogLevel", options.logLevel.c_str() } },
        { L"Log", { L"LogAsync", L"false" } },
        { L"DlssNr", { L"Enabled", options.nrOffStart ? L"false" : L"true" } },
        { L"DlssNr", { L"PresentWithoutUpscaler", L"true" } },
        { L"DlssNr", { L"NrBackend", L"daniel" } },
        { L"FrameGen", { L"Enabled", fg ? L"true" : L"false" } },
        { L"FrameGen", { L"FGInput", fg ? L"upscaler" : L"nofg" } },
        { L"FrameGen", { L"FGOutput", fg ? L"xefg" : L"nofg" } },
        { L"XeFG", { L"InterpolationCount", L"2" } },
        { L"DlssNr", { L"AmdModelScale", options.modelScale.c_str() } },
    };
    for (auto& [section, kv] : keys)
        WritePrivateProfileStringW(section, kv.first, kv.second, ini.c_str());
    // A larger menu, so that its first page is longer than the window and scrolls.
    if (options.wheel)
        WritePrivateProfileStringW(L"Menu", L"Scale", L"1.3", ini.c_str());
    for (auto& setting : options.settings)
    {
        const auto slash = setting.find(L'/'), equals = setting.find(L'=');
        if (slash < equals && equals != std::wstring::npos)
            WritePrivateProfileStringW(setting.substr(0, slash).c_str(),
                                       setting.substr(slash + 1, equals - slash - 1).c_str(),
                                       setting.substr(equals + 1).c_str(), ini.c_str());
    }
    for (auto name : { L"OptiScaler.log", L"amd_presr.log", L"dx12_smoke_result.txt" })
        fs::remove(dir / name, ec);

    int code = 0;
    {
        Lock held(lock);
        wchar_t self[MAX_PATH] {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(self) + L"\" --child " + args;
        STARTUPINFOW startup { sizeof(startup) };
        PROCESS_INFORMATION process {};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup,
                            &process))
        {
            std::printf("FAIL: child did not start\n");
            return 2;
        }
        if (WaitForSingleObject(process.hProcess, timeoutSeconds * 1000) != WAIT_OBJECT_0)
        {
            WriteStacks(process.hProcess, process.dwProcessId, dir / "hang-stacks.txt", "hung");
            std::printf("child hung: stacks in hang-stacks.txt\n");
            TerminateProcess(process.hProcess, 9);
            WaitForSingleObject(process.hProcess, 10000);
            std::printf("FAIL: child timed out after %lu s\n", timeoutSeconds);
            code = 1;
        }
        DWORD exitCode = 0;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        std::printf("child exit code %lu\n", exitCode);
        if (exitCode != 0)
            code = 1;
    }

    const auto result = ReadAll(dir / L"dx12_smoke_result.txt");
    const auto log = ReadAll(dir / L"OptiScaler.log");
    const auto runtimeLog = ReadAll(dir / L"amd_presr.log");
    const auto phases = ParsePhases(result);
    int failures = 0;
    auto expect = [&](bool ok, const char* what)
    {
        std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    auto phase = [&](const char* name) { return phases.contains(name) ? phases.at(name) : PhaseResult {}; };
    expect(code == 0, "child ran to completion and exited cleanly");
    expect(Count(result, "child-done") == 1, "child finished every phase");
    expect(Count(log, "887A0005") == 0 && Count(log, "Device removed reason") == 0, "no device removal in the log");
    // The runtime falls back for a frame on its own timeouts; a loaded machine can cause them, so they are counted.
    std::printf("info: NR runtime timeouts %d, frames left without NR while the runtime was busy %d, XeFG turned "
                "off %d and on %d times\n",
                Count(runtimeLog, "AMD timeout"), Count(log, "runtime still busy, frame left without NR"),
                Count(log, "SetEnabled: false"), Count(log, "SetEnabled: true"));
    // Red Dead Redemption's quirk (by the executable's name): the first swapchain is a plain one, XeFG's the next.
    wchar_t self[MAX_PATH] {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const bool plainFirst = _wcsicmp(fs::path(self).filename().c_str(), L"rdr.exe") == 0;
    std::vector<const char*> withNr, withoutNr, generating, single;
    if (options.nrOffStart)
    {
        withoutNr.push_back("guides-only");
        withNr.push_back("nr-late-on");
        generating = { "guides-only", "nr-late-on" };
    }
    if (options.lifecycle)
    {
        withNr.insert(withNr.end(), { "steady", "new-queue", "same-queue", "fg-page", "fg-page-closed", "restored" });
        // The path without frame generation leaves a minimized window alone, and final-image NR with XeFG
        // does the same.
        withoutNr.push_back("minimized");
        generating.insert(generating.end(), { "new-queue", "same-queue", "fg-page", "fg-page-closed", "restored" });
        if (fg)
        {
            withNr.insert(withNr.end(), { "fg-page-off", "fg-page-on" });
            single.push_back("fg-page-off");
            generating.push_back("fg-page-on");
        }
        (plainFirst ? single : generating).push_back("steady");
        // An XeFG swapchain is kept across a recreation (PreserveSwapChain) with its first queue, unless the
        // game's quirk says otherwise, so only a plain first swapchain makes NR move.
        if (plainFirst || !fg)
            expect(Count(log, "NR without upscaling: the game presents from another D3D12 queue") >= 1,
                   "NR follows the game to its new queue");
        expect(Count(result, "fg-page switched=") == 1 &&
                   std::atof(result.c_str() + result.find("fg-page switched=") + 17) > 0.1,
               "the menu shows its Frame Gen page");
        if (options.scenario == "nr")
        {
            // WARP has no runtime: NR waits for the hardware device, which comes back as the same device.
            withoutNr.push_back("warp");
            withNr.push_back("hardware");
            expect(Count(log, "NR without upscaling: the game presents from another D3D12 device") >= 2,
                   "NR follows the game to WARP and back");
        }
    }
    if (options.wheel)
    {
        const auto at = result.find("wheel scrolled=");
        const double scrolled = at == std::string::npos ? 0.0 : std::atof(result.c_str() + at + 15);
        std::printf("info: the menu page changed over %.1f%% of its area after the wheel\n", scrolled * 100);
        expect(scrolled > 0.1, "the mouse wheel scrolls the open menu");
        if (options.dinput)
            expect(Count(result, "dinput mouse exclusive") == 1, "the game reads its mouse through DirectInput");
        withNr = { "steady" };
        generating = { "steady" };
    }
    else if (options.hitch)
    {
        // Frame generation goes on after a few long frames, and the game gets its frame rate back.
        for (auto name : { "steady", "after-hitch", "later" })
            std::printf("info: %-11s real %6.1f fps, presented %6.1f/s, %.2f presents per frame\n", name,
                        phase(name).fps, phase(name).pps, phase(name).ratio);
        expect(phase("later").fps > 0.85 * phase("steady").fps, "the game keeps its frame rate after the long frames");
        withNr = { "steady", "after-hitch", "later" };
        generating = { "steady", "after-hitch", "later" };
    }
    else if (options.perf)
    {
        for (auto name : { "nr-fg", "nr-only", "none", "fg-only", "nr-fg-again" })
            std::printf("info: %-11s real %6.1f fps, presented %6.1f/s, %.2f presents per frame, worst frame %.1f ms\n",
                        name, phase(name).fps, phase(name).pps, phase(name).ratio, phase(name).worst);
        generating = { "nr-fg", "fg-only", "nr-fg-again" };
        single = { "nr-only", "none" };
        // XeFG's frames come on top of the game's, which keeps most of its own rate: NR is most of the frame. Without
        // NR XeFG's own work is a larger part of a short frame, but the presented rate must still rise.
        for (auto name : { "nr-fg", "nr-fg-again" })
        {
            expect(phase(name).pps > (options.expect2x ? 1.6 : 2.4) * phase("nr-only").pps,
                   (std::string("XeFG raises the presented frame rate with NR in ") + name).c_str());
            expect(phase(name).fps > 0.85 * phase("nr-only").fps,
                   (std::string("the game keeps its own frame rate with XeFG and NR in ") + name).c_str());
        }
        // At 2X XeFG's work on a frame this short takes about what it gives back, so there it must only not cost.
        if (options.expect2x)
            expect(phase("fg-only").pps > 0.9 * phase("none").pps, "XeFG keeps the presented frame rate without NR");
        else
            expect(phase("fg-only").pps > 1.1 * phase("none").pps, "XeFG raises the presented frame rate without NR");
        // When the presents reached DXGI, read from this process: a burst handed over at once puts about two thirds of
        // them less than a quarter of an even spacing after the previous one at 3X.
        for (auto name : generating)
        {
            const auto at = result.find(std::string("spacing ") + name + " ");
            const auto line = at == std::string::npos ? std::string() : result.substr(at, result.find('\n', at) - at);
            const auto bunched = line.find("bunched=");
            std::printf("info: %s\n", line.c_str());
            expect(bunched != std::string::npos && std::atof(line.c_str() + bunched + 8) < 10,
                   (std::string("XeFG's frames are spread over the game's frame period in ") + name).c_str());
        }
    }
    else if (!options.nrOffStart && !options.lifecycle)
    {
        expect(Count(log, "OptiInput::ApplyMenuVisibilityChangeLocked menu visibility changed 0 -> 1") >= 2,
               "menu opened twice");
        expect(Count(log, "OptiInput::ApplyMenuVisibilityChangeLocked menu visibility changed 1 -> 0") >= 2,
               "menu closed twice");
        // A Model scale change restarts the runtime's history for its settings.
        if (!options.badRuntime && !options.hdr10)
            expect(SettingsResets(runtimeLog) >= 1, "Model scale changed from the menu during the session");
        withNr = {
            "steady", "menu-open", "menu-closed", "fg-off", "fg-on", "nr-on", "scale", "resized", "menu-resized"
        };
        // The menu's "Off" notification covers well under 5% of the frame; NR changes most of it.
        withoutNr = { "nr-off" };
        generating = { "steady", "menu-open", "menu-closed", "fg-on",       "nr-off",
                       "nr-on",  "scale",     "resized",     "menu-resized" };
        single = { "fg-off" };
    }
    // Final-image NR takes 8-bit and SDR 10-bit frames, and XeFG's guides come from the same frame.
    if (options.r10)
        expect(Count(log, "NR without upscaling: backbuffer format") == 0, "the SDR R10G10B10A2 backbuffer is taken");
    if (options.hdr10)
    {
        expect(Count(log, "NR without upscaling: backbuffer format 24 is 10-bit in an HDR colour space") == 1,
               "OptiScaler.log names the HDR10 backbuffer once");
        for (auto name : withNr)
            if (std::string(name) != "menu-open" && std::string(name) != "menu-resized")
                withoutNr.push_back(name);
        withNr.clear();
        single.insert(single.end(), generating.begin(), generating.end());
        generating.clear();
    }
    // A runtime that cannot load leaves every frame as the game drew it, and without frame generation it costs
    // no GPU work: the frame ends before the copy and the optical flow.
    if (options.badRuntime)
    {
        expect(badRuntime.swapped && Count(runtimeLog, "Private AMD runtime hash mismatch") >= 1,
               "the runtime fails to load");
        expect(Count(log, "NR without upscaling: runtime failed") == 1, "OptiScaler.log names the failed runtime once");
        expect(Count(log, "runtime still busy") == 0, "a failed runtime is not reported as busy");
        for (auto name : withNr)
            if (std::string(name) != "menu-open" && std::string(name) != "menu-resized")
                withoutNr.push_back(name);
        withNr.clear();
        if (!fg)
        {
            std::printf("info: steady %.1f fps, nr-off %.1f fps\n", phase("steady").fps, phase("nr-off").fps);
            expect(phase("steady").fps > 0.6 * phase("nr-off").fps, "a failed runtime runs as fast as NR off");
        }
    }
    if (!options.noProbe)
    {
        for (auto name : withNr)
            expect(phase(name).least > 0.3, (std::string("NR changed every probed frame in ") + name).c_str());
        for (auto name : withoutNr)
            expect(phase(name).changed < 0.05,
                   (std::string("NR left the presented frame untouched in ") + name).c_str());
    }
    if (fg)
    {
        expect(Count(log, "XeFG swapchain created") >= 1, "XeFG swapchain created");
        for (auto name : generating)
            if (options.expect2x)
                expect(phase(name).ratio > 1.7 && phase(name).ratio < 2.3,
                       (std::string("XeFG presents one generated frame per frame in ") + name + " (2X)").c_str());
            else
                expect(phase(name).ratio >= 2.4,
                       (std::string("XeFG presents generated frames in ") + name + " (ratio >= 2.4)").c_str());
        for (auto name : single)
            expect(phase(name).ratio > 0.9 && phase(name).ratio < 1.2,
                   (std::string("one present per game frame in ") + name).c_str());
    }
    else
    {
        for (auto& [name, r] : phases)
        {
            // A minimized window may present nothing at all.
            std::string what = "one present per game frame in " + name;
            expect(name == "minimized" || (r.ratio > 0.9 && r.ratio < 1.1), what.c_str());
        }
    }
    if (options.debugLayer)
        expect(Count(result, "debug-layer on") == 1, "D3D12 debug layer enabled");
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

// The wheel helper: moves the cursor, turns the wheel down by notches, then leaves the window.
static int WheelHelper(LONG x, LONG y, int notches)
{
    SetCursorPos(x, y);
    Sleep(300);
    INPUT input {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = DWORD(-WHEEL_DELTA);
    for (int i = 0; i < notches; ++i)
    {
        SendInput(1, &input, sizeof(input));
        Sleep(60);
    }
    Sleep(300);
    SetCursorPos(0, 0);
    return 0;
}

// The click helper: moves the cursor, presses and releases the left button, then leaves the window, as a
// hand on the mouse does.
static int ClickHelper(LONG x, LONG y, DWORD holdMs)
{
    SetCursorPos(x, y);
    Sleep(300);
    INPUT input {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(input));
    Sleep(holdMs);
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(input));
    Sleep(300);
    SetCursorPos(0, 0);
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 5 && std::wstring(argv[1]) == L"--stacks")
    {
        const DWORD pid = std::wcstoul(argv[2], nullptr, 10);
        HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!process)
            return 1;
        std::string title;
        for (const wchar_t* c = argv[4]; *c; ++c)
            title += char(*c);
        WriteStacks(process, pid, Here() / argv[3], title.c_str());
        CloseHandle(process);
        return 0;
    }
    if (argc == 5 && std::wstring(argv[1]) == L"--wheel")
        return WheelHelper(std::wcstol(argv[2], nullptr, 10), std::wcstol(argv[3], nullptr, 10),
                           std::wcstol(argv[4], nullptr, 10));
    if (argc == 5 && std::wstring(argv[1]) == L"--click")
        return ClickHelper(std::wcstol(argv[2], nullptr, 10), std::wcstol(argv[3], nullptr, 10),
                           std::wcstoul(argv[4], nullptr, 10));
    Options options;
    bool child = false;
    std::wstring lock;
    DWORD timeout = 240;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--child")
            child = true;
        else if (arg == L"--bgra")
            options.bgra = true;
        else if (arg == L"--r10")
            options.r10 = true;
        else if (arg == L"--hdr10")
            options.hdr10 = true;
        else if (arg == L"--still")
            options.still = true;
        else if (arg == L"--debug-layer")
            options.debugLayer = true;
        else if (arg == L"--lock" && i + 1 < argc)
            lock = argv[++i];
        else if (arg == L"--dump")
            options.dump = true;
        else if (arg == L"--lifecycle")
            options.lifecycle = true;
        else if (arg == L"--nr-off-start")
            options.nrOffStart = true;
        else if (arg == L"--bad-runtime")
            options.badRuntime = true;
        else if (arg == L"--no-probe")
            options.noProbe = true;
        else if (arg == L"--stall-stacks")
            options.stallStacks = true;
        else if (arg == L"--expect-2x")
            options.expect2x = true;
        else if (arg == L"--wheel")
            options.wheel = true;
        else if (arg == L"--dinput")
            options.dinput = true;
        else if (arg == L"--perf")
            options.perf = options.noProbe = true;
        else if (arg == L"--borderless")
            options.borderless = true;
        else if (arg == L"--reflex")
            options.reflex = true;
        else if (arg == L"--hitch")
            options.hitch = true;
        else if (arg == L"--vsync")
            options.vsync = true;
        else if (arg == L"--gpu-load" && i + 1 < argc)
            options.gpuLoad = std::wcstol(argv[++i], nullptr, 10);
        else if (arg == L"--waitable" && i + 1 < argc)
            options.waitable = std::wcstol(argv[++i], nullptr, 10);
        else if (arg == L"--cpu-ms" && i + 1 < argc)
            options.cpuMs = std::wcstod(argv[++i], nullptr);
        else if (arg == L"--model-scale" && i + 1 < argc)
            options.modelScale = argv[++i];
        else if (arg == L"--set" && i + 1 < argc)
            options.settings.push_back(argv[++i]);
        else if (arg == L"--log-level" && i + 1 < argc)
            options.logLevel = argv[++i];
        else if (arg == L"--timeout" && i + 1 < argc)
            timeout = std::wcstoul(argv[++i], nullptr, 10);
        else
            for (wchar_t c : arg)
                options.scenario += char(c);
    }
    if (options.scenario != "nr" && options.scenario != "fg")
    {
        std::printf("usage: dx12_final_image_smoke.exe <nr|fg> [--bgra] [--debug-layer] [--lifecycle] "
                    "[--nr-off-start] [--bad-runtime] [--r10] [--hdr10] [--still] [--no-probe] [--lock <dir>] "
                    "[--timeout <seconds>]\n");
        return 2;
    }
    if (child)
        return Child(options);
    std::wstring args(options.scenario.begin(), options.scenario.end());
    args += options.bgra ? L" --bgra" : L"";
    args += options.r10 ? L" --r10" : L"";
    args += options.hdr10 ? L" --hdr10" : L"";
    args += options.still ? L" --still" : L"";
    args += options.debugLayer ? L" --debug-layer" : L"";
    args += options.cpuMs > 0 ? L" --cpu-ms " + std::to_wstring(options.cpuMs) : L"";
    args += options.dump ? L" --dump" : L"";
    args += options.lifecycle ? L" --lifecycle" : L"";
    args += options.nrOffStart ? L" --nr-off-start" : L"";
    args += options.badRuntime ? L" --bad-runtime" : L"";
    args += options.noProbe ? L" --no-probe" : L"";
    args += options.stallStacks ? L" --stall-stacks" : L"";
    args += options.perf ? L" --perf" : L"";
    args += options.wheel ? L" --wheel" : L"";
    args += options.dinput ? L" --dinput" : L"";
    args += options.borderless ? L" --borderless" : L"";
    args += options.reflex ? L" --reflex" : L"";
    args += options.hitch ? L" --hitch" : L"";
    args += options.vsync ? L" --vsync" : L"";
    args += options.gpuLoad ? L" --gpu-load " + std::to_wstring(options.gpuLoad) : L"";
    args += options.waitable ? L" --waitable " + std::to_wstring(options.waitable) : L"";
    return Parent(options, lock, timeout, args);
}
