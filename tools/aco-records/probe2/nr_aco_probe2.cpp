// nr_aco_probe2: which pipeline-binary container does this AMD driver hand out and take back?
//
//   nr_aco_probe2 capture  <files> <out>    the driver's own binaries: export, describe, re-import (safe)
//   nr_aco_probe2 variants <files> <out>    our ACO records in that container and in variations of it
//
// <files>: probe_a.spv, probe_b.spv (dispatched, checked against the CPU), net\*.spv (network kernels: compiled,
// exported and re-imported, never dispatched), records\<machine>\<fnv64>.nrp, ref-gfx12\<probe>.bin (an RDNA4
// driver's container of each probe, the base of the synthetic variants).
// <out>: report-<phase>.txt, blobs\ (the driver's binaries and keys), info\ (driver statistics, disassembly),
// variants\ (the containers we built). Reads and writes only its own buffers and files.
#define NOMINMAX
#include "nr_pipeline_binary.hpp"
#include "pal_probe.hpp"
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <random>

namespace fs = std::filesystem;
using nr::pal::Bytes; using nr::probe::fmt; using nr::probe::hex;
static FILE* g_report = nullptr;
static void say(const std::string& s) {
    std::printf("%s\n", s.c_str());
    if (g_report) { std::fprintf(g_report, "%s\n", s.c_str()); std::fflush(g_report); }
}
static void save(const fs::path& p, const void* d, size_t n) {
    std::error_code ec; fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary); f.write(static_cast<const char*>(d), std::streamsize(n));
}
static void save(const fs::path& p, const Bytes& b) { save(p, b.data(), b.size()); }
static void save(const fs::path& p, const std::string& s) { save(p, s.data(), s.size()); }
static std::vector<uint32_t> read_words(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate); if (!f) return {};
    std::vector<uint32_t> w(size_t(f.tellg()) / 4); f.seekg(0); f.read(reinterpret_cast<char*>(w.data()), std::streamsize(w.size() * 4));
    return w;
}
static double ms_since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); }

struct Gpu {
    VkInstance inst{}; VkPhysicalDevice phys{}; VkDevice dev{}; VkQueue q{}; uint32_t family = 0, device_id = 0;
    VkPhysicalDeviceMemoryProperties mem{}; bool binaries = false, exec_info = false, shader_info = false;
    std::string driver;
    bool init(bool info) {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "nr_aco_probe2"; app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ii.pApplicationInfo = &app;
        if (vkCreateInstance(&ii, nullptr, &inst)) { say("FAIL vkCreateInstance"); return false; }
        uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
        std::vector<VkPhysicalDevice> all(n); vkEnumeratePhysicalDevices(inst, &n, all.data());
        for (int pass = 0; pass < 2 && !phys; ++pass)
            for (auto p : all) {
                VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(p, &pr);
                if (pr.vendorID == 0x1002 && (pass || pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) { phys = p; break; }
            }
        if (!phys) { say("FAIL no AMD Vulkan device"); return false; }
        VkPhysicalDevicePipelineBinaryPropertiesKHR pbp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_PROPERTIES_KHR};
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &pbp};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ex(ne); vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, ex.data());
        auto has = [&](const char* e) { for (auto& x : ex) if (!std::strcmp(x.extensionName, e)) return true; return false; };
        const bool exts = has(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) && has(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        if (!exts) drv.pNext = nullptr;
        vkGetPhysicalDeviceProperties2(phys, &p2);
        device_id = p2.properties.deviceID;
        driver = fmt("%s %s (0x%08x)", drv.driverName, drv.driverInfo, p2.properties.driverVersion);
        if (!info) {
            say(fmt("GPU %s, device 0x%04x, driver %s, driverID %d, Vulkan %u.%u.%u", p2.properties.deviceName, p2.properties.deviceID, driver.c_str(),
                    int(drv.driverID), VK_API_VERSION_MAJOR(p2.properties.apiVersion), VK_API_VERSION_MINOR(p2.properties.apiVersion), VK_API_VERSION_PATCH(p2.properties.apiVersion)));
            say("pipelineCacheUUID " + hex(p2.properties.pipelineCacheUUID, VK_UUID_SIZE));
            if (exts) say(fmt("pipeline binary properties: internalCache %u, internalCacheControl %u, prefersInternalCache %u, precompiledInternalCache %u, compressedData %u",
                              pbp.pipelineBinaryInternalCache, pbp.pipelineBinaryInternalCacheControl, pbp.pipelineBinaryPrefersInternalCache,
                              pbp.pipelineBinaryPrecompiledInternalCache, pbp.pipelineBinaryCompressedData));
        }
        vkGetPhysicalDeviceMemoryProperties(phys, &mem);
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq); vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf.data());
        for (uint32_t i = 0; i < nq; ++i) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
        VkPhysicalDevicePipelineBinaryFeaturesKHR pbf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR};
        VkPhysicalDeviceMaintenance5FeaturesKHR m5f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &pbf};
        VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pef{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, &m5f};
        VkPhysicalDeviceFeatures2 qf2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &pef};
        const bool has_pe = has(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
        if (!exts) pef.pNext = nullptr;
        vkGetPhysicalDeviceFeatures2(phys, &qf2);
        binaries = exts && pbf.pipelineBinaries && m5f.maintenance5;
        if (!info) say(fmt("VK_KHR_pipeline_binary %s, maintenance5 %s, pipelineBinaries %s, VK_KHR_pipeline_executable_properties %s, VK_AMD_shader_info %s",
                           has(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) ? "yes" : "no", has(VK_KHR_MAINTENANCE_5_EXTENSION_NAME) ? "yes" : "no",
                           exts && pbf.pipelineBinaries ? "yes" : "no", has_pe && pef.pipelineExecutableInfo ? "yes" : "no", has(VK_AMD_SHADER_INFO_EXTENSION_NAME) ? "yes" : "no"));
        // The binary device is made as nr_aco_probe made it (and as the product does); the info device only
        // asks for statistics and disassembly, so neither changes what the other compiles.
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.subgroupSizeControl = VK_TRUE; f13.computeFullSubgroups = VK_TRUE;
        VkPhysicalDevicePipelineBinaryFeaturesKHR pbe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, &f13}; pbe.pipelineBinaries = VK_TRUE;
        VkPhysicalDeviceMaintenance5FeaturesKHR m5e{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &pbe}; m5e.maintenance5 = VK_TRUE;
        VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pee{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, &f13};
        pee.pipelineExecutableInfo = VK_TRUE;
        std::vector<const char*> en; void* chain = &f13;
        if (!info && binaries) { en.push_back(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME); en.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME); chain = &m5e; }
        if (info) {
            exec_info = has_pe && pef.pipelineExecutableInfo; shader_info = has(VK_AMD_SHADER_INFO_EXTENSION_NAME);
            if (exec_info) { en.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME); chain = &pee; }
            if (shader_info) en.push_back(VK_AMD_SHADER_INFO_EXTENSION_NAME);
        }
        float prio = 1.f;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &prio;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, chain};
        di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.enabledExtensionCount = uint32_t(en.size()); di.ppEnabledExtensionNames = en.data();
        if (VkResult r = vkCreateDevice(phys, &di, nullptr, &dev)) { say(fmt("FAIL vkCreateDevice %d", int(r))); return false; }
        vkGetDeviceQueue(dev, family, 0, &q);
        return true;
    }
    void done() { if (dev) { vkDeviceWaitIdle(dev); vkDestroyDevice(dev, nullptr); } if (inst) vkDestroyInstance(inst, nullptr); dev = {}; inst = {}; }
    struct Buf { VkBuffer b{}; VkDeviceMemory m{}; uint32_t* p{}; };
    Buf buffer(size_t bytes) {
        Buf r;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = bytes; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        vkCreateBuffer(dev, &bi, nullptr, &r.b);
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, r.b, &mr);
        uint32_t t = 0;
        for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
            const auto f = mem.memoryTypes[i].propertyFlags;
            if ((mr.memoryTypeBits >> i & 1) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { t = i; break; }
        }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = t;
        vkAllocateMemory(dev, &ai, nullptr, &r.m); vkBindBufferMemory(dev, r.b, r.m, 0);
        vkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&r.p));
        return r;
    }
    void free(Buf& b) { vkDestroyBuffer(dev, b.b, nullptr); vkFreeMemory(dev, b.m, nullptr); }
};

// The descriptor sets, push range and local size a compute SPIR-V declares (enough for the network kernels).
struct Layout { std::map<uint32_t, std::vector<VkDescriptorSetLayoutBinding>> sets; uint32_t push = 0; std::array<uint32_t, 3> wg{}; std::string text; };
static bool reflect(const std::vector<uint32_t>& w, Layout& L, std::string& why) {
    if (w.size() < 5 || w[0] != 0x07230203) { why = "not SPIR-V"; return false; }
    std::map<uint32_t, std::vector<uint32_t>> def;       // id -> instruction words (types, constants, variables)
    std::map<uint32_t, uint32_t> set, binding, stride; std::set<uint32_t> buffer_block;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> member_offset;
    std::vector<uint32_t> vars;
    for (size_t p = 5; p < w.size();) {
        uint32_t n = w[p] >> 16, op = w[p] & 0xffff;
        if (!n || p + n > w.size()) { why = "bad SPIR-V"; return false; }
        const uint32_t* a = &w[p + 1];
        if (op == 71 && n >= 3) { if (a[1] == 34) set[a[0]] = a[2]; if (a[1] == 33) binding[a[0]] = a[2]; if (a[1] == 3) buffer_block.insert(a[0]); if (a[1] == 6) stride[a[0]] = a[2]; }
        if (op == 72 && n >= 5 && a[2] == 35) member_offset[{a[0], a[1]}] = a[3];
        if (op == 16 && n == 6 && a[1] == 17) L.wg = {a[2], a[3], a[4]};
        if ((op >= 19 && op <= 33) || op == 43) def[a[op == 43 ? 1 : 0]] = std::vector<uint32_t>(&w[p], &w[p] + n);
        if (op == 59) { def[a[1]] = std::vector<uint32_t>(&w[p], &w[p] + n); vars.push_back(a[1]); }
        p += n;
    }
    std::function<uint32_t(uint32_t)> size = [&](uint32_t t) -> uint32_t {
        auto& d = def.at(t); uint32_t op = d[0] & 0xffff;
        if (op == 21 || op == 22) return d[2] / 8;
        if (op == 23) return d[3] * size(d[2]);
        if (op == 28) return def.at(d[3])[3] * (stride.count(t) ? stride[t] : size(d[2]));
        if (op == 30) { uint32_t m = 0; for (uint32_t i = 2; i < d.size(); ++i) m = std::max(m, member_offset[{t, i - 2}] + size(d[i])); return m; }
        throw std::runtime_error("push constant type");
    };
    for (uint32_t v : vars) {
        auto& d = def[v]; uint32_t sc = d[3], ptr = d[1];
        uint32_t t = def.at(ptr)[3];
        if (sc == 9) { try { L.push = (size(t) + 3) & ~3u; } catch (...) { L.push = 128; } continue; }
        if (!set.count(v) || !binding.count(v)) continue;
        uint32_t count = 1;
        while ((def.at(t)[0] & 0xffff) == 28 || (def.at(t)[0] & 0xffff) == 29) {
            if ((def.at(t)[0] & 0xffff) == 28) count *= def.at(def.at(t)[3])[3];
            t = def.at(t)[2];
        }
        auto& td = def.at(t); uint32_t op = td[0] & 0xffff; VkDescriptorType ty;
        if (op == 30) ty = (sc == 12 || buffer_block.count(t)) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (op == 25) ty = td[7] == 2 ? (td[3] == 5 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                                           : (td[3] == 5 ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        else if (op == 27) ty = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (op == 26) ty = VK_DESCRIPTOR_TYPE_SAMPLER;
        else { why = fmt("descriptor type op %u", op); return false; }
        // Several SPIR-V views of one binding (the network's aliased arenas) are one descriptor.
        auto& b = L.sets[set[v]];
        if (std::none_of(b.begin(), b.end(), [&](auto& x) { return x.binding == binding[v]; }))
            b.push_back({binding[v], ty, count, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }
    for (auto& [s, b] : L.sets) {
        std::sort(b.begin(), b.end(), [](auto& x, auto& y) { return x.binding < y.binding; });
        L.text += fmt("set %u:", s); for (auto& x : b) L.text += fmt(" %u=%s%s", x.binding, x.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? "ssbo" :
            x.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? "ubo" : x.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? "simg" :
            x.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? "cis" : x.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? "tex" : "other",
            x.descriptorCount > 1 ? fmt("[%u]", x.descriptorCount).c_str() : ""); L.text += "; ";
    }
    L.text += fmt("push %u bytes, local size %ux%ux%u", L.push, L.wg[0], L.wg[1], L.wg[2]);
    return true;
}

struct Probe { const char* file; uint32_t gx, gy, push[4], w, h; };
static const Probe kProbes[] = {{"probe_a.spv", 64, 1, {2654435761u, 12345u, 4000u, 0}, 4000, 1},
                                {"probe_b.spv", 5, 7, {40503u, 977u, 300u, 13u}, 300, 13}};
static uint32_t cpu(const Probe& p, const std::vector<uint32_t>& a, uint32_t x, uint32_t y) {
    if (p.h == 1) return a[x] * p.push[0] + p.push[1] + ((x / 64) << 20);
    const uint32_t wx = x / 64, wy = y / 2, lx = x % 64, ly = y % 2, l = ly * 64 + lx, m = 127 - l;
    const uint32_t mx = wx * 64 + m % 64, my = wy * 2 + m / 64;
    const uint32_t src = (mx < p.w && my < p.h) ? a[my * p.w + mx] : 0u;
    return (src * p.push[0] + p.push[1]) ^ (wx << 8) ^ (wy << 16) ^ (p.gx << 24) ^ ((l / 32) << 28);
}

struct Shader {
    std::string name; std::vector<uint32_t> spv; uint64_t key = 0; Layout L; const Probe* probe = nullptr;
    std::vector<VkDescriptorSetLayout> dsl; VkPipelineLayout layout{}; VkShaderModule module{};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    // Probe resources.
    Gpu::Buf in, out; VkDescriptorPool pool{}; VkDescriptorSet set{}; std::vector<uint32_t> input, ref;
    bool make(Gpu& g) {
        uint32_t top = L.sets.empty() ? 0 : L.sets.rbegin()->first + 1;
        for (uint32_t s = 0; s < top; ++s) {
            auto it = L.sets.find(s);
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            if (it != L.sets.end()) { li.bindingCount = uint32_t(it->second.size()); li.pBindings = it->second.data(); }
            VkDescriptorSetLayout d{}; if (vkCreateDescriptorSetLayout(g.dev, &li, nullptr, &d)) return false; dsl.push_back(d);
        }
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, L.push};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = uint32_t(dsl.size()); pli.pSetLayouts = dsl.data();
        pli.pushConstantRangeCount = L.push ? 1 : 0; pli.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(g.dev, &pli, nullptr, &layout)) return false;
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = spv.size() * 4; smi.pCode = spv.data();
        if (vkCreateShaderModule(g.dev, &smi, nullptr, &module)) return false;
        req.requiredSubgroupSize = 32;
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &req}; ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module; ci.stage.pName = "main"; ci.layout = layout;
        if (!probe) return true;
        const uint32_t n = probe->w * probe->h;
        input.resize(n); std::mt19937 rng(7); for (auto& v : input) v = rng();
        in = g.buffer(n * 4); out = g.buffer(n * 4); std::memcpy(in.p, input.data(), n * 4);
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
        vkCreateDescriptorPool(g.dev, &dpi, nullptr, &pool);
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl[0];
        vkAllocateDescriptorSets(g.dev, &dai, &set);
        VkDescriptorBufferInfo bi[2] = {{in.b, 0, VK_WHOLE_SIZE}, {out.b, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi[0], nullptr},
                                      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi[1], nullptr}};
        vkUpdateDescriptorSets(g.dev, 2, wr, 0, nullptr);
        return true;
    }
    void free(Gpu& g) {
        if (probe) { vkDestroyDescriptorPool(g.dev, pool, nullptr); g.free(in); g.free(out); }
        vkDestroyShaderModule(g.dev, module, nullptr); vkDestroyPipelineLayout(g.dev, layout, nullptr);
        for (auto d : dsl) vkDestroyDescriptorSetLayout(g.dev, d, nullptr);
    }
};

struct Runner {
    Gpu& g; VkCommandPool pool{}; VkCommandBuffer cmd{}; VkFence fence{}; bool lost = false;
    explicit Runner(Gpu& gpu) : g(gpu) {
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = g.family;
        vkCreateCommandPool(g.dev, &cpi, nullptr, &pool);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(g.dev, &cai, &cmd);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vkCreateFence(g.dev, &fci, nullptr, &fence);
    }
    ~Runner() { if (!lost) vkDeviceWaitIdle(g.dev); vkDestroyFence(g.dev, fence, nullptr); vkDestroyCommandPool(g.dev, pool, nullptr); }
    VkResult run(Shader& s, VkPipeline pipe, std::vector<uint32_t>& result) {
        const uint32_t n = s.probe->w * s.probe->h;
        std::memset(s.out.p, 0xAB, n * 4);
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkBeginCommandBuffer(cmd, &cbi);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &s.set, 0, nullptr);
        vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, s.L.push, s.probe->push);
        vkCmdDispatch(cmd, s.probe->gx, s.probe->gy, 1);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        vkEndCommandBuffer(cmd);
        vkResetFences(g.dev, 1, &fence);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VkResult r = vkQueueSubmit(g.q, 1, &si, fence);
        if (!r) r = vkWaitForFences(g.dev, 1, &fence, VK_TRUE, 10000000000ull);
        if (r == VK_ERROR_DEVICE_LOST || r == VK_TIMEOUT) lost = true;
        if (!r) result.assign(s.out.p, s.out.p + n);
        return r;
    }
    // Two runs against the reference: "BIT-EXACT ..." / "WRONG ..." / "dispatch failed".
    std::string check(Shader& s, VkPipeline pipe, bool* exact = nullptr) {
        std::vector<uint32_t> a, b; VkResult r = run(s, pipe, a); if (!r) r = run(s, pipe, b);
        if (exact) *exact = false;
        if (r) return fmt("the dispatch did not finish (%d%s)", int(r), lost ? ", device lost: stopping" : "");
        uint32_t bad = 0, unwritten = 0;
        for (size_t i = 0; i < a.size(); ++i) { bad += a[i] != s.ref[i]; unwritten += a[i] == 0xABABABABu; }
        if (!bad && a == b) { if (exact) *exact = true; return "BIT-EXACT with the driver's compile, 2 of 2 runs"; }
        return fmt("WRONG: %u of %zu words differ (%u never written)%s", bad, a.size(), unwritten, a == b ? "" : ", runs differ");
    }
};

struct Api {
    PFN_vkCreatePipelineBinariesKHR create{}; PFN_vkDestroyPipelineBinaryKHR destroy{}; PFN_vkGetPipelineBinaryDataKHR data{};
    PFN_vkReleaseCapturedPipelineDataKHR release{}; PFN_vkGetPipelineKeyKHR key{};
    explicit Api(VkDevice d) {
        create = (PFN_vkCreatePipelineBinariesKHR)vkGetDeviceProcAddr(d, "vkCreatePipelineBinariesKHR");
        destroy = (PFN_vkDestroyPipelineBinaryKHR)vkGetDeviceProcAddr(d, "vkDestroyPipelineBinaryKHR");
        data = (PFN_vkGetPipelineBinaryDataKHR)vkGetDeviceProcAddr(d, "vkGetPipelineBinaryDataKHR");
        release = (PFN_vkReleaseCapturedPipelineDataKHR)vkGetDeviceProcAddr(d, "vkReleaseCapturedPipelineDataKHR");
        key = (PFN_vkGetPipelineKeyKHR)vkGetDeviceProcAddr(d, "vkGetPipelineKeyKHR");
    }
    bool ok() const { return create && destroy && data && release && key; }
};
struct Blob { VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR}; Bytes data; };
static std::string keyhex(const VkPipelineBinaryKeyKHR& k) { return fmt("%u bytes ", k.keySize) + hex(k.key, std::min<uint32_t>(k.keySize, VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR)); }

// The driver's compile with CAPTURE_DATA, its binaries exported. The pipeline stays alive in *pipe.
static VkResult capture(Gpu& g, Api& api, Shader& s, VkPipeline* pipe, std::vector<Blob>& out, double* ms) {
    VkPipelineCreateFlags2CreateInfoKHR flags{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR};
    flags.flags = VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;
    auto ci = s.ci; ci.pNext = &flags;
    auto t = std::chrono::steady_clock::now();
    if (VkResult r = vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &ci, nullptr, pipe)) return r;
    *ms = ms_since(t);
    VkPipelineBinaryCreateInfoKHR bi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR}; bi.pipeline = *pipe;
    VkPipelineBinaryHandlesInfoKHR hi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    if (VkResult r = api.create(g.dev, &bi, nullptr, &hi)) return r;
    std::vector<VkPipelineBinaryKHR> h(hi.pipelineBinaryCount); hi.pPipelineBinaries = h.data();
    VkResult r = api.create(g.dev, &bi, nullptr, &hi);
    for (uint32_t i = 0; !r && i < hi.pipelineBinaryCount; ++i) {
        Blob b; VkPipelineBinaryDataInfoKHR di{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR}; di.pipelineBinary = h[i];
        size_t n = 0; r = api.data(g.dev, &di, &b.key, &n, nullptr);
        if (!r) { b.data.resize(n); r = api.data(g.dev, &di, &b.key, &n, b.data.data()); b.data.resize(std::min(n, b.data.size())); }
        out.push_back(std::move(b));
    }
    for (auto x : h) if (x) api.destroy(g.dev, x, nullptr);
    VkReleaseCapturedPipelineDataInfoKHR rel{VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR}; rel.pipeline = *pipe;
    api.release(g.dev, &rel, nullptr);
    return r;
}
// The product's way of keying an imported binary (nr::binary::create).
static VkPipelineBinaryKeyKHR our_key(const VkPipelineBinaryKeyKHR& k, const Bytes& blob, uint64_t spv) {
    auto key = k; uint64_t h = nr::pal::hash(blob.data(), blob.size()) ^ spv ^ 0x4e5250414c303031ull;
    for (uint32_t i = 0; i < key.keySize; ++i) { h ^= i + 1; h *= 1099511628211ull; key.key[i] = uint8_t(h >> (8 * (i % 8))); }
    return key;
}
// Binaries in, pipeline out; the step that failed in *why.
static VkPipeline import(Gpu& g, Api& api, Shader& s, const std::vector<Blob>& in, std::string& why, double* ms = nullptr) {
    std::vector<VkPipelineBinaryKeyKHR> keys; std::vector<VkPipelineBinaryDataKHR> data;
    for (auto& b : in) { keys.push_back(b.key); data.push_back({b.data.size(), const_cast<uint8_t*>(b.data.data())}); }
    VkPipelineBinaryKeysAndDataKHR kd{uint32_t(in.size()), keys.data(), data.data()};
    VkPipelineBinaryCreateInfoKHR bi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR}; bi.pKeysAndDataInfo = &kd;
    std::vector<VkPipelineBinaryKHR> h(in.size());
    VkPipelineBinaryHandlesInfoKHR hi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR}; hi.pipelineBinaryCount = uint32_t(h.size()); hi.pPipelineBinaries = h.data();
    auto t = std::chrono::steady_clock::now();
    VkResult r = api.create(g.dev, &bi, nullptr, &hi);
    VkPipeline pipe{};
    if (r) why = fmt("vkCreatePipelineBinariesKHR refused it (%d)", int(r));
    else {
        VkPipelineBinaryInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR}; info.binaryCount = uint32_t(h.size()); info.pPipelineBinaries = h.data();
        auto ci = s.ci; ci.pNext = &info; ci.stage.module = VK_NULL_HANDLE;
        r = vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe);
        if (r) { why = fmt("vkCreateComputePipelines refused it (%d)", int(r)); if (pipe) vkDestroyPipeline(g.dev, pipe, nullptr); pipe = {}; }
    }
    if (ms) *ms = ms_since(t);
    for (auto x : h) if (x) api.destroy(g.dev, x, nullptr);
    return pipe;
}

static std::vector<std::unique_ptr<Shader>> load_shaders(const fs::path& dir, bool with_net) {
    std::vector<std::unique_ptr<Shader>> all;
    auto add = [&](const fs::path& p, const Probe* probe) {
        auto s = std::make_unique<Shader>(); s->name = p.stem().string(); s->spv = read_words(p); s->probe = probe;
        if (s->spv.empty()) { say("FAIL " + p.string() + " missing"); return; }
        s->key = nr::pal::hash(s->spv.data(), s->spv.size() * 4);
        std::string why; if (!reflect(s->spv, s->L, why)) { say("FAIL " + s->name + ": " + why); return; }
        all.push_back(std::move(s));
    };
    for (auto& p : kProbes) add(dir / p.file, &p);
    std::error_code ec;
    if (with_net) for (auto& e : fs::directory_iterator(dir / "net", ec)) if (e.path().extension() == ".spv") add(e.path(), nullptr);
    return all;
}

// ---------------------------------------------------------------------------------------------------------------
static int phase_capture(const fs::path& dir, const fs::path& out) {
    Gpu g; if (!g.init(false)) return 1;
    if (!g.binaries) { say("RESULT no pipeline binaries on this driver: nothing to capture"); g.done(); return 1; }
    Api api(g.dev); if (!api.ok()) { say("FAIL pipeline binary functions missing"); g.done(); return 1; }
    {
        VkPipelineBinaryKeyKHR k{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        VkResult r = api.key(g.dev, nullptr, &k);
        say(fmt("global key (vkGetPipelineKeyKHR, no create info): %d, ", int(r)) + keyhex(k));
        save(out / "blobs" / "global.key", k.key, k.keySize);
    }
    auto shaders = load_shaders(dir, true);
    int captured = 0, roundtrip_ok = 0, failures = 0;
    {
        Runner run(g);
        for (auto& sp : shaders) {
            auto& s = *sp;
            say(""); say(fmt("== %s (%zu bytes of SPIR-V, key %016llx): %s", s.name.c_str(), s.spv.size() * 4, (unsigned long long)s.key, s.L.text.c_str()));
            if (!s.make(g)) { say("FAIL layout/module"); ++failures; continue; }
            VkPipelineCreateInfoKHR pci{VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR, &s.ci};
            VkPipelineBinaryKeyKHR pk{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
            VkResult kr = api.key(g.dev, &pci, &pk);
            say(fmt("pre-compile key: %d, ", int(kr)) + keyhex(pk));
            VkPipeline native{};
            if (s.probe) {
                auto t = std::chrono::steady_clock::now();
                if (VkResult r = vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &s.ci, nullptr, &native)) { say(fmt("FAIL driver compile %d", int(r))); ++failures; s.free(g); continue; }
                double ms = ms_since(t);
                if (VkResult r = run.run(s, native, s.ref)) { say(fmt("FAIL driver pipeline did not finish (%d)", int(r))); ++failures; vkDestroyPipeline(g.dev, native, nullptr); break; }
                uint32_t bad = 0;
                for (uint32_t y = 0; y < s.probe->h; ++y) for (uint32_t x = 0; x < s.probe->w; ++x) bad += s.ref[y * s.probe->w + x] != cpu(*s.probe, s.input, x, y);
                say(fmt("driver compile (%.1f ms) %s the CPU (%u of %zu words differ)", ms, bad ? "DIFFERS FROM" : "matches", bad, s.ref.size()));
                if (bad) ++failures;
            }
            VkPipeline cap{}; std::vector<Blob> blobs; double ms = 0;
            VkResult r = capture(g, api, s, &cap, blobs, &ms);
            if (r) { say(fmt("FAIL capture: %d", int(r))); ++failures; }
            else {
                ++captured;
                say(fmt("captured pipeline compiled in %.1f ms, %zu binaries", ms, blobs.size()));
                for (size_t i = 0; i < blobs.size(); ++i) {
                    auto base = fmt("%s.%zu", s.name.c_str(), i);
                    save(out / "blobs" / (base + ".bin"), blobs[i].data); save(out / "blobs" / (base + ".key"), blobs[i].key.key, blobs[i].key.keySize);
                    auto d = nr::probe::describe(blobs[i].data, s.L.push);
                    save(out / "blobs" / (base + ".txt"), d);
                    say(fmt("binary %zu: key ", i) + keyhex(blobs[i].key) + (std::memcmp(blobs[i].key.key, pk.key, pk.keySize) || blobs[i].key.keySize != pk.keySize ? " (differs from the pre-compile key)" : " (= pre-compile key)"));
                    say(d);
                }
                if (s.probe) say("captured pipeline: " + run.check(s, cap));
                // Round trip: the driver's own binary back in, with its key and with a key of our own.
                for (int k = 0; k < 2 && !run.lost; ++k) {
                    auto in = blobs; if (k) for (auto& b : in) b.key = our_key(b.key, b.data, s.key);
                    std::string why; double ims = 0;
                    VkPipeline p = import(g, api, s, in, why, &ims);
                    const char* what = k ? "round trip, our key" : "round trip, driver's key";
                    if (!p) { say(fmt("%s: REFUSED, %s", what, why.c_str())); ++failures; continue; }
                    bool exact = true;
                    std::string res = s.probe ? run.check(s, p, &exact) : "pipeline created (network kernel: not dispatched)";
                    say(fmt("%s: imported in %.1f ms; %s", what, ims, res.c_str()));
                    if (exact) ++roundtrip_ok; else ++failures;
                    vkDestroyPipeline(g.dev, p, nullptr);
                }
            }
            if (cap) vkDestroyPipeline(g.dev, cap, nullptr);
            if (native) vkDestroyPipeline(g.dev, native, nullptr);
            s.free(g);
            if (run.lost) break;
        }
    }
    g.done();
    // Statistics and the driver's disassembly, on a device of their own.
    Gpu gi;
    if (gi.init(true) && (gi.exec_info || gi.shader_info)) {
        auto props = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(gi.dev, "vkGetPipelineExecutablePropertiesKHR");
        auto stats = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(gi.dev, "vkGetPipelineExecutableStatisticsKHR");
        auto irs = (PFN_vkGetPipelineExecutableInternalRepresentationsKHR)vkGetDeviceProcAddr(gi.dev, "vkGetPipelineExecutableInternalRepresentationsKHR");
        auto sinfo = (PFN_vkGetShaderInfoAMD)vkGetDeviceProcAddr(gi.dev, "vkGetShaderInfoAMD");
        say(""); say("== driver statistics and disassembly (info\\)");
        for (auto& sp : load_shaders(dir, true)) {
            auto& s = *sp; s.probe = nullptr;
            if (!s.make(gi)) continue;
            auto ci = s.ci;
            if (gi.exec_info) ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
            VkPipeline p{};
            if (vkCreateComputePipelines(gi.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p)) { say(s.name + ": info compile failed"); s.free(gi); continue; }
            std::string line = s.name + ":";
            if (gi.exec_info && props && stats && irs) {
                VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR}; pi.pipeline = p; uint32_t ne = 0;
                props(gi.dev, &pi, &ne, nullptr);
                std::vector<VkPipelineExecutablePropertiesKHR> ep(ne, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR}); props(gi.dev, &pi, &ne, ep.data());
                std::string st;
                for (uint32_t e = 0; e < ne; ++e) {
                    VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR}; ei.pipeline = p; ei.executableIndex = e;
                    st += fmt("executable %u: %s (%s), subgroup %u\n", e, ep[e].name, ep[e].description, ep[e].subgroupSize);
                    uint32_t ns = 0; stats(gi.dev, &ei, &ns, nullptr);
                    std::vector<VkPipelineExecutableStatisticKHR> sv(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR}); stats(gi.dev, &ei, &ns, sv.data());
                    for (auto& x : sv) {
                        st += fmt("  %s = ", x.name);
                        switch (x.format) {
                        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: st += x.value.b32 ? "true" : "false"; break;
                        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: st += std::to_string(x.value.i64); break;
                        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: st += std::to_string(x.value.u64); break;
                        default: st += std::to_string(x.value.f64);
                        }
                        st += fmt("  (%s)\n", x.description);
                    }
                    uint32_t nr = 0; irs(gi.dev, &ei, &nr, nullptr);
                    std::vector<VkPipelineExecutableInternalRepresentationKHR> rv(nr, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
                    irs(gi.dev, &ei, &nr, rv.data());
                    std::vector<std::vector<char>> keep(nr);
                    for (uint32_t i = 0; i < nr; ++i) { keep[i].resize(rv[i].dataSize); rv[i].pData = keep[i].data(); }
                    if (nr) irs(gi.dev, &ei, &nr, rv.data());
                    for (uint32_t i = 0; i < nr; ++i) {
                        std::string nm = rv[i].name; for (auto& c : nm) if (!isalnum((unsigned char)c)) c = '_';
                        st += fmt("  internal representation %u: %s (%s), %zu bytes, %s\n", i, rv[i].name, rv[i].description, rv[i].dataSize, rv[i].isText ? "text" : "binary");
                        save(out / "info" / fmt("%s.e%u.%s.%s", s.name.c_str(), e, nm.c_str(), rv[i].isText ? "txt" : "bin"), keep[i].data(),
                             rv[i].isText && rv[i].dataSize ? strnlen(keep[i].data(), rv[i].dataSize) : rv[i].dataSize);
                    }
                }
                save(out / "info" / (s.name + ".stats.txt"), st); line += fmt(" %u executables, statistics saved", ne);
            }
            if (gi.shader_info && sinfo) {
                VkShaderStatisticsInfoAMD ss{}; size_t n = sizeof ss;
                if (!sinfo(gi.dev, p, VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_INFO_TYPE_STATISTICS_AMD, &n, &ss))
                    line += fmt(" | AMD: vgprs %u sgprs %u lds %u scratch %u, physical %u/%u", ss.resourceUsage.numUsedVgprs, ss.resourceUsage.numUsedSgprs,
                                ss.resourceUsage.ldsUsageSizeInBytes, unsigned(ss.resourceUsage.scratchMemUsageInBytes), ss.numPhysicalVgprs, ss.numPhysicalSgprs);
                for (auto [type, ext] : {std::pair{VK_SHADER_INFO_TYPE_DISASSEMBLY_AMD, ".amd-disasm.txt"}, std::pair{VK_SHADER_INFO_TYPE_BINARY_AMD, ".amd-binary.bin"}}) {
                    size_t sz = 0;
                    if (sinfo(gi.dev, p, VK_SHADER_STAGE_COMPUTE_BIT, type, &sz, nullptr) || !sz) { line += fmt(" | %s none", ext); continue; }
                    Bytes b(sz); VkResult rr = sinfo(gi.dev, p, VK_SHADER_STAGE_COMPUTE_BIT, type, &sz, b.data()); if (rr != VK_SUCCESS && rr != VK_INCOMPLETE) continue;
                    b.resize(sz);
                    if (type == VK_SHADER_INFO_TYPE_DISASSEMBLY_AMD) b.resize(strnlen(reinterpret_cast<char*>(b.data()), b.size()));
                    save(out / "info" / (s.name + ext), b);
                    if (type == VK_SHADER_INFO_TYPE_BINARY_AMD) save(out / "info" / (s.name + ".amd-binary.txt"), nr::probe::describe(b, s.L.push));
                    line += fmt(" | %s %zu bytes", ext, b.size());
                }
            }
            say(line);
            vkDestroyPipeline(gi.dev, p, nullptr); s.free(gi);
        }
    } else say("no statistics/disassembly extensions on this driver");
    gi.done();
    say(""); say(fmt("RESULT captured %d shaders, round trips OK %d, failures %d", captured, roundtrip_ok, failures));
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------
static uint32_t table_machine(uint32_t id) {
    if (id >= 0x73a0 && id <= 0x73bf) return 0x36; if (id >= 0x73c0 && id <= 0x73df) return 0x37;
    if (id >= 0x73e0 && id <= 0x73ff) return 0x38; if (id >= 0x7420 && id <= 0x743f) return 0x3e;
    if (id >= 0x7440 && id <= 0x745f) return 0x41; if (id >= 0x7470 && id <= 0x747f) return 0x46; if (id >= 0x7480 && id <= 0x749f) return 0x47;
    if (id >= 0x7550 && id <= 0x755f) return 0x4e; if (id >= 0x7590 && id <= 0x759f) return 0x48;
    return 0;
}
static const char* folder(uint32_t m) {
    switch (m) { case 0x36: return "navi21"; case 0x37: return "navi22"; case 0x38: return "navi23"; case 0x3e: return "navi24";
                 case 0x4e: case 0x48: return "gfx1201"; default: return nr::pal::is_gfx11(m) ? "navi31" : ""; }
}

static int phase_variants(const fs::path& dir, const fs::path& out) {
    Gpu g; if (!g.init(false)) return 1;
    if (!g.binaries) { say("RESULT no pipeline binaries on this driver"); g.done(); return 1; }
    Api api(g.dev); if (!api.ok()) { say("FAIL pipeline binary functions missing"); g.done(); return 1; }
    int exact_n = 0, refused = 0, wrong = 0, accepted_tamper = 0;
    std::string summary;
    {
        Runner run(g);
        for (auto& sp : load_shaders(dir, false)) {
            if (run.lost) break;
            auto& s = *sp;
            say(""); say(fmt("== %s (key %016llx)", s.name.c_str(), (unsigned long long)s.key));
            if (!s.make(g)) { say("FAIL layout/module"); continue; }
            VkPipeline native{};
            if (vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &s.ci, nullptr, &native) || run.run(s, native, s.ref)) { say("FAIL driver reference"); s.free(g); break; }
            VkPipeline cap{}; std::vector<Blob> own; double ms = 0;
            if (capture(g, api, s, &cap, own, &ms) || own.size() != 1) { say(fmt("own binary: not one binary (%zu); no variants", own.size())); }
            if (cap) vkDestroyPipeline(g.dev, cap, nullptr);
            if (own.size() != 1) { vkDestroyPipeline(g.dev, native, nullptr); s.free(g); continue; }
            const Bytes& ob = own[0].data;
            auto at = nr::probe::elf_offsets(ob);
            nr::probe::ElfView ov; if (!at.empty()) ov = nr::probe::view(ob, at[0]);
            const uint32_t eflags = ov.ok ? ov.flags : table_machine(g.device_id), mach = eflags & 0xff;
            say(fmt("own binary %zu bytes, ELF %s, machine 0x%02x (%s)", ob.size(), at.empty() ? "none" : fmt("at %zu", at[0]).c_str(), mach,
                    ov.ok ? "from its e_flags" : "from the device ID"));
            // Does the driver check the binary's integrity? Created only, never dispatched.
            for (int t = 0; t < 2; ++t) {
                Blob b = own[0];
                if (t == 0) b.data.resize(b.data.size() + 64, 0);
                else {
                    size_t p = b.data.size() / 2;
                    if (ov.ok) for (size_t i = 0; i < ov.names.size(); ++i) if (ov.names[i] == ".text" && ov.sections[i].size) p = at[0] + ov.sections[i].offset + ov.sections[i].size - 1;
                    b.data[p] ^= 0x01;
                }
                b.key = our_key(b.key, b.data, s.key);
                std::string why; VkPipeline p = import(g, api, s, {b}, why);
                say(fmt("tamper %s: %s (created only, not run)", t ? "one bit flipped in the last code byte" : "64 zero bytes appended", p ? "ACCEPTED" : ("refused: " + why).c_str()));
                if (p) { ++accepted_tamper; vkDestroyPipeline(g.dev, p, nullptr); }
            }
            // The record for this GPU (any record of the same generation if its own machine has none).
            nr::pal::Record rec; std::string rec_from; bool have = false;
            auto try_rec = [&](const fs::path& f) {
                try { rec = nr::pal::Record::parse(nr::pal::load(f.string())); have = true; rec_from = f.parent_path().filename().string(); } catch (...) {}
            };
            const auto keyfile = nr::binary::keyname(s.key);
            if (*folder(mach)) try_rec(dir / "records" / folder(mach) / keyfile);
            std::error_code ec;
            if (!have) for (auto& m : fs::directory_iterator(dir / "records", ec)) {
                if (!fs::exists(m.path() / keyfile)) continue; try_rec(m.path() / keyfile);
                if (have && !nr::pal::same_generation(rec.mach ? rec.mach : 0x4e, mach)) have = false; else if (have) break;
            }
            if (!have) { say(fmt("no record of this generation (machine 0x%02x) for %s", mach, s.name.c_str())); vkDestroyPipeline(g.dev, native, nullptr); s.free(g); continue; }
            say(fmt("record: %s (machine 0x%02x, %zu bytes of code)", rec_from.c_str(), rec.mach, rec.code.size()));
            Bytes ref; try { ref = nr::pal::load((dir / "ref-gfx12" / (s.name + ".bin")).string()); } catch (...) { say("no RDNA4 reference container: the synth variants are not built"); }
            auto rv = nr::probe::view(ref, 0);
            struct V { std::string name; std::function<Bytes(std::string&)> make; };
            std::vector<V> vs;
            vs.push_back({"own-strict", [&](std::string&) { return nr::pal::splice(ob, rec, s.L.push, s.L.wg); }});
            vs.push_back({"own-loose", [&](std::string& n) { return nr::probe::splice_loose(ob, rec, s.L.push, s.L.wg, {}, n); }});
            if (ov.ok && ov.has_md && !(ov.md_major == 3 && ov.md_minor == 6)) {
                nr::probe::Loose o; o.md_major = 3; o.md_minor = 6;
                vs.push_back({"own-loose-md3.6", [&, o](std::string& n) { return nr::probe::splice_loose(ob, rec, s.L.push, s.L.wg, o, n); }});
            }
            if (ov.ok && ov.osabi != 65) {
                nr::probe::Loose o; o.osabi = 65; o.abiver = rv.ok ? rv.abiver : 0;
                vs.push_back({"own-loose-osabi65", [&, o](std::string& n) { return nr::probe::splice_loose(ob, rec, s.L.push, s.L.wg, o, n); }});
            }
            nr::probe::Loose so; so.eflags = int(eflags);
            vs.push_back({"synth", [&, so](std::string& n) { return nr::probe::splice_loose(ref, rec, s.L.push, s.L.wg, so, n); }});
            if (ov.ok && rv.ok && (ov.osabi != rv.osabi || ov.abiver != rv.abiver)) {
                auto o = so; o.osabi = ov.osabi; o.abiver = ov.abiver;
                vs.push_back({"synth-own-header", [&, o](std::string& n) { return nr::probe::splice_loose(ref, rec, s.L.push, s.L.wg, o, n); }});
            }
            if (ov.ok && ov.has_md && rv.ok && (ov.md_major != rv.md_major || ov.md_minor != rv.md_minor)) {
                auto o = so; o.md_major = int(ov.md_major); o.md_minor = int(ov.md_minor);
                vs.push_back({"synth-own-md", [&, o](std::string& n) { return nr::probe::splice_loose(ref, rec, s.L.push, s.L.wg, o, n); }});
            }
            for (int v = 0; v < 4; ++v) if (!rv.ok || v != rv.abiver) {
                auto o = so; o.abiver = v;
                vs.push_back({fmt("synth-abiver%d", v), [&, o](std::string& n) { return nr::probe::splice_loose(ref, rec, s.L.push, s.L.wg, o, n); }});
            }
            std::map<uint64_t, std::string> seen;
            for (auto& v : vs) {
                if (run.lost) break;
                std::string notes; Bytes b;
                try { b = v.make(notes); } catch (const std::exception& e) { say(fmt("  %-20s not built (%s)", v.name.c_str(), e.what())); ++refused; continue; }
                const uint64_t h = nr::pal::hash(b.data(), b.size());
                save(out / "variants" / (s.name + "." + v.name + ".bin"), b);
                if (seen.count(h)) { say(fmt("  %-20s same bytes as %s", v.name.c_str(), seen[h].c_str())); continue; }
                seen[h] = v.name;
                Blob bl{own[0].key, b}; bl.key = our_key(bl.key, b, s.key);
                std::string why; double ims = 0; VkPipeline p = import(g, api, s, {bl}, why, &ims);
                if (!p) { say(fmt("  %-20s REFUSED: %s%s", v.name.c_str(), why.c_str(), notes.empty() ? "" : (" [" + notes + " ]").c_str())); ++refused; continue; }
                bool exact = false; auto res = run.check(s, p, &exact);
                say(fmt("  %-20s accepted in %.1f ms; %s%s", v.name.c_str(), ims, res.c_str(), notes.empty() ? "" : (" [" + notes + " ]").c_str()));
                if (exact) { ++exact_n; summary += " " + s.name + ":" + v.name; } else ++wrong;
                if (!run.lost) vkDestroyPipeline(g.dev, p, nullptr);
            }
            if (!run.lost) { vkDestroyPipeline(g.dev, native, nullptr); s.free(g); }
        }
    }
    g.done();
    say(""); say(fmt("RESULT variants bit-exact %d, refused/not built %d, wrong %d, tampered binaries accepted %d", exact_n, refused, wrong, accepted_tamper));
    if (!summary.empty()) say("bit-exact:" + summary);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 4 || (std::strcmp(argv[1], "capture") && std::strcmp(argv[1], "variants"))) {
        std::printf("usage: nr_aco_probe2 capture|variants <files> <out>\n"); return 2;
    }
    const fs::path dir = argv[2], out = argv[3];
    std::error_code ec; fs::create_directories(out, ec);
    g_report = std::fopen((out / (std::string("report-") + argv[1] + ".txt")).string().c_str(), "w");
    int r = 1;
    try { r = !std::strcmp(argv[1], "capture") ? phase_capture(dir, out) : phase_variants(dir, out); }
    catch (const std::exception& e) { say(std::string("FAIL ") + e.what()); }
    if (g_report) std::fclose(g_report);
    return r;
}
