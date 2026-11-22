// pipecheck <folder or .spv> ...: compiles every compute shader given (a folder: its *.spv) on the first AMD Vulkan
// device as the runtime does (wave32, robust access off, a descriptor layout read from the module, no pipeline
// cache), each in a child process (pipecheck --one <file>) with a time limit, since a compiler crash ends the
// process. One line a module: "PIPE <file> OK|FAIL|CRASH|TIMEOUT ...".
#include "child.hpp"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

static std::vector<uint32_t> Read(const std::wstring& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::vector<uint32_t> code(size_t(f.tellg()) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(code.data()), std::streamsize(code.size() * 4));
    return code;
}

// binding -> descriptor type, from the module's variables in set 0
static std::map<uint32_t, VkDescriptorType> Bindings(const std::vector<uint32_t>& c)
{
    std::map<uint32_t, uint32_t> binding, ptrClass, ptrType, varPtr;
    std::map<uint32_t, uint32_t> imageSampled;
    std::map<uint32_t, int> kind; // 1 sampled image, 2 sampler
    std::map<uint32_t, VkDescriptorType> out;
    for (size_t i = 5; i < c.size();)
    {
        const uint32_t op = c[i] & 0xFFFFu, wc = c[i] >> 16;
        if (!wc) break;
        if (op == 71 && c[i + 2] == 33) binding[c[i + 1]] = c[i + 3];
        else if (op == 25) imageSampled[c[i + 1]] = c[i + 7];       // OpTypeImage: sampled at word 7
        else if (op == 27) kind[c[i + 1]] = 1;                       // OpTypeSampledImage
        else if (op == 26) kind[c[i + 1]] = 2;                       // OpTypeSampler
        else if (op == 32) { ptrClass[c[i + 1]] = c[i + 2]; ptrType[c[i + 1]] = c[i + 3]; }
        else if (op == 59) varPtr[c[i + 2]] = c[i + 1];
        i += wc;
    }
    for (const auto& [var, b] : binding)
    {
        const auto p = varPtr.find(var);
        if (p == varPtr.end()) continue;
        const uint32_t sc = ptrClass[p->second], t = ptrType[p->second];
        VkDescriptorType d = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (sc == 2) d = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (sc == 0)
            d = kind[t] == 1 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
              : kind[t] == 2 ? VK_DESCRIPTOR_TYPE_SAMPLER
              : imageSampled[t] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        out[b] = d;
    }
    return out;
}

static bool HasExt(VkPhysicalDevice p, const char* e)
{
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(p, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> x(n);
    vkEnumerateDeviceExtensionProperties(p, nullptr, &n, x.data());
    return std::any_of(x.begin(), x.end(), [e](const VkExtensionProperties& v) { return !std::strcmp(v.extensionName, e); });
}

static int One(const std::wstring& path, const std::string& name)
{
    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "pipecheck";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ii.pApplicationInfo = &app;
    VkInstance instance {};
    if (VkResult r = vkCreateInstance(&ii, nullptr, &instance))
    {
        std::printf("PIPE %s FAIL vkCreateInstance VkResult %d\n", name.c_str(), r);
        return 4;
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(instance, &n, all.data());
    VkPhysicalDevice phys {};
    VkPhysicalDeviceProperties props {};
    for (int pass = 0; pass < 2 && !phys; ++pass)
        for (auto p : all)
        {
            vkGetPhysicalDeviceProperties(p, &props);
            if (props.vendorID == 0x1002 && (pass || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))
            {
                phys = p;
                break;
            }
        }
    if (!phys)
    {
        std::printf("PIPE %s FAIL no AMD Vulkan device\n", name.c_str());
        return 4;
    }
    const bool robustExt = HasExt(phys, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
    VkPhysicalDevicePipelineRobustnessFeaturesEXT hasRob { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT };
    VkPhysicalDeviceVulkan12Features has12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, robustExt ? &hasRob : nullptr };
    VkPhysicalDeviceFeatures2 has { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &has12 };
    vkGetPhysicalDeviceFeatures2(phys, &has);
    const bool robust = robustExt && hasRob.pipelineRobustness;

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
    f12.bufferDeviceAddress = has12.bufferDeviceAddress;
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
    qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2 };
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = uint32_t(ext.size()); di.ppEnabledExtensionNames = ext.data();
    VkDevice dev {};
    if (VkResult r = vkCreateDevice(phys, &di, nullptr, &dev))
    {
        std::printf("PIPE %s FAIL vkCreateDevice VkResult %d\n", name.c_str(), r);
        return 4;
    }
    const uint32_t push = std::min<uint32_t>(props.limits.maxPushConstantsSize, 256);
    const std::vector<uint32_t> code = Read(path);
    if (code.empty() || code[0] != 0x07230203u)
    {
        std::printf("PIPE %s FAIL unreadable\n", name.c_str());
        return 1;
    }
    std::vector<VkDescriptorSetLayoutBinding> lb;
    for (const auto& [b, t] : Bindings(code)) lb.push_back({ b, t, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr });
    VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = uint32_t(lb.size()); li.pBindings = lb.data();
    VkDescriptorSetLayout dsl {};
    vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl);
    VkPushConstantRange pcr { VK_SHADER_STAGE_COMPUTE_BIT, 0, push };
    VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VkPipelineLayout layout {};
    vkCreatePipelineLayout(dev, &pli, nullptr, &layout);
    VkShaderModuleCreateInfo smi { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = code.size() * 4; smi.pCode = code.data();
    VkShaderModule module {};
    vkCreateShaderModule(dev, &smi, nullptr, &module);
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO };
    req.requiredSubgroupSize = 32;
    VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &req };
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = module; cpi.stage.pName = "main";
    cpi.layout = layout;
    VkPipelineRobustnessCreateInfoEXT robustness { VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT };
    robustness.storageBuffers = robustness.uniformBuffers = robustness.vertexInputs =
        VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT;
    robustness.images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DISABLED_EXT;
    if (robust) cpi.pNext = &robustness;
    const auto t0 = std::chrono::steady_clock::now();
    VkPipeline pipe {};
    const VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r)
        std::printf("PIPE %s FAIL VkResult %d after %.0f ms\n", name.c_str(), r, ms);
    else
        std::printf("PIPE %s OK %.0f ms\n", name.c_str(), ms);
    vkDestroyPipeline(dev, pipe, nullptr);
    vkDestroyShaderModule(dev, module, nullptr);
    vkDestroyPipelineLayout(dev, layout, nullptr);
    vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(instance, nullptr);
    return r ? 1 : 0;
}

static std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += c < 128 ? char(c) : '?';
    return s;
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 3 && !std::wcscmp(argv[1], L"--one"))
        return One(argv[2], Narrow(argv[2]));
    if (!LoadLibraryW(L"vulkan-1.dll"))
    {
        std::printf("no Vulkan loader (vulkan-1.dll)\n");
        return 1;
    }
    DWORD limit = 300000;
    if (const char* e = std::getenv("PIPECHECK_TIMEOUT_S")) limit = DWORD(std::atoi(e)) * 1000;
    std::vector<std::wstring> files;
    for (int a = 1; a < argc; ++a)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(argv[a], ec))
        {
            std::vector<std::wstring> here;
            for (const auto& e : std::filesystem::directory_iterator(argv[a], ec))
                if (e.path().extension() == L".spv") here.push_back(e.path().wstring());
            std::sort(here.begin(), here.end());
            files.insert(files.end(), here.begin(), here.end());
        }
        else
            files.push_back(argv[a]);
    }
    int ok = 0, bad = 0;
    for (const auto& f : files)
    {
        double ms = 0;
        const long code = RunChild(L"--one \"" + f + L"\"", limit, &ms);
        code == 0 ? ++ok : ++bad;
        if (code == -1)
            std::printf("PIPE %s TIMEOUT after %.0f s\n", Narrow(f).c_str(), ms / 1000);
        else if (Crashed(code) || code == -2)
            std::printf("PIPE %s CRASH exit 0x%08lx after %.0f ms\n", Narrow(f).c_str(), (unsigned long) code, ms);
    }
    std::printf("pipelines: %d OK, %d not\n", ok, bad);
    return bad ? 1 : 0;
}
