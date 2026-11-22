#pragma once
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <filesystem>
#include <string>
class Config;
namespace AmdPreSr
{
struct Settings;
}
namespace DlssNr::AmdBridge
{
AmdPreSr::Settings SettingsFromConfig(const Config& cfg, float modelScale);
bool HasFiles();
// NeuralHub 0.8.0 serves DLSS Frame Generation: its host module sits in dlssnr_amd_host\ beside this DLL and an
// nvngx_dlssg.dll beside the host or the game. Only files and the INI are checked, so it answers before the host loads
// (slInit, the INI); a game that ships nvngx_dlssg.dll without the host is not taken for one. With danielblnc as the NR
// runtime only FGOutput=dlssg (the installer's Streamline/DLSS-FG box) or HostComputeBackend=D3D12 asks for it: the
// game's own nvngx_dlssg.dll would put the host on D3D12 and take the NR away. Never with the host kept on HIP, nor on
// a GPU before RDNA3 (HostD3d12Supported).
bool NeuralHubFgInstalled();
// The same with OptiScaler's library folder, HostComputeBackend, FGOutput=dlssg, an FGOutput set to another FG (FSR FG,
// XeFG, reprojection) and the NR runtime given, and no GPU check: Config's own load asks this way, since LibraryFolder()
// reads Config and would construct it again from inside its constructor.
bool NeuralHubFgInstalled(const std::filesystem::path& library, const std::string& backend, bool dlssgOutput,
                          bool otherFgOutput, bool danielNr);
// The host's D3D12 compute runtime has code for gfx11 and gfx12 only: an RX 6000 (gfx10.3) gets 0x887a0005 from the
// driver and the game dies. The GPU behind luid, or the primary one; true when its generation is unknown.
bool HostD3d12Supported(const LUID* luid = nullptr);
// NeuralHubFgInstalled() without the GPU check, for DllMain: identifying the GPU makes a DXGI factory, which inside
// DLL_PROCESS_ATTACH stopped Phantom Blade Zero from starting (0xc0000142).
bool NeuralHubFgFilesInstalled();
// The armed host found an nvngx_dlssg.dll, or NeuralHubFgInstalled(): DLSS-FG is offered on AMD.
bool NeuralHubFgServed();
// [DlssNr] HostComputeBackend=HIP: the 0.8.0 host stays on HIP for danielblnc's NR, and its DLSS-FG (D3D12 only) is off.
bool HostKeptOnHip();
// OptiScaler's library folder ([Libraries] OptiDllPath, OptiScaler\ beside this DLL by default). NVIDIA's
// nvngx_dlss(.d/.g).dll placed there feed NeuralHub ahead of the game's own copies.
std::filesystem::path LibraryFolder();
// Install submission expansion before the first wrapped list is exposed. No runtime/HIP initialization.
bool EnsureSubmissionHook(ID3D12CommandQueue*);
// wanted: a DLSS feature is being made through the host. With another NR runtime chosen the host loads only then, or
// when the INI asks for DLSS (Dx12Upscaler) or NeuralHub's DLSS-FG; otherwise it stays out (~850 MB of VRAM).
bool EnsureNgxHost(ID3D12Device*, ID3D12CommandQueue* = nullptr, bool wanted = false);
// The host was left out for that reason: DLSS is still offered on D3D12, and picking it loads the host.
bool NgxHostDeferred();
// NeuralHub 0.8.0 is installed beside this DLL on an AMD GPU (or already armed): its host can serve DLSS-SR/DLSS-RR.
bool NgxHostInstalled();
// For a Vulkan or D3D11 game, whose NGX calls never reach EnsureNgxHost: arms the host on OptiScaler's D3D12 device
// before a w/Dx12 DLSS feature is made. True when armed.
bool EnsureNgxHostForInterop();
bool Before(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, ID3D12CommandQueue*);
// Post-upscale placement: runs the model over the frame the upscaler (or Ray Reconstruction) has
// already finished, and hands its answer back instead of substituting the upscaler's input. The
// caller owns writing it into the frame, because only it knows that surface's format and state.
// Returns true when the backend owns this frame (result may still be null on a skipped frame).
bool After(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, ID3D12CommandQueue*, ID3D12Resource** outResult);
void Restore(NVSDK_NGX_Parameter*);
bool HasReplacement(NVSDK_NGX_Parameter*);
void InvalidateHistory();
// Person protection's state and its worker's (model loaded, CPU time of the last mask), for the menu.
std::string PersonStatus();
void TraceContextRelease(unsigned int handle, bool after);
std::string Status();
// A note from a path in front of the bridge (the Vulkan seam), shown as Status until the next frame reaches the runtime.
void Note(const std::string& text);
// When the stall watch (StallWatch.h) tripped: NR goes off for this session (the INI keeps it), the note below says
// why and the watch is cleared, so NR switched on again retries. Otherwise the note is cleared. True when it stood
// NR down. The upscaler path and NR without upscaling both ask before each frame.
bool StandDownIfTripped();
// Set when the stall watch switched NR off, cleared once NR runs again; what the menu shows next to Enabled.
std::string StandDownNote();
// The runtime's own status line (for mochizuki: network size and time, or the build in progress), refreshed by
// the render thread at most twice a second; empty until a session runs. Never waits on a frame being recorded.
std::string RuntimeStatus();
// True once the process-exit hook has run. Everything after it passes the original colour through.
bool Exiting();
// Smoothed GPU time the neural work takes on the game's queue per frame, all passes; 0 before the
// first reading.
float NeuralMs();
// The last frame's reading, unsmoothed; 0 when no model has run in the last second (off, refused,
// or another path). What the FPS overlay draws.
float NeuralMsLast();
// A reading the final image took itself (PresentExperimental.h), into the same menu line, overlay line and graph.
void ReportNeuralMs(float ms);
// "danielblnc 0.5.1", "mochizuki" or "lmxxf": the runtime the backend was built for; empty before.
std::string NeuralRuntime();
// Dynamic NR resolution: the scale in use and the rendered frame rate, or empty while it is off.
std::string DynamicStatus();
bool GraphicsRestartNeeded(UINT activePasses);
// pass1 SHA name ("0.3.0" / "0.3.1" / "0.4.0" / "0.4.1" / …) or nullptr if missing/unknown.
// Cached for menu display until the DLL path, size, or write time changes.
const char* RuntimeName();
void UpdateConfirmedRenderQueue(ID3D12CommandQueue* q);
} // namespace DlssNr::AmdBridge
