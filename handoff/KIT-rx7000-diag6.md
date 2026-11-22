# RX 7000 kit 6: aligned LDS rows and a gather-free V^T (2026-10-27)

Goal: kit 5 on an RX 7900 XTX (driver 32.0.32015.7029, 26.10.1) showed dispatch 11 (`attn`) right and deterministic
alone with `qpad0` (Q/K rows in LDS 64 bytes apart, 16-byte aligned) and with `elemio`/`nat`, and every other variant
still changing; with the native attention the network was right at 1080p/1440p (58-59 / 56-59 dB) but not at
1152x648/1280x720, where the sequence replay named dispatch 21 `vitattn` (changes run to run even alone, also with
zeros scrubbed). Kit 6 makes the coopmat fix the default and goes after vitattn. Local only: nothing pushed, uploaded
or posted, no game folder or driver touched.

- Branch `rdna3-k6`, worktree `wt-rdna3-k6`, from `rdna3-k5` faf2caff6. Junctions (not in git, removed before git
  commands, recreated after): `external\vulkan` -> `wt-upstream-merge\external\vulkan`,
  `third_party\mochizuki\toolchain` -> `opti`'s.
- Kit: `E:\Projetos\opti_rr_dlss5\rx7000-diag-kit\OpenNR-RX7000-diag6.zip`, 33.5 MB (35137510 bytes), 408 files,
  SHA-256 `A3701D0ED962314FC1D9362F92C3A574C3307227C3BA50E114A3A141C54F455D`. README beside it: `README-diag6.txt`.
  Needs `dlssnr-small.bin` (copied in from an earlier kit, else downloaded). No NVIDIA files, no models.
- Run here (RX 9070 XT, 26.10.1, `KIT_FORCE_RDNA3=1`, under `gpu-timing.lock`): 3 min end to end
  (`exports\k6work\kittest\result\SUMMARY.txt`).

## Audit: every cooperative-matrix load/store of shaders-rdna3

Done on the SPIR-V (`spvaudit.py` in the session scratchpad: every OpCooperativeMatrixLoad/StoreKHR, its storage
class, array, element type, matrix use, layout and stride), so it covers every define combination actually built.
Strides in bytes; "D" = dispatched by the RDNA3 network (small model, 1152x648-1440p sequence lists of kits 4/5).

Not a multiple of 16 bytes (the only ones):
- `attn` (D): `lds_q`/`lds_k`, f16, stride 36 = **72 B** (NR_QPAD 4): Acc ColumnMajor stores
  `include/attn_qkv_epi.glsl:56-59` (4), B ColumnMajor load `attn.comp:1396` (Q), A RowMajor load `attn.comp:1399` (K).
  Row starts 72*t: 8-byte aligned. **Fixed** (NR_QPAD 0: 64 B).

Runtime strides (checked by hand): `gemmvqkv`/`gemmvqkvs` (gemmvqkvs D) `gemm1x1.comp:1179` Acc RowMajor store to the
arena at stride N f16; the host requires N % 128 == 0 (gemmvqkvs) / N is 3*C: 256 B multiples, offsets tile-aligned.

Everything else is 32 B (f16 stride 16), 64 B, 128 B or 256 B (f32 stride 64), base offsets in whole 16x16 tiles
(NR_OPQ(k) = opaque*16k elements, NR_LXB_, NR_WKB, NR_V_LDS_OFFSET 2048 all multiples of 256 elements):
- LDS: `attn` lds_v 128 B; `ffwd3`/`ffwd3w` nr_ffwd_xchg 32 B; every `fswin*` lds_x/lds_y/ups_projected/
  input_features 32 B, lds_k 64 B, lds_v 128 B (fswindsp32/32nh lds_v also ColumnMajor 64 B).
- Buffers: everything else 32 B (f16) or 256 B (f32 bias, NR_LOAD_ACC_COL stride NR_WIN=64).
- The GEMMs `gemmproj*`, `gemmvact` have no coopmat load/store at all (hand-built fragments); `gemmnores`, `gemmpool`,
  `gemmvnores`, `gemmvproj` buffer loads 32 B.

Strided gathers (A ColumnMajor / B RowMajor, a lane reading sixteen elements one row apart): only `fswinds*` (A Col
from lds_y, 32 B, **not dispatched**) and **`vitattn`'s V^T** (A ColumnMajor from the arena, 16 f16 a lane 32 bytes
apart, `vit_attn_vt_chunk.glsl`, 2 per key block) — the only dispatched one, and vitattn has no LDS fragment at all.
So vitattn's run-to-run change is not an LDS-stride bug; the gather is its one construct no other dispatched kernel
uses. **Changed** (NR_VT_STAGE).

## What changed (local patch 23, RDNA3 tree only)

- `linux/shaders/rdna3/attn.comp`: `NR_QPAD` default 4 -> 0, plus `#error` if a Q/K row is not a multiple of 16
  bytes (skipped for NR_NATIVE). Upstream's own sweep there measured QPAD 0 = 4 within noise and 1 KB less LDS
  (an occupancy step, 11 -> 12 subgroups a SIMD). `attnnat` pins `NR_QPAD=4` so its SPIR-V stays kit 5's.
- `vit_attn.comp` + `include/vit_attn_vt_chunk.glsl`: `NR_VT_STAGE` (on in `pipelines.json`): lane l reads key l%16's
  sixteen dims of V tile l/16 (four 8-byte loads), writes them transposed to `lds_vt` (1 KB a wave; vitattn is one
  wave a workgroup and had no LDS, so no occupancy cost: 64 waves a WGP = 64 KB of 128 KB), subgroup barrier, and both
  V^T fragments load A RowMajor from 32-byte rows. Same values, same fragments, no gfx11 component map involved.
- `include/vit_native.glsl` (RDNA2 tree 22abb593d + NR_DOT2 + RTE, like attn_native) and `NR_NATIVE` in vit_attn.comp:
  pipeline `vitattnnat`, diagnostic only.
- `nr_graph.cpp` `create_kernels`: RDNA3 `attn`/`vitattn` load the shipped g_attn.spv / g_vitattn.spv (the kit 5
  default of the native attn is gone); `NR_RDNA3_ATTN` / `NR_RDNA3_VITATTN` = `native` | `<file>.spv` are diagnostics.
  Log: `[nr] RDNA3 attn: ...`, `[nr] RDNA3 vitattn: ...`.

## Verification here (RX 9070 XT; gfx12 cannot show the gfx11 fault)

- RDNA4 byte identity vs the **real 0.5.8 DLL** (`exports\k6work\ab.ps1`, `ab\ab.txt`): harness 40 frames, small and
  310.8 model, 1280x720 and 1920x1080, driver compiler and ACO: 8 of 8 identical SHA-256. `shaders\` (103 files) and
  `aco\` (324) byte-identical to kit 5's (= 0.5.8's); `shaders-rdna3\` differs only in g_attn, g_vitattn (changed)
  and g_vitattnnat (new); g_attnnat identical to kit 5's. `build_k6.py --check`: rel.spv of both kernels = 0.5.8's.
- V^T staging: probe `vt_stage_test.comp` (the same statements as the chunk) gives exactly the ColumnMajor load's
  fragment, both 100% equal to V^T. LDS probe: 72 and 64-byte rows both 100% right here.
- Alone (kreplay, FP8 inputs): `nat` vitattn 0.0000 rel (exact) and attn 0.0004 confirm the cases; rel/fix of both
  kernels are deterministic but rel 1.0 here (forced gfx11 code on gfx12 writes zeros), so fix = rel bit for bit on
  gfx12 says nothing more. `zfrag` crashes LLPC on gfx12 (left out).
- Forced network: deterministic at every size and mode (seq: none varies); pictures 25.9-26.7 dB for rel, k6 and
  native vitattn alike (wrong from dispatch 1 by design). Network ms forced here, under the lock: rel 5.47 / 6.9 /
  10.7 / 14.7 ms vs k6 5.71 / 7.2 / 11.4 / 15.3 ms (1152x648 / 720p / 1080p / 1440p): +4% on gfx12, native vitattn
  6.9 / 8.5 ms. gfx11 cost is measured by the kit.

## What the kit does

`files\kit6.ps1` (`run-test.bat`), 7 steps:
1. vkprobe + layout probes.
2. Probes (`mprobe\`, kreplay case, 50 runs zeros + 20 with garbage left in LDS/registers): `LDS72`/`LDS64` = a tile
   through an FP16 accumulator, stored ColumnMajor into LDS at 72/64-byte rows (pads unwritten), read back A RowMajor
   and B ColumnMajor; `VT` = vitattn's V^T as released (colmajor) and staged. Each out must be 100.00% equal.
3. Dispatch 11 (`attn\`: rel, fix, nat) and dispatch 21 (`vit\`: rel, fix, nat, velem, vkman0, vsplit0, vtrans0,
   voutvec0, fixvkman0) alone, kit 5's modes; D21 case = forced capture of dispatch 21 at 1152x648 with key 90 from
   the FP8 network, ref key 91 (240 tokens, read twin 202702848+1572864, write 26542080+491520).
4. Model. 5. Sequence replay (MZ_SEQ) with the k6 DLL as shipped, 1152x648/1280x720/1080p, scrub/zero variants,
   keys 74 and 91 against the RX 9070 XT. 6. Pictures 40 frames vs the RX 9070 XT: `rel` (0.5.8 DLL with 0.5.8's
   g_attn/g_vitattn copied back from `spv-rel\`) and `k6` at 4 sizes x 3 runs, network ms each run; `vnat` (k6 +
   native vitattn) at the two small sizes; `best` (a vitattn variant right alone when fix is not) at the two small sizes.
7. SUMMARY.txt and the result zip.

## Reading the player's result

- `Probes`: `LDS72 WRONG or CHANGES` and `LDS64 right` confirms the stride diagnosis in isolation. `VT`: colmajor
  wrong/changing and staged right = the gather is the vitattn fault; both right = the gather is not it by itself.
- `Alone | D11`: fix should read deterministic, rel ~0.0004 (= kit 5's qpad0). `Alone | D21`: the decisive line.
  fix right (rel ~0.01 or less, deterministic) = vitattn fixed. If fix still changes: `velem` right -> the gather's
  coopMatLoad, fix wrong -> something else in the staging (barrier); `vkman0`/`vsplit0`/`voutvec0`/`vtrans0` right ->
  that construct; only `nat` right -> another coopmat construct (the hand-built P^T B fragment and its shuffles,
  the FP16 accumulator `ph`), next kit takes those apart.
- `Replay`: "none" at all three sizes and keys 74/91 near the RX 9070 XT (twin rel ~0.1 or less, kit 5 showed
  0.08-0.10 at key 74 with nat) = no offender left; else the first offender is the next kernel.
- Pictures: `k6` GOOD and identical at all four sizes = the fix (kit 5's nat got 58-59 / 56-59 dB at 1080p/1440p);
  compare its network ms with `rel` for the cost. `vnat` GOOD at the small sizes while `k6` is not = vitattn's
  coopmat path is still wrong there.

## How it was built

- `exports\build-k6.cmd` (DLL + shaders into `exports\mochizuki-k6`).
- `python tools\rx7000-diag6\build_k6.py exports\k6work\spv --check exports\mochizuki-k6\dlssnr-amd\shaders-rdna3`.
- `python tools\rx7000-diag6\make_probes.py exports\k6work\mprobe`.
- D21 case: `tools\rx7000-diag3\capture.ps1 exports\k6work 21`, `aco-refs.ps1 exports\k6work 90,91`,
  `make_case.py cap\D21 case\sets21.txt r3stages.txt aco case\D21 D21` (sets21 by hand from the push constants,
  stages from `wt-rdna3-k3\exports\k3work\r3stage\stages.txt`); D11 = kit 5's case.
- `tools\rx7000-diag6\build-tools.cmd exports\k6work\tools6` (kreplay: rel/fix/nat wide, labels).
- `build-kit6.ps1 <kit 5 laid out files> exports\mochizuki-k6 exports\k6work exports\k6work\kitbuild`, then
  `pack-kit6.ps1 exports\k6work\kitbuild <zip>`.

## Uncertain

- vitattn's fault is a hypothesis: no LDS stride is involved, the gather is the construct unique to it among the
  dispatched kernels. If the probe `VT` and `velem` come back right on the card, the fault is elsewhere in vitattn.
- Whether the LDS bug is the misaligned `ds_load_b128` reading the unwritten pad elements (fits kit 4/5: zero scrub
  deterministic but wrong) is inferred, not seen in gfx11 ISA (no gfx11 compiler here).
- gfx11 cost of the V^T staging unknown (gfx12 forced: +4% network); occupancy unaffected by construction.
- kit 4 also listed gemmnores and gemmvqkvnorms as zero-scrub offenders; kit 5's replay with native attn found them
  deterministic, and they use no strided or misaligned coopmat access, so nothing changed there.
- `vitattnnat` (like kit 5's `attnnat`) is one more pipeline in shaders-rdna3, and the startup check
  (`Rdna3PipelineFiles`) compiles every .spv there: diagnostics only, drop both before a release if not used.
- The kit test here ran on Windows 25H2; the player's earlier kits ran on Windows 10 22H2 (no change expected).
