#include "pch.h"
#include "menu_overlay_base.h"
#include "menu_overlay_dx9.h"

#include <Config.h>
#include <Logger.h>
#include <misc/Screenshot.h>

#include <imgui/imgui_impl_dx9.h>

// The ImGui backend holds its own reference to this device.
static IDirect3DDevice9* _device = nullptr;
static IDirect3DDevice9* _failedDevice = nullptr;

bool MenuOverlayDx9::IsInited() { return _device != nullptr; }

void MenuOverlayDx9::Shutdown()
{
    if (_device == nullptr)
        return;

    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().BackendRendererUserData != nullptr)
        ImGui_ImplDX9_Shutdown();

    _device = nullptr;
    MenuOverlayBase::Shutdown();
}

void MenuOverlayDx9::BeforeReset()
{
    if (_device != nullptr && ImGui::GetCurrentContext() != nullptr &&
        ImGui::GetIO().BackendRendererUserData != nullptr)
    {
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
}

void MenuOverlayDx9::Present(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer, HWND hWnd)
{
    // The bridge has drawn NR into the back buffer; the menu comes next.
    Screenshot::Dx9(device, backBuffer);

    if (!Config::Instance()->OverlayMenu.value_or_default())
    {
        MenuOverlayBase::Present();
        return;
    }

    if (device == _failedDevice)
        return;

    if (_device != device || MenuOverlayBase::Handle() != hWnd)
    {
        Shutdown();

        if (MenuOverlayBase::IsInited())
            MenuOverlayBase::Shutdown();

        MenuOverlayBase::Init(hWnd, false);

        if (!MenuOverlayBase::IsInited() || ImGui::GetIO().BackendRendererUserData != nullptr ||
            !ImGui_ImplDX9_Init(device))
        {
            LOG_ERROR("D3D9 menu: ImGui initialization failed");
            MenuOverlayBase::Shutdown();
            _failedDevice = device;
            return;
        }

        _device = device;
        LOG_INFO("D3D9 menu ready, window {:X}", (size_t) hWnd);
    }

    ImGui_ImplDX9_NewFrame();

    if (!MenuOverlayBase::RenderMenu())
        return;

    ImGui::Render();

    // The backend saves and restores the fixed-function state it sets. The targets and the states a game may leave
    // in the way (sRGB writes, a colour write mask, texture transforms) are saved here around it.
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)))
        return;

    D3DCAPS9 caps {};
    device->GetDeviceCaps(&caps);
    const DWORD targetCount = std::clamp<DWORD>(caps.NumSimultaneousRTs, 1, 4);
    IDirect3DSurface9* targets[4] {};
    IDirect3DSurface9* depth = nullptr;

    for (DWORD i = 0; i < targetCount; ++i)
        device->GetRenderTarget(i, &targets[i]);

    device->GetDepthStencilSurface(&depth);

    device->SetRenderTarget(0, backBuffer);
    for (DWORD i = 1; i < targetCount; ++i)
        device->SetRenderTarget(i, nullptr);

    device->SetDepthStencilSurface(nullptr);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

    if (SUCCEEDED(device->BeginScene()))
    {
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        device->EndScene();
    }

    for (DWORD i = 0; i < targetCount; ++i)
    {
        if (i == 0 && targets[i] == nullptr)
            continue;

        device->SetRenderTarget(i, targets[i]);

        if (targets[i] != nullptr)
            targets[i]->Release();
    }

    device->SetDepthStencilSurface(depth);

    if (depth != nullptr)
        depth->Release();

    saved->Apply();
    saved->Release();

    // The same frame with the menu on it, when the screenshot asked for that copy.
    Screenshot::Dx9Menu(device, backBuffer);
}
