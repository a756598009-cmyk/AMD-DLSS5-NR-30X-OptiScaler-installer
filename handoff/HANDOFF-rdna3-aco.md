# RX 7000 ACO (gfx11) and RX 6000 ACO groundwork (2026-10-28)

Local only: nothing pushed, uploaded or posted, no game folder or driver touched, no version bumped.
Branch `rdna3-aco` (from `rdna3-k6` 4bf8a1fb5), worktree `wt-rdna3-aco`. Junctions as in the earlier handoffs (not in
git; remove before git commands, recreate after): `external\vulkan` -> `wt-upstream-merge\external\vulkan`,
`third_party\mochizuki\toolchain` -> `opti`'s. Base: TheAutomatic/DLSSNR-AMD `codex/rdna3-aco-port` (c46a5ed, MIT,
mochizuki PR #26), cloned read-only into `E:\Projetos\opti_rr_dlss5\ref-theautomatic-aco`. Credit: TheAutomatic for the
gfx11 PAL importer approach (gfx11 container, classic waitcnt, PC-high address word, TGID relocation, NRPAL002 scratch).

## What was ported (local patch 25 in `third_party/mochizuki/UPSTREAM.md`)

- `nr_pal_binary.hpp`: machines `is_gfx12/is_gfx11/is_gfx103`, `same_generation` (one gfx11 record serves every gfx11
  chip), NRPAL002 records (machine + scratch bytes a lane), `prologue_gfx11` for gfx11 **and gfx10.3** (SOP1/SOPP
  encodings, m0/null swap on gfx10.3, every system SGPR relocated from PAL's user count to the record's - TheAutomatic
  moved only the workgroup IDs; workgroup info and the scratch wave offset were left where PAL puts them, which is
  right only when both counts happen to be equal), scratch metadata. The gfx12 path is untouched.
- `nr_pipeline_binary.hpp`: with the `aco-gfx11` bundle, a pipeline whose record is missing or fails to import is
  compiled by the driver from its `shaders-rdna3` template (log `[nr] ACO gfx11 <name>: imported` / `not imported
  (<why>); the driver compiles its template`). Never the Linux SPIR-V itself (LLPC crashes on its rolled loops).
- `nrvk.hpp`: `MZ_LOG_LAYOUT=1` logs every kernel's set layout + push range (needed to make records).
- `MochizukiNrRuntime.cpp`: comments/log text only (Rdna3Aco already existed: bundle `dlssnr-amd\aco-gfx11`).
- Not taken from his PR: his records (made for phoenix gfx1103 with his SPIR-V; ours are ours), LUID/APU selection,
  spoof bypass, installer/package changes, Linux side, the temporal shells (our templates are `shaders-rdna3`, which
  the pipeline check already compiles).
- The product build does **not** ship `aco-gfx11` yet. Records are in `third_party/mochizuki/windows/data/aco/gfx11/`:
  `records\` (0.5.9 fix SPIR-V), `records-coop\` (0.5.8's coopmat attn QPAD 4 + coopmat vitattn), `layouts.txt`,
  produce logs. To ship after kit 7: build `linux/build/build_network.py rdna3` into `dlssnr-amd\aco-gfx11\shaders`,
  copy `records\` beside it.

## Record generation (reusable for gfx1030)

No WSL/VM here (virtualization features off), and Mesa 26 dropped the null winsys. Mesa **25.3.6** still has it and
RADV builds on Windows with MSVC (null winsys only). Everything in `E:\Projetos\opti_rr_dlss5\aco-tools\`
(work folder, not in git; the sources are in `tools/aco-records/`):

1. `git clone --depth 1 --branch mesa-25.3.6 https://gitlab.freedesktop.org/mesa/mesa.git aco-tools\mesa`, then
   `git apply tools\aco-records\mesa-25.3.6-nr-record.patch` (3 small patches, all env-gated):
   - `radv_shader.c`: `NR_RECORD_OUT=<prefix>` dumps a compute shader's ACO code (`.code`) and config/user SGPR layout
     (`.txt`: rsrc1/2/3, LDS, scratch, user SGPR locations, inline push mask);
   - `radv_null_winsys.c`: `NR_ADDR32_HI=<sentinel>` as the 32-bit address high word;
   - `aco_assembler.cpp`: every operand equal to the sentinel is encoded as **m0** (the PAL prologue sets m0 to the
     real high word), plus `NR_M0 subst/writes/constdata` on stderr;
   - `radv_shader_args.c`: no ring-offset user SGPRs for compute (PAL has none; gfx11 scratch does not use them).
2. `pip install --user meson ninja mako pyyaml packaging`; `aco-tools\build-mesa.cmd` (= `tools\aco-records\build-mesa.cmd`;
   vcvars 14.44, `-Dvulkan-drivers=amd -Dllvm=disabled -Db_ndebug=false`) -> `build\radv\src\amd\vulkan\vulkan_radeon.dll`.
3. `tools\aco-records\build-capture.cmd` -> `nr_aco_capture.exe` (loads the RADV DLL directly, no Vulkan loader, no GPU).
4. Layouts: run the runtime forced on the RDNA3 network with `MZ_LOG_LAYOUT=1` (any card; done at 1152x648, 1080p,
   1440p, small + 310.8 models) -> `layouts.txt` (41 kernels).
5. `python tools\aco-records\produce.py navi31 <spv dir> <out> --layouts layouts.txt` (each shader compiled twice with
   two sentinels; the code must be byte-identical, i.e. the sentinel is only a pointer high word; m0 never written,
   no constant data; scratch only on gfx11). 62 kernels in ~25 s. `make_record.py` writes NRPAL002
   (NRPAL001 for gfx12). `cmp_records.py` compares headers, `nrp_dis.py` disassembles (HIP SDK llvm-mc).
- Which target: **navi31 (gfx1100)**. ACO differs between gfx11 chips only in the VGPR budget it plans for
  (gfx1100/1101/1151: 1536 a SIMD; 1102/1103: 1024); the instructions and encodings are the same, and the importer
  takes any gfx11 record on any gfx11 GPU. gfx1100 = gfx1101 exactly (both testers). On gfx1102/1103 the same code is
  valid but may use more VGPRs than a 1024-VGPR plan would (occupancy only). One set serves all; make a phoenix set
  later only if RX 7600/780M owners measure a loss.
- gfx10.3: `produce.py navi21|navi22|navi23|navi24` (no cooperative matrices: `NR_CAPTURE_NO_COOP`). Scratch is
  refused on gfx10.3 (its code needs a ring table PAL has no slot for); the RDNA2 network will need spill-free kernels or
  a scratch-init prologue.
- Rules learned on the RX 9070 XT (our gfx1201 records of RDNA4's network, swapped in one at a time against
  upstream's): 44 of 55 bit-identical in the output, 3 at 55-56 dB (Mesa version), the rest explained:
  (a) RDNA4's ACO path pushes the Windows block with the arena address, and upstream's records index past that gap
  (+2 dwords; verified: our vitattn with that remap is bit-identical). The RDNA3 network removes the gap
  (`aco_push_gap`), so gfx11 records need no remap. (b) A combined image sampler before another binding: RADV and the
  AMD driver size it differently -> wrong offsets (temporal_pre/post). `produce.py` refuses those layouts; such kernels
  use the driver.

## Verification here (RX 9070 XT, 26.10.1; gfx11 code cannot run on gfx12)

- RDNA4 identity, final DLL vs the 0.5.9 release candidate (`exports\acowork\h\t.ps1 -StepList ab`, `h\ab\ab.txt`):
  see the commit message for the hashes (8 of 8; 258FEC01 / 01588CA6 / 2A308F2C / 04A421E7 driver compiler, ACO
  DD7C140B / 5AF86408 / 6938DBD5 / 14028FA5). `shaders\` 103, `aco\` 324, `int4\` 381, `shaders-rdna3\` 85 files
  byte-identical to the release candidate.
- Importer CPU test (`tools\aco-records\test-pal.cmd`): 199 records parse (62 RDNA4 + 75 int4 + TheAutomatic's 62 gfx11),
  all 137 gfx12 prologues byte-identical to the 0.5.8 adapter, 40000 random gfx11/gfx10.3 layouts correct in an SGPR
  interpreter; our 66 gfx11 + probe records parse.
- Fallback: forced RDNA3 with ACO asked on gfx12: 31 pipelines `not imported (record and GPU are different
  generations)`, output byte-identical to the driver-compiled forced run (1152x648, 1280x720).
- ACO probe on this card: gfx1201 records of both probe shaders imported and bit-exact; gfx10.3/gfx11 records refused.
- Kit 7 end to end with `KIT_FORCE_RDNA3=1 KIT_QUICK=1` (3 min): every step runs, zip written.

## Kit 7 (`E:\Projetos\opti_rr_dlss5\rx7000-diag-kit\OpenNR-RX7000-diag7.zip`, `README-diag7.txt`)

Built by `tools\rx7000-diag7\build-kit7.ps1 <kit 6 files> exports\mochizuki-aco exports\acowork exports\acowork\kitbuild`
and `pack-kit7.ps1`. One DLL (this branch). Reading `SUMMARY.txt`:
- `ACO probe: N of 2 ... bit-exact`: 2 = the gfx11 prologue/import works on that card in isolation. REFUSED/WRONG here
  = stop; nothing else ACO can be trusted (aco-probe.txt has the reason).
- `Alone | attn` / `vit` (driver): rel = 0.5.8 kernel, fix = 0.5.9 kernel (attn fix should be right, vit fix = native).
  `Alone | attn-aco` / `vit-aco`: `acocoop` = 0.5.8's coopmat kernel as ACO code, `acofix` = 0.5.9's. The decisive lines:
  acocoop right and deterministic = ACO fixes the coopmat kernels (LLPC was the bug). `ACO not imported` lines under
  them say why if a variant fell back (then the driver compiled the Linux SPIR-V, may CRASH-COMPILE: that is LLPC).
- Pictures `drv` / `acofix` / `acocoop`: GOOD = 40 dB+ every run vs RX 9070 XT, runs identical = no flicker,
  `network ms` per run, `ACO imported N pipelines, driver for M (names)`; expect imported 30-31 and driver for the
  temporal kernels and any not in layouts.txt. acofix GOOD and faster than drv = ship `records\`; acocoop GOOD and faster
  still = ship `records-coop\` (then the coop SPIR-V must be the bundle's shaders, see "What was ported").

## RDNA2 probe (`E:\Projetos\opti_rr_dlss5\rx6000-test-kit\send\OpenNR-RX6000-aco-probe.zip`, `.sha256.txt`)

`tools\aco-records\probe\` (`nr_aco_probe.cpp`, `probe_a/b.comp`, `build-probe.cmd`, `rx6000\`). Two tiny shaders
(A: 2 buffers, 3 inline push constants, workgroup ID; B: LDS + barrier, 2D grid, grid size, subgroup ID, 4 push
constants), records for navi21/22/23/24 (gfx1030/1031/1032/1034), navi31 and gfx1201. The driver compile is checked
against the CPU, then each record is spliced, imported and compared word for word (2 runs). Result file
`OpenNR-RX6000-aco-probe-result.txt`: `navi23 record ... BIT-EXACT` (RX 6600 = gfx1032; navi21/22/24 records should do
the same, same generation) = the gfx10.3 prologue and import work -> RDNA2 ACO network is feasible. `REFUSED (<why>)` =
adapter/driver refused (e.g. PAL metadata version, user-data layout): read the why. `WRONG` / did not finish = the
prologue is wrong for gfx10.3 (most likely the address32 high word or m0); the black-screen note in the README covers
a TDR. navi31/gfx1201 must be REFUSED (different generations).

## Archives

- Kit 7: `E:\Projetos\opti_rr_dlss5\rx7000-diag-kit\OpenNR-RX7000-diag7.zip`, 39992017 bytes, 519 files, SHA-256
  `BD3D9B66DB4157246D24CFBD9467B57DC60AC5A28DDE84A0F43BAD9F27CD00DE` (`.sha256.txt` and `README-diag7.txt` beside it).
  DLL in it: SHA-256 `D57C143C1593D60B2E321B54808845C1767603A06F8BABB2514B401DE33E3987` (this branch, 9fc08a23a).
- RX 6000 probe: `E:\Projetos\opti_rr_dlss5\rx6000-test-kit\send\OpenNR-RX6000-aco-probe.zip`, 193516 bytes, 17 files,
  SHA-256 `72D86C7B34CE95AEBF09BFDE56D045B50AF2D3C6FF47B6F16384F4BB73B2E37C`.

## Commits

9fc08a23a (code, records, tools, kits' sources) and the handoff commit on top. `git log rdna3-k6..rdna3-aco`.

## Uncertain / open

- Nothing gfx11 has run here; the importer's gfx11 path is proven only by the CPU interpreter and TheAutomatic's 780M
  result for the same instruction sequence (his prologue differs from ours in the system-SGPR handling).
- gfx10.3 prologue never ran anywhere (address32 high word assumed = PC high dword as on gfx11).
- Our records come from Mesa 25.3.6's ACO (Oct 2025 code base); upstream's RDNA4 records are from another Mesa. gfx11
  coopmat codegen quality vs newer Mesa unknown. 7 big gfx12 persistent kernels spill in 25.3.6 (not relevant on RDNA3:
  no persistent runs).
- `fswin*`/`gemmv*` gfx11 records use 256 VGPRs and spill (scratch 0.3-1.8 KB a lane, ACO on gfx11 FP16 fragments):
  ACO may be slower than LLPC for those; kit 7's ms will tell. `NR_ACO_NATIVE_KERNELS=<list>` makes chosen kernels
  driver-compiled for an A/B.
- Kernels not created in the layout runs (e.g. Natural style `_grade` variants other than the ones logged, temporal hp)
  have no record and use the driver; rerun the layout runs with more settings before shipping.
- One record per SPIR-V: a kernel created with two layouts would need two (none seen; `produce.py` warns).
