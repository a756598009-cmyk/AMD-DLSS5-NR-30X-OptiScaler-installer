#pragma once
#include "Host.h"
#include "LmxxfEvaluateCut.h"
#include "LmxxfColorProbe.h"
#include "LmxxfStagingProbe.h"
#include <atomic>
#include <filesystem>
#include <mutex>

struct MochizukiNrInfo; // mochizuki_runtime/MochizukiNrControls.h

namespace DlssNr::Backend
{
// Full Host for lmxxf, and for mochizuki's runtime, which implements the same ABI.
// Record: PrepareFrame → RecordInputs → Split → RecordOutputs → SetPendingEnqueue(EnqueueHip).
class LmxxfBackend final : public Host
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr; // Swapped only by MigrateQueue, under recordMutex and jobMutex.
    std::filesystem::path directory;
    std::wstring runtimeFile;   // LmxxfNrRuntime.dll or MochizukiNrRuntime.dll
    void* runtimeDll = nullptr; // HMODULE
    void* session = nullptr;
    bool sessionReady = false;
    // mochizuki: failed session starts since the last good one, when the next may run, and the permanent latch
    // set when the runtime reports this GPU or driver unsupported. Render thread only.
    uint32_t sessionFailures = 0;
    ULONGLONG nextSessionRetry = 0;
    bool sessionUnavailable = false;
    uint64_t frameId = 0; // Advanced only when a frame reaches PrepareFrame.
    float GameExposure(ID3D12GraphicsCommandList* cmd, const AmdPreSr::Frame& frame);
    // Both strings are written by the render, submit and exit threads and copied by the menu.
    mutable std::mutex statusMutex;
    std::string status { "lmxxf: idle" };
    std::string runtimeStatus;           // api GetStatus, refreshed by Record at most every 500 ms
    ULONGLONG runtimeStatusTick = 0;     // Render thread only.
    std::atomic<bool> exiting { false }; // Set by OnProcessExit; every entry point returns early after it.
    // InvalidateHistory found a Record running (the menu never waits on one); the next Record resets instead.
    std::atomic<bool> historyResetPending { false };
    // Function table copied from LmxxfNrGetApi (opaque here to keep header free of C ABI).
    struct Api;
    Api* api = nullptr;
    struct PendingJobInfo
    {
        void* job = nullptr;
        ID3D12CommandList* cmd = nullptr;
    };
    mutable std::mutex recordMutex; // The runtime session has one active job.
    mutable std::mutex jobMutex;
    PendingJobInfo pendingJobInfo;
    // pendingJobInfo.cmd, written under jobMutex and read without it: every proxy Reset and final Release reaches
    // ListRecycled, and one on a list without the job must cost a load, not the lock.
    std::atomic<ID3D12CommandList*> pendingList { nullptr };
    // ListRecycled is cancelling the pending job outside jobMutex (under jobMutex).
    bool cancelling = false;
    // The queue that executed the last job, when it is not the session's (AddRef'd; under jobMutex). Record
    // migrates the session to it on the render thread, never the submit thread.
    ID3D12CommandQueue* migrateTo = nullptr;
    LmxxfProbe::Mode diagnostic = LmxxfProbe::Mode::Off;
    LmxxfProbe::ColorCopy colorProbe;
    // mochizuki's guides as its runtime takes them (MochizukiGuides.h), and the last frame's jitter for their step.
    std::unique_ptr<class MochizukiGuides> guides;
    float lastJitterX = 0.f, lastJitterY = 0.f;
    bool haveJitter = false;
    // The last frame that reached PrepareFrame, to tell a second Evaluate of that same frame (NBA 2K27 sends one
    // before the first list is executed) from a new one while its job is pending. Render thread, under recordMutex.
    struct LastFrame
    {
        ID3D12Resource* colour = nullptr;
        UINT width = 0, height = 0;
        float jitterX = 0.f, jitterY = 0.f;
        ULONGLONG tick = 0;     // GetTickCount64 when it was recorded
        ULONGLONG interval = 0; // from the frame before it, 0 when unknown
    } lastFrame;
    bool repeatLogged = false;
    bool boundaryLogged = false;
    // Person protection: the last Record's first pass (LMXXF_NR_FRAME_FLAG_FIRST_PASS), and whether the runtime
    // refused the flag (one from before it), which then is not asked again.
    ID3D12Resource* firstPass = nullptr;
    bool firstPassRefused = false;
    LmxxfProbe::StagingProbe stagingProbe;
    uint64_t probeEvaluateId = 0;   // Host ordinal, NOT an engine frame ID or GPU completion.
    uint64_t evaluateSequence_ = 0; // Monotonic sequence across all Evaluate calls including bypassed.
    uint64_t boundaryProxyHits = 0;
    uint64_t boundaryCuts = 0;
    uint64_t boundaryRejects = 0;
    ID3D12Resource* RecordDiagnostic(ID3D12GraphicsCommandList*, const AmdPreSr::Frame&);

    bool EnsureRuntime();
    ID3D12Resource* FinishRecord(ID3D12GraphicsCommandList* recordCmd, void* jobHandle, void* privateOutput);
    bool ownLists = false; // the constructor's: see RecordConsumer
    ID3D12Resource* RecordProducer(ID3D12GraphicsCommandList* producer, void* jobHandle, void* privateOutput);
    bool EnsureSession();
    bool SessionStartFailed(const char* step, const char* err);
    void MigrateQueue(ID3D12CommandQueue* q);
    void RefreshRuntimeStatus();
    void ClearRuntimeStatus();
    void ResetHistoryNow();
    void SendControls();
    // Submission::Hooks::g_onListRecycled: routes to the backend constructed last, until it unregisters.
    static void OnListRecycled(ID3D12CommandList* list);
    void ListRecycled(ID3D12CommandList* list);
    void UnregisterListRecycled();
    // Every runtime but mochizuki speaks plain lmxxf (LmxxfNrRuntime.dll, Rdna2NrRuntime.dll).
    bool Lmxxf() const { return runtimeFile != L"MochizukiNrRuntime.dll"; }
    // HexNeural's Rdna2NrRuntime.dll: a failed start is retried with mochizuki's backoff, not on every frame.
    bool Rdna2() const { return runtimeFile == L"Rdna2NrRuntime.dll"; }
    uint32_t FrameInfoSize() const;
    void SetStatus(const char* s);

  public:
    LmxxfBackend(ID3D12Device* device, ID3D12CommandQueue* queue, const std::filesystem::path& directory,
                 const wchar_t* runtime = L"LmxxfNrRuntime.dll", bool ownLists = false);
    ~LmxxfBackend() override;
    LmxxfBackend(const LmxxfBackend&) = delete;
    LmxxfBackend& operator=(const LmxxfBackend&) = delete;

    ID3D12Resource* Record(ID3D12GraphicsCommandList*, const AmdPreSr::Frame&, const AmdPreSr::Settings&) override;
    int PendingListIndex(UINT, ID3D12CommandList* const*) const override;
    void Submitting(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) override;
    void TraceBoundary(const std::string&) override;
    void Submitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) override;
    bool Shutdown() override;
    void OnProcessExit() override;
    void InvalidateHistory() override;
    std::string Status() const override;
    std::string RuntimeStatus() const override;
    ID3D12Resource* FirstPass() const override { return firstPass; }
    bool GraphicsRestartNeeded(UINT activePasses) const override;
    // What mochizuki's MochizukiNrGetInfo said when the render thread last asked (at most every 500 ms, with the
    // runtime status), for the menu. False while no mochizuki session runs. Never waits on a frame being recorded.
    static bool MochizukiInfo(MochizukiNrInfo& out);
    // NR without upscaling (constructed with ownLists): the host owns its command lists, so Record takes the producer
    // list and records only the inputs; RecordConsumer adds the outputs to a second list, and SubmitOwnLists executes
    // the producer, runs the network after it on the queue, then executes the consumer and retires the job.
    bool RecordConsumer(ID3D12GraphicsCommandList* consumer);
    // exec: how the lists reach the queue, for a caller inside a submission (the between slot holds the queue hook,
    // so it passes the raw ExecuteCommandLists); null = the queue's own.
    using ExecFn = void(WINAPI*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    void SubmitOwnLists(ID3D12CommandQueue* q, ID3D12CommandList* producer, ID3D12CommandList* consumer,
                        ExecFn exec = nullptr);
    // Own lists that will not be submitted: the job recorded into them is dropped.
    void CancelOwnLists();
};
} // namespace DlssNr::Backend
