"""Check AmdLayout.h and the bootstrap contracts against the danielblnc runtime binaries.

Every value in a layout is a raw write into, or a call into, a closed-source DLL, and a wrong one
does not fail politely. This reads the tables out of the headers and checks each one against the
binary whose size and SHA-256 it names:

  * code entries (init, Record, Notify, shutdown, the wait helper) are .pdata function starts in
    .text, and graphicsWaitEnd is the helper's .pdata end;
  * the four wait-dispatch return addresses are, in order, the helper's only `call [rax+0x70]`s;
  * every data field (and recordLock+0x4c) lies in .data, and d3dCompileIat is the IAT slot of
    D3DCompile, or 0 when the runtime imports no D3DCompile at all;
  * Record's first three tests read enabled, nativeFailure and initDone, in that order, and Notify
    calls through the trampoline slot near its start;
  * every option with a [DlssNrOnAmd] key is the store that follows that key in the INI reader;
  * every other data field is reached by the pinned instruction sites in ANCHORS (opcode bytes,
    containing function and, where it matters, the call that follows);
  * seedCounter is the int Record zeroes at its pinned site, and seedSelfCheck the engine member the
    worker's one compare against seedCounter's engine member reads (SEED_SITES);
  * the bootstrap entry's call bytes, thread start, prologue and CreateThread import match (abi 1 only:
    an abi 2 host module is loaded as neuralhub_host.dll, whose DllMain starts nothing);
  * abi 2 (0.8.0 and later, NeuralHub's host module): every option is the store that follows its
    settings-registry key in the settings copy, formatEnum is the leaf with the pinned bytes, and
    detourAttachEx, requestStage, processInit and onceFlag are what DetourAttach, the stage-request
    log, NeuralHub_HostStart and its xchg say they are (check_host).

Header-level checks make sure every table except 0.2.17 (no binary at hand) gives each non-zero
data field one of those per-field checks, so a new field or table cannot go in unpinned.

No runtime ships with this repository. Pass the files or folders that hold them:

    python tests/amd_layout_binary_check.py <version.dll | folder> ...

With no argument it checks the headers alone. Standard library only.
"""

import hashlib
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAYOUT_H = ROOT / "OptiScaler/dlssnr/amd/AmdLayout.h"
HOSTLOAD_H = ROOT / "OptiScaler/dlssnr/amd/RuntimeHostLoad.h"

# Members that are .pdata function starts, and fields that are not data slots.
CODE = ("init", "record", "notify", "shutdown", "graphicsWaitBegin",
        "processInit", "detourAttachEx", "requestStage", "prepareWeights", "initCallbackFn",
        "computeOpen", "nrStageCallback", "presentBefore", "stageDispatch", "swapchainCreated")
# Code members that are leaf functions: no .pdata entry of their own, so they are pinned by their
# first bytes instead (formatEnum: add ecx,-2; cmp ecx,0x59, the DXGI_FORMAT range switch; ngxPublishFn:
# mov rax,[rip+...], the publish leaf, whose address the services init also stores in the GetState slot).
LEAF = {"formatEnum": "83 c1 fe 83 f9 59", "ngxPublishFn": "48 8b 05"}
DISPATCH = ("waitDispatchInit", "waitDispatchFallback", "waitDispatchSlices", "waitDispatchFinish")
NOT_DATA = set(CODE) | set(DISPATCH) | set(LEAF) | {"name", "size", "sha256", "d3dCompileIat", "graphicsWaitEnd",
                                                     "abi"}
# [DlssNrOnAmd] key -> layout member, for the options the runtime's INI reader stores directly.
INI_KEYS = {
    "Enabled": "enabled", "Temporal": "temporal", "UseFsrInputs": "fsrInputs", "UseDepth": "depthPresent",
    "Tonemap": "tonemap", "LocalTone": "tone", "LocalStructure": "structure", "SkinStructure": "skin",
    "UseAutoMask": "charMask", "ToneChannels": "toneChannels", "Scale": "scale", "SpinDraw": "spinDraw",
    "Async": "configuredInline", "Interop": "interop", "Style": "style", "ToneCurve": "toneCurve",
    "ToneLift": "toneLift", "UseGameExposure": "useGameExposure", "Quality": "quality",
}
# abi 2 has no [DlssNrOnAmd] reader: a settings registry holds the keys ("Section.Name" rows in .rdata) and
# one function copies their values into the option globals (0x1b770 on 0.8.0), each getter call taking the
# key's string just before the store. SpinDraw and Async left the settings, so spinDraw and
# configuredInline have only pinned sites. Registry key -> layout member.
SETTINGS_KEYS = {
    "Enhancement.Provider": "enabled", "Debug.DlssNrTemporal": "temporal", "Debug.DlssNrDepth": "depthPresent",
    "Debug.DlssNrTonemap": "tonemap", "DLSS-NR.ToneIntensity": "tone", "DLSS-NR.StructureIntensity": "structure",
    "DLSS-NR.SkinStructureIntensity": "skin", "Debug.DlssNrAutoMask": "charMask",
    "Debug.DlssNrToneChannel12": "toneChannels", "Debug.DlssNrResidualScale": "scale", "DLSS-NR.Style": "style",
    "DLSS-NR.ToneCurve": "toneCurve", "DLSS-NR.BlackLift": "toneLift", "Compatibility.GameExposure": "useGameExposure",
    "DLSS-NR.Quality": "quality", "Debug.HipZeroCopy": "interop",
}
# Fields the checks in check_layout pin by what the code does with them (besides INI_KEYS).
BEHAVIOUR_CHECKED = {"enabled", "nativeFailure", "initDone", "trampoline"}
# The noise seed pair: (Record's `mov dword [rip+seedCounter], 0` after the warm-up job, the worker's
# `mov r32, [engine+seedCounter]; cmp r32, [engine+seedSelfCheck]`, the only such pair in .text). The
# compare is the one the ReShade add-on's runtime_offsets.h cites (0x35142 on 0.4.1, 0x3bdd8 on 0.6.0).
SEED_SITES = {
    "0.4.1": (0x172c6, 0x35142), "0.4.2": (0x176e1, 0x372d3), "0.4.3": (0x17e91, 0x37f03),
    "0.5.0": (0x17ce1, 0x37f65), "0.5.1": (0x17ce1, 0x38a07), "0.6.0": (0x19d37, 0x3bdd8),
    # 0.8.0 moved the warm-up out of Record into the network run 0x22bf0, which Record calls directly, so
    # the zero is pinned in that callee (third element) rather than in Record itself.
    "0.8.0": (0x230e2, 0xbdb17, 0x22bf0),
}

# Pinned reference sites for every other data field: (field, site RVA, opcode bytes before the
# rip disp32, bytes after it, layout function the site must lie in or None, layout function the
# next call goes to or None[, offset the site adds to the field]). The instruction at the site must
# have exactly these bytes around its disp32, and the disp32 must reach the field. The 0.4.0 sites
# are the ones the verified maps cite. The 0.4.1 rows are the same instructions, found through the
# aligned instructions of each matched function (analysis-opti/implement/anchors/derive_041.py), and the
# 0.4.2 and 0.4.3 rows are the 0.4.1 ones mapped the same way (derive_042_043.py), and the 0.5.0 rows
# the 0.4.3 ones (derive_050.py). The 0.5.1 rows are the 0.5.0 ones through derive_051.py, at the same
# addresses, and the 0.6.0 rows the 0.5.1 ones through derive_060.py, but for Record's depthInverted read,
# which moved to another register (0x19a72, found by its field).
# The 0.3.1 and 0.3.0 rows are the same instructions in the same order, found by shape (daniel-runtime/analysis-opti/implement/anchors/derive_anchors.py and
# anchors.txt, which prints each site with its neighbours). 0.3.0 has no Notify-side jobDone wait
# and no mapped wait helper, so it lacks those rows.
# The 0.8.0 rows follow the 0.6.0 ones where the instruction survived (the evidence column of the
# danielblnc-0.8.0 notes beside this repository), and were read off the 0.8.0 binary's rip references
# where the host rewrite moved it. A containing function or callee given as a number is a .pdata start
# that is not a layout member: Record's work is split over the network run 0x22bf0 and the HIP job
# builder/worker 0x1dd00, the inline wait became the host table's 0xaad0, the option globals are written by
# the settings copy 0x1b770 only, and the first-present device event 0x2bd90 does the init sequence.
ANCHORS = {
    "0.8.0": [
        ('device', 0x123c0, '48 83 3d', '00', 'notify', None),  # cmp qword [device],0 after call [trampoline]
        ('queue', 0x123b2, '48 83 3d', '00', 'notify', None),  # cmp qword [queue],0
        ('queue', 0x12431, '48 89 35', '', 'notify', None),  # mov [queue],rsi; 'present queue %p'
        ('engine', 0x2bfe9, '48 8d 0d', '', 0x2bd90, 'init'),  # lea rcx,[engine]; lea rdx,weights; call init
        ('engine', 0x24aca, '48 8d 1d', '', 'record', None),  # lea rbx,[engine]
        ('engine', 0x23082, '48 8d 0d', '', 0x22bf0, None),  # warm-up job: lea rcx,[engine]
        ('historyView', 0x1fca1, '4c 8b 05', '', 0x1dd00, None),  # worker: mov r8,[historyView]; 'dlssnr_hist'
        ('historyValid', 0x230db, 'c6 05', '00', 0x22bf0, None),  # mov byte [historyValid],0 after the warm-up
        ('historyValid', 0x1e4bb, 'c6 05', '00', 0x1dd00, None),
        ('jobDone', 0x244fc, '8b 05', '', 'record', None),  # mov eax,[jobDone]; cmp eax,[jobId]
        ('jobDone', 0x244bd, '44 8b 05', '', 'record', None),  # 're-create has waited ... (done %d, submitted %d)'
        ('jobDone', 0x20189, '87 05', '', 0x1dd00, None),  # worker: xchg [jobDone],eax
        ('timeoutCount', 0x22ae8, 'c7 05', '00 00 00 00', 'shutdown', None),  # timeoutCount = 0
        ('timeoutCount', 0x20c4e, '3b 05', '', 0x1dd00, None),  # worker: cmp eax,[timeoutCount]
        ('watchdog', 0x24bdd, '44 8b 05', '', 'record', None),  # mov r8d,[watchdog] for the wait
        ('watchdog', 0x20c42, '87 05', '', 0x1dd00, None),  # worker: xchg [watchdog],eax
        ('pendingList', 0x12515, 'f0 48 0f b1 0d', '', 'notify', None),  # lock cmpxchg [pendingList],rcx
        ('pendingList', 0xacce, '48 87 05', '', 0xaad0, None),  # staging release: xchg [pendingList],rax
        ('jobId', 0x24502, '3b 05', '', 'record', None),  # cmp eax,[jobId]
        ('jobId', 0x24b53, '44 89 2d', '', 'record', None),  # mov [jobId],r13d before the publish
        ('jobId', 0x24e90, '44 89 2d', '', 'record', None),
        ('depthInverted', 0x1d969, '44 89 0d', '', 0x1d950, None),  # FSR flag hook: mov [depthInverted],r9d
        ('depthInverted', 0x2e933, '89 05', '', 0x2e8a0, None),  # event 0 sub 0x20: '... depth inverted %d%s ...'
        ('depthInverted', 0x22ebb, '8b 05', '', 0x22bf0, None),
        ('explicitDepth', 0x1d959, 'c6 05', '01', 0x1d950, None),  # mov byte [explicitDepth],1
        ('explicitDepth', 0x2e92c, 'c6 05', '01', 0x2e8a0, None),
        ('hipOrdinal', 0x24474, '8b 0d', '', 'record', None),  # mov ecx,[hipOrdinal] for hipSetDevice
        ('hipOrdinal', 0xdec4, '89 3d', '', 0xd8b0, None),  # device pick: mov [hipOrdinal],edi
        ('recreate', 0x2437e, '80 3d', '01', 'record', None),  # cmp byte [recreate],1
        ('recreate', 0x24544, 'c6 05', '00', 'record', None),  # mov byte [recreate],0 after the join/reset
        ('recreate', 0x1bc2a, 'c6 05', '01', 0x1b770, None),  # the settings copy requests a re-create
        ('recordLock', 0x2451b, '48 8d 0d', '', 'record', None),  # lea rcx,[recordLock]; call lock
        ('recordLock', 0x2452f, '81 3d', 'ff ff ff 7f', 'record', 'shutdown', 0x4c),  # cmp [lock+0x4c],0x7fffffff
        ('gate4c', 0x24398, '83 3d', '00', 'record', None),  # cmp dword [gate4c],0
        ('gate4c', 0x24804, '83 3d', '00', 'record', None),  # cmp dword [gate4c],0; jns
        ('gate4c', 0x22b7c, '87 0d', '', 'shutdown', None),
        ('gate68', 0x12529, '87 3d', '', 'notify', None),  # xchg [gate68],edi after the pendingList cmpxchg
        ('gate68', 0xacda, '87 05', '', 0xaad0, None),
        ('counter78', 0x246eb, 'ff 05', '', 'record', None),  # inc dword [counter78]
        ('graphicsPso', 0x92cc, '48 83 3d', '00', 'graphicsWaitBegin', None),  # cmp qword [graphicsPso],0
        ('graphicsPso', 0x92ed, '48 8b 15', '', 'graphicsWaitBegin', None),  # mov rdx,[graphicsPso] for SetPSO
        ('predicateReady', 0x8fd6, '80 3d', '01', 'graphicsWaitBegin', None),  # cmp byte [predicateReady],1
        ('spinDraw', 0x92bf, '83 3d', '00', 'graphicsWaitBegin', None),  # cmp dword [spinDraw],0
        ('spinDraw', 0x82f35, 'c7 05', '01 00 00 00', 0x82bf0, None),  # static init: spinDraw = 1 (no setting)
        ('configuredInline', 0x22902, '0f b6 05', '', 0x228b0, None),  # DEVICE REMOVED log, 'inline %d'
        ('configuredInline', 0x22e40, '0f b6 0d', '', 0x22bf0, None),
        ('interop', 0x22e36, '0f b6 0d', '', 0x22bf0, None),  # staging desc+0x34
        # The settings copy's stores, one per option (SETTINGS_KEYS ties each to its registry key).
        ('enabled', 0x1b834, '0f 94 05', '', 0x1b770, None),  # sete: Provider == "DLSS-NR"
        ('temporal', 0x1b98f, '0f 95 05', '', 0x1b770, None),
        ('depthPresent', 0x1b9a4, '0f 95 05', '', 0x1b770, None),
        ('tonemap', 0x1ba05, '89 05', '', 0x1b770, None),
        ('interop', 0x1ba19, '0f 95 05', '', 0x1b770, None),
        ('tone', 0x1baf0, 'f3 0f 11 05', '', 0x1b770, None),
        ('structure', 0x1bb04, 'f3 0f 11 05', '', 0x1b770, None),
        ('skin', 0x1bb18, 'f3 0f 11 05', '', 0x1b770, None),
        ('charMask', 0x1bb33, '89 0d', '', 0x1b770, None),
        ('scale', 0x1bb45, 'f3 0f 11 05', '', 0x1b770, None),
        ('style', 0x1bc8a, '89 05', '', 0x1b770, None),
        ('toneCurve', 0x1c6b1, '89 05', '', 0x1b770, None),
        ('toneLift', 0x1c6d2, 'f3 0f 11 05', '', 0x1b770, None),
        ('useGameExposure', 0x1c74a, '0f 94 05', '', 0x1b770, None),
        ('toneChannels', 0x1c76b, '89 0d', '', 0x1b770, None),
        ('quality', 0x1c7e7, '0f 94 05', '', 0x1b770, None),
        # Host-module fields.
        ('onceFlag', 0x693ba, '86 15', '', 0x69370, None),  # NeuralHub_HostStart: xchg byte [onceFlag],dl
        ('onceFlag', 0x6b9b8, '86 05', '', 0x6b900, None),  # DllMain, direct mode: xchg byte [onceFlag],al
        ('onceFlag', 0x6ba45, 'f6 05', '01', 0x6b900, None),  # DllMain, DLL_PROCESS_DETACH: test byte [onceFlag],1
        ('weightsOnce', 0x2bf10, '80 3d', '00', 0x2bd90, 'prepareWeights'),  # cmp byte [weightsOnce],0
        ('weightsOnce', 0x2bf1d, 'c6 05', '01', 0x2bd90, 'prepareWeights'),  # mov byte [weightsOnce],1
        ('initCallbackFn', 0x2bf29, '48 8d 05', '', 0x2bd90, None),  # lea rax,[initCallbackFn]
        ('initCallback', 0x2bf30, '48 89 05', '', 0x2bd90, None),  # mov [initCallback],rax
        # The runtime's own multipass: the settings copy stores entry 0 of each, the HIP job builder reads the count
        # and entries 1 of the arrays (passes 2) for the job.
        ('passCount', 0x1bcdc, '89 05', '', 0x1b770, None),
        ('passCount', 0x1ec12, '8b 15', '', 0x1dd00, None),
        ('passStyle', 0x1bce8, '89 05', '', 0x1b770, None),
        ('passStyle', 0x1ec27, 'f3 0f 2a 05', '', 0x1dd00, None, 4),
        ('passStructure', 0x1bcf6, 'f3 0f 11 05', '', 0x1b770, None),
        ('passStructure', 0x1ec4c, 'f3 0f 10 15', '', 0x1dd00, None, 4),
        ('passSkin', 0x1bd2b, 'f3 0f 11 15', '', 0x1b770, None),
        ('passTone', 0x1bd06, 'f3 0f 11 0d', '', 0x1b770, None),
        ('passTone', 0x1ec3c, 'f3 0f 10 05', '', 0x1dd00, None, 4),
        # NGX host: the services init stores the publish leaf in the GetState slot, which then writes the present
        # flag; the SR/RR CreateFeature-once functions (0x3ca80 / 0x5fcc0) test and set the init and gpu gates, the
        # weight builders (0x34830 / 0x58180) set the weights gates.
        ('ngxPublishFn', 0x13991, '48 8d 05', '', 0x13820, None),  # lea rax,[ngxPublishFn]
        ('ngxPublishSlot', 0x13998, '48 89 05', '', 0x13820, None),  # mov [ngxPublishSlot],rax
        ('ngxPresentFlag', 0xa4bb, '89 05', '', None, None),  # the leaf: mov [ngxPresentFlag],eax after setne al
        ('ngxGateSrInit', 0x3cc4b, '80 3d', '00', 0x3ca80, None),  # cmp byte [ngxGateSrInit],0
        ('ngxGateSrInit', 0x3cc5c, 'c6 05', '01', 0x3ca80, None),  # mov byte [ngxGateSrInit],1
        ('ngxGateSrGpu', 0x3cc96, '88 05', '', 0x3ca80, None),  # mov [ngxGateSrGpu],al
        ('ngxGateSrWeights', 0x34fd0, 'c6 05', '01', 0x34830, None),  # mov byte [ngxGateSrWeights],1
        ('ngxGateRrInit', 0x5ff9b, '80 3d', '00', 0x5fcc0, None),  # cmp byte [ngxGateRrInit],0
        ('ngxGateRrInit', 0x5ffac, 'c6 05', '01', 0x5fcc0, None),  # mov byte [ngxGateRrInit],1
        ('ngxGateRrGpu', 0x5ffec, '88 05', '', 0x5fcc0, None),  # mov [ngxGateRrGpu],al
        ('ngxGateRrWeights', 0x58920, 'c6 05', '01', 0x58180, None),  # mov byte [ngxGateRrWeights],1
        # Notify reads the chain head; the SR (0x313d0) and RR (0x33570) notifications take their pending list with a
        # cmpxchg; the workers (0x36f60 / 0x5a8b0) compare their done counter with the notified one.
        ('ngxNotifyChain', 0x1239c, '48 8b 05', '', 'notify', None),  # mov rax,[ngxNotifyChain]
        ('ngxSrPendingList', 0x31423, '48 0f b1 0d', '', 0x313d0, None),  # cmpxchg [ngxSrPendingList],rcx
        ('ngxRrPendingList', 0x335c3, '48 0f b1 0d', '', 0x33570, None),  # cmpxchg [ngxRrPendingList],rcx
        ('ngxSrJobDone', 0x370ff, '8b 0d', '', 0x36f60, None),  # mov ecx,[ngxSrJobDone]
        ('ngxSrJobNotified', 0x37105, '8b 05', '', 0x36f60, None),  # mov eax,[ngxSrJobNotified]
        ('ngxRrJobDone', 0x5a9cf, '8b 1d', '', 0x5a8b0, None),  # mov ebx,[ngxRrJobDone]
        ('ngxRrJobNotified', 0x5a9d5, '8b 05', '', 0x5a8b0, None),  # mov eax,[ngxRrJobNotified]
        # Compute runtime: EnsureCompute reads the state and compares the mode with 2 (D3D12).
        ('computeMode', 0x96cbb, '83 3d', '02', 0x96c10, None),  # cmp dword [computeMode],2
        ('computeState', 0x96c5b, '8b 05', '', 0x96c10, None),  # mov eax,[computeState]
        ('computeState', 0x96d1a, 'c7 05', '01 00 00 00', 0x96c10, None),  # mov dword [computeState],1
        # The present detour's "before" stage bumps the present counter; the FG addon's registration reads its ceiling.
        ('presentCounter', 0x127d9, '48 0f c1 05', '', 'presentBefore', None),  # lock xadd [presentCounter],rax
        ('fgMultiFrameCountMax', 0x6393e, '8b 1d', '', 0x636f0, None),  # mov ebx,[fgMultiFrameCountMax]
    ],
    "0.5.0": [
        ('device', 0xa032, '48 83 3d', '00', 'notify', None),
        ('queue', 0xa028, '48 83 3d', '00', 'notify', None),
        ('queue', 0xa091, '48 89 35', '', 'notify', None),
        ('engine', 0xc159, '48 8d 0d', '', None, 'init'),
        ('engine', 0x17cd5, '48 8d 0d', '', 'record', None),
        ('historyView', 0x1d506, '4c 8b 05', '', None, None),
        ('historyValid', 0x17ceb, 'c6 05', '00', 'record', None),
        ('jobDone', 0x159db, '39 05', '', 'record', None),
        ('jobDone', 0xa268, '39 2d', '', 'notify', None),
        ('timeoutCount', 0x1968f, 'c7 05', '00 00 00 00', 'shutdown', None),
        ('timeoutCount', 0x1e9d9, '3b 05', '', None, None),
        ('watchdog', 0x188dd, '44 8b 05', '', 'record', 'graphicsWaitBegin'),
        ('pendingList', 0xa0e5, 'f0 48 0f b1 0d', '', 'notify', None),
        ('pendingList', 0x18e62, '48 87 05', '', 'record', None),
        ('jobId', 0x159d5, '8b 05', '', 'record', None),
        ('jobId', 0x18e3c, '44 89 25', '', 'record', None),
        ('depthInverted', 0x17a3d, '44 8b 15', '', 'record', None),
        ('depthInverted', 0x14280, '44 89 0d', '', None, None),
        ('explicitDepth', 0x1426e, 'c6 05', '01', None, None),
        ('hipOrdinal', 0x156d4, '8b 0d', '', 'record', None),
        ('recreate', 0x159a9, '80 3d', '01', 'record', None),
        ('recreate', 0x15ce1, 'c6 05', '00', 'record', None),
        ('recordLock', 0x15cb8, '48 8d 0d', '', 'record', None),
        ('recordLock', 0x15ccc, '81 3d', 'ff ff ff 7f', 'record', 'shutdown', 0x4c),
        ('gate4c', 0x159b6, '83 3d', '00', 'record', None),
        ('gate68', 0x159bf, '83 3d', '00', 'record', None),
        ('gate68', 0xa0f9, '87 2d', '', 'notify', None),
        ('counter78', 0x1832f, 'ff 05', '', 'record', None),
        ('graphicsPso', 0x19edf, '48 83 3d', '00', 'graphicsWaitBegin', None),
        ('graphicsPso', 0x19f00, '48 8b 15', '', 'graphicsWaitBegin', None),
        ('predicateReady', 0x19be9, '80 3d', '01', 'graphicsWaitBegin', None),
    ],
    "0.5.1": [
        ('device', 0xa032, '48 83 3d', '00', 'notify', None),
        ('queue', 0xa028, '48 83 3d', '00', 'notify', None),
        ('queue', 0xa091, '48 89 35', '', 'notify', None),
        ('engine', 0xc159, '48 8d 0d', '', None, 'init'),
        ('engine', 0x17cd5, '48 8d 0d', '', 'record', None),
        ('historyView', 0x1d506, '4c 8b 05', '', None, None),
        ('historyValid', 0x17ceb, 'c6 05', '00', 'record', None),
        ('jobDone', 0x159db, '39 05', '', 'record', None),
        ('jobDone', 0xa268, '39 2d', '', 'notify', None),
        ('timeoutCount', 0x1968f, 'c7 05', '00 00 00 00', 'shutdown', None),
        ('timeoutCount', 0x1e9d9, '3b 05', '', None, None),
        ('watchdog', 0x188dd, '44 8b 05', '', 'record', 'graphicsWaitBegin'),
        ('pendingList', 0xa0e5, 'f0 48 0f b1 0d', '', 'notify', None),
        ('pendingList', 0x18e62, '48 87 05', '', 'record', None),
        ('jobId', 0x159d5, '8b 05', '', 'record', None),
        ('jobId', 0x18e3c, '44 89 25', '', 'record', None),
        ('depthInverted', 0x17a3d, '44 8b 15', '', 'record', None),
        ('depthInverted', 0x14280, '44 89 0d', '', None, None),
        ('explicitDepth', 0x1426e, 'c6 05', '01', None, None),
        ('hipOrdinal', 0x156d4, '8b 0d', '', 'record', None),
        ('recreate', 0x159a9, '80 3d', '01', 'record', None),
        ('recreate', 0x15ce1, 'c6 05', '00', 'record', None),
        ('recordLock', 0x15cb8, '48 8d 0d', '', 'record', None),
        ('recordLock', 0x15ccc, '81 3d', 'ff ff ff 7f', 'record', 'shutdown', 0x4c),
        ('gate4c', 0x159b6, '83 3d', '00', 'record', None),
        ('gate68', 0x159bf, '83 3d', '00', 'record', None),
        ('gate68', 0xa0f9, '87 2d', '', 'notify', None),
        ('counter78', 0x1832f, 'ff 05', '', 'record', None),
        ('graphicsPso', 0x19edf, '48 83 3d', '00', 'graphicsWaitBegin', None),
        ('graphicsPso', 0x19f00, '48 8b 15', '', 'graphicsWaitBegin', None),
        ('predicateReady', 0x19be9, '80 3d', '01', 'graphicsWaitBegin', None),
    ],
    "0.6.0": [
        ('device', 0xb4d2, '48 83 3d', '00', 'notify', None),
        ('queue', 0xb4c8, '48 83 3d', '00', 'notify', None),
        ('queue', 0xb531, '48 89 35', '', 'notify', None),
        ('engine', 0xda0f, '48 8d 0d', '', None, 'init'),
        ('engine', 0x19d2b, '48 8d 0d', '', 'record', None),
        ('historyView', 0x1fcae, '4c 8b 05', '', None, None),
        ('historyValid', 0x19d41, 'c6 05', '00', 'record', None),
        ('jobDone', 0x17a1b, '39 05', '', 'record', None),
        ('jobDone', 0xb708, '39 2d', '', 'notify', None),
        ('timeoutCount', 0x1b76f, 'c7 05', '00 00 00 00', 'shutdown', None),
        ('timeoutCount', 0x2129e, '3b 05', '', None, None),
        ('watchdog', 0x1a98a, '44 8b 05', '', 'record', 'graphicsWaitBegin'),
        ('pendingList', 0xb585, 'f0 48 0f b1 0d', '', 'notify', None),
        ('pendingList', 0x1af38, '48 87 05', '', 'record', None),
        ('jobId', 0x17a15, '8b 05', '', 'record', None),
        ('jobId', 0x1aee9, '44 89 25', '', 'record', None),
        ('depthInverted', 0x16260, '44 89 0d', '', None, None),
        ('depthInverted', 0x19a72, '8b 15', '', 'record', None),
        ('explicitDepth', 0x1624e, 'c6 05', '01', None, None),
        ('hipOrdinal', 0x17714, '8b 0d', '', 'record', None),
        ('recreate', 0x179e9, '80 3d', '01', 'record', None),
        ('recreate', 0x17d21, 'c6 05', '00', 'record', None),
        ('recordLock', 0x17cf8, '48 8d 0d', '', 'record', None),
        ('recordLock', 0x17d0c, '81 3d', 'ff ff ff 7f', 'record', 'shutdown', 0x4c),
        ('gate4c', 0x179f6, '83 3d', '00', 'record', None),
        ('gate68', 0x179ff, '83 3d', '00', 'record', None),
        ('gate68', 0xb599, '87 2d', '', 'notify', None),
        ('counter78', 0x1a3b2, 'ff 05', '', 'record', None),
        ('graphicsPso', 0x1c14f, '48 83 3d', '00', 'graphicsWaitBegin', None),
        ('graphicsPso', 0x1c170, '48 8b 15', '', 'graphicsWaitBegin', None),
        ('predicateReady', 0x1be59, '80 3d', '01', 'graphicsWaitBegin', None),
    ],
    "0.4.3": [
        ("device", 0xa232, "48 83 3d", "00", "notify", None),
        ("queue", 0xa228, "48 83 3d", "00", "notify", None),
        ("queue", 0xa291, "48 89 35", "", "notify", None),
        ("engine", 0xc32b, "48 8d 0d", "", None, "init"),
        ("engine", 0x17e85, "48 8d 0d", "", "record", None),
        ("historyView", 0x1d666, "4c 8b 05", "", None, None),
        ("historyValid", 0x17e9b, "c6 05", "00", "record", None),
        ("jobDone", 0x15b8b, "39 05", "", "record", None),
        ("jobDone", 0xa468, "39 2d", "", "notify", None),
        ("timeoutCount", 0x197ef, "c7 05", "00 00 00 00", "shutdown", None),
        ("timeoutCount", 0x1eb39, "3b 05", "", None, None),
        ("watchdog", 0x18a3c, "44 8b 05", "", "record", "graphicsWaitBegin"),
        ("pendingList", 0xa2e5, "f0 48 0f b1 0d", "", "notify", None),
        ("pendingList", 0x18fc1, "48 87 05", "", "record", None),
        ("jobId", 0x15b85, "8b 05", "", "record", None),
        ("jobId", 0x18f9b, "44 89 25", "", "record", None),
        ("depthInverted", 0x17bed, "44 8b 15", "", "record", None),
        ("depthInverted", 0x14430, "44 89 0d", "", None, None),
        ("explicitDepth", 0x1441e, "c6 05", "01", None, None),
        ("hipOrdinal", 0x15884, "8b 0d", "", "record", None),
        ("recreate", 0x15b59, "80 3d", "01", "record", None),
        ("recreate", 0x15e91, "c6 05", "00", "record", None),
        ("recordLock", 0x15e68, "48 8d 0d", "", "record", None),
        ("recordLock", 0x15e7c, "81 3d", "ff ff ff 7f", "record", "shutdown", 0x4c),
        ("gate4c", 0x15b66, "83 3d", "00", "record", None),
        ("gate68", 0x15b6f, "83 3d", "00", "record", None),
        ("gate68", 0xa2f9, "87 2d", "", "notify", None),
        ("counter78", 0x1848e, "ff 05", "", "record", None),
        ("graphicsPso", 0x1a03f, "48 83 3d", "00", "graphicsWaitBegin", None),
        ("graphicsPso", 0x1a060, "48 8b 15", "", "graphicsWaitBegin", None),
        ("predicateReady", 0x19d49, "80 3d", "01", "graphicsWaitBegin", None),
    ],
    "0.4.2": [
        ("device", 0x9de2, "48 83 3d", "00", "notify", None),
        ("queue", 0x9dd8, "48 83 3d", "00", "notify", None),
        ("queue", 0x9e41, "48 89 35", "", "notify", None),
        ("engine", 0xbedb, "48 8d 0d", "", None, "init"),
        ("engine", 0x176d5, "48 8d 0d", "", "record", None),
        ("historyView", 0x1cec6, "4c 8b 05", "", None, None),
        ("historyValid", 0x176eb, "c6 05", "00", "record", None),
        ("jobDone", 0x153db, "39 05", "", "record", None),
        ("jobDone", 0xa018, "39 2d", "", "notify", None),
        ("timeoutCount", 0x1903f, "c7 05", "00 00 00 00", "shutdown", None),
        ("timeoutCount", 0x1e3b7, "3b 05", "", None, None),
        ("watchdog", 0x1828c, "44 8b 05", "", "record", "graphicsWaitBegin"),
        ("pendingList", 0x9e95, "f0 48 0f b1 0d", "", "notify", None),
        ("pendingList", 0x18811, "48 87 05", "", "record", None),
        ("jobId", 0x153d5, "8b 05", "", "record", None),
        ("jobId", 0x187eb, "44 89 25", "", "record", None),
        ("depthInverted", 0x1743d, "44 8b 15", "", "record", None),
        ("depthInverted", 0x13c80, "44 89 0d", "", None, None),
        ("explicitDepth", 0x13c6e, "c6 05", "01", None, None),
        ("hipOrdinal", 0x150d4, "8b 0d", "", "record", None),
        ("recreate", 0x153a9, "80 3d", "01", "record", None),
        ("recreate", 0x156e1, "c6 05", "00", "record", None),
        ("recordLock", 0x156b8, "48 8d 0d", "", "record", None),
        ("recordLock", 0x156cc, "81 3d", "ff ff ff 7f", "record", "shutdown", 0x4c),
        ("gate4c", 0x153b6, "83 3d", "00", "record", None),
        ("gate68", 0x153bf, "83 3d", "00", "record", None),
        ("gate68", 0x9ea9, "87 2d", "", "notify", None),
        ("counter78", 0x17cde, "ff 05", "", "record", None),
        ("graphicsPso", 0x1988f, "48 83 3d", "00", "graphicsWaitBegin", None),
        ("graphicsPso", 0x198b0, "48 8b 15", "", "graphicsWaitBegin", None),
        ("predicateReady", 0x19599, "80 3d", "01", "graphicsWaitBegin", None),
    ],
    "0.4.1": [
        ("device", 0x9db2, "48 83 3d", "00", "notify", None),
        ("queue", 0x9da8, "48 83 3d", "00", "notify", None),
        ("queue", 0x9e11, "48 89 35", "", "notify", None),
        ("engine", 0xbeab, "48 8d 0d", "", None, "init"),
        ("engine", 0x172ba, "48 8d 0d", "", "record", None),
        ("historyView", 0x1ca56, "4c 8b 05", "", None, None),
        ("historyValid", 0x172d0, "c6 05", "00", "record", None),
        ("jobDone", 0x14fdb, "39 05", "", "record", None),
        ("jobDone", 0x9fe8, "39 2d", "", "notify", None),
        ("timeoutCount", 0x18c1f, "c7 05", "00 00 00 00", "shutdown", None),
        ("timeoutCount", 0x1df37, "3b 05", "", None, None),
        ("watchdog", 0x17e71, "44 8b 05", "", "record", "graphicsWaitBegin"),
        ("pendingList", 0x9e65, "f0 48 0f b1 0d", "", "notify", None),
        ("pendingList", 0x183f6, "48 87 05", "", "record", None),
        ("jobId", 0x14fd5, "8b 05", "", "record", None),
        ("jobId", 0x183d0, "44 89 25", "", "record", None),
        ("depthInverted", 0x1703d, "44 8b 15", "", "record", None),
        ("depthInverted", 0x13880, "44 89 0d", "", None, None),
        ("explicitDepth", 0x1386e, "c6 05", "01", None, None),
        ("hipOrdinal", 0x14cd4, "8b 0d", "", "record", None),
        ("recreate", 0x14fa9, "80 3d", "01", "record", None),
        ("recreate", 0x152e1, "c6 05", "00", "record", None),
        ("recordLock", 0x152b8, "48 8d 0d", "", "record", None),
        ("recordLock", 0x152cc, "81 3d", "ff ff ff 7f", "record", "shutdown", 0x4c),
        ("gate4c", 0x14fb6, "83 3d", "00", "record", None),
        ("gate68", 0x14fbf, "83 3d", "00", "record", None),
        ("gate68", 0x9e79, "87 2d", "", "notify", None),
        ("counter78", 0x178c3, "ff 05", "", "record", None),
        ("graphicsPso", 0x1946f, "48 83 3d", "00", "graphicsWaitBegin", None),
        ("graphicsPso", 0x19490, "48 8b 15", "", "graphicsWaitBegin", None),
        ("predicateReady", 0x19179, "80 3d", "01", "graphicsWaitBegin", None),
    ],
    "0.4.0": [
        ("device", 0x9e42, "48 83 3d", "00", "notify", None),  # cmp qword [device],0 after call [trampoline]
        ("queue", 0x9e38, "48 83 3d", "00", "notify", None),  # cmp qword [queue],0 after call [trampoline]
        ("queue", 0x9ea1, "48 89 35", "", "notify", None),  # mov [queue],rsi; 'present queue %p'
        ("engine", 0xbf3b, "48 8d 0d", "", None, "init"),  # lea rcx,[engine]; lea rdx,weights; call init
        ("engine", 0x172bd, "48 8d 0d", "", "record", None),  # lea rcx,[engine]; call history reset
        ("historyView", 0x1ca66, "4c 8b 05", "", None, None),  # worker: mov r8,[historyView]; 'dlssnr_hist'
        ("historyValid", 0x172d3, "c6 05", "00", "record", None),  # mov byte [historyValid],0 after the reset
        ("jobDone", 0x1506b, "39 05", "", "record", None),  # mov eax,[jobId]; cmp [jobDone],eax
        ("jobDone", 0xa078, "39 2d", "", "notify", None),  # inline wait: cmp [jobDone],ebp
        ("timeoutCount", 0x18c2f, "c7 05", "00 00 00 00", "shutdown", None),  # timeoutCount = 0
        ("timeoutCount", 0x1df47, "3b 05", "", None, None),  # worker: cmp eax,[timeoutCount]; jne store
        ("watchdog", 0x17e74, "44 8b 05", "", "record", "graphicsWaitBegin"),  # mov r8d,[watchdog]; call wait
        ("pendingList", 0x9ef5, "f0 48 0f b1 0d", "", "notify", None),  # lock cmpxchg [pendingList],rcx
        ("pendingList", 0x183f9, "48 87 05", "", "record", None),  # publish: xchg [pendingList],rax
        ("jobId", 0x15065, "8b 05", "", "record", None),  # mov eax,[jobId]; cmp [jobDone],eax
        ("jobId", 0x183d3, "44 89 25", "", "record", None),  # mov [jobId],r12d before the publish
        ("depthInverted", 0x170cd, "44 8b 15", "", "record", None),  # mov r10d,[depthInverted]
        ("depthInverted", 0x13910, "44 89 0d", "", None, None),  # FSR flag hook: mov [depthInverted],r9d
        ("explicitDepth", 0x138fe, "c6 05", "01", None, None),  # FSR flag hook: mov byte [explicitDepth],1
        ("hipOrdinal", 0x14d64, "8b 0d", "", "record", None),  # mov ecx,[hipOrdinal] for hipSetDevice
        ("recreate", 0x15039, "80 3d", "01", "record", None),  # cmp byte [recreate],1
        ("recreate", 0x15371, "c6 05", "00", "record", None),  # mov byte [recreate],0 after the join/reset
        ("recordLock", 0x15348, "48 8d 0d", "", "record", None),  # lea rcx,[recordLock]; call lock
        ("recordLock", 0x1535c, "81 3d", "ff ff ff 7f", "record", "shutdown", 0x4c),  # cmp [lock+0x4c],0x7fffffff
        ("gate4c", 0x15046, "83 3d", "00", "record", None),  # cmp dword [gate4c],0; jns
        ("gate68", 0x1504f, "83 3d", "00", "record", None),  # cmp dword [gate68],0; jns
        ("gate68", 0x9f09, "87 2d", "", "notify", None),  # xchg [gate68],ebp after the pendingList cmpxchg
        ("counter78", 0x178c6, "ff 05", "", "record", None),  # inc dword [counter78]
        ("graphicsPso", 0x1947f, "48 83 3d", "00", "graphicsWaitBegin", None),  # cmp qword [graphicsPso],0
        ("graphicsPso", 0x194a0, "48 8b 15", "", "graphicsWaitBegin", None),  # mov rdx,[graphicsPso] for SetPSO
        ("predicateReady", 0x19189, "80 3d", "01", "graphicsWaitBegin", None),  # cmp byte [predicateReady],1
    ],
    "0.3.1": [
        ("device", 0x9752, "48 83 3d", "00", "notify", None),
        ("queue", 0x9748, "48 83 3d", "00", "notify", None),
        ("queue", 0x97b1, "48 89 35", "", "notify", None),
        ("engine", 0xb84b, "48 8d 0d", "", None, "init"),
        ("engine", 0x15b2d, "48 8d 0d", "", "record", None),
        ("historyView", 0x1aeff, "4c 8b 05", "", None, None),
        ("historyValid", 0x15b43, "c6 05", "00", "record", None),
        ("jobDone", 0x138db, "39 05", "", "record", None),
        ("jobDone", 0x9988, "39 2d", "", "notify", None),
        ("timeoutCount", 0x1749f, "c7 05", "00 00 00 00", "shutdown", None),
        ("timeoutCount", 0x1bdec, "3b 05", "", None, None),
        ("watchdog", 0x166e4, "44 8b 05", "", "record", "graphicsWaitBegin"),
        ("pendingList", 0x9805, "f0 48 0f b1 0d", "", "notify", None),
        ("pendingList", 0x16c69, "48 87 05", "", "record", None),
        ("jobId", 0x138d5, "8b 05", "", "record", None),
        ("jobId", 0x16c43, "44 89 25", "", "record", None),
        ("depthInverted", 0x1593d, "44 8b 15", "", "record", None),
        ("depthInverted", 0x12180, "44 89 0d", "", None, None),
        ("explicitDepth", 0x1216e, "c6 05", "01", None, None),
        ("hipOrdinal", 0x135d4, "8b 0d", "", "record", None),
        ("recreate", 0x138a9, "80 3d", "01", "record", None),
        ("recreate", 0x13be1, "c6 05", "00", "record", None),
        ("recordLock", 0x13bb8, "48 8d 0d", "", "record", None),
        ("recordLock", 0x13bcc, "81 3d", "ff ff ff 7f", "record", "shutdown", 0x4c),
        ("gate4c", 0x138b6, "83 3d", "00", "record", None),
        ("gate68", 0x138bf, "83 3d", "00", "record", None),
        ("gate68", 0x9819, "87 2d", "", "notify", None),
        ("counter78", 0x16136, "ff 05", "", "record", None),
        ("graphicsPso", 0x17ccd, "48 83 3d", "00", "graphicsWaitBegin", None),
        ("graphicsPso", 0x17cee, "48 8b 15", "", "graphicsWaitBegin", None),
        ("predicateReady", 0x179d9, "80 3d", "01", "graphicsWaitBegin", None),
    ],
    "0.3.0": [
        ("device", 0x9483, "48 83 3d", "00", "notify", None),
        ("queue", 0x9479, "48 83 3d", "00", "notify", None),
        ("queue", 0x94e2, "48 89 35", "", "notify", None),
        ("engine", 0xb04a, "48 8d 0d", "", None, "init"),
        ("engine", 0x14d12, "48 8d 0d", "", "record", None),
        ("historyView", 0x19899, "4c 8b 05", "", None, None),
        ("historyValid", 0x14d28, "c6 05", "00", "record", None),
        ("jobDone", 0x129db, "39 05", "", "record", None),
        ("timeoutCount", 0x164c0, "c7 05", "00 00 00 00", "shutdown", None),
        ("timeoutCount", 0x1a6a0, "3b 05", "", None, None),
        ("watchdog", 0x158cc, "44 8b 05", "", "record", None),  # its wait helper 0x169a0 is not in the table
        ("pendingList", 0x9535, "f0 48 0f b1 0d", "", "notify", None),
        ("pendingList", 0x15d02, "48 87 05", "", "record", None),
        ("jobId", 0x129d5, "8b 05", "", "record", None),
        ("jobId", 0x15cdc, "44 89 25", "", "record", None),
        ("depthInverted", 0x14b22, "44 8b 15", "", "record", None),
        ("depthInverted", 0x11280, "44 89 0d", "", None, None),
        ("explicitDepth", 0x1126e, "c6 05", "01", None, None),
        ("hipOrdinal", 0x126d4, "8b 0d", "", "record", None),
        ("recreate", 0x129a9, "80 3d", "01", "record", None),
        ("recreate", 0x12ca1, "c6 05", "00", "record", None),
        ("recordLock", 0x12c78, "48 8d 0d", "", "record", None),
        ("recordLock", 0x12c8c, "81 3d", "ff ff ff 7f", "record", "shutdown", 0x4c),
        ("gate4c", 0x129b6, "83 3d", "00", "record", None),
        ("gate68", 0x129bf, "83 3d", "00", "record", None),
        ("gate68", 0x9549, "87 3d", "", "notify", None),  # xchg [gate68],edi
        ("counter78", 0x15315, "ff 05", "", "record", None),
    ],
}
# No binary of these is at hand, so their data fields have only the header-level checks.
NO_BINARY = {"0.2.17"}


def strip_comments(text):
    return re.sub(r"//[^\n]*", "", text)


def parse_layouts():
    text = strip_comments(LAYOUT_H.read_text(encoding="utf-8"))
    body = re.search(r"struct AmdLayout\s*\{(.*?)\};", text, re.S).group(1)
    members, defaults = [], {}
    for decl in re.findall(r"^\s*(?:const char\*|std::size_t|Sha256|std::uint32_t)\s+([^;]+);", body, re.M):
        for part in decl.split(","):
            # A default member initializer (abi = 1) is what a table that omits the member gets.
            m = re.match(r"\s*(\w+)\s*(?:=\s*(\S+))?", part)
            members.append(m.group(1))
            if m.group(2):
                defaults[m.group(1)] = m.group(2)
    layouts = {}
    for var, init in re.findall(r"inline constexpr AmdLayout (\w+)\s*\{(.*?)\};", text, re.S):
        sha = re.search(r'Sha256FromHex\("([0-9a-fA-F]{64})"\)', init).group(1).lower()
        init = re.sub(r'Sha256FromHex\("[0-9a-fA-F]+"\)', "SHA", init)
        values = {}
        if re.search(r"^\s*\.", init):
            for key, value in re.findall(r"\.(\w+)\s*=\s*([^,]+)", init):
                values[key] = value.strip()
            order = [m for m in members if m in values]
            if order != list(values):
                raise ValueError(f"{var}: designated initializers out of declaration order")
        else:
            for key, value in zip(members, [v.strip() for v in init.split(",") if v.strip()]):
                values[key] = value
        name = values.pop("name").strip('"')
        size = int(values.pop("size"))
        values.pop("sha256")
        fields = {m: int(values.get(m, defaults.get(m, "0")), 0) for m in members if m not in ("name", "size", "sha256")}
        layouts[var] = {"name": name, "size": size, "sha": sha, "fields": fields}
    listed = re.search(r"kAmdLayouts\[\]\s*=\s*\{([^}]*)\}", text).group(1)
    listed = re.findall(r"&(\w+)", listed)
    return members, layouts, listed


def parse_bootstraps():
    text = strip_comments(HOSTLOAD_H.read_text(encoding="utf-8"))
    table = re.search(r"Bootstrap bootstraps\[\]\s*\{(.*?)\n\};", text, re.S).group(1)
    out = {}
    for var, start, ret, raw in re.findall(r"\{\s*&(\w+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*\{([^}]*)\}\s*\}",
                                           table):
        out[var] = (int(start, 16), int(ret, 16), bytes(int(b, 0) for b in raw.replace("\n", " ").split(",")))
    start_bytes = re.search(r"startBytes\s*\{([^}]*)\}", text).group(1)
    return out, bytes(int(b, 0) for b in start_bytes.split(","))


class Image:
    """A PE mapped the way the loader would, plus its sections, .pdata and import slots."""

    def __init__(self, data):
        e = struct.unpack_from("<I", data, 0x3C)[0]
        if data[e:e + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        count, opt_size = struct.unpack_from("<H", data, e + 6)[0], struct.unpack_from("<H", data, e + 20)[0]
        opt = e + 24
        self.base = struct.unpack_from("<Q", data, opt + 24)[0]
        size_of_image, size_of_headers = struct.unpack_from("<II", data, opt + 56)
        dirs = [struct.unpack_from("<II", data, opt + 112 + 8 * i) for i in range(16)]
        self.img = bytearray(size_of_image)
        self.img[:size_of_headers] = data[:size_of_headers]
        self.sections = {}
        for i in range(count):
            off = opt + opt_size + 40 * i
            name = data[off:off + 8].rstrip(b"\0").decode("latin1")
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, off + 8)
            self.img[va:va + min(rawsize, vsize or rawsize)] = data[raw:raw + min(rawsize, vsize or rawsize)]
            self.sections[name] = (va, max(vsize, rawsize))
        self.funcs = {}
        pva, psize = dirs[3]
        for off in range(pva, pva + psize, 12):
            begin, end, _ = struct.unpack_from("<III", self.img, off)
            if begin:
                self.funcs[begin] = end
        # Export directory: its own DLL name and name -> RVA (forwarders included, the host checks none).
        self.export_name, self.exports = "", {}
        eva, _ = dirs[0]
        if eva:
            name_rva, _, count_funcs, count_names, funcs_rva, names_rva, ords_rva = struct.unpack_from(
                "<IIIIIII", self.img, eva + 12)
            self.export_name = self.cstr(name_rva)
            for i in range(count_names):
                name = self.cstr(struct.unpack_from("<I", self.img, names_rva + 4 * i)[0])
                ordinal = struct.unpack_from("<H", self.img, ords_rva + 2 * i)[0]
                self.exports[name] = struct.unpack_from("<I", self.img, funcs_rva + 4 * ordinal)[0]
        self.imports = {}
        for directory, delay in ((dirs[1], False), (dirs[13], True)):
            va, _ = directory
            while va:
                if delay:
                    attrs, dll, _, iat, names = struct.unpack_from("<IIIII", self.img, va)[:5]
                    step = 32
                else:
                    names, _, _, dll, iat = struct.unpack_from("<IIIII", self.img, va)
                    step = 20
                if not dll:
                    break
                dll_name = self.cstr(dll).lower()
                names = names or iat
                i = 0
                while True:
                    thunk = struct.unpack_from("<Q", self.img, names + 8 * i)[0]
                    if not thunk:
                        break
                    if not thunk >> 63:
                        self.imports[iat + 8 * i] = dll_name + "!" + self.cstr((thunk & 0x7FFFFFFF) + 2)
                    i += 1
                va += step

    def cstr(self, rva):
        end = self.img.index(0, rva)
        return self.img[rva:end].decode("latin1")

    def section(self, rva):
        for name, (va, size) in self.sections.items():
            if va <= rva < va + size:
                return name
        return None

    def rip(self, at, length):
        """Target of the rip-relative disp32 that ends `length` bytes into the instruction at `at`."""
        return at + length + struct.unpack_from("<i", self.img, at + length - 4)[0]

    def find_all(self, needle, start=0, end=None):
        end = len(self.img) if end is None else end
        at = self.img.find(needle, start, end)
        while at >= 0:
            yield at
            at = self.img.find(needle, at + 1, end)


def check_layout(image, layout, bootstrap, start_bytes, check):
    f = layout["fields"]
    text_va, text_size = image.sections[".text"]
    data_va, data_size = image.sections[".data"]
    for name in CODE:
        if f[name]:
            check(image.section(f[name]) == ".text" and f[name] in image.funcs,
                  f"{name} {f[name]:#x} is a .pdata function start in .text")
    for name, head in LEAF.items():
        if f[name]:
            rva, head = f[name], bytes.fromhex(head)
            inside = [b for b, e in image.funcs.items() if b <= rva < e]
            check(image.section(rva) == ".text" and not inside and image.img[rva:rva + len(head)] == head,
                  f"{name} {rva:#x} is a leaf in .text outside every .pdata range, starting {head.hex(' ')}")
    if f["graphicsWaitBegin"]:
        end = image.funcs.get(f["graphicsWaitBegin"])
        check(f["graphicsWaitEnd"] == end, f"graphicsWaitEnd {f['graphicsWaitEnd']:#x} is the helper's .pdata end")
        for name in DISPATCH:
            rva = f[name]
            check(f["graphicsWaitBegin"] < rva < f["graphicsWaitEnd"] and image.img[rva - 3:rva] == b"\xff\x50\x70",
                  f"{name} {rva:#x} returns from call [rax+0x70] inside the wait helper")
        # The helper has exactly these four Dispatch calls, in this order (init, fallback spin,
        # sliced spin, finish), so two of them cannot trade places unnoticed.
        sites = [at + 3 for at in image.find_all(b"\xff\x50\x70", f["graphicsWaitBegin"], f["graphicsWaitEnd"])]
        check(sites == [f[n] for n in DISPATCH], "the helper's Dispatch returns, in order, are "
              + ", ".join(f"{s:#x}" for s in sites))
    else:
        check(not any(f[n] for n in DISPATCH) and not f["graphicsWaitEnd"], "no wait-helper fields without its start")

    data = sorted((n, v) for n, v in f.items() if n not in NOT_DATA and v)
    stray = [f"{n}={v:#x}" for n, v in data if not data_va <= v < data_va + data_size]
    if f["recordLock"] and not data_va <= f["recordLock"] + 0x4c < data_va + data_size:
        stray.append("recordLock+0x4c")
    check(not stray, f"{len(data)} data fields inside .data [{data_va:#x}..{data_va + data_size:#x})"
          + (f" -- outside: {', '.join(stray)}" if stray else ""))

    compile_slots = [rva for rva, name in image.imports.items() if name.endswith("!D3DCompile")]
    if f["d3dCompileIat"]:
        check(f["d3dCompileIat"] in compile_slots, f"d3dCompileIat {f['d3dCompileIat']:#x} is the D3DCompile slot")
    else:
        check(not compile_slots, "d3dCompileIat 0: the runtime imports no D3DCompile")

    # Record opens with cmp byte [enabled],1; test byte [nativeFailure],1; test byte [initDone],1.
    at, gates = f["record"], []
    for opcode in (b"\x80\x3d", b"\xf6\x05", b"\xf6\x05"):
        found = image.img.find(opcode, at, f["record"] + 0x80)
        if found < 0:
            break
        # opcode, modrm, disp32, imm8: the disp32 does not end the instruction.
        gates.append(found + 7 + struct.unpack_from("<i", image.img, found + 2)[0])
        at = found + 7
    want = [f["enabled"], f["nativeFailure"], f["initDone"]]
    check(gates == want, "Record gates on enabled, nativeFailure, initDone: "
          + ", ".join(f"{g:#x}" for g in gates))

    calls = [image.rip(at, 6) for at in image.find_all(b"\xff\x15", f["notify"], f["notify"] + 0x40)]
    check(f["trampoline"] in calls, f"Notify calls through trampoline {f['trampoline']:#x}")

    if f["abi"] >= 2:
        # No INI reader: the settings copy takes each registry key's string (lea rcx or rdx) and
        # stores the value it gets back.
        lea_key = {}
        for opcode in (b"\x48\x8d\x0d", b"\x48\x8d\x15"):
            for at in image.find_all(opcode, text_va, text_va + text_size):
                lea_key.setdefault(image.rip(at, 7), []).append(at)
        for key, member in SETTINGS_KEYS.items():
            field = f.get(member, 0)
            if not field:
                continue
            # The key string itself, NUL-led so "X.Name" cannot match the tail of "X.Pass2Name".
            sites = [site for string in image.find_all(b"\0" + key.encode() + b"\0")
                     for site in lea_key.get(string + 1, [])]
            stored = any(stores_to(image, site + 7, site + 0x80, field) for site in sites)
            check(stored, f"setting {key} is stored at {member} {field:#x}")
    else:
        # INI reader: lea rdx,"<Key>" ... store to the field, before the next lea rcx,"DlssNrOnAmd".
        section_strings = {at for at in image.find_all(b"DlssNrOnAmd\0")}
        lea_rcx = sorted(at for at in image.find_all(b"\x48\x8d\x0d", text_va, text_va + text_size)
                         if image.rip(at, 7) in section_strings)
        lea_rdx = {}
        for at in image.find_all(b"\x48\x8d\x15", text_va, text_va + text_size):
            lea_rdx.setdefault(image.rip(at, 7), []).append(at)
        if not lea_rcx:
            check(False, "found the [DlssNrOnAmd] reader")
        for key, member in INI_KEYS.items():
            field = f.get(member, 0)
            if not field:
                continue
            stored = False
            for string in image.find_all(key.encode() + b"\0"):
                for site in lea_rdx.get(string, []):
                    stop = next((r for r in lea_rcx if r > site), site + 0x80)
                    stored = stored or stores_to(image, site + 7, min(stop, site + 0x80), field)
            check(stored, f"[DlssNrOnAmd] {key} is stored at {member} {field:#x}")

    check_anchors(image, layout, check)
    check_seed(image, layout, check)

    if f["abi"] >= 2:
        check_host(image, layout, check)
        print("  note abi 2: no bootstrap thread to isolate (neuralhub_host.dll's DllMain starts nothing)")
        return
    if bootstrap is None:
        check(False, "bootstrap isolation entry exists")
        return
    start, ret, call = bootstrap
    site = ret - len(call)
    check(bytes(image.img[site:ret]) == call, f"bootstrap call bytes at {site:#x}")
    check(image.rip(site, 7) == start, f"bootstrap lea r8 -> thread start {start:#x}")
    check(image.imports.get(image.rip(ret - 6, 6), "").endswith("kernel32.dll!CreateThread"),
          "bootstrap call goes through KERNEL32!CreateThread")
    check(start in image.funcs and bytes(image.img[start:start + len(start_bytes)]) == start_bytes,
          f"thread start {start:#x} is a .pdata start with the pinned prologue")


def stores_to(image, start, stop, field):
    """Whether an instruction in [start, stop) stores to [rip+disp32] == field."""
    # A store to [rip+disp32]: ModRM 00 xxx 101 right before the disp32, after a mov r/m,reg (88/89),
    # a movss/movsd/movups store (0f 11), a setcc (0f 9x), or a mov r/m,imm (c6 imm8, c7 imm32). The
    # target must be the field itself.
    for p in range(start, stop - 3):
        if image.img[p - 1] & 0xC7 != 0x05:
            continue
        op = image.img[p - 2]
        if op in (0x88, 0x89) or (image.img[p - 3] == 0x0F and (op == 0x11 or 0x90 <= op <= 0x9F)):
            end = p + 4
        elif op in (0xC6, 0xC7):
            end = p + (5 if op == 0xC6 else 8)
        else:
            continue
        if end + struct.unpack_from("<i", image.img, p)[0] == field:
            return True
    return False


def direct_calls(image, begin):
    """Targets of the `call rel32`s in the .pdata function at begin, in order. A byte scan, so a stray
    e8 inside another instruction may add a target, never remove one."""
    end = image.funcs.get(begin, begin)
    return [at + 5 + struct.unpack_from("<i", image.img, at + 1)[0] for at in image.find_all(b"\xe8", begin, end - 4)]


def check_host(image, layout, check):
    """abi 2: the host module's entry points are what its exports, Detours and its log say they are."""
    f = layout["fields"]
    text_va, text_size = image.sections[".text"]
    check(image.export_name.lower() == "version.dll",
          f"export directory name is {image.export_name!r} (the module ships as version.dll)")
    # DllMain compares its own basename with this name and, when it matches, returns without the
    # proxy, the process init or any thread: the mode the host loads it in.
    names = list(image.find_all(b"\0neuralhub_host.dll\0"))
    leas = [at for at in image.find_all(b"\x48\x8d", text_va, text_va + text_size)
            if image.img[at + 2] & 0xC7 == 0x05 and image.rip(at, 7) in {n + 1 for n in names}]
    check(bool(names) and bool(leas), f"\"neuralhub_host.dll\" is present and referenced from code ({len(leas)} lea)")

    start = image.exports.get("NeuralHub_HostStart")
    check(start in image.funcs, f"NeuralHub_HostStart is exported at a .pdata start ({start:#x})" if start
          else "NeuralHub_HostStart is exported")
    if start in image.funcs:
        check(f["processInit"] in direct_calls(image, start),
              f"NeuralHub_HostStart {start:#x} calls processInit {f['processInit']:#x}")
        # xchg byte [rip+disp32],r8: 86 /r with ModRM 00 reg 101.
        xchg = [at + 6 + struct.unpack_from("<i", image.img, at + 2)[0]
                for at in image.find_all(b"\x86", start, image.funcs[start] - 5) if image.img[at + 1] & 0xC7 == 0x05]
        check(f["onceFlag"] in xchg, f"onceFlag {f['onceFlag']:#x} is the byte NeuralHub_HostStart xchg's: "
              + ", ".join(f"{x:#x}" for x in xchg))

    # Detours: DetourAttach(ppPointer, pDetour) is an 11-instruction wrapper that calls
    # DetourAttachEx(ppPointer, pDetour, NULL, NULL, NULL). The host patches AttachEx to a success
    # return, so it must be the real one, not a look-alike with the same prologue.
    head = bytes.fromhex("4c 89 4c 24 20")
    check(image.img[f["detourAttachEx"]:f["detourAttachEx"] + len(head)] == head,
          f"detourAttachEx {f['detourAttachEx']:#x} starts {head.hex(' ')} (mov [rsp+0x20],r9)")
    wrapper = bytes.fromhex("48 89 54 24 10 48 89 4c 24 08 48 83 ec 38 48 c7 44 24 20 00 00 00 00"
                            " 45 33 c9 45 33 c0 48 8b 54 24 48 48 8b 4c 24 40 e8")
    sites = [at for at in image.find_all(wrapper, text_va, text_va + text_size)
             if at in image.funcs and image.img[at + len(wrapper) + 4:at + len(wrapper) + 9] == bytes.fromhex("48 83 c4 38 c3")]
    targets = [at + len(wrapper) + 4 + struct.unpack_from("<i", image.img, at + len(wrapper))[0] for at in sites]
    check(targets == [f["detourAttachEx"]], "the one DetourAttach wrapper ("
          + ", ".join(f"{s:#x}" for s in sites) + f") calls detourAttachEx {f['detourAttachEx']:#x}")

    # RequestStage arms the interceptors for a stage an add-on asked for, and logs it.
    begin = f["requestStage"]
    check(image.img[begin:begin + 3] == bytes.fromhex("55 41 57"), f"requestStage {begin:#x} starts 55 41 57")
    log = {n + 1 for n in image.find_all(b"\0host: stage %s requested by an addon")}
    refs = [at for at in image.find_all(b"\x48\x8d", begin, image.funcs.get(begin, begin))
            if image.img[at + 2] & 0xC7 == 0x05 and image.rip(at, 7) in log]
    check(bool(log) and bool(refs), f"requestStage logs \"host: stage %s requested by an addon\" ({len(refs)} lea)")


def unpinned_fields(layout):
    """Non-zero data fields that no INI or settings store, behaviour check or pinned site covers."""
    keys = SETTINGS_KEYS if layout["fields"].get("abi", 1) >= 2 else INI_KEYS
    covered = set(keys.values()) | BEHAVIOUR_CHECKED | {a[0] for a in ANCHORS.get(layout["name"], [])}
    if layout["name"] in SEED_SITES:
        covered |= {"seedCounter", "seedSelfCheck"}
    return sorted(n for n, v in layout["fields"].items() if v and n not in NOT_DATA and n not in covered)


def seed_compares(image, counter, self_check):
    """Sites of `[rex] mov r32, [base+disp8]; [rex] cmp r32, [base+disp32]` with these displacements."""
    m, (text_va, text_size) = image.img, image.sections[".text"]
    for at in image.find_all(bytes([0x8B]), text_va, text_va + text_size - 12):
        rex = m[at - 1] if 0x40 <= m[at - 1] <= 0x4F else None
        load = at + 1
        cmp_at = load + 2 + (1 if rex is not None else 0)
        if rex is not None and m[cmp_at - 1] != rex:
            continue
        if m[load] & 0xC0 != 0x40 or m[load] & 7 == 4 or m[load + 1] != counter or m[cmp_at] != 0x3B:
            continue
        modrm = m[cmp_at + 1]
        if modrm & 0xC0 != 0x80 or modrm & 0x3F != m[load] & 0x3F:
            continue
        if struct.unpack_from("<i", m, cmp_at + 2)[0] == self_check:
            yield at - (1 if rex is not None else 0)


def check_seed(image, layout, check):
    f = layout["fields"]
    sites = SEED_SITES.get(layout["name"])
    if not sites:
        check(not f["seedCounter"] and not f["seedSelfCheck"], "no seed fields without pinned sites")
        return
    zero, compare = sites[:2]
    # The function that zeroes it: Record, or (third element) a function Record calls directly.
    owner = sites[2] if len(sites) > 2 else f["record"]
    end = zero + 10
    ok = (image.img[zero:zero + 2] == bytes.fromhex("c7 05") and image.img[end - 4:end] == bytes(4)
          and image.rip(zero, 6) + 4 == f["seedCounter"] and owner <= zero < image.funcs.get(owner, 0))
    if owner != f["record"]:
        ok = ok and owner in direct_calls(image, f["record"])
    check(ok, f"seedCounter {f['seedCounter']:#x}: Record " + (f"zeroes it at {zero:#x}" if owner == f["record"]
          else f"calls {owner:#x}, which zeroes it at {zero:#x}"))
    counter, self_check = f["seedCounter"] - f["engine"], f["seedSelfCheck"] - f["engine"]
    found = list(seed_compares(image, counter, self_check)) if 0 < counter < 0x80 else []
    check(found == [compare], f"seedSelfCheck {f['seedSelfCheck']:#x}: the worker compares engine+{counter:#x} "
          f"with engine+{self_check:#x} only at {compare:#x}" + (f" -- found {[hex(a) for a in found]}" if found != [compare] else ""))


def check_anchors(image, layout, check):
    f = layout["fields"]
    for anchor in ANCHORS.get(layout["name"], []):
        field, site, pre, suf, where, callee = anchor[:6]
        add = anchor[6] if len(anchor) > 6 else 0
        pre, suf = bytes.fromhex(pre), bytes.fromhex(suf)
        end = site + len(pre) + 4 + len(suf)
        shape = image.img[site:site + len(pre)] == pre and image.img[end - len(suf):end] == suf
        target = end + struct.unpack_from("<i", image.img, site + len(pre))[0]
        ok = shape and f[field] != 0 and target == f[field] + add
        said = f"{field} {f[field]:#x}: site {site:#x} [{pre.hex(' ')} disp32 {suf.hex(' ')}]".replace(" ]", "]")
        said += f" reaches {target:#x}" + (f" = {field}+{add:#x}" if add else "")
        if where:
            # A member name, or the RVA of a .pdata function that is not a layout member.
            begin = f[where] if isinstance(where, str) else where
            ok = ok and begin <= site < image.funcs.get(begin, begin)
            said += f", inside {where}" if isinstance(where, str) else f", inside {where:#x}"
        if callee:
            # A direct call to the callee follows within 0x40 bytes.
            calls = [at + 5 + struct.unpack_from("<i", image.img, at + 1)[0]
                     for at in range(end, end + 0x40) if image.img[at] == 0xE8]
            ok = ok and (f[callee] if isinstance(callee, str) else callee) in calls
            said += f", then call {callee}" if isinstance(callee, str) else f", then call {callee:#x}"
        check(ok, said)


def main(argv):
    bad = []

    def check(ok, said):
        print(("  ok   " if ok else "  FAIL ") + said)
        if not ok:
            bad.append(said)

    members, layouts, listed = parse_layouts()
    bootstraps, start_bytes = parse_bootstraps()
    print(f"{LAYOUT_H.relative_to(ROOT).as_posix()}: {len(members)} members, {len(layouts)} tables\n")
    check(sorted(listed) == sorted(layouts), "every table is in kAmdLayouts: " + ", ".join(listed))
    check(len({l["name"] for l in layouts.values()}) == len(layouts), "runtime names are unique")
    check(len({(l["size"], l["sha"]) for l in layouts.values()}) == len(layouts), "size+SHA pairs are unique")
    # abi 2 host modules start no thread from DllMain, so only abi 1 tables have a bootstrap entry.
    isolated = sorted(var for var, l in layouts.items() if l["fields"]["abi"] < 2)
    check(sorted(bootstraps) == isolated, "every abi 1 table has one bootstrap entry, and only those")
    for var, layout in layouts.items():
        check(len(layout["fields"]) == len(members) - 3, f"{var}: {len(layout['fields'])} fields parsed")
        f = layout["fields"]
        check(bool(f["seedCounter"]) == bool(f["seedSelfCheck"]), f"{var}: seed fields mapped as a pair")
        if layout["name"] in NO_BINARY:
            print(f"  note {var}: no {layout['name']} binary at hand, its data fields have no pinned site")
            continue
        loose = unpinned_fields(layout)
        check(not loose, f"{var}: every data field has an INI, behaviour or pinned-site check"
              + (f" -- none for: {', '.join(loose)}" if loose else ""))
        zero = sorted({a[0] for a in ANCHORS.get(layout["name"], []) if not layout["fields"].get(a[0])})
        check(not zero, f"{var}: pinned sites name only mapped fields" + (f" -- unmapped: {', '.join(zero)}" if zero else ""))

    files = []
    for arg in argv[1:]:
        path = Path(arg)
        files += sorted(p for p in path.rglob("*.dll")) if path.is_dir() else [path]
    by_id = {(l["size"], l["sha"]): var for var, l in layouts.items()}
    seen = set()
    for path in files:
        data = path.read_bytes()
        var = by_id.get((len(data), hashlib.sha256(data).hexdigest()))
        if not var or var in seen:
            continue
        seen.add(var)
        layout = layouts[var]
        print(f"\n{var} ({layout['name']}) <- {path}")
        check_layout(Image(data), layout, bootstraps.get(var), start_bytes, check)
    for var in layouts:
        if var not in seen:
            print(f"\n{var} ({layouts[var]['name']}): no matching binary given, not checked")
    if argv[1:] and not seen:
        check(False, "at least one given file is a known runtime")

    print("\n" + (f"PASS ({len(seen)} runtime binaries checked)" if not bad else f"FAIL: {len(bad)} check(s)"))
    return 0 if not bad else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
