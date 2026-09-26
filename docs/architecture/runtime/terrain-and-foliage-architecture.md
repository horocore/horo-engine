# Terrain And Foliage Architecture

## Purpose

This document defines the terrain, landscape, foliage, and vegetation
subsystems for Horo Engine. It covers heightfield-based terrain, layer
painting, instanced foliage rendering, wind simulation, LOD, collision,
render extraction, streaming budget integration, and editor authoring tools.

Virtual-texture integration follows
[ADR-169](../../adr/169-vtx-producer-terrain-world-streaming-packaging-and-server-ownership.md).
Terrain owns canonical source, dataset/tile semantics and runtime generations; a VTX
adapter captures immutable exact-generation inputs and hints. Derived VTX pages never
become Terrain truth, and VTX invalidation cannot mutate Terrain state or cell authority.

[ADR-137](../../adr/137-terrain-foliage-ownership-data-tier-and-lifecycle.md)
is the foundation contract for this subsystem. Terrain is a backend-neutral runtime
vertical slice with distinct `TerrainApi` and `TerrainRuntime` ownership; it is not
part of generic Foundation, RuntimeScene internals or a renderer backend.
[ADR-138](../../adr/138-terrain-source-cooked-tile-cache-and-streaming-ownership.md)
defines the complementary source, cook, tile-manifest, cache, residency and generation-
replacement contract.
[ADR-139](../../adr/139-terrain-render-extraction-material-lod-and-tier-boundary.md)
defines the render-candidate, material, per-view LOD/visibility, product-profile and
core-1.0 versus post-1.0 GPU-driven boundary.
[ADR-140](../../adr/140-foliage-placement-baked-dynamic-state-and-eviction-ownership.md)
defines deterministic placement, baked/ephemeral/durable state classification, mutation,
capacity, persistence handoff and eviction ownership.
[ADR-141](../../adr/141-terrain-foliage-cross-system-ownership-and-readiness.md)
defines immutable consumer snapshots, typed readiness receipts, owner-safe-point
publication, aggregate activation, rollback and reverse-DAG retirement.
[ADR-142](../../adr/142-terrain-foliage-document-tool-undo-and-preview-ownership.md)
defines persistent authoring documents, typed tool routing, bounded tile-patch history,
isolated preview and distinct source-save/cook/runtime-persistence boundaries.
[ADR-143](../../adr/143-terrain-foliage-scale-budgets-observability-and-feature-boundary.md)
defines versioned core/high-end 1.0 scale and performance workloads, required
measurements and the explicit post-1.0 GPU-driven qualification boundary.

## Ownership And Data Boundaries

`HoroEngine::TerrainApi` owns public typed identities, revisions, handles,
descriptors and narrow command/query/snapshot interfaces. `HoroEngine::TerrainRuntime`
owns live dataset/tile/cluster generations, mutable overlays, preparation, snapshot
publication and retirement. Hosts compose it with Assets, Scene, World Streaming,
Render, Physics and Navigation through backend-neutral ports.

The data path is explicit:

```text
authored terrain/foliage assets and document commands
  -> deterministic target/tier-keyed cook
  -> immutable bounded dataset/tile/cluster payloads
  -> typed RuntimeScene binding
  -> TerrainRuntime generation and immutable snapshots
  -> leased render/physics/navigation/streaming projections
```

Editor documents own source mutation and undo. Assets/Pipeline own import, cook and
atomic artifact publication. RuntimeScene owns entity/binding lifetime. World
Streaming owns cell demand and aggregate reservations. Render, Physics and Navigation
own their realized resources and native-thread retirement. UI, CLI, MCP and gameplay
use typed application/Terrain capabilities and never mutate buffers directly.

Stable `TerrainDatasetId`, `TerrainTileId`, `FoliageTypeId`, `FoliageClusterId` and
`FoliageInstanceId` values are distinct from runtime handles, revisions, `AssetId`,
entity/cell IDs and native handles. TRF-001.2 freezes their exact encoding. Content,
residency, mutation and capability revisions advance independently so consumers check
the state they actually use.

### Stable Identity And Runtime Fencing Contract

`HoroEngine::TerrainApi` implements the TRF-001.2 identity boundary. Stable project,
dataset, foliage-type, cluster and baked-instance domains use distinct strong 128-bit
values. The all-zero representation is reserved. Project identity comes from canonical
project metadata; dataset and foliage-type identities are the first 128 bits of SHA-256
over a versioned domain separator, exact project bytes and a non-empty bounded canonical
semantic key. Display names, paths, locale, timestamps, pointer values and native handles
are prohibited derivation inputs.

`TerrainTileId` is not a hash of presentation state. Its canonical 25-byte encoding is
the exact dataset identity followed by signed world-tile X and Z in network byte order
and one LOD byte. Negative floor-quantized coordinates therefore remain stable across
world-origin rebases. Dataset manifest membership and compatibility remain separate
validation; equality of coordinates does not make tiles from different datasets equal.

Foliage-cluster identity is domain-separated over the exact tile encoding and a bounded
canonical cluster-provenance key, never an array position. Stable baked
`FoliageInstanceId` is domain-separated over the exact tile,
foliage type and bounded canonical placement-provenance key. Equal complete inputs yield
equal bytes independent of job order; changing the domain, project, dataset, tile, LOD,
type or placement key changes identity. A conflicting duplicate fails publication rather
than being renamed or assigned an array index.

Stable identity bytes are never recycled for different authored meaning. Deletion keeps
the identity reserved in source/cooked history; compatible recook preserves it, while a
new semantic object derives a new value. Runtime slot reuse always increments generation,
and generation exhaustion retires the slot instead of wrapping.

Runtime identities are deliberately different. `TerrainRuntimeHandle` contains the
stable dataset plus a registry slot and non-wrapping generation;
`RuntimeFoliageInstanceHandle` adds its own slot and non-wrapping generation. Access
validates exact dataset, both slots, both generations and Active lifecycle state before
registry use. Replacement makes the prior generation stale, Closing/Closed rejects new
access, overflow closes reuse, and no runtime handle has a serialization function.

`TerrainContentRevision`, `TerrainResidencyRevision`, `TerrainMutationRevision` and
`TerrainCapabilityRevision` are independent non-zero, non-wrapping strong types. One
`TerrainSnapshotRevision` captures all four; consumers compare the fields they actually
read instead of substituting a generic dirty flag. Bounded catalog validation performs
no registration, I/O, backend selection or ambient mutation and rejects malformed,
duplicate, foreign-dataset and over-capacity candidates transactionally.

### TRF-001.5 Async Preparation Boundary

`TerrainAsyncJobs` is the first `TerrainRuntime` implementation slice. The host
injects its Foundation `JobSystem`, an exact runtime/registry/revision fence,
finite work limits and explicit capability grants. Cook, load and edit-preview
requests enter through distinct `SubmitCook`, `SubmitLoad` and
`SubmitEditPreview` operations and submit immutable-input preparation to durable Foundation jobs with
captured parent cancellation, operation correlation and optional configuration.
Callbacks own their candidate values or leases; worker callbacks never publish
Terrain state or touch Scene, editor or native consumer owners.

The Terrain owner calls `Advance` at a safe point. Only a successful prepared job
whose complete captured fence still equals the current fence may run its atomic
publication callback. A `JobCancelled()` publication result remains cancellation
with its typed cause and is never retried; other failures retain their original
typed errors. Owner publication rejects reentrant fence replacement or nested
`Advance`, so its validated fence cannot change inside that callback. A queued or
running cancellation, replacement, capability revision or shutdown prevents
publication and terminates as cancellation, with stale revision retained as a
typed cause. A new fence cancels outstanding candidates without changing the
previous publication. Each accepted item has one immutable terminal outcome;
records remain bounded and durable until the owner explicitly forgets them.
Shutdown closes admission and publication, requests cancellation and reports
whether workers have drained without blocking the owner thread. Host code retains
candidate/provider dependencies until that drain and any downstream consumer
retirement are acknowledged. This slice does not create a second cell scheduler,
cache authority or generic Foundation terrain dependency.
Concrete height/weight tile cook and cache producers remain TRF-002.2/002.3/002.5,
tile residency/resource loading remains TRF-002.6/002.7, and authored preview
producers remain TRF-006.2 and later. Those producers compose this lifecycle
boundary when their typed payloads and owners exist; this slice does not claim
to cook, decode or preview terrain assets by itself.

### TRF-001.6 Product Composition Boundary

`TerrainComposition` is an inert, fixed-size `TerrainApi` decision, not a service
installer. A host provides exactly one typed availability fact and implementation
revision for every closed Terrain/Foliage capability. The exact Null, Headless,
Editor, Runtime or Unsupported policy resolves each fact to omitted, unavailable
or bound. Missing required capabilities fail composition; optional capabilities
remain explicitly unavailable. Null and Unsupported bind nothing even if a host
has implementations installed elsewhere. Unsupported is inspectable but rejects
work with a typed error.
Foliage runtime requires effective Terrain runtime, and CPU/GPU culling or wind
cannot report an effective grant without the required render/foliage path. Such
dependent optional grants report unavailable rather than pretending to run.

Headless omits all render extraction, culling, wind and authoring grants; Runtime
requires render extraction but omits authoring; Editor requires both authoring
grants while render extraction remains optional. Profile names do not select a
renderer, create an authoring document, choose a provider, or imply tier fallback.
The host alone owns construction and lifetime of any concrete service. New
TerrainAuthoring and FoliageAuthoring capability bits append to the existing
closed vocabulary without changing prior bit identities.

One immutable decision carries a non-wrapping `TerrainCapabilityRevision` and
source revisions for bound facts. Work admission checks the current revision,
explicit lifecycle and exact required grants; replacement returns a detached
newer decision and never mutates the old one on failure. Cancelling, shutdown,
stale and unsupported-profile paths have typed results. Existing registry and
async-job users may continue to supply their exact grants, but hosts that select
a product profile must derive those grants from the resolved decision, not from
profile-name strings or ambient discovery. No public contract gains a native
handle, renderer dependency or implicit global ownership.

## Terrain System

### Heightfield Model

Terrain heightfield source/cooked metadata describes a bounded regular grid. Bulk
samples remain inside an authored document transaction, immutable asset lease or
operation-owned decode candidate; the public runtime contract does not expose a
mutable owning vector for the whole terrain:

```cpp
struct TerrainHeightfieldDescriptor {
    TerrainDatasetId dataset;
    AssetId sourceAsset;
    TerrainSourceRevision sourceRevision;
    TerrainTileGrid grid;
    TerrainSampleEncoding encoding;
    SceneBounds bounds;
    TerrainDescriptorLimits limits;
};
```

### Terrain Component

A Scene entity carries only a typed binding to a cooked dataset and product feature
policy. Decoded tiles, layer arrays and dependent-system resources remain outside the
component:

```cpp
struct TerrainSceneBinding {
    TerrainDatasetId dataset;
    AssetId cookedDataset;
    TerrainSceneTransform transform;
    TerrainFeatureRequirements requiredFeatures;
    TerrainTierPolicy tierPolicy;
};
```

Layer definitions live in a bounded authored/cooked layer-set asset. The selected
Terrain plan validates its exact finite layer limit and a compatible cooked shader
variant before activation. Required content exceeding the resolved limit fails;
optional reduction is allowed only through an explicitly authored/cooked product
fallback and is never a runtime clamp that drops layers silently.

Heightfield assets are authored in the editor or imported from external
formats (heightmap PNG/EXR, RAW16, GeoTIFF, Houdini HeightField).

### Source, Cook And Dataset Ownership

External files are untrusted import inputs, not canonical terrain identity or runtime
data. Assets/Application owns tracked source bytes, `AssetId`, generic importer/cooker
orchestration, dependency scheduling, immutable cache storage, generation staging and
atomic publication. Terrain Import/Model owns format interpretation and the canonical
authored height/layer/hole/coordinate semantics; Terrain Cook owns deterministic tiling,
seams, LOD/mips, foliage clustering and Terrain artifact schemas. Neither contribution
creates a parallel asset graph, cache root, scheduler or publication pointer.

One immutable `CookedTerrainDatasetManifest` binds the selected content revision, tier,
coordinate profile, bounds, dependency fingerprint and canonically sorted tile/cluster
entries. Every entry declares stable typed address, exact bounds, seam/dependency
signatures, artifact identity, digest, byte/peak-work bounds and required neutral
consumer payloads. Tile artifacts are independently readable and verifiable, but the
verified manifest is the only membership authority; runtime never infers a dataset by
enumerating files.

The Terrain cook fingerprint includes the exact canonical source revision/digest, every
accepted dependency artifact, tiling/seam/LOD/compression policy, effective tier/limits,
Terrain schemas and algorithm versions, and generic target/toolchain envelope identity.
Paths, timestamps, locale, job order and runtime/provider handles are excluded. Native
GPU resources, solver objects and navigation-provider tiles remain separately keyed and
owned by Render, Physics and Navigation.

### Terrain Collision

Terrain collision geometry is derived from the heightfield at asset cook
time:

- A simplified heightfield mesh is generated per tile at a configurable
  collision resolution (typically coarser than the visual resolution)
- Collision tiles are fed to the physics system as static concave mesh
  colliders
- A cell prepares required collision and optional/required visual payloads under the
  same generation-tagged aggregate activation; headless visual absence is explicit
- Hole-carved vertices are excluded from the collision mesh; the physics
  system treats those areas as passthrough
- NavMesh generation respects the hole mask: holed areas are excluded from
  the walkable surface

```cpp
struct TerrainCollisionSettings {
    uint32_t  collisionResolution;   // vertices per tile side (default 33)
    bool      generatePerTile;       // true = one collider per tile
    float     collisionMargin;       // outward expansion for stability
};
```

### LOD And Tiling

Terrain is split into tiles at authoring time:

- Each tile is `N x N` vertices (default 129×129, corresponding to
  128×128 quads)
- Terrain Cook emits neutral bounded representation payloads and geometric-error,
  seam, skirt/morph and neighbor-compatibility metadata per LOD
- TerrainRuntime exposes only resident legal representations in its immutable snapshot
- RenderFrontend selects a compatible LOD per view from screen-space error, projection,
  output, hysteresis, profile and finite budget; distance alone is not the policy
- Native vertex/index resources and draw execution remain renderer-owned
- `TerrainLODSettings` contributes authored/cooked intent; it is not a backend command
  or permission to invent an unavailable representation

```cpp
struct TerrainLODSettings {
    uint8_t  lodCount;
    float    lod0Distance;
    float    lodDistanceMultiplier;
    bool     morphEnabled;
};
```

### Layer Painting

The editor terrain tools support:

- Height sculpting (raise, lower, smooth, flatten, noise)
- Layer weight painting (up to 4 layers simultaneously)
- Hole carving (per-vertex visibility mask)
- Spline-based path/road deformation

Paint operations use the terrain authoring document and typed edit-operation path from
[ADR-142](../../adr/142-terrain-foliage-document-tool-undo-and-preview-ownership.md).
Before mutation, the document executor freezes and reserves the exact canonical set of
affected tile/patch rectangles, including seam/apron and placement dependencies. One
atomic history entry owns lossless bounded `before` and `after` patch snapshots. A full
heightfield/dataset copy per edit and partial tile-by-tile commit are prohibited.

Undo/redo restores those committed patches and never reruns brush/noise/scatter
algorithms or reads current tool settings. Brush radius, falloff, strength, active layer
and foliage type are transient per-session workspace state; changing them does not alter
prior entries. A continuous gesture owns one revision-fenced preview overlay and commits
at most one operation, while cancellation restores the exact committed view.

Hole carving sets a per-vertex visibility flag in the terrain data. Holes
propagate to collision (excluded from collision mesh) and NavMesh (excluded
from walkable area). Characters and physics objects pass through holed
regions.

[ADR-108](../../adr/108-dynamic-overlay-carving-and-tile-rebuild-policy.md)
requires persistent/editor height, hole and spline changes to recook affected
collision/navigation tiles through their owning application operations. Runtime
terrain deformation cannot mutate cooked NavMesh or use simple obstacle carving as
general polygonization. It first makes the affected region conservatively
unavailable, then requires an explicitly composed transactional runtime grounded-
tile rebuild capability; the 1.0 runtime baseline returns
`UnsupportedDynamicChange` and keeps the safe exclusion until recook/replacement.

### Material Blending

Terrain materials are blended per-vertex using weight layers:

```cpp
struct TerrainMaterialLayer {
    AssetId  diffuseTexture;
    AssetId  normalTexture;
    AssetId  maskTexture;     // R=metallic, G=roughness, B=ambient occlusion
    float    uvScale;
    AssetId  materialFunction; // optional material-function graph reference
};
```

Terrain authoring/cook owns layer order, weight/hole encodings, material-function
references and required blend semantics. Material/Shader cook validates those semantics
and emits an explicit finite target permutation family. RenderFrontend selects only a
cooked variant compatible with the active raster recipe, neutral Terrain payload layout,
required features and effective limits; the backend realizes it without changing layer,
hole or blend meaning. Height-, slope- and noise-based transitions are permitted only
when declared by the material graph and cooked permutation contract. TRF-003.2 freezes
the exact layer limit, key and blend rules.

## Foliage System

### Instanced Rendering

Foliage exposes bounded neutral instance candidates. The core 1.0 renderer may group
them into backend-neutral direct or instanced batches after CPU per-view visibility and
LOD planning; it does not require compute, GPU Scene or indirect execution. A post-1.0
GPU-driven recipe may use renderer-owned compute culling and generated indirect batches
only after explicit capability, artifact, budget, fallback and backend qualification.
Backend-specific command names never appear in the Terrain contract:

- Each foliage type maps to one or more static meshes
- TerrainRuntime owns baked/dynamic logical instance membership and immutable transforms
- RenderFrontend owns native instance storage, per-view culling/LOD results and batching
- the private backend owns native resources, commands and synchronization

The stable definition below is the sole foliage type contract. Older illustrative
sketches that used generic asset identifiers, floating-point scale values or loose
booleans are intentionally superseded; authored data must use the typed,
fixed-point fields of `FoliageTypeDefinitionData`.

### Stable Foliage Definition Contract

`FoliageTypeDefinition` is the TRF-004.2 cook/runtime contract. It is an immutable,
fixed-size value keyed by `FoliageTypeId` and a non-wrapping
`FoliageDefinitionRevision`. Visual references use distinct stable
`FoliageMeshAssetId`, `FoliageMaterialAssetId` and `FoliageImpostorAssetId` domains: one
to four canonically ordered mesh LODs, one required material and one optional impostor.
Unused fixed-array entries must remain zero and duplicate mesh identities are invalid.

Placement uses the version-1 `StratifiedJitterV1` recipe with one authored 64-bit seed,
integer millimetre altitude/separation/coordinate quantum, milli-degree slope and
instances-per-square-kilometre density. Cook inputs are canonically sorted before the
algorithm runs. The coordinate quantum owns geometric tie-breaking; process RNG, wall
time, locale, pointer identity, filesystem order and worker completion order are never
inputs. A changed algorithm or quantization contract requires a new version and cook
fingerprint.

Culling stores strictly increasing integer-millimetre mesh LOD thresholds, an optional
later impostor threshold, a still-later cull distance and a bounded cross-fade width.
`CpuDirect` and `GpuIndirect` are exact recipes, not a preference ordering. Missing GPU
indirect support cannot fall back to CPU; missing impostor content cannot remove the far
representation silently. Runtime view selection remains Render-owned derived state.

Wind is fixed-point authored data. `None` requires every parameter to be zero;
`VertexBend` requires primary bend/frequency/flexibility and prohibits leaf flutter;
`VertexBendAndFlutter` additionally requires flutter. Collision is either entirely
absent or a positive neutral cylinder/capsule. Navigation blocking requires both
collision and the explicit navigation-blocking capability. These definitions carry no
scene wind state, native buffer, Physics body, Navigation handle or renderer command.

Validation uses the exact captured Terrain configuration/capability revision and closed
capability set before cook or runtime admission. Per-type instance counts cannot exceed
the captured tier limit. Insert requires no current definition; replacement requires
the same stable type plus the exact current and non-wrapping successor definition
revision. Inactive, cancelled or retiring owners reject admission, and every failure
leaves the current immutable definition unchanged.

**Instance limits**: `instanceLimit` is validated against the selected finite
provider-neutral Terrain tier during cook and runtime admission. Required content above
the limit fails with a typed result. Horo does not silently truncate, hide furthest
instances or infer limits from a graphics API name. An optional lower-density cooked
variant must be explicitly authored and selected by declared fallback policy.

### Foliage Collision

Foliage is **visual-only by default** — no collision response with
characters, projectiles, or physics objects. Per-foliage-type collision can
be opted in for gameplay-relevant foliage (e.g., large trees, destructible
bushes):

```cpp
struct FoliageCollisionSettings {
    bool      enableCollision;        // default false
    float     trunkRadius;            // simplified cylinder collision
    float     trunkHeight;
    bool      blockProjectiles;
    bool      blockNavigation;        // affects NavMesh carving
};
```

When collision is enabled, simplified collision primitives (cylinder or
capsule) are generated per instance at bake time and registered with the
physics system as static bodies. NavMesh carving at runtime (dynamic
obstacle removal) is not supported for foliage; foliage collision is
baked into the initial NavMesh generation.

Removing/destructing baked navigation-blocking foliage does not automatically open
walkability. It requires authored affected-tile recook or the explicit ADR-108
runtime rebuild capability over authoritative post-change geometry. Adding a
runtime blocker may use the logical overlay and supported optional carving, but
failure/absence remains conservative and observable.

### Render Extraction

`TerrainRenderExtractor` leases one immutable Terrain generation and publishes a bounded,
view-independent `TerrainRenderExtractionSnapshot`. Tile and foliage candidates carry
typed Terrain identities/revisions, neutral geometry/artifact references, bounds/
transform inputs, material requirements, legal LOD/seam data, hole/visibility semantics,
stable logical instance identity and finite cost estimates.

The snapshot contains no selected per-view LOD, visible list, draw command, indirect
argument, native handle, pipeline object or mutable Terrain pointer. Multiple views may
consume it without advancing Terrain state. Capacity exhaustion fails extraction rather
than truncating candidates.

RenderFrontend derives generation-checked `RenderObjectId` values, performs per-view
visibility/LOD/seam planning, resolves cooked material variants/resources and maps the
candidates into ADR-036 pass classifications. Opaque, Masked, TransparentSorted and
TransparentAdditive come from authored/cooked material semantics; “trunk” or “leaf” does
not imply a pass. The private backend only translates the validated plan.

### Decal Interaction

Decals project onto terrain surfaces by sampling the GBuffer within the
decal projection box. Foliage does not receive decals — the decal pass
renders before the foliage pass in the render graph, so foliage instances
are drawn on top of decaled terrain. This avoids decal-projection artifacts
on thin geometry (leaves, grass blades).

### Procedural Placement

Foliage instances are placed procedurally based on configurable rules:

- Density map (per-terrain-tile texture or global density curve)
- Slope/altitude constraints
- Exclusion zones (layer masks, water bodies, building footprints)
- Clustering for variety (clump size, radius)
- Random seed per foliage type for determinism

Placement is computed deterministically during Terrain cook from canonically sorted,
versioned rule/source inputs. A specified PRNG seed tuple, quantization and tie policy
produce stable ordered baked `FoliageInstanceId` records independent of locale, worker
count and job order. Oversized placement fails or uses an explicitly authored/cooked
lower-density variant; cook never truncates from host memory or camera distance. Exact
rule, PRNG and quantization schemas are TRF-004.2 work.

Runtime procedural spawning (for example, grass growing during gameplay) is supported
through the Terrain mutation command boundary:

```cpp
struct FoliageSpawnRequest {
    TerrainRuntimeHandle terrain;
    TerrainMutationRevision expectedRevision;
    FoliageTypeId foliageType;
    WorldCoordinate position;
    FoliageScale scale;
    FoliagePlacementSeed seed;
    FoliageLifetimePolicy lifetime;
    FoliageOverflowPolicy overflow;
    GameplayAuthorityContext authority;
};
```

Runtime-spawned instances live in a separate revisioned Terrain overlay; cooked cluster
artifacts remain immutable. The lifetime is explicit: cell-, session- or owner-bound
ephemeral state is excluded from slot saves, while a `DurableWorldDelta` creates an
authorized base-relative canonical spawn/update/tombstone through the Runtime Save/
Persistent World boundary. A baked removal references the exact base revision and stable
`FoliageInstanceId`; durable spawns use a distinct persistent mutation identity. Runtime,
render, Physics and Navigation handles never persist.

`TerrainRenderExtractor` merges the baked base and active overlay through one immutable
revisioned snapshot. Capacity exhaustion rejects atomically in the 1.0 baseline. Any
optional replacement policy must be registered/versioned with an eligible state class,
stable priority/tie order and persistence effect; it cannot evict baked/durable or another
owner's state. There is no implicit oldest-, furthest- or visibility-based deletion.

### LOD And Billboard Impostors

Foliage LOD uses:

- Mesh LOD chains per foliage type
- Billboard impostors for the farthest LOD
- Cross-fade between LOD levels to avoid popping
- Aggregate distance culling per foliage cluster

These are legal cooked representations and transition rules owned by Terrain content.
RenderFrontend selects among resident compatible representations per view. The 1.0
baseline uses bounded CPU selection; post-1.0 GPU selection remains renderer-owned and
cannot feed gameplay or World Streaming truth back synchronously.

Impostors are pre-baked at authoring time. The baker renders the foliage
mesh from multiple view angles and stores the result in a texture atlas.
Impostor rendering uses a single quad with atlas UV lookup and depth for
parallax correction.

### Wind Simulation

Wind is applied as vertex displacement in the foliage vertex shader:

```cpp
struct FoliageWindSettings {
    float  primaryStrength;
    float  secondaryStrength;
    float  primaryFrequency;
    float  secondaryFrequency;
    float  gustProbability;
    float  gustStrength;
    float  branchFlexibility;
    float  leafFlutter;
};
```

Wind parameters are configured per foliage type. Global wind direction and
base speed come from the scene's `WindComponent`. Wind is computed on the
GPU using procedural noise; no CPU simulation state per instance is
required.

### Editor Authoring

Terrain and foliage authoring tools are registered through the
`EditorPanelHost` extension system:

- **Terrain sculpt and paint tools** register as `ViewportPanel` overlay
  tools. The toolbar dispatches typed operations (`TerrainSculptOp`,
  `TerrainPaintOp`) through the `EditorToolbar` result channel. The
  viewport panel renders the tool-specific gizmo (brush radius indicator)
  and terrain overlay (height/weight visualization).
- **Foliage paint/erase/select tools** register similarly as
  `ViewportPanel` overlay tools. The foliage type palette is a separate
  `EditorTab` (`FoliagePaletteTab`) that publishes the active foliage type
  to the `EditorDataBus`.
- **Density visualization overlay** is rendered by the viewport panel as a
  post-process overlay, reading terrain density data from the terrain
  service. It does not require a separate tab.
- **Wind preview** is a viewport toggle that enables the `WindComponent`
  simulation in the editor viewport, animating foliage in real time for
  preview.

The `EditorToolbar` only produces typed results; terrain/foliage domain
operations are performed by the registered tools consuming those results.

Each canonical dataset and its dataset-local foliage placement source are edited through
one persistent asset-rooted authoring document. Reusable foliage-type definitions remain
separate referenced asset documents. The panel host owns route/focus/lifetime; document
services own source identity, revision, command/history, dirty/save/recovery/conflict
and derived preview state. Viewport tools, the foliage palette and overlays own input
and presentation only. They submit revision-checked `TerrainEditOperation` intent and
cannot mutate source, cooked tiles, TerrainRuntime or consumer-native state directly.

Interactive brush feedback is a disposable document interaction overlay. Higher-
fidelity preview captures one immutable source revision, uses the ordinary Terrain cook
contract and activates through an isolated generation-fenced preview session. Preview
success never clears dirty state, and stale results cannot become current by matching a
tile coordinate. Closing the document cancels owned work and retains its leases until
preview/runtime consumers acknowledge retirement.

Canonical source save is the shared atomic document-save path. Asset cook/publication,
editor autosave/recovery and ADR-140 Runtime Save foliage deltas are separate owners and
states. An intentional edit spanning a Scene binding and terrain/foliage source uses a
staged multi-document application transaction rather than two UI callbacks.

## Runtime Lifecycle And Readiness

TerrainRuntime uses the exhaustive aggregate states `Absent`, `Preparing`, `Prepared`,
`Active`, `Replacing`, `Suspended`, `Retiring` and `Failed`. Preparation validates the
Scene binding/effective plan, reserves all peak work and builds a detached candidate.
Workers return generation-tagged evidence only; the Terrain owner lane publishes the
candidate at the RuntimeScene safe point after every required participant is Prepared.

Terrain projects one revision-consistent leased source root into bounded consumer-specific
snapshots. Render receives ADR-139 candidates; Physics receives neutral collision shape/
material/subshape/hole inputs; Navigation receives neutral grounded-surface/hole/obstacle/
link inputs. Every snapshot carries Terrain/content/residency/mutation/capability, world/
cell/fence/request and origin evidence. It contains no mutable Terrain pointer or consumer-
native handle. TRF-005.2 freezes the exact collision/navigation projection schemas.

Each consumer prepares private state under its own worker/native affinity and returns an
immutable typed receipt through `Pending`, `Staged`/`StagedFallback`, `Prepared`,
`Published`, `Retiring` and `Retired` (or explicit `Unavailable`/`Failed`). `Prepared`
means all fallible work is complete and a no-fail owner-safe-point publication is ready;
it does not make state live. Render, Physics, Navigation and Terrain publish private roots
addressed only by one `ActivationTicket`. RuntimeScene/World Streaming exposes them only
after every required receipt matches and one aggregate root commits.

Readiness is an immutable generation-scoped snapshot with separate logical, streaming,
visual, collision, navigation and mutation dimensions. Each dimension is
`NotRequested`, `Preparing`, `Ready`, `Unavailable`, `Failed`, `Suspended` or
`Retiring`. Optional visual absence is valid for a headless server, while required
collision/navigation cannot be inferred from visual tile residency.

Failure or cancellation before publication destroys only candidate-owned state and
leaves the old Active generation unchanged. Cancellation after publication is an
explicit unload/replacement. Replacement retains the old dataset, snapshots, tiles,
GPU work, collision/navigation installs and module/provider dependencies until every
lease/fence retires; stale worker or participant evidence cannot publish by matching a
slot, coordinate or asset.

Required, Optional and Degradable criticality is frozen in the validated plan. Optional
absence is explicit; Degradable requires an exact predeclared substitute; a missing or
stale receipt is never success. After aggregate activation, required consumer/device loss
becomes a new Suspended/Failed/recovery or eviction transition rather than retroactive
rollback. Gameplay queries the committed aggregate capability and never probes a Physics
body, NavMesh, render result or decoded tile to infer another dimension's readiness.

Authored mutation goes through document/application commands and recook. Optional
runtime deformation/dynamic foliage uses a typed command with terrain generation,
expected mutation revision, bounded scope, gameplay authority, lifetime and overflow
policy. The owner builds a separate overlay candidate and atomically advances mutation
revision. Cooked data is never edited in place.

Foliage state is classified before commit. Baked instances are immutable content;
ephemeral overlays follow explicit cell/session/owner lifetime; durable mutations are
base-relative canonical deltas captured exactly once by Runtime Save/Persistent World.
Before cell eviction removes the last active dirty copy, Terrain publishes an immutable
delta root into the Persistent World ledger at the owner safe point. Failure/capacity
denial blocks retirement and keeps the source charged; it cannot drop state or force an
implicit user-slot save.

Shutdown closes admission, cancels/yields owned task groups, invalidates candidates,
requests exact-generation renderer/physics/navigation retirement and retains assets,
reservations and dependencies until all consumers acknowledge release. A deadline may
report `terrain.shutdown.incomplete`; it cannot force-free possibly referenced state.

## Memory And Streaming

World Streaming converts camera, gameplay, network and preload demand into typed,
generation-scoped Terrain residency requests. Terrain executes admitted work as a
bounded `Queued -> Reading -> Validating -> Decoding -> PreparingConsumers -> Prepared`
pipeline using immutable provider byte leases and cancellation tokens. It cannot create
a private camera-distance queue or begin allocation before the shared reservation is
accepted. Foliage clusters use the same authority and lifecycle.

“Terrain cache” must identify its owner. Assets owns immutable cook-cache entries and
published cooked generations; the Assets provider owns leased artifact/chunk bytes;
TerrainRuntime owns Horo-neutral decoded tile/cluster data; Render owns GPU residency;
Physics owns solver artifacts/bodies; Navigation owns provider artifacts and installed
query tiles. Each allocation has one charge and one release acknowledgement. Evicting
one class never implies release of another.

World Streaming owns the global cell priority queue and aggregate CPU/GPU/staging/
retirement ledger. Terrain owns only its provider-local cache policy within an admitted
slice. It cannot maintain a competing cell-demand scheduler, evict an activation-
critical pinned cell, borrow another feature's reservation or allocate before budget
growth is accepted. Every old/new replacement generation, decode/upload copy and
retiring resource remains accounted until its owner releases it.

### Streaming Budget

Terrain and foliage streaming budgets are **sub-allocations** carved from
the world-streaming system's `StreamingBudget`:

```cpp
struct TerrainStreamingBudget {
    ByteCount maxResidentTerrainBytes;
    ByteCount maxResidentFoliageBytes;
    uint32_t maxConcurrentTileLoads;
    uint32_t maxConcurrentRetirements;
};
```

The world-streaming `StreamingBudget` owns the total memory cap and reserves
slices for terrain and foliage. The resolved byte/count budget is a capability token,
not an additional allowance. Terrain may evict only disposable provider-local detail
inside its allocated slice; pressure involving a pinned/activation-critical cell is a
typed request to World Streaming. Foliage instance buffers are loaded per-cluster with
the priority assigned by World Streaming. Terrain tiles and foliage clusters are
payloads within `StreamingCell` objects. Active cells, replacement candidates, immutable
snapshots and consumer installs pin their exact dataset generation and leases.

Hot replacement validates a complete candidate generation before publication. A changed
tile expands to its manifest-declared seam/dependency closure; exact-signature matches
may reuse old immutable payloads, while incompatible neighbors prepare together. World
Streaming commits the aggregate cell only after every required Terrain, Render, Physics
and Navigation participant is prepared. Failure or cancellation discards candidate-
owned state and preserves the prior Active generation. Old generations retire only
after all byte, snapshot, native-resource and provider leases acknowledge release.

## Feature Tiers

Terrain tiers are provider-neutral product preference profiles: `Baseline`,
`Standard`, `High` and `Ultra`. They do not name graphics APIs, platforms, shader
models or devices and do not grant capability.

Resolution intersects the exact cooked variants with TerrainRuntime, World Streaming,
Render, Physics, Navigation, host-mode and product-policy capabilities. The immutable
result records the selected tier, exact finite layer/LOD/instance/byte/work limits,
enabled algorithms, all provider/cooked revisions and every explicit fallback reason.
TRF-001.3 owns the versioned numeric table and shared validator.

The version-1 canonical ceilings are fixed below. Byte values are binary MiB;
projects may lower any field, but cannot widen a field beyond the exact selected tier.
These ceilings bound descriptor admission and remain distinct from ADR-143 qualification
workload requirements.

The foliage cluster, instance and resident-byte limits form one optional group. A
project may set all three to zero to disable foliage explicitly; partially zero groups
are invalid. Every Terrain, staging, retirement and work ceiling remains strictly
positive, and zero never means unlimited.

| Limit | Baseline | Standard | High | Ultra |
|---|---:|---:|---:|---:|
| Height samples per axis | 4,097 | 8,193 | 16,385 | 32,769 |
| Tile interior quads | 128 | 128 | 256 | 256 |
| LOD levels / layers per tile | 4 / 4 | 6 / 8 | 8 / 12 | 12 / 16 |
| Active terrain tiles | 256 | 512 | 1,024 | 2,048 |
| Active foliage clusters / instances | 1,024 / 262,144 | 2,048 / 524,288 | 4,096 / 1,048,576 | 8,192 / 2,097,152 |
| Resident Terrain / Foliage MiB | 256 / 256 | 512 / 512 | 1,024 / 1,024 | 2,048 / 2,048 |
| Staging / retiring MiB | 128 / 128 | 256 / 256 | 512 / 512 | 1,024 / 1,024 |
| Bounded work items | 1,048,576 | 2,097,152 | 4,194,304 | 8,388,608 |

`TerrainConfigurationSnapshot` captures the exact table revision, project
configuration revision, effective capability revision, selected tier and project-
lowered limits. `TerrainDatasetDescriptor` captures stable dataset/content identity,
revisioned inclusive integer-millimetre bounds, regular-grid shape and the combined
Terrain/Foliage footprint. Both values are fixed-size inert data. Their validators do
not allocate, register, activate a provider, inspect a service locator or publish state.

`ResolveTerrainFeatureTier` accepts only the exact requested tier when it is present in
the captured content/host set. It does not interpret tier ordering. Insert admission
requires no current publication; replacement requires the exact current content and
its non-wrapping successor. Unchanged bounds retain their revision, while changed
bounds require its exact successor. Cancelled, Closing and Closed owners reject
admission before work.

| Capability family | Baseline | Standard | High | Ultra |
|---|---|---|---|---|
| Heightfield/layer/LOD scale | Minimal product-required cooked path | Increased finite authored/cooked limits | Higher finite quality limits | Highest qualified finite product limits |
| Foliage scale/culling | Bounded required representation with declared CPU/GPU path | Larger qualified plan and optional GPU culling | High-density qualified GPU path where supported | Maximum explicitly qualified plan, never “unlimited” |
| Wind/impostor quality | Minimal cooked/vertex path when required | Optional cooked impostor and richer wind variants | Higher qualified visual recipe | Highest qualified optional recipe |
| Collision/navigation | Independently required or unavailable by scene/host policy | Same ownership; quality may use a higher cooked variant | Same | Same |
| Runtime deformation/spawn | Explicit optional capability, not implied | Explicit optional capability | Explicit optional capability | Explicit optional capability |
| Editor authoring | Application/editor permission and host capability, not runtime tier | Same | Same | Same |

Required content that cannot fit the selected plan fails cook/activation with a typed
result. Optional reduction occurs only when the product declares an ordered fallback
and the compatible cooked variant exists. Runtime never silently clamps layers, drops
instances, removes collision, changes renderer/provider or switches CPU/GPU algorithms
to make a tier appear successful.

### Qualification Profiles And Budgets

Feature tiers are not benchmark profiles. ADR-143 defines two version-1 qualification
workloads that fix content, host/recipe, environment cohort and measurements:

| Required envelope | `TerrainCore1_0` | `TerrainHighEnd1_0` |
|---|---:|---:|
| Tile interior/stored samples | 128 x 128 quads / 129 x 129 samples | same |
| Simultaneously Active terrain tiles | 256 | 1,024 |
| Active foliage clusters / instances | 1,024 / 262,144 | 8,192 / 2,097,152 |
| Terrain/Foliage steady resident bytes | 256 MiB / 256 MiB | 1,024 MiB / 1,024 MiB |
| Staging / retiring allowance | 128 MiB / 128 MiB | 512 MiB / 512 MiB |
| Newly committed resident payload per frame/tick epoch | 4 MiB | 16 MiB |

Both qualify ADR-139's bounded CPU-selected visibility/LOD/seam and direct/instanced
render recipe. GPU Scene, compute selection and indirect command generation are outside
these workload identities. Post-1.0 GPU-driven support requires a new explicit recipe,
capacities, overflow/synchronization/fallback rules and backend cohorts; it cannot
silently satisfy or alter a version-1 result.

Cold cook floors are 16/64 published terrain tiles per second and 250,000/1,000,000
deterministically evaluated foliage placements per second for core/high-end protected
runner cohorts. Standard editor gestures require p95 overlay/commit latency no greater
than 16.67/100 ms for core and 8.33/50 ms for high-end. Headless fixed-tick Terrain
owner work is capped at p95 0.50/1.50 ms; visual CPU work, GPU bytes and draw/dispatch/
submit counts are exactly zero. Required metric absence invalidates qualification rather
than becoming zero.

These numbers are workload gates, not automatic runtime allowances or TRF-001.3
descriptor maxima. World Streaming retains aggregate admission, every byte is charged
once through its existing ledger, and the high-end envelope requires an explicitly
larger product/runner global budget.

## Verification

Required coverage includes:

- target/dependency checks for backend-neutral TerrainApi/Runtime and no native/editor/
  service-locator leakage;
- strong-type separation for dataset/tile/type/cluster/instance identity, runtime
  handles, content/residency/mutation/capability revisions and leases;
- malformed, oversized and incompatible source/cooked/scene descriptors with bounded
  decode and no mutable whole-dataset buffer crossing the public boundary;
- canonical dataset membership/order, tile digest/bounds/seam/dependency validation,
  complete cook-key invalidation and byte-identical deterministic recook/cache reuse;
- independent corruption/missing-artifact rejection and package-time verification with
  no directory enumeration or mixed-generation tile admission;
- every tier/cooked/provider capability combination, required failure and each declared
  fallback reason without silent clamp/drop/provider selection;
- view-independent render-candidate extraction, deterministic capacity failure, multi-
  view reuse and strict separation of residency, presentation visibility and gameplay;
- Terrain tier/render profile independence, material/pass classification, CPU 1.0 LOD/
  seam planning and optional post-1.0 GPU recipe admission/fallback;
- candidate failure/cancellation and replacement at every Scene/streaming/render/
  physics/navigation prepare/commit/retirement boundary;
- stale worker, snapshot, tile, GPU, collision and navigation evidence rejected across
  generation/revision changes;
- runtime mutation authority/revision/capacity/overflow cases with no partial update,
  cooked-source mutation or implicit foliage eviction;
- deterministic placement across worker/order/locale variations, baked/ephemeral/durable
  identity separation and base-relative save delta golden fixtures;
- active-to-dormant dirty foliage handoff, SaveCaptureEpoch one-copy capture, base-remap
  incompatibility and eviction pressure with no persistent state loss;
- consumer-specific immutable snapshot and typed receipt state machines, stale/duplicate/
  out-of-order evidence, criticality/fallback matrices and aggregate activation roots;
- adversarial Render/Physics/Navigation/Terrain safe-point ordering with no partial live
  state, complete pre-commit rollback and post-commit runtime failure transitions;
- world-streaming reservation/growth/pressure accounting for staging, old/new and
  retiring generations without double charge or oversubscription;
- source/cook/provider-byte/decoded/GPU/Physics/Navigation cache ownership, pinning and
  release tests, including pressure and replacement seam-closure cases;
- headless, dedicated server, editor preview, Null Render and every interactive backend
  consuming the same Horo contract; and
- bounded owner/worker/native-thread handoff, frame-hot allocation/I/O checks, device
  loss, world unload, repeated shutdown and retirement timeout;
- exact bounded affected-patch planning for every authoring tool, no whole-heightfield
  history entry, deterministic apply/undo/redo symmetry and one commit per gesture;
- authoring failure/cancel/stale completion preserving source revision, dirty state,
  history and notifications at every staged boundary; and
- source save, autosave/recovery, transient preview cook, published cook and Runtime Save
  state separation, including isolated preview teardown and stale-result rejection;
- exact core/high-end active-count, resident/staging/retirement and per-epoch streaming
  boundaries, including one-over-limit denial with checked accounting;
- protected-cohort cold cook, editor p95 and headless overhead gates with required metric
  availability, fixed fixtures and no invalid sample removal; and
- CPU-selected 1.0 recipe enforcement plus rejection of undeclared GPU-driven/fallback
  work under the same workload identity.

## Related Documents

- [Rendering Architecture](./rendering-architecture.md): terrain shader integration,
  GPU-driven rendering, `RenderWorldSnapshot` and `RenderInstance` model
- [Material And Shader Model](./material-and-shader-model.md): terrain material
  functions, `ShaderPermutationKey`, feature flags for layer count
- [World Streaming Architecture](./world-streaming-architecture.md): terrain tile and
  foliage cluster streaming, `StreamingBudget` sub-allocation
- [Physics Architecture](./physics-architecture.md): terrain collision geometry,
  foliage collision opt-in, hole mask propagation
- [Navigation And AI Architecture](./navigation-and-ai-architecture.md): NavMesh
  generation from terrain heightfield, hole mask exclusion
- [LOD And Culling Architecture](./lod-and-culling-architecture.md): foliage LOD chains,
  impostor baking, GPU-driven culling pipeline
- [Decal System Architecture](./decal-system-architecture.md): terrain decal projection,
  foliage decal passthrough
- [VFX And Particles Architecture](./vfx-and-particles-architecture.md):
  foliage-interacting VFX, debris on foliage collision
- [Editor Panel Host](../editor/editor-panel-host.md): terrain/foliage tool
  registration as `ViewportPanel` overlays and `EditorTab` panels
- [Editor Document Model](../editor/editor-document-model.md): undo system and
  terrain data model snapshots
- [ADR-137](../../adr/137-terrain-foliage-ownership-data-tier-and-lifecycle.md):
  module/data authorities, typed identity and revisions, provider-neutral tier plan,
  aggregate lifecycle, readiness and shutdown baseline
- [ADR-138](../../adr/138-terrain-source-cooked-tile-cache-and-streaming-ownership.md):
  canonical source and deterministic tile cooking, cache authorities, World Streaming
  residency, seam-safe replacement and generation retirement
- [ADR-139](../../adr/139-terrain-render-extraction-material-lod-and-tier-boundary.md):
  immutable render candidates, material/permutation boundary, renderer-owned per-view
  visibility/LOD, independent profile axes and CPU/GPU recipe split
- [ADR-140](../../adr/140-foliage-placement-baked-dynamic-state-and-eviction-ownership.md):
  deterministic placement, immutable baked base, ephemeral overlays, durable canonical
  deltas, capacity policy and no-loss cell eviction
- [ADR-141](../../adr/141-terrain-foliage-cross-system-ownership-and-readiness.md):
  consumer snapshots and receipts, owner-safe-point staging, aggregate readiness,
  rollback, runtime failure and reverse-dependency retirement
- [ADR-142](../../adr/142-terrain-foliage-document-tool-undo-and-preview-ownership.md):
  persistent authoring documents, typed tool routing, bounded tile-patch undo/redo,
  isolated preview and source-save/cook/runtime-persistence separation
- [ADR-143](../../adr/143-terrain-foliage-scale-budgets-observability-and-feature-boundary.md):
  core/high-end scale, memory, streaming, cook, editor and headless gates, required
  observability and core-1.0 versus post-1.0 recipe qualification
