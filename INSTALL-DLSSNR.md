# Install DLSS Neural Rendering

This fork is experimental. Do not use injection mods in anti-cheat-protected multiplayer games.

## Requirements

- A 64-bit game whose temporal upscaler reaches an OptiScaler D3D12 path. Native D3D12 is preferred;
  supported D3D11 and Vulkan games can use OptiScaler's D3D12 bridges.
- NVIDIA driver 616.56 or newer.
- The complete release archive from this repository. It includes `setup_windows.bat`, the
  `OptiScaler` backend folder, `OptiScaler.dll`, `OptiScaler.ini`, and `nvngx.dll_dlssnr.dll`.
- A separately obtained `nvngx_dlssnr.dll` 310.8 runtime appropriate for the GPU.

The two similarly named files are different and both are required:

| File | Purpose |
|---|---|
| `nvngx.dll_dlssnr.dll` | Open-source forwarder supplied by this project |
| `nvngx_dlssnr.dll` | NVIDIA-derived Neural Rendering runtime supplied separately by the user |

## Choose the correct runtime

| GPU | Runtime | SHA-256 |
|---|---|---|
| RTX 50 | Original NVIDIA-signed 310.8 | `E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E` |
| RTX 20 / 30 / 40 | ShortFuse cross-generation 310.8 | `E67DEE209320CDAFE0E93E45675D7AA34323A53ACC57A72B2E40A181581C989A` |

For RTX 20/30/40, obtain the compatibility runtime from
[ShortFuse's pinned RenoDX thread](https://discord.com/channels/1408098019194310818/1543976771920330884).
The compatibility DLL automatically selects an FP16-oriented path on RTX 20/30, an Ada-compatible
path on RTX 40, and leaves the RTX 50 path unchanged.

The compatibility runtime is modified, so Windows reports the original NVIDIA signature as invalid.
That is expected for this exact hash, but it removes the assurance provided by Authenticode. Keep
security protection enabled, use only the pinned developer attachment, and verify the SHA-256 value:

```powershell
Get-FileHash .\nvngx_dlssnr.dll -Algorithm SHA256
```

## Install

1. Close the game and its launcher.
2. Find the directory containing the real game executable, which is often below the game's root.
3. Back up any existing proxy DLL, `OptiScaler.ini`, and OptiScaler installation.
4. Extract the entire release archive into that executable directory. Do not copy only the two DLLs.
5. Put the correct `nvngx_dlssnr.dll` from the table above in the same directory.
6. Run `setup_windows.bat`. It renames `OptiScaler.dll` to a proxy filename the game will load and
   creates an uninstaller. `dxgi.dll` is the usual first choice. The validated Cyberpunk 2077 setup
   used `dbghelp.dll` to coexist with its existing loaders.
7. Enable NR in the `Insert` overlay, or edit `OptiScaler.ini`:

```ini
[DlssNr]
Enabled=true
RunBeforeSR=true
Passes=1
WorkingScale=1.0
```

Begin with one pass. RTX 20/30 use a much heavier FP16 path, so reduced model resolution may be
necessary. With `RunBeforeSR=true`, DLSS Performance at 3840x2160 gives the model a 1920x1080 input
before Super Resolution. `WorkingScale=0.5` lowers only the model's work resolution further.

For a portable setup, leave the process filter disabled:

```ini
[ProcessFilter]
TargetProcessName=auto
```

Do not copy an INI containing another game's executable name. A mismatch intentionally puts
OptiScaler into pass-through mode, which means no menu and no Neural Rendering.

## Game notes

- **Baldur's Gate 3:** install beside `bg3.exe` / `bg3_dx11.exe` in `Baldurs Gate 3\bin`.
  Use `Dx12Upscaler=dlss` for `bg3.exe`, or `Dx11Upscaler=dlss_12` for `bg3_dx11.exe`.
- **Hogwarts Legacy:** install in `Phoenix\Binaries\Win64`; `dxgi.dll` was validated.
- **Cyberpunk 2077:** install in `bin\x64`; `dbghelp.dll` was validated on the development machine.
  Existing CET/RED4ext/ReShade loaders can require a different proxy or correct chaining.

Do not install the RenoDX DLSS add-on merely to obtain its compatibility runtime. This OptiScaler
fork drives `nvngx_dlssnr.dll` itself, and two Neural Rendering injectors can conflict.

## Individual pass controls

Under **DLSS Neural Rendering → Model passes**, expand Pass 1, Pass 2, or Pass 3. Each contains
Style, Intensity, Local structure, Local tone, Skin structure, and Auto skin mask. Sliders commit
when released to avoid rebuilding the model on every movement. Set the model pass count to 2 or 3
to activate later passes; editing inactive passes prepares their settings without running them.

Later passes inherit pass 1 unless overridden, except Local tone, which defaults to 0 to preserve
the earlier build's appearance. Reset on a later-pass slider clears its override. Intensity,
local structure, and local tone range from 0 to 2; skin structure ranges from -1 to 2, with -1
following local structure. The corresponding INI keys are `Pass2Intensity`, `Pass2LocalStructure`,
`Pass2LocalTone`, `Pass2SkinStructure`, and `Pass2AutoMask`, with matching `Pass3...` keys.
Use `auto` for the default behavior. Styles retain `Pass2Style` / `Pass3Style`.

These controls apply to D3D12 multipass and its bridges, both before/after SR and after native RR.
Native Vulkan and the driver-proxy backend remain single-pass. Preset hints are still transmitted
at model creation, but a changed hint is not proof of a changed model. They are preserved under
**Advanced preset hints (effect unverified)** and in the INI for compatibility.

## Padded DLSS input sizes

With a build containing the padded pre-SR fix and `RunBeforeSR=true`, an origin-zero 2558x1439 image
inside a 2560x1440 colour texture runs NR at 2558x1439 when `WorkingScale=1`. The padding is not
processed or overwritten. This does not change the game's DLSS preset ratios. Non-zero colour
offsets and invalid rectangles still fall back after SR. The older release downloads do not gain
this fix through an INI change; the loaded OptiScaler proxy DLL must be updated.
See [validation and reporting instructions](docs/PADDED-PRESR.md).

## Neural Rendering with native Ray Reconstruction

In a game that already supports RR, enable RR in the game's settings and enable
**Apply after Ray Reconstruction (DX12)** in OptiScaler's Neural Rendering menu. The master
**Enable NR** switch must also be on. Equivalent INI settings:

```ini
[DlssNr]
Enabled=true
ApplyAfterRR=true
RRPasses=1
RRWorkingScale=0.5
```

RR reconstructs and upscales first. NR then processes that output before frame generation.
`RunBeforeSR` does not override this order. At 4K output, `RRWorkingScale=0.5` runs NR at
1920x1080 and resizes its edit for composition; it does not reduce RR's own resolution.
`RRPasses=1..3` is independent of ordinary `Passes`, while per-pass model profiles are shared.
Switching between SR and RR rebuilds NR history even when their dimensions match.
These controls apply to D3D12 and its bridges, not the upstream native Vulkan NR path.

If Cyberpunk's RR option is greyed out with this fork, avoid the `d3d12.dll` proxy: an
[upstream report](https://github.com/Dagherbou/OptiScaler_DLSSNR/issues/8) confirmed that using
`dxgi.dll` resolved a Streamline conflict. Back up existing loaders before changing the proxy.
Keep the game's genuine `nvngx_dlssd.dll` (RR) separate from `nvngx_dlssnr.dll` (NR).
For an RR-only comparison, disable the master NR switch; “Apply the model” merely hides the edit
and still incurs NR's GPU cost. Successful RR initialization alone does not prove image quality.

## AMD games without an upscaler (experimental)

The final-image route can run the danielblnc AMD runtime at D3D11, D3D12 or Vulkan
`Present`, without an upscaler call. The lmxxf and mochizuki runtimes run only inside an
upscaler call; with either one active the Neural tab says the final image is off. Put `dlssnr_amd_pass1.dll` and its
weights beside the OptiScaler proxy, then enable:

```ini
[DlssNr]
Enabled=true
PresentWithoutUpscaler=true
NrBackend=daniel
```

The **Neural** tab has the same final-image switch and shows its status. It starts
off by default. The final image includes the game's HUD. FidelityFX Optical Flow
estimates motion directly from successive frames, without installing ReShade.
The current route requires an unmultisampled RGBA8 or BGRA8 backbuffer; DXVK
D3D11 and HDR swapchains are not supported, and OpenGL has its own route (below).
Native Vulkan final-image NR uses a Vulkan-to-D3D12 shared image when supported, with host
readback as fallback.
The swapchain must support transfer source and destination usage, and Vulkan needs
`PresentWithoutUpscaler=true` before the game creates its swapchain: restart the game after
turning it on. On native Vulkan, XeFG/MFG presents through a D3D12 swapchain in a child window over the
game: set FG Input to OptiFG and FG Output to XeFG, then tick Frame Generation (Vulkan XeFG)
Active or press the FG key. The OptiScaler menu and overlays stay visible and usable over the
generated frames, and the FPS overlay shows the frames XeFG presents beside the game's, at most one
per display refresh when the game presents with vsync. XeFG pauses while the game window is in the
background and while frame generation is off; with NR off it keeps generating from the optical-flow
guides. Vulkan and D3D12 order their work on the GPU through a shared fence, and XeFG presents from a
thread of its own, so the game goes on to its next frame while NR and the generated frames run.
The first six frames after a reset use zero motion while the estimator warms up.
Its scene-change score resets neural history before a cut frame runs. The noise
seed is fixed on supported AMD runtime versions (0.4.1 through 0.6.0). On D3D11,
a bound, shader-readable depth buffer at Present is copied as a real depth guide;
otherwise depth remains synthetic. D3D12 final-image mode uses synthetic depth.
The look can still differ from the ReShade add-on and
the ordinary pre-SR path. If Optical Flow fails, the route uses zero motion and
reports the fallback in the Neural tab. A residual filter smooths small changes
with default strength 0.8 and threshold 4/255. RenoDX composition does not apply to the final
image; Effect strength sets the runtime's own strength there. A job the GPU holds for
`AmdStallStandDownMs` turns the route off until the game restarts, as on the upscaler path. The model keeps its temporal
history by default. Set `AmdEveryFrame=true` to compare the independent-frame
path, which can flicker more. ResizeBuffers releases its own
backbuffer reference after bounded GPU waits. If a wait fails, the route stops
for that process rather than reusing an unfinished image.
The Neural tab also lets you set one to three passes and a model scale of 50 to 100%.

### XeFG from a D3D11 final image (experimental)

The D3D11→D3D12 swapchain bridge can feed the final-image Optical Flow guides
to XeFG. Keep the settings above and add:

```ini
[FrameGen]
Enabled=true
FGInput=upscaler
FGOutput=xefg

[XeFG]
InterpolationCount=2
```

Place `libxess_fg.dll` and `libxell.dll` where OptiScaler loads its XeFG runtime.
`InterpolationCount=2` requests two generated frames between rendered frames;
the provider can cap this to its supported count. Restart the game after changing
the input or output. This path starts interpolation only after Optical Flow has
warmed up and the neural frame and guides are ready. It uses synthetic depth
on the bridge, so disocclusions and the HUD may still show artifacts. XeFG
requires its D3D12 proxy swapchain even though the game renders with D3D11.
Close RivaTuner Statistics Server (RTSS) first: with it hooked into the game, XeFG's
present stalls 25-35 ms a frame (Cities: Skylines at 3X: 235 fps out without it,
88-138 with constant stutter with it).

### 64-bit D3D9 games (experimental)

Name the OptiScaler DLL `d3d9.dll` and put it beside the game's 64-bit
executable, with the runtime files above. Do not also install OptiScaler as
`dxgi.dll` in the same folder. If `TargetProcessName` is set, it must name the
D3D9 executable. A `plugins\d3d9.dll` or `d3d9-original.dll` is loaded in place
of the system D3D9 runtime, for chaining another D3D9 wrapper.

The full OptiScaler menu (Insert) is drawn on the D3D9 device. With the
final-image settings above, each frame crosses from D3D9 to D3D12 through system
memory, because a classic D3D9 device cannot share a texture. NR runs there and
the result is copied back before the D3D9 Present. On an RX 9070 XT the copies
cost about 1.5 ms per frame at 1280x720 and 4.5 ms at 1920x1080, on top of the
network. A D3D9Ex device shares the frame with D3D12 instead. Multisampled
backbuffers are resolved on the way in and drawn over on the way back.

With the XeFG settings from the D3D11 section, a D3D12 XeFG swapchain takes over
the game window at the first frame and the D3D9 Present is skipped; the menu is
then drawn by the D3D12 overlay. This needs a windowed or borderless D3D9 device,
and a D3D9Ex device with `D3DSWAPEFFECT_FLIPEX` cannot have it, as its own
swapchain already holds the window (NR and the menu still run there). In
exclusive fullscreen D3D9 presents again, with NR and the menu. Back in a window,
or when the game moves to another window, the XeFG swapchain is made again and
frame generation resumes; the game stops for about 5 seconds while XeFG starts.
Frame generation runs while NR runs, since its guides come from NR's optical
flow. Turning NR off, unticking "Final image" or setting FG Output to None in
the menu turns generation off, and the frames keep going out through the same
swapchain: D3D9 cannot show a windowed frame in a window that swapchain has
presented to. The End key toggles generation and Numpad 5 toggles NR. With
`InterpolationCount` on auto (the INI default, or Auto in the menu's MFG list)
this route generates two frames per frame: with one, XeFG paces it badly (about
50 ms per rendered frame at 1920x1080, against 18 ms with two). 32-bit D3D9
games are not handled by this DLL.

The Optical Flow build needs an in-game visual and resize test before packaging for users.

### OpenGL games (experimental)

OpenGL games are reached by installing OptiScaler as `opengl32.dll` in the game's
folder (the folder of the executable that creates the OpenGL context). OptiScaler
loads the system `opengl32.dll`, or `opengl32-original.dll` or `plugins\opengl32.dll`
when present, and hooks its `wglSwapBuffers`. The full menu opens with the usual key
and is drawn through ImGui's OpenGL 3 renderer in a context of OptiScaler's own on the
game's window, so the game's OpenGL state is never touched.

With the `[DlssNr]` settings above, the back buffer is copied into a D3D12 texture
(`EXT_memory_object_win32`), processed by the same final-image route as D3D12, and
copied back before the game's present. The settings in `[FrameGen]` above add XeFG:
the generated frames are shown by a D3D12 swapchain in a child window over the game,
with the menu and the FPS overlay drawn over them; the child window is hidden while the
game is in the background. While the
window is being resized the game's image is shown as it is, and NR and XeFG start again
once the size has held for 300 ms. The menu's V-Sync settings set the game's swap
interval (`wglSwapIntervalEXT`). This DLL covers 64-bit games; a 32-bit OpenGL game takes
the package's `x86\opengl32.dll` and the 64-bit host in `OptiScaler64\` instead
(`tools/dx9-x86/README.md`).

## Optional DLSS Frame Generation

For the six NVIDIA Streamline/FG dependencies, the pinned download command, and separate instructions
for native/external FG versus OptiScaler's own FG, see [DLSS-FRAME-GENERATION.md](docs/DLSS-FRAME-GENERATION.md).
Do not copy another game's Streamline folder or assume NR working proves FG compatibility.
The optional component does not include the NR model or enable FG automatically.

## Diagnose a missing menu

Set:

```ini
[Log]
LogToFile=true
LogLevel=2
```

Then launch into a rendered scene and press `Insert` (`Alt+Insert` can help on some keyboard layouts).

- No `OptiScaler.log` beside the executable: the proxy was not loaded. Check the directory, proxy
  filename, antivirus quarantine, and conflicts with another DLL using the same proxy name.
- The log says `OptiScaler ... loaded` and `working as ...`: injection succeeded. A remaining problem
  belongs to the overlay input or Neural Rendering initialization, not the loader.
- `the model would not initialise`: on RTX 20/30/40, first check that the runtime hash is the
  compatibility `E67DEE...` build rather than the original `E16BC...` build.
- The menu toggles but does not accept input: try `[Hotfix] ManualInputPolling=true` and test without
  conflicting overlays.
