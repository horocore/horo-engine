# Destruction And Fracture Architecture

## Purpose

This document defines the destruction and fracture subsystem for Horo Engine.
It covers pre-fractured geometry, runtime fracture, debris generation,
destruction events, physics integration, audio and VFX coupling, and network
replication of destruction state.

[ADR-144](../../adr/144-destruction-ownership-authority-state-and-runtime-geometry-boundary.md)
is the foundation contract. Destruction is a scene-scoped runtime domain with one
semantic state authority, pre-cooked core-1.0 geometry, provider-neutral tiers and
aggregate Scene/Physics/Render publication. Runtime mesh cutting is post-1.0 only.
[ADR-145](../../adr/145-destruction-source-chunk-geometry-collision-and-cook-ownership.md)
defines source normalization, canonical DFR chunk/interior/connectivity artifacts,
solver-neutral collision inputs, Assets cache/publication and separate Physics/Render
derived products.
[ADR-146](../../adr/146-destruction-runtime-activation-physics-cleanup-and-rollback.md)
defines the runtime transaction over those products. Commands and post-step contact
evidence enter at bounded Destruction safe points; Physics prepares exact pre-cooked
chunk bodies privately; RuntimeScene alone exposes semantic, entity, Physics and Render
changes through one aggregate commit. Sleep or visibility never grants cleanup
authority, and rollback preserves the old root only before that commit.
[ADR-147](../../adr/147-destruction-event-and-cosmetic-consumer-ownership.md)
defines what happens afterward. Destruction appends bounded typed facts to its committed
journal; an application-owned dispatcher maps them to gameplay, VFX, Decal, Audio and
accessibility requests at destination safe points. Consumers own mapping realization,
admission, playback/simulation and native lifetime and cannot change DFR success.
[ADR-148](../../adr/148-fracture-document-generator-undo-and-preview-ownership.md)
specializes editor ownership. A persistent fracture asset document owns working source,
typed operations, bounded history and dirty state; generator workers produce detached
candidates; Assets owns durable source/cooked publication; high-fidelity preview uses an
isolated runtime/Physics world and never production state.
[ADR-149](../../adr/149-destruction-persistence-replication-streaming-and-authority.md)
defines canonical save/network reconstruction, explicit server authority, snapshot-first
late join and no-loss streaming handoff. DFR retains semantic sets/revision/seed;
Physics owns paired active-chunk motion; Persistent World owns dormant storage; World
Streaming alone decides residency.

## Ownership

`DestructionApi` owns backend-neutral identities, revisions, descriptors, commands,
results and immutable snapshots. `DestructionRuntime` owns live scene-scoped health,
semantic phase, broken/support membership, command admission, transition planning and
canonical events. RuntimeScene owns entity/binding lifetime and aggregate activation;
Physics and Render own bodies/shapes and render/GPU state; Assets/Destruction Cook own
source, deterministic artifacts and publication; gameplay owns damage permission.

Physics contacts, replicated records, scripts, UI, Audio, VFX and Decals never mutate
destruction state directly. They provide typed evidence/commands or consume post-commit
snapshots/events. Native handles and mutable buffers do not cross the public boundary.

## Identity Contract

`HoroEngine::DestructionApi` owns the backend-neutral identity vocabulary in
`Horo/Destruction/DestructionIdentity.h`. Authored `DestructibleId` and
artifact-local `DestructionChunkId` values are non-zero stable IDs issued by their
owning authoring/import boundary. They survive table reordering and runtime replacement;
names, array positions, paths, entity slots, native body/subshape IDs and pointer values
are never identities.

`FractureAssetId` wraps the Assets-owned path-independent `AssetId`.
`FractureArtifactContentIdentity` combines that stable asset with a non-wrapping durable
content revision and the SHA-256 digest of all canonical semantic cook inputs.
`FractureChunkIdentity` combines exact content with the stable artifact-local chunk ID,
so equal chunk values from replaced or unrelated content cannot alias. A content
replacement either supplies an explicit stable-chunk migration or produces
`StaleContent`; matching array positions do not establish compatibility.

Runtime references use `DestructionHandle { world, destructible, generation }`.
The handle is non-owning and proves neither registry residency nor backing Scene,
Physics or Render lifetime. The scene-scoped Destruction owner issues generations,
increments them on replacement, retirement/recreation and world reload, and permanently
retires an owner on counter exhaustion. Admission checks owner dimensions before the
generation fence, producing `IdentityUnknown` for a foreign owner and
`StaleGeneration` for a retired incarnation.

`DestructionCommandId` binds an owner-issued idempotency value to the exact target
handle. `DestructionEventOccurrenceId` is the deterministic composite of source handle,
committed semantic state revision, closed fact kind and canonical zero-based ordinal
within that revision. It is generated only after aggregate commit; replaying the same
committed journal produces the same value, while distinct canonical facts cannot
collide. Event access additionally rejects `StaleRevision`.

All identity encodings are fixed-width, padding-free network byte order and contain no
native representation. Zero is reserved for invalid scalar identities; decoders reject
an invalid dimension or unknown fact kind. Generation and revision helpers fail on
overflow rather than wrapping. The types and validation helpers are allocation-free and
thread-compatible values; they schedule no work and own no mutable records, so
cancellation, rollback and shutdown have nothing to drain in this API slice. Values may
outlive runtime owners but become unusable when later owner validation reports a stale
generation/content/revision. Live registry mutation, bounded work, synchronization and
shutdown draining remain the DestructionRuntime owner's responsibility.

## Descriptor Contract

`Horo/Destruction/DestructibleDescriptor.h` is the canonical, immutable core-1.0
configuration boundary. A descriptor captures the exact authored destructible and
fracture-content identities, a non-zero configuration publication revision, health
thresholds, typed trigger/support/repair/cleanup/replication intent, required and
optional feature sets, and finite limits. Creation copies fixed-size data only; it
does not load content, discover a provider, allocate runtime state, register a callback
or grant mutation authority. The value is thread-compatible and may be dropped on any
thread without shutdown work.

Runtime admission rechecks the submitted/current handle generation, authored owner,
configuration revision and content identity before validating decoded artifact counts
and peak bytes/work. Invalid or over-limit content fails before allocation or partial
publication. Required features must exist in the exact selected tier. Optional features
are intersected with tier support and remain absent when unsupported; there is no
substitution or tier fallback. Runtime geometry generation is a typed unsupported
request in every core-1.0 tier.

## Destruction Model

### Registry, query, and capability boundary

`Horo/Destruction/DestructionRegistry.h` defines the explicit discovery boundary.
One host-owned `DestructionRegistry` copies current state and capability projections into
fixed-capacity storage. Registry membership is not destructible residency or lifetime:
register, replace, remove, and shutdown never create, mutate, retain, or destroy a
destructible, artifact, Physics body, Render resource, authority grant, or backend object.
There is no global registry or service-location path.

Consumers retain `DestructionRegistrySnapshot` values. Each snapshot is immutable,
revisioned, sorted by the complete generation-safe handle, and remains readable after
registry replacement or shutdown. Exact lookup distinguishes an unknown authored owner
from a stale generation. Queries require an exact world, explicit finite result bound,
optional closed phase, and required-feature set. They perform fixed work, copy at most the
requested bound, and report `HasMore` rather than silently widening or substituting a
feature profile. Capability queries return fixed-size value evidence tied to the registry,
state, and capability revisions; the evidence grants no command authority and transfers
no feature ownership. Live operations must still revalidate those revisions at their
owning admission boundary.

Scene components carry stable binding and authored policy, not live mutable state:

```cpp
struct DestructibleSceneBinding {
    DestructibleId destructible;
    FractureAssetId fractureAsset;
    DestructionPolicyId policy;
    DestructionFeatureRequirements requiredFeatures;
};
```

`DestructionWorld` owns the foundational `Intact`, `Damaged` or terminal `Destroyed`
health state plus exact health, broken chunk set and support state under one monotonic
state revision. `Destroyed` records terminal semantic health only; exact broken,
detached, dormant and supported chunk membership cannot be inferred from the phase.
The independent runtime lifecycle is `Absent`, `Preparing`, `Prepared`, `Active`,
`Replacing`, `Suspended`, `Retiring` or `Failed`.

`Horo/Destruction/DestructionStateMachine.h` defines this narrow revisioned contract.
Commands bind an idempotency value to the exact runtime generation and expected state
revision. Preparation produces an immutable detached candidate, and the single
DestructionRuntime owner compares generation and revision again at its safe-point
commit. New command values advance monotonically within a generation, so bounded state
retains a high-water mark instead of an unbounded command-ID set. Two candidates
prepared from one revision therefore cannot both advance it; an exact latest-command
retry is a no-op, while conflicting or older command reuse is typed failure. Cancelling
or discarding a candidate is rollback because preparation never mutates the active
snapshot. Replacement admits only the next generation and resets its state at revision
one, invalidating all previous completions. Shutdown closes preparation, commit and
replacement admission without discarding the last published snapshot.

## Pre-Fractured Geometry

Pre-fractured meshes are authored and cooked offline through ADR-145:

- Assets owns immutable normalized source bytes, dependency scheduling, physical cache,
  package storage and atomic publication
- Destruction Cook owns fracture recipes, deterministic generation/import validation,
  canonical chunk/exterior/interior geometry and connectivity
- The canonical DFR artifact owns solver-neutral convex input with stable chunk/subshape
  identities, not native Physics shapes
- Physics separately cooks solver/profile/platform-specific immutable shape artifacts
  and owns runtime shape storage/retirement
- Mesh/Render separately lower canonical chunk geometry and own GPU resources

```cpp
struct CanonicalFractureArtifact {
    FractureArtifactContentIdentity content;
    FractureIntegrityDigest integrityDigest;
    AssetId sourceMesh;
    FractureRecipeId recipe;
    FractureChunkTable chunks;
    FractureConnectivityGraph connectivity;
    FractureGeometryTables geometry;
    FractureCollisionInputTable collisionInputs;
    LocalBounds bounds;
    MassIntegrationInputs massProperties;
    float volume;
};
```

Stable chunk identity is not an array index, source/display name, cache path, render
submesh or native shape ID. DFR owns canonical interior classification, winding,
material slots and UV policy. Render cannot reclassify faces, and Physics cannot change
chunk membership while realizing collision.

The DFR cook fingerprint includes normalized source/recipe/dependency digests,
algorithm/version/seed, coordinate/tolerance/repair policy, interior/material/UV and
connectivity rules, selected tier/limits and artifact/toolchain schemas. Physics and
Render add their own native target fingerprints to the accepted DFR artifact identity;
a solver/backend upgrade invalidates its derived product without changing DFR topology.

The DFR-002.3 offline Voronoi cook entry point accepts a closed, outward-wound
normalized source, exact source digest/revision and recipe/toolchain provenance. It
partitions a non-convex source along its deterministic surface-plane arrangement into
bounded convex regions, then clips each region against every ordered site bisector.
The result contains closed solver-neutral convex collision pieces under stable semantic
chunk IDs, visible exterior/interior faces, per-face material slots, volume/center-of-
mass inputs and site-neighbor connectivity. If the finite region, work or output budget
cannot hold the complete result, generation fails before publication. The detached
candidate is for later Assets publication and separate Physics/Render derived cooking;
runtime composition has no call into this cook entry point.

## Runtime Pre-Cooked Fracture

Runtime fracture can be triggered by:

- **Impact**: Collision with sufficient force/momentum
- **Damage**: Accumulated damage reaches threshold
- **Explosion**: Radial damage with falloff
- **Script**: Explicit fracture command from gameplay

```cpp
struct FractureEvent {
    FractureEventType    type;
    WorldCoordinate      impactPoint;
    Vector3              impactDirection;
    float                damage;
    float                radius;
};
```

Every trigger becomes an authority-, generation- and revision-checked typed command.
Physics contacts are immutable evidence consumed after the Physics step; callbacks do
not fracture objects directly. On a successful pre-cooked transition:

`Horo/Destruction/DestructionCommand.h` owns the portable command envelope for this
boundary. Impact, explosion, collision, direct damage and script sources retain fixed-
size Horo-space payloads plus exact command, capability-snapshot and authority-grant
revisions. Validation is allocation-free and rejects malformed, stale, unauthorized,
unsupported or over-limit input before owner state or a bounded queue changes. Script
commands require both script-origin authority and their typed damage or explicit-fracture
capability. Script fracture lowers only to pre-cooked destruction; it does not grant
runtime geometry generation.

Every accepted operation ends in a durable typed result. `Succeeded`, `Rejected`,
`Cancelled`, `Unsupported` and `Failed` remain separate values with a closed terminal
reason, command identity and source/terminal revisions. Replacement, stale completion
and shutdown produce rollback-requiring non-success results; they never promote a
private candidate or infer disposition from diagnostic text. Consumers migrating from
ad-hoc event structs must submit these typed commands and branch on the terminal enums,
not backend status codes or strings.

1. Destruction validates command/evidence generation and computes the exact cooked
   direct-detach plus unsupported-chunk closure and complete peak cost.
2. Physics and Render prepare required chunk representations privately under one
   transition ticket; ordinary queries still resolve the previous root.
3. Physics publishes prepared bodies into ticket-scoped routing only at its pre-step
   safe point, without making them public.
4. RuntimeScene commits semantic state, intact/chunk visibility, entities and required
   bodies through one activation-ticket-scoped aggregate root.
5. Canonical destruction facts append to the bounded queryable journal only after
   commit.
6. The application dispatcher maps committed facts to gameplay/network/save adapters
   and optional VFX, Audio, Decal and accessibility requests at owner safe points.

Failure or cancellation before commit preserves the intact/previous generation. Old
representations remain leased and charged until every consumer acknowledges retirement.

Core 1.0 never cuts, booleans, voxelizes, remeshes or triangulates source geometry; it
never creates interior surfaces, convex decomposition or mass properties at runtime.
Those products must exist in the validated fracture artifact. Missing/incompatible
content returns a typed failure instead of runtime generation or visual-only fallback.

## Debris System

Small debris particles are handled by the VFX system:

```cpp
struct DebrisSettings {
    AssetId     debrisMeshId;        // small generic debris mesh
    uint32_t    minParticleCount;
    uint32_t    maxParticleCount;
    float       initialVelocity;
    float       lifetime;
    float       fadeOutDuration;
    AssetId     impactDecalMaterial; // scorch/damage decal at impact point
};
```

Debris particles use a lightweight physics simulation (no collision between
debris particles, only against world geometry). They fade out after their
lifetime and are recycled from a debris pool.

## Hierarchical Fracture

Large structures fracture in stages:

1. Initial fracture: surface chunks break off
2. Structural collapse: when enough structural chunks are removed, the
   remaining chunks lose support
3. Secondary fracture: falling chunks fracture on impact with the ground

The chunk connectivity graph determines structural dependency:

```cpp
struct FractureChunkGraph {
    std::vector<std::vector<uint32_t>> adjacency;   // chunk index → neighbor indices
    std::vector<float>                 supportWeight; // how much this chunk supports others
};
```

Core 1.0 support loss uses deterministic graph reachability, not a stress solver.
After directly selected chunks are removed, the planner traverses the remaining cooked
graph from immutable anchors in stable chunk/neighbor order. Every remaining chunk not
reachable from an anchor joins the detach set. The complete result must fit active
limits or the transition fails atomically; it is never truncated. Damage accumulation
and threshold rules are separate from this closure step.

## Physics Integration

Fracture chunks use the physics system:

- Each activated chunk becomes a dynamic rigid body
- Chunk collision shapes use convex decomposition (pre-computed in the
  fracture asset)
- Initial velocities are derived from the fracture event
- Bodies prepare privately from the exact DFR/Physics shape artifact and become query-
  visible only with the aggregate RuntimeScene commit
- Contact callbacks collect bounded immutable evidence; they do not mutate Destruction,
  Scene structure or body topology
- Physics sleep remains solver state and does not grant cleanup authority
- Gameplay-authoritative and durable chunks retire only through explicit policy and,
  where required, a successful Runtime Save/Persistent World handoff
- Cosmetic debris is VFX-owned, finite-lived and excluded from canonical chunk state

## Event And Consumer Integration

`DestructionWorld` publishes immutable facts with stable world/destructible generation,
state revision, transition ticket, tick, kind and occurrence identity. Facts describe
committed damage, chunk activation, support loss, dormancy, reactivation or availability;
they do not name effect graphs, decal materials, audio media/voices or callbacks.

The application composition root owns the cooked semantic binding table and one
session-scoped `DestructionEventDispatcher`. It reads the bounded Destruction journal by
cursor after aggregate commit, fans out in cooked order and copies fixed-capacity values
into destination-owned queues. Required gameplay/accessibility delivery capacity is
reserved before the source transition commits. Optional cosmetic capacity failure is a
typed destination outcome and never rolls back canonical state.

VFX owns effect mapping realization, queue/pool admission, simulation and Render
extraction. The Decal presentation owner owns projection, attachment, lifetime and
retirement. Audio owns cue/media readiness, sample scheduling, voices, mixing and device
callbacks. Gameplay adapters may submit later domain commands but cannot reenter the
source commit. Every layer derives its request identity from the DFR occurrence, binding
generation, destination and layer ordinal so retries/reload cannot duplicate effects.

`EngineDataBus` may publish a coalesced journal-revision notification, not the required
fact payload. Save restore and late join reconstruct canonical state and do not replay
historical effects by default; prediction uses a separate cosmetic occurrence namespace.

## Network Replication

Destruction state is replicated through the normal authority and replication boundary:

- The authority server alone commits canonical destruction commands/state; autonomous
  and simulated clients hold replicas and cannot gain authority from contact/visibility
- NetworkRuntime captures versioned bounded snapshots/deltas and delivers typed apply
  commands; I/O threads do not mutate Destruction
- Save/network snapshots bind exact artifact content/chunk-table identity, semantic
  revision, deterministic seed, health/phase and broken/active/supported/dormant sets
- Active authoritative chunk motion is paired Physics-owned canonical state keyed by
  stable DFR chunk identity; native solver state is never serialized
- Late join receives a full semantic/Physics snapshot at one revision/tick, then applies
  a bounded contiguous delta stream through aggregate activation
- Gameplay-authoritative chunk motion uses existing Physics/network state replication;
  Destruction does not create a second transform stream
- Cosmetic chunks/debris may simulate client-side but cannot affect gameplay, saves or
  canonical hashes

## Editor Authoring

Fracture authoring tools:

- Fracture mesh import (import pre-fractured FBX with naming convention)
- Voronoi fracture generator (generate chunks from intact mesh in-editor)
- Fracture preview (play fracture animation in editor viewport)
- Chunk connectivity visualization
- Damage threshold and behavior configuration

The DFR-002.2 importer reads an FBX mesh occurrence named
`HoroChunk_<nonzero decimal ID>` with an optional `__display_label` suffix.
The numeric token is an explicit authored semantic chunk ID; the display label,
FBX element position and source path are not identities. The token must be
canonical decimal without leading zeroes and unique within the source. Mesh
ancestors define the imported chunk hierarchy. Assets copies normalized
world-space geometry, transform evidence and material assignment into a bounded
detached source under normalization schema 1. Destruction Cook schema 1 validates
the complete closed triangle topology,
IDs, hierarchy, finite transforms, material coverage and profile limits before
returning a sorted detached candidate. Import does not infer absent IDs, weld
surfaces, invent materials, publish an artifact, or activate a runtime world.

Each asset opens as one persistent `FractureAssetDocument` rooted at stable asset and
accepted source revision. The document owns working recipe/source/graph intent, typed
operation execution, history, dirty/saved state and derived candidate/preview status.
Panels, inspectors, tree/graph views and viewport gizmos own presentation/input only and
cannot mutate source, history or artifact publication directly.

Import and procedural generation capture an immutable exact-revision input snapshot and
write a bounded detached candidate in a document-owned cancellable operation. Completion
does not dirty or publish. Only an explicit accept operation may atomically apply the
candidate's authored semantic patch and record exact before/after history. Undo/redo
replays those patches or immutable source-section checkpoints; it never reruns the
generator or stores production cooked/native products.

Source save publishes through Assets and alone advances saved state. Production cook is
a separate Assets-owned ADR-145 generation. Preview cook is transient and activates the
ordinary DFR/RuntimeScene/Physics/Render contracts inside a generation-fenced
`FracturePreviewSession`; its Physics world, events, persistence-disabled state and
resources are distinct from production. Preview observations write back only through a
new explicit document operation.

## Feature Tiers

Tiers are provider-neutral product preference profiles. They do not name a graphics API,
platform, solver or device and do not grant capability:

| Feature family | `Baseline` | `Standard` | `High` |
|---|---|---|---|
| Pre-cooked fracture | 64 chunks, depth 1 | 256 chunks, depth 4, staged activation | 1,024 chunks, depth 8, staged hierarchical fracture |
| Active chunk bodies | 64 | 256 | 1,024 |
| Events / retained journal | 128 / 512 | 1,024 / 4,096 | 4,096 / 16,384 |
| Cosmetic debris particles | 256 | 1,024 | 4,096 |
| Artifact / transition / resident bytes | 16 / 32 / 64 MiB | 64 / 128 / 256 MiB | 256 / 512 / 1,024 MiB |
| Work items per transition | 8,192 | 65,536 | 524,288 |
| Runtime geometry generation | Unavailable in core 1.0 | Unavailable in core 1.0 | Unavailable in core 1.0 |

These are hard upper profiles, not reservations or inferred platform capability. A
descriptor may lower every value while preserving internal consistency. Resolution
intersects product request, cooked variants, runtime, World Streaming, Physics, Render
and host capabilities. Required unsupported content fails; fallback is only an
explicitly ordered product choice with a typed reason. Runtime geometry is a separate
post-1.0 capability, not an automatic `High` feature.

### Product-profile composition

The application composition root selects exactly one backend-neutral destruction
product profile: `Null`, `Headless`, `Editor`, `Standalone`, `Client` or `Server`.
It publishes an exact immutable availability fact for Physics, VFX, Audio and
Networking, then resolves the profile through `DestructionComposition`. The result
contains fixed-size decisions and evidence revisions only; it owns no service,
callback, native handle or mutable registry.

Required unavailable capabilities reject the exact profile. Optional unavailable
capabilities remain explicitly `Unavailable`, and capabilities excluded by a profile
remain `Omitted` even when their implementation exists in the host. Resolution never
installs a provider, upgrades or downgrades a tier, substitutes another profile, or
discovers ambient state. `Null` disables destruction explicitly. Headless/server
profiles omit presentation-only VFX and Audio, while client/server profiles require
Networking. Admission revalidates the complete composition revision and active
lifecycle, so replacement, cancellation and shutdown fence captured work.

## Lifecycle, Persistence And Shutdown

Destruction builds transitions as detached candidates with exact affected chunks,
consumer requirements and peak reservations. Workers validate immutable artifacts only;
the owner lane commits state. Physics and Render may publish prepared resources only to
ticket-scoped private routing. Stale revision/generation, authority loss, budget denial,
consumer failure or cancellation changes nothing before aggregate publication; private
resources retire after owner-safe readers drain.

Aggregate commit is the rollback boundary. Before it, failure preserves the complete old
root. After it, restore, dormancy or recovery is a new authorized command/revision rather
than an in-place rewind. A required consumer loss exposes typed suspended/failed
availability and never fabricates an intact state.

Runtime Save captures stable destructible/chunk identities, content/state revisions,
health, broken/support/dormant sets and required progress. Native bodies, shapes, GPU
resources, particles, voices, contacts and caches are derived/excluded. World Streaming
cannot evict the last durable state before the persistence owner accepts its handoff.
Sleeping, old or invisible chunks are not cleanup-eligible by implication. An explicit
bounded policy chooses canonical dormancy, and required durable state must be accepted
before live Scene/Physics/Render representations retire through another aggregate commit.
Persistent World stores dormant DFR semantic snapshots by stable world/cell/destructible
and exact content/revision identity, but does not interpret them. World Streaming owns
residency and may retire the last live copy only after exact-revision handoff succeeds.
Restore and later cell activation apply saved/dormant state before the aggregate root;
content mismatch requires a registered stable-chunk migration or typed rejection.

Shutdown closes admission, cancels task groups, invalidates candidates, requests exact-
generation consumer retirement and retains artifacts/reservations/modules until every
reader, job and native owner acknowledges release. A deadline reports incomplete
shutdown and never force-frees possibly referenced state.

## Verification

Required coverage includes authority/revision checks, every semantic/lifecycle
transition, deterministic contact ordering, cooked chunk closure, proof that core paths
perform no runtime geometry/collision cook, aggregate consumer rollback/replacement,
capacity and durable cleanup, save/restore, late join, authoritative/cosmetic motion,
headless/Null/all interactive backends, committed fact ordering, journal gaps, consumer
dedup/overload independence, typed editor operations, exact history, stale generator
work, preview isolation, cancellation and repeated shutdown.

## Related Documents

- [Destruction Setup UI Reference](./destruction-setup.html)
- [Destruction Product Composition Migration](../../guides/destruction-product-composition-migration.md)

- [Physics Architecture](./physics-architecture.md): fracture chunk physics
- [VFX And Particles Architecture](./vfx-and-particles-architecture.md): debris particle spawning
- [Audio Architecture](./audio-architecture.md): destruction sound events
- [Decal System Architecture](./decal-system-architecture.md): impact decals
- [Networking Architecture](./networking-architecture.md): destruction state replication
- [Scene Runtime](./scene-runtime.md): destructible component model
- [Asset Pipeline](./asset-pipeline.md): source, cook, cache and artifact publication
- [Save Game And Persistence](./save-game-and-persistence.md): canonical destruction
  state capture and durable dormancy
- [ADR-144](../../adr/144-destruction-ownership-authority-state-and-runtime-geometry-boundary.md):
  module/state authorities, command ordering, cooked activation, tiers, persistence,
  replication and post-1.0 runtime geometry boundary
- [ADR-145](../../adr/145-destruction-source-chunk-geometry-collision-and-cook-ownership.md):
  normalized source/recipe inputs, canonical DFR chunk/interior/connectivity artifact,
  solver-neutral collision inputs and separate Physics/Render derived products
- [ADR-146](../../adr/146-destruction-runtime-activation-physics-cleanup-and-rollback.md):
  command and contact safe points, deterministic support loss, private Physics body
  preparation, aggregate publication, cleanup, rollback, replacement and shutdown
- [ADR-147](../../adr/147-destruction-event-and-cosmetic-consumer-ownership.md):
  committed fact identity/journal, application dispatch, destination safe points,
  deduplication and gameplay/VFX/Decal/Audio ownership
- [ADR-148](../../adr/148-fracture-document-generator-undo-and-preview-ownership.md):
  persistent document, typed operations, detached generation, exact bounded history,
  Assets publication and isolated preview ownership
- [ADR-149](../../adr/149-destruction-persistence-replication-streaming-and-authority.md):
  canonical save/network state, server authority, paired Physics motion, late join,
  durable streaming handoff and compatibility
