#pragma once

#include "SysUtils.h"

// The OptiScaler menu over an OpenGL game, drawn through imgui_impl_opengl3 in OptiScaler's own
// context (see GL_Hooks.cpp). Generation identifies that context.
namespace MenuOverlayGl
{
// True when Setup has work to do: first frame, a new window or no renderer backend yet.
bool NeedsContext(HWND hwnd);
// OptiScaler's context is current.
void Setup(HWND hwnd, uint32_t generation);
// Runs the menu for this frame on the CPU; true when there is something to draw.
bool Frame();
// OptiScaler's context is current.
void Draw(uint32_t generation);
} // namespace MenuOverlayGl
