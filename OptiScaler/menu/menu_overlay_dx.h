#pragma once

#include "SysUtils.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace MenuOverlayDx
{
ID3D12GraphicsCommandList* MenuCommandList();
void CleanupRenderTarget(bool clearQueue, HWND hWnd);
// While held, Present draws nothing and creates no render targets, so a swapchain can be resized from another thread.
void HoldForResize(bool hold);
void Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
             const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP);
// Final-image NR on a native D3D12 game's frame at the frame generation swapchain's Present, before
// frame generation reads it, and the optical-flow motion and depth for XeFG.
void FinalImageFrameGen(IDXGISwapChain* fgSwapChain);
// The game's present into frame generation is about to start, or has returned, with final-image NR on D3D12.
void FrameGenerationPresent(bool returned);
// Called before the swapchain frame generation presents to is locked: spaces XeFG's own presents while the final image
// generates frames.
void SpaceFrameGenerationPresent(UINT flags);
// From the game's thread, on each frame final-image XeFG generates (D3D12 and D3D11 through D3D12): XeFG's presents are
// spaced for the next quarter second and the pacing hooks only forward, unless XeFG\ExtraPacing=true.
void SpaceFrameGeneration();
void ApplyThemeStyle();
} // namespace MenuOverlayDx
