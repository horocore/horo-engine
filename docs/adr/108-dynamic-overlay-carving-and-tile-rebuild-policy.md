# ADR-108: Dynamic Overlay, Carving and Tile-Rebuild Policy

- **Status**: Proposed
- **Date**: 2026-09-02
- **Supersedes**: None
- **Scope**: Moving agents and platforms, transient obstacles, doors, modifiers, terrain edits, destruction and streamed geometry; avoidance, logical blocker overlays, optional DetourTileCache carving, transactional runtime tile rebuild, authored recook, consistency, latency, budgets, failure, networking, persistence and lifecycle
- **Issue**: [NAV-005.1](https://github.com/HoroCore/horo-engine/issues/1267)
- **Jira**: [HORO-1267](https://horo-engine.atlassian.net/browse/HORO-1267)
- **Related**: [ADR-010](010-job-waiting-and-operation-store-ownership.md), [ADR-012](012-world-streaming-partition-authority-and-subsystem-boundaries.md), [ADR-016](016-navigation-target-ownership-and-dependency-boundary.md), [ADR-022](022-ai-fixed-tick-order-authority-and-simulation-budget.md), [ADR-085](085-physics-shape-authoring-cook-and-runtime-boundary.md), [ADR-104](104-default-navigation-provider-and-recast-detour-adoption.md), [ADR-105](105-navigation-asset-and-scene-ownership-boundary.md), [ADR-106](106-navigation-bake-ownership-transaction-and-cache.md), [ADR-107](107-navigation-query-consistency-and-snapshot-ownership.md)
- **Normative documents**: [Navigation and AI Architecture](../architecture/runtime/navigation-and-ai-architecture.md), [Terrain and Foliage Architecture](../architecture/runtime/terrain-and-foliage-architecture.md), [World Streaming](../architecture/runtime/world-streaming-architecture.md), [Character Controller Architecture](../architecture/runtime/character-controller-architecture.md)

## Context

ADR-105 makes cooked grounded NavMesh immutable derived data, and ADR-107 makes
queries read one atomically published topology/overlay snapshot. World changes
still need a policy. A moving agent, closing door, dropped crate, destroyed wall,
terrain hole and streamed cell do not have the same cost or topology semantics.
Calling all of them “dynamic obstacles” would let convenient provider features
become hidden world authority.

Detour avoidance changes desired velocity without changing reachability.
DetourTileCache can rebuild tiles from retained compressed layers for a bounded set
of simple obstacles, but it is optional and is not a general geometry cooker.
Arbitrary terrain or destruction changes need new geometry and polygonization;
ADR-104 keeps Recast out of normal runtime-only products unless an explicit future
capability composes it. World Streaming already owns cooked cell tile residency.

Safety also differs by direction. Adding a blocker can be represented immediately
by conservatively forbidding motion through a region. Removing baked obstruction
cannot safely invent newly walkable topology. A failed update must not permit agents
through closed doors or new geometry, and must not silently claim that a destroyed
wall opened a route.

This ADR classifies each change, fixes the one owning mechanism and its fallback,
and defines the consistency/budget contract before implementation.

## Decision

### 1. Four mechanisms have non-overlapping authority

| Mechanism | Changes reachability topology | Owner | Baseline availability |
|---|---:|---|---|
| Local avoidance / safe-velocity computation | No | NavigationRuntime policy plus optional crowd/avoidance provider | Optional; path following remains separate |
| `DynamicObstacleOverlay` / link-gate state | No cooked mutation; immediately constrains valid movement/query use | NavigationRuntime | Required logical capability when runtime dynamic blockers are enabled |
| `DynamicTileCarving` | Publishes a derived runtime topology generation from retained admitted tile layers | Optional topology provider (`DetourTileCache` initially), coordinated by NavigationRuntime | Optional |
| `RuntimeGroundedTileRebuild` | Publishes a derived runtime topology generation from a new immutable geometry snapshot | Explicit host-composed builder capability plus NavigationRuntime | Not part of the 1.0 runtime baseline |

Authored recook is a fifth, non-runtime path. Persistent definition, Scene, terrain,
foliage or collision changes go through ADR-106 `NavigationBakeService` and Asset
Pipeline publication. World Streaming tile add/remove is another distinct path: it
installs already cooked ADR-023 payloads through its cell transaction and is never
classified as carving or runtime rebuild.

Avoidance cannot create/remove polygons or prove a route. An overlay can only
constrain or invalidate use of the immutable base. Carving/rebuild creates a
detached runtime candidate and atomically swaps the ADR-107 combined root; neither
edits the cooked artifact. Provider support for one mechanism grants no other
capability.

### 2. Every change category has one primary mechanism and safe fallback

| World change | Primary mechanism | Optional refinement | Capability absence/failure |
|---|---|---|---|
| Moving navigation agent | Path/corridor plus local avoidance/safe velocity; Character/Physics owns movement | Optional crowd provider | No-avoidance policy retains bounded path following with collision-safe stop; never carve the agent |
| Moving non-agent vehicle/body | Logical dynamic blocker overlay | Carve only after explicit stationary qualification | Conservative blocker/affected-path invalidation; overflow is typed and owning gameplay uses safe stop/replan |
| Newly placed crate/simple blocker | Logical blocker overlay immediately | Supported bounded box/cylinder carving | Overlay remains authoritative; unsupported shape/carving returns typed outcome |
| Door open/closed | Stable `NavigationLinkId` gate and optional threshold blocker | Carving only for geometry not represented by the declared gate | Closed fails safe; open cannot claim new reachability until gate/topology publication succeeds |
| Pre-authored modifier/cost toggle | Typed overlay/filter revision for a declared bounded region | Transactional carve only when it changes excluded topology | Prior safe state or conservative exclusion; no ad hoc polygon flag writes |
| Persistent Scene/modifier/source edit | ADR-106 authored recook | None | Last valid artifact may remain visible as stale; play/release required data does not claim current |
| Terrain height/hole/spline edit | Editor/offline affected-tile recook | Explicit runtime tile rebuild capability for authoritative runtime deformation | Block affected region and return `UnsupportedDynamicChange`; never infer new ground |
| Destruction/removal of baked wall or foliage | Authored recook, or explicit runtime tile rebuild from authoritative post-change geometry | None | Old exclusion remains or affected region is conservatively unavailable; removal alone cannot open a path |
| Newly spawned static geometry | Logical blocker overlay first | Carving or explicit runtime rebuild by declared shape policy | Conservative blocker; unsupported update is visible |
| Moving platform/elevator | Character/Physics platform attachment plus explicit timed/conditional navigation link | Future moving-surface navigation capability | No continuous moving NavMesh; link is unavailable when transfer conditions fail |
| Streamed cell geometry | World Streaming atomic cooked tile install/remove | None | `NoNavigationData`/partial result and bounded residency demand; Navigation performs no rebuild or I/O |
| Origin rebase | ADR-026 coordinate translation of dynamic state | None | Static tile topology is not rebuilt; stale epoch results are converted or rejected |

An entity may have multiple roles only through explicit independent components. A
vehicle that navigates and blocks others has an agent identity plus a blocker
identity, with deterministic self-exclusion. It is not carved once per transform
update. A door's visual animation, Physics collision and navigation gate are
separate owners correlated by stable authored identity and committed phase, not by
native handles.

### 3. Moving agents use avoidance, never topology mutation

An agent's path describes preferred grounded reachability. NavigationRuntime
computes a desired velocity from corridor/path following and, when configured,
passes bounded neighbor/obstacle facts to the selected safe-velocity or crowd
provider. Character/Physics alone validates and applies physical movement.

Agent transforms do not enqueue carve/rebuild commands. Carving thousands of agents
would cause topology churn, erase usable corridors and make reachability depend on
worker timing. Other agents are dynamic neighbor facts; large vehicles may also
publish a coarse logical blocker under an explicit dual-role policy.

When avoidance is absent, overloaded or fails, the declared product policy is one
of `CollisionSafeStop`, `PathFollowingOnly` with Character collision enforcement,
or `AvoidanceRequired`. The first is the baseline safe fallback. `AvoidanceRequired`
fails capability admission. No policy silently passes through Physics bodies or
changes the graphics tier to choose agent behavior.

### 4. Logical overlays publish blockers before expensive topology work

Gameplay/Scene owners submit generation-checked `DynamicObstacleCommand` or link/
modifier commands. `NavigationRuntime` validates world/Scene authority, shape,
bounds, priority and command capacity, then applies the stable ordered batch at
`NavIntentCommit`. A successful batch publishes a new immutable overlay revision in
the combined ADR-107 root.

Adding/closing a blocker is fail-closed: the new overlay conservatively invalidates
intersecting paths and prevents new movement through the region before carving or
rebuild completes. Removing/opening intent cannot expose space that the current
topology does not contain. It removes the overlay only when the underlying baked or
new candidate topology and link state prove the route.

Overlay shapes are bounded Horo primitives/regions with stable obstacle IDs,
generation, owner, world, transform and policy. They contain no `dtObstacleRef` or
Physics pointer. Transform updates are coalesced latest-wins per obstacle; create/
remove ordering is deterministic. Queue overflow, invalid shape/world, stale handle
or unsupported policy rejects the command without partially changing the overlay.

The NAV-005.3 runtime contract projects immutable Scene dynamic-registry snapshots
onto topology-owner-supplied authored surface identities, exact world/topology
generations, and finite extents in the
same canonical Scene-local metre frame as grounded bake partitions. It keeps the
supported box/cylinder shape and layer facts per surface, clips conservative
footprints to that surface, and reports exact Scene binding and logical overlay
revision even for an empty projection. Bounded, layer-filtered surface segment probes
require topology-owner-split cross-surface paths and carry surface identity and that
same revision even when clear. Surface-keyed changed-region projection unions old/new
blocker bounds for moves, including crossed intermediate surfaces, clips to the surface, and is all-or-error under the
caller's output bound. The owner uses these regions and revision as affected-path
invalidation input; any held path whose exact coverage cannot prove independence
still fails currentness on the global obstacle revision under ADR-107.
The registry coalesces at most 64 newer source revisions for an active obstacle
into one staged update before the safe-point commit; it never writes to cooked
topology or to a retained older snapshot.

### 5. Door and modifier state use semantic gates

A normal door owns an authored stable `NavigationLinkId` or portal/gate descriptor
whose endpoints, grounded profiles and traversal rules were baked and validated.
Gameplay owns open/locked/destroyed state and submits a typed navigation gate
command. Navigation publishes enabled/disabled/cost state at `NavIntentCommit` with
the matching collision/interaction phase contract.

Closing disables traversal immediately and may add a conservative threshold
blocker. Opening enables the link only when the current topology, door generation
and required Physics/Gameplay readiness match. A mesh animation does not implicitly
toggle navigation, and Navigation does not poll render transforms or string tags.

Pre-authored cost/area modifiers may similarly toggle a declared bounded semantic
region through an overlay/filter revision. A runtime command cannot invent a new
area ID, resize arbitrary baked polygons or modify provider flags in place. Geometry
outside the declared gate/modifier contract is a carve/rebuild/recook request.

### 6. Carving is optional bounded subtraction over admitted tile layers

`DynamicTileCarving` initially means the ADR-104 optional DetourTileCache provider
over retained, validated compressed layers admitted with the active cooked tiles.
It supports only declared finite box/cylinder blocker classes, counts and sizes.
It is not arbitrary mesh Boolean geometry, terrain sculpting or a source cooker.

After the overlay publishes, NavigationRuntime groups touched tiles in canonical
coordinate/layer order and submits bounded carving work through Foundation
JobSystem. One private tile-cache candidate has one writer. Work checks cancellation
between deterministic obstacle/tile units and never mutates queried topology.

The candidate records base topology/overlay generations, touched tiles and exact
command sequence. Owner-thread adoption revalidates them and publishes the complete
replacement root at a safe point. Stale work is discarded/retried within policy.
Until adoption, the conservative overlay remains effective. Old topology retires
only after query leases drain.

Removing a carved obstacle may restore only walkability already represented by the
retained admitted base layer. It cannot create geometry absent from the cooked
source. Missing tile-cache layers, unsupported shapes or provider absence leave the
logical overlay path in effect and return a typed non-carved disposition.

### 7. Arbitrary geometry changes require transactional rebuild or recook

`RuntimeGroundedTileRebuild` is a distinct optional capability. It requires the
host to compose a builder in the runtime product, authorize an immutable canonical
post-change geometry snapshot and reserve explicit CPU/memory/staging budgets.
ADR-104's normal runtime-only Recast composition does not provide it.

The coordinator captures exact world/Scene, topology, overlay, geometry, terrain/
destruction and origin revisions. Each affected tile build owns private scratch.
All required tiles validate before one candidate-root publication; failure,
cancellation or staleness publishes none. Runtime output is transient derived
topology and is not inserted into Asset Registry, cook cache, `current.json`, Scene
source or the ADR-106 last-valid artifact.

For editor-authoritative or persistent changes, ADR-106 recook is the required
path. It publishes a new immutable cooked generation, after which normal Scene/cell
replacement installs it. A runtime rebuild cannot be saved as authoring truth. A
save/network system records semantic authoritative world changes and reconstructs
or reapplies them under declared capability; it never serializes native tiles.

### 8. World Streaming installs cooked content, not dynamic rebuild output

Cell load prepares the exact cooked NavigationMesh block and provider resources
under `PartitionEpoch`/`StreamingGeneration`. All required cell tiles publish with
the existing Scene/provider barrier. Cell eviction publishes logical tile removal
and invalidates paths before physical leases retire.

Navigation demand may ask World Streaming for missing cells through its bounded
authority request. It cannot capture streamed render/Physics geometry and rebuild a
parallel NavMesh, keep an evicted tile logically alive, or use carving as a second
residency cache. Cell content changes require a new cooked cell generation or an
explicit separately authorized runtime world-change mechanism above.

Dynamic overlays whose owners are cell-bound close admission and remove/revoke with
the same cell generation. Persistent/session-owned obstacles must name their
separate owner and are reprojected only after new cell topology is Active. A late
carve/rebuild completion from an evicted cell is stale even if memory remains leased.

### 9. Latency and budgets are mechanism-specific

| Mechanism | Visibility target | Work/budget rule |
|---|---|---|
| Agent avoidance | Declared fixed tick before Character locomotion | Bounded neighbors/samples/agents; overload uses stable priority and safe fallback |
| Overlay/gate command | Next eligible `NavIntentCommit`; same target tick only when admitted before its cutoff | Bounded queue, shapes and affected-path invalidations; no heap/I/O/wait in commit |
| Carving | Asynchronous multi-tick candidate after overlay safety publishes | Bounded queued obstacles, touched tiles, work units, scratch/staging and retries |
| Runtime tile rebuild | Asynchronous operation/candidate | Explicit capability plus tile/input/output/memory/concurrency/deadline limits |
| Authored recook | ADR-106 application operation | Latest-wins, dependency-aware cache, cross-process lock and atomic artifact publication |
| Streamed tile change | World Streaming lifecycle | Global cell residency/staging/retired budgets and activation deadlines |

Wall-clock completion is not guaranteed on a saturated runtime. A configured time
target stops admitting new best-effort units at Horo boundaries; it does not preempt
a provider call or drop an authoritative blocker. Deterministic modes use stable
command/tile/agent order and work-unit limits, never “whatever finished this frame.”

Retired topology, staging, query leases and tile-cache layers remain charged until
actually reclaimed. No mechanism double-counts or hides allocations in a provider.
When capacity is unavailable, adding a safety blocker favors the bounded overlay;
opening new reachability fails/deferred rather than bypassing validation.

### 10. Typed results expose applied mechanism and fallback

Dynamic update admission/publication returns a generation-checked handle and one
terminal disposition:

- `OverlayApplied`, `GateApplied` or `AvoidanceApplied`;
- `CarvingQueued` / `CarvedTopologyPublished`;
- `RebuildQueued` / `RebuiltTopologyPublished`;
- `RecookRequired` or `ResidencyRequested`;
- `NoOp` for an identical already-current semantic command;
- `UnsupportedDynamicChange` with required capability/change category;
- `AdmissionRejected` / `CapacityExceeded`;
- `StaleBase` / `InvalidWorld` / `InvalidHandle`;
- `Cancelled`; or
- `ProviderFailure`.

Queued is not success of topology publication. Results include obstacle/change ID,
world and base/result generations, selected mechanism, affected coverage, fallback
state and bounded diagnostic. Unsupported/failure never silently changes to a
different mechanism that could open reachability. A conservative overlay fallback
is reported explicitly and remains query-visible.

### 11. Network and persistence replicate semantic changes only

The authoritative gameplay/network role decides door, obstacle, terrain and
destruction truth. It transmits stable semantic IDs, commands, effective tick and
source/revision fingerprints according to the replication contract. Clients apply
the same supported navigation mechanism and validate resulting generation; they do
not receive provider refs, tile-cache memory or native blobs as authority.

Products requiring navigation-equivalent prediction/lockstep declare the exact
overlay/carve/rebuild capability and determinism fingerprint. ADR-104 does not
qualify TileCache carving or arbitrary runtime rebuild for initial cross-platform
determinism. An unqualified client uses server-authoritative movement/correction or
fails mode admission; it does not assume matching topology.

Save data stores durable semantic state necessary to reconstruct doors/obstacles or
authoritative terrain/destruction changes. Transient avoidance neighbors, pending
jobs, provider refs, carved tiles and runtime scratch are excluded. Restore stages
semantic state against the exact base artifact/cell generation before publishing
the Scene/navigation bundle.

### 12. Failure, replacement and shutdown remain fail-closed

Carve/rebuild candidates never replace the combined root unless every required
output validates and its captured base still matches. Failure retains the prior
topology plus current conservative overlay. For commands intended to remove a
blocker/open reachability, failure retains the closed/excluded state and reports why.

Scene replacement closes dynamic-command admission, cancels candidates and revokes
the `NavigationWorldId`. Cell eviction revokes cell-bound commands/candidates.
Workers may finish against leased data but cannot publish into replacement
generations. Provider replacement rebuilds private candidates from semantic state;
native obstacle/tile refs are never transferred.

Shutdown stops admission, publishes no new intent, cancels/drains work under
ADR-010, retires completion/candidate/overlay state, releases query/topology leases,
then destroys provider instances and allocation hooks. A timeout retains accounted
resources rather than force-freeing them. Cooked artifact leases follow Asset/Scene
lifetime and are never modified during teardown.

### 13. Qualification covers every category and fallback

Required evidence includes:

- moving agents using avoidance/path following without tile churn, deterministic
  self-exclusion for dual-role vehicles and each no-avoidance policy;
- blocker add/move/remove, transform coalescing, queue overflow and immediate
  conservative invalidation before async topology work;
- door open/close/lock/destroy races across Gameplay, Physics and navigation gate
  generations, with closed-state failure retention;
- declared modifier/filter toggles without provider-flag or polygon mutation;
- supported box/cylinder carving, absent capability/layers, unsupported shape,
  touched-tile/order/budget bounds, stale candidate and lease-safe retirement;
- terrain height/hole/spline and destruction/foliage add/remove with explicit
  recook, optional runtime rebuild and unsupported fail-closed behavior;
- moving platform conditional links and rejection of continuous moving NavMesh in
  the baseline;
- cell load/replace/evict with no parallel loader/rebuild and stale late completion;
- cooked artifact/cache/source byte immutability during every overlay/carve/rebuild
  success, failure, cancellation and shutdown path;
- exact per-mechanism result/fallback reporting and no queued-as-published outcome;
- fixed-tick/work/memory/queue/retired-resource overload behavior independent of
  renderer tier and worker completion timing; and
- save/network semantic reconstruction, capability mismatch and provider/world/
  Scene replacement without native identity transfer.

## Consequences

### Positive

- Every common world change has one named mechanism and observable fallback.
- New blockers become safe immediately without waiting for expensive topology work.
- Cooked assets remain immutable while optional runtime topology can evolve through
  atomic generations.
- World Streaming, Character/Physics, Gameplay and Navigation retain distinct
  authorities.
- Unsupported terrain/destruction/moving-surface behavior fails visibly instead of
  inventing reachability.

### Costs

- The runtime must maintain semantic overlays and source-generation provenance in
  addition to provider topology.
- Optional carving needs retained layers, staging memory and delayed retirement.
- Arbitrary runtime geometry rebuild requires a substantially heavier explicitly
  composed product capability.
- Removing baked blockers may remain unavailable until recook/rebuild completes.

## Rejected Alternatives

### Carve every moving agent and object

Rejected because per-transform topology churn is unbounded, harms corridor quality
and makes reachability timing-dependent. Agents use avoidance; only qualified
blockers may request carving.

### Mutate polygons or cooked NavMesh tiles in place

Rejected because queries could observe partial changes, rollback would be
impossible and derived provider state would become a second artifact authority.

### Treat DetourTileCache as a general runtime geometry cooker

Rejected because it rebuilds from retained admitted layers for supported obstacle
shapes. It does not own arbitrary terrain/destruction source capture or Asset cook.

### Open paths optimistically while removal/rebuild is pending

Rejected because absence of a blocker command does not prove newly walkable
topology. Opening reachability waits for a validated gate or topology generation.

### Rebuild navigation automatically from streamed render geometry

Rejected because it creates a second loader/cooker, bypasses cooked cell provenance
and makes headless/server topology depend on presentation content.

### Encode doors only as carved meshes

Rejected because ordinary door traversal is a semantic gate with stable identity,
state and cost. Repeated geometry carving adds latency and loses gameplay meaning.

### Persist or replicate provider-native carved tiles

Rejected because native data is tied to provider/build/base generations. Durable
state records semantic world changes and reconstructs derived topology.
