#pragma once
#include "dlssnr/amd/AmdPreSr.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <stdexcept>
#include <string>

// NR on OpenRR's output (NrImage=output): the network reads RR's finished picture scaled to the render size, which has
// no jitter in it, and its change comes back on that picture. The change is kept as a log delta on the render grid with a
// history of its own: reprojected with the game's vectors (Catmull-Rom, so a low rate does not blur it) and blended at a
// low rate, except where the picture says the history is of something else: the reprojected luminance outside this
// frame's range around the pixel (a moved edge, a disocclusion) takes this frame's delta, so nothing ghosts. Then it is
// enlarged to the picture joint bilateral on its luminance. Log as OpenRR's post: an edit scales with its pixel and the
// sum stays positive.
namespace OpenNrRrOutputNr
{
inline constexpr char Shader[] = R"(
Texture2D<float4> picture:register(t0);Texture2D<float2> motion:register(t1);Texture2D<float4> reduced:register(t2);
Texture2D<float4> result:register(t3);Texture2D<float4> delta:register(t4);Texture2D<float4> past:register(t5);
Texture2D<float4> kept:register(t6);
RWTexture2D<float4> reducedOut:register(u0);RWTexture2D<float4> deltaOut:register(u1);RWTexture2D<float4> keptOut:register(u2);
RWTexture2D<float4> pictureOut:register(u3);
cbuffer Params:register(b0){uint2 full,work;float2 motionExtent,motionScale;float logScale,sigma,alphaStill,alphaMove,clip,reject0,reject1;uint fresh;}

float3 Enc(float3 x){return log2(max(x,0)*logScale+1);}
float3 Dec(float3 v){return (exp2(v)-1)/logScale;}

// The picture's pixels under a render pixel, by coverage.
[numthreads(8,8,1)]void Reduce(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=work))return;
 float2 s=float2(full)/work,lo=id.xy*s,hi=(id.xy+1)*s;
 float4 sum=0;float total=0;int2 a=int2(floor(lo)),b=min(int2(ceil(hi)),int2(full));
 for(int y=a.y;y<b.y&&y<a.y+8;++y){float cy=min(hi.y,y+1)-max(lo.y,y);
  for(int x=a.x;x<b.x&&x<a.x+8;++x){float c=cy*(min(hi.x,x+1)-max(lo.x,x));sum+=c*picture.Load(int3(x,y,0));total+=c;}}
 reducedOut[id.xy]=sum/max(total,1e-6);}

// This frame's change, and the picture's log luminance in alpha.
[numthreads(8,8,1)]void Delta(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=work))return;
 float3 i=Enc(reduced.Load(int3(id.xy,0)).rgb);
 deltaOut[id.xy]=float4(Enc(result.Load(int3(id.xy,0)).rgb)-i,dot(i,float3(.2126,.7152,.0722)));}

// Catmull-Rom on the 12 taps without the corners, at a pixel position.
float4 PastAt(float2 p){
 float2 b=floor(p-.5),f=p-.5-b;int2 i=int2(b);
 float2 w0=f*(f*(2-f)-1)*.5,w1=(f*f*(3*f-5)+2)*.5,w2=f*(f*(4-3*f)+1)*.5,w3=f*f*(f-1)*.5;
 float wx[4]={w0.x,w1.x,w2.x,w3.x},wy[4]={w0.y,w1.y,w2.y,w3.y};float4 sum=0;float total=0;int2 top=int2(work)-1;
 [unroll]for(int y=0;y<4;++y)[unroll]for(int x=0;x<4;++x){
  if((x==0||x==3)&&(y==0||y==3))continue;
  float w=wx[x]*wy[y];sum+=w*past.Load(int3(clamp(i+int2(x-1,y-1),0,top),0));total+=w;}
 return sum/total;}

[numthreads(8,8,1)]void History(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=work))return;
 float4 d=delta.Load(int3(id.xy,0));
 float2 n=(float2(id.xy)+.5)/work;
 uint2 tm=min(uint2(n*motionExtent),uint2(motionExtent)-1);
 float2 mv=motion.Load(int3(tm,0))*motionScale/motionExtent;
 float2 q=n+mv;
 if(fresh||any(q<0)||any(q>1)){keptOut[id.xy]=d;return;}
 // this frame's delta around the pixel, mean and spread, and the picture's range
 float3 m1=0,m2=0;float lo=d.a,up=d.a;int2 top=int2(work)-1;
 [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
  float4 v=delta.Load(int3(clamp(int2(id.xy)+int2(x,y),0,top),0));m1+=v.rgb;m2+=v.rgb*v.rgb;lo=min(lo,v.a);up=max(up,v.a);}
 m1/=9;float3 g=clip*sqrt(max(m2/9-m1*m1,0));
 float4 h=PastAt(q*work);
 h.rgb=clamp(h.rgb,m1-g,m1+g);
 float2 px=mv*work;float a=lerp(alphaStill,alphaMove,saturate(2*sqrt(dot(px,px))));
 a=max(a,saturate((max(max(h.a-up,lo-h.a),0)-reject0)/(reject1-reject0)));
 float3 r=lerp(h.rgb,d.rgb,a);
 keptOut[id.xy]=float4(any(isnan(r))?d.rgb:r,d.a);}

// The kept delta on the picture.
float L(float3 c){float y=max(dot(c,float3(.2126,.7152,.0722)),0);return y/(1+y);}
[numthreads(8,8,1)]void Compose(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=full))return;
 float4 c=picture.Load(int3(id.xy,0));
 float2 p=(float2(id.xy)+.5)/full*work-.5;
 int2 b=int2(floor(p));float2 f=p-b;float y=L(c.rgb);
 float3 sum=0,plain=0;float total=0;
 [unroll]for(int j=0;j<4;++j){int2 o=int2(j&1,j>>1),q=clamp(b+o,0,int2(work)-1);
  float wb=(o.x?f.x:1-f.x)*(o.y?f.y:1-f.y);if(wb<=0)continue;
  float3 w=reduced.Load(int3(q,0)).rgb,d=kept.Load(int3(q,0)).rgb;
  float r=(L(w)-y)/sigma,wr=wb*exp(-r*r);
  sum+=wr*d;total+=wr;plain+=wb*d;}
 pictureOut[id.xy]=float4(Dec(Enc(c.rgb)+(total>1e-4?sum/total:plain)),c.a);}
)";

struct Options
{
    float logScale = 100.f;
    float sigma = .08f;
    float alphaStill = .1f, alphaMove = .2f; // history rate, still and from half a render pixel of motion up
    float clip = 100.f;                       // the past delta's range, in spreads of this frame's around the pixel
    float reject0 = .25f, reject1 = .75f;       // how far outside the picture's range (log2) the history starts to go, is gone
};

// Formats the picture may have here: a typed UAV store on every GPU this runs on. Anything else stays on the other hop.
inline bool PictureFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return true;
    default:
        return false;
    }
}

class Passes
{
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    static constexpr UINT kSets = 8, kViews = 11, kSlots = 5;
    static constexpr DXGI_FORMAT kFp16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> reduce, deltaPass, historyPass, compose;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    // At rest in NON_PIXEL_SHADER_RESOURCE.
    Resource reducedTex, deltaTex, keptTex[2], composed;
    Resource retired[kSets][kSlots];
    UINT set = 0, used = 0, parity = 0;
    bool fresh = true;
    struct Constants
    {
        UINT full[2], work[2];
        float motionExtent[2], motionScale[2];
        float logScale, sigma, alphaStill, alphaMove, clip, reject0, reject1;
        UINT fresh;
    } k {};
    static_assert(sizeof(Constants) == 16 * 4);
    static void Check(HRESULT hr)
    {
        if (FAILED(hr))
            throw std::runtime_error("NR on the output: D3D12 error " + std::to_string((UINT) hr));
    }
    static void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        if (!r || a == b)
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
    void Fit(Resource& r, UINT slot, UINT w, UINT h, DXGI_FORMAT format)
    {
        if (r && r->GetDesc().Width == w && r->GetDesc().Height == h && r->GetDesc().Format == format)
            return;
        fresh = true;
        retired[set][slot] = std::move(r);
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
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                              nullptr, IID_PPV_ARGS(&r)));
    }
    void Build()
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 0, 0, 0 },
                                             { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, 7 } };
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, sizeof(Constants) / 4 };
        D3D12_ROOT_SIGNATURE_DESC rd {};
        rd.NumParameters = 2;
        rd.pParameters = params;
        Microsoft::WRL::ComPtr<ID3DBlob> b, e;
        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e));
        Check(device->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&root)));
        auto pipeline = [&](const char* entry, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
        {
            Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
            const HRESULT hr = D3DCompile(Shader, sizeof(Shader), "NR on the output", nullptr, nullptr, entry, "cs_5_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (FAILED(hr))
                throw std::runtime_error(std::string("NR on the output, shader: ") +
                                         (errors ? (const char*) errors->GetBufferPointer() : entry));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
            pd.pRootSignature = root.Get();
            pd.CS = { code->GetBufferPointer(), code->GetBufferSize() };
            Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&out)));
        };
        pipeline("Reduce", reduce);
        pipeline("Delta", deltaPass);
        pipeline("History", historyPass);
        pipeline("Compose", compose);
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSets * kViews;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT s, UINT slot) const
    {
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(s * kViews + slot) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return cpu;
    }
    void Srv(UINT s, UINT slot, ID3D12Resource* r, DXGI_FORMAT format)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC v {};
        v.Format = format;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(r, &v, Cpu(s, slot));
    }
    void Uav(UINT s, UINT slot, ID3D12Resource* r, DXGI_FORMAT format)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u {};
        u.Format = format;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(r, nullptr, &u, Cpu(s, 7 + slot));
    }
    void Bind(ID3D12GraphicsCommandList* c)
    {
        auto hh = heap.Get();
        c->SetDescriptorHeaps(1, &hh);
        c->SetComputeRootSignature(root.Get());
        auto table = heap->GetGPUDescriptorHandleForHeapStart();
        table.ptr += UINT64(used * kViews) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        c->SetComputeRootDescriptorTable(0, table);
        c->SetComputeRoot32BitConstants(1, sizeof(k) / 4, &k, 0);
    }

  public:
    explicit Passes(ID3D12Device* d) : device(d) {}
    // The next frame starts the history from its own delta (a frame without NR, a cut).
    void Invalidate() { fresh = true; }
    // Records the picture (f.colour, f.width x f.height, its state f.colourState) scaled to w x h. Returns the RGBA16F
    // texture the network reads instead, in NON_PIXEL_SHADER_RESOURCE, valid until the next Reduce.
    ID3D12Resource* Reduce(ID3D12GraphicsCommandList* c, const AmdPreSr::Frame& f, UINT w, UINT h)
    {
        if (!root)
            Build();
        for (auto& r : retired[set])
            r.Reset();
        used = set;
        set = (set + 1) % kSets;
        Fit(reducedTex, 0, w, h, kFp16);
        k.full[0] = f.width, k.full[1] = f.height, k.work[0] = w, k.work[1] = h;
        const DXGI_FORMAT format = AmdPreSr::ReadFormat(f.colour->GetDesc().Format);
        Srv(used, 0, f.colour, format);
        Srv(used, 1, nullptr, DXGI_FORMAT_R16G16_FLOAT);
        Srv(used, 2, nullptr, kFp16);
        Srv(used, 3, nullptr, kFp16);
        Srv(used, 4, nullptr, kFp16);
        Srv(used, 5, nullptr, kFp16);
        Srv(used, 6, nullptr, kFp16);
        Uav(used, 0, reducedTex.Get(), kFp16);
        Uav(used, 1, nullptr, kFp16);
        Uav(used, 2, nullptr, kFp16);
        Uav(used, 3, nullptr, kFp16);
        const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Barrier(c, f.colour, f.colourState, read);
        Barrier(c, reducedTex.Get(), read, write);
        Bind(c);
        c->SetPipelineState(reduce.Get());
        c->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Barrier(c, reducedTex.Get(), write, read);
        Barrier(c, f.colour, read, f.colourState);
        return reducedTex.Get();
    }
    // After the network's result (the reduced texture's extent at its top left, NON_PIXEL_SHADER_RESOURCE) for the last
    // Reduce: f is that Reduce's picture, with the game's vectors. Returns the picture with the NR, of outFormat (typed
    // UAV stores, PictureFormat), f's extent, in NON_PIXEL_SHADER_RESOURCE.
    ID3D12Resource* Compose(ID3D12GraphicsCommandList* c, const AmdPreSr::Frame& f, ID3D12Resource* result, DXGI_FORMAT outFormat,
                            bool reset, const Options& o = {})
    {
        const UINT w = k.work[0], h = k.work[1];
        Fit(deltaTex, 1, w, h, kFp16);
        Fit(keptTex[0], 2, w, h, kFp16);
        Fit(keptTex[1], 3, w, h, kFp16);
        Fit(composed, 4, f.width, f.height, outFormat);
        k.motionExtent[0] = float(f.motionWidth ? f.motionWidth : w);
        k.motionExtent[1] = float(f.motionHeight ? f.motionHeight : h);
        k.motionScale[0] = f.motionScaleX;
        k.motionScale[1] = f.motionScaleY;
        k.logScale = o.logScale;
        k.sigma = o.sigma;
        k.alphaStill = o.alphaStill;
        k.alphaMove = o.alphaMove;
        k.clip = o.clip;
        k.reject0 = o.reject0;
        k.reject1 = o.reject1;
        k.fresh = (fresh || reset || !f.motion) ? 1 : 0;
        fresh = false;
        ID3D12Resource* past = keptTex[parity].Get();
        ID3D12Resource* now = keptTex[parity ^ 1].Get();
        parity ^= 1;
        Srv(used, 1, f.motion, f.motion ? AmdPreSr::ReadFormat(f.motion->GetDesc().Format) : DXGI_FORMAT_R16G16_FLOAT);
        Srv(used, 2, reducedTex.Get(), kFp16);
        Srv(used, 3, result, AmdPreSr::ReadFormat(result->GetDesc().Format));
        Srv(used, 4, deltaTex.Get(), kFp16);
        Srv(used, 5, past, kFp16);
        Srv(used, 6, now, kFp16);
        Uav(used, 1, deltaTex.Get(), kFp16);
        Uav(used, 2, now, kFp16);
        Uav(used, 3, composed.Get(), outFormat);
        const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Barrier(c, f.colour, f.colourState, read);
        Barrier(c, f.motion, f.motionState, read);
        Barrier(c, deltaTex.Get(), read, write);
        Bind(c);
        c->SetPipelineState(deltaPass.Get());
        c->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Barrier(c, deltaTex.Get(), write, read);
        Barrier(c, now, read, write);
        c->SetPipelineState(historyPass.Get());
        c->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Barrier(c, now, write, read);
        Barrier(c, composed.Get(), read, write);
        c->SetPipelineState(compose.Get());
        c->Dispatch((f.width + 7) / 8, (f.height + 7) / 8, 1);
        Barrier(c, composed.Get(), write, read);
        Barrier(c, f.motion, read, f.motionState);
        Barrier(c, f.colour, read, f.colourState);
        return composed.Get();
    }
};
} // namespace OpenNrRrOutputNr
