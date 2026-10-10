# Asynchronous grounded path batching

`NavigationCoordinator` is owned by the navigation simulation owner and composed
with the host's existing Foundation `JobSystem`. Its header belongs exclusively to
`HoroEngine::NavigationRuntime`. Existing provider and runtime-queue APIs remain
source compatible; consumers link the runtime target rather than adding source
include paths. The coordinator reuses the runtime queues' private bounded MPMC
transport primitive with a path-specific completion record.

Prepare the coordinator before fixed-tick work. Admission retains a world read
lease, exact caller incarnation, combined-root provenance, request/node/output
ceilings, cancellation ancestry and diagnostic/configuration context. It returns
a generation-safe handle without executing provider work or publishing a result.
World pause closes new admission but preserves previously admitted leases.

During the owner's scheduling phase call `Dispatch(tick)`. Repeated calls at one
tick share the same request and conservative node reservations. They also share
a separate owner-side selection-probe quota. Every full candidate-table scan is
charged before selection; exhaustion defers remaining work to the next tick.
The compiled probe ceiling bounds selection independently from provider queries
and prevents large admitted queues from resetting owner work through repeated
dispatch calls. Each logical owner
has both an outstanding-result quota and a per-tick request/node quota, shared by
all of its incarnations. A rotating logical-owner cursor provides round-robin
service, including when the tick budget is one. Within a caller, aged requests
precede fresh requests; otherwise priority, deadline and admission sequence break
ties. The sum of requested node ceilings is charged before dispatch, not reset
when a job finishes early.

Compatible world/Scene/topology leases, snapshot and revision fences, filters and
priorities share a partition. Configuration-bearing requests use individual
partitions because configuration revision numbers alone cannot prove identity
across independent configuration services. Each partition runs at most the
configured query count serially; the number of simultaneous partitions for a
world cannot exceed the provider's advertised exclusive query-scratch capacity.
Foundation owns execution priority and threads. Scheduler overload defers pending
requests; it neither blocks the owner nor silently drops accepted requests.

At `NavIntentCommit`, provide the current activation, combined-root provenance,
and current incarnations of retained callers, then call `Commit`. The caller span
is bounded by request capacity; it need contain only owners with retained
requests. It must be strictly sorted by logical owner ID, with one valid current
incarnation per owner. Ambiguous duplicate/replacement or unordered authority
slices reject the whole publication attempt without changing request state.
Binary lookup then bounds per-request caller validation instead of rescanning
all callers for every completion. This additive coordinator has no existing
production callers to migrate; new host adapters must sort their authority slice
before calling `Commit`. An all-zero activation represents an unloaded world. Publication checks
cancellation, exact Scene/world, caller incarnation and every captured snapshot,
topology, obstacle, filter, profile and origin fence. Currentness is conservative
for the whole topology. A stale root returns `StaleSnapshot`; this implementation
has no automatic retry or fabricated region coverage evidence.

Worker arrival cannot skip an earlier eligible admitted request. Later eligible
results wait behind that request until it produces a candidate, is cancelled,
becomes stale or exceeds its hard deadline. Future-target requests do not block
currently eligible work. Ready candidates may publish on their inclusive deadline
tick; after that tick an unpublished request returns `CapacityExceeded` and signals
cooperative cancellation. Cancelled requests produce exactly one terminal result,
and cancellation after publication cannot alter it.

`Take(handle)` transfers the terminal result once. It preserves the provider's
exact `Result<NavigationPath>`, including errors, geometric partial paths and their
stop/corridor evidence. This result does not assert missing-region coverage that
the current provider contract cannot establish. It grants no gameplay authority;
movement consumption still uses `NavigationPathPolicy` and a fresh owner-phase
observation. Workers never invoke callbacks or touch Scene/agent state.

Request storage and partition/fallback buffers are prepared under the inline
storage ceiling. Provider output is bounded separately by each admitted request;
Foundation records/callbacks and cancellation sources are bounded by request and
partition counts. A contended completion-ring publication retains the owned
result in the partition's prepared fallback slot. Only after Foundation terminal
state proves callback return or queued-work revocation may the owner collect that
fallback. Consumed result slots remain reserved while their partition runs;
handle generations advance only after quiescence and never wrap.

On Scene/host shutdown call `BeginShutdown`, which closes admission and signals
unpublished requests plus queued/running Foundation jobs. Continue owner terminal
publication/consumption while shutting down the host scheduler under its existing
policy. Coordinator destruction never waits: accepted callbacks retain owned
state and world leases. The host must finish JobSystem shutdown before unloading
provider code. A world unload or replacement invalidates old results immediately,
while leases keep provider memory physically safe until workers drain.

This coordinator implements best-effort asynchronous scheduling. It does not claim
deterministic publication ticks or measured throughput; deterministic fixed-tick
kernels require the distinct execution capability described in ADR-107.
