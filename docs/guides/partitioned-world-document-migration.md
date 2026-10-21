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

## Layer command and outliner projection migration (WST-009.4)

The existing workspace now owns `WorldLayerCommand` and its exact semantic layer
history. There is no second document, mutable panel-owned layer table or independent
selection authority. `CreateWorldLayer` takes a host-issued `StreamingLayerId`,
validated descriptor and insertion ordinal. `ReorderWorldLayer` takes stable identity
and a final ordinal in the complete authored order, including filtered-out rows.
`FilterWorldLayers` selects a bounded, exact UTF-8 substring and optional typed
ownership. `SetCurrentWorldLayer` selects an admitted identity or explicitly clears
context. Filtering never changes context; it may hide the current layer.
`CreateLayer` is the host adapter for a name-only creation request: the canonical
owner chooses an unused typed ID and appends through the same command path.
`VisibleLayers` projects stable IDs in authored order. Names are presentation values,
never identities. The caller resolves IDs against its captured immutable snapshot.

`ExecuteLayerCommand` and `ReplayLayerCommand` require the same owner/generation/
publication fence as other workspace mutations. They prepare the complete candidate
and history before publishing. Bad descriptors, missing IDs, stale fences, unsupported
filters, capacity overflow and cancellation preserve the exact old lease, context,
working set and history. A no-op preserves revision and redo. Undo/redo restore exact
layer-section values rather than replaying UI input, while publication revisions
continue increasing. Source edits mark dirty and undo restores the pre-command dirty
state; transient filter/context edits never mark authored metadata dirty.

History retains at most 128 transactions and 4 MiB of charged semantic metadata,
including before/after layer descriptors and name/filter bytes. Oldest entries are
evicted before a newly reserved transaction is committed. A new command drops redo;
a failed command does neither. Pages, objects, working-set changes and runtime data
are never retained in layer history. Complete external metadata commits and `CommitLoadedPage` clear the
layer history and clear a removed current identity. Unchanged complete commits leave
history intact. Working-set changes preserve history, and replay preserves the
current working set. Replacement and close clear history/context; cancellation gates
commands/replay but preserves immutable queries until explicit teardown.

This additive C++ contract remains in the existing `HoroEditorModel` header and target.
Existing consumers rebuild; no persisted schema or backend dependency changes.
The public consumer fixture covers command, replay and projection signatures.
Run `HoroPartitionedWorldWorkspaceTests` and build
`HoroEditorModelPublicHeaderConsumer` to validate the new contract.

The GUI host routes outliner actions through `WorldLayerAuthoring` and
`OpenCreateWorldLayer`. `EditorWorkspaceController::ApplyWorldLayerIntent` applies
canonical owner commands; the screen composes the exclusive creation modal. The
World Composition tab integrates the draw/route helper from its persistent document
surface. Registration of that surface remains the WST-009.3 child integration.

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
in different pages remain distinct. The single 256-object selection authority admits unloaded authored metadata, and additionally checks actual content membership for loaded pages. The actual `EditorSelectionModel` adapter
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

## Object assignment and bounds inspection (WST-009.5)

`PartitionedWorldWorkspace` is also the single authority for selected source object
addresses. `SetSelection` takes the exact immutable workspace fence, validates
all distinct page/local-object identities before publishing, and retains input
order. It does not require open geometry, alter dirty state, or control page
runtime demand. Selected pages remain unload dependencies under the manual residency policy. Selection and source changes share `WorldDocumentRevision`; there
is no second mutable Inspector selection or independent currentness authority.
Explicitly removing an unloaded source object prunes its selection in the same commit; loaded-page removal is rejected until payload ownership is resolved. Replace clears
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
file-format change. Override edits on unloaded metadata go through `Commit`; loaded pages use `CommitLoadedPage` with their coherent source. Both require the page's exact
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

### Layer GUI binding

`DrawWorldLayerOutliner` consumes one immutable document snapshot and emits at
most one `WorldLayerOutlinerIntent`. `Execute` carries a canonical command;
`Undo`/`Redo` carry direction; `Create` requests the host-owned
`CreateWorldLayerModal`. Every action carries the exact snapshot fence. The
outliner uses stable IDs and full-document ordinals, shared themed controls,
wrapped names and localized copy. It does not mutate source or maintain a second
history or selection.

The modal stores only a bounded name draft and its opening fence. The host
callback invokes `CreateLayer`; failure retains the draft and displays a localized
failure, while success closes the modal. Stale or closed-document failures require
explicit cancellation/reopening rather than silently retargeting a newer source.
The ordinary modal host owns exclusive input, cancellation and shutdown. Render
regressions cover narrow widths, long names, empty/filtered current context, both
supported locales, dark/light theme surfaces and cancellation without submission.

The shared host adapter is `RouteWorldLayerOutlinerIntent`, which translates a draw
result into the existing screen/controller command output. Canonical typed errors
remain available as `EditorWorkspaceViewModel::worldLayerFailure`; the world tab
resolves localized failure copy. The layer controller regression target exercises
real loaded Scene content, page pinning, the single selection/primary root, stale
command rejection and loaded-page dirty/history isolation.

The selection/query entry points synchronously borrow `WorldDocumentFence` and the
optional primary address by const reference. These larger input values are never
retained by reference; publications copy their owned state before returning.
Ordinary calls, including temporary/null primary values, are unchanged. Consumers
storing member-function pointers must use the const-reference parameter types;
the standalone public consumer asserts those exact signatures. Rebuild consumers
with this in-memory API evolution; no source-file or serialized identity changes.

The new LoadPage, CommitLoadedPage, UnloadPage and SetPagePinned methods borrow
their page descriptors and world fences by const reference for the synchronous call.
Selection adapters likewise borrow the optional primary and fence. Consumers rebuild against
the updated declarations; ordinary value and temporary arguments remain supported,
and no borrowed reference is stored. The selection adapter captures an owned fence
before publication because the argument may alias the immutable root being replaced.
The public-header consumer checks the exact manual load/unload signatures, and the
selection regression exercises a fence backed solely by that owner root. Decoded Scene
source is transferred out of its successful Result before moving its vectors into
the actual document; moving from the immutable lvalue Result view would copy instead.

Layer command, name-only creation and replay entry points likewise borrow the exact
fence for the synchronous operation. They never retain it or access it after root
publication. The layer consumer asserts the exact const-reference member signatures,
in addition to the canonical selection/query/manual-page signatures. Existing value
and temporary arguments continue working; consumers storing member pointers rebuild
against these updated in-memory declarations. Modal drafts and queued presentation
intents still own their fence values across frames.
