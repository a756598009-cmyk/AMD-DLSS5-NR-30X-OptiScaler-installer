# Upstream merge, 2026-10-24

Upstream OptiScaler master (`539c9a1ec`, 293 commits since the last merge base `65a5f6f9c`) merged into
OpenNR `dlss-neural-rendering` (`8098b8e71`, release 0.5.7-opennr).

- Worktree: `E:\Projetos\opti_rr_dlss5\wt-upstream-merge`
- Branch: `upstream-merge-2026-10` (local only, not pushed)
- Merge commit: `c5a836c8a` (parents `8098b8e71` ours, `539c9a1ec` upstream)
- The build and test fixes are part of the merge commit; the commit after it only adds this file
- The main checkout (`opti\`) was not touched. Its two uncommitted files (`RuntimeHostLoad.h`,
  `LmxxfBackend.cpp`) are not in this branch.
- Version stays `0.5.7` / `-opennr` (`OptiScaler/resource.h:32`); bump it when this becomes a release.

## How it was done

`git merge refs/upstream-ro/master` with zdiff3 markers: 75 files conflicted (about 290 hunks once split).
Many conflicts came from our 2026-09-22 fork import (`11d11d41f`, burak113's FSR-RR fork and TheAutomatic's
bridge), which carried an older copy of upstream code: where our side was only that older upstream code, I took
upstream's file (Hudfix_Dx11, HudCopy_Vk, Magnifier_Common.h, VulkanwDx12_Hooks, IFeature_VkwDx12 body,
Vulkan_Spoofing, DLSSFeature_Dx11On12, the Streamline plugin hooks). Where our side had NR or OpenNR logic, the
upstream version is the base and our additions were ported onto it.

Submodules: `external/vulkan` moved to `8864cdc` upstream. The worktree has its own shallow clones
(`git submodule update --init --depth 1`; opti's module stores are shallow, so `--reference` could not be used).

## Conflicts and how each was resolved

| File | Resolution |
|---|---|
| `misc/Quirks.h` | Upstream moved the exe table to `misc/GameConfigs.cpp` and turned config-type quirks into `.defaults`. Took upstream's enum, added ours: `UnlockPathTracingMenu`, `MochizukiGameExposureFeed`, `HideFsr4FromGame`, `DisableXeSSInputs`, `MochizukiPreprocessOn` (Quirks.h:34-38). |
| `misc/GameConfigs.cpp` | Our entries added: BeamNG `HideFsr4FromGame` (:146), RE9/RE9 demo `MochizukiGameExposureFeed` (:289, separate entry after upstream's MH Wilds/RE9/Onimusha group), 007 First Light `UnlockPathTracingMenu` (:404), Crimson Desert `DisableXeSSInputs` + `MochizukiPreprocessOn` (:571), eFootball `SkipD3D11FeatureLevelElevation` (:789), Stellar Blade `OnNonNvidia(RestoreComputeSignature)` + `ExtendedStateRestore` (:795, this one came from burak's fork, not upstream). Granblue, Onimusha (full game), pal7, ROTTR, NMS, DD2 were already upstream. |
| `dllmain.cpp` | Upstream's `ApplyGameConfigs`; our `DisableXeSSInputs` -> `EnableXeSSInputs=false` (:1468) and the mochizuki quirk defaults (:1475) run right after it. printQuirks has both sets. CEF `--type=` passthru kept (:1595). |
| `Config.h/.cpp`, `OptiScaler.ini` | Both key sets kept. Our XeFG DMFG keys dropped in favour of upstream's identical ones (upstream adds `TargetAsCapDMFG`, `OverrideInterpolationCount`); our `UnlockMFG`, `MaxInterpolatedFrames`, `ExtraPacing`, `DisableReflexSync`, menu `Width/Height/Anchor/SectionOrder` kept; upstream's `DisableOTA`, `Fsr4GpuSupport`, `Fsr4Model` etc. added. `value_as_chosen()` intact (Config.h:128). `OverlayMenu` is upstream's SoftDefault. |
| `proxies/FfxApi_Proxy.h` | Kept our (burak's) proxy - it carries the FSR-RR denoiser routing (`DenoiserApiGenerationDx12`, `VersionImplemented_RR`, effect-ID version query) - and applied upstream's 6 commits onto it: FSR4 driver provider routing in Create/Destroy/Configure/Dispatch, the Query split (`D3D12_QueryUpscalerVersions`, `D3D12_QuerySdk`), `IsUpscalerModule`/`UpscalerModule`, `FSR4Presets`/`FSR4Models` hooks replacing `FSR4ModelSelection`, upstream's InitFfxVk (539c9a1ec, hooked side like Dx12, pinning, `amd_fidelityfx_upscaler_vk.dll`). Our BeamNG/Crimson fix that also hooks the game's own loader is kept (:427). |
| `inputs/FfxApi_Dx12.cpp`, `inputs/XeSS_Common.cpp`, `proxies/XeSS_Proxy.h` | No conflicts; `QueryVersionsWithoutFsr4` (FfxApi_Dx12.cpp:401) now sits on top of upstream's version list (driver FSR4 names start with 4 and are filtered too). `hk_xessSetMaxResponsiveMaskValue` intact (XeSS_Proxy.h:479, XeSS_Common.cpp:163). |
| `fsr4/` | We had no local changes there; upstream's FSR4DriverProvider/FSR4Models/FSR4Presets replace FSR4ModelSelection/FSR4Upgrade. `Fsr4ForceModel` is migrated to `Fsr4GpuSupport` by upstream's Config code. |
| `hooks/D3D12_Hooks.cpp` | Upstream rewrote root-state tracking (per-list `CommandListState`, templated restore). Took upstream's file and ported every OpenNR addition onto it: includes, the AMD graphics tracker hooks (RS/IA/OM/predication/ClearState/Draw/RenderPass/Bundle/ExecuteIndirect/queries/Release/CreateCommandList(1)/CreateCommandSignature), tracker reports in every early hook, in the three late hooks and in the restore path, `AmdGraphicsTrackerWanted()` (:341) widening the attach conditions, the lmxxf `ArmCreate` + early-list proxy block (:3052), `NoteDeviceCaller`, the OpenGL driver passthrough, exposure scan on committed/placed resources (:2589), `IsRootSignatureTrackingEnabled`/`NativeDrawHookTarget`/`IsAmdGraphicsTrackerArmed` (:1890-1892). Change: our separate Reset detour is gone; upstream's `hkReset` now also reports `OnReset` and is attached when the tracker is wanted (two detours on one target in one transaction was a risk). |
| `hooks/Vulkan_Hooks.cpp` | Upstream added Vulkan FG (swapchain hooks, FG feature on device create, low latency 2, surface->window). Ported ours onto it: `IsForeignCreate` passthrough (:407/:517), NR device extensions (:678), `_nrGraphicsFamily` capture, `AmdVkPresent::Created/Destroyed` (:1160/:1179), the transfer-usage `requested` create info is now the start of upstream's create-info chain, OverlayMenu gating of the menu. |
| `hooks/Streamline_Hooks.cpp/.h`, `proxies/Streamline_Proxy.h` | Upstream rewrote plugin hooking into per-copy slots; took upstream's plugin section and re-added our `externalFrameGeneration` guards (Streamline_Hooks.cpp:3168...) and `isSetConstantsHooked`. Our RR tag diagnostics, `RecordGameDlssgOptions`, `GameRequestedInterpolationCount`, MFG unlock, local sl.dlss_g spoof logging kept. `dummy_slDLSSGGetState` forwards to the real Streamline (NeuralHub) first, then upstream's `FillDlssgInputState`. Our sl.dlss_g preload for NeuralHub kept (Streamline_Proxy.h:106). Removed our `applyMenuDlssgInterlock`: upstream fixed the same Vulkan menu/DLSSG deadlock by drawing on the presenting queue (01195e9bf). |
| `framegen/xefg/*`, `framegen/dlssg/*` | Upstream's Dynamic MFG (same lineage as our TheAutomatic port, newer) replaces ours: `XeFG_DynamicCount.h` and the inline count logic are upstream's. Kept ours: game-requested count via raw NGX and the NR-without-upscaling default of 2 generated frames when the count is auto and not following the game (XeFG_Dx12.cpp:775), D3D9 presenter handling, NeuralHub DMFG guard moved into upstream's `DLSSG_Common::SetOptions` (DLSSG_Common.cpp:110). |
| `NVNGX_Parameter.cpp` | Our XeFG/NeuralHub `MultiFrameCountMax` chain plus upstream's fallback to the real driver table. |
| `nvapi/NvApiHooks.cpp`, `low_latency/*`, fakenvapi | Upstream "lobotomised" fakenvapi (low latency moved to `InputCommon`); our sleep timing for Dynamic MFG is upstream's `take_sleep_ms` now. `DisableReflexSync` keeps precedence over upstream's ReflexSync hook (NvApiHooks.cpp:211). Our stalled-markers AntiLag 2 fix combined with upstream's `interpolated` flag (ll_antilag2.cpp:199). |
| `menu/menu_common.cpp/.h` | Our side-bar layout (NR/UP/FG/IMG/UI/ADV, docking, pills, NR badge, graphs, bottom bar, Neural page at :10934) kept as the skeleton. Upstream's new section functions are called from it: Output Scaling, Shader times, Init Flags, V-Sync, Status. Settings functions are upstream's where ours only moved code (image/ratio/output scaling/init flags); RenderActiveUpscalerSettings is upstream's plus our FSR-RR block and DLSSD->FSR-RR switching (:3273); FG selection/runtime are upstream's plus our external-FG box, NeuralHub DLSS-FG, child-window XeFG on Vulkan/OpenGL, D3D9 route, NR-without-upscaling output rules, RTX 40 MFG unlock, Extra Pacing, Only FG, RTSS warning; the FPS overlay is upstream's line model plus our NR runtime/time line (:2923); theme is upstream's swatches plus our docking combo. `ShowHelpMarker` restored (upstream removed it, our NR menu uses it). Upstream's card/Custom-tab layout functions compile but are not reachable from our layout. |
| `menu/menu_overlay_vk.cpp`, `_dx.cpp` | Upstream's menu blur and present-queue selection + ours: screenshot copies, vkd3d-proton DX overlay ownership (now via `IdentifyGpu::gameUsesVkd3dProton/Dxvk`), objects-usable guard, final-image menu hold. |
| `resource_tracking/ResTrack_dx12.*` | Upstream reworked tracking (release notifications instead of a Release hook, binding shards). Took upstream's file plus our exposure scan hook (:692). The FSR-RR resource inspector (burak's debug view) did not survive: its API is stubbed (:2873), the menu and FSRD compile and report nothing. |
| `upscalers/*` | NR seams intact: NVNGX_DLSS_Dx12.cpp:1287/1315/1332/1362, NVNGX_DLSS_Vk.cpp:1056/1135, IFeature_Dx11wDx12.cpp:496, IFeature_VkwDx12.cpp:2201. Upstream's per-feature D3D11 bridge resource cache adopted. Upstream's `DLSSFeatureDx11On12` and `DLSSFeatureVkOnDx12` replace our two DLSS-on-12 variants (ours removed); `NeuralHubArmOk` still lets DLSS through on AMD (FeatureProvider_Dx11/Vk/Dx12). `FSR31Feature_Dx12` (base of FSR-RR) moved to `state.ffxUpscalerVersionsDx12`. |
| `shaders/Shader_Dx11.cpp` | Kept ours: the destructor does not release `_currentOutResource` (InitializeUAV never AddRefs it; upstream's release is an over-release, caught by `tests/shader_dx11_ownership.cpp`). |
| `shaders/Shader_Dx12.cpp` | Upstream's `translateTypeless` plus our depth-plane SRV exception. |
| `with_dx12/*`, `wrapped/wrapped_swapchain.cpp`, `hooks/FG_Hooks.cpp`, `hooks/DxgiFactory_Hooks.cpp` | Union: upstream's XeFG present/resize synchronisation + our final-image resize hold, present fallback, D3D9 queue fallback, window-sized swapchain resolve, NeuralHub present hooks, colour-space log. Upstream commented out the XeFG back-buffer release loops in ResizeBuffers; followed it. |
| `OptiScaler.vcxproj(.filters)`, `.sln` | Union, then removed duplicates and entries for files that no longer exist. |
| `.github/workflows/build.yml` | Stays deleted (we removed the workflows). |

Double insertions found after the merge (both sides added the same block, git kept both) and removed:
FSRFG_Dx12.cpp callback-device block, `IFGFeature::_generationOff`, `State::lastFGPresentMs`, two menu
declarations, menu local redeclarations.

## What upstream brings

- FSR 4: driver DLL provider (amdxcffx64 / amdvkffx64) with live model switching from the menu, FSR 4 models
  FP8 / INT8 / INT8 RDNA2 / INT8 Lite (`[FSR] Fsr4Model`, `Fsr4GpuSupport` override), 4.1.2 model selection,
  FSR 4 upscaling and FSR 4 FG on Vulkan, GPU detection for FSR 4 availability (d8aebe45b, 65ee5d64c), live
  watermark toggle, its own auto exposure (0958d8e18).
- Vulkan frame generation: OptiFG (upscaler input), FSR FG output, DLSSG input and output, Vulkan hudfix,
  reprojection (WIP, needs DLSSG via Streamline), Vulkan GPU times per shader, Vulkan reflex/low latency.
- XeFG input (game's own XeSS-FG), Dynamic MFG for XeFG with "use target as cap", DLSSG follows the game's
  multiplier by default, FG mode slider.
- `misc/GameConfigs.cpp` per-game defaults, DRS init flag and passthrough, AMD flip-meter status and toggle,
  menu blur, card menu, menu height, DLSS Dx11 w/Dx12 and DLSS Vk w/Dx12, DeferredRelease, BeamNG quirk
  (6fd357379), DA sharpness crash fix (18d4d2e48), Enshrouded MVs, many crash fixes.

## INI keys for the installer template

`tools\PACKAGE_RELEASE.ps1` copies `OptiScaler.ini` whole and rewrites a few keys plus the `[DlssNr]` block, so
the new keys ship as `auto` without changes. Keys worth a decision:

- `[FSR] Fsr4Model` (0 FP8, 1 INT8, 2 INT8 RDNA2, 3 INT8 Lite) and `Fsr4GpuSupport` (0 none, 1 FP8, 2 INT8,
  3 INT8-2, 4 INT8 Lite). `Fsr4ForceModel` is gone (read once as `Fsr4GpuSupport`, then deleted on save).
  Leave auto for RX 9000; the installer's RX 7000/6000 paths could set `Fsr4GpuSupport=2`/`3` if we want FSR 4
  INT8 there.
- `[FSRFG] DisableFlipMetering`, `[Menu] Blur`, `BlurStrength`, `Height`, `FpsOverlayShowRatio`,
  `[OptiFG] HUDFixUITexture`, `HUDFixUIUnblend`, `HUDFixPersistentBindings`, `[NvApi] DisableOTA`,
  `[InitFlags] DynamicResolution`, `[XeFG] TargetAsCapDMFG`, `OverrideInterpolationCount`,
  `[fakenvapi] LowLatencyInput/LowLatencyOutput`: auto is fine.
- Our `[Menu] Width/Height/Anchor/SectionOrder` stay ours; upstream's `Height` comment was dropped in favour of
  ours (pixels, not unscaled).
- Payload check: FSR 4 on Vulkan through the driver needs a signed FFX SDK Vulkan DLL loaded
  (`amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll` or `amd_fidelityfx_framegeneration_vk.dll`,
  FSR4DriverProviderVk.cpp:300). The 0.5.7 package only has `amd_fidelityfx_vk.dll`. Add the SDK 2.x Vulkan DLLs
  to `OptiScaler\` in the payload if Vulkan FSR 4 is wanted.

## Build and tests

- `tools\build-optiscaler-fast.cmd`: BUILD_OK (`exports\release-local\OptiScaler.dll`, 28.6 MB). No new
  warnings beyond C4250 on FSR31/FSRD. Runtimes were not rebuilt: nothing under `OptiScaler/dlssnr` or the
  runtime sources changed in the merge.
- Headless, all PASS: `test-proxy-link` (against the 0.4.9 baseline), `test-amd-host-contracts`,
  `test-shader-compatibility` (after adding `DeferredRelease.cpp` to `tests/shader_dx12_srv.cpp`, which upstream's
  Shader_Dx12 now needs), `test-lmxxf-list-split`, `-create-execute`, `-evaluate-cut`, `-interposer-wrap`,
  `-list1-wrap`, `-same-frame-boundary`, `-color-probe`, `test-person`.
- On the RX 9070 XT, built DLL against the 0.5.7 release DLL (same staging, `exports\` of the worktree only):
  - `dx12_final_image_smoke nr` (danielblnc): all NR/present checks pass; "Model scale changed from the menu"
    fails, and fails the same way with the 0.5.7 DLL (the test's click position is stale). A first run also
    had 2 runtime timeouts and NR missing frames in two phases; the rerun was clean.
  - `dx12_final_image_smoke fg` (danielblnc + XeFG 3X): everything passes except the same model-scale click.
  - `dx12_final_image_smoke nr --set DlssNr/NrBackend=mochizuki`: same results as 0.5.7 (the "NR changed
    every probed frame" threshold is danielblnc's; 0.5.7 fails 4 checks, merged 3).
  - `test-dx11-final-image ... nr`: PASS on the second run (first run: NR skipped frames on the menu's
    Frame Gen page once, with a 142 ms hitch; not reproduced).
  - `test-vulkan-present`: core checks pass (NR through the shared D3D12 image, XeFG 3X, keys, menu over XeFG,
    real frame rate kept); 5 failures vs 11 for the 0.5.7 DLL in the same session, all click/focus checks,
    with "the test window cannot take the foreground (locked or busy desktop)" in both.
- Not run: anything writing to game folders, `tools\uishot`, D3D9/GL/x86 smoke tests.
- Not done: clang-format 20 (only 22 is installed here); a CI format check may want a pass on the edited files.

## Risks

1. D3D12_Hooks port: the tracker reports now sit in upstream's new hooks; `Reset` is one detour (`hkReset`) for
   both root-state recording and the tracker. danielblnc NR in real D3D12 games is the first thing to test.
   lmxxf/mochizuki same-frame path (`ArmCreate`, early proxies) was moved verbatim.
2. Vulkan: upstream's own FG hooks the swapchain now. Our final-image path (AmdVkPresent) and child-window XeFG
   have not met upstream's Vulkan FG in a game. The FG output rules: OpenGL keeps XeFG only through NR without
   upscaling; Vulkan allows upstream's OptiFG/FSR FG/DLSSG outputs, and the child-window XeFG when NR without
   upscaling runs (DLSSG then disabled).
3. Menu: upstream's card/Custom tab layout is not reachable; the MFG menu is upstream's slider, so there is no
   "Auto"/"Custom..." entry for the XeFG count any more (the INI's `auto` still works, including the
   NR-without-upscaling default of 3X).
4. FSR-RR resource inspector is stubbed (debug view only; FSR-RR itself is untouched).
5. FfxApi proxy keeps burak's structure with upstream's FSR 4 driver provider grafted in. The FSR 4 driver
   versions now appear in the game's FFX version list (`WhenNewestUpscaler`), filtered for BeamNG by
   `HideFsr4FromGame`.
6. Dynamic MFG is upstream's implementation (no XeFG 3 version gate; no automatic fallback when
   `SetNumInterpolatedFrames` fails).
7. Removed menu/DLSSG interlock: relies on upstream's presenting-queue fix for DLSSG on Vulkan.
8. ResTrack: hudless candidates take upstream's `CheckResource` info (our view-format override for typed views
   of typeless resources is gone).

## In-game test list

1. Cyberpunk 2077 (DX12): NR before SR with FSR 3.1/FSR 4 (FFX) on danielblnc, lmxxf and mochizuki; FSR-RR
   with NR between denoise and upscale; XeFG and FSR FG on top; menu Neural page; PresentWithoutUpscaler on;
   RE frame rate and stalls with AmdGraphicsWait=1 (tracker) and with lmxxf (submission hooks); check the log
   for "AMD graphics tracker hooks attached" or "lmxxf ArmCreate ok".
2. Cyberpunk: FSR 4 model switching in the menu (FP8/INT8) on the RX 9070 XT, driver FSR 4 (amdxcffx64) entry
   in the FFX upscaler list, watermark toggle.
3. BeamNG.drive (DX12 and Vulkan): graphics options page draws (HideFsr4FromGame), FSR 3/FSR 4 inputs reach
   OptiScaler (log "Hooked the game's own FFX module too" on DX12), XeSS selection does not crash
   (xessSetMaxResponsiveMaskValue), menu input, CEF children in passthru (one OptiScaler.log).
4. Crimson Desert: FSR 4.1 input with NR, XeSS inputs off by default (quirk), mochizuki pre-process on.
5. Resident Evil Requiem: mochizuki exposure steady in dark scenes (game exposure feed), RestoreComputeSignature.
6. A Vulkan game (e.g. Doom: The Dark Ages / No Man's Sky / Detroit) on the RX 9070 XT: FSR 4 upscaling through
   the driver (needs the Vulkan SDK DLLs, see above), upstream's OptiFG/FSR FG on Vulkan, mochizuki/danielblnc
   final image with child-window XeFG, menu over FG output.
7. WrongGPU with the fork: DLSS 4/4.5 SR on AMD; check DXGI spoofing keeps `neuralhub_host.dll`/`dlssnr_amd_*`
   unspoofed (Dxgi_Spoofing.cpp:33) and FidelityFX override paths load beside the DLL.
8. NeuralHub 0.8.0 host: DLSS-SR/RR on AMD, DLSS-FG through OptiScaler's Streamline, Dynamic MFG request falls
   back to a fixed multiplier.
9. A D3D11 game (Assetto Corsa or a Unity title) with NR on the D3D11 bridge and DLSS on12; Witcher 3 DRS
   (upstream's XeFGCameraMotionFill quirk is new there).

## Fix after the first Cyberpunk test (2026-10-25): NeuralHub DLSS-FG did nothing

Symptom: the game's DLSS-FG on, no generated frames; the NeuralHub host log has no `NGX CreateFeature DLSS-FG`
and OptiScaler.log has no `DLSSG options:` line. The INI had `FGOutput=DLSSG` with `FGInput=auto` (NoFG).

Cause: `hkslDLSSGSetOptions` (hooks/Streamline_Hooks.cpp:2354) forces the game's DLSS-G mode to `eOff` whenever the
output is DLSSG and the input is not DLSSG, to make room for OptiScaler's own DLSSG instance. With no FG input that
instance never exists (`FGHooks::CheckForFGStatus` returns false: "Can't init FG Feature"), so sl.dlss_g never
created the NGX feature. The same check is in 0.5.7 (`e42f3dc68`, older than the fork's merge base); 0.5.7 only
looked fine because an FG output picked in the menu applies after a restart, and the next start reads the saved
`FGOutput=DLSSG`. The game's DLSS-FG on NeuralHub (`useNativeDlssg`) needs the output at auto/None or DLSSG with
no input.

Fix: the game's DLSS-G is turned off only when OptiScaler has an FG input of its own (not NoFG, not DLSSG).
Anti-Lag 2 stalled-marker fix checked: present (ll_antilag2.cpp:196, combined with upstream's `interpolated`).

Re-test: Cyberpunk with `FGOutput=DLSSG` and with `FGOutput=auto`, game's DLSS-FG on: host log shows
`NGX CreateFeature DLSS-FG`, OptiScaler.log shows `DLSSG options: requested=1 forwarded=1`.

## Second Cyberpunk test (2026-10-25): DLSS-FG multiplier override missing, 3X unstable

### Override DLSSG Ratio missing from the menu

The menu code is unchanged from 0.5.7 (`Override DLSSG Ratio`, `Force Dynamic MFG`, same `[DLSSG]
OverrideInterpolationCount` / `OverrideForceDMFG` / `FramerateTargetDMFG` keys, menu_common.cpp:6253). It shows only
when OptiScaler has no DLSSG instance of its own (`!dlssgInputOrOutput`). The test INI has `FGOutput=DLSSG` with no FG
input, so `activeFgOutput` was DLSSG and the section was hidden. In 0.5.7 that INI turned the game's DLSS-FG off
(see the previous section), so the working 0.5.7 setup was output auto/None, where the section shows.

Fix: DLSSG output without an FG input is now run as no output (dllmain.cpp, after "Ensure valid FG configuration";
log line `FG Output DLSSG without an FG Input, the game's DLSSG runs as with no output`). That is exactly 0.5.7's
working path for the game's DLSS-FG, and it also keeps the DLSSG-output-only code away from the game's own DLSS-G
(ALLOW_TEARING forced on ResizeBuffers from b48fcb0dc, the local sl.reflex/sl.pcl handling, NvApi DRS overrides).
The menu's "restart to apply" check maps the same case to no output.

### DLSS-FG 3X unstable

0.5.7 built without `LOW_LATENCY_INPUTS`: fakenvapi drove Anti-Lag 2 and force-disabled low latency once DLSSG
generated more than 1 frame (Reflex_Hooks `_FgNumFramesToGenerate > 1`, fakenvapi did not create AL2 then).
Upstream removed that path (a79c2f34b, 5dfedd683) and e1351b397 keeps AL2 on with MFG, typing frames by counting
presents in the wrapped swapchain (`InputCommon::dlssg_present`). The test log has `LowLatency algo: AntiLag2` and no
`Multi frame generation, disabling` line during the 3X stretch: AL2 stayed on, which 0.5.7 never did at 3X. Streamline
logs `Frame rate over 100.00ms` and skipped presents there.

Fix: `InputCommon::update` (input_common.cpp) treats AL2 like LatencyFlex/XeLL when NeuralHub serves DLSS-FG
(`NeuralHubFgServed()`, cached once the count goes above 1): low latency is force-disabled at 3X and up, as in 0.5.7.
Upstream's AL2 MFG typing stays for every other case; 2X is unchanged.

Build OK, headless tests all PASS (proxy-link vs 0.4.9, amd-host-contracts, shader-compatibility, lmxxf
list-split/create-execute/evaluate-cut/interposer-wrap/list1-wrap/same-frame-boundary/color-probe, person).

Re-test in Cyberpunk (keep `FGOutput=DLSSG`, then once with `FGOutput=auto`):
1. OptiScaler.log has the `FG Output DLSSG without an FG Input` line; host log has `CreateFeature DLSS frame generation`.
2. Frame Gen page shows `Override DLSSG Ratio` (Default/Off/2X/3X/4X) and `Force Dynamic MFG`; 3X from the menu gives
   `DLSSG options: ... generatedFrames=2`.
3. Game at 3X: OptiScaler.log shows `Multi frame generation, disabling AntiLag2` and `Low latency Force State:
   ForceDisable`; play a minute and compare smoothness with 2X. If 3X is still unstable, send OptiScaler.log and
   the host log (count of `Frame rate over 100.00ms`).

## Third Cyberpunk test (2026-10-25): 3X still stutters, LatencyFlex at 3X as in 0.5.7

The previous fix force-disabled AntiLag 2 at 3X, but the merge's log still had 125 `Frame rate over 100.00ms`
against 21 for 0.5.7 in the same scene.

What 0.5.7 really did at 3X (nvapi/fakenvapi/low_latency_d3d.cpp, hooks/Reflex_Hooks.cpp:636-657 in 0.5.7): a
change of `_FgNumFramesToGenerate` set `fakenvapiReloadLowLatency`, and the rebuilt tech skipped AntiLag 2 and XeLL
when `dlssgDetectedInterpolationCount > 1` or Dynamic, so the output became LatencyFlex (0.5.7 log 15:03:48,
`LowLatency algo: LatencyFlex` right after `generatedFrames=2`). The same block set Force State to ForceDisable, and
`LatencyFlex::is_enabled()` honours it, so LatencyFlex never slept at 3X. 0.5.7's 3X pacing is therefore "no
low-latency sleep and no AntiLag 2 context", not LatencyFlex pacing. The merge kept the AntiLag 2 context alive and
only disabled it: it still sends `MarkEndOfFrameRendering` on every present marker and `SetFrameGenFrameType` on the
out-of-band present markers to the driver (ll_antilag2.cpp), and `dlssg_present` keeps typing presents for it.

Fix (low_latency/input/input_common.cpp, merge's own low-latency layer, no fakenvapi code restored; the game's
Reflex markers already reach it through InputReflex):
- `InputCommon::neuralhub_multi_frame()`: the game's DLSS-FG at 3X and up or Dynamic, and
  `DlssNr::AmdBridge::NeuralHubFgServed()` (checked once, when it first matters).
- `update_low_latency_tech`: when the output would be AntiLag 2 (Auto or picked, D3D, not a forced output, not
  OptiScaler's own request) and that holds, the output is LatencyFlex. Leaving AntiLag 2 goes through the existing
  `delay_deinit` (50 calls); going back to AntiLag 2 at 2X/off is immediate. Log:
  `NeuralHub DLSS-FG at 3X: LatencyFlex`, and `NeuralHub DLSS-FG at 2X: AntiLag2` (or `at off`) on the way back.
- `update`: the existing multi-frame rule still sets ForceDisable, as 0.5.7 did, so LatencyFlex doesn't sleep
  (upstream and 0.5.7 both say it falls apart with more than 1 generated frame). New: at 2X/off the Force State it
  had before goes back (0.5.7 left ForceDisable, which kept AntiLag 2 off at 2X after a 3X stretch). ForceEnable in
  the INI still wins and lets LatencyFlex sleep at 3X.
- Every other case (no NeuralHub, other FG outputs, Vulkan, XeFG, Reflex/FPS Limiter outputs) is unchanged.

Build OK, headless all PASS (proxy-link vs 0.4.9, amd-host-contracts, shader-compatibility, the seven lmxxf tests,
person).

Re-test in Cyberpunk (same scene and INI, `FGOutput=auto`):
1. 2X: `LowLatency algo: AntiLag2`, as before.
2. Switch to 3X: `Multi frame generation, disabling AntiLag2`, `Low latency Force State: ForceDisable`,
   `NeuralHub DLSS-FG at 3X: LatencyFlex`, then `FSR Anti-Lag 2.0 DX12 deinitialized` and `LowLatency algo:
   LatencyFlex`. Play a minute; count `Frame rate over 100.00ms` (0.5.7: 21).
3. Back to 2X: `NeuralHub DLSS-FG at 2X: AntiLag2`, `LowLatency algo: AntiLag2`, Force State back to InGame.
If 3X is still worse than 0.5.7, the remaining differences are outside the output (Streamline's reflex/pcl handling,
upstream's present path); send both logs.

## In-game results and open tasks (2026-10-25, written by the main session)
- OK: RDR2 DX12 + mochizuki; Cyberpunk lmxxf, mochizuki, danielblnc DLSS-FG at 2x/3x (after f9ebeac92, 6870c2183, b5aba4cfe). danielblnc NR stays off while its host is on D3D12 for FG (as in 0.5.7). 4x small hitches = NeuralHub.
- Open: (1) RDR2 Vulkan: FSR 2 DLLs hooked as DX12 -> amdvlk crash; UseFsr2VulkanInputs only hooks exe exports. (2) installer: ship a 310.x nvngx_dlss.dll for danielblnc 0.8.0 when the game's is older (RDR2 2.2.10 -> NeuralHub FSR fallback throws). (3) WrongGPU crashes RDR2 DX12 at start (D3D12Core.dll+0x2483e2). (4) OptiScaler's own FG in a game whose DLSS-FG already runs through the host stacks two FGs (black bands, hitches): block/hide it.
- Not yet tested: BeamNG, Crimson Desert, RE9, a Vulkan game with FSR 4, D3D11.

## FSR 2 DLLs on Vulkan, and OptiScaler's FG over the game's DLSS-FG (2026-10-25)

### RDR2 Vulkan: FSR 2 DLLs hooked as D3D12

Cause: `ffx_fsr2_api_x64.dll` (the generic FSR 2 API, shared by both backends) was always hooked with the D3D12
handlers. With a Vulkan context `ffxFsr2ContextCreate_Dx12` did not find the VkDevice among the D3D12 devices, took
the last D3D12 device instead (the Vk w/Dx12 interop one), built a D3D12 feature, and the dispatch then handed a
VkCommandBuffer to D3D12 (amdvlk64 crash). `UseFsr2VulkanInputs=true` only looked for exports in the exe.

Fix (general, any game with the FSR 2 DLLs on Vulkan; no game check):
- `ffx_fsr2_api_vk_x64.dll` is hooked too (dllmain and LoadLibrary, name vector `fsr2VkBE`):
  `ffxFsr2GetInterfaceVK` keeps the VkPhysicalDevice and the `fpCreateBackendContext` it hands out,
  `ffxFsr2GetScratchMemorySizeVK` the physical device.
- The handler set is picked per context at `ffxFsr2ContextCreate`: `FSR2Vk_IsVkDescription` compares the
  description's `callbacks.fpCreateBackendContext` with the Vulkan backend's (fallback: the module holding the callback
  exports `ffxFsr2GetInterfaceVK` and not `ffxFsr2GetInterfaceDX12`). Vulkan contexts go to the FSR2_Vk handlers
  (now taking the original function as an argument, so the exe path and the DLL path share them); dispatch and
  destroy follow the context. The pattern-hook variants pass Vulkan contexts through.
- If OptiScaler can't take the Vulkan context (no physical device, NGX init fails) the game's FSR 2 runs: create
  returns the game's result and dispatch passes through for contexts that are not ours (also fixes a null
  dereference in the exe Vk and pattern D3D12 dispatches).
- RDR2's GameConfigs quirk and d9b621109 untouched. D3D12 RDR2 is unchanged (its callbacks live in the dx12 DLL).

Log to look for: `Trying to hook the FSR2 Vulkan backend`, `ffxFsr2GetInterfaceVK: <addr>`, then on FSR 2 on
`FSR 2 context on the Vulkan backend, using the Vulkan inputs` and `ContextCreate_Vk context created`.

### OptiScaler's own DLSSG output stacked on the game's DLSS-FG (Cyberpunk, NeuralHub host)

With `FGInput=DLSSG`, `FGOutput=DLSSG` and NeuralHub serving DLSS-FG, `hkslInit` keeps the game's DLSS-G
(`useNativeDlssg`) and `hkslDLSSGSetOptions` forwards the game's mode to it, while OptiScaler's DLSSG output (its own
Streamline copy) generated on top whenever the menu's Frame Gen was Default or 2X+.

Fix:
- `State::gameDlssgNative` is set from `useNativeDlssg` in `hkslInit`; `State::GameDlssgBlocksOwnFG()` = that, DLSSG
  output, and FG input DLSSG (or none). `Config::IsFGEnabled()` returns false then, so our DLSSG instance stays
  deactivated whatever the menu or the INI say (log `hkslInit: the game's DLSS-FG runs on NeuralHub, OptiScaler's
  DLSSG output stays off`).
- Frame Gen page, DLSSG card: "Game's DLSS-FG: ON 3x / OFF" from the forwarded options, the line "The game's
  DLSS-FG is running; set it in the game", and the Frame Gen slider/Force Dynamic MFG disabled. Override DLSSG Ratio
  and the game's Force Dynamic MFG show (same `hkslDLSSGSetOptions` path as with no output). The FG hotkey drives the
  game's DLSS-FG through the ratio override, as with the Nvngx FG input.
- FPS overlay: `(Game DLSS-FG x3)` / `(Game DLSS-FG off)` from `dlssgLastSetMode` and the new `dlssgLastSetFrames`
  (the forwarded `numFramesToGenerate`).
- Without NeuralHub (game's DLSS-G removed, our DLSSG output) nothing changes.

Build OK, headless all PASS (proxy-link vs 0.4.9, amd-host-contracts, shader-compatibility, the seven lmxxf tests,
person).

Re-test:
1. RDR2, `system.xml` API Vulkan, INI defaults (EnableFsr2Inputs/UseFsr2VulkanInputs auto), FSR 2 on in the game:
   no crash; OptiScaler.log has the lines above; the overlay shows `VLK` and `FSR 2.X -> <OptiScaler upscaler>`;
   NR on (mochizuki, then danielblnc) runs. Play a few minutes, change FSR 2 quality once (context recreate).
2. RDR2 DX12: still the D3D12 handlers (`ffxFsr2ContextCreate_Dx12 context created`), as in the OK run.
3. Cyberpunk, `FGInput=DLSSG`, `FGOutput=DLSSG`, game's DLSS-FG on at 2X then 3X: one FG only (no black bands or
   freezes); Frame Gen page shows the line and the disabled slider; overlay says `Game DLSS-FG x2`/`x3`; Override
   DLSSG Ratio changes the multiplier; FG hotkey toggles the game's FG off/on.

## RDR2 Vulkan FSR 2: pause menu crash and the format-0 depth (2026-10-25)

Test of 613983a4 (hash matches the installed winmm.dll): FSR 2 on Vulkan works, Esc crashes, log has
`ffxApiGetVkFormat Unknown FFX surface format: 0` every frame.

Crash (dumps RDR2.exe.3760 and .24508 with the 613983a4 PDB, copy kept in the scratchpad): bucket
`HEAP_CORRUPTION ... BlockNotBusy_DOUBLE_FREE ... amdvlk64`, stack `RtlFreeHeap <- amdvlk64 <-
ContextDispatch_Vk <- FSR2Vk_ContextDispatch <- ffxFsr2ContextDispatch_Dx12 <- RDR2`. The calls are the
"clear last frame's views" block: 3760 at `vkDestroyImageView(colorImageView)`, 24508 at
`vkDestroyImageView(biasImageView)` (the reactive view). That block destroyed the static views but never reset
them. When a mask is missing for a frame (the pause menu stops sending reactive), its view is not recreated, so the
next frame destroys the same handle again; once amdvlk has handed the freed block to a new view (the color one),
the stale destroy frees that view and its own destroy is the double free. The NGX params also kept pointing at the
freed reactive/exposure resources. Same pattern in FfxApi_Vk.cpp.

Format 0: the dispatch log shows `depth: vk: 0 ffx: 0`. RDR2's FSR 2 Vulkan backend describes its depth as
FFX_SURFACE_FORMAT_UNKNOWN (its VkFormat -> FFX table has no entry for it), so the view was created with
VK_FORMAT_UNDEFINED. That's also why `IFeature_VkwDx12::ProcessVulkanTextures Depth not exist!!` showed up: that
feature was the menu's pick ("FSR 3.X/4 w/Dx12", 18:32:04); the ~10 re-creations in that log are upscaler changes
from the menu (w/Dx12, FSR 2.2.1, FSR 4 FP8/INT8/INT8 Lite), not the game. Routing Vulkan FSR 2 inputs to the
Vulkan provider, which offers w/Dx12 upscalers, is intended.

Fix (FSR2_Vk.cpp, FfxApi_Vk.cpp):
- Last frame's views go through `DeferredRelease` (they can still be in flight) and are reset to null; the
  optional params (exposure, bias mask, `FSR.reactive`, `FSR.transparencyAndComposition`) are cleared every
  dispatch before they are set again.
- FSR2_Vk.cpp no longer overwrites `FSR.reactive`/`FSR.transparencyAndComposition` with the raw VkImage after
  setting them to the NGX resource; FFXFeature_Vk reads them as `NVSDK_NGX_Resource_VK*`.
- `ffxGetTextureResourceVK` of ffx_fsr2_api_vk_x64 is hooked: when the backend returns UNKNOWN, the VkImage and the
  VkFormat the game passed are kept (map capped at 64), and the view uses that format. The WWZ quirk stays the
  fallback. The unknown-format warning is logged once per value.

Build OK, headless all PASS (proxy-link vs 0.4.9, amd-host-contracts, shader-compatibility, the seven lmxxf tests,
person).

Re-test RDR2 Vulkan, FSR 2 on, INI defaults:
1. No `Unknown FFX surface format: 0` spam; with LogLevel debug the FSR 4 dispatch shows `depth: ... vk: <non-zero>`.
2. Open/close the pause menu (Esc) several times, also the map: no crash.
3. Pick `FSR 3.X/4 w/Dx12` in the menu: no `Depth not exist!!`, picture OK. Then FSR 2.2.1 and FSR 4 again.
4. NR on (mochizuki) with Esc in and out.

## DLSS-NR on AMD at the Vulkan upscaler seam (2026-10-25)

Before: a Vulkan game with one of OptiScaler's native Vulkan upscalers (FSR 2.2 / FSR 3.X / XeSS / FSR 4 on Vulkan,
`IFeature_Vk`) only had NVIDIA's model at the seam (`EvaluateAfterUpscaleVk`, `nvngx.dll_dlssnr.dll`), so on AMD NR
never ran there (RDR2: `DLSS-NR Vulkan unavailable: nvngx.dll_dlssnr.dll was not found`), and the final image stood
aside because an upscaler existed.

### How it works now

New `dlssnr/amd/VulkanSeam.{h,cpp}` (`DlssNr::VkSeam`), called from `NVSDK_NGX_VULKAN_EvaluateFeature`
(inputs/NVNGX_DLSS_Vk.cpp) right before `deviceContext->Evaluate`, for every non-bridged feature:

- `VkSeam::Owns(physicalDevice)`: AMD adapter (DXGI vendor by the Vulkan device LUID, past the spoof) and
  `AmdBridge::HasFiles()`. On NVIDIA nothing changes; `EvaluateAfterUpscaleVk`/`EvaluateStageVk` decline when the seam
  owns NR, so NVIDIA's Vulkan model is never tried on AMD.
- The AMD runtimes take D3D12 resources (mochizuki imports them into its own Vulkan device), so the frame crosses to
  OptiScaler's D3D12 device (one per adapter) the way the Vk w/Dx12 upscalers cross: colour (game format), motion,
  depth (R32 through `DepthTransfer_Vk`) and exposure are copied by compute (`ResourceCopy_Vk`) into images shared
  with D3D12, on the game's command buffer, with acquire/release to `VK_QUEUE_FAMILY_EXTERNAL`. No CPU round trip.
- The game's command buffer is split at the evaluate with `Vulkan_wDx12`'s existing machinery (virtual command
  buffer + pending submission split at `vkQueueSubmit`): game part signals R on a shared D3D12 fence (imported as a
  timeline semaphore) -> D3D12 waits R, runs NR, signals D -> copy-back waits D -> continuation. The upscaler records
  into the continuation, where the game's own recording goes on (state replayed by the tracker).
- On D3D12, `DlssNr::EvaluateBeforeUpscale(list, params, queue)` runs on the copies with the parameter block's
  Color/Depth/MotionVectors/ExposureTexture pointed at them - the same seam a D3D12 game uses, so AmdBridge, all three
  runtimes, the settling/warm-up/DRS gates, person protection, stabilizer, RenoDX composition, the neural timer,
  the stall watch, status lines and per-game quirks are that path's, unchanged. Its replacement is copied (D3D12
  compute, format-converting) into a shared R16G16B16A16 image that becomes the upscaler's Color until `VkSeam::After`.
- lmxxf/mochizuki lists are proxies (`CreateProxiedCommandList` + `EnsureSubmissionHook` + `SetExpandEnabled`), as in
  the D3D11 bridge. The D3D12 work is executed only after the game's submit (new `PendingSubmission::settled`
  callback): lmxxf's (and danielblnc's) HIP wait on the CPU for the producer half, which needs the Vulkan copies to have
  been submitted. Executing at evaluate time deadlocked lmxxf in the first test.
- Pass-through (frame unchanged, upscaler reads the game's colour): NR off, runtime missing/settling/building/busy,
  stall watch stood down, danielblnc with "After the finished frame" (D3D12 only; a note says so), a second evaluate
  in an already split command buffer, a secondary command buffer, a colour/motion format with no exact D3D12 twin
  (sRGB etc.). Creation failures stop the seam for the session with a status line (`AmdBridge::Note`, shown as Status
  in the Neural page) and a log line. A submit that cannot be split (unsupported `pNext`) also stops it
  (`Vulkan_wDx12::abortedSubmissions`), since that frame lost its continuation.
- Log lines: `DLSS-NR Vulkan seam: ready on D3D12 device ... (proxied|plain lists)`, `first frame of NR before Super
  Resolution (WxH, colour format N)`, `frames pass through (<AmdBridge status>)` / `NR answers again after N frames
  passed through` (when it changes; first 40, then every 100th).
- Menu: each AMD runtime's Info shows `Vulkan: NR runs before Super Resolution on a D3D12 copy of the frame (N frames)`.
  The FPS overlay's NR time is AmdBridge's, so it works as on D3D12.

Other changes it needed:

- Vk w/Dx12 (`IFeature_VkwDx12::CreateDx12Device`): its D3D12 lists are now proxies with the submission hook when
  lmxxf/mochizuki run, like `IFeature_Dx11wDx12`, and its D3D12 work is then queued from the same `settled` callback
  (after the game's submit) instead of at evaluate time. Before, the D3D12 seam inside the bridge could not split those
  lists ("Split returned S_FALSE"), so lmxxf/mochizuki never ran with a w/Dx12 upscaler on Vulkan. The Vulkan seam
  never runs for bridged features: `bridged` is now `deviceContext->IsWithDx12()` (the old upscaler-type list missed
  upstream's bridges, whose type is the D3D12 feature's), checked before the evaluate, so NR runs once.
- `CommandBuffer_StateTracker`: state is tracked for a w/Dx12 feature or once the seam engaged
  (`State::vulkanSeamTracking`); one `Tracking()` helper replaces the 25 copies of the gate.
- `Vulkan_wDx12`: a command buffer begun or reset again while its transaction is pending (recorded, never submitted)
  now releases the D3D12 wait (`AbortUnsubmitted`) instead of leaving the D3D12 queue waiting forever; the CPU release
  no longer moves a fence backwards (it is a Vulkan timeline semaphore). Also benefits the w/Dx12 upscalers.
- `hkvkCreateDevice`: timeline semaphores are enabled when NR is on and an AMD runtime is installed (the split's
  semaphores are timeline; before this only FG/low latency enabled them). Like the Vulkan final image, NR must be on
  before the game starts.
- `AmdBridge`: `Note()` for the seam's status; initial status reads "waiting for an upscaler frame" (was "DirectX 12").
- The seam's state is never destroyed (the driver may be gone at exit; an exit-time destructor crashed the test).

Costs/limits to watch: the tracker runs on every game `vkCmd*` once the seam is engaged (as for w/Dx12); the CPU may
wait up to 2 s for the D3D12 work of the frame before last (3 slots), then frames pass through for a second; one
copy of colour/motion/depth (+exposure) per frame and one answer copy. Native DLSS passthrough on Vulkan (real NGX /
NeuralHub) is not covered.

### Tests

- Build OK. Headless all PASS (proxy-link vs 0.4.9, amd-host-contracts, shader-compatibility, the seven lmxxf tests,
  person).
- New `tests/vulkan_seam_smoke.cpp` + `tools/test-vulkan-seam.cmd <runtime folder> [mochizuki] [lmxxf] [daniel]`: a
  headless Vulkan "game" driving OptiScaler's NGX Vulkan exports with `VulkanUpscaler=fsr22`: first frames until NR
  reaches the upscaler, a quality change (feature recreated 1280x720 -> 960x540), 60 frames without evaluate (menu), a
  command buffer recorded twice before its submit, and after every evaluate a copy of the upscaled frame recorded by
  the game (must land after the upscaler). Each runtime's frames are compared with an NR-off run.
  `set UPSCALER=fsr21_12` (or `ffx_12`) runs the same through a Vk w/Dx12 upscaler and checks the Vulkan seam stays
  out and the D3D12 seam inside the bridge runs (`fsr22_12` is not a Vulkan option; it falls back to native FSR 2.2).
  Results on the RX 9070 XT (runtimes: mochizuki and lmxxf 0.5.7, danielblnc 0.6.0, folder
  `exportsulkan-seam-runtimes`): native FSR 2.2, all three ALL PASSED (mean frame difference against NR off 0.009 to
  0.018 at every checkpoint, longest frame under 50 ms after warm-up); FSR 2.1 w/Dx12, all three ALL PASSED.
  Found and fixed on the way: lmxxf deadlocked when the D3D12 work was executed at evaluate time (now after the game's
  submit, in the seam and in the w/Dx12 bridge), the seam's exit-time destructor crashed the process, and the w/Dx12
  check by upscaler type missed upstream's bridges (now `IsWithDx12()`).
  danielblnc 0.8.0 (NeuralHub host) was not tested here; RDR2 has it installed.
- `test-final-image-gate` with the Vulkan cases (fake VULKAN_SDK from external\vulkan + library\vulkan): all Vulkan
  final-image cases pass. The D3D11 `stall` case fails the same way with the 0.5.7 release DLL (runtime timing, not this
  change).
- Not run: `test-vulkan-present` (moves the real cursor), D3D12 game smoke tests (nothing on that path changed besides
  the status text), anything in game folders.

### In-game test (RDR2 Vulkan, FSR 2)

Install `exports\release-local\OptiScaler.dll` as `winmm.dll` in `E:\SteamLibrary\steamapps\common\Red Dead
Redemption 2` (keep a backup); `system.xml` API Vulkan; FSR 2 on in the game; OptiScaler.ini `[DlssNr] Enabled=true`,
`NrBackend=mochizuki`, `[Upscalers] VulkanUpscaler=auto` (FSR 2.2.1 Vulkan).

1. Start, load the save. OptiScaler.log: `DLSS-NR Vulkan seam: timeline semaphores enabled`, `... is an AMD GPU`,
   `ready on D3D12 device ... (proxied lists)`, `AMD neural: NrBackend=mochizuki runs mochizuki`, then (the first build
   of the network takes up to half a minute) `first frame of NR before Super Resolution (..., colour format N)`. None of:
   `DLSS-NR Vulkan unavailable`, `Aborting Vulkan w/Dx12 submit transaction: unsupported`, `NR is off until the game
   restarts`.
2. Insert menu, Neural page: Info shows `Vulkan: NR runs before Super Resolution ... (N frames)` with N growing; the FPS
   overlay shows the NR time. Toggle NR off/on and change a mochizuki strength: the picture follows.
3. Esc in and out ten times, open the map, back: no crash; NR comes back (`NR answers again` lines are fine).
4. Change FSR 2 quality in the game (Quality, Balanced, Performance, Ultra Performance): NR resumes after each
   (mochizuki rebuilds its network for a new render size; frames pass through meanwhile).
5. In OptiScaler's menu pick `FSR 3.X` (Vulkan), then `FSR 3.X/4 w/Dx12`: with w/Dx12 the log has `VkwDx12 same-frame
   NR: submission hook ready, lists proxied` and NR still runs (once: the NR time does not double).
6. Repeat 1-4 with `NrBackend=lmxxf`, then `NrBackend=daniel` (restart after each change; daniel shows `plain lists`).
7. Note the frame rate and NR time with NR on/off at the same spot. If something is off, send OptiScaler.log,
   mochizuki_nr.log and amd_bridge.log.

## DLSS-SR from the NeuralHub host in Vulkan games (2026-10-26)

RDR2 Vulkan (build 9e5d3a4e1): `DLSS w/Dx12` with danielblnc 0.8.0 ran at ~0.9 fps (host log: `host watchdog: job N
still running 200 ms`, `its capture never landed`), and `DLSS` (native Vulkan) fell back to XeSS.

### Cause

A CPU/GPU wait cycle, the same kind as the lmxxf deadlock fixed in 9e5d3a4e1. With NR before SR and DLSS-SR from the
host on the same D3D12 list, `AmdPreSr::Backend::Submitted` (called right after `ExecuteCommandLists`) wakes NR,
then **waits on the CPU for NR's job to finish** before waking DLSS-SR (`waitNrPass`, up to 5 s). NR's job first waits
for its capture, which sits behind the D3D12 queue's wait for the Vulkan copies, and those are only signalled once the
game calls `vkQueueSubmit`. For danielblnc the Vk w/Dx12 bridge used plain lists and executed its D3D12 work inside
the evaluate, so the game thread stood in `waitNrPass` before it could submit: every frame ended through the host's
200 ms watchdog or the 5 s timeout. The Vulkan seam never hit it (it always queues after the game's submit, and its
list has no NGX feature).

`[DlssNr] HostComputeBackend` is not involved: the wait is OptiScaler's submission order, the same with D3D12 compute.
HIP stays (RDR2 has no nvngx_dlssg.dll).

### Fix

- `IFeature_VkwDx12::CreateDx12Device`: the D3D12 work goes to the queue after the game's submit (`Settled`) with any
  AMD runtime installed or the host armed, not only with proxied lists. Log: `VkwDx12: D3D12 work queued after the
  game's submit (AMD NR runtime / NeuralHub host on plain lists)`.
- The host serves NGX for D3D12 only (`NVSDK_NGX_VULKAN_Init_ProjectID: not supported`). `FeatureProvider_Vk::GetFeature`:
  on a non-DLSS GPU with the host installed, `DLSS` becomes `DLSS w/Dx12` (log `DLSS on Vulkan runs w/Dx12: the
  NeuralHub host serves DLSS on D3D12 only`); the Vulkan menu lists only `DLSS w/Dx12` there.
- A Vulkan game never makes the NGX D3D12 calls that arm the host (`EnsureNgxHost`), so `DLSS w/Dx12` only worked after
  the Vulkan seam had run danielblnc once; picked at start it fell back to FSR 2.2. New `AmdBridge::NgxHostInstalled()`
  (AMD + 0.8.0 pass file, or armed) and `EnsureNgxHostForInterop()` (arms it on WithDx12's D3D12 device) are called
  when a Vulkan DLSS feature is made; the menu offers DLSS w/Dx12 once the host is installed (log `AMD neural:
  NeuralHub host for a w/Dx12 DLSS feature: armed`).
- `AMD NGX arm: ... D3D12 NGX exports are missing` was a false alarm in Vulkan games (NVNGXProxy's getters are null until
  its D3D12 init); the check now asks the host module for `NVSDK_NGX_D3D12_CreateFeature`.

### Tests

- Build OK; the 11 headless tests PASS (proxy-link against 0.4.9).
- `tools\test-vulkan-seam.cmd` takes `UPSCALER=dlss_12` or `UPSCALER=dlss` (routed, run as bridged). New checks: DLSS
  w/Dx12 created through the host with no fallback, D3D12 work queued after the submit, the host ran DLSS-SR, no
  `host watchdog: job` / `capture never landed` in `dlssnr_amd_host\dlssnr_on_amd.log`. Runtime folder
  `exports\vulkan-seam-runtimes-080` (RDR2's danielblnc 0.8.0: dlssnr_amd_pass1.dll, weights, dlssnr_amd_host\ with
  neuralhub_host.dll and nvngx_dlss.dll 310.9.1).
- With the defer change reverted the test reproduces the bug: watchdog fired, `HIP completion timeout pass 1`, NR off
  (no difference against NR off). With the fix, RX 9070 XT: `dlss_12` and `dlss` ALL PASSED (0 watchdog firings,
  host NR jobs ~5 ms with 0.3 ms waiting for the capture, DLSS-SR ~1.3 ms on the GPU, longest frame 35 ms; NR changes
  the DLSS frame by 0.010-0.012 mean); native FSR 2.2 (mochizuki, lmxxf, danielblnc 0.6.0, and 0.8.0) and FSR 2.1 w/Dx12
  (all three) ALL PASSED. The script now deletes `dlssnr_amd_host\*.log` per run (the host appends to it).

### In-game re-test (RDR2 Vulkan)

Install `exportselease-local\OptiScaler.dll` as `winmm.dll` (keep the backup); API Vulkan, FSR 2 on in the game;
OptiScaler.ini `[DlssNr] Enabled=true`, `NrBackend=daniel` (0.8.0 installed as now).

1. `[Upscalers] VulkanUpscaler=dlss_12`, start, load the save. OptiScaler.log: `AMD neural: NeuralHub host for a w/Dx12
   DLSS feature: armed`, `VkwDx12: D3D12 work queued after the game's submit`, `DLSSFeatureDx12::InitDLSS _CreateFeature
   result: NVSDK_NGX_Result_Success`, no `Falling back`. Frame rate normal (compare with FSR 2 + NR at the same spot).
2. `dlssnr_amd_host\dlssnr_on_amd.log`: `NGX: DLSS-SR #1 evaluates now done by DLSS-SR`, `SR job N done` lines, and no
   `host watchdog: job`, `capture never landed` or `SPIKE` after the first seconds.
3. Insert menu: the upscaler list shows `DLSS` only with the w/Dx12 tag. Switch to FSR 2.2 (Vulkan) and back to DLSS:
   NR keeps running both ways (Vulkan seam, then the w/Dx12 bridge), no stall on the switch.
4. Set `VulkanUpscaler=dlss` (native), restart: log has `DLSS on Vulkan runs w/Dx12`, same result as 1.
5. Change the game's FSR 2 quality twice and Esc in/out a few times: no freeze, DLSS-SR follows the new render size.
6. Optional: NR off with DLSS w/Dx12 (DLSS-SR alone) runs at normal speed too.
If anything is off: OptiScaler.log, amd_presr.log, amd_bridge.log and dlssnr_amd_host\dlssnr_on_amd.log.
