# Gameplay Runtime Integration

## Purpose

This document defines how project-owned gameplay modules integrate with runtime systems: game-owned assets, input actions, scheduled systems, scene/play lifecycle, game-owned component persistence, and deferred runtime extension points.

## Game-Owned Asset Types

Game modules may register asset types for source assets owned by the project:

```cpp
struct GameAssetTypeDescriptor {
    GameAssetTypeId typeId;
    uint32_t schemaVersion;
    std::vector<std::string> sourceExtensions;
    std::vector<AssetCookTargetId> cookTargets;
    GameAssetEditorRepresentation editor;
};

struct GameAssetHandlerBinding {
    void* userData;
    ImportCallback importAsset;
    SerializeCallback serializeAsset;
    CookCallback cookAsset;
};
```

The gameplay boundary validates and freezes these exact-generation callbacks
without adding a dependency from the engine asset core to project code. Authored
bytes use the host-owned `SerializedGameAsset` envelope described by
[Gameplay Module Boundary](./gameplay-module-boundary.md). Import and serialization
must return the registered type and current schema. Cook accepts only a current
envelope and declared target. Game importers and cookers are deterministic
functions over their bounded inputs and do not mutate scenes or editor state.

Editor asset browsers use the descriptor's authoring metadata to show
game-owned asset types, icons, validation diagnostics, and typed fields.
When code is missing or schema-skewed, the generic editor projection is read-only
and retains the opaque bytes, stable type identity, schema version, and payload
size. Missing-code presentation is a semantic fallback resolved by the editor
host through `workspace.game_asset.category.missing`; no gameplay-facing model
contains hard-coded host copy. Inspection descriptors and editor field lists are
owned snapshots that remain valid after registry replacement or project close.
The projection does not synthesize a replacement payload or silently select
another handler.
Runtime code accesses loaded assets through `AssetAccess` handles or leases, not
raw file paths. Asset loads may be asynchronous through the asset system's task
contract; gameplay jobs request work through approved asset APIs instead of
spawning ad hoc import or load threads.

Hot reload of game-owned assets follows the Asset Pipeline contract. Stable
logical asset IDs survive recook and reload. Runtime asset handles either retain
a valid lease to the old asset until a synchronization point or re-resolve to the
new revision through a typed invalidation event. A module unload invalidates
runtime loaders and callbacks owned by that module before the dynamic library is
unloaded.

## Input Actions

Game modules register semantic input actions and default bindings through
`InputActionRegistry`. Bindings are data, not hardcoded polling:

```cpp
struct ActionDescriptor {
    ActionId id;
    ActionValueType valueType;
    InputContextId context;
    std::span<const InputBinding> defaultBindings;
    InputConsumptionPolicy consumption;
};
```

The input system resolves device bindings, context priority, conflict
diagnostics, and rebinding UI from descriptors. Duplicate action IDs fail
registration. Binding conflicts are reported through the input configuration
validator; the gameplay module does not resolve conflicts by observing raw
device state.

Behaviors consume input from an immutable, tick-assigned gameplay input view in
`BehaviorContext`. Fixed-step behavior sees the input frame assigned to that
simulation tick. Presentation update may read presentation-safe held values for
camera or visual response, but edge-triggered gameplay actions are consumed only
through fixed-step policy. Behavior code does not read live platform input or GUI
input directly.

`InputConsumptionPolicy` declares how transitions behave when one input frame
maps to several simulation ticks or when gameplay contexts overlap:

- `PerAssignedTick`: a transition is visible only to the fixed tick it was
  assigned to
- `BufferedUntilConsumed`: a transition remains available until one eligible
  consumer consumes it or a declared timeout expires
- `HeldOnly`: only current held value is exposed; no edge transition is emitted
- `BroadcastReadOnly`: multiple consumers may read the value but none consumes it

Context priority is resolved before gameplay receives an input frame. GUI,
editor tool, modal, and gameplay contexts do not race for the same raw device
transition. If two gameplay consumers observe the same action, behavior depends
on the action's policy and declared gameplay routing; it is not determined by
callback order.

## Gameplay Systems

Game systems follow [Scene Runtime](../runtime/scene-runtime.md):

- declare phase and component access
- use fixed update for deterministic simulation
- use presentation phases for interpolation and visual state
- buffer structural ECS changes
- do not depend on data-bus event ordering inside one tick

Systems may publish committed low-frequency lifecycle notifications through
approved process event types. Per-entity or per-contact tick traffic remains in
scene-owned buffers and direct runtime APIs.

System scheduling is descriptor-driven:

```cpp
struct GameplaySystemDescriptor {
    GameplaySystemId id;
    GameplaySystemPhase phase;
    GameplayThreadAffinity affinity;
    GameplayComponentAccessSet access;
    std::vector<GameplaySystemId> after;
    std::vector<GameplaySystemId> before;
    std::vector<GameplayServiceId> requiredServices;
    std::vector<GameplayCapabilityId> requiredCapabilities;
};
```

Runtime phases are shared with Scene Runtime:

- `PrePhysics`: deterministic fixed-step preparation before physics
- `Physics`: physics-owned fixed-step simulation
- `PostPhysics`: deterministic fixed-step reads of physics results
- `Gameplay`: deterministic fixed-step gameplay simulation
- `Presentation`: variable-rate interpolation and visual-only state
- `RenderExtraction`: variable-rate render snapshot extraction

Fixed-step phases must not depend on render frame rate. Presentation and
render-extraction phases must not mutate simulation-authoritative state.
`reads` and `writes` declare component access for validation, ordering, and
future parallel execution. Structural entity/component changes are buffered and
committed only at synchronization points owned by Scene Runtime.

## System Instance Ownership

System registration binds a descriptor to module-owned logic:

```cpp
struct GameplaySystemRegistration {
    GameplaySystemDescriptor descriptor;
    GameplaySystemFactoryBinding factory;
};
```

Runtime-created system instances are scene-generation scoped. The host owns the runtime
handle that schedules a system, but the module owns the code and any
module-allocated state behind that system. All module-owned system instances,
callbacks, queued continuations, and jobs are destroyed or invalidated before
`Stop()` returns and before the dynamic library is unloaded.

Every behavior and system runtime created from a native registry acquires an
exact-generation lease. The lease pins the module implementation, registries,
factory functions, and dynamic library even if the public loaded-module wrapper
is released. Native reload retirement is restart-required while any such external
runtime remains alive; normal play-session quiescence destroys its runtime before
asking the module to prepare for unload.

System callbacks are invalid after module stop. A scene cannot keep a callable,
vtable pointer, function pointer, or type-erased deleter that points into an
unloaded game module.

## Scene And Play Lifecycle

Runtime scene activation creates system and behavior instances after all
required descriptors, assets, and services are available. Scene unload calls
behavior disable/destroy hooks and destroys scene-scoped systems before releasing
component pools and module-owned scene resources. The host guarantees this order
for normal scene unload, play-session stop, failed reload unwind, and module
stop.

Scene transitions are requested through scene/runtime use cases exposed by
`SceneRuntimeAccess`, not by directly replacing host state. A behavior may
request a transition, spawn, or additive scene operation only through the
declared command or request API. The runtime decides when the request commits and
which active scene, if any, remains valid during failure.

Additive scenes are separate `SceneRuntime` instances with distinct
`SceneRuntimeId` values, registries, behavior instances, and scene-scoped
services. A registered gameplay system is scoped to one runtime scene generation.
Cross-scene references use stable logical IDs or explicit runtime reference
handles and must be resolved before use:

```cpp
struct RuntimeEntityRef {
    SceneRuntimeId scene;
    EntityId entity;
};

Result<EntityId> ResolveEntityRef(SceneRuntimeAccess&, RuntimeEntityRef);
```

Resolution is generation-checked and scene-aware. If the target scene unloaded,
the entity generation no longer matches, or the reference is outside the
caller's allowed scene set, resolution returns a typed error. Behavior code may
cache `RuntimeEntityRef`, but must not cache a resolved `EntityId` across scene
unload or scene replacement.

Play-in-editor creates a runtime scene from a converted snapshot of the
authoring document. Runtime behavior mutates only the play-session scene, not
the authoring `SceneDocument`. Stopping play runs behavior shutdown, destroys the
runtime scene, releases module-owned play-session resources, and leaves the
authoring document at its pre-play revision unless an explicit editor command
imports a runtime result. `OnDisable()` and `OnDestroy()` are guaranteed during
normal PIE stop and other controlled scene shutdown paths. Forced process
termination, crash, or OS kill provides no gameplay callback guarantee; the OS
reclaims process resources. Host-level emergency shutdown still invalidates
module callbacks and releases owned host resources where the platform permits,
but behavior authors must not rely on `OnDestroy()` for durable persistence.

Behavior callback exceptions are contained at the runtime boundary. A throwing
`OnCreate()` still schedules `OnDestroy()` before the factory instance is released;
a throwing `OnEnable()` schedules both `OnDisable()` and `OnDestroy()`. Each cleanup
callback is attempted independently, the factory destroy binding always runs, and
activation returns a typed factory failure when activation or cleanup throws.

The editor play-session controller uses the following explicit state machine:

```text
Idle -> Starting -> Playing <-> Paused -> Stopping -> Idle
                       |
                     Failed
```

Play focuses a persistent Game document tab and renders the first valid active
runtime camera. Pause stops fixed simulation while presentation remains active.
Step consumes exactly one fixed tick while paused. Stop completes behavior
disable/destroy and runtime-scene destruction before restoring the previous
authoring document tab. A missing camera, stale prepared preview, unavailable
behavior descriptor, or gameplay source diagnostic prevents activation and is
presented in the Game tab.

## Game-Owned Components

Each serializable game component has:

- stable type ID
- schema version
- typed authoring/runtime representation
- validation
- conversion or construction adapter
- deterministic serialization
- explicit upgrade support for persistent content formats

C++ type names, RTTI names, addresses, and compiler-specific layout are not
persistent identity.

Save-game serialization is not defined by this document. When a save system
persists game-owned component or service state, it must use stable component
IDs, schema versions, and explicit upgrade paths rather than C++ layout
identity.

Game modules may later participate in a save system through explicit save
descriptors or hooks. That contract is intentionally separate from scene
authoring serialization. `BehaviorComponent.fields` are authoring/default data;
runtime behavior instance state is included in a save snapshot only if the
behavior or service declares a stable save payload and schema version. Save data
uses the same stable component/behavior IDs and upgrade rules as persistent
content, but it is not allowed to serialize raw C++ object memory, function
pointers, entity runtime addresses, or module allocator ownership.

## Replication Registration

Native gameplay modules contribute replication through the host-owned
`ReplicationRegistrationRegistry` in their open `GameRegistrationContext`.
Each contribution names an already registered component, generated behavior, or
gameplay service; carries one Network-owned schema and exact typed serializers;
and declares its owner-thread capture/apply phases plus component access. This is
an integration over the canonical component, behavior, and service registries,
not a second gameplay type registry.

Freeze resolves every owner and accessed component, rejects presentation/render
capture, non-owner affinity, foreign module schemas, duplicate schema ownership,
and incomplete serializer coverage, then publishes one immutable Network schema
and serializer generation. Consumers acquire a `GameplayReplicationLease`.
That lease pins the exact native module generation, its adapters, descriptor
snapshot, and code image; reload closes lease admission and becomes
restart-required while any prior replication lease remains alive.

Gameplay modules include only the backend-neutral replication declarations from
`Horo::Network`. They never depend on transport, socket, session backend, or host
headers. Behavior code does not infer network authority from local process state
or mutate replicated state outside declared simulation phases.

## Deferred Runtime Extension Points

Save-game, memory budgeting, and advanced platform integration are separate
runtime contracts, but gameplay module contracts must leave room for them.

Memory budget participation is required for production and console targets.
Development-only targets may run without enforced budgets, but allocations must
still be attributable when tracking is enabled. The concrete allocator and
budget API belongs to a future memory-system contract and must align with
[Ownership And Resource Lifetime](../foundation/ownership-and-resource-lifetime.md)
and [Observability Performance](../observability/observability-performance.md).
This document requires that module-owned long-lived allocations, asset caches,
behavior instances, services, and job scratch buffers are attributable to a
project/module category. A module may use a custom allocator only when it
reports budget ownership and obeys the binary-boundary allocation rules above.

## Related Documents

- [Gameplay Module Overview](./gameplay-module.md)
- [Gameplay Module Boundary](./gameplay-module-boundary.md)
- [Gameplay Behavior Authoring](./gameplay-behavior-authoring.md)
- [Scene Runtime](../runtime/scene-runtime.md)
- [Runtime Lifecycle](../runtime/runtime-lifecycle.md)
- [Asset Pipeline](../runtime/asset-pipeline.md)
- [Input Architecture](../runtime/input-architecture.md)
- [Horo Package System](../packages/package-system.md): library-provided services and game-owned asset types
