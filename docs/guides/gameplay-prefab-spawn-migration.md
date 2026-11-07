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

Current HPFB version3 retains at most64 sorted, nonzero `PrefabInitializationId`
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

Version1 and version2 cooked artifacts must be recooked and republished in a complete catalog
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

## Prepared group publication options

`PrefabTemplateProvider::QueuePreparedGroup` now takes `PrefabPreparedGroupOptions`
as its fourth argument instead of positional fixup, placement, receipt and
reference arguments. This keeps the owned publication metadata together and avoids
an eleven-argument call that can silently swap values. The lease, components and
operation cancellation arguments retain their order. Existing two- and three-argument
calls remain valid through the default options value.

The spawn service supplies placement, parent, receipt output, scope cancellation,
members, references and lineage with named aggregate fields. Its caller is migrated
in the same change; there is no positional compatibility overload. Receipt storage
is borrowed only for the synchronous submission call and written after successful
queueing. Component/metadata ownership and Scene commit fences remain unchanged.
The public-header consumer checks the new exact member-function signature.

`PrefabSpawnService::Acquire` and `Advance` now preserve const service routing
identity while operating on the explicitly shared Scene-owner state, just as a
const gameplay capability can queue a request. Const does not imply thread safety
or immutable runtime state. Ordinary callers are unchanged; member-function
pointers must include the const qualifier. The public consumer checks both forms.

Capability allocation uses `make_shared` with a private construction key. Neither
module code nor external callers can construct a grant or reset derived lineage;
only the admitted service and the existing derived-client factory can create the
key. Scope IDs advance after successful allocation, preserving allocation-failure
admission behavior. The consumer also verifies that the former unkeyed constructor
arguments cannot create a capability.

## Durable prefab Save and Load

Recook runtime prefabs as HPFB3 before durable spawning. The actual source cooker
now records the genuine root revision. Procedural templates without that evidence
remain usable for ephemeral spawning and reject durable capture with a typed error.
HPFB2 has no runtime fallback. Full envelope digest/target and member provenance
must match the saved creation recipe; source revision alone is insufficient.

`PrefabSpawnRequest::overrides` accepts explicit complete typed intent through the
shared runtime-safe baseline/delta implementation. Existing ordinary requests keep
their placement semantics. Initialization IDs and external binding slots remain
separate from property/component deltas. Saves store root TRS once as placement.

Install the actual Save inventory before the first structural prefab queue.
Pending unsubmitted OnCreate loads may already exist; already committed ephemeral
groups cannot acquire invented durable identities retroactively. Dynamic metadata
reserves all identities and aggregate payload before queue admission, and only the
real successful structural receipt publishes existence or tombstones.

After Load, retain the stable `SavePrefabInstanceId`, then use the current admitted
`GameplayPrefabContext::FindPersistent` to obtain a fresh ordinary handle. The
stable creator comes from host-admitted module routing, not the request or an old
process scope. A foreign creator, stale member incarnation or tombstoned occurrence
fails closed. Pre-Load contexts/handles stay revoked, and Despawn still follows the
existing real Scene transaction.

Rebuild all native Gameplay consumers against the next coordinated Gameplay SDK revision selected by the host. The
C++ request/context/persistence contracts changed; old-layout module images must
be rejected before construction. The external C ABI is unchanged. Host registration
must assign the new public headers and explicit SaveApi dependency to their exact
owning targets; broad include roots and duplicate override implementations are not
migration substitutes.

Private restore command migration also adds service-issued aggregate group admission,
command-only dense external-binding slots, and exact qualified occurrence partitions.
Ordinary public group bounds/lineage and published reference variant categories stay
the same. Only the actual Scene restore capability can admit aggregate partitions;
Scene validates the sole Bundle inventory and issues private occurrence ownership
pins, keeping independent Despawn semantics when instances share asset leases.
