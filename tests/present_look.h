// Every frame a D3D12 swapchain of this process presents, read back on that swapchain's own queue just before its
// Present: eight narrow columns spread across the width, enough to find rows of black that cross the picture, and the
// GPU time at which the queue reached that present. This is what the swapchain hands to the display, generated frames
// included, before any compositor, so tearing and the screen's refresh rate do not hide anything. The Present slots of
// the DXGI swapchain class are patched (InstallPresentLook) once the swapchain and its queue are known.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct PresentLook
{
    unsigned frames = 0, black = 0, rows = 0, run = 0, dropped = 0, partial = 0;
    // Time between the moments the presenting queue reached consecutive presents (GPU clock): p10/p50/p90 and the
    // gaps under a quarter of the mean, presents that reach the screen as one.
    double gapP10 = 0, gapP50 = 0, gapP90 = 0;
    unsigned bunched = 0;
    // Mean absolute difference (0..255) between each presented frame and the one before it, below the top quarter
    // where an FPS overlay sits: with tearing, every present that reaches the screen inside a refresh is a band of the
    // picture, and this step is how much the band stands out. On a still scene it should stay near 0.
    double stepP50 = 0, stepP90 = 0, stepMax = 0;
};

namespace present_look
{
constexpr UINT kStrips = 8, kStripWidth = 2, kSlots = 32, kPitch = 256;
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
struct Slot
{
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* readback = nullptr;
    UINT64 value = 0, sequence = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    double ms = 0;
};
struct State
{
    std::mutex mutex;
    PresentFn present = nullptr;
    Present1Fn present1 = nullptr;
    void** table = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12QueryHeap* stamps = nullptr;
    UINT64 frequency = 0, firstStamp = 0;
    Slot slots[kSlots];
    UINT64 signaled = 0, sequence = 0;
    std::atomic<bool> on { false };
    // The swapchain to read and the queue it presents on (GetDevice does not give a D3D12 swapchain's queue).
    std::atomic<void*> swap { nullptr }, queue { nullptr };
    PresentLook look;
    std::vector<double> gpu, steps;
    std::vector<BYTE> previous;
    std::string lines = "present_ms,gpu_ms,step,black_rows,partial_rows,first_black_row,last_black_row\n";
    LARGE_INTEGER rate {}, start {};
};
inline State state;

// Rows where at least six of the eight columns are black make a black row; three or more, a partial one.
inline void Analyse(Slot& slot)
{
    BYTE* bits = nullptr;
    const D3D12_RANGE range { 0, SIZE_T(kPitch) * slot.height + sizeof(UINT64) };
    if (FAILED(slot.readback->Map(0, &range, reinterpret_cast<void**>(&bits))))
        return;
    const bool tenBit = slot.format == DXGI_FORMAT_R10G10B10A2_UNORM;
    unsigned rows = 0, partial = 0, run = 0, longest = 0;
    int first = -1, last = -1;
    for (UINT y = 0; y < slot.height; ++y)
    {
        unsigned dark = 0;
        for (UINT s = 0; s < kStrips; ++s)
        {
            bool all = true;
            for (UINT x = 0; x < kStripWidth; ++x)
            {
                const BYTE* p = bits + size_t(y) * kPitch + size_t(s * kStripWidth + x) * 4;
                UINT top = 0;
                if (tenBit)
                {
                    const UINT v = *reinterpret_cast<const UINT*>(p);
                    top = std::max({ v & 1023u, (v >> 10) & 1023u, (v >> 20) & 1023u }) >> 2;
                }
                else
                    top = std::max({ p[0], p[1], p[2] });
                all = all && top <= 8;
            }
            dark += all;
        }
        const bool black = dark >= 6;
        rows += black;
        partial += dark >= 3;
        run = black ? run + 1 : 0;
        longest = std::max(longest, run);
        if (black)
        {
            if (first < 0)
                first = int(y);
            last = int(y);
        }
    }
    UINT64 stamp = 0;
    std::memcpy(&stamp, bits + size_t(kPitch) * slot.height, sizeof(stamp));
    // Drain reads the frames in present order on one thread, so the previous frame's columns are this one's
    // predecessor.
    const size_t row = kStrips * kStripWidth * 4, size = row * slot.height;
    double step = -1;
    if (!tenBit && state.previous.size() == size)
    {
        double sum = 0;
        size_t count = 0;
        for (UINT y = slot.height / 4; y < slot.height; ++y)
            for (size_t i = 0; i < row; ++i)
                if (i % 4 != 3)
                {
                    sum += std::abs(int(bits[size_t(y) * kPitch + i]) - int(state.previous[y * row + i]));
                    ++count;
                }
        step = count ? sum / double(count) : 0;
    }
    state.previous.resize(size);
    for (UINT y = 0; y < slot.height; ++y)
        std::memcpy(state.previous.data() + y * row, bits + size_t(y) * kPitch, row);
    slot.readback->Unmap(0, nullptr);
    std::lock_guard lock(state.mutex);
    if (step >= 0)
        state.steps.push_back(step);
    if (!state.firstStamp)
        state.firstStamp = stamp;
    const double gpuMs = state.frequency ? double(stamp - state.firstStamp) * 1000.0 / double(state.frequency) : 0.0;
    state.gpu.push_back(gpuMs);
    ++state.look.frames;
    state.look.black += rows > 0;
    state.look.rows += rows;
    state.look.partial += partial > 0;
    state.look.run = std::max(state.look.run, longest);
    char line[128];
    sprintf_s(line, "%.3f,%.3f,%.3f,%u,%u,%d,%d\n", slot.ms, gpuMs, step, rows, partial, first, last);
    state.lines += line;
}

inline void Capture(IDXGISwapChain* swap)
{
    if (!state.on || swap != state.swap.load())
        return;
    auto queue = static_cast<ID3D12CommandQueue*>(state.queue.load());
    queue->AddRef();
    IDXGISwapChain3* swap3 = nullptr;
    ID3D12Resource* back = nullptr;
    std::unique_lock lock(state.mutex);
    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swap3))) &&
        SUCCEEDED(swap3->GetBuffer(swap3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource),
                                   reinterpret_cast<void**>(&back))))
    {
        const D3D12_RESOURCE_DESC desc = back->GetDesc();
        if (!state.device)
        {
            queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&state.device));
            state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                      reinterpret_cast<void**>(&state.fence));
            D3D12_QUERY_HEAP_DESC heap { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots, 0 };
            state.device->CreateQueryHeap(&heap, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&state.stamps));
            queue->GetTimestampFrequency(&state.frequency);
        }
        // A slot is free again once its frame has been read; a present that finds the next one still busy is not read.
        const UINT index = UINT(state.sequence % kSlots);
        Slot& slot = state.slots[index];
        if (slot.sequence != 0)
            ++state.look.dropped;
        else
        {
            if (!slot.allocator)
            {
                state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                     reinterpret_cast<void**>(&slot.allocator));
                state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator, nullptr,
                                                __uuidof(ID3D12GraphicsCommandList),
                                                reinterpret_cast<void**>(&slot.list));
                slot.list->Close();
            }
            if (!slot.readback || slot.height < desc.Height)
            {
                if (slot.readback)
                    slot.readback->Release();
                D3D12_HEAP_PROPERTIES heap { D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC buffer {};
                buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                buffer.Width = UINT64(kPitch) * desc.Height + kPitch;
                buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
                buffer.SampleDesc.Count = 1;
                buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                state.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
                                                      reinterpret_cast<void**>(&slot.readback));
            }
            slot.allocator->Reset();
            slot.list->Reset(slot.allocator, nullptr);
            slot.list->EndQuery(state.stamps, D3D12_QUERY_TYPE_TIMESTAMP, index);
            D3D12_RESOURCE_BARRIER barrier {};
            barrier.Transition = { back, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE };
            slot.list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION src { back, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            D3D12_TEXTURE_COPY_LOCATION dst { slot.readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint.Footprint = { desc.Format, kStrips * kStripWidth, desc.Height, 1, kPitch };
            const UINT width = UINT(desc.Width);
            for (UINT s = 0; s < kStrips; ++s)
            {
                const UINT x = (width * (2 * s + 1)) / (2 * kStrips);
                const D3D12_BOX box { x, 0, 0, x + kStripWidth, desc.Height, 1 };
                slot.list->CopyTextureRegion(&dst, s * kStripWidth, 0, 0, &src, &box);
            }
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            slot.list->ResourceBarrier(1, &barrier);
            slot.list->ResolveQueryData(state.stamps, D3D12_QUERY_TYPE_TIMESTAMP, index, 1, slot.readback,
                                        UINT64(kPitch) * desc.Height);
            slot.list->Close();
            ID3D12CommandList* lists[] = { slot.list };
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(state.fence, ++state.signaled);
            LARGE_INTEGER now {};
            QueryPerformanceCounter(&now);
            slot.value = state.signaled;
            slot.sequence = ++state.sequence;
            slot.height = desc.Height;
            slot.format = desc.Format;
            slot.ms = double(now.QuadPart - state.start.QuadPart) * 1000.0 / double(state.rate.QuadPart);
        }
    }
    lock.unlock();
    if (back)
        back->Release();
    if (swap3)
        swap3->Release();
    queue->Release();
}

inline HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swap, UINT interval, UINT flags)
{
    if (!(flags & DXGI_PRESENT_TEST))
        Capture(swap);
    return state.present(swap, interval, flags);
}
inline HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* swap, UINT interval, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* parameters)
{
    if (!(flags & DXGI_PRESENT_TEST))
        Capture(swap);
    return state.present1(swap, interval, flags, parameters);
}

// Reads finished slots in the order they were presented.
inline void Drain()
{
    for (;;)
    {
        Slot* next = nullptr;
        {
            std::lock_guard lock(state.mutex);
            for (auto& slot : state.slots)
                if (slot.value && state.fence->GetCompletedValue() >= slot.value &&
                    (!next || slot.sequence < next->sequence))
                    next = &slot;
            if (next)
                next->value = 0;
        }
        if (!next)
            return;
        Analyse(*next);
        std::lock_guard lock(state.mutex);
        next->sequence = 0;
    }
}
} // namespace present_look

// Points the Present and Present1 slots of a swapchain vtable at the given functions and returns what was there.
inline bool PatchPresentSlots(void** table, void* present, void* present1, void** oldPresent, void** oldPresent1)
{
    DWORD old = 0;
    if (!VirtualProtect(table + 8, sizeof(void*) * 15, PAGE_READWRITE, &old))
        return false;
    *oldPresent = table[8];
    *oldPresent1 = table[22];
    table[8] = present;
    table[22] = present1;
    VirtualProtect(table + 8, sizeof(void*) * 15, old, &old);
    return true;
}

// real: a real DXGI swapchain object of this process (not a wrapper); queue: the D3D12 queue it presents from.
inline bool InstallPresentLook(IDXGISwapChain* real, ID3D12CommandQueue* queue)
{
    using namespace present_look;
    std::lock_guard lock(state.mutex);
    if (state.swap.load() != real)
    {
        queue->AddRef();
        if (auto old = static_cast<ID3D12CommandQueue*>(state.queue.exchange(queue)))
            old->Release();
        state.swap = real;
    }
    if (state.table)
        return true;
    void** table = *reinterpret_cast<void***>(real);
    // The originals are known before the slots change: another thread may present through them at once.
    state.present = reinterpret_cast<PresentFn>(table[8]);
    state.present1 = reinterpret_cast<Present1Fn>(table[22]);
    QueryPerformanceFrequency(&state.rate);
    void *present = nullptr, *present1 = nullptr;
    if (!PatchPresentSlots(table, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void*>(&HookPresent1),
                           &present, &present1))
        return false;
    state.table = table;
    return true;
}

// Reads every frame presented for the given time and returns what it found; the per-present lines go to csv.
inline PresentLook LookAtPresents(double seconds, const std::wstring& csv)
{
    using namespace present_look;
    {
        std::lock_guard lock(state.mutex);
        state.look = {};
        state.gpu.clear();
        state.steps.clear();
        state.previous.clear();
        state.firstStamp = 0;
        state.lines.resize(state.lines.find('\n') + 1);
        QueryPerformanceCounter(&state.start);
    }
    state.on = true;
    const ULONGLONG end = GetTickCount64() + ULONGLONG(seconds * 1000);
    while (GetTickCount64() < end)
    {
        if (state.fence)
            Drain();
        Sleep(1);
    }
    state.on = false;
    for (int i = 0; i < 200 && state.fence && state.signaled > state.fence->GetCompletedValue(); ++i)
        Sleep(5);
    if (state.fence)
        Drain();
    std::lock_guard lock(state.mutex);
    std::vector<double> gaps;
    for (size_t i = 1; i < state.gpu.size(); ++i)
        gaps.push_back(state.gpu[i] - state.gpu[i - 1]);
    if (!gaps.empty())
    {
        const double mean = (state.gpu.back() - state.gpu.front()) / double(gaps.size());
        std::sort(gaps.begin(), gaps.end());
        state.look.gapP10 = gaps[gaps.size() / 10];
        state.look.gapP50 = gaps[gaps.size() / 2];
        state.look.gapP90 = gaps[gaps.size() * 9 / 10];
        state.look.bunched = unsigned(std::count_if(gaps.begin(), gaps.end(), [&](double g) { return g < mean / 4; }));
    }
    if (!state.steps.empty())
    {
        auto steps = state.steps;
        std::sort(steps.begin(), steps.end());
        state.look.stepP50 = steps[steps.size() / 2];
        state.look.stepP90 = steps[steps.size() * 9 / 10];
        state.look.stepMax = steps.back();
    }
    if (!csv.empty())
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, csv.c_str(), L"wb") == 0 && file)
        {
            fwrite(state.lines.data(), 1, state.lines.size(), file);
            fclose(file);
        }
    }
    return state.look;
}
