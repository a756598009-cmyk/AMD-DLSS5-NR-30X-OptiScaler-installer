#pragma once
#include "AmdPreSr.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
namespace AmdPreSr
{
// Foveated NR: the network runs once on a smaller warped copy of the frame, full density in a centre zone, a
// smoothstep fall-off, a lower density toward the edges (per axis, so corners get edge x edge), with one temporal
// history; depth and motion are resampled onto the same grid and the answer is put back on the full frame as the
// network's change, upsampled edge-aware, so the game's own detail stays at every pixel and the centre is the
// network's answer unchanged. The warp is FoveatedDLSS5's (github.com/maximedgoulet/FoveatedDLSS5, MIT, Copyright (c)
// 2025 Carlos Lopez Jr., Licenses\FoveatedDLSS5_LICENSE.txt): same density profile and integral G, same Newton
// inverse, same motion rule.
// Here the edge density is solved so the working extent lands on an aligned size and the centre stays exactly 1:1,
// and the values travel as floats instead of a packed dword. FoveationAxis and the HLSL below are the two copies of
// the warp; change them together.
namespace Foveation
{
// Per axis, in units of the half-extent: a = centre half-width, b = fall-off length, e = edge density, g = G(1), the
// mean density (working extent / full extent). g == 1 is an identity axis.
struct Axis
{
    float a = 1, b = 0, e = 1, g = 1;
};
inline double G(double tau, const Axis& p)
{
    if (tau <= p.a)
        return tau;
    const double k = 1.0 - p.e, end = p.a + p.b, area = p.b * (1.0 - 0.5 * k);
    if (tau >= end)
        return p.a + area + p.e * (tau - end);
    const double q = (tau - p.a) / p.b, q2 = q * q;
    return p.a + p.b * (q - k * (q2 * q - 0.5 * q2 * q2));
}
// Normalized full position -> normalized working position (the shader's Warp1).
inline double Warp(double n, const Axis& p)
{
    if (p.g >= 1)
        return n;
    const double t = 2 * n - 1;
    return 0.5 + 0.5 * (t < 0 ? -1 : 1) * G(std::abs(t), p) / p.g;
}
// The edge density that makes G(1) = g with a and b fixed (G(1) is linear in it), or -1.
inline double SolveEdge(double a, double b, double g)
{
    if (b <= 0)
        return (g - a) / (1 - a);
    if (a + b <= 1)
        return (g - a - 0.5 * b) / (1 - a - 0.5 * b);
    const double q = (1 - a) / b, s = q * q * q - 0.5 * q * q * q * q;
    return 1 - (a + b * q - g) / (b * s);
}
struct Params
{
    float centreWidth = .5f, centreHeight = .6f; // share of the screen at full density
    float falloff = .12f;                        // fall-off length on every side, share of the screen height
    float edge = .5f;                            // density past the fall-off, per axis
};
// 1 light, 2 medium, 3 strong; NR pixels at 16:9 ~82 / 67 / 48%.
inline Params Preset(int preset)
{
    switch (preset)
    {
    case 1:
        return { .60f, .70f, .15f, .60f };
    case 3:
        return { .40f, .50f, .10f, .35f };
    default:
        return {};
    }
}
inline constexpr float kMinEdge = .2f;
// Below this the extra passes cost more than the pixels they save.
inline constexpr double kMinSaving = .05;
struct Grid
{
    UINT width = 0, height = 0; // full extent
    UINT workWidth = 0, workHeight = 0;
    Axis x, y;
    bool Active() const { return workWidth != width || workHeight != height; }
    double Share() const { return double(workWidth) * workHeight / (double(width) * height); }
    bool operator==(const Grid& o) const
    {
        return width == o.width && height == o.height && workWidth == o.workWidth && workHeight == o.workHeight;
    }
};
// One axis of `full` pixels: the working extent is the profile's mean density times full, on the multiple of
// `align` (the network's padding step, so no padded pixel is paid for) either side of it whose solved edge density is
// nearest the asked one (one less for an odd extent: the margin must be even so the centre lands on whole texels).
// Identity when neither fits.
inline Axis MakeAxis(UINT full, UINT& work, double centre, double falloffPx, double edge, UINT align)
{
    Axis p;
    work = full;
    const double half = 0.5 * full;
    p.a = float(std::clamp(centre, 0.0, 1.0));
    p.b = float(std::clamp(falloffPx / half, 0.0, 2.0));
    edge = std::clamp(edge, double(kMinEdge), 1.0);
    if (p.a >= 1 || edge >= 1 || full < 64)
        return Axis {};
    Axis want = p;
    want.e = float(edge);
    const UINT low = UINT(std::floor(G(1, want) * full / align)) * align;
    double best = 2;
    for (UINT w : { low, low + align })
    {
        // An odd extent (1707 render pixels at 2560 Quality) takes one pixel less: the margin stays even and the
        // network pads to the same step.
        if (w && ((full - w) & 1))
            --w;
        const double e = SolveEdge(p.a, p.b, double(w) / full);
        if (w == 0 || w >= full || e < kMinEdge || e >= 1 || std::abs(e - edge) >= best)
            continue;
        best = std::abs(e - edge);
        work = w;
    }
    if (work == full)
        return Axis {};
    p.g = float(double(work) / full);
    p.e = float(SolveEdge(p.a, p.b, p.g));
    return p;
}
inline Grid MakeGrid(UINT width, UINT height, const Params& params, UINT align = 64)
{
    Grid grid;
    grid.width = grid.workWidth = width;
    grid.height = grid.workHeight = height;
    const double falloffPx = double(params.falloff) * height;
    grid.x = MakeAxis(width, grid.workWidth, params.centreWidth, falloffPx, params.edge, align);
    grid.y = MakeAxis(height, grid.workHeight, params.centreHeight, falloffPx, params.edge, align);
    if (grid.Share() > 1 - kMinSaving)
    {
        grid.x = grid.y = {};
        grid.workWidth = width;
        grid.workHeight = height;
    }
    return grid;
}

inline constexpr char Shader[] = R"(
Texture2D<float4> colour:register(t0);Texture2D<float> depth:register(t1);Texture2D<float2> motion:register(t2);
Texture2D<float4> warped:register(t3);Texture2D<float4> result:register(t4);
RWTexture2D<float4> warpedOut:register(u0);RWTexture2D<float2> motionOut:register(u1);RWTexture2D<float> depthOut:register(u2);
RWTexture2D<float4> output:register(u3);
cbuffer Params:register(b0){uint2 full,work,guide;float2 motionExtent,motionScale;uint flags;float sigma;
 float4 ax,ay;float2 jitterStep;}
// One axis, p = (a, b, e, g): the density integral G over tau = |2n - 1|, its inverse (Newton in the fall-off only:
// the ramp's slope is at least e), and the maps between full (n) and working (v) normalized positions.
float G(float t,float4 p){if(t<=p.x)return t;float k=1-p.z,area=p.y*(1-.5*k);
 if(t>=p.x+p.y)return p.x+area+p.z*(t-p.x-p.y);float q=(t-p.x)/p.y,q2=q*q;return p.x+p.y*(q-k*(q2*q-.5*q2*q2));}
float GInv(float g,float4 p){if(g<=p.x)return g;float k=1-p.z,area=p.y*(1-.5*k);
 if(g>=p.x+area)return p.x+p.y+(g-p.x-area)/p.z;float r=(g-p.x)/p.y,q=r;
 [unroll]for(int i=0;i<4;++i){float q2=q*q;q=saturate(q-(q-k*(q2*q-.5*q2*q2)-r)/(1-k*(3*q2-2*q2*q)));}
 return p.x+p.y*q;}
float Warp1(float n,float4 p){if(p.w>=1)return n;float t=2*n-1;return .5+.5*sign(t)*G(abs(t),p)/p.w;}
float Unwarp1(float v,float4 p){if(p.w>=1)return v;float s=2*v-1;return .5+.5*sign(s)*GInv(abs(s)*p.w,p);}
float2 Warp(float2 n){return float2(Warp1(n.x,ax),Warp1(n.y,ay));}
float2 Unwarp(float2 v){return float2(Unwarp1(v.x,ax),Unwarp1(v.y,ay));}
float Snap(float x){float r=round(x);return abs(x-r)<2e-3?r:x;}

// Colour: the full pixels under the working texel, by coverage (one pixel, exactly, in the centre).
[numthreads(8,8,1)]void WarpColour(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=work))return;
 float2 lo=float2(Unwarp1(float(id.x)/work.x,ax),Unwarp1(float(id.y)/work.y,ay))*full;
 float2 hi=float2(Unwarp1(float(id.x+1)/work.x,ax),Unwarp1(float(id.y+1)/work.y,ay))*full;
 lo=clamp(float2(Snap(lo.x),Snap(lo.y)),0,full);hi=clamp(float2(Snap(hi.x),Snap(hi.y)),0,full);
 float4 sum=0;float total=0;int2 a=int2(floor(lo)),b=min(int2(ceil(hi)),int2(full));
 for(int y=a.y;y<b.y&&y<a.y+8;++y){float cy=min(hi.y,y+1)-max(lo.y,y);
  for(int x=a.x;x<b.x&&x<a.x+8;++x){float c=cy*(min(hi.x,x+1)-max(lo.x,x));sum+=c*colour.Load(int3(x,y,0));total+=c;}}
 warpedOut[id.xy]=sum/max(total,1e-6);}

// Guides at the working texel's centre: depth nearest (a blend would invent a surface), motion carried through the
// warp (previous = current + vector + the jitter step, in full pixels), in working pixels at scale 1.
[numthreads(8,8,1)]void WarpGuides(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=work))return;
 float2 v=(float2(id.xy)+.5)/work,n=Unwarp(v);
 uint2 t=min(uint2(saturate(n)*guide),guide-1);
 if(flags&1)depthOut[id.xy]=depth.Load(int3(t,0));
 float2 m=0;
 uint2 tm=min(uint2(saturate(n)*motionExtent),uint2(motionExtent)-1);
 if(flags&2)m=(Warp(n+motion.Load(int3(tm,0))*motionScale/motionExtent+jitterStep/full)-v)*work;
 motionOut[id.xy]=m;}

// Back on the full frame: the game's pixel plus the network's change around its working position, joint bilateral
// on the warped input's luminance against this pixel's. In the centre the position is a texel and this is the answer.
float L(float3 c){float y=max(dot(c,float3(.2126,.7152,.0722)),0);return y/(1+y);}
[numthreads(8,8,1)]void UnwarpResult(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=full))return;
 float4 c=colour.Load(int3(id.xy,0));
 float2 p=Warp((float2(id.xy)+.5)/full)*work-.5;p=float2(Snap(p.x),Snap(p.y));
 int2 b=int2(floor(p));float2 f=p-b;float y=L(c.rgb);
 float4 sum=0,plain=0;float total=0;
 [unroll]for(int j=0;j<4;++j){int2 o=int2(j&1,j>>1),q=clamp(b+o,0,int2(work)-1);
  float wb=(o.x?f.x:1-f.x)*(o.y?f.y:1-f.y);if(wb<=0)continue;
  float4 w=warped.Load(int3(q,0)),d=result.Load(int3(q,0))-w;
  float r=(L(w.rgb)-y)/sigma,wr=wb*exp(-r*r);
  sum+=wr*d;total+=wr;plain+=wb*d;}
 output[id.xy]=c+(total>1e-4?sum/total:plain);}
)";

class Passes
{
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    static constexpr UINT kSets = 8, kViews = 9;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> warpColour, warpGuides, unwarp;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    // At rest: warped colour, motion and depth in NON_PIXEL_SHADER_RESOURCE, output too.
    Resource warpedColour, warpedMotion, warpedDepth, output;
    Resource retired[kSets][4];
    UINT set = 0, idle = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE table {};
    struct Constants
    {
        UINT full[2], work[2], guide[2];
        float motionExtent[2], motionScale[2];
        UINT flags;
        float sigma;
        Axis x, y;
        float jitterStep[2];
    } k {};
    static_assert(sizeof(Constants) == 22 * 4);
    static void Check(HRESULT hr)
    {
        if (FAILED(hr))
            throw std::runtime_error("foveated NR D3D12 error " + std::to_string((UINT) hr));
    }
    static void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                        D3D12_RESOURCE_STATES b)
    {
        if (!r || a == b)
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
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                              IID_PPV_ARGS(&r)));
        return r;
    }
    void Fit(Resource& r, UINT slot, UINT w, UINT h, DXGI_FORMAT format)
    {
        if (r && r->GetDesc().Width == w && r->GetDesc().Height == h && r->GetDesc().Format == format)
            return;
        retired[set][slot] = std::move(r);
        r = Make(w, h, format);
    }
    void Build()
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0 },
                                             { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, 5 } };
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
            const HRESULT hr = D3DCompile(Shader, sizeof(Shader), "foveated NR", nullptr, nullptr, entry, "cs_5_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (FAILED(hr))
                throw std::runtime_error(std::string("foveated NR shader: ") +
                                         (errors ? (const char*) errors->GetBufferPointer() : entry));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
            pd.pRootSignature = root.Get();
            pd.CS = { code->GetBufferPointer(), code->GetBufferSize() };
            Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&out)));
        };
        pipeline("WarpColour", warpColour);
        pipeline("WarpGuides", warpGuides);
        pipeline("UnwarpResult", unwarp);
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSets * kViews;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    }
    void Srv(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, ID3D12Resource* r, DXGI_FORMAT format, UINT stride)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC v {};
        v.Format = format;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(r, &v, cpu);
        cpu.ptr += stride;
    }
    void Uav(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, ID3D12Resource* r, DXGI_FORMAT format, UINT stride)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u {};
        u.Format = format;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(r, nullptr, &u, cpu);
        cpu.ptr += stride;
    }
    // The warped colour keeps the game's kind of format (the runtimes read UNORM as display colour and float as scene
    // colour, so the format is part of the input), in one with typed UAV stores everywhere. sRGB formats read
    // linear, so they go to FP16, scene colour.
    static DXGI_FORMAT WarpFormat(DXGI_FORMAT f)
    {
        switch (f = ReadFormat(f))
        {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return f;
        default:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        }
    }

  public:
    explicit Passes(ID3D12Device* d) : device(d) {}
    // Before SR: the step from the last frame's jitter to this one's in full pixels, folded into the warped motion
    // (the caller then hands the runtime no jitter), and the extent the warped textures are allocated at, so a smaller frame
    // under dynamic resolution is a subrect of the same textures.
    struct Options
    {
        float jitterStepX = 0, jitterStepY = 0;
        UINT allocWidth = 0, allocHeight = 0;
        float sigma = .08f;
    };
    // Records the warp of f (colour, and depth and motion when it has them) onto grid's working extent and returns
    // the frame the runtime gets instead: our textures, guides at the colour's extent and scale 1. f's resources end
    // in the states they came in. The warped textures stay valid until the next Warp.
    Frame Warp(ID3D12GraphicsCommandList* c, const Frame& f, const Grid& grid, const Options& o = {})
    {
        if (!root)
            Build();
        for (auto& r : retired[set])
            r.Reset();
        const UINT w = grid.workWidth, h = grid.workHeight;
        const UINT aw = (std::max)(w, o.allocWidth), ah = (std::max)(h, o.allocHeight);
        const DXGI_FORMAT colourFormat = ReadFormat(f.colour->GetDesc().Format);
        Fit(warpedColour, 0, aw, ah, WarpFormat(colourFormat));
        Fit(warpedMotion, 1, aw, ah, DXGI_FORMAT_R16G16_FLOAT);
        Fit(warpedDepth, 2, aw, ah, DXGI_FORMAT_R32_FLOAT);
        const DXGI_FORMAT depthFormat = f.depth ? DepthReadFormat(f.depth->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;
        ID3D12Resource* depth = depthFormat != DXGI_FORMAT_UNKNOWN ? f.depth : nullptr;
        const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        table = heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(set) * kViews * stride;
        table.ptr += UINT64(set) * kViews * stride;
        set = (set + 1) % kSets;
        Srv(cpu, f.colour, colourFormat, stride);
        Srv(cpu, depth, depth ? depthFormat : DXGI_FORMAT_R32_FLOAT, stride);
        Srv(cpu, f.motion, f.motion ? ReadFormat(f.motion->GetDesc().Format) : DXGI_FORMAT_R16G16_FLOAT, stride);
        Srv(cpu, warpedColour.Get(), warpedColour->GetDesc().Format, stride);
        cpu.ptr += stride; // t4, the result, comes with Unwarp
        Uav(cpu, warpedColour.Get(), warpedColour->GetDesc().Format, stride);
        Uav(cpu, warpedMotion.Get(), DXGI_FORMAT_R16G16_FLOAT, stride);
        Uav(cpu, warpedDepth.Get(), DXGI_FORMAT_R32_FLOAT, stride);
        const UINT gw = f.afterUpscale && f.guideWidth ? f.guideWidth : f.width;
        const UINT gh = f.afterUpscale && f.guideHeight ? f.guideHeight : f.height;
        k = { { f.width, f.height },
              { w, h },
              { (std::max)(gw, 1u), (std::max)(gh, 1u) },
              { float(f.motionWidth ? f.motionWidth : gw), float(f.motionHeight ? f.motionHeight : gh) },
              { f.motionScaleX, f.motionScaleY },
              (depth ? 1u : 0u) | (f.motion ? 2u : 0u),
              o.sigma,
              grid.x,
              grid.y,
              { o.jitterStepX, o.jitterStepY } };
        const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Barrier(c, f.colour, f.colourState, read);
        Barrier(c, depth, f.depthState, read);
        Barrier(c, f.motion, f.motionState, read);
        for (auto r : { warpedColour.Get(), warpedMotion.Get(), warpedDepth.Get() })
            Barrier(c, r, read, write);
        auto hh = heap.Get();
        c->SetDescriptorHeaps(1, &hh);
        c->SetComputeRootSignature(root.Get());
        c->SetComputeRootDescriptorTable(0, table);
        c->SetComputeRoot32BitConstants(1, sizeof(k) / 4, &k, 0);
        c->SetPipelineState(warpColour.Get());
        c->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        c->SetPipelineState(warpGuides.Get());
        c->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        for (auto r : { warpedColour.Get(), warpedMotion.Get(), warpedDepth.Get() })
            Barrier(c, r, write, read);
        Barrier(c, f.colour, read, f.colourState);
        Barrier(c, depth, read, f.depthState);
        Barrier(c, f.motion, read, f.motionState);
        idle = 0;
        Frame out = f;
        out.colour = warpedColour.Get();
        out.colourState = read;
        out.width = w;
        out.height = h;
        out.motion = f.motion ? warpedMotion.Get() : nullptr;
        out.motionState = read;
        out.motionScaleX = out.motionScaleY = 1;
        out.motionWidth = out.motionHeight = 0;
        out.depth = depth ? warpedDepth.Get() : nullptr;
        out.depthState = read;
        out.afterUpscale = false;
        out.guideWidth = out.guideHeight = 0;
        out.fixedWidth = out.fixedHeight = 0;
        return out;
    }
    // After the runtime's result (NON_PIXEL_SHADER_RESOURCE, the working extent at its top left) for the frame the
    // last Warp made, on the same descriptor set: f is that Warp's full frame, whose colour must still hold the same
    // picture. Returns an FP16 texture of f's extent (allocated at least at allocWidth x allocHeight, f's at the top
    // left) in NON_PIXEL_SHADER_RESOURCE.
    ID3D12Resource* Unwarp(ID3D12GraphicsCommandList* c, const Frame& f, ID3D12Resource* result, UINT allocWidth = 0,
                           UINT allocHeight = 0)
    {
        Fit(output, 3, (std::max)(f.width, allocWidth), (std::max)(f.height, allocHeight),
            DXGI_FORMAT_R16G16B16A16_FLOAT);
        const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const UINT used = (set + kSets - 1) % kSets;
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(used) * kViews * stride + 4 * stride;
        Srv(cpu, result, ReadFormat(result->GetDesc().Format), stride);
        cpu.ptr += 3 * stride;
        Uav(cpu, output.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, stride);
        const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Barrier(c, f.colour, f.colourState, read);
        Barrier(c, output.Get(), read, write);
        auto hh = heap.Get();
        c->SetDescriptorHeaps(1, &hh);
        c->SetComputeRootSignature(root.Get());
        c->SetComputeRootDescriptorTable(0, table);
        c->SetComputeRoot32BitConstants(1, sizeof(k) / 4, &k, 0);
        c->SetPipelineState(unwarp.Get());
        c->Dispatch((f.width + 7) / 8, (f.height + 7) / 8, 1);
        Barrier(c, output.Get(), write, read);
        Barrier(c, f.colour, read, f.colourState);
        return output.Get();
    }
    // A frame without foveation: the textures go once no list that used them can still be pending.
    void Idle()
    {
        if (warpedColour && ++idle > kSets)
        {
            warpedColour.Reset();
            warpedMotion.Reset();
            warpedDepth.Reset();
            output.Reset();
            for (auto& s : retired)
                for (auto& r : s)
                    r.Reset();
        }
    }
};
} // namespace Foveation
} // namespace AmdPreSr
