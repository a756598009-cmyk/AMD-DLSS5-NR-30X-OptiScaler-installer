# RDNA3 (RX 7000) prep for an experimental release - 2026-10-19

Two local branches, nothing pushed, published or posted:
- `wt-rdna3` (opti), branch `mochizuki-rdna3`
- `wt-installer-rdna3` (installer), branch `rdna3-experimental`, made from `master` (f96a96b)

Work files (not in git): `wt-rdna3\exports\rdna3-work\` (`build.cmd` full build into `exports\mochizuki-rdna3`,
`build-dll.cmd` DLL only, `build-main.cmd` + `mainsrc\` + `ref-main\` = the main branch's build, `ab.ps1` the harness
A/B, `ab\<variant>\` its runs and logs).

## 1. Merge of dlss-neural-rendering (0.5.5-opennr, mochizuki v0.0.7)

- `27b5e02ef` committed the uncommitted port first, with the kit sources moved to `tools\rx7000-kit\` (exports is
  ignored; the scripts still write into `exports\rx7000-kit\`). Kit zip, models, caches and outputs stay out.
- `626502556` the merge. Conflicts: `nrvk.hpp` (v0.0.7's `SharedPipeline` vs our alias bindings / twin binding),
  `nr_graph.cpp` (3 hunks: v0.0.7 moved arena allocation, upload and zero fill into `GraphArenas`/`make_images`),
  `nr_runtime.cpp` (session destroy), `nr_runtime.hpp` (config fields, both kept). `MochizukiNrRuntime.cpp`,
  `UPSTREAM.md`, `build_network.py`, `build-mochizuki-runtime.cmd` merged by themselves.
- Twins under v0.0.7's shared memory: the activation twin now lives in `GraphAct` (so a network that takes over
  another's arena via `act_arena` also takes its twin), the weight twin in `GraphArenas::wgt_twin`, the plan tail's
  FP16 copy in `GraphStaging::twin`. `retarget` zeroes the activation twin and copies the tail twin; `build_shared`
  binds the slot's twins (throws if missing). The pipeline-sharing key also carries alias binding numbers. An RDNA3
  network is never offloaded (`download_network_weights` refuses: the twins would have to move too).
- Style grade: rdna4's `_grade` SPIR-V is the whole post block with FP8 coopmat (Float8EXT capabilities), so it cannot
  be reused. The RDNA3 tree got v0.0.7's hook (same lines in `linux/shaders/rdna3/fswin_t.comp`), `nr_style_grade.glsl`
  (plain f32 ALU) and the `style_grade` table; the RDNA3 `_grade` SPIR-V has no Float8. `build_network.py`'s grade loop
  now uses the selected tree. Forced on the 9070 XT, Natural runs and differs from Standard (graded).
- `shaders-rdna3`: 53 network (+grade), 11 temporal (+3 grade), 15 runtime; built as before (`--tree linux --no-quad`,
  unrolled, `NR_EDGE_BODIES=0`).

RDNA4 identity (harness, 40 frames, 1 pass, RGBA16F, RenoDX stage) against `ref-main` (built from
`git archive dlss-neural-rendering`): `dlssnr-amd\shaders` identical (`diff -r`), output byte-identical at 1080p for the
new model (`01588ca6...`), the old model (`04a421e7...`) and Natural (`3916aa46...`); rechecked with the final DLL
after tasks 2-5. The new/old hashes are also the kit's existing references.

Forced RDNA3 on the RX 9070 XT (driver 26.9.2): builds (16.2 s cold at 1080p) and runs both models; picture wrong as
expected.

## 2. Startup self-check (layout)

`OptiScaler/dlssnr/backend/mochizuki_runtime/mz_rdna3.{h,cpp}`, called from `Vulkan::Create` after the device exists
(`Rdna3Layout`). The kit's six probes (moved to `mochizuki_runtime/rdna3_probe/`, embedded as SPIR-V by
`glslang --vn` in the build script) run as tiny dispatches on the network's own device/queue: acc-f32, acc-f16,
frag-a, frag-b, wmma-lane-halves, kpair-swap, same checks as `vkprobe`. Any mismatch:
`[mochizuki] RDNA3 network off: <check: observed, expected>` (full text in the log) and Start throws
`[unsupported] RDNA3 network off: <first failure> (and N more, mochizuki_nr.log)`, which the existing path latches for
the adapter; the host keeps NR off and its status carries the reason. Verdict cached in
`dlssnr-amd\rdna3-check.txt` keyed `<deviceId>-<driverVersion>`.

Test, `MZ_FORCE_RDNA3=1` on the 9070 XT: fails cleanly in 124 ms with
`acc-f32: component c of lane l holds element (8(l/16) + c, l%16) (gfx12's map), expected (2c + l/16, l%16); acc-f16:
...; frag-a: 8 components a lane, expected 16; frag-b: ...; wmma-lane-halves: 640 of 1024 product elements wrong (384
taken from the other lane half)` (kpair-swap passes, as in the kit). Second start: cached, no dispatch. Not forced:
FP8 network, nothing runs. Diagnostic switch `MZ_RDNA3_CHECK=0` (no checks) / `=pipelines` (skip the probes only).
Risk left: the probes run unprotected in-process; an LLPC crash on those six tiny shaders would take the game down.

## 3. Protected compile

`mzr3::PipelineCheck`: before the first RDNA3 build on a driver (and shader set), on the builder thread and before it
takes `CoreBuildMutex`/`submitMutex`, so the game is not stalled (frames pass through as "building"). The child is
`rundll32 "<this DLL>",MochizukiNrPipelineCheck <LUID> <threads> <list> <log>`: no extra shipped file. It makes its
own device on the adapter (network's features, no FP8) and compiles every `.spv` of `shaders-rdna3` (recursive) and
`shaders\renodx` (99) like pipecheck (wave32, robust access off, layout reflected, no cache) on min(hw/2, 8) threads at
below-normal priority, logging start/ok/fail per pipeline. 300 s without progress = hang; a crash with several in
flight reruns each alone to name it. Failure: `[mochizuki] RDNA3 network off: g_attn.spv crashed AMD's shader
compiler (exit 0xC0000409)` / `... did not compile in N s` / `vkCreateComputePipelines failed on X (VkResult n)`,
session latch `rdna3Off` (EnsureNetwork returns it, so the status shows it), verdict kept in `rdna3-check.txt` with a
hash of the shader names+sizes; the next start then fails at Start with the cached reason. If the child cannot run
(no rundll32, no device) the network builds unprotected and nothing is cached.

Timing on the 9070 XT (forced, `MZ_RDNA3_CHECK=pipelines`): 12.3 s cold for 99 pipelines on 8 threads (slowest
`g_fswinpds256` 5.4 s). The AMD driver cache is per executable, so it warms nothing for the game: the network's own
cold build after it still took 15.7 s. Once per driver/shader set.
Tested: pass, cached pass (no child), crash hook (`MZ_TEST_RDNA3_COMPILE_CRASH=g_attn.spv` -> named correctly in 7.3 s),
hang hook (`MZ_TEST_RDNA3_COMPILE_HANG=g_vitattn.spv`, `MZ_TEST_RDNA3_TIMEOUT_MS=8000`), cached failure at Start. No
rundll32 left behind. Possible concern: some AV/anti-cheat may dislike a game spawning rundll32.

The kit harness (`tools\rx7000-kit\mz_e2e.cpp`) now stops on "network off" instead of waiting 15 min, and takes
`E2E_RENO_STYLE=n`. The kit zip itself was not repacked (it still has the pre-merge DLL).

## 4. ACO slot

`Vulkan::Rdna3Aco` (only when ACO was asked for): looks for `dlssnr-amd\aco-gfx11\{records,shaders}` and asks
`nr::pal::takes_machine(mach)` (new in `nr_pal_binary.hpp`, gfx1200/1201 only; mach from the device id: 0x41 gfx1100,
0x46 gfx1101, 0x47 gfx1102). Today: `[mochizuki] ACO off for the RDNA3 network: no gfx11 ACO records
(dlssnr-amd\aco-gfx11\records\); the driver compiles it` or `... the PAL adapter takes gfx1200/gfx1201 code objects
only`. If both were true it configures that bundle (and the core accepts an RDNA3 bundle only when named
`aco-gfx11`), Info reports network 2 and the pipeline check is skipped. What mochizuki's records need: see
`UPSTREAM.md` RDNA3 section (records named by the hash of `linux/build/build_network.py rdna3` SPIR-V in
`aco-gfx11\shaders\`, gfx11 support in the PAL adapter's register/user-data handling). Untested by construction.

## 5. Menu (`DlssNr_Menu.cpp`)

`MochizukiNrInfo::network` (new field at 348, sizeof stays 352): 0 FP8, 1 RDNA3 FP16, 2 RDNA3 ACO. On the mochizuki
page under the pass line: `RX 7000 (experimental, FP16 network)` with a help marker (slower, ~2 GB more VRAM at 1080p,
checked before the first build), or in red `RX 7000 network off: <reason>` (parsed from the runtime status, which
carries "RDNA3 network off: ...") with a help marker on what was checked and where the verdict is. ACO and int4 are
greyed out there with "Not on the RX 7000: ..." help. OptiScaler built (`tools\build-optiscaler-fast.cmd`, submodules
borrowed from `opti\external` through junctions, removed afterwards). Not seen in a game.
Edge left: with MochizukiInt4 on (and ACO), the host still sends the old-model flag on RDNA3.

Commit: `8c95d2ef3`.

## 6. Installer (`rdna3-experimental`)

- Component layout: new payload component `mochizuki-rdna3` (archive `mochizuki-rdna3-<version>.zip`, paths
  `dlssnr-amd.shaders-rdna3[.runtime|.temporal]/x`, destinations `dlssnr-amd\shaders-rdna3\...`), on demand. It is
  separate because v0.8.4 pins and installs every file of the `mochizuki` component on every RX 9000 (32 MB wasted
  there). Checked with v0.8.4's own Core (9cc5ace, built from `git archive` in scratch) on a payload that carries it:
  parses, pins 0 shaders-rdna3 files, and its `Engine.IsAllowed` accepts all 85 destinations (and the runtime's
  `rdna3-check*`, `pipeline-rdna3.cache`, `prewarm\manifest-rdna3.txt`).
- `tools\pin-mochizuki.ps1`: accepts `dlssnr-amd\shaders-rdna3\` (one folder level) and writes the second archive,
  the `mochizuki-rdna3` component and its upload line. Tried on a scratch copy of payload.json from
  `exports\mochizuki-rdna3` (85 files, 9.2 MB zip). The real payload.json is untouched.
- Core: `PayloadManifest.MochizukiRdna3Component`, `WithoutMochizukiRdna3()` (shares a `Without` helper with
  `WithoutLmxxf`), `Pins()` adds its files to `MochizukiFiles` when present; `Work.IsRx7000` (AMD, device 0x74xx or
  "RX 7xxx"/"PRO W7xxx" by name; iGPUs out), `Work.HasMochizukiRdna3`, install report line, uninstall sweeps what the
  RDNA3 runtime writes.
- App: `SystemState.Rx7000`, `GameEntry.MochizukiRdna3` (follows the folder until set), `GameSheet.Rdna3.cs`: a box
  "mochizuki on the RX 7000 (experimental)" in the NR runtime section, OptiScaler route, version carrying the
  component, RX 7000 card; off by default; the note warns (slower, ~2 GB more VRAM at 1080p, self-check, NR off with
  the reason). Ticked: mochizuki + the component go in and a fresh ini runs mochizuki; `Selected()` strips the
  component from every other install. `InstallLog` gets an `rx7000` line.
- Strings: `Str.MochizukiRdna3Toggle`, `Str.MochizukiRdna3Note` translated in all 21 files (the project translates
  every string, x-pirate included).
- Tests: `MochizukiTests.Rdna3.cs` (pins with/without, destinations allowed, install ticked/unticked, uninstall
  leaves the folder as it was, `IsRx7000` cases). `dotnet test`: 414 passed.
- uishot: new `Flows.Rdna3.cs` (fakes the machine as an RX 7000 through `Session.Machine`'s private setter, seeds
  1.0.4 with the component, checks the box and note, ticks, installs, restores the real card and installs again).
- Gate (`tools\gate.ps1 -Ui`): build, test (414), publish, ui and flows OK; line-limit FAIL on
  `tests/AmdNr.Core.Tests/GraphicsTests.cs` (504 lines), master's file as it is, untouched here. Screenshots:
  `C:\Users\matheus\AppData\Local\Temp\amdnr-gate-473cde0b551242148434e0cf942357a7\shots\flow-2-optiscaler-rx7000.png`
  (box unticked with its note) and `flow-2-optiscaler-rx7000-ticked.png` (the sheet scrolls back to the top after
  the refresh, so the box is out of view there).

## Commits

- opti `mochizuki-rdna3`: `27b5e02ef` (port + kit sources), `626502556` (merge), `8c95d2ef3` (checks, ACO slot, menu)
- installer `rdna3-experimental`: `dd83cc3`

## Before release

1. The volunteer's kit result on a real RX 7000 (layout probes PASS, pipecheck clean, PSNR >= 40 dB). Repack the kit
   first if the new DLL should be in it (`tools\rx7000-kit\build-kit.cmd`, `pack-kit.ps1`).
2. A game test on an RX 7000 with OptiScaler built from this branch: menu line, VRAM, the check's 12 s + cold build.
3. Release: merge `mochizuki-rdna3` into main, opti version bump, pin with `pin-mochizuki.ps1` (makes both archives),
   installer from `rdna3-experimental`. Old installers keep working (above).
4. Later: mochizuki0323's gfx11 ACO records and gfx11 in the PAL adapter (section 4).
