# Runtime console implementation and qualification

This is an implementation evidence ledger for DBG-001, not acceptance of an
unverified product composition. The original source baseline is `f51e0c0f44`;
the local console checkpoint was rebased unchanged through parents `5d0aff9d63`
and `d774e3dbd4`, then onto published parent `3df476ff430a` on 2026-11-13.
The current checkpoint is `2fc7c3c22d1f`; `git range-diff` confirms the checkpoint
content is unchanged. Before the latest integration, all 99 dirty source paths
were backed up. Restoration used the topic's immutable stash identity; the
original path inventory and unstaged state were preserved. All 96 nonoverlapping
files retained their contents after line-ending normalization; the three
overlapping files were merged with the published parent. This does not import
the main owner's unpublished changes or publish the branch.

Commit `b94a1a3` adds backward-compatible nonblocking heavy-lock admission and
its native-OS lock regression. The default remains blocking for existing callers;
bounded validation explicitly requests `blocking=False`. The two lock tests pass.

After the latest rebase, the console artifact-verifier, CI-grouping, Windows-quality
integration and local-build-cache Python modules passed together: 147 tests,
with two compiler/cache-dependent tests skipped. These are Python/tooling checks;
native console/GUI/SDK/profile/performance and final analyzer evidence remain
pending. The actual stacked quality base is `origin/codex/editor-improvements`
at `3df476ff430a`; the local branch of the same name contains unpublished work
and is not this validation base. Final preflight requires fresh worktree-specific
compile commands and C++ sensor evidence.
GitHub #1812 / Jira HORO-1768 and all seven children were read on 2026-11-11.
The GitHub parent and all seven children were reread live during the final source
integration pass on that date: policy remains closed/completed and the other six
children remain open, with unchanged acceptance scope. This does not change any
issue or narrow the required packaged/runtime/profile/downstream evidence.
The policy child #1842 / HORO-1798 is closed/Done. ADR-018 still says Proposed;
this work follows the normative runtime document without changing ADR status.

| Child | Implementation | Remaining acceptance evidence |
| --- | --- | --- |
| #1842 / HORO-1798 | Baseline written permission, registration, threading and packaged policy plus NET-007.9/NET-008.12/WST-010.8 reconciliation in the normative runtime document; inert descriptor revision is explained below | GitHub is closed/completed. ADR-018 remains Proposed; closure is not evidence of implementation/runtime qualification |
| #1843 / HORO-1799 | Source added: owned immutable registry, inert metadata, typed schema conversion, redacted bounded history, filtered discovery, startup allowlist, no-follow exact-path script capture and optional external SDK component | Native and SDK regressions have not run to completion |
| #1844 / HORO-1800 | Source added: actual thread identity checks, cutoff owner/render queues, JobSystem dispatch, existing OperationStore projection, cancellation and nonblocking shutdown; Editor core bootstrap is wired into actual owner/render phases | Native race/lifecycle and graphical-host qualification; remaining catalog/thread-capability integration; general JobSystem main-thread continuation remains unavailable |
| #1845 / HORO-1801 | Source added: help/find/version/clear, real public Scene inspection bindings, real Telemetry availability query and queue-admission audit adapter, approved Foundation support ZIP binding, typed variable inventory, profile-compiled default variable schemas, unified permission-filtered help/find, common executor text-to-variable dispatch, canonical ConfigurationService reads/commits, actual Editor history/visibility variable consumers, five apply policies and durable configuration-owner interface | Remaining three standard command handlers, host consent workflow, actual durable host adapter, remaining variable inventory/consumer host composition; native regressions |
| #1846 / HORO-1802 | Source added: terminal input controller, central modal routing, bounded layout/focus registry, logs/jobs source projections, 4 Hz refresh and virtual row windows; Editor development terminal and real log/operation panels use the same process authorities with localized shared controls and gallery regressions | Native/graphical qualification, packaged game-runtime surface, eight unavailable source panels, full style/IME/localization interaction states and measurements |
| #1847 / HORO-1803 | Source added: six-profile compile-selected command/variable catalogs, separate availability translation units, exact compiled-profile registry factory, canonical snapshot-backed configuration/Invocation capture, typed panel requests, Shipping local-UI opt-in, denied Shipping/Server overlay surfaces and fail-closed unsupported remote requests | Native six-profile builds and binary inspection; complete product artifact/config/launch wiring in a graphical host |
| #1848 / HORO-1804 | Native regression sources, optional SDK fixture and linked profile artifact/core fixture with bounded binary/manifest verifier added; Python verifier and CI grouping checks pass | Native fixture/results, final quality preflight, packaged development/shipping evidence, performance, platform and downstream integration evidence |

## Public boundary and migration

`HoroRuntimeConsole` owns the command registry, executor, core builtins and
variable contracts and depends only on Foundation. `HoroRuntimeConsoleStartup`
adds the existing root-bound ProjectReadFilesApi capability; `HoroRuntimeConsoleUi`
adds the existing Input capability. Each has one explicit public-header owner and
an automatically generated staged-header consumer. `HoroRuntimeConsoleDefaults`
owns the profile-selected catalog and explicit Telemetry adapters.
Core now also owns `ConsoleConfiguration.h` and the inert `OverlaySource.h`
identity contract. The latter was promoted from the in-branch UI header, keeping
all ten existing ordinals and adding a Count sentinel. Startup and UI share this
one source inventory without adding a reverse dependency or a second enum.
`HoroRuntimeConsoleScene` adds a private dependency on the existing Scene service
only in Editor/Development; other profiles compile an empty contribution without
the Scene dependency. No existing API is replaced. CLI descriptors and the
editor log pane remain separate presentation concerns; neither is evidence of
a running in-game terminal.

Command descriptors have allocation-free borrowed metadata. `Create` copies all
accepted bytes and nested enumeration values before returning an immutable
registry. Descriptor construction and composition invoke no service, handler,
backend discovery or activation. Handler binding is a separate host step rather
than a callable member in metadata; this makes the documented inert boundary
enforceable without exposing an executable pointer through discovery. Command
errors also expose an inert domain contribution for explicit ModuleHost registration.

Admission capacity includes owner/render requests already removed from the cutoff
queue and currently executing. Credits remain protected by the existing admission
mutex until terminal publication, and the drain guard releases remaining credits
on unwinding. A handler cannot regain its own capacity by resubmitting during
execution. Regression sources cover owner and render paths, normal completion,
handler exceptions and restored capacity after completion; native execution of
these new cases remains pending the allocated build slot.

`CommandExecutor::ValidateOwnerBoundary` now exposes a read-only admission check
under that same existing mutex, before host-owned deferred activation. The Editor
adapter no longer mirrors shutdown in its own Boolean: stopping the dispatcher
through either facade prevents a later owner call from committing its pending
configuration request. Wrong-thread and nested same-thread owner/render drains
also fail before host activation; an independent render thread does not block the
owner boundary. The check drains/reserves nothing and calls no store/handler.
It is an observation, not a lease or rollback promise for a concurrent Stop after
the observation. The host retains its existing lifecycle ownership. This adds
one active-path mutex observation; no performance improvement/budget is claimed.
Regression sources cover stopped pending-write preservation through both facades,
non-consuming/wrong-thread checks, same-thread reentry and a joined, deadline-
bounded independent render drain. Native execution and Core consumer rebuilds
remain pending.

The in-branch `ConsoleTerminal` constructor also requires the existing canonical
`ConfigurationService` alongside its existing process operation query. Every
eligible interactive submission captures that service's immutable snapshot and
passes it to the common executor. Worker execution never borrows the service or
polls newer revisions; painting performs no configuration query. The service must
outlive the terminal controller. A real single-worker regression source parks
the worker, submits with one committed setting value, publishes a newer value,
and checks both the retained first capture and a fresh subsequent submission.
It uses the actual Foundation configuration, scheduler and operation authorities;
native execution remains pending. Existing terminal test callers are migrated.
This does not establish production host or graphical presentation integration.
Options accept `--name value` anywhere, or `name value` after positional
arguments. A positional string equal to an option name remains a literal;
mixed duplicate option spellings are rejected before dispatch.

External projects opt into `find_package(HoroEngineGameplay REQUIRED COMPONENTS
RuntimeConsole)` and link `HoroEngine::RuntimeConsole`. The default SDK inventory
does not import this component. Its dedicated fixture checks descriptor-owned
namespace registration without importing Physics or RuntimeUi. This is a
build-tree SDK component, following the existing gameplay SDK export mechanism;
it is not evidence of a new installed product distribution.

The standalone SDK consumer now builds a project-owned `sample.inspect` handler
beside the real core commands, uses the same public configuration contribution
and four control factories, and exercises actual owner dispatch, permission
denial, process operation identity/output, help discovery, pending NextFrame
publication, bounded history and queued-operation cancellation through Stop.
It calls the exported owner-boundary validation API before activation and after
shutdown. No Editor host adapter, private header, native renderer or fabricated
engine subsystem handler is used. Its UI-control inventory checks public Core
ABI/state composition; it does not qualify a graphical UI consumer.

SDK configure now performs a fresh C++20 compiler/header-boundary probe using
only the imported optional target: Core configuration/source headers must compile,
while public Editor widgets, the private source-tree input header and the private
runtime view header must remain invisible to `__has_include`. The probe cache is
cleared on every SDK configure. Existing default/optional import checks still
reject unrelated Physics/RuntimeUi imports. The CMake runner bounds consumer
execution to 45 seconds and rejects numeric failures or textual timeout results.
These are new fixture sources; neither the external configure, compiler probe,
link nor execution has run on the current inputs. The separately allocated SDK
scope still requires `HoroRuntimeConsoleSdkContract` and its actual output.

`ConsoleHistory::SetEntryLimit` adds an explicit owner-boundary entry-budget
operation. Limits outside 1–10000 fail without mutation. Shrinking removes the
oldest rows in one range, updates byte/pending counts and advances the visible
history revision. Growing reserves bounded storage at this explicit boundary;
existing input/output/aggregate byte budgets remain unchanged. Eviction never
cancels a process operation or resurrects it on later result projection. Borrowed
rows can be invalidated by growth or eviction, so callers retain no row span
across this operation. Core public-header consumers require a fresh build.

`ConsoleHistoryVariableDescriptor` now owns the one inert history-control schema
used by both the non-shipping product catalog and explicit Editor composition.
Its unchanged `ConsoleHistoryLimitVariableId` is declared by the Core configuration
header; including `ConsoleProduct.h` still exposes the same name/value. No second
ID or competing descriptor is retained. The factory lives in its own static
archive object; the Shipping catalog does not reference it. This link boundary
requires fresh final-binary inspection as well as the catalog archive check;
source separation alone is not binary-absence evidence. Directly supplying this
Developer/NonShipping descriptor to a Shipping registry still fails even when
its ID is in a host variable allowlist. Public-header ownership remains Core for
configuration and Defaults for the product facade, and both consumers must rebuild.

`ConsoleVisibilityVariableDescriptor` likewise supplies the one Public session
Boolean schema shared by the product catalog and actual Editor. Its unchanged
`ConsoleVisibleVariableId` now comes from the same Core header and remains visible
through the product facade. The separate Core factory object is referenced by
Shipping's allowlisted Public catalog, while Server omits that contribution.
Archive positive controls exclude shared Core-owned control factory strings; the
linked-image scan and exact runtime catalog still require every selected name.
Native link/absence qualification remains pending.

The owning runtime architecture requires exactly one permission per descriptor;
grant masks may combine permissions. Restricted additionally requires an
authenticated context, and AdminCheat additionally requires server authority.
Namespaces use the canonical engine prefix inventory plus explicit host-approved
project prefixes. A terminal command cannot also own a namespace prefix.

Entity arguments carry the complete `scene:index:generation` reference in a
typed transport DTO. A scene adapter must map all three fields into its existing
`EntityRef` and validate that exact reference; it must not resolve a bare slot in
whichever replacement scene is active when the queue drains. No existing caller
is migrated because this console API is additive to the source baseline.

Console history stores only the prepared redacted projection. Raw malformed or
denied input is not retained. Handler output must be a safe presentation value;
this history type is not a replacement for the structured logging store.
Asynchronous rows retain the accepted OperationId and project results from the
same authoritative OperationStore at most 4 Hz. Terminal construction requires
that query authority explicitly. Pending rows, source cardinality and output
bytes remain bounded; a temporary sorted ID index avoids a history-by-store
quadratic scan. Updating a result does not evict peer rows. Clear/eviction cannot
resurrect a row, and an expired result is explicitly unavailable rather than
assigned a fabricated lifecycle state. Failure history shows only the stable
error code, excluding nested diagnostic payloads. These new regressions are
source evidence only until the native suite runs.

The in-branch history projection now also rejects a record count above the
source's advertised active+recent capacity. Before publishing any expected row
changes it validates every visible candidate's UTF-8 against a simulated
history-order byte budget, including space freed or consumed by earlier rows.
A malformed later result therefore leaves prior rows, pending counts and the
history revision untouched; a corrected snapshot can be retried at the same
source revision. Successful rows keep the existing sequential clipping policy.
This tightens malformed-input behavior of the new in-branch API; no source-baseline
caller exists. `ConsoleTerminal` and the Editor owner adapter retain the same
Result contract and require rebuilds rather than a compatibility state store.
The preflight adds a bounded read-only pass with no staging heap allocation;
allocation exceptions during output construction are not claimed transactional.
Regression sources cover late malformed output revealed by an earlier shrink,
same-revision retry, impossible capacity before unavailable-result publication,
and preserved growth/shrink clipping order. None has run natively yet, and no
performance/budget result is inferred from source inspection.

Runtime-only variables own their typed effective state. Configuration-backed
variables always read the canonical immutable snapshot; deferred session changes
are typed write requests, not effective configuration mirrors. Durable writes
require an injected configuration workflow that owns persistence and pending
publication together. The console has no direct file-writing fallback. Secret
reference settings cannot be exposed as console variables. Foundation currently
supports Boolean/Integer/String configuration values; floating-point diagnostic
variables are session-only rather than being coerced into strings.
Deferred configuration requests retain their validation revision. A concurrent
configuration publication causes a typed stale-write failure at activation;
the pending request remains visible instead of overwriting the newer value.
Interactive `name` reads and `name value` writes pass through the same command
executor, bounded owner queue, cancellation and operation store. They share
command tokenization and complete-token numeric conversion. Deferred writes
report active and pending values separately. Script startup remains explicitly
command-allowlisted; variable fallback is disabled for startup submissions.

`sys.metrics` reads Telemetry's bounded privacy-safe diagnostic snapshot. It
reports process-local instrument IDs, generation and typed availability, not
descriptor names, producer payloads or metric history. The explicit audit
adapter emits stable structured events with command ID and prepared redacted
text, excluding ambient context. Rejection by the existing telemetry queue
prevents execution. Acceptance is asynchronous queue admission, not a durability
or sink-delivery guarantee. Neither adapter initializes telemetry.

`diag.support_bundle` binds the existing Foundation ZIP generator through an
explicit host approval authority. Requests contain only previously approved
source allowlists, vetted metadata and an output path; approval acquisition must
not block on UI from a worker or reuse consent. The adapter limits work to 256
entries/64 MiB, dispatches to the real I/O queue and requires the common audit
gate. Sensitive result/error presentation is redacted. Cancellation is checked
before taking approval and before starting generation. Foundation's bounded
atomic generator has no internal cancellation API, so cancellation after that
point may still leave the approved committed output. No output is deleted by
the adapter. A production host consent workflow is still required.

The terminal controller acquires an actual InputRouter modal/text context and
submits through the same executor. It does not yet construct or paint a packaged
runtime UI document, start native IME input or provide a game host executable.
It forwards the central router's committed Unicode/pre-edit delivery once per
eligible frame, preserving modal preemption and focus-transfer discard rules.
Unit coverage of this controller must not be described as packaged UI evidence.

The logs adapter now filters severity, exact/dotted-prefix category and literal
message text without changing producer policy. It rebuilds only at the existing
4 Hz cutoff, including filter changes against an unchanged source revision.
Display/search messages are limited to 4096 complete UTF-8 bytes and categories
to 128; truncated rows are identified explicitly. Optional repeat grouping
compares complete bounded severity/category/message/context, so distinct
contexts and truncated payloads never merge. Display rows retain the original
immutable record and borrow its bytes. Reused bounded metadata buffers support
virtual windows of at most 128 rows without per-paint allocation. Hiding drops
snapshot/row references. Invalid new source data retains the previous valid
view and revision for retry; a log failure does not prevent the independent
operation panel from refreshing.

Operation windows now return `ConsoleOperationRow` rather than raw records. Each
row pins its original immutable process snapshot and exposes complete UTF-8
prefixes for title (512 bytes), phase (128), message (4096) and stable error code
(256). Raw nested error messages are excluded from display. Identity, lifecycle,
kind, finite progress, duplicate IDs and aggregate capacity are validated before
publishing any rows or advancing the accepted source revision. A malformed
snapshot preserves the previous valid view and can be retried at the same source
revision. Copied rows retain their original snapshot after the panel hides.
These display bounds do not change the OperationStore owner's snapshot-copy or
payload-size policy; no bounded-copy performance result is claimed.

The new in-branch presentation call sites were updated together: terminal
construction now requires the executor's operation query explicitly, and log
windows return `ConsoleLogRow` display fields/repetition counts instead of raw
record pointers. Operation windows likewise return the bounded pinned DTO. None
of these contracts existed in the source baseline. They remain
under the same UI header owner and require fresh public-header consumer builds.

## Canonical launch/configuration flow

The process root registers the inert `ConsoleConfigurationDescriptors(profile)`
contribution before sealing its shared Foundation schema. Console defaults do
not register themselves, query services, discover backends or create a second
resolver. Typed Boolean/Integer settings cover initial console/overlay requests,
the history limit, startup execution policy and each canonical panel preference.

The actual `HoroEditor` process root now registers this Editor-profile metadata
in `PrepareEditorConfiguration`, before `CreateEditorConfigurationSchema` seals
the same module/XR/editor schema. It links only the Core console contract for
this inert registration; no command handler, endpoint, input context or GUI is
activated by schema construction. Duplicate/conflicting module keys fail startup
through the existing error boundary instead of replacing a descriptor. The
`HoroEditorSettingsStoreTests` regression composes the real editor schema with
the console descriptors, resolves user/project inputs through Foundation and
checks that editor appearance commits and console history commits preserve each
other's values and earlier snapshots. The graphical host build and this additional native editor test target remain
unrun and outside the requested first core-only build scope.

The non-installed `HoroEditorConsoleLaunchInternal` target stages only its two
host headers and depends on ConsoleStartup; it exposes no broad application/src
root. The actual process first calls `CaptureEditorConsoleLaunch` and passes only
the remaining host slice to renderer, lighting, XR and editor parsers. Console
command/script payloads are opaque to those parsers. The root-owned table records
the existing host options that consume a separate value; adding a host value
option requires updating that token-ownership table. Console capture is bounded
to 256 tokens, and the enclosing invocation to 4096 arguments/1 MiB. Unknown
console spellings, malformed or remote requests fail before host/native startup.
`MergeEditorConsoleInputs` checks every Invocation key for collisions before
inserting any contribution, then the one canonical resolver handles all sources.

The Editor root resolves the typed launch from that same snapshot and prepares
the four-command core inventory (`help`, `find`, `version`, `clear`) plus the real
process Telemetry availability query and two read-only Scene descriptors before
native presentation. Only the four
core commands remain startup-allowlisted. Unknown/unbound capabilities and scripts without a composed
exact-path project authority fail preparation; this bootstrap does not fabricate
script grants or default handlers. Requested core startup is activated by the
actual `EditorRuntimeParticipant`. It uses the existing process JobSystem and the
same OperationStore, whose root ownership is now shared rather than a borrowed
stack object. Startup receipts remain owned by `EditorConsoleExecution`; redacted
history uses the same core history authority, and immediate safe output goes
through normal host logging. Owner commands drain at
`ApplyQueuedOwnerThreadCommands`, render commands at `RenderExecution` entry, and
shutdown stops console admission before stopping scheduler admission. Pending
history projects the existing operation store at most four times per second.
The root now shares ownership of that same canonical ConfigurationService with
the active variable bridge rather than manufacturing a service or a non-owning
shared-pointer alias. Existing Settings/XR/native-host callers retain references
to that exact object. Its event authority retains the existing process lifetime.
Lazy activation reads the current validated history limit, including
changes published after inert startup preparation. At each active owner entry,
it captures one immutable snapshot; only a changed revision projects the history
key through `ReadConsoleHistoryLimit` and applies the entry budget. Configuration
published during the command cutoff is consumed at the next owner boundary.
Render painting does not query this setting, and wrong-thread/stopped owner
dispatch cannot resize history. The inactive path creates no per-frame history
configuration query. Snapshot acquisition in the active tooling path and resize
costs remain unmeasured. Regression sources cover lazy activation, owner/render
separation, invalid-value recovery, stopped/wrong-thread access, eviction,
pending-result retirement, growth and preserved byte budgets. This supplies the
real history consumer. The Editor dispatcher now also owns the single shared
`console.history.limit` variable contribution and binds it into the same core
help/find inventory. Developer text submissions use the existing bounded owner
queue and process OperationStore; Public discovery/writes are denied before any
operation is created. Previously pending NextFrame requests apply at owner entry,
before draining the new cutoff, so a newly admitted pending write waits for the
following boundary. The configuration consumer then resizes actual history.
Owner shutdown closes activation as well as dispatcher admission.

A stale pending write remains pending and preserves its original typed activation
error. It does not fail the host frame or stop draining replacement commands.
The root reports the stable code through normal logging once per diagnostic
transition; it excludes candidate values and nested private payloads. A newly
submitted valid replacement can activate on a later boundary and clear that
diagnostic. The host regression source exercises the complete permission,
admission, pending, canonical commit, history resize, stale recovery and shutdown
path. Full variable inventory composition, durable persistence and the other
variable consumers remain unfinished; native execution is still absent.

The Editor now also composes `console.visible` into that exact inventory. Explicit
activation seeds it once from the resolved launch request; runtime visibility
writes do not modify the launch-only configuration preference. The view is created
from the typed session value and projects it again after the real owner cutoff,
then synchronizes central input eligibility through existing modal/native barriers.
The mapped shortcut and close/Escape intent use `RequestConsoleVisibility`, which
submits the Public variable through the same bounded dispatcher and process
OperationStore. Drawing records a one-shot intent and never changes the effective
variable or hides the terminal before admission. A rejected request keeps the
accepted presentation and shows only its stable error code; it does not terminate
the host frame. Normal shutdown still directly detaches presentation before
stopping command admission, without submitting new teardown commands.

Core host regression sources check both initial visibility states, actual receipt/
operation identity, full-store rejection, owner/render/thread separation, unchanged
canonical configuration and stopped admission. GUI sources check that Escape
remains a one-shot pending request until the same Public write drains and its typed
value is projected. These are source-only assertions until the native and GUI
scopes run. Other overlay/debug/asset/renderer variable consumers and durable
configuration integration remain unfinished.

The actual Editor inventory now also binds the shared Developer session schemas
for `overlay.logs.visible`, `overlay.jobs.visible`, `overlay.scene.visible` and
`overlay.network.visible` (when its real query is supplied),
preserving their existing catalog IDs. The product catalog uses those same inert
factories. The process-store, explicit Scene and Network controls are admitted; the
other six panel sources remain
unavailable and receive no inert host-only replacement variables.

At activation, the root seeds the four supported requested preferences from its already
resolved launch/current default-panel selection. They are runtime session state,
not writes to launch defaults or a second configuration resolver. The existing
overlay feature/focus gate remains independent: a true panel request does not
create or enable the feature, acquire input or start producer/query work while
that feature is closed/suspended. After the common owner cutoff, the root reads
accepted Boolean values and projects them into the existing layout model without
changing normalized bounds. An accepted false value immediately drops the hidden
panel's projection references. Drawing never writes effective session state.

Panel close buttons and control-hub choices now emit at most four coalesced typed
`OverlayVisibilityIntent` values per draw. The root submits them through the same
Developer dispatcher/OperationStore authority and applies only accepted state.
Permission/admission rejection preserves the previous view, exposes only a stable
localized request-error code and cannot be cleared by an unrelated successful
source refresh. Unsupported source requests fail before operation creation.
The additive inert intent DTO stays under the existing Core header owner; fresh
Core/UI/Defaults consumers and the actual graphical host still require builds.

New regression sources cover permission/source denial, actual operation identity,
owner/render separation, unchanged canonical configuration, stopped admission,
and real log/operation query counts while panel preferences and the independent
feature gate change. English/Turkish request-rejection copy and locale regression
coverage are aligned. No C++/GUI/native artifact result is claimed by these sources.
The inactive Editor path prepares inert core metadata but creates no executor,
terminal input lease or graphical console window. Host shortcut eligibility
checks remain frame work and still require measurement.

Headless regression sources invoke these exact host capture/preparation/execution
adapters: payload-versus-host ownership, retained input lifetime, malformed/remote
denial, atomic source collision, canonical precedence, actual process-operation
identity, owner/render separation, wrong-thread denial, clear at the owner cutoff,
shutdown, partial admission cancellation through the original process store and
consumed-plan replay rejection. These tests are in the requested
core test target; no C++ test or graphical-host execution has yet completed.
This is Editor core startup/phase wiring. The full default catalog, remaining
graphical overlay sources, durable variable consumers, terminal/headless product launch,
approved script-file workflow and packaged Shipping/Diagnostics qualification remain outstanding.

The graphical Editor source now also composes a transient development terminal
from `RuntimeConsoleView`, after normal screen drawing and before editor modal
composition in the existing GUI frame. It uses the existing selected ImGui
renderer; no renderer/backend or gameplay `UiOverlayLifecycle` grant is selected
by console code. `--console` requests initial visibility. The registered
`editor.runtime_console.toggle` action defaults to Ctrl+Shift+F10 or
Command+Shift+F10 and remains editable through the existing Settings keymap.
Activation is lazy when the console is initially hidden. Existing modal/native
owners, focus loss, minimized output and pending native presentation block the
surface. It never installs a competing workspace context; the terminal's existing
central modal/text lease suppresses gameplay only while eligible and is released
before dispatcher/scheduler shutdown. Newer same-priority modal leases are not
overridden. The terminal reasserts text ownership only after its original context
is eligible again, preserving stale-composition suppression.

The new `HoroEditorRuntimeConsoleViewInternal` target stages only the two private
GUI headers for its actual host/gallery consumers. Its shared record primitive
borrows the original history span, uses body/monospace roles and token-backed
selection/hover/disabled/navigation states, clips long lines, and paints at most
128 visible rows while retaining the original scroll extent. Selection ordinals
and native activation identities are invalidated by `ConsoleHistory::Revision`,
so eviction between mouse-down and mouse-up cannot select a different record.
An already bottom-pinned list follows the tail, while deliberate upward scrolling
is preserved. No row pointers or parallel text
history survive a source mutation. Selected safe output can be inspected and
copied by explicit user action. All static surface/status/keymap copy is present
in both editor locale catalogs. Reverse search and Up/Down restore complete
non-redacted commands only, preserving an unsent draft. Explicit Tab completes
permission-filtered descriptor names at the end of the first token; discovery
does not run on every paint. Submission uses `ConsoleTerminal`, including the
current canonical immutable configuration capture and common permission gate.
Completion now also reads the same optional dispatcher variable inventory under
the current host principal. The actual Editor supplies its history-control
inventory; gallery regression sources check Developer completion/operation
identity and Public non-disclosure without executing denied input.

The existing `InputTextControl` has additive `submitOnEnter` and typed synchronous
navigation props so Enter/Tab/Up/Down are handled inside native editing rather
than bypassing the shared component. Navigation copies replacements before
mutating native storage, including aliased views; default callers retain their
existing edit behavior and styling. These trailing props and the history revision
field change C++ object layout: GUI/Core consumers must rebuild against the new
headers. Ownership remains HoroGui/Core respectively, with no parallel ABI shim.
The relevant public-header consumers and actual graphical host must be rebuilt.

Gallery regression sources exercise native Enter/completion/history-buffer
replacement, alias safety, a full 10000-row Unicode history with bounded painting,
narrow/selected/disabled layouts, modal preemption and the actual view-to-core
dispatcher path. Source locale tests cover every new message in English/Turkish.
`HoroEditorUiComponentsRenderTests`, `HoroEditorSettingsStoreTests`, GUI/Core
header consumers and `HoroEditor` have not yet run on these inputs. Neither the
gallery sources nor Python CI grouping success establish visual/runtime success.
Manual normal/hover/active/disabled/focused/popup, long localized text, IME,
platform/backend, packaged development/shipping and performance evidence remain
outstanding. The Editor source binds four real core commands plus `sys.metrics`, `game.scene_tree` and `game.inspect`; remaining standard owner
adapters, variable consumers and the six other overlay sources remain unfinished.

The metric command now uses one implementation shared with non-shipping Defaults
through the non-installed `HoroRuntimeConsoleTelemetryInternal` target. Its narrow
staged header is private to explicit composition; it is neither a Core dependency
nor an SDK export. The public Defaults binder retains exact compiled-profile
affinity, while the actual Editor bootstrap explicitly composes its Editor
registry. Metadata/ID live once in a separate Core object, preserving the ID
visible through `ConsoleProduct.h`. Registry creation/binding starts or queries
no telemetry. Only an accepted Developer owner-thread invocation reads the
existing process diagnostic snapshot; output remains limited to generation,
instrument IDs and availability, excluding names/payloads/history.

Shipping Defaults continue selecting the unavailable binder and do not link the
diagnostic implementation. Both new internal implementation and Editor adapter
targets are excluded from ALL unless an actual host/test/product dependency
requires them. Archive positive controls account for Core-owned metric metadata;
the final linked-image scan still requires all selected names and rejects denied
ones. All six profiles and affected GUI/headless/testing option combinations must
be reconfigured and built before this topology can be qualified. The SDK compiler
probe also requires the new private metric header to remain invisible.

The new actual-host regression source checks no implicit initialization,
permission denial before operation creation, owner/render handoff, the same
process operation result, live availability published by the real Telemetry
runtime, privacy-safe output, cancellation/shutdown and denied startup use.
It creates no metrics store or sampler implementation. Native execution remains
unrun; `sys.metrics` does not supply the still-missing chart histories/profiler
capture or other owner APIs.

The Scene bindings now share one non-installed `HoroRuntimeConsoleSceneInternal`
implementation. The existing public service-owning binder retains its original
signature, exact compiled-profile check and execution-time `ActiveScene()` query.
The Editor uses an explicit publication slot instead: its RuntimeHost keeps sole
ownership of the preview service, and its participant copies `ActiveScene()` into
the slot immediately before the owner command cutoff. Commands retain only the
existing weak-liveness/revision-checked view, never a service pointer or Scene
storage. Absent publication clears the previous view. Scene replacement,
structural mutation, destruction and foreign-thread access retain the existing
fail-closed view and full scene/index/generation contracts. No ServiceLocator
lookup, scene mutation, shadow ECS or new Scene lifetime authority is added.

The stable Scene IDs remain visible through `ConsoleProduct.h`; their declaration
and inert descriptor factory now live in `ConsoleBuiltins.h`, owned by the same
Core target. Existing callers need no source migration; rebuild Core, Defaults,
Scene and Editor consumers together. Scene metadata has its own Core object so
Shipping consumers do not pull it into the linked image unless they explicitly
reference it. Defaults archive positive controls reflect that ownership; linked
binary controls still require the selected profile's complete metadata. The SDK
negative compiler probe includes the private Scene binding header. These new
target edges require fresh configure/build evidence for all affected profiles.

The unchanged `ConsoleSceneInspectionLimits` value also lives in the Core-owned
header, included by the original public Scene binder header. This keeps the
shared handler independent of the facade's target/header boundary; it introduces
no reverse dependency from the helper to its own caller and no extra SDK header.

New actual-host regression sources exercise permission denial before admission,
real hierarchy output, absent publication, retained views after unique Scene
owner destruction, stopped dispatch and startup denial. Existing public Scene
binder tests continue covering replacement and exact generation references.
All native Scene/Editor/header/SDK regressions remain unrun on these inputs.

The same non-installed Scene target now also provides a bounded read-only overlay
projection over explicitly published `RuntimeSceneView` values. It captures only
up to 256 entity/parent identities from at most 4096 slots at four Hz, in fixed
storage. Hidden projections query no Scene, and null publication/hiding clears
visible rows immediately. Replacement and retirement retain the existing weak
liveness and full runtime/index/generation identity contracts. Unchanged refreshes
preserve the projection revision so native row selection can remain stable.
This is a derived presentation projection, not a Scene/metrics store or producer.

The projection admits explicit Developer grants only in Editor/Development.
Native regression sources cover denied profiles/permissions/limits, hidden state,
real bounded topology/parent identities, cadence, replacement, exact budget,
foreign owner rejection and storage destruction. Its staged header is excluded
by the SDK negative compiler probe. The actual Editor now publishes the same weak
view into the explicitly bound Scene panel after owner dispatch. The panel's
bounded numeric entity/parent text is formatted outside paint; the shared native
virtual list paints at most 128 rows. Missing capability keeps the source toggle
unregistered, while absent/retired/empty scenes have distinct localized states.
Hiding/suspension clears the derived projection, and unchanged revisions retain
selection. Clipboard copying uses only the selected full entity reference.

`overlay.scene.visible` retains ID 0xDB62000D and DevelopmentOnly/NextFrame policy,
now from a separately linked Core factory shared with the default catalog. It is
seeded at activation and projected after the common owner boundary; UI intents
never mutate it directly. The factory's separate object preserves Profile and
Shipping metadata absence. The Defaults archive verifier accounts for Core-owned
metadata while the final binary still requires all selected names. The private
view target gains only an explicit non-installed Scene helper dependency; public
Core remains Foundation-only. Reconfigure/rebuild Core/Defaults/UI/Scene and GUI
consumers together. Six new messages are present in both locales.

Native regression sources now cover accepted NextFrame Scene controls, real
Scene publication/window visibility, retirement/unbound capability, zero unrelated
store queries, localized copy and bounded shared identity-row painting. Native
execution and manual GUI/platform/performance checks remain unrun; this source
connection does not prove packaged-game composition or visual correctness.

The independent Core terminal's explicit `Open(true)` path previously chose a
FocusedGuiWidget that still excluded Gameplay in the central router. It now
registers the typed `InputContextPropagation::GameplayPassThrough` policy.
Input owns this policy, not the console: only that focused inspection widget is
ignored when testing Gameplay exclusion. Every other context, modal/native-dialog
barrier, window focus and consumed transition still applies. Text focus remains
exclusive; an existing Gameplay pointer capture is preserved until ordinary
release/cancellation. Modal or ordinary GUI preemption still revokes it.
The default terminal open and default Input registration remain exclusive.

This additive Input API keeps the original two-argument symbol and source calls;
the new three-argument overload is opt-in and rejects invalid kinds/policies
before any router mutation. `HoroInput` remains the sole owner of `Input.h`, with
no public dependency changes. Rebuild Input and new console consumers together;
external hosts wanting inspection must opt in explicitly and retain their normal
high-to-low consumption order. The normative input documents record the narrow
exception to priority exclusion. This is not a Debug-band Runtime UI authority or
proof of a packaged graphical host.

New Input regression sources cover simultaneous inspection/gameplay eligibility,
exclusive text and consumed keys, retained capture, ordinary GUI/editor exclusion,
modal/native-dialog cancellation, window focus loss and mutation-free invalid
policy rejection. The terminal regression exercises real `Open(true)` and modal
preemption. `HoroInputTests` and the standalone Input public-header consumer are
now required in the first requested native scope, separately matching
`^HoroInputTests::` with no-tests=error. These native checks and performance
measurements remain unrun; no overhead improvement is inferred from source.

Simultaneous inspection/gameplay eligibility also requires capture-aware mouse
delivery. The existing central capture now gates direct pointer button/motion
consumption and mouse-button/wheel action bindings. A peer stays eligible for its
keyboard/gamepad input while mouse bindings remain neutral and do not consume
the owner's transitions. Touch ownership is unchanged. Native regression sources
exercise both Gameplay and inspection as the capture owner, prove blocked peers
do not consume delivery, and verify release restores ordinary routing. These
checks remain unrun under the same requested Input native scope.

The same transient Editor surface now owns log and operation windows plus a
source-availability hub. `editor.runtime_overlays.toggle` defaults to
Ctrl+Shift+F11 or Command+Shift+F11 through the existing configurable keymap.
The host supplies its explicit Developer principal, actual structured-log query,
the executor's process OperationStore query and refresh time. No second process
store or diagnostic producer is created. The operations panel describes only
published process operations; it does not claim complete JobSystem visibility or
offer a cancellation path outside the common dispatcher. The eight unbound
sources remain disabled and visibly unavailable in the hub.

Terminal and panels share one central modal/text lease. Closing the terminal
preserves eligible panel focus; modal/native/focus/presentation suspension drops
cached source projections and stops their queries. Hidden panels likewise retain
no snapshots. Panel visibility preserves the layout model's normalized bounds;
native move/resize intent is contained to the current logical work area. Log
severity, exact/namespace category, literal text and repeat controls affect only
display, never producer policy. Visible stores refresh at most four Hz. Query
failures preserve valid rows and remain visible until a successful projection
refresh. Both source projection revisions scope selection/native row identities,
including filter changes, hiding and suspension, so stale activation cannot
select a replacement record. The shared list paints at most 128 visible rows.

English/Turkish catalogs add 43 aligned overlay/action messages. Additional
regression sources cover UTF-8 field limits, pinned operation lifetime, malformed
source retry, capacity validation, independent log refresh, layout hide/show and
actual Editor source-query counts under explicit time, blocking and Developer
permission gates. They have not run natively. The GUI scope still requires
`HoroEditor`, `HoroEditorUiComponentsRenderTests`, `HoroEditorSettingsStoreTests`
and the affected GUI/Core/UI public-header consumers, followed by actual visual
and interaction checks. Python grouping and locale structure checks alone do
not establish those results.

`ConsoleLaunchArguments::Parse` captures a bounded immutable argument slice and
produces Foundation `ConfigurationSourceMap` Invocation inputs plus bounded
startup command/script requests in original argument order. Interleaved exec
and script inputs use one typed ordered sequence rather than two lists that
could reorder startup effects. The host merges those inputs with its other
Invocation inputs, rejecting key collisions, then runs the existing Foundation
resolver with the captured project/user/environment/packaged/session sources.
`ResolveConsoleLaunch` requires those exact Invocation winners in the snapshot;
it rejects an unmerged or mismatched capture rather than applying an extra
precedence rule outside configuration. Missing keys, wrong types, secret
references and history limits outside 1–10000 fail without ambient defaults.

Specific `--dev-overlay=name` selections contribute exact typed panel settings;
without `--dev-overlays` they select only the explicitly named panels. With the
bulk flag, configured defaults and explicit panels combine through normal source
resolution. Scene requests remain DevelopmentOnly. Shipping local UI approval,
command/variable/startup ID allowlists and authentication remain separate host
capabilities; settings cannot manufacture these grants. Startup-exec policy
admits only Project/PackagedProfile sources, so User/Environment/Session/
Invocation values cannot expand that policy. Remote requests remain unsupported
and create no endpoint.

The launch projection carries its exact snapshot revision and owns its bounded
requests; a later configuration publication does not mutate it. The default
`console.history.limit` variable now bridges to the canonical history setting,
with NextFrame activation, instead of maintaining a second session integer.
Other runtime-only diagnostic toggles retain descriptor-owned session state.
In-branch catalog callers now supply the actual configuration service; the old
standalone launch parser was replaced by capture/resolve calls. No baseline
caller is affected. Regression sources cover real resolver precedence,
unmerged input rejection, immutable revisions, disallowed source escalation,
malformed schemas, the Scene/profile matrix and canonical variable publication.
Native execution and actual host wiring remain outstanding.
Toggle-chord binding, native text-input activation and runtime visibility/history
consumers remain host input/presentation integration work; the setting
contribution does not pretend to activate those features.

`PreparedConsoleStartup` now joins the ordered capture to the same executor.
Preparation validates every direct command, exact script approval and expanded
script line before any handler is invoked. Aggregate limits bound 128 requests,
256 commands, sixteen 64 KiB script captures and the configured command-byte
budget. Configuration revision, project identity/generation and cancellation
are fenced. The plan owns raw execution inputs privately and retains no file
service or borrowed authority callback after capture.

Submission is one owner-thread attempt, bound to the identical immutable
registry. It revalidates the current principal across the whole sequence before
dispatch, retains the frozen configuration for worker capture and never waits
for jobs. Its closed outcome distinguishes every-command-admitted from a
partial admission failure containing all accepted receipts and the original
error. It cannot silently retry or replay a consumed plan. This is not a
transaction over arbitrary command side effects: the host observes/cancels
accepted operations and aborts unpublished startup through normal lifecycle
control. It must close the executor before project replacement. Regression
sources cover ordered owner execution, late script failures, budgets, stale
configuration/project/grants, registry/thread mismatch, partial cancellation
handles, replay rejection and borrowed-callback retirement; native results
remain absent until the allocated slot is available.
An additional real JobSystem regression source checks that a startup worker
receives the prepared configuration revision after a newer publication, rather
than reading ambient/latest settings during execution. Its completion polling
is bounded test-only verification; the production submission path never waits.

## Linked profile artifact fixture

`HoroRuntimeConsoleArtifact` links the selected Defaults and Core archives without
Catch2 or test assertion strings. It emits the actual complete selected command
and variable catalog, executes real help/find/version/clear handlers through the
common dispatcher and checks the existing operation authority for queued clear.
For non-shipping catalogs it selects the real history descriptor from the catalog,
then exercises typed queued writing, NextFrame configuration publication and
bounded history consumption. It creates no GUI, native renderer, remote endpoint
or fabricated subsystem handlers. Its manifest explicitly distinguishes selected
catalog metadata from the four actually bound core commands. This is a test
fixture, not the packaged `horo-engine`/Editor product or qualification of the
remaining default command adapters.

`HoroRuntimeConsoleArtifactContract` runs
`scripts/qualify_console_artifact.py` with the configured profile, exact linked
executable and exact Defaults archive. It requires an exact profile/catalog and
core/history execution manifest, positive metadata controls and absence of every
known unavailable command/variable name in both files. Artifact scans stream
bounded chunks; SHA-256 identities are checked again after native execution.
The report records the actual file paths, hashes, observed time and fixture-only
scope. Missing files, encoding failures and observation timeouts are incomplete
(exit 2), while contradictory catalogs/metadata/execution fail (exit 1). Neither
can produce a passing report. Reports belong in the ignored build tree.

The fixture is an explicit dependency of `HoroRuntimeConsoleProductTests`, so
existing product-test target builds also build the inspected image. The CTest
contract is headless, serial and in the existing Windows group. After an allocated
native configure/build, run the exact contract separately so missing registration
cannot be hidden by matching another test:

```text
cmake --build build/skeleton --target HoroRuntimeConsoleProductTests --parallel 1
ctest --test-dir build/skeleton -R "^HoroRuntimeConsoleArtifactContract$" --no-tests=error --output-on-failure
```

Repeat with a freshly configured `HORO_RUNTIME_CONSOLE_PROFILE` for all six
profiles and preserve each profile/configuration JSON report. The local ignored
validation helper now requests this exact contract in its separately allocated
product/profile scopes. The first core-only build request remains unchanged.
No fixture build, execution, binary scan or six-profile result has run yet.

Source inspection also found that Server omitted screenshot from its catalog
while still compiling its metadata in the shared public descriptor object.
The screenshot declaration/definition are now omitted under the Server compile
definition. The new all-profile negative scan must verify actual binary/archive
absence; source guards and synthetic verifier tests do not establish that result.

## Validation ledger

- `git diff --check`: passed after the current source edits.
- `clang-format --dry-run --Werror` over all console headers, sources and native
  regression files: passed.
- `pytest -q --tb=short tests/python/test_ci_workflow_grouping.py`: 39 passed.
- `pytest -q tests/python/test_console_artifact.py`: 31 passed using synthetic
  bytes and mocked process observations. These validate fail-closed verifier
  behavior, not any actual C++ artifact. Combined with CI grouping: 70 passed.
- `py_compile` for the artifact verifier and ignored local validation helper:
  passed. Native artifact/profile CTest contracts remain unrun.
- Earlier native Windows Debug configure in `build/skeleton`: succeeded with cached
  dependencies, native MSVC and optional GUI/SDL/OpenGL disabled.
  The current expanded source/target topology has not been configured or built.
- Initial sandbox compiler probe: failed with MSVC C1902 (PDB manager mismatch).
- Retried native build: interrupted before console tests ran; **not passed**.
- Earlier attempts invoked `scripts/dev.py heavy_build_lock` and one worker,
  but did not prove the canonical physical lock handle. No serialization claim
  is made for those attempts. The local validation helper now requires psutil
  evidence of the canonical Windows lock handle before any configure/build/test.
  Admission uses the canonical physical lock nonblocking; a busy attempt starts
  no heavy child or waiter. Historical named grants are not an admission gate.
- Linux/macOS, packaged development UI, Shipping binary inspection and GPU smoke:
  not run. No cross-platform or GPU success is implied.
- Sonar IDE, CLI secrets and Codacy: not yet run on final edits.
- `analyze.py doctor --offline` on 2026-11-13: incomplete; the Sonar extension is
  available but IDE settings were unavailable in this invocation. CMake, Ninja,
  VS Code, Sonar CLI and Codacy CLI were detected from the initialized native
  environment. This establishes neither IDE authentication nor C++ execution.
- Cloud-enabled doctor: local prerequisites ready, Codacy cloud configuration
  drift check incomplete (`cloud read exit 2`; authentication must be reviewed).
  No source or results were uploaded and no quality/security settings changed.
- Native measurement source uses one million disabled refresh calls, zero source
  queries and a 250 ns/call budget. No measured latency or budget pass is claimed
  until the test actually runs. The source-only long-session checks also have
  no native result yet.

## Cross-workstream seams

The 2026-11-11 live acceptance audit covers parent #1812 and every child
#1842–#1848. Only policy is closed; no other child is qualified by the clean
checkpoint. The persistent Codex goal is active for all remaining implementation,
integration and validation. The prior idle checkpoint was caused by the heavy
resource allocation and missing owner/host capabilities, not completion.

The next bounded native batch is a fresh MSVC Debug configure, followed by
`HoroRuntimeConsoleTests`, `HoroInputTests` and the affected public-header consumers,
then their exact CTest subsets. Product/SDK, Scene and the six-profile
catalog/absence matrix need subsequent bounded batches. Every phase must acquire
the canonical physical Windows heavy lock nonblocking, verify its actual handle,
pass CPU/RAM guards and use one worker, releasing the lock between batches.
The 2026-11-13 configure attempts found the physical lock busy and started no
heavy child. Final worktree-aware quality analysis uses the same admission
discipline; IDE authentication/trust remains unproven.

NET-007.9 (#1169), NET-008.12 (#1183) and WST-010.8 (#1652) must bind to this
registry/policy seam and the common thread dispatcher. This ledger does not claim
those complete downstream responsibilities are integrated. In particular the current
Foundation JobSystem does not expose a general main-thread continuation API;
`JobSystem.h` offers worker submission and durable job handles. Any console
safe-point queue must remain explicit at the host's
`ApplyQueuedOwnerThreadCommands`/render boundary, rather than pretending a
synchronous worker wait is a handoff.

Those three downstream issue bodies were reread live after the local parent
integration. The existing `NetworkDebuggerApplication` capability supports real
bounded, privacy-safe immutable queries. A new non-installed application adapter
now composes optional `net.debug_status` through the same registry/Developer gate,
owner cutoff and OperationStore. Metadata/binding query nothing and start no
service. Only an accepted invocation reads the supplied application query and
formats closed provenance plus scalar identities/counts, within the command byte
budget; history records, endpoints, player data, credentials and native text are
not formatted. Malformed closed state/count fields fail without partial output.

The Editor explicitly elects that descriptor during inert preparation, then
supplies the actual same Net-pane `NetworkDebuggerService` before execution.
Its one cold shared owner replaces the previous stack owner so queued handlers
retain an explicit lifetime; the normal host still controls Begin/Publish/Detach.
Existing service registrations borrow that same object. No locator discovery or
second diagnostic service exists. Non-electing compositions keep the descriptor
absent; electing without an owner fails before startup command submission. The
four-command startup allowlist is unchanged. Core remains Foundation-only, the
application adapter is separately linked/EXCLUDE_FROM_ALL, and its staged header
is absent from the external Core SDK. The catalog/Core binary fixture explicitly
rejects this optional application command in every profile, independently of the
actual Editor's opt-in inventory.

Native regression sources cover inert binding, Public denial, safe-point routing,
the shared operation result, real detached/disabled/stale/live/provider states,
replacement before execution, detach/shutdown/cancellation, wrong owner, malformed
counts, retained owner release, missing host capability and startup denial.
Native execution remains unrun. This is read-only downstream inspection only:
NET-007.9 administration and NET-008.12 disconnect/traffic pause/lag/resync controls
remain incomplete, and capture pause is not treated as traffic control.
WST-010.8 streaming diagnostics is still unbound. No downstream issue is closed.

The optional network inspection descriptor is now explicitly `AuditLogged`.
Editor execution composes the existing sanitized Telemetry audit adapter into
the common executor. Without an auditor, executor creation fails; if the existing
Telemetry runtime/queue rejects audit admission, the authorized network request
fails before Query, OperationStore insertion or command dispatch. Public/parser
denials still occur before audit. Only canonical prepared/redacted text is emitted
under the existing isolated context; this neither starts Telemetry nor guarantees
durable sink delivery. Other descriptors retain their explicit flag policies.

`CreateConsoleTelemetryAudit` keeps its name, type and behavior; its declaration
and implementation move from Defaults to Core to avoid coupling actual host audit
composition to a separately selected default-catalog profile. The original
`ConsoleTelemetryBindings.h` includes the Core declaration, so existing includes
remain source-compatible with one contract/implementation. Core still depends
only on Foundation; no logger/store or public header owner is added. Reconfigure
and rebuild Core, Defaults, affected consumers and SDK together. The external Core
SDK fixture now creates/passes this real adapter without initializing Telemetry.
Native regressions cover absent/rejecting/accepting audit, canonical text, no-query
failure and the actual host's unavailable/available Telemetry branches. Existing
real-sink audit/privacy regressions still apply. All native checks remain unrun.

The private application adapter now also provides a four-Hz Network overlay
summary model over the exact same query owner. It retains only copied provenance,
publication revision, six bounded history counts and capture status/drop totals;
it owns no network records, producer, transport or mutable session state. Explicit
Developer graphical profiles are required; Server and Shipping cannot create this
overlay model. Construction and hidden refresh query nothing. Hide clears the
summary and cadence; resume immediately reads current owner state.

Command and overlay reads share one validated query function. Malformed closed
state/capacity/revision fields and thrown query failures become typed errors
without exception text. Failed overlay reads preserve the last valid scalar
snapshot and failure through throttled frames; only an actual successful read or
hide clears the failure. Unchanged summaries preserve the display revision.
Native regression sources exercise real detached/live/provider/count publication,
cadence, hide/resume, failed and throwing reads, wrong owner and denied profiles.
Its private staged header is excluded by the Core SDK compiler probe. Execution
remains unrun. The actual Editor now supplies the same retained debugger query
to its Network overlay. Without that capability the panel is unregistered and
its runtime variable is not composed. With it, the fourth registered panel
renders only the scalar summary: explicit diagnostic state/provider, exact source
identities/revision, retained record counts and capture flags/drop totals. Shared
read-only controls preserve normal typography and width wrapping; twenty-five
new English/Turkish messages keep publication freshness distinct from transport
or gameplay admission. No mutable networking/control object reaches the view.

`overlay.network.visible` retains ID 0xDB62000B and its original NonShipping,
Immediate session policy, from the same Core factory now used by Defaults and
actual host composition. UI intents use the common Developer queue and operation
authority, independently of feature/focus gates. Public or unbound requests fail
before query activation. The factory shares only the existing NonShipping
log/job metadata object; Server/Shipping catalogs still omit those panels. Archive
positive controls account for Core-owned text while final-image controls remain
mandatory. Four real sources (logs/jobs/Scene/Network) are now wired; the other
six remain unbound. This does not implement network traffic mutation or charts.

Native regression sources cover actual network-only window creation, zero reads
while blocked/hidden or feature-disabled, explicit four-Hz time, no unrelated
log/operation queries, missing capability, both locales, and common variable
admission/owner activation without starting a diagnostic source. GUI/header/SDK,
profile, binary, manual and performance checks remain unrun.

`rg` of the baseline's `src` and `include` trees finds no `MetricsStore` or
`ProfilerCaptureService` implementation. Telemetry exposes bounded health and
metric availability (now bound to `sys.metrics`), but not the architecture's metric-history query/capture
services. Logger exposes a global minimum severity, not the category-scoped
`log.level` owner required here. `SaveThumbnailCapture` owns save-slot-specific
capture and must not be reused as a general screenshot policy by fabricating save
identities. These remaining owner capabilities need real adapters/contracts before their
standard console commands or charts can be honestly qualified. No replacement
metric store, global category-level emulation or fake capture handler is added.
