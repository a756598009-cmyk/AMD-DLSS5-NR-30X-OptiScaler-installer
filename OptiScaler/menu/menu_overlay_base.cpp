#include "pch.h"
#include "menu_overlay_base.h"
#include "menu_common.h"

#include <Config.h>
#include <State.h>
#include <framegen/IFGFeature_Dx12.h>
#include <menu/input/input_system.h>
#include <Logger.h>
#include <resource.h>

// #include "imgui/imgui.h"
// #include "imgui/imgui_impl_win32.h"

HWND MenuOverlayBase::Handle() { return MenuCommon::Handle(); }

void MenuOverlayBase::Dx11Ready() { MenuCommon::Dx11Inited(); }

void MenuOverlayBase::Dx12Ready() { MenuCommon::Dx12Inited(); }

void MenuOverlayBase::VulkanReady() { MenuCommon::VulkanInited(); }

bool MenuOverlayBase::IsInited() { return MenuCommon::IsInited(); }

bool MenuOverlayBase::IsVisible() { return MenuCommon::IsVisible(); }

void MenuOverlayBase::Init(HWND InHandle, bool isUWP)
{
    if (!Config::Instance()->OverlayMenu.value_or_default())
        return;

    LOG_FUNC();
    MenuCommon::Init(InHandle, isUWP);
}

bool MenuOverlayBase::RenderMenu()
{
    if (!Config::Instance()->OverlayMenu.value_or_default())
        return false;

    return MenuCommon::RenderMenu();
}

void MenuOverlayBase::Present() { MenuCommon::Present(); }

void MenuOverlayBase::Shutdown() { MenuCommon::Shutdown(); }

void MenuOverlayBase::HideMenu() { MenuCommon::HideMenu(); }

void MenuOverlayBase::ApplyThemeStyle() { MenuCommon::ApplyThemeStyle(); }

// Read by tools/dx9-x86's 64-bit host after each Present it makes for a 32-bit D3D9 game, so the
// game side can hold the game's input while the menu is open. Bit 0: menu visible. Bit 1: frame
// generation active. Bit 2: OptiScaler wrapped a swap chain (its DXGI hooks are in place).
extern "C" __declspec(dllexport) uint32_t OptiScalerOverlayState()
{
    uint32_t state = MenuCommon::IsVisible() ? 1u : 0u;
    auto fg = State::Instance().currentFG;
    if (fg != nullptr && fg->IsActive() && !fg->IsPaused())
        state |= 2u;
    if (State::Instance().currentWrappedSwapchain != nullptr)
        state |= 4u;
    return state;
}

// Text typed into the 32-bit game's window while the menu is open, passed on by the same host:
// OptiScaler cannot hook a window procedure in another process, so its text fields get it here.
extern "C" __declspec(dllexport) void OptiScalerAddText(const wchar_t* text, uint32_t count)
{
    if (text != nullptr)
        OptiInput::AddTextInput(text, count);
}
