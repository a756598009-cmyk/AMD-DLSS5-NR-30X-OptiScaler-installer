// A small Vulkan compute context, shared by every kernel bench and layer test.
//
// This is the part of the runtime that is not a kernel: device selection with
// the feature set decision 3 and decision 8 require, arena buffers, a compute
// pipeline built from an embedded SPIR-V blob with wave32 pinned, and a timed
// dispatch. It deliberately has no fallback: a missing feature throws with the
// name of the feature, per the fail-loudly rule.
//
// What it is not: the graph planner, the descriptor model of a shipping frame,
// or the OptiScaler adapter. Buffers here are host-visible so a test can read
// them back without staging; the adapter that owns the game's VkDevice will
// allocate the same arenas out of device-local memory.
#pragma once
#include "nr_path_u8.hpp"
#include "nr_gpu_arch.hpp"
#include "nr_gpu_completion.hpp"
#include "nr_log.hpp"   // (nrvk::blit's lines; the int4 path's too)
#if NR_INT4
#include "nr_build_cfg.hpp"
#endif
#include <vulkan/vulkan.h>
#include "nr_pipeline_binary.hpp"
#include "nr_arch.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <set>
#include <vector>
#ifdef NR_PIPELINE_STATS
#include <chrono>
#endif

namespace nrvk {

// The host ran out of memory or address space (VK_ERROR_OUT_OF_HOST_MEMORY,
// VK_ERROR_MEMORY_MAP_FAILED). A std::bad_alloc, so callers that retry a build
// after a host-memory failure (a 32-bit game while it is loading) treat it as
// one; everything else that catches std::exception still does.
struct HostMemoryError : std::bad_alloc {
    std::string text;
    explicit HostMemoryError(std::string t) : text(std::move(t)) {}
    const char* what() const noexcept override { return text.c_str(); }
};

inline void check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return;
    std::string text = std::string(what) + ": VkResult=" + std::to_string(r);
    if (r == VK_ERROR_OUT_OF_HOST_MEMORY || r == VK_ERROR_MEMORY_MAP_FAILED) throw HostMemoryError(std::move(text));
    throw nr::binary::VkFailure(text, r);   // with its result: a build tells video memory running short from the rest
}
#define NRVK_CHECK(expr) ::nrvk::check((expr), #expr)
// The network the device's architecture runs (nr_gpu_arch.hpp): RDNA4's e4m3 operands go to the FP8 matrix
// instructions, RDNA3's (gfx11 has none) travel as FP16 through the FP16 ones with FP32 accumulation
// (windows/shaders/rdna3/include/coopmm.glsl). Context::rdna3 says which; the host lowering (weight packing, the FP16
// twins) follows it at run time, so one program serves both.

// Every buffer Context::buffer made and its size, for MZ_KCAP (diagnostic, nr_runtime.cpp).
inline std::map<VkBuffer, VkDeviceSize>& buffer_sizes() { static std::map<VkBuffer, VkDeviceSize> m; return m; }
inline std::mutex& buffer_sizes_lock() { static std::mutex m; return m; }
struct Buffer {
    VkBuffer handle{};
    VkDeviceMemory memory{};
    void* mapped{};
    VkDeviceSize bytes{};
    VkDeviceAddress address{};   // buffer(.., device_address = true) only
    template <class T> T* as() { return static_cast<T*>(mapped); }
};

// Device memory several runtimes' images take turns in (the resize path): two regions; a runtime built at a new frame
// size takes its images from the region the size before it did not use, so the memory of the sizes a game's dynamic
// resolution leaves behind is reused after its recordings finish and are reset. A region's previous runtime is
// stale from then on (`gen`) and never recorded again; its images alias the new ones, which the GPU sees in order
// after a full barrier (the new runtime's first transitions are from UNDEFINED). An image that does not fit takes
// memory of its own, and `need` says how big the regions should have been.
struct ImageHeap {
    VkDevice device{};
    VkDeviceMemory memory{};
    uint32_t type{};
    VkDeviceSize region_cap{};
    VkDeviceSize cursor[2]{};
    uint64_t gen[2]{};
    nr::gpu::Uses uses[2];   // executable or pending recordings also retain aliased memory regions
    VkDeviceSize need[2]{};   // bytes the region's runtime asked for, fitted or not
    ImageHeap() = default;
    ImageHeap(const ImageHeap&) = delete;
    ImageHeap& operator=(const ImageHeap&) = delete;
    ~ImageHeap() { if (memory) vkFreeMemory(device, memory, nullptr); }
};

struct SharedPipeline;   // below; Context::pin

struct Context {
    // The network (nr_arch.hpp). RDNA3 (gfx11) and RDNA2 (gfx103x) have no FP8: their networks
    // (linux/shaders/rdna3, rdna2) hold the e4m3 values as FP16 operands, and RDNA2 has no
    // cooperative matrices at all. Set before `adopt` (RuntimeConfig::arch; upstream picks it from the
    // device's architecture, nr_gpu_arch.hpp).
    nr::Arch arch = nr::Arch::rdna4;
    bool fp16() const { return nr::fp16_network(arch); }
    const char* target_name() const {
        return arch == nr::Arch::rdna2 ? "RDNA2 (no matrices)"
             : arch == nr::Arch::rdna3 ? "RDNA3 (FP16 matrices)" : "RDNA4 (FP8 matrices)";
    }
    bool target_float8() const { return !fp16(); }
    VkComponentTypeKHR target_matrix_ab() const {
        return fp16() ? VK_COMPONENT_TYPE_FLOAT16_KHR : VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT;
    }

    // The FP16 networks: an arena's FP16 twin, which holds its e4m3 values as FP16 elements at the same
    // element index. Empty on RDNA4.
    std::map<VkBuffer, VkBuffer> fp16_twin;
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    VkPhysicalDeviceMemoryProperties mem{};
    float timestamp_period{};
    // bufferDeviceAddress is enabled on the device: kernels may read the activation arena through
    // a buffer_reference (vit_attn.comp NR_VBDA - LLPC's transposing global load). Set by create();
    // an adopted device says so through HostDevice::buffer_device_address.
    bool buffer_device_address = false;
    std::string gpu_name, driver_name;
    uint32_t gpu_vendor_id = 0, gpu_device_id = 0;   // telemetry and logs look the device up by ID
    bool device_local_host_visible = false;
    // A queue borrowed from a host API (vkd3d-proton, DXVK) is externally
    // synchronised by that API's own lock. When set, the lock is taken around
    // each submit and nothing else, so a build that spends seconds compiling
    // pipelines does not hold the game's queue for those seconds.
    std::function<void()> queue_lock, queue_unlock;
    void submit(const VkSubmitInfo& si, VkFence fence) {
        if (queue_lock) queue_lock();
        const VkResult r = vkQueueSubmit(queue, 1, &si, fence);
        if (queue_unlock) queue_unlock();
        NRVK_CHECK(r);
    }
#ifdef NR_PIPELINE_STATS
    // Evaluation builds only: compile time and the driver's
    // own register/LDS statistics for every pipeline, printed as it is built.
    bool pipeline_stats = false;
#endif

    // Which device the network runs on (the standalone tools'): NR_GPU picks by enumeration index; without it the
    // one AMD GPU whose architecture has a network (nr_gpu_arch.hpp) - more than one is an error that names them.
    void create() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "DLSS-NR-on-AMD";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ii.pApplicationInfo = &app;
        NRVK_CHECK(vkCreateInstance(&ii, nullptr, &instance));

        uint32_t count = 0;
        NRVK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        NRVK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        const char* forced = getenv("NR_GPU");
        std::string seen;
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(devices[i], &p);
            const nr::arch::GpuArch a = forced || p.vendorID != 0x1002 ? nr::arch::GpuArch::unknown : nr::arch::of(devices[i]);
            const bool want = forced ? i == uint32_t(atoi(forced)) : a != nr::arch::GpuArch::unknown;
            if (!want) continue;
            if (physical)
                throw std::runtime_error("more than one candidate GPU: " + seen + " and " + p.deviceName + "; set NR_GPU");
            physical = devices[i];
            gpu_name = p.deviceName;
            gpu_vendor_id = p.vendorID; gpu_device_id = p.deviceID;
            seen = p.deviceName;
        }
        if (!physical) throw std::runtime_error("no RDNA4 or RDNA3 GPU found; set NR_GPU");
        arch = nr::arch::of(physical) == nr::arch::GpuArch::rdna3 ? nr::Arch::rdna3 : nr::Arch::rdna4;

        require_matrix_config();

        VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
        VkPhysicalDeviceShaderFloat8FeaturesEXT fp8{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &coop; coop.pNext = &fp8; fp8.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        const bool no_fp8 = !target_float8();
        struct { bool have; const char* name; } required[] = {
            {bool(coop.cooperativeMatrix),            "cooperativeMatrix"},
            {no_fp8 || bool(fp8.shaderFloat8),                  "shaderFloat8"},
            {no_fp8 || bool(fp8.shaderFloat8CooperativeMatrix), "shaderFloat8CooperativeMatrix"},
            {bool(f12.storageBuffer8BitAccess),       "storageBuffer8BitAccess"},
            {bool(f12.shaderFloat16),                 "shaderFloat16"},
            {bool(f12.shaderInt8),                    "shaderInt8"},
            {bool(f12.vulkanMemoryModel),             "vulkanMemoryModel"},
            {bool(f13.subgroupSizeControl),           "subgroupSizeControl"},
            {bool(f11.storageBuffer16BitAccess),      "storageBuffer16BitAccess"},
        };
        for (const auto& r : required)
            if (!r.have) throw std::runtime_error(std::string("device feature missing: ") + r.name);

        // Enable exactly what is used, not everything the query returned.
        coop = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
        coop.cooperativeMatrix = VK_TRUE;
        fp8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
        // The FP8 features are the rdna4-fp8 target's; rdna3-fp16 leaves the struct zeroed in
        // the chain — explicitly disabled, not silently absent.
        if (target_float8()) fp8.shaderFloat8 = fp8.shaderFloat8CooperativeMatrix = VK_TRUE;
        f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        f11.storageBuffer16BitAccess = VK_TRUE;
        f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        f12.storageBuffer8BitAccess = VK_TRUE; f12.shaderFloat16 = VK_TRUE;
        f12.shaderInt8 = VK_TRUE; f12.vulkanMemoryModel = VK_TRUE;
        {
            VkPhysicalDeviceVulkan12Features q12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q.pNext = &q12;
            vkGetPhysicalDeviceFeatures2(physical, &q);
            // NR_NO_BDA=1: as if the device had none (checks the g_*nobda.spv fallback).
            const char* no_bda = std::getenv("NR_NO_BDA");
            const bool bda = q12.bufferDeviceAddress && !(no_bda && *no_bda == '1');
            f12.bufferDeviceAddress = bda;
            buffer_device_address = bda;
        }
        f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.subgroupSizeControl = VK_TRUE;
        // Core in 1.3 and still an opt-in feature: without it
        // `vkCmdPipelineBarrier2` is not dispatchable and the graph faults on
        // the first one. It is here for `--storage-barrier`, which says
        // "storage buffer" where the legacy mask can only say "shader read".
        f13.synchronization2 = VK_TRUE;
        coop.pNext = &fp8; fp8.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13;

        VkPhysicalDeviceSubgroupSizeControlProperties sg{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &drv; drv.pNext = &sg;
        vkGetPhysicalDeviceProperties2(physical, &props);
        driver_name = std::string(drv.driverName) + " / " + drv.driverInfo;
        timestamp_period = props.properties.limits.timestampPeriod;
        // Decision 8: coopmat properties are reported per subgroup size and
        // RADV's default compute wave is a heuristic, so 32 is required, not
        // requested. wave64 has different fragment sizes and LDS traffic.
        if (sg.minSubgroupSize > 32 || sg.maxSubgroupSize < 32 ||
            !(sg.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT))
            throw std::runtime_error("32-lane compute subgroups unavailable");

        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, queues.data());
        family = qcount;
        for (uint32_t i = 0; i < qcount; ++i)
            if (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
        if (family == qcount) throw std::runtime_error("no compute queue family");

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
        // Shared-memory blocks (GL_EXT_shared_memory_block) let a kernel
        // alias two LDS arrays whose lifetimes do not overlap; the C=256 fused
        // upsample body uses it to fit two workgroups a CU. Enabled when the
        // device has it; a build that aliases LDS requires it.
        VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wml{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
        {
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q.pNext = &wml;
            vkGetPhysicalDeviceFeatures2(physical, &q);
        }
        const bool have_wml = wml.workgroupMemoryExplicitLayout && wml.workgroupMemoryExplicitLayout8BitAccess
                              && wml.workgroupMemoryExplicitLayout16BitAccess;
        wml.pNext = nullptr;
        wml.workgroupMemoryExplicitLayoutScalarBlockLayout = VK_FALSE;
        if (have_wml) f13.pNext = &wml;
        std::vector<const char*> ext_list{VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME};
        if (target_float8()) ext_list.push_back(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
        if (have_wml) ext_list.push_back(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.pNext = &coop; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
        // Our pipelines ask for no robust buffer / image access (Kernel::create); the feature that allows it.
        VkPhysicalDevicePipelineRobustnessFeaturesEXT prf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
        {
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q.pNext = &prf;
            vkGetPhysicalDeviceFeatures2(physical, &q);
            uint32_t ne = 0;
            vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, nullptr);
            std::vector<VkExtensionProperties> have(ne);
            vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, have.data());
            bool listed = false;
            for (const auto& e : have) listed |= !std::strcmp(e.extensionName, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
            if (prf.pipelineRobustness && listed) {
                prf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
                prf.pipelineRobustness = VK_TRUE;
                VkBaseOutStructure* tail = reinterpret_cast<VkBaseOutStructure*>(&coop);
                while (tail->pNext) tail = tail->pNext;
                tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&prf);
                ext_list.push_back(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
                pipeline_robustness = true;
            }
        }
#ifdef NR_PIPELINE_STATS
        VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pex{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
        {
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q.pNext = &pex;
            vkGetPhysicalDeviceFeatures2(physical, &q);
        }
        if (pex.pipelineExecutableInfo) {
            pex.pNext = const_cast<void*>(di.pNext); di.pNext = &pex;
            ext_list.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
            di.enabledExtensionCount = uint32_t(ext_list.size()); di.ppEnabledExtensionNames = ext_list.data();
            pipeline_stats = true;
        }
        std::printf("pipeline statistics: %s\n", pipeline_stats ? "on" : "not supported by the driver");
        // In-kernel clock records (NR_PROF builds) read the shader and device clocks.
        VkPhysicalDeviceShaderClockFeaturesKHR clk{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
        {
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q.pNext = &clk;
            vkGetPhysicalDeviceFeatures2(physical, &q);
        }
        if (clk.shaderSubgroupClock && clk.shaderDeviceClock) {
            clk.pNext = const_cast<void*>(di.pNext); di.pNext = &clk;
            ext_list.push_back(VK_KHR_SHADER_CLOCK_EXTENSION_NAME);
            di.enabledExtensionCount = uint32_t(ext_list.size()); di.ppEnabledExtensionNames = ext_list.data();
        }
        std::printf("shader clocks: %s\n", clk.shaderSubgroupClock && clk.shaderDeviceClock ? "on" : "not supported");
#endif
        di.enabledExtensionCount = uint32_t(ext_list.size()); di.ppEnabledExtensionNames = ext_list.data();
        nr::binary::Features binary_features;
        binary_features.prepare(physical, di, ext_list);
        NRVK_CHECK(vkCreateDevice(physical, &di, nullptr, &device));
        if (binary_features.active) nr::binary::mark(device);
        vkGetDeviceQueue(device, family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    }

    // ---- running on somebody else's device --------------------------------
    // An injector does not get to create the device: the game already did, and
    // its swapchain images, its queue and its command buffers all belong to
    // that one. So `adopt` fills in everything `create` would have, from
    // handles the caller supplies, and then **checks rather than requests** -
    // the features this port needs have to have been enabled at the host's
    // `vkCreateDevice`, which is the layer's job to arrange on the way in.
    //
    // `why` comes back naming the first missing thing, so the caller can say
    // "running the fallback because shaderFloat8CooperativeMatrix is off"
    // instead of failing silently. A build that quietly does a lesser thing is
    // the failure mode this whole project is organised against.
    bool adopt(VkInstance inst, VkPhysicalDevice phys, VkDevice dev,
               VkQueue q, uint32_t qfamily, std::string* why = nullptr) {
        instance = inst; physical = phys; device = dev; queue = q; family = qfamily;
        auto no = [&](const char* n) {
            if (why) *why = n;
            return false;
        };
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR c{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
        VkPhysicalDeviceShaderFloat8FeaturesEXT f8{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
        VkPhysicalDeviceVulkan11Features v11{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features v12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features v13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        // The C=256 fused-upsample bodies alias two LDS arrays through shared
        // memory blocks (NR_UPS_ALIAS), which needs this extension.
        VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wml{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
        VkPhysicalDevicePipelineRobustnessFeaturesEXT prf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &c; c.pNext = &f8; f8.pNext = &v11; v11.pNext = &v12; v12.pNext = &v13; v13.pNext = &wml; wml.pNext = &prf;
        vkGetPhysicalDeviceFeatures2(physical, &f2);
        // DXVK and vkd3d-proton create their devices with robustBufferAccess2 and without this feature; RADV
        // compiles a pipeline by its own VkPipelineRobustnessCreateInfo whether or not the device enabled it.
        pipeline_robustness = prf.pipelineRobustness;
        if (arch != nr::Arch::rdna2 && !c.cooperativeMatrix) return no("cooperativeMatrix");
        if (target_float8() && !f8.shaderFloat8)                 return no("shaderFloat8");
        if (target_float8() && !f8.shaderFloat8CooperativeMatrix) return no("shaderFloat8CooperativeMatrix");
        if (!v11.storageBuffer16BitAccess)    return no("storageBuffer16BitAccess");
        if (!v12.storageBuffer8BitAccess)     return no("storageBuffer8BitAccess");
        if (!v12.shaderFloat16)               return no("shaderFloat16");
        if (!v13.subgroupSizeControl)         return no("subgroupSizeControl");
        if (!wml.workgroupMemoryExplicitLayout || !wml.workgroupMemoryExplicitLayout8BitAccess ||
            !wml.workgroupMemoryExplicitLayout16BitAccess)
            return no("workgroupMemoryExplicitLayout (8/16-bit)");

        VkPhysicalDeviceSubgroupSizeControlProperties sg{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        VkPhysicalDeviceDriverProperties drv{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &drv; drv.pNext = &sg;
        vkGetPhysicalDeviceProperties2(physical, &props);
        driver_name = std::string(drv.driverName) + " / " + drv.driverInfo;
        timestamp_period = props.properties.limits.timestampPeriod;
        gpu_vendor_id = props.properties.vendorID; gpu_device_id = props.properties.deviceID;
        // The same requirement `create` makes, and for the same reason: the
        // coopmat properties are reported per subgroup size and every fragment
        // size in this port is a wave32 one.
        if (sg.minSubgroupSize > 32 || sg.maxSubgroupSize < 32 ||
            !(sg.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT))
            return no("32-lane compute subgroups");
        vkGetPhysicalDeviceMemoryProperties(physical, &mem);
        adopted = true;
        return true;
    }
    // `destroy` must not tear down a device it did not create.
    bool adopted = false;
    // The driver has VK_EXT_pipeline_robustness: Kernel::create compiles every pipeline without robust buffer and
    // image access. Every access of ours is in bounds, so the output is the same; on a device with
    // robustBufferAccess2 (DXVK, vkd3d-proton) the robust pipelines are slower.
    bool pipeline_robustness = false;

    // ---- the pipeline cache, on disk ---------------------------------------
    //
    // Every compute pipeline in the network is created from SPIR-V at build
    // time. On this box that is free, because Mesa's own on-disk shader cache is
    // warm; the first time a machine ever runs the network it is not, and the
    // compile is the build. One VkPipelineCache, loaded before the pipelines are
    // created and written back after, moves that cost to the first run only.
    //
    // A cache file from another driver, another GPU or another driver version is
    // rejected by the implementation on the header it carries, so a stale file
    // costs a compile and never a wrong pipeline.
    VkPipelineCache pipeline_cache{};
    std::string pipeline_cache_path;

    void load_pipeline_cache(const std::string& path) {
        if (pipeline_cache || !device) return;
        pipeline_cache_path = path;
        std::vector<char> blob;
        {
            std::ifstream f(nr::u8::path(path), std::ios::binary | std::ios::ate);
            if (f) {
                const std::streamoff n = f.tellg();
                // 32 bytes is the header every implementation writes; anything
                // shorter cannot be a cache and is not offered as one.
                if (n >= 32) {
                    blob.resize(size_t(n));
                    f.seekg(0);
                    f.read(blob.data(), n);
                    if (!f) blob.clear();
                }
            }
        }
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = blob.size();
        ci.pInitialData = blob.empty() ? nullptr : blob.data();
        // Not fatal: a device with no pipeline cache builds every pipeline.
        if (vkCreatePipelineCache(device, &ci, nullptr, &pipeline_cache) != VK_SUCCESS)
            pipeline_cache = VK_NULL_HANDLE;
    }

    // Returns the bytes written, 0 when nothing was.
    size_t save_pipeline_cache() {
        if (!pipeline_cache || pipeline_cache_path.empty()) return 0;
        size_t bytes = 0;
        if (vkGetPipelineCacheData(device, pipeline_cache, &bytes, nullptr) != VK_SUCCESS || !bytes)
            return 0;
        std::vector<char> blob(bytes);
        if (vkGetPipelineCacheData(device, pipeline_cache, &bytes, blob.data()) != VK_SUCCESS)
            return 0;
        // Through a temporary and a rename: two processes starting at once must
        // not leave a half-written file behind for the next one to read.
        const std::string tmp = pipeline_cache_path + ".tmp";
        {
            std::ofstream f(nr::u8::path(tmp), std::ios::binary | std::ios::trunc);
            if (!f) return 0;
            f.write(blob.data(), std::streamsize(bytes));
            if (!f) return 0;
        }
        nr::u8::remove(pipeline_cache_path);
        if (nr::u8::rename(tmp, pipeline_cache_path) != 0) {
            nr::u8::remove(tmp);
            return 0;
        }
        return bytes;
    }

    // The extensions and features a host `vkCreateDevice` has to have enabled
    // for `adopt` to succeed. A layer adds these to the application's own
    // create info on the way through; nothing else can. The FP8 extension is
    // the rdna4-fp8 target's.
    const char* const* required_device_extensions(uint32_t& n) const {
        static const char* const with_fp8[] = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
                                               VK_EXT_SHADER_FLOAT8_EXTENSION_NAME};
        static const char* const without_fp8[] = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME};
        if (target_float8()) { n = 2; return with_fp8; }
        n = 1;
        return without_fp8;
    }

    // The 16x16x16 subgroup configuration with FP32 accumulation the target multiplies in;
    // the operand type comes from the target (e4m3 on gfx12, f16 on gfx11). Shared by
    // require_matrix_config and the rdna3 default device pick.
    static bool has_matrix_config(VkInstance inst, VkPhysicalDevice pd, VkComponentTypeKHR ab) {
        auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
            vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
        if (!query) return false;
        uint32_t n = 0;
        if (query(pd, &n, nullptr) != VK_SUCCESS || !n) return false;
        std::vector<VkCooperativeMatrixPropertiesKHR> p(
            n, {VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR});
        if (query(pd, &n, p.data()) != VK_SUCCESS) return false;
        for (const auto& m : p)
            if (m.MSize == 16 && m.NSize == 16 && m.KSize == 16 &&
                m.scope == VK_SCOPE_SUBGROUP_KHR && m.AType == ab && m.BType == ab &&
                m.CType == VK_COMPONENT_TYPE_FLOAT32_KHR &&
                m.ResultType == VK_COMPONENT_TYPE_FLOAT32_KHR && !m.saturatingAccumulation)
                return true;
        return false;
    }

    // The configuration the whole port rests on. Asserting it here means a driver that quietly
    // stops reporting it fails at startup with a name, rather than in a kernel with wrong numbers.
    void require_matrix_config() const {
        if (arch == nr::Arch::rdna2) return;   // no matrices: its fragments are plain arrays (rdna2/include/coopmm.glsl)
        if (has_matrix_config(instance, physical, target_matrix_ab())) return;
        if (!vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"))
            throw std::runtime_error("vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR absent");
        throw std::runtime_error(std::string(target_name())
                                 + ": 16x16x16 subgroup matrix configuration not reported");
    }

    // Arenas are plain DEVICE_LOCAL, never host-visible, and the host reaches
    // them through a staging copy.
    //
    // This is not fastidiousness, it is the difference between measuring the
    // GPU and measuring PCIe. The obvious shortcut - DEVICE_LOCAL that is also
    // HOST_VISIBLE|HOST_COHERENT, which resizable BAR makes available for all
    // of VRAM - looks like real VRAM and maps for free, but coherent host
    // access means the GPU cannot cache it normally, so a 1.75 MB working set
    // that belongs entirely in the 8 MB L2 goes to memory on every access
    // instead. gemm1x1 measured exactly at the VRAM bandwidth bound for its
    // traffic until this was changed, which is a plausible-looking number and a
    // completely fictitious one.
    //
    // It is also what the shipping runtime does: the OptiScaler adapter
    // allocates decision 6's two arenas out of device-local memory and the game
    // never maps them.
    Buffer buffer(VkDeviceSize bytes, bool host_visible = false, bool device_address = false) {
        Buffer b; b.bytes = bytes;
        device_address = device_address && buffer_device_address;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (device_address) info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        NRVK_CHECK(vkCreateBuffer(device, &info, nullptr, &b.handle));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.handle, &req);
        const VkMemoryPropertyFlags want =
            host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                         : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const VkMemoryPropertyFlags reject =
            host_visible ? 0 : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        uint32_t type = mem.memoryTypeCount;
        for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits & (1u << i)) && (f & want) == want && !(f & reject)) {
                type = i; break;
            }
        }
        // A device with no non-host-visible heap (an iGPU) has nothing to fall
        // back to, and a silent fallback is what this whole comment is about.
        if (type == mem.memoryTypeCount)
            throw std::runtime_error(host_visible ? "no host-visible memory type"
                                                  : "no device-local, non-host-visible memory type");
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size; alloc.memoryTypeIndex = type;
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        if (device_address) alloc.pNext = &flags;
        // Which allocation failed says more than the bare VkResult: an out of memory with VRAM
        // to spare is a size, a type or a heap, not the budget.
        if (const VkResult r = vkAllocateMemory(device, &alloc, nullptr, &b.memory); r != VK_SUCCESS)
            ::nrvk::check(r, ("vkAllocateMemory (buffer of " + std::to_string(req.size >> 20) + " MB, type " +
                              std::to_string(type) + ", heap " + std::to_string(mem.memoryTypes[type].heapIndex) +
                              (host_visible ? ", host visible" : ", device local") +
                              (device_address ? ", device address)" : ")")).c_str());
        NRVK_CHECK(vkBindBufferMemory(device, b.handle, b.memory, 0));
        {   // MZ_KCAP (nr_runtime.cpp) copies whole bound buffers
            std::lock_guard<std::mutex> guard(buffer_sizes_lock());
            buffer_sizes()[b.handle] = bytes;
        }
        if (device_address) {
            VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            ai.buffer = b.handle;
            b.address = vkGetBufferDeviceAddress(device, &ai);
        }
        if (host_visible) {
            NRVK_CHECK(vkMapMemory(device, b.memory, 0, bytes, 0, &b.mapped));
            std::memset(b.mapped, 0, bytes);
        }
        return b;
    }

    // Staged transfers. Setup path only - no attempt at overlap.
    //
    // **The staging buffer is at most kStageBytes, and big copies go through
    // it in pieces.** A mapping as large as the copy (the weights, a 4K input
    // image) needs that much contiguous address space, which a 32-bit game
    // often does not have (vkMapMemory: VK_ERROR_MEMORY_MAP_FAILED).
    static constexpr VkDeviceSize kStageBytes = VkDeviceSize(16) << 20;

    void transfer(Buffer& gpu, VkDeviceSize offset, void* host, VkDeviceSize bytes, bool to_gpu) {
        if (!bytes) return;
        const VkDeviceSize chunk = std::min(bytes, kStageBytes);
        Buffer stage = buffer(chunk, true);
        try {
            for (VkDeviceSize done = 0; done < bytes; done += chunk) {
                const VkDeviceSize n = std::min(chunk, bytes - done);
                uint8_t* h = static_cast<uint8_t*>(host) + done;
                if (to_gpu) std::memcpy(stage.mapped, h, size_t(n));
                one_shot([&](VkCommandBuffer cmd) {
                    VkBufferCopy region{};
                    region.srcOffset = to_gpu ? 0 : offset + done;
                    region.dstOffset = to_gpu ? offset + done : 0;
                    region.size = n;
                    vkCmdCopyBuffer(cmd, to_gpu ? stage.handle : gpu.handle,
                                    to_gpu ? gpu.handle : stage.handle, 1, &region);
                });
                if (!to_gpu) std::memcpy(h, stage.mapped, size_t(n));
            }
        } catch (...) {
            destroy(stage);
            throw;
        }
        destroy(stage);
    }
    // A buffer upload whose bytes are produced a staging piece at a time:
    // `fill(offset, n, dst)` writes bytes offset..offset+n of the range into dst.
    // The source never has to be one block (the graph's weight arena is chunked).
    void upload_with(Buffer& gpu, VkDeviceSize offset, VkDeviceSize bytes,
                     const std::function<void(VkDeviceSize, VkDeviceSize, uint8_t*)>& fill) {
        if (!bytes) return;
        const VkDeviceSize chunk = std::min(bytes, kStageBytes);
        Buffer stage = buffer(chunk, true);
        try {
            for (VkDeviceSize done = 0; done < bytes; done += chunk) {
                const VkDeviceSize n = std::min(chunk, bytes - done);
                fill(done, n, static_cast<uint8_t*>(stage.mapped));
                one_shot([&](VkCommandBuffer cmd) {
                    VkBufferCopy region{};
                    region.srcOffset = 0; region.dstOffset = offset + done; region.size = n;
                    vkCmdCopyBuffer(cmd, stage.handle, gpu.handle, 1, &region);
                });
            }
        } catch (...) {
            destroy(stage);
            throw;
        }
        destroy(stage);
    }
    void upload(Buffer& b, VkDeviceSize off, const void* data, VkDeviceSize bytes) {
        transfer(b, off, const_cast<void*>(data), bytes, true);
    }
    void download(Buffer& b, VkDeviceSize off, void* data, VkDeviceSize bytes) {
        transfer(b, off, data, bytes, false);
    }

    // ---- images: the network's outside edge -------------------------------
    //
    // **The two image adapters are the only layers that do not speak SSBO.**
    // `cc_tinlayout_fused_pre_block_swin_1h_32_1_ds_fp8` samples a CUDA texture
    // with `tex.2d.v4.f32.f32` and `cc_tinlayout_fused_post_block_swin_1h_32_fp8`
    // writes CUDA surfaces with `sust.p.2d.v4.b32` and has no `st.global` at
    // all. So the port needs a sampled image on the way in and a storage image
    // on the way out, and that is also the interface a game hands us: OptiScaler
    // passes VkImages, not buffers.
    //
    // The sampler is **MIRRORED_REPEAT with NEAREST filtering**, which is read
    // off the shipping kernel rather than chosen: the `selp` at each edge of its
    // coordinate computation is a mirror, not a clamp.
    struct Image {
        VkImage handle{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkSampler sampler{};          // null for a storage image
        uint32_t w{}, h{};
        VkFormat format{};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    };

    // `settle` moves the new image to the layout its shader will use, which
    // costs a one-shot submit and a fence wait. Every setup-path caller wants
    // that. A caller that is *recording a command buffer* must not block the
    // thread it is on, and can do the same transition in that buffer for free:
    // it passes false and owns the layout from then on.
    Image image(uint32_t w, uint32_t h, VkFormat format, bool sampled, bool settle = true,
                bool storage = true) {
        Image im; im.w = w; im.h = h; im.format = format;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {w, h, 1};
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        // **Storage on both.** The injector writes the network's input image
        // with a compute shader (`present_in`) and the graph then samples it, so
        // the sampled one has to be storage-capable too; the alternative is a
        // copy per frame for nothing.
        ii.usage = (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                            : VK_IMAGE_USAGE_STORAGE_BIT)
                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        // A sampled-only image (the runtime's input in the caller's own format,
        // which need not be storage-capable - *_SRGB's UNORM twin, 11/11/10).
        if (!storage) ii.usage &= ~VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT);
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        NRVK_CHECK(vkCreateImage(device, &ii, nullptr, &im.handle));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, im.handle, &req);
        uint32_t type = mem.memoryTypeCount;
        for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits & (1u << i)) &&
                (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
        }
        if (type == mem.memoryTypeCount)
            throw std::runtime_error("no device-local memory type for an image");
        image_bytes += req.size;
        bool placed = false;
        if (heap && heap->memory && (req.memoryTypeBits & (1u << heap->type))) {
            // In this runtime's region of the shared heap (ImageHeap); memory of its own when it does not fit.
            VkDeviceSize& cur = heap->cursor[heap_region];
            const VkDeviceSize off = (cur + req.alignment - 1) / req.alignment * req.alignment;
            heap->need[heap_region] = off + req.size;
            if (off + req.size <= heap->region_cap) {
                NRVK_CHECK(vkBindImageMemory(device, im.handle, heap->memory, heap->region_cap * heap_region + off));
                cur = off + req.size;
                placed = true;
            } else {
                cur = off + req.size;
            }
        }
        if (!placed) {
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.allocationSize = req.size; alloc.memoryTypeIndex = type;
            NRVK_CHECK(vkAllocateMemory(device, &alloc, nullptr, &im.memory));
            NRVK_CHECK(vkBindImageMemory(device, im.handle, im.memory, 0));
        }

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im.handle; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        NRVK_CHECK(vkCreateImageView(device, &vi, nullptr, &im.view));

        if (sampled) {
            VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = si.minFilter = VK_FILTER_NEAREST;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = si.addressModeV = si.addressModeW =
                VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            si.unnormalizedCoordinates = VK_FALSE;
            NRVK_CHECK(vkCreateSampler(device, &si, nullptr, &im.sampler));
        }
        // Both kinds go straight to the layout the shader will read or write
        // them in and stay there; nothing here renders, so there is no second
        // layout to move between.
        if (settle)
            transition(im, sampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_GENERAL);
        return im;
    }

    // Construction inside a recording (the resize path builds a runtime on the render thread): nothing is submitted.
    // `transition` keeps its barrier in `pending`, in order, for the first recording to record; anything else that
    // would submit throws, and the caller builds in the background instead.
    bool defer_transitions = false;
    std::vector<VkImageMemoryBarrier> pending;
    // Inside a recording: a pipeline not made yet (SharedPipeline) throws instead of compiling.
    bool registry_only = false;
    // Device memory every image made so far asked for.
    uint64_t image_bytes = 0;
    // Kept alive here as well as by the kernels (a network memory keeps every pipeline its runtimes used, so one built
    // inside a frame finds it made): null, none.
    std::vector<std::shared_ptr<SharedPipeline>>* pin = nullptr;
    // Images from this heap's region `heap_region` (ImageHeap); null: each its own allocation.
    ImageHeap* heap = nullptr;
    uint32_t heap_region = 0;

    // A one-shot submit, the same shape as `transfer`. `cb` records into the
    // command buffer and everything around it is boilerplate.
    template <typename F>
    void one_shot(F&& cb) {
        if (defer_transitions) throw std::runtime_error("a submit during construction inside a recording");
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex = family;
        VkCommandPool pool;
        NRVK_CHECK(vkCreateCommandPool(device, &cpi, nullptr, &pool));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        NRVK_CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        NRVK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        cb(cmd);
        NRVK_CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;
        NRVK_CHECK(vkCreateFence(device, &fi, nullptr, &fence));
        submit(si, fence);
        NRVK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 30000000000ull));
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
    }

    void transition(Image& im, VkImageLayout to) {
        if (im.layout == to) return;
        const VkImageLayout from = im.layout;
        if (defer_transitions) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = from; b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = im.handle;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            pending.push_back(b);
            im.layout = to;
            return;
        }
        one_shot([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = from; b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = im.handle;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &b);
        });
        im.layout = to;
    }

    // The image's rows, `rows` at a time through one staging buffer of at
    // most kStageBytes (see `transfer`). `io(y0, n, stage)` writes rows
    // y0..y0+n into the staging memory on the way to the GPU, and reads them
    // from it on the way back. Tightly packed rows, bytes / h each.
    void image_rows(Image& im, VkDeviceSize bytes, bool to_gpu,
                    const std::function<void(uint32_t, uint32_t, uint8_t*)>& io) {
        if (!bytes || !im.h) return;
        const VkDeviceSize row = bytes / im.h;
        const uint32_t step = uint32_t(std::max<VkDeviceSize>(1, std::min<VkDeviceSize>(im.h, kStageBytes / row)));
        Buffer stage = buffer(row * step, true);
        const VkImageLayout keep = im.layout;
        try {
            transition(im, to_gpu ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                  : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            for (uint32_t y0 = 0; y0 < im.h; y0 += step) {
                const uint32_t n = std::min(step, im.h - y0);
                if (to_gpu) io(y0, n, static_cast<uint8_t*>(stage.mapped));
                one_shot([&](VkCommandBuffer cmd) {
                    VkBufferImageCopy r{};
                    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    r.imageOffset = {0, int32_t(y0), 0};
                    r.imageExtent = {im.w, n, 1};
                    if (to_gpu)
                        vkCmdCopyBufferToImage(cmd, stage.handle, im.handle,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
                    else
                        vkCmdCopyImageToBuffer(cmd, im.handle,
                                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                               stage.handle, 1, &r);
                });
                if (!to_gpu) io(y0, n, static_cast<uint8_t*>(stage.mapped));
            }
            transition(im, keep);
        } catch (...) {
            destroy(stage);
            throw;
        }
        destroy(stage);
    }
    void image_transfer(Image& im, void* host, VkDeviceSize bytes, bool to_gpu) {
        const VkDeviceSize row = im.h ? bytes / im.h : 0;
        uint8_t* h = static_cast<uint8_t*>(host);
        image_rows(im, bytes, to_gpu, [&](uint32_t y0, uint32_t n, uint8_t* stage) {
            if (to_gpu) std::memcpy(stage, h + y0 * row, size_t(n * row));
            else std::memcpy(h + y0 * row, stage, size_t(n * row));
        });
    }
    void upload(Image& im, const void* data, VkDeviceSize bytes) {
        image_transfer(im, const_cast<void*>(data), bytes, true);
    }
    void download(Image& im, void* data, VkDeviceSize bytes) {
        image_transfer(im, data, bytes, false);
    }

    void destroy(Image& im) {
        if (im.sampler) vkDestroySampler(device, im.sampler, nullptr);
        vkDestroyImageView(device, im.view, nullptr);
        vkDestroyImage(device, im.handle, nullptr);
        vkFreeMemory(device, im.memory, nullptr);
        im = {};
    }

    void destroy(Buffer& b) {
        { std::lock_guard<std::mutex> guard(buffer_sizes_lock()); buffer_sizes().erase(b.handle); }
        if (b.mapped) vkUnmapMemory(device, b.memory);
        vkDestroyBuffer(device, b.handle, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
        b = {};
    }

    void destroy() {
        if (pipeline_cache) {
            vkDestroyPipelineCache(device, pipeline_cache, nullptr);
            pipeline_cache = VK_NULL_HANDLE;
        }
        // An adopted device belongs to the host application. Tearing it down
        // here would take the game with it.
        if (!adopted) {
            if (device) { nr::binary::forget(device); vkDestroyDevice(device, nullptr); }
            if (instance) vkDestroyInstance(instance, nullptr);
        }
        device = VK_NULL_HANDLE; instance = VK_NULL_HANDLE;
    }
};

inline std::vector<uint32_t> read_spirv(const std::string& path) {
    std::ifstream f(nr::u8::path(path), std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0 || n % 4) throw std::runtime_error("not a SPIR-V module: " + path);
    std::vector<uint32_t> code(size_t(n) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(code.data()), n);
    if (!f || code[0] != 0x07230203u) throw std::runtime_error("bad SPIR-V magic: " + path);
    return code;
}

// The storage-buffer bindings of a module that hold e4m3 data, by the name of their
// block: `*E4M3*`, `*Pair4`, and the small kernels' `Act` / `Act4`. On RDNA3 and RDNA2 the e4m3
// values live in FP16 twins of the arenas (`Context::fp16_twin`), indexed exactly like
// the byte arenas they shadow, and these are the bindings that read the twin.
inline std::set<uint32_t> e4m3_view_bindings(const std::vector<uint32_t>& code,
                                             std::set<uint32_t>* all_bindings = nullptr) {
    std::map<uint32_t, std::string> names;
    std::map<uint32_t, uint32_t> pointee, var_type, binding;
    for (size_t i = 5; i < code.size();) {
        const uint32_t op = code[i] & 0xFFFFu, wc = code[i] >> 16;
        if (!wc || i + wc > code.size()) break;
        if (op == 5 && wc >= 3)                                     // OpName
            names[code[i + 1]] = reinterpret_cast<const char*>(&code[i + 2]);
        else if (op == 32 && wc == 4 && code[i + 2] == 12)          // OpTypePointer StorageBuffer
            pointee[code[i + 1]] = code[i + 3];
        else if (op == 59 && wc >= 4 && code[i + 3] == 12)          // OpVariable StorageBuffer
            var_type[code[i + 2]] = code[i + 1];
        else if (op == 71 && wc >= 4 && code[i + 2] == 33)          // OpDecorate Binding
            binding[code[i + 1]] = code[i + 3];
        i += wc;
    }
    std::set<uint32_t> out;
    for (const auto& v : var_type) {
        const auto b = binding.find(v.first);
        const auto p = pointee.find(v.second);
        if (b == binding.end() || p == pointee.end()) continue;
        const std::string& n = names[p->second];
        if (all_bindings) all_bindings->insert(b->second);
        if (n.find("E4M3") != std::string::npos || n.find("Pair4") != std::string::npos ||
            n == "Act" || n == "Act4")
            out.insert(b->second);
    }
    return out;
}

// One compute pipeline over N storage-buffer bindings in set 0 plus a push
// constant block. That is decision 6's binding model, which is also the one
// that maps onto a D3D12 root signature.
// The parts of a compute pipeline that do not depend on what is bound: its shader module, set layout, pipeline layout
// and pipeline. Kernels of one device built from the same SPIR-V file, with the same binding types, push range and
// robustness, share one (the runtimes of one network at several frame sizes, the resize path); each keeps its own
// descriptor pool and set. Freed with the last kernel that uses it.
struct SharedPipeline {
    VkDevice device{};
    VkShaderModule module{};
    VkDescriptorSetLayout dsl{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    SharedPipeline() = default;
    SharedPipeline(const SharedPipeline&) = delete;
    SharedPipeline& operator=(const SharedPipeline&) = delete;
    ~SharedPipeline() {
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (module) vkDestroyShaderModule(device, module, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (dsl) vkDestroyDescriptorSetLayout(device, dsl, nullptr);
    }
};
inline std::mutex& shared_pipelines_lock() { static std::mutex m; return m; }
inline std::map<std::string, std::weak_ptr<SharedPipeline>>& shared_pipelines() {
    static std::map<std::string, std::weak_ptr<SharedPipeline>> m;
    return m;
}

struct Kernel {
    VkDevice device{};
    std::shared_ptr<SharedPipeline> shared;
    VkShaderModule module{};
    VkDescriptorSetLayout dsl{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    uint32_t push_range{};   // the layout's push constant bytes
    // What `retarget` needs: the image bindings' views (after the buffers) and the pool's sizes.
    uint32_t buffer_bindings{};
    std::vector<VkImageView> image_views;
    std::vector<VkDescriptorPoolSize> pool_sizes;
    std::vector<uint32_t> binding_ids;   // every binding of the set (the RDNA3 aliases' too)
    std::vector<VkBuffer> buffers;       // the buffer bindings' buffers as bound (twins in place), in binding order
    uint32_t aco_push_gap{}; // ACO INT4 and the FP16 networks use Linux's push layout, omitting Windows's arena address.
    // FP16 networks: which bindings read the FP16 twin of their arena and which are raw alias views, and the
    // context's twin map. A second descriptor set over this layout (nr_runtime.cpp kernel_set) has to
    // bind the same way `create` does, or the kernel reads and writes an arena in the wrong format.
    std::vector<uint32_t> twin_bindings, alias_bindings;
    // MZ_KCAP (diagnostic): the SPIR-V file and what `create` bound, binding by binding.
    std::string spirv;
    std::vector<std::pair<uint32_t, VkBuffer>> bound_buffers;
    const std::map<VkBuffer, VkBuffer>* twin_map{};
    std::string name;   // the SPIR-V file's stem (NR_STEP_TIMES)
    void push_constants(VkCommandBuffer cmd, uint32_t bytes, const void* data) const {
        if (!bytes) return;
        if (!aco_push_gap) {
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes, data);
            return;
        }
        if (bytes < aco_push_gap + 8 || bytes > 128)
            throw std::runtime_error("invalid ACO INT4 push block");
        std::array<uint8_t,128> compact{};
        std::memcpy(compact.data(), data, aco_push_gap);
        std::memcpy(compact.data()+aco_push_gap, static_cast<const uint8_t*>(data)+aco_push_gap+8,
                    bytes-aco_push_gap-8);
        // Keep the pipeline layout's declared range, including its zeroed tail.
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes, compact.data());
    }

    // **Image bindings follow the buffers**, in set 0, continuing the binding
    // numbers. The two image adapters are the only layers that need them - a
    // sampled image on the way in, a storage image on the way out - and every
    // other kernel keeps the pure-SSBO path untouched, which is decision 6's
    // binding model and the one that maps onto a D3D12 root signature.
    //
    // A sampled image is `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` and a
    // storage image is `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE`; which one a given
    // binding is comes from whether the `Image` carries a sampler.
    static constexpr uint32_t kAliasBase = 16;
    void create(Context& ctx, const std::string& spirv_path,
                const std::vector<VkBuffer>& bindings, uint32_t push_bytes,
                const std::vector<Context::Image*>& images = {}) {
        device = ctx.device;
        push_range = push_bytes;
        name = spirv_path.substr(spirv_path.find_last_of("/\\") + 1);
        std::vector<VkBuffer> bound(bindings);
        // FP16 networks: the e4m3 bindings get their arena's FP16 twin. Alias bindings (kAliasBase + k)
        // are raw-word views of the buffer at binding k and always get that buffer itself.
        std::vector<std::pair<uint32_t, VkBuffer>> aliases;
        if (ctx.fp16()) {
            std::set<uint32_t> declared;
            const std::set<uint32_t> e4m3 = e4m3_view_bindings(read_spirv(spirv_path), &declared);
            twin_map = &ctx.fp16_twin;
            twin_bindings.assign(e4m3.begin(), e4m3.end());
            for (uint32_t b : e4m3) {
                const auto twin = b < bound.size() ? ctx.fp16_twin.find(bound[b]) : ctx.fp16_twin.end();
                if (twin != ctx.fp16_twin.end()) bound[b] = twin->second;
            }
            for (uint32_t b : declared)
                if (b >= kAliasBase && b - kAliasBase < bindings.size())
                    aliases.push_back({b, bindings[b - kAliasBase]});
            for (const auto& alias : aliases) alias_bindings.push_back(alias.first);
            const auto name = spirv_path.substr(spirv_path.find_last_of("/\\") + 1);
            aco_push_gap = name.rfind("g_gemm", 0) == 0 && push_bytes >= 88 ? 80u
                         : name == "g_vitattn.spv" || name == "g_vitattncm.spv" ? 20u : 0u;
        }
#if NR_INT4
        if (!nr::binary::directory().empty() && nr::build_cfg("NR_I4_WEIGHTS")) {
            const auto name=spirv_path.substr(spirv_path.find_last_of("/\\")+1);
            if (!nr::binary::native_shell(name))
                aco_push_gap=name.rfind("g_gemm",0)==0 && push_bytes>=88 ? 80u : name=="g_vitattn.spv" ? 20u : 0u;
        }
#endif
        const uint32_t nb = uint32_t(bindings.size());
        const uint32_t ni = uint32_t(images.size());
        const uint32_t na = uint32_t(aliases.size());
        const uint32_t n = nb + ni + na;
        std::vector<VkDescriptorSetLayoutBinding> lb(n);
        for (uint32_t i = 0; i < nb; ++i)
            lb[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 0; i < ni; ++i)
            lb[nb + i] = {nb + i, images[i]->sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                     : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 0; i < na; ++i)
            lb[nb + ni + i] = {aliases[i].first, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        // The shared part: found by device, file, binding types, push range and robustness, or made here.
        std::string key = std::to_string(reinterpret_cast<uintptr_t>(device)) + "|" + spirv_path + "|" +
                          std::to_string(push_bytes) + "|" + (ctx.pipeline_robustness ? "r" : "-") + "|";
        for (const auto& b : lb) key += char('0' + int(b.descriptorType));
        // MZ_LOG_LAYOUT=1 (record making, tools/aco-records): each kernel's set layout and push range, which an ACO
        // record's descriptor offsets have to be made with.
        if (const char* ll = std::getenv("MZ_LOG_LAYOUT"); ll && *ll == '1') {
            std::string s;
            for (const auto& b : lb) s += " " + std::to_string(b.binding) + ":" + std::to_string(int(b.descriptorType));
            nr::logf("[nr] layout %s push %u bindings%s", spirv_path.substr(spirv_path.find_last_of("/\\") + 1).c_str(),
                     push_bytes, s.c_str());
        }
        for (uint32_t i = 0; i < na; ++i) key += "|" + std::to_string(aliases[i].first);
        {
            std::lock_guard<std::mutex> guard(shared_pipelines_lock());
            auto it = shared_pipelines().find(key);
            if (it != shared_pipelines().end()) shared = it->second.lock();
        }
        if (!shared && ctx.registry_only) throw std::runtime_error("a pipeline not made yet: " + spirv_path);
        if (!shared) {
            auto made = std::make_shared<SharedPipeline>();
            made->device = device;
            make_pipeline(ctx, spirv_path, push_bytes, lb, *made);
            std::lock_guard<std::mutex> guard(shared_pipelines_lock());
            auto& slot = shared_pipelines()[key];
            shared = slot.lock();   // another thread made it meanwhile: theirs, ours goes
            if (!shared) { slot = made; shared = made; }
        }
        module = shared->module; dsl = shared->dsl; layout = shared->layout; pipeline = shared->pipeline;
        spirv = spirv_path;
        bound_buffers.clear();
        for (uint32_t i = 0; i < nb; ++i) bound_buffers.push_back({i, bound[i]});
        for (const auto& alias : aliases) bound_buffers.push_back(alias);
        if (ctx.pin) {   // a memory's runtimes are built on two threads (inside frames, in the background)
            std::lock_guard<std::mutex> guard(shared_pipelines_lock());
            if (std::find(ctx.pin->begin(), ctx.pin->end(), shared) == ctx.pin->end()) ctx.pin->push_back(shared);
        }

        std::vector<VkDescriptorPoolSize> ps;
        if (nb + na) ps.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nb + na});
        for (uint32_t i = 0; i < ni; ++i)
            ps.push_back({images[i]->sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                             : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1});
        buffer_bindings = nb; pool_sizes = ps;
        buffers.assign(bound.begin(), bound.begin() + std::min<size_t>(bound.size(), nb));
        image_views.clear();
        for (uint32_t i = 0; i < ni; ++i) image_views.push_back(images[i]->view);
        binding_ids.clear();
        for (uint32_t b = 0; b < nb + ni; ++b) binding_ids.push_back(b);
        for (uint32_t i = 0; i < na; ++i) binding_ids.push_back(aliases[i].first);
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1; dpi.poolSizeCount = uint32_t(ps.size()); dpi.pPoolSizes = ps.data();
        NRVK_CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &pool));
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
        NRVK_CHECK(vkAllocateDescriptorSets(device, &dai, &set));
        std::vector<VkDescriptorBufferInfo> bi(nb + na);
        std::vector<VkDescriptorImageInfo> ii(ni);
        std::vector<VkWriteDescriptorSet> w(n);
        for (uint32_t i = 0; i < nb; ++i) {
            bi[i] = {bound[i], 0, VK_WHOLE_SIZE};
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = set; w[i].dstBinding = i; w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
        }
        for (uint32_t i = 0; i < ni; ++i) {
            ii[i] = {images[i]->sampler, images[i]->view, images[i]->layout};
            w[nb + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[nb + i].dstSet = set; w[nb + i].dstBinding = nb + i; w[nb + i].descriptorCount = 1;
            w[nb + i].descriptorType = images[i]->sampler
                ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[nb + i].pImageInfo = &ii[i];
        }
        for (uint32_t i = 0; i < na; ++i) {
            bi[nb + i] = {aliases[i].second, 0, VK_WHOLE_SIZE};
            w[nb + ni + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[nb + ni + i].dstSet = set; w[nb + ni + i].dstBinding = aliases[i].first; w[nb + ni + i].descriptorCount = 1;
            w[nb + ni + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nb + ni + i].pBufferInfo = &bi[nb + i];
        }
        vkUpdateDescriptorSets(device, n, w.data(), 0, nullptr);
    }

    // The shared part of create: layouts, module, pipeline (an ACO record or the driver's compile).
    void make_pipeline(Context& ctx, const std::string& spirv_path, uint32_t push_bytes,
                       const std::vector<VkDescriptorSetLayoutBinding>& lb, SharedPipeline& out) {
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = uint32_t(lb.size()); li.pBindings = lb.data();
        NRVK_CHECK(vkCreateDescriptorSetLayout(device, &li, nullptr, &out.dsl));

        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &out.dsl;
        pli.pushConstantRangeCount = push_bytes ? 1 : 0; pli.pPushConstantRanges = &pcr;
        NRVK_CHECK(vkCreatePipelineLayout(device, &pli, nullptr, &out.layout));

        const std::vector<uint32_t> code = read_spirv(spirv_path);
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = code.size() * 4; smi.pCode = code.data();
        NRVK_CHECK(vkCreateShaderModule(device, &smi, nullptr, &out.module));
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        req.requiredSubgroupSize = 32;
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cpi.stage.pNext = &req;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = out.module; cpi.stage.pName = "main";
        cpi.layout = out.layout;
        VkPipelineRobustnessCreateInfoEXT robustness{VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT};
        robustness.storageBuffers = robustness.uniformBuffers = robustness.vertexInputs =
            VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT;
        robustness.images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DISABLED_EXT;
        if (ctx.pipeline_robustness) cpi.pNext = &robustness;
#ifndef NR_PIPELINE_STATS
        NRVK_CHECK(nr::binary::create_or_default(device, ctx.pipeline_cache, cpi, spirv_path,
                                                code.data(), code.size() * 4, &out.pipeline, push_bytes));
#else
        if (ctx.pipeline_stats) cpi.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        // NR_DUMP_IR=<dir> (evaluation builds): also capture the driver's internal representations
        // and write each one to <dir>/<spv>.<executable>.<name>.txt. No pipeline cache then.
        const char* dump_ir = std::getenv("NR_DUMP_IR");
        if (ctx.pipeline_stats && dump_ir && *dump_ir)
            cpi.flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
        const auto t0 = std::chrono::steady_clock::now();
        const VkResult made = nr::binary::create_or_default(device, (dump_ir && *dump_ir) ? VK_NULL_HANDLE : ctx.pipeline_cache,
                                                       cpi, spirv_path, code.data(), code.size() * 4, &out.pipeline, push_bytes);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const std::string base = spirv_path.substr(spirv_path.find_last_of("/\\") + 1);
        std::printf("pipeline %s: %s in %.1f ms", base.c_str(), made == VK_SUCCESS ? "built" : "FAILED", ms);
        if (made == VK_SUCCESS && ctx.pipeline_stats) {
            auto props = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
                vkGetDeviceProcAddr(device, "vkGetPipelineExecutablePropertiesKHR"));
            auto stats = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
                vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR"));
            VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
            pi.pipeline = out.pipeline;
            uint32_t ne = 0;
            if (props && stats && props(device, &pi, &ne, nullptr) == VK_SUCCESS) {
                for (uint32_t e = 0; e < ne; ++e) {
                    VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
                    ei.pipeline = out.pipeline; ei.executableIndex = e;
                    uint32_t ns = 0;
                    if (stats(device, &ei, &ns, nullptr) != VK_SUCCESS) continue;
                    std::vector<VkPipelineExecutableStatisticKHR> s(
                        ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
                    if (stats(device, &ei, &ns, s.data()) != VK_SUCCESS) continue;
                    if (dump_ir && *dump_ir) {
                        auto irs = reinterpret_cast<PFN_vkGetPipelineExecutableInternalRepresentationsKHR>(
                            vkGetDeviceProcAddr(device, "vkGetPipelineExecutableInternalRepresentationsKHR"));
                        uint32_t nr = 0;
                        if (irs && irs(device, &ei, &nr, nullptr) == VK_SUCCESS && nr) {
                            std::vector<VkPipelineExecutableInternalRepresentationKHR> r(
                                nr, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
                            irs(device, &ei, &nr, r.data());
                            std::vector<std::vector<char>> buf(nr);
                            for (uint32_t i = 0; i < nr; ++i) { buf[i].resize(r[i].dataSize + 1); r[i].pData = buf[i].data(); }
                            irs(device, &ei, &nr, r.data());
                            for (uint32_t i = 0; i < nr; ++i) {
                                std::string fn = std::string(dump_ir) + "/" + base + "." + std::to_string(e) + "." + std::to_string(i) + ".txt";
                                std::ofstream f(nr::u8::path(fn), std::ios::binary);
                                f << "# " << r[i].name << " : " << r[i].description << "\n";
                                f.write(buf[i].data(), std::streamsize(r[i].dataSize));
                            }
                            std::printf(" | ir=%u", nr);
                        }
                    }
                    for (const auto& x : s) {
                        std::printf(" | %s=", x.name);
                        switch (x.format) {
                        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: std::printf("%u", x.value.b32); break;
                        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: std::printf("%lld", (long long)x.value.i64); break;
                        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: std::printf("%llu", (unsigned long long)x.value.u64); break;
                        default: std::printf("%g", x.value.f64); break;
                        }
                    }
                }
            }
        }
        std::printf("\n");
        std::fflush(stdout);
        NRVK_CHECK(made);
#endif
    }

    void destroy() {
        vkDestroyDescriptorPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE; set = VK_NULL_HANDLE;
        shared.reset();   // the pipeline itself goes with the last kernel that uses it
    }
    // **An image made after the kernel, in place of the stand-in it was created with** (an image the runtime only
    // makes when a frame needs it): a new set with every descriptor of the old one, the bindings that showed `from`
    // showing `to` (the same kind - sampled or storage - and the layout it is in now). The old set may still be
    // bound in a recording in flight, so its pool goes to `retired`, for the owner to destroy with the kernel.
    bool retarget(const Context::Image& from, const Context::Image& to, std::vector<VkDescriptorPool>& retired) {
        if (!pool || !from.view) return false;
        std::vector<uint32_t> hit;
        for (uint32_t i = 0; i < uint32_t(image_views.size()); ++i) if (image_views[i] == from.view) hit.push_back(i);
        if (hit.empty()) return false;
        if (bool(from.sampler) != bool(to.sampler)) throw std::runtime_error("retarget: an image of another kind");
        const VkDescriptorSet ns = copy_set(retired);
        const VkDescriptorImageInfo ii{to.sampler, to.view, to.layout};
        std::vector<VkWriteDescriptorSet> w;
        for (uint32_t i : hit) {
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            x.dstSet = ns; x.dstBinding = buffer_bindings + i; x.descriptorCount = 1;
            x.descriptorType = to.sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            x.pImageInfo = &ii;
            w.push_back(x);
            image_views[i] = to.view;
        }
        vkUpdateDescriptorSets(device, uint32_t(w.size()), w.data(), 0, nullptr);
        return true;
    }
    // The same for a buffer binding (`from`'s every binding of this kernel; never an RDNA3 alias - the one buffer this
    // moves, the noise table, has none).
    bool retarget_buffer(VkBuffer from, VkBuffer to, std::vector<VkDescriptorPool>& retired) {
        if (!pool || !from) return false;
        std::vector<uint32_t> hit;
        for (uint32_t i = 0; i < uint32_t(buffers.size()); ++i) if (buffers[i] == from) hit.push_back(i);
        if (hit.empty()) return false;
        const VkDescriptorSet ns = copy_set(retired);
        const VkDescriptorBufferInfo bi{to, 0, VK_WHOLE_SIZE};
        std::vector<VkWriteDescriptorSet> w;
        for (uint32_t i : hit) {
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            x.dstSet = ns; x.dstBinding = i; x.descriptorCount = 1;
            x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; x.pBufferInfo = &bi;
            w.push_back(x);
            buffers[i] = to;
        }
        vkUpdateDescriptorSets(device, uint32_t(w.size()), w.data(), 0, nullptr);
        return true;
    }
    // A new pool and set holding every descriptor of the current one; the old pool to `retired`.
    VkDescriptorSet copy_set(std::vector<VkDescriptorPool>& retired) {
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1; dpi.poolSizeCount = uint32_t(pool_sizes.size()); dpi.pPoolSizes = pool_sizes.data();
        VkDescriptorPool np{};
        NRVK_CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &np));
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = np; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
        VkDescriptorSet ns{};
        NRVK_CHECK(vkAllocateDescriptorSets(device, &dai, &ns));
        std::vector<VkCopyDescriptorSet> copies(binding_ids.size());
        for (size_t b = 0; b < binding_ids.size(); ++b) {
            copies[b] = {VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
            copies[b].srcSet = set; copies[b].srcBinding = binding_ids[b];
            copies[b].dstSet = ns; copies[b].dstBinding = binding_ids[b];
            copies[b].descriptorCount = 1;
        }
        vkUpdateDescriptorSets(device, 0, nullptr, uint32_t(copies.size()), copies.data());
        retired.push_back(pool);
        pool = np; set = ns;
        return ns;
    }
};

// Submit `repeats` back-to-back dispatches and return the GPU time for the
// whole timed region in milliseconds. Repeats are separated by a full barrier,
// so the number is a real dependent-dispatch cost and not an overlap artefact.
struct Runner {
    Context* ctx{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkQueryPool queries{};

    void create(Context& c) {
        ctx = &c;
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex = ctx->family;
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        NRVK_CHECK(vkCreateCommandPool(ctx->device, &cpi, nullptr, &pool));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        NRVK_CHECK(vkAllocateCommandBuffers(ctx->device, &cai, &cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        NRVK_CHECK(vkCreateFence(ctx->device, &fi, nullptr, &fence));
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2;
        NRVK_CHECK(vkCreateQueryPool(ctx->device, &qi, nullptr, &queries));
    }

    // `serialise` puts a full barrier between repeats, which is what an
    // unfused graph pays per layer. With it off the repeats overlap, which is
    // the closest this harness gets to what decision 11's fused segment sees:
    // the steady-state cost of the body with no launch, drain or first-load
    // latency of its own.
    bool serialise = true;
    // **Diagnostic only, and it computes the wrong frame.** Drops the
    // shader-write -> shader-read barrier between consecutive dispatches, so
    // every layer reads whatever its input buffer happened to hold. What it
    // measures is the ceiling: on this part such a barrier drains the whole
    // machine, and a dispatch that does not fill it - C=256 is 135 workgroups
    // and 4.2 waves a SIMD at 1080p - has its entire tail exposed with nothing
    // overlapping it. The graph emits 176 of these a frame and nobody had
    // priced them.
    bool no_barrier = false;
    bool read_barrier = false;
    // Emit a barrier only every `barrier_stride` steps. Diagnostic: it computes
    // the wrong frame, but it separates two explanations of why `--no-barrier`
    // is 1.35x. If the saving is proportional to the barriers removed it is the
    // drain; if it is all-or-nothing it is the cache, because a graph with no
    // barriers re-reads stale data that is already resident.
    uint32_t barrier_stride = 1;
    // An execution-only barrier: same wait, no cache maintenance. It separates
    // the two halves of the ~13 us each of these costs - if this is as slow as
    // the full one, the cost is the drain and only fewer dispatches can help;
    // if it is much faster, the cost is the cache op and a narrower barrier
    // can. Wrong output, because nothing is made visible.
    bool exec_barrier = false;
    // **A narrower barrier, and a correct one.** `VK_ACCESS_SHADER_READ_BIT` is
    // a broad mask - it covers sampled images, uniform buffers and storage
    // buffers - so the driver must invalidate the sampler and constant caches
    // too. Between two layers of this graph the only thing that moves is a
    // storage buffer, and Synchronization2 can say exactly that. Measured: the
    // cache half of these barriers is 0.67 ms of a 9.2 ms frame.
    bool storage_barrier = false;
    // **The invalidate half alone.** `srcAccessMask = 0`, so the barrier asks
    // for no availability operation and only for the visibility one: on gfx1201
    // that is the L0/GL1 invalidate and nothing else. It is sound on this part
    // and not in general, and the reason is in `coherent_act.glsl`: every cache
    // below the device coherence point is write-through, so the producing
    // dispatch's stores are already in L2 once the store counter drains and
    // there is nothing to make available. The consumer still has to be stopped
    // from hitting a stale L0 line, which is exactly what the destination half
    // does. It was built to ask whether the activation arena can drop
    // `coherent` - and with it device scope on every access - and buy the read
    // side back with one command-buffer packet a boundary instead.
    bool inv_barrier = false;
    // Drop the barrier after specific steps, by index. `--no-barrier` prices
    // every boundary at the frame's average; this prices one pair's.
    std::vector<uint8_t> no_barrier_after;

    // ---- how long one submit may hold the GPU --------------------------------
    // **This GPU also composites the user's desktop.** A submit is not
    // preemptible at any granularity that matters, so a 250 ms one blocks the
    // compositor for 250 ms; seven of those back to back is how this harness
    // reset the GPU and dropped the user to the login screen, twice.
    //
    // The fix is not fewer dispatches - the clock needs sustained load to leave
    // its 1.6 GHz idle, and a downclocked measurement is a useless one. It is
    // **more, shorter submits**: `chunk` dispatches per submit, submitted back
    // to back. The GPU stays busy across the fence waits, so DPM still boosts,
    // and the compositor gets a slot between chunks. Both requirements are
    // satisfied at once; the tension between them was imaginary.
    //
    // 0 means one submit for everything, which is right for a `--repeats 1`
    // correctness run and wrong for anything long. Size it from a probe:
    // chunk = submit budget / measured us per dispatch.
    uint32_t chunk = 0;

    double run(Kernel& k, uint32_t gx, uint32_t gy, uint32_t gz,
               const void* push, uint32_t push_bytes, uint32_t repeats = 1) {
        if (chunk && repeats > chunk) {
            double total = 0;
            for (uint32_t done = 0; done < repeats; done += chunk) {
                const uint32_t n = std::min(chunk, repeats - done);
                total += run_once(k, gx, gy, gz, push, push_bytes, n);
            }
            return total;
        }
        return run_once(k, gx, gy, gz, push, push_bytes, repeats);
    }

    double run_once(Kernel& k, uint32_t gx, uint32_t gy, uint32_t gz,
                    const void* push, uint32_t push_bytes, uint32_t repeats) {
        NRVK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        NRVK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        vkCmdResetQueryPool(cmd, queries, 0, 2);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.layout, 0, 1, &k.set, 0, nullptr);
        k.push_constants(cmd, push_bytes, push);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        VkMemoryBarrier between{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        between.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        between.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        for (uint32_t i = 0; i < repeats; ++i) {
            vkCmdDispatch(cmd, gx, gy, gz);
            if (serialise && i + 1 < repeats)
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &between, 0, nullptr, 0, nullptr);
        }
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        NRVK_CHECK(vkEndCommandBuffer(cmd));
        NRVK_CHECK(vkResetFences(ctx->device, 1, &fence));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        ctx->submit(si, fence);
        NRVK_CHECK(vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, 30000000000ull));
        uint64_t ts[2] = {0, 0};
        NRVK_CHECK(vkGetQueryPoolResults(ctx->device, queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                                         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        return double(ts[1] - ts[0]) * double(ctx->timestamp_period) * 1e-6;
    }

    // ---- the graph ------------------------------------------------------
    // Every other entry point here records one dispatch and submits it, which
    // is what a kernel test wants and what a *frame* must not do: 152 submits
    // with a fence wait each is 152 round trips to the driver, and the layers
    // are a strict chain so none of them overlap anyway.
    //
    // `run_graph` records the whole list into one command buffer with a
    // shader-write -> shader-read barrier between consecutive steps, and
    // submits once. The kernels differ, the push constants differ, the
    // descriptor sets differ - all of that is per step - and the buffers do not,
    // because every kernel in this project addresses one activation arena and
    // one weight arena through offsets in its push constants. That is what makes
    // a single set of bindings enough for the whole network.
    struct Step {
        Kernel* k{};
        uint32_t gx{1}, gy{1}, gz{1};
        const void* push{};
        uint32_t push_bytes{};
    };

    // Returns milliseconds for the whole recorded list, from the GPU's own
    // timestamps, so it is the frame time and not the submit time.
    // Zeroed before the first step and outside the timed region. An
    // uninitialised arena is not merely wrong data, it is **e4m3 NaN**: 0x7F and
    // 0xFF are this format's NaN, they are 1 byte in 128 of anything random, and
    // one of them destroys a whole accumulator tile of the next MMA. Every layer
    // whose producer has no kernel yet reads such a slot.
    VkBuffer zero_buffer{};
    VkBuffer zero_twin{};              // FP16 networks: the FP16 twin of zero_buffer, zeroed with it
    VkDeviceSize zero_twin_bytes{};
    // Arena-reuse diagnostics (nr_graph.cpp, NR_ARENA_UNWRITTEN): ranges filled
    // after the zero fill, and with what.
    uint32_t zero_value = 0;
    std::vector<std::pair<VkDeviceSize, VkDeviceSize>> poison;
    uint32_t poison_value = 0x3C3C3C3Cu;
    VkDeviceSize zero_bytes{};

    // ---- where the frame's milliseconds actually go --------------------------
    // A whole-graph number tells you the frame is 23.7 ms and nothing about
    // which of its 171 dispatches that is. A per-family table
    // is a *model* - each family's kernel run alone at the
    // plan's shape - and a model cannot see what the real chain does to a layer:
    // a cold arena, a launch that does not fill the machine, a barrier the
    // previous layer's tail is still draining into.
    //
    // With `per_step` the runner writes a BOTTOM_OF_PIPE timestamp after every
    // dispatch, so `step_ms[i]` is that dispatch's own time inside the frame.
    // **It costs nothing that is not already paid**: consecutive steps are
    // already separated by a full shader-write -> shader-read barrier, so there
    // is no overlap for the timestamps to destroy.
    bool per_step = false;
    std::vector<double> step_ms;
    VkQueryPool qstep{};
    uint32_t qstep_count{};
    std::vector<uint64_t> raw_ts;

    void ensure_step_queries(uint32_t n) {
        if (qstep_count >= n) return;
        if (qstep) vkDestroyQueryPool(ctx->device, qstep, nullptr);
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = n;
        NRVK_CHECK(vkCreateQueryPool(ctx->device, &qi, nullptr, &qstep));
        qstep_count = n;
    }

    double run_graph(const std::vector<Step>& steps, uint32_t repeats = 1) {
        // Per-step timing records one pass a submit and averages the passes, so
        // that a long `--repeats` still reaches the boost clock while every
        // individual number stays attributable.
        if (per_step) {
            std::vector<double> sum(steps.size(), 0.0);
            double total = 0;
            for (uint32_t r = 0; r < repeats; ++r) {
                total += run_graph_once(steps, 1, r == 0);
                for (size_t i = 0; i < steps.size(); ++i) sum[i] += step_ms[i];
            }
            for (double& v : sum) v /= double(repeats ? repeats : 1);
            step_ms.swap(sum);
            return total / double(repeats ? repeats : 1);
        }
        // Same rule as `run`: a pass of the whole graph is several milliseconds,
        // so `repeats` passes in one submit is exactly the shape that starves
        // the compositor. One pass a submit unless told otherwise.
        const uint32_t per = chunk ? chunk : 1u;
        if (repeats > per) {
            double total = 0;
            uint32_t recorded_repeats = 0;
            for (uint32_t done = 0; done < repeats; done += per) {
                const uint32_t count = std::min(per, repeats - done);
                total += run_graph_once(steps, count, count != recorded_repeats) * double(count);
                recorded_repeats = count;
            }
            return total / double(repeats);
        }
        return run_graph_once(steps, repeats);
    }

    double run_graph_once(const std::vector<Step>& steps, uint32_t repeats, bool record = true) {
        const uint32_t nq = per_step ? uint32_t(steps.size()) + 1 : 0;
        // Steps and push constants are fixed within one run_graph call. Reuse
        // that recording for its repeats; each pass still has its own submit
        // and fence. Re-record when a new call or a different batch begins.
        if (record) {
            NRVK_CHECK(vkResetCommandBuffer(cmd, 0));
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            NRVK_CHECK(vkBeginCommandBuffer(cmd, &bi));
            vkCmdResetQueryPool(cmd, queries, 0, 2);
            if (nq) {
                ensure_step_queries(nq);
                vkCmdResetQueryPool(cmd, qstep, 0, nq);
            }
            if (zero_buffer && zero_bytes) {
                vkCmdFillBuffer(cmd, zero_buffer, 0, zero_bytes, zero_value);
                if (zero_twin && zero_twin_bytes) vkCmdFillBuffer(cmd, zero_twin, 0, zero_twin_bytes, zero_value);
                for (const auto& pr : poison)
                    vkCmdFillBuffer(cmd, zero_buffer, pr.first, pr.second, poison_value);
                VkMemoryBarrier zb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                zb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                zb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &zb,
                                     0, nullptr, 0, nullptr);
            }
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
            if (nq) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qstep, 0);
            VkMemoryBarrier between{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            // `--inv-barrier` drops the source half; see `inv_barrier` above.
            between.srcAccessMask = inv_barrier ? 0u : VK_ACCESS_SHADER_WRITE_BIT;
            // `--read-barrier` narrows the destination to SHADER_READ. The
            // write bit is there because a later layer may also *write* a slot
            // this one wrote - aliasing inside the arena - so narrowing is only
            // safe if the plan says otherwise. It is a knob because the cost of
            // these barriers is 26% of the frame and the first question is
            // which part of that is the cache and which is the drain.
            between.dstAccessMask = read_barrier
                ? VK_ACCESS_SHADER_READ_BIT
                : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            for (uint32_t r = 0; r < repeats; ++r)
                for (size_t i = 0; i < steps.size(); ++i) {
                    const Step& s = steps[i];
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.k->pipeline);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.k->layout,
                                            0, 1, &s.k->set, 0, nullptr);
                    s.k->push_constants(cmd, s.push_bytes, s.push);
                    vkCmdDispatch(cmd, s.gx, s.gy, s.gz);
                    if (nq)
                        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                            qstep, uint32_t(i) + 1);
                    if (!no_barrier && (i + 1 < steps.size() || r + 1 < repeats)
                        && (i >= no_barrier_after.size() || !no_barrier_after[i])
                        && (barrier_stride <= 1 || (i % barrier_stride) == barrier_stride - 1)) {
                        if (storage_barrier) {
                            VkMemoryBarrier2 mb2{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                            mb2.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                            mb2.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                            mb2.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                            mb2.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                                              | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                            dep.memoryBarrierCount = 1;
                            dep.pMemoryBarriers = &mb2;
                            vkCmdPipelineBarrier2(cmd, &dep);
                        } else {
                            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                                 exec_barrier ? 0u : 1u, &between,
                                                 0, nullptr, 0, nullptr);
                        }
                    }
                }
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
            NRVK_CHECK(vkEndCommandBuffer(cmd));
        }
        NRVK_CHECK(vkResetFences(ctx->device, 1, &fence));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        ctx->submit(si, fence);
        NRVK_CHECK(vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, 30000000000ull));
        uint64_t ts[2] = {0, 0};
        NRVK_CHECK(vkGetQueryPoolResults(ctx->device, queries, 0, 2, sizeof(ts), ts,
                                         sizeof(uint64_t),
                                         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        if (nq) {
            raw_ts.assign(nq, 0);
            NRVK_CHECK(vkGetQueryPoolResults(ctx->device, qstep, 0, nq,
                                             nq * sizeof(uint64_t), raw_ts.data(),
                                             sizeof(uint64_t),
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            step_ms.assign(steps.size(), 0.0);
            for (size_t i = 0; i < steps.size(); ++i)
                step_ms[i] = double(raw_ts[i + 1] - raw_ts[i]) *
                             double(ctx->timestamp_period) * 1e-6;
        }
        return double(ts[1] - ts[0]) * double(ctx->timestamp_period) * 1e-6 / double(repeats);
    }

    void destroy() {
        if (qstep) vkDestroyQueryPool(ctx->device, qstep, nullptr);
        vkDestroyQueryPool(ctx->device, queries, nullptr);
        vkDestroyFence(ctx->device, fence, nullptr);
        vkDestroyCommandPool(ctx->device, pool, nullptr);
    }
};

// ---- blits on compute-only command buffers --------------------------------------------------------------------------
// A D3D12 COMPUTE list is recorded through vkd3d-proton into a compute queue's command buffer (Streamline runs DLSS frame
// generation's evaluate on one; the Present layers are recorded into it there). vkCmdBlitImage is a graphics command
// (RADV draws it): in such a command buffer it is an illegal packet ("Illegal opcode in command stream", the compute
// ring times out, the device is lost). While a recording into such a list is open (ComputeOnly, the session's
// begin_recording), blit() records runtime_blit.comp instead; anywhere else it records vkCmdBlitImage exactly as before.
inline bool& compute_only_recording() { static thread_local bool on = false; return on; }
struct ComputeOnly {
    bool was;
    explicit ComputeOnly(bool on) : was(compute_only_recording()) { compute_only_recording() = on; }
    ~ComputeOnly() { compute_only_recording() = was; }
    ComputeOnly(const ComputeOnly&) = delete;
    ComputeOnly& operator=(const ComputeOnly&) = delete;
};
// runtime_blit.spv's path: the first runtime built tells it (its adapter folder; the RenoDX stage has none of its own).
inline std::mutex& compute_blit_lock() { static std::mutex m; return m; }
inline std::string& compute_blit_spirv() { static std::string path; return path; }
inline void set_compute_blit_spirv(const std::string& path) {
    std::lock_guard<std::mutex> guard(compute_blit_lock());
    if (compute_blit_spirv().empty()) compute_blit_spirv() = path;
}

// runtime_blit.comp on one device: its pipeline, samplers, and a ring of per-blit views and descriptor pools, each slot
// reused once the recording that used it is done (nr_gpu_completion.hpp).
struct ComputeBlit {
    VkDevice device{};
    VkPhysicalDevice physical{};
    VkDescriptorSetLayout dsl{};
    VkPipelineLayout layout{};
    VkShaderModule module{};
    VkPipeline pipeline{};
    VkSampler nearest{}, linear{};
    bool failed = false;
    struct Slot { nr::gpu::Ticket gpu; VkImageView sv{}, dv{}; VkDescriptorPool pool{}; };
    std::vector<Slot> ring;
    uint64_t used = 0;
    std::map<VkFormat, bool> storable;
    struct Push { int32_t s[4]; int32_t d[4]; uint32_t linear; uint32_t rtz16; };

    bool init(const std::string& spirv) {
        try {
            VkDescriptorSetLayoutBinding b[2] = {
                {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = 2; li.pBindings = b;
            NRVK_CHECK(vkCreateDescriptorSetLayout(device, &li, nullptr, &dsl));
            VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, uint32_t(sizeof(Push))};
            VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
            NRVK_CHECK(vkCreatePipelineLayout(device, &pli, nullptr, &layout));
            const std::vector<uint32_t> code = read_spirv(spirv);
            VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smi.codeSize = code.size() * 4; smi.pCode = code.data();
            NRVK_CHECK(vkCreateShaderModule(device, &smi, nullptr, &module));
            VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = module; cpi.stage.pName = "main";
            cpi.layout = layout;
            NRVK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline));
            for (const VkFilter f : {VK_FILTER_NEAREST, VK_FILTER_LINEAR}) {
                VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                si.magFilter = si.minFilter = f;
                si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                NRVK_CHECK(vkCreateSampler(device, &si, nullptr, f == VK_FILTER_LINEAR ? &linear : &nearest));
            }
            ring.resize(16);
            return true;
        } catch (const std::exception& e) {
            nr::logf("[nr] the compute blit (runtime_blit.spv) could not be made: %s; blits into compute-only command "
                     "lists are left out", e.what());
            failed = true;
            return false;
        }
    }
    bool can_store(VkFormat f) {
        const auto it = storable.find(f);
        if (it != storable.end()) return it->second;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(physical, f, &fp);
        const bool ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        storable[f] = ok;
        return ok;
    }
    static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags ss,
                        VkAccessFlags sa, VkPipelineStageFlags ds, VkAccessFlags da) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to; b.srcAccessMask = sa; b.dstAccessMask = da;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    // One region. False (nothing recorded) when it cannot: a flip, another mip or layer, a destination format no storage
    // image takes, or a view / set that could not be made.
    bool record(VkCommandBuffer cmd, VkImage src, VkFormat sf, VkImageLayout sl, VkImage dst, VkFormat df,
                VkImageLayout dl, const VkImageBlit& r, VkFilter filter) {
        if (failed || !pipeline) return false;
        const int32_t dw = r.dstOffsets[1].x - r.dstOffsets[0].x, dh = r.dstOffsets[1].y - r.dstOffsets[0].y;
        if (dw <= 0 || dh <= 0 || r.srcOffsets[1].x <= r.srcOffsets[0].x || r.srcOffsets[1].y <= r.srcOffsets[0].y ||
            r.srcSubresource.mipLevel || r.dstSubresource.mipLevel || r.srcSubresource.baseArrayLayer ||
            r.dstSubresource.baseArrayLayer)
            return false;
        if (!can_store(df)) return false;
        Slot& slot = nr::gpu::slot(ring, size_t(used++ % ring.size()));
        if (slot.sv) vkDestroyImageView(device, slot.sv, nullptr);
        if (slot.dv) vkDestroyImageView(device, slot.dv, nullptr);
        slot.sv = slot.dv = VK_NULL_HANDLE;
        slot.gpu = nr::gpu::current();
        if (!slot.pool) {
            const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = 1; pi.poolSizeCount = 2; pi.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(device, &pi, nullptr, &slot.pool) != VK_SUCCESS) { slot.pool = VK_NULL_HANDLE; return false; }
        } else if (vkResetDescriptorPool(device, slot.pool, 0) != VK_SUCCESS) {
            return false;
        }
        const auto view = [&](VkImage im, VkFormat f, VkImageUsageFlags usage, VkImageView* out) {
            VkImageViewUsageCreateInfo uci{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
            uci.usage = usage;
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.pNext = &uci;
            vi.image = im; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            return vkCreateImageView(device, &vi, nullptr, out) == VK_SUCCESS;
        };
        if (!view(src, sf, VK_IMAGE_USAGE_SAMPLED_BIT, &slot.sv) || !view(dst, df, VK_IMAGE_USAGE_STORAGE_BIT, &slot.dv))
            return false;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = slot.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &dsl;
        VkDescriptorSet set{};
        if (vkAllocateDescriptorSets(device, &ai, &set) != VK_SUCCESS) return false;
        VkDescriptorImageInfo ii[2] = {{filter == VK_FILTER_LINEAR ? linear : nearest, slot.sv, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, slot.dv, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet = set; w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &ii[0];
        w[1].dstSet = set; w[1].dstBinding = 1; w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &ii[1];
        vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
        // the layouts the caller put them in for the blit -> GENERAL for the pass -> back; everything before it done
        // first, everything after it seeing its writes (a caller's barrier after a blit names the transfer stage)
        barrier(cmd, src, sl, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        barrier(cmd, dst, dl, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT);
        Push p{};
        p.s[0] = r.srcOffsets[0].x; p.s[1] = r.srcOffsets[0].y; p.s[2] = r.srcOffsets[1].x; p.s[3] = r.srcOffsets[1].y;
        p.d[0] = r.dstOffsets[0].x; p.d[1] = r.dstOffsets[0].y; p.d[2] = r.dstOffsets[1].x; p.d[3] = r.dstOffsets[1].y;
        p.linear = filter == VK_FILTER_LINEAR ? 1u : 0u;
        // RADV's blit exports these through FP16 toward zero (runtime_blit.comp): the same here
        switch (df) {
            case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
                p.rtz16 = 1u;
                break;
            default:
                p.rtz16 = 0u;
                break;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        vkCmdDispatch(cmd, uint32_t(dw + 7) / 8, uint32_t(dh + 7) / 8, 1);
        barrier(cmd, dst, VK_IMAGE_LAYOUT_GENERAL, dl, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        barrier(cmd, src, VK_IMAGE_LAYOUT_GENERAL, sl, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0);
        return true;
    }
};
inline std::map<VkDevice, std::unique_ptr<ComputeBlit>>& compute_blits() {
    static std::map<VkDevice, std::unique_ptr<ComputeBlit>> m;
    return m;
}

// vkCmdBlitImage, or on a compute-only command buffer runtime_blit.comp. The formats are the images' own (the views
// the pass needs). False when the compute pass could not record a region (said
// once; the destination keeps what it had - a wrong picture for that frame, never a hung queue).
inline bool blit(VkPhysicalDevice physical, VkDevice device, VkCommandBuffer cmd, VkImage src, VkFormat sf,
                 VkImageLayout sl, VkImage dst, VkFormat df, VkImageLayout dl, uint32_t regions, const VkImageBlit* r,
                 VkFilter filter) {
    if (!compute_only_recording()) {
        vkCmdBlitImage(cmd, src, sl, dst, dl, regions, r, filter);
        return true;
    }
    std::lock_guard<std::mutex> guard(compute_blit_lock());
    auto& cb = compute_blits()[device];
    if (!cb) {
        cb = std::make_unique<ComputeBlit>();
        cb->device = device; cb->physical = physical;
        if (compute_blit_spirv().empty()) {
            cb->failed = true;
            nr::logf("[nr] a blit into a compute-only command list, before any runtime was built: left out");
        } else {
            cb->init(compute_blit_spirv());
        }
    }
    bool ok = true;
    for (uint32_t i = 0; i < regions; ++i) ok = cb->record(cmd, src, sf, sl, dst, df, dl, r[i], filter) && ok;
    static bool said = false, said_fail = false;
    if (ok && !said) {
        said = true;
        nr::logf("[nr] a compute-only command list (DLSS-G's evaluate through Streamline, say): its blits run as a compute "
                 "pass (runtime_blit)");
    }
    if (!ok && !said_fail) {
        said_fail = true;
        nr::logf("[nr] a blit into a compute-only command list could not be recorded as a compute pass (VkFormat %u -> "
                 "%u); that image keeps what it had", unsigned(sf), unsigned(df));
    }
    return ok;
}

}  // namespace nrvk
