#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include <cassert>
#include <vector>

// Run next to OptiScaler.dll by tools/test-final-image-gate.ps1, which writes its INI. Exercises the native Vulkan
// device, swapchain and present hooks; tests/vulkan_present_smoke.cpp is the full Vulkan regression.
int main()
{
    assert(LoadLibraryW(L"vulkan-1.dll"));
    assert(LoadLibraryW(L"OptiScaler.dll"));
    Sleep(1000);

    const auto module = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = module;
    windowClass.lpszClassName = L"OptiVulkanPresentSmoke";
    assert(RegisterClassW(&windowClass));
    HWND window = CreateWindowW(windowClass.lpszClassName, L"Opti Vulkan smoke", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1920, 1080, nullptr, nullptr, module, nullptr);
    assert(window);
    ShowWindow(window, SW_SHOW);

    const char* instanceExtensions[] { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    VkInstance instance = VK_NULL_HANDLE;
    assert(vkCreateInstance(&instanceInfo, nullptr, &instance) == VK_SUCCESS);
    VkWin32SurfaceCreateInfoKHR surfaceInfo { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    surfaceInfo.hinstance = module;
    surfaceInfo.hwnd = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    assert(vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface) == VK_SUCCESS);

    uint32_t physicalCount = 0;
    assert(vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr) == VK_SUCCESS && physicalCount);
    std::vector<VkPhysicalDevice> physicals(physicalCount);
    assert(vkEnumeratePhysicalDevices(instance, &physicalCount, physicals.data()) == VK_SUCCESS);
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    for (auto candidate : physicals)
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, queues.data());
        for (uint32_t i = 0; i < count; ++i)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present);
            if (present && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            { physical = candidate; family = i; break; }
        }
        if (physical) break;
    }
    assert(physical);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* deviceExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = &deviceExtension;
    VkDevice device = VK_NULL_HANDLE;
    assert(vkCreateDevice(physical, &deviceInfo, nullptr, &device) == VK_SUCCESS);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    VkSurfaceCapabilitiesKHR caps {};
    assert(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps) == VK_SUCCESS);
    uint32_t formatCount = 0;
    assert(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr) == VK_SUCCESS && formatCount);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    assert(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats.data()) == VK_SUCCESS);
    VkSurfaceFormatKHR format = formats[0];
    for (auto option : formats)
        if (option.format == VK_FORMAT_B8G8R8A8_UNORM) { format = option; break; }
    VkSwapchainCreateInfoKHR chainInfo { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    chainInfo.surface = surface;
    chainInfo.minImageCount = caps.minImageCount;
    chainInfo.imageFormat = format.format;
    chainInfo.imageColorSpace = format.colorSpace;
    chainInfo.imageExtent = caps.currentExtent;
    chainInfo.imageArrayLayers = 1;
    chainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    chainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    chainInfo.preTransform = caps.currentTransform;
    chainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    chainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    chainInfo.clipped = VK_TRUE;
    VkSwapchainKHR chain = VK_NULL_HANDLE;
    assert(vkCreateSwapchainKHR(device, &chainInfo, nullptr, &chain) == VK_SUCCESS);
    uint32_t imageCount = 0;
    assert(vkGetSwapchainImagesKHR(device, chain, &imageCount, nullptr) == VK_SUCCESS && imageCount);
    std::vector<VkImage> images(imageCount);
    assert(vkGetSwapchainImagesKHR(device, chain, &imageCount, images.data()) == VK_SUCCESS);

    for (int frame = 0; frame < 125; ++frame)
    {
    VkSemaphoreCreateInfo semaphoreInfo { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    assert(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquired) == VK_SUCCESS);
    assert(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &rendered) == VK_SUCCESS);
    uint32_t imageIndex = 0;
    assert(vkAcquireNextImageKHR(device, chain, UINT64_MAX, acquired, VK_NULL_HANDLE, &imageIndex) == VK_SUCCESS);
    VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    assert(vkCreateCommandPool(device, &poolInfo, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo allocate { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocate.commandPool = pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    assert(vkAllocateCommandBuffers(device, &allocate, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images[imageIndex];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &rendered;
    assert(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    VkPresentInfoKHR present { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.waitSemaphoreCount = 1; present.pWaitSemaphores = &rendered;
    present.swapchainCount = 1; present.pSwapchains = &chain; present.pImageIndices = &imageIndex;
    auto result = vkQueuePresentKHR(queue, &present);
    assert(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR);
    assert(vkDeviceWaitIdle(device) == VK_SUCCESS);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroySemaphore(device, acquired, nullptr);
    vkDestroySemaphore(device, rendered, nullptr);
    }
    vkDestroySwapchainKHR(device, chain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    DestroyWindow(window);
}
