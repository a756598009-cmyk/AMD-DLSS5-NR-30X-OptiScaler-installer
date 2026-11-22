# lmxxf runtime source pin

- Upstream: https://github.com/lmxxf/dlss5-on-amd-9070xt-porting
- Commit: `2f19eebcd79ac7154fa4de354342cc1c105b218a` (0.46, synced 2026-11-09)
- License: MIT, Copyright (c) 2026 Kien (`LICENSE`)
- `hip_api.h` also carries the AMD HIP runtime API MIT notice from ROCm 7.1.1

This is a **vendored source closure**, not a git submodule and not the `analysis/` clone.
Files are copied byte-for-byte from that commit unless a later commit in this tree
says otherwise.

`tools/sync-lmxxf-upstream.ps1` pins sync to `-UpstreamRef` (default `origin/main`) via
`git archive` into a temp tree — the upstream working-tree branch cannot poison the copy.
Pass `-SkipUpstreamFetch` / `-AllowOfflineUpstream` when fetch is unavailable. Live
`shaders/*.hlsl` are mirror-cleaned to the top-level glue set only (`dx12-network/` is not vendored).

## OURS (do not follow upstream on sync)

Local product / stability ownership. `tools/sync-lmxxf-upstream.ps1` **preserves** the vendor files below by default; pass the named switch only when intentionally refreshing from upstream and re-applying local patches (fail-closed).

| Path | Owner note | Sync default |
|---|---|---|
| `Development/HIP/hip_d3d12_bridge.h` | Queue drain / ClearOutput / zero-residual safeguards and extra passes for `LmxxfNrRuntime` | **Preserve**; `-UpdateBridge` to overwrite + re-patch |
| `src/native_rgb_reflect.h` | Drop unused `#include "native_split.h"` so codec builds without the D3D12 network body | **Preserve**; `-UpdateReflect` to overwrite + re-drop include |
| `OptiScaler-…/dlssnr/backend/lmxxf_runtime/` (`LmxxfNrRuntime.cpp`, `LmxxfNrApi.h`, …) | OptiScaler bridge + C-ABI runtime (this product) | **Not in sync list** — never copied from upstream |
| `third_party/lmxxf/modules/` + local `hip/SHA256SUMS` gfx1201 rows | Shipping COMGR `.hsaco` built here (upstream git has no hsaco) | Built/refreshed by sync modules path, not taken from upstream git |

## FOLLOW (track upstream performance / recipe)

Synced from `-UpstreamRef` (default `origin/main`) via `git archive`. Intent: author kernel / geometry / codec / glue improvements.

| Path | Why |
|---|---|
| `hip/*.hip`, `hip/build-modules.ps1`, `hip/rtc_compile.cpp`, upstream `hip/SHA256SUMS` recipe rows | gfx1201 HIP kernels (then local rebuild of modules) |
| `Development/HIP/hip_api.h` | Loaded HIP ABI |
| `Development/HIP/hip_device_properties.h` | Device props |
| `Development/HIP/hip_reference_network.h` | HIP network (plus tiny local `#include <algorithm>` patch) |
| `Development/HIP/packed_weights.h` | Weight packing |
| `Development/HIP/swin_persistent_network.h`, `swin_persistent_types.h` | C256 persistent stage (`swin_run`, 0.37) |
| `src/native_hip_network.h` | HIP entry |
| `src/native_network_geometry.h` | 720 / 900 / 1080 tiers + FIT_LARGE helpers |
| `Development/HIP/small_model_assets.h`, `src/native_model_mode.h` | 0.44's Small model: its weights check, and the model mode (0.5/1/2/3) the network reads as it is built |
| `src/native_input_geometry.h` | Input viewport / fit |
| `src/native_lab_paths.h` | Paths, typed views, weight IO |
| `src/native_game_codec.h` | Scene encode / decode host |
| `src/native_game_rgb_input.h`, `src/native_rgb_texture.h` | RGB IO |
| `src/native_device_identity.h`, `src/native_pinned_resource.h`, `src/native_pso.h`, `src/native_shader_cache.h` | Supporting glue |
| `src/native_hip_env_options.h`, `src/native_frame_stats.h`, `src/native_format_fallback.h`, `src/native_hot_flags.h` | Included by the host and codec since 0.31-0.38; the runtime sets its options in code and does not call the env parser |
| `src/native_temporal_feed.h`, `src/native_temporal_coordinates.h`, `src/native_temporal_sample.h` | Motion vectors to coordinates, history warp (Patch D drops an unused include from the feed) |
| `shaders/*.hlsl` (top-level live glue only) | D3D12 glue; mirror-cleaned; `dx12-network/` not vendored |

## Excluded on purpose (not vendored)

- `src/native_submission_order_probe.cpp`, ReShade / MinHook addon
- `src/native_pre_upscale.h` (FFX replay; not a general splitter)
- `src/native_text_overlay.h`, `src/native_game_oneshot.h`, F6 overlay
- `src/native_game_frame.h` (`ProcessSubmittedFrame` convenience host)
- D3D12 network body (`native_split.h`, `native_actual_network70.h`, vit/c32/preblock, `shaders/dx12-network/`, etc.)
- `Development/` notes, benchmarks, and `.ps1` experiments
- Upstream `OptiScaler-DLSS5-AMD-*` packages, weights, and gitignored `.hsaco`
- Magpie packaging

## Patches applied in this tree

### General Headers
1. `NativeLabRoot()` still matches upstream (may fall back to `D:\\DLSSNR-Lab` when no `DLSS5-AMD\\native-game-flags.txt` is found). Product installs write that flags file beside the game.
2. `native_rgb_reflect.h` (**pinned**): dropped unused `native_split.h`; codec compiles without the D3D12 network body. Sync preserves it unless `-UpdateReflect`.
3. `SetNoise` skips the 201 MiB buffer when `fast_prefix` is on.
4. `#include <algorithm>` for MinGW/MSVC `std::sort` / `std::min` in `hip_reference_network.h` and `hip_d3d12_bridge.h`.

### `Development/HIP/hip_d3d12_bridge.h` (Vendor-Pinned & Patched)
> [!IMPORTANT]
> See **OURS** above. Sync preserves this header by default; only pass `-UpdateBridge` when intentionally pulling upstream bridge changes and verifying re-applied patches (fail-closed anchors + marker check).

### `src/native_rgb_reflect.h` (Vendor-Pinned & Patched)
> [!IMPORTANT]
> See **OURS** above. Upstream still `#include "native_split.h"` even though `NativeRgbReflect` does not use it; that would pull the excluded D3D12 network body. Sync preserves our header (include already removed) unless `-UpdateReflect`, which re-copies upstream then drops the include again.

1. **Queue Drain Completion Verification**: In `WaitForSubmittedWork()`, additionally checks `fence->GetCompletedValue() >= target` after `WaitForSingleObject` returns `WAIT_OBJECT_0`, preventing queue drain race conditions.
2. **Zero-Residual Fallback Path**:
   - `ClearOutputAsync()`: clears `output.mapped` via `hipMemsetAsync` and synchronizes the HIP stream.
   - `ClearOutputD3D12(targetQueue)`: synchronizes HIP stream first, then stages a zero-clear to `output.resource` on the target queue via a dedicated upload staging buffer (`zero_upload`), fences completion, and waits safely.
   - `ClearOutput(targetQueue)`: unified entry point for a submitted producer and an unsubmitted consumer. It validates the target queue, clears output, and advances the bridge to the phase appropriate for whether the consumer was already recorded. The caller must drain other queues that previously used the output.
3. **Clear Resource Lifecycle Management**:
   - Creates dedicated `zero_upload`, `clear_alloc`, and `clear_cmd` only on the first D3D12 fallback; normal `Create()` has no clear-only allocations.
   - Releases resources in destructor only after ensuring all in-flight GPU work has completed (`clear_submission_unconfirmed` check and `WaitForSubmittedWork()`).
4. **Failure State Recovery in Submit Notification**:
   - In `NotifyOutputSubmittedIfRecorded()`, if `failed` is true, safely resets `phase = Phase::Ready` without asserting `QueueContract`, allowing safe teardown or re-initialization.
5. **Runtime Opt-In and Consumer Queue Lifetime**:
   - `LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK` enables recovery in the C ABI; the default keeps strict enqueue errors.
   - Queue mismatch recovery drains both the original session queue and the target producer queue before HIP zeroing. The Runtime retains the consumer queue and drains it before frame reuse or destruction.
6. **Merged with upstream 0.39 (2026-10-01)**: upstream's bridge now carries TheAutomatic's recovery (items 1-5) itself, plus `DLSS5_HIP_INPUT_POLL`, `direct_input` and `PdlActive`. Ours is upstream's with item 7 merged back in (three-way merge against our pre-multipass header); the input poll stays off unless that variable is set.
7. **Extra passes**:
   - `EnqueueAfterProducer` takes a pass count (`LmxxfNrFrameInfo::passes`, 1 to 3). Every pass but the last runs the network and then copies its RGB output rows into the RGBA input with `hipMemcpy2DAsync`, which keeps the input's alpha. The copy goes in chunks of 2^19 rows, as upstream's `MultiPassFeed` does: on Windows the call silently stops at 2^20 rows, which left every pixel past the first 2^20 on pass 1's input. The function is resolved from the loaded HIP runtime, so `hip_api.h` stays as upstream has it.
   - `RecordPassInputCopy` gives each pass its own history buffer (`pass_history`, created on first use), and in that mode every pass but the last also copies its output into `pass_output` for `PassOutput`, which the runtime turns into the next frame's history. `RecordOutputReadable` makes those outputs readable along with the last one.
   - `SetPassOptions` (frame flags `PASS3_ESTIMATE`, `SKIN_PROTECT`): upstream's `DLSS5_MULTI_PASS_PREDICT` and `_SKIN_PROTECT` on these passes, with its own kernels (`multi-pass-predict.hsaco`, `multi-pass-skin.hsaco`, loaded on first use). With three passes the estimate runs two and extrapolates the third; skin blends the result back toward pass 1's. Without temporal history both are byte-identical to upstream's `DLSS5_MULTI_PASS=3`; 1080p three passes 30.1 -> 21.1 ms, skin +0.3 ms.

### Patch D: `src/native_temporal_feed.h`
Upstream includes `native_split.h` (the D3D12 network body) without using it. The sync script drops the include after every copy.

### Patch E: detail and colour strength up to 2
`NativeCodecParameters::ValidStrength` in `src/native_game_codec.h` accepts 0 to 2 instead of 0 to 1 (upstream accepts 0 to 3 since 0.38, so the sync leaves it alone then), and `shaders/native_codec_decode.hlsl` clamps the final result to its own `ClampAp1` gamut and to zero when either strength is above 1, where the blends extrapolate and could go negative. Up to 1 the output is unchanged. The sync script re-applies both after every copy.

### Patch G: residual smoothing in `shaders/native_output_smooth.hlsl`
`LmxxfSmoothResidual` runs upstream's output smoothing on the network's change alone. The block appended behind `#if SMOOTH_RESIDUAL` adds `residual_main`, which takes the RGB output minus the network's input (`base`, t1), blends it toward the previous frame's change (`warped`, t0; `pad` = 1 when it is valid) with the same threshold and strength rule, writes the input plus the result back to `rgb` and keeps the change in `residual` (u1) for the next frame. Upstream's `main` is untouched and compiles as before. The sync script re-appends the block after every copy when `residual_main` is missing, and fails when upstream's `main` entry is gone.

### Patch H: history copy at the network's row width
The Small model's geometry can make the network's rows wider than the colour's (1280x720 runs on 1344x768). The
history copy in `native_temporal_feed.h` / `.hlsl` reads the network's RGB with `NATIVE_RGB_PROCESSING_WIDTH` as its
stride when the two differ, the way upstream's RGB texture and decode do; otherwise it compiles as before. For the
same reason Patch G's `residual_main` keeps its change in the colour's rows (`NATIVE_VALID_WIDTH`) while it reads
and writes the network's. The sync script re-applies both.

### Sync to 0.41 (2026-10-05)
- Bridge: three-way merge again (base 8af8624, theirs 0.41). Upstream's network timing (`EnableNetworkTiming`,
  `PollNetworkTiming`), `hipSetDevice` before each enqueue (XMoon's PR #15) and the post-signal `hipStreamQuery` came in;
  our extra passes stay, and the timing span covers all of them. Upstream's own `DLSS5_MULTI_PASS` lives in the network
  and stays at its default of 1 here.
- `src/native_config_layers.h` joined the sync list (`native_lab_paths.h` includes it).
- `hip_reference_network.h` uses `__attribute__((noinline))`; the MSVC build force-includes
  `lmxxf_runtime/LmxxfMsvcCompat.h`, which defines it away. The vendor file is untouched.
- Runtime choices (`LmxxfNrRuntime.cpp`, `LmxxfProductionOptions.h`): all 71 blocks, `DLSS5_FAST_NUMERIC=1` unless the
  environment says otherwise (upstream's package default), and `DLSS5_NETWORK_FREE_RES` per frame: on for inputs up to
  1920x1080 other than 1600x900 and 1920x1080 themselves (those equal their tiers, and Compact 1080 keeps working), off
  above 1080p. The 720 tier's 1280x768 has no padding token in the ViT grid; Style had almost no effect there (42 dB
  between Styles, against 22-25 dB at 900/1080) and now matches the other sizes (22 dB at 1344x768).

### Sync to 0.42 (2026-10-14)
- Bridge: three-way merge again (base 0.41, theirs 3a1be68c). Upstream's HIP passthrough, `RequireHistory`, never-history
  sessions, direct history and its new events came in; our extra passes stay (`Enqueue` takes the pass count and the
  passthrough flag), and a recording lease's temporal flag is our `histories` bit 0.
- `Development/HIP/submit_pulse.h`, `submit_pulse_hip.h`, `src/native_experimental_history.h` and
  `src/native_fast_history_policy.h` joined the sync list (the network and `native_hip_network.h` include them).
- Runtime (`LmxxfNrRuntime.cpp`): the decoder's output now covers the whole colour texture, so the consumer copy takes
  `BufferFootprint()` from it. Upstream's `DLSS5_TEMPORAL_MODE` 1-3 (its fast history and low-frequency temporal,
  off by default there) are not ported: the runtime keeps its own temporal history.
- Modules: 40 per architecture (new: `c32-wave1-fast-norm900`, `swin-persistent-fast`), all built here with the
  driver's COMGR, the `llvm23` rows included; `hip/SHA256SUMS` holds the local hashes for both.
- Measured with the e2e harness (RX 9070 XT, driver 32.0.32015, against the 0.5.1 release): one pass is byte-identical
  at 1920x1080, 1600x900, 1280x720 (1344x720 network) and 1707x960; 1080p frame 10.45 -> 9.85 ms (median of 190
  frames, four interleaved runs). Two and three passes changed from 0.41 (the old bridge on the 0.42 network gives the
  0.42 result, so it is the network's). 0.42's three passes are byte-identical to upstream's own `DLSS5_MULTI_PASS=3`
  with `DLSS5_MULTI_PASS_PREDICT=0`, and as fast (29.0 ms at 1080p). Upstream's default (predict on) runs two passes and
  estimates the third (`mp_predict`): 19.9 ms, 35.4 dB from the real third pass.
- gfx1200 modules built but not run here (no RX 9060).

### Sync to 0.43 (2026-10-17)
- Upstream `1a057983` (0.43, payload source `10982cf4`). Only `hip/deep_fast.hip`, `hip/multihead_fast_padded.hip`,
  `hip/SHA256SUMS` and `Development/HIP/hip_reference_network.h` changed in the vendored set: decoder Up39/48 and
  downsample Down8/14/22/30 read FP8 weights in the W16 layout (`NR_DS_F8_W16`, on unless set to 0) and FP8 layouts get
  their own cache keys. The bridge, `src/` and the shaders are unchanged; nothing to merge.
- Modules: 40 per architecture rebuilt here with the driver's COMGR (14 gfx1201 modules changed); `hip/SHA256SUMS`,
  `modules/SHA256SUMS` and `modules-gfx1200/SHA256SUMS` hold the local hashes.
- e2e harness against the 0.5.3 release: 1 and 3 passes byte-identical at 1920x1080, 1600x900, 1280x720 and 1707x960.
  1080p median 10.21 -> 10.11 ms, 900p within noise. Upstream's 0.53-0.60 ms over 0.42 is mostly the October 14 work
  that 0.5.3 already ships; the October 17 part is ~20-35 us by its own ABBA.

### Sync to 0.44 (2026-10-19)
- Upstream `c24e3a97` (0.44 and its F9 hotfix, payload source `4764e4f1`). No `.hip` changed, so the 40 modules per
  architecture stay as built for 0.43 (upstream reuses all 80 too); `hip/SHA256SUMS` keeps the local hashes. New in
  the sync list: `Development/HIP/small_model_assets.h` and `src/native_model_mode.h`.
- What came in: NVIDIA 310.9.16's Small model, 31 blocks, from `<weights>/small` (`model-manifest.json` naming the
  DLL's SHA, 93 files checked by size), picked by `dlss5_mode::Requested()` as the network is built; its geometry
  (`NativeNetworkGeometry::Small`: each level halved and rounded up to 4, the 1080/900/720 tiers and free sizes),
  the RGB input, RGB texture and decode taking the network's row width, Small's adaptive ViT reuse gain.
- Bridge: three-way merge again; only upstream's `ModelMode`, `EffectiveModelMode` and `SetModelMode` accessors came
  in. Its in-place F9 switch (both models resident from the 1080 start, `PrepareForExternalInput`, no upload after
  the producer wait) is left out: the runtime builds the network again on a model change. A model the network goes
  back to with its kept weights differs from a fresh one as soon as a history comes in (46 dB on the frame after,
  both directions, e2e harness), and OptiScaler submits the producer before `EnqueueHip`, so the upload behind the
  producer wait that the hotfix forbids is what every first frame has always done here. MODE1 fast history, the F9
  hotkey and `DLSS5_MULTI_PASS` stay unused, as since 0.42.
- Runtime (`LmxxfNrRuntime.cpp`): the Small model runs, at any pass count, when its weights pass
  `SmallAssetsAvailable` and the modules it needs are there, unless the frame has
  `LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL` (upstream runs Small once only; our passes are the bridge's, which take any
  network geometry). The temporal chain gives its big buffers back to
  `NativeCreateCommittedResource`'s residency list when it goes: every chain built again (Style, compact 1080, a new
  colour size, a model change) left about 35 MB behind before (harness: +34 MiB a rebuild without end; now flat).
- Small weights: `tools/make-lmxxf-small.py` takes upstream's converter at this commit, recovers the layout maps its
  `release/` folder holds (not in its git) from the DLL's Original records and our lmxxf weights, proves them by
  converting the Original model again (184 files bit for bit), and checks the 93 Small files against upstream's
  `model-manifest-small.json`: all equal, 241,214,392 bytes. NVIDIA-derived, published as `lmxxf-weights-small`.
- e2e harness (RX 9070 XT, driver 26.9.2, temporal on), against main: the 71-block model with 1 and 3 passes is
  byte-identical at 1280x720, 1600x900, 1920x1080, 1707x960, 2560x1440 and 3840x2160, and with smoothing (both
  kinds) at 720p and 1080p. Small vs 71-block, frame time: 720p 5.9 -> 3.8 ms, 900p 7.5 -> 5.4, 1080p 10.3 -> 6.5, 1440p 10.3 ->
  6.5, 4K 10.8 -> 7.2, 1707x960 8.4 -> 5.4. Small is 34 dB from the 71-block model on one frame and edits less
  (28.6 dB from the input against 24.3).

### Sync to 0.45 (2026-10-24)
- Upstream `8181b5f2` (0.45 release; the code is `cde4cc6a`). Only `hip/deep_fast.hip`, `hip/vit_stream.inc`,
  `hip/c512_qkv_attention_compact.inc`, `hip/build-modules.ps1`, `hip/SHA256SUMS` and
  `Development/HIP/hip_reference_network.h` changed in the vendored set. The bridge, `src/` and the shaders are
  unchanged; nothing to merge. Upstream's header already includes `<algorithm>`, so Patch A is a no-op.
- What came in (speed only, same output): the compact ViT tail (contract/projection byte paths) for more processing
  shapes (Original 1600x960 and 1920x1088, Small 1600x1024, 1344x768 and 1920x1088, besides 1920x1152); paired
  producer/consumer ViT V fragments in head-major layout (`*_vfrag` kernels, `DLSS5_HIP_VIT_VFRAG=0` turns them off);
  the Q/K fragment layout on top of it for Original 1600x960 and Small 1600x1024 / 1920x1152 only
  (`DLSS5_HIP_VIT_QKFRAG=0` turns it off); C512 paired FP8 packing (`C512_PAIR_PACK 1` in the `c512-m32-mh` row).
  Both switches default on when unset; the runtime does not set them. The compact tail stays limited to gfx1201 boards
  named "9070 XT", as before.
- Modules: still 40 per architecture, rebuilt here with the driver's COMGR. 8 changed on each architecture:
  `c512-m32-mh`, `deep_fast-packed`, `deep_fast-packed-fast`, `vit-stream`, `vit-stream-fast` (new kernels or new
  defines) and `deep_fast`, `c512-m32-deep`, `vit-wide-deep` (same code; only the `__hip_cuid` source hash differs,
  since they compile `deep_fast.hip`). The other 32 rebuild byte-identical to the 0.44 set. `hip/SHA256SUMS`,
  `modules/SHA256SUMS` and `modules-gfx1200/SHA256SUMS` hold the local hashes. `modules-gfx1200/modules.json` was
  stale before and now matches its files.
- e2e harness (RX 9070 XT, temporal on, 80 frames), against the 0.44 runtime and modules: byte-identical for the
  71-block model and Small, 1 and 3 passes, at 1280x720, 1600x900, 1920x1080, 2560x1440, 3840x2160, 1707x960,
  1920x1072 and 1600x1000. The V fragments ran at 720p (Small), the 900/1080 tiers and the larger sizes (both models),
  1920x1072 and 1600x1000 (Small); Q/K at the 900 and 1080 tiers. Frame time, best of 4 ABBA medians, 0.44 -> 0.45 ms:
  71-block 720p 6.19 -> 5.84, 900p 7.59 -> 7.52, 1080p 10.41 -> 10.30, 1440p 10.39 -> 10.32; Small 720p 4.31 -> 4.12,
  900p 5.28 -> 5.23, 1080p 6.67 -> 6.57, 1440p 6.60 -> 6.53.
- gfx1200 modules built but not run here (no RX 9060).

### Sync to 0.46 (2026-11-09)
- Upstream `2f19eebc` (0.46 release base, "Round 5"). The later commits up to `60353098` (0.46 packages, 0.46.1, links)
  only touch `src/native_pre_upscale.h` and `src/native_submission_order_probe.cpp` among the vendorable files, both
  excluded here; 0.46.1's fix (`5f30cc5d`, deferred FFX replay pairing for Phantom Blade Zero) is in that add-on path,
  which OptiScaler does not have (it evaluates the game's FFX dispatch at once and never replays it later).
- Changed in the vendored set: `Development/HIP/hip_reference_network.h`, `hip/build-modules.ps1`, `hip/deep_fast.hip`,
  `hip/multihead_fast_padded.hip`, `hip/wave_owned_c32.inc`, `hip/wave_owned_mh.inc`,
  `hip/c512_qkv_attention_compact.inc`. The bridge, `src/` and the shaders are unchanged; nothing to merge, and every
  patch re-applied as before.
- What came in (speed only, same output on the tested shapes): C32 norm and out-projection hoists with the fused pool
  projection (`CW_NORM_HOIST`, `CW_OUTPROJ_HOIST`, `CW_POOL_FUSE`, `NR_C32_POOL=0` turns the fused pool off),
  `CW_FMED3_CLAMP` in the C32 wave1 rows, the Softmax ones-WMMA reciprocal hoist and paired exponent bit-map (C32,
  C64/C128/C256, SP, C512), a tighter C512 W16 FP8 gate with explicit history checks, SubmitPulse allowed on Small and
  the Original 1088 tier (it still needs a validated driver and stays off here: `init_reject_mask=262144`), the
  Backfeed knife (shape-limited, `NR_BACKFEED_*`) and the guarded four-accumulator denominator in
  `c32-wave1-fast-norm900` (`CW_EXACT_DEN_SUM`).
- Modules: still 40 per architecture, built here with the driver's COMGR (`hip/build-modules.ps1`, default targets,
  no row options). 18 changed on each architecture: `c32-wave1`, `-rtz`, `-fast`, `-fast-norm900`, `c64-wave2`,
  `-fast`, `c512-m32-mh`, `c512-m32-deep`, `deep_fast`, `deep_fast-packed`, `-packed-fast`,
  `multihead-fast-padded-wave`, `-wave-packed`, `swin-persistent`, `-fast`, `vit-stream`, `-fast`, `vit-wide-deep`;
  the other 22 rebuild byte-identical. `hip/SHA256SUMS` (both architectures), `modules/SHA256SUMS` and
  `modules-gfx1200/SHA256SUMS` hold the local hashes (upstream's rows differ for every module, as before).
- The sync script's own module build failed under PowerShell 7 here (`-File` binding error after the child build);
  the modules were built with `build-modules.ps1` directly and passed with `-NoBuildModules -ModulesPath`.
- e2e harness (RX 9070 XT, temporal on, 80 frames, static synthetic colour), against the 0.45 runtime and modules:
  byte-identical for the 71-block model and Small, 1 and 3 passes, at 1280x720, 1600x900, 1920x1080, 2560x1440,
  3840x2160 and 1707x960, and Small at 1920x1072 and 1600x1000 (18 cases). Network GPU time (the bridge's HIP events,
  6 ABBA rounds, best median), 0.45 -> 0.46 ms: Small 720p 2.59 -> 2.54, 900p 3.87 -> 3.71, 1080p 5.08 -> 4.88,
  1440p 5.05 -> 4.92; 71-block 720p 4.75 -> 4.61, 900p 6.26 -> 6.16, 1080p 8.85 -> 8.62, 1440p 8.84 -> 8.65.
- gfx1200 modules built but not run here (no RX 9060).

## Shipping modules (`.hsaco`)

Two sets: `modules/` (gfx1201, RX 9070 series) and `modules-gfx1200/` (RX 9060 series), shipped as `lmxxf-modules` and
`lmxxf-modules-gfx1200` (installers accept one folder level, so not upstream's `HIP\gfx1200\`). The runtime asks HIP
for the architecture in `Create` and takes the sibling folder on gfx1200. The sync script builds gfx1201 only; for
gfx1200 run `hip/build-modules.ps1 -Targets gfx1200` (pwsh, `rtc_compile.exe` from `rtc_compile.cpp`), copy the
`.hsaco` into `modules-gfx1200/`, write its flat `SHA256SUMS` and the `gfx1200/` rows of `hip/SHA256SUMS`.

Upstream **does not** publish `.hsaco` on git (`/release/` is gitignored; no GitHub release assets for modules).
Release zips copy `third_party/lmxxf/modules` as-is; GitHub Actions does **not** rebuild HIP kernels.

`tools/sync-lmxxf-upstream.ps1` therefore:

- syncs hip *sources* + `SHA256SUMS` from upstream git;
- by default runs `hip/build-modules.ps1 -Targets gfx1201` into a local build dir, then copies into `third_party/lmxxf/modules`;
- or accepts an explicit `-ModulesPath` to already-built flat gfx1201 `.hsaco`;
- **fails closed** if hip recipes change but modules do not, or modules disagree with `hip/SHA256SUMS` `gfx1201/` entries.

Use `-SkipModules -AllowStaleModules` only for intentional header-only syncs. Use `-NoBuildModules` with `-ModulesPath` when modules were built offline.

## Upstream Contribution & Decoupling Roadmap

1. **Keep Recovery Policy in Runtime**:
   - `network`, `output.mapped`, and `network->Stream()` are private within `D3D12Bridge`; its public `ClearOutput` is the minimal safe transport primitive.
   - The C ABI Runtime decides when to invoke that primitive and owns the cross-queue drain contract. The upstream contribution should include both layers.
2. **Instance Config Refactor**:
   - Process-global geometry/env -> instance config.
3. **Proxy Forwarding**:
   - Host P1: List1-10 command-list proxy forward (then hooks). Not this vendor tree.

## Runtime ABI (this tree)

C ABI in `include/LmxxfNrApi.h`. MSVC (primary) or MinGW (fallback) `tools/build-lmxxf-runtime.cmd` compiles the HIP bridge and codec into `LmxxfNrRuntime.dll`.

- Modules: `third_party/lmxxf/modules` (COMGR gfx1201 hsaco, tracked in git; built from current `hip/` + local COMGR (see modules/README.md Commit Base)).
- Weights: `LMXXF_WEIGHTS_DIR` tiled assets (not 0.24.2 `HIP/`).
- `QueryCapabilities.hip_ready` stays **0**. Product wiring lives in OptiScaler `lmxxf_runtime` / `LmxxfWired()` (not this vendor doc alone).
