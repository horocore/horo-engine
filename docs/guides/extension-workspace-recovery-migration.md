# Extension workspace recovery

HORO-113 / #113 adds host-owned unavailable-surface projections to the existing
workspace authority. No provider descriptor, executable callback, activation
lease, trust decision or process generation is reconstructed from disk.

## Source contract and callers

`EditorSurfaceRegistry::RecoverySnapshot` returns bounded copied workspace intent
for pending contributions and unavailable registered contributions. Entries carry
stable surface/package/module identities, opaque state, placement and the current
host availability reason. A configured Active status cannot make an absent
contribution active. Disabled, incompatible and failed reasons are process-local;
workspace schema3 and registry schema2 remain unchanged. Old documents require no
recook or rewrite. Session/project state stays outside workspace persistence.

The explicit GUI startup composition projects actual inventory enablement,
host-fit and activation results. Enable/disable changes remain restart-applied;
a currently active provider is not mislabeled disabled. The native manager
reserves bounded reason records before admission, permits candidate registration
through the real transaction, retains Active only when publication succeeds,
marks rejected candidate activation
failed and retirement missing. A successful retry restores the original user
intent only through real context/registration admission. Unknown or removed
contributions stay Missing, including contributions removed from an otherwise
active package. Reasons are not activation evidence.

The workspace GUI caches localized placeholders only on registry/locale changes.
It shows the stable identity, provider and actionable reason for saved open
surfaces. The explicit Close provider workspace surfaces action changes open/focus intent
for the exact provider's workspace group atomically; opaque bytes and placement
remain recoverable. Persisted legacy entries do not carry canonical activity/drawer
pair metadata. Closing the whole group preserves pair consistency without
fabricating a missing descriptor or weakening registration validation. No absent provider is called, no placeholder can
publish a form or queue a native action, and no synthetic document tab is added.

## Durable publication

`WorkspaceLayoutPersistence::Save(files, receipt, ...)` borrows the host's existing
`DurableFileSystem`. The editor supplies its composed project filesystem. The former implicit-native
Save/SaveLayout signatures are removed: EditorServices must never select a
concrete filesystem. All callers now pass their composition's filesystem and a
fresh replacement receipt; standalone tests compose native files explicitly at
their entry boundary. WorkspacePanelHost delegates the same borrowed owner and
receipt. This deliberate host-source migration preserves one publication path. All complete
candidates are validated before writing. The exact destination writer lock,
exclusive private staging and `AtomicReplaceTracked` are existing platform
primitives. A failure never removes the old destination, adopts a foreign stage,
or silently falls back to delete-and-rename. Only a stage created by the current
invocation may be cleaned up.

A false result with an uncommitted receipt leaves the previous document intact.
A false result with a committed receipt means the new complete document may be
published but directory durability was not confirmed. The editor retains its
admitted/dirty workspace intent while that screen exists and reports uncertainty;
it does not claim a rollback or confirmed save. Forced host teardown cannot promise
to persist newer in-memory intent on failed storage; the last complete document is
retained and uncertainty is logged. External host C++ callers should migrate to the
borrowed-filesystem overload and inspect the receipt before presenting recovery.
Rebuild source consumers of the changed C++ headers. Native Extension ABI1.4 and
GameModule SDK21 exports/layouts do not contain these host C++ contracts and are
unchanged; no native ABI epoch bump is appropriate.

## Owning validation

Registry coverage exercises repeated restore/save, zero-byte opaque values,
missing/disabled/incompatible/failed states, malformed and capacity admission,
close/shutdown and actual registration recovery. The real C11 activity fixture
runs activation failure followed by reinstallation with paired open state and
opaque drawer bytes retained. Filesystem faults exercise precommit preservation,
postcommit uncertainty, non-ASCII/spaced paths and foreign-stage survival. GUI
coverage renders real shared controls in a narrow headless ImGui context and
checks placeholder withdrawal on close/shutdown. These tests must execute in
canonical Manager, workspace persistence and GUI activity-view target routes;
source authoring alone is not runtime evidence.
