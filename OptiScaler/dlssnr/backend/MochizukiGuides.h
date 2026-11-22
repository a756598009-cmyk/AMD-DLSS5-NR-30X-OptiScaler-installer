#pragma once
#include "../amd/AmdPreSr.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>

namespace DlssNr::Backend
{
// What mochizuki's guides need before they cross to its Vulkan device, made on the game's list:
//  - the motion vectors with the camera jitter's step between this frame and the last added (MochizukiJitterHistory),
//    so the history is fetched where the previous frame's sample really was;
//  - a depth the runtime cannot copy as it is (D24S8, 24 bits a float image cannot take) read through its shader view
//    into R32F.
// A guide that needs neither goes through untouched.
inline constexpr char MochizukiGuidesShader[] = R"(
Texture2D<float2> motion:register(t0);Texture2D<float> depth:register(t1);
RWTexture2D<float2> motionOut:register(u0);RWTexture2D<float> depthOut:register(u1);
cbuffer Params:register(b0){uint mw,mh,dw,dh;float2 offset;uint flags,pad;}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if((flags&1)&&id.x<mw&&id.y<mh)motionOut[id.xy]=motion.Load(int3(id.xy,0))+offset;
 if((flags&2)&&id.x<dw&&id.y<dh)depthOut[id.xy]=depth.Load(int3(id.xy,0));
}
)";

class MochizukiGuides
{
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    // Descriptor sets in flight: a pending list reads its descriptors when it runs.
    static constexpr UINT kSets = 8, kViews = 4;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Resource motionOut, depthOut;
    // Textures replaced on a size change, kept as long as the descriptor set that last used them.
    Resource retired[kSets][2];
    UINT set = 0;

    static void Check(HRESULT hr)
    {
        if (FAILED(hr))
            throw std::runtime_error("mochizuki guides D3D12 error " + std::to_string((UINT) hr));
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
    Resource Make(UINT w, UINT h, DXGI_FORMAT format)
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
        Check(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&r)));
        return r;
    }
    void Build()
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 },
                                             { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2 } };
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, 8 };
        D3D12_ROOT_SIGNATURE_DESC rd {};
        rd.NumParameters = 2;
        rd.pParameters = params;
        Microsoft::WRL::ComPtr<ID3DBlob> b, e;
        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e));
        Check(device->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&root)));
        Check(D3DCompile(MochizukiGuidesShader, sizeof(MochizukiGuidesShader), "mochizuki guides", nullptr, nullptr,
                         "main", "cs_5_0", 0, 0, &b, &e));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root.Get();
        pd.CS = { b->GetBufferPointer(), b->GetBufferSize() };
        Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline)));
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSets * kViews;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    }

  public:
    // The runtime copies these depth formats as they are; any other depth is read through its view here.
    static bool RuntimeTakesDepth(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
            return true;
        default:
            return false;
        }
    }

    struct Out
    {
        ID3D12Resource* motion = nullptr;
        D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        ID3D12Resource* depth = nullptr;
        D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    };

    explicit MochizukiGuides(ID3D12Device* d) : device(d) {}

    // f's guides, with `offset` (in motion vector units) added to the vectors when shift is set, and its depth in
    // R32F when the runtime cannot take it. Returns f's own guide for anything it leaves alone: its depth is null when
    // it can be read neither way. Throws before recording anything on failure.
    Out Prepare(ID3D12GraphicsCommandList* c, const AmdPreSr::Frame& f, bool shift, float offsetX, float offsetY)
    {
        Out out { f.motion, f.motionState, f.depth, f.depthState };
        const D3D12_RESOURCE_DESC md = f.motion ? f.motion->GetDesc() : D3D12_RESOURCE_DESC {};
        const DXGI_FORMAT motionRead = f.motion ? AmdPreSr::ReadFormat(md.Format) : DXGI_FORMAT_UNKNOWN;
        const bool doMotion = shift && f.motion && md.SampleDesc.Count == 1 &&
                              !(md.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
                              motionRead != DXGI_FORMAT_UNKNOWN;
        const D3D12_RESOURCE_DESC dd = f.depth ? f.depth->GetDesc() : D3D12_RESOURCE_DESC {};
        const DXGI_FORMAT depthRead =
            f.depth && !(dd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) && dd.SampleDesc.Count == 1
                ? AmdPreSr::DepthReadFormat(dd.Format)
                : DXGI_FORMAT_UNKNOWN;
        const bool doDepth = f.depth && !RuntimeTakesDepth(dd.Format) && depthRead != DXGI_FORMAT_UNKNOWN;
        if (f.depth && !RuntimeTakesDepth(dd.Format) && !doDepth)
            out.depth = nullptr;
        if (!doMotion && !doDepth)
            return out;
        if (!pipeline)
            Build();
        const UINT mw = doMotion ? UINT(md.Width) : 0, mh = doMotion ? md.Height : 0;
        const UINT dw = doDepth ? UINT(dd.Width) : 0, dh = doDepth ? dd.Height : 0;
        for (auto& r : retired[set])
            r.Reset();
        if (doMotion && (!motionOut || motionOut->GetDesc().Width != mw || motionOut->GetDesc().Height != mh))
        {
            retired[set][0] = std::move(motionOut);
            motionOut = Make(mw, mh, DXGI_FORMAT_R32G32_FLOAT);
        }
        if (doDepth && (!depthOut || depthOut->GetDesc().Width != dw || depthOut->GetDesc().Height != dh))
        {
            retired[set][1] = std::move(depthOut);
            depthOut = Make(dw, dh, DXGI_FORMAT_R32_FLOAT);
        }
        const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(set) * kViews * stride;
        gpu.ptr += UINT64(set) * kViews * stride;
        set = (set + 1) % kSets;
        // A guide left alone gets a null view of the shader's type: the table is read whole.
        D3D12_SHADER_RESOURCE_VIEW_DESC sv {};
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        sv.Format = doMotion ? motionRead : DXGI_FORMAT_R32G32_FLOAT;
        device->CreateShaderResourceView(doMotion ? f.motion : nullptr, &sv, cpu);
        cpu.ptr += stride;
        sv.Format = doDepth ? depthRead : DXGI_FORMAT_R32_FLOAT;
        device->CreateShaderResourceView(doDepth ? f.depth : nullptr, &sv, cpu);
        cpu.ptr += stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv {};
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uv.Format = DXGI_FORMAT_R32G32_FLOAT;
        device->CreateUnorderedAccessView(doMotion ? motionOut.Get() : nullptr, nullptr, &uv, cpu);
        cpu.ptr += stride;
        uv.Format = DXGI_FORMAT_R32_FLOAT;
        device->CreateUnorderedAccessView(doDepth ? depthOut.Get() : nullptr, nullptr, &uv, cpu);

        constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (doMotion)
        {
            Barrier(c, f.motion, f.motionState, kRead);
            Barrier(c, motionOut.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (doDepth)
        {
            Barrier(c, f.depth, f.depthState, kRead);
            Barrier(c, depthOut.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        ID3D12DescriptorHeap* heaps[] = { heap.Get() };
        c->SetDescriptorHeaps(1, heaps);
        c->SetComputeRootSignature(root.Get());
        c->SetPipelineState(pipeline.Get());
        c->SetComputeRootDescriptorTable(0, gpu);
        struct
        {
            UINT mw, mh, dw, dh;
            float x, y;
            UINT flags, pad;
        } k { mw, mh, dw, dh, offsetX, offsetY, (doMotion ? 1u : 0u) | (doDepth ? 2u : 0u), 0 };
        c->SetComputeRoot32BitConstants(1, 8, &k, 0);
        c->Dispatch((std::max(mw, dw) + 7) / 8, (std::max(mh, dh) + 7) / 8, 1);
        if (doMotion)
        {
            Barrier(c, motionOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
            Barrier(c, f.motion, kRead, f.motionState);
            out.motion = motionOut.Get();
            out.motionState = kRead;
        }
        if (doDepth)
        {
            Barrier(c, depthOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
            Barrier(c, f.depth, kRead, f.depthState);
            out.depth = depthOut.Get();
            out.depthState = kRead;
        }
        return out;
    }
};
} // namespace DlssNr::Backend
