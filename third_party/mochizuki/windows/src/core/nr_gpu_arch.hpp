#pragma once
// The GPU's architecture, which decides the network NR runs on it: RDNA4 (gfx12) the network as written, FP8 through
// the FP8 matrix instructions; RDNA3 and RDNA3.5 (gfx11) the RDNA3 network, whose e4m3 values travel as FP16 through
// the FP16 matrix instructions, as ACO's machine code only (ACO Mode); anything else is not supported. One more
// architecture is one more value here and its own network beside the others.
//
// Asked of AMD's driver itself, not of the Vulkan loader (OptiScaler's GPU spoofing rewrites what the loader reports):
// its compiler makes a pipeline binary of a trivial shader on a device of its own, and the binary's ELF header names
// the machine the code is for (EF_AMDGPU_MACH). Where that cannot be done (no AMD driver module in the process, a
// driver without pipeline binaries), the GPU's PCI device ID - the driver's own where it can be had - by family.
#include "nr_driver_identity.hpp"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace nr::arch {

enum class GpuArch { unknown, rdna3, rdna4 };

inline const char* name(GpuArch a) {
    return a == GpuArch::rdna4 ? "RDNA4" : a == GpuArch::rdna3 ? "RDNA3" : "unknown";
}
// The folder of an architecture's ACO network inside dlssnr-amd\aco (RDNA4's is the bundle itself).
inline const char* aco_folder(GpuArch a) { return a == GpuArch::rdna3 ? "rdna3" : ""; }

// EF_AMDGPU_MACH of an AMDGPU ELF.
inline GpuArch from_machine(uint32_t mach) {
    switch (mach & 0xffu) {
    case 0x48: case 0x4e:   // gfx1200 (RX 9060), gfx1201 (RX 9070, Radeon AI PRO R9700)
        return GpuArch::rdna4;
    case 0x41: case 0x46: case 0x47: case 0x44:   // gfx1100 (RX 7900), gfx1101 (RX 7800 / 7700), gfx1102 (RX 7600),
                                                  // gfx1103 (Radeon 780M / 760M)
    case 0x43: case 0x4a: case 0x55: case 0x58:   // gfx1150 (890M), gfx1151 (8060S), gfx1152, gfx1153
        return GpuArch::rdna3;
    default:
        return GpuArch::unknown;
    }
}

// A PCI device ID by family: what is known to be RDNA4 or RDNA3; anything else unknown.
inline GpuArch from_device_id(uint32_t vendor, uint32_t device) {
    if (vendor != 0x1002) return GpuArch::unknown;
    if ((device & 0xff00u) == 0x7500u) return GpuArch::rdna4;   // Navi 48 / 44
    if ((device & 0xff00u) == 0x7400u) return GpuArch::rdna3;   // Navi 31 / 32 / 33
    switch (device) {
    case 0x15bf: case 0x15c8: case 0x1900: case 0x1901:   // Phoenix / Hawk Point (780M / 760M / 740M)
    case 0x150e: case 0x1586: case 0x1114:                 // Strix Point (890M), Strix Halo (8060S), Krackan Point
        return GpuArch::rdna3;
    default:
        return GpuArch::unknown;
    }
}

namespace detail {
// layout(local_size_x = 64) in; void main() {}
inline const uint32_t kProbe[] = {
    0x07230203,0x00010600,0x0008000b,0x0000000b,0x00000000,0x00020011,0x00000001,0x0006000b,0x00000001,0x4c534c47,
    0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,0x0005000f,0x00000005,0x00000004,0x6e69616d,
    0x00000000,0x00060010,0x00000004,0x00000011,0x00000040,0x00000001,0x00000001,0x00030003,0x00000002,0x000001c2,
    0x00040005,0x00000004,0x6e69616d,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00040015,
    0x00000006,0x00000020,0x00000000,0x00040017,0x00000007,0x00000006,0x00000003,0x0004002b,0x00000006,0x00000008,
    0x00000040,0x0004002b,0x00000006,0x00000009,0x00000001,0x0006002c,0x00000007,0x0000000a,0x00000008,0x00000009,
    0x00000009,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x000100fd,0x00010038,
};

#ifdef _WIN32
// The machine of the code AMD's driver module `icd` compiles for the GPU with this LUID: a pipeline binary of kProbe,
// on an instance and device of its own (so no hook in front of the loader sees them). 0 when it cannot be made.
inline uint32_t compiled_machine(HMODULE icd, const uint8_t* luid) {
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(icd, "vk_icdGetInstanceProcAddr")));
    auto create_instance = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance")) : nullptr;
    if (!create_instance) return 0;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "DLSSNR-AMD architecture check";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create_instance(&ici, nullptr, &instance) != VK_SUCCESS || !instance) return 0;
#define NR_ARCH_I(f) auto p_##f = reinterpret_cast<PFN_##f>(gipa(instance, #f))
    NR_ARCH_I(vkDestroyInstance);
    NR_ARCH_I(vkEnumeratePhysicalDevices);
    NR_ARCH_I(vkGetPhysicalDeviceProperties2);
    NR_ARCH_I(vkEnumerateDeviceExtensionProperties);
    NR_ARCH_I(vkCreateDevice);
    NR_ARCH_I(vkGetDeviceProcAddr);
#undef NR_ARCH_I
    uint32_t machine = 0;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t n = 0;
    if (p_vkEnumeratePhysicalDevices && p_vkGetPhysicalDeviceProperties2 &&
        p_vkEnumeratePhysicalDevices(instance, &n, nullptr) == VK_SUCCESS && n) {
        std::vector<VkPhysicalDevice> devices(n);
        if (p_vkEnumeratePhysicalDevices(instance, &n, devices.data()) == VK_SUCCESS)
            for (VkPhysicalDevice d : devices) {
                VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
                p_vkGetPhysicalDeviceProperties2(d, &p);
                if (id.deviceLUIDValid && std::memcmp(id.deviceLUID, luid, VK_LUID_SIZE) == 0) physical = d;
            }
    }
    bool offered = false;
    if (physical && p_vkEnumerateDeviceExtensionProperties && p_vkCreateDevice && p_vkGetDeviceProcAddr) {
        uint32_t e = 0;
        p_vkEnumerateDeviceExtensionProperties(physical, nullptr, &e, nullptr);
        std::vector<VkExtensionProperties> have(e);
        p_vkEnumerateDeviceExtensionProperties(physical, nullptr, &e, have.data());
        int both = 0;
        for (const auto& x : have)
            both += !std::strcmp(x.extensionName, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) ||
                    !std::strcmp(x.extensionName, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        offered = both == 2;
    }
    VkDevice device = VK_NULL_HANDLE;
    if (offered) {
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = 0; q.queueCount = 1; q.pQueuePriorities = &priority;
        VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
        m5.maintenance5 = VK_TRUE;
        VkPhysicalDevicePipelineBinaryFeaturesKHR pb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, &m5};
        pb.pipelineBinaries = VK_TRUE;
        const char* exts[] = {VK_KHR_PIPELINE_BINARY_EXTENSION_NAME, VK_KHR_MAINTENANCE_5_EXTENSION_NAME};
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &pb};
        di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &q;
        di.enabledExtensionCount = 2; di.ppEnabledExtensionNames = exts;
        if (p_vkCreateDevice(physical, &di, nullptr, &device) != VK_SUCCESS) device = VK_NULL_HANDLE;
    }
    if (device) {
#define NR_ARCH_D(f) auto p_##f = reinterpret_cast<PFN_##f>(p_vkGetDeviceProcAddr(device, #f))
        NR_ARCH_D(vkDestroyDevice);
        NR_ARCH_D(vkCreateShaderModule);
        NR_ARCH_D(vkDestroyShaderModule);
        NR_ARCH_D(vkCreatePipelineLayout);
        NR_ARCH_D(vkDestroyPipelineLayout);
        NR_ARCH_D(vkCreateComputePipelines);
        NR_ARCH_D(vkDestroyPipeline);
        NR_ARCH_D(vkCreatePipelineBinariesKHR);
        NR_ARCH_D(vkDestroyPipelineBinaryKHR);
        NR_ARCH_D(vkGetPipelineBinaryDataKHR);
        NR_ARCH_D(vkReleaseCapturedPipelineDataKHR);
#undef NR_ARCH_D
        if (p_vkCreateShaderModule && p_vkCreatePipelineLayout && p_vkCreateComputePipelines && p_vkCreatePipelineBinariesKHR &&
            p_vkGetPipelineBinaryDataKHR) {
            VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            sm.codeSize = sizeof kProbe; sm.pCode = kProbe;
            VkShaderModule module = VK_NULL_HANDLE;
            VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            VkPipelineLayout layout = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (p_vkCreateShaderModule(device, &sm, nullptr, &module) == VK_SUCCESS &&
                p_vkCreatePipelineLayout(device, &li, nullptr, &layout) == VK_SUCCESS) {
                VkPipelineCreateFlags2CreateInfoKHR flags{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR};
                flags.flags = VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;
                VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, &flags};
                ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = module; ci.stage.pName = "main";
                ci.layout = layout;
                if (p_vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS)
                    pipeline = VK_NULL_HANDLE;
            }
            if (pipeline) {
                VkPipelineBinaryCreateInfoKHR bi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};
                bi.pipeline = pipeline;
                VkPipelineBinaryHandlesInfoKHR hi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
                std::vector<VkPipelineBinaryKHR> binaries;
                if (p_vkCreatePipelineBinariesKHR(device, &bi, nullptr, &hi) == VK_SUCCESS && hi.pipelineBinaryCount) {
                    binaries.resize(hi.pipelineBinaryCount);
                    hi.pPipelineBinaries = binaries.data();
                    if (p_vkCreatePipelineBinariesKHR(device, &bi, nullptr, &hi) != VK_SUCCESS) binaries.clear();
                }
                for (VkPipelineBinaryKHR b : binaries) {
                    VkPipelineBinaryDataInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR};
                    info.pipelineBinary = b;
                    VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
                    size_t bytes = 0;
                    if (!machine && p_vkGetPipelineBinaryDataKHR(device, &info, &key, &bytes, nullptr) == VK_SUCCESS &&
                        bytes >= 64 && bytes <= (64u << 20)) {
                        std::vector<uint8_t> data(bytes);
                        if (p_vkGetPipelineBinaryDataKHR(device, &info, &key, &bytes, data.data()) == VK_SUCCESS &&
                            bytes >= 64 && !std::memcmp(data.data(), "\x7f" "ELF", 4) && data[18] == 224) {
                            uint32_t flags = 0;
                            std::memcpy(&flags, data.data() + 48, 4);
                            machine = flags;
                        }
                    }
                    if (p_vkDestroyPipelineBinaryKHR) p_vkDestroyPipelineBinaryKHR(device, b, nullptr);
                }
                if (p_vkReleaseCapturedPipelineDataKHR) {
                    VkReleaseCapturedPipelineDataInfoKHR r{VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR};
                    r.pipeline = pipeline;
                    p_vkReleaseCapturedPipelineDataKHR(device, &r, nullptr);
                }
                if (p_vkDestroyPipeline) p_vkDestroyPipeline(device, pipeline, nullptr);
            }
            if (layout && p_vkDestroyPipelineLayout) p_vkDestroyPipelineLayout(device, layout, nullptr);
            if (module && p_vkDestroyShaderModule) p_vkDestroyShaderModule(device, module, nullptr);
        }
        if (p_vkDestroyDevice) p_vkDestroyDevice(device, nullptr);
    }
    if (p_vkDestroyInstance) p_vkDestroyInstance(instance, nullptr);
    return machine;
}
#endif
}  // namespace detail

// The architecture of `physical` (the loader's), and how it was found (for the log). Asked once per GPU (LUID).
inline GpuArch of(VkPhysicalDevice physical, std::string* how = nullptr) {
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 asked{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(physical, &asked);
    struct Known { uint8_t luid[VK_LUID_SIZE]; GpuArch arch; std::string how; };
    static std::mutex m;
    static std::vector<Known> known;
    std::lock_guard<std::mutex> guard(m);
    if (id.deviceLUIDValid)
        for (const Known& k : known)
            if (!std::memcmp(k.luid, id.deviceLUID, VK_LUID_SIZE)) {
                if (how) *how = k.how;
                return k.arch;
            }
    GpuArch arch = GpuArch::unknown;
    std::string said;
    char text[160];
#ifdef _WIN32
    if (id.deviceLUIDValid) {
        // every AMD driver module in the process (one per driver package: an integrated GPU may have its own)
        std::vector<HMODULE> icds;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me{};
            me.dwSize = sizeof me;
            for (BOOL more = Module32FirstW(snap, &me); more; more = Module32NextW(snap, &me))
                if (_wcsicmp(me.szModule, L"amdvlk64.dll") == 0) icds.push_back(me.hModule);
            CloseHandle(snap);
        }
        for (HMODULE icd : icds) {
            const uint32_t mach = detail::compiled_machine(icd, id.deviceLUID);
            if (!mach) continue;
            arch = from_machine(mach);
            std::snprintf(text, sizeof text, "the driver's compiler makes machine 0x%02x code", unsigned(mach & 0xffu));
            said = text;
            break;
        }
    }
#endif
    if (said.empty()) {
        VkPhysicalDeviceProperties props = asked.properties;
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        const bool driver = nr::driver::amd_reported(physical, &props, &drv);
        arch = from_device_id(props.vendorID, props.deviceID);
        std::snprintf(text, sizeof text, "PCI device %04x:%04x (%s)", unsigned(props.vendorID), unsigned(props.deviceID),
                      driver ? "as AMD's driver reports it" : "as the Vulkan loader reports it");
        said = text;
    }
    if (id.deviceLUIDValid) {
        Known k{};
        std::memcpy(k.luid, id.deviceLUID, VK_LUID_SIZE);
        k.arch = arch;
        k.how = said;
        known.push_back(k);
    }
    if (how) *how = said;
    return arch;
}

}  // namespace nr::arch
