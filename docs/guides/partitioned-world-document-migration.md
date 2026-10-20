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

The workspace now also implements the editor manual residency contract described below.
The cell browser UI (#1630), bake, persistence,
undo/redo history, source-control merge, crash recovery or runtime preview activation.
Those services must consume exact immutable source revisions and must not clear dirty
metadata without their own durable publication contract.

## Manual page residency and selection (HORO-1635)

The existing `PartitionedWorldWorkspace` is the sole authoring authority. It now
owns actual immutable `SceneDocument` instances for loaded source pages, alongside
its authored metadata. An editor cell operation names the **source page AssetId**
and exact `WorldAuthoringPageDescriptor`, never a cooked `StreamingCellId`.
The editor controller composes this owner from the host-issued workspace identity.
Browser/command adapters obtain `WorldDocuments()` and invoke its narrow typed
operations; this ticket adds no separate document map or runtime scheduler.

`LoadPage` accepts canonical Scene source bytes after the host authorizes and reads
them outside draw paths. It decodes and validates the actual Scene document,
requires exact object-ID membership with the authored page, rejects unknown
components that would otherwise be dropped by decoding, and charges both the open
page slot and bounded source-byte admission. Valid nonempty pages retain their
names, transforms, typed components, prefab references and editor flags. Loading
is not runtime activation or an assertion of disk durability. No new file format,
source path, filesystem authority, or native backend capability is introduced.

`WorldPageContentLease` exposes only const spans over an actual document. Copies
retain that document across unload, replacement, close, shutdown and owner
destruction. The loaded head, source-byte charge, pin and source metadata publish
in one immutable world root. Default hard limits are 16 MiB source bytes per page
and 64 MiB of simultaneously resident source input; these are admission charges,
not measured allocator bytes. Decoding/object allocations and prepared successors
are bounded by these ceilings and the existing object limits. Historical readers
and temporary preparation overlap must be budgeted by their consumers.

`CommitLoadedPage` takes the complete successor metadata and edited canonical
Scene content. It validates both before publication, creates an actual dirty
SceneDocument through `LoadRecovered`, advances the page exactly once and retains
pins and surviving selection. Selected object removal rejects; the caller must
explicitly change selection first. Malformed, stale, unsupported, cancelled,
over-capacity or allocation-failed preparation retains the exact previous root.
The older metadata-only `Commit` rejects any change/removal to a loaded page;
callers editing loaded-page assignments/bounds must submit the coherent source
through `CommitLoadedPage`. This deliberate API restriction prevents divergence
between the published head and loaded content. Existing unloaded metadata clients
continue using `Commit`; the public-header consumer covers the new lease and
load/unload signatures. No compatibility shadow document is retained.

`UnloadPage` releases only a clean, unselected, unpinned page, leaving authored
metadata intact. It never evicts another page to make room. A dirty page returns
`world_document.dirty`; a pin or selected dependency returns
`world_document.conflict`. Resolve the explicit pin/selection first. Dirty payloads
remain owned until a durable-save integration exists or the user explicitly
discards the document via Close/Replace/Shutdown. There is no false MarkSaved or
unload-as-save behavior. `SetWorkingSet` remains metadata admission and cannot
remove loaded pages; it is not a bypass for unloading. `SetPagePinned` is owner-local
and transient and never changes runtime demand or grants budget bypass.

Selection uses `WorldAuthoringObjectAddress` (page plus object). Equal object IDs
in different pages remain distinct. The actual `EditorSelectionModel` adapter
forwards `SetWorldObjects` to the workspace root and emits a typed post-commit
notification only when selection changes. It stores no second page selection.
The controller's `SelectWorldObjects` uses this same adapter; ordinary flat scene
selection remains associated with its existing Scene document. Dependent page
selection survives every failed unload/edit/lifecycle operation.

The workspace leave guard captures the world fence and includes world dirty state.
Until durable world save exists it offers Discard/Stay for dirty world pages,
rejects an attempted Save, and rejects stale discard consent. Stay does not release
payloads or selection. Replacement clears residency/selection only after complete
new admission and the explicit dirty disposition. BeginCancellation gates all new
operations while retaining queries and explicit teardown. Successful shutdown
releases owner roots and pins; existing read leases remain usable.

Focused residency tests are part of `HoroPartitionedWorldWorkspaceTests`; the
selection adapter test belongs to `HoroEditorSelectionModelTests`, and the actual
controller ownership test belongs to `HoroEditorWorkspaceControllerTests` (GUI
composition build). Build those and `HoroEditorModelPublicHeaderConsumer` for
qualification. Manual browser widgets, filesystem publication, undo/recovery and
runtime preview remain their separately owned capabilities; these operations
already load and retain actual editor scene content without requiring those UI
or persistence features.
