#include "pch.h"

#include "Config.h"

#include "Util.h"

#include "nvapi/fakenvapi.h"
#include <misc/IdentifyGpu.h>
#include <dlssnr/amd/AmdBridge.h>
#include <menu/input/shortcut_keys.h>

#include <SimpleIni.h>

static CSimpleIniA ini;

// Plugins can read and write the ini from their own threads
static std::recursive_mutex iniMutex;

static inline int64_t GetTicks()
{
    LARGE_INTEGER ticks;

    if (!QueryPerformanceCounter(&ticks))
        return 0;

    return ticks.QuadPart;
}

static inline bool isInteger(const std::string& str, int& value)
{
    std::istringstream iss(str);
    return (iss >> value) && iss.eof();
}

static inline bool isUInt(const std::string& str, uint32_t& value)
{
    std::istringstream iss(str);
    return (iss >> value) && iss.eof();
}

static inline bool isFloat(const std::string& str, float& value)
{
    std::istringstream iss(str);
    return (iss >> value) && iss.eof();
}

// Where a save goes when OptiScaler.ini cannot be overwritten. It counts until the ini is newer, so a hand edit or the
// installer still wins, and a save that reaches the ini again removes it.
static std::filesystem::path FallbackIniPath(const std::filesystem::path& iniPath)
{
    auto path = iniPath;
    return path.replace_extension(L".saved.ini");
}

Config::Config()
{
    absoluteFileName = Util::DllPath().parent_path() / fileName;

    const auto fallback = FallbackIniPath(absoluteFileName);
    std::error_code iniError, fallbackError;
    const auto iniTime = std::filesystem::last_write_time(absoluteFileName, iniError);
    const auto fallbackTime = std::filesystem::last_write_time(fallback, fallbackError);

    if (fallbackError || (!iniError && fallbackTime <= iniTime) || !Reload(fallback))
        Reload(absoluteFileName);
}

// The one authority on "back to defaults" for the FSR-RR denoiser. Clearing
// each optional instead of re-assigning a literal keeps the member
// initializers in Config.h the single source of the default values, so this
// list can state no stale numbers - it only has to stay complete against the
// "FSR-RR" reads in Reload.
bool Config::ResetFfxDenoiserSettings()
{
    const bool contextSettingsChanged =
        // Presence, not the effective value: Auto and an explicit Direct both
        // read as 0 through value_or_default(), yet this reset returns the key
        // to Auto, so any explicit choice - Direct included - is a
        // context-creation change. A rebuilt context re-runs the automatic
        // classification and locks the effective signal type from runtime
        // evidence again; an already-Auto key keeps its locked type and needs
        // no rebuild.
        FfxDenoiserDiffuseSignalType.has_value() || FfxDenoiserSpecularSignalType.has_value() ||
        // Single-signal denoising declares the surviving signals at context
        // creation; only an explicit false differs from the default.
        (FfxDenoiserDenoiseDiffuse.has_value() && !FfxDenoiserDenoiseDiffuse.value()) ||
        (FfxDenoiserDenoiseSpecular.has_value() && !FfxDenoiserDenoiseSpecular.value()) ||
        // The provider index is resolved when the context is created.
        (FfxDenoiserIndex.has_value() && FfxDenoiserIndex.value() != 0) ||
        FfxDenoiserTaggedAmbientOcclusion.value_or_default() || FfxDenoiserNormalsInViewSpace.value_or_default() ||
        FfxDenoiserInternalDebugViews.value_or_default();

    FfxDenoiserIndex.reset();
    FfxDenoiserDebugMode.reset();
    FfxDenoiserDebugViewport.reset();
    FfxDenoiserInternalDebugViews.reset();
    FfxDenoiserDiffuseSignalType.reset();
    FfxDenoiserSpecularSignalType.reset();
    FfxDenoiserDenoiseDiffuse.reset();
    FfxDenoiserDenoiseSpecular.reset();
    FfxDenoiserTaggedNormalRoughness.reset();
    FfxDenoiserTaggedAmbientOcclusion.reset();
    FfxDenoiserNormalsInViewSpace.reset();
    FfxDenoiserUseTitleLinearDepth.reset();
    FfxDenoiserResponsivityThreshold.reset();
    FfxDenoiserResponsivityInvert.reset();
    FfxDenoiserBiasMaskStrength.reset();
    FfxDenoiserFloorDetailBoost.reset();
    FfxDenoiserFloorNormalSharpness.reset();
    FfxDenoiserFloorAlbedoGuide.reset();
    FfxDenoiserFloorLumSymmetry.reset();
    FfxDenoiserFloorGrazingSharpness.reset();
    FfxDenoiserFloorEnvelopeBias.reset();
    FfxDenoiserFloorSoftMin.reset();
    // Back to following NGX; feeds a conversion flag, not context creation.
    FfxDenoiserHardwareDepth.reset();
    FfxDenoiserUseAmdDefaults.reset();
    FfxDenoiserDisocThreshold.reset();
    FfxDenoiserCrossBlNormStr.reset();
    FfxDenoiserStabilityBias.reset();
    FfxDenoiserMaxRadiance.reset();
    FfxDenoiserRadianceClip.reset();
    FfxDenoiserGaussKernRelax.reset();
    FfxDenoiserDebugDepthMax.reset();
    FfxDenoiserDiagnostics.reset();
    FfxDenoiserCorrelationBias.reset();
    FfxDenoiserDiffuseHitDistance.reset();
    FfxDenoiserFloorIsolation.reset();
    FfxDenoiserRoughnessFloor.reset();
    FfxDenoiserFloorHandover.reset();
    FfxDenoiserFloorHandoverStrength.reset();
    FfxDenoiserFloorRawBlend.reset();
    FfxDenoiserDemodDivisorFloor.reset();
    FfxDenoiserFloorClampSmoothing.reset();
    FfxDenoiserFloorStructureGate.reset();
    FfxDenoiserFloorHandoverDetail.reset();
    FfxDenoiserFloorHandoverAnchorClamp.reset();
    FfxDenoiserFloorHandoverCorrelationMix.reset();

    return contextSettingsChanged;
}

bool Config::Reload(std::filesystem::path iniPath)
{
    std::lock_guard<std::recursive_mutex> lock(iniMutex);

    auto pathWStr = iniPath.wstring();

    LOG_INFO("Trying to load ini from: {0}", wstring_to_string(pathWStr));
    if (ini.LoadFile(iniPath.c_str()) == SI_OK)
    {
        State::Instance().nvngxIniDetected = exists(iniPath.parent_path() / "nvngx.ini");
        _log.clear();

        // Upscalers
        {
            // transform converts only when optional has a value
            Dx11Upscaler.set_from_config(readString("Upscalers", "Dx11Upscaler", true).transform(CodeToUpscaler));
            Dx12Upscaler.set_from_config(readString("Upscalers", "Dx12Upscaler", true).transform(CodeToUpscalerFfx));
            VulkanUpscaler.set_from_config(
                readString("Upscalers", "VulkanUpscaler", true).transform(CodeToUpscalerFfx));
        }

        // Frame Generation
        {
            FGEnabled.set_from_config(readBool("FrameGen", "Enabled"));
            ExternalFrameGeneration.set_from_config(readBool("FrameGen", "External"));
            FGDebugView.set_from_config(readBool("FrameGen", "DebugView"));

            FGInput.set_from_config(readString("FrameGen", "FGInput").transform(CodeToEnum<enum FGInput>));
            FGOutput.set_from_config(readString("FrameGen", "FGOutput").transform(CodeToEnum<enum FGOutput>));

            const bool canUseNvngxReplacement =
                FGInput.value_or_default() == FGInput::NvngxFG || FGOutput.value_or_default() == FGOutput::DLSSG;

            if (auto FGNvngxReplacementString = readString("FrameGen", "FGNvngxReplacement");
                canUseNvngxReplacement && FGNvngxReplacementString.has_value())
            {
                if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "none") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::None);
                else if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "nukems") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::Nukems);
                else if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "arturs") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::Arturs);
                else if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "ffx") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::FFX);
                else if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "combo") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::Combo);
                else if (!FGNvngxReplacementString->empty() &&
                         lstrcmpiA(FGNvngxReplacementString->c_str(), "auto") != 0)
                {
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::Custom);
                    State::Instance().fgNvngxCustom = FGNvngxReplacementString.value();
                }
            }

            if (auto forceXell = readBool("fakenvapi", "ForceXeLL"); forceXell.has_value() && forceXell.value())
            {
                FGInput.set_volatile_value(FGInput::ForceXeLL);
                FGOutput.set_volatile_value(FGOutput::XeFG);
            }

            auto ftInput = readInt("FrameGen", "FTInput");
            if (ftInput.has_value() && ftInput.value() >= 0 &&
                ftInput.value() <= (FGOutput.value_or_default() == FGOutput::XeFG ? 2 : 1))
            {
                FTInput.set_from_config(static_cast<FrameTimeSource>(ftInput.value()));
            }

            FGDrawUIOverFG.set_from_config(readBool("FrameGen", "DrawUIOverFG"));
            FGUIPremultipliedAlpha.set_from_config(readBool("FrameGen", "UIPremultipliedAlpha"));
            FGDisableHudless.set_from_config(readBool("FrameGen", "DisableHudless"));
            FGDisableUI.set_from_config(readBool("FrameGen", "DisableUI"));
            FGSkipReset.set_from_config(readBool("FrameGen", "SkipReset"));
            FGRectLeft.set_from_config(readInt("FrameGen", "RectLeft"));
            FGRectTop.set_from_config(readInt("FrameGen", "RectTop"));
            FGRectWidth.set_from_config(readInt("FrameGen", "RectWidth"));
            FGRectHeight.set_from_config(readInt("FrameGen", "RectHeight"));

            FGAllowedFrameAhead.set_from_config(readInt("FrameGen", "AllowedFrameAhead"));
            if (FGAllowedFrameAhead.has_value() && (FGAllowedFrameAhead.value() < 1 || FGAllowedFrameAhead.value() > 3))
                FGAllowedFrameAhead.reset();

            FGDepthValidNow.set_from_config(readBool("FrameGen", "DepthValidNow"));
            FGVelocityValidNow.set_from_config(readBool("FrameGen", "VelocityValidNow"));
            FGHudlessValidNow.set_from_config(readBool("FrameGen", "HudlessValidNow"));
            FGOnlyAcceptFirstHudless.set_from_config(readBool("FrameGen", "OnlyAcceptFirstHudless"));
            FGPreserveSwapChain.set_from_config(readBool("FrameGen", "PreserveSwapChain"));
            FGSkipResizeBuffers.set_from_config(readBool("FrameGen", "SkipResizeBuffers"));
            FGModifyBufferState.set_from_config(readBool("FrameGen", "ModifyBufferState"));
            FGModifySCIndex.set_from_config(readBool("FrameGen", "ModifySCIndex"));
            FGHudCutoff.set_from_config(readFloat("FrameGen", "HudCutoff"));
        }

        // FSR FG
        {
            FGDebugTearLines.set_from_config(readBool("FSRFG", "DebugTearLines"));
            FGDebugResetLines.set_from_config(readBool("FSRFG", "DebugResetLines"));
            FGDebugPacingLines.set_from_config(readBool("FSRFG", "DebugPacingLines"));
            FGAsync.set_from_config(readBool("FSRFG", "AllowAsync"));
            FGUseMutexForSwapchain.set_from_config(readBool("FSRFG", "UseMutexForSwapchain"));
            FGFramePacingTuning.set_from_config(readBool("FSRFG", "FramePacingTuning"));
            FGFPTSafetyMarginInMs.set_from_config(readFloat("FSRFG", "FPTSafetyMarginInMs"));
            FGFPTVarianceFactor.set_from_config(readFloat("FSRFG", "FPTVarianceFactor"));
            FGFPTAllowHybridSpin.set_from_config(readBool("FSRFG", "FPTHybridSpin"));
            FGFPTHybridSpinTime.set_from_config(readInt("FSRFG", "FPTHybridSpinTime"));
            FGFPTAllowWaitForSingleObjectOnFence.set_from_config(readBool("FSRFG", "FPTWaitForSingleObjectOnFence"));
            FSRFGEnableWatermark.set_from_config(readBool("FSRFG", "EnableWatermark"));
            FSRFGDisableFlipMetering.set_from_config(readBool("FSRFG", "DisableFlipMetering"));
        }

        // OptiFG
        {
            FGDisableHUDFix.set_from_config(readBool("OptiFG", "DisableHUDFix"));
            FGHUDFix.set_from_config(readBool("OptiFG", "HUDFix"));
            FGHUDLimit.set_from_config(readInt("OptiFG", "HUDLimit"));
            FGHUDFixExtended.set_from_config(readBool("OptiFG", "HUDFixExtended"));
            FGHUDFixUITexture.set_from_config(readBool("OptiFG", "HUDFixUITexture"));
            FGHUDFixUIUnblend.set_from_config(readBool("OptiFG", "HUDFixUIUnblend"));
            FGImmediateCapture.set_from_config(readBool("OptiFG", "HUDFixImmediate"));
            FGHudfixPersistentBindings.set_from_config(readBool("OptiFG", "HUDFixPersistentBindings"));
            FGUseShards.set_from_config(readBool("OptiFG", "UseShards"));
            FGAlwaysTrackHeaps.set_from_config(readBool("OptiFG", "AlwaysTrackHeaps"));
            FGResourceBlocking.set_from_config(readBool("OptiFG", "ResourceBlocking"));
            FGMakeDepthCopy.set_from_config(readBool("OptiFG", "MakeDepthCopy"));
            FGMakeMVCopy.set_from_config(readBool("OptiFG", "MakeMVCopy"));
            FGHudfixDisableRTV.set_from_config(readBool("OptiFG", "HudfixDisableRTV"));
            FGHudfixDisableSRV.set_from_config(readBool("OptiFG", "HudfixDisableSRV"));
            FGHudfixDisableUAV.set_from_config(readBool("OptiFG", "HudfixDisableUAV"));
            FGHudfixDisableOM.set_from_config(readBool("OptiFG", "HudfixDisableOM"));
            FGHudfixDisableDispatch.set_from_config(readBool("OptiFG", "HudfixDisableDispatch"));
            FGHudfixDisableDI.set_from_config(readBool("OptiFG", "HudfixDisableDI"));
            FGHudfixDisableDII.set_from_config(readBool("OptiFG", "HudfixDisableDII"));
            FGHudfixDisableSCR.set_from_config(readBool("OptiFG", "HudfixDisableSCR"));
            FGHudfixDisableSGR.set_from_config(readBool("OptiFG", "HudfixDisableSGR"));

            FGEnableDepthScale.set_from_config(readBool("OptiFG", "EnableDepthScale"));
            FGDepthScaleMax.set_from_config(readFloat("OptiFG", "DepthScaleMax"));

            FGDontUseSwapchainBuffers.set_from_config(readBool("OptiFG", "HUDFixDontUseSwapchainBuffers"));
            FGRelaxedResolutionCheck.set_from_config(readBool("OptiFG", "HUDFixRelaxedResolutionCheck"));

            FGResourceFlip.set_from_config(readBool("OptiFG", "ResourceFlip"));
            FGResourceFlipOffset.set_from_config(readBool("OptiFG", "ResourceFlipOffset"));

            FGAlwaysCaptureFSRFGSwapchain.set_from_config(readBool("OptiFG", "AlwaysCaptureFSRFGSwapchain"));
        }

        {
            FGXeFGInterpolationCount.set_from_config(readInt("XeFG", "InterpolationCount"));
            // Anything in range here survives a reload; out of range is reset to
            // the default rather than clamped, so an over-large value would look
            // like the setting silently reverting to 2X.
            if (FGXeFGInterpolationCount.has_value() &&
                (FGXeFGInterpolationCount.value() < 1 || FGXeFGInterpolationCount.value() > XeFGMaxInterpolations))
                FGXeFGInterpolationCount.reset();

            FGXeFGUnlockEnabled.set_from_config(readBool("XeFG", "UnlockMFG"));
            FGXeFGMaxInterpolatedFrames.set_from_config(readInt("XeFG", "MaxInterpolatedFrames"));
            if (FGXeFGMaxInterpolatedFrames.has_value() &&
                (FGXeFGMaxInterpolatedFrames.value() < 1 ||
                 FGXeFGMaxInterpolatedFrames.value() > XeFGMaxInterpolations))
                FGXeFGMaxInterpolatedFrames.reset();

            FGXeFGExtraPacing.set_from_config(readBool("XeFG", "ExtraPacing"));
            FGXeFGDynamicMFG.set_from_config(readBool("XeFG", "DynamicMFG"));
            FGXeFGFramerateTargetDMFG.set_from_config(readFloat("XeFG", "FramerateTargetDMFG"));
            if (FGXeFGFramerateTargetDMFG.has_value() && FGXeFGFramerateTargetDMFG.value() < 10.0f)
                FGXeFGFramerateTargetDMFG.reset();

            FGXeFGRoundingDMFG.set_from_config(readInt("XeFG", "RoundingDMFG"));
            if (FGXeFGRoundingDMFG.has_value() && (FGXeFGRoundingDMFG.value() < 0 || FGXeFGRoundingDMFG.value() > 2))
                FGXeFGRoundingDMFG.reset();

            FGXeFGDynamicAllow1X.set_from_config(readBool("XeFG", "DynamicAllow1X"));
            FGXeFGTargetAsCapDMFG.set_from_config(readBool("XeFG", "TargetAsCapDMFG"));

            FGXeFGOverrideInterpolationCount.set_from_config(readInt("XeFG", "OverrideInterpolationCount"));
            if (FGXeFGOverrideInterpolationCount.has_value() &&
                (FGXeFGOverrideInterpolationCount.value() < 0 || FGXeFGOverrideInterpolationCount.value() > 5))
                FGXeFGOverrideInterpolationCount.reset();

            FGXeFGIgnoreInitChecks.set_from_config(readBool("XeFG", "IgnoreInitChecks"));
            FGXeFGUIComposition.set_from_config(readBool("XeFG", "UIComposition"));
            FGXeFGDepthInverted.set_from_config(readBool("XeFG", "DepthInverted"));
            FGXeFGJitteredMV.set_from_config(readBool("XeFG", "JitteredMV"));
            FGXeFGHighResMV.set_from_config(readBool("XeFG", "HighResMV"));
            FGXeFGDebugView.set_from_config(readBool("XeFG", "DebugView"));
            FGXeFGForceBorderless.set_from_config(readBool("XeFG", "ForceBorderless"));
        }

        {
            FGDLSSGInterpolationCount.set_from_config(readInt("DLSSG", "InterpolationCount"));
            if (FGDLSSGInterpolationCount.has_value() &&
                (FGDLSSGInterpolationCount.value() < 1 || FGDLSSGInterpolationCount.value() > 6))
                FGDLSSGInterpolationCount.reset();

            FGDLSSGUseGamesReflexMarkers.set_from_config(readBool("DLSSG", "UseGamesReflexMarkers"));

            FGDLSSGOverrideInterpolationCount.set_from_config(readInt("DLSSG", "OverrideInterpolationCount"));
            if (FGDLSSGOverrideInterpolationCount.has_value() &&
                (FGDLSSGOverrideInterpolationCount.value() < 0 || FGDLSSGOverrideInterpolationCount.value() > 6))
                FGDLSSGOverrideInterpolationCount.reset();

            FGDLSSGFramerateTargetDMFG.set_from_config(readFloat("DLSSG", "FramerateTargetDMFG"));
            FGDLSSGOverrideForceDMFG.set_from_config(readBool("DLSSG", "OverrideForceDMFG"));
            FGDLSSGForceDMFG.set_from_config(readBool("DLSSG", "ForceDMFG"));
            FGDLSSGAdaMfgUnlock.set_from_config(readBool("DLSSG", "AdaMfgUnlock"));
            FGDLSSGAdaBlackwellKernels.set_from_config(readBool("DLSSG", "AdaBlackwellKernels"));
        }

        {
            ReprojectionFillMode.set_from_config(
                readString("Reprojection", "FillMode", true).transform(CodeToEnum<ReprojectionFill>));

            ReprojectionDepthCutoff.set_from_config(readFloat("Reprojection", "DepthCutoff"));
            ReprojectionCutoffExpand.set_from_config(readUInt("Reprojection", "CutoffExpand"));
            ReprojectionLateLatch.set_from_config(readBool("Reprojection", "LateLatch"));
            ReprojectionDirectionCache.set_from_config(readBool("Reprojection", "DirectionCache"));
            ReprojectionParallaxAuto.set_from_config(readBool("Reprojection", "ParallaxAuto"));
            ReprojectionParallaxScale.set_from_config(readFloat("Reprojection", "ParallaxScale"));
            ReprojectionThirdPersonDetection.set_from_config(readBool("Reprojection", "ThirdPersonDetection"));
        }

        // FSR FG Inputs
        {
            FSRFGSkipConfigForHudless.set_from_config(readBool("FSRFGInputs", "SkipConfigForHudless"));
            FSRFGSkipDispatchForHudless.set_from_config(readBool("FSRFGInputs", "SkipDispatchForHudless"));
        }

        // Framerate
        {
            FramerateLimit.set_from_config(readFloat("Framerate", "FramerateLimit"));
        }

        // FSR Common
        {
            FsrVerticalFov.set_from_config(readFloat("FSR", "VerticalFov"));
            FsrHorizontalFov.set_from_config(readFloat("FSR", "HorizontalFov"));
            FsrCameraNear.set_from_config(readFloat("FSR", "CameraNear"));
            FsrCameraFar.set_from_config(readFloat("FSR", "CameraFar"));
            FsrUseFsrInputValues.set_from_config(readBool("FSR", "UseFsrInputValues"));
        }

        // FSR
        {
            FsrVelocity.set_from_config(readFloat("FSR", "VelocityFactor"));
            FsrReactiveScale.set_from_config(readFloat("FSR", "ReactiveScale"));
            FsrShadingScale.set_from_config(readFloat("FSR", "ShadingScale"));
            FsrAccAddPerFrame.set_from_config(readFloat("FSR", "AccAddPerFrame"));
            FsrMinDisOccAcc.set_from_config(readFloat("FSR", "MinDisOccAcc"));
            FsrDebugView.set_from_config(readBool("FSR", "DebugView"));
            FfxUpscalerIndex.set_from_config(readInt("FSR", "UpscalerIndex"));
            FfxFGIndex.set_from_config(readInt("FSR", "FGIndex"));
            FsrUseMaskForTransparency.set_from_config(readBool("FSR", "UseReactiveMaskForTransparency"));
            DlssReactiveMaskBias.set_from_config(readFloat("FSR", "DlssReactiveMaskBias"));

            if (auto v = readEnum<FSR4Support>("FSR", "Fsr4GpuSupport"))
                Fsr4GpuSupport.set_from_config(*v);
            else
                Fsr4GpuSupport.reset();

            Fsr4EnableWatermark.set_from_config(readBool("FSR", "Fsr4EnableWatermark"));
            Fsr4DoNotLoadAmdxc64.set_from_config(readBool("FSR", "Fsr4DoNotLoadAmdxc64"));

            if (auto setting = readInt("FSR", "Fsr4Preset"); setting.has_value() && setting >= 0 && setting <= 5)
                Fsr4Preset.set_from_config(setting);

            Fsr4Model.set_from_config(readEnum<FSR4Model>("FSR", "Fsr4Model"));

            FsrNonLinearColorSpace.set_from_config(readBool("FSR", "FsrNonLinearColorSpace"));
            FsrNonLinearPQ.set_from_config(readBool("FSR", "FsrNonLinearPQ"));
            FsrNonLinearSRGB.set_from_config(readBool("FSR", "FsrNonLinearSRGB"));
            FsrAgilitySDKUpgrade.set_from_config(readBool("FSR", "FsrAgilitySDKUpgrade"));

            // Only sRGB or PQ should be enabled
            if (FsrNonLinearPQ.has_value() && FsrNonLinearPQ.value())
                FsrNonLinearSRGB.reset();
            else if (FsrNonLinearSRGB.has_value() && FsrNonLinearSRGB.value())
                FsrNonLinearPQ.reset();

            if (FsrNonLinearPQ.has_value() || FsrNonLinearSRGB.has_value())
                FsrNonLinearColorSpace.set_volatile_value(true);
            // FSR-RR
            // Every key read here must also be listed in ResetFfxDenoiserSettings,
            // which is what the menu's FSR-RR Reset button applies.
            {
                FfxDenoiserUseAmdDefaults.set_from_config(readBool("FSR-RR", "UseAmdDefaults"));
                FfxDenoiserDisocThreshold.set_from_config(readFloat("FSR-RR", "DisocclusionThreshold"));
                FfxDenoiserCrossBlNormStr.set_from_config(readFloat("FSR-RR", "CrossBilateralNormalStrength"));
                FfxDenoiserStabilityBias.set_from_config(readFloat("FSR-RR", "TemporalStabilityBias"));
                FfxDenoiserMaxRadiance.set_from_config(readFloat("FSR-RR", "MaxRadiance"));
                FfxDenoiserRadianceClip.set_from_config(readFloat("FSR-RR", "RadianceClipDeviation"));
                FfxDenoiserGaussKernRelax.set_from_config(readFloat("FSR-RR", "GaussianKernelRelaxation"));
                FfxDenoiserHardwareDepth.set_from_config(readBool("FSR-RR", "HardwareDepth"));
                FfxDenoiserIndex.set_from_config(readInt("FSR-RR", "ProviderIndex"));
                FfxDenoiserDebugDepthMax.set_from_config(readFloat("FSR-RR", "DebugViewLinearDepthMax"));
                FfxDenoiserDebugViewport.set_from_config(readInt("FSR-RR", "DebugViewViewport"));
                FfxDenoiserInternalDebugViews.set_from_config(readBool("FSR-RR", "InternalDebugViews"));
                FfxDenoiserDiagnostics.set_from_config(readBool("FSR-RR", "Diagnostics"));
                FfxDenoiserCorrelationBias.set_from_config(readFloat("FSR-RR", "CorrelationBias"));
                FfxDenoiserDiffuseHitDistance.set_from_config(readBool("FSR-RR", "DiffuseHitDistance"));
                FfxDenoiserFloorIsolation.set_from_config(readFloat("FSR-RR", "FloorIsolation"));
                FfxDenoiserRoughnessFloor.set_from_config(readFloat("FSR-RR", "RoughnessFloor"));
                FfxDenoiserFloorHandover.set_from_config(readInt("FSR-RR", "FloorHandover"));
                if (!FfxDenoiserFloorHandover.has_value())
                {
                    // Migrate the boolean this setting replaced, which gated only the
                    // zero-roughness case and therefore maps to mode 1.
                    if (const auto legacy = readBool("FSR-RR", "ZeroRoughHandover"); legacy.has_value())
                        FfxDenoiserFloorHandover = legacy.value() ? 1 : 0;
                }
                FfxDenoiserFloorHandoverStrength.set_from_config(readFloat("FSR-RR", "FloorHandoverStrength"));
                FfxDenoiserFloorRawBlend.set_from_config(readFloat("FSR-RR", "FloorRawBlend"));
                FfxDenoiserFloorStructureGate.set_from_config(readFloat("FSR-RR", "FloorStructureGate"));
                FfxDenoiserDemodDivisorFloor.set_from_config(readFloat("FSR-RR", "DemodDivisorFloor"));
                FfxDenoiserFloorClampSmoothing.set_from_config(readFloat("FSR-RR", "FloorClampSmoothing"));
                FfxDenoiserFloorHandoverDetail.set_from_config(readFloat("FSR-RR", "FloorHandoverDetail"));
                FfxDenoiserFloorHandoverAnchorClamp.set_from_config(readFloat("FSR-RR", "FloorHandoverAnchorClamp"));
                FfxDenoiserFloorHandoverCorrelationMix.set_from_config(
                    readFloat("FSR-RR", "FloorHandoverCorrelationMix"));
                FfxDenoiserDiffuseSignalType.set_from_config(readInt("FSR-RR", "DiffuseSignalType"));
                FfxDenoiserSpecularSignalType.set_from_config(readInt("FSR-RR", "SpecularSignalType"));
                FfxDenoiserDenoiseDiffuse.set_from_config(readBool("FSR-RR", "DenoiseDiffuse"));
                FfxDenoiserDenoiseSpecular.set_from_config(readBool("FSR-RR", "DenoiseSpecular"));
                FfxDenoiserTaggedNormalRoughness.set_from_config(readBool("FSR-RR", "TaggedNormalRoughness"));
                FfxDenoiserTaggedAmbientOcclusion.set_from_config(readBool("FSR-RR", "TaggedAmbientOcclusion"));
                FfxDenoiserNormalsInViewSpace.set_from_config(readBool("FSR-RR", "NormalsInViewSpace"));
                FfxDenoiserUseTitleLinearDepth.set_from_config(readBool("FSR-RR", "UseTitleLinearDepth"));
                FfxDenoiserResponsivityThreshold.set_from_config(readFloat("FSR-RR", "ResponsivityTrustThreshold"));
                FfxDenoiserResponsivityInvert.set_from_config(readBool("FSR-RR", "ResponsivityInvert"));
                FfxDenoiserBiasMaskStrength.set_from_config(readFloat("FSR-RR", "BiasMaskStrength"));
                FfxDenoiserFloorDetailBoost.set_from_config(readFloat("FSR-RR", "FloorDetailBoost"));
                FfxDenoiserFloorNormalSharpness.set_from_config(readFloat("FSR-RR", "FloorNormalSharpness"));
                FfxDenoiserFloorAlbedoGuide.set_from_config(readFloat("FSR-RR", "FloorAlbedoGuide"));
                FfxDenoiserFloorLumSymmetry.set_from_config(readFloat("FSR-RR", "FloorLumSymmetry"));
                FfxDenoiserFloorGrazingSharpness.set_from_config(readFloat("FSR-RR", "FloorGrazingSharpness"));
                FfxDenoiserFloorEnvelopeBias.set_from_config(readFloat("FSR-RR", "FloorEnvelopeBias"));
                FfxDenoiserFloorSoftMin.set_from_config(readFloat("FSR-RR", "FloorSoftMin"));
            }

            OpenNrRrEnabled.set_from_config(readBool("OpenNR-RR", "Enabled"));
            // "loop" is gone; an ini that still has it gets display
            OpenNrRrNrOnDenoised.set_from_config(readString("OpenNR-RR", "NrOnDenoised", true).transform(
                [](std::string v) { return _stricmp(v.c_str(), "loop") == 0 ? std::string("display") : v; }));
            OpenNrRrNrImage.set_from_config(readString("OpenNR-RR", "NrImage", true));
            OpenNrRrNrDeltaRate.set_from_config(readFloat("OpenNR-RR", "NrDeltaRate"));
            OpenNrRrMode.set_from_config(readString("OpenNR-RR", "Mode", true));
            OpenNrRrFp8.set_from_config(readBool("OpenNR-RR", "Fp8"));
            OpenNrRrBackend.set_from_config(readString("OpenNR-RR", "Backend", true));
            OpenNrRrVkInputLayout.set_from_config(readString("OpenNR-RR", "VkInputLayout", true));
            OpenNrRrUseDisocclusionMask.set_from_config(readBool("OpenNR-RR", "UseDisocclusionMask"));
            OpenNrRrUseSssGuide.set_from_config(readBool("OpenNR-RR", "UseSssGuide"));
            OpenNrRrInvertDisocclusionMask.set_from_config(readBool("OpenNR-RR", "InvertDisocclusionMask"));
            OpenNrRrClampDisocclusionMask.set_from_config(readBool("OpenNR-RR", "ClampDisocclusionMask"));
            OpenNrRrDisocclusionMaskScale.set_from_config(readFloat("OpenNR-RR", "DisocclusionMaskScale"));
            OpenNrRrUseResponsivityMask.set_from_config(readBool("OpenNR-RR", "UseResponsivityMask"));
            OpenNrRrUseColorBeforeTransparency.set_from_config(readBool("OpenNR-RR", "UseColorBeforeTransparency"));
            OpenNrRrUseDepthOfFieldGuide.set_from_config(readBool("OpenNR-RR", "UseDepthOfFieldGuide"));
            OpenNrRrUseReflectionMotionVectors.set_from_config(readBool("OpenNR-RR", "UseReflectionMotionVectors"));
            OpenNrRrUseLayerOpacityAndMotion.set_from_config(readBool("OpenNR-RR", "UseLayerOpacityAndMotion"));
        }

        // XeSS
        {
            BuildPipelines.set_from_config(readBool("XeSS", "BuildPipelines"));
            NetworkModel.set_from_config(readInt("XeSS", "NetworkModel"));
            CreateHeaps.set_from_config(readBool("XeSS", "CreateHeaps"));
        }

        // DLSS
        {
            // Don't enable again if set false because of no nvngx found
            DLSSEnabled.set_from_config(readBool("DLSS", "Enabled"));

            // --- DLSS 5 Neural Rendering (OptiScaler/dlssnr) ---
            DlssNrEnabled.set_from_config(readBool("DlssNr", "Enabled"));
            DlssNrPresent.set_from_config(readBool("DlssNr", "PresentWithoutUpscaler"));
            DlssNrRunBeforeSr.set_from_config(readBool("DlssNr", "RunBeforeSR"));
            DlssNrApplyAfterRR.set_from_config(readBool("DlssNr", "ApplyAfterRR"));
            DlssNrRRPasses.set_from_config(readUInt("DlssNr", "RRPasses"));
            DlssNrRRWorkingScale.set_from_config(readFloat("DlssNr", "RRWorkingScale"));
            DlssNrToggleKey.set_from_config(readShortcut("DlssNr", "ToggleKey"));
            DlssNrTransferStrength.set_from_config(readFloat("DlssNr", "TransferStrength"));
            DlssNrColourStrength.set_from_config(readFloat("DlssNr", "ColourStrength"));
            DlssNrMaxRatio.set_from_config(readFloat("DlssNr", "MaxRatio"));
            DlssNrTransfer.set_from_config(readUInt("DlssNr", "Transfer"));

            DlssNrWhitePointFromExposure.set_from_config(readBool("DlssNr", "WhitePointFromExposure"));
            DlssNrProbeD3D11.set_from_config(readBool("DlssNr", "ProbeD3D11"));
            DlssNrDebugView.set_from_config(readUInt("DlssNr", "DebugView"));
            DlssNrCompare.set_from_config(readUInt("DlssNr", "Compare"));
            DlssNrCompareSplit.set_from_config(readFloat("DlssNr", "CompareSplit"));
            DlssNrCompareZoom.set_from_config(readFloat("DlssNr", "CompareZoom"));
            DlssNrCompareSwap.set_from_config(readBool("DlssNr", "CompareSwap"));
            DlssNrCompareTags.set_from_config(readBool("DlssNr", "CompareTags"));
            DlssNrTagScale.set_from_config(readFloat("DlssNr", "TagScale"));
            DlssNrBadge.set_from_config(readBool("DlssNr", "Badge"));
            DlssNrBadgeCorner.set_from_config(readUInt("DlssNr", "BadgeCorner"));
            DlssNrBadgeScale.set_from_config(readFloat("DlssNr", "BadgeScale"));
            DlssNrWorkingScale.set_from_config(readFloat("DlssNr", "WorkingScale"));
            DlssNrPreUpscale.set_from_config(readBool("DlssNr", "PreUpscale"));
            DlssNrDualFeature.set_from_config(readBool("DlssNr", "DualFeature"));
            DlssNrDualEnlarger.set_from_config(readString("DlssNr", "DualEnlarger", true).transform(CodeToUpscalerFfx));

            if (auto v = readEnum<Scaler>("DlssNr", "ScalingDownscaler"))
                DlssNrScalingDownscaler.set_from_config(*v);
            else
                DlssNrScalingDownscaler.reset();
            DlssNrProxyProbe.set_from_config(readBool("DlssNr", "ProxyProbe"));
            DlssNrUseProxy.set_from_config(readBool("DlssNr", "UseProxy"));
            DlssNrScanExposure.set_from_config(readBool("DlssNr", "ScanExposure"));
            DlssNrWhitePointSource.set_from_config(readUInt("DlssNr", "WhitePointSource"));

            // Migrate the retired flag only when the new key was genuinely absent, which is why this
            // has to run AFTER the read above -- set_from_config assigns only into an empty optional,
            // so an ini that carries both keys keeps its explicit WhitePointSource.
            if (!DlssNrWhitePointSource.has_value() && DlssNrWhitePointFromExposure.has_value())
                DlssNrWhitePointSource = DlssNrWhitePointFromExposure.value() ? 1u : 0u;
            DlssNrScanMeter.set_from_config(readBool("DlssNr", "ScanMeter"));
            DlssNrScanTrim.set_from_config(readFloat("DlssNr", "ScanTrim"));
            DlssNrPasses.set_from_config(readUInt("DlssNr", "Passes"));
            DlssNrScanAnchorValue.set_from_config(readFloat("DlssNr", "ScanAnchorValue"));
            DlssNrScanAnchorWhitePoint.set_from_config(readFloat("DlssNr", "ScanAnchorWhitePoint"));
            DlssNrScanAnchors.set_from_config(readString("DlssNr", "ScanAnchors"));
            DlssNrScanInverted.set_from_config(readBool("DlssNr", "ScanInverted"));
            DlssNrWhitePointTrim.set_from_config(readFloat("DlssNr", "WhitePointTrim"));
            DlssNrAutoCapture.set_from_config(readBool("DlssNr", "AutoCapture"));
            DlssNrPassOverrides.set_from_config(readString("DlssNr", "PassOverrides"));
            DlssNrUnlockPasses.set_from_config(readBool("DlssNr", "UnlockPasses"));
            DlssNrWhitePointScale.set_from_config(readFloat("DlssNr", "WhitePointScale"));
            DlssNrPreset.set_from_config(readUInt("DlssNr", "Preset"));
            DlssNrIntensity.set_from_config(readFloat("DlssNr", "Intensity"));
            DlssNrStyle.set_from_config(readUInt("DlssNr", "Style"));
            DlssNrPass2Preset.set_from_config(readUInt("DlssNr", "Pass2Preset"));
            DlssNrPass2Style.set_from_config(readUInt("DlssNr", "Pass2Style"));
            DlssNrPass3Preset.set_from_config(readUInt("DlssNr", "Pass3Preset"));
            DlssNrPass3Style.set_from_config(readUInt("DlssNr", "Pass3Style"));
            DlssNrLocalStructure.set_from_config(readFloat("DlssNr", "LocalStructure"));
            DlssNrLocalTone.set_from_config(readFloat("DlssNr", "LocalTone"));
            AmdNeuralLighting.set_from_config(readBool("DlssNr", "AmdNeuralLighting"));
            AmdNeuralLightingStrength.set_from_config(readFloat("DlssNr", "AmdNeuralLightingStrength"));
            // An INI from before 0.4.8 has AmdColourGrade and no AmdStyle. Its AmdEncoding=2 is the sRGB its package
            // wrote as the default, read as 0.4.8's Linear, and its colour grade became the network's Style. Saving
            // writes AmdStyle, so this happens until the settings are first saved.
            const auto oldGrade = readInt("DlssNr", "AmdColourGrade");
            const bool before048 = oldGrade.has_value() && !readInt("DlssNr", "AmdStyle").has_value();
            const auto encoding = readInt("DlssNr", "AmdEncoding");
            AmdEncoding.set_from_config(before048 && encoding == 2 ? std::optional<int> {} : encoding);
            AmdEffectStrength.set_from_config(readFloat("DlssNr", "AmdEffectStrength"));
            AmdStabilizerStrength.set_from_config(readFloat("DlssNr", "AmdStabilizerStrength"));
            AmdStabilizerThreshold.set_from_config(readFloat("DlssNr", "AmdStabilizerThreshold"));
            DlssNrRenoComposition.set_from_config(readBool("DlssNr", "RenoComposition"));
            DlssNrRenoIntensity.set_from_config(readFloat("DlssNr", "RenoIntensity"));
            DlssNrRenoGuard.set_from_config(readFloat("DlssNr", "RenoGuard"));
            DlssNrRenoColour.set_from_config(readFloat("DlssNr", "RenoColour"));
            DlssNrRenoChromaClamp.set_from_config(readFloat("DlssNr", "RenoChromaClamp"));
            DlssNrRenoPedestal.set_from_config(readBool("DlssNr", "RenoPedestal"));
            NrPersonPartition.set_from_config(readBool("DlssNr", "NrPersonPartition"));
            NrPersonModel.set_from_config(readInt("DlssNr", "NrPersonModel"));
            NrFaceInputSize.set_from_config(readInt("DlssNr", "NrFaceInputSize"));
            NrPersonStrength.set_from_config(readFloat("DlssNr", "NrPersonStrength"));
            NrPersonDetailGain.set_from_config(readFloat("DlssNr", "NrPersonDetailGain"));
            NrPersonDebugMask.set_from_config(readBool("DlssNr", "NrPersonDebugMask"));
            AmdQuality.set_from_config(readInt("DlssNr", "AmdQuality"));
            AmdToneCurve.set_from_config(readInt("DlssNr", "AmdToneCurve"));
            AmdToneLift.set_from_config(readFloat("DlssNr", "AmdToneLift"));
            AmdUseGameExposure.set_from_config(readBool("DlssNr", "AmdUseGameExposure"));
            AmdStyle.set_from_config(before048 ? oldGrade : readInt("DlssNr", "AmdStyle"));
            AmdSlots.set_from_config(readInt("DlssNr", "AmdSlots"));
            AmdNrScale.set_from_config(readFloat("DlssNr", "AmdModelScale"));
            AmdDynamicScale.set_from_config(readBool("DlssNr", "AmdDynamicScale"));
            AmdDynamicTargetFps.set_from_config(readInt("DlssNr", "AmdDynamicTargetFps"));
            AmdEveryFrame.set_from_config(readBool("DlssNr", "AmdEveryFrame"));
            AmdFixedSeed.set_from_config(readBool("DlssNr", "AmdFixedSeed"));
            AmdSpinDraw.set_from_config(readInt("DlssNr", "AmdSpinDraw"));
            AmdGraphicsWait.set_from_config(readInt("DlssNr", "AmdGraphicsWait"));
            AmdStallStandDownMs.set_from_config(readInt("DlssNr", "AmdStallStandDownMs"));
            NrBackend.set_from_config(readString("DlssNr", "NrBackend", true));
            HostComputeBackend.set_from_config(readString("DlssNr", "HostComputeBackend"));
            HostZeroCopy.set_from_config(readBool("DlssNr", "HostZeroCopy"));
            LmxxfDiagnostic.set_from_config(readString("DlssNr", "LmxxfDiagnostic", true));
            // true/false only; missing or "auto" => false (do not enable FitLarge by accident).
            {
                const auto fitRaw = readString("DlssNr", "LmxxfFitLarge", true);
                if (!fitRaw.has_value() || fitRaw->empty() || _stricmp(fitRaw->c_str(), "auto") == 0)
                    LmxxfFitLarge.set_from_config(false);
                else
                    LmxxfFitLarge.set_from_config(readBool("DlssNr", "LmxxfFitLarge"));
            }
            // Runtime reads DLSS5_FIT_LARGE / flags; keep env aligned with ini so QueryCapabilities matches.
            {
                const bool fit = LmxxfFitLarge.value_or_default();
                _putenv(fit ? "DLSS5_FIT_LARGE=1" : "DLSS5_FIT_LARGE=0");
            }
            LmxxfTemporal.set_from_config(readBool("DlssNr", "LmxxfTemporal"));
            LmxxfSmoothStrength.set_from_config(readFloat("DlssNr", "LmxxfSmoothStrength"));
            LmxxfSmoothThreshold.set_from_config(readFloat("DlssNr", "LmxxfSmoothThreshold"));
            LmxxfSmoothResidual.set_from_config(readBool("DlssNr", "LmxxfSmoothResidual"));
            LmxxfStyle.set_from_config(readUInt("DlssNr", "LmxxfStyle"));
            LmxxfVitReuse.set_from_config(readBool("DlssNr", "LmxxfVitReuse"));
            LmxxfCompact1080.set_from_config(readBool("DlssNr", "LmxxfCompact1080"));
            LmxxfOriginalModel.set_from_config(readBool("DlssNr", "LmxxfOriginalModel"));
            LmxxfPassEstimate.set_from_config(readBool("DlssNr", "LmxxfPassEstimate"));
            LmxxfSkinProtect.set_from_config(readBool("DlssNr", "LmxxfSkinProtect"));
            LmxxfAutoExposure.set_from_config(readBool("DlssNr", "LmxxfAutoExposure"));
            MochizukiTemporal.set_from_config(readBool("DlssNr", "MochizukiTemporal"));
            MochizukiHistoryStrength.set_from_config(readFloat("DlssNr", "MochizukiHistoryStrength"));
            MochizukiStabilizerStrength.set_from_config(readFloat("DlssNr", "MochizukiStabilizerStrength"));
            MochizukiStabilizerThreshold.set_from_config(readFloat("DlssNr", "MochizukiStabilizerThreshold"));
            NrFoveation.set_from_config(readInt("DlssNr", "Foveation"));
            NrFoveationCentreWidth.set_from_config(readFloat("DlssNr", "FoveationCentreWidth"));
            NrFoveationCentreHeight.set_from_config(readFloat("DlssNr", "FoveationCentreHeight"));
            NrFoveationFalloff.set_from_config(readFloat("DlssNr", "FoveationFalloff"));
            NrFoveationEdge.set_from_config(readFloat("DlssNr", "FoveationEdge"));
            MochizukiAco.set_from_config(readBool("DlssNr", "MochizukiAco"));
            MochizukiRdna2Int8.set_from_config(readBool("DlssNr", "MochizukiRdna2Int8"));
            MochizukiInt4.set_from_config(readBool("DlssNr", "MochizukiInt4"));
            MochizukiOptimizedLayers.set_from_config(readString("DlssNr", "MochizukiOptimizedLayers", true)
                                                         .and_then(
                                                             [](const std::string& v) -> std::optional<uint32_t>
                                                             {
                                                                 if (v == "0" || v == "quality")
                                                                     return 0u;
                                                                 if (v == "1" || v == "balanced")
                                                                     return 1u;
                                                                 if (v == "2" || v == "performance")
                                                                     return 2u;
                                                                 if (v == "3" || v == "ultra" ||
                                                                     v == "ultra performance" ||
                                                                     v == "ultraperformance")
                                                                     return 3u;
                                                                 return std::nullopt;
                                                             }));
            MochizukiOriginalModel.set_from_config(readBool("DlssNr", "MochizukiOriginalModel"));
            MochizukiExperimental.set_from_config(readBool("DlssNr", "MochizukiExperimental"));
            MochizukiPlacement.set_from_config(readString("DlssNr", "MochizukiPlacement", true));
            MochizukiCascadeDetail.set_from_config(readBool("DlssNr", "MochizukiCascadeDetail"));
            MochizukiAntiHalo.set_from_config(readBool("DlssNr", "MochizukiAntiHalo"));
            MochizukiJitterHistory.set_from_config(readBool("DlssNr", "MochizukiJitterHistory"));
            MochizukiAntiHaloStrength.set_from_config(readFloat("DlssNr", "MochizukiAntiHaloStrength"));
            MochizukiPassEstimate.set_from_config(readBool("DlssNr", "MochizukiPassEstimate"));
            MochizukiApplyModel.set_from_config(readBool("DlssNr", "MochizukiApplyModel"));
            MochizukiUseDepth.set_from_config(readBool("DlssNr", "MochizukiUseDepth"));
            MochizukiDynamicResolution.set_from_config(readString("DlssNr", "MochizukiDynamicResolution", true));
            MochizukiPreprocess.set_from_config(readBool("DlssNr", "MochizukiPreprocess"));
            MochizukiPreprocessExposure.set_from_config(readUInt("DlssNr", "MochizukiPreprocessExposure"));
            MochizukiPreprocessBias.set_from_config(readFloat("DlssNr", "MochizukiPreprocessBias"));
            MochizukiPreprocessCurve.set_from_config(readUInt("DlssNr", "MochizukiPreprocessCurve"));
            MochizukiPreprocessContrast.set_from_config(readFloat("DlssNr", "MochizukiPreprocessContrast"));
            MochizukiPreprocessSaturation.set_from_config(readFloat("DlssNr", "MochizukiPreprocessSaturation"));
            MochizukiLayerMode.set_from_config(readUInt("DlssNr", "MochizukiLayerMode"));
            MochizukiNoiseEveryFrame.set_from_config(readBool("DlssNr", "MochizukiNoiseEveryFrame"));
            MochizukiStyleGrade.set_from_config(readBool("DlssNr", "MochizukiStyleGrade"));
            MochizukiHistoryPrecision.set_from_config(readUInt("DlssNr", "MochizukiHistoryPrecision"));
            MochizukiPreprocessInverse.set_from_config(readBool("DlssNr", "MochizukiPreprocessInverse"));
#define MZR_F(name, def, lo, hi, group, label, member, help) name.set_from_config(readFloat("DlssNr", #name));
#define MZR_U(name, def, lo, hi, group, label, member, help) name.set_from_config(readUInt("DlssNr", #name));
#define MZR_B(name, def, lo, hi, group, label, member, help) name.set_from_config(readBool("DlssNr", #name));
#include "dlssnr/backend/MochizukiRenoKeys.inc"
#undef MZR_F
#undef MZR_U
#undef MZR_B
            AmdGraphicsUnsafe.set_from_config(readInt("DlssNr", "AmdGraphicsUnsafe"));
            AmdRtgiEnabled.set_from_config(readBool("AmdRtgi", "Enabled"));
            AmdRtgiQuality.set_from_config(readUInt("AmdRtgi", "Quality"));
            AmdRtgiDenoiser.set_from_config(readUInt("AmdRtgi", "Denoiser"));
            AmdRtgiInspect.set_from_config(readUInt("AmdRtgi", "Inspect"));
            AmdRtgiContact.set_from_config(readFloat("AmdRtgi", "Contact"));
            AmdRtgiSaturation.set_from_config(readFloat("AmdRtgi", "Saturation"));
            AmdRtgiRadius.set_from_config(readFloat("AmdRtgi", "Radius"));
            AmdRtgiMix.set_from_config(readFloat("AmdRtgi", "Mix"));
            AmdRtgiLighting.set_from_config(readFloat("AmdRtgi", "Lighting"));
            AmdRtgiOcclusion.set_from_config(readFloat("AmdRtgi", "Occlusion"));
            AmdRtgiAmbient.set_from_config(readFloat("AmdRtgi", "Ambient"));
            AmdRtgiThickness.set_from_config(readFloat("AmdRtgi", "Thickness"));
            AmdRtgiSmoothness.set_from_config(readFloat("AmdRtgi", "Smoothness"));
            AmdRtgiFade.set_from_config(readFloat("AmdRtgi", "Fade"));
            AmdRtgiFov.set_from_config(readFloat("AmdRtgi", "Fov"));
            AmdRtgiFarPlane.set_from_config(readFloat("AmdRtgi", "FarPlane"));
            AmdLookEnabled.set_from_config(readBool("AmdLook", "Enabled"));
            AmdLookAppearance.set_from_config(readUInt("AmdLook", "Appearance"));
            AmdLookMix.set_from_config(readFloat("AmdLook", "Mix"));
            AmdLookMaterialDetail.set_from_config(readFloat("AmdLook", "MaterialDetail"));
            AmdLookShapeDefinition.set_from_config(readFloat("AmdLook", "ShapeDefinition"));
            AmdLookLocalLighting.set_from_config(readFloat("AmdLook", "LocalLighting"));
            AmdLookSkinDetail.set_from_config(readFloat("AmdLook", "SkinDetail"));
            AmdLookSkinSoftness.set_from_config(readFloat("AmdLook", "SkinSoftness"));
            AmdLookDetectSkin.set_from_config(readBool("AmdLook", "DetectSkin"));
            AmdLookSpecularControl.set_from_config(readFloat("AmdLook", "SpecularControl"));
            AmdLookHighlightRollOff.set_from_config(readFloat("AmdLook", "HighlightRollOff"));
            AmdLookColourSeparation.set_from_config(readFloat("AmdLook", "ColourSeparation"));
            AmdLookShadowDepth.set_from_config(readFloat("AmdLook", "ShadowDepth"));
            AmdLookAntiHalo.set_from_config(readFloat("AmdLook", "AntiHalo"));
            AmdLookFlatAreaProtection.set_from_config(readFloat("AmdLook", "FlatAreaProtection"));
            AmdLookInspect.set_from_config(readUInt("AmdLook", "Inspect"));
            AmdLookTone.set_from_config(readFloat("AmdLook", "Tone"));
            AmdLookExposureEV.set_from_config(readFloat("AmdLook", "ExposureEV"));
            AmdLookContrast.set_from_config(readFloat("AmdLook", "Contrast"));
            AmdLookSaturation.set_from_config(readFloat("AmdLook", "Saturation"));
            AmdLookHighlightCompression.set_from_config(readFloat("AmdLook", "HighlightCompression"));
            DlssNrSkinStructure.set_from_config(readFloat("DlssNr", "SkinStructure"));
            DlssNrAutoMask.set_from_config(readBool("DlssNr", "AutoMask"));
            DlssNrSkinProtection.set_from_config(readBool("DlssNr", "SkinProtection"));
            DlssNrSkinToneEnabled.set_from_config(readBool("DlssNr", "SkinToneEnabled"));
            DlssNrSkinDetail.set_from_config(readFloat("DlssNr", "SkinDetail"));
            DlssNrSkinColour.set_from_config(readFloat("DlssNr", "SkinColour"));
            DlssNrEnvironmentDetail.set_from_config(readFloat("DlssNr", "EnvironmentDetail"));
            DlssNrEnvironmentColour.set_from_config(readFloat("DlssNr", "EnvironmentColour"));
            DlssNrShowSkinMask.set_from_config(readBool("DlssNr", "ShowSkinMask"));
            DlssNrPass2Intensity.set_from_config(readFloat("DlssNr", "Pass2Intensity"));
            DlssNrPass2LocalStructure.set_from_config(readFloat("DlssNr", "Pass2LocalStructure"));
            DlssNrPass2LocalTone.set_from_config(readFloat("DlssNr", "Pass2LocalTone"));
            DlssNrPass2SkinStructure.set_from_config(readFloat("DlssNr", "Pass2SkinStructure"));
            DlssNrPass2AutoMask.set_from_config(readBool("DlssNr", "Pass2AutoMask"));
            DlssNrPass3Intensity.set_from_config(readFloat("DlssNr", "Pass3Intensity"));
            DlssNrPass3LocalStructure.set_from_config(readFloat("DlssNr", "Pass3LocalStructure"));
            DlssNrPass3LocalTone.set_from_config(readFloat("DlssNr", "Pass3LocalTone"));
            DlssNrPass3SkinStructure.set_from_config(readFloat("DlssNr", "Pass3SkinStructure"));
            DlssNrPass3AutoMask.set_from_config(readBool("DlssNr", "Pass3AutoMask"));
            DlssNrReversibleMode.set_from_config(readUInt("DlssNr", "ReversibleMode"));
            DlssNrApplyModel.set_from_config(readBool("DlssNr", "ApplyModel"));
            DlssNrHoldFrame.set_from_config(readBool("DlssNr", "HoldFrame"));
            UseGenericAppIdWithDlss.set_from_config(readBool("DLSS", "UseGenericAppIdWithDlss"));

            RenderPresetOverride.set_from_config(readBool("DLSS", "RenderPresetOverride"));

            constexpr size_t presetCount = 17;

            if (auto setting = readInt("DLSS", "RenderPresetForAll");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetForAll.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetDLAA");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetDLAA.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetUltraQuality");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetUltraQuality.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetQuality");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetQuality.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetBalanced");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetBalanced.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetPerformance");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetPerformance.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetUltraPerformance");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetUltraPerformance.set_from_config(setting);
        }
        // DLSSD
        {
            // Don't enable again if set false because of no nvngx found
            DLSSDRenderPresetOverride.set_from_config(readBool("DLSSD", "RenderPresetOverride"));

            constexpr size_t presetCount = 7;

            if (auto setting = readInt("DLSSD", "RenderPresetForAll");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetForAll.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetDLAA");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetDLAA.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetUltraQuality");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetUltraQuality.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetQuality");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetQuality.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetBalanced");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetBalanced.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetPerformance");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetPerformance.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetUltraPerformance");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetUltraPerformance.set_from_config(setting);
        }

        // NvngxFG
        {
            if (auto setting = readBool("Nukems", "MakeDepthCopy"); setting.has_value() && setting.value())
                NvngxFGMakeDepthCopy.set_from_config(setting); // For compat with older config
            else
                NvngxFGMakeDepthCopy.set_from_config(readBool("NvngxFG", "MakeDepthCopy"));

            NvngxFGDispatchFlags.set_from_config(readUInt("NvngxFG", "DispatchFlags"));
            NvngxFGShowDebug.set_from_config(readBool("NvngxFG", "ShowDebug"));
            NvngxFGDisableHudless.set_from_config(readBool("NvngxFG", "DisableHudless"));
        }

        // Logging
        {
            LogToFile.set_from_config(readBool("Log", "LogToFile"));
            LogLevel.set_from_config(readInt("Log", "LogLevel"));
            LogToConsole.set_from_config(readBool("Log", "LogToConsole"));
            LogToDebug.set_from_config(readBool("Log", "LogToDebug"));
            LogToNGX.set_from_config(readBool("Log", "LogToNGX"));
            OpenConsole.set_from_config(readBool("Log", "OpenConsole"));
            DebugWait.set_from_config(readBool("Log", "DebugWait"));
            LogSingleFile.set_from_config(readBool("Log", "SingleFile"));
            LogAsync.set_from_config(readBool("Log", "LogAsync"));
            LogAsyncThreads.set_from_config(readInt("Log", "LogAsyncThreads"));

            {
                auto setting = readString("Log", "LogFileName", false);

                if (setting.has_value() && setting.value().empty())
                    setting = std::nullopt;

                auto path = std::filesystem::path(setting.value_or(wstring_to_string(LogFileName.value_or_default())));
                auto filenameStem = path.stem();

                auto filename =
                    std::filesystem::path(LogSingleFile.value_or_default()
                                              ? filenameStem.wstring() + L".log"
                                              : filenameStem.wstring() + L"_" + std::to_wstring(GetTicks()) + L".log");

                if (setting.has_value())
                {
                    if (path.has_root_path())
                        LogFileName.set_from_config((path.parent_path() / filename).wstring());
                    else
                        LogFileName.set_from_config((Util::DllPath().parent_path() / filename).wstring());
                }
                else
                {
                    if (path.has_root_path())
                        LogFileName.set_volatile_value((path.parent_path() / filename).wstring());
                    else
                        LogFileName.set_volatile_value((Util::DllPath().parent_path() / filename).wstring());
                }
            }
        }

        // Sharpness
        {
            SharpnessShader.set_from_config(
                readString("Sharpness", "Shader", true).transform(CodeToEnum<SharpenShader>));
            OverrideSharpness.set_from_config(readBool("Sharpness", "OverrideSharpness"));

            if (auto setting = readFloat("Sharpness", "Sharpness"); setting.has_value())
                Sharpness.set_from_config(std::clamp(setting.value(), 0.0f, 1.3f));
        }

        // Menu
        {
            if (auto setting = readFloat("Menu", "Scale"); setting.has_value())
                MenuScale.set_from_config(std::clamp(setting.value(), 0.5f, 2.0f));

            if (auto setting = readFloat("Menu", "Width"); setting.has_value())
                MenuWidth.set_from_config(std::clamp(setting.value(), 200.0f, 8192.0f));

            if (auto setting = readFloat("Menu", "Height"); setting.has_value())
                MenuHeight.set_from_config(std::clamp(setting.value(), 150.0f, 8192.0f));

            if (auto setting = readInt("Menu", "Anchor"); setting.has_value())
                MenuAnchor.set_from_config(std::clamp(setting.value(), 0, 6));

            MenuSectionOrder.set_from_config(readString("Menu", "SectionOrder", true));
            MenuBasic.set_from_config(readBool("Menu", "Basic"));

            // Don't enable again if set false because of Linux issue
            OverlayMenu.set_from_config(readBool("Menu", "OverlayMenu"));
            ShortcutKey.set_from_config(readShortcut("Menu", "ShortcutKey"));
            ExtendedLimits.set_from_config(readBool("Menu", "ExtendedLimits"));
            ShowFps.set_from_config(readBool("Menu", "ShowFps"));
            UseHQFont.set_from_config(readBool("Menu", "UseHQFont"));
            DisableSplash.set_from_config(readBool("Menu", "DisableSplash"));

            if (auto setting = readUInt("Menu", "FpsOverlayPos"); setting.has_value())
            {
                FpsOverlayPosition.set_from_config(
                    (FpsOverlayPos) std::clamp(setting.value(), 0U, FpsOverlayPos_COUNT - 1));
            }

            if (auto setting = readUInt("Menu", "FpsOverlayType"); setting.has_value())
            {
                FpsOverlayType.set_from_config((FpsOverlay) std::clamp(setting.value(), 0U, FpsOverlay_COUNT - 1));
            }

            FpsShortcutKey.set_from_config(readShortcut("Menu", "FpsShortcutKey"));
            FpsCycleShortcutKey.set_from_config(readShortcut("Menu", "FpsCycleShortcutKey"));
            FpsOverlayHorizontal.set_from_config(readBool("Menu", "FpsOverlayHorizontal"));
            FpsOverlayShowRatio.set_from_config(readBool("Menu", "FpsOverlayShowRatio"));

            if (auto setting = readFloat("Menu", "FpsOverlayAlpha"); setting.has_value())
                FpsOverlayAlpha.set_from_config(std::clamp(setting.value(), 0.0f, 1.0f));

            if (auto setting = readFloat("Menu", "FpsScale"); setting.has_value())
                FpsScale.set_from_config(std::clamp(setting.value(), 0.5f, 2.0f));

            FontSize.set_from_config(readFloat("Menu", "FontSize"));
            TTFFontPath.set_from_config(readWString("Menu", "TTFFontPath"));
            MenuLanguage.set_from_config(readString("Menu", "Language"));

            FGShortcutKey.set_from_config(readShortcut("Menu", "FGShortcutKey"));
            ScreenshotKey.set_from_config(readShortcut("Menu", "ScreenshotKey"));
            ScreenshotPath.set_from_config(readWString("Menu", "ScreenshotPath"));
            ScreenshotWithMenu.set_from_config(readBool("Menu", "ScreenshotWithMenu"));

            LightTheme.set_from_config(readBool("Menu", "LightTheme"));
            OverlaysUseTheme.set_from_config(readBool("Menu", "OverlaysUseTheme"));
            MenuAccentColorR.set_from_config(readFloat("Menu", "AccentColorR"));
            MenuAccentColorG.set_from_config(readFloat("Menu", "AccentColorG"));
            MenuAccentColorB.set_from_config(readFloat("Menu", "AccentColorB"));
            MenuBGColorR.set_from_config(readFloat("Menu", "BGColorR"));
            MenuBGColorG.set_from_config(readFloat("Menu", "BGColorG"));
            MenuBGColorB.set_from_config(readFloat("Menu", "BGColorB"));
            MenuBGColorA.set_from_config(readFloat("Menu", "BGColorA"));
            MenuBlur.set_from_config(readBool("Menu", "Blur"));

            if (auto setting = readFloat("Menu", "BlurStrength"); setting.has_value())
                MenuBlurStrength.set_from_config(std::clamp(setting.value(), 0.0f, 4.0f));
            CustomTabEnabled.set_from_config(readBool("Menu", "CustomTabEnabled"));
            CustomTabCards.set_from_config(readString("Menu", "CustomTabCards"));
        }

        // Hooks
        {
            HookOriginalNvngxOnly.set_from_config(readBool("Hooks", "HookOriginalNvngxOnly"));
            EarlyHooking.set_from_config(readBool("Hooks", "EarlyHooking"));
            UseNtdllHooks.set_from_config(readBool("Hooks", "UseNtdllHooks"));
        }

        // RCAS
        {
            RcasEnabled.set_from_config(readBool("CAS", "Enabled"));

            MotionSharpnessEnabled.set_from_config(readBool("CAS", "MotionSharpnessEnabled"));

            if (auto setting = readFloat("CAS", "MotionSharpness"); setting.has_value())
                MotionSharpness.set_from_config(std::clamp(setting.value(), -1.3f, 1.3f));

            if (auto setting = readFloat("CAS", "MotionThreshold"); setting.has_value())
                MotionThreshold.set_from_config(std::clamp(setting.value(), 0.0f, 100.0f));

            if (auto setting = readFloat("CAS", "MotionScaleLimit"); setting.has_value())
                MotionScaleLimit.set_from_config(std::clamp(setting.value(), 0.01f, 100.0f));

            ContrastEnabled.set_from_config(readBool("CAS", "ContrastEnabled"));
            if (auto setting = readFloat("CAS", "Contrast"); setting.has_value())
                Contrast.set_from_config(std::clamp(setting.value(), -2.0f, 2.0f));

            DADepthScale.set_from_config(readFloat("CAS", "DADepthScale"));
            DADepthBias.set_from_config(readFloat("CAS", "DADepthBias"));
            DAClampOutput.set_from_config(readBool("CAS", "DAClampOutput"));
            UseDepthAwareSharpen.set_from_config(readBool("CAS", "UseDepthAwareSharpen"));
            UseDASDepthAwareSharpen.set_from_config(readBool("CAS", "UseDASDepthAwareSharpen"));
            DADepthIsLinear.set_from_config(readBool("CAS", "DADepthIsLinear"));

            MotionSharpnessDebug.set_from_config(readBool("CAS", "SharpenerDebug"));
        }

        // Magnifier
        {
            MagnifierEnabled.set_from_config(readBool("Magnifier", "Enabled"));

            if (auto setting = readFloat("Magnifier", "Size"); setting.has_value())
                MagnifierSize.set_from_config(std::clamp(setting.value(), 0.0f, 100.0f));

            if (auto setting = readInt("Magnifier", "ZoomFactor"); setting.has_value())
                MagnifierZoomFactor.set_from_config(std::clamp(setting.value(), 2, 20));

            if (auto setting = readFloat("Magnifier", "BorderSize"); setting.has_value())
                MagnifierBorderSize.set_from_config(std::clamp(setting.value(), 0.0f, 2.0f));

            if (auto setting = readFloat("Magnifier", "CursorOffsetX"); setting.has_value())
                MagnifierCursorOffsetX.set_from_config(std::clamp(setting.value(), -1000.f, 1000.f));
            if (auto setting = readFloat("Magnifier", "CursorOffsetY"); setting.has_value())
                MagnifierCursorOffsetY.set_from_config(std::clamp(setting.value(), -1000.f, 1000.f));

            if (auto setting = readFloat("Magnifier", "StaticPosX"); setting.has_value())
                MagnifierStaticPosX.set_from_config(std::clamp(setting.value(), 0.0f, 100.0f));
            if (auto setting = readFloat("Magnifier", "StaticPosY"); setting.has_value())
                MagnifierStaticPosY.set_from_config(std::clamp(setting.value(), 0.0f, 100.0f));
        }

        // Output Scaling
        {
            OutputScalingEnabled.set_from_config(readBool("OutputScaling", "Enabled"));

            if (auto v = readEnum<Scaler>("OutputScaling", "Downscaler"))
                OutputScalingDownscaler.set_from_config(*v);
            else
                OutputScalingDownscaler.reset();

            if (auto setting = readFloat("OutputScaling", "Multiplier"); setting.has_value())
                OutputScalingMultiplier.set_from_config(std::clamp(setting.value(), 0.5f, 3.0f));
        }

        // Init Flags
        {
            AutoExposure.set_from_config(readBool("InitFlags", "AutoExposure"));
            HDR.set_from_config(readBool("InitFlags", "HDR"));
            DepthInverted.set_from_config(readBool("InitFlags", "DepthInverted"));
            JitterCancellation.set_from_config(readBool("InitFlags", "JitterCancellation"));
            DisplayResolution.set_from_config(readBool("InitFlags", "DisplayResolution"));
            DynamicResolution.set_from_config(readBool("InitFlags", "DynamicResolution"));
            DisableReactiveMask.set_from_config(readBool("InitFlags", "DisableReactiveMask"));
        }

        // DRS
        {
            DrsMinOverrideEnabled.set_from_config(readBool("DRS", "DrsMinOverrideEnabled"));
            DrsMaxOverrideEnabled.set_from_config(readBool("DRS", "DrsMaxOverrideEnabled"));
        }

        // Upscale Ratio Override
        {
            UpscaleRatioOverrideEnabled.set_from_config(readBool("UpscaleRatio", "UpscaleRatioOverrideEnabled"));
            UpscaleRatioOverrideValue.set_from_config(readFloat("UpscaleRatio", "UpscaleRatioOverrideValue"));
        }

        // Quality Overrides
        {
            QualityRatioOverrideEnabled.set_from_config(readBool("QualityOverrides", "QualityRatioOverrideEnabled"));
            QualityRatio_DLAA.set_from_config(readFloat("QualityOverrides", "QualityRatioDLAA"));
            QualityRatio_UltraQuality.set_from_config(readFloat("QualityOverrides", "QualityRatioUltraQuality"));
            QualityRatio_Quality.set_from_config(readFloat("QualityOverrides", "QualityRatioQuality"));
            QualityRatio_Balanced.set_from_config(readFloat("QualityOverrides", "QualityRatioBalanced"));
            QualityRatio_Performance.set_from_config(readFloat("QualityOverrides", "QualityRatioPerformance"));
            QualityRatio_UltraPerformance.set_from_config(
                readFloat("QualityOverrides", "QualityRatioUltraPerformance"));
        }

        // Anisotropy
        {
            if (auto setting = readInt("Anisotropy", "AnisotropyOverride");
                setting.has_value() && setting.value() <= 16 && setting.value() >= 1)
                AnisotropyOverride.set_from_config(setting);

            if (AnisotropyOverride.has_value() && (AnisotropyOverride.value() > 16 || AnisotropyOverride.value() < 1))
                AnisotropyOverride.reset();

            AnisotropySkipPointFilter.set_from_config(readBool("Anisotropy", "SkipPointFilter"));
            AnisotropyModifyComp.set_from_config(readBool("Anisotropy", "AFModifyComparison"));
            AnisotropyModifyMinMax.set_from_config(readBool("Anisotropy", "AFModifyMinMax"));
        }

        // Mipmap
        {
            if (auto setting = readFloat("Mipmap", "MipmapBiasOverride");
                setting.has_value() && setting.value() <= 15.0 && setting.value() >= -15.0)
                MipmapBiasOverride.set_from_config(setting);

            // Unsure if that's needed but it resets invalid MipmapBiasOverride on config reload
            // Unexpected place for it but could be playing a role
            if (MipmapBiasOverride.has_value() &&
                (MipmapBiasOverride.value() > 15.0 || MipmapBiasOverride.value() < -15.0))
                MipmapBiasOverride.reset();

            MipmapBiasFixedOverride.set_from_config(readBool("Mipmap", "MipmapBiasFixedOverride"));
            MipmapBiasScaleOverride.set_from_config(readBool("Mipmap", "MipmapBiasScaleOverride"));
            MipmapBiasOverrideAll.set_from_config(readBool("Mipmap", "MipmapBiasOverrideAll"));
        }

        // Process Filter
        {
            ProcessExclusionList.set_from_config(readWString("ProcessFilter", "ProcessExclusionList", true));
            TargetProcess.set_from_config(readWString("ProcessFilter", "TargetProcessName", true));
        }

        // Hotfixes
        {
            CheckForUpdate.set_from_config(readBool("Hotfix", "CheckForUpdate"));
            DisableOverlays.set_from_config(readBool("Hotfix", "DisableOverlays"));

            SimulateWaitableObject.set_from_config(readBool("Hotfix", "SimulateWaitableObject"));

            RoundInternalResolution.set_from_config(readInt("Hotfix", "RoundInternalResolution"));

            RestoreComputeSignature.set_from_config(readBool("Hotfix", "RestoreComputeSignature"));
            RestoreGraphicSignature.set_from_config(readBool("Hotfix", "RestoreGraphicSignature"));
            ExtendedStateRestore.set_from_config(readBool("Hotfix", "ExtendedStateRestore"));
            PreferDedicatedGpu.set_from_config(readBool("Hotfix", "PreferDedicatedGpu"));
            PreferFirstDedicatedGpu.set_from_config(readBool("Hotfix", "PreferFirstDedicatedGpu"));
            SkipFirstFrames.set_from_config(readInt("Hotfix", "SkipFirstFrames"));
            UsePrecompiledShaders.set_from_config(readBool("Hotfix", "UsePrecompiledShaders"));
            ColorResourceBarrier.set_from_config(readInt("Hotfix", "ColorResourceBarrier"));
            MVResourceBarrier.set_from_config(readInt("Hotfix", "MotionVectorResourceBarrier"));
            DepthResourceBarrier.set_from_config(readInt("Hotfix", "DepthResourceBarrier"));
            MaskResourceBarrier.set_from_config(readInt("Hotfix", "ColorMaskResourceBarrier"));
            ExposureResourceBarrier.set_from_config(readInt("Hotfix", "ExposureResourceBarrier"));
            OutputResourceBarrier.set_from_config(readInt("Hotfix", "OutputResourceBarrier"));
            CreateD3D12DeviceForLuma.set_from_config(readBool("Hotfix", "CreateD3D12DeviceForLuma"));
        }

        // Dx11 with Dx12
        {
            Dx11DelayedInit.set_from_config(readInt("Dx11withDx12", "UseDelayedInit"));
            DontUseNTShared.set_from_config(readBool("Dx11withDx12", "DontUseNTShared"));
        }

        // NvApi
        {
            DisableFlipMetering.set_from_config(readBool("NvApi", "DisableFlipMetering"));
            DisableReflexSync.set_from_config(readBool("NvApi", "DisableReflexSync"));
            DisableOTA.set_from_config(readBool("NvApi", "DisableOTA"));
        }

        // Spoofing
        {
            DxgiSpoofing.set_from_config(readBool("Spoofing", "Dxgi"));
            DxgiFactoryWrapping.set_from_config(readBool("Spoofing", "DxgiFactoryWrapping"));
            DxgiBlacklist.set_from_config(readString("Spoofing", "DxgiBlacklist"));
            DxgiVRAM.set_from_config(readInt("Spoofing", "DxgiVRAM"));
            VulkanSpoofing.set_from_config(readBool("Spoofing", "Vulkan"));
            VulkanExtensionSpoofing.set_from_config(readBool("Spoofing", "VulkanExtensionSpoofing"));
            VulkanVRAM.set_from_config(readInt("Spoofing", "VulkanVRAM"));
            SpoofedGPUName.set_from_config(readWString("Spoofing", "SpoofedGPUName"));
            StreamlineSpoofing.set_from_config(readBool("Spoofing", "StreamlineSpoofing"));
            SpoofHAGS.set_from_config(readBool("Spoofing", "SpoofHAGS"));
            SpoofFeatureLevel.set_from_config(readBool("Spoofing", "D3DFeatureLevel"));
            SpoofedVendorId.set_from_config(readUInt("Spoofing", "SpoofedVendorId"));
            SpoofedDeviceId.set_from_config(readUInt("Spoofing", "SpoofedDeviceId"));
            TargetVendorId.set_from_config(readUInt("Spoofing", "TargetVendorId"));
            TargetDeviceId.set_from_config(readUInt("Spoofing", "TargetDeviceId"));
            UESpoofIntelAtomics64.set_from_config(readBool("Spoofing", "UEIntelAtomics"));
            SpoofRegistry.set_from_config(readBool("Spoofing", "Registry"));
            SpoofedDriver.set_from_config(readWString("Spoofing", "RegistryDriver"));
            SpoofUser32.set_from_config(readBool("Spoofing", "User32"));

            // Enable HAGS when DLSS-G will be used
            if (!SpoofHAGS.has_value())
            {
                // The library folder as the INI names it: Config::Instance() is still being constructed here.
                std::filesystem::path library(readWString("Libraries", "OptiDllPath").value_or(L"OptiScaler"));
                if (library.is_relative())
                    library = Util::DllPath().parent_path() / library;
                const auto nr = NrBackend.value_or_default();
                const bool danielNr = nr.empty() || _stricmp(nr.c_str(), "auto") == 0 || _stricmp(nr.c_str(), "daniel") == 0;
                const auto output = FGOutput.value_for_config_ignore_default();
                const bool neuralHubFg = DlssNr::AmdBridge::NeuralHubFgInstalled(
                    library, HostComputeBackend.value_or_default(), output == FGOutput::DLSSG,
                    output.has_value() && *output != FGOutput::DLSSG && *output != FGOutput::NoFG, danielNr);
                SpoofHAGS.set_volatile_value(FGInput.value_or_default() == FGInput::NvngxFG ||
                                             FGInput.value_or_default() == FGInput::DLSSG ||
                                             neuralHubFg);
            }
        }

        // fakenvapi
        {
            UseFakenvapi.set_from_config(readBool("fakenvapi", "UseFakenvapi"));
            ForceXeLL.set_from_config(readBool("fakenvapi", "ForceXeLL"));
            FN_ForceLatencyFlex.set_from_config(readBool("fakenvapi", "ForceLatencyFlex"));

            if (auto v = readEnum<LFXMode>("fakenvapi", "LatencyFlexMode"))
                FN_LatencyFlexMode.set_from_config(*v);
            else
                FN_LatencyFlexMode.reset();

            if (auto v = readEnum<ForceReflex>("fakenvapi", "ForceReflex"))
                FN_ForceReflex.set_from_config(*v);
            else
                FN_ForceReflex.reset();

            // 0 was none, it's the default now
            if (auto v = readUInt("fakenvapi", "LowLatencyInput");
                v.has_value() && *v > (uint32_t) ::LowLatencyInput::Auto && *v < (uint32_t) ::LowLatencyInput::_)
                LowLatencyInput.set_from_config((::LowLatencyInput) *v);
            else
                LowLatencyInput.reset();

            if (auto v = readUInt("fakenvapi", "LowLatencyOutput");
                v.has_value() && *v > (uint32_t) LowLatencyMode::Auto && *v < (uint32_t) LowLatencyMode::_)
                LowLatencyOutput.set_from_config((LowLatencyMode) *v);
            else
                LowLatencyOutput.reset();

            // DMFG is a mess with our reflex implementations, disable by default
            if (FGDLSSGOverrideForceDMFG.value_or_default() && !FN_ForceReflex.has_value())
                FN_ForceReflex.set_volatile_value(ForceReflex::ForceDisable);
        }

        // Inputs
        {
            EnableDlssInputs.set_from_config(readBool("Inputs", "EnableDlssInputs"));
            EnableXeSSInputs.set_from_config(readBool("Inputs", "EnableXeSSInputs"));

            EnableFsr2Inputs.set_from_config(readBool("Inputs", "EnableFsr2Inputs"));
            UseFsr2Inputs.set_from_config(readBool("Inputs", "UseFsr2Inputs"));
            UseFsr2Dx11Inputs.set_from_config(readBool("Inputs", "UseFsr2Dx11Inputs"));
            UseFsr2VulkanInputs.set_from_config(readBool("Inputs", "UseFsr2VulkanInputs"));
            Fsr2Pattern.set_from_config(readBool("Inputs", "Fsr2Pattern"));

            EnableFsr3Inputs.set_from_config(readBool("Inputs", "EnableFsr3Inputs"));
            UseFsr3Inputs.set_from_config(readBool("Inputs", "UseFsr3Inputs"));
            Fsr3Pattern.set_from_config(readBool("Inputs", "Fsr3Pattern"));

            EnableFfxInputs.set_from_config(readBool("Inputs", "EnableFfxInputs"));
            UseFfxInputs.set_from_config(readBool("Inputs", "UseFfxInputs"));
            EnableHotSwapping.set_from_config(readBool("Inputs", "EnableHotSwapping"));
        }

        // Plugins
        {
            PluginPath.set_from_config(readWString("Plugins", "Path"));
            LoadSpecialK.set_from_config(readBool("Plugins", "LoadSpecialK"));
            LoadReShade.set_from_config(readBool("Plugins", "LoadReShade"));
            LoadCustomAmdxc64OnRdna2.set_from_config(readBool("Plugins", "LoadCustomAmdxc64OnRdna2"));
            LoadAsiPlugins.set_from_config(readBool("Plugins", "LoadAsiPlugins"));
            LateAsiPluginsDelay.set_from_config(readInt("Plugins", "LateAsiPluginsDelay"));
        }

        // HDR
        {
            ForceHDR.set_from_config(readBool("HDR", "ForceHDR"));
            UseHDR10.set_from_config(readBool("HDR", "UseHDR10"));
            SkipColorSpace.set_from_config(readBool("HDR", "SkipColorSpace"));
        }

        // V-Sync
        {
            OverrideVsync.set_from_config(readBool("V-Sync", "OverrideVsync"));
            ForceVsync.set_from_config(readBool("V-Sync", "ForceVsync"));
            VsyncInterval.set_from_config(readInt("V-Sync", "SyncInterval"));
        }

        // Libraries
        {
            MainDllPath.set_from_config(readWString("Libraries", "OptiDllPath"));

            NvngxPath.set_from_config(readWString("Libraries", "NvngxPath"));
            NVNGX_DLSS_Library.set_from_config(readWString("Libraries", "NvngxDlssPath"));
            DLSSFeaturePath.set_from_config(readWString("Libraries", "NvngxFeaturePath"));
            NvapiDllPath.set_from_config(readWString("Libraries", "NvapiPath"));

            FfxDx12Path.set_from_config(readWString("Libraries", "FfxDx12Path"));
            FfxDx12SRPath.set_from_config(readWString("Libraries", "FfxDx12SRPath"));
            FfxDx12FGPath.set_from_config(readWString("Libraries", "FfxDx12FGPath"));
            FfxDx12RRPath.set_from_config(readWString("Libraries", "FfxDx12RRPath"));
            FfxDx12RCPath.set_from_config(readWString("Libraries", "FfxDx12RCPath"));
            FfxVkPath.set_from_config(readWString("Libraries", "FfxVkPath"));

            XeSSLibrary.set_from_config(readWString("Libraries", "XeSSPath"));
            XeSSLibrary.set_from_config(readWString("Libraries", "XeFGPath"));
            XeSSLibrary.set_from_config(readWString("Libraries", "XeLLPath"));
            XeSSDx11Library.set_from_config(readWString("Libraries", "XeSSDx11Path"));
        }

        // Reading old configs for compatibility reasons
        {
            _DONTUSE_Fsr4ForceEnableInt8.set_from_config(readBool("FSR", "Fsr4ForceEnableInt8"));

            // Named Fsr4ForceModel in older nightlies
            if (!Fsr4GpuSupport.has_value())
            {
                if (auto v = readEnum<FSR4Support>("FSR", "Fsr4ForceModel"))
                    Fsr4GpuSupport.set_from_config(*v);
            }
        }

        return true;
    }

    return false;
}

bool Config::LoadFromPath(const wchar_t* InPath)
{
    std::filesystem::path iniPath(InPath);
    auto newPath = iniPath / fileName;

    if (Reload(newPath))
    {
        absoluteFileName = newPath;
        return true;
    }

    return false;
}

std::string GetBoolValue(std::optional<bool> value)
{
    if (!value.has_value())
        return "auto";

    return value.value() ? "true" : "false";
}

template <typename T> std::string GetIntValue(std::optional<T> value, bool getHex = false)
{
    if (!value.has_value())
        return "auto";

    if constexpr (std::is_enum_v<T>)
    {
        using Underlying = std::underlying_type_t<T>;
        Underlying v = static_cast<Underlying>(value.value());

        if (getHex)
            return std::format("{:#x}", v);

        return std::to_string(v);
    }
    else
    {
        if (getHex)
            return std::format("{:#x}", value.value());

        return std::to_string(value.value());
    }
}

std::string GetFloatValue(std::optional<float> value)
{
    if (!value.has_value())
        return "auto";

    return std::to_string(value.value());
}

std::string GetIntValue(std::optional<int> value)
{
    if (!value.has_value())
        return "auto";

    return std::to_string(value.value());
}

bool Config::SaveIni()
{
    std::lock_guard<std::recursive_mutex> lock(iniMutex);

    // Upscalers
    {
        auto SaveUpscaler = [&](const char* key, auto& upscalerSetting)
        {
            std::string value = upscalerSetting.value_for_config()
                                    .transform(UpscalerToCode) // Turn enum into string
                                    .value_or("auto");

            ini.SetValue("Upscalers", key, value.c_str());
        };

        SaveUpscaler("Dx11Upscaler", Instance()->Dx11Upscaler);
        SaveUpscaler("Dx12Upscaler", Instance()->Dx12Upscaler);
        SaveUpscaler("VulkanUpscaler", Instance()->VulkanUpscaler);
    }

    // Frame Generation
    {
        ini.SetValue("FrameGen", "Enabled", GetBoolValue(Instance()->FGEnabled.value_for_config()).c_str());
        ini.SetValue("FrameGen", "External",
                     GetBoolValue(Instance()->ExternalFrameGeneration.value_for_config()).c_str());
        ini.SetValue("FrameGen", "DebugView", GetBoolValue(Instance()->FGDebugView.value_for_config()).c_str());

        std::string FGInputString =
            Instance()->FGInput.value_for_config().transform(EnumToCode<enum FGInput>).value_or("auto");
        ini.SetValue("FrameGen", "FGInput", FGInputString.c_str());

        std::string FGOutputString =
            Instance()->FGOutput.value_for_config().transform(EnumToCode<enum FGOutput>).value_or("auto");
        ini.SetValue("FrameGen", "FGOutput", FGOutputString.c_str());

        std::string FGNvngxReplacementString = "auto";
        if (auto FGNvngxReplacementHeld = Instance()->FGNvngxReplacement.value_for_config();
            FGNvngxReplacementHeld.has_value())
        {
            if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::None)
                FGNvngxReplacementString = "None";
            else if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::Nukems)
                FGNvngxReplacementString = "Nukems";
            else if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::Arturs)
                FGNvngxReplacementString = "Arturs";
            else if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::FFX)
                FGNvngxReplacementString = "FFX";
            else if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::Combo)
                FGNvngxReplacementString = "Combo";
            else if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::Custom)
                FGNvngxReplacementString = State::Instance().fgNvngxCustom;
        }
        ini.SetValue("FrameGen", "FGNvngxReplacement", FGNvngxReplacementString.c_str());

        std::optional<int> ftInput;
        if (Instance()->FTInput.has_value())
            ftInput = (int) Instance()->FTInput.value();

        ini.SetValue("FrameGen", "FTInput", GetIntValue(ftInput).c_str());
        ini.SetValue("FrameGen", "DrawUIOverFG", GetBoolValue(Instance()->FGDrawUIOverFG.value_for_config()).c_str());
        ini.SetValue("FrameGen", "UIPremultipliedAlpha",
                     GetBoolValue(Instance()->FGUIPremultipliedAlpha.value_for_config()).c_str());
        ini.SetValue("FrameGen", "DisableHudless",
                     GetBoolValue(Instance()->FGDisableHudless.value_for_config()).c_str());
        ini.SetValue("FrameGen", "DisableUI", GetBoolValue(Instance()->FGDisableUI.value_for_config()).c_str());
        ini.SetValue("FrameGen", "SkipReset", GetBoolValue(Instance()->FGSkipReset.value_for_config()).c_str());
        ini.SetValue("FrameGen", "RectLeft", GetIntValue(Instance()->FGRectLeft.value_for_config()).c_str());
        ini.SetValue("FrameGen", "RectTop", GetIntValue(Instance()->FGRectTop.value_for_config()).c_str());
        ini.SetValue("FrameGen", "RectWidth", GetIntValue(Instance()->FGRectWidth.value_for_config()).c_str());
        ini.SetValue("FrameGen", "RectHeight", GetIntValue(Instance()->FGRectHeight.value_for_config()).c_str());
        ini.SetValue("FrameGen", "AllowedFrameAhead",
                     GetIntValue(Instance()->FGAllowedFrameAhead.value_for_config()).c_str());
        ini.SetValue("FrameGen", "DepthValidNow", GetBoolValue(Instance()->FGDepthValidNow.value_for_config()).c_str());
        ini.SetValue("FrameGen", "VelocityValidNow",
                     GetBoolValue(Instance()->FGVelocityValidNow.value_for_config()).c_str());
        ini.SetValue("FrameGen", "HudlessValidNow",
                     GetBoolValue(Instance()->FGHudlessValidNow.value_for_config()).c_str());
        ini.SetValue("FrameGen", "OnlyAcceptFirstHudless",
                     GetBoolValue(Instance()->FGOnlyAcceptFirstHudless.value_for_config()).c_str());
        ini.SetValue("FrameGen", "PreserveSwapChain",
                     GetBoolValue(Instance()->FGPreserveSwapChain.value_for_config()).c_str());
        ini.SetValue("FrameGen", "SkipResizeBuffers",
                     GetBoolValue(Instance()->FGSkipResizeBuffers.value_for_config()).c_str());
        ini.SetValue("FrameGen", "ModifyBufferState",
                     GetBoolValue(Instance()->FGModifyBufferState.value_for_config()).c_str());
        ini.SetValue("FrameGen", "ModifySCIndex", GetBoolValue(Instance()->FGModifySCIndex.value_for_config()).c_str());
        ini.SetValue("FrameGen", "HudCutoff", GetFloatValue(Instance()->FGHudCutoff.value_for_config()).c_str());
    }

    // FSR FG output
    {
        ini.SetValue("FSRFG", "DebugTearLines", GetBoolValue(Instance()->FGDebugTearLines.value_for_config()).c_str());
        ini.SetValue("FSRFG", "DebugResetLines",
                     GetBoolValue(Instance()->FGDebugResetLines.value_for_config()).c_str());
        ini.SetValue("FSRFG", "DebugPacingLines",
                     GetBoolValue(Instance()->FGDebugPacingLines.value_for_config()).c_str());
        ini.SetValue("FSRFG", "AllowAsync", GetBoolValue(Instance()->FGAsync.value_for_config()).c_str());
        ini.SetValue("FSRFG", "UseMutexForSwapchain",
                     GetBoolValue(Instance()->FGUseMutexForSwapchain.value_for_config()).c_str());
        ini.SetValue("FSRFG", "FramePacingTuning",
                     GetBoolValue(Instance()->FGFramePacingTuning.value_for_config()).c_str());
        ini.SetValue("FSRFG", "FPTSafetyMarginInMs",
                     GetFloatValue(Instance()->FGFPTSafetyMarginInMs.value_for_config()).c_str());
        ini.SetValue("FSRFG", "FPTVarianceFactor",
                     GetFloatValue(Instance()->FGFPTVarianceFactor.value_for_config()).c_str());
        ini.SetValue("FSRFG", "FPTHybridSpin",
                     GetBoolValue(Instance()->FGFPTAllowHybridSpin.value_for_config()).c_str());
        ini.SetValue("FSRFG", "FPTHybridSpinTime",
                     GetIntValue(Instance()->FGFPTHybridSpinTime.value_for_config()).c_str());
        ini.SetValue("FSRFG", "FPTWaitForSingleObjectOnFence",
                     GetBoolValue(Instance()->FGFPTAllowWaitForSingleObjectOnFence.value_for_config()).c_str());
        ini.SetValue("FSRFG", "EnableWatermark",
                     GetBoolValue(Instance()->FSRFGEnableWatermark.value_for_config()).c_str());
        ini.SetValue("FSRFG", "DisableFlipMetering",
                     GetBoolValue(Instance()->FSRFGDisableFlipMetering.value_for_config()).c_str());
    }

    // XeFG output
    {
        ini.SetValue("XeFG", "InterpolationCount",
                     GetIntValue(Instance()->FGXeFGInterpolationCount.value_for_config()).c_str());
        ini.SetValue("XeFG", "UnlockMFG", GetBoolValue(Instance()->FGXeFGUnlockEnabled.value_for_config()).c_str());
        ini.SetValue("XeFG", "MaxInterpolatedFrames",
                     GetIntValue(Instance()->FGXeFGMaxInterpolatedFrames.value_for_config()).c_str());
        ini.SetValue("XeFG", "ExtraPacing", GetBoolValue(Instance()->FGXeFGExtraPacing.value_for_config()).c_str());
        ini.SetValue("XeFG", "DynamicMFG", GetBoolValue(Instance()->FGXeFGDynamicMFG.value_for_config()).c_str());
        ini.SetValue("XeFG", "FramerateTargetDMFG",
                     GetFloatValue(Instance()->FGXeFGFramerateTargetDMFG.value_for_config()).c_str());
        ini.SetValue("XeFG", "RoundingDMFG", GetIntValue(Instance()->FGXeFGRoundingDMFG.value_for_config()).c_str());
        ini.SetValue("XeFG", "DynamicAllow1X",
                     GetBoolValue(Instance()->FGXeFGDynamicAllow1X.value_for_config()).c_str());
        ini.SetValue("XeFG", "TargetAsCapDMFG",
                     GetBoolValue(Instance()->FGXeFGTargetAsCapDMFG.value_for_config()).c_str());
        ini.SetValue("XeFG", "OverrideInterpolationCount",
                     GetIntValue(Instance()->FGXeFGOverrideInterpolationCount.value_for_config()).c_str());
        ini.SetValue("XeFG", "IgnoreInitChecks",
                     GetBoolValue(Instance()->FGXeFGIgnoreInitChecks.value_for_config()).c_str());
        ini.SetValue("XeFG", "UIComposition", GetBoolValue(Instance()->FGXeFGUIComposition.value_for_config()).c_str());
        ini.SetValue("XeFG", "DepthInverted", GetBoolValue(Instance()->FGXeFGDepthInverted.value_for_config()).c_str());
        ini.SetValue("XeFG", "JitteredMV", GetBoolValue(Instance()->FGXeFGJitteredMV.value_for_config()).c_str());
        ini.SetValue("XeFG", "HighResMV", GetBoolValue(Instance()->FGXeFGHighResMV.value_for_config()).c_str());
        ini.SetValue("XeFG", "DebugView", GetBoolValue(Instance()->FGXeFGDebugView.value_for_config()).c_str());
        ini.SetValue("XeFG", "ForceBorderless",
                     GetBoolValue(Instance()->FGXeFGForceBorderless.value_for_config()).c_str());
    }

    {
        ini.SetValue("DLSSG", "InterpolationCount",
                     GetIntValue(Instance()->FGDLSSGInterpolationCount.value_for_config()).c_str());
        ini.SetValue("DLSSG", "UseGamesReflexMarkers",
                     GetBoolValue(Instance()->FGDLSSGUseGamesReflexMarkers.value_for_config()).c_str());
        ini.SetValue("DLSSG", "OverrideInterpolationCount",
                     GetIntValue(Instance()->FGDLSSGOverrideInterpolationCount.value_for_config()).c_str());
        ini.SetValue("DLSSG", "FramerateTargetDMFG",
                     GetFloatValue(Instance()->FGDLSSGFramerateTargetDMFG.value_for_config()).c_str());
        ini.SetValue("DLSSG", "OverrideForceDMFG",
                     GetBoolValue(Instance()->FGDLSSGOverrideForceDMFG.value_for_config()).c_str());
        ini.SetValue("DLSSG", "ForceDMFG", GetBoolValue(Instance()->FGDLSSGForceDMFG.value_for_config()).c_str());
        ini.SetValue("DLSSG", "AdaMfgUnlock", GetBoolValue(Instance()->FGDLSSGAdaMfgUnlock.value_for_config()).c_str());
        ini.SetValue("DLSSG", "AdaBlackwellKernels",
                     GetBoolValue(Instance()->FGDLSSGAdaBlackwellKernels.value_for_config()).c_str());
    }

    // Reprojection
    {
        std::string fillMode =
            ReprojectionFillMode.value_for_config().transform(EnumToCode<ReprojectionFill>).value_or("auto");

        ini.SetValue("Reprojection", "FillMode", fillMode.c_str());
        ini.SetValue("Reprojection", "DepthCutoff",
                     GetFloatValue(Instance()->ReprojectionDepthCutoff.value_for_config()).c_str());
        ini.SetValue("Reprojection", "CutoffExpand",
                     GetIntValue(Instance()->ReprojectionCutoffExpand.value_for_config()).c_str());
        ini.SetValue("Reprojection", "LateLatch",
                     GetBoolValue(Instance()->ReprojectionLateLatch.value_for_config()).c_str());
        ini.SetValue("Reprojection", "DirectionCache",
                     GetBoolValue(Instance()->ReprojectionDirectionCache.value_for_config()).c_str());
        ini.SetValue("Reprojection", "ParallaxAuto",
                     GetBoolValue(Instance()->ReprojectionParallaxAuto.value_for_config()).c_str());
        ini.SetValue("Reprojection", "ParallaxScale",
                     GetFloatValue(Instance()->ReprojectionParallaxScale.value_for_config()).c_str());
        ini.SetValue("Reprojection", "ThirdPersonDetection",
                     GetBoolValue(Instance()->ReprojectionThirdPersonDetection.value_for_config()).c_str());
    }

    // OptiFG
    {
        ini.SetValue("OptiFG", "DisableHUDFix", GetBoolValue(Instance()->FGDisableHUDFix.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFix", GetBoolValue(Instance()->FGHUDFix.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDLimit", GetIntValue(Instance()->FGHUDLimit.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFixExtended", GetBoolValue(Instance()->FGHUDFixExtended.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFixUITexture",
                     GetBoolValue(Instance()->FGHUDFixUITexture.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFixUIUnblend",
                     GetBoolValue(Instance()->FGHUDFixUIUnblend.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFixImmediate",
                     GetBoolValue(Instance()->FGImmediateCapture.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFixPersistentBindings",
                     GetBoolValue(Instance()->FGHudfixPersistentBindings.value_for_config()).c_str());
        ini.SetValue("OptiFG", "UseShards", GetBoolValue(Instance()->FGUseShards.value_for_config()).c_str());
        ini.SetValue("OptiFG", "AlwaysTrackHeaps",
                     GetBoolValue(Instance()->FGAlwaysTrackHeaps.value_for_config()).c_str());
        ini.SetValue("OptiFG", "ResourceBlocking",
                     GetBoolValue(Instance()->FGResourceBlocking.value_for_config()).c_str());
        ini.SetValue("OptiFG", "MakeDepthCopy", GetBoolValue(Instance()->FGMakeDepthCopy.value_for_config()).c_str());
        ini.SetValue("OptiFG", "MakeMVCopy", GetBoolValue(Instance()->FGMakeMVCopy.value_for_config()).c_str());

        ini.SetValue("OptiFG", "HudfixDisableRTV",
                     GetBoolValue(Instance()->FGHudfixDisableRTV.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableSRV",
                     GetBoolValue(Instance()->FGHudfixDisableSRV.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableUAV",
                     GetBoolValue(Instance()->FGHudfixDisableUAV.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableOM",
                     GetBoolValue(Instance()->FGHudfixDisableOM.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableDispatch",
                     GetBoolValue(Instance()->FGHudfixDisableDispatch.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableDI",
                     GetBoolValue(Instance()->FGHudfixDisableDI.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableDII",
                     GetBoolValue(Instance()->FGHudfixDisableDII.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableSCR",
                     GetBoolValue(Instance()->FGHudfixDisableSCR.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableSGR",
                     GetBoolValue(Instance()->FGHudfixDisableSGR.value_for_config()).c_str());

        ini.SetValue("OptiFG", "EnableDepthScale",
                     GetBoolValue(Instance()->FGEnableDepthScale.value_for_config()).c_str());
        ini.SetValue("OptiFG", "DepthScaleMax", GetFloatValue(Instance()->FGDepthScaleMax.value_for_config()).c_str());

        ini.SetValue("OptiFG", "HUDFixDontUseSwapchainBuffers",
                     GetBoolValue(Instance()->FGDontUseSwapchainBuffers.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFixRelaxedResolutionCheck",
                     GetBoolValue(Instance()->FGRelaxedResolutionCheck.value_for_config()).c_str());
        ini.SetValue("OptiFG", "ResourceFlip", GetBoolValue(Instance()->FGResourceFlip.value_for_config()).c_str());
        ini.SetValue("OptiFG", "ResourceFlipOffset",
                     GetBoolValue(Instance()->FGResourceFlipOffset.value_for_config()).c_str());

        ini.SetValue("OptiFG", "AlwaysCaptureFSRFGSwapchain",
                     GetBoolValue(Instance()->FGAlwaysCaptureFSRFGSwapchain.value_for_config()).c_str());
    }

    // FSR FG Inputs
    {
        ini.SetValue("FSRFGInputs", "SkipConfigForHudless",
                     GetBoolValue(Instance()->FSRFGSkipConfigForHudless.value_for_config()).c_str());
        ini.SetValue("FSRFGInputs", "SkipDispatchForHudless",
                     GetBoolValue(Instance()->FSRFGSkipDispatchForHudless.value_for_config()).c_str());
    }

    // Framerate
    {
        ini.SetValue("Framerate", "FramerateLimit",
                     GetFloatValue(Instance()->FramerateLimit.value_for_config()).c_str());
    }

    // Output Scaling
    {
        ini.SetValue("OutputScaling", "Enabled",
                     GetBoolValue(Instance()->OutputScalingEnabled.value_for_config()).c_str());
        ini.SetValue("OutputScaling", "Multiplier",
                     GetFloatValue(Instance()->OutputScalingMultiplier.value_for_config()).c_str());
        ini.SetValue("OutputScaling", "Downscaler", GetIntValue(Instance()->OutputScalingDownscaler).c_str());
    }

    // FSR common
    {
        ini.SetValue("FSR", "VerticalFov", GetFloatValue(Instance()->FsrVerticalFov.value_for_config()).c_str());
        ini.SetValue("FSR", "HorizontalFov", GetFloatValue(Instance()->FsrHorizontalFov.value_for_config()).c_str());
        ini.SetValue("FSR", "CameraNear", GetFloatValue(Instance()->FsrCameraNear.value_for_config()).c_str());
        ini.SetValue("FSR", "CameraFar", GetFloatValue(Instance()->FsrCameraFar.value_for_config()).c_str());
        ini.SetValue("FSR", "UseFsrInputValues",
                     GetBoolValue(Instance()->FsrUseFsrInputValues.value_for_config()).c_str());
    }

    // FSR
    {
        ini.SetValue("FSR", "VelocityFactor", GetFloatValue(Instance()->FsrVelocity.value_for_config()).c_str());
        ini.SetValue("FSR", "ReactiveScale", GetFloatValue(Instance()->FsrReactiveScale.value_for_config()).c_str());
        ini.SetValue("FSR", "ShadingScale", GetFloatValue(Instance()->FsrShadingScale.value_for_config()).c_str());
        ini.SetValue("FSR", "AccAddPerFrame", GetFloatValue(Instance()->FsrAccAddPerFrame.value_for_config()).c_str());
        ini.SetValue("FSR", "MinDisOccAcc", GetFloatValue(Instance()->FsrMinDisOccAcc.value_for_config()).c_str());
        ini.SetValue("FSR", "DebugView", GetBoolValue(Instance()->FsrDebugView.value_for_config()).c_str());
        ini.SetValue("FSR", "UpscalerIndex", GetIntValue(Instance()->FfxUpscalerIndex.value_for_config()).c_str());
        ini.SetValue("FSR", "FGIndex", GetIntValue(Instance()->FfxFGIndex.value_for_config()).c_str());
        ini.SetValue("FSR", "UseReactiveMaskForTransparency",
                     GetBoolValue(Instance()->FsrUseMaskForTransparency.value_for_config()).c_str());
        ini.SetValue("FSR", "DlssReactiveMaskBias",
                     GetFloatValue(Instance()->DlssReactiveMaskBias.value_for_config()).c_str());
        ini.SetValue("FSR", "Fsr4GpuSupport", GetIntValue(Instance()->Fsr4GpuSupport.value_for_config()).c_str());
        ini.SetValue("FSR", "Fsr4Preset", GetIntValue(Instance()->Fsr4Preset.value_for_config()).c_str());
        ini.SetValue("FSR", "Fsr4Model", GetIntValue(Instance()->Fsr4Model.value_for_config()).c_str());
        ini.SetValue("FSR", "Fsr4EnableWatermark",
                     GetBoolValue(Instance()->Fsr4EnableWatermark.value_for_config()).c_str());
        ini.SetValue("FSR", "Fsr4DoNotLoadAmdxc64",
                     GetBoolValue(Instance()->Fsr4DoNotLoadAmdxc64.value_for_config()).c_str());
        ini.SetValue("FSR", "FsrNonLinearColorSpace",
                     GetBoolValue(Instance()->FsrNonLinearColorSpace.value_for_config()).c_str());
        ini.SetValue("FSR", "FsrNonLinearPQ", GetBoolValue(Instance()->FsrNonLinearPQ.value_for_config()).c_str());
        ini.SetValue("FSR", "FsrNonLinearSRGB", GetBoolValue(Instance()->FsrNonLinearSRGB.value_for_config()).c_str());
        ini.SetValue("FSR", "FsrAgilitySDKUpgrade",
                     GetBoolValue(Instance()->FsrAgilitySDKUpgrade.value_for_config()).c_str());
        // FSR-RR
        {
            ini.SetValue("FSR-RR", "UseAmdDefaults",
                         GetBoolValue(Instance()->FfxDenoiserUseAmdDefaults.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "DisocclusionThreshold",
                         GetFloatValue(Instance()->FfxDenoiserDisocThreshold.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "CrossBilateralNormalStrength",
                         GetFloatValue(Instance()->FfxDenoiserCrossBlNormStr.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "TemporalStabilityBias",
                         GetFloatValue(Instance()->FfxDenoiserStabilityBias.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "MaxRadiance",
                         GetFloatValue(Instance()->FfxDenoiserMaxRadiance.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "RadianceClipDeviation",
                         GetFloatValue(Instance()->FfxDenoiserRadianceClip.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "GaussianKernelRelaxation",
                         GetFloatValue(Instance()->FfxDenoiserGaussKernRelax.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "HardwareDepth",
                         GetBoolValue(Instance()->FfxDenoiserHardwareDepth.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "ProviderIndex",
                         GetIntValue(Instance()->FfxDenoiserIndex.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "DebugViewLinearDepthMax",
                         GetFloatValue(Instance()->FfxDenoiserDebugDepthMax.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "DebugViewViewport",
                         GetIntValue(Instance()->FfxDenoiserDebugViewport.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "InternalDebugViews",
                         GetBoolValue(Instance()->FfxDenoiserInternalDebugViews.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "Diagnostics",
                         GetBoolValue(Instance()->FfxDenoiserDiagnostics.value_or_default()).c_str());
            ini.SetValue("FSR-RR", "CorrelationBias",
                         GetFloatValue(Instance()->FfxDenoiserCorrelationBias.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "DiffuseHitDistance",
                         GetBoolValue(Instance()->FfxDenoiserDiffuseHitDistance.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorIsolation",
                         GetFloatValue(Instance()->FfxDenoiserFloorIsolation.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "RoughnessFloor",
                         GetFloatValue(Instance()->FfxDenoiserRoughnessFloor.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorHandover",
                         GetIntValue(Instance()->FfxDenoiserFloorHandover.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorHandoverStrength",
                         GetFloatValue(Instance()->FfxDenoiserFloorHandoverStrength.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorRawBlend",
                         GetFloatValue(Instance()->FfxDenoiserFloorRawBlend.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorStructureGate",
                         GetFloatValue(Instance()->FfxDenoiserFloorStructureGate.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "DemodDivisorFloor",
                         GetFloatValue(Instance()->FfxDenoiserDemodDivisorFloor.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorClampSmoothing",
                         GetFloatValue(Instance()->FfxDenoiserFloorClampSmoothing.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorHandoverDetail",
                         GetFloatValue(Instance()->FfxDenoiserFloorHandoverDetail.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorHandoverAnchorClamp",
                         GetFloatValue(Instance()->FfxDenoiserFloorHandoverAnchorClamp.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorHandoverCorrelationMix",
                         GetFloatValue(Instance()->FfxDenoiserFloorHandoverCorrelationMix.value_for_config()).c_str());
            // Unset is a distinct mode (Auto) for both signal keys, so value_for_config()
            // would discard an explicit choice that happens to equal the class default and
            // silently reload it as Auto.
            ini.SetValue(
                "FSR-RR", "DiffuseSignalType",
                GetIntValue(Instance()->FfxDenoiserDiffuseSignalType.value_for_config_ignore_default()).c_str());
            // This is the one FSR-RR key where "unset" is a distinct mode (Auto) rather
            // than a synonym for the class default. value_for_config() discards a value
            // that equals the default, so an explicit "Indirect" - which happens to be
            // the default 1 - would be written back as auto and reload as Auto, silently
            // discarding the user's choice. Preserve whatever was actually set.
            ini.SetValue(
                "FSR-RR", "SpecularSignalType",
                GetIntValue(Instance()->FfxDenoiserSpecularSignalType.value_for_config_ignore_default()).c_str());
            ini.SetValue("FSR-RR", "TaggedNormalRoughness",
                         GetBoolValue(Instance()->FfxDenoiserTaggedNormalRoughness.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "TaggedAmbientOcclusion",
                         GetBoolValue(Instance()->FfxDenoiserTaggedAmbientOcclusion.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "NormalsInViewSpace",
                         GetBoolValue(Instance()->FfxDenoiserNormalsInViewSpace.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "UseTitleLinearDepth",
                         GetBoolValue(Instance()->FfxDenoiserUseTitleLinearDepth.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "ResponsivityTrustThreshold",
                         GetFloatValue(Instance()->FfxDenoiserResponsivityThreshold.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "ResponsivityInvert",
                         GetBoolValue(Instance()->FfxDenoiserResponsivityInvert.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "BiasMaskStrength",
                         GetFloatValue(Instance()->FfxDenoiserBiasMaskStrength.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorDetailBoost",
                         GetFloatValue(Instance()->FfxDenoiserFloorDetailBoost.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorNormalSharpness",
                         GetFloatValue(Instance()->FfxDenoiserFloorNormalSharpness.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorAlbedoGuide",
                         GetFloatValue(Instance()->FfxDenoiserFloorAlbedoGuide.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorLumSymmetry",
                         GetFloatValue(Instance()->FfxDenoiserFloorLumSymmetry.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorGrazingSharpness",
                         GetFloatValue(Instance()->FfxDenoiserFloorGrazingSharpness.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorEnvelopeBias",
                         GetFloatValue(Instance()->FfxDenoiserFloorEnvelopeBias.value_for_config()).c_str());
            ini.SetValue("FSR-RR", "FloorSoftMin",
                         GetFloatValue(Instance()->FfxDenoiserFloorSoftMin.value_for_config()).c_str());
        }

        ini.SetValue("OpenNR-RR", "Enabled", GetBoolValue(Instance()->OpenNrRrEnabled.value_for_config()).c_str());
        ini.SetValue("OpenNR-RR", "NrOnDenoised",
                     Instance()->OpenNrRrNrOnDenoised.value_for_config().value_or("auto").c_str());
        ini.SetValue("OpenNR-RR", "NrImage", Instance()->OpenNrRrNrImage.value_for_config().value_or("auto").c_str());
        ini.SetValue("OpenNR-RR", "NrDeltaRate", GetFloatValue(Instance()->OpenNrRrNrDeltaRate.value_for_config()).c_str());
        ini.SetValue("OpenNR-RR", "Mode", Instance()->OpenNrRrMode.value_for_config().value_or("auto").c_str());
        ini.SetValue("OpenNR-RR", "Fp8", GetBoolValue(Instance()->OpenNrRrFp8.value_for_config()).c_str());
        ini.SetValue("OpenNR-RR", "Backend", Instance()->OpenNrRrBackend.value_for_config().value_or("auto").c_str());
        ini.SetValue("OpenNR-RR", "VkInputLayout", Instance()->OpenNrRrVkInputLayout.value_for_config().value_or("read").c_str());
        ini.SetValue("OpenNR-RR", "UseDisocclusionMask",
                     GetBoolValue(Instance()->OpenNrRrUseDisocclusionMask.value_for_config()).c_str());
        ini.SetValue("OpenNR-RR", "UseSssGuide", GetBoolValue(Instance()->OpenNrRrUseSssGuide.value_for_config()).c_str());
        ini.SetValue("OpenNR-RR", "InvertDisocclusionMask",
                     GetBoolValue(Instance()->OpenNrRrInvertDisocclusionMask.value_for_config()).c_str());
        ini.SetValue("OpenNR-RR", "ClampDisocclusionMask",
                     GetBoolValue(Instance()->OpenNrRrClampDisocclusionMask.value_for_config()).c_str());
        ini.SetValue("OpenNR-RR", "DisocclusionMaskScale",
                     GetFloatValue(Instance()->OpenNrRrDisocclusionMaskScale.value_for_config()).c_str());
        ini.SetValue("OpenNR-RR", "UseResponsivityMask",
                     GetBoolValue(Instance()->OpenNrRrUseResponsivityMask.value_for_config()).c_str());
        ini.SetValue("OpenNR-RR", "UseColorBeforeTransparency",
                     GetBoolValue(Instance()->OpenNrRrUseColorBeforeTransparency.value_for_config()).c_str());
        ini.SetValue("OpenNR-RR", "UseDepthOfFieldGuide",
                     GetBoolValue(Instance()->OpenNrRrUseDepthOfFieldGuide.value_for_config()).c_str());
        ini.SetValue("OpenNR-RR", "UseReflectionMotionVectors",
                     GetBoolValue(Instance()->OpenNrRrUseReflectionMotionVectors.value_for_config()).c_str());
        ini.SetValue("OpenNR-RR", "UseLayerOpacityAndMotion",
                     GetBoolValue(Instance()->OpenNrRrUseLayerOpacityAndMotion.value_for_config()).c_str());
    }

    // XeSS
    {
        ini.SetValue("XeSS", "BuildPipelines", GetBoolValue(Instance()->BuildPipelines.value_for_config()).c_str());
        ini.SetValue("XeSS", "CreateHeaps", GetBoolValue(Instance()->CreateHeaps.value_for_config()).c_str());
        ini.SetValue("XeSS", "NetworkModel", GetIntValue(Instance()->NetworkModel.value_for_config()).c_str());
    }

    // DLSS
    {
        ini.SetValue("DLSS", "Enabled", GetBoolValue(Instance()->DLSSEnabled.value_for_config()).c_str());

        // --- DLSS 5 Neural Rendering (OptiScaler/dlssnr) ---
        ini.SetValue("DlssNr", "Enabled", GetBoolValue(Instance()->DlssNrEnabled.value_for_config()).c_str());
        ini.SetValue("DlssNr", "PresentWithoutUpscaler",
                     GetBoolValue(Instance()->DlssNrPresent.value_for_config()).c_str());
        ini.SetValue("DlssNr", "RunBeforeSR", GetBoolValue(Instance()->DlssNrRunBeforeSr.value_for_config()).c_str());
        ini.SetValue("DlssNr", "ApplyAfterRR", GetBoolValue(Instance()->DlssNrApplyAfterRR.value_for_config()).c_str());
        ini.SetValue("DlssNr", "RRPasses", GetIntValue(Instance()->DlssNrRRPasses.value_for_config()).c_str());
        ini.SetValue("DlssNr", "RRWorkingScale",
                     GetFloatValue(Instance()->DlssNrRRWorkingScale.value_for_config()).c_str());
        {
            auto toggle = Instance()->DlssNrToggleKey.value_for_config();
            ini.SetValue("DlssNr", "ToggleKey", GetIntValue(toggle, toggle > 0).c_str());
        }
        ini.SetValue("DlssNr", "TransferStrength",
                     GetFloatValue(Instance()->DlssNrTransferStrength.value_for_config()).c_str());
        ini.SetValue("DlssNr", "ColourStrength",
                     GetFloatValue(Instance()->DlssNrColourStrength.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MaxRatio", GetFloatValue(Instance()->DlssNrMaxRatio.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Transfer", GetIntValue(Instance()->DlssNrTransfer.value_for_config()).c_str());

        ini.SetValue("DlssNr", "ProbeD3D11", GetBoolValue(Instance()->DlssNrProbeD3D11.value_for_config()).c_str());
        ini.SetValue("DlssNr", "WhitePointFromExposure",
                     GetBoolValue(Instance()->DlssNrWhitePointFromExposure.value_for_config()).c_str());
        ini.SetValue("DlssNr", "DebugView", GetIntValue(Instance()->DlssNrDebugView.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Compare", GetIntValue(Instance()->DlssNrCompare.value_for_config()).c_str());
        ini.SetValue("DlssNr", "CompareSplit",
                     GetFloatValue(Instance()->DlssNrCompareSplit.value_for_config()).c_str());
        ini.SetValue("DlssNr", "CompareZoom", GetFloatValue(Instance()->DlssNrCompareZoom.value_for_config()).c_str());
        ini.SetValue("DlssNr", "CompareSwap", GetBoolValue(Instance()->DlssNrCompareSwap.value_for_config()).c_str());
        ini.SetValue("DlssNr", "CompareTags", GetBoolValue(Instance()->DlssNrCompareTags.value_for_config()).c_str());
        ini.SetValue("DlssNr", "TagScale", GetFloatValue(Instance()->DlssNrTagScale.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Badge", GetBoolValue(Instance()->DlssNrBadge.value_for_config()).c_str());
        ini.SetValue("DlssNr", "BadgeCorner", GetIntValue(Instance()->DlssNrBadgeCorner.value_for_config()).c_str());
        ini.SetValue("DlssNr", "BadgeScale", GetFloatValue(Instance()->DlssNrBadgeScale.value_for_config()).c_str());
        ini.SetValue("DlssNr", "WorkingScale",
                     GetFloatValue(Instance()->DlssNrWorkingScale.value_for_config()).c_str());
        ini.SetValue("DlssNr", "PreUpscale", GetBoolValue(Instance()->DlssNrPreUpscale.value_for_config()).c_str());
        ini.SetValue("DlssNr", "DualFeature", GetBoolValue(Instance()->DlssNrDualFeature.value_for_config()).c_str());
        ini.SetValue(
            "DlssNr", "DualEnlarger",
            Instance()->DlssNrDualEnlarger.value_for_config().transform(UpscalerToCode).value_or("auto").c_str());
        ini.SetValue("DlssNr", "ScalingDownscaler", GetIntValue(Instance()->DlssNrScalingDownscaler).c_str());
        ini.SetValue("DlssNr", "AutoCapture", GetBoolValue(Instance()->DlssNrAutoCapture.value_for_config()).c_str());

        // These were read every launch but never written, so nothing set through the menu survived a
        // restart -- the white-point source, both trims, the anchor, the pass count and the rest all
        // reset to default on the next run.
        ini.SetValue("DlssNr", "WhitePointSource",
                     GetIntValue(Instance()->DlssNrWhitePointSource.value_for_config()).c_str());
        ini.SetValue("DlssNr", "WhitePointTrim",
                     GetFloatValue(Instance()->DlssNrWhitePointTrim.value_for_config()).c_str());
        ini.SetValue("DlssNr", "ScanTrim", GetFloatValue(Instance()->DlssNrScanTrim.value_for_config()).c_str());
        ini.SetValue("DlssNr", "ScanAnchorValue",
                     GetFloatValue(Instance()->DlssNrScanAnchorValue.value_for_config()).c_str());
        ini.SetValue("DlssNr", "ScanAnchorWhitePoint",
                     GetFloatValue(Instance()->DlssNrScanAnchorWhitePoint.value_for_config()).c_str());
        ini.SetValue("DlssNr", "ScanAnchors", Instance()->DlssNrScanAnchors.value_for_config_or("").c_str());
        ini.SetValue("DlssNr", "PassOverrides", Instance()->DlssNrPassOverrides.value_for_config_or("").c_str());
        ini.SetValue("DlssNr", "UnlockPasses", GetBoolValue(Instance()->DlssNrUnlockPasses.value_for_config()).c_str());
        ini.SetValue("DlssNr", "ScanInverted", GetBoolValue(Instance()->DlssNrScanInverted.value_for_config()).c_str());
        ini.SetValue("DlssNr", "ScanMeter", GetBoolValue(Instance()->DlssNrScanMeter.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Passes", GetIntValue(Instance()->DlssNrPasses.value_for_config()).c_str());
        ini.SetValue("DlssNr", "UseProxy", GetBoolValue(Instance()->DlssNrUseProxy.value_for_config()).c_str());
        ini.SetValue("DlssNr", "ProxyProbe", GetBoolValue(Instance()->DlssNrProxyProbe.value_for_config()).c_str());
        // ScanExposure is a developer override with no menu control; persist it so a set ini keeps it.
        ini.SetValue("DlssNr", "ScanExposure", GetBoolValue(Instance()->DlssNrScanExposure.value_for_config()).c_str());
        ini.SetValue("DlssNr", "WhitePointScale",
                     GetFloatValue(Instance()->DlssNrWhitePointScale.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Preset", GetIntValue(Instance()->DlssNrPreset.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Intensity", GetFloatValue(Instance()->DlssNrIntensity.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Style", GetIntValue(Instance()->DlssNrStyle.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass2Preset", GetIntValue(Instance()->DlssNrPass2Preset.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass2Style", GetIntValue(Instance()->DlssNrPass2Style.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass3Preset", GetIntValue(Instance()->DlssNrPass3Preset.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass3Style", GetIntValue(Instance()->DlssNrPass3Style.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LocalStructure",
                     GetFloatValue(Instance()->DlssNrLocalStructure.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LocalTone", GetFloatValue(Instance()->DlssNrLocalTone.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdNeuralLighting",
                     GetBoolValue(Instance()->AmdNeuralLighting.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdNeuralLightingStrength",
                     GetFloatValue(Instance()->AmdNeuralLightingStrength.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdEncoding", GetIntValue(Instance()->AmdEncoding.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdEffectStrength",
                     GetFloatValue(Instance()->AmdEffectStrength.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdStabilizerStrength",
                     GetFloatValue(Instance()->AmdStabilizerStrength.value_for_config_ignore_default()).c_str());
        ini.SetValue("DlssNr", "AmdStabilizerThreshold",
                     GetFloatValue(Instance()->AmdStabilizerThreshold.value_for_config_ignore_default()).c_str());
        ini.SetValue("DlssNr", "RenoComposition",
                     GetBoolValue(Instance()->DlssNrRenoComposition.value_for_config()).c_str());
        ini.SetValue("DlssNr", "RenoIntensity",
                     GetFloatValue(Instance()->DlssNrRenoIntensity.value_for_config()).c_str());
        ini.SetValue("DlssNr", "RenoGuard", GetFloatValue(Instance()->DlssNrRenoGuard.value_for_config()).c_str());
        ini.SetValue("DlssNr", "RenoColour", GetFloatValue(Instance()->DlssNrRenoColour.value_for_config()).c_str());
        ini.SetValue("DlssNr", "RenoChromaClamp",
                     GetFloatValue(Instance()->DlssNrRenoChromaClamp.value_for_config()).c_str());
        ini.SetValue("DlssNr", "RenoPedestal", GetBoolValue(Instance()->DlssNrRenoPedestal.value_for_config()).c_str());
        ini.SetValue("DlssNr", "NrPersonPartition",
                     GetBoolValue(Instance()->NrPersonPartition.value_for_config()).c_str());
        ini.SetValue("DlssNr", "NrPersonModel", GetIntValue(Instance()->NrPersonModel.value_for_config()).c_str());
        ini.SetValue("DlssNr", "NrFaceInputSize", GetIntValue(Instance()->NrFaceInputSize.value_for_config()).c_str());
        ini.SetValue("DlssNr", "NrPersonStrength",
                     GetFloatValue(Instance()->NrPersonStrength.value_for_config()).c_str());
        ini.SetValue("DlssNr", "NrPersonDetailGain",
                     GetFloatValue(Instance()->NrPersonDetailGain.value_for_config()).c_str());
        ini.SetValue("DlssNr", "NrPersonDebugMask",
                     GetBoolValue(Instance()->NrPersonDebugMask.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdQuality", GetIntValue(Instance()->AmdQuality.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdToneCurve", GetIntValue(Instance()->AmdToneCurve.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdToneLift", GetFloatValue(Instance()->AmdToneLift.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdUseGameExposure",
                     GetBoolValue(Instance()->AmdUseGameExposure.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdStyle", GetIntValue(Instance()->AmdStyle.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdSlots", GetIntValue(Instance()->AmdSlots.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdModelScale", GetFloatValue(Instance()->AmdNrScale.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdDynamicScale", GetBoolValue(Instance()->AmdDynamicScale.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdDynamicTargetFps",
                     GetIntValue(Instance()->AmdDynamicTargetFps.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdEveryFrame", GetBoolValue(Instance()->AmdEveryFrame.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdFixedSeed", GetBoolValue(Instance()->AmdFixedSeed.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdSpinDraw", GetIntValue(Instance()->AmdSpinDraw.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdGraphicsWait", GetIntValue(Instance()->AmdGraphicsWait.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AmdStallStandDownMs",
                     GetIntValue(Instance()->AmdStallStandDownMs.value_for_config()).c_str());
        ini.SetValue("DlssNr", "NrBackend", Instance()->NrBackend.value_for_config().value_or("auto").c_str());
        ini.SetValue("DlssNr", "HostComputeBackend",
                     Instance()->HostComputeBackend.value_for_config().value_or("auto").c_str());
        ini.SetValue("DlssNr", "HostZeroCopy", GetBoolValue(Instance()->HostZeroCopy.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LmxxfDiagnostic",
                     Instance()->LmxxfDiagnostic.value_for_config().value_or("auto").c_str());
        ini.SetValue("DlssNr", "LmxxfFitLarge", GetBoolValue(Instance()->LmxxfFitLarge.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LmxxfTemporal", GetBoolValue(Instance()->LmxxfTemporal.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LmxxfSmoothStrength",
                     GetFloatValue(Instance()->LmxxfSmoothStrength.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LmxxfSmoothThreshold",
                     GetFloatValue(Instance()->LmxxfSmoothThreshold.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LmxxfSmoothResidual",
                     GetBoolValue(Instance()->LmxxfSmoothResidual.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LmxxfStyle", GetIntValue(Instance()->LmxxfStyle.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LmxxfVitReuse", GetBoolValue(Instance()->LmxxfVitReuse.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LmxxfCompact1080",
                     GetBoolValue(Instance()->LmxxfCompact1080.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LmxxfOriginalModel",
                     GetBoolValue(Instance()->LmxxfOriginalModel.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LmxxfPassEstimate",
                     GetBoolValue(Instance()->LmxxfPassEstimate.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LmxxfSkinProtect",
                     GetBoolValue(Instance()->LmxxfSkinProtect.value_for_config()).c_str());
        ini.SetValue("DlssNr", "LmxxfAutoExposure",
                     GetBoolValue(Instance()->LmxxfAutoExposure.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiTemporal",
                     GetBoolValue(Instance()->MochizukiTemporal.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiHistoryStrength",
                     GetFloatValue(Instance()->MochizukiHistoryStrength.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiStabilizerStrength",
                     GetFloatValue(Instance()->MochizukiStabilizerStrength.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiStabilizerThreshold",
                     GetFloatValue(Instance()->MochizukiStabilizerThreshold.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Foveation", GetIntValue(Instance()->NrFoveation.value_for_config()).c_str());
        ini.SetValue("DlssNr", "FoveationCentreWidth",
                     GetFloatValue(Instance()->NrFoveationCentreWidth.value_for_config()).c_str());
        ini.SetValue("DlssNr", "FoveationCentreHeight",
                     GetFloatValue(Instance()->NrFoveationCentreHeight.value_for_config()).c_str());
        ini.SetValue("DlssNr", "FoveationFalloff",
                     GetFloatValue(Instance()->NrFoveationFalloff.value_for_config()).c_str());
        ini.SetValue("DlssNr", "FoveationEdge", GetFloatValue(Instance()->NrFoveationEdge.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiAco", GetBoolValue(Instance()->MochizukiAco.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiRdna2Int8",
                     GetBoolValue(Instance()->MochizukiRdna2Int8.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiInt4", GetBoolValue(Instance()->MochizukiInt4.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiOptimizedLayers",
                     GetIntValue(Instance()->MochizukiOptimizedLayers.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiOriginalModel",
                     GetBoolValue(Instance()->MochizukiOriginalModel.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiExperimental",
                     GetBoolValue(Instance()->MochizukiExperimental.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiPlacement",
                     Instance()->MochizukiPlacement.value_for_config().value_or("auto").c_str());
        ini.SetValue("DlssNr", "MochizukiCascadeDetail",
                     GetBoolValue(Instance()->MochizukiCascadeDetail.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiAntiHalo",
                     GetBoolValue(Instance()->MochizukiAntiHalo.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiJitterHistory",
                     GetBoolValue(Instance()->MochizukiJitterHistory.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiAntiHaloStrength",
                     GetFloatValue(Instance()->MochizukiAntiHaloStrength.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiPassEstimate",
                     GetBoolValue(Instance()->MochizukiPassEstimate.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiApplyModel",
                     GetBoolValue(Instance()->MochizukiApplyModel.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiUseDepth",
                     GetBoolValue(Instance()->MochizukiUseDepth.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiDynamicResolution",
                     Instance()->MochizukiDynamicResolution.value_for_config().value_or("auto").c_str());
        ini.SetValue("DlssNr", "MochizukiPreprocess",
                     GetBoolValue(Instance()->MochizukiPreprocess.value_as_chosen()).c_str());
        ini.SetValue("DlssNr", "MochizukiPreprocessExposure",
                     GetIntValue(Instance()->MochizukiPreprocessExposure.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiPreprocessBias",
                     GetFloatValue(Instance()->MochizukiPreprocessBias.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiPreprocessCurve",
                     GetIntValue(Instance()->MochizukiPreprocessCurve.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiPreprocessContrast",
                     GetFloatValue(Instance()->MochizukiPreprocessContrast.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiPreprocessSaturation",
                     GetFloatValue(Instance()->MochizukiPreprocessSaturation.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiLayerMode",
                     GetIntValue(Instance()->MochizukiLayerMode.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiNoiseEveryFrame",
                     GetBoolValue(Instance()->MochizukiNoiseEveryFrame.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiStyleGrade",
                     GetBoolValue(Instance()->MochizukiStyleGrade.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiHistoryPrecision",
                     GetIntValue(Instance()->MochizukiHistoryPrecision.value_for_config()).c_str());
        ini.SetValue("DlssNr", "MochizukiPreprocessInverse",
                     GetBoolValue(Instance()->MochizukiPreprocessInverse.value_for_config()).c_str());
#define MZR_F(name, def, lo, hi, group, label, member, help)                                                           \
    ini.SetValue("DlssNr", #name, GetFloatValue(Instance()->name.value_for_config()).c_str());
#define MZR_U(name, def, lo, hi, group, label, member, help)                                                           \
    ini.SetValue("DlssNr", #name, GetIntValue(Instance()->name.value_for_config()).c_str());
#define MZR_B(name, def, lo, hi, group, label, member, help)                                                           \
    ini.SetValue("DlssNr", #name, GetBoolValue(Instance()->name.value_for_config()).c_str());
#include "dlssnr/backend/MochizukiRenoKeys.inc"
#undef MZR_F
#undef MZR_U
#undef MZR_B
        // RE9's quirk acts on auto (GameQuirk::MochizukiGameExposureFeed): the player's Auto stays 0
        ini.SetValue("DlssNr", "MochizukiRenoFeedMode",
                     GetIntValue(Instance()->MochizukiRenoFeedMode.value_as_chosen()).c_str());
        ini.SetValue("DlssNr", "AmdGraphicsUnsafe",
                     GetIntValue(Instance()->AmdGraphicsUnsafe.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Enabled", GetBoolValue(Instance()->AmdRtgiEnabled.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Quality", GetIntValue(Instance()->AmdRtgiQuality.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Denoiser", GetIntValue(Instance()->AmdRtgiDenoiser.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Inspect", GetIntValue(Instance()->AmdRtgiInspect.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Contact", GetFloatValue(Instance()->AmdRtgiContact.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Saturation", GetFloatValue(Instance()->AmdRtgiSaturation.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Radius", GetFloatValue(Instance()->AmdRtgiRadius.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Mix", GetFloatValue(Instance()->AmdRtgiMix.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Lighting", GetFloatValue(Instance()->AmdRtgiLighting.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Occlusion", GetFloatValue(Instance()->AmdRtgiOcclusion.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Ambient", GetFloatValue(Instance()->AmdRtgiAmbient.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Thickness", GetFloatValue(Instance()->AmdRtgiThickness.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Smoothness", GetFloatValue(Instance()->AmdRtgiSmoothness.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Fade", GetFloatValue(Instance()->AmdRtgiFade.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "Fov", GetFloatValue(Instance()->AmdRtgiFov.value_for_config()).c_str());
        ini.SetValue("AmdRtgi", "FarPlane", GetFloatValue(Instance()->AmdRtgiFarPlane.value_for_config()).c_str());
        ini.SetValue("AmdLook", "Enabled", GetBoolValue(Instance()->AmdLookEnabled.value_for_config()).c_str());
        ini.SetValue("AmdLook", "Appearance", GetIntValue(Instance()->AmdLookAppearance.value_for_config()).c_str());
        ini.SetValue("AmdLook", "Mix", GetFloatValue(Instance()->AmdLookMix.value_for_config()).c_str());
        ini.SetValue("AmdLook", "MaterialDetail",
                     GetFloatValue(Instance()->AmdLookMaterialDetail.value_for_config()).c_str());
        ini.SetValue("AmdLook", "ShapeDefinition",
                     GetFloatValue(Instance()->AmdLookShapeDefinition.value_for_config()).c_str());
        ini.SetValue("AmdLook", "LocalLighting",
                     GetFloatValue(Instance()->AmdLookLocalLighting.value_for_config()).c_str());
        ini.SetValue("AmdLook", "SkinDetail", GetFloatValue(Instance()->AmdLookSkinDetail.value_for_config()).c_str());
        ini.SetValue("AmdLook", "SkinSoftness",
                     GetFloatValue(Instance()->AmdLookSkinSoftness.value_for_config()).c_str());
        ini.SetValue("AmdLook", "DetectSkin", GetBoolValue(Instance()->AmdLookDetectSkin.value_for_config()).c_str());
        ini.SetValue("AmdLook", "SpecularControl",
                     GetFloatValue(Instance()->AmdLookSpecularControl.value_for_config()).c_str());
        ini.SetValue("AmdLook", "HighlightRollOff",
                     GetFloatValue(Instance()->AmdLookHighlightRollOff.value_for_config()).c_str());
        ini.SetValue("AmdLook", "ColourSeparation",
                     GetFloatValue(Instance()->AmdLookColourSeparation.value_for_config()).c_str());
        ini.SetValue("AmdLook", "ShadowDepth",
                     GetFloatValue(Instance()->AmdLookShadowDepth.value_for_config()).c_str());
        ini.SetValue("AmdLook", "AntiHalo", GetFloatValue(Instance()->AmdLookAntiHalo.value_for_config()).c_str());
        ini.SetValue("AmdLook", "FlatAreaProtection",
                     GetFloatValue(Instance()->AmdLookFlatAreaProtection.value_for_config()).c_str());
        ini.SetValue("AmdLook", "Inspect", GetIntValue(Instance()->AmdLookInspect.value_for_config()).c_str());
        ini.SetValue("AmdLook", "Tone", GetFloatValue(Instance()->AmdLookTone.value_for_config()).c_str());
        ini.SetValue("AmdLook", "ExposureEV", GetFloatValue(Instance()->AmdLookExposureEV.value_for_config()).c_str());
        ini.SetValue("AmdLook", "Contrast", GetFloatValue(Instance()->AmdLookContrast.value_for_config()).c_str());
        ini.SetValue("AmdLook", "Saturation", GetFloatValue(Instance()->AmdLookSaturation.value_for_config()).c_str());
        ini.SetValue("AmdLook", "HighlightCompression",
                     GetFloatValue(Instance()->AmdLookHighlightCompression.value_for_config()).c_str());
        ini.SetValue("DlssNr", "SkinStructure",
                     GetFloatValue(Instance()->DlssNrSkinStructure.value_for_config()).c_str());
        ini.SetValue("DlssNr", "AutoMask", GetBoolValue(Instance()->DlssNrAutoMask.value_for_config()).c_str());
        ini.SetValue("DlssNr", "SkinProtection",
                     GetBoolValue(Instance()->DlssNrSkinProtection.value_for_config()).c_str());
        ini.SetValue("DlssNr", "SkinToneEnabled",
                     GetBoolValue(Instance()->DlssNrSkinToneEnabled.value_for_config()).c_str());
        ini.SetValue("DlssNr", "SkinDetail", GetFloatValue(Instance()->DlssNrSkinDetail.value_for_config()).c_str());
        ini.SetValue("DlssNr", "SkinColour", GetFloatValue(Instance()->DlssNrSkinColour.value_for_config()).c_str());
        ini.SetValue("DlssNr", "EnvironmentDetail",
                     GetFloatValue(Instance()->DlssNrEnvironmentDetail.value_for_config()).c_str());
        ini.SetValue("DlssNr", "EnvironmentColour",
                     GetFloatValue(Instance()->DlssNrEnvironmentColour.value_for_config()).c_str());
        ini.SetValue("DlssNr", "ShowSkinMask", GetBoolValue(Instance()->DlssNrShowSkinMask.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass2Intensity",
                     GetFloatValue(Instance()->DlssNrPass2Intensity.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass2LocalStructure",
                     GetFloatValue(Instance()->DlssNrPass2LocalStructure.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass2LocalTone",
                     GetFloatValue(Instance()->DlssNrPass2LocalTone.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass2SkinStructure",
                     GetFloatValue(Instance()->DlssNrPass2SkinStructure.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass2AutoMask",
                     GetBoolValue(Instance()->DlssNrPass2AutoMask.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass3Intensity",
                     GetFloatValue(Instance()->DlssNrPass3Intensity.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass3LocalStructure",
                     GetFloatValue(Instance()->DlssNrPass3LocalStructure.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass3LocalTone",
                     GetFloatValue(Instance()->DlssNrPass3LocalTone.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass3SkinStructure",
                     GetFloatValue(Instance()->DlssNrPass3SkinStructure.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Pass3AutoMask",
                     GetBoolValue(Instance()->DlssNrPass3AutoMask.value_for_config()).c_str());
        ini.SetValue("DlssNr", "ReversibleMode",
                     GetIntValue(Instance()->DlssNrReversibleMode.value_for_config()).c_str());
        ini.SetValue("DlssNr", "ApplyModel", GetBoolValue(Instance()->DlssNrApplyModel.value_for_config()).c_str());
        ini.SetValue("DlssNr", "HoldFrame", GetBoolValue(Instance()->DlssNrHoldFrame.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetOverride",
                     GetBoolValue(Instance()->RenderPresetOverride.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetForAll",
                     GetIntValue(Instance()->RenderPresetForAll.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetDLAA", GetIntValue(Instance()->RenderPresetDLAA.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetUltraQuality",
                     GetIntValue(Instance()->RenderPresetUltraQuality.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetQuality",
                     GetIntValue(Instance()->RenderPresetQuality.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetBalanced",
                     GetIntValue(Instance()->RenderPresetBalanced.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetPerformance",
                     GetIntValue(Instance()->RenderPresetPerformance.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetUltraPerformance",
                     GetIntValue(Instance()->RenderPresetUltraPerformance.value_for_config()).c_str());
        ini.SetValue("DLSS", "UseGenericAppIdWithDlss",
                     GetBoolValue(Instance()->UseGenericAppIdWithDlss.value_for_config()).c_str());
    }

    // DLSSD
    {
        ini.SetValue("DLSSD", "RenderPresetOverride",
                     GetBoolValue(Instance()->DLSSDRenderPresetOverride.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetForAll",
                     GetIntValue(Instance()->DLSSDRenderPresetForAll.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetDLAA",
                     GetIntValue(Instance()->DLSSDRenderPresetDLAA.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetUltraQuality",
                     GetIntValue(Instance()->DLSSDRenderPresetUltraQuality.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetQuality",
                     GetIntValue(Instance()->DLSSDRenderPresetQuality.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetBalanced",
                     GetIntValue(Instance()->DLSSDRenderPresetBalanced.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetPerformance",
                     GetIntValue(Instance()->DLSSDRenderPresetPerformance.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetUltraPerformance",
                     GetIntValue(Instance()->DLSSDRenderPresetUltraPerformance.value_for_config()).c_str());
    }

    // NvngxFG
    {
        ini.SetValue("NvngxFG", "MakeDepthCopy",
                     GetBoolValue(Instance()->NvngxFGMakeDepthCopy.value_for_config()).c_str());
        ini.SetValue("NvngxFG", "DispatchFlags",
                     GetIntValue(Instance()->NvngxFGDispatchFlags.value_for_config(), true).c_str());
        ini.SetValue("NvngxFG", "ShowDebug", GetBoolValue(Instance()->NvngxFGShowDebug.value_for_config()).c_str());
        ini.SetValue("NvngxFG", "DisableHudless",
                     GetBoolValue(Instance()->NvngxFGDisableHudless.value_for_config()).c_str());
    }

    // Sharpness
    {
        std::string shader = SharpnessShader.value_for_config()
                                 .transform(EnumToCode<SharpenShader>) // Turn enum into string
                                 .value_or("auto");

        ini.SetValue("Sharpness", "Shader", shader.c_str());

        ini.SetValue("Sharpness", "OverrideSharpness",
                     GetBoolValue(Instance()->OverrideSharpness.value_for_config()).c_str());
        ini.SetValue("Sharpness", "Sharpness", GetFloatValue(Instance()->Sharpness.value_for_config()).c_str());
    }

    // CAS
    {
        ini.SetValue("CAS", "Enabled", GetBoolValue(Instance()->RcasEnabled.value_for_config()).c_str());

        ini.SetValue("CAS", "MotionSharpnessEnabled",
                     GetBoolValue(Instance()->MotionSharpnessEnabled.value_for_config()).c_str());
        ini.SetValue("CAS", "MotionSharpness", GetFloatValue(Instance()->MotionSharpness.value_for_config()).c_str());
        ini.SetValue("CAS", "MotionThreshold", GetFloatValue(Instance()->MotionThreshold.value_for_config()).c_str());
        ini.SetValue("CAS", "MotionScaleLimit", GetFloatValue(Instance()->MotionScaleLimit.value_for_config()).c_str());

        ini.SetValue("CAS", "ContrastEnabled", GetBoolValue(Instance()->ContrastEnabled.value_for_config()).c_str());
        ini.SetValue("CAS", "Contrast", GetFloatValue(Instance()->Contrast.value_for_config()).c_str());

        ini.SetValue("CAS", "DADepthScale", GetFloatValue(Instance()->DADepthScale.value_for_config()).c_str());
        ini.SetValue("CAS", "DADepthBias", GetFloatValue(Instance()->DADepthBias.value_for_config()).c_str());
        ini.SetValue("CAS", "DAClampOutput", GetBoolValue(Instance()->DAClampOutput.value_for_config()).c_str());
        ini.SetValue("CAS", "UseDepthAwareSharpen",
                     GetBoolValue(Instance()->UseDepthAwareSharpen.value_for_config()).c_str());
        ini.SetValue("CAS", "UseDASDepthAwareSharpen",
                     GetBoolValue(Instance()->UseDASDepthAwareSharpen.value_for_config()).c_str());
        ini.SetValue("CAS", "DADepthIsLinear", GetBoolValue(Instance()->DADepthIsLinear.value_for_config()).c_str());

        ini.SetValue("CAS", "SharpenerDebug",
                     GetBoolValue(Instance()->MotionSharpnessDebug.value_for_config()).c_str());
    }

    // Magnifier
    {
        ini.SetValue("Magnifier", "Enabled", GetBoolValue(Instance()->MagnifierEnabled.value_for_config()).c_str());

        ini.SetValue("Magnifier", "Size", GetFloatValue(Instance()->MagnifierSize.value_for_config()).c_str());
        ini.SetValue("Magnifier", "ZoomFactor",
                     GetIntValue(Instance()->MagnifierZoomFactor.value_for_config()).c_str());

        ini.SetValue("Magnifier", "BorderSize",
                     GetFloatValue(Instance()->MagnifierBorderSize.value_for_config()).c_str());
        ini.SetValue("Magnifier", "CursorOffsetX",
                     GetFloatValue(Instance()->MagnifierCursorOffsetX.value_for_config()).c_str());
        ini.SetValue("Magnifier", "CursorOffsetY",
                     GetFloatValue(Instance()->MagnifierCursorOffsetY.value_for_config()).c_str());

        ini.SetValue("Magnifier", "StaticPosX",
                     GetFloatValue(Instance()->MagnifierStaticPosX.value_for_config()).c_str());
        ini.SetValue("Magnifier", "StaticPosY",
                     GetFloatValue(Instance()->MagnifierStaticPosY.value_for_config()).c_str());
    }

    // Menu
    {
        ini.SetValue("Menu", "Scale", GetFloatValue(Instance()->MenuScale).c_str());
        ini.SetValue("Menu", "Width", GetFloatValue(Instance()->MenuWidth).c_str());
        ini.SetValue("Menu", "Height", GetFloatValue(Instance()->MenuHeight).c_str());
        ini.SetValue("Menu", "Anchor", GetIntValue(Instance()->MenuAnchor.value_for_config()).c_str());
        ini.SetValue("Menu", "SectionOrder", Instance()->MenuSectionOrder.value_for_config_or("auto").c_str());
        ini.SetValue("Menu", "Basic", GetBoolValue(Instance()->MenuBasic.value_for_config()).c_str());
        ini.SetValue("Menu", "OverlayMenu", GetBoolValue(Instance()->OverlayMenu.value_for_config()).c_str());

        auto setting = Instance()->ShortcutKey.value_for_config();
        ini.SetValue("Menu", "ShortcutKey",
                     GetIntValue(Instance()->ShortcutKey.value_for_config(), setting > 0).c_str());

        ini.SetValue("Menu", "ExtendedLimits", GetBoolValue(Instance()->ExtendedLimits.value_for_config()).c_str());
        ini.SetValue("Menu", "ShowFps", GetBoolValue(Instance()->ShowFps.value_for_config()).c_str());
        ini.SetValue("Menu", "UseHQFont", GetBoolValue(Instance()->UseHQFont.value_for_config()).c_str());
        ini.SetValue("Menu", "DisableSplash", GetBoolValue(Instance()->DisableSplash.value_for_config()).c_str());

        setting = Instance()->FGShortcutKey.value_for_config();
        ini.SetValue("Menu", "FGShortcutKey",
                     GetIntValue(Instance()->FGShortcutKey.value_for_config(), setting > 0).c_str());

        setting = Instance()->ScreenshotKey.value_for_config();
        ini.SetValue("Menu", "ScreenshotKey",
                     GetIntValue(Instance()->ScreenshotKey.value_for_config(), setting > 0).c_str());
        ini.SetValue("Menu", "ScreenshotPath",
                     wstring_to_string(Instance()->ScreenshotPath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Menu", "ScreenshotWithMenu",
                     GetBoolValue(Instance()->ScreenshotWithMenu.value_for_config()).c_str());

        setting = Instance()->FpsShortcutKey.value_for_config();
        ini.SetValue("Menu", "FpsShortcutKey",
                     GetIntValue(Instance()->FpsShortcutKey.value_for_config(), setting > 0).c_str());

        setting = Instance()->FpsCycleShortcutKey.value_for_config();
        ini.SetValue("Menu", "FpsCycleShortcutKey",
                     GetIntValue(Instance()->FpsCycleShortcutKey.value_for_config(), setting > 0).c_str());

        ini.SetValue("Menu", "FpsOverlayPos", GetIntValue(Instance()->FpsOverlayPosition.value_for_config()).c_str());
        ini.SetValue("Menu", "FpsOverlayType", GetIntValue(Instance()->FpsOverlayType.value_for_config()).c_str());
        ini.SetValue("Menu", "FpsOverlayHorizontal",
                     GetBoolValue(Instance()->FpsOverlayHorizontal.value_for_config()).c_str());
        ini.SetValue("Menu", "FpsOverlayShowRatio",
                     GetBoolValue(Instance()->FpsOverlayShowRatio.value_for_config()).c_str());
        ini.SetValue("Menu", "FpsOverlayAlpha", GetFloatValue(Instance()->FpsOverlayAlpha.value_for_config()).c_str());
        ini.SetValue("Menu", "FpsScale", GetFloatValue(Instance()->FpsScale.value_for_config()).c_str());
        ini.SetValue("Menu", "FontSize", GetFloatValue(Instance()->FontSize.value_for_config()).c_str());
        ini.SetValue("Menu", "Language", Instance()->MenuLanguage.value_for_config_or("auto").c_str());
        ini.SetValue("Menu", "TTFFontPath",
                     wstring_to_string(Instance()->TTFFontPath.value_for_config_or(L"auto")).c_str());

        ini.SetValue("Menu", "LightTheme", GetBoolValue(Instance()->LightTheme.value_for_config()).c_str());
        ini.SetValue("Menu", "OverlaysUseTheme", GetBoolValue(Instance()->OverlaysUseTheme.value_for_config()).c_str());
        ini.SetValue("Menu", "AccentColorR", GetFloatValue(Instance()->MenuAccentColorR.value_for_config()).c_str());
        ini.SetValue("Menu", "AccentColorG", GetFloatValue(Instance()->MenuAccentColorG.value_for_config()).c_str());
        ini.SetValue("Menu", "AccentColorB", GetFloatValue(Instance()->MenuAccentColorB.value_for_config()).c_str());
        ini.SetValue("Menu", "BGColorR", GetFloatValue(Instance()->MenuBGColorR.value_for_config()).c_str());
        ini.SetValue("Menu", "BGColorG", GetFloatValue(Instance()->MenuBGColorG.value_for_config()).c_str());
        ini.SetValue("Menu", "BGColorB", GetFloatValue(Instance()->MenuBGColorB.value_for_config()).c_str());
        ini.SetValue("Menu", "BGColorA", GetFloatValue(Instance()->MenuBGColorA.value_for_config()).c_str());
        ini.SetValue("Menu", "Blur", GetBoolValue(Instance()->MenuBlur.value_for_config()).c_str());
        ini.SetValue("Menu", "BlurStrength", GetFloatValue(Instance()->MenuBlurStrength.value_for_config()).c_str());
        ini.SetValue("Menu", "CustomTabEnabled", GetBoolValue(Instance()->CustomTabEnabled.value_for_config()).c_str());
        ini.SetValue("Menu", "CustomTabCards", Instance()->CustomTabCards.value_for_config_or("auto").c_str());
    }

    // Hooks
    {
        ini.SetValue("Hooks", "HookOriginalNvngxOnly",
                     GetBoolValue(Instance()->HookOriginalNvngxOnly.value_for_config()).c_str());
        ini.SetValue("Hooks", "EarlyHooking", GetBoolValue(Instance()->EarlyHooking.value_for_config()).c_str());
        ini.SetValue("Hooks", "UseNtdllHooks", GetBoolValue(Instance()->UseNtdllHooks.value_for_config()).c_str());
    }

    // InitFlags
    {
        ini.SetValue("InitFlags", "AutoExposure", GetBoolValue(Instance()->AutoExposure.value_for_config()).c_str());
        ini.SetValue("InitFlags", "HDR", GetBoolValue(Instance()->HDR.value_for_config()).c_str());
        ini.SetValue("InitFlags", "DepthInverted", GetBoolValue(Instance()->DepthInverted.value_for_config()).c_str());
        ini.SetValue("InitFlags", "JitterCancellation",
                     GetBoolValue(Instance()->JitterCancellation.value_for_config()).c_str());
        ini.SetValue("InitFlags", "DisplayResolution",
                     GetBoolValue(Instance()->DisplayResolution.value_for_config()).c_str());
        ini.SetValue("InitFlags", "DynamicResolution",
                     GetBoolValue(Instance()->DynamicResolution.value_for_config()).c_str());
        ini.SetValue("InitFlags", "DisableReactiveMask",
                     GetBoolValue(Instance()->DisableReactiveMask.value_for_config()).c_str());
    }

    // Upscale Ratio Override
    {
        ini.SetValue("UpscaleRatio", "UpscaleRatioOverrideEnabled",
                     GetBoolValue(Instance()->UpscaleRatioOverrideEnabled.value_for_config()).c_str());
        ini.SetValue("UpscaleRatio", "UpscaleRatioOverrideValue",
                     GetFloatValue(Instance()->UpscaleRatioOverrideValue.value_for_config()).c_str());
    }

    // Quality Overrides
    {
        ini.SetValue("QualityOverrides", "QualityRatioOverrideEnabled",
                     GetBoolValue(Instance()->QualityRatioOverrideEnabled.value_for_config()).c_str());
        ini.SetValue("QualityOverrides", "QualityRatioDLAA",
                     GetFloatValue(Instance()->QualityRatio_DLAA.value_for_config()).c_str());
        ini.SetValue("QualityOverrides", "QualityRatioUltraQuality",
                     GetFloatValue(Instance()->QualityRatio_UltraQuality.value_for_config()).c_str());
        ini.SetValue("QualityOverrides", "QualityRatioQuality",
                     GetFloatValue(Instance()->QualityRatio_Quality.value_for_config()).c_str());
        ini.SetValue("QualityOverrides", "QualityRatioBalanced",
                     GetFloatValue(Instance()->QualityRatio_Balanced.value_for_config()).c_str());
        ini.SetValue("QualityOverrides", "QualityRatioPerformance",
                     GetFloatValue(Instance()->QualityRatio_Performance.value_for_config()).c_str());
        ini.SetValue("QualityOverrides", "QualityRatioUltraPerformance",
                     GetFloatValue(Instance()->QualityRatio_UltraPerformance.value_for_config()).c_str());
    }

    // Anisotropy
    {
        ini.SetValue("Anisotropy", "AnisotropyOverride",
                     GetIntValue(Instance()->AnisotropyOverride.value_for_config()).c_str());
        ini.SetValue("Anisotropy", "ModifyComparison",
                     GetBoolValue(Instance()->AnisotropyModifyComp.value_for_config()).c_str());
        ini.SetValue("Anisotropy", "ModifyMinMax",
                     GetBoolValue(Instance()->AnisotropyModifyMinMax.value_for_config()).c_str());
        ini.SetValue("Anisotropy", "SkipPointFilter",
                     GetBoolValue(Instance()->AnisotropySkipPointFilter.value_for_config()).c_str());
    }

    // Mipmap
    {
        ini.SetValue("Mipmap", "MipmapBiasOverride",
                     GetFloatValue(Instance()->MipmapBiasOverride.value_for_config()).c_str());
        ini.SetValue("Mipmap", "MipmapBiasOverrideAll",
                     GetBoolValue(Instance()->MipmapBiasOverrideAll.value_for_config()).c_str());
        ini.SetValue("Mipmap", "MipmapBiasFixedOverride",
                     GetBoolValue(Instance()->MipmapBiasFixedOverride.value_for_config()).c_str());
        ini.SetValue("Mipmap", "MipmapBiasScaleOverride",
                     GetBoolValue(Instance()->MipmapBiasScaleOverride.value_for_config()).c_str());
    }

    // Process Filter
    {
        ini.SetValue("ProcessFilter", "TargetProcessName",
                     wstring_to_string(Instance()->TargetProcess.value_for_config_or(L"auto")).c_str());
        ini.SetValue("ProcessFilter", "ProcessExclusionList",
                     wstring_to_string(Instance()->ProcessExclusionList.value_for_config_or(L"auto")).c_str());
    }

    // Hotfixes
    {
        ini.SetValue("Hotfix", "CreateD3D12DeviceForLuma",
                     GetBoolValue(Instance()->CreateD3D12DeviceForLuma.value_for_config()).c_str());
        ini.SetValue("Hotfix", "CheckForUpdate", GetBoolValue(Instance()->CheckForUpdate.value_for_config()).c_str());
        ini.SetValue("Hotfix", "SimulateWaitableObject",
                     GetBoolValue(Instance()->SimulateWaitableObject.value_for_config()).c_str());
        ini.SetValue("Hotfix", "DisableOverlays", GetBoolValue(Instance()->DisableOverlays.value_for_config()).c_str());

        ini.SetValue("Hotfix", "RoundInternalResolution",
                     GetIntValue(Instance()->RoundInternalResolution.value_for_config()).c_str());

        ini.SetValue("Hotfix", "RestoreComputeSignature",
                     GetBoolValue(Instance()->RestoreComputeSignature.value_for_config()).c_str());
        ini.SetValue("Hotfix", "RestoreGraphicSignature",
                     GetBoolValue(Instance()->RestoreGraphicSignature.value_for_config()).c_str());
        ini.SetValue("Hotfix", "ExtendedStateRestore",
                     GetBoolValue(Instance()->ExtendedStateRestore.value_for_config()).c_str());
        ini.SetValue("Hotfix", "SkipFirstFrames", GetIntValue(Instance()->SkipFirstFrames.value_for_config()).c_str());

        ini.SetValue("Hotfix", "UsePrecompiledShaders",
                     GetBoolValue(Instance()->UsePrecompiledShaders.value_for_config()).c_str());
        ini.SetValue("Hotfix", "PreferDedicatedGpu",
                     GetBoolValue(Instance()->PreferDedicatedGpu.value_for_config()).c_str());
        ini.SetValue("Hotfix", "PreferFirstDedicatedGpu",
                     GetBoolValue(Instance()->PreferFirstDedicatedGpu.value_for_config()).c_str());

        ini.SetValue("Hotfix", "ColorResourceBarrier",
                     GetIntValue(Instance()->ColorResourceBarrier.value_for_config()).c_str());
        ini.SetValue("Hotfix", "MotionVectorResourceBarrier",
                     GetIntValue(Instance()->MVResourceBarrier.value_for_config()).c_str());
        ini.SetValue("Hotfix", "DepthResourceBarrier",
                     GetIntValue(Instance()->DepthResourceBarrier.value_for_config()).c_str());
        ini.SetValue("Hotfix", "ColorMaskResourceBarrier",
                     GetIntValue(Instance()->MaskResourceBarrier.value_for_config()).c_str());
        ini.SetValue("Hotfix", "ExposureResourceBarrier",
                     GetIntValue(Instance()->ExposureResourceBarrier.value_for_config()).c_str());
        ini.SetValue("Hotfix", "OutputResourceBarrier",
                     GetIntValue(Instance()->OutputResourceBarrier.value_for_config()).c_str());
    }

    // Dx11 with Dx12
    {
        ini.SetValue("Dx11withDx12", "DontUseNTShared",
                     GetBoolValue(Instance()->DontUseNTShared.value_for_config()).c_str());
    }

    // Logging
    {
        ini.SetValue("Log", "LogToFile", GetBoolValue(Instance()->LogToFile.value_for_config()).c_str());
        ini.SetValue("Log", "LogLevel", GetIntValue(Instance()->LogLevel.value_for_config()).c_str());
        ini.SetValue("Log", "LogToConsole", GetBoolValue(Instance()->LogToConsole.value_for_config()).c_str());
        ini.SetValue("Log", "LogToDebug", GetBoolValue(Instance()->LogToDebug.value_for_config()).c_str());
        ini.SetValue("Log", "LogToNGX", GetBoolValue(Instance()->LogToNGX.value_for_config()).c_str());
        ini.SetValue("Log", "OpenConsole", GetBoolValue(Instance()->OpenConsole.value_for_config()).c_str());
        ini.SetValue("Log", "SingleFile", GetBoolValue(Instance()->LogSingleFile.value_for_config()).c_str());
        ini.SetValue("Log", "LogFileName",
                     wstring_to_string(Instance()->LogFileName.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Log", "LogAsync", GetBoolValue(Instance()->LogAsync.value_for_config()).c_str());
        ini.SetValue("Log", "LogAsyncThreads", GetIntValue(Instance()->LogAsyncThreads.value_for_config()).c_str());
    }

    // NvApi
    {
        ini.SetValue("NvApi", "DisableFlipMetering",
                     GetBoolValue(Instance()->DisableFlipMetering.value_for_config()).c_str());
        ini.SetValue("NvApi", "DisableReflexSync",
                     GetBoolValue(Instance()->DisableReflexSync.value_for_config()).c_str());
        ini.SetValue("NvApi", "DisableOTA", GetBoolValue(Instance()->DisableOTA.value_for_config()).c_str());
    }

    // DRS
    {
        ini.SetValue("DRS", "DrsMinOverrideEnabled",
                     GetBoolValue(Instance()->DrsMinOverrideEnabled.value_for_config()).c_str());
        ini.SetValue("DRS", "DrsMaxOverrideEnabled",
                     GetBoolValue(Instance()->DrsMaxOverrideEnabled.value_for_config()).c_str());
    }

    // Spoofing
    {
        ini.SetValue("Spoofing", "Dxgi", GetBoolValue(Instance()->DxgiSpoofing.value_for_config()).c_str());
        ini.SetValue("Spoofing", "DxgiFactoryWrapping",
                     GetBoolValue(Instance()->DxgiFactoryWrapping.value_for_config()).c_str());
        ini.SetValue("Spoofing", "DxgiBlacklist", Instance()->DxgiBlacklist.value_for_config_or("auto").c_str());
        ini.SetValue("Spoofing", "Vulkan", GetBoolValue(Instance()->VulkanSpoofing.value_for_config()).c_str());
        ini.SetValue("Spoofing", "VulkanExtensionSpoofing",
                     GetBoolValue(Instance()->VulkanExtensionSpoofing.value_for_config()).c_str());
        ini.SetValue("Spoofing", "VulkanVRAM", GetIntValue(Instance()->VulkanVRAM.value_for_config()).c_str());
        ini.SetValue("Spoofing", "DxgiVRAM", GetIntValue(Instance()->DxgiVRAM.value_for_config()).c_str());
        ini.SetValue("Spoofing", "SpoofedGPUName",
                     wstring_to_string(Instance()->SpoofedGPUName.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Spoofing", "StreamlineSpoofing",
                     GetBoolValue(Instance()->StreamlineSpoofing.value_for_config()).c_str());
        ini.SetValue("Spoofing", "SpoofHAGS", GetBoolValue(Instance()->SpoofHAGS.value_for_config()).c_str());
        ini.SetValue("Spoofing", "D3DFeatureLevel",
                     GetBoolValue(Instance()->SpoofFeatureLevel.value_for_config()).c_str());
        ini.SetValue("Spoofing", "UEIntelAtomics",
                     GetBoolValue(Instance()->UESpoofIntelAtomics64.value_for_config()).c_str());
        ini.SetValue("Spoofing", "SpoofedVendorId",
                     GetIntValue(Instance()->SpoofedVendorId.value_for_config(), true).c_str());
        ini.SetValue("Spoofing", "SpoofedDeviceId",
                     GetIntValue(Instance()->SpoofedDeviceId.value_for_config(), true).c_str());
        ini.SetValue("Spoofing", "TargetVendorId",
                     GetIntValue(Instance()->TargetVendorId.value_for_config(), true).c_str());
        ini.SetValue("Spoofing", "TargetDeviceId",
                     GetIntValue(Instance()->TargetDeviceId.value_for_config(), true).c_str());
        ini.SetValue("Spoofing", "Registry", GetBoolValue(Instance()->SpoofRegistry.value_for_config()).c_str());
        ini.SetValue("Spoofing", "RegistryDriver",
                     wstring_to_string(Instance()->SpoofedDriver.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Spoofing", "User32", GetBoolValue(Instance()->SpoofUser32.value_for_config()).c_str());
    }

    // Plugins
    {

        ini.SetValue("Plugins", "Path", wstring_to_string(Instance()->PluginPath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Plugins", "LoadSpecialK", GetBoolValue(Instance()->LoadSpecialK.value_for_config()).c_str());
        ini.SetValue("Plugins", "LoadReShade", GetBoolValue(Instance()->LoadReShade.value_for_config()).c_str());
        ini.SetValue("Plugins", "LoadCustomAmdxc64OnRdna2",
                     GetBoolValue(Instance()->LoadCustomAmdxc64OnRdna2.value_for_config()).c_str());
        ini.SetValue("Plugins", "LoadAsiPlugins", GetBoolValue(Instance()->LoadAsiPlugins.value_for_config()).c_str());
        ini.SetValue("Plugins", "LateAsiPluginsDelay",
                     GetIntValue(Instance()->LateAsiPluginsDelay.value_for_config()).c_str());
    }

    // fakenvapi
    {
        ini.SetValue("fakenvapi", "UseFakenvapi", GetBoolValue(Instance()->UseFakenvapi.value_for_config()).c_str());
        ini.SetValue("fakenvapi", "ForceXeLL", GetBoolValue(Instance()->ForceXeLL.value_for_config()).c_str());
        ini.SetValue("fakenvapi", "ForceLatencyFlex",
                     GetBoolValue(Instance()->FN_ForceLatencyFlex.value_for_config()).c_str());
        ini.SetValue("fakenvapi", "LatencyFlexMode",
                     GetIntValue(Instance()->FN_LatencyFlexMode.value_for_config()).c_str());
        ini.SetValue("fakenvapi", "ForceReflex", GetIntValue(Instance()->FN_ForceReflex.value_for_config()).c_str());
        ini.SetValue("fakenvapi", "LowLatencyInput",
                     GetIntValue(Instance()->LowLatencyInput.value_for_config()).c_str());
        ini.SetValue("fakenvapi", "LowLatencyOutput",
                     GetIntValue(Instance()->LowLatencyOutput.value_for_config()).c_str());
    }

    // inputs
    {
        ini.SetValue("Inputs", "EnableDlssInputs",
                     GetBoolValue(Instance()->EnableDlssInputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "EnableXeSSInputs",
                     GetBoolValue(Instance()->EnableXeSSInputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "UseFsr2Inputs", GetBoolValue(Instance()->UseFsr2Inputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "UseFsr2Dx11Inputs",
                     GetBoolValue(Instance()->UseFsr2Dx11Inputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "UseFsr2VulkanInputs",
                     GetBoolValue(Instance()->UseFsr2VulkanInputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "Fsr2Pattern", GetBoolValue(Instance()->Fsr2Pattern.value_for_config()).c_str());
        ini.SetValue("Inputs", "UseFsr3Inputs", GetBoolValue(Instance()->UseFsr3Inputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "Fsr3Pattern", GetBoolValue(Instance()->Fsr3Pattern.value_for_config()).c_str());
        ini.SetValue("Inputs", "UseFfxInputs", GetBoolValue(Instance()->UseFfxInputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "EnableHotSwapping",
                     GetBoolValue(Instance()->EnableHotSwapping.value_for_config()).c_str());

        ini.SetValue("Inputs", "EnableFsr2Inputs",
                     GetBoolValue(Instance()->EnableFsr2Inputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "EnableFsr3Inputs",
                     GetBoolValue(Instance()->EnableFsr3Inputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "EnableFfxInputs", GetBoolValue(Instance()->EnableFfxInputs.value_for_config()).c_str());
    }

    // V-Sync
    {
        ini.SetValue("V-Sync", "OverrideVsync", GetBoolValue(Instance()->OverrideVsync.value_for_config()).c_str());
        ini.SetValue("V-Sync", "ForceVsync", GetBoolValue(Instance()->ForceVsync.value_for_config()).c_str());
        ini.SetValue("V-Sync", "SyncInterval", GetIntValue(Instance()->VsyncInterval.value_for_config()).c_str());

        if (Instance()->VsyncInterval.has_value())
        {
            if (Instance()->VsyncInterval.value() < 0 || Instance()->VsyncInterval.value() > 3)
                Instance()->VsyncInterval.reset();
        }
    }

    // Libraries
    {
        ini.SetValue("Libraries", "OptiDllPath",
                     wstring_to_string(Instance()->MainDllPath.value_for_config_or(L"auto")).c_str());

        ini.SetValue("Libraries", "NvngxPath",
                     wstring_to_string(Instance()->NvngxPath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "NvngxFeaturePath",
                     wstring_to_string(Instance()->DLSSFeaturePath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "NvngxDlssPath",
                     wstring_to_string(Instance()->NVNGX_DLSS_Library.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "NvapiPath",
                     wstring_to_string(Instance()->NvapiDllPath.value_for_config_or(L"auto")).c_str());

        ini.SetValue("Libraries", "FfxDx12Path",
                     wstring_to_string(Instance()->FfxDx12Path.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "FfxDx12SRPath",
                     wstring_to_string(Instance()->FfxDx12SRPath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "FfxDx12FGPath",
                     wstring_to_string(Instance()->FfxDx12FGPath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "FfxDx12RRPath",
                     wstring_to_string(Instance()->FfxDx12RRPath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "FfxDx12RCPath",
                     wstring_to_string(Instance()->FfxDx12RCPath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "FfxVkPath",
                     wstring_to_string(Instance()->FfxVkPath.value_for_config_or(L"auto")).c_str());

        ini.SetValue("Libraries", "XeSSPath",
                     wstring_to_string(Instance()->XeSSLibrary.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "XeFGPath",
                     wstring_to_string(Instance()->XeFGLibrary.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "XeLLPath",
                     wstring_to_string(Instance()->XeLLLibrary.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "XeSSDx11Path",
                     wstring_to_string(Instance()->XeSSDx11Library.value_for_config_or(L"auto")).c_str());
    }

    // Old configs, just delete them
    {
        ini.Delete("FSR", "Fsr4ForceEnableInt8");
        ini.Delete("FSR", "Fsr4ForceModel");
        ini.Delete("Nukems", "MakeDepthCopy", true);
    }

    return WriteIni();
}

bool Config::WriteIni()
{
    const auto path = absoluteFileName.wstring();
    const auto fallback = FallbackIniPath(absoluteFileName);

    LOG_INFO("Trying to save ini to: {0}", wstring_to_string(path));

    saveProblem.clear();

    if (ini.SaveFile(path.c_str()) >= 0)
    {
        std::error_code ignored;
        std::filesystem::remove(fallback, ignored);
        return true;
    }

    const auto error = GetLastError();
    const auto attributes = GetFileAttributesW(path.c_str());
    std::string reason = std::format("Windows error {}", error);

    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
        reason = "the file is read-only";
    else if (error == ERROR_ACCESS_DENIED)
        reason = "access denied";
    else if (error == ERROR_SHARING_VIOLATION)
        reason = "another program has it open";

    LOG_ERROR("Can't write {}: {} ({})", wstring_to_string(path), reason, error);

    if (ini.SaveFile(fallback.c_str()) >= 0)
    {
        LOG_WARN("Settings saved to {}", wstring_to_string(fallback.wstring()));
        saveProblem = std::format("Can't write OptiScaler.ini ({}).\nSaved to {} beside it, it loads from there on the "
                                  "next start.",
                                  reason, wstring_to_string(fallback.filename().wstring()));
        return true;
    }

    saveProblem = std::format("Settings NOT saved, can't write OptiScaler.ini ({}).\nRun the game as administrator or "
                              "install it outside Program Files.",
                              reason);
    return false;
}

bool Config::SaveXeFG()
{
    std::lock_guard<std::recursive_mutex> lock(iniMutex);

    ini.SetValue("XeFG", "DepthInverted", GetBoolValue(Instance()->FGXeFGDepthInverted.value_for_config()).c_str());
    ini.SetValue("XeFG", "JitteredMV", GetBoolValue(Instance()->FGXeFGJitteredMV.value_for_config()).c_str());
    ini.SetValue("XeFG", "HighResMV", GetBoolValue(Instance()->FGXeFGHighResMV.value_for_config()).c_str());

    return WriteIni();
}

std::optional<std::string> Config::GetRawValue(const char* section, const char* key)
{
    std::lock_guard<std::recursive_mutex> lock(iniMutex);

    const char* value = ini.GetValue(section, key, nullptr);
    if (value == nullptr)
        return std::nullopt;

    return std::string(value);
}

void Config::SetRawValue(const char* section, const char* key, const char* value)
{
    std::lock_guard<std::recursive_mutex> lock(iniMutex);
    ini.SetValue(section, key, value);
}

void Config::DeleteRawValue(const char* section, const char* key)
{
    std::lock_guard<std::recursive_mutex> lock(iniMutex);
    ini.Delete(section, key, true);
}

bool Config::SaveRawValues()
{
    std::lock_guard<std::recursive_mutex> lock(iniMutex);
    return WriteIni();
}

void Config::CheckUpscalerFiles()
{
    if (!State::Instance().nvngxExists)
        State::Instance().nvngxExists = std::filesystem::exists(Util::ExePath().parent_path() / L"nvngx.dll");

    if (!State::Instance().nvngxExists)
        State::Instance().nvngxExists = std::filesystem::exists(Util::ExePath().parent_path() / L"_nvngx.dll");

    if (!State::Instance().nvngxExists)
    {
        State::Instance().nvngxExists = GetModuleHandle(L"nvngx.dll") != nullptr;

        if (!State::Instance().nvngxExists)
            State::Instance().nvngxExists = GetModuleHandle(L"_nvngx.dll") != nullptr;

        if (State::Instance().nvngxExists)
            LOG_INFO("nvngx.dll found in memory");
        else
            LOG_WARN("nvngx.dll not found!");
    }
    else
    {
        LOG_INFO("nvngx.dll found in game folder");
    }

    if (auto nvngxReplacement = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx_dlss.dll");
        nvngxReplacement.has_value())
    {
        State::Instance().nvngxReplacement = nvngxReplacement.value().wstring();
    }

    State::Instance().libxessExists = std::filesystem::exists(Util::ExePath().parent_path() / L"libxess.dll");
    if (!State::Instance().libxessExists)
    {
        State::Instance().libxessExists = GetModuleHandle(L"libxess.dll") != nullptr;

        if (State::Instance().libxessExists)
            LOG_INFO("libxess.dll found in memory");
        else
            LOG_WARN("libxess.dll not found!");
    }
    else
    {
        LOG_INFO("libxess.dll found in game folder");
    }
}

bool Config::FGCanFollowGame()
{
    // SL1 has no slDLSSGSetOptions so we never learn what the game wants
    const auto& state = State::Instance();
    return state.activeFgInput == FGInput::DLSSG && state.streamlineVersion.major >= 2;
}

bool Config::FGFollowsGame() const { return !FGEnabled.has_value() && FGCanFollowGame(); }

bool Config::IsFGEnabled() const
{
    if (State::Instance().GameDlssgBlocksOwnFG())
        return false;

    // Force DMFG overrides the FG mode
    if (FGDLSSGForceDMFG.value_or_default() && State::Instance().activeFgOutput == FGOutput::DLSSG)
        return true;

    if (FGFollowsGame())
        return State::Instance().dlssgInputGameEnabled;

    return FGEnabled.value_or_default();
}

int Config::FGInterpolationCount(int outputCount) const
{
    if (FGFollowsGame())
        return State::Instance().dlssgInputGameInterpolationCount;

    return outputCount;
}

std::vector<std::string> Config::GetConfigLog() { return _log; }

std::optional<std::string> Config::readString(std::string section, std::string key, bool lowercase)
{
    std::string value = ini.GetValue(section.c_str(), key.c_str(), "auto");

    std::string lower = value;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });

    if (lower == "auto")
        return std::nullopt;

    _log.push_back(std::format("{}.{}: {}", section, key, value));

    return lowercase ? lower : value;
}

std::optional<std::wstring> Config::readWString(std::string section, std::string key, bool lowercase)
{
    std::string value = ini.GetValue(section.c_str(), key.c_str(), "auto");

    std::string lower = value;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });

    if (lower == "auto")
        return std::nullopt;

    _log.push_back(std::format("{}.{}: {}", section, key, value));

    return lowercase ? string_to_wstring(lower) : string_to_wstring(value);
}

std::optional<float> Config::readFloat(std::string section, std::string key)
{
    auto value = readString(section, key);

    try
    {
        float result;

        if (value.has_value() && isFloat(value.value(), result))
            return result;

        return std::nullopt;
    }
    catch (const std::bad_optional_access&) // missing or auto value
    {
        return std::nullopt;
    }
    catch (const std::invalid_argument&) // invalid float string for std::stof
    {
        return std::nullopt;
    }
    catch (const std::out_of_range&) // out of range for 32 bit float
    {
        return std::nullopt;
    }
}

std::optional<int> Config::readShortcut(std::string section, std::string key)
{
    auto value = readString(section, key);
    if (!value.has_value())
        return std::nullopt;

    auto parsed = ShortcutKeys::Parse(*value);
    if (!parsed.has_value())
        LOG_WARN("{}.{}: \"{}\" is not a key or key combination", section, key, *value);

    return parsed;
}

std::optional<int> Config::readInt(std::string section, std::string key)
{
    auto value = readString(section, key);
    if (!value.has_value())
        return std::nullopt;

    const auto& s = *value;
    try
    {
        size_t idx = 0;
        int result;

        // detect hex prefix
        if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X'))
        {
            result = std::stoi(s, &idx, 16);
        }
        else
        {
            result = std::stoi(s, &idx, 10);
        }

        // ensure we consumed the whole string
        if (idx == s.size())
            return result;
        else
            return std::nullopt;
    }
    catch (const std::bad_optional_access&) // missing or auto value
    {
        return std::nullopt;
    }
    catch (const std::invalid_argument&) // invalid float string for std::stof
    {
        return std::nullopt;
    }
    catch (const std::out_of_range&) // out// out of range for 32 bit float
    {
        return std::nullopt;
    }
}

std::optional<uint32_t> Config::readUInt(std::string section, std::string key)
{
    auto value = readString(section, key);
    if (!value.has_value())
        return std::nullopt;

    const auto& s = *value;
    try
    {
        size_t idx = 0;
        int result;

        // detect hex prefix
        if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X'))
        {
            result = std::stoi(s, &idx, 16);
        }
        else
        {
            result = std::stoi(s, &idx, 10);
        }

        // ensure we consumed the whole string
        if (idx == s.size())
            return result;
        else
            return std::nullopt;
    }
    catch (const std::bad_optional_access&) // missing or auto value
    {
        return std::nullopt;
    }
    catch (const std::invalid_argument&) // invalid float string for std::stof
    {
        return std::nullopt;
    }
    catch (const std::out_of_range&) // out// out of range for 32 bit float
    {
        return std::nullopt;
    }
}

std::optional<bool> Config::readBool(std::string section, std::string key)
{
    auto value = readString(section, key, true);
    if (value == "true")
        return true;
    else if (value == "false")
        return false;

    return std::nullopt;
}

// Only use for unsigned enums that have Enum::Count as the last entry
template <typename Enum> std::optional<Enum> Config::readEnum(std::string section, std::string key)
{
    static_assert(std::is_enum_v<Enum>, "Enum type required");

    auto value = readUInt(section, key);
    if (!value.has_value())
        return std::nullopt;

    using Underlying = std::underlying_type_t<Enum>;

    if (*value < static_cast<Underlying>(Enum::Count))
        return static_cast<Enum>(*value);

    return std::nullopt;
}

Config* Config::Instance()
{
    if (!_config)
        _config = new Config();

    return _config;
}
