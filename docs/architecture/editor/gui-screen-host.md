# GUI Screen Host Architecture

## Purpose

This document defines top-level screen navigation and lifetime inside
`HoroEditor`, including Welcome, Project Browser, project creation, onboarding,
and the Editor Workspace.

Screens are distinct from editor panels, tabs, modal workflows, and widget
popups.

## Core Decisions

- `HoroEditor` is one process and one GUI application.
- A screen owns the application content area for one route.
- Persistent shell chrome such as the status bar is owned by `GuiScreenHost`,
  not by the active screen. The screen receives the remaining bounded content
  region.
- Screen navigation is direct typed host coordination, not a data-bus command.
- Only the active screen receives interactive input.
- Screens receive narrow application capabilities and do not own business
  state already owned by application services.
- Editor Settings and Build & Release are editor modals, not top-level screens.
- Navigation with dirty or running work must pass an explicit leave decision.
- The startup project-selection experience is the `WelcomeScreen` route inside
  `GuiScreenHost`; it is not a separate launcher executable, module, lifecycle,
  or component set.
- Welcome and renderer-component repair remain reachable through a minimal
  product bootstrap presentation path that does not require a RenderApi backend.
  The editor workspace still requires one verified and available interactive
  renderer.

## Surface Taxonomy

| Surface | Placement | Lifetime |
|---|---|---|
| Screen | Replaces the application content route | Until navigation |
| Editor panel/tab | Inside persistent editor layout | Editor session |
| Modal workflow | Above active editor workspace | Temporary exclusive workflow |
| Widget popup | Attached to one surface | Short interaction |

Screens never appear in `EditorPanelHost`. Editor workflow modals never replace
the application route.

## Ownership

```text
HoroEditorApp
  +-- GuiScreenHost
  |     +-- EditorStatusBar
  |     +-- EditorStatusItemRegistry
  |     +-- WelcomeScreen
  |     +-- ProjectBrowserScreen
  |     +-- ProjectCreationScreen
  |     +-- ProjectLoadingScreen
  |     +-- EditorWorkspaceScreen
  |
  +-- Application Services
  +-- Engine Data Bus
```

`GuiScreenHost` owns the active screen object and navigation state.
`EditorWorkspaceScreen` composes `EditorLayer`, which owns panel and modal hosts.
The host-owned status bar persists across route replacement; see
[Editor Status Bar](./editor-status-bar.md).

## Screen Registration and Service Provisioning (Registry Architecture)

To avoid compiling every concrete screen header inside a single application
file or passing a monolithic context object with dozens of service references
through every constructor, `GuiScreenHost` utilizes explicit composition-time
service provisioning (`EditorServiceRegistry`) and factory registration
(`ScreenRegistry`).

```text
Composition Root (EditorLayer / App)
  |-- Populates -> EditorServiceRegistry (type-indexed typed service lookup)
  |-- Registers -> ScreenRegistry (map<GuiRouteKind, ScreenFactory>)
  +-- Passes to -> GuiScreenHost
```

```cpp
using ScreenFactory = std::function<std::unique_ptr<IGuiScreen>(
    const EditorServiceRegistry& services,
    const RouteParameters& parameters)>;
```

### Core Provisioning Rules

- **No Global Statics or Ambient Lookups:** Following [System Design](../foundation/system-design.md), screens and panels must not query process-global service locators (`ServiceLoader::Get()`) or mutate global registries during static initialization. All registrations happen explicitly at composition time.
- **Narrow Dependency Injection:** Each screen factory pulls from `EditorServiceRegistry` only the exact subset of services it requires. For example, `WelcomeScreen` pulls `LocalizationService&` and `WelcomeScreenController&`, while `ProjectCreationScreen` pulls `ProjectCreationService&` and `JobSystem&`.
- **Decoupled Route Construction:** `GuiScreenHost::Navigate()` does not contain hard-coded `switch/case` constructor blocks for every route. It queries `ScreenRegistry::CreateScreen(route.kind, m_services, route.parameters)`. If an extension package or future workspace workflow registers a new `GuiRouteKind` or custom panel, `GuiScreenHost` instantiates it transparently without modifying core navigation code or including external headers.

## Startup Flow

`WelcomeScreen` is the first active route of `HoroEditor`. It is not
special-cased outside the route system.

Startup order:

1. `HoroEditor` creates the platform adapter and minimal bootstrap presentation
   required for Welcome/component repair.
2. The renderer component service discovers and validates machine-local
   component records without loading a project renderer.
3. Composition constructs `GuiScreenHost`, registers every borrowed screen
   service, and then starts the host on `GuiRouteKind::Welcome`.
4. `WelcomeScreen` requests recent projects and renderer preflight snapshots from
   application/project-model services.
5. Create/open actions resolve renderer requirements before requesting project
   loading.
6. After one selected interactive renderer is `Available`, the composition root
   creates its presentation-capable window attachment, RenderApi backend, and
   editor GUI resources.
7. `GuiScreenHost` validates route payloads and leave guards before entering
   `ProjectLoading` or `EditorWorkspace`.

The welcome screen reads recent projects through project-model services. It does
not parse `<user-state>/horo/recent_projects.json` directly and does not persist
arbitrary thumbnail paths.

The bootstrap presentation path is not an engine renderer, cannot enter the
workspace or draw a project viewport, and must not register as OpenGL, Metal,
Vulkan, or Null. It exists only to keep the single HoroEditor application capable
of renderer install/repair when no interactive component is available.

## Recent Project Renderer Resolution

Recent-project cards include the requested renderer's local preflight state. A
card click does not navigate to `ProjectLoading` while its renderer requirement
is unresolved.

For a project requesting missing OpenGL while compatible Metal is available, the
Welcome screen owns a blocking resolution dialog with exactly these project-level
choices:

```text
[Install OpenGL]
[Use Metal for This Project]
[Cancel]
```

The product GUI does not offer a duplicate session-only `Open Once with Metal`
choice. `Use Metal for This Project` is an explicit persistent project-setting
change, capability-validated and committed transactionally through an
application use case. The Welcome screen does not edit project files directly.

If no compatible alternative is available, replacement is omitted and the
dialog offers install, repair, diagnostics, offline-package, and cancel actions
as applicable. Cancelling keeps the Welcome route active and leaves project state
unchanged.

Detailed component states, mutation ordering, cache invalidation, and required
tests are defined by
[Renderer Distribution And Availability](../runtime/renderer-distribution-and-availability.md#recent-project-renderer-preflight).

## Route Model

```cpp
enum class GuiRouteKind {
    Welcome,
    ProjectBrowser,
    ProjectCreation,
    ProjectLoading,
    EditorWorkspace
};
```

`RouteParameters` is a closed variant of route-specific parameter structs, not a
string map:

```cpp
struct WelcomeRouteParameters {};
struct ProjectBrowserRouteParameters {};

struct ProjectCreationRouteParameters {
    std::optional<TemplateId> initialTemplate;
};

struct ProjectLoadingRouteParameters {
    std::string projectRoot;
    std::string projectName;
};

struct EditorWorkspaceRouteParameters {
    ProjectSessionCandidateId session;
    std::optional<ProjectPath> initialScene;
};

using RouteParameters =
    std::variant<WelcomeRouteParameters,
                 ProjectBrowserRouteParameters,
                 ProjectCreationRouteParameters,
                 ProjectLoadingRouteParameters,
                 EditorWorkspaceRouteParameters>;

struct GuiRoute {
    GuiRouteKind kind;
    RouteParameters parameters;
};
```

The host validates that the parameter alternative matches `GuiRouteKind` and
that IDs and paths are structurally valid before constructing a destination
screen. A workspace route requires a non-zero session candidate ID; raw project
paths are accepted only by `ProjectLoading`. The workspace reserves the candidate
during `OnEnter()`, commits its single consumption after controller/panel setup,
and releases the reservation if entry fails. Invalid parameters return a typed
navigation error and leave the active route unchanged.

## Screen Contract

```cpp
enum class LeaveRequirementKind {
    DirtyDocument,
    UnsavedDraft,
    RunningOperation,
    RecoveryDecision,
    NativeDialogCompletion
};

enum class LeaveAction {
    Save,
    Discard,
    CancelOperation,
    KeepRunning,
    Wait,
    Stay
};

struct LeaveRequirement {
    LeaveRequirementKind kind;
    LeaveSubjectId subject;
    LeaveRequirementRevision revision;
    std::vector<LeaveAction> allowedActions;
};

enum class LeaveDisposition {
    Allow,
    Deny,
    RequireResolution
};

struct LeaveDecision {
    LeaveDisposition disposition;
    std::optional<LeaveRequirement> requirement;
};

struct ApplicationCloseTarget {};

struct LeaveTarget {
    std::variant<GuiRoute, ApplicationCloseTarget> value;
};

struct LeaveResolution {
    LeaveSubjectId subject;
    LeaveRequirementRevision revision;
    LeaveAction action;
};

enum class LeaveErrorCode {
    StaleSubject,
    ActionNotAllowed,
    OperationFailed
};

struct LeaveError {
    LeaveErrorCode code;
    std::vector<Diagnostic> diagnostics;
};

class GuiScreen {
public:
    virtual ScreenId Id() const = 0;
    virtual Result<void> OnEnter(const GuiRoute&) = 0;
    virtual void OnUpdate(float dt) = 0;
    virtual void Draw(const GuiContentRegion& contentRegion) = 0;
    virtual void CollectActivePanelIds(
        std::vector<std::string_view>& output) const;
    virtual LeaveDecision CanLeave(const LeaveTarget& target) const = 0;
    virtual Result<LeaveDecision>
    ResolveLeave(const LeaveTarget& target,
                 const LeaveResolution& resolution) = 0;
    virtual void OnLeave() = 0;
};
```

An `Allow` decision permits the transition. `Deny` keeps the current route
active without presentation. `RequireResolution` identifies one typed
requirement and the allowed actions, such as save, discard, cancel operation,
keep durable work running, wait for a native dialog, or stay on the current
screen. The requirement contains stable identities, not owning pointers or
presentation callbacks.

`OnEnter()` succeeds before the route becomes active. Destination construction
is staged while the current screen remains active. Before destination entry,
the old screen receives `OnLeave()` so process-wide panel/status registrations
cannot be torn down after the candidate has claimed them. If destination entry
fails, the host re-enters the old route; rollback failure is fatal to the GUI
session. Candidate-owned partial resources remain protected by RAII.

If construction fails, the current screen remains untouched. If `OnEnter()`
fails after old-screen teardown, the candidate is destroyed and the old screen
is re-entered with the unchanged active route. A rollback failure invalidates
the GUI session and requests shutdown rather than continuing with ambiguous
shared ownership.

`OnLeave()` executes once after leave permission and before destruction.
Navigation requested from `Draw()` is queued and committed only after that
screen callback returns; an active screen is never destroyed on its own stack.

## Navigation

```cpp
class GuiScreenHost {
public:
    Result<void, NavigationError> Navigate(GuiRoute destination);
    Result<void, NavigationError> RequestCloseApplication();
    const GuiRoute& ActiveRoute() const;
};

enum class NavigationErrorCode {
    InvalidRouteParameters,
    Busy,
    LeaveDenied,
    Cancelled,
    LeaveResolutionLimitExceeded,
    DestinationConstructionFailed,
    DestinationEntryFailed
};

struct NavigationError {
    NavigationErrorCode code;
    std::vector<Diagnostic> diagnostics;
};
```

`LeaveSubjectId` is a validated opaque identity that correlates the requirement
with the document, draft, job, recovery flow, or native dialog that produced it.
It is stable only while that blocker exists within one screen instance; it is
not a process-global or persistent identity. The host verifies that the displayed
subject, revision, and action still match its current pending requirement before
calling the owning screen, which performs its own domain-state validation.

The current host keeps one pending requirement/target pair and rejects a second
navigation request as busy until it commits or is cancelled.
`LeaveRequirementRevision` increases when the state or allowed actions of the
same blocker change.

`RequireResolution` must contain one requirement; `Allow` and `Deny` do not.
`CanLeave()` is a non-mutating query. After presenting an allowed action, the
host calls `ResolveLeave()`. The screen verifies the subject and action, commits
the corresponding save, discard, cancellation, detach, or recovery operation
through its owning service, and returns `Allow`, `Deny`, or a new requirement.
Failed resolution returns the foundation `Error` carried by `Result` and keeps
the current route active.

A resolution chain is allowed because one blocker may reveal another, such as
saving a dirty document before resolving a running operation. Each successful
`ResolveLeave()` must return `Allow`, `Deny`, or a requirement with a new subject
or higher requirement revision. Repeating the same unresolved requirement
without progress is an error. The host also enforces a small configured maximum
resolution count per navigation attempt; exceeding it cancels the attempt with
a structured diagnostic instead of presenting an unbounded loop.

Leave denial and user cancellation are typed non-success outcomes and do not
masquerade as entry failures. Callers branch on `NavigationErrorCode`, never on
diagnostic text.

Navigation requests commit at a GUI frame boundary. Reentrant route replacement
during `Draw()` is not allowed. `Navigate()` called during update, draw, or a
screen callback validates and records one deferred request; it never replaces
the active screen on the current call stack. A second request while navigation
or leave resolution is pending returns `NavigationErrorCode::Busy`.

At the frame boundary, the host resolves leave policy, prepares the destination,
and commits one route change. Failed validation, denied leave, failed entry, and
cancelled confirmation clear the pending request without changing the active
route.

A successful route commit is ordered:

1. validate that the pending request and active route are still current
2. obtain final leave permission
3. construct the candidate without claiming shared panel/status registrations
4. call `OnLeave()` on the previous screen
5. call candidate `OnEnter()` and claim shared registrations
6. if entry fails, destroy the candidate and re-enter the previous route
7. replace the active route and establish destination focus
8. destroy the previous screen
9. publish `GuiRouteChangedEvent`

No observer sees the destination route before its entry succeeds. Candidate
entry remains fallible after old-screen teardown because current workspace
registrations are process-wide. The host therefore treats re-entering the
previous route as the rollback operation. Rollback failure invalidates the GUI
session and requests shutdown.

Navigation is not published as a command on `EngineDataBus` or
`EditorDataBus`. After commit, the host publishes one bounded route-changed
notification for observers such as telemetry or window title presentation.

```cpp
struct GuiRouteChangedEvent {
    GuiRouteKind previousKind;
    GuiRouteKind currentKind;
    GuiRouteRevision previousRevision;
    GuiRouteRevision currentRevision;
};
```

`GuiRouteChangedEvent` is a process-level presentation/lifecycle notification
defined with the GUI host event types and published on `EngineDataBus`. It does
not carry route parameters, project paths, or command semantics.

Route identity is the route kind plus canonical typed parameters. Navigating to
the identical active route is a successful no-op: it does not run leave guards,
replace the screen, increment route revision, or publish an event. Navigating
between routes with the same kind but different parameters is a real
transition. In that case `previousKind == currentKind`, while the route
revisions differ so observers can distinguish the committed route instances
without receiving sensitive parameters.

`GuiRouteRevision` is a host-owned monotonic value incremented once for every
committed route instance. It is runtime correlation metadata, not persisted
navigation state.

## Leave Guards

A screen may require a decision before leaving because of:

- dirty scene documents
- project creation drafts
- running operations tied to the screen lifetime
- unresolved recovery flow
- active native dialog

The screen returns a typed leave requirement. The host presents the required
confirmation through the appropriate modal/dialog mechanism and retries
navigation with the resolved decision.

The navigation host does not guess whether work should be saved, cancelled, or
kept running.

`RequestCloseApplication()` uses the same `CanLeave()` and leave-resolution path
with `ApplicationCloseTarget`. Native window-close events call this method
rather than terminating the process directly. The application exits only after
the active screen allows leave and required save, discard, cancellation, detach,
or recovery decisions commit successfully.

An active native dialog produces `NativeDialogCompletion`. Ordinary navigation
and application close remain pending until the dialog returns a typed result or
cancellation; the host does not destroy or programmatically dismiss a platform
dialog that may still call back into the active screen. The request is then
revalidated before leave resolution resumes. Process-fatal or emergency
termination follows the platform emergency policy and does not attempt ordinary
interactive navigation.

## Editor Workspace Screen

Entering the editor workspace:

1. validate and open the project through application services
2. create the editor workspace controller
3. restore workspace state
4. construct `EditorLayer`, panel host, tabs, and modal host
5. activate the route after required initialization succeeds

Leaving:

1. resolve open modal and dirty-document policies
2. stop or detach durable jobs according to their task-group, cancellation, and
   operation-handle contracts
3. persist workspace state
4. destroy GUI surfaces and subscriptions
5. close the editor session
6. close or retain the project according to destination requirements

Process startup uses an explicit lifecycle boundary. Constructing
`GuiScreenHost` creates its registries but does not invoke a screen factory.
Composition registers every borrowed screen dependency and then calls
`Start(initialRoute)` exactly once. No screen may observe a partially populated
service registry.

Process shutdown uses the same explicit lifecycle boundary. `GuiScreenHost::Shutdown()`
is idempotent: it calls the active screen's `OnLeave()` at most once, destroys
the screen, and clears borrowed service registrations while their owners still
exist. The input router and all route-specific services are registered before
`Start()` constructs the initial screen, so screens can use their complete
dependency set during their entire lifecycle. The application first cancels input capture and resolves or force-detaches the
modal stack, then calls `Shutdown()` before composition-owned viewport, ImGui, renderer,
window, input, and project services begin destruction; the host destructor delegates
to `Shutdown()` only as a safety net.

The screen does not duplicate project-open or scene-save business logic.
Job ownership, cancellation, detach behavior, and shutdown joins follow
[Concurrency And Job System](../foundation/concurrency-and-jobs.md).

## Screen State

Screen-local presentation state includes:

- search text
- selected project-browser item
- local wizard step
- transient validation presentation
- focus target

Durable project and user state is committed through owning services.
Navigation state is not used as an untyped global store.

## Input And Focus

The active screen owns the base GUI interaction scope. Inactive screens are not
drawn interactively and do not receive input.

When the editor route has an open modal, modal scope supersedes workspace scope
without changing the active route.

Focus restoration occurs within the active screen after child modal or native
dialog closure. Route changes establish a new initial focus target.

## Data Bus Participation

Screens may subscribe to `EngineDataBus` for process-level lifecycle
notifications relevant to presentation. The editor workspace obtains its
session-local bus from `EditorWorkspaceController`.

Screens do not publish application commands. They call typed use cases and
observe committed results.

## Failure Presentation

Entry failures are mapped from typed errors:

- invalid or missing project remains in Project Browser with diagnostics
- recoverable editor initialization failure offers retry or return
- fatal renderer/window failure enters the host fatal presentation path

Logs are supporting evidence and are not parsed to choose navigation.

## Testing

Required tests cover:

- screen enter/leave exactly once
- failed destination entry preserving current route
- failed or partial destination entry releases candidate resources and does not
  call `OnLeave()` on the active screen
- deferred navigation requested during draw
- a second navigation request returns `Busy` while transition or leave
  resolution is pending
- invalid route parameter alternatives and semantic validation failures preserve
  the active route
- dirty-document leave guard
- invalid leave subject or disallowed action cannot resolve a leave requirement
- stale subject revisions and subjects from another screen instance or
  navigation attempt are rejected
- chained leave requirements must make progress and stop at the configured
  resolution limit
- running-job leave policy
- application close passes through the same leave guards as route navigation
- active native dialog defers navigation and application close until completion
- stale deferred navigation is revalidated after native dialog completion
- editor workspace construction and destruction order
- inactive screen input exclusion
- modal scope without route replacement
- one route-changed notification is published after commit and none is
  published for rejected or failed navigation
- identical-route navigation is a no-op, while same-kind navigation with
  different parameters commits with distinct route revisions
- focus initialization and restoration
- project operations shared with CLI and MCP

## Related Documents

- [Editor Panel Host](./editor-panel-host.md): persistent editor workspace layout
  and tab lifecycle.
- [Editor Modal Host](./editor-modal-host.md)
- [Editor Document Model](./editor-document-model.md)
- [Input Architecture](../runtime/input-architecture.md)
- [Project Model](./project-model.md)
- [Platform Abstraction](../foundation/platform-abstraction.md)
- [Concurrency And Job System](../foundation/concurrency-and-jobs.md)

## Native Mock Removal Migration

The native UI preview gallery and simulated build, test, release, and publish
workflows are removed from the editor. Design references remain in Mock Studio.
`HoroEditorApp` starts a typed production route through `GuiScreenHost::Start`;
`--ui-preview`, `StartUiPreview`, and `OpenUiPreview` have no replacement.
The corresponding `EditorMenuAction` preview values are removed. Build menu
entries remain disabled until real application capabilities are implemented.

`AssetImportModal` no longer exposes the gallery-only `Catalog`,
`PresentReadOnlySnapshot`, read-only presentation, canvas inset, initial advanced
expansion, or initial scroll APIs. Their only native caller was the removed
preview modal. Production callers continue to use the import operation and its
snapshot. Menu, host lifecycle, import presentation, and localization tests
cover the remaining production paths; simulated workflow tests are removed.

The import modal constructor now groups its optional borrowed capabilities in
`AssetImportModalServices` instead of five positional pointer arguments. The
composition root and lifecycle/presentation tests use named members; this keeps
the eight-argument constructor below the quality limit and preserves ownership
and default behavior. External callers should pass the same pointers in that
value; no service lookup or registration occurs inside it. The constructor reads
this aggregate through a const reference and copies its pointers, so a temporary
aggregate is valid while the pointed-to services must outlive the modal.
