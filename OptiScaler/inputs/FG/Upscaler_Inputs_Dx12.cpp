#include "pch.h"
#include "Upscaler_Inputs_Dx12.h"
#include "Upscaler_Inputs.h"
#include <hudfix/Hudfix_Dx12.h>
#include <resource_tracking/ResTrack_dx12.h>
#include "shaders/depth_scale/DS_Dx12.h"
#include "MathUtils.h"
#include <hooks/Streamline_Hooks.h>

using namespace OptiMath;
static DS_Dx12* DepthScale = nullptr;

// The game's camera when it also feeds Streamline; the upscaler call carries none
static bool GetStreamlineCamera(sl::Constants& out)
{
    if (!StreamlineHooks::isSetConstantsHooked())
        return false;

    const auto snapshot = StreamlineHooks::getSLConstantsSnapshot();
    out = snapshot.constants;

    return snapshot.frameIndex != UINT32_MAX && out.cameraNear != sl::INVALID_FLOAT &&
           out.cameraFar != sl::INVALID_FLOAT && out.cameraFOV != sl::INVALID_FLOAT && out.cameraNear > 0.0f &&
           out.cameraFar >= 0.0f && out.cameraFOV > 0.0f && std::isfinite(out.cameraNear + out.cameraFar + out.cameraFOV);
}

void UpscalerInputsDx12::Init(ID3D12Device* device)
{
    if (State::Instance().activeFgInput != FGInput::Upscaler)
        return;

    _device = device;
}

void UpscalerInputsDx12::Reset()
{
    if (State::Instance().currentFG == nullptr || State::Instance().activeFgInput != FGInput::Upscaler ||
        _device == nullptr)
        return;

    if (Config::Instance()->FGHUDFix.value_or_default())
        Hudfix_Dx12::ResetCounters();
}

void UpscalerInputsDx12::UpscaleStart(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Parameter* InParameters,
                                      IFeature_Dx12* feature)
{
    Hudfix_Dx12::SetSkipStatus(true);

    float mvScaleX = 0.0f;
    float mvScaleY = 0.0f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;

    // FSR Camera values
    auto camera = UpscalerInputs::GetCameraValues(*InParameters, feature);

    auto fg = State::Instance().currentFG;

    if (fg == nullptr || State::Instance().activeFgInput != FGInput::Upscaler || _device == nullptr)
        return;

    auto fgConstants = UpscalerInputs::GetFGConstants(feature);

    // FG takes device depth. Linear view depth (OpenRR, FSR-RR) goes to it as the reversed-Z depth of the game's
    // camera, with the camera and flags the Streamline input would give; without a Streamline camera, of the
    // default planes. Device depth is untouched.
    const bool scaleDepth = Config::Instance()->FGEnableDepthScale.value_or_default();
    bool linearDepth = !scaleDepth && feature->FGLinearDepth();
    sl::Constants slCamera {};
    const bool hasSlCamera = linearDepth && GetStreamlineCamera(slCamera);
    bool infiniteDepth = false;

    if (hasSlCamera)
    {
        camera.cameraNear = std::min(slCamera.cameraNear, slCamera.cameraFar);
        camera.cameraFar = std::max(slCamera.cameraNear, slCamera.cameraFar);
        camera.cameraVFov =
            slCamera.cameraFOV < 4.0f ? slCamera.cameraFOV : GetRadiansFromDeg(slCamera.cameraFOV);

        // Zero far is infinite; FSR-FG needs far above near even then, as on the Streamline input
        infiniteDepth = camera.cameraNear == 0.0f;
        if (infiniteDepth)
        {
            camera.cameraNear = camera.cameraFar;
            camera.cameraFar = camera.cameraNear + 1.0f;
        }
    }
    else if (linearDepth && camera.cameraNear < camera.cameraFar)
    {
        // GetCameraValues' order for reversed Z
        std::swap(camera.cameraNear, camera.cameraFar);
    }

    const float zNear = std::min(camera.cameraNear, camera.cameraFar);
    const float zFar = std::max(camera.cameraNear, camera.cameraFar);
    linearDepth = linearDepth && zNear > 0.0f && std::isfinite(zFar) && zFar > zNear;

    if (linearDepth)
    {
        fgConstants.flags |= FG_Flags::InvertedDepth;

        if (infiniteDepth)
            fgConstants.flags |= FG_Flags::InfiniteDepth;
    }

    // XeFG reads it when its swapchain is made, so a change shows the restart note in the menu. Set by
    // IFeature::SetInitParameters, this only differs when the per-frame depth type disagrees with the one at creation.
    if (const bool inverted = feature->DepthInverted() || linearDepth;
        Config::Instance()->FGXeFGDepthInverted.value_or_default() != inverted)
    {
        Config::Instance()->FGXeFGDepthInverted = inverted;
        Config::Instance()->SaveXeFG();
    }

    if (static bool loggedLinear = false; linearDepth != loggedLinear)
    {
        loggedLinear = linearDepth;
        if (linearDepth)
            LOG_INFO("FG: the upscaler's depth is linear, FG gets reversed-Z device depth (near {}, far {}, "
                     "vertical FOV {}, camera from {})",
                     zNear, infiniteDepth ? "infinite" : std::to_string(zFar), camera.cameraVFov,
                     hasSlCamera ? "Streamline" : "the defaults");
        else
            LOG_INFO("FG: the upscaler's depth is device depth again");
    }

    fg->EvaluateState(_device, fgConstants);

    // Not needed while FG or the option is off, its destructor defers the buffer release
    if (DepthScale != nullptr && (!Config::Instance()->IsFGEnabled() || (!scaleDepth && !linearDepth)))
    {
        delete DepthScale;
        DepthScale = nullptr;
    }

    int reset = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &reset);

    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);

    fg->StartNewFrame();

    auto aspectRatio = (float) feature->DisplayWidth() / (float) feature->DisplayHeight();
    fg->SetCameraValues(camera.cameraNear, camera.cameraFar, camera.cameraVFov, aspectRatio, camera.meterFactor);

    if (hasSlCamera)
        fg->SetCameraData(reinterpret_cast<float*>(&slCamera.cameraPos), reinterpret_cast<float*>(&slCamera.cameraUp),
                          reinterpret_cast<float*>(&slCamera.cameraRight),
                          reinterpret_cast<float*>(&slCamera.cameraFwd));
    fg->SetFrameTimeDelta(State::Instance().lastFGFrameTime);
    fg->SetMVScale(mvScaleX, mvScaleY);
    fg->SetJitter(jitterX, jitterY);
    fg->SetReset(reset);
    fg->SetInterpolationRect(feature->DisplayWidth(), feature->DisplayHeight());

    Hudfix_Dx12::UpscaleStart();

    // FG Prepare
    UINT frameIndex;
    if (!State::Instance().isShuttingDown && fg->IsActive() && Config::Instance()->IsFGEnabled() &&
        State::Instance().currentSwapchain != nullptr)
    {
        // Wait for present
        if (fg->Mutex.getOwner() == 2)
        {
            LOG_TRACE("Waiting for present!");
            fg->Mutex.lock(4);
            fg->Mutex.unlockThis(4);
        }

        bool allocatorReset = false;
        frameIndex = fg->GetIndex();

        ID3D12GraphicsCommandList* commandList = nullptr;
        commandList = InCmdList;

        LOG_DEBUG("(FG) copy buffers for fgUpscaledImage[{}], frame: {}", frameIndex, fg->FrameCount());

        ID3D12Resource* paramVelocity = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &paramVelocity) != NVSDK_NGX_Result_Success)
            InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &paramVelocity);

        if (paramVelocity != nullptr)
        {
            Dx12Resource setResource {};
            setResource.type = FG_ResourceType::Velocity;
            setResource.cmdList = commandList;
            setResource.resource = paramVelocity;
            setResource.state = (D3D12_RESOURCE_STATES) Config::Instance()->MVResourceBarrier.value_or(
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            setResource.validity = FG_ResourceValidity::ValidNow;

            if (feature->LowResMV())
            {
                setResource.width = feature->RenderWidth();
                setResource.height = feature->RenderHeight();
            }
            else
            {
                setResource.width = feature->TargetWidth();
                setResource.height = feature->TargetHeight();
            }

            fg->SetResource(&setResource);
        }

        ID3D12Resource* paramDepth = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_Depth, &paramDepth) != NVSDK_NGX_Result_Success)
            InParameters->Get(NVSDK_NGX_Parameter_Depth, (void**) &paramDepth);

        if (paramDepth != nullptr)
        {
            auto done = false;

            if (scaleDepth || linearDepth)
            {
                if (DepthScale == nullptr)
                    DepthScale = new DS_Dx12("Depth Scale", _device);

                if (DepthScale->CreateBufferResource(_device, paramDepth, 0, 0,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                    DepthScale->Buffer() != nullptr)
                {
                    DepthScale->SetBufferState(InCmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                    const uint32_t mode = linearDepth ? (fg->IsInvertedDepth() ? 2 : 1) : 0;

                    if (DepthScale->Dispatch(InCmdList, paramDepth, DepthScale->Buffer(), mode, zNear,
                                             infiniteDepth ? 0.0f : zFar))
                    {
                        Dx12Resource setResource {};
                        setResource.type = FG_ResourceType::Depth;
                        setResource.cmdList = commandList;
                        setResource.resource = DepthScale->Buffer();
                        setResource.width = feature->RenderWidth();
                        setResource.height = feature->RenderHeight();
                        setResource.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        setResource.validity = FG_ResourceValidity::UntilPresent;

                        fg->SetResource(&setResource);

                        done = true;
                    }
                }
            }

            if (!done)
            {
                Dx12Resource setResource {};
                setResource.type = FG_ResourceType::Depth;
                setResource.cmdList = commandList;
                setResource.resource = paramDepth;
                setResource.width = feature->RenderWidth();
                setResource.height = feature->RenderHeight();
                setResource.state = (D3D12_RESOURCE_STATES) Config::Instance()->DepthResourceBarrier.value_or(
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                setResource.validity = FG_ResourceValidity::ValidNow;

                fg->SetResource(&setResource);
            }
        }

        LOG_DEBUG("(FG) copy buffers done, frame: {0}", fg->FrameCount());
    }
}

void UpscalerInputsDx12::UpscaleEnd(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Parameter* InParameters,
                                    IFeature_Dx12* feature)
{
    Hudfix_Dx12::SetSkipStatus(false);

    auto fg = State::Instance().currentFG;

    if (fg == nullptr || State::Instance().activeFgInput != FGInput::Upscaler || _device == nullptr)
        return;

    // FG Dispatch
    if (fg->IsActive() && Config::Instance()->IsFGEnabled() && State::Instance().currentSwapchain != nullptr)
    {
        if (Config::Instance()->FGHUDFix.value_or_default())
        {
            // For signal after mv & depth copies
            Hudfix_Dx12::UpscaleEnd(feature->FrameCount(), State::Instance().lastFGFrameTime);

            ID3D12Resource* output = nullptr;
            if (InParameters->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success)
                InParameters->Get(NVSDK_NGX_Parameter_Output, (void**) &output);

            if (output == nullptr)
                return;

            ResourceInfo info {};
            auto desc = output->GetDesc();
            info.buffer = output;
            info.width = desc.Width;
            info.height = desc.Height;
            info.format = desc.Format;
            info.flags = desc.Flags;
            info.type = UAV;
            info.captureInfo = CaptureInfo::UpscalerCapture;

            Hudfix_Dx12::CheckForHudless(InCmdList, &info,
                                         (D3D12_RESOURCE_STATES) Config::Instance()->OutputResourceBarrier.value_or(
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                         true);
        }
        else
        {
            LOG_DEBUG("(FG) running, frame: {0}", feature->FrameCount());
        }
    }
}
