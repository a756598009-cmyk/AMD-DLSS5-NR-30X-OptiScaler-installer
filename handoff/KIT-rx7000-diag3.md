# RX 7000 kit 3: one kernel at a time (2026-10-27)

Goal: find which part of the RDNA3 network's fused Swin kernel `fswin32nh` (and the dispatches after it) is
non-deterministic on a real gfx11 card. Kit 2 (`handoff\HANDOFF-rdna3-padfix.md`) showed values changing between
clean runs on the RX 7800 XT (gfx1101, driver 26.10.1) at key 8 (dispatch 1, `fswin32nh`) at some sizes, key 71
(dispatch 8, `fswindsp256`) or key 73 (dispatch 10, `gemmprojc`) at others. Local only: nothing pushed, uploaded or
posted, no game folder touched.

- Branch `rdna3-k3`, worktree `wt-rdna3-k3`, from `rdna3-padfix` 1eb12d4ba. The toolchain junction
  (`third_party\mochizuki\toolchain` -> `opti`'s) is removed before git commands.
- Kit: `E:\Projetos\opti_rr_dlss5\rx7000-diag-kit\OpenNR-RX7000-diag3.zip`, 43.1 MB (45225919 bytes), 72 files,
  SHA-256 `3A3C32A211E8EDE6CD79205AD362E7030EDE279F3D9369A292B07E33F6B02154`. No models, no NVIDIA files: the inputs
  are activation slices and the weight pages each dispatch reads (from the converted model the installer ships).
- Run here (RX 9070 XT, driver 26.10.1): 0.8 min through `run-test.bat`, result zip 31 KB, everything deterministic
  (as expected); `exports\k3work\kit\SUMMARY-rx9070xt.txt`. On the RX 7800 XT add the first LLPC compile of ~50
  pipelines (not measured; the driver cache made them instant here).

## What the kit does

`files\kreplay.exe kit` runs five cases. A case is one dispatch of the released RDNA3 network (1152x648, small model,
clean frame: seed 0, no history, recording 3) on fixed bytes, in a child process per pipeline (a compiler or GPU
crash is recorded, not fatal), wave32 required and robust access off as the runtime makes it:

| case | dispatch | pipeline | reads | writes |
|------|----------|----------|-------|--------|
| D01 | 1 | `fswin32nh` (block 1, C=32) | key 0 (twin) | key 8 (twin), one counter byte |
| D02 | 2 | `fswindsp32nh` | key 8 | keys 16, 23 |
| D03 | 3 | `fswin64` | key 16 | key 24 |
| D08 | 8 | `fswindsp256` | key 56 | keys 64, 71 |
| D09 | 9 | `ffwd3` (first C=512 layer) | key 64 | key 72 |

For each case: the released pipeline 50 runs in each of 2 processes (`distinct` answers per output, chunks that
change, first answer compared across processes), 20 runs with new garbage in every byte the dispatch should not
read ("rand": catches reads outside the inputs, e.g. padding a gfx11 build reads and gfx12 does not), then every
variant 50 runs. Each output is measured against the RX 9070 XT's FP8 network's value of the same key (`rel` = mean
|d| / mean |ref|). SUMMARY.txt "IN SHORT" lists per case: deterministic or not, the deterministic variants, the
ones still changing, the ones that did not compile or run, and the deterministic one closest to the reference.

Inputs: every activation value a case reads is the FP8 network's own value of that key (e4m3 into the FP16 twin as
f16), since kit 2's clean stage comparison had the RX 7800 XT's values SAME up to key 71. Here the forced RDNA3
fused-Swin kernels are wrong by design (gfx11 layout), so D01-D08 show rel ~1-2 on the RX 9070 XT; `ffwd3` is not
layout-bound and D09 gives the FP8 network's key 72 byte for byte here (100% equal, rel 0), which checks the inputs,
their conversion and the references.

## Variants (`tools\rx7000-diag3\build_variants.py`)

Built like the shipped SPIR-V (glslang -E, `unroll_glsl.py`, glslang -V; `base.spv` is byte-identical to the shipped
`g_<pipeline>.spv` for all five). One define flipped to its simpler value at a time, where the pipeline sets it and
the SPIR-V changes; defines that change the host's weight layout or what the host bakes (`NR_BAKED_EXP_BIAS`,
`NR_PACKED_*`, `NR_ACT_HORNER`, `NR_QUANT_MODE`) are not flipped.

- `fswin32nh` (D01, 21): `fwaves1` (NR_FWAVES=1, one wave per window: no LDS shared between waves), `prob_early`
  (NR_PROB_LATE_SHUFFLE=0), `v_noswap` (NR_V_SWAP=0), `oob_nobranch` (NR_OOB_BRANCH=0), `coherent`
  (NR_COHERENT_ACT=1), `wave_uniform` (NR_WAVE_UNIFORM=1), `tchain0` (NR_TCHAIN=0), `stream0` (NR_STREAM_MLP=0),
  `exp_hi` (NR_EXP_NOHI=0), `dwsel0` (NR_OP_DWSEL=0), `act_alt0` (NR_ACT_ALT=0), `pkn0` (NR_PKN=0), `qkscale0`
  (NR_QK_SCALE_FAST=0), `act_f16` (NR_ACT_F32=0), `resid_f16` (NR_RESIDUAL_F32=0), `fence` (every `barrier()` with
  `memoryBarrierShared(); memoryBarrierBuffer();` on both sides), `ldsfence` (`memoryBarrierShared()` after every
  LDS store: the K/V stores to `lds_k`/`lds_v`), `sgfence` (`subgroupMemoryBarrierShared()` after every subgroup
  shuffle/broadcast), `allsafe` (all the ordering/exchange flips + fence + ldsfence), `allsafe1w` (allsafe +
  NR_FWAVES=1). Same SPIR-V as base, so left out: NR_QK_TOGETHER, NR_PROB_BOUNDED, NR_LIVE15_NOOOB, NR_PACK_C32;
  NR_NORM_F32=0 does not compile.
- `fswindsp256` (D08, 18): `qk_apart` (NR_QK_TOGETHER=0), `prob_early`, `v_noswap`, `oob_nobranch`, `coherent`,
  `wave_uniform`, `act_alt0`, `qkscale0`, `act_f16`, `resid_f16`, `pool_dpp0` (NR_POOL_DPP16=0), `ds_qpair0`,
  `ds_ident0`, `fence`, `ldsfence`, `sgfence`, `allsafe`.
- `ffwd3` (D09, 10): `coherent`, `wave_uniform`, `tchain0`, `ffwd_pair0`, `ffwd_koff0`, `ffwd_gmajor0`,
  `occ_lds0` (no NR_OCC_LDS padding), `fence`, `allsafe`.
- `rolled` (loops not unrolled) was built for all three and crashes LLPC 26.10.1 here (0xc0000005), so the kit
  leaves it out. `gemmprojc` and `attn` variants are built (`exports\k3work\spv`) but not in the kit (below).

## Findings made while building it (RX 9070 XT)

1. `fswin32nh` writes key 8 only in the FP16 twin (all 14,155,776 bytes) plus one tile-counter byte; it never
   writes key 8's slot in the byte arena. Kit 2's "first value that changes: key 8 ... in arena-e4m3/f16/f32 chunk 7"
   (twin not listed) is therefore stale bytes in an arena slot the RDNA3 network does not use for that value (the
   slot at 99778560 is shared by keys 8, 24, 40, 56, 72, 82), not `fswin32nh` output. The twin values kit 2 compared
   were SAME. So `fswin32nh` itself is probably not the source; D01 should come back deterministic.
2. `ffwd3` writes 884,736 of key 72's 983,040 twin bytes (the 864 real tokens of 960; the padding tokens are never
   written). Kit 2's key 72 "DIFFERENT 0.30, worst in chunk 14" and key 73 "twin chunks 14,15" are at the tensor's
   end, where such unwritten tails hold whatever the slot held before. Worth checking against the full raw dump
   before blaming a C=512 kernel.
3. `gemmprojc` and `attn` (dispatches 10-12) give the same answer here whatever they read (forced gfx11 code on
   gfx12 saturates or writes zeros), so their read sets cannot be found by poisoning on this card and they are not
   in the kit. A later kit needs their inputs from the plan (`voff`, the push offsets) instead.

## How it was built (all under `wt-rdna3-k3`)

- Runtime: local patch 20 (`third_party/mochizuki/UPSTREAM.md`), `MZ_KCAP=<dir> MZ_KCAP_DISP=<n>`: every buffer
  dispatch n binds, copied whole before it in recording `MZ_KCAP_FRAME`, with SPIR-V name, grid, push constants as
  pushed and bindings. Off by default; harness output byte-identical to kit 2's fixed DLL (RDNA4 1280x720 and
  1920x1080, forced RDNA3 1280x720, 40 frames). Build: `exports\build-k3.cmd`.
- `tools\rx7000-diag3\`: `capture.ps1` (forced RDNA3 clean captures), `aco-refs.ps1` (FP8 network values by key,
  `NR_NO_PERSIST=1`), `aco_capture.py` (a capture with the FP8 values in place, used for D09), `kreplay.cpp`
  (`readset`: read and write sets by poisoning 64 KB pages with two patterns, write probe, a final check with
  everything else poisoned; `case`; `kit`), `build-tools.cmd`, `readsets.ps1`, `build_variants.py`,
  `make_case.py`, `make_kit.py`, `pack-kit3.ps1`, `run-test.bat`, `README.txt`.
- Work folder `exports\k3work` (captures `cap\D<n>`, 860 MB each; `aco\`; `spv\`; `kit\`).

## Reading the player's result

- IN SHORT, per case: "released pipeline deterministic" vs "CHANGES from run to run". The first case that changes is
  the kernel to look at; its "deterministic variants" line names the defines/fences that stop it, "closest to the RX
  9070 XT" the one whose answer is also right (rel near 0; the released pipeline on a healthy gfx11 should be ~0.01
  or less on D01-D08 and ~0 on D09).
- "differs between processes" with each process deterministic: something read differs per process (memory not
  written by anyone), not a race.
- "reads bytes outside its inputs" (the rand line changes): the gfx11 build reads bytes the gfx12 run did not
  (padding, an out-of-range tile) - an uninitialised-read bug, not a race.
- Everything deterministic and rel small: the kernels are right in isolation; the run-to-run changes of kit 2 then
  come from what the network leaves in slots between dispatches (finding 2) or from overlap between neighbouring
  dispatches, which this kit does not reproduce.
- `result\runs\<case>_<variant>.txt` has the raw lines (hashes, per-output chunk lists, compile time).

## Uncertain

- Isolation: one dispatch alone, separate submits. A race that needs the previous dispatch still draining, or a
  particular cache state, will not show here.
- The inputs are the FP8 network's values; their element order matching the RDNA3 twin is confirmed by D09 (exact)
  and kit 2's SAME verdicts, not for every value.
- The read sets were found on gfx12; if gfx11 reads more, the rand test says so but zero mode would hide it.
- First-time LLPC compile time on the RX 7800 XT is unknown; the 10 minute target assumes ~2 s a pipeline.
- The kit is 43 MB because the activations (e4m3/f16) hardly compress; D04-D07 were left out for size.
