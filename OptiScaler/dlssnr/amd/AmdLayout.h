#pragma once
#include <cstddef>
#include <cstdint>

namespace AmdPreSr
{
// SHA256 as a fixed 32-byte digest. Prefer Sha256FromHex() over a raw
// {0x..} list: a hand-copied byte array that is one nibble short still
// compiles (the rest zero-fills) and then never matches at runtime.
struct Sha256
{
    unsigned char bytes[32];
};

constexpr unsigned char HexNibble(char c)
{
    if (c >= '0' && c <= '9')
        return static_cast<unsigned char>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<unsigned char>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return static_cast<unsigned char>(c - 'A' + 10);
    return 0xFF;
}

constexpr bool IsHexDigit(char c) { return HexNibble(c) <= 0x0F; }

// Exactly 64 hex digits + NUL. Wrong length is a compile error, which is
// the guard for the 0.3.1 digest that was hand-copied one character short.
template <std::size_t N> constexpr Sha256 Sha256FromHex(const char (&hex)[N])
{
    static_assert(N == 65, "SHA256 hex literal must be exactly 64 hex digits (plus NUL)");
    Sha256 out {};
    for (std::size_t i = 0; i < 32; ++i)
    {
        const char h = hex[i * 2];
        const char l = hex[i * 2 + 1];
        if (!IsHexDigit(h) || !IsHexDigit(l))
            return Sha256 {}; // invalid digit → all-zero digest (will not match)
        out.bytes[i] = static_cast<unsigned char>((HexNibble(h) << 4) | HexNibble(l));
    }
    return out;
}

struct AmdLayout
{
    const char* name;
    std::size_t size;
    Sha256 sha256;
    std::uint32_t d3dCompileIat; // 0 if the runtime has no D3DCompile import
    std::uint32_t init;
    std::uint32_t record;
    std::uint32_t notify;
    std::uint32_t shutdown;
    std::uint32_t trampoline;
    std::uint32_t device;
    std::uint32_t queue;
    std::uint32_t engine;
    std::uint32_t historyView;
    std::uint32_t historyValid;
    std::uint32_t initDone;
    std::uint32_t nativeFailure;
    std::uint32_t configuredInline;
    std::uint32_t jobDone;
    std::uint32_t timeoutCount;
    std::uint32_t watchdog;
    std::uint32_t interop;
    std::uint32_t pendingList;
    std::uint32_t jobId;
    std::uint32_t depthInverted;
    std::uint32_t explicitDepth;
    std::uint32_t enabled;
    std::uint32_t temporal;
    std::uint32_t fsrInputs;
    std::uint32_t depthPresent;
    std::uint32_t tonemap;
    std::uint32_t tone;
    std::uint32_t structure;
    std::uint32_t skin;
    std::uint32_t charMask;
    std::uint32_t toneChannels;
    std::uint32_t hipOrdinal;
    // Sticky "staging must be re-created" byte. Set when the runtime detects a
    // resize, a re-created upscaler context, or an INI change; cleared only
    // after it has drained the game's queue and joined its workers. Record
    // tests it as its first act, so 0 means "this call will not rebuild".
    std::uint32_t recreate;
    // Address of the mutex guarding Record. The verified 0.3.1 entry
    // acquires it through a blocking SRW-lock path. +0x4c is its ownership /
    // recursion count, not a waiter count or worker-busy indicator. It cannot
    // explain a refused Record by itself. 0 means the field is not mapped.
    std::uint32_t recordLock;
    // Diagnostic-only fields checked by Record's non-inline admission path.
    // These jns gates are bypassed by normal inline admission; gate68 also
    // stores the singleton job waiting for Notify. They are not independent
    // inline refusal reasons. counter78 advances after the packet check and
    // is only a coarse indication of how far a call got.
    // 0.2.17's three are inferred from the same jns pair and the same +0x1C and
    // +0x10 spacing as 0.3.0's, not read off a 0.2.17 window - treat as unverified.
    std::uint32_t gate4c;
    std::uint32_t gate68;
    std::uint32_t counter78;
    // 0.3.1 and later: inline-wait spin. 0 = Dispatch spin (this project's original wait);
    // non-zero = predicated 1-pixel Draw (this project's new wait). SpinDraw=0 is
    // still 0.3.1-sliced, so it is not identical to the 0.3.0 wait.
    // 0 on the layout field means "runtime does not expose this flag".
    std::uint32_t spinDraw;
    // Read-only new-wait diagnostics (0.3.1 and later); zero for earlier runtimes.
    // Bound to the SHA above, never used to invoke a private factory.
    std::uint32_t graphicsPso = 0;
    std::uint32_t predicateReady = 0;
    std::uint32_t graphicsWaitBegin = 0;
    std::uint32_t graphicsWaitEnd = 0;
    // Return addresses after Dispatch calls in the pinned wait helper.
    std::uint32_t waitDispatchInit = 0, waitDispatchFallback = 0;
    std::uint32_t waitDispatchSlices = 0, waitDispatchFinish = 0;
    // The runtime's Scale ([DlssNrOnAmd] Scale in its own INI), the fifth control after tone,
    // structure and the two mask-derived values. It scales how much of the network reaches the
    // frame: 4/128 is the full effect, 0 leaves the frame untouched. It is not NVIDIA's style,
    // though it sits where that would be. 0 = not mapped for this runtime.
    std::uint32_t scale = 0;
    // Options 0.3.3 added to [DlssNrOnAmd]. DllMain reads them from dlssnr_on_amd.ini before the
    // host gets the module, so the host pins them: an ini left in the game folder must not change
    // the image. style is an int 0-2 fed to the network as style/128 (NVIDIA's style input, the slot
    // 0.3.1 did not have), toneCurve an int (0 Reinhard, 1 ACES), toneLift a float 0-0.25 and
    // useGameExposure a byte (0 = ignore the game's exposure). 0 = not mapped for this runtime.
    std::uint32_t style = 0;
    std::uint32_t toneCurve = 0;
    std::uint32_t toneLift = 0;
    std::uint32_t useGameExposure = 0;
    // 0.4.2 and later: [DlssNrOnAmd] Quality, a byte, 1 = fast (the default: cheaper arithmetic,
    // about 13% faster on RX 9000), 0 = reference (NVIDIA's own arithmetic). The runtime copies it
    // into the engine on every job, so it can change while the game runs. 0 = not mapped.
    std::uint32_t quality = 0;
    // 0.4.1 and later, ints in the engine object: the evaluation count the pre-block kernel takes as
    // its noise seed (Record zeroes it after the warm-up job, the launch bumps it after every job),
    // and the count on which the worker reads the pre-block back for its one-time self-check. The
    // final-image path pins the seed at 0 and moves the self-check out of reach once a job has run,
    // as the ReShade add-on's FixedSeed does. 0 = not mapped.
    std::uint32_t seedCounter = 0;
    std::uint32_t seedSelfCheck = 0;
    // 0.8.0 and later (NeuralHub). abi 1 is the version.dll the host isolates (RuntimeHostLoad::Load, Packet);
    // abi 2 is NeuralHub's host module (RuntimeHostLoad::LoadHost, PacketV2): it is loaded as neuralhub_host.dll,
    // whose DllMain does nothing, and the host runs its process init after making DetourAttachEx a no-op success
    // and RequestStage a return, so the runtime hooks nothing and installs no interceptor. onceFlag is the byte
    // NeuralHub_HostStart takes, which DLL_PROCESS_DETACH needs to stop and detach the init threads. formatEnum
    // maps a DXGI_FORMAT to the runtime's own format number for PacketV2's descriptors. prepareWeights builds
    // dlssnr_on_amd_weights.bin from nvngx_dlssnr.dll beside the module (weightsOnce is its once-flag), and
    // initCallback is set to base + initCallbackFn before init, as the runtime's own first present does.
    std::uint32_t abi = 1;
    std::uint32_t processInit = 0;
    std::uint32_t onceFlag = 0;
    std::uint32_t detourAttachEx = 0;
    std::uint32_t requestStage = 0;
    std::uint32_t formatEnum = 0;
    std::uint32_t prepareWeights = 0;
    std::uint32_t weightsOnce = 0;
    std::uint32_t initCallback = 0;
    std::uint32_t initCallbackFn = 0;
    // 0.8.0 runs its passes itself, inside one job of one module ([DLSS-NR] Passes): passCount is the int it copies
    // into each job (1 to 3), and passStyle (ints), passStructure, passSkin and passTone (floats) are arrays of three,
    // one per pass, of which the job reads entries 1 and 2 (pass 1 takes style, structure, skin and tone above).
    // 0 = the runtime has no multipass of its own (the host loads one module per pass).
    std::uint32_t passCount = 0;
    std::uint32_t passStyle = 0;
    std::uint32_t passStructure = 0;
    std::uint32_t passSkin = 0;
    std::uint32_t passTone = 0;
    // 0.8.0 NGX host path (all RVAs from GATE-REPORT-0.8.0.md). ngxPublishSlot is the host-services
    // GetState slot (0x1a01268, written by services init 0x13820 @0x13998); it holds base + ngxPublishFn
    // (0xa4a0, the "publish Present state" leaf: it writes the present queue, the wait state and
    // ngxPresentFlag when the published device is set). ngxPresentFlag is the descriptor's dword +0x208
    // (RVA 0x1a15630): the SR/RR evaluate gates (0x386c0 / 0x5bc80) bail "HIP not initialised yet" /
    // "the HIP device is not chosen yet" while it is 0. The remaining six are the gate bytes the
    // CreateFeature-once functions (SR 0x3ca80, RR 0x5fcc0) write behind that flag: init (0x1a02e48 /
    // 0x1a03a50, "present published"), gpu (0x1a02e49 / 0x1a03a51, bit0 of the RDNA3/4 check 0x96c10,
    // which CreateFeature runs itself) and weights (0x1a02e21 / 0x1a03a29, "SR/RR: weights from ...").
    // A host-mode client must call the publisher once and may then write these six bytes itself.
    // 0 = not mapped for this runtime (before 0.8.0).
    std::uint32_t ngxPublishSlot = 0;
    std::uint32_t ngxPublishFn = 0;
    std::uint32_t ngxPresentFlag = 0;
    std::uint32_t ngxGateSrInit = 0;
    std::uint32_t ngxGateSrGpu = 0;
    std::uint32_t ngxGateSrWeights = 0;
    std::uint32_t ngxGateRrInit = 0;
    std::uint32_t ngxGateRrGpu = 0;
    std::uint32_t ngxGateRrWeights = 0;
    // 0.8.0 NGX stage isolation & D3D12 SR/RR submission notification:
    // nrStageCallback (0x2e8a0) is the internal DLSS-NR stage callback registered on the stage dispatcher (0x849e0);
    // in host mode it must return 0 so NVSDK_NGX_D3D12_EvaluateFeature does not fire an internal pre-upscale NR pass
    // alongside the host's own AmdPreSr::Backend::Record pass. ngxNotifyChain (0x1a01240) is the function-pointer head
    // called by Notify (0x12380 @0x1239c) that dispatches SR (0x313d0) and RR (0x33570) D3D12 submission notifications.
    std::uint32_t nrStageCallback = 0;
    std::uint32_t ngxNotifyChain = 0;
    std::uint32_t ngxSrPendingList = 0;
    std::uint32_t ngxSrJobNotified = 0;
    std::uint32_t ngxSrJobDone = 0;
    std::uint32_t ngxRrPendingList = 0;
    std::uint32_t ngxRrJobNotified = 0;
    std::uint32_t ngxRrJobDone = 0;
    // 0.8.0 compute runtime. computeMode (0x1a15728) is the int [Compute] Backend resolves to at process init, 2 for
    // D3D12. There every addon's HIP kernels run as D3D12 pipelines whose code objects are bound when the host opens
    // its D3D12 runtime: computeOpen (0xd9b20, bool(ID3D12Device*, std::string* reason)), called by EnsureCompute
    // (0x96c10) at the first present, which then sets computeState (0x1a157a8) to 1 (-1 failed, 0 not yet). Host mode
    // has no present of the host's own, so the client opens it. The HIP engine's init (.init) must not run there: its
    // hipMemcpyToSymbol(g_e4m3_lut) finds no code object and the host stops the process ("compute FATAL").
    std::uint32_t computeMode = 0;
    std::uint32_t computeOpen = 0;
    std::uint32_t computeState = 0;
    // 0.8.0 present stage. The host's Present detour (0x12950) runs presentBefore (0x12650, void(IDXGISwapChain*):
    // the present stage's "before" dispatch, then the present and host frame counters every addon and the D3D12
    // runtime's per-frame rings read), the present, then stageDispatch (0x849e0, void(int stage, int phase, void*
    // context)) with stage 3 phase 2, the "after" dispatch carrying the HRESULT. Host mode has no detour of its own:
    // the client calls both around each present, or the counters stay at 0 / 1 ("host frame 1 at present 0").
    // fgMultiFrameCountMax (0x1a04654) is the generated-frame ceiling the FG addon publishes as
    // DLSSG.MultiFrameCountMax ([DLSS-FG] MaxMultiplier; Auto is 3, 4x).
    std::uint32_t presentBefore = 0;
    // The host's present counter (0x1a04e48): moves when presentBefore takes the present.
    std::uint32_t presentCounter = 0;
    std::uint32_t stageDispatch = 0;
    std::uint32_t fgMultiFrameCountMax = 0;
    // swapchainCreated (0x11f10, void(IUnknown* queue, IDXGISwapChain*)) is what the host's CreateSwapChain detour runs:
    // it maps the swapchain to its queue, which presentBefore's device check (0x12140) looks up first, and tells the
    // addons (stage 0 phase 4). Host mode has no such detour, and the game's swapchain under Streamline answers no
    // GetDevice(ID3D12CommandQueue), so every present was turned away ("not on our device") until the client called it.
    std::uint32_t swapchainCreated = 0;
};

// 0.2.17 pass DLL, SHA256 bc97f3b0...
inline constexpr AmdLayout kAmd0217 {
    "0.2.17", 7248384, Sha256FromHex("bc97f3b06718e19042acaf227bfe15d1e43d4977f9dc2e39994fcc511445ff4e"),
    0x80e48,  0x19240, 0xf600,
    0x9170,   0x12690, 0x8daf8,
    0x8cee8,  0x8cef0, 0x8cef8,
    0x8d010,  0x8d018, 0x8d218,
    0x8d21a,  0x8d6c0, 0x8d6f4,
    0x8d6f8,  0x8d724, 0x8d82c,
    0x8d908,  0x8d914, 0x8d9b0,
    0x8d9b4,  0x8d9bc, 0x8d9bd,
    0x8d9be,  0x8d9bf, 0x8d9c0,
    0x8d9d0,  0x8d9d4, 0x8d9d8,
    0x8d9e0,  0x8d9e4, 0x8dad0,
    0x8daa8,  0x8da30, 0x8d8f4,
    0x8d910,  0x8d920, 0
};

// Alpha 0.3.0 version.dll. Fields from unique 0.2.17 instruction windows;
// Record 0x12640 from pendingList/jobId xchg owner; Notify+0x13 still calls trampoline.
inline constexpr AmdLayout kAmd03 {
    "0.3.0", 7290880, Sha256FromHex("8321cae728d28cb7632d0d58d3d913e91132bf7645c126505698fbe4cd5a0138"),
    0,       0x1fe80, 0x12640,
    0x9460,  0x161e0, 0x97c70,
    0x96f68, 0x96f70, 0x96f78,
    0x97090, 0x97098, 0x97298,
    0x9729a, 0x977a0, 0x977d4,
    0x977d8, 0x97804, 0x97984,
    0x97a60, 0x97a6c, 0x97b10,
    0x97b14, 0x97b1c, 0x97b1d,
    0x97b1e, 0x97b1f, 0x97b20,
    0x97b30, 0x97b34, 0x97b38,
    0x97b40, 0x97b44, 0x97c30,
    0x97c08, 0x97b90, 0x97a4c,
    0x97a68, 0x97a78, 0
};

// 0.3.1 version.dll (SHA b108d640). Mapped from 0.3.0 via unique instruction
// windows (analysis/map_a031_rva.py); Record/Notify/shutdown heads and the
// recreate sticky-bit xrefs match 0.3.0 role-for-role. Data section moved
// ~+0x3180 and .text grew — every RVA below is 0.3.1-specific.
inline constexpr AmdLayout kAmd031 {
    "0.3.1", 7304192, Sha256FromHex("b108d6407eb7f094a4f9111edd778eee7b978b648d413a9fc7aeedfdd914c154"),
    0,       0x21720, 0x13540,
    0x9720,  0x17150, 0x9ae68,
    0x9a0e8, 0x9a0f0, 0x9a100,
    0x9a218, 0x9a220, 0x9a420,
    0x9a422, 0x9a928, 0x9a95c,
    0x9a960, 0x9a98c, 0x9ab58,
    0x9ac38, 0x9ac44, 0x9ace8,
    0x9acec, 0x9acf4, 0x9acf5,
    0x9acf6, 0x9acf7, 0x9acf8,
    0x9ad08, 0x9ad0c, 0x9ad10,
    0x9ad18, 0x9ad1c, 0x9ae08,
    0x9ade0, 0x9ad68, 0x9ac24,
    0x9ac40, 0x9ac50, 0x9ab14,
    0x9ab20, 0x9aa88, 0x17980,
    0x180a6, 0x17b70, 0x17f10,
    0x17f6a, 0x18057, 0x9ad14
};

// 0.4.0 version.dll (SHA d62be3d8), carved from dlssnr_on_amd_setup.exe v0.4.0 at file offset
// 0x2609c7, size 0x990000 (the setup's own lea r8 at 0x4de3f and mov r9d at 0x4de46). Mapped from
// 0.3.1; every field has at least two independent derivations that agree (scripts and outputs in
// daniel-runtime/analysis-opti): unique instruction windows plus a whole-program data map for all
// fields (map_layout.py, datamap.py, verify_kAmd040.txt), difflib-aligned function pairs with a
// byte-window recheck (map-engine-state/, verify-code/), and the [DlssNrOnAmd] reader's key ->
// store pairs for the options (map-options/, implement/ini_reader_v040.txt). Code entries are .pdata
// starts whose normalised bodies match 0.3.1 (Record 2824 instructions, Notify 237, init 545).
// Record 0x14cd0 still gates on enabled 0xa8604, nativeFailure 0xa7d12 and initDone 0xa7d10;
// Notify+0x22 still calls [trampoline]; the wait helper 0x19130-0x19856 keeps its four Dispatch
// return sites at the 0.3.1 offsets. The data block moved +0xd7d8..+0xd920 in pieces, and 0.4.0
// inserted PollSpacing (0xa8420) after SpinDraw, so graphicsPso is SpinDraw+0x14, not +0xc. The
// engine object grew (historyView/historyValid/initDone at engine+0x148/+0x150/+0x438); the host
// uses absolute RVAs, so that does not matter here. tests/amd_layout_binary_check.py checks every
// field against the binary: code entries and gates by what the code does, options by their INI
// store, and every other data field by pinned instruction sites (its ANCHORS table).
inline constexpr AmdLayout kAmd040 {
    .name = "0.4.0",
    .size = 10027008,
    .sha256 = Sha256FromHex("d62be3d8b9fbb3c6c81982c4ddb3dfa00eb9662e3206925cbe5b7e1bc6798b80"),
    .d3dCompileIat = 0,
    .init = 0x26110,
    .record = 0x14cd0,
    .notify = 0x9e10,
    .shutdown = 0x188e0,
    .trampoline = 0xa87a0,
    .device = 0xa78c0,
    .queue = 0xa78c8,
    .engine = 0xa78d8,
    .historyView = 0xa7a20,
    .historyValid = 0xa7a28,
    .initDone = 0xa7d10,
    .nativeFailure = 0xa7d12,
    .configuredInline = 0xa8218,
    .jobDone = 0xa824c,
    .timeoutCount = 0xa8250,
    .watchdog = 0xa827c,
    .interop = 0xa8468,
    .pendingList = 0xa8548,
    .jobId = 0xa8554,
    .depthInverted = 0xa85f8,
    .explicitDepth = 0xa85fc,
    .enabled = 0xa8604,
    .temporal = 0xa8605,
    .fsrInputs = 0xa8606,
    .depthPresent = 0xa8607,
    .tonemap = 0xa8608,
    .tone = 0xa8618,
    .structure = 0xa861c,
    .skin = 0xa8620,
    .charMask = 0xa8628,
    .toneChannels = 0xa862c,
    .hipOrdinal = 0xa8728,
    .recreate = 0xa8700,
    .recordLock = 0xa8688,
    .gate4c = 0xa8534,
    .gate68 = 0xa8550,
    .counter78 = 0xa8560,
    .spinDraw = 0xa841c,
    .graphicsPso = 0xa8430,
    .predicateReady = 0xa8390,
    .graphicsWaitBegin = 0x19130,
    .graphicsWaitEnd = 0x19856,
    .waitDispatchInit = 0x19320,
    .waitDispatchFallback = 0x196c0,
    .waitDispatchSlices = 0x1971a,
    .waitDispatchFinish = 0x19807,
    .scale = 0xa8624,
    .style = 0xa8630,
    .toneCurve = 0xa8634,
    .toneLift = 0xa8638,
    .useGameExposure = 0xa863c,
};

// 0.4.1 version.dll (SHA 823063eb), carved from dlssnr_on_amd_setup.exe v0.4.1 at file offset
// 0x2609c7, size 0x975200. Mapped from 0.4.0 two ways that agree on every field (map_layout.py and
// datamap.py in daniel-runtime/analysis-opti, map_layout_040_041.txt and datamap_040_041.txt).
// Init, Notify, shutdown, the wait helper and the bootstrap are 0.4.0's instruction for
// instruction; Record adds the QueuePriority stream (a high-priority HIP stream, default 1, read by
// DllMain) and some engine members moved +0x18. The data block moved +0x2020 up to historyValid and
// +0x2038 from initDone on.
inline constexpr AmdLayout kAmd041 {
    .name = "0.4.1",
    .size = 9916928,
    .sha256 = Sha256FromHex("823063eb4c76b1334fd1800c41798873ae61d4016af0406f1f0b9dce57b1d376"),
    .d3dCompileIat = 0,
    .init = 0x26130,
    .record = 0x14c40,
    .notify = 0x9d80,
    .shutdown = 0x188d0,
    .trampoline = 0xaa7d8,
    .device = 0xa98e0,
    .queue = 0xa98e8,
    .engine = 0xa98f8,
    .historyView = 0xa9a40,
    .historyValid = 0xa9a48,
    .initDone = 0xa9d48,
    .nativeFailure = 0xa9d4a,
    .configuredInline = 0xaa250,
    .jobDone = 0xaa284,
    .timeoutCount = 0xaa288,
    .watchdog = 0xaa2b4,
    .interop = 0xaa4a0,
    .pendingList = 0xaa580,
    .jobId = 0xaa58c,
    .depthInverted = 0xaa630,
    .explicitDepth = 0xaa634,
    .enabled = 0xaa63c,
    .temporal = 0xaa63d,
    .fsrInputs = 0xaa63e,
    .depthPresent = 0xaa63f,
    .tonemap = 0xaa640,
    .tone = 0xaa650,
    .structure = 0xaa654,
    .skin = 0xaa658,
    .charMask = 0xaa660,
    .toneChannels = 0xaa664,
    .hipOrdinal = 0xaa760,
    .recreate = 0xaa738,
    .recordLock = 0xaa6c0,
    .gate4c = 0xaa56c,
    .gate68 = 0xaa588,
    .counter78 = 0xaa598,
    .spinDraw = 0xaa454,
    .graphicsPso = 0xaa468,
    .predicateReady = 0xaa3c8,
    .graphicsWaitBegin = 0x19120,
    .graphicsWaitEnd = 0x19846,
    .waitDispatchInit = 0x19310,
    .waitDispatchFallback = 0x196b0,
    .waitDispatchSlices = 0x1970a,
    .waitDispatchFinish = 0x197f7,
    .scale = 0xaa65c,
    .style = 0xaa668,
    .toneCurve = 0xaa66c,
    .toneLift = 0xaa670,
    .useGameExposure = 0xaa674,
    .seedCounter = 0xa9934,
    .seedSelfCheck = 0xa99fc,
};

// 0.4.2 and 0.4.3 version.dll (SHA 8aa2dcc5 and d1e32086). 0.4.2 is public since 2026-09-27 and 0.4.3 since
// 2026-09-28 (their setups hold these same files). Carved from their setups the same way,
// size 0xc61600 and 0xc28c00. Mapped from 0.4.1 two ways that agree on every field
// (map_layout_041_042/043.txt and datamap_041_042/043.txt). Init, shutdown, the wait helper and
// the bootstrap are 0.4.1's instruction for instruction; Record and Notify only moved engine
// members, which the host never touches. New INI keys: NoiseHandoff and Quality (0.4.2),
// OverlayKey (0.4.3). Same weights as 0.4.1.
inline constexpr AmdLayout kAmd042 {
    .name = "0.4.2",
    .size = 12981760,
    .sha256 = Sha256FromHex("8aa2dcc5b6596aca97995dbfd4e0a9790d8c15108495e0ed154dd15dbb5b465a"),
    .d3dCompileIat = 0,
    .init = 0x28170,
    .record = 0x15040,
    .notify = 0x9db0,
    .shutdown = 0x18cf0,
    .trampoline = 0xaf9b0,
    .device = 0xaeaa0,
    .queue = 0xaeaa8,
    .engine = 0xaeab8,
    .historyView = 0xaec00,
    .historyValid = 0xaec08,
    .initDone = 0xaef18,
    .nativeFailure = 0xaef1a,
    .configuredInline = 0xaf420,
    .jobDone = 0xaf454,
    .timeoutCount = 0xaf458,
    .watchdog = 0xaf484,
    .interop = 0xaf678,
    .pendingList = 0xaf758,
    .jobId = 0xaf764,
    .depthInverted = 0xaf808,
    .explicitDepth = 0xaf80c,
    .enabled = 0xaf814,
    .temporal = 0xaf815,
    .fsrInputs = 0xaf816,
    .depthPresent = 0xaf817,
    .tonemap = 0xaf818,
    .tone = 0xaf828,
    .structure = 0xaf82c,
    .skin = 0xaf830,
    .charMask = 0xaf838,
    .toneChannels = 0xaf83c,
    .hipOrdinal = 0xaf938,
    .recreate = 0xaf910,
    .recordLock = 0xaf898,
    .gate4c = 0xaf744,
    .gate68 = 0xaf760,
    .counter78 = 0xaf770,
    .spinDraw = 0xaf624,
    .graphicsPso = 0xaf640,
    .predicateReady = 0xaf598,
    .graphicsWaitBegin = 0x19540,
    .graphicsWaitEnd = 0x19c66,
    .waitDispatchInit = 0x19730,
    .waitDispatchFallback = 0x19ad0,
    .waitDispatchSlices = 0x19b2a,
    .waitDispatchFinish = 0x19c17,
    .scale = 0xaf834,
    .style = 0xaf840,
    .toneCurve = 0xaf844,
    .toneLift = 0xaf848,
    .useGameExposure = 0xaf84c,
    .quality = 0xaf84d,
    .seedCounter = 0xaeaf4,
    .seedSelfCheck = 0xaebbc,
};

inline constexpr AmdLayout kAmd043 {
    .name = "0.4.3",
    .size = 12749824,
    .sha256 = Sha256FromHex("d1e320862a8763ac39e7ce194536d4b6c55ba61bae9e8a92753cec32df67a457"),
    .d3dCompileIat = 0,
    .init = 0x28a20,
    .record = 0x157f0,
    .notify = 0xa200,
    .shutdown = 0x194a0,
    .trampoline = 0xb1b50,
    .device = 0xb0c10,
    .queue = 0xb0c18,
    .engine = 0xb0c28,
    .historyView = 0xb0d70,
    .historyValid = 0xb0d78,
    .initDone = 0xb1090,
    .nativeFailure = 0xb1092,
    .configuredInline = 0xb1598,
    .jobDone = 0xb15cc,
    .timeoutCount = 0xb15d0,
    .watchdog = 0xb15fc,
    .interop = 0xb17f0,
    .pendingList = 0xb18d0,
    .jobId = 0xb18dc,
    .depthInverted = 0xb1980,
    .explicitDepth = 0xb1984,
    .enabled = 0xb198c,
    .temporal = 0xb198d,
    .fsrInputs = 0xb198e,
    .depthPresent = 0xb198f,
    .tonemap = 0xb1990,
    .tone = 0xb19a0,
    .structure = 0xb19a4,
    .skin = 0xb19a8,
    .charMask = 0xb19b0,
    .toneChannels = 0xb19b4,
    .hipOrdinal = 0xb1ab0,
    .recreate = 0xb1a88,
    .recordLock = 0xb1a10,
    .gate4c = 0xb18bc,
    .gate68 = 0xb18d8,
    .counter78 = 0xb18e8,
    .spinDraw = 0xb179c,
    .graphicsPso = 0xb17b8,
    .predicateReady = 0xb1710,
    .graphicsWaitBegin = 0x19cf0,
    .graphicsWaitEnd = 0x1a416,
    .waitDispatchInit = 0x19ee0,
    .waitDispatchFallback = 0x1a280,
    .waitDispatchSlices = 0x1a2da,
    .waitDispatchFinish = 0x1a3c7,
    .scale = 0xb19ac,
    .style = 0xb19b8,
    .toneCurve = 0xb19bc,
    .toneLift = 0xb19c0,
    .useGameExposure = 0xb19c4,
    .quality = 0xb19c5,
    .seedCounter = 0xb0c64,
    .seedSelfCheck = 0xb0d2c,
};

// 0.5.0 version.dll (SHA cddfb09e), first an early build for danielblnc's supporters and public since
// 2026-09-29 (its setup holds this same file). Carved from its setup the same way, size 0x24e9600: it adds RDNA3
// (gfx11) register kernels beside the RDNA4 ones. Mapped from 0.4.3 two ways that agree on every
// field (map_layout_043_050.txt and datamap_043_050.txt). Shutdown, the wait helper and the bootstrap
// are 0.4.3's instruction for instruction; Init, Record and Notify only moved engine members, which the
// host never touches, and Record logs which kernels run. New INI key: Rdna3RegKernels, which the
// runtime reads and ignores.
inline constexpr AmdLayout kAmd050 {
    .name = "0.5.0",
    .size = 38703616,
    .sha256 = Sha256FromHex("cddfb09e019347957bf7b96c95c0e900e8d3062dfaed697a8a96b0a039aec31a"),
    .d3dCompileIat = 0,
    .init = 0x29870,
    .record = 0x15640,
    .notify = 0xa000,
    .shutdown = 0x19340,
    .trampoline = 0xb6b88,
    .device = 0xb5c18,
    .queue = 0xb5c20,
    .engine = 0xb5c30,
    .historyView = 0xb5d88,
    .historyValid = 0xb5d90,
    .initDone = 0xb60c8,
    .nativeFailure = 0xb60ca,
    .configuredInline = 0xb65d0,
    .jobDone = 0xb6604,
    .timeoutCount = 0xb6608,
    .watchdog = 0xb6634,
    .interop = 0xb6828,
    .pendingList = 0xb6908,
    .jobId = 0xb6914,
    .depthInverted = 0xb69b8,
    .explicitDepth = 0xb69bc,
    .enabled = 0xb69c4,
    .temporal = 0xb69c5,
    .fsrInputs = 0xb69c6,
    .depthPresent = 0xb69c7,
    .tonemap = 0xb69c8,
    .tone = 0xb69d8,
    .structure = 0xb69dc,
    .skin = 0xb69e0,
    .charMask = 0xb69e8,
    .toneChannels = 0xb69ec,
    .hipOrdinal = 0xb6ae8,
    .recreate = 0xb6ac0,
    .recordLock = 0xb6a48,
    .gate4c = 0xb68f4,
    .gate68 = 0xb6910,
    .counter78 = 0xb6920,
    .spinDraw = 0xb67d4,
    .graphicsPso = 0xb67f0,
    .predicateReady = 0xb6748,
    .graphicsWaitBegin = 0x19b90,
    .graphicsWaitEnd = 0x1a2b6,
    .waitDispatchInit = 0x19d80,
    .waitDispatchFallback = 0x1a120,
    .waitDispatchSlices = 0x1a17a,
    .waitDispatchFinish = 0x1a267,
    .scale = 0xb69e4,
    .style = 0xb69f0,
    .toneCurve = 0xb69f4,
    .toneLift = 0xb69f8,
    .useGameExposure = 0xb69fc,
    .quality = 0xb69fd,
    .seedCounter = 0xb5c7c,
    .seedSelfCheck = 0xb5d44,
};

// 0.5.1 version.dll (SHA 493b4a3b), the next early build danielblnc gives his supporters, not
// distributed with this project. Carved from its setup the same way, size 0x24c8a00. Mapped from 0.5.0
// two ways that agree on every field (map_layout_050_051.txt and datamap_050_051.txt). Record, Notify,
// shutdown, the wait helper and the bootstrap stay at 0.5.0's addresses; Init moved, and Record and
// Notify only moved engine members, which the host never touches. No new INI key.
inline constexpr AmdLayout kAmd051 {
    .name = "0.5.1",
    .size = 38569472,
    .sha256 = Sha256FromHex("493b4a3b80a21f7255109172ab7bb01ba08d35f2941718f441768f1abfc48acd"),
    .d3dCompileIat = 0,
    .init = 0x2a0c0,
    .record = 0x15640,
    .notify = 0xa000,
    .shutdown = 0x19340,
    .trampoline = 0xb9c20,
    .device = 0xb8c28,
    .queue = 0xb8c30,
    .engine = 0xb8c40,
    .historyView = 0xb8d98,
    .historyValid = 0xb8da0,
    .initDone = 0xb9160,
    .nativeFailure = 0xb9162,
    .configuredInline = 0xb9668,
    .jobDone = 0xb969c,
    .timeoutCount = 0xb96a0,
    .watchdog = 0xb96cc,
    .interop = 0xb98c0,
    .pendingList = 0xb99a0,
    .jobId = 0xb99ac,
    .depthInverted = 0xb9a50,
    .explicitDepth = 0xb9a54,
    .enabled = 0xb9a5c,
    .temporal = 0xb9a5d,
    .fsrInputs = 0xb9a5e,
    .depthPresent = 0xb9a5f,
    .tonemap = 0xb9a60,
    .tone = 0xb9a70,
    .structure = 0xb9a74,
    .skin = 0xb9a78,
    .charMask = 0xb9a80,
    .toneChannels = 0xb9a84,
    .hipOrdinal = 0xb9b80,
    .recreate = 0xb9b58,
    .recordLock = 0xb9ae0,
    .gate4c = 0xb998c,
    .gate68 = 0xb99a8,
    .counter78 = 0xb99b8,
    .spinDraw = 0xb986c,
    .graphicsPso = 0xb9888,
    .predicateReady = 0xb97e0,
    .graphicsWaitBegin = 0x19b90,
    .graphicsWaitEnd = 0x1a2b6,
    .waitDispatchInit = 0x19d80,
    .waitDispatchFallback = 0x1a120,
    .waitDispatchSlices = 0x1a17a,
    .waitDispatchFinish = 0x1a267,
    .scale = 0xb9a7c,
    .style = 0xb9a88,
    .toneCurve = 0xb9a8c,
    .toneLift = 0xb9a90,
    .useGameExposure = 0xb9a94,
    .quality = 0xb9a95,
    .seedCounter = 0xb8c8c,
    .seedSelfCheck = 0xb8d54,
};

// 0.6.0 version.dll (SHA 195c4a89), an early build danielblnc gives his supporters, not distributed
// with this project. Carved from its setup the same way (.rdata offset 0x262017), size 0x360d600: it
// adds RDNA2 (gfx103x) kernels. Mapped from 0.5.1 two ways that agree on every field
// (map_layout_051_060.txt, datamap_051_060.txt; the data moved +0x5060 to +0x50e8, and the option
// bytes from enabled on are no longer one block). Notify is 0xb4a0 by the function match (the
// instruction windows put it at a look-alike). The wait helper moved +0x2270 and matches byte for byte.
// New INI keys (FreshExposure, JitterComp, PreHoldCounter, ResetOnEnable, SrgbApplyFix,
// UsePreExposure) keep the runtime's defaults; the host writes none of them.
inline constexpr AmdLayout kAmd060 {
    .name = "0.6.0",
    .size = 56677888,
    .sha256 = Sha256FromHex("195c4a891b6eac4c1cb7671e10ff62bbbe2b17f1dfae1344dc5a6714e4775721"),
    .d3dCompileIat = 0,
    .init = 0x2cfb0,
    .record = 0x17680,
    .notify = 0xb4a0,
    .shutdown = 0x1b420,
    .trampoline = 0xbed08,
    .device = 0xbdc88,
    .queue = 0xbdc90,
    .engine = 0xbdca0,
    .historyView = 0xbde10,
    .historyValid = 0xbde18,
    .initDone = 0xbe1e8,
    .nativeFailure = 0xbe1ea,
    .configuredInline = 0xbe6f0,
    .jobDone = 0xbe724,
    .timeoutCount = 0xbe728,
    .watchdog = 0xbe754,
    .interop = 0xbe948,
    .pendingList = 0xbea28,
    .jobId = 0xbea34,
    .depthInverted = 0xbead8,
    .explicitDepth = 0xbeadc,
    .enabled = 0xbeaed,
    .temporal = 0xbeaee,
    .fsrInputs = 0xbeaef,
    .depthPresent = 0xbeaf0,
    .tonemap = 0xbeaf4,
    .tone = 0xbeb04,
    .structure = 0xbeb08,
    .skin = 0xbeb0c,
    .charMask = 0xbeb14,
    .toneChannels = 0xbeb18,
    .hipOrdinal = 0xbec30,
    .recreate = 0xbec08,
    .recordLock = 0xbeb70,
    .gate4c = 0xbea14,
    .gate68 = 0xbea30,
    .counter78 = 0xbea40,
    .spinDraw = 0xbe8f4,
    .graphicsPso = 0xbe910,
    .predicateReady = 0xbe868,
    .graphicsWaitBegin = 0x1be00,
    .graphicsWaitEnd = 0x1c526,
    .waitDispatchInit = 0x1bff0,
    .waitDispatchFallback = 0x1c390,
    .waitDispatchSlices = 0x1c3ea,
    .waitDispatchFinish = 0x1c4d7,
    .scale = 0xbeb10,
    .style = 0xbeb1c,
    .toneCurve = 0xbeb20,
    .toneLift = 0xbeb24,
    .useGameExposure = 0xbeb28,
    .quality = 0xbeb29,
    .seedCounter = 0xbdcec,
    .seedSelfCheck = 0xbddc8,
};

// 0.8.0 runtime (SHA be9172b6), public since 2026-10-16. It is no longer a setup's version.dll but the host module
// NeuralHub_v0.8.0_setup.exe carries inside neuralhub.exe (file offset 0x320562, size 0x610de00) and installs as
// %LOCALAPPDATA%\Danielblnc\NeuralHub\runtime\v0.8.0\neuralhub_host.dll. The host side was rewritten (an add-on host
// with NGX, FSR and XeSS interceptors and a D3D12 compute backend), but the HIP path keeps 0.6.0's contract: the same
// Init, a Record whose packet grew to 0xd0 bytes (PacketV2), the same Notify on the game's queue and the same in-place
// correction when nativePre is 0. Mapped from 0.6.0 by instruction alignment, string anchors and the shared function
// tables (danielblnc-0.8.0 notes beside this repository); run on an RX 9070 XT through the HIP backend, 48 of 48 frames
// recorded and completed. The options are no longer read by an INI reader: a settings registry refreshes them from
// neuralhub.ini on its own present events only, which a host-mode load never delivers, so the host's writes stand.
// UseFsrInputs, Async and SpinDraw left the settings (fsrInputs 0); D3DCompile is not imported.
inline constexpr AmdLayout kAmd080 {
    .name = "0.8.0",
    .size = 101768704,
    .sha256 = Sha256FromHex("be9172b68d0e77f98f04f941f4e2abe2c73eb01b8aafe94b26ee31eefefb5eb8"),
    .d3dCompileIat = 0,
    .init = 0x97350,
    .record = 0x24140,
    .notify = 0x12380,
    .shutdown = 0x22a20,
    .trampoline = 0x1a01208,
    .device = 0x1a14d98,
    .queue = 0x1a14da0,
    .engine = 0x1a01d58,
    .historyView = 0x1a01ee8,
    .historyValid = 0x1a01ef0,
    .initDone = 0x1a02560,
    .nativeFailure = 0x1a02562,
    .configuredInline = 0x1a025d0,
    .jobDone = 0x1a14cdc,
    .timeoutCount = 0x1a14ce0,
    .watchdog = 0x1a14cf4,
    .interop = 0x1a026c0,
    .pendingList = 0x1a15360,
    .jobId = 0x1a02704,
    .depthInverted = 0x1a0271c,
    .explicitDepth = 0x1a02720,
    .enabled = 0x1a02731,
    .temporal = 0x1a02732,
    .fsrInputs = 0,
    .depthPresent = 0x1a02733,
    .tonemap = 0x1a02734,
    .tone = 0x1a02744,
    .structure = 0x1a02748,
    .skin = 0x1a0274c,
    .charMask = 0x1a02754,
    .toneChannels = 0x1a02758,
    .hipOrdinal = 0x1a15718,
    .recreate = 0x1a027ca,
    .recordLock = 0x1a02778,
    .gate4c = 0x1a026f0,
    .gate68 = 0x1a15368,
    .counter78 = 0x1a02710,
    .spinDraw = 0x1a15400,
    .graphicsPso = 0x1a152b8,
    .predicateReady = 0x1a15290,
    .graphicsWaitBegin = 0x8f50,
    .graphicsWaitEnd = 0x96a0,
    .waitDispatchInit = 0x916d,
    .waitDispatchFallback = 0x9500,
    .waitDispatchSlices = 0x955a,
    .waitDispatchFinish = 0x964f,
    .scale = 0x1a02750,
    .style = 0x1a0275c,
    .toneCurve = 0x1a02760,
    .toneLift = 0x1a02764,
    .useGameExposure = 0x1a02768,
    .quality = 0x1a02769,
    .seedCounter = 0x1a01da4,
    .seedSelfCheck = 0x1a01ea0,
    .abi = 2,
    .processInit = 0x69410,
    .onceFlag = 0x1a04ccc,
    .detourAttachEx = 0x1693a0,
    .requestStage = 0x98100,
    .formatEnum = 0x5a90,
    .prepareWeights = 0x25630,
    .weightsOnce = 0x1a02561,
    .initCallback = 0x1a02410,
    .initCallbackFn = 0x71cf0,
    .passCount = 0x1a02850,
    .passStyle = 0x1a02854,
    .passStructure = 0x1a02860,
    .passSkin = 0x1a0286c,
    .passTone = 0x1a02878,
    .ngxPublishSlot = 0x1a01268,
    .ngxPublishFn = 0xa4a0,
    .ngxPresentFlag = 0x1a15630,
    .ngxGateSrInit = 0x1a02e48,
    .ngxGateSrGpu = 0x1a02e49,
    .ngxGateSrWeights = 0x1a02e21,
    .ngxGateRrInit = 0x1a03a50,
    .ngxGateRrGpu = 0x1a03a51,
    .ngxGateRrWeights = 0x1a03a29,
    .nrStageCallback = 0x2e8a0,
    .ngxNotifyChain = 0x1a01240,
    .ngxSrPendingList = 0x19f9320,
    .ngxSrJobNotified = 0x1a02ecc,
    .ngxSrJobDone = 0x1a02ed0,
    .ngxRrPendingList = 0x19fa900,
    .ngxRrJobNotified = 0x1a03af0,
    .ngxRrJobDone = 0x1a03af4,
    .computeMode = 0x1a15728,
    .computeOpen = 0xd9b20,
    .computeState = 0x1a157a8,
    .presentBefore = 0x12650,
    .presentCounter = 0x1a04e48,
    .stageDispatch = 0x849e0,
    .fgMultiFrameCountMax = 0x1a04654,
    .swapchainCreated = 0x11f10,
};

inline constexpr const AmdLayout* kAmdLayouts[] = { &kAmd0217, &kAmd03,  &kAmd031, &kAmd040, &kAmd041, &kAmd042,
                                                    &kAmd043,  &kAmd050, &kAmd051, &kAmd060, &kAmd080 };

// Compile-time sanity: the hex helper must land on the first/last digest byte
// of each known runtime. A wrong-length literal already fails Sha256FromHex;
// these catch a copy-paste that swapped two mid-string bytes.
static_assert(kAmd0217.sha256.bytes[0] == 0xbc && kAmd0217.sha256.bytes[31] == 0x4e);
static_assert(kAmd03.sha256.bytes[0] == 0x83 && kAmd03.sha256.bytes[31] == 0x38);
static_assert(kAmd031.sha256.bytes[0] == 0xb1 && kAmd031.sha256.bytes[31] == 0x54);
static_assert(kAmd040.sha256.bytes[0] == 0xd6 && kAmd040.sha256.bytes[31] == 0x80);
static_assert(kAmd041.sha256.bytes[0] == 0x82 && kAmd041.sha256.bytes[31] == 0x76);
static_assert(kAmd042.sha256.bytes[0] == 0x8a && kAmd042.sha256.bytes[31] == 0x5a);
static_assert(kAmd043.sha256.bytes[0] == 0xd1 && kAmd043.sha256.bytes[31] == 0x57);
static_assert(kAmd050.sha256.bytes[0] == 0xcd && kAmd050.sha256.bytes[31] == 0x1a);
static_assert(kAmd051.sha256.bytes[0] == 0x49 && kAmd051.sha256.bytes[31] == 0xcd);
static_assert(kAmd060.sha256.bytes[0] == 0x19 && kAmd060.sha256.bytes[31] == 0x21);
static_assert(kAmd080.sha256.bytes[0] == 0xbe && kAmd080.sha256.bytes[31] == 0xb8);
} // namespace AmdPreSr
