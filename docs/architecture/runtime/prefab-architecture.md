# Prefab Architecture

## Purpose

This document defines Horo Engine's prefab system: how reusable entity hierarchies
are authored, validated, and versioned as project assets, how prefab instances are
placed and expanded in scenes, and how cooked prefabs are dynamically spawned at
runtime by Gameplay and SceneRuntime systems.

Runtime UI templates are a separate typed element/property domain governed by
[ADR-083](../../adr/083-ui-template-identity-schema-and-expansion.md). They do not
reuse prefab `EntityId`, component payload, transform, behavior callback or runtime
spawn semantics; common stable-identity and deterministic-expansion principles do
not merge their authorities.

## Core Decisions And Dual-Role Model

Horo Engine resolves the tension between authoring convenience, static scene
optimization, and dynamic gameplay spawning through an explicit **dual-role**
architecture spanning two lifecycles:

1. **Authoring-Time Nested Template (`PrefabDocument` / `.prefab`)**:
   - Authored in the Editor and stored under `assets/prefabs/`.
   - Serialized as structured, project-versioned documents conforming to `ProjectVersion`.
   - Supports multi-object hierarchies, nested prefab instances, and shallow overrides in Tier 0; deep overrides and live variants belong to Tier 2.
   - For static scene objects, authoring instances are **pre-expanded and flattened** into
     a derived viewport projection of `SceneDocument` (without replacing authored instance references) and baked into `RuntimeSceneDefinition`
     (scene cook). This delivers optimal contiguous runtime memory layout with zero runtime
     template expansion overhead.

2. **Runtime-Spawnable Cooked Template (`CookedPrefab`)**:
   - Compiled by the Asset Pipeline from source `.prefab` files into immutable,
     platform-optimized binary artifacts (`core.prefab` asset type).
   - Registered in the `AssetRegistry` and `CookCatalog` with a stable 128-bit `AssetId`.
   - Gameplay requests spawn via `SceneCommandBuffer::RequestSpawnPrefab` (any thread,
     `Result<OperationId, PrefabError>`). `SceneRuntimeAccess::SpawnPrefab` commits on the scene owner thread
     (`OwnerThreadNextFrame`, [ADR-018](../../adr/018-command-registration-permissions-threading-and-packaged-build-policy.md); job wait in [ADR-010](../../adr/010-job-waiting-and-operation-store-ownership.md)).
   - Commit allocates fresh `EntityId`s, copies components, and parents the hierarchy, then
     publishes. `OnCreate` / `OnStart` run only after commit.

```text
Authoring: Editor / Tools
  [assets/prefabs/*.prefab] + [SceneDocument instance references / overrides]
          |                                   |
          |                                   +--> [Derived viewport projection]
          |                                   |
          |                                   +--> Scene cook / flatten
          |                                             |
          +--> Prefab cook / flatten                    v
                    |                         [RuntimeSceneDefinition]
                    v                                   |
              [CookedPrefab]                            v
                    |                          [SceneRuntime active]
                    +--> RequestSpawnPrefab             ^
                         -> bounded admission / load    |
                         -> owner-thread stage / commit-+
                         -> gameplay lifecycle after publication
```

---

## Capability Tiers

Prefab capabilities are staged across three contract-stable tiers. They describe subsystem
scope, not product milestone assignments or a global execution order; the
[Product Roadmap Model](../delivery/product-roadmap.md) and native issue dependencies
own those delivery decisions:

```text
+------------------------------------------------------------------------------+
| Tier 0: Authoring Template Expansion & Instantiation (Baseline)              |
| - Canonical source .prefab format governed by ProjectVersion                 |
| - Single-root and multi-object parent-child hierarchies                      |
| - Placed instances in SceneDocument (AssetId + root transform & overrides)   |
| - Deterministic offline expansion into RuntimeSceneDefinition                |
| - Static cycle detection rejecting recursive inclusion loops                 |
| - Opaque roundtrip preservation of unknown project-owned component data      |
+------------------------------------------------------------------------------+
                                    |
                                    v
+------------------------------------------------------------------------------+
| Tier 1: Runtime Dynamic Spawn from Cooked Prefab (Runtime Spawn)             |
| - Asset Pipeline compiles .prefab into binary CookedPrefab (core.prefab)     |
| - Registered in AssetRegistry and CookCatalog with AssetId                   |
| - Dynamic spawn APIs in SceneRuntime and Gameplay (SceneCommandBuffer)       |
| - Runtime EntityId allocation, hierarchy setup, component copy, lifecycle    |
| - Fail-safe error returns (missing asset, corrupted data, invalid component) |
+------------------------------------------------------------------------------+
                                    |
                                    v
+------------------------------------------------------------------------------+
| Tier 2: Live Variant Inheritance & Dynamic Override Tracking (Deferred)      |
| - Prefab Variants inheriting from base prefab asset with delta overrides     |
| - Multi-level variant chains with DAG cycle verification                     |
| - Live editor propagation across open documents upon base prefab mutation    |
| - Granular per-property override tracking (revert/apply to base prefab)      |
+------------------------------------------------------------------------------+
```

### Tier 0: Authoring Template Expansion & Instantiation (Baseline)

- **Source Asset Format**: `.prefab` files stored in `assets/prefabs/` as canonical JSON/structured
  documents governed by `ProjectVersion` (`docs/architecture/foundation/project-versioning-and-migration.md`).
- **Hierarchy Support**: Single-root and multi-object parent-child hierarchies within explicit bounds.
- **Scene Placement**: `SceneDocument` stores lightweight instance references (`ScenePrefabInstance`)
  comprising an `AssetId` and explicit root transform/property overrides.
- **Authoring Expansion**: The editor expands prefab instances during scene loading and viewport
  rendering. Scene cook flattens all static prefab instances into `RuntimeSceneDefinition`.
  Expansion is a pure bounded transformation over one immutable scene-document and resolver
  snapshot. Every required placement must resolve before the runtime candidate is published;
  missing, corrupt, cyclic, conflicting, stale or over-budget content rejects the complete
  candidate and retains the previous runtime definition. The authored `ScenePrefabInstance`
  remains the repair authority, while the editor may retain a separate broken-instance
  projection carrying its stable instance and source identities plus the typed failure chain.
- **Cycle Detection**: Static validation traps recursive inclusion chains (`A -> B -> A`) before
  expansion or serialization.
- **Component Preservation**: Unknown/plugin-owned component payloads are retained opaquely
  (`RawComponentPayload`) without data loss.

### Tier 1: Runtime Dynamic Spawn from Cooked Prefab (Runtime Spawn)

- **Cooked Binary Artifact**: The Asset Pipeline compiles `.prefab` assets into immutable binary
  `CookedPrefab` artifacts registered under `core.prefab` in `CookCatalog`.
- **Runtime Asset Management**: Loaded through `IAssetProvider` via stable `AssetId`.
- **Dynamic Spawn API**: `SceneCommandBuffer::RequestSpawnPrefab` (any thread, returns `Result<OperationId, PrefabError>`)
  wraps `SceneRuntimeAccess::SpawnPrefab` (owner thread, `OwnerThreadNextFrame`).
- **Lifecycle Guarantees**: Owner-thread `EntityId` allocation, staged component copy, commit,
  then `OnCreate`, `OnEnable` when enabled, and `OnStart` once before the first eligible fixed
  update after first enable. Created-disabled behaviors defer enable/start. Repeated IDs in
  inherited spawn lineage are rejected even across frames and asynchronous loads.
- **Fail-Safe Robustness**: Catalog misses, cooked-version mismatch, corruption, bounds, and
  spawn recursion fail the operation without publishing entities or invoking hooks.
- **Async load**: Unloaded-but-catalogued assets become an ADR-018 `WorkerJob` + `OperationStore`
  load-then-spawn. Callers do not poll `AssetNotLoaded`.

### Tier 2: Live Variant Inheritance & Dynamic Override Tracking (Deferred)

[ADR-093](../../adr/093-prefab-override-property-identity-and-delta-operations.md)
fixes the property/component identity and delta foundation used by this tier, and
[ADR-094](../../adr/094-prefab-nested-composition-and-variant-inheritance.md)
fixes nested-placement, single-parent variant and resolution semantics. Neither
decision moves the tier into the implemented baseline; delivery and UI remain
their owning work.

- **Prefab Variants**: A variant `.prefab` references exactly one immediate parent `AssetId` and
  stores only ADR-093 property/component deltas. V1 rejects multiple parents and hierarchy
  add/remove/reparent operations.
- **Combined Semantic Graph**: Multi-level variant chains (`Base -> VariantA -> VariantB`) and
  nested-placement edges are validated together for acyclicity and explicit bounds.
- **Live Editor Propagation**: When a base prefab is modified and saved, open variant documents,
  scenes referencing the prefab, and active viewport sessions update in real time.
- **Granular Override Management**: Deep per-property diffing, revert-to-prefab, and apply-to-prefab
  workflows in the Inspector panel.

### Stable Override Identity And Delta Algebra

Overrides address the exact source `AssetId`/revision, nested `LocalObjectId` scope,
target `LocalObjectId`, registered `ComponentTypeId`, persisted
`ComponentInstanceId` and typed `PropertyId` path. Built-in and project/package
components use the same schema registry. Display/localization/JSON/C++ names,
offsets, reflection/component order and collection indexes are never durable
identity.

Collections declare `Atomic`, `StableElements` or `CanonicalMap`. An unkeyed
sequence accepts only whole-property assignment. Granular element operations use a
persisted `CollectionElementId` or canonical typed map key and stable move anchors.

V1's closed operations are `AssignValue`, `InsertElement`, `RemoveElement`,
`MoveElement`, `AssignElementValue`, `AddComponent` and `RemoveComponent`. Revert
removes the corresponding record through an Editor command; it is not a serialized
operation. Hierarchy add/remove/reparent remains a separate decision and is rejected
by this algebra.

Records carry exact source/schema revision and canonical expected-value/component
digests. They sort canonically by object scope, local object, component type/
instance, property path, target class and operation rank. Duplicate/conflicting
same-layer operations fail rather than using serialized last-write-wins order.

Schema canonical bytes own equality. The comparison default is the exact effective
immediate source-layer value, not the current registry default or Inspector string.
Equal assignments are removed as redundant; added components store a complete
validated payload so later default changes cannot reinterpret them.

Source updates produce a detached deterministic three-way rebase candidate.
Simultaneous incompatible changes become explicit conflicts; missing/retyped/
unresolvable targets become losslessly preserved orphans. IDs retarget only through
registered ProjectVersion/schema migrations. Opening, saving or package unload
never guesses or drops records.

Apply, revert, conflict resolution, rebase and apply-to-prefab run through typed
document/application transactions with undo/history, revision, dirty-state and
atomic-save semantics. Inspector/file-watcher/serializer code does not mutate the
document or `.prefab` file directly.

### Nested Composition And Variant Resolution

[ADR-094](../../adr/094-prefab-nested-composition-and-variant-inheritance.md)
defines two and only two prefab semantic edges. A concrete prefab may own bounded
`NestedPlacement` edges, each identified by a unique persisted `LocalObjectId` slot.
A variant owns exactly one `VariantParent` edge and an ADR-093 override set. Resource
dependencies are not composition edges. Multiple inheritance, mixins, conditional
parents, construction scripts and V1 hierarchy add/remove/reparent deltas are
rejected.

Resolution uses one immutable Asset Registry snapshot. A concrete asset materializes
its authored objects, recursively resolves each nested source, applies the
placement-local override and mounts it at the placement slot. A variant resolves
its single immediate parent and then applies its delta. A scene instance resolves
its selected asset and applies its instance delta last. This recursive algorithm,
not serialized order, defines precedence and emits bounded per-object/property
provenance for the Editor.

The combined graph includes `NestedPlacement` and `VariantParent` edges and must be
acyclic even when a cycle crosses edge kinds. Variant paths are limited to 8 edges,
nested-placement paths to 16 edges and one concrete document to 256 direct nested
placements; existing expanded hierarchy/object/component/payload bounds still
apply. Validation, re-resolution and ADR-093 rebase produce detached candidates.
Cycle, revision, conflict, bound, cancellation or stale-snapshot failure publishes
nothing and preserves the previous document/projection.

Cook consumes the complete resolved candidate. Its dependency-aware key includes
edge kinds, placement IDs, reachable source revisions and override digests, then it
flattens the hierarchy. Packaged runtime does not traverse authoring inheritance or
composition graphs and never runs construction behavior to produce prefab data.

#### Asset Dependency Closure And Conflict Policy

`PrefabAssetDependencyClosure` is the owned, immutable handoff from prefab graph
resolution to scene/prefab cook composition. It is pinned to the exact Asset
Registry revision used by `PrefabDependencyGraphSnapshot` and contains the unique
transitive dependency set in ascending `AssetId` order. Each entry retains the
registry asset type and, for captured prefab sources, the exact source revision.
The requested roots are resolution inputs and are excluded from their own closure.

The closure builder may merge pre-existing scene/cook requirements, but validates
them against the same immutable registry publication first. Equal requirements
deduplicate. The same `AssetId` with another type or prefab source revision is an
explicit `DependencyConflict`; V1 never guesses, overwrites, or uses insertion
order as precedence. A stale registry/graph pairing, absent asset, invalid identity,
unsupported policy, or exceeded caller-owned capacity returns a typed failure and
publishes no partial result. This derived closure never mutates authored prefab
documents or the active runtime/cook generation.

`RuntimeDependencies()` is the source-free `RuntimeSceneDefinition` projection.
It retains canonical `AssetId`/type pairs for ordinary cooked resources and
pre-existing scene requirements, while excluding captured prefab-source nodes;
their source revisions remain closure evidence for cook keys and stale-result
rejection but never leak into the packaged runtime definition.

### External Reference Boundary And Binding Slots

[ADR-096](../../adr/096-prefab-external-reference-and-binding-slot-contract.md)
permits only stable prefab-local object/component references, typed `AssetId`
references and references to declared binding slots. Direct serialized scene object
IDs, runtime `EntityRef`, ECS/component addresses, pointers, paths and name/tag
queries are invalid in prefab source, variants, overrides and cooked templates.

A concrete prefab may declare at most 64 stable `PrefabBindingSlotId` interfaces,
each typed as an external scene object or registered component and marked Required
or Optional. Variants inherit declarations unchanged. Bindings are separate from
ADR-093 overrides and spawn initialization values: they map one declared interface
to an external identity for one placement/spawn and have no default comparison,
apply/revert or property operation semantics.

Static scene instances bind slots to stable scene-authoring targets. Nested
placements may bind an inner slot to a containing prefab-local target or re-expose
it through an exactly compatible outer slot; they cannot capture a scene target.
Dynamic spawn requests carry bounded copied `EntityRef` bindings that are
revalidated for target scene, generation, component schema, uniqueness and required
coverage at owner-thread commit. Missing required or invalid supplied optional
bindings fail transactionally. Omitted optional slots become typed `Unbound` with no
search or fallback.

Create-from-selection classifies every known reference. Internal targets become
stable local references, assets retain `AssetId`, and every external/opaque crossing
is reported for explicit include, expose-slot or cancel action. The transaction
never silently nulls, copies or captures an external scene object.

---

## Identity, Hierarchy, And Asset Model

### Single Source of Identity

1. **Asset Registry Integration**: Prefabs are first-class assets. Every prefab is identified
   authoritatively by its 128-bit `AssetId` declared in its companion sidecar (`.prefab.meta`).
2. **Path Decoupling**: Project paths (`assets/prefabs/weapons/rifle.prefab`) are editor convenience
   hints. If a prefab is moved or renamed, references remain valid through `AssetId`.
3. **Project Versioning**: Prefab documents share the unified project schema versioning and
   automated migration framework (`docs/architecture/foundation/project-versioning-and-migration.md`).

### Hierarchy Addressing And Scoped Identity Composition

A prefab defines a self-contained hierarchy of authored objects. Identity rules across multiple instances and nested scopes are:

```cpp
namespace Horo::Prefab {
    /** @brief Persisted local slot identifying an authored object; not a vector index. */
    struct LocalObjectId {
        std::uint32_t value{0};
        [[nodiscard]] constexpr bool IsRoot() const noexcept { return value == 0; }
        [[nodiscard]] constexpr auto operator<=>(const LocalObjectId&) const noexcept = default;
    };

    /** @brief Reference to a prefab asset in authoring documents. */
    struct PrefabAssetReference {
        Assets::AssetId assetId;
        std::string pathHint; // Advisory authoring path
    };
}
```

- **Stable local slots**: Root slot zero is reserved. Surviving `LocalObjectId` values do not
  change when objects are reordered, inserted, or deleted; deleted slots are not reused while
  references may remain. Sparse authored slots are mapped to dense cooked table indices.
- **Tier 0 Authoring Expansion**: Generate deterministic candidate scene IDs with a fixed,
  versioned hash over canonical instance-scope/local-ID bytes. Each nested instance extends
  the scope with its owning local slot, distinguishing repeated nested references. Check all
  candidates against authored and expanded IDs before publication. A collision fails with
  `PrefabError::IdentityCollision`; a 64-bit hash does not guarantee uniqueness and never
  authorizes overwriting another object. Repeated expansion of unchanged input is stable.
- **Tier 1 Runtime Spawning**: Use the existing Scene Runtime `EntityId { index, generation }`
  allocator. Reused slots increment generation and retire rather than wrap. Public results
  carry scene-qualified `EntityRef { SceneRuntimeId, EntityId }`; no prefab-specific monotonic
  counter or hash replaces that identity model. Allocation remains owner-thread-only.

---

## Prefab Document Schema And Explicit Bounds

`PrefabHardLimits` is the single code authority for immutable engine safety ceilings.
`PrefabProjectPolicy` may lower the configurable subset, and
`PrefabLimitProfile::Create` validates and owns that policy before an operation starts.
No raw project setting is accepted by a Prefab operation.

| Bound family | Project-configurable policy | Engine hard ceiling |
|---|---|---:|
| source hierarchy / objects / components | yes | `16` / `256` / `64` |
| source / expanded / cooked payload bytes | yes, independently | `4 MiB` each |
| referenced assets / direct nested placements | yes | `256` / `256` |
| variant / nested graph depth | yes | `8` / `16` |
| override / conflict-or-orphan records | yes | `16,384` / `16,384` |
| property path / value / aggregate override bytes | yes | `32` / `1 MiB` / `16 MiB` |
| binding slots / uses / instance bindings | yes | `64` / `256` / `64` |
| runtime spawn lineage depth | yes | `8` including the target |
| object display-name bytes | no; hard safety only | `256` |

The expansion work limit is derived, not independently configured: checked arithmetic sums
the accepted object, object-component, dependency, direct-placement, graph-depth,
override-path, conflict-or-orphan and binding-use maxima. An operation-local `PrefabExpansionBudget`
charges work before performing it. Invalid policy returns `LimitProfileInvalid`; exhausting
an already valid captured budget returns `WorkBudgetExceeded` without consuming the failed
charge or publishing a partial candidate.

Required prefab expansion is transactional at the scene boundary. A failure in one placement
does not skip that placement, publish the successfully expanded placements, or replace a prior
runtime definition with a partial hierarchy. The failure retains the stable scene-local instance
identity and the complete source-asset chain used by the resolver. Broken editor projections are
repairable authoring views only; they are never runtime definitions and contain no mutable Prefab
state or source paths.

Limits apply to the fully expanded hierarchy as well as individual inputs: root depth is 1,
object count includes all nested expansions, and component count includes built-in, opaque,
and behavior components. Bound source, expanded, and cooked payload sizes independently;
validate incrementally before expansion exceeds allocation limits. Limits are enforced at
save/import/placement, at cook (no artifact on failure), and before runtime staging.

Runtime validates the artifact envelope and actual table offsets, lengths, alignment, parent
indices, component counts, and hierarchy; header counts alone are not trusted. A concrete source
requires exactly one root, unique local IDs, valid parents, and an acyclic connected hierarchy;
it may own nested placements but no variant parent. A variant owns no parallel object or placement
hierarchy and requires exactly one immediate parent. Invalid authored graphs return
`InvalidHierarchy`; malformed cooked encoding returns `CorruptedPayload`; well-formed over-limit
input returns the relevant bounds error. Combined nested/inheritance cycles return
`CyclicComposition` at authoring/cook time.

`PrefabDocument::Create` now requires a validated `PrefabLimitProfile`. This public-contract
change prevents a document from silently validating against defaults that differ from the
project snapshot. The only in-repository callers were Prefab document tests and were migrated
atomically. External callers migrate by resolving project policy once, retaining the returned
profile for the operation, and passing it with the candidate. There is no compatibility
overload or second default authority. Project JSON/UI persistence is application-owned and is
introduced separately; resolver, cook and spawn tickets consume the same profile when those
operations are implemented.

### Authoring Document Structure (`PrefabDocument`)

```cpp
struct PrefabObjectNode {
    LocalObjectId localId;
    std::optional<LocalObjectId> parentLocalId;
    std::string name;
    Vec3 localPosition{0.0f, 0.0f, 0.0f};
    Vec3 localScale{1.0f, 1.0f, 1.0f};
    Quat localRotation{Quat::Identity()};

    // Component payloads (typed built-in components + opaque custom payloads)
    std::optional<Runtime::MeshComponent> mesh;
    std::optional<Runtime::CameraComponent> camera;
    std::optional<Runtime::LightComponent> light;
    std::optional<Runtime::AudioSourceComponent> audioSource;
    std::vector<RawComponentPayload> customComponents;
    std::vector<Gameplay::BehaviorAttachmentDefinition> behaviors;
};

struct NestedPrefabPlacementV1 {
    LocalObjectId placementLocalId; // Persisted scope segment, never an array index
    std::optional<LocalObjectId> parentLocalId;
    Assets::AssetId sourcePrefab;
    PrefabSourceRevision authoredAgainst;
    Transform localRootTransform;
    PrefabOverrideSetV1 overrides;
};

struct PrefabVariantInheritanceV1 {
    Assets::AssetId immediateParent; // Exactly one parent for a variant
    PrefabSourceRevision authoredAgainst;
    PrefabOverrideSetV1 overrides;
};

struct PrefabDocument {
    HoroProjectVersion projectVersion; // Unified authoring version, not a prefab schema counter
    Assets::AssetId assetId;
    std::vector<PrefabObjectNode> objects; // concrete only; first node is root; IDs may be sparse
    std::vector<NestedPrefabPlacementV1> nestedPlacements; // concrete assets only
    std::optional<PrefabVariantInheritanceV1> variant; // variant assets only
    std::vector<PrefabBindingSlotDeclarationV1> bindingSlots; // concrete assets only
    std::vector<Assets::AssetId> referencedAssets;
};
```

---

## Runtime Cooked Prefab (`CookedPrefab`)

The Asset Pipeline cooks source `.prefab` files into immutable `CookedPrefab` artifacts.

### Cook Boundary And Artifact Roles

[ADR-095](../../adr/095-prefab-cook-boundary-and-artifact-model.md) separates four
representations. Project-owned `PrefabDocument` source is migrated before cook and
is not a runtime artifact. ADR-094's immutable `EffectivePrefabCandidateV1` is the
only nested/variant expansion result consumed by viewport, Scene Cook and Prefab
Cook; it is never edited, registered, packaged or loaded by runtime.

Scene Cook embeds static placements as ordinary entity/component definitions in
the cooked scene's `RuntimeSceneDefinition`. Prefab Cook emits a separate flat
`CookedPrefab` only for declared dynamic-spawn roots. Both invoke the same Prefab
resolver; neither generic Assets nor Scene Runtime re-expands the graph. AssetCook
owns bounded scheduling, cache validation, staging, deterministic manifests and
atomic generation publication, while the Prefab/Scene domains own semantic
resolution and their typed output schemas.

The dependency-aware key covers exact source and transitive revisions/digests,
ProjectVersion, package/schema/resolver revisions, semantic edges/placement IDs,
override digests, contribution/output/envelope versions, target/profile and—per
role—scene placement/conversion or dynamic-spawn format policy. Candidate failure,
cancellation, corruption, stale completion or publication failure leaves the prior
`current.json` and provider generation active.

Standard releases ship no raw `.prefab`, sidecar, effective candidate or editor
provenance. Static-only prefab content appears only in the cooked scene. A
`CookedPrefab` and its resource closure ship only when reachable from a declared
dynamic-spawn root. Developer source profiles may include sources explicitly, but
they never become runtime parsing authority.

Source `.prefab` files share `ProjectVersion` and its migration pipeline. Cooked blobs do **not**.
The header carries a distinct `cookedFormatVersion`. Runtime never migrates cooked bytes.
Prefab cooking preserves the Asset Pipeline's complete canonical versioned cache-key inputs:
asset identity/type, source and cooker-input metadata digests, effective settings/schema,
cooker contribution identity/version, typed target/profile, and artifact-envelope version.
Its dependency-aware extension also covers `ProjectVersion`, `cookedFormatVersion`, and
canonically ordered transitive dependency identities/artifact digests. Dependency-free
AST-001C `CacheKeyV1` rejects nested-prefab dependencies; Tier 1 requires the explicit
[dependency-aware cache extension](./asset-pipeline.md#incremental-cook-and-cache-reuse),
not an incompatible change to V1 or a parallel prefab-only cache.

Source migrations, dependency/target/settings changes, and cooker/format changes invalidate
output. `UnsupportedCookedVersion` fails the runtime operation; only authoring/build tools
recook and republish a compatible catalog/artifact. Runtime neither migrates nor recooks.
`CorruptedPayload` is a separate envelope, magic, digest, or structural validation failure.

Development hot reload activates a fully validated replacement generation. Later
spawns use its `CookedPrefab`; already spawned entities remain unchanged. Static
source changes produce a new cooked scene candidate and require the normal Scene
Runtime activation transaction. Asset reload never patches scene-owned entities or
falls back to source parsing.

### Binary Layout

- **Header / envelope**: Magic bytes (`HPFB`), `cookedFormatVersion`, object count, and payload
  size. The standard artifact envelope validates a cryptographic digest of the actual cooked
  payload bytes. Source/dependency digests are provenance/cache inputs, not substitutes for
  cooked-byte integrity verification.
- **Hierarchy Table**: Flat array of parent-child slot indices and local transforms.
- **Component Tables**: Contiguous, aligned arrays of primitive component data and behavioral descriptors.
- **Asset Reference Table**: List of dependent `AssetId`s required for instantiation.

```cpp
namespace Horo::Runtime {
    class CookedPrefab final {
    public:
        [[nodiscard]] Assets::AssetId GetAssetId() const noexcept;
        [[nodiscard]] std::uint32_t GetObjectCount() const noexcept;
        [[nodiscard]] std::span<const PrefabSlotDescriptor> GetHierarchy() const noexcept;
        [[nodiscard]] std::span<const Assets::AssetId> GetDependencies() const noexcept;
        // ... internal typed component table accessors
    };
}
```

---

## Dynamic Runtime Spawning Contract

This contract conforms to [ADR-018](../../adr/018-command-registration-permissions-threading-and-packaged-build-policy.md)
and [ADR-010](../../adr/010-job-waiting-and-operation-store-ownership.md).
Spawn *commit* is
`CommandThreadPolicy::OwnerThreadNextFrame`. The main/editor thread never `Wait()`s a worker
for spawn completion.

```cpp
namespace Horo::Runtime {
    struct PrefabSpawnRequest {
        SceneRuntimeId targetScene;
        Assets::AssetId prefabAssetId;
        Transform spawnTransform;
        std::optional<EntityRef> parentEntity{std::nullopt};
        BoundedVector<PrefabRuntimeBindingV1> externalBindings;
        PrefabInitializationValuesV1 initialization;
    };

    struct SpawnedPrefabHandle {
        EntityRef rootEntity;
        std::vector<EntityRef> childEntities;
    };

    class SceneCommandBuffer {
    public:
        // Any thread. Enqueue only. Typed admission error or an OperationId.
        Result<OperationId, PrefabError> RequestSpawnPrefab(const PrefabSpawnRequest& request);
    };

    class SceneRuntimeAccess {
    public:
        // Internal owner-thread drain; resident assets and captured context required.
        Result<SpawnedPrefabHandle, PrefabError> SpawnPrefab(const PrefabSpawnRequest& request);
    };
}
```

`RequestSpawnPrefab` is the planned scene-bound gameplay admission seam; `SpawnPrefab` is
its internal owner-thread implementation. These are Tier 1 target contracts, not existing
SCN-001 APIs. The extension adds thread-safe admission without changing the thread affinity
of existing `SceneCommandBuffer::Create` / `Destroy`. Hierarchy staging uses the later ECS
contract; it must not bypass Scene Runtime's transactional structural publication.

Successful admission returns an `OperationId`; a closed/full queue or full operation store
returns `AdmissionRejected` without accepting work. The host-composed spawn coordinator
exclusively mutates the authoritative `OperationStore`. It captures owned request values,
target scene identity, cancellation, diagnostic/configuration context, and explicit lineage
before enqueue; workers do not retain borrowed scene/parent pointers. The internal access
facade is bound to this captured context for the drain; the public request struct does not
let callers manufacture or erase ancestry.

### Loaded vs not-yet-loaded

- Catalog miss completes the accepted operation with `AssetNotFound`.
- A catalogued non-resident template and its required dependencies load through the composed
  `AssetLoadService` / `IAssetProvider` path. Workers perform bounded, cancellation-aware I/O;
  they do not synchronously wait for another queued worker or owner-thread commit. Completion
  schedules an owner-thread continuation holding immutable asset leases. The coordinator
  projects progress and terminal results through the original `OperationId`.
- Direct internal `SpawnPrefab` without resident required assets returns `AssetNotLoaded` as
  a misuse diagnostic. Gameplay uses admission; it never polls that error.
- Owner-thread publication revalidates cancellation, target `SceneRuntimeId`, parent generation
  and scene membership, and pinned dependencies. Scene unload/replacement closes admission,
  cancels pending operations, and rejects late continuations with `Cancelled` or
  `SceneUnavailable`; stale/cross-scene parents return `InvalidParent`.

### Spawning Execution Sequence

```text
SceneCommandBuffer::RequestSpawnPrefab(request)     // any thread; bound context
  -> validate/copy context and lineage; reserve bounded operation/queue capacity
  -> reject admission or return OperationId; never mutate SceneRuntime
  -> coordinator resolves catalog and schedules required asset loads
  -> asset completion enqueues owner-thread continuation with immutable leases

SceneRuntimeAccess::SpawnPrefab(request)            // internal owner-thread drain
  -> validate inherited lineage, cancellation, scene and parent identity
  -> validate cooked version, actual payload digest, hierarchy, bounds, capacity
  -> STAGE: IDs, component data, behavior descriptors/storage (no behavior instances)
  -> staging failure: discard unpublished storage and handles; fail operation
  -> recheck cancellation and lifetime at CommitDeferredLifecycleChanges
  -> COMMIT: atomically publish the complete hierarchy and successful handle
  -> construct behaviors; OnCreate; OnEnable if enabled
  -> OnStart once after first enable, before the first eligible fixed update
```

`OwnerThreadNextFrame` is the dispatch classification. Structural publication uses Scene
Runtime's `CommitDeferredLifecycleChanges`; it is not an extra mutation phase in
`PreUpdate` / `DebugPhase`. Preparation may precede publication but never leaks candidate
handles or partial entities. Work submitted by lifecycle callbacks is eligible only in a
subsequent bounded drain batch. Host-configured queue count/byte and per-drain work budgets
prevent unbounded fan-out in a single frame.

Staging does not construct gameplay behavior instances, invoke hooks, or emit bus/network
side effects. Therefore rollback has no `OnDestroy` obligation. After structural commit,
construction/hook faults follow normal gameplay fault handling and do not retroactively
fail or roll back the successful structural spawn. Faulted behaviors do not start/update;
every constructed behavior receives `OnDestroy` exactly once during normal teardown.
Created-disabled behaviors still run `OnCreate` but defer `OnEnable` / `OnStart` until enabled.

Commit and cancellation are serialized by the coordinator/owner-thread protocol: cancellation
accepted before publication leaves no entities; cancellation after publication does not undo
commit or change the successful spawn into a cancelled operation.

### Runtime spawn recursion

Cook flattens nested references; runtime does not recursively expand raw prefabs. Lifecycle
spawn chains still need protection across queues, frame boundaries, and asynchronous loads:

- Each accepted spawn carries immutable lineage ending in its target `AssetId`.
- Requests originating in `OnCreate`, initial `OnEnable` / `OnStart`, or their asynchronous
  continuations inherit that lineage through an explicitly bound command context. A caller
  cannot drop ancestry when submitting through that context. No global or thread-local stack
  is the authority, and the internal direct path must perform the same validation.
- Before appending the requested ID, a repeated ID fails with `SpawnRecursionDetected`;
  exceeding `MaximumRuntimeSpawnDepth` (8, including the new target) fails with
  `SpawnDepthExceeded`. This rejects both `A -> A` and delayed `A -> B -> A`.
- Independent later gameplay requests start new lineages, so repeated legitimate spawning
  remains supported. Child rejection does not roll back an already-committed parent.

---

## Preservation Of Unknown Project Components

To support modular gameplay packages and project-specific C++ plugins:

1. **Opaque Preservation Contract**:

   ```cpp
   struct RawComponentPayload {
       ComponentTypeId componentType;
       ComponentInstanceId componentInstance;
       std::uint32_t schemaVersion{1};
       std::vector<std::uint8_t> serializedBytes;
   };
   ```

2. **Editor Roundtrip**: If an authored prefab or scene contains custom component types unknown to
   the base editor binary, the data is preserved in `RawComponentPayload` verbatim. Saving, cloning,
   or expanding the prefab preserves these components without truncation.
3. **Runtime Spawning Safety**: If an unregistered component type is encountered during runtime
   spawn, the transaction aborts before publishing entities or invoking behavior lifecycle hooks.
   Staging discards unpublished storage (no gameplay behavior instance was constructed) and returns `PrefabError::ComponentTypeUnregistered`.

Unknown override records for that component preserve their stable component/property
envelope and original bounded canonical bytes as ADR-093 orphans. Package unload,
round-trip save or source rebase cannot translate them through display names or
discard them. Cook/runtime expansion remains blocked until the schema is available
and the records validate or an explicit transaction deletes them.

---

## Diagnostics And Error Model

Dynamic spawning and authoring expansion report structured, typed errors:

```cpp
enum class PrefabError : std::uint32_t {
    Success = 0,
    AssetNotFound,              // AssetId not registered in CookCatalog / AssetRegistry
    AssetNotLoaded,             // Direct owner-thread spawn while blob is not resident (misuse)
    UnsupportedCookedVersion,   // cookedFormatVersion not understood by this runtime
    CorruptedPayload,           // Checksum or magic header validation failed
    HierarchyDepthExceeded,     // Template exceeds MaximumPrefabHierarchyDepth (16)
    ObjectCountExceeded,        // Template exceeds MaximumPrefabObjectCount (256)
    PayloadTooLarge,            // Template exceeds MaximumPrefabPayloadBytes (4 MiB)
    CyclicReferenceDetected,    // Nested prefab inclusion contains a cycle (authoring/cook)
    CyclicComposition,          // Combined nested-placement/variant graph contains a cycle
    MissingPrefabSource,        // Semantic edge references no available source revision
    InvalidPlacement,           // Placement ID, parent, transform or scope is invalid
    MultipleVariantParents,     // Variant does not have exactly one immediate parent
    VariantDepthExceeded,       // More than 8 variant-parent edges in one path
    CompositionDepthExceeded,   // More than 16 nested-placement edges in one path
    CompositionEdgeLimitExceeded, // More than 256 direct placements in one concrete source
    SpawnRecursionDetected,     // Requested AssetId repeats in inherited spawn lineage
    SpawnDepthExceeded,         // Lineage exceeds MaximumRuntimeSpawnDepth
    AdmissionRejected,         // Queue/store capacity exhausted or admission closed
    Cancelled,                 // Cancellation accepted before structural commit
    SceneUnavailable,          // Captured target scene unloaded or replaced
    InvalidParent,             // Stale generation or parent outside target scene
    IdentityCollision,         // Deterministic authoring ID collides with another object
    InvalidHierarchy,          // Duplicate local IDs, invalid parents, roots, or graph
    ComponentCountExceeded,    // More than MaximumPrefabComponentsPerObject (64)
    EntityAllocationExhausted,  // Scene runtime ran out of available EntityIds
    ComponentTypeUnregistered, // Cooked template references an unavailable component type
    ComponentAllocationFailed, // Memory allocation failed for component pool
    InvalidReference,          // Unsupported or unresolved persisted reference class
    DuplicateOrConflictingOverride, // One layer contains competing semantic operations
    BindingSlotInvalid,        // Declaration/use/re-exposure schema is invalid
    RequiredBindingMissing,    // Required instance/spawn slot has no binding
    BindingTargetInvalid,      // Supplied target is stale, cross-scene or incompatible
    BindingLimitExceeded       // Declaration/use/instance-binding bound exceeded
};
```

### Validation Matrix

| Case Category | Test Scenario | Expected Outcome |
|---|---|---|
| **Valid** | Single-root prefab with mesh & transform | Expands/spawns correctly; root entity created |
| **Valid** | Multi-object hierarchy (depth 3, 5 objects) | Preserves relative transforms and child parenting |
| **Valid** | Dynamic spawn with enabled gameplay behavior | `OnCreate`, `OnEnable`, then `OnStart` before first eligible fixed update |
| **Boundary** | Maximum hierarchy depth (exactly 16) | Expansion and spawn succeed |
| **Boundary** | Maximum object count (exactly 256) | Allocation and instantiation succeed |
| **Boundary** | Empty overrides on placed instance | Inherits base template values completely |
| **Malformed** | Cycle detection (`A -> B -> A`) | Rejection with `PrefabError::CyclicReferenceDetected` |
| **Malformed** | Hierarchy depth > 16 or count > 256 | Rejection with boundary error diagnostic |
| **Malformed** | Corrupted magic header or truncated payload | Rejection with `PrefabError::CorruptedPayload` |
| **Lifecycle** | Spawn non-existent `AssetId` | Operation fails `AssetNotFound`; no scene leak; no poll |
| **Lifecycle** | Spawn catalogued but not-yet-loaded prefab | `OperationId` load-then-spawn; not a sync `AssetNotLoaded` |
| **Lifecycle** | `OnCreate` spawns the same `AssetId` | `SpawnRecursionDetected`; no extra entities |
| **Lifecycle** | Staging fails after some entities allocated | Discard staged handles; no `OnCreate` / no `OnDestroy` |
| **Lifecycle** | Cooked format newer than runtime | `UnsupportedCookedVersion`; not `CorruptedPayload` |
| **Lifecycle** | Unknown component in cooked template | Staging abort; no hooks; `ComponentTypeUnregistered` |
| **Lifecycle** | Scene destruction with active spawned entities | Cleanly tears down entities and behavior instances |
| **Malformed** | Authoring depth > 16 on save | Save rejected; `.prefab` not written |
| **Malformed** | Cook of over-limit prefab | Cook fails; no `CookedPrefab` artifact |
| **Identity** | Reorder/insert/delete authored objects | Surviving local IDs and overrides remain stable |
| **Identity** | Repeated nested instances or injected hash collision | Distinct scoped IDs; collision fails atomically with `IdentityCollision` |
| **Identity** | Reuse a destroyed runtime entity slot | Old generation and old scene references remain invalid |
| **Boundary** | Lineage depth 8 then 9 across queued frames | Depth 8 accepted; depth 9 returns `SpawnDepthExceeded` |
| **Boundary** | Expanded nested count/payload or component count exceeds limit | Reject incrementally before oversized allocation or publication |
| **Malformed** | Duplicate local IDs, missing parent, multiple roots | `InvalidHierarchy`; no authoring expansion published |
| **Malformed** | Tampered cooked bytes with unchanged source digest | Actual artifact payload digest fails; `CorruptedPayload` |
| **Lifecycle** | Delayed `A -> B -> A`, including asset-load continuation | `SpawnRecursionDetected`; rejected child publishes nothing |
| **Lifecycle** | Independent later spawn of A | Fresh lineage; legitimate repeat succeeds |
| **Lifecycle** | Spawn queue or operation store full | `AdmissionRejected`; no orphan operation or accepted work |
| **Lifecycle** | Cancel/unload/replace scene while asset loads | No late publication into destroyed or replacement scene |
| **Lifecycle** | Parent destroyed/reused while asset loads | `InvalidParent`; no attachment to replacement entity |
| **Lifecycle** | Created-disabled behavior | `OnCreate` runs; `OnEnable`/`OnStart` wait for first enable |
| **Lifecycle** | Post-commit behavior construction or hook fault | Structural spawn remains committed; fault reported; normal teardown |
| **Cook** | Nested dependency, target, settings, or version changes | Full canonical cache key changes; no stale artifact reuse |
| **Identity** | Built-in/project component and property renamed or reordered | Stable IDs retain target; labels/offsets/order are irrelevant |
| **Identity** | Same component type appears twice on one object | Persisted `ComponentInstanceId` selects the exact occurrence |
| **Identity** | Source inserts before an overridden collection element | Stable element ID remains targeted; unkeyed numeric path is rejected |
| **Valid** | Assign/insert/remove/move/add-component/remove-component override | Canonical typed delta resolves in stable order |
| **Malformed** | Duplicate/conflicting operation targets in one layer | Reject; no last-write-wins or partial candidate |
| **Lifecycle** | Source and override change the same value incompatibly | Preserve explicit three-way conflict and original override bytes |
| **Lifecycle** | Source deletes/retypes target or package schema unloads | Preserve orphan/opaque record; cook blocked until explicit resolution |
| **Lifecycle** | Revert/rebase/apply-to-prefab fails or is cancelled | Document/source/override/history remain unchanged |
| **Valid** | Same variant is nested twice under distinct placement IDs | Two distinct scopes with identical inherited values and independent placement overrides |
| **Valid** | Base -> VariantA -> VariantB plus scene override | Oldest-to-newest variant precedence, then scene-instance override |
| **Boundary** | Variant depth 8 and nested-placement depth 16 | Exact bounds resolve and cook |
| **Malformed** | Variant depth 9, nested depth 17, or 257 direct placements | Typed graph limit; no candidate/artifact publication |
| **Malformed** | Nested A -> B plus variant parent B -> A | `CyclicComposition` with canonical edge path; prior projection remains active |
| **Malformed** | Variant has two parents or owns hierarchy deltas | Explicit rejection; no serialized-order merge or guessed topology |
| **Lifecycle** | Source changes during descendant re-resolution | Stale completion cannot replace newer registry/document snapshot |
| **Capability** | Construction callback could affect composition | Callback is never invoked; unsupported request is rejected |
| **Valid** | Local object/component and AssetId references | Stable identities resolve without path/index dependence |
| **Valid** | Required static/dynamic typed binding supplied | Conversion/spawn validates before atomic publication |
| **Valid** | Optional binding omitted | Canonical typed `Unbound`; no discovery or fallback |
| **Malformed** | Prefab stores scene ID, EntityRef, pointer, path or name query | `InvalidReference`; source/artifact rejected |
| **Malformed** | Missing required or invalid supplied optional binding | Transaction fails; no partial scene/entities/hooks |
| **Lifecycle** | Create-from-selection crosses boundary | Explicit include/expose/cancel report; no silent null/copy/capture |
| **Lifecycle** | Bound target is destroyed/reused | Typed unavailable/stale result; never retargeted |

---

## Related Documents

- [ADR-017: Prefab Role, Ownership and Capability-Tier Decision](../../adr/017-prefab-role-ownership-and-capability-tiers.md)
- [ADR-093: Prefab Override Property Identity and Delta Operations](../../adr/093-prefab-override-property-identity-and-delta-operations.md)
- [ADR-094: Prefab Nested Composition and Variant Inheritance](../../adr/094-prefab-nested-composition-and-variant-inheritance.md)
- [ADR-095: Prefab Cook Boundary and Artifact Model](../../adr/095-prefab-cook-boundary-and-artifact-model.md)
- [ADR-096: Prefab External Reference and Binding Slot Contract](../../adr/096-prefab-external-reference-and-binding-slot-contract.md)
- [ADR-018: Command Registration, Permissions, Threading and Packaged-Build Policy](../../adr/018-command-registration-permissions-threading-and-packaged-build-policy.md)
- [ADR-010: Job Waiting and Operation Store Ownership](../../adr/010-job-waiting-and-operation-store-ownership.md)
- [Scene Runtime Architecture](./scene-runtime.md)
- [Asset Pipeline Architecture](./asset-pipeline.md)
- [Gameplay Behavior Authoring](../extensions/gameplay-behavior-authoring.md)
- [Project Versioning and Migration](../foundation/project-versioning-and-migration.md)
- [Editor Document Model](../editor/editor-document-model.md)
