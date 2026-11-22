# Handoff: mochizuki RDNA2, next layer B round (GEMM, attn, ffwd3, vitattn; 2026-10-27)

Worktree `wt-rdna2`, branch `mochizuki-rdna2`, local commits only (nothing pushed, uploaded or posted, no game
folders touched). Before: `HANDOFF-rdna2-phase3.md`. Clean room: the GPL RDNA2 repository was not opened, searched,
fetched or cloned, nor anything derived from it (line added to the plan's section 14). Sources: the plan, the MIT
mochizuki tree (layer A kernels, the host), the earlier handoffs, my own measurements.

## Result

- **Native RDNA2 kernels for every remaining hot pipeline**: `gemmproj`, `gemmprojc`, `gemmvact`, `gemmvqkvnorms`
  (one GEMM body), `attn` (C=512 window attention), `ffwd3` and `ffwd3w` (grouped C=512 feed-forward), `vitattn` (ViT
  global attention). Same switch as phase 3: `NR_NATIVE=1` in `rdna2/pipelines.json`, its own include with its own
  `main`, remove the define and the emulated kernel is back.
- **Output byte-identical to layer A**, first frame, both models, 720p, 1080p and 1440p (the 1440p run is what
  dispatches `ffwd3w`): E9F5E89B / D07096C4 / D0A1B50E / A5A0A394 (720p/1080p, Small/310.8), EB2040C4 / 3AA32BB2 at
  1440p. No kernel needed the 0.1 dB allowance. Against the RDNA4 FP8 output it is therefore layer A's PSNR: 44.43 /
  45.54 / 42.48 / 42.72 dB, all >= 40. The 40-frame outputs of the phase-3 build and this one are identical too
  (FA35B425 / C9A4FB6E / 188D4AAD / 44C578C4): both now sit on the same side of the RenoDX wall-clock gate.
- **Faster, 9070 XT forced** (status line, network ms, 40 frames, two alternating runs each, phase-3 build -> this):
  Small 720p 36.4 -> 28.6, Small 1080p 60.9 -> 46.6, 310.8 720p 75.9 -> 59.8, 310.8 1080p 119.5 -> 89.4
  (layer A was 86.9 / 162.4 / 196.7 / 344.3: 3.0x to 3.9x for the whole network now).
- **Lean on gfx1030** (vkisa, null NAVI21, driver 9.2.10.342): 26-163 VGPRs, **0 scratch everywhere**, `v_dot2_f32_f16`
  with `s_buffer_load_dwordx4` operands, 0 `ds_bpermute`, each compiles in 0-3 s (the emulated ones: 237-256 VGPRs,
  0.7-3.8 KB scratch, 4-213 s, `ffwd3w` no pipeline at all).
- RDNA4 and RDNA3 unchanged: `shaders\` and `shaders-rdna3\` SPIR-V byte-identical to the previous export, and
  `ab.ps1` with the rebuilt DLL gives the 0.5.8 hashes (table below).
- One host change, RDNA2 only: `tchain_active()` returns false on RDNA2 even with `NR_TCHAIN=1` (see Design).
- Cold build (fresh `ab\p4_r2cold`, `ab.ps1 -Force rdna2`, Small 720p): pipeline check 90 of 90 in **3.6 s** on 8
  threads (slowest `g_attn` 3.0 s; was 16.7 s), network ready in **72.6 s** (was 168 s; phase 1-2 ~70 min). The serial
  first build (no prewarm manifest) is 59 s of pipelines on the 9070 XT's gfx1201 compiler; the driver cache may have
  held some, so lower bounds.

## Design

Common to all four: the operand that is the same for every lane is read as aligned `uvec4` at a uniform address
(`s_buffer_load_dwordx4`, wave index through `subgroupBroadcastFirst(gl_SubgroupID)`), each SGPR straight into
`NR_DOT2` (`v_dot2_f32_f16`); every output is one FP32 chain over the k pairs in k order from 0 exactly as the emulated
`nr_mma` chain forms it (its products are commutative, so A x B and the `NR_SWAP_AB`/`QKSWAP` B x A forms give the same
bits); every epilogue rounding is the emulated function on the same value. Inside `attn.comp`, `ffwd3_t.comp` and
`vit_attn.comp` (which `unroll_glsl.py` unrolls) no `[[unroll]]` attribute: the script leaves it dangling before a block.

- **GEMM (`include/gemm_native.glsl`)**: same grid, push block and tile (`NR_MTILE` x `NR_NTILE`). A wave owns
  `NR_NG_TW` consecutive tokens (their 16-k rows are the scalar operand) and 32 x `NR_NG_CL` channels (lane l channel
  32c + l, its N-pair weights two `uvec4` a k fragment, one fragment ahead, ping-pong register sets). The workgroup runs
  `NR_NG_WAVES` waves over the emulated tile (local size override in `gemm1x1.comp`); waves 0..`NR_WAVES`-1 signal the
  token tiles the emulated waves signal, after a barrier, so tile-counter counts are unchanged. QKV norm: the head's 32
  channels sit in the 32 lanes, so the emulated FP16 butterfly (pair sum across lane ^ 16, then xor 8/4/2/1) is the
  same shuffles. Settings swept on the 9070 XT (Small 720p `gemmproj`: 4 emulated waves 5.95 ms; native 4 waves
  TW8/CL4 6.0, TW16/CL2 3.47, 8 waves 2.7-3.3, 16 waves 2.53-2.90, 32 waves 2.68-2.88): `gemmproj` TW4/CL2,
  `gemmprojc` TW8/CL1, `gemmvact` TW16/CL2, `gemmvqkvnorms` TW8/CL3, all 16 waves (32 waves cap gfx1030 at 64 VGPRs).
  The lever was waves in flight and weight bytes a wave, not the dot products (the 4-wave native is no faster on
  gfx1201: its emulated kernel there has no spills and cheap `ds_bpermute`).
- **Scalar activations and the tile counters**: the GEMM and ViT-attention bodies read activations through the scalar
  cache. Nothing in the dispatch writes them and the barrier between dispatches invalidates that cache; a tile-counter
  wait would not. The FP16 networks already ran without tile counters unless `NR_TCHAIN=1`; RDNA2 now never does
  (`nr_graph.cpp`, `if (g_rdna2) return false;`). attn and ffwd3 read only weights through the scalar cache.
- **attn (`include/attn_native.glsl`)**: one workgroup a window and head (same grid, remap, 2 waves). (1) Projection:
  lane l of wave w owns tokens l and l + 32 and the head rows 48w..48w+47 (weights scalar, each feeding two tokens);
  Q/K rows to LDS as FP16 `[row][token]`, V quantised and transposed `[channel][token pair]` (pairs packed with the
  neighbouring lane). (2) Norms per token, the `attn_qkv_epi.glsl` tree, both fragment halves in one lane. (3) One query
  a lane: q in registers, K and V rows by LDS broadcast, 64 logits, the emulated exponent pairs (16n + 4m + hh,
  +2) and den order, probabilities, context, a 64-byte row store. 20 KB LDS.
- **ffwd3/ffwd3w (`include/ffwd_native.glsl`)**: one lane a token over the workgroup's tiles (64 or 128 tokens, local
  size 16 x WGW x FM), the group's three matrices as scalar weights, r[64] in registers, the 256 hidden units sixteen at
  a time straight into the 64 output sums (hidden index = k of that product, in order). No LDS.
- **vitattn (`include/vit_native.glsl`)**: one lane a query (32 a workgroup, as emulated), Q in registers; every key's K
  and V rows scalar; V pairs (two keys, one channel) built with `s_pack_ll/hh_b32_b16`; the guard chunk, block clamps,
  FP16 denominator partials and padding subtraction as `vit_attn_vt_chunk.glsl`.

## Per kernel (9070 XT forced, NR_STEP_TIMES=20, phase-3 build -> this, ms a frame; gfx1030 = vkisa .342, wave32)

Step tables back to back (`exports\rdna2-work\p4\`); the untouched Swin kernels moved -1% to +20% between the two runs,
so read single digits of a percent as noise.

| pipeline | n | Small 720p | Small 1080p | 310.8 720p | 310.8 1080p | gfx1030 native: VGPR / scratch / dot2 / s_bufx4 / instr (WG) | emulated gfx1030 |
|---|---|---|---|---|---|---|---|
| gemmproj | 15 | 6.08 -> 2.53 | 6.54 -> 4.11 | 5.37 -> 2.21 | 6.66 -> 3.22 | 55 / 0 / 128 / 24 / 840 (512) | 256 / 3600 B, 16k instr |
| gemmprojc | 7 / 31 | 0.76 -> 0.43 | 1.26 -> 0.71 | 2.90 -> 1.79 | 5.69 -> 2.42 | 26 / 0 / 128 / 40 / 912 (512) | 256 / 3824 B |
| gemmvact | 8 | 2.38 -> 1.54 | 3.08 -> 3.05 | 2.07 -> 1.35 | 3.13 -> 2.39 | 68 / 0 / 512 / 73 / 2048 (512) | 237 / 2112 B |
| gemmvqkvnorms | 8 | 2.01 -> 1.59 | 2.60 -> 2.52 | 1.70 -> 1.40 | 2.67 -> 2.02 | 78 / 0 / 384 / 40 / 2199 (512) | 256 / 2992 B |
| attn | 4 / 16 | 4.87 -> 2.36 | 6.44 -> 3.66 | 17.2 -> 8.41 | 26.1 -> 11.8 | 163 / 0 / 3584 / 202 / 13k (64), LDS 20 KB | 256 / 1744 B, 40k |
| ffwd3 | 4 / 16 | 2.56 -> 1.43 | 3.30 -> 1.37 | 9.06 -> 5.30 | 13.3 -> 4.59 | 108 / 0 / 1536 / 390 / 4080 (64) | 150 / 0 (ffwd3w: no pipeline / 256 + 672 B) |
| vitattn | 8 | 3.45 -> 1.64 | 7.46 -> 2.75 | 3.11 -> 1.51 | 7.55 -> 2.18 | 68 / 0 / 2048 / 517 / 7845 (32) | 256 / 2496 B |
| ported total | | 22.1 -> 11.5 | 30.7 -> 18.2 | 41.4 -> 22.0 | 65.1 -> 28.7 | | |

ffwd3w (1440p, 310.8): 5.6 ms a frame native (16x); gfx1030 108 VGPRs, 0 scratch, WG 128. Static VALU gfx1030 /
gfx1201 (dot2 against two fma_mix): gemm 0.82-0.91, attn 0.81, ffwd3 0.62, vitattn 0.51.

## A/B (SHA-256 prefixes, `mz_e2e`)

| | Small 720p | Small 1080p | 310.8 720p | 310.8 1080p |
|---|---|---|---|---|
| RDNA2 forced, layer A and this build, 1 frame | E9F5E89B | D07096C4 | D0A1B50E | A5A0A394 |
| RDNA4, 40 frames, rebuilt DLL (`ab\p4_r4`) | 258FEC01 | 01588CA6 | 2A308F2C | 04A421E7 |
| RDNA3 forced, 40 frames, `MZ_RDNA3_CHECK=0` (`ab\p4_r3`) | D68F26C1 | FA85987C | A2178F0E | FE4E456B |

RDNA4/RDNA3 rows equal 0.5.8's (phase 1-2 table).

## Estimate, RX 6000 network time, Small model 720p (ESTIMATE, not measured)

Phase 3's method on this build's step sums (35.1 ms on the 9070 XT forced: native Swin ~23.0, ported ~11.5, the
emulated rest ~0.6): family time x gfx1030/gfx1201 static VALU (Swin 0.75, ported 0.77 time-weighted, rest 1.1) x
the card's VALU rate against the 9070 XT (6900 XT 0.95, 6800 1.41, 6700 XT 1.58). The status line is ~20% under the
step sum (timestamp overhead), so the steady network time would be about that much lower.

| card | Small 720p (phase 3) | Small 720p now | 310.8 1080p now |
|---|---|---|---|
| RX 6900 XT | ~38 ms | **~26 ms** | ~71 ms |
| RX 6800 | ~56 ms | **~38 ms** | ~105 ms |
| RX 6700 XT | ~63 ms | **~43 ms** | ~117 ms |

Not modelled: bandwidth, cache sizes, occupancy, and above all the scalar data cache's throughput on RDNA2. The ported
kernels lean on scalar loads far more than the Swin ones (one `s_buffer_load_dwordx4` every 4-12 `v_dot2`); if the
K$ cannot feed that, the GEMM settings with fewer scalar loads (larger `NR_NG_CL`) are the first knob.

## Remaining hot spots (Small 720p / 310.8 1080p, this build's step table)

1. Native Swin family, now ~2/3 of the frame: `fswin256` 2.1 / 17.3, `fswin128` 1.4 / 10.2, `fswin64` 1.7 / 9.1,
   `fswin32nh` 1.9 / 6.7. Ideas from phase 3 still open (weights in LDS if K$ limits, persistent runs).
2. Pre block 4.2 / 7.0 ms and post block 3.2 / 5.4 ms: the largest single dispatches at 720p. Not done this round: the
   pre block's time is mostly the image-feature staging (texture taps of colour, depth dilation, motion, the 5-tap
   history, the Gaussian noise when no noise field), which is not RDNA2-specific and not dot products.
3. attn 2.4 / 11.8: the attention phase is one query a lane with K and V by LDS broadcast (512 `ds_read_b128` a lane);
   two queries a lane or 4 waves a workgroup are the next things to try.
4. Still emulated, all small: `gemmprojw` (ViT contraction at >= 768 tokens, i.e. 1440p and up), `gemmpool`,
   `gemmprojt`, `gemmvqkvs`, `gemmnores`, `gemmvqkv/norm/proj/nores`, and the non-persistent `fswinds*`. `gemmprojw`
   matters at 1440p+ and fits the GEMM body (needs `NR_NG_*` settings and a byte check).

## Risks / open

1. **No RX 6000 run** (as before): all timing is gfx1201 running gfx103x-style code; gfx1030 ISA from the .342 null
   device only. The gemm settings were picked on the 9070 XT.
2. Scalar cache coherence: correct as long as a barrier separates the QKV/GEMM producer from these consumers (the RDNA2
   host now guarantees it). Any future RDNA2 chaining of these kernels must read activations as vector loads.
3. Multi-frame paths: frame 1 byte-identical; 40-frame outputs also identical between the phase-3 build and this one.
4. vkisa occupancy: `attn` 163 VGPRs and 20 KB LDS gives ~3 workgroups a CU; the GEMMs use 512-thread workgroups.
5. Artefact damage, mine: an early timing loop reused the existing `ab\a1` folder (phase 1-2 run). Its
   `g_gemmvact.spv` was restored (layer A, identical to `n9`/`la`) and a `build.pending` marker removed, but its
   `new_720p.txt` and `mochizuki_nr.log` were overwritten by a run that hung (killed). New runs use `ab\p4_*`.

## Commits (on 62e393dbb)

```
1121e99b3 native pointwise GEMM (gemmproj, gemmprojc, gemmvact, gemmvqkvnorms)
6bafaac91 native C=512 window attention (attn)
277885c5a native grouped C=512 feed-forward (ffwd3, ffwd3w)
22abb593d native ViT global attention (vitattn)
b4fe17336 host: no tile counters on RDNA2; UPSTREAM.md
(last)    this handoff
```

## Files

- `third_party/mochizuki/linux/shaders/rdna2/include/{gemm_native,attn_native,ffwd_native,vit_native}.glsl` (new);
  `gemm1x1.comp`, `attn.comp`, `ffwd3_t.comp`, `vit_attn.comp` (the `NR_NATIVE` switch, local-size overrides);
  `pipelines.json` (`NR_NATIVE=1` on 9 more pipelines, `NR_NG_TW/CL/WAVES` on the GEMMs);
  `windows/src/core/nr_graph.cpp` (`tchain_active`); `UPSTREAM.md`. Outside git: the plan's section 14 line.

## Artefacts (outside git)

- `exports/mochizuki-rdna2/`: rebuilt DLL (the `tchain_active` change) and `dlssnr-amd` (shaders, shaders-rdna3
  unchanged; shaders-rdna2 with the new kernels).
- `exports/rdna2-work/`: `nat4.ps1` (`nat.ps1` with `-From`, default `ab\n9`), `build-p4.log`, `p4\` (step tables,
  `base_*` = phase-3 build), `ab\p4_base` (phase-3 set), `ab\p4_final`, `ab\p4_r2cold` (cold build, warm cache now),
  `ab\p4_r4`, `ab\p4_r3`.
- Scratch `C:\Users\matheus\AppData\Local\Temp\claude\E--Projetos-opti-rr-dlss5\795b344c-93eb-4227-8c2f-f4383752f0e5\scratchpad\rdna2-p4`:
  `scripts\` (`mk.sh`, `isa.sh`, `one.sh <pipeline> <tag> [defines]`), `spv\`, `out\` (gfx1030 and `g12_*` gfx1201
  ISA and stats), `set\` (overlay sets), `prev\` (the previous export's shaders and DLL).
