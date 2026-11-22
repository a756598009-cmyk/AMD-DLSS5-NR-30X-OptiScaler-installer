#pragma once
#include <d3d12.h>
#include <cstdint>
#include <unordered_map>

namespace DlssNr::Submission
{
// Tracks barriers seen on the logical list for split admission and
// post-Execute promotion/decay expectations (plan D / M3).
// Not a full D3D12 state validator — fail-closed on known-dangerous patterns.
class ResourceStateBook
{
    // Last known state after barriers on this logical recording.
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> states;
    bool openSplitBarrier = false;
    // Enhanced split barriers begun (SyncAfter SPLIT) and not yet ended (SyncBefore SPLIT).
    uint32_t openEnhancedSplits = 0;
    bool sawUnorderedAccess = false;

  public:
    void Reset()
    {
        states.clear();
        openSplitBarrier = false;
        openEnhancedSplits = 0;
        sawUnorderedAccess = false;
    }

    // States that do NOT decay across ExecuteCommandLists (Microsoft M3).
    static bool SurvivesExecute(D3D12_RESOURCE_STATES s)
    {
        const D3D12_RESOURCE_STATES keep =
            D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_RENDER_TARGET |
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST |
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE |
            D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE | D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE |
            D3D12_RESOURCE_STATE_STREAM_OUT;
        return (s & keep) != 0;
    }

    // After producer Execute, decay tracked states to the expected post-Execute
    // values so continuation-side tracking starts from the boundary, not a wipe.
    void ApplyExecuteDecay()
    {
        for (auto& kv : states)
        {
            if (!SurvivesExecute(kv.second))
                kv.second = D3D12_RESOURCE_STATE_COMMON;
        }
        openSplitBarrier = false;
        openEnhancedSplits = 0;
        // UAV flag is informational; leave sawUnorderedAccess as-is for diagnostics.
    }

    bool TryGet(ID3D12Resource* r, D3D12_RESOURCE_STATES* out) const
    {
        if (!r || !out)
            return false;
        auto it = states.find(r);
        if (it == states.end())
            return false;
        *out = it->second;
        return true;
    }

    // Returns false + reason if this barrier batch makes a later Split unsafe.
    bool OnBarriers(UINT n, const D3D12_RESOURCE_BARRIER* b, const char** reasonOut)
    {
        if (reasonOut)
            *reasonOut = nullptr;
        if (!b || n == 0)
            return true;
        for (UINT i = 0; i < n; ++i)
        {
            const D3D12_RESOURCE_BARRIER& bar = b[i];
            if (bar.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
            {
                // The barrier is already recorded in the producer before a cut. The
                // continuation executes later on the same queue, preserving its order.
                // Aliased resources no longer have reliable entries in this state book.
                if (!bar.Aliasing.pResourceBefore || !bar.Aliasing.pResourceAfter)
                    states.clear();
                else
                {
                    states.erase(bar.Aliasing.pResourceBefore);
                    states.erase(bar.Aliasing.pResourceAfter);
                }
                continue;
            }
            if (bar.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
            {
                sawUnorderedAccess = true;
                // UAV barrier alone is OK to track; do not reject yet.
                continue;
            }
            if (bar.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                continue;
            const auto flags = bar.Flags;
            if (flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)
                openSplitBarrier = true;
            if (flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY)
            {
                if (!openSplitBarrier)
                {
                    if (reasonOut)
                        *reasonOut = "split_barrier_end_without_begin";
                    return false;
                }
                openSplitBarrier = false;
            }
            if (bar.Transition.pResource)
                states[bar.Transition.pResource] = bar.Transition.StateAfter;
        }
        return true;
    }

    // Enhanced barriers: layouts survive ExecuteCommandLists and every access before the boundary is complete
    // after it, so a cut is safe unless a split barrier is open across it.
    bool OnEnhancedBarriers(UINT32 numGroups, const D3D12_BARRIER_GROUP* groups, const char** reasonOut)
    {
        if (reasonOut)
            *reasonOut = nullptr;
        for (UINT32 g = 0; groups && g < numGroups; ++g)
        {
            const D3D12_BARRIER_GROUP& group = groups[g];
            for (UINT32 i = 0; i < group.NumBarriers; ++i)
            {
                D3D12_BARRIER_SYNC before, after;
                switch (group.Type)
                {
                case D3D12_BARRIER_TYPE_GLOBAL:
                    before = group.pGlobalBarriers[i].SyncBefore;
                    after = group.pGlobalBarriers[i].SyncAfter;
                    break;
                case D3D12_BARRIER_TYPE_TEXTURE:
                    before = group.pTextureBarriers[i].SyncBefore;
                    after = group.pTextureBarriers[i].SyncAfter;
                    break;
                case D3D12_BARRIER_TYPE_BUFFER:
                    before = group.pBufferBarriers[i].SyncBefore;
                    after = group.pBufferBarriers[i].SyncAfter;
                    break;
                default:
                    if (reasonOut)
                        *reasonOut = "enhanced_barrier_unknown_type";
                    return false;
                }
                if (before == D3D12_BARRIER_SYNC_SPLIT)
                {
                    if (!openEnhancedSplits)
                    {
                        if (reasonOut)
                            *reasonOut = "enhanced_split_end_without_begin";
                        return false;
                    }
                    --openEnhancedSplits;
                }
                if (after == D3D12_BARRIER_SYNC_SPLIT)
                    ++openEnhancedSplits;
            }
        }
        return true;
    }

    // Call before Split: open BEGIN_ONLY without END must refuse the cut.
    bool CanSplit(const char** reasonOut) const
    {
        if (reasonOut)
            *reasonOut = nullptr;
        if (openSplitBarrier)
        {
            if (reasonOut)
                *reasonOut = "open_split_barrier";
            return false;
        }
        if (openEnhancedSplits)
        {
            if (reasonOut)
                *reasonOut = "open_enhanced_split_barrier";
            return false;
        }
        return true;
    }

    size_t TrackedCount() const { return states.size(); }
};
} // namespace DlssNr::Submission
