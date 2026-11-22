#include "pch.h"
#include "VulkanPresentExperimental.h"
#include "PresentExperimental.h"
#include "AmdBridge.h"
#include "ChildFgSettings.h"
#include "../backend/Selector.h"

#include <Config.h>
#include <Logger.h>
#include <Util.h>
#include <State.h>
#include <menu/menu_common.h>
#include <proxies/XeFG_Proxy.h>
#include <proxies/XeFGPacing.h>
#include <proxies/XeLL_Proxy.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <mutex>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <vector>
#include <algorithm>

namespace AmdVkPresent
{
using Microsoft::WRL::ComPtr;
namespace
{
// Set while the XeFG window is shown, read without the bridge lock on every present.
std::atomic<bool> fgLive = false;

LRESULT CALLBACK FgWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (message == WM_DESTROY)
        PostQuitMessage(0);
    return DefWindowProcW(window, message, wparam, lparam);
}

// The XeFG output window is a disabled child of the game window, so mouse input falls through to the
// game and it can never take focus. A child window shares the input queue of its parent's thread;
// it gets a thread of its own that pumps its messages, because the game's present thread often
// never does and its pending messages would then hold back all input of the game window.
struct FgWindow
{
    HWND window = nullptr;
    std::thread thread;

    bool Create(HWND parent, uint32_t width, uint32_t height)
    {
        static const bool registered = []
        {
            WNDCLASSW type {};
            type.lpfnWndProc = FgWindowProc;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"OptiScalerVulkanXeFG";
            return RegisterClassW(&type) != 0;
        }();
        if (!registered)
            return false;
        // A thread that finishes after Create gave up destroys its window and exits.
        struct Start
        {
            std::mutex lock;
            std::condition_variable done;
            HWND window = nullptr;
            bool finished = false, abandoned = false;
        };
        auto start = std::make_shared<Start>();
        thread = std::thread(
            [start, parent, width, height]
            {
                HWND window =
                    CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"OptiScalerVulkanXeFG", L"", WS_CHILD | WS_DISABLED, 0, 0,
                                    width, height, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
                if (!window)
                    LOG_ERROR("Vulkan XeFG: child window failed ({})", GetLastError());
                {
                    std::lock_guard hold(start->lock);
                    start->finished = true;
                    if (start->abandoned)
                    {
                        if (window)
                            DestroyWindow(window);
                        return;
                    }
                    start->window = window;
                }
                start->done.notify_one();
                MSG message;
                while (window && GetMessageW(&message, nullptr, 0, 0) > 0)
                    DispatchMessageW(&message);
            });
        std::unique_lock hold(start->lock);
        if (!start->done.wait_for(hold, std::chrono::seconds(2), [&] { return start->finished; }))
        {
            start->abandoned = true;
            LOG_ERROR("Vulkan XeFG: child window creation did not finish");
            thread.detach();
            return false;
        }
        window = start->window;
        return window != nullptr;
    }

    void Destroy()
    {
        if (window)
            PostMessageW(window, WM_CLOSE, 0, 0);
        window = nullptr;
        if (!thread.joinable())
            return;
        if (WaitForSingleObject(thread.native_handle(), 2000) == WAIT_OBJECT_0)
            thread.join();
        else
        {
            LOG_WARN("Vulkan XeFG: child window thread did not exit");
            thread.detach();
        }
    }

    // At process exit the thread is gone already and the loader lock is held, so it cannot be joined; a
    // joinable std::thread destroyed calls abort (Detroit: Become Human closing).
    ~FgWindow()
    {
        if (thread.joinable())
            thread.detach();
    }
};

// Logs the average time of one per-present step every 120 presents.
struct Timing
{
    uint64_t micros = 0;
    unsigned count = 0;

    void Add(std::chrono::steady_clock::time_point start, const char* step)
    {
        micros += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        if (++count < 120)
            return;
        LOG_INFO("Vulkan final-image bridge: {} average {:.1f} ms over 120 presents", step, micros / 120000.0);
        micros = count = 0;
    }
};

struct FgPresenter
{
    static void Log(const char* message, xefg_swapchain_logging_level_t level, void*)
    {
        if (level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING)
            LOG_WARN("Vulkan XeFG SDK: {}", message);
    }

    FgWindow output;
    HWND window = nullptr;
    xefg_swapchain_handle_t context = nullptr;
    xell_context_handle_t xell = nullptr;
    ComPtr<IDXGISwapChain3> swap;
    // Two allocators: the copies of one frame may still run on the GPU while the next frame records its own.
    ComPtr<ID3D12CommandAllocator> allocators[2];
    uint64_t used[2] {};
    ComPtr<ID3D12GraphicsCommandList> command;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> hudless;
    ComPtr<ID3D12CommandQueue> queue;
    uint64_t serial = 0;
    uint32_t frameId = 0;
    AmdPresentExperimental::ChildFgSettings settings;
    int maximum = 0;
    bool enabled = false;
    bool visible = false;
    bool resetOnResume = true;
    // Presents may tear, as the game's own do when it presents without vsync.
    bool tearing = false;
    std::chrono::steady_clock::time_point previous {};
    // XeFG's Present can hold its caller for most of a frame while the frame's NR runs, so it runs on a thread of its
    // own: the game's present thread hands the frame over and goes on with the next one. The game's thread calls Idle
    // before anything else of the presenter and before the next frame's NR, which rewrites the guides and images XeFG
    // reads until its Present returns.
    std::thread worker;
    std::mutex jobLock;
    std::condition_variable jobChanged;
    AmdPresentExperimental::Guides job;
    bool posted = false, quit = false, presentFailed = false;
    Timing presentTiming;
    // XeFG's output over the last second, for the FPS overlay.
    unsigned outputFrames = 0;
    std::chrono::steady_clock::time_point outputStart {};

    // As ~FgWindow: never Released before the process exits.
    ~FgPresenter()
    {
        if (worker.joinable())
            worker.detach();
    }

    void Idle()
    {
        std::unique_lock hold(jobLock);
        jobChanged.wait(hold, [&] { return !posted; });
    }

    void Post(const AmdPresentExperimental::Guides& guides)
    {
        if (!worker.joinable())
            worker = std::thread(
                [this]
                {
                    std::unique_lock hold(jobLock);
                    while (true)
                    {
                        jobChanged.wait(hold, [&] { return posted || quit; });
                        if (!posted)
                            return;
                        hold.unlock();
                        const auto start = std::chrono::steady_clock::now();
                        const bool presented = Present(job);
                        presentTiming.Add(start, "XeFG present on its own thread");
                        hold.lock();
                        presentFailed |= !presented;
                        posted = false;
                        jobChanged.notify_all();
                    }
                });
        {
            std::lock_guard hold(jobLock);
            job = guides;
            posted = true;
        }
        jobChanged.notify_all();
    }

    void Show(bool show)
    {
        if (!window) return;
        ShowWindowAsync(window, show ? SW_SHOWNA : SW_HIDE);
        visible = fgLive = AmdPresentExperimental::childFgShown = show;
    }

    bool Reached(uint64_t value)
    {
        if (fence->GetCompletedValue() >= value)
            return true;
        HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!completed)
            return false;
        const bool done = SUCCEEDED(fence->SetEventOnCompletion(value, completed)) &&
                          WaitForSingleObject(completed, 4000) == WAIT_OBJECT_0;
        CloseHandle(completed);
        return done;
    }

    void Pause()
    {
        Idle();
        if (context && enabled)
            XeFGProxy::SetEnabled()(context, false);
        enabled = false;
        if (visible) Show(false);
        resetOnResume = true;
        previous = outputStart = {};
        outputFrames = 0;
        AmdPresentExperimental::childFgOutputFps = 0;
    }

    void Release()
    {
        Pause();
        if (worker.joinable())
        {
            {
                std::lock_guard hold(jobLock);
                quit = true;
            }
            jobChanged.notify_all();
            worker.join();
        }
        quit = presentFailed = false;
        job = {};
        swap.Reset();
        if (context)
            XeFGProxy::Destroy()(context);
        context = nullptr;
        if (xell)
            XeLLProxy::DestroyContext()(xell);
        xell = nullptr;
        // Generated frames still in flight read the hudless copy and the NR guides; D3D12 frees a
        // resource at once, so the queue drains before any of them goes.
        if (queue && fence && SUCCEEDED(queue->Signal(fence.Get(), ++serial)))
            Reached(serial);
        queue.Reset();
        command.Reset();
        for (auto& allocator : allocators)
            allocator.Reset();
        used[0] = used[1] = 0;
        fence.Reset();
        hudless.Reset();
        output.Destroy();
        window = nullptr;
        frameId = serial = 0;
        maximum = 0;
        settings.Reset();
        enabled = false;
        visible = false;
        resetOnResume = true;
        previous = {};
    }

    // `immediate`: the game presents without vsync. XeFG's frames may then tear like the game's; with vsync the
    // display's refresh rate limits what XeFG puts out, and through it the game's real frame rate.
    bool Create(HWND parent, ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t width, uint32_t height,
                DXGI_FORMAT format, bool immediate)
    {
        if (!parent || !XeFGProxy::InitXeFG() || !XeFGProxy::D3D12CreateContext() ||
            !XeFGProxy::GetProperties() || !XeFGProxy::D3D12InitFromSwapChainDesc() ||
            !XeFGProxy::D3D12GetSwapChainPtr() || !XeFGProxy::D3D12TagFrameResource() ||
            !XeFGProxy::TagFrameConstants() || !XeFGProxy::SetPresentId() ||
            !XeFGProxy::SetEnabled() || !XeFGProxy::GetLastPresentStatus())
            return false;
        if (!output.Create(parent, width, height))
            return false;
        this->queue = queue;
        window = output.window;
        const auto created = XeFGProxy::D3D12CreateContext()(device, &context);
        if (created != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            LOG_ERROR("Vulkan XeFG: D3D12CreateContext failed ({})", (int) created);
            return false;
        }
        if (XeFGProxy::SetLoggingCallback())
            XeFGProxy::SetLoggingCallback()(context, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, Log, nullptr);
        if (!XeLLProxy::InitXeLL() || !XeLLProxy::D3D12CreateContext() ||
            !XeLLProxy::SetSleepMode() || !XeLLProxy::DestroyContext() ||
            !XeFGProxy::SetLatencyReduction())
        {
            LOG_ERROR("Vulkan XeFG: XeLL functions unavailable");
            return false;
        }
        const auto xellCreated = XeLLProxy::D3D12CreateContext()(device, &xell);
        if (xellCreated != XELL_RESULT_SUCCESS)
        {
            LOG_ERROR("Vulkan XeFG: XeLL context failed ({})", (int) xellCreated);
            return false;
        }
        xell_sleep_params_t sleep {};
        sleep.bLowLatencyMode = true;
        const auto sleepSet = XeLLProxy::SetSleepMode()(xell, &sleep);
        const auto latencySet = XeFGProxy::SetLatencyReduction()(context, xell);
        if (sleepSet != XELL_RESULT_SUCCESS || latencySet != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            LOG_ERROR("Vulkan XeFG: XeLL setup failed ({}, {})", (int) sleepSet, (int) latencySet);
            return false;
        }
        xefg_swapchain_properties_t properties {};
        const auto queried = XeFGProxy::GetProperties()(context, &properties);
        if (queried != XEFG_SWAPCHAIN_RESULT_SUCCESS || properties.maxSupportedInterpolations == 0)
        {
            LOG_ERROR("Vulkan XeFG: GetProperties failed ({}, maximum {})", (int) queried,
                      properties.maxSupportedInterpolations);
            return false;
        }
        maximum = static_cast<int>(properties.maxSupportedInterpolations);
        DXGI_SWAP_CHAIN_DESC1 description {};
        description.Width = width;
        description.Height = height;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 3;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.Scaling = DXGI_SCALING_STRETCH;
        xefg_swapchain_d3d12_init_params_t parameters {};
        parameters.maxInterpolatedFrames = properties.maxSupportedInterpolations;
        parameters.uiMode = XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS;
        ComPtr<IDXGIFactory2> factory;
        const auto factoryResult = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(factoryResult))
        {
            LOG_ERROR("Vulkan XeFG: DXGI factory failed ({:X})", (UINT) factoryResult);
            return false;
        }
        ComPtr<IDXGIFactory5> factory5;
        BOOL allowed = FALSE;
        tearing =
            immediate && SUCCEEDED(factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowed, sizeof(allowed))) &&
            allowed;
        if (tearing)
            description.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        {
            ScopedVulkanCreatingSC creating;
            const auto initialized = XeFGProxy::D3D12InitFromSwapChainDesc()(
                context, window, &description, nullptr, queue, factory.Get(), &parameters);
            if (initialized != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                LOG_ERROR("Vulkan XeFG: InitFromSwapChainDesc failed ({})", (int) initialized);
                return false;
            }
            const auto obtained = XeFGProxy::D3D12GetSwapChainPtr()(context, IID_PPV_ARGS(&swap));
            if (obtained != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                LOG_ERROR("Vulkan XeFG: GetSwapChainPtr failed ({})", (int) obtained);
                return false;
            }
        }
        // The back buffer carries OptiScaler's menu and overlays and the hudless copy only the game, so
        // XeFG interpolates the game and lays the overlay unwarped over every generated frame.
        if (XeFGProxy::SetUiCompositionState())
        {
            const auto composition =
                XeFGProxy::SetUiCompositionState()(context, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
            if (composition != XEFG_SWAPCHAIN_RESULT_SUCCESS)
                LOG_WARN("Vulkan XeFG: UI composition unavailable ({})", (int) composition);
        }
        D3D12_HEAP_PROPERTIES heap { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC hudlessDesc {};
        hudlessDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        hudlessDesc.Width = width;
        hudlessDesc.Height = height;
        hudlessDesc.DepthOrArraySize = hudlessDesc.MipLevels = 1;
        hudlessDesc.Format = format;
        hudlessDesc.SampleDesc.Count = 1;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &hudlessDesc,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&hudless))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[0]))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[1]))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr,
                                             IID_PPV_ARGS(&command))) ||
            FAILED(command->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;
        const DWORD parentThread = GetWindowThreadProcessId(parent, nullptr);
        LOG_INFO("Vulkan XeFG presenter created at {}x{}, maximum {} interpolations, {}; parent thread {}, presenter "
                 "thread {}",
                 width, height, maximum, tearing ? "tearing like the game" : "no tearing", parentThread,
                 GetCurrentThreadId());
        return true;
    }

    // Copies the frame on the presenter's queue: `overlaid`, the final image with OptiScaler's overlay drawn on it,
    // becomes the back buffer and `colour`, the same image without, the hudless colour.
    bool Copy(ID3D12Resource* overlaid, ID3D12Resource* colour)
    {
        if (!swap)
            return false;
        auto& allocator = allocators[frameId % 2];
        ComPtr<ID3D12Resource> back;
        if (!Reached(used[frameId % 2]) ||
            FAILED(swap->GetBuffer(swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back))) ||
            FAILED(allocator->Reset()) || FAILED(command->Reset(allocator.Get(), nullptr)))
            return false;
        AmdPresentExperimental::Transition(command.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT,
                                            D3D12_RESOURCE_STATE_COPY_DEST);
        command->CopyResource(back.Get(), overlaid);
        AmdPresentExperimental::Transition(command.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                            D3D12_RESOURCE_STATE_PRESENT);
        AmdPresentExperimental::Transition(command.Get(), hudless.Get(), D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_COPY_DEST);
        command->CopyResource(hudless.Get(), colour);
        AmdPresentExperimental::Transition(command.Get(), hudless.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                           D3D12_RESOURCE_STATE_COMMON);
        if (FAILED(command->Close())) return false;
        ID3D12CommandList* lists[] { command.Get() };
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), ++serial))) return false;
        used[frameId % 2] = serial;
        return true;
    }

    // Tags the guides and presents the copied frame with the generated ones.
    bool Present(const AmdPresentExperimental::Guides& guides)
    {
        if (!swap || !guides.motion || !guides.depth)
            return false;
        if (!settings.Apply(context, maximum, "Vulkan"))
            return false;
        const uint32_t id = ++frameId;
        auto tag = [&](xefg_swapchain_resource_type_t type, ID3D12Resource* resource) {
            xefg_swapchain_d3d12_resource_data_t data {};
            data.type = type;
            data.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
            data.resourceSize = { guides.width, guides.height };
            data.pResource = resource;
            data.incomingState = D3D12_RESOURCE_STATE_COMMON;
            return XeFGProxy::D3D12TagFrameResource()(context, nullptr, id, &data) ==
                   XEFG_SWAPCHAIN_RESULT_SUCCESS;
        };
        if (!tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, guides.motion.Get()) ||
            !tag(XEFG_SWAPCHAIN_RES_DEPTH, guides.depth.Get()) || !tag(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, hudless.Get()))
            return false;
        xefg_swapchain_frame_constant_data_t constants {};
        for (int i : { 0, 5, 10, 15 })
            constants.viewMatrix[i] = constants.projectionMatrix[i] = 1.0f;
        constants.motionVectorScaleX = constants.motionVectorScaleY = 1.0f;
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = previous.time_since_epoch().count() ?
            std::chrono::duration<float, std::milli>(now - previous).count() : 16.7f;
        previous = now;
        constants.frameRenderTime = std::clamp(elapsed, 1.0f, 100.0f);
        XeFGPacing::NoteFedFrameTime(constants.frameRenderTime);
        constants.resetHistory = resetOnResume || guides.reset || elapsed > 250.0f;
        if (XeFGProxy::TagFrameConstants()(context, id, &constants) != XEFG_SWAPCHAIN_RESULT_SUCCESS ||
            XeFGProxy::SetPresentId()(context, id) != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            return false;
        if (!enabled)
        {
            if (XeFGProxy::SetEnabled()(context, true) != XEFG_SWAPCHAIN_RESULT_SUCCESS)
                return false;
            enabled = true;
        }
        if (!visible)
            Show(true);
        if (FAILED(swap->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0)))
            return false;
        resetOnResume = false;
        xefg_swapchain_present_status_t status {};
        if (XeFGProxy::GetLastPresentStatus()(context, &status) != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            return true;
        if (id == 2 || id % 30 == 0)
            LOG_INFO("Vulkan XeFG: frame {}, presented {}, generated {}, result {}, visible {}, output {:.1f} fps", id,
                     status.framesPresented, status.isFrameGenEnabled, (int) status.frameGenResult,
                     IsWindowVisible(window) != FALSE, AmdPresentExperimental::childFgOutputFps.load());
        // Frames that cannot tear reach the screen at most once a refresh.
        if (!outputStart.time_since_epoch().count())
            outputStart = now;
        else
            outputFrames += status.framesPresented;
        if (const float seconds = std::chrono::duration<float>(now - outputStart).count(); seconds >= 1.0f)
        {
            float fps = outputFrames / seconds;
            MONITORINFOEXW monitor {};
            monitor.cbSize = sizeof(monitor);
            DEVMODEW mode {};
            mode.dmSize = sizeof(mode);
            if (!tearing && GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
                EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
                fps = std::min(fps, static_cast<float>(mode.dmDisplayFrequency));
            AmdPresentExperimental::childFgOutputFps = fps;
            outputFrames = 0;
            outputStart = now;
        }
        return true;
    }
};

struct Bridge
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    uint32_t enabledFamily = UINT32_MAX;
    VkExtent2D extent {};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    HWND hwnd = nullptr;
    FgPresenter fg;
    bool fgFailed = false;
    std::atomic<bool> fgPending = false;
    uint32_t fgImage = 0;
    std::vector<VkImage> images;
    std::vector<VkSemaphore> ready, overlaid;
    VkCommandPool pool = VK_NULL_HANDLE;
    // A command buffer and the fence of its last submit.
    struct Step
    {
        VkCommandBuffer buffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool pending = false;
    };
    // The copies in, out and to the overlay of a present. With the GPU link two presents are in flight, each with
    // its own set; without it every copy is waited for on the CPU and step [0][0] serves them all.
    Step steps[2][3];
    uint32_t set = 0;
    // The Vulkan queue and queue12 order their work on the GPU through a D3D12 fence shared as a Vulkan
    // semaphore, so a present goes on while its NR runs. `linkValue` is the last value either side was given,
    // `d3d12Done` the last one queue12 signals after reading the shared images, `linkWaited` the last one a Vulkan
    // submit waited for, on `linkQueue`.
    ComPtr<ID3D12Fence> link12;
    VkSemaphore link = VK_NULL_HANDLE;
    uint64_t linkValue = 0, d3d12Done = 0, linkWaited = 0;
    VkQueue linkQueue = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool copyAllowed = false;
    bool stopped = false;
    bool gpuInterop = false;
    bool sharedInitialized = false;
    VkImage sharedImage = VK_NULL_HANDLE, overlayImage = VK_NULL_HANDLE;
    VkDeviceMemory sharedMemory = VK_NULL_HANDLE, overlayMemory = VK_NULL_HANDLE;
    bool overlayInitialized = false;
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12Resource> shared12, overlay12;
    AmdPresentExperimental::Guides fgGuides;
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11Texture2D> colour, readback;

    void Release()
    {
        fg.Release();
        // The game may be submitting to its queue from another thread; our own fences need no queue access.
        Settle();
        if (shared12)
            AmdPresentExperimental::BeforeResize();
        if (link)
            vkDestroySemaphore(device, link, nullptr);
        link = VK_NULL_HANDLE;
        link12.Reset();
        linkValue = d3d12Done = linkWaited = 0;
        linkQueue = VK_NULL_HANDLE;
        set = 0;
        shared12.Reset();
        overlay12.Reset();
        fgGuides = {};
        for (auto image : { sharedImage, overlayImage })
            if (image)
                vkDestroyImage(device, image, nullptr);
        for (auto memory : { sharedMemory, overlayMemory })
            if (memory)
                vkFreeMemory(device, memory, nullptr);
        sharedImage = overlayImage = VK_NULL_HANDLE;
        sharedMemory = overlayMemory = VK_NULL_HANDLE;
        gpuInterop = sharedInitialized = overlayInitialized = fgPending = false;
        if (mapped) vkUnmapMemory(device, memory);
        mapped = nullptr;
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        for (auto s : ready) if (s) vkDestroySemaphore(device, s, nullptr);
        for (auto s : overlaid)
            if (s)
                vkDestroySemaphore(device, s, nullptr);
        for (auto& row : steps)
            for (auto& step : row)
            {
                if (step.fence)
                    vkDestroyFence(device, step.fence, nullptr);
                step = {};
            }
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        pool = VK_NULL_HANDLE;
        ready.clear();
        overlaid.clear();
        images.clear();
        readback.Reset(); colour.Reset();
        swapchain = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
        copyAllowed = false; stopped = false;
        fgFailed = false;
    }

    // The D3D12 and D3D11 objects depend on the GPU only, so a new VkDevice on the same GPU keeps them, and the
    // final-image NR context made on them keeps working.
    bool SameAdapter(VkPhysicalDevice next) const
    {
        LUID luid {};
        if (d12)
            luid = d12->GetAdapterLuid();
        else
        {
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC description {};
            if (!d11 || FAILED(d11.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
                FAILED(adapter->GetDesc(&description)))
                return false;
            luid = description.AdapterLuid;
        }
        VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties.pNext = &id;
        vkGetPhysicalDeviceProperties2(next, &properties);
        return id.deviceLUIDValid && std::memcmp(&luid, id.deviceLUID, sizeof(luid)) == 0;
    }

    bool InitD3D11()
    {
        if (!d11)
        {
        VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties.pNext = &id;
        vkGetPhysicalDeviceProperties2(physical, &properties);
        if (!id.deviceLUIDValid)
        {
            AmdPresentExperimental::Report("Vulkan NR: GPU has no Windows adapter LUID");
            return false;
        }
        LUID wanted {};
        std::memcpy(&wanted, id.deviceLUID, sizeof(wanted));
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 description {};
            if (SUCCEEDED(candidate->GetDesc1(&description)) &&
                description.AdapterLuid.LowPart == wanted.LowPart &&
                description.AdapterLuid.HighPart == wanted.HighPart)
            {
                adapter = candidate;
                break;
            }
        }
        if (!adapter)
        {
            AmdPresentExperimental::Report("Vulkan NR: matching D3D11 adapter unavailable");
            return false;
        }
        const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL selected {};
        auto hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                    &d11, &selected, &immediate);
        if (hr == E_INVALIDARG)
            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1, D3D11_SDK_VERSION,
                                   &d11, &selected, &immediate);
        if (FAILED(hr)) return false;
        }
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = extent.width; desc.Height = extent.height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB
                          ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(d11->CreateTexture2D(&desc, nullptr, &colour))) return false;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(d11->CreateTexture2D(&desc, nullptr, &readback));
    }

    bool InitInterop()
    {
        if (!vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandlePropertiesKHR"))
            return false;
        if (!d12)
        {
            VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
            VkPhysicalDeviceProperties2 properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            properties.pNext = &id;
            vkGetPhysicalDeviceProperties2(physical, &properties);
            if (!id.deviceLUIDValid) return false;
            LUID wanted {};
            std::memcpy(&wanted, id.deviceLUID, sizeof(wanted));
            ComPtr<IDXGIFactory1> factory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0;; ++i)
            {
                ComPtr<IDXGIAdapter1> candidate;
                if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 description {};
                if (SUCCEEDED(candidate->GetDesc1(&description)) &&
                    description.AdapterLuid.LowPart == wanted.LowPart &&
                    description.AdapterLuid.HighPart == wanted.HighPart)
                { adapter = candidate; break; }
            }
            if (!adapter || FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                                    IID_PPV_ARGS(&d12))))
                return false;
            D3D12_COMMAND_QUEUE_DESC queueDesc {};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue12)))) return false;
        }
        return CreateShared(sharedImage, sharedMemory, shared12);
    }

    // Shares a D3D12 fence with Vulkan as the GPU link. A device without the semaphore import keeps the CPU waits.
    bool InitLink()
    {
        auto import = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(device, "vkImportSemaphoreWin32HandleKHR"));
        HANDLE handle = nullptr;
        if (!import || FAILED(d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&link12))) ||
            FAILED(d12->CreateSharedHandle(link12.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
        {
            link12.Reset();
            return false;
        }
        VkSemaphoreCreateInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkImportSemaphoreWin32HandleInfoKHR imported { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
        imported.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        imported.handle = handle;
        bool linked = vkCreateSemaphore(device, &info, nullptr, &link) == VK_SUCCESS;
        imported.semaphore = link;
        linked = linked && import(device, &imported) == VK_SUCCESS;
        CloseHandle(handle);
        if (!linked)
        {
            if (link)
                vkDestroySemaphore(device, link, nullptr);
            link = VK_NULL_HANDLE;
            link12.Reset();
        }
        return linked;
    }

    // A D3D12 texture of the swapchain size, imported into Vulkan as `target`.
    bool CreateShared(VkImage& target, VkDeviceMemory& targetMemory, ComPtr<ID3D12Resource>& target12)
    {
        auto getHandleProperties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
            vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandlePropertiesKHR"));
        const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
        D3D12_HEAP_PROPERTIES heapProperties {};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC resourceDesc {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Width = extent.width;
        resourceDesc.Height = extent.height;
        resourceDesc.DepthOrArraySize = resourceDesc.MipLevels = 1;
        resourceDesc.Format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                             (bgra ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
        ComPtr<ID3D12Resource> candidate;
        if (FAILED(d12->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_SHARED, &resourceDesc,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&candidate))))
            return false;
        HANDLE handle = nullptr;
        if (FAILED(d12->CreateSharedHandle(candidate.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
            return false;
        VkExternalMemoryImageCreateInfo external { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageCreateInfo imageInfo { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.pNext = &external;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = { extent.width, extent.height, 1 };
        imageInfo.mipLevels = imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        bool success = false;
        do
        {
            if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) break;
            VkMemoryWin32HandlePropertiesKHR handleProperties { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
            if (getHandleProperties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
                                    handle, &handleProperties) != VK_SUCCESS) break;
            VkMemoryRequirements requirements {};
            vkGetImageMemoryRequirements(device, image, &requirements);
            VkPhysicalDeviceMemoryProperties memoryProperties {};
            vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
            uint32_t type = UINT32_MAX;
            const uint32_t allowed = requirements.memoryTypeBits & handleProperties.memoryTypeBits;
            for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
                if ((allowed & (1u << i)) &&
                    (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                { type = i; break; }
            if (type == UINT32_MAX) break;
            VkMemoryDedicatedAllocateInfo dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
            dedicated.image = image;
            VkImportMemoryWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
            import.pNext = &dedicated;
            import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
            import.handle = handle;
            VkMemoryAllocateInfo allocation { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocation.pNext = &import;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = type;
            if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
                vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS)
                break;
            success = true;
        } while (false);
        CloseHandle(handle);
        if (!success)
        {
            if (image) vkDestroyImage(device, image, nullptr);
            if (memory) vkFreeMemory(device, memory, nullptr);
            return false;
        }
        target = image;
        targetMemory = memory;
        target12 = candidate;
        return true;
    }

    bool InitVulkan()
    {
        if (extent.width == 0 || extent.height == 0 || extent.width > 16384 || extent.height > 16384 ||
            images.empty() || enabledFamily == UINT32_MAX || !copyAllowed)
            return false;
        family = enabledFamily;
        VkQueue expected = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, family, 0, &expected);
        if (queue != expected)
        {
            AmdPresentExperimental::Report("Vulkan NR: present queue differs from graphics queue");
            return false;
        }
        VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo alloc { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        for (auto& row : steps)
            for (auto& step : row)
                if (vkAllocateCommandBuffers(device, &alloc, &step.buffer) != VK_SUCCESS ||
                    vkCreateFence(device, &fenceInfo, nullptr, &step.fence) != VK_SUCCESS)
                    return false;
        ready.resize(images.size(), VK_NULL_HANDLE);
        overlaid.resize(images.size(), VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphoreInfo { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        for (auto& s : ready)
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &s) != VK_SUCCESS) return false;
        for (auto& s : overlaid)
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &s) != VK_SUCCESS)
                return false;
        if (InitInterop())
        {
            gpuInterop = true;
            LOG_INFO("Vulkan final-image NR: D3D12 shared GPU image active (no host readback)");
            if (InitLink())
                LOG_INFO("Vulkan final-image NR: Vulkan and D3D12 ordered on the GPU through a shared fence");
            else
                LOG_WARN("Vulkan final-image NR: no shared fence, every frame waits for NR on the CPU");
            return true;
        }
        LOG_WARN("Vulkan final-image NR: D3D12 image import unavailable, using host readback fallback");
        VkBufferCreateInfo bufferInfo { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = VkDeviceSize(extent.width) * extent.height * 4;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements {};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties {};
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memoryProperties.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            { type = i; break; }
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo memoryInfo { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        memoryInfo.allocationSize = requirements.size; memoryInfo.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &memoryInfo, nullptr, &memory) != VK_SUCCESS ||
            vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS ||
            vkMapMemory(device, memory, 0, bufferInfo.size, 0, &mapped) != VK_SUCCESS)
            return false;
        return InitD3D11();
    }

    void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout before, VkImageLayout after,
                      VkAccessFlags source, VkAccessFlags target)
    {
        VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = source; barrier.dstAccessMask = target;
        barrier.oldLayout = before; barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    bool Begin(Step& step)
    {
        if (step.pending && vkWaitForFences(device, 1, &step.fence, VK_TRUE, 4'000'000'000ull) != VK_SUCCESS)
            return false;
        step.pending = false;
        if (vkResetFences(device, 1, &step.fence) != VK_SUCCESS || vkResetCommandBuffer(step.buffer, 0) != VK_SUCCESS)
            return false;
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkBeginCommandBuffer(step.buffer, &begin) == VK_SUCCESS;
    }

    // Submits `step` with `waits` and `signals`; on the GPU link the semaphore takes the value beside it, the
    // others ignore theirs.
    bool Submit(Step& step, const std::vector<VkSemaphore>& waits, const std::vector<uint64_t>& waitValues,
                const std::vector<VkSemaphore>& signals, const std::vector<uint64_t>& signalValues)
    {
        if (vkEndCommandBuffer(step.buffer) != VK_SUCCESS)
            return false;
        std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkD3D12FenceSubmitInfoKHR values { VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR };
        values.waitSemaphoreValuesCount = static_cast<uint32_t>(waitValues.size());
        values.pWaitSemaphoreValues = waitValues.data();
        values.signalSemaphoreValuesCount = static_cast<uint32_t>(signalValues.size());
        values.pSignalSemaphoreValues = signalValues.data();
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        if (link)
            submit.pNext = &values;
        submit.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
        submit.pWaitSemaphores = waits.data();
        submit.pWaitDstStageMask = stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &step.buffer;
        submit.signalSemaphoreCount = static_cast<uint32_t>(signals.size());
        submit.pSignalSemaphores = signals.data();
        if (vkQueueSubmit(queue, 1, &submit, step.fence) != VK_SUCCESS)
            return false;
        step.pending = true;
        for (size_t i = 0; link && i < waits.size(); ++i)
            if (waits[i] == link)
            {
                linkWaited = waitValues[i];
                linkQueue = queue;
            }
        return true;
    }

    bool Finish(Step& step)
    {
        if (vkWaitForFences(device, 1, &step.fence, VK_TRUE, 4'000'000'000ull) != VK_SUCCESS)
            return false;
        step.pending = false;
        return true;
    }

    void SharedBarrier(VkCommandBuffer cmd, VkImageLayout before, VkImageLayout after, uint32_t sourceFamily,
                       uint32_t targetFamily, VkAccessFlags sourceAccess, VkAccessFlags targetAccess,
                       VkPipelineStageFlags sourceStage, VkPipelineStageFlags targetStage, VkImage image)
    {
        VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = sourceAccess;
        barrier.dstAccessMask = targetAccess;
        barrier.oldLayout = before;
        barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = sourceFamily;
        barrier.dstQueueFamilyIndex = targetFamily;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, sourceStage, targetStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // Copies `source` into `target` where one of them is a shared image, after this queue's earlier copies of it.
    void CopyShared(VkCommandBuffer cmd, VkImage source, VkImage target, bool into, bool initialized)
    {
        const VkImage shared = into ? target : source;
        const VkImage image = into ? source : target;
        ImageBarrier(cmd, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     into ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                     into ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT);
        SharedBarrier(cmd, initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                      into ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_QUEUE_FAMILY_EXTERNAL, family, 0,
                      into ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, shared);
        VkImageCopy region {};
        region.srcSubresource.aspectMask = region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = region.dstSubresource.layerCount = 1;
        region.extent = { extent.width, extent.height, 1 };
        vkCmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region);
        SharedBarrier(cmd, into ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_GENERAL, family, VK_QUEUE_FAMILY_EXTERNAL,
                      into ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, shared);
        ImageBarrier(cmd, image, into ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, into ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
                     0);
    }

    // The game's wait semaphores are spent once a submit has waited on them. Until the present gets ours, it
    // waits on none.
    static void Spent(VkPresentInfoKHR& present)
    {
        present.waitSemaphoreCount = 0;
        present.pWaitSemaphores = nullptr;
    }

    // After a failed step the present goes out once our submits have finished. Returns false only when the
    // device is lost.
    bool Settle()
    {
        VkResult result = VK_SUCCESS;
        for (auto& row : steps)
            for (auto& step : row)
                if (step.pending)
                {
                    const VkResult waited = vkWaitForFences(device, 1, &step.fence, VK_TRUE, UINT64_MAX);
                    step.pending = waited != VK_SUCCESS;
                    if (waited != VK_SUCCESS)
                        result = waited;
                }
        if (result != VK_SUCCESS)
            LOG_ERROR("Vulkan final-image bridge: fence wait returned {}", (int) result);
        return result != VK_ERROR_DEVICE_LOST;
    }

    // With the GPU link nothing here waits for NR: queue12 starts once the image is in, and the copy back waits
    // for NR on the GPU. Without it each step is waited for on the CPU.
    bool ProcessShared(VkPresentInfoKHR& present, bool neural, bool wantFg)
    {
        const uint32_t imageIndex = present.pImageIndices[0];
        if (imageIndex >= images.size())
            return false;
        fg.Idle();
        if (fg.presentFailed)
        {
            fg.Release();
            fgFailed = true;
            LOG_ERROR("Vulkan XeFG: presentation failed; NR will continue");
        }
        if (link)
            set ^= 1;
        Step& in = steps[set][0];
        Step& out = steps[set][link ? 1 : 0];
        if (!Begin(in))
            return false;
        auto image = images[imageIndex];
        CopyShared(in.buffer, image, sharedImage, true, sharedInitialized);
        // Besides the game's semaphores the copy waits until queue12 has read the shared images of the last frame.
        // A value is waited for once, as a binary semaphore requires: when the last copy back already waited for it
        // on this queue, the copy's first barrier, which covers every earlier command of the queue, orders the copy
        // after that one.
        std::vector<VkSemaphore> waits(present.pWaitSemaphores, present.pWaitSemaphores + present.waitSemaphoreCount);
        std::vector<uint64_t> waitValues(waits.size(), 0);
        if (link && d3d12Done > linkWaited)
        {
            waits.push_back(link);
            waitValues.push_back(d3d12Done);
        }
        else if (link && d3d12Done && linkQueue != queue && !Settle())
            return false;
        const uint64_t copied = linkValue + 1;
        if (!Submit(in, waits, waitValues, link ? std::vector { link } : std::vector<VkSemaphore> {}, { copied }))
            return false;
        Spent(present);
        if (link)
            linkValue = copied;
        else if (!Finish(in))
            return false;
        sharedInitialized = true;

        auto settings = DlssNr::AmdBridge::SettingsFromConfig(
            *Config::Instance(), Config::Instance()->AmdNrScale.value_or_default());
        settings.spinDraw = 0;
        if (link && FAILED(queue12->Wait(link12.Get(), copied)))
            return false;
        if (!AmdPresentExperimental::RenderResource(shared12.Get(), d12.Get(), queue12.Get(),
                                                    Util::DllPath().parent_path(), settings, &fgGuides, neural,
                                                    link != VK_NULL_HANDLE))
            stopped = true;
        const uint64_t processed = linkValue + 1;
        if (link)
        {
            if (FAILED(queue12->Signal(link12.Get(), processed)))
                return false;
            linkValue = d3d12Done = processed;
        }
        static bool loggedGuides = false;
        if (fgGuides.motion && fgGuides.depth && !loggedGuides)
        {
            LOG_INFO("Vulkan XeFG inputs ready: D3D12 colour, motion and depth at {}x{}",
                     fgGuides.width, fgGuides.height);
            loggedGuides = true;
        }
        const HWND root = GetAncestor(hwnd, GA_ROOT);
        const HWND foreground = GetForegroundWindow();
        const bool background = root && IsWindowVisible(root) && foreground &&
            GetAncestor(foreground, GA_ROOT) != root;
        if (!wantFg || background)
        {
            if (fg.window && (fg.enabled || fg.visible))
            {
                fg.Pause();
                LOG_INFO("Vulkan XeFG paused while {}",
                         !wantFg ? "frame generation is off" : "game window is in background");
            }
            if (!wantFg)
                fgFailed = false;
        }
        if (wantFg && !background && !fgFailed && fgGuides.motion && fgGuides.depth)
        {
            if ((!overlay12 && !CreateShared(overlayImage, overlayMemory, overlay12)) ||
                (!fg.window && !fg.Create(hwnd, d12.Get(), queue12.Get(), extent.width, extent.height,
                                          shared12->GetDesc().Format, presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR)))
            {
                fg.Release();
                fgFailed = true;
                LOG_ERROR("Vulkan XeFG: D3D12 presenter initialization failed");
            }
            fgPending = fg.window != nullptr;
            fgImage = imageIndex;
        }
        else if (fg.window && !fgGuides.motion)
            fg.Pause();

        if (!Begin(out))
            return false;
        CopyShared(out.buffer, sharedImage, image, false, true);
        if (!Submit(out, link ? std::vector { link } : std::vector<VkSemaphore> {}, { processed },
                    { ready[imageIndex] }, { 0 }))
            return false;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &ready[imageIndex];
        return true;
    }

    // Runs once OptiScaler's overlay is in the swapchain image: hands that image to XeFG as the back
    // buffer and the NR output, still in shared12, as the hudless colour.
    bool PresentGenerated(VkPresentInfoKHR& present)
    {
        fgPending = false;
        // A frame marked before a present that failed further down is not this one; it is skipped.
        const uint32_t imageIndex = present.pImageIndices[0];
        if (imageIndex != fgImage)
            return true;
        Step& copy = steps[set][link ? 2 : 0];
        if (!Begin(copy))
            return false;
        CopyShared(copy.buffer, images[imageIndex], overlayImage, true, overlayInitialized);
        std::vector<VkSemaphore> waits(present.pWaitSemaphores, present.pWaitSemaphores + present.waitSemaphoreCount);
        std::vector<VkSemaphore> signals { overlaid[imageIndex] };
        const uint64_t shown = linkValue + 1;
        if (link)
            signals.push_back(link);
        if (!Submit(copy, waits, std::vector<uint64_t>(waits.size(), 0), signals, { 0, shown }))
            return false;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &overlaid[imageIndex];
        if (link)
            linkValue = shown;
        else if (!Finish(copy))
            return false;
        overlayInitialized = true;
        // queue12 copies both images once they are in and tells Vulkan it has read them before XeFG presents;
        // without the link the CPU waits for that copy instead.
        bool copied =
            (!link || SUCCEEDED(queue12->Wait(link12.Get(), shown))) && fg.Copy(overlay12.Get(), shared12.Get());
        if (link)
        {
            const uint64_t read = linkValue + 1;
            if (FAILED(queue12->Signal(link12.Get(), read)))
                return false;
            linkValue = d3d12Done = read;
        }
        else
            copied = copied && fg.Reached(fg.serial);
        if (copied)
            fg.Post(fgGuides);
        else
        {
            fg.Release();
            fgFailed = true;
            LOG_ERROR("Vulkan XeFG: presentation failed; NR will continue");
        }
        return true;
    }

    bool Process(VkPresentInfoKHR& present, bool neural, bool wantFg)
    {
        if (gpuInterop)
            return ProcessShared(present, neural, wantFg);
        // The host readback has no guides-only frame and no XeFG.
        if (!neural)
            return true;
        const uint32_t imageIndex = present.pImageIndices[0];
        if (imageIndex >= images.size()) return false;
        Step& step = steps[0][0];
        if (!Begin(step))
            return false;
        auto image = images[imageIndex];
        ImageBarrier(step.buffer, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                     VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { extent.width, extent.height, 1 };
        vkCmdCopyImageToBuffer(step.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        ImageBarrier(step.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_ACCESS_TRANSFER_READ_BIT, 0);
        VkBufferMemoryBarrier hostRead { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostRead.buffer = buffer; hostRead.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(step.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                             &hostRead, 0, nullptr);
        if (!Submit(step, { present.pWaitSemaphores, present.pWaitSemaphores + present.waitSemaphoreCount }, {}, {},
                    {}))
            return false;
        Spent(present);
        if (!Finish(step))
            return false;

        const UINT pitch = extent.width * 4;
        immediate->UpdateSubresource(colour.Get(), 0, nullptr, mapped, pitch, 0);
        auto settings = DlssNr::AmdBridge::SettingsFromConfig(
            *Config::Instance(), Config::Instance()->AmdNrScale.value_or_default());
        settings.spinDraw = 0;
        if (!AmdPresentExperimental::RenderTexture11(colour.Get(), d11.Get(), Util::DllPath().parent_path(), settings))
            stopped = true;
        immediate->CopyResource(readback.Get(), colour.Get());
        D3D11_MAPPED_SUBRESOURCE result {};
        if (FAILED(immediate->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &result))) return false;
        for (UINT y = 0; y < extent.height; ++y)
            std::memcpy(static_cast<unsigned char*>(mapped) + size_t(y) * pitch,
                        static_cast<unsigned char*>(result.pData) + size_t(y) * result.RowPitch, pitch);
        immediate->Unmap(readback.Get(), 0);

        if (!Begin(step))
            return false;
        VkBufferMemoryBarrier hostWrite { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        hostWrite.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        hostWrite.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        hostWrite.srcQueueFamilyIndex = hostWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostWrite.buffer = buffer; hostWrite.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(step.buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                             &hostWrite, 0, nullptr);
        ImageBarrier(step.buffer, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(step.buffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        ImageBarrier(step.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        if (!Submit(step, {}, {}, { ready[imageIndex] }, {}))
            return false;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &ready[imageIndex];
        return true;
    }
};

std::mutex mutex;
Bridge bridge;
Timing nrTiming, fgTiming;
}

void Created(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, HWND hwnd,
             uint32_t graphicsFamily, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& description)
{
    std::lock_guard lock(mutex);
    const bool sameDevice = bridge.device == device;
    bridge.Release();
    if (!sameDevice && !bridge.SameAdapter(physicalDevice))
    {
        bridge.queue12.Reset();
        bridge.d12.Reset();
        bridge.immediate.Reset();
        bridge.d11.Reset();
    }
    bridge.instance = instance; bridge.physical = physicalDevice; bridge.device = device;
    bridge.hwnd = hwnd;
    bridge.enabledFamily = graphicsFamily;
    bridge.swapchain = swapchain; bridge.extent = description.imageExtent;
    bridge.format = description.imageFormat;
    bridge.presentMode = description.presentMode;
    const bool rgba8 = bridge.format == VK_FORMAT_R8G8B8A8_UNORM ||
                       bridge.format == VK_FORMAT_R8G8B8A8_SRGB ||
                       bridge.format == VK_FORMAT_B8G8R8A8_UNORM ||
                       bridge.format == VK_FORMAT_B8G8R8A8_SRGB;
    bridge.copyAllowed = rgba8 && (description.imageUsage &
        (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ==
        (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    LOG_INFO("Vulkan final-image bridge: swapchain {}x{}, format {}, transfer {}, present mode {}", bridge.extent.width,
             bridge.extent.height, (int) bridge.format, bridge.copyAllowed, (int) bridge.presentMode);
    uint32_t count = 0;
    if (vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr) == VK_SUCCESS && count > 0)
    {
        bridge.images.resize(count);
        if (vkGetSwapchainImagesKHR(device, swapchain, &count, bridge.images.data()) != VK_SUCCESS)
            bridge.images.clear();
    }
}

void Destroyed(VkDevice device, VkSwapchainKHR swapchain)
{
    std::lock_guard lock(mutex);
    if (bridge.device == device && bridge.swapchain == swapchain) bridge.Release();
}

bool Process(VkQueue queue, VkPresentInfoKHR& present)
{
    auto config = Config::Instance();
    const bool neural = config->DlssNrEnabled.value_or_default();
    // With NR off XeFG keeps running on the optical-flow guides alone, as on the DXGI routes.
    const bool wantFg = config->FGEnabled.value_or_default() && State::Instance().activeFgInput == FGInput::Upscaler &&
                        State::Instance().activeFgOutput == FGOutput::XeFG;
    static bool neuralBefore = true;
    if (config->DlssNrPresent.value_or_default() && neural != neuralBefore)
    {
        neuralBefore = neural;
        LOG_INFO("Vulkan final-image bridge: NR {}", neural ? "on" : wantFg ? "off, XeFG runs on its guides" : "off");
    }
    if ((!neural && !wantFg) || !config->DlssNrPresent.value_or_default() ||
        !DlssNr::Backend::PresentRuntimeSupported() || present.swapchainCount != 1 ||
        State::Instance().swapchainApi != API::Vulkan)
    {
        bridge.fgPending = false;
        // XeFG shows the last frame it was given until it is paused.
        if (fgLive)
        {
            std::lock_guard lock(mutex);
            bridge.fg.Pause();
            LOG_INFO("Vulkan XeFG paused while {}", wantFg ? "final-image NR is off" : "frame generation is off");
        }
        return true;
    }
    static std::once_flag logged;
    std::call_once(logged, [&] {
        LOG_INFO("Vulkan final-image bridge: enabled {}, final image {}, backend {}, API {}",
                 config->DlssNrEnabled.value_or_default(), config->DlssNrPresent.value_or_default(),
                 config->NrBackend.value_or_default(), (int) State::Instance().swapchainApi);
    });
    std::lock_guard lock(mutex);
    bridge.fgPending = false;
    // A game with an upscaler runs NR inside the upscaler call, so the final image stands aside and hides its
    // XeFG window in case the upscaler appeared mid-session.
    const bool upscaler = State::Instance().currentFeature != nullptr;
    if (!upscaler && bridge.swapchain == VK_NULL_HANDLE)
        AmdPresentExperimental::Report("Vulkan NR: the game made its swapchain before NR without upscaling was on; "
                                       "restart the game");
    if (upscaler || bridge.swapchain != present.pSwapchains[0] || bridge.stopped)
    {
        bridge.fg.Pause();
        return true;
    }
    bridge.queue = queue;
    if (!bridge.pool && !bridge.InitVulkan())
    {
        bridge.stopped = true;
        AmdPresentExperimental::Report("Vulkan NR: bridge initialization failed");
        return true;
    }
    const auto start = std::chrono::steady_clock::now();
    if (!bridge.Process(present, neural, wantFg))
    {
        bridge.stopped = true;
        AmdPresentExperimental::Report("Vulkan NR: copy or synchronization failed; NR is off until the game restarts");
        return bridge.Settle();
    }
    nrTiming.Add(start, bridge.gpuInterop ? "shared D3D12 image" : "host readback");
    static std::once_flag copiedOnce;
    std::call_once(copiedOnce, [&] {
        LOG_INFO("Vulkan final-image bridge: first frame processed using {}",
                 bridge.gpuInterop ? "shared D3D12 image" : "host readback");
    });
    return true;
}

bool PresentGenerated(VkPresentInfoKHR& present)
{
    if (!bridge.fgPending)
        return true;
    std::lock_guard lock(mutex);
    if (!bridge.fgPending || bridge.swapchain != present.pSwapchains[0])
        return true;
    const auto start = std::chrono::steady_clock::now();
    if (bridge.PresentGenerated(present))
    {
        fgTiming.Add(start, "XeFG copy");
        return true;
    }
    bridge.stopped = true;
    bridge.fg.Release();
    AmdPresentExperimental::Report("Vulkan NR: copy or synchronization failed; NR is off until the game restarts");
    return bridge.Settle();
}
} // namespace AmdVkPresent
