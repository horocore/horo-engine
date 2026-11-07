# D3D12 command submission

Delivery: HORO-333 / #333 [RND-007.4].

This backend-private service realizes command recording, queue ordering and GPU
completion. It does not select the renderer, create a window, compile shaders or
provide the remaining D3D12 graphics/presentation features.

## Authority and admission

Device sessions opt in with explicit `D3D12InitializationRequest::commandQueues`,
barrier mode and command limits. An empty queue list keeps a resource-only
session. `D3D12Initialization::Commands()` borrows the initialized service on the
render owner; it does not discover queues or select a mode. One command owner
borrows a canonical resource incarnation. Once it has executed GPU work,
replacing it requires a fresh resource incarnation so old timelines cannot be
restarted under the same identity.

The frontend owns the compiled graph, logical resource states, effective queue
identities, submission order and timeline points. D3D12 consumes the existing
`RenderGraphExecutionRequest`, `RenderQueueSubmission` and
`RenderGraphLogicalState` contracts. Native enums and COM handles remain inside
the D3D12 target.

The complete request must pass portable validation and native representability
checks before any command list is submitted. Unsupported workloads and invalid
resource or queue evidence return typed errors. They do not select another
backend or silently omit work.

This command component accepts Scene-role graphs containing buffer-copy and
ordering work. UI composition and shader-dependent draw/dispatch workloads
require their separate pipeline adapters. Frame tokens remain opaque issued
identities; the command service does not invent a second host frame scheduler.

Queue roles may alias only when the admitted logical topology names the same
effective queue. Independent native queues require explicit admission. GPU waits
refer to exact previously admitted timeline points; normal submission does not
wait for the CPU or GPU to become idle.

Completion receipts preserve exact issued values, including gaps in frontend
timeline numbering. Polling completion does not itself expire a receipt. Only
capacity pressure from new work may retire proven-complete old receipt ranges;
dependencies used by the incoming batch remain protected. An observation whose
exact issuance evidence has expired returns a typed receipt-expired outcome.
Callers must keep required dependencies inside the configured receipt window;
a timeline high-water mark cannot manufacture a receipt for an unissued gap.

## Barrier mode

Legacy barriers are the baseline established by ADR-032. Enhanced barriers are
an explicit service-creation requirement, qualified by the native feature query
and command-list interface. An unavailable required mode returns unsupported;
the service never silently changes modes.

Resource state remains bound to the actual resource generation and canonical
resource owner. Integration with the existing upload/readback service must
preserve its COMMON entry/exit boundary, including GPU ordering and pending
resource lifetime. A graph-local ordinal alone is not native resource identity.

For default-heap resources, the private adapter admits an explicit native COMMON
entry and return boundary proven by that resource owner. It records the
contents-preserving preparation into the graph's declared logical access state,
then realizes the compiled transitions. Submission boundaries return to COMMON
before an ordered later use re-establishes its logical state. An external release
that requires a different native handoff is unsupported and must reject the
complete batch before execution. Upload/readback heaps retain their own fixed
native states; they are not treated as default-heap COMMON resources.

## Lifetime and failure

The render owner serializes admission, recording, submission, observation and
shutdown. Command allocators and lists are reused only after actual completion
evidence. Finite slot limits return busy rather than allocating unbounded work.

Cancellation before submission discards unsent work. Once native execution may
have begun, cancellation cannot release command storage or resident-resource
leases. A failed completion signal must retain that ownership for recovery;
absence of a signal is not completion. Native device loss remains a distinct
terminal outcome.

Shutdown closes admission and polls pending work without a normal-frame idle
wait. The host retains the entire native composition until drained or until an
explicit device-loss recovery path safely retires it. Declared bounded CPU wait
purposes remain governed by the existing `RenderCpuWait` contract.

## Qualification

Local macOS arm64 Debug qualification built the D3D12 library and all three
focused test executables. The command service passed nine portable cases with
405 assertions, initialization passed 22 cases with 215 assertions, and the
resource-envelope check passed one case with five assertions: 625 executed
assertions, zero failures, errors or skips. The affected CI-routing Python test
also passed. These are portable admission and lifecycle results. Windows-only
native bodies were excluded from this build; its zero skip count does not mean
that the native Windows cases ran.

The existing Windows WARP job is the native qualification lane. Its command
target adds 14 Windows-only cases covering real copied bytes, queue-to-queue and
transfer interoperability, allocator reuse, pending resource retention, owner
replacement, cancellation, device removal and shutdown. Their execution remains
pending CI. Enhanced support and unsupported outcomes must be reported
separately. A successful WARP run does not establish physical adapter,
interactive window or performance acceptance. No new coverage or hosted quality
clearance is claimed; local Sonar is waived and Codacy was not rerun.

## References

- [ADR-032: D3D12 baseline](../adr/032-d3d12-baseline-and-agility-sdk-policy.md)
- [ADR-173: queue submission and fences](../adr/173-render-queue-submission-and-fence-contract.md)
- [ADR-175: resource state and barriers](../adr/175-render-resource-state-and-barrier-model.md)
- [Microsoft enhanced-barrier specification](https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html)
- [Microsoft resource-barrier guidance](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12)
