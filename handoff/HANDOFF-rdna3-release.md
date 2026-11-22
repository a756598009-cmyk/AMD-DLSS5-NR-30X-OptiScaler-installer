# RX 7000 (RDNA3) mochizuki fix for the next OpenNR update (2026-10-28)

Local only: nothing pushed, uploaded or posted, no game folder or driver touched, no version number bumped.
Branch `rdna3-k6`, worktree `wt-rdna3-k6`. Junctions (not in git; remove them before git commands, recreate
after): `external\vulkan` -> `wt-upstream-merge\external\vulkan`, `third_party\mochizuki\toolchain` -> `opti`'s.
`git status` reports ` D external/vulkan` while the junction is off. That is the junction, so leave it out of every commit.

## What changed (commit 7ee1b81c1, local patch 24 in `third_party/mochizuki/UPSTREAM.md`)

Kit 6 rerun on an RX 7900 XTX (driver 32.0.32015.7029): attn with `NR_QPAD=0` is right and deterministic. vitattn
was only right with the native body (no cooperative matrices), and the full network then measured 54-56 dB at
1152x648 and 53-56 dB at 1280x720 (0.5.8: 23-25 dB). The cost was +0.6 ms at 648p and +0.8 ms at 720p. That pair is now the default:

- `linux/shaders/rdna3/pipelines.json`: `vitattn` = 0.5.8's defines + `NR_NATIVE=1`. The shipped `g_vitattn.spv` is
  byte for byte kit 6's `g_vitattnnat.spv` (SHA-256 9FF33CA0...5069DC), and `g_attn.spv` is byte for byte kit 6's
  (61F1B8EE...35773E). Those are the exact two kernels the 7900 XTX ran in the passing `vnat` pictures.
- `attn.comp`: `NR_QPAD` 0 plus the `#error` for rows that are not a multiple of 16 bytes (kit 6). The `NR_NATIVE`
  branch is removed, and so is `include/attn_native.glsl`.
- `vit_attn.comp`: `NR_VT_STAGE` and `lds_vt` are removed. `include/vit_attn_vt_chunk.glsl` is back to 0.5.8's. `NR_NATIVE`
  includes `vit_native.glsl` (comments updated).
- `pipelines.json`: the diagnostic pipelines `attnnat` and `vitattnnat` are removed. `shaders-rdna3` has the same 85 files as
  0.5.8, and only `g_attn.spv` and `g_vitattn.spv` differ. The startup check (`Rdna3PipelineFiles`) lists the folder
  itself, so it compiles 99 pipelines (85 + renodx) as in 0.5.8. Its key hashes names and sizes, so players are
  checked again once after the update.
- `nr_graph.cpp` `create_kernels`: `NR_RDNA3_ATTN` / `NR_RDNA3_VITATTN` keep only the `<file>.spv` form (diagnostic,
  logged only when used). `=native` is gone.

Brought by the earlier kit commits on this branch, which were already in the DLL the 7900 XTX ran, so they belong
to the fix (all RDNA3 only): no persistent runs unless `NR_RDNA3_PERSIST=1` (1eb12d4ba), and the twinbuf barrier
between RDNA3 dispatches by default (`NR_RDNA3_SYNC`, kit 4). The diagnostics `MZ_STAGE_DUMP`, `MZ_KCAP`, `MZ_SEQ` and
`MZ_ONE_QUEUE` are off by default. The kit tools are in `tools/rx7000-diag*` (not shipped). The kit 6 scripts still mention
`NR_RDNA3_VITATTN=native`, which now does nothing.

## Verification (RX 9070 XT, 26.10.1, `exports\fixwork\`, layout as in a game folder)

- RDNA4 vs the real 0.5.8 DLL (`tests.ps1 -StepList ab`, `ab\ab.txt`): harness at 40 frames, small and 310.8 models,
  1280x720 and 1920x1080, driver compiler and ACO. **8 of 8 identical SHA-256** (small 1080p 01588CA6..., 310.8 1080p
  04A421E7..., the known references). `shaders\` (103 files), `aco\` (324) and `int4\` (381) are byte-identical to the 0.5.8
  release. In the archives, `mochizuki-<v>.zip` differs from 0.5.8's only in `MochizukiNrRuntime.dll`.
- RDNA3 forced (`MZ_FORCE_RDNA3=1`), 3 runs of 40 frames: **deterministic at 1152x648, 1280x720, 1920x1080 and 2560x1440**,
  for both 0.5.8 and the fix. On gfx12 the forced output is wrong by design (gfx11 fragment layout), and here 0.5.8 and
  the fix give the same hash at every size. This test proves no variation and no crash. It does not prove correctness;
  the 7900 XTX result does.
- Sequence replay (`MZ_SEQ`, clean frame, released barriers, scrub and zero variants), 4 sizes: **no dispatch varies**,
  none differs from running alone, and no variant differs (`seq\seq-summary.txt`).
- Startup check end to end with the DLL beside `dlssnr-amd`, forced:
  - `MZ_RDNA3_CHECK=pipelines`: `RDNA3 pipeline check passed: 99 of 99 pipelines compiled in 12.5 s on 8 threads`
    (slowest g_fswinpds256 5.5 s), verdict cached, then the network builds and runs.
  - Full check: the layout probes fail cleanly on gfx12, as designed (`RDNA3 network off: acc-f32 ...`).
  (`check\check.txt`)
- Network ms, forced, under `gpu-timing.lock`, interleaved, median of 5 (`timing\timing.txt`):

  | size | 0.5.8 | fix | delta |
  |---|---|---|---|
  | 1152x648 | 5.40 | 6.49 | +1.09 |
  | 1280x720 | 6.97 | 8.48 | +1.51 |
  | 1920x1080 | 10.88 | 13.29 | +2.41 |
  | 2560x1440 | 15.07 | 18.43 | +3.36 |

  On the 7900 XTX the same two kernels cost +0.6 / +0.8 ms at 648p / 720p (5.97 -> 6.6, 6.4 -> 7.2). Scaling as on the 9070,
  that is roughly +1.3 ms at 1080p (11.7 ms) and +1.8 ms at 1440p (19 ms). This is an estimate; it was not measured on gfx11.

## Release artifacts (`exports\release-rdna3-fix\`, `SHA256SUMS.txt`)

Built like 0.5.8: `exports\build-fix.cmd` (= `tools\build-mochizuki-runtime.cmd` with vcvars 14.44 and Vulkan SDK
1.4.357.0) into `exports\mochizuki-fix`, then installer `tools\pin-mochizuki.ps1 -From runtime -License
third_party\mochizuki\LICENSE -Version 0.5.9-opennr` on a **scratch copy** of payload.json (the installer repo is
untouched). `runtime\` = the new DLL, the shaders that were built, and 0.5.8's prewarm manifest (valid: `shaders\` is
unchanged, and pin checked 103 files), with the models and `dlssnr-int4.bin` hard-linked from 0.5.8's release.

- `runtime\MochizukiNrRuntime.dll` 1654784 bytes `8c680ecdd3c216a0c57dda0d4b1ece2c43b237721ab0d4eb5de3af875ec0b513`
- `hf\mochizuki-rdna3-0.5.9-opennr.zip` 9376213 bytes `47a0d9853f9d5bd030dcbd6f835050f853a0ed0b5fa63de6a7425b03237d4fd0`
- `hf\mochizuki-0.5.9-opennr.zip` 84160813 bytes `c162472ad4ba1f59db40047e4fc92e09a56839a660d2d62201a981a4be629a68`
  (the DLL is shared, so this archive changes too)
- Models unchanged: dlssnr.bin version 1, dlssnr-small.bin version 1.

`0.5.9-opennr` in the names is a placeholder for the next version. The archive bytes do not depend on the name, so if
the version differs, rerun pin with the real `-Version` on the real payload. The DLL here is built from this branch.
A DLL built after the merge from the same tree may differ in PE timestamp bytes; if you rebuild, repin.

## Merging into opti main

`dlss-neural-rendering` is at ca2544ec3 with nothing on top, and `rdna3-k6` is based on it. A merge is a fast-forward or a
clean merge (`git merge-tree` reports no conflicts). The commits to bring, in order:

1a5babdfc, 1eb12d4ba, d5556e7a5, 7794bfa6d, faf2caff6, 9c3475a90, 7ee1b81c1, and this handoff's commit on top.

`git merge --no-ff rdna3-k6` from main. 83 files, ~12k lines, most of them `tools/rx7000-diag*` and handoffs.
Expected conflicts: none today. If `mochizuki-rdna2` is merged first, expect conflicts in `third_party/mochizuki/UPSTREAM.md`
(patch numbering, list of local patches), and possibly `nr_graph.cpp` / `nr_runtime.cpp` / `build_network.py`
near the RDNA3 hunks. The installer change for the update (faa397e) is in the other repo and does not interact with this.

## Changelog line

- RX 7000 (mochizuki, experimental): possible fix for the flicker / wrong picture at some resolutions (seen at 648p and
  720p internal on an RX 7900 XTX); the RX 7000 network is about 0.6-0.8 ms slower there. Please test and report.

## Uncertain

- Only one RX 7000 card has confirmed this: a 7900 XTX, in the kit harness, not in a game. The 7800 XT (veyrune) has not run kit 6.
- Two runs on the 7900 XTX still differ from each other at ~55 dB (invisible; before, 25-35 dB). The cause is not known.
- gfx11 cost at 1080p and 1440p is estimated, not measured. vitattn's coopmat path was right at 1080p and 1440p on the 7900 XTX,
  so choosing by size would win back ~1-2 ms there. That was not done: one path is simpler and is the tested one.
- Why the coopmat vitattn fails on gfx11 is still open (not the V^T gather; vsplit0/vtrans0 alone hint at the
  key-chunk split). Next: test TheAutomatic's ACO gfx11 records (mochizuki PR #26) in a kit 7.
