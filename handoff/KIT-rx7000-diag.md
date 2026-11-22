# RX 7000 diagnostic kit (mochizuki RDNA3 flicker) - 2026-10-27

For the RX 7800 XT player (gfx1101, driver 26.10.1, BeamNG) whose RDNA3 network flickers at some sizes, worst at
1152x648. Local only: nothing pushed, uploaded or posted, no game folder touched.

- Zip to send: `E:\Projetos\opti_rr_dlss5\rx7000-diag-kit\OpenNR-RX7000-diag.zip`, **45.6 MB** (47784832 bytes), 252
  files, SHA-256 `689ACCD38E71A8F0A75DC644950F19C59C5C9A344D10D03863482B2F39B3B4CD` (references 23 MB, the released RDNA3
  shaders, the edge-body .spv, two DLLs, tools; no models: they download as in kit 1, from the same links)
- Branch `rdna3-diag` (worktree `wt-rdna3-diag`, from `dlss-neural-rendering` ca2544ec3), committed locally
- Sources: `tools\rx7000-diag\` (kit.ps1, sizes.ps1, run-test.bat, README.txt, mz_e2e.cpp, compare.cpp, stagecmp.cpp,
  pipecheck.cpp, child.hpp, vkprobe.cpp, build_edge.py, build-tools.cmd, make-refs.ps1, build-kit.ps1, pack-kit.ps1)
- Core change: local patch 18 in `third_party\mochizuki\UPSTREAM.md` (`MZ_STAGE_DUMP`, `nr_runtime.cpp` `Impl::StageDump`,
  `windows\src\core\stage_stats.comp`, two lines in `tools\build-mochizuki-runtime.cmd`)
- Work products (not in git): `exports\rel058\` (the released HF zips unpacked), `exports\mochizuki-diag\` (diag DLL
  build), `exports\edge7`, `exports\edge6` (variant .spv), `exports\reference\` + `exports\make-refs.log`,
  `exports\diagwork\` (where the references were made), `exports\kit\` (kit laid out), `exports\test run *\` (tests)

## What the player does

Unzip, double-click `run-test.bat`, wait, send `OpenNR-RX7000-diag-result-<date>.zip`. No admin, Python or VS. The
models download from the same HF links as the first kit (both still answer 200 with the right sizes), SHA-256 checked.
On this PC the whole forced run took 29 min (GPU shared with another agent's runs); on an RX 7000 the RDNA3 check of the first run
(99 pipelines compiled in a child) adds a few minutes. README says 45 to 90 minutes.

## What it runs (kit.ps1)

Frames: 40 per run (1 pass, RenoDX stage, no pan, RGBA16F, test.png = `arrumar_cor\original_sem_nr.png`). 40, not
fewer: at 12 frames the RX 9070 XT itself differs between a cold and a warm build (48-49 dB, something adapts with wall
time), at 40 a cold and a warm run are byte-identical.

Sizes (`sizes.ps1`): 960x540, 1024x576, 1152x648, 1280x720, 1280x800, 1344x756, 1536x864, 1600x900, 1920x1080,
1920x1200, 2048x1152, 2560x1440; models small (310.9, default) and large (310.8, `E2E_FLAGS=8192`).

1. vkprobe + the six layout probes (as kit 1).
2. Models.
3. Released 0.5.8 DLL (`files\rel`, byte for byte the HF `mochizuki-0.5.8-opennr.zip` DLL; shaders-rdna3 and renodx
   from the HF zips), 3 runs per size and model. Each run against the RX 9070 XT reference; run 1 against runs 2 and 3
   at full resolution (`compare --bins`: identical, or PSNR and the box of pixels that differ). GOOD = all runs >= 40
   dB and no NaN; BAD otherwise. At 1152x648 and 1080p run 1 keeps output.jpg and diff_x16.png.
4. Stage dumps: the diag DLL (`files\diag`) twice per size and model with `MZ_STAGE_DUMP`, dump at recording 36.
   `stagecmp` against the reference dump (first diverging value) and dump 1 against dump 2 (`--runs`, first value
   that changes between runs, with the views and chunks that changed). The persistent runs' error words come with it.
5. For small 1152x648: the first diverging value's raw bytes (`MZ_STAGE_FULL=<key>`, arena + twin), kept if <= 80 MB.
6. Variants, small model, all 12 sizes; run A released DLL, run B diag DLL with a dump:
   - `tchain`: `NR_TCHAIN=1`
   - `noreuse`: `NR_ARENA_REUSE=0`
   - `edge7`: `NR_EDGE_BODIES=1` on the seven persistent runs (mauri870's own defines, local patch 12 undone)
   - `edge6`: the same without fswinp256 (the run LLPC crashed on); the kit ships only edge7's files and takes
     edge6 as those minus `g_fswinp256.spv` (the six others are byte-identical)
   The edge variants first go through `pipecheck` (each .spv in a child process: a compiler crash is a line
   "CRASH", the variant is then not run); edge6's files are byte-identical to edge7's, so edge7's check answers for
   them. When they compile, the variant gets `files\v_edge*\rel` and `\diag`, each a copy of that DLL with its own
   `dlssnr-amd` (released shaders with the variant .spv over them, models hard-linked or copied): the DLL reads
   `dlssnr-amd` beside itself when it is there, else beside the exe (`DataDirectory`), and ignores the harness's
   assets argument. The runtime's own RDNA3 check runs on each folder too.
7. SUMMARY.txt (IN SHORT: sizes right / wrong / changing run to run; then the details) and the zip.

Our switches: `KIT_FORCE_RDNA3=1` (`MZ_FORCE_RDNA3=1`, `MZ_RDNA3_CHECK=0`), `KIT_QUICK=1` (3 sizes, small model, 2
runs), `KIT_QUIET=1`.

## The per-stage capture (MZ_STAGE_DUMP), and why this way

Off unless `MZ_STAGE_DUMP=<dir>` is set; then, in recording `MZ_STAGE_FRAME` only, after each network dispatch's own
barrier, every activation value that dispatch produced and a later dispatch reads gets statistics from a small
compute shader. Values are named by their plan key (`key_of(block, layer)` and the skip/pool/ups slots), taken from
`voff.used` (first and last lowered step) and `disp[].lo/hi`, which is what makes RDNA4 and RDNA3 comparable even
where their dispatches differ: both networks lower the same plan, so the same key is the same tensor. Values a
dispatch makes and uses up inside itself are skipped (arena reuse may give their bytes to a later value of the same
dispatch).

The element type of a slot is not recorded anywhere, so each value is read as e4m3, f16 and f32 from the arena and,
on RDNA3, as f16 from its FP16 twin (twin index = arena byte index); stagecmp takes the best matching pairing (e4m3
against the twin first). Each view is cut into 16 contiguous chunks; per chunk: sum, sum |x|, sum x^2, max |x|,
NaN/Inf count, zeros, and an order-free hash of (index, decoded value bits) with -0 as +0, so an e4m3 byte and its
FP16 twin hash alike. A dump is ~150 KB at 1080p (`stages.bin`) plus a text index (`stages.txt`: dispatches with
kernel names, values with key/dispatch/offset/size/plan step, persistent-run epoch and error words). Chunk sums
instead of full tensors keep the dumps (and the references in the kit) small; the chunk index says where in the tensor
it goes wrong (the last chunks are the tensor's end, i.e. the bottom of the frame for these token layouts), and the
hash catches any bit change run to run. Full bytes only for one value, chosen by key (`MZ_STAGE_FULL`), which the kit
takes for the first diverging value at 1152x648.

stagecmp verdicts per value: IDENTICAL (all chunk hashes), SAME (every chunk's sum within 1% of its size), CLOSE
(5%), DIFFERENT. First divergence = first DIFFERENT value in plan order, with the dispatch and kernel on both sides,
the worst chunk, and "C=256 persistent run (fswinp256): yes/no" (the coordinator's hypothesis). `table.txt` has every
value with its per-chunk deviation.

### Reference graph: ACO, not the Windows FP8 network

The stage references are the RX 9070 XT's **ACO** network (`E2E_CREATE_FLAGS=2`), which runs the Linux kernel set
(`linux_network()`: gemmprojc, tile counters, ...), the same set the RDNA3 network runs. Against the Windows FP8 set
the C=512 level (keys 72-82: ffwd3, gemmproj5 vs gemmprojc, attn) differs by 20-60% in the last chunks (rows past the
image the two sets treat differently) while everything after it is SAME again; with ACO as the reference those keys
line up (dispatch lists are the same length, 71 at 1080p, and the kernels match name for name). Measured at 1080p:
Windows FP8 vs ACO stage dump: 46 same, 13 close, 16 different (all 16 at that level); ACO vs ACO: all identical.
The picture references are the same ACO runs (what an RX 9070 XT shows by default). The mapping is therefore by plan
key; no per-kernel mapping table is needed.

### Byte identity (item 6 of the brief), RX 9070 XT, driver 26.10.1

`exports\make-refs.log`, every size and both models (24 lines): the diag DLL with the dump off is byte-identical to
the released 0.5.8 DLL (Windows FP8), and the diag DLL with the dump on gives the same frame as the released DLL on
ACO. Windows FP8 vs ACO pictures: 53.8-57.7 dB (that is "RDNA4 against RDNA4"; the earlier RX 7900 XT's good sizes
were 47-54 dB against the Windows network).

## The /16 level per size (plan probe), and what the earlier tester saw

| size | small /16 h x w | large /16 h x w |
|---|---|---|
| 960x540 | 36x60 (h%8=4) | same |
| 1024x576 | 36x64 (4) | same |
| 1152x648 | 48x72 (0) | 44x72 (4) |
| 1280x720 | 48x84 (0) | same |
| 1280x800 | 52x80 (4) | same |
| 1344x756 | 48x84 (0) | same |
| 1536x864 | 56x96 (0) | same |
| 1600x900 | 64x100 (0) | 60x100 (4) |
| 1920x1080 | 72x120 (0) | 68x120 (4) |
| 1920x1200 | 76x120 (4) | same |
| 2048x1152 | 72x128 (0) | same |
| 2560x1440 | 92x160 (4) | same |

The first tester: 1440p fine (4), 720p both models wrong (0 for both), one 1080p fine and one wrong: at 1080p small is
h%8=0 and large h%8=4, which fits the hypothesis. The large model at 1152x648 and 1600x900 is h%8=4 too, so if the
hypothesis holds the large model should be right there and the small one wrong.

## Tests here (RX 9070 XT), copies of the kit in folders with spaces, models hard-linked

- quick, forced (`exports\test run quick\`): every step ran; the RDNA3 network on gfx12 is wrong as expected (~26 dB),
  run to run identical, first divergence key 0 (the first kernel, fswinimagepreds32nh, arena-e4m3 vs twin-f16, as a
  wrong fragment layout should give), variants ran, edge7/edge6 reported "not run" with every .spv CRASH 0xc0000005 in
  LLPC (driver 26.10.1 on gfx12 crashes on all seven edge-body runs, not only fswinp256), the 1152x648 raw dump
  written (20 MB, key 0), result zip 7.4 MB.
- full, forced (`exports\test run full\`, 29 min, result zip 9.4 MB): all 24 size/model lines, 24 STAGES lines (first
  divergence key 0 everywhere, as expected on gfx12), run to run identical pictures at every size; on the large model
  at five sizes two persistent runs reported a wait that ran out (`run 1/4 epoch 37 error 1`) and a few twin values
  changed between dump runs - the forced RDNA3 network on gfx12, not meaningful for the RX 7000, but it shows those
  paths report. edge7/edge6 not run (LLPC crash), full dump written.
- unforced: the FP8 network is picked, every run is skipped with "this card picked the FP8 network" (0 min).
- variant path (`exports\test run variant\`): with edge7's files swapped for the released ones (so LLPC compiles them)
  edge7 and edge6 ran from `v_edge7\rel` / `v_edge6\rel` (the log shows the weights unpacked there) and were cleaned
  up. That is the only way to exercise it here, since the real edge-body .spv crash LLPC on this card.

## When the zip comes back

1. SUMMARY.txt "IN SHORT": which sizes are GOOD/BAD, which change run to run. Compare with the /16 table.
2. `STAGES <size>` lines: the first diverging key and the RDNA3 kernel that made it; "fswinp256: yes" at all bad
   sizes and none at good ones confirms patch 12 / the C=256 partial window. `stages\<size>\table.txt`: per value and
   chunk; the chunk numbers say where (tensor end = bottom rows).
3. `runs:` (in STAGES) and `runs r1-r2` (in the run lines): a value that changes between runs points at a race; the
   views listed say whether it is the live view (twin-f16 for e4m3 values) or a stale one. Persistent-run error words
   non-zero = a grid wait ran out.
4. Variants: which of tchain / noreuse / edge7 / edge6 makes a bad size GOOD.
5. `full\small_1152x648\full_<key>_twin.bin` (FP16 per element, arena-byte indexed) and `_arena.bin`: to compare
   against an ACO dump made here with the same `MZ_STAGE_FULL=<key>` (diagwork has everything: `E2E_W=1152 E2E_H=648
   E2E_CREATE_FLAGS=2 MZ_STAGE_DUMP=x MZ_STAGE_FRAME=36 MZ_STAGE_FULL=<key> mz_e2e.exe .\diag\MochizukiNrRuntime.dll
   . test.png out.bin 40 1`).

## Rebuild

```
tools\rx7000-diag\build-diag.cmd                       (diag DLL into exports\mochizuki-diag)
python tools\rx7000-diag\build_edge.py exports\edge7    (and exports\edge6 --keep-p256)
tools\rx7000-diag\make-refs.ps1 <work> <reference>      (work = exports\diagwork: rel\, diag\, dlssnr-amd\, tools)
tools\rx7000-diag\build-kit.ps1 exports\rel058 exports\mochizuki-diag exports exports\reference exports\kit
tools\rx7000-diag\pack-kit.ps1 exports\kit E:\Projetos\opti_rr_dlss5\rx7000-diag-kit\OpenNR-RX7000-diag.zip
```
`third_party\mochizuki\toolchain` is a junction to `opti`'s (glslang, slang); remove it before git commands.
