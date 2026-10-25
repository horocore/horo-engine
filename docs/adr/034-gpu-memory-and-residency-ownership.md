# ADR-034: GPU Memory and Residency Ownership

- **Status**: Proposed
- **Date**: 2026-08-31
- **Supersedes**: None
- **Scope**: GPU backing accounting, reservation transactions and pressure policy
- **Issue**: [RND-010.1](https://github.com/HoroCore/horo-engine/issues/358)
- **Jira**: [HORO-358](https://horo-engine.atlassian.net/browse/HORO-358)
- **Normative document**: [Rendering Architecture](../architecture/runtime/rendering-architecture.md)

## Context

[ADR-027](027-renderer-resource-identity-and-descriptors.md) defines resident
identity and retirement, including `ResourceOperationId` as the registry
completion identity for one reserved generation. ADR-027 leaves physical
allocation and budget policy to RND-010. Budget denials, capacity failures and
stalled-retirement diagnostics are typed results under
[ADR-008](008-error-model-exception-boundary-and-registry.md); they do not reuse
resource-handle invalidity as the memory-policy error. The current public
`IRenderBackend` has lifecycle and execution-plan contracts, not a heap allocator
or memory ledger. Native viewport and GUI allocations still exist in the OpenGL
and Metal editor integrations. This ADR defines their target ownership; it does
not claim those paths already enforce the decision or implement new runtime APIs
in M0.

GPU memory is shared with other applications and may be shared with the CPU.
Native allocation sizes, placement, and residency mechanisms differ by backend.
At the same time, [World Streaming](../architecture/runtime/world-streaming-architecture.md#budget-reservation-and-admission)
already owns aggregate cell admission and forbids providers from expanding their
allowances. Independent renderer, cell, and virtual-texture budgets would allow
overcommit or count the same backing allocation more than once.

## Decision

### 1. Separate policy, accounting, and native allocation

| Responsibility | Owner | Boundary |
|---|---|---|
| Finite memory envelopes, emergency headroom, quality/failure policy and distribution between worlds/editor/services | Application host | Explicit composition/configuration; no process-global service discovery or API-name-derived budgets. |
| Renderer reservation ledger, logical resource readiness, reconstruction records and eviction eligibility | Render frontend | Typed Horo contracts; no native heap values, world-cell selection, or asset I/O. |
| Allocation requirements, memory pools/heaps, native placement, mapping/coherency, fences and destruction | Selected render backend | Backend-private implementation; cannot invent higher budgets or discard live resources. |
| Aggregate cell CPU/GPU/staging admission and cell residency/eviction | StreamingPartitionAuthority | Renderer resources consume its reservations; renderer pressure is input to this authority, not permission to evict a cell. |
| Texture-page/LOD/cache selection within an allowance | Owning feature, such as Virtual Texturing or VFX | Uses admitted renderer backing; cannot claim more memory or invalidate required cell content. |
| Source/cooked identity, storage and asynchronous loading | Asset Pipeline | Supplies reconstruction data; `AssetId` is not a GPU allocation. |

Foundation supplies existing allocation domains, results, jobs and observability
primitives. It does not acquire a dependency on renderer heaps or world policy.
Hosts that use several frontends/devices on the same physical memory pool must
divide one host envelope into non-overlapping allowances. A second preview or
world cannot receive the full device budget again. Opaque pool identities are
process-local; native adapter identities stay behind the backend boundary.

### 2. Account backing capacity, not just descriptor payload

Each charged allocation has one owner scope and generation-safe reservation
identity. The renderer tracks distinct quantities: reserved but unallocated
capacity, committed backing capacity, live payload, pending upload/readback,
retiring capacity, and reusable slack. Live payload and retiring/slack breakdowns
are views of committed capacity, not additional totals. Committed capacity plus
unallocated reservations must fit the owner allowance and enclosing hard caps.

Whole native heap/block capacity is charged, including padding and unused space.
A suballocation consumes existing charged slack without charging the block again.
Releasing a suballocation does not return block capacity to the enclosing budget;
that happens only when the block is actually destroyed, or ownership is explicitly
transferred to another admitted scope. Shared pools therefore have a separately
admitted owner and leases; they cannot hide unused blocks in a departing cell.
Dedicated allocations follow the same rule.

Empty-block reclaim is required. A block becomes destroy-eligible only when it
has no live suballocations and every GPU/CPU reader of its last contents has
retired. Destroy runs on the render-capable owner thread at a `RenderSafePoint`,
inside the same per-frame destruction budget as pressure step 1. M0 does not add
a background job that relocates persistent allocations. Moving live persistent
resources is a later optional path and must use ADR-027 new-generation
replacement with admitted overlap. Unused empty blocks must be destroyed; slack
inside a still-occupied block stays charged. Long-running worlds and dedicated
servers cannot rely on process exit to return that capacity. RND-010.2 measures
block sizes; it does not make empty-block reclaim optional.

Descriptor bytes, allocation bytes, and process/device telemetry are separately
named and never summed as though they were disjoint allocations.

UMA CPU/GPU mappings of the same physical backing count once in an aggregate
physical-memory total; CPU/GPU accessibility are attributes. Separate decoded,
upload and device copies count separately even if they contain identical data.
Every pool report identifies its accounting domain and whether domains overlap.
Discrete local/non-local pools have separate constraints; their capacities are
not interchangeable. Renderer metadata, retained CPU reconstruction data, queue
storage and emergency reporting storage also consume their host CPU allowances.

For explicit APIs, backend requirement queries supply size/alignment/type
constraints before allocation. Where native backing size is opaque, the backend
uses a documented conservative charge model for Horo-requested resource storage
and marks it estimated. Unknown descriptor cost is rejected, never zero. Hidden
driver/compositor overhead cannot be given an exact application hard cap: reserve
host headroom, expose observed process usage when available, and retain failure
handling. The ledger is an admission guarantee for accounted Horo costs, not a
promise that every native allocation succeeds or that all driver bytes are known.

### 3. One reservation transaction across streaming and rendering

A world-streaming GPU reservation is a claim against the renderer's host envelope
as well as a component of the cell aggregate. The two ledgers project the same
charge identity; they are not additive physical allocations or independent grants.
Non-streamed resources use an explicit host/editor/service owner scope.

1. A host-composed provider adapter obtains a backend cost plan and reserves the
   cell aggregate and renderer claim before starting native allocation/upload.
   No allocation occurs while either admission is missing. A rejected second
   admission releases the unused first claim; queue and completion capacity are
   reserved as part of the transaction. Neither owner calls into the other while
   holding a lock or waits synchronously for its thread.
2. Claims name the scope incarnation, device owner, resource attempt and budget
   revision. Issued claims remain charged across revisions; lowering a cap does
   not erase them. Stale/cancelled attempts cannot publish into a new world/device.
3. Allocation consumes a claim, rather than adding a second resident charge.
   Additional requirements, new heap growth, replacement overlap and scratch
   request growth **before** allocation. If denied, defer within a bounded queue
   or fail that attempt; never overshoot and report it afterward.
4. Success publishes only after required uploads and native dependencies are
   usable. Cancellation/failure releases unused capacity but keeps submitted work,
   partial native allocations and staging charged until acknowledged retirement.
5. Shared ownership transfer needs destination admission before source release.
   Accounting records move once; leases do not multiply charges. Cell retirement
   follows [ADR-012](012-world-streaming-partition-authority-and-subsystem-boundaries.md)
   acknowledgement barriers (`BeginRetire` / `PollBarrier`): a cell stays
   `Evicting` until Scene removal and every started provider is `Retired`,
   including shared-owner transfer.

For example, a charged 64 MiB pool containing 20 MiB of live textures still costs
64 MiB, not 20 or 84 MiB. Removing one 8 MiB texture produces reusable slack after
retirement; it does not free 8 MiB of physical capacity from that pool's charge.
An unrelated cell cannot spend that slack without an admitted lease/transfer.

### 4. Memory classes and allocator strategy

Public descriptors express usage and lifetime, not Vulkan memory-type indices,
D3D12 heap flags or Metal storage modes. Backend-private pools separate persistent
device resources, upload, readback, and transient work by compatible requirements
and charged owner. Mapping never bypasses backend flush/invalidate requirements.

Use backend-owned suballocation for compatible frequent small allocations where
the API exposes it; use dedicated allocations when required or justified by size,
lifetime or fragmentation. Exact block sizes and allocator data structures are
implementation choices to measure in RND-010.2, not public constants. No new
allocator dependency is selected here. A helper library, if adopted, remains
private, pinned and subject to the same accounting and lifetime contracts.

Upload/readback arenas are bounded. Reuse waits for the actual last GPU/CPU reader,
not merely a frame-number modulo. Readback completion produces an owned or leased
result with a bounded retention policy; forgotten consumers cause backpressure,
not reuse beneath them. Transient aliasing requires a render-graph lifetime proof
including asynchronous queues, native compatibility and synchronization. If that
proof or backend support is absent, allocate non-aliased backing **within budget**
or reject the plan. Aliasing cannot be assumed to make an otherwise oversized plan
fit. Persistent resources cannot move silently: compaction/replacement uses the
new-generation and dependent-rebuild rules of ADR-027, with overlap admitted.

### 5. Dynamic budget observations and backend parity

[ADR-028](028-renderer-capability-limits-and-product-profiles.md) treats available
memory as dynamic telemetry, not an immutable capability or guaranteed reservation.
The backend reports pool identity, sample revision/time, estimated usage and
available budget when known, and explicit unavailable/estimated provenance.
Observations do not replace the renderer ledger or increase a configured cap.

Admission checks the hard accounting cap and, when a usable native observation
exists, observed headroom after host emergency margin and reservations not yet
reflected in that observation. Check order is fixed: (1) hard accounting cap and
enclosing cell/host allowances, then (2) usable native observed headroom. If both
would fail, the primary typed result is the accounting or cell-budget denial.
Native missing-headroom or OOM is a distinct secondary diagnostic, never a
substitute that hides the ledger failure. If accounting admits the request and
only native observation fails, report the native-headroom or OOM-class result.
Device loss remains ADR-027 reconstruction, not a budget code. Do not subtract
accounted bytes from native usage twice. When overlap cannot be established,
conservatively withhold that headroom
until a fresh sample rather than granting speculative capacity. Reclamation is
not treated as observed native relief before acknowledgement/sample evidence.
Missing telemetry uses the finite host cap and reserve; it is not unlimited memory
and does not itself disable the backend. Settings define the sample interval,
maximum sample age, margins and pressure hysteresis; invalid/missing required
settings fail composition. No product-tier name supplies an implicit byte count.

| Backend | Required realization of this policy |
|---|---|
| OpenGL | Keep allocation/context calls private and honor declared charges and failures. Budget telemetry may be unavailable; baseline cannot require a vendor memory extension or claim an exact physical residency API. |
| Metal | Charge resources/heaps, distinguish shared backing from duplicate storage, and treat working-set observations as estimates. Do not use a recommendation as reserved memory. |
| Vulkan | Select compatible memory types/heaps, obey requirement/alignment and allocation-count limits, and suballocate where appropriate. `VK_EXT_memory_budget` is optional telemetry, not an added ADR-031 baseline requirement. |
| D3D12 | Charge committed allocations/whole placed-resource heaps against their pools and observe DXGI budgets. Native residency readiness is a backend submission prerequisite, not a replacement for Horo logical readiness. |
| Null | Exercise the same reservations, queue limits, generations and retirement contract using injected costs, pressure and completion ticks. It owns no real GPU heap and proves no hardware capacity or performance claim. |

The Vulkan budget extension describes changing guidelines and estimated usage;
see [Khronos memory-budget contract](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_budget.html).
DXGI budgets can change and reservation is not a guarantee; see
[Microsoft residency guidance](https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency).
Metal's threshold is approximate; see
[Apple working-set guidance](https://developer.apple.com/documentation/metal/mtldevice/recommendedmaxworkingsetsize).
These facts justify conservative admission; the host caps and failure policy here
are Horo decisions, not vendor guarantees.

### 6. Residency, pressure and failure policy

Logical readiness remains ADR-027's `Pending/Ready/Retiring/Retired/Failed` model.
An unmaterialized asset/cache key is not a dormant reusable resource handle.
Eviction logically retires the generation; reloading creates a new one. Native
paging/mapping is private, but no submission may access inaccessible backing.
The initial policy keeps required backing accessible until retirement; optional
native residency optimizations must preserve that contract without CPU waits.

Pressure follows this ordered, bounded policy:

1. Drain already eligible retirements within the per-frame destruction budget.
2. Stop admitting optional growth and ask owners to trim disposable caches/unused
   heap blocks. Retiring bytes stay charged; scheduled frees are not new capacity.
3. Owners select permitted lower-cost cooked/implemented representations and
   submit admitted replacements. No implicit quality degradation, shader omission,
   heap-type substitution, CPU spill or renderer switch is allowed.
4. For activation-critical streamed content, request an admitted fallback or cell
   eviction through StreamingPartitionAuthority. Pins are not a budget bypass and
   memory pressure is not permission for a backend to revoke them.
5. If required work still cannot fit, reject/defer the new attempt with a typed
   budget/capacity result. If the existing required working set exceeds a reduced
   target, stop new dependent submissions until the host reduces demand or enters
   its explicit failure/recovery path; never destroy live backing to meet a counter.

Stopping admission/submissions does not stop completion polling, owner queue
drains, or already admitted cleanup. These retain reserved capacity so the pressure
response cannot deadlock the reclamation needed to leave pressure.

Disposable means the owning feature has authorized removal, a valid missing-data
or reconstruction path exists, and no activation-critical contract is violated.
Owners order eligible cache candidates by declared priority, oldest accepted-use
sequence, then stable cache key to break ties. They only request retirement; pins
and all relevant GPU queues still gate physical release. A hot-path global scan
of arbitrary scene objects or source-asset deletion is forbidden.

Pressure enters when accounted use reaches the configured high watermark, a fresh
native sample crosses its margin, or allocation fails. It exits only after the
configured lower watermark and healthy observations for the configured recovery
sample count (ledger-only recovery when telemetry is unavailable). Configuration
requires low < high <= hard cap and finite work/retry limits. An over-cap revision
is an observable pressure state, not subtraction that underflows the ledger.

Native OOM produces a typed allocation failure with bounded diagnostics and cleans
up partial state. Retry requires changed admission/pressure evidence and a new
bounded attempt; no busy retry or normal-frame GPU-idle wait. Budget denial, queue
exhaustion, unsupported allocation, native OOM and device loss remain distinct.
Device loss uses ADR-027 reconstruction and new owner generations; a replacement
device cannot make old work disappear from CPU/retirement accounting. Teardown
uses documented backend release semantics, not an impossible lost-device fence
wait. When release cannot be established, retain the charge and report stalled
retirement rather than promising usable memory.

### 7. Threading, feature and presentation integration

Reservation consumption, registry publication and native mutation run serially
on the host-declared render-capable thread at its safe points. Other owners submit
bounded requests and receive completions; worker preparation does not allocate
native resources through an undocumented exception. Platform/native callbacks
enqueue observations, not mutate the ledger. Pressure decisions and budget changes
publish immutable revisioned snapshots to other threads.

[ADR-018](018-command-registration-permissions-threading-and-packaged-build-policy.md)
`RenderSafePoint` uses this same owner thread. CPU preparation uses the
[ADR-010](010-job-waiting-and-operation-store-ownership.md) job contract; resource
attempts use the ADR-027 `ResourceOperationId` defined in that ADR's creation-result
section (registry completion identity for one reserved generation, not a resident
handle and not `Horo::OperationId`), with user-visible operations projected
through `OperationStore` when appropriate. No second scheduler or blocking poll
is introduced. Retirement/completion capacity is reserved before admission, so a
full producer queue cannot prevent cleanup. Normal Null completions are delivered
at the same owner drain points, never inline solely because no GPU exists.

Virtual Texturing owns page choice, feedback and page-table intent; the renderer
owns physical atlas/sparse backing and safe mapping execution. Page eviction in
an allocated atlas frees a slot, not heap bytes. Sparse support requires effective
capabilities and implementation; an atlas fallback needs its own admitted cost.
Old page-table readers must retire before remapping/reusing backing. Neither page
streaming nor renderer eviction changes world-cell residency independently.

[ADR-011](011-vfx-effect-ownership-simulation-domain-and-renderer-boundary.md)
remains the VFX ownership and overflow authority for logical instances, spawn
queues and cosmetic birth dropping (`EffectCapacityExceeded`, event-queue full).
GPU backing those systems consume — `GpuParticleSimulator` buffers, indirect-draw
arguments, upload/sort copies and frames in flight — is charged on this ledger.
`ResourceBudgetExceeded` is this ADR's admission result, not permission for VFX
to grow pools during Update. Pressure step 2 may trim only VFX disposable GPU
caches the owner has authorized (scratch/sort copies with a reconstruction path).
It must not evict a live required gameplay effect or treat `EffectInstancePool`
overflow as heap-block destruction. ADR-011's "never grow during Update" rule and
this ledger's reservation-before-allocate rule are the same transaction: VFX
admits the logical instance only after the renderer claim exists.

Presentation targets, swapchain-related backing estimates, GUI/font textures and
viewport resources belong to explicit host/editor scopes in the same envelope.
[ADR-033](033-presentation-and-display-ownership.md) output replacement reserves
old/new overlap and transient resources before realization. If it cannot fit,
preserve valid old output or suspend according to that ADR; do not publish the new
extent as **active** before successful budgeted realization. A pending candidate
may still be published for layout/extraction under ADR-033; failure skips that
candidate's output and never submits its plan against mismatched old targets.
Externally owned
compositor resources are reported separately with provenance and headroom, not
destroyed or claimed as renderer-owned allocations.

## Migration And Verification

This is the canonical RND-010 policy. Rendering, world streaming and virtual
texturing summaries link here; they do not define competing GPU ledgers. Existing
Foundation allocation-domain and world-cell lifecycle contracts remain intact.

| Delivery | Required next implementation work |
|---|---|
| RND-010.2 / #359 | Typed cost plans, finite host configuration, reservation identities, pool accounting, native requirements and private allocator integration; route current viewport/GUI allocations through admitted scopes. |
| RND-010.3/.4 / #360–361 | Bounded upload/readback arenas, copy accounting, cancellation, consumer leases and completion publication. |
| RND-010.5/.6 / #362–363 | Graph alias proofs and descriptor/binding storage charged to its actual owner; no unbudgeted helper heaps. |
| RND-010.7/.8 / #364–365 | Multi-queue retirement, pressure/retry policy, partial failure, device-loss cleanup and allocation-free emergency diagnostics. |
| RND-010.9/.10 / #366–367 | Capability-gated sparse backing and admitted atlas path; platform qualification, fragmentation/peak/budget/retirement measurements. |

Do not migrate by wrapping a second counter around existing native allocations.
Replace each path's ownership and charge source, expose unsupported paths honestly,
and remove its obsolete local budget. No serialized native allocation or resource
handle is introduced. Future persisted budget settings require the existing
configuration schema and project migration process; M0 changes no file format.

Implementation acceptance must cover:

- Exact-cap admission, checked arithmetic/alignment, unknown cost, dedicated and
  pooled growth, block slack, shared transfer, UMA non-duplication and separate
  discrete pools; two worlds/previews cannot spend the same host allowance.
- Partial reservation failure, stale scope/device attempts, cancellation before
  and after submission, replacement peak, retained source copies, bounded queues
  and readback consumers. Cleanup must progress when producer admission is full.
- Multi-queue GPU use and dependent pins preventing reuse; no capacity refund on
  logical release, cell eviction or a frame-index rollover alone.
- Native budget shrink/unavailable/stale samples, OOM injection, hysteresis,
  optional versus required content, admitted quality fallback, and stalled
  retirement without unsafe reclamation. Pressure paths use bounded diagnostics.
- GL/Metal parity plus future Vulkan/D3D12 native validation; deterministic Null
  schedules replay the same logical outcomes but do not replace GPU smoke tests.
- Metrics separating charged capacity, payload, reservations, retired bytes,
  fragmentation/slack and native estimates, with owner/provenance and failure
  reason. RND-010.10 records workloads and backend measurements before tuning.

### Physical backing identity migration

A compatible logical pool can contain multiple physical blocks. Every admitted
`RenderMemoryPlacement` therefore carries both its `RenderMemoryPoolId` and a
`RenderMemoryBlockId`. The existing renderer ledger issues the block identity
from its monotonic block sequence and renderer incarnation; native backends do
not introduce a second allocator authority. Suballocations within one block share
that identity. Sibling blocks, including equal-sized and dedicated blocks, have
distinct identities even when both placements start at offset zero.

Native heap lookup, overlap checks and retirement use the pool and block pair.
Capacity, storage, compatibility, scope and lifetime checks remain mandatory.
Reclaiming one block must not release a sibling's backing or remove a nonempty
logical pool. Pool grouping, pool limits and whole-block accounting are unchanged.

This adds a required field to the in-process C++ placement layout. Rebuild the
frontend, all first-party backends, SDK consumers and direct placement factories
as one source cohort; do not mix old and new C++ objects or binaries. Missing,
zero or foreign-renderer block identities are rejected, without a legacy pool-only
fallback. These identities are runtime values, not persisted project data; this
change adds no project-file format or native handles to the public contract.

## Consequences

Owners can reject work before allocating and explain where memory remains charged.
Cell admission and GPU residency compose without competing world authorities.
Conservative estimates, reserved overlap and whole-block charging may admit less
content than optimistic byte counts. Shared-pool leases and revisioned transactions
add bookkeeping, but make cancellation, multiple worlds and pressure reviewable.
No allocator algorithm, block-size performance claim or hardware qualification is
proved by this document; downstream implementation and measurements remain required.

## Rejected Alternatives

- **Independent budgets for every feature**: lets their total exceed host/device
  capacity and duplicates shared allocations.
- **World Streaming allocates native heaps**: reverses dependencies and conflates
  cell authority with renderer implementation; non-streamed/editor work still needs
  admission.
- **Resource payload size equals GPU cost**: ignores padding, pooled slack, copies,
  replacement overlap and deferred destruction.
- **Treat native available-memory telemetry as a guaranteed cap**: observations
  change and may be estimates or unavailable; native failure remains possible.
- **Free on logical release or evict the least-used live handle globally**: violates
  dependency pins, GPU completion and activation-critical cell contracts.
- **Overcommit then wait for GPU idle on OOM**: turns pressure into frame stalls and
  cannot guarantee reclamation; use bounded admission and explicit host failure.
- **Expose one native heap model as the public abstraction**: privileges an API and
  leaks memory-type/placement policy into feature code.
