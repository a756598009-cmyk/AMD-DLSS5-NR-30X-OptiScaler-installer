// The 64-bit side of the 32-bit D3D9 bridge (proxy.cpp starts it). It loads OptiScaler.dll from
// its own folder before it creates anything, so OptiScaler sees an ordinary D3D11 program: a device
// on the game's adapter and a swap chain on the game's window. Each frame the game side hands over
// is copied into that swap chain and presented, and OptiScaler's final-image NR, frame generation
// and menu run there as they do in a D3D11 game.
//   OptiScalerDx9Host.exe <pipe name> <game pid>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>
#include "dx9_wire.h"

using Microsoft::WRL::ComPtr;
using namespace dx9wire;

namespace
{
std::filesystem::path folder;

void Log(const char* format, ...)
{
    char line[1024] {};
    SYSTEMTIME t {};
    GetLocalTime(&t);
    int used = sprintf_s(line, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, format);
    vsprintf_s(line + used, sizeof(line) - used, format, args);
    va_end(args);
    FILE* file = nullptr;
    if (_wfopen_s(&file, (folder / L"dx9-host.log").c_str(), L"a") == 0 && file)
    {
        fprintf(file, "%s\n", line);
        fclose(file);
    }
}

// OptiScaler starts a thread of its own when it loads (getGpuInfo: it makes a DXGI factory, which
// OptiScaler hooks, and hooks amdxc64). Detours runs one transaction at a time, so a DXGI factory made
// here at the same moment can leave OptiScaler without its factory hooks ("Failed to hook IDXGIFactory:
// 10DD"), and then without NR, frame generation and menu. Waits up to 10 s for the threads that start
// in OptiScaler.dll's own code to finish.
void WaitForOptiScalerThreads(HMODULE opti)
{
    MODULEINFO module {};
    using Query = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    const auto query =
        reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    const Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!query || !snapshot || !GetModuleInformation(GetCurrentProcess(), opti, &module, sizeof(module)))
        return;
    const auto begin = static_cast<BYTE*>(module.lpBaseOfDll), end = begin + module.SizeOfImage;
    const ULONGLONG deadline = GetTickCount64() + 10000;
    THREADENTRY32 entry { sizeof(entry) };
    for (BOOL more = Thread32First(snapshot.value, &entry); more; more = Thread32Next(snapshot.value, &entry))
    {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId())
            continue;
        const Handle thread(OpenThread(SYNCHRONIZE | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID));
        BYTE* start = nullptr;
        // 9: ThreadQuerySetWin32StartAddress.
        if (!thread || query(thread.value, 9, &start, sizeof(start), nullptr) != 0 || start < begin || start >= end)
            continue;
        const ULONGLONG now = GetTickCount64();
        const DWORD waited = WaitForSingleObject(thread.value, now < deadline ? DWORD(deadline - now) : 0);
        Log("OptiScaler thread %lu %s", entry.th32ThreadID, waited == WAIT_OBJECT_0 ? "finished" : "still running");
    }
}

struct Host
{
    Handle pipe, game;
    HWND window = nullptr, surface = nullptr;
    uint32_t (*overlayState)() = nullptr;
    void (*addText)(const wchar_t*, uint32_t) = nullptr;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap;
    bool tearing = false;
    ComPtr<ID3D11Texture2D> shared;
    ComPtr<ID3D11Query> copied;
    Handle section;
    const void* view = nullptr;
    Build build {};
    bool built = false;
    uint64_t presented = 0;

    void Release()
    {
        built = false;
        shared.Reset();
        if (view)
            UnmapViewOfFile(view);
        view = nullptr;
        section.reset();
    }

    bool Reply(Kind kind, Result result, uint32_t error = 0, uint64_t frame = 0, uint32_t state = 0)
    {
        Ack ack;
        ack.header.kind = kind;
        ack.header.bytes = sizeof(Ack) - sizeof(Header);
        ack.result = result;
        ack.error = error;
        ack.generation = build.generation;
        ack.frame = frame;
        ack.state = state;
        return Send(pipe.value, game.value, &ack, sizeof(ack));
    }

    bool Hello(const dx9wire::Hello& hello)
    {
        window = reinterpret_cast<HWND>(static_cast<uintptr_t>(hello.window));
        DWORD owner = 0;
        if (!IsWindow(window) || !GetWindowThreadProcessId(window, &owner) || owner != hello.pid)
        {
            Log("window %08X does not belong to the game (%lu)", hello.window, owner);
            return false;
        }
        // OptiScaler's menu reads the cursor on this thread against the game's window; the same DPI
        // awareness as that window gives it the coordinates the game's back buffer is laid out in.
        SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(window));
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
            return false;
        ComPtr<IDXGIFactory4> factory4;
        ComPtr<IDXGIAdapter1> adapter;
        const LUID luid { hello.luidLow, hello.luidHigh };
        if (FAILED(factory.As(&factory4)) || FAILED(factory4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
        {
            Log("no adapter with the game's LUID %08X:%08X", hello.luidHigh, hello.luidLow);
            return false;
        }
        const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr =
            D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                              2, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(hr))
        {
            Log("D3D11CreateDevice failed %08lX", hr);
            return false;
        }
        D3D11_QUERY_DESC query { D3D11_QUERY_EVENT, 0 };
        if (FAILED(device->CreateQuery(&query, &copied)))
            return false;
        Log("device ready for window %08X of pid %lu", hello.window, hello.pid);
        return true;
    }

    Result Build(const dx9wire::Build& request)
    {
        Release();
        build = request;
        // One swap chain per host, on the child window it was made for: a game that moves to a new
        // window gets a new host.
        const HWND target = reinterpret_cast<HWND>(static_cast<uintptr_t>(request.window));
        if (swap && target != surface)
        {
            Log("stage %llu is for window %08X, the swap chain is on %p", request.generation, request.window, surface);
            return Result::Failed;
        }
        if (request.transport == Transport::SharedTexture)
        {
            // A D3D9 shared handle is not a kernel handle: the same value opens it in any process,
            // sign-extended from the 32-bit side.
            const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<intptr_t>(static_cast<int32_t>(request.handle)));
            if (FAILED(device->OpenSharedResource(handle, IID_PPV_ARGS(&shared))))
            {
                Log("OpenSharedResource failed for %08llX", request.handle);
                return Result::Failed;
            }
            D3D11_TEXTURE2D_DESC desc {};
            shared->GetDesc(&desc);
            if (desc.Width != request.width || desc.Height != request.height ||
                static_cast<uint32_t>(desc.Format) != request.format)
            {
                Log("shared texture is %ux%u format %u, expected %ux%u format %u", desc.Width, desc.Height, desc.Format,
                    request.width, request.height, request.format);
                return Result::Failed;
            }
        }
        else
        {
            section.reset(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.handle)));
            view = MapViewOfFile(section.value, FILE_MAP_READ, 0, 0, SIZE_T(request.pitch) * request.height);
            if (!view)
                return Result::Failed;
        }
        if (!swap)
        {
            DXGI_SWAP_CHAIN_DESC1 desc {};
            desc.Width = request.width;
            desc.Height = request.height;
            desc.Format = static_cast<DXGI_FORMAT>(request.format);
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            // Without tearing a windowed flip swap chain waits for the display even at interval 0, which
            // would cap a game that asked for no vsync at the refresh rate.
            ComPtr<IDXGIFactory5> factory5;
            BOOL allowed = FALSE;
            tearing = SUCCEEDED(factory.As(&factory5)) &&
                      SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowed,
                                                              sizeof(allowed))) &&
                      allowed;
            desc.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
            // The game side's child window, made for this host alone.
            if (!IsWindow(target) || GetAncestor(target, GA_ROOT) != GetAncestor(window, GA_ROOT))
            {
                Log("window %08X is not a child of the game's window", request.window);
                return Result::Failed;
            }
            const HRESULT hr = factory->CreateSwapChainForHwnd(device.Get(), target, &desc, nullptr, nullptr, &swap);
            if (FAILED(hr))
            {
                Log("CreateSwapChainForHwnd failed %08lX", hr);
                return Result::Failed;
            }
            surface = target;
            factory->MakeWindowAssociation(surface, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
            if (overlayState && !(overlayState() & StateHooked))
            {
                Log("OptiScaler did not wrap the swap chain (no DXGI hooks): no NR, frame generation or menu");
                return Result::Unhooked;
            }
        }
        else
        {
            DXGI_SWAP_CHAIN_DESC1 desc {};
            swap->GetDesc1(&desc);
            if (desc.Width != request.width || desc.Height != request.height)
            {
                const HRESULT hr =
                    swap->ResizeBuffers(0, request.width, request.height, DXGI_FORMAT_UNKNOWN, desc.Flags);
                if (FAILED(hr))
                {
                    Log("ResizeBuffers failed %08lX", hr);
                    return Result::Failed;
                }
            }
        }
        built = true;
        Log("stage %llu: %ux%u format %u through %s", request.generation, request.width, request.height, request.format,
            request.transport == Transport::SharedTexture ? "a shared texture" : "shared memory");
        return Result::Ready;
    }

    // Answers the frame; false only when the device is gone and the host should exit.
    bool Frame(const dx9wire::Frame& frame)
    {
        if (!built || frame.generation != build.generation)
            return Reply(Kind::Frame, Result::Failed, 0, frame.id);
        if (addText && frame.textCount)
            addText(frame.text, std::min<uint32_t>(frame.textCount, ARRAYSIZE(frame.text)));
        {
            ComPtr<ID3D11Texture2D> back;
            if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back))))
                return Reply(Kind::Frame, Result::Failed, 0, frame.id);
            if (shared)
                context->CopyResource(back.Get(), shared.Get());
            else
                context->UpdateSubresource(back.Get(), 0, nullptr, view, build.pitch, 0);
            context->End(copied.Get());
        }
        const HRESULT hr =
            swap->Present(frame.syncInterval, tearing && !frame.syncInterval ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
        {
            const HRESULT reason = device->GetDeviceRemovedReason();
            Log("frame %llu: device lost, Present %08lX, reason %08lX", frame.id, hr, reason);
            Reply(Kind::Frame, Result::DeviceLost, static_cast<uint32_t>(reason), frame.id);
            return false;
        }
        // The game side writes the shared texture again next frame: the copy out of it must be done.
        const ULONGLONG deadline = GetTickCount64() + GpuWaitMs;
        HRESULT done = S_FALSE;
        while ((done = context->GetData(copied.Get(), nullptr, 0, 0)) == S_FALSE && GetTickCount64() < deadline)
            YieldProcessor();
        if (done != S_OK)
        {
            Log("frame %llu: copy out of the game's frame did not finish", frame.id);
            return Reply(Kind::Frame, Result::Failed, static_cast<uint32_t>(done), frame.id);
        }
        const uint32_t state = overlayState ? overlayState() : 0;
        if (++presented == 1 || presented % 600 == 0)
            Log("frame %llu presented (%llu so far) at interval %u, Present %08lX, menu %u, frame generation %u",
                frame.id, presented, frame.syncInterval, hr, state & StateMenu, (state & StateFrameGen) >> 1);
        return Reply(Kind::Frame, FAILED(hr) ? Result::Failed : Result::Presented, static_cast<uint32_t>(hr), frame.id,
                     state);
    }

    int Run()
    {
        for (;;)
        {
            Header header;
            if (!Receive(pipe.value, game.value, &header, sizeof(header), INFINITE) || !ValidHeader(header))
            {
                Log("pipe closed or malformed request; leaving");
                return 1;
            }
            union
            {
                dx9wire::Hello hello;
                dx9wire::Build build;
                dx9wire::Frame frame;
            } body {};
            if (header.bytes && !Receive(pipe.value, game.value, &body, header.bytes))
                return 1;
            bool ok = true;
            switch (header.kind)
            {
            case Kind::Hello:
                ok = Reply(Kind::Hello, Hello(body.hello) ? Result::Ready : Result::Failed);
                break;
            case Kind::Build:
                build.generation = body.build.generation;
                ok = Reply(Kind::Build, Build(body.build));
                break;
            case Kind::Frame:
                if (!Frame(body.frame))
                    return 2;
                break;
            case Kind::Drop:
                Release();
                ok = Reply(Kind::Drop, Result::Ready);
                break;
            case Kind::Quit:
                Reply(Kind::Quit, Result::Ready);
                return 0;
            }
            if (!ok)
                return 1;
        }
    }
};
} // namespace

int wmain(int argc, wchar_t** argv)
{
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    folder = std::filesystem::path(path).parent_path();
    if (argc < 3)
        return 64;
    const DWORD gamePid = static_cast<DWORD>(_wtoi(argv[2]));
    Host host;
    host.game.reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, gamePid));
    // The last session's OptiScaler.log is kept beside the new one, so a host that failed can be read
    // after the next has started.
    MoveFileExW((folder / L"OptiScaler.log").c_str(), (folder / L"OptiScaler.previous.log").c_str(),
                MOVEFILE_REPLACE_EXISTING);
    // OptiScaler hooks the dxgi.dll and d3d11.dll this program already imports, as it does in a game.
    const HMODULE opti = LoadLibraryW((folder / L"OptiScaler.dll").c_str());
    Log("host %lu for game %lu, OptiScaler.dll %s", GetCurrentProcessId(), gamePid, opti ? "loaded" : "missing");
    if (!opti)
        return 66;
    host.overlayState = reinterpret_cast<uint32_t (*)()>(GetProcAddress(opti, "OptiScalerOverlayState"));
    host.addText = reinterpret_cast<void (*)(const wchar_t*, uint32_t)>(GetProcAddress(opti, "OptiScalerAddText"));
    WaitForOptiScalerThreads(opti);
    host.pipe.reset(
        CreateFileW(argv[1], GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    ULONG server = 0;
    if (!host.game || !host.pipe || !GetNamedPipeServerProcessId(host.pipe.value, &server) || server != gamePid)
    {
        Log("cannot reach the game's pipe");
        return 65;
    }
    const int code = host.Run();
    host.Release();
    host.swap.Reset();
    Log("host leaving with %d after %llu frames", code, host.presented);
    return code;
}
