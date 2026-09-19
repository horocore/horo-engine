# Editor Document Model Architecture

## Purpose

This document defines editor document ownership, commands, history
transactions, dirty state, save, autosave, crash recovery, external file
changes, and conversion to runtime scenes.

## Core Decisions

- `SceneDocument` is the authoritative editable scene state.
- All undoable mutations execute through typed editor commands.
- History stores semantic transactions, not arbitrary memory snapshots.
- Document revision, saved revision, and runtime preview revision are distinct.
- Save is validated and atomic.
- Autosave writes recovery state; it does not redefine the user's saved file.
- Recovery is explicit and never silently overwrites the source document.
- GUI panels, tabs, modals, MCP handlers, and background workers do not mutate
  document storage directly.

## Ownership

```text
EditorWorkspaceController
  +-- SceneDocument
  +-- EditorHistory
  +-- EditorSelectionModel
  +-- EditorViewportModel
  +-- EditorDataBus
  +-- DocumentSaveService
  +-- DocumentRecoveryService
```

The workspace controller owns the active document session. Tabs and panels
receive command, query, and subscription capabilities through their context.

One document session has a stable `DocumentSessionId`, a monotonic
`DocumentRevision`, and an immutable `DocumentStateId` for each committed
content state. Undo and redo create a new revision notification while restoring
the state ID of the semantic history entry they apply.

## Surface And Document Identity

Workspace routing uses separate identities for application surfaces and persistent
source documents. `SurfaceTypeId` names a registered surface type such as
`horo.viewport` or `horo.game`; it is not a document instance and must not be
used as a mutable tab handle. `DocumentKind` describes the document semantics,
while `SourceDocumentId` stores a validated project-relative path using `/`
separators. `DocumentOpenKey` combines those two persistent values.

`DocumentInstanceId` is session-local and monotonic. `DocumentIdentity` combines
it with a `DocumentOpenKey` for the lifetime of one open instance. Opening an
existing key returns `FocusExisting` and the existing instance; it never creates
a second tab for the same source identity. Different source identities with the
same `DocumentKind` are independent and may be open concurrently. Workspace
serialization persists only `DocumentOpenKey`, never a session instance value;
restore allocates a fresh instance and therefore cannot resurrect a stale handle.

Surface lifecycle is explicit through `SurfaceCapability` flags. The built-in
Viewport surface is pinned and restorable but not closable. The Game surface is
conditional on the play-session state, closable, and restorable. These descriptors
capture existing behavior without making the identity layer own rendering,
play-session, or panel lifetime; those remain in the workspace/application host.

## Document Model

The document stores authoring state:

- stable scene object IDs and hierarchy
- typed component authoring values
- logical asset references
- primitive descriptors for built-in procedural meshes and shapes
- scene settings
- editor-authorable metadata

Primitive descriptors reference the `PrimitiveCatalog` and store parameters such
as radius, height, and tessellation rather than imported asset IDs. See
[Built-In Scene Primitives](../runtime/built-in-scene-primitives.md).

It does not store:

- ImGui widget state
- selected objects
- panel layout
- open modals
- runtime ECS handles
- physics body pointers
- renderer backend handles

Workspace presentation state belongs to [Project Model](./project-model.md).

After committed document changes that remove or invalidate selected objects,
the workspace controller reconciles `EditorSelectionModel` against the new
`SceneDocument` state and publishes a selection change if needed.

## Queries And Mutation

Read APIs expose immutable views or snapshots:

```cpp
class SceneDocumentQueries {
public:
    DocumentRevision Revision() const noexcept;
    SceneObjectView Find(SceneObjectId id) const;
    SceneDocumentSnapshot Snapshot() const;
};
```

Mutation is internal to the command executor and document services. Returning a
mutable object reference to arbitrary UI code is forbidden.

## Command Contract

```cpp
class EditorCommand {
public:
    virtual Result<void> Validate(const EditorCommandContext&) const = 0;
    virtual Result<CommandDelta> Apply(EditorCommandContext&) = 0;
    virtual Result<void> Revert(EditorCommandContext&, const CommandDelta&) = 0;
    virtual CommandMetadata Metadata() const = 0;
};
```

A command:

- validates before committing mutation
- owns enough typed data to undo its committed change
- has deterministic apply and revert behavior
- identifies affected objects and change category
- does not draw UI or publish success before commit
- does not launch untracked asynchronous work

Command application is all-or-nothing. If `Apply()` fails mid-execution, the executor restores the pre-command state through the command delta, transaction journal, or staged mutation mechanism; it must not rely on arbitrary whole-memory snapshots as the normal history representation. Failed commands do not increment document revision and do not publish document-change notifications.

Commands should prefer staged mutation or a transaction journal when failure
can occur before a complete undo delta is available.

Commands that require external engine operations coordinate through an
application use case and commit document changes only after the required result
is available.

## Transactions

History transactions group commands into one user-visible undo step:

```cpp
class EditorTransaction {
public:
    Result<void> Execute(std::unique_ptr<EditorCommand> command);
    Result<HistoryEntry> Commit();
    void Rollback();
};
```

Examples:

- one transform drag with many preview updates
- multi-object property edit
- asset drop that creates several scene objects
- hierarchy reparenting with transform preservation
- creating a prefab from the selected object and replacing it with a prefab instance reference

Only a committed transaction increments the authoritative document revision and
publishes one coherent change notification.

### Prefab Override Transactions

[ADR-093](../../adr/093-prefab-override-property-identity-and-delta-operations.md)
requires prefab override assign/add/remove/move, revert, source rebase, conflict/
orphan resolution and apply-to-prefab to enter through typed Editor commands or a
multi-document application use case. Inspector widgets, file watchers and
serializers never mutate override maps or `.prefab` files directly.

Commands pin the document session/revision, source prefab revision and component
schema registry revision, build a complete canonical override/rebase candidate and
then commit override records, conflicts/orphans, dirty state and one history entry
atomically. Failure/cancellation preserves the old set. Undo/redo owns exact semantic
deltas including losslessly preserved opaque records.

Apply-to-prefab prepares source edits, affected instance rebase candidates,
permissions/source-control and durable publication before removing instance records.
An external publication with uncertain/partial outcome receives an explicit
reconciliation record; the editor cannot claim in-memory rollback undid an already
published source file.

[ADR-094](../../adr/094-prefab-nested-composition-and-variant-inheritance.md)
extends the same transaction boundary to nested-placement and single-parent variant
resolution. The application pins one Asset Registry snapshot, validates the
combined `NestedPlacement`/`VariantParent` graph, resolves affected assets in
deterministic dependency order and prepares every ADR-093 rebase before commit.
The visible document and viewport receive one complete candidate; a cycle, missing
revision, conflict, cancellation or stale async completion leaves the prior graph,
projection, dirty state and history unchanged.

The editor exposes concrete, variant, nested-placement and scene-instance layers
separately through bounded provenance. A variant has exactly one immediate parent.
Inspector and graph controls cannot add a second parent, run construction scripts,
edit the flattened projection or mutate dependants from a file-watcher callback.
They submit typed commands against stable `AssetId`, source revision and persisted
placement `LocalObjectId` scope.

[ADR-096](../../adr/096-prefab-external-reference-and-binding-slot-contract.md)
makes create-from-selection a reference-boundary audit inside the same transaction.
Known internal references are remapped to new stable prefab-local IDs, assets retain
typed `AssetId` identity, and every target outside the selection is reported with
its owning object/component/property and target classification. The user explicitly
includes the target, exposes a required/optional typed slot, or cancels. Unknown
opaque reference semantics block publication.

The command prepares the prefab source, replacement instance, local-ID remap, slot
declarations, instance binding set, history and selection reconciliation together.
It never silently nulls, copies, reparents or captures an external scene object.
Failure, cancellation or stale document/source revision leaves every document,
file, dirty state, history and selection unchanged. External Bindings, Overrides
and Initialization remain separate Inspector models and command schemas.

Intermediate transaction state is not observable by tabs or panels until the
transaction commits, unless the transaction is explicitly marked as a preview
transaction.

## Preview Edits

Continuous tools may maintain preview state during an interaction. Preview state
is either:

- local presentation state rendered without document mutation
- a document transaction with reversible intermediate values and one final
  history entry

Preview mutations may update an `interaction_preview_revision`, but they do not update
the authoritative document revision or dirty state until the transaction commits.
Subscribers that need live preview updates observe preview notifications and
query the current preview overlay/state.

Cancelling the gesture restores the exact pre-transaction state and publishes
the appropriate final revision notification.

## History

```cpp
struct HistoryEntry {
    HistoryEntryId id;
    CommandLabel label;
    DocumentStateId beforeState;
    DocumentStateId afterState;
    std::vector<CommittedCommand> commands;
};
```

History has bounded item and memory budgets. Evicting old entries never changes
the current document.

Committed history entries own immutable command deltas required for undo/redo.
Command objects must not depend on external mutable UI state or live pointers after commit.

`Undo` and `Redo`:

- execute only on the editor owner thread
- use the same validation and notification path as normal commits
- fail safely if an external dependency required by the operation is no longer
  available
- clear redo history after a new committed edit

Save is not an undoable document command.

## Revision And Dirty State

The document tracks:

```text
current_revision             monotonic committed-state notification sequence
current_state_id             identity of the currently visible authored state
saved_revision               revision captured by the last successful save
saved_state_id               identity written by the last successful save
autosaved_revision
runtime_preview_revision
interaction_preview_revision
```

The document is dirty when `current_state_id != saved_state_id`.

Undoing back to the saved state restores its `DocumentStateId` and clears dirty
status even though the monotonic `DocumentRevision` advances and history still
contains entries. Saving updates `saved_revision` and `saved_state_id` only
after the atomic file replacement succeeds.

## Save

Save flow:

1. capture an immutable snapshot and target revision
2. validate the snapshot
3. serialize deterministically
4. write a sibling temporary file
5. flush according to durability policy
6. atomically replace the destination
7. verify required metadata
8. mark the target revision saved if it is still current

Atomic replacement uses the platform-appropriate primitive and durability policy.
On POSIX-like systems, the destination directory is flushed when required by the
selected durability policy. On Windows, replacement preserves expected file
metadata according to the document format policy.

If edits occur while serialization is prepared, the successful save applies to
its captured revision. The document remains dirty relative to newer edits.

Failed save preserves the original file and returns structured diagnostics.

## Save As

`Save As` validates project boundaries, extension, identity rules, reference
updates, and destination conflicts. It commits the document's new location only
after the destination is durable.

Changing location is coordinated by the workspace controller and project model;
tabs do not update path state independently.

After successful `Save As`, the active document location is updated and `saved_revision` is set to the saved target revision if that revision is still current.
`Save Copy As` writes a separate copy without changing the active document
identity or saved revision.

## Autosave

Autosave is enabled by typed user/project policy and triggers from:

- elapsed dirty duration
- edit-count threshold
- focus or lifecycle checkpoints where safe

Autosave captures an immutable revision and writes a recovery document under the
user state or project recovery area. It does not overwrite the canonical scene
file and does not change `saved_revision`.

Autosave is coalesced, bounded, and does not run concurrently for the same
document. Repeated failures use backoff and surface one actionable status rather
than a notification flood.

Recovery storage has a retention policy bounded by document count, session
count, total storage, and record age. Clean shutdown removes obsolete recovery
records only after verifying that the canonical file contains the intended
document state or the user has explicitly discarded the recovered changes.

## Recovery

Recovery records contain:

- engine and document format version
- project, document, and session identity
- canonical document path
- saved revision and recovered revision
- timestamp and clean-shutdown marker relation
- checksum and safe recovery metadata

On startup after an unclean session, the editor compares recovery state with the
canonical file and offers:

- inspect recovered version
- restore into the active document
- save recovered content to a new file
- discard recovery state

Recovery loading treats recovered data as untrusted input and validates it using
the same document validation path as normal scene loading.

Restoration creates a new dirty editor session. It never silently overwrites the
canonical file.

## External File Changes

The document service tracks the file identity or content hash last read/written.
When the canonical file changes externally:

- clean document: offer or perform policy-approved reload
- dirty document: offer compare, keep local, reload external, or save local as
- active save: detect conflict before replacement

By default, external reload starts a new history branch, clears redo history, and prevents undo across the reload boundary. It is never injected as a hidden command. Rebase is allowed only through an explicit policy that rebuilds valid semantic history entries.

## Navigation Definition And Scene Intent

[ADR-110](../../adr/110-navigation-editor-surface-and-command-ownership.md)
applies this document contract to a persistent `NavigationDefinitionDocument`
rooted at the definition `AssetId`. It owns only profiles, areas, build/tile and
scope policy with its own session/revision/history/dirty/save/recovery state.
`SceneDocument` separately owns definition references, sources, modifiers, links
and dynamic-obstacle intent through typed Scene commands.

Starting a navigation bake is an application use case, not a document command.
ADR-106 publication neither advances history nor clears dirty state, and undo does
not delete cooked artifacts. A deliberate cross-document edit requires a staged
multi-document transaction; UI surfaces cannot write either document directly.

## Gameplay AI Asset Documents

[ADR-111](../../adr/111-gameplay-ai-document-panel-and-runtime-debug-ownership.md)
defines separate document routes for blackboard schemas, behavior-tree/state-
machine/utility graphs and EQS templates. Each keeps its own asset-rooted session,
revision, semantic history, dirty/save/recovery/conflict state and subsystem
validator. Shared graph widgets emit commands but own no semantic storage.

Cross-schema/reference edits use the staged multi-document transaction contract.
Compilation is revision-correlated derived work and does not clear dirty state.
Live PIE blackboards, nodes, tasks and query results never enter document history;
importing reviewed runtime data requires a separate typed editor command.

## Cinematic Sequence Asset Documents

[ADR-121](../../adr/121-cinematic-editor-document-and-authoring-context.md)
defines each sequence asset as a first-class persistent `SequenceDocument` rooted at
its stable `AssetId`. `EditorWorkspaceController` and the document tab host own its
session, route, typed command/history, dirty, save/autosave/recovery/conflict and
derived compile/preview revisions through this document contract. The Create Sequence
modal is only a transient asset-creation workflow; it opens the persistent tab after
atomic publication and owns no hidden document or undo stack.

An optional generation-checked `SequenceAuthoringContext` attaches the sequence
session to an immutable SceneDocument binding-resolution snapshot. It enables scene
picking, property enumeration, recording and preview but owns no authored content.
The sequence remains editable without a scene. Rename/reorder preserve stable object
bindings; wrong scene, deletion, component/schema change, reload or scene replacement
publish derived typed stale states without clearing or name-retargeting the authored
identity. Repair is an explicit undoable Sequence command.

Sequence edits and scene edits remain in their owning documents. An intentional
two-document action uses the staged multi-document application transaction; UI cannot
run two direct mutations or maintain a third history. Scrub/play state, live handles,
authority leases and viewport state belong to a disposable preview/session projection
and never enter document storage. Save, autosave, recovery, external conflict, Save
As/Copy As, close guards and stale worker-result rejection use the shared policies
above unchanged.

## VFX Effect Asset Documents

[ADR-129](../../adr/129-vfx-editor-document-live-preview-and-module-authoring.md)
defines each effect asset as a persistent `VfxEffectDocument` rooted at stable asset
and source revision. Document services own session/revision, typed commands/history,
dirty/save/autosave/recovery/conflict and derived compile/preview state; the panel host
owns the tab route and presentation lifecycle. Existing particle-system assets migrate
through the effect source schema rather than remaining a second document class.

Stack and graph are independent source kinds with distinct command/layout models but
one catalog, semantic compiler and ADR-126 `CompiledVfxEffectDescriptor` target. A
deferred/unavailable graph UI preserves source or exposes read-only inspection; it
does not flatten or save the graph as a stack. Compiler and preview success never
advance saved revision or clear dirty state.

Effect-spawned/reusable decals are authored within the effect document. Scene-placed
decal components remain SceneDocument state. Projection manipulation uses transient
preview overlays and one typed command to the owning document; intentional cross-
document edits use the staged multi-document transaction and never a third history.

Live preview compiles one immutable document snapshot with the ordinary VFX cooker and
activates the resulting descriptor through normal `VfxWorld`, CPU/GPU/Null, extraction
and Renderer services in an isolated preview host. Preview controls, particles, leases,
diagnostics and fixture inputs are disposable generation-fenced state. They do not
mutate documents or publish into live gameplay/Audio/event authorities. Close/project
teardown cancels derived work and retires resources through normal runtime boundaries.

## Terrain And Foliage Authoring Documents

[ADR-142](../../adr/142-terrain-foliage-document-tool-undo-and-preview-ownership.md)
specializes this contract for asset-rooted terrain datasets. Their dataset-local
foliage placement source is part of the same document, while reusable foliage-type
definitions remain separate referenced asset documents. Document services own
canonical height/weight/hole/spline/placement state,
revision, typed commands, bounded history, dirty/save/recovery/conflict and derived
preview revisions. Toolbars, viewport tools, palettes and overlays own presentation and
input only; runtime and cooked tiles are never editable document storage.

Each `TerrainEditOperation` preflights an exact canonical closure of affected tile/patch
rectangles, including seams, aprons and placement dependencies. One atomic history
record stores lossless bounded `before` and `after` values. Whole-heightfield/dataset
copies per edit, partial tile commits and undo by rerunning a brush, noise or scatter
algorithm are prohibited. A continuous gesture may show a disposable revision-fenced
overlay but commits at most one history transaction.

Heavy work uses immutable snapshots and document-owned cancellable task groups; only
the document owner may revalidate and commit a matching completion. Source save, editor
autosave/recovery, transient preview cook, published Asset cook and Runtime Save/
Persistent World foliage deltas remain independent. High-fidelity preview activates
ordinary Terrain runtime contracts in an isolated generation-fenced session and never
mutates the document or a live gameplay world.

## Fracture Asset Documents

[ADR-148](../../adr/148-fracture-document-generator-undo-and-preview-ownership.md)
specializes this contract for fracture assets. One persistent `FractureAssetDocument`
owns the unsaved working recipe/source/graph intent, revision, typed operations, bounded
semantic history, dirty/saved identity, recovery/conflict and derived generator/cook/
preview status. Assets remains the durable source and cooked-generation publication
authority. Panels, tree/graph views, inspectors and viewport tools own no source or
history.

Import and procedural generation capture one immutable exact-revision input snapshot
and produce a reserved detached candidate in a document-owned task group. Completion
returns to the owner and changes no document state. An explicit accept operation applies
the candidate's authored semantic patch atomically and records exact before/after values
or immutable source-section checkpoints. Undo/redo never reruns a generator or retains
production cooked meshes, Physics shapes/bodies or GPU resources.

High-fidelity preview cooks a captured source/candidate in a transient non-published
namespace and activates normal DFR/RuntimeScene/Physics/Render contracts in an isolated
`FracturePreviewSession`. Its world, bodies, events and resources cannot enter production
PhysicsWorld, DestructionWorld, streaming, persistence, replication or Assets current-
generation state. Save, production cook and preview each advance only their own typed
revision/status.

## PCG Graph Documents

[ADR-155](../../adr/155-pcg-graph-document-preview-bake-and-undo-ownership.md)
specializes this contract for PCG graph assets. One persistent `PCGGraphDocument` owns
the unsaved working source, monotonic revision, typed commands, bounded semantic
history, dirty/saved identity, validation and save/recovery/conflict state. Workspace,
graph canvas, palette, inspector, diagnostics and preview panels own presentation state
only and cannot mutate graph or Scene storage.

Validation, cook, preview and bake capture exact immutable document/catalog/dependency
revisions and return detached candidates to the document/workflow owner. Stale
completions cannot update current diagnostics or output. Undo/redo applies exact source
or target semantic patches and never reruns the graph.

Preview uses the ordinary transient PCG cook and runtime evaluator inside an isolated
`PCGPreviewSession`; it cannot publish source, production Scene, streaming, save or
network state. Bake is a separate explicit aggregate target-owner transaction whose
receipt records the exact graph revision. Graph saved/dirty and bake current/stale are
independent state dimensions, and provenance-aware bake undo cannot remove hand-authored
or adopted content.

## Runtime Conversion

The document converts to `RuntimeSceneDefinition` through an editor service.
Conversion:

- reads a stable document snapshot
- validates runtime-required fields
- resolves logical asset references
- emits structured diagnostics
- never gives runtime systems mutable document access

[ADR-087](../../adr/087-scene-to-physics-ownership-and-conversion.md) limits this
editor service to mapping explicit authored rigid-body, collider, trigger and
constraint components into typed Horo payloads with stable object/component/slot
IDs and source evidence. It does not infer bodies from hierarchy/render meshes,
choose native shapes/filters or allocate Physics objects. The Physics-owned plan
builder performs semantic conversion for editor, packaged and headless paths, and
the aggregate scene candidate owns activation/rollback.

Play and preview sessions record the document revision from which they were
built.

Runtime preview validity is keyed by the document revision and the identities
and revisions of resolved asset dependencies used during conversion. Runtime
preview caches are invalidated when `current_revision` differs from
`runtime_preview_revision` or any recorded dependency identity or revision no
longer matches.

## Data Bus

After commit, the document publishes:

```cpp
struct SceneDocumentChangedEvent {
    DocumentRevision revision;
    DocumentChangeKind kind;
    bool dirty;
    AffectedObjectSummary affected;
};
```

The event is bounded. Subscribers query document authorities for full state.
Only the document/history owner publishes committed document-change events.

`AffectedObjectSummary` is advisory for efficient UI invalidation. Subscribers
must tolerate incomplete summaries and query `SceneDocument` for authoritative
state.

## Threading

Document mutation and history execute on the editor owner thread. Workers may
validate, serialize, diff, or convert immutable snapshots.

Worker completion verifies document session and revision before committing save,
autosave, recovery, or preview results.

## Testing

Required tests cover:

- command apply/revert symmetry
- transaction rollback and one-entry grouping
- dirty state across save, undo, and redo
- bounded history eviction
- save with concurrent later edits
- interrupted and failed atomic writes
- autosave coalescing and backoff
- recovery retention bounds and clean-shutdown cleanup
- recovery after unclean shutdown
- external file conflict behavior
- stale worker result rejection
- document-to-runtime conversion isolation
- command failure after partial apply restores pre-command state
- preview transaction cancel restores exact pre-interaction document state
- delete object reconciles invalid selection
- Save As updates active document location only after durable write
- Save Copy As does not update active document location
- external reload while autosave/save worker is pending
- runtime preview invalidation when dependent asset identities or revisions
  change
- recovery file with stale/unsupported document format version
- bounded AffectedObjectSummary does not break subscriber correctness
- prefab override apply/revert/rebase/conflict resolution has command symmetry,
  one-entry history and no direct Inspector/file-watcher mutation
- apply-to-prefab failure or uncertain external publication preserves instance
  intent and reports reconciliation state
- sequence asset create/open/close uses a persistent document tab and shared command,
  dirty, save, autosave, recovery and external-conflict ownership without a parallel
  sequencer stack or serializer state
- sequence authoring context covers no/wrong/replaced scene, stable rename/reorder,
  missing object/component, schema mismatch, explicit repair and stale preview/
  binding/compile results without mutating authored identity
- VFX effect create/open/close uses one persistent document tab and shared command,
  dirty, save, autosave, recovery and external-conflict ownership without a particle-
  or decal-specific undo/serializer path
- stack/graph source kinds preserve independent commands/layout while compiling through
  one semantic target; unavailable graph UI never converts or mutates its source
- VFX preview compiles an immutable document revision and executes normal runtime
  services; stale derived work, tab/project teardown and decal-gizmo cancel/commit
  cannot mutate either VfxEffectDocument or SceneDocument unexpectedly
- terrain/foliage tools produce canonical bounded affected tile/patch closures and one
  exact before/after history entry per gesture without whole-heightfield copies
- terrain/foliage apply, undo and redo are symmetric without reading current tool state
  or rerunning brush/noise/scatter algorithms
- stale/cancelled terrain edit and preview completions cannot mutate document, dirty,
  history, cook or live runtime state across edit, reload, close or project shutdown
- terrain source save, autosave/recovery, preview cook, published cook and Runtime Save
  foliage persistence advance independently
- fracture editor surfaces share one typed document operation path and cannot directly
  mutate source, dirty state, history or artifact publication
- fracture generation candidates are detached, bounded and revision-fenced; completion
  alone cannot commit, dirty, save or publish them
- fracture undo/redo restores exact authored patches/checkpoints without rerunning the
  generator or storing production derived/native products
- fracture preview uses an isolated normal runtime composition and cannot mutate
  production Physics, Destruction, streaming, persistence or replication state

## Related Documents

- [Editor Data Bus](./editor-data-bus.md)
- [Editor Panel Host](./editor-panel-host.md)
- [Editor Modal Host](./editor-modal-host.md)
- [Project Model](./project-model.md)
- [Built-In Scene Primitives](../runtime/built-in-scene-primitives.md)
- [Scene Runtime](../runtime/scene-runtime.md)
- [Asset Pipeline](../runtime/asset-pipeline.md)
- [Prefab Architecture](../runtime/prefab-architecture.md)
- [Save Game And Persistence](../runtime/save-game-and-persistence.md)
- [Cinematic Sequencer](../runtime/cinematic-sequencer-architecture.md)
- [VFX And Particles](../runtime/vfx-and-particles-architecture.md)
- [Terrain And Foliage](../runtime/terrain-and-foliage-architecture.md)
