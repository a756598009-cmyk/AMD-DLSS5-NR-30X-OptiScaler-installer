# 007 First Light — Path Tracing / Ray Reconstruction Unlock — RE Notes

Game: 007 First Light 1.2.0.0 (RUNE repack, Glacier engine / IOI)
Exe SHA1: cbbbd23388498b77edecf2829e3c402838bb2730
OptiScaler feature: `GameQuirk::UnlockPathTracingMenu` (misc/Quirks.h + misc/FirstLightPTUnlock.h/.cpp)
Status after latest build: menu PT/RR visible; PT OFF -> game enters/loads stable (confirmed by user);
PT enable at menu -> crash in NRD resource setup (see "current wall").

## Architecture of the gate

Menu visibility is driven by Glacier JSONTemplate variables:

- `$dlssSupport` = 4 bytes published from the feature object:
  [0]=DLSS ok, [1]=(dlss-something != 0), **[2]=Ray Reconstruction support**,
  [3]=other (FG/reflex-ish)
- `$israytracingavailable` = published byte[7]

Publishing chain (all offsets RVA-relative to exe base 0x140000000, ASLR-safe via
GetModuleHandle(nullptr) + RVA):

- Manager instance: `[exe+0x5C6DBC8]` (static global)
- Feature object (obj2): `[manager + 0x17C90]`
- Feature object members (patched to 1 by the watcher):
  +0x49 (RT available -> $israytracingavailable / byte[7]),
  +0x59 ($dlssSupport[2] = RR),
  +0x5D, +0x111, +0x112 (AND-combined flags -> globals 0x1466E5741/42)
- Publisher F2 `0x140D80870` (vtable+0x30 of obj2 at 0x142D41448) copies members to
  static globals at `0x1466E5738..0x1466E5750` every time the menu evaluates.
- Feature object global mirror: `0x145E0FA90`
- obj2 vtable: `0x142D41448`

## ZRayTracer singleton

- Static slot: `[exe + 0x5E107D8]` (also manager+0x189D8)
- Class: `ZRayTracer` (RTTI `.?AVZRayTracer@@`), vtable `0x142D4C6E0`
- 62 rip-relative LOADS of the static slot, ZERO stores/leas/data refs ->
  written only through the renderer init (computed/generic path)
- ctor `0x140DC87E0` (initializes +0x59B0 registry, +0x59B8 tracker, +0x5C20 flag, etc.)
- Consumers crash on null: deferred RT init at `0x140E3CDB0..0x140E3CDDE`,
  level loading `0x140DCE480 / 0x140DCE50F`, tracker user `0x142777010`

## Patches applied by FirstLightPTUnlock (pattern-scanned, ASLR-safe)

1. ZRayTracer creation gate `0x140E2DEF2` (renderer init):
   `48 89 87 B0 7C 01 00 | 48 8B 87 90 7C 01 00 | 44 38 70 49 | 74 39`
   = `mov [rdi+0x17CB0], rax; mov rax,[rdi+0x17C90]; cmp byte [rax+0x49], r14b; je skip`
   -> NOP the trailing `74 39` so ZRayTracer ctor runs unconditionally.

2. Feature-object sub-object gate `0x140CCA0C5` (feature object construction):
   `4C 89 35 D3 59 14 05 | 41 0F B6 76 49 | 40 84 F6 | 74 59`
   = `mov [rip->145E0FA90], r14; movzx esi, byte [r14+0x49]; test sil,sil; je skip`
   -> NOP the trailing `74 59`. Creates the sub-object at obj2+0x40857C0
   (needed by level loading ray paths; crash site was `0xCDBA44`).

3. Feature flags (+0x49/+0x59/+0x5D/+0x111/+0x112) written via obj2 as soon as
   the object exists (~100ms after attach; tight 100ms poll, then 5s re-assert).
   Originally delayed to 60s because the engine's one-shot deferred RT init
   (~+49s) read the flags and touched the then-null ZRayTracer; since patch 1
   creates the singleton unconditionally, the delay was removed so the deferred
   init runs fully and builds the RT instance / material tables (consumer of
   the missing tables crashed at 0xDCD81A).

4. NRD setup null-guard (entry of `0x140E4BB00`): the entry's first two
   instructions (`48 8B C4 | 48 89 58 20`, unique 40-byte prologue pattern,
   exactly 1 hit in the image) are replaced with `jmp rel32` into a small RWX
   stub page VirtualAlloc'd near the exe (hint scan exeBase +/- i*0x10000000,
   i = 1..8, 64K-aligned, so rel32 reaches). The stub:
   `mov rax,[rsp+0x28]` (5th arg = ptr to TracePathTracing builder struct)
   `mov rax,[rax]` (member[0] = NRD integration object) `test rax,rax`;
   NULL -> `mov qword [rcx],0; mov qword [rcx+8],0; mov rax,rcx; ret`
   (arg1 out-struct = {0,0}, return arg1 - bit-identical to the function's own
   "NRD disabled" path, the `je 0x140E4BDE6` at 0x140E4BCA5);
   non-NULL -> replay `mov rax,rsp; mov [rax+0x20],rbx` then `jmp entry+7`.
   RAX is dead at entry (safe to clobber); flags from `test` survive `ret`-independent.
   Result: PT toggles on and runs un-denoised instead of crashing.
   NOTE: first version of the stub build wrote only the 4 rel32 bytes of the
   trailing jmp and left the 0xE9 opcode as zero (stub page is zero-init) ->
   the non-null path corrupted `[rsp]` and fell off the stub page -> startup
   crash at the stub page's last byte once the flags were applied early
   (WER fault address 0x130000FFF = stub page 0x130000000 + 0xFFF). Fixed by
   writing the opcode byte; keep this in mind when adding further stubs.

Timing: gate patches must land BEFORE the renderer init (watcher polls 100ms from
DLL attach, gates patch within ~100ms - way before renderer init at ~+2-5s).

## Crash history (WER Event 1000 offsets, all in exe unless noted)

| Offset | Cause | Status |
|---|---|---|
| 0xE3CDDE | ZRayTracer null, deferred RT init (`cmp [obj2+0x49]` then `[ZRT+0x59B0]`) | FIXED (gate 1) |
| 0xDCE50F | ZRayTracer null, level loading | FIXED (gate 1) |
| 0x2777054 / 0x2777089 | fake-dummy experiments (abandoned) | N/A |
| 0xCDBA44 | featureObj+0x40857C0 null (5th arg `this`) | FIXED (gate 2) |
| 0xE4BB9C | NRD setup `mov r9,[rax+0x10]`, rax = struct[0] from TracePathTracing builder return | GUARDED (entry null-guard, patch 4) |
| 0xDCD81A | RT instance/material collect `vmovups ymm0,[rax]`, rax = [mat+0xC8] null | ROOT CAUSE: deferred RT init skipped (flags late) - fixed by removing 60s flag delay |
| 0xCCD1D7 | game's own GPUCrashReport exit (int3, exit code 2000) after DXGI_ERROR_DEVICE_HUNG | CURRENT WALL: GPU-side hang in the game's own PT render |

## Current wall details (GPU driver hang / TDR) - isolated to FSR-RR denoiser dispatch

- FINAL matrix (runs 1-6): {OptiScaler RR dispatch OFF + upscaler ON} OK;
  {RR dispatch ON + upscaler OFF} OK (2090 dispatches); {both ON} =
  deterministic DEP AV (upscaler dll+0xC9500, vtable[8] past the 8-method
  ffxProvider ABI) with EVERY provider variant (driver-side AND internal).
  The provider-registration interplay (IAmdExtFfxApi) was fully ruled out:
  the hook chain (DriverStore amdxc64 load + detour + custom
  AmdExtFfxApi/UpdateFfxApiProvider) worked end to end (run 5 v3) and the
  crash reproduced at the same RVA. The RR denoiser has NO internal provider
  (RR = driver-only via amdxcffx64; the driver even rejected the dll's
  registration with 0x80070057 and the RR still worked).
- The architectural fix (DeferredDispatch, build 2026-09-17 17:11): the RR
  denoiser dispatch is recorded into an OptiScaler-owned scratch command
  list (3 slot round-robin, one direct allocator+list each, a fence guards
  allocator reuse and the destructor path) during evaluate, and submitted to
  the title's direct queue (State::currentCommandQueue) from the wrapped
  swapchain's Present hook - after every title submission of the frame, so
  the denoiser and the upscaler never share a command list. The
  FSRDPreprocessor gained deferred-dispatch mode: the title-side conversion
  skips its SRV->UAV output transitions (the deferred recording owns them
  via TransitionDenoiserOutputsToUav/ToRead), IFeature gained a virtual
  SubmitDeferredCommandLists (no-op default), and until the first submission
  the composition uses the RawSourceBlit passthrough instead of reading
  uninitialized denoiser outputs. Debug visualization modes are forced off
  in deferred mode. The composition consumes the denoised signals one frame
  late (a lighting-response latency trade-off, far better than no
  denoising).
- Run 7 result (user, 17:31 session): no crash, ~6550 dispatches all
  successful, scratch lists submitted 1:1 at present - but the image was
  indistinguishable from DisableDenoiserDispatch. Root cause: the
  intermediate textures were RECYCLED across the deferred boundary, so the
  dispatch's results could never reach the composition:
  1. The floor seed/filter ping-pong wrote its scratch into
     m_outputBuffer1/2 - the same textures the denoiser writes its outputs
     to and the composition reads. On a single list the dispatch overwrote
     the floor scratch after conversion; deferred, the composition of frame
     N read the floor-filter scratch (and the dispatch's real output was
     overwritten by frame N+1's floor pass before anyone read it).
  2. The composition wrote its composed colour into Resources.Motion, which
     the deferred dispatch still reads as its motion-vector input - the
     denoiser ran on composed-colour-as-MVs every frame, killing temporal
     reprojection.
  Fix (build 2026-09-17 18:44): in deferred mode the floor passes ping-pong
  on dedicated m_floorScratch1/2, and the composition writes a dedicated
  m_compositionColor (GetCompositionOutput returns it in deferred mode).
  The non-deferred path keeps the historical recycling untouched (it also
  gives DisableDenoiserDispatch its "floor filter as poor-man's denoiser"
  look). Known deferred caveat: PublishAmbientOcclusionOutput's copy checks
  m_ambientOcclusionOutputInUavState at record time and would fail if AO
  were enabled - AO is off in this title, left as future work.
- Run 8 result (user, 19:21 session, recycling fix active): no crash, no
  visual change - and FloorIsolation=0 turns the screen COMPLETELY BLACK.
  That black is diagnostic gold: with isolation 0 the floor layer is zero
  and the residual handed to RR is the full raw colour, so a working
  denoiser would still be visible. Black => the denoised contribution to
  the composition is ~zero; the visible image is essentially the floor
  layer alone (which lerps toward raw for "microcontrast", hence the noise
  at isolation 1).
- Output probe (build 19:20, one-shot GPU readback of the diffuse OUTPUT
  vs its INPUT signal at dispatch #60 of each context): in-game context
  showed INPUT alive (maxLuma 0.98, 1.9% bright) and OUTPUT identically
  zero (avg 0.0, 100% dead). The dispatch returns OK but never writes the
  output textures. Not the signal type: the session's last contexts ran
  Direct+Direct (user flipped the ini to auto mid-session) and the probe
  was still hard zero. All previous "biraz denoised" impressions were the
  floor layer, in every configuration tested so far.
- Next A/B (build 19:28): the same probe now also records on the TITLE's
  command list in non-deferred mode (Run 6 config: DeferredDispatch=false +
  DisableUpscalerDispatch=true). Output alive there => the scratch list is
  the problem; zero in both => the AMD RR dispatch itself no-ops in this
  process (provider/driver layer).
- A/B result (user, 19:30 session): ZERO on the title's own command list
  too (input alive: maxLuma 0.26, 0.8% bright; output identically 0.0,
  100% dead). The scratch list is exonerated; the dispatch itself no-ops.
- ROOT CAUSE FOUND (same session's log): "Amdxc64Hooks::Init Failed to load
  amdxc64.dll". The DriverStore search added in run 5 is gated on
  NoDriverProvider=true, but that flag went back to false after run 4
  (it over-blocks FSR4), and the 007 First Light quirk DoNotLoadAmdxc64
  vetoes the bare-name load - so amdxc64 never loaded. The FFX denoiser
  dll's GetModuleHandleW("amdxc64.dll") then hit OptiScaler's K32 hook,
  which hands out a FAKE handle; the dll cannot reach the driver's FFX
  provider and falls back to its INTERNAL provider - the 'FSR Ray
  Regeneration - 1.2.0' (id 0x101020000) that QueryDenoiserVersions lists.
  That internal provider's dispatch returns FFX_API_RETURN_OK while
  writing NOTHING to the signal outputs. Every probe session ran against
  this stub; every visual test to date (including run 6's "biraz
  denoised") was the floor layer alone. The driver provider, when actually
  loaded (run 5 v3 era), DID real GPU work - that is what produced the
  original denoiser+upscaler same-list hang.
- Fix (build 19:37): Amdxc64Hooks::Init also performs the DriverStore
  search when amd_fidelityfx_denoiser_dx12.dll is deployed next to
  OptiScaler (MainDllPath check) - deploying the denoiser dll is the
  explicit opt-in for RR, and RR needs the real driver provider even on
  titles whose quirk vetoes the bare-name load. Ini restored to the full
  deferred config for run 9: DeferredDispatch=true,
  DisableUpscalerDispatch=false, SignalTypes auto (Direct+Direct).
- Run 9/10 (19:57 + 20:05 sessions, real driver provider confirmed loaded,
  probe re-armed every 600 dispatches, then extended to 3 targets with raw
  RGBA dumps): 10652+6459 dispatches, 0 failures, NO crash - the deferred
  architecture holds with real driver GPU work. But in-game probes show
  diffuse AND specular outputs EXACTLY (0,0,0,0) while the diffuse input
  carries real radiance (avg up to 4.5e-3, max 0.70, 8% bright; alpha
  65504 = "missing diffuse ray length"). Same context, dark scenes fill
  both outputs with uniform 1.1027e-5 (RGB, alpha 0); the fill flips to
  exact 0.0 at the in-game transition - a global behaviour change, not a
  per-pixel function of input. The dispatch returns OK in all cases.
  Standing hypotheses: the driver's RR ignores our scratch list and runs
  on its own internal submission at dispatch-call time (reading
  pre-conversion inputs), or it validates something about the dispatch
  (alpha 65504 diffuse hit distance?) and drops both signals.
- Title-list A/B result (user, 20:12 session): the driver dispatch DOES run
  on the title's command list - but writes ONLY the diffuse output's ALPHA
  (per-pixel alive, e.g. 3.6e-3..6.8e-3) while RGB stays exactly 0. On the
  scratch list it writes nothing at all. So the dispatch is live and
  list-sensitive, and something zeroes the radiance specifically.
  Prime suspect: the diffuse input's alpha carries 65504 (FP16-max
  "environment miss", s_MissingDiffuseHitDistance) on every pixel because
  the title supplies no diffuse ray length - if the driver applies
  miss-equals-no-signal semantics even to the DIRECT diffuse path (whose
  alpha the contract leaves undefined), the denoised RGB would be zeroed
  everywhere while its alpha bookkeeping still gets written. Fits every
  observation so far.
- Experiment (build 20:20): s_MissingDiffuseHitDistance 65504 -> 1.0f
  (finite "typical" hit; every pixel an active diffuse sample), shader
  recompiled cs_6_2 + fp16, header regenerated with disassembly prefix.
  Tested first in the title-list config (DeferredDispatch=false +
  DisableUpscalerDispatch=true) where the driver demonstrably writes, so
  any RGB change is attributable to this one variable.
- Result (user, 20:20 session): input alpha now 1.0 everywhere, but output
  RGB is STILL exactly 0 - while the output ALPHAS became much more alive
  (up to 0.71 diffuse / 0.86 specular, clearly responding to the input
  alpha change: ~4e-3 with 65504 input, ~0.7 with 1.0 input). The
  denoiser demonstrably READS our input texture and WRITES the output
  texture; RGB is zeroed deliberately. Alpha-semantics theory dead.
  Also confirmed against AMD's SDK-v2 sample (denoiserrendermodule.cpp):
  our desc chain/resources match theirs exactly (they write alpha=0 for
  direct diffuse inputs, 65504 only for indirect misses).
- Standing theory (next test): the driver-side RR is a COUPLED pipeline -
  the pass that materializes denoised RGB runs as part of the FSR4
  upscaler dispatch, so a lone denoiser dispatch only writes metadata.
  Every test so far had the upscaler either disabled or on a different
  command list. The original same-list crash is now attributed to the
  upscaler using the DRIVER provider (vtable[8] OOB, '4.1.1'/'FSR4-i8'
  jump) - so the coupled test runs the upscaler on its INTERNAL provider
  (NoDriverProvider=true blocks only the Upscaling registration) while the
  denoiser keeps the driver provider, both on the title's list:
  DeferredDispatch=false, DisableUpscalerDispatch=false,
  NoDriverProvider=true. Crash here => the provider fight was not the
  (only) crash cause; alive RGB => coupled theory confirmed.
- Same-list coupled test result: 0x80000003 @ exe+0xCCD1D7 (the game's
  GPUCrashReport int3) - a GPU-side crash even with the upscaler on its
  internal provider. Provider split fixed the CPU AV but the denoiser GPU
  work followed by any upscaler dispatch on ONE list still kills the GPU.
  Stable deferred config restored.
- CP77 CROSS-CHECK (user's observation "worked in CP77 with the pre-merge
  burak113 build" - correct and decisive): stock 61846a23 build + today's
  driver + same RR 1.2 dll = CP77 runs flawlessly (1763 RR + 1763 FSR
  dispatches, 0 failures, clean exit). Driver/machine exonerated.
- CP77 turned into an automated bisect rig (tools_tmp/cp77test.sh:
  deploy dll -> launch -> poll log for 'Dispatching FSR-RR' -> compare log
  size after 25s freeze window -> taskkill; exit 0 good / 1 hang / 125
  unclear). Results:
  * b4086151 (pre-merge WIP): GOOD, RR=2461.
  * e63a04b1 (merge): State.h committed BROKEN (mangled conflict garbage);
    65e8e54b also fails to compile (Streamline_Hooks order); 55ac4de6
    builds but RR never created (NGX capability) - game stable, no RR.
  * fca4efe9: builds, RR created => BAD-hang (RR=1, log frozen).
  * HEAD-clean: BAD-hang. Full working tree + DeferredDispatch=true:
    ALSO BAD-hang (RR=1) => the hang is INDEPENDENT of the command-list
    split; the denoiser ffxDispatch call itself poisons the next upscaler
    ffxDispatch (CPU deadlock profile).
  * Exonerated one by one: GpuTime Start/End, the new post-process chain,
    the ffx loader load, shader bytes (stock .cso still hangs), all
    uncommitted session work (HEAD-clean still hangs).
  * KILLER CONFIRMED: the 0.10.0 merge replaced FfxApi_Proxy.h with the
    master 0.10.0 port. fca4efe9 + the FORK proxy (bridged: WLOG macros,
    designated-init fix, IsSRReady/IsDenoiserReady/IsFGReady(bool)
    defaults, ImGuiNotify include) => GOOD (RR=2391). The master proxy
    lost TryInstallFfxModuleHooksDx12 - the per-module
    CreateContext/Dispatch/Configure/Query detours (EnableFfxInputs).
    Without those, ffx dispatch descriptors are not processed the way the
    fork expects, which explains BOTH the CP77 hang and the 007 RGB=0
    probe results.
- Final main-tree build (22:41): fork FfxApi_Proxy (bridged) + deferred +
  probes => CP77 GOOD (RR=2641). Deployed to 007 Retail with
  DeferredDispatch=true for the next user run; the probe will show
  whether the diffuse/specular output RGB is now written.
- 007 run with that build: stable (2900 dispatches, 0 failures) but the
  in-game probe STILL shows exact-zero outputs with alive input. The
  FfxApi proxy hooks were not the RGB=0 cause either. CP77 with the same
  build + DeferredDispatch=true (added to its ini during testing) also
  shows a bad image => the remaining common factor is OUR deferred
  dispatch architecture itself, which exists in no working build: stock
  uses same-list dispatch and CP77's image was clean there.
- Decisive test queued: CP77 with DeferredDispatch=false on the same
  binary (our non-deferred path reproduces stock behaviour by design).
  Clean image => the deferred recording/submission breaks the driver RR
  dispatch (list-independent: even recording into a never-submitted
  scratch list poisons the next upscaler dispatch, and with deferred ON
  the denoiser never writes). Then either fix the deferred submission
  specifics (fence/queue/desc states) or ship same-list on titles where
  it works and keep deferred only where the same-list hangs.
- RESULT (user, 22:58 CP77 session): image CLEAN with
  DeferredDispatch=false on the same binary. And the fixed probe finally
  reported numbers: diffuse AND specular outputs ALIVE (avgLuma ~0.004,
  0% dead pixels) with temporal accumulation visible (outputs alive on a
  frame whose input was dead). RR is genuinely working on the same-list
  path with the fork proxy. The deferred architecture itself is what
  breaks the driver RR dispatch (recording into a scratch list - even
  before submission - poisons/zeroes everything).
- NEXT: 007 with DeferredDispatch=false + fork proxy - NEVER tested. The
  original 007 same-list hangs were all observed on MASTER-proxy builds;
  the proxy may have been the real hang cause all along, not the
  same-list dispatch itself. If 007 hangs here too, deferred must be
  fixed instead (submission specifics); if it works, the whole deferred
  architecture can be dropped or kept purely as a fallback.
- 007 same-list + fork proxy: NO hang (3464 dispatches, 0 failures) -
  the hang was the proxy all along. But outputs still zero RGB with
  alive alpha, and HardwareDepth=true override (linear-depth hypothesis)
  changed nothing. CP77 comparison pinned the remaining differences:
  CP77 = hardware depth + LH + specularHitDistance PRESENT +
  DirectDiffuse+IndirectSpecular; 007 = linear depth (SL tag literally
  'LinearDepth', R32_FLOAT UAV-capable texture) + RH + NO hit distances
  + Direct+Direct.
- Single-signal path implemented (user theory: '007 only sends a raw
  spec signal; our two-signal system gives up'): DenoiseDiffuse /
  DenoiseSpecular ini keys gate both the context signalFlags and the
  dispatch chain; ValidateRRDispatchChain takes expected booleans now.
  First specular-only run crashed in LogRRDispatchSnapshot's resource
  inventory on every frame (SL VEH swallowed 484 dumps; dispatch never
  reached - the flashing screen). Snapshot/probe battery now skipped in
  single-signal mode. Second run: clean, 14930 dispatches, 0 dumps.
  Probe verdict: specular-only still writes NOTHING (output zero); the
  'alive diffuse output' was the floor-filter content (non-deferred
  floor ping-pong still recycles outputBuffer2), and the 'denoised
  diffuse' debug view was showing that floor - hence the blurry image.
- Current test (23:53): CP77-equivalent combo - DenoiseDiffuse=true +
  DenoiseSpecular=true + SpecularSignalType=1 (Indirect) so the chain is
  DirectDiffuse + IndirectSpecular exactly like the working CP77 setup
  (diffuse alpha=65504 miss, specular alpha=-1 invalid since the game
  ships no hit distance). If the driver accepts Indirect-without-guide
  here, RR should finally write in 007.
- RESOLVED isolation step (user experiment, 2026-09-17): removing
  `amd_fidelityfx_denoiser_dx12.dll` from Retail made the hang disappear - the
  game's own PT renders fine without it (22865+ frames in one session). The
  hang source is therefore OptiScaler's FSR-RR denoiser GPU dispatch on RDNA4,
  NOT the game's own PT passes and NOT NRD.dll.
- Full RT pipeline runs end-to-end on AMD without the dll: startup RT init
  completes (flags early), PT renders in-game.
- Consequence of the dll being absent: OptiScaler reports
  `SuperSamplingDenoising.Available=0` (InitNGXParameters,
  NVNGX_Parameter.cpp) because `FfxApiProxy::IsDenoiserApiImplementedDx12()`
  is false. With path tracing on, RR is the game's ONLY upscaler - it never
  creates the NGX feature at all (0 CreateFeature/0 EvaluateFeature in the
  log) and renders native. The per-frame
  `slValidateFeatureContext 'kFeatureDLSS_RR' context is missing` errors are
  the game still trying to evaluate a feature that was never created.
- Fix shipped (build 2026-09-17 14:32, deployed as dxgi.dll): new
  `[FSR-RR] UpscalerOnly` ini key (default false). When true and SR is ready,
  OptiScaler reports `SuperSamplingDenoising.Available=1` so the game creates
  DLSS_RR, `FeatureProvider_Dx12` allows creating FSRDFeatureDx12 without the
  denoiser API, `InitFSR3` skips `CreateDenoiserContext` (FSRDConvShader
  stays null), and `EvaluateInternal` skips `PrepareDenoiserInput` +
  denoiser/composition dispatch - the FSR3.1 upscaler runs on the raw
  PT inputs. Expect a very noisy image (no denoiser); the point is verifying
  the in-game upscaler dispatch. Denoiser debug modes are forced off under
  this flag since there are no converted signals to display.
- First test round (14:35): the feature created and initialized, but every
  evaluate failed - `UpdateSize` saw the render size exceed the creation
  ceiling because `_denoiserCtxDesc.maxRenderSize` stays {0,0} without a
  denoiser context, producing an infinite create/recreate loop (~200ms
  period) and the "Upscaler failed to run!" popup. Fixed in build 14:39:
  the UpscalerOnly branch now seeds the ceiling from the current render
  size, mirroring `CreateDenoiserContext`'s pre-creation seeding. The
  shimmering/waviness observed is consistent with the per-frame history
  resets this loop caused.
- Remaining wall (dll restored): the FSR-RR denoiser GPU dispatch hangs on
  RDNA4 (~96 frames after first dispatch). Suspicts:
  (a) RR 1.2 provider/dispatch backend incompatibility warning in the log,
  (b) absent denoiser inputs (RR_DIAG: specularHitDistance=absent,
  invalid=zero) potentially driving denoiser shaders into unbounded loops,
  (c) NRD.dll passes (now dead on AMD thanks to the null-guard).
  Next diagnostic: NVIDIA/AMD Aftermath GPU crash dump (game loads
  GFSDK_Aftermath_Lib.x64.dll) - locate the faulting pipeline/shader.

### Hang anatomy (live catch, 14:45 session, dll restored)

Repro timeline in one session (game start 14:45:02, death 14:45:12):
RR NGX create 07.543 -> evaluate#1 07.782 skipped ("can't restore root
signature") -> evaluate#2 07.824 full RR path: conversion OK -> AMD denoiser
dispatch returned OK (frame 0, reset=true) -> composition OK -> FSR31 upscaler
dispatch began 07.829 -> **CPU AV 0.95ms later** -> present 08.149 returned
0x887A0005 (DEVICE_HUNG), WER 0xCCD1D7 (GPUCrashReport int3) at 14:45:12.
Only ONE denoiser dispatch ever succeeded before the hang. The frametime
before the evaluate spiked to ~590ms (a stall), the crashing frame ran at
335ms.

- Streamline mini-dump
  (C:/ProgramData/NVIDIA/Streamline/007FirstLight/1789645507829377/sl-sha-260595cb.dmp):
  exception tid=0x5d4c (23884 = the SL dlss_d evaluate thread, confirmed via
  the dlssdBeginEvent lines on the same tid), code 0xc0000005, param[0]=0x8
  (DEP), RIP = amd_fidelityfx_upscaler_dx12.dll+0xC9500. No stack memory in
  the dump.
- The fault address is INSIDE .rdata: the dll's .text ends at ~0xA98DB,
  0xC9500 is in .rdata. At that RVA sits the ASCII string "4.1.1" followed by
  "FSR4-i8" - the FSR4 model/version strings - directly AFTER an 8-entry
  function-pointer table (RVA 0xC94C0..0xC94F8, entries 0x180006830..0x180007fc0),
  which is stored as a global object's vptr in .data (initializer code at
  .text 0x1095/0x12cd writes it). The version string "3.1.5" (the dll's FSR
  3.1 api version, matches FfxApiProxy::VersionDx12) sits right before the
  table.
- Interpretation: a caller invoked an out-of-range virtual/function-table
  entry (index 8) on an 8-method interface - the call target landed on the
  version string "4.1.1" and the thread tried to EXECUTE data -> DEP. A
  deterministic (same RVA in both the 13:43 and 14:45 hangs), not
  data-random, ABI/interface mismatch. The exception was on the evaluate
  thread, ~1ms after the FSR31 upscaler dispatch began inside the RR
  evaluate - i.e. the crash is in the FFX upscaler dll's dispatch path, with
  the AMD denoiser dispatch having just completed on the same command list.
- Cascade: SL's VEH catches the AV, writes the mini-dump (~320ms) and
  continues; the half-recorded/corrupted command stream gets submitted; the
  GPU hangs on it; present returns DEVICE_HUNG; the game's GPUCrashReport
  (int3 0xCCD1D7) exits with code 2000. The GPU hang may be a consequence of
  the CPU crash OR the denoiser's GPU work hanging first and the AV being the
  device-loss aftermath - the fixed RVA suggests a deterministic CPU-side
  ABI bug as the primary fault.
- Working comparisons: FSR4 upscaler dispatch (FFXFeatureDx12, no RR, no
  denoiser) works (13:55 session); UpscalerOnly (FSR31 upscaler, no AMD
  denoiser dispatch at all) works (user confirmed). Only the full RR pipeline
  (denoiser dispatch on the same command list before the upscaler dispatch)
  crashes.
- Bisect shipped (build 15:08): `[FSR-RR] DisableDenoiserDispatch` ini key
  (default false, enabled in the deployed ini) - skips ONLY
  `DispatchDenoiser` while conversion, composition and the upscaler still run
  and the evaluate reports success. If the hang/AV disappears -> the AMD
  denoiser dispatch (or its side effects on shared FFX state) is the trigger;
  if it persists -> our conversion/composition shaders are suspect and the
  D3D12 debug layer (the log's InfoQueue note) is the next diagnostic.

### Bisect result 1 (user run, 15:10 session): AMD denoiser dispatch IS the trigger

- `DisableDenoiserDispatch=true`, dll restored: the game ran 7+ minutes
  stable (log ends 15:17, present result 0 throughout) with conversion,
  composition, FSR31 upscaler dispatch and the denoiser CONTEXT created - the
  ONLY skipped step was the `FfxApiProxy::D3D12_Dispatch` call on the denoiser
  context. The denoiser dispatch call is the trigger.
- New reverse-bisect shipped (build 15:25): `[FSR-RR] DisableUpscalerDispatch`
  - skips ONLY the upscaler dispatch (composition output blitted to the game
  output), the AMD denoiser dispatch runs. Expected outcomes: crash -> the
  denoiser's GPU work hangs on RDNA4 on its own; no crash -> the crash needs
  the denoiser dispatch followed by the upscaler dispatch on the same command
  list (an interaction, consistent with the AV being in the upscaler dll).
- amdxc64 angle: the FFX dlls call `GetModuleHandleW("amdxc64.dll")` 92x per
  session (the upscaler dll's .rdata - including the crash region - contains
  "amdxc64.dll"/"AmdExtD3DCreateInterface"). OptiScaler's K32 hook gives a
  FAKE HMODULE when amdxc64 is absent and fsr4Support != None; in the stable
  session there are ZERO fake-handle gives (the real amdxc64 was already
  loaded, from DriverStore B026218) and `Amdxc64Hooks::Init` FAILED to load it
  earlier - so OptiScaler's FSR4 driver-model hooking never attached while the
  dlls use the real unhooked extension. In the crash dump the real amdxc64 is
  loaded. Worth testing `Fsr4DoNotLoadAmdxc64` and the FSR4 model-selection
  hooks as follow-up variables once the reverse bisect splits the question.

### Bisect result 2 (user run, 15:27 session): denoiser GPU work alone is fine

- `DisableUpscalerDispatch=true`, denoiser dispatch ON: 2090 successful
  denoiser dispatches (60 and 600 dispatch stability milestones logged, 0
  failures), 4463+ frames, no crash. The RR 1.2 dll's GPU work runs perfectly
  on RDNA4 by itself.
- Result matrix: {denoiser OFF + upscaler ON} OK, {denoiser ON + upscaler
  OFF} OK, {both ON} deterministic crash (DEP AV in the upscaler dll's
  dispatch). The crash is an interaction between the two dispatches on the
  same command list, not a shader hang in either one.
- Run 3 shipped (build 15:34): `[FSR-RR] DisableNativeDenoiser=true` - the PT
  unlock's NRD setup guard stub is extended to always take the "denoiser
  disabled" path (branch bytes become xor rax,rax + nops), so the title's own
  denoiser integration never builds (in the 15:10 session the title created 3
  of its own denoiser contexts on the same dll - 12 factory calls vs
  OptiScaler's 6). Two denoiser clients sharing one AMD denoiser dll is the
  remaining suspect for the interaction crash. If run 3 is clean, the fix is
  simply shipping DisableNativeDenoiser=true; if it still crashes, the
  interaction is inside OptiScaler's own denoiser+upscaler dispatch pair and
  the next variables are the dispatch flags (NON_GAMMA_ALBEDO), signal types
  (Direct vs Indirect), or a D3D12 debug layer run.
- Run 3 second attempt (15:38) also INVALID: crashed at 0x13000000D with
  0xc0000005 at stub offset 0xD = the stub's own `mov qword [rcx],0`. The
  bytes were correct this time - but the setup function is called from
  multiple sites with different ABIs, and at least one of them does not pass
  the out-struct pointer in rcx, so the unconditional zero-out faulted on the
  write. The always-disable-through-the-entry-stub approach is DEAD; the
  entry stub stays null-guard-only (guarding just the member[0]==null case,
  which only ever happens at the designed call site).
- Run 3 v3 shipped (build 15:48), safer force-off: the title's "native
  denoiser enabled" flag byte at `*(*(exe+0x5E0FAA0) + 0x8EA8)` is read by
  THREE sites in the NRD family (0xE4BC9E in the setup fn 0x140E4BB00,
  0xE4D068 in fn 0x140E4C1C0, 0xE4DA1B in fn 0x140E4D8E0). The new
  DisableNativeDenoiser implementation: (1) the setup function's
  `je 0x140E4BDE6` is made unconditional (jmp to its own disabled epilogue,
  which returns arg1 unchanged - correct ABI at every call site, no race);
  (2) the flag byte itself is zeroed and re-asserted in every watcher loop
  (100ms during init, 5s after) so the other two consumers also take their
  own disabled paths, consistent with how the title behaves on AMD before the
  unlock (flag stays 0 there). Pre-unlock AMD runs prove byte=0 is a stable
  game state.
- Run 3 v3 (15:48) result: the je-patch and the flag zeroing both landed
  (log: "Native denoiser force-off installed", 0 game-native denoiser factory
  calls) but the title crashed at 0xE4CCF6 (0xc0000005, `mov rcx,[rax+0x10]`,
  rax null) inside its NRD-family function 0x140E4C1C0 - the title's own code
  dereferences state only the denoiser setup registers, so suppressing the
  native denoiser is NOT survivable. DisableNativeDenoiser is now documented
  as unusable and defaults to false again.
- The crash mechanism refined (run 4 candidate): FFX SDK 2.x ffxProvider ABI
  has 8 virtuals (dtor, CanProvide, IsSupported, CreateContext, DestroyContext,
  Configure, Query, Dispatch) - the deterministic vtable[8] AV in the upscaler
  dll matches a caller built against a NEWER provider ABI calling index 8 on
  an 8-method provider object. GetProvider resolves the DRIVER-SIDE provider
  (ffxProviderExternal via amdxc64/IAmdExtFfxApi) BEFORE internal ones; the
  FFX dlls register their providers into the driver through
  IAmdExtFfxApi::UpdateFfxApiProvider, and two FFX clients (denoiser + upscaler
  contexts) sharing that per-device driver state is the leading suspect.
  OptiScaler's own fake-HMODULE mechanism (hk_K32_GetModuleHandleW) was
  designed to intercept exactly this but never fired because amdxc64 was
  already loaded by the driver.
- Run 4 result (user run, 16:04): NO CRASH - the driver-side provider
  interplay IS the mechanism - but it over-blocked: FSR4 upscaler AND FSR4 FG
  both disappeared because the fake amdxc64 handle broke the GAME's own
  FSR4 detection (AmdExtD3DDevice8 -> E_NOINTERFACE) and blocked every
  provider registration, while RR has NO internal provider at all (the RR
  query returned 0 versions - the "FSR Ray Regeneration - 1.2.0" provider we
  had been enumerating IS the driver-side one). With no RR provider the game
  never even created the NGX feature (0 CreateFeature), so this run tested
  nothing about the RR dispatch itself.
- Run 5 shipped (build 16:15): the surgical version.
  (1) Amdxc64Hooks::Init now finds amdxc64.dll in the DriverStore (bare
  LoadLibrary fails - it is not on the loader search path; the same search
  amdxcffx64 already used) so the real detour attaches and every
  AmdExtD3DCreateInterface call lands in OptiScaler's wrapped factory. The
  K32 fake-handle hack is reverted (unneeded once the detour is attached; the
  game's own FSR4 detection keeps working through the forwarding wrapper).
  (2) UpdateFfxApiProvider now blocks ONLY the Upscaling effect's
  registration -> the upscaler dll's contexts run on its internal providers
  (the internal 3.1.5 provider was verified present in the 16:04 log), while
  the RR denoiser (driver-only implementation) and FSR4 FG keep registering
  with the driver. The denoiser+upscaler two-dll provider interplay that
  crashes is expected to disappear; FG and FSR4 detection stay intact.
- Run 5 first attempt (16:16) INVALID: crashed with the ORIGINAL signature
  (0xCCD1D7 GPUCrashReport after the first denoiser dispatch) because the
  DriverStore search never ran - "Trying to load" never logged. Cause: the
  search was gated on `primaryGpu.fsr4Support != None`, but
  Amdxc64Hooks::Init runs before the D3D12 capabilities update, so
  fsr4Support is still None at that point and the gate blocked the search
  every time. Fixed by dropping the gate in build 16:17; run 5 needs a
  quirk flag only guards the bare-name load; NoDriverProvider explicitly
  opts into the detour). Same lesson twice: check the quirk-driven volatile
  config values, not just the ini text.
- Run 5 third attempt (16:21): the hook chain FINALLY worked end to end
  (amdxc64 loaded from DriverStore, detour attached, 14 custom-AmdExtFfxApi
  handouts, 5x "UpdateFfxApiProvider blocked ... for: Upscaling") and the
  crash STILL reproduced at the SAME RVA (upscaler dll+0xC9500, DEP, vtable[8]
  - confirmed via the new Streamline dump 1789651287136161). Two extra
  findings: the driver REJECTED the denoiser dll's provider registration with
  0x80070057 (E_INVALIDARG) twice, and RR still worked (the driver's own
  provider serves the denoiser regardless of the registration outcome). The
  crash is INDEPENDENT of which provider implementation the upscaler context
  runs (internal here, driver-side in the defaults) - so the provider-
  registration interplay is NOT the AV's direct cause.
- Full matrix now: {OptiScaler RR dispatch OFF + upscaler ON, dll present} OK
  (run 1, 7+ min); {OptiScaler RR dispatch ON + upscaler OFF} OK (run 2, 2090
  dispatches); {both ON} deterministic crash with every provider variant.
  Leading model: OptiScaler's own driver-side RR dispatch racing the title's
  native AMD denoiser path (the dll is present, the title's native setup runs
  with flags-early and builds its contexts) - run 1 had OptiScaler's dispatch
  off and was stable with the dll present.
- Run 6 deployed (ini only): DisableDenoiserDispatch=true with UpscalerOnly
  false - OptiScaler's FSR-RR keeps conversion/composition/upscaler but skips
  its OWN denoiser dispatch; the title's native AMD denoiser (present dll)
  is expected to do the denoising. USER MUST CHECK: is the image actually
  denoised (the native denoiser working) or still raw? If stable + denoised,
  this is the shipping configuration.

## Resolved wall details

### 0xE4BB9C (NRD setup) - resolved via entry null-guard (patch 4)

- Crash function `0x140E4BB00..0x140E4BE13` - NRD resource setup
  ('NRD:Packed Diff Radiance Hit Dist' / 'NRD:Packed Spec Radiance Hit Dist').
- Called from `0x140E8BDE0..0x140E90B64` (ZRayTracer AllocateIntermediateBuffers-ish)
  at `0x140E8DFEC`, 5th arg = pointer to stack struct at caller `[rsp+0x2c0]`,
  filled from the TracePathTracing builder return (builder called at
  `0x140E8D115`, sret `[rsp+0x170]`, 0x50 bytes copied to `[rsp+0x810]`).
- `0x140DC4B90` = TracePathTracing frame-graph pass builder
  (strings 'TracePathTracing', 'D:\p4\...\pathtracing.cpp'). Its returned struct's
  member[0] is NULL on AMD -> NRD setup dereferences it -> AV.
- The function has its own "NRD disabled" path (`cmp byte [0x145E0FAA0+0x8EA8],0`
  / `je 0x140E4BDE6` at 0x140E4BCA5) which leaves arg1 = {0,0} and returns it;
  the caller tolerates that shape (it just copies `[rax]` 16 bytes onward).
  The guard (patch 4) reproduces exactly this shape when struct[0] is null.
- Prologue pattern (40 bytes, exactly 1 hit in the whole image):
  `48 8B C4 48 89 58 20 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 A0
   48 81 EC 60 01 00 00 4C 8B A5 C0 00 00 00 48 8B F1`

### 0xDCD81A (RT instance/material collect) - root cause: late flags

- Function `0x140DCD701..0x140DCD8A9` iterates 12-byte instance entries
  (`[rsi+0x1aa8]`, `id-1` or `[rsi+0x1B24]-1` fallback), looks up the
  0xD8-stride material pool at `[rsi+0x1a68]` (indices from `[entry+0]` /
  `[entry+4]`), reads `[mat+0xC8]` and copies 64 bytes of material data into
  the output (also writes `idx*2` / `idx*2+1` pairs to `[r13+...]`).
- No direct callers (job/callback invoked via pointer).
- On AMD with the 60s flag delay the deferred RT init at ~+49s skipped
  -> per-material RT data never built -> `[mat+0xC8]` NULL. Removing the delay
  makes the deferred init run with flags set (patch 1 makes ZRayTracer exist
  early, so the init's own ZRT use is safe).

## RR menu toggle (still can't be enabled)

RR toggle shows On (default) but grayed. Separate gate - candidates: $dlssSupport
bytes [1]/[3] semantics, or the toggle's own JSONTemplate condition. Not yet RE'd.

## OptiScaler / FSR-RR notes

- The game never evaluates DLSS_RR: log shows 0 `slEvaluateFeature feature 1001`
  calls; all evaluates are feature 0 (DLSS). FSR-RR (FSRDFeature_Dx12) was never
  created because the game never requests RR - its RT pipeline never ran.
- Merge dispatch verified correct: inputs/NVNGX_DLSS_Dx12.cpp creates FSR_RR on
  non-Nvidia (line ~650); Streamline_Hooks forwards feature 1001 to inline SL.
- If the RT pipeline ever fully initializes, RR evaluate should start flowing into
  SL -> NGX -> FSR-RR automatically.

## Tooling notes

- x64dbg attach FAILS ("Could not open process") - use external RW instead:
  `tools_tmp/rw.py` (ctypes ReadProcessMemory/WriteProcessMemory).
  PEB ImageBase lookup via NtQueryInformationProcess (ASLR! the exe DID rebase).
- Crash offsets: `Get-WinEvent -LogName Application -FilterHashtable @{Id=1000}` ->
  "Fault offset" = RVA (add 0x140000000 for VA, verify base live first).
- Static analysis: capstone scripts in tools_tmp (disasm.py, rtti_vt.py, pe_scan*.py);
  function bounds via .pdata (binary search, 12-byte entries, .pdata va 0x6961000
  raw 0x3a27a00 size 0x20217c).
- RPKG: chunk0/chunk1 are RPK2 magic but a 2026 layout that rpkg-cli 2.34.0 cannot
  import (hash header entry appears to be {hash u64; off_hi u32; size_flags u32;
  off_lo u32} vs H3's {hash u64; offset u64; size u32}). packagedefinition.txt is
  XTEA-encrypted (RPKG-Tool `decrypt_packagedefinition_thumbs` has the H3 key;
  007's key differs - the exe's own XTEA at VA 0x1400804D5 takes the key as an arg).
- If the flag-gate route stalls, the rpkg route = find the platform singleton
  registration data (ZRayTracer etc.) and clone it for AMD platform.

## Repo files changed

- `OptiScaler/misc/Quirks.h` - enum UnlockPathTracingMenu + QUIRK_ENTRY for
  007firstlight.exe + printQuirks entry
- `OptiScaler/misc/FirstLightPTUnlock.h/.cpp` - watcher: gate patches (2x) +
  feature flag re-assert thread (60s first patch, 5s cadence)
- `OptiScaler/dllmain.cpp` - include, printQuirks, StartWatcher call
- `OptiScaler/OptiScaler.vcxproj` + `.filters` - new files
- Build: `F:\VisualStudio\MSBuild\Current\Bin\MSBuild.exe OptiScaler.sln /p:Configuration=Release /p:Platform=x64`
  -> deploy `x64/Release/a/OptiScaler.dll` as game Retail/dxgi.dll
