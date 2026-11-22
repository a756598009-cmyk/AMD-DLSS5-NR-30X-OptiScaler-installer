# Handoff: mochizuki RDNA2, phases 1 and 2 (2026-10-27)

Worktree `wt-rdna2`, branch `mochizuki-rdna2` (from `dlss-neural-rendering` ca2544ec3, 0.5.8-opennr). Local commits
only, nothing pushed. Plan: `RDNA2-MOCHIZUKI-PORT.md` (sections 5.1, 7, 9). Phase 0: `handoff/PHASE0-probe.md`.
Clean room: the GPL RDNA2 repository was not opened, searched, fetched or cloned, nor anything derived from it (line
added to the plan's section 14). Sources used: the plan, the MIT mochizuki tree (incl. mauri870's rdna3), the RDNA3
handoffs and the phase 0 report.

## Result

- RDNA2 network runs end to end on the RX 9070 XT forced (`MZ_FORCE_RDNA2=1`), both models, 720p and 1080p, every
  result >= 42.5 dB against the RDNA4 FP8 network.
- RDNA4 output byte-identical to the 0.5.8 release DLL (720p, 1080p, both models), and RDNA3 forced output too.
- Slow, as layer A should be: 86 ms (Small model, 720p) to 340 ms (310.8 model, 1080p) a frame on the 9070 XT.
- First cold build of the RDNA2 pipelines is long (about 70 min serial on this PC, see below).

## What was done

### Phase 2: the RDNA2 network (layer A)
- `third_party/mochizuki/linux/shaders/rdna2/`: copy of `rdna3/` whose `include/coopmm.glsl` implements the same macro
  interface without cooperative matrices.
  - A fragment is an array per lane of the 32-lane subgroup, in the layout the RDNA3 kernels were written against:
    A `float16_t[16]`, component c of lane l = (l%16, c); B `float16_t[16]`, (c, l%16); accumulator `float[8]`
    (FP16 accumulator `float16_t[8]`), (2c + l/16, l%16). Row m of a product is formed from lane half m%2 of both
    operands, as on gfx11, so the RDNA3 kernels' register-built operands and the `kswap` weights stay valid.
  - `NR_MMA` (`nr_mma`): the 8 A rows a lane needs come from its half by `subgroupShuffle` of packed FP16 pairs
    (64 shuffles a 16x16x16), summed with B into FP32 by `NR_DOT2`, the single place of the inner product:
    `fma(float(a.x), float(b.x), fma(float(a.y), float(b.y), s))` (phase 0: this is `v_dot2_f32_f16` on gfx1030 and
    gfx1201). The FP16-accumulator overload sums in FP32 and rounds once.
  - Loads/stores are per-element loops in `do { } while (false)` macros, with `subgroupMemoryBarrier()` after each
    store and before each load (the collective op ordered them; a wave stores an accumulator to LDS and reads it back).
    Every site's array has the fragment's own element type (checked once at build time with decoy overloads, since a
    mismatch would compile as a silent conversion).
  - RoundingModeRTE for 16-bit in every RDNA2 entry point unless it already asks for it (`NR_HALF_RTE`), per phase 0
    (gfx103x narrows toward zero by default). The SPIR-V declares no CooperativeMatrix capability (spirv-dis).
- Other shaders touched, minimal, all because an array has no one-argument constructor: 13 sites
  `NR_FRAG_X(0.0)` -> `NR_FILL_X(0.0)`, `NR_FRAG_ACC(h)` -> `NR_WIDEN_ACC(h)` (`fswin_t.comp`, `vit_attn.comp`,
  `include/fswin_body.glsl`, `include/fswin_hw_attn.glsl`, `include/vit_attn_vt_chunk.glsl`). `noise_field.comp`
  (no coopmm include) declares RoundingModeRTE itself.
- `pipelines.json`: RDNA3's (no int4) without the persistent runs' 9 pipelines (`fswinp*`, `fswinpds*`, `fswinpup*`),
  which the RDNA2 host never builds. Markers identical (`kswap 1`). 44 network + 11 temporal + 15 runtime files.
- `linux/build/arch/rdna2.sh`; `windows/build/build_network.py rdna2 --tree linux --no-quad`;
  `tools/build-mochizuki-runtime.cmd` writes `dlssnr-amd/shaders-rdna2`.

### Phase 1: host
- `nr_arch.hpp`: `nr::Arch { rdna4, rdna3, rdna2 }`, `fp16_network()`, folder and cache names. `bool rdna3` became
  `Arch arch` in `RuntimeConfig` and `nrvk::Context`; `Context::fp16()` is what `rdna3` meant (twins, Linux kernel
  set, `kswap`, tile counters off by default). RDNA2: `shaders-rdna2`, `pipeline-rdna2.cache`, ACO off, `adopt`
  needs neither the cooperative matrix feature nor a matrix configuration.
- RDNA2 builds no persistent runs (`nr_graph.cpp`, as `NR_NO_PERSIST=1`): their waits are poll budgets sized for
  RDNA4's speed. With them, on the forced 9070 XT: the 310.8 model at 720p changed run to run (43.8 to 53.7 dB) and
  at 1080p its C=128 run (claiming off the counter) took 1.2 s and gave 24 dB; a 16x/64x budget made 720p repeatable
  but the 1080p C=128 claim path still never finished its waits. Per layer it is the same arithmetic.
- `MochizukiNrRuntime.cpp`:
  - RDNA2 by device id: Navi 21/22/23 0x73A0-0x73FF, Navi 24 0x7420-0x743F, APUs 0x163F (Van Gogh), 0x1681
    (Rembrandt), 0x164E (Raphael), 0x1506 (Mendocino), whatever the driver reports (phase 0: the RDNA2 driver branch
    reports and emulates FP16 16x16x16 matrices, so an RX 6000 would otherwise get the RDNA3 network). An RDNA2 id
    overrides `MZ_FORCE_RDNA3` too. Any other AMD device with neither FP8 nor those matrices also gets RDNA2 (was
    `[unsupported]`). `MZ_FORCE_RDNA2=1` takes it on any card.
  - RDNA2 requires subgroup shuffles in compute and `shaderRoundingModeRTEFloat16`; FP8 and the matrix extension are
    not required in the precheck and are left out of the device's chain and extension list.
  - Several Vulkan devices on one adapter: FP8 > RDNA3 > RDNA2, then the newest driver.
  - `Fp16Checks`/`Fp16Pipelines` (were RDNA3-only): the protected pipeline compile (rundll32 child, build stage 6) for
    `shaders-rdna2` + `shaders\renodx`, verdict in `dlssnr-amd\rdna2-check.txt` (`MZ_RDNA2_CHECK=0` skips), 30 min
    without progress before a compile counts as hung (5 min for RDNA3); no layout probes for RDNA2. Off text
    `RDNA2 network off: ...`. Prewarm variant `-rdna2`. VRAM estimate adds the twins. `MochizukiNrInfo::network` = 3.
  - `mz_rdna3.cpp` child enables the matrix extension only when offered.
- Menu (`DlssNr_Menu.cpp`): `RX 6000 (experimental, FP16 network)` + help, `RX 6000 network off: ...` + help, stage 6
  text for the RX 6000, ACO/int4 greyed with the card's name. OptiScaler builds (`tools\build-optiscaler-fast.cmd`).
- Diagnostics: `NR_STEP_TIMES=n` (`nr_runtime.cpp`, any network): a timestamp after every network dispatch of pass 1;
  logs per frame the dispatches over 5 ms and, after n frames, a per-pipeline table. `Kernel::name` (SPIR-V file name).
- Harness `tools/rx7000-kit/mz_e2e.cpp`: `E2E_WAIT_MIN` (default 15), Destroy on a failed wait (keeps the partial
  pipeline cache), prints `network N, menu: <the page's line>`.
- Docs: `third_party/mochizuki/UPSTREAM.md` (table row, section "RDNA2", credit line, build counts).

## Commits (on ca2544ec3)
```
af7c0bece mochizuki: RDNA2 network (linux/shaders/rdna2) ...
e43204d3f mochizuki core: the network as nr::Arch ...
0b1cd9f7c mochizuki host: the RDNA2 network for RX 6000 ...
63c45c6ce rx7000-kit mz_e2e: E2E_WAIT_MIN ...
dcb90954b mochizuki RDNA2: shaders-rdna2 without the persistent runs' pipelines ...
9b319b4a0 mochizuki host: the RDNA2 pipeline check waits 30 min ...
(last)    mochizuki RDNA2: coopmm.glsl subgroup memory barriers, UPSTREAM.md, this handoff
```

## Files touched
- Shaders: `third_party/mochizuki/linux/shaders/rdna2/**` (new), `linux/build/arch/rdna2.sh` (new)
- Core: `windows/src/core/nr_arch.hpp` (new), `nrvk.hpp`, `nr_graph.cpp`, `nr_runtime.cpp`, `nr_runtime.hpp`
- Host: `OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrRuntime.cpp`, `MochizukiNrControls.h`,
  `mz_rdna3.cpp`, `mz_rdna3.h`; `OptiScaler/dlssnr/DlssNr_Menu.cpp`
- Build/tools: `tools/build-mochizuki-runtime.cmd`, `tools/rx7000-kit/mz_e2e.cpp`
- Docs: `third_party/mochizuki/UPSTREAM.md`, `handoff/HANDOFF-rdna2-phase1-2.md`; outside git:
  `RDNA2-MOCHIZUKI-PORT.md` section 14

## Measurements (RX 9070 XT, AMD Software 26.10.1 / Vulkan driver 0x008001A0, harness `mz_e2e`, test.png, 1 pass)

A/B against the 0.5.8 release DLL (`wt-rdna3/exports/rdna3-work/ab/rel058`, SHA-256 7e7f8234..., same as
`wt-upstream-merge/exports/release-0.5.8`), 40 frames, output SHA-256 prefixes:

| | new 720p | new 1080p | old 720p | old 1080p |
|---|---|---|---|---|
| RDNA4, 0.5.8 | 258FEC01 | 01588CA6 | 2A308F2C | 04A421E7 |
| RDNA4, this branch | 258FEC01 | 01588CA6 | 2A308F2C | 04A421E7 |
| RDNA3 forced, 0.5.8 | D68F26C1 | FA85987C | A2178F0E | FE4E456B |
| RDNA3 forced, this branch | D68F26C1 | FA85987C | A2178F0E | FE4E456B |

Checked three times through the session (the last with the final DLL). `shaders\` and `shaders-rdna3\` SPIR-V
byte-identical to the base build.

RDNA2 forced, PSNR against the RDNA4 FP8 output (`compare.exe`, peak 1.0, 0 NaN/Inf):

| | new 720p | new 1080p | old 720p | old 1080p |
|---|---|---|---|---|
| 1 frame (repeatable byte for byte, two runs) | 48.46 dB | 50.92 dB | 48.50 dB | 49.04 dB |
| 40 frames (spread over runs) | 44.4 - 55.6 | 56.4 | 48.0 - 54.3 | 42.7 |
| network ms (status line, steady) | 85.5 | 161.0 | 193.9 | 340.2 |
| RDNA4 network ms, same harness | 4.1 | 6.6 | 6.3 | 9.6 |

The 40-frame spread is not the network: the RenoDX stage's normalisation and look history advance by wall-clock
frame time (`nr_renodx.cpp` `plan_frame_scale`, clamped 1/240 to 0.1 s), and RDNA2's 720p frames (85-145 ms)
straddle the 0.1 s clamp, so runs differ (about 55 dB between two runs); at 1080p every frame is over 0.1 s and the
output repeats. Frame 1 does not depend on the clock and repeats exactly.

Build and memory:
- First RDNA2 build ever (no prewarm manifest, pipelines serial, AMD driver cold): 38.5 min with the persistent
  pipelines (Small model, 720p), 70 min for the final set (26 graph pipelines, 36 in all; plus ~90 s for the 310.8
  model's extra ones). Later builds 1-2 s (pipeline cache + manifest, 8 threads).
- Pipeline check (stage 6) with the persistent pipelines, cold: 98 of 99 in 1782 s on 8 threads, slowest
  `g_fswinp256` 956 s, then `g_fswinpup256` declared hung at 300 s without progress: hence the 30-min RDNA2 timeout
  and the persistent pipelines dropped. Final set (90 files) passes; measured only warm (0.4 s, the child's driver
  cache already had them); cold not re-measured (estimate 15-25 min).
- VRAM growth while building: 1048 MB at 720p (estimate 1336), 1834 MB at 1080p (estimate 2309).
- Menu line through the harness (`MochizukiNrGetInfo`): `network 3, menu: RX 6000 (experimental, FP16 network)`;
  RDNA3 forced `network 1, menu: RX 7000 (experimental, FP16 network)`; RDNA4 `network 0`.

## Per-pipeline timing of the emulated network (phase 3 input)

`NR_STEP_TIMES=20`, RX 9070 XT forced to RDNA2 (gfx1201 running gfx103x-style code: the ranking is the useful part,
absolute numbers need an RX 6000). Full tables in `exports/rdna2-work/step-times-final.txt`.

Small model (default), 720p, 90.4 ms in 77 dispatches / 1080p, 173.3 ms:

| pipeline | n | 720p ms | 1080p ms |
|---|---|---|---|
| temporal_pre_fp32nh (pre block) | 1 | 9.57 | 20.31 |
| temporal_post_fp32 (post block) | 1 | 9.12 | 19.93 |
| fswin256 | 2 | 8.34 | 15.40 |
| fswin64 | 2 | 6.06 | 12.79 |
| fswin128 | 2 | 5.44 | 12.08 |
| fswin32nh | 2 | 5.37 | 11.31 |
| gemmproj | 15 | 5.31 | 6.51 |
| fswinfusedup128 / 256 / 64 / 32 | 1 each | 5.08 / 4.41 / 3.63 / 2.73 | 10.06 / 7.20 / 7.49 / 5.58 |
| attn | 4 | 4.26 | 6.35 |
| vitattn | 8 | 3.01 | 7.36 |
| fswindsp64 / 256 / 32nh / 128 | 1 each | 2.85 / 2.82 / 2.77 / 2.44 | 5.85 / 4.32 / 5.67 / 4.24 |
| ffwd3 | 4 | 2.25 | 3.24 |
| gemmvact | 8 | 2.06 | 3.04 |
| gemmvqkvnorms | 8 | 1.73 | 2.55 |
| gemmprojc | 7 | 0.65 | 1.27 |
| the rest (gemmpool, gemmprojt, gemmvqkvs, gemmnores, decups) | | < 0.6 | < 0.8 |

310.8 model, 720p 200.3 ms in 153 dispatches / 1080p 346.0 ms: fswin256 (14x) 57.3 / 92.2, fswin128 (10x) 27.2 /
49.9, fswin64 (6x) 17.6 / 33.4, attn (16x) 16.0 / 22.9, fswin32nh (4x) 10.2 / 19.7, pre and post blocks 9.3+8.9 /
18.3+17.9, ffwd3 (16x) 8.4 / 11.9, then fswin32, gemmproj, the fused upsamples and downsamples (2-10 ms each).

Reading: the fused Swin family (`fswin*`, `fswinds*`, `fswinfusedup*`, pre/post blocks) is about 80% of the frame in
both models; GEMM and the C=512/ViT attention are small. Layer B should start with `fswin_t.comp`'s window body
(C=256 and C=128 first for the 310.8 model, the pre/post blocks and C=32/64 for the Small one), as the plan expected.

## Open issues

1. **Nothing ran on an RX 6000.** Everything above is gfx1201 running the RDNA2 code; per phase 0 the gfx103x driver
   (9.2.10.342 branch) compiles a different backend. Needs: `vkprobe` on a tester's RX 6000 (does it really report
   coopmat; RTE float16 and shuffles), the pipeline check there (time, crashes), and frame times.
2. **Cold build time.** ~70 min serial for the network on first run on this CPU (i7-10700F), plus the pipeline check
   (8 threads). On a tester's PC this is "NR shows after an hour" the first time. The menu's stage-6 text says "half an
   hour or more". Layer B kernels will be smaller; until then the installer could ship `prewarm\manifest-rdna2.txt`
   so the first build runs the 8-thread prewarm instead of the serial path (not done).
3. **Persistent runs off on RDNA2** (a perf feature, about 3% on RDNA4). When the kernels get faster (layer B) the
   poll budget can be revisited.
4. **RDNA3 network, 310.8 model, 1080p, forced on the RX 9070 XT: 0.6-1.2 s a frame**, in 0.5.8 as in this branch
   (same hashes). With RDNA2 the same C=128 persistent run (claiming off the counter, 558 windows > 400, 6 layers +
   DS) ran out of its waits every time even with 16x the budget, so it looks like a liveness problem of the Linux
   network's claim path on the AMD Windows driver, not of the emulation. Untested on a real RX 7000; if a tester
   reports slow frames with the old model at 1080p+, try `NR_NO_PERSIST=1` (or `NR_PERSIST_WG_128`).
5. **Run-to-run output at 720p** varies through the RenoDX stage's wall-clock smoothing (above), on any network whose
   frames straddle 0.1 s; not an RDNA2 bug, but it makes 40-frame A/B on RDNA2 inexact. Use 1 frame or 1080p for
   byte comparisons.
6. **NR_DOT2 not ISA-checked here.** It is phase 0's pattern verbatim; checking that LLPC still fuses it inside the
   unrolled `nr_mma` (and how many VGPRs the emulated kernels use, any scratch) needs `vkisa` with driver .342 per
   pipeline (`scratchpad\rdna2-phase0\vkisa`). Not done: each pipeline takes minutes to compile.
7. Device-id list for RDNA2 is hand-written (dGPU ranges + 4 APUs). RDNA1 (0x73xx below 0x73A0, 0x7310-0x734F) and
   anything else without FP8/matrices also lands on RDNA2 by the fallback rule; untested there (RDNA1 Navi 10 has
   no `v_dot2`, so it would be slower still).
8. Not done (later phases): installer component `mochizuki-rdna2`, `pin-mochizuki.ps1` zip, RX 6000 kit, in-game test
   (OptiScaler path was only built, not run in a game: the harness drives the same DLL and shows the menu's line).

## Artefacts (exports, outside git)
- `exports/mochizuki-rdna2/`: DLL + `dlssnr-amd` (shaders, shaders-rdna3, shaders-rdna2); `exports/base-058/` the
  base-commit build; `exports/release-local/OptiScaler.dll` (branch build).
- `exports/rdna2-work/`: `build.cmd`, `build-dll.cmd`, `build-tools.cmd`, `ab.ps1` (A/B with `-Force none|rdna3|rdna2`),
  `junctions-on.cmd`/`junctions-off.cmd` (vulkan headers from `wt-upstream-merge`: `opti\external\vulkan` is older than
  this branch's submodule and lacks VK_EXT_shader_float8), `compare.exe`, `mz_e2e.exe`, `refs/` (RDNA4 references and
  diff images), `step-times-final.txt`, `ab/<variant>/` (runs and logs; `ab/r2s` has a warm pipeline-rdna2.cache and
  manifest).
