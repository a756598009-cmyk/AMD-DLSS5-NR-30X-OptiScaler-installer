#pragma once
// A network job the GPU has not finished long after it was submitted. Two players on an RX 9060 XT
// with danielblnc's runtime saw single jobs take 3.8 to 4.2 s, every 10 to 30 minutes; the game
// froze for those seconds and came back, until two came 14 s apart and the whole machine locked,
// power button included. One is the warning, so the network stands down on it; the player can switch it on again.
//
// Timed on a thread-pool timer from the submission, not at the next present: while the GPU is held
// the game presents nothing either, and a loading screen that presents nothing looks the same from
// there. Here a loading screen does not: what was submitted finishes in milliseconds whether the
// game presents again or not. The oldest unfinished value is the one timed, so a later submission
// does not restart the clock on a job that is stuck. A device that is gone reports UINT64_MAX,
// complete, and is the device-removed path's to handle.
//
// The ReShade add-on carries the same file (core/shared/stall_watch.h).
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>

namespace stallwatch
{

// Where the queue starts the next job armed on this thread, when the caller can name it: a fence value signalled on
// that queue just before the job. The clock starts once the queue got there, not at the submission. NR without
// upscaling queues its job behind the game's own frame and its waits on other APIs (the D3D11 bridge's shared fence,
// the DX11-to-DX12 bridge's copy), and a game queue held for seconds (an AMD ReLive capture, a mode change, a long
// loading frame) is not an NR job the GPU held. Set it around the Submitted call with Gate.
struct Start
{
    ID3D12Fence* fence = nullptr;
    UINT64 value = 0;
};
inline thread_local Start nextStart;
struct Gate
{
    Gate(ID3D12Fence* f, UINT64 v) { nextStart = { f, v }; }
    ~Gate() { nextStart = {}; }
};

struct Watch
{
    std::mutex lock;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence, start;
    // since stays 0 until the queue reaches `start` (Gate above).
    UINT64 value = 0, since = 0, startValue = 0;
    std::atomic<UINT64> limitMs { 2000 }, trippedMs { 0 };
    PTP_TIMER timer = nullptr;

    // Never destroyed: the timer may still be running its last tick as the module goes.
    static Watch& Get()
    {
        static Watch& w = *new Watch;
        return w;
    }

    static void CALLBACK Tick(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER)
    {
        auto& w = *static_cast<Watch*>(context);
        std::lock_guard guard(w.lock);
        const UINT64 limit = w.limitMs.load();
        if (w.fence == nullptr || w.value == 0 || limit == 0 || w.trippedMs.load() != 0)
            return;
        if (w.fence->GetCompletedValue() >= w.value)
            return;
        if (w.since == 0)
        {
            if (w.start != nullptr && w.start->GetCompletedValue() < w.startValue)
                return;
            w.start = nullptr;
            w.since = GetTickCount64();
            return;
        }
        const UINT64 held = GetTickCount64() - w.since;
        if (held >= limit)
            w.trippedMs.store(held);
    }

    // After each submission: `value` is what `fence` reaches when it is done. Kept while the one
    // before it is still unfinished.
    void Arm(ID3D12Fence* f, UINT64 v)
    {
        if (f == nullptr || v == 0 || limitMs.load() == 0)
            return;
        std::lock_guard guard(lock);
        if (fence == nullptr || fence.Get() != f || fence->GetCompletedValue() >= value)
        {
            fence = f;
            value = v;
            start = nextStart.fence;
            startValue = nextStart.value;
            since = start == nullptr ? GetTickCount64() : 0;
        }
        if (timer == nullptr)
        {
            timer = CreateThreadpoolTimer(Tick, this, nullptr);
            if (timer != nullptr)
            {
                FILETIME due {};
                ULARGE_INTEGER at {};
                at.QuadPart = static_cast<ULONGLONG>(-250LL * 10000); // 250 ms from now
                due.dwLowDateTime = at.LowPart;
                due.dwHighDateTime = at.HighPart;
                SetThreadpoolTimer(timer, &due, 250, 50);
            }
        }
    }

    // How long the job that stood the network down had been running; 0 while none has.
    UINT64 Tripped() const { return trippedMs.load(); }

    // After a stand-down, so the network can be switched on again in the same session: the job that
    // tripped it is forgotten, and the next submission starts the clock afresh.
    void Clear()
    {
        std::lock_guard guard(lock);
        fence = nullptr;
        start = nullptr;
        value = since = 0;
        trippedMs.store(0);
    }

    // When the module unloads without the process ending: no tick may run after it. At process
    // exit the timer goes with everything else.
    void Stop(bool processExit)
    {
        if (timer == nullptr || processExit)
            return;
        SetThreadpoolTimer(timer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(timer, TRUE);
        CloseThreadpoolTimer(timer);
        timer = nullptr;
    }
};

} // namespace stallwatch
