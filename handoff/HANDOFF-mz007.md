# mochizuki 0.0.7 sync (worktree wt-mz007, branch mochizuki-0.0.7)

Uncommitted. Base: 0.5.5-opennr (6a47f0448). Upstream: mochizuki0323/DLSSNR-AMD tag v0.0.7 (cc27501 Windows,
29fda44 Linux). Details in `third_party/mochizuki/UPSTREAM.md`, section "Sync to v0.0.7".

## Taken

Three-way merge of every file v0.0.7 changed under `windows/src/core`, `windows/shaders`, `windows/build`,
`windows/data/aco` and the Linux network ACO is built from (`linux/build/build_network.py`, `linux/shaders`), base
ab00d2b, ours = the vendored tree. Three conflicts, all resolved by keeping both sides (build-cancel check moved into
upstream's new `create_kernels`; `NR_GRAPH_ARGS` before upstream's share block; our `first_full` transfer beside
upstream's dropped `up_shown`).

- Style grade: Natural and Cinematic graded after the post block (`nr_style_grade.glsl`, `_grade` pipeline twins, 4+4
  new ACO records). Host: `Runtime::set_style_grade` per frame from the new control.
- Motion vectors at the nearest texel: `RuntimeConfig::motion_point`, set by the host.
- Motion Without Depth: in the core (reads `[Network] Motion Without Depth` from a `dlssnr-amd.ini` beside the game,
  default 1). OptiScaler's RenoDX stage path already passed no depth, so nothing changes; `MochizukiUseDepth` kept,
  still only for a network outside the stage.
- Resize path, image heaps, residency (`GraphAct`, offload/reload), shared pipelines (`nrvk::SharedPipeline`): in the
  core. Host uses `act_arena` only: a network built while the frames keep the current one (model or int4 switch, pass
  change) runs in the current one's activation arena when it fits ("activation arena: taken over" in the log).
- Bright dots on linear HDR below 100% model resolution: `runtime_transfer.comp` clamp taken. Only used outside the
  RenoDX stage; the stage's resolve (always used by OptiScaler) clamped already.
- Typeless exposure: OptiScaler side, `LmxxfBackend.cpp` (`ExposureFloat`: R32/R32G32/RGBA32/R16/RGBA16 typeless read
  as their float format). Typeless motion vectors were taken before.
- Network names: core's `nr_ini_network.hpp`; the host's build log line now says `CC_SMALL_FP8`, `CC_SILVER_AARDWOLD`
  or `CC_SILVER_AARDWOLD_INT4_MIXED`.
- CC_SMALL_FP8 left-edge stale block: the Windows tree had that fix already (`kinds & 32` barrier); 0.0.7 brings it
  to Linux only.
- `rdna4.sh` did not change: `tools/build-mochizuki-runtime.cmd` defines unchanged.

## Left out

- Upstream host UI (`windows/src/pe/`): in-game network drop-down and Ctrl+F11 cycling, OSD, Engine data panel,
  `[Network] Residency` modes. Not cheap in our host (it keeps one network resident by design); our menu keeps the
  Old model / int4 switches, which already rebuild in the background while the current network runs.
- `share_for` / deferred builds inside a frame / `carry_history` (Resize History): our host keeps a DRS game on one
  network built for a 64 px bucket and never rebuilds inside it, so per-size runtimes are not needed and there is no
  history to carry across a rebuild.

## Local patches

All 11 kept (none done by upstream). Patch 1's check now sits in `create_kernels`.

## Colour-format conflict

Kept ours (fb1d9b800, `float-colour-shared`). Upstream's fix lets a runtime of another tex-in format share the
network, but only through `share_for`, i.e. a new runtime per format. Ours needs no build at all (one RGBA16F network,
a blit each way) and the marker makes the next session start right; with the RenoDX stage the network is RGBA16F
anyway and only the stage depends on the frame's format. Upstream's core change is taken; it composes with ours but
our host does not call it.

## New setting

- `[DlssNr] MochizukiStyleGrade` (bool, default true), `Config.h/.cpp`, `tools/PACKAGE_RELEASE.ps1` (ini doc),
  `README.md` table; menu: mochizuki page, Model section, "Style grade" checkbox under the model controls, in Reset
  all. ABI: `MochizukiNrControls::style_grade` appended (a shorter struct from an older host means on).

## Files changed

- `third_party/mochizuki/`: `windows/src/core/{nr_graph.cpp, nr_runtime.cpp, nr_runtime.hpp, nrvk.hpp,
  nr_renodx.cpp, nr_renodx.hpp, nr_ini_network.hpp, nr_ini_int4.hpp, nr_control_shared.hpp, tinlayout.hpp}`,
  `windows/shaders/{passes/runtime_transfer.comp, rdna4/fswin_t.comp, rdna4/pipelines.json,
  rdna4/include/nr_style_grade.glsl (new)}`, `windows/build/build_network.py`, `windows/data/aco/{shell-aliases.txt,
  int4/shell-aliases.txt, records/ +4, int4/records/ +4}`, `linux/build/build_network.py`,
  `linux/shaders/{passes/runtime_transfer.comp, rdna4/fswin_t.comp, rdna4/pipelines.json,
  rdna4/include/nr_style_grade.glsl (new)}`, `UPSTREAM.md`
- `OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrRuntime.cpp`, `MochizukiNrControls.h`
- `OptiScaler/dlssnr/backend/LmxxfBackend.cpp`, `OptiScaler/dlssnr/DlssNr_Menu.cpp`, `OptiScaler/Config.h`,
  `OptiScaler/Config.cpp`, `README.md`, `tools/PACKAGE_RELEASE.ps1`
- The worktree's empty submodules were checked out from the main checkout's local repos (no network, main config
  untouched) so OptiScaler builds here.

## Builds

- Runtime: `exports\mz007-work\runtime` (BUILD_OK, no compiler warnings; 55 network / 6 temporal / 15 runtime
  pipelines; all 62 + 75 ACO records match the SPIR-V built here; `dlssnr-int4.bin` remade and checked against
  upstream's hash, byte-identical to 0.5.5's). SPIR-V against 0.5.5: only `runtime_transfer*.spv` changed, plus the new
  `_grade` files.
- OptiScaler: `exports\release-local\OptiScaler.dll` (BUILD_OK; no warnings in the files touched).

## Harness (RX 9070 XT, 26.9.2, ACO on, 40 frames, network GPU time; 0.5.5 shipped vs new)

| | new model old / new | 310.8 model old / new | output |
|---|---|---|---|
| 1280x720 | 3.22 / 3.21 ms | 5.04 / 5.00 ms | byte-identical |
| 1920x1080 | 5.30 / 5.28 ms | 7.75 / 7.43 ms | byte-identical |
| 3840x2160 | 20.9 / 20.5 ms | 24.3 / 24.6 ms | byte-identical |

Also byte-identical at 1080p: pan 8 px a frame (both models; the vectors are at the colour's resolution, so the
nearest texel is the bilinear sample), R10G10B10A2, Standard set through the controls, Cinematic with the grade off, a
live model switch (new build shares the arena) and a live int4 switch. With the grade on: Natural 25.9 dB from 0.5.5
(15% darker on average), Cinematic 38.4 dB; cost 5.16 -> 5.23 ms. Standard with no motion is identical, so no
unexplained change. Logs: `exports\mz007-work\harness\matrix.txt`, `matrix2.txt`.

## Prewarm

Shaders changed (two new files, the transfer passes), so the list was made again (ACO off, 720p/1080p/1440p/4K and
1080p with the 310.8 model, from an empty `prewarm\`): `shaders 0e82c4f766c0e13c 103`, 38 pipelines (0.5.5: 101 files,
33 pipelines; it lacked `ffwd3w`, `gemmprojw`, `gemmvqkvnorm` and the two grade pipelines). pin-mochizuki.ps1 checks
the count on the "shaders" line: 103 = the files in `dlssnr-amd\shaders`. Not published.

## Install folder

`exports\mz007-work\install`: `dxgi.dll` (the new OptiScaler.dll), `MochizukiNrRuntime.dll`, `dlssnr-amd\` (shaders,
aco, int4, `dlssnr-int4.bin`, prewarm manifest, `dlssnr.bin` and `dlssnr-small.bin` copied from the 0.5.5 folder).
Never commit or publish the model files. `exports\mz007-work\old` / `new` are the harness copies.

## To test in game

- Cyberpunk 2077: MochizukiRenoStyle Natural and Cinematic, "Style grade" on and off (live, no rebuild): the two
  styles should now look clearly apart, off should match 0.5.5. Standard must look exactly as in 0.5.5.
- Old model / int4 switch in the menu while playing: switch still in the background; the log should show "activation
  arena: taken over" and VRAM should peak lower than in 0.5.5.
- First start with ACO: a few seconds more for the new grade records' import, once.
- Dying Light: The Beast (typeless exposure): with RenoDX feed from the game's exposure, NR should follow the game's
  exposure instead of metering the picture.
- Moving-object edges: change only where a game's motion vectors are smaller than the colour; not expected in
  Cyberpunk.
- Regression checks: Neverness to Everness / FF7 Rebirth (colour format switches, our fix kept), a DRS game (Witcher 3)
  unchanged.
