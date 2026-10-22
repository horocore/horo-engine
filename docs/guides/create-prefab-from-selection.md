# Create Prefab from Selection Subtree

The hierarchy's **Create Prefab from Subtree** action opens a transient filename
modal. Its request captures the ordinary root, document revision and replacement
epoch. Confirmation rechecks the actual workspace owner, Play state, pending
recovery and external Scene conflict before obtaining the shared project mutation
lease. A stale modal cannot apply to a newly loaded or subsequently edited Scene. The last accepted
persisted Scene byte identity is checked synchronously before source writes and
again before commit under its existing source publication lease. Unsaved working
content is not compared to disk. The existing empty `settings.defaultScene`
opening creates an explicitly untitled Scene without a disk fence; a missing or
changed persisted source is never treated as untitled.

The destination must be a portable `.prefab` filename under the existing project
`assets` directory (or the existing legacy `Assets` spelling selected by ProjectLayout). Parent directories must already exist and cannot be redirects;
protected project directories are excluded. Both the source and `.horo` identity
sidecar must be absent. The workflow obtains both canonical source publication
locks before attempting either exclusive creation.

## Source and reference boundary

The complete selected subtree receives new parent-first prefab-local object IDs
and stable component occurrence IDs. Its containing-scene parent and root transform
move to the linked placement; the source root transform becomes identity. Child
local transforms and component values retain their canonical meaning.

Built-in payloads use the existing canonical Scene encoder and prefab component
projection/validation. Preparation consumes an exact canonical Scene capture;
numerical authoring validity remains enforced by the canonical document load and
command admission before export. Creation does not duplicate those validators.
Registered scalar behavior fields require the exact captured
active descriptor and schema version. Supported asset references retain their
AssetIds, including references on disabled authored components. They must resolve in
the captured registry; physical material and navigation definition types are checked.

No new reference persistence or binding algebra is introduced. Every explicit
Physics body reference, Character excluded-body reference and navigation surface
relationship crossing the selected boundary is reported with its owning object,
property and target object. Incoming references are inspected too. Internal object
Collider and constraint endpoints wholly inside the selection are rewritten through
its exact parent-first local identity map, including reserved root ID zero, and
retain their body slots. The canonical private prefab decoder admits that local
identity domain; ordinary Scene decoding and Runtime validators still reject zero
Scene IDs. Source conversion resolves locals through `PrefabSceneIdentityMap`,
and cooked spawning resolves source provenance into typed group fixups before
runtime use. Character excluded-body and navigation relationships whose local
rebinding codec is unavailable are rejected explicitly.
The current typed reference capabilities are:

| Authored site | Internal selection | Crossing / incoming policy | Existing rebind owner |
| --- | --- | --- | --- |
| Object parent | Parent-first local ID; source root detached | Containing parent moves to placement | Prefab hierarchy / Scene identity map |
| `colliders.body` | Exact local object and body slot, including root 0 | Reject with source, target and property | Source identity map; cooked source-provenance group fixups |
| `physicsConstraints.first.body`, `.second.body` | Exact local endpoints; world anchor retained | Reject with source, target and property | Same canonical Physics endpoint owners |
| `characterController.collision.excludedBody` | Explicit unsupported diagnostic | Reject incoming and outgoing, including disabled data | No current local rebind owner |
| Navigation surface owners from sources, regions, modifiers and link endpoints | Explicit unsupported diagnostic | Reject incoming and outgoing with source, target and property | No current creation rebind owner |
| Typed AssetIds (audio, shape, material, navigation definition) | Preserve exact identities; validate registry/type | Asset references do not become Scene object bindings | Captured AssetRegistry / canonical component codec |
| Nested prefab parent | Explicit nested-boundary diagnostic | Reject attached nested occurrence | Existing placement boundary is preserved |
| Opaque custom component payloads | Explicit unsupported diagnostic | Reject even outside selection because inbound references cannot be proven absent | No interpretation or guessed repair |

Opaque custom component payloads cannot prove reference safety and block creation,
including possible incoming references from outside the selection.

Nested prefab boundaries, generated/locked objects and nonportable editor state
are rejected. Imported mesh references, legacy trigger volumes, Audio Listener
payloads and other representations without the existing prefab codec are rejected
rather than omitted. AI decision-asset projection and explicit navigation-source
relationships remain unavailable in this creation tier. These restrictions affect
creation admission; existing authored sources remain unchanged. Reference repair,
including an explicit selection expansion or a future binding-slot workflow, is a
separate user action. The operation never nulls or copies an external target. The modal shows a bounded
first 32 sites and an explicit remaining count; the immutable transaction result
retains the complete audit for recovery and diagnostics.

## Commit, failure, history and recovery

`SceneDocumentTransactions::PreparePrefabReplacement` reserves the complete
candidate root, linked placement and one bounded canonical semantic history entry.
This extends the existing transaction owner; it does not introduce a second Scene
or history authority. All fallible authored-state preparation completes before
source creation. The owner/state fence is rechecked at final publication, whose
root/history swaps allocate nothing and invoke no callbacks. Canonical selection
clearing and prefab-root selection are also completed before observer notification.

Source and sidecar creation use `WritePrivateDurable` and retain their exact
created/content receipts. Failure rolls back only an unchanged, regular, single-link
owned creation. Missing identity evidence, foreign replacement, modified content,
partial/uncertain writes and cleanup failures retain explicit per-file recovery
receipts and diagnostics. The workspace retains an unresolved outcome instead of
allowing a subsequent attempt to overwrite its recovery evidence.

The additional optional `FileEntryInfo::identity` is opaque same-platform object
and mutation evidence from the existing native no-follow read snapshot. This narrow
public contract is needed to distinguish a foreign same-byte replacement during
rollback. Native inspection supplies it when its no-follow read snapshot is available;
metadata-only inspection remains supported with identity unavailable. Existing custom providers default to
unavailable and cleanup therefore fails closed. Callers compare tokens without
interpreting native IDs. The public header consumer and native replacement tests
cover this additive migration. No other existing caller needs to supply a token.

Identity/digest checks do not implement an OS-level compare-delete. Project and
per-source leases serialize cooperating writers; noncooperating changes after the
last check remain outside that authority. No cross-file power-loss atomicity or new
general journal is promised. A crash may leave an unreferenced asset pair or a
partial creation requiring inspection. Existing Scene autosave/recovery and dirty
state remain owned by their current services; this workflow does not mark the Scene
saved or discard its recovery data.

One Undo restores the exact ordinary subtree and removes the linked placement;
Redo restores the placement. The new reusable asset remains on disk on Undo, which
avoids deleting a source another document may already reference. Notification or
derived projection failure after commit is reported separately and never rolls
back the files or the committed Scene/history/selection.

## Validation scope

Focused tests exercise real canonical document/history owners, source/sidecar
persistence and codecs, one-step Undo/Redo, initial dirty-state preservation,
destination conflicts, cancellation, stale revision, owner shutdown, second-file
failure, provider exceptions, foreign different-byte and same-byte replacements,
missing identity and failed cleanup receipts. Controller tests exercise the actual
menu command endpoint, selection, retained source capture and postcommit observer
failure. Public consumer coverage includes the prepared replacement and optional
opaque identity contracts. Local and hosted execution evidence is recorded in the
PR; this guide does not claim those checks have already run.
