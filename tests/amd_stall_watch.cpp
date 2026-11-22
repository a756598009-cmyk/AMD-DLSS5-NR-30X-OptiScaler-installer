// StallWatch.h on WARP fences signalled from the CPU: a job trips once it was held past the limit, and a job behind
// a start fence the queue has not reached yet is not timed until it does.
#include "../OptiScaler/dlssnr/amd/StallWatch.h"
#include <dxgi1_4.h>
#include <cstdio>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

static int failures = 0;
static void Expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

int main()
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
    {
        std::printf("FAIL no WARP device\n");
        return 1;
    }
    ComPtr<ID3D12Fence> job, start;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&job));
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&start));
    auto& w = stallwatch::Watch::Get();
    w.limitMs.store(500);

    w.Arm(job.Get(), 1);
    Sleep(1000);
    Expect(w.Tripped() >= 500, "a job held past the limit trips");
    w.Clear();
    job->Signal(1);

    {
        stallwatch::Gate gate(start.Get(), 1);
        w.Arm(job.Get(), 2);
    }
    Sleep(1200);
    Expect(w.Tripped() == 0, "a job whose queue has not reached its start is not timed");
    start->Signal(1);
    Sleep(1200);
    Expect(w.Tripped() >= 500, "the same job trips once its queue started it");
    w.Clear();
    job->Signal(2);

    {
        stallwatch::Gate gate(start.Get(), 2);
        w.Arm(job.Get(), 3);
    }
    start->Signal(2);
    Sleep(200);
    job->Signal(3);
    Sleep(900);
    Expect(w.Tripped() == 0, "a job that finishes in time does not trip");

    w.Stop(false);
    return failures ? 1 : 0;
}
