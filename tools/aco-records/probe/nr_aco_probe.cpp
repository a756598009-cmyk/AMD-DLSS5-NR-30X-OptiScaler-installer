// nr_aco_probe: does this GPU's driver take ACO machine code through VK_KHR_pipeline_binary (the PAL adapter of
// nr_pal_binary.hpp), and does the imported code compute bit-exactly what the driver's own compile does?
//
//   nr_aco_probe <dir> <report.txt>
//
// <dir> holds probe_a.spv / probe_b.spv and records/<machine>/<fnv64 of the SPIR-V>.nrp for one or more machines.
// For each probe: the driver compiles the SPIR-V (the reference, also checked against the CPU), then each record
// is spliced into that compile's container and imported; its output is compared word for word. A record the adapter
// or the driver refuses is reported with the reason and the reference stays. Reads and writes only its own buffers.
#define NOMINMAX
#include "nr_pipeline_binary.hpp"
#include <windows.h>
#include <chrono>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static FILE* g_report = nullptr;
static void say(const char* fmt, ...) {
    char b[2048];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    std::printf("%s\n", b);
    if (g_report) { std::fprintf(g_report, "%s\n", b); std::fflush(g_report); }
}

struct Probe { const char* file; uint32_t gx, gy, push[4], push_bytes, w, h; };

struct Gpu {
    VkInstance inst{}; VkPhysicalDevice phys{}; VkDevice dev{}; VkQueue q{}; uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties mem{}; bool binaries = false;
    bool init() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "nr_aco_probe"; app.apiVersion = VK_API_VERSION_1_3;
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
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
        vkGetPhysicalDeviceProperties2(phys, &p2);
        say("GPU %s, device 0x%04x, driver %s %s (0x%08x)", p2.properties.deviceName, p2.properties.deviceID, drv.driverName,
            drv.driverInfo, p2.properties.driverVersion);
        vkGetPhysicalDeviceMemoryProperties(phys, &mem);
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq); vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf.data());
        for (uint32_t i = 0; i < nq; ++i) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ex(ne); vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, ex.data());
        auto has = [&](const char* e) { for (auto& x : ex) if (!std::strcmp(x.extensionName, e)) return true; return false; };
        VkPhysicalDevicePipelineBinaryFeaturesKHR pbf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR};
        VkPhysicalDeviceMaintenance5FeaturesKHR m5f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &pbf};
        VkPhysicalDeviceFeatures2 qf2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &m5f};
        const bool exts = has(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) && has(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        if (exts) vkGetPhysicalDeviceFeatures2(phys, &qf2);
        binaries = exts && pbf.pipelineBinaries && m5f.maintenance5;
        say("VK_KHR_pipeline_binary %s, maintenance5 %s, pipelineBinaries feature %s", has(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) ? "yes" : "no",
            has(VK_KHR_MAINTENANCE_5_EXTENSION_NAME) ? "yes" : "no", exts && pbf.pipelineBinaries ? "yes" : "no");
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.subgroupSizeControl = VK_TRUE; f13.computeFullSubgroups = VK_TRUE;
        VkPhysicalDevicePipelineBinaryFeaturesKHR pbe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, &f13};
        pbe.pipelineBinaries = VK_TRUE;
        VkPhysicalDeviceMaintenance5FeaturesKHR m5e{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &pbe};
        m5e.maintenance5 = VK_TRUE;
        std::vector<const char*> en;
        if (binaries) { en.push_back(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME); en.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME); }
        float prio = 1.f;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &prio;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, binaries ? (void*)&m5e : (void*)&f13};
        di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.enabledExtensionCount = uint32_t(en.size()); di.ppEnabledExtensionNames = en.data();
        if (VkResult r = vkCreateDevice(phys, &di, nullptr, &dev)) { say("FAIL vkCreateDevice %d", int(r)); return false; }
        vkGetDeviceQueue(dev, family, 0, &q);
        return true;
    }
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
};

static std::vector<uint32_t> read_words(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::vector<uint32_t> w(size_t(f.tellg()) / 4); f.seekg(0); f.read(reinterpret_cast<char*>(w.data()), std::streamsize(w.size() * 4));
    return w;
}

// The CPU's answer (as the shaders say).
static uint32_t cpu_a(const Probe& p, const std::vector<uint32_t>& a, uint32_t i) { return a[i] * p.push[0] + p.push[1] + ((i / 64) << 20); }
static uint32_t cpu_b(const Probe& p, const std::vector<uint32_t>& a, uint32_t x, uint32_t y) {
    const uint32_t wx = x / 64, wy = y / 2, lx = x % 64, ly = y % 2, l = ly * 64 + lx, m = 127 - l;
    const uint32_t mx = wx * 64 + m % 64, my = wy * 2 + m / 64;
    const uint32_t src = (mx < p.w && my < p.h) ? a[my * p.w + mx] : 0u;
    const uint32_t sg = l / 32;
    return (src * p.push[0] + p.push[1]) ^ (wx << 8) ^ (wy << 16) ^ (p.gx << 24) ^ (sg << 28);
}

int main(int argc, char** argv) {
    if (argc != 3) { std::printf("usage: nr_aco_probe <dir> <report.txt>\n"); return 2; }
    const fs::path dir = argv[1];
    g_report = std::fopen(argv[2], "w");
    Gpu g;
    if (!g.init()) return 1;
    const Probe probes[] = {{"probe_a.spv", 64, 1, {2654435761u, 12345u, 4000u, 0}, 12, 4000, 1},
                            {"probe_b.spv", 5, 7, {40503u, 977u, 300u, 13u}, 16, 300, 13}};
    VkCommandPool pool{}; VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = g.family;
    vkCreateCommandPool(g.dev, &cpi, nullptr, &pool);
    VkCommandBuffer cmd{}; VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g.dev, &cai, &cmd);
    VkFence fence{}; VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vkCreateFence(g.dev, &fci, nullptr, &fence);
    int failures = 0, imported_ok = 0, refused = 0, wrong = 0;
    for (const auto& p : probes) {
        const auto spv = read_words(dir / p.file);
        if (spv.empty()) { say("FAIL %s missing", p.file); ++failures; continue; }
        const uint64_t key = nr::pal::hash(spv.data(), spv.size() * 4);
        const uint32_t n = p.w * p.h;
        std::vector<uint32_t> input(n);
        std::mt19937 rng(7); for (auto& v : input) v = rng();
        auto in = g.buffer(n * 4), out = g.buffer(n * 4);
        std::memcpy(in.p, input.data(), n * 4);
        VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                              {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 2; li.pBindings = lb;
        VkDescriptorSetLayout dsl{}; vkCreateDescriptorSetLayout(g.dev, &li, nullptr, &dsl);
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, p.push_bytes};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &dsl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
        VkPipelineLayout layout{}; vkCreatePipelineLayout(g.dev, &pli, nullptr, &layout);
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
        VkDescriptorPool dp{}; vkCreateDescriptorPool(g.dev, &dpi, nullptr, &dp);
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
        VkDescriptorSet set{}; vkAllocateDescriptorSets(g.dev, &dai, &set);
        VkDescriptorBufferInfo bi[2] = {{in.b, 0, VK_WHOLE_SIZE}, {out.b, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi[0], nullptr},
                                     {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi[1], nullptr}};
        vkUpdateDescriptorSets(g.dev, 2, w, 0, nullptr);
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = spv.size() * 4; smi.pCode = spv.data();
        VkShaderModule module{}; vkCreateShaderModule(g.dev, &smi, nullptr, &module);
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO}; req.requiredSubgroupSize = 32;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &req}; ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module; ci.stage.pName = "main"; ci.layout = layout;
        auto run = [&](VkPipeline pipe, std::vector<uint32_t>& result) -> bool {
            std::memset(out.p, 0xAB, n * 4);
            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkBeginCommandBuffer(cmd, &cbi);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, p.push_bytes, p.push);
            vkCmdDispatch(cmd, p.gx, p.gy, 1);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            vkEndCommandBuffer(cmd);
            vkResetFences(g.dev, 1, &fence);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
            if (vkQueueSubmit(g.q, 1, &si, fence)) return false;
            if (vkWaitForFences(g.dev, 1, &fence, VK_TRUE, 10000000000ull)) return false;
            result.assign(out.p, out.p + n);
            return true;
        };
        // The reference: the driver's compile, against the CPU.
        VkPipeline native{};
        if (VkResult r = vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &native)) { say("FAIL %s: driver compile %d", p.file, int(r)); ++failures; continue; }
        std::vector<uint32_t> ref;
        if (!run(native, ref)) { say("FAIL %s: driver pipeline did not finish", p.file); ++failures; continue; }
        uint32_t cpu_bad = 0;
        for (uint32_t y = 0; y < p.h; ++y)
            for (uint32_t x = 0; x < p.w; ++x) {
                const uint32_t i = y * p.w + x;
                if (ref[i] != (p.h == 1 ? cpu_a(p, input, i) : cpu_b(p, input, x, y))) ++cpu_bad;
            }
        say("%s (key %016llx): driver compile %s the CPU (%u of %u words differ)", p.file, (unsigned long long)key,
            cpu_bad ? "DIFFERS FROM" : "matches", cpu_bad, n);
        if (cpu_bad) ++failures;
        std::array<uint32_t, 3> wg{};
        for (size_t q = 5; q < spv.size();) { auto len = spv[q] >> 16, op = spv[q] & 0xffff; if (op == 16 && len == 6 && spv[q + 2] == 17) wg = {spv[q + 3], spv[q + 4], spv[q + 5]}; if (!len) break; q += len; }
        // Every record for this SPIR-V, one machine folder each.
        std::error_code ec;
        for (const auto& m : fs::directory_iterator(dir / "records", ec)) {
            const fs::path rec = m.path() / (std::string(nr::binary::keyname(key)));
            if (!fs::exists(rec)) continue;
            const std::string tag = m.path().filename().string();
            if (!g.binaries) { say("  %s record: NOT TRIED (no pipeline binaries on this driver)", tag.c_str()); ++refused; continue; }
            try {
                const auto bytes = nr::pal::load(rec.string());
                const auto record = nr::pal::Record::parse(bytes);
                const auto t0 = std::chrono::steady_clock::now();
                VkPipeline imp = nr::binary::create(g.dev, ci, record, p.push_bytes, wg);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                std::vector<uint32_t> got;
                std::vector<uint32_t> again;
                const bool ran = run(imp, got) && run(imp, again);
                uint32_t bad = 0, unwritten = 0;
                if (ran) for (uint32_t i = 0; i < n; ++i) { bad += got[i] != ref[i]; unwritten += got[i] == 0xABABABABu; }
                const bool stable = ran && got == again;
                if (ran && !bad && stable) ++imported_ok; else ++wrong;
                say("  %s record (machine 0x%02x, %u bytes of code): imported in %.1f ms; %s", tag.c_str(), record.mach, unsigned(record.code.size()), ms,
                    !ran ? "the dispatch did not finish" : !bad && stable ? "BIT-EXACT with the driver's compile, 2 of 2 runs"
                    : ("WRONG: " + std::to_string(bad) + " of " + std::to_string(n) + " words differ (" + std::to_string(unwritten) +
                       " never written)" + (stable ? "" : ", runs differ")).c_str());
                vkDestroyPipeline(g.dev, imp, nullptr);
            } catch (const std::exception& e) {
                ++refused;
                say("  %s record: REFUSED (%s); the driver's compile would be used", tag.c_str(), e.what());
            }
        }
        vkDestroyPipeline(g.dev, native, nullptr);
    }
    say("RESULT imported bit-exact %d, refused %d, wrong %d, failures %d", imported_ok, refused, wrong, failures);
    if (g_report) std::fclose(g_report);
    return failures ? 1 : 0;
}
