# ADR-013: Environment Query Ownership, Item and Scoring Model

- **Status**: Proposed
- **Date**: 2026-08-28
- **Supersedes**: None
- **Scope**: EQS orchestration, typed providers, plans/items/contexts, scoring, safe points, deterministic/adaptive budgets, cache identity and cancellation
- **Issue**: [GAI-004.1](https://github.com/HoroCore/horo-engine/issues/1346)
- **Jira**: [HORO-1346](https://horo-engine.atlassian.net/browse/HORO-1346)
- **Normative document**: [Navigation And AI Architecture — EQS](../architecture/runtime/navigation-and-ai-architecture.md#environment-query-system-eqs)

## Context

Tactical AI queries compose candidate generation, geometric/navigation tests and
ranking to find cover, targets or vantage points. The previous proposal described
active query execution as passive snapshot provision, combined millisecond cutoffs
with unconditional replay determinism, and treated ordinary slice exhaustion as a
terminal partial result. Its cache omitted dependencies such as Perception and its
cancellation wording implied that every running provider job could be stopped.
These contracts need explicit authority, timing, identity and lifetime rules.

## Decision

**Gameplay AI owns tactical orchestration and scoring. Navigation, Physics and
Perception own both their domain data and query execution; EQS submits bounded
typed read-only requests. Immutable submission inputs govern an accepted query.
Deterministic execution uses work units and declared-tick provider publication;
adaptive execution may use wall-time targets with weaker timing guarantees.
Ordinary yielding is nonterminal. Cache correctness requires complete input and
provider identities. Cancellation suppresses late results while safely retiring
work that cannot be interrupted.**

### Ownership And Composition

| Responsibility | Authority |
|---|---|
| Admission, query handles, lifecycle, scheduling and terminal publication | EnvironmentQueryManager within Gameplay AI |
| Candidate/test stages | Internal QueryExecutor |
| Normalization/aggregation/ranking | Internal QueryScoring |
| Result entries and bounded eviction | Internal QueryCache |
| Navigation projection, containment and path/cost kernels | NavigationCoordinator / NavigationApi under ADR-016 |
| Collision ray/sweep/overlap kernels | Physics owner or its read-only query adapter |
| Agent-specific sensed state and memory | Perception domain |
| Decisions using immutable results | Behavior trees / decision graphs |

Sole authority is a **subsystem** rule, not a mandate for one concrete manager class
to implement every operation. Internal roles do not create independent schedulers,
thread pools or OperationStores. EQS never implements pathfinding from borrowed
NavMesh data, mutates provider state or gives clients access to server-private
perception. Host composition injects domain adapters; no runtime editor dependency
or provider discovery through global services is introduced.

### Assets, Inputs And Extensions

EnvironmentQueryTemplate has a stable AssetId registered in AssetRegistry;
EnvironmentQueryPlan is its immutable cooked artifact, published through CookCatalog.
Stable StageIds survive renaming/reordering. The plan records complete stage/context
schemas, dependencies, result policy, scores and budgets. Runtime pins its artifact
digest for the query lifetime rather than changing plans on hot reload.

Follow the [Asset Pipeline](../architecture/runtime/asset-pipeline.md) and
[ADR-017](017-prefab-role-ownership-and-capability-tiers.md): preserve every canonical
cook-key field and use the dependency-aware extension for referenced assets and
provider/schema artifacts. ProjectVersion authoring migration precedes cook;
cooked-format/schema compatibility is independently checked at runtime. Unsupported
versions, broken dependencies and bounds errors fail before activation. This is a
required implementation contract, not a claim of a completed EQS cooker.

Point uses canonical WorldCoordinate64; Actor carries a non-owning EntityRef with
scene and entity generation; DirectionalRay includes an origin and finite direction.
Custom items carry a stable type/schema ID and bounded immutable canonical bytes
(maximum 64 inline bytes). No pointers, borrowed transient views, native C++ object
layouts or callbacks enter payloads. Context data uses bounded retained storage.

Required contexts, positions, identities, parameters, tags/team/faction/stance,
seed and provider read versions are captured at successful submission. Contexts
remain unchanged across ticks: consumers requiring fresher state cancel/reissue.
Results expose as-of ticks and revisions; they are not promises of current state.
Provider reads use retained immutable versions or validate the captured revision;
inability to honor it yields StaleInputs, never a silent context refresh.

Custom item/context/stage descriptors are inert metadata, following
[ADR-018](018-command-registration-permissions-threading-and-packaged-build-policy.md).
They declare unique namespaced IDs, schema versions, size/alignment, dependencies,
owner thread/safe point, mode capabilities and canonical serialization/hash/equality.
Host composition rejects duplicates, missing dependencies, cycles and incompatible
schemas. A provider without complete deterministic cache identity makes the query
uncacheable, not partially keyed.

### Generators, Tests And Scoring

Grid, donut-ring and cone generators have checked dimensions and maxItems.
NavMeshProjectionGenerator submits Navigation-owned work; PerceivedEntitiesGenerator
reads an authorized perception snapshot. Stable item indices are assigned before
async dispatch using canonical generation/entity order.

`Horo/AI/EnvironmentQueryGenerators.h` provides the candidate kernels through
`HoroEngine::AISceneIntegration`. Lattice points use exact global millimeters
and stable Z/X order. Out-of-range offsets are skipped and counted. Captured
authored or Navigation-projected points preserve the provider's canonical input
order. The perception path accepts only the currently authorized revision and
deduplicates weak entity references. Each successful batch records the stage,
generator, provider, and revision. Consumers supply bounded output storage and
link the scene integration target. Navigation request dispatch and staged
execution are composed separately from these kernels.

GeometryLineOfSightTest measures physical obstruction through Physics.
PerceivedVisibilityTest asks what an agent senses/remembers through Perception.
Distance, dot-product, path-cost and cover-exposure tests declare all actual provider
and context dependencies. Geometric visibility and perception are not synonyms.

Each test filters, scores, or both. Scores normalize to [0,1] using declared fixed
ranges. Scored plans use `sum(weight * score) / sum(weight)`; cook/admission require
finite nonnegative weights and a finite positive total, and reject invalid ranges
and statically reproducible arithmetic failures. Explicit filter-only plans bypass
the formula and use stable selection ordering.

Runtime NaN/Inf or overflow marks a candidate InvalidScore, excludes it from winning,
and produces a bounded diagnostic; a displayed zero does not make invalid data
eligible. Validation tests surface reproducible authoring errors. Ranking compares
plan-versioned fixed-point ScoreKeys, then the configured tie-breaker and stable
item index, with explicit precision/rounding and StageId aggregation order. No
hash-map/pointer order or non-transitive epsilon comparator is permitted.

`Horo/AI/EnvironmentQueryScoring.h` joins one bounded rule per compiled test
stage to its declared provider. Scoring consumes captured provider facts in
stable StageId order. Version-one keys round the normalized score to the nearest
millionth, with half steps rounded up. A configured signed secondary key
precedes the final item-index tie break. Missing, incomparable, failed and
non-finite readings disqualify the candidate and identify the failed stage and
provider. The owning provider adapter retains the original typed error. The
scoring interface operates on caller-owned scratch and returns the eligible
best-N prefix. Provider dispatch belongs to the staged query owner.

### Threading, Safe Points And Modes

EQS runs on the simulation owner using existing ADR-021/022 phases. BlackboardSync
drains eligible results and publishes immutable terminal records before blackboards
freeze. At the start of AiDecisionEvaluate, previously admitted queries advance;
behavior nodes then submit requests or consume published results. New admissions
never execute or callback inline. Navigation retains NavIntentCommit publication;
Physics retains its owner-thread/outside-Step or leased-snapshot query contract.
Provider completions feed a later BlackboardSync, not a reentrant decision callback.

EQS accounts for stage/item work and provider admissions; providers own their own
kernel/scratch/queue budgets. Backpressure yields before acceptance; an accepted
request is not resubmitted each tick. Host composition intersects budgets, so EQS
cannot reset provider quotas or gain capacity by nesting jobs. Background work uses
Foundation JobSystem/JobId. Owner paths never Wait/Join; workers never wait on nested
provider jobs. Only user-facing tooling gets OperationId through the application
coordinator's shared OperationStore under [ADR-010](010-job-waiting-and-operation-store-ownership.md).

| Mode | Authoritative budget | Replay guarantee |
|---|---|---|
| DeterministicWorkUnits | Bounded item/stage/provider units, stable order and declared tick deadlines | Same semantic result and publication tick for identical input/revision histories and declared provider/numeric contract |
| AdaptiveRealtime | Same hard caps plus optional wall-time targets at yield boundaries | No guarantee of identical completion timing or partial candidate populations |

Deterministic providers execute bounded kernels on their declared owning executor,
following ADR-016, instead of selecting whichever worker finished first. Unsupported
mode, unavailable pinned inputs or unreservable quotas cause explicit rejection;
there is no synchronous wait fallback. Result caching is disabled in deterministic
mode to prevent cache state changing work allocation and completion timing.
Adaptive decisions require recorded outcomes/timing for replay. Neither mode claims
unverified cross-platform physics or floating-point bit identity.

### Yielding, Partial Results And Deadlines

Running, Yielded, AwaitingAdmission and AwaitingProvider are nonterminal. A per-tick
budget cutoff retains state and resumes later; it never creates PartialSuccess.

| Terminal outcome | Meaning |
|---|---|
| Completed | All stages finished; empty eligible set is a valid result with no winner |
| PartialSuccess | Caller opted in; hard deadline or explicit FinishEarly ends the query with at least one fully filtered/scored candidate |
| TimedOut | Hard deadline reached without an allowed partial result, even if some candidates exist but complete results were required |
| Failed | Typed provider/schema/scoring failure or explicit early finish with no eligible partial candidate |
| StaleInputs | A required captured provider version can no longer be honored |
| Cancelled / Aborted | Caller cancellation / required entity or scene invalidation; no tactical winner |

Deadlines count simulation ticks from accepted submission; pause freezes tick-age.
Lifecycle invalidation/cancellation wins over completion, then eligible completions
(including on the deadline tick) precede deadline evaluation. Partial results include
coverage and snapshot provenance and never enter the result cache. Population-based
normalization cannot publish partial ranks before its required full-stage barrier.

### Cache Correctness And Quantization

Adaptive cache identity contains template AssetId/artifact digest, full canonical
inputs and effective settings, extension/numeric versions, and the sorted revision
set of every generator/context/filter/score/tie-breaker dependency. Include Navigation,
Physics, Perception, entity lifecycle, team/tag and custom-provider revisions whenever
read. Tokens identify provider instance, incarnation, revision and global/spatial scope.
Unknown reads disable caching; TTL never replaces revision validation. Hash matches
must also compare canonical key data. Scoped tokens are allowed only when the entire
read footprint is known; otherwise use a conservative global token.

ContextQuantizationPolicy defaults to Exact. Explicit spatial grids declare positive
size, rounding and accepted error tolerance. Canonical millimeter position storage
is not a mandatory cache cell size. Approximate plans evaluate the same quantized
context they hash and report that provenance; quantizing only the key would return
incorrect results. Other inputs remain exact unless their schema explicitly defines
an equally valid approximation policy. Consumers revalidate live eligibility before
acting on historical/approximate results.

Only complete successful immutable snapshots are cached. Lookup validates current
provider revisions, scene/owner authority and every entity generation/identity.
Dead entities cause a miss rather than silent filtering/re-ranking. Caller-owned
result storage survives eviction, but Actor handles do not keep entities alive.
Consumers revalidate generation and freshness before subsequent use.

### Cancellation, Capacity And Validation

Query handles include manager/scene incarnation, slot and generation. Cancellation
requests provider interruption where supported; work already running may finish.
Late results are discarded by query/request generation without invoking dead owners
or mutating replacement entities. Retained leases/payloads outlive worker access;
retired uncancellable work still consumes admission capacity until reclaimed.
Shutdown follows ADR-010 teardown rules without ordinary tick/render/transport waits.

The normative [EQS profiles](../architecture/runtime/navigation-and-ai-architecture.md#eqs-compute-profiles-and-qualification)
set explicit LowCpu/MediumCpu/HighCpu limits for concurrent queries, candidates,
work units, provider admissions/in-flight requests, stages, payloads and total bytes.
Adaptive wall targets vary by profile; graphics API selection grants no EQS capacity.
Provider quotas and GameplayAiProfile still constrain admission. Stable round-robin
slices prevent one query consuming every tick's work; overload yields or rejects,
never grows memory without bounds.

Implementation qualification covers deterministic versus adaptive histories,
yield/partial/deadline transitions, provider affinity/backpressure, cache dependency
and entity invalidation, approximate input semantics, invalid math/custom schemas,
asset versioning and cancellation of providers that cannot stop running work. These
are downstream acceptance requirements, not tests implemented by this ADR-only PR.

## Consequences

- Tactical orchestration is centralized without taking domain query execution away
  from Navigation/Physics/Perception or requiring a monolithic manager class.
- Replay guarantees match actual budgets and provider behavior rather than a seed
  and tie-breaker alone.
- Snapshot age, partial coverage, stale dependencies and cancellation are visible
  typed outcomes; retained result bytes are distinct from entity lifetime.

## Rejected Alternatives

- **Passive snapshot providers with EQS-owned pathfinding**: Duplicates domain query
  execution and breaks Navigation/Physics encapsulation.
- **Wall-clock cutoff plus unconditional deterministic partial success**: Different
  completion populations can change the winner across machines and runs.
- **Terminal result at every tick-budget cutoff**: Prevents resumable staged work.
- **Position-only keys or unconditional millimeter quantization**: Omits semantic
  dependencies or chooses an inappropriate accuracy/cache-reuse policy.
- **Guaranteed physical interruption of every provider job**: Not implementable for
  all providers; cooperative cancellation and safe result suppression are required.

## Owner-driven adaptive execution

`Horo/AI/EnvironmentQueryExecution.h` admits a single-generator query with
stable candidate-index tie breaking. The host supplies an exclusive adapter
that pins the agent, context and provider publications. The adapter owns
accepted asynchronous work and keeps that work cancellable until it returns a
completion. Only BlackboardSync consumes provider completions and publishes
terminal results. AiDecisionEvaluate advances work under the query, agent and
global allowances supplied by the host. Generation reserves reduction work,
and scoring is reserved before dispatching the candidate's first test.

At the deadline, an eligible completion can contribute to the result. Partial
results include only fully evaluated candidates. The host still owns adapter
composition, provider quotas and scheduling across queries. Deterministic
execution requires an owning-executor tick contract before it can use this seam.

`EvaluateQueryPerception` reads the exact authorized listener publication for
one live entity. It distinguishes remembered from currently sensed entities,
optionally filters by sense and exposes only the match and highest eligible
confidence. A replaced scene, publication, or candidate generation yields a
typed stale result.

The queued Physics probe lives in AISightIntegration, which already owns the
Physics capability dependency. It checks the exact host publication and returns
binary clearance from a bounded, blocking-only ray or overlap. The host supplies
local-frame geometry and retires the probe on source replacement. Capacity
backpressure admits no request. A stale binding cancels outstanding work.

The Navigation adapter is owned by AINavigationIntegration, whose explicit
dependencies are AI and NavigationRuntime. QueryNavigationProbe submits one
bounded request to the injected NavigationCoordinator and consumes only its
owner-published terminal record. It never dispatches or commits the coordinator,
resets provider quotas or accesses backend storage. The host keeps the coordinator
alive until probe cancellation/retirement, then drains provider jobs through the
existing Navigation lifecycle. Reachable paths expose finite non-negative cost;
partial or budget-limited paths remain incomparable, while exact provider errors
retain their original domain and code.

## Deterministic owner-kernel admission

AdmitDeterministic requires IOwnerQueryExecutionSource. Its final transport
methods execute bounded native generation/test kernels on the simulation owner,
charge their captured work-unit limits and retain complete facts for the following
BlackboardSync. The source cannot override the transport with worker-dependent
polling. Each kernel consumes a local allowance; exceeding its declaration produces
SchedulerBudgetExhausted and exposes no tactical winner.

The host reserves the query, agent and global slices before execution. The existing
deterministic AiTaskScheduler can reserve the complete agent allowance globally;
the query then charges the same issued allowance once when agent/global references
alias. Kernels still retain their pinned inputs and any provider-local reservations.

Every consecutive BlackboardSync must precede Advance after submission. A skipped
owner tick retires the retained result and produces SchedulerPhaseInvalid without
consuming late facts. Cancellation and input invalidation retain their existing
precedence. Results carry the admitted mode and actual terminal publication tick.

Current NavigationCoordinator and queued Physics probes remain adaptive sources.
Their deterministic admission requires a concrete bounded owner-kernel adapter
and its provider reservation contract. There is no implicit conversion from their
asynchronous probes into deterministic sources. Query result caching is absent
from this execution path.
