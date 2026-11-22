# lmxxf 0.44 sync (worktrees wt-lmxxf044 and wt-installer-lmxxf044, branch lmxxf-0.44)

Committed on both branches, local only. opti: `4b787f9fd` on main 93e5f9427. Installer: `57d69b8` on master f96a96b.
Upstream lmxxf/dlss5-on-amd-9070xt-porting `c24e3a97` (0.44 and its F9 hotfix, payload source 4764e4f1). Details in
`third_party/lmxxf/UPSTREAM.md`, sections "Patch H" and "Sync to 0.44".

## Upstream 0.44, taken and left

- No `.hip` changed (checked: `git diff 1a057983 c24e3a97 -- hip` is empty). The 40+40 modules stay as built for 0.43,
  `hip/SHA256SUMS` keeps the local hashes. The sync script ran with `-UpstreamRef c24e3a97 -SkipUpstreamFetch
  -SkipModules -AllowStaleModules -SkipBuild`, upstream's `hip/SHA256SUMS` put back to ours after it.
- Taken through the sync script: the network's Small model (31 blocks from `<weights>/small`, picked by
  `dlss5_mode::Requested()` as it is built), its geometry (`NativeNetworkGeometry::Small`, 1080/900/720 tiers and free
  sizes; the network's rows can be wider than the colour's, 1280x720 runs on 1344x768), the RGB input / RGB texture /
  decode taking that row width, Small's adaptive ViT reuse, `NativeLabRootOverride`, hot-flag changes. New in the sync
  list: `small_model_assets.h`, `native_model_mode.h`. Vendored files now equal upstream except the bridge (ours) and
  patches D, E, G, H.
- Bridge (ours): three-way merge, only the `ModelMode`, `EffectiveModelMode`, `SetModelMode` accessors.
- Left out: the in-place F9 switch (`PrepareForExternalInput`, upload ban after the producer wait, both models resident
  from a 1080 start), MODE1 fast history and its history-head swap, the F9 hotkey, `DLSS5_MULTI_PASS`. Why the switch
  is out: tested here, a model the network returns to with its kept weights differs from a fresh build as soon as a
  history comes in (46 dB on the first history frame, both directions); with a rebuild every switch is byte-identical to
  a cold start. The upload-after-wait deadlock the hotfix fixes cannot happen here: OptiScaler executes the producer list
  before `EnqueueHip`, and every first frame already uploads after the wait.
- Upstream's own `src/LmxxfNrRuntime.cpp` changes ported by hand into ours: Small choice before the geometry,
  `opt.small_geometry`, model change drains and tears the chain down. Their Onimusha/XboxGames path and hot-reload keys
  are not ours.

## Design

- ABI: no struct change. The existing `LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL` (1<<13, mochizuki's) now means for lmxxf:
  run the 71-block model. Without it, with one pass and the Small weights present (and the c32-wave1, c64-wave2,
  c512-m32-*, vit-wide-deep modules), the runtime runs Small. An older lmxxf runtime refuses the flag, so only a host
  and runtime from the same package should be mixed (they ship together in the OptiScaler zip).
- Passes (changed since, see "Small with 2 and 3 passes"): Small runs once. Passes 2-3 always run the 71-block model, as upstream's 1/2/3 modes (0.5 = Small one pass).
  So estimate pass 3, skin protect and person protection (all need 2+ passes) run on the 71-block model. A codec
  passthrough frame keeps the model the network was built with.
- Key `[DlssNr] LmxxfOriginalModel` (bool, default false). Menu, lmxxf page, Effect section, above Passes: "Old model"
  checkbox, disabled and shown ticked when Passes > 1; in Reset all. Compact 1080 help notes Small always runs 1152 rows.
- Switching model rebuilds the network synchronously in PrepareFrame, like a Style change (a second or two), never
  in place. Status line and log name the model (`model=small|original`).
- Fixes found on the way, both in 0.5.5 too:
  - the temporal chain never untracked its big buffers from `NativeCreateCommittedResource`'s residency list: every
    rebuild with temporal on (Style, compact 1080, colour size, model) leaked ~35 MB of VRAM without end (main:
    +700 MiB over 39 rebuilds; new: flat after the HIP allocator settles);
  - Patch H + Patch G change: the history copy and the residual smoothing read the network's rows at its own width,
    needed for Small's wider rows (no effect on the 71-block model, byte-identical).

## Small weights

- Upstream's converter needs layout maps from its `release/` folder, which is not in its git and not on this machine.
  `tools/make-lmxxf-small.py` (committed) takes the converter and evidence from the upstream clone at c24e3a97,
  recovers the maps by matching the DLL's Original records against our lmxxf weights, proves them by converting the
  Original model again with `--compare` (184 files bit for bit), converts Small, and checks it against upstream's
  `model-manifest-small.json`.
- Result: 93 files + `model-manifest.json`, 241,214,392 bytes, every SHA-256 equal to upstream's receipt, the manifest
  itself byte-identical to upstream's. Source DLL: scratchpad `nrdll_new\nvngx_dlssnr.dll` (310.9.16,
  2bc19bc0..., the one mochizuki's small model came from).
- Staged (not committed): `wt-lmxxf044\exports\lmxxf044-work\small\`, zipped for upload in
  `exports\lmxxf044-work\publish\lmxxf-weights-small-1.zip` (99,813,808 bytes, sha256
  86ec19a8be8bfe0507bbfc6bfaaafc2124332a56c617678d4c5d2a6785666d91).
- To make them again: `py -3 -I tools\make-lmxxf-small.py --dll <nvngx_dlssnr.dll 310.9.16> --weights
  <native-game-tiled-assets> --out <empty folder>` (about a minute and a half).

## Installer

- New component `lmxxf-weights-small` (versioned, zipped like `lmxxf-weights`), inside OptiScaler releases only,
  on demand, left out on non-RDNA4 cards (`WithoutLmxxf`), not for 32-bit games. Game path
  `native-game-tiled-assets\small\<file>`; payload path `native-game-tiled-assets.small/<file>` (dot for the folder,
  as mochizuki's `dlssnr-amd.shaders.*`), mapped by `Work.OptiDestination`; `Engine.IsAllowed` accepts one file level
  below `native-game-tiled-assets/small/`; uninstall takes the folder.
- Released installers: every installer validates every path of every release (2 segments max, plain names) and fails
  the whole payload otherwise, hence the dot. With the dot they parse it (checked: the pinned payload copy parses and
  pins 94 files with the new code, same `Check`), and they fetch only components named in their own lists, so they
  ignore it. Not in `components` top level, so no first-run download.
- payload.json is untouched (previous syncs were pinned at release time with the pin scripts, no placeholders).
  New `tools/pin-lmxxf-small.ps1` (verifies every file against model-manifest.json, fixed-order zip, pins, prints the
  upload command); `pin-optiscaler.ps1` lends the component to later releases like `lmxxf-weights`.
- Tests: `LmxxfSmallTests` (3: parse + deeper path refused + pins + WithoutLmxxf, allowed names and destination,
  install below the weights and uninstall). `dotnet test`: 404/404. `tools\gate.ps1 -Ui`: build, test, publish, ui,
  flows OK; line-limit FAIL on `tests/AmdNr.Core.Tests/GraphicsTests.cs` (504 lines), already so on master f96a96b.

## Verification (RX 9070 XT, driver 26.9.2, e2e harness `exports\lmxxf044-work\h`, temporal on unless said)

- 71-block model vs main build (runtime from 93e5f9427): byte-identical, 1 and 3 passes, at 1280x720, 1600x900,
  1920x1080, 1707x960, 2560x1440, 3840x2160; also with smoothing and residual smoothing at 720p and 1080p.
- Frame time (harness wall time per frame, codec and full sync included; best of 4 ABBA medians), 71 -> Small:
  720p 5.87 -> 3.83 ms, 900p 7.52 -> 5.41, 1080p 10.29 -> 6.54, 1440p 10.30 -> 6.50, 4K 10.75 -> 7.15,
  1707x960 8.37 -> 5.42 (about -35%). Upstream reports network-only 9.16 -> 5.55 at 1080.
- PSNR: Small vs 71-block one frame without history 34.3 dB (1080) / 34.0 (720); after 80 static frames with history
  29.5 dB (our history drifts on a static frame for both models: 71-block with vs without history 30.9 dB, Small
  41.8). Small edits less: 28.6 dB from the input vs 24.3 for the 71-block. All outputs finite. Small vs NVIDIA cannot
  be measured here (upstream: ~47 dB vs RTX 5090).
- Switches: Small <-> 71-block every 10-20 frames at 1080 (same size), at 900 (1600x1024 vs 1600x960), passes 1 <-> 3,
  and colour 1280x720 <-> 1920x1080: no failure; after 8-9 switches the output is byte-identical to a cold run of that
  model; VRAM settles (1080: 1466 MiB after ~9 switches, flat through 40).
- Regression tests: test-amd-host-contracts, test-shader-compatibility, test-hip-runtime-load, test-lmxxf-evaluate-cut,
  -nr-abi, -create-execute, -list-split, -list1-wrap, -same-frame-boundary, -color-probe, -nr-harness: all PASS.
  (test-amd-runtime-init needs a danielblnc DLL, not run.)
- Builds: `LmxxfNrRuntime.dll` (sha256 53044c33...) and `OptiScaler.dll` (sha256 88a6cd69...) with MSVC 14.44. The
  OptiScaler build needed the submodules: junctions from `external\*` to `opti\external\*`, removed afterwards (empty
  folders back as checked out).

## Install folder for Cyberpunk

`wt-lmxxf044\exports\lmxxf044-work\install\` (863 MB), laid out as `bin\x64` needs it:
`dxgi.dll` (the new OptiScaler.dll), `LmxxfNrRuntime.dll`, `lmxxf-modules\`, `lmxxf-modules-gfx1200\`, `shaders\`
(lmxxf's, changed), `native-game-tiled-assets\` (the 460 pinned weights, checked against the payload, plus `small\`).
Back up `dxgi.dll` first as usual; set `NrBackend=lmxxf` in OptiScaler.ini (Cyberpunk runs mochizuki now). The
`OptiScaler\` folder already there is fine.

## What to test in game

1. lmxxf with 1 pass: status/log says `model=small`; compare look and the neural pass meter with "Old model" ticked.
2. Tick/untick Old model and change Passes 1 <-> 2/3: a short rebuild hitch, no black frames, no crash; VRAM in the
   overlay should not climb with repeated switches.
3. Temporal history and Smoothing (also "Smooth the effect only") with Small: no ghost/shear, especially at a 720p-ish
   render size (the wider-rows case).
4. Dynamic resolution / resolution change while on Small.

## Publish steps (for the user)

1. Merge `lmxxf-0.44` into opti main; build the release as usual (tools\build-release-local.cmd / PACKAGE_RELEASE).
2. After the OptiScaler release is on GitHub, in the installer repo (after merging `lmxxf-0.44`):
   `pwsh tools\pin-optiscaler.ps1 -Zip <OptiScaler-<v>.zip> -Published <date>`
3. `pwsh tools\pin-lmxxf-small.ps1 -From E:\Projetos\opti_rr_dlss5\wt-lmxxf044\exports\lmxxf044-work\small -Version <v>`
   (writes `publish-lmxxf-weights-small-1\lmxxf-weights-small-1.zip`, same bytes as the staged one, and pins it).
4. Upload before publishing the payload:
   `hf upload zmodelerlover/amd-nr "<repo>\publish-lmxxf-weights-small-1\lmxxf-weights-small-1.zip" lmxxf-weights-small/1/lmxxf-weights-small-1.zip --repo-type dataset --commit-message "lmxxf 0.44 Small model weights 1 (NVIDIA 310.9.16)"`
5. `tools\publish-payload.ps1` as usual. The add-on/ReShade side is not touched (lmxxf runs in OptiScaler only).

## Small with 2 and 3 passes (2026-10-19, after bd814bd01)

- Rule removed: the model is `[DlssNr] LmxxfOriginalModel` (`LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL`) alone, at any pass
  count. Runtime: `passes == 1` out of the Small choice. Menu: "Old model" no longer greyed or forced ticked with
  Passes > 1; help, `Config.h`, `LmxxfNrApi.h`, README and UPSTREAM.md say so. The host already sent the flag from the
  key only. Installer untouched (nothing there depends on it).
- Nothing else needed: the extra passes, estimate (`mp_predict_*` over the network's W x H, 16x16 tiles), skin
  (`mp_skin_*` over n = W x H), the per-pass history feeds/samplers and the first pass's codec all take the network's
  geometry as pass 1 does, wider rows included.
- Harness (`h\r.ps1`, `h\lmxxf_e2e.cpp` now dumps `first_pass_output` with `E2E_FIRST`; new runtime in `h\sp`, the
  previous one still in `h\new`), temporal on, best of 3 alternating medians, ms:

  | | 720p Small / 71 | 1080p Small / 71 | 1440p Small / 71 |
  |---|---|---|---|
  | 1 pass | 4.65 / 7.24 | 6.84 / 10.41 | 7.27 / 10.46 |
  | 2 passes | 7.79 / 11.03 | 12.64 / 20.32 | 12.62 / 20.20 |
  | 3 passes | 9.76 / 15.93 | 18.72 / 30.14 | 18.55 / 30.00 |
  | 3 with estimate | 7.20 / 11.69 | 13.37 / 21.03 | |

- PSNR after 80 static frames (dB): Small 2 passes vs Small 1: 27.5 / 29.3 / 28.9 (720/1080/1440); 3 vs 1: 24.8 /
  23.5 / 23.4. Small vs 71-block at the same passes: 1 pass 27.2 / 29.5 / 29.7, 2 passes 20.5 / 24.1 / 25.0, 3 passes
  21.7 / 21.8 / 22.8. All outputs finite, no failure; no shift or edge band between Small 1 and 3 passes at 720p.
- Paths checked on Small: estimate (35-41 dB from real 3 passes without history), skin at 2 and 3 passes (output
  changes, 23-34 dB), smoothing and residual smoothing at 3 passes, first pass output (person protection): handed out
  and byte-identical to a 1-pass run at 720p and 1080p, also with skin on.
- Byte-identical to the previous runtime: 71-block 1, 2 and 3 passes and Small 1 pass at 720p, 1080p, 1440p (71-block
  also equal to main's base runs).
- Switches (400 frames, every 10): passes 1 <-> 3 on Small at 1080p/720p, VRAM flat (1401 / 978 MiB from the start);
  Small <-> 71-block at 3 passes, VRAM settles after ~9 switches and stays flat (1080p 1834 / 1706 MiB), the same
  shape as the 1-pass switch before; the output after the switches equals a cold run of that model, both directions.
- Seen, not a Small bug: with history on a static image the output moves from frame to frame, more with each pass and
  more with the 71-block model (frame 79 vs 80, 1080p: Small 51.6 / 38.6 / 28.1 dB for 1/2/3 passes, 71-block 27.3 /
  23.6 / 21.2). At 720p Small with 3 passes sits further from its no-history result (23 dB vs 30-33 at 900p/1080p),
  and so does 1408x792, which has no wider rows, so it is the size, not the row width.
- Tests: host-contracts, shader-compatibility (with the junctions), hip-runtime-load, the lmxxf ones and nr-harness:
  PASS (amd-runtime-init still needs a danielblnc DLL).
- Builds in `exports\lmxxf044-work\install`: `dxgi.dll` sha256 433f47945a28d90ab44243cb04ca7cb40a55b12c42725d86535949c9ab7c88e8,
  `LmxxfNrRuntime.dll` sha256 009e1921c6beb065459d181c6d36d270e3f7065e31b3597282242d48567f915b. Junctions removed.
- In game: Small with Passes 2 and 3 (log `model=small`), Estimate pass 3 and Skin protect with it, person protection.
