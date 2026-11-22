#pragma once

#include "SysUtils.h"

namespace GLHooks
{
// Hooks the present calls of the real opengl32.dll. Used on the opengl32.dll route.
void Hook(HMODULE opengl32);

// An OpenGL entry point of the real opengl32.dll, from wglGetProcAddress for extensions and the
// module's exports for OpenGL 1.1. Needs a current context for extensions.
PROC GetProc(const char* name);

// True when an OpenGL driver (a module exporting DrvSwapBuffers) is on the calling stack. AMD's driver
// presents a window that covers the monitor through a D3D12 device and DXGI swapchain of its own, made
// inside wglMakeCurrent or on a thread of its own; those are the driver's and are not hooked.
bool CalledFromDriver();

// True once the OpenGL driver made a D3D12 device of its own: it presents the game's window through DXGI.
bool DriverPresentsThroughDxgi();
} // namespace GLHooks
