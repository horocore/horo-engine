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

## Main owning-host integration and cleanup boundaries

The manager's private manifest and recovery-report helpers now carry the explicit
selected-module span through failure admission and incompatible presentation.
Before native callbacks, exact provider keys are owned by the recovery guard and
new status rows are copied and capacity-reserved as a detached batch. Publication
then uses nonthrowing entry moves and status assignments. Failed admission cannot
partially publish that batch. Existing admitted rows require no allocation during
guard unwind. An empty selection retains whole-package admission behavior;
retirement marks only the loaded record's admitted module IDs Missing, preserving
independently owned modules from the same package. No public function signature,
object layout, module descriptor or native ABI export changes for this private
host migration. The changed headers require normal host source rebuilds.

The precise `cpp:S2486` exceptions on `WorkspaceStage` and `EditorRecoveryGuard`
are intentional destructor boundaries. A foreign filesystem cleanup exception
must not mask the original publication result or exception, and presentation
failure must not interrupt native activation rollback or failure-owner admission
closure. Neither catch grants authority or pretends cleanup succeeded. An owned
uncommitted stage may remain orphaned after failed removal. The writer lease
remains alive through private write, atomic replacement and stage cleanup.

The Windows split-parser allocation timeout has not yet identified its failing
position or proved a library/production root cause. Its exhaustive test now emits
bounded stack-buffer stderr markers for the measured request count, each attempted
index, the injected allocation size, and the final completed count. Every original
fault position, allocation/free oracle and per-test deadline is retained. These
markers provide evidence for the next actual Windows run; they are not a fix or
a local Windows validation claim.

## Completed manifest DOM retirement

The full admission allocation sweep exposed a completed nlohmann JSON destructor
allocating its descendant work list while retiring `ParseExtensionManifest`'s
private `Result<Json>`. LLDB observed `json::data::~data` crossing its noexcept
boundary into terminate. The private parser now returns a move-only manifest DOM
owner. Its cleanup scratch is reserved from the existing raw scan's observed
container depth **before** any DOM exists, then transferred with the completed
DOM through synchronous schema validation. Retirement iteratively removes scalar
children and empties containers in postorder using only the preallocated scratch;
no C++ recursion or new heap requests are needed at that boundary.

Default depth16 and all nonzero caller budgets remain unchanged, including custom
admission above depth64. Document bytes, duplicate fields, version, malformed JSON
and schema diagnostics retain their existing authority and order. The change is
private to the Extensions implementation; public parser signatures, wire format,
object layouts and native exports do not change. This addresses completed DOM
retirement only. It does not claim persistent-OOM safety for upstream partially
constructed parser state. Whole-position valid/schema-error/custom-depth sweeps
and the original full admission sweep must execute after the change.

## Windows allocation qualification composition

Only the two new global allocation-failure sweeps are omitted from MSVC checked
Debug compilation, following the existing Scene allocation qualification policy.
Checked STL container constructors and moves can allocate iterator proxies inside
`noexcept` boundaries, making global one-shot failure injection unsupported for
that composition. Debug retains the functional admission, recovery, rollback and
manifest validation cases. This is a supported-composition decision from the
existing contract, not an observed Windows stack for these two new cases.

Windows `ci-audio-release` builds the entire engine and Manager with the same
Release STL ABI. Two exact registered CTests must be discovered and executed with
zero failures, errors or skips. Their output must prove a nonzero measured count
and equal completed count for the admission sweep and all three manifest sweeps.
No position is sampled or disabled in Release. Other supported compositions run
the same full sweeps. The separate legacy Split-parser timeout remains unproven;
its full Debug test and bounded diagnostic markers remain unchanged.
