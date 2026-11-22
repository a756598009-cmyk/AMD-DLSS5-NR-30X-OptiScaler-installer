# NR backend selector

How OptiScaler picks one of the AMD NR runtimes, and how the lmxxf backend records its work.
danielblnc and lmxxf ship since 0.2.0-amd-nr; mochizuki is being tried out.

## Choosing the runtime

`[DlssNr] NrBackend` is read once at launch (`Selector.cpp`). Each backend installs its own D3D12
hooks when the device is created, so the menu's "NR runtime" combo applies on the next launch.

| `NrBackend` | Active backend |
|---|---|
| `daniel` | `DanielBackend`, runtime `dlssnr_amd_pass1-3.dll` |
| `lmxxf` | `LmxxfBackend`, runtime `LmxxfNrRuntime.dll` |
| `mochizuki` | `LmxxfBackend`, runtime `MochizukiNrRuntime.dll` |
| `off` or `none` | None: no AMD Record, the original colour goes to SR |
| `auto`, empty or missing | lmxxf when `LmxxfNrRuntime.dll` sits beside OptiScaler and `dlssnr_amd_pass1.dll` does not, otherwise daniel |
| Anything else | daniel |

`AmdBridge::HasFiles` looks for the active backend's runtime DLL. The `submission/` hooks are armed
only while lmxxf or mochizuki is the active backend (`SubmissionHooksWanted`).

`LmxxfBackend::Record` refuses frames after the upscale (`afterUpscale`), so lmxxf runs only before
Super Resolution. This is a local change on top of TheAutomatic's lmxxf files.

## Passes and temporal history

`[DlssNr] Passes` (1 to 3) runs the network again on its own output inside the same HIP enqueue.
With `LmxxfTemporal` each pass also gets its own output from the previous frame as the network's
history, warped by the game's motion vectors: `TemporalChain` in `LmxxfNrRuntime.cpp` records the
motion conversion, coordinates and one sampler per pass before the cut, and one history feed per pass
after it. The flow is upstream's `native_game_frame.h` for one pass. `OutputSmooth` (upstream's
`DLSS5_OUTPUT_SMOOTH`, `LmxxfSmoothStrength`/`LmxxfSmoothThreshold`) then pulls the last pass's
output toward its warped history where they differ little, before it is shown or kept.

`LmxxfSmoothResidual` (frame flag `LMXXF_NR_FRAME_FLAG_SMOOTH_RESIDUAL`) moves that blend onto the
network's change alone. `OutputSmooth::RecordResidual` (`residual_main` in `native_output_smooth.hlsl`,
vendored Patch G) takes the last output minus the network's input (`PostBase`), pulls it toward the
previous frame's change, warped by `residualSampler` along the same coordinates as the histories, and
adds it back to the input; the colour blend does not run. The change it keeps goes to
`TemporalChain::residual` for the next frame. It runs in the same place, so the network's history is
still the image shown, and the game's jittered samples reach the upscaler as they arrived.

## mochizuki

`MochizukiNrRuntime.dll` implements the lmxxf ABI (`LmxxfNrApi.h`), so `LmxxfBackend` drives it
unchanged apart from the DLL name. Inside, mochizuki0323's Vulkan network (`third_party/mochizuki`)
runs on a Vulkan device of its own, on the game's adapter:

- `RecordInputs` copies the colour and the motion vectors into shared D3D12 buffers the Vulkan device
  imports.
- `EnqueueHip`, between the two halves of the game's list, signals the game queue's shared fence, submits
  the network on Vulkan behind a wait on that fence (imported as a timeline semaphore), and makes the game
  queue wait for the network's signal.
- `RecordOutputs` copies the result into the texture Super Resolution gets.

The network is built for each render extent and colour format on a background thread; frames pass
through until it is ready. It reads `dlssnr-amd\dlssnr.bin` and `dlssnr-amd\shaders\` beside the DLL,
keeps its pipeline cache there and logs to `mochizuki_nr.log`.

Its settings are the `[DlssNr] Mochizuki*` keys, and it takes none of its tuning from another runtime's
(`TransferStrength`, `Passes`, `AmdModelScale`, `AmdDynamicScale` and the model keys stay danielblnc's,
lmxxf's and NVIDIA's). The network always runs inside upstream's RenoDX stage (`MochizukiNrControls::renodx`,
always 1 from this host), so the model, pass stack, resolution, composition, look and encoding controls are
the stage's `MochizukiReno*` keys (`MochizukiRenoKeys.inc`). `LmxxfBackend::Record` sends them two ways, both
only when the backend is not lmxxf:

- In `LmxxfNrFrameInfo`: the stage's pass count (`MochizukiRenoPasses`, for the first pass person protection
  asks for; the runtime takes the count from the stage's settings), the network's flags (int4, the 310.8
  model) and the game's motion vectors. No depth: the stage's network reads the vectors at the pixel.
- Through the runtime's own `MochizukiNrSetControls` export (`mochizuki_runtime/MochizukiNrControls.h`):
  the `reno_*` fields, history strength, apply model, dynamic resolution, the preprocess and the noise
  seed. They go only when they change or the session is new, starting from the runtime's
  `MochizukiNrGetControlDefaults`, and apply on the next frame (`RenoPasses`, `RenoResolutionScale` and
  `RenoSourceEncoding` rebuild the network, so the menu commits them when the control is released).

The game's exposure (`MochizukiNrSetExposure`) is the stage's feed. Person protection's first pass
(`LMXXF_NR_FRAME_FLAG_FIRST_PASS`) is the stage's pass 1 in the frame's own encoding, blitted to RGBA32F
before pass 2 runs (`StageFrame::first`). `MochizukiCascadeDetail`, `MochizukiAntiHalo(Strength)`,
`MochizukiPassEstimate` and `MochizukiUseDepth` do nothing in the stage and are out of the menu and the INI.

`MochizukiTemporal` switches the history (without the key, `LmxxfTemporal` does). Every default is
RenoDX's own. `AmdModelScale` and `AmdDynamicScale` do
not reach mochizuki: the settle gate in `AmdBridge::Run` ignores both for it.
`MochizukiDynamicResolution` is the runtime's `drs_mode`. With `auto` (the default), once the render
subrect is smaller than the colour texture, the network runs at a bucket: per axis the largest subrect
seen, rounded up to 64 px, within the texture. A subrect change inside the bucket only restarts the
history, with no rebuild and no settle (the gate in `AmdBridge::Run` then settles on the colour texture).
The bucket grows with a larger subrect, and shrinks after 30 s of frames of one colour texture whose
subrects all stay at least 128 px inside it on both axes. A game that makes a new colour texture at every
step instead (Cyberpunk 2077) is bucketed in `auto` too: textures of one format and aspect share one bucket
at the largest of them, the frame's buffers and the texture handed to SR stay that size, and the gate in
`AmdBridge::Run` folds the smaller textures into it as it does for danielblnc and lmxxf. `exact` rebuilds for every render resolution,
as before; `always` buckets every frame, also across colour texture reallocations. Motion vectors in a
format the bucket cannot blit run as `exact` (logged once a session). The runtime's own default, for a
host that does not send `drs_mode`, is `exact`.
`MochizukiNrGetInfo` feeds the menu's status (model extent, network time, last build), read with the
runtime status at most twice a second.
`EvaluateAtSeam` ignores `ApplyAfterRR` for lmxxf and mochizuki, which take only the seam before Super
Resolution.

## Toolchain

`LmxxfNrRuntime.dll` is MinGW, built by `tools\build-lmxxf-runtime.cmd exports\lmxxf-runtime` from
`lmxxf_runtime/` and the vendored source in `third_party/lmxxf` (pinned in its `UPSTREAM.md`).
OptiScaler (MSVC) talks to it through `LmxxfNrApi.h` only. `tools/PACKAGE_RELEASE.ps1` ships it with
the gfx1201 modules and the top-level shaders.

- Modules: `lmxxf-modules` beside the DLL (or `LMXXF_MODULES_DIR`).
- Weights, never shipped: the first of `native-game-tiled-assets\`, `lmxxf-weights\` and the folder
  named in `lmxxf-weights-dir.txt`, all beside OptiScaler, then `LMXXF_WEIGHTS_DIR`. These are the
  tiled assets; the 0.24.2 `HIP/` folder is never read.

`MochizukiNrRuntime.dll` is MSVC, built with its shaders by `tools\build-mochizuki-runtime.cmd` (see
`third_party/mochizuki/UPSTREAM.md`).

## Record sandwich (fail-closed)

`LmxxfBackend::Record`: PrepareFrame → **require** `ILogicalCommandList` proxy → RecordInputs → Split → RecordOutputs → `SetPendingEnqueue(EnqueueHip)`.
Non-proxy / Split fail → `CancelUnsubmitted`, return **nullptr** (ordinary SR). No Record-time EnqueueHip.
The runtime owns one job per session. If another Evaluate arrives before the previous
game list is submitted, Record returns the original Color for that Evaluate and
keeps the earlier job intact. Cancelling the earlier job after its output has
already been handed to SR would invalidate the recorded continuation.
`Pending()` is a process-wide singleton because the product has one active NR
backend. Its slot is keyed by the logical list supplied to the between callback;
submitting other game or FG lists does not consume it. Multiple simultaneous NR
backends would require a per-backend or per-session registry.

## Product Execute

When `ExpandEnabled()`, `AmdBridge::ExecuteBatch` always `ExecuteExpanded` (QI proxy → `ExecuteOnWithBetween`).
`ExecuteExpanded` forwards the current logical list as an explicit callback argument.
If `EnqueueHip` fails, the callback reads the runtime's thread-local error on the
submission thread and retains it for the next `PrepareFrame` failure log. `Retire`
and `ResetHistory` failures are also logged at their call sites. This preserves
the original error when the runtime subsequently reports only a poisoned session.
`PendingListIndex` stays **-1** (Daniel-only batch isolation); lmxxf intentionally does not use it.

## Admission and continuation

- Split refused on: open query at the cut / invalid query scope / predication / enhanced barrier / open split barrier / aliasing / render pass / RTAS / meta / root·sample overflow → ordinary SR. Completed queries and timestamp EndQuery remain eligible.
- Continuation seed: viewport/scissor/topology/PSO/rootsig/heaps/blend/stencil/OM + IA/SO/VRS/strip-cut/view-mask + RootBindState + sample positions + depth bounds.
- `ResourceStateBook::ApplyExecuteDecay` updates **our book** only; it does not rewrite game barriers.

## Known limits

- `QueryCapabilities` always reports `hip_ready=0`. Whether HIP loaded is in `OptiScaler.log`.
- `RootBindState` tracks up to 64 root parameters and 64 root constants; past that the split is refused.
- Execute decay has not been checked against the D3D12 debug layer in a live game.
- While the hooks are armed, lists created with `CreateCommandList1` are wrapped as well.
- A render resolution above 1080p needs `LmxxfFitLarge` and can hitch.
