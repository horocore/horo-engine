# Editor Panel and Tab Architecture

## Purpose

This document defines the layout model for the Horo Editor workspace: how panels,
tabs, docks, and toolbars are composed into the main editing screen. It is a
companion to [Editor Data Bus](./editor-data-bus.md), which defines how tabs
communicate.

The editor workspace shown in the screenshots is structured as:

```text
+----------------------------------------------------------+
|  System menu bar (File / Edit / Assets / GameObject / Component / Window / Build / Help)             |
+----------------------------------------------------------+
|  Toolbar (Select / Move / Rotate / Scale / Play / Scene) |
+----------+-------------------------------+---------------+
|          |                               |               |
|  Left    |                               |   Right       |
|  Dock    |        Viewport               |   Dock        |
|          |                               |               |
|  -       |                               |  Properties   |
|  Hierar- |                               |               |
|  chy     |                               |               |
|  -       |                               |               |
|  Project |                               |               |
|          |                               |               |
+----------+-------------------------------+---------------+
|  Bottom Dock (Assets / Console / MCP / Performance)      |
+----------------------------------------------------------+
|  Status bar                                              |
+----------------------------------------------------------+
```

Each visible region has a clear architectural owner.

## Top-Level Owners

```text
EditorLayer
    |
    +-- EditorWorkspaceController owns editor-session state and command routing
    |       |
    |       +-- SceneDocument
    |       +-- EditorSelectionModel
    |       +-- EditorHistory
    |       +-- EditorDataBus
    |
    +-- EditorMenuBar            system menu bar (File, Edit, Assets, GameObject, Component, Window, Build, Help)
    +-- EditorToolbar            top icon bar (select, transform, play, scene)
    +-- EditorPanelHost          owns layout tree and tab containers
    |       |
    |       +-- RootSplit (vertical)
    |               |
    |               +-- MainRowSplit (horizontal)
    |               |       |
    |               |       +-- LeftSplit (vertical)
    |               |       |       +-- HierarchyTabStack
    |               |       |       +-- ProjectTabStack
    |               |       |
    |               |       +-- ContentSplit (horizontal)
    |               |               +-- ViewportPanel
    |               |               +-- PropertiesTabStack
    |               |
    |               +-- BottomTabStack
    |                       +-- AssetsTab
    |                       +-- ConsoleTab
    |                       +-- McpTab
    |                       +-- PerformanceTab
    |                       +-- RenderInspectorTab (registered, closed by default)
    |                       +-- NavigationTab (registered, closed by default)
    |                       +-- GameplayAiTab (registered, closed by default)
    |
    +-- EditorModalHost          exclusive modal workflows above the workspace
```

`EditorLayer` is the GUI composition root. It creates the workspace controller,
panel host, modal host, menu bar, toolbar, and viewport render
integration, then forwards frame lifecycle calls.

The persistent status bar is shell chrome owned by `GuiScreenHost`, not by
`EditorLayer` or `EditorPanelHost`. Panels may publish bounded status
contributions through the host registry, including an
`OnlyWhenPanelActive` visibility policy. See
[Editor Status Bar](./editor-status-bar.md).

`EditorLayer` is also the GUI coordinator for top-level presentation actions. It
consumes typed results from the menu bar and toolbar, opens GUI-only surfaces
through `EditorModalHost` or `EditorPanelHost`, and forwards editor-session
operations to `EditorWorkspaceController`. It does not implement domain
operations itself. `EditorWorkspaceController` is not a GUI coordinator and
does not depend on menu, toolbar, panel, or modal types.

```cpp
class EditorLayer {
private:
    void Handle(const EditorMenuResult& result);
    void Handle(const EditorToolbarResult& result);
};
```

These handlers perform routing only. Each result remains a narrow type owned by
its source surface; the editor does not introduce one central variant containing
every possible GUI or domain action.

`EditorWorkspaceController` owns:

- the active `SceneDocument`
- the authoritative `EditorSelectionModel`
- the command dispatcher and `EditorHistory`
- the `EditorDataBus`
- editor-session services
- borrowed application use-case and process-service interfaces
- editor-session lifecycle and workspace state coordination

`EditorPanelHost` owns layout and tab lifetime. Individual tabs own only their
presentation state and drawing.

The bottom Console tab is a presentation consumer of the foundation
`IStructuredLogQuery` capability. It does not parse terminal text or own a second
logger. Terminal, persistent JSONL, and Console delivery fan out from the same
accepted structured record.

The optional dockable `RenderInspectorTab` follows
[ADR-049](../../adr/049-render-graph-and-resource-inspector-ui.md). It consumes an
application-owned immutable renderer-inspection query service and owns only
selection, filters, graph pan/zoom and other presentation state. It does not reach
into the render frontend/backend, resource registry, memory ledger or metrics
stores. It is closed by default; restoring/opening/closing it cannot arm
instrumentation, and a hidden tab performs no capture, paging, layout or polling.

`EditorModalHost` is a separate overlay owner. Settings, Build & Release, import,
and confirmation workflows are not tabs or layout nodes. While a modal is open,
the panel host remains mounted and rendered but receives no user interaction.
Tabs and panels remain subscribed to `EditorDataBus`, so presentation changes
published by the modal or an owning settings service can update the background
workspace while its interaction is blocked.
See [Editor Modal Host](./editor-modal-host.md).

## Panel Host

The implemented workspace uses `WorkspacePanelHost` and a validated
`WorkspaceLayout` tree containing `SplitNode`, `TabStackNode`, and `PanelNode`
values. `WorkspacePanelRegistry` owns `IWorkspacePanel` instances and injects a
scoped `PanelContext`; it is not a callback-only registry. `EditorWorkspaceView`
is the ImGui presentation adapter for that model and emits typed workspace
commands for tab activation, panel docking, activity-bar reordering, and seam
resizing. `WorkspaceDockArea` remains placement metadata and a command boundary;
it is not the authoritative layout representation.

Layout persistence validates the node tree before activation and falls back to
the versioned default layout on structural failure. Splitter and panel drag
gestures acquire central input-router capture, so modal opening, focus loss, or
owner destruction cancels them before any dock mutation is committed.

`EditorPanelHost` is a thin layout and tab-lifecycle manager. It knows:

- the workspace layout tree
- which tabs live in each tab stack
- which tab is active in each tab stack
- split direction, ratios, minimum sizes, and collapsed state
- tab registration, attachment, detachment, and destruction order

It does **not** know:

- what a SceneDocument contains
- what the current selection means
- how to render a gizmo
- how to import an asset
- how to execute an MCP command
- how to execute editor or application commands

### Layout Model

The layout is a tree rather than a fixed list of left/right/bottom regions. This
allows the current workspace and future workspaces to compose nested split views,
tab stacks, and dedicated panels without changing the host interface.

```cpp
struct LayoutNode;

struct SplitNode {
    LayoutNodeId id;
    SplitAxis axis;
    float ratio = 0.5f;
    float firstMinimumSize = 160.0f;
    float secondMinimumSize = 160.0f;
    std::unique_ptr<LayoutNode> first;
    std::unique_ptr<LayoutNode> second;
};

struct TabStackNode {
    LayoutNodeId id;
    std::vector<TabId> tabs;
    std::optional<TabId> activeTab;
    bool collapsed = false;
};

struct PanelNode {
    LayoutNodeId id;
    PanelId panel;
};

struct LayoutNode {
    std::variant<SplitNode, TabStackNode, PanelNode> value;
};
```

The viewport is a `PanelNode`, not a special hard-coded dock. The default editor
layout places it in the center of the tree, but alternative workspace layouts
may place other dedicated panels there.

### Host Interface

```cpp
struct TabPlacement {
    LayoutNodeId stack;
    std::optional<size_t> index; // No index appends to the stack.
};

struct TabRegistration {
    TabPlacement fallbackPlacement;
    bool openByDefault = true;
};

struct PanelRegistration {
    LayoutNodeId fallbackNode;
};

class EditorPanelHost {
public:
    Result<void, PanelHostError>
    RegisterTab(std::unique_ptr<EditorTab> tab,
                const TabRegistration& registration);
    Result<void, PanelHostError>
    RegisterPanel(std::unique_ptr<EditorPanel> panel,
                  const PanelRegistration& registration);
    Result<void, PanelHostError> UnregisterTab(TabId tabId);
    Result<void, PanelHostError> UnregisterPanel(PanelId panelId);
    Result<void, PanelHostError> OpenTab(TabId tabId, TabPlacement placement);
    Result<void, PanelHostError> MoveTab(TabId tabId, TabPlacement placement);
    Result<void, PanelHostError> CloseTab(TabId tabId);
    Result<void, PanelHostError> SetActiveTab(LayoutNodeId stackId, TabId tabId);

    EditorTab* FindTab(TabId tabId);
    EditorPanel* FindPanel(PanelId panelId);

    LayoutLoadReport LoadLayout(const WorkspaceLayout& layout);
    WorkspaceLayout SaveLayout() const;

    void OnUpdate(float dt);
    void Draw();
};

enum class PanelHostErrorCode {
    DuplicateSurfaceId,
    UnknownSurface,
    UnknownLayoutNode,
    InvalidPlacement,
    TabNotInStack,
    SurfaceStillReferenced
};

struct PanelHostError {
    PanelHostErrorCode code;
    std::optional<TabId> tab;
    std::optional<PanelId> panel;
    std::optional<LayoutNodeId> node;
};

enum class LayoutLoadIssueCode {
    UnsupportedSchema,
    InvalidNode,
    MissingSurface,
    GeometryClamped
};

struct LayoutLoadIssue {
    LayoutLoadIssueCode code;
    std::optional<TabId> tab;
    std::optional<PanelId> panel;
    std::optional<LayoutNodeId> node;
};

struct LayoutLoadReport {
    bool usedDefaultLayout = false;
    std::vector<LayoutLoadIssue> issues;
};
```

`TabPlacement` identifies the target stack and optional insertion index.
Placement is validated before any layout mutation. An out-of-range explicit
index, a non-stack target, or a target that cannot accept the tab returns
`InvalidPlacement`.

`PanelRegistration::fallbackNode` names a dedicated `PanelNode` declared by the
versioned default layout or by the provider's transactionally registered layout
extension. Registering a panel does not create arbitrary split nodes implicitly.

Errors and load issues include stable codes plus relevant surface or node IDs
for diagnostics. Callers never parse an error message to determine recovery.

`TabId`, `PanelId`, and `LayoutNodeId` are validated value types backed by stable
serialized strings. Duplicate surface IDs, unknown node kinds, invalid node
references, and attempts to activate a tab outside its stack return
`PanelHostError`; they do not silently modify the layout. Dedicated panels
implement the same attach, update, draw, and detach lifecycle as tabs but do not
expose a tab label or participate in a `TabStackNode`.

Loaded splitter ratios and dimensions are clamped against the current window
size, UI scale, and node minimum sizes. Structurally invalid or incompatible
layouts fall back to the versioned default layout while preserving recoverable
surface state. References to temporarily unavailable optional surfaces are
reported in `LayoutLoadReport` and skipped without making the remaining layout
invalid.

Runtime splitter input uses screen-space seam rectangles and explicit pointer
capture rather than transparent ImGui overlay windows. This keeps hit testing
independent of platform window z-order and preserves an active resize while the
pointer moves outside the narrow seam. Activity-panel drag/drop and modal popup
ownership suppress new splitter capture. Conversely, an active splitter owns the
primary pointer before dock rendering and suppresses competing panel-header and
activity-item drag sources; overlapping hit regions must never start both operations.
Panel rearrangement additionally requires the primary press to originate in that
panel's header. Merely crossing a panel header while another typed drag payload is
active must not replace the payload or reveal workspace split targets.

Registration makes a surface available and attaches its lifecycle; placement is
a separate layout operation. `TabRegistration` and `PanelRegistration` provide
fallback placement metadata for new workspaces, schema migration, and newly
available extensions. Fallback placement does not reopen a tab recorded as
closed in persisted workspace state. `OpenTab()` places an already registered
tab, `MoveTab()` changes its stack and index, and `CloseTab()` removes it from
the visible layout without unregistering or destroying it. `UnregisterTab()` and
`UnregisterPanel()` detach and destroy the registered surface after removing all
layout references.

### Panel and Tab Service Provisioning (Registry Architecture)

To support modular addition of workspace panels (such as `HierarchyPanel`,
`InspectorPanel`, `ConsoleTab`, or third-party package dashboards) without
passing a monolithic context object or modifying `EditorWorkspaceController`
for each new surface, panel construction uses composition-time service
provisioning (`EditorServiceRegistry`).

```cpp
using TabFactory = std::function<std::unique_ptr<EditorTab>(
    const EditorServiceRegistry& services)>;
using PanelFactory = std::function<std::unique_ptr<EditorPanel>(
    const EditorServiceRegistry& services)>;
```

- **Scoped Dependency Injection:** When `EditorLayer` activates the workspace, `EditorWorkspaceController` populates the session `EditorServiceRegistry` with narrow capabilities (`EditorDataBus&`, `SceneDocument&`, `EditorSelectionModel&`).
- **Dynamic Surface Instantiation:** Each panel or tab descriptor registers its factory function with `PanelRegistry`. `EditorPanelHost` constructs registered surfaces by supplying only the required services, avoiding tight coupling between panels and preventing header bloat at the workspace composition root.

## Surface Contracts

Tabs implement `EditorTab`; dedicated panels implement `EditorPanel`:

```cpp
enum class EditorSurfaceVisibility {
    Visible,
    Hidden
};

class EditorTab {
public:
    virtual ~EditorTab() = default;

    virtual Horo::Editor::TabId Id() const = 0;
    virtual std::string_view TabLabel() const = 0;

    virtual void OnAttach(EditorTabContext& ctx) {}
    virtual void OnDetach() {}
    virtual void OnVisibilityChanged(EditorSurfaceVisibility visibility) {}
    virtual void OnUpdate(float dt, EditorSurfaceVisibility visibility) {}
    virtual void Draw() = 0;

    virtual TabWorkspaceState SaveWorkspaceState() const { return {}; }
    virtual void LoadWorkspaceState(const TabWorkspaceState& state) {}
};

struct EditorPanelContext {
    EditorDataBus& events;
    SceneDocument& document;
    EditorSelectionModel& selection;
    EditorViewportModel& viewport;
    EditorCommandDispatcher& commands;
};

class EditorPanel {
public:
    virtual ~EditorPanel() = default;

    virtual Horo::Editor::PanelId Id() const = 0;

    virtual void OnAttach(EditorPanelContext& ctx) {}
    virtual void OnDetach() {}
    virtual void OnVisibilityChanged(EditorSurfaceVisibility visibility) {}
    virtual void OnUpdate(float dt, EditorSurfaceVisibility visibility) {}
    virtual void Draw() = 0;
};
```

Tabs receive:

- a stable `EditorTabContext` containing references to editor models, the
  command dispatcher, and `EditorDataBus`
- feature-specific application capabilities through the concrete tab factory or
  constructor

Dedicated panels receive an `EditorPanelContext` governed by the same rules:
cohesive editor-session queries, models, commands, and notifications belong in
the stable context; feature-specific application or rendering capabilities are
provided through the concrete panel factory or constructor.

Tabs request state changes through typed models, editor commands, or application
use cases. They subscribe to notifications they need to react to.

Lifecycle guarantees:

- `OnAttach()` is called exactly once after successful surface registration.
- A failed registration never attaches or retains the surface.
- `OnDetach()` is called exactly once before unregistering or destroying an
  attached surface.
- Tabs and dedicated panels are detached in reverse registration order before
  the context, editor bus, models, or services are destroyed.
- The host supplies visibility state and reports visibility transitions for
  tabs and dedicated panels.
- After `OnAttach()`, the host delivers the initial visibility before the first
  `OnUpdate()`. Later visibility transitions are delivered before the next
  update or draw for that surface.
- `OnVisibilityChanged()` runs once for each visibility transition and is used
  to acquire or release visibility-scoped presentation resources.
  `OnUpdate()` receives the current visibility every update so ordinary
  per-frame work can remain explicit without querying host internals.
- A tab is visible only when it is active in a non-collapsed stack with visible
  layout bounds. A dedicated panel is visible when its node and all ancestors
  are non-collapsed and have visible bounds. Modal interaction blocking does not
  by itself change surface visibility.
- Hidden surfaces remain attached and receive notifications for invalidation,
  but they do not perform expensive refreshes, thumbnail generation, filesystem
  scans, render-target work, or metric queries until visible or explicitly
  scheduled as a bounded background job.
- Subscriptions use move-only RAII tokens so tab destruction cannot leave
  dangling handlers.

## Extension Surfaces

Built-in and plugin-provided tabs and panels register through explicit editor
extension points and follow the same ID, capability, lifecycle, input, and
data-bus rules. Factories provide fallback placement metadata; they do not
mutate the active layout directly.

The panel host is the IDE surface integration point. It must be capable of
hosting first-party tabs and external add-on tabs through the same registration
path. A package can add a new tab, dedicated panel, diagnostics view, asset tool,
profiler view, or project utility without requiring a new `EditorLayer` member
or a hard-coded branch in the workspace layout code.

Extension tab descriptors declare:

- stable contribution ID and provider module ID;
- display label, icon token, and localization key;
- fallback tab stack or dedicated panel node;
- whether the surface opens by default in new workspaces;
- requested editor-session events and process-event bridge imports;
- required capabilities such as scene queries, selection queries, asset queries,
  log queries, metric queries, or specific editor commands;
- workspace-state schema version and byte limits.

The host validates the descriptor and resolves capabilities. First-party product
code may create an internal surface factory and call the ordinary `RegisterTab()`
or `RegisterPanel()` path. An external package instead supplies the validated
typed UI schema/state/action contract from
[ADR-056](../../adr/056-external-editor-ui-boundary.md); a host-owned adapter
registers that projection through the same layout path. External code does not
implement `IWorkspacePanel` or receive the whole `EditorLayer`, raw ImGui dock
IDs, renderer backend objects, C++ component objects or process-global services.

```mermaid
sequenceDiagram
    participant EM as Extension Manager
    participant Registry as Editor Surface Registry
    participant Host as EditorPanelHost
    participant Tab as Host UI Session
    participant Bus as EditorDataBus

    EM->>Registry: Commit editor.tab descriptor
    Registry->>Host: Register schema + fallback placement
    Host->>Tab: Attach copied schema/state + scoped actions
    Tab->>Bus: Subscribe through host-owned tokens
    Bus-->>Tab: Notify selected allowlisted events
    Tab->>Tab: Invalidate local presentation cache
    Tab->>Host: Save bounded workspace state
    Host->>Tab: Close admission and detach before provider shutdown
```

Requested event subscriptions are advisory for validation, diagnostics, and
permission review. The host-owned UI session performs subscription through the
typed `EditorDataBus` API at attach time; external module/controller code never
receives the C++ bus. Event payloads remain invalidation hints, and the session
queries the authoritative model or store through approved capabilities.

Extensions may contribute new editor-local event types only under their stable
module prefix. Those events are visible only to the active editor session unless
an explicit process-level bridge is documented and approved.

Persisted layouts may reference a surface whose provider is not currently
available. The host reports the missing ID in `LayoutLoadReport`, skips it
safely, and retains its serialized placement as unresolved layout metadata.
`EditorWorkspaceController` preserves the unavailable surface's bounded opaque
workspace state so both can be restored if the provider becomes available in a
later session.

Plugin disable, update, and removal normally take effect after restart. During
shutdown, plugin surfaces detach before plugin shutdown callbacks. If a future
plugin API explicitly supports safe runtime unload, all contributed surfaces
must be detached and destroyed, subscriptions and registered callbacks must be
removed, extension registrations must be withdrawn transactionally,
plugin-owned asynchronous work must be cancelled or joined, and queued callbacks
into plugin code must be drained before the plugin code is unloaded. Failure to
prove any invariant rejects runtime unload and leaves restart as the only
supported path.

## Mapping to the UI

### Left Dock

Visible in the screenshot as the tall panel on the left, split into two
sections:

- **Hierarchy tab**: shows the scene object tree (`Room`, `floor_000`, `wall_north`,
  etc.) and the search filter.
  - Owner: `HierarchyTab`
  - Writes selection through: `EditorSelectionModel`
  - Subscribes to: selection and document notifications
  - Asset drops use the row center as a child target and the row edges as a
    same-parent target. The empty region remains the explicit scene-root target.

- **Project tab**: shows the project file tree (`assets`, `shaders`, `src`,
  `CMakeLists.txt`).
  - Owner: `ProjectTab`
  - Writes asset selection through: `EditorSelectionModel`
  - Opens files through: typed editor/application commands
  - Subscribes to: `ProjectOpenedEvent`

## Embedded Source Editor

The editor workspace includes source-editing surfaces for project scripts,
native behavior templates, generated-code previews, shader source, package
manifests, and diagnostics-linked text files. These surfaces are editor panels or
modal-owned editors; they are not separate top-level screens and they are not a
replacement for an external IDE.

Initial source editor integration uses **Zep** as the preferred embedded editor
widget, wrapped behind Horo-owned source-editor interfaces. Zep was selected over
`goossens/ImGuiColorTextEdit` for the first code-oriented editor surface because
it is more aligned with engine IDE workflows: embeddable core, ImGui renderer,
tabs and splits, modal/modeless editing modes, key mapping, search, markers,
REPL/live-coding orientation, and broader adoption. The research snapshot used
for this decision was collected on `2026-07-05T11:45:01Z`:

| Candidate | Stars | License | Recent activity | Fit |
|---|---:|---|---|---|
| `Rezonality/zep` | 1030 | MIT | pushed 2026-05-23 | Preferred code editor foundation |
| `goossens/ImGuiColorTextEdit` | 218 | MIT | pushed 2026-06-29 | Good lightweight text/diff widget, fallback or specialized read-only/editor-lite use |

GitHub stars are a weak popularity signal, not an architecture requirement. The
selection is based on embedding model, feature surface, license, maintenance,
and expected extension path. If Zep integration proves too invasive, the fallback
is `goossens/ImGuiColorTextEdit` for a narrower text editor while Horo-owned IDE
services remain unchanged.

Zep and any future text editor library remain private to the editor source-editor
adapter target. Public Horo headers expose Horo value types only:

```cpp
struct SourceEditorDocumentId;
struct SourceEditorCursor;
struct SourceEditorSelection;
struct SourceDiagnosticMarker;

class ISourceEditorSurface {
public:
    virtual void SetText(std::string_view text) = 0;
    virtual SourceEditSnapshot CaptureSnapshot() const = 0;
    virtual void ApplyDiagnostics(std::span<const SourceDiagnosticMarker>) = 0;
};
```

AI-assisted editing, code completion, IntelliSense, refactoring, and agent mode
are not implemented inside Zep. The source editor widget is the presentation and
text interaction surface only. Intelligence is provided by Horo-owned services:

- `SourceDocumentService` owns document identity, snapshots, dirty state,
  encoding, and save/reload coordination.
- `LanguageServiceClient` owns LSP-style completion, hover, go-to-definition,
  rename, diagnostics, semantic tokens, and formatting integration.
- `AiCodeAssistService` owns prompt construction, context selection, tool
  approvals, patch preview, and agent-mode sessions.
- `SourceEditorController` coordinates editor commands, selections, diagnostics,
  and inline assistant results without exposing third-party editor internals.

Completion and AI results are applied as explicit editor commands with previews,
undo entries, and diagnostics. Agent mode may propose multi-file changes, but it
does not mutate source files directly from the widget callback. All writes go
through project document/application services and the same approval policy used
by MCP or other automation surfaces.

Required source editor constraints:

- no third-party editor type in public Horo APIs;
- no direct filesystem writes from the widget;
- diagnostics use typed source ranges, not parsed console strings;
- completion/AI providers are cancellable and tied to document revision;
- stale completion or agent results are rejected when the source snapshot changes;
- large files use bounded memory and may open in read-only or external-editor
  mode until a streaming editor contract exists;
- editor shortcuts participate in the workspace shortcut conflict policy.

## Embedded Node Editors

Node-based authoring surfaces appear inside workspace panels or dedicated editor
tabs. They are used for shader/material graphs, behavior graphs, state machines,
and future audio/procedural graphs. The node widget is presentation only; graph
asset schemas, validation, compilation, and runtime execution are owned by the
relevant subsystem.

Initial node editor integration uses **imgui-node-editor** for production graph
surfaces and keeps **imnodes** as the lightweight fallback/prototype option. The
research snapshot used for this decision was collected on `2026-07-05T11:45:01Z`:

| Candidate | Stars | License | Recent activity | Fit |
|---|---:|---|---|---|
| `thedmd/imgui-node-editor` | 4447 | MIT | pushed 2026-03-29 | Preferred production graph editor foundation |
| `Nelarius/imnodes` | 2462 | MIT | pushed 2026-05-13 | Lightweight fallback/prototype/simple graph surfaces |

`imgui-node-editor` has the stronger adoption signal and is designed as a richer
node editor layer on Dear ImGui. `imnodes` is smaller and explicitly immediate
mode, which is useful for prototypes or simple internal tools, but Horo's graph
surfaces need stable persisted node/link IDs, navigation, selection, context
menus, graph validation overlays, and complex interaction policies.

Graph editor library types remain private to Horo adapter targets. Public graph
surfaces exchange Horo graph documents:

```cpp
struct GraphDocumentId;
struct GraphNodeId;
struct GraphPinId;
struct GraphLinkId;
struct GraphDiagnostic;

class INodeGraphSurface {
public:
    virtual void SetGraphSnapshot(const GraphViewSnapshot&) = 0;
    virtual std::vector<GraphEditCommand> DrainUserCommands() = 0;
    virtual void ApplyDiagnostics(std::span<const GraphDiagnostic>) = 0;
};
```

The widget may produce user-intent commands such as create node, connect pins,
move selection, rename node, or open context menu. It does not validate gameplay,
shader, material, audio, or procedural semantics itself. Subsystem validators own
type checking, cycle rules, asset references, feature requirements, code/shader
generation, and runtime compatibility.

Procedural audio graph surfaces additionally follow
[ADR-071](../../adr/071-procedural-audio-graph-ownership.md). The Editor owns the
document controller, node placement, selection, undo/redo, localized presentation
and preview controls; AudioModel owns schema/edit validation and AudioCook owns the
compiled generator. Preview uses the ordinary AudioFrontend/voice/mixer path, and
stale compile or diagnostic results are rejected by document revision.

## VFX Effect Document Surface

[ADR-129](../../adr/129-vfx-editor-document-live-preview-and-module-authoring.md)
places every editable effect asset in a persistent `VfxEffectDocument` tab. The panel
host owns route, placement, focus, visibility and surface lifecycle; document services
own source revision, commands/history, dirty/save/recovery/conflict and derived compile
or preview state. Opening the same asset focuses its existing writable tab. Create,
import, templates, Save As and confirmation remain transient modal routes that open no
tab until their application transaction succeeds.

The baseline stack surface composes Horo module/property/curve/output controls. A
future graph surface uses the shared node-editor adapter but keeps graph source commands
and layout distinct. Both emit typed commands and display compiler diagnostics; neither
validates or executes particle semantics during draw. If graph UI is unavailable, the
host preserves the source or offers explicit read-only inspection rather than silently
flattening it into the stack.

The effect viewport is an isolated preview surface. It compiles an immutable document
revision and consumes normal `VfxWorld`/Renderer output through a generation-fenced
preview capability. Its camera, grid, seed, clock, fixture, selected module and runtime
handles are presentation/preview state. Closing the document cancels derived work and
retires resources; a hidden tab does not continue preview work without a separately
admitted bounded background operation.

Effect decal outputs use the same document tab. Scene-placed decal projection remains
owned by SceneDocument. Box/OrientedBox manipulation shares viewport/gizmo capture,
updates a transient overlay during drag and sends exactly one typed command to the
owning document on commit. It never writes renderer state or maintains decal-specific
history/persistence.

## Viewport Panel

The viewport panel owns rendering of the active scene camera or game camera in
the center and does not participate in tab stacking.

- Owner: `ViewportPanel`
- Responsibilities:
  - render the scene through the render target
  - draw gizmos and selection highlights
  - handle viewport navigation input
  - route picking through `EditorSelectionModel`
  - route drag-drop mutations through editor commands
- Writes viewport state through: `EditorViewportModel`
- Subscribes to: selection and document notifications

`ViewportPanel` owns viewport presentation state and input mapping. Renderer
backend resources and render-target lifetime belong to the editor render
integration and renderer services. The panel receives a typed render-target
handle or view and never stores backend-specific renderer objects directly.

The current navigation mapping is:

- right mouse drag: fly-camera look;
- right mouse capture plus `W/A/S/D` and `Q/E`: forward/side/vertical movement;
- `Shift` during fly capture: accelerated movement;
- middle mouse drag: camera-relative pan;
- `Alt` plus left mouse drag: orbit around the current target;
- mouse wheel: camera-relative dolly.

The top-left projection control switches between `Perspective · Shaded` and
`Orthographic · Shaded` through an `EditorViewportModel` command. Switching
projection preserves approximate apparent scale at the orbit target and remains
editor-session state; it does not dirty the authored scene or mutate a Camera
component. Perspective pan derives world units per pixel from target distance,
vertical field of view, and viewport height. Orthographic pan derives it from
orthographic height and viewport height. Wheel input scales target distance or
orthographic height exponentially.

The routed `editor.viewport.focus_selected` action (`F` by default) is eligible
only while the viewport is hovered or focused. It frames bounds from the selected
instance in the current render snapshot. Missing selection or renderable bounds
is a no-op. Projection changes and focus update viewport revision only and never
create document history.

Move-gizmo drags update only an explicit viewport preview snapshot while the
pointer is captured. Releasing the pointer submits one transform command through
`EditorHistory`; cancelling restores the committed document snapshot. Preview
frames increment viewport revision but do not increment document revision,
publish document-change events, dirty the scene, or create undo entries.
Linear move and scale axes whose direction is nearly parallel to the camera view
use a colored end-on ring around the hub. Their perspective projection has no
stable screen-space arrow direction. Dragging this ring uses vertical pointer
movement and the projected scale of a perpendicular axis. Rotation rings retain
their projected plane behavior.
All three rotation rings start from one world-space radius computed from the
camera depth and projection. Their projected points are then fitted to the same
screen-space major radius; the ellipse shape still reflects each axis plane.
The move gizmo draws screen-sized, filled arrowheads with shaded shafts and a
central hub. Pointer hit testing covers the shaft and tip while leaving the hub
clear of axis capture.

Content Browser assets that support scene instantiation follow the same preview
boundary. While the primary pointer remains held over the viewport, the editor
adds one tinted, non-pickable mesh instance to the render snapshot and updates
its placement from the pointer ray. `Escape` removes that transient instance;
releasing the pointer removes it and submits exactly one undoable document
command. A viewport drop always creates a root object; hierarchy parenting is
chosen only by an explicit hierarchy drop target.

The active viewport renderer reports its typed clip-depth convention through
`IEditorViewportRenderer`. `ViewportPanel` forwards that value to gizmo
projection, pointer-ray construction, and picking commands. These consumers do
not inspect or branch on a concrete renderer identifier.

The viewport grid is world-space editor presentation, not a screen-space ImGui
decoration and not authored scene content. `ViewportPanel` forwards the committed
grid-visibility setting through the editor-private viewport renderer contract.
Shared allocation-free geometry generation places the grid on the world XZ plane,
snaps it to world cells around the camera target, and selects a minor spacing from
the `1/2/5 × 10ⁿ` sequence. Perspective spacing derives from target distance and
vertical field of view; orthographic spacing derives from orthographic height.
Concrete adapters draw the same geometry before scene meshes with depth testing
enabled and depth writes disabled, allowing scene geometry to cover the grid
without making the grid a document object or picking target.

Viewport implementation responsibilities are split by lifetime and authority:

- `ViewportPanel` composes the render-target surface and presentation overlays;
- `EditorViewportGridGeometry` owns bounded adaptive world-grid generation
  without per-frame heap allocation or backend-native types;
- `ViewportInteractionController` arbitrates mutually exclusive gizmo and
  navigation sessions and emits typed workspace commands;
- `ViewportInteractionCapture` owns routed pointer-capture tokens and guarantees
  deterministic cancellation before panel detachment;
- `ViewportNavigationController` maps input snapshots to camera, focus, and
  picking commands without mutating viewport state;
- `TransformGizmoGeometry` owns screen projection, handle drawing, and hit
  testing;
- `TransformGizmoController` owns only pointer-derived transient drag state and
  emits preview, commit, or cancellation commands;
- `TransformGizmoMath` validates drag inputs and evaluates local/world
  Move/Rotate/Scale results without ImGui, document mutation, or silent
  decomposition fallback.

These components remain editor-presentation adapters. `EditorViewportModel`,
`EditorSelectionModel`, `SceneDocument`, and `EditorHistory` remain the
authorities. Geometry drawing and input mapping must not commit scene state
directly. Singular parents, non-finite input, and unrepresentable transform
updates terminate the transient preview through a typed error; they must not
substitute identity axes or commit a partial result.

Terrain/Foliage viewport tools follow
[ADR-142](../../adr/142-terrain-foliage-document-tool-undo-and-preview-ownership.md).
Their controllers own routed pointer capture, brush/spline visualization and an
ephemeral revision-fenced stroke accumulator only. The foliage palette owns active type
and filters as workspace presentation state. Tools submit typed edit intent to the
active asset-rooted document; they cannot write canonical samples/placements, dirty
state, history, cooked tiles, TerrainRuntime or renderer/physics/navigation state.

The viewport may render a disposable interaction overlay or an immutable isolated
preview-session result. Modal capture, tool/document switch, revision change, capacity
denial, panel detachment or shutdown cancels the interaction without a document edit.
Pointer release submits at most one bounded operation whose exact affected tile/patch
closure and before/after history are owned by the document executor.

### Right Dock

Visible as the Properties panel.

- **Properties tab**: shows transform, components, and asset fields for the
  current selection.
  - Owner: `PropertiesTab`
  - Reads selection from: `EditorSelectionModel`
  - Subscribes to: selection and document notifications
  - Executes: undoable editor commands when values are edited

The Inspector projects the complete ordered object selection and a separate
primary object identity. A transform axis is presented as mixed when selected
objects disagree on that axis. Dragging a mixed axis applies the primary draft's
delta relative to each selected object's authored axis value, preserving their
spacing and per-object offsets. Editing a uniform axis keeps absolute assignment
semantics. Every untouched translation, rotation, and scale axis retains each
object's authored value. The presentation reducer emits one typed batch preview
during drag and one atomic document command on commit. The document executor
validates the complete update set before mutation, drops unchanged entries, and
records at most one undo transaction. Escape, selection or revision changes,
and competing workspace commands clear the whole transient preview set without
mutating the document.

### Bottom Dock

Visible as the Workspace panel with the following tabs:

- **Assets tab**: thumbnail browser for project assets.
  - Owner: `AssetsTab`
  - Writes asset selection through: `EditorSelectionModel`
  - Subscribes to: bridged `AssetImportedEvent`

- **Console tab**: engine log output.
  - Owner: `ConsoleTab`
  - Subscribes to: bridged `ConsoleLogEvent`
  - The process composition root owns `ObservabilityRuntime`, which owns the
    optional bounded `StructuredLogStore`. The tab receives a narrow log-query
    capability through its factory or constructor.
  - The logging system appends structured records to `StructuredLogStore`.
    `StructuredLogEventAdapter` publishes a small revision/count
    `ConsoleLogEvent` on `EngineDataBus`, and the editor bridge republishes it
    on `EditorDataBus`. The tab queries the store for the required range.
  - Clearing or filtering the tab changes presentation state; it does not delete
    persistent log files or change logger configuration implicitly.

- **Build Output tab**: bounded typed build and cook diagnostics.
  - Owner: `GlobalDockBuildOutputPane`
  - Queries: `IBuildOutputQuery`; it never parses log categories for status.
  - Projects owned revision snapshots only. Diagnostic severity and terminal result
    remain separate; result-less warnings and errors are not rewritten into operation
    outcomes by the panel.
  - Build session and operation identities correlate rows with their producer-owned
    work. Hiding, closing, or detaching the tab stops observation only and never
    cancels a producer or changes store retention.
  - Optional absolute source locations carry line and column metadata. The
    workspace validates that a target is a regular non-symlink beneath the
    active project before dispatching platform navigation. The validated request
    preserves line and column through the platform capability; a host without a
    configured source-editor adapter may fall back to revealing the file.
  - Empty projections and bounded-retention drops are presented explicitly; a
    user must not mistake a filtered or truncated snapshot for producer silence.

- **Operations tab**: user-facing import, cook, build, and validation progress.
  - Owner: `GlobalDockOperationsPane`
  - Queries: `IOperationQuery`
  - Executes: cooperative cancellation through `IOperationControl`
  - The panel owns only search, column visibility, and presentation filters;
    `OperationStore` owns bounded lifecycle snapshots and terminal retention.
  - Empty projections and dropped terminal-history counts are visible presentation
    states, not inferred from log text.
  - Navigation bake rows are projections of the application-owned
    `NavigationBakeService` defined by
    [ADR-106](../../adr/106-navigation-bake-ownership-transaction-and-cache.md).
    A navigation panel/menu may submit or request cancellation, but panel closure,
    docking and workspace restoration never own the operation, builder, staging,
    publication lock or artifact lifetime.

- **Navigation tab**: navigation authoring readiness, bake actions/diagnostics and
  bounded runtime inspection.
  - Owner: `NavigationTab`, registered closed by default.
  - Queries: document/definition readiness, artifact/currentness,
    `IOperationQuery` and immutable `INavigationInspectionQuery` snapshots.
  - Executes: typed document commands, ADR-106 bake submission/control and
    separately authorized runtime-debug commands through narrow capabilities.
  - Owns only filters, selection, focused `OperationId`, overlay interest and
    other bounded presentation state. Hide/close stops polling and releases
    presentation leases; it never cancels a bake, ends play, changes runtime
    lifetime or accesses provider/native state.
  - Provider/extension detail uses validated host-rendered schema under ADR-110
    and ADR-056, not arbitrary plugin ImGui.

- **Gameplay AI tab**: selected-agent blackboard, active plan/node/task, perception,
  EQS and budget inspection.
  - Owner: `GameplayAiTab`, registered closed by default.
  - Queries: bounded immutable `IGameplayAiInspectionQuery` pages carrying world,
    agent, plan/schema and tick generations.
  - Executes: document-open actions and separately authorized typed runtime-debug
    commands through safe-point capabilities.
  - Owns only presentation selection, filters and paging. Hide/close stops polling
    and releases leases; it never cancels a task, query, simulation or PIE session.
  - Provider detail uses ADR-111/ADR-056 host-rendered schema, never runtime/native
    pointers or arbitrary plugin ImGui.

- **MCP tab**: MCP command history and activity.
  - Owner: `McpTab`
  - Subscribes to: bridged `McpCommandExecutedEvent`

- **Performance tab**: CPU, memory, frame, and subsystem metrics.
  - Owner: `PerformanceTab`
  - Subscribes to: coalesced bridged `MetricsChangedEvent` and profiler
    capture-state notifications
  - The process composition root owns `MetricsStore` and
    `ProfilerCaptureService` through `ObservabilityRuntime`. The tab receives
    narrow metric-query and capture-operation capabilities through its factory
    or constructor.
  - Queries: bounded ranges from `MetricsStore`
  - Executes: typed start/stop capture operations through
    `ProfilerCaptureService`
  - Does not receive one data-bus event per metric sample or profiler zone

### Toolbar

Top icon bar with select, move, rotate, scale, play, pause, scene dropdown,
help, and settings.

- Owner: `EditorToolbar`
- Returns typed interaction results to the `EditorLayer` GUI coordinator
- The coordinator opens GUI-only workflows through `EditorModalHost`
- Domain operations are routed to `EditorWorkspaceController` or application
  use cases directly
- Does not own document state

### Menu Bar

The system menu bar at the top of the window (File, Edit, Assets, GameObject, Component, Window, Build, Help on macOS).

- Owner: `EditorMenuBar`
- Returns typed interaction results to the `EditorLayer` GUI coordinator
- The coordinator opens Settings and Build & Release through `EditorModalHost`
- Save, undo, and other domain operations are routed to typed commands or
  application use cases
- On macOS this maps to the native system menu bar; on Windows and Linux it is
  rendered inside the application window
- Does not own document state

`EditorMenuBar` and `EditorToolbar` route actions through the same interaction
scope policy as panels. When `EditorModalHost` owns interaction,
workspace-mutating menu and toolbar actions are disabled or rejected before
command dispatch.

### Status Bar

Persistent shell-owned bottom line rendered by `GuiScreenHost`.

- Owner: `GuiScreenHost`
- Workspace bridge: `EditorWorkspaceScreen`
- Inputs: bounded `EditorStatusItemContent` snapshots in the host registry
- Does not call panel/plugin providers or query document models during draw

## Data Flow Example: Selecting an Object

1. User clicks `wall_north` in `HierarchyTab` (left panel).
2. `HierarchyTab` calls `EditorSelectionModel::SetObjects(...)`.
3. `EditorSelectionModel` atomically updates the authoritative selection.
4. The selection model publishes `SelectionChangedEvent` on `EditorDataBus`.
5. `PropertiesTab` queries the selection model and refreshes inspected fields.
6. `ViewportPanel` queries the selection model and syncs the gizmo.
7. `EditorWorkspaceScreen` publishes the committed selection snapshot to the
   host status registry; the next status draw presents `Sel: 1`.

No tab knows the others exist. The event producer does not know which panel
will consume the event, and tabs attaching later can query the current selection
without replaying old events.

## Data Flow Example: Editing a Transform

1. User drags a value in `PropertiesTab`.
2. `PropertiesTab` creates a `SetTransformCommand` and submits it through
   `EditorTabContext::commands`.
3. `EditorHistory` executes the command inside an editor transaction.
4. After the transaction commits, `SceneDocument` increments its revision and
   publishes one `SceneDocumentChangedEvent` through the editor bus.
5. `HierarchyTab` receives it and refreshes dirty indicators.
6. `EditorWorkspaceScreen` publishes the committed dirty-state snapshot and the
   shell status bar presents `Unsaved`.
7. The viewport renders the updated object on the next frame through the
   existing runtime binding.

GUI interactions, MCP tools, CLI commands, and scripted batch operations invoke
the same application use cases. Editor-local undoable edits additionally pass
through `EditorHistory`.

## Layout Persistence

The panel host persists only the layout slice of workspace state:

- the versioned layout tree
- tab placement and active tab per tab stack
- splitter ratios and collapsed state
- explicitly closed tab IDs

The host exposes `LoadLayout` and `SaveLayout` to read and write this state:

```cpp
struct WorkspaceLayout {
    uint32_t schemaVersion = 1;
    LayoutNode root;
    std::vector<TabId> closedTabs;
};

struct TabWorkspaceState {
    uint32_t schemaVersion = 1;
    SerializedObject payload;
};
```

`SerializedObject` is a JSON-compatible object value with deterministic
serialization. It contains data only: bounded strings, numbers, booleans,
arrays, nested objects, and null. It cannot contain executable callbacks, raw
pointers, borrowed handles, or capability-bearing objects.

Workspace loading enforces configured per-surface and aggregate byte limits,
nesting-depth limits, collection-size limits, and string-length limits before a
payload reaches a tab. Oversized or malformed payloads produce structured
diagnostics and are ignored. Plugin state for an unavailable provider remains
opaque but is retained only within the same limits.

`EditorWorkspaceController` owns the complete workspace document and asks the
panel host to serialize or restore its layout slice. Tabs serialize their
project-scoped presentation state into per-tab workspace entries. Personal,
cross-project preferences remain in `EditorUserSettings`.

The persistence boundary is:

- `TabWorkspaceState` stores project-scoped or workspace-instance presentation,
  such as the current Console filter, expanded Project tree nodes, selected
  Performance graphs, and per-project column layout.
- `EditorUserSettings` stores cross-project defaults and behavior preferences,
  such as the default Console severity filter, preferred graph units, and
  whether a tool tab opens automatically in new workspaces.
- domain, document, asset, release, log, metric, and profiler data remain in
  their authoritative models and stores; tab state may contain only stable
  references and presentation choices.

Each tab owns migration of its versioned presentation state. Unknown or
unsupported tab-state versions for an available tab are ignored with structured
diagnostics instead of blocking workspace load. State for an unavailable
provider remains opaque and is not interpreted by the host. State payloads never
grant capabilities or bypass normal validation when converted back into runtime
presentation state.

## Testing

Required coverage:

- structurally invalid layouts fall back to the versioned default layout
- splitter ratios are clamped by current window size, UI scale, and minimums
- duplicate `TabId`, `PanelId`, and `LayoutNodeId` values return typed errors
- activating a tab outside its stack fails without modifying the layout
- registration and placement remain separate operations
- tab placement validates target stack and insertion index atomically
- failed registration does not call `OnAttach()`
- dedicated panels follow the documented attach, visibility, update, draw, and
  detach lifecycle
- attached surfaces detach exactly once in reverse registration order
- hidden surfaces receive notifications but skip visible-only expensive work
- layout save/load preserves tab placement, active tabs, collapsed state, and
  explicitly closed tabs
- unsupported tab workspace-state versions produce diagnostics and are ignored
- malformed, oversized, or over-nested serialized tab state is rejected before
  delivery to a tab
- `LayoutLoadReport` identifies each missing plugin-provided surface and its
  skipped placement while opaque state is preserved
- `SaveLayout()` to `LoadLayout()` round trips explicitly closed tab IDs without
  reopening them through fallback placement
- modal interaction scope prevents menu, toolbar, and panel command dispatch
- editor data-bus notifications still reach attached background tabs while a
  modal blocks their interaction
- viewport panels receive a render-target view without owning backend resources
- transform editing publishes one document event only after transaction commit
- terrain/foliage gestures commit at most one bounded document operation and cancel
  exactly across modal capture, tool/document switch, stale revision and panel detach
- terrain palettes/overlays own no canonical source/history/runtime state, and isolated
  preview close rejects stale work before retiring generation-owned resources

## Why Not Put This in Application?

`Application` owns the process, the window, and the frame loop. It must remain
agnostic to editor UI structure so that:

- game-only and headless hosts can reuse the same `Application` base
- welcome and project-browser screens are not burdened with editor panel state
- editor layout changes do not require `Application` recompilation
- a tab in any panel can communicate with a tab in any other panel without the
  host layer knowing either tab exists
- VFX effect repeat-open focuses one writable persistent document tab; create failure
  opens none, dirty close uses the common leave guard, and close/project teardown
  cancels generation-fenced preview before resource retirement
- stack authoring remains usable without graph UI; unavailable graphs preserve source
  or open read-only and never flatten into stack state
- VFX/decal gizmo drag, cancel and commit preserve pointer capture, transient preview
  and exactly-one-command history across normal, modal interruption and stale revision

The editor workspace is a higher-level concern composed on top of
`Application`. Cross-panel notification flow lives entirely below
`EditorLayer`.

## Relationship to `ui-design-system.md`

- Design tokens, primitives, and composite components live in the GUI module.
- Panels and tabs use those components.
- This document defines how those components are assembled into the editor
  workspace.
- Modal workflow surfaces are assembled above the workspace by
  `EditorModalHost`; they are outside the layout tree.

## File Mapping

Proposed layout:

```text
src/editor/app/
    EditorLayer.h/cpp
src/editor/document/
    EditorWorkspaceController.h/cpp
src/editor/data_bus/
    EditorDataBus.h/cpp
src/editor/status_bar/
    EditorStatusBar.h/cpp
    EditorStatusBarModel.cpp
src/editor/panels/
    EditorPanelHost.h/cpp
    EditorTab.h
    EditorTabContext.h
    EditorSelectionModel.h/cpp
    EditorViewportModel.h/cpp
    EditorMenuBar.h/cpp
    EditorToolbar.h/cpp
    tabs/
        HierarchyTab.h/cpp
        ProjectTab.h/cpp
        PropertiesTab.h/cpp
        AssetsTab.h/cpp
        ConsoleTab.h/cpp
        McpTab.h/cpp
        PerformanceTab.h/cpp
    panels/
        ViewportPanel.h/cpp
src/editor/modals/
    EditorModalHost.h/cpp
    EditorModal.h
    EditorModalContext.h
    SettingsModal.h/cpp
    BuildReleaseModal.h/cpp
    ImportAssetModal.h/cpp
    ConfirmationModal.h/cpp
src/editor/design_system/components/
    ...
```

## See Also

- [Editor Workspace Layout](./editor-workspace.html): HTML reference design for the
  full editor workspace with menu bar, toolbar, docks, viewport, and status bar.
- [Welcome Screen](./welcome-screen.html): HTML reference design for the startup
  surface with recent projects, new/open actions, and news feed.
- [Asset Browser](./asset-browser.html): HTML reference design for the main asset
  browser with folder tree, asset grid, and preview pane.
- [Editor Data Bus](./editor-data-bus.md)
- [Editor Modal Host](./editor-modal-host.md)
- [Settings Modal](./settings-modal.html): HTML reference design for the editor
  settings workflow surface.
- [Editor Document Model](./editor-document-model.md)
- [GUI Screen Host](./gui-screen-host.md)
- [GUI Design System](./ui-design-system.md)
- [System Design](../foundation/system-design.md)
- [Observability Architecture](../observability/observability.md)
- [Extension System](../extensions/plugin-system.md)
