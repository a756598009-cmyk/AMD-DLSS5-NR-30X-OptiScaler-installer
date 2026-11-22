#pragma once
#include "nr_pal_binary.hpp"
#include "nr_driver_identity.hpp"
#include "nr_log.hpp"
#include "nr_build_cfg.hpp"
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include "nr_path_u8.hpp"   // UTF-8 paths (a game folder with any letters)
#include <map>
#include <mutex>
#include <set>

namespace nr::binary {
// The ACO network for the whole process: the bundle directory (records/ + shaders/), empty when off.
// NR_ACO_DIR (tests) wins; otherwise the product's switch sets it with configure() before the first
// device is created. disable() turns it off for the rest of the process (a GPU, driver or device that
// cannot take it): every network built afterwards uses the driver's own compiler.
struct Mode {
    std::mutex m;
    bool configured = false, disabled = false;
    std::string dir, why;
    std::vector<std::string> shells;   // template SPIR-V directories, searched in order
    std::string cache;                 // where imported binaries are kept between runs; empty: none
    std::map<std::string,std::string> aliases;   // a template's name -> the native SPIR-V that serves as it
    uint64_t identity = 0;             // the GPU and driver the device was made on (cache key part)
};
inline Mode& mode() { static Mode s; return s; }
// The bundle's own ACO network of RDNA3 (nr_gpu_arch.hpp) in its rdna3 folder: its records, its SPIR-V, its templates
// (shells: small shaders with the network's interfaces, since RDNA3 has no native network to take them from) and
// their aliases. The first device's architecture decides it for the process. False (and the ACO network off: RDNA3
// then has no NR) when the bundle has none.
inline bool use_rdna3(std::string* why) {
    auto& s=mode();std::lock_guard<std::mutex> l(s.m);
    if(s.disabled || !s.configured){*why="ACO Mode is off";return false;}
    if(s.dir.size()>6 && s.dir.compare(s.dir.size()-6,6,"/rdna3")==0)return true;
    const std::string dir=s.dir+"/rdna3";
    std::ifstream probe(nr::u8::path(dir+"/shell-aliases.txt"));
    if(!probe){s.disabled=true;s.why=*why="the ACO network has no RDNA3 records ("+dir+")";return false;}
    std::map<std::string,std::string> aliases;
    for(std::string a,b;probe>>a>>b;)aliases[a]=b;
    s.dir=dir;s.shells={dir+"/shells"};s.aliases=std::move(aliases);
    return true;
}
inline void configure(const std::string& dir, std::vector<std::string> shells, const std::string& cache,
                      std::map<std::string,std::string> aliases) {
    auto& s=mode();std::lock_guard<std::mutex> l(s.m);
    s.configured=true;s.dir=dir;s.shells=std::move(shells);s.cache=cache;
    s.aliases=std::move(aliases);
}
inline std::string alias_of(const std::string& name) {
    auto& s=mode();std::lock_guard<std::mutex> l(s.m);
    auto it=s.aliases.find(name);return it==s.aliases.end()?std::string():it->second;
}
inline std::string cache_dir() {
    if(const char* p=std::getenv("NR_ACO_CACHE")) if(*p) return p;
    auto& s=mode();std::lock_guard<std::mutex> l(s.m);return s.cache;
}
inline uint64_t identity() { auto& s=mode();std::lock_guard<std::mutex> l(s.m);return s.identity; }
inline std::string directory() {
    auto& s=mode();std::lock_guard<std::mutex> l(s.m);
    if(s.disabled)return "";
    std::string dir;
    if(const char* p=std::getenv("NR_ACO_DIR")) if(*p) dir=p;
    if(dir.empty() && s.configured) dir=s.dir;
#if NR_INT4
    // Each build selects its own records; changing the network never changes the process mode.
    if(!dir.empty() && nr::build_cfg("NR_I4_WEIGHTS")) dir+="/int4";
#endif
    return dir;
}
inline void disable(const std::string& why) {
    auto& s=mode();std::lock_guard<std::mutex> l(s.m);
    if(!s.disabled){s.disabled=true;s.why=why;}
}
// Why the ACO network is off although it was asked for; empty when it was not asked for or is on.
inline std::string disabled_why() { auto& s=mode();std::lock_guard<std::mutex> l(s.m);return s.disabled?s.why:""; }
inline std::vector<std::string> shell_dirs() {
    if(const char* p=std::getenv("NR_ACO_SHELL_DIR")) if(*p) return {p};
    auto& s=mode();std::lock_guard<std::mutex> l(s.m);return s.shells;
}
inline std::mutex devices_mutex;
inline std::set<VkDevice> enabled_devices;
// A device's code objects as its driver writes them (device_code): the machine in e_flags, the key size and the
// user-data entry its pipeline layouts put the push constants at (1 on a plain device; nr_pal_binary.hpp XglLayout).
struct DeviceCode { uint32_t mach=0,key_size=0,push_base=1; };
inline std::map<VkDevice,DeviceCode> device_codes;
inline void mark(VkDevice d) {std::lock_guard<std::mutex> l(devices_mutex);enabled_devices.insert(d);}
inline void forget(VkDevice d) {std::lock_guard<std::mutex> l(devices_mutex);enabled_devices.erase(d);device_codes.erase(d);}
inline bool enabled(VkDevice d) {std::lock_guard<std::mutex> l(devices_mutex);return enabled_devices.count(d)!=0;}
// A failure of ACO's machine code itself (no record for the SPIR-V, its template, the driver refusing the import):
// the build that met it turns ACO Mode off for the process and builds again with the driver's compiler. What is not
// one - the device or the host out of memory, a lost device - stays the error it is: that build fails as it would
// without ACO, and the next one tries ACO's machine code again.
struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
struct VkFailure : std::runtime_error {
    VkResult result;
    VkFailure(const std::string& what,VkResult r) : std::runtime_error(what),result(r) {}
};
inline void check(VkResult r,const char* what) {
    if(r!=VK_SUCCESS)throw VkFailure(std::string(what)+": "+std::to_string(int(r)),r);
}
struct Features {
    VkPhysicalDevicePipelineBinaryFeaturesKHR pb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    bool active=false;
    // A GPU, driver or device chain that cannot take the ACO network turns it off for the process
    // (disable): the device is created as it would be without it, never refused.
    void prepare(VkPhysicalDevice physical,VkDeviceCreateInfo& ci,std::vector<const char*>& extensions) {
        if(directory().empty())return;
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&drv};
        vkGetPhysicalDeviceProperties2(physical,&props);
        if(props.properties.vendorID!=0x1002 || drv.driverID!=VK_DRIVER_ID_AMD_PROPRIETARY) {
            // A hook in front of the loader (OptiScaler's GPU spoofing) may say so: AMD's driver is asked itself.
            const std::string said=props.properties.deviceName;
            if(nr::driver::amd_reported(physical,&props.properties,&drv))
                nr::logf("[nr] ACO network: the loader reports %s, AMD's driver %s",said.c_str(),props.properties.deviceName);
            else
                nr::logf("[nr] ACO network: the loader reports %s, and no AMD driver in the process lists this GPU",said.c_str());
        }
        if(props.properties.vendorID!=0x1002 || drv.driverID!=VK_DRIVER_ID_AMD_PROPRIETARY)
            return disable("the ACO network needs AMD's own Windows Vulkan driver");
        uint32_t n=0;
        if(vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,nullptr)!=VK_SUCCESS)return disable("cannot list the device extensions");
        std::vector<VkExtensionProperties> have(n);
        if(vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,have.data())!=VK_SUCCESS)return disable("cannot list the device extensions");
        auto listed=[&](const char* name){for(uint32_t i=0;i<n;++i)if(!std::strcmp(have[i].extensionName,name))return true;return false;};
        if(!listed(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) || !listed(VK_KHR_MAINTENANCE_5_EXTENSION_NAME))
            return disable("the driver has no VK_KHR_pipeline_binary / VK_KHR_maintenance5");
        pb.pNext=&m5;VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&pb};
        vkGetPhysicalDeviceFeatures2(physical,&q);
        if(!pb.pipelineBinaries || !m5.maintenance5)return disable("the driver offers no pipelineBinaries / maintenance5");
        // The chain is ours (our own device's, or the copy of a game's that vkdevice::augment makes): a structure that
        // leaves the two features off has them turned on here rather than ACO Mode turned off for the whole process
        // (a game's VkPhysicalDeviceVulkan14Features commonly says maintenance5 = false for nothing).
        bool have_pb=false,have_m5=false;
        for(auto* s=static_cast<VkBaseOutStructure*>(const_cast<void*>(ci.pNext));s;s=s->pNext) {
            if(s->sType==pb.sType) {have_pb=true;reinterpret_cast<VkPhysicalDevicePipelineBinaryFeaturesKHR*>(s)->pipelineBinaries=VK_TRUE;}
            if(s->sType==m5.sType) {have_m5=true;reinterpret_cast<VkPhysicalDeviceMaintenance5FeaturesKHR*>(s)->maintenance5=VK_TRUE;}
            if(s->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES) {
                have_m5=true;reinterpret_cast<VkPhysicalDeviceVulkan14Features*>(s)->maintenance5=VK_TRUE;
            }
        }
        auto add=[&](const char* name){for(auto* e:extensions)if(!std::strcmp(e,name))return;extensions.push_back(name);};
        add(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME);add(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        if(!have_m5){m5.pNext=const_cast<void*>(ci.pNext);ci.pNext=&m5;}
        if(!have_pb){pb.pNext=const_cast<void*>(ci.pNext);ci.pNext=&pb;}
        ci.enabledExtensionCount=uint32_t(extensions.size());ci.ppEnabledExtensionNames=extensions.data();active=true;
        {
            auto& st=mode();std::lock_guard<std::mutex> l(st.m);
            uint64_t id=pal::hash(props.properties.pipelineCacheUUID,VK_UUID_SIZE);
            id^=(uint64_t(props.properties.driverVersion)<<32)^props.properties.deviceID;
            st.identity=pal::hash(&id,sizeof id);
        }
        nr::logf("[nr] ACO network: pipeline binaries on %s, %s (driverVersion 0x%08x)",props.properties.deviceName,
                 drv.driverInfo,props.properties.driverVersion);
    }
};
struct Functions {
    VkDevice device{};
    PFN_vkCreatePipelineBinariesKHR create{};PFN_vkDestroyPipelineBinaryKHR destroy{};
    PFN_vkGetPipelineBinaryDataKHR data{};PFN_vkReleaseCapturedPipelineDataKHR release{};
    explicit Functions(VkDevice d):device(d) {
        create=reinterpret_cast<PFN_vkCreatePipelineBinariesKHR>(vkGetDeviceProcAddr(d,"vkCreatePipelineBinariesKHR"));
        destroy=reinterpret_cast<PFN_vkDestroyPipelineBinaryKHR>(vkGetDeviceProcAddr(d,"vkDestroyPipelineBinaryKHR"));
        data=reinterpret_cast<PFN_vkGetPipelineBinaryDataKHR>(vkGetDeviceProcAddr(d,"vkGetPipelineBinaryDataKHR"));
        release=reinterpret_cast<PFN_vkReleaseCapturedPipelineDataKHR>(vkGetDeviceProcAddr(d,"vkReleaseCapturedPipelineDataKHR"));
        pal::require(create && destroy && data && release,"pipeline binary functions unavailable");
    }
};
struct Binaries {
    Functions& f;std::vector<VkPipelineBinaryKHR> handles;
    explicit Binaries(Functions& api):f(api) {}
    ~Binaries(){for(auto h:handles)if(h){if(std::getenv("NR_ACO_DUMP")){std::fprintf(stderr,"ACO destroy binary %p\n",(void*)h);std::fflush(stderr);}f.destroy(f.device,h,nullptr);}}
};
struct Captured {
    Functions& f;VkPipeline pipeline{};
    explicit Captured(Functions& api):f(api) {}
    ~Captured(){if(pipeline) {
        VkReleaseCapturedPipelineDataInfoKHR r{VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR};r.pipeline=pipeline;
        if(std::getenv("NR_ACO_DUMP")){std::fprintf(stderr,"ACO release captured data\n");std::fflush(stderr);}
        f.release(f.device,&r,nullptr);
        if(std::getenv("NR_ACO_DUMP")){std::fprintf(stderr,"ACO destroy captured pipeline\n");std::fflush(stderr);}
        vkDestroyPipeline(f.device,pipeline,nullptr);
    }}
};
inline std::string keyname(uint64_t h) {char s[24];std::snprintf(s,sizeof s,"%016llx.nrp",static_cast<unsigned long long>(h));return s;}
inline bool network_path(const std::string& path) {
    auto name=path.substr(path.find_last_of("/\\")+1);
    return name.rfind("g_",0)==0 || name.rfind("temporal_pre_fp32",0)==0 || name.rfind("temporal_post_fp32",0)==0;
}
// Diagnostic control: compile selected shell kernels with LLPC while retaining
// the same graph, buffers and push constants used by the ACO run.
inline bool native_shell(const std::string& name) {
    const char* selected=std::getenv("NR_ACO_NATIVE_KERNELS");
    if(!selected || !*selected)return false;
    std::string list(selected);size_t start=0;
    while(start<=list.size()) {
        auto end=list.find(',',start);auto token=list.substr(start,end-start);
        if(token=="*" || token==name || (!token.empty() && token.back()=='*' &&
           name.compare(0,token.size()-1,token,0,token.size()-1)==0))return true;
        if(end==std::string::npos)break;start=end+1;
    }
    return false;
}
// An imported binary kept between runs: the captured template's compile is the slow part of a build
// (the driver compiles the native template only to get its container). File: "NRAC", version 1, the key
// size and key, the data size and data, fnv64 of the data. A file that does not check out is ignored.
inline bool load_cached(const std::string& file,VkPipelineBinaryKeyKHR& key,pal::Bytes& blob) {
    std::ifstream in(nr::u8::path(file),std::ios::binary);if(!in)return false;
    char magic[4];uint32_t version=0,key_size=0;uint64_t size=0,check=0;
    in.read(magic,4);in.read(reinterpret_cast<char*>(&version),4);in.read(reinterpret_cast<char*>(&key_size),4);
    if(!in || std::memcmp(magic,"NRAC",4)!=0 || version!=1 || !key_size || key_size>VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR)return false;
    key.keySize=key_size;in.read(reinterpret_cast<char*>(key.key),VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR);
    in.read(reinterpret_cast<char*>(&size),8);if(!in || !size || size>64*1024*1024)return false;
    blob.resize(size);in.read(reinterpret_cast<char*>(blob.data()),std::streamsize(size));in.read(reinterpret_cast<char*>(&check),8);
    return bool(in) && check==pal::hash(blob.data(),blob.size());
}
inline void save_cached(const std::string& file,const VkPipelineBinaryKeyKHR& key,const pal::Bytes& blob) {
    const std::string tmp=file+".tmp";
    {
        std::ofstream out(nr::u8::path(tmp),std::ios::binary|std::ios::trunc);if(!out)return;
        const uint32_t version=1,key_size=key.keySize;const uint64_t size=blob.size(),check=pal::hash(blob.data(),blob.size());
        out.write("NRAC",4);out.write(reinterpret_cast<const char*>(&version),4);out.write(reinterpret_cast<const char*>(&key_size),4);
        out.write(reinterpret_cast<const char*>(key.key),VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR);
        out.write(reinterpret_cast<const char*>(&size),8);out.write(reinterpret_cast<const char*>(blob.data()),std::streamsize(size));
        out.write(reinterpret_cast<const char*>(&check),8);if(!out)return;
    }
    nr::u8::remove(file);nr::u8::rename(tmp,file);
}
inline VkPipeline import_pipeline(Functions& f,VkDevice device,const VkComputePipelineCreateInfo& ci,
                                  const VkPipelineBinaryKeyKHR& key_in,const pal::Bytes& blob) {
    VkPipelineBinaryKeyKHR key=key_in;
    VkPipelineBinaryDataKHR bd{blob.size(),const_cast<uint8_t*>(blob.data())};VkPipelineBinaryKeysAndDataKHR kd{1,&key,&bd};
    VkPipelineBinaryCreateInfoKHR bi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};bi.pKeysAndDataInfo=&kd;
    VkPipelineBinaryHandlesInfoKHR hi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    Binaries imported(f);imported.handles.resize(1);hi.pipelineBinaryCount=1;hi.pPipelineBinaries=imported.handles.data();
    check(f.create(device,&bi,nullptr,&hi),"import binary");
    VkPipelineBinaryInfoKHR input{VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR};input.binaryCount=1;input.pPipelineBinaries=imported.handles.data();input.pNext=ci.pNext;
    auto imported_ci=ci;imported_ci.pNext=&input;imported_ci.stage.module=VK_NULL_HANDLE;
    VkPipeline pipeline{};VkResult result=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&imported_ci,nullptr,&pipeline);
    if(result!=VK_SUCCESS){if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);check(result,"create imported pipeline");}
    return pipeline;
}
// The driver's own compile of ci as its one PAL code object, and the binary's key.
inline pal::Bytes capture_binary(Functions& f,VkDevice device,const VkComputePipelineCreateInfo& ci,VkPipelineBinaryKeyKHR& key) {
    Captured captured(f);
    VkPipelineCreateFlags2CreateInfoKHR flags{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR};
    flags.flags=VkPipelineCreateFlags2KHR(ci.flags)|VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;flags.pNext=ci.pNext;
    auto capture_ci=ci;capture_ci.pNext=&flags;
    check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&capture_ci,nullptr,&captured.pipeline),"capture pipeline");
    Binaries original(f);VkPipelineBinaryCreateInfoKHR bi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};bi.pipeline=captured.pipeline;
    VkPipelineBinaryHandlesInfoKHR hi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    check(f.create(device,&bi,nullptr,&hi),"count captured binaries");
    pal::require(hi.pipelineBinaryCount==1,"expected one PAL compute binary");original.handles.resize(1);
    hi.pPipelineBinaries=original.handles.data();check(f.create(device,&bi,nullptr,&hi),"capture binaries");
    VkPipelineBinaryDataInfoKHR di{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR};di.pipelineBinary=original.handles[0];
    key=VkPipelineBinaryKeyKHR{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};size_t bytes=0;
    check(f.data(device,&di,&key,&bytes,nullptr),"size captured binary");
    pal::require(bytes && bytes<=64*1024*1024,"captured binary size limit");pal::Bytes blob(bytes);
    check(f.data(device,&di,&key,&bytes,blob.data()),"read captured binary");pal::require(bytes<=blob.size(),"captured binary grew");blob.resize(bytes);
    return blob;
}
// The GPU's machine as its driver writes it in e_flags, from the driver's compile of an empty kernel with the
// pipeline's layout (push_bytes: that layout's push range): once a device (gfx10.3 containers are built, not captured,
// and need it; the import cache's key takes the push constants' place).
inline DeviceCode device_code(Functions& f,VkDevice device,const VkComputePipelineCreateInfo& ci,uint32_t push_bytes) {
    {std::lock_guard<std::mutex> l(devices_mutex);auto it=device_codes.find(device);if(it!=device_codes.end())return it->second;}
    // layout(local_size_x = 32) in; void main() {} (glslang 16.5.0, --target-env vulkan1.3)
    static const uint32_t empty_kernel[]={
        0x07230203,0x00010600,0x0008000b,0x0000000b,0x00000000,0x00020011,0x00000001,0x0006000b,
        0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
        0x0005000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x00060010,0x00000004,0x00000011,
        0x00000020,0x00000001,0x00000001,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,
        0x6e69616d,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00040015,
        0x00000006,0x00000020,0x00000000,0x00040017,0x00000007,0x00000006,0x00000003,0x0004002b,
        0x00000006,0x00000008,0x00000020,0x0004002b,0x00000006,0x00000009,0x00000001,0x0006002c,
        0x00000007,0x0000000a,0x00000008,0x00000009,0x00000009,0x00050036,0x00000002,0x00000004,
        0x00000000,0x00000003,0x000200f8,0x00000005,0x000100fd,0x00010038};
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=sizeof empty_kernel;sm.pCode=empty_kernel;
    VkShaderModule module{};check(vkCreateShaderModule(device,&sm,nullptr,&module),"create empty kernel module");
    auto empty_ci=ci;empty_ci.stage.module=module;
    VkPipelineBinaryKeyKHR key{};pal::Bytes blob;
    try{blob=capture_binary(f,device,empty_ci,key);}catch(...){vkDestroyShaderModule(device,module,nullptr);throw;}
    vkDestroyShaderModule(device,module,nullptr);
    pal::require(blob.size()>=64,"captured binary size limit");
    const uint32_t flags=pal::read<uint32_t>(blob,48);
    const pal::Elf elf(blob);
    DeviceCode dc{elf.mach,key.keySize};
    pal::require(dc.key_size && dc.key_size<=VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR,"driver binary key size");
    // Where the push constants are (1 on a plain device, 2 with VK_EXT_vertex_input_dynamic_state, ...): an empty
    // kernel's .user_data_limit counts only the entries it uses, so a small kernel of its own layout - set 0 binding 0
    // a storage buffer, 4 push-constant bytes, both read - is compiled: its limit is push_base + 2. Metadata without it
    // (none seen so far) or a probe that fails keeps entry 1, the plain device's.
    // #version 450 / layout(local_size_x = 32) in; / layout(set = 0, binding = 0) buffer B { uint v[]; };
    // layout(push_constant) uniform P { uint x; }; / void main() { v[gl_GlobalInvocationID.x] = x; } (glslang 16.5.0)
    static const uint32_t layout_kernel[]={
        0x07230203,0x00010600,0x0008000b,0x0000001f,0x00000000,0x00020011,0x00000001,0x0006000b,0x00000001,0x4c534c47,
        0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,0x0008000f,0x00000005,0x00000004,0x6e69616d,
        0x00000000,0x0000000a,0x0000000f,0x00000016,0x00060010,0x00000004,0x00000011,0x00000020,0x00000001,0x00000001,
        0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00030005,0x00000008,0x00000042,
        0x00040006,0x00000008,0x00000000,0x00000076,0x00030005,0x0000000a,0x00000000,0x00080005,0x0000000f,0x475f6c67,
        0x61626f6c,0x766e496c,0x7461636f,0x496e6f69,0x00000044,0x00030005,0x00000014,0x00000050,0x00040006,0x00000014,
        0x00000000,0x00000078,0x00030005,0x00000016,0x00000000,0x00040047,0x00000007,0x00000006,0x00000004,0x00030047,
        0x00000008,0x00000002,0x00050048,0x00000008,0x00000000,0x00000023,0x00000000,0x00040047,0x0000000a,0x00000021,
        0x00000000,0x00040047,0x0000000a,0x00000022,0x00000000,0x00040047,0x0000000f,0x0000000b,0x0000001c,0x00030047,
        0x00000014,0x00000002,0x00050048,0x00000014,0x00000000,0x00000023,0x00000000,0x00020013,0x00000002,0x00030021,
        0x00000003,0x00000002,0x00040015,0x00000006,0x00000020,0x00000000,0x0003001d,0x00000007,0x00000006,0x0003001e,
        0x00000008,0x00000007,0x00040020,0x00000009,0x0000000c,0x00000008,0x0004003b,0x00000009,0x0000000a,0x0000000c,
        0x00040015,0x0000000b,0x00000020,0x00000001,0x0004002b,0x0000000b,0x0000000c,0x00000000,0x00040017,0x0000000d,
        0x00000006,0x00000003,0x00040020,0x0000000e,0x00000001,0x0000000d,0x0004003b,0x0000000e,0x0000000f,0x00000001,
        0x0004002b,0x00000006,0x00000010,0x00000000,0x00040020,0x00000011,0x00000001,0x00000006,0x0003001e,0x00000014,
        0x00000006,0x00040020,0x00000015,0x00000009,0x00000014,0x0004003b,0x00000015,0x00000016,0x00000009,0x00040020,
        0x00000017,0x00000009,0x00000006,0x00040020,0x0000001a,0x0000000c,0x00000006,0x0004002b,0x00000006,0x0000001c,
        0x00000020,0x0004002b,0x00000006,0x0000001d,0x00000001,0x0006002c,0x0000000d,0x0000001e,0x0000001c,0x0000001d,
        0x0000001d,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x00050041,0x00000011,
        0x00000012,0x0000000f,0x00000010,0x0004003d,0x00000006,0x00000013,0x00000012,0x00050041,0x00000017,0x00000018,
        0x00000016,0x0000000c,0x0004003d,0x00000006,0x00000019,0x00000018,0x00060041,0x0000001a,0x0000001b,0x0000000a,
        0x0000000c,0x00000013,0x0003003e,0x0000001b,0x00000019,0x000100fd,0x00010038};
    uint64_t limit=0;
    VkDescriptorSetLayout set_layout{};VkPipelineLayout layout{};VkShaderModule probe{};
    try{
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sl.bindingCount=1;sl.pBindings=&binding;
        check(vkCreateDescriptorSetLayout(device,&sl,nullptr,&set_layout),"create layout probe set");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount=1;pl.pSetLayouts=&set_layout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&range;
        check(vkCreatePipelineLayout(device,&pl,nullptr,&layout),"create layout probe layout");
        VkShaderModuleCreateInfo pm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};pm.codeSize=sizeof layout_kernel;pm.pCode=layout_kernel;
        check(vkCreateShaderModule(device,&pm,nullptr,&probe),"create layout probe module");
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};pci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
        pci.stage.module=probe;pci.stage.pName="main";pci.layout=layout;
        VkPipelineBinaryKeyKHR pkey{};
        const pal::Bytes pb=capture_binary(f,device,pci,pkey);
        const pal::Elf pe(pb);
        limit=pal::pal_metadata(pe.data[pe.find(".note")]).at("amdpal.pipelines").items.at(0).at(".user_data_limit").number();
    }catch(const std::exception& e){
        nr::logf("[nr] ACO: the user-data layout probe failed (%s); push constants taken at entry 1",e.what());
    }
    if(probe)vkDestroyShaderModule(device,probe,nullptr);
    if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
    if(set_layout)vkDestroyDescriptorSetLayout(device,set_layout,nullptr);
    if(limit) {
        try{dc.push_base=pal::xgl_layout_of_limit(limit,4).push_base;}
        catch(const std::exception&){nr::logf("[nr] ACO: the layout probe's user-data limit %llu is not understood",(unsigned long long)limit);limit=0;}
    }
    nr::logf("[nr] ACO: the driver's code objects are for machine 0x%02x (e_flags 0x%x), push constants at user data %u%s",
             dc.mach,flags,dc.push_base,limit?"":" (assumed)");
    std::lock_guard<std::mutex> l(devices_mutex);return device_codes.emplace(device,dc).first->second;
}
inline VkPipeline create(VkDevice device,const VkComputePipelineCreateInfo& ci,const pal::Record& record,uint32_t push_bytes,
                         std::array<uint32_t,3> workgroup,const std::string& cache_file={}) {
    Functions f(device);
    if(!cache_file.empty()) {
        VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};pal::Bytes blob;
        if(load_cached(cache_file,key,blob)) {
            try{return import_pipeline(f,device,ci,key,blob);}catch(const std::exception&){}   // made again below
        }
    }
    const bool trace=std::getenv("NR_ACO_DUMP")!=nullptr;
    auto step=[&](const char* s){if(trace){std::fprintf(stderr,"ACO %s: %s\n",keyname(record.spv_hash).c_str(),s);std::fflush(stderr);}};
    // The caller's pNext chain is retained. Flags2 and binary input are owned here.
    for(auto* p=static_cast<const VkBaseInStructure*>(ci.pNext);p;p=p->pNext)
        pal::require(p->sType!=VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR &&
                     p->sType!=VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR,"unsupported pipeline input chain");
    pal::require(!ci.stage.pSpecializationInfo && std::strcmp(ci.stage.pName,"main")==0 &&
                 !(ci.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT),"unsupported pipeline specialization/entry");
    VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};pal::Bytes blob;
    if(pal::is_gfx103(record.mach)) {
        // gfx10.3: the container is built (nr_pal_binary.hpp synthesize), with the GPU's machine; no template compile.
        const DeviceCode dc=device_code(f,device,ci,push_bytes);
        step("synthesize");blob=pal::synthesize(record,dc.mach,push_bytes,workgroup,dc.push_base);key.keySize=dc.key_size;step("synthesized");
    } else {
        step("capture pipeline");
        blob=capture_binary(f,device,ci,key);
        if(const char* dump=std::getenv("NR_ACO_DUMP")) if(*dump) {
            std::ofstream out(nr::u8::path(std::string(dump)+"/"+keyname(record.spv_hash)+".captured.elf"),std::ios::binary);
            pal::require(bool(out),"cannot open captured binary dump");
            out.write(reinterpret_cast<const char*>(blob.data()),std::streamsize(blob.size()));
            pal::require(bool(out),"cannot write captured binary dump");
        }
        step("captured");blob=pal::splice(blob,record,push_bytes,workgroup);step("spliced");
    }
    // Domain-separate the imported key and include the complete transformed data.
    pal::require(key.keySize && key.keySize<=VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR,"driver binary key size");
    uint64_t h=pal::hash(blob.data(),blob.size())^record.spv_hash^0x4e5250414c303031ull;
    for(uint32_t i=0;i<key.keySize;++i) {h^=i+1;h*=1099511628211ull;key.key[i]=uint8_t(h>>(8*(i%8)));}
    step("import binary");
    VkPipeline pipeline=import_pipeline(f,device,ci,key,blob);
    step("pipeline created");
    if(!cache_file.empty())save_cached(cache_file,key,blob);
    return pipeline;
}
// The FP16 networks' bundles (MochizukiNrRuntime.cpp): dlssnr-amd/aco-gfx11 (RDNA3, ACO's gfx11 code) and
// aco-gfx103x (RDNA2, gfx10.3 code), each pipeline with a fallback to the driver. Their log tag; empty for aco.
inline std::string fallback_bundle(const std::string& dir) {
    const auto cut=dir.find_last_of("/\\");const auto name=dir.substr(cut==std::string::npos?0:cut+1);
    return name=="aco-gfx11"?"gfx11":name=="aco-gfx103x"?"gfx103x":"";
}
inline bool gfx11_bundle(const std::string& dir) { return fallback_bundle(dir)=="gfx11"; }
// aco-gfx11 as OpenNR ships it: records\ only for the kernels ACO's code runs faster on RX 7000, named by the hash of
// shaders-rdna3's SPIR-V or, where ACO's differs, by that one's, which shaders\ holds (no g_attn.spv). The network is
// then shaders-rdna3's with those kernels taken from shaders\; a full bundle (the kits', every kernel's SPIR-V in shaders\) keeps its own.
inline bool gfx11_slim(const std::string& dir) {
    return gfx11_bundle(dir) && !std::ifstream(nr::u8::path(dir+"/shaders/g_attn.spv")).good();
}
// This thread's pipelines are the driver's compile whatever the bundle holds (nr_graph.cpp create_kernels: an RX 7000
// network outside the sizes ACO's code was checked at).
inline thread_local bool driver_only=false;
inline VkResult create_aco(VkDevice device,VkPipelineCache cache,const VkComputePipelineCreateInfo& ci,const std::string& dir,
                           const std::string& path,const uint32_t* code,size_t bytes,VkPipeline* out,uint32_t push_bytes);
inline VkResult create_fallback(VkDevice device,VkPipelineCache cache,const VkComputePipelineCreateInfo& ci,const std::string& dir,
                                const std::string& tag,const std::string& path,const uint32_t* code,size_t bytes,
                                VkPipeline* out,uint32_t push_bytes) {
    // gfx11 / gfx10.3: a pipeline whose record is missing or does not import is compiled by the driver from its
    // template (shaders-rdna3 / shaders-rdna2, the same kernel built for the driver), so one pipeline never takes the
    // network down. gfx10.3's bundle SPIR-V is shaders-rdna2's itself: without a template the driver compiles it.
    const auto name=path.substr(path.find_last_of("/\\")+1);
    try {
        const VkResult r=create_aco(device,cache,ci,dir,path,code,bytes,out,push_bytes);
        nr::logf("[nr] ACO %s %s: %s",tag.c_str(),name.c_str(),native_shell(name)?"driver (NR_ACO_NATIVE_KERNELS)":"imported");
        return r;
    } catch(const std::exception& e) {
        std::string t;
        for(const auto& d:shell_dirs()) {
            std::ifstream probe(nr::u8::path(d+"/"+name),std::ios::binary);
            if(probe) {t=d+"/"+name;break;}
        }
        nr::logf("[nr] ACO %s %s: not imported (%s); %s",tag.c_str(),name.c_str(),e.what(),
                 !t.empty()?"the driver compiles its template":tag=="gfx103x"?"the driver compiles it":"no template to compile instead");
        if(t.empty() && tag=="gfx103x")return vkCreateComputePipelines(device,cache,1,&ci,nullptr,out);
        if(t.empty())throw;
        auto shell=pal::load(t);
        pal::require(shell.size()%4==0,"invalid shell SPIR-V size");
        std::vector<uint32_t> words(shell.size()/4);std::memcpy(words.data(),shell.data(),shell.size());
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=shell.size();sm.pCode=words.data();
        VkShaderModule module{};check(vkCreateShaderModule(device,&sm,nullptr,&module),"create template module");
        auto native_ci=ci;native_ci.stage.module=module;
        const VkResult r=vkCreateComputePipelines(device,cache,1,&native_ci,nullptr,out);
        vkDestroyShaderModule(device,module,nullptr);
        return r;
    }
}
inline VkResult create_aco(VkDevice device,VkPipelineCache cache,const VkComputePipelineCreateInfo& ci,const std::string& dir,
                           const std::string& path,const uint32_t* code,size_t bytes,VkPipeline* out,uint32_t push_bytes) {
    pal::require(enabled(device),"binary mode was not enabled when this device was created");
    auto h=pal::hash(code,bytes);
    const auto record_bytes=pal::load(dir+"/records/"+keyname(h));
    auto record=pal::Record::parse(record_bytes);
    pal::require(record.spv_hash==h,"SPIR-V record mismatch");
    std::array<uint32_t,3> workgroup{};
    for(size_t p=5;p<bytes/4;) {
        auto n=code[p]>>16,op=code[p]&65535u;
        pal::require(n && n<=bytes/4-p,"invalid SPIR-V instruction");
        if(op==16 && n==6 && code[p+2]==17) {
            pal::require(!workgroup[0],"multiple SPIR-V local sizes");
            workgroup={code[p+3],code[p+4],code[p+5]};
        }
        p+=n;
    }
    pal::require(workgroup[0]!=0,"literal SPIR-V local size required");
    // The PAL container comes from the driver's own compile of a template SPIR-V with the same interface:
    // the first shell directory holding the name (NR_ACO_SHELL_DIR, or the product's aco/shell then the
    // native network shaders), else the Linux SPIR-V itself.
    std::string shell_path;
    {
        auto name=path.substr(path.find_last_of("/\\")+1);
        // The name itself, else the native shader that serves as its template (the bundle's alias list).
        // A temporal pre/post block's template is looked for in a temporal\ folder first (RDNA3's shells keep them there).
        const bool temporal=name.rfind("temporal_",0)==0;
        for(const auto& n:{name,alias_of(name)}) {
            if(n.empty() || !shell_path.empty())continue;
            for(const auto& d:shell_dirs()) {
                for(const auto& f:{temporal?d+"/temporal/"+n:std::string(),d+"/"+n}) {
                    if(f.empty())continue;
                    std::ifstream probe(nr::u8::path(f),std::ios::binary);
                    if(probe) {shell_path=f;break;}
                }
                if(!shell_path.empty())break;
            }
        }
    }
    if(!shell_path.empty()) {
        auto name=path.substr(path.find_last_of("/\\")+1);
        auto shell=pal::load(shell_path);
        pal::require(shell.size()%4==0,"invalid shell SPIR-V size");
        std::vector<uint32_t> words(shell.size()/4);std::memcpy(words.data(),shell.data(),shell.size());
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=shell.size();sm.pCode=words.data();
        VkShaderModule module{};check(vkCreateShaderModule(device,&sm,nullptr,&module),"create shell module");
        auto shell_ci=ci;shell_ci.stage.module=module;
        try{
            if(native_shell(name)) {
                std::fprintf(stderr,"ACO diagnostic native shell: %s\n",name.c_str());
                check(vkCreateComputePipelines(device,cache,1,&shell_ci,nullptr,out),"create native shell pipeline");
            } else {
                // The cached import is valid for this record, this template, these push/workgroup values,
                // this GPU + driver, this build (the splice that made it: a plugin replaced by hand keeps
                // the folder's aco-cache\ of an older build) and this device's user-data layout (a device with
                // VK_EXT_vertex_input_dynamic_state puts the push constants one entry later).
                std::string cache_file;
                const std::string cache=cache_dir();
                if(!cache.empty()) {
                    Functions f(device);
                    const uint32_t push_base=device_code(f,device,shell_ci,push_bytes).push_base;
#ifdef NR_BUILD_STAMP
                    static const char build[]=NR_BUILD_STAMP;
#else
                    static const char build[]="unstamped";
#endif
                    uint64_t k[7]={pal::hash(record_bytes.data(),record_bytes.size()),pal::hash(shell.data(),shell.size()),
                                   identity(),push_bytes,(uint64_t(workgroup[0])<<32)|workgroup[1],workgroup[2],
                                   pal::hash(build,sizeof build-1)^(uint64_t(push_base-1)<<56)};
                    char b[40];std::snprintf(b,sizeof b,"%016llx.nrb",static_cast<unsigned long long>(pal::hash(k,sizeof k)));
                    cache_file=cache+"/"+b;
                }
                *out=create(device,shell_ci,record,push_bytes,workgroup,cache_file);
            }
        }catch(...){vkDestroyShaderModule(device,module,nullptr);throw;}
        vkDestroyShaderModule(device,module,nullptr);return VK_SUCCESS;
    }
    pal::require(!gfx11_bundle(dir),"no gfx11 template (shaders-rdna3) for this kernel");
    nr::logf("[nr] ACO network: no template for %s; the driver compiles its own SPIR-V for the container",
             path.substr(path.find_last_of("/\\")+1).c_str());
    *out=create(device,ci,record,push_bytes,workgroup);return VK_SUCCESS;
}
inline VkResult create_or_default(VkDevice device,VkPipelineCache cache,const VkComputePipelineCreateInfo& ci,
                                 const std::string& path,const uint32_t* code,size_t bytes,VkPipeline* out,uint32_t push_bytes) {
    auto dir=directory();
    if(dir.empty() || !network_path(path) || driver_only)return vkCreateComputePipelines(device,cache,1,&ci,nullptr,out);
    // The FP16 networks' bundles fall back a pipeline at a time; aco's failure turns ACO Mode off (Error).
    const std::string tag=fallback_bundle(dir);
    if(!tag.empty())return create_fallback(device,cache,ci,dir,tag,path,code,bytes,out,push_bytes);
    try {
        return create_aco(device,cache,ci,dir,path,code,bytes,out,push_bytes);
    } catch(const Error&) {
        throw;
    } catch(const std::bad_alloc&) {
        throw;
    } catch(const VkFailure& e) {
        if(e.result==VK_ERROR_OUT_OF_HOST_MEMORY || e.result==VK_ERROR_OUT_OF_DEVICE_MEMORY || e.result==VK_ERROR_DEVICE_LOST)throw;
        throw Error(e.what());
    } catch(const std::exception& e) {
        throw Error(e.what());
    }
}
} // namespace nr::binary
