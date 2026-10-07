# Typed Gameplay Prefab Spawn and Despawn

HORO-1039 / [PFB-004.3] exposes cooked runtime creation through
`GameplayPrefabContext`, owned by `HoroPrefabRuntime`. Gameplay includes
`Horo/Prefab/PrefabSpawnService.h` and links `HoroEngine::PrefabRuntime` when it
uses this capability. `Behavior.h` only forward declares the capability, so
ordinary Gameplay API consumers acquire no new public dependency. The runtime
runner privately links Prefab Runtime; Scene remains independent of Prefab.

## Host composition

Register `MakeGameplayStructuralParticipant(activeGameplay)` against an actual
application-owned pointer slot before starting `RuntimeSceneService`. Activate
the scene, then construct `PrefabTemplateProvider` over that exact service and
the published registry/loader. Select the module and independently grant
`GameplayWorldSelection::prefabPermission`. Call the service-backed
`GameplayWorldComposition::Create(scenes, templates, world, epoch, selection)`.
The provider, Scene service, optional frozen custom component registry and host
pointer slot outlive the composition. Another service with equal numeric scene
IDs is rejected.

Dispatch `FixedTick` with the host's exact `FixedStepContext`. Dispatch the
composition's `OnPhase` at the normal lifecycle safe point; it advances cooked
preparation, delegates the transaction to the actual Scene service, then observes
that transaction's dedicated receipt. The composition owns Scene phase dispatch
on this path: do not independently dispatch the same service twice. Set the host
slot only after successful composition and clear it at a drained safe point after
shutdown. The standalone Scene factory rejects a prefab grant, because a separate
mutable Scene cannot stand in for the transaction authority.

`BehaviorRuntime::Create(RuntimeSceneService&, ...)` is the corresponding runtime
integration. Every fixed callback receives its own retained module client through
`BehaviorContext::PrefabContext()`. Presentation and teardown contexts expose no
new grant. Each attachment captures immutable creation lineage once, rather than
allocating a client every tick. A retained spawned client cannot reset that
lineage; repeated template identities and a seventeenth level fail admission.
The host revokes the module scope before disable/destroy callbacks and module
unload. The service must retire before its borrowed provider/Scene authorities.

## Request and completion contract

Spawn requests copy an exact `AssetId`, canonical artifact digest, cook target,
root local placement, optional generation-qualified parent, initialization
interfaces, and separate external bindings. The placement replaces the root's
cooked local transform; descendant local transforms retain their cooked values.
Consumers receive an operation and an opaque committed group identity, never a
mutable Scene, direct entity allocator, source resolver, or authoring override.

Requests and observations use the Scene owner lane. Cancellation and scope
revocation are atomic, nonblocking operations callable from another thread. Host
command adapters route callers to that owner lane; the former illustrative
`RequestSpawnPrefab`/`SceneRuntimeAccess::SpawnPrefab` names had no implemented
callers to migrate. They do not authorize synchronous worker mutation.

`notBeforeTick` addresses the first eligible safe point. Async load completion
may occur later; work expires 64 ticks after that point. Admission rejects zero,
past, overflowing, or more than 64-ahead addresses. One complete structural
transaction is outstanding at a time, with 32 pending operations, 128 retained
observations and 32 root module scopes. Another host structural batch may reject
submission with its original typed Scene error. Loads never block or join workers.

Scene resolves every group reference after reserving the entire hierarchy and
prepares all required native owners before its final scene/registry/cancellation
check. Failure before publication discards the complete candidate and runs no
creation hooks. A receipt records committed topology before success is observed.
Post-publication factory/hook errors remain separate host operation errors; they
cannot turn an already committed group into a rollback. Handles preserve actual
committed evidence even if shutdown happens inside a publication notification.

Despawn accepts only a committed group from the exact service and module scope.
Scene validates all original generations and complete resource-group membership,
then destroys children before parents in one transaction. Foreign children,
partial groups, reused entities, stale scenes, revocation and cancellation reject
the whole retirement. Retained immutable template bytes survive provider eviction;
Scene pins exact artifact resources until the final owning group entity retires.

## Initialization and cooked format migration

HPFB version 2 adds at most 64 sorted, nonzero `PrefabInitializationId`
declarations. IDs are a distinct type from binding/property identities. The
cooker selects an existing behavior member and field occurrence, a closed value
kind, required/optional semantics, and finite inclusive numeric bounds. Targets
must be unique and defaults valid. Callers provide only the declared identity and
a copied Boolean, Integer, Number, String, Vec2, Vec3 or Quaternion value.
Strings are limited to 256 bytes; numeric/vector values must be finite and
quaternions valid. Missing optional inputs preserve the cooked default.

Initialization validates full required coverage, uniqueness, declared identity,
exact type and bounds before any Scene staging. It cannot add/remove components,
alter hierarchy, address paths, or reinterpret PFB-003 override records. Runtime
behavior attachment IDs are freshly allocated from the bounded Scene domain,
with exhaustion checked before publication; template occurrence IDs are not
reused as live attachment IDs.

Version 1 cooked artifacts must be recooked and republished in a complete catalog
generation. They return `UnsupportedCookedVersion`; there is no silent fallback,
byte reinterpretation, source parsing or in-place authoring migration. Include
the cooked format version and complete initialization declaration table in cook
keys/provenance. Existing authoring documents do not change version in this task.

Reference interfaces remain separate: local entity/member, typed asset, required
or optional external binding. Missing required and invalid supplied optional
bindings fail before mutation and again at Scene publication. Optional absence is
explicit `Unbound`. `GameplayPrefabContext::Reference` returns copied Horo values
and rechecks entity generations; target destruction produces a typed unavailable
reference failure without ownership cascade or slot retargeting.

## Regression and public-consumer targets

`HoroPrefabSpawnServiceTests` exercises actual provider loading, native behavior
construction through the service-backed runner, group publication/destruction,
typed initialization, boundaries, malformed data, rollback, cancellation,
revocation, scene replacement, references and retained lifetimes.
`HoroGameplayPhysicsTests` additionally contains the production native-module
prefab host regression. `HoroPrefabSpawnPublicHeaderConsumer` verifies the owned
header contract; the native test module is a real capability consumer.

These checks require execution; source presence is not a passing result. Required
qualification also includes the existing cooked/provider, Scene transaction,
Gameplay lifecycle, prefab Physics/AI and affected public-header consumers.
