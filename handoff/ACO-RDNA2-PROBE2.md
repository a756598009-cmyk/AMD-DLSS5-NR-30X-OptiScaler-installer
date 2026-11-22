# RX 6000 ACO probe 2: which pipeline-binary container does the RDNA2 driver use? (2026-10-28)

Local only: nothing pushed, uploaded or posted. Branch `rdna3-aco`, worktree `wt-rdna3-aco`. RDNA2 network shaders,
`wt-rdna2`, opti main and the other worktrees untouched.

## Why

Probe 1 on the RX 6600 (driver 26.8.1, `AMD proprietary shader compiler`, driverVersion 0x00800161): all 12 records
`REFUSED (unsupported PAL ELF)`. That message is ours, not the driver's: `nr::pal::Elf` refused the **driver's own
binary** (the container we splice into) at the header check, before the generation check (so gfx1201 and navi31 got
the same reason). The check covers ELF magic/class, EI_OSABI 65, ET_REL, EM_AMDGPU, `takes_machine(e_flags)` (exact
values, so feature bits like xnack/sramecc in e_flags fail it), no program headers, 64-byte section headers. The RX
9070 XT (26.10.1) and the RX 7900 XTX (32.0.32015.7029) say `(LLPC)` in driverInfo and pass; the RDNA1/2 branch
compiles Vulkan with AMD's proprietary compiler, and its container is unknown. Probe 2 captures it.

## Kit

`E:\Projetos\opti_rr_dlss5\rx6000-test-kit\send\OpenNR-RX6000-aco-probe2.zip`, 591804 bytes, 21 files, SHA-256
`707FA38BAFA48C9BF2F206F57F8163E0982B4195FDA707891BE5AB050F8334BE` (`.sha256.txt` beside it). `nr_aco_probe2.exe`
SHA-256 `21D21DE4F0453590A720C154F8D55F4813A186D8A123A54536EC5A9ADD2E8BD9`.
One `run-test.bat`, English `README.txt`, `files\` (exe, `probe_a/b.spv`, `records\<navi21..24,navi31,gfx1201>`
byte-identical to probe 1's, `net\g_gemmproj.spv` + `net\g_fswin32.spv` copied unchanged from
`OpenNR-RX6000-test-v4.zip`, `ref-gfx12\probe_a/b.bin` = this RX 9070 XT's driver containers). Installs nothing, no
game/driver touched; the result goes to `OpenNR-RX6000-aco-probe2-result.zip` next to the bat (system `tar.exe`,
`Compress-Archive` fallback), written after each step so a TDR in step 2 still leaves step 1's data.

Sources: `tools/aco-records/probe2/` (`nr_aco_probe2.cpp`, `pal_probe.hpp`, `analyse_blob.py`, `build-probe2.cmd`,
`build-kit2.ps1`, `rx6000/README.txt`, `rx6000/run-test.bat`, `ref-gfx12/`). Build:
`build-probe2.cmd exports\acowork\probe2` (also runs probe 1's `build-probe.cmd` into `base\` for spv + records), then
`build-kit2.ps1 exports\acowork\probe2 <RX6000-test-v4.zip> <kit dir> <zip>` (captures `ref-gfx12` on this card, packs
with `..\probe\pack-rx6000.ps1`). The product headers (`nr_pal_binary.hpp`, `nr_pipeline_binary.hpp`) are unchanged;
the lenient splice lives only in `pal_probe.hpp`.

### Step 1, `capture` (safe: only the driver's own code runs) -> `report-capture.txt`, `blobs\`, `info\`

- GPU/driver/driverID/Vulkan version, `pipelineCacheUUID`, `VkPhysicalDevicePipelineBinaryPropertiesKHR`
  (**`compressedData`**, internal cache flags), which info extensions exist, the global key (`vkGetPipelineKeyKHR(NULL)`).
- For each shader (probe_a, probe_b, g_fswin32, g_gemmproj; layouts from a small SPIR-V reflection, aliased bindings
  merged, wave32 required as the product does): the pre-compile key (`vkGetPipelineKeyKHR`), the driver compile with
  `CAPTURE_DATA`, every binary from `vkCreatePipelineBinariesKHR` + `vkGetPipelineBinaryDataKHR` saved raw as
  `blobs\<shader>.<i>.bin` / `.key`, and `blobs\<shader>.<i>.txt` = size, entropy, first 64 bytes, compression magic
  (zstd/LZ4/zlib/gzip), every `\x7fELF` offset, for each ELF: OSABI/ABI version/type/machine/e_flags split into
  machine + feature bits, the section table, notes (owner/type/size, small ones in hex), symbols, the first `.text`
  bytes, the whole PAL msgpack metadata, and **`strict importer refuses:`** = each requirement of `nr::pal::Elf` and
  `splice` checked on its own (the exact list of what differs from what our gfx10.3 import expects).
- Probes: the driver compile checked against the CPU, the captured pipeline run; then the round trip: the driver's
  own binary imported back with its key and with our product key, each dispatched twice and compared word for word
  (network kernels: imported and created, never dispatched).
- A second device with `VK_KHR_pipeline_executable_properties` + `VK_AMD_shader_info` (so the binary device is the
  same as the product's): `info\<shader>.stats.txt`, internal representations (`.e0.ISA_cs.txt`, `.e0.ELF_cs.bin` on
  LLPC), `amd-disasm.txt`, `amd-binary.bin` (+ its description). These show the proprietary compiler's ISA, VGPR/SGPR/
  LDS/scratch use - useful even if import never works (compare with ACO's records).

### Step 2, `variants` -> `report-variants.txt`, `variants\` (the containers we built)

Per probe, a fresh capture of the driver's own binary, then:
- Integrity: our-keyed copy with 64 zero bytes appended, and with one bit flipped in the last `.text` byte: created
  only, never run. `ACCEPTED` = the driver does not checksum the blob.
- The record for this GPU (machine from the blob's e_flags, else from the device ID: navi23 on an RX 6600).
- `own-strict` (product splice = probe 1), `own-loose` (no header/version checks, driver's header and metadata kept,
  `.user_data_limit` set to ours, ELF inside a wrapper handled with size fields rewritten), `own-loose-md3.6`,
  `own-loose-osabi65` (only if the blob differs there), `synth` (our record in **the RX 9070 XT's LLPC container**
  with e_flags set to this GPU: does the RDNA2 driver load an LLPC-style PAL ELF at all?), `synth-own-header`,
  `synth-own-md`, `synth-abiver0..3`. Identical bytes are not run twice. Each accepted variant is dispatched twice and
  compared to the driver; a lost device stops the step.

## Reading the RX 6600 result

1. `report-capture.txt`, the `probe_a` block. Find which case it is:
   - **ELF at 0, strict refuses only on e_flags** (e.g. `features 0x...`): the container is PAL ELF with feature
     bits. `own-loose` should then be `BIT-EXACT` -> RDNA2 ACO works; fix = mask feature bits in `takes_machine`
     (product change, small).
   - **ELF at 0 with other differences** (OSABI, ABI version, amdpal.version, missing keys, user-data map entries,
     relocations): `strict importer refuses:` lists them; look at which variant passed. Metadata keys the proprietary
     compiler writes differently (register names, `.user_data_reg_map` values outside our set, a different
     `.user_data_limit`) mean `Arguments` must learn its layout.
   - **ELF inside a wrapper** (`ELF magic at: N` with N > 0, or a suffix): header layout of the wrapper from
     `head`; `own-loose` tries it with size fields rewritten. A refused wrapper with an ELF inside = a checksum or
     signature in the wrapper; `analyse_blob.py` on two blobs (`probe_a` vs `probe_b`) shows which prefix bytes change.
   - **No ELF, high entropy, `compressedData 1`**: compressed. `python -I tools\aco-records\probe2\analyse_blob.py
     <blob>` tries zlib/gzip/lzma (and zstd/lz4 if those modules are installed) and finds the ELF inside.
   - **No ELF, not compressed**: an opaque proprietary container. Then only `synth` can help.
2. `round trip ...: BIT-EXACT` must hold for both keys (driver key and our key). If import of the driver's own
   binary fails, pipeline binaries are unusable on that driver for any purpose -> stop.
3. `report-variants.txt`: any `BIT-EXACT` variant = a working path (that container + our prologue on gfx10.3; also
   the first proof that `prologue_gfx11`'s gfx10.3 branch is right). `accepted ... WRONG` = container taken, code or
   register setup wrong (prologue, address high word, user SGPR layout): diff `variants\*.bin` against the own blob.
   `REFUSED: vkCreateComputePipelines refused it (-13)` = PAL's loader refused it (see the RDNA4 reference below:
   that is what a wrong EI_ABIVERSION does). `vkCreatePipelineBinariesKHR refused it` = the import step itself checks
   the data.
4. Offline: `python -I analyse_blob.py <rx6600 blob> --ref tools\aco-records\probe2\ref-gfx12\probe_a.0.bin` prints
   the header/section/note/metadata differences key by key; `--dis` disassembles `.text` for the e_flags machine
   with the HIP SDK llvm-mc; `--json` for a full dump.

## RDNA4 reference (RX 9070 XT, 26.10.1 LLPC; `tools/aco-records/probe2/ref-gfx12/`)

- Binary properties: internalCache 1, internalCacheControl 0, prefersInternalCache 0, precompiled 0,
  **compressedData 0**. Global key 20 bytes; pipeline keys 16 bytes, and the exported binary's key differs from
  `vkGetPipelineKeyKHR`'s pre-compile key.
- One binary per compute pipeline = a plain PAL code object: ELF64 LE, EI_OSABI 65 (AMDGPU_PAL), EI_ABIVERSION 0,
  ET_REL, EM_AMDGPU 224, e_flags 0x4e exactly (gfx1201, no feature bits), no program headers. Sections: null,
  `.strtab`, `.text` (align 256, code padded with `s_code_end` to 256 bytes), `.AMDGPU.gpr_maximums` (empty),
  `.note.GNU-stack` (empty), `.note`, `.symtab` (null + `_amdgpu_cs_main` FUNC GLOBAL at 0, size = code bytes).
  `.note`: owner `AMD` type 11 (ISA name `amdgcn--amdpal--gfx1201`), owner `AMDGPU` type 32 (msgpack, ~940 bytes).
- Metadata `amdpal.version` [3, 6], one pipeline: `.api` Vulkan, `.type` Cs, `.user_data_limit` = push dwords + 2
  (5 for 12 bytes, 23 for g_fswin32's 84), `.spill_threshold` 65535, `.compute_registers` {tg_size_en, tgid_x/y/z_en,
  tidig_comp_cnt, x/y_interleave}, `.hardware_stages.cs` {checksum_value, debug_mode, entry_point_symbol, excp_en,
  float_mode 192, forward_progress, image_op, lds_size, mem_ordered, scratch_en, scratch_memory_size, sgpr_count,
  sgpr_limit 106, threadgroup_dimensions, trap_present, user_data_reg_map (32 entries: 0x10000000 global table,
  then the push dwords 1..n and the set pointer, 0x10000006 workgroup count when the grid size is read, 0x10000002
  spill table when there are more than 16 user SGPRs: g_fswin32 has 16 user SGPRs with the spill table in the last),
  user_sgprs, vgpr_count, vgpr_limit 256, wavefront_size 32, wg_round_robin, wgp_mode}, `.internal_pipeline_hash`,
  `.shaders.compute` {api_shader_hash, hardware_mapping [".cs"]}, `.xgl_cache_info` {128_bit_cache_hash,
  llpc_version "89.0"}.
- The strict importer takes all four (probes and both network kernels); round trips bit-exact with both keys.
- No integrity check: 64 appended bytes and a flipped `.text` bit are accepted at import and pipeline creation.
  `vkCreatePipelineBinariesKHR` takes anything; PAL checks at `vkCreateComputePipelines`: EI_ABIVERSION 1, 2 or 3 ->
  VK_ERROR_UNKNOWN (-13). So header fields matter, the payload is trusted.
- `VK_AMD_shader_info` BINARY = the same code object plus `.AMDGPU.comment.llvmir` (LLVM IR text) and an `llvmir`
  symbol; the pipeline-executable IRs are `ISA.cs` (text) and `ELF.cs` (that same object).
- `own-loose` and `synth` give the same bytes as `own-strict` here (the lenient path reproduces the product splice).
- g_fswin32 compiles in ~4 s here (169 VGPRs, 107 SGPRs, 8 KB LDS, no scratch); g_gemmproj 0.2 s (51 VGPRs). The
  whole kit runs in ~10-15 s on this card.

## RDNA3 (gfx11) from kit 7, no bytes captured

Kit 7 on the RX 7900 XTX (32.0.32015.7029, `LLPC`) imported 29 pipelines through the strict importer, so its
container passed every check above: EI_OSABI 65, EI_ABIVERSION 0, e_flags 0x41/0x46 with no feature bits, the same
section set (`.text` align 256, one `_amdgpu_cs_main`, no relocations), amdpal 3.6, `.user_data_limit` = slot + 1 and
reg-map entries inside our set. It is the RDNA4 container with another machine. (Probe 2 also runs on RX 7000 if a
byte-level gfx11 sample is wanted: `synth` there uses the navi31 record.)

## What we expect, and next steps

- Most likely (my guess): the RDNA2 legacy branch returns a PAL ELF from the proprietary compiler with differences
  in e_flags feature bits and/or metadata (or a compressed/wrapped PAL ELF). Then `own-loose` (or a variant) tells us
  the exact fix, and the next build changes `takes_machine`/`splice`/`Arguments` for gfx10.3 only (gfx11/gfx12 paths
  and RDNA4 identity untouched), followed by a network-sized test (one gemm and one fswin32 record against the
  driver, like kit 7's `Alone` steps).
- If only `synth` passes: the driver loads LLPC-style PAL ELFs; the product could build gfx10.3 containers from a
  shipped template instead of the driver's (bigger change, needs care with per-generation register metadata).
- If round trip of its own binary fails, or everything is refused with -13 and no ELF is found: RDNA2 ACO on
  Windows is not feasible through VK_KHR_pipeline_binary on that driver; RDNA2 stays on the driver-compiled network
  (the `info\` ISA still helps tune its shaders).
- gfx10.3 scratch stays unsupported (records must be spill-free), as noted in `HANDOFF-rdna3-aco.md`.
