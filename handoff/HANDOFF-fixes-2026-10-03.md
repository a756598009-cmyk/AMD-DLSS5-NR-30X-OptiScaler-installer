# Handoff — 2026-10-03 round of fixes

Everything here is **uncommitted and unreleased**: the user wants to review it first ("For now no release, I want to see
everything first"). Nothing was published on HF or GitHub in this round. The three repositories have local changes:

| Repository | Branch @ base | What changed |
|---|---|---|
| `opti-fixes\` (worktree of `opti`) | `fixes` @ `283fdb0a` (0.4.9-amd-nr) | async, protection without locking up the session, mochizuki log/marker, fix of the mochizuki shaders for driver 32.0.32015 |
| `reshade\` (add-on) | `master` @ `551eea0` (v0.7.9) | async in the 64-bit panel, protection without locking up the session, bridge protocol v6, automatic fallback from mochizuki to danielblnc |
| `AMD-NR-ReShade-Installer\` | `master` @ `bbe817b` (v0.7.10) | Daniel's runtime version per game, RX 9000 policy, badges, async by default, locked-file messages, warning about another mod's `d3d11.dll` |

`opti-fixes` is a separate folder just for these fixes; NR/FG without upscaler remains in the main `opti`,
untouched. `external\` in `opti-fixes` is a junction to the main checkout. Build: `tools\build-optiscaler-fast.cmd`.

---

## 1. User requests and how they turned out

### 1.1 Choosing Daniel's runtime version (from 0.4.3 on)
- **Installer.** `payload.json` gained `releases.runtime` with 0.6.0, 0.5.1, 0.5.0 and 0.4.3; each one carries the
  `runtime` (patched pass1 + weights, for the add-on) and the `opti-runtime` (`dlssnr_amd_runtime-<v>.dll`, raw, for
  OptiScaler). The files are already on HF (`runtime/<v>/` and `opti-runtime/`).
- `PayloadManifest.WithRuntime(version, async)`, `RuntimeVersions()`, `RuntimeChoice` (survives a `With()` of
  an add-on/OptiScaler version applied afterwards), `PayloadPins.RuntimeChosen` / `RuntimeAsync`.
- `Work.RuntimeRunsOn` (table `AddonAcceptedRuntimes` + OptiScaler's `AcceptedRuntime`): only the versions
  that the chosen add-on/OptiScaler runs are shown.
- UI: "danielblnc version" row (`DanielVersionRow`) on the game's page; choice saved in `GameStore.DanielRuntime`.
- **Per-card policy** (the user's next request), in `Work.Runtimes.cs`:
  - RX 9000: all versions; **0.4.3 is the default** with a green **Recommended** badge; the others with an orange
    **Unstable** badge (`Work.RuntimeOffer/RuntimePick/BadgeFor`, `RuntimeOption.cs`, styles `Border.tag.good/.warn`).
  - RX 6000/7000: only the newest (0.6.0), no badge.
  - Texts in 21 languages (`Str.RuntimeRecommended`, `Str.RuntimeUnstable`, `Str.DanielVersionTip`).
- Tests: `RuntimeChoiceTests.cs`.

### 1.2 Button to turn async mode on/off
- **Add-on:** "Timing" (Same frame / Asynchronous) now also in the 64-bit panel (`ui/sections/performance.cpp`,
  `NetworkTiming`), writes `Inline` to `amd-nr.ini`. The runtime locks the mode when it sets up the staging, so it takes effect on the
  next launch (`RuntimeInline()` in `runtimes.inc` reads it once per session). In async: `RuntimeBusy()` does not use
  `kJobCounter` (it does not advance in async) and the `kInlineActive` check only applies in same-frame.
- **OptiScaler:** `[DlssNr] AmdAsync` (Config.h/.cpp, `OptiScaler.ini`), "Timing" combo in the menu
  (`DlssNr_Menu.cpp`), `AmdBridge::AsyncSession()` (read once). In async: forces the "after the finished
  frame" placement (a correction one frame late would accumulate in the upscaler's history), a single pass, and the slots
  retire only by fence (`AmdPreSr.cpp`, `async`; refusals logged by sampling).
- **Installer:** on RX 9000 with an unstable version, the installation turns async on (`Work.GoesInAsync`: `Inline=0` in
  `amd-nr.ini` / `AmdAsync=true` in `OptiScaler.ini`, only that key). 0.4.3 and RX 6000/7000 are left alone.
  Note: the published OptiScaler 0.4.9 ignores `AmdAsync`; only the `opti-fixes` build reads it.
- Measured outside the game (real runtime, RX 9070 XT, 1080p): game queue ~0.45 ms/frame in async against 15–20 ms in
  same-frame; frames that arrive while the network is busy are skipped (they go out without NR).
- `framecheck` counts skipped frames in async; `amd_runtime_init_smoke` gained `AMD_TEST_ASYNC`.

### 1.3 Protection: turning off without locking up the session
- `StallWatch::Clear()` (add-on `core/shared/stall_watch.h` and opti `StallWatch.h`).
- Add-on: `JobGate` turns `Enabled` off, stores `standDownMs`, clears the watch; the panel shows "off: the GPU held
  a job for X s; tick On to try again". 32-bit bridge: `WireStatus.stallMs`, **protocol v6**
  (`bridge_ipc.h`, `frontend32.cpp`, `host64.cpp`, `panel32.cpp`, `protocol_test.cpp`, `docs/x86bridge.md`).
- OptiScaler: `AmdBridge` turns `DlssNrEnabled` off (volatile), stores the note (`StandDownNote()`), toast + red text in
  the menu; turning "Enable NR" back on tries again.
- **Not tested in game.** framecheck does not go through `JobGate`. To test: `StallStandDownMs=5` /
  `AmdStallStandDownMs=5`, launch, watch it turn off, turn it back on.

### 1.4 RX 9000: mochizuki default in the add-on, lmxxf in OptiScaler
- No experimental checkbox: on RX 9000 mochizuki always goes in (`WantsMochizuki`); the add-on uses mochizuki when
  there is no `NrBackend` (`runtime_choice.h`); a new/our OptiScaler.ini gets `NrBackend=lmxxf` (`RunsLmxxf`).
- **Warning:** see section 3 — on driver 32.0.32015 the published mochizuki (0.4.9) crashes the game. Do not publish the
  "mochizuki default on RX 9000" policy before publishing the fixed shaders.

---

## 2. Other changes in this round

### Installer
- **A locked file names the process** (`Engine.Holders.cs`, Restart Manager): "dxgi.dll is open in BatmanAK.exe
  (PID …)…"; used in installation, pre-check and uninstallation (`Engine.OpenElsewhere`, `Engine.Write`).
- **Warning about another mod's `d3d11.dll`** on the D3D10/11 routes (`Work.CheckForeignD3d11`). Origin: the Batman report
  (RX 6600, d3d11 wrapper from 2017 + `ori_d3d11.dll`; ReShade "Skipping swap chain… without a proxy Direct3D
  device"; the crash was unrelated to Daniel — the add-on/runtime never loaded).
- Tests: `HeldFilesTests.cs`; 4 old tests adjusted for the new text. Last `dotnet test`: 340/340;
  `gate.ps1 -Ui`: build, line-limit, ui and flows OK (the test ran after the adjustments).
- **Review build:** `AMD-NR-ReShade-Installer\publish-review-fixes\AMD-NR-ReShade-Installer.exe`. The
  `config.json` in that folder points `ManifestUrl` to `http://127.0.0.1:9/payload.json` (fails fast and falls back to the
  local `payload.json`, which has `releases.runtime`). `dotnet publish` overwrites that config: redo it afterwards.

### Add-on: automatic fallback when mochizuki dies during the build
- The mochizuki DLL keeps `dlssnr-amd\build.pending` during the build. At launch, if it exists and mochizuki
  is chosen, the add-on runs danielblnc, writes `NrBackend=danielblnc`, renames it to `build.failed` and shows a
  warning in the panel (`runtime_choice::MochizukiBuildDied`, `mochizuki.inc`, `general.cpp`). Choosing mochizuki
  again tries once more (with detailed logging). Tested with framecheck on the breaking driver: full cycle OK.
- 32-bit bridge: the fallback happens in the helper; the warning only appears in the log.
- Possible false alarm: closing the game during the seconds of compilation counts as a dead build.

### OptiScaler: input in Assetto Corsa (patch from player GhostNappa, reviewed)
Origin: `E:\FDM-downloads\Optiscaler fix - Assetto Corsa.txt` (parts B to E; part A did not come). Old pending item:
`opti\handoff\assetto-corsa-menu-input.md`.
- **B, as received:** DirectInput "Other" devices (wheel, pedal, joystick) are no longer blocked with the menu
  open (`ShouldBlockDirectInputOtherLocked` = false). Zeroing `DIJOYSTATE` put the axes at the extreme (pedal
  floored).
- **C, as received:** an accepted hotkey marks `lastInputTick` (`menu_common.cpp`), so the same key seen by message and
  by polling does not toggle the menu twice.
- **D+E, with an adjustment:** the `WH_MOUSE_LL` hook also goes into the game process, but only with the menu open, the WndProc
  taken over and a DirectInput mouse in exclusive mode (`hkDirectInputSetCooperativeLevel`, vtable 13,
  `DirectInputMouseExclusive`). The hook's buttons go to `LowLevelMouseDown` and polling ORs them with the
  async state, instead of being turned off; the position still comes from polling. The original patch turned the hook on all the time and
  in any game without a WndProc (risk of locking up the system mouse and of losing MSFS's clicks).
- Build: `exports\assetto-input\OptiScaler.dll` (= `exports\release-local`). Not tested in Assetto (not installed
  here); waiting on GhostNappa's feedback.

### mochizuki DLL (opti-fixes, `OptiScaler/dlssnr/backend/mochizuki_runtime/`)
- `BuildMarker` (`build.pending` / `build.failed`) and verbose mode: with a previous dead build, the next one logs
  each prewarm and core pipeline, with the `.spv` (`mz_interpose`: `Capture::Verbose`, `NameOf`).

---

## 3. mochizuki × AMD driver 32.0.32015.2008 — investigation and fix

### Symptom
Reports (BeamNG, RX 9060 XT) and the user's own Cyberpunk: `mochizuki_nr.log` ends at "building the
network" and the game crashes. Reproduced on the user's RX 9070 XT as soon as he installed 32.0.32015: 0xC0000005 in
`vkCreateComputePipelines`, new and old DLL. The same build worked on 32.0.31041.

### Cause 1 — crash during compilation
- Tool `spvprobe` (compiles a `.spv` with the real layout, one per process): only 4 of 60 shaders crash the driver:
  `g_attn`, `g_ffwd3`, `g_ffwd3w`, `g_fswinpds128` (the `temporal_*` ones were a false positive from a wrong layout in the probe).
- Trigger (source cut-down of `ffwd3`): loops that index `coopmat` arrays with the loop variable
  (`h[k]`; in `fswinpds128`, loops `for (int m = mh; …)` that `unroll_glsl.py` left in because `mh` is a constant from
  an outer copy).

### Cause 2 — wrong image (blotches, darker, flicker in Cyberpunk)
- With the crash worked around, the network ran but with the wrong image. Ruled out with tests on the GPU: layout of the FP8
  A/B/accumulator fragments (FP32 and FP16), FP8 16×16×16 MMA exact against the CPU, FP8 conversions (the clamped ones saturate; the
  raw conversion above ~464 becomes NaN, but forcing saturation everywhere does not fix it), FP16 subnormals, barriers
  (`NR_COHERENT_BARRIERS=0`, `NR_INV_BARRIERS=0`), `NR_NO_PERSIST`, upstream's `spirv_precise.py`, the reference
  DLL. Interop/encode correct (output = input at strength 0).
- **Layer-by-layer localization:** `nr_graph` (the command-line front end of `nr_graph.cpp` itself,
  compiled outside the DLL) with the saved native plan (`planout`), `--no-reuse --dump-all`. Recorded on 31041
  (reference, deterministic) and on 32015: everything identical up to step 059; **first divergence at 060,
  `b031l3 CCVit1DAttention` (kernel `vitattn`)**. Inside it (`NR_VDUMP`): softmax denominator correct,
  numerator saturated at ±448.
- **Cause:** the same driver defect, here producing wrong math silently — `uint` loops over `coopmat`
  arrays (`ctx[b][n]`, `vf[n]`…). `unroll_glsl.py` only unrolled `int` loops.

### Fix (opti-fixes, `third_party/mochizuki/windows/build/`)
- `unroll_glsl.py`: `with_value()` (substitutes the outer copy's constant in the inner loops' headers and
  unrolls again) and support for `uint` loops (`16u`, `uint(...)`, copies `const uint V = Nu`).
- `build_network.py`: `UNROLLED = {fswin_t, ffwd3_t, attn, vit_attn}`.
- 6–8 shaders change; the others stay identical. Checked: the transformations do not change the result (on 31041 the
  fixed shaders give output **bit for bit identical** to the reference; on 32015, different forms of the same kernel give
  identical outputs).
- Build with everything: `opti-fixes\exports\mochizuki-fix2\` (DLL + `dlssnr-amd\shaders`). **This is the set to
  publish** when the user approves (new version of the `mochizuki` component in the payload).
- `nr_runtime.cpp` gained `NR_GRAPH_ARGS` (diagnostics, only via environment variable). Decide whether it stays.

### Result on 32015 (against 31041)
| case | mean error before | after |
|---|---|---|
| 1080p (1/2/3 passes, R10, F32, prep, no motion) | ~0.08 | ~0.001 |
| 1707×961, 4K | 0.09–0.17 | 0.001–0.002 |
| **1280×720** | 0.10 | **0.011, ~2% darker** |
~0.001 = 1/3 of an 8-bit step (same order as the mochizuki × NVIDIA DLL difference in upstream's `docs/ngx-verification`
); it comes from rounding by one FP8 step in 0.07% of the values of step 060, propagated.
Network time the same as before (~8–9 ms at 1080p in Cyberpunk).

### 720p / FSR Quality, Balanced, Ultra Performance (found later, `mochizuki-fix3`)
- User's video (1080p output): blotches like those at 1080p before the fix in Quality (1280×720),
  Balanced (~1129×635) and Ultra Performance (640×360), not in Performance (960×540). NR runs at the render
  resolution.
- Pattern: the ViT attention works in blocks of 64 tokens. Good resolutions have token counts that are multiples of 64
  (540p 192, 961p 448, 1080p 640); the bad ones do not (360p 96, 635p 240, 720p 288) and go through the "guarded" body of the
  last block (`include/vit_attn_vt_chunk.glsl`, `NR_VT_GUARD 1`).
- Cause: 32015 miscomputes the choice between a loaded `coopmat` fragment and a zeroed one
  (`if (...) load else kfr = NR_FRAG_A(0.0)`, `if (...) pe = NR_FRAG_E4M3(0.0)`).
  Proof without the old driver: `nr_graph --vattn-tokens 608` at 1080p (tokens 576+ are zero, so the expected output
  is the harmonic mean of the outputs for 576 and 640). Original body: 15% below the prediction (corr 0.977); fixed
  body: ratio 1.0009 (corr 0.9994).
- Fix (shader): the padding tile's K loaded from the clamped live tile (`min(j0 + kb, jlast)`), padding logit
  = 0 and probability = 0 in the f16 values, with no matrix select.
- `unroll_glsl.py`: also unrolls loops with `continue` (copy in `do { } while (false)`) and loops up to
  `.length()` of a 16×16 subgroup `coopmat` (8 per lane, subgroup fixed at 32). Only dynamic-count loops
  remain. `attn`, `vitattn*`, `fswin*32`, `fswinimage*` change.
- Battery (`wt\mz-driver-32015\harness\cmp.py mkfix5`) against 31041: 1080p/961p/4K ~0.001 (same as fix2);
  720p 0.011 → **0.0040** (corr 0.9996, +0.7% brightness). Still ~4× the others: a small deviation remains at 720p.
- Export: `opti-fixes\exports\mochizuki-fix3\` (same DLL as fix2 + new shaders). **Replaces fix2 as the
  set to publish.** Installed in Cyberpunk (backup `_optiscaler_backup_2026-10-03_premzfix3`).
- Tests for this part: `wt\mz-driver-32015\nrgraph\vitmask.py`, `vittail.py`, `gemmtail.py` (the ViT GEMMs
  have no final-block problem).

- In-game test of fix3 (Cyberpunk, 2026-10-03 22:54): the user confirmed the blotches are gone. Log without errors:
  shaders recompiled on detecting the change, network built at 1080p, 720p (1 to 3 passes), 635p, 540p, 360p,
  560×350 and 587×330.

### Round on driver 31041 (closes 720p)
- `driver-round.ps1 old` and `new`: with fix3, both drivers give **byte-for-byte identical** output in every case
  (1080p, 961p, 4K, 720p, 635p, 360p, passes 2/3, formats; only `prep` varies 0.001 between runs). 32015 is
  fully worked around.
- The "0.004 remainder" at 720p was not a defect: it is the reference `b_fin_720` (original shaders on 31041) that is
  wrong. mochizuki's original kernel gets the incomplete block of the ViT attention wrong **on any driver** when
  there is real data after the end of the sequence (as at 720p).
  Proof (`nrgraph\vitreal.py`, 1080p `--vattn-tokens 544`, on 31041): the independent implementation of the same
  shader (`NR_VTRANS=0`, with and without `NR_VKMASK`) matches fix3 (0.2–0.5% of bytes, rounding level; at
  640 tokens the original and the independent one already differ by 0.44%); the original differs by 58%, fix2 by 83%.
- 1080p: fix3, fix2 and the reference differ by ~0.001 from one another (deterministic runs). It is FP8 rounding of the
  different forms of the code, same order as the mochizuki × NVIDIA DLL difference.
- Possible notice to upstream (mochizuki0323): the partial final block of `vit_attn_vt_chunk.glsl` (NR_VT_GUARD 1)
  gives a wrong result; the fix3 correction works for them too.

---

## 4. Test installations in the games

| Game | What is installed | Backup |
|---|---|---|
| Cyberpunk 2077 (`E:\Games\Cyberpunk2077\bin\x64`) | `dxgi.dll` = OptiScaler from `opti-fixes`; `dlssnr_amd_pass1-3.dll` = raw runtime 0.6.0; mochizuki = `mochizuki-fix3` (DLL + shaders); `OptiScaler.ini` with `NrBackend = mochizuki` (the user chose it) | `_optiscaler_backup_2026-10-03_prefixes`, `…_premzverbose`, `…_premzfix`, `…_premzfix2`, `…_premzfix3` |
| GTA IV 32-bit (`E:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV`) | `amd-nr.addon32` + `amd-nr-host64.exe` (protocol v6) + patched runtime 0.6.0; mochizuki = `mochizuki-fix3` (before there was one from 2026-09-27) | `_amdnr_backup_2026-10-03_prefixes`, `_amdnr_backup_2026-10-03_premzfix3` |

Cyberpunk's `OptiScaler.ini` still has the leftover `AmdEncoding = 0`.

---

## 5. Tools (stable copy in `E:\Projetos\opti_rr_dlss5\wt\mz-driver-32015\`)

- `probe\spvprobe.exe file.spv KINDS PUSH` — compiles a shader with a per-binding layout (`B` buffer, `I` storage
  image, `S` sampler) and tells whether the driver survives it. `bisect.py` / `cut.py` / `mini.py` — define/source cut-downs/
  minimal shaders. `fp8run.exe` + `*.comp` — GPU tests (`LAYOUT`, `MMA`, `ACC`, `CMP`, `STRIDE`).
  `satfp8.py` — marks FP8 conversions as saturating (experiment, did not fix it).
- `nrgraph\nr_graph.exe` (build: `build_nrgraph.cmd`), `planout.exe W H FILE` (native plan), `plan1080.txt`,
  `plan720.txt`, inputs `in1080/in720.rgba32f`, reference `dump_old_31041_1080p\` (index + 184 files;
  `184-k566` varies between runs, ignore it), `out_old.rgba32f`. `vitloop.py` / `vitedit.py` — variants of
  `vitattn` compared at step 060. `unroll_uint.py` / `unroll_full.py` — unroller prototypes.
- `harness\` — `mz_e2e` (D3D12 → mochizuki DLL; `E2E_PAN/E2E_W/E2E_H/E2E_FORMAT/E2E_PREP/E2E_NOMOTION/
  E2E_STRENGTH`), `run.cmd`, `battery.ps1` (10 cases), references `b_fin_*.bin` (driver 31041, 2026-10-01/02) and
  the test folder `mkfix2`.

---

## 6. Open decisions for the user

1. Publish the fixed mochizuki (`mochizuki-fix3`) and only then the "mochizuki default on RX 9000" policy.
   Until then, either keep the old checkbox, or have the installer not make mochizuki the default on driver 32015+.
2. Notify upstream (mochizuki0323) of the partial final block of the ViT attention (section 3).
3. Test in game: protection without locking up the session (section 1.3) and async on a weak card (RX 9060/6000).
4. `NR_GRAPH_ARGS` in `nr_runtime.cpp`: keep as diagnostics or remove.
5. Publication order when approved: opti (new tag), add-on, installer, payload (`releases.runtime`,
   new `mochizuki` component), mirror on Extras.
