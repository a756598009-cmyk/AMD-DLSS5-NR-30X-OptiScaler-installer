#pragma once

#include <vulkan/vulkan.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_vk.h>

#include <string>

// DLSS-NR on AMD at the seam of OptiScaler's native Vulkan upscalers, before Super Resolution.
//
// The AMD runtimes take D3D12 resources (danielblnc, lmxxf, and mochizuki, which imports them into its own Vulkan
// device), so the frame crosses to OptiScaler's D3D12 device the way the Vk w/Dx12 upscalers cross it: images shared
// between the two APIs, a D3D12 fence imported as a timeline semaphore, and the game's command buffer split at the
// evaluate through Vulkan_wDx12's pending submission. The D3D12 seam (DlssNr::EvaluateBeforeUpscale, AmdBridge) runs
// on the copies unchanged, so the settings, status lines, fallbacks and per-game quirks are that path's. Its answer
// lands in an image of ours that the upscaler reads as its colour. Nothing crosses through the CPU.
//
//   game command buffer:  ... evaluate -> copies of colour, depth, motion, exposure -> signal R
//   D3D12 queue:          wait R -> AmdBridge (the runtime) -> answer into the shared colour -> signal D
//   copy-back:            wait D -> the shared colour back to Vulkan
//   continuation:         the upscaler, then whatever the game records after its evaluate
//
// The Vk w/Dx12 upscalers already run on D3D12 and take the D3D12 seam there; this one never runs for them.
namespace DlssNr::VkSeam
{
// An AMD GPU and an AMD runtime installed beside OptiScaler: NR on this device goes through this seam, and NVIDIA's
// Vulkan model (nvngx.dll_dlssnr.dll) stands aside.
bool Owns(VkPhysicalDevice physicalDevice);

// Called before the upscaler records. Returns the command buffer it must record into: cmd itself when the frame
// passes through, else the continuation the game's recording moves to. When the runtime answered, the parameter
// block's colour is ours until After.
VkCommandBuffer Before(VkCommandBuffer cmd, NVSDK_NGX_Parameter* params, VkPhysicalDevice physicalDevice,
                       VkDevice device);

// After the upscaler recorded: the game's colour goes back into the parameter block.
void After(NVSDK_NGX_Parameter* params);

// Frames the runtime's answer reached an upscaler through this seam, and the last reason it did not, for the menu.
unsigned long long Frames();
std::string Status();
} // namespace DlssNr::VkSeam
