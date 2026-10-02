# ADR-109: Avoidance, Crowd and Renderer-Independent Budget

- **Status**: Proposed
- **Date**: 2026-09-02
- **Supersedes**: None
- **Scope**: Grounded-agent path following, local safe-velocity computation, crowd-provider selection, deterministic and best-effort modes, project scale and quality profiles, admission, overload, failure, networking, persistence, observability and lifecycle
- **Issue**: [NAV-006.1](https://github.com/HoroCore/horo-engine/issues/1279)
- **Jira**: [HORO-1279](https://horo-engine.atlassian.net/browse/HORO-1279)
- **Related**: [ADR-010](010-job-waiting-and-operation-store-ownership.md), [ADR-016](016-navigation-target-ownership-and-dependency-boundary.md), [ADR-022](022-ai-fixed-tick-order-authority-and-simulation-budget.md), [ADR-024](024-perception-ownership-sense-policy-and-budget.md), [ADR-085](085-physics-shape-authoring-cook-and-runtime-boundary.md), [ADR-104](104-default-navigation-provider-and-recast-detour-adoption.md), [ADR-107](107-navigation-query-consistency-and-snapshot-ownership.md), [ADR-108](108-dynamic-overlay-carving-and-tile-rebuild-policy.md)
- **Normative documents**: [Navigation and AI Architecture](../architecture/runtime/navigation-and-ai-architecture.md), [Concurrency and Jobs](../architecture/foundation/concurrency-and-jobs.md)

## Context

Grounded navigation has three adjacent operations that must not collapse into one
provider feature. Pathfinding proves a route over a topology snapshot. Local
avoidance selects a safe desired velocity from a preferred path velocity and nearby
facts. Character/Physics validates and commits physical movement. A crowd library
may help with the middle operation, but it cannot own agent identity, gameplay
intent, transforms or simulation scheduling.

ADR-104 selected DetourCrowd only as the optional `DetourLocalAvoidance`
capability and excluded it from the initial deterministic tier. ADR-108 established
that moving agents use avoidance rather than topology carving and named
collision-safe stop as the baseline fallback. The remaining architecture still
uses “RVO” loosely, couples crowd enablement to a broad gameplay profile and does
not fix separate capacity and quality envelopes or overload behavior.

Renderer labels are especially unsuitable for this policy. OpenGL, Metal, Vulkan,
D3D12 and `NullRenderer` say nothing about authoritative host CPU, memory, fixed-
tick rate, agent density or provider qualification. A headless dedicated server
may own the largest crowd, while a visually high-end client may own no AI agents.

This ADR fixes the 1.0 provider strategy, separates the three authorities, defines
explicit deterministic/best-effort modes and establishes renderer-independent
project profiles with finite admission and failure semantics.

## Decision

### 1. Path, safe velocity and physical movement have distinct owners

| Stage | Owner | Input | Output | Forbidden authority |
|---|---|---|---|---|
| Route/corridor and preferred velocity | NavigationRuntime path-following policy over ADR-107 snapshots | High-level movement intent, agent profile, path/corridor, topology/overlay generations | Bounded preferred velocity plus route provenance | Cannot write transforms or decide collision response |
| Local safe velocity | `CrowdSimulationCoordinator` plus optional `INavigationCrowdBackend` | Immutable avoidance batch with preferred velocities, neighbors and logical obstacle segments | Per-agent safe desired velocity and typed disposition | Cannot mutate topology, gameplay intent, ECS identity or Physics state |
| Physical locomotion | Character/Physics | Desired velocity, controller state and collision world | Authoritative position, velocity and contacts | Cannot invent a navigation route or rewrite the crowd batch |

Gameplay owns movement goals, formation/lane/tactical policy and whether an agent is
authoritative. NavigationRuntime owns logical `CrowdAgentHandle` identity, grouping,
snapshot capture, project-profile admission, stable scheduling, output validation
and publication at `NavIntentCommit`. A provider owns only its private transient
kernel state while executing an admitted batch.

The safe-velocity stage may slow, sidestep or stop an agent within its declared
acceleration/speed envelope. It cannot advance beyond the active corridor, traverse
a disabled link, ignore an ADR-108 blocker or claim that a route exists. Character/
Physics may further constrain or stop the result and remains the final movement
authority.

### 2. The 1.0 baseline uses the provider seam, not a new Horo RVO/ORCA solver

For best-effort products, ADR-104's pinned DetourCrowd adapter is the first
production `DetourLocalAvoidance` provider. Its adaptive velocity-sampling behavior
is exposed only as the Horo safe-velocity contract. Horo does not describe it as
ORCA, promise reciprocal collision-free motion or expose Detour sample parameters
through public/durable types.

Horo does not implement a separate RVO/ORCA solver for 1.0. An ORCA, RVO or another
safe-velocity implementation may later satisfy `INavigationCrowdBackend`, but it
must declare algorithm identity, numeric/build fingerprint, supported quality
envelopes, capacity, threading and determinism qualification. Provider selection is
a host composition decision made before Scene activation; it is not changed by a
graphics setting or per-agent string.

Path following without local avoidance remains supported. It does not allocate a
provider crowd instance. Products that require best-effort avoidance must compose a
qualified provider; products that do not require it use the explicit fallback in
Section 7 rather than an unbounded hidden implementation.

### 3. Avoidance execution mode is explicit and independently admitted

```cpp
enum class AvoidanceExecutionMode : uint8_t {
    Disabled,
    BestEffortBounded,
    DeterministicQualified,
};
```

`Disabled` performs no local safe-velocity solve. `BestEffortBounded` admits finite
provider batches as Foundation jobs and applies only results eligible for their
declared target tick. `DeterministicQualified` requires a provider/profile/build/
platform tuple with recorded deterministic evidence and executes bounded kernels in
stable order on the owning deterministic executor; it never selects output by
worker completion time or wall-clock remainder.

The initial DetourCrowd adapter is `BestEffortBounded` only. It cannot satisfy
`DeterministicQualified`. A deterministic simulation may therefore choose
`Disabled` plus `CollisionSafeStop`, or fail product/Scene admission when avoidance
is required. Selecting deterministic AI scheduling does not silently relabel a
best-effort provider as deterministic.

Deterministic qualification states its exact scope. `SameBuildSamePlatform` output
repeatability is distinct from cross-build or cross-platform lockstep. Required
numeric mode, fixed delta, provider fingerprint, agent ordering, topology/profile
fingerprints and golden evidence are part of the capability. Unsupported scope is
a typed composition/admission failure.

### 4. Scale and quality are independent project dimensions

`NavigationCrowdProfile` references one scale profile and one quality profile. The
scale profile owns capacity, scheduling and memory/work envelopes. The quality
profile owns per-agent neighborhood/constraint work. A project may use a small
dense crowd or a large conservative crowd without pretending either is a graphics
feature tier.

```cpp
enum class NoAvoidancePolicy : uint8_t {
    CollisionSafeStop,
    PathFollowingWithPhysicsGuard,
    AvoidanceRequired,
};

struct NavigationCrowdProfile {
    NavigationCrowdScaleProfileId scale;
    AvoidanceQualityProfileId     quality;
    AvoidanceExecutionMode        executionMode;
    NoAvoidancePolicy             noAvoidancePolicy;
};
```

Built-in scale defaults are exact admission envelopes:

| Scale profile | Logical agents | Provider-resident agents | Avoidance evaluations/tick | Batches/tick | Pending result ticks | Provider persistent memory |
|---|---:|---:|---:|---:|---:|---:|
| `CrowdSmall` | 64 | 64 | 64 | 1 | 1 | 4 MiB |
| `CrowdMedium` | 512 | 512 | 256 | 4 | 2 | 32 MiB |
| `CrowdLarge` | 2,048 | 2,048 | 512 | 8 | 3 | 128 MiB |
| `CrowdDedicated` | 4,096 | 4,096 | 1,024 | 16 | 3 | 256 MiB |

Provider-resident capacity is a ceiling, not a promise that every agent evaluates
each tick in best-effort mode. `Logical agents` includes admitted agents using a
fallback while provider slots/results are unavailable. Pending snapshots, output
buffers, spatial indices, retired provider state and allocator overhead are charged
to the memory envelope; native allocations cannot be hidden outside it.

Built-in quality defaults use backend-neutral maximum facts/work:

| Quality profile | Neighbor candidates/agent | Obstacle segments/agent | Constraint/sample work units/agent | Velocity candidates returned | Horizon ticks |
|---|---:|---:|---:|---:|---:|
| `AvoidanceOff` | 0 | 0 | 0 | 0 | 0 |
| `AvoidanceConservative` | 8 | 8 | 32 | 1 | 8 |
| `AvoidanceBalanced` | 16 | 16 | 96 | 1 | 12 |
| `AvoidanceDense` | 32 | 24 | 192 | 1 | 16 |

A work unit is a Horo admission unit, not a native loop count. Each provider
descriptor declares the maximum Horo envelope it can implement and a reviewed
mapping from these limits to its private knobs. A provider must reject an envelope
it cannot honor; it cannot silently lower or exceed it. Only one validated velocity
candidate is published per agent even if a private algorithm samples many.

Projects may define custom profiles, but every field is finite, validated at load
and included in the runtime/capability fingerprint. Larger values require measured
workload evidence; the built-ins are initial policy defaults, not performance
claims or automatic hardware detection.

`AvoidanceOff` requires `Disabled`; enabled execution requires a non-Off quality.
`AvoidanceRequired` cannot pair with `Disabled`. In deterministic mode, the number
of agents requiring avoidance must fit both provider-resident and per-tick
evaluation caps; other logical agents must explicitly use a no-avoidance policy.
Invalid combinations reject profile/product admission.

### 5. Graphics and automatic hardware probing grant zero crowd capacity

The application/project configuration selects the `NavigationCrowdProfile` for
each authoritative simulation role. Graphics backend, render feature tier, GPU
vendor, resolution, frame rate, editor viewport quality and headless state are not
inputs to profile selection or admission.

CPU/memory measurements may inform a developer's authored profile choice, but the
runtime does not silently switch profiles by core count, benchmark, battery state or
thermal state. Such automatic changes would alter authoritative population and
scheduling. A host may reduce optional best-effort frequency only through the
declared overload policy while retaining the same caps and observable disposition.

Standalone, dedicated server and local-authority test hosts may select any profile
their product qualifies. A network client that only interpolates server-owned
agents selects no authoritative crowd profile regardless of render capability.
`NullRenderer` neither implies `NavigationNull` nor reduces a dedicated server's
crowd budget.

### 6. Admission and tick selection are stable and bounded

Agent creation first reserves a generation-safe logical slot, profile/accounting
capacity and, when required, a provider slot. Exceeding a hard logical/provider/
memory cap returns `CapacityExceeded`; no partially registered ECS/provider agent
survives. Optional-provider admission may retain a logical agent under the declared
fallback, with an explicit `AvoidanceUnavailable` disposition.

At each fixed tick, the coordinator builds immutable batches from committed
authoritative transforms, velocities, radii, preferred velocities, logical
obstacles, topology/overlay/profile/origin generations and stable agent IDs. It
excludes an agent's own blocker identity and deterministically clamps malformed or
out-of-envelope input before provider invocation or rejects the batch.

Avoidance layer identity is project-stable and distinct from obstacle/query layers.
An owner-safe-point capture validates the complete layer table and each enabled
agent's directed avoid mask and finite [0, 1] right-of-way priority together with
motion facts. Directed masks may be non-reciprocal. Priority biases the optional
sampler's preference for its own desired velocity, while every admitted neighbor
still receives conservative collision validation. Neither mask nor priority grants
gameplay/network authority, changes scheduling priority, or weakens Character/
Physics movement authority. A failed capture publishes no partial tick policy.

`DeterministicQualified` processes every required agent each tick; if the exact
declared work cannot be admitted, the mode fails rather than time-slicing.
`BestEffortBounded` selects at most the scale limit by authored priority class,
overdue age and stable agent ID. Distance/significance may form an authored
priority class, but raw camera distance, visibility or render LOD never does.
Deadline aging prevents an admitted low-priority agent from starving.

One provider instance has one writer per batch/update. Independent groups may run
in parallel only when their native state and output storage are disjoint. Workers
receive owned snapshots and output buffers; they never read live ECS/Physics
containers, publish handles or call gameplay.

### 7. No-avoidance and overload behavior fail safely

`CollisionSafeStop` is the engine default. An agent with no eligible safe velocity
publishes zero local locomotion intent, while externally imposed platform/gravity
motion remains Character/Physics-owned. `PathFollowingWithPhysicsGuard` may publish
the corridor-clamped preferred velocity, but Character/Physics must sweep/collide
and stop it; this policy is opt-in and makes no collision-free crowd guarantee.
`AvoidanceRequired` rejects product/Scene/agent admission when the requested mode,
provider, quality or capacity is unavailable.

In best-effort overload, an unselected or late agent may reuse the last validated
safe velocity for at most the profile's pending-result tick limit after rechecking
path/overlay/world generations. It is clamped toward stop each missed tick. After
that bound, or after any blocker/path invalidation, it uses the no-avoidance policy.
There is no indefinite stale velocity, queue growth or unbudgeted synchronous retry.

Provider failure, invalid numeric output, capacity loss or cancellation rejects the
affected batch atomically. Other validated groups may publish independently only if
the grouping boundary was declared before execution. Results distinguish
`AvoidanceApplied`, `AvoidanceSkipped`, `AvoidanceUnavailable`, `CapacityExceeded`,
`StaleSnapshot`, `InvalidAgent`, `Cancelled` and `ProviderFailure`; a fallback is
recorded separately and never reported as provider success.

### 8. Publication uses target ticks and current generation validation

Best-effort work captures a target publication tick and enters Foundation
`JobSystem`; completion order has no authority. At `NavIntentCommit`, the owner
consumes eligible batches in target-tick/group/agent order and revalidates world,
Scene, agent slot, path/corridor, topology, overlay, obstacle, profile, origin and
input-transform generations.

A late or stale result cannot move a replacement agent. It is retired and the
fallback applies. Publication writes only Navigation-owned desired-velocity state
for Character/Physics consumption in the declared locomotion phase. It does not
write transforms, directly step Physics or mutate the provider while a worker reads
it.

Deterministic mode invokes its qualified bounded kernel on the owning executor in
stable order and publishes for the declared tick without waiting on a background
job. A cheap/disabled path still follows the same phase and result semantics; agent
registration or submission never invokes gameplay callbacks inline.

### 9. Formation, lanes, topology and rendering remain outside avoidance

Formation membership, slots, leader intent, road lanes, traffic rules and density
tactics are Gameplay/AI policies that produce preferred velocities or navigation
goals. The safe-velocity provider may avoid immediate collisions but does not own
group decisions. Traffic/road graph routing remains a separate future navigation
domain.

ADR-108 dynamic overlays and topology candidates provide obstacle/link facts. An
avoidance provider cannot carve tiles, toggle a door, request streaming or treat a
neighbor grid as authoritative reachability. Conversely, topology changes do not
move an agent.

Rendering may consume a bounded immutable debug projection after simulation commit.
Visibility/culling does not register/deregister agents, choose quality, skip
authoritative work or expose native crowd indices. Debug draw absence in headless or
shipping products has no simulation effect.

### 10. Network and save state preserve semantic authority

The authoritative host runs crowd avoidance for server-owned agents. Clients
receive public locomotion/transform state under the replication contract; they do
not receive private neighbor sets, samples, provider indices or avoidance grids.
Client prediction for an eligible locally owned agent requires a separately
qualified matching provider/profile fingerprint or uses the product's correction
policy.

Save state records durable gameplay movement goals and any declared agent settings.
Logical/provider crowd slots, last safe velocity, pending batches, neighbor grids,
work queues, native samples and failure counters are transient. Restore recreates
logical agents against the selected project profile after Scene/topology readiness;
provider-native identity is never serialized.

Changing scale, quality, execution mode or provider while a Scene is active requires
a host-owned staged replacement transaction. It closes admission, constructs and
validates a new private population from semantic state, atomically switches at a
fixed-tick boundary and retires the old provider after leases drain. A settings UI
cannot mutate live provider knobs directly.

### 11. Observability measures budgets without becoming policy

Per-world/profile metrics include admitted logical/provider agents, evaluated/
skipped/late/fallback agents, batch counts, neighbor/obstacle facts, Horo work units,
queue depth, persistent/staging/retired bytes, result age and typed failures. Timing
is sampled under observability policy and can tune future authored profiles; it does
not change current authoritative selection or output.

Diagnostics identify Horo world/profile/group/agent generations and bounded reason
codes. They do not expose provider pointers, native indices, private gameplay goals
or server-only neighbor facts to unauthorized clients. Renderer frame metrics and
GPU tiers are not crowd labels.

### 12. Replacement and shutdown preserve ownership

Scene/world teardown closes agent/batch admission, invalidates logical generations,
cancels pending work, rejects late completion, retires output/snapshot state, removes
provider agents on its owning thread and destroys the provider only after all leases
and jobs finish under ADR-010.

Provider failure or replacement never transfers native agent indices/state. The
coordinator reconstructs private state from current Horo semantic agents and
committed transforms. Memory remains charged while retired batches/provider state
are still live; timeout cannot force-free storage a worker may access.

### 13. Qualification covers modes, profiles and fallbacks

Required evidence includes:

- path/preferred velocity, safe velocity and Character/Physics movement remaining
  separate under success, stop, collision, cancellation and teardown;
- exact built-in scale/quality initialization, invalid/custom-envelope rejection,
  hard capacity and native-allocation accounting;
- DetourCrowd best-effort composition and absence, plus proof it is never admitted
  as `DeterministicQualified`;
- deterministic provider fixtures with stable agent/fact ordering, target ticks,
  worker-count variation and declared numeric/build scope;
- best-effort priority/age selection, maximum stale reuse, clamped stop, queue/
  memory/work overload and no starvation;
- every `NoAvoidancePolicy`, including required-capability admission failure and
  Physics-guarded path following;
- stale world/Scene/agent/path/topology/overlay/profile/origin result rejection and
  generation-safe slot reuse;
- dual-role self-exclusion, neighbor/obstacle fact caps, malformed numeric provider
  output and atomic per-group failure;
- dedicated `NullRenderer`, presentation client and graphically high-end local host
  proving identical authored crowd profile behavior; and
- save/network/provider replacement and shutdown without native identity, private
  neighbor data, callback or lifetime leakage.

## Consequences

### Positive

- Navigation routes, local avoidance and physical movement now have one owner each.
- The 1.0 provider/algorithm claim is precise: DetourCrowd is optional best-effort,
  while a deterministic provider requires separate qualification.
- Capacity and quality can be tuned independently without graphics-tier coupling.
- Overload, provider absence and no-avoidance behavior are explicit and observable.
- Headless servers and presentation clients select profiles by simulation role.

### Costs

- The coordinator needs logical agents, immutable fact batches, stable scheduling,
  generation validation and fallback state outside the provider.
- Provider adapters must map backend-neutral work envelopes and account native
  memory instead of exposing their own quality knobs directly.
- Deterministic products have no 1.0 crowd avoidance unless a provider is separately
  implemented and qualified.
- Large project profiles require workload-specific measurement before adoption.

## Rejected Alternatives

### Make DetourCrowd mandatory for every navigation product

Rejected because path queries and path following do not require local crowd state,
runtime-only/headless products vary, and the initial provider is not qualified for
deterministic fixed-tick simulation.

### Call the initial provider ORCA/RVO and promise collision-free motion

Rejected because DetourCrowd uses its own adaptive velocity-sampling behavior and
Character/Physics remains final collision authority. Generic terminology must not
overstate an algorithm or guarantee.

### Implement a second Horo-owned ORCA solver for 1.0

Rejected because it would add a second complex numeric kernel and qualification
burden before the provider seam is exercised. A future implementation can compete
behind the same contract.

### Infer crowd scale or quality from graphics/render tier

Rejected because presentation capability does not determine authoritative CPU,
memory, host role or agent density, and dedicated servers may have no renderer.

### Auto-scale authoritative capacity from hardware probes

Rejected because machines would admit different populations and scheduling without
an authored product decision. Measurements inform profile authoring, not authority.

### Update every agent every tick in best-effort mode regardless of budget

Rejected because it creates unbounded frame work and hides overload. Stable
priority/aging and safe fallback make degradation finite and observable.

### Reuse the last safe velocity indefinitely

Rejected because topology, blockers and neighbors may have changed. Reuse is
generation-checked, clamped and bounded before the selected no-avoidance fallback.
