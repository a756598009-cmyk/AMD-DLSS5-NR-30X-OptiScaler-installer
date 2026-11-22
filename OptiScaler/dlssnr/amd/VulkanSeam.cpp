#include "pch.h"
#include "VulkanSeam.h"

#include "AmdBridge.h"
#include "AmdPreSr.h"
#include "../DlssNr.h"
#include "../backend/Selector.h"
#include "../submission/SubmissionHooks.h"

#include <Config.h>
#include <State.h>
#include <hooks/VulkanwDx12_Hooks.h>
#include <misc/DeferredRelease.h>
#include <misc/SkipSpoof.h>
#include <proxies/D3D12_Proxy.h>
#include <shaders/depth_transfer/DT_Vk.h>
#include <shaders/resource_copy/RC_Vk.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <vulkan/vulkan_win32.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <utility>

namespace DlssNr::VkSeam
{
using Microsoft::WRL::ComPtr;

namespace
{
// Three, so a game that records a frame before it submits the last one never waits here: a slot comes back once the
// D3D12 work of the frame before last finished, which needs that frame submitted, not the last one.
constexpr uint32_t kSlots = 3;

// An image both APIs see: a D3D12 committed resource made shareable, imported into the game's Vulkan device.
struct SharedImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    ComPtr<ID3D12Resource> d12;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    NVSDK_NGX_Resource_VK ngx {};

    bool Matches(uint32_t w, uint32_t h, VkFormat f) const
    {
        return image != VK_NULL_HANDLE && width == w && height == h && format == f;
    }
};

struct Slot
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    // A shared D3D12 fence, imported into Vulkan as linkSemaphore: the game's command buffer signals it once the
    // copies are in, the D3D12 queue once NR's answer is out.
    ComPtr<ID3D12Fence> link;
    VkSemaphore linkSemaphore = VK_NULL_HANDLE;
    // Orders the continuation after the copy-back (Vulkan_wDx12's split waits for it).
    VkSemaphore backSemaphore = VK_NULL_HANDLE;
    VkCommandBuffer back = VK_NULL_HANDLE, next = VK_NULL_HANDLE;
    // The frame waiting for its game submit (Settled): the link values it waits for and signals, and its number.
    uint64_t copied = 0, finished = 0, frameNo = 0;
    std::atomic<bool> queued { false };
};

struct Seam
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    bool ready = false;
    // Off for the session: something that will not come back by itself (an extension missing, a creation failed).
    bool stopped = false;
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue;
    // Signalled with the frame number once that frame's D3D12 work finished.
    ComPtr<ID3D12Fence> done;
    HANDLE doneEvent = nullptr;
    uint64_t frame = 0;
    // The last value handed to a link fence or a copy-back semaphore; each takes values from this one counter.
    uint64_t value = 0;
    std::array<Slot, kSlots> slots;
    uint32_t family = UINT32_MAX;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::unique_ptr<ResourceCopy_Vk> colourCopy, motionCopy, exposureCopy;
    std::unique_ptr<DepthTransfer_Vk> depthCopy;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    SharedImage colour, motion, depth, exposure, answer;
    // The last copy-back took the answer back to Vulkan; the next frame hands it to D3D12 first.
    bool answerOwned = false;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descriptorSize = 0;
    PFN_vkGetMemoryWin32HandlePropertiesKHR getHandleProperties = nullptr;
    PFN_vkImportSemaphoreWin32HandleKHR importSemaphore = nullptr;
    // When a wait for an earlier frame ran out; frames pass through without waiting for a second after it.
    ULONGLONG waitedOutAt = 0;
    // Vulkan_wDx12::abortedSubmissions as of the last frame this seam split.
    uint32_t aborts = 0;
    // Whether the last split frame carried the runtime's answer, the frames since that changed, and how often it did.
    bool answering = false;
    uint64_t sinceChange = 0, changes = 0;
};

std::mutex mutex;
// For the process, like the runtime it feeds: at exit the driver may already be gone, so nothing is destroyed.
Seam& seam = *new Seam;
std::atomic<unsigned long long> frames { 0 };
std::mutex statusMutex;
std::string status;

// The parameter block whose colour is ours until After, and the game's colour it held.
thread_local NVSDK_NGX_Parameter* replaced = nullptr;
thread_local void* gameColour = nullptr;

void Report(const std::string& why, bool error = false)
{
    static std::set<std::string> said;
    {
        std::lock_guard lock(statusMutex);
        if (status == why)
            return;
        status = why;
    }
    if (said.insert(why).second)
    {
        if (error)
            LOG_ERROR("DLSS-NR Vulkan seam: {}", why);
        else
            LOG_INFO("DLSS-NR Vulkan seam: {}", why);
    }
    AmdBridge::Note("AMD neural on Vulkan: " + why);
}

void Stop(const std::string& why)
{
    seam.stopped = true;
    Report(why + "; NR is off until the game restarts", true);
}

// On the thread that submitted the game's command buffer, right after that submit (or after its transaction was
// abandoned, its D3D12 wait released on the CPU): the frame's D3D12 work goes to the queue. Only the slot's own state is
// touched, and Before does not reuse the slot until this frame's D3D12 work has finished, so the seam's mutex, which
// Before may hold while it waits for an earlier frame, is not taken.
void Settled(void* context, bool aborted)
{
    static std::mutex submitMutex;
    Slot& slot = seam.slots[reinterpret_cast<uintptr_t>(context) % kSlots];
    if (!slot.queued.exchange(false))
        return;
    std::lock_guard lock(submitMutex);
    if (FAILED(seam.queue->Wait(slot.link.Get(), slot.copied)))
        LOG_ERROR("DLSS-NR Vulkan seam: the D3D12 queue could not wait for Vulkan");
    ID3D12CommandList* lists[] { slot.list.Get() };
    seam.queue->ExecuteCommandLists(1, lists);
    if (FAILED(seam.queue->Signal(slot.link.Get(), slot.finished)))
    {
        // The copy-back waits for this value on the GPU; without it the game's queue would hang.
        LOG_ERROR("DLSS-NR Vulkan seam: the D3D12 queue could not signal Vulkan; signalled on the CPU instead");
        if (slot.link->GetCompletedValue() < slot.finished)
            slot.link->Signal(slot.finished);
    }
    seam.queue->Signal(seam.done.Get(), slot.frameNo);
    if (aborted)
    {
        static bool said = false;
        if (!std::exchange(said, true))
            LOG_WARN("DLSS-NR Vulkan seam: a split frame was abandoned before its submit; its NR ran unordered");
    }
}

// The formats a compute copy writes and D3D12 reads with the same bits in the same channels.
DXGI_FORMAT SharedFormat(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R16G16_SFLOAT:
        return DXGI_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT:
        return DXGI_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R16G16_SNORM:
        return DXGI_FORMAT_R16G16_SNORM;
    case VK_FORMAT_R16G16_UNORM:
        return DXGI_FORMAT_R16G16_UNORM;
    case VK_FORMAT_R32_SFLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case VK_FORMAT_R16_SFLOAT:
        return DXGI_FORMAT_R16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool Valid(const NVSDK_NGX_Resource_VK* r)
{
    return r != nullptr && r->Resource.ImageViewInfo.Image != VK_NULL_HANDLE &&
           r->Resource.ImageViewInfo.ImageView != VK_NULL_HANDLE &&
           r->Resource.ImageViewInfo.Format != VK_FORMAT_UNDEFINED && r->Resource.ImageViewInfo.Width != 0 &&
           r->Resource.ImageViewInfo.Height != 0;
}

NVSDK_NGX_Resource_VK* Param(NVSDK_NGX_Parameter* params, const char* name)
{
    NVSDK_NGX_Resource_VK* r = nullptr;
    if (params->Get(name, (void**) &r) != NVSDK_NGX_Result_Success)
        return nullptr;
    return Valid(r) ? r : nullptr;
}

// The physical adapter behind the Vulkan device, past the vendor spoof.
bool IsAmdAdapter(const LUID& luid)
{
    const auto skip = SkipSpoof::AddEntry(SkipSpoofType::Thread);
    bool amd = false;
    IDXGIFactory4* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        IDXGIAdapter1* adapter = nullptr;
        if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
        {
            DXGI_ADAPTER_DESC1 desc {};
            amd = SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x1002;
            adapter->Release();
        }
        factory->Release();
    }
    SkipSpoof::RemoveEntry(skip);
    return amd;
}

bool AdapterLuid(VkPhysicalDevice physical, LUID& luid)
{
    VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    properties.pNext = &id;
    vkGetPhysicalDeviceProperties2(physical, &properties);
    if (!id.deviceLUIDValid)
        return false;
    std::memcpy(&luid, id.deviceLUID, sizeof(luid));
    return true;
}

void Barrier(D3D12_RESOURCE_BARRIER* out, UINT& n, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (r == nullptr || from == to)
        return;
    auto& b = out[n++];
    b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
}

VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout from, VkImageLayout to, uint32_t fromFamily,
                                  uint32_t toFamily, VkAccessFlags fromAccess, VkAccessFlags toAccess)
{
    VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = fromFamily;
    b.dstQueueFamilyIndex = toFamily;
    b.srcAccessMask = fromAccess;
    b.dstAccessMask = toAccess;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return b;
}

// Earlier frames may still use them on either API.
void Retire(SharedImage& s)
{
    if (s.image == VK_NULL_HANDLE && !s.d12)
        return;
    DeferredRelease::Enqueue(
        [device = seam.device, image = s.image, view = s.view, memory = s.memory, d12 = s.d12]() mutable
        {
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(device, view, nullptr);
            if (image != VK_NULL_HANDLE)
                vkDestroyImage(device, image, nullptr);
            if (memory != VK_NULL_HANDLE)
                vkFreeMemory(device, memory, nullptr);
            d12.Reset();
        },
        (uint64_t) seam.device);
    s = {};
}

bool CreateShared(SharedImage& s, uint32_t width, uint32_t height, VkFormat vkFormat, DXGI_FORMAT dxgiFormat,
                  bool d3d12Writes)
{
    Retire(s);

    D3D12_HEAP_PROPERTIES heap { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = dxgiFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                 (d3d12Writes ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> resource;
    if (FAILED(seam.d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, IID_PPV_ARGS(&resource))))
        return false;
    HANDLE handle = nullptr;
    if (FAILED(seam.d12->CreateSharedHandle(resource.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
        return false;

    VkExternalMemoryImageCreateInfo external { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.pNext = &external;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = vkFormat;
    info.extent = { width, height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    bool created = false;
    do
    {
        if (vkCreateImage(seam.device, &info, nullptr, &s.image) != VK_SUCCESS)
            break;
        VkMemoryWin32HandlePropertiesKHR handleProperties { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
        if (seam.getHandleProperties(seam.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle,
                                     &handleProperties) != VK_SUCCESS)
            break;
        VkMemoryRequirements requirements {};
        vkGetImageMemoryRequirements(seam.device, s.image, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties {};
        vkGetPhysicalDeviceMemoryProperties(seam.physical, &memoryProperties);
        const uint32_t allowed = requirements.memoryTypeBits & handleProperties.memoryTypeBits;
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((allowed & (1u << i)) &&
                (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = i;
        if (type == UINT32_MAX)
            break;
        VkMemoryDedicatedAllocateInfo dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.image = s.image;
        VkImportMemoryWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
        import.pNext = &dedicated;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        import.handle = handle;
        VkMemoryAllocateInfo allocation { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocation.pNext = &import;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (vkAllocateMemory(seam.device, &allocation, nullptr, &s.memory) != VK_SUCCESS ||
            vkBindImageMemory(seam.device, s.image, s.memory, 0) != VK_SUCCESS)
            break;
        VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = s.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = vkFormat;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(seam.device, &view, nullptr, &s.view) != VK_SUCCESS)
            break;
        created = true;
    } while (false);
    CloseHandle(handle);

    if (!created)
    {
        if (s.view != VK_NULL_HANDLE)
            vkDestroyImageView(seam.device, s.view, nullptr);
        if (s.image != VK_NULL_HANDLE)
            vkDestroyImage(seam.device, s.image, nullptr);
        if (s.memory != VK_NULL_HANDLE)
            vkFreeMemory(seam.device, s.memory, nullptr);
        s = {};
        return false;
    }

    s.d12 = resource;
    s.format = vkFormat;
    s.width = width;
    s.height = height;
    s.ngx.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    s.ngx.Resource.ImageViewInfo.ImageView = s.view;
    s.ngx.Resource.ImageViewInfo.Image = s.image;
    s.ngx.Resource.ImageViewInfo.SubresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    s.ngx.Resource.ImageViewInfo.Format = vkFormat;
    s.ngx.Resource.ImageViewInfo.Width = width;
    s.ngx.Resource.ImageViewInfo.Height = height;
    s.ngx.ReadWrite = false;
    return true;
}

bool CreateTimeline(VkSemaphore& semaphore)
{
    VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    info.pNext = &type;
    return vkCreateSemaphore(seam.device, &info, nullptr, &semaphore) == VK_SUCCESS;
}

// The answer of the runtime, in whatever format it hands it, into the shared colour the upscaler reads.
bool CreateCopyPipeline()
{
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 };
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = { 2, ranges };
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = { 0, 0, 2 };
    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = parameters;
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(seam.d12->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&seam.root))))
        return false;
    const char* shader = "Texture2D<float4> src:register(t0); RWTexture2D<float4> dst:register(u0);"
                         "cbuffer C:register(b0){uint w,h;} [numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID)"
                         "{if(p.x<w&&p.y<h)dst[p.xy]=src.Load(int3(p.xy,0));}";
    if (FAILED(D3DCompile(shader, std::strlen(shader), "Vulkan seam answer copy", nullptr, nullptr, "main", "cs_5_0", 0,
                          0, &blob, &errors)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline {};
    pipeline.pRootSignature = seam.root.Get();
    pipeline.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
    if (FAILED(seam.d12->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&seam.pipeline))))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 2 * kSlots;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(seam.d12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&seam.heap))))
        return false;
    seam.descriptorSize = seam.d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

// A new VkDevice (the game made another one): what belonged to the old one may still be referenced by work that will
// never finish, so it is left alone rather than destroyed. Only the D3D12 device, one per adapter anyway, is kept.
void Forget()
{
    auto d12 = seam.d12;
    for (auto& slot : seam.slots)
    {
        slot.allocator.Detach();
        slot.list.Detach();
        slot.link.Detach();
    }
    seam.queue.Detach();
    seam.done.Detach();
    for (auto* s : { &seam.colour, &seam.motion, &seam.depth, &seam.exposure, &seam.answer })
        s->d12.Detach();
    seam.colourCopy.release();
    seam.motionCopy.release();
    seam.exposureCopy.release();
    seam.depthCopy.release();
    seam.root.Detach();
    seam.pipeline.Detach();
    seam.heap.Detach();
    seam.d12.Detach();
    // Nothing it owns is left to release, so this only clears it.
    seam.~Seam();
    new (&seam) Seam();
    seam.d12 = d12;
}

// Everything that belongs to the device rather than to a frame. Answers why it could not be made, or null.
const char* Init(VkPhysicalDevice physical, VkDevice device)
{
    if (seam.ready && seam.device == device)
        return nullptr;
    if (seam.device != VK_NULL_HANDLE && seam.device != device)
    {
        LOG_INFO("DLSS-NR Vulkan seam: the game made another VkDevice; starting over on it");
        Forget();
    }
    seam.device = device;
    seam.physical = physical;

    seam.getHandleProperties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
        vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandlePropertiesKHR"));
    seam.importSemaphore = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
        vkGetDeviceProcAddr(device, "vkImportSemaphoreWin32HandleKHR"));
    if (seam.getHandleProperties == nullptr || seam.importSemaphore == nullptr)
        return "the game's Vulkan device has no external memory or semaphore support";

    LUID luid {};
    if (!AdapterLuid(physical, luid))
        return "the GPU reports no Windows adapter LUID";

    if (!seam.d12)
    {
        ScopedSkipSpoofingGlobal skipSpoofing {};
        ScopedSkipVulkanHooks skipVulkanHooks {};
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
            return "the D3D12 adapter of the game's GPU was not found";
        const HRESULT created =
            D3d12Proxy::Module() != nullptr
                ? D3d12Proxy::D3D12CreateDevice_()(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&seam.d12))
                : D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&seam.d12));
        if (FAILED(created))
            return "OptiScaler's D3D12 device could not be created";
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(seam.d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&seam.queue))))
        return "the D3D12 queue could not be created";

    // lmxxf and mochizuki run between the two halves of the list (the D3D12 seam's evaluate cut), so their lists are
    // proxies and the queue gets the submission hook, as the D3D11 bridge's do.
    const bool proxied = Backend::SubmissionHooksWanted();
    if (!AmdBridge::EnsureSubmissionHook(seam.queue.Get()))
        return "the D3D12 submission hook could not be installed";
    if (proxied)
        Submission::Hooks::SetExpandEnabled(true);

    if (FAILED(seam.d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&seam.done))))
        return "a D3D12 fence could not be created";
    if (seam.doneEvent == nullptr)
        seam.doneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (seam.doneEvent == nullptr)
        return "an event could not be created";

    for (auto& slot : seam.slots)
    {
        if (FAILED(seam.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))))
            return "a D3D12 command allocator could not be created";
        const HRESULT list = proxied ? Submission::Hooks::CreateProxiedCommandList(
                                           seam.d12.Get(), 0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(),
                                           nullptr, IID_PPV_ARGS(&slot.list))
                                     : seam.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                   slot.allocator.Get(), nullptr,
                                                                   IID_PPV_ARGS(&slot.list));
        if (FAILED(list) || FAILED(slot.list->Close()))
            return "a D3D12 command list could not be created";

        HANDLE handle = nullptr;
        if (FAILED(seam.d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&slot.link))) ||
            FAILED(seam.d12->CreateSharedHandle(slot.link.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
            return "a shared D3D12 fence could not be created";
        VkImportSemaphoreWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        import.handle = handle;
        const bool linked = CreateTimeline(slot.linkSemaphore) &&
                            (import.semaphore = slot.linkSemaphore, seam.importSemaphore(device, &import)) ==
                                VK_SUCCESS;
        CloseHandle(handle);
        if (!linked)
            return "the shared D3D12 fence could not be imported into Vulkan";
        if (!CreateTimeline(slot.backSemaphore))
            return "a Vulkan timeline semaphore could not be created";
    }

    if (!CreateCopyPipeline())
        return "the D3D12 copy shader could not be built";

    seam.ready = true;
    LOG_INFO("DLSS-NR Vulkan seam: ready on D3D12 device {:X} ({} lists)", (size_t) seam.d12.Get(),
             proxied ? "proxied" : "plain");
    return nullptr;
}

// Command buffers of the queue family the game evaluates on.
bool EnsureFamily(uint32_t family)
{
    if (seam.pool != VK_NULL_HANDLE && seam.family == family)
        return true;
    if (seam.pool != VK_NULL_HANDLE)
    {
        LOG_INFO("DLSS-NR Vulkan seam: queue family {} -> {}", seam.family, family);
        DeferredRelease::Enqueue([device = seam.device, pool = seam.pool]()
                                 { vkDestroyCommandPool(device, pool, nullptr); }, (uint64_t) seam.device);
        seam.pool = VK_NULL_HANDLE;
    }
    VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = family;
    if (vkCreateCommandPool(seam.device, &poolInfo, nullptr, &seam.pool) != VK_SUCCESS)
        return false;
    seam.family = family;
    VkCommandBufferAllocateInfo allocate { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocate.commandPool = seam.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    for (auto& slot : seam.slots)
        if (vkAllocateCommandBuffers(seam.device, &allocate, &slot.back) != VK_SUCCESS ||
            vkAllocateCommandBuffers(seam.device, &allocate, &slot.next) != VK_SUCCESS)
            return false;
    return true;
}

bool Begin(VkCommandBuffer buffer)
{
    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkResetCommandBuffer(buffer, 0) == VK_SUCCESS && vkBeginCommandBuffer(buffer, &begin) == VK_SUCCESS;
}

void CopyAnswer(ID3D12GraphicsCommandList* list, ID3D12Resource* result, D3D12_RESOURCE_STATES resultState,
                uint32_t slotIndex)
{
    const auto desc = result->GetDesc();
    const UINT width = std::min<UINT>(static_cast<UINT>(desc.Width), seam.answer.width);
    const UINT height = std::min<UINT>(desc.Height, seam.answer.height);
    auto cpu = seam.heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = seam.heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(slotIndex) * 2 * seam.descriptorSize;
    gpu.ptr += UINT64(slotIndex) * 2 * seam.descriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = AmdPreSr::ReadFormat(desc.Format);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    seam.d12->CreateShaderResourceView(result, &srv, cpu);
    cpu.ptr += seam.descriptorSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    seam.d12->CreateUnorderedAccessView(seam.answer.d12.Get(), nullptr, &uav, cpu);

    const bool readable = (resultState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
    D3D12_RESOURCE_BARRIER barriers[2];
    UINT n = 0;
    if (!readable)
        Barrier(barriers, n, result, resultState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(barriers, n, seam.answer.d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(n, barriers);

    ID3D12DescriptorHeap* heaps[] { seam.heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(seam.root.Get());
    list->SetPipelineState(seam.pipeline.Get());
    list->SetComputeRootDescriptorTable(0, gpu);
    const UINT extent[] { width, height };
    list->SetComputeRoot32BitConstants(1, 2, extent, 0);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    n = 0;
    if (!readable)
        Barrier(barriers, n, result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, resultState);
    Barrier(barriers, n, seam.answer.d12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(n, barriers);
}

D3D12_RESOURCE_STATES StateFrom(const CustomOptional<int32_t, NoDefault>& configured)
{
    return configured.has_value() ? static_cast<D3D12_RESOURCE_STATES>(configured.value())
                                  : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}
} // namespace

bool Owns(VkPhysicalDevice physical)
{
    if (physical == VK_NULL_HANDLE || !AmdBridge::HasFiles())
        return false;
    static std::mutex cacheMutex;
    static VkPhysicalDevice cached = VK_NULL_HANDLE;
    static bool amd = false;
    std::lock_guard lock(cacheMutex);
    if (cached != physical)
    {
        LUID luid {};
        amd = AdapterLuid(physical, luid) && IsAmdAdapter(luid);
        cached = physical;
        LOG_INFO("DLSS-NR Vulkan seam: physical device {:X} is {}an AMD GPU", (size_t) physical, amd ? "" : "not ");
    }
    return amd;
}

VkCommandBuffer Before(VkCommandBuffer cmd, NVSDK_NGX_Parameter* params, VkPhysicalDevice physical, VkDevice device)
{
    replaced = nullptr;
    gameColour = nullptr;
    const auto& cfg = *Config::Instance();
    if (cmd == VK_NULL_HANDLE || params == nullptr || device == VK_NULL_HANDLE ||
        !cfg.DlssNrEnabled.value_or_default() || !Owns(physical) || AmdBridge::Exiting())
        return cmd;

    // A second evaluate in a command buffer already split at the first: the game's recording, and so the upscaler,
    // went on in that continuation. Splitting again inside it is not supported.
    if (const auto continuation = Vulkan_wDx12::GetVirtualCommandBuffer(cmd); continuation != VK_NULL_HANDLE)
    {
        Report("a second upscaler evaluate in one command buffer passes through");
        return continuation;
    }

    const auto kind = Backend::ActiveKindFromConfig();
    if (kind == Backend::Kind::Off)
        return cmd;
    // The D3D12 seam's "After the finished frame" (danielblnc's ApplyAfterRR) edits the upscaler's output in place;
    // on Vulkan NR runs before Super Resolution only.
    if (kind == Backend::Kind::Daniel && cfg.DlssNrApplyAfterRR.value_or_default())
    {
        Report("\"After the finished frame\" is Direct3D 12 only; turn it off to run NR before Super Resolution");
        return cmd;
    }

    // From here the game's command buffers are tracked, so the continuation can take over their bound state.
    State::Instance().vulkanSeamTracking = true;

    std::lock_guard lock(mutex);
    if (seam.stopped)
        return cmd;

    // A submit that could not be split ran the game's command buffer without its continuation: that frame lost
    // everything recorded after the evaluate, and every frame would.
    if (seam.frame > 0 && Vulkan_wDx12::abortedSubmissions.load() != seam.aborts)
    {
        Stop("the game's queue submit could not be split (see \"Aborting Vulkan w/Dx12 submit transaction\" above)");
        return cmd;
    }

    const auto level = Vulkan_wDx12::cmdBufferStateTracker.GetCommandBufferLevel(cmd);
    const auto family = Vulkan_wDx12::cmdBufferStateTracker.GetCommandBufferQueueFamily(cmd);
    if (!level.has_value() || *level != VK_COMMAND_BUFFER_LEVEL_PRIMARY)
    {
        Report("the upscaler runs in a secondary or unknown command buffer");
        return cmd;
    }
    if (!family.has_value())
    {
        Report("the upscaler's command buffer comes from a pool of unknown queue family");
        return cmd;
    }

    auto* colourIn = Param(params, NVSDK_NGX_Parameter_Color);
    auto* depthIn = Param(params, NVSDK_NGX_Parameter_Depth);
    auto* motionIn = Param(params, NVSDK_NGX_Parameter_MotionVectors);
    auto* exposureIn = Param(params, NVSDK_NGX_Parameter_ExposureTexture);
    if (colourIn == nullptr || depthIn == nullptr || motionIn == nullptr)
    {
        Report(colourIn == nullptr  ? "the parameter block carried no colour"
               : depthIn == nullptr ? "the parameter block carried no depth"
                                    : "the parameter block carried no motion vectors");
        return cmd;
    }
    const auto& colourInfo = colourIn->Resource.ImageViewInfo;
    const auto& depthInfo = depthIn->Resource.ImageViewInfo;
    const auto& motionInfo = motionIn->Resource.ImageViewInfo;
    const DXGI_FORMAT colourFormat = SharedFormat(colourInfo.Format);
    const DXGI_FORMAT motionFormat = SharedFormat(motionInfo.Format);
    if (colourFormat == DXGI_FORMAT_UNKNOWN || motionFormat == DXGI_FORMAT_UNKNOWN)
    {
        Report(std::string(colourFormat == DXGI_FORMAT_UNKNOWN ? "colour" : "motion vector") + " format " +
               std::to_string(int(colourFormat == DXGI_FORMAT_UNKNOWN ? colourInfo.Format : motionInfo.Format)) +
               " cannot cross to D3D12");
        return cmd;
    }
    const DXGI_FORMAT exposureFormat = exposureIn != nullptr ? SharedFormat(exposureIn->Resource.ImageViewInfo.Format)
                                                             : DXGI_FORMAT_UNKNOWN;
    if (exposureIn != nullptr && exposureFormat == DXGI_FORMAT_UNKNOWN)
    {
        static bool said = false;
        if (!std::exchange(said, true))
            LOG_INFO("DLSS-NR Vulkan seam: exposure format {} cannot cross to D3D12; the runtime goes without it",
                     (int) exposureIn->Resource.ImageViewInfo.Format);
        exposureIn = nullptr;
    }

    if (const char* why = Init(physical, device))
    {
        Stop(why);
        return cmd;
    }

    // The slot comes back once the D3D12 work of the frame before last finished.
    const uint64_t frameNo = seam.frame + 1;
    const uint32_t slotIndex = static_cast<uint32_t>(frameNo % kSlots);
    Slot& slot = seam.slots[slotIndex];
    if (frameNo > 2)
    {
        const uint64_t needed = frameNo - 2;
        const uint64_t completed = seam.done->GetCompletedValue();
        if (completed == UINT64_MAX)
        {
            Stop("OptiScaler's D3D12 device was removed");
            return cmd;
        }
        if (completed < needed)
        {
            if (GetTickCount64() - seam.waitedOutAt < 1000)
                return cmd;
            if (FAILED(seam.done->SetEventOnCompletion(needed, seam.doneEvent)) ||
                WaitForSingleObject(seam.doneEvent, 2000) != WAIT_OBJECT_0)
            {
                seam.waitedOutAt = GetTickCount64();
                Report("an earlier frame's NR has not finished (that frame was not submitted yet); frames pass "
                       "through meanwhile");
                return cmd;
            }
        }
    }

    if (!EnsureFamily(*family))
    {
        Stop("Vulkan command buffers could not be created");
        return cmd;
    }

    // The images that cross, sized and formatted like the game's. A new answer image was never Vulkan's to hand back.
    if (!seam.answer.Matches(colourInfo.Width, colourInfo.Height, VK_FORMAT_R16G16B16A16_SFLOAT))
        seam.answerOwned = false;
    if ((!seam.colour.Matches(colourInfo.Width, colourInfo.Height, colourInfo.Format) &&
         !CreateShared(seam.colour, colourInfo.Width, colourInfo.Height, colourInfo.Format, colourFormat, false)) ||
        (!seam.motion.Matches(motionInfo.Width, motionInfo.Height, motionInfo.Format) &&
         !CreateShared(seam.motion, motionInfo.Width, motionInfo.Height, motionInfo.Format, motionFormat, false)) ||
        (!seam.depth.Matches(depthInfo.Width, depthInfo.Height, VK_FORMAT_R32_SFLOAT) &&
         !CreateShared(seam.depth, depthInfo.Width, depthInfo.Height, VK_FORMAT_R32_SFLOAT, DXGI_FORMAT_R32_FLOAT,
                       false)) ||
        (!seam.answer.Matches(colourInfo.Width, colourInfo.Height, VK_FORMAT_R16G16B16A16_SFLOAT) &&
         !CreateShared(seam.answer, colourInfo.Width, colourInfo.Height, VK_FORMAT_R16G16B16A16_SFLOAT,
                       DXGI_FORMAT_R16G16B16A16_FLOAT, true)) ||
        (exposureIn != nullptr &&
         !seam.exposure.Matches(exposureIn->Resource.ImageViewInfo.Width, exposureIn->Resource.ImageViewInfo.Height,
                                exposureIn->Resource.ImageViewInfo.Format) &&
         !CreateShared(seam.exposure, exposureIn->Resource.ImageViewInfo.Width,
                       exposureIn->Resource.ImageViewInfo.Height, exposureIn->Resource.ImageViewInfo.Format,
                       exposureFormat, false)))
    {
        Stop("an image shared with D3D12 could not be created");
        return cmd;
    }

    if (!seam.colourCopy)
        seam.colourCopy = std::make_unique<ResourceCopy_Vk>("NR seam colour", device, physical);
    if (!seam.motionCopy)
        seam.motionCopy = std::make_unique<ResourceCopy_Vk>("NR seam motion", device, physical);
    if (!seam.exposureCopy)
        seam.exposureCopy = std::make_unique<ResourceCopy_Vk>("NR seam exposure", device, physical);
    if (!seam.depthCopy || seam.depthFormat != depthInfo.Format)
    {
        // Its descriptor sets may still be bound by an earlier frame's command buffer.
        if (seam.depthCopy)
            DeferredRelease::Enqueue([old = seam.depthCopy.release()]() { delete old; }, (uint64_t) device);
        seam.depthCopy = std::make_unique<DepthTransfer_Vk>("NR seam depth", device, physical, depthInfo.Format);
        seam.depthFormat = depthInfo.Format;
    }
    if (!seam.colourCopy->CanRender() || !seam.motionCopy->CanRender() || !seam.exposureCopy->CanRender() ||
        !seam.depthCopy->CanRender())
    {
        Stop("the Vulkan copy shaders could not be built");
        return cmd;
    }

    // The copies, on the game's command buffer after everything it recorded before its evaluate. The game's images
    // are read where upscalers read them, in SHADER_READ_ONLY_OPTIMAL, and never transitioned.
    const uint32_t queueFamily = *family;
    {
        VkMemoryBarrier visible { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        visible.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        visible.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkImageMemoryBarrier barriers[6];
        uint32_t n = 0;
        for (auto* s : { &seam.colour, &seam.motion, &seam.depth })
            barriers[n++] = ImageBarrier(s->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                         VK_QUEUE_FAMILY_EXTERNAL, queueFamily, 0, VK_ACCESS_SHADER_WRITE_BIT);
        if (exposureIn != nullptr)
            barriers[n++] = ImageBarrier(seam.exposure.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                         VK_QUEUE_FAMILY_EXTERNAL, queueFamily, 0, VK_ACCESS_SHADER_WRITE_BIT);
        // The upscaler of the last frame read the answer; D3D12 writes it next.
        if (seam.answerOwned)
            barriers[n++] = ImageBarrier(seam.answer.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                         VK_IMAGE_LAYOUT_GENERAL, queueFamily, VK_QUEUE_FAMILY_EXTERNAL,
                                         VK_ACCESS_SHADER_READ_BIT, 0);
        seam.answerOwned = false;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &visible, 0, nullptr, n, barriers);

        seam.colourCopy->Dispatch(device, cmd, colourInfo.ImageView, seam.colour.view,
                                  { colourInfo.Width, colourInfo.Height });
        seam.motionCopy->Dispatch(device, cmd, motionInfo.ImageView, seam.motion.view,
                                  { motionInfo.Width, motionInfo.Height });
        seam.depthCopy->Dispatch(device, cmd, depthInfo.ImageView, seam.depth.view, { depthInfo.Width, depthInfo.Height });
        if (exposureIn != nullptr)
            seam.exposureCopy->Dispatch(device, cmd, exposureIn->Resource.ImageViewInfo.ImageView, seam.exposure.view,
                                        { exposureIn->Resource.ImageViewInfo.Width,
                                          exposureIn->Resource.ImageViewInfo.Height });

        n = 0;
        for (auto* s : { &seam.colour, &seam.motion, &seam.depth })
            barriers[n++] = ImageBarrier(s->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, queueFamily,
                                         VK_QUEUE_FAMILY_EXTERNAL, VK_ACCESS_SHADER_WRITE_BIT, 0);
        if (exposureIn != nullptr)
            barriers[n++] = ImageBarrier(seam.exposure.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                         queueFamily, VK_QUEUE_FAMILY_EXTERNAL, VK_ACCESS_SHADER_WRITE_BIT, 0);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, n, barriers);
    }

    // The copy-back: the answer comes back to Vulkan where the upscaler reads its colour.
    if (!Begin(slot.back))
    {
        Report("a Vulkan command buffer could not be recorded", true);
        return cmd;
    }
    {
        const auto acquire = ImageBarrier(seam.answer.image, VK_IMAGE_LAYOUT_GENERAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_EXTERNAL,
                                          queueFamily, 0, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(slot.back, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &acquire);
    }
    if (vkEndCommandBuffer(slot.back) != VK_SUCCESS)
    {
        Report("a Vulkan command buffer could not be recorded", true);
        return cmd;
    }

    // The continuation: the game's recording moves there, with the state it had bound.
    if (!Begin(slot.next))
    {
        Report("a Vulkan command buffer could not be recorded", true);
        return cmd;
    }
    if (!Vulkan_wDx12::RegisterVirtualCommandBuffer(cmd, slot.next))
    {
        vkEndCommandBuffer(slot.next);
        Report("the game's command buffer could not be split", true);
        return cmd;
    }
    {
        vk_state::ReplayParams replay {};
        replay.ReplayGraphicsPipeline = true;
        replay.ReplayComputeToo = false;
        replay.ReplayViewportScissor = true;
        replay.ReplayExtendedDynamicState = true;
        replay.ReplayPushConstants = true;
        replay.ReplayVertexIndex = true;
        replay.RequiredGraphicsSetMask = 0xFFFFFFFFu;
        replay.OverrideGraphicsLayout = VK_NULL_HANDLE;
        Vulkan_wDx12::cmdBufferStateTracker.CaptureAndReplay(cmd, slot.next, replay);
    }
    auto unsplit = [&]()
    {
        if (const auto next = Vulkan_wDx12::RemoveVirtualCommandBuffer(cmd); next != VK_NULL_HANDLE)
            Vulkan_wDx12::EndCmdBuffer(next);
    };

    // The D3D12 seam on the copies, exactly as a D3D12 game's upscaler evaluate reaches it.
    if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr)))
    {
        unsplit();
        Report("the D3D12 command list could not be reset", true);
        return cmd;
    }
    const D3D12_RESOURCE_STATES colourState = StateFrom(cfg.ColorResourceBarrier);
    const D3D12_RESOURCE_STATES motionState = StateFrom(cfg.MVResourceBarrier);
    const D3D12_RESOURCE_STATES depthState = StateFrom(cfg.DepthResourceBarrier);
    const D3D12_RESOURCE_STATES exposureState = StateFrom(cfg.ExposureResourceBarrier);
    ID3D12Resource* exposure12 = exposureIn != nullptr ? seam.exposure.d12.Get() : nullptr;
    {
        D3D12_RESOURCE_BARRIER barriers[4];
        UINT n = 0;
        Barrier(barriers, n, seam.colour.d12.Get(), D3D12_RESOURCE_STATE_COMMON, colourState);
        Barrier(barriers, n, seam.motion.d12.Get(), D3D12_RESOURCE_STATE_COMMON, motionState);
        Barrier(barriers, n, seam.depth.d12.Get(), D3D12_RESOURCE_STATE_COMMON, depthState);
        Barrier(barriers, n, exposure12, D3D12_RESOURCE_STATE_COMMON, exposureState);
        if (n)
            slot.list->ResourceBarrier(n, barriers);
    }

    void* gameDepth = nullptr;
    void* gameMotion = nullptr;
    void* gameExposure = nullptr;
    void* colourVk = nullptr;
    params->Get(NVSDK_NGX_Parameter_Color, &colourVk);
    params->Get(NVSDK_NGX_Parameter_Depth, &gameDepth);
    params->Get(NVSDK_NGX_Parameter_MotionVectors, &gameMotion);
    params->Get(NVSDK_NGX_Parameter_ExposureTexture, &gameExposure);
    params->Set(NVSDK_NGX_Parameter_Color, (void*) seam.colour.d12.Get());
    params->Set(NVSDK_NGX_Parameter_Depth, (void*) seam.depth.d12.Get());
    params->Set(NVSDK_NGX_Parameter_MotionVectors, (void*) seam.motion.d12.Get());
    params->Set(NVSDK_NGX_Parameter_ExposureTexture, (void*) exposure12);

    EvaluateBeforeUpscale(slot.list.Get(), params, seam.queue.Get(), frameNo);

    ID3D12Resource* result = nullptr;
    if (AmdBridge::HasReplacement(params))
        params->Get(NVSDK_NGX_Parameter_Color, (void**) &result);
    AmdBridge::Restore(params);
    params->Set(NVSDK_NGX_Parameter_Color, colourVk);
    params->Set(NVSDK_NGX_Parameter_Depth, gameDepth);
    params->Set(NVSDK_NGX_Parameter_MotionVectors, gameMotion);
    params->Set(NVSDK_NGX_Parameter_ExposureTexture, gameExposure);

    if (result != nullptr)
        CopyAnswer(slot.list.Get(), result, colourState, slotIndex);
    {
        D3D12_RESOURCE_BARRIER barriers[4];
        UINT n = 0;
        Barrier(barriers, n, seam.colour.d12.Get(), colourState, D3D12_RESOURCE_STATE_COMMON);
        Barrier(barriers, n, seam.motion.d12.Get(), motionState, D3D12_RESOURCE_STATE_COMMON);
        Barrier(barriers, n, seam.depth.d12.Get(), depthState, D3D12_RESOURCE_STATE_COMMON);
        Barrier(barriers, n, exposure12, exposureState, D3D12_RESOURCE_STATE_COMMON);
        if (n)
            slot.list->ResourceBarrier(n, barriers);
    }
    const bool closed = SUCCEEDED(slot.list->Close());

    // The D3D12 work goes to its queue once the game's submit carrying this command buffer is made (Settled): lmxxf's
    // and danielblnc's HIP wait on the CPU until the copies they read are done, which needs that submit first. What
    // the runtime recorded runs either way, its jobs and slots are released by its submission; when the transaction
    // cannot be set up only the order against Vulkan is lost.
    const uint64_t copied = ++seam.value;
    const uint64_t finished = ++seam.value;
    const uint64_t backValue = ++seam.value;
    Vulkan_wDx12::PendingSubmission pending {};
    pending.submitCommandBuffer = cmd;
    pending.resourceCopySemaphore = slot.linkSemaphore;
    pending.resourceCopyFence = slot.link.Get();
    pending.resourceCopyValue = copied;
    pending.copyBackSemaphore = slot.backSemaphore;
    pending.d3d12CompleteValue = finished;
    pending.copyBackValue = backValue;
    pending.copyBackCommandBuffer = slot.back;
    pending.barrierCommandBuffer = slot.next;
    pending.settled = &Settled;
    pending.settledContext = reinterpret_cast<void*>(uintptr_t(slotIndex));
    slot.copied = copied;
    slot.finished = finished;
    slot.frameNo = frameNo;
    slot.queued = closed;
    if (!closed || !Vulkan_wDx12::RegisterPendingSubmission(pending))
    {
        slot.queued = false;
        unsplit();
        if (closed)
        {
            ID3D12CommandList* lists[] { slot.list.Get() };
            seam.queue->ExecuteCommandLists(1, lists);
        }
        seam.queue->Signal(seam.done.Get(), frameNo);
        seam.frame = frameNo;
        Report(closed ? "the split submission could not be registered" : "the D3D12 command list could not be closed",
               true);
        return cmd;
    }
    seam.frame = frameNo;
    seam.answerOwned = true;
    seam.aborts = Vulkan_wDx12::abortedSubmissions.load();

    // Said when it changes (the network building after a resize, the runtime settling, busy or stood down), so a log
    // shows when NR left and came back.
    ++seam.sinceChange;
    if ((result != nullptr) != seam.answering)
    {
        seam.answering = result != nullptr;
        if (++seam.changes <= 40 || seam.changes % 100 == 0)
        {
            if (seam.answering)
                LOG_INFO("DLSS-NR Vulkan seam: NR answers again after {} frames passed through", seam.sinceChange);
            else
                LOG_INFO("DLSS-NR Vulkan seam: frames pass through ({})", AmdBridge::Status());
        }
        seam.sinceChange = 0;
    }

    if (result != nullptr)
    {
        replaced = params;
        gameColour = colourVk;
        params->Set(NVSDK_NGX_Parameter_Color, (void*) &seam.answer.ngx);
        if (frames.fetch_add(1) == 0)
            LOG_INFO("DLSS-NR Vulkan seam: first frame of NR before Super Resolution ({}x{}, colour format {})",
                     colourInfo.Width, colourInfo.Height, (int) colourInfo.Format);
        std::lock_guard statusLock(statusMutex);
        status.clear();
    }
    return slot.next;
}

void After(NVSDK_NGX_Parameter* params)
{
    if (params != nullptr && params == replaced)
        params->Set(NVSDK_NGX_Parameter_Color, gameColour);
    replaced = nullptr;
    gameColour = nullptr;
}

unsigned long long Frames() { return frames.load(); }

std::string Status()
{
    std::lock_guard lock(statusMutex);
    return status;
}
} // namespace DlssNr::VkSeam
