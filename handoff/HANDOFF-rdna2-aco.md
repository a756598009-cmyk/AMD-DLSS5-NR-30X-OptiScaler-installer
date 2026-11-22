# RX 6000 ACO: importer and RDNA2 network loader (2026-10-28)

Local only: nothing pushed, uploaded or posted, no game folder or driver touched, no version bumped. RDNA2 network
shaders unchanged (wt-rdna2 is another workflow's; only read). Before: `HANDOFF-rdna3-aco.md`, `ACO-RDNA2-PROBE2.md`.
The RX 6600 kit v5 result (the final RDNA2 shader form, `PICK`) is still pending, so **no final records are made**:
only a dry run on the current wt-rdna2 export.

## Branches and worktrees

- `rdna3-aco` (worktree `wt-rdna3-aco`): `9d36920cb` importer (local patch 26) and the record tools, plus the tools
  fix fbf457d76 (`git log dd989104c..rdna3-aco`).
- `rdna2-aco` (new worktree `wt-rdna2-aco`, from `mochizuki-rdna2` 664adef39): `2892a9b28` merge of rdna3-aco
  dd989104c (conflicts resolved, see below), `070786c68` and `90bb7c601` merges of 9d36920cb and fbf457d76, then the RDNA2 loader commit (local
  patch 27) and this handoff. Junction (not in git): `third_party\mochizuki\toolchain` -> `opti`'s. Build:
  `exports\build.cmd` -> `exports\mochizuki-rdna2-aco` (tools\build-mochizuki-runtime.cmd).

## What probe 2 said (RX 6600, 26.8.1, "AMD proprietary shader compiler")

The driver's own pipeline binary is a PAL ELF at offset 0 (OSABI 65, ABIVERSION 0, ET_REL, EM_AMDGPU, no checksum),
but from AMD's proprietary compiler: e_flags 0x938 (gfx1032 + xnack any / sramecc off), `.data`/`.comment` sections,
`.text` not 256-aligned in the file, metadata with `.registers` keyed by register **number** (0x2E42.. =
COMPUTE_USER_DATA_n), no global-table entry in `.user_data_reg_map`, other keys (`.resource_hash`, `.uses_uavs`...).
Its round trip works (8/8). What it imports with our code is the **LLPC container** (`synth`: the RX 9070 XT's
sections and metadata keys, e_flags = the bare machine 0x38): BIT-EXACT 2/2, so the gfx10.3 prologue is right on real
hardware. EI_ABIVERSION 1-3: refused (-13). The data is kept in `tools/aco-records/probe2/rx6600-result/`.

## What changed (local patch 26, rdna3-aco; `third_party/mochizuki/UPSTREAM.md`)

- `nr_pal_binary.hpp`
  - `elf_machine(e_flags)`: gfx10.3 machines with feature bits 8-11 are taken (0x938 -> 0x38); gfx11/gfx12 exact as
    before. `Elf` stores that machine.
  - MessagePack maps with non-string keys: kept (NUL + their bytes) and written back unchanged; named lookups never
    match them.
  - `splice_elf` = splice's body; `splice` = `Elf` + `splice_elf` (gfx12/gfx11 output byte-identical).
  - `synthesize(record, machine, push, workgroup)`: the gfx10.3 container from scratch, the exact layout of the RX
    6600's accepted `synth` (header, 7 sections, symbol, metadata keys and order) with the record's settings; per
    pipeline `.api_shader_hash` = {spv hash, 0}, `.128_bit_cache_hash` = {spv hash, code hash} (synth carried probe_a's
    RDNA4 values) and the ISA note names the GPU's machine (synth said gfx1201; PAL ignores it). Refuses: non-gfx10.3
    record or machine (`record and GPU are different generations`), IEEE mode set (the container has no field; ACO
    never sets it), scratch (still refused in `Record::parse` for gfx10.3).
- `nr_pipeline_binary.hpp`
  - A gfx10.3 record is never spliced into a captured template (the driver's container is the proprietary one):
    `device_code` compiles an **empty kernel** with the pipeline's layout once a device (capture, read e_flags and the
    key size, `[nr] ACO: the driver's code objects are for machine 0x.. (e_flags 0x..)`), then the container is
    synthesized; **no template compile** for imported pipelines (the RX 6600 compiles g_fswin32 in 5.5 s).
  - `capture_binary` factored out of `create` (same calls).
  - `fallback_bundle`: `aco-gfx11` and `aco-gfx103x` both fall back per pipeline; RDNA2 log lines
    `[nr] ACO gfx103x <name>: imported` / `not imported (<why>); the driver compiles its template`. A gfx103x kernel
    without a template is compiled by the driver from the bundle's SPIR-V (it is shaders-rdna2's own).
- Tools (`tools/aco-records`): `produce-rdna2.ps1` (one step, below); `produce.py` gives VGPR/scratch for refused
  kernels and knows RADV's gfx10.3 APU families; `make_record.py` vangogh/rembrandt/raphael_mendocino, scratch reason
  with sizes, IEEE-mode refusal; `test_pal_prologue.cpp` + `test-pal.cmd` compare with dd989104c's adapter and
  `--probe2` checks the RX 6600 data (below).

## RDNA2 loader (local patch 27, rdna2-aco)

- `MochizukiNrRuntime.cpp` `Rdna2Aco()`: with ACO asked (LMXXF_NR_CREATE_FLAG_ACO) and `dlssnr-amd\aco-gfx103x\records`
  + `\shaders` present, the RDNA2 network runs that bundle (templates = shaders-rdna2, shaders-rdna2\temporal),
  `[mochizuki] ACO: the RDNA2 network runs ACO's gfx10.3 machine code from ...`; otherwise
  `ACO off for the RDNA2 network: no gfx10.3 ACO records ...; the driver compiles it`. The startup pipeline check
  (Fp16Pipelines) is skipped with the bundle, as on RDNA3 ACO. Status `network` stays 3 (the menu knows 3 = RX 6000).
- `nr_runtime.cpp`: the RDNA2 network takes only a bundle named `aco-gfx103x` (never RDNA4's `aco`).
- Not shipped: the product build has no `aco-gfx103x`.

## Device -> record mapping: one generic gfx1030 (navi21) set for every RDNA2 GPU

- ISA: gfx1030-1036 are one instruction set (LLVM's `gfx10-3-generic` target covers all of them); all have wave32,
  WGP mode, 64 KB LDS and the same VGPR file a SIMD (1024 wave32 VGPRs); e_flags differ only in the machine number
  and the xnack/sramecc feature fields, which the code does not depend on.
- ACO: Mesa 25.3.6 makes the **same bytes** for every family: the probe records navi21/22/23/24 differ only in the
  machine field, and the dry run's 33 network records for navi23, navi24 and rembrandt (gfx1035) are byte-identical
  to navi21's but for that field (99 + 33 comparisons). vangogh/raphael_mendocino captures of g_gemmproj: same code.
- The importer checks only the generation (`same_generation`) and writes the **GPU's** machine (from its own driver's
  empty-kernel object) into the container, so the record's machine never reaches PAL.
- So: ship one `aco-gfx103x` made with `-Family navi21`. Per-chip sets would be identical files.

## Dry run (current wt-rdna2 export, rolled form, `shaders-rdna2` 72 files)

`produce-rdna2.ps1 -Shaders E:\Projetos\opti_rr_dlss5\wt-rdna2\exports\mochizuki-rdna2\dlssnr-amd\shaders-rdna2 -Out <scratch>\dry`
(24 layout runs: small + 310.8, 1152x648/720p/1080p/1440p, Style off/Natural/Cinematic, 2 frames; 41 layouts; then
produce.py navi21, ~3 min CPU). **33 of 49 network kernels get records, 0 with scratch, 0 IEEE.**

- Records (ACO VGPRs, wave32): fswin64/128/256, fswindsp64/128/256, fswinfusedup64/128/256: 256 (LDS 16-64 KB);
  fswin32/32nh, fswindsp32nh, fswinfusedup32/32nh, fswinimagepost32(_grade), fswinimagepreds32nh, attn: 192;
  ffwd3/ffwd3w 160; gemmnores/pool/projw/vact/vqkvnorm(s) 128; vitattn 96; gemmproj/projt/vqkvs 80; gemmprojc 64;
  decups/noisefield 32.
- Refused (the driver compiles them): `temporal_pre_fp32nh`, `temporal_post_fp32`, `temporal_post_fp32_grade` -
  a sampled image before another binding (RADV and AMD size it differently; ACO 192 VGPRs, 0 scratch); 13 not created
  by the RDNA2 graph in any layout run (fswindsp32, fswinimagepreds32, gemmvnores/vproj/vqkv, repack, upsview,
  temporal_pre_fp32 / _nhhp, temporal_post_fp32h16(_grade) / hp(_grade); 32-192 VGPRs, 0 scratch).
- Note: ACO uses more VGPRs than the RX 6600's own compiler (g_fswin32 192 vs 119, g_gemmproj 80 vs 55;
  `rx6600-result\*.stats.txt`): fewer waves on the C=32 bodies. Speed is for the kit to measure.

## Verification on the RX 9070 XT (26.10.1)

GPU runs under `E:\Projetos\opti_rr_dlss5\gpu-timing.lock`; harness `wt-rdna3-aco\exports\acowork\h` (`t.ps1`, and
`ab\`, `fallback\`, `r2\`).
- Importer CPU test (`tools\aco-records\test-pal.cmd <work> --probe2 tools\aco-records\probe2 <record dirs>`): 269
  records (RDNA4 62, int4 75, TheAutomatic's gfx11 62, ours gfx11 33 + 33 coop, probe navi23/navi31) parse, 137 gfx12
  prologues = rdna3-k6's, **every prologue = dd989104c's** (slots 5/23/33), gfx12 and gfx11 splices into the RX 9070
  XT's containers = dd989104c's, the 4 RX 6600 code objects read (machine 0x38, numbered keys, metadata round trip;
  dd989104c refused them), synthesize = the RX 6600's synth for both probes (but the per-pipeline hashes and ISA note),
  navi21 record = navi23 record container, gfx11/gfx12 records and a gfx12 machine refused; 40000 random layouts; the
  dry run's 33 records also parse. 0 failures.
- rdna3-aco DLL (9d36920cb's headers), RDNA4 vs the 0.5.9 release candidate: 8 of 8 identical (258FEC01 / 01588CA6 /
  2A308F2C / 04A421E7 driver, DD7C140B / 5AF86408 / 6938DBD5 / 14028FA5 ACO). Forced RDNA3 + aco-gfx11 records on
  gfx12: 31 `not imported (record and GPU are different generations)`, output = driver's (4AABFA89 at 1152x648,
  D68F26C1 at 720p), same as dd989104c.
- rdna2-aco DLL: exports byte-identical (shaders-rdna2 72 files = wt-rdna2's; shaders 103, shaders-rdna3 85, aco 324,
  int4 381 = rdna3-aco's). RDNA4 8 of 8 identical (same hashes as above). Forced RDNA2 (driver) = wt-rdna2's own DLL
  (7A6DCFA0 at 1152x648, FD74ABAF at 720p, 20 frames).
- Forced RDNA2 with ACO asked and the dry-run `aco-gfx103x` on gfx12: `[nr] ACO: the driver's code objects are for
  machine 0x4e (e_flags 0x4e)`, then every pipeline falls back (small: 28 `record and GPU are different generations`
  + 3 `cannot open binary` = the temporal kernels without a record; 310.8: 29 + 3), imported 0, output **identical**
  to the driver run at 1152x648 and 1280x720 for both models (7A6DCFA0 / FD74ABAF / B83A1C6E / 37A0B7D9).
- Not possible here: an actual gfx10.3 import (needs an RX 6000).

## After kit v5: the final records in one step

1. Build the PICK's shaders-rdna2 (wt-rdna2's workflow) and this branch's DLL with that tree merged
   (`exports\build.cmd`).
2. `powershell -File tools\aco-records\produce-rdna2.ps1 -Shaders <PICK export>\dlssnr-amd\shaders-rdna2 -Out <dir>`
   (defaults: `-Dll exports\mochizuki-rdna2-aco`, `-Family navi21`; mz_e2e.exe from wt-rdna2\exports\rdna2-work,
   models + test.png from wt-rdna3\exports\rx7000-kit; takes `gpu-timing.lock` for the layout runs; `-Layouts <file>`
   skips them; needs `aco-tools` (Mesa 25.3.6 RADV + nr_aco_capture.exe, HANDOFF-rdna3-aco.md) and the HIP SDK
   llvm-mc). Output: `<dir>\aco-gfx103x\{shaders,records}`, `layouts.txt`, `produce-log.txt`, `summary.txt`.
3. Check `summary.txt` (any `scratch on gfx1030` = that kernel stays on the driver; fine, or make it spill-free),
   `test-pal.cmd <work> --probe2 tools\aco-records\probe2 <dir>\aco-gfx103x\records` (0 failures).
4. Copy `aco-gfx103x` into the kit's / product's `dlssnr-amd\`. The records match only the exact SPIR-V they were made
   from: any shaders-rdna2 rebuild needs a new run.

## Kit plan: ACO against the driver on the RX 6600

One DLL (rdna2-aco), the PICK's shaders-rdna2 and its `aco-gfx103x`; steps in order, result zip after each step:
1. **Alone** (safe first): `g_gemmproj` and `g_fswin32` (the 16-user-SGPR spill-table path, not covered by probe 2's
   4-dword probes) imported from their records and dispatched on fixed inputs against the driver's compile of the same
   SPIR-V, 2 runs each: bit-identical is not required (different compilers), compare against a CPU/driver reference
   at >= 50 dB and run-to-run identical. A TDR here = stop, report.
2. **Network, ACO vs driver**: Small 720p and 310.8 720p (then 1080p), 40 frames each, `E2E_CREATE_FLAGS=2` vs `0`,
   alternating rounds, `network ms`, picture vs the RX 9070 XT FP8 reference (>= 40 dB as the kit v5 quality steps),
   runs identical; count `[nr] ACO gfx103x ...: imported` (expect 33 with the dry-run layouts) and list
   `not imported` reasons.
3. **Cold start**: first build time with ACO (no template compiles for imported kernels) vs the driver's (kit v5's
   cold numbers), `dlssnr-amd\aco-cache` emptied before.
4. Per-kernel A/B if ACO is slower overall: `NR_ACO_NATIVE_KERNELS=g_fswin*` etc. (driver for chosen kernels) to see
   which families win with which compiler (ACO's higher VGPR counts on the C=32 bodies are the first suspect).
Decision: ship `aco-gfx103x` if step 2 is GOOD and faster (or faster cold start at equal speed); mixed results =
ship the records only for the kernels that win (drop the rest from `records\`, they fall back).

## Merge plan

- `rdna2-aco` = `mochizuki-rdna2` + `rdna3-aco`; it is the branch to release from for RDNA2 ACO. Into
  `dlss-neural-rendering`: first whatever the RX 7000 release takes from rdna3-aco (7ee1b81c1 etc., as
  HANDOFF-rdna3-release.md), then `mochizuki-rdna2` (the RDNA2 network) - or simply `rdna2-aco`, which holds both.
- Conflicts already resolved here, expected again if mochizuki-rdna2 and rdna3-aco are merged separately:
  - `nr_graph.cpp` persistent runs: RDNA2 always off (`g_rdna2`), RDNA3 off unless `NR_RDNA3_PERSIST=1`
    (`g_fp16 && !g_rdna2`); rdna3-aco's `g_rdna3` does not exist in the RDNA2 tree (`g_fp16` / `g_rdna2`).
  - `nr_runtime.cpp` `record_all`: keep both the step timestamps (`step_pending`) and `seq_end_frame`.
  - Semantic (no textual conflict, compile errors): rdna3-aco's `ctx.rdna3` -> `ctx.arch == nr::Arch::rdna3` (5 in
    nr_runtime.cpp, 1 in nr_graph.cpp), so RDNA2 keeps its barriers and labels.
  - `MochizukiNrRuntime.cpp`, `UPSTREAM.md`, `nrvk.hpp`, `build-mochizuki-runtime.cmd`: merged cleanly.
- After kit v5 changes shaders-rdna2 in mochizuki-rdna2: merge mochizuki-rdna2 into rdna2-aco again (shader-only
  changes do not conflict), rebuild, rerun produce-rdna2.ps1.
- Later importer changes go to rdna3-aco and are merged into rdna2-aco (tools and headers only there).

## Risks / open

- Only probe-sized code has run on RDNA2 hardware. Untested there: the spill-table user-data path (> 16 user SGPRs or
  push pointer), the grid-size pointer with the network's kernels, LDS up to 64 KB, 256-VGPR records, the per-pipeline
  `.api_shader_hash` / cache hash values (synth used fixed ones), the gfx1032 ISA note (synth's said gfx1201), and the
  empty-kernel capture (`device_code`) itself.
- DX10_CLAMP: ACO's gfx10.3 rsrc1 sets it and the container has no field; PAL is expected to set it for compute
  (it does not show in the metadata either way). A kernel relying on clamp NaN behaviour would show in kit step 1/2.
- Other RX 6000 drivers (older/newer than 26.8.1, or one whose RDNA2 branch moves to LLPC) may export another
  container; the import of the LLPC-style one is what was proven. A refusal there falls back per pipeline.
- ACO VGPRs higher than the proprietary compiler's: possibly slower. Mesa 25.3.6 is the record compiler.
- Records are tied to the SPIR-V bytes: shaders-rdna2 and records must be produced together.
