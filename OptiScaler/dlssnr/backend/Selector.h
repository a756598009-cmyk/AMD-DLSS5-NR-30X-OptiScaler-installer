#pragma once
#include "Kind.h"

namespace DlssNr::Backend
{
Kind RequestedKind();
Kind ActiveKindFromConfig();
// The DLL behind Kind::Lmxxf: Rdna2NrRuntime.dll when NrBackend (at launch) is rdna2 or hexneural, else
// LmxxfNrRuntime.dll.
const wchar_t* LmxxfRuntimeFile();
// True while lmxxf (with LmxxfWired()) or mochizuki is active, or OpenRR was enabled at launch. Mutual exclusion
// vs graphics tracker.
bool SubmissionHooksWanted();
// The runtimes NR without upscaling runs on: danielblnc, and mochizuki between two lists of its own.
bool PresentRuntimeSupported();
// Where mochizuki runs with this game's upscaler ([DlssNr] MochizukiPlacement, as the game started with it). Before SR
// for every other runtime and on anything but a native D3D12 swapchain (the bridges and the Vulkan seam run before SR
// only).
enum class MochizukiPlace
{
    BeforeSr,
    AfterSr,
    Present
};
MochizukiPlace MochizukiPlacementNow();
} // namespace DlssNr::Backend
