# ADR-073: Runtime UI Ownership, Scope and Update Order

- **Status**: Proposed
- **Date**: 2026-09-02
- **Supersedes**: None
- **Scope**: Runtime UI service ownership, game/player/scene/viewport scopes, instance identity, activation, frame update order, input, pause/suspension, render extraction, unload, failure, compatibility, and shutdown
- **Issue**: [RUI-001.1](https://github.com/HoroCore/horo-engine/issues/698)
- **Jira**: [HORO-698](https://horo-engine.atlassian.net/browse/HORO-698)
- **Parent**: [RUI-001](https://github.com/HoroCore/horo-engine/issues/700)
- **Related**: [ADR-004](004-cli-core-gui-boundary.md), [ADR-033](033-presentation-and-display-ownership.md), [ADR-054](054-extension-and-package-authority-boundary.md)
- **Normative documents**: [Game UI and HUD](../architecture/runtime/game-ui-and-hud.md), [Runtime Lifecycle](../architecture/runtime/runtime-lifecycle.md), [Scene Runtime](../architecture/runtime/scene-runtime.md), [Input Architecture](../architecture/runtime/input-architecture.md), [Rendering Architecture](../architecture/runtime/rendering-architecture.md)

## Context

Runtime game UI includes global menus, per-player HUDs, scene-owned overlays and
world-space canvases, and viewport-local presentation. Those objects can have
different lifetimes even when they render into the same view. Treating all UI as
scene-owned destroys loading/menu/player UI during transitions; treating it as one
process-global world lets scene or player data survive past its owner. Treating a
viewport as the semantic owner of every canvas couples content to presentation.

Runtime UI also crosses input, simulation, variable update, layout, render
extraction, presentation, pause, hot reload, scene unload, editor preview, and
shutdown. Without one phase contract it can read mutable ECS state, hit-test a
layout the player never saw, render from editor widgets, or retire assets while an
in-flight frame still uses them. The system needs typed Horo-owned scopes and one
generation-safe lifecycle before document, layout, text, rendering, and authoring
children are implemented.

## Decision

### 1. RuntimeUiService is the sole runtime UI authority

The application/game runtime owns one `RuntimeUiService` per game-instance
runtime. The service owns UI scope registries, runtime element instances, focus/
interaction state, binding and layout snapshots, lifecycle transactions, viewport
attachments, render extraction, diagnostics and retirement. It uses narrow Assets,
Localization, Accessibility, Input and Renderer contracts; it does not own their
registries, native objects or policy.

No subsystem creates a hidden global `UiWorld`, mutates UI elements by service
locator, or renders game UI directly. Scene, gameplay, player, viewport, editor,
CLI and MCP code submit typed commands/queries through application capabilities.
Runtime UI never includes or exposes ImGui, SDL, OpenGL, Metal, Vulkan, D3D12,
native-window, editor-widget or renderer-backend types.

Authoring `UiDocument`, cooked document bytes, mutable runtime instance state,
immutable interaction/layout snapshots and `UiRenderSnapshot` are distinct
objects with distinct owners. Editor authoring state is never a runtime instance.

### 2. Every runtime instance has exactly one semantic owner scope

`UiOwnerScope` is a closed typed variant:

```cpp
enum class UiOwnerScopeKind : std::uint8_t {
    GameInstance,
    Player,
    Scene,
    Viewport,
};
```

| Scope | Owner identity and lifetime | Examples |
|---|---|---|
| `GameInstance` | One game-runtime generation; survives scene/player/viewport replacement and ends before that game runtime | boot/loading UI, main menu, global settings, transition cover |
| `Player` | One generation-checked local-player/session identity; may survive scenes and attach to one or more admitted viewports | HUD, inventory, ability bar, player pause/menu state |
| `Scene` | One exact `SceneRuntimeId` generation; activates and unloads with that scene context | scene objective panel, world-space nameplates, scene tutorial overlay |
| `Viewport` | One exact Horo viewport/presentation attachment generation; ends on viewport destruction/replacement | per-view camera overlay, split-screen mask, viewport-local safe-area surface |

An instance chooses one scope at creation and cannot silently migrate. A game-
instance or player instance attached to a viewport remains owned by its original
scope; attachment does not transfer ownership. A scene entity may supply a stable
binding/anchor reference, but UI elements are not ordinary ECS entities and do not
reuse `EntityId` as their identity.

Cross-scope relationships use stable typed references and owner-approved queries.
They do not retain pointers to another scope's tree/state. Scope destruction
invalidates every runtime handle in that scope before its slot can be reused.

### 3. Runtime identity is generation checked and authoring identity is stable

Each runtime tree has a `RuntimeUiInstanceId` and every element is addressed by a
`UiElementHandle` containing service/runtime/scope generation plus slot generation.
Handles are transient and never serialized. Commands name the expected scope,
instance, element and document revisions; stale or cross-scope handles fail.

Both IDs use a fixed 128-bit value: one nonzero 64-bit `UiOwnershipGeneration`
uniquely identifies the service/runtime/scope incarnation, followed by a 32-bit
slot and a nonzero 32-bit slot generation. Zero is invalid. Slot generations use
checked increment; releasing a slot at `UINT32_MAX` retires that slot permanently
instead of wrapping. Ownership generations also use checked increment; exhausting
`UINT64_MAX` closes new Runtime UI admission with `GenerationExhausted` and requires
host-owned runtime replacement. No counter wraps, saturates into a reusable value,
or reuses an identity still observable by a command, snapshot, render epoch, or
lease.

Element slots form one owner-wide namespace, not a tree-local namespace. Before a
tree is created, the owning Runtime UI service monotonically reserves a disjoint
contiguous slot range equal to that tree's declared element capacity. The range is
tombstoned until its `UiOwnershipGeneration` ends even if tree preparation fails or
the canvas retires. A tree maps its private indexes into that range and rejects
handles outside it before checking the local slot generation. This preserves the
fixed 128-bit representation while preventing two canvases in one scope from
aliasing the same local slot and generation. Range exhaustion closes new tree
admission with `GenerationExhausted`; ranges never wrap, hash, overlap, or reuse.

Authored `UiDocumentId`/`AssetId` and stable `UiElementId` values survive cook,
reload and runtime instantiation. They are used for diagnostics, binding, saved
semantic state and explicit reload reconciliation, not as mutable pointer/slot
indexes. Duplicate, missing, malformed or type-incompatible IDs reject the
candidate. Runtime focus, hover, press, capture, animation cursor, layout boxes and
renderer resources are not authoring data.

Modal activation slots identify their actual retained modal-root element namespace,
not nesting depth. A graph-wide EVER-issued incarnation counter distinguishes
repeated/deep activations; popping and graph reload do not reset it. Route
activation slots identify their never-reused stack namespace, with an EVER-issued
activation incarnation. Complete asset reload requires that stack namespace to
be backed by the allocator-issued canvas-root element slot. These reservations
include unused and tombstoned capacity, and remain burned after cancellation,
retirement and final lease drain. Exhaustion refuses new admission before
mutating the active generation. This prevents a raw old popped modal/route
handle from aliasing a replacement owner even when the semantic player/layer
scope stays the same; a coordinator publication fence alone cannot provide that
invariant. See [Runtime UI hot reload migration](../guides/runtime-ui-hot-reload-migration.md).

### 4. Scope and instance lifecycle are explicit transactions

The service state and each scope/instance use named generations and states:

```text
Created -> Preparing -> Ready -> Activating -> Active
Preparing | Activating -> Failed
Active -> Deactivating -> Retiring -> Destroyed
Ready -> Retiring -> Destroyed
```

Preparation resolves the cooked document, schemas, styles, fonts/images/materials,
localization and binding descriptors; validates limits and capabilities; builds a
private element tree; allocates bounded runtime state; and prepares renderer/input
descriptors without publishing them. Activation publishes a complete instance at
an owner-thread safe point only after its semantic owner exists and every required
dependency is ready. Failure unwinds the candidate and preserves the prior active/
last-good generation.

Visibility, enabled state, input eligibility, pause/time policy and viewport
attachment are orthogonal typed state; they are not lifecycle aliases. Hiding an
instance does not destroy it, pausing gameplay does not deactivate it, and a
temporarily unavailable viewport does not transfer it to another owner.

[ADR-080](080-runtime-ui-presentation-scope-layer-and-route.md) projects these
owner scopes into typed presentation bands and transactional route stacks without
changing ownership or lifetime authority.

Scope create/destroy, document replacement and cross-tree structural transactions
commit during `ApplyQueuedOwnerThreadCommands`, or at the next
`CommitDeferredLifecycleChanges` when requested after that frame's command cutoff.
No scope/tree generation changes during layout, extraction or rendering.

### 5. Runtime UI has one canonical frame order

Runtime UI participates in the existing `RuntimePhase` sequence and adds no phase:

1. `BuildInputSnapshot` commits normalized device/action/text evidence.
2. `ApplyQueuedOwnerThreadCommands` commits admitted scope lifecycle, document/
   resource completions and prior structural transactions.
3. `FixedUpdate` advances gameplay/physics only. Runtime UI does not mutate trees,
   animate presentation or dispatch UI actions from fixed simulation.
4. During `VariableUpdate`, Runtime UI:
   1. reads immutable committed gameplay/application binding snapshots;
   2. routes the current input snapshot against the last successfully presented
      `UiInteractionSnapshot` for that player/viewport;
   3. emits typed UI actions and commits one bounded UI-local state transaction;
   4. advances declared presentation animation/time policies;
   5. resolves bindings, measure/arrange, focus/navigation and hit-test state;
   6. publishes one immutable instance/layout/interaction generation.
5. `RenderExtraction` projects active instances into immutable per-view
   `UiRenderSnapshot` values using the published generation only. Each exact view
   owns a bounded preallocated extraction store; publication performs a bounded
   free-slot scan and returns typed exhaustion instead of blocking, overwriting an
   in-flight snapshot, or allocating fallback frame storage. Snapshot copies retain
   one immutable slot until the last render lease retires.
6. `RenderExecution` renders world-space canvases through declared world passes
   and screen-space canvases at the frontend-owned composition point.
7. `RenderGui` remains host/editor/development GUI composition; runtime game UI
   does not become ImGui work merely because an editor hosts the game viewport.
8. A successful presentation adopts the extracted interaction revision as the
   next frame's hit-test/focus evidence. A transient skipped presentation that
   leaves the prior image visible continues to route input only against the last
   successfully presented snapshot; it never hit-tests the newer unpublished
   layout. Interaction is disabled when no such snapshot exists, its owner/scope/
   attachment generation has retired, or output invalidation means the host cannot
   assert that the prior image remains visible. A later matching presentation
   re-enables interaction and advances the adopted revision.
9. `CommitDeferredLifecycleChanges` retires detached/removed generations after
   frame ownership is closed; `EndFrame` publishes bounded metrics/observations.

The last-presented interaction rule makes input target the geometry the player
actually saw. UI actions may mutate UI-local state in the same VariableUpdate, but
gameplay/scene/renderer mutations are typed commands for their owner's next safe
point. A button callback never writes ECS, renderer or asset state directly.

### 6. Pause, step, focus loss and suspension are distinct

Gameplay pause stops fixed simulation but keeps Runtime UI lifecycle, input,
VariableUpdate, layout, rendering and required services active. UI bindings observe
the latest committed gameplay state.
[ADR-077](077-runtime-ui-animation-clock-and-time-domain.md) replaces this ADR's
provisional three-value `UiTimePolicy` sketch with the normative Simulation,
PresentationUnscaled, ScreenTransition, EditorPreview, DeterministicTest and Manual
domain/sample/policy contract. PresentationUnscaled remains the default for menus,
navigation, focus and accessibility feedback. Simulation time freezes when no
fixed tick commits and never multiplies a guessed pause/rate value. Manual time
advances only through an admitted owner command.

A UI action may request pause/resume through an application pause capability, but
Runtime UI does not own pause tokens or set scheduler state. Single-step advances
one complete fixed tick; Runtime UI then runs one ordinary VariableUpdate and
renders the newly committed state without pretending the UI was a fixed system.

Focus loss follows product input/render policy and releases pointer/text capture;
it is not automatic gameplay pause or UI deactivation. Host suspension skips UI
VariableUpdate/extraction/rendering with the rest of presentation. Resume resets
the presentation-delta baseline, revalidates attachments, and never catches up
suspended wall time.

### 7. Input and focus are per player/viewport interaction context

[ADR-078](078-runtime-ui-input-context-and-player-routing.md) is the normative
specialization for audience identity, UI/gameplay/editor priority, modal scope,
consumption, assignment, pointer/text routing, focus/capture and device modality.
Each interactive attachment owns a generation-checked `RuntimeUiInputContextId`
that names game runtime, optional player, viewport, active route/layer and
interaction snapshot revision. It consumes normalized Input Architecture actions,
pointer and text/IME snapshots; it never polls native devices or window APIs.

Packaged-game routing places the top runtime UI modal/screen context before its
associated gameplay context. Editor-play routing places native dialogs, editor
modals and the focused editor widget/tool before the embedded runtime UI context;
runtime UI cannot consume editor shortcuts outside its focused game viewport.
Capture/focus tokens end on release, focus loss, route/scope/viewport destruction,
modal replacement, device loss, suspension or shutdown.

Focus is scoped, not one process-global element pointer. Split-screen players and
multiple viewports have independent focus/navigation/capture unless an explicit
game-instance modal policy blocks them. Structural mutation reconciles focus by
stable element ID and declared fallback; it never leaves a dangling element handle.

### 8. Viewports own attachments, not foreign semantic state

A `UiViewportAttachment` owns Horo logical viewport/view identity, framebuffer and
logical extents, DPI/safe-area snapshot, render mode/layer, input region, player
association, visibility and presented interaction revision. Native window/surface/
swapchain/image handles remain Platform/Renderer private.

Game/player/scene instances attach through generation-checked descriptors and may
be projected into multiple compatible views without duplicating semantic state.
Viewport-scoped instances are destroyed with their attachment. Resize, DPI/safe-
area, output, render-view or backend changes prepare a new attachment/layout/render
generation and atomically replace the old one; they do not mutate a snapshot in
flight.

Screen-space overlay UI composes after world/display transform unless a declared
render plan places it earlier. World-space UI produces backend-neutral render
instances under the scene/view depth and visibility policy. Runtime UI never issues
native commands, presents, selects a renderer/backend, or owns GPU retirement.

### 9. Scene activation and unload have a UI barrier

Scene-scoped UI preparation may run with the scene candidate, but activation waits
until the exact `SceneRuntimeId` generation and required assets/bindings are ready.
It publishes before the first eligible Runtime UI VariableUpdate/extraction for
that active scene. A failed required UI candidate prevents the owning scene/product
activation according to explicit policy; optional UI omission is declared and
observable, never inferred from load failure.

Scene UI reads immutable binding/anchor snapshots identified by scene/object/
component generation. It retains no ECS component-pool pointers and cannot perform
world queries during layout/rendering. Scene replacement does not destroy game-
instance or persistent player scopes. Player removal does not destroy unrelated
scene/global UI; viewport replacement does not destroy foreign-owned instances.

Unload closes new commands/input for the scope, cancels/joins preparation and
binding work, releases focus/capture, publishes removal for every attachment,
waits for UI/render snapshots and resource leases to retire, destroys runtime
state, then releases assets. Late completions and stale handles are rejected by
generation. Repeated unload is idempotent.

### 10. Editor preview is isolated from both editor UI and play state

HoroEditor owns `UiDocument` tabs, dirty/save state, selection, undo/redo,
inspectors, node/canvas presentation and preview controls. A preview creates an
isolated game runtime and ordinary typed Runtime UI scopes/attachments. It uses the
same cook/load/activation/input/layout/extraction/render path as a packaged game.

Editor ImGui IDs, widget pointers, docking, selection, gizmos and transient style
state never enter the document or runtime. Runtime mutations do not edit the
authoring document. Applying an admitted preview/play change back requires an
explicit revision-checked editor command with undo. Closing the tab/project or
stopping preview follows the normal scope retirement barrier.

### 11. Compatibility, errors and reload preserve the last good generation

Documents, cooked payloads, element/property descriptors, commands, snapshots and
capabilities are versioned Horo-owned schemas. Unknown required element/property,
duplicate IDs, malformed trees, dependency/type mismatch, limit overflow,
unsupported render/input/layout capability, stale owner, or migration failure
returns a typed error with stable document/scope/element evidence. Backend/vendor
error enums and editor display strings are translated only at adapters.

Hot reload prepares a complete replacement. Stable authored element IDs may
reconcile only explicitly compatible state such as focus, scroll or declared
control value; runtime handles, renderer resources, captures and arbitrary widget
memory never migrate. Publication swaps one complete generation at a safe point.
Failure retains the old generation and diagnostic evidence.

Composition is explicit: `Omitted`, `ModelOnly`, or `Rendered`. Headless tools may
use `ModelOnly` for validation, bindings, layout and Null extraction without a
window/GPU/input attachment. A product requiring visible/interactable UI fails
capability preflight if only model/omitted composition is available; no hidden
window, editor GUI or backend fallback is created.

### 12. Shutdown stops producers before dependencies

Shutdown closes external/editor/gameplay UI command admission and all input
contexts first. It deactivates scene, player, viewport and game-instance scopes;
cancels/joins document/resource/binding jobs; publishes final attachment removals;
waits for UI extraction/render epochs and leases; destroys runtime trees and UI
resource realizations through their owners; then releases RuntimeUiService before
Renderer, Assets, Input, Localization or Platform dependencies disappear.

Unexpected failure follows the same bounded retirement where safe. Partial startup,
failed activation, no viewport, lost renderer and repeated stop are idempotent. A
timeout or surviving render/resource lease reports terminal/pending failure and
retains required owner state; it never force-frees memory visible to another frame.

### 13. Migration and verification

Game UI and HUD adopts this four-scope owner model and canonical phase order.
Runtime Lifecycle records Runtime UI participation without adding phases. Scene,
Input and Rendering documents project their scope, context and snapshot boundaries.
RUI-001.2 and later children define exact document/tree schemas, structural
commands, cook/reload and validation without creating another runtime UI owner.

Required contract coverage includes:

- game-instance/player/scene/viewport scope creation, exact owner identity,
  persistence across unrelated owner replacement and no silent scope migration;
- duplicate/missing/malformed stable IDs, stale/cross-scope handles, slot reuse,
  generation wrap policy and actionable typed diagnostics;
- partial dependency/asset/style/font/localization/binding failure, transactional
  activation, required versus optional policy and last-good retention;
- deterministic owner-command/VariableUpdate/layout/extraction order across zero/
  multiple fixed ticks and 30/60/144 Hz render cadence;
- input against last-presented layout while a transient skip preserves its image,
  unpublished-layout rejection, invalidated-output suppression, UI-local
  transaction visibility and cross-owner command deferral;
- gameplay pause, nested pause tokens, single-step, UI time policies, focus loss,
  host suspension/resume and no suspended-time catch-up;
- split-screen/multiple viewport/player focus and capture, editor-play priority,
  modal blocking, text/IME, device/focus loss and scope destruction;
- screen/world-space extraction, multi-view projection, resize/DPI/safe-area/
  output/backend replacement and no native/editor types in public snapshots;
- scene activation/unload/replacement with pending work, player removal, viewport
  loss, game transition cover persistence, late completion and repeated unload;
- hot reload state reconciliation by stable authored ID with old runtime/render/
  input leases retired before reuse;
- Omitted/ModelOnly/Rendered composition, headless Null extraction and required-
  capability failure without a hidden window/GUI/backend;
- editor preview isolation, explicit apply-back command, tab/project/play stop and
  no editor transient state in documents/runtime;
- partial startup, renderer loss, shutdown with active scopes/jobs/captures/frames,
  terminal lease timeout and repeated shutdown.

## Consequences

Global menus, persistent player HUDs, scene overlays and viewport-local surfaces
can coexist without sharing lifetime or presentation ownership. UI actions target
what was actually presented, fixed simulation remains deterministic, pause menus
stay responsive, and renderer/editor/native state stays behind typed adapters.

The cost is explicit scopes, lifecycle transactions, prior-presented interaction
snapshots, phase cutoffs, generation/lease tracking and separate authoring/runtime/
render objects. These are required to prevent stale input, dangling scene UI and
mid-frame mutation.

## Rejected Alternatives

### Put every UI instance in the active scene

Rejected because main/loading UI and persistent player HUDs must survive scene
replacement, while scene-owned world UI must not outlive its scene.

### Make the viewport own every canvas it displays

Rejected because view attachment is presentation, not semantic ownership. Resize,
split-screen or renderer replacement must not silently destroy game/player state.

### Represent UI elements as ordinary ECS entities

Rejected because document/tree identity, focus/layout mutation and UI scope lifetime
differ from scene entity lifetime. Typed bindings connect them without aliasing IDs.

### Update Runtime UI in FixedUpdate

Rejected because pointer/text navigation and presentation animation would depend on
fixed catch-up count and pause. UI reads committed simulation results in
VariableUpdate and emits commands for owner safe points.

### Hit-test the newest unpublished layout

Rejected because the player may click geometry that was never presented. Input uses
the last successfully presented interaction revision.

### Render Runtime UI through ImGui in editor play

Rejected because packaged games, headless tests and renderer peers need the same
retained Horo snapshots, and editor widget state cannot be runtime identity.

### Let UI callbacks mutate gameplay or renderer state directly

Rejected because it bypasses owner phases, validation, undo/security and lifetime.
UI emits typed actions/commands to the authoritative owner.
