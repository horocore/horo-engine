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


## Streaming source authoring (HORO-1633)

The canonical `PartitionedWorldDocument` now includes bounded authored `sources`.
`PartitionedWorldSource` reuses World Streaming's exact sphere, box, frustum and
path-volume types, source identity/revision, priority, intent and desired-residency
values. Debug color is editor source metadata. Runtime owner tokens, partition
epochs and registration are intentionally absent: authoring never mounts a world
or changes runtime residency.

`EditSource` is the single atomic source command path. It issues strictly increasing
revisions, validates the entire document and records one bounded source-history
step. `ReplaySources` restores authored values with fresh revisions; it never rewinds
a publication fence. Metadata `Commit` invalidates source history, while transient
working-set changes preserve it. Capacity, cancellation, invalid/unsupported input,
stale fences and owner lifecycle failures preserve both root and history. A full
history explicitly rejects new edits until history is rewound or a deliberate
metadata publication invalidates it. No history eviction or implicit save occurs.
Open/replacement and successful close reset history; immutable retained leases
remain safe. The existing schema version remains an in-memory model version: this
change defines no competing persisted world format.

The GUI controller borrows the host's single `PartitionedWorldWorkspace` through
`EditorWorkspaceDependencies::partitionedWorld`. The host must outlive the controller
or call `DetachWorldSourceOwner` before destroying that owner. Panels receive owned
immutable leases, never raw owner memory. The active world host supplies a
`WorldViewportBinding` (exact root, global origin, monotonic view revision) through
`SetWorldViewportBinding`. It owns camera/origin mapping and dirty/save/leave policy;
the source tool does not invent another world or selection authority. The production
world-tab composition is delivered by HORO-1630; until that composition is integrated,
the source toolbar reports unavailable and does not synthesize a document.

The viewport popup creates all four shapes and edits intent, target state, priority
and debug color using shared controls and localized copy. Canonical control-point
gizmos preview only in panel-owned transient storage; release submits one ordinary
source command. Escape, focus loss, input revocation, document replacement, origin
change, view replacement and detach cancel the preview. Undo/redo use the same
canonical owner. Play mode rejects source edits at the controller boundary.

`WorldSourceViewportPoint` subtracts integer global coordinates before any float
conversion, including near both signed-64-bit limits. Local projection admits an
8000-meter precision window. `WorldSourceViewportGlobal` rounds only a bounded
local offset to millimeters and adds it with checked integer arithmetic; absolute
global coordinates never pass through float or double. The host rebases distant
views by publishing a new origin/view revision. Off-window points are unsupported,
not clamped or silently relocated.

Affected consumers are `HoroEditorModel`, its public-header consumer, the GUI
workspace/controller/viewport and focused model/controller/localization tests. No
new public header ownership or reverse dependency is introduced. Existing aggregate
initializers remain source compatible because the new source collection and limits
are additive with defaults. Source-only local checks do not qualify native GUI or
GPU interaction; actual current-head hosted checks and a compatible editor session
remain separate evidence.
