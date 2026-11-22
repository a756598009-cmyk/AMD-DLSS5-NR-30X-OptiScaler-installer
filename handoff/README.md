# Handoff — OptiScaler AMD (three-fork merge)

What this repository is, how it got this way, how to build and install it, and what is
still open. Written for whoever picks this up next, including a future me.

Paths below are written generically:

- `<REPO>` — this checkout
- `<GAME_DIR>` — the folder holding the game executable (for many titles that is the
  folder with the `.exe`; for Cyberpunk 2077 it is `.../bin/x64`)

---

## 1. What this is

One OptiScaler build for AMD hardware, merged from three forks that each solved a
different part of the same problem.

| Source | Branch / head at merge | What it contributes |
|---|---|---|
| `MatheusFerreiraS/neural-amd-opti` | `dlss-neural-rendering` @ `7b7220bb` | Base. DLSS-NR plumbing (`nvngx_dlssnr` proxy), native-Vulkan NR, exposure scan, MFG unlock, Streamline packaging |
| `TheAutomatic/dlss-5-amd-project` | `main` @ `2792909e` | `OptiScaler/dlssnr/amd/` — bridge into the danielblnc DLSS-NR-on-AMD HIP runtime, multi-slot scheduling, D3D12 state freeze/restore, native RTGI, AmdLook |
| `burak113/OptiScaler` | `ffx-denoise-experimental` @ `3da4808e` | FSR-RR — the FidelityFX denoiser wired in as a Ray Reconstruction provider, its floor/signal/responsivity controls, fakenvapi work, PT/RR gate bypass |

Upstream of all three: `optiscaler/OptiScaler` → `Dagherbou/OptiScaler_DLSSNR` →
`wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`. The AMD neural runtime itself is
`danielblnc/DLSS-NR-on-AMD` and is **not** reimplemented here — this repo is the bridge
into it.

**The two denoise routes are alternatives, not a stack.** AMD neural rendering
(`[DlssNr]`) runs the neural model through the AMD runtime; FSR-RR (`[FSR-RR]`) uses
AMD's FidelityFX denoiser as the RR provider. Super-resolution stays FFX/FSR in both.

### Current state

Everything is on `dlss-neural-rendering`. The work was committed on the branch `amd-nr-0.1.0`
(the three-fork merge exactly as it had been staged, then everything section 5 lists after the
merge, then formatting), which pull request #1 merged into `dlss-neural-rendering`. Fixes since
then are committed there directly.

Eleven GitHub releases, all built by `tools/PACKAGE_RELEASE.ps1`:

- `v0.1.0-amd-nr`: the FidelityFX upscaler, frame generation and denoiser never load from the
  package layout, so FSR falls back to FSR 2 and Ray Reconstruction is greyed out. Do not use it.
- `v0.1.1-amd-nr`: that fixed (section 5, "FidelityFX modules load from the OptiScaler folder").
- `v0.2.0-amd-nr`: the lmxxf runtime, effect strength, colour grade and XeSS multi frame
  generation (section 5, "0.2.0").
- `v0.3.0-amd-nr`: lmxxf multipass, temporal history per pass and output smoothing, lmxxf detail
  and colour strength up to 2, the Neural tab in sections and the red theme (section 5, "0.3.0").
- `v0.4.0-amd-nr`: the host side of a third runtime, mochizuki (its runtime and model are not in
  the zip), and host fixes that reach every runtime: process exit, command lists recycled before
  they run, the upscalers' colour barriers, OptiScaler's own Vulkan devices and the placement of
  the runtimes that run before Super Resolution (section 5, "0.4.0").
- `v0.4.1-amd-nr`: danielblnc runtime 0.4.0 next to 0.3.1 and 0.3.0 (section 5, "danielblnc
  runtime 0.4.0").
- `v0.4.2-amd-nr`: danielblnc runtime 0.4.1 next to 0.4.0, 0.3.1 and 0.3.0 (section 5,
  "danielblnc runtime 0.4.1").
- `v0.4.3-amd-nr`: danielblnc's supporter builds 0.4.2 and 0.4.3 accepted when the user brings one
  (0.4.1 stays the default), the "DLSS 5 mode" and "Tone curve" controls, pass 2 and 3 made from
  pass 1, and mochizuki kept on after a render-resolution change (section 5, "danielblnc runtime
  0.4.2 and 0.4.3").
- `v0.4.4-amd-nr`: a faster mochizuki runtime that rounds its input like the original, `Setup.bat`
  leaving encoding and history to the defaults, two stabilizers off by default, danielblnc's 0.5.0
  supporter build accepted and the public 0.4.2 recommended (section 5, "0.4.4").
- `v0.4.5-amd-nr`: lmxxf and mochizuki beside a ReShade that wraps the game's lists, danielblnc's
  0.5.1 supporter build accepted and the public 0.4.3 recommended (section 5, "0.4.5").
- `v0.4.6-amd-nr`: the RenoDX composition after the AMD runtimes, one Effect strength on danielblnc, its
  own Black lift and exposure source, the upscale ratio and output scaling in the Upscaling tab, an old
  INI's `AmdEncoding=0` read as sRGB, and the public 0.5.0 recommended (section 5, "0.4.6").
- `v0.4.7-amd-nr`: danielblnc's 0.6.0 supporter build accepted (RDNA2) and the public 0.5.1 recommended
  (section 5, "0.4.7").
- `v0.4.8-amd-nr`: danielblnc's colour as his own (Linear encoding, the network's Style), an Info drawer, the
  game's exposure for mochizuki, mochizuki0323's Preprocess, history weight and colour formats, RE Engine's
  `_storage_` and Watch Dogs: Legion fixed (section 5, "0.4.8").

The build reports itself as `0.4.8-amd-nr`, and the packager writes
`dist/OptiScaler-0.4.8-amd-nr.zip`.

The [AMD-NR ReShade Installer](https://github.com/zmodelerlover/AMD-NR-ReShade-Installer) installs
this build as its OptiScaler route. v0.4.0 knows only `v0.1.1-amd-nr`; v0.5.0 and later offer every
version its payload manifest lists, newest first, and install the newest with the lmxxf weights. The
manifest has pinned `v0.4.6-amd-nr` since 2026-09-29 (payload only; the app stays v0.6.8); v0.5.x installs it without
mochizuki. The manifest pins each release zip by URL and SHA-256, and every file it extracts from it
by hash. The danielblnc runtime (0.4.2, `8aa2dcc5…`, inside the `v0.4.3-amd-nr` and `v0.4.4-amd-nr`
release entries as their own `opti-runtime`, 0.4.1 inside `v0.4.2-amd-nr` and 0.4.0 inside `v0.4.1-amd-nr`; 0.3.1, `b108d640…`, at the top level for every older release, which does not
accept 0.4.x) and the lmxxf weights (`native-game-tiled-assets.zip`) come from the Hugging Face
dataset `zmodelerlover/amd-nr`, never from this repository. v0.6.0 and later also offer mochizuki: with **NR runtime: mochizuki** ticked
on an RDNA4 card it installs `MochizukiNrRuntime.dll` with `dlssnr-amd\shaders\`,
`dlssnr-amd\prewarm\manifest.txt` and the model, `dlssnr-amd\dlssnr.bin`, and sets
`NrBackend=mochizuki`. The manifest is not in this repository and
`tools\build-mochizuki-runtime.cmd` does not make it: the runtime writes it after its first serial
build, and it holds only for the `dlssnr-amd\shaders\` it was made from (their file list and sizes),
so the payload needs one made from the shaders it ships. Never replace an asset on a published tag:
every installer would refuse the new bytes. A new release is a new tag, and then new pins in that
installer's `payload/payload.json` (its `handoffs/HANDOFF-v0.5.0-2026-09-23.md` has the steps, and
`handoffs/HANDOFF-v0.6.0-2026-09-25.md` those for 0.4.0 and mochizuki).

---

## 2. How the merge was done (reproducible)

Useful if you ever need to redo it or pull the sources forward.

The three forks share real ancestry, which allowed proper three-way merges instead of
hand-porting files:

```
optiscaler/OptiScaler
  |- Dagherbou/OptiScaler_DLSSNR
  |    |- 21274132  <- in this fork's history
  |         |- this fork  (dlss-neural-rendering)
  |         |- wilsjo2/main  (PreSR-Multipass)
  |              |- MatheusGViana (vendored as a folder, history lost)
  |                   |- TheAutomatic/dlss-5-amd-project
  |- 65a5f6f9  <- merge base with burak113
```

**burak113** shares linear git history, so it was an ordinary
`git merge --no-commit` (base `65a5f6f9`, 6 conflicts).

**dlss5** had no shared history — it vendored an OptiScaler snapshot as the folder
`OptiScaler-DLSSNR-PreSR-Multipass-main/`. The trick that made this a real merge:

1. Find the `wilsjo2` commit closest to that vendored snapshot by counting differing
   files. It was `6cdb5f7e` ("Document rebuilt v0.6.2 swapchain fixes release").
2. Build a synthetic commit whose tree is the vendored subtree and whose parent is that
   commit, so git can compute a genuine merge base (`21274132`):

```sh
git commit-tree "dlss5/main^{tree}:OptiScaler-DLSSNR-PreSR-Multipass-main" -p 6cdb5f7e \
  -m "dlss5 graft"
```

3. Merge that synthetic commit. 13 conflicts instead of a 126-file manual port.

**One correction is required before step 3.** The dlss5 tree flattened six submodules
(`magic_enum`, `nvapi`, `simpleini`, `spdlog`, `unordered_dense`, `vulkan`) into plain
files. Merging as-is replaces the submodules with loose files. Rebuild the graft tree
with the gitlinks restored:

```sh
export GIT_INDEX_FILE=/tmp/graft.idx
git read-tree "dlss5/main^{tree}:OptiScaler-DLSSNR-PreSR-Multipass-main"
for n in magic_enum nvapi simpleini spdlog unordered_dense vulkan; do
  sha=$(git ls-tree 6cdb5f7e:external "$n" | awk '{print $3}')
  git rm -r -q --cached "external/$n"
  git update-index --add --cacheinfo "160000,$sha,external/$n"
done
git write-tree   # feed this tree to commit-tree instead
```

### Merge reconciliations worth knowing

The non-obvious calls, because a future merge will hit them again:

- **Pass ceiling.** This fork's `kMaxPasses = 30` design was kept; dlss5's
  `MaxPassCount` is now an **alias** of it, so both lineages' arrays size from one
  number. (The AMD backend clamps to 3 separately — see section 5.)
- **The DLSS-NR core.** Both lineages renamed the same ancestor function: this fork to
  `EvaluateAtSeam`, dlss5 to `EvaluateInternal`. They were unified into one function
  whose signature carries both sets of parameters (`sourceIn/destIn/destArrival` from
  here, `forcePost/submissionEpoch` from dlss5). The body references both names for the
  same flag, so a local `const bool beforeUpscale = preUpscale;` alias sits at the top.
- **The multi-pass loop** came from dlss5, because the surrounding auto-merged code
  already used its `effectivePasses` / `PassTuning`. Taking that loop silently dropped
  the only `g_nr.wroteTarget = true` assignment, which would have made
  `PreUpscaleResult()` and `EvaluateStage()` report "nothing written" forever. It was
  re-added at the end of a successful resolve. **Check this if you re-merge.**
- **`swapchainEncoding`** was superseded by burak's richer `outputColorSpace` model.
- **`Shader_Dx11` over-release**: dlss5's fix is correct (those pointers are identity
  caches; the SRV/UAV own the references).
- **State-envelope guard**: this fork's placement won — it sits after the device is
  acquired and releases it plus restores the barrier on the skip path. dlss5's version
  at the top of `Dispatch` was a duplicate and was removed.

### Out-of-tree pieces

dlss5's top-level `tools/`, `tests/`, `shaders/`, `package-amd-presr/`, `.githooks/`
were brought in. Because the vendored folder was flattened to the repo root, the
pre-build step's path had to change from `$(ProjectDir)..\..\tools\` to
`$(ProjectDir)..\tools\` in `OptiScaler.vcxproj`. dlss5 had also deleted
`OptiScaler/shaders/shader_tools/{dxc,fxc,dxv}.exe` and friends; the remaining `.bat`
scripts call them via `%~dp0`, so they were restored.

---

## 3. Building

```sh
git submodule update --init
```

Then MSBuild, `Release|x64`, PlatformToolset **v143**. Visual Studio 2022 provides it
directly; VS 2026 (v18) also ships it — check for
`VC/Auxiliary/Build/Microsoft.VCToolsVersion.v143.default.txt`.

```sh
MSBuild.exe OptiScaler.sln -p:Configuration=Release -p:Platform=x64 -m -v:minimal
```

`OptiScaler.vcxproj` sets `VcpkgEnabled=false`. With a machine-wide `vcpkg integrate install`, the
build otherwise links vcpkg's freetype import library instead of the static
`external/freetype/freetype.lib`, and the DLL then refuses to load in any game folder that has no
`freetype.dll` of its own. Every build made on the development machine before that property was
added had this dependency; Cyberpunk 2077 hid it because a `freetype.dll` sits in its folder. Check with
`dumpbin /dependents`: `freetype.dll` must not appear.

The `clang-format Check` workflow runs clang-format **20** over `OptiScaler/` (except
`OptiScaler/include/`) on every push and pull request. Visual Studio ships a newer clang-format
that breaks some lines differently, so format with the same version before pushing:
`pip install clang-format==20.1.8`, then `clang-format -i --style=file <files>`. Regenerated shader
headers under `precompile/` need it too; the upstream ones are formatted the same way.

The binary is produced as `x64/Release/OptiScaler.dll` and then **moved** by a
post-build step into `x64/Release/a/` together with the rest of the package. Look for
it there, not in `x64/Release/`.

> **Long paths.** `external/FidelityFX-SDK-v2` has deep shader paths. Checking out this
> repo under an already-deep directory fails submodule init with "Filename too long".
> Keep the checkout shallow in the filesystem, or set `git config core.longpaths true`.

---

## 4. Installing (AMD neural path)

Everything goes in `<GAME_DIR>`:

| File | Source | Notes |
|---|---|---|
| `dxgi.dll` | `x64/Release/a/OptiScaler.dll`, renamed | The proxy |
| `OptiScaler.ini` | `x64/Release/a/OptiScaler.ini` | |
| `OptiScaler/` | `x64/Release/a/OptiScaler/` | FFX / XeSS / Agility deps |
| `dlssnr_amd_pass1.dll`, `dlssnr_amd_pass2.dll`, `dlssnr_amd_pass3.dll` | three copies of danielblnc's `version.dll` 0.4.0, 0.3.1 or 0.3.0 | One **pass**, not one slot |
| `dlssnr_on_amd_weights.bin` | produced by `dlssnr_on_amd_setup.exe` | Game-agnostic; copy between games freely |
| `amd_fidelityfx_denoiser_dx12.dll` | AMD redistributable | Only needed for FSR-RR. Not built by this repo; loaded by name |

`tools/install-amd-presr.ps1` automates this. It is interactive — do not run it with
`-NonInteractive` in a folder that has other mods, because that branch moves aside any
injection DLL it finds without asking (it would take REFramework's `dinput8.dll` with
it).

### Rules that are easy to get wrong

- **danielblnc's `version.dll` must NOT stay in the game folder.** OptiScaler hosts the
  runtime itself through the `pass` copies. Leaving the author's proxy means two things
  drive the same runtime. The installer moves it aside for this reason.
- **The pass DLLs are identified by SHA-256**, not by name — see
  `OptiScaler/dlssnr/amd/AmdLayout.h` (`kAmd0217`, `kAmd03`, `kAmd031`, `kAmd040`). An
  unsupported build is refused. All passes must be the same version.
- **Proxy choice**: `dxgi.dll` is the safe default. `d3d12.dll` has a reported
  Streamline conflict that **greys out Cyberpunk 2077's Ray Reconstruction option** —
  see the warning in `setup_windows.bat`. For Vulkan titles use `winmm.dll`.
- Titles that gate Ray Reconstruction on NVIDIA hardware need OptiScaler's spoofing
  (`Dxgi=auto`) for the option to appear at all.

### Minimum config for the AMD neural path

```ini
[Upscalers]
Dx12Upscaler=fsr-rr        ; or ffx/xess — fsr-rr routes RR to the FidelityFX denoiser

[DlssNr]
Enabled=true               ; ships disabled; 'auto' resolves to false
ApplyAfterRR=true          ; the AMD placement: true = after the finished frame, false = before SR
                           ; true is REQUIRED for any title driving Ray Reconstruction
RRWorkingScale=1.0         ; 1.0 = model over every pixel of the finished frame
```

`RunBeforeSR` does nothing on the AMD backend. `ApplyAfterRR` alone places the model there (section
5, "One placement switch").

---

## 5. What was added after the merge

Beyond reconciling the three forks:

**AMD neural rendering after the upscaler.** The AMD bridge only ever substituted the
upscaler's *input* (pre-SR) and refused every post-upscale position outright. FSR-RR
denoises and enlarges in one dispatch, so there is no seam before it — which meant the
model could never follow it. Added:

- `Frame::afterUpscale` plus `guideWidth/guideHeight` (`AmdPreSr.h`)
- Relaxed guide-extent validation for that mode, and depth always resampled there. This
  was tractable because the depth/motion conversion shaders already ratio-map, which
  works for upsampling as well as downsampling.
- `AmdBridge::After()` — reads `NVSDK_NGX_Parameter_Output` instead of `Color` and hands
  its answer back rather than substituting
- `WriteAmdPostAnswer()` in `DlssNr_Dx12.cpp` — writes that answer into the frame with a
  format-converting blit (`OS_Dx12`, the same resampler the supersampling legs use),
  restoring the frame's arrival state

**One pass count for the AMD backend.** It was reading `DlssNrRRPasses` in the post
placement, which left the `Passes` control doing nothing in the only placement an RR
title ever reaches. It now reads `DlssNrPasses` in both.

**One placement switch for the AMD backend.** Every Super Resolution evaluate offers the pass
both seams: first `EvaluateBeforeUpscale`, which the NGX entry and both bridges call unconditionally
for SR, then `EvaluateAfterUpscale`. The AMD branch of `EvaluateAtSeam` only gated the seam after
the upscaler. With "After the finished frame" chosen on an SR title, the backend got the render-size
frame and the output-size frame on the same evaluate. `AmdBridge::Run`'s settling check (a static
width/height/scale) saw a size change on every call, reset its 300 ms timer and invalidated history
each time, so the model never ran, or ran twice where render and output sizes match. Cyberpunk's
`amd_presr.log` recorded 939 of these size flips in about 16 minutes.

Now `ApplyAfterRR` alone decides, and the AMD branch declines the seam not chosen before the backend
sees it. The "Processing point" combo reads and writes only that key. It used to read `RunBeforeSR`,
which the backend never consulted, so a fresh install displayed "After the finished frame" while
running before SR. The amber warning for RR titles in the "before" placement now says the model is
not running; it used to claim the model ran after the frame regardless. Defaults are unchanged:
`ApplyAfterRR=false` is the before-SR placement dlss5 always ran. Checked in Cyberpunk 2077 in both
placements, with and without RR.

**sRGB is the default encoding.** `[DlssNr] AmdEncoding` defaults to 2 (sRGB) instead of 0
(Auto) in `Config.h`, the menu's reset button and the packaged INI. In Cyberpunk 2077 it was the
steadiest option and held highlights best. Auto was removed because it did exactly what Linear (1)
does: neither converts, while sRGB and Gamma 2.2 decode before the model and re-encode after. The
combo offers Linear, sRGB and Gamma 2.2. The stored numbers stay 1/2/3 so existing INIs keep their
meaning, and `AmdBridge.cpp` clamps a leftover `AmdEncoding=0` to Linear.

**"Every-frame" is now "Disable temporal stabilization", off by default.**
`[DlssNr] AmdEveryFrame` sets the runtime's `temporal` byte
(`AmdPreSr.cpp`, `L->temporal = everyFrame ? 0 : 1`), so with it on the model runs without
temporal history. Its only other effect, the post-Execute
wait in `WaitAfterSubmitIfEveryFrame`, is skipped whenever more than one slot is configured (the
default is 3), except during a native rebuild. The old label named neither effect. The menu label,
help text and INI comments now describe it, and the default moved from `true` to `false` (history
on) in `Config.h` and the packaged INI. The INI key keeps its name so existing files still load.

**Dynamic NR resolution (AMD backend).** `[DlssNr] AmdDynamicScale` (default off) and
`AmdDynamicTargetFps` (default 60). While the rendered frame rate stays under the target, the model
scale steps down from the configured one (`AmdModelScale` before SR, `RRWorkingScale` after the
finished frame) through 85, 70, 55 and 40% of it, never under 25% of the frame. The controller is
`OptiScaler/dlssnr/amd/DynamicScale.h`, stepped once per frame from `AmdBridge::Run`, where the
interval between calls is the rendered frame time even with frame generation on. It changes rarely
on purpose: down after 2 s under the target, up after 5 s with 15% headroom, a 10 s hold after any
change and 60 s after a step down before it tries to go back up. Every scale change rebuilds the
runtime's staging and restarts the model's history, and in Cyberpunk's logs NR was off for 0.3 to
5.4 s after each change (median 1.25 s over 31 changes in one session, 2.8 s over 15 in the first
dynamic-scale test). A step must cut the scale by at least 10%, so the 25% floor cannot turn the
last level into a rebuild for almost nothing (from a 50% ceiling it stops at 27.5% instead of
going on to 25%). In that test, going from 50% to 35% raised the rendered frame rate from about 60
to 67-74 fps, and the lower levels gave no consistent gain. A continuous controller would keep the
effect blinking; that cost only goes away if the runtime can run on a subregion of a fixed
allocation, which it cannot today. A frame cap at or under the target hides the headroom, so the
level never climbs back; the menu help says to set the target a little under the cap.

Every host of this runtime has been seen to keep a little more VRAM after each resolution change.
OptiScaler releases everything it allocates per size (slot colours, guide crops, scale scratch,
the encoding output), so the growth is most likely inside the runtime's staging rebuild, which
OptiScaler cannot fix. The controller therefore makes at most 8 changes per session
(`DynamicScale::kMaxChanges`) and then holds its level until the option is turned off and on. Each
"AMD boundary: settings change" line in `amd_presr.log` now ends with the process's local VRAM use
and budget (`VramUsage` in `AmdBridge.cpp`), so the cost per change can be read off a session:
standing still and flipping the NR resolution slider back and forth gives the cleanest numbers,
since the game's own streaming moves the total too.
`tests/amd_dynamic_scale.cpp` covers the controller and runs first in
`tools/test-amd-host-contracts.cmd`.
Exercised once in Cyberpunk 2077 in the "after" placement: it stepped down every 12 s as designed,
never found the headroom to step up, and the session ended cleanly.

**Neural pass meter (AMD backend).** The AMD section of the menu shows "Neural pass: N ms per
frame (M fps on its own)". `AmdBridge::Run` wraps `Backend::Record` in a `GpuTime_Dx12` pair, the
same timestamp helper the NVIDIA path and the upscalers use, so the number is the GPU time of
everything Record puts on the game's list for all passes: input copies, the wait for the model and
the applied result. Readings under 0.1 ms are dropped because a refused Record records nothing, and
the rest is smoothed (10% per frame) so the text stays readable. The fps is 1000 divided by that
time, the rate the model alone could sustain.

**Tabbed main menu.** The two-column table (`RenderMainMenuTable`) became `RenderMainMenuTabs` in
`menu_common.cpp`: Neural, Upscaling, Frame Gen, Image, Interface and Advanced, each tab a list of
the same section functions the table called. Above the tabs, `RenderMainMenuStatusPills` shows one
pill per route into OptiScaler (nvngx.dll or nvngx_dlss/dlssd on DLSS-capable GPUs, nvngx
replacement, libxess, FSR hooks, FSR 3.1, SR and FG), green when present; the "Exists / Doesn't
Exist" lines of the no-upscaler message were removed in its favour. The theme keeps unselected tabs
neutral and the selected one in the accent colour, and the DLSS-NR header opens by default since it
fills the Neural tab. The window still auto-resizes, so its width is the widest of the pill row and
the open tab.

**Tools and packaging after the flatten.** The dlss5 scripts still pointed into the vendored
`OptiScaler-DLSSNR-PreSR-Multipass-main/` folder and at a Visual Studio BuildTools install at a
fixed path. The tests include headers from `OptiScaler/` directly now, the `.cmd` scripts find
Visual Studio through `vswhere`, and `tools/build-release-local.cmd` (all regression tests, then a
Release build into `exports/release-local/`) passes end to end. `tools/PACKAGE_RELEASE.ps1` reads
from the repository root, packages whichever of `x64/Release/a/OptiScaler.dll` and
`exports/release-local/OptiScaler.dll` is newer, adds `amd_fidelityfx_denoiser_dx12.dll` for
FSR-RR, and ships the root `README.md`, which gained an "Installing the release" section. Its
existing tripwire still refuses the danielblnc runtime (`version.dll`, `version-<ver>.dll`, `dlssnr_amd_pass*.dll`,
`dlssnr_on_amd_setup.exe`), the weights and any `nvngx*.dll`, in the staged folder and again in
the finished zip, so users supply those themselves as the README explains.

**FidelityFX modules load from the OptiScaler folder (0.1.1).** The package keeps
`amd_fidelityfx_upscaler_dx12.dll`, `..._framegeneration_dx12.dll` and `..._denoiser_dx12.dll` in
`OptiScaler/`, next to the loader. burak113's `LoadFfxModuleDx12` (in `FfxApi_Proxy.h`) loaded
these by bare name, which only searches the game folder, while the base and dlss5 lineages went
through `Util::LoadProxyLibrary` with the Opti dll path. The merge took burak's version, so in
0.1.0 none of the three loaded: FSR fell back to FSR 2, and on a non-DLSS GPU
`NVNGX_Parameter.cpp` reports `SuperSamplingDenoising.Available` only when the upscaler and the
denoiser are both ready, so the game greyed out Ray Reconstruction. The function now uses
`LoadProxyLibrary` too: Opti dll path first, then by name. Confirmed in Cyberpunk 2077 from a
clean install of the package: all three load from `OptiScaler/`, FSR 3.x runs, and FSR-RR (FSR
Ray Regeneration 1.2.0) creates its context. `amd_fidelityfx_radiancecache_dx12.dll` is not
shipped, so the log carries a harmless "Can't find" warning for it.

**"Upscaler failed to run!" on preset changes.** `FSRDFeatureDx12::UpdateSize` refuses a
frame and requests a rebuild when the render size exceeds the allocation ceiling the
context was created with — a planned bail-out, not a failure, but it raised an error
toast. Now suppressed when the feature itself set `changeBackend`. The underlying churn
was also removed: the ceiling is rounded up to a multiple of 8, because titles and
Streamline round the same ratio differently (1129x635 created vs 1130x636 dispatched
forced a full rebuild — and a temporal history reset — on every preset change).

**Menu honesty pass.** The AMD section had fixed text claiming "before Super Resolution"
whichever placement ran, and the switch that chose it lived in a branch the AMD backend
never reaches (the section returns early). Now: a single **Processing point** combo
(before / after), one scale slider bound to whichever placement is live, a warning when
a Ray Reconstruction title makes the choice moot, the pass slider capped at 3, and the
NVIDIA-chain-only controls (pass-limit lift, per-pass overrides, the second after-RR
pass counter) hidden while the AMD backend is running.

**Diagnostics.** A one-shot probe logging the bound normals resource, its format, the
resolved roughness source, and whether the Streamline `NormalRoughness` tag was
available. Also `[FSR-RR] TaggedNormalRoughness` (**default off**, see section 7).

**Packaging.** `tools/PACKAGE_RELEASE.ps1` rewrites the whole `[DlssNr]` section when
building a release, so it did not know the post-RR keys. `RRWorkingScale` and `RRPasses`
were added there; `Enabled=false` is unchanged.

### 0.2.0

**Second NR runtime: lmxxf.** Ported from TheAutomatic's `release/1.9.0` up to `c127e04b`
(tag `v1.9.1-alpha`, lmxxf upstream 0.29). The upstream folder
`OptiScaler-DLSSNR-PreSR-Multipass-main/` maps to this repo's root; `tools/`, `tests/` and
`third_party/` stay at the root. The next sync diffs from `c127e04b`. Their lmxxf files
(`dlssnr/backend/`, `dlssnr/submission/`, `lmxxf_runtime/`) are best taken whole from upstream,
then re-apply the local changes (`LmxxfBackend::Record` refuses `afterUpscale` frames; the
comments in `backend/Kind.h` and both folders' `README.md` describe this repository) and run
clang-format. `third_party/lmxxf/` is vendored MIT source plus gfx1201 modules;
`tools/build-lmxxf-runtime.cmd exports\lmxxf-runtime` builds `LmxxfNrRuntime.dll`, and the
packager ships it with the modules and the top-level `*.hlsl`. The weights
(`native-game-tiled-assets\`) are never shipped.

**Choosing the runtime.** `[DlssNr] NrBackend` (`daniel`, `lmxxf`, `off`, `auto`). The menu's
"NR runtime" combo appears when both runtimes are present and applies on the next launch:
`Backend::Selector` latches the value read at startup, because each backend installs its own
D3D12 hooks when the device is created. `NrBackend`/`LmxxfDiagnostic` used to be skipped on
save whenever they equalled the default, so switching back to daniel never reached the ini;
they are now written as `auto`.

**lmxxf menu.** Its own block: runtime label, neural pass meter, Detail/Colour strength, Debug
view. Everything else in the AMD section belongs to the danielblnc runtime and is hidden. The
meter works on lmxxf because the two timestamps land on either side of the list split, around
the HIP work.

**Effect strength and colour grade (danielblnc).** The runtime's `[DlssNrOnAmd] Scale`
(0.3.1 RVA `0x9ad14`, 0.4.0 RVA `0xa8624`, default 4/128) scales how much of the network reaches
the frame: 0 leaves the frame untouched. It sits where NVIDIA's `style/128` would be but is not the
style; tested in Cyberpunk, 0/128 did nothing and 1/128 and 2/128 were weak. `AmdEffectStrength`
(0 to 1 of 4/128, layout field `scale`, 0.3.1 and 0.4.0). `AmdColourGrade` applies NVIDIA's
post-network grade for Model B (natural: -0.10 EV, contrast -0.25, saturation -10%) or C
(cinematic: saturation -15%) in the AmdLook shader, at full strength. NVIDIA's real style input
(network parameter `+0x94` in `nvngx_dlssnr.dll`) has no slot in 0.3.1; 0.3.3 added one
(`[DlssNrOnAmd] Style`, see "danielblnc runtime 0.4.0").

**Logging.** `slEvaluateFeature` logged at Info once per frame (5 MB in 17 minutes); it is Debug
now.

**XeSS multi frame generation on AMD.** Ported from
[Coldwood1026/OptiScalerDp4aUnlock](https://github.com/Coldwood1026/OptiScalerDp4aUnlock), a fork
of upstream at `5ee53e38`, which is already in this history: `git diff 5ee53e38 coldwood/master --
OptiScaler OptiScaler.ini`, then `git apply -3`. `XeFGUnlock.h` patches `libxess_fg.dll` so it
reports and accepts more than one interpolated frame, `XeLLUnLock.h` raises the frame count
`libxell.dll` accepts in `xellSetGeneratedFramesCount`, and `XeFGPacing.h` spaces out each
generated frame above 2X. All three patch the loaded image, check every byte first and roll back
on a mismatch. They only know `libxess_fg` build `0x69CB0F4D` and `libxell` 1.3.2 (`0x6A561284`),
the ones in `external/xess`: a newer XeSS SDK there needs new RVAs. The game is told the real
ceiling, and the menu's Auto follows the game's DLSSG multiplier. Kept from our side in the
merge: the generated labels of "Override DLSSG Ratio" (theirs went back to fixed arrays, which
overflow past 6X) and the struct-version guard on `numFramesToGenerateMax`. Their Intel
`ExtraPacing` default had been inserted between an `if` and its `else if` in `getGpuInfo`; it now
sits before the chain.

One addition of ours. `libxess_fg` has no static `libxell` import: it calls
`GetModuleHandleExA("libxell.dll")` and gets the first module of that name. Cyberpunk loads its
own 1.1 before OptiScaler starts, and that copy lacks `xellSetGeneratedFramesCount` and
`xellSetDisplayInfo`, so XeFG refused OptiScaler's XeLL context ("XeLL context is not supported";
the missing export is only logged at debug level). `RedirectAllExports` cannot cover exports the
old copy does not have, and loading ours first does not help either, since the game's copy is
already there. `XeFGProxy::PointXeLLLookupAtOurs` replaces that import in our `libxess_fg` with a
lookup that returns OptiScaler's `libxell`. Tested in Cyberpunk: 2X to 8X switched live, Auto
followed the game. Above 4X it needs VSync or a frame rate cap.

### 0.3.0

**lmxxf multipass.** `[DlssNr] Passes` (1 to 3, the key the danielblnc runtime already reads)
now reaches lmxxf through `LmxxfNrFrameInfo::passes`, and the lmxxf block of the menu has its own
Passes slider. The extra passes stay inside the HIP work between the two halves of the game's
list: `D3D12Bridge::Enqueue` runs the network, copies its RGB output into the RGBA input
(`hipMemcpy2DAsync`, 12 to 16 bytes per pixel, alpha kept) and runs it again. The D3D12 side is
unchanged: one cut, one fence wait, one decode. Decode compares the last output with the original
proxy, so the effect compounds. The network keeps no state between calls in the product (no
history, fixed seed, adaptive ViT off), which is why one session serves every pass where
danielblnc needs one module per pass.

Measured with a scratch harness on the development RX 9070 XT, 1920x1080 input (1080p network
tier), after the first ~20 frames of GPU warm-up: HIP span 18.5 ms for 1 pass, 36.5 ms for 2,
55 ms for 3. The copy between passes does not show in the numbers. The output moved by a mean of
0.009 (max 0.08) from 1 to 2 passes and about the same from 2 to 3. The runtime still accepts a
`LmxxfNrFrameInfo` without the field (80 bytes) and the 64-byte legacy one.

**lmxxf temporal history per pass.** With several passes the residual flickered in motion: the
product ran lmxxf without history, so each frame's answer stood alone and every extra pass fed that
variation back in. `[DlssNr] LmxxfTemporal` (default on, "Temporal history" in the menu) gives each
pass its own output from the previous frame, warped by the game's motion vectors, as the network's
history input. It is upstream's own temporal path (`src/native_temporal_{feed,coordinates,sample}.h`,
now vendored, fast variant), which upstream runs for one pass in its game host; the runtime keeps
one feed and sampler per pass, and the bridge keeps every pass's output for the next frame. The
motion vectors, their scale (NGX = FFX convention) and the reset flag cross the ABI in
`LmxxfNrFrameInfo`; history is dropped on reset, on a frame without NR (the host counts every
Evaluate in `frame_id`), after 250 ms without a frame, on a zeroed output and on a geometry change.

Measured with the scratch harness (1080p tier, residual = output minus input, mean change from one
frame to the next): with a quarter-pixel jitter on a still image, 0.0101 to 0.0082 at 1 pass,
0.0131 to 0.0106 at 2, 0.0130 to 0.0118 at 3. Panning 2 px per frame: 0.0074 to 0.0069 at 1 pass,
about even at 2 and 3; flipping the vectors' sign made it 17 to 45% worse, which confirms the sign
and scale. On a still image without jitter the history loop itself moves the residual by about
0.004 per frame, which is what upstream's `DLSS5_OUTPUT_SMOOTH` is for (below). In Cyberpunk the
history alone already made the effect visibly steadier, one pass included. Cost at the 1080p tier: about 0.5 to 1.5 ms per frame; VRAM of about 70 MB per pass for
the history and its warp, 60 MB more for each pass after the first (its bridge copy and the kept
output), and 50 MB for the motion and coordinate buffers.

**lmxxf output smoothing.** Upstream's `DLSS5_OUTPUT_SMOOTH` (`native_output_smooth.hlsl`, the
host class `OutputSmooth` in `LmxxfNrRuntime.cpp`): where the last pass's output differs from its
warped history by less than the threshold, it is blended toward it, by the strength at no
difference. It runs in the consumer half before the output is shown or kept as history, so the
blend is recursive, as upstream has it. `[DlssNr] LmxxfSmoothStrength` (default 0.8, 0 off) and
`LmxxfSmoothThreshold` (default 10, in 1/255) are upstream's production values; both reach the
runtime in `LmxxfNrFrameInfo` and apply live. Only the last pass is smoothed: the passes before it
feed each other inside one HIP enqueue, where there is no D3D12 work. Same harness, threshold 10,
strength 0.8, frame-to-frame residual change: still image 0.0043 to 0.0009, panning 0.0069 to
0.0020 at 1 pass and 0.0097 to 0.0036 at 3, quarter-pixel jitter 0.0083 to 0.0070 at 1 pass and
0.0120 to 0.0094 at 3. The residual's size stays the same, and the cost does not show.

**lmxxf detail and colour strength up to 2.** `DlssNrTransferStrength` and `DlssNrColourStrength`
were held to 0..1 on lmxxf (menu, host, runtime and upstream's codec). They now reach 2, which the
danielblnc page already allowed for its structure. In the decode shader both are blends between the
original and the model's result, so above 1 they extrapolate: at 2 the model's edit is twice its
size. The final colour is then clamped to the shader's own `ClampAp1` gamut and to zero, so an
extrapolation cannot turn negative; up to 1 nothing changes (`UPSTREAM.md`, Patch E). In the harness,
with both at 2 the residual was about four times its size at 1, and so was its frame-to-frame change.

**Red theme.** The menu's default accent (`[Menu] AccentColorR/G/B`, `Config.h`) is red, 0.86 /
0.09 / 0.12, where it was blue (0.00 / 0.40 / 0.77); the "Red" preset uses the same value and "Blue"
still gives the old look. The dark theme's greys (backgrounds, text, borders, popups and dimming)
lost their slight blue tint. The light theme is unchanged.

**Neural tab in sections.** With an AMD runtime the tab keeps Enable NR, the runtime and the neural
pass meter at the top, then one column of titled sections: for danielblnc Processing (placement,
resolution, dynamic resolution), Scheduling (temporal stabilization, slots, wait mode), Effect and
Colour (grade, encoding, the appearance filter and the RTGI experiment); for lmxxf Temporal, Effect
and Inspect. A two-column version was tried and dropped. The NVIDIA-chain page is unchanged.

### 0.4.0

**Third NR runtime: mochizuki.**
[mochizuki0323/DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD) (MIT, v0.0.1, commit
`791b046`, vendored under `third_party/mochizuki/`) reimplements the NR network of NVIDIA's
`nvngx_dlssnr.dll` 310.8.0 as Vulkan compute shaders on FP8 (`VK_KHR_cooperative_matrix`,
`VK_EXT_shader_float8`), so it needs RDNA4. Upstream's own host asks vkd3d-proton for the game's
Vulkan device, and a native D3D12 game has none. `MochizukiNrRuntime.dll`
(`dlssnr/backend/mochizuki_runtime/`) creates a Vulkan device of its own on the game's adapter
(matched by LUID) and runs the network between the two halves of the game's command list, with the
list cut lmxxf already uses. Colour, motion vectors and the result cross in shared D3D12 buffers
(buffers, not textures, so the two APIs never have to agree on a tiling), and two shared D3D12
fences, imported as Vulkan timeline semaphores, order the work, one per direction. The DLL
implements the lmxxf C ABI (`LmxxfNrApi.h`) plus four exports of its own (`MochizukiNrControls.h`),
so `LmxxfBackend` hosts it with only the DLL name changed; its `PrepareSession` runs inside
`ScopedSkipVulkanHooks` and `ScopedCreatingD3DDevice`. `[DlssNr] NrBackend=mochizuki` selects it
(`auto` never does). It runs before Super Resolution only and ignores `ApplyAfterRR`. The model's
temporal history follows the game's motion vectors (2- and 4-channel float formats); in a harness
pan of 2 px per frame the effect changed 73% less from frame to frame than without history, and more
with the vectors' sign flipped. Beside the DLL: `dlssnr-amd\dlssnr.bin` (the model),
`dlssnr-amd\shaders\` (39 network, 4 temporal and 6 runtime pipelines),
`dlssnr-amd\prewarm\manifest.txt`, `dlssnr-amd\pipeline.cache` and `mochizuki_nr.log`.

`tools\build-mochizuki-runtime.cmd <out>` builds the DLL and its shaders (MSVC, Vulkan SDK 1.4.357
or newer, Python; glslang 16.5.0 is downloaded once and its hash checked). The DLL imports only
`vulkan-1.dll` and `KERNEL32.dll`. The zip carries only the host side: the backend, the keys and the
menu. The AMD-NR installer supplies the runtime, its shaders, the prewarm manifest and the model.
The model is made from `nvngx_dlssnr.dll` 310.8.0 (`e16bcf15…`) with upstream's scripts
(`third_party/mochizuki/UPSTREAM.md`, "The model"): 599 entries, 147,756,560 bytes.

**mochizuki keys and menu.** 29 `[DlssNr] Mochizuki*` keys (`Config.h`, the packaged INI, the root
README): temporal history and its strength, detail (0 to 2) and colour (0 to 4) strength, passes
(1 to 3), model scale (0.25 to 1), the model's controls (intensity, style, local tone and structure,
skin structure, automatic mask), highlight guard, white point, linear input, apply model, dynamic
resolution, and six overrides each for passes 2 and 3. It reads none of the danielblnc, lmxxf or
NVIDIA keys; without `MochizukiTemporal`, `LmxxfTemporal` decides. Every default is the network's
own except `MochizukiColourStrength`, 0: the game's own colour at the network's luminance (1, the
runtime's default, applies the network's colour change in full). At the runtime's defaults the
output is byte-identical to the first build's in the harness. Passes, model scale and linear input
rebuild the network; the rest apply on the next frame through `MochizukiNrSetControls`, which the
host calls only when a value changes or a session is new. The Neural tab has a mochizuki section:
Temporal, Effect, Model (with a Pass 2/3 node), Advanced and Status (network size, GPU time median
and p95, last build, history use, refused motion formats, failures).

**Host fixes that reach every runtime.**

- Process exit. The hook on `RtlExitUserProcess` sets a latch first; from then on `AmdBridge::Run`
  passes the original colour to SR and nothing is enqueued between a list's halves.
  `Host::OnProcessExit` replaces the plain `Shutdown`: danielblnc still shuts down, lmxxf is
  destroyed only if its locks come free within 2 s, and mochizuki is left to the OS (`Destroy` could
  wait out a network build, and `FreeLibrary` would unmap code other threads still run). Also: an
  evaluate whose colour is already the model's answer (`DlssNrPreUpscale`, `DualFeature`) passes
  through instead of costing the history, `frameId` no longer advances on declined calls, a frame
  recorded on another adapter's device passes through, and a failing session start backs off from
  2 to 60 s.
- Command lists recycled before they run. A list reset or released without being executed left its
  pending job behind, and lmxxf or mochizuki stayed on "previous frame not submitted". The proxy now
  reports it (`Hooks::g_onListRecycled`, `CommandListProxy.h`) and `LmxxfBackend` cancels that job.
- Upscaler colour barriers. XeSS, FSR 2.2, FSR 2.1.2 and FSR 3.1 (FSR-RR included, built on it)
  applied the game's configured colour barriers to the NR replacement, which arrives in
  `NON_PIXEL_SHADER_RESOURCE` and must stay there. They skip it now; the Unreal quirk still records
  the game colour's state, which `AmdBridge` reads.
- OptiScaler's own Vulkan devices. An instance or device created while both `vulkanSkipHooks` and
  `creatingD3DDevice` are set passes straight through the Vulkan hooks: no NR extensions, no
  AntiLag probe, no `State` capture. Those are mochizuki's runtime and `IdentifyGpu`'s probe, and
  under Proton the instances and devices DXVK and vkd3d-proton create for the DXGI factories and
  D3D12 devices OptiScaler makes for itself (`with_dx12`, `VkwDx12`, the menu's GPU list). The
  game's own instances and devices, DXVK's and vkd3d-proton's included, still take the full hook.
  Wave 1 found the passthrough is not reached on native Windows D3D12, only under Proton, with
  `LoadVulkanManually`, or when an overlay loaded `vulkan-1.dll` first; it was not checked there.
- Placement of the runtimes that run before SR. `EvaluateAtSeam` ignores `ApplyAfterRR` for lmxxf
  and mochizuki. A `true` left over from danielblnc sent every frame to the seam they refuse, so NR
  stayed off.
- Settle gate. `AmdBridge::Run` no longer lets danielblnc's `AmdModelScale` and `AmdDynamicScale`
  restart mochizuki's history or pause it. With mochizuki's dynamic resolution the gate keys on the
  colour allocation, so a subrect change only restarts the history. For danielblnc and lmxxf the
  gate is unchanged.

**mochizuki runtime stability.** A failed start is torn down without leaks, and an unsupported GPU
or driver is logged once (`[unsupported]`). Only device loss is sticky: a frame whose network fails
shows the original colour (never black), and a failed build is retried, 5, 30 and 120 s after
running out of memory. VRAM is checked before a build (D3DKMT; "insufficient VRAM", retried every
10 s); the 1080p network takes about 0.7 GB. Two fences instead of one, so neither goes backwards; a
watchdog releases the game's queues after 5 s without progress on the Vulkan side; the runtime
declares `ANY_QUEUE`, so a frame from another queue waits for the last one instead of recreating the
session. Builds run on a second Vulkan queue, and a change of size no longer waits for the GPU on
the render thread (`PrepareFrame` 31.0 / 21.6 ms to 0.44 / 0.57 ms). A change of passes, model scale
or linear input keeps the old network serving until the new one is ready.
`MochizukiDynamicResolution` (`auto` by default) runs the network in a bucket, per axis the largest
render subrect seen rounded up to 64 px inside the colour texture, so a dynamic-resolution title
does not rebuild: NR ran on 300 of 300 frames of the harness's dynamic-resolution run, against 15 of
300 with `exact`. The harness's 21 fault-injection cases (`mz_stress all`) pass.

**mochizuki performance.** RX 9070 XT, driver 26.8.1, against the first build (on 0.3.0), in
interleaved pairs in one session, output byte-identical: the network's span on the game's queue went
from 10.23 to 9.39 ms at 1920x1080 (-0.86 ms, -8.4%) and from 17.32 to 16.10 ms at 2560x1440
(-1.23 ms, -7.1%); the network alone (`nr_graph --per-layer`) from 9.69 to 8.89 ms and from
15.82 to 14.65 ms. The gain is all shaders: word-wide e4m3 reads, two work-split knobs and two LLPC
codegen knobs (upstream patches 2 to 4 below). The CPU cost of a frame's enqueue stays at 0.1 ms.
For comparison, lmxxf's HIP span at 1080p, one pass, is 18.5 ms.

**Cold build and the prewarm manifest.** The AMD driver keys its pipeline cache by executable, so
the first start in each game compiles every pipeline: 54.8 s in Cyberpunk 2077 with the first build,
24.8 s in the harness. `mz_interpose.cpp` records the pipelines the core creates in
`dlssnr-amd\prewarm\manifest.txt`. With it, the runtime compiles those 32 pipelines on
min(8, cores - 2) threads at below-normal priority (`MZ_COMPILE_THREADS`; 1 turns it off) before the
core asks for them, and the network was ready in 5.8 to 6.0 s in the harness, against 21.6 s without
the manifest. `pipeline.cache` is now also written after the adapter and temporal pipelines. Without
a manifest the runtime compiles in series and writes one (no build step makes it); it must be
regenerated whenever the shaders change. Frames pass through without NR until the network is
ready. Not yet measured in a game.

**Upstream patches** (`third_party/mochizuki/UPSTREAM.md`, entries 1 to 5): the `VkImageAspectFlags`
cast MSVC needs; word views for the e4m3 weight reads and the persistent runs' stores in
`fswin_t.comp`; `fswin32` `NR_FWAVES` 1 to 2 and `fswinfusedup128` `NR_EXPAND_GROUP` 4 to 2 in
`pipelines.json`; `NR_QUAD` and `NR_V_ROW` in `fswin_t.comp`, `coopmm.glsl` and `pipelines.json`;
and `nr::build_cancel`, which stops a network build between pipelines and saves the cache first.
Patches 2 to 4 leave the output byte-identical and are candidate upstream pull requests (not checked
on RADV). Its "Build" and "Barriers and `NR_CHAIN`" sections record what was measured and not
adopted.

**Not in 0.4.0.** Held for a later release: a prefilter for the model-scale downscale
(`MochizukiModelScale` below 1 shimmers in slow pans, so it is not recommended yet), memory
residency priority, and an opt-in prewarm at session start. Open in the runtime: the persistent
queue's 16-bit item index, which above about 21.8 MP of input (8K class) stalls a frame for about
0.5 s with a wrong picture; depth input; a placement after SR. The danielblnc and lmxxf runtimes are
unchanged, and so is what the zip ships besides `OptiScaler.dll`, the INI and the README. The
in-game checks still pending are in `handoff/mochizuki-backend.md`, section 11.7, which has the
whole record of the mochizuki work in Portuguese: architecture, measurements, tools, risks and open
items.

### danielblnc runtime 0.4.0

Released in `v0.4.1-amd-nr`, after an in-game A/B against 0.3.1 in Cyberpunk 2077. The host accepts
[DLSS-NR-on-AMD v0.4.0](https://github.com/danielblnc/DLSS-NR-on-AMD/releases/tag/v0.4.0) next to
0.3.1 and 0.3.0: `kAmd040` in `AmdLayout.h` (SHA `d62be3d8…`, 10,027,008 bytes) and its bootstrap
entry in `RuntimeHostLoad.h` (DllMain `0x5980` calls CreateThread at `0x727d`; the thread start
`0x8d20` has the same prologue). 0.3.3 is not accepted.

**Where the DLL comes from.** Since 0.3.3 the setup is a Tauri app that keeps `version.dll` in its
own `.rdata` instead of appending it (0.4.0: file offset `0x2609c7`, `0x990000` bytes, written out
by the setup's `lea r8` at RVA `0x4de3f` and `mov r9d` at `0x4de46`). `install-amd-presr.ps1`
(`Get-EmbeddedRuntime`) takes it out by its PE headers and keeps it only if its SHA is a known
runtime, without running the setup; it still runs the setup when the weights are missing. The same
code finds the DLL appended to the 0.3.0 and 0.3.1 setups. A setup that is present is always
opened, even when a runtime was already found, and the newest known runtime wins (0.4.0 > 0.3.1 >
0.3.0): an update from 0.3.1, where the old `version.dll` an earlier install saved next to
`Setup.bat` or the old pass1 in the game is still around, installs the setup's 0.4.0. The same
rule picks among the `version.dll` / pass1 candidates. The chosen version is printed by name, an
older setup or candidate is reported and left alone, and `-AuthorDll` still installs exactly the
file given (with a warning if the setup holds a newer one). A saved `version.dll` next to
`Setup.bat` that is an older known runtime is renamed `version-<ver>.dll` and replaced by the one
installed; staging a runtime out of the game folder never overwrites a `version.dll` there.

**How the table was mapped.** From 0.3.1, outside this repo in
`daniel-runtime/analysis-opti` (start with `verify_kAmd040.txt`). Every field has at least two
independent derivations that agree: unique instruction windows and a whole-program data
map for all of them, difflib-aligned function pairs with a byte-window recheck for the code and
engine fields, and the `[DlssNrOnAmd]` reader's key-to-store pairs for the options. The traps were
`charMask` (the window vote said `0xa841c`, which is SpinDraw; the reader stores UseAutoMask at
`0xa8628`) and `graphicsPso`, which is SpinDraw+0x14 now because 0.4.0 put `PollSpacing`
(`0xa8420`, a root constant both wait shaders ignore) after SpinDraw. The host code the runtime
exposes is 0.3.1's apart from data RVAs, which moved in pieces (+0xd7d8 to +0xd920), and the
engine object's own offsets (historyView, historyValid and initDone at engine+0x148, +0x150 and
+0x438), which the host never derives. `tests/amd_layout_binary_check.py` checks every field
of the 0.3.0, 0.3.1 and 0.4.0 tables against its binary: `.pdata` starts for the code entries,
the wait helper's four Dispatch returns in order, Record's three gates, Notify's call through the
trampoline, the INI key stores for the options, and for every other data field one or two pinned
instruction sites (opcode bytes around the disp32, the function they sit in, and for `engine`,
`watchdog` and `recordLock+0x4c` the call that follows), plus `.data` membership and the bootstrap
bytes. A header-level check fails when a non-zero data field of any table but 0.2.17 (no binary
here) has none of these. A mutation run moved every non-zero member of the three tables by
-8..+0x10 and swapped neighbours, 1218 cases, and every one failed the check
(`daniel-runtime/analysis-opti/implement/anchors/mutate.py`). `tools/test-amd-host-contracts.cmd`
runs it on the folder in `AMD_RUNTIME_DIR`, or on the headers alone.

**Checked without a game.** `tools/test-amd-runtime-init.cmd <version.dll> <weights>` loads the
real runtime as `InitPass` does and runs 1080p frames through Record, ExecuteCommandLists, Notify
and a fence in both wait modes. On an RX 9070 XT, 0.4.0: bootstrap suppressed on the real DllMain,
init true in about 1 s, 48 of 48 frames recorded and completed in each mode with no timeout,
`graphicsPso` and `predicateReady` set in the draw wait, shutdown and process exit clean. 15.2 ms
per frame from submit to fence against 27.9 ms for 0.3.1 through the same harness. The inputs are
blank, so this says nothing about the picture.

**What changed in the runtime.** The speed-up is in the HIP kernels: register-resident Swin and
ViT kernels that run only on `gfx12` (RDNA4; `DLSSNR_NO_REG` turns them off). RDNA3 gets the
0.3.3 kernels. A persistent "chain" kernel is opt-in through `DLSSNR_CHAIN`; its flag waits give
up after 100 ms and mark the frame untrustworthy, so the host must never set it. 0.3.3 added four
`[DlssNrOnAmd]` options: `Style` (0 to 2, fed to the network as style/128: NVIDIA's style input,
which 0.3.1 had no slot for), `ToneCurve` (Reinhard or ACES), `ToneLift` (0 to 0.25) and
`UseGameExposure` (default 1). DllMain reads them from `dlssnr_on_amd.ini`; the only later reread is
in the runtime's own Present detour, which the isolated bootstrap never installs. `InitPass` pins
them to the runtime defaults (0, Reinhard, 0, 1), which give 0.3.1's behaviour, so an ini left in
the game folder cannot change the image. `ToneChannels` now gates only the structure input and
tone always reaches the network; AmdBridge already sends tone 0 whenever ToneChannels is off. The
residual-apply shader grew (ACES, lift, and it keeps negative residuals that 0.3.1 clamped), so the
default image may differ slightly from 0.3.1.

**Weights.** Unchanged: the same `dlssnr_on_amd_weights.bin` (147,689,451 bytes, SHA
`6bf8dc93…`). The loader (`init`, `0x26110`) is 0.3.1's `0x21720` with one engine offset moved, and
the converter's 153-tensor table is identical, so a file made by any 0.3.x setup works.

**Open.** An A/B in a game against 0.3.1 (picture and frame time). Whether to drive `Style` from
`DlssNrStyle`, which needs that comparison first. The AMD-NR installer still installs 0.3.1.

### danielblnc runtime 0.4.1

Released in `v0.4.2-amd-nr`, after an in-game test in Cyberpunk 2077.
[DLSS-NR-on-AMD v0.4.1](https://github.com/danielblnc/DLSS-NR-on-AMD/releases/tag/v0.4.1) is
accepted next to 0.4.0, 0.3.1 and 0.3.0: `kAmd041` in `AmdLayout.h` (SHA `823063eb…`, 9,916,928
bytes, in the setup's `.rdata` at file offset `0x2609c7`) and its bootstrap entry in
`RuntimeHostLoad.h` (DllMain `0x58c0` calls CreateThread at `0x71bd`; worker `0x8c90`).
`install-amd-presr.ps1` knows the hash and ranks it newest.

**How the table was mapped.** From 0.4.0, in `daniel-runtime/analysis-opti`: instruction windows
(`map_layout_040_041.txt`) and the aligned-function data map (`datamap_040_041.txt`) agree on every
field. Init, Notify, shutdown, the wait helper, DllMain and the bootstrap worker are 0.4.0's
instruction for instruction. Record gained the QueuePriority block and some engine members moved
+0x18; the host uses none of those offsets. The data block moved +0x2020 up to `historyValid` and
+0x2038 from `initDone` on. The 0.4.1 rows of `ANCHORS` are 0.4.0's sites carried through the same
alignment (`implement/anchors/derive_041.py`), and `amd_layout_binary_check.py` passes on all four
binaries.

**What changed in the runtime.** One new `[DlssNrOnAmd]` key, `QueuePriority` (default 1): Record
creates the HIP stream with `hipStreamCreateWithPriority` at the highest priority instead of the
null stream. DllMain reads it, so with no ini the host gets the default; the host does not pin it.
The weights are unchanged (the loader is 0.4.0's).

**Checked without a game.** `tools/test-amd-runtime-init.cmd` on an RX 9070 XT: init true in about
1 s, 64 of 64 frames in both wait modes with no timeout. Three alternating runs: 0.4.1 14.4-14.6 ms
per frame, 0.4.0 15.1-15.3 ms. The harness keeps the GPU otherwise idle, so it does not show what
the priority stream buys under a game's load.


### danielblnc runtime 0.4.2 and 0.4.3 (supporter builds)

Released in `v0.4.3-amd-nr`. danielblnc gives these early builds to his supporters; this project does not
ship them, and neither does the installer's payload. The host accepts them when the user brings
one: `kAmd042` (SHA `8aa2dcc5…`, 12,981,760 bytes) and `kAmd043` (SHA `d1e32086…`, 12,749,824
bytes) in `AmdLayout.h`, with their bootstrap entries in `RuntimeHostLoad.h` (DllMain calls
CreateThread at `0x715d` in both; worker `0x8cc0` and `0x90a0`). `install-amd-presr.ps1` knows both
hashes, ranks them above 0.4.1 and takes them out of their setups like the others; it still
recommends 0.4.1 to everyone else.

**One file is enough.** A runtime keeps its state in its own module, so each pass needs its own
file. `InitPass` makes `dlssnr_amd_pass2.dll` and `pass3.dll` as copies of `pass1.dll` when they are
missing or hold another runtime (logs "pass N copied from pass 1"), so installing a runtime by hand
means replacing `dlssnr_amd_pass1.dll` alone.

**How the tables were mapped.** From 0.4.1, with the same two methods (`map_layout_041_042.txt`,
`map_layout_041_043.txt`, `datamap_041_042.txt`, `datamap_041_043.txt`); they agree on every field.
Init, shutdown, the wait helper and the bootstrap worker are 0.4.1's instruction for instruction;
Record, Notify and 0.4.3's Init only moved engine members. `graphicsWaitEnd` is the helper's
`.pdata` end, as before. The `ANCHORS` rows come from `implement/anchors/derive_042_043.py`.

**What changed in the runtime.** New kernels (the DLL grew by 3 MB) and three `[DlssNrOnAmd]`
keys: `NoiseHandoff` (default 0) and `Quality` (default `fast`) in 0.4.2, `OverlayKey` (default
`End`) in 0.4.3. The weights are the same as 0.4.1's.

**Fast and quality.** `Quality` is a byte (`quality` in the layout: `0xaf84d`, `0xb19c5`), 1 = fast
(the runtime's default: cheaper arithmetic, RX 9000 only; on other GPUs only the colour conversion
changes), 0 = reference (NVIDIA's arithmetic). The runtime's per-job function copies it into the
engine every job and logs `quality: ...` in `dlssnr_on_amd.log` when it changes; the network then
re-uploads the flag and replans, so it can switch while the game runs. The host writes it before
each Record from `[DlssNr] AmdQuality` (0 fast, 1 quality; menu "DLSS 5 mode"), so an ini left in
the game folder does not decide it. Smoke test, 0.4.3, dispatch wait: reference 12.6 ms, fast 10.8
ms per frame. The other two keys are left at the runtime's defaults.

**Checked without a game.** `amd_layout_binary_check.py` passes on 0.3.1, 0.4.0, 0.4.1, 0.4.2 and
0.4.3; the synthetic bootstrap fixtures pass for both contracts; `tools/test-amd-runtime-init.cmd` on
an RX 9070 XT passes for both, 24 of 24 frames per wait mode with no timeout. Dispatch wait per
frame, same harness: 0.4.1 14.7 ms, 0.4.2 12.3 ms, 0.4.3 10.9 ms.

### 0.4.4

Released in `v0.4.4-amd-nr`. The mochizuki runtime is faster and rounds its input the way NVIDIA's network does;
`Setup.bat` stops forcing Linear with the history off on danielblnc; two temporal stabilizers are in,
both off by default; danielblnc's 0.4.2 is public and the recommended runtime; and the host accepts
danielblnc's 0.5.0 supporter build. Every existing default in `Config.h` and the packaged INI is as in `v0.4.3-amd-nr`.

**mochizuki runtime speed.** Nine changes to the mochizuki runtime, each taken from
upstream's `linux/` tree at v0.0.2 (`4f62a8a`) or v0.0.2.1 (`4f663b5`) and kept only where the
output stayed byte-identical and LLPC ran it faster. `third_party/mochizuki/UPSTREAM.md`, entries 6
to 14, has each one with its own measurements, and what was tried and left out.

**What changed.** In the network:
- `gemm1x1.comp` and `ffwd3_t.comp` are upstream's v0.0.2.1 files, with `NR_QKV_NO_BCAST` on
  `gemmvqkvnorm` and `NR_POOL_FRAG` on `gemmpool`. The pool's sums are `precise`, which LLPC needs to
  keep the output.
- `ffwd3` takes its tiles group-major, and a new `ffwd3w` takes two tiles a subgroup for layers of
  2560 tokens and more (1440p and up).
- The attention sources are upstream's too, with `NR_ATTN_VFRAG`, `NR_ATTN_QKSWAP` and `NR_ATTN_EDGE`
  on `attn` and `NR_VOUT_VEC` on `vitattn`.
- The C=128 persistent runs take in their downsample and upsample layers (two new pipelines), and the
  C=256 upsample run launches one workgroup per item at 1080p and 720p.
- `shader-constants.txt` records three new keys, `ffwd_gmajor`, `ffwd_fm2_min` and `persist_one`.

Around the network:
- With one pass the temporal history alternates between two images, so the copy of the history
  each frame is gone.
- On the linear path at model scale 1 and one pass (FP16, 11/11/10 and RGBA32F frames) the host
  uploads the frame straight into the core's image in the frame's own format; there is no RGBA32F
  copy of it any more.
- The alpha pass runs only for the control mask, native compose and 8-bit frames, where it changes
  the result.

All of it is in `MochizukiNrRuntime.dll` and its shaders.

**Measured.** RX 9070 XT, driver 26.8.1, against the runtime built from `5fead302`, interleaved in
one session:
- The network's span on the game's queue (`mz_timing`, median of 7 pairs): from 9.641 to 9.158 ms
  at 1920x1080 (-0.577 ms, -6.0%) and from 16.003 to 14.903 ms at 2560x1440 (-1.099 ms, -6.9%).
  An independent check, rebuilt from the patch, measured -0.493 and -0.484 ms at 1920x1080 and -1.096
  and -1.031 ms at 2560x1440 in two more sessions (7 pairs each, all 28 pairs faster), so about -5 to
  -6% at 1080p and -6.5 to -6.9% at 1440p.
- The network alone (`nr_graph --per-layer`, 3 runs): 10.396 -> 9.872 ms and 15.127 -> 14.780 ms.
  Per pipeline: `gemmvqkvnorm` -0.145 / -0.087 ms, `attn` -0.065 / -0.052, `vitattn` -0.069 /
  -0.023, the C=128 runs -0.057 / -0.051 (two pipelines where there were three), `fswinpup256` -0.053
  at 1080p, `ffwd3` -0.019 at 1080p. At 1440p `ffwd3w`'s own time is level with `ffwd3`'s, and
  `gemmproj`, which reads its output, is 0.262 ms faster. Pipelines no change touches moved by -0.021
  to +0.018 ms, this session's noise.
- `nr_graph` does not run the three changes around the network. Measured one at a time, their gains
  add up to -0.27 ms at 1080p and -0.61 ms at 1440p.
- Output: M0's equivalence set against the goldens, `nr_graph --out-image` at 1080p, 1440p and
  3840x2160, and runtime dumps against the previous build (2 and 3 passes, model scale 0.5, no
  motion, strength, a varying input alpha, RGBA32F and R10G10B10A2 frames, odd sizes, dynamic
  resolution auto and always, 3840x2160): all byte-identical. The input rounding below then changes
  the output on purpose.
- `mz_stress`: `all` 21/21, and 22/22 under dynamic resolution auto and always. `oombuild` still
  misses its 7 s limit when other work loads the CPU, for this build and the previous one alike.
- Cold build at 1080p: 22.7 and 27.7 s against 22.5 and 27.2 s without the prewarm manifest, 8.0,
  7.4 and 6.6 s against 7.6, 7.0 and 6.6 s with it.
- VRAM while the network is built: +16 MB at 1080p (698 -> 714 MB), +28 MB at 1440p, +64 MB at
  3840x2160.

**To ship it.** The shader folder has 42 network, 4 temporal and 7 runtime pipelines (39, 4 and 6
before). New: `g_ffwd3w.spv`, `g_fswinpds128.spv`, `g_fswinpup128.spv` and
`runtime/runtime_encode_in.spv`. Changed: `g_attn`, `g_vitattn`, `g_ffwd3`, `g_gemmpool`,
`g_gemmvqkvnorm`, `g_fswinpup256` and both `shader-constants.txt`. The installer's `payload.json`
lists every SPV by name, so it needs the new files and hashes, or a network of 2560 tokens or more
(1440p and up) fails to build without `ffwd3w`. The prewarm manifest must be regenerated for the new
shaders (31 pipelines from a cold 1080p run; a 1440p session adds `ffwd3w` to it). The standalone
`nr_graph.exe` of the harness must be rebuilt, since the old one refuses the new keys. Not yet
checked in a game.

**mochizuki pre-block input rounding.** The fused pre block converts its colour sample to half precision.
Taken straight from the texture, LLPC folds that conversion into a 16-bit texture return, which truncates
an f32 texel, so about half the colour samples came in one half-precision step low. NVIDIA's network rounds
to nearest even (`cvt.rn.f16.f32`). `image_input.glsl` now rounds the sample's bits to 10 mantissa bits
with integer ops before the conversion (exact for every f32 from 2^-14 to 65504, checked on the CPU against
numpy). Upstream's own form, an RTE execution mode, does not stop the fold on LLPC and costs 4 VGPRs. Only
`g_fswinimagepreds32.spv` and `temporal/temporal_pre_fp32.spv` change. The output moves by a mean of 1.1e-3
to 4.8e-3 against M0's goldens, spread over the frame; `mz_pan` over 200 frames with correct vectors: the
frame-to-frame change of the effect 0.00140 -> 0.00132, the other cases level. Cost: +0.001 ms
(`third_party/mochizuki/UPSTREAM.md`, entry 15).

**Setup.bat leaves encoding and temporal history to the defaults (danielblnc).**
`tools/install-amd-presr.ps1`, which the package ships as `Setup.ps1` behind `Setup.bat`, upserted
`AmdEncoding = 0` and `AmdEveryFrame = true` into the game's `[DlssNr]` on every install and upgrade.
Both came in with the TheAutomatic port (`a3d517e5`), and every release from `v0.2.0-amd-nr` to
`v0.4.3-amd-nr` carries them. So every game set up or upgraded through `Setup.bat` ran danielblnc
with Linear encoding and the model's temporal history off, while `Config.h`, the menu and the
packaged INI default to sRGB (`AmdEncoding` 2) with history on (`AmdEveryFrame` false). The upsert
now writes `auto` for both keys. `Config.cpp` reads `auto` like a missing key (`readString` returns
nothing for it and `readInt` and `readBool` pass that on), so the values come from `Config.h`, and a
later change of either default reaches upgraded games without touching the script. The other
upserted keys are unchanged. lmxxf and mochizuki read neither key. The AMD-NR ReShade Installer does
not run `Setup.bat` (outside two of its handoff notes, nothing in that repository names it,
`install-amd-presr` or either key), so its installs are not affected.

A game set up by an older `Setup.bat` keeps `0` and `true` until Setup runs again. To switch
sooner: Ins menu, danielblnc section, set Encoding to sRGB and untick "Disable temporal
stabilization". Either change restarts the model's history.

Measured with the stability harness of `exports\mochizuki-work` (full table and commands in
`night-stab\METRICS.txt` and `INTEGRATION.txt`): the eight synthetic sequences of `stab\seq` played
through OptiScaler's own `AmdPreSr.cpp` (`daniel_stab_run.exe` built from this tree) and scored by
`stab_metrics.py`, from what the old script set (Linear, history off) to what the new one leaves
(sRGB, history on). Lower boil and flicker are steadier; strength is the size of the effect.

| danielblnc | sequence | boil | flicker_res | flicker_out | strength | detail | settle after a reset |
|---|---|---|---|---|---|---|---|
| 0.4.1 | still_jitter_720 | 0.00314 to 0.00189 (-40%) | 0.00424 to 0.00302 (-29%) | 0.00860 to 0.00820 | 0.02046 to 0.01939 | 0.899 to 1.094 | 8 to 14 |
| 0.4.1 | still_jitter | 0.00267 to 0.00167 (-38%) | 0.00345 to 0.00252 (-27%) | 0.00619 to 0.00582 | 0.02089 to 0.01949 | 0.909 to 1.126 | 10 to 14 |
| 0.4.1 | pan_slow_720 | | 0.00460 to 0.00383 (-17%) | 0.00947 to 0.00889 | 0.02275 to 0.02496 | 0.980 to 1.000 | |
| 0.4.1 | pan_slow | | 0.00373 to 0.00308 (-18%) | 0.00640 to 0.00584 | 0.02218 to 0.02309 | 0.979 to 0.980 | |
| 0.4.1 | pan_fast | 0.00752 to 0.00573 (-24%) | 0.00610 to 0.00477 (-22%) | 0.01004 to 0.00868 | 0.02634 to 0.02486 | 0.894 to 0.864 | 29 to 32 |
| 0.4.1 | hf_pan | 0.00569 to 0.00481 (-15%) | 0.00700 to 0.00668 (-5%) | 0.02437 to 0.02200 | 0.02536 to 0.03312 | 0.765 to 0.687 | 0 to 33 |
| 0.4.1 | object | 0.00449 to 0.00335 (-25%) | 0.00469 to 0.00378 (-19%) | 0.01130 to 0.01070 | 0.02517 to 0.02474 | 0.893 to 0.960 | 59 to 32 |
| 0.4.1 | cut | 0.00291 to 0.00247 (-15%) | 0.00377 to 0.00312 (-17%) | 0.00863 to 0.00823 | 0.02171 to 0.02174 | 0.944 to 1.031 | 8, 10 to 11, 14 |
| 0.4.3 | still_jitter_720 | 0.00316 to 0.00190 (-40%) | 0.00432 to 0.00303 (-30%) | 0.00866 to 0.00820 | 0.02041 to 0.01949 | 0.901 to 1.093 | 8 to 14 |
| 0.4.3 | still_jitter | 0.00268 to 0.00169 (-37%) | 0.00344 to 0.00252 (-27%) | 0.00619 to 0.00582 | 0.02090 to 0.01952 | 0.910 to 1.128 | 9 to 14 |
| 0.4.3 | pan_slow_720 | | 0.00458 to 0.00386 (-16%) | 0.00945 to 0.00890 | 0.02278 to 0.02519 | 0.980 to 1.003 | |
| 0.4.3 | pan_slow | | 0.00371 to 0.00311 (-16%) | 0.00638 to 0.00586 | 0.02220 to 0.02325 | 0.981 to 0.981 | |
| 0.4.3 | pan_fast | 0.00747 to 0.00553 (-26%) | 0.00604 to 0.00474 (-22%) | 0.00999 to 0.00868 | 0.02638 to 0.02526 | 0.894 to 0.864 | 30 to 32 |
| 0.4.3 | hf_pan | 0.00573 to 0.00481 (-16%) | 0.00694 to 0.00670 (-3%) | 0.02428 to 0.02199 | 0.02538 to 0.03345 | 0.763 to 0.685 | 14 to 22 |
| 0.4.3 | object | 0.00451 to 0.00332 (-26%) | 0.00478 to 0.00379 (-21%) | 0.01135 to 0.01070 | 0.02519 to 0.02491 | 0.893 to 0.961 | 59 to 32 |
| 0.4.3 | cut | 0.00292 to 0.00245 (-16%) | 0.00380 to 0.00312 (-18%) | 0.00865 to 0.00824 | 0.02170 to 0.02180 | 0.944 to 1.034 | 8, 12 to 11, 14 |

What the history costs on the same runs. After a reset the image takes longer to settle, except on
`object`, and the reset itself jumps more: `pop_reset` 0.547 to 0.984 on still_jitter_720 and
0.043 to 0.377 on hf_pan (0.4.1). The whole frame pumps in step with the camera jitter: the
jitter-locked step of the mean residual goes from 0.00050 to 0.00336 on pan_fast and from 0.00023 to
0.00103 on pan_slow (0.4.1; 0.4.3 moves the same way). Per-frame detail drops on the two fast or
fine pans: hf_pan 0.765 to 0.687, pan_fast 0.894 to 0.864. flicker_out, the flicker of the image
the upscaler gets, falls by only 4.6 to 13.5%. On `object` the ghost against the history-off run is
0.00094 (0.4.1) and 0.00078 (0.4.3); ghosting with history on behind FSR in a real game is
unmeasured. The harness's GPU times are not a cost figure: the runs shared the GPU with another
workflow.

Two host behaviours that come with history on and now reach Setup installs too. The model's
history restarts on a game reset, a size, guide, pass or setting change, a timeout, an explicit
invalidate or a gap over 250 ms (`AmdPreSr.cpp:1622-1624`); a frame the host skips inside that
window is not in the list, which the harness (it waits on every frame) cannot show. And the
post-Execute wait of `WaitAfterSubmitIfEveryFrame` runs only with history off, so it no longer runs
during a native rebuild either.

A limit of sRGB that Setup installs now meet: `ColorEncoding.h` decodes the game's colour to linear
light into an RGBA16F texture before the model, and the decode overflows half precision above about
107 (107 gives 65344, 108 gives infinity). The install-history-defaults item's hard-content check
(`exports\mochizuki-work\stab-install-history-defaults\verify-hard-content`) found outputs capped at
107.06 on a sequence with highlights up to 4096, which Linear kept. Whether Cyberpunk 2077's
pre-upscaler colour goes that high is unmeasured.

**danielblnc 0.4.2 is public.** danielblnc published 0.4.2 on 2026-09-27 ([Alpha 0.4.2](https://github.com/danielblnc/DLSS-NR-on-AMD/releases/tag/v0.4.2)). The public setup holds the same
`version.dll` as the supporter build `v0.4.3-amd-nr` already accepts (`kAmd042`, `8aa2dcc5…`), so the host
needs no change. The AMD-NR installer's payload installs it with `0.4.3-amd-nr` since that day (installer
`handoffs/HANDOFF-runtime-0.4.2-2026-09-27.md`); its ReShade route stays on the patched 0.4.1. Here
`install-amd-presr.ps1`, `README.md` and the host's log recommend 0.4.2 where they named 0.4.1, and only
0.4.3 and 0.5.0 are called supporter builds.

**danielblnc 0.5.0 (supporter build).** Another early build danielblnc gives his supporters; this
project does not ship it, and neither does the installer's payload. The host accepts it when the user
brings one, like 0.4.2 and 0.4.3: `kAmd050` in `AmdLayout.h` (SHA `cddfb09e…`, 38,703,616 bytes, in the
setup's `.rdata` at file offset `0x26175f`) and its bootstrap entry in `RuntimeHostLoad.h` (DllMain calls
CreateThread at `0x6eed`; worker `0x8ea0`). `install-amd-presr.ps1` knows the hash, ranks it newest, takes
it out of its setup like the others and recommends 0.4.2. The DLL tripled in size because it adds
RDNA3 (gfx11) register kernels beside the RDNA4 ones; on the RX 9070 XT it logs `network kernels:
register kernels (RDNA4)`. One new `[DlssNrOnAmd]` key, `Rdna3RegKernels`, which the runtime reads and
ignores. The weights are the same as 0.4.1's.
Mapped from 0.4.3 with the same two methods (`daniel-runtime/analysis-opti`: `map_layout_043_050.txt`,
`datamap_043_050.txt`), which agree on all 42 data fields. Shutdown, the wait helper and the bootstrap
worker are 0.4.3's instruction for instruction; Init, Record and Notify only moved engine members (+0x30),
which the host never touches. The `ANCHORS` rows come from `implement/anchors/derive_050.py`.
`amd_layout_binary_check.py` passes on 0.3.1, 0.4.0, 0.4.1, 0.4.2, 0.4.3 and 0.5.0; the synthetic
bootstrap fixture and `test-amd-host-contracts.cmd` (with the binaries) pass; the installer exit tests
pass (22). `tools/test-amd-runtime-init.cmd` on an RX 9070 XT, alternating with 0.4.3: init true with
the 0.4.x weights, 24 of 24 frames per wait mode with no timeout, dispatch wait 10.17 to 10.43 ms per
frame against 10.65 to 10.66 ms for 0.4.3. Not tested in a game.

**Residual stabilizer (danielblnc, mochizuki), off by default.** `[DlssNr] AmdStabilizerStrength` /
`AmdStabilizerThreshold` and `MochizukiStabilizerStrength` / `MochizukiStabilizerThreshold` (strength 0 to
1, default 0; threshold 0.5 to 8 in 1/255, default 2; menu "Stabilizer" and "Stabilizer threshold" under
danielblnc's "Disable temporal stabilization" and in mochizuki's Temporal section). A host pass after the
runtime and before SR (`AmdPreSr::ResidualStabilizer`, `dlssnr/amd/ResidualStabilizer.h`, called from
`AmdBridge::Run`) filters the runtime's edit over time: the previous edit is moved along the nearest
surface's motion vector plus the jitter step, kept only on the same depth surface, and clamped to the
current edit plus or minus the threshold, so no pixel moves by more than strength times threshold whatever
the vectors. Pixels without history pass the runtime's result through bit for bit; without readable depth
the pass skips the frame. The runtimes, their histories and SR's inputs are untouched. At strength 0
nothing is recorded, allocated or compiled. Cost when on: 0.18 to 0.33 ms a frame at 1080p on the RX 9070
XT, 49.8 MB of textures.
Measured by replaying the runtimes' recorded outputs of the stability sequences
(`exports\mochizuki-work\residual-stabilizer\RESULTS.txt`): danielblnc 0.4.1 at 0.5 / 2 cuts the
effect's flicker to 0.54x to 0.73x on all eight sequences, with the effect's size 0.99x to 1.00x and
detail 0.98x to 1.03x, but newly uncovered background behind a moving object lags a little more (ghost
0.00094 to 0.00126), which is why it stays off; 0.4.3 moves the same way. With the Linear, history-off
setup older `Setup.bat` installs have, 0.5 / 2 passes every check (flicker 0.54x to 0.62x). mochizuki
loses a few percent of detail at every setting that helps, so no value is recommended for it. Not tested
in a game.

**lmxxf: smoothing the effect only, off by default.** `[DlssNr] LmxxfSmoothResidual` (default false;
menu "Smooth the effect only" in lmxxf's Temporal section) moves lmxxf's output smoothing onto the
network's change alone: the host sets `LMXXF_NR_FRAME_FLAG_SMOOTH_RESIDUAL`, and `RecordOutputs` pulls the
output minus the input toward the previous frame's difference, warped like the histories, then adds it
back (`residual_main` in `third_party/lmxxf/shaders/native_output_smooth.hlsl`, Patch G).
`LmxxfSmoothStrength` and `LmxxfSmoothThreshold` keep their meaning and apply to that difference. With the
key off the output is byte-identical to `v0.4.3-amd-nr`. On the stability sequences it cuts the effect's
flicker to 0.53x to 0.76x with the effect's size and detail within 2%, but the effect lags on newly
uncovered background (ghost 0.00165 against -0.00013) and settles one frame later after a reset, which
is why it stays off (`exports\mochizuki-work\stab-lmxxf-residual-smooth\RESULTS.txt`). Cost when on:
about +0.26 to +0.37 ms a frame at 1080p on a busy GPU. Not tested in a game.

### 0.4.5

Released in `v0.4.5-amd-nr`. The mochizuki and lmxxf runtimes are 0.4.4's; the installer's payload reuses them.

**danielblnc 0.4.3 is public** since 2026-09-28 ([Alpha 0.4.3](https://github.com/danielblnc/DLSS-NR-on-AMD/releases/tag/v0.4.3)).
Its setup holds the same `version.dll` `d1e32086` this host already accepted, so only the text changed:
`README.md`, `install-amd-presr.ps1` (rank note and messages), `PACKAGE_RELEASE.ps1`, the host's log line
and `tests/amd_installer_exit.py` recommend 0.4.3. Only 0.5.0 is supporter-only now. The AMD-NR installer
already installs 0.4.3 with OptiScaler 0.4.4 (payload, not this repo).

**lmxxf and mochizuki beside a separate ReShade.** With ReShade loaded as `dxgi.dll` (OptiScaler as
`winmm.dll`), mochizuki never ran: the log stopped at `AMD pre-SR: awaiting execution queue observation`.
ReShade wraps the game's D3D12 lists. NGX got ReShade's wrapper, and `ExecuteCommandLists` got ReShade's
`_orig`, our `CommandListProxy`, so `AwaitingListTracker` never matched a pointer. `Run` in `AmdBridge.cpp`
now asks the list for `ILogicalCommandList` (ReShade passes unknown interfaces on to `_orig`) and works with
our proxy, so the list and its device are the ones the queue sees. Without a wrapping layer nothing changes.
Confirmed by a user in Conan Exiles Enhanced (RX 9070) with the test build.

**danielblnc 0.5.1 (supporter build).** Not distributed, like 0.5.0. `kAmd051` (SHA `493b4a3b…`,
38,569,472 bytes, same `.rdata` offset `0x26175f` in its setup) and its bootstrap entry (same call site
`0x6eed` and worker `0x8ea0`; CreateThread's IAT slot is now `0xaa9c0`). Mapped from 0.5.0 two ways that
agree on all 42 data fields (`map_layout_050_051.txt`, `datamap_050_051.txt`; the block moved +0x3010 up
to `historyValid`, +0x3098 from `initDone`). Init moved to `0x2a0c0`; Record, Notify, shutdown and the
wait helper kept their addresses, and Record and Notify only moved engine members (+0x88). No new INI key.
`ANCHORS` rows from `implement/anchors/derive_051.py` (the same sites as 0.5.0). `amd_layout_binary_check.py`
passes on 0.3.1 to 0.5.1, the host contracts and installer exit tests pass, and
`tools/test-amd-runtime-init.cmd` on an RX 9070 XT runs 24 of 24 frames per wait mode with no timeout:
9.69 to 9.79 ms per frame in dispatch wait, 10.22 ms on 0.5.0 in the same session. Not tested in a game.

### 0.4.6

Released in `v0.4.6-amd-nr`. The mochizuki and lmxxf runtimes are 0.4.4's; the installer's payload reuses them.

**danielblnc 0.5.0 is public** since 2026-09-29 ([Alpha 0.5.0](https://github.com/danielblnc/DLSS-NR-on-AMD/releases/tag/v0.5.0)),
the same `version.dll` `cddfb09e` `kAmd050` maps, so only the text changed: README, Setup and the host's log
recommend it, and only 0.5.1 is a supporter build. The installer's payload installs it on both routes.

**An old INI's `AmdEncoding=0` reads as sRGB.** The Setup.bat before 0.4.4 wrote 0 (the former Auto) into every game
it set up, and 0 read as Linear, so those games never got the sRGB default a first install gets. It now reads as
sRGB, and the menu shows it so; choosing Linear writes 1 as before.

**danielblnc's own Black lift and exposure source** (`AmdToneLift` 0-0.25, `AmdUseGameExposure`; Colour section,
"Black lift" and "Exposure: Game / Auto"). The host used to pin both at init (no lift, the game's exposure). The
runtime reads ToneLift in the per-job options copy (0x1c0a0 on 0.4.3) and UseGameExposure in a function Record calls
every frame (0x19ba0), so both are written on every Record like the tone curve, and a change restarts the history.

**One Effect strength on danielblnc** (`AmdEffectStrength`, now 0-2): without the composition it is the runtime's
Scale; with it the runtime keeps its default and the slider is the composition's intensity, which moves without
restarting the model's history. lmxxf and mochizuki keep "Effect intensity" (`RenoIntensity`) in the composition
block. Above 100% without the composition the runtime gets up to twice its default Scale, untested in a game.

**Upscale Ratio Override and Output Scaling** moved from the Image tab to the Upscaling tab
(`MenuCommon::RenderUpscaleRatioSettings`), unchanged.


**RenoDX composition** (`RenoComposition`, `[DlssNr] RenoComposition`, off by default; menu "RenoDX
composition" in every runtime's Effect section). The composition of RenoDX's DLSS Neural Rendering add-on
as it stands in PEQHUB/RenoDX-DLSS5-Generic (MIT, credited in `Licenses/RenoDX_ATTRIBUTION.txt`), after the
runtime and the stabilizer, before SR, on all three runtimes. The runtime's answer is not taken as the
picture: its edit is measured in a proxy (linear light over the pre-exposure and a divisor, through the
max-channel shoulder at 0.75) and put back on the game's own colour as a gain bounded in log2 (luminance by
`RenoGuard`, 2x by default, chroma `RenoChromaClamp` stops), faded out on the black floor. `RenoIntensity`
scales the edit up to 1 and above it raises the luminance ratio to a power inside the guard; both are the ReShade
add-on's controls. `RenoColour` 0 keeps only the edit's luminance ratio (the game's hue). The divisor is their Stable governor (median to 0.30 only when the frame
is proven over-range; attack 4, release 0.5 stops per second, hysteresis). `RenoPedestal` takes the network's
lift off near-black 32x32 blocks. danielblnc's frames are composed in the transfer `AmdEncoding` gives them;
lmxxf and mochizuki as they are.
`tests/amd_reno_composition.cpp` (in `test-amd-host-contracts.cmd`) runs the stage on the GPU: the game's
colour unchanged where the runtime changed nothing, +1 and -1 stop carried, +4 bounded to +2, a clipped HDR
highlight keeping its range, colour 0 keeping hue and luminance, the black floor untouched, the pedestal
removed only in dark areas, an sRGB frame round-tripped, and the divisor snapping and holding. Cost on an RX
9070 XT: 0.07 ms at 1080p and 0.11 ms at 1440p, 0.18 and 0.26 ms with the pedestal. Tried and approved in Cyberpunk 2077 by the user.

---

### 0.4.7

Released in `v0.4.7-amd-nr`. The mochizuki and lmxxf runtimes are 0.4.4's; the installer's payload reuses them.

**danielblnc 0.5.1 is public** since 2026-10-01 ([Alpha 0.5.1](https://github.com/danielblnc/DLSS-NR-on-AMD/releases/tag/v0.5.1)).
Its setup is byte for byte the supporter one, so the `version.dll` is the `493b4a3b` `kAmd051` already maps and only
the text changed: README, Setup and the host's log recommend it.

**danielblnc 0.6.0 (supporter build).** Not distributed. It adds RX 6000 (RDNA2) cards, which need AMD's HIP SDK
7.2 runtime because their drivers ship HIP 6.4 only, and fixes flicker in dark areas and on small lights before
upscaling. `kAmd060` (SHA `195c4a89…`, 56,677,888 bytes, `.rdata` offset `0x262017` in its setup). Mapped from 0.5.1
two ways that agree on every data field (`map_layout_051_060.txt`, `datamap_051_060.txt`; the block moved +0x5060
to +0x50e8, and the option bytes from `enabled` on are spread out). Every code entry moved: Init `0x2cfb0`, Record
`0x17680`, Notify `0xb4a0` (the function match; the instruction windows picked a look-alike), shutdown `0x1b420`;
the wait helper moved +0x2270 and matches byte for byte. The bootstrap calls CreateThread (IAT `0xaf170`) at
`0x70ed` for the worker `0x91c0`. `ANCHORS` rows from `implement/anchors/derive_060.py`, but for Record's
`depthInverted` read, now `8b 15` at `0x19a72`. Six new INI keys (FreshExposure, JitterComp, PreHoldCounter,
ResetOnEnable, SrgbApplyFix, UsePreExposure) keep the runtime's defaults. `amd_layout_binary_check.py` passes on 0.3.1
to 0.6.0, the host contracts and installer exit tests pass, and `tools/test-amd-runtime-init.cmd` on an RX 9070 XT runs
24 of 24 frames per wait mode with no timeout, 9.41 ms per frame in dispatch wait. Not tested in a game, and not on an
RDNA2 card.

### 0.4.8

Released in `v0.4.8-amd-nr`, with mochizuki `0.4.8-amd-nr` (runtime and shaders) beside it in the installer's
payload. Nothing below was tested on a GPU outside the games named; the mochizuki harnesses (`mz_bench`,
`mz_timing`) were not run.

**danielblnc's colour, as his own runtime gives it.** Compared on NTE against his overlay and TheAutomatic's fork
(`arrumar_cor/`), two things made ours paler and the styles inert:
- `AmdEncoding` defaulted to sRGB, which decodes the pre-upscale colour as sRGB before the network and encodes the
  answer back: the network saw a darker frame and changed less. Default is Linear (1) now, as his runtime and
  TheAutomatic hand it over; an INI's 0 also reads as Linear.
- The network's Style input (`L->style`) was pinned to 0 at init and never written. "Colour grade" was our post-pass
  imitation of NVIDIA's Model B/C. It is gone: `AmdStyle` (0 Standard, 1 Natural, 2 Cinematic) goes to the runtime on
  every Record and restarts the history when it changes.

**Info drawer.** The status lines under Experimental (runtime status, the backend's note, mochizuki's network figures)
sit in a closed "Info" tree on all three runtimes. The frame-time graph and the Neural pass line stay where they were.

**Exposure only for HDR input.** NGX applies the exposure texture to HDR colour only. Watch Dogs: Legion hands an
R8G8B8A8 colour with an exposure that grows huge in the dark; applied again it overflowed the network into a green
frame. AmdBridge drops the texture for a colour without `IsHDR` or in an 8- or 10-bit UNORM format, for every runtime.

**Typeless motion below 100%.** The resampling path refused `R16G16_TYPELESS` vectors (Watch Dogs: Legion) and NR went
off below 100% NR resolution. `ReadFormat` maps the two-channel typeless formats to FLOAT.

**mochizuki.**
- The game's exposure (`MochizukiGameExposure`, on): the backend copies the exposure texel into an 8-slot readback
  ring on the frame's list and reads it seven frames later; `MochizukiNrSetExposure`, a new optional export, makes the
  white point `MochizukiWhitePoint / exposure`. RE Engine's linear colour reached the network 6-7x too bright
  (the Preprocess meter said -2.7 EV in Resident Evil Requiem) and the effect stayed in the shadows. Menu: top of the
  mochizuki section; Info shows the white point in use.
- `dlssnr-amd\` beside the exe when it is not beside the DLL (`DataDirectory`): RE Engine loads every DLL from a copy
  in `_storage_\`, and the network failed at once there.
- From upstream after v0.0.2.1 (`third_party/mochizuki/UPSTREAM.md`, entry 16): the post block's history weight times
  the model's `blend_scale` 0.7397; R9G9B9E5, B8G8R8X8, 16-bit UNORM/SNORM, R8G8B8A8_SNORM and the 16-bit packed
  colour formats (the blit-only ones where the GPU can blit them); the Preprocess (exposure, curve, contrast,
  saturation before the network, taken back out of its answer; `MochizukiPreprocess*`, off, a menu drawer, the
  metered EV in Info), new shader `runtime/runtime_prep.spv`.
- Left for the next version: the in-place motion sampling and per-frame pre-block noise (d1185d2) and the Windows
  network rebuilt for the AMD compiler (228d3a6, b1419b0): `handoff/mochizuki-upstream-2026-10-01.md`.

### 0.4.9

Released in `v0.4.9-amd-nr`, with mochizuki `0.4.9-amd-nr` beside it in the installer's payload. The details of each
part are in the sections at the end of this file (stall protection, lmxxf 0.39) and in
`handoff/mochizuki-upstream-2026-10-01.md`.

- **Menu.** PR #2 (Caliel666): the window resizes, never past the screen. Rounded corners, a Discord invite beside
  Open Wiki, the title without the commit and build date. Sections down the left (NR, UP, FG, IMG, UI, ADV, full
  names on hover) and the menu anchored to a corner (drag it there, or Interface -> Menu corner; `[Menu] Anchor`).
- **FPS overlay.** With NR running: "NR <runtime>" on the first line, the model's GPU time and average at Full, its
  graph at Full + graph. Frame-time lines in the theme's colours by default (`OverlaysUseTheme`), and Numpad 5 the
  default NR toggle key.
- **Stall protection.** One danielblnc job the GPU holds for 2 s turns NR off until the game restarts
  (`AmdStallStandDownMs`, 0 off); `StallWatch.h`, the same as the add-on's.
- **Logs.** DLSS availability reads (`SuperSampling.*`) logged once each, and one warning when 30 s of presented
  frames reached no upscaler in OptiScaler (the S.T.A.L.K.E.R. 2 report).
- **lmxxf.** Upstream 0.39 (network ~1.8x faster at 1080p), Style (`LmxxfStyle`), reuse on still scenes
  (`LmxxfVitReuse`), the compact 1080 tier (`LmxxfCompact1080`), the 0.38 colour-format fallback and the RX 9060
  series (`lmxxf-modules-gfx1200`, which Setup copies and the uninstaller removes too).
- **mochizuki.** Upstream 82560c4's Windows network and motion work (8.5 ms at 1080p from 9.3; about ten times
  steadier in motion below 1080p), `NR_EDGE_BODIES=0` for a 23 s cold build.

### 0.5.1

Not released yet; the build names itself `0.5.1-amd-nr`. Nothing below was tested in a game or on a GPU: the build passes and
so do the host contracts (`tools\test-amd-host-contracts.cmd`, now with `tests\amd_stall_watch.cpp` on WARP fences).

- **danielblnc 0.6.0's green noise.** 0.6.0 reads a float pre-exposure at `0x60` of the record packet
  (`movss xmm0,[rsi+0x60]`, its UsePreExposure path, on by default). `Packet` in `AmdPreSr.cpp` ended at `0x60` on the
  stack, so the runtime divided the colour by whatever lay there (95385, 17703, ... in a GTA V Enhanced log). The packet
  carries `preExposure = 1` now (the exposure handed over already has the game's pre-exposure divided out) and is
  `0x68` long; 0.3.1 to 0.5.1 never read past `0x5c`. Every route (upscaler path, NR without upscaling on D3D9, D3D11,
  D3D12, Vulkan and OpenGL) builds its packet there.
- **NR without upscaling no longer stops for the session** (Mafia III with an AMD ReLive recording or screenshot,
  screen transitions in missions):
  - The stall watch turns Enable NR off as on the upscaler path, with the note under it; switching it on again retries.
    Both paths ask `AmdBridge::StandDownIfTripped`. Frame generation keeps its guides meanwhile.
  - The stall clock starts when the queue reaches NR's job (`stallwatch::Gate`, a fence signalled just before the
    list), not at the submission: on the D3D11 bridge the job waits for the game's D3D11 queue, on the DX11-to-DX12
    bridge for the copy, and a game queue held for 2 s by a capture or a mode change was blamed on NR.
  - A frame past the 4 s GPU timeout stops NR until it finishes, then NR goes on and the game's backbuffer is handed
    back (it was kept until process exit, and the game's next `ResizeBuffers` failed with `DXGI_ERROR_INVALID_CALL`). A
    resize waits 4 s for such a frame, as a present does, and logs that it waits. A context a failure stopped stays
    stopped; the D3D11 bridge resumes only once its D3D11 side's wait is done too. Every timeout is logged. Vulkan and
    OpenGL still stop their own bridge on one.
  - A failed runtime is retried 10 s later, three times a session at most, when the failure left nothing behind
    (`Backend::Retry`: device alive, completion timeline trusted, no slot busy, no pending list in the runtime, every
    loaded pass initialized). A pass whose initialization failed is never retried, and a Record on it counts as a
    failure (it refused every frame while NR looked healthy).
  - A swapchain on another D3D11 device (an overlay's, a capture's) is left alone, logged once, instead of turning NR
    off with "D3D11 device changed; restart required". When it alone presents for 120 frames it is the game's new
    device: the D3D11 side of the bridge moves to it (same adapter; the D3D12 device and runtime stay), or the status
    says to restart the game.
  - A D3D11 sRGB backbuffer (Unity in linear colour, Virt-A-Mate) goes through its UNORM twin; an unsupported format
    and a bridge failure are logged, not only shown in the menu.
- **DX11-to-DX12 bridge.** A step of the crossing that fails presents the hidden D3D11 swapchain and returns its result
  instead of `DXGI_ERROR_DEVICE_REMOVED`, which games quit on; the screen keeps the last frame, and after 30 such frames
  in a row the error goes back as in 0.5.0. The copy into a D3D12 swapchain of another size is skipped, and that
  swapchain is resized to the game's size again every tenth such frame. `ResizeBuffers` and `ResizeBuffers1` refresh
  the buffer count (and keep the fake index inside it) whenever the game's own resize succeeded; a D3D12 resize that
  failed is tried once more with that swapchain's own flags, and its error goes to the game if that fails too.
- **Logs.** `amd_bridge.log` takes the render batch split once (it went in every frame: 29 MB in a Cyberpunk report). The upscaler path logs once which runtime `NrBackend` got, and warns
  when the name is none of them. The menu key's open and close and the AMD graphics tracker hooks (with why) are logged
  at Info.
- **Build.** The `tools\*.cmd` scripts take `VS_ROOT` when it is set instead of asking vswhere, which lists nothing when
  the Visual Studio installer's registry is broken, and `gen_ffx_opticalflow.ps1` finds dxc in the SDK vcvars names
  (`WindowsSdkDir`).

## 6. Diagnostics playbook

When FSR-RR "runs but does nothing", the denoiser dispatching successfully proves
nothing — its inputs can be empty while every dispatch reports success.

Set `[FSR-RR] Diagnostics=true` and read `OptiScaler.log` for `[RR_INPUT_PROBE]`. The
probe records every 60th conversion. What to look at:

| Line | Healthy | Meaning when wrong |
|---|---|---|
| `floor/raw crossing` | low % | high % = the residual collapses and the denoiser is handed nothing |
| `specular signal input ... dead(<1e-5)` | low % | 100% = no specular radiance reaches the denoiser |
| `motion ... exactZeroXY` | low % | 100% = no motion, so no temporal accumulation |
| `normals channels: roughness ... exactZero` | low % | 100% = no material guidance |
| `skip signal ... avgLuma` | low | high = the frame is bypassing the denoiser and reaching the screen raw |

Debug views (`Debug View` in the advanced window) render these per pixel;
`DenoiserFraction` and `SkipRawInject` are the two worth knowing.

`docs/fsrd_pipeline_contract.md` is the authoritative description of the FSR-RR
preprocessor, including the energy split it maintains:

```
floor    = isolation * filter(raw)
residual = max(0, raw - floor)
out      = floor + denoise(demod_remod(residual)) + skip
```

**Read the probe's format field carefully.** It reports the *RR-facing* buffer, which is
OptiScaler's own converted allocation, not the title's source texture. Mistaking one for
the other costs a full debugging round-trip.

For the AMD backend, `AmdBridge::Status()` drives the menu status line, and the runtime
version in use comes from `AmdBridge::RuntimeName()`.

---

## 7. Known issues and open items

- **The clang-format check on tag `v0.1.0-amd-nr` stays red.** The tag points at `531c221a`,
  which predates the formatting commit `a12c4a9f`; the branch and its pull request pass. The
  formatting changed no code, so the released DLL is unaffected. It was left alone because moving
  the tag would change what a published release points at. `v0.1.1-amd-nr` is formatted.
- **`[FSR-RR] TaggedNormalRoughness` has never been observed to fire.** It was written
  for a title whose normals binding looked like it carried no roughness; the real cause
  turned out to be an external mod zeroing the ray-tracing buffers. It is left in reach,
  **default off**, because it is unvalidated — not because it is known to help.
- **Motion vectors read 100% zero** in the RR-facing probe during the session that had
  the external mod active. Not re-measured since that mod was disabled. If FSR-RR
  misbehaves on a new title, check this first.
- **The post-upscale enlarge** uses the AMD runtime's internal shader, not a
  user-selectable filter. `Record()` always returns the already-enlarged image, so
  routing it through `OS_Dx12` instead would mean changing that contract.
- **The Vulkan and D3D11 paths** raise the same "Upscaler failed to run!" toast shape
  that was fixed for D3D12. Not changed — FSR-RR is D3D12-only and there was no evidence
  of it firing there.
- **`AmdSlots` reaches 5** but only 3 pass DLLs exist. Slots and passes are *different*
  arrays (`slots[kMaxSlots=5]` vs `runtime[3]`); slots need no extra files.
- The post-upscale placement is **compile-verified and lightly exercised**, not
  systematically tested across titles. Depth and motion are point-upsampled from the
  render grid, so watch for ghosting and shimmer.

---

## 8. Gotchas that cost real time

- **External mods can zero the ray-tracing buffers.** REFramework's "Raytracing Tweaks"
  made FSR-RR receive dead signals while every dispatch reported success. If the inputs
  look impossible, suspect another mod before suspecting the binding.
- **OptiScaler rewrites `OptiScaler.ini` when settings are saved.** Explicit values can
  come back as `auto`. For `[DlssNr] Enabled` that silently means *off*, which reads
  exactly like a regression on the next launch.
- **`dlssnr_amd_pass1-3.dll` are passes, not slots.** The ceiling of three is physical:
  `std::array<HMODULE, 3> runtime` in `AmdPreSr.cpp`.
- **The AMD menu branch returns early.** Anything added to the shared DLSS-NR menu below
  that point is dead code while the AMD backend is installed.
- **Test a release in a clean game folder.** The 0.1.0 packaging bug survived a full test
  session because the development game folder still had FidelityFX DLLs at its root from an
  earlier manual install. Extract the zip into a folder holding only the game's own files, run
  `Setup.bat`, and read `OptiScaler.log` for where each `amd_fidelityfx_*` module loaded from.
- **Every SR frame offers the pass both seams.** Any gate added to one placement needs its
  counterpart on the other, or both reach the AMD backend on the same evaluate and its settling
  check stalls it (section 5, "One placement switch").
- **A feature can decline a frame on purpose.** `changeBackend` set from inside an
  evaluate means "rebuild me", not "I failed".

---

## 9. Map of the code

| Path | What lives there |
|---|---|
| `OptiScaler/dlssnr/amd/` | AMD bridge: `AmdBridge` (NGX-facing entry), `AmdPreSr` (the backend, slots, passes, guides), `AmdLayout.h` (runtime identification by SHA), `HipRuntimeLoad.h`, `RuntimeHostLoad.h` |
| `OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp` | The DLSS-NR pass itself. `EvaluateAtSeam` is the core; the AMD gate sits near its top |
| `OptiScaler/dlssnr/DlssNr_Menu.cpp` | The Ins-menu NR page. AMD section first, then an early return, then the NVIDIA-only controls |
| `OptiScaler/upscalers/fsr31/FSRDFeature_Dx12.cpp` | FSR-RR: NGX/Streamline input resolution, denoiser context, dispatch |
| `OptiScaler/shaders/fsrd_preprocess/` | The FSR-RR preprocessor (floor filter, conversion, composition) and its input probe |
| `docs/fsrd_pipeline_contract.md` | The FSR-RR pipeline contract — read before touching the preprocessor |
| `tools/` | Installer, packaging, shader-identity generation. Required at build time |

## Stall protection (unreleased, 2026-10-01)

Two players with an RX 9060 XT and the danielblnc runtime saw jobs of 3.8 to 4.2 s every 10 to 30 minutes (WWE 2K26 and
Cyberpunk on ReShade, Bellwright on OptiScaler); two stalls close together froze the whole PC. `AmdPreSr` arms
`StallWatch.h` on each `Signal` of the completion fence; a thread pool timer measures the oldest value still
pending since submission, and `AmdBridge::Run` turns NR off until the game restarts when it exceeds `AmdStallStandDownMs`
(2000, 0 disables), with the message suggesting mochizuki. Only the daniel backend arms it. The same file is in the ReShade
add-on (`core/shared/stall_watch.h`). Tested in isolation (a stuck fence fires in ~2.1 s, jobs that finish do not
fire); not tested in a game with OptiScaler.

## lmxxf 0.39 (unreleased, 2026-10-01)

`third_party/lmxxf` synced from `7ef24e7` (0.29) to `8af8624` (0.39 + two days of unreleased commits) with
`tools/sync-lmxxf-upstream.ps1`, which now copies the kernels' `.inc` files and six new headers and only raises the codec's
strength limit when upstream is still at 1 (it accepts up to 3). The bridge (`hip_d3d12_bridge.h`) was merged three
ways: upstream already brings TheAutomatic's recovery, so our multipass went back on top of its header. 31
gfx1201 modules compiled here with the driver's COMGR (LLVM 21, the same as lmxxf; the hashes differ because of the source text,
the gfx1201 lines of `hip/SHA256SUMS` are the local ones). `LmxxfProductionOptions` turns on the groups of the 0.31–0.39 templates
(wave_owned, c512_m32, vit_proj_n64, pdl, vit_stream=3, swin_run and the byte paths); `RuntimeOptions` turns off
each group whose module is missing. Same weights (`lmxxf-weights.1`).

Measured with an end-to-end test (1920×1080 image, synchronizing every frame, Cyberpunk's runtime vs the new one): 1 pass
19.3 → 10.5 ms, 3 passes 56.5 → 30.5 ms. The new kernels are bit-exact against the old chain of the same build (0
differing values with the seven modules removed); against the old runtime it gives 50.4 dB, the intentional numerical change
of 0.36 (FMA in float). Not tested in game nor with temporal history. To release: a new opti with the new modules and
`native_format_convert.hlsl`, and new pins in the installer's payload.

lmxxf Style (same day): `LmxxfStyle` (0 Standard, 1 Natural, 2 Cinematic; default 1, what the kernels had
built in) in the ini and in the lmxxf menu. It goes through `LMXXF_NR_FRAME_FLAG_STYLE` + a `style` field at the end of `LmxxfNrFrameInfo`
(mochizuki accepts a larger struct and does not receive the flag). The runtime puts `DLSS5_STYLE` in the environment before creating the network,
which writes Style/128 into the kernels on load; switching rebuilds the network like a size change. Tested: Style 1 and without the
flag bit-identical to before; 0 and 2 at 20–25 dB; switching 0→2 midway gives the same as starting at 2. An old runtime
refuses the new struct, so OptiScaler and LmxxfNrRuntime.dll have to ship together.

Four more for lmxxf (same day, tested in the scratchpad's `lmxxf_e2e`, 1080p image):
- **RX 9060 (gfx1200):** 31 modules in `third_party/lmxxf/modules-gfx1200`, packaged as `lmxxf-modules-gfx1200`
  next to `lmxxf-modules`. `Create` asks HIP for the architecture by LUID and switches folder on gfx1200; any other
  architecture fails with a clear message. On the 9070 nothing changes (tested); on the 9060 not tested, no card available.
- **ViT reuse** (`LmxxfVitReuse`, flag `VIT_REUSE`, `DLSS5_VIT_ADAPTIVE=1` put in the environment every frame): only
  with 1 pass. Still image: identical output, ~1 ms faster; the lmxxf log shows reuse from the 3rd frame on.
- **1088 rows** (`LmxxfCompact1080`, flag `COMPACT_1080`): 46.3 dB against 1152, larger difference in the last rows;
  switching midway rebuilds the network because of the processing size change (`builtProcW/H`) and comes back bit-identical.
- **Extra formats** (0.38 fallback + RGB9E5): the codec receives `privateFloatOutput` and the output becomes a private RGBA16F,
  which the upscaler reads as is. R10G10B10A2 used to fail in the codec; now it runs (45.9 dB, due to the 10-bit quantization).
Installer: `lmxxf-modules-gfx1200` in `OwnedFolders`. In the release, the files in that folder cannot go into the
`optiscaler` component that old apps install (they refuse the name in the transaction): they go into an on-demand component, like
mochizuki.
