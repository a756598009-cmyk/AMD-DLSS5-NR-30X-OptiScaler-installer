# P1 / G1 submission split

## Done (harness)

- `CommandListProxy` List1–10; Create/Execute Detours in `SubmissionHooks`, armed only while lmxxf is the active backend (`SubmissionHooksWanted`).
- `CreateCommandList1`: wrap as **closed** proxy; producer allocator binds on first `Reset`.
- Split Execute with optional **between** callback (HIP slot).
- `ContinuationState` seed: viewport/scissor/topology/PSO/root/heaps/blend/stencil/OM + IA (IB/VB), SO, VRS, strip-cut, view-instance mask + **root tables/CBV/SRV/UAV/constants** (`RootBindState`) + **sample positions** + **depth bounds**.
- Ray tracing splits: the continuation restores `SetPipelineState1`'s state object (it and the PSO replace each other) and `SetPredication`; RTAS builds and `DispatchRays` leave no other list state (The Witcher 3 runs its upscaler in a list that traces rays).
- Fail-closed (no restore yet): meta → `MarkSplitIneligible`; root index/constants overflow → `root_overflow`; sample-position overflow → `sample_positions`.
- `ResourceStateBook`: refuse an open split transition barrier; preserve recorded aliasing-barrier order across the cut and invalidate affected tracked states; after Split apply **Execute decay** (read states → COMMON; RT/DSV-write/UAV/COPY_DEST/… survive).
- Product Execute path: when `ExpandEnabled()`, `AmdBridge::ExecuteBatch` always runs `ExecuteExpanded` (QI `ILogicalCommandList` → `ExecuteOnWithBetween`). `PendingListIndex` is Daniel-only batch isolation; lmxxf returns -1 on purpose.
- Unreal startup: install the Execute hook before exposing early proxies; before the first swapchain, wrap only DIRECT lists created by the host executable. Other modules retain native lists until the existing post-swapchain switch. The swapchain log reports how many game lists were wrapped early.
- Min G1 admission reject (`MarkSplitIneligible`): open split barrier / aliasing / `resource_state`; open Begin/End query at the cut, invalid query scope, an enhanced split barrier open at the cut / render_pass. Completed queries and timestamp EndQuery remain eligible. Split failure → ordinary SR (safe reject).
- Tests: `test-lmxxf-list-split.cmd`, `test-lmxxf-create-execute.cmd`, `test-lmxxf-list1-wrap.cmd`, `test-lmxxf-evaluate-cut.cmd`.

## Open

- Execute decay has not been checked against the D3D12 debug layer in a live game.
