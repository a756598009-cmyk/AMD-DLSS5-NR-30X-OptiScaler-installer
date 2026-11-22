// Compiles one compute SPIR-V with Mesa's RADV (null winsys, ACO) for the GPU family in RADV_FORCE_FAMILY,
// the way the mochizuki runtime creates its pipelines (one descriptor set, storage buffers for the buffer
// bindings, storage images / combined image samplers as declared, one push range, subgroup size 32).
// The patched RADV (NR_RECORD_OUT) writes <prefix>.code and <prefix>.txt; make_record.py turns them into
// an NRPAL002 record.
//
//   nr_aco_capture <vulkan_radeon.dll> <shader.spv> <out prefix>
//
// No GPU is touched: RADV's null winsys only compiles. The Vulkan loader is not used either.
#include <vulkan/vulkan.h>
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

static void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) { std::fprintf(stderr, "%s: VkResult %d\n", what, int(r)); std::exit(1); }
}

struct Reflect {
    std::map<uint32_t, VkDescriptorType> bindings;   // set 0
    bool other_sets = false;
};

// Set 0 bindings and their descriptor types, from the module's decorations and variable types.
static Reflect reflect(const std::vector<uint32_t>& w) {
    std::map<uint32_t, uint32_t> set, binding, type_of_var, storage_of_var, pointee, opcode_of;
    std::map<uint32_t, std::vector<uint32_t>> types;
    std::map<uint32_t, bool> buffer_block;
    for (size_t p = 5; p < w.size();) {
        const uint32_t n = w[p] >> 16, op = w[p] & 0xffff;
        if (!n) break;
        const uint32_t* a = &w[p + 1];
        if (op == 71 && n >= 4) {   // OpDecorate
            if (a[1] == 34) set[a[0]] = a[2];
            if (a[1] == 33) binding[a[0]] = a[2];
            if (a[1] == 3) buffer_block[a[0]] = true;
        } else if (op == 59 && n >= 4) {   // OpVariable
            type_of_var[a[1]] = a[0];
            storage_of_var[a[1]] = a[2];
        } else if (op == 32 && n == 4) {   // OpTypePointer
            pointee[a[0]] = a[2];
            opcode_of[a[0]] = op;
        } else if (op >= 19 && op <= 39) {   // type declarations
            opcode_of[a[0]] = op;
            types[a[0]] = std::vector<uint32_t>(a + 1, a + n - 1);
        }
        p += n;
    }
    Reflect r;
    for (const auto& [var, b] : binding) {
        if (!set.count(var)) continue;
        if (set[var] != 0) { r.other_sets = true; continue; }
        uint32_t t = pointee[type_of_var[var]];
        while (opcode_of[t] == 28 || opcode_of[t] == 29) t = types[t][0];   // arrays
        VkDescriptorType d = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        const uint32_t sc = storage_of_var[var];
        if (sc == 12 || (sc == 2 && buffer_block[t])) d = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (sc == 2) d = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (opcode_of[t] == 27) d = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;   // OpTypeSampledImage
        else if (opcode_of[t] == 25) d = types[t][5] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (opcode_of[t] == 26) d = VK_DESCRIPTOR_TYPE_SAMPLER;
        r.bindings[b] = d;
    }
    return r;
}

int main(int argc, char** argv) {
    if (argc != 4) { std::fprintf(stderr, "usage: %s <vulkan_radeon.dll> <shader.spv> <out prefix>\n", argv[0]); return 2; }
    std::ifstream in(argv[2], std::ios::binary | std::ios::ate);
    if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
    const size_t bytes = size_t(in.tellg());
    std::vector<uint32_t> spv(bytes / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(spv.data()), std::streamsize(bytes));
    if (bytes % 4 || spv.size() < 5 || spv[0] != 0x07230203) { std::fprintf(stderr, "not SPIR-V\n"); return 2; }
    const Reflect rf = reflect(spv);
    if (rf.other_sets) { std::fprintf(stderr, "descriptor sets other than 0 are not supported\n"); return 3; }

    HMODULE icd = LoadLibraryA(argv[1]);
    if (!icd) { std::fprintf(stderr, "cannot load %s\n", argv[1]); return 2; }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(icd, "vk_icdGetInstanceProcAddr"));
    if (auto negotiate = reinterpret_cast<VkResult (VKAPI_PTR*)(uint32_t*)>(
            GetProcAddress(icd, "vk_icdNegotiateLoaderICDInterfaceVersion"))) {
        uint32_t v = 5;
        negotiate(&v);
    }
    if (!gipa) { std::fprintf(stderr, "no vk_icdGetInstanceProcAddr\n"); return 2; }
#define IFN(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
    VkInstance instance = VK_NULL_HANDLE;
    IFN(vkCreateInstance);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nr-aco-capture";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    check(vkCreateInstance(&ii, nullptr, &instance), "vkCreateInstance");
    IFN(vkEnumeratePhysicalDevices);
    IFN(vkGetPhysicalDeviceProperties);
    IFN(vkCreateDevice);
    IFN(vkGetDeviceProcAddr);
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate");
    if (!count) { std::fprintf(stderr, "no RADV device (RADV_FORCE_FAMILY set?)\n"); return 1; }
    std::vector<VkPhysicalDevice> pds(count);
    check(vkEnumeratePhysicalDevices(instance, &count, pds.data()), "enumerate");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pds[0], &props);
    std::fprintf(stderr, "device: %s\n", props.deviceName);

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.subgroupSizeControl = VK_TRUE; f13.computeFullSubgroups = VK_TRUE; f13.maintenance4 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    f12.shaderFloat16 = VK_TRUE; f12.shaderInt8 = VK_TRUE; f12.storageBuffer8BitAccess = VK_TRUE;
    f12.uniformAndStorageBuffer8BitAccess = VK_TRUE; f12.vulkanMemoryModel = VK_TRUE;
    f12.vulkanMemoryModelDeviceScope = VK_TRUE; f12.bufferDeviceAddress = VK_TRUE;
    f12.shaderSubgroupExtendedTypes = VK_TRUE; f12.scalarBlockLayout = VK_TRUE;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    f11.storageBuffer16BitAccess = VK_TRUE; f11.uniformAndStorageBuffer16BitAccess = VK_TRUE;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, &f11};
    coop.cooperativeMatrix = VK_TRUE;
    VkPhysicalDeviceShaderFloat8FeaturesEXT fp8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT, &coop};
    fp8.shaderFloat8 = VK_TRUE; fp8.shaderFloat8CooperativeMatrix = VK_TRUE;
    VkPhysicalDeviceFeatures2 feats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &coop};
    feats.features.shaderInt16 = VK_TRUE; feats.features.shaderInt64 = VK_TRUE;
    std::vector<const char*> ext = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME};
    if (getenv("NR_CAPTURE_NO_COOP")) { ext.clear(); feats.pNext = &f11; }   // gfx10.3 has no cooperative matrices
    if (getenv("NR_CAPTURE_FP8")) { ext.push_back(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME); feats.pNext = &fp8; }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &feats};
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = uint32_t(ext.size()); di.ppEnabledExtensionNames = ext.data();
    VkDevice device{};
    check(vkCreateDevice(pds[0], &di, nullptr, &device), "vkCreateDevice");
#define DFN(name) auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name))
    DFN(vkCreateDescriptorSetLayout); DFN(vkCreatePipelineLayout); DFN(vkCreateShaderModule);
    DFN(vkCreateComputePipelines);

    // The runtime's layout: buffer bindings 0..n-1 (holes filled with storage buffers), then what the module
    // declares above them (images, the alias views at 16 + k).
    std::vector<VkDescriptorSetLayoutBinding> lb;
    uint32_t dense = 0;
    for (const auto& [b, t] : rf.bindings) if (b < 16 && t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) dense = std::max(dense, b + 1);
    for (uint32_t b = 0; b < dense; ++b)
        lb.push_back({b, rf.bindings.count(b) ? rf.bindings.at(b) : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    for (const auto& [b, t] : rf.bindings) if (b >= dense) lb.push_back({b, t, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    // NR_CAPTURE_LAYOUT="<binding>:<VkDescriptorType> ..." (the runtime's MZ_LOG_LAYOUT line): the exact layout the
    // runtime makes for this kernel, which the descriptor offsets in the code must match.
    if (const char* l = getenv("NR_CAPTURE_LAYOUT"); l && *l) {
        lb.clear();
        unsigned b = 0, t = 0;
        for (const char* p = l; sscanf(p, " %u:%u", &b, &t) == 2;) {
            lb.push_back({b, VkDescriptorType(t), 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
            while (*p == ' ') ++p;
            while (*p && *p != ' ') ++p;
        }
        for (const auto& [rb, rt] : rf.bindings) {
            bool found = false;
            for (const auto& x : lb) found |= x.binding == rb && x.descriptorType == rt;
            if (!found) { std::fprintf(stderr, "NR_CAPTURE_LAYOUT lacks binding %u type %d\n", rb, int(rt)); return 3; }
        }
    }
    for (const auto& l : lb) std::fprintf(stderr, "binding %u type %d\n", l.binding, int(l.descriptorType));
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = uint32_t(lb.size()); li.pBindings = lb.data();
    VkDescriptorSetLayout dsl{};
    check(vkCreateDescriptorSetLayout(device, &li, nullptr, &dsl), "set layout");
    const uint32_t push = getenv("NR_CAPTURE_PUSH") ? uint32_t(atoi(getenv("NR_CAPTURE_PUSH"))) : 128;
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &dsl;
    pli.pushConstantRangeCount = push ? 1 : 0; pli.pPushConstantRanges = &pcr;
    VkPipelineLayout layout{};
    check(vkCreatePipelineLayout(device, &pli, nullptr, &layout), "pipeline layout");
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = bytes; smi.pCode = spv.data();
    VkShaderModule module{};
    check(vkCreateShaderModule(device, &smi, nullptr, &module), "shader module");
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    req.requiredSubgroupSize = 32;
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &req};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = module; cpi.stage.pName = "main";
    cpi.layout = layout;
    _putenv_s("NR_RECORD_OUT", argv[3]);
    SetEnvironmentVariableA("NR_RECORD_OUT", argv[3]);
    VkPipeline pipeline{};
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline), "compute pipeline");
    std::fprintf(stderr, "compiled %s\n", argv[2]);
    // Exit without tearing down: the process ends here.
    std::fflush(stderr);
    ExitProcess(0);
}
