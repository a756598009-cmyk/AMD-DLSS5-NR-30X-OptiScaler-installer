// Runs this exe again with other arguments, as a child sharing the console and output handles, and waits for it.
// A driver crash ends only the child. Returns the exit code, or -1 if it timed out (the child is killed).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <string>

static std::wstring SelfPath()
{
    std::wstring p(32768, L'\0');
    p.resize(GetModuleFileNameW(nullptr, p.data(), DWORD(p.size())));
    return p;
}

static long RunChild(const std::wstring& args, DWORD timeoutMs, double* ms = nullptr)
{
    std::fflush(stdout);
    std::fflush(stderr);
    std::wstring cmd = L"\"" + SelfPath() + L"\" " + args;
    STARTUPINFOW si { sizeof si };
    PROCESS_INFORMATION pi {};
    const ULONGLONG t0 = GetTickCount64();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
        return -2;
    long code = -1;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT)
    {
        TerminateProcess(pi.hProcess, 0xDEAD);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    else
    {
        DWORD c = 0;
        GetExitCodeProcess(pi.hProcess, &c);
        code = long(c);
    }
    if (ms)
        *ms = double(GetTickCount64() - t0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// A crash's exit code is an NTSTATUS such as 0xC0000005.
static bool Crashed(long code) { return code < -2 || code > 255; }
