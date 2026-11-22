# Handoff: mochizuki backend (third AMD NR runtime)

Complete record of the work on 2026-09-24 that got the neural network from the
**mochizuki0323/DLSSNR-AMD** project (Vulkan) running inside this OptiScaler fork, alongside the
danielblnc and lmxxf runtimes. Written for whoever continues, person or AI: what was done and in what order, what was
measured, where the files are, how to reproduce it and what is left.

Paths: `<REPO>` is this checkout. Section 7 lists the local state of the development machine, with
real paths.

**Status:** works in Cyberpunk 2077 (tested by the user). Installed for testing in GTA V Enhanced,
awaiting testing. **Nothing committed yet.**

**Update of 2026-09-25:** two waves of work (2026-09-24/25) added controls, stability
fixes, performance and test tooling. Summary, numbers and open items in **section 11**. The candidate for the
first release is the snapshot `<REPO>/exports/mochizuki-test-wave2/`, not yet tested in game.

---

## 1. Summary

- DLSSNR-AMD reimplements the DLSS 5 NR network (NVIDIA `nvngx_dlssnr.dll` 310.8.0) in Vulkan compute shaders
  that use FP8 (`VK_KHR_cooperative_matrix` + `VK_EXT_shader_float8`, RDNA4 WMMA instructions).
  The weights come from the user's own NVIDIA DLL.
- The author's host only works with the game running on vkd3d-proton: it requests the game's `VkDevice` and command
  buffer from the vkd3d interop interfaces. In native D3D12 that does not exist.
- **Solution used here:** the game stays on native D3D12. The network runs on its **own Vulkan device** on the
  same GPU, slotted between the two halves of the game's command list, with the same list-splitting
  mechanism the fork already used for lmxxf (HIP).
- The runtime is `MochizukiNrRuntime.dll`, which implements the lmxxf ABI (`LmxxfNrApi.h`). OptiScaler
  selects it with `[DlssNr] NrBackend=mochizuki`.
- This version only turns on and off (`Enable NR`), runs before Super Resolution and uses the model's own temporal
  history with the game's motion vectors. No controls; Phase 2 (§9) adds them.

| Headline number | 2026-09-24 (0.3.0) | 2026-09-25 (wave 2, §11.4) |
|---|---|---|
| Network GPU time at 1920x1080 (RX 9070 XT, driver 26.8.1) | 9.9 ms (lmxxf: 18.5 ms) | 8.9 ms |
| First network build | ~55 s in game, 24.8 s in the harness | 5.8-6.0 s in the harness with the prewarm manifest; in game, not measured |
| Subsequent builds | 1.5 to 3 s | 1.5 to 1.7 s in the harness |
| Stability in motion with history | 73% more stable than without history | same (bit-for-bit identical output) |

---

## 2. Step by step (2026-09-24, in order)

1. **Repository analysis.** Upstream created the same day: v0.0.1, commit `791b046`, MIT. Conclusion:
   its host does not work for native D3D12; the way forward is an own Vulkan device with D3D12 interop,
   reusing the lmxxf slot-in. The user decided to implement it directly.
2. **Model extraction.** `dlssnr.bin` generated from the local `nvngx_dlssnr.dll` 310.8.0 (SHA-256
   `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`): 599 entries, 147,756,560 bytes,
   all checked against the author's hashes. The author's bash script fails on Windows; the Python steps
   were run by hand (section 6.2).
3. **Shaders.** glslang 16.5.0 for Windows (the version the author pins). `build_network.py` generated 39
   network pipelines, 4 temporal and 6 runtime ones. Two independent compilations produced identical SPIR-V.
4. **Upstream in the repository.** Core, shaders and scripts copied to `third_party/mochizuki`
   (56 files, 1.8 MB) with `UPSTREAM.md` recording the commit and the patch.
5. **Runtime.** `OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrRuntime.cpp`: lmxxf ABI,
   Vulkan device chosen by the LUID of the game's GPU, shared D3D12 buffers, D3D12 fence imported
   as a timeline semaphore, network build on a separate thread.
6. **Build with MSVC.** `tools/build-mochizuki-runtime.cmd`. It needed a one-line patch to the author's
   code, a shim for `dirent.h` and the Vulkan SDK 1.4.357 headers.
7. **First test outside the game (`mz_bench`).** Build in 24 s cold and 1.5 s with cache. Network in
   9.9 to 10.2 ms at 1080p; full frame in 12 to 13 ms. Output checked: aligned with the input
   (correlation 0.984 at zero shift), no NaN.
8. **Integration into OptiScaler.** `Kind.h`, `Selector`, `AmdBridge`, `LmxxfBackend` (now receives the DLL
   name), menu (combo with the installed runtimes and a mochizuki section with only status and cost) and
   `Config.h`. OptiScaler built; clang-format clean.
9. **Protection against OptiScaler's Vulkan hooks.** The runtime's device creation runs inside
   `ScopedSkipVulkanHooks` and `ScopedCreatingD3DDevice`.
10. **Phase test (`mz_phases`).** In a single session: 1080p, 720p in R11G11B10 with R32G32 vectors at
    1440p, 1440p, 1080p. Background build, draining and rebuilding worked.
11. **Installation in Cyberpunk** with `install-mochizuki.ps1` (backup of the previous `dxgi.dll`; INI from the
    0.3.0 package with `NrBackend=mochizuki`).
12. **Architecture document** for another AI to port (section 7).
13. **User test in Cyberpunk: it worked.** ~20 min session without errors; network at 1920x1080 (ready
    in 54.8 s the first time) and at 1280x720 (2.7 s). The log showed `motion 0x0 DXGI 0`: the motion
    vectors did not arrive and the network ran without history.
14. **Vector fix.** Cyberpunk sends the vectors in `R16G16B16A16_FLOAT` and the runtime only
    accepted 2 channels. It now accepts 4 channels (the core reads only R and G) and logs rejected formats.
15. **Validation of vector sign and scale (`mz_pan`).** Content moving 2 px per frame, vectors
    in RGBA16F. Correct vectors: 73% more stable than without history. Inverted sign: worse than without
    history. The convention is correct (section 4.6).
16. **Cyberpunk updated with the fixed runtime and GTA V Enhanced installed.**
17. **Harnesses saved** in `<REPO>/exports/mochizuki-harness/` and record published (artifact and this
    file).

---

## 3. Results

### 3.1 Measurements

Numbers from 2026-09-24, before waves 1 and 2. The current ones are in §11.4 (the "~23 ms" at 1440p came from a moving average and
a cold build; measured later: 16.7-17.3 ms on 0.3.0).

| Measurement | Value | Where |
|---|---|---|
| Network GPU time, 1920x1080 | 9.9 ms | `mz_bench`, 100 frames |
| Full frame, 1920x1080 (copies, submit, wait) | 12.0 ms | `mz_bench`, median |
| Network GPU time, 2560x1440 | ~23 ms | `mz_phases` |
| Cold build, harness | 24 s | first time per executable |
| Cold build, Cyberpunk 1080p | 54.8 s | game log |
| Build with cache | 1.5 s | harness |
| In-game rebuild on resolution change | 2.7 s | Cyberpunk, 720p |
| Network memory at 1080p | ~537 MB | 384 MB of activations, 153 MB of weights |
| Reference: lmxxf (HIP), 1080p | 18.5 ms | measured on 0.3.0 |
| Reference: author, 1080p | 9.4 ms (Windows), 6.2 ms (Linux) | upstream README |

### 3.2 Stability in motion (`mz_pan`)

How much the network's effect (output minus input) changes from one frame to the next, following content that
moves 2 px per frame. Lower is more stable.

| Case | Change between frames | Average effect |
|---|---|---|
| No vectors (as in the first Cyberpunk test) | 0.00541 | 0.0206 |
| Correct vectors (mv = -2 px) | 0.00148 | 0.0288 |
| Vectors with inverted sign | 0.00576 | 0.0303 |
| Static image (floor) | 0.00078 | 0.0299 |

### 3.3 Robustness

| Test | Result |
|---|---|
| Output aligned with input (correlation by shift from -2 to +2 px) | peak at 0, 0.984 |
| Resolution change mid-session | drains, rebuilds and continues |
| R16G16B16A16 and R11G11B10 colour | works, no NaN |
| R16G16, R32G32 (1440p with 720p colour) and R16G16B16A16 vectors | works |
| Cyberpunk 2077, ~20 min, 1080p and 720p | worked, no errors in the log |

---

## 4. Architecture (essentials)

Runtime details also in `OptiScaler/dlssnr/backend/README.md` (mochizuki section) and in
`third_party/mochizuki/UPSTREAM.md`.

### 4.1 Components

```
Game (native D3D12)
 └─ OptiScaler (dxgi.dll)
     ├─ AmdBridge              intercepts the upscaler's Evaluate and swaps the colour for the NR result
     ├─ Kind / Selector        NrBackend = daniel | lmxxf | mochizuki
     ├─ LmxxfBackend           host of the "lmxxf" ABI; loads the DLL by name
     └─ submission/            command list proxy: cuts the list and calls a "between" callback

MochizukiNrRuntime.dll (MSVC, C ABI from LmxxfNrApi.h)
 ├─ MochizukiNrRuntime.cpp     session, D3D12 <-> Vulkan interop, jobs
 └─ nr_runtime.cpp (+ nr_graph.cpp), nr_native_plan.cpp   upstream core
      └─ vulkan-1.dll -> AMD's Vulkan driver

Next to the DLL:
 dlssnr-amd/dlssnr.bin         weights (generated from NVIDIA's DLL)
 dlssnr-amd/shaders/           network SPIR-V, runtime/ and temporal/
 dlssnr-amd/pipeline.cache     generated on the first run
 mochizuki_nr.log              runtime and core log
```

### 4.2 Flow of a frame

```
Render thread (Evaluate)                        Submit thread (ExecuteCommandLists)
PrepareFrame  : network ready? geometry resources
RecordInputs  : colour and vectors -> shared buffers (segment 1)
SplitSegments : cuts the list
RecordOutputs : output buffer -> "result" texture (segment 2)
AmdBridge swaps the upscaler's colour for "result"
                                                Execute(segment 1)
                                                between -> EnqueueHip:
                                                   D3D12 queue: Signal(fence, N)
                                                   Vulkan: waits N, buffer -> images,
                                                           record_engine, image -> buffer,
                                                           signals N+1
                                                   D3D12 queue: Wait(fence, N+1)
                                                Execute(segment 2)  (copy + upscaler)
                                                Submitted -> Retire
```

The queue used is the one that executes the list. Synchronization is done entirely through the shared fence, so any
queue on the same device works. Between frames, the signal/wait chain orders the reuse of buffers and images.
Three slots (command buffer + `VkFence`) protect only the reuse of command buffers on the CPU.

Since wave 2 (S3) there are **two** shared fences, one per direction: `toVk` (the game queue signals
`produced`, Vulkan waits) and `fromVk` (Vulkan signals `finished`, the game queue waits), so that
neither goes backwards. A frame coming from another game queue first waits for the last frame of the previous queue; the
runtime declares `ANY_QUEUE`, so the host does not recreate the session when switching queues. A watchdog releases the
game queues if the Vulkan side stalls (§11.3).

### 4.3 Own Vulkan device

- Vulkan 1.3 instance, no layers. Physical device chosen by the **LUID** of the D3D12 adapter
  (`VkPhysicalDeviceIDProperties::deviceLUID`). Family `GRAPHICS | COMPUTE`; since wave 2 (S4), two queues
  when the family has them (priorities 1.0 and 0.5): frames use the first and the network build the second.
- Features: `cooperativeMatrix`; `shaderFloat8` and `shaderFloat8CooperativeMatrix`;
  `storageBuffer16BitAccess`; `storageBuffer8BitAccess`, `shaderFloat16`, `shaderInt8`,
  `vulkanMemoryModel`, `timelineSemaphore`; `subgroupSizeControl`, `synchronization2`;
  `workgroupMemoryExplicitLayout` with 8 and 16 bits.
- Extensions: `VK_KHR_cooperative_matrix`, `VK_EXT_shader_float8`, `VK_KHR_workgroup_memory_explicit_layout`,
  `VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32`.
- The `nr::Runtime` constructor **checks** the features instead of enabling them and submits to the queue to
  upload the weights; that is why, with a single queue, it has a mutex shared between frames and build. With
  two, the build does not compete for the frame queue.

### 4.4 Interop

- **Buffers, not textures:** importing a texture requires both APIs to agree on tiling. Buffers are
  linear. It costs one extra copy on each side, negligible at 1080p.
- Buffer: `CreateCommittedResource` with `D3D12_HEAP_FLAG_SHARED` (size rounded up to 64 KB),
  `CreateSharedHandle`; in Vulkan, `VkExternalMemoryBufferCreateInfo` with
  `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT`, memory type via
  `vkGetMemoryWin32HandlePropertiesKHR`, **dedicated** allocation with `VkImportMemoryWin32HandleInfoKHR`.
  The NT handle stays with whoever created it; it is closed on destruction.
- Fence: `CreateFence(D3D12_FENCE_FLAG_SHARED)` + `CreateSharedHandle`; in Vulkan, a timeline semaphore and
  `vkImportSemaphoreWin32HandleKHR` with `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT`. The counter is
  the same on both sides. Since wave 2 there are two such fences, one per direction (§4.2).
- Three buffers per geometry: input (colour), output (result) and vectors.

### 4.5 Formats and footprints

| DXGI (includes TYPELESS) | VkFormat | Bytes |
|---|---|---|
| `R16G16B16A16_FLOAT` | `R16G16B16A16_SFLOAT` | 8 |
| `R32G32B32A32_FLOAT` | `R32G32B32A32_SFLOAT` | 16 |
| `R11G11B10_FLOAT` | `B10G11R11_UFLOAT_PACK32` | 4 |
| `R10G10B10A2_UNORM` | `A2B10G10R10_UNORM_PACK32` | 4 |
| `R8G8B8A8_UNORM(_SRGB)` | `R8G8B8A8_UNORM` | 4 |
| `B8G8R8A8_UNORM(_SRGB)` | `B8G8R8A8_UNORM` | 4 |
| vectors `R16G16_FLOAT` / `R32G32_FLOAT` | `R16G16_SFLOAT` / `R32G32_SFLOAT` | 4 / 8 |
| vectors `R16G16B16A16_FLOAT` / `R32G32B32A32_FLOAT` | the same in SFLOAT | 8 / 16 |

- Float colour (RGBA16F, R11G11B10, RGBA32F) is treated as linear light: `linear_input = true`,
  `white_point = 1.0`.
- Footprint of the `w x h` rectangle: `pitch = align(w * bytes, 256)`; in D3D12 the footprint uses the
  texture's format; in Vulkan `bufferRowLength = pitch / bytes`.
- The `result` texture has the desc of the game's colour and always stays in `NON_PIXEL_SHADER_RESOURCE`, the same
  contract as the lmxxf output.

### 4.6 Motion vectors and history

- OptiScaler/NGX convention: `value * MV_Scale = pixels of the vector extent`; previous position =
  current + mv.
- The author's shader samples the history at `(uv + mv) * extent`, with `mv` in normalized UV. Therefore
  `motion_scale = MV_Scale / vector extent` (output when the game does not use `MVLowRes`, render
  when it does). Validated with `mz_pan` (section 3.2).
- Frame `reset` = game reset, or a non-continuous frame (`frame_id` out of sequence or more than 250 ms
  since the previous one), or a pending `ResetHistory`/`CancelUnsubmitted`.
- Depth is not passed yet.

### 4.7 Network build

- Per extent and colour format, on a `std::thread`. While building, `PrepareFrame` returns
  `LMXXF_NR_UNAVAILABLE` with "building the network" and the frame passes through without NR.
- Extent or format change (text from 2026-09-24; updated in wave 2, S4/S5): the render thread no longer waits
  for the GPU (with a single game queue; with several, the old buffers are still drained). The old network is
  freed by its own thread after Vulkan finishes its last frame, and the new one is built on the
  second queue. Change of passes, model scale or linear input only: frames
  keep going through the old network until the new one is ready ("keep serving"). With `MochizukiDynamicResolution` set to
  `auto` (default) or `always`, changing the render subrect within the bucket rebuilds nothing (§9, Phase 2).
- Pipeline cache in `dlssnr-amd/pipeline.cache`. The AMD driver separates entries per executable:
  the first time in each game compiles everything. Since wave 2 (P3), `dlssnr-amd/prewarm/manifest.txt` describes
  the 32 pipelines and the runtime compiles them in parallel (8 threads, below-normal priority) before the core,
  and the cache is written again after the adapters and the temporal ones.
- Failing build (text from 2026-09-24): marked the session as failed. Since wave 2 (S2) only device loss is
  permanent; out of memory retries after 5, 30 and 120 s, other errors when the key changes.

### 4.8 Changes in OptiScaler

| File | Change |
|---|---|
| `dlssnr/backend/Kind.h` | `Kind::Mochizuki`, `ParseKind("mochizuki")` |
| `dlssnr/backend/Selector.cpp/.h` | `SubmissionHooksWanted()` also applies to mochizuki |
| `dlssnr/amd/AmdBridge.cpp` | `HasFiles()` and creation of `LmxxfBackend(..., L"MochizukiNrRuntime.dll")` |
| `dlssnr/backend/LmxxfBackend.cpp/.h` | DLL name as a parameter; lmxxf configuration only when `Lmxxf()`; `PrepareSession` inside `ScopedSkipVulkanHooks` + `ScopedCreatingD3DDevice`; mochizuki `UNAVAILABLE` only updates the status; status prefix `mochizuki` |
| `dlssnr/DlssNr_Menu.cpp` | combo with the installed runtimes; mochizuki section with status and cost |
| `Config.h` | `NrBackend` comment |
| `dlssnr/backend/README.md` | mochizuki section |
| `.gitignore` | `third_party/mochizuki/toolchain/` (downloaded glslang) |

Changes from waves 1 and 2 in OptiScaler (output, backoff, keys, menu, job cancellation, upscaler
barriers, dynamic resolution): §11.2 and §11.3.

---

## 5. Where everything is (in the repository)

| Path | What it is |
|---|---|
| `OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrRuntime.cpp` | the runtime (new) |
| `OptiScaler/dlssnr/backend/mochizuki_runtime/compat/dirent.h` | POSIX shim for MSVC (new) |
| `OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h` | the shared C ABI |
| `third_party/mochizuki/` | upstream pinned at `791b046` + `UPSTREAM.md` (new) |
| `tools/build-mochizuki-runtime.cmd` | build of the DLL and shaders (new) |
| `exports/mochizuki-runtime/` | build output, outside git, with `dlssnr-amd/` |
| `exports/mochizuki-harness/` | `mz_bench.cpp`, `mz_phases.cpp`, `mz_pan.cpp`, `build-harness.cmd`, outside git |
| `exports/cyberpunk-test/` | `install-mochizuki.ps1`, `remove-test.ps1` and the installed lists, outside git |
| `OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrControls.h` | control exports (`MochizukiNrSetControls`, `GetInfo`, `GetControlDefaults`, `GetFeatures`), wave 1 |
| `OptiScaler/dlssnr/backend/mochizuki_runtime/mz_interpose.cpp/.h` | pipeline capture, manifest and parallel prewarm, wave 2 (P3) |
| `tools/sweep_mochizuki_knobs.py` | sweep of the `pipelines.json` knobs on a copy of the upstream, wave 2 (P6) |
| `exports/mochizuki-work/` | plan, goldens, tools, reports (`wave1-report.md`, `wave2-report.md`) and outputs of each package, outside git |
| `exports/mochizuki-test-wave2/` | frozen snapshot for the in-game test (candidate for the first release), outside git |

---

## 6. How to reproduce

### 6.1 Runtime and shaders

In a Visual Studio "x64 Native Tools" prompt, with Vulkan SDK 1.4.357 or newer (`VULKAN_SDK`) and
Python on the PATH:

```
cd <REPO>
tools\build-mochizuki-runtime.cmd exports\mochizuki-runtime
```

The script downloads glslang 16.5.0 the first time (hash checked) and builds the DLL with the network
constants (the same as in `third_party/mochizuki/windows/build/arch/rdna4.sh`).

### 6.2 Model

Use `python`, not `python3` (on Windows, `python3` may be the Microsoft Store shortcut). `W` is a working
folder.

```
cd <REPO>\third_party\mochizuki\linux\package\model-tools
python inspect_nr.py <nvngx_dlssnr.dll 310.8.0> --output W\inventory --extract > W\inspect.json
copy descriptor.json W\graph\
python unpack_swin_family.py --artifacts W --output W\unpacked --max-c 256
python unpack_splitswin.py --artifacts W --output W\unpacked-splitswin
python unpack_vit.py --artifacts W --output W\unpacked-vit
python unpack_preblock.py W\inventory\weights\block0.layer0.layer.bin W\unpacked-preblock
python unpack_postblock.py W\inventory\weights\block70.layer0.layer.bin W\unpacked-postblock
python pack_model.py --root W --list model-files.txt --out dlssnr.bin --verify model-files.sha256
```

Expected result: `599 entries, 140.9 MiB`. Place it in `exports\mochizuki-runtime\dlssnr-amd\`.

### 6.3 Tests outside the game

```
cd <REPO>\exports\mochizuki-harness
build-harness.cmd
mz_bench.exe ..\mochizuki-runtime\MochizukiNrRuntime.dll 100
mz_phases.exe ..\mochizuki-runtime\MochizukiNrRuntime.dll 1920x1080:r16:40,1280x720:r11:40:2560x1440,2560x1440:r16:30
mz_pan.exe ..\mochizuki-runtime\MochizukiNrRuntime.dll 60 n,p,w,s
```

- `mz_bench`: cost, output different from input, no NaN.
- `mz_phases`: formats and resolution changes in one session.
- `mz_pan`: `n` no vectors, `p` correct vectors, `w` inverted sign, `s` static image. `p` must
  be much lower than `n` and `w`.

The DLL's `printf` output (static CRT) gets mixed with the harness's; the reliable results are in the
harness's own lines and in `mochizuki_nr.log` next to the DLL.

Since wave 1 there are also `mz_timing`, `mz_stress`, `mz_compare.py` and the equivalence and performance scripts
in `exports/mochizuki-work/tools/`; the exact procedures are in `exports/mochizuki-work/PROCEDURES.txt` and
the summary in §11.5.

### 6.4 OptiScaler

```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" OptiScaler.sln -p:Configuration=Release -p:Platform=x64 -m
```

### 6.5 Installing and removing in a game (test)

```powershell
$s = '<REPO>\exports\cyberpunk-test'
& "$s\install-mochizuki.ps1" -Game '<GAME_DIR>' -List "$s\installed-<game>.txt"
& "$s\remove-test.ps1"      -Game '<GAME_DIR>' -List "$s\installed-<game>.txt"
```

The installation copies the new OptiScaler as `dxgi.dll` (backing up the previous one), the `OptiScaler\` folder
of dependencies from the 0.3.0 package, `MochizukiNrRuntime.dll`, `dlssnr-amd\` and an `OptiScaler.ini` from the package
with `NrBackend=mochizuki` (only if there is no INI). In game: DLSS in the graphics options, Insert, `Enable NR`.

Warning (2026-09-25): `install-mochizuki.ps1` copies the runtime from `exports\mochizuki-runtime\`, which is still the build
from before waves 1 and 2, and does not copy `dlssnr-amd\prewarm\`. To test the wave 2 snapshot, copy
`exports\mochizuki-test-wave2\` by hand (`OptiScaler.dll` as `dxgi.dll`, `MochizukiNrRuntime.dll` and all of `dlssnr-amd\`,
with `prewarm\manifest.txt`), or use the snapshot install script once it exists (package D3).

---

## 7. Local state (development machine)

| Path | What it is |
|---|---|
| `E:\Projetos\decompile_nr\nvngx_dlssnr.dll` | model source (310.8.0) |
| `E:\Games\Cyberpunk2077\bin\x64\` | test installation; backup in `_optiscaler_backup_2026-09-24_1931build`; fixed runtime; list in `installed.txt` |
| `E:\Games\GTAVEnhanced\` | test installation (folder was clean); list in `installed-gtav.txt` |
| `C:\Users\matheus\Desktop\mochizuki-runtime-arquitetura.md` | detailed architecture with code and porting guide for other hosts |
| https://claude.ai/artifact/2kYTAUa8gJY3hKcZmjVwSD | this same record as a (private) page |

---

## 8. Problems found and solutions

| Problem | Cause | Solution |
|---|---|---|
| Author's host does not work on native D3D12 | depends on vkd3d-proton's `VkDevice` | own Vulkan device with D3D12 interop |
| Model extraction fails on Windows | bash script uses `/tmp`; `python3` is the Store shortcut | Python steps by hand with `python` |
| Error C2397 in MSVC | enum-to-flags conversion inside braces in `nr_runtime.cpp` | cast `VkImageAspectFlags(...)`, recorded in `UPSTREAM.md` |
| `dirent.h` missing | POSIX header in `telemetry.hpp` | shim in `compat/` |
| No `VK_EXT_shader_float8` | OptiScaler's Vulkan 1.4.310 headers | Vulkan SDK 1.4.357 |
| `Get-FileHash` unavailable | PowerShell called from inside `cmd` | glslang download and hash in Python |
| Runtime device intercepted | OptiScaler's hooks on `vulkan-1.dll` alter extensions and register the device | `ScopedSkipVulkanHooks` + `ScopedCreatingD3DDevice` in `PrepareSession` |
| Slow build again in another program | AMD pipeline cache separated per executable | expected, once per game |
| Cyberpunk without temporal history | vectors in `R16G16B16A16_FLOAT` rejected | 4-channel formats accepted; rejections in the log |
| `small` breaks the harness build | `windows.h` macro | different name |

---

## 9. Roadmap

### Phase 0: minimum version (done)

- Vulkan runtime with the lmxxf ABI, turned on and off by `Enable NR`, only before SR.
- Temporal history with the game's vectors, validated with `mz_pan`.
- Working in Cyberpunk 2077.

### Phase 1: validation in games (in progress)

- **GTA V Enhanced:** installed, awaiting testing (story mode, BattlEye off, DLSS in the options).
- Cyberpunk again with history: check `motion 1920x1080 DXGI 10` in the log and the image in motion.
- More D3D12 games with DLSS, including one that sends vectors at output resolution.
- Long sessions: TDR, VRAM, alt-tab, menus that change resolution, frame generation on.
- **Next step:** test in Cyberpunk the snapshot `exports/mochizuki-test-wave2/` (candidate for the first
  release) with the checklist from `exports/mochizuki-work/wave2-report.md` §7, which combines the open items of
  wave 1 (output during the build, controls, alt-tab, upscaler barriers) with those of wave 2 (first
  build with prewarm, network kept during the rebuild, dynamic resolution, VRAM, watchdog). None of this
  was run in game by the agents.

### Phase 2: controls (implemented; in-game test pending)

- Own keys `[DlssNr] Mochizuki*` (Config.h, Config.cpp, package INI, README). The mochizuki
  settings do not come from any danielblnc, lmxxf or NVIDIA key (`TransferStrength`,
  `ColourStrength`, `Passes`, `AmdModelScale`, `AmdDynamicScale`, `DlssNrRRWorkingScale` and the model ones).
  Every default is the network's, so an INI without these keys runs as before.
- `AmdModelScale` and `AmdDynamicScale` do not affect mochizuki: the stabilization wait in
  `AmdBridge::Run` ignores both for it (S5). The menu warning and button were removed.
- Dynamic resolution: `MochizukiDynamicResolution` (`auto`, default; `exact`; `always`) becomes the runtime's
  `drs_mode`. In `auto`, when the render subrect becomes smaller than the colour texture, the network runs in a bucket
  (per axis, the largest subrect seen, rounded up to 64 px, within the texture): changing the subrect within it
  only resets the history, without rebuilding the network or waiting for stabilization (the bridge's wait now
  looks at the colour texture). The bucket grows with a larger subrect and shrinks after 30 s of frames from the
  same colour texture with subrects at least 128 px smaller on both axes. `exact` rebuilds at every
  resolution, as before; `always` uses the bucket on every frame, also across texture reallocations. The runtime's
  own default (zeroed struct, `MochizukiNrGetControlDefaults`) is `exact`; it is the host that requests `auto`. A
  `drs_mode` above 2, or motion vectors in a format that does not allow blit, run as `exact` (the log says
  when the vectors prevent the bucket). The `drs_mode` comment in `MochizukiNrControls.h` describes the three modes.
- Per frame, in `LmxxfNrFrameInfo`: `MochizukiDetailStrength` (0 to 2), `MochizukiColourStrength` (0 to 4;
  lmxxf stays at 0 to 2; the host default is 0, the game's own colour in the network's luminance, while
  the runtime's is 1), `MochizukiModelScale` (0.25 to 1, in steps of 0.05) and `MochizukiPasses`
  (1 to 3). Scale and passes rebuild the network, which is built for exactly that number of passes.
- Through the `MochizukiNrSetControls` export (`MochizukiNrControls.h`): intensity, style, local tone, local
  structure, skin, automatic mask, highlight guard (`max_ratio`, 1 to 8), `set_history_strength`,
  `set_white_point` (fixed at `MochizukiWhitePoint`, 1 by default; no pre-exposure for now), linear
  input, apply model and the overrides for passes 2 and 3. The host only calls it when something changes or the session is new.
- `MochizukiTemporal` turns on history; without the key, `LmxxfTemporal` applies.
- Menu section with these controls; passes and resolution apply on slider release. The status shows the
  model extent, the network time (median and p95), the last build and warns when motion
  vectors are rejected.
- `EvaluateAtSeam` ignores `ApplyAfterRR` for lmxxf and mochizuki; the menu warns and offers a button to
  turn the key off.
- Pending in game: each slider changes the image live; passes and resolution rebuild once, on release; the
  status shows the network time. `MochizukiModelScale` below 1 is not yet recommended: in slow
  panning an alternating residue remains (see C1).

### Phase 3: quality and cost (partly done in waves 1 and 2)

- Done (§11): network cost −0.86 ms at 1080p and −1.23 ms at 1440p, bit-for-bit identical output (P4, P6, P9);
  first build 24.8 → ~6 s in the harness (P3); stability (S1-S5, H1, S8); dynamic resolution (S5).
- Suspended by the user for the first release: pre-filter in the core's downscale for `MochizukiModelScale`
  below 1 (C3; today there is flicker in slow panning), memory residency priority (P5) and optional
  prewarm at session start (P8).
- Candidates measured but not built (P18): fuse `gemmproj→gemmvqkvnorm` (0.15 ms of barrier at 1080p) and
  `vitattn→gemmproj` (0.04-0.10 ms).
- Guard for the persistent queue's 16-bit index (S9, in progress separately): only matters above ~21.8 MP of
  input (8K).
- Depth in `EngineFrame.depth` (improves edges in motion).
- Read the colour without assuming the resource state.
- Fewer copies: compute conversion in D3D12 or direct texture import.
- Support after SR, for games with Ray Reconstruction.
- Parity with Linux (6.2 ms) is not reachable without compiler parity (LLPC versus ACO).

### Phase 4: release

- Released in 0.4.0-amd-nr (2026-09-25):
  - the OptiScaler zip carries mochizuki support (backend, the 29 `Mochizuki*` keys in the INI, the menu), but not
    the runtime;
  - the installer (AMD-NR ReShade Installer) installs mochizuki like the other runtimes: `MochizukiNrRuntime.dll`,
    `dlssnr-amd\shaders\`, `dlssnr-amd\prewarm\manifest.txt` and the `dlssnr.bin` model, checked by hash;
  - the prewarm manifest must be regenerated whenever the shaders change.
- Pending: native Windows extraction tool (`tools/extract-mochizuki-model.*`, package D1), which does not
  exist yet.

### Phase 5: maintenance (ongoing)

- Track the upstream (born on 2026-09-24 and likely to change quickly); sync script like the lmxxf one
  (`tools/sync-mochizuki-upstream.ps1`, package D2, planned separately). The local patches to reapply are in
  `third_party/mochizuki/UPSTREAM.md` (entries 1-5, §11.6).
- On each update: rebuild and run the equivalence against the goldens (`tools/mz_equiv.py`), `mz_stress all`
  and `mz_perf.py --baseline` (§11.5) before installing; regenerate the prewarm manifest if the shaders change.

---

## 10. Pending decisions and risks

**User decisions:**
- Whether `NrBackend=auto` should pick mochizuki when it is installed.
- When to commit and in which version to release.
- Criterion waivers accepted for 0.4.0, recorded with numbers and reasons in
  `exports/mochizuki-work/release/WAIVERS.md`:
  - C1 (scale 0.5 and flicker at 0.75): only with `MochizukiModelScale` below 1 (default 1).
  - P3: adapters at 0.08 s on the second run, paid on every warm build also by default (startup
    cost only, the output does not change); and the exception to DECISIONS 7 under CPU starvation.
  - S5 (±30% band at 2304x1296; Q1 analysis pending): applies with the default `MochizukiDynamicResolution=auto`
    whenever the game's subrect is smaller than the colour texture.
  - The M0 measurement notes.
  The user can revoke any of them before the commit.
- When to resume C3, P5 and P8 (suspended for the first release).

**Risks:**
- GTA V Enhanced ships BattlEye: story mode only, with BattlEye off. Never in online play.
- First run of each game: ~55 s without NR while the pipelines compile (2026-09-24). With the prewarm
  manifest next to the DLL, 6 s in the harness (versus 21.6 s without it); in game not measured yet. Without the manifest the
  runtime compiles serially and writes it at the end.
- VRAM: the 1080p network takes ~0.7 GB more (698 MB measured by S2), in a separate Vulkan context. During
  a rebuild that keeps the old network, both coexist (the VRAM check counts both). The VRAM
  estimate was calibrated only on this GPU and this driver.
- A 250 ms slot wait timing out 3 times in a row counts as device loss and turns NR off until the end
  of the process; a legitimate ~1 s GPU stall could trigger it (not seen in testing). The watchdog
  declares the device lost after 5 s without progress on the Vulkan side.
- With `MochizukiDynamicResolution` set to `auto`, when the render resolution drops the network still costs the
  bucket size; the first time the subrect becomes smaller than the texture costs a `PrepareFrame` of ~12-13 ms.
- Input resolutions above ~21.8 MP (8K) trigger the 16-bit limit of the upstream's persistent queue
  (~0.5 s frame and wrong image) until S9 lands.
- The input copy assumes the reported colour state; if the game is in another one, the D3D12 debug layer
  complains.
- Upstream at 0.0.1, with the Windows tree described by the author as experimental.
- The mochizuki controls use only the `Mochizuki*` keys: a value tuned for danielblnc or lmxxf
  (`TransferStrength`, `Passes`, `AmdModelScale`) does not apply here, and vice versa. Exception: without
  `MochizukiTemporal`, `LmxxfTemporal` applies. Each change of passes, model scale or network
  size rebuilds the network; since wave 2 (S4) passes and scale keep frames on the old network during the
  rebuild, and only a size change still leaves one or two seconds without NR. More passes cost more
  VRAM. The network size is the render resolution with `MochizukiDynamicResolution=exact`; with `auto`
  (default) or `always` it is the bucket, which only changes when a larger subrect arrives, when the colour texture changes
  or when it shrinks (see §9).

---

## 11. Waves 1 and 2 (2026-09-24/25)

Work done by agents in packages, with a plan, independent review of each package and integration at the end of
each wave. Plan and user decisions in `exports/mochizuki-work/plan/` (`plan.md`, `DECISIONS.md`,
`WAVE2.md`); integration reports in `exports/mochizuki-work/wave1-report.md` and `wave2-report.md`.

Rules followed: nothing committed, nothing installed in game; every change to the runtime or shaders must keep the
output **bit-for-bit identical** to the 2026-09-24 goldens with default values; performance decided by interleaved
pairs in the same session (5 pairs), never by the status average.

| Wave | Packages | Result |
|---|---|---|
| 1 (2026-09-24 evening to 2026-09-25 04h) | M0 harness and goldens; S1 runtime hygiene; C1 controls in the runtime; H1 lifecycle in the host; C2 keys, menu and docs; S8 job cancellation; S6 upscaler barriers; S7 Vulkan hooks; P1 ISA kit; P2 sweeps; P4 word reads | integrated; snapshot `exports/mochizuki-test-wave1/` |
| 2 (2026-09-25) | S2 errors and recovery; S3 cross-API synchronization; P3 parallel prewarm; S4 build off the frame path; S5 dynamic resolution; P6 LLPC knobs; P9 codegen cleanup; P7 unroll options (nothing adopted); P18 barriers (nothing adopted) | integrated; snapshot `exports/mochizuki-test-wave2/`, candidate for the first release |
| suspended by the user | C3 stability with model scale below 1; P5 memory residency; P8 optional prewarm at session start | for the first release |

### 11.1 Final integration (2026-09-25, 17h20-17h50)

| Check | Result |
|---|---|
| Runtime build | BUILD_OK, 0 warnings; the DLL imports only `vulkan-1.dll` and `KERNEL32.dll` |
| Equivalence with the goldens | exact (`mz_bench` 1080p x3 and 1440p, `mz_phases`, `mz_pan`); also with `drs_mode=1`, the host default |
| `mz_stress all` | 21 of 21 cases; with dynamic resolution in `auto` (S5 copy with `drspad`), 22 of 22. With the harness's `mz_stress` already including dynamic resolution (RL2): 21/21, and 22/22 in `auto` and in `always` |
| OptiScaler build (Release x64) | 0 errors; 32 warnings, all pre-existing |
| clang-format 20.1.8 | 0 violations in the 21 changed C/C++ files |
| Keys and defaults | 29 `Mochizuki*` keys checked; host defaults equal to the network's |
| Unexpected changes | none: 33 paths changed since the 2026-09-24 backup, all from some package |

### 11.2 Controls, keys and menu

All in `[DlssNr]`; the default of each one reproduces the 2026-09-24 output. Details of each control in §9, Phase 2.

| Key | Default | Range and effect |
|---|---|---|
| `MochizukiTemporal` | true (without the key, `LmxxfTemporal` applies) | temporal history |
| `MochizukiHistoryStrength` | 1 | 0 to 1, live |
| `MochizukiDetailStrength` / `MochizukiColourStrength` | 1 / 0 | 0 to 2 / 0 to 4, live; colour 0 keeps the game's colour, 1 applies the network's colour |
| `MochizukiPasses` | 1 | 1 to 3, rebuilds (the old network serves during the build) |
| `MochizukiModelScale` | 1 | 0.25 to 1 in steps of 0.05, rebuilds; below 1 not yet recommended |
| `MochizukiIntensity`, `MochizukiStyle`, `MochizukiLocalTone`, `MochizukiLocalStructure`, `MochizukiSkinStructure`, `MochizukiAutoMask` | 1, 0, 1, 1, −1, true | model controls, live |
| `MochizukiMaxRatio` | 2 | 1 to 8, highlight protection |
| `MochizukiWhitePoint` | 1 | 0.01 to 100, linear colour only, INI only |
| `MochizukiApplyModel` | true | false: runs the network and shows the original frame |
| `MochizukiLinearInput` | 0 | 0 auto, 1 yes, 2 no; rebuilds |
| `MochizukiDynamicResolution` (wave 2) | `auto` | `auto`, `exact`, `always` |
| `MochizukiPass2*` and `MochizukiPass3*` (12 keys) | `auto` | inherit pass 1, with local tone 0 |

Runtime environment variables (not INI keys): `MZ_COMPILE_THREADS` (prewarm threads; default
min(8, cores − 2); 1 disables), `MZ_PROBE_PIPELINE_BINARY=1` (pipeline binary key probe) and the
`MZ_TEST_*` failure hooks used by `mz_stress` (read once, no cost when absent).

Menu (Neural → "DLSS Neural Rendering", mochizuki runtime): warnings for `ApplyAfterRR` (with a "Run before Super
Resolution" button) and for Ray Reconstruction; Temporal ("Temporal history", "History strength"); Effect ("Passes", "NR
resolution (% of render)", both on release; "Detail strength", "Colour strength", "Highlight guard"); Model
("Style", "Intensity", "Local structure", "Local tone", "Skin structure", "Auto skin mask", "Pass 2/3" node);
Advanced ("Linear input", "Dynamic resolution" (wave 2), "Network only (no effect)"); Status (network extent,
GPU time median and p95, last build, history use, rejected vectors, "Failed: ..."). The
`AmdDynamicScale` warning and button were removed in wave 2.

### 11.3 Stability

| Problem (2026-09-24) | Fix | Package |
|---|---|---|
| Session start failure leaked the Vulkan instance and D3D12 references every frame, with no wait | start undone without leaks; host waits 2 to 60 s between attempts; `[unsupported]` logged once | S1, H1 |
| `done[]` fences stayed signaled after a `Drain` | `vkResetFences` | S1 |
| Exiting the game during the build hung for up to ~55 s | on process exit the host calls neither `Destroy` nor `FreeLibrary`; outside of exit, `Destroy` waits at most 2 s and the build finishes on its own; the core can stop between pipelines saving the cache | H1, S4 |
| Any error turned NR off until the end of the process | only device loss is permanent; a failing frame shows the original colour (never black); a failing build retries | S2 |
| No VRAM check | admission before building (D3DKMT), "insufficient VRAM", retry every 10 s; the network is freed if the frame buffers do not fit | S2 |
| A single fence in both directions could go backwards; the game queue got stuck if Vulkan died | two fences; watchdog (5 s without progress); game queues released on device loss | S3 |
| Queue switch destroyed the session on the submit thread; `Retire` outside the lock | `ANY_QUEUE` (no migration, the new queue's frame waits for the last one of the previous queue); `Retire` under lock; job handles with generation | H1, S2, S3 |
| Resolution change drained the game queue on the render thread (21-31 ms) | 0.4-0.6 ms; old network freed by a thread; passes and scale rebuild with the old network serving; second Vulkan queue for building | S4 |
| Dynamic resolution rebuilt the network on every change | bucket in `auto`: 0 rebuilds, NR on 100% of frames in the test | S5 |
| Resources freed under an orphan list not yet executed | "graveyard" freed later | S2 |
| Pending job from a never-executed list turned NR off | cancelled on the list's `Reset`/`Release` | S8 |
| Path with non-ANSI characters made the build fail | UTF-8 root | S1 |
| `frameId` advanced on rejected calls (history reset every frame with `DlssNrPreUpscale`) | deduplicated | H1 |
| XeSS, FSR 2.2, FSR 2.1.2 and FSR 3.1 barriers touched the NR output texture | they no longer do | S6 |
| OptiScaler's Vulkan hooks on the runtime's instance and device (Proton, overlays) | pass straight through | S7 |
| `ApplyAfterRR` put mochizuki after SR; `AmdModelScale`/`AmdDynamicScale` reset the history | ignored for mochizuki | C2, S5 |

### 11.4 Performance (RX 9070 XT, 26.8.1; same session, interleaved pairs)

| Measurement | 0.3.0 (2026-09-24) | Wave 1 | Wave 2 | Total |
|---|---|---|---|---|
| Network interval on the game queue (`mz_timing`), 1080p | 10.23 ms | 9.80 ms | **9.39 ms** | −0.86 ms (−8.4%) |
| Same, 1440p | 17.32 ms | 16.76 ms | **16.10 ms** | −1.23 ms (−7.1%) |
| Network only (`nr_graph --per-layer`), 1080p | 9.69 ms | 9.27 ms | **8.89 ms** | −0.80 ms |
| Same, 1440p | 15.82 ms | 15.26 ms | **14.65 ms** | −1.17 ms |
| "network" status, 1080p / 1440p | 9.80 / 16.70 ms | 9.31 / 16.14 ms | 8.9 / 15.5 ms | |
| First build in the harness | 24.8 s | 23.2-23.7 s | **5.8-6.0 s** with the manifest; 21.6 s without | −76% |
| `PrepareFrame` on resolution change | 31.0 / 21.6 ms | same | **0.44 / 0.57 ms** | |
| `EnqueueHip` CPU | 0.098 ms | same | same | |

- Where the network gain comes from, always with identical output: P4 (wave 1: e4m3 weights read in words), P6
  (`fswin32` with `NR_FWAVES=2`; `fswinfusedup128` with `NR_EXPAND_GROUP=2`) and P9 (e4m3 conversions in quads,
  `NR_QUAD`: `s_setreg` 7134 → 4158; V blocks in RowMajor, `NR_V_ROW`: `ds_load_u8` 512 → 0).
- First build: 32 pipelines compiled on 8 threads in ~4.3 s from
  `dlssnr-amd/prewarm/manifest.txt`; the cache now includes adapters and temporal ones (P3). Not measured in game.
- Not adopted: P7 (`--private-index` builds in 18.6 s, but the network becomes +3.3 ms slower and the output changes); P18
  (the upstream's `NR_CHAIN` times out on every frame; the safe forms cost +0.2 to +0.8 ms; persistence at
  C=32 is slower and wrong at 4K); N1 from P6 (`fswinpds256` without spill, but 12.7% slower).
- The `mz_phases` status number varies a lot between sessions (16 or 25 ms at 1440p for the same DLL): it is the
  GPU clock in that harness, as in `mz_pan`. Compare only `mz_timing`, `mz_bench` and `nr_graph`.

### 11.5 New tools (in `exports/`, outside git)

- `mochizuki-harness/`: `mz_bench`, `mz_phases` (dynamic resolution tokens `alloc=` and `sub=`), `mz_pan`,
  `mz_timing` (times on the D3D12 queue: seg1, interval, seg2), `mz_stress` and `mz_compare.py`; all accept
  `--ctl`, UTF-16 paths and `--dump-dir`. `mz_stress` runs each case in a child process: drain, drainval,
  cancel, leak, leakfail, unsupported, enumfail, build-destroy, rebuild-serve, busy, retire-late, orphan,
  timeout, enqthrow, oombuild, vram, vramband, lost, xq, drop and rerelease (`all`, 21 cases); `drspad` only by name.
  With `MZ_STRESS_DRS_MODE=1` (auto) or `2` (always) each session gets that `drs_mode` and `busy` expects the
  subrect change to keep the network (adopted from the S5 copy in package RL2, 2026-09-25).
- `mochizuki-work/golden/`: reference outputs from 2026-09-24 (bit-for-bit), timings, cold build and the
  `mz_stress` result on 0.3.0.
- `mochizuki-work/tools/`: `mz_equiv.py` (exact equivalence in one command, ~30 s), `mz_perf.py` (interleaved
  pairs with `--baseline`), clang-format 20.1.8 (venv `cf`) and RGA.
- `mochizuki-work/isa/` (ISA kit: `dump-isa.cmd`, `isa_count.py`), `sweeps/` (P2), `nrgraph/` (standalone `nr_graph`
  and plans), `P4/ngbench.py` (interleaved `nr_graph` with image check).
- In the repository: `tools/sweep_mochizuki_knobs.py` (sweep of `pipelines.json` knobs).
- Exact procedures (locks, builds, equivalence, performance, cold build, stress):
  `exports/mochizuki-work/PROCEDURES.txt`.

### 11.6 Patches to the upstream (`third_party/mochizuki/UPSTREAM.md`)

1. `nr_runtime.cpp`: cast to `VkImageAspectFlags` (2026-09-24).
2. `fswin_t.comp`: e4m3 weight reads and persistent-run writes in words (P4).
3. `pipelines.json`: `fswin32` `NR_FWAVES` 1 → 2 and `fswinfusedup128` `NR_EXPAND_GROUP` 4 → 2 (P6).
4. `fswin_t.comp`, `include/coopmm.glsl` and `pipelines.json`: `NR_QUAD` and `NR_V_ROW` (P9).
5. `nr_runtime.hpp` and `nr_graph.cpp`: `nr::build_cancel`, stopping the build between pipelines, saving the cache
   first (S4).

Record-only sections: "## Build" (P7: unroll variants and `spirv-opt`, none adopted) and "## Barriers and
`NR_CHAIN`" (P18: measurements, fusion candidates, and the 16-bit limit of the persistent queue index). Patches
2 to 4 are candidates for upstream PRs (identical output; not checked on RADV/ACO).

### 11.7 What is left

**In-game tests (user):** the full list, in Portuguese, is in `exports/mochizuki-work/wave2-report.md`
§7 (29 items) and combines the open items of wave 1 with those of wave 2. The main points: first build with the
manifest and output during the build; live controls; passes and resolution rebuilding without a gap without NR;
resolution changes without stutter; a game with dynamic resolution; alt-tab; VRAM at the limit; no "watchdog" in the
log during normal play; upscaler barriers with the D3D12 debug layer (S6).

**Open items:**
- Criterion waivers: recorded in `exports/mochizuki-work/release/WAIVERS.md` (package RL2, 2026-09-25): C1
  (scale 0.5; flicker at 0.75, fix deferred to C3), P3 (adapters at 0.08 s on the second run,
  versus 0.05; exception to DECISIONS 7), S5 (±30% band at 2304x1296; the bucket output is identical to the exact network's
  on the frame with extended edges; the Q1 report is missing) and M0. The user can revoke any of them.
- Texts fixed by RL2 (2026-09-25, comments only): `drs_mode` in `MochizukiNrControls.h` (modes 0, 1 and 2), the
  colour range in `LmxxfNrApi.h:87` (0 to 2 in lmxxf, 0 to 4 in mochizuki) and a comment in `LmxxfEvaluateCut.h`
  explaining that the "output zeroed" in the queue-switch message applies only to lmxxf (the message itself, which goes
  to `OptiScaler.log`, did not change). The "about half a minute" in the menu and status is still outdated,
from before the prewarm.
- `PROCEDURES.txt` §8 and §9 now describe the 21 cases, the mandatory `xq`, the cases from S1 to S5, the
  `mz_phases` dynamic resolution tokens and the S5 key and default checks; `build-mochizuki-runtime-stats.cmd`
  (in `exports/mochizuki-work`) links again (P3 patch applied by RL2).
- VRAM estimate calibrated on a single GPU; a `CreateGeometry` failure for another reason keeps the network
  resident; a retired network waits for the next frame with no time limit (0.3 to 0.9 GB if the game stops
  producing frames).
- One build at a time in the process; `Destroy` 1 s after the start of a cold build returns in 2.0 s
  (limit 2.5).
- Tight margins in the harness (wave2-final and the three clean RL2 rounds): `oombuild` ready in 6.6-6.9 s
  (limit 7), `drop` and `rerelease` in 5.2-5.5 s (limit 6).
  With Cyberpunk open on the same machine, `oombuild` took 7.5 s and failed (RL2, 2026-09-25): run `mz_stress` with
  the machine idle.
- 16-bit limit of the persistent queue (S9, separate) and the P18 fusion candidates.
- Nothing tested in game: real TDR, game queue switch, D3DKMT under OptiScaler's spoofing, first build
  in game with the manifest.

## 12. Performance: overnight round of 2026-09-27 (not yet released)

Nine changes coming from upstream v0.0.2 (`4f62a8a`) and v0.0.2.1 (`4f663b5`), which only touched the `linux/` tree
(tuned on RADV). Each one went in only with bit-for-bit identical output and a gain on LLPC; the full record, with the
numbers for each one and what was left out, is in `third_party/mochizuki/UPSTREAM.md`, entries 6 to 14.

| Change | Where |
|---|---|
| Keys `ffwd_gmajor`, `ffwd_fm2_min`, `persist_one` in `shader-constants.txt` | `nr_graph.cpp`, `nr_shader_manifest.hpp` |
| Upstream `gemm1x1.comp` and `ffwd3_t.comp`; `NR_QKV_NO_BCAST` and `NR_POOL_FRAG` (with `precise`) | shaders |
| Group-major `ffwd3` and the new `ffwd3w` from 2560 tokens (1440p and up) | shaders, `nr_graph.cpp` |
| Upstream attention; `NR_ATTN_VFRAG`, `NR_ATTN_QKSWAP`, `NR_ATTN_EDGE`, `NR_VOUT_VEC` | shaders |
| C=128 persistent runs fold DS and UPS (`fswinpds128`, `fswinpup128`; masks 6) | `pipelines.json`, `nr_graph.cpp` |
| One workgroup per item in the C=256 run that starts at UPS (1080p and 720p) | `fswin_t.comp`, `nr_graph.cpp` |
| Ping-pong temporal history with one pass (no per-frame copy) | `nr_runtime.cpp` |
| Linear path: the frame goes straight to the `keep` image in its own format (no RGBA32F copy) | `nr_runtime.cpp`, host |
| Alpha pass only with mask, native composition or 8-bit frame | `nr_runtime.cpp` |

Measured on the RX 9070 XT, driver 26.8.1, against the `5fead302` runtime, interleaved pairs in the same session:
- `mz_timing` (median of 7 pairs): 9.641 → 9.158 ms at 1080p (−0.577 ms, −6.0%) and 16.003 → 14.903 ms at
  1440p (−1.099 ms, −6.9%). The independent check, with a rebuild from the patch, measured −0.493 and −0.484 ms
  at 1080p and −1.096 and −1.031 ms at 1440p in two more sessions of 7 pairs; all 28 pairs were faster.
- `nr_graph --per-layer` (3 runs): 10.396 → 9.872 ms and 15.127 → 14.780 ms (the three changes outside the network
  do not show up here; on their own they add up to −0.27 and −0.61 ms).
- Identical output: EQUIV against the goldens, `nr_graph --out-image` at 1080p, 1440p and 4K, and runtime dumps with 2
  and 3 passes, scale 0.5, no motion, strength, variable alpha, RGBA32F, R10G10B10A2, odd sizes and dynamic
  resolution (auto and always).
- `mz_stress`: `all` 21/21, and 22/22 with `MZ_STRESS_DRS_MODE=1` and `=2`. With the CPU busy, `oombuild` exceeds the
  7 s limit on both DLLs (the new one and the previous one), as already happened.
- Cold build at 1080p: 22.7 and 27.7 s versus 22.5 and 27.2 s without the manifest; 8.0, 7.4 and 6.6 s versus 7.6,
  7.0 and 6.6 s with it.
- VRAM: +16 MB at 1080p, +28 MB at 1440p, +64 MB at 4K.

To publish: 42 network pipelines, 4 temporal and 7 runtime ones (previously 39, 4 and 6). The installer's
`payload.json` needs the four new SPVs (`g_ffwd3w`, `g_fswinpds128`, `g_fswinpup128`,
`runtime/runtime_encode_in`) and the new hashes; without `ffwd3w`, the network for 1440p and up does not build. The
prewarm manifest must be redone (31 pipelines in a cold run at 1080p) and the harness's `nr_graph.exe`
must be rebuilt. The in-game test is pending. Integration record, with every command and number:
`exports/mochizuki-work/night-perf/INTEGRATION.txt`.

Rounding of the pre-block input (`UPSTREAM.md`, entry 15), together with the nine changes above: the
pre-block converts the colour sample to half precision, and LLPC folded that conversion into a 16-bit texture
return that truncates the f32 texel. Now the bits are rounded to nearest even before the
conversion, like the original network's `cvt.rn.f16.f32`. This is the only output change, on purpose: mean
|d| from 1,1e-3 to 4,8e-3 against the M0 goldens, cost +0.001 ms. The integrated build (the nine changes plus this one)
gives byte-for-byte identical EQUIV to the build that has only this change, and the goldens for that output are in
`exports/mochizuki-work/golden-2026-09-27`.
