#pragma once

/**
 * @brief Common strings and identifiers used internally by OptiScaler
 */
namespace OptiKeys
{
using CString = const char[];

// Application name provided to upscalers
inline constexpr CString ProjectID = "OptiScaler";

// ID code used for the Vulkan input provider
inline constexpr CString VkProvider = "OptiVk";
// ID code used for the DX11 input provider
inline constexpr CString Dx11Provider = "OptiDx11";
// ID code used for the DX12 input provider
inline constexpr CString Dx12Provider = "OptiDx12";

// ID code used for the XeSS upscaler backend
inline constexpr CString XeSS = "xess";
// ID code used for the XeSS upscaler backend used with the DirectX 11 on 12 compatibility layer
inline constexpr CString XeSS_11on12 = "xess_12";
// ID code used for the FSR 2.1.x upscaler backend
inline constexpr CString FSR21 = "fsr21";
// ID code used for the FSR 2.1.x upscaler backend used with the DirectX 11 on 12 compatibility layer
inline constexpr CString FSR21_11on12 = "fsr21_12";
// ID code used for the FSR 2.2.x upscaler backend
inline constexpr CString FSR22 = "fsr22";
// ID code used for the FSR 2.2.x upscaler backend used with the DirectX 11 on 12 compatibility layer
inline constexpr CString FSR22_11on12 = "fsr22_12";
// ID code used for the FSR 3.1+ upscaler backend
inline constexpr CString FSR31 = "fsr31";
// ID code used for the FSR 3.1+ upscaler backend used with the DirectX 11 on 12 compatibility layer
inline constexpr CString FSR31_11on12 = "fsr31_12";
// ID code used for the DLSS upscaler backend
inline constexpr CString DLSS = "dlss";
// ID code used for the DLSS-D/Ray Reconstruction upscaler+denoiser backend
inline constexpr CString DLSSD = "dlssd";
// ID code used for the FSR Ray Regeneration
inline constexpr CString FSR_RR = "fsr-rr";

inline constexpr CString FSR_UpscaleWidth = "FSR.upscaleSize.width";
inline constexpr CString FSR_UpscaleHeight = "FSR.upscaleSize.height";

inline constexpr CString FSR_NearPlane = "FSR.cameraNear";
inline constexpr CString FSR_FarPlane = "FSR.cameraFar";
inline constexpr CString FSR_CameraFovVertical = "FSR.cameraFovAngleVertical";
inline constexpr CString FSR_FrameTimeDelta = "FSR.frameTimeDelta";
inline constexpr CString FSR_ViewSpaceToMetersFactor = "FSR.viewSpaceToMetersFactor";
inline constexpr CString FSR_TransparencyAndComp = "FSR.transparencyAndComposition";
inline constexpr CString FSR_Reactive = "FSR.reactive";

} // namespace OptiKeys

namespace OptiTexts
{
using CString = const char[];

// User friendly name for FSR-RR backend
inline constexpr CString FSR_RR_Name = "FSR Ray Regeneration";
} // namespace OptiTexts