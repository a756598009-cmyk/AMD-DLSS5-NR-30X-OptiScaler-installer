# RDNA3 (RX 7000) wrong and changing picture - analysis, fix, kit 2 (2026-10-27)

Input: the RX 7800 XT result of kit 1 (gfx1101, driver 26.10.1 / 32.0.32015.7029, Win 10 25H2). Local only: nothing
pushed, uploaded or posted, no game folder touched.

- Branch `rdna3-padfix`, worktree `wt-rdna3-fix`, from `rdna3-diag` 1a5babdfc (= `dlss-neural-rendering` ca2544ec3 +
  the stage dump, which is off by default and byte-identical). The toolchain junction
  (`third_party\mochizuki\toolchain` -> `opti`'s) is removed before git commands.
- Code change: `third_party/mochizuki/windows/src/core/nr_graph.cpp` (persistent runs off on RDNA3), local patch 19 in
  `third_party/mochizuki/UPSTREAM.md`.
- Kit 2: `E:\Projetos\opti_rr_dlss5\rx7000-diag-kit\OpenNR-RX7000-diag2.zip`, 28.3 MB (29667005 bytes), 189 files,
  SHA-256 `F81FDAB0CBFFDD6F1EA5AEAAF00F54CF97F2159E19F9BEA3E38E6FB25DB8DF92`. Sources `tools\rx7000-diag2\`.
- Fixed DLL: `exports\mochizuki-fix\MochizukiNrRuntime.dll` (`exports\build-fix.cmd`), SHA-256
  `F5E174B094E377F3E7192CAA4DDDC3FEC3504B71E406FE93F58424F66A367B3F`.

## Evidence

### 1. Padding at key 8 (small 1152x648): refuted

Raw key 8 (block 1, fswin32nh, 384x576x32) from the player against an RX 9070 XT ACO dump made here with the same
key (`MZ_STAGE_FULL=8`, recording 36), indexed with the tinlayout map (`tinlayout.hpp`):
- The error is spread over the whole tensor: mean |d| 0.046-0.093 per 4-row band (8-16% of mean |x|) from row 0 down,
  no step where the real image ends (row 324 of 384); rows 320-376 mirror rows 256-320 in both dumps (the pre block
  reflects the image into the padding, `image_input.glsl`), and their error mirrors too. Padding holds the reflected
  image, not garbage, on both cards.
- Only 44.8% of the elements are equal to the reference, although every twin value is an e4m3 value (the RDNA3
  network rounds to e4m3 in ALU), so a right network would match almost everywhere.

### 2. Kit 1's stage dumps at recording 36 cannot name the first wrong layer

The harness runs with motion vectors, so the pre block gets a per-frame noise seed and the history (the network's
previous output). On the RX 9070 XT (ACO) key 0 changes in 81% of its bytes between any two recordings (10, 20, 30,
36, 39) while every run is byte-identical at a given recording. With `NR_DIAG_SEED0=1 NR_DIAG_TGATE0=1` all 75 values
are identical at recordings 3 and 36. So on the RX 7800 XT "key 0 changes run to run" and "first divergence key 8"
are frames 1-35 coming back through the history: any run-to-run change anywhere in the network shows up at key 0 of
a later frame. Key 0's own deviation is smooth (|x| sums +0.3% at the top to +1.6% at the bottom against the
reference, run to run 0.05-0.5% per chunk), the signature of a changed input, not of a broken first kernel.

### 3. Key 71 "deviation ~0.99" at good sizes: an artefact of unused padding

At 960x540 key 71 (the C=256 skip, 2560 tokens of which 2160 real) has, on the RX 7800 XT, values up to 40-44 in
its last three chunks where the ACO reference has zeros; stagecmp then pairs arena-e4m3 views and reports ~0.99.
The RDNA3 arena layout puts that slot over key 8's bytes (the ACO layout gives it fresh memory), so the unwritten
padding tail holds stale data. Same for keys 82, 83, 160, 171. Reproduced on the RX 9070 XT forced onto the RDNA3
network; with `NR_ARENA_REUSE=0` the tails are zero. Nobody reads them in a way that matters: kit 1's noreuse variant
gives the same pictures (960x540 33.4/33.1 dB against 33.7/33.2/33.2).

### 4. The persistent runs of the RDNA3 kernel set are broken (shown on gfx12, inferred on gfx11)

- RX 9070 XT forced onto the RDNA3 network, 310.8 model: 1600x900 583 ms and 1920x1080 1188 ms a frame with the
  persistent runs (the waits run out, error word set), 14.8 and 18.2 ms without; kit 1's forced run also had twin
  values of `fswinpds128` changing between runs.
- Kit 2 forced here, clean runs (seed 0, no history): the released DLL has values made by the persistent runs
  changing between two runs at every size (small: key 184 by fswinpup256 or key 200 by fswinpup128; large 1080p:
  23 of 127 values from key 112 by fswinpds128); the fixed DLL: all values identical in both runs at every size.
- Where they finish, the persistent runs give the same picture as one dispatch per layer (forced, small model,
  1152x648, 1280x720, 1920x1080: byte-identical), so turning them off changes no arithmetic.
- The RX 7900 XT earlier (dd54fc2c3): the tile counters, the other place where data crosses workgroups without a
  barrier, gave a darker picture that changed from run to run.
- On the RX 7800 XT no persistent run reported a wait that ran out, so there it would be stale reads, not timeouts.
  This is not proven on gfx11: kit 2 checks it.

## Cause (best explanation)

The RDNA3 network's persistent runs (`fswinp*`, `fswinpds*`, `fswinpup*`: several Swin layers in one dispatch, the
workgroups handing windows to each other through flags in the arena) are not reliable on the AMD Windows driver with
mauri870's kernels: on gfx12 their waits run out and their values change run to run; on the RX 7800 XT the network
changes run to run at every size and the history spreads it over the next frames. The padding is not the cause.

## Fix

`nr_graph.cpp`: an RDNA3 build does not merge layers into persistent runs (one dispatch per layer, the barrier
between) unless `NR_RDNA3_PERSIST=1`, as it already keeps the barriers the tile counters would drop. The RDNA4 path
is untouched.

- RDNA4 byte identity (harness, RX 9070 XT, driver 26.10.1, 40 frames, FP8 network): released 0.5.8 DLL against the
  fixed DLL, small and 310.8 model, 1280x720 and 1920x1080: identical SHA-256 (6A89FDF7..., 0D5AFED3..., 7DC466CE...,
  A3E8589F...); the RDNA4 log still says "6 merged runs".
- Forced RDNA3 here: small model byte-identical to the released DLL (1152x648, 1280x720), runs identical; 310.8 model
  1600x900 / 1920x1080 no longer stalls and is run to run identical.
- Cost on the forced RX 9070 XT: small 1152x648 5.5 -> 5.7 ms, 1280x720 6.9 -> 7.3 ms (+5-7%).
- The shaders are unchanged (`shaders-rdna3` byte-identical to 0.5.8): only `MochizukiNrRuntime.dll` changes.
  The pipelines the fix uses (`fswin64/128/256`, `fswinds*`, `fswindsp*`, `fswinfusedup*`) are in the 99 the
  runtime's RDNA3 check compiles; they were in kit 1's check on the player's card too.

## Kit 2 (`tools\rx7000-diag2\`)

`kit2.ps1`, `sizes2.ps1`, `make-refs2.ps1`, `build-kit2.ps1`, `pack-kit2.ps1`, `run-test.bat`, `README.txt`; the
tools are kit 1's (`tools\rx7000-diag\build-tools.cmd`). Ships `rel\` (released 0.5.8), `rdiag\` (0.5.8 + stage
dump), `fix\`, the released RDNA3 and RenoDX shaders, kit 1's picture references for its sizes and the clean
references (`exports\reference2`, made by `make-refs2.ps1` here: ACO, seed 0, no history, 12 frames, dump at
recording 3, each size run twice byte-identical). No models: downloaded as in kit 1 (or copied from kit 1).

Runs (small 1152x648, 1280x720, 1344x756, 960x540, 1920x1080, 2560x1440; large 1152x648, 1920x1080):
1. vkprobe and the layout probes; models.
2. Pictures, 40 frames as kit 1: `rel` and `fix`, 3 runs each, PSNR against the RX 9070 XT and run 1 against runs 2
   and 3; `fixfb` (fix + `NR_INV_BARRIERS=0`, full memory barriers) at 1152x648 and 1280x720.
3. Clean runs (`NR_DIAG_SEED0=1 NR_DIAG_TGATE0=1`, 12 frames, dump at recording 3), `rdiag` and `fix`, twice each:
   PSNR against the clean reference, run to run, stagecmp first divergence and `--runs` first changing value.
4. Raw FP16 twin of one value at 1152x648 (fix: the first value that changes run to run, else the first that
   differs; rdiag if the fix has none), up to 32 MB.

Forced on the RX 9070 XT (`exports\test run kit2\`): 8 min (3 of them the released DLL's 310.8 stalls at 1080p),
result zip 8.4 MB, every step ran; pictures wrong by design (~25-27 dB), the findings of section 4 above.

## Reading the result

- IN SHORT: sizes right / wrong / changing per DLL. The hoped-for answer: `fix` GOOD (>= 40 dB; a right RDNA3
  network should be well above, the RX 7900 XT's good sizes were 47-54 dB) and "picture changes from run to run: none".
- `CLEAN fix` lines, `runs:`: "all values identical" = the network itself is deterministic now. If a value still
  changes, it names the dispatch and kernel: that is where to look next (no history, so it is the real origin).
  `CLEAN rdiag` `runs:` should name a persistent-run kernel (fswinp*/fswinpds*/fswinpup*) if this explanation is
  right.
- `first divergence` in the clean lines is the first layer that differs from the RX 9070 XT without history; on
  the player's card a right network gives SAME/CLOSE (e4m3 rounding) through to the end; a DIFFERENT value names the
  kernel to look at (the clean table.txt has per-chunk detail; chunk 15 = bottom of the frame).
- `fixfb` against `fix`: if only `fixfb` is right, the inv-only inter-dispatch barrier (no availability op) is not
  enough on gfx11 and the RDNA3 build should also set `runner.inv_barrier = false`.
- `full\<dll>.small_1152x648\full_<key>_twin.bin`: compare with an ACO dump made here with the same key:
  `exports\diagwork` of `wt-rdna3-diag`, `E2E_W=1152 E2E_H=648 E2E_CREATE_FLAGS=2 NR_DIAG_SEED0=1 NR_DIAG_TGATE0=1
  MZ_STAGE_DUMP=x MZ_STAGE_FRAME=3 MZ_STAGE_FULL=<key> mz_e2e.exe .\diag\MochizukiNrRuntime.dll . test.png out.bin 12 1`.

## Still open

- Not proven on gfx11: the fix makes the forced RDNA3 network deterministic here, but whether the RX 7800 XT's
  run-to-run changes come from the persistent runs only kit 2 can say. If `fix` still changes, the clean `runs:` line
  points at the kernel (an LDS race or an LLPC gfx11 miscompile are the next suspects).
- Even deterministic, the picture may still be wrong on gfx11 at some sizes (kit 1's best was 40.9 dB at 2560x1440
  with persistence on); the clean first divergence will show where.
- The 7 edge-body persistent shaders are moot with the fix (persistent runs off).
