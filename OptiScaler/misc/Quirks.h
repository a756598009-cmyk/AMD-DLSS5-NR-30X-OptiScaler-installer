#pragma once

#include "SysUtils.h"

#include <flag-set-cpp/flag_set.hpp>

// Game specific hacks that are checked deeper in code
// Per-game config defaults and the exe -> quirk table live in GameConfigs.cpp
enum class GameQuirk : uint64_t
{
    CyberpunkHudlessState,
    FSRFGHudlessMismatchFixup, // Shader extracts UI from swapchain, alpha cut off and apply to hudless
    SkipFsr3Method,
    FastFeatureReset,
    LoadD3D12Manually,
    LoadVulkanManually,
    KernelBaseHooks,
    VulkanDLSSBarrierFixup,
    ForceUnrealEngine,
    NoFSRFGFirstSwapchain,
    FixSlSimulationMarkers,
    HitmanReflexHacks,
    SkipD3D11FeatureLevelElevation,
    CreateD3D12DeviceForLuma,
    ForceCreateD3D12Device,
    ForceDepthD32S8,
    PregmataFixDLSSModes,
    IgnoreValidUntilEvaluateForFG,
    IgnoreTagsWithoutHudlessForFG,
    ForceFGRenderSizeMVs,
    CreateSLOnThe2ndDevice,
    XeFGCameraMotionFill,      // XeFG inputs: velocity has alpha 0 where only the camera moved (The Witcher 3)
    DLSSGFrameLatencyWaitable, // Game waits on the frame latency waitable, SL must not consume it too
    UnlockPathTracingMenu,     // 007 First Light: patch the capability flags that hide Path Tracing / DLSS RR
    MochizukiGameExposureFeed, // mochizuki: feed the game's exposure instead of the content meter (RE9)
    HideFsr4FromGame,          // Leave FSR 4 out of the FFX version list the game sees (BeamNG)
    DisableXeSSInputs,         // Don't answer the game's XeSS calls (Crimson Desert)
    MochizukiPreprocessOn,     // mochizuki pre-process on by default (Crimson Desert)
    // Don't forget to add the new entry to printQuirks
    _
};
