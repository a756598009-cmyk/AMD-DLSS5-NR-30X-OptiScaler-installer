# Handoff: updates from mochizuki0323/DLSSNR-AMD (2026-10-01)

Upstream gained 7 commits after 4f663b5 (v0.0.2.1, 2026-09-27), up to 51b65a6 (2026-10-01). This fork uses its
`windows/`, with 15 patches of ours on top (`third_party/mochizuki/UPSTREAM.md`). The patches do not apply
directly: what was taken was ported by hand, and is in entry 16 of `UPSTREAM.md`.

## Done in this version (not committed, not tested in game)

1. **History weight matching NVIDIA's (d1185d2).** The post block blends the previous frame with the weight the
   network emits times the model's `blend_scale`, 0.7397. We used 1.0, so we carried more of the previous frame
   than the original. `kPostBlendScale` in `nr_runtime.cpp`. Changes the image in motion.
2. **Colour formats (743326d).** The host accepts R9G9B9E5_SHAREDEXP (RE Engine: Resident Evil Requiem, Monster
   Hunter Wilds), B8G8R8X8 (UNORM, SRGB, TYPELESS), R16G16B16A16 UNORM and SNORM, R8G8B8A8_SNORM and the packed
   16-bit ones (B5G6R5, B5G5R5A1, B4G4R4A4). Those that only go through a blit are only accepted where the GPU can blit them
   (`NeedsBlitCheck` + `Session::Blittable`). Before, a game in these formats got no NR.
3. **Preprocess (729a05d).** Exposure (auto by histogram, or fixed), curve, contrast and saturation before the network,
   undone on the answer.
   - ABI: new fields at the end of `MochizukiNrControls` and `MochizukiNrInfo::preprocess_ev`.
   - `MochizukiPreprocess*` keys in `[DlssNr]`.
   - mochizuki menu, "Preprocess" drawer; the measured exposure shows in Info.
   - Off by default. The first time it is turned on, the network is rebuilt once already able to run Preprocess
     (`Session::prepWanted`).
   - New `runtime_prep.spv` shader in `dlssnr-amd/shaders/runtime/`: the installer's mochizuki component has to
     carry it, and the prewarm manifest must be remade.
   - The ReShade add-on does not expose Preprocess (it sends the short struct, the runtime uses the defaults).

Validated: runtime and OptiScaler build, host contract tests. **Missing:** running the runtime on a GPU
(EQUIV against the goldens: a frame without history should come out the same with Preprocess off; with history it changes
because of item 1) and testing in game. The harnesses (`mz_bench`, `mz_phases`, `mz_pan`, `mz_timing`) are no longer in
`exports/` and need to be redone or recovered.

## Done on 2026-10-02 (not committed, not tested in game)

A and B below are done, in a simpler way than suggested: upstream's `windows/` (82560c4) already had
both, so it came in whole and only two patches of ours stayed on top (the build's stop flag and `NR_EDGE_BODIES=0`,
which takes the cold build from 109 s to 23 s without changing the output). Upstream covers the rest of the 16 old patches;
the list is in `third_party/mochizuki/UPSTREAM.md`. The host gained `bufferDeviceAddress`, the `SAMPLED` images
(read in place) and `preprocess_unknee`; `frame_image` is gone.

Measured in a new harness (`mz_e2e` in the session scratchpad: real image, pan with exact vectors, same
interface as lmxxf): 1080p 9.26 -> 8.50 ms, 4K 55.6 -> 48.1 ms, 3 passes 42.3 -> 38.3 ms; at 1707x961 and 720p
the output in motion became ~10x steadier. 2 and 3 passes, R10G10B10A2, RGBA32F, odd size, 4K and
Preprocess run without errors. New prewarm list in `exports/mochizuki-runtime/dlssnr-amd/prewarm/` (38 lines,
65 shaders; cold 6.1 s with it). To release: `pin-mochizuki.ps1` with this build (shaders changed: removed
`runtime_encode_in.spv`, added `runtime_depth.spv`, `noise_field` and others) and an in-game test.

## Was for the next version (done above)

### A. Motion like NVIDIA's DLL (rest of d1185d2)

- The pre and post blocks read the game's motion vectors at full resolution, in place, instead of
  a 1/4-resolution copy that mixed foreground motion with background motion on every silhouette.
  When the texture cannot be sampled with linear filtering, the old copy remains.
- Motion and depth subrects that do not start at (0, 0) are respected (`runtime_depth.comp` receives the
  offset). `EngineFrame` gains `motion_texture_width/height` and `motion_x/y`, `depth_x/y`.
- Pre block noise: the seed is the number of frames since the first or the last reset, as in the DLL; the frame
  with seed 0 uses the precomputed field, the others compute it in the shader (`noise_field.comp`, `image_noise.glsl`).
- What he fixed in the vectors' scale (normalizing by the motion region) our host already does correctly
  (`MochizukiNrRuntime.cpp`, `next.motionScaleX = info->motion_scale_x / float(motionW)`).
- His measurement, moving sequences against NVIDIA's DLL on an RTX 5090: +2 to +5 dB of PSNR over
  0.0.2.4, cost +0.005 ms at 1080p.
- Files: `windows/src/core/nr_runtime.cpp/.hpp`, `windows/shaders/rdna4/fswin_t.comp`,
  `include/image_input.glsl`, `passes/runtime_depth.comp`. Touches the same files as our patches 12 to 15,
  so it is a hand port, with new goldens.

### B. Network rebuilt for the Windows AMD compiler (228d3a6 + b1419b0)

- His numbers, offline on an RX 9070 XT:

  | Resolution | Upstream Windows | Ours (`nr_graph --per-layer`) |
  |---|---|---|
  | 1080p | 7.21 ms | ~8.5 ms |
  | 1440p | 12.59 ms | ~14.7 ms |
  | 4K | 27.32 ms | ~33.6 ms |

- He says the Windows version was not tested in games, and that the image differs slightly from Linux.
- Includes: barriers replaced by per-tile counters, C=512 projections with their own pipeline, fold of the
  C=256 downsample/upsample in every extension, `fswin_t.comp` split into `include/fswin_body.glsl`,
  `quad_quant_glsl.py`, `coopmm.glsl` and `pipelines.json` redone, f32->f16->f32 round trips that the AMD compiler
  discarded (`NR_Q32_DIRECT`, `NR_Q32_STAGE`), `HostDevice::buffer_device_address` and network builds one at a time.
- Suggested path: start from 51b65a6's `windows/`, reapply only what our host needs (patch 1, the
  MSVC cast; 5, the build's stop flag; 12 to 14, the runtime changes) and check which of our LLPC adjustments
  (2 to 4, 6 to 11) it already covers. Measure with `mz_timing` against the current build, redo the goldens, regenerate the
  prewarm manifest and test in game before publishing.

### Not useful for us

`windows/src/pe/` (his NGX and ReShade hosts, which need vkd3d-proton), including the "in-place" of the
OptiScaler route and the unload fix of his ReShade add-on; `linux/`; his installer; the C++ model
extractor (`dlssnr_extract_model.exe`); the package names (648831a, 51b65a6).
