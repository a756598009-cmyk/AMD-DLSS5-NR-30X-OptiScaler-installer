#include <pch.h>

#include "Streamline_Hooks.h"

#include <Util.h>
#include <Config.h>

#include <nvapi/fakenvapi.h>
#include <misc/IdentifyGpu.h>
#include <framegen/nvngx/Nvngx_FG.h>
#include <dlssnr/amd/AmdBridge.h>
#include <framegen/dlssg/MfgUnlock.h>
#include <framegen/xefg/XeFG_Dx12.h>
#include <proxies/KernelBase_Proxy.h>
#include <imgui/ImGuiNotify.hpp>

#include <json.hpp>
#include <magic_enum.hpp>
#include "detours/detours.h"
#include <sl1_reflex.h>
#include <NVNGX_Parameter.h>
#include <plugins/PluginApi.h>

std::mutex StreamlineHooks::rrSignalTagMutex {};
RRSignalTagDiagnostics StreamlineHooks::rrSignalTagDiagnostics {};
std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, static_cast<size_t>(RRTaggedSignal::Count)>
    StreamlineHooks::rrTaggedD3D12Resources {};
SLTagInventoryDiagnostics StreamlineHooks::slTagInventoryDiagnostics {};
RRNGXPointerDiagnostics StreamlineHooks::rrNGXPointerDiagnostics {};

namespace
{
thread_local uint32_t g_rrActiveEvaluationFrame = UINT32_MAX;
thread_local uint32_t g_rrActiveEvaluationViewport = UINT32_MAX;

class ScopedRRActiveEvaluationFrame
{
  public:
    ScopedRRActiveEvaluationFrame(uint32_t frame, uint32_t viewport) noexcept
        : m_previousFrame(g_rrActiveEvaluationFrame), m_previousViewport(g_rrActiveEvaluationViewport)
    {
        g_rrActiveEvaluationFrame = frame;
        g_rrActiveEvaluationViewport = viewport;
    }

    ~ScopedRRActiveEvaluationFrame()
    {
        g_rrActiveEvaluationFrame = m_previousFrame;
        g_rrActiveEvaluationViewport = m_previousViewport;
    }

  private:
    uint32_t m_previousFrame;
    uint32_t m_previousViewport;
};
} // namespace

static bool TryGetRRTaggedSignal(sl::BufferType type, RRTaggedSignal& signal)
{
    switch (type)
    {
    case sl::kBufferTypeNormalRoughness:
        signal = RRTaggedSignal::NormalRoughness;
        return true;
    case sl::kBufferTypeEmissive:
        signal = RRTaggedSignal::Emissive;
        return true;
    case sl::kBufferTypeSpecularMotionVectors:
        signal = RRTaggedSignal::SpecularMotionVectors;
        return true;
    case sl::kBufferTypeReflectionMotionVectors:
        signal = RRTaggedSignal::ReflectionMotionVectors;
        return true;
    case sl::kBufferTypeSpecularHitDistance:
        signal = RRTaggedSignal::SpecularHitDistance;
        return true;
    case sl::kBufferTypeSpecularRayDirectionHitDistance:
        signal = RRTaggedSignal::SpecularRayDirectionHitDistance;
        return true;
    case sl::kBufferTypeLinearDepth:
        signal = RRTaggedSignal::LinearDepth;
        return true;
    case sl::kBufferTypeDiffuseHitNoisy:
        signal = RRTaggedSignal::DiffuseNoisy;
        return true;
    case sl::kBufferTypeDiffuseHitDenoised:
        signal = RRTaggedSignal::DiffuseDenoised;
        return true;
    case sl::kBufferTypeSpecularHitNoisy:
        signal = RRTaggedSignal::SpecularNoisy;
        return true;
    case sl::kBufferTypeSpecularHitDenoised:
        signal = RRTaggedSignal::SpecularDenoised;
        return true;
    case sl::kBufferTypeShadowNoisy:
        signal = RRTaggedSignal::ShadowNoisy;
        return true;
    case sl::kBufferTypeShadowDenoised:
        signal = RRTaggedSignal::ShadowDenoised;
        return true;
    case sl::kBufferTypeAmbientOcclusionNoisy:
        signal = RRTaggedSignal::AmbientOcclusionNoisy;
        return true;
    case sl::kBufferTypeAmbientOcclusionDenoised:
        signal = RRTaggedSignal::AmbientOcclusionDenoised;
        return true;
    case sl::kBufferTypeShadowHint:
        signal = RRTaggedSignal::ShadowHint;
        return true;
    case sl::kBufferTypeReflectionHint:
        signal = RRTaggedSignal::ReflectionHint;
        return true;
    default:
        return false;
    }
}

static bool IsRRCheckerboardInput(RRTaggedSignal signal)
{
    return signal == RRTaggedSignal::DiffuseNoisy || signal == RRTaggedSignal::SpecularNoisy ||
           signal == RRTaggedSignal::ShadowNoisy || signal == RRTaggedSignal::AmbientOcclusionNoisy;
}

static const char* GetRRTagSourceName(RRTagSource source)
{
    switch (source)
    {
    case RRTagSource::SetTag:
        return "slSetTag";
    case RRTagSource::SetTagForFrame:
        return "slSetTagForFrame";
    case RRTagSource::EvaluateFeature:
        return "slEvaluateFeature";
    default:
        return "unknown";
    }
}

const char* StreamlineHooks::getSLBufferTypeName(sl::BufferType type)
{
    static constexpr std::array<const char*, 68> names { "Depth",
                                                         "MotionVectors",
                                                         "HUDLessColor",
                                                         "ScalingInputColor",
                                                         "ScalingOutputColor",
                                                         "Normals",
                                                         "Roughness",
                                                         "Albedo",
                                                         "SpecularAlbedo",
                                                         "IndirectAlbedo",
                                                         "SpecularMotionVectors",
                                                         "DisocclusionMask",
                                                         "Emissive",
                                                         "Exposure",
                                                         "NormalRoughness",
                                                         "DiffuseHitNoisy",
                                                         "DiffuseHitDenoised",
                                                         "SpecularHitNoisy",
                                                         "SpecularHitDenoised",
                                                         "ShadowNoisy",
                                                         "ShadowDenoised",
                                                         "AmbientOcclusionNoisy",
                                                         "AmbientOcclusionDenoised",
                                                         "UIColorAndAlpha",
                                                         "ShadowHint",
                                                         "ReflectionHint",
                                                         "ParticleHint",
                                                         "TransparencyHint",
                                                         "AnimatedTextureHint",
                                                         "BiasCurrentColorHint",
                                                         "RaytracingDistance",
                                                         "ReflectionMotionVectors",
                                                         "Position",
                                                         "InvalidDepthMotionHint",
                                                         "Alpha",
                                                         "OpaqueColor",
                                                         "ReactiveMaskHint",
                                                         "TransparencyAndCompositionMaskHint",
                                                         "ReflectedAlbedo",
                                                         "ColorBeforeParticles",
                                                         "ColorBeforeTransparency",
                                                         "ColorBeforeFog",
                                                         "SpecularHitDistance",
                                                         "SpecularRayDirectionHitDistance",
                                                         "SpecularRayDirection",
                                                         "DiffuseHitDistance",
                                                         "DiffuseRayDirectionHitDistance",
                                                         "DiffuseRayDirection",
                                                         "HiResDepth",
                                                         "LinearDepth",
                                                         "BidirectionalDistortionField",
                                                         "TransparencyLayer",
                                                         "TransparencyLayerOpacity",
                                                         "Backbuffer",
                                                         "NoWarpMask",
                                                         "ColorAfterParticles",
                                                         "ColorAfterTransparency",
                                                         "ColorAfterFog",
                                                         "ScreenSpaceSubsurfaceScatteringGuide",
                                                         "ColorBeforeScreenSpaceSubsurfaceScattering",
                                                         "ColorAfterScreenSpaceSubsurfaceScattering",
                                                         "ScreenSpaceRefractionGuide",
                                                         "ColorBeforeScreenSpaceRefraction",
                                                         "ColorAfterScreenSpaceRefraction",
                                                         "DepthOfFieldGuide",
                                                         "ColorBeforeDepthOfField",
                                                         "ColorAfterDepthOfField",
                                                         "ScalingOutputAlpha" };

    return type < names.size() ? names[type] : "Unknown/custom";
}

const char* StreamlineHooks::getRRTaggedSignalName(RRTaggedSignal signal)
{
    switch (signal)
    {
    case RRTaggedSignal::NormalRoughness:
        return "NormalRoughness";
    case RRTaggedSignal::Emissive:
        return "Emissive";
    case RRTaggedSignal::SpecularMotionVectors:
        return "SpecularMotionVectors";
    case RRTaggedSignal::ReflectionMotionVectors:
        return "ReflectionMotionVectors";
    case RRTaggedSignal::SpecularHitDistance:
        return "SpecularHitDistance";
    case RRTaggedSignal::SpecularRayDirectionHitDistance:
        return "SpecularRayDirectionHitDistance";
    case RRTaggedSignal::LinearDepth:
        return "LinearDepth";
    case RRTaggedSignal::DiffuseNoisy:
        return "Diffuse.Noisy";
    case RRTaggedSignal::DiffuseDenoised:
        return "Diffuse.Denoised";
    case RRTaggedSignal::SpecularNoisy:
        return "Specular.Noisy";
    case RRTaggedSignal::SpecularDenoised:
        return "Specular.Denoised";
    case RRTaggedSignal::ShadowNoisy:
        return "Shadow.Noisy";
    case RRTaggedSignal::ShadowDenoised:
        return "Shadow.Denoised";
    case RRTaggedSignal::AmbientOcclusionNoisy:
        return "AmbientOcclusion.Noisy";
    case RRTaggedSignal::AmbientOcclusionDenoised:
        return "AmbientOcclusion.Denoised";
    case RRTaggedSignal::ShadowHint:
        return "ShadowHint";
    case RRTaggedSignal::ReflectionHint:
        return "ReflectionHint";
    default:
        return "Unknown";
    }
}

const char* StreamlineHooks::getRRPreferredTagFormat(RRTaggedSignal signal)
{
    switch (signal)
    {
    case RRTaggedSignal::NormalRoughness:
        return "game-defined";
    case RRTaggedSignal::Emissive:
        return "shader-readable color texture";
    case RRTaggedSignal::SpecularMotionVectors:
    case RRTaggedSignal::ReflectionMotionVectors:
        return "DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT, "
               "DXGI_FORMAT_R16G16B16A16_FLOAT, or DXGI_FORMAT_R32G32B32A32_FLOAT";
    case RRTaggedSignal::SpecularHitDistance:
        return "DXGI_FORMAT_R16_FLOAT or DXGI_FORMAT_R32_FLOAT";
    case RRTaggedSignal::SpecularRayDirectionHitDistance:
        return "DXGI_FORMAT_R16G16B16A16_FLOAT or DXGI_FORMAT_R32G32B32A32_FLOAT";
    case RRTaggedSignal::LinearDepth:
        return "DXGI_FORMAT_R32_FLOAT or DXGI_FORMAT_R16_FLOAT";
    case RRTaggedSignal::DiffuseNoisy:
    case RRTaggedSignal::DiffuseDenoised:
    case RRTaggedSignal::SpecularNoisy:
    case RRTaggedSignal::SpecularDenoised:
        return "DXGI_FORMAT_R16G16B16A16_FLOAT";
    case RRTaggedSignal::ShadowNoisy:
        return "DXGI_FORMAT_R16_FLOAT";
    case RRTaggedSignal::ShadowDenoised:
    case RRTaggedSignal::AmbientOcclusionNoisy:
    case RRTaggedSignal::AmbientOcclusionDenoised:
        return "DXGI_FORMAT_R8_UNORM";
    case RRTaggedSignal::ShadowHint:
    case RRTaggedSignal::ReflectionHint:
        return "hint only; not an RR signal";
    default:
        return "unknown";
    }
}

bool StreamlineHooks::isRRPreferredTagFormat(RRTaggedSignal signal, DXGI_FORMAT format)
{
    switch (signal)
    {
    case RRTaggedSignal::NormalRoughness:
        return format != DXGI_FORMAT_UNKNOWN;
    case RRTaggedSignal::Emissive:
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return true;
        default:
            return false;
        }
    case RRTaggedSignal::SpecularMotionVectors:
    case RRTaggedSignal::ReflectionMotionVectors:
        switch (format)
        {
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return true;
        default:
            return false;
        }
    case RRTaggedSignal::SpecularHitDistance:
        return format == DXGI_FORMAT_R16_FLOAT || format == DXGI_FORMAT_R32_FLOAT;
    case RRTaggedSignal::SpecularRayDirectionHitDistance:
        return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    case RRTaggedSignal::LinearDepth:
        return format == DXGI_FORMAT_R32_FLOAT || format == DXGI_FORMAT_R16_FLOAT;
    case RRTaggedSignal::DiffuseNoisy:
    case RRTaggedSignal::DiffuseDenoised:
    case RRTaggedSignal::SpecularNoisy:
    case RRTaggedSignal::SpecularDenoised:
        return format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    case RRTaggedSignal::ShadowNoisy:
        return format == DXGI_FORMAT_R16_FLOAT;
    case RRTaggedSignal::ShadowDenoised:
    case RRTaggedSignal::AmbientOcclusionNoisy:
    case RRTaggedSignal::AmbientOcclusionDenoised:
        return format == DXGI_FORMAT_R8_UNORM;
    default:
        return false;
    }
}

const char* StreamlineHooks::getRRCheckerboardAssessment(RRTaggedSignal signal,
                                                         const RRTaggedResourceDiagnostic& diagnostic,
                                                         uint32_t renderWidth, uint32_t renderHeight)
{
    if (!diagnostic.observed)
        return "not-observed";
    if (!diagnostic.present)
        return "cleared";
    if (!IsRRCheckerboardInput(signal))
        return "not-applicable";
    if (renderWidth == 0 || renderHeight == 0)
        return "render-size-unknown";
    if (diagnostic.effectiveHeight != renderHeight)
        return "resolution-mismatch";
    if (diagnostic.effectiveWidth == renderWidth)
        return "full-resolution";

    const uint64_t doubledWidth = static_cast<uint64_t>(diagnostic.effectiveWidth) * 2u;
    const uint64_t renderWidth64 = renderWidth;
    if (doubledWidth + 1u >= renderWidth64 && doubledWidth <= renderWidth64 + 1u)
        return "half-width-candidate";

    return "resolution-mismatch";
}

RRSignalTagDiagnostics StreamlineHooks::getRRSignalTagDiagnostics()
{
    std::scoped_lock lock(rrSignalTagMutex);
    return rrSignalTagDiagnostics;
}

Microsoft::WRL::ComPtr<ID3D12Resource> StreamlineHooks::getRRTaggedD3D12Resource(RRTaggedSignal signal)
{
    std::scoped_lock lock(rrSignalTagMutex);
    return rrTaggedD3D12Resources[static_cast<size_t>(signal)];
}

RRD3D12SignalTagSnapshot StreamlineHooks::getRRD3D12SignalTagSnapshot()
{
    std::scoped_lock lock(rrSignalTagMutex);
    RRD3D12SignalTagSnapshot snapshot {};
    snapshot.generation = rrSignalTagDiagnostics.generation;
    snapshot.activeEvaluationFrame = g_rrActiveEvaluationFrame;
    snapshot.activeEvaluationViewport = g_rrActiveEvaluationViewport;
    for (size_t i = 0; i < snapshot.resources.size(); ++i)
    {
        snapshot.resources[i].diagnostic = rrSignalTagDiagnostics.resources[i];
        snapshot.resources[i].resource = rrTaggedD3D12Resources[i];
    }
    return snapshot;
}

SLConstantsSnapshot StreamlineHooks::getSLConstantsSnapshot()
{
    std::scoped_lock lock(setConstantsMutex);
    const auto& state = State::Instance();
    return { .constants = state.slLastConstants,
             .frameIndex = state.slLastConstantsFrame,
             .viewport = state.slLastConstantsViewport };
}

void StreamlineHooks::resetRRSignalTagDiagnostics()
{
    std::scoped_lock lock(rrSignalTagMutex);
    rrSignalTagDiagnostics = {};
    rrTaggedD3D12Resources = {};
}

static bool HasResourceMetadataChanged(const RRTaggedResourceDiagnostic& previous,
                                       const RRTaggedResourceDiagnostic& next)
{
    return !previous.observed || previous.present != next.present || previous.debugName != next.debugName ||
           previous.nativeWidth != next.nativeWidth || previous.nativeHeight != next.nativeHeight ||
           previous.effectiveWidth != next.effectiveWidth || previous.effectiveHeight != next.effectiveHeight ||
           previous.extentLeft != next.extentLeft || previous.extentTop != next.extentTop ||
           previous.format != next.format || previous.dimension != next.dimension ||
           previous.resourceFlags != next.resourceFlags || previous.mipLevels != next.mipLevels ||
           previous.arraySize != next.arraySize || previous.sampleCount != next.sampleCount ||
           previous.state != next.state || previous.lifecycle != next.lifecycle || previous.viewport != next.viewport;
}

static std::string GetD3D12DebugObjectName(ID3D12Object* object)
{
    if (object == nullptr)
        return {};

    UINT nameSize = 0;
    object->GetPrivateData(WKPDID_D3DDebugObjectName, &nameSize, nullptr);
    if (nameSize > 1)
    {
        std::string name(nameSize, '\0');
        if (SUCCEEDED(object->GetPrivateData(WKPDID_D3DDebugObjectName, &nameSize, name.data())))
        {
            name.resize(strnlen(name.c_str(), name.size()));
            return name;
        }
    }

    nameSize = 0;
    object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &nameSize, nullptr);
    if (nameSize > sizeof(wchar_t))
    {
        std::vector<wchar_t> name(nameSize / sizeof(wchar_t), L'\0');
        if (SUCCEEDED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &nameSize, name.data())))
            return wstring_to_string(name.data());
    }

    return {};
}

static RRTaggedResourceDiagnostic CaptureSLResourceTag(const sl::ResourceTag& tag, uint32_t frameIndex,
                                                       uint32_t viewport, RRTagSource source)
{
    RRTaggedResourceDiagnostic result {};
    result.observed = true;
    result.present = tag.resource != nullptr && tag.resource->native != nullptr;
    result.frameIndex = frameIndex;
    result.viewport = viewport;
    result.source = source;
    result.lifecycle = tag.lifecycle;

    if (!result.present)
        return result;

    result.resourceAddress = tag.resource->native;
    result.state = tag.resource->state;

    auto* resource = reinterpret_cast<ID3D12Resource*>(tag.resource->native);
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    result.debugName = GetD3D12DebugObjectName(resource);
    result.nativeWidth = desc.Width;
    result.nativeHeight = desc.Height;
    result.format = desc.Format;
    result.dimension = desc.Dimension;
    result.resourceFlags = desc.Flags;
    result.mipLevels = desc.MipLevels;
    result.arraySize = desc.DepthOrArraySize;
    result.sampleCount = desc.SampleDesc.Count;

    result.usesExtent = static_cast<bool>(tag.extent);
    result.extentLeft = tag.extent.left;
    result.extentTop = tag.extent.top;
    result.effectiveWidth =
        result.usesExtent ? tag.extent.width : static_cast<uint32_t>(std::min<uint64_t>(desc.Width, UINT32_MAX));
    result.effectiveHeight = result.usesExtent ? tag.extent.height : desc.Height;
    return result;
}

SLTagInventoryDiagnostics StreamlineHooks::getSLTagInventoryDiagnostics()
{
    std::scoped_lock lock(rrSignalTagMutex);
    return slTagInventoryDiagnostics;
}

void StreamlineHooks::probeSLResourceTag(const sl::ResourceTag& tag, uint32_t frameIndex, uint32_t viewport,
                                         RRTagSource source)
{
    RRTaggedResourceDiagnostic next = CaptureSLResourceTag(tag, frameIndex, viewport, source);
    bool shouldLog = false;

    {
        std::scoped_lock lock(rrSignalTagMutex);
        auto entry = std::find_if(
            slTagInventoryDiagnostics.resources.begin(), slTagInventoryDiagnostics.resources.end(),
            [&tag](const SLTaggedResourceInventoryEntry& candidate) { return candidate.type == tag.type; });

        if (entry == slTagInventoryDiagnostics.resources.end())
        {
            slTagInventoryDiagnostics.resources.push_back({ tag.type, next });
            entry = std::prev(slTagInventoryDiagnostics.resources.end());
            shouldLog = true;
        }
        else
        {
            next.updateCount = entry->resource.updateCount + 1;
            shouldLog = HasResourceMetadataChanged(entry->resource, next);
            entry->resource = next;
        }

        if (entry->resource.updateCount == 0)
            entry->resource.updateCount = 1;

        std::sort(slTagInventoryDiagnostics.resources.begin(), slTagInventoryDiagnostics.resources.end(),
                  [](const SLTaggedResourceInventoryEntry& left, const SLTaggedResourceInventoryEntry& right)
                  { return left.type < right.type; });
        ++slTagInventoryDiagnostics.generation;
    }

    if (!shouldLog)
        return;

    if (!next.present)
    {
        LOG_INFO("[SL_TAG_INVENTORY] {} type={} ({}) frame={} cleared", GetRRTagSourceName(source), tag.type,
                 getSLBufferTypeName(tag.type), frameIndex);
        return;
    }

    const auto formatName = magic_enum::enum_name(next.format);
    LOG_INFO("[SL_TAG_INVENTORY] {} type={} ({}) frame={} ptr={}, debugName='{}', native={}x{}, effective={}x{}, "
             "extent=[{},{}], format={}({}), dimension={}, mips={}, arrays={}, samples={}, "
             "resourceFlags={:#x}, state={:#x}",
             GetRRTagSourceName(source), tag.type, getSLBufferTypeName(tag.type), frameIndex, next.resourceAddress,
             next.debugName, next.nativeWidth, next.nativeHeight, next.effectiveWidth, next.effectiveHeight,
             next.extentLeft, next.extentTop, formatName.empty() ? "UNKNOWN" : formatName,
             static_cast<uint32_t>(next.format), static_cast<uint32_t>(next.dimension), next.mipLevels, next.arraySize,
             next.sampleCount, static_cast<uint32_t>(next.resourceFlags), next.state);
}

void StreamlineHooks::logSLTagInventoryDiagnostics(uint32_t renderWidth, uint32_t renderHeight)
{
    const auto diagnostics = getSLTagInventoryDiagnostics();
    LOG_INFO("[SL_TAG_INVENTORY] snapshot: generation={}, observedTypes={}, render={}x{}", diagnostics.generation,
             diagnostics.resources.size(), renderWidth, renderHeight);

    for (const auto& entry : diagnostics.resources)
    {
        const auto& diagnostic = entry.resource;
        if (!diagnostic.present)
        {
            LOG_INFO("[SL_TAG_INVENTORY] snapshot type={} ({}): cleared, updates={}", entry.type,
                     getSLBufferTypeName(entry.type), diagnostic.updateCount);
            continue;
        }

        const auto formatName = magic_enum::enum_name(diagnostic.format);
        const bool fullResolution = renderWidth != 0 && renderHeight != 0 && diagnostic.effectiveWidth == renderWidth &&
                                    diagnostic.effectiveHeight == renderHeight;
        const bool halfWidthCandidate =
            renderWidth != 0 && renderHeight != 0 && diagnostic.effectiveHeight == renderHeight &&
            static_cast<uint64_t>(diagnostic.effectiveWidth) * 2u + 1u >= renderWidth &&
            static_cast<uint64_t>(diagnostic.effectiveWidth) * 2u <= static_cast<uint64_t>(renderWidth) + 1u;

        LOG_INFO("[SL_TAG_INVENTORY] snapshot type={} ({}): ptr={}, debugName='{}', effective={}x{}, native={}x{}, "
                 "format={}({}), state={:#x}, updates={}, resolutionClass={}",
                 entry.type, getSLBufferTypeName(entry.type), diagnostic.resourceAddress, diagnostic.debugName,
                 diagnostic.effectiveWidth, diagnostic.effectiveHeight, diagnostic.nativeWidth, diagnostic.nativeHeight,
                 formatName.empty() ? "UNKNOWN" : formatName, static_cast<uint32_t>(diagnostic.format),
                 diagnostic.state, diagnostic.updateCount,
                 fullResolution       ? "full-resolution"
                 : halfWidthCandidate ? "half-width-candidate"
                                      : "other");
    }
}

RRNGXPointerDiagnostics StreamlineHooks::getRRNGXPointerDiagnostics()
{
    std::scoped_lock lock(rrSignalTagMutex);
    return rrNGXPointerDiagnostics;
}

static const char* GetNGXPointerKindName(RRNGXPointerKind kind)
{
    switch (kind)
    {
    case RRNGXPointerKind::OpaquePointer:
        return "opaque-pointer";
    case RRNGXPointerKind::D3D11Resource:
        return "D3D11-resource";
    case RRNGXPointerKind::D3D12Resource:
        return "D3D12-resource";
    default:
        return "unknown";
    }
}

void StreamlineHooks::probeRRNGXPointerParameters(const NVSDK_NGX_Parameter& parameters)
{
    static std::atomic_uint64_t probeCalls = 0;
    const uint64_t probeCall = probeCalls.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock(rrSignalTagMutex);
        if (rrNGXPointerDiagnostics.generation != 0 && probeCall % 120u != 0)
            return;
    }

    uint32_t allocationType = NGX_AllocTypes::Unknown;
    if (parameters.Get(NGX_AllocTypes::AllocKey.data(), &allocationType) != NVSDK_NGX_Result_Success ||
        (allocationType != NGX_AllocTypes::InternDynamic && allocationType != NGX_AllocTypes::InternPersistent))
    {
        std::scoped_lock lock(rrSignalTagMutex);
        rrNGXPointerDiagnostics.tableInspectable = false;
        return;
    }

    const auto& internalParameters = static_cast<const NVNGX_Parameters&>(parameters);
    const auto pointers = internalParameters.enumeratePointerParameters();

    std::vector<RRNGXPointerDiagnostic> nextEntries;
    nextEntries.reserve(pointers.size());
    for (const auto& pointer : pointers)
    {
        RRNGXPointerDiagnostic next {};
        next.name = pointer.name;
        next.present = pointer.address != nullptr;
        next.address = pointer.address;

        switch (pointer.kind)
        {
        case NGXPointerParameterKind::D3D11Resource:
            next.kind = RRNGXPointerKind::D3D11Resource;
            break;
        case NGXPointerParameterKind::D3D12Resource:
            next.kind = RRNGXPointerKind::D3D12Resource;
            break;
        default:
            next.kind = RRNGXPointerKind::OpaquePointer;
            break;
        }

        // Only exact D3D12 resource values are dereferenced. NGX void pointers
        // can also contain matrices, callbacks, or backend-specific objects.
        if (next.present && next.kind == RRNGXPointerKind::D3D12Resource)
        {
            const auto desc = static_cast<ID3D12Resource*>(pointer.address)->GetDesc();
            next.nativeWidth = desc.Width;
            next.nativeHeight = desc.Height;
            next.format = desc.Format;
            next.dimension = desc.Dimension;
            next.resourceFlags = desc.Flags;
            next.mipLevels = desc.MipLevels;
            next.arraySize = desc.DepthOrArraySize;
            next.sampleCount = desc.SampleDesc.Count;
        }

        nextEntries.push_back(std::move(next));
    }

    std::sort(nextEntries.begin(), nextEntries.end(),
              [](const RRNGXPointerDiagnostic& left, const RRNGXPointerDiagnostic& right)
              { return left.name < right.name; });

    std::vector<RRNGXPointerDiagnostic> changedEntries;
    {
        std::scoped_lock lock(rrSignalTagMutex);
        for (auto& next : nextEntries)
        {
            const auto previous =
                std::find_if(rrNGXPointerDiagnostics.parameters.begin(), rrNGXPointerDiagnostics.parameters.end(),
                             [&next](const RRNGXPointerDiagnostic& candidate) { return candidate.name == next.name; });

            if (previous != rrNGXPointerDiagnostics.parameters.end())
            {
                next.updateCount = previous->updateCount + 1;
                const bool changed =
                    previous->kind != next.kind || previous->present != next.present ||
                    previous->nativeWidth != next.nativeWidth || previous->nativeHeight != next.nativeHeight ||
                    previous->format != next.format || previous->dimension != next.dimension ||
                    previous->resourceFlags != next.resourceFlags || previous->mipLevels != next.mipLevels ||
                    previous->arraySize != next.arraySize || previous->sampleCount != next.sampleCount;
                if (changed)
                    changedEntries.push_back(next);
            }
            else
            {
                next.updateCount = 1;
                changedEntries.push_back(next);
            }
        }

        rrNGXPointerDiagnostics.tableInspectable = true;
        rrNGXPointerDiagnostics.parameters = nextEntries;
        ++rrNGXPointerDiagnostics.generation;
    }

    for (const auto& entry : changedEntries)
    {
        if (entry.kind != RRNGXPointerKind::D3D12Resource || !entry.present)
        {
            LOG_INFO("[RR_NGX_INVENTORY] key='{}', kind={}, ptr={}, present={}", entry.name,
                     GetNGXPointerKindName(entry.kind), entry.address, entry.present);
            continue;
        }

        const auto formatName = magic_enum::enum_name(entry.format);
        LOG_INFO("[RR_NGX_INVENTORY] key='{}', kind={}, ptr={}, size={}x{}, format={}({}), "
                 "dimension={}, mips={}, arrays={}, samples={}, resourceFlags={:#x}",
                 entry.name, GetNGXPointerKindName(entry.kind), entry.address, entry.nativeWidth, entry.nativeHeight,
                 formatName.empty() ? "UNKNOWN" : formatName, static_cast<uint32_t>(entry.format),
                 static_cast<uint32_t>(entry.dimension), entry.mipLevels, entry.arraySize, entry.sampleCount,
                 static_cast<uint32_t>(entry.resourceFlags));
    }
}

void StreamlineHooks::logRRNGXPointerDiagnostics()
{
    const auto diagnostics = getRRNGXPointerDiagnostics();
    LOG_INFO("[RR_NGX_INVENTORY] snapshot: inspectable={}, generation={}, pointerParameters={}",
             diagnostics.tableInspectable, diagnostics.generation, diagnostics.parameters.size());

    for (const auto& entry : diagnostics.parameters)
    {
        if (entry.kind != RRNGXPointerKind::D3D12Resource || !entry.present)
        {
            LOG_INFO("[RR_NGX_INVENTORY] snapshot key='{}': kind={}, ptr={}, present={}, updates={}", entry.name,
                     GetNGXPointerKindName(entry.kind), entry.address, entry.present, entry.updateCount);
            continue;
        }

        const auto formatName = magic_enum::enum_name(entry.format);
        LOG_INFO("[RR_NGX_INVENTORY] snapshot key='{}': kind={}, ptr={}, size={}x{}, "
                 "format={}({}), dimension={}, updates={}",
                 entry.name, GetNGXPointerKindName(entry.kind), entry.address, entry.nativeWidth, entry.nativeHeight,
                 formatName.empty() ? "UNKNOWN" : formatName, static_cast<uint32_t>(entry.format),
                 static_cast<uint32_t>(entry.dimension), entry.updateCount);
    }
}

void StreamlineHooks::resetRRInputInventoryDiagnostics()
{
    std::scoped_lock lock(rrSignalTagMutex);
    slTagInventoryDiagnostics = {};
    rrNGXPointerDiagnostics = {};
}

void StreamlineHooks::probeRRResourceTag(const sl::ResourceTag& tag, uint32_t frameIndex, uint32_t viewport,
                                         RRTagSource source)
{
    RRTaggedSignal signal;
    if (!TryGetRRTaggedSignal(tag.type, signal))
        return;

    RRTaggedResourceDiagnostic next = CaptureSLResourceTag(tag, frameIndex, viewport, source);

    RRTaggedResourceDiagnostic previous {};
    bool shouldLog = false;
    {
        std::scoped_lock lock(rrSignalTagMutex);
        auto& stored = rrSignalTagDiagnostics.resources[static_cast<size_t>(signal)];
        previous = stored;
        next.updateCount = previous.updateCount + 1;

        shouldLog = HasResourceMetadataChanged(previous, next);

        stored = next;
        auto& retainedResource = rrTaggedD3D12Resources[static_cast<size_t>(signal)];
        // eOnlyValidNow submitted through SetTag cannot be cached after that call
        // returns. A local ResourceTag passed directly to EvaluateFeature remains
        // inside its declaring call, however, and is retained only until the local
        // overlay is restored when that evaluation returns.
        retainedResource = next.present && (next.lifecycle != sl::ResourceLifecycle::eOnlyValidNow ||
                                            source == RRTagSource::EvaluateFeature)
                               ? reinterpret_cast<ID3D12Resource*>(next.resourceAddress)
                               : nullptr;
        ++rrSignalTagDiagnostics.generation;
    }

    if (!shouldLog)
        return;

    if (!next.present)
    {
        LOG_INFO("[RR_TAG_DIAG] {} {} frame={} cleared", GetRRTagSourceName(source), getRRTaggedSignalName(signal),
                 frameIndex);
        return;
    }

    const auto formatName = magic_enum::enum_name(next.format);
    const auto lifecycleName = magic_enum::enum_name(next.lifecycle);
    LOG_INFO("[RR_TAG_DIAG] {} {} frame={} ptr={}, debugName='{}', native={}x{}, effective={}x{}, "
             "extent=[{},{}], format={}({}), dimension={}, mips={}, arrays={}, samples={}, "
             "resourceFlags={:#x}, state={:#x}, lifecycle={}, preferredFormat={} ({})",
             GetRRTagSourceName(source), getRRTaggedSignalName(signal), frameIndex, next.resourceAddress,
             next.debugName, next.nativeWidth, next.nativeHeight, next.effectiveWidth, next.effectiveHeight,
             next.extentLeft, next.extentTop, formatName.empty() ? "UNKNOWN" : formatName,
             static_cast<uint32_t>(next.format), static_cast<uint32_t>(next.dimension), next.mipLevels, next.arraySize,
             next.sampleCount, static_cast<uint32_t>(next.resourceFlags), next.state,
             lifecycleName.empty() ? "UNKNOWN" : lifecycleName, isRRPreferredTagFormat(signal, next.format),
             getRRPreferredTagFormat(signal));
}

void StreamlineHooks::logRRSignalTagDiagnostics(uint32_t renderWidth, uint32_t renderHeight)
{
    const RRSignalTagDiagnostics diagnostics = getRRSignalTagDiagnostics();
    LOG_INFO("[RR_TAG_DIAG] snapshot: generation={}, render={}x{}", diagnostics.generation, renderWidth, renderHeight);

    for (size_t i = 0; i < diagnostics.resources.size(); ++i)
    {
        const auto signal = static_cast<RRTaggedSignal>(i);
        const auto& diagnostic = diagnostics.resources[i];
        if (!diagnostic.observed)
        {
            LOG_INFO("[RR_TAG_DIAG] snapshot {}: not observed", getRRTaggedSignalName(signal));
            continue;
        }

        if (!diagnostic.present)
        {
            LOG_INFO("[RR_TAG_DIAG] snapshot {}: cleared, updates={}", getRRTaggedSignalName(signal),
                     diagnostic.updateCount);
            continue;
        }

        const auto formatName = magic_enum::enum_name(diagnostic.format);
        LOG_INFO("[RR_TAG_DIAG] snapshot {}: ptr={}, debugName='{}', effective={}x{}, native={}x{}, "
                 "format={}({}), state={:#x}, updates={}, checkerboard={}, preferredFormat={} ({})",
                 getRRTaggedSignalName(signal), diagnostic.resourceAddress, diagnostic.debugName,
                 diagnostic.effectiveWidth, diagnostic.effectiveHeight, diagnostic.nativeWidth, diagnostic.nativeHeight,
                 formatName.empty() ? "UNKNOWN" : formatName, static_cast<uint32_t>(diagnostic.format),
                 diagnostic.state, diagnostic.updateCount,
                 getRRCheckerboardAssessment(signal, diagnostic, renderWidth, renderHeight),
                 isRRPreferredTagFormat(signal, diagnostic.format), getRRPreferredTagFormat(signal));
    }
}
#include <shaders/reproject/mouseInputs/InputCollection.h>

#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

static std::wstring GetModulePath(HMODULE module)
{
    if (module == nullptr)
        return {};

    wchar_t buffer[MAX_PATH * 2] {};
    const auto len = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::wstring(buffer, len);
}

// Plugin may fail (denylist etc.) and leave pluginJSON empty
static bool HasPluginJson(bool result, const char** pluginJSON)
{
    return result && pluginJSON != nullptr && *pluginJSON != nullptr && **pluginJSON != '\0';
}

// SystemCaps lives inside sl.common's image. When Streamline drops one sl.common copy and keeps another,
// a pointer we got earlier can point into an unmapped (or reused) range, so check before every access.
static bool IsWritableMemory(const void* address, size_t size)
{
    if (address == nullptr)
        return false;

    MEMORY_BASIC_INFORMATION mbi {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;

    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;

    constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & writable) == 0)
        return false;

    const auto end = reinterpret_cast<uintptr_t>(address) + size;
    const auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end <= regionEnd;
}

static bool IsSL1AndFGActive()
{
    const auto& state = State::Instance();

    return state.streamlineVersion.major == 1 && state.activeFgInput == FGInput::DLSSG &&
           state.activeFgOutput != FGOutput::NoFG;
}

// Game's SL1 DLSSG has to stay inert when we use its inputs or provide DLSSG ourselves,
// otherwise its swapchain hooks end up stacked on top of our FG swapchain
static bool IsSL1AndDLSSGManaged()
{
    const auto& state = State::Instance();

    return state.streamlineVersion.major == 1 &&
           (state.activeFgInput == FGInput::DLSSG || state.activeFgOutput == FGOutput::DLSSG);
}

static void PatchSL1PluginJson(nlohmann::json& configJson)
{
    if (!IsSL1AndDLSSGManaged())
        return;

    LOG_DEBUG("Patching SL1 plugin JSON for external FG management");

    if (configJson.contains("/hooks"_json_pointer))
        configJson["hooks"].clear();

    if (configJson.contains("/exclusive_hooks"_json_pointer))
        configJson["exclusive_hooks"].clear();

    if (configJson.contains("/external/feature/tags"_json_pointer))
        configJson["external"]["feature"]["tags"].clear();

    if (configJson.contains("/vsync/supported"_json_pointer))
        configJson["vsync"]["supported"] = true;

    if (configJson.contains("/external/hws/required"_json_pointer))
        configJson["external"]["hws"]["required"] = false;
}

char* StreamlineHooks::trimStreamlineLog(const char* msg)
{
    char* result = (char*) malloc(strlen(msg) + 1);
    if (!result)
        return nullptr;

    strcpy(result, msg);

    size_t length = strlen(result);
    if (length > 0 && result[length - 1] == '\n')
    {
        result[length - 1] = '\0';
    }

    return result;
}

void StreamlineHooks::streamlineLogCallback(sl::LogType type, const char* msg)
{
    if (msg == nullptr)
        return;

    char* trimmed_msg = trimStreamlineLog(msg);
    if (trimmed_msg != nullptr)
    {
        switch (type)
        {
        case sl::LogType::eWarn:
            LOG_WARN("{}", trimmed_msg);
            break;
        case sl::LogType::eInfo:
            LOG_INFO("{}", trimmed_msg);
            break;
        case sl::LogType::eError:
            LOG_ERROR("{}", trimmed_msg);
            break;
        case sl::LogType::eCount:
            LOG_ERROR("{}", trimmed_msg);
            break;
        }

        free(trimmed_msg);
    }

    if (o_logCallback != nullptr)
        o_logCallback(type, msg);
}

static bool IsNeuralHubFgArmed() { return DlssNr::AmdBridge::NeuralHubFgServed(); }

sl::Result StreamlineHooks::hkslInit(const sl::Preferences& pref, uint64_t sdkVersion)
{
    LOG_FUNC();

    sl::Preferences localPref = pref;

    if (localPref.logMessageCallback != &streamlineLogCallback)
        o_logCallback = localPref.logMessageCallback;
    localPref.logLevel = sl::LogLevel::eCount;
    localPref.logMessageCallback = &streamlineLogCallback;

    // renderAPI is optional so need to be careful, should only matter for Vulkan
    renderApi = localPref.renderAPI;

    State::Instance().slFGInputs.reportEngineType(localPref.engine);

    // Treat engine type set in Streamline as ground truth
    if (localPref.engine == sl::EngineType::eUnreal)
        State::Instance().gameQuirks |= GameQuirk::ForceUnrealEngine;

    std::filesystem::path localSlPath(Config::Instance()->MainDllPath.value());
    localSlPath = localSlPath / L"streamline"; // Hardcoded streamline folder

    auto localSlPathStr = localSlPath.wstring();

    std::vector<const wchar_t*> storage;

    // Replace the SL files to allow for MFG
    if (State::Instance().activeFgInput == FGInput::NvngxFG && std::filesystem::exists(localSlPath / L"sl.common.dll"))
    {
        storage.assign(localPref.pathsToPlugins, localPref.pathsToPlugins + localPref.numPathsToPlugins);

        std::filesystem::path pluginsDir;

        // Find the first path that contains sl.common.dll
        // If storage is empty, look in the exe folder. pathsToPlugins is an optional field
        if (storage.empty())
        {
            std::filesystem::path exeFolder = Util::ExePath().parent_path();
            if (std::filesystem::exists(exeFolder / L"sl.common.dll"))
            {
                pluginsDir = exeFolder;
            }
        }
        else
        {
            for (const wchar_t* pathStr : storage)
            {
                if (!pathStr)
                    continue;

                std::filesystem::path p = pathStr;
                if (std::filesystem::exists(p / L"sl.common.dll"))
                {
                    pluginsDir = p;
                    break;
                }
            }
        }

        std::vector<std::string> missingDlls;
        bool hasNewerPlugin = false;

        // If we found the plugins folder, scan its contents
        if (!pluginsDir.empty() && std::filesystem::exists(pluginsDir))
        {
            for (const auto& entry : std::filesystem::directory_iterator(pluginsDir))
            {
                if (!entry.is_regular_file())
                    continue;

                std::wstring filename = entry.path().filename().wstring();

                std::wstring lowerName = filename;
                to_lower_in_place(lowerName);

                // Skip interposer
                if (lowerName == L"sl.interposer.dll")
                    continue;

                const bool isSlDll = lowerName.starts_with(L"sl.") && lowerName.ends_with(L".dll");
                const bool isNvLowLatency = lowerName == L"nvlowlatencyvk.dll";

                if (isSlDll || isNvLowLatency)
                {
                    std::filesystem::path localDllPath = localSlPath / filename;

                    // Check if localSlPath also has this DLL
                    if (!std::filesystem::exists(localDllPath))
                    {
                        missingDlls.push_back(entry.path().filename().string());
                    }
                    else
                    {
                        // Compare versions
                        version_t pluginVer, pluginProdVer;
                        version_t localVer, localProdVer;

                        bool gotPluginVer = Util::GetFileVersion(entry.path().wstring(), &pluginVer, &pluginProdVer);
                        bool gotLocalVer = Util::GetFileVersion(localDllPath.wstring(), &localVer, &localProdVer);

                        if (gotPluginVer && gotLocalVer)
                        {
                            if (localVer > pluginVer)
                            {
                                hasNewerPlugin = true;
                            }
                        }
                    }
                }
            }
        }

        // Insert local path only if a newer plugin was found
        if (hasNewerPlugin)
        {
            LOG_DEBUG("Making the game use local streamline files");

            storage.insert(storage.begin(), localSlPathStr.c_str());
            localPref.pathsToPlugins = storage.data();
            localPref.numPathsToPlugins = (uint32_t) storage.size();

            if (!missingDlls.empty())
            {
                std::string toastMsg = "You are missing the following dlls from the streamline folder:\n";
                for (const auto& missingDll : missingDlls)
                {
                    toastMsg += "- " + missingDll + "\n";
                }

                ImGui::InsertNotification({ ImGuiToastType::Warning, 20000, toastMsg.c_str() });
            }
        }
    }

    const bool neuralHubFg = IsNeuralHubFgArmed();
    const bool useNativeDlssg = neuralHubFg &&
                                State::Instance().activeFgOutput != FGOutput::XeFG &&
                                State::Instance().activeFgOutput != FGOutput::FSRFG;

    State::Instance().gameDlssgNative = useNativeDlssg;

    if (useNativeDlssg)
    {
        if (State::Instance().GameDlssgBlocksOwnFG())
            LOG_INFO("hkslInit: the game's DLSS-FG runs on NeuralHub, OptiScaler's DLSSG output stays off");

        std::vector<sl::Feature> localFeaturesToLoad(pref.featuresToLoad, pref.featuresToLoad + pref.numFeaturesToLoad);
        if (std::find(localFeaturesToLoad.begin(), localFeaturesToLoad.end(), sl::kFeatureDLSS_G) == localFeaturesToLoad.end())
        {
            localFeaturesToLoad.push_back(sl::kFeatureDLSS_G);
            LOG_INFO("hkslInit: added sl::kFeatureDLSS_G to featuresToLoad for NeuralHub DLSS-FG");
        }

        localPref.featuresToLoad = localFeaturesToLoad.data();
        localPref.numFeaturesToLoad = (uint32_t) localFeaturesToLoad.size();

        return o_slInit(localPref, sdkVersion);
    }
    else if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgOutput == FGOutput::DLSSG)
    {
        std::vector<sl::Feature> localFeaturesToLoad(pref.featuresToLoad, pref.featuresToLoad + pref.numFeaturesToLoad);
        std::erase(localFeaturesToLoad, sl::kFeatureDLSS_G);

        localPref.featuresToLoad = localFeaturesToLoad.data();
        localPref.numFeaturesToLoad = (uint32_t) localFeaturesToLoad.size();

        // return so that localFeaturesToLoad is valid
        return o_slInit(localPref, sdkVersion);
    }

    // bool hookSetTag =
    //     (State::Instance().activeFgInput == FGInput::NvngxFG || State::Instance().activeFgInput == FGInput::DLSSG);

    // if (hookSetTag)
    //     localPref->flags &= ~(sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins);

    // To prevent mixed up OTA situations
    if (Config::Instance()->DisableOTA.value_or_default())
    {
        localPref.flags &= ~sl::PreferenceFlags::eAllowOTA;
        localPref.flags &= ~sl::PreferenceFlags::eLoadDownloadedPlugins;
    }

    return o_slInit(localPref, sdkVersion);
}

sl::Result StreamlineHooks::hkslIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapterInfo)
{
    void* retAddr = _ReturnAddress();
    LOG_INFO("slIsFeatureSupported: feature {} caller {}", (int) feature, retAddr);

    if (feature == sl::kFeatureDLSS_G || feature == sl::kFeatureDLSS_RR)
        return sl::Result::eOk;

    auto result = o_slIsFeatureSupported(feature, adapterInfo);
    LOG_INFO("slIsFeatureSupported: feature {} result: {} caller {}", (int) feature, (int) result, retAddr);
    return result;
}

sl::Result StreamlineHooks::hkslIsFeatureLoaded(sl::Feature feature, bool& loaded)
{
    if (feature == sl::kFeatureDLSS_G || feature == sl::kFeatureDLSS_RR)
    {
        loaded = true;
        return sl::Result::eOk;
    }

    return o_slIsFeatureLoaded(feature, loaded);
}

sl::Result StreamlineHooks::hkslGetFeatureRequirements(sl::Feature feature, sl::FeatureRequirements& requirements)
{
    LOG_INFO("slGetFeatureRequirements: feature {}", (int) feature);

    if (feature == sl::kFeatureDLSS_G)
        return sl::Result::eOk;

    auto result = o_slGetFeatureRequirements(feature, requirements);
    LOG_INFO("slGetFeatureRequirements: feature {} result: {} flags: {} driverReq: {}", (int) feature, (int) result,
             (int) requirements.flags, requirements.driverVersionRequired.toStr());
    return result;
}

sl::Result StreamlineHooks::hkslGetFeatureVersion(sl::Feature feature, sl::FeatureVersion& version)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        version.versionSL = { State::Instance().streamlineVersion.major, State::Instance().streamlineVersion.minor,
                              State::Instance().streamlineVersion.patch };
        version.versionNGX = { 4, 2, 0 };

        return sl::Result::eOk;
    }

    return o_slGetFeatureVersion(feature, version);
}

// Games usually ask for the max before our FG swapchain exists, so the output can't tell us yet
static int OutputMaxInterpolationCount()
{
    auto& state = State::Instance();

    if (auto fg = state.CurrentFG(); fg != nullptr && fg->GetMaxInterpolationCount() > 1)
        return fg->GetMaxInterpolationCount();

    if (state.activeFgOutput == FGOutput::XeFG)
    {
        static int xefgMax = 0;
        static bool xefgQueried = false;

        // GetState can be called every frame, only try once
        if (!xefgQueried && state.currentD3D12Device != nullptr)
        {
            xefgQueried = true;
            xefgMax = XeFG_Dx12::QueryMaxInterpolationCount(state.currentD3D12Device);
        }

        // Before XeFG can be asked, the most the unlock allows
        return xefgMax > 0 ? xefgMax : Config::XeFGMaxInterpolations;
    }

    return 1;
}

// Reports Opti's FG output capabilities to the game when DLSSG is used as FG Input
static void FillDlssgInputState(sl::DLSSGState& state)
{
    auto fg = State::Instance().CurrentFG();

    if (fg != nullptr && fg->IsActive() && !fg->IsPaused())
        state.numFramesActuallyPresented = fg->GetInterpolatedFrameCount() + 1;
    else
        state.numFramesActuallyPresented = 1;

    if (state.structVersion >= sl::kStructVersion2)
        state.numFramesToGenerateMax = OutputMaxInterpolationCount();
}

sl::Result StreamlineHooks::dummy_slDLSSGGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                                  const sl::DLSSGOptions* options)
{
    if (o_slDLSSGGetState != nullptr &&
        State::Instance().activeFgOutput != FGOutput::XeFG &&
        State::Instance().activeFgOutput != FGOutput::FSRFG)
    {
        auto res = o_slDLSSGGetState(viewport, state, options);
        if (res == sl::Result::eOk)
            return res;
    }

    FillDlssgInputState(state);
    state.estimatedVRAMUsageInBytes = 300 * 1024 * 1024;

    if (state.structVersion >= sl::kStructVersion2)
        state.bIsVsyncSupportAvailable = sl::Boolean::eTrue;

    return sl::Result::eOk;
}

sl::Result StreamlineHooks::dummy_slDLSSGSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    auto& state = State::Instance();

    // Only reading v1 fields, safe for any struct version
    const bool enabled = options.mode != sl::DLSSGMode::eOff;
    const int count = std::max(static_cast<int>(options.numFramesToGenerate), 1);

    if (enabled != state.dlssgInputGameEnabled || count != state.dlssgInputGameInterpolationCount)
    {
        LOG_INFO("Game requested DLSSG mode: {}, numFramesToGenerate: {}", magic_enum::enum_name(options.mode),
                 options.numFramesToGenerate);

        if (enabled && !state.dlssgInputGameEnabled && Config::Instance()->FGFollowsGame())
            state.fgChanged = true;

        state.dlssgInputGameEnabled = enabled;
        state.dlssgInputGameInterpolationCount = count;
    }

    // Still a dummy as far as the provider is concerned - the game is told the call succeeded and
    // OptiScaler keeps driving frame generation itself. But the request is the game's own, and it is
    // the only copy of it that exists on this path, so it has to be kept.
    RecordGameDlssgOptions(viewport, options);

    if (o_slDLSSGSetOptions != nullptr &&
        State::Instance().activeFgOutput != FGOutput::XeFG &&
        State::Instance().activeFgOutput != FGOutput::FSRFG)
    {
        return hkslDLSSGSetOptions(viewport, options);
    }

    return sl::Result::eOk;
}

sl::Result StreamlineHooks::hkslGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        if (strcmp(functionName, "slDLSSGSetOptions") == 0)
        {
            function = &dummy_slDLSSGSetOptions;

            return sl::Result::eOk;
        }

        if (strcmp(functionName, "slDLSSGGetState") == 0)
        {
            function = &dummy_slDLSSGGetState;

            return sl::Result::eOk;
        }
    }

    return o_slGetFeatureFunction(feature, functionName, function);
}

sl::Result StreamlineHooks::hkslSetFeatureLoaded(sl::Feature feature, bool loaded)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        return sl::Result::eOk;
    }

    return o_slSetFeatureLoaded(feature, loaded);
}

sl::Result StreamlineHooks::hkslSetTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                                       uint32_t numTags, sl::CommandBuffer* cmdBuffer)
{
    if (renderApi == sl::RenderAPI::eD3D11)
    {
        LOG_ERROR("hkslSetTag doesn't support DX11");
        return o_slSetTag(viewport, tags, numTags, cmdBuffer);
    }

    if (renderApi == sl::RenderAPI::eCount)
        LOG_WARN("Incomplete Streamline hooks");

    if (tags == nullptr)
    {
        LOG_WARN("Game trying to remove a tag");
        return o_slSetTag(viewport, tags, numTags, cmdBuffer);
    }

    // This rule is about what OptiScaler reports as FG input, and it must not gate the
    // inventory probes in the loop below. The batch it matches is a DLSS call rather than an
    // FG frame, but it is still a tagging call, and it is often the only one that carries the
    // title's RR tags - emissive, hit distance, the title's own linear depth. Returning here
    // loses the discovery of inputs the RR path later looks for, which is a silent
    // degradation: the input is simply never bound, exactly as if the title never published
    // it. Probe every batch; suppress only the tagging.
    bool skipFgTagging = false;
    if (State::Instance().activeFgInput == FGInput::DLSSG &&
        State::Instance().gameQuirks[GameQuirk::IgnoreTagsWithoutHudlessForFG])
    {
        bool hasDepth = false;
        bool hasMVs = false;
        bool hasHudless = false;

        for (uint32_t i = 0; i < numTags; i++)
        {
            if (tags[i].resource == nullptr || tags[i].resource->native == nullptr)
                continue;

            if (tags[i].type == sl::kBufferTypeDepth)
                hasDepth = true;

            if (tags[i].type == sl::kBufferTypeMotionVectors)
                hasMVs = true;

            if (tags[i].type == sl::kBufferTypeHUDLessColor)
                hasHudless = true;
        }

        // Try to skip a DLSS call. The tags still reach Streamline untouched, so only the
        // bookkeeping below is skipped.
        if (hasDepth && hasMVs && !hasHudless)
        {
            LOG_DEBUG("Skipping the FG tagging of potential DLSS resources");
            skipFgTagging = true;
        }
    }

    for (uint32_t i = 0; i < numTags; i++)
    {
        if (renderApi == sl::RenderAPI::eD3D12)
        {
            probeSLResourceTag(tags[i], UINT32_MAX, static_cast<uint32_t>(viewport), RRTagSource::SetTag);
            probeRRResourceTag(tags[i], UINT32_MAX, static_cast<uint32_t>(viewport), RRTagSource::SetTag);
        }

        const auto typeEnum = (BufferType) tags[i].type;

        if (tags[i].resource == nullptr || tags[i].resource->native == nullptr)
        {
            LOG_TRACE("Resource of type: {} is null, continuing", magic_enum::enum_name(typeEnum));
            continue;
        }

        // A skipped batch carries no hudless colour by definition, so the state repair below -
        // which exists for that resource - has nothing to act on, and what remains is the FG
        // reporting this rule is about.
        if (skipFgTagging)
            continue;

        // Cyberpunk hudless state fix for RDNA 2
        if (State::Instance().gameQuirks & GameQuirk::CyberpunkHudlessState && renderApi != sl::RenderAPI::eVulkan &&
            tags[i].resource->state ==
                (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
            tags[i].type == sl::kBufferTypeHUDLessColor)
        {
            tags[i].resource->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            LOG_TRACE("Changing hudless resource state");
        }

        if (State::Instance().activeFgInput == FGInput::DLSSG &&
            (tags[i].type == sl::kBufferTypeHUDLessColor || tags[i].type == sl::kBufferTypeDepth ||
             tags[i].type == sl::kBufferTypeHiResDepth || tags[i].type == sl::kBufferTypeLinearDepth ||
             tags[i].type == sl::kBufferTypeMotionVectors || tags[i].type == sl::kBufferTypeUIColorAndAlpha ||
             tags[i].type == sl::kBufferTypeBidirectionalDistortionField))
        {
            State::Instance().slFGInputs.reportResource(tags[i], cmdBuffer, 0);
        }
        else if (State::Instance().activeFgInput == FGInput::NvngxFG)
        {
            LOG_TRACE("Tagging resource of type: {}", magic_enum::enum_name(typeEnum));
        }
    }

    auto result = o_slSetTag(viewport, tags, numTags, cmdBuffer);
    return result;
}

sl::Result StreamlineHooks::hkslSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport,
                                               const sl::ResourceTag* resources, uint32_t numResources,
                                               sl::CommandBuffer* cmdBuffer)
{
    if (renderApi == sl::RenderAPI::eD3D11)
    {
        LOG_ERROR("hkslSetTagForFrame doesn't support DX11");
        return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    }

    if (renderApi == sl::RenderAPI::eCount)
        LOG_WARN("Incomplete Streamline hooks");

    if (resources == nullptr)
    {
        LOG_WARN("Game trying to remove a tag");
        return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    }

    LOG_DEBUG("frameIndex: {}", static_cast<uint32_t>(frame));

    // See hkslSetTag: the FG rule suppresses OptiScaler's FG bookkeeping, not the RR tag
    // inventory, which this batch may be the only carrier of.
    bool skipFgTagging = false;
    if (State::Instance().activeFgInput == FGInput::DLSSG &&
        State::Instance().gameQuirks[GameQuirk::IgnoreTagsWithoutHudlessForFG])
    {
        bool hasDepth = false;
        bool hasMVs = false;
        bool hasHudless = false;

        for (uint32_t i = 0; i < numResources; i++)
        {
            if (resources[i].resource == nullptr || resources[i].resource->native == nullptr)
                continue;

            if (resources[i].type == sl::kBufferTypeDepth)
                hasDepth = true;

            if (resources[i].type == sl::kBufferTypeMotionVectors)
                hasMVs = true;

            if (resources[i].type == sl::kBufferTypeHUDLessColor)
                hasHudless = true;
        }

        // Try to skip a DLSS call
        if (hasDepth && hasMVs && !hasHudless)
        {
            LOG_DEBUG("Skipping the FG tagging of potential DLSS resources");
            skipFgTagging = true;
        }
    }

    for (uint32_t i = 0; i < numResources; i++)
    {
        if (renderApi == sl::RenderAPI::eD3D12)
        {
            probeSLResourceTag(resources[i], static_cast<uint32_t>(frame), static_cast<uint32_t>(viewport),
                               RRTagSource::SetTagForFrame);
            probeRRResourceTag(resources[i], static_cast<uint32_t>(frame), static_cast<uint32_t>(viewport),
                               RRTagSource::SetTagForFrame);
        }

        const auto typeEnum = (BufferType) resources[i].type;

        if (resources[i].resource == nullptr || resources[i].resource->native == nullptr)
        {
            LOG_TRACE("Resource of type: {} is null, continuing", magic_enum::enum_name(typeEnum));
            continue;
        }

        if (skipFgTagging)
            continue;

        if (State::Instance().activeFgInput == FGInput::DLSSG &&
            (resources[i].type == sl::kBufferTypeHUDLessColor || resources[i].type == sl::kBufferTypeDepth ||
             resources[i].type == sl::kBufferTypeHiResDepth || resources[i].type == sl::kBufferTypeLinearDepth ||
             resources[i].type == sl::kBufferTypeMotionVectors || resources[i].type == sl::kBufferTypeUIColorAndAlpha ||
             resources[i].type == sl::kBufferTypeBidirectionalDistortionField))
        {
            State::Instance().slFGInputs.reportResource(resources[i], cmdBuffer, (uint32_t) frame);
        }
        else if (State::Instance().activeFgInput == FGInput::NvngxFG)
        {
            LOG_TRACE("Tagging resource of type: {}", magic_enum::enum_name(typeEnum));
        }
    }

    auto result = o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    return result;
}

sl::Result StreamlineHooks::hkslEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame,
                                                const sl::BaseStructure** inputs, uint32_t numInputs,
                                                sl::CommandBuffer* cmdBuffer)
{
    LOG_DEBUG("slEvaluateFeature: feature {}", (int) feature);
    uint32_t activeViewport = UINT32_MAX;
    if (numInputs > 0 && inputs != nullptr)
    {
        for (uint32_t i = 0; i < numInputs; ++i)
        {
            if (inputs[i] != nullptr && inputs[i]->structType == sl::ViewportHandle::s_structType)
            {
                activeViewport = static_cast<uint32_t>(*reinterpret_cast<const sl::ViewportHandle*>(inputs[i]));
                break;
            }
        }
    }

    const ScopedRRActiveEvaluationFrame activeEvaluationFrame(static_cast<uint32_t>(frame), activeViewport);
    LOG_DEBUG("frameIndex: {}", static_cast<uint32_t>(frame));

    // ResourceTag inputs to slEvaluateFeature are local to this evaluation and
    // must not replace tags submitted through slSetTag/slSetTagForFrame. Overlay
    // them while the intercepted feature runs, then restore the global entries.
    constexpr size_t rrSignalCount = static_cast<size_t>(RRTaggedSignal::Count);
    std::array<bool, rrSignalCount> localTagOverrides {};
    std::array<RRTaggedResourceDiagnostic, rrSignalCount> savedGlobalDiagnostics {};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, rrSignalCount> savedGlobalResources {};
    if (renderApi == sl::RenderAPI::eD3D12 && numInputs > 0 && inputs != nullptr)
    {
        std::scoped_lock lock(rrSignalTagMutex);
        for (uint32_t i = 0; i < numInputs; ++i)
        {
            if (inputs[i] == nullptr || inputs[i]->structType != sl::ResourceTag::s_structType)
                continue;

            const auto* tag = reinterpret_cast<const sl::ResourceTag*>(inputs[i]);
            RRTaggedSignal signal;
            if (!TryGetRRTaggedSignal(tag->type, signal))
                continue;

            const size_t signalIndex = static_cast<size_t>(signal);
            if (localTagOverrides[signalIndex])
                continue;

            localTagOverrides[signalIndex] = true;
            savedGlobalDiagnostics[signalIndex] = rrSignalTagDiagnostics.resources[signalIndex];
            savedGlobalResources[signalIndex] = rrTaggedD3D12Resources[signalIndex];
        }
    }

    if (numInputs > 0 && inputs != nullptr)
    {
        for (uint32_t i = 0; i < numInputs; i++)
        {
            if (inputs[i] == nullptr)
                continue;

            if (inputs[i]->structType == sl::ResourceTag::s_structType)
            {
                auto tag = (const sl::ResourceTag*) inputs[i];

                if (renderApi == sl::RenderAPI::eD3D12)
                {
                    probeSLResourceTag(*tag, static_cast<uint32_t>(frame), activeViewport,
                                       RRTagSource::EvaluateFeature);
                    probeRRResourceTag(*tag, static_cast<uint32_t>(frame), activeViewport,
                                       RRTagSource::EvaluateFeature);
                }

                if (State::Instance().activeFgInput == FGInput::DLSSG &&
                    (tag->type == sl::kBufferTypeHUDLessColor || tag->type == sl::kBufferTypeDepth ||
                     tag->type == sl::kBufferTypeHiResDepth || tag->type == sl::kBufferTypeLinearDepth ||
                     tag->type == sl::kBufferTypeMotionVectors || tag->type == sl::kBufferTypeUIColorAndAlpha ||
                     tag->type == sl::kBufferTypeBidirectionalDistortionField))
                {
                    State::Instance().slFGInputs.reportResource(*tag, cmdBuffer, (uint32_t) frame);
                }
            }
        }
    }

    auto result = o_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);

    if (renderApi == sl::RenderAPI::eD3D12)
    {
        std::scoped_lock lock(rrSignalTagMutex);
        bool resourcesChanged = false;
        for (size_t i = 0; i < rrSignalCount; ++i)
        {
            if (localTagOverrides[i])
            {
                rrSignalTagDiagnostics.resources[i] = std::move(savedGlobalDiagnostics[i]);
                rrTaggedD3D12Resources[i] = std::move(savedGlobalResources[i]);
                resourcesChanged = true;
            }

            const auto& diagnostic = rrSignalTagDiagnostics.resources[i];
            const bool belongsToEvaluation =
                diagnostic.viewport == activeViewport &&
                (diagnostic.frameIndex == UINT32_MAX || diagnostic.frameIndex == static_cast<uint32_t>(frame));
            if (belongsToEvaluation && diagnostic.lifecycle == sl::ResourceLifecycle::eValidUntilEvaluate &&
                rrTaggedD3D12Resources[i])
            {
                // The declared lifetime ends as this evaluation returns. Keep
                // the last metadata for diagnostics, but release the retained
                // native resource so it cannot be rebound by a later frame.
                rrTaggedD3D12Resources[i].Reset();
                resourcesChanged = true;
            }
        }

        if (resourcesChanged)
            ++rrSignalTagDiagnostics.generation;
    }

    return result;
}

sl::Result StreamlineHooks::hkslAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature,
                                                  const sl::ViewportHandle& viewport)
{
    if (feature == sl::kFeatureDLSS_G)
        return sl::Result::eOk;

    return o_slAllocateResources(cmdBuffer, feature, viewport);
}

sl::Result StreamlineHooks::hkslFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport)
{
    if (feature == sl::kFeatureDLSS_G)
        return sl::Result::eOk;

    return o_slFreeResources(feature, viewport);
}

sl::Result StreamlineHooks::hkslGetNativeInterface(void* proxyInterface, void** baseInterface)
{
    LOG_FUNC();
    auto result = o_slGetNativeInterface(proxyInterface, baseInterface);
    return result;
}

sl::Result StreamlineHooks::hkslSetD3DDevice(void* d3dDevice)
{
    LOG_FUNC();
    auto result = o_slSetD3DDevice(d3dDevice);
    return result;
}

void StreamlineHooks::streamlineLogCallback_sl1(sl1::LogType type, const char* msg)
{
    if (msg == nullptr)
        return;

    char* trimmed_msg = trimStreamlineLog(msg);

    if (trimmed_msg != nullptr)
    {
        switch (type)
        {
        case sl1::LogType::eLogTypeWarn:
            LOG_WARN("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeInfo:
            LOG_INFO("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeError:
            LOG_ERROR("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeCount:
            LOG_ERROR("{}", trimmed_msg);
            break;
        }

        free(trimmed_msg);
    }

    if (o_logCallback_sl1)
        o_logCallback_sl1(type, msg);
}

bool StreamlineHooks::hkslInit_sl1(const sl1::Preferences& pref, int applicationId)
{
    LOG_FUNC();

    sl1::Preferences localPref = pref;

    if (localPref.logMessageCallback != &streamlineLogCallback_sl1)
        o_logCallback_sl1 = localPref.logMessageCallback;
    localPref.logLevel = sl1::LogLevel::eLogLevelCount;
    localPref.logMessageCallback = &streamlineLogCallback_sl1;
    return o_slInit_sl1(localPref, applicationId);
}

bool StreamlineHooks::hkslSetTag_sl1(const sl1::Resource* resource, sl1::BufferType tag, uint32_t id,
                                     const sl1::Extent* extent)
{
    if (IsSL1AndFGActive())
        State::Instance().s_sl1FGInputs.setTag(resource, tag, id, extent);

    return o_slSetTag_sl1(resource, tag, id, extent);
}

bool StreamlineHooks::hkslSetConstants_sl1(const sl1::Constants& values, uint32_t frameIndex, uint32_t id)
{
    std::scoped_lock lock(setConstantsMutex);

    LOG_TRACE("SL1 slSetConstants frameIndex: {}, id: {}", frameIndex, id);

    if (IsSL1AndFGActive())
        State::Instance().s_sl1FGInputs.setConstants(values, frameIndex, id);

    return o_slSetConstants_interposer_sl1(values, frameIndex, id);
}

bool StreamlineHooks::hkslEvaluateFeature_sl1(sl1::CommandBuffer* cmdBuffer, sl1::Feature feature, uint32_t frameIndex,
                                              uint32_t id)
{
    LOG_TRACE("SL1 slEvaluateFeature feature: {}, frameIndex: {}, id: {}", magic_enum::enum_name(feature), frameIndex,
              id);

    if (IsSL1AndFGActive() && feature == sl1::Feature::eFeatureReflex)
    {
        const auto marker = (sl1::ReflexMarker) id;

        if (marker == sl1::ReflexMarker::eReflexMarkerRenderSubmitStart)
        {
            State::Instance().s_sl1FGInputs.evaluateState();
            State::Instance().s_sl1FGInputs.evaluateFeature(cmdBuffer, feature, frameIndex, id);
        }
        else if (marker == sl1::ReflexMarker::eReflexMarkerPresentStart)
        {
            State::Instance().s_sl1FGInputs.markPresent(frameIndex);
        }
    }

    return o_slEvaluateFeature_sl1(cmdBuffer, feature, frameIndex, id);
}

// Always re-read from params: the caps struct belongs to the sl.common copy Streamline currently uses,
// which can change (game plugins vs OTA plugins), so a cached pointer can go stale.
void StreamlineHooks::hookSystemCaps(sl::param::IParameters* params)
{
    if (params == nullptr)
        return;

    if (State::Instance().streamlineVersion.major > 1)
    {
        SystemCaps* caps = nullptr;
        sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &caps);

        if (caps != nullptr && !IsWritableMemory(caps, sizeof(SystemCaps)))
        {
            LOG_WARN("SystemCaps pointer {:X} is not valid memory, skipping arch spoofing", (size_t) caps);
            caps = nullptr;
        }

        systemCaps = caps;
    }
    else if (State::Instance().streamlineVersion.major == 1)
    {
        // This should be Streamline 1.5 as previous versions don't even have slOnPluginLoad
        LOG_TRACE("Attempting to get system caps for Streamline v1, this could fail depending on the exact version");

        SystemCapsSl15* caps = nullptr;
        sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &caps);

        if (caps != nullptr && !IsWritableMemory(caps, sizeof(SystemCapsSl15)))
        {
            LOG_WARN("SystemCaps (SL1) pointer {:X} is not valid memory, skipping arch spoofing", (size_t) caps);
            caps = nullptr;
        }

        systemCapsSl15 = caps;
    }
}

uint32_t StreamlineHooks::getSystemCapsArch(SystemCaps* altSystemCaps)
{
    uint32_t highestArch = 0;

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (!fakenvapi::isUsingAsMainNvapi() && primaryGpu.vendorId == VendorId::Nvidia)
    {
        if (State::Instance().streamlineVersion.major > 1)
        {
            auto caps = altSystemCaps != nullptr ? altSystemCaps : systemCaps;
            if (caps && IsWritableMemory(caps, sizeof(SystemCaps)))
            {
                for (auto& adapter : caps->adapters)
                {
                    if (adapter.architecture > highestArch)
                        highestArch = adapter.architecture;
                }
            }
        }
        else if (State::Instance().streamlineVersion.major == 1)
        {
            if (systemCapsSl15 && IsWritableMemory(systemCapsSl15, sizeof(SystemCapsSl15)))
            {
                const auto count = (std::min) (systemCapsSl15->gpuCount, kMaxNumSupportedGPUs);
                for (uint32_t i = 0; i < count; i++)
                {
                    if (systemCapsSl15->architecture[i] > highestArch)
                        highestArch = systemCapsSl15->architecture[i];
                }
            }
        }
    }

    // By default spoof Pascal, gets Reflex but not DLSSD
    // Could be problematic if not using fakenvapi but nvapi might not be initialized yet
    if (highestArch == 0)
        highestArch = NV_GPU_ARCHITECTURE_GP100;

    return highestArch;
}

void StreamlineHooks::setArch(uint32_t arch, SystemCaps* altSystemCaps)
{
    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    // altSystemCaps has to be sl2+
    if (State::Instance().streamlineVersion.major > 1 || altSystemCaps)
    {
        // Assumes that altCaps are always for SL2+
        auto caps = altSystemCaps != nullptr ? altSystemCaps : systemCaps;
        if (caps && !IsWritableMemory(caps, sizeof(SystemCaps)))
        {
            LOG_WARN("SystemCaps at {:X} is no longer valid, not writing arch", (size_t) caps);

            if (caps == systemCaps)
                systemCaps = nullptr;

            caps = nullptr;
        }

        if (caps)
        {
            const auto count = (std::min) (caps->gpuCount, kMaxNumSupportedGPUs);
            for (uint32_t i = 0; i < count; i++)
            {
                caps->adapters[i].architecture = arch;
                caps->adapters[i].vendor = VendorId::Nvidia;
            }

            if (fakenvapi::isUsingAsMainNvapi() || primaryGpu.vendorId != VendorId::Nvidia)
                caps->driverVersionMajor = 999;

            caps->hwsSupported = true;
        }
    }
    else if (State::Instance().streamlineVersion.major == 1)
    {
        if (systemCapsSl15 && !IsWritableMemory(systemCapsSl15, sizeof(SystemCapsSl15)))
        {
            LOG_WARN("SystemCaps (SL1) at {:X} is no longer valid, not writing arch", (size_t) systemCapsSl15);
            systemCapsSl15 = nullptr;
        }

        if (systemCapsSl15)
        {
            const auto count = (std::min) (systemCapsSl15->gpuCount, kMaxNumSupportedGPUs);
            for (uint32_t i = 0; i < count; i++)
                systemCapsSl15->architecture[i] = arch;

            if (fakenvapi::isUsingAsMainNvapi() || primaryGpu.vendorId != VendorId::Nvidia)
                systemCapsSl15->driverVersionMajor = 999;

            systemCapsSl15->hwSchedulingEnabled = true;
        }
    }
}

// Spoof arch based on feature and current arch
void StreamlineHooks::spoofArch(uint32_t currentArch, sl::Feature feature, SystemCaps* altSystemCaps)
{
    constexpr uint32_t maxArch = 0xFFFFFFFF;

    // Don't change arch for DLSS/DLSSD with turing and above
    if (feature == sl::kFeatureDLSS)
    {
        if (currentArch < NV_GPU_ARCHITECTURE_TU100)
            return setArch(maxArch, altSystemCaps);
    }

    // Don't spoof DLSSD at all
    else if (feature == sl::kFeatureDLSS_RR)
    {
        return;
    }

    // Don't change arch for DLSSG with ada and above
    else if (feature == sl::kFeatureDLSS_G)
    {
        if (State::Instance().activeFgNvngx != FGNvngxReplacement::None)
        {
            if (!Nvngx_FG::isDx12Available() && !Nvngx_FG::isVulkanAvailable())
                return setArch(0, altSystemCaps);
        }

        if (currentArch < NV_GPU_ARCHITECTURE_AD100)
            return setArch(maxArch, altSystemCaps);
    }

    else if (feature == sl::kFeatureReflex || feature == sl::kFeaturePCL)
    {
        if (fakenvapi::isUsingAsMainNvapi())
            return setArch(maxArch, altSystemCaps);
    }
}

bool StreamlineHooks::hkdlss_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                            const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureDLSS);
    }

    auto result = o_dlss_slOnPluginLoad != nullptr && o_dlss_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    if (!HasPluginJson(result, pluginJSON))
        return result;

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON, nullptr, false);
    if (configJson.is_discarded())
    {
        LOG_ERROR("Failed to parse plugin JSON");
        return result;
    }

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslDLSSGetOptimalSettings(const sl::DLSSOptions& options,
                                                       sl::DLSSOptimalSettings& settings)
{
    if (o_slDLSSGetOptimalSettings == nullptr)
        return sl::Result::eErrorFeatureMissing;

    static bool modesBroken = false;

    auto localOptions = options;

    if (localOptions.mode == sl::DLSSMode::eOff)
        modesBroken = true;

    if (modesBroken)
    {
        if (localOptions.mode == sl::DLSSMode::eMaxPerformance)
            localOptions.mode = sl::DLSSMode::eUltraPerformance;
        else if (localOptions.mode == sl::DLSSMode::eBalanced)
            localOptions.mode = sl::DLSSMode::eMaxPerformance;
        else if (localOptions.mode == sl::DLSSMode::eMaxQuality)
            localOptions.mode = sl::DLSSMode::eBalanced;
        else if (localOptions.mode == sl::DLSSMode::eUltraQuality)
            localOptions.mode = sl::DLSSMode::eMaxQuality;
        else if (localOptions.mode == sl::DLSSMode::eUltraPerformance)
            localOptions.mode = sl::DLSSMode::eDLAA;
    }

    return o_slDLSSGetOptimalSettings(localOptions, settings);
}

bool StreamlineHooks::hkdlssg_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                             const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    const bool neuralHubFg = IsNeuralHubFgArmed();

    bool shouldSpoofArch =
        Config::Instance()->StreamlineSpoofing.value_or_default() &&
        (State::Instance().activeFgInput == FGInput::NvngxFG || State::Instance().activeFgInput == FGInput::DLSSG || neuralHubFg);

    uint32_t currentArch = 0;
    if (shouldSpoofArch)
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureDLSS_G);
    }

    auto result = o_dlssg_slOnPluginLoad != nullptr && o_dlssg_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (shouldSpoofArch)
        setArch(currentArch);

    if (!HasPluginJson(result, pluginJSON))
        return result;

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON, nullptr, false);
    if (configJson.is_discarded())
    {
        LOG_ERROR("Failed to parse plugin JSON");
        return result;
    }

    const bool useNativeDlssg = neuralHubFg &&
                                State::Instance().activeFgOutput != FGOutput::XeFG &&
                                State::Instance().activeFgOutput != FGOutput::FSRFG;

    // Kill the DLSSG streamline swapchain hooks
    if (!useNativeDlssg && (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgOutput == FGOutput::DLSSG))
    {
        if (configJson.contains("/hooks"_json_pointer))
            configJson["hooks"].clear();

        if (configJson.contains("/exclusive_hooks"_json_pointer))
            configJson["exclusive_hooks"].clear();

        if (configJson.contains("/external/feature/tags"_json_pointer))
            configJson["external"]["feature"]["tags"].clear(); // We handle the DLSSG resources

        if (configJson.contains("/external/vk/device/queues/compute/count"_json_pointer))
            configJson["external"]["vk"]["device"]["queues"]["compute"]["count"] = 0;

        if (configJson.contains("/external/vk/device/queues/graphics/count"_json_pointer))
            configJson["external"]["vk"]["device"]["queues"]["graphics"]["count"] = 0;

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgInput == FGInput::NvngxFG || neuralHubFg)
    {
        if (configJson.contains("/vsync/supported"_json_pointer))
            configJson["vsync"]["supported"] = true; // disable eVSyncOffRequired

        if (configJson.contains("/external/hws/required"_json_pointer))
            configJson["external"]["hws"]["required"] = false; // disable eHardwareSchedulingRequired

        // if (configJson.contains("/external/vk/opticalflow/supported"_json_pointer))
        //     configJson["external"]["vk"]["opticalflow"]["supported"] = true;
    }

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

const char* StreamlineHooks::hkdlssg_slGetPluginJSONConfig_sl1()
{
    if (o_dlssg_slGetPluginJSONConfig_sl1 == nullptr)
        return nullptr;

    static std::string patchedConfig;

    const char* originalConfig = o_dlssg_slGetPluginJSONConfig_sl1();

    if (originalConfig == nullptr)
        return originalConfig;

    try
    {
        auto configJson = nlohmann::json::parse(originalConfig);

        LOG_DEBUG("SL1 DLSSG JSON before patch: {}", configJson.dump());

        PatchSL1PluginJson(configJson);
        // RemoveSL1DLSSGHookEntriesRecursive(configJson);

        patchedConfig = configJson.dump();

        LOG_DEBUG("SL1 DLSSG JSON after patch: {}", patchedConfig);

        return patchedConfig.c_str();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to patch SL1 DLSSG JSON config: {}", e.what());
        return originalConfig;
    }
}

bool StreamlineHooks::hklocal_dlssg_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                                   const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    bool shouldSpoofArch = Config::Instance()->StreamlineSpoofing.value_or_default();

    uint32_t currentArch = 0;
    SystemCaps* localSystemCaps = nullptr;
    if (shouldSpoofArch)
    {
        sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &localSystemCaps);

        if (localSystemCaps && !IsWritableMemory(localSystemCaps, sizeof(SystemCaps)))
            localSystemCaps = nullptr;

        if (localSystemCaps)
        {
            currentArch = getSystemCapsArch(localSystemCaps);
            spoofArch(currentArch, sl::kFeatureDLSS_G, localSystemCaps);
        }
    }

    // Once per load: whether the architecture the plugin is about to check was spoofed (the DLSSG output on AMD
    // depends on it).
    if (localSystemCaps)
        LOG_INFO("local sl.dlss_g slOnPluginLoad: {} GPU(s), adapter 0 vendor {:x} arch {:x} (real {:x}), driver {}, hws {}",
                 localSystemCaps->gpuCount, (uint32_t) localSystemCaps->adapters[0].vendor,
                 localSystemCaps->adapters[0].architecture, currentArch, localSystemCaps->driverVersionMajor,
                 localSystemCaps->hwsSupported);
    else
        LOG_WARN("local sl.dlss_g slOnPluginLoad: no system caps (spoofing {}), its hardware check sees the real GPU",
                 shouldSpoofArch);

    auto result =
        o_local_dlssg_slOnPluginLoad != nullptr && o_local_dlssg_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (shouldSpoofArch && localSystemCaps)
        setArch(currentArch, localSystemCaps);

    if (!HasPluginJson(result, pluginJSON))
        return result;

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON, nullptr, false);
    if (configJson.is_discarded())
    {
        LOG_ERROR("Failed to parse plugin JSON");
        return result;
    }

    if (configJson.contains("/external/hws/required"_json_pointer))
        configJson["external"]["hws"]["required"] = false; // disable eHardwareSchedulingRequired

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslSetConstants(const sl::Constants& values, const sl::FrameToken& frame,
                                             const sl::ViewportHandle& viewport)
{
    LOG_TRACE("called with frameIndex: {}, viewport: {}", (unsigned int) frame, (unsigned int) viewport);

    {
        std::scoped_lock lock(setConstantsMutex);
        State::Instance().slLastConstants = values;
        State::Instance().slLastConstantsFrame = static_cast<uint32_t>(frame);
        State::Instance().slLastConstantsViewport = static_cast<uint32_t>(viewport);
        // As with its tags and markers: another FG input (the upscaler's, the final image's) starts its own frames,
        // and a second start every game frame left every other frame undispatched (Red Dead Redemption).
        if (State::Instance().activeFgInput == FGInput::DLSSG)
            State::Instance().slFGInputs.setConstants(values, (uint32_t) frame);
    }

    return o_slSetConstants(values, frame, viewport);
}

bool StreamlineHooks::hkcommon_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                              const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    auto result = o_common_slOnPluginLoad != nullptr && o_common_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (!HasPluginJson(result, pluginJSON))
    {
        LOG_DEBUG("Plugin load failed or returned no JSON, skipping patching");
        return result;
    }

    try
    {
        nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

        auto& slVersion = State::Instance().streamlineVersion;

        // Grab a version of the potentially updated sl.common
        // Opti assumes that all plugins will have this version
        configJson.at("version").at("major").get_to(slVersion.major);
        configJson.at("version").at("minor").get_to(slVersion.minor);
        configJson.at("version").at("build").get_to(slVersion.patch);

        // Completely disables Streamline hooks
        // if (true)
        //    configJson["hooks"].clear();
        //    configJson["exclusive_hooks"].clear();
        //}

        PatchSL1PluginJson(configJson);

        config = configJson.dump();
        *pluginJSON = config.c_str();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to patch sl.common JSON: {}", e.what());
    }

    return result;
}

sl::Result StreamlineHooks::hkslDLSSGSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    if (o_slDLSSGSetOptions == nullptr)
        return sl::Result::eErrorFeatureMissing;

    lastDlssgViewport = viewport;
    lastDlssgOptions = options;

    // Avoid reading past the game's struct's size
    sl::DLSSGOptions newOptions {};
    auto newStructVer = newOptions.structVersion;

    if (options.structVersion == 1)
        memcpy(&newOptions, &options, 104);
    else if (options.structVersion == 2 || options.structVersion == 3)
        memcpy(&newOptions, &options, 112);
    else if (options.structVersion == 4 || options.structVersion == 5)
        memcpy(&newOptions, &options, 120);
    else
        newOptions = options;

    newOptions.structVersion = newStructVer;

    auto& state = State::Instance();

    // Disable game's DLSSG when we are trying to create our own instance of DLSSG. Without an FG input there is no
    // instance of ours, so the game's DLSS-FG stays on (on AMD it runs on NeuralHub's NGX host).
    if (state.activeFgInput != FGInput::DLSSG && state.activeFgInput != FGInput::NoFG &&
        state.activeFgOutput == FGOutput::DLSSG)
    {
        newOptions.mode = sl::DLSSGMode::eOff;
        return o_slDLSSGSetOptions(viewport, newOptions);
    }

    // Make DLSSG auto always mean On
    if (newOptions.mode == sl::DLSSGMode::eAuto)
        newOptions.mode = sl::DLSSGMode::eOn;

    const auto dlssgPotentiallyActive = newOptions.mode == sl::DLSSGMode::eOn ||
                                        newOptions.mode == sl::DLSSGMode::eAuto ||
                                        newOptions.mode == sl::DLSSGMode::eDynamic;

    bool enableDynamicMode = Config::Instance()->FGDLSSGOverrideForceDMFG.value_or_default() &&
                             state.dlssgGameDMFGSupported && dlssgPotentiallyActive;

    if (enableDynamicMode)
    {
        newOptions.mode = sl::DLSSGMode::eDynamic;
    }

    if (newOptions.mode == sl::DLSSGMode::eDynamic && Config::Instance()->FGDLSSGFramerateTargetDMFG.has_value())
    {
        newOptions.dynamicTargetFrameRate = Config::Instance()->FGDLSSGFramerateTargetDMFG.value();
    }

    LOG_TRACE("DLSSG Modified Mode: {}", magic_enum::enum_name(newOptions.mode));

    if (dlssgPotentiallyActive && state.streamlineVersion >= feature_version { 2, 7, 1 })
    {
        // Before the read, so the count this captures is the patched one. Five stays under the
        // sanity bound below.
        MfgUnlock::TryApply();

        // nvngx_dlssg.dll can load after this runs, and the ceiling read before it does is Ada's
        // 1. Caching that holds it for the session and clamps the override to it. ModuleFound
        // means the patches have been attempted, so from there the answer is final either way.
        const bool unlockPending =
            Config::Instance()->FGDLSSGAdaMfgUnlock.value_or_default() && !MfgUnlock::LastStatus().ModuleFound;

        // Populate dlssgMfgMax once
        if (!state.dlssgMfgMax.has_value() && !unlockPending)
        {
            sl::DLSSGState localState {};
            sl::DLSSGOptions localOptions {};
            if (o_slDLSSGGetState(viewport, localState, &localOptions) == sl::Result::eOk)
            {
                // A wrapper ahead of the snippet can answer a lower ceiling than the patched one.
                if (auto unlockedMax = MfgUnlock::UnlockedMax(); unlockedMax > localState.numFramesToGenerateMax)
                    localState.numFramesToGenerateMax = unlockedMax;

                if (localState.numFramesToGenerateMax > 0 && localState.numFramesToGenerateMax < 6)
                {
                    state.dlssgMfgMax = localState.numFramesToGenerateMax;
                    LOG_TRACE("Saving original numFramesToGenerateMax: {}", state.dlssgMfgMax.value());

                    // Volatile: the clamp holds for this run and leaves the ini setting alone.
                    if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value() &&
                        Config::Instance()->FGDLSSGOverrideInterpolationCount.value() > state.dlssgMfgMax.value())
                    {
                        Config::Instance()->FGDLSSGOverrideInterpolationCount.set_volatile_value(
                            state.dlssgMfgMax.value());
                    }
                }
            }
        }

        // Won't take effect with Dynamic
        if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value())
        {
            auto overrideCount = Config::Instance()->FGDLSSGOverrideInterpolationCount.value();
            if (overrideCount != 0)
                newOptions.numFramesToGenerate = overrideCount;
            else if (!enableDynamicMode)
                newOptions.mode = sl::DLSSGMode::eOff;
        }
    }
    else if (dlssgPotentiallyActive && Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value() &&
             Config::Instance()->FGDLSSGOverrideInterpolationCount.value() != 0)
    {
        // Once. slDLSSGSetOptions runs per frame.
        static bool warnedNoMfg = false;

        if (!warnedNoMfg)
        {
            warnedNoMfg = true;
            LOG_WARN("Interpolation count override ignored: Streamline {}.{}.{}, multi frame generation "
                     "needs 2.7.1",
                     state.streamlineVersion.major, state.streamlineVersion.minor, state.streamlineVersion.patch);
        }
    }

    state.dlssgLastSetMode = newOptions.mode;
    state.dlssgLastSetFrames = newOptions.mode == sl::DLSSGMode::eOff ? 0 : (int) newOptions.numFramesToGenerate;

    const auto result = o_slDLSSGSetOptions(viewport, newOptions);
    static thread_local int loggedMode = -1;
    static thread_local uint32_t loggedCount = ~0u;
    if (loggedMode != static_cast<int>(newOptions.mode) || loggedCount != newOptions.numFramesToGenerate ||
        result != sl::Result::eOk)
    {
        LOG_INFO("DLSSG options: requested={} forwarded={} generatedFrames={} result={}",
                 static_cast<int>(options.mode), static_cast<int>(newOptions.mode), newOptions.numFramesToGenerate,
                 static_cast<int>(result));
        loggedMode = static_cast<int>(newOptions.mode);
        loggedCount = newOptions.numFramesToGenerate;
    }
    return result;
}

sl::Result StreamlineHooks::hkslDLSSGGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                              const sl::DLSSGOptions* options)
{
    if (o_slDLSSGGetState == nullptr)
        return sl::Result::eErrorFeatureMissing;

    // Ahead of every read of numFramesToGenerateMax, which is the value the patch raises.
    MfgUnlock::TryApply();

    sl::Result result {};

    const auto originalStructVersion = state.structVersion;
    if (originalStructVersion < 4)
    {
        sl::DLSSGState newState {};

        // We might be feeding a newer struct to an older SL but that seems to work just fine for this Get function
        result = o_slDLSSGGetState(viewport, dynamic_cast<sl::DLSSGState&>(newState), options);
        if (result != sl::Result::eOk)
            return result;

        // Copy back data to game's struct
        memcpy(&state, &newState, 56); // struct ver 1 size
        state.structVersion = originalStructVersion;

        if (originalStructVersion >= 2)
        {
            state.numFramesToGenerateMax = newState.numFramesToGenerateMax;
            state.bReserved4 = newState.bReserved4;
            state.bIsVsyncSupportAvailable = newState.bIsVsyncSupportAvailable;

            // nvngx_dlssg.dll answers the real ceiling, but a Streamline wrapper between here and the
            // snippet can carry a lower one of its own. Publish the unlocked count. Struct version 1
            // ends ahead of this field, so the raise stays inside this branch.
            if (auto unlockedMax = MfgUnlock::UnlockedMax(); unlockedMax > state.numFramesToGenerateMax)
                state.numFramesToGenerateMax = unlockedMax;
        }

        if (originalStructVersion >= 3)
        {
            state.inputsProcessingCompletionFence = newState.inputsProcessingCompletionFence;
            state.lastPresentInputsProcessingCompletionFenceValue =
                newState.lastPresentInputsProcessingCompletionFenceValue;
        }

        State::Instance().dlssgGameDMFGSupported = newState.bIsDynamicMFGSupported == sl::eTrue;
    }
    else
    {
        result = o_slDLSSGGetState(viewport, state, options);
        if (result != sl::Result::eOk)
            return result;
        State::Instance().dlssgGameDMFGSupported = state.bIsDynamicMFGSupported == sl::eTrue;

        // The wrapper's ceiling, replaced by the unlocked count.
        if (auto unlockedMax = MfgUnlock::UnlockedMax(); unlockedMax > state.numFramesToGenerateMax)
            state.numFramesToGenerateMax = unlockedMax;
    }

    if (!State::Instance().dlssgGameDMFGSupported)
    {
        Config::Instance()->FGDLSSGOverrideForceDMFG.set_volatile_value(false);
    }

    auto& optiState = State::Instance();

    if (optiState.streamlineVersion >= feature_version { 2, 7, 1 })
    {
        // Provisional until the snippet has been seen. See the note in hkslDLSSGSetOptions.
        const bool unlockPending =
            Config::Instance()->FGDLSSGAdaMfgUnlock.value_or_default() && !MfgUnlock::LastStatus().ModuleFound;

        if (!optiState.dlssgMfgMax.has_value() && !unlockPending)
        {
            sl::DLSSGState localState {};
            sl::DLSSGOptions localOptions {};
            if (o_slDLSSGGetState(viewport, localState, &localOptions) == sl::Result::eOk)
            {
                // A wrapper ahead of the snippet can answer a lower ceiling than the patched one.
                if (auto unlockedMax = MfgUnlock::UnlockedMax(); unlockedMax > localState.numFramesToGenerateMax)
                    localState.numFramesToGenerateMax = unlockedMax;

                if (localState.numFramesToGenerateMax > 0 && localState.numFramesToGenerateMax < 6)
                {
                    optiState.dlssgMfgMax = localState.numFramesToGenerateMax;
                    LOG_TRACE("Saving original numFramesToGenerateMax: {}", optiState.dlssgMfgMax.value());

                    // Volatile: the clamp holds for this run and leaves the ini setting alone.
                    if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value() &&
                        Config::Instance()->FGDLSSGOverrideInterpolationCount.value() > optiState.dlssgMfgMax.value())
                    {
                        Config::Instance()->FGDLSSGOverrideInterpolationCount.set_volatile_value(
                            optiState.dlssgMfgMax.value());
                    }
                }
            }
        }
    }

    if (optiState.activeFgInput == FGInput::DLSSG)
    {
        if (optiState.currentFG != nullptr && options != nullptr &&
            options->flags & sl::DLSSGFlags::eRequestVRAMEstimate)
            state.estimatedVRAMUsageInBytes = static_cast<uint64_t>(256 * 1024) * 1024;

        FillDlssgInputState(state);

        LOG_DEBUG("Status: {}, numFramesActuallyPresented: {}", magic_enum::enum_name(state.status),
                  state.numFramesActuallyPresented);
    }

    return result;
}

bool StreamlineHooks::hkreflex_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                              const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureReflex);
    }

    auto result = o_reflex_slOnPluginLoad != nullptr && o_reflex_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    if (!HasPluginJson(result, pluginJSON))
        return result;

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON, nullptr, false);
    if (configJson.is_discarded())
    {
        LOG_ERROR("Failed to parse plugin JSON");
        return result;
    }

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslReflexSetOptions(const sl::ReflexOptions& options)
{
    if (o_slReflexSetOptions == nullptr)
        return sl::Result::eErrorFeatureMissing;

    reflexGamesLastMode = options.mode;

    sl::ReflexOptions newOptions = options;

    if (Config::Instance()->FN_ForceReflex == ForceReflex::ForceEnable)
        newOptions.mode = sl::ReflexMode::eLowLatencyWithBoost;

    // Will cause a pink screen when used with DLSSG
    // if (Config::Instance()->FN_ForceReflex == 1)
    //     newOptions.mode = sl::ReflexMode::eOff;

    return o_slReflexSetOptions(newOptions);
}

sl::Result StreamlineHooks::hkslReflexSleep(const sl::FrameToken& frame)
{
    if (o_slReflexSleep == nullptr)
        return sl::Result::eErrorFeatureMissing;

    // if (State::Instance().activeFgOutput == FGOutput::DLSSG && StreamlineProxy::IsD3D12Inited() &&
    //     Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default())
    //{
    //     return StreamlineProxy::ReflexSleep()(frame);
    // }

    return o_slReflexSleep(frame);
}

void* StreamlineHooks::hkdlss_slGetPluginFunction(PFN_slGetPluginFunction original, const char* functionName,
                                                  void* caller)
{
    LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_dlss_slOnPluginLoad = (PFN_slOnPluginLoad) original(functionName);
        return &hkdlss_slOnPluginLoad;
    }

    if (strcmp(functionName, "slDLSSGetOptimalSettings") == 0 &&
        State::Instance().gameQuirks & GameQuirk::PregmataFixDLSSModes)
    {
        o_slDLSSGetOptimalSettings = (decltype(&slDLSSGetOptimalSettings)) original(functionName);
        return &hkslDLSSGetOptimalSettings;
    }

    return original(functionName);
}

void* StreamlineHooks::hkdlssg_slGetPluginFunction(PFN_slGetPluginFunction original, const char* functionName,
                                                   void* caller)
{
    LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_dlssg_slOnPluginLoad = (PFN_slOnPluginLoad) original(functionName);
        return &hkdlssg_slOnPluginLoad;
    }

    if (strcmp(functionName, "slDLSSGSetOptions") == 0)
    {
        o_slDLSSGSetOptions = (decltype(&slDLSSGSetOptions)) original(functionName);

        // Give steam overlay the original as it seems to be hooking it
        auto steamOverlay = KernelBaseProxy::GetModuleHandleA_()("gameoverlayrenderer64.dll");
        if (steamOverlay != nullptr)
        {
            if (HMODULE callerModule = Util::GetCallerModule(caller); callerModule == steamOverlay)
                return o_slDLSSGSetOptions;
        }

        return &hkslDLSSGSetOptions;
    }

    if (strcmp(functionName, "slDLSSGGetState") == 0)
    {
        o_slDLSSGGetState = (decltype(&slDLSSGGetState)) original(functionName);

        // Give steam overlay the original as it seems to be hooking it
        auto steamOverlay = KernelBaseProxy::GetModuleHandleA_()("gameoverlayrenderer64.dll");
        if (steamOverlay != nullptr)
        {
            if (HMODULE callerModule = Util::GetCallerModule(caller); callerModule == steamOverlay)
                return o_slDLSSGGetState;
        }

        return &hkslDLSSGGetState;
    }

    if (strcmp(functionName, "slGetPluginJSONConfig") == 0 && IsSL1AndDLSSGManaged())
    {
        o_dlssg_slGetPluginJSONConfig_sl1 = reinterpret_cast<PFN_slGetPluginJSONConfig_sl1>(original(functionName));

        if (o_dlssg_slGetPluginJSONConfig_sl1 != nullptr)
        {
            LOG_WARN("Hooking SL1 DLSSG slGetPluginJSONConfig");
            return &hkdlssg_slGetPluginJSONConfig_sl1;
        }
    }

    // Ensure that we have those DLSSG calls
    if (!o_slDLSSGSetOptions)
        o_slDLSSGSetOptions = (decltype(&slDLSSGSetOptions)) original("slDLSSGSetOptions");

    if (!o_slDLSSGGetState)
        o_slDLSSGGetState = (decltype(&slDLSSGGetState)) original("slDLSSGGetState");

    return original(functionName);
}

void* StreamlineHooks::hklocal_dlssg_slGetPluginFunction(PFN_slGetPluginFunction original, const char* functionName,
                                                         void* caller)
{
    // LOG_DEBUG("{}", functionName);

    // An NGX FG replacement, or NeuralHub's DLSS-FG on AMD: either way the plugin's hardware check has to see the
    // spoofed architecture. Real DLSSG on NVIDIA is left alone.
    if (strcmp(functionName, "slOnPluginLoad") == 0 &&
        (State::Instance().activeFgNvngx != FGNvngxReplacement::None || IsNeuralHubFgArmed()))
    {
        o_local_dlssg_slOnPluginLoad = (PFN_slOnPluginLoad) original(functionName);
        return &hklocal_dlssg_slOnPluginLoad;
    }

    return original(functionName);
}

bool StreamlineHooks::hkreflex_slSetConstants_sl1(const void* data, uint32_t frameIndex, uint32_t id)
{
    if (o_reflex_slSetConstants_sl1 == nullptr)
        return false;

    // Streamline v1's version of slReflexSetOptions + slPCLSetMarker
    static sl1::ReflexConstants constants {};
    constants = *(const sl1::ReflexConstants*) data;

    reflexGamesLastMode = (sl::ReflexMode) constants.mode;

    LOG_DEBUG("mode: {}, frameIndex: {}, id: {}", (uint32_t) constants.mode, frameIndex, id);

    if (Config::Instance()->FN_ForceReflex == ForceReflex::ForceEnable)
        constants.mode = sl1::ReflexMode::eReflexModeLowLatencyWithBoost;

    // Will cause a pink screen when used with DLSSG
    // else if (Config::Instance()->FN_ForceReflex == 1)
    //     constants.mode = sl1::ReflexMode::eReflexModeOff;

    return o_reflex_slSetConstants_sl1(&constants, frameIndex, id);
}

void* StreamlineHooks::hkreflex_slGetPluginFunction(PFN_slGetPluginFunction original, const char* functionName,
                                                    void* caller)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slSetConstants") == 0 && State::Instance().streamlineVersion.major == 1)
    {
        o_reflex_slSetConstants_sl1 = (PFN_slSetConstants_sl1) original(functionName);
        return &hkreflex_slSetConstants_sl1;
    }

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_reflex_slOnPluginLoad = (PFN_slOnPluginLoad) original(functionName);
        return &hkreflex_slOnPluginLoad;
    }

    if (strcmp(functionName, "slReflexSetOptions") == 0)
    {
        o_slReflexSetOptions = (decltype(&slReflexSetOptions)) original(functionName);
        return &hkslReflexSetOptions;
    }

    if (strcmp(functionName, "slReflexSleep") == 0)
    {
        o_slReflexSleep = (decltype(&slReflexSleep)) original(functionName);
        return &hkslReflexSleep;
    }

    // TODO: Hopefully a game doesn't call both, maybe separate
    if (strcmp(functionName, "slReflexSetMarker") == 0 &&
        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||
         State::Instance().activeFgInput == FGInput::DLSSG))
    {
        o_slPCLSetMarker = (decltype(&slPCLSetMarker)) original(functionName);
        return &hkslPCLSetMarker;
    }

    return original(functionName);
}

sl::Result StreamlineHooks::hkslPCLSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame)
{
    if (o_slPCLSetMarker == nullptr)
        return sl::Result::eErrorFeatureMissing;

    // Indiana Jones sends a nullptr on startup :)
    const void* volatile framePtr = &frame;
    if (framePtr == nullptr)
    {
        LOG_WARN("Null frame token for marker {}", (uint32_t) marker);
        return o_slPCLSetMarker(marker, frame);
    }

    // if (State::Instance().activeFgOutput == FGOutput::DLSSG && StreamlineProxy::IsD3D12Inited() &&
    //     Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default())
    //{
    //     return StreamlineProxy::PCLSetMarker()(marker, frame);
    // }

    // HACK for broken games
    if (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers)
    {
        static uint64_t last_simulation_end_id = 0;
        if (marker == sl::PCLMarker::eSimulationEnd)
        {
            last_simulation_end_id = frame;
        }

        if (marker == sl::PCLMarker::eSimulationStart && last_simulation_end_id >= frame && o_slGetNewFrameToken)
        {
            const uint64_t correction_offset = last_simulation_end_id - frame + 1;
            uint32_t newFrameId = static_cast<uint32_t>(frame + correction_offset);

            sl::FrameToken* newFramePointer {};
            auto result = o_slGetNewFrameToken(newFramePointer, &newFrameId);

            LOG_WARN("Simulation start marker sent after end marker, offset: {}", correction_offset);
            InputCollection::getInstance().startCollectingForFrame(newFrameId);

            result = o_slPCLSetMarker(marker, *newFramePointer);
            return result;
        }
    }

    if (marker == sl::PCLMarker::eSimulationStart)
        InputCollection::getInstance().startCollectingForFrame(frame);

    if (State::Instance().activeFgInput == FGInput::DLSSG)
    {
        if (State::Instance().streamlineVersion.major == 1)
        {
            if (marker == sl::PCLMarker::eRenderSubmitStart)
            {
                State::Instance().s_sl1FGInputs.evaluateState();
            }
            else if (marker == sl::PCLMarker::ePresentStart)
            {
                State::Instance().s_sl1FGInputs.markPresent(frame);
            }
        }
        else
        {
            if (marker == sl::PCLMarker::eRenderSubmitStart)
            {
                State::Instance().slFGInputs.evaluateState();
            }
            else if (marker == sl::PCLMarker::ePresentStart)
            {
                State::Instance().slFGInputs.markPresent(frame);
            }
        }
    }

    return o_slPCLSetMarker(marker, frame);
}

bool StreamlineHooks::hkpcl_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                           const char** pluginJSON)
{
    LOG_FUNC();

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeaturePCL);
    }

    auto result = o_pcl_slOnPluginLoad != nullptr && o_pcl_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    return result;
}

void* StreamlineHooks::hkpcl_slGetPluginFunction(PFN_slGetPluginFunction original, const char* functionName,
                                                 void* caller)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slPCLSetMarker") == 0 &&
        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||
         State::Instance().activeFgInput == FGInput::DLSSG))
    {
        o_slPCLSetMarker = (decltype(&slPCLSetMarker)) original(functionName);
        return &hkslPCLSetMarker;
    }

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_pcl_slOnPluginLoad = (PFN_slOnPluginLoad) original(functionName);
        return &hkpcl_slOnPluginLoad;
    }

    return original(functionName);
}

bool StreamlineHooks::hk_setVoid(void* self, const char* key, void** value)
{
    // LOG_DEBUG("{}", key);

    if (strcmp(key, sl::param::common::kSystemCaps) == 0)
    {
        LOG_TRACE("Attempting to change system caps for Streamline v1, this could fail depending on the exact version");

        // SystemCapsSl15 is not entirely correct for Streamline 1.3
        // But we here only use the beginning that matches + extra
        auto caps = (SystemCapsSl15*) value;

        if (caps)
        {
            caps->gpuCount = 1;
            caps->architecture[0] = UINT_MAX;
            caps->driverVersionMajor = 999;

            // HAGS
            *((char*) value + 56) = (char) 0x01;
        }
    }

    return o_setVoid(self, key, value);
}

void StreamlineHooks::hkcommon_slSetParameters_sl1(void* params)
{
    LOG_FUNC();

    if (o_setVoid == nullptr && params)
    {
        void** vtable = *(void***) params;

        // It's flipped, 0 -> set void*, 7 -> get void*
        o_setVoid = (PFN_setVoid) vtable[0];

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (o_setVoid != nullptr)
            DetourAttach(&(PVOID&) o_setVoid, hk_setVoid);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook setVoid: {:X}", detourResult);
            o_setVoid = nullptr;
        }
    }

    if (o_common_slSetParameters_sl1 != nullptr)
        o_common_slSetParameters_sl1(params);
}

void* StreamlineHooks::hkcommon_slGetPluginFunction(PFN_slGetPluginFunction original, const char* functionName,
                                                    void* caller)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_common_slOnPluginLoad = (PFN_slOnPluginLoad) original(functionName);
        return &hkcommon_slOnPluginLoad;
    }

    // Used around Streamline v1.3, as 1.5 doesn't seem to have it anymore
    if (strcmp(functionName, "slSetParameters") == 0)
    {
        o_common_slSetParameters_sl1 = (PFN_slSetParameters_sl1) original(functionName);
        return &hkcommon_slSetParameters_sl1;
    }

    return original(functionName);
}

void StreamlineHooks::updateForceReflex()
{
    // Not needed for Streamline v1 as slSetConstants is sent every frame
    if (o_slReflexSetOptions)
    {
        sl::ReflexOptions options;

        auto forceReflex = Config::Instance()->FN_ForceReflex.value_or_default();

        if (forceReflex == ForceReflex::ForceEnable)
            options.mode = sl::ReflexMode::eLowLatencyWithBoost;
        else if (forceReflex == ForceReflex::ForceDisable)
            options.mode = sl::ReflexMode::eOff;
        else if (forceReflex == ForceReflex::InGame)
            options.mode = reflexGamesLastMode;

        auto result = o_slReflexSetOptions(options);
        if (result != sl::Result::eOk)
        {
            LOG_WARN("Failed to update Reflex mode with error code: {} ({:X})", magic_enum::enum_name(result),
                     (UINT) result);
        }
    }
}

void StreamlineHooks::updateDlssgOptions()
{
    if (o_slDLSSGSetOptions)
    {
        LOG_FUNC();
        hkslDLSSGSetOptions(lastDlssgViewport, lastDlssgOptions);
    }
}

void StreamlineHooks::RecordGameDlssgOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    lastDlssgViewport = viewport;
    lastDlssgOptions = options;

    // Only on change - the game re-applies its options every time the settings are touched, and
    // this needs to say what the game asked for, not how often it said it.
    static int lastCount = -1;
    static int lastMode = -1;

    const int count = (int) options.numFramesToGenerate;
    const int mode = (int) options.mode;

    if (count != lastCount || mode != lastMode)
    {
        lastCount = count;
        lastMode = mode;

        LOG_INFO("Game set DLSSG options: mode {}, numFramesToGenerate {}", magic_enum::enum_name(options.mode), count);
    }
}

int StreamlineHooks::GameRequestedInterpolationCount()
{
    // lastDlssgOptions holds the game's raw, unmodified options, everything we override goes into a copy
    const bool dlssgPotentiallyActive = lastDlssgOptions.mode == sl::DLSSGMode::eOn ||
                                        lastDlssgOptions.mode == sl::DLSSGMode::eAuto ||
                                        lastDlssgOptions.mode == sl::DLSSGMode::eDynamic;

    if (!dlssgPotentiallyActive)
        return 0;

    return (int) lastDlssgOptions.numFramesToGenerate;
}

// SL INTERPOSER

void StreamlineHooks::unhookInterposer()
{
    LOG_FUNC();

    // Plugin routes sit in front of OptiScaler's own detours, they go first
    PluginApi::OnInterposerUnhooking();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_slSetTag)
        DetourDetach(&(PVOID&) o_slSetTag, hkslSetTag);

    if (o_slSetTagForFrame)
        DetourDetach(&(PVOID&) o_slSetTagForFrame, hkslSetTagForFrame);

    if (o_slSetConstants)
        DetourDetach(&(PVOID&) o_slSetConstants, hkslSetConstants);

    if (o_slEvaluateFeature)
        DetourDetach(&(PVOID&) o_slEvaluateFeature, hkslEvaluateFeature);

    if (o_slSetTagForFrame)
    {
        DetourDetach(&(PVOID&) o_slSetTagForFrame, hkslSetTagForFrame);
        o_slSetTagForFrame = nullptr;
    }

    if (o_slInit)
        DetourDetach(&(PVOID&) o_slInit, hkslInit);

    if (o_slInit_sl1)
        DetourDetach(&(PVOID&) o_slInit_sl1, hkslInit_sl1);

    if (o_slSetTag_sl1)
        DetourDetach(&(PVOID&) o_slSetTag_sl1, hkslSetTag_sl1);

    if (o_slSetConstants_interposer_sl1)
        DetourDetach(&(PVOID&) o_slSetConstants_interposer_sl1, hkslSetConstants_sl1);

    if (o_slEvaluateFeature_sl1)
        DetourDetach(&(PVOID&) o_slEvaluateFeature_sl1, hkslEvaluateFeature_sl1);

    // if (o_logCallback)
    //     DetourDetach(&(PVOID&) o_logCallback, streamlineLogCallback);
    // else if (o_logCallback_sl1)
    //     DetourDetach(&(PVOID&) o_logCallback_sl1, streamlineLogCallback);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("DetourTransactionCommit error: {:X}", detourResult);
    }
    else
    {
        o_slInit = nullptr;
        o_slInit_sl1 = nullptr;
        o_slSetTag = nullptr;
        o_slSetTagForFrame = nullptr;
        o_slEvaluateFeature = nullptr;
        o_slSetConstants = nullptr;
        o_slSetTag_sl1 = nullptr;
        o_slSetConstants_interposer_sl1 = nullptr;
        o_slEvaluateFeature_sl1 = nullptr;
        o_logCallback = nullptr;
        o_logCallback_sl1 = nullptr;
        hookedInterposerModule = nullptr;
    }
}

// Call it just after sl.interposer's load or if sl.interposer is already loaded
void StreamlineHooks::hookInterposer(HMODULE slInterposer)
{
    if (State::Instance().externalFrameGeneration)
        return;
    LOG_FUNC();

    if (!slInterposer)
    {
        LOG_WARN("Streamline module in NULL");
        return;
    }

    // Interposer needs this or it might end in an infinite loop calling itself
    if (hookedInterposerModule == slInterposer)
        return;

    // Looks like when reading DLL version load methods are called
    // To prevent loops disabling checks for sl.interposer.dll
    auto owner = State::GetOwner();
    State::DisableChecks(owner, "sl.interposer");

    if (o_slSetTag || o_slInit || o_slInit_sl1 || o_slSetTag_sl1 || o_slSetConstants_interposer_sl1 ||
        o_slEvaluateFeature_sl1)
        unhookInterposer();

    // After unhookInterposer, it resets hookedInterposerModule
    hookedInterposerModule = slInterposer;

    {
        // Wide on purpose. The A variant returns the path in the process's ANSI codepage while
        // string_to_wstring() decodes as UTF-8, so any game installed under a path with non-ASCII
        // characters got a mangled name here, GetFileVersion() failed and sl_version stayed 0.0.0.
        // Every Streamline hook below is gated on that number, so a game with a real 2.x interposer
        // ended up with no hooks installed at all and its FG input chain silently dead.
        wchar_t dllPath[MAX_PATH] = { 0 };
        GetModuleFileNameW(slInterposer, dllPath, MAX_PATH);

        LOG_TRACE("slInterposer path: {}", wstring_to_string(dllPath));

        version_t sl_version;
        if (!Util::GetFileVersion(dllPath, &sl_version))
            LOG_WARN("Could not read sl.interposer.dll's version, reporting it as 0.0.0");

        State::Instance().streamlineVersion.major = sl_version.major;
        State::Instance().streamlineVersion.minor = sl_version.minor;
        State::Instance().streamlineVersion.patch = sl_version.patch;

        LOG_INFO("Streamline version: {}.{}.{}", sl_version.major, sl_version.minor, sl_version.patch);

        if (sl_version.major >= 2)
        {
            o_slSetTag =
                reinterpret_cast<decltype(&slSetTag)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTag"));
            o_slSetTagForFrame = reinterpret_cast<decltype(&slSetTagForFrame)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTagForFrame"));
            o_slInit = reinterpret_cast<decltype(&slInit)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slInit"));
            o_slEvaluateFeature = reinterpret_cast<decltype(&slEvaluateFeature)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slEvaluateFeature"));
            o_slSetConstants = reinterpret_cast<decltype(&slSetConstants)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetConstants"));
            o_slGetNativeInterface = reinterpret_cast<decltype(&slGetNativeInterface)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetNativeInterface"));
            o_slSetD3DDevice = reinterpret_cast<decltype(&slSetD3DDevice)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetD3DDevice"));
            o_slGetNewFrameToken = reinterpret_cast<decltype(&slGetNewFrameToken)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetNewFrameToken")); // Not hooked

            // For making the game think DLSSG is loaded and supported
            // but making SL not actually load the plugin
            o_slIsFeatureSupported = reinterpret_cast<decltype(&slIsFeatureSupported)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slIsFeatureSupported"));
            o_slIsFeatureLoaded = reinterpret_cast<decltype(&slIsFeatureLoaded)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slIsFeatureLoaded"));
            o_slGetFeatureRequirements = reinterpret_cast<decltype(&slGetFeatureRequirements)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetFeatureRequirements"));
            o_slGetFeatureVersion = reinterpret_cast<decltype(&slGetFeatureVersion)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetFeatureVersion"));
            o_slGetFeatureFunction = reinterpret_cast<decltype(&slGetFeatureFunction)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetFeatureFunction"));
            o_slAllocateResources = reinterpret_cast<decltype(&slAllocateResources)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slAllocateResources"));
            o_slFreeResources = reinterpret_cast<decltype(&slFreeResources)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slFreeResources"));

            if (o_slInit != nullptr)
            {
                LOG_TRACE("Hooking v2");
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());

                DetourAttach(&(PVOID&) o_slInit, hkslInit);

                // RR signal discovery must remain available even when frame generation is
                // disabled. The probe hooks only inspect DX12 tag metadata and forward the
                // original calls unchanged.
                if (o_slSetTag != nullptr)
                    DetourAttach(&(PVOID&) o_slSetTag, hkslSetTag);

                if (o_slSetTagForFrame != nullptr)
                    DetourAttach(&(PVOID&) o_slSetTagForFrame, hkslSetTagForFrame);

                // Allow constants to be hooked without DLSSG
                if (o_slSetConstants != nullptr)
                    DetourAttach(&(PVOID&) o_slSetConstants, hkslSetConstants);

                if (o_slEvaluateFeature != nullptr)
                    DetourAttach(&(PVOID&) o_slEvaluateFeature, hkslEvaluateFeature);

                if (State::Instance().activeFgInput == FGInput::DLSSG)
                {
                    if (o_slIsFeatureSupported != nullptr)
                        DetourAttach(&(PVOID&) o_slIsFeatureSupported, hkslIsFeatureSupported);

                    if (o_slIsFeatureLoaded != nullptr)
                        DetourAttach(&(PVOID&) o_slIsFeatureLoaded, hkslIsFeatureLoaded);

                    if (o_slGetFeatureRequirements != nullptr)
                        DetourAttach(&(PVOID&) o_slGetFeatureRequirements, hkslGetFeatureRequirements);

                    if (o_slGetFeatureVersion != nullptr)
                        DetourAttach(&(PVOID&) o_slGetFeatureVersion, hkslGetFeatureVersion);

                    if (o_slGetFeatureFunction != nullptr)
                        DetourAttach(&(PVOID&) o_slGetFeatureFunction, hkslGetFeatureFunction);

                    if (o_slSetFeatureLoaded != nullptr)
                        DetourAttach(&(PVOID&) o_slSetFeatureLoaded, hkslSetFeatureLoaded);

                    if (o_slAllocateResources != nullptr)
                        DetourAttach(&(PVOID&) o_slAllocateResources, hkslAllocateResources);

                    if (o_slFreeResources != nullptr)
                        DetourAttach(&(PVOID&) o_slFreeResources, hkslFreeResources);
                }

                // if (o_slGetNativeInterface != nullptr)
                //     DetourAttach(&(PVOID&) o_slGetNativeInterface, hkslGetNativeInterface);

                // if (o_slSetD3DDevice != nullptr)
                //     DetourAttach(&(PVOID&) o_slSetD3DDevice, hkslSetD3DDevice);

                auto detourResult = DetourTransactionCommit();
                if (detourResult != NO_ERROR)
                {
                    LOG_ERROR("Failed to hook sl.interposer v2: {:X}", detourResult);
                    o_slSetTag = nullptr;
                    o_slSetTagForFrame = nullptr;
                    o_slInit = nullptr;
                    o_slEvaluateFeature = nullptr;
                    o_slAllocateResources = nullptr;
                    o_slSetConstants = nullptr;
                    o_slGetNativeInterface = nullptr;
                    o_slSetD3DDevice = nullptr;
                    o_slIsFeatureSupported = nullptr;
                    o_slIsFeatureLoaded = nullptr;
                    o_slGetFeatureRequirements = nullptr;
                    o_slGetFeatureVersion = nullptr;
                    o_slGetFeatureFunction = nullptr;
                }
            }

            // Plugins that handle Streamline functions themselves (plugin API version 3)
            PluginApi::OnInterposerHooked(slInterposer, sl_version.major, sl_version.minor, sl_version.patch);
        }
        else if (sl_version.major == 1)
        {
            o_slInit_sl1 =
                reinterpret_cast<decltype(&sl1::slInit)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slInit"));
            o_slSetTag_sl1 = reinterpret_cast<decltype(&sl1::slSetTag)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTag"));
            o_slSetConstants_interposer_sl1 = reinterpret_cast<decltype(&sl1::slSetConstants)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetConstants"));
            o_slEvaluateFeature_sl1 = reinterpret_cast<decltype(&sl1::slEvaluateFeature)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slEvaluateFeature"));

            LOG_INFO("SL1 exports - slInit: {}, slSetTag: {}, slSetConstants: {}, slEvaluateFeature: {}",
                     o_slInit_sl1 != nullptr, o_slSetTag_sl1 != nullptr, o_slSetConstants_interposer_sl1 != nullptr,
                     o_slEvaluateFeature_sl1 != nullptr);

            if (o_slInit_sl1 || o_slSetTag_sl1 || o_slSetConstants_interposer_sl1 || o_slEvaluateFeature_sl1)
            {
                LOG_TRACE("Hooking v1");
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());

                if (o_slInit_sl1)
                    DetourAttach(&(PVOID&) o_slInit_sl1, hkslInit_sl1);

                if (IsSL1AndFGActive())
                {
                    if (o_slSetTag_sl1)
                        DetourAttach(&(PVOID&) o_slSetTag_sl1, hkslSetTag_sl1);

                    if (o_slSetConstants_interposer_sl1)
                        DetourAttach(&(PVOID&) o_slSetConstants_interposer_sl1, hkslSetConstants_sl1);

                    if (o_slEvaluateFeature_sl1)
                        DetourAttach(&(PVOID&) o_slEvaluateFeature_sl1, hkslEvaluateFeature_sl1);
                }

                auto detourResult = DetourTransactionCommit();
                if (detourResult != NO_ERROR)
                {
                    LOG_ERROR("Failed to hook sl.interposer v1: {:X}", detourResult);
                    o_slInit_sl1 = nullptr;
                    o_slSetTag_sl1 = nullptr;
                    o_slSetConstants_interposer_sl1 = nullptr;
                    o_slEvaluateFeature_sl1 = nullptr;
                }
            }
        }
    }

    State::EnableChecks(owner);
}

// SL PLUGINS (sl.dlss, sl.dlss_g, sl.reflex, sl.pcl, sl.common)
//
// Streamline may load several copies of the same plugin, e.g. the game's sl.reflex.dll and then an OTA
// 1B0_E658703.dll, decide that one of them is not allowed (OTA denylist) and FreeLibrary it while it keeps
// using the other one. So every copy gets its own detour slot and stays hooked for as long as it's mapped.
// A slot is released after the module has been unmapped (onModuleFreed / releaseDeadSlots), which never
// writes into the freed (and possibly reused) address range.

const char* StreamlineHooks::pluginName(SlPlugin plugin)
{
    switch (plugin)
    {
    case SlPlugin::Dlss:
        return "sl.dlss";
    case SlPlugin::Dlssg:
        return "sl.dlss_g";
    case SlPlugin::LocalDlssg:
        return "local sl.dlss_g";
    case SlPlugin::Reflex:
        return "sl.reflex";
    case SlPlugin::Pcl:
        return "sl.pcl";
    case SlPlugin::Common:
        return "sl.common";
    default:
        return "unknown";
    }
}

std::mutex& StreamlineHooks::pluginMutex(SlPlugin plugin)
{
    switch (plugin)
    {
    case SlPlugin::Dlss:
        return mutexHookDlss;
    case SlPlugin::Dlssg:
        return mutexHookDlssg;
    case SlPlugin::LocalDlssg:
        return mutexHookLocalDlssg;
    case SlPlugin::Reflex:
        return mutexHookReflex;
    case SlPlugin::Pcl:
        return mutexHookPcl;
    case SlPlugin::Common:
    default:
        return mutexHookCommon;
    }
}

HMODULE& StreamlineHooks::activePluginModule(SlPlugin plugin)
{
    switch (plugin)
    {
    case SlPlugin::Dlss:
        return hookedDlssModule;
    case SlPlugin::Dlssg:
        return hookedDlssgModule;
    case SlPlugin::LocalDlssg:
        return hookedLocalDlssgModule;
    case SlPlugin::Reflex:
        return hookedReflexModule;
    case SlPlugin::Pcl:
        return hookedPclModule;
    case SlPlugin::Common:
    default:
        return hookedCommonModule;
    }
}

template <StreamlineHooks::SlPlugin P, size_t I> void* StreamlineHooks::slotGetPluginFunction(const char* functionName)
{
    auto original = pluginSlots[(size_t) P][I].original;

    // Can't happen while the module is mapped, but don't crash if it somehow does
    if (original == nullptr)
        return nullptr;

    return dispatchGetPluginFunction(P, original, functionName, _ReturnAddress());
}

void* StreamlineHooks::dispatchGetPluginFunction(SlPlugin plugin, PFN_slGetPluginFunction original,
                                                 const char* functionName, void* caller)
{
    if (functionName == nullptr)
        return original(functionName);

    switch (plugin)
    {
    case SlPlugin::Dlss:
        return hkdlss_slGetPluginFunction(original, functionName, caller);
    case SlPlugin::Dlssg:
        return hkdlssg_slGetPluginFunction(original, functionName, caller);
    case SlPlugin::LocalDlssg:
        return hklocal_dlssg_slGetPluginFunction(original, functionName, caller);
    case SlPlugin::Reflex:
        return hkreflex_slGetPluginFunction(original, functionName, caller);
    case SlPlugin::Pcl:
        return hkpcl_slGetPluginFunction(original, functionName, caller);
    case SlPlugin::Common:
        return hkcommon_slGetPluginFunction(original, functionName, caller);
    default:
        return original(functionName);
    }
}

StreamlineHooks::PFN_slGetPluginFunction StreamlineHooks::slotDetour(SlPlugin plugin, size_t index)
{
    static_assert(kMaxPluginInstances == 4, "Update the detour table below");
    static_assert((size_t) SlPlugin::Count == 6, "Update the detour table below");

    using PFN = PFN_slGetPluginFunction;
    static const PFN table[(size_t) SlPlugin::Count][kMaxPluginInstances] = {
        { &slotGetPluginFunction<SlPlugin::Dlss, 0>, &slotGetPluginFunction<SlPlugin::Dlss, 1>,
          &slotGetPluginFunction<SlPlugin::Dlss, 2>, &slotGetPluginFunction<SlPlugin::Dlss, 3> },
        { &slotGetPluginFunction<SlPlugin::Dlssg, 0>, &slotGetPluginFunction<SlPlugin::Dlssg, 1>,
          &slotGetPluginFunction<SlPlugin::Dlssg, 2>, &slotGetPluginFunction<SlPlugin::Dlssg, 3> },
        { &slotGetPluginFunction<SlPlugin::LocalDlssg, 0>, &slotGetPluginFunction<SlPlugin::LocalDlssg, 1>,
          &slotGetPluginFunction<SlPlugin::LocalDlssg, 2>, &slotGetPluginFunction<SlPlugin::LocalDlssg, 3> },
        { &slotGetPluginFunction<SlPlugin::Reflex, 0>, &slotGetPluginFunction<SlPlugin::Reflex, 1>,
          &slotGetPluginFunction<SlPlugin::Reflex, 2>, &slotGetPluginFunction<SlPlugin::Reflex, 3> },
        { &slotGetPluginFunction<SlPlugin::Pcl, 0>, &slotGetPluginFunction<SlPlugin::Pcl, 1>,
          &slotGetPluginFunction<SlPlugin::Pcl, 2>, &slotGetPluginFunction<SlPlugin::Pcl, 3> },
        { &slotGetPluginFunction<SlPlugin::Common, 0>, &slotGetPluginFunction<SlPlugin::Common, 1>,
          &slotGetPluginFunction<SlPlugin::Common, 2>, &slotGetPluginFunction<SlPlugin::Common, 3> },
    };

    return table[(size_t) plugin][index];
}

// Module still mapped at the same base, same file, and our patch is still in place
// (a freed + reloaded copy of the same dll at the same base would have fresh, unpatched code)
bool StreamlineHooks::isSlotAlive(const PluginHookSlot& slot)
{
    if (slot.module == nullptr || slot.target == nullptr || slot.path.empty())
        return false;

    const auto current = GetModulePath(slot.module);
    if (current.empty() || _wcsicmp(current.c_str(), slot.path.c_str()) != 0)
        return false;

    // Safe to read, the module checked above is mapped
    return memcmp(slot.target, slot.patchBytes, kPatchBytes) == 0;
}

// Plugin provided function pointers that point into the module that is gone
void StreamlineHooks::clearPluginPointersInRange(uintptr_t base, size_t size)
{
    if (base == 0 || size == 0)
        return;

    auto clear = [base, size](auto& ptr, const char* name)
    {
        const auto address = reinterpret_cast<uintptr_t>(ptr);
        if (ptr != nullptr && address >= base && address < base + size)
        {
            LOG_DEBUG("Clearing {}, it pointed into an unloaded plugin", name);
            ptr = nullptr;
        }
    };

    clear(o_dlss_slOnPluginLoad, "o_dlss_slOnPluginLoad");
    clear(o_slDLSSGetOptimalSettings, "o_slDLSSGetOptimalSettings");

    clear(o_dlssg_slOnPluginLoad, "o_dlssg_slOnPluginLoad");
    clear(o_dlssg_slGetPluginJSONConfig_sl1, "o_dlssg_slGetPluginJSONConfig_sl1");
    clear(o_slDLSSGSetOptions, "o_slDLSSGSetOptions");
    clear(o_slDLSSGGetState, "o_slDLSSGGetState");

    clear(o_local_dlssg_slOnPluginLoad, "o_local_dlssg_slOnPluginLoad");

    clear(o_reflex_slOnPluginLoad, "o_reflex_slOnPluginLoad");
    clear(o_reflex_slSetConstants_sl1, "o_reflex_slSetConstants_sl1");
    clear(o_slReflexSetOptions, "o_slReflexSetOptions");
    clear(o_slReflexSleep, "o_slReflexSleep");

    clear(o_pcl_slOnPluginLoad, "o_pcl_slOnPluginLoad");
    clear(o_slPCLSetMarker, "o_slPCLSetMarker");

    clear(o_common_slOnPluginLoad, "o_common_slOnPluginLoad");
    clear(systemCaps, "systemCaps");
    clear(systemCapsSl15, "systemCapsSl15");
    clear(o_common_slSetParameters_sl1, "o_common_slSetParameters_sl1");
}

// Caller holds pluginMutex(plugin)
void StreamlineHooks::releaseSlot(SlPlugin plugin, size_t index, bool detach)
{
    auto& slot = pluginSlots[(size_t) plugin][index];

    if (slot.module == nullptr)
        return;

    const bool alive = isSlotAlive(slot);

    if (detach && alive)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&) slot.original, (PVOID) slotDetour(plugin, index));

        if (auto r = DetourTransactionCommit(); r != NO_ERROR)
        {
            LOG_ERROR("Failed to unhook {} ({:X}): {:X}", pluginName(plugin), (size_t) slot.module, r);
            return;
        }

        LOG_DEBUG("Unhooked {} at {:X}", pluginName(plugin), (size_t) slot.module);
    }
    else if (!alive)
    {
        // Never touch the memory, it's gone or belongs to another image now
        LOG_DEBUG("{} at {:X} is no longer loaded, releasing its hook slot", pluginName(plugin), (size_t) slot.module);

        clearPluginPointersInRange(reinterpret_cast<uintptr_t>(slot.module), slot.imageSize);
    }

    const auto releasedModule = slot.module;

    slot.original = nullptr;
    slot.target = nullptr;
    slot.module = nullptr;
    slot.imageSize = 0;
    slot.path.clear();
    memset(slot.patchBytes, 0, sizeof(slot.patchBytes));

    // Point "active" module to another live copy, if any
    auto& active = activePluginModule(plugin);
    if (active == releasedModule)
    {
        active = nullptr;

        for (auto& other : pluginSlots[(size_t) plugin])
        {
            if (other.module != nullptr)
            {
                active = other.module;
                break;
            }
        }
    }
}

// Caller holds pluginMutex(plugin)
void StreamlineHooks::releaseDeadSlots(SlPlugin plugin)
{
    for (size_t i = 0; i < kMaxPluginInstances; i++)
    {
        const auto& slot = pluginSlots[(size_t) plugin][i];

        if (slot.module != nullptr && !isSlotAlive(slot))
            releaseSlot(plugin, i, false);
    }
}

// Caller holds pluginMutex(plugin)
void StreamlineHooks::hookPlugin(SlPlugin plugin, HMODULE module)
{
    const auto name = pluginName(plugin);

    if (module == nullptr)
    {
        LOG_WARN("{} module is NULL", name);
        return;
    }

    // Drop copies that Streamline already unloaded (e.g. a rejected OTA plugin)
    releaseDeadSlots(plugin);

    auto& slots = pluginSlots[(size_t) plugin];

    for (auto& slot : slots)
    {
        if (slot.module == module)
        {
            LOG_TRACE("{} at {:X} is already hooked", name, (size_t) module);
            activePluginModule(plugin) = module;
            return;
        }
    }

    auto target =
        reinterpret_cast<PFN_slGetPluginFunction>(KernelBaseProxy::GetProcAddress_()(module, "slGetPluginFunction"));

    if (target == nullptr)
    {
        LOG_WARN("{} at {:X} has no slGetPluginFunction", name, (size_t) module);
        return;
    }

    size_t index = kMaxPluginInstances;
    for (size_t i = 0; i < kMaxPluginInstances; i++)
    {
        if (slots[i].module == nullptr)
        {
            index = i;
            break;
        }
    }

    if (index == kMaxPluginInstances)
    {
        LOG_ERROR("No free hook slot for {} at {:X}, {} copies are already loaded", name, (size_t) module,
                  kMaxPluginInstances);
        return;
    }

    auto& slot = slots[index];
    slot.original = target;

    LOG_TRACE("Hooking slGetPluginFunction in {} (slot {})", name, index);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) slot.original, (PVOID) slotDetour(plugin, index));

    if (auto r = DetourTransactionCommit(); r != NO_ERROR)
    {
        LOG_ERROR("Failed to hook {}: {:X}", name, r);
        slot.original = nullptr;
        return;
    }

    size_t imageSize = 0;
    {
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            auto nt =
                reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                imageSize = nt->OptionalHeader.SizeOfImage;
        }
    }

    slot.target = reinterpret_cast<void*>(target);
    slot.module = module;
    slot.imageSize = imageSize;
    slot.path = GetModulePath(module);
    memcpy(slot.patchBytes, slot.target, kPatchBytes);

    activePluginModule(plugin) = module;

    LOG_TRACE("Hooked {} at {:X} (slot {}): {}", name, (size_t) module, index, wstring_to_string(slot.path));
}

// Caller holds pluginMutex(plugin)
void StreamlineHooks::unhookPlugin(SlPlugin plugin)
{
    for (size_t i = 0; i < kMaxPluginInstances; i++)
        releaseSlot(plugin, i, true);
}

bool StreamlineHooks::isPluginHooked(SlPlugin plugin)
{
    for (const auto& slot : pluginSlots[(size_t) plugin])
    {
        if (slot.module != nullptr)
            return true;
    }

    return false;
}

void StreamlineHooks::onModuleFreed(PVOID module)
{
    if (module == nullptr)
        return;

    for (size_t p = 0; p < (size_t) SlPlugin::Count; p++)
    {
        const auto plugin = (SlPlugin) p;

        // Cheap pre-check without the lock, worst case we miss it here and releaseDeadSlots catches it later
        bool ours = false;
        for (const auto& slot : pluginSlots[p])
        {
            if (slot.module == (HMODULE) module)
            {
                ours = true;
                break;
            }
        }

        if (!ours)
            continue;

        // try_lock: never block here, FreeLibrary can be called from places we don't control.
        // If it's busy, the hook call holding it will release dead slots itself.
        std::unique_lock lock(pluginMutex(plugin), std::try_to_lock);
        if (!lock.owns_lock())
            continue;

        releaseDeadSlots(plugin);
    }
}

// SL DLSS

void StreamlineHooks::unhookDlss()
{
    LOG_FUNC();
    unhookPlugin(SlPlugin::Dlss);
}

void StreamlineHooks::hookDlss(HMODULE slDlss)
{
    // The external FG owner keeps Streamline to itself
    if (State::Instance().externalFrameGeneration)
        return;

    LOG_FUNC();
    hookPlugin(SlPlugin::Dlss, slDlss);
}

// SL DLSSG

void StreamlineHooks::unhookDlssg()
{
    LOG_FUNC();
    unhookPlugin(SlPlugin::Dlssg);
}

void StreamlineHooks::hookDlssg(HMODULE slDlssg)
{
    // The external FG owner keeps Streamline to itself
    if (State::Instance().externalFrameGeneration)
        return;

    LOG_FUNC();
    hookPlugin(SlPlugin::Dlssg, slDlssg);
}

// Local SL DLSSG

void StreamlineHooks::unhookLocalDlssg()
{
    LOG_FUNC();
    unhookPlugin(SlPlugin::LocalDlssg);
}

void StreamlineHooks::hookLocalDlssg(HMODULE slDlssg)
{
    // The external FG owner keeps Streamline to itself
    if (State::Instance().externalFrameGeneration)
        return;

    LOG_FUNC();
    hookPlugin(SlPlugin::LocalDlssg, slDlssg);
}

// SL REFLEX

void StreamlineHooks::unhookReflex()
{
    LOG_FUNC();
    unhookPlugin(SlPlugin::Reflex);
}

void StreamlineHooks::hookReflex(HMODULE slReflex)
{
    // The external FG owner keeps Streamline to itself
    if (State::Instance().externalFrameGeneration)
        return;

    LOG_FUNC();
    hookPlugin(SlPlugin::Reflex, slReflex);
}

// SL PCL

void StreamlineHooks::unhookPcl()
{
    LOG_FUNC();
    unhookPlugin(SlPlugin::Pcl);
}

void StreamlineHooks::hookPcl(HMODULE slPcl)
{
    // The external FG owner keeps Streamline to itself
    if (State::Instance().externalFrameGeneration)
        return;

    LOG_FUNC();
    hookPlugin(SlPlugin::Pcl, slPcl);
}

// SL COMMON

void StreamlineHooks::unhookCommon()
{
    LOG_FUNC();
    unhookPlugin(SlPlugin::Common);

    systemCaps = nullptr;
    systemCapsSl15 = nullptr;
}

void StreamlineHooks::hookCommon(HMODULE slCommon)
{
    // The external FG owner keeps Streamline to itself
    if (State::Instance().externalFrameGeneration)
        return;

    LOG_FUNC();
    hookPlugin(SlPlugin::Common, slCommon);
}

bool StreamlineHooks::isInterposerHooked() { return o_slInit != nullptr || o_slInit_sl1 != nullptr; }

bool StreamlineHooks::isDlssHooked() { return isPluginHooked(SlPlugin::Dlss); }

bool StreamlineHooks::isDlssgHooked() { return isPluginHooked(SlPlugin::Dlssg); }

bool StreamlineHooks::isLocalDlssgHooked() { return isPluginHooked(SlPlugin::LocalDlssg); }

bool StreamlineHooks::isCommonHooked() { return isPluginHooked(SlPlugin::Common); }

bool StreamlineHooks::isSetConstantsHooked() { return o_slSetConstants != nullptr; }

bool StreamlineHooks::isPclHooked() { return isPluginHooked(SlPlugin::Pcl); }

bool StreamlineHooks::isReflexHooked() { return isPluginHooked(SlPlugin::Reflex); }
