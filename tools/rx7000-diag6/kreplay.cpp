// kreplay: runs one mochizuki network dispatch again and again on fixed bytes, to see whether a kernel gives the same
// answer every time on this GPU (the RX 7000 kits 3, 5 and 6; this copy is kit 6's: scrub fills, rel/fix/nat summary).
//
//   kreplay kit <files dir> <result dir>           the kit: every case in <files>\kit.txt, each run in a child
//   kreplay case <case dir> <spv> <n> <fill> <out>  one case, n runs; fill zero|rand (what the bytes nobody should
//                                                   read hold: zeros, or a new pattern every run), optionally
//                                                   +dirty / +clean: before every run a dispatch that leaves
//                                                   garbage / zeros in every workgroup's LDS and ~96 VGPRs a lane
//                                                   (the runtime's seq_scrub.comp); results in <out>
//   kreplay readset <capture dir> <spv> <out>       (here) which bytes of a capture (MZ_KCAP) the dispatch reads and
//                                                   writes, by poisoning pages, so a case carries only those
//   kreplay full <capture dir> <spv> <out.bin>      (here) the capture run once whole; its write set's bytes
//
// A case (case.txt): the dispatch's SPIR-V, grid, push constants, buffers and bindings; "slice" lines name the bytes
// put into the buffers before every run (data.bin), "out" lines the regions hashed after it, optionally with a
// reference (the RX 9070 XT's FP8 network's value of the same plan key) the result is measured against.
#include "child.hpp"
#include <cstdint>
#include "scrub_spv.h"   // seq_scrub.comp, glslang -V --vn kScrubSpv
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::vector<uint8_t> ReadFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::vector<uint8_t> b(size_t(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()), std::streamsize(b.size()));
    return b;
}

static bool WriteFile(const fs::path& p, const void* d, size_t n)
{
    std::ofstream f(p, std::ios::binary);
    f.write(static_cast<const char*>(d), std::streamsize(n));
    return bool(f);
}

static uint64_t Hash(const uint8_t* p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static float F16(uint16_t h)
{
    const uint32_t s = uint32_t(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023;
    uint32_t b;
    if (e == 0)
    {
        if (!m) b = s;
        else
        {
            float f = std::ldexp(float(m), -24);
            return s ? -f : f;
        }
    }
    else if (e == 31) b = s | 0x7f800000u | (m << 13);
    else b = s | ((e + 112) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

static float E4M3(uint8_t v)
{
    const int s = v >> 7, e = (v >> 3) & 15, m = v & 7;
    if (e == 15 && m == 7) return NAN;
    const float f = e ? std::ldexp(1.0f + m / 8.0f, e - 7) : std::ldexp(m / 8.0f, -6);
    return s ? -f : f;
}

// An e4m3 value as the f16 that holds it exactly (the RDNA3 network's FP16 twin of an e4m3 arena).
static uint16_t E4M3ToF16(uint8_t v)
{
    const uint16_t s = uint16_t(v & 0x80) << 8;
    const int e = (v >> 3) & 15, m = v & 7;
    if (e == 15 && m == 7) return uint16_t(s | 0x7E00);
    if (e == 0)
    {
        if (!m) return s;
        // m/8 * 2^-6 = m * 2^-9: an f16 normal (2^-9 .. 2^-7)
        int mm = m, ee = -9;
        while (!(mm & 8)) { mm <<= 1; --ee; }
        return uint16_t(s | uint16_t((ee + 3 + 15) << 10) | uint16_t((mm & 7) << 7));
    }
    return uint16_t(s | uint16_t((e - 7 + 15) << 10) | uint16_t(m << 7));
}

// ---- the case file ----------------------------------------------------------------------------------------------

struct Case
{
    std::string name, spirv, about;
    uint32_t gx = 1, gy = 1, gz = 1, pushRange = 0;
    std::vector<uint32_t> push;
    std::vector<uint64_t> bufSize;
    std::vector<std::pair<uint32_t, uint32_t>> bindings; // binding, buffer
    struct Slice { uint32_t buf; uint64_t off, bytes, at; bool e4m3 = false; };   // e4m3: data.bin holds bytes/2 e4m3 values, put in as f16
    std::vector<Slice> slices;
    struct Out { uint32_t buf; uint64_t off, bytes; std::string kind, label, ref, refEnc; };   // refEnc e4m3: the reference file holds e4m3 values of an f16 output
    std::vector<Out> outs;
};

static bool LoadCase(const fs::path& file, Case& c, std::string& err)
{
    std::ifstream f(file);
    if (!f) { err = "cannot open " + file.string(); return false; }
    std::string line;
    while (std::getline(f, line))
    {
        std::istringstream s(line);
        std::string k;
        s >> k;
        if (k == "name") s >> c.name;
        else if (k == "about") { std::getline(s, c.about); if (!c.about.empty() && c.about[0] == ' ') c.about.erase(0, 1); }
        else if (k == "spirv") s >> c.spirv;
        else if (k == "grid") s >> c.gx >> c.gy >> c.gz;
        else if (k == "push_range") s >> c.pushRange;
        else if (k == "push") { std::string w; while (s >> w) c.push.push_back(uint32_t(std::stoul(w, nullptr, 16))); }
        else if (k == "buffer") { uint32_t i; uint64_t n; s >> i >> n; if (c.bufSize.size() <= i) c.bufSize.resize(i + 1); c.bufSize[i] = n; }
        else if (k == "binding") { uint32_t b, i; s >> b >> i; c.bindings.push_back({ b, i }); }
        else if (k == "slice") { Case::Slice x {}; std::string e; s >> x.buf >> x.off >> x.bytes >> x.at >> e; x.e4m3 = e == "e4m3"; c.slices.push_back(x); }
        else if (k == "out") { Case::Out o {}; s >> o.buf >> o.off >> o.bytes >> o.kind >> o.label >> o.ref >> o.refEnc; if (o.ref == "-") o.ref.clear(); c.outs.push_back(o); }
    }
    if (c.bufSize.empty() || c.bindings.empty()) { err = "no buffers in " + file.string(); return false; }
    return true;
}

// ---- Vulkan -----------------------------------------------------------------------------------------------------

struct Gpu
{
    VkInstance instance {};
    VkPhysicalDevice phys {};
    VkDevice dev {};
    VkQueue queue {};
    uint32_t family = 0;
    VkCommandPool pool {};
    VkCommandBuffer cmd {};
    VkFence fence {};
    VkPhysicalDeviceMemoryProperties mem {};
    VkPhysicalDeviceProperties props {};
    VkPhysicalDeviceDriverProperties drv { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    bool robust = false;
    uint64_t waitNs = 20000000000ull;
    std::string err;

    struct Buf { VkBuffer b {}; VkDeviceMemory m {}; void* map {}; uint64_t size = 0; };

    static bool HasExt(VkPhysicalDevice p, const char* e)
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(p, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> x(n);
        vkEnumerateDeviceExtensionProperties(p, nullptr, &n, x.data());
        return std::any_of(x.begin(), x.end(), [e](const VkExtensionProperties& v) { return !std::strcmp(v.extensionName, e); });
    }

    bool Init()
    {
        VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "kreplay";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ii { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ii.pApplicationInfo = &app;
        if (VkResult r = vkCreateInstance(&ii, nullptr, &instance)) { err = "vkCreateInstance " + std::to_string(r); return false; }
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        std::vector<VkPhysicalDevice> all(n);
        vkEnumeratePhysicalDevices(instance, &n, all.data());
        for (int pass = 0; pass < 2 && !phys; ++pass)
            for (auto p : all)
            {
                vkGetPhysicalDeviceProperties(p, &props);
                if (props.vendorID == 0x1002 && (pass || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) { phys = p; break; }
            }
        if (!phys) { err = "no AMD Vulkan device"; return false; }
        vkGetPhysicalDeviceProperties(phys, &props);
        VkPhysicalDeviceProperties2 p2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv };
        vkGetPhysicalDeviceProperties2(phys, &p2);
        vkGetPhysicalDeviceMemoryProperties(phys, &mem);
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf.data());
        for (uint32_t i = 0; i < nq; ++i)
            if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }

        const bool robustExt = HasExt(phys, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
        VkPhysicalDevicePipelineRobustnessFeaturesEXT hasRob { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT };
        VkPhysicalDeviceFeatures2 has { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, robustExt ? &hasRob : nullptr };
        vkGetPhysicalDeviceFeatures2(phys, &has);
        robust = robustExt && hasRob.pipelineRobustness;

        // As the runtime's own device (nrvk.hpp), less what this tool does not use.
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
        VkPhysicalDeviceVulkan11Features f11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features f12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features f13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wml { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR };
        VkPhysicalDevicePipelineRobustnessFeaturesEXT rob { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT };
        coop.cooperativeMatrix = VK_TRUE;
        f11.storageBuffer16BitAccess = VK_TRUE;
        f12.storageBuffer8BitAccess = f12.shaderFloat16 = f12.shaderInt8 = f12.vulkanMemoryModel = VK_TRUE;
        f12.vulkanMemoryModelDeviceScope = VK_TRUE;
        f13.subgroupSizeControl = f13.synchronization2 = VK_TRUE;
        wml.workgroupMemoryExplicitLayout = wml.workgroupMemoryExplicitLayout8BitAccess =
            wml.workgroupMemoryExplicitLayout16BitAccess = VK_TRUE;
        rob.pipelineRobustness = VK_TRUE;
        coop.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13; f13.pNext = &wml;
        if (robust) wml.pNext = &rob;
        VkPhysicalDeviceFeatures2 f2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &coop };
        f2.features.shaderInt16 = VK_TRUE;
        std::vector<const char*> ext = { VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME };
        if (robust) ext.push_back(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
        float prio = 1.f;
        VkDeviceQueueCreateInfo qi { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &prio;
        VkDeviceCreateInfo di { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2 };
        di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = uint32_t(ext.size()); di.ppEnabledExtensionNames = ext.data();
        if (VkResult r = vkCreateDevice(phys, &di, nullptr, &dev)) { err = "vkCreateDevice " + std::to_string(r); return false; }
        vkGetDeviceQueue(dev, family, 0, &queue);
        VkCommandPoolCreateInfo pi { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = family;
        vkCreateCommandPool(dev, &pi, nullptr, &pool);
        VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &ai, &cmd);
        VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence(dev, &fi, nullptr, &fence);
        return true;
    }

    bool Alloc(Buf& out, uint64_t size, bool host)
    {
        out.size = size;
        VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = std::max<uint64_t>(size, 16);
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(dev, &bi, nullptr, &out.b)) { err = "vkCreateBuffer"; return false; }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, out.b, &req);
        // Host buffers are read back by the processor: cached system memory first (an uncached or write-combined
        // type makes every hash and compare crawl), then any coherent host-visible type.
        const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = ~0u;
        for (int pass = 0; pass < 2 && type == ~0u; ++pass)
            for (uint32_t i = 0; i < mem.memoryTypeCount && type == ~0u; ++i)
            {
                const auto f = mem.memoryTypes[i].propertyFlags;
                if (!(req.memoryTypeBits & (1u << i))) continue;
                if (!host) { if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) type = i; continue; }
                if ((f & hv) != hv) continue;
                if (pass == 0 && (!(f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) || (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) continue;
                type = i;
            }
        if (type == ~0u) { err = "no memory type"; return false; }
        VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size; ai.memoryTypeIndex = type;
        if (VkResult r = vkAllocateMemory(dev, &ai, nullptr, &out.m)) { err = "vkAllocateMemory " + std::to_string(size >> 20) + " MB: " + std::to_string(r); return false; }
        vkBindBufferMemory(dev, out.b, out.m, 0);
        if (host) vkMapMemory(dev, out.m, 0, VK_WHOLE_SIZE, 0, &out.map);
        return true;
    }

    void Free(Buf& b)
    {
        if (b.map) vkUnmapMemory(dev, b.m);
        if (b.b) vkDestroyBuffer(dev, b.b, nullptr);
        if (b.m) vkFreeMemory(dev, b.m, nullptr);
        b = {};
    }

    void Begin()
    {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
    }

    bool Submit()
    {
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        vkResetFences(dev, 1, &fence);
        if (VkResult r = vkQueueSubmit(queue, 1, &si, fence)) { err = "vkQueueSubmit " + std::to_string(r); return false; }
        if (VkResult r = vkWaitForFences(dev, 1, &fence, VK_TRUE, waitNs)) { err = "vkWaitForFences " + std::to_string(r) + " (device lost or a 20 s hang)"; return false; }
        return true;
    }

    void Barrier()
    {
        VkMemoryBarrier mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    // Host bytes into a device buffer through a staging buffer, in 64 MB pieces.
    bool Upload(Buf& dst, uint64_t dstOff, const uint8_t* src, uint64_t n)
    {
        const uint64_t piece = 64ull << 20;
        Buf st;
        if (!Alloc(st, std::min(n, piece), true)) return false;
        for (uint64_t o = 0; o < n; o += piece)
        {
            const uint64_t k = std::min(piece, n - o);
            std::memcpy(st.map, src + o, size_t(k));
            Begin();
            VkBufferCopy c { 0, dstOff + o, k };
            vkCmdCopyBuffer(cmd, st.b, dst.b, 1, &c);
            if (!Submit()) { Free(st); return false; }
        }
        Free(st);
        return true;
    }

    bool Download(Buf& src, uint64_t off, uint8_t* dst, uint64_t n)
    {
        const uint64_t piece = 64ull << 20;
        Buf st;
        if (!Alloc(st, std::min(n, piece), true)) return false;
        for (uint64_t o = 0; o < n; o += piece)
        {
            const uint64_t k = std::min(piece, n - o);
            Begin();
            VkBufferCopy c { off + o, 0, k };
            vkCmdCopyBuffer(cmd, src.b, st.b, 1, &c);
            if (!Submit()) { Free(st); return false; }
            std::memcpy(dst + o, st.map, size_t(k));
        }
        Free(st);
        return true;
    }
};

// The dispatch: set layout over the case's bindings (storage buffers), its push range, the SPIR-V; wave32 and robust
// buffer access off, as the runtime makes it (nrvk.hpp make_pipeline).
struct Kernel
{
    VkDescriptorSetLayout dsl {};
    VkPipelineLayout layout {};
    VkShaderModule module {};
    VkPipeline pipe {};
    VkDescriptorPool dp {};
    VkDescriptorSet set {};
    double compileMs = 0;

    bool Make(Gpu& g, const Case& c, const fs::path& spv, std::vector<Gpu::Buf>& bufs)
    {
        const std::vector<uint8_t> code = ReadFile(spv);
        if (code.size() < 20 || code.size() % 4) { g.err = "unreadable SPIR-V " + spv.string(); return false; }
        std::vector<VkDescriptorSetLayoutBinding> lb;
        for (const auto& b : c.bindings) lb.push_back({ b.first, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr });
        VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = uint32_t(lb.size()); li.pBindings = lb.data();
        vkCreateDescriptorSetLayout(g.dev, &li, nullptr, &dsl);
        VkPushConstantRange pcr { VK_SHADER_STAGE_COMPUTE_BIT, 0, c.pushRange };
        VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1; pli.pSetLayouts = &dsl;
        pli.pushConstantRangeCount = c.pushRange ? 1 : 0; pli.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(g.dev, &pli, nullptr, &layout);
        VkShaderModuleCreateInfo smi { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smi.codeSize = code.size(); smi.pCode = reinterpret_cast<const uint32_t*>(code.data());
        if (VkResult r = vkCreateShaderModule(g.dev, &smi, nullptr, &module)) { g.err = "vkCreateShaderModule " + std::to_string(r); return false; }
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO };
        req.requiredSubgroupSize = 32;
        VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &req };
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = module; cpi.stage.pName = "main";
        cpi.layout = layout;
        VkPipelineRobustnessCreateInfoEXT robustness { VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT };
        robustness.storageBuffers = robustness.uniformBuffers = robustness.vertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT;
        robustness.images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DISABLED_EXT;
        if (g.robust) cpi.pNext = &robustness;
        const auto t0 = std::chrono::steady_clock::now();
        const VkResult r = vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe);
        compileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (r) { g.err = "vkCreateComputePipelines VkResult " + std::to_string(r); return false; }
        VkDescriptorPoolSize ps { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, uint32_t(lb.size()) };
        VkDescriptorPoolCreateInfo dpi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
        vkCreateDescriptorPool(g.dev, &dpi, nullptr, &dp);
        VkDescriptorSetAllocateInfo dai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
        vkAllocateDescriptorSets(g.dev, &dai, &set);
        std::vector<VkDescriptorBufferInfo> bi(c.bindings.size());
        std::vector<VkWriteDescriptorSet> w(c.bindings.size());
        for (size_t i = 0; i < c.bindings.size(); ++i)
        {
            bi[i] = { bufs[c.bindings[i].second].b, 0, VK_WHOLE_SIZE };
            w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, c.bindings[i].first, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi[i], nullptr };
        }
        vkUpdateDescriptorSets(g.dev, uint32_t(w.size()), w.data(), 0, nullptr);
        return true;
    }

    void Record(Gpu& g, const Case& c)
    {
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        if (c.pushRange)
        {
            std::vector<uint32_t> p(c.push);
            p.resize((c.pushRange + 3) / 4, 0);
            vkCmdPushConstants(g.cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, c.pushRange, p.data());
        }
        vkCmdDispatch(g.cmd, c.gx, c.gy, c.gz);
    }
};

// The scrub (the runtime's seq_scrub.comp, kit 4's MZ_SEQ scrub/zero variants): 1024 workgroups of 256 lanes fill
// their 64 KB of LDS and ~96 VGPRs a lane with a pattern (or zeros), so the dispatch after it starts from that.
struct Scrub
{
    VkDescriptorSetLayout dsl {};
    VkPipelineLayout layout {};
    VkPipeline pipe {};
    VkDescriptorPool dp {};
    VkDescriptorSet set {};
    VkShaderModule module {};
    Gpu::Buf sink;

    bool Make(Gpu& g)
    {
        if (!g.Alloc(sink, 256, false)) return false;
        VkDescriptorSetLayoutBinding lb { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1; li.pBindings = &lb;
        vkCreateDescriptorSetLayout(g.dev, &li, nullptr, &dsl);
        VkPushConstantRange pcr { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
        VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(g.dev, &pli, nullptr, &layout);
        VkShaderModuleCreateInfo smi { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smi.codeSize = sizeof kScrubSpv; smi.pCode = kScrubSpv;
        if (vkCreateShaderModule(g.dev, &smi, nullptr, &module)) { g.err = "scrub module"; return false; }
        VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr };
        cpi.layout = layout;
        if (vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe)) { g.err = "scrub pipeline"; return false; }
        VkDescriptorPoolSize ps { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
        VkDescriptorPoolCreateInfo dpi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
        vkCreateDescriptorPool(g.dev, &dpi, nullptr, &dp);
        VkDescriptorSetAllocateInfo dai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
        vkAllocateDescriptorSets(g.dev, &dai, &set);
        VkDescriptorBufferInfo bi { sink.b, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi, nullptr };
        vkUpdateDescriptorSets(g.dev, 1, &w, 0, nullptr);
        return true;
    }

    void Record(Gpu& g, uint32_t seed, bool zero)
    {
        const uint32_t push[2] = { seed, zero ? 1u : 0u };
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(g.cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        vkCmdDispatch(g.cmd, 1024, 1, 1);
        vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);
    }
};

static std::string Hex(uint64_t h)
{
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}

// ---- one case, n runs -------------------------------------------------------------------------------------------

struct Stats { uint64_t n = 0, equal = 0, nan = 0; double sumAbs = 0, maxAbs = 0, sumRef = 0; };

static Stats Compare(const std::string& kind, const uint8_t* a, const uint8_t* r, uint64_t bytes)
{
    Stats s;
    const int w = kind == "f32" ? 4 : kind == "f16" ? 2 : 1;
    for (uint64_t i = 0; i + w <= bytes; i += w)
    {
        float x, y;
        if (w == 4) { std::memcpy(&x, a + i, 4); std::memcpy(&y, r + i, 4); }
        else if (w == 2) { uint16_t p, q; std::memcpy(&p, a + i, 2); std::memcpy(&q, r + i, 2); x = F16(p); y = F16(q); }
        else { x = E4M3(a[i]); y = E4M3(r[i]); }
        ++s.n;
        if (!std::isfinite(x) || !std::isfinite(y)) { if (std::isfinite(x) != std::isfinite(y)) ++s.nan; else ++s.equal; continue; }
        if (x == y) ++s.equal;
        const double d = std::fabs(double(x) - double(y));
        s.sumAbs += d; s.sumRef += std::fabs(double(y));
        s.maxAbs = std::max(s.maxAbs, d);
    }
    return s;
}

static int RunCase(const fs::path& dir, const fs::path& spv, int n, const std::string& fill, const fs::path& outFile)
{
    FILE* o = _wfopen(outFile.wstring().c_str(), L"w");
    if (!o) return 3;
    auto say = [&](const char* fmt, auto... a) { std::fprintf(o, fmt, a...); std::fflush(o); };
    Case c;
    std::string err;
    if (!LoadCase(dir / "case.txt", c, err)) { say("FAIL %s\n", err.c_str()); std::fclose(o); return 2; }
    const std::vector<uint8_t> packed = ReadFile(dir / "data.bin");
    std::vector<uint8_t> data;
    for (auto& sl : c.slices)
    {
        const uint64_t at = data.size();
        if (sl.e4m3)
        {
            if (sl.at + sl.bytes / 2 > packed.size()) { say("FAIL data.bin too short\n"); std::fclose(o); return 2; }
            data.resize(at + sl.bytes);
            for (uint64_t j = 0; j < sl.bytes / 2; ++j)
            {
                const uint16_t h = E4M3ToF16(packed[sl.at + j]);
                std::memcpy(&data[at + 2 * j], &h, 2);
            }
        }
        else
        {
            if (sl.at + sl.bytes > packed.size()) { say("FAIL data.bin too short\n"); std::fclose(o); return 2; }
            data.insert(data.end(), packed.begin() + sl.at, packed.begin() + sl.at + sl.bytes);
        }
        sl.at = at;
    }
    Gpu g;
    if (!g.Init()) { say("FAIL %s\n", g.err.c_str()); std::fclose(o); return 2; }
    say("DEVICE %s | %s %s\n", g.props.deviceName, g.drv.driverName, g.drv.driverInfo);
    std::vector<Gpu::Buf> bufs(c.bufSize.size());
    for (size_t i = 0; i < bufs.size(); ++i)
        if (!g.Alloc(bufs[i], c.bufSize[i], false)) { say("FAIL %s\n", g.err.c_str()); std::fclose(o); return 2; }
    Gpu::Buf pristine, readback;
    uint64_t outBytes = 0;
    for (const auto& x : c.outs) outBytes += x.bytes;
    if (!g.Alloc(pristine, std::max<uint64_t>(data.size(), 16), false) || !g.Alloc(readback, std::max<uint64_t>(outBytes, 16), true) ||
        (!data.empty() && !g.Upload(pristine, 0, data.data(), data.size())))
    {
        say("FAIL %s\n", g.err.c_str()); std::fclose(o); return 2;
    }
    // fill: zero | rand, then optionally +dirty / +clean (the scrub before each run).
    const size_t plus = fill.find('+');
    const std::string bufFill = fill.substr(0, plus), scrubMode = plus == std::string::npos ? "" : fill.substr(plus + 1);
    Scrub scrub;
    if (!scrubMode.empty() && !scrub.Make(g)) { say("FAIL %s\n", g.err.c_str()); std::fclose(o); return 2; }
    Kernel k;
    say("COMPILE start\n");
    if (!k.Make(g, c, spv, bufs)) { say("COMPILE FAIL %s\n", g.err.c_str()); std::fclose(o); return 1; }
    say("COMPILE OK %.0f ms\n", k.compileMs);

    const int kChunks = 16;
    std::vector<std::map<uint64_t, int>> seen(c.outs.size());
    std::vector<std::vector<std::set<uint64_t>>> chunkSeen(c.outs.size(), std::vector<std::set<uint64_t>>(kChunks));
    std::vector<std::vector<uint8_t>> first(c.outs.size());
    std::vector<uint64_t> diffBytes(c.outs.size(), 0);
    double gpuMs = 0;
    for (int it = 0; it < n; ++it)
    {
        g.Begin();
        // What nobody should read: zeros, or (fill rand) a different pattern every run.
        uint32_t pat = 0;
        if (bufFill == "rand") { pat = 0x9E3779B9u * uint32_t(it + 1); pat ^= pat >> 15; pat *= 0x85EBCA6Bu; pat ^= pat >> 13; }
        for (auto& b : bufs) vkCmdFillBuffer(g.cmd, b.b, 0, VK_WHOLE_SIZE, pat);
        g.Barrier();
        for (const auto& s : c.slices)
        {
            VkBufferCopy cp { s.at, s.off, s.bytes };
            vkCmdCopyBuffer(g.cmd, pristine.b, bufs[s.buf].b, 1, &cp);
        }
        g.Barrier();
        if (!scrubMode.empty()) scrub.Record(g, 7919u * uint32_t(it + 1) + 17u, scrubMode == "clean");
        k.Record(g, c);
        g.Barrier();
        uint64_t at = 0;
        for (const auto& x : c.outs)
        {
            VkBufferCopy cp { x.off, at, x.bytes };
            vkCmdCopyBuffer(g.cmd, bufs[x.buf].b, readback.b, 1, &cp);
            at += x.bytes;
        }
        const auto t0 = std::chrono::steady_clock::now();
        if (!g.Submit()) { say("RUNFAIL at run %d: %s\n", it + 1, g.err.c_str()); std::fclose(o); return 4; }
        gpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        at = 0;
        for (size_t i = 0; i < c.outs.size(); ++i)
        {
            const auto& x = c.outs[i];
            const uint8_t* p = static_cast<const uint8_t*>(readback.map) + at;
            ++seen[i][Hash(p, size_t(x.bytes))];
            const uint64_t cb = (x.bytes + kChunks - 1) / kChunks;
            for (int q = 0; q < kChunks; ++q)
            {
                const uint64_t a = std::min<uint64_t>(x.bytes, q * cb), e = std::min<uint64_t>(x.bytes, a + cb);
                chunkSeen[i][q].insert(Hash(p + a, size_t(e - a)));
            }
            if (it == 0) first[i].assign(p, p + x.bytes);
            else
                for (uint64_t b = 0; b < x.bytes; ++b) diffBytes[i] += p[b] != first[i][b];
            at += x.bytes;
        }
    }
    say("RUNS %d fill %s, %.2f ms a run (fill, copies, dispatch, readback)\n", n, fill.c_str(), gpuMs / n);
    bool allSame = true;
    for (size_t i = 0; i < c.outs.size(); ++i)
    {
        const auto& x = c.outs[i];
        std::string chunks;
        for (int q = 0; q < kChunks; ++q)
            if (chunkSeen[i][q].size() > 1) chunks += (chunks.empty() ? "" : ",") + std::to_string(q);
        std::vector<std::pair<int, uint64_t>> by;
        for (const auto& h : seen[i]) by.push_back({ h.second, h.first });
        std::sort(by.rbegin(), by.rend());
        std::string hs;
        for (size_t j = 0; j < by.size() && j < 6; ++j) hs += (j ? " " : "") + Hex(by[j].second) + "x" + std::to_string(by[j].first);
        if (by.size() > 6) hs += " ...";
        allSame &= seen[i].size() == 1;
        say("OUT %s %s bytes %llu distinct %zu chunks_changing %s changed_bytes_vs_run1 %llu hashes %s\n", x.label.c_str(),
            x.kind.c_str(), (unsigned long long)x.bytes, seen[i].size(), chunks.empty() ? "-" : chunks.c_str(),
            (unsigned long long)diffBytes[i], hs.c_str());
        if (!x.ref.empty())
        {
            std::vector<uint8_t> ref = ReadFile(dir / x.ref);
            if (x.refEnc == "e4m3")
            {
                std::vector<uint8_t> f(ref.size() * 2);
                for (size_t j = 0; j < ref.size(); ++j) { const uint16_t h = E4M3ToF16(ref[j]); std::memcpy(&f[2 * j], &h, 2); }
                ref.swap(f);
            }
            if (ref.size() != x.bytes) say("REF %s missing or wrong size\n", x.label.c_str());
            else
            {
                const Stats s = Compare(x.kind, first[i].data(), ref.data(), x.bytes);
                say("REF %s equal %.2f%% mean_abs %.6g max_abs %.6g mean_ref %.6g rel %.4f nonfinite_mismatch %llu\n",
                    x.label.c_str(), 100.0 * s.equal / std::max<uint64_t>(s.n, 1), s.sumAbs / std::max<uint64_t>(s.n, 1), s.maxAbs,
                    s.sumRef / std::max<uint64_t>(s.n, 1), s.sumAbs / std::max(s.sumRef, 1e-30), (unsigned long long)s.nan);
            }
        }
    }
    say("RESULT %s\n", allSame ? "DETERMINISTIC" : "CHANGES");
    std::fclose(o);
    vkDeviceWaitIdle(g.dev);
    return 0;
}

// ---- here: the read and write sets of a capture ------------------------------------------------------------------

struct Range { uint32_t buf; uint64_t off, bytes; };

// The storage-buffer bindings of a module that are not read-only (no NonWritable on the variable or its block's
// members).
static std::set<uint32_t> WritableBindings(const std::vector<uint8_t>& bytes)
{
    std::vector<uint32_t> c(bytes.size() / 4);
    std::memcpy(c.data(), bytes.data(), c.size() * 4);
    std::map<uint32_t, uint32_t> binding, varType, ptrType;
    std::set<uint32_t> nonWritable, nonWritableMember, storageVars;
    for (size_t i = 5; i < c.size();)
    {
        const uint32_t op = c[i] & 0xFFFFu, wc = c[i] >> 16;
        if (!wc || i + wc > c.size()) break;
        if (op == 71 && wc >= 4 && c[i + 2] == 33) binding[c[i + 1]] = c[i + 3];
        else if (op == 71 && wc >= 3 && c[i + 2] == 24) nonWritable.insert(c[i + 1]);
        else if (op == 72 && wc >= 4 && c[i + 3] == 24) nonWritableMember.insert(c[i + 1]);
        else if (op == 32 && wc == 4) ptrType[c[i + 1]] = c[i + 3];
        else if (op == 59 && wc >= 4 && (c[i + 3] == 12 || c[i + 3] == 2)) { varType[c[i + 2]] = c[i + 1]; storageVars.insert(c[i + 2]); }
        i += wc;
    }
    std::set<uint32_t> out;
    for (uint32_t v : storageVars)
    {
        const auto b = binding.find(v);
        if (b == binding.end()) continue;
        const uint32_t block = ptrType[varType[v]];
        if (!nonWritable.count(v) && !nonWritableMember.count(block)) out.insert(b->second);
    }
    return out;
}

struct Full
{
    Gpu g;
    Case c;
    std::vector<std::vector<uint8_t>> before;
    std::vector<Gpu::Buf> work, pristine;
    Kernel k;
    std::vector<Range> writes;
    std::vector<uint8_t> answer;   // the write set's bytes after one run on the whole capture
    Gpu::Buf rb;

    bool Load(const fs::path& dir, const fs::path& spv)
    {
        std::string err;
        if (!LoadCase(dir / "capture.txt", c, err)) { std::printf("%s\n", err.c_str()); return false; }
        if (!g.Init()) { std::printf("%s\n", g.err.c_str()); return false; }
        // The buffers a binding can write (no NonWritable): only these are poisoned with the second pattern or in
        // the write probe. The weight buffers hold the tile-chain records and tables, which must stay sane.
        const std::set<uint32_t> wb = WritableBindings(ReadFile(spv));
        for (const auto& b : c.bindings)
            if (wb.count(b.first)) written.insert(b.second);
        before.resize(c.bufSize.size());
        work.resize(c.bufSize.size());
        pristine.resize(c.bufSize.size());
        for (size_t i = 0; i < c.bufSize.size(); ++i)
        {
            before[i] = ReadFile(dir / ("buf" + std::to_string(i) + ".bin"));
            if (before[i].size() != c.bufSize[i]) { std::printf("buf%zu.bin: wrong size\n", i); return false; }
            if (!g.Alloc(work[i], c.bufSize[i], false) || !g.Alloc(pristine[i], c.bufSize[i], false) ||
                !g.Upload(pristine[i], 0, before[i].data(), before[i].size()))
            {
                std::printf("%s\n", g.err.c_str()); return false;
            }
        }
        if (!k.Make(g, c, spv, work)) { std::printf("%s\n", g.err.c_str()); return false; }
        return true;
    }

    void Restore(const std::vector<Range>& r)
    {
        for (const auto& x : r)
        {
            VkBufferCopy cp { x.off, x.off, x.bytes };
            vkCmdCopyBuffer(g.cmd, pristine[x.buf].b, work[x.buf].b, 1, &cp);
        }
    }

    // Byte ranges where `a` and `b` differ (exactly, no merging across equal bytes).
    static void Diff(uint32_t buf, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, std::vector<Range>& out)
    {
        uint64_t i = 0;
        while (i < a.size())
        {
            if (a[i] == b[i]) { ++i; continue; }
            uint64_t e = i + 1;
            while (e < a.size() && a[e] != b[e]) ++e;
            out.push_back({ buf, i, e - i });
            i = e;
        }
    }

    static std::vector<Range> Merge(std::vector<Range> r, uint64_t gap)
    {
        std::sort(r.begin(), r.end(), [](const Range& a, const Range& b) { return a.buf != b.buf ? a.buf < b.buf : a.off < b.off; });
        std::vector<Range> m;
        for (const auto& x : r)
            if (!m.empty() && m.back().buf == x.buf && x.off <= m.back().off + m.back().bytes + gap)
                m.back().bytes = std::max(m.back().off + m.back().bytes, x.off + x.bytes) - m.back().off;
            else m.push_back(x);
        return m;
    }

    // First guess at the write set: what one run on the whole capture changes. It misses bytes the dispatch writes
    // with the value they already had (a slot the same value held in the frame before).
    bool FirstWrites()
    {
        g.Begin();
        for (size_t i = 0; i < work.size(); ++i) { VkBufferCopy cp { 0, 0, c.bufSize[i] }; vkCmdCopyBuffer(g.cmd, pristine[i].b, work[i].b, 1, &cp); }
        g.Barrier();
        k.Record(g, c);
        if (!g.Submit()) return false;
        std::vector<Range> w;
        for (size_t i = 0; i < work.size(); ++i)
        {
            std::vector<uint8_t> after(c.bufSize[i]);
            if (!g.Download(work[i], 0, after.data(), after.size())) return false;
            Diff(uint32_t(i), after, before[i], w);
        }
        writes = Merge(w, 0);
        return Answer();
    }

    // The write set again with everything but `reads` poisoned, twice: every byte that does not keep what was put
    // there was written.
    bool ProbeWrites(const std::vector<Range>& reads)
    {
        std::vector<Range> w(writes);
        for (uint32_t pat : { 0xFFFFFFFFu, 0x3C5A3C5Au })
        {
            g.Begin();
            for (size_t i = 0; i < work.size(); ++i)
                if (Written(uint32_t(i))) vkCmdFillBuffer(g.cmd, work[i].b, 0, VK_WHOLE_SIZE, pat);
                else { VkBufferCopy cp { 0, 0, c.bufSize[i] }; vkCmdCopyBuffer(g.cmd, pristine[i].b, work[i].b, 1, &cp); }
            g.Barrier();
            Restore(reads);
            g.Barrier();
            k.Record(g, c);
            if (!g.Submit()) return false;
            for (size_t i = 0; i < work.size(); ++i)
            {
                if (!Written(uint32_t(i))) continue;
                std::vector<uint8_t> expect(c.bufSize[i]), after(c.bufSize[i]);
                for (size_t j = 0; j + 4 <= expect.size(); j += 4) std::memcpy(&expect[j], &pat, 4);
                for (size_t j = expect.size() & ~size_t(3); j < expect.size(); ++j) expect[j] = uint8_t(pat >> (8 * (j & 3)));
                for (const auto& r : reads)
                    if (r.buf == i) std::memcpy(&expect[r.off], &before[i][r.off], size_t(r.bytes));
                if (!g.Download(work[i], 0, after.data(), after.size())) return false;
                Diff(uint32_t(i), after, expect, w);
            }
        }
        writes = Merge(w, 0);
        return Answer();
    }

    std::set<uint32_t> written;   // the buffers the first run changed: only these get the second poison pattern
    bool Written(uint32_t b) const { return written.count(b) != 0; }

    // The write set's bytes after one run on the whole capture.
    bool Answer()
    {
        uint64_t total = 0;
        for (const auto& w : writes) total += w.bytes;
        if (rb.b) g.Free(rb);
        if (!g.Alloc(rb, std::max<uint64_t>(total, 16), true)) return false;
        g.Begin();
        for (size_t i = 0; i < work.size(); ++i) { VkBufferCopy cp { 0, 0, c.bufSize[i] }; vkCmdCopyBuffer(g.cmd, pristine[i].b, work[i].b, 1, &cp); }
        g.Barrier();
        k.Record(g, c);
        g.Barrier();
        uint64_t at = 0;
        for (const auto& w : writes) { VkBufferCopy cp { w.off, at, w.bytes }; vkCmdCopyBuffer(g.cmd, work[w.buf].b, rb.b, 1, &cp); at += w.bytes; }
        if (!g.Submit()) return false;
        answer.assign(static_cast<const uint8_t*>(rb.map), static_cast<const uint8_t*>(rb.map) + total);
        return true;
    }

    // Poison `p` with `pat`, run, and say whether the write set came out different.
    int Test(const std::vector<Range>& p, uint32_t pat)
    {
        g.Begin();
        Restore(writes);
        g.Barrier();
        for (const auto& x : p) vkCmdFillBuffer(g.cmd, work[x.buf].b, x.off, x.bytes, pat);
        g.Barrier();
        k.Record(g, c);
        g.Barrier();
        uint64_t at = 0;
        for (const auto& w : writes) { VkBufferCopy cp { w.off, at, w.bytes }; vkCmdCopyBuffer(g.cmd, work[w.buf].b, rb.b, 1, &cp); at += w.bytes; }
        g.Barrier();
        Restore(p);
        if (!g.Submit())
        {
            if (g.err.find("vkWaitForFences 2 ") == std::string::npos) return -1;
            vkDeviceWaitIdle(g.dev);
            ++timeouts;
            return 1;
        }
        return std::memcmp(rb.map, answer.data(), answer.size()) != 0;
    }

    // The pages whose poisoning changes the answer (either of two patterns), halving from whole buffers.
    bool Reads(std::vector<Range>& out, int& tests)
    {
        const uint64_t page = 65536;   // what a case carries is whole 64 KB pages of what the dispatch reads
        std::vector<Range> reads;
        const auto t0 = std::chrono::steady_clock::now();
        for (uint32_t b = 0; b < c.bufSize.size(); ++b)
        {
            std::vector<Range> todo { { b, 0, c.bufSize[b] & ~uint64_t(3) } };
            while (!todo.empty())
            {
                const Range r = todo.back();
                todo.pop_back();
                bool hit = false;
                for (uint32_t pat : kPats)
                {
                    if (pat != kPats[0] && !Written(b)) break;   // weights: NaN only (a record or table must stay ~0)
                    const int t = Test({ r }, pat);
                    if (++tests % 500 == 0)
                        std::printf("  %d tests, %.0f s\n", tests, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                    if (t < 0) return false;
                    if (t) { hit = true; break; }
                }
                if (!hit) continue;
                if (r.bytes <= page) { reads.push_back(r); continue; }
                const uint64_t half = ((r.bytes / 2) + page - 1) / page * page;
                todo.push_back({ b, r.off + half, r.bytes - half });
                todo.push_back({ b, r.off, half });
            }
        }
        out = Merge(reads, 0);
        return true;
    }
    static constexpr uint32_t kPats[2] = { 0xFFFFFFFFu, 0x3C5A3C5Au };
    int timeouts = 0;
};

static int ReadSet(const fs::path& dir, const fs::path& spv, const fs::path& out)
{
    Full f;
    std::vector<Range> r0, r1;
    int tests = 0;
    f.g.waitNs = 3000000000ull;   // a poisoned tile-chain record can make workgroups wait: a hit, not a hang
    const auto t0 = std::chrono::steady_clock::now();
    auto lap = [&](const char* what) {
        std::printf("  %s at %.1f s\n", what, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        return true;
    };
    if (!f.Load(dir, spv) || !lap("loaded") || !f.FirstWrites() || !lap("first writes") || !f.Reads(r0, tests) ||
        !lap("reads") || !f.ProbeWrites(r0) || !lap("write probe") || !f.Reads(r1, tests) || !lap("reads again"))
    {
        std::printf("FAIL %s\n", f.g.err.c_str());
        return 1;
    }
    // Once more if the second read set found more inputs: their outputs may have been missed too.
    if (r1.size() != r0.size() || !std::equal(r1.begin(), r1.end(), r0.begin(), [](const Range& a, const Range& b) { return a.buf == b.buf && a.off == b.off && a.bytes == b.bytes; }))
    {
        r0 = r1;
        if (!f.ProbeWrites(r0) || !f.Reads(r1, tests)) { std::printf("FAIL %s\n", f.g.err.c_str()); return 1; }
    }
    // Check: everything but the read set poisoned (both patterns) gives the same answer.
    std::vector<Range> rest;
    for (uint32_t b = 0; b < f.c.bufSize.size(); ++b)
    {
        uint64_t at = 0;
        for (const auto& r : r1)
            if (r.buf == b) { if (r.off > at) rest.push_back({ b, at, r.off - at }); at = r.off + r.bytes; }
        const uint64_t end = f.c.bufSize[b] & ~uint64_t(3);
        if (end > at) rest.push_back({ b, at, end - at });
    }
    bool ok = true;
    for (uint32_t pat : Full::kPats)
    {
        std::vector<Range> r;
        for (const auto& x : rest)
            if (pat == Full::kPats[0] || f.Written(x.buf)) r.push_back(x);
        ok &= f.Test(r, pat) == 0;
    }
    FILE* o = _wfopen(out.wstring().c_str(), L"w");
    for (const auto& w : f.writes) std::fprintf(o, "write %u %llu %llu\n", w.buf, (unsigned long long)w.off, (unsigned long long)w.bytes);
    for (const auto& r : r1) std::fprintf(o, "read %u %llu %llu\n", r.buf, (unsigned long long)r.off, (unsigned long long)r.bytes);
    std::fprintf(o, "answer %s\n", Hex(Hash(f.answer.data(), f.answer.size())).c_str());
    std::fprintf(o, "timeouts %d\n", f.timeouts);
    std::fprintf(o, "check %s\n", ok ? "OK" : "FAILED");
    std::fclose(o);
    WriteFile(fs::path(out).replace_extension(".answer.bin"), f.answer.data(), f.answer.size());
    std::printf("readset: %zu write ranges, %zu read ranges, %d tests, check %s\n", f.writes.size(), r1.size(), tests, ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}

static int FullRun(const fs::path& dir, const fs::path& spv, const fs::path& out)
{
    Full f;
    if (!f.Load(dir, spv) || !f.FirstWrites()) { std::printf("FAIL %s\n", f.g.err.c_str()); return 1; }
    WriteFile(out, f.answer.data(), f.answer.size());
    std::printf("full: %zu write ranges, %zu bytes, hash %s\n", f.writes.size(), f.answer.size(), Hex(Hash(f.answer.data(), f.answer.size())).c_str());
    return 0;
}

// ---- the kit ----------------------------------------------------------------------------------------------------

static std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t ch : w) s += ch < 128 ? char(ch) : '?';
    return s;
}

static std::vector<std::string> Lines(const fs::path& p)
{
    std::vector<std::string> v;
    std::ifstream f(p);
    std::string l;
    while (std::getline(f, l)) v.push_back(l);
    return v;
}

static std::string Field(const std::string& line, const std::string& key)
{
    const size_t a = line.find(" " + key + " ");
    if (a == std::string::npos) return "";
    size_t b = a + key.size() + 2, e = line.find(' ', b);
    return line.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

struct Child { long code = 0; double ms = 0; std::vector<std::string> out; };

static Child RunOne(const fs::path& caseDir, const fs::path& spv, int n, const char* fill, const fs::path& outFile, DWORD limitMs)
{
    Child c;
    std::error_code ec;
    fs::remove(outFile, ec);
    c.code = RunChild(L"case \"" + caseDir.wstring() + L"\" \"" + spv.wstring() + L"\" " + std::to_wstring(n) + L" " +
                          std::wstring(fill, fill + std::strlen(fill)) + L" \"" + outFile.wstring() + L"\"",
                      limitMs, &c.ms);
    c.out = Lines(outFile);
    return c;
}

static std::string Status(const Child& c)
{
    if (c.code == -1) return "TIMEOUT";
    if (Crashed(c.code) || c.code == -2)
    {
        bool compiled = false;
        for (const auto& l : c.out) compiled |= l.rfind("COMPILE OK", 0) == 0;
        char b[64];
        std::snprintf(b, sizeof b, "%s 0x%08lx", compiled ? "CRASH-RUN" : "CRASH-COMPILE", (unsigned long)c.code);
        return b;
    }
    for (const auto& l : c.out)
    {
        if (l.rfind("COMPILE FAIL", 0) == 0) return "COMPILE-FAIL";
        if (l.rfind("RUNFAIL", 0) == 0) return "RUN-FAIL";
        if (l.rfind("FAIL", 0) == 0) return "FAIL";
        if (l.rfind("RESULT ", 0) == 0) return l.substr(7);
    }
    return "NO-RESULT exit " + std::to_string(c.code);
}

static std::string OutsLine(const Child& c, bool hashes)
{
    std::string s;
    for (const auto& l : c.out)
        if (l.rfind("OUT ", 0) == 0)
        {
            std::istringstream is(l);
            std::string t, label;
            is >> t >> label;
            s += "\n        " + label + ": " + Field(l, "distinct") + " distinct";
            const std::string ch = Field(l, "chunks_changing");
            if (ch != "-") s += ", chunks changing " + ch + ", bytes differing from run 1 (summed) " + Field(l, "changed_bytes_vs_run1");
            if (hashes) { const size_t h = l.find(" hashes "); if (h != std::string::npos) s += ", hashes" + l.substr(h + 7); }
        }
        else if (l.rfind("REF ", 0) == 0)
        {
            std::istringstream is(l);
            std::string t, label;
            is >> t >> label;
            if (l.find("missing") != std::string::npos) continue;
            s += "\n        " + label + " vs RX 9070 XT: " + Field(l, "equal") + " equal, mean |d| " + Field(l, "mean_abs") +
                 " (mean |ref| " + Field(l, "mean_ref") + ", rel " + Field(l, "rel") + "), max |d| " + Field(l, "max_abs");
        }
    return s;
}

static double RefRel(const Child& c)
{
    double worst = -1;
    for (const auto& l : c.out)
        if (l.rfind("REF ", 0) == 0 && l.find("missing") == std::string::npos)
            worst = std::max(worst, std::atof(Field(l, "rel").c_str()));
    return worst;
}

static std::string FirstHashes(const Child& c)
{
    std::string s;
    for (const auto& l : c.out)
        if (l.rfind("OUT ", 0) == 0) { const size_t h = l.find(" hashes "); s += l.substr(h + 8, 16) + ";"; }
    return s;
}

static int Kit(const fs::path& files, const fs::path& res)
{
    const auto t0 = std::chrono::steady_clock::now();
    std::error_code ec;
    fs::create_directories(res / "runs", ec);
    // kit.txt: "case <id> <dir> <runs> <about>", then "variant <name> <spv> <about>" lines (the first three, rel, fix
    // and nat, also run in more processes and with garbage outside the inputs); "processes", "rand_runs", "scrub_runs".
    struct Var { std::string name, spv, about; };
    struct KCase { std::string id, dir, about; int runs = 50; std::vector<Var> vars; };
    std::vector<KCase> cases;
    int procs = 2, randRuns = 20, scrubRuns = 20;
    for (const auto& l : Lines(files / "kit.txt"))
    {
        std::istringstream s(l);
        std::string k;
        s >> k;
        if (k == "case") { KCase c; s >> c.id >> c.dir >> c.runs; std::getline(s, c.about); cases.push_back(c); }
        else if (k == "variant" && !cases.empty()) { Var v; s >> v.name >> v.spv; std::getline(s, v.about); cases.back().vars.push_back(v); }
        else if (k == "processes") s >> procs;
        else if (k == "rand_runs") s >> randRuns;
        else if (k == "scrub_runs") s >> scrubRuns;
    }
    FILE* sum = _wfopen((res / "SUMMARY.txt").wstring().c_str(), L"w");
    std::string detail, inShort;
    auto both = [&](const std::string& t) { std::printf("%s", t.c_str()); detail += t; };
    {
        Gpu g;
        if (g.Init())
            both("INFO vulkan device: " + std::string(g.props.deviceName) + " | " + g.drv.driverName + " " + g.drv.driverInfo +
                 " | driverVersion " + Hex(g.props.driverVersion).substr(8) + "\n");
        else both("INFO no vulkan device: " + g.err + "\n");
        if (g.dev) vkDestroyDevice(g.dev, nullptr);
        if (g.instance) vkDestroyInstance(g.instance, nullptr);
    }
    auto relText = [](double r) { char b[32]; if (r < 0) return std::string("rel -"); std::snprintf(b, sizeof b, "rel %.4f", r); return std::string(b); };
    for (const auto& c : cases)
    {
        const fs::path dir = files / c.dir;
        both("\n== " + c.id + c.about + "\n");
        std::string allDet, rightDet, still, broken, modeDep;
        std::string baseLine, fixLine, natLine;
        for (size_t v = 0; v < c.vars.size(); ++v)
        {
            const Var& x = c.vars[v];
            const bool wide = v < 3;
            const std::string about = x.about.substr(x.about.find_first_not_of(' ') == std::string::npos ? 0 : x.about.find_first_not_of(' '));
            both("  " + x.name + " (" + about + ")\n");
            // zero fill in one or more processes, then the two scrubs, then (wide) garbage outside the inputs.
            struct M { std::string tag, fill, what; int runs; };
            std::vector<M> modes;
            for (int p = 1; p <= (wide ? procs : 1); ++p)
                modes.push_back({ "p" + std::to_string(p), "zero", "zero fill, process " + std::to_string(p), c.runs });
            modes.push_back({ "dirty", "zero+dirty", "garbage left in LDS/registers before each run", scrubRuns });
            modes.push_back({ "clean", "zero+clean", "zeros left in LDS/registers before each run", scrubRuns });
            if (wide) modes.push_back({ "rand", "rand", "new garbage outside its inputs every run", randRuns });
            bool det = true, ran = true, sameAll = true;
            std::string firstH, status, worstRel;
            double rel0 = -1, relMax = -1;
            for (const auto& m : modes)
            {
                const Child r = RunOne(dir, files / x.spv, m.runs, m.fill.c_str(), res / "runs" / (c.id + "_" + x.name + "_" + m.tag + ".txt"), 180000);
                const std::string st = Status(r);
                const double rel = RefRel(r);
                if (st != "DETERMINISTIC" && st != "CHANGES") { ran = false; if (status.empty()) status = st; }
                det &= st == "DETERMINISTIC";
                const std::string h = FirstHashes(r);
                if (firstH.empty()) firstH = h; else if (h != firstH) sameAll = false;
                if (m.tag == "p1") rel0 = rel;
                relMax = std::max(relMax, rel);
                both("    " + m.what + ": " + st + " (" + std::to_string(m.runs) + " runs, " + relText(rel) + ")" + OutsLine(r, m.tag == "p1") + "\n");
                if (!ran) break;
            }
            std::string verdict;
            if (!ran) { verdict = "did not run (" + status + ")"; broken += (broken.empty() ? "" : ", ") + x.name + " " + status; }
            else
            {
                verdict = std::string(det ? "deterministic in every mode" : "CHANGES from run to run") +
                          (sameAll ? ", same answer in every mode" : ", the answer DEPENDS on the mode (what LDS/registers/unread bytes held)") +
                          ", " + relText(rel0) + (relMax > rel0 + 1e-4 ? " (worst mode " + relText(relMax).substr(4) + ")" : "");
                char b[48]; std::snprintf(b, sizeof b, " (%.4f)", rel0);
                if (det && sameAll) { allDet += (allDet.empty() ? "" : ", ") + x.name + b; if (rel0 >= 0 && relMax < 0.05) rightDet += (rightDet.empty() ? "" : ", ") + x.name + b; }
                else if (det) modeDep += (modeDep.empty() ? "" : ", ") + x.name + b;
                else still += (still.empty() ? "" : ", ") + x.name;
            }
            both("    => " + verdict + "\n");
            if (v == 0) baseLine = verdict;
            if (v == 1) fixLine = verdict;
            if (v == 2) natLine = verdict;
        }
        std::string line = "  " + c.id + ":\n      released 0.5.8 (" + c.vars[0].name + "): " + baseLine;
        if (c.vars.size() > 1) line += "\n      kit 6 fix (" + c.vars[1].name + "): " + fixLine;
        if (c.vars.size() > 2) line += "\n      RDNA2 native (" + c.vars[2].name + "): " + natLine;
        line += "\n      deterministic in every mode, same answer: " + (allDet.empty() ? std::string("none") : allDet);
        line += "\n      of those right (rel under 0.05 against the RX 9070 XT): " + (rightDet.empty() ? std::string("none") : rightDet);
        if (!modeDep.empty()) line += "\n      deterministic per mode but the answer depends on the mode: " + modeDep;
        if (!still.empty()) line += "\n      still changing: " + still;
        if (!broken.empty()) line += "\n      did not run: " + broken;
        inShort += line + "\n";
    }
    const double mins = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 60;
    std::fprintf(sum, "OpenNR RX 7000 diagnostic kit 6, one dispatch alone, %.1f min\n\nIN SHORT\n%s\n"
                 "Each case is one dispatch of the RDNA3 network on fixed bytes (its inputs from the RX 9070 XT's FP8\n"
                 "network, 1152x648, small model, clean frame). Every variant runs with zeros outside its inputs, then\n"
                 "with garbage and with zeros left in LDS and registers by a dispatch before it, the released, fixed and\n"
                 "native pipelines also in a second process and with garbage outside their inputs. rel = mean |d| / mean\n"
                 "|ref| against the FP8 network's value of the same key (a right kernel: about 0.01 or less; the native\n"
                 "body of dispatch 11 gives 0.0004 on an RX 9070 XT).\n\nDETAILS\n%s", mins, inShort.c_str(), detail.c_str());
    std::fclose(sum);
    std::printf("\nIN SHORT\n%s\nDone in %.1f min.\n", inShort.c_str(), mins);
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!LoadLibraryW(L"vulkan-1.dll")) { std::printf("no Vulkan loader (vulkan-1.dll)\n"); return 1; }
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    if (mode == L"case" && argc >= 7) return RunCase(argv[2], argv[3], _wtoi(argv[4]), Narrow(argv[5]), argv[6]);
    if (mode == L"readset" && argc >= 5) return ReadSet(argv[2], argv[3], argv[4]);
    if (mode == L"full" && argc >= 5) return FullRun(argv[2], argv[3], argv[4]);
    if (mode == L"kit" && argc >= 4) return Kit(argv[2], argv[3]);
    std::printf("kreplay kit <files> <result> | case <dir> <spv> <n> <zero|rand> <out> | readset <capture> <spv> <out> | full <capture> <spv> <out.bin>\n");
    return 1;
}
