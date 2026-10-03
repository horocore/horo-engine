# ADR-094: Prefab Nested Composition and Variant Inheritance

- **Status**: Proposed
- **Date**: 2026-09-02
- **Supersedes**: None
- **Scope**: Nested prefab placement and single-parent variant edge semantics, stable placement identity, deterministic resolution and precedence, graph validation, source-revision rebase, editor provenance, cook flattening, limits and qualification
- **Issue**: [PFB-005.1](https://github.com/HoroCore/horo-engine/issues/1046)
- **Jira**: [HORO-1046](https://horo-engine.atlassian.net/browse/HORO-1046)
- **Related**: [ADR-017](017-prefab-role-ownership-and-capability-tiers.md), [ADR-093](093-prefab-override-property-identity-and-delta-operations.md)
- **Normative documents**: [Prefab Architecture](../architecture/runtime/prefab-architecture.md), [Editor Document Model](../architecture/editor/editor-document-model.md), [Prefab Editor Reference](../../mock-studio/designs.md#architecture-runtime-prefab-editor), [Project Versioning and Migration](../architecture/foundation/project-versioning-and-migration.md)

## Context

Prefab Architecture permits one prefab to contain repeated placements of another
and defers variant inheritance to Tier 2. ADR-093 gives component, property and
collection overrides stable identity, but intentionally leaves the graph that
supplies those layers undefined. Without a graph contract, a nested reference can
be confused with inheritance, serialized order can accidentally choose precedence,
and multiple variants or construction callbacks can make one asset resolve to
different effective hierarchies.

Nested composition and variants serve different purposes. A nested placement adds
one independently addressable occurrence of a referenced hierarchy beneath an
owning hierarchy. A variant specializes one complete source asset without adding a
second base. A scene instance then customizes the selected effective asset. These
layers must remain separately inspectable even though cook ultimately flattens the
result.

Graph changes also cross document lifetimes. A source revision can arrive while a
variant, containing prefab, scene and viewport are open. Resolving directly into
live documents would allow partial publication, stale async completion and silent
loss of ADR-093 conflicts or opaque data. The production model therefore needs a
bounded, deterministic candidate operation before implementation begins.

## Decision

### 1. Composition has two closed edge kinds

The authoring dependency graph admits exactly two prefab-to-prefab semantic edges:

| Edge | Owner | Cardinality | Meaning |
|---|---|---:|---|
| `NestedPlacement` | Concrete prefab hierarchy | Zero or more, each at a unique persisted placement slot | Mount one effective referenced hierarchy beneath one owning object |
| `VariantParent` | Variant prefab asset | Exactly one | Inherit one complete effective parent and apply one specialization delta |

Ordinary mesh, material, audio and behavior dependencies remain resource edges.
They do not compose prefab objects or participate in variant precedence. A prefab
asset is either concrete or variant in V1. A concrete asset owns authored objects
and nested placements. A variant owns one `VariantParent` edge and one ADR-093
override set; it does not own a parallel hierarchy.

The following are unsupported and fail validation:

- multiple variant parents, mixins, traits or merge bases;
- conditional, tag-, path- or query-selected parents;
- a variant replacing its parent edge according to platform or runtime state;
- construction scripts, callbacks or behavior execution during resolution;
- V1 variant add/remove/reparent of objects or nested placement edges;
- interpreting resource dependencies as prefab composition.

These are capability limits, not hidden extension points. A future hierarchy delta
algebra requires a new versioned decision and migration; unknown graph edge or
operation kinds are preserved by authoring migration where possible but cannot
resolve or cook.

### 2. Every nested occurrence has stable placement identity

A nested placement is authored data:

```cpp
struct NestedPrefabPlacementV1 {
    LocalObjectId placementLocalId;
    std::optional<LocalObjectId> parentLocalId;
    Assets::AssetId sourcePrefab;
    PrefabSourceRevision authoredAgainst;
    Transform localRootTransform;
    PrefabOverrideSetV1 overrides;
};

struct PrefabVariantInheritanceV1 {
    Assets::AssetId immediateParent;
    PrefabSourceRevision authoredAgainst;
    PrefabOverrideSetV1 overrides;
};
```

`placementLocalId` is a persisted nonzero slot in the containing concrete prefab's
single local-ID namespace. It is not an array index, display name, path or referenced
asset ID. Reordering siblings or moving the source asset does not change it, and a
deleted slot is not reused while an override, conflict, orphan, history entry or
external reference may retain it. Repeated placements of the same `sourcePrefab`
therefore remain distinct.

The referenced root is mounted at the placement slot. Descendant addresses extend
ADR-093's `nestedInstanceScope` with `placementLocalId`; their final
`sourceObject` remains the descendant's local ID in its owning source asset.
`parentLocalId` selects the containing hierarchy parent, while
`localRootTransform` is the placement transform. Neither becomes a property path or
an implicit override record.

References carry authoritative `AssetId` and exact authored-against source revision.
Paths and labels are presentation hints only. Validation rejects zero, duplicate or
colliding local slots and a parent that is missing or belongs to the mounted source.

### 3. Resolution is one deterministic recursive function

`ResolvePrefabAsset(asset, snapshot)` returns an immutable effective hierarchy and
provenance table or a typed failure. It has no access to wall-clock time, locale,
filesystem discovery, service locators, runtime entities or gameplay callbacks.

1. Load and migrate the requested document and every reachable document into one
   immutable registry snapshot. Pin `AssetId`, content revision, `ProjectVersion`
   and schema-registry revision.
2. Validate every document and the combined semantic graph before materializing an
   effective hierarchy.
3. For a concrete asset, materialize its authored objects. Resolve each nested
   placement's `sourcePrefab`, apply that placement's ADR-093 override set, then
   mount the result beneath `parentLocalId` using `placementLocalId` as the new
   scope segment.
4. For a variant, resolve its single `immediateParent`, then apply the variant's
   ADR-093 override set to that complete effective parent candidate.
5. For a scene instance, resolve its selected source asset and then apply the
   scene-instance override set.
6. Validate final identity, hierarchy, component, payload and expansion limits and
   publish only the complete candidate.

This algorithm defines precedence without relying on serialized record order. For
any effective value, the oldest concrete source is lowest precedence; referenced
asset variant layers apply oldest-to-newest; the owning nested-placement override
applies after the referenced asset resolves; containing variant layers then apply
when their parent candidate returns; and the outer scene-instance override is last.
An ADR-093 duplicate or incompatible operation in one layer remains an error rather
than last-write-wins.

A variant may reference another variant because each asset still has one immediate
parent. A nested placement may reference a concrete prefab or variant. Both cases
use the same recursive function and provenance rules; neither creates multiple
inheritance.

### 4. The combined graph is bounded and acyclic

Validation builds one graph keyed by `AssetId` and includes both semantic edge
kinds. A deterministic depth-first traversal sorts outgoing nested placements by
`placementLocalId` and reports the first canonical cycle path, including each edge
kind and placement ID. A cycle across kinds, such as `A NestedPlacement B` and
`B VariantParent A`, is still a cycle.

V1 enforces these independent limits:

- at most 8 `VariantParent` edges in one resolution path;
- at most 16 `NestedPlacement` edges in one resolution path;
- at most 256 direct nested placements in one concrete document;
- the existing maximum expanded hierarchy depth, object count, component count and
  payload limits after all inheritance and composition layers apply.

Exact-bound inputs succeed. The next edge or expanded item fails before recursion
or allocation exceeds its budget. Memoization may reuse an immutable effective
asset candidate within the pinned snapshot, but mounting always produces a distinct
placement scope. Cached output cannot erase occurrence identity or provenance.

Missing assets/revisions, unsupported migrated schemas, duplicate placement IDs,
invalid parents, unresolved ADR-093 records, cycles and exceeded limits fail the
candidate. No partial hierarchy, dependency table or cooked artifact is published.

### 5. Provenance keeps layers separately inspectable

Resolution emits bounded provenance beside the effective candidate. Each effective
object and overridden property can identify:

- source asset and source revision;
- semantic edge kind and, for nesting, the complete placement scope;
- concrete, variant, nested-placement or scene-instance layer;
- the ADR-093 override record, conflict or orphan responsible for the value.

The provenance table is editor/build evidence, not runtime entity identity and not
an additional source of truth. The Prefab Editor presents the single-parent chain,
nested placement graph, effective precedence and unresolved conflicts separately.
It never flattens those concepts into an editable anonymous property map.

### 6. Source changes use detached, transactional candidates

A registry change invalidates affected resolved candidates through reverse
`AssetId` dependencies. The application pins the old and new registry snapshots,
re-resolves descendants in deterministic topological order and runs ADR-093
three-way rebase independently for every affected variant, nested placement and
scene instance.

Open documents and viewport projections receive one complete candidate through the
Editor transaction/application boundary. Candidate preparation may run on workers
using owned immutable inputs. Commit rechecks document session/revision, registry
snapshot and cancellation on the owning thread. Failure, cancellation, stale
completion, cycle introduction or any unresolved required record preserves the
previous document, dirty state, history and visible projection. Conflicts and
orphans remain explicit and lossless.

Saving a concrete prefab, variant or scene never edits dependants as an incidental
serializer side effect. Apply-to-source is the ADR-093 multi-document publication
workflow. File watchers only report invalidation; Inspector and graph widgets issue
typed commands.

### 7. Cook consumes the resolved candidate, not the source graph at runtime

Scene cook and prefab cook use the same resolver and flatten the validated effective
hierarchy into their existing runtime definitions. The dependency-aware cache key
includes the resolver/graph schema version, ordered semantic edge kinds and
placement IDs, every reachable source identity/revision/artifact digest and the
canonical override-set digests. A transitive source, edge, placement or override
change therefore invalidates reuse.

Packaged runtime receives flattened typed hierarchy data. It does not parse source
prefabs, traverse `VariantParent`, apply authoring overrides, run construction
scripts or choose a parent dynamically. Provenance retained for diagnostics is
bounded and optional; it cannot authorize runtime mutation.

### 8. Errors and qualification are typed

The authoring/cook surface distinguishes at least `MissingPrefabSource`,
`SourceRevisionUnavailable`, `UnsupportedPrefabSchema`, `InvalidPlacement`,
`MultipleVariantParents`, `CyclicComposition`, `VariantDepthExceeded`,
`CompositionDepthExceeded`, `CompositionEdgeLimitExceeded`, ADR-093
`OverrideConflict`/`OverrideOrphan`, and the existing hierarchy/payload limit
errors. Diagnostics include the owning asset, edge kind and stable placement scope
without using localized text as identity.

Required focused coverage includes:

| Category | Scenario | Expected outcome |
|---|---|---|
| Valid | Concrete A nests the same variant B twice | Two distinct scopes resolve identically except for placement identity/transform |
| Valid | Base -> VariantA -> VariantB, then scene override | Base, oldest-to-newest variants, then scene layer determine the value |
| Valid | Outer variant overrides an object inside its parent's nested placement | Nested source and placement resolve first; outer variant targets the stable nested scope |
| Boundary | Variant depth 8 and nested depth 16 | Exact bounds resolve and cook |
| Boundary | Next variant/nested edge or expanded object exceeds its limit | Typed limit failure before partial allocation/publication |
| Malformed | Duplicate/zero placement ID or missing containing parent | `InvalidPlacement`; old candidate remains active |
| Malformed | A nests B while B inherits A | Canonical combined-edge cycle diagnostic; no artifact |
| Malformed | Variant declares two parents or an unknown hierarchy operation | Explicit rejection; no guessed merge order |
| Malformed | Referenced revision is missing or ADR-093 target is orphaned | Typed unresolved candidate; opaque record retained |
| Lifecycle | Source changes while descendant re-resolution is running | Stale completion cannot replace the newer registry/document snapshot |
| Lifecycle | Rebase is cancelled or one affected document fails | No partial multi-document/viewport publication |
| Cook | Transitive variant, placement or override changes | Canonical cache key changes; runtime artifact contains only flattened data |
| Capability | Construction callback would affect resolved output | Callback is never invoked; unsupported request is rejected |

## Consequences

- Nested composition, variant inheritance and per-instance customization have
  distinct identities and precedence while sharing ADR-093's delta algebra.
- Single-parent variants keep resolution linear at each asset and make provenance,
  rebase and conflict ownership reviewable.
- Combined-edge cycle validation and explicit limits prevent recursion and
  unbounded authoring/cook work before publication.
- V1 variants cannot add, remove or reparent hierarchy objects. That constraint is
  deliberate until a closed, migratable hierarchy delta algebra is accepted.
- Runtime stays independent of authoring graph traversal and construction-script
  behavior at the cost of recooking when any transitive semantic input changes.

## Alternatives Considered

### Treat nested placement and inheritance as the same edge

Rejected because repeated occurrence identity, mounting and transform semantics do
not exist for inheritance, while inheritance precedence does not exist for an
ordinary nested placement.

### Allow multiple variant parents with serialized merge order

Rejected because order becomes semantic ambient state, diamond conflicts gain no
single immediate default, and ADR-093 three-way rebase loses an unambiguous source.

### Run construction scripts while resolving

Rejected because callbacks introduce code availability, side effects, ordering,
threading and determinism into a data-validation/cook boundary.

### Resolve source graphs lazily in packaged runtime

Rejected because it ships authoring schemas and migration policy, increases runtime
failure modes and allows editor/runtime results to diverge.

### Publish each resolved dependant as soon as it finishes

Rejected because users could observe a mixture of source revisions and a failed
later candidate could not restore external or already-observed state atomically.
