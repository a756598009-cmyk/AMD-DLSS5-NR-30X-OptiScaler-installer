#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>

#include <nvsdk_ngx_vk.h>

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

// NR at the seam of OptiScaler's native Vulkan upscalers (dlssnr/amd/VulkanSeam). Run in a folder holding
// OptiScaler.dll, OptiScaler.ini and an AMD NR runtime (tools\test-vulkan-seam.cmd prepares one). The program is a
// small Vulkan game that drives OptiScaler's NGX Vulkan exports the way a DLSS or FSR 2 title does through them: one
// SR feature, an evaluate every frame on the game's command buffer, and after the evaluate the game goes on recording
// in that command buffer (here: a copy of the upscaled frame to a readback buffer), which the seam moves to the
// continuation it splits off.
//
//   vulkan_seam_smoke run <label>          renders, and writes <label>.bin with the frame at each checkpoint
//   vulkan_seam_smoke compare <on> <off>   the frames of an NR run against those of a run without NR
//
// The run walks through what a game does: first frames, a quality change (the feature made again at another render
// size), a pause where no evaluate comes (a menu), a command buffer recorded twice before it is submitted, and the
// evaluate again. Exit code is the number of failed checks.

namespace
{
constexpr uint32_t kDisplayW = 1920, kDisplayH = 1080;
constexpr uint32_t kSampleW = 240, kSampleH = 135;
int failures = 0;
// The upscaler is a Vk w/Dx12 one (VulkanUpscaler=fsr22_12): NR runs at the D3D12 seam inside that bridge, and the
// Vulkan seam must stay out of it.
bool bridged = false;

void Print(const char* format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::printf("%s\n", text);
    std::fflush(stdout);
}

void Check(bool ok, const char* what)
{
    Print("%s %s", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

std::string ReadLog()
{
    std::ifstream file("OptiScaler.log", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

size_t Count(const std::string& text, const char* needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

uint16_t FloatToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    int exp = int((x >> 23) & 0xff) - 127 + 15;
    uint32_t man = x & 0x7fffff;
    if (exp <= 0)
        return uint16_t(sign);
    if (exp >= 31)
        return uint16_t(sign | 0x7c00);
    return uint16_t(sign | (exp << 10) | (man >> 13));
}

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
    std::memcpy(&f, &bits, 4);
    return f;
}

using PFN_Init = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice,
                                      PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr,
                                      const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
using PFN_GetCapability = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
using PFN_Create = NVSDK_NGX_Result (*)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                        NVSDK_NGX_Handle**);
using PFN_Evaluate = NVSDK_NGX_Result (*)(VkCommandBuffer, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*,
                                          PFN_NVSDK_NGX_ProgressCallback);
using PFN_Release = NVSDK_NGX_Result (*)(NVSDK_NGX_Handle*);

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    NVSDK_NGX_Resource_VK ngx {};
};

struct Game
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE, staging = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE, stagingMemory = VK_NULL_HANDLE;
    void* readbackMapped = nullptr;
    void* stagingMapped = nullptr;
    Image colour, depth, motion, output;
    PFN_Init init = nullptr;
    PFN_GetCapability capability = nullptr;
    PFN_Create create = nullptr;
    PFN_Evaluate evaluate = nullptr;
    PFN_Release release = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    uint32_t renderW = 0, renderH = 0;
    bool firstFrame = true;
    bool sourcesReady = false;
};

Game game;

uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties properties {};
    vkGetPhysicalDeviceMemoryProperties(game.physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

bool MakeImage(Image& image, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
               VkImageAspectFlags aspect)
{
    VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { width, height, 1 };
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    if (vkCreateImage(game.device, &info, nullptr, &image.image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(game.device, image.image, &requirements);
    VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(game.device, &allocate, nullptr, &image.memory) != VK_SUCCESS ||
        vkBindImageMemory(game.device, image.image, image.memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = image.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = { aspect, 0, 1, 0, 1 };
    if (vkCreateImageView(game.device, &view, nullptr, &image.view) != VK_SUCCESS)
        return false;
    image.format = format;
    image.width = width;
    image.height = height;
    image.ngx.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    image.ngx.Resource.ImageViewInfo.ImageView = image.view;
    image.ngx.Resource.ImageViewInfo.Image = image.image;
    image.ngx.Resource.ImageViewInfo.SubresourceRange = view.subresourceRange;
    image.ngx.Resource.ImageViewInfo.Format = format;
    image.ngx.Resource.ImageViewInfo.Width = width;
    image.ngx.Resource.ImageViewInfo.Height = height;
    image.ngx.ReadWrite = (usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    return true;
}

void FreeImage(Image& image)
{
    if (image.view)
        vkDestroyImageView(game.device, image.view, nullptr);
    if (image.image)
        vkDestroyImage(game.device, image.image, nullptr);
    if (image.memory)
        vkFreeMemory(game.device, image.memory, nullptr);
    image = {};
}

bool MakeBuffer(VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = size;
    info.usage = usage;
    if (vkCreateBuffer(game.device, &info, nullptr, &buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements {};
    vkGetBufferMemoryRequirements(game.device, buffer, &requirements);
    VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    return vkAllocateMemory(game.device, &allocate, nullptr, &memory) == VK_SUCCESS &&
           vkBindBufferMemory(game.device, buffer, memory, 0) == VK_SUCCESS &&
           vkMapMemory(game.device, memory, 0, size, 0, &mapped) == VK_SUCCESS;
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { aspect, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
}

// A detailed picture the network has something to say about: soft gradients, edges, fine stripes and noise.
void FillPattern(uint32_t width, uint32_t height)
{
    auto* texels = static_cast<uint16_t*>(game.stagingMapped);
    uint32_t seed = 12345;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            seed = seed * 1664525u + 1013904223u;
            const float u = float(x) / width, v = float(y) / height;
            const float noise = float(seed >> 8) / float(1u << 24) * 0.08f;
            const bool disc = (u - .35f) * (u - .35f) + (v - .5f) * (v - .5f) < .04f;
            const float stripes = ((x / 3 + y / 7) % 2) ? .1f : 0.f;
            float r = .2f + .6f * u + noise + (disc ? .4f : 0.f);
            float g = .3f + .4f * v + stripes + noise;
            float b = .5f + .3f * std::sin(u * 40.f) * std::cos(v * 23.f) + noise;
            uint16_t* t = texels + (size_t(y) * width + x) * 4;
            t[0] = FloatToHalf(r);
            t[1] = FloatToHalf(g);
            t[2] = FloatToHalf(b);
            t[3] = FloatToHalf(1.f);
        }
}

bool MakeSources(uint32_t renderW, uint32_t renderH)
{
    FreeImage(game.colour);
    FreeImage(game.depth);
    FreeImage(game.motion);
    const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!MakeImage(game.colour, renderW, renderH, VK_FORMAT_R16G16B16A16_SFLOAT,
                   sampled | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !MakeImage(game.depth, renderW, renderH, VK_FORMAT_D32_SFLOAT,
                   sampled | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT) ||
        !MakeImage(game.motion, renderW, renderH, VK_FORMAT_R16G16_SFLOAT, sampled | VK_IMAGE_USAGE_STORAGE_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    FillPattern(renderW, renderH);
    game.renderW = renderW;
    game.renderH = renderH;
    game.sourcesReady = false;
    return true;
}

// The sources of this render size: the picture uploaded, depth 0.5, no motion. Recorded on the frame's own buffer.
void RecordSources(VkCommandBuffer cmd)
{
    Barrier(cmd, game.colour.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { game.renderW, game.renderH, 1 };
    vkCmdCopyBufferToImage(cmd, game.staging, game.colour.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Barrier(cmd, game.colour.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Barrier(cmd, game.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearDepthStencilValue depth { 0.5f, 0 };
    VkImageSubresourceRange depthRange { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    vkCmdClearDepthStencilImage(cmd, game.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth, 1, &depthRange);
    Barrier(cmd, game.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Barrier(cmd, game.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue zero {};
    VkImageSubresourceRange colourRange { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, game.motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &colourRange);
    Barrier(cmd, game.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    game.sourcesReady = true;
}

bool Begin()
{
    if (vkWaitForFences(game.device, 1, &game.fence, VK_TRUE, 10'000'000'000ull) != VK_SUCCESS)
        return false;
    vkResetFences(game.device, 1, &game.fence);
    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(game.cmd, &begin) == VK_SUCCESS;
}

bool Submit()
{
    if (vkEndCommandBuffer(game.cmd) != VK_SUCCESS)
        return false;
    VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &game.cmd;
    return vkQueueSubmit(game.queue, 1, &submit, game.fence) == VK_SUCCESS;
}

bool CreateFeature(uint32_t renderW, uint32_t renderH)
{
    if (game.feature)
    {
        vkDeviceWaitIdle(game.device);
        game.release(game.feature);
        game.feature = nullptr;
    }
    if (!MakeSources(renderW, renderH))
        return false;
    auto* p = game.params;
    p->Set(NVSDK_NGX_Parameter_Width, renderW);
    p->Set(NVSDK_NGX_Parameter_Height, renderH);
    p->Set(NVSDK_NGX_Parameter_OutWidth, kDisplayW);
    p->Set(NVSDK_NGX_Parameter_OutHeight, kDisplayH);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int) NVSDK_NGX_PerfQuality_Value_Balanced);
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
           (int) (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
    if (!Begin())
        return false;
    const auto created = game.create(game.device, game.cmd, NVSDK_NGX_Feature_SuperSampling, p, &game.feature);
    if (!Submit() || created != NVSDK_NGX_Result_Success)
    {
        Print("CreateFeature1 returned %X", (unsigned) created);
        return false;
    }
    return vkWaitForFences(game.device, 1, &game.fence, VK_TRUE, 10'000'000'000ull) == VK_SUCCESS;
}

// One frame: the evaluate on the game's command buffer, then the game's own copy of the upscaled frame.
bool Frame(bool evaluate, bool reset, bool recordTwice = false)
{
    for (int pass = recordTwice ? 0 : 1; pass < 2; ++pass)
    {
        if (pass == 1 || !recordTwice)
        {
            if (!Begin())
                return false;
        }
        else
        {
            // The first recording is thrown away before it is submitted.
            if (vkWaitForFences(game.device, 1, &game.fence, VK_TRUE, 10'000'000'000ull) != VK_SUCCESS)
                return false;
            VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            if (vkBeginCommandBuffer(game.cmd, &begin) != VK_SUCCESS)
                return false;
        }
        if (!game.sourcesReady)
            RecordSources(game.cmd);
        if (game.firstFrame)
        {
            Barrier(game.cmd, game.output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_GENERAL);
            game.firstFrame = false;
        }
        if (evaluate)
        {
            auto* p = game.params;
            p->Set(NVSDK_NGX_Parameter_Color, (void*) &game.colour.ngx);
            p->Set(NVSDK_NGX_Parameter_Depth, (void*) &game.depth.ngx);
            p->Set(NVSDK_NGX_Parameter_MotionVectors, (void*) &game.motion.ngx);
            p->Set(NVSDK_NGX_Parameter_Output, (void*) &game.output.ngx);
            p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
            p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
            p->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
            p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
            p->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
            p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, game.renderW);
            p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, game.renderH);
            p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 16.6f);
            const auto result = game.evaluate(game.cmd, game.feature, p, nullptr);
            if (result != NVSDK_NGX_Result_Success)
            {
                Print("EvaluateFeature returned %X", (unsigned) result);
                return false;
            }
        }
        if (pass == 0)
        {
            vkEndCommandBuffer(game.cmd);
            vkResetCommandBuffer(game.cmd, 0);
            continue;
        }
        // What the game records after its evaluate: it reads the upscaled frame.
        Barrier(game.cmd, game.output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { kDisplayW, kDisplayH, 1 };
        vkCmdCopyImageToBuffer(game.cmd, game.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, game.readback, 1,
                               &region);
        Barrier(game.cmd, game.output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL);
        if (!Submit())
            return false;
    }
    return true;
}

// The upscaled frame, averaged down to kSampleW x kSampleH RGB.
std::vector<float> Sample()
{
    vkWaitForFences(game.device, 1, &game.fence, VK_TRUE, 10'000'000'000ull);
    std::vector<float> out(size_t(kSampleW) * kSampleH * 3, 0.f);
    const auto* texels = static_cast<const uint16_t*>(game.readbackMapped);
    const uint32_t bx = kDisplayW / kSampleW, by = kDisplayH / kSampleH;
    for (uint32_t y = 0; y < kSampleH * by; ++y)
        for (uint32_t x = 0; x < kSampleW * bx; ++x)
        {
            const uint16_t* t = texels + (size_t(y) * kDisplayW + x) * 4;
            float* o = out.data() + (size_t(y / by) * kSampleW + x / bx) * 3;
            for (int c = 0; c < 3; ++c)
                o[c] += HalfToFloat(t[c]) / float(bx * by);
        }
    return out;
}

bool Setup()
{
    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.pApplicationInfo = &app;
    if (vkCreateInstance(&instanceInfo, nullptr, &game.instance) != VK_SUCCESS)
        return false;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(game.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(game.instance, &count, physicals.data());
    for (auto candidate : physicals)
    {
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && game.physical)
            continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queues.data());
        for (uint32_t i = 0; i < families; ++i)
            if ((queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            {
                game.physical = candidate;
                game.family = i;
                Print("GPU: %s", properties.deviceName);
                break;
            }
    }
    if (!game.physical)
        return false;
    float priority = 1.f;
    VkDeviceQueueCreateInfo queueInfo { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = game.family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features features12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.pNext = &features12;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    if (vkCreateDevice(game.physical, &deviceInfo, nullptr, &game.device) != VK_SUCCESS)
        return false;
    vkGetDeviceQueue(game.device, game.family, 0, &game.queue);
    VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = game.family;
    VkCommandBufferAllocateInfo allocate { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateCommandPool(game.device, &poolInfo, nullptr, &game.pool) != VK_SUCCESS)
        return false;
    allocate.commandPool = game.pool;
    if (vkAllocateCommandBuffers(game.device, &allocate, &game.cmd) != VK_SUCCESS ||
        vkCreateFence(game.device, &fenceInfo, nullptr, &game.fence) != VK_SUCCESS)
        return false;
    if (!MakeBuffer(game.readback, game.readbackMemory, game.readbackMapped, VkDeviceSize(kDisplayW) * kDisplayH * 8,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        !MakeBuffer(game.staging, game.stagingMemory, game.stagingMapped, VkDeviceSize(kDisplayW) * kDisplayH * 8,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !MakeImage(game.output, kDisplayW, kDisplayH, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT))
        return false;

    const HMODULE opti = GetModuleHandleW(L"OptiScaler.dll");
    game.init = (PFN_Init) GetProcAddress(opti, "NVSDK_NGX_VULKAN_Init");
    game.capability = (PFN_GetCapability) GetProcAddress(opti, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    game.create = (PFN_Create) GetProcAddress(opti, "NVSDK_NGX_VULKAN_CreateFeature1");
    game.evaluate = (PFN_Evaluate) GetProcAddress(opti, "NVSDK_NGX_VULKAN_EvaluateFeature");
    game.release = (PFN_Release) GetProcAddress(opti, "NVSDK_NGX_VULKAN_ReleaseFeature");
    if (!game.init || !game.capability || !game.create || !game.evaluate || !game.release)
        return false;
    wchar_t here[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, here);
    if (game.init(0x5eed, here, game.instance, game.physical, game.device, vkGetInstanceProcAddr, vkGetDeviceProcAddr,
                  nullptr, NVSDK_NGX_Version_API) != NVSDK_NGX_Result_Success ||
        game.capability(&game.params) != NVSDK_NGX_Result_Success || !game.params)
        return false;
    return true;
}

// Frames until the log says NR reached the upscaler, or the time runs out.
bool RunUntil(const char* needle, size_t atLeast, int seconds, bool reset)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    for (int frame = 0; std::chrono::steady_clock::now() < end; ++frame)
    {
        if (!Frame(true, reset && frame == 0))
            return false;
        if (frame % 30 == 29 && Count(ReadLog(), needle) >= atLeast)
            return true;
    }
    return false;
}

// Frames of play for a while, for a runtime that settles without saying so in a line of ours.
void PlayFor(int seconds)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end)
        if (!Frame(true, false))
            return;
}

// Frames of play; answers the longest one in milliseconds.
double Play(int frames)
{
    double longest = 0;
    for (int i = 0; i < frames; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        if (!Frame(true, false))
            return 1e9;
        vkWaitForFences(game.device, 1, &game.fence, VK_TRUE, 10'000'000'000ull);
        longest = std::max(longest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                                        .count());
    }
    return longest;
}

int Run(const std::string& label)
{
    if (!LoadLibraryW(L"vulkan-1.dll") || !LoadLibraryW(L"OptiScaler.dll"))
    {
        Print("FAIL setup: OptiScaler.dll or vulkan-1.dll did not load (%lu)", GetLastError());
        return 1;
    }
    Sleep(1000);
    if (!Setup())
    {
        Print("FAIL setup: Vulkan device or NGX init");
        return 1;
    }
    const std::string startLog = ReadLog();
    const bool nrOn = startLog.find("DLSS-NR Vulkan seam: timeline semaphores") != std::string::npos;
    Print("NR seam expected: %s", nrOn ? "yes" : "no");
    std::vector<std::vector<float>> checkpoints;

    // 1: the first frames, until NR's answer reaches the upscaler (mochizuki builds its network first).
    Check(CreateFeature(1280, 720), "feature created at 1280x720 -> 1920x1080");
    if (nrOn && bridged)
        PlayFor(45);
    else if (nrOn)
        Check(RunUntil("first frame of NR before Super Resolution", 1, 180, true),
              "NR's answer reached the upscaler at the Vulkan seam");
    else
        Check(Play(120) < 5000, "frames without NR");
    double longest = Play(90);
    Check(longest < 1500, ("no frame held the game (longest " + std::to_string(int(longest)) + " ms)").c_str());
    checkpoints.push_back(Sample());

    // 2: a quality change, the feature made again at another render size. mochizuki builds a network for the new size
    // and frames pass through meanwhile; danielblnc and lmxxf keep the larger frame's model and go on answering.
    const std::string beforeChange = ReadLog();
    const size_t answered = Count(beforeChange, "NR answers again");
    const size_t passed = Count(beforeChange, "frames pass through");
    Check(CreateFeature(960, 540), "feature made again at 960x540 (quality change)");
    Play(60);
    if (nrOn && bridged)
        PlayFor(45);
    else if (nrOn && Count(ReadLog(), "frames pass through") > passed)
        Check(RunUntil("NR answers again", answered + 1, 180, false), "NR answers again after the quality change");
    Play(120);
    longest = Play(60);
    Check(longest < 1500, ("after the quality change, longest frame " + std::to_string(int(longest)) + " ms").c_str());
    checkpoints.push_back(Sample());

    // 3: a menu, no evaluate for a while, then a command buffer recorded twice before it is submitted.
    for (int i = 0; i < 60; ++i)
        Frame(false, false);
    Check(Frame(true, false, true), "a command buffer recorded twice before its submit");
    longest = Play(240);
    Check(longest < 1500, ("after the pause, longest frame " + std::to_string(int(longest)) + " ms").c_str());
    checkpoints.push_back(Sample());

    vkDeviceWaitIdle(game.device);
    std::ofstream file(label + ".bin", std::ios::binary);
    for (const auto& c : checkpoints)
        file.write(reinterpret_cast<const char*>(c.data()), std::streamsize(c.size() * sizeof(float)));

    const std::string log = ReadLog();
    if (nrOn && bridged)
    {
        Check(log.find("DLSS-NR Vulkan seam: ready on D3D12 device") == std::string::npos,
              "the Vulkan seam stayed out of the w/Dx12 upscaler (no NR twice)");
        Check(log.find("DLSS-NR: the Vulkan bridge reached the hand-off") != std::string::npos,
              "the w/Dx12 upscaler reached the D3D12 seam");
        Check(log.find("NrBackend=daniel runs") != std::string::npos ||
                  log.find("VkwDx12 same-frame NR: submission hook ready, lists proxied") != std::string::npos,
              "lmxxf and mochizuki get proxied lists in the w/Dx12 upscaler");
        Check(log.find("Aborting Vulkan w/Dx12 submit transaction: unsupported") == std::string::npos,
              "every submit was split");
    }
    else if (nrOn)
    {
        Check(log.find("DLSS-NR Vulkan seam: ready on D3D12 device") != std::string::npos, "seam ready on D3D12");
        Check(log.find("DLSS-NR Vulkan unavailable") == std::string::npos,
              "NVIDIA's Vulkan model was not tried on AMD");
        Check(log.find("NR is off until the game restarts") == std::string::npos, "the seam never stopped");
        Check(log.find("Aborting Vulkan w/Dx12 submit transaction: command buffer recorded again") != std::string::npos,
              "the twice-recorded command buffer's transaction was released");
        Check(log.find("Aborting Vulkan w/Dx12 submit transaction: unsupported") == std::string::npos,
              "every submit was split");
    }
    // VulkanUpscaler=dlss_12 or dlss with danielblnc 0.8.0: DLSS-SR is served by the NeuralHub host on D3D12. Native
    // Vulkan DLSS is routed to w/Dx12, the host is armed for it, and its jobs never wait on a submit that comes later.
    char upscalerName[32] {};
    GetEnvironmentVariableA("UPSCALER", upscalerName, sizeof(upscalerName));
    if (std::string(upscalerName).rfind("dlss", 0) == 0)
    {
        std::ifstream hostFile("dlssnr_amd_host/dlssnr_on_amd.log", std::ios::binary);
        const std::string host(std::istreambuf_iterator<char>(hostFile), {});
        if (std::string(upscalerName) == "dlss")
            Check(log.find("DLSS on Vulkan runs w/Dx12") != std::string::npos, "native Vulkan DLSS went to w/Dx12");
        Check(log.find("_CreateFeature result: NVSDK_NGX_Result_Success") != std::string::npos &&
                  log.find("Falling back") == std::string::npos && log.find("init failed") == std::string::npos,
              "DLSS w/Dx12 created through the NeuralHub host, no fallback");
        Check(log.find("VkwDx12: D3D12 work queued after the game's submit") != std::string::npos ||
                  log.find("lists proxied") != std::string::npos,
              "the w/Dx12 D3D12 work waits for the game's submit");
        Check(host.find("evaluates now done by DLSS-SR") != std::string::npos, "the host ran DLSS-SR");
        Check(host.find("host watchdog: job") == std::string::npos && host.find("capture never landed") == std::string::npos,
              ("no host job outlived its budget (" + std::to_string(Count(host, "host watchdog: job")) + " fired)").c_str());
    }

    // The game's copy after its evaluate saw an upscaled frame (it ran after the upscaler, not before).
    bool lit = true;
    for (const auto& c : checkpoints)
    {
        double mean = 0;
        for (float v : c)
            mean += v;
        mean /= double(c.size());
        lit = lit && mean > 0.2 && mean < 2.0;
    }
    Check(lit, "the game's copy after its evaluate read the upscaled frame at every checkpoint");
    Print("%d failed", failures);
    return failures;
}

std::vector<float> Load(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes { std::istreambuf_iterator<char>(file), {} };
    std::vector<float> out(bytes.size() / sizeof(float));
    std::memcpy(out.data(), bytes.data(), out.size() * sizeof(float));
    return out;
}

int Compare(const std::string& on, const std::string& off)
{
    const auto a = Load(on), b = Load(off);
    const size_t per = size_t(kSampleW) * kSampleH * 3;
    if (a.size() != b.size() || a.empty() || a.size() % per)
    {
        Print("FAIL compare: %zu and %zu values", a.size(), b.size());
        return 1;
    }
    const char* names[] { "first frames", "after the quality change", "after the pause" };
    for (size_t k = 0; k < a.size() / per; ++k)
    {
        double diff = 0;
        for (size_t i = 0; i < per; ++i)
            diff += std::abs(a[k * per + i] - b[k * per + i]);
        diff /= double(per);
        Check(diff > 0.002, ("NR changed the upscaled frame " + std::string(names[k % 3]) + " (mean difference " +
                             std::to_string(diff) + ")")
                                .c_str());
    }
    Print("%d failed", failures);
    return failures;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "run";
    if (mode == "compare" && argc > 3)
        return Compare(argv[2], argv[3]);
    bridged = argc > 3 && std::string(argv[3]) == "bridged";
    return Run(argc > 2 ? argv[2] : "frames");
}
