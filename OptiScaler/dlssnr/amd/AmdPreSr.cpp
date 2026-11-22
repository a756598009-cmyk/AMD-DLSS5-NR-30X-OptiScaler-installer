#include "AmdPreSr.h"
#include "AmdLayout.h"
#include "AmdBridge.h"
#ifdef AMD_RETIRE_DIAGNOSTICS
#include "RetirementDiagnostics.h"
#endif
#include "RuntimeNotification.h"
#include "RuntimeHostLoad.h"
#include "HipRuntimeLoad.h"
#include "SubmissionState.h"
#include "GraphicsTracker.h"
#include "GraphicsInvocation.h"
#include "NativeWaitHooks.h"
#include "StallWatch.h"
#include <hooks/D3D12_Hooks.h>
#ifndef AMD_GRAPHICS_SOURCE_ID
#define AMD_GRAPHICS_SOURCE_ID "unfingerprinted"
#endif
#include <Config.h>
#include <State.h>
#include <Logger.h>
#include <proxies/NVNGX_Proxy.h>
#include "ColorEncoding.h"
#include "AmdLookShader.h"
#include "RtgiNative.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <dxgi.h>
#include <chrono>
#include <algorithm>
#include <functional>
#include <fstream>
#include <mutex>
#include <vector>
#include <map>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <cmath>

using Microsoft::WRL::ComPtr;
namespace dlssnr
{
namespace crashsnap
{
void EnableCrashSnap();
}
}
namespace AmdPreSr
{
namespace
{
template <class T> T& At(HMODULE h, size_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(h) + rva); }
void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr))
        throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(static_cast<unsigned>(hr)));
}
void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if (!r || a == b)
        return;
    D3D12_RESOURCE_BARRIER v {};
    v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    c->ResourceBarrier(1, &v);
}
struct Packet
{
    ID3D12GraphicsCommandList* list;
    ID3D12Resource* colour;
    UINT colourState, pad14;
    ID3D12Resource* motion;
    UINT motionState, pad24;
    ID3D12Resource* depth;
    UINT depthState, pad34;
    ID3D12Resource* exposure;
    UINT exposureState;
    float scaleX, scaleY;
    // Native FFX pre mode has different input/output semantics. B supplies its
    // own FP16 staging and uses the existing non-pre packet path (zero).
    uint8_t nativePre;
    uint8_t pad4d[3];
    UINT renderWidth, renderHeight;
    // B does not request native pre-mode reprojection. Explicit zero avoids
    // passing stack data as jitter; this is not a new NGX-to-FFX jitter mapping.
    float jitterX, jitterY;
    // danielblnc 0.6.0 reads a pre-exposure here (UsePreExposure, on by default) and divides the colour by it. The
    // exposure handed over already has the game's pre-exposure taken out, so it is 1. Without this field the runtime
    // read the stack past the packet (95385, 17703, ... in a GTA V Enhanced log) and the frame came out green noise.
    // 0.3.1 to 0.5.1 never read past 0x5c.
    float preExposure = 1.0f;
    UINT pad64;
};
static_assert(sizeof(Packet) == 0x68 && offsetof(Packet, scaleX) == 0x44);
static_assert(offsetof(Packet, nativePre) == 0x4c && offsetof(Packet, renderWidth) == 0x50);
static_assert(offsetof(Packet, renderHeight) == 0x54 && offsetof(Packet, jitterX) == 0x58);
static_assert(offsetof(Packet, jitterY) == 0x5c && offsetof(Packet, preExposure) == 0x60);
// abi 2 (0.8.0). Each resource is a descriptor the runtime's interceptors fill from GetDesc: the resource, the API (1,
// D3D12), the runtime's own format number, the DXGI format, the D3D12_RESOURCE_STATES it is in (raw states now: the
// 4 the old packet took is RENDER_TARGET here, and the runtime's pre-upscale copy fails with E_INVALIDARG on it), and
// the size. An absent input is all zero. The list handle is {list, api 1, tag 3}, as every bundle builder writes it.
struct ResourceV2
{
    ID3D12Resource* resource;
    UINT api, format, dxgi, state, width, height;
    UINT64 reserved;
};
struct PacketV2
{
    ID3D12GraphicsCommandList* list;
    UINT listApi, listTag;
    ResourceV2 colour, motion, depth, exposure;
    float scaleX, scaleY;
    // 0: the correction is written into colour (the host's own FP16 staging), as with the old packet. 1 is the
    // runtime's pre-upscale mode, which keeps colour and returns its own copy; the host does not use it.
    uint8_t nativePre;
    uint8_t padb9[3];
    UINT renderWidth, renderHeight;
    float jitterX, jitterY;
    float preExposure = 1.0f;
};
static_assert(sizeof(ResourceV2) == 0x28 && sizeof(PacketV2) == 0xd0);
static_assert(offsetof(PacketV2, colour) == 0x10 && offsetof(PacketV2, motion) == 0x38);
static_assert(offsetof(PacketV2, depth) == 0x60 && offsetof(PacketV2, exposure) == 0x88);
static_assert(offsetof(PacketV2, scaleX) == 0xb0 && offsetof(PacketV2, nativePre) == 0xb8);
static_assert(offsetof(PacketV2, renderWidth) == 0xbc && offsetof(PacketV2, jitterX) == 0xc4);
static_assert(offsetof(PacketV2, preExposure) == 0xcc);
using InitFn = bool(__fastcall*)(void*, const std::string*);
using RecordFn = void(__fastcall*)(Packet*);
using RecordV2Fn = bool(__fastcall*)(PacketV2*);
using FormatEnumFn = UINT(__fastcall*)(DXGI_FORMAT);
using NotifyFn = void(__fastcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using HipSetFn = int (*)(int);
constexpr char CopyShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Extent : register(b0) { uint w; uint h; uint sourceW; uint sourceH; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=w || p.y>=h)return;
 if(w==sourceW && h==sourceH){dst[p.xy]=src.Load(int3(p.xy,0));return;}
 // Integrate the entire source pixel footprint. A single bilinear sample aliases
 // narrow emissive lines when the model runs far below the input resolution.
 float2 lo=float2(p.xy)*float2(sourceW,sourceH)/float2(w,h);
 float2 hi=float2(p.xy+1)*float2(sourceW,sourceH)/float2(w,h);
 int2 first=int2(floor(lo)); float4 sum=0;float total=0;
 [loop]for(int y=first.y;y<int(ceil(hi.y));++y)
 [loop]for(int x=first.x;x<int(ceil(hi.x));++x){
  float2 coverage=max(0,min(hi,float2(x+1,y+1))-max(lo,float2(x,y)));
  float weight=coverage.x*coverage.y;
  sum+=src.Load(int3(clamp(int2(x,y),0,int2(sourceW-1,sourceH-1)),0))*weight;total+=weight;
 }
 dst[p.xy]=sum/max(total,1e-6);
})";
constexpr char ResolveShader[] = R"(
Texture2D<float4> src:register(t0);
Texture2D<float4> baseline:register(t1);
Texture2D<float4> edited:register(t2);
RWTexture2D<float4> dst:register(u0);
cbuffer Extent:register(b0){uint w,h,lowW,lowH;};
float3 delta(int2 p){p=clamp(p,0,int2(lowW-1,lowH-1));return edited.Load(int3(p,0)).rgb-baseline.Load(int3(p,0)).rgb;}
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){
 if(p.x>=w||p.y>=h)return;
 float2 q=(float2(p.xy)+.5)*float2(lowW,lowH)/float2(w,h)-.5;
 int2 a=int2(floor(q));float2 t=frac(q);
 float3 d=lerp(lerp(delta(a),delta(a+int2(1,0)),t.x),lerp(delta(a+int2(0,1)),delta(a+1),t.x),t.y);
 float4 c=src.Load(int3(p.xy,0));
 // A reduced neural pixel mixes surfaces and small emitters. Suppress its edit
 // where the original pixel disagrees with that footprint, rather than spreading
 // the edit blindly across high-contrast edges. No previous frame is reused.
 int2 hi=int2(lowW-1,lowH-1);
 float3 b=lerp(lerp(baseline.Load(int3(clamp(a,0,hi),0)).rgb,baseline.Load(int3(clamp(a+int2(1,0),0,hi),0)).rgb,t.x),
 lerp(baseline.Load(int3(clamp(a+int2(0,1),0,hi),0)).rgb,baseline.Load(int3(clamp(a+1,0,hi),0)).rgb,t.x),t.y);
 float3 magnitude=max(max(abs(c.rgb),abs(b)),1e-5);
 float mismatch=max(abs(c.r-b.r)/magnitude.r,max(abs(c.g-b.g)/magnitude.g,abs(c.b-b.b)/magnitude.b));
 float confidence=1-smoothstep(.15,.75,mismatch);
 // Keep extreme low-resolution edits bounded relative to the current footprint.
 float3 limit=.5*max(abs(b),abs(c.rgb));
 d=clamp(d,-limit,limit)*confidence;
 dst[p.xy]=float4(clamp(c.rgb+d,0,65504),c.a);
})";
constexpr char DepthShader[] = R"(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer Extent : register(b0) { uint w; uint h; uint sourceW; uint sourceH; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x<w && p.y<h) {
 uint2 q=min(uint2((float2(p.xy)+.5)*float2(sourceW,sourceH)/float2(w,h)),uint2(sourceW-1,sourceH-1));
 dst[p.xy]=src.Load(int3(q,0)); }
})";
constexpr char MotionShader[] = R"(
Texture2D<float2> src : register(t0);
RWTexture2D<float2> dst : register(u0);
cbuffer Extent : register(b0) { uint w; uint h; uint sourceW; uint sourceH; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=w || p.y>=h) return;
 uint2 q=min(uint2((float2(p.xy)+0.5)*float2(sourceW,sourceH)/float2(w,h)),uint2(sourceW-1,sourceH-1));
 // Keep the sampled vector unchanged; convert its pixel scale in the packet.
 dst[p.xy]=src.Load(int3(q,0));
})";
constexpr char ExposureShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer Extent : register(b0) { uint w; uint h; float preExposure; float exposureScale; };
[numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID) {
 float e=src.Load(int3(0,0,0)).r*exposureScale/preExposure;
 dst[uint2(0,0)]=isfinite(e) && e>0 ? e : 1.0;
})";
std::string Layout(ID3D12Resource* resource)
{
    auto d = resource->GetDesc();
    return std::to_string(d.Width) + "x" + std::to_string(d.Height) + " format=" + std::to_string(d.Format) +
           " flags=" + std::to_string(d.Flags) + " samples=" + std::to_string(d.SampleDesc.Count) +
           " array=" + std::to_string(d.DepthOrArraySize) + " dimension=" + std::to_string(d.Dimension);
}
const AmdLayout* IdentifyRuntime(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
    BCRYPT_ALG_HANDLE alg {};
    unsigned char digest[32] {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return nullptr;
    auto result = BCryptHash(alg, nullptr, 0, data.data(), static_cast<ULONG>(data.size()), digest, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (result < 0)
        return nullptr;
    for (auto layout : kAmdLayouts)
        if (data.size() == layout->size && std::memcmp(digest, layout->sha256.bytes, 32) == 0)
            return layout;
    return nullptr;
}
ResourceV2 DescribeV2(HMODULE runtime, const AmdLayout* L, ID3D12Resource* resource, D3D12_RESOURCE_STATES state)
{
    if (!resource)
        return {};
    const auto d = resource->GetDesc();
    const auto format = reinterpret_cast<FormatEnumFn>(reinterpret_cast<uintptr_t>(runtime) + L->formatEnum)(d.Format);
    return { resource, 1, format, static_cast<UINT>(d.Format), static_cast<UINT>(state), static_cast<UINT>(d.Width),
             d.Height, 0 };
}
} // namespace
const char* IdentifyRuntimeName(const std::filesystem::path& passDll)
{
    auto* layout = IdentifyRuntime(passDll);
    return layout ? layout->name : nullptr;
}
// ---- NeuralHub 0.8.0 NGX arming (GATE-REPORT-0.8.0.md; validated by harness080/ngx_probe) ----
// 0 = not armed, 1 = armed, -1 = arming attempted and failed (permanent for the session).
std::atomic<int> g_ngxArm { 0 };
std::atomic<HMODULE> g_ngxHostModule { nullptr };
std::atomic<const AmdLayout*> g_ngxLayout { nullptr };

namespace
{
ID3D12CommandList* MatchNgxPendingLists(HMODULE h, const AmdLayout* L, UINT n, ID3D12CommandList* const* lists,
                                        bool& outSr, bool& outRr, int* outIndex = nullptr)
{
    outSr = false;
    outRr = false;
    if (outIndex)
        *outIndex = -1;
    if (!lists || !n || !h || !L || !L->ngxNotifyChain)
        return nullptr;
    ID3D12CommandList* srList =
        L->ngxSrPendingList ? At<ID3D12CommandList* volatile>(h, L->ngxSrPendingList) : nullptr;
    ID3D12CommandList* rrList =
        L->ngxRrPendingList ? At<ID3D12CommandList* volatile>(h, L->ngxRrPendingList) : nullptr;
    if (!srList && !rrList)
        return nullptr;
    ID3D12CommandList* matched = nullptr;
    for (UINT i = 0; i < n; ++i)
    {
        if (srList && lists[i] == srList)
        {
            outSr = true;
            if (!matched)
            {
                matched = lists[i];
                if (outIndex)
                    *outIndex = static_cast<int>(i);
            }
        }
        if (rrList && lists[i] == rrList)
        {
            outRr = true;
            if (!matched)
            {
                matched = lists[i];
                if (outIndex)
                    *outIndex = static_cast<int>(i);
            }
        }
    }
    return matched;
}

void WaitNgxInFlight(HMODULE h, const AmdLayout* L, bool checkSr, bool checkRr,
                     const std::function<void(const std::string&)>& log = {})
{
    if (!h || !L)
        return;
    const auto waitJob = [&](uint32_t notifiedRva, uint32_t doneRva, const char* label)
    {
        if (!notifiedRva || !doneRva)
            return;
        const UINT target = static_cast<UINT>(
            InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&At<UINT>(h, notifiedRva)), 0, 0));
        if (!target)
            return;
        const auto start = std::chrono::steady_clock::now();
        unsigned spins = 0;
        while (static_cast<UINT>(
                   InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&At<UINT>(h, doneRva)), 0, 0)) < target)
        {
            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsedMs > 500)
            {
                if (log)
                    log(std::string("AMD NGX ") + label + " completion wait timeout (target=" + std::to_string(target) +
                        ")");
                break;
            }
            if (++spins < 2000)
                YieldProcessor();
            else if (spins < 10000)
                SwitchToThread();
            else
                Sleep(1);
        }
    };
    if (checkSr)
        waitJob(L->ngxSrJobNotified, L->ngxSrJobDone, "SR");
    if (checkRr)
        waitJob(L->ngxRrJobNotified, L->ngxRrJobDone, "RR");
}

void NotifyNgxLists(HMODULE h, const AmdLayout* L, ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists,
                    bool waitCompletion, const std::function<void(const std::string&)>& log = {})
{
    bool srMatched = false, rrMatched = false;
    if (!MatchNgxPendingLists(h, L, n, lists, srMatched, rrMatched))
        return;
    // The slot holds no reference: 0.8.0's presentBefore writes the swapchain's queue there raw on every present, so
    // releasing what we find in it took one reference off the game's present queue per frame and freed it a few
    // frames after DLSS was picked (crash in dxgi.dll or ReShade64.dll). The reference lives here instead.
    static std::atomic<ID3D12CommandQueue*> held { nullptr };
    if (queue && L->queue && At<ID3D12CommandQueue*>(h, L->queue) != queue)
    {
        if (held.load(std::memory_order_acquire) != queue)
        {
            queue->AddRef();
            if (auto old = held.exchange(queue, std::memory_order_acq_rel))
                old->Release();
        }
        At<ID3D12CommandQueue*>(h, L->queue) = queue;
    }
    auto chain = At<NotifyFn volatile>(h, L->ngxNotifyChain);
    if (!chain)
        return;
    chain(queue, n, lists);
    if (waitCompletion)
        WaitNgxInFlight(h, L, srMatched, rrMatched, log);
}
} // namespace

int NgxPendingListIndex(UINT count, ID3D12CommandList* const* lists)
{
    HMODULE h = g_ngxHostModule.load(std::memory_order_acquire);
    const AmdLayout* L = g_ngxLayout.load(std::memory_order_acquire);
    bool sr = false, rr = false;
    int idx = -1;
    MatchNgxPendingLists(h, L, count, lists, sr, rr, &idx);
    return idx;
}

void NgxSubmitted(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists)
{
    if (!queue || !lists || !n)
        return;
    HMODULE h = g_ngxHostModule.load(std::memory_order_acquire);
    const AmdLayout* L = g_ngxLayout.load(std::memory_order_acquire);
    NotifyNgxLists(h, L, queue, n, lists, false);
}

// A present can pass two of the callers (the wrapped swapchain and the frame generation swapchain's hook, one inside
// the other): only the outermost pair reaches the host, so its counters advance once per present.
static thread_local int ngxPresentDepth = 0;
// The host took the present (its present counter moved): only then does its "after" stage run. A swapchain it turns
// away ("not on our device") left the "after" stage running alone.
static thread_local bool ngxPresentTaken = false;

// A host stage call above 20 ms, once a second at most (The Sims 4: 100 ms in every D3D12 present once FG had run).
static void ReportSlowNgxStage(const char* stage, std::chrono::steady_clock::time_point start)
{
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    static std::atomic<long long> lastReport { 0 };
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    if (ms > 20.0 && now - lastReport.load() > 1000)
    {
        lastReport = now;
        LOG_WARN("NeuralHub present stage {} took {:.1f} ms", stage, ms);
    }
}

void NgxPresentBefore(IUnknown* swapchain, IUnknown* queueHint)
{
    if (ngxPresentDepth++ != 0)
        return;
    ngxPresentTaken = false;
    HMODULE h = g_ngxHostModule.load(std::memory_order_acquire);
    const AmdLayout* L = g_ngxLayout.load(std::memory_order_acquire);
    if (!swapchain || !h || !L || !L->presentBefore || !L->presentCounter ||
        g_ngxArm.load(std::memory_order_acquire) != 1)
        return;
    // The host finds a swapchain's queue in the map its CreateSwapChain detour fills, then asks the swapchain itself;
    // host mode has no such detour, and under Streamline the swapchain answers no GetDevice(ID3D12CommandQueue), so every
    // present was turned away ("not on our device") and the FG's frame counters never moved. Tell it once per
    // swapchain, with the queue the game made it on (or the game's render queue).
    static IUnknown* registered = nullptr;
    if (L->swapchainCreated && swapchain != registered)
    {
        // A D3D11 game's swapchain hands its D3D11 device instead: the bridge's render queue then.
        ID3D12CommandQueue* queue = nullptr;
        if (!queueHint || FAILED(queueHint->QueryInterface(IID_PPV_ARGS(&queue))))
            if (auto* render = State::Instance().currentCommandQueue)
                render->QueryInterface(IID_PPV_ARGS(&queue));
        if (queue)
        {
            reinterpret_cast<void (*)(IUnknown*, IUnknown*)>(reinterpret_cast<std::uintptr_t>(h) +
                                                            L->swapchainCreated)(queue, swapchain);
            queue->Release();
            registered = swapchain;
        }
    }
    const auto counter = At<volatile std::uint64_t>(h, L->presentCounter);
    const auto start = std::chrono::steady_clock::now();
    reinterpret_cast<void (*)(IUnknown*)>(reinterpret_cast<std::uintptr_t>(h) + L->presentBefore)(swapchain);
    ReportSlowNgxStage("before", start);
    ngxPresentTaken = At<volatile std::uint64_t>(h, L->presentCounter) != counter;
    // Still turned away: the queue's device is not the one the host was seeded with. Said once.
    static bool reported = false;
    if (!ngxPresentTaken && !reported)
    {
        reported = true;
        ID3D12Device* queueDevice = nullptr;
        if (auto* queue = At<ID3D12CommandQueue*>(h, L->queue))
            queue->GetDevice(IID_PPV_ARGS(&queueDevice));
        LOG_WARN("NeuralHub's present stage turned swapchain {:X} away: host queue {:X} on device {:X}, host seeded with "
                 "device {:X}",
                 (size_t) swapchain, (size_t) At<ID3D12CommandQueue*>(h, L->queue), (size_t) queueDevice,
                 (size_t) At<ID3D12Device*>(h, L->device));
        if (queueDevice)
            queueDevice->Release();
    }
}

void NgxPresentAfter(IUnknown* swapchain, HRESULT result)
{
    if (ngxPresentDepth > 0 && --ngxPresentDepth != 0)
        return;
    const bool taken = ngxPresentTaken;
    ngxPresentTaken = false;
    HMODULE h = g_ngxHostModule.load(std::memory_order_acquire);
    const AmdLayout* L = g_ngxLayout.load(std::memory_order_acquire);
    if (!taken || !swapchain || !h || !L || !L->stageDispatch || g_ngxArm.load(std::memory_order_acquire) != 1)
        return;
    // The context the host's detour (0x12950) builds for stage 3 phase 2: size 0x58 and version 1, the swapchain,
    // two {1, kind} tags around the present queue, and the HRESULT.
    struct PresentContext
    {
        std::uint32_t size = 0x58;
        std::uint32_t version = 1;
        std::uint8_t reserved[0x28] {};
        IUnknown* swapchain = nullptr;
        std::uint32_t swapchainTag[2] { 1, 4 };
        ID3D12CommandQueue* queue = nullptr;
        std::uint32_t queueTag[2] { 1, 2 };
        HRESULT result = S_OK;
        std::uint32_t pad = 0;
    } context;
    static_assert(sizeof(PresentContext) == 0x58 && offsetof(PresentContext, swapchain) == 0x30 &&
                  offsetof(PresentContext, queue) == 0x40 && offsetof(PresentContext, result) == 0x50);
    context.swapchain = swapchain;
    context.queue = At<ID3D12CommandQueue*>(h, L->queue);
    context.result = result;
    const auto start = std::chrono::steady_clock::now();
    reinterpret_cast<void (*)(int, int, void*)>(reinterpret_cast<std::uintptr_t>(h) + L->stageDispatch)(3, 2,
                                                                                                         &context);
    ReportSlowNgxStage("after", start);
}

unsigned int NgxFgMultiFrameCountMax()
{
    HMODULE h = g_ngxHostModule.load(std::memory_order_acquire);
    const AmdLayout* L = g_ngxLayout.load(std::memory_order_acquire);
    if (!h || !L || !L->fgMultiFrameCountMax || g_ngxArm.load(std::memory_order_acquire) != 1)
        return 0;
    return At<std::uint32_t>(h, L->fgMultiFrameCountMax);
}

std::string RvaHex(std::uint32_t rva)
{
    std::string out("0x");
    char digits[12];
    snprintf(digits, sizeof(digits), "%x", rva);
    return out + digits;
}

bool NeuralHubArmOk()
{
    return g_ngxArm.load(std::memory_order_acquire) == 1;
}

namespace
{
std::atomic<bool> g_nrRefusedOnD3d12 { false };
}
bool NrRefusedOnHostD3d12() { return g_nrRefusedOnD3d12.load(std::memory_order_relaxed); }

bool ArmDlssNgx(HMODULE hostModule, const AmdLayout* L, const char* tag,
                const std::function<void(const std::string&)>& log)
{
    ::dlssnr::crashsnap::EnableCrashSnap();
    if (!hostModule || !L || !L->ngxPublishSlot || !L->ngxPresentFlag || !log)
        return false;
    std::string name = tag ? tag : L->name;
    if (g_ngxArm.load(std::memory_order_acquire) != 0)
        return g_ngxArm.load(std::memory_order_acquire) == 1;
    int expected = 0;
    if (!g_ngxArm.compare_exchange_strong(expected, -1))
        return g_ngxArm.load(std::memory_order_acquire) == 1;
    bool armed = false;
    try
    {
        // Step 1a: the host services init (0x13820) writes GetState into the slot; wait for it.
        auto* slot =
            reinterpret_cast<void* volatile*>(reinterpret_cast<std::uintptr_t>(hostModule) + L->ngxPublishSlot);
        for (int waited = 0; *slot == nullptr && waited < 2000; waited += 10)
            Sleep(10);
        if (*slot == nullptr)
        {
            log("AMD NGX arm: " + name + ": host services never wrote the GetState slot " +
                RvaHex(L->ngxPublishSlot) + "; publish failed");
        }
        else
        {
            // Step 1b: publish the "first Present" snapshot (the pinned leaf 0xa4a0, no arguments).
            // It writes the published queue (0x1a15628), the wait state (0x1a15620) and
            // ngxPresentFlag (0x1a15630 = the seeded device is set), while the CreateFeature-once
            // functions read exactly that dword at state+0x208.
            auto* publish = *reinterpret_cast<void* (__fastcall* volatile*)()>(slot);
            void* state = publish();
            const std::uint32_t presentFlag = At<volatile std::uint32_t>(hostModule, L->ngxPresentFlag);
            log("AMD NGX arm: " + name + ": host GetState -> " +
                std::to_string(reinterpret_cast<std::uintptr_t>(state)) + " firstPresentFlag=" +
                std::to_string(presentFlag) + " (" + RvaHex(L->ngxPresentFlag) + ")");
            // Step 1c: only a verified publish arms the gates.
            if (state && presentFlag == 1)
            {
                // Step 2: arm the SR/RR evaluate-gate bytes. init ("present published"), gpu
                // (the CreateFeature-once functions run the module's own RDNA3/4 check afterwards)
                // and weights ("weights: " byte; writing it removes the extract race the probe
                // saw work). The Record/Notify flow (record/notify RVAs below) does not touch
                // these, so packet plumbing is unchanged.
                At<volatile std::uint8_t>(hostModule, L->ngxGateSrInit) = 1;
                At<volatile std::uint8_t>(hostModule, L->ngxGateSrGpu) = 1;
                At<volatile std::uint8_t>(hostModule, L->ngxGateSrWeights) = 1;
                At<volatile std::uint8_t>(hostModule, L->ngxGateRrInit) = 1;
                At<volatile std::uint8_t>(hostModule, L->ngxGateRrGpu) = 1;
                At<volatile std::uint8_t>(hostModule, L->ngxGateRrWeights) = 1;
                log("AMD NGX arm: " + name + ": gates SR init=1 gpu=1 weights=1 (" +
                    RvaHex(L->ngxGateSrInit) + "/" + RvaHex(L->ngxGateSrGpu) + "/" +
                    RvaHex(L->ngxGateSrWeights) + ") RR init=1 gpu=1 weights=1 (" +
                    RvaHex(L->ngxGateRrInit) + "/" + RvaHex(L->ngxGateRrGpu) + "/" +
                    RvaHex(L->ngxGateRrWeights) + ")");
                armed = true;
            }
        }
    }
    catch (const std::exception& e)
    {
        log("AMD NGX arm: " + name + ": failed: " + e.what());
    }
    if (armed)
    {
        g_ngxHostModule.store(hostModule, std::memory_order_release);
        g_ngxLayout.store(L, std::memory_order_release);
    }
    g_ngxArm.store(armed ? 1 : -1, std::memory_order_release);
    if (!armed)
        return false;
    // Step 4: route the NGX front end into the host module, and let the DLSS DLL search find the
    // delivered nvngx_dlss(.d).dll that sit next to neuralhub_host.dll (GetModuleHandle never
    // reloads children the host already has in-process).
    const HMODULE routed = NVNGXProxy::NVNGXModule();
    if (routed == nullptr || routed == hostModule)
    {
        NVNGXProxy::InitNVNGX(hostModule);
        // NVNGXProxy's getters stay null until its D3D12 init; ask the module itself.
        if (NVNGXProxy::NVNGXModule() == hostModule &&
            GetProcAddress(hostModule, "NVSDK_NGX_D3D12_CreateFeature") != nullptr)
            log("AMD NGX arm: " + name + ": NVNGXProxy routed to neuralhub_host exports (NVSDK_NGX_D3D12_*)");
        else
            log("AMD NGX arm: " + name + ": NVNGXProxy routed, but D3D12 NGX exports are missing");
    }
    else
        log("AMD NGX arm: " + name + ": another NVNGX module is already live; front end not rerouted");
    wchar_t modulePath[MAX_PATH] {};
    const DWORD pathLength = GetModuleFileNameW(hostModule, modulePath, MAX_PATH);
    if (pathLength && pathLength < MAX_PATH)
    {
        const auto hostFolder = std::filesystem::path(modulePath).parent_path();
        const auto dlss = hostFolder / L"nvngx_dlss.dll";
        const auto dlssd = hostFolder / L"nvngx_dlssd.dll";
        const auto dlssg = hostFolder / L"nvngx_dlssg.dll";
        std::error_code ec;
        if (std::filesystem::exists(dlss, ec))
            State::Instance().NVNGX_DLSS_Path = dlss.wstring();
        if (std::filesystem::exists(dlssd, ec))
            State::Instance().NVNGX_DLSSD_Path = dlssd.wstring();
        if (std::filesystem::exists(dlssg, ec) && DlssNr::AmdBridge::NeuralHubFgInstalled())
            State::Instance().NVNGX_DLSSG_Path = dlssg.wstring();
        if (State::Instance().NVNGX_DLSS_Path.has_value() || State::Instance().NVNGX_DLSSD_Path.has_value() ||
            State::Instance().NVNGX_DLSSG_Path.has_value())
            log("AMD NGX arm: " + name + ": DLSS provider DLLs found next to neuralhub_host: " +
                HipRuntimeLoad::Utf8(hostFolder.wstring()));
    }
    else
        log("AMD NGX arm: " + name + ": could not resolve the host module folder for the DLSS DLLs");
    // The DLSS upscaler path must be treated as available from now on (menus, feature provider,
    // intercepted game NGX calls). Volatile: the ini is not rewritten by an arm step.
    Config::Instance()->DLSSEnabled.set_volatile_value(true);
    State::Instance().NeuralHubArmOk = true;
    log("AMD NGX arm: " + name + ": DLSS-SR/DLSS-RR via NeuralHub host armed");
    return true;
}
struct Backend::Impl
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> scaleBaseline, scaleOutput;
    ComPtr<ID3D12PipelineState> resolvePipeline;
    ComPtr<ID3D12Resource> motionCrop, depthCrop;
    std::unique_ptr<RtgiNative> rtgi;
    bool rtgiFailed = false;
    std::string rtgiStatus;
    ComPtr<ID3D12Resource> lookColour;
    ComPtr<ID3D12PipelineState> lookPipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12PipelineState> depthPipeline;
    ComPtr<ID3D12PipelineState> motionPipeline, exposurePipeline;
    UINT lastMotionWidth = 0, lastMotionHeight = 0;
    UINT lastInputWidth = 0, lastInputHeight = 0;
    bool hadExposure = false;
    std::array<HMODULE, 3> runtime {};
    // Set once InitPass finished a pass. Its module stays loaded when it did not (Backend::Retry refuses that).
    std::array<bool, 3> initialized {};
    std::array<UINT, 3> observedTimeouts {};
    std::filesystem::path directory;
    std::wstring modules = L"dlssnr_amd_pass";
    std::string status = "AMD pre-SR: not initialized";
    std::atomic<bool> failed { false };
    // The 0.8.0 host runs on its D3D12 compute runtime (DLSS-FG armed): its NR goes through the host's own D3D12 entry
    // (0x2d8a0), not Record, whose HIP engine is never initialised there. Record refuses NR until that is wired.
    bool hostD3d12 = false;
    std::atomic<bool> resetRequested { true };
    Settings lastSettings {};
    bool haveSettings = false;
    // Latch failures that invalidate the shared completion timeline. Such work
    // remains pinned rather than being retired using an untrustworthy fence.
    bool completionOrderValid = true;
    bool graphicsFallbackReported = false;
    UINT64 frames = 0, serial = 0;
    UINT64 gfxAdmitSamples = 0, gfxAdmitOk = 0;
    UINT64 gfxArmedSamples = 0, gfxDrawCalls = 0, gfxSpin0Calls = 0, gfxSpin1Calls = 0;
    std::map<std::string, UINT64> gfxReasons;
    std::array<GraphicsSnap::GraphicsStartupGate, 3> gfxStartup;
    GraphicsSnap::GraphicsRestartState gfxRestart;
    std::array<UINT64, 3> gfxNativeSamples {};
    std::array<int, 3> gfxLastRequested { -1, -1, -1 };
    std::array<int, 3> gfxLastSpin { -1, -1, -1 };
    std::array<bool, 3> gfxLastPso {};
    std::array<int, 3> gfxLastMode { -1, -1, -1 };
    std::array<UINT64, 3> gfxModeLogAt {};
    std::array<UINT, 3> gfxDetailSeen {};
    std::array<bool, 3> gfxStartupFallbackReported {};
    UINT64 lastSubmitted = 0, lastCompleted = 0, completedFrames = 0;
    UINT64 pendingSkips = 0, fenceSkips = 0, fenceRecoveries = 0;
    UINT64 retryAfter = 0, timeoutEvents = 0;
    bool resetAfterTimeout = false;
    // Per-job state and the GPU resources that job borrows. The original runtime is a single worker
    // on one HIP stream, so slots never run concurrently: an extra slot only
    // lets the CPU record the next frame while the previous job is still
    // retiring, instead of blocking the render thread in Submitted.
    //
    // Too few slots and a frame that finds every buffer busy is recorded with no
    // NR at all. Slot count therefore changes denoise coverage as well as frame
    // timing; the available aggregate logs do not establish a causal feedback
    // direction between skips and later capture waits.
    // Measured with the count flipped mid-run at one standing position:
    //
    //   Onimusha  2/3 slots: 0 skips; no frame-time difference detected
    //   YYSLS AB  2 slots: about 1200-1440 counter increments/segment; 3: 0
    //   YYSLS     separate 60 s sweep: 1800 at 2 slots; 0 at 3, 4 and 5
    //
    // The YYSLS "win" at two slots is frames that carried no NR at all. The
    // count is the runtime's own skip counter. The AB log segments and the
    // 45-second PresentMon windows do not share boundaries, so no skip rate is
    // derived from them and the separate sweep is not compared numerically.
    static constexpr UINT kMaxSlots = 5;
    // kDefaultSlots == 1 reproduces the original one-frame-outstanding behaviour
    // exactly, which is what the control build is for. The macro used to work the
    // other way round, so every build that forgot to define it silently produced
    // a lower-throughput single-slot build - a trap that caught this project once.
    // AMD_SINGLESLOT now only moves the default; the option can still raise it.
#ifdef AMD_SINGLESLOT
    static constexpr UINT kDefaultSlots = 1;
#else
    static constexpr UINT kDefaultSlots = 3;
#endif
    struct Slot
    {
        std::atomic<ID3D12CommandList*> pending { nullptr };
        std::atomic<UINT64> completion { 0 };
        std::array<UINT, 3> jobs {};
        // Passes recorded into THIS slot. Global activePasses is only the
        // config for the next Record; a slot that has not finished must retire and
        // notify against the count it was recorded with (hot 1↔2 pass change).
        // UINT_MAX = never recorded this slot. 0 is a real value (the runtime refused).
        static constexpr UINT kPassUnset = 0xffffffffu;
        UINT passCount = kPassUnset;
        bool afterUpscale = false;
        SubmissionState submission;
        ComPtr<ID3D12CommandQueue> submissionQueue;
        std::unique_ptr<ColorEncoding> decode, encode;
        // The original runtime reads this and writes its correction back into it (in place), so no
        // two outstanding jobs may share one.
        ComPtr<ID3D12Resource> colour;
        ComPtr<ID3D12Resource> exposureCopy;
    };
    // Every slot gets its own copy of the whole descriptor block. A single
    // shared block aliases across slots: Record repoints descriptor 1 at the
    // active slot's colour, so a list still executing for the other slot would
    // read and write the wrong texture. Design section 3.3 forbids that, and
    // section 3.2 already asked for 14 per slot - this is that.
    static constexpr UINT kDescriptorsPerSlot = 14;
    static constexpr UINT kDescriptors = kDescriptorsPerSlot * kMaxSlots;
    std::array<Slot, kMaxSlots> slots;
    // What the option asks for this frame, and how many buffers actually exist.
    // They differ for one frame at most: wantSlots is read straight from the
    // settings so a change takes effect immediately, and liveSlots trails it
    // until the buffers have been brought into line. Slot structs above
    // liveSlots hold no texture, so asking for three reserves memory for three.
    UINT wantSlots = kDefaultSlots;
    UINT liveSlots = 0;
    UINT activeSlot = 0;
    UINT skipWaits = 0;
    UINT recordCalls = 0;
    UINT unsubmittedSkips = 0;
    // The original runtime joins its workers and clears the abort buffer while it rebuilds staging,
    // which it does after a resize, a re-created upscaler context or an INI
    // change. While that work is unfinished the extra slot must not be used to skip
    // the Submitted wait - doing so hung the game.
    //
    // A timer cannot guard this: the rebuild happens on whichever later Record
    // The original runtime chooses, so any window simply expires first and the crash follows. It
    // publishes its own decision as a sticky byte instead - set when it detects
    // the change, cleared only after it has drained the queue and joined its
    // workers - and a rebuild happens on exactly those calls that read 1 at
    // entry. Reading it is therefore the real guard.
    bool NativeRebuilding() const
    {
        if (!L || !L->recreate)
            return true; // unknown layout: assume the worst
        for (UINT i = 0; i < runtime.size(); ++i)
            if (auto h = runtime[i])
                if (At<volatile uint8_t>(h, L->recreate) != 0)
                    return true;
        return false;
    }
    // True while any slot still owns the resources its job borrowed.
    bool AnySlotBusy() const
    {
        for (size_t k = 0; k < slots.size(); ++k)
            if (slots[k].pending.load(std::memory_order_acquire))
                return true;
        return false;
    }
    bool HasUnsubmitted() const
    {
        for (const auto& sl : slots)
            if (sl.pending.load(std::memory_order_acquire) && sl.submission.BlocksRecord())
                return true;
        return false;
    }
    // Prefilter: collect every slot whose recorded list appears in `lists`.
    // Only the atomic pending pointer is read here. Callers (Submitting/Submitted)
    // must NOT hold p->lock yet — Record holds that lock across the runtime
    // Record call, and taking it first deadlocks on same-thread re-entry.
    //
    // This must not stop at the first match. The next frame can reuse the very
    // same command-list pointer while an older slot holding that pointer is
    // still retiring, so one batch can match two slots. Returning the older,
    // already-submitted one made the caller's lock-held check fail and return,
    // and the newer slot was then never submitted: it stayed occupied until the
    // 5s abandon and every Record in between was skipped. `submitted` is a plain
    // bool written under p->lock, so it cannot be read here; hand every
    // candidate to the caller, which holds the lock and can choose.
    UINT FindPendingCandidates(UINT n, ID3D12CommandList* const* lists, std::array<UINT, kMaxSlots>& outSlots,
                               std::array<ID3D12CommandList*, kMaxSlots>& outPending) const
    {
        UINT count = 0;
        for (size_t k = 0; k < slots.size(); ++k)
        {
            auto candidate = slots[k].pending.load(std::memory_order_acquire);
            if (!candidate)
                continue;
            for (UINT i = 0; i < n; ++i)
            {
                if (lists[i] != candidate)
                    continue;
                outSlots[count] = static_cast<UINT>(k);
                outPending[count] = candidate;
                ++count;
                break;
            }
        }
        return count;
    }
    // Requires p->lock. Picks the first candidate that is still that slot's
    // pending list and has not been submitted yet.
    bool PickUnsubmitted(const std::array<UINT, kMaxSlots>& candSlots,
                         const std::array<ID3D12CommandList*, kMaxSlots>& candPending, UINT count, UINT& outSlot,
                         ID3D12CommandList*& outPending) const
    {
        for (UINT c = 0; c < count; ++c)
        {
            const auto& sl = slots[candSlots[c]];
            if (sl.pending.load() == candPending[c] && !sl.submission.submitted)
            {
                outSlot = candSlots[c];
                outPending = candPending[c];
                return true;
            }
        }
        return false;
    }
    // Highest fence value any slot is still waiting on.
    UINT64 LatestCompletion() const
    {
        UINT64 value = 0;
        for (size_t k = 0; k < slots.size(); ++k)
            value = (std::max) (value, slots[k].completion.load());
        return value;
    }
    bool deviceLostReported = false;
    bool shutDown = false;
    UINT width = 0, height = 0, activePasses = 0, lastPasses = 0;
    // 0.8.0: the passes its one module runs inside each job (activePasses is 1 there).
    UINT runtimePasses = 1;
    HipSetFn hipSet = nullptr;
    int hipDevice = -1;
    const AmdLayout* L = nullptr;
    std::mutex lock;
#ifdef AMD_RETIRE_DIAGNOSTICS
    RetirementDiagnostics diagnostics;
#endif
    void LogDiagnostic(const std::string& s) const
    {
        std::ofstream out(directory / L"amd_presr.log", std::ios::app);
        out << GetTickCount64() << " " << s << '\n';
    }
    void Log(const std::string& s)
    {
        status = s;
        LogDiagnostic(s);
    }
    void TraceBoundary(const std::string& reason)
    {
        const auto gpu = fence ? fence->GetCompletedValue() : 0;
        const auto removed = device->GetDeviceRemovedReason();
        // Report the first slot with work outstanding; with one slot that
        // is the only one, so the line keeps the original format.
        const Slot* traced = &slots[0];
        for (size_t k = 0; k < slots.size(); ++k)
            if (slots[k].pending.load(std::memory_order_acquire))
            {
                traced = &slots[k];
                break;
            }
        Log("AMD boundary: " + reason +
            " pending=" + std::to_string(reinterpret_cast<uintptr_t>(traced->pending.load())) +
            " submitted=" + std::to_string(traced->submission.submitted) +
            " recordedAt=" + std::to_string(traced->submission.recordedAt) +
            " submittedAt=" + std::to_string(traced->submission.submittedAt) + " fence=" + std::to_string(gpu) + "/" +
            std::to_string(traced->completion.load()) + " deviceHR=" + std::to_string(static_cast<UINT>(removed)) +
            " NR=" + std::to_string(width) + "x" + std::to_string(height));
        for (UINT i = 0; i < runtime.size(); ++i)
            if (auto h = runtime[i])
                Log("AMD boundary pass " + std::to_string(i + 1) +
                    " native=" + std::to_string(At<UINT>(h, L->jobDone)) + "/" + std::to_string(traced->jobs[i]) +
                    " nativePending=" + std::to_string(reinterpret_cast<uintptr_t>(At<void*>(h, L->pendingList))) +
                    " timeouts=" + std::to_string(At<UINT>(h, L->timeoutCount)));
        if (FAILED(removed) && !deviceLostReported)
        {
            deviceLostReported = true;
            failed = true;
            // Read whatever DRED the game/OS collected. Do not change device
            // creation settings or globally enable a debug layer in the game.
            ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
            if (SUCCEEDED(device.As(&dred)))
            {
                D3D12_DRED_PAGE_FAULT_OUTPUT1 fault {};
                const auto hr = dred->GetPageFaultAllocationOutput1(&fault);
                if (SUCCEEDED(hr))
                    Log("AMD DRED page fault: VA=" + std::to_string(fault.PageFaultVA));
                else
                    // 0x887a0004: driver did not report a page fault. Do not
                    // print "page fault" when the API said there was none.
                    Log("AMD DRED page fault: not available hr=" + std::to_string(static_cast<UINT>(hr)));
                D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs {};
                const auto bh = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
                if (SUCCEEDED(bh))
                {
                    Log("AMD DRED breadcrumbs: available");
                    UINT count = 0;
                    for (auto node = breadcrumbs.pHeadAutoBreadcrumbNode; node && count++ < 16; node = node->pNext)
                        Log("AMD DRED list=" + std::to_string(reinterpret_cast<uintptr_t>(node->pCommandList)) +
                            " progress=" +
                            std::to_string(node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0) + "/" +
                            std::to_string(node->BreadcrumbCount));
                }
                else
                    Log("AMD DRED breadcrumbs: not available hr=" + std::to_string(static_cast<UINT>(bh)));
            }
        }
    }
    // Diagnostic dump of every slot. Used when slots refuse to retire (yysls
    // skip/stall) so the next field run can show which state is stuck.
    void LogSlotSnapshot(const char* reason)
    {
        const auto gpuDone = fence ? fence->GetCompletedValue() : UINT64_MAX;
        Log(std::string("AMD slot-snap: ") + reason + " fence=" + std::to_string(gpuDone) +
            " lastSubmitted=" + std::to_string(lastSubmitted) + " completedFrames=" + std::to_string(completedFrames) +
            " pendingSkips=" + std::to_string(pendingSkips) + " nativeRebuild=" +
            std::to_string(NativeRebuilding() ? 1 : 0) + " failed=" + std::to_string(failed ? 1 : 0));
        for (size_t k = 0; k < slots.size(); ++k)
        {
            const auto& sl = slots[k];
            const auto pending = sl.pending.load(std::memory_order_acquire);
            const auto target = sl.completion.load();
            const UINT passCount = (sl.passCount == Slot::kPassUnset) ? 0u : sl.passCount;
            std::string jobs;
            std::string dones;
            for (UINT i = 0; i < static_cast<UINT>(sl.jobs.size()); ++i)
            {
                if (i)
                {
                    jobs += ",";
                    dones += ",";
                }
                jobs += std::to_string(sl.jobs[i]);
                UINT done = 0;
                if (L && i < runtime.size() && runtime[i])
                    done = static_cast<UINT>(InterlockedCompareExchange(
                        reinterpret_cast<volatile LONG*>(&At<UINT>(runtime[i], L->jobDone)), 0, 0));
                dones += std::to_string(done);
            }
            Log("AMD slot-snap k=" + std::to_string(k) +
                " pending=" + std::to_string(reinterpret_cast<uintptr_t>(pending)) +
                " submitted=" + std::to_string(sl.submission.submitted ? 1 : 0) + " recordedAt=" +
                std::to_string(sl.submission.recordedAt) + " submittedAt=" + std::to_string(sl.submission.submittedAt) +
                " passCount=" + std::to_string(passCount) + " jobs=[" + jobs + "] jobDone=[" + dones + "]" +
                " completion=" + std::to_string(target) +
                " fenceOk=" + std::to_string(gpuDone != UINT64_MAX && target != 0 && gpuDone >= target ? 1 : 0));
        }
    }
    // Retire one slot if its job has finished. Called with `lock` held. Keep
    // every borrowed resource alive until BOTH native inference and the actual
    // D3D12 submission have retired.
    void RetireSlot(UINT k, bool waitForGpu, const char* source
#ifdef AMD_RETIRE_DIAGNOSTICS
                    ,
                    RetirementDiagnostics::Event* sample
#endif
    )
    {
        Slot& sl = slots[k];
        if (!sl.pending.load(std::memory_order_acquire))
            return;
        if (!completionOrderValid)
            return;
        bool nativeDone = true;
        bool timedOut = false;
        // Use this slot's recorded pass count, not the global config.
        // 0 is valid (the runtime refused); only kPassUnset means "never recorded".
        const UINT passCount = sl.passCount == Slot::kPassUnset ? 0u : sl.passCount;
        for (UINT i = 0; i < passCount; ++i)
        {
            const auto done = static_cast<UINT>(
                InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&At<UINT>(runtime[i], L->jobDone)), 0, 0));
            nativeDone &= sl.jobs[i] != 0 && done >= sl.jobs[i];
#ifdef AMD_RETIRE_DIAGNOSTICS
            // sample is null for every slot except the one RetireSubmission
            // chose to instrument. Writing through it crashed a two-slot build as soon
            // as two slots were pending (s13/s14).
            if (sample)
                sample->done[i] = done;
#endif
            timedOut |= At<UINT>(runtime[i], L->timeoutCount) > observedTimeouts[i];
        }
        auto gpuDone = fence->GetCompletedValue();
        if (gpuDone == UINT64_MAX && !deviceLostReported)
            TraceBoundary("device removed while retiring");
        const auto target = sl.completion.load();
#ifdef AMD_RETIRE_DIAGNOSTICS
        if (sample)
        {
            sample->nativeDone = nativeDone; // Exactly the value passed to CanRetire.
            sample->gpuBefore = gpuDone;
            sample->target = target;
        }
#endif
        // Preserve the existing short recording-thread wait, but never block
        // on a list that the game has not submitted yet, or from Status().
        if (waitForGpu && sl.submission.submitted && nativeDone && gpuDone < target)
        {
#ifdef AMD_RETIRE_DIAGNOSTICS
            const auto waitStart = RetirementDiagnostics::Clock();
#endif
            const auto start = GetTickCount64();
            while (gpuDone < target && GetTickCount64() - start < 16)
            {
                Sleep(1);
                gpuDone = fence->GetCompletedValue();
            }
#ifdef AMD_RETIRE_DIAGNOSTICS
            if (sample)
                sample->waitMs = diagnostics.Milliseconds(RetirementDiagnostics::Clock() - waitStart);
#endif
        }
#ifdef AMD_RETIRE_DIAGNOSTICS
        if (sample)
        {
            sample->gpuAfter = gpuDone;
            sample->retired = sl.submission.CanRetire(nativeDone, gpuDone, target);
        }
#endif
        if (sl.submission.CanRetire(nativeDone, gpuDone, target))
        {
            sl.pending.store(nullptr, std::memory_order_release);
            sl.passCount = Slot::kPassUnset;
            sl.submission = {};
            sl.submissionQueue.Reset();
            // Clear the fence target. A later Record must not inherit a stale
            // completion from a previous generation (it made fenceOk look true
            // for a list that was never submitted).
            sl.completion.store(0, std::memory_order_release);
            lastSubmitted = GetTickCount64();
            if (!failed && passCount && !timedOut)
            {
                ++completedFrames;
                lastCompleted = lastSubmitted;
                status = "Completed AMD pre-SR passes=" + std::to_string(passCount) + " at " + std::to_string(width) +
                         "x" + std::to_string(height);
                if (completedFrames <= 3 || completedFrames % 120 == 0)
                    Log(status);
            }
            return;
        }
        if (sl.submission.ReportStall(GetTickCount64()))
        {
            Log("AMD submission stalled >5s; retaining list/resources until completion. submitted=" +
                std::to_string(sl.submission.submitted) + " nativeDone=" + std::to_string(nativeDone) + " passes=" +
                std::to_string(passCount) + " fence=" + std::to_string(gpuDone) + "/" + std::to_string(target));
            LogSlotSnapshot("stall");
        }
    }
    void RetireSubmission(bool waitForGpu = false, const char* source = "Unknown"
#ifdef AMD_RETIRE_DIAGNOSTICS
                          ,
                          RetirementDiagnostics::Event* recordEvent = nullptr
#endif
    )
    {
#ifdef AMD_RETIRE_DIAGNOSTICS
        RetirementDiagnostics::Scope timing(diagnostics, directory, L ? L->name : "uninitialized", source, recordEvent);
        auto& sample = timing.event;
        sample.passes = activePasses;
        sample.width = width;
        sample.height = height;
        sample.everyFrame = haveSettings && lastSettings.everyFrame;
        // Sample the first slot with work outstanding. In single-slot mode that is
        // the only slot, so the recorded diagnostic matches the original.
        UINT sampled = kMaxSlots;
        for (UINT k = 0; k < kMaxSlots; ++k)
            if (slots[k].pending.load(std::memory_order_acquire))
            {
                sampled = k;
                break;
            }
        if (sampled < kMaxSlots)
        {
            sample.pending = reinterpret_cast<uintptr_t>(slots[sampled].pending.load(std::memory_order_acquire));
            sample.submitted = slots[sampled].submission.submitted;
            sample.recordedAt = slots[sampled].submission.recordedAt;
            sample.submittedAt = slots[sampled].submission.submittedAt;
            sample.jobs = slots[sampled].jobs;
            sample.target = slots[sampled].completion.load();
            for (UINT k = 0; k < kMaxSlots; ++k)
                RetireSlot(k, waitForGpu, source, k == sampled ? &sample : nullptr);
        }
#else
        for (UINT k = 0; k < kMaxSlots; ++k)
            RetireSlot(k, waitForGpu, source);
#endif
    }
    // Execute has already happened. Wait only for HIP job-done, not the D3D12
    // fence: that fence covers FSR and the rest of the batch and was stalling
    // ExecuteCommandLists down to ~30 FPS. The original runtime's GPU inline still serializes NR
    // before FSR on the list. Record may still skip if the fence has not signaled yet.
    void WaitAfterSubmitIfEveryFrame(UINT k)
    {
        if (!haveSettings || !lastSettings.everyFrame)
            return;
        // The wait existed for one reason: with a single slot, the next Record
        // would skip unless this frame's job had already retired. An extra slot
        // is exactly what removes that need, so with two slots the render thread
        // must not block here - blocking is the cost this whole change removes.
        // Not while the original runtime is rebuilding, though: that is when the wait is load-bearing.
        if (wantSlots > 1 && !NativeRebuilding())
        {
            // Throttled trace of the fast path, so a run shows whether it was
            // taken and how far the native counter had progressed.
            if (++skipWaits <= 3 || skipWaits % 300 == 0)
                Log("AMD wait skipped (not rebuilding); count=" + std::to_string(skipWaits) +
                    " nativeDone=" + std::to_string(L && runtime[0] ? At<UINT>(runtime[0], L->jobDone) : 0) +
                    " job=" + std::to_string(slots[k].jobs[0]));
            return;
        }
#ifdef AMD_RETIRE_DIAGNOSTICS
        // Observational only: no wait behaviour is changed here.
        RetirementDiagnostics::Scope timing(diagnostics, directory, L ? L->name : "uninitialized", "EfWaitLoop");
        auto& sample = timing.event;
        sample.everyFrame = true;
        sample.passes = (slots[k].passCount == Slot::kPassUnset) ? 0u : slots[k].passCount;
        sample.width = width;
        sample.height = height;
        sample.jobs = slots[k].jobs;
        sample.target = slots[k].completion.load();
        const auto waitEntry = RetirementDiagnostics::Clock();
#endif
        // Use THIS slot's recorded pass count, not the global activePasses
        // (the next Record may already have rewritten it).
        const UINT slotPasses = (slots[k].passCount == Slot::kPassUnset) ? 0u : slots[k].passCount;
        unsigned iterations = 0;
#ifdef AMD_RETIRE_DIAGNOSTICS
        bool nativeAtEntry = true; // first poll result: did we wait at all?
#endif
        const auto start = GetTickCount64();
        while (GetTickCount64() - start < 80)
        {
            bool nativeDone = true;
            for (UINT i = 0; i < slotPasses; ++i)
            {
                if (!runtime[i] || slots[k].jobs[i] == 0)
                {
                    nativeDone = false;
                    break;
                }
                const auto done = static_cast<UINT>(InterlockedCompareExchange(
                    reinterpret_cast<volatile LONG*>(&At<UINT>(runtime[i], L->jobDone)), 0, 0));
                if (done < slots[k].jobs[i])
                {
                    nativeDone = false;
                    break;
                }
            }
#ifdef AMD_RETIRE_DIAGNOSTICS
            if (iterations == 0)
                nativeAtEntry = nativeDone;
#endif
            if (nativeDone)
            {
                RetireSubmission(false, "EveryFrameWait");
#ifdef AMD_RETIRE_DIAGNOSTICS
                sample.outcome = "done";
#endif
                break;
            }
            ++iterations;
            Sleep(1);
        }
#ifdef AMD_RETIRE_DIAGNOSTICS
        if (sample.outcome == std::string_view("poll"))
            sample.outcome = "budget"; // fell out of the 80 ms loop without finishing
        sample.waitIterations = iterations;
        sample.waitedBeforeDone = !nativeAtEntry;
        sample.gpuAfter = fence ? fence->GetCompletedValue() : 0;
        sample.waitMs = diagnostics.Milliseconds(RetirementDiagnostics::Clock() - waitEntry);
#endif
    }
    void InitHip()
    {
        if (hipSet)
            return;
        HipRuntimeLoad::WindowsApi api;
        const auto selected = HipRuntimeLoad::Initialize(api, device->GetAdapterLuid(),
                                                         [this](const std::string& message) { Log(message); });
        // Failed API/LUID/device checks never publish a partially initialized backend.
        hipDevice = selected.device;
        hipSet = selected.setDevice;
    }
    // abi 2: the runtime has to be loaded under the name neuralhub_host.dll, and in that mode it reads its settings,
    // writes its log and builds its weights in its own folder, so it gets one beside the pass files. Copied from
    // pass 1 when missing or of another runtime, as passes 2 and 3 are for the older runtimes. Its neuralhub.ini is
    // rewritten on every load: no add-on of its own asks for a stage (the host feeds the network itself), and the
    // compute backend follows [DlssNr] HostComputeBackend. HIP (the default) runs the network on AMD's HIP runtime
    // with the inline wait on the game's queue; D3D12 runs the same kernels in the game's command list, where there
    // is no second queue and no inline wait (the host itself suggests it when the HIP inline wait times out).
    std::filesystem::path PrepareHostModule(const std::filesystem::path& pass)
    {
        const auto folder = directory / L"dlssnr_amd_host";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (ec)
            throw std::runtime_error("Could not create dlssnr_amd_host: " + ec.message());
        const auto module = folder / L"neuralhub_host.dll";
        if (IdentifyRuntime(module) != L)
        {
            std::filesystem::copy_file(pass, module, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
                throw std::runtime_error("Could not copy " + pass.filename().string() +
                                         " to dlssnr_amd_host\\neuralhub_host.dll: " + ec.message());
            Log("AMD runtime: dlssnr_amd_host\\neuralhub_host.dll copied from " + pass.filename().string());
        }
        // NVIDIA's DLLs come from OptiScaler's library folder first: a copy there is the one the user chose, so the
        // host's follows it whenever they differ. The game's own copies only fill a gap, and never replace a newer
        // one already beside the host.
        const auto library = DlssNr::AmdBridge::LibraryFolder();
        for (const auto& name : { L"nvngx_dlss.dll", L"nvngx_dlssd.dll", L"nvngx_dlssg.dll" })
        {
            const auto dst = folder / name;
            const auto chosen = library / name;
            const bool fromLibrary = std::filesystem::exists(chosen, ec);
            const auto src = fromLibrary ? chosen : directory / name;
            if (!std::filesystem::exists(src, ec))
                continue;
            bool copy = !std::filesystem::exists(dst, ec);
            if (!copy && fromLibrary)
                copy = std::filesystem::file_size(src, ec) != std::filesystem::file_size(dst, ec) ||
                       std::filesystem::last_write_time(src, ec) != std::filesystem::last_write_time(dst, ec);
            if (!copy)
                continue;
            std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
                Log("AMD runtime: could not copy " + HipRuntimeLoad::Utf8(src.wstring()) + " to dlssnr_amd_host: " +
                    ec.message());
            else
                Log("AMD runtime: dlssnr_amd_host\\" + HipRuntimeLoad::Utf8(name) + " copied from " +
                    (fromLibrary ? "OptiScaler's library folder" : "game folder"));
        }
        // NVIDIA's DLSS provider DLLs next to the host module arm the DLSS-SR/DLSS-RR
        // evaluate gates (GATE-REPORT-0.8.0.md): with them present this build's arm step
        // takes the game's NGX evaluates through DLSS's own network. Without them the
        // module's FSR provider would answer the DLSS evaluates with the game's loaded
        // FFX API, whose version may differ from the driver's DLL this provider expects
        // (a DLSS evaluate crashed on the game's FFX 2.3.0: exception 0xc0000005 after
        // "replaced by FSR provider"), so Passthrough stays the fallback.
        const bool armNgx = std::filesystem::exists(folder / L"nvngx_dlss.dll", ec);
        const bool hip = DlssNr::AmdBridge::HostKeptOnHip();
        // With danielblnc's NR only FGOutput=dlssg (the installer's DLSS-FG box) or HostComputeBackend=D3D12 arms DLSS-FG:
        // a game's own nvngx_dlssg.dll (Cyberpunk, The Witcher 3) put the host on D3D12 and left the NR off for good.
        const LUID luid = device->GetAdapterLuid();
        const bool d3d12Ok = DlssNr::AmdBridge::HostD3d12Supported(&luid);
        const bool armFg = DlssNr::AmdBridge::NeuralHubFgInstalled() && d3d12Ok;
        std::ofstream ini(folder / L"neuralhub.ini", std::ios::binary | std::ios::trunc);
        ini << "; Written by OptiScaler on every load: the runtime is hosted";
        if (armNgx)
        {
            ini << "; NVIDIA's nvngx_dlss(.d).dll arm the DLSS-SR/DLSS-RR gates\r\n"
                   "[Upscaling]\r\nProvider=DLSS-SR\r\n[RayReconstruction]\r\nProvider=DLSS-RR\r\n";
            Log("AMD runtime: neuralhub.ini arms DLSS-SR/DLSS-RR providers (NVngx DLLs in dlssnr_amd_host)");
        }
        else
        {
            ini << ", its own add-ons stay idle.\r\n"
                   "[Upscaling]\r\nProvider=Passthrough\r\n[RayReconstruction]\r\nProvider=Passthrough\r\n";
        }
        if (armFg)
        {
            ini << "[FrameGeneration]\r\nProvider=DLSS-FG\r\n";
            Log("AMD runtime: neuralhub.ini arms DLSS-FG provider");
        }
        else
        {
            ini << "[FrameGeneration]\r\nProvider=Off\r\n";
        }
        // auto follows the FG: D3D12 for it, HIP for danielblnc's NR. HIP and D3D12 are taken as asked, except D3D12 on
        // a GPU before RDNA3, where the driver refuses the host's code object and removes the device.
        std::string hostBackend = Config::Instance()->HostComputeBackend.value_or_default();
        if (_stricmp(hostBackend.c_str(), "auto") == 0)
            hostBackend = armFg ? "D3D12" : "HIP";
        else if (hip)
            Log("AMD runtime: HostComputeBackend=HIP keeps danielblnc's NR running; NeuralHub's DLSS-FG stays off");
        else if (!d3d12Ok && _stricmp(hostBackend.c_str(), "D3D12") == 0)
        {
            hostBackend = "HIP";
            Log("AMD runtime: HostComputeBackend=D3D12 ignored: the host's D3D12 runtime needs an RX 7000 or newer "
                "(gfx11/gfx12); HIP instead, NeuralHub's DLSS-FG off");
            LOG_WARN("AMD neural: HostComputeBackend=D3D12 ignored on this GPU (needs gfx11/gfx12), the host runs on HIP");
        }
        ini << "[Enhancement]\r\nProvider=Off\r\n[Compute]\r\nBackend="
            << hostBackend << "\r\n";
        if (armFg)
        {
            ini << "[Compatibility]\r\nGpuSpoof=Streamline\r\nIgnoreHagsForFrameGeneration=Auto\r\n";
            Log("AMD runtime: neuralhub.ini enables GpuSpoof=Streamline and IgnoreHagsForFrameGeneration=Auto for DLSS-FG");
        }
        else
        {
            ini << "[Compatibility]\r\nGpuSpoof=Off\r\nIgnoreHagsForFrameGeneration=Off\r\n";
        }
        // [Debug] HipZeroCopy: inline mode needs zero-copy; without it the host copies the inputs and
        // runs the network asynchronously (no inline wait, so a staging re-create cannot deadlock it).
        const bool zeroCopy = Config::Instance()->HostZeroCopy.value_or_default();
        ini << "[Debug]\r\nHipZeroCopy=" << (zeroCopy ? 1 : 0) << "\r\n";
        if (!ini)
            throw std::runtime_error("Could not write dlssnr_amd_host\\neuralhub.ini");
        Log("AMD runtime: neuralhub.ini [Compute] Backend=" + hostBackend +
            ", [Debug] HipZeroCopy=" + (zeroCopy ? "1" : "0"));
        return folder;
    }
    // abi 2 without dlssnr_on_amd_weights.bin beside the passes: the runtime builds it from nvngx_dlssnr.dll in its
    // own folder (copied there from beside the passes or the game's executable), as its first present would.
    std::filesystem::path BuildHostWeights(HMODULE h, const std::filesystem::path& folder)
    {
        const auto built = folder / L"dlssnr_on_amd_weights.bin";
        if (!std::filesystem::exists(built))
        {
            const auto snippet = folder / L"nvngx_dlssnr.dll";
            if (!std::filesystem::exists(snippet))
            {
                wchar_t exe[MAX_PATH] {};
                GetModuleFileNameW(nullptr, exe, MAX_PATH);
                for (const auto& source : { directory / L"nvngx_dlssnr.dll",
                                            std::filesystem::path(exe).parent_path() / L"nvngx_dlssnr.dll" })
                {
                    std::error_code ec;
                    if (std::filesystem::exists(source) &&
                        std::filesystem::copy_file(source, snippet, std::filesystem::copy_options::overwrite_existing,
                                                   ec))
                        break;
                }
            }
            if (!std::filesystem::exists(snippet))
                throw std::runtime_error("dlssnr_on_amd_weights.bin is required (or nvngx_dlssnr.dll to build it)");
            At<uint8_t>(h, L->weightsOnce) = 1;
            reinterpret_cast<void (*)()>(reinterpret_cast<uintptr_t>(h) + L->prepareWeights)();
            if (!std::filesystem::exists(built))
                throw std::runtime_error("AMD runtime could not build dlssnr_on_amd_weights.bin from nvngx_dlssnr.dll "
                                         "(see dlssnr_amd_host\\dlssnr_on_amd.log)");
            Log("AMD runtime: dlssnr_on_amd_weights.bin built from nvngx_dlssnr.dll");
        }
        return built;
    }
    // EnsureCompute's D3D12 branch without its env log (which reads a swapchain the host has not seen yet): open
    // the runtime on the seeded device and record it open, so the addons' own EnsureCompute calls return at once.
    void OpenHostD3d12(HMODULE h)
    {
        if (!L->computeOpen || !L->computeState)
            throw std::runtime_error("AMD runtime " + std::string(L->name) + " has no D3D12 compute runtime mapped");
        if (At<int>(h, L->computeState) == 1)
            return;
        // The host's std::string (MSVC layout), as EnsureCompute builds it: empty, inline buffer. A longer reason
        // lands in the host's heap and stays there (once per process).
        struct HostString
        {
            char buffer[16] {};
            std::size_t size = 0;
            std::size_t capacity = 15;
        } reason;
        using OpenFn = bool (*)(ID3D12Device*, HostString*);
        const bool open = reinterpret_cast<OpenFn>(reinterpret_cast<uintptr_t>(h) + L->computeOpen)(device.Get(), &reason);
        const char* text = reason.capacity >= sizeof(reason.buffer) ? *reinterpret_cast<const char* const*>(reason.buffer)
                                                                    : reason.buffer;
        if (!open)
        {
            At<int>(h, L->computeState) = -1;
            throw std::runtime_error("AMD runtime: the host's D3D12 compute runtime did not open: " +
                                     std::string(text, reason.size));
        }
        At<int>(h, L->computeState) = 1;
        Log("AMD runtime: host D3D12 compute runtime open (see dlssnr_amd_host\\dlssnr_on_amd.log)");
    }
    void InitPass(UINT i)
    {
        // A module that loaded but whose initialization failed stays loaded (see below) and refuses every Record:
        // that is a failure each time, not a pass that is ready.
        if (runtime[i])
        {
            if (!initialized[i])
                throw std::runtime_error("AMD pass " + std::to_string(i + 1) + " failed to initialize earlier");
            return;
        }
        if (i > 0 && L && L->abi >= 2)
            throw std::runtime_error("AMD runtime " + std::string(L->name) + " runs its passes in one module");
        InitHip();
        auto path = directory / (modules + std::to_wstring(i + 1) + L".dll");
        // One module per pass, so one file per pass: passes 2 and 3 are copies of pass 1, made here when missing or
        // of another runtime, so a runtime is installed by replacing pass 1 alone. Pass 1 loaded first and set L.
        // A backend with modules of its own copies its first one from dlssnr_amd_pass1.dll too.
        const bool own = modules != L"dlssnr_amd_pass";
        const AmdLayout* source = own && i == 0 ? IdentifyRuntime(directory / L"dlssnr_amd_pass1.dll") : nullptr;
        if ((i > 0 && IdentifyRuntime(path) != L) || (own && i == 0 && IdentifyRuntime(path) != source))
        {
            std::error_code ec;
            std::filesystem::copy_file(directory / L"dlssnr_amd_pass1.dll", path,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
                throw std::runtime_error("Could not copy dlssnr_amd_pass1.dll to pass " + std::to_string(i + 1) + ": " +
                                         ec.message());
            Log("AMD runtime: " + path.filename().string() + " copied from dlssnr_amd_pass1.dll");
        }
        auto identified = IdentifyRuntime(path);
        if (!identified)
            throw std::runtime_error("Private AMD runtime hash mismatch: pass " + std::to_string(i + 1));
        if (L && L != identified)
            throw std::runtime_error("Mixed AMD runtime versions across passes");
        L = identified;
        Log(std::string("AMD runtime ") + L->name);
        auto weights = directory / L"dlssnr_on_amd_weights.bin";
        const bool host = L->abi >= 2;
        if (!std::filesystem::exists(weights) && !(host && L->prepareWeights))
            throw std::runtime_error("dlssnr_on_amd_weights.bin is required");
        HMODULE h {};
        std::filesystem::path hostFolder;
        if (host)
        {
            hostFolder = PrepareHostModule(path);
            h = RuntimeHostLoad::LoadHost((hostFolder / L"neuralhub_host.dll").c_str(), L);
            runtime[i] = h;
            Log("AMD runtime host mode: loaded as neuralhub_host.dll, its hooks and interceptors off; host owns "
                "submission and configuration");
            if (!std::filesystem::exists(weights))
                weights = BuildHostWeights(h, hostFolder);
        }
        else
        {
            // A's standalone DllMain normally creates its own hook thread. Isolate
            // the pinned bootstrap BEFORE it can run; a post-LoadLibrary patch races it.
            h = RuntimeHostLoad::Load(path.c_str(), L);
            // Retain module even on failure: CRT registered HIP kernels; no unsafe unloading.
            runtime[i] = h;
            auto isolated = std::string("AMD runtime bootstrap isolated: host owns submission and configuration");
            if (RuntimeHostLoad::LastPathFailures())
                isolated += " (module path spelling differed from load path)";
            Log(isolated);
        }
        // The hash above fixes this private module's import layout. Older games
        // ship a 2013 D3DCompiler that rejects the FP16 typed UAV load shader.
        // Bind only this module's compiler import; leave the game's DLL intact.
        static HMODULE systemCompiler = []
        {
            wchar_t systemPath[MAX_PATH] {};
            auto length = GetSystemDirectoryW(systemPath, MAX_PATH);
            if (!length || length >= MAX_PATH)
                return HMODULE(nullptr);
            auto path = std::filesystem::path(systemPath) / L"d3dcompiler_47.dll";
            return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }();
        auto compile = systemCompiler ? GetProcAddress(systemCompiler, "D3DCompile") : nullptr;
        if (!compile)
            throw std::runtime_error("System D3DCompile unavailable for AMD neural shaders");
        if (L->d3dCompileIat)
        {
            auto import = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(h) + L->d3dCompileIat);
            DWORD previousProtection = 0;
            if (!VirtualProtect(import, sizeof(void*), PAGE_READWRITE, &previousProtection))
                throw std::runtime_error("Could not bind private AMD shader compiler");
            InterlockedExchangePointer(import, reinterpret_cast<void*>(compile));
            DWORD unused = 0;
            if (!VirtualProtect(import, sizeof(void*), previousProtection, &unused))
                throw std::runtime_error("Could not restore private AMD import protection");
            Log("Private AMD shaders use System32 D3DCompiler; game compiler preserved");
        }
        else
            Log("AMD runtime has no D3DCompile import; using engine default");
        // All passes notify after the bridge's single real submission.
        At<NotifyFn>(h, L->trampoline) = AlreadySubmitted;

        At<ID3D12Device*>(h, L->device) = device.Get();
        device->AddRef();
        At<ID3D12CommandQueue*>(h, L->queue) = queue.Get();
        queue->AddRef();
        At<int>(h, L->hipOrdinal) = hipDevice;
        // [Compute] Backend=D3D12 (DLSS-FG needs it): open the host's D3D12 runtime with the seeded device before
        // anything dispatches, as its EnsureCompute does at the first present (AmdLayout.h, computeOpen).
        hostD3d12 = host && L->computeMode && At<int>(h, L->computeMode) == 2;
        if (hostD3d12)
            OpenHostD3d12(h);
        // NeuralHub 0.8.0 NGX host arming (GATE-REPORT-0.8.0.md): with device, queue and HIP
        // ordinal seeded, publish the "first Present" state once through the host's own
        // GetState (0xa4a0) and write the SR/RR evaluate-gate bytes, so NGX DLSS-SR/DLSS-RR
        // routed into this module pass the gates on an AMD GPU. Idempotent, once per process.
        if (L->ngxPublishSlot)
            ArmDlssNgx(h, L, L->name, [this](const std::string& s) { Log(s); });
        At<uint8_t>(h, L->configuredInline) = 1;
        At<uint8_t>(h, L->interop) = 1;
        At<uint8_t>(h, L->enabled) = 1;
        if (L->fsrInputs)
            At<uint8_t>(h, L->fsrInputs) = 1;
        At<uint8_t>(h, L->depthPresent) = 1;
        At<int>(h, L->tonemap) = -1;
        // 0.4.x reads these from dlssnr_on_amd.ini in DllMain, and only its own Present detour
        // reads them again, which the isolated bootstrap never installs. Pin the runtime defaults,
        // which reproduce 0.3.1: no style, Reinhard, no lift, the game's exposure when given. The
        // style, tone curve, lift, exposure source and Quality then follow the menu on every frame.
        if (L->style)
            At<int>(h, L->style) = 0;
        if (L->toneCurve)
            At<int>(h, L->toneCurve) = 0;
        if (L->toneLift)
            At<float>(h, L->toneLift) = 0.f;
        if (L->useGameExposure)
            At<uint8_t>(h, L->useGameExposure) = 1;
        std::string file = weights.string();
        if (L->initCallback)
            At<uintptr_t>(h, L->initCallback) = reinterpret_cast<uintptr_t>(h) + L->initCallbackFn;
        // On D3D12 the host's own NR setup (0x2bd90) skips the HIP engine's init too: the network's weights load into
        // VRAM at its first upscale.
        if (hostD3d12)
            Log("AMD runtime: D3D12 compute, the HIP engine's init is skipped (NR weights load at the first upscale)");
        else if (hipSet(hipDevice) != 0 || !reinterpret_cast<InitFn>(reinterpret_cast<uintptr_t>(h) + L->init)(
                                               reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(h) + L->engine), &file))
            throw std::runtime_error("AMD engine initialization failed");
        // 0.8.0 keeps the inline switch in the engine object, which init constructs with inline on.
        if (host)
            At<uint8_t>(h, L->configuredInline) = 1;
        // SpinDraw must be set before the first staging Record so A can create
        // its 1-pixel-draw PSO. AmdGraphicsWait=1 requests new wait; otherwise original wait.
        if (L->spinDraw)
        {
            // New wait when this invocation armed a restore plan, or AmdGraphicsUnsafe
            // (dirty insert: no complete D3D12 graphics-state restore).
            int want = Config::Instance()->AmdGraphicsWait.value_or_default() ? 1 : 0;
            if (want && !GraphicsSnap::RestoreArmed() && !Config::Instance()->AmdGraphicsUnsafe.value_or_default())
                want = 0;
            At<int>(h, L->spinDraw) = want;
            Log(want ? std::string("AMD runtime: SpinDraw=1 (new wait via AmdGraphicsWait)")
                     : std::string("AMD runtime: SpinDraw=0 (original wait)"));
        }
        At<uint8_t>(h, L->initDone) = 1;
        initialized[i] = true;
        Log("Initialized independent AMD pass " + std::to_string(i + 1));
    }
    void InitShader()
    {
        if (root)
            return;
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 };
        D3D12_ROOT_PARAMETER params[3] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, 24 };
        D3D12_DESCRIPTOR_RANGE residualRange { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, 0 };
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = { 1, &residualRange };
        D3D12_ROOT_SIGNATURE_DESC desc { 3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, error;
        Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
              "Root signature serialize");
        Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
              "Root signature create");
        Check(D3DCompile(CopyShader, sizeof(CopyShader), "AMD active crop", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error),
              "Crop shader compile");
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps {};
        ps.pRootSignature = root.Get();
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&pipeline)), "Crop pipeline");
        Check(D3DCompile(DepthShader, sizeof(DepthShader), "AMD depth conversion", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error),
              "Depth shader compile");
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&depthPipeline)), "Depth pipeline");
        Check(D3DCompile(MotionShader, sizeof(MotionShader), "AMD motion resample", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error),
              "Motion shader compile");
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&motionPipeline)), "Motion pipeline");
        Check(D3DCompile(ExposureShader, sizeof(ExposureShader), "AMD exposure conversion", nullptr, nullptr, "main",
                         "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error),
              "Exposure shader compile");
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&exposurePipeline)), "Exposure pipeline");
        Check(D3DCompile(ResolveShader, sizeof(ResolveShader), "AMD residual resolve", nullptr, nullptr, "main",
                         "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error),
              "Resolve compile");
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&resolvePipeline)), "Resolve pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kDescriptors,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "Crop heap");
    }
};
Backend::Backend(ID3D12Device* d, ID3D12CommandQueue* q, const std::filesystem::path& dir, const wchar_t* modules)
    : p(new Impl)
{
    p->device = d;
    p->queue = q;
    p->directory = dir;
    p->modules = modules;
    // The tail of this line identifies the build. Four earlier rounds were
    // analysed without it and the logs could not be told apart.
#ifdef AMD_SINGLESLOT
    static constexpr const char* kBuildTag = " [r27-contract default=1 control]";
#else
    static constexpr const char* kBuildTag = " [r27-contract default=3 cap=5]";
#endif
    // The build tag names the default, not the count in force: the option can
    // change it while the game runs, and the change logs its own line when it
    // lands. Three earlier rounds were analysed without a tag and the logs could
    // not be told apart.
    p->LogDiagnostic("AMD graphics build source=" AMD_GRAPHICS_SOURCE_ID);
    p->Log("AMD submission revision 20260927-1.8.9: multi-slot default; 0.3.1/0.4.x new wait with guarded restore; "
           "Every-frame back on Ins menu; runtime 0.8.0 (host mode), 0.6.0, 0.5.1, 0.5.0, 0.4.3, 0.4.2 or 0.4.1" +
           std::string(kBuildTag));
    try
    {
        Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p->fence)), "Completion fence");
        const auto pass1 = dir / (std::wstring(modules) + L"1.dll");
        const AmdLayout* layout = IdentifyRuntime(pass1);
        if (layout && layout->abi >= 2 && layout->ngxPublishSlot)
            p->InitPass(0);
    }
    catch (const std::exception& e)
    {
        p->failed = true;
        p->Log(e.what());
    }
}
ID3D12Resource* Backend::Record(ID3D12GraphicsCommandList* cmd, const Frame& incoming, const Settings& cfg)
{
    // Install missing observers on this list's actual implementation, before
    // holding the backend lock or calling A. Observation never gates NR.
    const auto earlyDrawTarget = D3D12Hooks::NativeDrawHookTarget();
    GraphicsSnap::NativeWaitHooks::Coverage waitCoverage {};
    if (cmd && cfg.spinDraw && Config::Instance()->AmdGraphicsWait.value_or_default())
        waitCoverage = GraphicsSnap::NativeWaitHooks::Ensure(cmd, earlyDrawTarget);
    std::lock_guard guard(p->lock);
    // wantSlots is plain state guarded by `lock`, like liveSlots and activeSlot.
    // Widening the choice of buffer here only lets the pick below take a slot
    // whose texture does not exist yet; the rebuild pass further down runs in
    // this same call and creates it before anything is recorded into it.
    p->wantSlots = (std::clamp) (cfg.slots, 1u, Impl::kMaxSlots);
    Frame f = incoming;
#ifdef AMD_RETIRE_DIAGNOSTICS
    p->diagnostics.BeginRecord(p->frames != 0);
    RetirementDiagnostics::Scope timing(p->diagnostics, p->directory, p->L ? p->L->name : "uninitialized", "Record");
    timing.event.outcome = "other_skip";
    p->RetireSubmission(true, "Record", &timing.event);
#else
    p->RetireSubmission(true);
#endif
    if (p->failed || !cmd || !f.colour || !f.motion || !f.depth)
        return nullptr;
    if (p->hostD3d12)
    {
        static const std::string refused =
            "AMD runtime: danielblnc's NR does not run while its host is on D3D12 for DLSS-FG; choose lmxxf or "
            "mochizuki for NR, or set [DlssNr] HostComputeBackend=HIP for danielblnc's NR without NeuralHub's FG";
        if (p->status != refused)
        {
            p->status = refused;
            p->Log(refused);
        }
        if (!g_nrRefusedOnD3d12.exchange(true, std::memory_order_relaxed))
            LOG_WARN("{}", refused);
        return nullptr;
    }
    const auto listType = cmd->GetType();
    if (listType != D3D12_COMMAND_LIST_TYPE_DIRECT && listType != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        return nullptr;
    const auto listId = reinterpret_cast<uint64_t>(cmd);
    GraphicsSnap::InvocationState noEnvelope {};
    noEnvelope.listId = listId;
    noEnvelope.listType = static_cast<UINT>(listType);
    noEnvelope.requested = cfg.spinDraw != 0;
    auto* gfx = GraphicsSnap::GraphicsInvocationFor(listId);
    if (!gfx)
        gfx = &noEnvelope;
    const auto logGraphics = [&]()
    {
        if (!gfx->requested)
            return;
        ++p->gfxAdmitSamples;
        p->gfxAdmitOk += gfx->admitted;
        p->gfxArmedSamples += gfx->armed;
        ++p->gfxReasons[gfx->reason];
        if (p->gfxAdmitSamples <= 3 || p->gfxAdmitSamples % 300 == 0)
        {
            std::string histogram;
            for (const auto& [reason, count] : p->gfxReasons)
                histogram += " " + reason + "=" + std::to_string(count);
            p->LogDiagnostic(
                "AMD graphics admission n=" + std::to_string(p->gfxAdmitSamples) +
                " ok=" + std::to_string(p->gfxAdmitOk) + " reason=" + gfx->reason +
                " requested=" + std::to_string(gfx->requested) + " list=" + std::to_string(listId) +
                " listType=" + std::to_string(gfx->listType) + " generation=" + std::to_string(gfx->generation) +
                " generationKnown=" + std::to_string(gfx->generationKnown) + " predDisabled=" +
                std::to_string(gfx->predDisabled) + " renderPassIdle=" + std::to_string(gfx->renderPassIdle) +
                " psoReady=" + std::to_string(gfx->psoReady) + " admitted=" + std::to_string(gfx->admitted) +
                " freeze=" + std::to_string(gfx->frozen) + " pin=" + std::to_string(gfx->pinned) +
                " plan=" + std::to_string(gfx->planned) + " armed=" + std::to_string(gfx->armed) +
                " outcome=" + gfx->outcome + " gates{" + gfx->gates + "}");
            p->LogDiagnostic("AMD graphics totals armed=" + std::to_string(p->gfxArmedSamples) + " nativeSpin0=" +
                             std::to_string(p->gfxSpin0Calls) + " nativeSpin1=" + std::to_string(p->gfxSpin1Calls) +
                             " drawObserved=" + std::to_string(p->gfxDrawCalls) + " reasons:" + histogram);
        }
    };
    struct LogGraphicsOnReturn
    {
        const decltype(logGraphics)& log;
        ~LogGraphicsOnReturn() { log(); }
    } graphicsLog { logGraphics };
    // Dispatch/Copy are illegal inside (or between suspended/resuming) render
    // passes too. This must skip all NR commands, not just switch to compute.
    if (GraphicsSnap::GraphicsTracker().IsRenderPassUnsafe(listId))
    {
        gfx->outcome = "render_pass_skip";
        return nullptr;
    }
    // A owns only one not-yet-notified list/job. Submitted slots remain free
    // to overlap; do not overwrite that singleton while waiting for Execute.
    if (p->HasUnsubmitted())
    {
#ifdef AMD_RETIRE_DIAGNOSTICS
        timing.event.outcome = "unsubmitted_skip";
#endif
        if (++p->unsubmittedSkips <= 3 || p->unsubmittedSkips % 120 == 0)
            p->Log("AMD skipped: previous Record still awaits submission; count=" +
                   std::to_string(p->unsubmittedSkips));
        return nullptr;
    }
    if (Config::Instance()->AmdGraphicsWait.value_or_default() && cfg.spinDraw == 0 && !p->graphicsFallbackReported)
    {
        p->graphicsFallbackReported = true;
        p->Log("AMD AmdGraphicsWait requested but Settings.spinDraw is 0; check bridge wiring");
    }
    const auto deviceStatus = p->device->GetDeviceRemovedReason();
    if (FAILED(deviceStatus))
    {
        p->failed = true;
        p->Log("AMD stopped: D3D12 device lost, HRESULT=" + std::to_string(static_cast<UINT>(deviceStatus)));
        p->TraceBoundary("Record device removed");
        return nullptr;
    }
    // Pick the slot for this frame. With one slot this is the original
    // behaviour: that slot must have retired or the frame is skipped. With two,
    // the second slot lets the CPU keep recording while the previous job is
    // still retiring, instead of blocking the render thread in Submitted.
    //
    // Only slots below wantSlots are handed out, so lowering the option takes
    // effect on this frame; the buffers above it are released a frame or two
    // later, once whatever is still using them has retired.
    Impl::Slot* sl = nullptr;
    for (UINT k = 0; k < p->wantSlots; ++k)
        if (!p->slots[k].pending.load(std::memory_order_acquire))
        {
            sl = &p->slots[k];
            p->activeSlot = static_cast<UINT>(k);
            break;
        }
    if (!sl)
    {
#ifdef AMD_RETIRE_DIAGNOSTICS
        timing.event.outcome = "pending_skip";
#endif
        // Do not wait here. Execute/Submitted needs this lock to Notify HIP.
        // Every-frame waits after Execute in Submitted instead.
        if (++p->pendingSkips <= 3 || p->pendingSkips % 120 == 0)
        {
            p->Log("AMD skipped: no free neural slot; count=" + std::to_string(p->pendingSkips));
            p->LogSlotSnapshot("skip");
        }
        return nullptr;
    }
    const auto completion = sl->completion.load();
#ifdef AMD_RETIRE_DIAGNOSTICS
    const auto extraGpuBefore = p->fence->GetCompletedValue();
    if (extraGpuBefore < completion)
#else
    if (p->fence->GetCompletedValue() < completion)
#endif
    {
#ifdef AMD_RETIRE_DIAGNOSTICS
        timing.event.extraWaited = true;
        timing.event.extraGpuBefore = extraGpuBefore;
        timing.event.extraTarget = completion;
        const auto waitStart = RetirementDiagnostics::Clock();
#endif
        // Only wait for already submitted GPU work. Never wait here for an
        // unsubmitted list: its submission may depend on the recording thread.
        // A short scheduling delay used to bypass the effect for a whole frame.
        const auto start = GetTickCount64();
        while (p->fence->GetCompletedValue() < completion && GetTickCount64() - start < 16)
            Sleep(1);
#ifdef AMD_RETIRE_DIAGNOSTICS
        timing.event.extraWaitMs = p->diagnostics.Milliseconds(RetirementDiagnostics::Clock() - waitStart);
        const auto extraGpuAfter = p->fence->GetCompletedValue();
        timing.event.extraGpuAfter = extraGpuAfter;
        if (extraGpuAfter < completion)
#else
        if (p->fence->GetCompletedValue() < completion)
#endif
        {
#ifdef AMD_RETIRE_DIAGNOSTICS
            timing.event.outcome = "fence_skip";
#endif
            if (++p->fenceSkips)
                p->Log("AMD skipped: submitted GPU work not finished after 16 ms; count=" +
                       std::to_string(p->fenceSkips));
            return nullptr;
        }
        if (++p->fenceRecoveries <= 3 || p->fenceRecoveries % 120 == 0)
            p->Log("AMD continuity: prior GPU work retired after short wait; count=" +
                   std::to_string(p->fenceRecoveries));
    }
    bool timedOut = false;
    for (UINT i = 0; i < p->runtime.size(); ++i)
        if (auto h = p->runtime[i])
        {
            UINT count = static_cast<UINT>(
                InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&At<UINT>(h, p->L->timeoutCount)), 0, 0));
            if (count > p->observedTimeouts[i])
            {
                p->timeoutEvents += count - p->observedTimeouts[i];
                timedOut = true;
            }
            // The native count resets when staging is recreated.
            p->observedTimeouts[i] = count;
        }
    if (timedOut)
    {
        p->retryAfter = GetTickCount64() + 1000;
        p->resetAfterTimeout = true;
        p->Log("AMD timeout: native fallback may reuse the previous residual; retry in 1s with fresh history. Events=" +
               std::to_string(p->timeoutEvents));
    }
    if (GetTickCount64() < p->retryAfter)
        return nullptr;
    try
    {
        // Reject transient/dummy guides before any GPU commands or native jobs.
        // A later valid frame must be allowed to recover without restarting.
        const auto cd = f.colour->GetDesc();
        const UINT iw = f.width ? f.width : UINT(cd.Width), ih = f.height ? f.height : cd.Height;
        // Post-upscale the guides are the render grid's and are meant to be smaller than the
        // colour; only their layout has to hold. Pre-SR they must still cover it.
        const UINT gw = f.afterUpscale ? (f.guideWidth ? f.guideWidth : 1u) : iw;
        const UINT gh = f.afterUpscale ? (f.guideHeight ? f.guideHeight : 1u) : ih;
        for (auto guide : { f.motion, f.depth })
        {
            auto gd = guide->GetDesc();
            if (gd.Width < gw || gd.Height < gh || gd.SampleDesc.Count != 1 || gd.DepthOrArraySize != 1 ||
                gd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            {
                const std::string reason = "AMD neural: waiting for valid full-size guides; received " + Layout(guide);
                if (p->status != reason)
                    p->Log(reason);
                p->resetRequested = true;
                return nullptr;
            }
        }
        auto desc = f.colour->GetDesc();
        UINT w = f.width ? f.width : static_cast<UINT>(desc.Width), h = f.height ? f.height : desc.Height;
        if (p->frames == 0 || p->lastInputWidth != w || p->lastInputHeight != h)
        {
            p->Log("Input active=" + std::to_string(w) + "x" + std::to_string(h) + " colour=" + Layout(f.colour));
            p->Log("Input motion=" + Layout(f.motion) + " depth=" + Layout(f.depth));
        }
        if (!w || !h || w > desc.Width || h > desc.Height || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 ||
            desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            throw std::runtime_error("Unsupported active colour extent/layout");
        // Display-resolution vectors are resampled, never cropped as if they
        // belonged to the render-resolution pixel grid.
        const UINT guideW = f.guideWidth ? f.guideWidth : w;
        const UINT guideH = f.guideHeight ? f.guideHeight : h;
        for (auto guide : { f.motion, f.depth })
        {
            auto gd = guide->GetDesc();
            if (gd.Width < guideW || gd.Height < guideH || gd.SampleDesc.Count != 1 || gd.DepthOrArraySize != 1 ||
                gd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
                throw std::runtime_error(std::string("Unsupported AMD pre-SR ") +
                                         (guide == f.motion ? "motion: " : "depth: ") + Layout(guide));
        }
        const UINT inputW = w, inputH = h;
        const float scale = std::isfinite(cfg.modelScale) ? std::clamp(cfg.modelScale, .25f, 1.f) : 1.f;
        const UINT baseW = (std::max) (inputW, f.fixedWidth), baseH = (std::max) (inputH, f.fixedHeight);
        w = (std::min) (baseW, (std::max) (32u, UINT(std::lround(baseW * scale))));
        h = (std::min) (baseH, (std::max) (32u, UINT(std::lround(baseH * scale))));
        const bool scaled = w != inputW || h != inputH;
        // The guides' own active extent: the render grid post-upscale, the colour's otherwise.
        const UINT guideInW = f.guideWidth ? f.guideWidth : inputW;
        const UINT guideInH = f.guideHeight ? f.guideHeight : inputH;
        const UINT mvW = f.motionWidth ? f.motionWidth : guideInW, mvH = f.motionHeight ? f.motionHeight : guideInH;
        const auto depthDesc = f.depth->GetDesc();
        // The private AMD runtime already accepts typeless/depth-stencil guides
        // and stages only the colour-sized active region. Preparing another
        // crop/conversion on the game's command list duplicates that work and
        // invalidates some UE 4.26 command lists (Stellar Blade reports
        // E_INVALIDARG from Close). Pass the original guides through instead.
        // Post-upscale the depth is the render grid's and never matches the model's working size,
        // so it is always ratio-sampled across; pre-SR only a reduced model scale needs that.
        const bool convertDepth = scaled || f.afterUpscale;
        if (convertDepth && (depthDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            throw std::runtime_error("NR scale: depth is not shader readable; use 100%");
        if (convertDepth && DepthReadFormat(depthDesc.Format) == DXGI_FORMAT_UNKNOWN)
            throw std::runtime_error("NR scale: unsupported depth view; use 100%");
        const bool resampleMotion = mvW != w || mvH != h;
        const auto motionDesc = f.motion->GetDesc();
        if (resampleMotion && (f.motionWidth > motionDesc.Width || f.motionHeight > motionDesc.Height))
            throw std::runtime_error("Display motion extent exceeds its allocation");
        const DXGI_FORMAT motionRead = ReadFormat(motionDesc.Format);
        if (resampleMotion && motionRead != DXGI_FORMAT_R16G16_FLOAT && motionRead != DXGI_FORMAT_R32G32_FLOAT &&
            motionRead != DXGI_FORMAT_R16G16_SNORM && motionRead != DXGI_FORMAT_R16G16B16A16_FLOAT &&
            motionRead != DXGI_FORMAT_R32G32B32A32_FLOAT)
            throw std::runtime_error("Unsupported display motion format: " + Layout(f.motion));
        ID3D12Resource* exposureSource = nullptr;
        if (f.exposure)
        {
            const auto ed = f.exposure->GetDesc();
            if (ed.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && ed.SampleDesc.Count == 1 &&
                ed.DepthOrArraySize == 1 && !(ed.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
                (ed.Format == DXGI_FORMAT_R32_FLOAT || ed.Format == DXGI_FORMAT_R32G32_FLOAT ||
                 ed.Format == DXGI_FORMAT_R32G32B32A32_FLOAT || ed.Format == DXGI_FORMAT_R16_FLOAT ||
                 ed.Format == DXGI_FORMAT_R16G16B16A16_FLOAT))
                exposureSource = f.exposure;
        }
        if (f.motion->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
            throw std::runtime_error("Unsupported depth-stencil motion buffer: " + Layout(f.motion));
        p->activePasses = std::clamp(cfg.passes, 1u, 3u);
        // 0.8.0 is one module whatever the pass count, and runs the passes itself; pass 1 tells which runtime this is.
        if (p->activePasses > 1 && !p->runtime[0])
            p->InitPass(0);
        if (p->L && p->L->abi >= 2)
        {
            p->runtimePasses = p->L->passCount ? p->activePasses : 1u;
            p->activePasses = 1;
        }
        const UINT passKey = p->L && p->L->abi >= 2 ? p->runtimePasses : p->activePasses;
        bool passChange = p->lastPasses != passKey;
        p->lastPasses = passKey;
        for (UINT i = 0; i < p->activePasses; ++i)
            p->InitPass(i);
        const AmdLayout* L = p->L;
        if (!L)
            return nullptr;
        for (UINT i = 0; i < p->activePasses; ++i)
        {
            if (L->spinDraw && !Config::Instance()->AmdGraphicsUnsafe.value_or_default() &&
                p->gfxStartup[i].ShouldDefer(cfg.spinDraw != 0, gfx->armed, GetTickCount64()))
            {
                gfx->outcome = "graphics_startup_wait";
                return nullptr;
            }
            if (L->spinDraw && cfg.spinDraw && !gfx->armed && !p->gfxStartup[i].HasRecorded() &&
                !p->gfxStartupFallbackReported[i])
            {
                p->gfxStartupFallbackReported[i] = true;
                p->LogDiagnostic("AMD graphics startup: 2000ms grace expired; pass=" + std::to_string(i + 1) +
                                 " starting compute, reason=" + gfx->reason +
                                 "; later admission alone cannot create A graphics PSO");
            }
        }
        p->InitShader();
        const bool resize = p->width != w || p->height != h;
        const bool countChange = p->wantSlots != p->liveSlots;
        if (resize || countChange)
        {
            // Slot selection above only guarantees that the slot we picked is
            // idle, but this rebuild releases slot colours. Releasing or
            // rewriting a texture that an unfinished list still references is a
            // use-after-free, so defer to a frame where the buffers being
            // touched have retired. Nothing has been recorded into cmd yet at
            // this point, so returning here costs one frame of NR and nothing
            // else.
            //
            // A resize rewrites every buffer; a shrink releases the ones above
            // the new count. A grow creates only new buffers and touches none of
            // the live ones, so it needs no drain at all - which matters because
            // on a game that keeps every slot busy, a frame with nothing
            // outstanding can be a long wait, and the option would look stuck.
            if (resize || p->wantSlots < p->liveSlots)
            {
                const UINT first = resize ? 0u : p->wantSlots;
                for (UINT k = first; k < p->liveSlots; ++k)
                    if (p->slots[k].pending.load(std::memory_order_acquire))
                        return nullptr;
            }
            // Every live slot needs its own FP16 target, not just whichever one
            // is active on the frame the count changes. A slot with a null
            // colour is refused by the runtime outright: the call returns in well under a
            // microsecond, having logged nothing, advanced no counter and set no
            // state - which is exactly the refusal that took three rounds to pin
            // down. Buffers above the count are released instead, so asking for
            // three reserves memory for three: one of these is w*h*8 bytes, where
            // w,h is the RENDER extent (f.width is the DLSS render subrect, not
            // the output). That is 16.6 MB at a 1080p render and 29.5 MB at 1440p,
            // so a 4K output at DLSS Quality reserves ~29 MB per slot, not 66.
            D3D12_HEAP_PROPERTIES hp {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = w;
            rd.Height = h;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            for (UINT k = 0; k < Impl::kMaxSlots; ++k)
            {
                auto& slot = p->slots[k];
                if (k >= p->wantSlots)
                {
                    if (slot.colour)
                        slot.colour.Reset();
                    if (slot.exposureCopy)
                        slot.exposureCopy.Reset();
                    slot.decode.reset();
                    slot.encode.reset();
                    continue;
                }
                const auto desc = slot.colour ? slot.colour->GetDesc() : D3D12_RESOURCE_DESC {};
                if (!slot.colour || desc.Width != w || desc.Height != h)
                {
                    slot.colour.Reset();
                    Check(p->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                             IID_PPV_ARGS(&slot.colour)),
                          "Active FP16 texture");
                }
            }
            if (countChange)
                p->Log("AMD slots: " + std::to_string(p->wantSlots) + " (buffers " + std::to_string(p->wantSlots) +
                       ", cap " + std::to_string(Impl::kMaxSlots) + ")");
            p->liveSlots = p->wantSlots;
            p->width = w;
            p->height = h;
        }
        const bool convertEncoding = cfg.encoding == 2 || cfg.encoding == 3;
        if (convertEncoding)
        {
            if (!sl->decode)
                sl->decode = std::make_unique<ColorEncoding>(p->device.Get());
            if (!sl->encode)
                sl->encode = std::make_unique<ColorEncoding>(p->device.Get());
            f.colour = sl->decode->Run(cmd, f.colour, f.colourState, inputW, inputH, cfg.encoding, false);
            f.colourState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        auto prepareGuide = [&](ID3D12Resource* source, ComPtr<ID3D12Resource>& crop)
        {
            auto rd = source->GetDesc();
            if (rd.Width == w && rd.Height == h)
                return source;
            if (!crop || crop->GetDesc().Width != w || crop->GetDesc().Height != h ||
                crop->GetDesc().Format != rd.Format)
            {
                crop.Reset();
                rd.Width = w;
                rd.Height = h;
                rd.MipLevels = 1;
                rd.Flags = D3D12_RESOURCE_FLAG_NONE;
                D3D12_HEAP_PROPERTIES hp {};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                Check(p->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                         IID_PPV_ARGS(&crop)),
                      "Guide crop");
            }
            return crop.Get();
        };
        auto motion = f.motion;
        auto depth = f.depth;
        const auto& look = cfg.look;
        const bool lookOn = look.enabled && (look.mix > 0 || look.tone > 0 || look.inspect != 0);
        const bool applyLook = lookOn;
        auto createScratch = [&](ComPtr<ID3D12Resource>& resource, UINT sw, UINT sh, DXGI_FORMAT format)
        {
            if (resource && resource->GetDesc().Width == sw && resource->GetDesc().Height == sh)
                return;
            resource.Reset();
            D3D12_HEAP_PROPERTIES hp {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = sw;
            rd.Height = sh;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = format;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            Check(p->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&resource)),
                  "Guide scratch");
        };
        if (scaled)
        {
            createScratch(p->scaleBaseline, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
            // Shared by the slots in flight, so under dynamic resolution it keeps the largest frame's size.
            createScratch(p->scaleOutput, baseW, baseH, DXGI_FORMAT_R16G16B16A16_FLOAT);
        }
        if (convertDepth)
        {
            createScratch(p->depthCrop, w, h, DXGI_FORMAT_R32_FLOAT);
            depth = p->depthCrop.Get();
        }
        if (resampleMotion)
        {
            createScratch(p->motionCrop, w, h, DXGI_FORMAT_R16G16_FLOAT);
            motion = p->motionCrop.Get();
        }
        if (exposureSource)
            createScratch(sl->exposureCopy, 1, 1, DXGI_FORMAT_R32_FLOAT);
        if (applyLook)
        {
            createScratch(p->lookColour, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (!p->lookPipeline)
            {
                ComPtr<ID3DBlob> blob, error;
                auto hr = D3DCompile(AmdLookShader, sizeof(AmdLookShader), "AMD integrated appearance", nullptr,
                                     nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error);
                if (FAILED(hr) && error)
                    p->Log(static_cast<const char*>(error->GetBufferPointer()));
                Check(hr, "Appearance shader compile");
                D3D12_COMPUTE_PIPELINE_STATE_DESC ps {};
                ps.pRootSignature = p->root.Get();
                ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
                Check(p->device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&p->lookPipeline)),
                      "Appearance pipeline");
            }
        }
        // A dynamic resolution step keeps the model's extent and its history.
        const bool guideChange = (!f.fixedWidth && (p->lastInputWidth != inputW || p->lastInputHeight != inputH)) ||
                                 p->lastMotionWidth != f.motionWidth || p->lastMotionHeight != f.motionHeight ||
                                 p->hadExposure != (exposureSource != nullptr);
        if (resize || guideChange || p->frames == 0)
            p->Log("Guide mapping: motion=" + std::to_string(f.motionWidth) + "x" + std::to_string(f.motionHeight) +
                   " resampled=" + std::to_string(resampleMotion) +
                   " exposure=" + (exposureSource ? Layout(exposureSource) : "auto") +
                   " preExposure=" + std::to_string(f.preExposure) + " tone=" + std::to_string(cfg.tone));
        const UINT slotBase = p->activeSlot * Impl::kDescriptorsPerSlot;
        const UINT descriptorStride =
            p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = p->heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(slotBase) * descriptorStride;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = ReadFormat(f.colour->GetDesc().Format);
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        p->device->CreateShaderResourceView(f.colour, &srv, cpu);
        cpu.ptr += p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p->device->CreateUnorderedAccessView(sl->colour.Get(), nullptr, &uav, cpu);
        auto guideDescriptors = [&](UINT slot, ID3D12Resource* source, ID3D12Resource* target, DXGI_FORMAT format)
        {
            auto handle = p->heap->GetCPUDescriptorHandleForHeapStart();
            auto stride = p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            handle.ptr += slot * stride;
            auto guideSrv = srv;
            guideSrv.Format = ReadFormat(source->GetDesc().Format);
            p->device->CreateShaderResourceView(source, &guideSrv, handle);
            handle.ptr += stride;
            auto guideUav = uav;
            guideUav.Format = format;
            p->device->CreateUnorderedAccessView(target, nullptr, &guideUav, handle);
        };
        // Indices are absolute, so each caller adds the slot's block base.
        if (resampleMotion)
            guideDescriptors(slotBase + 4, f.motion, motion, DXGI_FORMAT_R16G16_FLOAT);
        if (exposureSource)
            guideDescriptors(slotBase + 6, exposureSource, sl->exposureCopy.Get(), DXGI_FORMAT_R32_FLOAT);
        if (applyLook)
            guideDescriptors(slotBase + 8, sl->colour.Get(), p->lookColour.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (convertDepth)
        {
            // Distinct descriptor slots: overwriting the colour descriptors here
            // would change the earlier dispatch when the GPU consumes the list.
            cpu.ptr += p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            srv.Format = DepthReadFormat(depthDesc.Format);
            p->device->CreateShaderResourceView(f.depth, &srv, cpu);
            cpu.ptr += p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            uav.Format = DXGI_FORMAT_R32_FLOAT;
            p->device->CreateUnorderedAccessView(depth, nullptr, &uav, cpu);
        }
        gfx->commandsRecorded = true;
        gfx->outcome = "recorded";
        Barrier(cmd, f.colour, f.colourState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, sl->colour.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(p->root.Get());
        cmd->SetPipelineState(p->pipeline.Get());
        auto heap = p->heap.Get();
        cmd->SetDescriptorHeaps(1, &heap);
        {
            auto t0 = p->heap->GetGPUDescriptorHandleForHeapStart();
            t0.ptr += static_cast<SIZE_T>(slotBase) * descriptorStride;
            cmd->SetComputeRootDescriptorTable(0, t0);
        }
        UINT dims[] { w, h, inputW, inputH };
        cmd->SetComputeRoot32BitConstants(1, 4, dims, 0);
        cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Barrier(cmd, sl->colour.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, f.colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.colourState);
        Barrier(cmd, f.motion, f.motionState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, f.depth, f.depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, exposureSource, f.exposureState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto copyGuide = [&](ID3D12Resource* source, ID3D12Resource* dest)
        {
            if (source == dest)
                return;
            Barrier(cmd, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cmd, dest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION from {}, to {};
            from.pResource = source;
            to.pResource = dest;
            D3D12_BOX box { 0, 0, 0, w, h, 1 };
            cmd->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
            Barrier(cmd, dest, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        };
        if (resampleMotion)
        {
            Barrier(cmd, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetPipelineState(p->motionPipeline.Get());
            auto table = p->heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += static_cast<SIZE_T>(slotBase + 4) * descriptorStride;
            cmd->SetComputeRootDescriptorTable(0, table);
            UINT motionDims[] { w, h, mvW, mvH };
            cmd->SetComputeRoot32BitConstants(1, 4, motionDims, 0);
            cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            Barrier(cmd, motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (convertDepth)
        {
            Barrier(cmd, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetPipelineState(p->depthPipeline.Get());
            // Source extent is the depth's own, which post-upscale is smaller than the colour.
            UINT depthDims[] { w, h, guideInW, guideInH };
            cmd->SetComputeRoot32BitConstants(1, 4, depthDims, 0);
            auto table = p->heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += static_cast<SIZE_T>(slotBase + 2) * descriptorStride;
            cmd->SetComputeRootDescriptorTable(0, table);
            cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            Barrier(cmd, depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        else
            copyGuide(f.depth, depth);
        if (exposureSource)
        {
            auto exposure = sl->exposureCopy.Get();
            Barrier(cmd, exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetPipelineState(p->exposurePipeline.Get());
            auto table = p->heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += static_cast<SIZE_T>(slotBase + 6) * descriptorStride;
            cmd->SetComputeRootDescriptorTable(0, table);
            struct
            {
                UINT w, h;
                float preExposure, exposureScale;
            } constants { 1, 1, std::isfinite(f.preExposure) && f.preExposure > 0 ? f.preExposure : 1,
                          std::isfinite(f.exposureScale) && f.exposureScale > 0 ? f.exposureScale : 1 };
            cmd->SetComputeRoot32BitConstants(1, 4, &constants, 0);
            cmd->Dispatch(1, 1, 1);
            Barrier(cmd, exposure, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        UINT accepted = 0;
        if (scaled)
            copyGuide(sl->colour.Get(), p->scaleBaseline.Get());
        const bool settingsChanged =
            cfg.encoding != p->lastSettings.encoding || cfg.toneChannels != p->lastSettings.toneChannels ||
            cfg.modelScale != p->lastSettings.modelScale || !p->haveSettings || cfg.tone != p->lastSettings.tone ||
            cfg.structure != p->lastSettings.structure || cfg.skin != p->lastSettings.skin ||
            cfg.everyFrame != p->lastSettings.everyFrame || (L->scale && cfg.strength != p->lastSettings.strength) ||
            (L->style && cfg.style != p->lastSettings.style) ||
            (L->toneCurve && cfg.toneCurve != p->lastSettings.toneCurve) ||
            (L->toneLift && cfg.toneLift != p->lastSettings.toneLift) ||
            (L->useGameExposure && cfg.gameExposure != p->lastSettings.gameExposure);
        const bool explicitReset = p->resetRequested.exchange(false);
        const bool gap = p->lastSubmitted && GetTickCount64() - p->lastSubmitted > 250;
        if (f.reset || resize || guideChange || passChange || p->resetAfterTimeout || settingsChanged ||
            explicitReset || gap)
        {
            p->Log("AMD history reset: frame=" + std::to_string(p->frames) + " game=" + std::to_string(f.reset) +
                   " resize=" + std::to_string(resize) + " guides=" + std::to_string(guideChange) +
                   " passes=" + std::to_string(passChange) + " timeout=" + std::to_string(p->resetAfterTimeout) +
                   " settings=" + std::to_string(settingsChanged) + " explicit=" + std::to_string(explicitReset) +
                   " gap=" + std::to_string(gap));
        }
        for (UINT i = 0; i < p->activePasses; ++i)
        {
            auto r = p->runtime[i];
            if (L->spinDraw)
            {
                // New wait only on DIRECT lists. Safe path requires armed restore;
                // AmdGraphicsUnsafe skips that (dirty insert, no complete restore).
                const bool unsafe = Config::Instance()->AmdGraphicsUnsafe.value_or_default();
                int want = Config::Instance()->AmdGraphicsWait.value_or_default() ? 1 : 0;
                if (want && listType != D3D12_COMMAND_LIST_TYPE_DIRECT)
                    want = 0;
                else if (want && !gfx->armed && !unsafe)
                    want = 0;
                At<int>(r, L->spinDraw) = want;
            }
            // 0x8d9bd is Temporal in the original 0.2.17. Default on (skip-frame path).
            // Every-frame mode matches author 0.3: skip history inputs, do not
            // clear history-valid (0x8d018) each frame.
            At<uint8_t>(r, L->temporal) = cfg.everyFrame ? 0 : 1;
            if (cfg.fixedSeed && L->seedCounter && L->seedSelfCheck)
            {
                if (At<int>(r, L->seedCounter) > 0)
                    At<int>(r, L->seedSelfCheck) = -1;
                At<int>(r, L->seedCounter) = 0;
            }
            // The history-valid flag and the current borrowed history view (engine
            // +0x120/+0x118 in 0.3.1, +0x150/+0x148 in 0.4.x). Clear only at a
            // quiescent frame boundary.
            if (f.reset || resize || guideChange || passChange || p->resetAfterTimeout || settingsChanged ||
                explicitReset || gap)
            {
                At<uint8_t>(r, L->historyValid) = 0;
                At<void*>(r, L->historyView) = nullptr;
            }
            At<UINT>(r, L->depthInverted) = f.depthInverted;
            At<uint8_t>(r, L->explicitDepth) = 1; // explicit depth convention, no heuristic
            At<float>(r, L->tone) = i == 0 ? cfg.tone : 0;
            At<float>(r, L->structure) = cfg.structure;
            At<float>(r, L->skin) = cfg.skin;
            if (L->scale)
                At<float>(r, L->scale) = cfg.strength * 4.f / 128.f;
            if (L->quality)
                At<uint8_t>(r, L->quality) = cfg.fast ? 1 : 0;
            if (L->style)
                At<int>(r, L->style) = int(cfg.style);
            if (L->toneCurve)
                At<int>(r, L->toneCurve) = int(cfg.toneCurve);
            // Both reach the network through the job the runtime builds on each Record, as the tone curve does.
            if (L->toneLift)
                At<float>(r, L->toneLift) = cfg.toneLift;
            if (L->useGameExposure)
                At<uint8_t>(r, L->useGameExposure) = cfg.gameExposure ? 1 : 0;
            At<UINT>(r, L->toneChannels) = cfg.toneChannels ? 1u : 0u;
            At<UINT>(r, L->charMask) = 1; // Enable native semantic character-mask channel.
            if (L->passCount)
            {
                // The runtime's own passes take what one module per pass took: the same style, structure and skin,
                // the tone on the first pass only. A new count rebuilds its staging, as its own settings refresh
                // does when Passes changes.
                const int count = static_cast<int>(p->runtimePasses);
                if (At<int>(r, L->passCount) != count)
                {
                    At<int>(r, L->passCount) = count;
                    if (L->recreate)
                        At<uint8_t>(r, L->recreate) = 1;
                    p->Log("AMD runtime passes: " + std::to_string(count) + " (inside the runtime's job)");
                }
                for (UINT k = 0; k < 3; ++k)
                {
                    At<int>(r, L->passStyle + 4 * k) = int(cfg.style);
                    At<float>(r, L->passStructure + 4 * k) = cfg.structure;
                    At<float>(r, L->passSkin + 4 * k) = cfg.skin;
                    At<float>(r, L->passTone + 4 * k) = k == 0 ? cfg.tone : 0;
                }
            }
            // The old shader ceiling expired at high render resolutions even
            // when inference finished well inside the original runtime's watchdog.
            // Scale the spin allowance with pixels, but retain a hard ceiling
            // in the private shader if notification is lost. This is an
            // iteration allowance, not a portable millisecond conversion.
            At<UINT>(r, L->watchdog) =
                static_cast<UINT>(std::clamp<UINT64>(262144 + (UINT64(w) * h + 1) / 2, 262144, 2097152));
            Packet packet {};
            packet.list = cmd;
            packet.colour = sl->colour.Get();
            packet.colourState = 4;
            packet.motion = motion;
            packet.motionState = 4;
            packet.depth = depth;
            packet.depthState = 4;
            packet.exposure = exposureSource ? sl->exposureCopy.Get() : nullptr;
            packet.exposureState = 4;
            packet.scaleX = f.motionScaleX * (resampleMotion ? float(w) / mvW : 1.0f);
            packet.scaleY = f.motionScaleY * (resampleMotion ? float(h) / mvH : 1.0f);
            packet.renderWidth = w;
            packet.renderHeight = h;
            // Snapshot everything that can explain a refusal, and time the call.
            // jobId is diagnostic; the pending list is the publication contract.
            // Notify consumes it, not the worker.
            const void* pendingBefore = At<ID3D12CommandList*>(r, L->pendingList);
            const unsigned recreateBefore = L->recreate ? At<volatile uint8_t>(r, L->recreate) : 0;
            const UINT jobBefore = At<UINT>(r, L->jobId);
            const UINT doneBefore = At<UINT>(r, L->jobDone);
            // 0.3.1 and 0.4.x use a blocking mutex. +0x4c is its ownership/recursion
            // count, not a waiter count or a measure of worker saturation.
            const UINT lockBefore = L->recordLock ? At<UINT>(r, L->recordLock + 0x4c) : 0;
            const int gate4c = L->gate4c ? At<int>(r, L->gate4c) : 0;
            const int gate68 = L->gate68 ? At<int>(r, L->gate68) : 0;
            const UINT count78 = L->counter78 ? At<UINT>(r, L->counter78) : 0;
            // Written BEFORE the call, so a process that dies inside Record
            // leaves this as the last line - which is itself the answer.
            // Keep startup samples and a sparse heartbeat; refusals retain
            // their complete diagnostic line below.
            if (++p->recordCalls <= 3 || p->recordCalls % 300 == 0)
                p->Log("AMD Record enter: n=" + std::to_string(p->recordCalls) +
                       " jobBefore=" + std::to_string(jobBefore) + " doneBefore=" + std::to_string(doneBefore) +
                       " listBefore=" + std::to_string(reinterpret_cast<uintptr_t>(pendingBefore)) +
                       " recreate=" + std::to_string(recreateBefore) + " lockCount=" + std::to_string(lockBefore) +
                       " gate4c=" + std::to_string(gate4c) + " gate68=" + std::to_string(gate68) +
                       " count78=" + std::to_string(count78));
            const int actualSpin = L->spinDraw ? At<int>(r, L->spinDraw) : 0;
            const bool aPsoBefore = L->graphicsPso && At<void*>(r, L->graphicsPso) != nullptr;
            const bool predBefore = L->predicateReady && At<uint8_t>(r, L->predicateReady) == 1;
            const auto nativeBase = reinterpret_cast<uintptr_t>(r);
            GraphicsSnap::ScopedNativeDrawObservation draws(
                listId, L->graphicsWaitBegin ? nativeBase + L->graphicsWaitBegin : 0,
                L->graphicsWaitEnd ? nativeBase + L->graphicsWaitEnd : 0,
                { L->waitDispatchInit ? nativeBase + L->waitDispatchInit : 0,
                  L->waitDispatchFallback ? nativeBase + L->waitDispatchFallback : 0,
                  L->waitDispatchSlices ? nativeBase + L->waitDispatchSlices : 0,
                  L->waitDispatchFinish ? nativeBase + L->waitDispatchFinish : 0 });
            const auto callStart = std::chrono::steady_clock::now();
            if (L->abi >= 2)
            {
                // Every input is in NON_PIXEL_SHADER_RESOURCE here (the barriers above), the state PacketV2 now
                // states as itself.
                constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                PacketV2 v2 {};
                v2.list = cmd;
                v2.listApi = 1;
                v2.listTag = 3;
                v2.colour = DescribeV2(r, L, packet.colour, read);
                v2.motion = DescribeV2(r, L, packet.motion, read);
                v2.depth = DescribeV2(r, L, packet.depth, read);
                v2.exposure = DescribeV2(r, L, packet.exposure, read);
                v2.scaleX = packet.scaleX;
                v2.scaleY = packet.scaleY;
                v2.renderWidth = packet.renderWidth;
                v2.renderHeight = packet.renderHeight;
                reinterpret_cast<RecordV2Fn>(nativeBase + L->record)(&v2);
            }
            else
                reinterpret_cast<RecordFn>(nativeBase + L->record)(&packet);
            const auto callMicros =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - callStart)
                    .count();
            p->gfxStartup[i].MarkRecorded();
            const bool aPsoAfter = L->graphicsPso && At<void*>(r, L->graphicsPso) != nullptr;
            const bool predAfter = L->predicateReady && At<uint8_t>(r, L->predicateReady) == 1;
            p->gfxDrawCalls += draws.observation.count;
            if (actualSpin)
                ++p->gfxSpin1Calls;
            else
                ++p->gfxSpin0Calls;
            const int nativeMode = draws.observation.count                                                          ? 0
                                   : draws.observation.dispatchSlices || draws.observation.dispatchFallback         ? 1
                                   : gfx->requested && (!waitCoverage.drawCovered || !waitCoverage.dispatchCovered) ? 2
                                   : draws.observation.dispatchWait                                                 ? 3
                                                                                                                    : 4;
            static constexpr const char* nativeModes[] = { "graphics_recorded", "compute_wait_recorded",
                                                           "observation_incomplete", "wait_dispatch_only",
                                                           "no_wait_calls_observed" };
            const UINT64 nativeSample = ++p->gfxNativeSamples[i];
            const UINT64 logNow = GetTickCount64();
            const bool requestedChanged = p->gfxLastRequested[i] != static_cast<int>(gfx->requested);
            const bool modeChanged =
                p->gfxLastSpin[i] != actualSpin || p->gfxLastPso[i] != aPsoAfter || p->gfxLastMode[i] != nativeMode;
            // User requests always produce one line per active pass. Automatic
            // fallback chatter is limited per pass, without a lifetime quota.
            if (nativeSample <= 3 || nativeSample % 300 == 0 || requestedChanged ||
                (modeChanged && logNow - p->gfxModeLogAt[i] >= 1000))
            {
                p->LogDiagnostic(
                    "AMD graphics native: pass=" + std::to_string(i + 1) + " record=" + std::to_string(p->recordCalls) +
                    " requested=" + std::to_string(gfx->requested) + " SpinDraw=" + std::to_string(actualSpin) +
                    " aGraphicsPsoBefore=" + std::to_string(aPsoBefore) +
                    " aGraphicsPsoAfter=" + std::to_string(aPsoAfter) +
                    " predReadyBefore=" + std::to_string(predBefore) + " predReadyAfter=" + std::to_string(predAfter) +
                    " drawObserved=" + std::to_string(draws.observation.count) +
                    " dispatchWait=" + std::to_string(draws.observation.dispatchWait) +
                    " dispatchInit=" + std::to_string(draws.observation.dispatchInit) +
                    " dispatchFallback=" + std::to_string(draws.observation.dispatchFallback) +
                    " dispatchSlices=" + std::to_string(draws.observation.dispatchSlices) + " dispatchFinish=" +
                    std::to_string(draws.observation.dispatchFinish) + " mode=" + nativeModes[nativeMode]);
                p->gfxLastRequested[i] = static_cast<int>(gfx->requested);
                p->gfxLastSpin[i] = actualSpin;
                p->gfxLastPso[i] = aPsoAfter;
                p->gfxLastMode[i] = nativeMode;
                p->gfxModeLogAt[i] = logNow;
            }
            // Address/filter diagnostics are emitted once per category and
            // pass, keeping failed observation actionable without log floods.
            std::string detailKinds;
            const auto addDetail = [&](bool present, UINT bit, const char* name)
            {
                if (present && !(p->gfxDetailSeen[i] & bit))
                {
                    p->gfxDetailSeen[i] |= bit;
                    if (!detailKinds.empty())
                        detailKinds += ',';
                    detailKinds += name;
                }
            };
            addDetail(nativeSample == 1, 1u, "first_sample");
            addDetail(gfx->requested, 64u, "first_graphics_request");
            addDetail(gfx->requested && !waitCoverage.drawCovered, 2u, "draw_uncovered");
            addDetail(gfx->requested && !waitCoverage.dispatchCovered, 4u, "dispatch_uncovered");
            addDetail(draws.observation.hookHits && !draws.observation.count, 8u, "draw_filtered");
            addDetail(draws.observation.dispatchHook && !draws.observation.dispatchWait, 16u, "dispatch_filtered");
            addDetail(actualSpin && aPsoBefore && predBefore && !draws.observation.count, 32u, "graphics_not_observed");
            if (!detailKinds.empty())
                p->LogDiagnostic(
                    "AMD graphics native detail: pass=" + std::to_string(i + 1) +
                    " record=" + std::to_string(p->recordCalls) + " categories=" + detailKinds +
                    " requested=" + std::to_string(gfx->requested) + " SpinDraw=" + std::to_string(actualSpin) +
                    " drawHook=" + std::to_string(draws.observation.hookHits) +
                    " drawSameList=" + std::to_string(draws.observation.sameList) +
                    " drawCaller=" + std::to_string(draws.observation.callerMatched) +
                    " drawObserved=" + std::to_string(draws.observation.count) +
                    " drawMismatch=" + std::to_string(draws.observation.mismatchReturn) +
                    " drawMismatchList=" + std::to_string(draws.observation.mismatchList) + " drawTarget=" +
                    std::to_string(waitCoverage.drawTarget) + " earlyDrawTarget=" + std::to_string(earlyDrawTarget) +
                    " drawCovered=" + std::to_string(waitCoverage.drawCovered) +
                    " drawAttachError=" + std::to_string(waitCoverage.drawError) +
                    " dispatchTarget=" + std::to_string(waitCoverage.dispatchTarget) +
                    " dispatchCovered=" + std::to_string(waitCoverage.dispatchCovered) +
                    " dispatchAttachError=" + std::to_string(waitCoverage.dispatchError) +
                    " dispatchHook=" + std::to_string(draws.observation.dispatchHook) +
                    " dispatchSameList=" + std::to_string(draws.observation.dispatchSameList) +
                    " dispatchWait=" + std::to_string(draws.observation.dispatchWait) +
                    " dispatchInit=" + std::to_string(draws.observation.dispatchInit) +
                    " dispatchFallback=" + std::to_string(draws.observation.dispatchFallback) +
                    " dispatchSlices=" + std::to_string(draws.observation.dispatchSlices) +
                    " dispatchFinish=" + std::to_string(draws.observation.dispatchFinish) +
                    " dispatchMismatch=" + std::to_string(draws.observation.dispatchMismatchReturn) +
                    " waitRange=" + std::to_string(nativeBase + (L->graphicsWaitBegin ? L->graphicsWaitBegin : 0)) +
                    "-" + std::to_string(nativeBase + (L->graphicsWaitEnd ? L->graphicsWaitEnd : 0)) +
                    " mode=" + nativeModes[nativeMode]);
            sl->jobs[i] = At<UINT>(r, L->jobId);
            // Staging recreation resets the native job counter. After a resize,
            // job 1 can follow job 1, so counter equality does not mean rejection.
            // The native pending-list pointer is the actual submission contract.
            const bool recorded = At<ID3D12CommandList*>(r, L->pendingList) == cmd;
            if (recorded && L->graphicsPso)
                p->gfxRestart.OnRecorded(i, aPsoAfter);
            if (recorded)
            {
                // Runtime 0.2.17 owns the abort word in its HIP flags buffer.
                // Native staging rebuilds join workers and clear that buffer.
                ++accepted;
            }
            if (At<uint8_t>(r, L->nativeFailure))
            {
                p->failed = true;
                p->Log("AMD pass native failure: " + std::to_string(i + 1) + " job=" + std::to_string(sl->jobs[i]));
                break;
            }
            if (!recorded)
            {
                // No matching pending list means we must not claim publication.
                // Counters alone cannot establish why Record declined.
                p->Log("AMD Record refused: jobBefore=" + std::to_string(jobBefore) + " jobAfter=" +
                       std::to_string(At<UINT>(r, L->jobId)) + " doneBefore=" + std::to_string(doneBefore) +
                       " listBefore=" + std::to_string(reinterpret_cast<uintptr_t>(pendingBefore)) + " listAfter=" +
                       std::to_string(reinterpret_cast<uintptr_t>(At<ID3D12CommandList*>(r, L->pendingList))) +
                       " recreate_before=" + std::to_string(recreateBefore) +
                       " lockCount=" + std::to_string(lockBefore) + " gate4c=" + std::to_string(gate4c) +
                       " gate68=" + std::to_string(gate68) + " count78=" + std::to_string(count78) +
                       " count78_after=" + std::to_string(L->counter78 ? At<UINT>(r, L->counter78) : 0) + " call_us=" +
                       std::to_string(callMicros) + " slot=" + std::to_string(static_cast<UINT>(sl - &p->slots[0])) +
                       " busy=" + std::to_string(p->AnySlotBusy() ? 1 : 0));
                break;
            }
            // Healthy calls are logged sparsely so the log still shows whether
            // the runtime ever blocks, which is what decides if admission control is viable.
            if (p->recordCalls <= 3 || p->recordCalls % 300 == 0)
                p->Log("AMD Record ok: n=" + std::to_string(p->recordCalls) +
                       " jobAfter=" + std::to_string(At<UINT>(r, L->jobId)) + " call_us=" + std::to_string(callMicros) +
                       " slot=" + std::to_string(static_cast<UINT>(sl - &p->slots[0])));
        }
        Barrier(cmd, f.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.motionState);
        Barrier(cmd, f.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.depthState);
        Barrier(cmd, exposureSource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.exposureState);
        p->activePasses = accepted;
        // Bind this slot's notify/retire pass count to what was actually recorded.
        sl->passCount = accepted;
        sl->afterUpscale = f.afterUpscale;
#ifdef AMD_RETIRE_DIAGNOSTICS
        timing.event.accepted = accepted;
#endif
        if (accepted)
        {
            ++p->frames;
        }
        // Even accepted == 0 has B's copy/conversion/barrier commands recorded.
        // Track that list until its D3D fence completes before reusing resources.
        sl->submission.Record(GetTickCount64());
        sl->pending.store(cmd, std::memory_order_release);
        if (p->failed)
            return nullptr;
        if (applyLook)
        {
            auto bounded = [](float v, float lo, float hi, float fallback)
            { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; };
            struct Constants
            {
                UINT w, h, appearance, inspect;
                float mix, material, shape, lighting, skin, softness, specular, rollOff;
                float colour, shadow, halo, flat, tone, exposureEV, contrast, saturation;
                float compression, preExposure;
                UINT detectSkin;
            } c { w,
                  h,
                  (std::min) (look.appearance, 3u),
                  lookOn ? (std::min) (look.inspect, 3u) : 0u,
                  lookOn ? bounded(look.mix, 0, 1, 1) : 0.f,
                  bounded(look.materialDetail, 0, 2, 1.15f),
                  bounded(look.shapeDefinition, 0, 2, 1.2f),
                  bounded(look.localLighting, 0, 2, 1.15f),
                  bounded(look.skinDetail, 0, 2, 1.1f),
                  bounded(look.skinSoftness, 0, 1, .486f),
                  bounded(look.specularControl, 0, 1, .58f),
                  bounded(look.highlightRollOff, 0, 1, .9f),
                  bounded(look.colourSeparation, 0, 1, 0),
                  bounded(look.shadowDepth, 0, 1, .2f),
                  bounded(look.antiHalo, 0, 1, .901f),
                  bounded(look.flatAreaProtection, 0, 1, 0),
                  lookOn ? bounded(look.tone, 0, 1, 0) : 0.f,
                  bounded(look.exposureEV, -3, 3, 1),
                  bounded(look.contrast, .5f, 1.5f, 1),
                  bounded(look.saturation, 0, 2, 1),
                  bounded(look.highlightCompression, 0, 1, 0),
                  std::isfinite(f.preExposure) && f.preExposure > 0 ? f.preExposure : 1,
                  look.detectSkin };
            static_assert(sizeof(Constants) == 23 * sizeof(UINT));
            Barrier(cmd, p->lookColour.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetComputeRootSignature(p->root.Get());
            cmd->SetPipelineState(p->lookPipeline.Get());
            cmd->SetDescriptorHeaps(1, &heap);
            auto table = p->heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += static_cast<SIZE_T>(slotBase + 8) * descriptorStride;
            cmd->SetComputeRootDescriptorTable(0, table);
            cmd->SetComputeRoot32BitConstants(1, 23, &c, 0);
            cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            Barrier(cmd, p->lookColour.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        auto finalColour = applyLook ? p->lookColour.Get() : sl->colour.Get();
        if (cfg.rtgi.enabled && !p->rtgiFailed)
        {
            try
            {
                if (!p->rtgi)
                    p->rtgi = std::make_unique<RtgiNative>(p->device.Get(), p->directory / L"experimental_lighting");
                Frame rtgiFrame = f;
                rtgiFrame.colour = finalColour;
                rtgiFrame.width = w;
                rtgiFrame.height = h;
                rtgiFrame.depth = depth;
                rtgiFrame.depthState = scaled ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : f.depthState;
                rtgiFrame.colourState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                rtgiFrame.motion = motion;
                rtgiFrame.motionState =
                    motion == f.motion ? f.motionState : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                rtgiFrame.motionScaleX *= resampleMotion ? float(w) / mvW : 1.0f;
                rtgiFrame.motionScaleY *= resampleMotion ? float(h) / mvH : 1.0f;
                rtgiFrame.reset |= resize || guideChange || passChange || p->resetAfterTimeout || explicitReset || gap;
                finalColour = p->rtgi->Record(cmd, rtgiFrame, cfg.rtgi);
                p->rtgiStatus = "Experimental effect active";
            }
            catch (const std::exception& e)
            {
                // Retain resources referenced by any already recorded commands.
                // A failed optional effect must not disable the neural backend.
                p->rtgiFailed = true;
                p->rtgiStatus = e.what();
                p->Log(p->rtgiStatus);
            }
        }
        else if (!cfg.rtgi.enabled)
        {
            if (p->rtgi)
                p->rtgi->ResetHistory();
            p->rtgiStatus.clear();
        }
        if (scaled)
        {
            guideDescriptors(slotBase + 10, f.colour, p->scaleOutput.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            auto handle = p->heap->GetCPUDescriptorHandleForHeapStart();
            auto stride = p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            handle.ptr += static_cast<SIZE_T>(slotBase + 12) * stride;
            auto v = srv;
            v.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            p->device->CreateShaderResourceView(p->scaleBaseline.Get(), &v, handle);
            handle.ptr += stride;
            p->device->CreateShaderResourceView(finalColour, &v, handle);
            Barrier(cmd, f.colour, f.colourState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, p->scaleOutput.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetComputeRootSignature(p->root.Get());
            cmd->SetDescriptorHeaps(1, &heap);
            cmd->SetPipelineState(p->resolvePipeline.Get());
            auto table = heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += static_cast<SIZE_T>(slotBase + 10) * stride;
            cmd->SetComputeRootDescriptorTable(0, table);
            // Root table 2 names the (baseline, finalColour) pair written just above at
            // slotBase+12/+13, so it is two descriptors along from table 0. This used to
            // add (slotBase+2) on top of the already-advanced handle, which resolves to
            // 2*slotBase+12: correct for slot 0 and wrong for every other slot - slot 1
            // read another slot's pair, and from slot 3 it pointed past the end of the
            // kDescriptors heap outright. Only reachable on the `scaled` path, which is
            // why it survived: every measurement so far ran at NR resolution 100%.
            table.ptr += static_cast<SIZE_T>(2) * stride;
            cmd->SetComputeRootDescriptorTable(2, table);
            UINT rc[] { inputW, inputH, w, h };
            cmd->SetComputeRoot32BitConstants(1, 4, rc, 0);
            cmd->Dispatch((inputW + 7) / 8, (inputH + 7) / 8, 1);
            Barrier(cmd, p->scaleOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, f.colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.colourState);
            finalColour = p->scaleOutput.Get();
        }
        p->resetAfterTimeout = false;
        p->lastSettings = cfg;
        p->haveSettings = true;
        p->lastInputWidth = inputW;
        p->lastInputHeight = inputH;
        p->lastMotionWidth = f.motionWidth;
        p->lastMotionHeight = f.motionHeight;
        p->hadExposure = exposureSource != nullptr;
        if (p->frames <= 120 || resize)
            p->Log("Recorded pre-SR " + std::to_string(w) + "x" + std::to_string(h) +
                   " passes=" + std::to_string(p->activePasses));
        if (convertEncoding)
            finalColour = sl->encode->Run(cmd, finalColour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, inputW,
                                          inputH, cfg.encoding, true);
#ifdef AMD_RETIRE_DIAGNOSTICS
        timing.event.outcome = "recorded";
#endif
        return finalColour;
    }
    catch (const std::exception& e)
    {
        p->failed = true;
        p->Log(e.what());
        return nullptr;
    }
}
int Backend::PendingListIndex(UINT count, ID3D12CommandList* const* lists) const
{
    if (!lists)
        return -1;
    for (size_t s = 0; s < p->slots.size(); ++s)
    {
        auto pending = p->slots[s].pending.load(std::memory_order_acquire);
        if (!pending)
            continue;
        for (UINT i = 0; i < count; ++i)
            if (lists[i] == pending)
                return static_cast<int>(i);
    }
    return NgxPendingListIndex(count, lists);
}
void Backend::TraceBoundary(const std::string& reason)
{
    std::lock_guard guard(p->lock);
    p->TraceBoundary(reason);
}
void Backend::Submitting(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists)
{
    if (!queue)
        return;
    // Atomic-only prefilter. Do not take p->lock here: Record may already hold it.
    std::array<UINT, Impl::kMaxSlots> candSlots {};
    std::array<ID3D12CommandList*, Impl::kMaxSlots> candPending {};
    const UINT cands = p->FindPendingCandidates(n, lists, candSlots, candPending);
    if (!cands)
        return;
    std::lock_guard guard(p->lock);
    const AmdLayout* L = p->L;
    if (!L)
        return;
    // Choose under the lock. submitted is a plain bool, and a newer slot can
    // hold the same list pointer as an older one that is already submitted.
    UINT slot = 0;
    ID3D12CommandList* pending = nullptr;
    if (!p->PickUnsubmitted(candSlots, candPending, cands, slot, pending))
        return;
    auto& sl = p->slots[slot];
    if (p->frames <= 120)
        p->Log("Neural submission: lists=" + std::to_string(n) +
               " queueType=" + std::to_string(static_cast<UINT>(queue->GetDesc().Type)));
    // Match the recorded list, not the swapchain's presentation queue. FG can
    // replace the latter, and the renderer may also migrate between queues.
    // Preserve a single ordered fence timeline across queue migration. A high
    // signal on a different queue is otherwise no proof that older work ended.
    // This is a GPU dependency inserted BEFORE Execute, not a CPU/HIP wait.
    if (queue != p->queue.Get())
    {
        if (p->serial && FAILED(queue->Wait(p->fence.Get(), p->serial)))
        {
            p->failed = true;
            p->completionOrderValid = false;
            p->Log("Render queue migration Wait failed; stopping NR and retaining outstanding resources");
        }
        // p->queue keeps the reference; the slot is borrowed (0.8.0's host overwrites it raw at every present).
        for (auto h : p->runtime)
            if (h)
                At<ID3D12CommandQueue*>(h, L->queue) = queue;
        p->queue = queue;
        p->Log("Render submission queue changed; dependency on completion=" + std::to_string(p->serial));
    }
    sl.submissionQueue = queue;
    // Bind the real queue here, but only wake HIP after ExecuteCommandLists.
    // A capture-wait kernel launched before D3D12 submission can occupy the GPU
    // while the capture it depends on is still queued on the CPU.
}
void Backend::Submitted(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists)
{
    // Every caller must pair Submitting -> real Execute -> Submitted. A queue
    // dependency inserted here would be too late to protect this list's work.
    if (!queue)
        return;
    // Same lock-after-match order as Submitting. Do not lock first: Record can
    // hold p->lock while this thread re-enters via the Execute hook.
    std::array<UINT, Impl::kMaxSlots> candSlots {};
    std::array<ID3D12CommandList*, Impl::kMaxSlots> candPending {};
    const UINT cands = p->FindPendingCandidates(n, lists, candSlots, candPending);
    if (!cands)
    {
        NgxSubmitted(queue, n, lists);
        return;
    }
    std::lock_guard guard(p->lock);
    const AmdLayout* L = p->L;
    if (!L)
    {
        NgxSubmitted(queue, n, lists);
        return;
    }
    // A reused command-list pointer can also match an older submitted slot;
    // only the current unsubmitted generation may be published here.
    UINT slot = 0;
    ID3D12CommandList* pending = nullptr;
    if (!p->PickUnsubmitted(candSlots, candPending, cands, slot, pending))
    {
        NgxSubmitted(queue, n, lists);
        return;
    }
    auto& sl = p->slots[slot];
    if (sl.submissionQueue.Get() != queue)
    {
        p->failed = true;
        p->completionOrderValid = false;
        p->Log("AMD submission was not prepared on this queue; retaining resources");
    }
    // Notify uses this slot's recorded pass count. 0 = the runtime refused (no HIP job).
    const UINT passCount = (sl.passCount == Impl::Slot::kPassUnset) ? 0u : sl.passCount;
    bool ngxSrMatched = false, ngxRrMatched = false;
    const bool hasNgxOnList =
        p->runtime[0] && MatchNgxPendingLists(p->runtime[0], L, n, lists, ngxSrMatched, ngxRrMatched) != nullptr;
    if (passCount == 0)
    {
        if (hasNgxOnList)
            NotifyNgxLists(p->runtime[0], L, queue, n, lists, false, [this](const std::string& s) { p->Log(s); });
    }
    else if (hasNgxOnList && sl.afterUpscale)
    {
        // Post-upscale: DLSS-SR / DLSS-RR is recorded ahead of NR on the same D3D12 command list.
        // Wake SR/RR and wait for its HIP worker to finish before waking NR; otherwise NR's GPU
        // capture-wait kernel (0x1544c0) occupies the HIP stream while NR's D3D12 capture sits
        // behind SR/RR's D3D12 spin-wait.
        NotifyNgxLists(p->runtime[0], L, queue, n, lists, true, [this](const std::string& s) { p->Log(s); });
    }
    else if (p->runtime[0] && L->ngxNotifyChain)
    {
        // Ensure no earlier frame's SR/RR job is still occupying the HIP stream before waking NR.
        WaitNgxInFlight(p->runtime[0], L, true, true, [this](const std::string& s) { p->Log(s); });
    }
    const auto notifyPass = [&](UINT i, bool suppressNgxChain)
    {
        auto h = p->runtime[i];
        const bool matched = At<ID3D12CommandList*>(h, L->pendingList) == pending;
        if (matched)
        {
            NotifyFn savedChain = nullptr;
            if (suppressNgxChain && L->ngxNotifyChain)
            {
                savedChain = At<NotifyFn volatile>(h, L->ngxNotifyChain);
                At<NotifyFn volatile>(h, L->ngxNotifyChain) = nullptr;
            }
            reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(h) + L->notify)(queue, n, lists);
            if (suppressNgxChain && L->ngxNotifyChain)
                At<NotifyFn volatile>(h, L->ngxNotifyChain) = savedChain;
        }
        if (!matched || At<ID3D12CommandList*>(h, L->pendingList) != nullptr)
        {
            p->failed = true;
            p->completionOrderValid = false;
            p->Log("AMD Notify did not consume the expected pending list; retaining resources, pass=" +
                   std::to_string(i + 1));
            return false;
        }
        return true;
    };
    const auto waitNrPass = [&](UINT i)
    {
        auto h = p->runtime[i];
        auto start = GetTickCount64();
        unsigned spins = 0;
        while (static_cast<UINT>(InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&At<UINT>(h, L->jobDone)),
                                                            0, 0)) < sl.jobs[i])
        {
            if (GetTickCount64() - start > 5000)
            {
                p->failed = true;
                p->Log("HIP completion timeout pass " + std::to_string(i + 1));
                break;
            }
            if (++spins < 2000)
                YieldProcessor();
            else if (spins < 10000)
                SwitchToThread();
            else
                Sleep(1);
        }
    };
    if (passCount == 1)
    {
        const bool deferNgxAfterNr = hasNgxOnList && !sl.afterUpscale;
        if (notifyPass(0, deferNgxAfterNr) && deferNgxAfterNr)
        {
            waitNrPass(0);
            NotifyNgxLists(p->runtime[0], L, queue, n, lists, false, [this](const std::string& s) { p->Log(s); });
        }
        auto value = ++p->serial;
        if (FAILED(queue->Signal(p->fence.Get(), value)))
        {
            p->failed = true;
            p->Log("D3D12 completion Signal failed; resources retained");
            return;
        }
        stallwatch::Watch::Get().Arm(p->fence.Get(), value);
        sl.completion.store(value);
        sl.submission.Submit(GetTickCount64());
        p->WaitAfterSubmitIfEveryFrame(slot);
        return;
    }
    const bool deferNgxAfterNr = hasNgxOnList && !sl.afterUpscale;
    for (UINT i = 0; i < passCount; ++i)
    {
        if (!notifyPass(i, deferNgxAfterNr))
            break;
        // All runtimes use HIP stream 0. Publish the next pass only once the previous
        // worker finished; otherwise its capture-wait kernel could block the first pass.
        waitNrPass(i);
    }
    if (deferNgxAfterNr)
        NotifyNgxLists(p->runtime[0], L, queue, n, lists, false, [this](const std::string& s) { p->Log(s); });
    UINT64 value = ++p->serial;
    if (FAILED(queue->Signal(p->fence.Get(), value)))
    {
        p->failed = true;
        p->Log("D3D12 completion Signal failed");
        return;
    }
    stallwatch::Watch::Get().Arm(p->fence.Get(), value);
    sl.completion.store(value);
    sl.submission.Submit(GetTickCount64());
    p->WaitAfterSubmitIfEveryFrame(slot);
    p->RetireSubmission(false, "Submitted");
}
bool Backend::GraphicsRestartNeeded(UINT activePasses) const { return p->gfxRestart.NeedsRestart(activePasses); }
std::string Backend::Status() const
{
    std::lock_guard guard(p->lock);
    const AmdLayout* L = p->L;
    p->RetireSubmission(false, "Status");
    auto reportedTimeouts = p->timeoutEvents;
    for (UINT i = 0; L && i < p->runtime.size(); ++i)
        if (p->runtime[i])
        {
            auto count = At<UINT>(p->runtime[i], L->timeoutCount);
            if (count > p->observedTimeouts[i])
                reportedTimeouts += count - p->observedTimeouts[i];
        }
    // Menu / Status must name the runtime that was actually identified —
    // 0.3.0, 0.3.1, 0.4.0 and 0.4.1 are all valid, and the user cannot tell them
    // apart from pass DLL filenames alone.
    const std::string runtimeTag = L ? (std::string("AMD runtime ") + L->name + " | ") : std::string();
    if (!p->failed && p->lastSubmitted)
        return runtimeTag + p->status + (p->rtgiStatus.empty() ? "" : " | " + p->rtgiStatus) +
               " | completed frames=" + std::to_string(p->completedFrames) +
               (p->lastCompleted
                    ? " last completion " + std::to_string((GetTickCount64() - p->lastCompleted) / 1000) + "s ago"
                    : " no successful completion") +
               " | timeout events=" + std::to_string(reportedTimeouts) +
               " | skipped pending/GPU=" + std::to_string(p->pendingSkips) + "/" + std::to_string(p->fenceSkips);
    return runtimeTag + p->status;
}
UINT64 Backend::RecordedFrames() const { return p->frames; }
void Backend::InvalidateHistory() { p->resetRequested.store(true); }
bool Backend::Ready()
{
    std::lock_guard guard(p->lock);
    if (!p->fence)
        return false;
    p->RetireSubmission(false, "Ready");
    const auto completed = p->fence->GetCompletedValue();
    return !p->failed && !p->AnySlotBusy() && completed != UINT64_MAX && completed >= p->LatestCompletion();
}
bool Backend::Failed() const { return p->failed; }
bool Backend::Retry()
{
    std::lock_guard guard(p->lock);
    const AmdLayout* L = p->L;
    if (!p->failed || !p->fence || !L || p->shutDown || p->deviceLostReported || !p->completionOrderValid ||
        FAILED(p->device->GetDeviceRemovedReason()))
        return false;
    p->RetireSubmission(false, "Retry");
    const auto completed = p->fence->GetCompletedValue();
    if (p->AnySlotBusy() || completed == UINT64_MAX || completed < p->LatestCompletion())
        return false;
    for (auto h : p->runtime)
        if (h && At<ID3D12CommandList*>(h, L->pendingList) != nullptr)
            return false;
    // A pass that never initialized cannot be retried: its module stays loaded half set up.
    for (UINT i = 0; i < p->runtime.size(); ++i)
        if (p->runtime[i] && !p->initialized[i])
            return false;
    p->failed = false;
    p->resetRequested = true;
    p->Log("AMD backend: retrying after a failure, with fresh history");
    return true;
}
bool Backend::Shutdown()
{
    std::lock_guard guard(p->lock);
    const AmdLayout* L = p->L;
    if (!p->fence)
        return false;
    p->RetireSubmission(false, "Shutdown");
#ifdef AMD_RETIRE_DIAGNOSTICS
    p->diagnostics.Flush(p->directory, L ? L->name : "uninitialized", "shutdown");
#endif
    const auto completed = p->fence->GetCompletedValue();
    if (p->AnySlotBusy() || completed == UINT64_MAX || completed < p->LatestCompletion())
        return false;
    for (auto h : p->runtime)
        if (h && L)
        {
            if (p->hipSet)
                p->hipSet(p->hipDevice);
            reinterpret_cast<void (*)()>(reinterpret_cast<uintptr_t>(h) + L->shutdown)();
        }
    p->failed = true;
    p->shutDown = true;
    p->Log("Workers stopped outside loader lock");
    return true;
}
} // namespace AmdPreSr
