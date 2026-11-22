#pragma once

#include <d3d9.h>

// D3D9 frames for the final-image NR and XeFG. A classic D3D9 device cannot share a texture, so there each frame
// crosses to D3D12 through system memory; a D3D9Ex device shares it with D3D12. Without frame generation the
// processed frame goes back into the D3D9 backbuffer and the menu is drawn there. With XeFG chosen at startup
// (FGInput upscaler, FGOutput XeFG) a D3D12 XeFG swapchain takes over the game window, as the D3D11 route does, and
// the D3D9 Present is skipped.
namespace Dx9WithDx12
{
// Returns true when the frame went out through the D3D12 swapchain.
bool Present(IDirect3DDevice9* device, IDirect3DSwapChain9* chain, HWND overrideWindow);
// Releases the default-pool objects, which a device Reset requires, and the XeFG swapchain before exclusive
// fullscreen.
void BeforeReset(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS* params);
} // namespace Dx9WithDx12
