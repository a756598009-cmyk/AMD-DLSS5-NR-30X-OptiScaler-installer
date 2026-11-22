#include "pch.h"
#include <dlssnr/amd/PresentExperimental.h>
#include <dlssnr/amd/AmdBridge.h>
#include <dlssnr/backend/Selector.h>
#include "menu_overlay_base.h"
#include "menu_overlay_dx.h"

#include <Util.h>
#include <Logger.h>
#include <Config.h>
#include <hooks/FG_Hooks.h>
#include <proxies/XeFGPacing.h>
#include <misc/Screenshot.h>

#include <imgui/imgui_impl_dx11.h>
#include <imgui/imgui_impl_dx12.h>
#include <imgui/imgui_impl_win32.h>

#include "menu_common.h"
#include <shaders/menu_blur/MenuBlur_Dx11.h>
#include <shaders/menu_blur/MenuBlur_Dx12.h>

// menu
// Sized for the largest swapchain DXGI allows, the menu can draw on chains created by other layers
static int const NUM_BACK_BUFFERS = 16; // DXGI_MAX_SWAP_CHAIN_BUFFERS
static int const SRV_HEAP_SIZE = 64;
static bool _dx11Device = false;
static bool _dx12Device = false;

// for dx11
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static ID3D11RenderTargetView* g_pd3dRenderTarget = nullptr;
// A D3D10 game's frame on OptiScaler's own D3D11 device (AmdPresentExperimental::Bridge10): the menu draws there, on
// the device the frame crossed to, instead of the swapchain's back buffer. _targetView is the texture the view is on.
static ID3D11Texture2D* _dx10Target = nullptr;
static ID3D11Texture2D* _targetView = nullptr;
static std::unique_ptr<MenuBlur_Dx11> g_menuBlurDx11;

// for dx12
static ID3D12Device* g_pd3dDeviceParam = nullptr;
static ID3D12DescriptorHeap* g_pd3dRtvDescHeap = nullptr;
static ID3D12DescriptorHeap* g_pd3dSrvDescHeap = nullptr;
static DescriptorHeapAllocator g_pd3dSrvDescHeapAlloc;
static ID3D12CommandQueue* g_pd3dCommandQueue = nullptr;
static ID3D12GraphicsCommandList* g_pd3dCommandList = nullptr;
static ID3D12CommandAllocator* g_commandAllocators[NUM_BACK_BUFFERS] = {};
static ID3D12Resource* g_mainRenderTargetResource[NUM_BACK_BUFFERS] = {};
static D3D12_CPU_DESCRIPTOR_HANDLE g_mainRenderTargetDescriptor[NUM_BACK_BUFFERS] = {};
static std::unique_ptr<MenuBlur_Dx12> g_menuBlur;

// Tracks GPU completion of menu command lists so an allocator is never reset while still executing
static ID3D12Fence* g_menuFence = nullptr;
static HANDLE g_menuFenceEvent = nullptr;
static UINT64 g_menuFenceValue = 0;
static UINT64 g_allocatorFenceValues[NUM_BACK_BUFFERS] = {};

// current command queue for dx12 swapchain
static IUnknown* currentSCCommandQueue = nullptr;

// status
static bool _isInited = false;
// With final-image NR, XeFG draws the overlay from its own present thread while the game thread may release it for a
// resize.
static std::recursive_mutex _overlayMutex;
static std::atomic<bool> _heldForResize = false;
static bool _d3d12Captured = false;

// Presents that reached the swapchain frame generation presents to and the longest time between two of them, for the
// final-image rate line.
static std::atomic<UINT64> _fgOutputPresents = 0;
static std::atomic<double> _fgLongestGapMs = 0, _fgLastOutputMs = 0;
// XeFG presents one frame of each burst from the game's thread, inside the game's Present, and the others from a thread
// of its own right after it, so above 2X a whole burst reaches the screen within a millisecond or two. With final-image
// NR on D3D12, or D3D11 through D3D12, those others are spread over the time the game spends outside frame generation's
// Present, on XeFG's thread only, and the pacing hooks in XeFG (XeFGPacing.h) only forward meanwhile, unless
// XeFG\ExtraPacing=true asks for them. A wait ends when the game's next frame reaches frame generation, so the game
// never waits for it.
static std::atomic<DWORD> _fgGameThread = 0;
static std::atomic<double> _fgPeriodMs = 0, _fgReturnedMs = 0, _fgSpacingUntilMs = 0;
static std::atomic<UINT64> _fgFrames = 0;

void MenuOverlayDx::FrameGenerationPresent(bool returned)
{
    const double now = Util::MillisecondsNow();
    if (returned)
    {
        _fgReturnedMs = now;
        return;
    }
    const double interval = now - _fgReturnedMs;
    _fgPeriodMs = _fgReturnedMs == 0 || interval > 100 ? 0
                  : _fgPeriodMs > 0                    ? 0.9 * _fgPeriodMs + 0.1 * interval
                                                       : interval;
    _fgFrames++;
}

void MenuOverlayDx::SpaceFrameGenerationPresent(UINT flags)
{
    static std::atomic<double> burstStart = 0;
    static std::atomic<UINT> burstIndex = 0;
    static std::atomic<UINT64> burstFrame = 0;
    const double now = Util::MillisecondsNow();
    const auto& state = State::Instance();
    if ((flags & DXGI_PRESENT_TEST) || now > _fgSpacingUntilMs || state.currentFGSwapchain == nullptr)
        return;
    if (GetCurrentThreadId() == _fgGameThread)
    {
        burstStart = now;
        burstIndex = 0;
        burstFrame = _fgFrames.load();
        return;
    }
    auto fg = state.currentFG;
    const double period = _fgPeriodMs;
    if (burstStart == 0 || period <= 0 || fg == nullptr || _fgFrames != burstFrame)
        return;
    const UINT count = fg->GetInterpolatedFrameCount();
    const double until = burstStart + ++burstIndex * period / (count + 1);
    if (burstIndex > count || until <= now || until - now > period)
        return;
    // A high resolution timer a millisecond at a time, then a spin.
    struct Timer
    {
        HANDLE handle =
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        ~Timer()
        {
            if (handle)
                CloseHandle(handle);
        }
    };
    static thread_local Timer timer;
    for (double left = until - now; left > 0 && _fgFrames == burstFrame; left = until - Util::MillisecondsNow())
    {
        LARGE_INTEGER due { .QuadPart = -static_cast<LONGLONG>(std::min(left - 0.5, 1.0) * 10000) };
        if (left > 0.5 && timer.handle && SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(timer.handle, 100);
        else
            YieldProcessor();
    }
}

void MenuOverlayDx::SpaceFrameGeneration()
{
    _fgGameThread = GetCurrentThreadId();
    if (!Config::Instance()->FGXeFGExtraPacing.value_or(false))
    {
        _fgSpacingUntilMs = Util::MillisecondsNow() + 250;
        XeFGPacing::BypassFor(0.25);
    }
}

// for showing
static bool _showRenderImGuiDebugOnce = true;

static IID streamlineRiid {};
static bool CheckForRealObject(std::string functionName, IUnknown* pObject, IUnknown** ppRealObject)
{
    if (streamlineRiid.Data1 == 0)
    {
        auto iidResult = IIDFromString(L"{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}", &streamlineRiid);

        if (iidResult != S_OK)
            return false;
    }

    auto qResult = pObject->QueryInterface(streamlineRiid, (void**) ppRealObject);

    if (qResult == S_OK && *ppRealObject != nullptr)
    {
        LOG_INFO("{} Streamline proxy found!", functionName);
        (*ppRealObject)->Release();
        return true;
    }

    return false;
}

static int GetCorrectDXGIFormat(int eCurrentFormat)
{
    switch (eCurrentFormat)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    return eCurrentFormat;
}

static void CreateRenderTargetDx12(ID3D12Device* device, IDXGISwapChain* pSwapChain)
{
    LOG_FUNC();

    DXGI_SWAP_CHAIN_DESC sd;
    HRESULT hr = pSwapChain->GetDesc(&sd);

    if (hr != S_OK)
    {
        LOG_ERROR("pSwapChain->GetDesc: {0:X}", (unsigned long) hr);
        return;
    }

    if (sd.BufferCount > NUM_BACK_BUFFERS)
    {
        LOG_ERROR("Swapchain has {} buffers, menu supports {}", sd.BufferCount, NUM_BACK_BUFFERS);
        return;
    }

    for (UINT i = 0; i < sd.BufferCount; ++i)
    {
        ID3D12Resource* pBackBuffer = nullptr;
        auto result = pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));

        if (result != S_OK)
        {
            LOG_ERROR("pSwapChain->GetBuffer: {:X}", (unsigned long) result);
            return;
        }

        if (pBackBuffer != nullptr)
        {
            D3D12_RENDER_TARGET_VIEW_DESC desc = {};
            desc.Format = static_cast<DXGI_FORMAT>(GetCorrectDXGIFormat(sd.BufferDesc.Format));
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

            device->CreateRenderTargetView(pBackBuffer, &desc, g_mainRenderTargetDescriptor[i]);
            g_mainRenderTargetResource[i] = pBackBuffer;
        }
    }

    LOG_INFO("done!");
}

static void CleanupRenderTargetDx12(bool clearQueue)
{
    if (!_isInited || !_dx12Device || State::Instance().isShuttingDown)
        return;

    for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
    {
        SAFE_RELEASE(g_mainRenderTargetResource[i]);
    }

    LOG_TRACE("clearQueue: {}", clearQueue);

    if (clearQueue)
    {
        // With final-image NR the menu draws on XeFG's queue from XeFG's present thread: its last list may still run.
        if (AmdPresentExperimental::IsTarget() && currentSCCommandQueue != nullptr && g_pd3dDeviceParam != nullptr)
        {
            ID3D12Fence* fence = nullptr;
            HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (done != nullptr &&
                SUCCEEDED(g_pd3dDeviceParam->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
                SUCCEEDED(((ID3D12CommandQueue*) currentSCCommandQueue)->Signal(fence, 1)) &&
                SUCCEEDED(fence->SetEventOnCompletion(1, done)) && WaitForSingleObject(done, 2000) != WAIT_OBJECT_0)
            {
                LOG_WARN("menu queue did not go idle in 2 s");
            }
            SAFE_RELEASE(fence);
            if (done != nullptr)
                CloseHandle(done);
        }
        // Uses a descriptor from the ImGui heap, needs to go before it
        MenuCommon::SetBackgroundBlur(ImTextureID_Invalid);
        g_menuBlur.reset();

        if (MenuOverlayBase::IsInited() && g_pd3dDeviceParam != nullptr && g_pd3dSrvDescHeap != nullptr &&
            ImGui::GetIO().BackendRendererUserData)
        {
            // std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ImGui_ImplDX12_Shutdown(false);
        }

        SAFE_RELEASE(g_pd3dRtvDescHeap);
        SAFE_RELEASE(g_pd3dSrvDescHeap);

        for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
        {
            SAFE_RELEASE(g_commandAllocators[i]);
        }

        SAFE_RELEASE(g_pd3dCommandList);

        SAFE_RELEASE(g_menuFence);

        if (g_menuFenceEvent != nullptr)
        {
            CloseHandle(g_menuFenceEvent);
            g_menuFenceEvent = nullptr;
        }

        g_menuFenceValue = 0;
        memset(g_allocatorFenceValues, 0, sizeof(g_allocatorFenceValues));

        if (g_pd3dCommandQueue != nullptr)
            g_pd3dCommandQueue = nullptr;

        g_pd3dSrvDescHeapAlloc.Destroy();

        // SAFE_RELEASE(g_pd3dDeviceParam);

        _dx12Device = false;
        _isInited = false;
    }
}

static void CreateRenderTargetDx11(IDXGISwapChain* pSwapChain)
{
    if (_dx10Target)
    {
        g_pd3dDevice->CreateRenderTargetView(_dx10Target, nullptr, &g_pd3dRenderTarget);
        _targetView = _dx10Target;
        return;
    }

    ID3D11Texture2D* pBackBuffer = NULL;
    pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));

    if (pBackBuffer)
    {
        DXGI_SWAP_CHAIN_DESC sd;
        pSwapChain->GetDesc(&sd);

        D3D11_RENDER_TARGET_VIEW_DESC desc = {};
        desc.Format = static_cast<DXGI_FORMAT>(GetCorrectDXGIFormat(sd.BufferDesc.Format));
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, &desc, &g_pd3dRenderTarget);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTargetDx11(bool shutDown)
{
    if (!_isInited || !_dx11Device || State::Instance().isShuttingDown)
        return;

    if (!shutDown)
        LOG_FUNC();

    SAFE_RELEASE(g_pd3dRenderTarget);

    MenuCommon::SetBackgroundBlur(ImTextureID_Invalid);
    g_menuBlurDx11.reset();

    if (g_pd3dDevice != nullptr)
        g_pd3dDevice = nullptr;

    _dx11Device = false;
    _isInited = false;
}

static void RenderImGui_DX11(IDXGISwapChain* pSwapChain)
{
    bool drawMenu = false;

    do
    {
        if (!MenuOverlayBase::IsInited())
            break;

        // Draw only when menu activated
        // if (!MenuOverlayBase::IsVisible())
        //    break;

        if (!_dx11Device || g_pd3dDevice == nullptr)
            break;

        drawMenu = true;

    } while (false);

    if (!drawMenu)
    {
        MenuOverlayBase::HideMenu();
        return;
    }

    LOG_FUNC();

    ImGuiIO& io = ImGui::GetIO();
    (void) io;

    if (io.BackendRendererUserData == nullptr)
    {
        if (_dx10Target ? g_pd3dDevice != nullptr : pSwapChain->GetDevice(IID_PPV_ARGS(&g_pd3dDevice)) == S_OK)
        {
            if (!_dx10Target)
                g_pd3dDevice->Release();
            g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);
            g_pd3dDeviceContext->Release();
            ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
        }
    }

    if (_isInited)
    {
        // The D3D10 bridge makes its shared texture again on a resize.
        if (_dx10Target && _targetView != _dx10Target)
            SAFE_RELEASE(g_pd3dRenderTarget);
        if (!g_pd3dRenderTarget)
            CreateRenderTargetDx11(pSwapChain);

        if (ImGui::GetCurrentContext() && g_pd3dRenderTarget)
        {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();

            ID3D11Texture2D* backBuffer = nullptr;
            pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));

            // Blur textures are only created once the menu is opened, menu decides if they are used
            bool blurReady = false;
            if (backBuffer != nullptr && (g_menuBlurDx11 || MenuOverlayBase::IsVisible()))
            {
                if (!g_menuBlurDx11)
                    g_menuBlurDx11 = std::make_unique<MenuBlur_Dx11>("MenuBlur", g_pd3dDevice);

                blurReady = g_menuBlurDx11->Prepare(backBuffer);
            }

            if (blurReady)
                MenuCommon::SetBackgroundBlur(g_menuBlurDx11->TextureId(), g_menuBlurDx11->UVScale());
            else
                MenuCommon::SetBackgroundBlur(ImTextureID_Invalid);

            if (MenuOverlayBase::RenderMenu())
            {
                ImGui::Render();

                if (blurReady && MenuCommon::BackgroundBlurUsed())
                    g_menuBlurDx11->Dispatch(g_pd3dDeviceContext, backBuffer);

                g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pd3dRenderTarget, NULL);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }

            if (backBuffer != nullptr)
                backBuffer->Release();
        }
    }
}

static void RenderImGui_DX12(IDXGISwapChain* pSwapChainPlain)
{
    bool drawMenu = false;
    IDXGISwapChain3* pSwapChain = nullptr;

    do
    {
        if (pSwapChainPlain->QueryInterface(IID_PPV_ARGS(&pSwapChain)) != S_OK || pSwapChain == nullptr)
            return;

        if (!MenuOverlayBase::IsInited())
            break;

        // Draw only when menu activated
        // if (!MenuOverlayBase::IsVisible())
        //    break;

        if (!_dx12Device || currentSCCommandQueue == nullptr || g_pd3dDeviceParam == nullptr)
            break;

        drawMenu = true;

    } while (false);

    if (!drawMenu)
    {
        MenuOverlayBase::HideMenu();
        auto releaseResult = pSwapChain->Release();
        return;
    }

    LOG_FUNC();

    // Get device from swapchain
    ID3D12Device* device = g_pd3dDeviceParam;

    ImGuiIO& io = ImGui::GetIO();
    (void) io;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    // Generate ImGui resources
    if (!io.BackendRendererUserData && currentSCCommandQueue != nullptr)
    {
        LOG_DEBUG("ImGui::GetIO().BackendRendererUserData == nullptr");

        HRESULT result;

        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = NUM_BACK_BUFFERS;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            desc.NodeMask = 1;

            {
                ScopedSkipHeapCapture skipHeapCapture {};
                result = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dRtvDescHeap));
            }

            if (result != S_OK)
            {
                LOG_ERROR("CreateDescriptorHeap(g_pd3dRtvDescHeap): {0:X}", (unsigned long) result);
                MenuOverlayBase::HideMenu();
                CleanupRenderTargetDx12(true);
                pSwapChain->Release();
                return;
            }

            SIZE_T rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();

            for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
            {
                g_mainRenderTargetDescriptor[i] = rtvHandle;
                rtvHandle.ptr += rtvDescriptorSize;
            }
        }

        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = SRV_HEAP_SIZE;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

            {
                ScopedSkipHeapCapture skipHeapCapture {};
                result = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dSrvDescHeap));
            }

            if (result != S_OK)
            {
                LOG_ERROR("CreateDescriptorHeap(g_pd3dSrvDescHeap): {0:X}", (unsigned long) result);
                MenuOverlayBase::HideMenu();
                CleanupRenderTargetDx12(true);
                pSwapChain->Release();
                return;
            }

            g_pd3dSrvDescHeapAlloc.Create(device, g_pd3dSrvDescHeap);
        }

        for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
        {
            result =
                device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocators[i]));

            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocator[{0}]: {1:X}", i, (unsigned long) result);
                MenuOverlayBase::HideMenu();
                CleanupRenderTargetDx12(true);
                pSwapChain->Release();
                return;
            }
        }

        result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocators[0], NULL,
                                           IID_PPV_ARGS(&g_pd3dCommandList));
        if (result != S_OK)
        {
            LOG_ERROR("CreateCommandList: {0:X}", (unsigned long) result);
            MenuOverlayBase::HideMenu();
            CleanupRenderTargetDx12(true);
            pSwapChain->Release();
            return;
        }

        result = g_pd3dCommandList->Close();
        if (result != S_OK)
        {
            LOG_ERROR("g_pd3dCommandList->Close: {0:X}", (unsigned long) result);
            MenuOverlayBase::HideMenu();
            CleanupRenderTargetDx12(false);
            pSwapChain->Release();
            return;
        }

        result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_menuFence));
        g_menuFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (result != S_OK || g_menuFenceEvent == nullptr)
        {
            LOG_ERROR("CreateFence: {0:X}, event: {1:X}", (unsigned long) result, (size_t) g_menuFenceEvent);
            MenuOverlayBase::HideMenu();
            CleanupRenderTargetDx12(true);
            pSwapChain->Release();
            return;
        }

        g_menuFence->SetName(L"MenuFence");
        g_menuFenceValue = 0;
        memset(g_allocatorFenceValues, 0, sizeof(g_allocatorFenceValues));

        DXGI_SWAP_CHAIN_DESC scDesc;
        pSwapChain->GetDesc(&scDesc);

        ImGui_ImplDX12_InitInfo initInfo {};
        initInfo.Device = device;
        initInfo.CommandQueue = (ID3D12CommandQueue*) currentSCCommandQueue;
        initInfo.NumFramesInFlight = NUM_BACK_BUFFERS;
        initInfo.RTVFormat = scDesc.BufferDesc.Format;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.SrvDescriptorHeap = g_pd3dSrvDescHeap;
        initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle,
                                           D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle)
        { return g_pd3dSrvDescHeapAlloc.Alloc(out_cpu_handle, out_gpu_handle); };
        initInfo.SrvDescriptorFreeFn =
            [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle)
        { return g_pd3dSrvDescHeapAlloc.Free(cpu_handle, gpu_handle); };

        ImGui_ImplDX12_Init(&initInfo);

        pSwapChain->Release();
        return;
    }

    if (_isInited && currentSCCommandQueue != nullptr)
    {
        // Generate render targets
        if (!g_mainRenderTargetResource[0])
        {
            CreateRenderTargetDx12(device, pSwapChain);
            pSwapChain->Release();
            return;
        }

        // If everything is ready render the frame
        if (ImGui::GetCurrentContext() && g_mainRenderTargetResource[0])
        {
            _showRenderImGuiDebugOnce = true;

            UINT backBufferIdx = pSwapChain->GetCurrentBackBufferIndex();

            if (backBufferIdx >= NUM_BACK_BUFFERS || !g_mainRenderTargetResource[backBufferIdx])
            {
                pSwapChain->Release();
                return;
            }

            ImGui_ImplDX12_NewFrame();

            // Blur textures are only created once the menu is opened, menu decides if they are used
            bool blurReady = false;
            if (g_menuBlur || MenuOverlayBase::IsVisible())
            {
                if (!g_menuBlur)
                    g_menuBlur = std::make_unique<MenuBlur_Dx12>("MenuBlur", device, &g_pd3dSrvDescHeapAlloc);

                blurReady = g_menuBlur->Prepare(g_mainRenderTargetResource[backBufferIdx]);
            }

            if (blurReady)
                MenuCommon::SetBackgroundBlur(g_menuBlur->TextureId(), g_menuBlur->UVScale());
            else
                MenuCommon::SetBackgroundBlur(ImTextureID_Invalid);

            if (MenuOverlayBase::RenderMenu())
            {
                ImGui::Render();

                const bool blurMenu = blurReady && MenuCommon::BackgroundBlurUsed();
                ID3D12CommandAllocator* commandAllocator = g_commandAllocators[backBufferIdx];

                // Previous command list recorded with this allocator might still be executing
                const UINT64 waitValue = g_allocatorFenceValues[backBufferIdx];
                if (g_menuFence != nullptr && g_menuFence->GetCompletedValue() < waitValue)
                {
                    DWORD waitResult = WAIT_FAILED;

                    if (g_menuFence->SetEventOnCompletion(waitValue, g_menuFenceEvent) == S_OK)
                        waitResult = WaitForSingleObject(g_menuFenceEvent, 2000);

                    if (waitResult != WAIT_OBJECT_0)
                    {
                        LOG_ERROR("Menu fence wait failed: {:X}", waitResult);
                        pSwapChain->Release();
                        return;
                    }
                }

                auto result = commandAllocator->Reset();
                if (result != S_OK)
                {
                    LOG_ERROR("commandAllocator->Reset: {0:X}", (unsigned long) result);
                    CleanupRenderTargetDx12(false);
                    pSwapChain->Release();
                    return;
                }

                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = g_mainRenderTargetResource[backBufferIdx];
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

                result = g_pd3dCommandList->Reset(commandAllocator, nullptr);
                if (result != S_OK)
                {
                    LOG_ERROR("g_pd3dCommandList->Reset: {0:X}", (unsigned long) result);
                    pSwapChain->Release();
                    return;
                }

                if (blurMenu)
                {
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    g_pd3dCommandList->ResourceBarrier(1, &barrier);

                    g_menuBlur->Dispatch(g_pd3dCommandList, g_mainRenderTargetResource[backBufferIdx], backBufferIdx);

                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                }

                g_pd3dCommandList->ResourceBarrier(1, &barrier);
                g_pd3dCommandList->OMSetRenderTargets(1, &g_mainRenderTargetDescriptor[backBufferIdx], FALSE, NULL);
                g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);

                ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);

                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                g_pd3dCommandList->ResourceBarrier(1, &barrier);

                result = g_pd3dCommandList->Close();
                if (result != S_OK)
                {
                    LOG_ERROR("g_pd3dCommandList->Close: {0:X}", (unsigned long) result);
                    CleanupRenderTargetDx12(true);
                    pSwapChain->Release();
                    return;
                }

                ID3D12CommandList* ppCommandLists[] = { g_pd3dCommandList };
                ((ID3D12CommandQueue*) currentSCCommandQueue)->ExecuteCommandLists(1, ppCommandLists);

                if (g_menuFence != nullptr)
                {
                    result = ((ID3D12CommandQueue*) currentSCCommandQueue)->Signal(g_menuFence, ++g_menuFenceValue);

                    if (result == S_OK)
                        g_allocatorFenceValues[backBufferIdx] = g_menuFenceValue;
                    else
                        LOG_ERROR("Menu fence Signal: {:X}", (unsigned long) result);
                }
            }
        }
        else
        {
            if (_showRenderImGuiDebugOnce)
                LOG_INFO("!(ImGui::GetCurrentContext() && currentSCCommandQueue && g_mainRenderTargetResource[0])");

            MenuOverlayBase::HideMenu();
            _showRenderImGuiDebugOnce = false;
        }
    }

    pSwapChain->Release();
}

ID3D12GraphicsCommandList* MenuOverlayDx::MenuCommandList() { return g_pd3dCommandList; }

void MenuOverlayDx::CleanupRenderTarget(bool clearQueue, HWND hWnd)
{
    LOG_FUNC();
    if (clearQueue)
        AmdPresentExperimental::BeforeResize();

    auto fg = State::Instance().currentFG;
    if (fg != nullptr && fg->FrameGenerationContext() != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    std::unique_lock lock(_overlayMutex, std::defer_lock);
    if (AmdPresentExperimental::IsTarget())
        lock.lock();
    if (_dx11Device)
        CleanupRenderTargetDx11(false);
    else
        CleanupRenderTargetDx12(clearQueue);
}

void MenuOverlayDx::HoldForResize(bool hold) { _heldForResize = hold; }

void MenuOverlayDx::Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                            const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP)
{
    // D3D10: the frame crosses to OptiScaler's own D3D11 device, NR and the menu run there, and it comes back. Only
    // with NR without upscaling on; otherwise OptiScaler leaves a D3D10 game's frames alone, as before.
    Microsoft::WRL::ComPtr<ID3D10Device> device10;
    if (!_dx10Target && pDevice && SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&device10))))
    {
        if (!AmdPresentExperimental::IsTarget() || (Flags & DXGI_PRESENT_TEST) || (hWnd && IsIconic(hWnd)))
            return;
        ID3D11Device* device11 = nullptr;
        ID3D11Texture2D* frame = AmdPresentExperimental::Begin10(pSwapChain, device10.Get(), &device11);
        if (!frame)
            return;
        if (Config::Instance()->DlssNrEnabled.value_or_default() && !State::Instance().currentFeature &&
            DlssNr::Backend::PresentRuntimeSupported())
        {
            auto settings = DlssNr::AmdBridge::SettingsFromConfig(*Config::Instance(),
                                                                  Config::Instance()->AmdNrScale.value_or_default());
            settings.spinDraw = 0;
            AmdPresentExperimental::RenderTexture11(frame, device11, Util::DllPath().parent_path(), settings);
        }
        _dx10Target = frame;
        Present(pSwapChain, SyncInterval, Flags, pPresentParameters, device11, hWnd, isUWP);
        _dx10Target = nullptr;
        AmdPresentExperimental::End10();
        return;
    }

    // Through the D3D11 bridge, Dx11wDx12SC runs NR before XeFG; the swapchain XeFG presents to is not a second
    // target. On native D3D12 with a frame generation swapchain this is the swapchain frame generation presents
    // its output to; FinalImageFrameGen has already run NR on the game's frame. D3D9 runs NR in its own bridge.
    const bool fgPresents = State::Instance().swapchainInteropApi == SwapchainInteropApi::None &&
                            State::Instance().currentFGSwapchain != nullptr;
    if (fgPresents && !(Flags & DXGI_PRESENT_TEST) && AmdPresentExperimental::IsTarget())
    {
        const double now = Util::MillisecondsNow();
        const double last = _fgLastOutputMs.exchange(now);
        if (last > 0 && now - last > _fgLongestGapMs)
            _fgLongestGapMs = now - last;
        _fgOutputPresents++;
    }
    // mochizuki's Present placement: on the finished picture of a game whose upscaler runs too.
    const bool mochizukiPresent = DlssNr::Backend::MochizukiPlacementNow() == DlssNr::Backend::MochizukiPlace::Present;
    if (!_dx10Target && (mochizukiPresent || (AmdPresentExperimental::IsTarget() && !State::Instance().currentFeature)) &&
        Config::Instance()->DlssNrEnabled.value_or_default() && !fgPresents &&
        State::Instance().swapchainInteropApi != SwapchainInteropApi::Dx9wDx12 &&
        !(State::Instance().activeFgOutput == FGOutput::XeFG && State::Instance().currentFGSwapchain == pSwapChain &&
          !FGHooks::IsDx12InteropPresentSC(pSwapChain)) &&
        !(State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
          !FGHooks::IsDx12InteropPresentSC(pSwapChain)) &&
        (!State::Instance().currentFG || !State::Instance().currentFG->IsActive() ||
         State::Instance().currentFG->IsPaused()) &&
        DlssNr::Backend::PresentRuntimeSupported() && !(Flags & DXGI_PRESENT_TEST) && pDevice &&
        (!hWnd || !IsIconic(hWnd)))
    {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&queue))))
        {
            auto settings = DlssNr::AmdBridge::SettingsFromConfig(
                *Config::Instance(), Config::Instance()->AmdNrScale.value_or_default());
            settings.spinDraw = 0;
            AmdPresentExperimental::Render(pSwapChain, queue.Get(), Util::DllPath().parent_path(), settings);
        }
        else
        {
            Microsoft::WRL::ComPtr<ID3D11Device> device11;
            if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&device11))))
            {
                auto settings = DlssNr::AmdBridge::SettingsFromConfig(
                    *Config::Instance(), Config::Instance()->AmdNrScale.value_or_default());
                settings.spinDraw = 0;
                AmdPresentExperimental::Render11(pSwapChain, device11.Get(), Util::DllPath().parent_path(), settings);
            }
        }
    }

    // The frame as it goes to the screen, NR included, before OptiScaler's overlay is drawn on it. With frame
    // generation this is the swapchain frame generation presents to, so a generated frame can be the one saved.
    if (!(Flags & DXGI_PRESENT_TEST) && pDevice)
        Screenshot::Dxgi(pSwapChain, pDevice, _dx10Target);

    if (!Config::Instance()->OverlayMenu.value_or_default())
    {
        MenuOverlayBase::Present();
        return;
    }

    // MenuOverlayVk holds io.BackendRendererUserData, so ImGui_ImplDX11/DX12 would run against its
    // data. MenuOverlayVk stands down for vkd3d-proton D3D12 swapchains; this covers the reverse
    // order, where it claimed the backend first.
    if (State::Instance().menuOverlayIsVulkan)
        return;

    std::unique_lock lock(_overlayMutex, std::defer_lock);
    if (AmdPresentExperimental::IsTarget())
    {
        lock.lock();
        if (_heldForResize)
            return;
    }

    LOG_DEBUG("");

    ID3D12CommandQueue* cq = nullptr;
    ID3D11Device* device = nullptr;
    ID3D12Device* device12 = nullptr;

    // try to obtain directx objects and find the path
    if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
    {
        if (!_dx11Device)
            LOG_DEBUG("D3D11Device captured");

        _dx11Device = true;
    }
    else if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (!_dx12Device)
            LOG_DEBUG("D3D12CommandQueue captured");

        if (!CheckForRealObject(__FUNCTION__, pDevice, &currentSCCommandQueue))
            currentSCCommandQueue = pDevice;

        if (((ID3D12CommandQueue*) currentSCCommandQueue)->GetDevice(IID_PPV_ARGS(&device12)) == S_OK)
        {
            if (!_dx12Device)
                LOG_DEBUG("D3D12Device captured");

            _dx12Device = true;
        }
    }

    // Process window handle changed, update base
    if (MenuOverlayBase::Handle() != hWnd)
    {
        LOG_DEBUG("Handle changed {:X} -> {:X}", (size_t) MenuOverlayBase::Handle(), (size_t) hWnd);

        if (MenuOverlayBase::IsInited())
            MenuOverlayBase::Shutdown();

        MenuOverlayBase::Init(hWnd, isUWP);

        _isInited = false;
    }

    // Init
    if (!_isInited)
    {
        if (_dx11Device)
        {
            CleanupRenderTargetDx11(false);

            g_pd3dDevice = device;

            CreateRenderTargetDx11(pSwapChain);
            MenuOverlayBase::Dx11Ready();
            _isInited = true;
        }
        else if (_dx12Device && (g_pd3dDeviceParam != nullptr || device12 != nullptr))
        {
            if (g_pd3dDeviceParam != nullptr && device12 == nullptr)
                device12 = g_pd3dDeviceParam;

            CleanupRenderTargetDx12(true);

            g_pd3dCommandQueue = cq;
            g_pd3dDeviceParam = device12;

            MenuOverlayBase::Dx12Ready();
            _isInited = true;
        }
    }

    {
        ScopedSkipHeapCapture skipHeapCapture {};

        // Render menu
        if (_dx11Device)
            RenderImGui_DX11(pSwapChain);
        else if (_dx12Device)
            RenderImGui_DX12(pSwapChain);
    }

    // The same frame again with the menu drawn on it, when the screenshot asked for that copy: the D3D11 draw and
    // the D3D12 list are in, the real present not yet.
    if (!(Flags & DXGI_PRESENT_TEST))
        Screenshot::DxgiMenu(pSwapChain, pDevice, _dx10Target);

    // release used objects
    if (cq != nullptr)
        cq->Release();

    if (device != nullptr)
        device->Release();

    if (device12 != nullptr)
        device12->Release();
}

void MenuOverlayDx::FinalImageFrameGen(IDXGISwapChain* fgSwapChain)
{
    auto& state = State::Instance();
    const auto& config = *Config::Instance();
    auto fg = state.currentFG;

    // mochizuki's Present placement with frame generation: NR on the game's frame, before frame generation reads it.
    // Frame generation keeps its own inputs from the upscaler.
    if (DlssNr::Backend::MochizukiPlacementNow() == DlssNr::Backend::MochizukiPlace::Present &&
        (state.currentFeature || !AmdPresentExperimental::IsTarget()))
    {
        DXGI_SWAP_CHAIN_DESC placed {};
        fgSwapChain->GetDesc(&placed);
        if (config.DlssNrEnabled.value_or_default() && fg != nullptr && fg->GetCommandQueue() != nullptr &&
            !(placed.OutputWindow && IsIconic(placed.OutputWindow)))
        {
            auto settings = DlssNr::AmdBridge::SettingsFromConfig(config, config.AmdNrScale.value_or_default());
            settings.spinDraw = 0;
            AmdPresentExperimental::Render(fgSwapChain, fg->GetCommandQueue(), Util::DllPath().parent_path(), settings);
        }
        return;
    }

    // The game's queue: XeFG runs its interpolation there too, so NR, the guides and XeFG are ordered
    // by the queue itself.
    if (!AmdPresentExperimental::IsTarget() || state.swapchainInteropApi != SwapchainInteropApi::None ||
        state.currentFeature || fg == nullptr || fg->GetCommandQueue() == nullptr ||
        !DlssNr::Backend::PresentRuntimeSupported())
        return;

    auto queue = fg->GetCommandQueue();
    DXGI_SWAP_CHAIN_DESC desc {};
    fgSwapChain->GetDesc(&desc);
    // No NR while the window is minimized, as without frame generation; XeFG keeps its guides.
    const bool neural = config.DlssNrEnabled.value_or_default() && !(desc.OutputWindow && IsIconic(desc.OutputWindow));
    const bool fgEnabled = config.FGEnabled.value_or_default();
    // XeFG, or the DLSSG output (OptiScaler's own Streamline, which NeuralHub's DLSS-FG serves on AMD).
    const bool dlssg = state.activeFgOutput == FGOutput::DLSSG;
    const bool xefg = (state.activeFgOutput == FGOutput::XeFG || dlssg) && state.activeFgInput == FGInput::Upscaler;
    AmdPresentExperimental::Guides guides;
    bool ready = false;

    // With NR off XeFG still gets the optical-flow guides. They are made only while XeFG takes them.
    const bool wantGuides = xefg && fgEnabled;
    if (neural || wantGuides)
    {
        auto settings = DlssNr::AmdBridge::SettingsFromConfig(config, config.AmdNrScale.value_or_default());
        settings.spinDraw = 0;
        ready = AmdPresentExperimental::Render(fgSwapChain, queue, Util::DllPath().parent_path(), settings,
                                               wantGuides ? &guides : nullptr, neural);
    }

    if (!xefg)
        return;

    static bool wasReady = true;
    if (fgEnabled && wasReady && !ready)
        LOG_WARN("XeFG final-image guides unavailable: {}", AmdPresentExperimental::Status());
    wasReady = ready || !fgEnabled;

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        return;

    // Frames without guides are not frame generation frames.
    if (fgEnabled && ready)
        fg->StartNewFrame();

    if (!ready && fg->IsActive())
        fg->Deactivate();

    FG_Constants constants {};
    constants.flags |= FG_Flags::DisplayResolutionMVs;
    constants.displayWidth = ready ? guides.width : desc.BufferDesc.Width;
    constants.displayHeight = ready ? guides.height : desc.BufferDesc.Height;

    if (ready || fg->FrameGenerationContext() == nullptr || !fgEnabled)
        fg->EvaluateState(device.Get(), constants);

    // Frame generation on and its guides made, yet XeFG not generating: what holds it, every 30 s.
    static ULONGLONG idleSince = 0, idleReported = 0;
    if (fgEnabled && ready && (!fg->IsActive() || fg->IsPaused()))
    {
        const auto now = GetTickCount64();
        if (idleSince == 0)
            idleSince = now;
        if (now - idleSince > 5000 && now - idleReported > 30000)
        {
            idleReported = now;
            LOG_WARN("Final-image XeFG: frame generation is on but XeFG has not generated for {} s (active {}, paused "
                     "until frame {} at frame {}, context {}, swapchain {})",
                     (now - idleSince) / 1000, fg->IsActive(), fg->TargetFrame(), fg->FrameCount(),
                     fg->FrameGenerationContext() != nullptr, state.currentFGSwapchain != nullptr);
        }
    }
    else
        idleSince = 0;

    if (!ready || !fg->IsActive() || fg->IsPaused())
        return;

    // XeFG generates: OptiScaler spaces its presents and the pacing hooks forward, until a quarter second after the
    // last such frame. Streamline paces DLSSG's presents itself.
    if (!dlssg)
        SpaceFrameGeneration();

    // Optical flow has no camera: an identity view, a nominal projection and pixel motion at output size.
    fg->SetCameraValues(0.1f, 1000.0f, 1.5707963f, float(guides.width) / float(guides.height));
    fg->SetMVScale(1.0f, 1.0f);
    fg->SetJitter(0.0f, 0.0f);
    fg->SetReset(guides.reset);
    fg->SetInterpolationRect(guides.width, guides.height);

    Dx12Resource resource {};
    resource.width = guides.width;
    resource.height = guides.height;
    resource.state = D3D12_RESOURCE_STATE_COMMON;
    resource.validity = FG_ResourceValidity::UntilPresent;
    resource.type = FG_ResourceType::Velocity;
    resource.resource = guides.motion.Get();
    const bool motionTagged = fg->SetResource(&resource);
    resource.type = FG_ResourceType::Depth;
    resource.resource = guides.depth.Get();
    const bool depthTagged = fg->SetResource(&resource);

    static bool reported = false;
    if (!motionTagged || !depthTagged)
    {
        LOG_WARN("Final-image NR: XeFG rejected motion or depth guides");
        fg->Deactivate();
    }
    else if (!reported)
    {
        reported = true;
        LOG_INFO("Final-image NR: XeFG takes optical-flow motion and depth at {}x{} on the game's queue", guides.width,
                 guides.height);
    }

    // Every 5 s while XeFG runs: the game's frames, how many NR changed, how many reset XeFG's history and the presents
    // that reached DXGI. One present per frame means XeFG showed the game's frames only.
    static ULONGLONG since = 0;
    static UINT frames = 0, changed = 0, resets = 0;
    static UINT64 presentsBefore = 0;
    const auto now = GetTickCount64();
    if (since == 0 || now - since > 6000)
    {
        since = now;
        frames = changed = resets = 0;
        presentsBefore = _fgOutputPresents;
        _fgLongestGapMs = 0;
        return;
    }
    frames++;
    changed += neural && AmdPresentExperimental::LastFrameModified();
    resets += guides.reset;
    if (now - since < 5000)
        return;
    const double seconds = (now - since) / 1000.0;
    const double perFrame = double(_fgOutputPresents - presentsBefore) / frames;
    LOG_INFO(
        "Final-image XeFG: {:.1f} frames/s, {:.1f} presents/s ({:.2f} per frame{}, longest gap {:.1f} ms), NR on {} "
        "of {} frames, {} history resets",
        frames / seconds, perFrame * frames / seconds, perFrame,
        perFrame < 1.2 ? ": XeFG presented no generated frames" : "", _fgLongestGapMs.load(), changed, frames, resets);
    since = now;
    frames = changed = resets = 0;
    presentsBefore = _fgOutputPresents;
    _fgLongestGapMs = 0;
}

void MenuOverlayDx::ApplyThemeStyle() { MenuOverlayBase::ApplyThemeStyle(); }
