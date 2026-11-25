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

The local chat composer offers a separate numeric viewport review. It shows
camera position, target, up, projection and clipping values, the active edit
tool and bounded selected IDs. The viewport lease is retired by camera, tool,
selection, Scene, preview or Play transitions. The viewport action opens the
local chat and stages this review before any prompt is sent. The user can add
or remove authored local transforms for the same selected IDs. Selection-only
review keeps its own lease and does not admit camera data.

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
- The worker transcription session accepts one admitted Audio capture generation
  and one explicitly selected provider. It bounds input duration and transcript
  updates, rejects gaps and provider timestamps outside the admitted PCM window,
  and issues final evidence only after input is explicitly sealed. The Editor
  rechecks context and cancellation when it adopts evidence. PCM is borrowed
  for one provider call and is never replayed into a replacement provider.
- The macOS adapter requires speech authorization and checks that the selected
  locale supports device-local recognition before creating a Speech request.
  It uses Audio-owned PCM and retains only the latest framework result. Its
  authorization action is separate from microphone permission and stream opening.
- The macOS microphone owner reserves one foreground speech lease, separate
  from test and recording purposes. Reads use the exact Audio grant and report
  interruption instead of delivering queued PCM after a drop. Explicit sealing
  detaches the device before handing the last PCM blocks to the worker. Project
  replacement and cancellation revoke the lease even after detachment.
- The speech worker receives live PCM through a bounded single-producer queue.
  After sealing, it consumes the detached tail after the live blocks and then
  finalizes provider input. The editor polls one latest partial or the terminal
  result without waiting. The capture lease revokes both and queued PCM is
  cleared at worker retirement.
- XR owns session identity, poses, rays, actions, tracking validity, and
  interaction-profile state. AIA consumes time-aligned snapshots and does not
  poll OpenXR.
- The editor aligns final speech with bounded XR observations in the Audio
  capture clock domain. Each observation carries the host's before/after
  measurement around its XR cutoff. XR timestamps order publications within
  that session. Uncertain phrase boundaries and overlapping observations fail
  explicitly. Missing spatial evidence produces a speech-only result. The
  envelope retains the speech lease and rechecks XR origin and binding revisions
  when the editor adopts it.
- The native XR Input owner brackets its actual runtime time sample when the
  host supplies an admitted Audio capture clock. It publishes that interval
  only with the matching committed pose cutoff. A separate interval brackets
  synchronization of the controller action state.
- The macOS listening owner lends that clock only while its exact microphone
  lease accepts input. A pose batch must match both the measured XR cutoff
  and the listening generation before the editor retains it.
  Command assembly accepts finite World-space points, positive-radius spheres,
  and nonempty ray segments from that cutoff.
  The local panel retains eligible final speech when spatial geometry or timing
  is unusable, or when the spatial history exceeds its capacity. It marks the
  spatial evidence as unavailable and carries no rejected coordinates forward.
- The editor observes the existing player-scoped XR pointer after Input commits.
  A new selection press during the same listening operation retains its measured
  ray. Held buttons and source replacement do not create new observations.
  The ray carries its own pose interval. When both adjacent states have valid
  action intervals, the press carries the range between the observed released
  and pressed states. An uncertain press cannot be inferred from the ray's time.
- On macOS, the local Agent panel offers explicit listening controls for the
  current authored scene in focused XR Play. The scene selection is optional.
  Speech and microphone permission are
  requested separately. Play capture retains only a transient transcript preview
  and does not grant model, tool or mutation authority. The panel clears capture
  when presentation, selection or the Play lineage is withdrawn.
  Before capture starts, the panel identifies the on-device path. A recording
  limit, unavailable recognizer, or low-confidence result releases that capture
  and reports the reason. A new recording requires a new user action.
  One elapsed-time budget covers provider open, input, and result polling.
  Expiration signals cooperative provider cancellation even during a provider call.
  The local panel displays only the latest admitted partial as a draft. Final
  recognition replaces it and enables reference review. Cancellation or context
  withdrawal clears the draft.
- The local Agent panel can request a read-only review of captured selection
  references. The workspace checks the exact authored revision and active Play
  scene, then reports each candidate's visibility, lock, or scope status.
  Multiple captured candidates require clarification.
- In authored XR Play, an admitted controller press can record the nearest mesh
  triangle hit per intersected draw. The query borrows the latest committed
  authored geometry on the owner lane and applies a fixed triangle and hit budget.
  The Agent panel can review the same-world hits aligned to the speech interval.
  Incomplete hit queries produce an unavailable result.
  Location review shows press-time world coordinates in meters for eligible
  objects. Separate points on one object remain separate location candidates.
  Physical surface review uses the separate one-session plane grant. The
  controller press copies bounded hit coordinates and plane identities from
  the admitted publication. Each preview checks that the same grant and all
  referenced plane identities are still live.
  Play mesh hits and listening lineage retain the structural revision of the
  draws, so recycled object identities require a new observation.
  World UI dispatch owns its targeted and reserved gestures. Scene evidence
  observes the raw button edge after that routing decision, including releases
  and UI-owned presses. A held UI press cannot become a later scene reference.
  Mounted XR Play resolves hits against the prepared origin-relative cell
  geometry. The review carries the exact Scene structural revision and uses
  runtime membership for read-only cell references. Cell or origin retirement
  withdraws the captured review.
- Each listening operation retains at most one fixed-capacity spatial history
  on the editor owner lane. A batch that exceeds the remaining capacity makes
  that history unusable for the utterance. Sealing stops both PCM and spatial
  input before the worker completes, and cancellation clears the observations.
- Physics, XR interaction, and gameplay own point, grab, contact, throw, and
  trigger mechanics. AIA may adapt those events into spatial evidence but does
  not own the mechanics or gameplay meaning.
  The on-demand Physics preview reads contact starts and trigger entries for the
  selected object in one completed Play tick. It filters by the object's
  resident body handles when they fit the Physics filter budget and uses the
  complete tick otherwise. It displays the tick alongside
  the events and leaves speech timing separate. Incomplete event batches and
  hidden, locked, or deleted peers suppress the preview. A new selection,
  document revision, or Scene structural revision withdraws the captured review.
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

The C++ context provider assembles the selected evidence before the request
reaches the model.

The editor Agent turn's explicit attachment path captures an ordered typed preview
bound to one project generation, trust revision, request and provider residence.
Source ranges use an already-open immutable source snapshot. A selected file may
use the bounded rooted file reader. Diagnostics and build status use the
authenticated read-only MCP query route. An editor owner may use the owned result
of a query prepared on a worker while the MCP owner pumps its queue. Selected
scene evidence, build output, notes and reports can arrive as revisioned text
attachments from their owners. No document is opened during this preparation.

The preview exposes the exact redacted provider payload and its provenance.
After the user confirms it, the turn rechecks the project, trust and producer
revisions and consumes the matching consent. Attachment text travels as
user-role evidence. The completed conversation excludes these ephemeral context
messages, so a later request must separately confirm its selected context.

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

The editor turn runtime receives a host-selected provider and an existing
authenticated in-process MCP session. It waits on a host worker while the
canonical MCP controller dispatches through its registered owners. Each turn
admits one inference or tool operation at a time, bounds its history, rounds,
stream events, tool calls, output-token allowance and deadline, and returns one
terminal outcome. One editor runtime admits one active turn and rejects
concurrent or reentrant work. Malformed or repeated tool intents are rejected before their
round submits any tool. Tool execution keeps the controller's approval check,
and termination of the turn cancels a still-pending exact operation. The
runtime does not retry provider requests or publish partial conversation history.

For the local scene count tool, the chat composer presents the tool, its empty
argument list, the current project and the exact count before sending a turn.
The user may deny the review or approve one read-only call for that turn. The
composer drops the review when it is hidden, the project is detached, the
observed count changes, or the review expires. The worker receives only the
approved count and owns the bounded, single-use tool session. Stopping the
turn revokes that session's exact principal, including during provider I/O.
Once the controller accepts the call, chat can open the MCP dock at its exact
audit identity. The history view matches the numeric principal, session,
generation and operation. The renderer receives no tool arguments or results
through this link.

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

For authored object replacements, `SceneDocumentTransactions::PreviewReplacements`
validates the complete batch and returns the effective typed changes for review.
The review owns detached values tied to the original document opening and
revision. After approval, the owner calls `ApplyReviewedReplacements` with fresh
edit admission. It revalidates the reviewed batch against the live document and
records one canonical history entry.

`scene.propose_transforms` accepts a bounded batch of local transforms for
visible authored objects on the exact Scene revision returned by inspection.
`scene.propose_edits` can combine local transforms, object renames, and enabled
state changes on existing authored lights in the same review and undo entry.
The viewport displays the name and light changes alongside the visual transform
preview. Light state takes effect only when the operator applies the review.
The MCP proposal grant admits the preview. Apply and discard are local viewport
controls. The preview is retired when the originating authority expires or the
authored Scene is replaced.

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

Structured source patches use a versioned object with a files array. A file
record declares an operation, its project-relative path, and the operation's
allowed text, destination, or original-byte hunks. The editor supplies the
proposal identity and the exact source snapshots offered to the model. Parsing
checks duplicate fields, finite limits, project path policy, and the original
hunk bytes. It returns a typed proposal without opening, editing, or saving any
file. Recovery uses its separate committed envelope.

Unified text diffs use paired project-relative paths, declared old and new
line ranges, and exact source context. The decoder accepts plain unified
headers and matching Git text headers, preserves source line endings, and
recognizes the missing-final-newline marker. Creates and complete deletions
use the unified null-file sentinel. Renames are represented explicitly in the
structured format. Git hash text supplies no source revision authority.

Workspace edit planning binds one admitted proposal to explicit read-only
observations of its source and destination paths. Current source leases must
match both the buffer and saved-base evidence. The workspace host supplies
observed absence for creates and renames. The planner produces one bounded
preview or typed conflict per operation in canonical proposal order. Its output
retains the exact proposal digest and never changes source, history, or disk.
Permissions, paths, and revision evidence are checked again by the mutation
owner before application.

The source editor projects the selected operation from that plan into its
existing gutter and text decorations. It maps original byte coordinates to
display coordinates and retains the exact source publication. Navigation
only moves the caret. A newer source publication or a tentative widget edit
retires the markers and reports a conflict until a new plan is selected.
The local model panel offers review for a complete JSON change or unified diff
returned to a turn with explicitly selected whole source files. The open-tab
picker retains the exact selected snapshots across pages. One consent action
covers all displayed redacted context. Proposal admission is limited to the
unredacted file snapshots sent in that turn. Each completed-reply card shows
the admitted path and queues the corresponding existing source tab through
the workspace command lane. Reviews remain scoped to each tab. Rejecting a
proposal records the choice against its digest and disables its chat actions.

A review decision set binds its canonical file and hunk indices to the
proposal digest. File and bulk decisions update their selected units
atomically. Structural operations have one file decision. Acceptance and
undo validate current workspace evidence before publishing a decision.
The detached review encoding retains the current unit states and proposal
digest for recovery. It carries no source bytes or mutation authority.
The chat shows one change range at a time for each selected edit. Its
controls use the original byte range and the canonical hunk index for the
decision, even if the displayed file later becomes stale.
Deleting a source with unsaved edits is a planning conflict, even when the
proposal captures the current revision. The review also identifies a case-only
rename as a portable path conflict. Both checks reject acceptance independently
of the preview so a supplied preview cannot bypass them.
For create or rename review the user may name one exact project-relative
destination for the current reply. An I/O worker walks the existing parent
directories and distinguishes absent leaves, occupied names, missing parents,
case aliases and redirects. The destination grants read-only review scope.
New file text is shown as a bounded excerpt. Restoring a saved review checks
its structural destinations again before restoring accepted decisions.
Applying one accepted creation stages an ordinary dirty Source tab without a
disk base. Even an empty draft stays dirty until an explicit Save or Save All
publishes the file with an exclusive rename. The grouped Undo action can close
the exact unchanged draft. A later user edit or save retires that undo
authority, and rejected creation leaves the live document registry intact.
An accepted deletion requires an explicit move to project Trash. The worker
captures the exact saved bytes. Under the project and source writer leases the
owner rechecks the clean source and its native identity before moving it. The
Trash entry preserves the original relative path and complete bytes. The tab
closes after the native move reaches durability. A post-move failure attempts
an exclusive move back to the original name, preserving any new occupant.
The grouped Undo action restores the current deletion's witnessed payload
only to its original project-relative path. It checks the path with the same
no-follow destination inspection as the review and refuses a new file, case
alias, redirect, open document, or replaced Trash payload. A failed restore
after a visible move attempts to return the payload to Trash. Once restored,
the file can be reopened through the normal Source route.
Confirming a single reviewed rename moves the existing disk base with an
exclusive same-filesystem rename. The editor rekeys the same document instance
only after a durable move, retaining the open tab and text history. Accepted
rename hunks and existing unsaved edits stay in the resulting buffer. A later
source save publishes the buffer at its new name. On failure the owner keeps
the original routing and attempts an exclusive rollback after a visible move.
The grouped Undo action can restore the old spelling and pre-review buffer
while the rename's published document state is current. A worker verifies the
renamed disk base before the owner moves it back. Undo uses the same exclusive
move and leaves either path intact when the old name has acquired a new
occupant. Saving or editing the renamed document retires its grouped undo.

The Local Model panel can explicitly replace the recovery checkpoint at
`.horo/local/agent/source-review` with the admitted proposal and its current decisions. The
checkpoint keeps the exact proposal, including the reviewed change text, and
the allowed path identities. It excludes the provider conversation and its
request context. The editor publishes the file durably on its storage worker
under an exclusive project-file lease. Restoring builds a new read-only plan
from current open source snapshots. Each restored acceptance requires matching
path, buffer and saved-base digests, and valid original ranges. Editor
revision counters may differ after reopening a byte-identical source. The
checkpoint preserves the decisions at save time. Undo tracks decisions made
in the current review session.

The Local Model panel queues an explicit Apply command with the accepted review
and its exact offered-file policy. The workspace dispatches it after drawing.
The preparation worker uses one plan, aggregate text budget, and audit for
the entire accepted selection. It renders accepted text from original ranges,
inspects each structural destination, and records saved-file evidence before
returning the detached candidate. Text-only publication rejects candidates
that also carry structural operations.
When the accepted file operations are creations, the source owner stages
the draft identities and buffers alongside any text replacements. It
revalidates the edit bases before exposing the group. Creation remains
unsaved until ordinary Save. Grouped Undo replays the edits and retires the
drafts together while every affected publication is still current.
Groups may include confirmed renames and deletions alongside edits and drafts.
Each deletion has its own reserved payload and durable origin manifest in
project Trash. A successful group closes the deleted source sessions.
The source owner stages all rekeys and acquires the writer leases before any
move. It makes the new buffers and routing identities visible only after
every rename is durable. A later failure rolls visible moves back in reverse
order using exclusive moves. If an original name is now occupied, recovery
keeps the source review and Trash evidence available. The workspace keeps
the pre-application document roots.
After a successful group, Undo restores the prior names and working buffers,
replays the text edits, and closes the unsaved drafts. It verifies the
renamed disk bases and the captured Trash payload identities before using
them. Every original destination must be free. An unavailable payload, a
new occupant, or a newer user edit prevents completion of the group. Files
restored from Trash can be reopened through the ordinary Source route.
Exclusive moves use the Windows, macOS, or Linux native rename primitive.
The review reports cross-volume moves and unavailable exclusive rename support
as distinct refusals. Recovery uses the same native primitive and does not
copy files between volumes.
The source owner rechecks the policy, the live publications and the saved disk
bases before publishing one batch of buffer changes. Only accepted text ranges
enter that batch. The workspace retains its latest receipt for a single grouped
Undo action while every affected publication is still current. Source files are
written through the ordinary save flow.

Queued Apply and Undo commands carry the current workspace attachment's
cancellation token. Detaching the workspace cancels that admission before an
old command can publish. The owner serializes accepted transactions with manual
source edits and checks current leases and disk bases at execution. A competing
command that loses its evidence reports a conflict. An older review never
rebases onto new user or external edits without another review.

The command shares its immutable preview with one workspace-owned preparation
job. Policy validation, reconstruction of the accepted text, and exact reads
of the saved disk bytes run on an I/O worker. The editor polls completion
without waiting. The source owner compares the current leases and native file
mutation evidence, then publishes the group. A manual edit
can advance a source while the job is running. In that case the owner rejects
the stale preparation before publishing any buffer.
Missing native file identity evidence also prevents publication.

Successful review application attaches content-free attribution to the exact
source owner receipt. It contains proposal and policy commitments, canonical
decision states, and the previous and published revisions of accepted files by
proposal index. Source paths, ranges, text, and provider responses stay outside
the attribution. The panel identifies the last successful proposal by its digest.

The application audit has a separate versioned, bounded encoding with a
checksum over its canonical payload. Each accepted file has exactly one
ordered before-and-after revision pair. Decoding provides attribution only
and cannot open a document, dispatch an edit, or restore undo authority.
The workspace appends the audit on an I/O worker after the source owner returns
its receipt. Atomic publication retains the most recent 16 applications in
`.horo/local/agent/source-review-history` under the project and destination
leases. Reopening a workspace reads historical attribution without restoring a
live transaction or touching source files. Corrupt history stays on disk and
the panel reports a read or publication error.
Each record has a project-local sequence, assigned under the writer lease.
The editor advances only the outcome of that exact record. An application is
saved when every changed revision has a durable in-place source save receipt.
Post-apply validation captures a live saved review from the source owner.
Later edits, closed buffers, and history restored on reopen cannot supply that
receipt. The disk check compares the saved bytes and verifies that deleted or
renamed source paths are still absent. It runs on an I/O worker and may reject
changes that the workspace has not observed yet.
The source review panel offers separate build and test approvals. Build
approval captures the saved application on the workspace command lane and
starts an isolated native build. Test approval is available after that
specific build succeeds. The result belongs to the live audit and its source
revisions. Undo, edits, or workspace retirement invalidate that result.
Source Save As, later edits, and unconfirmed writes cannot supply that proof.
An explicit group undo records a reverted application. An invalidated unsaved
revision or an applied-only record reopened without live ownership is stale.
The reader retains older records without outcome evidence as historical
attribution. It never authorizes replay or replaces source bytes.

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

`HoroModelProviderAdapters` privately consumes the security credential contract
and zeroizing byte owner. The model contract remains independent of security.
The editor host owns credential provider selection and lifetime. The adapter
receives only the injected provider and opaque reference for dispatch.

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

`HoroEditor --local-agent` enables the Local Model panel in the workspace.
Enter the loopback HTTP service address and an installed model, or discover
models from that address. Discovery and explicitly submitted text run on a
worker. The workspace keeps running while the service responds. The panel sends
the entered message, and workspace detachment cancels pending work.

The panel can authorize a single read-only MCP query for the displayed scene
object count. The worker checks that the selected installed model supports
tools before sending the prompt. Its authenticated session exposes only the
approved count, and releases the snapshot when the turn ends.

### Cloud Provider

- Connects through a provider adapter selected by host/project policy
- Uses an opaque credential reference rather than persisting a raw secret in
  project settings
- Receives only the context admitted and disclosed for the current request
- Declares retry, quota, streaming, tool-call, and data-residency capabilities

The host-bound OpenAI-compatible adapter resolves typed credential references
through the shared security provider for each HTTPS operation. The adapter
checks the endpoint before lookup and keeps resolved bytes in zeroizing
application-owned storage. Endpoint and model consent remain host-owned.

The desktop chat panel exposes cloud mode when the host supplies a native
credential backend. Each model list or inference request requires approval of
the displayed HTTPS endpoint. The inference review also displays the selected
model and exact message. The worker sends one user message and has no project
context, history, or tool capability. The editor restores the model, endpoint,
and opaque credential reference from a user-level profile. The native
credential store owns the key, and the panel exposes explicit removal.

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

The private history owner publishes bounded SQLite images under
`.horo/local/agent/conversations.sqlite`. It uses an exclusive project-file
lease and durable atomic replacement. The host opens the store on an editor
worker. Corrupt, foreign-project, and unsupported images produce an empty,
unavailable view.
The original images remain available for recovery. Committed streaming
responses reopen as incomplete. The history owner requires its own host text
filter and keeps only admitted display text, numeric usage, and opaque typed
attachment and tool references. Provider request envelopes and tool payloads
remain ephemeral.
Version 1 images remain readable. The next durable write publishes the current
version, which adds a build output reference kind.

The Local Model panel opens the committed project's history on the editor I/O
lane. After a plain text exchange, the panel shows the exact redacted prompt
and response before offering a save. Approval writes both messages as a single
project-scoped operation. The editor owns the background model operation and
continues to poll history and turn completion while the panel is hidden.
Detaching the workspace cancels the operation and discards late replies.

The panel also offers recovery for an explicitly approved plain-text request.
It commits the reviewed, redacted prompt and an empty response checkpoint on
the history worker before starting inference. While a plain response streams,
the user can freeze and approve a redacted draft of the complete lines received
so far. Later deltas never change that preview. A successful reply requires its
own review before the panel completes the same checkpoint. A restart after an
interruption shows the saved prompt, any approved draft, and an incomplete-response
marker.

Saved conversation management uses the same project worker and is bound to the
selected conversation. The panel supports rename, archive, restore, and deletion
after confirmation. An export reads the sanitized JSON projection. The user can
copy that exact preview from the panel.

History is searchable from the chat panel. Old conversations are
auto-archived after 30 days (configurable).

### Context Injection

The Local Model panel can review a Source document chosen from the open tabs,
the active Source document, or the explicit selection in its existing editor widget.
The workspace supplies an immutable editor snapshot, without opening or
reading a file. The panel shows the redacted bytes and local destination
before issuing one-use approval. Range approval also checks that the same
selection still exists. Redaction checks any intersecting sensitive line
and enclosing private-key block before clipping to the selected range.
Source edits, close, and workspace detachment invalidate the preview before
provider dispatch. After a successful contextual turn, a separate history
approval records only the displayed numeric document or build output reference.
An executed scene count tool can be recorded through the same review. These
conversation records contain localized host labels and an opaque reference
scoped to the turn.

The separate saved-file action takes an explicit project-relative path. The
workspace's rooted read authority reads a bounded text file on the agent worker
without opening a Source tab. The panel previews the redacted result and the
inference worker verifies the same saved revision before using the approval.
These saved-file attachments and responses remain ephemeral.

The user can explicitly capture bounded clipboard text as a note or report.
The panel freezes the selected bytes at capture, identifies the attachment type
and revision, and displays the redacted preview before consent. Later clipboard
changes have no effect on the frozen preview. The clipboard is read only on
that selection, and the attachment and response remain out of conversation history.

The panel offers separate previews for the active gameplay build and the last
completed build of the current project. It reads only the retained output for
the selected operation, marks omitted output, and redacts the text before review.
The active and completed builds also offer separate diagnostics previews. The
selected operation contributes a bounded capture of its warnings and errors to
an authenticated, read-only MCP session on the agent worker. The session exposes
only the project's diagnostic query for that capture and is closed before model
inference. The worker returns the revisioned page for redaction and review.
The active Decision Graph can expose its current Problems through the same
query. The workspace supplies only bounded findings from that open document and
revokes the review when the active document, its revision, or its validation
result changes.
Changes to the selected operation's output invalidate its approval. Other
builds and process logs have no effect while the selected output is retained.

An inline source proposal review shows error and warning counts for the open
source revision behind the proposal. The counts come from the existing language
service publication. They describe the current buffer and never certify the
proposed replacement text. When the source changes, the original review
conflicts and its counts are withdrawn.
After the user applies a review, an open source tab can show the language
findings for its exact applied buffer. A later edit retires this association.
The buffer remains in the source owner and applying the review grants no build
or test authority.
The chat review can also check the syntax of a proposed HoroScript file before
application. The bounded Lua parser runs on a worker against the immutable
preview, without opening the script's libraries or executing it. The result is
visible only for that exact proposal plan and current file base. The check
leaves the review decisions and workspace files untouched.
For C++ files, the same review can ask the host's configured Clangd to check
the proposed bytes against the project's compilation database. A separate
language process owns the one-file preview, with a deadline and indexing
disabled. The result describes that file's compilation context and stays within
the review. It does not replace the open editor buffer's language publication.

The workspace can also capture up to sixteen selected IDs from the current
ordinary Scene for a separate local-model review. The capture identifies the
scene and selection revisions. A scene edit, replacement, or selection change
revokes its witness before the next dispatch. It stays out of conversation history.

Other intended editor context surfaces include:

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

The local chat panel displays completed, redacted lines while an inference
without tools is running, including turns with explicitly approved attachments.
The display polls the bounded worker draft without waiting for the producer.
Final delivery uses the terminal response. A live display does not approve
conversation storage or create a history checkpoint.
Regenerate resubmits the last plain prompt to the same selected model and
endpoint. The separate composer keeps its current draft. Context-bearing and
tool turns require a new explicit selection and approval.
The panel reports when the approved scene-count tool starts and how it settles.
This transient status is emitted only for admitted dispatch and carries no tool
arguments or result payload. The query can finish before the assistant response.
The chat's text fields own workspace keyboard focus only while active. Hiding
the panel releases that input context while the editor continues polling the
turn. Actions use the dock's available width and expose clipped labels through
focused and hovered tooltips.

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

The command host registers `viewport.propose_light_state` in the same immutable
tool catalog as the inspection and Scene review tools. The tool requires both
inspection and proposal grants, the exact Scene revision, and a bounded list of
visible object identities. An optional world-space stroke restricts that list to
committed pivots inside its swept radius. It accepts at most 16 points, a 100 m
radius and a 256 m total stroke length. An out-of-scope target rejects the whole
request. The review uses typed light edits and the same local Apply, discard,
history and authority checks as other Scene proposals.

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
- An explicit viewport context request includes at most 16 selected objects.
  Camera-only context skips authored transform lookup. When transforms are
  requested, the workspace resolves the bounded selection in one pass over the
  authored objects, preserves selection order, and revokes the snapshot when
  the selection changes.
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

### Disabled Viewport Agent Frame Cost

With the Local Agent panel disabled, the workspace leaves the viewport review
callback empty. The viewport frame skips the review control and gathers no
agent context.

The opt-in `[viewport-agent-frame-cost]` case in `HoroViewportPanelRenderTests`
times `ViewportPanel::DrawPanel` with an 800 × 500 logical viewport, 4,096
viewport model entries, and a ready test renderer. It warms up 100 frames and
reports the mean of 10,000 timed frames. Invoke the test binary with
`'[viewport-agent-frame-cost]' -s` to see the timing and draw counts.

On macOS arm64 in Debug, 11 alternating pairs compared the current disabled
path with the same panel compiled with the offer predicate fixed to false and
the agent button and action dispatch removed. The baseline median was 40.13 µs
per frame (40.00–45.69 µs across runs). The disabled median was 40.25 µs
(39.99–40.98 µs), with a median paired difference of +0.049 µs. Both produced
1,506 vertices and 5,229 indices. The difference was below the observed
run-to-run variation. This fixture measures panel CPU work and leaves GPU
frame cost and native renderer parity to backend qualification.

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
