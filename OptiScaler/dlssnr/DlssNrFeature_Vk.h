#pragma once

#include <vulkan/vulkan.h>

#include <shaders/dlssnr/DlssNr_Common.h>
#include <shaders/Shader_Vk.h>

#include <optional>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

// DLSS 5 Neural Rendering on Vulkan, natively.
//
// The model ships a complete Vulkan surface -- fourteen exported entry points against D3D12's ten,
// including both extension requirement queries -- so a Vulkan game has no need of the D3D12 bridge
// this pass reached it through before. What stopped it was never the model; it was the device. NGX
// loads its kernels through two NVIDIA vendor extensions no game enables, and a Vulkan device's
// extension list is fixed at creation, so asking afterwards gets a permanent no. OptiScaler already
// appends them in its vkCreateDevice hook, and Enshrouded confirmed the device is created with them.
//
// What differs from the D3D12 path, and why:
//
//   * The game's colour, depth and motion arrive already wrapped. NGX hands Vulkan resources over as
//     NVSDK_NGX_Resource_VK, so only this pass's own images need wrapping.
//   * Layouts are explicit. There is no equivalent of a D3D12 resource state promotion, so every
//     image is moved to the layout each dispatch needs and moved back.
//   * There is no root signature to save and restore, so no envelope. The bindless hazard that cost
//     007 First Light a device on D3D12 has no Vulkan counterpart.
//
// The composition shader is shared, compiled from the same source to SPIR-V. Any behavioural
// difference between the two backends is a bug rather than a design.

class Config;

namespace DlssNr
{

// Runs the model over what the upscaler just wrote, on the same command buffer.
//
// Everything Vulkan needs that D3D12 does not is passed rather than looked up: the device handles
// belong to the game's instance and there is no ambient place to find them from here.
//
// Safe to call every frame. It builds what it needs on first use and disables itself for the session
// rather than retrying into a crash.
void EvaluateAfterUpscaleVk(VkCommandBuffer cmdBuffer, NVSDK_NGX_Parameter* params, VkInstance instance,
                            VkPhysicalDevice physicalDevice, VkDevice device);

// The pass as one stage of an upscaler's own pipeline, on two frames the caller already holds.
//
// Everything the model needs beyond the two frames -- depth, motion vectors, the create flags, the
// reset -- still comes from the parameter block, because those are the game's and unchanged by where
// the stage sits. Answers whether the edit reached dest; false leaves dest untouched.
//
// Both images belong to the caller's pipeline, where surfaces rest in VK_IMAGE_LAYOUT_GENERAL between
// stages. dest is written as a storage image and is bound in GENERAL without a transition, so it must
// be in GENERAL on arrival. Declining puts source back in GENERAL as well, so a caller falling back to
// a copy finds the layout it left.
bool EvaluateStageVk(VkCommandBuffer cmdBuffer, NVSDK_NGX_Parameter* params, VkInstance instance,
                     VkPhysicalDevice physicalDevice, VkDevice device, const VkImageInfo& source,
                     const VkImageInfo& dest);

// The surface the stage before this one should write. Owned here; caller borrows.
// Rebuilt when the frame it must match changes size or format.
//
// Handed back in VK_IMAGE_LAYOUT_GENERAL, which is where an upscaler writing a storage image expects
// to find its target and where the other stages of that pipeline leave theirs.
VkImageInfo StageInputSurfaceVk(VkCommandBuffer cmdBuffer, VkDevice device, VkPhysicalDevice physicalDevice,
                                const VkImageInfo& like);

// Whether the model is being carried by an upscaler's own pipeline: the arrangement is switched on
// and has been seen to work. EvaluateAfterUpscaleVk asks this and declines when it answers yes.
//
// Both halves matter. Asking only the setting made the model silent whenever the split did not apply;
// asking only what happened would keep declining after the setting was turned off.
bool StageCarriesTheModelVk();

// Whether the native Vulkan path is up, and why not if it is not.
bool IsRunningVk();
const char* FailureReasonVk();

// How many frames it has actually composed. The menu needs this to tell "up but nothing has come
// through yet" apart from "running", and the D3D12 counters say nothing about this path.
unsigned long long FramesVk();

// What the pass last cost on the GPU, in milliseconds, or nothing if it has not been measured yet.
// A timestamp pair either side of the whole pass, read three frames later so the query is retired.
std::optional<double> LastGpuTimeVk();

// Whether the game offers an exposure texture on this path. Observed only: it is not read, because
// binding the game's image means naming a layout this side cannot know. For the menu, and to settle
// whether reading it is worth the risk on any real Vulkan game.
bool ExposureOfferedVk();

void ShutdownVk(bool deviceAlive = true);

} // namespace DlssNr
