#include <pch.h>

#include <functional>
#include <vector>

#include "IFeature_Dx12.h"
#include "FeatureProvider_Dx12.h"
#include "State.h"
#include <dlssnr/DlssNr.h>
#include <misc/DeferredRelease.h>

void IFeature_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                                    D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState) const
{
    if (InBeforeState == InAfterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = InResource;
    barrier.Transition.StateBefore = InBeforeState;
    barrier.Transition.StateAfter = InAfterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    InCommandList->ResourceBarrier(1, &barrier);
}

// A render-resolution surface carrying the format of the frame it will become.
//
// Shader_Dx12 has a helper for this and keeps it protected, so the pipeline builds its own. Rebuilt
// when the size or the format moves under it, which a resolution change does.
static bool EnsureIntermediate(ID3D12Device* device, ID3D12Resource* like, unsigned int width, unsigned int height,
                               ID3D12Resource** out)
{
    if (device == nullptr || like == nullptr || out == nullptr || width == 0 || height == 0)
        return false;

    const D3D12_RESOURCE_DESC likeDesc = like->GetDesc();

    if (*out != nullptr)
    {
        const D3D12_RESOURCE_DESC have = (*out)->GetDesc();

        if (have.Width == width && have.Height == height && have.Format == likeDesc.Format)
            return true;

        (*out)->Release();
        *out = nullptr;
    }

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = likeDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    return SUCCEEDED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(out)));
}

// The upscaler that enlarges what the model edited, built once on first use.
//
// Created through the same provider as any other upscaler in this tree, so the choice degrades the way
// every other upscaler choice does -- ask for DLSS on a machine without it and FSR arrives instead.
//
// The provider reads the resolutions from the parameter block, and this feature's own half of the
// split has already lowered them. They are put back exactly as found: the block is the game's.
bool IFeature_Dx12::EnsureEnlarger(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    const auto wanted = Config::Instance()->DlssNrDualEnlarger.value_for_config();

    if (!wanted.has_value())
        return false;

    if (Enlarger != nullptr)
        return EnlargerType == wanted;

    if (EnlargerType.has_value())
        return false; // a build already failed for this choice; do not retry every frame

    EnlargerType = wanted;

    unsigned int width = 0, height = 0, outWidth = 0, outHeight = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Width, &width);
    InParameters->Get(NVSDK_NGX_Parameter_Height, &height);
    InParameters->Get(NVSDK_NGX_Parameter_OutWidth, &outWidth);
    InParameters->Get(NVSDK_NGX_Parameter_OutHeight, &outHeight);

    InParameters->Set(NVSDK_NGX_Parameter_Width, RenderWidth());
    InParameters->Set(NVSDK_NGX_Parameter_Height, RenderHeight());
    InParameters->Set(NVSDK_NGX_Parameter_OutWidth, DisplayWidth());
    InParameters->Set(NVSDK_NGX_Parameter_OutHeight, DisplayHeight());

    std::unique_ptr<IFeature_Dx12> built = nullptr;
    bool ok = FeatureProvider_Dx12::GetFeature(wanted.value(), IFeature::GetNextHandleId(), InParameters, &built) &&
              built != nullptr;

    if (ok)
    {
        built->MarkEnlargementStage();
        ok = built->Init(Device, InCommandList, InParameters);
    }

    InParameters->Set(NVSDK_NGX_Parameter_Width, width);
    InParameters->Set(NVSDK_NGX_Parameter_Height, height);
    InParameters->Set(NVSDK_NGX_Parameter_OutWidth, outWidth);
    InParameters->Set(NVSDK_NGX_Parameter_OutHeight, outHeight);

    if (!ok)
    {
        LOG_ERROR("DLSS-NR dual feature: {} would not build for the enlargement, falling back to the output scaler",
                  UpscalerDisplayName(wanted.value()));
        return false;
    }

    Enlarger = std::move(built);

    LOG_INFO("DLSS-NR dual feature: {} enlarges {}x{} to {}x{} after the model", Enlarger->Name(), RenderWidth(),
             RenderHeight(), DisplayWidth(), DisplayHeight());

    return true;
}

bool IFeature_Dx12::Init(ID3D12Device* InDevice, ID3D12GraphicsCommandList* InCommandList,
                         NVSDK_NGX_Parameter* InParameters)
{
    Device = InDevice;

    auto result = InitInternal(InCommandList, InParameters);

    if (result)
    {
        if (!Config::Instance()->OverlayMenu.value_or_default() && (Imgui == nullptr || Imgui.get() == nullptr))
            Imgui = std::make_unique<Menu_Dx12>(Util::GetProcessWindow(), InDevice);

        OutputScaler = std::make_unique<OS_Dx12>("Output Scaling", InDevice, (TargetWidth() < DisplayWidth()));
        RCAS = std::make_unique<RCAS_Dx12>("RCAS", InDevice);
        // DLSS/DLSSD take the bias mask as is
        if (GetUpscalerType() != Upscaler::DLSS && GetUpscalerType() != Upscaler::DLSSD)
            Bias = std::make_unique<Bias_Dx12>("Bias", InDevice);
        Magnifier = std::make_unique<Magnifier_Dx12>("Magnifier", InDevice);

        UpscalerTime = std::make_unique<GpuTime_Dx12>(InDevice);
    }

    return result;
}

bool IFeature_Dx12::Evaluate(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    // Frees deferred objects even when presents aren't hooked
    DeferredRelease::Collect();

    if (!IsInited())
    {
        LOG_ERROR("Not inited!");
        return false;
    }

    if (Config::Instance()->OverrideSharpness.value_or_default())
        _sharpness = Config::Instance()->Sharpness.value_or_default();
    else
        _sharpness = GetSharpness(InParameters);

    if (_sharpness > 1.0f)
        _sharpness = 1.0f;

    // Those upcalers don't have their own sharpness so always need to use RCAS when sharpness is set
    auto upscaler = GetUpscalerType();
    bool useRcas = upscaler == Upscaler::XeSS ||
                   (upscaler == Upscaler::DLSS && Version() >= feature_version(2, 5, 1)) || upscaler == Upscaler::DLSSD;

    if (!useRcas)
        useRcas = Config::Instance()->RcasEnabled.value_or_default();

    if (_sharpness == 0.0f)
        useRcas = false;

    // Need RCAS for MAS
    if (!useRcas && (Config::Instance()->MotionSharpnessEnabled.value_or_default() &&
                     Config::Instance()->MotionSharpness.value_or_default() > 0.0f))
    {
        useRcas = true;
    }

    if (!RCAS->IsInit())
        useRcas = false;

    // The model between the halves of the upscaler. SetInitParameters has already pointed the upscaler
    // at render resolution, so the enlargement is not optional here -- without it the frame reaching
    // the game would be the small one.
    const bool useDualFeature = DualFeatureSplit();

    // Asked for and not taken. The split is decided from three numbers settled when the feature was
    // built, so a mismatch here is silent and looks exactly like the option doing nothing.
    if (!useDualFeature && !_isEnlargementStage && Config::Instance()->DlssNrDualFeature.value_or_default() &&
        Config::Instance()->DlssNrEnabled.value_or_default())
    {
        static unsigned int saidTarget = 0;

        if (saidTarget != TargetWidth())
        {
            saidTarget = TargetWidth();
            LOG_WARN("DLSS-NR dual feature: asked for, not taken -- target {}x{}, render {}x{}, display {}x{}",
                     TargetWidth(), TargetHeight(), RenderWidth(), RenderHeight(), DisplayWidth(), DisplayHeight());
        }
    }

    // An upscaler does the enlarging when one is asked for and builds. Otherwise the spatial scaler,
    // which needs nothing the first half has already consumed and so cannot be wrong about it.
    const bool useUpscalerEnlarger = useDualFeature && EnsureEnlarger(InCommandList, InParameters);

    bool useOutputScaling =
        (useDualFeature && !useUpscalerEnlarger) || (Config::Instance()->OutputScalingEnabled.value_or_default() &&
                                                     (LowResMV() || RenderWidth() == DisplayWidth()));

    if (!OutputScaler->IsInit())
        useOutputScaling = false;

    ID3D12Resource* paramOutput = nullptr;
    ID3D12Resource* paramMotion = nullptr;
    ID3D12Resource* paramDepth = nullptr;

    InParameters->Get(NVSDK_NGX_Parameter_Output, &paramOutput);
    InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &paramMotion);
    InParameters->Get(NVSDK_NGX_Parameter_Depth, &paramDepth);

    // Order is important as that's the order of shader dispatch
    std::vector<ShaderPass> pipeline;

    // First, so it runs on what the upscaler wrote and before anything enlarges it. The model asks for
    // a 1:1 scaling ratio at every quality level, so the only way to run it on fewer pixels is to give
    // it a smaller frame -- which is what the upscaler writing at render resolution produces.
    if (useDualFeature)
    {
        pipeline.push_back({ // Setup
                             [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
                             { return DlssNr::StageInputSurface(InCommandList, nextOutput); },

                             // Dispatch
                             [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
                             {
                                 if (DlssNr::EvaluateStage(InCommandList, InParameters, input, output))
                                     return true;

                                 // A pass that declines leaves the frame where it is, so the enlargement still has
                                 // something to read. The upscaler's own result, unedited, is the right fallback.
                                 ResourceBarrier(InCommandList, input, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
                                 ResourceBarrier(InCommandList, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_COPY_DEST);

                                 InCommandList->CopyResource(output, input);

                                 ResourceBarrier(InCommandList, input, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                                 ResourceBarrier(InCommandList, output, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                                 return true;
                             } });
    }

    if (useUpscalerEnlarger)
    {
        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  // Render resolution, matching what the first half wrote rather than what this stage
                  // produces -- nextOutput is the game's frame and is display sized.
                  if (!EnsureIntermediate(Device, nextOutput, RenderWidth(), RenderHeight(), &EnlargerInput))
                      return nullptr;

                  return EnlargerInput;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  // The game's own block, borrowed. Everything the enlarging upscaler needs per frame --
                  // motion vectors, depth, jitter, the reset -- is the game's and already in it; only
                  // the two frames differ from what the game described.
                  ID3D12Resource* gameColor = nullptr;
                  InParameters->Get(NVSDK_NGX_Parameter_Color, &gameColor);

                  InParameters->Set(NVSDK_NGX_Parameter_Color, input);
                  InParameters->Set(NVSDK_NGX_Parameter_Output, output);

                  const bool ok = Enlarger->Evaluate(InCommandList, InParameters);

                  InParameters->Set(NVSDK_NGX_Parameter_Color, gameColor);

                  // Dropped rather than retried: EnlargerType stays set, so EnsureEnlarger declines from
                  // here on and the next frame is built around the spatial scaler instead.
                  if (!ok)
                  {
                      LOG_WARN("DLSS-NR dual feature: the enlargement failed, dropping back to the output scaler");
                      Enlarger.reset();
                  }

                  return ok;
              } });
    }

    if (useOutputScaling)
    {
        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  if (OutputScaler->CreateBufferResource(Device, nextOutput, TargetWidth(), TargetHeight(),
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                  {
                      OutputScaler->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                      return OutputScaler->Buffer();
                  }
                  return nullptr;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  LOG_DEBUG("Scaling output...");
                  OutputScaler->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                  if (!OutputScaler->Dispatch(InCommandList, input, output))
                  {
                      Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
                      State::Instance().changeBackend[Handle()->Id] = true;
                      return false;
                  }
                  return true;
              } });
    }

    _actualSharpness = _sharpness;
    if (useRcas)
    {
        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  // Disable any built-in sharpness shaders
                  InParameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
                  _sharpness = 0.0f;

                  if (RCAS->CreateBufferResource(Device, nextOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                  {
                      RCAS->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                      return RCAS->Buffer();
                  }
                  return nullptr;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  if (!RCAS->CanRender() || !paramMotion || !paramOutput)
                      return true;

                  RCAS->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                  RcasConstants rcasConstants {};

                  rcasConstants.Sharpness = _actualSharpness.value_or(_sharpness);
                  rcasConstants.DepthIsLinear = DepthLinear();
                  rcasConstants.DepthIsReversed = DepthInverted();
                  rcasConstants.IsHdr = IsHdr();

                  // Restore value
                  _sharpness = _actualSharpness.value_or(_sharpness);
                  _actualSharpness.reset();

                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &rcasConstants.MvScaleX);
                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &rcasConstants.MvScaleY);

                  float nearPlane = 0.0f;
                  float farPlane = 0.0f;

                  // We need camera near and far for DLSSD
                  // We passthrough those values from the DLSSG params onto the upscaler's params
                  if (InParameters->Get("DLSSG.CameraNear", &nearPlane) == NVSDK_NGX_Result_Success &&
                      InParameters->Get("DLSSG.CameraFar", &farPlane) == NVSDK_NGX_Result_Success)
                  {
                      rcasConstants.CameraNear = nearPlane;
                      rcasConstants.CameraFar = farPlane;
                  }
                  else
                  {
                      rcasConstants.CameraNear = Config::Instance()->FsrCameraNear.value_or_default();
                      rcasConstants.CameraFar = Config::Instance()->FsrCameraFar.value_or_default();
                  }

                  if (!RCAS->Dispatch(InCommandList, input, paramMotion, rcasConstants, output, paramDepth))
                  {
                      Config::Instance()->RcasEnabled.set_volatile_value(false);
                      return false;
                  }
                  return true;
              } });
    }

    if (Magnifier->ShouldRun())
    {
        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  if (Magnifier->CreateBufferResource(Device, nextOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                  {
                      Magnifier->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                      return Magnifier->Buffer();
                  }

                  return nullptr;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  if (!Magnifier->CanRender() || !paramMotion || !paramOutput)
                      return true;

                  Magnifier->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                  return Magnifier->Dispatch(InCommandList, input, output);
              } });
    }

    // Iterate BACKWARDS to establish where each shader needs to pull its input from
    ID3D12Resource* currentTarget = paramOutput;
    for (auto it = pipeline.rbegin(); it != pipeline.rend(); ++it)
    {
        ID3D12Resource* requiredInput = it->Setup(currentTarget);
        if (requiredInput)
        {
            it->outputBuffer = currentTarget;
            it->inputBuffer = requiredInput;
            currentTarget = requiredInput; // Shift the target back for the next previous stage
        }
    }

    // Upscaler will write to the first active shader, or just output
    InParameters->Set(NVSDK_NGX_Parameter_Output, currentTarget);

    UpscalerTime->Start(InCommandList);

    auto evalResult = EvaluateInternal(InCommandList, InParameters);

    UpscalerTime->End(InCommandList);

    if (!evalResult)
    {
        // Output still points at the first stage's buffer, which is this pipeline's and is render
        // sized. Leaving it there hands the game's next reader a surface it does not own; every other
        // exit from here restores it.
        InParameters->Set(NVSDK_NGX_Parameter_Output, paramOutput);

        static bool said = false;

        if (!said)
        {
            said = true;
            LOG_ERROR("Upscaler evaluate failed; {} pipeline stage(s) skipped this frame", pipeline.size());
        }

        return false;
    }

    // Iterate FORWARDS to execute the shaders in the defined order
    for (auto& pass : pipeline)
    {
        if (pass.inputBuffer && pass.outputBuffer)
        {
            if (!pass.Dispatch(pass.inputBuffer, pass.outputBuffer))
            {
                return true;
            }
        }
    }

    // imgui
    if (!Config::Instance()->OverlayMenu.value_or_default() && _frameCount > 30)
    {
        if (Imgui != nullptr && Imgui.get() != nullptr)
        {
            if (Imgui->IsHandleDifferent())
            {
                Imgui.reset();
            }
            else
                Imgui->Render(InCommandList, paramOutput);
        }
        else
        {
            if (Imgui == nullptr || Imgui.get() == nullptr)
                Imgui = std::make_unique<Menu_Dx12>(GetForegroundWindow(), Device);
        }
    }

    InParameters->Set(NVSDK_NGX_Parameter_Output, paramOutput);

    return evalResult;
}

std::optional<double> IFeature_Dx12::ReadUpscalerTime(void* commandQueueVoid)
{
    ID3D12CommandQueue* commandQueue = (ID3D12CommandQueue*) commandQueueVoid;

    lastUpscalerTime = UpscalerTime->ReadGpuTime(commandQueue);
    lastRcasTime = RCAS->ReadGpuTime(commandQueue);
    lastOutputScalingTime = OutputScaler->ReadGpuTime(commandQueue);

    return sumOpts(lastUpscalerTime, lastRcasTime, lastOutputScalingTime);
}

void IFeature_Dx12::ReadDetailedGpuTimes(void* commandQueueVoid, std::vector<DetailedGpuTime>& detailedGpuTimes)
{
    ID3D12CommandQueue* commandQueue = (ID3D12CommandQueue*) commandQueueVoid;

    detailedGpuTimes.clear();

    // Do not call ReadGpuTime twice for shaders
    if (lastUpscalerTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { ShortName(), lastUpscalerTime.value(), true });

    if (lastRcasTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { RCAS->Name(), lastRcasTime.value(), true });

    if (lastOutputScalingTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { OutputScaler->Name(), lastOutputScalingTime.value(), true });

    auto magnifierTime = Magnifier->ReadGpuTime(commandQueue);

    if (magnifierTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { Magnifier->Name(), magnifierTime.value(), false });
}

IFeature_Dx12::IFeature_Dx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters) {}

IFeature_Dx12::~IFeature_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    Imgui.reset();
    OutputScaler.reset();
    RCAS.reset();
    Bias.reset();
    Enlarger.reset();

    if (EnlargerInput != nullptr)
    {
        EnlargerInput->Release();
        EnlargerInput = nullptr;
    }
}
