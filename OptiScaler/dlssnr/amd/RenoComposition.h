#pragma once
#include "AmdPreSr.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
namespace AmdPreSr
{
// The colour composition of RenoDX's DLSS Neural Rendering add-on (PEQHUB/RenoDX-DLSS5-Generic, MIT, derived from
// clshortfuse/renodx; Licenses\RenoDX_ATTRIBUTION.txt), after the runtime and before SR. The runtime's answer is not
// taken as the picture: its edit is measured in a proxy (linear light over the pre-exposure and a divisor, through a
// max-channel shoulder at 0.75, as v6_encode builds the network's input) and put back on the game's own colour as a
// gain bounded in log2 (luminance by the guard, a multiple either way; chroma +-chromaStops), faded out where the
// proxy sits at the floor (v6_resolve, Display codec, bounded ratio). Intensity scales the edit up to 1 and above it
// raises the luminance ratio to a power, still inside the guard, as the ReShade add-on's ratio composition does. Colour
// strength 0 keeps only its luminance ratio, the game's hue, as the divisor family's luminance_only does. Where the
// runtime changed nothing the game's colour passes unchanged, and highlights the runtime clipped keep the game's range.
// The divisor is v6_autoscale's Stable governor: it only ever shrinks an over-range proxy (median to 0.30 once 1/64 to
// 1/16 of the samples sit past the shoulder), rising at attack and falling at release stops per second, with a
// hysteresis hold. The optional pedestal pass is v6_pedestal_reduce and v6_commit: in 32x32 blocks whose brightest
// original pixel is near black, it takes off the lift the network added.
inline constexpr char RenoCompositionShader[] = R"(
Texture2D<float4> colour:register(t0);Texture2D<float4> result:register(t1);
RWTexture2D<float4> output:register(u0);RWTexture2D<float4> blocks:register(u1);RWTexture2D<float4> commit:register(u2);
cbuffer Params:register(b0){uint w,h,encoding,flags;float preExposure,colourStrength,chromaStops,attack,release,
 darkGate,pedestalCap,intensity,guard;}
static const float3 kW=float3(.212639,.715169,.072192);
float T(float v,bool inv){float a=abs(v),y=a;
 if(encoding==2)y=inv?(a<=.0031308?12.92*a:1.055*pow(a,1/2.4)-.055):(a<=.04045?a/12.92:pow((a+.055)/1.055,2.4));
 if(encoding==3)y=pow(a,inv?1/2.2:2.2);
 return sign(v)*y;}
float3 Lin(float3 c){return float3(T(c.r,false),T(c.g,false),T(c.b,false))/preExposure;}
float3 Enc(float3 l){l*=preExposure;return float3(T(l.r,true),T(l.g,true),T(l.b,true));}
float3 Shoulder(float3 p){p=max(p,0);float m=max(p.x,max(p.y,p.z));
 if(m>.75)p*=(.75+.25*(1-exp(-5.77078*(m-.75))))/m;return p;}
float Trust(float3 p){return smoothstep(1./1024,1./64,max(p.x,max(p.y,p.z)));}
float Divisor(){return max(commit[uint2(0,0)].r,1e-8);}

groupshared uint histogram[256];groupshared uint valid,shoulder;
uint Bin(float v){return min(uint(floor((clamp(log2(max(v,5.96046448e-8)),-24,24)+24)*(256./48))),255);}
[numthreads(64,1,1)]void Scale(uint local:SV_GroupIndex){
 for(uint b=local;b<256;b+=64)histogram[b]=0;
 if(local==0){valid=0;shoulder=0;}
 GroupMemoryBarrierWithGroupSync();
 for(uint i=local;i<64*36;i+=64){
  uint2 cell=uint2(i&63,i>>6),p=min(uint2((cell.x*2+1)*w/128,(cell.y*2+1)*h/72),uint2(w-1,h-1));
  float3 c=Lin(colour.Load(int3(p,0)).rgb);float v=max(c.x,max(c.y,c.z));
  if((asuint(v)&0x7F800000)!=0x7F800000&&v>0){InterlockedAdd(histogram[Bin(v)],1);InterlockedAdd(valid,1);
   if(v>.75)InterlockedAdd(shoulder,1);}}
 GroupMemoryBarrierWithGroupSync();
 if(local!=0)return;
 float fraction=valid?float(shoulder)/float(valid):0,gate=smoothstep(-6,-4,log2(max(fraction,1e-6))),candidate=1;
 if(gate>0){uint middle=(valid-1)>>1,cumulative=0,bin=0,below=0;
  for(uint k=0;k<256;++k){uint next=cumulative+histogram[k];if(next>middle){bin=k;below=cumulative;break;}cumulative=next;}
  float within=(float(middle-below)+.5)/float(max(histogram[bin],1)),median=-24+(float(bin)+within)*(48./256);
  candidate=exp2(gate*max(median-log2(.3),0));}
 float4 carried=commit[uint2(0,0)];float previous=carried.r,committed=candidate,tracking=0;
 if(!(flags&1)&&previous>0){float error=log2(candidate/previous);
  tracking=abs(error)>(carried.g>.5?.02:.15)?1:0;
  committed=tracking>0?previous*exp2(clamp(error,-release,attack)):previous;}
 commit[uint2(0,0)]=float4(committed,tracking,candidate,fraction);}

float3 Composed(uint2 p){
 float d=Divisor();float3 o=Lin(colour.Load(int3(p,0)).rgb),po=Shoulder(o/d),pn=Shoulder(Lin(result.Load(int3(p,0)).rgb)/d);
 float3 e=log2(max(pn,1./1024)/max(po,1./1024));float l=dot(kW,e);float t=Trust(po);
 float g=log2(guard);float3 x=o*exp2(t*(clamp(l*intensity,-g,g)+min(intensity,1)*clamp(e-l,-chromaStops,chromaStops)));
 float y=dot(kW,max(o,0)),k=y>0?dot(kW,x)/y:1,kb=clamp(k,1/guard,guard);
 x*=k>0?kb/k:1;
 return lerp(o*kb,x,colourStrength);}
[numthreads(8,8,1)]void Resolve(uint3 id:SV_DispatchThreadID){
 uint ow,oh;output.GetDimensions(ow,oh);
 if(id.x>=ow||id.y>=oh)return;
 float4 r=result.Load(int3(id.xy,0));
 output[id.xy]=id.x>=w||id.y>=h?r:float4(Enc(Composed(id.xy)),r.a);}

groupshared float sumFinal[256],sumOriginal[256],maxOriginal[256];groupshared uint count[256];
[numthreads(16,16,1)]void Blocks(uint3 group:SV_GroupID,uint3 thread:SV_GroupThreadID){
 uint index=thread.y*16+thread.x;float f=0,s=0,m=0;uint n=0;float d=Divisor();
 for(uint y=0;y<2;++y)for(uint x=0;x<2;++x){uint2 p=group.xy*32+thread.xy*2+uint2(x,y);
  if(p.x<w&&p.y<h){float u=dot(kW,max(Lin(colour.Load(int3(p,0)).rgb),0))/d;
   f+=dot(kW,Composed(p))/d;s+=u;m=max(m,u);++n;}}
 sumFinal[index]=f;sumOriginal[index]=s;maxOriginal[index]=m;count[index]=n;
 GroupMemoryBarrierWithGroupSync();
 if(index!=0)return;
 float tf=0,ts=0,tm=0;uint tn=0;
 for(uint i=0;i<256;++i){tf+=sumFinal[i];ts+=sumOriginal[i];tm=max(tm,maxOriginal[i]);tn+=count[i];}
 float c=max(float(tn),1);blocks[group.xy]=float4(tf/c,ts/c,tm,0);}

[numthreads(8,8,1)]void Pedestal(uint3 id:SV_DispatchThreadID){
 if(id.x>=w||id.y>=h)return;
 int2 last=int2((w+31)/32,(h+31)/32)-1;float2 pos=(float2(id.xy)+.5)/32-.5;
 int2 b0=clamp(int2(floor(pos)),0,last),b1=min(b0+1,last);float2 f=saturate(pos-float2(b0));
 float4 m=blocks[b0]*(1-f.x)*(1-f.y)+blocks[int2(b1.x,b0.y)]*f.x*(1-f.y)+blocks[int2(b0.x,b1.y)]*(1-f.x)*f.y+
  blocks[b1]*f.x*f.y;
 if(m.z>darkGate)return;
 float3 x=max(Composed(id.xy)-clamp(m.x-m.y,0,pedestalCap)*Divisor(),0);
 output[id.xy]=float4(Enc(x),result.Load(int3(id.xy,0)).a);}
)";
class RenoComposition
{
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    // Descriptor sets in flight: the game's resources rotate and a pending list reads its descriptors when it runs.
    static constexpr UINT kSets = 8, kViews = 5;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> scale, resolve, blocks, pedestal;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    // commit (the governor's divisor, 1x1) and blockMeans rest in UNORDERED_ACCESS; output in
    // NON_PIXEL_SHADER_RESOURCE.
    Resource output, blockMeans, commit;
    Resource retired[kSets][2];
    UINT set = 0, idle = 0;
    static void Check(HRESULT hr)
    {
        if (FAILED(hr))
            throw std::runtime_error("AMD RenoDX composition D3D12 error " + std::to_string((UINT) hr));
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
    static void UavBarrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r)
    {
        D3D12_RESOURCE_BARRIER v {};
        v.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        v.UAV.pResource = r;
        c->ResourceBarrier(1, &v);
    }
    Resource Make(UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_STATES state)
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
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)));
        return r;
    }
    void Build()
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 },
                                             { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 2 } };
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, 16 };
        D3D12_ROOT_SIGNATURE_DESC rd {};
        rd.NumParameters = 2;
        rd.pParameters = params;
        Microsoft::WRL::ComPtr<ID3DBlob> b, e;
        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e));
        Check(device->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&root)));
        auto pipeline = [&](const char* entry, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
        {
            Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
            Check(D3DCompile(RenoCompositionShader, sizeof(RenoCompositionShader), "AMD RenoDX composition", nullptr,
                             nullptr, entry, "cs_5_0", 0, 0, &code, &errors));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
            pd.pRootSignature = root.Get();
            pd.CS = { code->GetBufferPointer(), code->GetBufferSize() };
            Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&out)));
        };
        pipeline("Scale", scale);
        pipeline("Resolve", resolve);
        pipeline("Blocks", blocks);
        pipeline("Pedestal", pedestal);
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSets * kViews;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
        commit = Make(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

  public:
    // Keeps the device only: the pipelines and the textures come with the first Record.
    explicit RenoComposition(ID3D12Device* d) : device(d) {}
    // Records the composition of result (NON_PIXEL_SHADER_RESOURCE, the frame's render subrect at its top left) over
    // f.colour and returns an FP16 texture of result's size in the same state. encoding is the transfer both are in
    // (1 linear, 2 sRGB, 3 gamma 2.2, as AmdEncoding); snap drops the governor's divisor to this frame's estimate,
    // seconds is the time since the last composed frame; intensity (0-2) and guard (1-8, a multiple) are the ReShade
    // add-on's. Throws before recording anything on failure.
    ID3D12Resource* Record(ID3D12GraphicsCommandList* c, const Frame& f, ID3D12Resource* result, UINT encoding,
                           bool snap, float seconds, float intensity, float guard, float colourStrength,
                           float chromaStops, bool removePedestal)
    {
        if (!root)
            Build();
        const UINT w = f.width, h = f.height;
        const auto size = result->GetDesc();
        for (auto& r : retired[set])
            r.Reset();
        if (!output || output->GetDesc().Width != size.Width || output->GetDesc().Height != size.Height)
        {
            retired[set][0] = std::move(output);
            output = Make(static_cast<UINT>(size.Width), size.Height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        const UINT bw = (w + 31) / 32, bh = (h + 31) / 32;
        if (!blockMeans || blockMeans->GetDesc().Width != bw || blockMeans->GetDesc().Height != bh)
        {
            retired[set][1] = std::move(blockMeans);
            blockMeans = Make(bw, bh, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(set) * kViews * stride;
        gpu.ptr += UINT64(set) * kViews * stride;
        set = (set + 1) % kSets;
        for (auto [r, format] : { std::pair { f.colour, ReadFormat(f.colour->GetDesc().Format) },
                                  std::pair { result, ReadFormat(size.Format) } })
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC v {};
            v.Format = format;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(r, &v, cpu);
            cpu.ptr += stride;
        }
        for (auto [r, format] : { std::pair { output.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT },
                                  std::pair { blockMeans.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT },
                                  std::pair { commit.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT } })
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u {};
            u.Format = format;
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(r, nullptr, &u, cpu);
            cpu.ptr += stride;
        }
        seconds = std::clamp(seconds, 0.f, .25f);
        struct
        {
            UINT w, h, encoding, flags;
            float preExposure, colourStrength, chromaStops, attack, release, darkGate, pedestalCap, intensity, guard;
            UINT pad[3];
        } k { w,
              h,
              std::clamp(encoding, 1u, 3u),
              (snap ? 1u : 0u) | (removePedestal ? 2u : 0u),
              std::isfinite(f.preExposure) && f.preExposure > 0 ? f.preExposure : 1.f,
              std::clamp(colourStrength, 0.f, 1.f),
              std::clamp(chromaStops, .25f, 2.f),
              4.f * seconds,
              .5f * seconds,
              .005f,
              .01f,
              std::clamp(intensity, 0.f, 2.f),
              std::clamp(guard, 1.f, 8.f),
              {} };
        static_assert(sizeof(k) == 16 * sizeof(UINT));
        const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Barrier(c, f.colour, f.colourState, read);
        Barrier(c, output.Get(), read, write);
        auto hh = heap.Get();
        c->SetDescriptorHeaps(1, &hh);
        c->SetComputeRootSignature(root.Get());
        c->SetComputeRootDescriptorTable(0, gpu);
        c->SetComputeRoot32BitConstants(1, 16, &k, 0);
        UavBarrier(c, commit.Get());
        c->SetPipelineState(scale.Get());
        c->Dispatch(1, 1, 1);
        UavBarrier(c, commit.Get());
        c->SetPipelineState(resolve.Get());
        c->Dispatch((static_cast<UINT>(size.Width) + 7) / 8, (size.Height + 7) / 8, 1);
        if (removePedestal)
        {
            UavBarrier(c, output.Get());
            c->SetPipelineState(blocks.Get());
            c->Dispatch(bw, bh, 1);
            UavBarrier(c, blockMeans.Get());
            c->SetPipelineState(pedestal.Get());
            c->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        }
        Barrier(c, output.Get(), write, read);
        Barrier(c, f.colour, read, f.colourState);
        idle = 0;
        return output.Get();
    }
    // A frame without the pass. The textures go once no list that used them can still be pending, the same depth
    // the descriptor sets rely on; the governor's divisor stays, and the next frame snaps anyway.
    void Idle()
    {
        if (output && ++idle > kSets)
        {
            output.Reset();
            blockMeans.Reset();
            for (auto& s : retired)
                for (auto& r : s)
                    r.Reset();
        }
    }
};
} // namespace AmdPreSr
