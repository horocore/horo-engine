# Game UI And HUD Architecture

## Purpose

This document defines Horo Engine's runtime game UI system: menus, HUDs,
overlays, in-game screens, focus/navigation, input routing, rendering,
serialization, templates, editor authoring, and package boundaries.

Game UI is runtime game content. It is not the same system as HoroEditor panels,
tabs, modals, inspectors, or the editor design-system widgets.

[ADR-161](../../adr/161-xr-interaction-runtime-ui-locomotion-and-accessibility-ownership.md)
keeps XR world-space interaction inside this same Runtime UI authority. XR adapters
supply generation-scoped ray/direct/proximity evidence, but Runtime UI owns presented
hit testing, focus, capture, semantic action and command production. Renderer owns
per-view projection/occlusion/pixels, and neither can create a second XR widget tree.

[ADR-073](../../adr/073-runtime-ui-ownership-scope-and-update-order.md) is the
single normative owner of RuntimeUiService, game/player/scene/viewport scopes,
instance lifecycle, frame update order, pause/suspension, input/presentation
revisions, unload, compatibility and shutdown.
[ADR-074](../../adr/074-runtime-ui-layout-units-and-measure-arrange.md) owns the
logical unit, box-model, constraint-precedence, measure/arrange, overflow and
deterministic-rounding contract.
[ADR-075](../../adr/075-runtime-ui-font-asset-family-and-fallback.md) owns runtime
font source/face/family identity, deterministic face/fallback selection, cook and
platform-discovery boundaries. This document projects those decisions into the
element, authoring and product model.

```text
HoroEditor UI:
  editor tabs, panels, modals, inspector, asset browser, project browser

Game UI:
  main menu, pause menu, HUD, health bar, ammo counter, inventory panel,
  dialogue box, crosshair, interaction prompt, loading screen
```

The editor may provide authoring tools for game UI, but the authored UI itself
belongs to runtime scene/game content.

## Core Decisions

- Game UI and HUD are runtime content, not editor UI.
- One game runtime owns `RuntimeUiService`; every UI instance has exactly one
  game-instance, player, scene, or viewport semantic owner scope.
- HoroEditor authoring tools invoke the same application/runtime UI creation use
  cases as CLI and MCP adapters.
- Game UI input uses the input action system and supports keyboard, mouse, touch,
  and gamepad navigation.
- UI rendering is a declared render pass or overlay pass, not hidden immediate
  rendering from gameplay code.
- UI elements use typed components and stable IDs; serialized UI does not store
  renderer backend handles, live input focus pointers, or editor widget state.
- Templates are presets over core UI elements, not special runtime systems.

## Ownership Boundary

```text
Application / Game Runtime
  +-- RuntimeUiService
      +-- GameInstance scope
      +-- Player scopes
      +-- Scene scopes
      +-- Viewport scopes and attachments
      +-- UiDocument/runtime-instance registry
      +-- UiLayout/interaction snapshots
      +-- UiInputRouter
      +-- UiRenderExtractor
      +-- UiBindingStore
```

Each runtime instance chooses one scope and cannot silently migrate. Game-instance
UI such as loading/main menus survives scene replacement. Player UI survives only
with its generation-checked local player/session. Scene UI ends with its exact
`SceneRuntimeId`. Viewport UI ends with that logical viewport attachment. Attaching
game/player/scene UI to a viewport does not transfer semantic ownership.

UI elements have stable authored `UiElementId` values and transient generation-
checked runtime handles; they are not ordinary ECS entities. HoroEditor owns only
authoring documents, previews, inspector state and revision-checked editor commands.
It never owns or lends widget pointers to a game runtime instance.

The Runtime UI owner reserves disjoint, non-reusable element-slot ranges for every
retained tree in one ownership generation. Tree storage remains local and bounded,
but published handles use the owner-wide slot so equal local indexes in two canvases
cannot alias. Destroyed or failed tree ranges remain tombstoned until the owner
generation ends.

### Retained-tree creation migration

`UiElementTree::Create` now requires the owner service's retained
`UiElementSlotAllocator`. Every caller of the former
`Create(descriptor, elements)` overload must keep one allocator for the complete
`UiOwnershipGeneration` and call `Create(ownerElementSlots, descriptor, elements)`.
Creating an allocator per tree is invalid because it would restart the owner-wide
namespace and permit cross-canvas handle aliasing. Failed preparation burns its
reserved range; shutdown does not make the range reusable.

## Instance Lifecycle And Frame Order

Documents, cooked data, mutable runtime trees, immutable interaction/layout
snapshots and render snapshots are separate generations. Preparation validates and
resolves every required dependency privately; activation publishes one complete
tree at an owner-thread safe point. Failure retains the prior active/last-good
generation. Scope create/destroy, document replacement and structural transactions
commit only at ADR-073 lifecycle cutoffs, never during layout or rendering.

Runtime UI uses the existing host phases:

1. owner commands commit lifecycle/resource/structural work before simulation;
2. fixed simulation publishes gameplay state but does not update the UI tree;
3. VariableUpdate reads committed binding snapshots, routes input against the
   last successfully presented interaction layout, applies bounded UI-local state,
   advances declared UI time, resolves bindings/layout/focus/hit tests, and
   publishes an immutable generation;
4. RenderExtraction creates per-view `UiRenderSnapshot` values from view-owned,
   preallocated bounded stores; live snapshot copies lease their immutable slots,
   and store exhaustion fails explicitly without blocking or fallback allocation;
5. RenderExecution composes world- and screen-space UI; `RenderGui` remains
   editor/development GUI, not the game UI implementation;
6. successful presentation adopts the next interaction revision, then deferred
   lifecycle retirement releases old generations after frame leases close.

UI actions are typed application/gameplay commands. A button handler cannot write
ECS, renderer, asset or scheduler state directly. Failed or skipped presentation
suppresses interaction for that viewport until a matching layout is presented, so
the player cannot click geometry that was never visible.

### Asynchronous action and busy-state lifecycle

The typed action router owns a preallocated `UiAsyncActionStore`; controls own
only copied presentation state. An asynchronous action provider retains one
move-only completion lease in its operation coordinator and owns execution through
structured jobs. Workers may observe its lock-free cancellation token but never
invoke a control, tree or widget, or publish directly into mutable UI state.
Prepared results cross the existing bounded owner-command handoff before
VariableUpdate projects busy/progress/typed failure state into controls.

Screen stacks may own routers by exact route incarnation. Committed close/back/
clear, replacement and shutdown cancel their pending operations before releasing
the route-owned router; cancelled/rejected navigation preserves them. Reload
retires the old router before publishing its new revision context. The first
terminal transition is immutable, and completion leases retain only bounded
operation storage after route/control/tree retirement. Outstanding cancellation
observers prevent slot reuse, so late work cannot target a replacement generation.

Finite queue/operation capacity supplies typed backpressure. Progress is monotonic
within finite numbered phases; immutable failures preserve their original typed
identity and bounded diagnostics. Successful frame operations allocate nothing,
poll no job, perform no I/O and wait on nothing. Provider execution and eventual
drain remain outside UI; UI cancellation does not claim rollback of an
authoritative operation already committed. See the
[asynchronous action migration guide](../../guides/runtime-ui-async-action-migration.md)
for composition, affinity, retention and consumer migration.

## Accessibility projection and change publication

`UiAccessibilityExtractor` copies the existing typed core/contributed semantics
against the exact retained tree. Snapshot nodes now follow retained preorder,
regardless of contributor input order; parents are the nearest exposed semantic
ancestor. Hidden nodes remove their entire subtree. Offscreen propagates to visible
descendants, while covered/suppressed/suspended ancestors prevent descendants from
remaining actionable or focused. Disabled controls remain readable. An exposed
relation targeting an omitted node fails transactionally instead of dangling.

`UiAccessibilityChangePublisher` is an optional Runtime UI owner-thread companion
to that extractor, owned by the same instance/canvas/document composition. It
retains one latest immutable snapshot lease, publishes generation-fenced removals,
insertions, reparent/reorder records, coalesced property masks, authoritative focus
changes and owned polite/assertive announcements. It never consults render nodes,
Platform, gameplay or mutable widget pointers. Consumers correlate PreviousRevision
and Revision with the complete snapshot descriptor; only matching successful
presentation evidence may authorize native exposure. Model-only recording needs
no renderer. Traversal/navigation policy and native adapters are separate work.

Create preallocates node indexes, delta output, text and deduplication history.
Delta overflow discards partial output and publishes one Resynchronize record;
consumers must adopt the complete latest snapshot before further deltas. Suppressed
announcements are remembered and never replayed during resync. Event IDs are scoped
to exact node generations and an inclusive, explicit semantic-revision window.
Unexpired history exhaustion, invalid/stale announcements and source regressions
return typed failures while preserving prior output and the last-good baseline.
Retire publishes final reverse-preorder removals and focus release, closes admission
idempotently and releases its lease; other snapshot leases remain readable.

Migration: former extraction callers must consume snapshot-owned node order rather
than assuming candidate order, and must not refer to hidden nodes from exposed
relations. The new changes header belongs only to HoroRuntimeUi. Existing extractors
and their slot budgets are unchanged; a composed publisher occupies one slot lease,
so at least two concurrent slots are needed for subsequent successful publication.
Reload within the same semantic owner uses the publisher; owner/canvas/document
replacement retires the old publisher and creates a new one. Output borrows last
until successful publication, retirement, owner replacement or destruction.

## Pause, Suspension And Teardown

Gameplay pause stops fixed simulation, not Runtime UI lifecycle, input, layout or
rendering. ADR-077 makes Simulation, PresentationUnscaled, ScreenTransition,
EditorPreview, DeterministicTest and Manual distinct domains; menus/navigation
default to unscaled while gameplay-bound UI advances from committed simulation
evidence. Pause/resume is requested through the application pause capability, not
owned by a UI element. Single-step advances one fixed tick followed by one ordinary
UI VariableUpdate.

Host suspension skips UI variable/extraction/render work and releases interactive
capture according to Input policy. Resume resets presentation delta and revalidates
attachments without catching up suspended wall time. Focus loss is a separate
product policy and does not automatically pause or destroy UI.

Scene/player/viewport/game teardown closes command and input admission, cancels and
joins preparation/binding work, releases focus/capture, publishes removal snapshots,
waits for render/resource leases, destroys runtime state, then releases assets.
Unrelated scopes survive. Shutdown retires all scopes before Renderer, Assets,
Input, Localization or Platform dependencies disappear and is idempotent after
partial activation.

### Persistent UI scene reconciliation

`UiSceneReconciliation` owns actual `UiHotReload` publishers by immutable semantic
scope and exact Scene provider lineage. Complete private same-document runtime
candidates rebind Scene providers without inventing an authored revision; aggregate
publication preserves game/player ownership, retires only the outgoing exact Scene
owners, and admits prepared incoming Scene UI at ADR-073 cutoffs. Required source
loss is explicit, non-scene providers remain unchanged, and old leases drain through
the existing publisher barriers. See the [scene reconciliation migration guide](../../guides/runtime-ui-scene-reconciliation-migration.md)
for host Scene publication ordering, unavailable-source policy and inert identity
target migration. Viewport attachment and audience reconciliation remain independent.

## Presentation Scope, Bands And Routes

[ADR-080](../../adr/080-runtime-ui-presentation-scope-layer-and-route.md) keeps
semantic owner scope, input audience, route-stack membership, presentation band
and visibility as independent typed dimensions. Persistence derives only from the
ADR-073 GameInstance/Player/Scene/Viewport owner; moving content between visual
bands or covering it never transfers ownership or extends lifetime.

Core presentation order is fixed from World, HUD, Screen and Overlay through Modal,
Loading and Debug. Backends may batch within the published plan but cannot reorder
these semantic bands. Game, player, scene and viewport route stacks remain
independent, with explicit cross-stack cover/arbitration for modal and loading
presentation instead of implicit process-global z values.

Push, pop, replace and cross-stack operations prepare privately, then commit or
roll back atomically at ADR-073 lifecycle cutoffs. Routes move through Entering,
Visible, Covered, Suppressed, Suspended, Exiting and Retiring visibility states
without making visibility a lifetime alias. A route becomes input-eligible only
after its matching interaction revision is successfully presented.

Transition loading is GameInstance-owned so it survives scene replacement and can
cover recovery. Debug presentation is profile-gated, non-authoritative and cannot
gain hidden input or gameplay mutation authority.

## Core Runtime UI Primitives

[ADR-176](../../adr/176-runtime-ui-element-and-control-taxonomy.md) is the single
authority for core Runtime UI element kinds, orthogonal capabilities, typed
control semantics, descriptor ownership, validation, compatibility, migration,
and rejected alternatives. This document projects that decision into the retained
tree, layout, input, accessibility, template, and rendering flows; it does not
maintain a second taxonomy.

Game-specific UI is ordinary finite document/template composition. Packages may
extend Runtime UI only through an explicit namespaced extension contract and never
by reinterpreting a core kind or introducing a parallel registry.

## Canvas And Coordinate Spaces

A `UiCanvas` declares:

- render mode: screen-space overlay, screen-space camera, or world-space
- reference resolution
- scaling policy
- safe-area policy
- DPI/font scale policy
- sorting layer and order
- input scope

The canvas does not choose semantic owner lifetime. Its containing runtime
instance already names one ADR-073 owner scope, while each viewport attachment
supplies logical extent, DPI, safe area, view identity and presented revision.

```cpp
struct UiCanvasDescriptor {
    UiCanvasId id;
    UiElementId rootElement;
    UiRenderMode renderMode;
    UiCanvasReferenceResolution referenceResolution;
    UiScaleMode scaleMode;
    UiCanvasPresentationPolicy presentation;
};
```

This descriptor is the coordinate-space foundation. `UiCanvasPresentationPolicy`
is the single typed source for safe-area mode, uniform UI scale, accessibility
font scale, and downstream pixel snapping. The viewport owner publishes one
immutable `UiCanvasViewportEvidence` value containing physical extent, optional
physical-pixels-per-DIP evidence, and physical safe-area insets. Runtime UI
resolves that evidence into `UiResolvedScreenCanvas` without querying Platform,
Renderer, or an editor widget.
Serialized canvases may omit the presentation block for backward compatibility;
the loader supplies the neutral `Ignore`, `1/1`, `1/1`, and `Disabled` defaults.

`UiRenderMode` is the closed `ScreenSpaceOverlay`, `ScreenSpaceCamera`, or
`WorldSpace` vocabulary. `UiScaleMode` is the closed `ScaleWithScreenSize`,
`ConstantPixelSize`, or `ConstantPhysicalSize` vocabulary. For
`ScaleWithScreenSize`, Runtime UI chooses the smaller width/reference-width and
height/reference-height ratio, preserving uniform logical units while exposing
any additional logical extent on a wider or taller viewport. Constant pixel size
uses one physical pixel per DIP. Constant physical size consumes a caller-resolved
positive rational pixels-per-DIP value; Runtime UI does not query a display or
invent DPI evidence.

Screen resolution uses checked integer rational arithmetic and ties-to-even
rounding into the 1/64-DIP logical domain. When safe-area mode is `Inset`, the
resolver keeps the full viewport for scale selection, then publishes a bounded
physical content rectangle and computes the logical extent from that rectangle.
The content rectangle is the layout root's viewport and is also the origin used
by screen hit testing. UI scale is folded into the resolved physical-pixels-per-
DIP ratio; font scale is carried into intrinsic text measurement and shaping but
does not change non-text layout geometry. Resize, monitor/DPI, safe-area, or
viewport changes produce a new immutable resolution input; consumers replace
layout and render generations at their normal safe point.

Physical pixel snapping is downstream derived render data. `Edges` snapping uses
the resolved safe content rectangle and ties-to-even conversion, and may only
change generated render vertices. It never changes logical layout, scroll extent,
serialized values, or hit-test geometry. World-space canvases retain their
authored logical extent in headless/runtime state; safe-area and screen-pixel
snapping are rejected for that mode because camera projection and device pixels
remain Renderer-owned.

Screen-space UI is resolved after world rendering unless a render graph pass
explicitly composes it earlier. World-space UI produces normal render instances
and participates in visibility and depth policy declared by the canvas.

## Responsive Presentation Profiles

[Responsive Presentation Profiles](responsive-presentation-profiles.md) is the
single proposed decision source for constraints plus finite typed layout variants,
deterministic aspect/form-factor/input/split-view selection and neutral fallback.
It owns that decision's limits, per-attachment publication, errors and source/cook
migration; this document does not duplicate its selector or precedence rules.
Profiles preserve the existing semantic tree and ownership and supply effective
layout styles to the ordinary layout engine. The proposal does not claim profile
selection or the complete RuntimeUiService is implemented on current main.

## Element Tree And Layout

UI content is a retained tree:

```text
Canvas
  +-- Screen
      +-- Frame
          +-- Text
          +-- Button
          +-- ProgressBar
```

Elements own stable authoring IDs. Layout is computed from typed constraints:

- `Auto`, logical `Dip`, or same-axis `Percent` preferred lengths;
- margins, border, padding and explicit content-box sizing;
- min/preferred/max size and positive aspect ratio;
- anchors, offsets and alignment;
- flow, flex and grid allocation;
- overflow, clipping and scroll policy.

`Intrinsic` is a revisioned measurement source for `Auto`, not a serialized length
kind. Runtime layout uses signed fixed-point `UiScalar` values at 1/64 DIP. Checked
arithmetic, ties-to-even division and stable authored-order remainder distribution
make logical boxes independent of frame rate, thread schedule and renderer backend.

Constraint precedence is fixed: validate; take a parent-assigned or authored
definite size; otherwise measure intrinsic `Auto`; derive the one remaining auto
axis from aspect ratio; clamp min/max with minimum winning an inverted bound; then
apply parent allocation/alignment and compute boxes/overflow. Property edit or
serialization order never changes the result.

Anchors define a containing segment. Two anchors plus auto stretch, one anchor plus
auto uses intrinsic size, and definite size is aligned within the anchor segment.
Absolute/anchored elements do not contribute to flex/grid flow but do contribute
to post-arrange overflow.

Layout has two logical phases:

```text
Measure(available range)
  -> intrinsic min/preferred/max contributions
Arrange(final content rectangle)
  -> final boxes, overflow, clip chains and interaction geometry
```

An indefinite parent percentage behaves as auto during intrinsic measurement and
may trigger one bounded arrange-time remeasure after the axis becomes definite.
Further change is a typed non-convergence failure. Flow, flex and grid share the
same unit/min/max/aspect precedence; flex freezes items at bounds while
redistributing space, and grid resolves definite, intrinsic, then fractional tracks
with stable remainder order.

Arrangement publishes one complete immutable `UiLayoutSnapshot` for rendering and
hit testing. Required failure publishes nothing and retains the prior last-good
generation according to ADR-073. Gameplay code does not mutate boxes or render
quads directly. Physical pixel snapping is downstream derived render data and
cannot feed back into logical layout, scroll extent or serialized state.

### Clipping, Scroll Content And Bring-Into-View

Runtime UI derives clipping and scrolling from the exact immutable layout
generation through `UiLayoutClipEngine`. Each arranged element supplies one typed
overflow policy: `Visible` contributes no clip, `Clip` establishes a descendant
clip, and `Scroll` establishes both a clip and a bounded scroll record. Scroll
content is the checked union of the arranged content box and descendant overflow;
signed offsets are clamped to the resulting leading/trailing bounds before they
are projected into canvas-space translations.

The published projection stores ancestor-ordered clip nodes, source-aligned
element records and immutable scroll records. Nested scroll translations are
accumulated in tree order, and a focus graph's generation-fenced
`UiFocusBringIntoViewRequest` is resolved from the innermost scroll ancestor
outward. Each intermediate offset is clamped before the next ancestor is
evaluated, so a failed or stale request cannot partially publish nested state.
The focus graph emits the request but never calls layout or scroll code.

Projection publication uses bounded preallocated snapshot slots and retains the
last-good immutable generation when source, capacity or lifecycle validation
fails. Input and rendering consume the published projection; neither recomputes
clip chains, scroll extents or reveal offsets.

## Panel And Frame Contract

`Panel` and `Frame` are core UI building blocks.

```text
Frame
  - background
  - border
  - padding
  - corner radius
  - optional title/header
  - child content area
```

Frames are used for inventory windows, settings pages, dialogue boxes, pause
menus, confirmation dialogs, and other runtime game screens. Editor modals use a
separate HoroEditor modal system and do not share runtime `Frame` state.

## Input, Focus, And Navigation

Game UI consumes normalized input actions:

```text
ui.navigate
ui.submit
ui.cancel
ui.tab_next
ui.tab_previous
ui.scroll
ui.pointer_move
ui.pointer_press
ui.text_input
```

Focus and navigation rules:

- opening a modal-like game screen may block gameplay input according to policy
- focus moves to the screen's declared default element
- gamepad D-pad/left-stick navigation uses the focus graph
- `submit` activates the focused control
- `cancel` closes the current screen or triggers the declared back action
- pointer interaction and keyboard/gamepad focus remain synchronized
- text input is explicit and scoped to focused input fields

A pause menu example:

```text
Pause menu opens
  -> gameplay input is blocked
  -> focus moves to Resume
  -> D-pad navigates controls
  -> South/A submits
  -> East/B cancels
```

The shared `UiFocusGraph` contract keeps this state scoped to one exact runtime
instance, player audience, and presentation layer. A graph copies a complete
bounded candidate of stable element IDs and generation-checked current handles;
it owns neither the retained tree nor renderer/layout state. Authored neighbor
links and declared defaults are resolved deterministically. Missing or stale
targets return typed no-target or recovery outcomes, while modal roots form an
inclusive top-only trap and save a bounded stable-ID restoration path. Reloads
publish a complete candidate before reconciling by stable ID, and retirement
closes admission before releasing focus, modal, and restoration state.

Focus changes may include typed bring-into-view evidence for the scroll owner,
but the graph never calls layout or scroll code. Input consumes the graph's
owner/revision evidence only for the matching last-presented interaction
generation; gameplay action ownership remains a separate contract.

`UiFocusGraph::UpdateLayout` copies hit-test rectangles from the matching immutable
`UiLayoutSnapshot` without retaining a lease or calling layout/scroll services. The
Runtime UI owner admits only last-presented geometry, with exact instance, canvas,
document and tree evidence and equal/newer interaction revision. Updates are atomic,
use preallocated scratch and adopt the accepted interaction revision. Missing exact
handles, including recycled slots, have no spatial geometry. Reload clears geometry
until a matching update, and empty graphs are valid focus scopes. Existing callers
keep their authored links; owners enabling geometry call UpdateLayout after creation
or replacement and after adopting a newly presented layout. No public header ownership
or target dependency changes are required: both contracts remain RuntimeUi-owned.

Cardinal Move uses authored links first. Without a link it searches positive-area
layout boxes inside the active modal, excluding nonfocusable nodes and hidden or
disabled ancestor paths. In the requested center half-plane it ranks perpendicular
overlap, forward doubled-center distance, perpendicular interval gap, perpendicular
doubled-center distance, then stable ID bytes. Integer 1/64-DIP rectangles cannot
represent nonfinite values; negative extents fail layout publication. Signed 64-bit
projection preserves ordering at int32 coordinate extremes without squared distances.
Typed None/Horizontal/Vertical/Both wrap applies only when no forward candidate
exists, choosing the opposite center edge, then perpendicular gap/distance and stable
ID. Next/Previous retain their authored semantics. Invalid declared links retain the
existing recovery policy instead of silently becoming automatic navigation.

Graph creation/reload are bounded owner safe-point preparation operations. Handle
indices and parent indices are prepared there; layout adoption takes O(N + M log N)
bounded work and cardinal search O(N * maximum tree depth), without allocation or I/O.
`SetParticipation` updates focusable/enabled/visible state with exact current owner
and element-generation evidence, recovering invalid focus immediately without
allocation. Structural changes use complete transactional Reload candidates. Failed
updates preserve prior geometry; retirement and shutdown close admission and release
geometry together with focus, modal and restoration storage.

High-frequency pointer movement does not travel through data buses. The UI input
router consumes input snapshots during VariableUpdate through one per-player/
viewport `RuntimeUiInputContextId`. It hit-tests the last successfully presented
interaction revision. Split-screen focus/capture is independent unless an explicit
game-instance modal policy blocks multiple contexts.

After targeting, `UiEventDispatcher` freezes one bounded root path from exact
instance, canvas, document, retained-tree and presented-interaction evidence. It
routes capture from the inclusive root or active modal root toward the target,
invokes the target once, then bubbles over the frozen ancestors. Handled,
propagation-stop and default-prevention are independent typed outcomes; a handled
event does not silently imply either of the other two. An exclusive modal root is
an inclusive route boundary, and a target outside it fails rather than leaking to
the lower tree.

The dispatcher preallocates route storage and is non-reentrant. It retains no
handler, element record or tree reference beyond the synchronous call. Structural
mutation is admitted only through the retained tree's owner safe point; if a
handler commits a replacement or destroys an element, the frozen tree revision is
invalidated, remaining callbacks and the default action are suppressed, and the
caller receives a typed failure. Handler exceptions are contained at the callback
boundary. Retirement closes admission and shutdown is idempotent.

`UiPointerCaptureStore` is the Runtime UI owner of continued pointer delivery. It
preallocates a bounded set of move-only leases keyed by exact input context and
pointer, and copies the initiating button, view, target route, retained-tree
revision and last-presented interaction revision into each lease. It owns no
handler, tree, renderer, platform or gameplay pointer. Release and cancellation
are owner-thread operations; cancellation reasons remain observable on the lease
until its caller releases it, so a cancelled token cannot alias a reused slot.
Focus/device loss, modal replacement, route or target destruction, reload, scene/
scope or viewport teardown, suspension and shutdown must cancel the affected
leases before the next generation can admit delivery; `CancelContext`,
`CancelInstance`, `CancelCanvas` and `CancelView` provide the explicit teardown
boundaries. Reconciliation rejects foreign or stale owner evidence and prevents
capture from crossing contexts or viewports.

## Typed Actions, Commands, And Default Navigation

Interactive controls do not retain callbacks or gameplay references. They emit
typed `UiActionCommand` values through a generation-fenced `UiActionRouter`.
Button, form, route, and gameplay commands each carry a distinct stable action
identity; default navigation uses a closed direction vocabulary and exact
presented focus handles. Arguments are a fixed-capacity typed payload containing
only bounded scalar, UTF-8 text, and Horo-owned identity values.

One admitted request captures its runtime instance, canvas, document, tree and
interaction revisions. The owner rejects foreign or stale evidence before queue
admission. A consumer returns exactly one typed `Handled`, `Rejected`, `Pending`,
`Completed`, or `Cancelled` result correlated to that request. `Pending` carries
an owner-local operation identity; terminal results never mutate the widget or
retain a provider, gameplay, script, renderer, platform, or editor object.

Default navigation results distinguish focus movement, submit/cancel dispatch, and
no-target evidence. A no-target result cannot silently fall through to gameplay;
the input/context owner decides the declared handled or rejected policy. Queue
storage is reserved at router creation, so frame-hot admission and dispatch do
not allocate, block, perform I/O, or invoke another subsystem synchronously.

[ADR-180](../../adr/180-runtime-ui-interactive-control-state-machines-and-default-actions.md) owns
the semantic state machine for core interactive controls. `Button`, `Toggle`,
`Slider` and `TextInput` use closed typed descriptor/state variants with exact
owner and presented-element evidence. Pointer press/release and normalized
keyboard/gamepad submit edges share one pressed transition contract; disabled
controls cannot acquire focus, focus/capture loss and cancellation clear
transient press/repeat/editing state, and text cancellation restores the
edit-session value.

Control handling is a two-step default-action boundary. `Handle` may stage a
`DefaultPending` result, but checked values, scalar values and submitted text do
not commit until the route owner calls `ApplyDefault`; a prevented route calls
`SuppressDefault`. New input is rejected until that decision is made. Repeat is
driven by bounded owner ticks, emits at most one default per admitted tick, and
does not catch up through an unbounded burst. Control descriptors contain no
callbacks, renderer/style state, native handles or gameplay references.

Retirement closes new admission, reload creates a new owner/revision generation,
and shutdown discards queued requests without invoking a consumer. Late results
from an old request or operation are stale and cannot affect a replacement UI
generation. Async operation ownership, progress, busy projection, and
cancellation observation are layered above this result boundary.

[ADR-078](../../adr/078-runtime-ui-input-context-and-player-routing.md) keeps device,
input user, local player, logical viewport and UI context identities separate. Each
context declares a single-player, shared-player, game-instance or unassigned-join
audience and a viewport policy. Routing records every transition's winner/outcome
in a bounded immutable consumption ledger; gameplay receives only the unconsumed
projection for its exact player/tick.

Viewport-, Player- and GameInstance-exclusive modals block associated lower
contexts even when no UI element handles an action; only a finite host-owned safety
passthrough list may bypass them. Assignment change neutralizes old held/captured
input before a new player/context activates. Focus, restoration, capture and active
device modality remain per context/audience rather than process global.

An XR pointer is another generation-scoped Input source for the exact player/viewport
context. It hit-tests the last successfully presented interaction snapshot. Switching
ray/direct modes, tracking or session loss, assignment change, modal/route exclusion,
presentation-revision loss and source destruction release capture and neutralize the
source. A Physics/Renderer hit is evidence only; it cannot set focus or invoke a widget.

### Semantic audio and haptic feedback

The Runtime UI owner may observe completed action/navigation results, focus
changes, applied control defaults, and genuine control cancellation through the
typed `UiFeedbackQueue` contract. These produce `Navigate`, `Focus`, `Confirm`,
`Cancel`, `Error`, or `Boundary` intents. A press that only stages a default, a
suppressed default, an ignored input, stale/retiring action rejection, and
reload/retirement focus recovery do not produce a cue. The owner observes a
semantic outcome once; observing both a navigation completion and its derived
focus change would intentionally create two distinct cues. That is a composition
decision, not hidden deduplication.

Intent values carry the exact presented Runtime UI owner/revision and originating
element; focus-originated intents also carry the per-player/presentation-layer
scope. The owner preallocates a bounded FIFO before frame work. Admission returns
explicit malformed, stale, capacity, or lifecycle failures; it never performs
audio/haptic work, allocation, or I/O. On reload the old queue retires and
discards its stale cues; the new presented owner gets a new queue. Shutdown
likewise discards undelivered optional cues.

At a non-frame-hot host safe point, a borrowed `UiFeedbackRealizer` may map an
intent to optional audio and/or haptic effects. Its per-modality result explicitly
distinguishes skipped, realized, unavailable, and failed. Provider failure does
not roll back or change the UI action, and the queue never owns a provider or
retains a native handle. A host with no audio/haptic capability may simply drain
and discard intents. This is an additive contract: no existing control API or
authored document needs migration. Live host audio/haptic composition is separate
from this Runtime UI contract and must respect its own backend lifetime.

## Runtime Accessibility Semantics

[ADR-082](../../adr/082-runtime-ui-accessibility-capability-and-ownership.md) makes
Runtime UI authoritative for typed roles, accessible state/actions/relationships,
semantic focus and immutable semantic snapshots. Each snapshot records the exact
localization/style/layout/route/interaction generation, and nodes become native-
visible only after matching presentation succeeds.

Native accessibility requests return as revision-checked input commands through
the owning player/audience context; Platform cannot mutate widgets or gameplay.
Configuration owns preferences, ADR-081 owns localized accessible text, and
Renderer owns pixels only. Hidden/modal/covered/offscreen exposure is explicit and
cannot leave lower routes natively actionable.

Support is reported per capability. Keyboard/gamepad navigation and semantic/model-
only output do not imply native screen-reader support. Null reports Unsupported;
Recording is test-only; each native Supported claim requires platform-specific
interoperability evidence.

## Runtime Localization Boundary

[ADR-081](../../adr/081-runtime-ui-and-localization-ownership-boundary.md) keeps
catalog, namespace, locale normalization/policy evidence, typed formatting and
translation fallback in Localization. Runtime UI serializes typed message keys and
argument values, freezes one immutable localization snapshot during VariableUpdate
and publishes resolved text with the matching font/shaping/layout revisions.

Localization returns bounded Unicode text, semantic spans and locale evidence;
Runtime UI Text owns segmentation, bidi, shaping, line breaking and layout. Locale
change events carry revision identity rather than pointers/callbacks. A required
replacement failure retains the last-good complete UI generation instead of mixing
languages or layout revisions in one frame.

Translation, font and localized visual/audio asset fallback remain independent.
Localization supplies the normalized locale chain, ADR-075 selects font faces,
Assets delivers declared stable asset variants, and the owning UI document chooses
Required, UseNeutral or Omit presentation policy.

## Runtime Text And Font Contract

Runtime text/style documents reference stable `FontFamilyAssetId` or typed semantic
font tokens, never filenames, installed-family strings, collection indexes, native
font handles or glyph-atlas slots. A Horo family descriptor owns ordered faces with
weight, stretch, normal/italic/oblique style, variation defaults and explicit
synthetic-style policy.

[ADR-075](../../adr/075-runtime-ui-font-asset-family-and-fallback.md) matches a
requested face deterministically by style compatibility, stretch distance, weight
distance and stable authored order. It expands the primary, family and locale/
script fallback families into one finite acyclic chain and selects the first face
whose immutable coverage supports a complete shaping cluster. Fallback never scans
the operating system or splits a grapheme cluster codepoint by codepoint.

Shipping UI declares `SelfContained` or explicitly capability-gated
`SystemAugmented` font policy. Installed-font discovery is optional Platform
capability data and is never appended as a hidden fallback. Editor preview can show
a discovered face as non-portable, but portable save/cook requires an authorized
tracked font source.

Assets publishes versioned cooked family/face artifacts and dependencies; Runtime
UI owns semantic matching, coverage and logical metrics; later text shaping owns
glyph-run generation; Renderer owns only glyph atlas/raster resource realization.
Font/locale/style/content changes prepare a complete new font/layout generation.
Old face bytes, metrics, shaped runs, layout and atlas resources remain leased by
old interaction/render snapshots until retirement.

Missing required family/artifact fails candidate activation and retains last-good
state. Unsupported later content follows the declared replacement,
omit-with-advance or strict policy with bounded redacted diagnostics; it cannot
perform runtime source I/O, remote download or platform fallback.

## Runtime Style And Token Contract

[ADR-076](../../adr/076-runtime-ui-style-asset-token-and-inheritance.md) defines a
runtime-only typed style model. Runtime elements reference stable style asset,
class and token IDs; they never serialize editor theme keys, `ImGuiStyle`, native
colors, renderer resources or computed-style slots.

Token categories cover linear semantic colors, ADR-074 logical spacing/sizes,
ADR-075 typography, typed imagery, shape/border/shadow, bounded scalar/enum values
and motion references. Aliases must retain the exact category. Style assets and
classes have at most one base; token, asset and class graphs are flattened and
cycle-checked during cook/preparation.

Property precedence is fixed: registered default, root-to-leaf style asset, allowed
parent-element inheritance, element type/default class, stable authored class list,
typed inline value, matching visual-state blocks, then host Accessibility/user
policy. Only explicitly registered typography/text properties inherit through the
element tree; layout, background, border, transform and interaction properties do
not inherit by default.

Visual states are a closed typed mask. Checked/selected, focused, hovered/dragging,
pressed, invalid and disabled/busy overrides apply through stable Selection, Focus,
Pointer, Activation, Validation and Availability layers. Terminal Availability is
the highest state layer, so Disabled/Busy declarations override conflicting Invalid
properties; the host Accessibility/user policy remains the final property overlay.
Within one layer, less specific blocks apply before more-specific blocks and
authored order breaks ties. Style data observes interaction state; it cannot create
it.

Resolution publishes immutable generation-correlated `UiComputedStyle` values.
Measure-affecting changes prepare new layout; paint-only changes still publish a new
render/interaction-correlated style snapshot. Required failure retains last-good
state, and old computed styles/dependencies remain leased until old frames retire.
HoroEditor authors and previews these assets through an isolated runtime adapter;
its own design-system theme is never a runtime inheritance source.

## Animation Clock And Time Domains

[ADR-077](../../adr/077-runtime-ui-animation-clock-and-time-domain.md) replaces raw
variable delta with generation-scoped `UiClockSample` snapshots and six closed
domains: Simulation, PresentationUnscaled, ScreenTransition, EditorPreview,
DeterministicTest and Manual. Runtime UI owns timeline accumulation/lifecycle while
the host supplies committed simulation and clamped monotonic presentation evidence.
Renderer, Platform, skeletal Animation and Sequencer do not advance UI timelines.

Simulation advances only from committed fixed ticks and therefore follows gameplay
pause/rate/single-step without double scaling. PresentationUnscaled is the default
for interactive menus and ignores gameplay pause/time scale. ScreenTransition is a
presentation child clock gated by one exact route/screen generation. Preview is
isolated editor session state; deterministic test and manual time advance only from
explicit ordered commands.

Clock/cursor/rate accumulation uses checked integer/rational arithmetic with carried
remainders. Host suspension holds presentation/transition/preview live playback and
resume starts with zero delta. Multiple viewports, extraction retries or repeated
rendering cannot advance an animation.

Enter/exit transitions may block screen activation/retirement only when declared
required and within finite deadlines. Element/screen/scope removal, replacement,
dependency failure, reload policy or shutdown completes/cancels one generation
exactly once. Reduced-motion policy resolves before publication and can make motion
zero-duration while still completing the lifecycle in the same VariableUpdate.

VariableUpdate freezes one clock snapshot, evaluates timelines once in stable order,
then resolves style/layout/focus and publishes immutable values. Render phases only
consume those values; they never sample time or fire completion markers.

## Rendering Contract

Game UI rendering is backend-neutral and extracted into render data:

```cpp
struct UiRenderSnapshot {
    std::span<const UiDrawCommand> commands;
    std::span<const UiTextRun> textRuns;
    std::span<const UiClipRect> clips;
    UiRevision revision;
};
```

The UI renderer supports:

- colored rectangles and borders
- textured images and sprites
- signed-distance-field or atlas-backed text
- clipping and scroll masks
- opacity and simple transitions
- screen-space and world-space canvas projection

UI render data uses the renderer frontend. It does not call backend APIs from UI
components. Viewport attachments and snapshots contain only Horo view/extent/DPI/
safe-area/resource identities; native surfaces, swapchains, command buffers and
editor GUI texture IDs remain private to Platform/Renderer adapters.

### Render-graph composition admission

`Horo/Runtime/Render/UiRenderComposition.h` is the single typed bridge from an
immutable per-view `UiRenderSnapshot` to renderer-owned graph authoring. The bridge
lives in `RenderFrontend`, which may depend on Runtime UI; Runtime UI never depends
on Renderer and never receives graph resources or pass identities back.

Each admission request names exactly one `UiRenderViewId`, one `RenderGraphOwnerId`,
one output extent, a finite pass bound and an already ordered borrowed pass span.
Every pass binds one immutable snapshot to a canonical graph pass, explicit color
input/output textures and, for world-space UI only, a depth attachment. Texture
shape, sample count, format and usage reuse the existing Horo
`RenderTextureDescriptor`; the UI bridge does not define a competing color-format
vocabulary. Distinct color input/output resources require a sampleable input and an
attachment-capable output. All graph-local references share the request owner.

Composition order is semantic and stable: scene-before-post-process,
scene-after-post-process, then display overlay; within each point the fixed World,
HUD, Screen, Overlay, Modal, Loading and Debug bands apply. World-space UI is a
depth-aware scene pass before post-processing. Camera UI is a scene pass without a
depth attachment. Screen UI may use an explicitly selected scene or display point,
but never gains world-depth authority. The renderer may batch inside one compatible
entry; it may not reorder entries across points or bands.

Validation is a bounded allocation-free scan over caller-owned entries. A successful
call only admits the declarations. The frame owner then retains its own
`UiRenderSnapshot` leases through graph execution and presentation; the borrowed
request is never retained. Empty views are valid. Capacity overflow, mixed views,
foreign graph owners, duplicate snapshot generations, malformed targets and invalid
space/depth policy fail atomically with typed Runtime UI errors.

Renderer produces the renderer-independent `UiPresentationReceipt` declared by
Runtime UI. A receipt is `Presented`, `Skipped` or `Failed` and is correlated to the
exact view, canvas, snapshot and interaction revisions. Runtime UI owns receipt
application. Skipped/failed receipts consume their strictly increasing snapshot
revision but cannot advance the last-presented interaction revision. Only a valid
Presented receipt can make matching hit-test geometry eligible for next-frame input;
stale completion is rejected.

Rejected alternatives:

- Runtime UI does not author `RenderGraph` objects or depend on renderer headers;
  that would reverse the ownership direction.
- Backends do not infer composition from canvas type, texture format or native render
  targets; the host/frontend supplies the explicit semantic point and band.
- A successful graph execution is not treated as successful presentation. Adoption
  waits for an explicit receipt, and skipped/failed frames retain the previous
  interaction revision.
- Admission does not allocate an unbounded owning plan in the frame-hot path. The
  frame owner preallocates bounded storage and retains snapshot leases explicitly.

## Serialization

Game UI is serialized as project/scene content:

- stable UI element IDs
- element type and typed properties
- hierarchy and layout constraints
- style references
- text localization keys
- image/font/material asset references
- binding declarations
- screen route metadata

It does not serialize:

- live focus object pointers
- transient hover/pressed state
- renderer handles
- editor inspector state
- ImGui widget state

## Data Binding

Runtime UI may bind to gameplay state through declared binding descriptors:

```text
Health Bar Template
  = Panel
    + Text "HP"
    + ProgressBar bound to player.health / player.maxHealth
```

Bindings are explicit, typed, and validated. They must not perform stringly-typed
reflection work in hot rendering paths.

[ADR-079](../../adr/079-runtime-ui-binding-provider-schema-identity-and-lifetime.md)
defines stable namespace-owned provider/property IDs, major/minor schema versions
and canonical property fingerprints. Cooked bindings reference a provider type,
property, minimum compatible schema, typed owner selector and read/write direction;
they never store a C++ member/reflection path, mutable pointer, service key or live
provider instance.

Provider type descriptors are finite inert metadata registered as validated batches
by the host composition root. Runtime instances choose exactly one GameInstance,
Player, Scene or Module activation scope and enter through explicit capability/
generation leases. Missing/ambiguous required selectors fail rather than choosing
the nearest/latest provider.

During VariableUpdate, UI freezes one coherent immutable provider snapshot set.
Layout/style/render never call provider getters or retain mutable ECS/module state.
A write-capable property accepts only a typed expected-revision owner command; read
visibility does not imply write authority and UI never receives a mutable setter.

Revocation closes read/write admission, publishes unavailable evidence, cancels
pending work and drains snapshot/command/callback/UI-generation leases before a
scene/player/game/module disappears. Editor preview uses explicit fixture providers
and schema projections, not live runtime pointers or editor widgets.

### Versioned retained binding publication

`HoroEngine::RuntimeUi` owns `UiBindingStore.h`. A host resolves each descriptor
to one exact `UiBindingProviderInstanceId` before preparation; it supplies an owned
schema snapshot, exact provider revision and initial property values. Store creation
copies metadata and values, resolves stable element IDs to current retained handles,
builds property-to-target adjacency, and reserves bounded target text storage.
Required missing values reject preparation; optional missing values use the typed
fallback. No provider object, module pointer, callback or contributor span is retained.

At the VariableUpdate binding cutoff, `UiBindingStore::Apply(tree, batches, layout)`
validates the complete ordered provider delta set before any publication. Each
batch carries exact schema evidence, an expected committed revision and a strictly
newer snapshot revision. Property slots refer only to that exact identity-sorted
schema. Reordered/duplicate providers or slots, missing revision ancestry, malformed
values, stale tree/document/instance evidence and capacity overflow reject the whole
set. Count and aggregate input-byte limits bound frame work. A producer completing
after the cutoff submits its delta at the next cutoff; layout/render never poll it.

Only changed properties visit their compiled subscribers. Equal values and empty
frames produce no dirty work. Text, localized text and visibility changes queue
exact `Measure` invalidations through `UiLayoutEngine::InvalidateBatch`; provider
layout flags may add further measure dependencies. That API validates ownership,
handles, tree revisions and complete queue capacity before replacing its queue.
The binding store publishes target copies and provider revisions only after queue
admission succeeds. The normal layout request uses `Current().content`; this revision
advances only for layout work, so progress, enabled and other paint/action changes
do not trigger the layout engine's global source-change fallback. Ancestor measure
and dependent arrange work remains the incremental layout owner's responsibility.

Text/intrinsic and UI-local state consumers read the store's `Find(tree, binding)`
projection at the same owner phase. It is a synchronous borrow, never a render lease.
Text, layout, accessibility and render publication copy their derived immutable state
into their existing snapshot stores before any later binding mutation. `DrainDirty`
copies accumulated exact binding/element/property work in descriptor order to bounded
owner storage for paint, accessibility and actions; insufficient capacity acknowledges
nothing and never truncates notifications. No new focus/accessibility authority is
introduced by this binding dependency record.

`Unregister` publishes optional fallbacks and removes readable required values,
reports required unavailability for the owning UI activation policy, and permanently
closes that provider incarnation in the store. The host stops provider producers
before unregister and must retry reported layout backpressure or retire the entire
UI generation before releasing its provider scope. Retirement closes all target
reads and update admission; retirement/shutdown abandon pending write reservations
before releasing authority leases and Horo-owned copies. Old downstream
immutable leases remain with their existing owners. Repeated unregister/retirement/
shutdown is harmless. Structural/document reload prepares a new store against the
replacement retained tree and fresh provider evidence; an old store never silently
rebinds a recycled slot or stable ID.

This is an additive publication API. Existing descriptor validation and callers
remain valid. SourceToTarget and TwoWay retain committed provider projections.
TargetToSource requires an explicitly authored typed `initialTarget` in the resolved
descriptor and ignores incoming provider deltas. Store preparation validates this
value against provider and target limits and reports its origin as `UiLocal` until
an accepted commit. Missing/invalid values fail preparation atomically. Readable
directions reject `initialTarget` so the provider projection remains authoritative;
it requires the same explicit write admission as TwoWay. Converter metadata needs an
executable conversion capability and preparation still rejects converters. Provider cadence
is producer-driven for OnChange, EveryVariableUpdate and Manual; none permits a
getter during layout or extraction. `HoroRuntimeUiPublicHeaderConsumer` covers the
new header's isolated target ownership, and binding-store regression coverage drives
the ordinary retained tree, declarative evaluator and incremental layout publisher.

### Two-way write authority and conflict policy

The host explicitly calls `AdmitWrites` with an exact presented owner, authored
action, binding, Change/Blur/Submit trigger and leased `UiBindingWriteAuthority`.
Read/write schema access is necessary but never grants permission. Admission checks
the provider incarnation, semantic scope, complete schema, property slot/signature
and non-reusable host permission incarnation. Descriptor validation stays inert.
The API remains owned by `HoroEngine::RuntimeUi`; no gameplay ABI change is required.

Before editing, `BeginEdit` freezes the committed provider revision and exact
presented element. A routed form/gameplay request supplies bounded typed control
data to `QueueWrite`. The admitted trigger must match. `QueueControlDefault`
previews the real control's staged default, verifies the complete action payload,
admits the write, then applies only its UI-local pending draft. A prevented default
cannot produce a write. Text change/blur owners route the current bounded text
projection through the same form action boundary. `CancelEdit` closes an unqueued
draft; failed admission leaves committed target/provider/layout state untouched.

`RejectStale` is the sole conflict policy. Neither the UI nor the adapter may silently
retry a stale draft against newer state. A fresh edit requires a new capture and
request. Commands retain exact provider/schema/property/capability, complete
document/tree/presented-element evidence, expected revision, request and operation
correlation. Replayed action requests and recycled elements/providers are rejected.

At the explicit shared provider/UI owner safe point, `ProcessWrite` processes at most
one command per call in deterministic round-robin binding order, so a pending producer
cannot starve another binding. The authority validates all fences,
permission, expected revision and domain rules on each preparation/pending poll.
The adapter owns authoritative state and structured jobs; Pending does not transfer
execution to the control. Ready reserves a non-failing commit of exactly the command
value at the next provider revision. The store stages read subscribers and the
accepted target, atomically admits precise layout invalidations through the existing
publisher, then invokes that reserved commit. Capacity, malformed values, provider
rejection/error/cancellation and stale revisions leave committed state unchanged.
The provider cannot reenter UI or publish state during preparation; asynchronous
work returns private evidence for later owner-thread preparation, never UI callbacks.

The new provider capability's `Prepare` boundary is exception-free (`noexcept`),
matching the Foundation public engine module policy. The owned adapter converts
private standard/third-party exceptions into its typed `Result` before returning;
RuntimeUi preserves that original error rather than translating generic exceptions.
Async producers copy command inputs before the synchronous borrow ends and retain
their own execution leases until terminal completion or cancellation drains.

Terminal feedback retains original Foundation error evidence and occupies its bounded
slot until `DrainWriteResults` acknowledges it. Pending poll evidence is nonterminal.
The owner calls `ReconcileControl` after an outcome to replace the draft with committed
target state. Reconciliation preserves focus/availability and clears transient edits.
Revocation closes write admission before cancelling private work; retirement produces
one cancellation per admitted command. Shutdown/destruction/move replacement abandon
reservations before releasing authority leases. The adapter retains its provider and
module image/callback leases until its jobs drain, so old work cannot mutate a newly
activated owner or outlive mapped code. `UpdateWritePresentation` adopts a newer
successfully presented interaction without rebuilding read bindings, cancelling old
edit/reservation evidence first; structural/document replacements still prepare a new
store. No normal frame waits, provider polling from layout, renderer
objects, hidden registration, reflection or gameplay ABI mutation is introduced.

Presented action-router replacement passes the old `LastIssuedSequence()` into the
new descriptor's `previousSequence`. The host retains this high-water mark for the
whole ownership generation, including periods with no active route, preventing
request identity reuse while keeping each router's queue bounded and private.

Migration is additive: existing read-only callers keep their API. Write users provide
explicit host admissions, capture edit sessions before input, route typed commands,
process them at the documented safe point, acknowledge terminal feedback and reconcile
controls. Preallocation includes a bounded draft per target; successful supported
control writes need no heap allocation after preparation. Native module composition,
list/record editing and general asynchronous UI progress remain separate contracts.

Write entry points borrow large owner/source/edit inputs through const references
only until return, copying evidence into retained command storage when admitted.
`UiControlStateMachine::Handle` likewise borrows its input until return; ordinary
call expressions remain valid, while member-function-pointer aliases must use
`const UiControlInput&`. This avoids a redundant large input copy without changing
input ownership or admitting asynchronous borrows. The affected in-tree control
and binding consumers are covered by the isolated public-header and regression builds.

## Templates And Presets

[ADR-083](../../adr/083-ui-template-identity-schema-and-expansion.md) makes
templates first-class Assets-backed authoring fragments over core/registered UI
elements. Stable template-local element, parameter, slot and document-instance IDs
are distinct from runtime handles and display/path/index values. Templates do not
add runtime element kinds or execute callbacks.

Parameters target exact typed registered properties; named slots admit bounded
typed document-owned child fragments. Arbitrary string property paths and deep
linked-instance overrides are not supported. Nested templates form a finite
acyclic dependency graph and expand deterministically under complete identity,
schema, dependency and budget validation.

Before expansion, the load-time `HoroEngine::UiTemplateGraph` authoring/cook
domain resolves an owned snapshot of exact template semantic revisions and
host-pinned package versions. The bounded graph
walk produces dependency-first template IDs and a canonical package closure;
it rejects cycles by template asset ID, absent revisions, incompatible interface
versions, missing or incompatible locked packages, and exhausted budgets. This
load-time step performs no package selection, provider I/O, or runtime mutation.

Insert expands into fresh ordinary document-owned elements and retains no update
relationship. Linked instance serializes the asset, accepted semantic revision,
public arguments and slot content while its subtree remains a derived projection.
Template updates mark UpdateAvailable and require an explicit previewed atomic
rebase; they never patch documents or running UI on source save.

Detach materializes fresh persisted element IDs and removes the link. Cook resolves
exact accepted/locked dependencies and flattens linked instances into ordinary
`CookedUiDocument` elements, so packaged Runtime UI has no source-template,
propagation or detach authority.

Core templates may include:

```text
Create > UI > Main Menu Template
Create > UI > Pause Menu Template
Create > UI > Basic HUD Template
Create > UI > Health Bar Template
Create > UI > Dialogue Box Template
Create > UI > Loading Screen Template
Create > UI > Interaction Prompt Template
```

Example:

```text
Main Menu Template
  Canvas
    Screen
      Frame
        Title Text
        Continue Button
        New Game Button
        Settings Button
        Quit Button
```

## Create Menu Integration

The built-in create menu exposes runtime UI separately from scene meshes and
editor panels:

```text
Create
├── Empty Object
├── Camera
├── Light
├── 3D Object
│   ├── Cube
│   ├── Sphere
│   ├── Capsule
│   ├── Cylinder
│   ├── Cone
│   └── Plane
├── Physics
│   ├── Trigger Volume
│   └── Collider
├── Audio
│   └── Audio Source
└── UI
    ├── Canvas
    ├── Screen
    ├── Panel
    ├── Frame
    ├── Text
    ├── Image
    ├── Button
    ├── Progress Bar
    ├── Slider
    ├── Checkbox
    ├── Input Field
    ├── Scroll View
    ├── Main Menu Template
    ├── Pause Menu Template
    ├── HUD Template
    └── Dialogue Box Template
```

The create menu is generated from runtime UI descriptors and scene primitive
descriptors. It must not hardcode divergent object lists in editor, CLI, and MCP
adapters.

## Editor Authoring

HoroEditor provides authoring tools for runtime UI:

- UI hierarchy view
- canvas preview at reference resolutions
- anchor/layout inspector
- style inspector
- font and localization preview
- focus/navigation graph visualization
- template insertion
- binding validation
- play-mode preview

The editor authoring UI uses HoroEditor panel/modal systems, but edited content
remains runtime UI data. Preview creates an isolated ordinary game runtime and
ADR-073 scopes/attachments. Preview/play mutations apply back only through an
explicit document-revision-checked editor command with undo.

## Core Vs Package Boundary

Core engine provides:

- Canvas, Screen, Panel, Frame
- Text, Image, Button
- Progress Bar, Slider, Checkbox/Toggle, Input Field, Scroll View
- Layout groups, anchors, scaling, safe-area handling
- Focus/navigation and UI input actions
- UI rendering pass
- UI serialization
- basic templates

Packages or future features may provide:

- RPG inventory system
- dialogue system
- quest tracker
- minimap
- rich text and markup
- advanced UI animation/timeline
- WebView
- marketplace widget packs
- domain-specific HUD frameworks

## Metrics And Observability

Game UI exposes bounded metrics:

- element count
- layout time
- text shaping time
- draw command count
- overdraw estimate when supported
- atlas usage
- input focus changes
- binding update count

Metrics follow [Observability Metrics And Profiling](../observability/observability-performance.md).

Runtime UI diagnostic evidence uses nine closed categories: document, layout,
text, input, focus, binding, render, accessibility and lifecycle. A record owns
one canonical Runtime UI error code, severity, a bounded message and at most six
typed correlation fields in stable key order. Document, element and canvas values
reuse Runtime UI identities; player, viewport and Foundation operation values are
dependency-neutral scalar projections used only as evidence and never establish
new identity authority. The operation `Error` remains control-flow authority. A
diagnostic record is inert and owns no log sink, store, renderer or runtime state.

## Testing

Required tests cover:

- game/player/scene/viewport scope lifetime and unrelated-scope persistence
- lifecycle activation rollback, scene/viewport unload and repeated shutdown
- canonical owner-command/VariableUpdate/layout/extraction/presentation order
- last-presented hit testing across skipped/failed presentation
- gameplay pause, single-step, UI time policy and host suspension/resume
- layout determinism across reference resolutions
- safe-area and scaling behavior
- focus graph navigation with keyboard and gamepad
- pointer hit testing and clipping
- submit/cancel routing
- serialization round-trip
- template expansion into core primitives
- binding validation failures
- null/headless UI render extraction
- editor/CLI/MCP create equivalence

## Related Documents

- [UI Canvas Editor UI Reference](../../../mock-studio/designs.md#architecture-runtime-ui-canvas-editor): widget palette, hierarchy, anchors, and design-time canvas preview panel.

- [Runtime Lifecycle](./runtime-lifecycle.md): frame phases, pause, suspension and
  shutdown order specialized by ADR-073.
- [Input Architecture](./input-architecture.md): action maps, gamepad, focus, and
  interaction scopes.
- [Rendering Architecture](./rendering-architecture.md): render extraction,
  frontend/backend boundaries, and pass composition.
- [Scene Runtime](./scene-runtime.md): runtime scene definitions and ECS
  conversion.
- [Built-In Scene Primitives](./built-in-scene-primitives.md): scene object and
  primitive creation catalog.
- [Editor Panel Host](../editor/editor-panel-host.md): HoroEditor panels and tabs
  that are separate from runtime UI.
- [Editor Modal Host](../editor/editor-modal-host.md): HoroEditor modal system,
  separate from game menus.

## Typed asset generation reload

`UiScreenTransition` owns asynchronous whole-screen preparation over the existing
Assets-backed loader and complete `UiReloadGeneration` composition. It retains
the current last-good publisher through cancellation, timeout and failure, then
swaps complete publishers at an ADR-073 structural safe point. Frame polling
reads stage/deadline facts without pumping assets; composition and deferred
reclamation are explicit load-time operations. Old whole-generation leases remain
pinned, while fresh presentation evidence gates the replacement screen's input.
See [the asynchronous screen transition contract](../../guides/runtime-ui-screen-transition-migration.md).

`UiHotReload` is owned by `HoroEngine::RuntimeUi`. It prepares complete real
Assets closures and actual canvas owners privately, reconciles compatible stable
IDs, then publishes one whole generation at the ADR-073 owner safe point. Actual
source changes invalidate a prepared candidate; rejected loads/candidates keep
the last good generation. Compatible text edit drafts retain their Cancel
baseline, while pressed/native IME/capture/pending action state is reset.

Publication closes bounded owner admission without invoking an external binding
write authority. Explicit load-time retirement collection abandons reservations
once while retaining authority/module/dependency pins until lease drain. Raw
modal/route handles use never-reused allocator-backed namespaces and EVER-issued
incarnations; retired history remains burned. Render/input consumers retain the
whole generation lease and check its current-publication fence. Presentation
eligibility requires a successful receipt for the replacement interaction.

See [the host migration and lifetime contract](../../guides/runtime-ui-hot-reload-migration.md).
This implements the typed RuntimeUi backend responsibility; the aggregate
RuntimeUiService and host semantic-scope registry remain separate composition
work. No concrete backend or host scope rebind is selected by reload.

## Overlay, modal and restoration lifecycle

`UiOverlayLifecycle` belongs to `HoroEngine::RuntimeUi`. It transfers entire
privately prepared, Assets-backed `UiHotReload` publishers for independently
owned layers. The selected canvas must contain exactly one actual active route;
the route's cooked band/order/modal metadata remains authoritative. Host-issued
context, logical viewport, player and render-view associations are explicit.
`UiOverlayViewportId` is a logical UI audience identity, not a renderer view or
an Input device/player index; multiple render views may share one logical viewport.
The owner never discovers those associations or installs an Input context token.

Priority is deterministic by core band, cooked order, then never-reused layer
activation. Viewport/Player/GameInstance exclusion applies to associated lower
contexts even before a modal's first successful presentation. Passive overlays
do not consume input or steal focus. Hosts gate their existing navigation/text/
pointer adapters through `InputStatus`, neutralize held input and dispatch the
existing focus-loss/control cancellation contract when eligibility changes.
Gameplay blocking and safety passthrough remain the Input host's ADR-078 policy;
neither this owner nor a widget mutates gameplay or assigns a device.

Opening a modal installs the existing focus graph's inclusive trap/default and
saves lower focus as bounded stable-ID ancestry. It cancels real lower-context
pointer capture. Closing an exact uncovered modal restores only surviving,
unblocked scopes. Missing/destroyed/disabled targets use the existing graph's
ancestor/default/first recovery policy, never a saved transient handle. Covered
modals cannot be dismissed out of order. Reload uses whole-generation preparation
and the ADR-073 commit point; cancellation or invalid modal composition preserves
the last-good layer. A new interaction requires fresh presentation evidence.

Active slots (at most 64), ordinary retirement slots (at most 64), and an
independent shutdown reserve are allocated at creation. Frame operations use
bounded scans, fixed snapshot scratch, and value-only restoration paths; no I/O,
backend selection, callback or generation reclamation occurs on those paths.
Dismissal can backpressure without removing a modal barrier. Shutdown closes
admission idempotently and retains actual generations until producer/render/read
leases drain. Explicit load-time `CollectRetired` invokes existing deferred
abandonment; reentrant layer mutation is rejected and reentrant shutdown closes
admission immediately, deferring transfers until the callback borrow ends.

Migration is additive: existing public owners and durable UI schema/version are
unchanged. The new header has one RuntimeUi CMake owner and standalone-header
consumer coverage. `UiFocusGraph::CaptureRestoration` and `Restore` export/import
copied stable ancestry under exact static scope and nonfuture revision checks.
Rebuild RuntimeUi consumers for the new symbols. Hosts carry the EVER-issued
layer incarnation when recreating an owner. No aggregate RuntimeUiService,
frontend/editor composition, durable authoring schema or renderer activation is
claimed by this backend contract.
