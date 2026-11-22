#include "pch.h"
#include "FirstLightPTUnlock.h"

#include "State.h"
#include "Config.h"

#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

// Offsets below are valid for 007 First Light 1.2.0.0
// (exe sha1 cbbbd23388498b77edecf2829e3c402838bb2730).
//
// [exe + ManagerSlotRva] holds the engine's central manager instance.
//   manager + FeatureObjectOffset -> graphics feature object, vtable at exe + FeatureObjectVtableRva
//     obj + 0x49  -> published as the "$israytracingavailable" menu template variable
//     obj + 0x59  -> published as $dlssSupport byte 2 (DLSS Ray Reconstruction support)
//     obj + 0x5D  -> published alongside the DLSS support block
//     obj + 0x111 -> AND'd with obj + 0x110 and published for menu conditions
//     obj + 0x112 -> AND'd with obj + 0x110 and published for menu conditions
//
// A publisher method copies these members into static globals every time the menu
// evaluates the template variables, so patching the object itself is enough.
//
// WARNING - the flags used to be written only after the engine's one-shot
// deferred RT init (~49s after boot on 1.2.0.0) because that init read the
// flags and dereferenced the then-null ZRayTracer (crash 0xE3CDDE). Since the
// ZRayTracer creation gate (patch 1) now constructs the singleton
// unconditionally at renderer init, the flags are applied as early as possible
// instead, so the deferred init sees them set and builds the RT instance /
// material tables (crash 0xDCD81A was a consumer of those missing tables).
//
// The ZRayTracer singleton (exe + 0x5E107D8 static, manager + 0x189D8) is only
// constructed when the renderer init sees the "raytracing available" flag
// (feature object + 0x49) already set - but the renderer init runs before our
// flag patch can land, so on spoofed/non-Nvidia GPUs the ZRayTracer is never
// created and every consumer of exe + 0x5E107D8 crashes on the null pointer
// (0xE3CDDE deferred RT init, 0xDCE50F level loading). Instead of racing the
// renderer init with the flags, patch the conditional jump in the renderer init
// so the ZRayTracer constructor at exe + 0xDC87E0 runs unconditionally.
static constexpr uintptr_t ManagerSlotRva = 0x5C6DBC8;
static constexpr uintptr_t FeatureObjectOffset = 0x17C90;
static constexpr uintptr_t FeatureObjectVtableRva = 0x2D41448;

static std::atomic<bool> watcherStarted { false };
static std::atomic<bool> zrayTracerGatePatched { false };
static std::atomic<bool> featureSubGatePatched { false };

// mov [rdi+0x17CB0], rax | mov rax, [rdi+0x17C90] | cmp byte [rax+0x49], r14b | je skip
static constexpr BYTE ZRayTracerGatePattern[] = { 0x48, 0x89, 0x87, 0xB0, 0x7C, 0x01, 0x00, 0x48, 0x8B, 0x87,
                                                  0x90, 0x7C, 0x01, 0x00, 0x44, 0x38, 0x70, 0x49, 0x74, 0x39 };

// mov [rip->145E0FA90], r14 | movzx esi, byte [r14+0x49] | test sil, sil | je skip
// This one gates the construction of the sub-object at feature object + 0x40857C0
// which the level loading / ray tracing resource paths dereference without a check.
static constexpr BYTE FeatureObjectSubGatePattern[] = { 0x4C, 0x89, 0x35, 0xD3, 0x59, 0x14, 0x05, 0x41, 0x0F,
                                                        0xB6, 0x76, 0x49, 0x40, 0x84, 0xF6, 0x74, 0x59 };

struct GatePatch
{
    const BYTE* pattern;
    SIZE_T size;
    std::atomic<bool>* patched;
};

static const GatePatch GatePatches[] = {
    { ZRayTracerGatePattern, sizeof(ZRayTracerGatePattern), &zrayTracerGatePatched },
    { FeatureObjectSubGatePattern, sizeof(FeatureObjectSubGatePattern), &featureSubGatePatched },
};

// NRD resource setup (0x140E4BB00) dereferences member[0] of its 5th argument
// (the struct returned by the TracePathTracing frame-graph pass builder
// 0x140DC4B90, passed as a pointer in [rsp+0x28] at entry). That member is the
// NRD integration object and is NULL on non-Nvidia GPUs, so enabling path
// tracing crashes at 0x140E4BB9C (mov r9, [rax+0x10]) while creating the
// 'NRD:Packed Diff/Spec Radiance Hit Dist' resources.
//
// Fix: guard the function entry. When member[0] is NULL, zero the out-struct
// (arg1) and return it - exactly what the function's own "NRD disabled" path
// does (the je 0x140E4BDE6 at 0x140E4BCA5 skips pass registration and leaves
// arg1 = {0, 0} zeroed at the top). Path tracing then runs without the NRD
// denoiser instead of crashing. On a non-NULL member[0] the original prologue
// is replayed and the function behaves normally.
static std::atomic<bool> nrdSetupGuardPatched { false };

// Entry prologue of the NRD resource setup function (VA 0x140E4BB00):
//   mov rax, rsp | mov [rax+0x20], rbx | push rbp..r15 | lea rbp,[rsp-0x60] |
//   sub rsp, 0x160 | mov r12, [rbp+0xC0] (5th arg) | mov rsi, rcx (arg1)
static constexpr BYTE NrdSetupProloguePattern[] = {
    0x48, 0x8B, 0xC4,                                                 // mov rax, rsp
    0x48, 0x89, 0x58, 0x20,                                           // mov [rax+0x20], rbx
    0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, // push rbp,rsi,rdi,r12..r15
    0x48, 0x8D, 0x6C, 0x24, 0xA0,                                     // lea rbp, [rsp-0x60]
    0x48, 0x81, 0xEC, 0x60, 0x01, 0x00, 0x00,                         // sub rsp, 0x160
    0x4C, 0x8B, 0xA5, 0xC0, 0x00, 0x00, 0x00,                         // mov r12, [rbp+0xC0]
    0x48, 0x8B, 0xF1,                                                 // mov rsi, rcx
};

// Guard stub. First 39 bytes are static, the last 5 (jmp entry+7) get their
// rel32 patched at install time.
static constexpr BYTE NrdGuardCode[] = {
    0x48, 0x8B, 0x44, 0x24, 0x28,                   // mov rax, [rsp+0x28] (5th arg: struct ptr)
    0x48, 0x8B, 0x00,                               // mov rax, [rax]      (member[0] = NRD object)
    0x48, 0x85, 0xC0,                               // test rax, rax
    0x75, 0x13,                                     // jne prologueContinue
    0x48, 0xC7, 0x00, 0x00, 0x00, 0x00, 0x00,       // mov qword ptr [rcx], 0
    0x48, 0xC7, 0x41, 0x08, 0x00, 0x00, 0x00, 0x00, // mov qword ptr [rcx+8], 0
    0x48, 0x89, 0xC8,                               // mov rax, rcx
    0xC3,                                           // ret
    0x48, 0x8B, 0xC4,                               // mov rax, rsp (original entry instr 1)
    0x48, 0x89, 0x58, 0x20,                         // mov [rax+0x20], rbx (original entry instr 2)
};

static BYTE* s_stubFree = nullptr;
static SIZE_T s_stubFreeLeft = 0;

// Small executable scratch page for the guard stubs, allocated near the exe so
// a rel32 jmp from the patched entry can reach it
static BYTE* AllocateStub(SIZE_T bytes)
{
    if (s_stubFreeLeft < bytes)
    {
        const auto exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        void* page = nullptr;
        for (int i = 1; i <= 8 && !page; i++)
        {
            for (int up = 0; up < 2 && !page; up++)
            {
                uintptr_t hint = up ? exeBase + i * 0x10000000 : exeBase - i * 0x10000000;
                hint &= ~static_cast<uintptr_t>(0xFFFF); // 64KB allocation granularity
                page = VirtualAlloc(reinterpret_cast<LPVOID>(hint), 0x1000, MEM_RESERVE | MEM_COMMIT,
                                    PAGE_EXECUTE_READWRITE);
            }
        }

        if (page == nullptr)
        {
            LOG_WARN("Could not allocate a stub page near the exe base");
            return nullptr;
        }

        s_stubFree = static_cast<BYTE*>(page);
        s_stubFreeLeft = 0x1000;
    }

    auto* stub = s_stubFree;
    s_stubFree += bytes;
    s_stubFreeLeft -= bytes;
    return stub;
}

static constexpr uintptr_t FeatureFlagOffsets[] = { 0x49, 0x59, 0x5D, 0x111, 0x112 };

static bool IsReadable(uintptr_t address, size_t size)
{
    if (address < 0x10000)
        return false;

    MEMORY_BASIC_INFORMATION mbi {};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)))
        return false;

    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;

    const auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return address + size <= regionEnd;
}

// Returns the graphics feature object ptr or 0 (validates manager chain + vtable)
static uintptr_t GetValidatedFeatureObject()
{
    const auto exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (exeBase == 0)
        return 0;

    const auto managerSlot = exeBase + ManagerSlotRva;
    if (!IsReadable(managerSlot, sizeof(uintptr_t)))
        return 0;

    const auto manager = *reinterpret_cast<uintptr_t*>(managerSlot);
    if (!IsReadable(manager + FeatureObjectOffset, sizeof(uintptr_t)))
        return 0;

    const auto featureObject = *reinterpret_cast<uintptr_t*>(manager + FeatureObjectOffset);
    if (!IsReadable(featureObject, sizeof(uintptr_t)))
        return 0;

    // Vtable check guards against game updates shifting the layout around,
    // on a mismatch we do nothing instead of writing to garbage
    const auto vtable = *reinterpret_cast<uintptr_t*>(featureObject);
    if (vtable != exeBase + FeatureObjectVtableRva)
        return 0;

    return featureObject;
}

// Returns true if any flag value was changed this call
static bool ApplyFeatureFlags()
{
    const auto featureObject = GetValidatedFeatureObject();
    if (featureObject == 0)
        return false;

    bool changed = false;
    for (const auto offset : FeatureFlagOffsets)
    {
        if (!IsReadable(featureObject + offset, 1))
            return false;

        auto* flag = reinterpret_cast<BYTE*>(featureObject + offset);
        if (*flag != 1)
        {
            *flag = 1;
            changed = true;
        }
    }

    return changed;
}

static bool AllFeatureFlagsSet()
{
    const auto featureObject = GetValidatedFeatureObject();
    if (featureObject == 0)
        return false;

    for (const auto offset : FeatureFlagOffsets)
    {
        if (!IsReadable(featureObject + offset, 1))
            return false;

        if (*reinterpret_cast<BYTE*>(featureObject + offset) != 1)
            return false;
    }

    return true;
}

// NOP the conditional jumps that skip RT singleton constructions
static void PatchRTGates()
{
    const auto exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (exeBase == 0)
        return;

    MODULEINFO mi {};
    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof(mi)))
        return;

    const BYTE* base = static_cast<const BYTE*>(mi.lpBaseOfDll);
    const SIZE_T size = mi.SizeOfImage;

    for (auto& gate : GatePatches)
    {
        if (gate.patched->load(std::memory_order_acquire))
            continue;

        for (SIZE_T i = 0; i + gate.size + 4 <= size; i++)
        {
            if (std::memcmp(base + i, gate.pattern, gate.size) == 0)
            {
                void* jeAddr = const_cast<BYTE*>(base + i + gate.size - 2); // the trailing je
                const BYTE nops[2] = { 0x90, 0x90 };
                DWORD oldProtect = 0;
                if (VirtualProtect(jeAddr, sizeof(nops), PAGE_EXECUTE_READWRITE, &oldProtect))
                {
                    std::memcpy(jeAddr, nops, sizeof(nops));
                    VirtualProtect(jeAddr, sizeof(nops), oldProtect, &oldProtect);
                    FlushInstructionCache(GetCurrentProcess(), jeAddr, sizeof(nops));
                    gate.patched->store(true, std::memory_order_release);
                    LOG_INFO("RT creation gate patched at {:#x}", (size_t) jeAddr);
                }
                break;
            }
        }
    }
}

static bool AllGatesPatched()
{
    for (auto& gate : GatePatches)
    {
        if (!gate.patched->load(std::memory_order_acquire))
            return false;
    }
    return true;
}

// Installs the entry null-guard on the NRD resource setup function:
// entry -> [guard stub] -> (member[0] == null ? out-struct {0,0} + ret : original prologue)
static void ApplyNrdSetupGuard()
{
    if (nrdSetupGuardPatched.load(std::memory_order_acquire))
        return;

    const auto exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (exeBase == 0)
        return;

    MODULEINFO mi {};
    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof(mi)))
        return;

    const BYTE* base = static_cast<const BYTE*>(mi.lpBaseOfDll);
    const SIZE_T size = mi.SizeOfImage;

    constexpr SIZE_T GuardStubSize = sizeof(NrdGuardCode) + 5; // static code + jmp back
    static_assert(sizeof(NrdSetupProloguePattern) >= 5);

    for (SIZE_T i = 0; i + sizeof(NrdSetupProloguePattern) <= size; i++)
    {
        if (std::memcmp(base + i, NrdSetupProloguePattern, sizeof(NrdSetupProloguePattern)) != 0)
            continue;

        const auto entry = reinterpret_cast<BYTE*>(const_cast<BYTE*>(base + i));

        auto* stub = AllocateStub(GuardStubSize);
        if (stub == nullptr)
            return;

        // Build the stub: guard code + jmp entry+7 (replays the overwritten
        // prologue and rejoins the function after the patched 5 bytes).
        // NOTE: do not extend this stub into an always-disabled mode - the
        // setup function has several call sites and not all of them pass the
        // out-struct pointer in rcx, so an unconditional zero-out faults on
        // the write (seen as an AV at the stub's own mov [rcx] instruction).
        // Use ApplyNativeDenoiserForceOff instead.
        BYTE stubBytes[GuardStubSize] {};
        std::memcpy(stubBytes, NrdGuardCode, sizeof(NrdGuardCode));
        stubBytes[sizeof(NrdGuardCode)] = 0xE9; // jmp rel32 opcode
        const int32_t backJmp = static_cast<int32_t>((reinterpret_cast<uintptr_t>(entry) + 7) -
                                                     (reinterpret_cast<uintptr_t>(stub) + GuardStubSize));
        std::memcpy(stubBytes + sizeof(NrdGuardCode) + 1, &backJmp, sizeof(backJmp));

        DWORD oldProtect = 0;
        if (!VirtualProtect(stub, GuardStubSize, PAGE_EXECUTE_READWRITE, &oldProtect))
            return;
        std::memcpy(stub, stubBytes, GuardStubSize);
        VirtualProtect(stub, GuardStubSize, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), stub, GuardStubSize);

        // Redirect the function entry to the stub (covers the first two
        // prologue instructions, which the stub replays)
        BYTE entryJmp[5];
        entryJmp[0] = 0xE9; // jmp rel32
        const int32_t entryRel =
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - (reinterpret_cast<uintptr_t>(entry) + 5));
        std::memcpy(entryJmp + 1, &entryRel, sizeof(entryRel));

        if (!VirtualProtect(entry, sizeof(entryJmp), PAGE_EXECUTE_READWRITE, &oldProtect))
            return;
        std::memcpy(entry, entryJmp, sizeof(entryJmp));
        VirtualProtect(entry, sizeof(entryJmp), oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), entry, sizeof(entryJmp));

        nrdSetupGuardPatched.store(true, std::memory_order_release);
        LOG_INFO("NRD setup null-guard installed at {:#x}", (size_t) entry);
        return;
    }
}

// The renderer state slot holding the title's "native denoiser enabled" flag
// byte (used at three sites in the NRD family: 0xE4BC9E, 0xE4D068, 0xE4DA1B
// all read *(exe+0x5E0FAA0) + 0x8EA8).
static constexpr SIZE_T RendererStateSlotRva = 0x5E0FAA0;
static constexpr SIZE_T NativeDenoiserFlagOffset = 0x8EA8;

// Zeroes the title's "native denoiser enabled" flag so every consumer of it
// takes its own denoiser-disabled path, consistent with how the title runs
// on AMD before the PT unlock (the flag stays 0 there).
static void ForceNativeDenoiserFlagOff()
{
    const auto exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (exeBase == 0)
        return;

    const auto slotPtr = *reinterpret_cast<uintptr_t*>(exeBase + RendererStateSlotRva);
    if (slotPtr == 0)
        return;

    auto* flag = reinterpret_cast<volatile BYTE*>(slotPtr + NativeDenoiserFlagOffset);
    *flag = 0;
}

// The title's denoiser setup function (VA 0x140E4BB00) guards its registration
// path behind "cmp byte [rbx+0x8EA8],0 / je 0x140E4BDE6", where the target is
// the function's own disabled epilogue (returns arg1 unchanged). Making that
// je unconditional sends every caller through the function's own disabled
// path with correct prologue and argument semantics - unlike the entry stub,
// which cannot zero out the out-struct for call sites with a different ABI.
static constexpr BYTE NrdEnableCheckPattern[] = {
    0x80, 0xBB, 0xA8, 0x8E, 0x00, 0x00, 0x00, // cmp byte ptr [rbx+0x8EA8], 0
    0x0F, 0x84,                               // je rel32
};
static constexpr SIZE_T NrdDisabledEpilogueRva = 0xE4BDE6;
static std::atomic<bool> nrdForceOffPatched { false };

static void ApplyNativeDenoiserForceOff()
{
    if (nrdForceOffPatched.load(std::memory_order_acquire))
        return;

    const auto exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (exeBase == 0)
        return;

    MODULEINFO mi {};
    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof(mi)))
        return;

    const BYTE* base = static_cast<const BYTE*>(mi.lpBaseOfDll);
    const SIZE_T size = mi.SizeOfImage;

    for (SIZE_T i = 0; i + sizeof(NrdEnableCheckPattern) + 4 <= size; i++)
    {
        if (std::memcmp(base + i, NrdEnableCheckPattern, sizeof(NrdEnableCheckPattern)) != 0)
            continue;

        const auto jeAddr = reinterpret_cast<uintptr_t>(base + i + 7);
        int32_t jeRel = 0;
        std::memcpy(&jeRel, base + i + 7 + 2, sizeof(jeRel));
        const auto jeTarget = jeAddr + 6 + jeRel;

        // Safety: only patch the known setup function's check
        if (jeTarget - exeBase != NrdDisabledEpilogueRva)
            continue;

        // je rel32 (6 bytes) -> jmp rel32 (5 bytes) + nop
        BYTE patch[6];
        patch[0] = 0xE9;
        const int32_t jmpRel = static_cast<int32_t>(jeTarget - (jeAddr + 5));
        std::memcpy(patch + 1, &jmpRel, sizeof(jmpRel));
        patch[5] = 0x90;

        auto* patchAddr = reinterpret_cast<BYTE*>(jeAddr);
        DWORD oldProtect = 0;
        if (!VirtualProtect(patchAddr, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect))
            return;
        std::memcpy(patchAddr, patch, sizeof(patch));
        VirtualProtect(patchAddr, sizeof(patch), oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), patchAddr, sizeof(patch));

        nrdForceOffPatched.store(true, std::memory_order_release);
        LOG_INFO("Native denoiser force-off installed at {:#x} (branch now always "
                 "jumps to the disabled epilogue {:#x})",
                 (size_t) jeAddr, (size_t) jeTarget);
        return;
    }
}

void FirstLightPTUnlock::StartWatcher()
{
    bool expected = false;
    if (!watcherStarted.compare_exchange_strong(expected, true))
        return;

    std::thread(
        []()
        {
            LOG_INFO("Path Tracing menu unlock watcher started");

            // Patch the RT creation gates as early as possible, before the
            // renderer/feature object init runs (poll fast until they land)
            while (!AllGatesPatched() && !State::Instance().isShuttingDown)
            {
                PatchRTGates();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Install the NRD setup null-guard before the user can enable
            // path tracing (the setup function runs when PT is toggled on)
            while (!nrdSetupGuardPatched.load(std::memory_order_acquire) && !State::Instance().isShuttingDown)
            {
                ApplyNrdSetupGuard();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Apply the feature flags as early as the feature object exists -
            // the engine's deferred RT init (~+49s) must see them set so it
            // builds the RT instance / material tables instead of skipping
            while (!AllFeatureFlagsSet() && !State::Instance().isShuttingDown)
            {
                ApplyFeatureFlags();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            LOG_INFO("Path Tracing / Ray Reconstruction menu flags patched");

            while (!State::Instance().isShuttingDown)
            {
                if (ApplyFeatureFlags())
                    LOG_INFO("Path Tracing / Ray Reconstruction menu flags re-asserted");

                // Keep re-asserting the native-denoiser flag in case the game
                // recomputes or re-enables it mid-session

                // Keep re-checking in case the game recomputes the flags
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        })
        .detach();
}
