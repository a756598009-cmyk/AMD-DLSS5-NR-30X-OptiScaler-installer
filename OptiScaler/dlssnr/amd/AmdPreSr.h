#pragma once
#include <d3d12.h>
#include <filesystem>
#include <functional>
#include <string>

namespace AmdPreSr
{
struct AmdLayout;
// Identify pass DLL by SHA+size. Returns kAmdLayouts[].name or nullptr.
const char* IdentifyRuntimeName(const std::filesystem::path& passDll);
// 0.8.0 host NGX arming (GATE-REPORT-0.8.0.md): wait for the host services' GetState slot
// (ngxPublishSlot), call the publisher (its RVA ngxPublishFn, no arguments) so the "first
// Present" dword (ngxPresentFlag) is written from the seeded device, then arm the SR and RR
// evaluate-gate bytes (init/gpu/weights). Logs every step through `log`. Idempotent: the
// second and later calls return the first result without touching the module again.
// Returns false when the publish verification fails (and no gate was written).
bool ArmDlssNgx(HMODULE hostModule, const AmdLayout* layout, const char* tag,
                const std::function<void(const std::string&)>& log);
// True once ArmDlssNgx completed (publish verified, gates armed); false before or on failure.
bool NeuralHubArmOk();
// danielblnc's NR met the host on D3D12 (DLSS-FG armed) and was refused: Record never runs it there.
bool NrRefusedOnHostD3d12();
// 0.8.0 NGX D3D12 SR/RR submission hooks: isolate and notify SR/RR command lists even when NR
// did not record on that list (or when NR is off / settling / skipped).
int NgxPendingListIndex(UINT count, ID3D12CommandList* const* lists);
void NgxSubmitted(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists);
// 0.8.0 host present stage (AmdLayout.h, presentBefore): what the host's own Present detour runs around every present
// of the real swapchain, generated frames included. Nothing before the host is armed.
void NgxPresentBefore(IUnknown* swapchain, IUnknown* queueHint = nullptr);
void NgxPresentAfter(IUnknown* swapchain, HRESULT result);
// The host's DLSS-FG generated-frame ceiling (DLSSG.MultiFrameCountMax), 0 before the host is armed.
unsigned int NgxFgMultiFrameCountMax();
struct Frame
{
    ID3D12Resource *colour = nullptr, *motion = nullptr, *depth = nullptr, *exposure = nullptr;
    UINT width = 0, height = 0;
    float motionScaleX = 1, motionScaleY = 1;
    // Active display extent, excluding allocation padding; zero means render-resolution vectors.
    UINT motionWidth = 0, motionHeight = 0;
    float preExposure = 1, exposureScale = 1;
    bool reset = false, depthInverted = false;
    // NGX Jitter_Offset in render pixels, zero when the motion vectors already carry the jitter.
    float jitterX = 0, jitterY = 0;
    // Post-upscale placement: colour is the frame the upscaler (or Ray Reconstruction) finished,
    // so the game's guides belong to the smaller render grid rather than to it. The guide
    // conversion ratio-samples them up to the model's working size instead of requiring that they
    // cover the colour extent, which is the one rule the pre-SR placement can take for granted.
    bool afterUpscale = false;
    // Active extent of motion and depth, when it is not the colour's. Zero keeps the pre-SR
    // meaning: the guides share the colour's active extent.
    UINT guideWidth = 0, guideHeight = 0;
    // The model's extent under a game's dynamic resolution: the largest frame seen, which a smaller one is resampled
    // up to. Zero sizes the model from the frame.
    UINT fixedWidth = 0, fixedHeight = 0;
    D3D12_RESOURCE_STATES colourState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES exposureState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
};
struct LookSettings
{
    bool enabled = false;
    UINT appearance = 2, inspect = 0;
    bool detectSkin = true;
    float mix = 1, materialDetail = 1.15f, shapeDefinition = 1.2f, localLighting = 1.15f;
    float skinDetail = 1.1f, skinSoftness = .486f, specularControl = .58f, highlightRollOff = .9f;
    float colourSeparation = 0, shadowDepth = .2f, antiHalo = .901f, flatAreaProtection = 0;
    float tone = 0, exposureEV = 1, contrast = 1, saturation = 1, highlightCompression = 0;
};
struct RtgiSettings
{
    bool enabled = false;
    UINT quality = 2, denoiser = 1, inspect = 0;
    float mix = 1, lighting = 5, occlusion = 1, ambient = 1;
    float thickness = .1f, smoothness = .5f, fade = .3f, fov = 60, farPlane = 600;
    float contact = 0, saturation = 1, radius = 1;
    bool operator==(const RtgiSettings&) const = default;
};
struct Settings
{
    UINT encoding = 1; // 1 Linear, 2 sRGB, 3 Gamma 2.2
    bool everyFrame = false;
    bool fixedSeed = false;
    // How many frames may be running at the NR stage at once. Too few and a frame that
    // finds every buffer busy carries no NR at all. In one YYSLS AB session the
    // counter rose by about 1200-1440 per two-slot segment and stayed flat with
    // three; the log and PresentMon windows were not aligned, so this is not a
    // skip rate. Onimusha had no skips at either two or three.
    // See AmdPreSr.cpp for what the bound costs in memory.
    UINT slots = 3;
    bool toneChannels = false;
    float modelScale = 1;
    UINT passes = 1;
    // Person protection: the first pass's answer as well (Host::FirstPass), lmxxf and mochizuki with two or more
    // passes.
    bool firstPass = false;
    // Graphics preference from the bridge; each Record still requires admission.
    int spinDraw = 0;
    float tone = 0, structure = 1, skin = 1;
    // Share of the network's effect, 1 = the runtime's default Scale of 4/128.
    float strength = 1;
    // The runtime's Quality: true = fast, false = reference (0.4.2 and later).
    bool fast = true;
    // The runtime's ToneCurve: 0 Reinhard, 1 ACES (0.4.0 and later).
    UINT toneCurve = 0;
    // The runtime's ToneLift (0-0.25) and UseGameExposure (false = its own auto-exposure), 0.4.0 and later.
    float toneLift = 0;
    bool gameExposure = true;
    // The network's style input, danielblnc's Style: 0 Standard, 1 Natural, 2 Cinematic (0.3.3 and later).
    UINT style = 0;
    LookSettings look;
    RtgiSettings rtgi;
};
inline DXGI_FORMAT DepthReadFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_FLOAT:
        return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
inline DXGI_FORMAT ReadFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    // Motion vectors: Watch Dogs: Legion hands R16G16_TYPELESS.
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    default:
        return f;
    }
}
// Process lifetime owner: intentionally not destroyed/unloaded while HIP threads exist.
class Backend
{
    struct Impl;
    Impl* p;

  public:
    // modules: the runtime files this backend loads, <modules>1-3.dll. A loaded runtime cannot be isolated again, so
    // two backends in one process (the upscaler path and NR without upscaling) each need files of their own.
    Backend(ID3D12Device*, ID3D12CommandQueue*, const std::filesystem::path& directory,
            const wchar_t* modules = L"dlssnr_amd_pass");
    // Records pre-SR work. Returns a FP16 input for the upscaler, or nullptr on skip/failure.
    ID3D12Resource* Record(ID3D12GraphicsCommandList*, const Frame&, const Settings&);
    // Bind the render queue and enqueue a migration dependency BEFORE Execute.
    // Required for every submission; does not publish HIP work yet.
    int PendingListIndex(UINT, ID3D12CommandList* const*) const;
    void Submitting(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    // Diagnostic snapshot only; does not flush, cancel, or destroy GPU work.
    void TraceBoundary(const std::string&);
    // Must run immediately AFTER real queue submission, including non-upscale lists.
    void Submitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    bool Ready();
    bool Failed() const; // stopped for the process: load, device or runtime failure, or shutdown
    // After a failure that left nothing behind: the device lives, the completion timeline holds, no slot still owns a
    // job and the runtime has no pending list. Clears the failure and restarts the history; false when it may not.
    bool Retry();
    bool Shutdown();          // call before loader-lock teardown, after all submissions
    void InvalidateHistory(); // applied at the next safe recording boundary
    std::string Status() const;
    // Lock-free menu snapshot, restricted to the currently configured passes.
    bool GraphicsRestartNeeded(UINT activePasses) const;
    UINT64 RecordedFrames() const;
};
} // namespace AmdPreSr
