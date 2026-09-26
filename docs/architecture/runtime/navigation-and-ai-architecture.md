# Navigation And AI Architecture

## Purpose

This document defines grounded navigation (NavMesh), pathfinding, tactical environment queries, and AI subsystems
for Horo Engine. It covers navigation mesh generation, runtime pathfinding,
dynamic obstacle avoidance, AI perception, behavior integration, crowd
simulation, fixed-tick simulation phase ordering, network authority roles,
and project-configured simulation budget profiles.

## Target Ownership And Architecture Boundaries

Navigation has four planned CMake targets. They are target contracts for downstream
implementation, not a claim that all four already exist in the current CMake tree.
Arrows below denote compile/link dependencies; runtime dispatch uses injected interfaces.

```text
NavigationRuntime -------> NavigationApi -------> Foundation (includes SceneMath)
NavigationRecastDetour ---> NavigationApi
NavigationNull ----------> NavigationApi

Game / Server host ------> NavigationRuntime + selected runtime provider
Cook / Bake host --------> selected mesh builder + Asset Pipeline

Host creates provider(s), then injects typed interfaces into coordinators.
NavigationRuntime never links or includes a concrete provider.
```

| Target | Direct Horo dependencies | Private vendor dependency | Public-header owner |
|---|---|---|---|
| `HoroEngine::NavigationApi` | `HoroEngine::Foundation` | None | Neutral types, requests, handles, provider interfaces |
| `HoroEngine::NavigationRuntime` | `NavigationApi`, `Foundation` | None | `NavigationCoordinator.h`, `NavigationDynamicRegistry.h` |
| `HoroEngine::NavigationRecastDetour` | `NavigationApi`, `Foundation` | Selected Recast/Detour components only | Horo-only `Backends/RecastDetourProvider.h` factory/descriptor |
| `HoroEngine::NavigationNull` | `NavigationApi`, `Foundation` | None | Horo-only `Backends/NullProvider.h` factory/descriptor |

`SceneMath` is a Foundation-owned contract (`Horo/Math/SceneMath.h`), not a separate
CMake target. These dependencies provide `WorldCoordinate64`, `Vec3`, `Aabb`, and
`Quaternion`; navigation does not invent another math library. Asset, world-streaming,
and scene adapters are composed by the host; they do not add reverse dependencies
from these four navigation targets to application, Gameplay, Rendering, or GUI.

### Default Provider And Module Profile

[ADR-104](../../adr/104-default-navigation-provider-and-recast-detour-adoption.md)
selects `recastnavigation/recastnavigation` commit
`9f4ce64458dfae86e1239c525ddc219c4e9e06f1` as the first production
grounded NavMesh provider. Detour is the required real runtime query/topology
baseline. Recast is required only in bake/cook compositions. DetourTileCache is
the optional `DynamicTileCarving` capability and DetourCrowd is the optional
`DetourLocalAvoidance` capability; neither is implied by selecting real pathfinding.
DebugUtils, RecastDemo and examples are absent from product targets/packages.

The exact commit, private 64-bit poly-reference build and other locked native
options form the provider fingerprint. Public and durable identity remains Horo-
owned; a provider pin or option change invalidates matching derived caches and
requires the ADR-104 upgrade/recook/qualification process.

### NavigationApi: Consumer Contracts

`include/Horo/Navigation/` contains neutral queries/paths, build settings, immutable
`NavMeshData` views, obstacle descriptions, navigation identities, typed
`NavigationErrorCode` / `Result<T>` results, and `CrowdAgentConfig` / `CrowdAgentHandle`.
Crowd types belong here because gameplay consumers need them to request agent creation
and movement. Scheduler queues, logical agent storage, and vendor crowd state do not.

`NavMeshData` is the version-one immutable cooked grounded-surface contract. Its
fixed header declares the exact neutral format, little-endian byte order, compression,
profile identity/build geometry/digest, global-origin coordinate frame, metric tile grid, table counts, encoded
and decoded byte sizes, and payload checksum before any decoded-table allocation.
Project limits may lower but never exceed compiled ceilings. Every tile has one stable
`(x, z, layer)` key, finite bounds, contiguous vertex/polygon/adjacency/link/provenance
ranges and its own checksum, so a streaming adapter can validate and address a tile
without accepting neighboring tiles. Complete artifact publication additionally requires
strictly ordered unique keys and an exact gap-free partition of every declared table.

Portable vertices, polygons, adjacency, traversal areas, off-mesh links and source
provenance remain the semantic authority. Optional provider payload bytes occupy a
separate opaque byte table with independent fingerprint, format version, endian,
compression, decoded-size bound and checksum metadata. A provider receives those bytes
only on an exact compatibility match; mismatch is a typed cooked-version failure and
absence is distinct from corruption. Provider payloads can always be discarded without
changing portable topology meaning.

Separate provider capabilities use narrow Horo-owned contracts (`NavigationBackend.h`
and `NavigationMeshBuilder.h`):

- `INavigationQueryBackend`: bounded spatial queries over pinned immutable topology.
- `INavigationTopologyBackend`: staged tile installation/removal and obstacle carving;
  produces a candidate topology without mutating data being queried.
- `INavigationCrowdBackend`: optional bounded crowd/avoidance batch computation.
- `INavigationMeshBuilder`: offline/background bake from owned geometry and settings.

There is no monolithic `INavigationBackend` combining all four lifecycles. A provider
may implement several capabilities, but hosts inject only those required by the use case.
Missing required capabilities fail composition; optional capabilities return `Unsupported`.
A runtime-only server composes query/topology and optionally crowd, without the builder.

### NavigationRuntime: Policy, State, And Scheduling

`src/runtime/navigation/runtime/` owns `NavigationCoordinator`, logical obstacle/agent
state, query admission/cache policy, hierarchical search policy, and
`CrowdSimulationCoordinator`. It schedules Foundation jobs and publishes validated
results at the owning simulation phase. It never discovers concrete providers, registers
itself through a global singleton, performs vendor carving, or chooses cell residency.

`NavigationWorldLifecycle` is the host-facing per-Scene publication authority. The host
stages a fully initialized provider record tagged with the exact Scene incarnation, Scene
generation, navigation world, and topology generation. `CommitAtSafePoint` performs the
only publication swap and leaves the previous world unchanged on stale identity or bounded
retirement-capacity failure. Pause closes admission without cancelling the unchanged world;
replacement, unload, and shutdown atomically revoke admission and request cooperative
cancellation. Worker-visible read leases pin immutable records after logical revocation,
so provider storage is reclaimed only after the final lease drains. Lifecycle mutation and
lease acquisition are owner-thread operations; acquired leases and cancellation tokens may
cross workers. Shutdown never blocks a frame thread waiting for those workers.

`NavigationDynamicRegistry` is the provider-neutral owner-thread authority for Scene-scoped
obstacle and semantic modifier contributions. A contribution carries a stable logical
identity plus exact Scene/world, owner-generation, source-revision, shape, layer, priority,
and fixed-tick evidence. Registration, replacement, and removal are value-only staged
commands; `CommitAtSafePoint` is the only normal publication path, while
`ReplaceSceneAtSafePoint` clears the old Scene publication and advances slot generations.
Duplicate identities, stale owner/source revisions, old handles, excessive update rates,
and profile-bound counts fail deterministically without changing the active publication.
Snapshots copy the bounded active records into immutable storage that query and avoidance
jobs may retain without a Scene or provider lifetime lease; their exact Scene binding and
registry revision remain the currentness fence for later result publication.

`NavigationRuntimeQueues` is the prepared transport boundary around that lifetime authority.
It owns separate power-of-two command, query, and completion rings with explicit capacities
and one aggregate storage ceiling. Publication is bounded, nonblocking and allocation-free;
the record moves only after a producer owns a slot, so full, contended, invalid, and closed
outcomes leave caller ownership intact. Query records carry their world lease, request
cancellation, and operation context by value. Completion records carry exact Scene, world,
topology, request-handle, and admission-order fences. No queue executes provider work or
callbacks. Shutdown closes command/query admission, joins producers and provider jobs under
the host shutdown policy, closes completion publication, and drains terminal candidates on
the owner before destroying Scene or provider storage.

`DynamicObstacleOverlay` records logical Horo obstacle changes; the topology backend
owns concrete carving and private tile-cache mutation. The crowd coordinator selects
agents, stable ordering, budgets, and target ticks; the crowd backend owns provider-specific
avoidance state and kernels. Typed snapshots/commands cross this boundary, never `dt*`
pointers. Navigation requests velocities; character locomotion owns transform/physics writes.

### NavigationRecastDetour: Private Execution

`src/runtime/navigation/backends/recast_detour/` owns all `rc*` / `dt*` objects,
including mesh/query instances, tile caches, crowd instances, and build scratch. It
translates Horo values and typed errors at the interface boundary. Query instances/scratch
are exclusively leased per job; mutable crowd/tile-cache instances have one writer and
are never concurrently queried while being mutated.

The initial `HoroEngine::NavigationRecastDetour` implementation accepts a bounded,
borrowed Horo polygon-topology descriptor at composition, validates and copies it
transactionally, and prepares a fixed pool of Detour query objects and scratch. Calls
beyond the declared lease count return `AdmissionRejected` without waiting. Exact world
and topology generations are checked before native work; malformed topology, allocation
failure, cancellation, and stale requests remain typed Horo failures. The descriptor is
an activation seam, not the durable NavMesh artifact schema owned by NAV-002.7.

Private runtime and build source groups are separate. A runtime-only provider composition
excludes Recast voxelization/build objects and their private link dependencies; enabling
the bake capability adds them for tools. Runtime topology/crowd support retains only its
required Detour components. This is an implementation gate, not reliance on linker dead
stripping to hide an always-linked builder.

The host activates inert provider descriptors or calls Horo-only factories and injects
owned query/topology/crowd interfaces into the coordinator. Selection happens before scene
activation. Stop admission, cancel/drain outstanding work under ADR-010, release snapshots,
then destroy providers before unloading their module; no callback or lease may outlive it.

The selected Recast provider also exposes an `INavigationMeshBuilder` implementation for
one synchronous, already-admitted tile invocation. The builder owns no scheduler, cache,
artifact publication or source lifetime. It runs the pinned walkability-filter, compact-
heightfield, region, contour and polygon stages against one borrowed canonical partition,
then returns an owned provider-neutral tile result. A successful result is explicitly
`Built` or `Empty`; typed capacity failure never mutates caller-owned or neighboring tile
state. Provider log categories are reduced to bounded neutral warning counts, and output
statistics contain no native Recast types or timing-dependent values.

Recast tile builds own separate contexts, intermediate data and allocator domains.
Each Detour query job exclusively leases one `dtNavMeshQuery` and scratch/node
pool against a pinned immutable mesh snapshot. Mutable native topology,
`dtTileCache` and `dtCrowd` instances have one owner/writer per batch. The provider
creates no thread pool; all admission and execution use Foundation JobSystem.

### NavigationNull: Feature Absent, Not Fake Navigation

`src/runtime/navigation/backends/null/` implements neutral capability absence.

| Operation | Null result |
|---|---|
| Path, projection/nearest-point, or navigation raycast | `NoNavigationData` |
| Build, install/carve topology, or create/update crowd agent | `Unsupported` |

Null never reports a successful straight-line route or pretends to evaluate geometry.
The same coordinator admits requests and publishes terminal results at the same scheduled
phase as a real provider; a cheap internal result does not invoke a caller inline. Cancellation,
capacity, stale-handle, and shutdown validation are shared. Tests needing successful paths or
scripted delays inject a separate test-only fake, not different production Null semantics.

Headless describes presentation, not navigation capability. Dedicated/headless hosts may
select Recast/Detour for real navigation, deliberately select Null for feature absence, or
omit navigation entirely. `NullRenderer` does not select `NavigationNull`.

### Foundation Qualification

Navigation foundation changes are qualified against one shared provider-neutral
contract. The deterministic test provider and the production Null and Recast/Detour
providers must preserve the same capability validation, cooperative cancellation,
and deterministic-result rules. Linux CI separately builds the narrow navigation
target set with the default provider enabled and omitted; the ordinary platform
matrix proves the enabled composition on Linux, macOS, and Windows.

Runtime qualification repeatedly replaces active worlds while old worker leases and
completion records remain retained. Each old lease must be logically revoked before
its provider is reclaimed, and every stale completion must remain distinguishable by
its exact Scene/world/topology fences and be drained without publication. Queue tests
admit exactly the platform-specific `RequiredStorageBytes` projection and instrument
all prepared command, query, and completion directions for zero frame-hot allocation.

## Subsystem Decoupling

Editor viewport camera navigation remains in Gui; orbit/pan/fly/focus code never depends
on NavigationApi or NavigationRuntime. A separate editor adapter may extract Horo debug
geometry for a NavMesh overlay, but navigation never reads camera/UI state or links rendering.
Gameplay owns decision graphs, perception, blackboards, and tactical scoring; navigation owns
spatial evaluation. Gameplay consumes typed admission/query interfaces, and navigation never
includes behavior nodes or script bindings. Host adapters bridge Assets, World Streaming,
and scene lifetime without making them providers' dependencies.

[ADR-137](../../adr/137-terrain-foliage-ownership-data-tier-and-lifecycle.md)
keeps Terrain/Foliage on that decoupled boundary. Terrain produces immutable,
generation-tagged cooked topology/source or runtime-overlay evidence; Navigation owns
tile preparation, provider objects, query consistency, publication and retirement.
Visual terrain or a committed hole/deformation cannot mutate live NavMesh state or
imply navigation readiness. ADR-108 conservative exclusion plus explicit recook/rebuild
governs topology change, and aggregate scene/cell activation checks exact generations.

## Navigation Integration Contracts

### Submission, Jobs, And Publication

[ADR-107](../../adr/107-navigation-query-consistency-and-snapshot-ownership.md)
requires one atomically published `NavigationWorldSnapshot` combining exact Scene/
world incarnation, topology and tile generations, streaming fences, logical
obstacle overlay, filter/profile configuration, origin epoch, coverage and private
provider leases. `NavigationReadSnapshot` keeps referenced memory valid but does
not keep it logically current or resident. A tile/root candidate publishes wholly
at a safe point; readers observe the complete old or new root, never a partial
replacement.

Queries declare `ExactSnapshot`, `CurrentAtPublish` or bounded `CurrentOrRetry`
consistency independently from `RequireComplete`/`AllowPartial` coverage. Immediate
queries are owner-thread, allocation/I/O/wait-free kernels with hard work/output
bounds. Full, hierarchical and multi-tile paths use the bounded coordinator and
Foundation JobSystem. Workers receive one immutable read lease and exclusive
scratch, enqueue owned completion records and never complete a handle or mutate
gameplay inline.

`NavigationCoordinator` admits owned typed requests with captured navigation-world/scene
incarnation, caller authority, cancellation, configuration, topology/obstacle revision, target
tick, and bounded output/node-expansion budgets. Admission returns `Result<NavRequestHandle>`;
closed admission or exhausted queue/result capacity returns `AdmissionRejected` with no
accepted job. The generation-checked request handle identifies a bounded result record and
correlates with a Foundation `JobId`; it is not a second task scheduler.

All background query/crowd/build work uses Foundation `JobSystem`, with exclusive scratch and
immutable input leases. Completion enqueues data, never a callback that writes live scene
state. Owner-thread `NavIntentCommit` consumes eligible results in stable request/agent order,
rechecks incarnation, handle generation, authority, cancellation, and relevant revisions, and
publishes paths/desired velocities before character locomotion. Stale results return
`StaleSnapshot` or `InvalidHandle` and may be resubmitted within budget; they are never applied
to replacement agents or tiles. A held path/corridor must be revalidated before later use too.

ADR-018 `OwnerThreadNextFrame` console handlers submit typed navigation commands for this
phase; they do not introduce a second mutation phase in `PreUpdate` / `DebugPhase`. Heavy
bake/export commands use asynchronous `OperationId` progress per ADR-010. The application
operation coordinator alone mutates the shared `OperationStore`; it may aggregate multiple
jobs. Per-agent tick jobs need no user-visible operation record. Main/editor/transport owners
never `Wait()`/`Join()` navigation jobs; allowed teardown waits follow ADR-010. No second
thread pool, per-backend executor, or independent OperationStore is permitted.

Deterministic fixed-tick mode cannot select results according to which worker happened to
finish. It runs bounded deterministic navigation kernels in stable agent/request order at the
declared tick, on the owning executor, without waiting on jobs. The request still completes
through scheduled publication, never inline at submission. Required topology must already be
pinned; unavailable data has a deterministic typed outcome. A provider unable to meet the
bounded deterministic kernel contract fails mode selection explicitly. Best-effort mode may
run the same kernels as jobs; pending work retains its prior valid intent or a configured safe
stop, never an incomplete new result. Completion timing is not an authority override.

Scene teardown closes admission, cancels pending work, rejects late results, then retires
resources after readers finish. Query cancellation before publication prevents result
application; cancellation after publication does not retroactively undo an applied intent.

Only the simulation/Navigation owner publishes asynchronous terminal results at
`NavIntentCommit`, in stable request order. It revalidates handle, cancellation,
world/Scene authority and every topology/tile/overlay/filter/origin dependency.
Results distinguish `CompletePath`, opt-in `PartialPath`, proven `NoPath`,
unavailable `NoNavigationData`, `StaleSnapshot`, invalid identity, cancellation,
capacity, unsupported capability and provider failure. Missing/evicted coverage is
never `NoPath`; retained paths revalidate before later movement use.

### Obstacle And Provider State Synchronization

The owner applies bounded obstacle command batches at `NavIntentCommit` to logical
`DynamicObstacleOverlay` state and publishes an immutable obstacle revision. Carving occurs
only in the injected topology backend, on an isolated candidate. Readers pin an immutable
pair of topology and overlay revisions; no worker reads a container concurrently modified
by gameplay or by another job. Candidate topology is installed at a later safe point only
if its base revisions still match. Until ready, newly blocking obstacle intent uses a
conservative avoidance/stop overlay so agents cannot follow newly obstructed corridors.

Each mutable `dtCrowd`/tile-cache instance is exclusively owned for its batch; sharing an
instance between parallel updates is forbidden. Parallel queries lease separate query/scratch
objects against read-only mesh snapshots. Old snapshots and backend storage are retired only
after all readers release them. No blocking writer wait is introduced on the frame path.

`NavQueryCache` is bounded and thread-safe. Keys include query/filter/agent configuration and
all contributing tile/obstacle-region generations, including explored regions for negative
or partial answers. If exact dependency coverage is unavailable, a whole-navigation-world
revision is the conservative fallback. A change invalidates all affected entries; it need
not invalidate unrelated regions. Cache hits and late worker results both revalidate their
revision coverage before publication; external path users cannot bypass that check.

### Generation-Safe Navigation Identities

`CrowdAgentHandle` contains `NavigationWorldId` (a unique incarnation bound by the host to
one scene generation), slot index, and slot generation. Removing an agent increments its
generation; a generation that cannot advance retires the slot permanently. Worlds/incarnations
are never reused while references may survive. Stale, removed, or cross-world handles fail
with `NavigationErrorCode::InvalidHandle`, without modifying another agent. Request and
obstacle handles use the same lifetime rule. Provider polygon/tile references are translated
to Horo handles with topology-generation validation; a raw vendor bit pattern is not a stable
public identity. Queued operations revalidate handles on apply, not just at submission.

### World Streaming And Tile Lifetime

World Streaming is the authority for cell residency, load/eviction decisions, and global
resident-memory limits. A host-composed `IFeatureStreamingProvider` adapter decodes/stages the
ADR-023 NavigationMesh cell payload into private provider resources. Cell tile publication
or logical removal occurs with the existing `CommitDeferredLifecycleChanges` cell transaction;
`NavIntentCommit` consumes the committed topology revision and never sees partially installed
cell state. The adapter participates in rollback if any required cell provider fails.
The ADR-012 provider protocol captures both PartitionEpoch and per-cell
StreamingGeneration, reserves staging/resident/retired bytes, and explicitly
acknowledges Ready/Prepared before activation. Eviction acknowledges Retired only
after every query/tile lease and provider-affine release completes; logical removal
alone is not physical deallocation. Optional/fallback policy cannot bypass an
activation-critical navigation requirement.

Navigation tile streaming means requesting needed content and installing/releasing navigation
resources under that contract; it is not another cell loader. Missing tiles generate bounded
residency requests through the adapter. Denied/unavailable content produces a typed
`NoNavigationData` or explicitly partial path, not a direct I/O bypass. In-flight queries hold
leases; eviction first removes tiles from the active topology and invalidates affected paths,
then reclaims private bytes after readers retire. Results from logically evicted tiles cannot
be committed merely because a lease still keeps their memory alive.

The shared world budget accounts for cell-backed navigation resources and their retired but
still-pinned storage until actually freed. Navigation caches/scratch use explicit host-assigned
sub-budgets and do not independently charge the same allocation again. `wst.*` residency and
forced-eviction commands (ADR-018 / WST-010.8) enter the same world state machine. Standalone
scenes without World Streaming use a host asset-lifetime adapter with the same snapshot and
lease rules; neither case adds a concrete WorldStreaming dependency to NavigationRuntime.

[ADR-141](../../adr/141-terrain-foliage-cross-system-ownership-and-readiness.md)
applies the aggregate receipt protocol to Terrain/Foliage navigation. Terrain lends a
bounded immutable neutral grounded-surface/hole/obstacle/link snapshot tagged with exact
Terrain/content/mutation/cell/request/origin evidence. Navigation owns artifact/provider
validation, candidate topology, query-root publication and native/provider-affine
retirement. Its Ready/Prepared/Published receipt names the resulting topology revision;
ordinary queries cannot reach the activation-scoped candidate until the aggregate Scene/
cell root commits. Missing/stale receipts do not imply coverage, and a retained old tile
cannot satisfy a new generation. Retired is acknowledged only after query/tile leases and
provider resources release.

Global locations use `WorldCoordinate64`; provider-local coordinates use SceneMath conversions
with a captured origin epoch. ADR-026 rebases translate dynamic paths/obstacles consistently.
A late result from an old origin is converted or rejected before application; rebasing alone
does not invalidate static tile topology or its stable world identity.

### Complete Third-Party Encapsulation And Allocation

The no-leak rule covers all public Horo navigation headers and all consumers outside
`NavigationRecastDetour`, including transitive includes, forward declarations, aliases,
macros, inline/template bodies, factory signatures, and exported ABI types. It covers the
entire vendor include tree (`Recast*.h`, `Detour*.h`, debug helpers, and future headers), not
an allowlist of forbidden examples such as `DetourTileCache.h` or `RecastAlloc.h`.

Required enforcement when the navigation targets are introduced:

1. Register each header under exactly one target in `cmake/HoroPublicHeaderOwnership.cmake`
   and use `horo_configure_target_header_boundary`; `NavigationCoordinator.h` belongs to
   Runtime, provider factories to their providers, and neutral contracts to Api. Reject
   unowned/duplicate headers and broad repository include roots at configure time.
2. Build the existing generated isolated public-header consumers, including Api-only and
   Null/runtime-only compositions without vendor include paths. Vendor usage requirements
   must remain PRIVATE; static-library link closure may carry implementation archives but
   must not propagate vendor headers, definitions, or options to consumers.
3. Add a CI include-boundary check over compiler dependency/include traces and compile
   commands for every non-provider translation unit. Reject any resolved path under the
   pinned vendor tree, even if reached through a renamed wrapper header. Scan public AST
   declarations/inline bodies and exported symbols for vendor declarations; vendor symbol
   visibility is private in shared builds.
4. Negative fixtures deliberately inject direct and transitive vendor headers, forward
   declarations, public aliases, and leaked include directories; each must fail the relevant
   gate. Run them with real-provider, runtime-only/no-builder, Null, and navigation-omitted
   configurations. Passing an empty scan before targets exist is not enforcement evidence.

These are implementation acceptance gates; this documentation-only ADR does not claim the
navigation-specific trace/AST/export fixtures already run in CI. Existing header ownership
and isolated consumer machinery is reused rather than replaced.

All provider allocations (persistent meshes, query scratch, crowd, tile cache, and bake
scratch) route through bounded, tracked Foundation memory domains. Install the vendor
[Recast allocation hooks](https://github.com/recastnavigation/recastnavigation/blob/main/Recast/Include/RecastAlloc.h)
and [Detour allocation hooks](https://github.com/recastnavigation/recastnavigation/blob/main/Detour/Include/DetourAlloc.h)
once per linked vendor-library instance during host activation, before any vendor allocation
or concurrent work. Additional hosts share the established routing and cannot replace it.
Their routing must be thread-safe and retain the allocating domain for cross-thread frees; hooks/domain
lifetime outlasts every allocation. Never swap hooks per scene or job. Explicit tile-cache
allocators and provider-owned C++ buffers follow the same rule. A path that cannot be routed
must be adapted or rejected during provider validation, not silently exempted. Allocation
failure returns a typed bounded failure, with no partial state publication or fallback to
untracked malloc. Failure-injection/accounting tests cover shutdown and concurrent queries.

Project navigation profiles are the sole typed authority for agent, surface, resident-tile,
query, resident-memory, and per-tick work ceilings. Every limit is positive, explicit in its
unit, checked for aggregate overflow, and validated before activation. A developer-preview
preference is revision-bound non-authoritative input: resolution may reduce or clamp project
ceilings but never expand them. Provider capability evidence is checked against the exact
profile revision before activation, and editor, packaged, and headless hosts use the same
resolver. Renderer backends and graphics feature tiers do not select or alter navigation
profiles.

## Navigation Mesh

### NavMesh Generation

NavMesh is generated from scene collision geometry:

- Static mesh colliders are voxelized and used to build a navigation mesh
- NavMesh generation runs as an offline asset cook step or background tooling job
- Generated NavMesh is stored as immutable cooked `NavMeshData` under the source
  definition AssetId and its typed scope/profile partitions

Tile generation is independently bounded by vertex, polygon, link, owned-byte and work-unit
ceilings. A tile builder validates its exact `(x, z, layer)` bounds before voxelization,
sorts canonical triangles by stable provenance, applies slope and span walkability filters,
partitions regions, simplifies contours and emits convex polygons with stable adjacency and
source-provenance ranges. A tile with no surviving walkable region is a valid empty result;
malformed input, provider failure, cancellation and budget exhaustion are typed failures.

```cpp
struct NavMeshBuildSettings {
    float  agentRadius;
    float  agentHeight;
    float  maxSlope;
    float  stepHeight;
    float  cellSize;           // voxel size
    float  regionMinSize;
    bool   generateOffMeshLinks;
};
```

Multiple grounded NavMesh surfaces can exist for profiles such as humans, large
creatures and ground vehicles. Flying, swimming and space traversal require a
future volumetric provider/artifact; they are not ordinary grounded profiles.

The immutable navigation-area registry owns its identity-sorted query-filter
descriptors. Exact filter lookup returns a non-owning immutable pointer into that
storage; the pointer is valid only for the lifetime of its registry. Callers of the
initial by-value lookup contract migrate from `result.Value().field` to
`result.Value()->field` and must not retain the pointer beyond registry teardown.
The registry is non-copyable and non-assignable. Move construction transfers
storage and all borrowed-pointer lifetime responsibility to the destination.

Grounded agent profiles use stable project-owned non-zero identities; display names
and localization are presentation only. Each profile owns its bake-only radius,
height, slope, step, voxel-cell and minimum-region values. Those values are finite
and validated before bake admission, while runtime speed and Character movement
settings remain outside this contract. The profile identity partitions generated
surfaces even when profiles consume the same source geometry.

Renaming preserves identity and build values. Duplication copies build values into
a caller-issued distinct identity. Deleting a referenced profile fails until the
authoring transaction either proves it unreferenced or supplies one explicit
surviving replacement for every exact reference; no default profile repair exists.

### NavMesh Asset And Cook Contract

[ADR-105](../../adr/105-navigation-asset-and-scene-ownership-boundary.md)
separates four one-way representations: durable `NavigationDefinition` plus typed
Scene navigation intent, an ephemeral immutable `NavigationBakeInputSnapshot`, a
published `GroundedNavMeshArtifact` exposed as neutral `NavMeshData`, and a
generation-scoped runtime `NavigationTopologySnapshot`. Generated polygons, tiles,
provider sections, topology handles and carved results never become authored Scene
truth or flow back into a definition.

The `NavigationDefinition` uses the stable Asset Registry `AssetId` assigned in its
sidecar and owns grounded profiles, build/tile policy, areas and bake-scope policy.
Typed Scene components reference it and contribute explicit accepted collision
sources, modifiers, grounded off-mesh links and dynamic-obstacle intent through
stable Scene object/component/contribution IDs. Moving or renaming source preserves
those identities. Render geometry is not implicit input; every geometry contribution
names an exact validated source/collision artifact.

The typed Scene boundary exposes `NavigationSurfaceComponent`,
`NavigationRegionComponent`, `NavigationModifierComponent`, and
`NavigationLinkComponent`. A surface pins its definition `AssetId`, grounded
profile set, component generation, enabled state, and object-subtree or explicit
local-bounds bake scope. A region has a distinct stable identity, references one
exact surface, and declares finite local bounds plus include/exclude and source
selection policy. A modifier owns a stable identity, one exact surface reference,
and a finite object-local box or Y-axis cylinder whose closed operation is
exclusion, area override, or area-and-cost override. A grounded link is owned by
its containing Scene object, has ordered start/end endpoints with exact surface
references, selects intersection-compatible grounded profiles, and records an
explicit start-to-end or bidirectional policy; endpoint order is never inferred or
canonicalized away. Scene-wide validation rejects malformed payloads, duplicate
identities, missing surfaces, coincident endpoints, and profile incompatibility
before history or runtime publication. Runtime conversion uses only one committed
Scene state and carries its identity as the definition revision; disabled
components remain authored but are absent from that runtime projection, and editor
drafts, generated topology, and provider handles never cross this boundary.
`HoroRuntimeScene` therefore has one deliberate public dependency on the
backend-neutral `HoroNavigationApi`; the edge carries only typed identities and
component validation. Navigation providers and `HoroNavigationRuntime` remain
downstream and cannot enter Scene ownership or composition.

`NavigationSourceGeometrySnapshot` is the owned NAV-002.6 geometry boundary inside
that larger bake-input capture. It accepts only immutable static-collider, terrain,
procedural-generation, and approved-custom contribution views. There is deliberately
no renderer producer kind or renderer dependency: an explicit render-derived policy,
if introduced later, must remain versioned and cannot displace available canonical
collision/navigation geometry. Each contribution declares right-handed Y-up,
right-handed Z-up, or left-handed Y-up axes plus a finite positive metres-per-unit
scale. Capture converts axes and units first, repairs winding for handedness changes,
then applies the positive canonical-metre transform. Capture validates those conventions,
finite transformed vertices, indexed non-degenerate triangles, stable producer and
contribution identities, area/material semantics, and qualified contribution, vertex,
triangle, and owned-byte bounds before publishing any snapshot.

The snapshot copies all geometry, sorts contributions by stable authored identity,
and attaches producer identity, contribution identity, exact source revision/digest,
and original triangle index to every canonical triangle. Workers retain only its
immutable views. Before later bake adoption, the owner supplies the complete current
source observation set; any capture-revision, source-revision, digest, missing, or
additional contribution mismatch rejects the attempt as stale. The snapshot itself
has no clock, worker, cancellation, publication, or shutdown lifecycle.

`NavigationBakeInputSnapshot` is the NAV-003.2 composition boundary above that
geometry snapshot. It accepts stable surface/profile/source bindings and modifier
volumes in arbitrary order, resolves every profile, area and query filter against one
immutable capture, drops filter-excluded triangles, transforms modifier bounds to
canonical right-handed Y-up metres, and emits contiguous tile-builder partitions.
Profiles and referenced areas are identity-sorted; partitions are sorted by profile
and surface; source bindings add producer and contribution identity; modifiers add
their stable identity. The snapshot retains complete triangle provenance and computes
one deterministic fingerprint over the resulting canonical values.

Capture is bounded independently by profile, referenced-area, surface-binding, modifier, emitted-
triangle, work-unit and total-owned-byte ceilings. Missing references, duplicate keys,
non-finite or degenerate values, and capacity exhaustion fail the complete transaction
without truncation. Final adoption requires the exact request generation plus
definition, Scene, registry, project-profile, coordinate-policy and geometry revisions,
then revalidates the complete producer revision/digest observation set. Cancelled,
failed, superseded and shutdown operation states are explicit non-publishable results;
the snapshot does not itself schedule jobs or own the operation lifecycle defined by
ADR-106.

Bake captures one bounded revision-consistent snapshot with exact project, Scene,
definition, registry, package, geometry, profile, settings, coordinate, schema,
cooker and provider provenance. The host's Asset Pipeline adapter registers the
canonical navigation type and cooker, writes immutable artifacts to `CookCatalog`,
and loads them through `IAssetProvider` / `AssetLoadService`. `NavMeshData` itself
does not import the registry/provider implementation; the adapter associates the
definition AssetId, scope/profile/tile role and neutral payload while pinning its
lifetime. Generated scope/profile/tile partitions live inside the cook product
rooted at the definition AssetId and do not receive new authoring AssetIds or
sidecars.

Source definitions/settings follow `HoroProjectVersion` migration. Cooked bytes carry an
independent `navMeshFormatVersion` and the standard artifact envelope, including actual-byte
digest, sizes, and validated table ranges. Runtime rejects unsupported versions, malformed
indices/links, non-finite coordinates, bounds violations, or corrupt digests before installing
any tile. It never parses authoring JSON, migrates source, or recooks at runtime.

Cook uses the complete canonical Asset Pipeline key plus its explicit dependency-aware
extension: asset identity/type, source and metadata digests, settings/schema (including agent
profile and coordinate conventions), source/dependency geometry artifact digests in canonical
order, cooker identity/version, typed target/profile, and envelope/navmesh-format versions.
Project migration and provider/cooker format changes invalidate affected outputs. The current
dependency-free CacheKeyV1 cannot silently bake dependency-bearing meshes; this capability
requires the versioned dependency-aware extension also required by ADR-017.

Provider-specific optimized bytes, if emitted, carry an explicit provider-format version and
are private cooked sections decoded by that provider, never exposed as public vendor structs.
The host chooses a compatible payload/provider pairing or returns `UnsupportedCookedVersion`.
Cell packaging reuses the same cooked content through ADR-023's NavigationMesh payload; it
must not create competing AssetIds or an independent residency/cook authority.

Scene conversion resolves required definition/profile/scope references to exact
published artifact identities and digests. Runtime validates and materializes a
detached topology candidate; Scene or cell activation publishes it atomically.
Missing/stale/corrupt required data blocks activation, while explicit optional use
reports `NoNavigationData`. Runtime never opens source, bakes, migrates or selects an
inactive prior generation. Dynamic carving derives a new runtime topology generation
without mutating the published artifact; a persistent change edits authored intent
and requires recook.

### Navigation Bake Operation And Publication

[ADR-106](../../adr/106-navigation-bake-ownership-transaction-and-cache.md)
makes one application-owned `NavigationBakeService` the admission and lifetime
authority for Editor, CLI, MCP, play and release-cook requests. Presentation
adapters submit typed project/definition/scope/profile/target requests, observe the
shared `OperationStore` and request cooperative cancellation. A panel, transport
session or private builder never owns the accepted operation.

Each attempt pins the complete ADR-105 immutable input and computes a canonical
fingerprint before cache/build work. Requests coalesce by project, definition,
scope, grounded profile set and target/profile: identical fingerprints join; a
different request supersedes the active or one bounded pending successor. Only the
latest request generation may cross the final adoption barrier. Tile/profile jobs
use Foundation `JobSystem` and bounded host-owned memory/output writers; the
provider creates no scheduler, cache or publication path.

Navigation uses a versioned dependency-aware cache key covering definition/Scene/
contributor/geometry digests, project and schema revisions, grounded semantic
tables, coordinate/tile/build policy, catalog/cooker/provider fingerprints,
target/profile and payload/envelope formats. Cache hits receive the same validation
as fresh output and do not become active merely by existing.

Unique same-filesystem staging plus a process resource lease and Platform
`ExclusiveFileLock` serialize every writer to one canonical publication key across
Editor/CLI/CI processes. The operation assembles and verifies a complete immutable
generation, then atomically replaces `current.json` last. Failure, cancellation,
timeout, supersession or crash before that point preserves the prior generation.
Recovery validates the current pointer or an explicit successful receipt; it never
selects the newest directory. Accepted operations end exactly `Succeeded`,
`Failed`, `Cancelled`, `Superseded` or `TimedOut`, with the latter two projected to
the shared operation model using typed causes.

### NavMesh Data

The runtime NavMesh is a compact data structure:

```cpp
struct NavMeshData {
    std::vector<NavMeshVertex>  vertices;
    std::vector<NavMeshPoly>    polygons;
    std::vector<NavMeshLink>    offMeshLinks;     // jump, ladder, teleport
    NavMeshTileGrid             tileGrid;
    NavMeshQueryFilter          defaultFilter;
};
```

NavMesh tile requests are driven by active-agent demand, but cell residency remains
World Streaming policy as defined above; nearby agents do not authorize a second loader.

### Area And Query-Filter Registry

Authored navigation areas and reusable query filters use stable non-zero identities;
array order, display names and localized labels are never identity. Project and
package contributions enter one typed registry and are resolved in canonical identity
order. A duplicate area, filter or per-filter area override is a conflict rather than
an input-order precedence rule.

Every base and overridden traversal cost is finite and non-negative before registry
activation. An empty include mask means all areas, while matching exclusion flags
always win. Exact unknown area and filter identities return typed errors and never
silently select a default descriptor. Provider-native area flags remain private to
the selected backend and are translated from these Horo-owned policies.

## Pathfinding

### A* With NavMesh

Pathfinding uses A* on the NavMesh polygon graph:

```cpp
struct PathfindingRequest {
    NavigationWorldId             world;
    NavigationGeneration           topology;
    WorldCoordinate64              start;
    WorldCoordinate64              end;
    NavigationFilterId             filter;
    CoveragePolicy                 coveragePolicy; // RequireComplete or AllowPartial
    QueryLimits                    limits;        // bounded expansions and output points
    CancellationToken              cancelToken;
};

struct PathfindingResult {
    std::vector<WorldCoordinate64> waypoints;
    PathfindingStatus              status;      // Reachable, Partial, Unreachable, BudgetExceeded
    PathStopReason                 stopReason;  // why the frontier stopped
    WorldCoordinate64              stopPosition;
    float                          pathCost;
    float                          pathLength;
    NavigationGeneration            sourceGeneration;
};
```

- `NavigationFilterId` resolves through the immutable area/filter registry; exclusion wins and every base or override cost must be finite and non-negative before search.
- A* orders equal-cost work by canonical polygon identity and admits no more than the declared node/open-set bound.
- Path straightening (string pulling) produces a compact waypoint list
- `RequireComplete` reports `Unreachable` or `BudgetExceeded` without pretending a prefix is complete; `AllowPartial` reports a bounded prefix with the exact stop reason, stop position and source generation.
- Cancellation remains a typed terminal outcome and never publishes a path.
- Pathfinding requests are asynchronous and can be cancelled

### Hierarchical Pathfinding

For large worlds, hierarchical pathfinding is used:

- High-level graph connects NavMesh tiles
- Coarse path through tiles is found first
- Fine path within each tile is computed lazily as the agent moves
- High-level graph is regenerated when streaming loads or unloads tiles

## Dynamic Obstacles

[ADR-108](../../adr/108-dynamic-overlay-carving-and-tile-rebuild-policy.md)
separates local avoidance, logical blocker/link/modifier overlays, optional
`DynamicTileCarving`, optional `RuntimeGroundedTileRebuild`, authored recook and
World Streaming tile transactions. These mechanisms are not fallbacks with
equivalent authority: cooked `NavMeshData` is immutable, and every runtime topology
change stages and atomically publishes a new ADR-107 combined generation.

Moving navigation agents use path following plus bounded local avoidance/safe
velocity; Character/Physics owns movement. They are never carved per transform.
Large moving bodies and newly added simple blockers publish a conservative logical
overlay first. Normal doors use stable `NavigationLinkId` gate state and an optional
threshold blocker. Declared modifiers use typed overlay/filter revisions rather
than direct polygon/provider-flag writes.

When `DynamicTileCarving` and retained admitted tile layers are available, supported
finite box/cylinder blockers may produce a detached affected-tile candidate. Missing
capability/layers or unsupported shapes leave the conservative overlay visible and
return a typed disposition. Removing a blocker restores only topology represented
by the retained base layers.

Arbitrary terrain, destruction or new-static-geometry changes require ADR-106
authored recook, or a separately composed `RuntimeGroundedTileRebuild` capability
over an immutable authoritative geometry snapshot. That runtime capability is not
in the 1.0 baseline and its output remains transient; it cannot update Asset
Registry, cook cache, `current.json` or Scene source. Streamed geometry installs/
removes already cooked cell tiles only through World Streaming.

```cpp
struct DynamicObstacle {
    WorldCoordinate64  center;
    float              radius;
    float              height;
    bool               isMoving;
    ObstaclePriority   priority;
};
```

Adding/closing a blocker targets the next eligible `NavIntentCommit` and fails
closed before asynchronous carve/rebuild work. Opening new reachability waits for a
validated gate/topology generation. Overlay, carving, rebuild, recook and residency
each have separate queue/work/memory/staging/retired budgets and typed outcomes;
graphics tiers never choose them. Paths intersecting a changed dependency are
invalidated and re-requested under ADR-107 consistency policy.

## AI Perception

### Gameplay Truth vs Presentation Separation

The AI perception subsystem is an authoritative simulation component owned by
`PerceptionManager` in `SceneRuntime`. It evaluates what AI agents detect and
remember entirely within the simulation layer.

```text
+--------------------------------------------------------------------------+
|                            SIMULATION LAYER                              |
|                                                                          |
|  Gameplay Systems / Emitters               Perception Subsystem          |
|  +-------------------------+ StimulusEvent +--------------------------+  |
|  | Weapon / Footstep /     |-------------->| PerceptionManager        |  |
|  | Health / Scene Emitter  | (Typed Data)  | - Sense Registries       |  |
|  +-------------------------+               | - Time-Sliced Schedulers |  |
|                                            +--------------------------+  |
|                                                          |               |
|                                                          v               |
|                                            +--------------------------+  |
|                                            | Agent Perception Memory  |  |
|                                            | - Bounded Tracked Targets|  |
|                                            | - Linear Decay / State   |  |
|                                            +--------------------------+  |
|                                                          | (Observer)    |
+----------------------------------------------------------|---------------+
                                                           |
                                                           v
+--------------------------------------------------------------------------+
|                           PRESENTATION LAYER                             |
|                                                                          |
|  Debug & Editor Visualizers                Audio & VFX Presentation      |
|  +-------------------------+               +--------------------------+  |
|  | Vision Cones / Hearing  |<--------------| Sensory Alert Cues       |  |
|  | Sensed Stimuli Overlay  |  (Read-Only)  | Dynamic Music Triggers   |  |
|  +-------------------------+               +--------------------------+  |
+--------------------------------------------------------------------------+
```

1. **Strict Presentation Independence**:
   - Perception **NEVER** scrapes render viewports, GPU depth buffers, pixel
     occlusion culling buffers, or audio mixer voice buffers for gameplay truth.
   - Headless servers, automated continuous integration tests, and dedicated
     server processes execute full perception logic without any graphics context,
     GPU, display, or audio mixer initialized.
2. **Typed Stimulus Transmission**:
   - Gameplay systems emit strongly typed data structures rather than inspecting
     presentation state.
3. **Unidirectional Presentation Observation**:
   - Debug overlays, editor gizmos, gameplay HUDs, and adaptive audio layers may
     observe perception state for visualization or audio cues, but presentation
     never mutates or feeds back into perception truth.

### Senses, Authorities, And Query Seams

Sense implementations, typed stimulus payloads, and listener configurations are
composed through the immutable `PerceptionDescriptorRegistry` contract before a
`SceneRuntime` begins sensing work. `SenseTypeId`, `StimulusTypeId`, and
`PerceptionListenerTypeId` are the only save/wire identities; localized display
names and contribution order never determine identity. Native, script, and package
origins carry a stable `PerceptionProviderId` plus a non-zero descriptor version.
Duplicate identities conflict across all origins instead of introducing implicit
source precedence.

Registry capture resolves every listener against the identity-sorted sense and
stimulus sets and against product-supplied, backend-neutral capability bits. A
missing dependency, incompatible declared version interval, or capability rejects
a required listener. The same condition
records a typed unavailable state for an optional listener without disabling
unrelated listeners. The captured registry owns all descriptor data and exposes
only const spans, so a sensing job may borrow one frozen snapshot for its full
bounded execution window. Descriptor capture is inert: it does not install
services, select a backend, or touch ambient runtime state.

Every built-in sense has an explicit authority, timing owner, and underlying
query seam:

`PerceptionFilteredMemory` is the Foundation-only admission boundary for an
agent's perception. Gameplay supplies typed affiliation (friendly, neutral,
hostile, or stable custom identity), non-zero team identities when team rules
apply, listener/source layer membership and masks, and authoritative source
visibility. A candidate is rejected when either layer/mask intersection is empty,
when its affiliation or team is excluded, or when gameplay visibility is hidden
or team-only without a matching team. Render visibility never enters this test.
The simulation owner checks payload-free candidate facts before expensive spatial
or physics queries and rechecks completed observations against the captured policy
revision before memory ingestion. Denied and stale decisions contain no stimulus
payload. One bounded, live-source-checked snapshot is the intended publication
source for future blackboard and debug consumers. There is no production
`PerceptionSensePoll` or perception projection yet; #1329 owns batched delivery
and blackboard projection and must consume this gate instead of raw sensing
buffers or direct memory.

Policy replacement is staged and validated on the simulation owner, then committed
once at the future `PerceptionSensePoll` safe point. A changed policy returns one
new revision and rechecks bounded admission facts, removing only memory it now
denies before publication. An identical policy returns the current revision and
preserves memory. Gameplay team, affiliation, layer, and visibility changes can
recheck an exact remembered fact at a safe point without a new observation.
Jobs captured under the prior revision are stale and cannot ingest or publish
after the swap. Repeated or backward-tick commits leave the active policy unchanged.

| Sense | Timing Owner / Model | Authority & Source | Query Seam | Purpose |
|---|---|---|---|---|
| **Sight** | Periodic time-sliced (`PerceptionManager` tick) | `PerceptionManager` + `PhysicsWorld` | `PhysicsWorld::Raycast` / `Sweep` for LOS occlusion; `SceneRuntime` spatial index for candidates | Visual detection within FOV cone, peripheral angle, and sight radius |
| **Hearing** | Periodic time-sliced / stimulus queue drain | `PerceptionManager` + `AudioStimulusEmitter` | Distance attenuation + optional `PhysicsWorld` acoustic obstruction raycast | Acoustic detection of footsteps, gunshots, explosions, and environmental noise |
| **Damage** | Event-driven (immediate on hit) | `HealthSystem` / `CombatSystem` | Direct gameplay event carrying instigator entity, damage amount, and hit direction | Detection of inflicted harm, alerting agent to attacker identity/direction |
| **Touch** | Event-driven (fixed physics tick) | `PhysicsWorld` collision dispatcher | Contact manifolds and trigger overlap events | Immediate awareness of physical contact, collisions, and proximity penetration |
| **Team** | Event-driven affiliation/distress updates plus periodic awareness broadcast | `TeamPerceptionRelay` | Squad/faction registry and communication radius or radio channel | Shared squad awareness, target spotting distribution, and distress alerts |

```cpp
enum class AISense : uint8_t {
    Sight     = 1 << 0,
    Hearing   = 1 << 1,
    Damage    = 1 << 2,
    Touch     = 1 << 3,
    Team      = 1 << 4,
};

struct SightStimulus {
    EntityId        targetEntity;
    WorldCoordinate position;
    float           visualStrength;
};

struct HearingStimulus {
    EntityId        emitterEntity;
    WorldCoordinate position;
    float           loudness;
    uint32_t        soundTag;
};

struct DamageStimulus {
    EntityId        instigatorEntity;
    WorldCoordinate hitLocation;
    float           damageAmount;
    uint32_t        damageType;
};

struct TouchStimulus {
    EntityId        otherEntity;
    WorldCoordinate contactPoint;
    Vector3         contactNormal;
};

struct TeamStimulus {
    EntityId        sourceTeammate;
    EntityId        targetEntity;
    WorldCoordinate lastKnownPosition;
    uint32_t        alertLevel;
};

struct StimulusEvent {
    AISense         sense;
    EntityId        sourceEntity;
    WorldCoordinate location;
    float           intensity;
    float           expirationTime;
};
```

1. **Physics Query Seam**:
   - Line-of-sight (LOS) occlusion checks execute against `PhysicsWorld` using
     dedicated query channels (`CollisionChannel::Visibility` / `SightOcclusion`).
   - LOS queries are read-only and operate against physics spatial acceleration
     structures without mutating collision state.
2. **Scene Spatial Seam**:
   - Candidate emitter gathering queries `SceneRuntime` spatial acceleration
     structures (octree / BVH) to discover potential emitters within sensory
     range before issuing detailed LOS physics traces, eliminating $O(N^2)$ scaling.
3. **Team Dispatch Split**:
   - Membership, faction, direct distress, and explicit target-spot events wake or
     invalidate affected agents immediately.
   - Periodic relay ticks share selected, already-committed perception facts among
     eligible teammates under bounded range/fan-out budgets. They do not read
     another agent's mutable in-progress sense evaluation.

### Update Policies And Time-Sliced Budgets

Perception uses a hybrid execution model balancing immediate reactivity and
bounded compute cost:

1. **Hybrid Execution Model**:
   - **Event-Driven Inputs (`Damage`, `Touch`, team affiliation/distress)**:
     Dispatched immediately when gameplay events occur or collected during the
     fixed simulation tick. Work is $O(E)$ where $E$ is the emitted event count.
   - **Continuous Periodic Inputs (`Sight`, `Hearing`, team awareness relay)**:
     Evaluated at scheduled intervals across fixed simulation ticks and distributed evenly via
     time-slicing.
2. **Time-Sliced Raycast Budgets**:
   - The engine enforces hard limits per fixed simulation tick:
     - `maxSightRaycastsPerTick`: Maximum physics LOS raycasts allowed across all
       agents in one fixed simulation tick. It is profile-bounded (for example,
       at most 16 for `LowCpu` and 128 for `MediumCpu`).
     - `maxPerceptionExecutionTime`: CPU budget cap per tick (e.g. 1.0 ms).
     - `maxAgentsEvaluatedPerTick`: Maximum agent sight sweeps per tick.
   - Agents use weighted fair round-robin queues. Deadline aging promotes agents
     as their LOD service interval approaches; overdue agents run
     oldest-deadline-first before normal weighted slots, preventing a sustained
     near-agent workload from starving distant queues.
   - Scene activation validates the admitted population against the configured
     service intervals and budgets. Runtime over-capacity preserves hard CPU and
     raycast limits, emits `PerceptionBudgetUnsatisfied`, and applies the configured
     deterministic admission/degradation policy rather than silently starving work.
3. **Sensory Level of Detail (LOD)**:
   - Evaluation frequency scales with distance from simulation relevance anchors
     such as active player characters and gameplay objectives. Rendering camera or
     frustum state never controls authoritative LOD on network hosts or headless
     servers:
     - **LOD 0 (Near, $[0, 25\text{m})$)**: Full evaluation rate (e.g. 10 Hz / every 6 ticks at 60Hz).
     - **LOD 1 (Medium, $[25\text{m}, 60\text{m})$)**: Halved evaluation rate (e.g. 5 Hz / every 12 ticks).
     - **LOD 2 (Far/Active, $[60\text{m}, \text{dormancyDistance})$, default endpoint 150m)**: Throttled evaluation rate (e.g. 1-2 Hz / every 30-60 ticks). Agents beyond the endpoint remain LOD2 while gameplay-pinned, inside active simulation/streaming relevance, or retaining a high-priority active stimulus.
     - **LOD 3 (Dormant)**: Entered only outside active simulation relevance (for example, an inactive streaming cell), or beyond `dormancyDistance` with no gameplay pin or high-priority stimulus. Continuous senses pause except for a bounded recheck (default 120 ticks); event-driven stimuli wake the agent immediately and recompute LOD.

### Memory Model And Linear Decay

```cpp
enum class PerceptionPersistencePolicy : uint8_t {
    PreserveMemory, // Persist durable knowledge and resume simulation-time decay
    ResetOnRestore  // Start with empty perception memory after restore
};

struct AIPerceptionConfig {
    float          sightRadius;
    float          sightAngle;           // Peripheral vision cone half-angle
    float          hearingRadius;
    float          touchRadius;          // Contact/proximity range
    float          dormancyDistance{150.0F}; // LOD2-to-LOD3 distance candidate
    float          memoryDuration{10.0F};    // Max lifetime in simulation seconds
    float          decayRate{0.1F};          // Linear decay rate, inverse seconds
    float          forgetThreshold{0.0F};    // Strength threshold for eviction
    uint32_t       maxTrackedStimuli{16};    // Runtime cap, <= kMaxTrackedStimuli
    PerceptionPersistencePolicy persistencePolicy{PerceptionPersistencePolicy::PreserveMemory};
    PerceptionMask senseMask;            // Active sense bitmask
};

struct PerceivedStimulus {
    AISense         sense;
    EntityId        sourceEntity;
    WorldCoordinate lastKnownPosition;
    Vector3         lastKnownVelocity;
    float           strength;            // Normalized sensed strength [0.0, 1.0]
    float           age;                 // Seconds since last observed
    bool            isCurrentlySensed;   // Active line-of-sight or contact
};

struct AIPerceptionMemory {
    static constexpr uint32_t kMaxTrackedStimuli = 32; // compile-time hard cap
    std::array<PerceivedStimulus, kMaxTrackedStimuli> entries{};
    uint32_t count = 0;                 // live count, <= config.maxTrackedStimuli
    uint32_t maxTrackedStimuli = 16;    // runtime cap, default 16, max 32
};
```

1. **Bounded Stimulus Storage**:
   - Each agent maintains an `AIPerceptionMemory` container with a fixed capacity
     (`maxTrackedStimuli` default 16, compile-time hard cap 32), preventing dynamic heap allocation
     during perception ticks.
2. **Linear Decay And Forgetting**:
   - Tracked stimuli record `age` (seconds since last sensed) and normalized
     `strength` ($[0.0, 1.0]$).
   - While actively observed, `strength` is refreshed to $1.0$ and `age` resets
     to $0.0$.
   - `decayRate` is measured in inverse simulation seconds ($s^{-1}$), with a
     default of $0.1\,s^{-1}$.
   - When sight/sound is lost, `strength` decays linearly and is clamped:
     $$\text{strength}(t) = \max(0, 1.0 - \text{decayRate} \cdot \text{ageSeconds})$$
   - When $\text{strength} \le \text{forgetThreshold}$ (default $0.0$) or
     $\text{ageSeconds} \ge \text{memoryDuration}$ (default 10 simulation
     seconds), the stimulus is purged.
3. **Last Known Position & Velocity Tracking**:
   - Perception records store `lastKnownPosition` and `lastKnownVelocity`.
   - Behavior trees and blackboards query last known location rather than live
     target transforms, preventing AI agents from cheating through walls.
4. **Lifecycle And Weak Entity Safety**:
   - Perception memory stores generation-checked `EntityId` handles.
   - When an entity is destroyed or pooled, subsequent perception queries detect
     stale handles and discard the record immediately, preventing use-after-free
     and stale target locking.

The implemented per-agent `AIPerceptionMemory` contract in `HoroEngine::AI` stores
at most the host-profile cap in fixed storage, with an additional cap for each
listener identity (both at most 32). `Observe`, `MarkLost`, and `AdvanceTo`
receive the committed simulation tick and exact fixed quantum from the fixed-step
owner; no wall-clock or variable frame delta enters aging. Repeating the same tick
during pause leaves age and confidence unchanged. Replay rewind explicitly resets
memory to the restored tick before deterministic re-ingestion; backward ticks
without reset are rejected. A record retains its
first sensed time until forgetting, refreshes its last sensed time and last-known
facts on reacquisition, and expires at the configured age even if a sense omitted
an explicit loss notification. A lost record decays from its last sensed time and
is removed at the configured confidence threshold. Memory queries require the
`HoroAISceneIntegration` live-source adapter, which reacquires the current scene
view and rejects destroyed or recycled generations; destruction may also eagerly
call `ForgetSource`. This memory component does not publish to a blackboard or
choose sense scheduling policy; those remain separate phase owners.

## Environment Query System (EQS)

### EQS Ownership And Provider Execution

Gameplay AI is the sole subsystem authority for tactical orchestration and ranking.
This is not a requirement that one manager class implement the entire subsystem:

| Responsibility | Logical implementation owner |
|---|---|
| Admission, handles, cancellation, scheduling, terminal publication | EnvironmentQueryManager |
| Bounded generator/test stage execution | QueryExecutor |
| Normalization, aggregation and stable ranking | QueryScoring |
| Bounded immutable result entries and eviction | QueryCache |

These are internal roles, not new independent services, schedulers or thread pools.
Navigation, Physics and Perception retain authority over their data **and query
execution**. EQS issues bounded typed read-only requests; it cannot take a NavMesh
snapshot and implement its own pathfinder, step physics or mutate sensory memory.

| Provider | Owned operations | EQS boundary |
|---|---|---|
| NavigationCoordinator / NavigationApi | Projection, containment, reachability and path cost | Typed admission/result handles under ADR-016; no vendor types or independent EQS path jobs |
| Physics owner | Raycast, sweep, overlap and geometric visibility | Owner-thread queries outside Step, or leased read-only query snapshots |
| Perception domain | Agent-specific sensed state and memory | Immutable authorized snapshot/query; not equivalent to geometric line of sight |
| Behavior trees / decision graphs | Tactical intent consumption | Generation-checked query handles and immutable results; never blocking waits |

### EQS Asset, Context And Extension Contracts

EnvironmentQueryTemplate is an authoring asset with stable AssetId metadata in
AssetRegistry. Its immutable cooked EnvironmentQueryPlan contains stable StageIds,
ordered stages, typed context/parameter schemas, provider dependencies, scoring and
execution policy. Names and editor ordering do not determine identity. Plan identity
includes the AssetId and exact cooked artifact digest; hot reload cannot silently
change an in-flight plan.

Cooking uses the Asset Pipeline's canonical versioned key, preserving source and
semantic-metadata digests, effective settings, schema/cooker versions, target profile
and artifact envelope. Plans with referenced assets or custom provider/schema
artifacts require the dependency-aware extension (all transitive identities/digests),
not an EQS-only replacement tuple. CookCatalog publishes the validated artifact by
AssetId. Authoring ProjectVersion migration and cooked-format version are distinct:
migrate authoring before cook; runtime rejects unsupported format/schema/dependencies
before activation and retains the last valid loaded plan. Unknown newer schemas,
cycles and over-budget plans are typed failures. These extend the ADR-017 asset
pattern and do not claim cooker/runtime implementation is already present.

The following structures are schematic typed contracts, not existing public headers:

```cpp
enum class EnvQueryItemType : uint8_t { Point, Actor, DirectionalRay, Custom };

struct QueryContextSnapshot {
    EntityRef querier;                  // non-owning, scene + entity generation
    WorldCoordinate64 querierPosition; // canonical global position (ADR-026)
    Vec3 querierForward;
    std::optional<EntityRef> target;
    std::optional<WorldCoordinate64> targetPosition;
    SimulationTick submittedTick;
    ContextPayloadRef customContext;   // immutable bounded owned/retained bytes
};

struct QueryCustomPayload {
    CustomItemTypeId type;
    uint32_t schemaVersion;
    uint32_t size;                      // validated <= bytes.size()
    std::array<std::byte, 64> bytes;
};

struct ScoredItem {
    EnvQueryItemType itemType;
    WorldCoordinate64 position;         // Point, or captured Actor/ray origin
    std::optional<EntityRef> entity;     // Actor only; never an owning reference
    Vec3 direction;                     // DirectionalRay only, finite normalized vector
    QueryCustomPayload custom;          // Custom only
    ScoreKey totalScore;                // canonical rank key from [0,1]
    uint32_t itemIndex;                  // stable generator-assigned identity
};
```

Built-in contexts are Querier, Target, their captured locations and the canonical
WorldOrigin, never an implicit rebased float origin. Custom context/parameter bytes
include all declared inputs such as team, faction, stance, gameplay tags, masks,
seed and tunable query parameters. Required contexts resolve at successful
submission on the simulation owner against the current declared snapshots. A
provider unable to capture immediately returns AdmissionDeferred/Rejected without
an accepted query; it never blocks or changes the meaning of submission time.

An accepted query always evaluates its submission context and captured provider
read set. A result carries submitted/completed ticks, plan digest, input digest and
provider revisions so consumers can enforce maximum age. Consumers needing fresher
positions cancel/reissue; in-flight context is never silently refreshed. Provider
snapshots may have different declared source ticks, but one immutable revision set
is fixed for the query. A provider must execute against that retained version or
validate that its state still matches; otherwise the query ends with StaleInputs.
Memory leases preserve bytes, not permission to treat logically invalid data as
current. Cancellation, scene changes and provider invalidation remain observable.

Custom item/context/stage providers use inert domain descriptors composed by the
host, following ADR-018's registration discipline. They declare stable namespaced
IDs, schema versions, bounded sizes/alignment, capabilities, owner thread/safe point,
provider dependencies, serialization and canonical equality/hash functions. Creation
has no registration or service-discovery side effects. Composition rejects duplicate
IDs, incompatible versions, missing dependencies, cycles and unsupported modes.

Custom bytes are canonical serialized values, not arbitrary native object memory:
no raw pointers, borrowed views, owning C++ objects or callbacks. They remain immutable
while retained and use bounded Foundation-owned storage. The inline item cap is 64
bytes; larger items require a separately reviewed schema, not an unbounded allocation.
Context payloads have a profile cap. Canonical encoding excludes padding, normalizes
endianness and floating zero, and rejects non-finite inputs. Cacheable custom values
must provide complete stable hash/equality; otherwise the whole query bypasses cache.

### EQS Generators And Tests

GridGenerator, DonutRingGenerator and ConeGenerator produce bounded candidate sets.
NavMeshProjectionGenerator submits Navigation-owned projection requests; it does
not execute pathfinding internally. PerceivedEntitiesGenerator reads an authorized
perception snapshot and enumerates stable entity identities, not hash-map order.
All generators validate dimensions, spacing and arithmetic overflow at cook/admission
and enforce maxItems. Item indices are assigned in stable generation order **before**
provider dispatch; async completion order cannot change candidate identity.

| Test | Meaning / execution owner |
|---|---|
| GeometryLineOfSightTest | Physics ray/sweep obstruction for declared geometry/layer policy |
| PerceivedVisibilityTest | Agent-specific sensed/remembered visibility from Perception |
| DistanceTest | EQS math for Euclidean/Manhattan/Chebyshev; Navigation for path distance |
| DotProductTest | EQS directional alignment from captured finite inputs |
| PathfindingCostTest | Navigation-owned bounded path/cost request |
| CoverExposureTest | Composes declared bounded Physics queries and captured stance/threat context |

The old LineOfSightTest name migrates to GeometryLineOfSightTest explicitly; it
cannot silently switch between physical obstruction and an agent's perception.
Tests declare FilterOnly, ScoreOnly or FilterAndScore. The compiled plan records the
union of **all** generator, context, filter, scoring and tie-breaker dependencies,
including custom providers. Query execution cannot perform undeclared reads.

### EQS Scoring And Ranking

Scoring maps finite metrics to [0,1] with explicit fixed ranges and curves (linear,
inverse linear, bounded sigmoid or threshold step). Ranges are plan/context inputs,
not min/max estimates that change with whichever candidates happened to finish.
Any future population-dependent normalization needs a full-stage barrier and cannot
publish partial rankings before its domain is complete.

For scored plans, TotalScore = sum(weight * normalizedScore) / sum(weight).
Cook and admission require finite nonnegative weights, valid normalization ranges
and a finite strictly positive total weight. They reject zero denominators and
statically reproducible invalid math. An explicitly filter-only plan does not use
this formula; surviving candidates use the configured stable selection ordering.

Runtime non-finite intermediate values or aggregation overflow mark a candidate
InvalidScore and exclude it from eligibility, even if every other score is zero.
Diagnostics are typed, bounded and deduplicated; assigning a display score of zero
never makes an invalid candidate a winner. Validation/dev tests must report the
reproducible source stage rather than silently hide authoring errors.

ScoreKey is a plan-versioned fixed-point rank value obtained with declared precision,
rounding and overflow rules after normalization. Ranking is highest total ScoreKey,
then configured tie-breaker key/direction, then stable itemIndex. Aggregation follows
compiled StageId order; no pointer/hash iteration or epsilon-based non-transitive
sort comparisons are allowed. Deterministic ranking assumes identical complete
provider inputs and the declared platform/numeric contract; it is not a blanket
cross-platform physics or floating-point bit-identity guarantee.

### EQS Owner Thread, Safe Points And Provider Budgets

EnvironmentQueryManager runs on the scene's simulation owner; it has no dedicated
thread. It uses the existing ADR-021/022 phase order, not an extra AI phase:

1. BlackboardSync drains eligible immutable completions, validates generation and
   captured revisions, and publishes terminal records before blackboards freeze.
   A behavior observer may copy a result through the normal blackboard write seam.
2. At the start of AiDecisionEvaluate, QueryExecutor advances previously admitted
   queries under the EQS budget. Decision nodes then submit new requests or consume
   published results. A newly accepted query first executes on a later tick.
3. Provider requests execute/publish at their declared owner seams. Navigation uses
   NavIntentCommit per ADR-016; Physics immediate kernels run on its owner outside
   Step, or its adapter dispatches leased-snapshot work. Results feed the next
   eligible BlackboardSync, never a reentrant decision callback.

Both cached hits and cheap/null-provider failures use scheduled publication; neither
completes inline at Submit. Null navigation reports NoNavigationData, never a fake
successful path. Queries do not run inside arbitrary console WorkerJob callbacks.
ADR-018 OwnerThreadNextFrame commands enqueue bounded admission for the simulation
owner; they do not mutate manager state from transport/render/editor threads.

EQS budgets stage advancement, candidate batches and provider **admissions**.
Providers retain their own bounded kernel/scratch/queue budgets. Accepted work is
charged once to each relevant budget; an EQS slice cannot reset a provider's quota,
create a nested thread pool or reserve unbounded child requests. Backpressure leaves
the stage AwaitingAdmission until a later bounded attempt or hard deadline.
Already accepted work is not resubmitted on every tick.

All background work uses Foundation JobSystem with JobId correlation. Workers read
retained immutable inputs and write bounded result records, never live blackboards
or manager state. Owner callbacks never Wait/Join, and workers never block waiting
for nested provider jobs. Only exposed diagnostic/tool operations need OperationId
in the application-owned OperationStore under ADR-010/018; ordinary per-agent queries
use their own generation-checked request handles, not competing operation stores.

### EQS Execution Modes And Terminal Outcomes

| Mode | Scheduling contract | Guarantee |
|---|---|---|
| DeterministicWorkUnits | Stable query/stage/item order; explicit operation quotas and tick deadlines; provider-owned bounded deterministic kernels at declared ticks | Repeatable semantic result and publication tick for identical submissions, revision histories and declared numeric/provider contract |
| AdaptiveRealtime | Same hard memory/work caps plus an optional host wall-time stop at item/stage boundaries; async provider jobs | Bounded work and stable ordering of available data; completion timing and partial candidate population are not replay-deterministic |

Deterministic mode follows ADR-016's owning-executor kernel path, not whichever
worker finishes first. Each plan/provider declares the bounded units and publication
latency of its operations. Host composition validates compatible phase placement and
reserves quotas before admitting deterministic work. A provider without this mode,
pinned inputs or a bounded kernel returns UnsupportedMode/AdmissionRejected; EQS
never substitutes timing-dependent partial results or waits for a worker. Results
for a deterministic batch become visible at its declared tick in stable order.
Query result caching is disabled in deterministic mode so hit/miss/eviction cannot
change work allocation or terminal timing; cooked-asset caching is unaffected.

Adaptive mode may use a wall-time target such as the MediumCpu profile's 1500 us,
but it is not a hard preemption guarantee. A running unit completes within its own
validated bound before yielding. Replay of adaptive decisions requires recording
submissions, provider outcomes and chosen publication/terminal ticks; a random seed
and tie-breaker alone cannot reproduce the scheduling history.

```text
Accepted -> Running -> Yielded / AwaitingAdmission / AwaitingProvider -> Running
                                  (all nonterminal)
Running -> Completed / PartialSuccess / TimedOut / Failed / StaleInputs
Any nonterminal -> Cancelled / Aborted
Terminal record published once at BlackboardSync
```

- **Completed**: All planned stages finished, with ranked eligible items. An empty
  result is Completed with no winner, not a timeout.
- **Yielded**: Per-tick budget ended; retain work and resume. It is never a
  QueryResultStatus and never produces PartialSuccess by itself.
- **PartialSuccess**: Only if caller opted into partial results and a hard query
  deadline or explicit FinishEarly request ends execution with at least one fully
  generated, filtered and scored candidate. Unfinished-stage candidates are ineligible.
- **TimedOut**: Hard deadline reached without an allowed partial result, including
  when valid candidates exist but the caller requested complete results only.
- **Failed**: InvalidScore for all otherwise eligible scoring candidates or a terminal
  provider/schema error; preserve its typed cause, not a fabricated zero-score winner.
- **StaleInputs**: A required captured provider version can no longer be honored.
- **Cancelled / Aborted**: Caller cancellation / required entity or scene invalidation.
  Neither returns a tactical winner; terminalReason distinguishes explicit early
  finish without eligible items (NoEligiblePartial, Failed) from hard timeout.

The plan declares hard deadline in simulation ticks from accepted submission.
Pause freezes this tick-age; cancellation/shutdown remains serviced by host lifecycle
work. During pause/teardown, that owner service may finalize cancellation records
without invoking behavior callbacks; ordinary tactical publication still uses
BlackboardSync. At a boundary, lifecycle invalidation and cancellation take precedence, then
eligible completions (including on the deadline tick), then deadline resolution.
Only fully evaluated candidates participate in partial ranking. A partial result
includes coverage counts and all snapshot provenance; it is not a complete result
and is never inserted into the result cache.

### EQS Cache Identity, Quantization And Result Lifetime

Adaptive-mode cache keys contain:

- Template AssetId, exact plan artifact digest, schema/extension/numeric versions.
- Canonical **complete** context and dynamic-input bytes (not only positions), seed,
  filters, mode, result/early-exit policy and relevant effective configuration.
- The sorted dependency-revision set for every read: provider instance/incarnation,
  dependency ID, global or spatial scope, and revision token. Include Perception,
  tags/team/faction/custom context providers whenever consumed, plus scene incarnation
  and lifecycle revisions for entity-bearing inputs/results.

Hash equality alone is insufficient: compare canonical key data to avoid collision
aliasing. TTL is an additional eviction policy, never a substitute for revision and
entity validation. Scoped tile/region/broadphase tokens may reduce unrelated misses;
a provider unable to identify the complete read footprint uses a conservative global
token or marks the query uncacheable. Unknown dependencies cannot be ignored.
If execution discovers additional regions, validate the stored complete read footprint
on lookup; the initial hash is only an index until full dependency equality is proven.

ContextQuantizationPolicy is a versioned template policy: Exact by default, or an
explicit positive spatial grid with declared error tolerance and rounding. Canonical
millimeter WorldCoordinate64 storage is not a mandatory EQS quantization cell. A
coarser cell is opt-in approximate semantics, not a free exact-result optimization:
evaluate on that same canonicalized context, return its provenance, and require the
caller to accept the tolerance. Keep entity identities, directions, flags and other
inputs exact unless their schema declares an equally explicit policy. Quantizing only
the cache hash while evaluating unquantized inputs is forbidden. Before acting, the
consumer revalidates live geometry/target eligibility through the owning domain.

Cache stores only successful complete immutable data snapshots. Lookup rechecks
provider revisions, scene incarnation, request authority and every referenced EntityRef
generation/identity; a dead entity or stale dependency makes it a miss, never a
silently filtered/re-ranked hit. QueryResult uses owned/retained bounded storage, so
eviction cannot free a caller's data. This does not keep Actor entities alive. Results
carry as-of ticks/revisions and consumers validate entity/scene generation and their
freshness tolerance again before later use. A retained result is a historical snapshot,
not permission to dereference a stale handle or apply a now-invalid tactical action.

### EQS Cancellation And Resource Reclamation

A query handle includes scene/manager incarnation, slot and generation. EntityRef is
a non-owning generation-validated identity, not a weak_ptr. Required querier/target
or scene invalidation stops admission immediately and marks the query Aborted on the
owner; every result publication also checks lifecycle validity before delivery.

Cancellation is cooperative: request provider cancellation where supported; already
running work may finish. Completion records carry query generation and provider
request identity, so late/cancelled/aborted results are discarded without invoking a
dead behavior owner or mutating replacement entities. Terminal publication occurs
once; cancellation after publication does not retroactively undo an observed result.

Retain provider snapshot leases, custom payloads and result slots until workers and
provider operations release them. Retired work still counts against global in-flight
limits; releasing a query handle cannot bypass capacity by starting more uncancellable
jobs. Reclamation occurs on the owning service, outside worker access. Shutdown closes
admission, cancels requests, suppresses callbacks and retires resources under ADR-010's
allowed teardown rules, without ordinary tick/render/transport waits.

### EQS Compute Profiles And Qualification

EqsExecutionProfile is a host-selected CPU/memory policy independent of renderer
backend. These baseline caps are explicit defaults, not measured platform promises:

| Field | LowCpu | MediumCpu | HighCpu / DedicatedServer |
|---|---:|---:|---:|
| Active queries (including cancelling/retired work) | 16 | 64 | 256 |
| Candidates per query | 128 | 512 | 2048 |
| EQS item-stage work units per tick | 128 | 1024 | 4096 |
| Provider admissions per tick | 16 | 64 | 256 |
| Total in-flight provider requests | 32 | 256 | 1024 |
| Maximum stages per plan | 16 | 32 | 64 |
| Custom context bytes per query | 1024 | 4096 | 16384 |
| Total query/cache/retained bytes | 4194304 | 33554432 | 134217728 |
| Adaptive wall-time target (us; unused in deterministic mode) | 500 | 1500 | 3000 |

Every work unit has a validated bound; provider node-expansion/trace quotas and
hard query deadlines are part of the plan/provider contract, not inferred from the
wall-time target. Host composition intersects EQS admissions with provider and
GameplayAiProfile limits; selecting a larger EQS profile grants no extra Navigation,
Physics or Perception capacity. Round-robin slices in stable query-ID order prevent
one query monopolizing all work. Work-unit and byte exhaustion yields or rejects
admission as specified above, never grows containers or blocks. Record counters for
units, pending requests, cache hits/misses, stale results, partial coverage, invalid
scores and budget exhaustion; wall duration is telemetry in deterministic mode.

Required implementation tests (not implemented by this documentation change):

- Deterministic queries reproduce results/publication ticks with identical inputs
  under varied render rates and worker completion order; unsupported providers fail
  mode selection. Adaptive partial outcomes explicitly carry weaker guarantees.
- Per-tick yield resumes; complete/empty, opt-in partial, timeout with/without valid
  candidates, early finish and same-tick cancel/deadline precedence are distinct.
- Navigation/Physics retain query execution and owner affinity; no nested waits,
  duplicate admissions or per-query pools; backpressure respects both budgets.
- Invalidate cached results on Perception/team/tag/parameter/schema/region changes,
  collision-safe canonical equality, and entity destruction after completion. Test
  exact and approximate quantization at cell edges and after origin rebase.
- Reject zero total weights, invalid ranges, NaN/Inf, generator overflow, duplicate
  extension IDs, pointer-bearing payload schemas, oversized/custom invalid bytes,
  undeclared provider reads, newer cooked versions and broken dependency digests.
- Same-scene respawn, scene replacement, cancel-before/after-publication and providers
  that cannot stop in-flight work never deliver to dead owners or free leased data.
- Capacity includes retired work and retained results; stress cache eviction, owner
  teardown and cancellation storms without orphaned requests or unbounded growth.

## AI Architecture And Runtime Ownership

### Subsystem Boundaries And Decoupling From Editor AI

Horo Engine strictly isolates runtime gameplay AI (`HoroAI` / `HoroEngine::AI`) from Editor LLM / Agent tooling (`AIA-001` / `HoroEditor` AI):

- **Gameplay AI Runtime (`HoroAI`)**: A high-performance, deterministic C++20 runtime linked into game clients and dedicated servers. It executes local decision graphs (Behavior Trees, Hierarchical State Machines, Utility AI, HTN/GOAP planners), spatial perception queries, pathfinding, and crowd simulation under strict per-frame CPU and memory budgets with zero unbudgeted heap allocations.
- **Editor AI Agent Tooling (`AIA-001`)**: An authoring and development assistant embedded in `HoroEditor` using external or local large language models (LLMs) and the Model Context Protocol (MCP). It operates asynchronously on human authoring requests (code generation, scene inspection, asset imports) and is never compiled into runtime game packages or dedicated servers.
- **Optional NPC LLM Dialogue Seam (`GAI-007.4`)**: Generative AI dialogue or narrative agents exist strictly as bounded, asynchronous gameplay service providers (`INpcDialogueService`). They communicate through network/async jobs with explicit fallback behaviors and never block the fixed simulation tick or hold raw pointers to ECS memory.

### AI Runtime Ownership And Scene Generation Safety

`HoroAI` / `HoroEngine::AI` owns the canonical gameplay-AI identity contract.
`AgentId`, `ControllerTypeId`, `TaskId`, `BlackboardSchemaId`, and
`BlackboardKeyId` are distinct, persistent non-zero identities with one fixed-width
network-byte-order encoding. Display names, graph order, and array indices never
define them. Rename and reorder preserve identity; authoring duplication must issue
a different identity. Descriptor-set validation is bounded and rejects invalid or
duplicate values within each strong identity domain before activation, without
registering state or consulting an ambient service.

`AgentHandle` and `TaskHandle` are process-local runtime identities containing the
exact `SceneRuntime` incarnation plus slot and non-zero generation. They are never
serialized. A scene replacement, slot generation mismatch, malformed handle, or
generation exhaustion rejects access; exhausted slots retire instead of wrapping.
Stable authored identities and runtime handles therefore remain separate sources of
truth.

`BlackboardSchema` is the immutable schema-1 admission boundary. It owns at most
128 descriptors sorted by `BlackboardKeyId`; each key declares a scalar kind or a
flat inline collection capped at 16 elements, optionality, read/write access and an
optional type-checked default. Schema capture copies borrowed inputs transactionally,
using one bounded load-time allocation so the returned move-only value stays small
and never aliases authoring storage. Runtime instances may size
their value array once from this schema, but storage and synchronization remain owned
by the later blackboard runtime work.

`BlackboardInstance` is the per-agent, Scene-owned materialization of one admitted
schema publication. Its binding fences the exact `AiRuntimeIncarnation`, agent slot
generation, schema identity/version/publication generation, and instance generation.
It materializes schema-sized active and scratch value stores at activation; detached
write batches and commit change facts use fixed-capacity storage, so ordinary
`BlackboardSync` commits do not grow containers or allocate. A batch captures the binding and revision,
is validated completely, and is applied only by the owning `BlackboardSync` safe point.
Repeated staging of one key coalesces in place with deterministic last-write-wins
semantics and counts once against the unique-key limit. Unknown, read-only,
type-invalid, or stale writes reject the whole batch.
Changed-key facts are emitted in stable `BlackboardKeyId` order and the monotonic
revision advances only when stored values change.

Each instance owns a fixed-capacity observer registry scoped to its exact agent and
instance generation. Registrations bind one stable key and one generation-checked
task handle to a borrowed, non-throwing owner-thread callback; callback context must
remain alive until removal, task cancellation, instance replacement, or teardown.
One immutable key-sorted notification batch is assembled per changed revision, and
matching callbacks execute in deterministic registry-slot order without a generic
event-bus scan. Notification storage and spans are callback-scoped and cannot be
retained. Publication first freezes the matching callback/context pairs so it never
holds a mutable registry or blackboard iterator across user code. Every blackboard
or observer-registry mutation entry point rejects reentrant callback mutation with a
typed error; callers stage follow-up work for the next `BlackboardSync`. Removal and
task cancellation guarantee no later callbacks, while replacement and teardown
invalidate all registrations before releasing their borrowed contexts.

`BlackboardSnapshot` owns an immutable value copy rather than a pointer to mutable
instance storage, so workers cannot mutate or retain the Scene-owned store. Ordinary
commits do not change an existing snapshot. Schema/instance replacement allocates and
validates its complete default/migration candidate before publication, then expires
the old generation lease; failed replacement leaves the old binding, values, revision,
and snapshots active. Teardown similarly expires retained snapshots and rejects stale
batches. A replaced agent or `SceneRuntime` incarnation is torn down and re-created,
not rebound through schema replacement.

Entity and asset values cross HoroAI's Foundation-only public boundary as explicitly
named blackboard stored projections: the exact scene-incarnation/slot/generation tuple
or canonical 128-bit asset bytes. They are not alternate canonical identities.
RuntimeScene and Assets adapters convert these projections and revalidate residency,
generation, asset type and availability before use. Exact world positions reuse
Foundation-owned `WorldCoordinate64` directly. Unavailable key/type payloads are
either rejected or retained byte-for-byte in a fixed 128-byte opaque envelope according
to the schema's explicit version policy; recursive or dynamically owned collections
are not part of this contract.

All gameplay AI runtime state is owned strictly by the active `SceneRuntime` generation:

```cpp
struct AiBrainState {
    DecisionTreeInstanceId  instanceId;
    DecisionGraphAssetId    assetId;
    ExecutionNodeIndex      currentNode;
    ExecutionStatus         status;
    TickTimestamp           lastEvaluationTick;
};

struct PerceptionMemory {
    std::vector<PerceivedStimulus> stimuli;  // bounded capacity reserved at scene activation
    std::optional<EntityId>        primaryTarget;
    WorldCoordinate64              lastKnownTargetLocation;
    TickTimestamp                  lastSeenTimestamp;
};

struct BlackboardState {
    BlackboardSchemaId             schemaId;
    uint32_t                       schemaVersion;
    std::vector<BlackboardValue>   values;   // schema-sized once at scene activation
    BlackboardRevision             revision;
};
```

`PerceptionMemory` is the scene-owned agent record. Its stimulus buffer is the
bounded `AIPerceptionMemory` store from ADR-024 (`maxTrackedStimuli` default 16,
compile-time hard cap 32). `PerceptionMemory::stimuli` reserves that cap during
scene activation and never grows during simulation; overflow follows the configured
oldest/lowest-significance eviction policy. `BlackboardState::values` is sized
exactly once from its immutable schema. Neither container allocates on fixed-tick
paths. `AIBlackboardView` is the typed access view over `BlackboardState`; example
layouts named `AIBlackboard` describe keys, not a second store.

1. **Generation Validation**: AI components (`AiAgentComponent`, `AiControllerComponent`), blackboard instances, perception memories, and task execution contexts reference entities using generation-checked `EntityId` (or `EntityRef { SceneRuntimeId, EntityId }`). Any stale handle referencing an entity destroyed in an earlier frame or previous scene generation is rejected.
2. **Scene Lifecycle Binding**:
   - Scene activation instantiates AI brains, blackboards, and perception memories from immutable schema assets.
   - Scene transition, replacement, or unload immediately cancels all pending AI background jobs and destroys all AI runtime state.
   - Play-In-Editor (PIE) executes AI logic exclusively within the isolated runtime scene clone; AI state is never written back to the authoring scene document.
3. **No Process-Global AI Singletons**: AI systems do not maintain ambient or process-global agent registries. All state queries are scoped to `SceneRuntimeAccess`.

## Fixed-Tick Simulation Scheduling And Safe Points

Gameplay AI execution is partitioned into discrete, deterministic **coarse groups** within the fixed simulation tick (ADR-021). They are not a second scheduler. ADR-022 names the fine-grained mutation phases that implement them; character locomotion commit, animation, and render extraction belong to that broader tick in [Simulation Lifecycle And Fixed-Tick Phase Ordering](#simulation-lifecycle-and-fixed-tick-phase-ordering).

```text
ADR-021 coarse group               ADR-022 fine phase
1. SystemPhase::Perception      -> PerceptionSensePoll
2. SystemPhase::BlackboardSync  -> BlackboardSync
3. SystemPhase::AiDecision      -> AiDecisionEvaluate
4. SystemPhase::AiIntentDispatch-> NavIntentCommit
                                   + typed combat/animation intent enqueue
5. SystemPhase::Gameplay        -> CharacterControllerLocomotion
                                   + IBehaviorInstance::OnFixedUpdate
                               then AnimationRigUpdate
                               then variable-rate RenderExtraction (not a fixed phase)
```

### Phase Contracts

1. **`SystemPhase::Perception` (Perception Update)**:
   - Performs spatial candidate broadphase queries and line-of-sight raycasts against physics and scene geometry.
   - Ingests sensory stimuli (sight, hearing, proximity, damage, team communication) and applies decay in `PerceptionMemory`.
   - Read-only with respect to scene ECS transforms; writes only to agent-private perception memory.
2. **`SystemPhase::BlackboardSync` (Blackboard Mutation Safe Point)**:
   - Commits batched external blackboard writes and perception stimulus reflections into blackboard keys.
   - Enforces schema validation against `BlackboardSchema`.
   - Fires registered blackboard change observers at a deterministic safe point.
   - Freezes blackboard state for the upcoming decision phase.
3. **`SystemPhase::AiDecision` (Decision Evaluation)**:
   - Evaluates high-level decision graphs (Behavior Trees, State Machines, Utility AI, Planners) using frozen blackboard and perception state.
   - Emits high-level action and navigation intents (e.g. move-to destination, attack intent, cover request).
   - Does not step physics or mutate scene topology directly.
4. **`SystemPhase::AiIntentDispatch` (Action/Navigation Intent Dispatch)**:
   - Translates decision outputs into typed intents. Navigation and steering commit in `NavIntentCommit`.
   - Combat actions and animation triggers enqueue as typed intents consumed later; they are not absorbed into `NavIntentCommit`.
5. **`SystemPhase::Gameplay` (Behavior Script Step & Locomotion)**:
   - Generic object-attached gameplay behaviors (`IBehaviorInstance::OnFixedUpdate`) and character controllers step **after** AI decision evaluation.
   - This group is not the AI decision phase.

### Scene Mutation Safe Points

AI decision nodes, behavior trees, and background tasks are strictly forbidden from mutating Scene ECS topology (creating/destroying entities, adding/removing components) directly during decision evaluation or worker job execution. All structural changes must be recorded into the `SceneCommandBuffer` and committed at the standard Scene Runtime synchronization point (`CommitDeferredLifecycleChanges`).

## Behavior Integration And AI Decision Graphs

AI decision making is authored as dedicated graph assets that compile into flat, immutable runtime execution plans and execute within the standard scene behavior lifecycle without introducing a secondary task manager or visual scripting engine.

### Graph Asset Model And UI Independence

[ADR-111](../../adr/111-gameplay-ai-document-panel-and-runtime-debug-ownership.md)
maps blackboard schemas, the three decision graph kinds and EQS templates to
explicit asset-document routes. Decision/EQS graph tabs reuse the shared Horo
`GraphViewSnapshot`/`GraphEditCommand` surface, while subsystem document
controllers own semantic validation, history, save/conflict and compilation.
Cooked plans and live Scene-owned instances never become editor document state.

1. **Pure Semantic Graph Assets**:
   - Persisted graph identity is established by typed assets: `BehaviorTreeAsset` (`.horo_bt`), `StateMachineAsset` (`.horo_sm`), and `UtilityAiAsset` (`.horo_utility`).
   - Assets contain only semantic topology: stable `GraphId`, `NodeId`, `PinId`, `PropertyId`, schema versions, node type identifiers, property bindings, and blackboard key bindings.
   - Visual presentation data (node layout $(x, y)$, routing bends, comments, zoom/pan) is stored exclusively in the asset's canonical `.meta` sidecar (for example, `Guard.horo_bt.meta`) or stripped during asset cooking. `.editor_meta` is not supported.
   - Runtime modules and cooked assets have **zero dependency** on `imgui-node-editor` or editor widget libraries.

2. **Compilation To Flat Runtime Plans**:
   - Source graph assets are compiled by `DecisionGraphCompiler` into immutable runtime execution plans:
     - `CookedBehaviorTreePlan`
     - `CookedStateMachinePlan`
     - `CookedUtilityPlan`
   - A cooked plan is a contiguous array of node descriptors with precomputed integer jump offsets, child ranges, decorator masks, and blackboard index offsets.
   - Cross-paradigm call nodes reference an entry in the cooked plan's dependency table. Entries use stable asset identity and expected plan kind; they never contain runtime pointers or inline copies of another plan. Cooking rejects missing references, kind/schema mismatches, dependency cycles, and nesting beyond the configured bound.
   - Static plans are shared and read-only across all agent instances executing the asset, eliminating per-agent plan duplication and pointer chasing.
   - Per-agent dynamic state is stored in an allocation-conscious `DecisionInstanceState`. Parallel composites own bounded active branches, and each branch owns a bounded stack of cross-plan execution frames. Hot-reload and restore map state by `(DecisionGraphAssetId, DecisionNodeId)`, never by transient array index.

```cpp
struct CookedDecisionNode {
    DecisionNodeId           stableId; // persisted identity; survives recompile index shifts
    DecisionNodeTypeId       typeId;
    uint16_t                 parentIndex;
    uint16_t                 firstChildIndex;
    uint16_t                 childCount;
    uint16_t                 decoratorMask;
    DecisionNodeKind         nodeKind; // Composite, Decorator, Task, Service
    std::span<const uint8_t> staticPayload; // includes Wait duration range and other static params
};

struct DecisionSubplanRef {
    DecisionGraphAssetId assetId;
    DecisionPlanKind expectedKind;
    uint32_t requiredSchemaVersion;
};

struct DecisionExecutionFrame {
    DecisionGraphAssetId planAssetId;
    DecisionNodeId activeNodeId;
};

struct ActiveDecisionBranch {
    static constexpr uint16_t kMaxSubplanDepth = 8;
    std::array<DecisionExecutionFrame, kMaxSubplanDepth> frames{};
    uint16_t frameCount{0};
    JobHandle runningTask{};
};

struct DecisionInstanceState {
    static constexpr uint16_t kMaxActiveBranches = 8; // Parallel fan-out bound
    std::array<ActiveDecisionBranch, kMaxActiveBranches> activeBranches{};
    uint16_t activeBranchCount{0};
    // decorator memory and timers use (planAssetId, activeNodeId), never array indices
};

struct CookedDecisionPlanHeader {
    DecisionGraphAssetId                assetId;
    uint32_t                            schemaVersion;
    BlackboardSchemaId                  requiredBlackboardSchema;
    std::vector<DecisionSubplanRef>      subplanDependencies;
};

struct CookedBehaviorTreePlan {
    CookedDecisionPlanHeader             header;
    std::vector<CookedDecisionNode>     nodes;
    std::vector<uint16_t>               serviceIndices;
};
```

`CookedBehaviorTreePlan`, `CookedStateMachinePlan`, and `CookedUtilityPlan` all
begin with the same `CookedDecisionPlanHeader`; therefore any paradigm can own
validated subplan dependencies without merging or copying the referenced plan.

### Core 1.0 AI Paradigms

Horo 1.0 standardizes on three complementary decision paradigms:

#### 1. Behavior Trees (BT)

- **Composites**:
  - `Selector`: Evaluates children sequentially until one succeeds or runs.
  - `Sequence`: Evaluates children sequentially until one fails or runs.
  - `Parallel`: Evaluates all children concurrently with configurable completion policies (`RequireOneSuccess`, `RequireAllSuccess`, `RequireAllComplete`, `StopOthersOnFailure`).
- **Decorators (Conditions & Flow Control)**:
  - `Inverter`: Inverts child result (`Success` $\leftrightarrow$ `Failure`).
  - `Cooldown`: Enforces a mandatory cooldown duration between child executions.
  - `Loop`: Repeats child execution for a fixed count or while a condition is satisfied.
  - `BlackboardCheck`: Compares blackboard keys against constants, ranges, or other keys with reactive abort policies (`None`, `Self`, `LowerPriority`, `Both`).
  - `TimeLimit`: Aborts child task if execution exceeds a specified duration.
- **Tasks (Leaf Action Nodes)**:
  - `MoveTo`: Issues asynchronous pathfinding and movement requests to `NavigationSystem`.
  - `Wait`: Pauses branch execution for a fixed or randomized duration.
  - `PlayAnim`: Triggers an animation montage or clip and waits for completion.
  - `CustomTask`: User-defined native C++ or script tasks conforming to the decision task contract.
- **Services**:
  - Periodic background evaluations attached to composite or subtree nodes, updating blackboard values or environment queries while the subtree remains active.

#### 2. Hierarchical State Machines (HSM)

- `StateMachineAsset` is the canonical asset family for both flat and hierarchical state topology. HSM is the execution paradigm, not a separate persisted asset type.
- Hierarchical states with nested sub-state machines.
- Explicit entry actions, update actions, and exit actions.
- Event-triggered and condition-triggered transitions evaluated against blackboard state.
- State machines can host Behavior Trees as nested sub-state behaviors.

#### 3. Simple Utility Scoring

- Evaluates competing actions using consideration response curves (Linear, Polynomial, Logistic, Step).
- Normalized scoring $[0.0, 1.0]$ with priority weighting and multiplier aggregation.
- Utility selector selects the top-scoring action or samples from a top-tier bucketed probability distribution.

#### 4. Explicit Post-1.0 Extension Paradigms

Advanced planning and learning models are explicitly classified as **Post-1.0 Extensions**:

- **Hierarchical Task Networks (HTN)**: Domain planning with methods and compound tasks.
- **Goal-Oriented Action Planning (GOAP)**: Dynamic action graphs resolved via regression over world-state preconditions and effects.
- **Reinforcement Learning (RL) & Learned Policies**: On-device neural network inference / ML-agent decision models.
- **Conversational / Large Language Model (LLM) Decision Providers**: External generative AI NPC reasoning seams.

These paradigms will integrate via dedicated provider extension seams without breaking or modifying the 1.0 decision core.

### Runtime Task & Lifecycle Alignment

Implementation status on 12 September 2026: `HoroEngine::AI` provides the
generation-fenced `AiTaskLifecycle` contract shared by native, script, and graph
tasks. It owns the `Idle -> Running -> Succeeded | Failed | Cancelled` transition,
one immutable terminal result, detached operation context, and post-terminal
cleanup claim. Decision-plan execution and concrete task adapters remain later work.

Headless hosts that deliberately omit gameplay AI compose `NullAiRuntime`. Its
availability is always false and every task admission returns the typed
`ai.runtime.unavailable` failure without starting a lifecycle or fabricating a
successful decision. Focused lifecycle tests use a renderer-, audio-, editor-,
and Scene-independent deterministic harness over small admitted blackboard schemas,
typed agent/task generations, and declared scripted outcomes. The harness hashes
canonical integer bytes and transition facts with fixed FNV-1a and exposes explicit
cancellation, stale-completion, and capacity-rejection fault points.

1. **Standard Execution Context**:
   - AI tasks evaluate through `BehaviorExecutionContext` (extending `BehaviorContext`), granting controlled access to scene resources, typed blackboard views, input, command buffers, and cancellation tokens.
   - `AIDecisionSystem` is the sole scheduling authority in `AiDecisionEvaluate` (ADR-021 `AiDecision`). `AiControllerComponent` and eligible `BehaviorComponent` attachments are inert plan bindings discovered by that system; components do not own runners. Generic `OnFixedUpdate` behaviors still run later in `Gameplay` / `CharacterControllerLocomotion`.
   - `GameplayInputAccess` is inherited for task parity with generic gameplay behaviors. It exposes only read-only semantic actions for possessed/player-controlled entities, never raw device state; NPC-only contexts receive a deterministic empty snapshot.

`AiTaskOperationContext` captures only the persistent task definition, exact
task/agent runtime handles, and cooperative cancellation token. It never retains
mutable Scene state. `AIDecisionSystem` starts the lifecycle and calls
`PrepareResume(FixedTick | Event, activeAgent)` before invoking task code. Detached
worker completions return to that owner and call `CompleteSuccess` or
`CompleteFailure`; both revalidate the exact agent generation and cancellation
token before publishing. Native, script, and graph adapters do not define their
own status vocabularies or terminal-result stores.

1. **Cooperative Asynchronous Task Execution**:
   - While a task is `Running`, `PrepareResume` admits recurring fixed-tick or event resumes until completion or cancellation.
   - Long-running async requests (e.g. `MoveTo` navigation or animation playback) use the captured `CancellationToken`. Cancellation requested before admission, during execution, or after a worker completion is resolved at the serialized owner boundary. Whichever terminal transition publishes first remains immutable.
   - Agent generation retirement publishes `Cancelled(AgentGenerationRetired)` before task-owned resources disappear. The lifecycle owner then claims cleanup exactly once and acknowledges completion after downstream requests and subscriptions are released. Late worker completions cannot replace that terminal result.

1. **Safe-Point Hot Reload And Plan Replacement**:
   - Asset compilation produces a new `CookedDecisionPlan`.
   - Running instances migrate at tick boundaries: compatible active paths retain state; incompatible edits trigger `OnAbort()` on active tasks and restart evaluation from the root node.
   - Stale or invalid plans never replace the active runtime plan and log typed compiler diagnostics.

1. **Save And Restore**:
   - Under ADR-114, the AI subsystem contributes one owned canonical state adapter to `RuntimeSaveService` and captures state only at the aggregate save snapshot safe point. Component reflection and Scene serialization are not alternate authorities.
   - Durable state includes plan asset/schema identity, bounded execution-frame stacks keyed by stable `DecisionNodeId`, serializable decorator/timer memory, and schema-approved blackboard values.
   - `JobHandle`, cancellation tokens, path/query handles, pointers, and cooked array indices are transient and never serialized. Restore resolves plans in staging and restarts asynchronous work only after atomic state application. Missing or incompatible plans reset the affected agent to its root with a typed diagnostic.

1. **Blackboard Storage And Typed Keys**:
   - Blackboard state is shared between behaviors and AI nodes:

`PerceptionManager` does not write `AIBlackboardView` directly. At the end of the
Perception phase it publishes immutable staged `PerceptionDelta` records.
`AIBlackboardSyncSystem` alone maps those deltas and last-known facts into
schema-approved keys during `SystemPhase::BlackboardSync`. Decision tasks and
Service nodes consume the committed view and are not alternate writers for this
seam.

```cpp
struct AIBlackboard {
    std::optional<EntityId>        target;
    std::optional<WorldCoordinate> lastKnownTargetPosition;
    std::vector<WorldCoordinate64>   patrolPath;
    uint32_t                       patrolIndex;
    float                          alertLevel;
    AIState                        currentState;
};
```

Navigation commands (move-to, follow, patrol) are issued from behavior nodes
and executed by the navigation system.

### Perception Save And Restore

The AI subsystem's ADR-114 canonical adapter contributes its versioned perception
state. `PreserveMemory` captures durable sense/tag data,
stable source references where available, last-known position/velocity, strength,
and simulation age; `ResetOnRestore` explicitly opts ambient agents out.

Raycast queues, scheduler buckets/deadlines, raw entity generations, and in-flight
event buffers are transient. Restore resolves stable sources during staging,
retains last-known facts when a source no longer exists, republishes restored
deltas at the first `BlackboardSync`, and resumes decay from saved simulation age
without applying wall-clock offline time.

## Crowd Simulation

[ADR-109](../../adr/109-avoidance-crowd-and-renderer-independent-budget.md)
separates route/corridor and preferred-velocity production, local safe-velocity
computation and Character/Physics movement. NavigationRuntime owns logical agents,
immutable fact batches, project-profile admission, stable scheduling and
`NavIntentCommit` publication. An optional `INavigationCrowdBackend` owns only its
private bounded safe-velocity kernel/state; Character/Physics remains final
collision, velocity and transform authority.

When that backend is composed, group coordination may request:

- Local steering and dynamic avoidance
- Safe-velocity evaluation for Gameplay-authored formation/lane preferred velocities
- Bounded density facts used only by the local solver

```cpp
struct CrowdAgentConfig {
    float    neighborRadius;
    uint32_t maxNeighbors;
    float    avoidanceRadius;
    float    maxSpeed;
    float    maxAcceleration;
};
```

The 1.0 best-effort provider is ADR-104's optional DetourCrowd adapter. Horo does
not call it ORCA/RVO or promise collision-free motion, and it cannot satisfy the
initial deterministic tier. Horo ships no separate ORCA/RVO kernel for 1.0.
Deterministic simulations either compose a separately qualified provider, use the
declared collision-safe no-avoidance policy or fail admission when avoidance is
required.

Scale (`CrowdSmall`, `CrowdMedium`, `CrowdLarge`, `CrowdDedicated`) and quality
(`AvoidanceOff`, `AvoidanceConservative`, `AvoidanceBalanced`, `AvoidanceDense`)
are independent project dimensions with exact finite capacity, fact, work, memory,
batch and result-age envelopes in ADR-109. Best-effort selection uses authored
priority, deadline age and stable agent ID; camera visibility/render LOD and
graphics backend are never inputs. Late/unselected work has bounded validated
velocity reuse followed by the configured collision-safe fallback.

Formation, density and corridor-formation policy remain Horo Gameplay/AI
orchestration above the provider. Traffic/road lane graphs are a separate future
navigation domain, not a crowd flag. Products needing only grounded paths do not
link or allocate DetourCrowd.

## Asynchronous AI Tasks And Job System Integration

Asynchronous AI workloads—such as NavMesh pathfinding, best-effort crowd batches,
perception visibility raycasts, and tactical environment queries—must execute
through the Foundation `JobSystem`:

- **Single Task Scheduler**: AI subsystems must not instantiate private worker thread pools or bespoke background task runners.
- **Scene-Scoped Cancellation**: Every async AI job captures a `CancellationToken` bound to the active `SceneRuntime` generation. When a scene unloads or transitions, all active AI jobs are cancelled immediately.
- **Worker Thread Invariant**: Background AI jobs never mutate scene ECS components or live blackboard instances directly. Completed results (e.g. `PathfindingResult`, visibility test results) are queued into thread-safe result buffers and applied to agent memory on the main simulation thread at designated phase safe points.

## Debugging And Visualization

[ADR-110](../../adr/110-navigation-editor-surface-and-command-ownership.md)
maps navigation tooling onto the shared editor architecture. A persistent
`NavigationDefinitionDocument` and ordinary `SceneDocument` commands own authored
definition/Scene intent. The dockable `NavigationTab` is a closed-by-default query
and action projection; ADR-106 `NavigationBakeService`/`OperationStore` owns every
accepted bake, so panel hide/close never cancels work or publishes an artifact.

Live inspection uses a bounded backend-neutral `INavigationInspectionQuery` over
immutable world/topology/overlay/profile generations. Viewport overlays consume
neutral debug primitives and own presentation state only. UI callbacks and provider
extensions receive no mutable runtime/provider/native object; authorized debug
actions use typed application/runtime commands at declared safe points. Navigation
surfaces use the shared design system and localization catalogs.

- NavMesh visualization overlay (walkable areas, obstacles, off-mesh links)
- Path visualization (active paths with waypoints)
- Perception visualization (sight cones, hearing radii, known stimuli)
- AI debug panel (blackboard inspector, behavior tree state, active path)
- NavMesh generation diagnostics (build time, coverage percentage)

The closed-by-default `GameplayAiTab` reads bounded immutable, generation-scoped
runtime snapshots. Hiding/closing it stops polling and releases leases only; it
cannot cancel tasks, EQS work, simulation or PIE. Authorized developer mutations
are typed, permissioned runtime commands committed at fixed-tick safe points, never
direct widget writes. Provider UI remains inert metadata/host-rendered schema.

## Simulation Lifecycle And Fixed-Tick Phase Ordering

AI simulation executes as a fixed-timestep pipeline within the engine's fixed
update loop, defined by [Runtime Lifecycle Architecture](./runtime-lifecycle.md).
Six ordered phases execute in each simulation tick; they implement the ADR-021
coarse groups. A variable-rate presentation
bridge runs afterward and consumes the committed snapshots:

```text
PerceptionSensePoll
        |  (Raw sensory stimuli buffers)
        v
  BlackboardSync
        |  (Committed agent knowledge & alerts)
        v
AiDecisionEvaluate
        |  (High-level action & movement intent)
        v
 NavIntentCommit
        |  (Pathfinding queries, steering & avoidance velocity)
        v
CharacterControllerLocomotion  (Physics Integration)
        |  (Committed world transforms & collision contacts)
        v
 AnimationRigUpdate
        |  (Skeletal pose evaluation)
        v
  RenderExtraction  (variable-rate presentation bridge; not a numbered phase)
        |  (Immutable presentation snapshot)
```

### Phase Contracts And Invariants

Phases 1-6 run inside the fixed simulation tick and may mutate simulation state
under the phase contracts below. `RenderExtraction` is the following
variable-rate presentation bridge, not a seventh fixed-tick phase.

1. **`PerceptionSensePoll`**:
   - Gathers all ADR-024 sensory stimuli (Sight line-of-sight candidates, Hearing events, Damage dispatches, Touch/Proximity overlaps, and Team/Affiliation events or bounded relays) into staged per-agent stimulus buffers.
   - Operates as a read-only pass over physics spatial structures and audio event queues.
   - Invariant: Does NOT mutate agent blackboard state, behavior tree node states, or world transforms.
2. **`BlackboardSync`**:
   - Ingests staged stimulus buffers into each agent's `BlackboardState`.
   - Evaluates stimulus decay over time, updates last known target locations, adjusts agent alert levels, and processes incoming team/squad broadcast events.
   - Invariant: All blackboard mutations are completed within this phase; blackboards become read-only during subsequent decision evaluation.
3. **`AiDecisionEvaluate`**:
   - Evaluates behavior trees, finite state machines, or utility AI models against current blackboard state.
   - Selects agent actions, targets, and tactical states.
   - Invariant: Pure decision evaluation. Does NOT directly manipulate physics bodies, apply forces, or invoke rendering commands. Emits high-level movement/action intent.
4. **`NavIntentCommit`**:
   - Translates movement intent into concrete navigation actions: submits asynchronous pathfinding requests, follows active waypoints, and evaluates the selected local safe-velocity capability.
   - Computes desired kinematic horizontal/vertical velocity vectors for character locomotion.
   - Invariant: Submits desired velocity to character controllers without stepping the physics world directly.
5. **`CharacterControllerLocomotion`**:
   - Executes character controller updates within the physics tick step. Resolves collisions, ground support, slope limits, stepping, and external forces.
   - Commits authoritative entity world positions, orientations, and velocities.
   - Invariant: Transforms and velocities committed in this phase represent the authoritative simulation state for the tick.
6. **`AnimationRigUpdate`**:
   - Evaluates animation blend trees, locomotion state machines, and procedural IK based on committed locomotion velocity and active action tags.
   - Computes skeletal bone transform matrices.
   - Invariant: Evaluates presentation pose from committed simulation data; animation updates do not retroactively alter physics locomotion within the same tick.

### Variable-Rate Presentation Bridge — `RenderExtraction`

This bridge is not a fixed-tick phase.

- Extracts immutable presentation snapshots from previous and current simulation ticks using interpolation factor $\alpha$.
- Executed during variable-rate frame updates, completely decoupled from fixed-tick simulation loops.
- Invariant: Strictly read-only presentation extraction.

## Network Authority And Host Roles

The execution of AI simulation phases is strictly governed by the host process's network role:

```text
[ Dedicated Server (Authoritative Host) ]
  ├── Phase 1: PerceptionSensePoll
  ├── Phase 2: BlackboardSync (SERVER-PRIVATE)
  ├── Phase 3: AiDecisionEvaluate (SERVER-PRIVATE)
  ├── Phase 4: NavIntentCommit & Avoidance
  └── Phase 5: CharacterControllerLocomotion (Physics Authority)
           │
           │ (Replicated Public Transforms, Velocities, Action Tags)
           ▼ [ Network Transport Layer ]
[ Client Host (Presentation-Only Peer) ]
  ├── Network Jitter Buffer & Snapshot Ingestion
  ├── Transform Interpolation (alpha)
  ├── Client AnimationRigUpdate (Visual Blending)
  └── RenderExtraction Bridge -> Viewport Presentation
  (NO perception queries, NO blackboards, NO decision trees)
```

### Host Role Matrix

| Host Role | Authority Scope | Executed Phases | Network Replication Output |
|---|---|---|---|
| **Standalone** | Full local simulation and presentation authority. | Fixed phases 1–6, then the variable-rate extraction bridge. | None (local single process). |
| **Dedicated Server** | Authoritative simulation owner for all AI agents. | Fixed phases 1–5; phase 6 only when authoritative skeletal hitboxes require it; no presentation bridge. | Replicates entity `NetworkId`, transform, velocity, and public animation tags. |
| **Client** | Presentation-only consumer for server-replicated AI. | Client animation update plus the variable-rate extraction bridge; no authoritative AI phases for server-owned agents. | Receives replication; submits no AI authority. |

### Authority, Privacy, And Security Boundaries

- **Clients Never Run Authoritative AI**: Connected clients NEVER run `PerceptionSensePoll`, `BlackboardSync`, `AiDecisionEvaluate`, or `NavIntentCommit` for server-owned AI agents. Clients receive replicated transform, velocity, and state tags from the server.
- **Server State Privacy**: Agent perception memories (e.g. sight awareness meters, target tracking scores) and `BlackboardState` internal representations (behavior tree execution nodes, patrol indices, threat scoring matrices) are **server-private**.
- **No Private State Serialization**: Network replication protocols MUST NOT synchronize private perception data or blackboard state to clients. Only publicly observable gameplay attributes (positions, rotations, locomotion speeds, equip states, public audio cues) are sent over the wire. This prevents client-side wallhacks, radar exploits, and unnecessary bandwidth consumption.

## Simulation Execution Modes

The engine supports two explicit simulation scheduling modes for AI:

| Simulation Mode | Scheduling Contract | Primary Use Cases | Allowed Host Roles |
|---|---|---|---|
| **Deterministic Fixed-Tick** | Strict lockstep execution. Every active agent is evaluated on every fixed tick in deterministic entity-ID order. Time-slicing skips, frame-rate dependent heuristics, and random job interleavings are forbidden. | Lockstep multiplayer, replay recording and bit-identical playback, automated AI regression testing. | Standalone, Dedicated Server, Headless Test Harness |
| **Best-Effort Bounded Time-Slicing** | Distance- and significance-based Level of Detail (Simulation LOD). Agents near players update at full frequency; distant agents update at fractional rates (e.g. 1/2, 1/4 rate) with bounded maximum latency guarantees. Job queues are amortized across workers within fixed per-tick execution budgets. | High-density open-world scenes, large-scale RTS/RPG titles, single-player games exceeding per-tick CPU budgets. | Standalone, Dedicated Server |

Client hosts in networked multiplayer run neither mode for remote AI; they perform presentation-only state interpolation.

## Gameplay AI Profiles And Simulation Budgets

Gameplay decision cadence and perception query fidelity are configured through
typed `GameplayAiProfile` definitions. Navigation/crowd scale, quality, execution
mode and fallback are selected independently through ADR-109
`NavigationCrowdProfile`; dynamic-obstacle policy is owned by ADR-108.

```cpp
enum class AiLodSchedulingPolicy : uint8_t {
    FullRate,
    DistanceBands,
    PriorityAged,
};

struct GameplayAiProfile {
    std::string_view profileName;
    uint32_t         maxPerceptionQueriesPerTick;
    uint32_t         perceptionWorkerThreads;
    AiLodSchedulingPolicy lodSchedulingPolicy;
    float            highFrequencyRadius;      // Distance (m) for full-rate evaluation
    float            mediumFrequencyRadius;    // Distance (m) for 1/2 rate evaluation
    float            lowFrequencyRadius;       // Distance (m) for 1/4 rate evaluation
};
```

### Standard Engine Profiles

| Feature / Budget Parameter | `LowCpu` | `MediumCpu` | `HighCpu` | `DedicatedServer` |
|---|---|---|---|---|
| **Target CPU Threads** | 2–4 | 6–8 | 12+ | 16+ |
| **Perception Queries / Tick** | 16 | 128 | 512 | 1,024 |
| **Perception Workers** | 1 | 2 | 4 | 4 |
| **LOD Scheduling Policy** | `DistanceBands` | `DistanceBands` | `DistanceBands` | `PriorityAged` |
| **High-Frequency Radius** | 15 m | 25 m | 35 m | 0 (unused) |
| **Medium-Frequency Radius** | 35 m | 60 m | 90 m | 0 (unused) |
| **Low-Frequency Radius** | 60 m | 120 m | 180 m | 0 (unused) |

These values fully initialize every `GameplayAiProfile` field; each column name
is its `profileName`. Built-in profiles are exact defaults; larger hosts use
validated project-defined profiles rather than interpreting `+` as an unbounded
runtime value. `FullRate` disables time-slicing and ignores zero-valued radii.
`DistanceBands` requires strictly increasing positive radii. `PriorityAged`
requires zero radii and uses the weighted fair queue plus deadline aging defined
by ADR-024.

`maxPerceptionQueriesPerTick` caps all costly spatial and physics queries admitted
by `PerceptionSensePoll`. ADR-024's `maxSightRaycastsPerTick` is the LOS-raycast
subset and must not exceed the aggregate cap. Event receipt is free of this query
budget; any resulting LOS, overlap, or spatial lookup consumes one admitted query.

### AI Schedule And Profile Verification

- Lifecycle tests assert exactly six ordered fixed-tick phases and a read-only
  post-commit `RenderExtraction` bridge.
- Host-role tests prove that dedicated servers do not depend on extraction and
  that clients cannot submit authoritative AI work for server-owned agents.
- Profile fixtures initialize every field from the canonical table and reject
  invalid perception worker counts, radius/policy combinations, and sight-raycast
  budgets above the aggregate perception-query cap. ADR-109 fixtures separately
  validate every navigation crowd scale/quality/mode/fallback envelope.
- Shipping replication schema tests reject private blackboard and perception
  memory fields.

### Graphics Decoupling Invariant

**Architectural Rule**: Graphics backends (`OpenGL`, `Metal`, `Vulkan`, `NullRenderer`) grant **zero** AI capacity, perception query fidelity, or gameplay authority.

- Selecting a higher-end graphics API (e.g. Vulkan vs OpenGL) does NOT increase AI agent limits or perception budgets.
- Headless dedicated servers running with `NullRenderer` operate with full CPU/memory capacity and are not artificially throttled by presentation-tier checks.
- AI/navigation budgets come exclusively from validated project/host-role profiles;
  CPU and memory measurements inform those authored choices but do not silently
  change authoritative capacity at runtime.

## Navigation Resource Guidance

Navigation scaling depends on measured CPU/worker/memory costs, not graphics
features. ADR-109's `NavigationCrowdProfile` is the authority for logical/provider
agent capacity and avoidance work/memory; ADR-108 owns dynamic-obstacle budgets.
`GameplayAiProfile` does not duplicate either. Navigation cache, query scratch and
path queues remain separate finite validated host/project settings. Tuning changes
require workload, platform and measurement evidence, not a renderer label.

## Navigation Boundary And Integration Verification

| Case | Required result |
|---|---|
| Api-only consumer uses SceneMath | Builds through Foundation without invented SceneMath target |
| Runtime linked without a concrete provider | Builds; host injection supplies capabilities |
| Runtime-only server without builder/graphics | Real queries and optional crowd only when selected; no bake or graphics link requirement |
| Pinned Recast/Detour source/options/modules | Exact commit/fingerprint, no floating/system source; Detour runtime and Recast bake baselines with TileCache/Crowd opt-in only |
| Direct/transitive vendor include or public vendor type | Boundary negative fixture fails; no ABI leak |
| Concurrent queries, obstacle edits, and carving | Immutable revisions; no shared mutable vendor instance races |
| Unrelated region changes / contributing tile changes | Unrelated cache entries may survive; affected entries and late paths cannot publish stale topology |
| Agent removed, slot reused, or scene replaced during query | InvalidHandle; no mutation of replacement agent |
| Cell eviction while a query holds a lease | Logical removal immediately prevents new/late use; bytes freed only after readers retire and remain budgeted until then |
| World-cell staging fails or wst forced eviction occurs | Shared transaction/rollback and residency authority preserved |
| Null versus real-provider admission, cancellation, publication | Same phase/order contract; Null never returns fake geometry success |
| Different worker completion orders in deterministic mode | Same declared-tick output; no timing-dependent result selection |
| Queue/result capacity exhausted or shutdown begins | Typed admission rejection/cancellation; no orphan job or operation |
| Authoring geometry/settings/dependency/target changes | Correct versioned cook-key invalidation and stable AssetId |
| Cooked version/digest/indices invalid | Typed failure before tile installation |
| Provider allocation failure / destruction | Tracked bounded failure; no leaks, partial publication, or hook-lifetime violation |
| Origin rebase with a query in flight | Dynamic coordinates converted/rejected consistently; static topology identity unchanged |
| Flying/swimming/space or traffic/lane request | Explicit distinct-capability unsupported result; never coerced into a grounded agent profile/area flag |

These are required downstream runtime/CI tests, not tests implemented by this ADR-only change.

## Related Documents

- [ADR-013: Environment Query Ownership, Item and Scoring Model](../../adr/013-environment-query-ownership-item-and-scoring-model.md)

- [ADR-010: Job Waiting and Operation Store Ownership](../../adr/010-job-waiting-and-operation-store-ownership.md)
- [ADR-017: Prefab Asset and Spawn Contracts](../../adr/017-prefab-role-ownership-and-capability-tiers.md)
- [ADR-018: Command Dispatch and World Streaming Diagnostics](../../adr/018-command-registration-permissions-threading-and-packaged-build-policy.md)
- [ADR-023: World Index and Cell Format](../../adr/023-world-index-and-cell-format-architecture-decision.md)
- [ADR-026: Large-World Precision](../../adr/026-large-world-precision-and-floating-origin-strategy.md)
- [Asset Pipeline](./asset-pipeline.md)
- [World Streaming](./world-streaming-architecture.md)
- [Header Visibility and Ownership](../foundation/header-visibility-and-ownership.md)
- [ADR-016: Navigation Target Ownership and Dependency Boundary](../../adr/016-navigation-target-ownership-and-dependency-boundary.md)
- [ADR-104: Default Navigation Provider and Recast-Detour Adoption](../../adr/104-default-navigation-provider-and-recast-detour-adoption.md)
- [Navigation Bake UI Reference](./navigation-bake.html)

- [ADR-021: Gameplay AI Ownership, Scheduling and Behavior Boundary](../../adr/021-gameplay-ai-ownership-scheduling-and-behavior-boundary.md)
- [ADR-022: AI Fixed-Tick Order, Authority and Simulation Budget](../../adr/022-ai-fixed-tick-order-authority-and-simulation-budget.md)
- [ADR-024: Perception Ownership, Sense Policy and Budget Decision](../../adr/024-perception-ownership-sense-policy-and-budget.md)
- [ADR-025: AI Decision Assets and Shared Gameplay Behavior Boundary](../../adr/025-ai-decision-assets-and-gameplay-behavior-boundary.md)
- [Runtime Lifecycle Architecture](./runtime-lifecycle.md): Fixed-tick simulation loop and interpolation model
- [Multiplayer Replication Architecture](./multiplayer-replication-architecture.md): Server authority, RPCs, and property replication
- [Networking Architecture](./networking-architecture.md): Network transport and I/O threading
- [Gameplay Behavior Authoring](../extensions/gameplay-behavior-authoring.md): Behavior tree and state machine authoring
- [Physics Architecture](./physics-architecture.md): Collision geometry and character controller locomotion
- [World Streaming Architecture](./world-streaming-architecture.md): NavMesh tile streaming
- [Scene Runtime](./scene-runtime.md): Agent entity and component model
- [Concurrency And Jobs](../foundation/concurrency-and-jobs.md): Parallel crowd and perception jobs
- [Save Game And Persistence](./save-game-and-persistence.md): durable perception-memory capture and staged restore
- [ADR-114: Canonical Runtime World Persistence Boundary](../../adr/114-canonical-runtime-world-persistence-boundary.md)
- [ADR-137: Terrain and Foliage Ownership, Data, Tier and Lifecycle](../../adr/137-terrain-foliage-ownership-data-tier-and-lifecycle.md)
- [ADR-141: Terrain/Foliage Cross-System Ownership and Readiness](../../adr/141-terrain-foliage-cross-system-ownership-and-readiness.md)
- [Navigation Bake UI HTML Reference](./navigation-bake.html): non-normative
  static UI reference
- [Debug Console And Overlays](./debug-console-and-overlays.md): AI debug visualization
