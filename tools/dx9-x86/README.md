# OptiScaler for 32-bit D3D9, D3D10, D3D11 and OpenGL games

OptiScaler.dll is x64 only, and so are the NR runtimes and XeFG, so a 32-bit game cannot load any of
them. This folder holds a pair that brings the 64-bit OptiScaler to such a game without changing it:

- `proxy.cpp` builds a 32-bit `d3d9.dll`. It forwards every export to the system `d3d9.dll`, starts
  the host and, at each Present of the device's own swap chain, copies the back buffer to the host.
- `host.cpp` builds `OptiScalerDx9Host.exe` (x64). It loads `OptiScaler.dll` from its own folder
  before it creates anything, waits for the thread OptiScaler starts as it loads (it makes a DXGI
  factory and installs hooks; Detours runs one transaction at a time, and a DXGI factory made here at
  the same moment left OptiScaler without its factory hooks twice in about 32 starts), then
  creates a D3D11 device on the game's adapter and a swap chain on a child window of the game's
  window. Each frame it receives is copied into that swap chain and
  presented, so OptiScaler sees an ordinary D3D11 program: final-image NR (`PresentWithoutUpscaler`),
  XeFG through its D3D11-to-D3D12 swap chain and the full menu all run as they do in a D3D11 game,
  from the same `OptiScaler.ini`.
- `dx9_wire.h` is the pipe between the two. The pipe I/O is the AMD NR ReShade add-on's x86 bridge
  transport (`reshade/core/x86bridge/bridge_io.h`).
- `bridge.h` is what the game side of every 32-bit API shares: starting the host, the child window, the
  input held back while the menu is open, and the bridge itself, over an `Api` that brings the device, its
  window and adapter, and the copy of the back buffer. `proxy.cpp` is the D3D9 one.
- `proxy_dxgi.cpp` builds a 32-bit `dxgi.dll` for D3D10 and D3D11 games (`exports\dxgi-x86`). `d3d10.dll`,
  `d3d10_1.dll` and `d3d11.dll` make their swap chains through the `dxgi.dll` beside the game
  (`D3D11CreateDeviceAndSwapChain` included), so this one proxy covers all three: it hooks the factory's swap
  chain creation and the Present of swap chains on a D3D10 or D3D11 device, and hands the back buffer
  (resolved first when multisampled) to the same host through a texture shared by handle, copied on the
  game's own device (for D3D11, on its immediate context). Swap chains of other devices (a D3D12 queue's) and
  swap chains with a frame latency waitable object are presented natively.
- `proxy_gl.cpp` builds a 32-bit `opengl32.dll` for OpenGL games (`exports\gl-x86`), forwarding every
  export (`exports_opengl32.inc`) to the system one. Its `wglSwapBuffers` (gdi32's `SwapBuffers` reaches it
  too) and `wglSwapLayerBuffers` hand the default framebuffer to the same host. The frame crosses through a
  D3D11 texture of a device of the proxy's own on the context's GPU, shared by its (KMT) handle and
  imported into the game's context with `GL_EXT_memory_object_win32`: one blit, flipped to D3D's row order
  (resolved first when multisampled), then `glFinish`. AMD drivers expose both that extension and
  `WGL_NV_DX_interop2` in 32-bit processes (measured on an RX 9070 XT, Adrenalin 26.9.2); the memory object
  needs no lock per frame. A context without it, or `SharedMemory=true`, sends the rows through memory
  (`glReadPixels`). A swap from another context than the one the copy was made in makes a new copy, and
  while the host presents the game's swap is skipped, as its Present is in D3D.

A game that asks for fullscreen (either API) stays windowed, since an exclusive fullscreen swap chain owns the
display and the host's would never show: its window becomes a borderless one over its monitor, made on the
window's own thread, and a DXGI game is told it is fullscreen (`ForceWindowed=0` in `[Dx9X86]` or `[DxgiX86]`
opts out). While the menu is open, DirectInput 8 devices read as untouched as well (state zeroed, buffered data
keeps only releases), for games that read the mouse there. `tools\test-dx10-x86.cmd` and
`tools\test-dx11-x86.cmd` run it end to end. An OpenGL game that goes fullscreen through a mode change or a popup
window over its monitor is followed like any resize (the child window takes the new client area).

While the host presents, the game's own Present is skipped: one presenter owns the picture for the
whole session, whatever NR, frame generation or the menu do, so turning them on and off never hands
the window from one swap chain to another.

## The child window, and a host that is lost

The host never presents into the game's window itself. A flip swap chain made by another process
that ends without releasing it leaves its window unable to take any other flip swap chain (measured:
`E_ACCESSDENIED` ten seconds after the presenting process was ended), and the game's own Present no
longer shows there either: the picture stays frozen while the game runs on. So the proxy makes a
child window covering the game's client area, on the game window's own thread, and the host presents
into that. The child takes no input (it is disabled and transparent to hit testing), so the mouse and
keys still reach the game's window, and it follows the game window's size. It is asked for, and ended,
with posted messages: an engine that renders on a thread of its own may keep its window thread waiting
for that very thread, so the Present never waits for the window thread (the game presents natively for
the frame or two until the child exists; with no child after 3 s it is asked for again).

The host serves one window. A second device of the game that presents to another window (a launcher,
a video or a tool view) is presented natively while the host's window is still shown and presented to,
and costs no host. A game that releases its device and window and makes new ones, hides the window the
host serves, or stops presenting to it for a second while it presents to another window, gets a new
host on that window (not counted as a failure); the host's swap chain stays on the child window it was
made for. A request for the child window that reaches the window's thread after a child was made or
taken for that window makes none.

A plain D3D9 device that is lost (`TestCooperativeLevel` does not report `D3D_OK`) presents natively,
which gives the game the `D3DERR_DEVICELOST` it resets on, and the host is kept.

When the host does not answer, reports its device lost, refuses a new back buffer size, dies, or
reports that OptiScaler did not wrap its swap chain (no DXGI hooks, so no NR, frame generation or
menu), the proxy closes the pipe (the host releases its swap chain and leaves; one still there after five
seconds is ended), destroys the child window, presents that frame natively, and starts a new host
with a new child window: a second later, or at once when the host refused a new size (XeFG cannot
resize while anything holds a back buffer, so a Reset to a new size with frame generation on costs a
new host). After four failures in a row, each before the host presented 600 frames, the session
stays native. The host starts on a thread of its own; the game presents natively until it is up.

## How a frame crosses

- D3D9Ex devices (Source engine games such as Half-Life 2): the back buffer is copied into an
  A8R8G8B8 render target created with a shared handle, which the host opens directly. The game side
  waits for that copy on the GPU before it hands the frame over.
- Plain D3D9 devices, which cannot share a texture: the copy is read back to system memory and
  written into a section of memory the host maps; the host uploads it.

A Present with a source or destination rectangle hands over that part of the back buffer, stretched to
where it would land in the window; the rest of the copy keeps the frame before. When the rectangles
cannot be copied (a multisampled back buffer cannot be stretched) the whole back buffer is.

With `Pipelined=true` (the default) the game side hands a frame over and returns to the game,
which renders its next frame while the host presents; the host's answer is collected at the next
Present, before the shared copy is written again. `Pipelined=false` waits for the host at every Present.
The host presents at the interval the game asked for. At interval 0 it presents with tearing allowed,
so a game without vsync is not held to the display's rate.

The game provides colour only. Motion comes from FidelityFX Optical Flow and depth is synthetic,
exactly as in OptiScaler's D3D11 final-image route.

## The menu

The host draws OptiScaler's menu into its swap chain. OptiScaler's input layer sees that the window
belongs to another process and reads the keyboard and mouse itself (polled keys and cursor, raw mouse
through an input sink). While the menu is open, the proxy holds the game's mouse and keyboard
messages and `WM_INPUT`; `GetCursorPos` returns where the cursor was when the menu opened and
`SetCursorPos` does nothing; `GetAsyncKeyState`, `GetKeyState` and `GetKeyboardState` read every key
as up; and the game's cursor clip is lifted (`ClipCursor` is held and `GetClipCursor` returns the
game's own clip), then put back when the menu closes. So the camera does not move and the game does
not act on keys under the menu. It does so with a message hook (`WH_GETMESSAGE`) on the game window's thread, which
sees every queued message before the game whatever window procedures are installed later; the key, cursor and clip
calls are patched in the import tables of every module loaded when a host starts and when the menu
opens. A key or mouse button the game saw go down before the menu opened still gets its release, so
nothing stays held in the game. Games that read the mouse through DirectInput still see it.

Text typed while the menu is open (Ctrl+click on a slider, input fields) is turned into characters on
the game window's thread and sent to the host with the next frame, which hands them to OptiScaler's
menu (`OptiScalerAddText`): OptiScaler cannot hook a window procedure in another process.

## Install

Put these where the game loads `d3d9.dll` from (for Half-Life 2 that is `bin`), with `dxgi.dll` in place of
`d3d9.dll` for a D3D10 or D3D11 game and `opengl32.dll` for an OpenGL game (beside the executable that makes the
context); one proxy per game:

```
d3d9.dll                      the 32-bit proxy (or dxgi.dll, or opengl32.dll)
OptiScaler64\
  OptiScalerDx9Host.exe
  OptiScaler.dll              the 64-bit OptiScaler build
  OptiScaler.ini
  dlssnr_amd_pass1.dll        the danielblnc runtime, and its dlssnr_on_amd_weights.bin
  libxess_fg.dll, libxell.dll for XeFG
```

Never put a `version.dll` in `OptiScaler64`: the host would load it as the system's. `OptiScaler.ini`
needs at least:

```ini
[DlssNr]
Enabled=true
PresentWithoutUpscaler=true

[FrameGen]
Enabled=true
FGInput=upscaler
FGOutput=xefg
```

The `[FrameGen]` part is only for XeFG. The proxy reads its own keys from the same file, under `[Dx9X86]` for
`d3d9.dll`, `[DxgiX86]` for `dxgi.dll` (`Enabled`, `ForceWindowed`, `SharedMemory`, `Pipelined`) and `[GlX86]` for
`opengl32.dll` (`Enabled`, `SharedMemory`, `Pipelined`):

```ini
[Dx9X86]
; false leaves the game alone (the proxy only forwards)
Enabled=true
; A fullscreen D3D9 device owns the display and the host's swap chain would never be seen, so a
; fullscreen request is turned into a window of the same size. A windowed D3D9 device takes only
; intervals 0 and 1, so one asked for 2, 3 or 4 is made 1 in D3D9 and the host presents at the asked
; one. false keeps fullscreen.
ForceWindowed=true
; true sends frames through memory even when the device can share a texture (for testing)
SharedMemory=false
; true lets the game render its next frame while the host presents this one; false waits for the
; host at every Present
Pipelined=true
```

Logs, all in `OptiScaler64`: `dx9-x86.log`, `dxgi-x86.log` or `opengl32-x86.log` (game side), `dx9-host.log`
(host), `OptiScaler.log`
(with `LogToFile=true`; the one from the host before is kept as `OptiScaler.previous.log`) and
`amd_presr.log` (NR).

## Build and test

`tools\build-dx9-x86.cmd` builds the proxies, the host and the x86 smoke program into
`exports\dx9-x86`, `exports\dxgi-x86` and `exports\gl-x86`. `tools\test-dx11-x86.cmd` and `tools\test-gl-x86.cmd`
take an installed folder as `tools\test-dx10-x86.cmd` does and run `tests\dx11_x86_smoke.cpp`
(`D3D11CreateDeviceAndSwapChain`, the factory's `CreateSwapChain`, a flip swap chain) and `tests\gl_x86_smoke.cpp`
(gdi32's `SwapBuffers` or `wglSwapBuffers`, the shared texture or memory): the host presents most frames without a
fault, the screen over the window shows the scene upright and changed by the network (read through GDI while it
holds still), the menu opens with Insert, the host follows a resize and a fullscreen toggle, and the program ends
within a minute of its run. `tools\test-dx9-x86.cmd` builds them, stages `exports\release-local\OptiScaler.dll`
with a runtime and XeFG, and runs `tests\dx9_x86_smoke.cpp` through every case under the GPU lock:
native D3D9, the bridge on D3D9 and D3D9Ex, the memory transport, NR with its toggle key, NR with
XeFG with the frame generation key and a device Reset to a new size, NR and frame generation both
turned off, the menu with mouse input held from the game, a Reset without frame generation, the window
on a thread that is blocked while the game renders (with and without a host replacement, and with
XeFG), the host ended mid-run with and without frame generation, the final-image Model scale dragged
in the menu with XeFG on, a key held across the menu, a value typed into the menu, a device and window
made again, the window minimized with XeFG on, a fullscreen request at interval 2 (D3D9 and D3D9Ex), a
second device on a second window and the move to it, a late request for the child window, keys polled
and the cursor clipped under the menu, Present rectangles (natively and through the host), a device
reported lost and then reset, a panned scene (the picture must move as the scene does; with NR, the
motion NR gets must match the pan and the picture must change little beyond the move), what the screen
shows with XeFG on (read back through Desktop Duplication: no rows of black while the scene pans, and on a
still scene NR in every frame shown, generated ones included, as strong as without frame generation),
OptiScaler without its DXGI hooks, and frame times at 1920x1080 (each timed case must have run what it
times, in one host).
