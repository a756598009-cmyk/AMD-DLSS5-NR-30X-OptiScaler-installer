// Vectored exception handler: one minidump in %TEMP%, the first time a crash lands in this module (or in no module at
// all), to be read with OptiScaler.pdb. Every exception still goes on with EXCEPTION_CONTINUE_SEARCH.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <Shlobj.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace dlssnr
{
namespace crashsnap
{
static PVOID s_handle = nullptr;
static volatile long s_taken = 0;

static HMODULE ThisModule()
{
    return reinterpret_cast<HMODULE>(&__ImageBase);
}

static bool InThisModule(std::uintptr_t pc)
{
    HMODULE m = ThisModule();
    MODULEINFO mi {};
    if (!GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi)))
        return false;
    const auto base = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
    return pc >= base && pc < base + mi.SizeOfImage;
}

static bool InAnyModule(std::uintptr_t address, HMODULE* module = nullptr)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &m))
        return false;
    if (module)
        *module = m;
    return true;
}

static bool ReadStack(std::uintptr_t address, std::uintptr_t& value)
{
    __try
    {
        value = *reinterpret_cast<const std::uintptr_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// A call through a null or stale pointer faults outside every module, so the dump alone names no code: the caller's
// return address is on top of the stack. Writes it, and the stack's other return-address candidates, as module+offset
// to %TEMP%\opti_crash_stack.txt.
static void WriteStackText(EXCEPTION_POINTERS* ptrs)
{
    wchar_t tmp[MAX_PATH] {}, path[MAX_PATH * 2] {};
    if (!GetTempPathW(MAX_PATH, tmp))
        return;
    _snwprintf_s(path, _TRUNCATE, L"%sopti_crash_stack.txt", tmp);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f)
        return;
    const auto* record = ptrs->ExceptionRecord;
    fprintf(f, "code %08lx at %p\n", record ? record->ExceptionCode : 0, record ? record->ExceptionAddress : nullptr);
    const std::uintptr_t rsp = ptrs->ContextRecord ? ptrs->ContextRecord->Rsp : 0;
    for (int i = 0, found = 0; rsp && i < 512 && found < 24; ++i)
    {
        std::uintptr_t value = 0;
        if (!ReadStack(rsp + i * sizeof(std::uintptr_t), value))
            break;
        HMODULE m = nullptr;
        if (!InAnyModule(value, &m))
            continue;
        wchar_t name[MAX_PATH] {};
        GetModuleFileNameW(m, name, MAX_PATH);
        const wchar_t* file = wcsrchr(name, L'\\');
        fprintf(f, "[rsp+%03x] %ls+0x%llx\n", i * 8, file ? file + 1 : name,
                static_cast<unsigned long long>(value - reinterpret_cast<std::uintptr_t>(m)));
        ++found;
    }
    fclose(f);
}

static void WriteDump(EXCEPTION_POINTERS* ptrs)
{
    wchar_t tmp[MAX_PATH] {}, path[MAX_PATH * 2] {};
    if (!GetTempPathW(MAX_PATH, tmp))
        return;
    _snwprintf_s(path, _TRUNCATE, L"%sopti_crash.dmp", tmp);
    if (auto fh = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); fh != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei {};
        mei.ThreadId = ::GetCurrentThreadId();
        mei.ExceptionPointers = const_cast<EXCEPTION_POINTERS*>(ptrs);
        mei.ClientPointers = FALSE;
        MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithUnloadedModules);
        ::MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), fh, type, &mei, nullptr, nullptr);
        ::CloseHandle(fh);
    }
}

static LONG WINAPI OnException(EXCEPTION_POINTERS* ptrs)
{
    DWORD code = ptrs->ExceptionRecord ? ptrs->ExceptionRecord->ExceptionCode : 0;
    // Real crashes and aborts, not the exceptions C++ EH and SEH use for control flow.
    if (code != 0xC0000005 && code != 0xC0000409 && code != 0xC0000417 && code != 0x80000003)
        return EXCEPTION_CONTINUE_SEARCH;
    std::uintptr_t pc = ptrs->ExceptionRecord
                            ? reinterpret_cast<std::uintptr_t>(ptrs->ExceptionRecord->ExceptionAddress)
                            : 0;
    // Faults in this module, and calls through a null or stale pointer (the PC in no module at all).
    const bool strayCall = code == 0xC0000005 && !InAnyModule(pc);
    if (!strayCall && (!pc || !InThisModule(pc)))
        return EXCEPTION_CONTINUE_SEARCH;
    if (_InterlockedCompareExchange(&s_taken, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    WriteStackText(ptrs);
    WriteDump(ptrs);
    return EXCEPTION_CONTINUE_SEARCH;
}


void EnableCrashSnap()
{
    if (s_handle)
        return;
    s_handle = ::AddVectoredExceptionHandler(1, (PVECTORED_EXCEPTION_HANDLER) OnException);
}
} // namespace crashsnap
} // namespace dlssnr
