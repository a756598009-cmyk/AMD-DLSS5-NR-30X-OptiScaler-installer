#pragma once

#include "SysUtils.h"
#include <d3d9.h>

// The OptiScaler menu drawn by a D3D9 device over its own backbuffer, before Present.
namespace MenuOverlayDx9
{
void Present(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer, HWND hWnd);
// Releases the default-pool objects a device Reset needs gone. They are rebuilt on the next Present.
void BeforeReset();
// Gives ImGui back when another overlay takes the window or the device goes away.
void Shutdown();
bool IsInited();
} // namespace MenuOverlayDx9
