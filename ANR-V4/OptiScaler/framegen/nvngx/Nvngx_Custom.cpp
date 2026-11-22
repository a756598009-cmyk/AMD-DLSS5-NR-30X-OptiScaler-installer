#include "pch.h"
#include "Nvngx_Custom.h"

#include <NVNGX_Parameter.h>
#include <misc/CustomProviders.h>

void Nvngx_Custom::LoadLibraries()
{
    dll = CustomProviders::LoadDll(_name, CustomProviders::DlssgDllPath(_name));
    if (dll == nullptr)
        return;

    // This OpenNR 0.6.10 Nvngx_DllProxy does not expose V.B20's LoadNgxExports().
    // Resolve the NGX DLSSG exports here, using the same export names as V.B20.
    _DLSSG_D3D12_Init = (PFN_D3D12_Init) GetProcAddress(dll, "NVSDK_NGX_D3D12_Init");
    _DLSSG_D3D12_Init_Ext = (PFN_D3D12_Init_Ext) GetProcAddress(dll, "NVSDK_NGX_D3D12_Init_Ext");
    _DLSSG_D3D12_Shutdown = (PFN_D3D12_Shutdown) GetProcAddress(dll, "NVSDK_NGX_D3D12_Shutdown");
    _DLSSG_D3D12_Shutdown1 = (PFN_D3D12_Shutdown1) GetProcAddress(dll, "NVSDK_NGX_D3D12_Shutdown1");
    _DLSSG_D3D12_GetScratchBufferSize =
        (PFN_D3D12_GetScratchBufferSize) GetProcAddress(dll, "NVSDK_NGX_D3D12_GetScratchBufferSize");
    _DLSSG_D3D12_CreateFeature = (PFN_D3D12_CreateFeature) GetProcAddress(dll, "NVSDK_NGX_D3D12_CreateFeature");
    _DLSSG_D3D12_ReleaseFeature = (PFN_D3D12_ReleaseFeature) GetProcAddress(dll, "NVSDK_NGX_D3D12_ReleaseFeature");
    _DLSSG_D3D12_GetFeatureRequirements =
        (PFN_D3D12_GetFeatureRequirements) GetProcAddress(dll, "NVSDK_NGX_D3D12_GetFeatureRequirements");
    _DLSSG_D3D12_EvaluateFeature =
        (PFN_D3D12_EvaluateFeature) GetProcAddress(dll, "NVSDK_NGX_D3D12_EvaluateFeature");
    _DLSSG_D3D12_PopulateParameters_Impl =
        (PFN_D3D12_PopulateParameters_Impl) GetProcAddress(dll, "NVSDK_NGX_D3D12_PopulateParameters_Impl");

    _DLSSG_VULKAN_Init = (PFN_VULKAN_Init) GetProcAddress(dll, "NVSDK_NGX_VULKAN_Init");
    _DLSSG_VULKAN_Init_Ext = (PFN_VULKAN_Init_Ext) GetProcAddress(dll, "NVSDK_NGX_VULKAN_Init_Ext");
    _DLSSG_VULKAN_Init_Ext2 = (PFN_VULKAN_Init_Ext2) GetProcAddress(dll, "NVSDK_NGX_VULKAN_Init_Ext2");
    _DLSSG_VULKAN_Shutdown = (PFN_VULKAN_Shutdown) GetProcAddress(dll, "NVSDK_NGX_VULKAN_Shutdown");
    _DLSSG_VULKAN_Shutdown1 = (PFN_VULKAN_Shutdown1) GetProcAddress(dll, "NVSDK_NGX_VULKAN_Shutdown1");
    _DLSSG_VULKAN_GetScratchBufferSize =
        (PFN_VULKAN_GetScratchBufferSize) GetProcAddress(dll, "NVSDK_NGX_VULKAN_GetScratchBufferSize");
    _DLSSG_VULKAN_CreateFeature = (PFN_VULKAN_CreateFeature) GetProcAddress(dll, "NVSDK_NGX_VULKAN_CreateFeature");
    _DLSSG_VULKAN_CreateFeature1 =
        (PFN_VULKAN_CreateFeature1) GetProcAddress(dll, "NVSDK_NGX_VULKAN_CreateFeature1");
    _DLSSG_VULKAN_ReleaseFeature =
        (PFN_VULKAN_ReleaseFeature) GetProcAddress(dll, "NVSDK_NGX_VULKAN_ReleaseFeature");
    _DLSSG_VULKAN_GetFeatureRequirements =
        (PFN_VULKAN_GetFeatureRequirements) GetProcAddress(dll, "NVSDK_NGX_VULKAN_GetFeatureRequirements");
    _DLSSG_VULKAN_EvaluateFeature =
        (PFN_VULKAN_EvaluateFeature) GetProcAddress(dll, "NVSDK_NGX_VULKAN_EvaluateFeature");
    _DLSSG_VULKAN_PopulateParameters_Impl =
        (PFN_VULKAN_PopulateParameters_Impl) GetProcAddress(dll, "NVSDK_NGX_VULKAN_PopulateParameters_Impl");

    auto populate = _DLSSG_D3D12_PopulateParameters_Impl != nullptr
                        ? _DLSSG_D3D12_PopulateParameters_Impl
                        : (PFN_D3D12_PopulateParameters_Impl) _DLSSG_VULKAN_PopulateParameters_Impl;

    NVNGX_Parameters params(API::DX12, false);
    int countMax = 0;
    if (populate != nullptr && populate(&params) == NVSDK_NGX_Result_Success &&
        params.Get("DLSSG.MultiFrameCountMax", &countMax) == NVSDK_NGX_Result_Success && countMax > 0)
    {
        _maxFakeFrames = countMax;
    }

    LOG_INFO("Custom DLSSG {} initialized, max fake frames: {}", _name, _maxFakeFrames);
}
