# D3D12 native command recording checkpoint

HORO-333 / #333 / [RND-007.4] has a first target-private native command
recording slice. This is not a complete D3D12 graph backend, renderer,
barrier adapter, presentation path or hardware qualification.

## One existing owner

`ID3D12Resources::Commands()` borrows the native context's `ID3D12Commands`.
The service is prepared during resource initialization and dies before that
context's native parents. It does not create another device, queue, fence,
resource registry, memory ledger or device incarnation. The existing direct
queue and resource-transfer fence are shared; private fence values map native
retirement only, while `RenderQueueSubmission` retains frontend-issued logical
order and signal authority.

All operations are externally serialized on the context's render owner thread.
A zero, cancelled, reused or foreign recording ticket cannot borrow an active
native list. Context identity comes from the existing process-wide incarnation
producer; the recording counter is local and never wraps.

`maximumCommandBatches` is an explicit one-through-128 slot bound, default 16.
Every allocator/list pair is created and initially closed during startup. Normal
successful `Begin` reuses an unused pair rather than creating new native objects
or growing metadata. Native driver allocator backing is opaque; this count bound
is not a measured byte charge or a claim of allocation-free driver execution.

## Recording and submission

1. Configure the exact frontend-issued effective direct queue once.
2. Begin a recording ticket from the bounded pool.
3. Borrow its native list through `D3D12NativeCommands.h`, which is neither public
   nor installed. The borrow ends at close, cancellation or shutdown.
4. Close the list through the service.
5. Submit with strictly increasing canonical order/signal values and a synchronous
   lease-transfer receipt. The receipt moves to backend completion authority
   before the irreversible native Execute call.
6. Poll the exact ticket without a CPU wait. Cancel a completed ticket to return
   its slot; completed observations do not silently consume identities.

The native encoder must validate finite workload/state/resource admission and
retain all referenced backing with the canonical frontend completion lease.
The service cannot inspect arbitrary native recording calls. Only one ticket
may own a given outstanding lease; multi-list graph aggregation is not yet
implemented. Raw native recording is an internal primitive, not a public/manual
barrier API or permission for scene/editor feature code to issue D3D12 calls.

A recording cancelled before submission is closed before allocator reuse. A
failed native close/reset quarantines that slot until context recreation; it
cannot be reset optimistically. Cancellation after submission stops observation,
not GPU execution. Native slots and leases survive cancellation and failed queue
signaling until the existing context fence proves completion. All submitters
poll pending signaling before admitting later fence values, so a failed signal
cannot be overtaken by a newer resource transfer or command submission.

The original resource service's COMMON boundary and first-use placed-memory
activation requirements are unchanged. This checkpoint provides no resource
state lowering and does not allow a caller to assume those obligations vanished.
Nonempty timeline waits are explicitly unsupported, not ignored or remapped.
Legacy graph barriers, optional enhanced-barrier query/admission and translation,
matched interqueue handoffs, logical submission observation, frame-token
integration and full graph/workload lowering remain required subsequent work in
HORO-333. No optional feature is advertised by this internal primitive.

## Shutdown and failures

The existing resource `StopAdmission` abandons unsent recordings and marks
submitted records for terminal reclamation. Its `PollDrain` retains the complete
context while either copies or command submissions remain pending. There is no
second command shutdown authority or normal-frame CPU/GPU wait. Actual device
removal remains an error; native disposal can then release retained command
leases without fabricating fence completion.

Validation/native failures retain typed D3D12 codes and native HRESULT details.
Failures before Execute leave the completion receipt with the caller. Failure
after Execute retains backend completion ownership even when Submit reports an
error. Counter exhaustion fails before UINT64_MAX is issued or wraps.

## Evidence and limits

`HoroD3D12CommandTests` is registered with the shared Catch/CTest helper and the
focused Windows CI target group. It includes a portable slot-bound case plus
explicit Windows x64 WARP cases. The real timestamp/resolve test records native
GPU commands and checks a nonzero resolved timestamp only after actual fence
completion; resource upload/readback tests are not substituted for this proof.
A test-only native queue gate proves pending/nonblocking poll and drain,
allocator retention, cancellation and eventual owner-thread lease release.
Other cases cover admission, foreign incarnation/thread, quarantined native
recording failure, near-exhaustion seeds, one outstanding lease authority and
real device removal immediately after native Execute. Signal-retry coverage
injects skipped Signal attempts returning E_FAIL around actual submitted native
work; it is not evidence of a naturally occurring driver Signal failure.

WARP tests intentionally use the existing explicit test fixture, not production
Windows/Agility host admission. They do not qualify a physical GPU, performance,
a packaged runtime, legacy/enhanced graph barriers or interqueue execution.
Local analyzer and hosted quality gates remain separate from native test results.

## Native references

- [Allocator Reset lifetime requirements](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandallocator-reset)
- [Command-list Reset and failed Close requirements](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-reset)
- [Resource owner and transfer boundary](d3d12-resource-service.md)
- [Canonical graph submission and retirement](../architecture/runtime/vulkan-command-execution.md)
