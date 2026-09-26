# Procedural Generation Architecture

## Purpose

This document defines the procedural content generation (PCG) subsystem for
Horo Engine. It covers PCG graphs, node-based generation pipelines, spatial
queries, point-based generation, runtime vs offline generation, and editor
authoring tools.

[ADR-151](../../adr/151-pcg-ownership-authority-tier-and-lifecycle.md) is the
normative owner of subsystem authority, execution modes, determinism classes, feature
tiers, headless/null composition and lifecycle. [ADR-150](../../adr/150-pcg-graph-source-cooked-plan-cache-and-runtime-ownership.md)
owns graph source, cooked-plan, cache, evaluation-intermediate, publication,
replacement and destruction boundaries. The structures below describe domain
semantics; they do not authorize runtime source interpretation or direct scene
mutation from graph nodes.

## Authority And Artifact Model

PCG separates four representations with different owners and lifetimes:

1. `PCGGraphSource` is versioned authored intent. Assets owns its durable identity,
   bytes and revision; the PCG document owns only an unsaved working candidate.
2. `CookedPCGPlan` is the one immutable executable representation. PCG Cook lowers a
   closed source/dependency/node-library snapshot while Assets owns scheduling, cache,
   staging and atomic generation publication.
3. Evaluation intermediates are bounded operation-local point sets, attributes and
   scratch. They are disposable and never become source or a global cache.
4. Generated outputs are typed immutable candidates. RuntimeScene, Terrain/Foliage,
   Physics, Render, Navigation or another target subsystem owns committed state after
   an explicit host-coordinated transaction.

Offline bake, live preview and runtime generation evaluate the same validated cooked
plan. Runtime never parses graph source, invokes a compiler, repairs a plan or chooses
a cache entry as active content.

`PCGPointCloudWorkspace` implements the operation-local point-column boundary.
Admission takes the exact cooked plan and a maximum record count and immutable schema
for every PointSet output pin. It derives last readers from canonical routes, proves
the peak simultaneously live record count, and reserves typed contiguous column
buffers and metadata within the tier scratch slice before evaluation. Outputs with
no downstream route remain final until the operation ends. A caller replacing an
operation supplies the still-charged old workspace bytes so old/new overlap is
admitted before the new buffers are allocated.

One owner thread advances nodes in cooked order. The current node writes only its
own bounded columns through guarded value setters; sealing validates every value
and revokes copied writers before downstream reads. An output buffer may be assigned to a later output
only after its last routed reader has finished. Fan-out therefore retains one
immutable source buffer through all consumers. Borrowed read/write views expire at
their documented stage transitions; cancellation closes access, and destruction
releases every slot. A new workspace is required for a new evaluation, source
revision, or replacement. The cooked plan exposes its captured operational tier for
this admission; its portable byte format and compiler version are unchanged.

### Graph Source Schema 1.1

`PCGGraphAsset` is the implemented bounded semantic source value. It owns stable graph,
node, graph-global pin, edge and exposed-input identities; exact node-type versions;
typed pin direction/cardinality/value contracts; an explicit Offline/Runtime/Hybrid
intent; and one deterministic seed. Capture validates a detached candidate completely,
canonicalizes nodes, pins, edges and exposed inputs by stable identity, proves edge
direction/type/cardinality and DAG invariants, then publishes one immutable value.

The canonical `HPCG` byte envelope uses fixed-width network byte order and contains no
native handles, addresses, editor layout, callbacks or executable code. Decoding checks
the envelope and tier-derived counts before reserving containers, rejects trailing or
truncated data, and reuses the same semantic validation boundary as direct creation.
Floating defaults must be finite and signed zero is normalized before publication, so
container order and equivalent zero representations cannot change canonical bytes.

Node support is supplied as one immutable host-composed catalog projection. An unknown
node either fails with `pcg.graph.node_type_unknown` or is retained as inert bounded
payload under `PreserveInert`; a preserved graph is explicitly not cook-eligible. It is
never skipped, substituted or discovered globally. Older compatible source enters only
through an explicitly supplied bounded `IPCGGraphSourceMigrator`, and its output must be
the exact current schema and pass full decode/validation. Assets remains the owner of
the surrounding `AssetId`, durable bytes and atomic publication revision.

PCG owns plan validation and pure evaluation, not generated feature truth. Product
gameplay/server authority permits semantic runtime requests; a host transaction
coordinator revalidates authority and target revisions, while RuntimeScene,
Terrain/Foliage, Physics, Navigation and Render each prepare and own their committed
state. Successful evaluation alone never grants publication authority.

### Generation Plan And Output Delta Contract

`Horo/PCG/PCGGenerationPlan.h` is the implemented PCG-4.2 boundary between pure
evaluation and target preparation. A detached candidate binds one exact graph
generation and execution, deterministic seed, stable replacement lineage and
generated set, monotonically advancing set revision, explicit cell, target owner,
required capability set, and target-issued validation receipt. Dependencies and
create/update/remove operations carry typed stable identities, fixed content
fingerprints, and complete work/resident/preparation/retirement estimates.

`CreatePCGGenerationPlan` validates and sorts the detached dependency and output
containers before publishing a shared immutable root. The validation context supplies
the current exact target receipt and a bounded immutable snapshot of target-owned
output provenance. Create fails on an existing logical output. Update and removal
require the same logical output, lineage, generated set, cell, target owner, ownership
generation, and prior content fingerprint from that snapshot, plus a newer set
revision. A graph/name/tag/folder/spatial scan therefore cannot authorize deletion.

Capabilities are explicit Horo-owned `PCGCapabilitySet` values. A missing capability
returns `pcg.capability.unsupported`; an old receipt or target/capability generation
returns a typed stale failure. Cancellation and shutdown close plan admission before
allocation. Count, reference, work, and lifecycle-byte bounds use checked arithmetic,
and failure publishes no plan or target mutation. Replacement creates a detached new
root while retained readers continue observing the old immutable plan.

## Spatial Input And Node Catalog

[ADR-152](../../adr/152-pcg-spatial-input-snapshot-and-node-library-ownership.md)
is the normative owner of spatial capture, query execution, numerical determinism,
core-node catalog and extension policy. RuntimeScene, Terrain/Foliage, spline features,
Physics, Navigation and World Streaming retain their own mutable truth. A host safe
point captures one coherent immutable `PCGSpatialInputSnapshot` containing copied Horo
values and/or exact-generation read leases; PCG never retains their mutable storage or
native objects.

Evaluations select `ExactSnapshot`, `CurrentAtCommit` or bounded `CurrentOrRetry`
consistency. A lease preserves memory but not logical currentness. Positive, negative
and partial queries record exact provider/revision/coverage dependencies, and missing
coverage is an unavailable input rather than an empty result.

The v1 built-in catalog covers typed inputs, bounded sampling/query, filters,
transforms, point operations and output intents. Each node has a stable ID, versioned
schema, determinism class, capability declaration and complete cost function. The host
publishes one immutable catalog generation; unknown nodes and untrusted on-demand code
loading fail explicitly.

### Spatial Snapshot Schema 1

`PCGSpatialSnapshot` is the implemented provider-neutral input boundary. A detached
candidate carries stable snapshot, provider, source, revision and element identities;
one right-handed Y-up coordinate contract; a non-zero origin epoch; complete requested
bounds and coverage; and owned surface-triangle, box/sphere-volume, spline and grid
values. Capture validates the complete candidate and sorts every element family by
stable identity before publishing an immutable shared root. It retains no source
container, mutable span, native handle, callback or service reference. The public
snapshot is a cheap copyable value handle over that shared immutable state; callers do
not add a second owning pointer around the handle.

Schema 1 accepts an empty value set only with explicit `Complete` coverage. `Partial`
and `Missing` fail with `pcg.spatial.coverage_unavailable`, so absent residency or
provider data cannot become a false negative query result. Non-finite coordinates,
unsupported axes/precision, zero origin epochs, invalid transforms/bounds, degenerate
triangles/splines/volumes, overflowing grids, duplicate identities and tier/byte
excesses fail before publication.

Replacement preserves provider/source lineage, requires a distinct snapshot identity
and strictly newer source revision, and publishes a new immutable root without
mutating old readers. `ValidatePCGSpatialSnapshotCurrent` compares the captured
provider/source/revision/origin tuple with current owner evidence. A retained old root
therefore remains memory-valid while failing current-at-commit validation. Schema 1
copies canonical Horo values; provider read-lease and multi-provider coordinator
semantics remain later integration work and cannot be inferred from this value model.

### Deterministic Provenance Schema 1

`Horo/PCG/PCGProvenance.h` is the PCG-1.5 immutable evaluation provenance boundary.
An evaluation captures the exact graph identity, source revision and canonical content
digest, authored graph seed, world identity, signed cell coordinates, stable node
identity, policy version, typed input revisions and content digests, and every provider's
source revision, snapshot identity, origin epoch and canonical content digest. The host
supplies the digest of canonical bytes when capturing an input or provider; a revision
alone does not prove unchanged content. Capture sorts bounded input and provider sets
by stable identity and rejects duplicates, missing evidence and closed lifecycle states.
The captured root owns its records, so replacing source or provider snapshots does not
mutate readers already evaluating the previous root.

The closed determinism class is `PortableDeterministic`, `ProfileDeterministic` or
`BestEffortPreview`. Each maps to a matching numeric support tier. Portable integer
semantics require no profile fingerprint; certified profile floating point requires an
exact non-zero fingerprint. Preview is explicitly ineligible for reproducible seed,
output hash and reuse. A non-deterministic input or provider is admissible only for
preview. These are semantic promises, independent of operational capacity tiers.
Admission checks the declared capability; it does not certify an implementation's
numeric behavior. Hosts must select only kernels qualified for the declared promise.

Within a deterministic tier, a versioned SHA-256 provenance key encodes fixed-width
network-order values. Sample streams derive from that key and stable sample identity,
never traversal or thread completion order. Output hashing sorts by node, pin, sample
and ordinal, includes canonical output content, and omits the attempt-local execution
value. One output batch must still belong to one exact graph revision, node and
execution. Reuse requires exact current provenance key equality and a deterministic
tier; changed graph, input, provider, world/cell, node, numeric policy or certified
profile invalidates it. The owner must recapture current authoritative evidence before
calling the reuse check. This contract is a pure foundation for future cooked-plan and
evaluation integration; no graph evaluator is implemented by this schema.

## PCG Model

PCG is expressed as a directed acyclic graph (DAG) of nodes:

```cpp
struct PCGGraph {
    PCGGraphId            id;
    std::string           name;
    std::vector<PCGNode>  nodes;
    std::vector<PCGEdge>  edges;
    PCGGraphInputs        inputs;       // exposed parameters
};
```

Each node reads from input pins and writes to output pins. Data flows through
the graph as point clouds (spatial points with attributes).

### PCG Point

The fundamental data unit is a spatial point in immutable structure-of-arrays storage.
Core transform, local bounds, density and deterministic seed are parallel columns, as
is every schema-declared attribute. Density is finite and inclusive in `[0, 1]`;
transforms and bounds follow the finite Horo Scene Math contract.

```cpp
struct PCGPointCoreColumns {
    std::vector<Math::Transform> transforms;
    std::vector<Math::Aabb> bounds;
    std::vector<float> densities;
    std::vector<std::uint64_t> seeds;
};
```

Schema-1 attribute values are a closed vocabulary of boolean, signed/unsigned integer,
scalar and Horo `Vec2`/`Vec3`/`Vec4` columns. Canonical lowercase ASCII namespaced keys
are validated and sorted before schema publication. Unknown, duplicate, mismatched,
non-finite, over-limit or unequal-length columns reject the complete detached candidate;
there is no public `VariantMap`, per-point map, string conversion or partial repair.
Successful replacement publishes a new immutable snapshot while existing reader leases
retain the old snapshot until they release it.

### Node Types

**Spatial Nodes** generate or transform points in space:

- **Surface Sampler**: Sample points on mesh surfaces
- **Volume Sampler**: Sample points within a volume (box, sphere)
- **Spline Sampler**: Sample points along a spline
- **Grid Sampler**: Regular grid distribution
- **Random Scatter**: Poisson disc, random jitter, stratified sampling

**Filter Nodes** remove or modify points:

- **Density Filter**: Keep points with density above/below threshold
- **Bounds Filter**: Keep points within bounding volume
- **Slope Filter**: Filter by surface slope (for terrain)
- **Distance Filter**: Remove points too close to each other
- **Attribute Filter**: Filter by arbitrary attribute condition

**Transform Nodes** modify point attributes:

- **Transform Modifier**: Apply translation, rotation, scale
- **Random Offset**: Add random variation to transform
- **Align To Surface**: Orient points to match surface normal
- **Attribute Noise**: Add Perlin/Simplex noise to attributes

**Generation Nodes** produce typed output intents; target owners decide whether and how
to realize them during a later aggregate commit:

- **Static Mesh Spawner**: Emit static-mesh placement intents at points
- **Foliage Spawner**: Emit foliage placement intents at points
- **Actor Spawner**: Emit gameplay-entity spawn intents at points
- **Decal Spawner**: Emit decal projection intents at points

```cpp
struct PCGMeshSpawnerSettings {
    std::vector<PCGMeshEntry> meshes;    // weighted random selection
    Vector2                    scaleRange;
    bool                       alignToSurface;
    uint32_t                   seed;
};

struct PCGMeshEntry {
    AssetId   meshId;
    float     weight;           // selection probability weight
};
```

## PCG Graph Evaluation

Graph evaluation is deterministic given the same cooked plan, seed and immutable
input snapshot:

1. Validate the cooked plan, exact dependencies, capabilities and cost envelope.
2. Capture immutable exposed and spatial inputs plus the deterministic seed domain.
3. Reserve the complete evaluation-intermediate and output-candidate budget.
4. Evaluate precomputed stages and pass operation-owned point data through typed edges.
5. Return typed generated-output candidates with stable provenance.
6. Revalidate and prepare every target owner, then publish all required outputs through
   one aggregate commit or roll back the candidate completely.

Generation nodes do not create scene objects as an evaluation side effect. Partial,
failed, cancelled, stale or over-budget evaluations publish no external state.

### Generated Output Ownership

[ADR-153](../../adr/153-pcg-pure-evaluation-commit-and-generated-output-ownership.md)
is the normative owner of candidate provenance, target preparation, aggregate commit,
regeneration, cleanup and ownership transfer. Every output belongs to a stable
generated-set lineage and per-object identity. RuntimeScene, Terrain/Foliage, Physics,
Navigation, Render or the applicable gameplay owner stores exact provenance beside its
committed state and remains authoritative for current existence/ownership.

Regeneration computes create/update/retain/retire/conflict operations against one exact
prior ownership generation, prepares new state beside old state and commits atomically.
Cleanup names the exact lineage, set revision, scope, layer and ownership generation;
it never scans by graph ID, display name, tag, folder, hierarchy, component shape or
spatial bounds. Hand-authored, explicitly adopted and differently owned content cannot
be removed by stale PCG cleanup.

```cpp
struct PCGExecutionContext {
    PCGGraphId             graphId;
    uint32_t               seed;
    PCGGraphInputs         inputs;
    CancellationToken      cancelToken;
    PCGGenerationFlags     flags;
};
```

## Runtime vs Offline

PCG can run offline (editor bake) or at runtime:

- **Offline**: All generation is done in the editor and baked into the scene.
  Generated objects become regular static scene objects.
- **Runtime**: Generation runs during gameplay. Generated objects are
  dynamically created and destroyed. Used for dungeon generation, loot
  placement, procedural quest areas.

```cpp
enum class PCGGenerationMode {
    Offline,      // editor-only, baked to scene
    Runtime,      // gameplay-time generation
    Hybrid,        // offline base with runtime detail
};
```

Runtime PCG uses the same graph infrastructure as offline, with additional
constraints (time budget per frame, asynchronous evaluation). Both modes consume an
exact immutable cooked-plan generation. Publishing or evicting a cook-cache entry does
not replace a live plan; old plans and their dependencies remain leased until every
evaluation and generated-output candidate drains.

The closed execution modes are:

- **Offline bake**: evaluates accepted authoring inputs and returns a detached bake
  candidate for an explicit document/asset transaction.
- **Editor preview**: publishes only into an isolated replaceable preview session.
- **Runtime**: prepares an aggregate candidate that requires product/server authority
  and target-owner commit.
- **Hybrid**: keeps baked base and runtime overlay provenance separate; the overlay
  cannot rewrite baked/source truth.
- **Validation only**: returns diagnostics, costs and readiness without output
  preparation or commit capability.

A headless host may install the real deterministic evaluator without Editor or Render.
`PCGNull` advertises no evaluation capability and completes requests with typed
unavailability; it never returns a fabricated empty success.

## Determinism And Reproducibility

PCG is designed for determinism:

- Same graph + same seed + same inputs = same output
- Seeds can be set explicitly or derived from world coordinates for
  coordinate-based determinism
- Random number generation uses a per-node deterministic RNG seeded from the
  execution context
- Determinism enables re-generation (modify graph, re-run, get consistent
  results)
- Non-deterministic inputs (e.g., gameplay state) are explicitly marked

Plans and requests select one explicit determinism class:

- **Portable deterministic**: canonical output matches across every certified host for
  the declared semantic/numeric policy and is eligible for canonical bake or
  authoritative runtime state.
- **Profile deterministic**: output matches only inside one exact certified platform/
  toolchain/capability profile and is limited to product uses that pin that profile.
- **Best-effort preview**: invariants and bounds hold without output equivalence; it is
  restricted to isolated preview or explicitly cosmetic disposable output.

Determinism class is independent of workload tier. Runtime never silently downgrades a
request when a node, target or numeric policy cannot meet the required class.

## Integration

PCG integrates with:

- **Scene Runtime**: Generated objects are normal entities with components
- **World Streaming**: Generated content within a streaming cell is scoped
  to that cell's load/unload
- **Terrain**: Terrain height can be an input to PCG graphs
- **Foliage**: PCG can generate foliage placement
- **NavMesh**: Generated walkable surfaces trigger NavMesh re-baking

These integrations use immutable spatial snapshots and typed target-owner candidates.
They do not expose live provider storage to PCG or grant nodes authority to mutate the
integrated subsystem.

[ADR-154](../../adr/154-pcg-cross-system-authority-readiness-and-commit-boundary.md)
defines the integration adapter and readiness contract. Adapters depend on both the PCG
and target public APIs and are composed by the host; PCG Core does not depend on target
implementations. Each target prepares private state and returns a transaction/generation/
dependency/cost-scoped receipt. Required owners publish together at one aggregate safe
point or all prepared state rolls back.

Terrain/Foliage retains dataset, type, instance, cook, runtime placement, extraction and
eviction authority. World Streaming owns cell demand/reservation/publication; Scene/
Prefab owns entity expansion/identity; Navigation owns topology; VFX observes only
post-commit facts; Network and Runtime Save capture target-owner canonical state rather
than PCG workers/intermediates. Required unavailable capabilities fail the transaction;
declared optional omission is typed and changes the selected output-plan identity.

## Editor Authoring

The PCG editor provides a node-graph editing surface:

- Node palette with categorized node types
- Drag-and-drop node placement and connection
- Visual preview of point clouds at each node output
- Live generation preview in the editor viewport
- Seed randomization for variation exploration
- Bake-to-scene command

[ADR-155](../../adr/155-pcg-graph-document-preview-bake-and-undo-ownership.md)
defines the editor ownership boundary. A persistent graph document owns working source,
revision, typed commands, semantic history and save/recovery/conflicts; panels own only
presentation. Preview captures one immutable revision and uses the ordinary transient
cook/runtime path in an isolated world. Bake is a separate target-owner aggregate
transaction, and undo/redo restores exact semantic patches without reevaluation.

## Feature Tiers

[ADR-156](../../adr/156-pcg-scale-budgets-trust-and-release-scope.md) fixes three
provider-neutral version-1 operational profiles. They are not renderer APIs, hardware
detection results or authority levels. At composition/cook time they resolve to an
immutable Horo-owned profile containing supported modes, node/output families,
determinism classes and finite graph, point, memory, work, concurrency and overlap
limits.

| Feature | `PCGBaseline` | `PCGStandard` | `PCGHigh` |
|---|---:|---:|---:|
| Offline bake / validation | Yes | Yes | Yes |
| Isolated editor preview | Yes, bounded | Yes | Yes |
| Semantic runtime/headless commit | No | Yes | Yes |
| Nodes / edges per graph | 32 / 64 | 256 / 512 | 1,024 / 2,048 |
| Points in one node output | 16K | 256K | 2M |
| Total materialized point records | 64K | 1M | 8M |
| Concurrent evaluations / admitted queue | 1 / 4 | 4 / 32 | 8 / 64 |
| Total PCG-owned/charged memory | 112 MiB | 1,024 MiB | 4,096 MiB |
| Hierarchical graphs | No | No | No |
| Custom executable node providers | No | No | No |

Unknown cost is not zero. Complete plan/input/intermediate/candidate/target and old/new
overlap costs are reserved before admission. Saturation returns typed backpressure or
yields already admitted work; it never truncates points, drops authoritative requests,
skips nodes, chooses a new seed or commits a partial result.

## Trust, Observability And Release Scope

Graph/cooked/cache/package/spatial/save/network values are untrusted until their schema,
bounds, digest, dependency, cost, capability and semantic invariants pass. Built-in code
trust does not waive input/resource validation. Version 1 executes repository-built,
host-composed node types only; graph content cannot load native libraries, scripts,
WASM, processes or remote providers.

Normal metrics use finite dimensions such as mode, profile, determinism class, phase,
target kind and outcome. Graph/asset/node/object/cell/user IDs, paths, coordinates, seeds
and attribute values are prohibited metric dimensions. Detailed per-operation evidence
requires a bounded capability-gated diagnostic snapshot and follows process
Observability redaction, retention and export policy.

M5/1.0 qualifies the built-in single-DAG CPU path, immutable spatial inputs, offline/
preview/runtime/headless modes, exact target commit and the limits above through
PCG-7.2–PCG-7.6. Hierarchical/subgraph composition (PCG-7.7) and custom executable
providers (PCG-7.8) are post-1.0. They do not gate M5 and are rejected under version-1
plan/profile identities.

## Stable Identity Contract

PCG source persists distinct non-zero graph, node and pin identities. Names, paths,
array positions, graph-layout coordinates, object addresses and container allocation
order never participate in identity. A graph generation pairs its stable graph
identity with an exact non-zero durable revision. Execution identity includes that
generation, and generated-output identity additionally includes the stable output node,
pin, source sample and deterministic ordinal.

All identity values have fixed-width canonical network-byte-order encodings. Reload
and deterministic recook preserve the authored values exactly. Resolution compares the
complete generation and execution association, so completion or output from a retired
revision cannot resolve into its replacement. Process-local registry handles, worker
tokens, callbacks, filesystem paths and backend-native values remain outside this
durable contract and are rebuilt by their owners after load.

## Registry And Host Composition Contract

`Horo/PCG/PCGRegistry.h` is the bounded PCG-1.4 composition and query surface. The
application host explicitly creates one `PCGRegistry` with a host-unique process-local
incarnation identity, projects an exact interactive,
headless or null capability set, and contributes inert graph and node-runtime
descriptors. Registration performs no static initialization, service discovery, source
scan, asset load, worker start, callback invocation or target mutation.

Each successful mutation publishes a new registry generation. `PCGRegistrySnapshot`
owns immutable, stable-ID-sorted graph and node-runtime arrays. Its process-local graph
and runtime handles contain the issuing registry incarnation and generation, dense slot and exact
durable graph or semantic runtime association. A handle resolves only through that
issuing snapshot. Replacement and unregister leave older snapshots readable; resolving
their handles through a newer snapshot returns a typed stale-handle failure.

Graph execution queries require the exact durable graph revision, every explicitly
requested capability, every graph-declared capability and an exact registered runtime
for each node type. Missing evidence returns `IdentityStale`,
`UnsupportedCapability` or `RuntimeUnavailable`; lookup never chooses another graph,
node type, runtime contract, execution mode or output path. Registration and
replacement are composition-owner-thread operations. Published snapshots are immutable
and may be copied to concurrent readers without exposing mutable registry storage.

Interactive composition may grant editor-preview and render-output capabilities.
Headless composition can install the real deterministic validation/evaluation catalog
and non-render output capabilities, but rejects editor-preview and render-output grants.
Null composition grants no validation, evaluation or output capability. It may retain
inert metadata for discovery, but every capability-bearing admission completes with the
same typed unsupported result rather than fabricated empty success. `Close()` ends new
publication and releases the live catalog while already issued snapshots retain their
owned immutable data.

## Async Operation And Invalidation Boundary

`Horo/PCG/PCGAsyncOperation.h` is the PCG-1.6 owner-lane operation coordinator.
It accepts only exact registered scene/cell/graph scopes and captures the durable
`GraphGeneration` together with the digest of canonical source bytes. Its fence
also carries a never-reused runtime generation and exact plan, input and authority
publications. A matching graph revision with different source bytes is not current.
The coordinator does not depend on a concrete cooked-plan type; a host attaching a
plan checks its graph generation and source digest on the owner lane before
admission and again at any later transaction boundary.

Worker callbacks prepare owned immutable candidates and may spawn structured
children. The parent cannot finish until every accepted child is joined. Only the
creating owner lane advances completion and publishes an immutable PCG candidate or
result after an exact-current fence check. This is not permission to commit Scene,
Terrain/Foliage or another target: target preparation and aggregate commit remain
external host/target-owner transactions that revalidate authority and revisions.
Every accepted operation reaches one immutable success, failure, cancellation or
stale terminal with typed cause and child/work accounting. Repeated completion and
late cancellation cannot change it.

Graph, cell, scene and host invalidation close matching admission/publication and
request cancellation. The host retains exact inputs, plans, modules, candidate and
target-owner leases until every worker, child and owner completion drains. Scope
re-registration needs a strictly newer compatible fence after that drain; shutdown
cannot fabricate completion, detach work or release an owner still reachable by it.
An owner can sweep all completions without blocking, then forget terminal records and
retire a closed graph scope to return its finite admission capacity. Scope retirement
requires no retained operation record and never changes an issued terminal result.

## Pre-Compile Graph Validation Contract

`Horo/PCG/PCGGraphValidation.h` is the PCG-2.3 boundary between canonical authored
source and the later plan compiler. `ValidatePCGGraph` accepts one immutable
`PCGGraphAsset`, one retained immutable `PCGRegistrySnapshot`, explicit caller
capabilities, finite work/diagnostic ceilings and a captured lifecycle admission gate.
It never repairs source, discovers a runtime, mutates a registry or emits an executable
plan.

Validation requires the exact durable graph revision, descriptor/source node identity
and type agreement, the host's explicit validation grant, graph and caller capability
grants, and one exact runtime handle for every node. Success owns a compact node array
in deterministic dependency-first order; stable node identity breaks ties between
independent nodes. Every handle is fenced to the returned registry generation, so a
compiler must retain the issuing snapshot and must not resolve the handle through a
replacement generation. The result also captures a SHA-256 digest of canonical
source bytes; compilation checks this against its source rather than trusting only
the durable revision identifier.

Malformed topology and unknown-node policy are rejected by `PCGGraphAsset::Create`
before this boundary. Missing runtime evidence is accumulated in stable node order with
graph/revision/node provenance up to the admitted diagnostic ceiling. Capacity,
cancellation and shutdown reject without partial validated output. Retained snapshots
remain valid after replacement or registry shutdown, while new validation against a
replacement snapshot requires the replacement graph revision and runtime contracts.

## Canonical Cooked Graph Plan

`Horo/PCG/PCGCookedPlan.h` is the PCG-2.4 lowering boundary. `CompilePCGGraph`
requires the same canonical source and retained registry snapshot that produced a
`PCGValidatedGraph`. It checks exact graph and provider-generation handles, then
copies dependency-first nodes, pin schemas, semantic payloads, provider contract
versions, capability requirements, plan-local pin routes, input constants and
exposed-input defaults. The result owns its data and canonical network-order bytes;
it retains no authoring, registry, backend or process-local handles. The byte
contract records plan/compiler/source schemas, graph revision and provider semantic
contract versions plus the SHA-256 digest of exact canonical source bytes captured
by pre-compile validation; even same-revision altered source is rejected.
Caller-lowered and tier plan-byte bounds reject output before publication. The
plan is a cook result, not an editable source or a command to
mutate Scene or another target owner. An exposed binding supplies that input's
fallback and excludes its authored pin default from constants; a simultaneous
incoming edge to the same input is rejected as ambiguous routing.

## Related Documents

- [PCG Ownership, Authority, Tier and Lifecycle](../../adr/151-pcg-ownership-authority-tier-and-lifecycle.md)
- [PCG Graph Source, Cooked Plan, Cache and Runtime Ownership](../../adr/150-pcg-graph-source-cooked-plan-cache-and-runtime-ownership.md)
- [PCG Spatial Input Snapshot and Node-Library Ownership](../../adr/152-pcg-spatial-input-snapshot-and-node-library-ownership.md)
- [PCG Pure Evaluation, Commit and Generated-Output Ownership](../../adr/153-pcg-pure-evaluation-commit-and-generated-output-ownership.md)
- [PCG Cross-System Authority, Readiness and Commit Boundary](../../adr/154-pcg-cross-system-authority-readiness-and-commit-boundary.md)
- [PCG Graph Document, Preview, Bake and Undo Ownership](../../adr/155-pcg-graph-document-preview-bake-and-undo-ownership.md)
- [PCG Scale Budgets, Trust and Release Scope](../../adr/156-pcg-scale-budgets-trust-and-release-scope.md)
- [PCG Graph Editor UI Reference](./pcg-graph-editor.html)

- [Scene Runtime](./scene-runtime.md): generated objects as entities
- [Terrain And Foliage Architecture](./terrain-and-foliage-architecture.md): terrain and foliage as PCG inputs/outputs
- [World Streaming Architecture](./world-streaming-architecture.md): PCG scope per streaming cell
- [Navigation And AI Architecture](./navigation-and-ai-architecture.md): NavMesh re-baking after PCG
- [Editor Document Model](../editor/editor-document-model.md): PCG graph asset editing
