// mz_rdna3.h. The probe shaders are rdna3_probe/*.comp, built by tools/build-mochizuki-runtime.cmd into headers
// (glslang --vn) of the same names.
#include "mz_rdna3.h"
#include "mz_interpose.h"

#include <intrin.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include "acc16.h"
#include "acc32.h"
#include "fraga.h"
#include "fragb.h"
#include "halves.h"
#include "kswap.h"

namespace mzr3
{
namespace
{
std::mutex g_fileMutex;

std::string Narrow(const std::wstring& w)
{
    std::string s(size_t(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr)),
                  '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s)
{
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), int(w.size()));
    return w;
}

// ---- the layout probes ----

struct Buffer
{
    VkDevice device {};
    VkBuffer buffer {};
    VkDeviceMemory memory {};
    void* data {};
    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer()
    {
        if (buffer)
            vkDestroyBuffer(device, buffer, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
    }
};

struct Gpu
{
    VkPhysicalDevice physical {};
    VkDevice device {};
    VkQueue queue {};
    uint32_t family = 0;

    bool Make(Buffer& b, size_t bytes) const
    {
        b.device = device;
        VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (vkCreateBuffer(device, &bi, nullptr, &b.buffer) != VK_SUCCESS)
            return false;
        VkMemoryRequirements mr {};
        vkGetBufferMemoryRequirements(device, b.buffer, &mr);
        VkPhysicalDeviceMemoryProperties mem {};
        vkGetPhysicalDeviceMemoryProperties(physical, &mem);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = 0;
        while (type < mem.memoryTypeCount &&
               !((mr.memoryTypeBits >> type & 1) && (mem.memoryTypes[type].propertyFlags & want) == want))
            ++type;
        VkMemoryAllocateInfo ma { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ma.allocationSize = mr.size;
        ma.memoryTypeIndex = type;
        if (type == mem.memoryTypeCount || vkAllocateMemory(device, &ma, nullptr, &b.memory) != VK_SUCCESS ||
            vkBindBufferMemory(device, b.buffer, b.memory, 0) != VK_SUCCESS ||
            vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.data) != VK_SUCCESS)
            return false;
        std::memset(b.data, 0, bytes);
        return true;
    }

    // One dispatch of `code` at wave32 over two storage buffers; empty when it ran, else why not.
    std::string Run(const uint32_t* code, size_t bytes, const Buffer& in, const Buffer& out, uint32_t groups) const
    {
        struct Objects
        {
            VkDevice d;
            VkShaderModule module {};
            VkDescriptorSetLayout dsl {};
            VkPipelineLayout layout {};
            VkPipeline pipeline {};
            VkDescriptorPool pool {};
            VkCommandPool commands {};
            VkFence fence {};
            ~Objects()
            {
                if (fence)
                    vkDestroyFence(d, fence, nullptr);
                if (commands)
                    vkDestroyCommandPool(d, commands, nullptr);
                if (pool)
                    vkDestroyDescriptorPool(d, pool, nullptr);
                if (pipeline)
                    vkDestroyPipeline(d, pipeline, nullptr);
                if (layout)
                    vkDestroyPipelineLayout(d, layout, nullptr);
                if (dsl)
                    vkDestroyDescriptorSetLayout(d, dsl, nullptr);
                if (module)
                    vkDestroyShaderModule(d, module, nullptr);
            }
        } o { device };
        const auto failed = [](const char* what, VkResult r) { return std::string(what) + " VkResult " + std::to_string(r); };
        VkShaderModuleCreateInfo smi { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smi.codeSize = bytes;
        smi.pCode = code;
        if (VkResult r = vkCreateShaderModule(device, &smi, nullptr, &o.module))
            return failed("vkCreateShaderModule", r);
        const VkDescriptorSetLayoutBinding lb[2] = {
            { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
        };
        VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 2;
        li.pBindings = lb;
        if (VkResult r = vkCreateDescriptorSetLayout(device, &li, nullptr, &o.dsl))
            return failed("vkCreateDescriptorSetLayout", r);
        VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &o.dsl;
        if (VkResult r = vkCreatePipelineLayout(device, &pli, nullptr, &o.layout))
            return failed("vkCreatePipelineLayout", r);
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO
        };
        req.requiredSubgroupSize = 32;
        VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &req };
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = o.module;
        cpi.stage.pName = "main";
        cpi.layout = o.layout;
        if (VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &o.pipeline))
            return failed("vkCreateComputePipelines", r);
        const VkDescriptorPoolSize ps { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
        VkDescriptorPoolCreateInfo dpi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = 1;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        if (VkResult r = vkCreateDescriptorPool(device, &dpi, nullptr, &o.pool))
            return failed("vkCreateDescriptorPool", r);
        VkDescriptorSetAllocateInfo dai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dai.descriptorPool = o.pool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &o.dsl;
        VkDescriptorSet set {};
        if (VkResult r = vkAllocateDescriptorSets(device, &dai, &set))
            return failed("vkAllocateDescriptorSets", r);
        const VkDescriptorBufferInfo bi[2] = { { in.buffer, 0, VK_WHOLE_SIZE }, { out.buffer, 0, VK_WHOLE_SIZE } };
        VkWriteDescriptorSet w[2] {};
        for (uint32_t i = 0; i < 2; ++i)
        {
            w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
        VkCommandPoolCreateInfo cpci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cpci.queueFamilyIndex = family;
        if (VkResult r = vkCreateCommandPool(device, &cpci, nullptr, &o.commands))
            return failed("vkCreateCommandPool", r);
        VkCommandBufferAllocateInfo cba { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cba.commandPool = o.commands;
        cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cba.commandBufferCount = 1;
        VkCommandBuffer cb {};
        if (VkResult r = vkAllocateCommandBuffers(device, &cba, &cb))
            return failed("vkAllocateCommandBuffers", r);
        VkCommandBufferBeginInfo bgi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bgi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, o.pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, o.layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cb, groups, 1, 1);
        VkMemoryBarrier mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
        vkEndCommandBuffer(cb);
        VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (VkResult r = vkCreateFence(device, &fi, nullptr, &o.fence))
            return failed("vkCreateFence", r);
        VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        if (VkResult r = vkQueueSubmit(queue, 1, &si, o.fence))
            return failed("vkQueueSubmit", r);
        if (VkResult r = vkWaitForFences(device, 1, &o.fence, VK_TRUE, 10'000'000'000ull))
            return failed("vkWaitForFences", r);
        return {};
    }
};

// Exact for the small integers and the e4m3 values used here (all normal halves).
uint16_t Half(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    const uint32_t s = (u >> 16) & 0x8000;
    if ((u & 0x7FFFFFFF) == 0)
        return uint16_t(s);
    return uint16_t(s | uint32_t(int((u >> 23) & 255) - 127 + 15) << 10 | (u & 0x7FFFFF) >> 13);
}

enum class Fragment
{
    Accumulator,
    A,
    B
};

// Which element each component of each lane holds, element (r, c) loaded as 16 r + c + 1.
std::string Map(const Gpu& g, const char* name, const uint32_t* code, size_t bytes, bool f32, Fragment kind)
{
    Buffer in, out;
    if (!g.Make(in, 256 * 4) || !g.Make(out, 2048 * 4))
        return std::string(name) + ": no host-visible buffer";
    for (int i = 0; i < 256; ++i)
        if (f32)
            static_cast<float*>(in.data)[i] = float(i + 1);
        else
            static_cast<uint16_t*>(in.data)[i] = Half(float(i + 1));
    float* o = static_cast<float*>(out.data);
    std::fill(o, o + 2048, -1.f);
    if (const std::string why = g.Run(code, bytes, in, out, 1); !why.empty())
        return std::string(name) + ": the probe did not run (" + why + ")";
    const unsigned len = unsigned(std::max(o[1024], 0.f)), want = kind == Fragment::Accumulator ? 8u : 16u;
    unsigned g11 = 0, g12 = 0, total = 0, firstLane = 0, firstComponent = 0;
    int firstSeen = -2;
    for (unsigned l = 0; l < 32; ++l)
        for (unsigned c = 0; c < len && c < 32; ++c)
        {
            const int v = int(o[32 * l + c]) - 1, r = v / 16, col = v % 16;
            const int k12 = int(8 * (l / 16) + c);
            const bool on11 = kind == Fragment::Accumulator ? r == int(2 * c + l / 16) && col == int(l % 16)
                              : kind == Fragment::B         ? r == int(c) && col == int(l % 16)
                                                            : r == int(l % 16) && col == int(c);
            const bool on12 = kind == Fragment::Accumulator ? r == k12 && col == int(l % 16)
                              : kind == Fragment::B         ? r == k12 && col == int(l % 16)
                                                            : r == int(l % 16) && col == k12;
            g11 += on11;
            g12 += on12;
            ++total;
            if (!on11 && firstSeen == -2)
            {
                firstSeen = v;
                firstLane = l;
                firstComponent = c;
            }
        }
    const char* expected = kind == Fragment::Accumulator ? "(2c + l/16, l%16)"
                           : kind == Fragment::B         ? "(c, l%16)"
                                                         : "(l%16, c)";
    const float wave = o[1025];
    if (len == want && total && g11 == total && wave == 32.f)
        return {};
    char text[320];
    if (wave != 32.f)
        std::snprintf(text, sizeof text, "%s: subgroup size %g, expected 32", name, wave);
    else if (len != want)
        std::snprintf(text, sizeof text, "%s: %u components a lane, expected %u", name, len, want);
    else if (total && g12 == total)
        std::snprintf(text, sizeof text,
                      "%s: component c of lane l holds element %s (gfx12's map), expected %s", name,
                      kind == Fragment::Accumulator ? "(8(l/16) + c, l%16)"
                      : kind == Fragment::B         ? "(8(l/16) + c, l%16)"
                                                    : "(l%16, 8(l/16) + c)",
                      expected);
    else
        std::snprintf(text, sizeof text,
                      "%s: %u of %u components off the map, lane %u component %u holds element (%d, %d), expected %s",
                      name, total - g11, total, firstLane, firstComponent, firstSeen < 0 ? -1 : firstSeen / 16,
                      firstSeen < 0 ? -1 : firstSeen % 16, expected);
    return text;
}

// Which lane half each output row is computed from (halves.comp): row m from half m%2 of both operands alone.
std::string Halves(const Gpu& g)
{
    const char* name = "wmma-lane-halves";
    Buffer in, out;
    if (!g.Make(in, 512 * 2) || !g.Make(out, 1024 * 4))
        return std::string(name) + ": no host-visible buffer";
    uint16_t* h = static_cast<uint16_t*>(in.data);
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j)
        {
            h[i * 16 + j] = Half(i == j ? 1.f : 0.f);
            h[256 + i * 16 + j] = Half(float(16 * i + j + 1));
        }
    if (const std::string why = g.Run(mzr3_probe_halves, sizeof mzr3_probe_halves, in, out, 4); !why.empty())
        return std::string(name) + ": the probe did not run (" + why + ")";
    const float* o = static_cast<const float*>(out.data);
    long wrong = 0, fromOther = 0;
    for (int mode = 0; mode < 4; ++mode)
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 16; ++j)
            {
                const float got = o[mode * 256 + i * 16 + j];
                const bool marked = mode == 0 || mode == 2 ? (i & 1) != 0 : (i & 1) == 0;
                const float data = float(16 * i + j + 1), marker = 1000.f + float(mode < 2 ? i : j);
                wrong += got != (marked ? marker : data);
                fromOther += got == (marked ? data : marker);
            }
    if (!wrong)
        return {};
    char text[200];
    std::snprintf(text, sizeof text,
                  "%s: %ld of 1024 product elements wrong (%ld taken from the other lane half), expected row m from "
                  "lane half m%%2 alone",
                  name, wrong, fromOther);
    return text;
}

float E4m3(std::mt19937& rng)
{
    for (;;)
    {
        const uint32_t b = rng() & 0xFF;
        if ((b & 0x7F) == 0x7F)
            continue; // NaN
        const int e = (b >> 3) & 15, m = b & 7;
        const float v = e ? std::ldexp(1.f + float(m) / 8.f, e - 7) : std::ldexp(float(m) / 8.f, -6);
        return b & 0x80 ? -v : v;
    }
}

// Every k pair of both operands swapped in the upper lane half (kswap.comp) changes no product, and the plain product
// is the CPU's.
std::string Kswap(const Gpu& g)
{
    const char* name = "kpair-swap";
    constexpr uint32_t tiles = 400;
    Buffer in, out;
    if (!g.Make(in, size_t(tiles) * 512 * 2) || !g.Make(out, size_t(tiles) * 512 * 4))
        return std::string(name) + ": no host-visible buffer";
    std::vector<float> a(size_t(tiles) * 512);
    std::mt19937 rng(12345);
    uint16_t* h = static_cast<uint16_t*>(in.data);
    for (size_t i = 0; i < a.size(); ++i)
        h[i] = Half(a[i] = E4m3(rng));
    if (const std::string why = g.Run(mzr3_probe_kswap, sizeof mzr3_probe_kswap, in, out, tiles); !why.empty())
        return std::string(name) + ": the probe did not run (" + why + ")";
    const float* o = static_cast<const float*>(out.data);
    long differ = 0, offRef = 0;
    for (uint32_t t = 0; t < tiles; ++t)
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 16; ++j)
            {
                const float plain = o[t * 512 + i * 16 + j], swapped = o[t * 512 + 256 + i * 16 + j];
                differ += std::memcmp(&plain, &swapped, 4) != 0;
                double ref = 0, mag = 0;
                for (int k = 0; k < 16; ++k)
                {
                    const double p = double(a[t * 512 + i * 16 + k]) * a[t * 512 + 256 + k * 16 + j];
                    ref += p;
                    mag += std::fabs(p);
                }
                offRef += std::fabs(plain - ref) / (mag + 1e-30) > 1e-5;
            }
    if (!differ && !offRef)
        return {};
    char text[200];
    std::snprintf(text, sizeof text,
                  "%s: %ld of %u products change with the upper half's k pairs swapped and %ld are off the CPU "
                  "product, expected none",
                  name, differ, tiles * 256, offRef);
    return text;
}

// ---- the pipeline check: the child ----

// binding -> descriptor type, from the module's variables (pipecheck's reflection).
std::map<uint32_t, VkDescriptorType> Bindings(const std::vector<uint32_t>& c)
{
    std::map<uint32_t, uint32_t> binding, ptrClass, ptrType, varPtr, imageSampled;
    std::map<uint32_t, int> kind; // 1 sampled image, 2 sampler
    std::map<uint32_t, VkDescriptorType> out;
    for (size_t i = 5; i < c.size();)
    {
        const uint32_t op = c[i] & 0xFFFFu, wc = c[i] >> 16;
        if (!wc || i + wc > c.size())
            break;
        if (op == 71 && wc >= 4 && c[i + 2] == 33)
            binding[c[i + 1]] = c[i + 3];
        else if (op == 25 && wc >= 9)
            imageSampled[c[i + 1]] = c[i + 7];
        else if (op == 27)
            kind[c[i + 1]] = 1;
        else if (op == 26)
            kind[c[i + 1]] = 2;
        else if (op == 32 && wc >= 4)
        {
            ptrClass[c[i + 1]] = c[i + 2];
            ptrType[c[i + 1]] = c[i + 3];
        }
        else if (op == 59 && wc >= 4)
            varPtr[c[i + 2]] = c[i + 1];
        i += wc;
    }
    for (const auto& [var, b] : binding)
    {
        const auto p = varPtr.find(var);
        if (p == varPtr.end())
            continue;
        const uint32_t sc = ptrClass[p->second], t = ptrType[p->second];
        VkDescriptorType d = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (sc == 2)
            d = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (sc == 0)
            d = kind[t] == 1                ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                : kind[t] == 2              ? VK_DESCRIPTOR_TYPE_SAMPLER
                : imageSampled[t] == 2      ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                            : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        out[b] = d;
    }
    return out;
}

struct Lines
{
    HANDLE file = INVALID_HANDLE_VALUE;
    std::mutex mutex;
    void Put(const std::string& line)
    {
        std::lock_guard lock(mutex);
        const std::string l = line + "\n";
        DWORD written = 0;
        WriteFile(file, l.data(), DWORD(l.size()), &written, nullptr);
    }
};

std::vector<std::wstring> ReadList(const std::wstring& path)
{
    std::vector<std::wstring> files;
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    for (std::string line; std::getline(f, line);)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            files.push_back(Wide(line));
    }
    return files;
}

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* e)
{
    return std::any_of(list.begin(), list.end(), [e](const VkExtensionProperties& x) { return !std::strcmp(x.extensionName, e); });
}

// The child: a device on the adapter, then every listed file compiled on `threads` threads. "start i" before file i
// and "ok i ms" or "fail i VkResult" after; "error ..." when no device could be made; "done" at the end. With `data`
// (the runtime's dlssnr-amd) and a prewarm manifest for data\shaders<variant>, a file the manifest names is compiled
// with the create infos it lists, its machine code kept in the folder `keep` (mzi::ManifestPipelines).
void Child(const LUID& luid, unsigned threads, const std::wstring& listPath, Lines& out, const std::wstring& data = {},
           const std::wstring& variant = {}, const std::wstring& keep = {})
{
    const std::vector<std::wstring> files = ReadList(listPath);
    // With the manifest the device is the network's own, as Vulkan::Create makes it (application name, features,
    // extensions): the driver keys its pipeline cache on them, and the build has to find what this compiles.
    const bool mirror = !data.empty() && !keep.empty();
    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = mirror ? "OptiScaler NR" : "OptiScaler NR pipeline check";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ii.pApplicationInfo = &app;
    VkInstance instance {};
    if (VkResult r = vkCreateInstance(&ii, nullptr, &instance))
        return out.Put("error vkCreateInstance VkResult " + std::to_string(r));
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(instance, &n, all.data());
    VkPhysicalDevice physical {};
    VkPhysicalDeviceProperties props {};
    for (VkPhysicalDevice p : all)
    {
        VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
        vkGetPhysicalDeviceProperties2(p, &p2);
        if (id.deviceLUIDValid && !std::memcmp(id.deviceLUID, &luid, sizeof luid) && p2.properties.apiVersion >= VK_API_VERSION_1_3)
        {
            physical = p;
            props = p2.properties;
            break;
        }
    }
    if (!physical)
        return out.Put("error no Vulkan 1.3 device on the adapter");
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> offered(en);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &en, offered.data());
    const bool robustExtension = HasExtension(offered, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
    VkPhysicalDevicePipelineRobustnessFeaturesEXT hasRobust { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT };
    VkPhysicalDeviceVulkan12Features has12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, robustExtension ? &hasRobust : nullptr };
    VkPhysicalDeviceFeatures2 has { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &has12 };
    vkGetPhysicalDeviceFeatures2(physical, &has);
    const bool robust = robustExtension && hasRobust.pipelineRobustness && !mirror;

    // What the network's device enables (MochizukiNrRuntime.cpp Vulkan::Create), FP8 left out, and the matrices too
    // where the driver has none (an RX 6000: the RDNA2 network has no cooperative matrices).
    const bool coopExtension =
        HasExtension(offered, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) && (!mirror || variant == L"-rdna3");
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
    VkPhysicalDeviceVulkan11Features f11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features f13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wml {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR
    };
    VkPhysicalDevicePipelineRobustnessFeaturesEXT rob { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT };
    coop.cooperativeMatrix = VK_TRUE;
    f11.storageBuffer16BitAccess = VK_TRUE;
    f12.storageBuffer8BitAccess = f12.shaderFloat16 = f12.shaderInt8 = f12.vulkanMemoryModel = VK_TRUE;
    f12.bufferDeviceAddress = has12.bufferDeviceAddress;
    f12.timelineSemaphore = mirror && has12.timelineSemaphore;
    f13.subgroupSizeControl = f13.synchronization2 = VK_TRUE;
    // RDNA2's INT8 network (v_dot4): its shaders declare the integer dot product (the runtime checked it is offered).
    f13.shaderIntegerDotProduct = variant == L"-rdna2-int8" ? VK_TRUE : VK_FALSE;
    wml.workgroupMemoryExplicitLayout = wml.workgroupMemoryExplicitLayout8BitAccess =
        wml.workgroupMemoryExplicitLayout16BitAccess = VK_TRUE;
    rob.pipelineRobustness = VK_TRUE;
    coop.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = &f13;
    f13.pNext = &wml;
    if (robust)
        wml.pNext = &rob;
    VkPhysicalDeviceFeatures2 f2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                   coopExtension ? static_cast<void*>(&coop) : &f11 };
    std::vector<const char*> extensions = { VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME };
    if (coopExtension)
        extensions.insert(extensions.begin(), VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    if (robust)
        extensions.push_back(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
    if (mirror)
        for (const char* e : { "VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32" })
            if (HasExtension(offered, e))
                extensions.push_back(e);
    mzi::BinaryProbe binaryFeatures; // VK_KHR_pipeline_binary, to keep the machine code
    if (mirror)
        wml.pNext = binaryFeatures.Prepare(physical, offered, extensions, wml.pNext, true);
    const float priority = 1.f;
    VkDeviceQueueCreateInfo qi { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2 };
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = uint32_t(extensions.size());
    di.ppEnabledExtensionNames = extensions.data();
    VkDevice device {};
    if (VkResult r = vkCreateDevice(physical, &di, nullptr, &device))
        return out.Put("error vkCreateDevice VkResult " + std::to_string(r));
    const uint32_t push = std::min<uint32_t>(props.limits.maxPushConstantsSize, 256);
    std::unique_ptr<mzi::ManifestPipelines> manifest;
    std::wstring binaries;
    if (mirror && binaryFeatures.active)
    {
        manifest = std::make_unique<mzi::ManifestPipelines>(data, variant);
        std::error_code ec;
        std::filesystem::remove_all(keep, ec);
        if (!manifest->Total() || !std::filesystem::create_directories(keep, ec))
            manifest.reset();
        else
        {
            binaries = keep;
            std::ofstream(std::filesystem::path(keep + L"\\identity.txt"), std::ios::binary) << mzi::BinaryIdentity(physical) << "\n";
        }
    }
    char crash[MAX_PATH] {}, hang[MAX_PATH] {};
    GetEnvironmentVariableA("MZ_TEST_RDNA3_COMPILE_CRASH", crash, MAX_PATH);
    GetEnvironmentVariableA("MZ_TEST_RDNA3_COMPILE_HANG", hang, MAX_PATH);

    std::atomic<size_t> next { 0 };
    const auto work = [&]
    {
        for (size_t i; (i = next++) < files.size();)
        {
            out.Put("start " + std::to_string(i));
            const std::string leaf = Narrow(std::filesystem::path(files[i]).filename().wstring());
            if (crash[0] && leaf == crash) // test hooks: a compiler that crashes or never returns on this file
                __fastfail(FAST_FAIL_FATAL_APP_EXIT);
            if (hang[0] && leaf == hang)
                Sleep(INFINITE);
            if (manifest && manifest->Count(files[i]))
            {
                const auto t0 = std::chrono::steady_clock::now();
                const VkResult r = manifest->Compile(device, files[i], binaries);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                out.Put(r ? "fail " + std::to_string(i) + " VkResult " + std::to_string(r)
                          : "ok " + std::to_string(i) + " " + std::to_string(int(ms)));
                continue;
            }
            std::ifstream f(std::filesystem::path(files[i]), std::ios::binary | std::ios::ate);
            std::vector<uint32_t> code(f ? size_t(f.tellg()) / 4 : 0);
            f.seekg(0);
            f.read(reinterpret_cast<char*>(code.data()), std::streamsize(code.size() * 4));
            if (code.empty() || code[0] != 0x07230203u)
            {
                out.Put("fail " + std::to_string(i) + " unreadable");
                continue;
            }
            std::vector<VkDescriptorSetLayoutBinding> lb;
            for (const auto& [b, t] : Bindings(code))
                lb.push_back({ b, t, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr });
            VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            li.bindingCount = uint32_t(lb.size());
            li.pBindings = lb.data();
            VkDescriptorSetLayout dsl {};
            vkCreateDescriptorSetLayout(device, &li, nullptr, &dsl);
            const VkPushConstantRange pcr { VK_SHADER_STAGE_COMPUTE_BIT, 0, push };
            VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pli.setLayoutCount = 1;
            pli.pSetLayouts = &dsl;
            pli.pushConstantRangeCount = 1;
            pli.pPushConstantRanges = &pcr;
            VkPipelineLayout layout {};
            vkCreatePipelineLayout(device, &pli, nullptr, &layout);
            VkShaderModuleCreateInfo smi { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            smi.codeSize = code.size() * 4;
            smi.pCode = code.data();
            VkShaderModule module {};
            vkCreateShaderModule(device, &smi, nullptr, &module);
            VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req {
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO
            };
            req.requiredSubgroupSize = 32;
            VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &req };
            cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpi.stage.module = module;
            cpi.stage.pName = "main";
            cpi.layout = layout;
            VkPipelineRobustnessCreateInfoEXT robustness { VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT };
            robustness.storageBuffers = robustness.uniformBuffers = robustness.vertexInputs =
                VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT;
            robustness.images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DISABLED_EXT;
            if (robust)
                cpi.pNext = &robustness;
            const auto t0 = std::chrono::steady_clock::now();
            VkPipeline pipeline {};
            const VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            out.Put(r ? "fail " + std::to_string(i) + " VkResult " + std::to_string(r)
                      : "ok " + std::to_string(i) + " " + std::to_string(int(ms)));
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyShaderModule(device, module, nullptr);
            vkDestroyPipelineLayout(device, layout, nullptr);
            vkDestroyDescriptorSetLayout(device, dsl, nullptr);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < std::max(threads, 1u); ++t)
        pool.emplace_back(work);
    work();
    for (auto& t : pool)
        t.join();
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    out.Put("done");
}

// ---- the pipeline check: the parent ----

std::wstring ModulePath()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModulePath), &self);
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(self, path.data(), DWORD(path.size())));
    return path;
}

std::vector<std::string> ReadLines(const std::wstring& path)
{
    std::vector<std::string> lines;
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    for (std::string line; std::getline(f, line);)
        lines.push_back(line);
    return lines;
}

struct Run
{
    bool started = false, finished = false, timedOut = false, stopped = false;
    DWORD exit = 0;
    std::string error;            // the child's "error ..." line
    std::set<size_t> inFlight;    // started, never finished
    std::map<size_t, std::string> failed;
    size_t compiled = 0;
    double slowestMs = 0;
    size_t slowest = 0;
};

// One child over `indices` of `files`, `threads` at a time. Waits for it, killing it after `timeoutMs` without
// progress or once `stop` is set.
Run RunChild(const LUID& luid, const std::vector<std::wstring>& files, const std::vector<size_t>& indices,
             unsigned threads, const std::wstring& work, const std::atomic<bool>* stop, DWORD timeoutMs,
             const std::wstring& extra = {})
{
    Run run;
    const std::wstring list = work + L"\\pipelines.txt", log = work + L"\\pipelines.log";
    {
        std::ofstream f(std::filesystem::path(list), std::ios::binary | std::ios::trunc);
        for (size_t i : indices)
            f << Narrow(files[i]) << "\n";
    }
    DeleteFileW(log.c_str());
    wchar_t system[MAX_PATH] {};
    GetSystemDirectoryW(system, MAX_PATH);
    wchar_t luidText[40];
    swprintf_s(luidText, L"%08lX%08lX", static_cast<unsigned long>(luid.HighPart), luid.LowPart);
    std::wstring cmd = L"\"" + std::wstring(system) + L"\\rundll32.exe\" \"" + ModulePath() +
                       L"\",MochizukiNrPipelineCheck " + luidText + L" " + std::to_wstring(threads) + L" \"" + list +
                       L"\" \"" + log + L"\"" + extra;
    STARTUPINFOW si { sizeof si };
    PROCESS_INFORMATION pi {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr, nullptr, &si, &pi))
    {
        run.error = "rundll32 did not start (error " + std::to_string(GetLastError()) + ")";
        return run;
    }
    run.started = true;
    size_t seen = 0;
    ULONGLONG progress = GetTickCount64();
    for (;;)
    {
        const bool ended = WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0;
        const std::vector<std::string> lines = ReadLines(log);
        if (lines.size() != seen)
        {
            seen = lines.size();
            progress = GetTickCount64();
        }
        if (ended)
            break;
        if ((stop && stop->load()) || GetTickCount64() - progress > timeoutMs)
        {
            (stop && stop->load() ? run.stopped : run.timedOut) = true;
            TerminateProcess(pi.hProcess, 0xDEAD);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }
    GetExitCodeProcess(pi.hProcess, &run.exit);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    for (const std::string& line : ReadLines(log))
    {
        std::istringstream in(line);
        std::string what;
        size_t i = 0;
        in >> what;
        if (what == "done")
            run.finished = true;
        else if (what == "error")
            run.error = line.substr(6);
        else if (in >> i && i < indices.size())
        {
            const size_t file = indices[i];
            if (what == "start")
                run.inFlight.insert(file);
            else
            {
                run.inFlight.erase(file);
                std::string rest;
                std::getline(in >> std::ws, rest);
                if (what == "ok")
                {
                    ++run.compiled;
                    if (const double ms = std::atof(rest.c_str()); ms > run.slowestMs)
                    {
                        run.slowestMs = ms;
                        run.slowest = file;
                    }
                }
                else
                    run.failed[file] = rest;
            }
        }
    }
    return run;
}

std::string Leaf(const std::wstring& path) { return Narrow(std::filesystem::path(path).filename().wstring()); }

std::string Exit(DWORD code)
{
    char text[24];
    std::snprintf(text, sizeof text, "exit 0x%08lX", static_cast<unsigned long>(code));
    return text;
}
} // namespace

std::string Key(uint32_t deviceId, uint32_t driverVersion)
{
    char text[24];
    std::snprintf(text, sizeof text, "%04x-%08x", deviceId, driverVersion);
    return text;
}

bool Cached(const std::wstring& file, const std::string& key, const char* stage, std::string& why)
{
    std::lock_guard lock(g_fileMutex);
    for (const std::string& line : ReadLines(file))
    {
        std::istringstream in(line);
        std::string k, s, verdict;
        if (!(in >> k >> s >> verdict) || k != key || s != stage)
            continue;
        std::getline(in >> std::ws, why);
        if (!why.empty() && why.back() == '\r')
            why.pop_back();
        if (verdict == "PASS")
            why.clear();
        return verdict == "PASS" || verdict == "FAIL";
    }
    return false;
}

void Keep(const std::wstring& file, const std::string& key, const char* stage, const std::string& why)
{
    std::lock_guard lock(g_fileMutex);
    std::vector<std::string> kept;
    for (const std::string& line : ReadLines(file))
    {
        std::istringstream in(line);
        std::string k, s;
        if ((in >> k >> s) && !(k == key && s == stage))
            kept.push_back(line);
    }
    std::ofstream f(std::filesystem::path(file), std::ios::binary | std::ios::trunc);
    for (const std::string& line : kept)
        f << line << "\n";
    f << key << " " << stage << " " << (why.empty() ? "PASS" : "FAIL " + why) << "\n";
}

std::string LayoutCheck(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family)
{
    const Gpu g { physical, device, queue, family };
    std::string failed;
    for (const std::string& why :
         { Map(g, "acc-f32", mzr3_probe_acc32, sizeof mzr3_probe_acc32, true, Fragment::Accumulator),
           Map(g, "acc-f16", mzr3_probe_acc16, sizeof mzr3_probe_acc16, false, Fragment::Accumulator),
           Map(g, "frag-a", mzr3_probe_fraga, sizeof mzr3_probe_fraga, false, Fragment::A),
           Map(g, "frag-b", mzr3_probe_fragb, sizeof mzr3_probe_fragb, false, Fragment::B), Halves(g), Kswap(g) })
        if (!why.empty())
            failed += (failed.empty() ? "" : "; ") + why;
    return failed;
}

std::string PipelineCheck(const LUID& luid, const std::vector<std::wstring>& files, const std::wstring& work,
                          const std::atomic<bool>* stop, bool& ran, std::string& note, unsigned timeout,
                          const std::wstring& data, const std::wstring& variant)
{
    const auto t0 = std::chrono::steady_clock::now();
    ran = false;
    CreateDirectoryW(work.c_str(), nullptr);
    DWORD timeoutMs = timeout;
    if (char t[16] {}; GetEnvironmentVariableA("MZ_TEST_RDNA3_TIMEOUT_MS", t, sizeof t))
        timeoutMs = DWORD(std::strtoul(t, nullptr, 10));
    // As the prewarm: below-normal priority, a few threads, so the game keeps its CPU.
    const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
    std::vector<size_t> all(files.size());
    for (size_t i = 0; i < all.size(); ++i)
        all[i] = i;
    const std::wstring keep = work + L"\\binaries";
    std::error_code ec;
    std::filesystem::remove_all(keep, ec);
    Run run = RunChild(luid, files, all, threads, work, stop, timeoutMs,
                       data.empty() ? std::wstring() : L" \"" + data + L"\" \"" + variant + L"\" \"" + keep + L"\"");
    const auto done = [&]
    {
        char text[200];
        std::snprintf(text, sizeof text, "%zu of %zu pipelines compiled in %.1f s on %u threads", run.compiled,
                      files.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), threads);
        note = text;
        if (run.compiled)
            note += ", the slowest " + Leaf(files[run.slowest]) + " " + std::to_string(int(run.slowestMs)) + " ms";
    };
    if (run.stopped)
    {
        done();
        return "stopped: the session is being destroyed";
    }
    if (!run.started || !run.error.empty())
    {
        done();
        return run.error;
    }
    ran = true;
    if (run.finished)
    {
        done();
        if (run.failed.empty())
            return {};
        const auto& [file, why] = *run.failed.begin();
        return "vkCreateComputePipelines failed on " + Leaf(files[file]) + " (" + why + ")";
    }
    // A crash or a hang with several pipelines in flight: each again alone, to name the one.
    std::vector<size_t> suspects(run.inFlight.begin(), run.inFlight.end());
    if (suspects.size() > 1)
        for (size_t file : suspects)
        {
            const Run alone = RunChild(luid, files, { file }, 1, work, stop, timeoutMs);
            if (alone.stopped)
            {
                done();
                ran = false;
                return "stopped: the session is being destroyed";
            }
            if (!alone.finished && alone.error.empty())
            {
                done();
                return Leaf(files[file]) + (alone.timedOut ? " did not compile in " + std::to_string(timeoutMs / 1000) + " s"
                                                          : " crashed AMD's shader compiler (" + Exit(alone.exit) + ")");
            }
        }
    done();
    std::string names;
    for (size_t file : suspects)
        names += (names.empty() ? "" : ", ") + Leaf(files[file]);
    if (suspects.empty())
        return "the check's process ended early (" + Exit(run.exit) + ")";
    if (run.timedOut)
        return names + " did not compile in " + std::to_string(timeoutMs / 1000) + " s";
    return (suspects.size() > 1 ? "AMD's shader compiler crashed on one of " : "") + names +
           (suspects.size() > 1 ? " (" : " crashed AMD's shader compiler (") + Exit(run.exit) + ")";
}
} // namespace mzr3

// rundll32 "<this DLL>",MochizukiNrPipelineCheck <adapter LUID> <threads> <list file> <log file>
// [<dlssnr-amd> <variant> <binaries folder>] (mzr3::PipelineCheck).
extern "C" __declspec(dllexport) void CALLBACK MochizukiNrPipelineCheckW(HWND, HINSTANCE, LPWSTR command, int)
{
    int n = 0;
    LPWSTR* argv = CommandLineToArgvW(command, &n);
    if (!argv || n < 4)
        ExitProcess(2);
    const unsigned long long id = std::wcstoull(argv[0], nullptr, 16);
    LUID luid {};
    luid.HighPart = LONG(id >> 32);
    luid.LowPart = DWORD(id);
    mzr3::Lines out;
    out.file = CreateFileW(argv[3], GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out.file == INVALID_HANDLE_VALUE)
        ExitProcess(3);
    if (n >= 7)
        mzr3::Child(luid, unsigned(std::wcstoul(argv[1], nullptr, 10)), argv[2], out, argv[4], argv[5], argv[6]);
    else
        mzr3::Child(luid, unsigned(std::wcstoul(argv[1], nullptr, 10)), argv[2], out);
    CloseHandle(out.file);
    ExitProcess(0);
}
