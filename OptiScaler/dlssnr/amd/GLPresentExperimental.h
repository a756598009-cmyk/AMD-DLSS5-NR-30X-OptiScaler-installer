#pragma once

#include "SysUtils.h"

// Final-image NR and XeFG for OpenGL games. Runs in OptiScaler's own context on the game's
// device context (see GL_Hooks.cpp): the back buffer crosses into a D3D12 texture through
// EXT_memory_object_win32, the D3D12 final-image route processes it in place, and it comes back.
namespace AmdGlPresent
{
bool Wanted();
// OptiScaler's context is current; generation identifies it.
void Process(HWND hwnd, uint32_t width, uint32_t height, uint32_t generation);
// After OptiScaler's menu and overlays are drawn on the back buffer: XeFG presents that image with the
// generated frames. OptiScaler's context is current.
void PresentGenerated(HWND hwnd);
} // namespace AmdGlPresent
