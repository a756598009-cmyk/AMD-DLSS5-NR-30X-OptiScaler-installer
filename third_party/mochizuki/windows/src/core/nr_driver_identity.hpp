#pragma once
// A physical device as AMD's Windows driver itself reports it. A hook in front of the Vulkan loader can rewrite what
// vkGetPhysicalDeviceProperties2 answers: OptiScaler's GPU spoofing shows the AMD card as an NVIDIA one (vendor and
// device IDs, name, driver version, driver ID), so a check of those turns the ACO network off on the very driver it
// was made for. The driver's module (amdvlk64.dll, in the process once the loader has loaded it for an instance) is
// asked directly through an instance of its own, and its physical device with the same LUID is the answer. A machine
// with an AMD integrated GPU has one amdvlk64.dll per driver package (the same name, different folders), each listing
// only its own GPU: every one in the process is asked. False when none lists the device: the caller keeps what the
// loader said.
#include <vulkan/vulkan.h>
#include <cstring>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace nr::driver {

#ifdef _WIN32
// One AMD driver module, through an instance of its own: its physical device with this LUID, if it lists one.
inline bool ask_icd(HMODULE icd, const uint8_t* luid, VkPhysicalDeviceProperties* props,
                    VkPhysicalDeviceDriverProperties* drv) {
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(icd, "vk_icdGetInstanceProcAddr")));
    auto create = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance")) : nullptr;
    if (!create) return false;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "DLSSNR-AMD driver check";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create(&ci, nullptr, &instance) != VK_SUCCESS || !instance) return false;
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto get = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
    bool found = false;
    uint32_t n = 0;
    if (enumerate && get && enumerate(instance, &n, nullptr) == VK_SUCCESS && n) {
        std::vector<VkPhysicalDevice> devices(n);
        if (enumerate(instance, &n, devices.data()) == VK_SUCCESS)
            for (uint32_t i = 0; i < n && !found; ++i) {
                VkPhysicalDeviceDriverProperties d{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
                VkPhysicalDeviceIDProperties di{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &d};
                VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &di};
                get(devices[i], &p);
                if (di.deviceLUIDValid && std::memcmp(di.deviceLUID, luid, VK_LUID_SIZE) == 0) {
                    *props = p.properties;
                    void* next = drv->pNext;
                    *drv = d;
                    drv->pNext = next;
                    found = true;
                }
            }
    }
    if (destroy) destroy(instance, nullptr);
    return found;
}
#endif

// `props` and `drv` (its pNext kept) filled from the driver; `drv` need not be chained to anything.
inline bool amd_reported(VkPhysicalDevice physical, VkPhysicalDeviceProperties* props,
                         VkPhysicalDeviceDriverProperties* drv) {
#ifndef _WIN32
    (void)physical; (void)props; (void)drv;
    return false;
#else
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 asked{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(physical, &asked);
    if (!id.deviceLUIDValid) return false;
    // every module of that name in the process
    std::vector<HMODULE> icds;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W m{};
        m.dwSize = sizeof m;
        for (BOOL more = Module32FirstW(snap, &m); more; more = Module32NextW(snap, &m))
            if (_wcsicmp(m.szModule, L"amdvlk64.dll") == 0) icds.push_back(m.hModule);
        CloseHandle(snap);
    }
    for (HMODULE icd : icds)
        if (ask_icd(icd, id.deviceLUID, props, drv)) return true;
    return false;
#endif
}

}  // namespace nr::driver
