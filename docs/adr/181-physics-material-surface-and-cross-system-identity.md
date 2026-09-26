# ADR-181: Physics Material, Surface and Cross-System Identity

- **Status**: Proposed
- **Date**: 2026-09-25
- **Scope**: Physical material identity, optional semantic surface identity, collider/slot mapping, committed evidence, consumer ownership, lifecycle and compatibility
- **Issue**: [PHY-006.7](https://github.com/HoroCore/horo-engine/issues/893)
- **Jira**: [HORO-893](https://horo-engine.atlassian.net/browse/HORO-893)
- **Parent**: [PHY-006](https://github.com/HoroCore/horo-engine/issues/833)
- **Related**: [ADR-085](085-physics-shape-authoring-cook-and-runtime-boundary.md), [ADR-087](087-scene-to-physics-ownership-and-conversion.md), [ADR-089](089-character-controller-ownership-implementation-and-update-order.md), [ADR-091](091-footstep-and-locomotion-event-ownership.md), [ADR-147](147-destruction-event-and-cosmetic-consumer-ownership.md)
- **Normative documents**: [Physics Architecture](../architecture/runtime/physics-architecture.md), [Character Controller Architecture](../architecture/runtime/character-controller-architecture.md), [Audio Architecture](../architecture/runtime/audio-architecture.md), [VFX and Particles Architecture](../architecture/runtime/vfx-and-particles-architecture.md)

## Context

`PhysicsMaterialAssetDescriptor` already gives a persistent `PhysicsMaterialAssetId`,
source revision, friction, restitution, density and independent combine policies. It
deliberately has no gameplay, acoustic, renderer or solver-native field. Authored
`ColliderComponent::materials` maps `PhysicsMaterialSlotId` to an asset identity;
triangle mesh cook retains slots. Query hits and contact records currently copy
optional physical asset/generation/slot evidence. Character's implemented descriptor
has a required `PhysicsQueryMaterial` fallback, while its architecture still sketches
one combined `SurfaceMaterial` containing physical coefficients and sound/VFX assets.
ADR-091 expects a non-null `SurfaceMaterialId` for committed grounded locomotion.

These facts need a stable bridge. A physical material may appear as several game
surfaces, and several physical materials may present the same surface. An effect or
sound asset can also change without altering friction. Neither native solver IDs nor
render material IDs can be durable cross-system keys.

## Decision

### 1. Distinct identities and authorities

| Identity | Authority | Meaning and lifetime |
|---|---|---|
| `PhysicsMaterialAssetId` | Asset/Physics material domain | Persistent typed asset ID for physical coefficients and combine policy; source revision selects immutable content. |
| `PhysicsMaterialSlotId` | Shape/collider authoring and cook | Stable nonzero slot within the owning collider/shape, including a cooked subshape; it is not a project-global material ID. |
| `SurfaceMaterialId` | Project surface catalog | Persistent, nonzero, opaque typed UUID for gameplay surface meaning; stable across display rename, asset move and consumer retargeting. |
| Runtime body/shape/world handles | Physics world | Generation-checked process-local ownership and query evidence; never serialized as surface identity. |

`SurfaceMaterialId` is a proposed narrow backend-neutral value contract shared by
scene, Character and application consumers; no such type or catalog ships today.
This ADR reserves a future committed `.horo/surfaces.json` project catalog as the
single authority, with project identity, schema version, unique surface IDs, display
metadata and optional typed gameplay tags. The Project Model's portable-file list,
project-open validation and migration chain must be extended when that schema is
implemented. The catalog contains no physical coefficients or Audio/VFX asset
references. This new ID deliberately adopts the canonical lowercase RFC 9562 UUID
wire form already used by `AssetId` and ADR-086 collision IDs, while remaining a
distinct typed identity rather than an `AssetId` sidecar reference. Zero, malformed
and duplicate IDs fail validation; unknown schema versions fail with a migration
diagnostic.
The catalog is not a solver registry or a service locator. Display text, `SurfaceType`
names, collision layers, navigation `SurfaceId`, renderer/VFX material asset IDs and
native material indexes are different domains and never convert implicitly.

The physical asset remains the sole source for friction, restitution, density and
combine policy. A surface catalog edit cannot change a contact response. Consumer
binding assets owned by Gameplay/application map `SurfaceMaterialId` plus explicit
game context to typed gameplay effects, Audio cue IDs or VFX effect IDs. Those
bindings may have independent revisions and optional destinations. Physics and
Character never store media/effect asset IDs or choose presentation policy.

### 2. Explicit collider-slot mapping

The authored `ColliderComponent` supplies a required physical binding for each
used material slot in its existing `materials` list. A future versioned component
schema adds an optional `surfaceBindings` list of
`(PhysicsMaterialSlotId, SurfaceMaterialId)` values.
The semantic binding is a separate typed field, not a new field in
`PhysicsMaterialAssetDescriptor` and not an inferred mapping from asset name,
render mesh, renderer material, collision layer or physical coefficients. The
normalized key is the exact scene object + collider slot + shape subresource (when
present) + `PhysicsMaterialSlotId`. The same physical asset may therefore be bound
to distinct surface IDs on different colliders; multiple physical assets may share
one surface ID. Compound and mesh child mappings retain the exact authored slot
and subshape provenance through cook and runtime projection.

Scene/asset authoring may offer a reusable default surface suggestion, but the
committed collider binding is the authority. An omitted semantic binding means
`None`, not an implicit default. Empty, duplicate or unknown slot bindings, missing
physical assets, and nonzero surface IDs absent from the captured project catalog
are typed admission failures. A sensor may carry the same optional semantic
binding for overlap facts, but a sensor never gains a physical response merely
because it has one. A shape with no resolvable physical binding is rejected under
ADR-087; it does not silently acquire a native default.

The scene/cook persistence schema must version this additive optional binding.
Old scenes without the field decode as `None` and preserve their existing physical
bindings. Import/migration cannot infer `Metal`, `Wood` or any other surface by
display name. Editors may present an explicit conversion operation; conversion
must remain reviewable and undoable. Runtime activation never rewrites authored
files or adds a second source of truth.

### 3. Resolution and publication boundary

The Physics scene-plan builder consumes an immutable scene snapshot, exact physical
asset revisions and project surface-catalog revision. It validates the entire
collider/slot mapping before native construction. Asset delivery retains physical
leases for the candidate world; the plan retains only Horo IDs, revisions and a
bounded compiled mapping. The private solver adapter receives physical contact
properties only. It cannot see surface catalog definitions or consumer bindings.

The mapping is activated with the complete scene/Physics bundle under ADR-087.
Each published world has one mapping generation and catalog fingerprint. Replacing
an asset, mapping, shape or catalog builds a candidate and swaps at the declared
safe point/aggregate scene commit. Failure leaves the previous complete mapping
and world visible. Old revisions retire after in-flight steps, queries and copied
event readers drain. Unload closes admission and invalidates the mapping generation;
an old asset ID or slot cannot alias a new world.

Current `PhysicsQueryMaterial` and `PhysicsEventMaterial` remain physical evidence:
asset ID, generation and slot. They do not become semantic IDs merely by renaming.
The published Horo-side projection of a hit/contact may additionally carry
`optional<SurfaceMaterialId>` with exact world, scene and mapping generation, tick
or query snapshot, collider and subshape provenance. A missing physical material
evidence value remains missing; a valid physical material with no surface binding
has an explicit absent semantic value. Producers copy values before leaving the
solver callback; no catalog lookup or consumer callback occurs there. The
projection rejects stale/foreign generations rather than consulting the latest
catalog and changing the meaning of an old result.

Contact endpoints each keep their own physical and optional surface evidence.
No combined contact coefficient or winner is treated as the semantic surface of
both sides. A caller that needs one surface, such as a grounded Character, selects
the supporting collider endpoint using its declared contact/grounding policy.
Ambiguous or missing support cannot be resolved by callback order or by picking
the first non-null ID.

### 4. Character, Gameplay, Audio and VFX contracts

Character consumes same-world, generation-checked Horo query evidence and publishes
the selected support's physical material and `SurfaceMaterialId` in its bounded
committed snapshot. Its descriptor carries an explicit project-valid default
`SurfaceMaterialId` for the case where support exists but the collider has no
semantic binding. This is separate from the existing physical
`PhysicsQueryMaterial` fallback, which remains the physical query fallback. If
support is absent/stale, Character does not invent a ground surface. The descriptor
is rejected at admission if its required default ID is missing from the captured
catalog. `SurfaceChanged` compares semantic IDs under the same committed generation;
a physical asset revision change alone is not a surface change.

ADR-091's non-null grounded `SurfaceMaterialId` refers to this selected-or-explicit-
default semantic value. The post-commit locomotion adapter joins the exact marker,
Character tick and mapping generation, then applies a captured Gameplay/application
binding revision. Gameplay may use the same semantic ID for deterministic rules at
its permitted simulation boundary; cosmetic mapping or availability never changes
that committed rule result. Audio receives cue intent; VFX receives effect intent.
Neither subsystem queries Physics, reads physical coefficients to infer a surface,
or owns the project surface catalog. A missing optional cue/effect or absent consumer
is an explicit no-op with bounded diagnostics, not a simulation failure.

Other consumers, including impact and destruction dispatchers, use the same typed
semantic evidence and their own application binding tables. They choose an endpoint
and effect policy explicitly. No global `physical material -> audio/VFX` lookup is
added to Physics, and no render material asset doubles as a gameplay surface.

### 5. Thread, platform, failure and compatibility policy

Authoring/catalog validation and cook may run as cancellable background work over
captured immutable revisions. World preparation, mapping activation and structural
replacement are Physics-owner-thread operations outside a step. Native callbacks
only copy bounded Horo evidence. Post-step/after-commit consumers read immutable
generation-fenced values; Audio's callback and VFX/render threads never dereference
Physics, scene or catalog storage. No steady-tick asset lookup, allocation, blocking
I/O or cross-thread mutable registry is required. Capacity for compiled mappings
and copied results is admitted before publication; overflow or conflicting mapping
fails the candidate with a stable typed error and source location.

The serialized identities and Horo projection have the same meaning on qualified
desktop and headless Physics compositions. The private solver's material index and
contact-combine implementation are platform-specific implementation details. A
`Null` or omitted Physics composition reports its capability as unavailable; it
does not fabricate physical contacts or semantic surface evidence. Existing
CanonicalV1 platform and deterministic-fingerprint limits remain in force. Cooked
mapping/schema revisions and relevant asset/catalog revisions join compatibility
checks for replay, checkpoint and package admission; mismatches fail explicitly
instead of replaying a prior surface with a new binding.

This ADR is a contract decision, not a claim that semantic projection and catalog
types already ship. Implementation must add the narrow typed model, versioned
persistence, query/contact projection, Character default and consumer bindings in
their owning tickets, with migration and regression coverage. Until then, callers
may use the implemented physical evidence only; no ad hoc string or native bridge
is an interim surface contract. Procedural surface inference from texture pixels,
runtime mesh names or coefficient thresholds is unsupported.

## Consequences and trade-offs

The extra catalog and per-slot binding make authoring and activation validation
more explicit, and mapping replacement needs one more generation-fenced snapshot.
They allow sound/VFX/gameplay policy to change independently of physical response
and let one physical asset participate in multiple semantic surfaces. Content that
omits a surface remains valid for Physics but needs an explicit Character fallback
for grounded locomotion presentation.

## Rejected alternatives

- **One universal `SurfaceMaterial` asset holding coefficients and cue/effect IDs.**
  Simpler authoring, but makes physical cook/replay identity change with cosmetic
  edits and forces headless Physics to depend on Audio/VFX policy.
- **Map only by `PhysicsMaterialAssetId`.** Convenient global table, but cannot
  distinguish differently presented uses of the same material on two colliders or
  preserve exact mesh subshape provenance.
- **Let consumers inspect Jolt material IDs, names or render materials.** Avoids
  a Horo mapping table, but breaks backend neutrality, persistence, headless use
  and generation safety.
- **Late live catalog lookup for each event.** Easier initial wiring, but a reload
  could reinterpret an already committed tick and callback work would become
  unbounded and thread-sensitive.

## Qualification required for implementation

Cover one-to-many and many-to-one bindings, mesh/compound subshape selection,
missing/duplicate/unknown IDs, old-scene `None` migration, explicit Character
fallback, no-support suppression, same-tick evidence, asset/catalog reload,
candidate rollback, unload and stale generations. Verify headless/Null behavior,
bounded callback copies, allocation-free steady ticks, deterministic ordering and
consumer absence/failure without changing simulation results.
