# OpenNR OptiScaler

An [OptiScaler](https://github.com/optiscaler/OptiScaler) fork for AMD Radeon cards, part of the OpenNR project. Games
that ask for DLSS get NVIDIA's DLSS 5 neural rendering (NR) networks and DLSS Ray Reconstruction, run on AMD hardware
by open and community runtimes. Everything else OptiScaler does (FSR, XeSS, frame generation, spoofing) is still here.

This README covers what the fork adds. For OptiScaler itself, see the
[upstream project](https://github.com/optiscaler/OptiScaler) and its wiki.

Discord: <https://discord.gg/wYhvS3JSHM>

Ko-fi: <https://ko-fi.com/T6T213OVFE>

## Supported GPUs

| Feature | Radeon cards |
|---|---|
| NR, danielblnc 0.6.0 (HIP) | RX 6000, RX 7000, RX 9000 with HIP 7 (RX 6000 needs AMD's HIP SDK 7.2) |
| NR, danielblnc 0.8.0 (NeuralHub host) | RX 7000, RX 9000 |
| NR, lmxxf 0.45 | RX 9070 and RX 9060 series |
| NR, mochizuki 0.0.8.2 (Vulkan) | RX 9000; RX 7000, RDNA3 APUs and RX 6000 (INT8) on experimental networks |
| Ray Reconstruction, OpenRR 0.1.6 | RX 9000 (RX 7000 and RDNA 3.5 experimental), D3D12 and Vulkan games |
| Ray Reconstruction, FSR-RR (AMD Ray Regeneration) | RX 9000 |

## Install

The easy way is the [OpenNR Installer](https://github.com/zmodelerlover/OpenNR-Installer/releases). It finds your
games, downloads this fork, the runtimes and their weights, and can take it all back out.

By hand:

1. Extract the release next to the game's executable (for Cyberpunk 2077, `bin\x64`).
2. Put the runtime files you want next to `Setup.bat`: danielblnc's `version.dll` or `dlssnr_on_amd_setup.exe` (and
   `dlssnr_on_amd_weights.bin` if you have it), or lmxxf's `native-game-tiled-assets\`. mochizuki and OpenRR are set
   up by the installer.
3. Close the game, run `Setup.bat`, pick the game folder and a proxy name (`dxgi.dll` is the usual one).
   `Uninstall_OptiScaler_NR.bat` in the game folder removes it.
4. In game, press **Insert**, open the **Neural** tab and turn on **Enable NR**. Pick the runtime under
   **NR runtime** (applies on the next launch).

No runtime's weights ship in this repository or its releases. They come from NVIDIA's models and are made on your PC
or downloaded by the installer.

## Features

- **Three NR runtimes**, picked per game with `NrBackend` in `[DlssNr]`: danielblnc's
  [DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD) (0.6.0 on HIP, or 0.8.0 as NeuralHub's host, which
  also brings its DLSS-SR, DLSS-RR and DLSS-FG to AMD), [lmxxf's HIP port](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)
  and [mochizuki0323's Vulkan port](https://gitlab.com/mochizuki0323/DLSSNR-AMD).
- **NR before upscaling, or on the final image** in games without an upscaler (D3D9, D3D11, D3D12, Vulkan, OpenGL).
- **OpenRR**: our own DLSS Ray Reconstruction runtime (MIT), built from the game's `nvngx_dlssd.dll` 310.9.1. When it
  can't run, Ray Reconstruction falls back to FSR-RR. In D3D12 games mochizuki runs on its denoised image by default. `Mode` picks New (default, cheaper kernels, about 8% faster at a small measured loss of precision) or Legacy. Settings in
  `[OpenNR-RR]`.
- **Foveated NR** (experimental, off by default, `Foveation`): the network runs at full density in the centre and lower
  toward the edges, about 15 to 35% less neural pass time. mochizuki before the upscaler, also with OpenRR in D3D12
  games; mochizuki and danielblnc on the final image in D3D11 and D3D12.
- **FSR SDK 2.4**: FSR 4.1.2 upscaling, frame generation 4.0.2 and Ray Regeneration 1.2.1 as FSR-RR
  (`Dx12Upscaler=fsr-rr`).
- **XeSS multi frame generation** up to 8X on AMD cards (`FGOutput=xefg`).
- **RenoDX colour composition** after the runtime (`RenoComposition`, off by default) and **person protection**
  (`NrPersonPartition`, off by default).
- **32-bit games**: D3D9, D3D10, D3D11 and OpenGL through the proxies in `x86\` and a 64-bit host
  ([tools/dx9-x86/README.md](tools/dx9-x86/README.md)).
- An NR toggle key (Numpad 5), a neural pass timer on the FPS overlay, an optional "NR ON" badge, and a screenshot key
  (F6) that saves the frame with NR applied.

Every key is documented in `OptiScaler.ini`, next to its default. On NVIDIA cards the fork can drive NVIDIA's own
DLSS-NR instead; see [INSTALL-DLSSNR.md](INSTALL-DLSSNR.md).

## Building

Visual Studio with the v143 toolset, `Release|x64`, after `git submodule update --init`. Keep `tools\` beside the
solution, a pre-build step runs from it.

- `tools\build-lmxxf-runtime.cmd exports\lmxxf-runtime` builds the lmxxf runtime.
- `tools\build-mochizuki-runtime.cmd exports\mochizuki-runtime` builds the mochizuki runtime (MSVC prompt, Vulkan SDK
  1.4.357 or newer, Python).
- `tools\build-person-worker.cmd` builds the person protection worker.
- `tools\PACKAGE_RELEASE.ps1` makes the release zip.

OpenRR is built from its own repository. CI checks formatting with clang-format 20 under `OptiScaler/`.

## Credits

- [OptiScaler](https://github.com/optiscaler/OptiScaler) by cdozdil and contributors, which everything here is built
  on. It uses [FreeType](https://gitlab.freedesktop.org/freetype/freetype) under the FTL.
- [Dagherbou/OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR), which first put DLSS-NR into
  OptiScaler, and [wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass),
  which added the pre-SR multipass pipeline.
- [MatheusGViana](https://github.com/MatheusGViana/dlss-5-amd-project) and
  [TheAutomatic](https://github.com/TheAutomatic/dlss-5-amd-project): the AMD bridge, multi-slot scheduling, the D3D12
  state freeze and restore, the lmxxf integration, and person protection.
- [danielblnc/DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD): the closed-source runtime this bridges
  into. It is not reimplemented or redistributed here.
- [lmxxf/dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) (MIT), vendored under
  `third_party/lmxxf`.
- [mochizuki0323/DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD) (MIT), vendored under `third_party/mochizuki`.
- [burak113/OptiScaler](https://github.com/burak113/OptiScaler/tree/ffx-denoise-experimental): FSR-RR.
- [Coldwood1026/OptiScalerDp4aUnlock](https://github.com/Coldwood1026/OptiScalerDp4aUnlock): the XeFG multi frame
  generation unlock and pacing. YiBoF added its Intel Arc support.
- [RenoDX](https://github.com/clshortfuse/renodx) by clshortfuse: the colour composition in `dlssnr.hlsl`.
- [FoveatedDLSS5](https://github.com/maximedgoulet/FoveatedDLSS5) by maximedgoulet (MIT): the warp behind foveated NR.
- Person protection models: PP-HumanSeg (Apache-2.0) and YuNet (MIT) from the OpenCV Zoo, Ultralytics YOLO11n-seg
  (AGPL-3.0), run by ONNX Runtime (MIT). See `third_party/onnxruntime/README.md`.

NVIDIA's DLSS binaries and model weights are not redistributed. FidelityFX, XeSS and the DirectX Agility SDK keep
their own licences, in `Licenses/`.

## License

GPL-3.0, the same as OptiScaler. See `LICENSE`.
