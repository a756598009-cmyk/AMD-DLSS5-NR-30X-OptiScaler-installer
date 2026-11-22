#pragma once
// Person protection (NrPersonPartition) on the game's command list, from TheAutomatic/dlss-5-amd-project (GPL-3.0),
// dlssnr/person/PersonPartition.h. Theirs keeps each recording's resources through recording observers; here, as in
// the residual stabilizer, every recording is taken to run once and in order on the game's queue: rings deep enough
// for the lists in flight (descriptor sets, mask uploads, 25 guide frames), one output texture. Captures for the
// model are read back once a fence signalled after the list's submission (Submitted) has passed, or, when that list
// is never seen submitted, once the frame number the capture writes last has reached the readback.
#include "PersonInference.h"
#include "PersonShader.h"
#include "PersonSettings.h"
#include "PersonDiagnostics.h"
#include "PersonCapture.h"
#include "../amd/AmdPreSr.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

namespace DlssNr::Person
{
// Starts, restarts or stops the worker for these settings (cheap when nothing changed; a failure latches until the
// next change). True once its model is loaded. directory: the OptiScaler DLL's folder.
inline bool Prepare(bool enabled, const std::filesystem::path& directory, int model, unsigned faceSize)
{
    try
    {
        Worker().Configure(enabled, directory / L"person-model", ModelFileName(model),
                           model == 2 ? Face::BoundedSize(faceSize) : 320);
        return enabled && Worker().Available();
    }
    catch (...)
    {
        return false;
    }
}

class Partition
{
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    // Descriptor sets and mask uploads in flight, as the stabilizer's: a pending list reads them when it runs.
    static constexpr UINT kSets = 8, kSrvs = 33, kViews = kSrvs + 4, kGuides = HistoryCount + 1;
    static constexpr UINT kCapture = 3 * ModelSize * ModelSize; // floats; one more holds the frame marker
    struct Guide
    {
        Resource texture, mask;
        uint64_t frame = 0, epoch = 0, tick = 0;
        float jitterX = 0, jitterY = 0;
        bool maskAccepted = false;
    };
    struct Capture
    {
        enum State
        {
            Free,
            Recorded,
            Submitted
        } state = Free;
        Resource readback;
        ID3D12CommandList* list = nullptr; // compared only, never dereferenced
        UINT64 fence = 0;
        uint64_t epoch = 0, frame = 0, tick = 0;
        unsigned width = 0, height = 0;
    };
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> capturePso, warpPso, composePso;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    Resource uploads[kSets], input, output, retired[kSets];
    float* uploadMapped[kSets] {};
    Guide guides[kGuides];
    // Submitted runs on whichever thread submits; everything else under the bridge's frame mutex.
    std::mutex captureMutex;
    Capture captures[CaptureSchedule::MaxPending];
    CaptureSchedule schedule;
    uint64_t epoch = 1, frame = 0;
    unsigned width = 0, height = 0;
    UINT set = 0, idle = 0;
    std::atomic<bool> invalid { false };
    PersonDiagnostics diagnostic;
    mutable std::mutex statusMutex;
    std::string status;

    static void Check(HRESULT hr, const char* what)
    {
        if (FAILED(hr))
            throw std::runtime_error(std::string("person protection: ") + what);
    }
    static void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                        D3D12_RESOURCE_STATES b)
    {
        if (a == b)
            return;
        D3D12_RESOURCE_BARRIER v {};
        v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        v.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
        c->ResourceBarrier(1, &v);
    }
    Resource Buffer(UINT64 bytes, D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = type;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (type == D3D12_HEAP_TYPE_DEFAULT)
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto state = type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                           : type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Resource r;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)),
              "buffer");
        return r;
    }
    Resource Texture(UINT w, UINT h, DXGI_FORMAT format)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = format;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Resource r;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                              IID_PPV_ARGS(&r)),
              "texture");
        return r;
    }
    void Build()
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvs, 0, 0, 0 },
                                             { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, kSrvs } };
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, sizeof(Constants) / 4 };
        D3D12_ROOT_SIGNATURE_DESC rd {};
        rd.NumParameters = 2;
        rd.pParameters = params;
        Microsoft::WRL::ComPtr<ID3DBlob> b, e;
        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e), "root signature");
        Check(device->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&root)),
              "root signature");
        for (auto [entry, pso] : { std::pair { "capture_main", &capturePso }, std::pair { "warp_main", &warpPso },
                                   std::pair { "compose_main", &composePso } })
        {
            b.Reset();
            e.Reset();
            if (FAILED(D3DCompile(Shader, std::strlen(Shader), "person protection", nullptr, nullptr, entry, "cs_5_1",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &e)))
                throw std::runtime_error(std::string("person protection shader: ") +
                                         (e ? static_cast<const char*>(e->GetBufferPointer()) : entry));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
            pd.pRootSignature = root.Get();
            pd.CS = { b->GetBufferPointer(), b->GetBufferSize() };
            Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(pso)), entry);
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSets * kViews;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "descriptor heap");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        for (UINT i = 0; i < kSets; ++i)
        {
            uploads[i] = Buffer(MaskSize * MaskSize * 4, D3D12_HEAP_TYPE_UPLOAD);
            D3D12_RANGE none {};
            Check(uploads[i]->Map(0, &none, reinterpret_cast<void**>(&uploadMapped[i])), "mask upload");
        }
        input = Buffer((kCapture + 4) * 4, D3D12_HEAP_TYPE_DEFAULT);
    }
    void SetStatus(const char* s)
    {
        std::lock_guard lock(statusMutex);
        status = s;
    }
    // A new mask stream: masks and guides of the old one are never used again. Caller holds the frame mutex.
    void Restart(ResetReason reason)
    {
        diagnostic.Reset(reason);
        ++epoch;
        schedule = {};
    }
    const Guide* GuideFor(uint64_t f) const
    {
        const Guide& g = guides[f % kGuides];
        return f && g.frame == f && g.epoch == epoch && g.texture ? &g : nullptr;
    }
    // Readbacks the GPU has finished go to the worker, the newest current one only; the rest are dropped, never
    // queued (a busy worker drops them all). Caller holds the frame mutex.
    void Collect(uint64_t now)
    {
        std::lock_guard lock(captureMutex);
        if (!fence)
            return;
        const UINT64 completed = fence->GetCompletedValue();
        Capture* newest = nullptr;
        std::vector<Capture*> done;
        for (auto& c : captures)
        {
            bool finished = c.state == Capture::Submitted && completed >= c.fence;
            // A list never seen at submission (a layer wrapping the game's lists): its capture writes the frame
            // number last, so once that reached the readback the copy has.
            if (c.state == Capture::Recorded && frame > c.frame + 2)
            {
                float* marker = nullptr;
                D3D12_RANGE range { kCapture * 4, (kCapture + 1) * 4 };
                if (SUCCEEDED(c.readback->Map(0, &range, reinterpret_cast<void**>(&marker))))
                {
                    finished = marker[kCapture] == float(c.frame % (1u << 24));
                    D3D12_RANGE none {};
                    c.readback->Unmap(0, &none);
                }
                if (!finished && frame > c.frame + 120)
                    c.state = Capture::Free; // the list was dropped unexecuted
            }
            if (!finished)
                continue;
            done.push_back(&c);
            if (diagnostic.enabled)
                diagnostic.captureAge.Add(now - c.tick);
            if (c.epoch == epoch && now - c.tick <= 250 && schedule.Newer(c.frame) &&
                (!newest || c.frame > newest->frame))
                newest = &c;
        }
        bool sent = false;
        if (newest && Worker().Ready())
        {
            auto image = std::make_shared<Image>();
            image->epoch = newest->epoch;
            image->frame = newest->frame;
            image->tick = newest->tick;
            image->width = newest->width;
            image->height = newest->height;
            image->rgb.resize(kCapture);
            void* data = nullptr;
            D3D12_RANGE range { 0, kCapture * 4 };
            if (SUCCEEDED(newest->readback->Map(0, &range, &data)))
            {
                std::memcpy(image->rgb.data(), data, kCapture * 4);
                D3D12_RANGE none {};
                newest->readback->Unmap(0, &none);
                sent = Worker().Submit(image);
            }
            if (sent)
                schedule.Submitted(newest->frame);
        }
        if (diagnostic.enabled)
        {
            diagnostic.captureSubmitted += sent;
            diagnostic.captureDiscarded += done.size() - sent;
        }
        for (auto* c : done)
            c->state = Capture::Free;
    }

  public:
    // Keeps the device only: the pipeline and the textures come with the first Record.
    explicit Partition(ID3D12Device* d) : device(d) {}

    // Records the composition and returns an FP16 texture of final's size in NON_PIXEL_SHADER_RESOURCE: inside a
    // current mask the person takes first (pass 1's answer, or final itself in single pass) with the strength and
    // detail of settings, the rest final. first and final are readable (NON_PIXEL_SHADER_RESOURCE) at the frame's
    // render subrect; the colour, motion and depth are in the frame's states and are left there. Returns final, and
    // records nothing, when the guides are missing or unusable; continuous is false after a frame without it.
    ID3D12Resource* Record(ID3D12GraphicsCommandList* c, const AmdPreSr::Frame& f, ID3D12Resource* first,
                           ID3D12Resource* final, bool jitteredMotion, bool continuous,
                           std::shared_ptr<const Mask> mask, Settings settings)
    {
        settings = settings.Bounded();
        const uint64_t now = GetTickCount64();
        diagnostic.Enable(settings.debugMask, now);
        if (invalid.exchange(false))
            Restart(ResetReason::External);
        const auto depthDesc = f.depth ? f.depth->GetDesc() : D3D12_RESOURCE_DESC {};
        const auto depthFormat = f.depth && !(depthDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
                                     ? AmdPreSr::DepthReadFormat(depthDesc.Format)
                                     : DXGI_FORMAT_UNKNOWN;
        if (!first || !final || !f.colour || !f.motion || depthFormat == DXGI_FORMAT_UNKNOWN || jitteredMotion ||
            c->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            Restart(ResetReason::Guides);
            SetStatus("Person protection bypassed: no first pass, or no reliable motion and depth");
            return final;
        }
        try
        {
            if (!root)
                Build();
            Collect(now);
            const UINT w = f.width, h = f.height;
            for (auto* r : { f.colour, first, final })
            {
                const auto d = r->GetDesc();
                if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1 || d.Width < w ||
                    d.Height < h || (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
                    throw std::runtime_error("unsupported colour texture");
            }
            if (!continuous || f.reset || w != width || h != height)
                Restart(!continuous ? ResetReason::Delayed : f.reset ? ResetReason::External : ResetReason::Resize);
            width = w;
            height = h;
            retired[set].Reset();
            const auto size = final->GetDesc();
            if (!output || output->GetDesc().Width != size.Width || output->GetDesc().Height != size.Height)
            {
                retired[set] = std::move(output);
                output = Texture(static_cast<UINT>(size.Width), size.Height, DXGI_FORMAT_R16G16B16A16_FLOAT);
            }

            // This frame's guide: the motion and depth key on the 160 grid, and the mask warped to it.
            ++frame;
            Guide& g = guides[frame % kGuides];
            if (!g.texture)
            {
                g.texture = Texture(MaskSize, MaskSize, DXGI_FORMAT_R32G32B32A32_FLOAT);
                g.mask = Texture(MaskSize, MaskSize, DXGI_FORMAT_R32G32_FLOAT);
            }
            g.frame = frame;
            g.epoch = epoch;
            g.tick = now;
            g.jitterX = f.jitterX;
            g.jitterY = f.jitterY;
            const Guide* prev = GuideFor(frame - 1);

            // A mask from age frames ago is traced back through every guide since; any missing one refuses it.
            unsigned age = 0;
            bool accepted = mask && mask->values.size() == MaskSize * MaskSize && mask->epoch == epoch &&
                            mask->width == w && mask->height == h && now - mask->tick <= 250 && mask->frame <= frame &&
                            frame - mask->frame <= HistoryCount;
            if (accepted)
            {
                age = unsigned(frame - mask->frame);
                for (unsigned i = 1; i <= age && accepted; ++i)
                    accepted = GuideFor(frame - i) != nullptr;
            }
            if (!accepted)
                age = 0;
            if (diagnostic.enabled)
            {
                const auto reason =
                    accepted               ? MaskDecision::Accepted
                    : !mask                ? MaskDecision::Missing
                    : mask->epoch != epoch ? MaskDecision::Epoch
                    : mask->values.size() != MaskSize * MaskSize || mask->width != w || mask->height != h
                        ? MaskDecision::Size
                    : now - mask->tick > 250                                    ? MaskDecision::Expired
                    : mask->frame > frame || frame - mask->frame > HistoryCount ? MaskDecision::FrameAge
                                                                                : MaskDecision::Guides;
                diagnostic.Mask(reason);
                if (mask)
                {
                    diagnostic.maskAge.Add(now - mask->tick);
                    if (mask->epoch != diagnostic.rawEpoch || mask->frame != diagnostic.rawFrame)
                    {
                        diagnostic.rawEpoch = mask->epoch;
                        diagnostic.rawFrame = mask->frame;
                        diagnostic.arrivalAge.Add(now - mask->tick);
                        if (!mask->values.empty())
                            diagnostic.rawCoverage.Add(uint64_t(std::count_if(mask->values.begin(), mask->values.end(),
                                                                              [](float v) { return v > .5f; })) *
                                                       10000 / mask->values.size());
                    }
                }
            }
            g.maskAccepted = accepted;
            if (accepted)
                std::memcpy(uploadMapped[set], mask->values.data(), MaskSize * MaskSize * 4);
            else
                std::memset(uploadMapped[set], 0, MaskSize * MaskSize * 4);

            // A capture for the model, about 20 a second, at most four in flight.
            Capture* slot = nullptr;
            {
                std::lock_guard lock(captureMutex);
                size_t pending = 0;
                for (auto& cap : captures)
                {
                    // The game recorded this list again: the capture it held was never submitted.
                    if (cap.state == Capture::Recorded && cap.list == c)
                        cap.state = Capture::Free;
                    pending += cap.state != Capture::Free;
                }
                if (schedule.Request(CaptureClock(), pending, Worker().Available()))
                    for (auto& cap : captures)
                        if (cap.state == Capture::Free)
                        {
                            slot = &cap;
                            break;
                        }
                if (slot)
                {
                    if (!slot->readback)
                        slot->readback = Buffer((kCapture + 4) * 4, D3D12_HEAP_TYPE_READBACK);
                    slot->state = Capture::Recorded;
                    slot->list = c;
                    slot->epoch = epoch;
                    slot->frame = frame;
                    slot->tick = now;
                    slot->width = w;
                    slot->height = h;
                }
            }

            const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
            auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
            cpu.ptr += SIZE_T(set) * kViews * stride;
            gpu.ptr += UINT64(set) * kViews * stride;
            auto srv = [&](ID3D12Resource* r, DXGI_FORMAT format)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC v {};
                v.Format = format;
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                v.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(r, &v, cpu);
                cpu.ptr += stride;
            };
            auto uav = [&](ID3D12Resource* r, DXGI_FORMAT format)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
                v.Format = format;
                v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                device->CreateUnorderedAccessView(r, nullptr, &v, cpu);
                cpu.ptr += stride;
            };
            srv(f.colour, AmdPreSr::ReadFormat(f.colour->GetDesc().Format));
            srv(first, AmdPreSr::ReadFormat(first->GetDesc().Format));
            srv(final, AmdPreSr::ReadFormat(size.Format));
            srv(f.motion, AmdPreSr::ReadFormat(f.motion->GetDesc().Format));
            srv(f.depth, depthFormat);
            srv(g.texture.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
            for (unsigned i = 1; i <= HistoryCount; ++i)
            {
                const Guide* old = GuideFor(frame - i);
                srv(old ? old->texture.Get() : nullptr, DXGI_FORMAT_R32G32B32A32_FLOAT);
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC bv {};
            bv.Format = DXGI_FORMAT_R32_FLOAT;
            bv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            bv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            bv.Buffer.NumElements = MaskSize * MaskSize;
            device->CreateShaderResourceView(uploads[set].Get(), &bv, cpu);
            cpu.ptr += stride;
            srv(g.mask.Get(), DXGI_FORMAT_R32G32_FLOAT);
            srv(prev ? prev->mask.Get() : nullptr, DXGI_FORMAT_R32G32_FLOAT);
            D3D12_UNORDERED_ACCESS_VIEW_DESC iv {};
            iv.Format = DXGI_FORMAT_R32_FLOAT;
            iv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            iv.Buffer.NumElements = kCapture + 4;
            device->CreateUnorderedAccessView(input.Get(), nullptr, &iv, cpu);
            cpu.ptr += stride;
            uav(g.texture.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
            uav(g.mask.Get(), DXGI_FORMAT_R32G32_FLOAT);
            uav(output.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);

            const UINT mvW = f.motionWidth ? f.motionWidth : w, mvH = f.motionHeight ? f.motionHeight : h;
            const float scaleX = float(w) / (std::max) (w, h), scaleY = float(h) / (std::max) (w, h);
            const float deltaMs = prev ? float(std::clamp<uint64_t>(now - prev->tick, 1, 100)) : 100.f;
            const Constants k {
                w,
                h,
                mvW,
                mvH,
                f.motionScaleX / mvW,
                f.motionScaleY / mvH,
                prev ? (prev->jitterX - f.jitterX) / w : 0.f,
                prev ? (prev->jitterY - f.jitterY) / h : 0.f,
                std::isfinite(f.preExposure) && f.preExposure > 0 ? f.preExposure : 1.f,
                slot ? 1u : 0u,
                age,
                accepted ? 1u : 0u,
                scaleX,
                scaleY,
                (1 - scaleX) * .5f,
                (1 - scaleY) * .5f,
                f.depthInverted ? 1u : 0u,
                settings.strength,
                settings.detail,
                accepted ? (std::min) (MaskFreshness(now - mask->tick), age <= 20 ? 1.f : float(24 - age) / 4.f) : 0.f,
                accepted && prev && prev->maskAccepted ? 1u : 0u,
                1.f - std::exp(-deltaMs / 30.f),
                1.f - std::exp(-deltaMs / 50.f),
                settings.debugMask ? 1.f : 0.f,
                float(frame % (1u << 24))
            };

            const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            Barrier(c, f.colour, f.colourState, read);
            Barrier(c, f.motion, f.motionState, read);
            Barrier(c, f.depth, f.depthState, read);
            auto* hh = heap.Get();
            c->SetDescriptorHeaps(1, &hh);
            c->SetComputeRootSignature(root.Get());
            c->SetComputeRootDescriptorTable(0, gpu);
            c->SetComputeRoot32BitConstants(1, sizeof(k) / 4, &k, 0);
            Barrier(c, g.texture.Get(), read, write);
            c->SetPipelineState(capturePso.Get());
            c->Dispatch(slot ? 80 : 20, slot ? 80 : 20, 1);
            Barrier(c, g.texture.Get(), write, read);
            if (slot)
            {
                Barrier(c, input.Get(), write, D3D12_RESOURCE_STATE_COPY_SOURCE);
                c->CopyBufferRegion(slot->readback.Get(), 0, input.Get(), 0, (kCapture + 4) * 4);
                Barrier(c, input.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, write);
            }
            Barrier(c, g.mask.Get(), read, write);
            c->SetPipelineState(warpPso.Get());
            c->Dispatch(MaskSize / 8, MaskSize / 8, 1);
            Barrier(c, g.mask.Get(), write, read);
            Barrier(c, output.Get(), read, write);
            c->SetPipelineState(composePso.Get());
            c->Dispatch((static_cast<UINT>(size.Width) + 7) / 8, (size.Height + 7) / 8, 1);
            Barrier(c, output.Get(), write, read);
            Barrier(c, f.depth, read, f.depthState);
            Barrier(c, f.motion, read, f.motionState);
            Barrier(c, f.colour, read, f.colourState);
            set = (set + 1) % kSets;
            idle = 0;
            SetStatus(accepted ? "Person protection active" : "Person protection: waiting for a current mask");
            return output.Get();
        }
        catch (const std::exception& e)
        {
            Restart(ResetReason::Error);
            SetStatus(e.what());
            return final;
        }
    }

    // After the game's lists were handed to the queue: a capture recorded on one of them is read back once the
    // fence signalled here has passed. Any thread.
    void Submitted(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
    {
        std::lock_guard lock(captureMutex);
        bool signal = false;
        for (auto& cap : captures)
            if (cap.state == Capture::Recorded)
                for (UINT i = 0; i < n; ++i)
                    if (lists[i] == cap.list)
                    {
                        if (!signal && FAILED(q->Signal(fence.Get(), fenceValue + 1)))
                            return;
                        signal = true;
                        cap.state = Capture::Submitted;
                        cap.fence = fenceValue + 1;
                    }
        fenceValue += signal;
    }

    // Any thread: the next Record starts a new mask stream.
    void Invalidate() { invalid = true; }

    // A frame without the pass. The output goes once no list that used it can still be pending.
    void Idle()
    {
        if (++idle == 1)
            Restart(ResetReason::External);
        if (output && idle > kSets)
        {
            output.Reset();
            for (auto& r : retired)
                r.Reset();
        }
        if (idle == kSets + 1)
            SetStatus("");
    }

    // One line every two seconds while the mask overlay is on; empty otherwise. Caller holds the frame mutex.
    std::string DiagnosticReport() { return diagnostic.Report(GetTickCount64()); }

    // The mask stream and the last frame recorded: a mask taken now carries these (tests).
    uint64_t Epoch() const { return epoch; }
    uint64_t Frame() const { return frame; }

    std::string Status() const
    {
        std::lock_guard lock(statusMutex);
        return status;
    }
};
} // namespace DlssNr::Person
