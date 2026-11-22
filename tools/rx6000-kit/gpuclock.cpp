// gpuclock <seconds> [stop file]: samples the AMD GPUs' graphics clock, activity, power and temperature through the
// driver's display library (atiadlxx.dll, ADL2_New_QueryPMLogData_Get) every 100 ms, until <seconds> pass or the
// stop file appears. Read-only: it sets nothing. One "SAMPLE" line a GPU a sample, then a "CLOCK" summary per GPU
// over the samples with graphics activity of 50% or more (the GPU busy with the network).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

struct AdapterInfo // ADL's, Windows layout
{
    int iSize, iAdapterIndex;
    char strUDID[256];
    int iBusNumber, iDeviceNumber, iFunctionNumber, iVendorID;
    char strAdapterName[256], strDisplayName[256];
    int iPresent, iExist;
    char strDriverPath[256], strDriverPathExt[256], strPNPString[256];
    int iOSDisplayIndex;
};
struct Sensor
{
    int supported, value;
};
struct PmLog
{
    int size;
    Sensor sensors[256];
};
// ADLSensorType: 1 graphics clock (MHz), 2 memory clock, 8 edge temperature, 19 graphics activity (%), 23 ASIC power (W)
enum { kGfxClock = 1, kMemClock = 2, kTemp = 8, kActivity = 19, kPower = 23 };

static void* __stdcall Alloc(int bytes) { return std::malloc(size_t(bytes)); }

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int seconds = argc > 1 ? std::max(1, std::atoi(argv[1])) : 60;
    const char* stop = argc > 2 ? argv[2] : nullptr;
    HMODULE dll = LoadLibraryW(L"atiadlxx.dll");
    if (!dll)
    {
        std::printf("CLOCK unavailable: no atiadlxx.dll\n");
        return 0;
    }
    using Create = int (*)(void* (__stdcall*)(int), int, void**);
    using Destroy = int (*)(void*);
    using Count = int (*)(void*, int*);
    using Info = int (*)(void*, AdapterInfo*, int);
    using Query = int (*)(void*, int, PmLog*);
    auto create = reinterpret_cast<Create>(GetProcAddress(dll, "ADL2_Main_Control_Create"));
    auto destroy = reinterpret_cast<Destroy>(GetProcAddress(dll, "ADL2_Main_Control_Destroy"));
    auto count = reinterpret_cast<Count>(GetProcAddress(dll, "ADL2_Adapter_NumberOfAdapters_Get"));
    auto info = reinterpret_cast<Info>(GetProcAddress(dll, "ADL2_Adapter_AdapterInfo_Get"));
    auto query = reinterpret_cast<Query>(GetProcAddress(dll, "ADL2_New_QueryPMLogData_Get"));
    void* ctx = nullptr;
    if (!create || !destroy || !count || !info || !query || create(Alloc, 1, &ctx))
    {
        std::printf("CLOCK unavailable: ADL did not start\n");
        return 0;
    }
    int n = 0;
    std::vector<AdapterInfo> all;
    if (!count(ctx, &n) && n > 0 && n < 256)
    {
        all.resize(size_t(n));
        for (auto& a : all)
            a.iSize = sizeof(AdapterInfo);
        if (info(ctx, all.data(), int(all.size() * sizeof(AdapterInfo))))
            all.clear();
    }
    // ADL lists one adapter a display output: one a PCI bus position, AMD only (ADL's vendor id is decimal 1002),
    // whose query works.
    std::map<int, AdapterInfo> gpus;
    for (const auto& a : all)
    {
        if (gpus.count(a.iBusNumber))
            continue;
        PmLog d {};
        d.size = sizeof d;
        const int q = query(ctx, a.iAdapterIndex, &d);
        std::printf("ADAPTER %d %s, vendor %d, bus %d, present %d, query %d, graphics clock sensor %d\n",
                    a.iAdapterIndex, a.strAdapterName, a.iVendorID, a.iBusNumber, a.iPresent, q,
                    d.sensors[kGfxClock].supported);
        if (a.iVendorID == 1002 && !q && d.sensors[kGfxClock].supported)
            gpus[a.iBusNumber] = a;
    }
    if (gpus.empty())
    {
        std::printf("CLOCK unavailable: no AMD GPU answers the performance log query\n");
        destroy(ctx);
        return 0;
    }
    std::map<int, std::vector<int>> busy, any;
    std::map<int, std::vector<int>> power, temp, mem;
    const ULONGLONG t0 = GetTickCount64();
    for (int s = 0; GetTickCount64() - t0 < ULONGLONG(seconds) * 1000; ++s)
    {
        if (stop && GetFileAttributesA(stop) != INVALID_FILE_ATTRIBUTES)
            break;
        for (const auto& [bus, a] : gpus)
        {
            PmLog d {};
            d.size = sizeof d;
            if (query(ctx, a.iAdapterIndex, &d))
                continue;
            const auto v = [&](int k) { return d.sensors[k].supported ? d.sensors[k].value : -1; };
            std::printf("SAMPLE %llu ms bus %d gfx %d MHz mem %d MHz activity %d%% power %d W edge %d C\n",
                        GetTickCount64() - t0, bus, v(kGfxClock), v(kMemClock), v(kActivity), v(kPower), v(kTemp));
            any[bus].push_back(v(kGfxClock));
            if (v(kActivity) >= 50)
            {
                busy[bus].push_back(v(kGfxClock));
                power[bus].push_back(v(kPower));
                temp[bus].push_back(v(kTemp));
                mem[bus].push_back(v(kMemClock));
            }
        }
        Sleep(100);
    }
    const auto median = [](std::vector<int> x) { if (x.empty()) return -1; std::sort(x.begin(), x.end()); return x[x.size() / 2]; };
    for (const auto& [bus, a] : gpus)
    {
        const auto& b = busy[bus];
        if (b.empty())
            std::printf("CLOCK %s (bus %d): never 50%% busy in %zu samples (max clock %d MHz)\n", a.strAdapterName, bus,
                        any[bus].size(), any[bus].empty() ? -1 : *std::max_element(any[bus].begin(), any[bus].end()));
        else
            std::printf("CLOCK %s (bus %d): while busy, graphics %d MHz median (%d-%d), memory %d MHz, %d W, %d C, %zu of %zu samples\n",
                        a.strAdapterName, bus, median(b), *std::min_element(b.begin(), b.end()),
                        *std::max_element(b.begin(), b.end()), median(mem[bus]), median(power[bus]), median(temp[bus]),
                        b.size(), any[bus].size());
    }
    destroy(ctx);
    return 0;
}
