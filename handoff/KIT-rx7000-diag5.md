# RX 7000 kit 5: the C=512 window attention (2026-10-27)

Goal: kit 4 showed dispatch 11 (`attn`, `CCSplitSwin16HQKVAttn`, C=512, writes key 74, grid 5x3x16 at 1152x648) is
the first dispatch that goes wrong on the RX 7800 XT (gfx1101, 26.10.1): non-deterministic even alone on an idle queue,
deterministic only with zeros scrubbed into LDS/registers before it, and then still wrong (twin rel 0.28-0.52 against
the RX 9070 XT's FP8 network). Kit 5 looks for the fault in that kernel and tries the RDNA2 tree's native attention
in its place. Local only: nothing pushed, uploaded or posted, no game folder touched.

- Branch `rdna3-k5`, worktree `wt-rdna3-k5`, from `rdna3-k4` 7794bfa6d. Junctions (not in git, removed before git
  commands): `external\vulkan` -> `wt-upstream-merge\external\vulkan`, `third_party\mochizuki\toolchain` -> `opti`'s.
- Kit: `E:\Projetos\opti_rr_dlss5\rx7000-diag-kit\OpenNR-RX7000-diag5.zip`, 34.1 MB (35720758 bytes), 393 files,
  SHA-256 `08155F62D57D814C497EFE81696CAABD9407ECB4A298677284C8C5C8F8C2F41F`. README beside it: `README-diag5.txt`.
  Needs `dlssnr-small.bin` (copied in from kit 1/2/4, else downloaded). No NVIDIA files.
- Run here (RX 9070 XT, 26.10.1, `KIT_FORCE_RDNA3=1`, 15 of the 16 attention pipelines; qpad0 checked separately):
  4 min end to end. Attention alone: nat rel 0.0004, deterministic in every mode; base and the variants wrong by
  design (gfx12 is not gfx11); qkswap0 changes run to run here (gfx11 code on gfx12 writes A/B components 8-15 a gfx12
  fragment does not have: not evidence about gfx11). Replays with the native attention deterministic at all three
  sizes (wrong from dispatch 1, by design). Pictures: rel / nat / k5rel 25.9-26.7 dB, every run identical.

## Reading attn.comp (the released defines)

`pipelines.json` `attn`: NR_ACC_F16=0, NR_ATTN_PAIR_QUANT, NR_IMAGE, NR_ATTN_TRANSPOSE, NR_ATTN_VTRANS, NR_ATTN_KOFF,
NR_ATTN_OOB_BRANCH, NR_ATTN_VFRAG, NR_ATTN_QKSWAP, NR_ATTN_EXP2, NR_ATTN_REMAP=16, NR_OCC_LDS=3072,
NR_GEMM_REMAP_PC, NR_ATTN_WPAIR, NR_TCHAIN, NR_APIPE=2; source defaults NR_AWAVES=2, NR_SHARE_HEAD=1, NR_HSPLIT=16
(one head a workgroup at gridZ=16), NR_AM=2, NR_AN=6, NR_AKF=8, NR_QPAD=4 (Q/K rows 36 f16 = 72 bytes), NR_FRAG_EPI,
fused QK norm (attn_qkv_epi.glsl). Dispatch 11's push: tiles 9x6, windows 5x3 (tile column 9 outside the image),
no shift, tc_rec ~0.

What was checked and found consistent with the gfx11 map the kit 4 probes measured on the player's card (accumulator
component c of lane l = (2c + l/16, l%16); A/B component k of lane l = (l%16, k), both lane halves equal):

- Uninitialised LDS: none at source level. Stage 1 writes Q, K (all 64 tokens x 32 dims, the out-of-image tiles
  through the zero branch) and V (32 x 64) before the workgroup barrier; stage 3 reads only those (the 4 pad elements
  of each Q/K row are never read). No remainder: 64 tokens = 2 waves x 2 tiles, one head a workgroup, 4 query
  blocks dealt 2 a wave. The pipelined k loop (APIPE=2, AKF=8, C=512) loads each step once before its MMAs.
- Uninitialised registers: none at source level. Every fragment built by components is written whole: the QKSWAP
  copies (qfaB, qfbA[0..3]) from the same loads, the FP16 accumulators of the epilogue (qf0/qf1/kf0/kf1/vfr0/vfr1,
  8 components, both branches), P (16 components; lanes l and l^16 build the same values), the output `result`.
- The gfx11 translations against RDNA4: the norm tree (d bit 0 = lane half), exp2 pairs two keys apart (`bo + 2`),
  the denominator's half exchange first, P's interleave (key 2c from the lower half, 2c+1 from the upper), the WPAIR
  record addressing (+0, +1, +64, +65 vec4 units), V's ColumnMajor [dim][token] store, the output's tile-blocked
  ColumnMajor store: all consistent. The native body (below) computes the same formulas without any fragment and
  is exact on the RX 9070 XT with the same weights, bias and scale offsets, so the host's data for this kernel is
  right too.

So the formulas are right for gfx11 and nothing in the source reads memory or registers it did not write. What is left
is how the cooperative-matrix code is compiled and executed on gfx11. Since a zero scrub (registers/LDS never written
by the kernel) makes the answer deterministic, the fault reads state the program never wrote in program order, which
a hazard would not (it reads the program's own older value). Suspects, in order:

1. The FP16 accumulator fragments (`NR_FRAG_E4M3` = coopmat<f16, Accumulator>) built by component writes and
   `coopMatStore` ColumnMajor into LDS at strides 36 and 64 (attn_qkv_epi.glsl: Q, K, V, 6 stores a tile) and into the
   output. On gfx11 a 16-bit WMMA accumulator is 8 VGPRs with each value in one half; a lowering that packs halves
   would read the other, never-written half. ffwd3 (right and deterministic on the card) stores the same type
   ColumnMajor but only at stride 16. Variants `elemst`, `zfrag`.
2. `coopMatLoad` from LDS at a 72-byte row stride (8-byte aligned; B ColumnMajor for Q, A RowMajor for K). Variant
   `qpad0` (64 bytes), `elemio` (no fragment load or store at all).
3. The QKSWAP operand copies and the hand-built P operand (`qkswap0`, `transpose0`: other code shapes for the same
   math), the exp2 pairs (`exp2_0`), the pipelined loads (`apipe1`).
4. Ordering (`fence`, `sgfence`, `coherent`) and LDS footprint (`occ_lds0`), the zero-init controls (`zlds`, `zall`).

NR_V_SWAP (kit 3's fswin finding) is not in attn.

## What the kit does

`files\kit5.ps1`, six steps (`run-test.bat`):

1. vkprobe + layout probes.
2. Dispatch 11 alone (`files\kreplay.exe kit files\attn`, kit 3's method, `tools\rx7000-diag5\kreplay.cpp`): the
   case is kit 3's capture `D11a` (forced RDNA3, 1152x648, small model, clean frame, recording 3) with key 73 (its
   input) from the RX 9070 XT's FP8 network, the read set from the plan (key 73's twin, the QKV matrix in the FP16
   weight twin, bias and scale), the reference the FP8 network's key 74. 16 pipelines (`build_attn5.py`): base
   (= shipped g_attn.spv), nat (= g_attnnat.spv) and the variants above. Each: 50 runs zero fill; 20 runs with the
   scrub leaving garbage in LDS/VGPRs before each run ("dirty", new seed a run); 20 with zeros there ("clean"); base
   and nat also a second process and 20 runs with garbage outside the inputs. A verdict per pipeline: deterministic
   in every mode, the same answer in every mode, rel against the RX 9070 XT. kreplay gained `+dirty`/`+clean` fills
   (the runtime's seq_scrub.comp, embedded as `scrub_spv.h`).
3. The model (verified, or downloaded as kits 2/4).
4. Sequence replay (kit 4's `MZ_SEQ`) with the native attention (`NR_RDNA3_ATTN=native`), released barriers,
   variants scrub and zero, at 1152x648, 1280x720, 1920x1080 against kit 4's FP8 references: the next offender if
   any; SUMMARY quotes key 74 against the RX 9070 XT and the first value off by more than 0.1.
5. Pictures, 40 frames, against the RX 9070 XT (kit 1's references): released 0.5.8 and the k5 DLL with the native
   attention at 1152x648, 1280x720, 1920x1080, 2560x1440, 3 runs each; the k5 DLL with the released attention (= kit
   4's twinbuf DLL) at the two small sizes; and, if step 2 found a variant deterministic in every mode with the same
   answer and rel under 0.05, the k5 DLL with that SPIR-V (`NR_RDNA3_ATTN=<file>`) at the two small sizes.
6. SUMMARY.txt and the result zip.

## How the native attention is wired (local patch 22)

- `linux/shaders/rdna3/include/attn_native.glsl`: the RDNA2 file unchanged plus `NR_DOT2` (an fma pair; RDNA2's
  coopmm.glsl has it) and RDNA2's round-to-nearest-even FP16 execution mode. Bindings (0 act twin out, 2 FP16
  weight twin, 3 f32 weights, 4 act twin in), push block, grid, remap and tile-counter hooks are attn's; the
  weights are the same WPAIR records (the RDNA2 tree was copied from this one).
- `attn.comp`: `#if NR_NATIVE` includes it in place of main. `pipelines.json`: `attnnat` = attn's defines +
  `NR_NATIVE=1`. Every other SPIR-V of `shaders-rdna3` and `shaders` is byte-identical to 0.5.8's.
- `nr_graph.cpp` `create_kernels`: RDNA3 network, kern `attn`: `g_attnnat.spv` unless `NR_RDNA3_ATTN=released`
  (default native when the file exists); `NR_RDNA3_ATTN=<file>.spv` diagnostic. Logged.
- On the RX 9070 XT the native body is right on its own inputs: dispatch 11 alone, FP8 inputs: 99.35% equal, rel
  0.0004, deterministic in every fill/scrub, any process. In the forced RDNA3 network the earlier dispatches are
  wrong on gfx12 by design, so the picture stays wrong here; the right picture can only be judged on an RX 7000.

## RDNA4 byte identity

`exports\k5work\ab.ps1`: harness, 40 frames, 0.5.8 DLL vs k5 DLL, small and 310.8 model, 1280x720 and 1920x1080,
driver compiler and ACO: 8 of 8 identical SHA-256 (`exports\k5work\ab\ab.txt`).

## How it was built

- `exports\build-k5.cmd` (DLL + shaders into `exports\mochizuki-k5`), `exports\build-dll-only.cmd`.
- `python tools\rx7000-diag5\build_attn5.py exports\k5work\spv --check exports\mochizuki-k5\dlssnr-amd\shaders-rdna3`.
- The case: `exports\k5work\case\sets11.txt` (read/write set by hand from the push constants), then
  `tools\rx7000-diag3\make_case.py <k3work>\cap\D11a sets11.txt <k3work>\r3stage\stages.txt <k3work>\aco case\D11 D11`.
- `tools\rx7000-diag5\build-tools.cmd exports\k5work\tools5` (kreplay).
- `build-kit5.ps1 <k4work>\kit2\OpenNR-RX7000-diag2\files exports\mochizuki-k5 <k4work>\seqref exports\k5work
  exports\k5work\kitbuild`, then `pack-kit5.ps1 exports\k5work\kitbuild <zip>`.

## Reading the player's result

- `Attention alone D11` (IN SHORT): "released attn" should read CHANGES (kit 4). The decisive lines:
  - `nat` deterministic in every mode, same answer, rel ~0.01 or less: the native body is right on gfx11 (expected,
    it uses no fragment layout); then the pictures' `nat` rows say whether the network is right from there on.
  - a variant in "of those right": that construct is the fault. `elemst` right -> the FP16 accumulator stores
    (suspect 1); `elemio` right but not `elemst` -> the fragment loads; `qpad0` right -> the 72-byte stride; `zfrag`
    right -> the f16 accumulator's unwritten components (then the RDNA3 fix is to keep zero-init or element stores in
    attn_qkv_epi.glsl and the output store).
  - "deterministic per mode but the answer depends on the mode": still reads unwritten LDS/registers, now
    deterministically.
  - nat also CHANGES or wrong: the fault is below the shader (driver/hardware on this card, e.g. the scalar loads),
    not the cooperative matrices.
- SEQ lines (native attention): "changes run to run at none" and key 74's twin rel ~0.01 = attention fixed in the
  sequence; a later "first offender" (kit 4's zero run pointed at 15/17/20/21: attn, gemmnores, gemmvqkvnorms,
  vitattn) is the next kernel to take apart, and "first value off" names the first one that is wrong (not only
  varying).
- Pictures: `nat` GOOD (40 dB or more) and "identical" at all four sizes = the fix; `nat` identical run to run but
  below 40 dB = deterministic now, a later kernel still wrong (see SEQ).

## Uncertain

- Nothing in the source of attn was found reading unwritten LDS or registers, and its gfx11 formulas check out, so
  no RDNA3 attn source fix is offered; the kit decides between the suspects on the card. If `elemst`/`elemio`/`zfrag`/
  `qpad0` comes out right there, that change is the fix candidate (the kit already runs it in the full network).
- The native body is right on gfx12 with FP8 inputs; on gfx11 it is the same scalar/v_dot2 code (no fragment), but
  it has never run on an RX 7000. It declares RTE FP16 rounding (shaderRoundingModeRTEFloat16, not checked by the
  RDNA3 host; AMD reports it on RDNA3).
- Native is slower: whole network on the forced 9070 XT 5.4 -> 8.0 ms at 1152x648, 11.1 -> 13.7 ms at 1080p,
  14.7 -> 18.4 ms at 1440p.
- The case's inputs are the FP8 network's key 73; the read set is written by hand from the push constants (checked:
  nat gives 99.35% equal with zeros and with garbage outside it).
- With the native attention the SEQ step shows whether gemmnores/gemmvqkvnorms/vitattn (kit 4's zero-scrub
  offenders 17/20/21) still vary; they are not taken apart alone in this kit.
- Time on the RX 7800 XT not measured: 4 min here; kit 4 took 7 min there for a similar load, plus the first LLPC
  compile of 16 attention pipelines (nat 3.4 s here).
