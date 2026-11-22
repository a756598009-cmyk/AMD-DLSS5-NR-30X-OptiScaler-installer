// Loads a proxy DLL by full path and prints which graphics modules are in the process afterwards.
// tools\test-proxy-link.ps1 compares the output against the 0.4.9 release DLL.
#include <windows.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
        return 2;
    const wchar_t* names[] { L"d3d9.dll", L"d3d11.dll", L"d3d12.dll", L"dxgi.dll", L"vulkan-1.dll", L"opengl32.dll" };
    for (auto name : names)
        wprintf(L"before %s=%d\n", name, GetModuleHandleW(name) != nullptr);
    if (!LoadLibraryW(argv[1]))
    {
        wprintf(L"load failed %lu\n", GetLastError());
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    for (auto name : names)
        wprintf(L"after %s=%d\n", name, GetModuleHandleW(name) != nullptr);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
}
