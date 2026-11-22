# mochizuki0323/DLSSNR-AMD

The Vulkan network behind `MochizukiNrRuntime.dll` (`NrBackend=mochizuki`).

- Source: https://gitlab.com/mochizuki0323/DLSSNR-AMD (moved from GitHub on 2026-10-30 with its history; the
  commits named here are the same on GitLab)
- Pinned commit: `931c50f` (2026-11-08, tag v0.0.8.3): `windows/src/core/`, `windows/shaders/` (rdna4, passes,
  renodx, renodx-prep, renodx-fast), `windows/build/` and `windows/data/aco/` (records, int4) as upstream has them,
  apart from the local patches below, and the Linux network ACO is made from (`linux/build/build_network.py`,
  `linux/shaders/rdna4`, `linux/shaders/passes`) and `linux/data/` (int4, int4-ultra, optimized, experimental) from
  the same tag. See Sync to v0.0.8.3. Upstream's unreleased `dev/0.0.8.4` is not taken
- From `9503f6b` (2026-11-13, tag v0.0.9) only the core fixes of Sync to v0.0.9 (core): the PAL adapter's user-data
  layout (GitLab issue 5), the int4 QKV guard, the Preprocess neutral inverse and the Edit strength guard
- Before it: `c53878b` (v0.0.8.2, 2026-11-02; v0.0.8.1 is `339938c`), `cc27501` (v0.0.7, 2026-10-19), and before that `8a70f7f4` (v0.0.5-win-preview) with the hotfix
  `f35f00f` and `windows/src/core/` from `ab00d2b` (v0.0.6-win-preview-alpha)
- `linux/package/model-tools/` is still from `791b046` (v0.0.1); the model it makes has not changed
- Licence: MIT, `LICENSE` beside this file

## What is here

Only what the runtime build reads, at the upstream paths:

| Path | Use |
|---|---|
| `windows/src/core/` | the network runtime (`nr::Runtime`), compiled into the DLL |
| `windows/shaders/` | the network and the passes around it, GLSL; `renodx*` the RenoDX stage's |
| `windows/build/build_network.py`, `unroll_glsl.py`, `quad_quant_glsl.py` | GLSL to SPIR-V, the upstream way |
| `windows/build/arch/rdna4.sh` | the constants host and shaders share; `tools/build-mochizuki-runtime.cmd` repeats them |
| `linux/package/model-tools/` | makes `dlssnr.bin` from a user's `nvngx_dlssnr.dll` 310.8.0 |
| `linux/shaders/rdna3/`, `linux/build/arch/rdna3.sh`, `LICENSE-rdna3` | the RDNA3 network, from mauri870/DLSSNR-RDNA3 (see RDNA3) |
| `linux/shaders/rdna2/`, `linux/build/arch/rdna2.sh` | the RDNA2 network: RDNA3's with its matrices emulated (local, see RDNA2) |

Since v0.0.5 `windows/src/core/` also holds upstream's RenoDX stage (`nr_renodx.*`, `renodx/`), int4 mixed
(`nr_ini_int4.hpp`, `NR_INT4`) and ACO pipeline binaries (`nr_pipeline_binary.hpp`, `nr_pal_binary.hpp`). All three
are built and run as the sections below say (ACO, int4 mixed, RenoDX); `nr_renodx_keys.hpp` (upstream's INI names) is
not compiled, since OptiScaler's `[DlssNr]` keys set the stage.

Upstream's `windows/src/pe/` (its own NGX and ReShade hosts, which need the game to run on vkd3d-proton),
`windows/package/` and `windows/vulkan-loader/` are not used. `OptiScaler/dlssnr/backend/mochizuki_runtime/
MochizukiNrRuntime.cpp` hosts the runtime instead, on a Vulkan device of its own, behind the lmxxf C ABI.

## Local patches

1. `windows/src/core/nr_runtime.hpp` and `nr_graph.cpp`: a host's stop flag for a network build. `nr::build_cancel`
   is a thread-local pointer to an `std::atomic<bool>`, set around the `Runtime` constructor with
   `nr::BuildCancelScope`; the graph build's pipeline loop (`create_kernels` since v0.0.7) checks it before each
   pipeline and, once set, writes the pipeline cache it has so far and throws `NR build cancelled`. `MochizukiNrRuntime.dll`'s `Destroy` during a cold
   build then returns in under a second instead of waiting out the compile, and the next start keeps what was
   compiled. Null by default.
2. `windows/shaders/rdna4/pipelines.json`: `NR_EDGE_BODIES=0` on the eight pipelines upstream builds with 1
   (`fswin32` and the six folded persistent runs, `fswinp256`). An edge body is a copy of the window body for the
   windows at the image's edge; LLPC compiles each copy, and the folded runs' SPIR-V grows 5 to 7 times.
   - Output byte-identical with and without (1080p runtime dumps).
   - Cold network build with no prewarm list and no driver cache (a fresh exe name): 109 s with them, 23.4 s
     without (164 s the first time ever, before the AMD driver had any of it). With the prewarm list: 49 s and 6.1 s.
   - Network time, three interleaved runs each (runtime's own GPU time): 1080p 8.48-8.52 ms with them,
     8.51-8.70 ms without; 1440p 14.95-15.01 and 14.91-15.15 ms. Within the noise.
3. `windows/src/core/nr_runtime.cpp`: `NR_GRAPH_ARGS` (diagnostic), more graph options after the runtime's own.
4. (0.5.3) `nr_runtime.cpp` `resolve_frame_pass`: Intensity above 1 extrapolates on the frame's last pass only; the
   passes before stop at 1 (it compounded to about ratio^8 over three passes).
5. (0.5.3) `shaders/passes/cascade_blur.comp` (windows and linux, the ACO build reads linux/): the detail-only
   cascade's low-pass is edge-aware, weighted by the first pass's input in log luminance, binding 2 = `shown_keep`.
   The box blur left a band of its radius (`mh/60`) around thin edges.
6. (0.5.3) `Controls::anti_halo` and `anti_halo_margin`: `shaders/passes/runtime_dehalo.comp` after the last pass
   holds the answer's luminance inside the first pass's 3x3 range (the first answer is kept in `first_answer`).
7. (0.5.3) `Controls::pass3_estimate`: with three passes the third is `runtime_predict_gain.comp` and
   `runtime_predict_apply.comp`, after lmxxf's `multi_pass_predict.hip`; its timestamps still bracket it.
8. (0.5.4) `nr_runtime.cpp`: `RuntimeConfig::max_passes` up to 30 (upstream refuses above 16), and the timing read's stamp
   array sized for it, after TheAutomatic/dlss-5-amd-project's `08-thirty-layer-mode.patch` (GPL-3.0), for the host's
   `MochizukiUnlockPasses`. 1 to 16 passes run as before (byte-identical at 1 and 3).
9. (0.5.4) `nr_runtime.hpp`/`.cpp`: `RuntimeConfig::first_pass_output`, after TheAutomatic/dlss-5-amd-project's
   `04-first-pass-output.patch` (GPL-3.0), for the host's person protection (`NrPersonPartition`): after pass 1 the
   transfer runs once more into a frame-sized RGBA32F `first_full` (`first_pass_output()`), skipped with the
   preprocess on or the model not applied (`first_pass_written()`). Off, nothing is made or recorded.
10. (0.5.4) `nr_renodx.hpp`/`.cpp`: `StageConfig::first_format` and `StageFrame::first`, the same first pass for the RenoDX
    stage: before pass 2 runs, pass 1's result in the frame's encoding (HDR: the pedestal reduce and the commit, as
    the stack's end makes them, into a `first_out` of the result's format; SDR: the pass's own image) is blitted into
    the host's image; with NR off, the frame as it goes out (`exports_first()`, `first_written()`). Without it set,
    nothing is made or recorded: output byte-identical at 1 and 3 passes, and pass 1 equal to a one-pass run's.
11. (0.5.4) `windows/shaders/renodx/v6_common.hlsli` `ResolvePixel`, the display codec (`Curve == 1`): upstream lerps the
    whole bounded gain by `ColorStrength` and never reads `TransferStrength` there, so colour 0 turned the network
    off. The gain is now `exp2(trust * (TransferStrength * luma + ColorStrength * chroma))`, the split the divisor
    family already makes; at 1 and 1 the same gain as before. `nr_renodx.cpp` `chain_active` follows: a Display
    frame changes the image when either strength is above 0 (upstream: colour, or transfer with pedestal removal).

25. (0.0.8.2 sync) The runtime keeps its composition. Upstream 0.0.8 took everything around the network out of
    `nr_runtime.cpp` (the linear path, Model resolution, the transfer and enlargement, multi-pass, the detail-only
    cascade, the runtime's own Preprocess; `runtime_encode/transfer/upscale/downscale/taps/prep` and `cascade_*` are
    gone from `windows/shaders/passes`) and runs it in its own host (the wilsjo2 flow). Here `nr_runtime.cpp/.hpp`
    stay 0.0.7's with patches 3 to 9 and these of 0.0.8.x ported: the pre block's binding 5 is the noise table
    (`NrSession::noise_buf`: the direct pre set, the control mask's pre, the temporal pre's buffers), History
    Precision as `RuntimeConfig::history_f16` (default true, NR_HIST16 wins; the RGBA16F post where the folder has it
    and its grade twin, else RGBA32F; one pass only), and the 1x1 depth image with Motion Without Depth. The passes
    stay in both `build_network.py` `RUNTIME` lists beside upstream's new `runtime_blit`; `runtime_alpha.comp` is
    upstream's (round-half-up of the product, as the post block's store).
26. (0.0.8.2 sync) `nr_runtime.hpp` `Preprocess::inverse` (default true), which the RenoDX stage's reworked
    [Preprocess] reads; `runtime_prep.comp` ignores it.
27. (0.0.8.2 sync) `nr_graph.cpp` `build_shared`: without `--lazy-images` the noise table is filled there as `build`
    fills it (upstream fills it only through its runtime's lazy path; the host here never takes the shared build).

The new passes are in both `build_network.py` `RUNTIME` lists.

34. (0.0.8.3 sync) `nr_runtime.cpp`/`.hpp`: CC_SILVER_AARDWOLD_OPTIMIZED (`RuntimeConfig::network = 3`,
    `RuntimeConfig::layers`, `Runtime::optimized()`, `int4()`, `layers()`) as upstream has it, except the places, as
    patch 33 does for the experimental network: its shaders in `dlssnr-amd/aco/optimized/shaders`, its weights and
    pipeline cache `aco-cache/dlssnr-optimized.nrb` and `pipeline-optimized.nrb`, its int4 weights
    `aco-cache/dlssnr-optimized-int4.nrb` (upstream: `CC_SILVER_AARDWOLD_OPTIMIZED.bin`,
    `CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin` beside the models, made by its installer; the model files keep their names
    here, `dlssnr.bin` and `dlssnr-small.bin`); RDNA4 only. And `nr::make_int4_weights` (`windows/src/tools/
    nr_int4_weights.cpp`'s run, under the build lock), so that the host makes the int4 weights at first use.
35. (0.0.9 core sync) The PAL adapter's user-data layout beyond upstream's: v0.0.9 reads where the device puts the
    push constants (`pal_metadata`, `xgl_layout`, `push_base` in `Arguments` and `prologue`) for its gfx12 and gfx11
    splices. Here the same `push_base` also reaches `prologue_gfx11` (gfx11 and gfx10.3: the push block's address was
    `s96 + 4` fixed, `0x84`; now `0x80 + 4 * push_base`) and `synthesize` (the gfx10.3 container, which no driver
    compile gives a layout to: `nr_pipeline_binary.hpp` `device_code`, once a device, compiles a small kernel of its
    own layout - set 0 binding 0 a storage buffer, 4 push-constant bytes, both read - and takes `.user_data_limit` - 2,
    `DeviceCode::push_base`; an empty kernel's limit counts only the entries it uses (1 on both layouts, RX 9070 XT);
    metadata without it or a failed probe keeps entry 1, as the RX 6600 probe ran). Log: `[nr] ACO: the driver's
    code objects are for machine 0x4e (...), push constants at user data 1` (2 with VK_EXT_vertex_input_dynamic_state).
    `splice_elf` takes the layout (`XglLayout`) from `splice` or `synthesize`; upstream's new machine check in `splice`
    (gfx10.3 refused) is not taken, `same_generation` stays. The import cache's key (`aco-cache`) also takes the
    device's `push_base` (the same key as before on a plain device, so an existing aco-cache stays valid: its splices
    are byte-identical). At push_base 1 every prologue and splice is byte-identical to the
    adapter before (`tools/aco-records/test-pal.cmd`: 199 gfx12 prologues = rdna3-k6's, 265 = dd989104c's, and the
    40000 random gfx11/gfx10.3 layouts now also at push_base 1-3).

### Sync to v0.0.9, core only (2026-11-16)

Upstream v0.0.9 (`9503f6b`) is one squashed commit (Interleaved presets, native D3D12, processing changes; the RDNA4
network, its records and data unchanged). Taken, as upstream has them:
- GitLab issue 5 (ACO refused on a device with VK_EXT_vertex_input_dynamic_state, graphics pipeline libraries or
  transform feedback, where XGL puts the push constants after entry 1): `nr_pal_binary.hpp` `pal_metadata`,
  `XglLayout` / `xgl_layout`, `push_base`; with local patch 35 for the gfx11 / gfx10.3 prologue and `synthesize`;
- the int4 QKV guard: `linux/build/build_network.py` writes `int4-qkv.txt` (the `NR_SWQ4` pipelines: `fswin256`,
  `fswindsp256`, `fswinp256`, `fswinpds256`, in `int4-ultra\shaders`) and `nr_graph.cpp` refuses a build that would run
  one on a layer without its int4 QKV tables (`g_qkv4_blocks`; log `int4 QKV guard: ...`). A wrong picture without an
  error was seen upstream on RDNA3 Optimized Ultra with persistent runs off; RDNA4 runs them, so here it only guards;
- the Preprocess neutral inverse bounded near the clip (`renodx-prep/prep_math.glsl`, `prep_resolve.hlsli`: the gap
  to the clip at least 1/128, was 1e-6), against pure-colour blotches when Edit strength drives a bright channel to the
  clip, and Edit strength's guard for a channel near zero (`renodx-fast/st_apply.comp`). The three files are
  upstream's byte for byte; `passes/runtime_prep.comp` (the runtime's own preprocess, outside the stage) keeps its
  1e-6 as upstream's does.

Host (`MochizukiNrRuntime.cpp`, not in this tree): a network build that fails with `nr::binary::Error` (ACO's code
itself: a record, its template, an import the driver refuses, a layout the adapter does not know) turns ACO off for the
process (`nr::binary::disable`) and builds again with the driver's compiler, as upstream's host does; before, NR stayed
off ("network build failed ... unsupported XGL user-data layout"). Out of memory and a lost device stay the build's
failure.

Not taken: the D3D12 passthrough (`windows/src/pe/nr_d3d12_passthrough*`, `nr_d3d12_net_plan.hpp`), Passes (fast)
Interleaved and the renumbered `fast_method` (OptiScaler's `MochizukiRenoFastMethod` keeps 0 Extrapolation .. 3 Edit
strength), the history rotation (`Temporal::extra`, `select_history`), `EngineFrame::history`, upstream's RDNA3 bundle
(`windows/data/aco/rdna3`, record stand-ins, `record_reference`), the driver-asked architecture detection
(`nr_gpu_arch.hpp`, `nr_gpu_arch_ids.hpp`, `GpuArch`; `nr::Arch` of patch 31 stays), the `splice` machine check that
refuses gfx10.3, `renodx_prep.comp`'s fused paths and the `_up` resolves, `weights_out`/`plan_d3d12_network`, the HUD,
NR Control and FG changes of its host.

Data the build makes: every network SPIR-V of every folder (shaders, shaders-rdna3, shaders-rdna2, shaders-rdna2-int8,
aco, aco/int4, aco/optimized, aco/experimental, int4, int4-performance, int4-ultra) and aco-gfx11 byte-identical to
mz-gfx11-hybrid's build; new `int4-ultra\shaders\int4-qkv.txt`; five RenoDX SPIR-V changed (`renodx_prep`,
`renodx_st_apply`, `renodx_v6_pedestal_reduce_unshaped_prep`, `renodx_v6_resolve_commit_prep`,
`renodx_v6_resolve_prep`, each 16-276 bytes larger), so `prewarm\manifest.txt` (its folder line: file list and sizes
of `shaders\`) was made again; the RDNA3 and RDNA2 lists cover their own folders and stay.

Harness (RX 9070 XT, AMD Software 26.10.1, `tools/rx7000-kit/mz_e2e.cpp`, test.png, 40 frames, pan 8 px a frame where
noted; `D:\mz-009-core\t\tests.ps1` of the sync), against mz-gfx11-hybrid (470041070, the staged build):
- RDNA4 small and 310.8 model, 720p and 1080p, driver compiler and ACO, pan: byte-identical (8 of 8); Optimized Ultra
  (int4; log `int4 QKV guard: 4 pipelines, 8 layers with tables, consistent`) and Experimental, ACO, 720p and 1080p: byte-identical;
- forced RDNA3 (`MZ_FORCE_RDNA3=1`) and RDNA2 FP16 / INT8 (`MZ_FORCE_RDNA2=1`, `MZ_RDNA2_INT8=1`), 1152x648 and 1080p,
  ACO off and on: byte-identical (12 of 12);
- RenoDX stage, 1080p, pan, exposure off (`E2E_PREP_EXPOSURE=0`; with auto exposure the stage is not deterministic run
  to run, 51 dB between two runs of the same build): Preprocess filmic and neutral alone, and Passes (fast)
  Extrapolation, byte-identical; Edit strength 2 / 4: 52.0-52.6 dB, 1.8% of the pixels (about 1.0-1.3% by more than
  1/255; dark channels of bright pixels, the guard); neutral Preprocess + Edit strength 4: 28.4-28.6 dB, 1.8% of the
  pixels, max 2.4-2.6 (the pure-colour blotches gone); filmic + Edit strength 4: 54.2 dB, 1.4%. Two runs gave the same
  bytes;
- issue 5 (`E2E_DEVICE_EXTENSIONS=VK_EXT_vertex_input_dynamic_state=1000352000`, renamed exe, own data folder), ACO,
  720p and 1080p: the build before fails (`unsupported XGL user-data layout`, NR off); now `push constants at user data
  2`, ACO imports and the output is byte-identical to the plain device's;
- every ACO record damaged (`adapter code checksum`): before, the build failed; now `ACO off: the ACO network did not
  build (...); building with the driver's compiler` and the output is byte-identical to the driver-compiled network's.

### Sync to v0.0.8.3 (2026-11-09)

One upstream commit (`931c50f`). Merged three ways (base `c53878b`, ours, theirs) over the vendored paths; every
local patch kept, none made unnecessary. Taken as upstream has it:
- CC_SILVER_AARDWOLD_OPTIMIZED in place of CC_SILVER_AARDWOLD_INT4_MIXED: the 310.8 model with ReLU in the Swin MLPs,
  fixed attention on decoder layers 66, 67 and 70, merged V/O projections, the constant C=512 heads skipped
  (`const-heads.txt`, `NR_CONSTH`, the `0x7FC00001` scale mark in `nr_graph.cpp`), decoder layers on pipelines of
  their own (`block-routes.txt`), weights = the model XOR `linux/data/optimized/dlssnr-optimized.delta` (7.4 MB, none
  of NVIDIA's weights; entries the model does not have, `performance/<name>`, are added) checked by SHA-256. Optimized
  layers: Quality (no int4), Balanced (`int4/`, `pipelines-int4.json`, new tables, `NR_SWI4_ROT=8`), Performance
  (`pipelines-performance.json`, Balanced's tables, the pack's `performance/` entries via `--model-pack-variant`),
  Ultra Performance (`pipelines-ultra.json`, `linux/data/int4-ultra/`, the default). The int4 weights are made from
  the optimized pack, not the model (new hash); the int4 records are upstream's new ones (44 removed, 62 added);
  `nr_ini_int4.hpp`/`nr_ini_network.hpp` read `[Network] OptimizedLayers` and take the old name as the new network;
- 5 new default ACO records (the optimized network's `fswin32d`/`v`, `attn` with `NR_CONSTH`) and aliases; the default
  network's SPIR-V and records are unchanged;
- the RenoDX stage: `NRSdrBlackRestore` and `NRDisplayPedestal` renumbered (0 Off, the new default; 3 Auto). The host
  maps OptiScaler's keys (0 Auto, as before; 3 now Off) and its own defaults to the old Auto, so the picture is the
  same unless Off is chosen;
- `nr_vram_tag.hpp` `BuildScope`, `nr_control_shared.hpp` version 17 (not used here).

Not taken: upstream's host (`windows/src/pe/`: Memory Guard off by default, the frame generation fixes, the menus,
diagnostics), packaging and installers. Issue #5 (ACO refused with VK_EXT_vertex_input_dynamic_state or graphics
pipeline libraries, `splice` assuming push constants at user-data entry 1) needs no patch here: the core only ever
runs on the host's own VkDevice (`MochizukiNrRuntime.cpp` `Vulkan::Create`), which enables neither; Vulkan, D3D11 and
D3D9 games reach it through D3D12 resources it imports.

Harness (RX 9070 XT, AMD Software 26.10.1, 40 frames, pan 8 px a frame, the sync worktree's `out/tests.ps1`), against the
dc7566a11 build: small and 310.8 model, 720p and 1080p, driver compiler and ACO, byte-identical (the first run of small
1080p with the driver compiler differed once at 54.9 dB; four more runs of both builds, one from a cold pipeline cache,
gave the same bytes); CC_SMALL_FP8_EXPERIMENTAL and forced RDNA3 (720p) byte-identical. Every network SPIR-V the
build makes (shaders, shaders-rdna3, shaders-rdna2, aco, aco/experimental) is byte-identical to the build before, and
every SPIR-V of the new networks has its upstream record (optimized 66, int4 78, performance 80, ultra 80; only the
two temporal motion shaders, as before). CC_SILVER_AARDWOLD_OPTIMIZED, ACO, network GPU time, CC_SILVER_AARDWOLD /
Quality / Balanced / Performance / Ultra Performance: 720p 5.6 / 4.0 / 4.1 / 4.0 / 4.0 ms, 1080p 10.5 / 6.7 / 6.5 / 6.2
/ 6.3 ms; 39.4-41.0 dB from CC_SILVER_AARDWOLD's picture (0.7-1.8% darker), deterministic over two runs. Weights made
at first use: the FP8 pack 0.5 s, the int4 weights 4.8 s, both with upstream's SHA-256.

### Sync to v0.0.8.2 (2026-11-03)

Upstream moved to GitLab; 0.0.8.1 (`339938c`, with TheAutomatic's RDNA3 merge request #26) and 0.0.8.2 (`050b8e5`,
`c53878b`). Merged three ways (base `cc27501`, ours, theirs) over the vendored paths only. Taken as upstream has it:
- the network's video memory (0.0.8.2): slots that no chosen kernel touches get no bytes (`dead`), inside a
  persistent run a layer's output takes the bytes of the output two layers before, the arena laid out largest
  first when that is smaller than the first fit, the post block's never-written second output a 1x1 image
  (`post_no_out1`), the seed-0 noise table in a buffer of its own (`noise_buffer`, the pre block's binding 5, filled
  at build: both RDNA4 folders say so in their manifest; the RDNA3 network keeps it in the weight blob and its
  binding 5 gets a 16-byte stand-in). Harness, RX 9070 XT, ACO: activation arena 1080p small model 409.7 -> 168.0 MB, 310.8 model 392.2 -> 211.0
  MB, 720p small 193.7 -> 88.7 MB; the process grew by 784 -> 475 MB building the 1080p small network;
- the RDNA4 network's shaders (`fswin_t.comp`, `fswin_body.glsl`, `image_input.glsl`, `swin_math.glsl`,
  `pipelines.json`): the noise read, `post_no_out1`, and CC_SMALL_FP8_EXPERIMENTAL's `NR_SACT` / `NR_FIXP` /
  `NR_VOMERGE`, all off in the shipped pipelines. The ACO records of the changed shaders are upstream's new ones:
  62 of 62 default and 75 of 75 int4 match the SPIR-V built here (the folder's other 39 records are
  CC_SMALL_FP8_EXPERIMENTAL's; see its section);
- UTF-8 paths everywhere (`nr_path_u8.hpp`), `nr_vram_tag.hpp`, `nr_gpu_completion.hpp` (the stage's retire
  tickets), `nrvk::blit` (a compute blit on compute-only lists, `runtime_blit.comp`), the PAL adapter's gfx11 and
  gfx11.5 code objects and record version 2 (`nr_pal_binary.hpp`), the ACO loader's fixes (a failure of ACO's code
  itself turns ACO off and builds again with the driver; the import cache keyed on the build too);
- the RenoDX stage (`nr_renodx.*`, `windows/shaders/renodx*`): Passes (fast) (`renodx-fast/st_*.comp`; Extrapolation,
  Feedback, Hybrid, Edit strength), the [Preprocess] rework with Inverse (`renodx-prep/prep_resolve.hlsli`, the
  resolve built again with `NR_PREP_RESOLVE` as `*_prep.spv`), `legacy_decode_reduce`, `renodx_debug`, less GPU time
  for the same output. Local patches 10 and 11 kept. The stage's own new defaults (Detail stability off, the Present
  hook point) do not reach OptiScaler, whose `MochizukiReno*` keys set every field; the stage's NRNoiseEveryFrame is
  upstream host's and not read here;
- RDNA3 in `windows/`: upstream took the same mauri870 fork changes this tree took for patch 13 (FP16 matrix
  configuration, the twins, alias bindings, `plain_tiles`, `kswap`, no persistent runs and no tile chains by
  default). Their copy and ours met in `nrvk.hpp` and `nr_graph.cpp`; upstream's structure is kept where it is the
  same thing (`Context::target_float8`, `has_matrix_config`, the `Kernel` fields `retarget` needs), ours where it
  differs (the twins owned by `GraphAct` / `GraphArenas::wgt_twin` / `GraphStaging::twin`, patch 15; `g_rdna3`, patch
  19's `NR_RDNA3_PERSIST`, `aco_push_gap` for the Linux push blocks). Patch 17's `takes_machine` now takes gfx11 too,
  as upstream's adapter does; the RDNA3 network still runs ACO only from an `aco-gfx11` bundle.

Not taken: upstream's own RDNA3 network (`windows/shaders/rdna3`, `windows/data/aco/rdna3`, `build_aco_network.py`;
see RDNA3), the wilsjo2 style and flow, NR Control (`windows/shaders/control`; `nr_control_shared.hpp` follows
upstream and is not used),
`--lazy-images` (the input copy and answer image made on first use: the composition here binds them at build), the
live History Precision switch (`set_history_f16`; a change here takes a new network), the `carry_history` changes,
supersampling Model resolution, the 0.0.8.2 resize queue and prefetch (upstream's host), signing and packaging.

Dynamic resolution: upstream builds a runtime per render size (`share_for`) and starts each feature's history afresh
at a new size, as NVIDIA's DLL does; `[Network] Resize History = 1` (off by default) carries it with
`Runtime::carry_history`, a bilinear blit of the whole history onto the new size. That is what the reverted 3eff79ec5
did here, and in Cyberpunk 2077 it made each step more visible. The host keeps 5421603fd (auto buckets Cyberpunk's
per-step reallocations, no rebuilds) and restarts the history when the subrect changes, as upstream's default does.

Harness, RX 9070 XT (40 frames, `exports/mochi0082-verify/tests.ps1`), against the 0.5.9 release runtime:
- with NR_HIST16=0 (the RGBA32F history of 0.5.9) byte-identical at 720p and 1080p, small and 310.8 model, driver
  compiler and ACO (8 of 8), and with a pan of 8 px a frame (ACO, 720p and 1080p);
- as shipped (RGBA16F history with ACO): 54.7-57.3 dB from 0.5.9 (mean 0.1-0.2% darker), pan 56.1-57.1 dB; the
  driver-compiled network has no RGBA16F post and stays byte-identical;
- the network's GPU time, 0.5.9 / this: ACO small 720p 3.15 / 2.82 ms, 1080p 5.25 / 5.20, 310.8 720p 5.05 / 4.89,
  1080p 7.45 / 7.07; driver compiler small 720p 4.09 / 4.33, 1080p 6.99 / 6.08, 310.8 720p 6.74 / 5.81, 1080p
  9.96 / 9.08 (one run each, a few tenths of noise);
- forced RDNA3 (MZ_FORCE_RDNA3=1, ACO off): every size deterministic over three runs, 1152x648 byte-identical to
  0.5.9, 720p and 1080p within a few values (max 0.0005; the gfx12 run of the gfx11 network is wrong by design);
  1152x648 6.9 ms, 720p 8.4 ms, 1080p 13.0-13.4 ms, as 0.5.9. The startup pipeline check passed 109 of 109 (99
  before: the stage's ten new shaders).

### Sync to v0.0.7 (2026-10-19)

All eleven patches are kept; v0.0.7 does none of them. Taken as upstream has it:
- the Style grade: Natural and Cinematic graded after the post block as NVIDIA's DLL does
  (`include/nr_style_grade.glsl`, `NR_STYLE_GRADE`, the `style_grade` table of `pipelines.json`: the
  `g_fswinimagepost32_grade` and `temporal_post_fp32*_grade` twins, dispatched only for those two Styles, with their
  ACO records, 4 default and 4 int4). `Runtime::set_style_grade`, on by default, is the host's `MochizukiStyleGrade`;
  Standard runs the pipeline it always did;
- `RuntimeConfig::motion_point`: the game's vectors sampled at the nearest texel, as the DLL binds them. The host sets
  it. `[Network] Motion Without Depth` (default 1: the depth no longer picks where the vector is read) is read from a
  `dlssnr-amd.ini` beside the game, which OptiScaler does not write; the RenoDX stage passed no depth to the network
  already, so nothing changes there and `MochizukiUseDepth` still only reaches a network outside the stage;
- `runtime_transfer.comp`: negative linear channels taken as zero before the ratios (bright dots on scRGB frames below
  100% model resolution). The transfer runs only for a network outside the RenoDX stage; the stage's own resolve,
  which OptiScaler always uses, clamped them already;
- the resize path (`GraphCore`, `build_shared`, `RuntimeConfig::share`/`deferred`/`plan_only`, image heaps,
  `carry_history` for `[Network] Resize History`), residency (`GraphAct`, `RuntimeConfig::act_arena`,
  `offload_network`/`reload_network`) and pipelines shared by every kernel of one SPIR-V file (`nrvk::SharedPipeline`).
  The weight blob now ends with the extent-dependent noise field (same values, other offsets) and arena zeroing moved
  into `retarget`; nothing of it changes a picture. The host uses only `act_arena`: a network built while the frames
  keep the current one takes that one's activation arena when it fits (log: "activation arena: taken over"); a switch
  to the 310.8 model at 1080p then allocates no second 400 MB arena. Not used: `share_for`, deferred builds inside a
  frame and `carry_history`. The host keeps a dynamic-resolution game on one network built for its bucket (64 px,
  `MochizukiDynamicResolution`), which never rebuilds within it, so upstream's per-size runtimes are not needed;
  Resize History has nothing to carry there;
- a game whose colour switches format: upstream lets a runtime of another tex-in format share the network
  (`share_arguments` clears `--tex-in-format`/`--out0-format`), through `share_for`. The host's own fix
  (`float-colour-shared`, one RGBA16F network for R11G11B10_FLOAT and RGBA16F frames, a blit each way) stays: it
  switches with no build at all and the marker builds the right network from a later session's first frame; with
  the RenoDX stage the network is RGBA16F anyway and only the stage depends on the frame's format;
- the network names (`nr_ini_network.hpp`: `CC_SMALL_FP8`, `CC_SILVER_AARDWOLD`, `CC_SILVER_AARDWOLD_INT4_MIXED`),
  which the host's build log line now uses; `nr_ini_int4.hpp` and `nr_control_shared.hpp` (NR Control's shared
  memory, `windows/src/pe/`'s menu) follow upstream and are not used by the host;
- `nr_renodx` `Stage::facts()` (the Engine data panel's source), `tinlayout.hpp`'s faster weight packing.

Already in our tree: the CC_SMALL_FP8 stale block at the left edge (fixed in `linux/` by 0.0.7) was fixed on Windows
before (the barrier kept between the C=64 decoder and the fused C=32 upsample, `kinds & 32`). Upstream's in-game
network switch, OSD, Engine data panel and `[Network] Residency` modes are its own host's (`windows/src/pe/`); the
typeless exposure is OptiScaler's (`LmxxfBackend.cpp` reads an R32/R16/RGBA typeless exposure as its float format;
typeless motion vectors were taken already).

Harness (RX 9070 XT, AMD Software 26.9.2, ACO, 40 frames, the runtime's own GPU time), 0.5.5 against this tree, new
model / 310.8 model: 720p 3.22 / 5.04 and 3.21 / 5.00 ms, 1080p 5.30 / 7.75 and 5.28 / 7.43, 4K 20.9 / 24.3 and
20.5 / 24.6. Output byte-identical at all six, with a pan of 8 px a frame (both models), R10G10B10A2, Standard set
through the controls, Cinematic with the grade off, and across a live model switch and an int4 switch. With the grade:
Natural 25.9 dB from 0.5.5 (15% darker on average), Cinematic 38.4 dB; 5.16 -> 5.23 ms.

### Sync to v0.0.5 (2026-10-14)

Upstream took in both driver 32.0.32015 fixes of 2026-10-04 (the unrolled `ffwd3_t`, `attn` and `vit_attn`, and
`vit_attn`'s padding tile, now killed after the load rather than chosen before it), so they are no longer local.
`NR_TCHAIN_TILES=1` joined `rdna4.sh` and the build script. Every pipeline now carries a
`VkPipelineRobustnessCreateInfoEXT` with robust access off where the driver has the extension; the prewarm
(`mz_interpose.cpp`) records and replays it, and its manifest is version 2 (a version 1 list is refused and made again).

The runtime's own GPU time, RX 9070 XT, driver 32.0.32015, against the 0.5.1 release (same harness, pan of 8 px a
frame): 1080p 13.9 / 13.9 ms, 2 / 3 passes 28.0 / 27.4 and 41.6 / 40.3, 1707x961 11.6 / 11.4, 1280x720 7.9 / 7.6,
3840x2160 51.7 / 50.9. Output byte-identical at 1080p (1, 2 and 3 passes, R10G10B10A2), 1707x961, 1280x720,
1129x635, 4K and without motion vectors; 53-56 dB from it with an RGBA32F frame and with the preprocess on.
Not taken: `forget_history`/`drop_history`, which the host does not call yet. Upstream's enlargement below 100% model
scale (`Controls::transfer`, `classic_scaler`) and `noise_every_frame` are the host's `MochizukiEnlargeMode`,
`MochizukiClassicScaler` and `MochizukiNoiseEveryFrame` since 2026-10-17.

Host side (not in this tree): `MochizukiNrRuntime.cpp` enables `bufferDeviceAddress` when the driver offers it and
sets `HostDevice::buffer_device_address`; marks its colour and motion images `SAMPLED`
(`ColourFrame::usage`), so the core reads them in place; and sets `RuntimeConfig::preprocess_unknee` on the linear
path. `nr_runtime.cpp` is compiled with the defines of `rdna4.sh`.

### Dropped on 2026-10-02

The previous pin was `791b046` (v0.0.1) with sixteen patches, most of them parts of upstream's `linux/` tree
ported by hand and tuned for LLPC. Upstream's `windows/` now carries all of it, rebuilt for the AMD Windows driver
(228d3a6, b1419b0) and with the motion and history work of d1185d2:
- the history ping-pong, the preprocess, the colour formats and the post block's 0.7397 weight (old 12, 16) are
  upstream's own;
- the frame kept in its own format and read in place (old 13) is upstream's `ColourFrame::usage` path;
- the alpha pass skipped where it changes nothing (old 14) is upstream's `NR_POST_ALPHA`;
- the pre block's round-to-nearest colour input (old 15) is upstream's `NR_HALF_RTE`, which on LLPC gives the same
  bytes as ours did (RGBA32F frames with values between two halves, compared byte for byte);
- word-wide e4m3 reads, quad quantisation, the V tiles in rows and the work-split knobs (old 2 to 4, 7 to 11) have
  upstream equivalents (`NR_WLOAD_U32`, `NR_WPAIR_U32`, `quad_quant_glsl.py`, `NR_V_LDS_ROW`), tuned by upstream;
- the MSVC cast (old 1) is fixed upstream.

Measured against the previous build (RX 9070 XT, driver of 2026-08-18, the runtime's own GPU time):

| Case | Previous | Now |
|---|---|---|
| 1080p, 1 pass | 9.18-9.34 ms | 8.45-8.52 ms |
| 1080p, 2 / 3 passes | 29.0 / 42.3 ms | 25.8 / 38.3 ms |
| 1707x961 / 1280x720 | 12.7 / 8.6 ms | 11.0 / 7.6 ms |
| 3840x2160 | 55.6 ms | 48.1 ms |
| Cold build with the prewarm list | 7.6 s | 6.1 s |

The output is 49-56 dB from the previous build at 1080p and above (upstream's per-frame noise and the changes
above). A pan with exact motion vectors at 1707x961 and 1280x720 is about ten times steadier frame to frame than
before (mean |out_n - shift(out_n-1)| 0.0007 against 0.0102 and 0.0075), the motion-vector work of d1185d2.

## ACO (OptiScaler's `MochizukiAco`)

Upstream's [Network] ACO Mode: the network as machine code made in advance with Mesa's ACO, loaded through
`VK_KHR_pipeline_binary` instead of compiled by the AMD driver. Vendored for it: `linux/build/build_network.py`,
`linux/shaders/rdna4/` and `linux/shaders/passes/` (the Linux network, which the records were made from), and
`windows/data/aco/records/` and `shell-aliases.txt` (62 used, the Style grade twins included; since v0.0.8.2 the folder holds 101, the other 39 CC_SMALL_FP8_EXPERIMENTAL's). The build script writes
`dlssnr-amd/aco/` beside the shaders: `shaders/` built from the Linux sources, `records/`, `shell-aliases.txt`.
Each record is named by the FNV-1a hash of its SPIR-V, so the Linux network must build byte for byte as upstream's
(glslang 16.5.0): all 62 match.

The host (`MochizukiNrRuntime.cpp`) takes `LMXXF_NR_CREATE_FLAG_ACO`: once a process, before its Vulkan device,
`nr::binary::configure` with the bundle, the shell folders (`aco/shell`, our `shaders`, `shaders/temporal`) and
`dlssnr-amd/aco-cache/` for the imports; the device gets pipeline binaries and maintenance5 through
`nr::binary::Features` when the driver offers them, else the core falls back to the driver's compiler. Our
`NR_EDGE_BODIES=0` shaders serve only as templates for the code's container, so the patch stays. The native prewarm
list is skipped with ACO on (the imports are cached instead).

Measured with the harness (RX 9070 XT, AMD Software 26.9.2, pan 8 px a frame), the runtime's own GPU time, driver
compiler / ACO: 1080p 13.9 / 10.5 ms, 2 passes 27.4 / 20.2, 1707x961 11.4 / 8.9, 1280x720 7.6 / 6.0, 3840x2160
50.9 / 38.5. Picture 54-57 dB from the driver-compiled one. First start 22.5 s (the imports), then 1.5 s.

## int4 mixed (OptiScaler's `MochizukiInt4`, until 0.0.8.3)

Since v0.0.8.3 `MochizukiInt4` runs CC_SILVER_AARDWOLD_OPTIMIZED (below); this section is how int4 mixed was.
Upstream's second network, part of it in 4-bit integers, built only as ACO's machine code. Vendored for it:
`linux/data/int4/rdna4/` (the quantisation tables `vit/` and `swin/`, `settings.txt`, the weights' hash),
`linux/shaders/rdna4/pipelines-int4.json`, `windows/data/aco/int4/` (75 records and their aliases) and
`windows/src/tools/nr_int4_weights.cpp`. The core is compiled with `NR_INT4=1` (every unit: `RuntimeConfig` holds
`int4_mixed` under it). The build writes `dlssnr-amd/aco/int4/` (the Linux int4 network and its records, the aliases
merged into `aco/shell-aliases.txt`) and `dlssnr-amd/int4/` (the tables, settings and the same SPIR-V); with
`MZ_MODEL` set to a `dlssnr.bin` it also runs `dlssnr-int4-weights.exe` and writes `dlssnr-amd/dlssnr-int4.bin`
(53 MB, 8 s), checked against `dlssnr-int4.sha256`: the model the payload pins gives upstream's hash exactly. All 75
records match the SPIR-V built here.

The host asks for it a frame at a time (`LMXXF_NR_FRAME_FLAG_INT4`, part of the network key): a change rebuilds the
network while the frames keep the current one where VRAM allows, then frees it; one network stays resident (no
upstream KeepBoth). Without ACO or the files the core logs why and builds the default network.

Harness (as above, ACO on), default / int4: 1080p 10.1 / 9.2 ms, 1280x720 5.9 / 5.2, 3840x2160 38.5 / 34.9. The
picture is 39-41 dB from the default network's, 0.6-0.8% darker on average. A switch in a running session: the int4
network ready in 4.5 s, the frames on the old one meanwhile. First int4 build 23 s (the imports), then 4.7 s.

## CC_SMALL_FP8_EXPERIMENTAL (OptiScaler's `MochizukiExperimental`)

Upstream's fourth network (0.0.8, Windows since 0.0.8.1, at the same `c53878b`; no newer release on 2026-11-05): the 310.9
small model made faster, in FP8 like it, ACO's machine code only, so RX 9000 only. ReLU as the Swin MLP activation
(`NR_SACT`), one fixed attention table a layer on three decoder layers (`NR_FIXP`, the V and output projections merged,
`NR_VOMERGE`), the changed layers' weights fitted to the original network's outputs. Vendored for it:
`linux/shaders/rdna4/pipelines-experimental.json` and `linux/data/experimental/` (`*.delta`, each changed entry XOR the
small model's, which holds none of NVIDIA's weights; `*.sha256`, of the result). The 39 records were already in
`windows/data/aco/records`, and the graph's `fixed-attention.txt`, `--spv-dir-aco` and `fswin32nhd` came with 0.0.8.2.

Local patch 33: `nr_runtime.cpp` `RuntimeConfig::network = 2` and `Runtime::experimental()` as upstream has them, except
the places: the shaders in `dlssnr-amd/aco/experimental/shaders` (`tools/build-mochizuki-runtime.cmd`), which
`pin-mochizuki.ps1` and the installers take as they are, upstream's `dlssnr-amd/experimental` being a new top folder for
them; and the weights and pipeline cache as `dlssnr-amd/aco-cache/dlssnr-small-experimental.nrb` and
`pipeline-experimental.nrb`, since an uninstall removes the `.nrb` files there. Upstream's installer makes the weights;
here the host does, at the first use (`MochizukiNrRuntime.cpp` `MakeExperimentalWeights`, 0.5 s, the small model with the
difference XORed in, kept when the SHA-256 is upstream's). Anything missing: the small model, and the log says why.
`config.arch` other than RDNA4 or no ACO: the same.

The host asks for it a frame at a time (`LMXXF_NR_FRAME_FLAG_EXPERIMENTAL`, part of the network key), like int4 mixed: a
change builds it while the frames keep the current one (1.0 s here). `MochizukiNrInfo::experimental` says it runs.

Harness (RX 9070 XT, AMD Software 26.10.1, ACO, 60 frames, 4 rounds, the runtime's own GPU time), default / this: 720p 3.14 /
2.78 ms, 1080p 4.97 / 4.40, 1440p 7.1 / 6.5, 4K 13.71 / 12.10. The picture 45.9-47.0 dB from the default network's
(pan of 8 px a frame at 1080p: 47.0 dB). All default paths byte-identical to the build before: RDNA4 small and 310.8
model, 720p and 1080p, driver and ACO, pan, int4 mixed; forced RDNA3 and RDNA2. The flag on the forced networks or without
ACO: byte-identical to it being off.

## CC_SILVER_AARDWOLD_OPTIMIZED (OptiScaler's `MochizukiInt4` + `MochizukiOptimizedLayers`)

Upstream's network of v0.0.8.3, int4 mixed's successor (see Sync to v0.0.8.3), ACO's machine code only, RX 9000.
The build (`tools/build-mochizuki-runtime.cmd`) writes `dlssnr-amd/aco/optimized/` (`shaders/` from `--optimized`,
`dlssnr-optimized.delta`, `dlssnr-optimized.sha256`, and `dlssnr-optimized-int4.sha256` = upstream's
`linux/data/int4/rdna4/dlssnr-int4.sha256`), `int4/` (Balanced: `--int4`, its tables and settings), `int4-performance/`
(`--performance`, Balanced's tables) and `int4-ultra/` (`--ultra`, Balanced's tables with int4-ultra's and its
settings); the int4 records (all three levels) in `aco/int4/records`.

The host asks for it with `LMXXF_NR_FRAME_FLAG_INT4` (OptiScaler's `MochizukiInt4`, so an old `MochizukiInt4=true`
gets it) at `MochizukiNrControls::optimized_layers` (OptiScaler's `MochizukiOptimizedLayers`, 0 Quality to 3 Ultra
Performance, default 3 as upstream's), both part of the network key. On the builder thread, before the build, it makes
`aco-cache/dlssnr-optimized.nrb` from `dlssnr.bin` and the difference (`MakeWeights`, the experimental network's code
with upstream's added entries) and, for a level with int4 layers, `aco-cache/dlssnr-optimized-int4.nrb` with
`nr::make_int4_weights` and `int4-ultra/`, kept when its SHA-256 is upstream's. Anything missing: the core runs
CC_SILVER_AARDWOLD, or the network at Quality, and logs why. `MochizukiNrInfo::optimized` is 1 + the level running.

## NR's place (OptiScaler's `MochizukiPlacement`)

Upstream's route for OptiScaler has a Placement a layer: `before` the upscaler (a copy of the colour it hands the upscaler),
`after` it (on its output), `present` (the back buffer at the game's Present, before frame generation; its default since
0.0.8). OptiScaler's key: `before` is the seam mochizuki always had and stays the default. `after` is the seam danielblnc
already took (`AmdBridge::After`, `WriteAmdPostAnswer`), now open to mochizuki: the vectors stay on the render grid
(`motion_width` the guides' extent; a uniform pan with the vectors on half the grid gives the same bytes as on the whole).
`present` is PresentExperimental's frame (`NR without upscaling`) in a game whose upscaler runs too, on optical flow, not
the game's vectors as upstream's, and SDR back buffers only. Without an upscaler that checkbox is upstream's Present.
Both only on a native D3D12 swapchain (`Backend::MochizukiPlacementNow`); OpenRR's NR on the denoised image stands down
for them. Nothing in the core.

## RenoDX (always on in OptiScaler)

Upstream's RenoDX stage (`windows/src/core/nr_renodx.*`, `renodx/codec_gain.hpp`; RenoDX DLSS5 add-on v8.5.0-rc10,
MIT, `renodx/LICENSE`) is compiled into the DLL as it is, with its shaders: `windows/shaders/renodx/` built with Slang
2026.19 (`RENODX_VULKAN`) and `windows/shaders/renodx-prep/` with glslang, into `dlssnr-amd/shaders/renodx/`
(`tools/build-mochizuki-renodx.py`, which also checks `prep_math.glsl` against `runtime_prep.comp` as upstream does).
No core patch: the stage only uses the core's `record_engine` with a target and a feature per pass, and, for a frame
without motion vectors, `record` in place on a copy of the proxy. The stage itself carries local patch 10.

The host side is TheAutomatic/dlss-5-amd-project's integration (GPL-3.0): with `MochizukiNrControls::renodx` the
network is built for the stage's extent (`working_resolution`), RGBA16F, one pass, `native_compose`, and
`ProductRuntime` holds the stage; each frame the stage encodes the frame, calls the network once per pass of its
stack (1-4, 1-30 unlocked; feature pass + 1, passes 5 and later with pass 4's controls), and composes back into the
frame's image. The game's exposure (`MochizukiNrSetExposure`) is RenoDX's feed, else its content meter. The game's
depth is not passed (it is at the frame's extent). OptiScaler's own composition (`RenoComposition`) stays for
danielblnc and lmxxf and is skipped with mochizuki. Its ~85 `MochizukiReno*` keys and defaults are theirs, RenoDX's
own. Since 2026-10-17 the stage is always on (`MochizukiNrControls::renodx` defaults to 1 and OptiScaler always sends
1), and the host's own model, pass, scale, strength, white point and enlargement keys are gone in favour of the
stage's; the runtime still runs the network on its own for a host that sends 0.

Harness, RX 9070 XT, 1080p RGBA16F, ACO, pan 8 px a frame, wall time a frame with a full wait (not GPU time): off
31.4 ms, RenoDX 1 / 2 / 6 passes 31.8 / 36.2 / 61.3 ms; the SDR path (R10G10B10A2, legacy codec) runs too.

## RDNA3 (RX 7000, no FP8)

The same network on gfx11, from mauri870/DLSSNR-RDNA3 (branch `rdna3`, a fork of upstream at `939b607`):

- Source: https://github.com/mauri870/DLSSNR-RDNA3
- Pinned commit: `aa392284` (2026-10-16)
- Licence: MIT, `LICENSE-rdna3` beside this file (the fork's own file; the notice is upstream's)

Every e4m3 value is held as FP16 and multiplied in the FP16 x FP16 -> FP32 16x16x16 WMMA, the e4m3 rounding done in
ALU (the fork's `README-RDNA3.md`). Its kernels assume the gfx11 fragment layout the fork measured on RADV: an
accumulator component `c` of lane `l` is element `(2c + l/16, l % 16)`, an A or B fragment holds sixteen components a
lane, and the WMMA computes row `m` from lane half `m % 2` alone (`kswap` below).

Taken: `linux/shaders/rdna3/` (without `pipelines-int4.json`) and `linux/build/arch/rdna3.sh`, as the fork has them
but for local patch 12; and the fork's host changes to `linux/src/core` (7c6361d, 767911f, eecb894, 612b0ba), ported
into `windows/src/core` as local patch 13:
- `nrvk.hpp`: the FP16 matrix configuration instead of FP8, no float8 features on `adopt`; an FP16 twin of the
  activation and weight arenas (`Context::fp16_twin`), bound by `Kernel::create` to every binding whose block holds
  e4m3 data (found by block name, `e4m3_view_bindings`); alias bindings `16 + k` get the arena itself (raw 32-bit
  views: tile counters, sync words, scales); the runner zeroes the activation twin with the arena.
- `nr_graph.cpp`: the twins allocated beside the arenas, the weight twin uploaded as FP16 decodes of the e4m3 bytes;
  the grouped FFN tiles packed in pairs but plain `[row][k]` (`plain_tiles`); the fused Swin weight tiles left plain,
  with the odd rows of the tiles that meet register-built operands k-pair swapped (`kswap` in
  `shader-constants.txt`, also accepted by `nr_shader_manifest.hpp`).
- `nr_runtime.cpp` `kernel_set`: a second descriptor set binds twins and aliases as `create` does.

Left out: the fork's int4 mixed for gfx11 (its pipelines, the `iu4` rewrite of `v_wmma_i32_16x16x16_iu4` through
RADV's pipeline binaries), temporal reuse (`runtime_reuse_*`, `Controls::reuse_every`), the edit gain below 100% model
scale in `runtime_transfer.comp` (32f3b1c, it changes the RDNA4 picture), `--value-stats`, `linux/test/`,
`linux/src/pe/`, packaging and docs. `nr_device_features.hpp` (the Vulkan layer's) is not in the DLL.

Local patches:

12. `linux/shaders/rdna3/pipelines.json`: `NR_EDGE_BODIES=0` on its seven persistent runs (`fswinp256`, `fswinpds*`,
    `fswinpup*`), as local patch 2 does for RDNA4. With the edge bodies LLPC (AMD Software 26.9.2) crashes on
    `fswinp256`.
13. One DLL for both networks: the fork's compile-time `NR_ARCH_RDNA3` is `RuntimeConfig::rdna3` and
    `Context::rdna3` (set before `adopt`, and on the arena probe's session). With it the core reads
    `dlssnr-amd/shaders-rdna3/` and `pipeline-rdna3.cache`, turns ACO off, and the graph build takes the Linux
    network's kernel set (`linux_network()`, as ACO's: tile counters on the ViT, decoder and encoder kernels,
    `gemmprojc`, `fswin32` as the frame tick, the N-pair attention weights). The Linux network's push blocks have no
    arena address, so `gemm*` and `vitattn` push the Windows block without it (`aco_push_gap`, as ACO int4 does).
    The twins are destroyed with the arenas (the fork's runtime never frees them). The defines of `rdna3.sh` are
    `rdna4.sh`'s and `NR_ARCH_RDNA3=1`, so the core is compiled once, with `rdna4.sh`'s. Without `rdna3` every new
    branch is off: the RDNA4 network's SPIR-V is byte-identical and so is its output (harness, 720p, 1080p, 4K).
14. The build: `windows/build/build_network.py rdna3 --tree linux --no-quad` (new `--tree`), the Windows build's
    preprocessing and unroll applied to `linux/shaders/rdna3`; LLPC crashes on the fork's rolled loops (most fswin,
    ffwd3, attn and temporal post pipelines, as it does on the Linux RDNA4 network). `unroll_glsl.py` drops
    `[[dont_unroll]]` (`NR_EXPAND_HLOOP`, `NR_EXPAND_KLOOP`, kept rolled for RADV's register count), on which LLPC
    crashes too; no RDNA4 source has one. Its `.length()` = 8 is right here: only accumulators ask. No four-wide
    quantisation (`quad_quant_glsl.py` is written for the FP8 pair loops). So the SPIR-V is not the fork's; built
    plainly (`linux/build/build_network.py rdna3`, the same script, glslang and defines as the fork's
    `build_network.py rdna3`) it would be, and that build crashes LLPC.
15. v0.0.7's shared network memory (`GraphAct`, `GraphArenas`, the resize path's `GraphStaging`): the twins live with
    the memory they shadow, not in the session. A `GraphAct` holds the activation twin (twice `cap`) and is freed
    with it, so a network that takes over another's arena (`RuntimeConfig::act_arena`) uses that arena's twin;
    `GraphArenas::wgt_twin` is the weight twin (twice `wgt_cap`), `GraphStaging::twin` the plan's tail as FP16.
    `retarget` zeroes the activation twin with the arena and copies the tail's twin with the tail; `build_shared`
    binds the slot's twins. The pipeline-sharing key (`nrvk::SharedPipeline`) also names the alias bindings. An RDNA3
    network is never offloaded (`download_network_weights` refuses it: the twins would have to go and come back).
16. The Style grade on RDNA3: `linux/shaders/rdna3/fswin_t.comp` takes v0.0.7's `NR_STYLE_GRADE` hook as upstream put
    it in `linux/shaders/rdna4/fswin_t.comp` (same lines), `include/nr_style_grade.glsl` is copied from rdna4 (plain
    f32 ALU), and `pipelines.json` has rdna4's `style_grade` table (`fswinimagepost32`, the three `temporal_post_fp32*`
    variants). The RDNA4 `_grade` SPIR-V cannot serve RDNA3: each is the whole post block, FP8 matrices included.
    The RDNA3 ones declare no Float8 capability. On the forced RX 9070 XT Natural runs and differs from Standard.
17. The ACO slot for RDNA3: `nr_pal_binary.hpp` names the code objects it takes (`takes_machine`: gfx1200 and
    gfx1201, as before) so the host can ask before it would take gfx11 records, and `nr_runtime.cpp` lets an RDNA3
    network run ACO only from a bundle named `aco-gfx11` (never RDNA4's records). No gfx11 records exist yet.
18. The stage dump (a diagnostic, both networks): `nr_runtime.cpp` `Impl::StageDump` and `windows/src/core/
    stage_stats.comp` (embedded by `tools\build-mochizuki-runtime.cmd`). With `MZ_STAGE_DUMP=<dir>`, in recording
    `MZ_STAGE_FRAME` (default 8) every activation value a later dispatch reads (by plan key, `voff.used`) gets 16
    chunk statistics (sum, sum |x|, sum x^2, max |x|, NaN/Inf, zeros, an order-free hash of the decoded values) right
    after the dispatch that made it, read as e4m3, f16 and f32 from the arena and on RDNA3 as f16 from the twin;
    `MZ_STAGE_FULL=<key>` also copies that value's raw bytes; the persistent runs' epoch and error words are copied
    at the end. `stages.txt` (dispatches, values, error words) and `stages.bin` are written at the next recording.
    It adds dispatches and barriers to that one recording only. Without `MZ_STAGE_DUMP` nothing is created or
    recorded: the output is byte-identical to the 0.5.8 DLL's (RX 9070 XT, harness, 12 sizes, both models, FP8 and
    ACO), and with it the frame is too. `tools\rx7000-diag\stagecmp.cpp` compares two dumps.
19. No persistent runs on RDNA3 (`nr_graph.cpp`, the merge of plain fused-Swin layers into `fswinp*` dispatches):
    an RDNA3 build runs every layer as its own dispatch, with the barrier between, unless `NR_RDNA3_PERSIST=1`, as
    it keeps the barriers the tile counters would drop. The persistent runs pass data between workgroups of one
    dispatch through flags. On the RX 7800 XT (driver 26.10.1) the RDNA3 network was wrong and changed from run to
    run at every size; the RX 9070 XT forced onto that network runs out of its waits with the 310.8 model at
    1600x900 and up (0.6 to 1.2 s a frame, error word set) and is byte-identical with and without them wherever
    they finish (small model 1152x648, 1280x720, 1920x1080). Forced RDNA3, 310.8 model: 1600x900 583 -> 14.8 ms,
    1920x1080 1188 -> 18.2 ms, run to run identical; small model 5.5 -> 5.8 ms at 1152x648, 6.9 -> 7.4 ms at 720p.
    The RDNA4 network is unchanged (byte-identical to 0.5.8, harness, 720p and 1080p, both models).
20. The kernel capture (a diagnostic, both networks): `nr_runtime.cpp` `Impl::KernelCapture`, `nrvk.hpp`
    `buffer_sizes()` (every buffer `Context::buffer` makes and its size) and `Kernel::spirv` / `bound_buffers` (what
    `create` bound). With `MZ_KCAP=<dir>` and `MZ_KCAP_DISP=<n>`, in recording `MZ_KCAP_FRAME` (default 3) every
    buffer network dispatch n binds is copied whole right before it (`MZ_KCAP_AFTER=1`: after it), with its SPIR-V
    name, grid, push constants as pushed and bindings (`capture.txt`, `buf<k>.bin`), so `tools\rx7000-diag3\kreplay`
    can run that one dispatch again. Without `MZ_KCAP` nothing is created or recorded.
21. The sequence check and the RDNA3 barrier choice (RX 7000 kit 4): `nr_runtime.cpp` `Impl::SeqDiag`,
    `Impl::sync_barrier`, `windows/src/core/seq_scrub.comp` (embedded by `toolsuild-mochizuki-runtime.cmd`), and in
    the host `MZ_ONE_QUEUE=1` (no second queue for the builds). With `MZ_SEQ=<dir>` the buffers the network writes are
    copied before dispatch `MZ_SEQ_FROM` (1) of recording `MZ_SEQ_FRAME` (3), what each dispatch ran (pipeline, set,
    push, grid) is kept, and at the next recording, the device idle, every dispatch i up to the one before the post
    block is replayed: dispatches from..i in one submit with the frame's own barriers, `MZ_SEQ_RUNS` (3) times, the
    values i makes (plan keys; arena and FP16 twin) read back and hashed; i alone in its own submit after
    `vkQueueWaitIdle`, on the bytes the from..i-1 prefix left, `MZ_SEQ_ISO` (3) times; and the prefix with the
    variants of `MZ_SEQ_VARIANTS` (full: srcAccess SHADER_WRITE; global: memory write -> memory read/write over all
    commands; twinbuf: global plus whole buffer barriers on the arena and its twin; submit: one submit per dispatch;
    scrub / zero: before each dispatch a dispatch that leaves its 64 KB of LDS and ~96 VGPRs a lane with a random
    pattern / zeros). `MZ_SEQ_REF=1` writes each value's every `MZ_SEQ_SAMPLE`-th 256-byte block (the FP8 network's,
    the reference), `MZ_SEQ_REFS=<dir>` measures against one. With persistent runs the weights are copied too
    (`MZ_SEQ_ALL=1` always); a persistent run's replay is not faithful (fswinpup256 changes between replays on the forced
    RX 9070 XT while the frame itself does not), so the kit replays the one-dispatch-a-layer network only.
    `<dir>/seq.txt`: a RESULT line per check (the first dispatch that changes, that differs from itself alone, per
    variant), then every dispatch. Nothing is created or recorded without `MZ_SEQ`.
    `NR_RDNA3_SYNC` = `released` | `full` | `global` | `twinbuf` (default) | `zero` picks the barrier between two
    RDNA3 network dispatches (the same as the variants above; `zero` the released barrier plus the zero scrub before
    every network dispatch). RDNA4 never reads it: byte-identical to the 0.5.8 DLL (harness, 40 frames, small and
    310.8 model, 1280x720 and 1920x1080, driver compiler and ACO). On the forced RX 9070 XT every mode gives the same
    picture; released / full / global / twinbuf cost the same (1152x648 5.7 ms, 1080p 11.0-11.4 ms), zero about
    twice that (10.0 / 21.0 ms).
22. The RDNA3 window attention as the RDNA2 tree's native body (RX 7000 kit 5): `linux/shaders/rdna3/include/
    attn_native.glsl` is `mochizuki-rdna2`'s (6bafaac91) with `NR_DOT2` and its round-to-nearest-even FP16 mode added,
    `attn.comp` takes it under `NR_NATIVE=1` as the RDNA2 tree does, and `pipelines.json` builds it as `attnnat` (the
    `attn` defines plus `NR_NATIVE=1`): no cooperative matrices, so no gfx11 fragment layout. `nr_graph.cpp`
    `create_kernels` loads `g_attnnat.spv` for the RDNA3 network's `attn` dispatches when it exists, unless
    `NR_RDNA3_ATTN=released`; `NR_RDNA3_ATTN=<file>.spv` loads that one (a diagnostic, same bindings and push block).
    The log says which: `[nr] RDNA3 window attention: ...`. Same bindings, push block and grid as `attn`. On the RX
    9070 XT, dispatch 11 alone on the FP8 network's own inputs (kreplay, 1152x648) gives key 74 99.35% equal to the
    FP8 network's (rel 0.0004), deterministic with any fill and scrub; the released `attn` is wrong there by design.
    RDNA4 never reads it: byte-identical to the 0.5.8 DLL (harness, 40 frames, small and 310.8 model, 1280x720 and
    1920x1080, driver compiler and ACO); `shaders` and `shaders-rdna3` byte-identical to 0.5.8's but for the new file.
23. RDNA3 cooperative-matrix memory access kept 16-byte aligned and gather-free (RX 7000 kit 6). Kit 5 on an RX 7900
    XTX: `attn` alone is right and deterministic with `NR_QPAD=0` (Q/K rows in LDS 64 bytes apart) and wrong, changing
    run to run, with the released 4 (72-byte rows, 8-byte aligned); the next dispatch that changes even alone is
    `vitattn`, which has no LDS fragments but is the only dispatched RDNA3 kernel with a ColumnMajor A load from a
    buffer (V^T, sixteen f16 a lane 32 bytes apart). Every coopMatLoad/Store of `shaders-rdna3` was audited in the
    SPIR-V: the 72-byte rows were the only stride that is not a multiple of 16 bytes (the runtime strides of
    gemmvqkv/gemmvqkvs are N, a multiple of 128). Changes, RDNA3 tree only: `attn.comp` `NR_QPAD` defaults to 0
    with an `#error` for rows that are not a multiple of 16 bytes (`attnnat` keeps 4, its SPIR-V unchanged);
    `vit_attn.comp` `NR_VT_STAGE` (on in `pipelines.json`): each lane reads one key's sixteen dims of a V tile with
    four 8-byte loads, stores them transposed to 1 KB of LDS a wave, and both V^T fragments load RowMajor from
    32-byte rows (`vit_attn_vt_chunk.glsl`); `include/vit_native.glsl` (the RDNA2 tree's, 22abb593d, NR_DOT2 and RTE
    added) as pipeline `vitattnnat`. `nr_graph.cpp` `create_kernels`: the shipped `g_attn.spv` / `g_vitattn.spv` by
    default; `NR_RDNA3_ATTN` / `NR_RDNA3_VITATTN` = `native` or `<file>.spv` are diagnostics. On the RX 9070 XT the
    staged V^T equals the ColumnMajor load bit for bit (probe, 100% equal to V^T); RDNA4 byte-identical to the 0.5.8
    DLL (harness, 40 frames, small and 310.8 model, 1280x720 and 1920x1080, driver compiler and ACO); `shaders` and
    `aco` byte-identical, `shaders-rdna3` differs only in g_attn, g_vitattn and the new g_vitattnnat.
24. The RDNA3 network as released after kit 6 (RX 7900 XTX rerun with the model): `attn` with `NR_QPAD=0` (patch 23)
    is right and deterministic; `vitattn` still changes run to run with the staged V^T (the V^T probe was right
    either way, so the gather was not it), and the full network is right at 1152x648 and 1280x720 only with the
    native body (54-56 / 53-56 dB, 0.5.8 23-25 dB; +0.6-0.8 ms). So `vitattn` in `pipelines.json` is the native
    body (`NR_NATIVE=1`, its SPIR-V the kit's `g_vitattnnat.spv`), `NR_VT_STAGE` is gone (`vit_attn_vt_chunk.glsl`
    as in 0.5.8), and the diagnostic pipelines `attnnat` / `vitattnnat` and `include/attn_native.glsl` are removed,
    since the startup pipeline check compiles every `.spv` in `shaders-rdna3`. `NR_RDNA3_ATTN` / `NR_RDNA3_VITATTN`
    keep only the `<file>.spv` form (diagnostic, logged when used). RDNA4 byte-identical to the 0.5.8 DLL.
28. ACO for the RDNA3 network (gfx11), based on TheAutomatic's work (TheAutomatic/DLSSNR-AMD branch
    codex/rdna3-aco-port, mochizuki PR #26, MIT): the gfx11 PAL importer idea (gfx11 code objects, the classic
    `s_waitcnt`, the address32 high word as the PC's high dword, the workgroup IDs following the user SGPR count,
    NRPAL002 records with the machine and the scratch size) re-done here, and gfx10.3 added:
    - `nr_pal_binary.hpp`: `is_gfx12` / `is_gfx11` / `is_gfx103`, `same_generation` (a gfx11 record goes into any gfx11
      container: ACO's code for gfx1100 differs from gfx1103's only in how many VGPRs it leaves free), NRPAL002 parse
      (scratch only on gfx11, LDS allocation at least the use), `prologue_gfx11` for gfx11 and gfx10.3 (encodings,
      m0 = 124/125, every system SGPR - workgroup IDs, workgroup info, scratch wave offset - moved from PAL's user count
      to the record's, upward highest first before the argument copies or downward lowest first after them), the
      scratch metadata. The gfx12 path is unchanged: every RDNA4 and int4 record's prologue is byte-identical to the
      0.5.8 adapter's (`tools/aco-records/test_pal_prologue.cpp`, which also runs 40000 random gfx11/gfx10.3 layouts
      through an SGPR interpreter of the prologue).
    - `nr_pipeline_binary.hpp`: with the `aco-gfx11` bundle a pipeline whose record is missing or does not import is
      compiled by the driver from its template in `shaders-rdna3` (logged `[nr] ACO gfx11 <name>: imported` /
      `not imported (<why>); the driver compiles its template`), never the Linux SPIR-V itself (LLPC crashes on its
      rolled loops). RDNA4's bundle keeps the old behaviour.
    - `nrvk.hpp`: `MZ_LOG_LAYOUT=1` logs each kernel's set layout and push range (records must be made with them).
    - Records: `windows/data/aco/gfx11/records` (the 0.5.9 fix's Linux SPIR-V, `tools/rx7000-diag7/build_spv.py fix`)
      and `records-coop` (0.5.8's attn and vitattn, `... coop`), 33 each, made for gfx1100 (navi31) by
      `tools/aco-records` (Mesa 25.3.6 RADV null winsys + three small patches, on Windows) with `layouts.txt`.
      Kernels the RDNA3 graph did not create in the layout runs, and the three temporal kernels with a combined image
      sampler before another binding (RADV and the AMD driver size it differently), have no record: the driver
      compiles them. Not shipped in the product build yet (RX 7000 kit 7 first).
    Our producer checked on the RX 9070 XT: its gfx1201 records of RDNA4's network are bit-identical in the output
    to upstream's for 44 of 55 kernels one at a time, 3 more at 55-56 dB (another Mesa), and the rest are explained
    (push block gap that RDNA4's records remap and the RDNA3 network removes; image descriptors).
29. ACO's gfx10.3 code on the RX 6000 driver (RX 6000 ACO probe 2, `tools/aco-records/probe2/rx6600-result`): that
    driver branch compiles with AMD's proprietary compiler, whose code objects carry feature bits in e_flags (0x938 on
    an RX 6600) and metadata of their own (registers keyed by number), and it imports an LLPC-style container with
    our record bit-exact. `nr_pal_binary.hpp`: `elf_machine` (gfx10.3 feature bits masked; gfx11/gfx12 exact as
    before), MessagePack maps with non-string keys (kept and written back), `splice_elf` (splice's body) and
    `synthesize` (the gfx10.3 container built from scratch: the RX 9070 XT's sections and metadata keys, this GPU's
    machine, the record's settings; no IEEE mode, scratch still refused). `nr_pipeline_binary.hpp`: a gfx10.3 record
    is never spliced into a captured template: `device_code` compiles an empty kernel once a device for the GPU's
    machine and key size, and the container is synthesized (no template compile); `aco-gfx103x` bundles fall back
    per pipeline like `aco-gfx11` (log `[nr] ACO gfx103x <name>: ...`). gfx12 and gfx11 splices and prologues are
    byte-identical to the patch-28 adapter (`test_pal_prologue.cpp`, `--probe2`).
30. ACO for the RDNA2 network (branch rdna2-aco): with ACO asked for, the host (`MochizukiNrRuntime.cpp` Rdna2Aco)
    takes `dlssnr-amd/aco-gfx103x` (`shaders/` = shaders-rdna2's SPIR-V as it is, `records/` one gfx1030 record per
    kernel, `tools/aco-records/produce-rdna2.ps1`) when both folders are there, with shaders-rdna2 as the templates
    of the per-pipeline fallback, and skips the startup pipeline check as the RDNA3 ACO network does; `nr_runtime.cpp`
    lets the RDNA2 network run that bundle only. Without the bundle the RDNA2 network is compiled by the driver as
    before. Not shipped in the product build yet. Records for the rdna2-mega shaders-rdna2 (`produce-rdna2.ps1`, Mesa
    25.3.6, navi21): `windows/data/aco/gfx103x/records`, 33 of the 49 network kernels (0 scratch), with
    `layouts.txt`, `produce-log.txt` and `summary.txt`; the temporal pre/post kernels the network uses
    (`temporal_pre_fp32nh`, `temporal_post_fp32h16`, `_grade`) have a sampled image before another binding and stay
    with the driver. The bundle is `dlssnr-amd\aco-gfx103x\shaders` = shaders-rdna2 as built + those records.
    `ConfigureAco` no longer needs RDNA4's `aco\`: without it the FP16 networks' own bundle is set up, and an RDNA4 device
    then runs without ACO.

The host (`MochizukiNrRuntime.cpp`) takes the RDNA3 network on an AMD device (as AMD's driver reports it, through
OptiScaler's GPU spoofing) without VK_EXT_shader_float8 or its features, when the FP16 configuration is reported;
VK_EXT_shader_float8 and its features are then neither required nor enabled. A device 0x75xx (RX 9000) without FP8 is
refused as before. A device with FP8 is preferred over one without on the same adapter. `MZ_FORCE_RDNA3=1` takes the
RDNA3 network on any card. ACO and int4 mixed are off with it. The prewarm uses `shaders-rdna3`,
`pipeline-rdna3.cache` and `prewarm\manifest-rdna3.txt`. The log says which network and why:
`[mochizuki] the RDNA3 network, on FP16 matrices: <reason>` or `[mochizuki] the FP8 network (VK_EXT_shader_float8)`.
The VRAM check adds the twins (336 MB and 384 bytes a model pixel).

Before the RDNA3 network runs, the host checks the device (`OptiScaler/dlssnr/backend/mochizuki_runtime/mz_rdna3.cpp`),
and either check failing keeps NR off with the reason (`[mochizuki] RDNA3 network off: ...`, shown on the menu's
mochizuki page). Both verdicts are kept in `dlssnr-amd\rdna3-check.txt` by device id and driver version (the
pipelines' also by the names and sizes of the shader files), so each runs once a driver:
- the layout probes of the RX 7000 kit (`rdna3_probe/*.comp`, embedded as SPIR-V): accumulator f32 and f16 maps, A and
  B with 16 components a lane, the lane-half rule and the k-pair swap, as tiny dispatches on the network's own device
  at the session's start (124 ms on the RX 9070 XT). A failure is the adapter's `[unsupported]` reason;
- every pipeline of `shaders-rdna3` and `shaders\renodx` (99) compiled in a child process (`rundll32` on the DLL's
  `MochizukiNrPipelineCheck` entry) on the builder thread before its locks, at below-normal priority on up to 8
  threads, 300 s without progress counting as a hang. A crash with several pipelines in flight runs each of them
  again alone to name it. The child's AMD driver cache is its own (per executable), so it warms nothing for the game:
  on the RX 9070 XT the check took 12.3 s cold, then the network's own cold build 15.7 s.
`MZ_RDNA3_CHECK=0` skips both, `=pipelines` the probes only (with `MZ_FORCE_RDNA3=1` on an RX 9000, whose layout
fails the probes); the hooks `MZ_TEST_RDNA3_COMPILE_CRASH=<file>`, `MZ_TEST_RDNA3_COMPILE_HANG=<file>` and
`MZ_TEST_RDNA3_TIMEOUT_MS` test the child's paths.

The first start without the second compile (local patch 32, RDNA3 and RDNA2): AMD's driver keeps what it compiles for
the executable that compiled it, its own cache and a VkPipelineCache the child writes alike (a cache file written by
one process and loaded by another under another name: every pipeline compiled again), so the check's work never
reached the game, which then built the network serially. Now, with a prewarm list for the network's folder
(`prewarm\manifest-rdna3.txt`, `manifest-rdna2.txt`, made and shipped as RDNA4's `manifest.txt`), the child compiles each
file the list names with the core's own create infos (the same device features, extensions and application name as
`Vulkan::Create`, plus VK_KHR_pipeline_binary) and keeps each pipeline's machine code (`mzi::ManifestPipelines`,
`work\binaries\`, NRAC files named by the hash of the list's line, `identity.txt` = device, driver, cache UUID). After a
passed check `mzi::AdoptBinaries` moves them to `dlssnr-amd\binaries-<net>\`; the network's device enables
VK_KHR_maintenance5 + VK_KHR_pipeline_binary (FP16 networks only), the build's Capture imports each core pipeline whose
binary is there (`mzi_vkCreateComputePipelines`, the driver compiles any other, and any import that fails), and the
prewarm leaves those out. Files the list does not name keep the check's reflected layout. `MZ_CHECK_CACHE=0`: as
before. Same machine code, so the same output (forced RDNA3 and RDNA2 hashes unchanged). RX 9070 XT, small 720p, cold
driver (shaders with a SPIR-V generator word of their own), time to the first frame: RDNA2 62.3 s (check 17.5 s,
36 pipelines built serially in 40.9 s) -> 27.4 s with the list alone (prewarm 10.5 s on 8 threads) -> 22.7 s now
(check 18.2 s, every core pipeline imported); RDNA3 52.6 s -> 26.4 s -> 26.6 s (check 20.5 s against 16.6 s: the
exact create infos and the capture cost what the prewarm saved here; the second start is 1.4 s); RDNA4 7.5 s
(its list, unchanged).

ACO for the RDNA3 network: with ACO asked for, the host looks for `dlssnr-amd\aco-gfx11\` laid out as `aco\`
(`records\` named by the hash of the SPIR-V in `shaders\`, which `linux/build/build_network.py rdna3` makes, and an
optional `shell\` and `shell-aliases.txt`) and takes it when the PAL adapter takes the GPU's code objects (gfx1100
0x41, gfx1101 0x46, gfx1102 0x47 by device id); otherwise `[mochizuki] ACO off for the RDNA3 network: no gfx11 ACO
records ...` or `... the PAL adapter does not take this GPU's code objects`, and the driver compiles
`shaders-rdna3`. Since local patch 28 the adapter takes gfx11 (and gfx10.3) code objects and a pipeline without a
working record falls back to the driver alone; the records are in `windows/data/aco/gfx11/`.

Local patch 34 (2026-11-16, RX 7000 speed kit on an RX 7900 XTX and RX 7800 XT): the build ships `aco-gfx11` with
only the 8 records ACO won on both cards (`tools/build-mochizuki-aco-gfx11.py`: fswin32nh, fswinfusedup64, gemmproj,
gemmprojc, gemmprojt, gemmvact, gemmvqkvs, and vitattn as ACO's compile of the matrix body; `windows/data/aco/gfx11/
shaders` keeps the three whose ACO SPIR-V is not shaders-rdna3's). With that bundle (`nr::binary::gfx11_slim`: no
`shaders\g_attn.spv`) the network is shaders-rdna3's, those kernels taken from the bundle's `shaders\`, within 240-960
ViT tokens (1152x648-2560x1440, `NR_RDNA3_ACO_MIN`/`_MAX`); outside that range every kernel is the driver's
(`nr::binary::driver_only`). The driver's matrix vitattn (`g_vitattncm`) runs from 448 tokens (1706x960) up: wrong at
240/288 on both cards, right at 448-2160. Log: `[nr] RDNA3 ACO: N ViT tokens, ...` and `[nr] RDNA3 vitattn:
ACO-cooperative-matrix|cooperative-matrix|native body`.

On an RX 9070 XT with `MZ_FORCE_RDNA3=1` (driver 26.9.2) every pipeline compiles, the network builds (16 s cold at
1080p) and runs at 720p, 1080p and 4K without errors; the picture is wrong, as gfx12's fragment layout is not gfx11's.
Nothing has run on an RX 7000 yet. After the v0.0.7 merge (2026-10-19): the RDNA4 network's SPIR-V is byte-identical
to the main branch's build and so is its output (harness, 1080p, both models, Standard and Natural); forced RDNA3
builds in 16.2 s cold at 1080p and runs both models.

Upstream's own RDNA3 network (0.0.8.1, TheAutomatic's #26; RX 7000 and Radeon 700M / 800M / 8000S): the same fork's
kernels moved to `windows/shaders/rdna3`, built plainly with glslang (`build_aco_network.py`) and run only as ACO's
gfx11 machine code (`windows/data/aco/rdna3`, 62 records, templates in `shells/`); without ACO's code RDNA3 has no NR
there. Not taken; ours stays the default (2026-11-03):
- its `attn` keeps `NR_QPAD 4` (72-byte rows) and its `vitattn` the cooperative-matrix body with the ColumnMajor V^T
  load: the pair that was wrong and changed run to run on an RX 7900 XTX here (patches 23 and 24). Under ACO the
  LLPC side of it may not exist, but nothing here can run gfx11 code to check it;
- it keeps the edge bodies and persistent-run pipelines (off by default in its host, as patch 19 does here);
- it needs a driver that takes pipeline binaries; ours compiles with the driver and was right on an RX 7900 XTX,
  RX 7800 XT and Radeon 8060S;
- its records are named by the hash of its SPIR-V, so they cannot serve our `shaders-rdna3` (other sources and the
  LLPC unroll). The PAL adapter now takes gfx11 code objects (patch 17), so an opt-in is a matter of shipping its
  folder as `aco-gfx11` with its SPIR-V and shells and checking the push layout (`aco_push_gap`) on an RX 7000
  first.

## RDNA2 (RX 6000, no FP8, no matrices)

Local work (OpenNR, `RDNA2-MOCHIZUKI-PORT.md`), not from upstream or a fork. gfx103x has neither FP8 nor WMMA, so
`linux/shaders/rdna2/` is a copy of `linux/shaders/rdna3/` whose `include/coopmm.glsl` implements the same macro
interface without cooperative matrices (layer A of the plan):

- a 16x16 fragment is a plain array in each lane of the 32-lane subgroup, in the layout the RDNA3 kernels already
  assume (A: component c of lane l is (l % 16, c); B: (c, l % 16); accumulator: (2c + l / 16, l % 16)), with row m of
  a product formed from lane half m % 2, as on gfx11, so `kswap` and the kernels' register-built operands hold;
- `NR_MMA` takes the A rows a lane needs from its half by `subgroupShuffle` (packed FP16 pairs) and sums FP16 pair
  products into FP32 (`NR_DOT2`: `fma(float(a.x), float(b.x), fma(float(a.y), float(b.y), s))`, which the AMD driver
  turns into `v_dot2_f32_f16` on gfx103x and gfx1201: phase 0, `wt-rdna2/handoff/PHASE0-probe.md`);
- loads and stores are per-element loops (every site loads and stores its own element type; checked at build time
  once with decoy overloads), with a `subgroupMemoryBarrier()` after each store and before each load, as the
  collective operation ordered them (a wave stores an accumulator to LDS and reads it back as an operand);
  `NR_NARROW_ACC`/`NR_WIDEN_ACC` are element-wise; no int4;
- every RDNA2 kernel declares RoundingModeRTE for 16-bit (gfx103x narrows toward zero otherwise).

Changes to the other RDNA2 shaders, all because an array has no one-argument constructor: the fragment fills
`NR_FRAG_X(0.0)` became `NR_FILL_X(0.0)` and the conversions `NR_FRAG_ACC(h)` became `NR_WIDEN_ACC(h)` (13 sites in
`fswin_t.comp`, `vit_attn.comp`, `fswin_body.glsl`, `fswin_hw_attn.glsl`, `vit_attn_vt_chunk.glsl`), and
`noise_field.comp` (no `coopmm.glsl`) declares RoundingModeRTE itself. `pipelines.json` is RDNA3's (no int4) without
the persistent runs' pipelines (`fswinp*`, `fswinpds*`, `fswinpup*`: the RDNA2 host builds none, below) and the
unfused downsample (`fswinds*`: the host always fuses it into `fswindsp*`, and an RDNA2 build with `--ds-tok-raster`,
the one switch that would keep it, throws); the markers
are the same, `kswap 1` included. Built by `windows/build/build_network.py rdna2 --tree linux --no-quad` into
`dlssnr-amd/shaders-rdna2` (`tools/build-mochizuki-runtime.cmd`).

Native kernels (layer B): every fused Swin pipeline (`fswin*`, `fswinfusedup*`, `fswindsp*`, the pre and post blocks
and their temporal variants) has `NR_NATIVE=1` in `pipelines.json`, which makes `fswin_t.comp` take
`include/fswin_native.glsl` instead of `fswin_body.glsl`: one lane per token, weights as uniform `uvec4` reads (scalar
loads into `v_dot2_f32_f16`), no cross-lane traffic in the projections, the window attention through LDS. Same
bindings, push block, weights and rounding; the network output is byte-identical to the emulated body's. The post
block's pixel composition moved to `include/fswin_imgout.glsl`, which both bodies include (the emulated SPIR-V is
unchanged). Removing `NR_NATIVE=1` from a pipeline gives back its emulated kernel.

The other hot pipelines have native bodies the same way, each in its own include with its own `main`, byte-identical
to the emulated kernel: `gemm1x1.comp` -> `include/gemm_native.glsl` (every `gemm*` pipeline of the RDNA2 set:
`gemmproj`, `gemmprojc`, `gemmprojt`, `gemmprojw`, `gemmpool`, `gemmnores`, `gemmvact`, `gemmvproj`, `gemmvnores`,
`gemmvqkv`, `gemmvqkvs`, `gemmvqkvnorm`, `gemmvqkvnorms`; a wave owns `NR_NG_TW` tokens read as uniform `uvec4` rows
and 32 x `NR_NG_CL` channels, the workgroup `NR_NG_WAVES` waves over the emulated tile; N-pair or tile-blocked
weights; the tiled e4m3 epilogue with the `NR_OUT_PERM` raster copies and the 2x2 pool, or the plain FP16 one;
`gemmvproj`, `gemmvnores` and `gemmvqkv` are never dispatched by this host but the pipeline check compiles them, and
emulated each took 22-45 s on an RX 6600), `attn.comp` -> `include/attn_native.glsl` (the C=512 window
attention: the QKV projection with scalar weights, then one query a lane against K and V broadcast from LDS),
`ffwd3_t.comp` -> `include/ffwd_native.glsl` (`ffwd3`, `ffwd3w`: one lane a token, the group's matrices as scalar
weights, the hidden units streamed) and `vit_attn.comp` -> `include/vit_native.glsl` (one lane a query, each key's K and
V rows as scalar reads). The GEMM and ViT attention bodies read activations through the scalar cache, so the RDNA2
host never runs tile counters (`tchain_active()`, even with `NR_TCHAIN=1`).

The pre and post blocks (`fswinimagepreds32*`, `fswinimagepost32*` and their `temporal_*` variants) are C=32 Swin
blocks at the full working resolution, four times the tokens of a `fswin32nh`, which is why they were a quarter of the
RDNA2 frame. In `fswin_native.glsl` they now (1) compute the 16 image features of the lane's own pixel
(`nr_pixel_features`, split out of `image_input.glsl`'s `nr_prepare_features`) instead of staging the 8x8 group in a
2 KB LDS buffer behind a barrier (LDS 10 -> 8 KB, the occupancy of the other C=32 blocks), (2) keep the 64 attention
probabilities of pass 1 in registers for pass 2 (`NV_PCACHE`) instead of computing the logits and exponents again,
and (3) read the post block's blend gains as one dword a pair (scalar loads instead of 64 16-bit vector loads). Same
arithmetic and order: the output is byte-identical. The other Swin pipelines' SPIR-V is unchanged.

`NV_PCACHE` is now the default of every C=32 body (`fswin32`, `fswin32nh`, `fswindsp32*`, `fswinfusedup32*`), not only
the pre and post blocks (gfx1030 .342: 116-122 VGPRs, 8 waves a SIMD as before, 0 scratch). The C>=64 bodies keep
their two attention calls (`NV_PCACHE=0`): with the cache, one rolled call fits gfx1030 (188-192 VGPRs, still 4 waves
a SIMD, the LDS limit) but spills 8 KB on gfx1201, where the forced runs are timed; `NV_PCACHE=1` builds that form
(the RX 6000 kit times it on the card as a variant). `attn_native.glsl` keeps Q and K in FP16 row pairs and puts the
quantised q and k over them once each lane has read its rows (one barrier more): LDS 20 -> 12 KB, gfx1030 3 -> 5
waves a SIMD. The C=32 fused upsample and the wide one read their blend gains as one dword a pair. Same arithmetic
and order: the output is byte-identical.

`NV_PCACHE` is now the default of every native Swin body, C>=64 included: an RX 6600 (driver 26.8.1) ran the C>=64
cached form 2-5% faster with the same picture, against 2-6x slower on gfx1201 (it spills there; the RDNA2 set never
runs on gfx1201 outside the forced tests). `NV_PCROLL` says how pass 1 fills the cache: 0 unrolled (four copies of the
logits; the RX 6600's compiler took 5.2-6.8 s for each such body, against 2.9-3.4 s before the cache), 1 rolled (the
default: one copy, each block shifted in at the end of a 32-entry queue so the indices stay constant; gfx1030 .342
143-148 VGPRs for C=32, 7 waves a SIMD instead of 8, 228 for C>=64, LDS-bound at 4 either way), 2 two blocks an
iteration. Same values and order in all three: byte-identical output. `build_network.py` now passes `--define` to the
temporal variants too (it did to the other network pipelines only). The RX 6000 kit compiles the three cold on the
card and times them, to confirm the default.

Host (local patch 31): `bool rdna3` became `nr::Arch` (`nr_arch.hpp`: rdna4, rdna3, rdna2) in `RuntimeConfig` and
`nrvk::Context`; `Context::fp16()` is what `rdna3` meant everywhere (twins, Linux kernel set, `kswap`, NR_TCHAIN off).
RDNA2 reads `shaders-rdna2` and `pipeline-rdna2.cache`, runs no ACO, and `adopt` requires neither the cooperative
matrix feature nor a matrix configuration, and builds no persistent runs (`nr_graph.cpp`, as `NR_NO_PERSIST=1`): their
waits are poll budgets sized for RDNA4's kernels, the emulated ones are tens of times slower, and the waits ran out
(the 310.8 model on the forced RX 9070 XT: 720p changed run to run, 1080p 24 dB and 1.2 s in one C=128 run). RDNA4
and RDNA3 take the same branches as before: their output is byte-identical to 0.5.8's (harness, 720p and 1080p,
both models; RDNA3 forced). `NR_STEP_TIMES=n` (diagnostic, any network) logs every network dispatch's time over n
frames, by pipeline.

`MochizukiNrRuntime.cpp` takes the RDNA2 network for an RDNA2 device id (Navi 21/22/23 0x73A0-0x73FF, Navi 24
0x7420-0x743F, the APUs 0x163F, 0x1681, 0x164E, 0x1506) whatever the driver reports (its branch reports and emulates
the FP16 matrices itself: phase 0), and for any other AMD device with neither FP8 nor those matrices;
`MZ_FORCE_RDNA2=1` takes it on any card. It needs subgroup shuffles and `shaderRoundingModeRTEFloat16`, enables no
FP8 and no matrix extension, keeps the pipeline check (`rdna2-check.txt`, `MZ_RDNA2_CHECK=0` skips it; 30 min
without progress before a compile counts as hung, its kernels take minutes each) and has no layout probes. The log says `[mochizuki] the RDNA2 network, its matrices emulated in ALU ...`; the menu's mochizuki
page `RX 6000 (experimental, FP16 network)`, or `RX 6000 network off: ...`. `MochizukiNrInfo::network` is 3.

On v0.0.8.2 (branch rdna2-mega, 2026-11-03): the RDNA2 tree needed no shader change. Its pre block still reads the
noise table from the weight blob, so binding 5 gets the 16-byte stand-in; the dead-slot and arena changes apply to it
as they do to RDNA3 (it shares RDNA3's graph). `history_f16` (MochizukiHistoryPrecision 16, the default) gives it the
RGBA16F history and `temporal_post_fp32h16` like the other networks. Forced on the RX 9070 XT: with
`NR_HIST16=0` the output is byte-identical to rdna2-aco's (small and 310.8, 1152x648 and 720p, 20 frames), with the
16-bit default 55.0-55.9 dB from it, the clean one-frame hashes of the RX 6000 kit unchanged at 720p, 1080p and
1440p for both models. The shaders-rdna2 network files are rdna2-aco's byte for byte; `runtime\` gained 0.0.8.2's
`runtime_alpha` and `runtime_blit` as `shaders-rdna3\` did. ACO gfx10.3 records for this SPIR-V:
`windows/data/aco/gfx103x/` (below).

The RDNA2 techniques (FP16 dot products with FP32 sums, emulated e4m3 rounding) were inspired by the public README and
CHANGELOG of a GPL-3.0 RDNA2 port of the DLSS network; none of its code was read or used (the plan's clean-room rules).

## Build

`tools\build-mochizuki-runtime.cmd [out]` from an MSVC developer prompt, with the Vulkan SDK 1.4.357 or newer
(`VULKAN_SDK`) and Python. It downloads glslang 16.5.0, the version the upstream SPIR-V is pinned to, and Slang
2026.19 (SHA-256 checked, for the RenoDX shaders) into `toolchain\` here, and writes `MochizukiNrRuntime.dll` and
`dlssnr-amd\shaders\` (with `renodx\`), `dlssnr-amd\shaders-rdna3\` (53 network, 11 temporal, 16 runtime) and
`shaders-rdna2\` (40 network, 11 temporal, 16 runtime) to `out` (default `exports\mochizuki-runtime`).

The shader folder has 55 network, 6 temporal and 16 runtime pipelines (v0.0.8.2 and the local passes;
`runtime\FidelityFX-FSR1-LICENSE.txt` beside the FSR 1 EASU in `runtime_upscale`) and 30 RenoDX files, 113 files. The prewarm list
(`dlssnr-amd\prewarm\manifest.txt`) must be made again whenever the shaders change: with ACO off (it is skipped with
ACO on), run the runtime once at 720p, 1080p, 1440p and 3840x2160 and once at 1080p with the 310.8 model, from an
empty `prewarm\`, and take the list it writes: 38 pipelines for v0.0.7 and for v0.0.8.2
(`exports/mochi0082-verify/prewarm.ps1` in the sync's worktree). The 0.5.5 list had 33: it lacked `ffwd3w`,
`gemmprojw` and `gemmvqkvnorm` (the larger extents and the 310.8 model) besides the two new Style grade pipelines.
The FP16 networks have lists of their own, which their first start needs (local patch 32): `prewarm\manifest-rdna2.txt`
and `manifest-rdna3.txt`, made the same way with the network forced (`MZ_FORCE_RDNA2=1` / `MZ_FORCE_RDNA3=1`, the check
skipped): small at 1152x648, 720p, 1080p, 1440p and 4K, Natural and Cinematic at 1080p, the 310.8 model at 720p and
1080p; 41 pipelines each for this build (`exports\verify\manifests.ps1` in the rdna2-mega worktree). Remake them
whenever shaders-rdna2 or shaders-rdna3 change.

## The model

Never shipped with the code. `dlssnr-amd\dlssnr.bin` beside the DLL is made from `nvngx_dlssnr.dll` 310.8.0
(SHA-256 `e16bcf15…6e1fc8e`) by the scripts in `linux/package/model-tools/`: `inspect_nr.py --extract`, the
five `unpack_*.py`, then `pack_model.py --verify model-files.sha256`, as `extract_model.sh` runs them. The result
is 599 entries, 147,756,560 bytes.

`nvngx_dlssnr.dll` 310.9.16 (SHA-256 `2bc19bc0…40e28789f`) holds that model (its preset 1, byte-identical
`dlssnr.bin`) and a new one, its default: `dlssnr-amd\dlssnr-small.bin`, 255 entries, 113,917,248 bytes (SHA-256
`7e966e26…10043c6e33`). Upstream's `windows/package/model-tools/dlssnr_extract_model.cpp` makes both in two seconds
(`cl /std:c++17 /O2 /EHsc`, standard library only; every entry checked against the tested model). Same shaders, a
shorter plan (`nr_native_plan_data_small.hpp`, its ViT blocks are 11..18), 1080 rows padded to 1152 as NVIDIA's DLL
does. The host asks for it with `RuntimeConfig::network = 1` unless the frame carries
`LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL` (OptiScaler's `MochizukiOriginalModel`, or int4 mixed, which has the 310.8 model
only); without `dlssnr-small.bin` the core runs the 310.8 model and logs why. Harness, RX 9070 XT, ACO, one pass:
720p 5.65 -> 3.39 ms, 1080p 9.78 -> 6.29, 1440p 16.9 -> 10.6, 4K 36.3 -> 23.0, three passes at 1080p 27.9 -> 18.2;
36 dB from the 310.8 model's picture; the 310.8 model byte-identical to the build before.

## Updating

Copy the paths above from a newer upstream commit, reapply the patches, rebuild, and run the runtime against a
D3D12 frame before shipping it.
