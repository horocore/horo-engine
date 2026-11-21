# ADR-107: Navigation Query Consistency and Snapshot Ownership

- **Status**: Proposed
- **Date**: 2026-09-02
- **Supersedes**: None
- **Scope**: Navigation read snapshots, topology/tile/overlay consistency, immediate bounded queries, asynchronous path work, completion publication, stale/unavailable/no-path outcomes, Scene transition, World Streaming replacement, cache identity, leases, budgets and shutdown
- **Issue**: [NAV-004.1](https://github.com/HoroCore/horo-engine/issues/1255)
- **Jira**: [HORO-1255](https://horo-engine.atlassian.net/browse/HORO-1255)
- **Related**: [ADR-010](010-job-waiting-and-operation-store-ownership.md), [ADR-012](012-world-streaming-partition-authority-and-subsystem-boundaries.md), [ADR-016](016-navigation-target-ownership-and-dependency-boundary.md), [ADR-022](022-ai-fixed-tick-order-authority-and-simulation-budget.md), [ADR-026](026-large-world-precision-and-floating-origin-strategy.md), [ADR-105](105-navigation-asset-and-scene-ownership-boundary.md), [ADR-106](106-navigation-bake-ownership-transaction-and-cache.md)
- **Normative documents**: [Navigation and AI Architecture](../architecture/runtime/navigation-and-ai-architecture.md), [Scene Runtime](../architecture/runtime/scene-runtime.md), [World Streaming](../architecture/runtime/world-streaming-architecture.md), [Concurrency and Job System](../architecture/foundation/concurrency-and-jobs.md), [Runtime Lifecycle](../architecture/runtime/runtime-lifecycle.md)

## Context

NavigationRuntime already requires immutable query inputs, generation-checked
handles and owner-thread result publication. ADR-105 defines the cooked artifact
and runtime topology boundary, while World Streaming may replace or evict tiles and
dynamic obstacle intent may advance independently of a completed carved topology.
The missing contract is the exact state a query observes and what happens when that
state changes before work completes.

A worker retaining memory safely is not enough. A path over an evicted tile must not
be applied merely because the old bytes remain leased. Likewise, returning
`NoPath` when tiles are absent conflates proven unreachability with unavailable
evidence. Query callers need explicit consistency and coverage policy without
receiving provider refs or mutable topology.

Small projection/raycast tests are useful inside a fixed simulation phase, while a
full or hierarchical path may exceed the frame budget. Treating both as always
synchronous would introduce stalls; treating every query as async would add latency
and make deterministic fixed-tick behavior depend on worker completion. This ADR
defines both paths and one publication authority.

## Decision

### 1. Navigation publishes one combined immutable world snapshot

`NavigationCoordinator` owns the active `NavigationWorldSnapshot`. A published
snapshot contains or pins:

- `NavigationWorldId` bound to one exact `SceneRuntimeId` incarnation;
- monotonic topology generation and the canonical set of tile IDs/generations;
- cell `PartitionEpoch` and per-cell `StreamingGeneration`/residency fences where
  World Streaming supplies tiles;
- logical dynamic-obstacle overlay revision and conservative pending-blocker data;
- area/filter/profile/configuration revisions;
- origin epoch and all conversions needed to interpret provider-local coordinates;
- provider instance/fingerprint and immutable private topology resources; and
- leases, coverage index and bounded memory-accounting identities.

Topology, coverage and overlay references form one atomic root. A reader acquires
that root once; it cannot combine topology generation N with coverage or overlay
state from N+1. Provider query objects/scratch are exclusive per invocation, but
they all read the same pinned immutable topology root.

Tile install/removal, carved-topology adoption, filter/profile replacement and
Scene activation construct a detached candidate. The Navigation owner publishes
the complete root at the declared safe point with one no-fail swap. Candidate
failure leaves the old root active. No query can observe a loop that mutates public
tile entries one by one.

### 2. A read lease separates memory validity from logical currency

Acquiring a query produces a move-only `NavigationReadSnapshot`/lease with an
opaque `NavigationSnapshotToken`. The token names world/Scene incarnation,
topology/overlay/filter/origin revisions and exact coverage dependencies. It exposes
Horo values and query capabilities only; native pointers/refs remain provider-
private.

The lease keeps all referenced topology, tiles, provider resources and allocation
domains physically valid until the query and completion record release them. It
does not keep a tile logically resident, prevent a new world publication, or grant
permission to apply a result after its source generation was revoked.

Logical removal first publishes a new root without the tile and invalidates affected
paths/results. Physical reclamation waits for read leases. Retired memory remains
charged to the owning navigation/world budget; pressure rejects/defer new work or
requests policy action rather than freeing data beneath readers.

Snapshot tokens are transient diagnostics/validation identities. They are not
serialized into Scene source, cooked artifacts, saves or network protocols and
cannot be resolved after their world incarnation retires.

### 3. Consistency policy and coverage policy are explicit and orthogonal

Every query declares one consistency policy:

| Policy | Contract |
|---|---|
| `ExactSnapshot` | Answer against the acquired immutable token. The result remains a truthful historical answer but carries no claim that the token is still current. Intended for diagnostics and explicitly snapshot-based calculations. |
| `CurrentAtPublish` | Answer against one token and publish only if all relevant token revisions and authority still match. This is the default for gameplay paths. |
| `CurrentOrRetry` | Apply `CurrentAtPublish`; on staleness the coordinator may reacquire and retry within an explicit attempt/work/deadline budget. Exhaustion returns stale, never an older answer. |

A caller cannot request “latest” as a mutable view. “Current” is checked at a
specific owner-thread boundary. `ExactSnapshot` results still require separate
authority/currentness validation before any gameplay state mutation.

Every query also declares `CoveragePolicy::RequireComplete` or
`CoveragePolicy::AllowPartial`. Complete requires all topology needed to prove the
requested answer under the query's bounds. Partial allows a bounded prefix/frontier
and returns the exact missing tile/region coverage plus residency-request hints.
Partial is never reported as a complete path or `NoPath`.

### 4. Immediate queries are owner-thread, allocation-free bounded kernels

Immediate queries are reserved for operations with a declared small upper bound,
such as point projection/nearest location, navigation raycast, local reachability or
a fixed-node local path. They may run only on the Navigation/simulation owner during
an allowed phase in which no topology publication can interleave.

Admission acquires the current root and preallocated exclusive query scratch before
execution. The request provides hard node/output/work bounds. The call performs no
I/O, asset request, job submission, heap growth, provider mutation, blocking wait or
callback. Capacity exhaustion returns a typed bounded outcome.

The result answers its exact token. Because the owner cannot publish a new root
inside the call, `CurrentAtPublish` is satisfied at return. The caller must still
validate retained paths/tokens at the later mutation phase. Calls from workers,
transport/UI threads or during topology mutation are rejected/deferred rather than
silently moved to an unsafe provider path.

Deterministic fixed-tick mode uses these bounded kernels in stable request order on
the owning executor at the declared tick. It never waits for a worker or chooses
which result to use by completion time.

### 5. Full path work is asynchronous and never completes inline

Full, hierarchical, multi-tile or otherwise nontrivial path requests enter the
bounded `NavigationCoordinator` admission queue. Successful admission returns a
generation-checked `NavRequestHandle`; queue/result/scratch exhaustion returns
`AdmissionRejected` without a handle or job.

At owner-thread admission drain, the coordinator validates caller/world authority,
captures the requested snapshot token and allocates bounded result storage. A
Foundation `JobSystem` task receives owned request data, the read lease,
cancellation and exclusive provider query/scratch. It cannot access live Scene,
World Streaming or mutable overlay containers.

Workers write one owned completion record into the bounded completion queue. They
never invoke gameplay callbacks, mutate an agent/blackboard/component, advance a
request handle to publicly complete, request tiles directly or publish a new
topology. Even a cache hit or Null result follows scheduled completion; `Submit`
never invokes the consumer inline.

### 6. Only the simulation owner publishes completions

The simulation/Navigation owner is the sole completion publisher. During
`NavIntentCommit` it drains eligible records in stable request identity order and:

1. validates request-handle slot/generation and cancellation;
2. validates `NavigationWorldId`/`SceneRuntimeId` and caller authority;
3. applies the declared consistency policy against the active combined root;
4. validates every contributing tile/overlay/filter/origin dependency, including
   dependencies of negative and partial answers;
5. performs a bounded retry admission for `CurrentOrRetry` when allowed; and
6. atomically writes the terminal result record and any accepted navigation intent
   before Character/Physics locomotion.

No worker, gameplay system, editor adapter, World Streaming provider or backend may
publish completions. `CommitDeferredLifecycleChanges` publishes prepared Scene/cell
topology before the `NavIntentCommit` phase that may consume it; a Scene transition
closes the old world's admission before replacement. Stable owner ordering, not
queue arrival, defines deterministic visibility.

An immediate return on the owner is a direct kernel result, not asynchronous
completion publication. Any later agent/path mutation still occurs at its owning
phase and revalidates the token.

### 7. Outcomes distinguish proof, absence and staleness

Terminal results use typed outcome plus bounded diagnostic/provenance:

| Outcome | Meaning |
|---|---|
| `CompletePath` | A full accepted path was proven over complete coverage for the result token |
| `PartialPath` | Caller allowed partial and receives a valid bounded prefix/frontier plus missing coverage |
| `NoPath` | Complete required coverage was present and the bounded completed search proved no route |
| `NoNavigationData` | Required topology/profile/start/end coverage was unavailable; reachability was not disproved |
| `StaleSnapshot` | Relevant topology, tile, overlay, filter, origin or authority revision changed before required publication/use |
| `InvalidWorld` / `InvalidHandle` | Scene/world or request identity is revoked, foreign or generation-stale |
| `Cancelled` | Cancellation won before terminal publication |
| `CapacityExceeded` | Declared node/output/scratch/result/retry bound prevented completion |
| `Unsupported` | Selected provider/product lacks the requested query or consistency capability |
| `ProviderFailure` | Valid admitted provider work failed without a more specific public outcome |

`NoPath` is never used for missing/evicted tiles, absent navigation, an unsupported
provider, timeout/capacity, cancellation or stale data. `NoNavigationData` is never
cached as permanent unreachable truth. `PartialPath` cannot be consumed by a caller
that requested complete coverage.

Each path-like result carries its snapshot token, covered/explored tile-generation
set, overlay/filter/profile revision, origin conversion evidence, completion tick
and completeness flag. Provider refs and raw pointers are excluded.

### 8. Tile/overlay replacement invalidates by exact dependency coverage

A query records every tile/region it read or whose absence contributed to its
answer. Path corridors record all traversed and validation-border dependencies;
negative results record the explored frontier. When exact coverage cannot be
proven, the query conservatively depends on the whole topology generation.

Publishing a tile replacement/removal or new logical obstacle overlay invalidates
all results/caches whose dependency set intersects the change. Unrelated region
changes may preserve results only when exact coverage evidence proves independence.
Cache hits pass the same currentness/coverage validation as worker output.

Newly blocking obstacle intent is present in the combined snapshot immediately as a
conservative stop/avoid region, even if provider carving is pending. Queries cannot
use an older carved mesh while omitting the newer logical blocker. When a carved
candidate publishes, it advances topology/overlay coupling atomically; readers of
the old pair remain memory-safe but logically stale for currentness policy.

A held path/corridor is not perpetual authority. Before each later use or movement-
intent publication, the coordinator revalidates its relevant revisions/coverage.
Failure yields stale/no-data policy and an optional bounded resubmit, never movement
over silently replaced topology.

### 9. World Streaming controls residency during queries

World Streaming alone admits cell/tile residency. A query snapshot observes only
committed Active coverage. Resident-but-not-activated staging and logically evicted
tiles are unavailable even when their bytes exist.

Missing coverage may emit a bounded typed residency demand through the host adapter
after query completion/admission policy. It does not perform direct I/O, pin an
unadmitted cell or wait synchronously. `RequireComplete` returns
`NoNavigationData`; `AllowPartial` may return `PartialPath` with the demand. Denied,
budget-exhausted and permanently absent demands remain distinguishable diagnostics.

A cell transaction publishes all its required navigation tiles with its Scene/provider
barrier or none. Replacement advances the relevant partition/cell/topology
generations as one visible root. Eviction publishes logical removal first; late
worker results are stale even while their leases delay physical retirement.

### 10. Scene replacement revokes the whole navigation incarnation

Before Scene unload/replacement commits, the old `NavigationWorldId` closes
admission and requests cancellation of pending queries. Replacement constructs a
new world/root with a never-reused incarnation. There is no token/path transfer by
matching entity, definition AssetId or tile coordinates.

Workers may finish safely against leased old memory, but owner publication resolves
them to `InvalidWorld` or `Cancelled` according to the cancellation race. They can
never mutate the replacement Scene. Result handles remain queryable only for their
bounded terminal-retention policy and cannot resolve provider resources after lease
release.

Gameplay that wants a path after replacement resolves stable authored identity in
the new Scene and submits a new request. Editor debug history may display old token
provenance as historical data but cannot label it current or use it as a live pick.

### 11. Cancellation, shutdown and race precedence are fixed

Admission rejection creates no handle. For accepted work, terminal publication is
exactly once. At `NavIntentCommit`, precedence is:

1. invalid/replaced world or invalid handle prevents any result application;
2. an observed cancellation prevents unpublished success;
3. required-current revision mismatch yields `StaleSnapshot` or bounded retry;
4. otherwise the validated worker/kernel outcome publishes.

After a terminal result publishes, later cancellation or topology change does not
rewrite history, but any later gameplay use revalidates currentness. Cache insertion
accepts only validated complete outcomes and cannot make a stale result current.

Shutdown closes admission, revokes worlds, requests cancellation, drains completion
records on the owner, then uses the bounded host shutdown wait to join jobs. Result
storage and query scratch release before provider destruction; snapshots/tiles/
allocation hooks release only after the last lease. A timeout retains charged
resources and reports stalled retirement rather than force-freeing live memory.

### 12. Qualification covers consistency and lifecycle races

Required evidence includes:

- atomic multi-tile/root publication with readers observing only complete old or
  complete new topology/coverage/overlay tuples;
- exact versus current-at-publish versus bounded-retry policy and complete versus
  opt-in partial coverage combinations;
- owner-thread immediate query phase/size/allocation/thread rejection and
  deterministic stable-order execution;
- asynchronous admission/capacity, no inline callback, exclusive query scratch,
  worker completion queue and sole `NavIntentCommit` publication;
- distinct complete, partial, no-path, no-data, stale, invalid-world/handle,
  cancelled, capacity, unsupported and provider-failure outcomes;
- tile add/replace/evict, pending blocker, carved topology, filter/profile/origin
  revision and exact/conservative dependency invalidation;
- negative/partial cache dependencies and currentness revalidation on cache hits;
- residency request allowed/denied/budgeted/permanently absent behavior with no
  direct navigation I/O;
- Scene replacement/unload with queued/running/completed-unpublished work and no
  late application to a new incarnation;
- lease-delayed tile/provider retirement, pressure accounting and shutdown timeout
  without use-after-free; and
- race/sanitizer/failure-injection coverage across cancellation, completion,
  topology publication, eviction and Scene transition.

## Consequences

### Positive

- Every query observes one explainable topology/overlay/coverage revision set.
- Memory-safe leases and logical-currentness checks protect different invariants
  explicitly.
- Callers can distinguish proven no-route from missing or stale data.
- Immediate fixed-budget kernels and asynchronous paths share result semantics
  without sharing unsafe execution behavior.
- World Streaming, Scene lifecycle and Navigation retain their existing authorities.
- Tactical path consumers use a revocable owner-lane request view. A retired
  coordinator cannot admit or return results through that view. The worker
  keeps its immutable provider lease until its terminal fence.

### Costs

- Snapshots and results carry detailed revision/coverage provenance.
- Negative and partial answers may require conservative whole-world invalidation
  when exact explored coverage is unavailable.
- Retired tile memory remains charged until slow readers drain.
- Current-or-retry can spend additional bounded work during rapid topology change.

## Rejected Alternatives

### Query mutable live topology under a reader lock

Rejected because a long path would either block tile publication or observe mixed
tile/overlay generations. Immutable roots give readers stable input and writers a
detached commit boundary.

### Treat a lease as proof that a result is still current

Rejected because leases prove memory lifetime only. Logical eviction, Scene
replacement or blocker publication must invalidate application even while bytes
remain alive.

### Return `NoPath` for missing tiles

Rejected because incomplete evidence cannot prove unreachability and would poison
negative caches and gameplay fallback decisions.

### Run every query synchronously

Rejected because full/hierarchical searches can exceed frame budgets and block the
simulation owner. Only declared bounded kernels qualify for immediate execution.

### Run every query asynchronously

Rejected because small fixed-tick queries would gain avoidable latency and worker-
completion nondeterminism. The owner path remains bounded and explicit.

### Let workers publish directly to agents

Rejected because workers cannot atomically validate live authority, phase order and
current topology, and would race Scene/agent replacement. Workers publish only
owned completion records to the owner queue.

### Keep an evicted tile logically available while readers hold it

Rejected because residency authority and memory lifetime would be conflated. New or
late use stops at logical eviction; leases delay only physical reclamation.
