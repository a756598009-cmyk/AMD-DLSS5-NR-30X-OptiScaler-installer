#pragma once

#include <Config.h>
#include <Logger.h>
#include <State.h>
#include <proxies/XeFG_Proxy.h>

#include <algorithm>
#include <atomic>
#include <climits>

// The menu's XeFG choices for the child-window presenters of the Vulkan and OpenGL routes.
namespace AmdPresentExperimental
{
// What the running presenter reports, for the menu. Zero while no presenter runs.
inline std::atomic<int> childFgMaximum = 0;
inline std::atomic<int> childFgInterpolated = 0;
// Whether the presenter's window shows XeFG's frames, for the FPS overlay.
inline std::atomic<bool> childFgShown = false;
// The frames a second XeFG presents, at most the display's refresh rate when they cannot tear, for the FPS overlay.
// Zero when the presenter does not measure it.
inline std::atomic<float> childFgOutputFps = 0;

struct ChildFgSettings
{
    int interpolated = 0;
    int cap = INT_MAX;
    bool debugView = false;
    bool onlyGenerated = false;

    // Auto (no value) is two generated frames. A count XeFG refuses falls back to one for the rest of the
    // presenter's life. Returns false when XeFG accepts not even one.
    bool Apply(xefg_swapchain_handle_t context, int maximum, const char* api)
    {
        int requested = std::clamp(Config::Instance()->FGXeFGInterpolationCount.value_or(2), 1,
                                   std::max(1, std::min(maximum, cap)));
        if (requested != interpolated && XeFGProxy::SetNumInterpolatedFrames())
        {
            auto result = XeFGProxy::SetNumInterpolatedFrames()(context, requested);
            if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS && requested > 1)
            {
                LOG_WARN("{} XeFG: {} generated frames refused ({}), using one", api, requested, (int) result);
                cap = requested = 1;
                result = XeFGProxy::SetNumInterpolatedFrames()(context, requested);
            }
            if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
                return false;
            LOG_INFO("{} XeFG: {} generated frames per frame", api, requested);
        }
        interpolated = requested;
        if (auto enable = XeFGProxy::EnableDebugFeature())
        {
            const bool view = Config::Instance()->FGXeFGDebugView.value_or_default();
            if (view != debugView && enable(context, XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES, view,
                                            nullptr) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
                debugView = view;
            const bool only = State::Instance().fgOnlyGenerated;
            if (only != onlyGenerated && enable(context, XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION, only,
                                                nullptr) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
                onlyGenerated = only;
        }
        childFgMaximum = maximum;
        childFgInterpolated = interpolated;
        return true;
    }

    void Reset()
    {
        *this = {};
        childFgMaximum = 0;
        childFgInterpolated = 0;
    }
};
} // namespace AmdPresentExperimental
