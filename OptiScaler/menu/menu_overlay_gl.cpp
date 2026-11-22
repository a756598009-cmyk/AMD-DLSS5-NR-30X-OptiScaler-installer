#include "pch.h"

#include "menu_overlay_gl.h"
#include "menu_overlay_base.h"

#include <Logger.h>
#include <State.h>
#include <hooks/GL_Hooks.h>

#include <imgui/imgui_impl_opengl3.h>

typedef const unsigned char*(WINAPI* PFN_glGetString)(unsigned int);

static bool _backend = false;
static uint32_t _generation = 0;
static bool _visible = false;
static bool _loggedDraw = false;
static bool _loggedOpenDraw = false;

// GLSL 1.30 comes with OpenGL 3.0; older contexts get the backend's GLSL 1.20 shaders.
static const char* GlslVersion()
{
    auto getString = (PFN_glGetString) GLHooks::GetProc("glGetString");
    auto version = getString != nullptr ? (const char*) getString(0x1F02) : nullptr; // GL_VERSION
    LOG_INFO("OpenGL menu: context version {}", version != nullptr ? version : "unknown");
    return version != nullptr && atoi(version) >= 3 ? "#version 130" : "#version 120";
}

bool MenuOverlayGl::NeedsContext(HWND hwnd)
{
    if (!Config::Instance()->OverlayMenu.value_or_default())
        return false;

    return !_backend || !MenuOverlayBase::IsInited() || MenuOverlayBase::Handle() != hwnd;
}

void MenuOverlayGl::Setup(HWND hwnd, uint32_t generation)
{
    if (MenuOverlayBase::IsInited() && (MenuOverlayBase::Handle() != hwnd || generation != _generation))
    {
        // Objects of an older OptiScaler context are left to it; only this context's can be deleted.
        if (_backend && generation == _generation)
            ImGui_ImplOpenGL3_Shutdown();

        _backend = false;
        MenuOverlayBase::Shutdown();
    }

    if (!MenuOverlayBase::IsInited())
        MenuOverlayBase::Init(hwnd, false);

    if (!MenuOverlayBase::IsInited() || _backend)
        return;

    _backend = ImGui_ImplOpenGL3_Init(GlslVersion());
    _generation = generation;
    LOG_INFO("OpenGL menu: ImGui renderer {} for window {:X}", _backend ? "ready" : "failed", (size_t) hwnd);
}

bool MenuOverlayGl::Frame()
{
    if (!_backend || !MenuOverlayBase::IsInited())
        return false;


    const bool haveFrame = MenuOverlayBase::RenderMenu();

    if (MenuOverlayBase::IsVisible() != _visible)
    {
        _visible = !_visible;
        LOG_INFO("OpenGL menu: {}", _visible ? "opened" : "closed");
    }

    if (!haveFrame)
        return false;

    ImGui::Render();
    return true;
}

void MenuOverlayGl::Draw(uint32_t generation)
{
    // The backend's objects belong to an older context; Setup rebuilds them next frame.
    if (generation != _generation)
    {
        _backend = false;
        return;
    }

    auto drawData = ImGui::GetDrawData();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplOpenGL3_RenderDrawData(drawData);

    if (drawData != nullptr && drawData->CmdListsCount > 0 && (!_loggedDraw || (_visible && !_loggedOpenDraw)))
    {
        _loggedDraw = true;
        _loggedOpenDraw = _visible;
        LOG_INFO("OpenGL menu: frame drawn{}, {} draw lists, {} vertices", _visible ? " with the menu open" : "",
                 drawData->CmdListsCount, drawData->TotalVtxCount);
    }
}
