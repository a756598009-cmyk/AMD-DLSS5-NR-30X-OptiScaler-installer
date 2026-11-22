#include "pch.h"
#include "FeatureProvider_Dx12.h"

#include "Util.h"
#include "Config.h"
#include "dlssnr/amd/AmdBridge.h"

#include "NVNGX_Parameter.h"

#include "upscalers/dlss/DLSSFeature_Dx12.h"
#include "upscalers/dlssd/DLSSDFeature_Dx12.h"
#include "upscalers/fsr2/FSR2Feature_Dx12.h"
#include "upscalers/fsr2_212/FSR2Feature_Dx12_212.h"
#include "upscalers/ffx/FFXFeature_Dx12.h"
#include "upscalers/fsr31/FSRDFeature_Dx12.h"
#include "upscalers/opennr_rr/OpenNrRrFeature_Dx12.h"
#include "upscalers/xess/XeSSFeature_Dx12.h"
#include "FeatureProvider_Dx11.h"
#include <proxies/FfxApi_Proxy.h>
#include <misc/IdentifyGpu.h>

bool FeatureProvider_Dx12::GetFeature(Upscaler upscaler, UINT handleId, NVSDK_NGX_Parameter* parameters,
                                      std::unique_ptr<IFeature_Dx12>* feature)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();
    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    // AmdPreSr NeuralHub host: DLSS-SR/DLSS-RR become providers on an AMD GPU once the host
    // module's present publish and gate arm succeeded; NVIDIA keeps its own dlssCapable path.
    const bool dlssCapableForThisProcess = primaryGpu.dlssCapable || state.NeuralHubArmOk;
    ScopedSkipHeapCapture skipHeapCapture {};

    switch (upscaler)
    {
    case Upscaler::XeSS:
        *feature = std::make_unique<XeSSFeatureDx12>(handleId, parameters);
        break;

    case Upscaler::FSR21:
        *feature = std::make_unique<FSR2FeatureDx12_212>(handleId, parameters);
        break;

    case Upscaler::FSR22:
        *feature = std::make_unique<FSR2FeatureDx12>(handleId, parameters);
        break;

    case Upscaler::FFX:
        *feature = std::make_unique<FFXFeatureDx12>(handleId, parameters);
        break;

    case Upscaler::DLSS:
        if (dlssCapableForThisProcess && state.NVNGX_DLSS_Path.has_value())
        {
            *feature = std::make_unique<DLSSFeatureDx12>(handleId, parameters);
            break;
        }
        else
        {
            *feature = std::make_unique<FSR2FeatureDx12_212>(handleId, parameters);
            upscaler = Upscaler::FSR21;
            break;
        }

    case Upscaler::DLSSD:
        if (dlssCapableForThisProcess && state.NVNGX_DLSSD_Path.has_value())
        {
            *feature = std::make_unique<DLSSDFeatureDx12>(handleId, parameters);
            break;
        }
        else
        {
            *feature = std::make_unique<FSR2FeatureDx12_212>(handleId, parameters);
            upscaler = Upscaler::FSR21;
            break;
        }

    case Upscaler::OpenNrRr:
        *feature = std::make_unique<OpenNrRrFeatureDx12>(handleId, parameters);

        if ((*feature)->ModuleLoaded())
            break;

        upscaler = Upscaler::FSR_RR;
        [[fallthrough]];

    case Upscaler::FSR_RR:
        if (FfxApiProxy::IsDenoiserApiImplementedDx12())
        {
            *feature = std::make_unique<FSRDFeatureDx12>(handleId, parameters);
            break;
        }
        else
        {
            // FSR-RR has no upscaler fallback: a Ray Reconstruction request without
            // the denoiser API is a failure, not a quality downgrade. UpscalerOnly
            // opts into creating the feature anyway with the denoiser skipped.
            LOG_ERROR("FfxApi denoiser API is not implemented for Dx12");
            return false;
        }

    default:
        *feature = std::make_unique<FSR2FeatureDx12_212>(handleId, parameters);
        upscaler = Upscaler::FSR21;
        break;
    }

    bool loaded = (*feature)->ModuleLoaded();

    if (!loaded)
    {
        // Fail after the constructor
        ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "Falling back to FSR 2.1.2" });
        *feature = std::make_unique<FSR2FeatureDx12_212>(handleId, parameters);
        upscaler = Upscaler::FSR21;
        loaded = true; // Assuming the fallback always loads successfully
    }

    // DLSSD is stored in the config as DLSS
    if (upscaler == Upscaler::DLSSD)
        upscaler = Upscaler::DLSS;

    cfg.Dx12Upscaler = upscaler;

    return loaded;
}

// The old feature's sizes, flags and quality, written into the block the new feature is created from.
static void ApplyCreateSize(ContextData<IFeature_Dx12>* contextData)
{
    auto* params = contextData->createParams;
    params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, contextData->createFlags);
    params->Set(NVSDK_NGX_Parameter_Width, contextData->createWidth);
    params->Set(NVSDK_NGX_Parameter_Height, contextData->createHeight);
    params->Set(NVSDK_NGX_Parameter_OutWidth, contextData->createOutWidth);
    params->Set(NVSDK_NGX_Parameter_OutHeight, contextData->createOutHeight);
    params->Set(NVSDK_NGX_Parameter_PerfQualityValue, contextData->createQuality);
    if (contextData->featureID == NVSDK_NGX_Feature_RayReconstruction)
        DLSSDFeature::SyncCreateParams(params);
}

bool FeatureProvider_Dx12::ChangeFeature(Upscaler upscaler, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                                         UINT handleId, NVSDK_NGX_Parameter* parameters,
                                         ContextData<IFeature_Dx12>* contextData)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();

    if (!state.changeBackend[handleId])
        return false;

    // danielblnc 0.8.0's host left out beside another NR runtime: DLSS picked in the menu loads it now.
    if ((state.newBackend == Upscaler::DLSS || state.newBackend == Upscaler::DLSSD) &&
        DlssNr::AmdBridge::NgxHostDeferred())
        DlssNr::AmdBridge::EnsureNgxHost(device, nullptr, true);

    const bool dlssOnNonCapable = !IdentifyGpu::getPrimaryGpu().dlssCapable && !state.NeuralHubArmOk
                                  && (state.newBackend == Upscaler::DLSS || state.newBackend == Upscaler::DLSSD);
    const bool recreate = state.newBackend == Upscaler::Reset;
    if (recreate || dlssOnNonCapable)
        state.newBackend = cfg.Dx12Upscaler.value_or_default();

    if (contextData->featureID == NVSDK_NGX_Feature_RayReconstruction)
    {
        const auto primaryGpu = IdentifyGpu::getPrimaryGpu();
        const bool dlssdCapable =
            (primaryGpu.dlssCapable || state.NeuralHubArmOk) && state.NVNGX_DLSSD_Path.has_value();
        const bool fsrRrCapable =
            primaryGpu.vendorId != VendorId::Nvidia && FfxApiProxy::IsDenoiserApiImplementedDx12();
        const bool openNrRrCapable =
            state.newBackend == Upscaler::OpenNrRr && device != nullptr && OpenNrRr::Usable(device->GetAdapterLuid());

        if ((state.newBackend == Upscaler::DLSS || state.newBackend == Upscaler::DLSSD) && dlssdCapable)
        {
            state.newBackend = Upscaler::DLSSD;
            contextData->featureKey = Upscaler::DLSSD;
        }
        else if ((state.newBackend == Upscaler::FFX || state.newBackend == Upscaler::FSR_RR) && fsrRrCapable)
        {
            state.newBackend = Upscaler::FSR_RR;
            contextData->featureKey = Upscaler::FSR_RR;
        }
        else if (openNrRrCapable)
        {
            contextData->featureKey = Upscaler::OpenNrRr;
        }
        else if (contextData->featureKey == Upscaler::OpenNrRr)
        {
            // Leaving OpenRR, by choice or after a failure: the provider Ray Reconstruction had before it.
            state.newBackend = fsrRrCapable ? Upscaler::FSR_RR : dlssdCapable ? Upscaler::DLSSD : Upscaler::FSR21;
            contextData->featureKey = state.newBackend;
        }
        else if (state.newBackend != contextData->featureKey)
        {
            state.newBackend = contextData->featureKey;
        }
    }
    else if (state.newBackend == Upscaler::OpenNrRr)
    {
        // OpenRR is for Ray Reconstruction only. A Super Resolution feature recreated while the config names it
        // runs FFX; one asked to switch to it keeps what it runs.
        if (!recreate)
        {
            state.changeBackend[handleId] = false;
            return false;
        }

        state.newBackend = Upscaler::FFX;
    }

    contextData->changeBackendCounter++;

    LOG_INFO("changeBackend is true, counter: {0}", contextData->changeBackendCounter);

    // first release everything
    if (contextData->changeBackendCounter == 1)
    {
        if (state.currentFG != nullptr && state.activeFgInput == FGInput::Upscaler)
        {
            state.fgChanged = true;
            state.clearCapturedHudlesses = true;
        }

        if (contextData->feature != nullptr)
        {
            LOG_INFO("changing backend to {}", UpscalerDisplayName(state.newBackend));

            auto* dc = contextData->feature.get();
            // Use given params if using DLSS passthrough
            const bool isPassthrough = state.newBackend == Upscaler::DLSSD || state.newBackend == Upscaler::DLSS;

            contextData->createParams = isPassthrough ? parameters : GetNGXParameters(API::DX12, false);
            contextData->createParamsOwned = !isPassthrough;
            contextData->createFlags = dc->GetFeatureFlags();
            contextData->createWidth = dc->RenderWidth();
            contextData->createHeight = dc->RenderHeight();
            contextData->createOutWidth = dc->DisplayWidth();
            contextData->createOutHeight = dc->DisplayHeight();
            contextData->createQuality = dc->PerfQualityValue();
            ApplyCreateSize(contextData);

            dc = nullptr;

            State::Instance().currentFeature = nullptr;

            Util::DelayedDestroy(std::move(contextData->feature));

            // LOG_DEBUG("sleeping before reset of current feature for 1000ms");
            // std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            // contextData->feature.reset();
            // contextData->feature = nullptr;
        }
        else // Clean up state if no feature is set
        {
            LOG_ERROR("can't find handle {0} in Dx12Contexts!", handleId);

            state.newBackend = Upscaler::Reset;
            state.changeBackend[handleId] = false;

            if (contextData->createParamsOwned && contextData->createParams != nullptr)
            {
                TryDestroyNGXParameters(contextData->createParams, NVNGXProxy::D3D12_DestroyParameters());
                contextData->createParams = nullptr;
                contextData->createParamsOwned = false;
            }
            else
            {
                contextData->createParams = nullptr;
            }

            contextData->changeBackendCounter = 0;

            return false;
        }

        return true;
    }

    // create new feature
    if (contextData->changeBackendCounter == 2)
    {
        LOG_INFO("Creating new {} upscaler", UpscalerDisplayName(state.newBackend));
        contextData->feature.reset();

        // The game's block was refilled by the evaluates since: a Ray Reconstruction switched back from FSR-RR was
        // created at display size (1920x1080 for a 1280x720 frame in Cyberpunk) and the GPU hung.
        if (!contextData->createParamsOwned)
            ApplyCreateSize(contextData);

        if (!GetFeature(state.newBackend, handleId, contextData->createParams, &contextData->feature))
        {
            LOG_ERROR("Upscaler can't created");
            return false;
        }

        return true;
    }

    // init feature
    if (contextData->changeBackendCounter == 3)
    {
        if (!contextData->createParamsOwned)
            ApplyCreateSize(contextData);

        auto initResult = contextData->feature->Init(device, cmdList, contextData->createParams);

        contextData->changeBackendCounter = 0;

        if (!initResult)
        {
            LOG_ERROR("init failed with {0} feature", UpscalerDisplayName(state.newBackend));

            if (state.newBackend == Upscaler::OpenNrRr)
            {
                OpenNrRr::Block("it did not initialise");
                state.newBackend = FfxApiProxy::IsDenoiserApiImplementedDx12() ? Upscaler::FSR_RR : Upscaler::FSR21;
                ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "OpenRR did not start, see the log" });
            }
            else if (state.newBackend != Upscaler::DLSSD)
            {
                if (cfg.Dx12Upscaler == Upscaler::DLSS)
                {
                    state.newBackend = Upscaler::XeSS;
                    ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "Falling back to XeSS" });
                }
                else
                {
                    state.newBackend = Upscaler::FSR21;
                    ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "Falling back to FSR 2.1.2" });
                }
            }
            else
            {
                // Retry DLSSD
                state.newBackend = Upscaler::DLSSD;
            }

            state.changeBackend[handleId] = true;

            return false;
        }
        else
        {
            LOG_INFO("init successful for {0}, upscaler changed", UpscalerDisplayName(state.newBackend));

            state.newBackend = Upscaler::Reset;
            state.changeBackend[handleId] = false;
        }

        // Delete only the temporary table this provider created. The game's own table is
        // still referenced by the feature during Evaluate and must outlive ChangeFeature.
        if (contextData->createParamsOwned && contextData->createParams != nullptr)
        {
            TryDestroyNGXParameters(contextData->createParams, NVNGXProxy::D3D12_DestroyParameters());
            contextData->createParams = nullptr;
            contextData->createParamsOwned = false;
        }
    }

    // if initial feature can't be inited
    state.currentFeature = contextData->feature.get();

    if (state.currentFG != nullptr && state.activeFgInput == FGInput::Upscaler)
        state.currentFG->UpdateTarget();

    return true;
}
