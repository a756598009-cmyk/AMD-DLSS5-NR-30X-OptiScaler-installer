#pragma once
#include <vulkan/vulkan.h>
#include "IFeature.h"

#include <shaders/rcas/RCAS_Vk.h>
#include <shaders/output_scaling/OS_Vk.h>
#include <shaders/magnifier/Magnifier_Vk.h>
#include <gpu_time/GpuTime_Vk.h>

class IFeature_Vk : public virtual IFeature
{
  private:
    struct ShaderPass
    {
        // Requests the target image it needs to write to. Returns the image the PREVIOUS stage must write to.
        std::function<VkImageInfo(const VkImageInfo& nextOutput)> Setup;

        // Runs the shader
        std::function<bool(const VkImageInfo& input, const VkImageInfo& output)> Dispatch;

        // Internal state tracked by the pipeline setup loop
        VkImageInfo inputBuffer {};
        VkImageInfo outputBuffer {};
    };

  protected:
    VkInstance Instance = nullptr;
    VkPhysicalDevice PhysicalDevice = nullptr;
    VkDevice Device = nullptr;
    PFN_vkGetInstanceProcAddr GIPA = nullptr;
    PFN_vkGetDeviceProcAddr GDPA = nullptr;

    std::unique_ptr<OS_Vk> OutputScaler = nullptr;
    std::unique_ptr<RCAS_Vk> RCAS = nullptr;
    std::unique_ptr<Magnifier_Vk> Magnifier = nullptr;

    // An image and everything needed to describe it again. Vulkan cannot be asked what an image was
    // created as, so the shape is kept beside the handles rather than queried the way a D3D12 resource
    // description is.
    struct OwnedSurface
    {
        VkImage Image = VK_NULL_HANDLE;
        VkDeviceMemory Memory = VK_NULL_HANDLE;
        VkImageView View = VK_NULL_HANDLE;
        uint32_t Width = 0;
        uint32_t Height = 0;
        VkFormat Format = VK_FORMAT_UNDEFINED;
    };

    // The second half, when Neural Rendering runs inside this upscaler and the enlargement is another
    // upscaler rather than the spatial output scaler. Built on first use, on this feature's own device,
    // and marked so it does not try to split itself in turn.
    std::unique_ptr<IFeature_Vk> Enlarger = nullptr;
    OwnedSurface EnlargerInput {};
    std::optional<Upscaler> EnlargerType;

    bool EnsureEnlarger(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters);

    // The render-resolution surface between the model and the enlargement. Rebuilt when the size or
    // the format moves under it, which a resolution change does. Left in VK_IMAGE_LAYOUT_GENERAL,
    // where every surface between two of these stages rests.
    bool EnsureEnlargerInput(VkCommandBuffer InCmdBuffer, VkFormat format, uint32_t width, uint32_t height);
    void ReleaseEnlargerInput();
    std::unique_ptr<GpuTime_Vk> UpscalerTime = nullptr;

    virtual bool InitInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters) = 0;
    virtual bool EvaluateInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters) = 0;

  public:
    virtual bool Init(VkInstance InInstance, VkPhysicalDevice InPD, VkDevice InDevice, VkCommandBuffer InCmdBuffer,
                      PFN_vkGetInstanceProcAddr InGIPA, PFN_vkGetDeviceProcAddr InGDPA,
                      NVSDK_NGX_Parameter* InParameters);
    virtual bool Evaluate(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters);

    std::optional<double> ReadUpscalerTime(void* unused) override;
    void ReadDetailedGpuTimes(void* unused, std::vector<DetailedGpuTime>& detailedGpuTimes) override;

    IFeature_Vk(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters) : IFeature(InHandleId, InParameters) {}

    bool IsWithDx12() override { return false; }
    API Api() const override { return API::Vulkan; }

    virtual ~IFeature_Vk();
};
