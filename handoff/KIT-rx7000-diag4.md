# RX 7000 kit 4: the network as a sequence (2026-10-27)

Goal: kit 3 showed five RDNA3 dispatches deterministic and close to the FP8 network when each runs alone on fixed
bytes. Kit 4 replays the whole clean frame on the player's card dispatch by dispatch to find the first dispatch whose
output in the sequence differs from itself alone or changes run to run, tries barrier/submit/scrub variants, and runs
the full network, released against a candidate fix, in the same run. Local only: nothing pushed, uploaded or posted,
no game folder touched.

- Branch `rdna3-k4`, worktree `wt-rdna3-k4`, from `rdna3-k3` d5556e7a5. Junctions (not in git):
  `external\vulkan` -> `wt-upstream-merge\external\vulkan`, `third_party\mochizuki\toolchain` -> `opti`'s.
- Kit: `E:\Projetos\opti_rr_dlss5\rx7000-diag-kit\OpenNR-RX7000-diag4.zip`, 28.0 MB (29327191 bytes), 371 files,
  SHA-256 `ADECEF99A8A7ACF175B7D4AB1E72B50253EBC7DBE0F2AB72A2235883533CBD6D`. README beside it: `README-diag4.txt`.
  Needs `dlssnr-small.bin` (copied in from kit 1/2, else downloaded as kit 2 did). No NVIDIA files.
- Run here (RX 9070 XT, 26.10.1, `KIT_FORCE_RDNA3=1`): 5 min through `run-test.bat`, result zip 0.55 MB;
  `exports\k4work\SUMMARY-rx9070xt-forced.txt`. Every replay deterministic and equal to the dispatches alone, every
  picture run-to-run identical, pictures ~26 dB (wrong by design: gfx12 is not gfx11's layout).

## Barriers between RDNA3 network dispatches (0.5.8, what the code does)

All in `nr_runtime.cpp` `Runtime::record` (the network loop) and `compute_barrier`:

| where | barrier | covers |
|---|---|---|
| between network steps i, i+1 (not the last) | `VkMemoryBarrier`, COMPUTE -> COMPUTE, srcAccess **0** (`inv_barrier`: `coherent-act.txt` is 0 in `shaders-rdna3`, so `runner.inv_barrier = true`, `exec_barrier = false`), dst SHADER_READ\|SHADER_WRITE | every buffer: a global memory barrier is not range-limited, so the FP16 twins (separate VkBuffers) are covered exactly like the arenas. What is missing is only the availability half (srcAccess). |
| same, `NR_INV_BARRIERS=0` (kit 2 `fixfb`) | srcAccess SHADER_WRITE | the full Vulkan dependency, all buffers, twins included. Kit 2: no change. So hypothesis (1), twins not covered, is ruled out by construction; the twin is not reached by a narrower barrier anywhere. |
| skipped steps | `runner.no_barrier_after` from tile counters (`g_tc_nobar`) and `NR_CHAIN_KERNELS` | none on RDNA3: `tchain_active()` is false unless `NR_TCHAIN=1`, NR_CHAIN unset. Every RDNA3 step has its barrier. |
| after the last step | full (src SHADER_WRITE) | all |
| `retarget` (first recording) | fill of arena and twin (2 x act_total), noise field into the weights, then TRANSFER\|COMPUTE -> ALL, src TRANSFER_WRITE\|SHADER_WRITE | all |
| stage dump / kernel capture (diagnostics) | full barriers and copies around each dispatch | all; kit 2's stage-dump runs still changed run to run with these in place |

Queues: the core submits frames on the host's queue 0; a second queue (lower priority) is used only for network
builds, which the host waits for (fence) before the network runs. No buffer is written by two queues during frames.
Arena reuse: offsets by lifetime (`arena_layout`), the twin indexed at twice the byte offset, so a twin slot is
reused exactly when its arena slot is; reuse is ordered by the same barriers.

So nothing in the recording leaves a twin or arena write unordered from the next dispatch. Kit 4 checks this on the
card instead of trusting it, and adds the checks for the other explanation that fits kits 2 and 3 (deterministic
alone, nondeterministic in sequence, not cured by barriers): a dispatch reading LDS or registers it never wrote,
whose leftover content depends on which kernel ran on that CU before (the `scrub` / `zero` variants).

## What the kit does

`files\kit4.ps1` (from `tools\rx7000-diag4\`), five steps:

1. vkprobe + layout probes; 2. the small model (verified).
3. Sequence check, k4 DLL, clean frame (seed 0, no history), 6 recordings, frame barriers released
   (`NR_RDNA3_SYNC=released`), `MZ_SEQ` (local patch 21) on recording 3, cases 1152x648, 1152x648 +
   `NR_ARENA_REUSE=0`, 1152x648 + `MZ_ONE_QUEUE=1`, 1280x720 (+ noreuse), 1920x1080 (+ noreuse). For each dispatch i
   (1..75 of 77, the post block left out): dispatches 1..i in one submit with the frame's own barriers, 3 runs, the
   values i makes hashed (plan keys, arena and twin); i alone (prefix in one submit, its bytes kept, then i in its own
   submit after `vkQueueWaitIdle`), 3 runs; variants full, global, twinbuf, submit (one submit per dispatch), scrub
   (random LDS/VGPR leftovers before each dispatch), zero (zeros there), 3 runs each; and each value against the RX
   9070 XT's FP8 network (`reference\seqref\<size>`, every 32nd 256-byte block: twin rel, arena bytes equal, % zero).
   RESULT lines per case: the first dispatch that changes run to run, that differs from itself alone, that changes
   alone; per variant the same. SUMMARY quotes the first offender's lines (bytes differing, max |d|, where in the
   value as a fraction of its size: a tail near 1.0 is the padding tokens of kit 3's finding 2).
4. The fix: the first of full, global, twinbuf, zero that is deterministic and equal to alone in every case, if a
   case reproduced the problem; else `twinbuf`.
5. Full network, 40 frames, against the RX 9070 XT pictures (kit 1's): released 0.5.8 (`rel\`) and the k4 DLL with
   that `NR_RDNA3_SYNC`, 1152x648, 1280x720, 1920x1080, 2560x1440, 3 runs each; controls at the two small sizes: k4
   with the released barrier (the no-persistent-run change alone, kit 2's `fix`) and with `zero`.

Persistent runs (`NR_RDNA3_PERSIST=1`, the 0.5.8 layout) are not replayed: their replay is not faithful
(fswinpup256 changes between replays on the forced 9070 XT, alone too, while the frame itself is byte-identical run
to run and equal to the one-dispatch-a-layer picture). Kit 2 already showed the layer-by-layer network has the same
fault, so the kit replays that one.

## The candidate fix (`NR_RDNA3_SYNC`, local patch 21)

RDNA3 only; read once per runtime: `released` | `full` | `global` | `twinbuf` (default) | `zero`. Default `twinbuf`:
between network dispatches a global memory barrier MEMORY_WRITE\|SHADER_WRITE -> MEMORY_READ/WRITE\|SHADER_READ/WRITE
over ALL_COMMANDS plus whole-buffer barriers on the activation arena and its FP16 twin. `zero`: the released barrier
plus a scrub dispatch with zeros (LDS, ~96 VGPRs) before every network dispatch, a stopgap for uninitialised reads
(the real fix would then be in the shader that reads them). On the forced 9070 XT every mode gives the same picture;
cost: released/full/global/twinbuf 5.7 ms at 1152x648, 11.0-11.4 ms at 1080p; zero 10.0 / 21.0 ms.

Honest expectation: by the analysis above, and kit 2's `fixfb`, a barrier mode is unlikely to be the cure. If the
replay reproduces the fault and only `zero`/`scrub` change it, the fault is a dispatch reading leftovers; if
`submit` alone cures it, it is overlap across dispatches that no in-command-buffer barrier stops (driver side); if
the replay is deterministic and equal to alone everywhere but the full-network pictures still change, the fault is
outside the replayed dispatches (the pre/post block, the passes around the network, the host's recording).

## RDNA4 byte identity

`exports\k4work\ab.ps1`: harness (as games run it), 40 frames, released 0.5.8 DLL against the k4 DLL, small and
310.8 model, 1280x720 and 1920x1080, driver compiler and ACO: 8 of 8 identical SHA-256 (`exports\k4work\ab\ab.txt`).
`shaders-rdna3` byte-identical to 0.5.8's; only the DLL changes.

## How it was built

- `exports\build-k4.cmd` (full: DLL + shaders), `exports\build-dll-only.cmd` (DLL only). Output
  `exports\mochizuki-k4`. Work folder `exports\k4work` (rel\, k4\, dlssnr-amd\ with models, kit2\ unpacked, seqref\).
- `tools\rx7000-diag4\make-seqref.ps1 exports\k4work seqref` (FP8, ACO, `NR_NO_PERSIST=1`, clean, 3 sizes, 13 MB).
- `build-kit4.ps1 <kit2 files> <k4 dll> <seqref> <kit dir>`, then `pack-kit4.ps1 <kit dir> <zip>`.

## Reading the player's result

- `SEQ <case>` lines: "in sequence changes run to run at" / "differs from the same dispatch alone at" name the
  first offending dispatch; the "first offender" block shows how much and where (fractions near 1.0 = the tensor's
  padding tail). "alone changes at" = the kernel itself is nondeterministic on its own real inputs (kit 3 used FP8
  inputs, which may not hit the path).
- A variant line "varies none; differs from alone none" is a cure; the kit then used the first such mode as the fix.
- `ref key` lines (in `seq\<case>\seq.txt`): twin rel vs the FP8 network; on a healthy gfx11 network ~0.003-0.03 at
  the encoder (kit 3), rising slowly with depth. A sudden jump names the first wrong (not just varying) dispatch.
- Pictures: `fix` GOOD and "changes run to run: none" at all four sizes = validated.

## Uncertain

- The scrub's effect is not verified here (the 9070 XT kernels read no leftovers, so it changed nothing); it relies
  on AMD not clearing LDS/VGPRs between dispatches, and covers whatever CUs its 1024 workgroups land on.
- The replay starts from a copy taken before dispatch 1 and runs on our own command buffer; a fault that needs the
  frame's pre block or the host's command stream right before the network is outside it (then the pictures still
  show it and the replay does not).
- Time on the RX 7800 XT: the replays took 12-30 s per case here; with a ~1.5x slower card and a cold LLPC cache
  (~20 s first build here) the kit should stay near 10 minutes, not measured.
- References are from the ACO FP8 network; driver-compiled FP8 would differ by ~50 dB-level noise only.
