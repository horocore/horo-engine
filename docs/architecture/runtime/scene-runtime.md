# Scene Runtime Architecture

## Purpose

This document defines the runtime scene model, ECS ownership, entity and
component identity, system scheduling, scene transitions, authoring conversion,
serialization boundaries, and runtime references.

## Core Decisions

- Editor documents and runtime scenes are separate models.
- `RuntimeSceneDefinition` is the immutable handoff into runtime construction.
- One scene runtime owns its registry, systems, physics bindings, and
  scene-scoped resources.
- Entities use generation-checked runtime identities.
- Structural ECS changes occur at explicit synchronization points.
- System order is declared and validated; it is not determined by event order.
- Persistent files store stable logical IDs, never runtime addresses or handles.
- Scene transitions are transactional and generation-aware.

## Implemented SCN-001 Baseline

The implemented SCN-001 baseline is the backend-neutral
`HoroEngine::RuntimeScene` target. With AST-001B it depends on Foundation,
Runtime, and the backend-neutral Assets API; it does not depend on editor
documents, renderer backends, physics, or GUI types.

The baseline deliberately stops short of a complete ECS. It provides:

- immutable, validated `RuntimeSceneDefinition` values
- owner-thread `RuntimeScene` storage and allocation-free borrowed views
- `EntityRef { SceneRuntimeId, EntityId }` stale-reference validation
- transactional activation, replacement, unload, and structural command batches
- deferred `Create(RuntimeEntityCreateInfo)` and `Destroy(EntityRef)`
- editor preview extraction from the active runtime scene.

Every create command supplies the complete initial transform, optional existing
parent, optional authored identity, primitive descriptor, and initial typed
component set. Component add/remove, reparent, system scheduling, and parallel
iteration remain future ECS work.

Scene activation and structural batches commit only during
`CommitDeferredLifecycleChanges`. A candidate is prepared without touching the
active scene; a failed preparation or structural batch preserves the active
scene. Only one preparation, transition, or structural batch may be pending at
a time.

`RuntimeScene` is neither copyable nor movable. One published runtime identity
therefore has exactly one owner. Structural slots, the free list, and authored
lookup index live in private `RuntimeSceneStorage`. A structural transaction
copies only this identity-free storage into a private candidate, validates the
whole command batch, and publishes it with one owner-thread swap. The candidate
cannot escape through the public API and never carries `SceneRuntimeId`.

Each published storage has a monotonically increasing structural revision.
`RuntimeSceneView` captures that revision; a successful non-empty mutation makes
older views stale, while an empty batch or failed mutation preserves both the
revision and existing views. Scene replacement creates a new runtime identity,
so every `EntityRef` from the replaced scene is rejected even if slot and
generation values coincide.

Entity generations start at one. Destroyed slots are reused in deterministic
LIFO order after incrementing the generation. A slot at the configured maximum
generation is retired permanently; generations never wrap and therefore cannot
make an ancient reference valid again.

The steady-state path with no pending transition or structural batch performs no
scene-owned heap allocation. Definition building, candidate preparation,
activation, and structural mutation are load/mutation paths and may allocate.

## Implemented AST-001B Asset Resolution

`RuntimeSceneDefinition` carries a canonical AssetId-sorted set of required
`SceneAssetDependency { AssetId, expected AssetTypeId }` values.
`SceneDefinitionBuilder::RequireAsset` deduplicates identical requirements and
rejects one identity requested with conflicting types. The definition is an
in-memory typed handoff, not a serialized schema, and therefore carries no
format version or migration policy.

`RuntimeSceneService::QueuePreparation` is the single activation admission path.
The former public prepare/candidate activation pair does not exist. Assetless
definitions remain valid for editor preview and headless use. Asset-bearing
definitions require an injected `AssetRegistry` and `AssetLoadService`.

Preparation pins one immutable registry snapshot, validates every required ID
and expected type before I/O, and submits bounded asynchronous loads. The
default limits are 1,024 dependencies, eight concurrent requests, and one GiB
of logical resident bytes per candidate. Limits are explicit and configurable.
The byte budget includes reused and newly loaded payloads exactly once per
candidate. It is not a process-global memory budget; old and replacement scenes
may temporarily coexist at the lifecycle boundary.

Budget accounting is incremental. After each reused payload or completed load,
the running total is checked before more work is admitted. An over-budget,
empty, missing, mistyped, cancelled, or provider-failed dependency cancels the
remaining preparation and preserves the active scene. Provider errors keep
their original error code and gain scene/asset diagnostic context.

Published registry state is immutable. Workers use only the `AssetRecord` and
revision captured by `AssetLoadService`; they never query mutable registry
state. At the owner-thread safe point, the service compares the captured
revision with the authoritative registry revision. A stale candidate is
dropped, even when every worker completed successfully.

An active scene pins immutable payload leases. A replacement at the same
registry revision reuses each matching AssetId/type payload independently;
dependency input ordering has no meaning. Reused bytes still count toward the
candidate logical budget, but shared physical storage is not duplicated.
`RuntimeSceneView::FindAsset` provides allocation-free typed ID/type/byte access.

`QueuePreparation`, `QueueUnload`, structural commands, lifecycle phases, and
`Shutdown` are owner-thread operations. Cross-thread callers require a future
host command queue. A second operation is deliberately reject-and-retry with
`scene.operation.in_progress`; AST-001B does not add an unbounded transition
queue. Unload requests cooperative cancellation and drops the candidate without
waiting on the owner thread; shutdown cancels and joins owned preparation
handles before service destruction. Runtime identities are monotonically
assigned only after a complete candidate is built; failed preparations do not
consume an identity.

## Model Boundary

```text
SceneDocument
    |
    | validate and convert
    v
RuntimeSceneDefinition
    |
    | instantiate
    v
SceneRuntime
    +-- Registry
    +-- Systems
    +-- PhysicsWorld
    +-- ResourceLeases
```

`SceneDocument` may contain editor metadata, incomplete drafts, selection hints,
and authoring conveniences. Runtime modules do not parse it directly.

`RuntimeSceneDefinition` contains validated, typed, backend-neutral data needed
to instantiate a runtime scene.

## Physics Shape Boundary

[ADR-085](../../adr/085-physics-shape-authoring-cook-and-runtime-boundary.md)
requires scene conversion to resolve every collider to an exact published Physics
shape artifact and stable material/subshape bindings before activation. Runtime
scene data carries Horo identities, expected artifact digests and body-motion
intent, never source paths, raw geometry or solver-native handles.

Scene preparation acquires immutable shape leases. Activation fails atomically
when a required artifact is absent, incompatible or exceeds limits; Runtime does
not import, cook or invent a fallback collider. Reload prepares candidate leases
off to the side and commits body shape swaps only at the Physics pre-step safe
point. Retired leases remain alive until scene, query and in-flight Physics readers
have drained.

[ADR-086](../../adr/086-collision-layer-profile-and-query-channel-policy.md)
also requires scene conversion to resolve each collider's stable project
`CollisionProfileId` against the exact locked collision-schema fingerprint.
Candidate preparation acquires one immutable filter-schema generation and compiles
private runtime/native tables; scene data never stores display names, bit positions
or solver object layers. Missing or stale profile/channel IDs block activation
rather than using the project's authoring default.

[ADR-087](../../adr/087-scene-to-physics-ownership-and-conversion.md) defines the
owning conversion and activation transaction. Scene Model carries explicit typed
rigid-body, collider and constraint intent; Physics alone validates semantics and
builds a canonical `PhysicsScenePlan` plus detached world candidate. A collider
never infers a body from hierarchy/render data, and a constraint never binds a
runtime entity handle or creates a missing endpoint.

The host injects Physics through the generic scene activation-participant seam;
RuntimeScene does not interpret solver semantics or discover services. The one
`RuntimeSceneService::QueuePreparation` candidate owns detached ECS storage and
all participant candidates. At `CommitDeferredLifecycleChanges`, ECS, resource
leases, Physics world and the private generation-scoped binding table publish as a
single no-fail bundle. Any earlier failure/cancellation destroys only candidate
state, consumes no public identities and preserves the prior active scene/world.

[ADR-144](../../adr/144-destruction-ownership-authority-state-and-runtime-geometry-boundary.md)
uses the same aggregate seam for destruction. Scene data carries only stable
destructible/fracture-asset/policy binding intent. The scene-scoped Destruction owner
commits canonical health, semantic phase and chunk/support membership; Physics and
Render prepare pre-cooked chunk representations privately. RuntimeScene exposes the
new semantic revision, entity/component set, intact/chunk visibility and required
bodies only through one complete activation root. Core 1.0 never generates missing
mesh topology or collision during scene activation.

[ADR-146](../../adr/146-destruction-runtime-activation-physics-cleanup-and-rollback.md)
specializes that seam for intact-to-chunk transitions and cleanup. One transition ticket
closes the exact semantic snapshot, ECS batch, Physics/Render receipts, leases and peak
reservation. Participant-private publication remains unreachable through ordinary
Scene/Physics/Render queries; `CommitDeferredLifecycleChanges` is the sole public
visibility and rollback boundary. Pre-commit failure preserves the old root, while
post-commit restore, dormancy or recovery requires a new revisioned aggregate
transaction. RuntimeScene never infers fracture from a body or rewinds native handles.

## Navigation Authoring And Runtime Boundary

[ADR-105](../../adr/105-navigation-asset-and-scene-ownership-boundary.md)
requires `SceneDocument` to store typed navigation intent only. Stable Scene
object/component/contribution IDs identify explicit surface, geometry-source,
modifier, grounded-link and dynamic-obstacle declarations that reference a tracked
`NavigationDefinition` AssetId. Components never serialize generated triangles,
polygons, adjacency, tiles, provider refs/blobs or runtime topology handles.

The application bake operation captures one immutable, revision-consistent view of
the Scene intent and exact accepted geometry artifacts. That
`NavigationBakeInputSnapshot` is derived operation state, not a Scene revision or
asset. Asset Pipeline publishes its validated `GroundedNavMeshArtifact`; generated
data never modifies document dirty state, undo history or source serialization.

Scene conversion resolves each required definition/profile/scope to an exact
published artifact identity and expected digest. Navigation prepares a detached
generation-scoped topology candidate through the activation-participant seam. The
candidate publishes atomically with the Scene; missing, stale, corrupt or
unsupported required navigation rejects activation and preserves the old Scene.
An explicitly optional capability may activate with typed `NoNavigationData`, but
neither required nor optional policy authorizes runtime bake, source I/O or a fake
path. Dynamic obstacle/carving state is runtime-owned transient overlay state and
cannot write back into the Scene or cooked base.

[ADR-107](../../adr/107-navigation-query-consistency-and-snapshot-ownership.md)
binds one `NavigationWorldId` and combined query-snapshot root to the exact active
`SceneRuntimeId`. Scene unload/replacement closes old navigation admission and
requests cancellation before commit, then publishes a never-reused world
incarnation with the new Scene. Old read leases may keep provider bytes safe while
workers finish, but their results are `InvalidWorld`/cancelled and cannot apply to
the replacement. `CommitDeferredLifecycleChanges` makes prepared Scene/cell
topology visible before the following owner-thread `NavIntentCommit`; workers never
publish into Scene components.

## Runtime UI Scope Boundary

[ADR-073](../../adr/073-runtime-ui-ownership-scope-and-update-order.md) makes
`RuntimeUiService` an application/game-runtime service rather than a SceneRuntime
subsystem. A scene may declare scene-scoped UI documents and stable scene/object/
component bindings, but the resulting UI elements are not ECS entities and the UI
tree does not live in component pools.

Scene UI preparation may accompany a scene candidate. Activation waits for the
exact `SceneRuntimeId` generation and publishes before that scene's first eligible
Runtime UI VariableUpdate/extraction. Bindings and world anchors are immutable
generation-tagged snapshots; UI never retains component-pool pointers or performs
world queries during layout/rendering.

[ADR-079](../../adr/079-runtime-ui-binding-provider-schema-identity-and-lifetime.md)
requires each scene binding provider instance to name the exact `SceneRuntimeId`
generation. It activates with the scene/UI barrier, publishes immutable typed
snapshots, and enters revocation before scene storage teardown. Unload closes new
reads/writes, cancels pending commands and drains old UI/snapshot/callback leases;
late provider work cannot publish into a replacement scene generation.

Scene unload closes its UI scope and input admission, cancels/joins pending UI
work, publishes removal for attached views, waits for UI/render/resource leases,
then releases scene UI state. Game-instance and persistent player UI survive scene
replacement; viewport replacement does not transfer or destroy foreign semantic
ownership. Late completions and stale references fail by generation.

## Runtime Identity

```cpp
struct EntityId {
    uint32_t index;
    uint32_t generation;
};
```

Runtime entity IDs are valid only within their owning `SceneRuntimeId`.
Cross-scene references include both identities or use stable logical scene
object IDs resolved during instantiation.

Serialization uses stable IDs:

```text
scene_id
object_id
component_id where required
asset_id
```

Generation counters prevent stale entity references from aliasing newly created
entities.

## Registry Ownership

The scene runtime owns:

- entity allocation and destruction
- component pools
- structural command buffer
- registered systems and execution schedule
- scene-local event queues
- runtime reference resolution

Component pools own component values. A component must not delete a resource
owned by physics, renderer, assets, or another service. Such relationships use
typed handles or explicit leases.

## Component Contract

Components are typed state carriers. They:

- have clear copy/move behavior
- avoid hidden thread synchronization
- do not invoke GUI or transport services
- do not own system scheduling
- declare authoring-schema and runtime persistence participation metadata
- use stable asset and entity references

Components do not self-serialize arbitrary memory for runtime saves. Under
[ADR-114](../../adr/114-canonical-runtime-world-persistence-boundary.md), exactly one
subsystem-owned canonical adapter owns each durable semantic field. Reflection and
component storage support authoring/runtime access but do not grant persistence
authority.

Polymorphic gameplay behavior is owned through explicit behavior components
registered by the gameplay module boundary, not hidden in arbitrary component
constructors or destructors. A behavior component stores stable behavior type ID
and serialized fields; runtime behavior instances are created during scene
activation and are driven by declared scene phases.

## Structural Changes

The SCN-001 baseline records entity creation and destruction in an owner-thread command
buffer:

```cpp
class SceneCommandBuffer {
public:
    DeferredEntity Create(RuntimeEntityCreateInfo initialState);
    void Destroy(EntityRef entity);
};
```

The runtime applies the full batch transactionally at
`CommitDeferredLifecycleChanges`. A create token resolves to an `EntityRef`
only through a successful `StructuralCommitResult`. References to another
deferred entity in the same batch are not supported in the baseline.

Component add/remove, reparent, and direct system-owned component writes require
the later ECS access/scheduling contract. They are not silently emulated by the
baseline storage API.

## System Contract

```cpp
struct SystemDescriptor {
    SystemId id;
    SystemPhase phase;
    ComponentAccessSet reads;
    ComponentAccessSet writes;
    std::vector<SystemId> after;
    std::vector<SystemId> before;
};
```

Phases include:

- `PrePhysics`
- `Physics`
- `PostPhysics`
- `Gameplay`
- `Presentation`
- `RenderExtraction`

The scheduler validates cycles, duplicate ownership, and incompatible parallel
access. A stable order is produced for equal dependencies.

Initial implementations may execute systems serially. The access contract still
exists so future parallelism does not require redesigning system ownership.

Within a fixed tick,
[ADR-061](../../adr/061-animation-ownership-update-order-and-clock.md) requires
gameplay parameter commit before animation pre-physics evaluation, root-motion
request admission before Character platform/query movement, Physics next,
Character support/transform finalization without a second move, and typed
post-Physics pose override/finalization before tick publication. ADR-089 owns the
Character-specific details. These are dependencies within the existing phase graph,
not new `RuntimePhase` values.

## Runtime Save And Restore Integration

[Runtime persistence](./save-game-and-persistence.md) is coordinated by an
application/session-owned service, not a service retaining a replaceable scene
reference. Capture hands workers an owned immutable, revision-consistent snapshot;
live ECS pools are not held frozen after the lifecycle safe point. ADR-114 assigns the
core Scene adapter only persistent entity existence, authored/spawn identity,
tombstones, hierarchy/reference remaps and explicitly owned core fields. Gameplay,
Physics/Character and Persistent World adapters retain their own canonical semantics;
Scene never reflects over their component memory.

Slot restore resolves a compatible cooked authoring base, composes stable-ID keyed
overrides/spawns/tombstones and prepares a private bundle of Scene, gameplay, slot
player and persistent world state through the existing QueuePreparation admission
seam. It never edits SceneDocument or cooked assets. CommitDeferredLifecycleChanges
publishes all prepared roots without fallible work or intermediate observers. Its
scene candidate cannot auto-activate while the composite bundle gate is pending;
account settings/achievements remain outside that transaction. This composite commit
is an implementation requirement beyond SCN-001, not a second public activation path.
Old scene/participant resources retire asynchronously with their leases preserved.

[ADR-092](../../adr/092-character-controller-determinism-and-state-composition.md)
requires the Character provider snapshot to share the exact committed tick, scene/
structural revision, origin, determinism fingerprint and paired Physics checkpoint.
Restore resolves all stable support/controller bindings inside the detached aggregate
candidate; a standalone, partial or mixed-generation Character restore cannot enter
the publication gate.

## Runtime Scene Definition

The definition contains:

- schema version and scene identity
- entity definitions and stable object IDs
- typed component payloads
- asset dependencies
- primitive descriptors for built-in procedural meshes and shapes
- scene references
- required engine capabilities
- initial settings and spawn data

Conversion resolves authoring defaults, primitive descriptors, and imported
asset references. It emits diagnostics. Instantiation does not accept arbitrary
string property bags for built-in component behavior.

Prefab references placed in the authoring document are expanded before the
runtime scene is built. Runtime modules see only the expanded objects, never raw
prefab paths or mutable Prefab state. Conversion is a pure bounded transformation
over immutable scene-document and resolver snapshots. It stages every authored and
expanded entity in one detached builder and publishes only after all required
placements resolve; missing, corrupt, cyclic, conflicting, stale or over-budget
content rejects the complete candidate and leaves the active definition unchanged.
The editor may retain a repairable broken-instance projection keyed by the authored
`ScenePrefabInstance`, but that projection is not runtime-valid. See [Prefab
Architecture](./prefab-architecture.md).

The current backend-neutral `RuntimeSceneDefinition` accepts the typed runtime component
set and behavior data only. Expansion still preserves opaque prefab payloads in the
authoring candidate, but conversion rejects a required instance containing a payload with
no registered runtime projection; it must remain repairable in the editor rather than be
silently dropped from the runtime definition.

[ADR-096](../../adr/096-prefab-external-reference-and-binding-slot-contract.md)
requires prefab-local references to resolve inside the complete expanded candidate
and forbids prefab source from retaining scene IDs, `EntityRef`, pointers, paths or
name queries to external scene objects. Static prefab instance bindings live in the
containing `SceneDocument`, keyed by stable typed slot ID and stable scene target.
Conversion resolves required/optional bindings against one immutable document
snapshot before publishing `RuntimeSceneDefinition`.

Runtime binding tables contain scene-qualified generation-checked references and
do not own target lifetime. A missing required, duplicate, stale or incompatible
binding rejects activation; omitted optional bindings become typed `Unbound`.
Target destruction after activation makes access unavailable without retargeting a
reused entity slot. Scene reload rebuilds the table as part of its detached
candidate and never carries raw addresses from the previous runtime generation.

## Load State Machine

```text
Unloaded
  -> Preparing
  -> Activating
  -> Active
  -> Unloading
  -> Unloaded

Active -> PreparingReload -> ActivatingReload -> Active
```

Preparation may validate, load assets, and build CPU-side state on workers.
Activation occurs on the runtime owner thread and either commits a complete
scene or leaves the previous state active.

## Transactional Activation

Loading a replacement scene builds a candidate runtime. It becomes active only
after:

- definition validation succeeds
- required assets and capabilities are available
- any requested Physics determinism tier/fingerprint/evidence is qualified under
  [ADR-088](../../adr/088-physics-determinism-capability-and-support-tiers.md)
- systems initialize successfully
- physics and render bindings are ready
- startup hooks succeed

Failure destroys the candidate and preserves the previous active scene where
the operation contract allows it.

## Reload

Reload strategy is explicit per state category:

| State | Default behavior |
|---|---|
| Definition-owned component data | Replace from new definition |
| Runtime transient state | Recreate |
| Explicitly preservable state | Transfer through typed policy |
| Asset handles | Re-resolve or retain valid lease |
| Physics state | Rebuild unless preservation contract exists |

Reload does not copy arbitrary component memory by type name. Preservation uses
typed adapters and stable object IDs. Development hot-reload/restart preservation is
not automatically a durable ADR-114 participant or save-schema promise.

## Scene References

Scene references are logical and cycle-validated. The runtime defines whether a
reference is:

- embedded
- streamed
- activated as a subscene
- used only as a spawn/template source

Reference loading participates in the parent task group and cancellation tree.
An unloaded parent cannot receive a late child activation.

## Editor Interaction

The editor session owns `SceneDocument`, history, selection, and dirty state.
Document commands produce a new document revision. Runtime preview or play
sessions consume converted definitions.

Runtime notifications may update editor presentation through the explicit
engine-to-editor bridge. They do not mutate the authoring document unless an
editor command explicitly applies a change.

In the SCN-001 baseline each committed editor document revision is converted to a new
definition and fully reactivated. Gizmo drag state is an editor-owned render
overlay; cancel removes the overlay and commit first updates the document. The
new transform becomes authoritative only when the replacement scene reaches the
lifecycle commit boundary, so the viewport observes it on the following frame.

Render extraction and picking finish against the old active scene before a
replacement commit. Picking results carry their source `SceneRuntimeId` and are
discarded when that ID no longer matches the active runtime. Editor selection is
owned as logical `SceneObjectId`: it resolves to the replacement scene when the
object remains and is cleared when the authored object was deleted. Editor state
does not retain an old runtime's `EntityRef`.

## Replication Ownership Boundary

[ADR-099](../../adr/099-replication-ownership-authority-and-compatibility.md)
keeps canonical component/gameplay values and mutation safe points in Scene and
their declaring systems. `NetworkRuntime` may invoke a registered capture adapter
with a validated read-only owner-thread view at the network capture safe point; it
never scans component memory or infers wire fields from ECS layout.

[ADR-175](../../adr/175-replication-dirty-state-capture-and-copy-strategy.md)
requires that safe point to follow the complete simulation and structural commit.
NetworkRuntime copies each admitted declared state once into a private bounded candidate
and publishes it only after complete validation. Dirty notifications are lossy,
idempotent scheduling hints; they carry no values and cannot replace canonical owner
state, revision reconciliation or comparison with the prior immutable snapshot.

Received replication is a bounded typed apply command carrying scene/entity,
authority, object and schema generations. Scene revalidates them at the owner safe
point and invokes the declaring owner's apply adapter. The adapter commits one
valid mutation or fails without partial visibility; NetworkRuntime cannot write a
component pool directly. Scene reload/destruction changes generation/authority
epoch so late records cannot target reused entity slots.

Standalone, authority-server, autonomous-client and simulated-client roles are
explicit host/world capabilities. Entity locality, possession, input device and
same-process listen-server composition never grant Scene write authority.

## Data Bus Relationship

System-to-system behavior inside one deterministic tick uses direct scheduling,
typed queues, or declared runtime services. It does not depend on unordered
process event delivery.

After committed lifecycle changes, the runtime may publish:

- `SceneLoadedEvent`
- `SceneReloadedEvent`
- `SceneUnloadedEvent`
- `SceneRuntimeFailedEvent`

Payloads remain bounded and identify the authoritative runtime state to query.

## Serialization

Authoring serialization, cooked runtime scene serialization, and in-memory ECS
layout are separate formats.

Persisted authored scenes and cooked runtime scenes will each own an independent
semantic format version `{ major, minor, patch }`. Major changes are
incompatible, minor changes require an explicit compatible-reader or migration
rule, and patch changes do not alter the serialized shape. The in-memory
`RuntimeSceneDefinition` is deliberately not assigned that version; conversion
produces the current typed contract after the owning serialized format has been
validated or migrated.

Runtime binary formats are:

- versioned
- bounds checked
- endian and alignment explicit
- validated before allocation
- independent from C++ object memory layout

## Testing

Required tests cover:

- stale entity generation rejection
- component ownership and destruction
- deferred structural changes during iteration
- deterministic system order and cycle rejection
- document-to-runtime conversion diagnostics
- transactional load and failed activation rollback
- reload preservation policies
- nested reference cancellation and cycle detection
- scene unload with jobs, physics, and render leases
- binary format corruption and version behavior
- destruction binding conversion and aggregate semantic/entity/render/Physics chunk
  publication with no runtime geometry fallback
- destruction transition-ticket isolation, pre-commit rollback, post-commit compensating
  transactions and dormancy cleanup with no partial query visibility

## Related Documents

- [Runtime Lifecycle](./runtime-lifecycle.md)
- [Built-In Scene Primitives](./built-in-scene-primitives.md)
- [Project Model](../editor/project-model.md)
- [Asset Pipeline](./asset-pipeline.md)
- [Prefab Architecture](./prefab-architecture.md)
- [Physics Architecture](./physics-architecture.md)
- [Rendering Architecture](./rendering-architecture.md)
- [Editor Document Model](../editor/editor-document-model.md)
- [Save Game And Persistence](./save-game-and-persistence.md)
- [Destruction And Fracture](./destruction-and-fracture-architecture.md)
- [ADR-114](../../adr/114-canonical-runtime-world-persistence-boundary.md): canonical
  state classification, Scene base/override composition and adapter ownership.
