// The network's pipelines, compiled ahead of the core on several threads.
//
// tools/build-mochizuki-runtime.cmd compiles the core (nr_runtime.cpp, which includes nr_graph.cpp and nrvk.hpp) with
// vkCreateComputePipelines, vkCreateShaderModule, vkCreateDescriptorSetLayout, vkCreatePipelineLayout,
// vkCreatePipelineCache and vkDestroyPipelineCache renamed to the mzi_* functions of mz_interpose.cpp; the core calls
// all six directly, never through vkGetDeviceProcAddr. They forward to the real entry points and, while a Capture is
// open on the calling thread, also record every compute pipeline's create info and the core's pipeline cache.
//
// A network build (MochizukiNrRuntime.cpp, on the builder thread) opens a Capture around the core's constructor:
// - Prewarm, before it: the pipelines dlssnr-amd\prewarm\manifest.txt lists (an earlier build's, each as a SPIR-V file
//   of dlssnr-amd\shaders and a layout) are compiled on several threads into dlssnr-amd\pipeline.cache, which the core
//   then loads, so that its own serial compile finds every one of them there;
// - Finish, after it: pipeline.cache is written again, since the core writes it before it makes its adapter and
//   temporal pipelines, and so is the manifest when it lacks a pipeline of this build or describes other shaders.
// Nothing here throws. A problem is logged, and the core builds as it always did.
#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace mzi
{
class Capture
{
  public:
    // root: the runtime's folder, the core's RuntimeConfig::root. Everything is off without root\dlssnr-amd (the
    // core's development layout, which keeps its pipeline cache elsewhere). variant: L"-rdna3" for the RDNA3
    // network, whose files are shaders-rdna3, pipeline-rdna3.cache and prewarm\manifest-rdna3.txt.
    Capture(VkDevice device, VkPhysicalDevice physical, const std::wstring& root,
            const std::wstring& variant = L"") noexcept;
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // Before the core's constructor. MZ_COMPILE_THREADS threads (default min(8, logical processors - 2); 1 turns the
    // prewarm off) compile the manifest's pipelines at below-normal priority, while the calling thread watches the
    // processor time they get. Only when they together get less than half a processor (normal-priority threads keep
    // every one busy) does the calling thread compile the next pipeline itself, at its own priority, and once none is
    // left, raise the starved threads to its priority for the pipeline each is still compiling. Nothing is compiled,
    // after a log line, when the manifest is missing or does not match the shaders. A set `stop` ends it between two
    // pipelines.
    void Prewarm(const std::atomic<bool>* stop = nullptr) noexcept;

    // After a build in this folder that never finished (MochizukiNrRuntime.cpp, BuildMarker): every pipeline the
    // prewarm and the core compile is logged as it starts and as it ends, so that the next report names the one a
    // crash in the driver's compiler was on.
    void Verbose() noexcept;

    // After the core's constructor returned.
    void Finish() noexcept;

    struct State;

  private:
    std::unique_ptr<State> state;
};

// The FP16 networks' pipeline check (mz_rdna3.cpp) runs in a child process, and AMD's driver keeps a pipeline cache
// for the executable that filled it only (neither its own cache nor a VkPipelineCache the child writes serves the
// game). With a prewarm manifest for the shaders, the child compiles each file the manifest names with the create
// infos it lists (the core's own) and keeps each pipeline's machine code as a VK_KHR_pipeline_binary, which any process
// imports as it is; AdoptBinaries puts them in dlssnr-amd\binaries<variant>\, where the build's Capture imports them
// instead of compiling and the prewarm leaves them out. The network's device has the extension for it (BinaryProbe).
class ManifestPipelines
{
  public:
    // data: the runtime's dlssnr-amd folder; variant as Capture's. Lists nothing when the manifest is missing or was
    // made for other shaders.
    ManifestPipelines(const std::wstring& data, const std::wstring& variant) noexcept;
    ~ManifestPipelines();
    ManifestPipelines(const ManifestPipelines&) = delete;
    ManifestPipelines& operator=(const ManifestPipelines&) = delete;
    // Create infos listed for a shader file (its full path, under data\shaders<variant>).
    size_t Count(const std::wstring& file) const noexcept;
    // Each of them made and destroyed; with `binaries` (a folder) its machine code kept there when the device has
    // VK_KHR_pipeline_binary. The first failure, or VK_SUCCESS.
    VkResult Compile(VkDevice device, const std::wstring& file, const std::wstring& binaries = {}) const noexcept;
    size_t Total() const noexcept;

    struct State;

  private:
    std::unique_ptr<State> state;
};

// The check child's binaries (the folder `from`, with its identity.txt) as data\binaries<variant>\, replacing what was
// there. Logs how many; false when there were none.
bool AdoptBinaries(const std::wstring& data, const std::wstring& variant, const std::wstring& from) noexcept;
// The device and driver a binary folder holds code for (identity.txt): another one's is not imported.
std::string BinaryIdentity(VkPhysicalDevice physical);

// MZ_PROBE_PIPELINE_BINARY=1 (read once per process): Vulkan::Create enables VK_KHR_maintenance5 and
// VK_KHR_pipeline_binary when the device offers both, and logs the global pipeline key (vkGetPipelineKeyKHR without a
// create info) with the pipelineCacheUUID, to find out whether the key depends on the executable. Nothing changes
// when it is unset.
struct BinaryProbe
{
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5 {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR
    };
    VkPhysicalDevicePipelineBinaryFeaturesKHR binaries {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR
    };
    bool enabled = false;

    bool active = false; // the two are on the device (asked for, or `always`)

    // Adds the two extensions to `extensions` and returns the features to chain into the device's create info in
    // front of `next`, when asked for (or `always`: the FP16 networks' binaries) and offered; returns `next` otherwise.
    void* Prepare(VkPhysicalDevice physical, const std::vector<VkExtensionProperties>& offered,
                  std::vector<const char*>& extensions, void* next, bool always = false);
    // Once the device exists.
    void Report(VkDevice device, VkPhysicalDevice physical) const;
};
} // namespace mzi
