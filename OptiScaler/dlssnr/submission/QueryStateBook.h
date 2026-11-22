#pragma once
#include <d3d12.h>
#include <cstdio>
#include <functional>
#include <unordered_set>

namespace DlssNr::Submission
{
// A non-timestamp BeginQuery/EndQuery pair must stay on one native command list.
// Completed queries and timestamp EndQuery calls do not prevent a later cut. An open statistics query does not
// either: it is ended before the cut and begun again after it, so it counts only the part after the cut (a profiler
// or HUD number, not something the image depends on). An open occlusion query still refuses the cut.
class QueryStateBook
{
    struct Key
    {
        ID3D12QueryHeap* heap;
        D3D12_QUERY_TYPE type;
        UINT index;

        bool operator==(const Key& other) const
        {
            return heap == other.heap && type == other.type && index == other.index;
        }
    };

    struct Hash
    {
        size_t operator()(const Key& key) const
        {
            const size_t h = std::hash<ID3D12QueryHeap*> {}(key.heap);
            return h ^ (static_cast<size_t>(key.type) << 16) ^ static_cast<size_t>(key.index);
        }
    };

    std::unordered_set<Key, Hash> open;

    static bool Statistics(D3D12_QUERY_TYPE type)
    {
        return type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS || type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1 ||
               (type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 && type <= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3);
    }

    static const char* Name(D3D12_QUERY_TYPE type)
    {
        switch (type)
        {
        case D3D12_QUERY_TYPE_OCCLUSION:
            return "OCCLUSION";
        case D3D12_QUERY_TYPE_BINARY_OCCLUSION:
            return "BINARY_OCCLUSION";
        case D3D12_QUERY_TYPE_PIPELINE_STATISTICS:
            return "PIPELINE_STATISTICS";
        case D3D12_QUERY_TYPE_PIPELINE_STATISTICS1:
            return "PIPELINE_STATISTICS1";
        default:
            return Statistics(type) ? "SO_STATISTICS" : "OTHER";
        }
    }

  public:
    void Reset() { open.clear(); }

    bool Begin(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index)
    {
        if (type == D3D12_QUERY_TYPE_TIMESTAMP)
            return false;
        return open.insert({ heap, type, index }).second;
    }

    bool End(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index)
    {
        if (type == D3D12_QUERY_TYPE_TIMESTAMP)
            return true;
        return open.erase({ heap, type, index }) == 1;
    }

    bool CanSplit() const
    {
        for (const Key& key : open)
            if (!Statistics(key.type))
                return false;
        return true;
    }

    // Around a cut: end the open statistics queries on the list before it, begin them again on the one after.
    void EndOpen(ID3D12GraphicsCommandList* list) const
    {
        for (const Key& key : open)
            list->EndQuery(key.heap, key.type, key.index);
    }
    void BeginOpen(ID3D12GraphicsCommandList* list) const
    {
        for (const Key& key : open)
            list->BeginQuery(key.heap, key.type, key.index);
    }

    // "open_query(OCCLUSION index 3, 2 open)": the first query that refuses the cut.
    void Describe(char* out, size_t size) const
    {
        for (const Key& key : open)
            if (!Statistics(key.type))
            {
                std::snprintf(out, size, "open_query(%s index %u, %zu open)", Name(key.type), key.index,
                              open.size());
                return;
            }
        std::snprintf(out, size, "open_query");
    }
};
} // namespace DlssNr::Submission
