#pragma once
#include "AmdPreSr.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <string>
namespace AmdPreSr
{
// Temporal filter on the runtime's edit r = E(result) - E(colour), E(c) = sRGB(c' / (1 + c')) with c' the colour
// over the frame's pre-exposure. The history is moved along the nearest surface's motion vector plus the jitter step
// and clamped to r +- delta, so a pixel moves by at most alpha * delta in E whatever the vectors say. Pixels without
// a continuous history on the same surface pass the result through bit for bit. history.a keeps the depth key.
inline constexpr char ResidualStabilizerShader[] = R"(
Texture2D<float4> colour:register(t0);Texture2D<float4> result:register(t1);Texture2D<float2> motion:register(t2);
Texture2D<float> depth:register(t3);Texture2D<float4> history:register(t4);
RWTexture2D<float4> output:register(u0);RWTexture2D<float4> next:register(u1);
cbuffer Params:register(b0){uint w,h,mvW,mvH;float2 mvScale,jitterStep;float alpha,delta,preExposure;uint flags;}
float3 E(float3 c){c=min(max(c,0),65504)/preExposure;c/=1+c;return c<=.0031308?c*12.92:1.055*pow(c,1/2.4)-.055;}
float3 Einv(float3 y){float3 s=clamp(y,0,.99999326);s=s<=.04045?s/12.92:pow((s+.055)/1.055,2.4);
 return min(s/max(1-s,1e-7),65504)*preExposure;}
float Key(int2 p){float d=depth.Load(int3(p,0));return flags&2?d:1-d;}
bool Near(float a,float b){return abs(a-b)<=.1*max(max(a,b),1e-6);}
void Axis(float p,float extent,out float3 pos,out float3 weight){
 float c=floor(p-.5)+.5,t=saturate(p-c),t2=t*t,t3=t2*t;
 float l=t2-.5*(t+t3),m=1.5*t3-2.5*t2+1,r=.5*(t3-t2),o=1-l-m-r,s=m+o;
 pos=clamp(float3(c-1,c+o/s,c+2),.5,extent-.5);weight=float3(l,s,r);}
float3 Fetch(float2 x){float2 p=clamp(x-.5,0,float2(w-1,h-1));uint2 lo=uint2(p),hi=min(lo+1,uint2(w-1,h-1));
 float2 f=p-float2(lo);
 float3 top=history.Load(int3(lo,0)).rgb*(1-f.x)+history.Load(int3(hi.x,lo.y,0)).rgb*f.x;
 float3 bottom=history.Load(int3(lo.x,hi.y,0)).rgb*(1-f.x)+history.Load(int3(hi,0)).rgb*f.x;
 return top*(1-f.y)+bottom*f.y;}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 uint ow,oh;output.GetDimensions(ow,oh);
 if(id.x>=ow||id.y>=oh)return;
 float4 o=result.Load(int3(id.xy,0));
 if(id.x>=w||id.y>=h){output[id.xy]=o;return;}
 float3 e=E(colour.Load(int3(id.xy,0)).rgb),r=E(o.rgb)-e;
 int2 p=int2(id.xy),tap=p;
 float k=Key(p),best=k;
 static const int o3[3]={0,-1,1};
 for(int j=0;j<3;++j)for(int i=0;i<3;++i){
  int2 q=clamp(p+int2(o3[i],o3[j]),0,int2(w-1,h-1));float kq=Key(q);if(kq>best){best=kq;tap=q;}}
 uint2 m=min(uint2((float2(tap)+.5)*float2(mvW,mvH)/float2(w,h)),uint2(mvW-1,mvH-1));
 float2 x=float2(p)+.5+motion.Load(int3(m,0))*mvScale+jitterStep;
 bool valid=(flags&1)&&x.x>=0&&x.x<w&&x.y>=0&&x.y<h;
 if(valid){float2 q=clamp(x-.5,0,float2(w-1,h-1));uint2 lo=uint2(q),hi=min(lo+1,uint2(w-1,h-1));
  valid=Near(history.Load(int3(lo,0)).a,k)||Near(history.Load(int3(hi.x,lo.y,0)).a,k)||
   Near(history.Load(int3(lo.x,hi.y,0)).a,k)||Near(history.Load(int3(hi,0)).a,k);}
 float3 rp=r;
 if(valid){
  float3 px,py,wx,wy;Axis(x.x,w,px,wx);Axis(x.y,h,py,wy);
  float left=wx.x*wy.y,top=wx.y*wy.x,center=wx.y*wy.y,bottom=wx.y*wy.z,right=wx.z*wy.y;
  float3 H=(Fetch(float2(px.x,py.y))*left+Fetch(float2(px.y,py.x))*top+Fetch(float2(px.y,py.y))*center+
   Fetch(float2(px.y,py.z))*bottom+Fetch(float2(px.z,py.y))*right)/(left+top+center+bottom+right);
  rp+=alpha*(clamp(H,r-delta,r+delta)-r);}
 output[id.xy]=valid?float4(Einv(e+rp),o.a):o;
 next[id.xy]=float4(rp,k);
}
)";
class ResidualStabilizer
{
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    // Descriptor sets in flight: the game's resources rotate and a pending list reads its descriptors when it runs.
    static constexpr UINT kSets = 8, kViews = 7;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Resource history[2], output;
    // Textures replaced on a size change, kept as long as the descriptor set that last used them.
    Resource retired[kSets][3];
    UINT current = 0, set = 0, idle = 0;
    float jitterX = 0, jitterY = 0;
    std::atomic<bool> invalid { false };
    static void Check(HRESULT hr)
    {
        if (FAILED(hr))
            throw std::runtime_error("AMD stabilizer D3D12 error " + std::to_string((UINT) hr));
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
    Resource Make(UINT w, UINT h)
    {
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
        Resource r;
        Check(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&r)));
        return r;
    }
    void Build()
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0 },
                                             { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 5 } };
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, 12 };
        D3D12_ROOT_SIGNATURE_DESC rd {};
        rd.NumParameters = 2;
        rd.pParameters = params;
        Microsoft::WRL::ComPtr<ID3DBlob> b, e;
        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e));
        Check(device->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&root)));
        Check(D3DCompile(ResidualStabilizerShader, sizeof(ResidualStabilizerShader), "AMD stabilizer", nullptr, nullptr,
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
    // Keeps the device only: the pipeline and the textures come with the first Record.
    explicit ResidualStabilizer(ID3D12Device* d) : device(d) {}
    // Records the filter over result (NON_PIXEL_SHADER_RESOURCE, the frame's render subrect at its top left) and
    // returns an FP16 texture of result's size in the same state. Without shader-readable depth it records nothing and
    // returns result itself: the depth test is what keeps one surface's history off another. Throws before recording
    // anything on failure.
    ID3D12Resource* Record(ID3D12GraphicsCommandList* c, const Frame& f, ID3D12Resource* result, bool continuous,
                           float alpha, float threshold255)
    {
        const auto depthDesc = f.depth ? f.depth->GetDesc() : D3D12_RESOURCE_DESC {};
        const auto depthFormat = f.depth && !(depthDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
                                     ? DepthReadFormat(depthDesc.Format)
                                     : DXGI_FORMAT_UNKNOWN;
        if (depthFormat == DXGI_FORMAT_UNKNOWN)
            return result;
        if (!pipeline)
            Build();
        const UINT w = f.width, h = f.height;
        const auto size = result->GetDesc();
        for (auto& r : retired[set])
            r.Reset();
        if (!history[0] || history[0]->GetDesc().Width != w || history[0]->GetDesc().Height != h)
        {
            retired[set][0] = std::move(history[0]);
            retired[set][1] = std::move(history[1]);
            history[0] = Make(w, h);
            history[1] = Make(w, h);
            continuous = false;
        }
        if (!output || output->GetDesc().Width != size.Width || output->GetDesc().Height != size.Height)
        {
            retired[set][2] = std::move(output);
            output = Make(static_cast<UINT>(size.Width), size.Height);
        }
        continuous = !invalid.exchange(false) && continuous;
        const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(set) * kViews * stride;
        gpu.ptr += UINT64(set) * kViews * stride;
        set = (set + 1) % kSets;
        auto view = [&](ID3D12Resource* r, DXGI_FORMAT format)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC v {};
            v.Format = format;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(r, &v, cpu);
            cpu.ptr += stride;
        };
        view(f.colour, ReadFormat(f.colour->GetDesc().Format));
        view(result, ReadFormat(size.Format));
        view(f.motion, ReadFormat(f.motion->GetDesc().Format));
        view(f.depth, depthFormat);
        view(history[current].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        ID3D12Resource* next = history[1 - current].Get();
        for (auto* r : { output.Get(), next })
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u {};
            u.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(r, nullptr, &u, cpu);
            cpu.ptr += stride;
        }
        const UINT mvW = f.motionWidth ? f.motionWidth : w, mvH = f.motionHeight ? f.motionHeight : h;
        struct
        {
            UINT w, h, mvW, mvH;
            float mvScaleX, mvScaleY, jitterStepX, jitterStepY, alpha, delta, preExposure;
            UINT flags;
        } k { w,
              h,
              mvW,
              mvH,
              f.motionScaleX * w / mvW,
              f.motionScaleY * h / mvH,
              jitterX - f.jitterX,
              jitterY - f.jitterY,
              alpha,
              threshold255 / 255.f,
              std::isfinite(f.preExposure) && f.preExposure > 0 ? f.preExposure : 1.f,
              (continuous ? 1u : 0u) | (f.depthInverted ? 2u : 0u) };
        static_assert(sizeof(k) == 12 * sizeof(UINT));
        const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Barrier(c, f.colour, f.colourState, read);
        Barrier(c, f.motion, f.motionState, read);
        Barrier(c, f.depth, f.depthState, read);
        Barrier(c, output.Get(), read, write);
        Barrier(c, next, read, write);
        auto hh = heap.Get();
        c->SetDescriptorHeaps(1, &hh);
        c->SetComputeRootSignature(root.Get());
        c->SetPipelineState(pipeline.Get());
        c->SetComputeRootDescriptorTable(0, gpu);
        c->SetComputeRoot32BitConstants(1, 12, &k, 0);
        c->Dispatch((static_cast<UINT>(size.Width) + 7) / 8, (size.Height + 7) / 8, 1);
        Barrier(c, next, write, read);
        Barrier(c, output.Get(), write, read);
        Barrier(c, f.depth, read, f.depthState);
        Barrier(c, f.motion, read, f.motionState);
        Barrier(c, f.colour, read, f.colourState);
        current = 1 - current;
        jitterX = f.jitterX;
        jitterY = f.jitterY;
        idle = 0;
        return output.Get();
    }
    // Any thread: the next Record starts without history.
    void Invalidate() { invalid = true; }
    // A frame without the pass. The textures go once no list that used them can still be pending, the same depth
    // the descriptor sets rely on.
    void Idle()
    {
        if (output && ++idle > kSets)
        {
            history[0].Reset();
            history[1].Reset();
            output.Reset();
            for (auto& s : retired)
                for (auto& r : s)
                    r.Reset();
        }
    }
};
} // namespace AmdPreSr
