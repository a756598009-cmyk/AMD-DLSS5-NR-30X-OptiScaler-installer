#pragma once

#include "SysUtils.h"
#include "State.h"

#include <optional>
#include <filesystem>

enum HasDefaultValue
{
    WithDefault,
    NoDefault,
    SoftDefault // Change always gets saved to the config
};

template <class T, HasDefaultValue defaultState = WithDefault> class CustomOptional : public std::optional<T>
{
  private:
    T _defaultValue;
    std::optional<T> _configIni;
    bool _volatile;

  public:
    CustomOptional(T defaultValue)
        requires(defaultState != NoDefault)
        : std::optional<T>(), _defaultValue(std::move(defaultValue)), _configIni(std::nullopt), _volatile(false)
    {
    }

    CustomOptional()
        requires(defaultState == NoDefault)
        : std::optional<T>(), _defaultValue(T {}), _configIni(std::nullopt), _volatile(false)
    {
    }

    // Prevents a change from being saved to ini
    constexpr void set_volatile_value(const T& value)
    {
        if (!_volatile)
        { // make sure the previously set value is saved
            if (this->has_value())
                _configIni = this->value();
            else
                _configIni = std::nullopt;
        }
        _volatile = true;
        std::optional<T>::operator=(value);
    }

    // Use this when first setting a CustomOptional
    constexpr void set_from_config(const std::optional<T>& opt)
    {
        if (!this->has_value())
        {
            _configIni = opt;
            std::optional<T>::operator=(opt);
        }
    }

    constexpr CustomOptional& operator=(const T& value)
    {
        _volatile = false;
        std::optional<T>::operator=(value);
        return *this;
    }

    constexpr CustomOptional& operator=(T&& value)
    {
        _volatile = false;
        std::optional<T>::operator=(std::move(value));
        return *this;
    }

    constexpr CustomOptional& operator=(const std::optional<T>& opt)
    {
        _volatile = false;
        std::optional<T>::operator=(opt);
        return *this;
    }

    constexpr CustomOptional& operator=(std::optional<T>&& opt)
    {
        _volatile = false;
        std::optional<T>::operator=(std::move(opt));
        return *this;
    }

    // Needed for string literals for some reason
    constexpr CustomOptional& operator=(const char* value)
        requires std::same_as<T, std::string>
    {
        _volatile = false;
        std::optional<T>::operator=(T(value));
        return *this;
    }

    constexpr T value_or_default() const&
        requires(defaultState != NoDefault)
    {
        return this->has_value() ? this->value() : _defaultValue;
    }

    constexpr T value_or_default() &&
        requires(defaultState != NoDefault)
    {
        return this->has_value() ? std::move(this->value()) : std::move(_defaultValue);
    }

    constexpr std::optional<T> value_for_config()
        requires(defaultState == WithDefault)
    {
        if (_volatile)
        {
            if (_configIni != _defaultValue)
                return _configIni;

            return std::nullopt;
        }

        if (!this->has_value() || *this == _defaultValue)
            return std::nullopt;

        return this->value();
    }

    // For a key a game quirk may change: what the player set is written even when it is the default, so a quirk
    // that acts only on auto does not undo it; a quirk's own value, or none, is written as auto.
    constexpr std::optional<T> value_as_chosen()
    {
        if (_volatile)
            return _configIni;

        return *this;
    }

    constexpr std::optional<T> value_for_config()
        requires(defaultState != WithDefault)
    {
        if (_volatile)
            return _configIni;

        if (this->has_value())
            return this->value();

        return std::nullopt;
    }

    constexpr T value_for_config_or(T other)
    {
        auto option = value_for_config();

        if (option.has_value())
            return option.value();
        else
            return other;
    }

    // Like value_for_config(), but keeps an explicit value that happens to equal
    // the class default - "unset" stays a distinct state.
    constexpr std::optional<T> value_for_config_ignore_default()
        requires(defaultState == WithDefault)
    {
        if (_volatile)
            return _configIni;

        if (this->has_value())
            return this->value();

        return std::nullopt;
    }
};

constexpr inline int UnboundKey = -1;
constexpr uint32_t NV_PRESET_LATEST = 0x00FFFFFF;

enum FpsOverlayPos : uint32_t
{
    FpsOverlayPos_TopLeft,
    FpsOverlayPos_TopRight,
    FpsOverlayPos_BottomLeft,
    FpsOverlayPos_BottomRight,
    FpsOverlayPos_COUNT,
};

enum FpsOverlay : uint32_t
{
    FpsOverlay_JustFPS,
    FpsOverlay_Simple,
    FpsOverlay_Detailed,
    FpsOverlay_DetailedGraph,
    FpsOverlay_Full,
    FpsOverlay_FullGraph,
    FpsOverlay_ReflexTimings,
    FpsOverlay_COUNT,
};

// Output scaling downscaler
enum class Scaler : uint32_t
{
    FSR1 = 0,
    Bicubic = 1,
    CatmullRom = 2,
    Lanczos2 = 3,
    Lanczos3 = 4,
    Kaiser2 = 5,
    Kaiser3 = 6,
    Magic = 7,
    Count
};

enum class ForceReflex : uint32_t
{
    InGame,
    ForceDisable,
    ForceEnable,
    Count
};

enum class LFXMode : uint32_t
{
    Conservative,
    Aggressive,
    ReflexIDs,
    Count
};

enum class LowLatencyInput : uint32_t
{
    None,
    Auto,
    AntiLag2,
    Reflex,
    XeLL,
    UeLowLatency,
    OptiScaler, // OptiScaler's own Streamline (DLSSG output) when the game has no low latency of its own
    _
};

enum class LowLatencyMode : uint32_t
{
    None,
    Auto,
    LatencyFlex,
    AntiLag2,
    XeLL,
    AntiLagVk,
    Reflex,
    FpsLimiter,
    _
};

enum class ReprojectionFill : uint32_t
{
    StrechEdge,
    Dithering,
    Noise,
    Debug,
    SoftEdge,
};

template <> struct EnumConfig<ReprojectionFill>
{
    static constexpr auto default_value = ReprojectionFill::Dithering;

    static constexpr std::pair<ReprojectionFill, std::string_view> mapping[] = {
        { ReprojectionFill::StrechEdge, "strech" },
        { ReprojectionFill::Dithering, "dithering" },
        { ReprojectionFill::Noise, "noise" },
        { ReprojectionFill::Debug, "debug" },
        { ReprojectionFill::SoftEdge, "soft" }
    };
};

class Config
{
    bool WriteIni();

  public:
    Config();

    // Init flags
    CustomOptional<bool, NoDefault> DepthInverted;
    CustomOptional<bool, NoDefault> AutoExposure;
    CustomOptional<bool, NoDefault> HDR;
    CustomOptional<bool, NoDefault> JitterCancellation;
    CustomOptional<bool, NoDefault> DisplayResolution;
    CustomOptional<bool, NoDefault> DynamicResolution;
    CustomOptional<bool, NoDefault> DisableReactiveMask;
    CustomOptional<float> DlssReactiveMaskBias { 0.45f };

    // Logging
    CustomOptional<bool> LogToFile { false };
    CustomOptional<bool> LogToConsole { false };
    CustomOptional<bool> LogToDebug { false };
    CustomOptional<bool> LogToNGX { false };
    CustomOptional<bool> OpenConsole { false };
    CustomOptional<bool> DebugWait { false }; // not in ini
    CustomOptional<int> LogLevel { 2 };
    CustomOptional<std::wstring> LogFileName { L"OptiScaler.log" };
    CustomOptional<bool> LogSingleFile { true };
    CustomOptional<bool> LogAsync { false };
    CustomOptional<int> LogAsyncThreads { 4 };

    // XeSS
    CustomOptional<bool> BuildPipelines { true };
    CustomOptional<int32_t> NetworkModel { 0 };
    CustomOptional<bool> CreateHeaps { true };

    // --- DLSS 5 Neural Rendering (OptiScaler/dlssnr) --- removable as one block -----------------
    // DLSS Neural Rendering: a detail-synthesis pass over the upscaler's output. Off by default -- it is
    // an undocumented feature driven directly through its snippet, not something NVIDIA exposes.
    CustomOptional<bool> DlssNrEnabled { false };
    // Run AMD NR (danielblnc) on the final D3D11, D3D12 or Vulkan swapchain image when the game has no upscaler.
    CustomOptional<bool> DlssNrPresent { false };
    // Run the NR pass on the upscaler's colour input, at render resolution, immediately before SR.
    // Off preserves the v0.2.0 post-upscale placement.
    CustomOptional<bool> DlssNrRunBeforeSr { false };
    CustomOptional<bool> DlssNrApplyAfterRR { false };
    CustomOptional<unsigned int> DlssNrRRPasses { 1 };
    CustomOptional<float> DlssNrRRWorkingScale { 1.0f };
    // Toggles the pass in game. Unbound by default -- a key that does something unexpected is worse
    // than one that does nothing.
    CustomOptional<int> DlssNrToggleKey { VK_NUMPAD5 };
    CustomOptional<uint32_t> DlssNrPreset { 0 };
    CustomOptional<float> DlssNrIntensity { 1.0f };
    // 0 default (standard), 1 natural, 2 cinematic -- the model's own processing profiles.
    CustomOptional<uint32_t> DlssNrStyle { 0 };
    // Optional per-pass model profiles. Pass 1 uses Preset/Style above; an absent override inherits
    // pass 1. Keeping inheritance explicit preserves every existing configuration and lets changing
    // the base profile update the whole stack unless a later pass was deliberately specialised.
    CustomOptional<uint32_t, NoDefault> DlssNrPass2Preset;
    CustomOptional<uint32_t, NoDefault> DlssNrPass2Style;
    CustomOptional<uint32_t, NoDefault> DlssNrPass3Preset;
    CustomOptional<uint32_t, NoDefault> DlssNrPass3Style;
    CustomOptional<float> DlssNrLocalStructure { 1.0f };
    CustomOptional<float> DlssNrLocalTone { 1.0f };
    CustomOptional<bool> AmdNeuralLighting { true };
    CustomOptional<float> AmdNeuralLightingStrength { .5f };
    // 1 Linear, 2 sRGB, 3 Gamma 2.2. An old INI's 0 (the former Auto) reads as Linear, the default.
    // Linear hands the network the frame as danielblnc's own runtime does; decoding it as sRGB first
    // darkens what the network sees and leaves skin and hair paler than his.
    CustomOptional<int> AmdEncoding { 1 };
    // danielblnc's one Effect strength, 0-2. Without the RenoDX composition it is the runtime's Scale as a fraction of
    // its 4/128 default (0.3.1 and later, the runtimes whose layout maps Scale); with it the runtime keeps its default
    // and this is the composition's intensity in place of RenoIntensity.
    CustomOptional<float> AmdEffectStrength { 1.0f };
    // Stabilizer after the runtime, before SR: along the game's motion vectors it smooths frame-to-frame changes of
    // the effect smaller than the threshold (1/255 of the tone-mapped sRGB value, 0.5-8) by the strength (0-1, 0 off).
    CustomOptional<float> AmdStabilizerStrength { 0.0f };
    CustomOptional<float> AmdStabilizerThreshold { 2.0f };
    // RenoDX's composition after the runtime (and the stabilizer), before SR, on every runtime: the runtime's edit
    // put back on the game's own colour as a gain bounded in log2 (RenoComposition.h). Off by default. Intensity (0-2)
    // and Guard (1-8, the most a pixel's brightness may move, as a multiple either way) are the ReShade add-on's.
    // Colour 0 keeps the game's hue (the edit's luminance only), 1 the edit's colour too; ChromaClamp bounds the colour
    // edit in stops (0.25-2); Pedestal takes off the lift the network adds in near-black areas.
    CustomOptional<bool> DlssNrRenoComposition { false };
    CustomOptional<float> DlssNrRenoIntensity { 1.0f };
    CustomOptional<float> DlssNrRenoGuard { 2.0f };
    CustomOptional<float> DlssNrRenoColour { 1.0f };
    CustomOptional<float> DlssNrRenoChromaClamp { 1.0f };
    CustomOptional<bool> DlssNrRenoPedestal { false };
    // Person protection (NrPersonPartition, from TheAutomatic/dlss-5-amd-project), lmxxf and mochizuki before SR, off
    // by default: a person mask from person-worker.exe (person-model\ beside the DLL's folder) keeps the first pass's
    // answer on people when two or more passes run, the last one elsewhere. Strength and DetailGain (0-1) scale the
    // person's correction and its fine detail; below 1 they apply in single pass too. Model: 0 PP-HumanSeg, 1
    // YOLO11n-seg, 2 YuNet (faces only; FaceInputSize 320, 384, 416 or 448). DebugMask tints the mask and logs it.
    CustomOptional<bool> NrPersonPartition { false };
    CustomOptional<int> NrPersonModel { 0 };
    CustomOptional<int> NrFaceInputSize { 320 };
    CustomOptional<float> NrPersonStrength { 1.0f };
    CustomOptional<float> NrPersonDetailGain { 1.0f };
    CustomOptional<bool> NrPersonDebugMask { false };
    // The runtime's arithmetic: 0 fast (its default, about 13% faster on RX 9000), 1 quality
    // (NVIDIA's reference arithmetic). danielblnc 0.4.2 and later.
    CustomOptional<int> AmdQuality { 0 };
    // The display curve the danielblnc runtime shows the network the frame through: 0 Reinhard
    // (its default), 1 ACES. danielblnc 0.4.0 and later.
    CustomOptional<int> AmdToneCurve { 0 };
    // The danielblnc runtime's own Black lift (ToneLift, 0-0.25, 0 its default) and exposure source
    // (UseGameExposure: true the exposure the game hands the upscaler when it does, false its own auto-exposure).
    // danielblnc 0.4.0 and later.
    CustomOptional<float> AmdToneLift { 0.0f };
    CustomOptional<bool> AmdUseGameExposure { true };
    // danielblnc's Style, the network's style input: 0 Standard, 1 Natural, 2 Cinematic.
    CustomOptional<int> AmdStyle { 0 };
    // 1-5 in the ini; the menu offers 2-5. Too few and a frame that finds every
    // buffer busy carries no NR at all, so this decides whether the mode works
    // rather than how fast it runs. See AmdPreSr.cpp for the measurements.
    //
    // The default has to follow AMD_SINGLESLOT as well, or the control build
    // contradicts its own banner: AmdBridge overwrites the backend's default
    // from here on the first frame, so a control build with this left at 3
    // would announce "default=1" and then run three.
#ifdef AMD_SINGLESLOT
    CustomOptional<int> AmdSlots { 1 };
#else
    CustomOptional<int> AmdSlots { 3 };
#endif
    CustomOptional<float> AmdNrScale { 1 };
    // Dynamic NR resolution: steps the model's scale down from the configured one while the
    // rendered frame rate stays under AmdDynamicTargetFps. See dlssnr/amd/DynamicScale.h.
    CustomOptional<bool> AmdDynamicScale { false };
    CustomOptional<int> AmdDynamicTargetFps { 60 };
    // true turns the model's temporal history off (and, with a single slot, waits
    // for the model after every Execute; with more than one slot that wait is
    // skipped except during a native rebuild). Exposed on the Ins menu as
    // "Disable temporal stabilization"; the key keeps its original name so
    // existing INIs still work. Off by default: history on, image steadied.
    CustomOptional<bool> AmdEveryFrame { false };
    // danielblnc 0.4.1 to 0.6.0: the runtime's noise seed set back every frame (NR without upscaling always does).
    CustomOptional<bool> AmdFixedSeed { true };
    // Legacy INI key. SpinDraw is driven only by AmdGraphicsWait.
    CustomOptional<int> AmdSpinDraw { 0 };
    // New wait (1) vs original wait (0). Default 1 since 1.8.4; still being tested.
    // Live switching needs installed hooks and a ready 1-pixel-draw PSO; otherwise restart.
    CustomOptional<int> AmdGraphicsWait { 1 };
    // One danielblnc job the GPU holds this long stands NR down for the session (StallWatch.h). 0 = off.
    CustomOptional<int> AmdStallStandDownMs { 2000 };
    // NR host: daniel (default), lmxxf, mochizuki, off. Missing key = daniel. Restart to change.
    CustomOptional<std::string> NrBackend { "daniel" };
    // Host compute runtime written to dlssnr_amd_host\neuralhub.ini ([Compute] Backend). auto: D3D12 when an
    // nvngx_dlssg.dll is there for NeuralHub's DLSS-FG (D3D12 only), HIP otherwise. HIP: danielblnc's NR runs (HIP
    // only on our path) and the FG stays off. D3D12: the kernels run as pipelines in the game's list. Restart.
    CustomOptional<std::string> HostComputeBackend { "auto" };
    // Host zero-copy interop ([Debug] HipZeroCopy): true (default) shares the game's textures with HIP and runs the
    // network inline on the game's queue (the inline wait); false copies the inputs and runs the network
    // asynchronously (no inline wait, no same-frame result, but no spin/staging deadlock either). Restart.
    CustomOptional<bool> HostZeroCopy { true };
    // lmxxf diagnostics: original/copy-current/staging-current/staging-previous,
    // proxy-original/split-original (NO NR). off requires a same-frame boundary. Restart to change.
    CustomOptional<std::string> LmxxfDiagnostic { "off" };
    // Fit Color inputs above 1920x1080 onto the 1080 network (DLSS5_FIT_LARGE).
    // Default false: missing/auto => false (Palworld: FitLarge+~2K Color same-frame can hitch ~2s/frame).
    // Opt-in with explicit true; applied to env on Config load / lmxxf backend start; installer writes flags.
    CustomOptional<bool> LmxxfFitLarge { false };
    // Each lmxxf pass reads its own output from the previous frame, moved by the game's motion vectors.
    CustomOptional<bool> LmxxfTemporal { true };
    // With LmxxfTemporal: where the result differs from the previous frame's by less than the threshold (in 1/255),
    // it is pulled toward it, by the strength at no difference. Strength 0 turns it off.
    CustomOptional<float> LmxxfSmoothStrength { 0.8f };
    CustomOptional<float> LmxxfSmoothThreshold { 10.0f };
    // The same smoothing on the model's change to the frame only (output minus input, against that change in the
    // previous frame), so the game's own samples reach the upscaler as they arrived.
    CustomOptional<bool> LmxxfSmoothResidual { false };
    // lmxxf's NGX Style: 0 Standard (NVIDIA's default), 1 Natural (the one its kernels had built in), 2 Cinematic.
    // A change rebuilds the network.
    CustomOptional<uint32_t> LmxxfStyle { 1 };
    // Reuse the network's ViT part where the picture has not changed: faster on a still scene, slightly lossy.
    // One pass only.
    CustomOptional<bool> LmxxfVitReuse { false };
    // The 1080 tier on 1920x1088 rows instead of 1152: about 4% faster, slightly lossy at the bottom. Rebuilds the
    // network when changed.
    CustomOptional<bool> LmxxfCompact1080 { false };
    // The 71-block model instead of the Small one of NVIDIA's 310.9.16 DLL (lmxxf 0.44's 0.5x), which runs at any
    // pass count when native-game-tiled-assets\small is there. A change rebuilds the network
    // (LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL).
    CustomOptional<bool> LmxxfOriginalModel { false };
    // Three passes: run two and extrapolate the third from them, about a third less time, close to a real third
    // pass (upstream's multi-pass predict). On by default.
    CustomOptional<bool> LmxxfPassEstimate { true };
    // Two or three passes: skin-coloured areas go back toward pass 1's result, so faces do not take the extra passes.
    // By colour (upstream's mask), so brown and orange surfaces are protected too.
    CustomOptional<bool> LmxxfSkinProtect { false };
    // The codec's white point measured from the frame's mean luminance instead of a fixed 1.0, for games that hand
    // very dark linear colour (KCD2 at night: grain, a green tint). Off by default.
    CustomOptional<bool> LmxxfAutoExposure { false };
    // mochizuki's settings, the INI key being the member's name. Its tuning comes from these keys only, never from
    // the danielblnc, lmxxf or NVIDIA ones, so each runtime keeps its own. The network always runs inside upstream's
    // RenoDX stage, whose MochizukiReno* settings (MochizukiRenoKeys.inc, RenoDX's own, with its defaults) hold the
    // model, pass, resolution, composition, look and encoding controls; the keys here are what the stage has no
    // setting for. RenoPasses, RenoResolutionScale and RenoSourceEncoding rebuild the network (a second or two
    // without NR); the rest apply on the next frame. One leftover: without MochizukiTemporal, LmxxfTemporal decides.
    //
    // History from the previous frame, moved along the game's motion vectors. An INI without the key follows
    // LmxxfTemporal, which switched it before it had its own, so a value set here is always saved, true included.
    CustomOptional<bool, SoftDefault> MochizukiTemporal { true };
    // How much of the previous frame's result the network's last block blends into this one, 0..1.
    CustomOptional<float> MochizukiHistoryStrength { 1.0f };
    // The host stabilizer after the runtime, as AmdStabilizerStrength and AmdStabilizerThreshold are for danielblnc.
    CustomOptional<float> MochizukiStabilizerStrength { 0.0f };
    CustomOptional<float> MochizukiStabilizerThreshold { 2.0f };
    // Foveated NR (dlssnr/amd/Foveation.h), before SR (mochizuki) and NR without upscaling: the network on a smaller
    // warped copy, full density in the centre. 0 off, 1 light, 2 medium, 3 strong, 4 the four values below (shares of the screen;
    // the fall-off is a share of its height, the edge a density 0.2-1).
    CustomOptional<int> NrFoveation { 0 };
    CustomOptional<float> NrFoveationCentreWidth { 0.5f };
    CustomOptional<float> NrFoveationCentreHeight { 0.6f };
    CustomOptional<float> NrFoveationFalloff { 0.12f };
    CustomOptional<float> NrFoveationEdge { 0.5f };
    // The network as ACO's machine code (the Linux driver's compiler, made in advance) instead of AMD's Windows
    // compiler: about a fifth faster, the same picture. On by default; its author tested it on AMD Software 26.9.2
    // only. Takes effect when the game starts again; without the driver's pipeline binaries the driver compiles as
    // before.
    CustomOptional<bool> MochizukiAco { true };
    // RX 6000 (the RDNA2 network): its INT8 form (dlssnr-amd\shaders-rdna2-int8, the 8-bit dot products) instead of
    // the FP16 one. On by default (other cards ignore it); taken when the game starts. Sets MZ_RDNA2_INT8 for the
    // runtime, which falls back to FP16 without the folder, the driver feature or a passing pipeline check.
    CustomOptional<bool> MochizukiRdna2Int8 { true };
    // With ACO on an RX 9000: upstream's CC_SILVER_AARDWOLD_OPTIMIZED, which replaced the int4 mixed network this key
    // first named (upstream 0.0.8.3): the 310.8 model made faster, its picture very close to that model's. A change
    // rebuilds the network in a few seconds while the frames keep the current one, without a restart.
    CustomOptional<bool> MochizukiInt4 { false };
    // Its Optimized layers ([Network] OptimizedLayers): 0 Quality, 1 Balanced, 2 Performance, 3 Ultra Performance (the
    // default, as upstream's). The words quality, balanced, performance and ultra are read too. A change rebuilds.
    CustomOptional<uint32_t> MochizukiOptimizedLayers { 3 };
    // The 310.8 model instead of 310.9's new one, which runs when dlssnr-amd\dlssnr-small.bin is there; int4 mixed
    // implies it. A change rebuilds the network (LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL).
    CustomOptional<bool> MochizukiOriginalModel { false };
    // With ACO on an RX 9000, on the new model: upstream's CC_SMALL_FP8_EXPERIMENTAL, that model made faster (ReLU, fixed
    // attention on three decoder layers, its changed weights fitted). The picture differs somewhat. A change rebuilds the
    // network like int4 mixed (LMXXF_NR_FRAME_FLAG_EXPERIMENTAL).
    CustomOptional<bool> MochizukiExperimental { false };
    // Where mochizuki runs in a native D3D12 game with an upscaler, as upstream's Placement: before (the upscaler's
    // input), after (its output, or Ray Reconstruction's) or present (the finished picture on the back buffer). Anything
    // else is before. Taken at the game's start. See Backend::MochizukiPlacementNow.
    CustomOptional<std::string> MochizukiPlacement { "before" };
    // No effect while the network runs in the RenoDX stage, which stacks its own passes (RenoPasses) and resolves
    // each itself; kept, out of the menu and the INI, for passes outside it. With two or three passes: the later ones
    // add only detail (LMXXF_NR_FRAME_FLAG_CASCADE_DETAIL); the result is held inside the first pass's local
    // brightness range (LMXXF_NR_FRAME_FLAG_ANTI_HALO), 1 holding each pixel to it and 0 letting a quarter past it;
    // with three, the third is estimated from the first two (LMXXF_NR_FRAME_FLAG_PASS3_ESTIMATE), which may come
    // back reworked for the stage.
    CustomOptional<bool> MochizukiCascadeDetail { true };
    CustomOptional<bool> MochizukiAntiHalo { true };
    CustomOptional<float> MochizukiAntiHaloStrength { 0.8f };
    CustomOptional<bool> MochizukiPassEstimate { true };
    // Before Super Resolution: the camera jitter's step between frames added to the motion vectors the history is
    // fetched with (they leave the jitter out). Experimental, off.
    CustomOptional<bool> MochizukiJitterHistory { false };
    // false runs the network at its whole cost but shows the original frame, to measure it or compare.
    CustomOptional<bool> MochizukiApplyModel { true };
    // No effect in the RenoDX stage, whose network reads the vectors at the pixel, so no depth goes to the runtime;
    // kept, out of the menu and the INI. It handed the network the game's depth, which picks the motion vector of the
    // nearer surface on silhouettes.
    CustomOptional<bool> MochizukiUseDepth { true };
    // Dynamic resolution. exact builds the network for the render resolution, so each change of it rebuilds the
    // network (a second or two without NR). auto, the default, does so until the render resolution drops below the
    // size of the game's colour buffer; from then on it keeps one network, built for the largest resolution seen, and
    // the frame moves inside it. always does that from the first frame, also when the game reallocates its buffers.
    // Anything else is auto.
    CustomOptional<std::string> MochizukiDynamicResolution { "auto" };
    // Preprocess (mochizuki0323's): the picture the network is shown, changed before it runs and taken back out of its
    // answer, in the RenoDX stage's encoding. Off by default. Exposure 0 off, 1 auto (a histogram auto exposure, for
    // games that do not hand theirs over; it replaces RenoDX's divisor), 2 fixed (the bias alone); bias -8..8 EV;
    // curve 0 none, 1 neutral, 2 reinhard, 3 filmic, 4 gt, 5 aces, 6 agx (in place of RenoDX's); contrast 0.5..2;
    // saturation 0.05..2.
    CustomOptional<bool> MochizukiPreprocess { false };
    CustomOptional<uint32_t> MochizukiPreprocessExposure { 1 };
    CustomOptional<float> MochizukiPreprocessBias { 0.0f };
    CustomOptional<uint32_t> MochizukiPreprocessCurve { 3 };
    CustomOptional<float> MochizukiPreprocessContrast { 1.0f };
    CustomOptional<float> MochizukiPreprocessSaturation { 1.0f };
    // Read and ignored, as TheAutomatic/dlss-5-amd-project keeps it: its older builds used 1 to force 30 layers.
    CustomOptional<uint32_t> MochizukiLayerMode { 0 };
    // Frames without motion vectors: the network's noise seed moves every frame, as NVIDIA's DLL does.
    CustomOptional<bool> MochizukiNoiseEveryFrame { false };
    // Natural and Cinematic graded after the network, as NVIDIA's DLL does (upstream 0.0.7); off gives the earlier picture.
    CustomOptional<bool> MochizukiStyleGrade { true };
    // The temporal history's precision: 16 (RGBA16F, as NVIDIA's DLL and upstream 0.0.8.2) or 32 (RGBA32F, as before).
    CustomOptional<uint32_t> MochizukiHistoryPrecision { 16 };
    // Preprocess's change taken back through the curve's bounded inverse (upstream 0.0.8.1); off, as a ratio.
    CustomOptional<bool> MochizukiPreprocessInverse { true };
#define MZR_F(name, def, lo, hi, group, label, member, help) CustomOptional<float> name { def };
#define MZR_U(name, def, lo, hi, group, label, member, help) CustomOptional<uint32_t> name { def };
#define MZR_B(name, def, lo, hi, group, label, member, help) CustomOptional<bool> name { def };
#include "dlssnr/backend/MochizukiRenoKeys.inc"
#undef MZR_F
#undef MZR_U
#undef MZR_B
    // Experimental dirty insert: request SpinDraw=1 even when freeze/admission fails.
    // No complete D3D12 graphics-state restore — risk matches the danielblnc runtime. Default 0.
    CustomOptional<int> AmdGraphicsUnsafe { 0 };
    CustomOptional<bool> AmdRtgiEnabled { false };
    CustomOptional<uint32_t> AmdRtgiQuality { 2 };
    CustomOptional<uint32_t> AmdRtgiDenoiser { 1 };
    CustomOptional<uint32_t> AmdRtgiInspect { 0 };
    CustomOptional<float> AmdRtgiContact { 0 };
    CustomOptional<float> AmdRtgiSaturation { 1 };
    CustomOptional<float> AmdRtgiRadius { 1 };
    CustomOptional<float> AmdRtgiMix { 1 };
    CustomOptional<float> AmdRtgiLighting { 5 };
    CustomOptional<float> AmdRtgiOcclusion { 1 };
    CustomOptional<float> AmdRtgiAmbient { 1 };
    CustomOptional<float> AmdRtgiThickness { .1f };
    CustomOptional<float> AmdRtgiSmoothness { .5f };
    CustomOptional<float> AmdRtgiFade { .3f };
    CustomOptional<float> AmdRtgiFov { 60 };
    CustomOptional<float> AmdRtgiFarPlane { 600 };
    CustomOptional<bool> AmdLookEnabled { false };
    CustomOptional<uint32_t> AmdLookAppearance { 2 };
    CustomOptional<float> AmdLookMix { 1.0f };
    CustomOptional<float> AmdLookMaterialDetail { 1.15f };
    CustomOptional<float> AmdLookShapeDefinition { 1.20f };
    CustomOptional<float> AmdLookLocalLighting { 1.15f };
    CustomOptional<float> AmdLookSkinDetail { 1.10f };
    CustomOptional<float> AmdLookSkinSoftness { 0.486f };
    CustomOptional<bool> AmdLookDetectSkin { true };
    CustomOptional<float> AmdLookSpecularControl { 0.58f };
    CustomOptional<float> AmdLookHighlightRollOff { 0.9f };
    CustomOptional<float> AmdLookColourSeparation { 0.0f };
    CustomOptional<float> AmdLookShadowDepth { 0.2f };
    CustomOptional<float> AmdLookAntiHalo { 0.901f };
    CustomOptional<float> AmdLookFlatAreaProtection { 0.0f };
    CustomOptional<uint32_t> AmdLookInspect { 0 };
    CustomOptional<float> AmdLookTone { 0.0f };
    CustomOptional<float> AmdLookExposureEV { 1.0f };
    CustomOptional<float> AmdLookContrast { 1.0f };
    CustomOptional<float> AmdLookSaturation { 1.0f };
    CustomOptional<float> AmdLookHighlightCompression { 0.0f };
    // -1 means follow local structure, which is the model's own default. It is not a strength of zero.
    CustomOptional<float> DlssNrSkinStructure { -1.0f };
    CustomOptional<bool> DlssNrAutoMask { true };
    // Sparse per-pass model settings, "2:intensity=0.5,style=1;3:intensity=0.3". A pass with no
    // entry uses the values above.
    CustomOptional<std::string> DlssNrPassOverrides { "" };
    // Lifts the pass slider past kDefaultMaxPasses. Each pass is another model run and another NGX
    // feature holding its own history.
    CustomOptional<bool> DlssNrUnlockPasses { false };
    // Optional final-composition filter, not NVIDIA's semantic auto mask.
    CustomOptional<bool> DlssNrSkinProtection { false };
    CustomOptional<bool> DlssNrSkinToneEnabled { true };
    CustomOptional<float> DlssNrSkinDetail { 1.0f };
    CustomOptional<float> DlssNrSkinColour { 1.0f };
    CustomOptional<float> DlssNrEnvironmentDetail { 1.0f };
    CustomOptional<float> DlssNrEnvironmentColour { 1.0f };
    CustomOptional<bool> DlssNrShowSkinMask { false };
    CustomOptional<float, NoDefault> DlssNrPass2Intensity;
    CustomOptional<float, NoDefault> DlssNrPass2LocalStructure;
    CustomOptional<float, NoDefault> DlssNrPass2LocalTone;
    CustomOptional<float, NoDefault> DlssNrPass2SkinStructure;
    CustomOptional<bool, NoDefault> DlssNrPass2AutoMask;
    CustomOptional<float, NoDefault> DlssNrPass3Intensity;
    CustomOptional<float, NoDefault> DlssNrPass3LocalStructure;
    CustomOptional<float, NoDefault> DlssNrPass3LocalTone;
    CustomOptional<float, NoDefault> DlssNrPass3SkinStructure;
    CustomOptional<bool, NoDefault> DlssNrPass3AutoMask;

    // How much of the model's edit reaches the frame. Separated because detail synthesis is a luminance
    // edit and any colour shift is usually the part you do not want, and allowed past 1.0 because
    // exaggerating an edit is the only honest way to see whether there is one.
    CustomOptional<float> DlssNrTransferStrength { 1.0f };
    CustomOptional<float> DlssNrColourStrength { 1.0f };

    // The RenoDX reversible proxy mode. 0 = today's soft-knee encode + our composition (default,
    // byte-identical); 1 = unclipped Neutwo proxy + our composition; 2 = Neutwo proxy + pure-inverse
    // replace. An in-game A/B and a way back. Default 0 = byte-identical to before.
    CustomOptional<uint32_t> DlssNrReversibleMode { 0 };

    // Whether the model's edit is applied. Off keeps the pass running (so Hold frame works) but shows
    // the clean upscaler frame -- for A/B'ing NR on/off on a frozen frame. Default true.
    CustomOptional<bool> DlssNrApplyModel { true };

    // Frame hold: freeze the NR pass's input so a live setting change re-renders the SAME frame -- the
    // only clean way to A/B our settings. A live testing toggle, not really a saved preference; off by
    // default. See dlssnr/design/frame-hold.md.
    CustomOptional<bool> DlssNrHoldFrame { false };

    // The most the pass may multiply or divide a pixel by. A detail pass has no business restyling a
    // light source, whatever the model returns.
    CustomOptional<float> DlssNrMaxRatio { 2.0f };

    // How a model that worked below the frame's size is brought back. 0 classic, 1 matched
    // residual. Only has an effect when Model resolution is under 100%.
    CustomOptional<uint32_t> DlssNrTransfer { 1 };

    // Measure the white point from the frame instead of taking it from the slider. On a frame the
    // game already tone mapped there is nothing to measure and this has no effect.
    //
    // Off by default, because it is not finished. The pass writes its result back into the same buffer
    // the meter reads, so with the pass running the meter is partly measuring its own output and the
    // two chase each other: Enshrouded, one session, 1545 samples spanning 0.01 to 97.9 with 57 jumps
    // beyond 1.5x in a single frame. Measured in the same spot seconds apart, 41.31 with the pass off
    // against 0.46 with it on. That is visible as the picture pumping and occasionally flickering.
    //
    // The slider is the supported control until the loop is broken. This stays as an opt-in so the
    // behaviour can still be looked at.

    // Take the white point from the game's own exposure texture instead of measuring or guessing.
    // Off by default until it has been seen to work in more than one game.
    CustomOptional<bool> DlssNrWhitePointFromExposure { true };

    // Ask the model, once, whether it will run on Direct3D 11 without the bridge.
    //
    // Off by default and deliberately so. Everything else this pass does reads memory it already owns;
    // this one initialises an NVIDIA subsystem on the game's live D3D11 device, in a process where the
    // D3D12 NGX instance is already running. It should return an error code and nothing more, but
    // "should" is doing work in that sentence and it ships into games nobody can test first.
    CustomOptional<bool> DlssNrProbeD3D11 { false };

    // 0 off, 1 the picture the model was shown, 2 its raw answer, 3 what it changed, amplified.
    CustomOptional<uint32_t> DlssNrDebugView { 0 };

    // Showing the pass against itself, without having to toggle it and remember what the last frame
    // looked like. 0 off, 1 side by side, 2 a wipe.
    //
    // Side by side squeezes the whole frame into each half, so it is a comparison rather than
    // something to play in. The wipe cuts one frame and resamples nothing, so it is; the split is a
    // stored setting and stays where it was put once the menu closes.
    CustomOptional<uint32_t> DlssNrCompare { 0 };
    CustomOptional<float> DlssNrCompareSplit { 0.5f };

    // Side by side only. 1 fits the whole frame at its right shape and accepts the bars; 2 fills
    // the half and crops the sides off instead.
    CustomOptional<float> DlssNrCompareZoom { 1.0f };

    // Which side the edited frame sits on, in both comparison modes.
    CustomOptional<bool> DlssNrCompareSwap { false };

    // Labels drawn onto the two sides of a comparison, so a screenshot still says which is which.
    // Drawn into the frame's own plane with a clip per side: in the wipe they are revealed and hidden
    // by the split exactly as the images are, and there is nothing to drag.
    CustomOptional<bool> DlssNrCompareTags { false };
    CustomOptional<float> DlssNrTagScale { 1.5f };

    // The "NR ON" badge in a corner of the frame while the model runs, so a screenshot or a video says
    // NR was on. Corner: 0 top left, 1 top right, 2 bottom left, 3 bottom right.
    CustomOptional<bool> DlssNrBadge { false };
    CustomOptional<uint32_t> DlssNrBadgeCorner { 1 };
    CustomOptional<float> DlssNrBadgeScale { 1.0f };

    // The fraction of the frame's resolution the model works at. The frame itself is never reduced --
    // only the model's contribution is computed small and enlarged, so the picture underneath is
    // untouched whatever this is set to. 1.0 is full resolution and behaves exactly as before.
    CustomOptional<float> DlssNrWorkingScale { 1.0f };

    // Run the model before the upscaler instead of after it.
    //
    // The model asks for a 1:1 scaling ratio at every quality level it accepts, so the only way to run
    // it on fewer pixels is to hand it a smaller frame. Here that is the game's render-resolution
    // colour buffer, which is also rendered samples rather than the upscaler's reconstruction.
    //
    // Experimental: colour at this point is jittered per frame and the model takes no jitter offset.
    CustomOptional<bool> DlssNrPreUpscale { false };

    // Split the upscaler in two and put the model between the halves.
    //
    // The upscaler is built to write at render resolution instead of display resolution, which for
    // ray reconstruction makes it a denoiser and nothing else. The model then runs on a clean,
    // temporally settled frame at render resolution, and the enlargement happens after it.
    //
    // This is the arrangement that answers the jitter objection to DlssNrPreUpscale: the frame the
    // model is shown here has already been through temporal accumulation, so the subpixel offset the
    // model cannot be told about has been resolved before it ever sees the picture.
    CustomOptional<bool> DlssNrDualFeature { false };

    // Which upscaler performs the enlargement in that arrangement, or none for the spatial output
    // scaler. Read through the same name table as every other upscaler choice, and resolved through
    // the same provider -- so a machine without DLSS is handed FSR here exactly as it is anywhere else.
    //
    // Unset means the spatial scaler: it needs no motion vectors, no depth and no jitter, so it is the
    // one option that cannot be wrong about them. The upscalers are sharper and answer to the jitter
    // question, which the first half has already consumed.
    CustomOptional<Upscaler, NoDefault> DlssNrDualEnlarger;
    // Filter used for NR supersampling (working scale > 1): the model runs above native, and this is
    // the downscaler that averages its answer back to native. Independent of OutputScalingDownscaler
    // so NR and Output Scaling can run different filters at once. Lanczos3 is the sharp default.
    CustomOptional<Scaler> DlssNrScalingDownscaler { Scaler::Lanczos3 };

    // Ask the driver's own nvngx.dll whether it will dispatch Neural Rendering, once per session.
    //
    // Everything here drives the model's DLL directly through a forwarder, because the model refuses
    // callers whose module path does not contain "nvngx.dll". But the model ships inside the driver
    // store, and NVIDIA does not ship a feature DLL that no dispatcher can reach -- so the driver's
    // nvngx.dll may well know feature 18 already. If it does, the forwarder is unnecessary, the
    // signature question disappears, and users stop needing a 165 MB copy in every game folder.
    //
    // Off by default: it is a diagnostic, not a feature.
    CustomOptional<bool> DlssNrProxyProbe { false };

    // Run Neural Rendering through the driver's own nvngx.dll rather than through the forwarder.
    //
    // This is how DLSS itself is called. The forwarder exists only because driving the model
    // directly trips its caller check, and a probe showed the driver dispatches feature 18 already:
    // asking for 18 answers differently from asking for a feature that does not exist. OptiScaler
    // also already tells the driver where to look, since NVNGX_FeatureInfo_Paths carries the game
    // and OptiScaler folders into Init_Ext.
    //
    // Off until it is shown to produce the same picture. If it does, the forwarder can go.
    CustomOptional<bool> DlssNrUseProxy { false };

    // Look for the exposure the game computed but never handed to the upscaler.
    //
    // Off by default, and it has to be. Reading a resource the game owns means assuming what state
    // it is in, and unlike depth and motion vectors -- where NGX documents the contract -- a buffer
    // found by its shape comes with no promise at all. UNORDERED_ACCESS is the reasonable
    // assumption, since every candidate got here by having a UAV made on it, but it is an
    // assumption, and nobody who has not asked for the scan should be carrying that risk.
    //
    // It decides nothing either way. It watches and it reports, because the last two times a number
    // was inferred here it went straight into the interface and was wrong.
    CustomOptional<bool> DlssNrScanExposure { false };

    // Anchoring the scan: the white point that looked right, and the scan's value at that moment.
    //
    // The absolute white point cannot be derived from a buffer whose units are unknown. What CAN be
    // derived is every value after the first: if the scan's number halves, the scene got twice as
    // bright, and the white point follows -- whatever the number actually means, because only the
    // ratio is used and the units cancel.
    //
    // So the user sets it once, in one lighting condition, and presses a button. After that it stays
    // correct through every cave and every noon without being touched again. Which is also the shape
    // that makes per-game profiles work: one person anchors a game, everybody else gets the number.
    //
    // Zero means not anchored, and then nothing happens at all.
    // A lamp in the corner showing what the scan currently thinks the light is doing: red for dark,
    // green for full light, and the shades between. Off by default; it is for watching the thing
    // work, not for playing with.
    // Where the white point comes from. One control, because there is one answer.
    //
    //   0  the paper white slider, and nothing else
    //   1  the exposure the game hands the upscaler
    //   2  a buffer the scan found, anchored to a white point the user chose once
    //
    // This replaces two independent checkboxes that could both be on. They were made exclusive by
    // greying, which deadlocked -- each disabled the other, so once both were set the only way out
    // was a button the notice never mentioned -- and then by clearing, which silently undid a
    // setting the user had made. Both were attempts to stop an illegal state being REACHED. A single
    // choice cannot reach it: there is nothing to keep consistent, because there is only one value.
    CustomOptional<uint32_t> DlssNrWhitePointSource { 1 };

    CustomOptional<bool> DlssNrScanMeter { false };

    CustomOptional<float> DlssNrScanAnchorValue { 0.0f };      // legacy single anchor, migrated then unused
    CustomOptional<float> DlssNrScanAnchorWhitePoint { 0.0f }; // legacy single anchor, migrated then unused

    // The multi-point anchor table, serialised as "scan:white;scan:white;..." ascending. See
    // dlssnr/design/multi-point-anchoring.md. Replaces the single pair above; a pre-existing single
    // anchor is migrated into a one-row table on first load.
    CustomOptional<std::string> DlssNrScanAnchors { std::string() };

    // Whether the scan's number rises or falls with the light.
    //
    // A found buffer carries no contract. Most engines store an exposure -- a multiplier that goes
    // DOWN as the scene gets brighter -- but some store its reciprocal, and nothing in the buffer
    // says which. Rather than guess and be silently wrong in half the games, this is one click: if
    // the picture moves the wrong way, flip it.
    CustomOptional<bool> DlssNrScanInverted { false };

    // The trim on an exposure-derived white point, kept apart from the manual divisor on purpose.
    //
    // These are two different quantities that happened to share one slider: the manual path wants an
    // absolute divisor on an open-ended linear buffer, which in Nioh 3 is about 240, and the exposure
    // path wants a multiplier on a number the game already supplied, where anything far from 1 is
    // a sign the read is wrong rather than a preference. Sharing one stored value meant touching the
    // slider in one mode silently destroyed the number found in the other.
    //
    // 1.0 is the identity: take the game's exposure exactly as given. That is the "safe value", and
    // it is safe by construction rather than by being written down somewhere.
    CustomOptional<float> DlssNrWhitePointTrim { 1.0f };

    // The scan's trim, kept apart from the exposure texture's.
    //
    // They are trims on different things and a value found against one is meaningless against the
    // other. Sharing one slider meant switching source silently carried a number across, so a
    // picture that had been tuned came back wrong for a reason nothing on screen explained.
    CustomOptional<float> DlssNrScanTrim { 1.0f };

    // How many times to run the model over the same frame, each pass shown the last one's answer.
    //
    // A count of features, not a setting on one: every pass has its own NGX feature carrying its own
    // temporal history, and each is built on a frame of its own before it is first evaluated. The
    // proxy the composition differences against is written once, by the encode, and never by the
    // chain, so what the composition receives is the whole chain's edit against the frame's own
    // picture rather than the last pass's edit against the one before it.
    //
    // 1 is what the model was trained for. Above that it is being asked to enhance its own output,
    // which is outside its training distribution: detail compounds, and so does anything it got
    // wrong. The ceiling is DlssNr::kMaxPasses.
    //
    // The cost is exactly linear -- the model is 98% of the frame's expense and every pass pays it
    // again. The passes are sequential and each one needs the last one's output, so there is no
    // amortisation. VRAM grows with the count as well: a feature's history is its own.
    CustomOptional<uint32_t> DlssNrPasses { 1 };

    // Which depth convention the model is told the guide uses.
    //
    //   0  what the game's own DLSS feature was created with, which is what it means for the upscaler
    //   1  force normal
    //   2  force inverted
    //
    // Writes one set of matched before/after frames per session, without anyone having to ask. The
    // folder is cleared at the start of each run, so it holds one session's worth and never grows.
    CustomOptional<bool> DlssNrAutoCapture { true };

    // Multiplies the (auto or manual) white point before the encode: what the model considers "white".
    // Higher means highlights sit lower on the curve and the model treats them as less extreme.
    CustomOptional<float> DlssNrWhitePointScale { 1.0f };

    // --- end DLSS 5 Neural Rendering -------------------------------------------------------------

    // DLSS
    CustomOptional<bool> DLSSEnabled { true };
    CustomOptional<bool> RenderPresetOverride { false };
    CustomOptional<uint32_t> RenderPresetForAll { 0 };
    CustomOptional<uint32_t> RenderPresetDLAA { 0 };
    CustomOptional<uint32_t> RenderPresetUltraQuality { 0 };
    CustomOptional<uint32_t> RenderPresetQuality { 0 };
    CustomOptional<uint32_t> RenderPresetBalanced { 0 };
    CustomOptional<uint32_t> RenderPresetPerformance { 0 };
    CustomOptional<uint32_t> RenderPresetUltraPerformance { 0 };

    // DLSSD
    CustomOptional<bool> DLSSDRenderPresetOverride { false };
    CustomOptional<uint32_t> DLSSDRenderPresetForAll { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetDLAA { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetUltraQuality { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetQuality { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetBalanced { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetPerformance { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetUltraPerformance { 0 };

    // Nukems
    CustomOptional<bool> NvngxFGMakeDepthCopy { false };

    // Libraries
    CustomOptional<std::wstring, NoDefault> MainDllPath;
    CustomOptional<std::wstring, NoDefault> FfxDx12Path;
    CustomOptional<std::wstring, NoDefault> FfxDx12SRPath;
    CustomOptional<std::wstring, NoDefault> FfxDx12FGPath;
    CustomOptional<std::wstring, NoDefault> FfxDx12RRPath;
    CustomOptional<std::wstring, NoDefault> FfxDx12RCPath;
    CustomOptional<std::wstring, NoDefault> FfxVkPath;
    CustomOptional<std::wstring, NoDefault> XeSSLibrary;
    CustomOptional<std::wstring, NoDefault> XeFGLibrary;
    CustomOptional<std::wstring, NoDefault> XeLLLibrary;
    CustomOptional<std::wstring, NoDefault> XeSSDx11Library;
    CustomOptional<std::wstring, NoDefault> NvngxPath;
    CustomOptional<std::wstring, NoDefault> NVNGX_DLSS_Library;
    CustomOptional<std::wstring, NoDefault> DLSSFeaturePath;
    CustomOptional<std::wstring, NoDefault> NvapiDllPath;

    // Sharpness
    CustomOptional<SharpenShader> SharpnessShader { SharpenShader::RCAS };
    CustomOptional<bool> OverrideSharpness { false };
    CustomOptional<float> Sharpness { 0.4f };

    // RCAS
    CustomOptional<bool> RcasEnabled { false };
    CustomOptional<bool> ContrastEnabled { false };
    CustomOptional<float> Contrast { -0.3f };

    // DA Sharpening
    CustomOptional<float, NoDefault> DADepthScale;
    CustomOptional<float, NoDefault> DADepthBias;
    CustomOptional<bool, NoDefault> DAClampOutput;
    CustomOptional<bool> UseDepthAwareSharpen { false };
    CustomOptional<bool> UseDASDepthAwareSharpen { false };
    CustomOptional<bool> DADepthIsLinear { false };

    // MAS
    CustomOptional<bool> MotionSharpnessEnabled { false };
    CustomOptional<bool> MotionSharpnessDebug { false };
    CustomOptional<float> MotionSharpness { 0.2f };
    CustomOptional<float> MotionThreshold { 0.0f };
    CustomOptional<float> MotionScaleLimit { 10.0f };

    // Magnifier
    CustomOptional<bool> MagnifierEnabled { false };
    CustomOptional<float> MagnifierSize { 15.f }; // % of screen Height
    CustomOptional<int> MagnifierZoomFactor { 4 };
    CustomOptional<float> MagnifierBorderSize { 0.3f };   // % of screen Height
    CustomOptional<float> MagnifierCursorOffsetX { 0.f }; // Pixels
    CustomOptional<float> MagnifierCursorOffsetY { 0.f }; // Pixels
    CustomOptional<float, NoDefault> MagnifierStaticPosX; // % of screen Width, static pos enabled if both are defined
    CustomOptional<float, NoDefault> MagnifierStaticPosY; // % of screen Height

    // Menu
    CustomOptional<float, NoDefault> MenuScale;
    // Main overlay size in pixels (saved when "Save Settings" is pressed)
    CustomOptional<float, NoDefault> MenuWidth;
    CustomOptional<float, NoDefault> MenuHeight;
    // Side of the screen the main menu is docked to, as ReShade docks its overlay: 0 floating (centred until moved),
    // 1 left, 2 right, 5 top, 6 bottom (3 and 4, the bottom corners before, are left and right). Docking or floating
    // the menu by dragging it sets this.
    CustomOptional<int> MenuAnchor { 0 };
    // Order of the menu's side bar buttons, their short names comma-separated ("NR,UP,FG,IMG,UI,ADV"). Dragging a
    // button up or down sets it; a name left out goes after the ones given.
    CustomOptional<std::string> MenuSectionOrder { "" };
    // The menu opens on the Basic page (true) or on the sections down the side (false).
    CustomOptional<bool> MenuBasic { true };
    CustomOptional<bool, SoftDefault> OverlayMenu { true };
    CustomOptional<int> ShortcutKey { VK_INSERT };
    CustomOptional<bool> ExtendedLimits { false };
    CustomOptional<bool> ShowFps { false };
    /// 0 Top Left, 1 Top Right, 2 Bottom Left, 3 Bottom Right
    CustomOptional<FpsOverlayPos> FpsOverlayPosition { FpsOverlayPos_TopLeft };
    /// 0 Only FPS, 1 +Avg FPS & Upscaler info 2 +Frame Time,
    /// 3 +Upscaler Time, 4 +Frame Time Graph, 5 +Upscaler Time Graph
    /// 6 +Reflex timings
    CustomOptional<FpsOverlay> FpsOverlayType { FpsOverlay_JustFPS };
    CustomOptional<int> FpsShortcutKey { VK_PRIOR };
    CustomOptional<int> FpsCycleShortcutKey { VK_NEXT };
    CustomOptional<bool> FpsOverlayHorizontal { false };
    CustomOptional<bool> FpsOverlayShowRatio { false };
    CustomOptional<float> FpsOverlayAlpha { 0.4f };
    CustomOptional<float, NoDefault> FpsScale; // No value means same as MenuScale
    CustomOptional<bool> UseHQFont { true };
    CustomOptional<bool> DisableSplash { false };
    CustomOptional<float> FontSize { 14.0f };
    CustomOptional<std::wstring, NoDefault> TTFFontPath;
    // A language code of the menu's tables. No value means the language of Windows
    CustomOptional<std::string, NoDefault> MenuLanguage;
    CustomOptional<int> FGShortcutKey { VK_END };
    // Saves the presented frame without OptiScaler's overlays. ScreenshotPath empty means
    // <Pictures>\OptiScaler\<exe name>. With the menu or the FPS overlay shown, ScreenshotWithMenu adds a copy with it.
    CustomOptional<int> ScreenshotKey { VK_F6 };
    CustomOptional<std::wstring, NoDefault> ScreenshotPath;
    CustomOptional<bool> ScreenshotWithMenu { true };
    CustomOptional<bool> LightTheme { false };
    CustomOptional<bool> OverlaysUseTheme { true };
    CustomOptional<float> MenuAccentColorR { 0.86f };
    CustomOptional<float> MenuAccentColorG { 0.09f };
    CustomOptional<float> MenuAccentColorB { 0.12f };
    CustomOptional<float> MenuBGColorR { 0.0f };
    CustomOptional<float> MenuBGColorG { 0.0f };
    CustomOptional<float> MenuBGColorB { 0.0f };
    CustomOptional<float> MenuBGColorA { 0.93f };
    CustomOptional<bool> MenuBlur { true };
    CustomOptional<float> MenuBlurStrength { 1.0f };
    CustomOptional<bool> CustomTabEnabled { true };
    CustomOptional<std::string> CustomTabCards { "upscaler,sharpness,fg_selection,fg_fsrfg,fg_xefg,fg_dlssg,fg_"
                                                 "reprojection,fg_optifg,fg_nvngx,fg_fsrfg_inputs,fg_sl_"
                                                 "inputs,framerate" }; // Comma separated ids of the boxes

    // Hooks
    CustomOptional<bool> HookOriginalNvngxOnly { false };
    CustomOptional<bool> EarlyHooking { false };
    CustomOptional<bool, SoftDefault> UseNtdllHooks { true };

    // Upscale Ratio Override
    CustomOptional<bool> UpscaleRatioOverrideEnabled { false };
    CustomOptional<float> UpscaleRatioOverrideValue { 1.3f };

    // DRS
    CustomOptional<bool> DrsMinOverrideEnabled { false };
    CustomOptional<bool> DrsMaxOverrideEnabled { false };

    // Quality Overrides
    CustomOptional<bool> QualityRatioOverrideEnabled { false };
    CustomOptional<float> QualityRatio_DLAA { 1.0f };
    CustomOptional<float> QualityRatio_UltraQuality { 1.3f };
    CustomOptional<float> QualityRatio_Quality { 1.5f };
    CustomOptional<float> QualityRatio_Balanced { 1.7f };
    CustomOptional<float> QualityRatio_Performance { 2.0f };
    CustomOptional<float> QualityRatio_UltraPerformance { 3.0f };

    // ProcessFilter
    CustomOptional<std::wstring, NoDefault> TargetProcess;
    CustomOptional<std::wstring> ProcessExclusionList = {
        L"crashpad_handler.exe|crashreport.exe|crashreporter.exe|crs-handler.exe|crs-uploader.exe|crs-video.exe|"
        L"unitycrashhandler64.exe|idtechlauncher.exe|cefviewwing.exe|ace-setup64.exe|ace-service64.exe|"
        L"qtwebengineprocess.exe|platformprocess.exe|bugsplathd64.exe|bssndrpt64.exe|pspcsdkappmgr.exe|pspcsdkcore.exe|"
        L"pspcsdkstttts.exe|pspcsdktelemetry.exe|pspcsdkui.exe|pspcsdkupdatechecker.exe|pspcsdkvoicechat.exe|"
        L"pspcsdkwebview.exe|windhawk.exe|vscodium.exe|crash_reporter.exe|steamerrorreporter64.exe|crashreportclient."
        L"exe|edcefcrashpadprocess.exe|edcefrenderprocess.exe|unicrashreporter.exe"
    };

    // Hotfixes
    CustomOptional<bool> CheckForUpdate { true };
    CustomOptional<bool, SoftDefault> DisableOverlays { false };

    CustomOptional<bool> SimulateWaitableObject { false };

    CustomOptional<float, NoDefault> MipmapBiasOverride; // disabled by default
    CustomOptional<bool> MipmapBiasFixedOverride { false };
    CustomOptional<bool> MipmapBiasScaleOverride { false };
    CustomOptional<bool> MipmapBiasOverrideAll { false };

    CustomOptional<int, NoDefault> AnisotropyOverride; // disabled by default
    CustomOptional<bool> OverrideShaderSampler { true };
    CustomOptional<bool> AnisotropyModifyComp { true };
    CustomOptional<bool> AnisotropyModifyMinMax { true };
    CustomOptional<bool> AnisotropySkipPointFilter { true };

    CustomOptional<int, NoDefault> RoundInternalResolution; // disabled by default

    CustomOptional<int, NoDefault> SkipFirstFrames; // disabled by default
    CustomOptional<bool, SoftDefault> RestoreComputeSignature { false };
    CustomOptional<bool, SoftDefault> RestoreGraphicSignature { false };
    CustomOptional<bool, SoftDefault> ExtendedStateRestore { false };

    CustomOptional<bool> UsePrecompiledShaders { true };

    CustomOptional<bool> UseGenericAppIdWithDlss { false };
    CustomOptional<bool> PreferDedicatedGpu { true };
    CustomOptional<bool> PreferFirstDedicatedGpu { false };

    CustomOptional<int32_t, NoDefault> ColorResourceBarrier;    // disabled by default
    CustomOptional<int32_t, NoDefault> MVResourceBarrier;       // disabled by default
    CustomOptional<int32_t, NoDefault> DepthResourceBarrier;    // disabled by default
    CustomOptional<int32_t, NoDefault> ExposureResourceBarrier; // disabled by default
    CustomOptional<int32_t, NoDefault> MaskResourceBarrier;     // disabled by default
    CustomOptional<int32_t, NoDefault> OutputResourceBarrier;   // disabled by default

    CustomOptional<bool, SoftDefault> CreateD3D12DeviceForLuma { false };

    // Upscalers
    CustomOptional<Upscaler, SoftDefault> Dx11Upscaler { Upscaler::FSR22 };
    CustomOptional<Upscaler, SoftDefault> Dx12Upscaler { Upscaler::XeSS };
    CustomOptional<Upscaler, SoftDefault> VulkanUpscaler { Upscaler::FSR22 };

    // Output Scaling
    CustomOptional<bool> OutputScalingEnabled { false };
    CustomOptional<float> OutputScalingMultiplier { 1.5f };
    CustomOptional<Scaler> OutputScalingDownscaler { Scaler::FSR1 };

    // FSR
    CustomOptional<bool> FsrDebugView { false };
    CustomOptional<bool> Fsr4EnableDebugView { false };
    CustomOptional<int> FfxUpscalerIndex { 0 };
    CustomOptional<int> FfxFGIndex { 0 };
    CustomOptional<bool> FsrUseMaskForTransparency { true };
    CustomOptional<bool> FsrNonLinearColorSpace { false };
    CustomOptional<bool> FsrNonLinearSRGB { false };
    CustomOptional<bool> FsrNonLinearPQ { false };
    CustomOptional<bool> FsrAgilitySDKUpgrade { false };

    // These default values will be overwritten at upscaler init time with optimized values
    CustomOptional<float> FsrVelocity { 1.0f };
    CustomOptional<float> FsrReactiveScale { 1.0f };
    CustomOptional<float> FsrShadingScale { 1.0f };
    CustomOptional<float> FsrAccAddPerFrame { 0.333f };
    CustomOptional<float> FsrMinDisOccAcc { -0.333f };

    // OpenRR: Ray Reconstruction on the DLSS-RR network (OpenNrRrRuntime.dll), D3D12, RDNA4. On by default: it
    // only takes over where the runtime accepts the GPU and nvngx_dlssd.dll, FSR-RR otherwise
    CustomOptional<bool> OpenNrRrEnabled { true };
    // mochizuki NR on the denoised render-size image, between the network and its temporal post: off or display (the
    // network keeps its own history, the shown image adds the NR as a delta)
    CustomOptional<std::string> OpenNrRrNrOnDenoised { "display" };
    // what that NR reads on the D3D12 backend: denoised (the network's image, jittered) or output (an experiment: RR's
    // finished picture at the render size, no jitter, the change kept in a history on OptiScaler's side)
    CustomOptional<std::string> OpenNrRrNrImage { "denoised" };
    // denoised: how fast the NR's delta follows where the camera moves, 0.01..1; 0.3 is the behaviour before 0.05
    CustomOptional<float> OpenNrRrNrDeltaRate { 0.05f };
    // new (the runtime's cheaper kernels, a small measured loss of precision), fast (the experimental level above it, a
    // larger loss) or legacy, flipped live from the menu. Anything else is new: an old Mode=faithful means nothing now
    CustomOptional<std::string> OpenNrRrMode { "new" };
    // experimental: part of the New/Fast network in FP8, flipped live from the menu; Legacy ignores it
    CustomOptional<bool> OpenNrRrFp8 { false };
    // what runs the network: auto (D3D12 in the game's own command list when the runtime's check passes, otherwise
    // Vulkan), d3d12 or vulkan; read when the runtime is created
    CustomOptional<std::string> OpenNrRrBackend { "auto" };
    // Vulkan games: the layout the game keeps the guides in at the evaluate (read = SHADER_READ_ONLY_OPTIMAL, as NGX
    // takes them; general = GENERAL)
    CustomOptional<std::string> OpenNrRrVkInputLayout { "read" };
    // diagnostics (RE Requiem's rain): feed DLSS.DisocclusionMask and the SSS guide (or ColorBeforeFog) to the
    // network, and read the mask as 1 - mask; applied on the next frame. The scale multiplies the mask and the clamp
    // keeps it in 0..1 afterwards (RE Requiem writes values up to 3000; DLSS-RR takes them as they are)
    CustomOptional<bool> OpenNrRrUseDisocclusionMask { true };
    CustomOptional<bool> OpenNrRrUseSssGuide { true }; // RE Requiem's image needs it; off only to compare
    CustomOptional<bool> OpenNrRrInvertDisocclusionMask { false };
    CustomOptional<bool> OpenNrRrClampDisocclusionMask { true };
    CustomOptional<float> OpenNrRrDisocclusionMaskScale { 1.0f };
    // the rest of the DLSS-D inputs nvngx_dlssd.dll feeds rr2 (docs/NOTA-ENTRADAS.md in OpenNR-Denoiser), each used when
    // the game gives it; off for diagnosis, applied on the next frame
    CustomOptional<bool> OpenNrRrUseResponsivityMask { true };
    CustomOptional<bool> OpenNrRrUseColorBeforeTransparency { true };
    CustomOptional<bool> OpenNrRrUseDepthOfFieldGuide { true };
    CustomOptional<bool> OpenNrRrUseReflectionMotionVectors { true };
    CustomOptional<bool> OpenNrRrUseLayerOpacityAndMotion { true };

    // FSR-RR
    CustomOptional<int> FfxDenoiserIndex { 0 };
    CustomOptional<uint64_t> FfxDenoiserDebugMode { 0 };
    // -1: overview, 0..FFX_API_DENOISER_DEBUG_VIEW_MAX_VIEWPORTS-1: fullscreen viewport
    CustomOptional<int> FfxDenoiserDebugViewport { -1 };
    // Enables AMD's internal RR debug descriptors. Requires context recreation.
    CustomOptional<bool> FfxDenoiserInternalDebugViews { false };
    CustomOptional<int> FfxDenoiserDiffuseSignalType { 0 };  // 0: Direct, 1: Indirect
    CustomOptional<int> FfxDenoiserSpecularSignalType { 1 }; // 0: Direct, 1: Indirect
    // Single-signal denoising: dispatch only the enabled signals. A title whose raw
    // signal content only makes sense for one of the two can denoise the surviving
    // signal alone; the disabled signal's output stays zero and composition falls
    // back to the floor/raw-correlation path for it. Requires context recreation.
    CustomOptional<bool> FfxDenoiserDenoiseDiffuse { true };
    CustomOptional<bool> FfxDenoiserDenoiseSpecular { true };
    // Uses only semantic Streamline AO noisy/denoised tags. Resource-inspector
    // candidates are deliberately never promoted to signal inputs.
    // A Streamline title tags NormalRoughness where its normals and roughness actually live. That
    // is more authoritative than the NGX normals binding, which some titles leave as a normals-only
    // texture whose alpha carries nothing -- a 2-bit alpha reads as roughness 0 everywhere and the
    // denoiser then has no material guidance at all. Preferred only when the NGX path published no
    // separate roughness, so a title that supplies one properly is untouched.
    // Off by default: written for a title whose NGX normals looked like they carried no roughness,
    // which turned out to be an external mod zeroing the buffers rather than the binding. It has
    // never been seen to fire, so it stays available rather than assumed.
    CustomOptional<bool> FfxDenoiserTaggedNormalRoughness { false };
    CustomOptional<bool> FfxDenoiserTaggedAmbientOcclusion { false };
    // Disabled preserves the existing contract that RR input normals are world-space.
    CustomOptional<bool> FfxDenoiserNormalsInViewSpace { false };
    // Prefer the title's own linearised view depth when it publishes one, instead of
    // deriving it from the title's depth buffer and the projection. Off by default: the
    // derived path is the one every title so far has been validated against.
    CustomOptional<bool> FfxDenoiserUseTitleLinearDepth { false };
    // Responsivity values on the unstable side of this threshold route the specular
    // radiance through the spatial path as well. Zero disables the test.
    CustomOptional<float> FfxDenoiserResponsivityThreshold { 0.0f };
    // Selects which side of that threshold counts as unstable, since the polarity of the
    // title's mask is a title property.
    CustomOptional<bool> FfxDenoiserResponsivityInvert { false };

    // Routes pixels flagged by the DLSS bias-current-color mask (particles, alpha layers,
    // animated and video textures) around the denoiser via the floor and skip signal.
    // 0 restores the behaviour where the mask was bound but unused.
    CustomOptional<float> FfxDenoiserBiasMaskStrength { 1.0f };

    // Fraction of the floor filter's high-frequency luminance residual pushed back into the
    // floor on the final pass, so texture microcontrast bypasses the denoiser.
    CustomOptional<float> FfxDenoiserFloorDetailBoost { 0.0f };

    // Exponent on the floor filter's normal edge-stopping weight. Higher stops harder at
    // creases and silhouettes; 0 disables the term.
    CustomOptional<float> FfxDenoiserFloorNormalSharpness { 16.0f };

    // Fraction of the floor's luminance edge stop released where diffuse albedo says two taps
    // share a material, so shadows and reflections reach the denoiser instead of the floor.
    CustomOptional<float> FfxDenoiserFloorAlbedoGuide { 1.0f };

    // Blends the floor's luminance normaliser from centre-only (0) to max(centre, tap) (1).
    CustomOptional<float> FfxDenoiserFloorLumSymmetry { 1.0f };

    // Additional normal edge-stop exponent in proportion to screen-space surface slope.
    CustomOptional<float> FfxDenoiserFloorGrazingSharpness { 0.0f };
    // How far each a-trous pass returns a downward-biased estimate instead of the bilateral
    // mean, bounding the floor below the raw colour. Off by default; the crossing it removes
    // measures ~0.1% of a typical frame, so it is a bound rather than a fix. The knob that
    // fixed this title's floor softness is FloorDetailBoost.
    CustomOptional<float> FfxDenoiserFloorEnvelopeBias { 0.0f };

    // Soft knee on the opt-in floor/raw ceiling clamp. 0 is the exact min().
    CustomOptional<float> FfxDenoiserFloorSoftMin { 0.0f };

    // Overrides the DLSS.Use.HW.Depth interpretation. Unset follows NGX, which
    // defaults to linear when the title publishes nothing - and reading a hardware
    // depth buffer as linear collapses the whole scene to sub-unit distances.
    CustomOptional<bool, NoDefault> FfxDenoiserHardwareDepth;

    // Pushes AMD's own queried baseline for the six tunable RR keys instead of the
    // values below. A/B reference only - the fork's defaults remain the shipping
    // configuration, and the sliders keep their values while this is enabled.
    CustomOptional<bool> FfxDenoiserUseAmdDefaults { false };

    CustomOptional<float> FfxDenoiserDisocThreshold { 0.1f };
    CustomOptional<float> FfxDenoiserCrossBlNormStr { 0.5f };
    CustomOptional<float> FfxDenoiserStabilityBias { 0.5f };
    CustomOptional<float> FfxDenoiserMaxRadiance { 4e4f };
    CustomOptional<float> FfxDenoiserRadianceClip { 40.0f };
    CustomOptional<float> FfxDenoiserGaussKernRelax { 0.5f };
    CustomOptional<float> FfxDenoiserDebugDepthMax { 1024.0f };

    // Records the probe readbacks: seven render targets per input probe interval plus two per
    // denoiser output probe, and the log lines that report them. Off by default - the numbers
    // are for diagnosis, and an always-on probe wrote a gigabyte of log in a session.
    CustomOptional<bool> FfxDenoiserDiagnostics { false };

    CustomOptional<float> FfxDenoiserCorrelationBias { 1.0f };
    // Binds the title's diffuse ray length into the diffuse signal's alpha. Without
    // it that alpha is a constant FP16-max "ray miss", which is what RR's non-PSR
    // reflection handling reads. No effect when the title provides no such resource.
    CustomOptional<bool> FfxDenoiserDiffuseHitDistance { true };
    CustomOptional<float> FfxDenoiserFloorIsolation { 1.0f };
    CustomOptional<float> FfxDenoiserRoughnessFloor { 0.1f };
    // Hands exact-zero-roughness (type-1) pixels to the spatial floor instead of RR.
    // Which pixels take the floor handover - the graft that recombines RR's low frequencies
    // with the floor's high frequencies in composition. Because that combination happens
    // after denoising it never removes anything from the denoiser's input, which is what
    // makes it safe to widen beyond the zero-roughness pixels it was written for.
    // 0 = off, 1 = exact-zero-roughness (type-1) pixels only, 2 = every pixel.
    CustomOptional<int> FfxDenoiserFloorHandover { 1 };
    // Scales the graft weight so the handover can be applied partially. 1.0 is the full
    // handover and 0.0 is inert.
    CustomOptional<float> FfxDenoiserFloorHandoverStrength { 1.0f };

    // Scales the raw-preserving blend inside the floor. 1.0 keeps the floor's microcontrast
    // where the guide allows it; 0.0 leaves a pure spatial floor, which is also how the
    // blend's contribution to the image can be removed outright for comparison.
    CustomOptional<float> FfxDenoiserFloorRawBlend { 1.0f };

    // Floor on the albedo used as the demodulation divisor. The floor caps the gain on
    // dark surfaces, but everything it cannot represent is handed to the skip signal, which
    // reaches the screen without passing the denoiser - so a high floor trades amplified
    // noise inside the denoiser for unfiltered noise beside it. Lowering it keeps the
    // demodulate/remodulate round trip faithful.
    CustomOptional<float> FfxDenoiserDemodDivisorFloor { 8e-3f };

    // Opt-in energy guard on the floor/raw clamp: the floor may not exceed the raw, so the
    // share that does is replaced with a low pass of the raw. The replacement is the raw's own
    // noise and the skip signal publishes it unfiltered, so enabling this republishes the raw's
    // grain on exactly the pixels it clamps; averaging the ceiling attenuates that but cannot
    // remove it. 0 - the default - leaves the floor unclamped and closes the residual instead.
    CustomOptional<float> FfxDenoiserFloorClampSmoothing { 0.0f };

    // How far the diffuse albedo's local structure suppresses that blend. The blend's test is
    // whether the raw sample resembles the floor, which on a noisy input the noise itself
    // answers, so flat surfaces pass raw grain through while the floor stays smooth. Where
    // albedo shows structure the raw sample carries real detail and is kept; where it does not,
    // the variation is not material and is refused. 0.0 reproduces the ungated behaviour.
    CustomOptional<float> FfxDenoiserFloorStructureGate { 1.0f };
    // Blends that handover between FloorSeed's isotropic floor (0), which erases thin
    // structure, and a directional hybrid median that preserves panel text (1).
    CustomOptional<float> FfxDenoiserFloorHandoverDetail { 1.0f };
    // Band-split only. Replaces the single mid band with the floor chain's own five
    // a-trous detail levels, each shrunk against its own threshold. The chain is
    // already this decomposition, so the mid band is a one-boundary approximation of
    // a five-boundary split that exists either way.
    // Handover refinements, each inert at zero and composing with the blend mode
    // rather than replacing it.
    CustomOptional<float> FfxDenoiserFloorHandoverAnchorClamp { 2.0f };
    CustomOptional<float> FfxDenoiserFloorHandoverCorrelationMix { 1.0f };
    // Per-level thresholds, finest (a-trous stride 1) first, in the same multiples of
    // the floor's local spread as the thresholds above. Noise is broadband and panel
    // structure is not, so the noise-to-signal ratio is worst at the finest level -
    // hence the descending defaults.

    // FSR4
    // Overrides the FSR 4 support detected for the GPU
    CustomOptional<FSR4Support> Fsr4GpuSupport { FSR4Support::None };
    CustomOptional<uint32_t, NoDefault> Fsr4Preset;
    CustomOptional<FSR4Model, NoDefault> Fsr4Model;
    CustomOptional<bool> Fsr4EnableWatermark { false };
    CustomOptional<bool, SoftDefault> Fsr4DoNotLoadAmdxc64 { false };

    // FSR Common
    CustomOptional<float> FsrVerticalFov { 60.0f };
    CustomOptional<float> FsrHorizontalFov { 0.0f }; // off by default
    CustomOptional<float> FsrCameraNear { 0.1f };
    CustomOptional<float> FsrCameraFar { 100000.0f };
    CustomOptional<bool, SoftDefault> FsrUseFsrInputValues { true };

    // dx11wdx12
    CustomOptional<bool> Dx11DelayedInit { false };
    CustomOptional<bool, SoftDefault> DontUseNTShared { true };

    // vulkanwdx12
    CustomOptional<bool> VulkanUseCopyForInputs { false };
    CustomOptional<bool> VulkanUseCopyForOutput { false };

    // NVAPI Override
    CustomOptional<bool> DisableFlipMetering { false };
    CustomOptional<bool> DisableReflexSync { false };
    CustomOptional<bool> DisableOTA { false };

    // Spoofing
    CustomOptional<bool, SoftDefault> DxgiSpoofing { true };
    CustomOptional<bool> DxgiFactoryWrapping { false };
    CustomOptional<bool, SoftDefault> StreamlineSpoofing { true };
    CustomOptional<std::string, NoDefault> DxgiBlacklist; // disabled by default
    CustomOptional<int, NoDefault> DxgiVRAM;              // disabled by default
    CustomOptional<bool, SoftDefault> VulkanSpoofing { false };
    CustomOptional<bool, SoftDefault> VulkanExtensionSpoofing { false };
    CustomOptional<int, NoDefault> VulkanVRAM; // disabled by default
    CustomOptional<bool> SpoofHAGS { false };
    CustomOptional<bool> SpoofFeatureLevel { false };
    CustomOptional<uint32_t> SpoofedVendorId { VendorId::Nvidia };
    CustomOptional<uint32_t> SpoofedDeviceId { 0x2B85 };
    CustomOptional<uint32_t, NoDefault> TargetVendorId;
    CustomOptional<uint32_t, NoDefault> TargetDeviceId;
    CustomOptional<std::wstring> SpoofedGPUName { L"NVIDIA GeForce RTX 5090" };
    CustomOptional<bool> UESpoofIntelAtomics64 { false };
    CustomOptional<bool> SpoofRegistry { false };
    CustomOptional<bool> SpoofUser32 { false };
    CustomOptional<std::wstring> SpoofedDriver { L"32.0.15.9155" };

    // Plugins
    CustomOptional<std::wstring, NoDefault> PluginPath;
    CustomOptional<bool> LoadSpecialK { false };
    CustomOptional<bool> LoadReShade { false };
    CustomOptional<bool> LoadCustomAmdxc64OnRdna2 { false };
    CustomOptional<bool> LoadAsiPlugins { false };
    CustomOptional<int> LateAsiPluginsDelay { 30 };

    // Frame Generation
    CustomOptional<FGInput> FGInput { FGInput::NoFG };
    CustomOptional<bool> ExternalFrameGeneration { false };
    CustomOptional<FGOutput> FGOutput { FGOutput::NoFG };
    CustomOptional<FGNvngxReplacement> FGNvngxReplacement { FGNvngxReplacement::Nukems };
    CustomOptional<bool> FGDrawUIOverFG { false };
    CustomOptional<bool> FGUIPremultipliedAlpha { true };
    CustomOptional<bool, SoftDefault> FGDisableHudless { false };
    CustomOptional<bool> FGDisableUI { false };
    CustomOptional<bool> FGSkipReset { false };
    CustomOptional<int> FGAllowedFrameAhead { 1 };
    CustomOptional<bool> FGDepthValidNow { false };
    CustomOptional<bool> FGVelocityValidNow { false };
    CustomOptional<bool> FGHudlessValidNow { false };
    CustomOptional<bool> FGOnlyAcceptFirstHudless { false };
    CustomOptional<bool, SoftDefault> FGPreserveSwapChain { true };
    CustomOptional<bool, SoftDefault> FGSkipResizeBuffers { false };
    CustomOptional<bool> FGModifyBufferState { false };
    CustomOptional<bool> FGModifySCIndex { false };
    CustomOptional<float> FGHudCutoff { 0.0f };
    CustomOptional<FrameTimeSource> FTInput { FrameTimeSource::Input };
    CustomOptional<bool> FGReprojectionEnabled { false }; // WIP Not saved or loaded

    // OptiFG
    // Unset (auto) follows the game's DLSSG state when FG Input is DLSSG, use IsFGEnabled() to read
    CustomOptional<bool, SoftDefault> FGEnabled { false };
    CustomOptional<bool> FGUseMutexForSwapchain { true };
    CustomOptional<bool> FGMakeMVCopy { true };
    CustomOptional<bool> FGMakeDepthCopy { true };
    CustomOptional<bool> FGResourceFlip { false };
    CustomOptional<bool> FGResourceFlipOffset { false };
    CustomOptional<bool, SoftDefault> FGAlwaysCaptureFSRFGSwapchain { false };

    CustomOptional<int, NoDefault> FGRectLeft;
    CustomOptional<int, NoDefault> FGRectTop;
    CustomOptional<int, NoDefault> FGRectWidth;
    CustomOptional<int, NoDefault> FGRectHeight;

    // OptiFG - Hudfix
    CustomOptional<bool, SoftDefault> FGDisableHUDFix { false };
    CustomOptional<bool> FGHUDFix { false };
    CustomOptional<int> FGHUDLimit { 1 };
    CustomOptional<bool> FGHUDFixExtended { false };
    CustomOptional<bool> FGHUDFixUITexture { true };
    CustomOptional<bool, NoDefault> FGHUDFixUIUnblend;
    CustomOptional<bool> FGImmediateCapture { false };
    CustomOptional<bool> FGHudfixPersistentBindings { true };
    CustomOptional<bool> FGDontUseSwapchainBuffers { false };
    CustomOptional<bool> FGRelaxedResolutionCheck { false };
    CustomOptional<bool> FGHudfixDisableRTV { false };
    CustomOptional<bool> FGHudfixDisableSRV { false };
    CustomOptional<bool> FGHudfixDisableUAV { false };
    CustomOptional<bool> FGHudfixDisableOM { false };
    CustomOptional<bool> FGHudfixDisableDispatch { false };
    CustomOptional<bool> FGHudfixDisableDI { false };
    CustomOptional<bool> FGHudfixDisableDII { false };
    CustomOptional<bool> FGHudfixDisableSCR { true };
    CustomOptional<bool> FGHudfixDisableSGR { true };

    // OptiFG - Resource Tracking
    CustomOptional<bool> FGAlwaysTrackHeaps { false };
    CustomOptional<bool> FGResourceBlocking { false };
    CustomOptional<bool> FGUseShards { false };

    // OptiFG - DLSS-D Depth scale
    CustomOptional<bool> FGEnableDepthScale { false };
    CustomOptional<float> FGDepthScaleMax { 10000.0f };

    // FSR-FG
    CustomOptional<bool> FGDebugView { false };
    CustomOptional<bool> FGDebugResetLines { false };
    CustomOptional<bool> FGDebugTearLines { false };
    CustomOptional<bool> FGDebugPacingLines { false };
    CustomOptional<bool> FGAsync { false };
    CustomOptional<bool> FGFramePacingTuning { true };
    CustomOptional<float> FGFPTSafetyMarginInMs { 0.01f };
    CustomOptional<float> FGFPTVarianceFactor { 0.3f };
    CustomOptional<bool> FGFPTAllowHybridSpin { false };
    CustomOptional<int> FGFPTHybridSpinTime { 2 };
    CustomOptional<bool> FGFPTAllowWaitForSingleObjectOnFence { false };
    CustomOptional<bool> FSRFGDisableFlipMetering { false };

    CustomOptional<bool> FSRFGSkipConfigForHudless { false };
    CustomOptional<bool> FSRFGSkipDispatchForHudless { false };
    CustomOptional<bool> FSRFGEnableWatermark { false };

    // XeFG
    //
    // The ceiling for the whole XeFG path, in interpolations: 7 is an 8X
    // multiplier. This is the limit itself, not a sanity bound with the real one
    // living somewhere else - FGXeFGMaxInterpolatedFrames defaults to it, the
    // unlock patches report it to the provider as maxSupportedInterpolations,
    // and XeFG_Dx12 declares it as the swapchain's maxInterpolatedFrames at init,
    // so one number decides what the provider accepts, what the menu offers, and
    // what the ini may ask for.
    //
    // It used to be 31 - a bound nothing enforced, so the menu went to 32X while
    // the provider was still told 31 on every launch. Past 8X the burst outruns
    // any display and the extra frames only buy latency, so 8X is where it stops.
    //
    // The patch that raises the provider's reported maximum writes a 4 byte
    // immediate, so the encoding has never been what limited this.
    static constexpr int32_t XeFGMaxInterpolations = 7;

    CustomOptional<bool> FGXeFGIgnoreInitChecks { false };
    CustomOptional<int> FGXeFGInterpolationCount { 1 };
    CustomOptional<bool> FGXeFGUnlockEnabled { true };
    // What the unlock patch reports to the provider as its maximum, and what
    // the multiplier menu lets you pick. Those are not two settings: the number
    // written into the U3/U4/U5 patches is read back as
    // xefg_swapchain_properties_t::maxSupportedInterpolations, and
    // XeFG_Dx12 fills xefg_swapchain_d3d12_init_params_t::maxInterpolatedFrames
    // from it at swapchain init - so raising the menu ceiling necessarily
    // declares the same number to the provider at init on every launch.
    //
    // It used to be 5, which is what capped the menu at 6X; the ceiling above is
    // now 8X and is what this defaults to. Setting it lower narrows both the menu
    // and what the provider is told, which is the supported way to go back down:
    // if the provider sizes anything from this at init, the symptom would be a
    // failed init or exhausted VRAM, and a value it rejects is clamped in
    // XeFGUnlock to the ceiling rather than passed through.
    CustomOptional<int> FGXeFGMaxInterpolatedFrames { XeFGMaxInterpolations };
    CustomOptional<bool> FGXeFGExtraPacing { true };
    CustomOptional<bool> FGXeFGDynamicMFG { false };          // Count picked up to FGXeFGInterpolationCount
    CustomOptional<float> FGXeFGFramerateTargetDMFG { 0.0f }; // 0 = the FPS limit, else just under the refresh rate
    CustomOptional<int> FGXeFGRoundingDMFG { 1 };             // 0 at most, 1 around, 2 at least the target
    CustomOptional<bool> FGXeFGDynamicAllow1X { true };       // Dynamic MFG may turn frame generation off (1X)
    CustomOptional<bool> FGXeFGTargetAsCapDMFG { false };     // The Dynamic MFG target is also the FPS limit
    CustomOptional<int, NoDefault> FGXeFGOverrideInterpolationCount; // Game's own XeFG (XeFG input), 0 = off
    CustomOptional<bool> FGXeFGUIComposition { false };
    CustomOptional<bool> FGXeFGDepthInverted { true };
    CustomOptional<bool> FGXeFGJitteredMV { false };
    CustomOptional<bool> FGXeFGHighResMV { false };
    CustomOptional<bool> FGXeFGDebugView { false };
    CustomOptional<bool, SoftDefault> FGXeFGForceBorderless { false };

    // DLSSG
    CustomOptional<int> FGDLSSGInterpolationCount { 1 }; // For Opti's own SL instance
    CustomOptional<bool> FGDLSSGUseGamesReflexMarkers { true };
    CustomOptional<int, NoDefault>
        FGDLSSGOverrideInterpolationCount; // For overriding game's value sent to SL, could be Nvngx FG, could be noFG
                                           // but someone just uses real DLSSG
    CustomOptional<bool> FGDLSSGOverrideForceDMFG { false };   // Overrides game's DLSSG mode to Dynamic
    CustomOptional<bool> FGDLSSGForceDMFG { false };           // Overrides Opti's DLSSG mode to Dynamic
    CustomOptional<float> FGDLSSGFramerateTargetDMFG { 0.0f }; // 0.0 means auto-detects the display refresh rate
    CustomOptional<bool> FGDLSSGAdaMfgUnlock { false };
    CustomOptional<bool> FGDLSSGAdaBlackwellKernels { false }; // Blackwell kernels on Ada, see MfgUnlock

    // Reprojection
    CustomOptional<ReprojectionFill> ReprojectionFillMode { ReprojectionFill::Dithering };
    CustomOptional<float> ReprojectionDepthCutoff { 0.1f };
    CustomOptional<uint32_t> ReprojectionCutoffExpand { 1 };
    CustomOptional<bool> ReprojectionLateLatch { true };
    CustomOptional<bool> ReprojectionDirectionCache { true };
    CustomOptional<bool> ReprojectionParallaxAuto { true };   // estimated from the motion vectors
    CustomOptional<float> ReprojectionParallaxScale { 0.0f }; // when not auto, 0 = rotation only
    CustomOptional<bool> ReprojectionThirdPersonDetection { true };

    // As per
    // https://github.com/artur-graniszewski/dlss-enabler-main/blob/a92464d468eb0d91ae17befa66c6bf6229f20b9f/Utils/DlssgProxy.cpp#L1033
    CustomOptional<uint32_t> NvngxFGDispatchFlags { 0x10000000 }; // IGNORE_UI_TEXTURE
    CustomOptional<bool> NvngxFGShowDebug { false };
    CustomOptional<bool> NvngxFGDisableHudless { false };

    // fakenvapi
    CustomOptional<bool, SoftDefault> UseFakenvapi { true };
    CustomOptional<bool> ForceXeLL { false };
    CustomOptional<bool> FN_ForceLatencyFlex { false };
    CustomOptional<LFXMode> FN_LatencyFlexMode { LFXMode::Conservative };
    CustomOptional<ForceReflex> FN_ForceReflex { ForceReflex::InGame };
    CustomOptional<LowLatencyInput> LowLatencyInput { LowLatencyInput::Auto };
    CustomOptional<LowLatencyMode> LowLatencyOutput { LowLatencyMode::Auto };

    // Inputs
    CustomOptional<bool> EnableDlssInputs { true };
    CustomOptional<bool> EnableXeSSInputs { true };
    CustomOptional<bool, SoftDefault> UseFsr2Inputs { true };
    CustomOptional<bool, SoftDefault> UseFsr2Dx11Inputs { false };
    CustomOptional<bool, SoftDefault> UseFsr2VulkanInputs { false };
    CustomOptional<bool, SoftDefault> Fsr2Pattern { false };
    CustomOptional<bool, SoftDefault> UseFsr3Inputs { true };
    CustomOptional<bool, SoftDefault> Fsr3Pattern { false };
    CustomOptional<bool, SoftDefault> UseFfxInputs { true };
    CustomOptional<bool> EnableHotSwapping { false };
    CustomOptional<bool, SoftDefault> EnableFsr2Inputs { true };
    CustomOptional<bool, SoftDefault> EnableFsr3Inputs { true };
    CustomOptional<bool, SoftDefault> EnableFfxInputs { true };

    // Framerate
    CustomOptional<float> FramerateLimit { 0.0f };

    // HDR
    CustomOptional<bool> ForceHDR { false };
    CustomOptional<bool> UseHDR10 { false };
    CustomOptional<bool> SkipColorSpace { false };

    // V-Sync
    CustomOptional<bool, SoftDefault> OverrideVsync { false };
    CustomOptional<bool, NoDefault> ForceVsync;
    CustomOptional<UINT> VsyncInterval { 0 };

    // Old configs for compat reasons
    CustomOptional<bool, NoDefault> _DONTUSE_Fsr4ForceEnableInt8;

    bool LoadFromPath(const wchar_t* InPath);
    bool SaveIni();
    bool SaveXeFG();

    // Keys that aren't OptiScaler options (plugin settings), kept in the ini as they are
    std::optional<std::string> GetRawValue(const char* section, const char* key);
    void SetRawValue(const char* section, const char* key, const char* value);
    void DeleteRawValue(const char* section, const char* key);
    bool SaveRawValues();

    // What the last save could not do, for the menu to say. Empty when OptiScaler.ini took it.
    std::string saveProblem;

    void CheckUpscalerFiles();

    // Applies the FSR-RR denoiser's default profile: every FfxDenoiser* setting
    // goes back to the default declared on its member. The single place that
    // knows the complete setting set and which keys take effect at context
    // creation - the menu's Reset button goes through this instead of keeping
    // its own assignment list. Returns true when a context-creation setting
    // changed and the caller must rebuild the RR context.
    bool ResetFfxDenoiserSettings();
    // FG on/off and interpolation count, following the game's DLSSG state when FGEnabled is unset
    static bool FGCanFollowGame();
    bool FGFollowsGame() const;
    bool IsFGEnabled() const;
    int FGInterpolationCount(int outputCount) const;

    std::vector<std::string> GetConfigLog();

    static Config* Instance();

  private:
    inline static Config* _config;
    inline static std::vector<std::string> _log;

    std::filesystem::path absoluteFileName;
    std::wstring fileName = L"OptiScaler.ini";

    bool Reload(std::filesystem::path iniPath);

    std::optional<std::string> readString(std::string section, std::string key, bool lowercase = false);
    std::optional<std::wstring> readWString(std::string section, std::string key, bool lowercase = false);
    std::optional<float> readFloat(std::string section, std::string key);
    std::optional<int> readInt(std::string section, std::string key);
    // A shortcut: the int form, or a readable one such as Ctrl+Shift+Home (menu/input/shortcut_keys.h).
    std::optional<int> readShortcut(std::string section, std::string key);
    std::optional<uint32_t> readUInt(std::string section, std::string key);
    std::optional<bool> readBool(std::string section, std::string key);

    template <typename Enum> std::optional<Enum> readEnum(std::string section, std::string key);
};
