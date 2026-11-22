// vkprobe <probe dir>: Windows, DXGI and Vulkan facts of every GPU, then the fragment layout checks the RDNA3
// network relies on, each in a child process (vkprobe --check <name> <probe dir>) so a driver crash ends only that
// check. Each check prints "CHECK <name> PASS|FAIL|SKIPPED ..." and exits 0, 1 or 3.
#include "child.hpp"
#include <dxgi1_6.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

static const char* Comp(VkComponentTypeKHR t)
{
    switch (t)
    {
    case VK_COMPONENT_TYPE_FLOAT16_KHR: return "f16";
    case VK_COMPONENT_TYPE_FLOAT32_KHR: return "f32";
    case VK_COMPONENT_TYPE_FLOAT64_KHR: return "f64";
    case VK_COMPONENT_TYPE_SINT8_KHR: return "s8";
    case VK_COMPONENT_TYPE_SINT16_KHR: return "s16";
    case VK_COMPONENT_TYPE_SINT32_KHR: return "s32";
    case VK_COMPONENT_TYPE_SINT64_KHR: return "s64";
    case VK_COMPONENT_TYPE_UINT8_KHR: return "u8";
    case VK_COMPONENT_TYPE_UINT16_KHR: return "u16";
    case VK_COMPONENT_TYPE_UINT32_KHR: return "u32";
    case VK_COMPONENT_TYPE_UINT64_KHR: return "u64";
    case VK_COMPONENT_TYPE_BFLOAT16_KHR: return "bf16";
    case VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT: return "e4m3";
    case VK_COMPONENT_TYPE_FLOAT8_E5M2_EXT: return "e5m2";
    default: return "?";
    }
}

static std::string Reg(HKEY root, const char* key, const char* value)
{
    char buf[256] {};
    DWORD n = sizeof buf;
    if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &n) != ERROR_SUCCESS)
        return "";
    return buf;
}

static void Windows()
{
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v { sizeof v };
    if (auto f = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        f(&v);
    DWORD ubr = 0, n = sizeof ubr;
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "UBR", RRF_RT_REG_DWORD,
                 nullptr, &ubr, &n);
    const char* nt = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::printf("windows: %s %s, build %lu.%lu.%lu.%lu\n", Reg(HKEY_LOCAL_MACHINE, nt, "ProductName").c_str(),
                Reg(HKEY_LOCAL_MACHINE, nt, "DisplayVersion").c_str(), v.dwMajorVersion, v.dwMinorVersion,
                v.dwBuildNumber, ubr);
    const char* cn = "SOFTWARE\\AMD\\CN";
    std::printf("amd software: DriverVersion %s, CNVersion %s\n", Reg(HKEY_LOCAL_MACHINE, cn, "DriverVersion").c_str(),
                Reg(HKEY_LOCAL_MACHINE, cn, "CNVersion").c_str());
    SYSTEM_INFO si {};
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms { sizeof ms };
    GlobalMemoryStatusEx(&ms);
    std::printf("cpu threads %lu, system memory %.1f GB\n", si.dwNumberOfProcessors, double(ms.ullTotalPhys) / (1 << 30));
}

static void Dxgi()
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        std::printf("dxgi: no factory\n");
        return;
    }
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d {};
        a->GetDesc1(&d);
        LARGE_INTEGER umd {};
        const bool hasUmd = SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
        char name[256] {};
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
        std::printf("dxgi adapter %u: %s, vendor %04x device %04x subsys %08x rev %02x, dedicated %llu MB, shared %llu MB",
                    i, name, d.VendorId, d.DeviceId, d.SubSysId, d.Revision,
                    (unsigned long long) (d.DedicatedVideoMemory >> 20), (unsigned long long) (d.SharedSystemMemory >> 20));
        if (hasUmd)
            std::printf(", driver %u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart),
                        LOWORD(umd.LowPart));
        std::printf("\n");
        a->Release();
    }
    factory->Release();
}

static bool Has(const std::vector<VkExtensionProperties>& list, const char* e)
{
    return std::any_of(list.begin(), list.end(), [e](const VkExtensionProperties& x) { return !std::strcmp(x.extensionName, e); });
}

static VkInstance MakeInstance()
{
    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "vkprobe";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ii.pApplicationInfo = &app;
    VkInstance instance {};
    if (VkResult r = vkCreateInstance(&ii, nullptr, &instance))
    {
        std::printf("vkCreateInstance VkResult %d\n", r);
        return nullptr;
    }
    return instance;
}

static std::vector<VkCooperativeMatrixPropertiesKHR> CoopProps(VkInstance inst, VkPhysicalDevice p)
{
    auto q = reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
        vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
    uint32_t n = 0;
    if (!q || q(p, &n, nullptr) != VK_SUCCESS)
        return {};
    std::vector<VkCooperativeMatrixPropertiesKHR> m(n, { VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR });
    q(p, &n, m.data());
    m.resize(n);
    return m;
}

static bool HasConfig(const std::vector<VkCooperativeMatrixPropertiesKHR>& m, VkComponentTypeKHR c)
{
    return std::any_of(m.begin(), m.end(), [c](const VkCooperativeMatrixPropertiesKHR& x) {
        return x.MSize == 16 && x.NSize == 16 && x.KSize == 16 && x.scope == VK_SCOPE_SUBGROUP_KHR &&
               x.AType == VK_COMPONENT_TYPE_FLOAT16_KHR && x.BType == VK_COMPONENT_TYPE_FLOAT16_KHR && x.CType == c &&
               x.ResultType == c && !x.saturatingAccumulation;
    });
}

static void Device(VkInstance inst, uint32_t index, VkPhysicalDevice p)
{
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(p, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> ext(n);
    vkEnumerateDeviceExtensionProperties(p, nullptr, &n, ext.data());
    const bool coop = Has(ext, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME), fp8 = Has(ext, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME),
               bin = Has(ext, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME), m5 = Has(ext, VK_KHR_MAINTENANCE_5_EXTENSION_NAME),
               wml = Has(ext, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME),
               rob = Has(ext, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);

    VkPhysicalDeviceDriverProperties drv { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceVulkan13Properties p13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, &drv };
    VkPhysicalDeviceSubgroupProperties sub { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, &p13 };
    VkPhysicalDevicePipelineBinaryPropertiesKHR binp { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_PROPERTIES_KHR, &sub };
    VkPhysicalDeviceCooperativeMatrixPropertiesKHR coopp { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR,
                                                           bin ? static_cast<void*>(&binp) : &sub };
    VkPhysicalDeviceProperties2 pp { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, coop ? &coopp : coopp.pNext };
    vkGetPhysicalDeviceProperties2(p, &pp);
    const VkPhysicalDeviceProperties& pr = pp.properties;
    const char* type = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? "discrete"
                       : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                                                                                 : "other";
    std::printf("\nvulkan device %u: %s (%s), vendor %04x device %04x\n", index, pr.deviceName, type, pr.vendorID, pr.deviceID);
    std::printf("  api %u.%u.%u, driverVersion 0x%08x, driver id %d \"%s\" \"%s\", conformance %u.%u.%u.%u\n",
                VK_API_VERSION_MAJOR(pr.apiVersion), VK_API_VERSION_MINOR(pr.apiVersion), VK_API_VERSION_PATCH(pr.apiVersion),
                pr.driverVersion, int(drv.driverID), drv.driverName, drv.driverInfo, drv.conformanceVersion.major,
                drv.conformanceVersion.minor, drv.conformanceVersion.subminor, drv.conformanceVersion.patch);
    std::printf("  subgroup size %u, size control %u..%u, required-size stages 0x%x, compute workgroup subgroups %u\n",
                sub.subgroupSize, p13.minSubgroupSize, p13.maxSubgroupSize, p13.requiredSubgroupSizeStages,
                p13.maxComputeWorkgroupSubgroups);
    std::printf("  max shared memory %u, max push constants %u\n", pr.limits.maxComputeSharedMemorySize,
                pr.limits.maxPushConstantsSize);

    VkPhysicalDeviceShaderFloat8FeaturesEXT f8 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT };
    VkPhysicalDevicePipelineBinaryFeaturesKHR binf { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR };
    VkPhysicalDeviceMaintenance5FeaturesKHR m5f { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR };
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wmlf {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR };
    VkPhysicalDevicePipelineRobustnessFeaturesEXT robf { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT };
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coopf { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
    VkPhysicalDeviceVulkan13Features f13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13 };
    VkPhysicalDeviceVulkan11Features f11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12 };
    VkPhysicalDeviceFeatures2 ff { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11 };
    void** tail = &f13.pNext;
    const auto chain = [&](bool on, auto& s) { if (on) { *tail = &s; tail = &s.pNext; } };
    chain(fp8, f8);
    chain(bin, binf);
    chain(m5, m5f);
    chain(wml, wmlf);
    chain(rob, robf);
    chain(coop, coopf);
    vkGetPhysicalDeviceFeatures2(p, &ff);
    std::printf("  shaderFloat16 %u, shaderInt8 %u, shaderInt16 %u, storageBuffer8BitAccess %u, storageBuffer16BitAccess %u,"
                " bufferDeviceAddress %u, vulkanMemoryModel %u (device scope %u), subgroupSizeControl %u,"
                " computeFullSubgroups %u, synchronization2 %u, maintenance4 %u\n",
                f12.shaderFloat16, f12.shaderInt8, ff.features.shaderInt16, f12.storageBuffer8BitAccess,
                f11.storageBuffer16BitAccess, f12.bufferDeviceAddress, f12.vulkanMemoryModel,
                f12.vulkanMemoryModelDeviceScope, f13.subgroupSizeControl, f13.computeFullSubgroups,
                f13.synchronization2, f13.maintenance4);
    std::printf("  VK_KHR_cooperative_matrix %s", coop ? "yes" : "no");
    if (coop)
        std::printf(" (cooperativeMatrix %u, robustBufferAccess %u, stages 0x%x)", coopf.cooperativeMatrix,
                    coopf.cooperativeMatrixRobustBufferAccess, coopp.cooperativeMatrixSupportedStages);
    std::printf("\n  VK_EXT_shader_float8 %s", fp8 ? "yes" : "no");
    if (fp8)
        std::printf(" (shaderFloat8 %u, shaderFloat8CooperativeMatrix %u)", f8.shaderFloat8, f8.shaderFloat8CooperativeMatrix);
    std::printf("\n  VK_KHR_pipeline_binary %s", bin ? "yes" : "no");
    if (bin)
        std::printf(" (pipelineBinaries %u; internalCache %u, internalCacheControl %u, prefersInternalCache %u,"
                    " precompiledInternalCache %u, compressedData %u)",
                    binf.pipelineBinaries, binp.pipelineBinaryInternalCache, binp.pipelineBinaryInternalCacheControl,
                    binp.pipelineBinaryPrefersInternalCache, binp.pipelineBinaryPrecompiledInternalCache,
                    binp.pipelineBinaryCompressedData);
    std::printf("\n  VK_KHR_maintenance5 %s", m5 ? "yes" : "no");
    if (m5)
        std::printf(" (maintenance5 %u)", m5f.maintenance5);
    std::printf("\n  VK_KHR_workgroup_memory_explicit_layout %s", wml ? "yes" : "no");
    if (wml)
        std::printf(" (layout %u, scalarBlockLayout %u, 8BitAccess %u, 16BitAccess %u)", wmlf.workgroupMemoryExplicitLayout,
                    wmlf.workgroupMemoryExplicitLayoutScalarBlockLayout, wmlf.workgroupMemoryExplicitLayout8BitAccess,
                    wmlf.workgroupMemoryExplicitLayout16BitAccess);
    std::printf("\n  VK_EXT_pipeline_robustness %s", rob ? "yes" : "no");
    if (rob)
        std::printf(" (pipelineRobustness %u)", robf.pipelineRobustness);
    std::printf("\n");

    const auto cm = CoopProps(inst, p);
    std::printf("  cooperative matrix configurations: %zu\n", cm.size());
    for (const auto& c : cm)
        std::printf("    M%u N%u K%u  A %s  B %s  C %s  Result %s  scope %s%s\n", c.MSize, c.NSize, c.KSize, Comp(c.AType),
                    Comp(c.BType), Comp(c.CType), Comp(c.ResultType),
                    c.scope == VK_SCOPE_SUBGROUP_KHR ? "subgroup" : c.scope == VK_SCOPE_WORKGROUP_KHR ? "workgroup" : "other",
                    c.saturatingAccumulation ? "  saturating" : "");

    VkPhysicalDeviceMemoryProperties mp {};
    vkGetPhysicalDeviceMemoryProperties(p, &mp);
    for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
        std::printf("  heap %u: %llu MB%s\n", i, (unsigned long long) (mp.memoryHeaps[i].size >> 20),
                    mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " device local" : "");

    // What MochizukiNrRuntime.dll decides for this device (Vulkan::Create).
    const bool amd = pr.vendorID == 0x1002, rdna4 = amd && (pr.deviceID & 0xFF00) == 0x7500;
    const bool fp8ok = fp8 && f8.shaderFloat8 && f8.shaderFloat8CooperativeMatrix;
    const char* net = !amd ? "none (not AMD)"
                      : fp8ok ? "FP8 network"
                      : rdna4 ? "refused (RX 9000 without FP8)"
                      : HasConfig(cm, VK_COMPONENT_TYPE_FLOAT32_KHR) ? "RDNA3 network (FP16 matrices)"
                                                                     : "refused (no FP16 x FP16 -> FP32 16x16x16 matrices)";
    std::printf("  runtime would pick: %s\n", net);
}

static void Report()
{
    Windows();
    Dxgi();
    VkInstance inst = MakeInstance();
    if (!inst)
        return;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(inst, &n, all.data());
    for (uint32_t i = 0; i < n; ++i)
        Device(inst, i, all[i]);
    vkDestroyInstance(inst, nullptr);
}

// ---- the checks ----

struct Gpu
{
    VkInstance inst {};
    VkPhysicalDevice phys {};
    VkDevice dev {};
    VkQueue queue {};
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties mem {};
    std::vector<VkCooperativeMatrixPropertiesKHR> coop;
};

[[noreturn]] static void Skip(const char* name, const char* why)
{
    std::printf("CHECK %s SKIPPED %s\n", name, why);
    std::exit(3);
}

static Gpu Open(const char* name)
{
    Gpu g;
    g.inst = MakeInstance();
    if (!g.inst)
        Skip(name, "no Vulkan instance");
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.inst, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(g.inst, &n, all.data());
    for (int pass = 0; pass < 2 && !g.phys; ++pass)
        for (auto p : all)
        {
            VkPhysicalDeviceProperties pr {};
            vkGetPhysicalDeviceProperties(p, &pr);
            if (pr.vendorID == 0x1002 && (pass || pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))
            {
                g.phys = p;
                break;
            }
        }
    if (!g.phys)
        Skip(name, "no AMD Vulkan device");
    VkPhysicalDeviceProperties pr {};
    vkGetPhysicalDeviceProperties(g.phys, &pr);
    std::printf("  on %s\n", pr.deviceName);
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> ext(en);
    vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &en, ext.data());
    if (!Has(ext, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME))
        Skip(name, "no VK_KHR_cooperative_matrix");
    VkPhysicalDeviceVulkan13Properties p13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES };
    VkPhysicalDeviceProperties2 pp { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p13 };
    vkGetPhysicalDeviceProperties2(g.phys, &pp);
    if (p13.minSubgroupSize > 32 || p13.maxSubgroupSize < 32 || !(p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT))
        Skip(name, "wave32 cannot be required for compute");
    g.coop = CoopProps(g.inst, g.phys);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, qf.data());
    while (g.family < qn && !(qf[g.family].queueFlags & VK_QUEUE_COMPUTE_BIT))
        ++g.family;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cf { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
    VkPhysicalDeviceVulkan13Features f13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &cf };
    VkPhysicalDeviceVulkan12Features f12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13 };
    VkPhysicalDeviceVulkan11Features f11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12 };
    VkPhysicalDeviceFeatures2 f2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11 };
    cf.cooperativeMatrix = VK_TRUE;
    f13.subgroupSizeControl = f13.computeFullSubgroups = VK_TRUE;
    f12.shaderFloat16 = f12.vulkanMemoryModel = VK_TRUE;
    f11.storageBuffer16BitAccess = VK_TRUE;
    const char* names[] = { VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME };
    float prio = 1.f;
    VkDeviceQueueCreateInfo qi { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = g.family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2 };
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = names;
    if (VkResult r = vkCreateDevice(g.phys, &di, nullptr, &g.dev))
    {
        std::printf("  vkCreateDevice VkResult %d\n", r);
        Skip(name, "the device cannot be created with cooperative matrices, FP16 and wave32");
    }
    vkGetDeviceQueue(g.dev, g.family, 0, &g.queue);
    vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem);
    return g;
}

struct Buf
{
    VkBuffer b {};
    VkDeviceMemory m {};
    void* p = nullptr;
};

static Buf MakeBuf(Gpu& g, size_t bytes)
{
    Buf r;
    VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    vkCreateBuffer(g.dev, &bi, nullptr, &r.b);
    VkMemoryRequirements mr {};
    vkGetBufferMemoryRequirements(g.dev, r.b, &mr);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = 0;
    while (type < g.mem.memoryTypeCount &&
           !((mr.memoryTypeBits >> type & 1) && (g.mem.memoryTypes[type].propertyFlags & want) == want))
        ++type;
    VkMemoryAllocateInfo ma { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = type;
    vkAllocateMemory(g.dev, &ma, nullptr, &r.m);
    vkBindBufferMemory(g.dev, r.b, r.m, 0);
    vkMapMemory(g.dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.p);
    std::memset(r.p, 0, bytes);
    return r;
}

// One dispatch of <dir>\<spv> at wave32 over two storage buffers. False if the pipeline failed.
static bool Dispatch(Gpu& g, const std::wstring& spv, Buf& in, Buf& out, uint32_t groups)
{
    std::ifstream f(spv, std::ios::binary | std::ios::ate);
    if (!f)
    {
        std::printf("  %ls missing\n", spv.c_str());
        return false;
    }
    std::vector<uint32_t> code(size_t(f.tellg()) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(code.data()), std::streamsize(code.size() * 4));
    VkShaderModuleCreateInfo smi { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = code.size() * 4;
    smi.pCode = code.data();
    VkShaderModule mod {};
    vkCreateShaderModule(g.dev, &smi, nullptr, &mod);
    VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
                                           { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT } };
    VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 2;
    li.pBindings = lb;
    VkDescriptorSetLayout dsl {};
    vkCreateDescriptorSetLayout(g.dev, &li, nullptr, &dsl);
    VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    VkPipelineLayout layout {};
    vkCreatePipelineLayout(g.dev, &pli, nullptr, &layout);
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO };
    req.requiredSubgroupSize = 32;
    VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &req, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main" };
    cpi.layout = layout;
    VkPipeline pipe {};
    if (VkResult r = vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe))
    {
        std::printf("  vkCreateComputePipelines VkResult %d\n", r);
        return false;
    }
    VkDescriptorPoolSize ps { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPoolCreateInfo dpi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VkDescriptorPool pool {};
    vkCreateDescriptorPool(g.dev, &dpi, nullptr, &pool);
    VkDescriptorSetAllocateInfo dai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    VkDescriptorSet set {};
    vkAllocateDescriptorSets(g.dev, &dai, &set);
    VkDescriptorBufferInfo bi[2] = { { in.b, 0, VK_WHOLE_SIZE }, { out.b, 0, VK_WHOLE_SIZE } };
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
    vkUpdateDescriptorSets(g.dev, 2, w, 0, nullptr);
    VkCommandPoolCreateInfo cpci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = g.family;
    VkCommandPool cpool {};
    vkCreateCommandPool(g.dev, &cpci, nullptr, &cpool);
    VkCommandBufferAllocateInfo cba { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cba.commandPool = cpool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer cb {};
    vkAllocateCommandBuffers(g.dev, &cba, &cb);
    VkCommandBufferBeginInfo bgi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cb, &bgi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cb, groups, 1, 1);
    VkMemoryBarrier mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (VkResult r = vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE); r || (r = vkQueueWaitIdle(g.queue)))
    {
        std::printf("  submit/wait VkResult %d\n", r);
        return false;
    }
    return true;
}

// Exact for the small integers and the e4m3 values used here (all normal halves: e4m3's least is 2^-9).
static uint16_t Half(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    const uint32_t s = (u >> 16) & 0x8000;
    if ((u & 0x7FFFFFFF) == 0)
        return uint16_t(s);
    return uint16_t(s | uint32_t(int((u >> 23) & 255) - 127 + 15) << 10 | (u & 0x7FFFFF) >> 13);
}

// Fragment maps: element (r, c) holds 16 r + c + 1. Which element each component of each lane holds.
static int Map(const char* name, const std::wstring& spv, VkComponentTypeKHR acc, unsigned expectLen, bool accumulator, bool isB)
{
    Gpu g = Open(name);
    if (acc != VK_COMPONENT_TYPE_MAX_ENUM_KHR && !HasConfig(g.coop, acc))
        Skip(name, acc == VK_COMPONENT_TYPE_FLOAT16_KHR ? "no FP16 x FP16 -> FP16 16x16x16 subgroup configuration"
                                                         : "no FP16 x FP16 -> FP32 16x16x16 subgroup configuration");
    Buf in = MakeBuf(g, 256 * 4), out = MakeBuf(g, 2048 * 4);
    for (int i = 0; i < 256; ++i)
    {
        if (acc == VK_COMPONENT_TYPE_FLOAT32_KHR)
            static_cast<float*>(in.p)[i] = float(i + 1);
        else
            static_cast<uint16_t*>(in.p)[i] = Half(float(i + 1));
    }
    float* o = static_cast<float*>(out.p);
    std::fill(o, o + 2048, -1.f);
    if (!Dispatch(g, spv, in, out, 1))
    {
        std::printf("CHECK %s FAIL the probe did not run\n", name);
        return 1;
    }
    const unsigned len = unsigned(o[1024]);
    std::printf("  length() %u, subgroup size seen by the shader %g\n", len, o[1025]);
    // Fit the known layouts. gfx11 accumulator: (2c + l/16, l%16); gfx12: (8(l/16) + c, l%16);
    // gfx11 A: (l%16, c), B: (c, l%16) in both halves; gfx12 A: (l%16, 8(l/16) + c), B: (8(l/16) + c, l%16).
    unsigned g11 = 0, g12 = 0, total = 0;
    for (unsigned l = 0; l < 32; ++l)
        for (unsigned c = 0; c < len && c < 32; ++c)
        {
            const int v = int(o[32 * l + c]) - 1;
            const int r = v / 16, col = v % 16;
            ++total;
            if (accumulator)
            {
                g11 += r == int(2 * c + l / 16) && col == int(l % 16);
                g12 += r == int(8 * (l / 16) + c) && col == int(l % 16);
            }
            else
            {
                const int k12 = int(8 * (l / 16) + c);
                g11 += isB ? r == int(c) && col == int(l % 16) : r == int(l % 16) && col == int(c);
                g12 += isB ? r == k12 && col == int(l % 16) : r == int(l % 16) && col == k12;
            }
        }
    const auto show = [&](unsigned l) {
        std::printf("  lane %2u:", l);
        for (unsigned c = 0; c < len && c < 32; ++c)
        {
            const int v = int(o[32 * l + c]) - 1;
            std::printf(v < 0 ? " (?)" : " (%d,%d)", v / 16, v % 16);
        }
        std::printf("\n");
    };
    std::printf("  observed (row,col) of each component:\n");
    for (unsigned l : { 0u, 1u, 2u, 15u, 16u, 17u, 31u })
        show(l);
    const bool lenOk = len == expectLen, ok = lenOk && g11 == total && o[1025] == 32.f;
    const char* fit = g11 == total ? (accumulator ? "gfx11 map (2c + l/16, l%16)" : isB ? "gfx11 map (c, l%16), both halves" : "gfx11 map (l%16, c), both halves")
                      : g12 == total ? (accumulator ? "gfx12 map (8(l/16) + c, l%16)" : isB ? "gfx12 map (8(l/16) + c, l%16)" : "gfx12 map (l%16, 8(l/16) + c)")
                                     : "an unknown map";
    std::printf("CHECK %s %s length %u (want %u), %s, %u of %u components on the gfx11 map, wave %g\n", name,
                ok ? "PASS" : "FAIL", len, expectLen, fit, g11, total, o[1025]);
    return ok ? 0 : 1;
}

static int Halves(const std::wstring& spv)
{
    const char* name = "wmma-lane-halves";
    Gpu g = Open(name);
    if (!HasConfig(g.coop, VK_COMPONENT_TYPE_FLOAT32_KHR))
        Skip(name, "no FP16 x FP16 -> FP32 16x16x16 subgroup configuration");
    Buf in = MakeBuf(g, 512 * 2), out = MakeBuf(g, 1024 * 4);
    uint16_t* h = static_cast<uint16_t*>(in.p);
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j)
        {
            h[i * 16 + j] = Half(i == j ? 1.f : 0.f);
            h[256 + i * 16 + j] = Half(float(16 * i + j + 1));
        }
    if (!Dispatch(g, spv, in, out, 4))
    {
        std::printf("CHECK %s FAIL the probe did not run\n", name);
        return 1;
    }
    // Element (i, j) of mode m: the data when row i came from the unmarked half, 1000 + k when from the marked one
    // (B marked: k = i, the identity A picks B's row i; A marked: k = j).
    const float* o = static_cast<const float*>(out.p);
    long wrong = 0, fromOther = 0;
    for (int mode = 0; mode < 4; ++mode)
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 16; ++j)
            {
                const float got = o[mode * 256 + i * 16 + j];
                const bool upperRow = (i & 1) != 0;
                const bool marked = mode == 0 || mode == 2 ? upperRow : !upperRow;
                const float data = float(16 * i + j + 1), marker = 1000.f + float(mode < 2 ? i : j);
                const float want = marked ? marker : data;
                wrong += got != want;
                fromOther += got == (marked ? data : marker);
            }
    std::printf("  rows 0..3 of mode 0 (B, upper half marked; even rows should be data, odd rows 1000 + row):\n");
    for (int i = 0; i < 4; ++i)
        std::printf("    %g %g %g %g ...\n", o[i * 16], o[i * 16 + 1], o[i * 16 + 2], o[i * 16 + 3]);
    std::printf("CHECK %s %s output row m from lane half m%%2 of both operands: %ld of 1024 elements differ"
                " (%ld look taken from the other half)\n",
                name, wrong ? "FAIL" : "PASS", wrong, fromOther);
    return wrong ? 1 : 0;
}

static float E4m3(std::mt19937& rng)
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

static int Kswap(const std::wstring& spv)
{
    const char* name = "kpair-swap";
    Gpu g = Open(name);
    if (!HasConfig(g.coop, VK_COMPONENT_TYPE_FLOAT32_KHR))
        Skip(name, "no FP16 x FP16 -> FP32 16x16x16 subgroup configuration");
    const uint32_t tiles = 400;
    Buf in = MakeBuf(g, size_t(tiles) * 512 * 2), out = MakeBuf(g, size_t(tiles) * 512 * 4);
    std::vector<float> a(size_t(tiles) * 512);
    std::mt19937 rng(12345);
    uint16_t* h = static_cast<uint16_t*>(in.p);
    for (size_t i = 0; i < a.size(); ++i)
    {
        a[i] = E4m3(rng);
        h[i] = Half(a[i]);
    }
    if (!Dispatch(g, spv, in, out, tiles))
    {
        std::printf("CHECK %s FAIL the probe did not run\n", name);
        return 1;
    }
    const float* o = static_cast<const float*>(out.p);
    long differ = 0, offRef = 0;
    double worst = 0;
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
                const double err = std::fabs(plain - ref) / (mag + 1e-30);
                worst = std::max(worst, err);
                offRef += err > 1e-5;
            }
    const bool ok = !differ && !offRef;
    std::printf("CHECK %s %s %ld of %u products differ with every k pair swapped in the upper half;"
                " %ld off the CPU product (worst relative error %.2g)\n",
                name, ok ? "PASS" : "FAIL", differ, tiles * 256, offRef, worst);
    return ok ? 0 : 1;
}

static const char* kChecks[] = { "acc-f32", "acc-f16", "frag-a", "frag-b", "wmma-lane-halves", "kpair-swap" };

static int Check(const std::string& name, const std::wstring& dir)
{
    std::printf("[%s]\n", name.c_str());
    if (name == "acc-f32")
        return Map("acc-f32", dir + L"/acc32.spv", VK_COMPONENT_TYPE_FLOAT32_KHR, 8, true, false);
    if (name == "acc-f16")
        return Map("acc-f16", dir + L"/acc16.spv", VK_COMPONENT_TYPE_FLOAT16_KHR, 8, true, false);
    if (name == "frag-a")
        return Map("frag-a", dir + L"/fraga.spv", VK_COMPONENT_TYPE_MAX_ENUM_KHR, 16, false, false);
    if (name == "frag-b")
        return Map("frag-b", dir + L"/fragb.spv", VK_COMPONENT_TYPE_MAX_ENUM_KHR, 16, false, true);
    if (name == "wmma-lane-halves")
        return Halves(dir + L"/halves.spv");
    if (name == "kpair-swap")
        return Kswap(dir + L"/kswap.spv");
    return 2;
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 4 && !std::wcscmp(argv[1], L"--check"))
    {
        std::string name;
        for (const wchar_t* p = argv[2]; *p; ++p)
            name += char(*p);
        return Check(name, argv[3]);
    }
    const std::wstring dir = argc > 1 ? argv[1] : L"probe";
    if (!LoadLibraryW(L"vulkan-1.dll"))
    {
        Windows();
        Dxgi();
        std::printf("no Vulkan loader (vulkan-1.dll)\n");
        for (const char* c : kChecks)
            std::printf("CHECK %s SKIPPED no Vulkan loader\n", c);
        return 0;
    }
    Report();
    std::printf("\nfragment layout checks (wave32):\n");
    for (const char* c : kChecks)
    {
        std::wstring args = L"--check ";
        for (const char* p = c; *p; ++p)
            args += wchar_t(*p);
        const long code = RunChild(args + L" \"" + dir + L"\"", 60000);
        if (code == -1)
            std::printf("CHECK %s TIMEOUT after 60 s\n", c);
        else if (Crashed(code) || code == -2)
            std::printf("CHECK %s CRASH exit 0x%08lx\n", c, (unsigned long) code);
    }
    return 0;
}
