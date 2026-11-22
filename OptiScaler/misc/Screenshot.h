#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vulkan/vulkan.h>

struct IUnknown;
struct IDXGISwapChain;
struct ID3D11Texture2D;
struct IDirect3DDevice9;
struct IDirect3DSurface9;

// The screenshot key: the next frame the game presents, as it reaches the screen (NR's final image included),
// without OptiScaler's overlays. Each capture point below sits after everything that draws into the frame and
// before the overlay does. The GPU copy happens there; the file is written by a thread of its own.
// With the menu open or the FPS overlay on (and ScreenshotWithMenu), the ...Menu points copy the same frame again
// once the overlay is drawn on it, before the real present: "<name> (menu).png", or "<name> (overlay).png" with
// only the FPS overlay, beside the clean one.
namespace Screenshot
{
void Request();
// <Pictures>\OptiScaler\<exe name>, or ScreenshotPath.
std::filesystem::path Folder();
std::filesystem::path DefaultFolder();

// The folder picker, on a thread of its own. Picked hands the chosen folder over once, on the menu's thread.
void PickFolder();
bool Picking();
std::optional<std::wstring> Picked();

// D3D11 and D3D12 through DXGI. device is the swapchain's device or queue. frame, when given, is the D3D11
// texture the frame is in instead of the back buffer (a D3D10 game's frame on OptiScaler's own device).
void Dxgi(IDXGISwapChain* swapChain, IUnknown* device, ID3D11Texture2D* frame = nullptr);
void DxgiMenu(IDXGISwapChain* swapChain, IUnknown* device, ID3D11Texture2D* frame = nullptr);

struct VulkanTarget
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent {};
    bool transferSource = false;
};
// The copy waits for the present's semaphores, and the present then waits for the copy.
void Vulkan(const VulkanTarget& target, VkPresentInfoKHR& present);
void VulkanMenu(const VulkanTarget& target, VkPresentInfoKHR& present);

// OpenGL, with OptiScaler's context current on the game's device context: true when Gl has work this frame.
bool GlWanted();
void Gl(uint32_t width, uint32_t height, uint32_t generation);
bool GlMenuWanted();
void GlMenu(uint32_t width, uint32_t height, uint32_t generation);

void Dx9(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer);
void Dx9Menu(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer);
} // namespace Screenshot
