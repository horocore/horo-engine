# Partitioned world document and workspace model (WST-009.2)

## Ownership and consumers

`Horo/Editor/PartitionedWorldWorkspace.h` belongs to `HoroEditorModel`
(`HoroEngine::EditorModel`). It provides a headless, stateful Editor workspace model,
not another runtime admission validator. The model owns one optional
`PartitionedWorldDocument` and a separate transient open-page working set. Application
composition can own this model and expose its typed operations to later workspace UI
or automation adapters; this change does not add a GUI route or replace
`EditorWorkspaceController`'s existing scene document.

The new public dependency is EditorModel -> WorldStreaming for inert identities,
source-page heads, object descriptors, layer values, bounds, grid policy and checked
revision arithmetic. There is no reverse dependency and no runtime discovery,
mount, registration, callback or asset resolution. Existing consumers rebuild; no
previous API, persisted project version or file format changes. Header ownership,
standalone-header compilation, typed consumer assertions and the dependency allowlist
are updated together.

## Source metadata, not cooked state

- Settings own the stable `WorldPartitionId`, ordered canonical millimeter content
  bounds and validated authored grid policy. Bounds must fit that grid's level-zero
  envelope.
- Layers retain stable `StreamingLayerId`, owned UTF-8 names and typed layer metadata.
  Renaming or reordering does not regenerate IDs. Unknown ownership/flags,
  contradictory client/server flags and invalid priorities fail closed.
- Each page owns an exact `WorldAuthoringPageDescriptor` and its metadata-only object
  records. Object identity is `WorldAuthoringObjectAddress` (page asset plus nonzero
  local ID), not `sourceAsset`, a display name, vector index or runtime entity handle.
  The same local object number may be used in different pages.
- Every object record belongs to its page's exact immutable revision and an existing
  stable layer. Descriptor-version and placement failures retain their original
  WorldStreaming error identity.
- Empty worlds, empty pages and unloaded geometry are representable. No cooked cells,
  chunk package assets or `WorldPartitionDescriptor` are required. The latter remains
  an immutable **runtime** projection of `world.index`, not an editable document.
- Open pages are a distinct transient vector of admitted page asset IDs. Closing an
  open page never deletes its source metadata or changes authored dirty state. Runtime
  residency/eviction has no mutation capability into this owner.

Version one uses the spatial-page / revision-checked decision in
[World Authoring Source And Collaboration Contract](../architecture/runtime/world-streaming-architecture.md#world-authoring-source-and-collaboration-contract).
Page assets are independent of cooked `StreamingCellId`; future bake may map pages and
cells many-to-many without renaming either source identity.

## Transactions and fences

The host supplies a never-reused workspace `AssetId` to the owner constructor. It is
an incarnation identity, not the world or page identity. Every immutable snapshot has
an exact `{owner, WorldDocumentGeneration, WorldDocumentRevision}` fence. These types
are distinct from page revisions, mounted partition epochs and runtime residency
generations. Callers must capture the newest snapshot before every mutation.

`Open` validates and copies a complete source aggregate. A second Open is a conflict,
never an implicit discard. `Commit` is the one complete typed metadata transaction:
settings, layers, page/object additions, replacements and removals are checked together.
The partition identity cannot be edited; changing worlds requires `Replace`.
Unchanged pages retain identical heads and object records. Every changed existing
page must name its exact non-wrapping revision successor. Removed pages are pruned
from the working set in the same publication, so no dangling open-page IDs escape.

`SetWorkingSet` checks distinct page membership and the open-page ceiling without
altering authored metadata or dirty state. Both source commits and working-set changes
advance the publication revision, fencing stale commands across either kind of change.
Exact no-ops retain the existing lease and revision. Source commits set dirty; no
operation pretends to save or establish filesystem durability.

All validation and preparation precede the one immutable-root publication. Invalid,
unsupported, conflicting, stale, over-capacity and cancelled requests preserve the
previous root, dirty state, working set and generation. C++ allocation exceptions also
have this strong guarantee; they propagate rather than fabricating a success or trying
to allocate an out-of-memory diagnostic. Stop tokens are checked at admission and
immediately before publication. This is synchronous bounded tooling work, not a job
queue, and cancellation does not roll back an already completed publication.

## Lifecycle and limits

All service access belongs to the constructing thread. Wrong-thread calls reject
before touching mutable state; synchronized readers consume only immutable leases.
The owner cannot be copied or moved. Retained leases remain usable after edits,
replacement, close, shutdown and destruction; historical retention budgets belong to
the consuming service.

`Replace` requires the current fence and explicit `Discard` for dirty metadata. It
validates the new document first, then issues a fresh generation, clean admission state
and empty working set. Failed replacement preserves the previous document. `Close`
also requires the exact fence and dirty decision; reopening issues a fresh generation.
Generations and publication revisions never wrap.

`BeginCancellation` permanently gates new opens, commits, replacements and working-set
changes, but retains queries and permits explicit Close/Shutdown. `Shutdown` is
idempotent; a dirty-preserving failure does not change lifecycle or release state.
Explicit discard releases ownership, not reader snapshots. Destruction releases only
owned memory and invokes no external callbacks or persistence.

Default limits are also hard maxima: 256 layers, 4,096 page metadata records, 65,536
object descriptors, 65,536 aggregate layer-name bytes and 64 open pages. Hosts may
choose smaller nonzero ceilings. These bound **metadata**, not geometry or payload
residency. Input counts and aggregate byte ceilings are checked before owned copying;
no frame-time or streaming-performance claim is made.

## Validation and scope

Build `HoroPartitionedWorldWorkspaceTests`, `HoroEditorModelPublicHeaderConsumer`,
`HoroWorldStreamingTests` and `HoroWorldStreamingPublicHeaderConsumer`, then run:

```sh
ctest --test-dir build/skeleton -R 'Horo(PartitionedWorldWorkspace|WorldStreaming)Tests' --output-on-failure --no-tests=error
```

The focused tests exercise owned-copy admission, exact/overflow capacities, malformed
and unsupported metadata, stable IDs, atomic settings/layer/page/object transactions,
page successor/exhaustion, stale and foreign owner fences, independent working sets,
no-op revisions, cancellation, dirty preservation, replacement, reopen, shutdown,
wrong-thread rejection and retained readers. The consumer asserts const leases and
type separation using only declared EditorModel dependencies.

This is the complete metadata/workspace foundation, not manual cell payload I/O,
load/unload/pinning (#1635), the cell browser UI (#1630), bake, persistence,
undo/redo history, source-control merge, crash recovery or runtime preview activation.
Those services must consume exact immutable source revisions and must not clear dirty
metadata without their own durable publication contract.

## Object assignment and bounds inspection (WST-009.5)

`PartitionedWorldWorkspace` is also the single authority for selected source object
addresses. `SetSelection` takes the exact immutable workspace fence, validates
all distinct page/local-object identities before publishing, and retains input
order. It does not require open geometry, alter dirty state, or control page
load/unload/pin. Selection and source changes share `WorldDocumentRevision`; there
is no second mutable Inspector selection or independent currentness authority.
Removing a source object prunes its selection in the same commit. Replace clears
selection and changes the generation. Retained immutable snapshots remain usable.

`InspectSelectedObjects` is an explicit owner-thread tooling query against that
exact fence. Consumers retain the returned const lease and render it without
rerunning enumeration on every frame. Requery after a source/selection publication.
Cancellation is checked before allocation, between selected objects and before
return. Cancelling/closed owners reject new inspection. No query publishes state,
opens geometry, performs I/O, mounts a world, or claims runtime residency.

Each row retains its source descriptor (including exact inclusive global
millimeter bounds and source revision), layer, LOD and authored spanning directive.
It explains automatic cell coverage, whether the object spans multiple cells,
whether the source direct-cell threshold requires a decision, and effective
placement/cells. Always-present objects report nonspatial placement with empty
coverage. Invalid geometric bounds or a missing/conflicting geometric override
retain the original WorldStreaming diagnostic and claim no effective placement.
Unrelated selected rows remain available as a complete, explicitly diagnosed
query result; no source state is repaired implicitly.

`WorldSpatialAssignment::CreateForAuthoring` uses the existing cook assignment
kernel for bounds, quantization, cardinality and canonical ordering. Its source
domain has no cooked-cell/package authority. The original cooked `Create` still
requires every covered cell to exist in the published descriptor. Both feed the
same `WorldSpanningObjectPlan` for single-owner, split-per-cell and nonspatial
policies. There is no Editor implementation of assignment or spanning policy.

Migration: existing callers rebuild against the same EditorModel and
WorldStreaming targets and staged public headers. Aggregate fields are additive:
settings default the direct-cell threshold to one; objects default to LOD zero
and no override. Existing cook callers keep `Create`; authoring queries supply an
explicit source domain. This evolves the in-memory model only, with no persisted
file-format change. Override edits go through `Commit`, including the page's exact
revision successor and each object's matching revision. Unknown policy/LOD,
malformed directive identity or an incompatible always-present override fails
before publication. Geometric override mistakes remain inspectable diagnostics.

Selection is bounded at 256 objects (hosts may lower it). Query ceilings default
and cap at 4,096 automatic cells per object and 65,536 aggregate retained automatic
plus effective cells. Both cell arrays are charged, including duplicate entries
across the two explanations. Invalid/raised budgets, exhausted enumeration,
stale/foreign fences and cancellation return typed errors with no partial result
or mutation; consumers never receive silently truncated lists. Historic result
retention remains the consumer's responsibility.

Regression coverage lives in `PartitionedWorldObjectInspectionTests.cpp` within
`HoroPartitionedWorldWorkspaceTests`, with shared-kernel parity coverage in
`WorldSpatialAssignmentTests.cpp` within `HoroWorldStreamingTests`. Build those and
`HoroEditorModelPublicHeaderConsumer` / `HoroWorldStreamingPublicHeaderConsumer`;
run the existing world-document and WorldStreaming CTest groups. No native/GPU
interaction or GUI rendering result is implied by these model-level tests.

The selection/query entry points synchronously borrow `WorldDocumentFence` and the
optional primary address by const reference. These larger input values are never
retained by reference; publications copy their owned state before returning.
Ordinary calls, including temporary/null primary values, are unchanged. Consumers
storing member-function pointers must use the const-reference parameter types;
the standalone public consumer asserts those exact signatures. Rebuild consumers
with this in-memory API evolution; no source-file or serialized identity changes.
