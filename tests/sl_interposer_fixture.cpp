// A stand-in for Streamline's sl.interposer.dll for tests\lmxxf_interposer_wrap.cpp: it passes the caller's
// CreateCommandList and CreateCommandList1 on to the device, as the interposer does, or makes a list on a thread of its
// own, with no caller above. Built without optimisation so the calls stay calls, not jumps.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

extern "C" __declspec(dllexport) HRESULT FixtureCreateList(ID3D12Device* device, ID3D12CommandAllocator* alloc,
                                                           void** out)
{
    const HRESULT hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                                 __uuidof(ID3D12GraphicsCommandList), out);
    return hr;
}

extern "C" __declspec(dllexport) HRESULT FixtureCreateList1(ID3D12Device* device, void** out)
{
    ID3D12Device4* device4 = nullptr;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&device4));
    if (FAILED(hr))
        return hr;
    hr = device4->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                     __uuidof(ID3D12GraphicsCommandList), out);
    device4->Release();
    return hr;
}

struct Job
{
    ID3D12Device* device;
    ID3D12CommandAllocator* alloc;
    void* out;
    HRESULT hr;
};

static DWORD WINAPI Own(void* p)
{
    auto* job = static_cast<Job*>(p);
    job->hr = job->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, job->alloc, nullptr,
                                             __uuidof(ID3D12GraphicsCommandList), &job->out);
    return 0;
}

extern "C" __declspec(dllexport) HRESULT FixtureCreateOwnList(ID3D12Device* device, ID3D12CommandAllocator* alloc,
                                                              void** out)
{
    Job job { device, alloc, nullptr, E_FAIL };
    HANDLE thread = CreateThread(nullptr, 0, Own, &job, 0, nullptr);
    if (!thread)
        return E_FAIL;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    *out = job.out;
    return job.hr;
}

// Calls back into the test, which then asks whose call it is.
extern "C" __declspec(dllexport) void* FixtureCall(void* (*fn)())
{
    void* result = fn();
    return result;
}
