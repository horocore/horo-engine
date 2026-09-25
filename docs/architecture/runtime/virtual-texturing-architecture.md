# Virtual Texturing Architecture

## Purpose And Decision Authority

Virtual texturing is an optional Post-1.0 runtime vertical slice that decouples
logical texture address space from resident GPU backing. It coordinates cooked page
artifacts, bounded demand, logical residency intent, renderer realization and material
sampling without taking ownership from Assets, Materials, World Streaming, Renderer
or source-domain producers.

[ADR-164: Virtual Texturing Ownership, Product Scope and Capability Tier](../../adr/164-virtual-texturing-ownership-product-scope-and-capability-tier.md)
is the normative ownership, composition, product-scope, capability and lifecycle
decision. Exact identities, numeric bounds, artifact formats, queues, feedback policy
and renderer contracts are assigned to the dependent VTX tickets. Illustrations in
this document do not create a second contract.

## Ownership Summary

| Domain | Owns | Does not own |
|---|---|---|
| VTX API/runtime | Logical identities/generations, bounded demand merge, page choice, feature-local eviction and residency snapshots | Files, material definitions, world cells, native GPU resources or a global scheduler |
| Assets/Pipeline | Source identity, deterministic cook, immutable artifact publication and bounded byte leases | Runtime demand, logical residency or GPU mapping |
| Materials | Authored semantic slots, sampling intent and required/fallback variants | Page priority, physical slots or feedback state |
| World Streaming | Cell relevance, aggregate admission and activation/retirement barriers | Per-page policy or a second VTX state machine |
| Renderer | GPU reservations, physical atlas/sparse backing, uploads, mappings, graph passes and safe retirement | Logical page identity, demand policy or artifact discovery |
| Producers | Source-domain meaning and immutable payload contribution | Global admission, package paths or GPU allocation |
| Host/application | Composition, product policy and effective-tier admission | Feature/runtime state after ownership transfer |

The host composes VTX through typed ports. VTX uses Assets for page bytes, the injected
ADR-010 job contract for bounded preparation, ADR-034 reservations for GPU work and
Renderer requests for realization. It never opens a project/build/cache path, queries a
service locator, creates its own global worker pool or exposes a backend-native handle.

## Data And Runtime Flow

Authored content is deterministically cooked into immutable target/capability-keyed
artifacts. Runtime data moves through generation-checked typed requests:

```text
material/producer/world demand snapshots
  -> VTX bounded demand merge and logical page plan
  -> Assets page-byte lease
  -> Renderer admitted upload and mapping
  -> VTX generation-checked residency commit
  -> immutable residency/binding projection
```

Demand is evidence, not authority. Renderer completion becomes visible only when it
matches the active VTX generation and all required leases/readiness records. Workers
may prepare immutable data, but VTX owner safe points commit logical state and Renderer
safe points mutate GPU state.

Replacement admits old/new peak overlap and publishes atomically. Cancellation
invalidates the operation/generation before late completions arrive. Stale work releases
its asset leases and reservations without publication. Physical slots and resources are
reused only after the relevant GPU work and dependent leases retire.

## Source, Artifact And Page-Store Model

[ADR-165: Virtual Texture Source, Cooked Artifact, Page Store and Cache Ownership](../../adr/165-virtual-texture-source-cooked-artifact-page-store-and-cache-ownership.md)
is the normative source/cook/storage decision. Assets owns tracked source identity,
generic cook scheduling/cache/staging, atomic generation publication, packages and
runtime byte leases. VTX import/cook owns page geometry, mip/tail, border, color,
encoding and deterministic aggregation semantics.

One immutable cooked generation contains a root manifest and canonically ordered,
bounded page-pack artifacts. A logical page is the independently requested and
verified content unit; a pack is the range-addressable Assets storage unit. Pages are
not independent authored assets or mutable side files. The manifest is the sole
membership/order authority and records exact pack/page ranges, costs and digests.

Runtime pins an exact published/package generation and requests finite ranges through
an Assets-owned provider port. Local filesystem, archive, memory/test and future remote
providers have the same semantics. VTX never constructs paths, asks for an ambient
"current" generation, enumerates storage or combines packs from different generations.

The cook cache, published generation, provider byte cache, VTX decoded-page cache and
Renderer physical-page cache are distinct owner/lifetime domains. Eviction in one does
not imply publication, invalidation, unmapping or capacity release in another.

## Memory And World-Streaming Boundary

[ADR-034: GPU Memory and Residency Ownership](../../adr/034-gpu-memory-and-residency-ownership.md)
owns GPU allocation, reservations and pressure policy. VTX selects disposable pages
within an admitted allowance; Renderer owns native atlas/sparse backing and safe mapping
execution. Evicting a logical page from an allocated atlas frees a slot, not physical
heap capacity.

For streamed worlds, VTX consumes World Streaming's aggregate reservation through a
host-composed adapter. Neither VTX page eviction nor Renderer pressure independently
evicts a world cell. Activation-critical page failure participates in the cell barrier;
optional content may use only a fallback variant admitted before activation.

[ADR-166: VTX Feature-Local Residency and Eviction Within Global Reservations](../../adr/166-vtx-feature-local-residency-and-eviction-within-global-reservations.md)
defines the execution boundary. World Streaming/host policy owns global CPU, I/O,
decoded/staging memory, GPU, queue and frame-work admission. VTX may merge demand,
prioritize, pin and evict pages only inside a generation-scoped multidimensional
reservation. Renderer separately owns physical GPU claims and retirement.

Pins are typed owner leases, not booleans. Repeated compatible demand coalesces, shared
pages retain one charge identity plus consumer leases, and only unpinned disposable
pages are eviction candidates. Eviction first revokes logical availability, then waits
for Assets, worker, snapshot and Renderer acknowledgements. `Evicting` state remains
charged; cancellation or page-table removal alone never refunds capacity.

## Feedback, Camera Hints And Prediction

[ADR-167: VTX Feedback, Readback, Prediction and Camera-Data Ownership](../../adr/167-vtx-feedback-readback-prediction-and-camera-data-ownership.md)
owns this boundary. VTX contributes a bounded semantic feedback plan. Renderer owns
render-graph placement, GPU feedback/compaction/copy resources, asynchronous readback
and retirement, then publishes immutable delayed observations with exact texture,
device, frame and view generations.

Camera/view owners publish immutable per-view snapshots with explicit cut, teleport,
origin-rebase, projection and dynamic-resolution discontinuities. Producers publish
typed bounded hints with provenance. VTX retains neither live Camera/Scene pointers nor
producer containers and cannot select camera authority.

VTX deterministically merges observations and predicts bounded demand candidates.
Feedback and prediction are evidence only: candidates still pass ADR-166 coalescing,
pin, priority and reservation policy. Missing/sampled/overflowed/cancelled observations
carry explicit completeness; normal frames never wait for readback or allocate a larger
buffer in response to overflow.

## Renderer Realization And Material Sampling

[ADR-168: VTX GPU Page Table, Physical Cache, Shader and Material Ownership](../../adr/168-vtx-gpu-page-table-physical-cache-shader-and-material-ownership.md)
defines the realization boundary. VTX emits finite generation/revision-scoped logical
map/unmap intent without physical slots or handles. Renderer owns page-table and
atlas/sparse resources, uploads, mapping, descriptors, graph work, synchronization and
GPU-safe retirement, then returns a backend-neutral completion.

`Atlas` and `Sparse` realize one canonical sampling contract and are admitted/
qualified independently. A frame captures one immutable binding snapshot; upload
finishes before mapping visibility, unmap precedes physical reuse, and old resources
remain leased through every submitted reader. No partial mapping revision is visible.

Materials own semantic VTX slots, sampling intent and cooked required/fallback
variants. They serialize no descriptor index, physical coordinate or native handle.
Shader variants are produced offline under ADR-035; a page miss may emit bounded
feedback and use a declared fallback, but cannot compile, allocate or mutate mappings.

## Producers, Packaging And Servers

[ADR-169: VTX Producer, Terrain, World Streaming, Packaging and Server Ownership](../../adr/169-vtx-producer-terrain-world-streaming-packaging-and-server-ownership.md)
defines producer and product integration. Generic texture/material and Terrain adapters
capture exact immutable source/dependency generations and return detached canonical page
inputs or bounded runtime hints. They never push mutable pages, paths or live domain
objects into VTX. Terrain remains canonical Terrain authority; VTX pages are derived
visual artifacts.

World Streaming requests VTX readiness inside its cell reservation and alone commits or
evicts the aggregate cell. Release walks typed published dependencies to include exact
VTX manifests/packs, Material/shader variants and every allowed fallback. It never scans
cook caches or page directories.

Dedicated/headless runtime products resolve VTX to `Unavailable` and exclude client-only
page packs, GPU variants, feedback and residency. Listen servers include them only for
the explicitly composed local graphical client scope. Cook/validation tools may use
non-rendering VTX contracts without advertising server runtime support.

## Settings, Tooling And Qualification

[ADR-170: VTX Settings, Diagnostics, Capture and Qualification Ownership](../../adr/170-vtx-settings-diagnostics-capture-and-qualification-ownership.md)
defines typed preflight, immutable VTX/Renderer snapshots, low-cardinality telemetry,
authorized commands, finite private captures and native release evidence. UI, CLI and
MCP use the same queries/commands and never inspect mutable/native state.

Page/texture/cell identities are prohibited metric dimensions; detail uses paginated
snapshots, sampled events or explicit captures. Captures exclude paths, payload bytes,
raw camera trajectories and native handles by default. Null tests prove contracts only;
Atlas/Sparse product claims require target/backend/tier native qualification.

## Capability And Product Scope

VTX uses feature-local tiers:

| Tier | Meaning |
|---|---|
| `Unavailable` | VTX assets are rejected; supported ordinary texture/material variants remain available |
| `Atlas` | An explicitly budgeted physical atlas and logical page-table path is implemented and admitted |
| `Sparse` | Qualified native sparse/tiled backing satisfies the same VTX semantics |

Content policy (`Required`, `OptionalWithFallback`, `Disabled`) is separate from the
tier. Effective support is the intersection of compiled implementation, selected
backend/device operations, formats and limits, budgets, shader/material variants,
cooked artifacts and product policy. Backend names, profile ranks and extension bits do
not prove support. Sparse-to-atlas fallback requires independent atlas admission.

Horo 1.0 does not require VTX. VTX runtime delivery remains Post-1.0 and depends on the
RND-010.9 sparse/atlas residency foundation. Headless and dedicated-server hosts do not
initialize GPU residency; validation/cook tools may compose narrow non-rendering
contracts explicitly.

## Unsupported Shortcuts

- direct runtime filesystem/cache-path access or loose-file discovery;
- process-global VTX schedulers, renderer selection or memory ledgers;
- backend/API names and native handles in public/persisted contracts;
- runtime cooking or shader generation after a page miss;
- silent capability, quality or content fallback;
- unbounded work/queues, normal-frame blocking waits or frame-index-only reuse; and
- debug UI state acting as runtime authority.

Failures are typed and observable, including unsupported composition, missing artifacts
or variants, invalid descriptors, capacity, stale generation, cancellation, I/O,
decode, renderer admission, upload/mapping, device loss and stalled retirement.

## Related Documents

- [ADR-164: Virtual Texturing Ownership, Product Scope and Capability Tier](../../adr/164-virtual-texturing-ownership-product-scope-and-capability-tier.md)
- [ADR-034: GPU Memory and Residency Ownership](../../adr/034-gpu-memory-and-residency-ownership.md)
- [Virtual Texturing Debug UI Reference](../../../mock-studio/designs.md#architecture-runtime-virtual-texturing-debug)
- [Rendering Architecture](./rendering-architecture.md): virtual texture realization and passes
- [Material And Shader Model](./material-and-shader-model.md): semantic virtual-texture slots and variants
- [Asset Pipeline](./asset-pipeline.md): cooked artifacts and bounded byte access
- [World Streaming Architecture](./world-streaming-architecture.md): aggregate admission and cell barriers
- [Terrain And Foliage Architecture](./terrain-and-foliage-architecture.md): typed producer integration
- [LOD And Culling Architecture](./lod-and-culling-architecture.md): bounded demand inputs
