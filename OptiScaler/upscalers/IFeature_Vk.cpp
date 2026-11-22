#include <pch.h>

#include <algorithm>
#include <functional>
#include <vector>

#include "IFeature_Vk.h"
#include "FeatureProvider_Vk.h"
#include "State.h"
#include "nvsdk_ngx_vk.h"
#include <dlssnr/DlssNrFeature_Vk.h>

// Vulkan has no equivalent of a D3D12 resource state promotion, so a transfer states where its two
// images are and puts them back. Surfaces between two pipeline stages rest in VK_IMAGE_LAYOUT_GENERAL.
static void TransferBarrier(VkCommandBuffer cmdBuffer, VkImage image, VkImageLayout from, VkImageLayout to)
{
    if (image == VK_NULL_HANDLE || from == to)
        return;

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                            VK_ACCESS_TRANSFER_READ_BIT;

    // Nothing to make visible out of UNDEFINED: the contents are discarded by the transition.
    barrier.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : barrier.dstAccessMask;

    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
}

static uint32_t FindMemoryTypeIndex(VkPhysicalDevice physicalDevice, uint32_t typeBits,
                                    VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps {};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }

    return UINT32_MAX;
}

void IFeature_Vk::ReleaseEnlargerInput()
{
    if (Device == VK_NULL_HANDLE)
    {
        EnlargerInput = OwnedSurface {};
        return;
    }

    if (EnlargerInput.View != VK_NULL_HANDLE)
        vkDestroyImageView(Device, EnlargerInput.View, nullptr);

    if (EnlargerInput.Image != VK_NULL_HANDLE)
        vkDestroyImage(Device, EnlargerInput.Image, nullptr);

    if (EnlargerInput.Memory != VK_NULL_HANDLE)
        vkFreeMemory(Device, EnlargerInput.Memory, nullptr);

    EnlargerInput = OwnedSurface {};
}

// A render-resolution surface carrying the format of the frame it will become.
//
// Shader_Vk has a helper for this and keeps it protected, so the pipeline builds its own. STORAGE
// because the model's resolve writes it, SAMPLED because the enlarging upscaler reads it, TRANSFER
// both ways because the fallback copy uses it as either end.
bool IFeature_Vk::EnsureEnlargerInput(VkCommandBuffer InCmdBuffer, VkFormat format, uint32_t width, uint32_t height)
{
    if (Device == VK_NULL_HANDLE || PhysicalDevice == VK_NULL_HANDLE || width == 0 || height == 0 ||
        format == VK_FORMAT_UNDEFINED)
        return false;

    if (EnlargerInput.Image != VK_NULL_HANDLE && EnlargerInput.Width == width && EnlargerInput.Height == height &&
        EnlargerInput.Format == format)
    {
        // Written by the model's resolve as a storage image, which is only legal in GENERAL. Taken
        // there from UNDEFINED every frame, the way the other stages take their own intermediates:
        // wherever the enlargement left it is unknown here, and the resolve rewrites every texel so
        // the discard costs nothing.
        TransferBarrier(InCmdBuffer, EnlargerInput.Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        return true;
    }

    // Previous frames' command buffers still reference the surface about to be freed. Only a
    // resolution change reaches here, so the stall is a one-off hitch rather than a per-frame cost.
    if (EnlargerInput.Image != VK_NULL_HANDLE)
        vkDeviceWaitIdle(Device);

    ReleaseEnlargerInput();

    VkImageCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { width, height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(Device, &info, nullptr, &EnlargerInput.Image) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR dual feature: could not create a {}x{} enlargement input", width, height);
        ReleaseEnlargerInput();
        return false;
    }

    VkMemoryRequirements req {};
    vkGetImageMemoryRequirements(Device, EnlargerInput.Image, &req);

    VkMemoryAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        FindMemoryTypeIndex(PhysicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(Device, &alloc, nullptr, &EnlargerInput.Memory) != VK_SUCCESS ||
        vkBindImageMemory(Device, EnlargerInput.Image, EnlargerInput.Memory, 0) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR dual feature: could not back a {}x{} enlargement input", width, height);
        ReleaseEnlargerInput();
        return false;
    }

    VkImageViewCreateInfo view {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = EnlargerInput.Image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (vkCreateImageView(Device, &view, nullptr, &EnlargerInput.View) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR dual feature: could not view a {}x{} enlargement input", width, height);
        ReleaseEnlargerInput();
        return false;
    }

    EnlargerInput.Width = width;
    EnlargerInput.Height = height;
    EnlargerInput.Format = format;

    TransferBarrier(InCmdBuffer, EnlargerInput.Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    return true;
}

// The upscaler that enlarges what the model edited, built once on first use.
//
// Created through the same provider as any other upscaler in this tree, so the choice degrades the way
// every other upscaler choice does -- ask for DLSS on a machine without it and FSR arrives instead.
//
// The provider reads the resolutions from the parameter block, and this feature's own half of the
// split has already lowered them. They are put back exactly as found: the block is the game's.
bool IFeature_Vk::EnsureEnlarger(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters)
{
    const auto wanted = Config::Instance()->DlssNrDualEnlarger.value_for_config();

    if (!wanted.has_value())
        return false;

    if (Enlarger != nullptr)
        return EnlargerType == wanted;

    if (EnlargerType.has_value())
        return false; // a build already failed for this choice; do not retry every frame

    EnlargerType = wanted;

    unsigned int width = 0, height = 0, outWidth = 0, outHeight = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Width, &width);
    InParameters->Get(NVSDK_NGX_Parameter_Height, &height);
    InParameters->Get(NVSDK_NGX_Parameter_OutWidth, &outWidth);
    InParameters->Get(NVSDK_NGX_Parameter_OutHeight, &outHeight);

    InParameters->Set(NVSDK_NGX_Parameter_Width, RenderWidth());
    InParameters->Set(NVSDK_NGX_Parameter_Height, RenderHeight());
    InParameters->Set(NVSDK_NGX_Parameter_OutWidth, DisplayWidth());
    InParameters->Set(NVSDK_NGX_Parameter_OutHeight, DisplayHeight());

    std::unique_ptr<IFeature_Vk> built = nullptr;
    bool ok = FeatureProvider_Vk::GetFeature(wanted.value(), IFeature::GetNextHandleId(), InParameters, &built) &&
              built != nullptr;

    if (ok)
    {
        built->MarkEnlargementStage();
        ok = built->Init(Instance, PhysicalDevice, Device, InCmdBuffer, GIPA, GDPA, InParameters);
    }

    InParameters->Set(NVSDK_NGX_Parameter_Width, width);
    InParameters->Set(NVSDK_NGX_Parameter_Height, height);
    InParameters->Set(NVSDK_NGX_Parameter_OutWidth, outWidth);
    InParameters->Set(NVSDK_NGX_Parameter_OutHeight, outHeight);

    if (!ok)
    {
        LOG_ERROR("DLSS-NR dual feature: {} would not build for the enlargement, falling back to the output scaler",
                  UpscalerDisplayName(wanted.value()));
        return false;
    }

    Enlarger = std::move(built);

    LOG_INFO("DLSS-NR dual feature: {} enlarges {}x{} to {}x{} after the model", Enlarger->Name(), RenderWidth(),
             RenderHeight(), DisplayWidth(), DisplayHeight());

    return true;
}
#include <misc/DeferredRelease.h>

bool IFeature_Vk::Init(VkInstance InInstance, VkPhysicalDevice InPD, VkDevice InDevice, VkCommandBuffer InCmdBuffer,
                       PFN_vkGetInstanceProcAddr InGIPA, PFN_vkGetDeviceProcAddr InGDPA,
                       NVSDK_NGX_Parameter* InParameters)
{
    Instance = InInstance;
    PhysicalDevice = InPD;
    Device = InDevice;
    GIPA = InGIPA;
    GDPA = InGDPA;

    auto result = InitInternal(InCmdBuffer, InParameters);

    if (result)
    {
        OutputScaler = std::make_unique<OS_Vk>("Output Scaling", InDevice, InPD, (TargetWidth() < DisplayWidth()));
        RCAS = std::make_unique<RCAS_Vk>("RCAS", InDevice, InPD);
        Magnifier = std::make_unique<Magnifier_Vk>("Magnifier", InDevice, InPD);
        UpscalerTime = std::make_unique<GpuTime_Vk>(InDevice, InPD);
    }

    return result;
}

bool IFeature_Vk::Evaluate(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters)
{
    // Frees deferred objects even when presents aren't hooked
    DeferredRelease::Collect();

    if (!IsInited())
    {
        LOG_ERROR("Not inited!");
        return false;
    }

    if (Config::Instance()->OverrideSharpness.value_or_default())
        _sharpness = Config::Instance()->Sharpness.value_or_default();
    else
        _sharpness = GetSharpness(InParameters);

    if (_sharpness > 1.0f)
        _sharpness = 1.0f;

    auto upscaler = GetUpscalerType();
    bool useRcas = upscaler == Upscaler::XeSS ||
                   (upscaler == Upscaler::DLSS && Version() >= feature_version(2, 5, 1)) || upscaler == Upscaler::DLSSD;

    if (!useRcas)
        useRcas = Config::Instance()->RcasEnabled.value_or_default();

    if (_sharpness == 0.0f)
        useRcas = false;

    if (!useRcas && (Config::Instance()->MotionSharpnessEnabled.value_or_default() &&
                     Config::Instance()->MotionSharpness.value_or_default() > 0.0f))
    {
        useRcas = true;
    }

    if (!RCAS->IsInit())
        useRcas = false;

    // The model between the halves of the upscaler. SetInitParameters has already pointed the upscaler
    // at render resolution, so the enlargement is not optional here -- without it the frame reaching
    // the game would be the small one.
    const bool useDualFeature = DualFeatureSplit();

    // Asked for and not taken. The split is decided from three numbers settled when the feature was
    // built, so a mismatch here is silent and looks exactly like the option doing nothing.
    if (!useDualFeature && !_isEnlargementStage && Config::Instance()->DlssNrDualFeature.value_or_default() &&
        Config::Instance()->DlssNrEnabled.value_or_default())
    {
        static unsigned int saidTarget = 0;

        if (saidTarget != TargetWidth())
        {
            saidTarget = TargetWidth();
            LOG_WARN("DLSS-NR dual feature: asked for, not taken -- target {}x{}, render {}x{}, display {}x{}",
                     TargetWidth(), TargetHeight(), RenderWidth(), RenderHeight(), DisplayWidth(), DisplayHeight());
        }
    }

    // An upscaler does the enlarging when one is asked for and builds. Otherwise the spatial scaler,
    // which needs nothing the first half has already consumed and so cannot be wrong about it.
    const bool useUpscalerEnlarger = useDualFeature && EnsureEnlarger(InCmdBuffer, InParameters);

    bool useOutputScaling =
        (useDualFeature && !useUpscalerEnlarger) || (Config::Instance()->OutputScalingEnabled.value_or_default() &&
                                                     (LowResMV() || RenderWidth() == DisplayWidth()));

    if (!OutputScaler->IsInit())
        useOutputScaling = false;

    NVSDK_NGX_Resource_VK* paramOutput = nullptr;
    NVSDK_NGX_Resource_VK* paramMotion = nullptr;
    NVSDK_NGX_Resource_VK* paramDepth = nullptr;

    InParameters->Get(NVSDK_NGX_Parameter_Output, (void**) &paramOutput);
    InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &paramMotion);
    InParameters->Get(NVSDK_NGX_Parameter_Depth, (void**) &paramDepth);

    // Save the original output so we can restore it later
    VkImageInfo originalOutput {};
    if (paramOutput)
    {
        originalOutput.Image = paramOutput->Resource.ImageViewInfo.Image;
        originalOutput.ImageView = paramOutput->Resource.ImageViewInfo.ImageView;
        originalOutput.SubresourceRange = paramOutput->Resource.ImageViewInfo.SubresourceRange;
        originalOutput.Format = paramOutput->Resource.ImageViewInfo.Format;
        originalOutput.Width = paramOutput->Resource.ImageViewInfo.Width;
        originalOutput.Height = paramOutput->Resource.ImageViewInfo.Height;
    }

    const VkImageUsageFlags intermediateUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Order is important as that's the order of shader dispatch
    std::vector<ShaderPass> pipeline;

    // First, so it runs on what the upscaler wrote and before anything enlarges it. The model asks for
    // a 1:1 scaling ratio at every quality level, so the only way to run it on fewer pixels is to give
    // it a smaller frame -- which is what the upscaler writing at render resolution produces.
    if (useDualFeature)
    {
        pipeline.push_back({ // Setup
                             [&](const VkImageInfo& nextOutput) -> VkImageInfo
                             { return DlssNr::StageInputSurfaceVk(InCmdBuffer, Device, PhysicalDevice, nextOutput); },

                             // Dispatch
                             [&](const VkImageInfo& input, const VkImageInfo& output) -> bool
                             {
                                 if (DlssNr::EvaluateStageVk(InCmdBuffer, InParameters, Instance, PhysicalDevice,
                                                             Device, input, output))
                                     return true;

                                 // A pass that declines leaves the frame where it is, so the enlargement still has
                                 // something to read. The upscaler's own result, unedited, is the right fallback.
                                 // Both surfaces rest in GENERAL, and declining puts the input back there.
                                 TransferBarrier(InCmdBuffer, input.Image, VK_IMAGE_LAYOUT_GENERAL,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                                 TransferBarrier(InCmdBuffer, output.Image, VK_IMAGE_LAYOUT_GENERAL,
                                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

                                 VkImageCopy region {};
                                 region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                                 region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                                 region.extent = { std::min(input.Width, output.Width),
                                                   std::min(input.Height, output.Height), 1 };

                                 vkCmdCopyImage(InCmdBuffer, input.Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                output.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                                 TransferBarrier(InCmdBuffer, input.Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 VK_IMAGE_LAYOUT_GENERAL);
                                 TransferBarrier(InCmdBuffer, output.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_IMAGE_LAYOUT_GENERAL);

                                 return true;
                             } });
    }

    if (useUpscalerEnlarger)
    {
        pipeline.push_back(
            { // Setup
              [&](const VkImageInfo& nextOutput) -> VkImageInfo
              {
                  // Render resolution, matching what the first half wrote rather than what this stage
                  // produces -- nextOutput is the game's frame and is display sized.
                  if (!EnsureEnlargerInput(InCmdBuffer, nextOutput.Format, RenderWidth(), RenderHeight()))
                      return VkImageInfo {};

                  VkImageInfo info = nextOutput;
                  info.Image = EnlargerInput.Image;
                  info.ImageView = EnlargerInput.View;
                  info.SubresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                  info.Width = EnlargerInput.Width;
                  info.Height = EnlargerInput.Height;
                  return info;
              },

              // Dispatch
              [&](const VkImageInfo& input, const VkImageInfo& output) -> bool
              {
                  // The game's own block, borrowed. Everything the enlarging upscaler needs per frame
                  // -- motion vectors, depth, jitter, the reset -- is the game's and already in it;
                  // only the two frames differ from what the game described. Vulkan carries those two
                  // as wrappers the game owns, so the fields are swapped in place and put back.
                  //
                  // input arrives in VK_IMAGE_LAYOUT_GENERAL, which is the layout the game's own
                  // colour buffer arrives in and what NGX requires of a resource it is handed. The
                  // substitution therefore hands the enlarger exactly the contract its Color slot
                  // already has, whatever that backend then declares the resource to be in.
                  NVSDK_NGX_Resource_VK* paramColour = nullptr;
                  InParameters->Get(NVSDK_NGX_Parameter_Color, (void**) &paramColour);

                  if (paramColour == nullptr || paramOutput == nullptr)
                  {
                      LOG_WARN("DLSS-NR dual feature: no colour or output wrapper to point at the enlargement");
                      return false;
                  }

                  const NVSDK_NGX_ImageViewInfo_VK gameColour = paramColour->Resource.ImageViewInfo;

                  const auto point = [](NVSDK_NGX_Resource_VK* res, const VkImageInfo& at)
                  {
                      res->Resource.ImageViewInfo.Image = at.Image;
                      res->Resource.ImageViewInfo.ImageView = at.ImageView;
                      res->Resource.ImageViewInfo.SubresourceRange = at.SubresourceRange;
                      res->Resource.ImageViewInfo.Format = at.Format;
                      res->Resource.ImageViewInfo.Width = at.Width;
                      res->Resource.ImageViewInfo.Height = at.Height;
                  };

                  point(paramColour, input);
                  point(paramOutput, output);

                  const bool ok = Enlarger->Evaluate(InCmdBuffer, InParameters);

                  paramColour->Resource.ImageViewInfo = gameColour;

                  // Dropped rather than retried: EnlargerType stays set, so EnsureEnlarger declines
                  // from here on and the next frame is built around the spatial scaler instead.
                  if (!ok)
                  {
                      LOG_WARN("DLSS-NR dual feature: the enlargement failed, dropping back to the output scaler");
                      Enlarger.reset();
                  }

                  return ok;
              } });
    }

    if (useOutputScaling)
    {
        pipeline.push_back(
            { // Setup
              [&](const VkImageInfo& nextOutput) -> VkImageInfo
              {
                  if (OutputScaler->CreateImageResource(Device, PhysicalDevice, TargetWidth(), TargetHeight(),
                                                        nextOutput.Format, intermediateUsage))
                  {
                      OutputScaler->SetImageLayout(InCmdBuffer, OutputScaler->GetImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                                   VK_IMAGE_LAYOUT_GENERAL, nextOutput.SubresourceRange);

                      // TODO: improve query of the output info
                      VkImageInfo info = nextOutput;
                      info.Image = OutputScaler->GetImage();
                      info.ImageView = OutputScaler->GetImageView();
                      info.Width = TargetWidth();
                      info.Height = TargetHeight();
                      return info;
                  }
                  return VkImageInfo {}; // Returns null handle
              },

              // Dispatch
              [&](const VkImageInfo& input, const VkImageInfo& output) -> bool
              {
                  LOG_DEBUG("Scaling output...");
                  OutputScaler->SetImageLayout(InCmdBuffer, input.Image, VK_IMAGE_LAYOUT_GENERAL,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, input.SubresourceRange);

                  if (!OutputScaler->Dispatch(InCmdBuffer, input, output))
                  {
                      Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
                      State::Instance().changeBackend[Handle()->Id] = true;
                      return false;
                  }
                  return true;
              } });
    }

    _actualSharpness = _sharpness;
    if (useRcas)
    {
        pipeline.push_back(
            { // Setup
              [&](const VkImageInfo& nextOutput) -> VkImageInfo
              {
                  InParameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
                  _sharpness = 0.0f;

                  if (RCAS->CreateImageResource(Device, PhysicalDevice, nextOutput.Width, nextOutput.Height,
                                                nextOutput.Format, intermediateUsage))
                  {
                      RCAS->SetImageLayout(InCmdBuffer, RCAS->GetImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_GENERAL, nextOutput.SubresourceRange);

                      VkImageInfo info = nextOutput;
                      info.Image = RCAS->GetImage();
                      info.ImageView = RCAS->GetImageView();
                      return info;
                  }
                  return VkImageInfo {};
              },

              // Dispatch
              [&](const VkImageInfo& input, const VkImageInfo& output) -> bool
              {
                  if (!RCAS->CanRender() || !paramMotion || !paramOutput)
                      return true;

                  RCAS->SetImageLayout(InCmdBuffer, input.Image, VK_IMAGE_LAYOUT_GENERAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, input.SubresourceRange);

                  RcasConstants rcasConstants {};
                  rcasConstants.Sharpness = _actualSharpness.value_or(_sharpness);
                  rcasConstants.DepthIsLinear = DepthLinear();
                  rcasConstants.DepthIsReversed = DepthInverted();
                  rcasConstants.IsHdr = IsHdr();

                  _sharpness = _actualSharpness.value_or(_sharpness);
                  _actualSharpness.reset();

                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &rcasConstants.MvScaleX);
                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &rcasConstants.MvScaleY);

                  float nearPlane = 0.0f;
                  float farPlane = 0.0f;

                  if (InParameters->Get("DLSSG.CameraNear", &nearPlane) == NVSDK_NGX_Result_Success &&
                      InParameters->Get("DLSSG.CameraFar", &farPlane) == NVSDK_NGX_Result_Success)
                  {
                      rcasConstants.CameraNear = nearPlane;
                      rcasConstants.CameraFar = farPlane;
                  }
                  else
                  {
                      rcasConstants.CameraNear = Config::Instance()->FsrCameraNear.value_or_default();
                      rcasConstants.CameraFar = Config::Instance()->FsrCameraFar.value_or_default();
                  }

                  // In Vulkan we pass the Info structs instead of raw resources
                  VkImageInfo mvInfo = *(VkImageInfo*) &paramMotion->Resource.ImageViewInfo;
                  VkImageInfo depthInfo = *(VkImageInfo*) &paramDepth->Resource.ImageViewInfo;

                  if (!RCAS->Dispatch(Device, InCmdBuffer, rcasConstants, input, mvInfo, output, &depthInfo))
                  {
                      Config::Instance()->RcasEnabled.set_volatile_value(false);
                      return false;
                  }
                  return true;
              } });
    }

    if (Magnifier->ShouldRun())
    {
        pipeline.push_back(
            { // Setup
              [&](const VkImageInfo& nextOutput) -> VkImageInfo
              {
                  if (Magnifier->CreateImageResource(Device, PhysicalDevice, nextOutput.Width, nextOutput.Height,
                                                     nextOutput.Format, intermediateUsage))
                  {
                      Magnifier->SetImageLayout(InCmdBuffer, Magnifier->GetImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                                VK_IMAGE_LAYOUT_GENERAL, nextOutput.SubresourceRange);

                      VkImageInfo info = nextOutput;
                      info.Image = Magnifier->GetImage();
                      info.ImageView = Magnifier->GetImageView();
                      return info;
                  }
                  return VkImageInfo {};
              },

              // Dispatch
              [&](const VkImageInfo& input, const VkImageInfo& output) -> bool
              {
                  if (!Magnifier->CanRender())
                      return true;

                  Magnifier->SetImageLayout(InCmdBuffer, input.Image, VK_IMAGE_LAYOUT_GENERAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, input.SubresourceRange);

                  // In Vulkan we pass the Info structs instead of raw resources
                  VkImageInfo mvInfo = *(VkImageInfo*) &paramMotion->Resource.ImageViewInfo;
                  VkImageInfo depthInfo = *(VkImageInfo*) &paramDepth->Resource.ImageViewInfo;

                  return Magnifier->Dispatch(InCmdBuffer, input, output);
              } });
    }

    // Iterate BACKWARDS to establish where each shader needs to pull its input from
    VkImageInfo currentTarget = originalOutput;
    for (auto it = pipeline.rbegin(); it != pipeline.rend(); ++it)
    {
        VkImageInfo requiredInput = it->Setup(currentTarget);
        if (requiredInput.Image != VK_NULL_HANDLE)
        {
            it->outputBuffer = currentTarget;
            it->inputBuffer = requiredInput;
            currentTarget = requiredInput; // Shift the target back for the next previous stage
        }
    }

    // Write target back into the params
    // In DX11/DX12 we set ngx param but in Vulkan we can set just the resource info
    if (paramOutput)
    {
        paramOutput->Resource.ImageViewInfo.Image = currentTarget.Image;
        paramOutput->Resource.ImageViewInfo.ImageView = currentTarget.ImageView;
        paramOutput->Resource.ImageViewInfo.SubresourceRange = currentTarget.SubresourceRange;
        paramOutput->Resource.ImageViewInfo.Format = currentTarget.Format;
        paramOutput->Resource.ImageViewInfo.Width = currentTarget.Width;
        paramOutput->Resource.ImageViewInfo.Height = currentTarget.Height;
    }

    // The wrapper is the game's and it is pointing at a surface of this pipeline's, which under the
    // split is render sized. Every exit from here puts it back.
    const auto restoreOutput = [&]()
    {
        if (paramOutput == nullptr)
            return;

        paramOutput->Resource.ImageViewInfo.Image = originalOutput.Image;
        paramOutput->Resource.ImageViewInfo.ImageView = originalOutput.ImageView;
        paramOutput->Resource.ImageViewInfo.SubresourceRange = originalOutput.SubresourceRange;
        paramOutput->Resource.ImageViewInfo.Format = originalOutput.Format;
        paramOutput->Resource.ImageViewInfo.Width = originalOutput.Width;
        paramOutput->Resource.ImageViewInfo.Height = originalOutput.Height;
    };

    UpscalerTime->Start(InCmdBuffer);

    auto evalResult = EvaluateInternal(InCmdBuffer, InParameters);

    UpscalerTime->End(InCmdBuffer);

    if (!evalResult)
    {
        restoreOutput();

        static bool said = false;

        if (!said)
        {
            said = true;
            LOG_ERROR("Upscaler evaluate failed; {} pipeline stage(s) skipped this frame", pipeline.size());
        }

        return false;
    }

    // Iterate FORWARDS to execute the shaders in the defined order
    for (auto& pass : pipeline)
    {
        if (pass.inputBuffer.Image != VK_NULL_HANDLE && pass.outputBuffer.Image != VK_NULL_HANDLE)
        {
            if (!pass.Dispatch(pass.inputBuffer, pass.outputBuffer))
            {
                restoreOutput();
                return false;
            }
        }
    }

    restoreOutput();

    _frameCount++;

    return evalResult;
}

IFeature_Vk::~IFeature_Vk()
{
    if (State::Instance().isShuttingDown)
        return;

    // The enlargement is another feature on this device; it goes before the surface between them.
    Enlarger.reset();
    ReleaseEnlargerInput();
}

std::optional<double> IFeature_Vk::ReadUpscalerTime(void* unused)
{
    if (!UpscalerTime || !RCAS || !OutputScaler)
        return std::nullopt;

    lastUpscalerTime = UpscalerTime->ReadGpuTime();
    lastRcasTime = RCAS->ReadGpuTime();
    lastOutputScalingTime = OutputScaler->ReadGpuTime();

    return sumOpts(lastUpscalerTime, lastRcasTime, lastOutputScalingTime);
}

void IFeature_Vk::ReadDetailedGpuTimes(void* unused, std::vector<DetailedGpuTime>& detailedGpuTimes)
{
    detailedGpuTimes.clear();

    // Do not call ReadGpuTime twice for shaders
    if (lastUpscalerTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { ShortName(), lastUpscalerTime.value(), true });

    if (lastRcasTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { RCAS->Name(), lastRcasTime.value(), true });

    if (lastOutputScalingTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { OutputScaler->Name(), lastOutputScalingTime.value(), true });

    if (!Magnifier)
        return;

    auto magnifierTime = Magnifier->ReadGpuTime();

    if (magnifierTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { Magnifier->Name(), magnifierTime.value(), false });
}
