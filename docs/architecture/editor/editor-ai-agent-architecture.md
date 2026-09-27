# Editor AI Agent Architecture

## Purpose

This document defines the Editor AI Agent system for Horo Engine. It covers the
editor-integrated side chat, viewport inline agent, magic AI tools, agent
context model, Horo MCP tool-calling bridge, conversation persistence, and the
internal request/response pipeline.

The Editor AI Agent provides an available authoring assistant for the engine
editor when the host composition and project policy enable it. Horo ships the
MCP-backed capability registry needed by the agent, but registration is not
authorization: project trust, provider policy, context admission, tool
permissions, and per-operation approval still apply. The agent never receives
implicit unrestricted project read/write access.

Its persistent UI surface is a side panel shared across editor screens;
contextual entry points include viewport inline prompts, magic AI tools, and an
optional immersive XR authoring surface. Every surface shares the same provider,
context, permission, transaction, history, and audit owners.

[ADR-172](../../adr/172-immersive-agent-ownership-authoring-mode-and-risk.md)
is the normative authority for the immersive surface's developer-only audience,
Edit/Simulate/Play admission, multimodal evidence classification, explicit
approval, transaction boundary, privacy and cross-system non-ownership. Voice,
gaze, pointing and physical interaction never constitute proposal approval.

## System Overview

```text
┌──────────────────────────────────────────────────────────┐
│  Editor UI (HTML / ImGui)                                │
│  ┌──────────────────────┐  ┌────────────────────────────┐│
│  │ Activity Bar         │  │ AI Chat Panel (slide-in)   ││
│  │  [AI Chat btn] ──────┼──│  Header · History · Input  ││
│  └──────────────────────┘  └─────────┬──────────────────┘│
│                                      │                   │
│  ┌───────────────────────────────────┼──────────────────┐│
│  │ Horo MCP Bridge (WebSocket / IPC) │                  ││
│  │  scene.query  scene.select  scene.move               ││
│  │  asset.list   asset.import  build.trigger            ││
│  │  editor.inspect  editor.run_tests                    ││
│  └───────────────────────────────────┼──────────────────┘│
│                                      │                   │
│  ┌───────────────────────────────────┼──────────────────┐│
│  │ AI Backend (local or remote LLM) │                   ││
│  │  Context assembly → Prompt → Tool calls → Response   ││
│  └──────────────────────────────────────────────────────┘│
└──────────────────────────────────────────────────────────┘
```

The chat panel is a shared UI component (`ai-chat.js` / `ai-chat.css`) injected
into every editor screen that has a right activity bar. The panel opens as a
fixed-position slide-in overlay, respecting the status bar and global dock.

The side panel is not the only entry point. The same agent session is also
available from the editor viewport as an inline prompt and from contextual
"magic" tools such as auto-align, cleanup, and local refactor brushes. These
entry points share the same backend, permission model, history, MCP bridge, and
audit trail.

## Interaction Surfaces

### Side Panel Chat

The side panel is the persistent conversation surface. It is used for general
questions, longer multi-step tasks, build/debug explanations, and reviewing tool
results. It owns conversation history and displays tool-call progress.

### Viewport Inline Chat

Viewport inline chat is a transient prompt anchored to the current editor
viewport state. It is intended for in-context editing while the user is flying
through the scene in editor mode.

Example flow:

1. User enters editor fly mode and navigates the scene using the editor camera.
2. User selects an object or points the reticle at a region.
3. User opens inline chat with a shortcut or right-activity-bar action.
4. User types: "clean up this area", "align these walls", or "make this corner
   cleaner".
5. The agent receives selection, editor camera pose, reticle hit, visible object
   set, and local scene neighborhood.
6. The agent proposes a bounded editor transaction through MCP tools and waits
   for review before application.

The editor camera used by inline chat is not a gameplay camera component. It is
the authoring viewport camera owned by the editor host. Runtime scenes must not
depend on it.

```cpp
struct ViewportInlineInvocation {
    EditorViewportId viewportId;
    EditorCameraPose cameraPose;
    Ray              reticleRay;
    std::optional<RaycastHit> reticleHit;
    std::vector<EntityId> visibleEntities;
    std::vector<EntityId> selectedEntities;
    Rect             screenAnchor;     // where the inline prompt is drawn
    float            neighborhoodRadius;
};
```

### Magic AI Tools

Magic AI tools are deterministic editor tools with an agent-driven planning
layer. They behave like brushes or gizmos: the user points at scene geometry,
drags a region, or selects objects, then invokes a specific action.

Examples:

- **Auto Align**: align selected objects to floor, grid, wall normal, or nearest
  dominant axis.
- **Auto Refactor Scene**: rename, group, reparent, or clean hierarchy within a
  selected scope.
- **Layout Cleanup Brush**: paint over a region and ask the agent to remove
  overlaps, snap props, or normalize spacing.
- **Material Consistency Brush**: propagate material choices to similar objects
  inside the brush radius.
- **Collision Fix Brush**: inspect selected/nearby objects and generate collider
  adjustments.

These tools must expose bounded inputs and deterministic postconditions. The
agent may choose parameters, but execution goes through editor transactions and
can be previewed, accepted, or reverted.

### Immersive Multimodal Authoring

Immersive authoring lets a developer wearing an XR headset issue voice commands
while pointing at, selecting, grabbing, or physically interacting with spatial
content. It is an Editor AI surface, not an XR runtime subsystem and not an
in-game NPC/dialogue feature.

```text
Audio capture -------> speech transcription ----+
                                                  |
XR poses/rays/actions ----------------------------+--> multimodal envelope
                                                  |          |
Editor selection / scene revisions ---------------+          v
                                                     intent + tool plan
                                                             |
                                                             v
                                                scene proposal + ghost preview
                                                             |
                                                   explicit non-voice approval
                                                             |
                                                             v
                                                   editor transaction/history
```

Ownership rules:

- Audio owns input-device and timestamped capture semantics. AIA consumes an
  admitted bounded stream and does not open a native microphone directly.
- A provider-neutral speech service owns partial/final transcription, locale,
  confidence, cancellation, and local/cloud adapters. A transcript is evidence,
  never approval.
- XR owns session identity, poses, rays, actions, tracking validity, and
  interaction-profile state. AIA consumes time-aligned snapshots and does not
  poll OpenXR.
- Physics, XR interaction, and gameplay own point, grab, contact, throw, and
  trigger mechanics. AIA may adapt those events into spatial evidence but does
  not own the mechanics or gameplay meaning.
- MCP validates admitted tools. The editor document/command owner validates and
  commits mutations. AIA owns orchestration and presentation only.

Both editor-owned authoring proxies and real scene/gameplay objects may provide
spatial references. Every evidence record identifies its source, timestamp,
confidence, object generation, scene revision, and XR session generation.
Destroyed, stale, cross-project, hidden, locked, or unauthorized identities are
rejected before planning.

Immersive requests follow an explicit presentation state machine:

```text
Idle -> Listening -> Understanding -> PreparingPreview -> ReadyForReview
                                                       -> Rejected -> Idle
                                                       -> Applying -> Completed
                                                                    -> Failed
```

Voice, gaze, a pointing gesture, contact, or a physical throw cannot transition
`ReadyForReview` to `Applying`. Approval uses an explicit admitted controller,
keyboard, or accessible UI action bound to the exact proposal and document
revision. Camera and world-transform changes receive heightened impact warning.

Authoring mode is part of admission, not presentation state:

| Mode | Immersive agent contract |
|---|---|
| Edit | Query, propose, preview and explicitly approve one editor transaction |
| Simulate | Inspect the isolated simulation and prepare a candidate; return to Edit, revalidate and approve before document mutation |
| Play / PIE | Bounded diagnostics/capture only; no runtime or document mutation |
| Packaged game/server | Capability absent |

Mode, document, project or XR-session replacement invalidates outstanding
approval. A retained proposal remains non-authoritative and must be reviewed
against fresh context.

Long-running transcription, planning, tool execution, and preview generation
are cancellable and expose stable progressive feedback. They do not hide
latency with speculative authoritative world mutation. Feedback supports
captions, non-voice input, reduced motion, handedness, localization, and more
than color/audio alone for critical states.

## Numeric-First Agent Perception

The agent must stay as close to engine-authoritative data as possible. Visual
input is useful for human review and ambiguous layout questions, but it must not
be the primary source of truth for scene reasoning.

The default perception path is numeric and structured:

- transforms, bounds, pivots, and hierarchy from the scene model
- raycast hits, surface normals, and distances from editor query services
- collision shapes and physics contact data from the physics/editor debug layer
- material IDs, mesh IDs, asset GUIDs, and component descriptors from asset and
  scene registries
- grid settings, snap intervals, world units, and editor tool state from the
  editor model
- diagnostics, validation errors, and command history from typed editor systems

Screenshots and framebuffer captures are secondary evidence. They may be
provided when the user asks for visual interpretation or when the agent needs to
compare intended composition against rendered appearance, but the capture should
be paired with numeric overlays whenever practical.

### Why Numeric Data Is Preferred

LLMs handle structured numeric state more reliably than raw pixels for engine
editing tasks. A model can reason more accurately from:

- object AABB: min/max coordinates
- object-to-object distances
- surface normals and hit positions
- selection transforms and pivots
- grid/snap deltas
- overlap volumes and penetration depths
- material/mesh identifiers

than from a screenshot alone. If something is visually wrong, the agent should
first ask the engine for the numeric state that explains the visible problem.

Example: instead of asking the model to infer that two walls are misaligned from
pixels, the editor should provide wall endpoints, bounding boxes, dominant axes,
grid offsets, and gap/overlap measurements.

The same rule applies to temporal behavior. If an animation, character
movement, camera motion, physics interaction, or gameplay sequence looks wrong,
the agent should not need a video as its primary evidence. The engine should be
able to provide sampled analytic data for the relevant time range: positions,
velocities, accelerations, root motion deltas, animation curve values, state
machine transitions, contact events, input samples, and frame timings.

### Agent-Driven Debug Capture

The agent has the same editor control surface as a developer using the editor,
plus access to structured engine telemetry. The user should not have to manually
run a special capture unless policy requires confirmation. When a prompt needs
runtime evidence, the agent may drive the editor through MCP:

1. enter editor simulation or preview mode;
2. set playback range, camera, selected actor, or debug target;
3. enable required telemetry streams;
4. run or scrub the sequence for a bounded time window;
5. collect numeric samples and event traces;
6. stop playback and restore editor state;
7. analyze the data and propose an edit or explanation.

This makes the agent closer to an internal IDE/debugger than a passive chat
window. It can inspect, run, sample, and measure editor/game state through typed
services while preserving undo, permissions, and auditability.

```cpp
struct AgentDebugCaptureRequest {
    DebugCaptureKind kind;       // Animation, Movement, Physics, Camera, Gameplay
    EntityId target;
    TimeRange range;
    float sampleRateHz;
    std::vector<TelemetryChannel> channels;
    bool restoreEditorStateAfterCapture;
};

struct AgentDebugCaptureResult {
    CaptureId id;
    DebugCaptureKind kind;
    TimeRange capturedRange;
    std::vector<TelemetrySeries> series;
    std::vector<DebugEvent> events;
    std::vector<AgentSceneIssue> detectedIssues;
};
```

### Temporal Analytics

Temporal systems should expose data as curves, samples, and events instead of
forcing the agent to infer behavior from video frames.

```cpp
struct TelemetrySample {
    double timeSeconds;
    std::string channel;
    TelemetryValue value;
};

struct TelemetrySeries {
    EntityId subject;
    std::string channel;        // position.x, speed, rootYaw, footLock, state
    std::vector<TelemetrySample> samples;
};

struct DebugEvent {
    double timeSeconds;
    DebugEventKind kind;        // StateTransition, ContactBegin, Notify, Error
    EntityId subject;
    std::string label;
};
```

For animation and movement analysis, useful channels include:

- root transform and root-motion delta per frame
- world position, velocity, acceleration, and angular velocity
- animation state, blend weights, normalized time, and notifies
- foot contact, foot locking error, stride length, and ground distance
- input vector, desired velocity, actual velocity, and controller state
- collision contacts, penetration depth, slope angle, and floor normal
- camera target, camera position, FOV, shake offsets, and clipping distance
- frame time, tick order, and relevant subsystem timings

The agent may request an annotated viewport capture after analyzing telemetry,
but the video/image should explain the numeric finding rather than replace it.

### Visual Data With Numeric Overlays

When visual data is required, the editor should generate an annotated capture:

```cpp
struct AgentViewportCapture {
    ImageId image;
    EditorCameraPose cameraPose;
    std::vector<AgentViewportAnnotation> annotations;
};

struct AgentViewportAnnotation {
    EntityId entity;
    Rect screenBounds;
    Aabb worldBounds;
    TransformSnapshot transform;
    std::string label;
    float distanceToCamera;
};
```

The capture lets the agent correlate pixels with authoritative scene objects.
The image is never sent alone when the engine can provide IDs, transforms,
bounds, or measurements for the same region.

### Numeric Problem Reports

Magic AI tools should produce numeric problem reports before proposing edits:

```cpp
struct AgentSceneIssue {
    IssueKind kind;                 // Misalignment, Overlap, FloatingObject, Gap
    std::vector<EntityId> entities;
    float severity;
    std::string metricName;         // gap, penetration, angleDelta, gridOffset
    float metricValue;
    float expectedValue;
    std::string explanation;
};
```

The agent response may describe the issue in plain language, but the decision
should be grounded in these metrics. This is especially important for alignment,
collision, layout cleanup, scale normalization, and visual consistency tools.

### Data Access Priority

Agent context assembly follows this priority order:

1. Engine-owned typed data models and registries.
2. Editor query services: raycasts, bounds, selection, visible entities.
3. Agent-driven debug captures: telemetry series, sampled curves, events.
4. Diagnostics and validation reports.
5. Annotated viewport captures with numeric overlays.
6. Raw screenshots or videos only when no structured representation exists.

This keeps the agent deterministic, debuggable, and close to the engine instead
of relying on fragile visual guessing.

## Context Model

Before each request, the context provider assembles only the editor context
admitted for that request and makes the active context visible to the user:

```cpp
struct AgentContext {
    // Scene state
    std::string           activeScenePath;
    std::string           activeSceneName;
    uint32_t              objectCount;
    std::vector<EntityId> selectedEntities;
    std::string           viewportCameraName;

    // Asset state
    std::string           lastImportedAsset;
    uint32_t              dirtyAssetCount;

    // Editor state
    EditorMode            editorMode;       // Edit, Play, Simulate
    std::string           activeEditorPage; // Workspace, MaterialEditor, etc.

    // Viewport / inline invocation state
    bool                  hasInlineInvocation;
    EditorViewportId      activeViewport;
    EditorCameraPose      editorCameraPose;
    std::optional<RaycastHit> reticleHit;
    std::vector<EntityId> visibleEntities;
    std::string           activeTool;       // Select, Move, Rotate, AI Brush, etc.

    // Build state
    std::string           lastBuildTarget;
    BuildResult           lastBuildResult;

    // Selection details
    struct SelectedEntityInfo {
        EntityId   id;
        std::string name;
        std::string type;          // Mesh, Light, Camera, Empty, etc.
        std::vector<std::string> componentNames;
        TransformSnapshot transform;
    };
    std::vector<SelectedEntityInfo> selectedDetails;

    // Numeric-first perception data
    std::vector<BoundsSummary>       selectedBounds;
    std::vector<DistanceMeasurement> localMeasurements;
    std::vector<SurfaceSample>       nearbySurfaces;
    std::vector<AgentSceneIssue>     numericIssues;
    std::optional<AgentViewportCapture> annotatedCapture;

    // Optional time-series telemetry gathered by agent-driven debug capture
    std::vector<TelemetrySeries>      telemetrySeries;
    std::vector<DebugEvent>           debugEvents;
    std::optional<AgentDebugCaptureResult> lastDebugCapture;

    // Recent editor state useful for agent reasoning
    std::vector<EditorCommandSummary> recentCommands;
    std::vector<DiagnosticSummary>    recentDiagnostics;
};
```

The context is assembled on the C++ side by the `AgentContextProvider` and
serialized into the system prompt (or tool-call preamble) before each
request reaches the LLM.

## Request Pipeline

```text
User types message
      │
      ▼
┌──────────────────┐
│ Input validation │  sanitize, rate-limit, trim
└────────┬─────────┘
         ▼
┌──────────────────┐
│ Context assembly │  AgentContextProvider::gather()
└────────┬─────────┘
         ▼
┌─────────────────┐
│ Prompt build    │  system prompt + context + conversation history + user msg
└────────┬────────┘
         ▼
┌─────────────────┐
│ LLM inference   │  local (Ollama / llama.cpp) or remote (Anthropic / OpenAI)
└────────┬────────┘
         ▼
   ┌─────┴──────┐
   │ Tool call? │
   └─────┬──────┘
    Yes  │  No ──────────────────────┐
         ▼                           │
┌──────────────────┐                 │
│ MCP tool execute │                 │
│ (scene, asset,   │                 │
│  build, editor)  │                 │
└────────┬─────────┘                 │
         ▼                           │
┌───────────────────┐                │
│ Tool result → LLM │ (loop)         │
└────────┬──────────┘                │
         ▼                           ▼
┌──────────────────┐    ┌─────────────────┐
│ Streaming render │    │ Static render   │
│ (token-by-token) │    │ (full response) │
└────────┬─────────┘    └────────┬────────┘
         │                       │
         └──────────┬────────────┘
                    ▼
         ┌─────────────────┐
         │ History append  │
         └────────┬────────┘
                  ▼
         ┌─────────────────┐
         │ UI update       │
         └─────────────────┘
```

## Tool Calling

The host can compose the Horo MCP bridge without a third-party plugin. Every
agent tool call goes through that bridge, but availability remains distinct from
admission and authorization. The effective tool set is derived from host role,
project trust, user policy, context scope, capabilities, and operation approval.
The model may request a tool; it never decides its own authority.

## Internal IDE Communication

The agent integration uses three distinct channels. They must not be collapsed
into one generic event bus.

### 1. Engine Data Bus — Notifications Only

The Engine Data Bus publishes committed editor state changes:

- active scene changed
- selection changed
- object transform committed
- asset imported or reloaded
- build status changed
- diagnostics updated
- viewport camera moved

The bus is **not** a command bus. Agent requests must not mutate editor state by
publishing bus events. Bus subscribers receive facts about committed state; they
do not execute user intent.

```cpp
struct AgentRelevantEditorEvent {
    EditorEventKind kind;
    Timestamp       timestamp;
    EntityId        subject;
    std::string     summary;
};
```

The `AgentContextProvider` listens to the bus and maintains a compact rolling
cache used when building prompts. This keeps inline prompts fast without asking
the scene system for a full snapshot on every keystroke.

### 2. MCP Bridge — Tool Execution

All agent-initiated reads and writes go through Horo MCP tools. The MCP bridge
is the authoritative automation boundary because it already defines schemas,
validation, capabilities, diagnostics, and stable error handling.

For write-capable tools, the MCP implementation must translate requests into
editor commands/transactions rather than mutating scene storage directly.

### 3. Editor Command System — Undoable Mutation

Scene edits produced by the agent are applied as editor transactions:

```cpp
struct AgentEditTransaction {
    TransactionId id;
    ConversationId conversation;
    std::string label;              // "AI: align selected wall pieces"
    std::vector<EditorCommand> commands;
    bool previewOnly;
};
```

The command system owns undo/redo, dirty-state updates, selection restoration,
and UI refresh. Agent tools do not bypass it.

### Communication Flow

```text
Viewport / Side Panel / AI Brush
      │
      ▼
AgentFrontendEvent
      │
      ▼
AgentSessionController
      │ gathers context from:
      │   - AgentContextProvider cache (bus-fed)
      │   - direct editor queries when needed
      ▼
AgentRuntime
      │
      ├── read/write request ──► Horo MCP Bridge ──► EditorCommandSystem
      │                                      │
      │                                      └── Engine Data Bus publishes result
      │
      └── response stream ─────► UI surface that initiated the request
```

This keeps the system deterministic: the bus observes, MCP validates and
executes, and the editor command system owns mutation.

### Scene Tools

| Tool                     | Description                     | Write |
| ------------------------ | ------------------------------- | ----- |
| `scene.query`            | Find objects by name, type, tag | No    |
| `scene.select`           | Set editor selection            | Yes   |
| `scene.move`             | Move selected objects           | Yes   |
| `scene.delete`           | Delete selected objects         | Yes   |
| `scene.create_primitive` | Create a new primitive          | Yes   |
| `scene.get_hierarchy`    | Read the full scene tree        | No    |
| `scene.snapshot`         | Dump scene state for context    | No    |

### Viewport Tools

| Tool                          | Description                                      | Write |
| ----------------------------- | ------------------------------------------------ | ----- |
| `viewport.get_camera`         | Read editor viewport camera pose                 | No    |
| `viewport.pick`               | Raycast from reticle or screen coordinate        | No    |
| `viewport.visible_entities`   | Return entities visible in current viewport      | No    |
| `viewport.capture_annotated`   | Capture viewport image with entity/metric overlays | No  |
| `viewport.draw_preview`       | Draw transient gizmo/overlay preview             | Yes   |
| `viewport.clear_preview`      | Clear agent-created transient overlays           | Yes   |
| `viewport.focus_selection`    | Move editor camera to selected or target objects | Yes   |

### Numeric Inspection Tools

| Tool                         | Description                                         | Write |
| ---------------------------- | --------------------------------------------------- | ----- |
| `inspect.transforms`         | Return transforms, pivots, and local/world matrices | No    |
| `inspect.bounds`             | Return AABBs/OBBs, extents, centers, and volumes    | No    |
| `inspect.distances`          | Measure gaps, overlaps, angles, and nearest points  | No    |
| `inspect.surfaces`           | Sample normals, slopes, and hit positions           | No    |
| `inspect.collisions`         | Return collider shapes and penetration reports      | No    |
| `inspect.grid_offsets`       | Return snap/grid deltas for selected objects        | No    |
| `inspect.visual_issues`      | Generate numeric issue reports for visible region   | No    |

### Debug And Telemetry Tools

| Tool                         | Description                                            | Write |
| ---------------------------- | ------------------------------------------------------ | ----- |
| `debug.enter_preview`        | Enter editor preview/simulation mode for capture       | Yes   |
| `debug.exit_preview`         | Stop preview and restore editor state                  | Yes   |
| `debug.scrub_time`           | Scrub animation, sequencer, or simulation time         | Yes   |
| `debug.capture_telemetry`    | Run bounded capture and return numeric time series     | Yes   |
| `debug.get_capture`          | Read a previous capture by ID                          | No    |
| `debug.compare_captures`     | Compare two captures and return numeric deltas/issues  | No    |
| `debug.animation_curves`     | Read sampled animation curves and state transitions    | No    |
| `debug.movement_trace`       | Read movement/controller samples for an actor          | No    |
| `debug.physics_trace`        | Read contacts, penetrations, impulses, and floor data  | No    |
| `debug.camera_trace`         | Read camera position, target, FOV, shake, and clipping | No    |

Telemetry tools allow the agent to run the editor like a developer would, but
with direct access to the underlying math. They are bounded by time range,
sample rate, target entity, and enabled channels. Captures must restore editor
state unless the user explicitly asks to keep the session running.

### AI Action Tools

| Tool                         | Description                                           | Write |
| ---------------------------- | ----------------------------------------------------- | ----- |
| `ai.align_selection`         | Align selected objects using explicit constraints     | Yes   |
| `ai.cleanup_region`          | Produce bounded layout cleanup in a painted region    | Yes   |
| `ai.refactor_hierarchy`      | Rename, group, or reparent objects in selected scope  | Yes   |
| `ai.fix_collision_region`    | Generate collider adjustments for selected scope      | Yes   |
| `ai.material_consistency`    | Normalize materials across similar nearby objects     | Yes   |
| `ai.preview_transaction`     | Build an unapplied transaction preview                | Yes   |
| `ai.apply_transaction`       | Commit a previously previewed transaction             | Yes   |
| `ai.revert_transaction`      | Revert an agent-created preview or applied edit       | Yes   |

AI action tools are not free-form mutation endpoints. Each action has a typed
schema, bounded scope, preview output, and undoable transaction path.

### Asset Tools

| Tool           | Description              | Write |
| -------------- | ------------------------ | ----- |
| `asset.list`   | List assets in project   | No    |
| `asset.import` | Import an external asset | Yes   |
| `asset.info`   | Get asset metadata       | No    |

### Build Tools

| Tool            | Description          | Write |
| --------------- | -------------------- | ----- |
| `build.trigger` | Start a build        | Yes   |
| `build.status`  | Query build progress | No    |

### Editor Tools

| Tool               | Description                    | Write |
| ------------------ | ------------------------------ | ----- |
| `editor.open_page` | Navigate to an editor screen   | Yes   |
| `editor.inspect`   | Read inspector property values | No    |
| `editor.run_tests` | Trigger project test suite     | Yes   |

Write-capable tools produce a reviewable proposal and require explicit user
approval before execution. Project policy may streamline presentation for
deterministic tools, but it cannot let model output, voice, gaze, pointing, or a
gesture approve its own mutation. Destructive and immersive scene changes always
retain the visible proposal and revision-bound approval boundary.

## Backend Providers

The system supports pluggable LLM backends:

The `HoroEngine::ModelProvider` contract owns copied requests, ordered text and
tool-intent events, provider-reported usage, sanitized errors, cooperative
cancellation, discovery, and explicit feature admission. `HoroEngine::ModelProviderAdapters`
provides real Ollama and OpenAI-compatible HTTP adapters. The editor host registers
factories explicitly, selects one provider before inference, and passes an opaque
credential reference plus a dispatch-time resolver for cloud access. A registered
factory has no ambient side effect. Tool intents are proposals for the agent's
separate policy and approval path; adapters never invoke MCP tools. A host may
advertise tool support only after confirming it for the selected model; missing
support returns `UnsupportedCapability` before transport work. HTTP and provider
JSON types remain private to the adapter target. This seam belongs only to the
editor agent, never `HoroAI` gameplay or packaged runtime composition.

```cpp
enum class AgentProvider {
    Local,
    Cloud,
    Hybrid,
};
```

### Local Provider

- Runs the model on the developer's machine
- Recommended for offline work, privacy-sensitive projects
- Has no network dependency after its model/runtime is available
- Reports model capabilities, context limits, cost class, and availability
  through the same provider contract as remote adapters

### Cloud Provider

- Connects through a provider adapter selected by host/project policy
- Uses an opaque credential reference rather than persisting a raw secret in
  project settings
- Receives only the context admitted and disclosed for the current request
- Declares retry, quota, streaming, tool-call, and data-residency capabilities

### Provider Selection

The active provider is selected in the chat panel header. Per-conversation
override is supported. System administrators can lock the provider via
project settings.

### Provider Budget And Degraded Operation

Provider selection also resolves a policy-bounded budget for transcription and
model inference. The policy can limit requests, tokens/audio duration, monetary
estimate, concurrency, retries, and session/project totals. Usage presentation
must distinguish an estimate from provider-authoritative billing.

Rate limiting, quota exhaustion, offline state, credential loss, and provider
failure are normal lifecycle results. They may offer an admitted local provider,
queue a user-visible retry, or stop the request. They never trigger unbounded
background retries, silently switch data residency, or mutate scene state.
Cancellation and shutdown revoke retries and release provider work.

## Conversation Management

### History

Conversation history is stored per-project in a local SQLite database:

```cpp
struct ConversationRecord {
    ConversationId  id;
    std::string     title;             // auto-generated from first prompt
    std::string     editorPage;        // which page the chat started on
    std::string     sceneContextHint;  // snapshot of active scene at creation
    Timestamp       createdAt;
    Timestamp       lastMessageAt;
    uint32_t        messageCount;
    bool            pinned;
};
```

History is searchable from the chat panel. Old conversations are
auto-archived after 30 days (configurable).

### Context Injection

Users can explicitly add context to a prompt:

- **Selection**: "Use the selected object" — current selection is injected
- **Viewport**: "What's in front of the camera?" — viewport frustum snapshot
- **Console**: "Check the last error" — recent console log entries
- **File**: drag-and-drop a file reference into the chat input

These are surfaced in the chat UI as context chips above the input area.

## Privacy and Security

### Data Boundaries

- **Local mode**: No data leaves the machine. The LLM runs on-device.
- **Cloud mode**: Only context explicitly admitted by the active policy and
  shown to the user is sent to the provider. Project files, scene data, assets,
  transcripts, tracking evidence, and tool results are individually classified;
  enabling cloud inference does not admit them automatically.
- **Tool results**: MCP tool responses are included in the LLM context. Read
  results are schema-bounded and redacted before provider admission. Binary
  assets, raw voice, continuous gaze/pose history, camera frames, and environment
  geometry are excluded by default.

The host-owned `Horo::Security::AgentPolicy` is the shared local/cloud admission
boundary for future agent-provider composition. The UI must show the exact
classified, redacted preview and provider residence before recording one-request
consent; the policy binds consent to each item's content digest, classification,
project/trust revision, and provider. Context is copied into an ephemeral bounded
envelope only after this check. It does not retain raw context in consent or audit
state. The host calls the one-use dispatch authorization immediately before the
provider send; it rechecks the exact envelope and current project/trust revision.
Hybrid providers resolve to the actual local or cloud residence for each request;
there is no less restrictive third admission path.
Credentials and raw sensor data are never ordinary context; project files,
tool results, proposed changes, transcripts, and sensitive project data require host redaction before
admission. The host owns accurate classification/redaction and must not pass the
policy's mutable consent/approval methods to a model or provider adapter.

### Permission Model

```cpp
enum class AgentPermission {
    ReadScene,       // read scene hierarchy and properties
    WriteScene,      // create, move, delete objects
    ReadAssets,      // list and inspect assets
    WriteAssets,     // import, rename, delete assets
    TriggerBuild,    // start build pipeline
    RunTests,        // execute test suite
    NavigateEditor,  // open editor pages
};
```

Permissions are configured per-project in the AI settings panel. The agent's
effective permission set is displayed as a badge in the chat header.

Every non-read-only operation, in local and cloud mode alike, requires an exact
provider/operation/proposal-revision approval consumed immediately before dispatch.
An untrusted project cannot acquire mutation, execution, credential, or network
authority through an approval. Host project/trust revision changes revoke pending
consent and approval; an already returned decision cannot be cached across that
boundary. `AgentPolicy` supplies this runtime contract and bounded, payload-free
decision history; provider adapters and the agent loop must integrate it at their
later composition boundaries rather than treating this contract as automatic
provider or MCP dispatch wiring.

### Audit Trail

All tool invocations append schema-bounded, redacted audit records with:

- Timestamp
- Tool name and admitted argument summary
- Result summary (success / error code)
- User confirmation status
- Provider/session, operation, proposal, and document revision identities

Raw credentials, raw voice, continuous gaze/pose history, camera frames,
environment geometry, and unrestricted tool payloads are not copied into the
audit store. The editor console is a projection of the audit record rather than
its authoritative owner.

## Editor Integration

### Panel Lifecycle

1. The chat panel is a shared component loaded by every editor screen
2. It initializes as a hidden fixed-position panel (right side, 380px)
3. Clicking the AI Chat button in the right activity bar toggles it open
4. Opening the panel establishes (or resumes) a conversation
5. Closing the panel hides it; the conversation persists in the background

### Activity Bar Button

When the AI capability is composed, the AI Chat button is contributed to the
right activity bar through the shared panel/command registration path. The
button:

- Shows a chat bubble icon
- Highlights blue when the panel is open
- Is absent when AI is disabled or unavailable and does not appear on modal
  dialogs

### Cross-Page State

Opening a different editor screen preserves the conversation. The chat panel
re-attaches to the existing conversation when opened on the new page.
Context is refreshed to reflect the new page's state.

## Viewport Inline Editing Workflow

Viewport inline editing is available only in editor mode. It is disabled in
runtime play mode unless the editor is running a simulation clone with explicit
edit routing.

### Invocation

Inline chat can be opened by:

- keyboard shortcut while the viewport has focus
- right activity bar AI Chat action with viewport focus
- context menu on selected object or reticle hit
- magic AI brush/gizmo action

The inline prompt is rendered near the reticle or selection bounds. It must not
steal camera navigation unless the user starts typing. Pressing Escape closes the
prompt and returns full focus to viewport navigation.

### Context Scope

The inline request receives a tighter context than the side panel:

```cpp
struct InlineAgentContext {
    ViewportInlineInvocation invocation;
    std::vector<SelectedEntityInfo> selection;
    std::vector<VisibleEntitySummary> visibleNeighborhood;
    std::vector<SurfaceSample> nearbySurfaces;
    std::vector<DiagnosticSummary> localDiagnostics;
};
```

The context is intentionally local. Inline prompts should not scan the entire
project unless the agent explicitly asks for broader context through MCP.

### Preview Before Commit

For ambiguous visual edits, the agent should generate a preview transaction
first:

1. Build candidate edit commands.
2. Draw ghost transforms, outlines, brush falloff, or before/after markers.
3. Present inline actions: **Apply**, **Refine**, **Cancel**.
4. Commit only through `ai.apply_transaction`.

Even low-risk exact requests, such as "move selected object 1m up", produce a
reviewable proposal. The UI can keep this review compact, but agent-authored
scene mutation does not bypass revision-bound approval. Destructive or immersive
changes receive a more prominent impact summary.

### Example: Fix This Corner

```text
User: "clean up this area"
Context:
  - editor camera pose
  - reticle hit: wall_corner_03
  - selected: none
  - visible: 21 nearby objects
Agent:
  1. viewport.pick
  2. scene.query neighborhood around hit
  3. ai.cleanup_region previewOnly=true
  4. viewport.draw_preview
User clicks Apply
Agent:
  5. ai.apply_transaction
```

## Magic AI Tool Contracts

Magic AI tools must be implemented as editor tools, not as hidden prompt-only
features. Each tool provides:

- typed input schema
- visible scope indicator
- deterministic constraints
- preview renderer
- undoable transaction output
- validation report

```cpp
struct MagicAiToolDescriptor {
    ToolId id;
    std::string displayName;
    std::string description;
    AgentPermission requiredPermission;
    bool supportsBrushInput;
    bool supportsSelectionInput;
    bool requiresPreviewBeforeApply;
    ToolSchema inputSchema;
};
```

### Brush-Based Tools

Brush-based tools sample the scene under the brush stroke and build a bounded
edit region:

```cpp
struct AiBrushStroke {
    EditorViewportId viewportId;
    std::vector<ScreenPoint> points;
    float radiusWorld;
    BrushFalloff falloff;
    std::vector<EntityId> affectedCandidates;
};
```

The agent may choose how to modify candidates inside the region, but it may not
expand scope silently. If it needs more objects, it must ask or present a larger
preview scope.

### Auto Alignment

Auto alignment should use explicit constraints rather than vague transforms:

```cpp
struct AutoAlignRequest {
    std::vector<EntityId> targets;
    AlignReference reference;      // Grid, Floor, WallNormal, SelectionAverage
    AxisMask axes;
    float snapStep;
    bool preserveRelativeOffsets;
    bool previewOnly;
};
```

The implementation should prefer deterministic geometry queries: floor raycasts,
normal sampling, bounding boxes, grid settings, and dominant-axis detection.
The LLM should choose strategy; the editor tool should execute the strategy.

## Runtime and Performance Constraints

- Inline prompt context gathering must be incremental and bounded. Do not block
  the render thread on full-scene serialization.
- Viewport raycasts and neighborhood queries run through existing editor query
  services and must respect editor frame budget.
- Agent-driven debug captures must be bounded by explicit target, time range,
  sample rate, and telemetry channel list. They must not start unbounded play
  sessions or silently keep simulation running after capture.
- Telemetry sampling must run through debug/observability services, not ad hoc
  per-frame string serialization from hot runtime paths.
- Long-running agent operations run asynchronously and stream progress to the
  invoking surface.
- Preview overlays are transient editor resources; they are not scene objects and
  are never saved.
- Every applied agent transaction must be undoable in one user-level undo step.
- Voice/transcription and XR evidence queues are bounded; continuous audio,
  gaze, pose, or environment history is not retained as ordinary context.
- Disabled immersive/provider paths do not add work to the XR frame loop or
  audio callback.

## Failure Handling

If an agent action fails:

- keep the preview visible if possible
- show the failing tool and stable error code in the chat
- do not partially commit commands
- restore previous selection and editor tool
- append the failed transaction to the audit trail

## Static Demo (Current Implementation)

The current shared `ai-chat.js` / `ai-chat.css` provides a static frontend:

- Slide-in panel with header, message history, and input area
- Echo-response demo: messages are echoed back with a static reply
- Escape key closes the panel
- Panel respects the status bar height via `syncPanelBottom()`

Backend integration points are marked with `TODO(ai-agent)` comments in the
JavaScript source.

## Future Work

- **Agent-to-agent communication**: Multiple agents (e.g., architect +
  implementer) collaborating on complex tasks
- **In-game voice agents**: player-facing dialogue/NPC agents are a separate
  runtime product from the editor-owned immersive authoring workflow
- **Proactive suggestions**: Agent monitors console output and offers help
  when errors or warnings appear
- **Code generation**: Agent writes C++ component stubs, material graphs,
  or behavior tree nodes directly into the project
- **Tutorial mode**: Agent guides new users through engine workflows
