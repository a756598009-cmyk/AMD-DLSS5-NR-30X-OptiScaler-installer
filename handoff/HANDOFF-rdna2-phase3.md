# Handoff: mochizuki RDNA2, phases 3 and 4 (native kernels, 2026-10-27)

Worktree `wt-rdna2`, branch `mochizuki-rdna2`, local commits only (nothing pushed, uploaded or posted, no game
folders touched). Plan: `RDNA2-MOCHIZUKI-PORT.md` (5.2, 6, 9, 11, 12). Before: `HANDOFF-rdna2-phase1-2.md`,
`PHASE0-probe.md`. Clean room: the GPL RDNA2 repository was not opened, searched, fetched or cloned, nor anything
derived from it (line added to the plan's section 14). Sources: the plan, the MIT mochizuki tree (the emulated
kernels and the host), the two handoffs, my own measurements.

## Result

- **Every fused Swin pipeline now has a native RDNA2 kernel**: `fswin32/32nh/64/128/256`, `fswinfusedup32/32nh/64/128/256`,
  `fswindsp32/32nh/64/128/256`, the pre block (`fswinimagepreds32/32nh`, `temporal_pre_fp32/fp32nh/fp32nhhp`) and the
  post block (`fswinimagepost32`, its `_grade`, `temporal_post_fp32/fp32hp/fp32h16`). That family was ~80% of the frame.
- **Output byte-identical to the emulated network (layer A)**, first frame, 720p and 1080p, both models (SHA-256 below).
  Not "within 0.1 dB": the same bytes. So against the RDNA4 FP8 output it is exactly layer A's PSNR: 44.43 / 45.54 /
  42.48 / 42.72 dB (Small 720p / Small 1080p / 310.8 720p / 310.8 1080p), all >= 40 dB.
- **Faster, 9070 XT forced** (`MZ_FORCE_RDNA2=1`, clean back-to-back runs, network ms from the status line):
  Small 720p 86.9 -> 37.1, Small 1080p 162.4 -> 60.4, 310.8 720p 196.7 -> 76.4, 310.8 1080p 344.3 -> 120.8
  (2.3x to 2.8x for the whole network; 3x to 8x for the Swin kernels themselves).
- **They compile for gfx1030** (driver 9.2.10.342, `vkisa`, null NAVI21): 110-155 VGPRs, **no scratch**, `v_dot2_f32_f16`
  fed by `s_buffer_load_dwordx4` weights, in 0-6 s each. The emulated `fswin128/256`, `fswinds128/256` (and others, list
  below) **do not compile at all on gfx1030** (`vkCreateComputePipelines` -> -1 after 2-4 min); the others use all 256
  VGPRs and spill 1.9-3.6 KB to scratch. So before this, the RDNA2 network could not have run on a real RX 6000 with
  this driver; with it, the Swin part can.
- RDNA4 and RDNA3 unchanged: `build_network.py rdna4 --check` and `rdna3 --tree linux --check` against the 0.5.8-equal
  export say identical (SPIR-V byte for byte), the DLL is not rebuilt (no host change), and `ab.ps1` gives the phase 1-2
  hashes again (table below).
- Cold build with the native kernels (fresh folder, `ab.ps1 -Force rdna2`, Small 720p): pipeline check 90 of 90 in
  16.7 s on 8 threads (slowest `g_fswinfusedup64` 4.6 s), network ready in 168 s (was ~70 min). The driver's own shader
  cache may have held some of the native pipelines from earlier runs, so treat the check's 16.7 s as a lower bound;
  the 141 s graph build is mostly the emulated GEMM/attention pipelines.

## Design (include/fswin_native.glsl)

`NR_NATIVE=1` in a pipeline's defines makes `fswin_t.comp` take `include/fswin_native.glsl` (its own `main`) instead
of `fswin_body.glsl`. Same bindings, push block, window grid, weights (the FP16 twin, `kswap` layout) and outputs.
Removing the define gives the emulated kernel back, per pipeline.

- **One lane per token.** The weights are then the same for every lane: read as `uvec4` at uniform addresses
  (`s_buffer_load_dwordx4`, wave index through `subgroupBroadcastFirst(gl_SubgroupID)`: `gl_SubgroupID` alone stays a
  VGPR on gfx103x and the weights became vector loads), each SGPR going straight into `v_dot2_f32_f16`. No shuffle in
  any projection (the emulated `NR_MMA` costs one `ds_bpermute` per `v_dot2`, plus 16-bit element loads).
- **C >= 64 (head split, 32 x heads invocations, one window a workgroup):** lane l owns tokens l and l+32 (each scalar
  weight feeds two `v_dot2`). Activations in LDS as `[k fragment][8-channel half][token]` uvec4: a lane's 8 channels are
  one `ds_read_b128`, a wave's 32 reads consecutive. Wave w owns hidden group w (expand streamed straight into the
  grouped middle projection, 16 hidden rows at a time, nothing of the 4C hidden stored), output channels 32w.. (MLP
  contract, output projection) and head w (Q, K, V, attention). K and V of a head go to LDS slots that are dead by then
  (the head's own context slots and the y slots, the residual y kept in registers), read by every lane at one address
  (broadcast). Softmax in two passes over the keys (sum, then normalise and multiply by V; the logits are recomputed,
  ~2% of the MACs) so nothing is indexed dynamically. LDS as the emulated body: 2 x 64 x C FP16 (64 KB at C=256).
- **C = 32 (one head, two waves):** lane = token, x in registers, the MLP streamed (expand 16 rows, straight into the
  contract), the same attention helper, K/V 8 KB of LDS.
- **Variants** around the same bodies: the wide fused upsample (projection of the level below staged in LDS, then the
  skip blend as stage 0), the C=32 fused upsample (projection in registers), the fused downsample (2x2 pool of the FP32
  output into LDS, the downsample projection, its addressing/clearing copied), the pre block (image features via
  `image_input.glsl`, the FP16 lift, the pooled copy), the post block (skip/up blend, the FP16 RGB projection, the pixel
  composition now in `include/fswin_imgout.glsl`, shared by both bodies). Tile-counter prologue/wait/signal as the
  emulated body's.
- **Same arithmetic, so the same bytes:** every product goes through `NR_DOT2` (coopmm.glsl) on the same k pairs in
  the same order and the same accumulator start (0 or the FP32 residual); where the emulated body multiplies
  k-pair-swapped storage by a swapped operand (`kswap`: Wmid, the C=32 E/Ct/QKV/Wo, the odd keys of QK, the odd V
  channels of AV) the native one swaps the pair (`.yx`) too; the Q/K norm partials, the FP16 softmax sum tree, the pool's
  FP32-then-FP16 adds and the exponent trick's pairs (`NR_EXP_NOHI` couples the two halves of a pair) are reproduced
  in the emulated body's order; every rounding is the same function on the same value.
- Long loops are kept rolled (`kf = kf + 1` headers that `unroll_glsl.py` leaves alone, and `nv_opaque` =
  `gl_WorkGroupID.z` in the trip count so the driver does not unroll them): unrolled, the shader was 46k instructions
  and 160-256 VGPRs; rolled, 15k and 154.

## Per kernel (9070 XT forced; gfx1030 = vkisa, driver 9.2.10.342, wave32)

Times: Small 720p from clean back-to-back runs (layer A `ab/la`, native `ab/n9`, 16:43 and 17:0x); 310.8 1080p the
same. A later paired table under external GPU load is in `exports/rdna2-work/step-times-native.txt` (its absolute
numbers are ~1.5x and noisy; its ratios agree for the big kernels).

| pipeline | dB vs layer A | Small 720p ms | 310.8 1080p ms | gfx1030 VGPR / scratch / static v_dot2 / s_buffer_load_dwordx4 / waves a SIMD | emulated on gfx1030 |
|---|---|---|---|---|---|
| fswin256 | identical | 8.34 -> 1.90 | 93.1 -> 16.4 (14x) | 154 / 0 / 4864 / 419 / 4 (LDS 64 KB) | does not compile (-1) |
| fswin128 | identical | 5.44 -> 1.23 | 50.6 -> 9.7 (10x) | 154 / 0 / 4864 / 435 / 4 (LDS 32 KB) | does not compile (-1) |
| fswin64 | identical | 6.06 -> 1.58 | 33.6 -> 7.9 (6x) | 154 / 0 / 4864 / 435 / 4 (LDS 16 KB) | 256 / 1936 B |
| fswin32nh, fswin32 | identical | 5.37 -> 1.79 | 19.7 -> 6.0 (4x) | 110 / 0 / 3328 / 647 / 8 | 256 / 2400 B |
| pre (temporal_pre_fp32nh) | identical | 9.57 -> 3.93 | 18.3 -> 6.7 | 110 / 0 / 3584 / 717 / 6 (LDS 10 KB) | 256 / 2272 B, 59k instr |
| post (temporal_post_fp32) | identical | 9.12 -> 2.89 | 17.9 -> 5.1 | 110 / 0 / 3328 / 649 / 8 | 256 / 2256 B, 60k instr |
| fswinfusedup256 / 128 / 64 | identical | 4.41 / 5.08 / 3.63 -> 1.08 / <0.95 / <0.95 | | 155 / 0 / 5120 / ~500 / 4 | -1 in the parallel run, not retried alone |
| fswinfusedup32(nh) | identical | 2.73 -> 0.95 | | 110-120 / 0 / 3840 / 775 / 8 | |
| fswindsp256 / 128 / 64 | identical | 2.82 / 2.44 / 2.85 -> 1.20 / <0.95 / <0.95 | | 154 / 0 / 5376 / ~550 / 4 | |
| fswindsp32(nh) | identical | 2.77 -> <0.95 | | 115 / 0 / 3584 / 717 / 8 | 256 / 2416 B, 72k instr |

Waves a SIMD on gfx1030: min of 1024 / VGPRs (rounded up to 8) and the LDS limit (64 KB a CU, 2 SIMD32 a CU). The
C >= 64 kernels are LDS-bound at 4 (one 64 KB window at C=256; VGPRs would allow 6), the C=32 ones VGPR-bound at 8-9.
Static instruction counts: 15-19k for C >= 64 (12-15k VALU), 9-11k for C = 32 (6-7.6k VALU), against 59-93k for the
emulated ones that compile. All in `scratchpad\rdna2-p3\out\all_nat.txt` / `all_base*.txt` (see Artefacts).

The 9070 XT is a weak proxy: the installed gfx1201 driver compiles **these kernels with `v_fma_mix_f32` instead of
`v_dot2_f32_f16`** (0 dot2 in every RDNA2 pipeline, native or emulated: e.g. native fswin32nh 6785 fma_mix, 11.9k VALU
against 6.2k on gfx1030), so on gfx1201 each pair costs two instructions where gfx1030 needs one. The byte-identical
output means the two instructions round the same as `v_dot2` here.

## A/B (SHA-256 prefixes of `mz_e2e` output)

| | Small 720p | Small 1080p | 310.8 720p | 310.8 1080p |
|---|---|---|---|---|
| RDNA2 forced, layer A, 1 frame | E9F5E89B | D07096C4 | D0A1B50E | A5A0A394 |
| RDNA2 forced, native (spv set and the `build_network.py` set), 1 frame | E9F5E89B | D07096C4 | D0A1B50E | A5A0A394 |
| RDNA4, 40 frames (`ab.ps1 -Force none`) | 258FEC01 | 01588CA6 | 2A308F2C | 04A421E7 |
| RDNA3 forced, 40 frames (`MZ_RDNA3_CHECK=0`) | D68F26C1 | FA85987C | A2178F0E | FE4E456B |

RDNA4/RDNA3 rows equal the phase 1-2 table (= 0.5.8). The RDNA3-forced run needs `MZ_RDNA3_CHECK=0` on the 9070 XT now
(the layout probes reject gfx12's fragment map, as designed). Multi-frame RDNA2 hashes differ between layer A and native
only through wall-clock behaviour (RenoDX look history: `kHistoryMaxGapSeconds` = 0.25 s; layer A's 0.35 s frames
always restart it, the native 0.2 s frames keep it); the network itself is compared on frame 1.

## Estimate, RX 6000 network time at 720p, Small model (ROUGH, not measured)

Method: per family, 9070 XT forced time x (gfx1030 static VALU / gfx1201 static VALU) x (9070 XT VALU rate / card's
VALU rate), VALU rate = CUs x 2 SIMD32 x game clock (one wave32 VALU instruction a SIMD a clock; no dual issue for these).
9070 XT: 64 CU, 2.4 GHz (307 G/s). 6900 XT: 80 CU, 2.015 GHz (322 G/s, factor 0.95). 6800: 60 CU, 1.815 GHz (218 G/s,
1.41). 6700 XT: 40 CU, 2.424 GHz (194 G/s, 1.58). Native Swin VALU ratio 0.52-0.92 (dot2 vs fma_mix), ~0.75 weighted;
the emulated rest 1.04-1.21 (~1.1, and they spill on gfx1030, which this ignores). Clean Small 720p step sums on the
9070 XT: native Swin ~20 ms, the rest ~22.6 ms.

| card | Swin (native) | rest (emulated) | network |
|---|---|---|---|
| RX 6900 XT | ~14 ms | ~24 ms | **~38 ms** |
| RX 6800 | ~21 ms | ~35 ms | **~56 ms** |
| RX 6700 XT | ~24 ms | ~39 ms | **~63 ms** |

Bandwidth, cache and occupancy differences are not modelled; the emulated rest's scratch spills could make it
1.5-2x worse, and some of those pipelines may not compile on gfx1030 at all (below). The estimate says the next
work is the non-Swin kernels: they are now more than half of the frame on RDNA2.

## Remaining hot spots (emulated, Small 720p / 310.8 1080p, 9070 XT forced)

1. `gemmproj` (gemm1x1.comp, 15x) 5.9 / 6.3 ms; `gemmprojc` 0.7 / 5.4; `gemmvact` 2.3 / 2.9; `gemmvqkvnorms` 1.9 / 2.5.
   gfx1030: 236-256 VGPRs, 2.1-3.8 KB scratch each.
2. `attn` (C=512 attention) 4.7 / **24.2 ms (19% of the 310.8 frame)**; gfx1030 256 VGPRs, 1744 B scratch.
3. `vitattn` 3.3 / 7.4; `ffwd3` 2.5 / 12.5 (`ffwd3w`: 256 VGPRs, 672 B scratch).
4. Pre block 3.9 / 6.7 ms: its image-feature staging (textures, the Gaussian noise when there is no noise field) now
   weighs more than its network part.
Same recipe should apply (plan 6.4: register blocks of rows x tokens, operands staged in LDS, scalar weights).

## Emulated pipelines that fail to compile on gfx1030 (driver .342)

One at a time: `g_fswin128`, `g_fswin256`, `g_fswinds128`, `g_fswinds256` -> -1 (VK_ERROR_OUT_OF_HOST_MEMORY after
1.5-4 min); `g_fswin32/32nh/64`, `g_fswinds32/64`, `g_ffwd3w` compile, at 256 VGPRs with 0.7-2.5 KB scratch. In a 4-way
parallel run more failed (the fused up/downsample and image kernels too). One emulated Swin compile takes ~5.6 GB of
host memory in the null-device driver and the PC ran out of page file, so the parallel failures are at least partly
this PC's memory; the one-at-a-time retry of the rest was stopped for that reason. All of these are native now except
`fswinds128/256` (the non-persistent downsample, dispatched by neither model here; not ported). A tester's pipeline
check (stage 6, 8 threads) of the emulated network on an RX 6000 would likely have failed the same way.
Lists: `scratchpad\rdna2-p3\out\all_base.txt` (parallel), `all_base1.txt` (one at a time).

## Commits (on af100d0ef)

```
a01b2fe64 native fused Swin body for fswin64/128/256
e6ae68c3d native C=32 one-head Swin body (fswin32, fswin32nh)
f9bb5b981 native pre block
7556898eb native post block (+ include/fswin_imgout.glsl, emulated SPIR-V unchanged)
ebcacb514 native wide fused upsample (fswinfusedup64/128/256)
4401f4907 native fused downsample (fswindsp64/128/256)
25dfc9ee1 native C=32 fused upsample and downsample
(last)    UPSTREAM.md, this handoff
```

## Files

- `third_party/mochizuki/linux/shaders/rdna2/include/fswin_native.glsl` (new), `include/fswin_imgout.glsl` (new, moved
  out of `fswin_t.comp`), `fswin_t.comp` (the `NR_NATIVE` switch), `pipelines.json` (`NR_NATIVE=1` on 18 pipelines;
  the 6 temporal variants inherit it), `UPSTREAM.md` (RDNA2 section). Outside git: the plan's section 14 line.

## Open / risks

1. **No RX 6000 run.** Everything is the 9070 XT running gfx103x-style code plus gfx1030 ISA from the .342 null device.
   Real cards may run a newer RDNA2 driver branch (phase 0 risk).
2. Multi-frame paths not compared byte for byte (the temporal history gate, `ow`, the HPASS and h16 post variants and
   the `_grade` post): their code is shared with or copied from the emulated body, and frame 1 does not open the gate.
   A deterministic-clock harness option would close this.
3. `fswinds128/256` (non-`p` downsample) not ported; not dispatched by either model here.
4. Persistent runs stay off on RDNA2; with the native kernels 3-6x faster their poll budgets could be revisited.
5. LDS: the C=256 kernels use 64 KB a workgroup (as the emulated ones; accepted by .342), one workgroup a CU.
6. Scalar cache: each window streams the layer's weights through the scalar path (1.5 MB at C=256); not measured on
   RDNA2. If it turns out to be the limit, weights in LDS per workgroup (plan risk 2) or two windows a workgroup.

## Artefacts (outside git)

- `exports/rdna2-work/nat.ps1` (forced-RDNA2 runs from `ab/r2s`'s warm cache with SPIR-V overrides, `MZ_RDNA2_CHECK=0`),
  `bincmp.py`, `ab/la` (layer A), `ab/n3`..`ab/n9` (native steps), `ab/nat-cold`, `ab/r4-final`, `ab/r3-final`,
  `step-times-native.txt`.
- `exports/mochizuki-rdna2/dlssnr-amd/shaders-rdna2` now holds the native network (`build_network.py rdna2 --tree linux
  --no-quad`); DLL unchanged.
- `scratchpad\rdna2-p3` (session scratch: `C:\Users\matheus\AppData\Local\Temp\claude\E--Projetos-opti-rr-dlss5\795b344c-93eb-4227-8c2f-f4383752f0e5\scratchpad\rdna2-p3`):
  `vkisa\` (phase 0's tool, now with explicit binding types `0:s,6:i,8:t`), `scripts\` (`mk.sh` one pipeline as
  build_network does, `isa.sh`, `count.py`, `binds.py`, `allisa.sh`, `native_on.py`), `out\` (ISA + stats).
