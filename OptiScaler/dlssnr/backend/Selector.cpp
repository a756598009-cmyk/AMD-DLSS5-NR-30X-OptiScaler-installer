#include "pch.h"
#include "Selector.h"
#include <Config.h>
#include <State.h>
#include <Util.h>
#include <filesystem>

namespace DlssNr::Backend
{
namespace
{
// NrBackend as the game started with it. Each backend installs its own D3D12 hooks as the device is
// created, so a value changed in the menu applies from the next launch, never to this session.
const std::optional<std::string>& LaunchValue()
{
    static const std::optional<std::string> value =
        Config::Instance()->NrBackend.has_value() ? std::optional(Config::Instance()->NrBackend.value()) : std::nullopt;
    return value;
}
} // namespace

Kind RequestedKind()
{
    const auto& raw = LaunchValue();
    return raw ? ParseKind(*raw) : Kind::Daniel;
}

const wchar_t* LmxxfRuntimeFile()
{
    const auto& raw = LaunchValue();
    const bool rdna2 = raw && (_stricmp(raw->c_str(), "rdna2") == 0 || _stricmp(raw->c_str(), "hexneural") == 0);
    return rdna2 ? L"Rdna2NrRuntime.dll" : L"LmxxfNrRuntime.dll";
}

Kind ActiveKindFromConfig()
{
    const auto requested = RequestedKind();
    const auto& raw = LaunchValue();
    const bool isAuto = !raw || raw->empty() || (_stricmp(raw->c_str(), "auto") == 0);
    if (isAuto)
    {
        std::error_code ec;
        const auto dir = Util::DllPath().parent_path();
        const bool hasDaniel = std::filesystem::exists(dir / L"dlssnr_amd_pass1.dll", ec);
        const bool hasLmxxf = std::filesystem::exists(dir / L"LmxxfNrRuntime.dll", ec);
        if (hasLmxxf && !hasDaniel && LmxxfWired())
            return Kind::Lmxxf;
    }
    return ActiveKind(requested);
}

// The runtimes that run between the two halves of the game's command list need the split: lmxxf, mochizuki and
// OpenRR. Like NrBackend, OpenRR's Enabled counts as the game started with it.
bool SubmissionHooksWanted()
{
    static const bool openNrRr = Config::Instance()->OpenNrRrEnabled.value_or_default();
    const auto kind = ActiveKindFromConfig();
    return (LmxxfWired() && kind == Kind::Lmxxf) || kind == Kind::Mochizuki || openNrRr;
}

bool PresentRuntimeSupported()
{
    const auto kind = ActiveKindFromConfig();
    return kind == Kind::Daniel || kind == Kind::Mochizuki;
}

MochizukiPlace MochizukiPlacementNow()
{
    static const MochizukiPlace asked = []
    {
        const auto key = Config::Instance()->MochizukiPlacement.value_or_default();
        return _stricmp(key.c_str(), "after") == 0     ? MochizukiPlace::AfterSr
               : _stricmp(key.c_str(), "present") == 0 ? MochizukiPlace::Present
                                                       : MochizukiPlace::BeforeSr;
    }();
    if (asked == MochizukiPlace::BeforeSr || ActiveKindFromConfig() != Kind::Mochizuki)
        return MochizukiPlace::BeforeSr;
    const auto& state = State::Instance();
    // Not selected: no present yet, or one that came through frame generation's hook, which only D3D12 games have.
    const bool d3d12 = state.swapchainApi == API::DX12 || state.swapchainApi == API::NotSelected;
    return d3d12 && state.swapchainInteropApi == SwapchainInteropApi::None ? asked : MochizukiPlace::BeforeSr;
}
} // namespace DlssNr::Backend
