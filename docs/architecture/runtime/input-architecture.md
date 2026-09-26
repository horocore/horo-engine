# Input Architecture

## Purpose

This document defines platform input collection, frame snapshots, action
mapping, editor and gameplay routing, focus, capture, modal exclusivity,
rebinding, and deterministic simulation consumption.

## Core Decisions

- Platform events are normalized into immutable input snapshots.
- Raw device state and semantic actions are separate layers.
- Input routing follows explicit interaction scopes and focus ownership.
- Editor modals block workspace and gameplay command handling before handlers
  execute.
- Fixed simulation consumes tick-assigned input commands, not mutable live
  device state.
- Bindings are typed configuration with conflict validation.
- Input is not distributed through the general process data bus.
- Gamepad input is a first-class runtime input device, not an optional addon.
- Optional packages may improve gamepad mappings, glyphs, vendor metadata, and
  advanced device features, but common controllers must work without installing
  an external package.

## Layer Model

```text
OS / Window Events
       |
       v
Platform Input Collector
       |
       v
RawInputSnapshot
       |
       v
Input Router + Action Maps
       |
       +-- GUI text/navigation input
       +-- Runtime UI player/viewport contexts
       +-- Editor commands and viewport controls
       +-- Gameplay InputFrame
```

The platform collector owns device state. Consumers receive immutable snapshots
for one frame.

Accessibility sticky modifiers, hold/repeat thresholds and toggle actions are
semantic interpretation in `InputMapping`/router after collection; they never
rewrite physical transitions in `RawInputSnapshot`. Focus loss, disconnect,
binding replacement and context removal release synthetic action state before it
can leak into another context. Mapping adds no per-event allocations or unbounded
waits. See [Accessibility Architecture](./accessibility-architecture.md).

## Raw Snapshot

```cpp
struct RawInputSnapshot {
    FrameNumber frame;
    KeyboardState keyboard;
    PointerState pointer;
    std::vector<GamepadState> gamepads;
    TextInput text;
    ModifierState modifiers;
    WindowInputState window;
};
```

It includes held state and transitions:

- down
- pressed this frame
- released this frame
- repeated text/navigation events where applicable

Scroll, pointer delta, and text input are frame-local values.

## Event Collection

Native callbacks update a platform-owned collector or bounded event queue. After
the host polls events, the collector commits one snapshot.

Callbacks do not:

- mutate scene or editor state
- invoke application use cases
- publish one data-bus event per key or pointer movement
- call renderer or GUI business logic

## Action Mapping

```cpp
struct ActionDescriptor {
    ActionId id;
    ActionValueType valueType;
    InputContextId context;
    std::vector<InputBinding> defaultBindings;
};
```

Actions may be digital, one-dimensional, or two-dimensional. Text and IME
composition remain snapshot data for the focused text surface; they are not
encoded as semantic action bindings.

Examples:

```text
editor.save
editor.undo
viewport.orbit
viewport.move
gameplay.move
gameplay.jump
```

Bindings may combine keys, buttons, axes, modifiers, and chords. They use typed
device controls rather than arbitrary display strings.

## Input Contexts

Contexts form an ordered routing stack:

```text
Native Dialog
Modal Child
Modal Root
Focused GUI Widget
Editor Tool Capture
Editor Workspace
Gameplay
Global Non-Mutating Commands
```

The first eligible context may consume an input transition. Held state is still
available only where policy allows it.

Contexts are pushed and removed through RAII tokens. A destroyed tab, modal,
tool, or play session cannot leave an active context behind.

[ADR-078](../../adr/078-runtime-ui-input-context-and-player-routing.md) specializes
the ADR-073 UI ownership boundary with generation-checked
`RuntimeUiInputContextId` values. Device, input user, local player, logical viewport
and UI context remain distinct identities joined by immutable assignment and
attachment revisions. A context declares one single-player, shared-player, game-
instance or unassigned-join audience plus a typed viewport policy.

In a packaged game, host-critical dialogs, game-instance modals, player/viewport
modals and ordinary UI routes precede their associated gameplay context. In editor
play, native dialogs, editor modals and the focused editor widget/tool precede
Runtime UI; the game UI context is eligible only when its embedded game viewport
owns focus. Runtime UI cannot consume editor shortcuts from an unfocused viewport.

Each action transition is consumed at most once and recorded in an immutable
`InputConsumptionLedger`. Non-exclusive UI blocks only handled transitions;
Viewport, Player and GameInstance exclusive modals block associated lower contexts
even when unhandled, except for a finite host-owned safety/global passthrough list.
Gameplay input frames consume only the ledger-filtered projection for their exact
player/tick mapping.

Runtime UI pointer/focus routing uses the last successfully presented
`UiInteractionSnapshot`, not an unpublished layout. Failed/skipped presentation,
scope or viewport destruction, route replacement, focus/device loss, suspension
and shutdown neutralize the context and release capture. Split-screen players keep
independent focus/capture unless an explicit game-instance modal policy blocks
them. UI actions are typed application/gameplay commands for owner safe points,
not direct ECS mutations from input handlers.

Device assignment changes publish transactionally, neutralize held input/capture
under the old owner and start the new owner from an explicit neutral snapshot.
Per-player/context modality (`KeyboardMouse`, `Gamepad`, `Touch`, `Pen`,
`Accessibility`, `Unknown`) changes only from routing-eligible meaningful evidence
after deadzone/noise filtering. Modality drives presentation/glyphs but never
reassigns a device or grants routing priority.

## Focus And Capture

Focus selects the keyboard and accessibility target. Capture grants a surface
continued pointer delivery during an active gesture.

Capture:

- has one owner
- records the initiating button/pointer
- ends on release, Escape/cancellation, focus loss, modal/native-dialog opening,
  pointer-device loss, owner destruction, context removal, or token destruction
- prevents competing viewport and GUI gestures

Opening a root modal cancels workspace capture, gizmo drag, drag/drop, and
incomplete editor gestures before activating modal scope.

The editor wraps its synchronous project and location folder pickers in
`NativeDialog` RAII contexts. Their destruction restores the preceding context
stack without leaving ambient dialog state behind.

## Modal Exclusivity

While `EditorInteractionScopeKind::Modal` is active:

- only the top modal and its widget popups receive input
- editor shortcuts do not execute
- viewport navigation and picking are suppressed
- gameplay input is neutralized unless the modal explicitly represents an
  in-game overlay with a separate contract
- background model and job updates continue without interactive input

A dim layer alone is not an input barrier. Central routing enforces exclusivity.

See [Input Layer and Modal Ownership](./input-layer-ownership.md) for the
`EditorInteractionScope` ↔ `InputContextKind` mapping, frame-order invariants,
and `PointerCaptureToken` cancellation catalogue.

## Text Input

Text input uses native text/composition events and is separate from physical key
bindings. Active text widgets consume editing commands before global shortcuts.

IME composition is owned by the focused text surface and is cancelled or
committed according to platform policy when focus scope changes.

## Gameplay Input Frames

The runtime transforms action state into a simulation input frame:

```cpp
struct GameplayInputFrame {
    SimulationTick tick;
    Vec2 move;
    Vec2 look;
    bool jumpPressed;
    bool interactPressed;
};
```

One fixed tick consumes one assigned input frame. When several simulation ticks
run during one presentation frame, edge-triggered actions are consumed according
to the action's declared policy and do not fire accidentally on every catch-up
tick.

`GameplayInputFrameBuilder` is owned per player by the host input boundary. After
the snapshot is committed and higher-priority UI consumers have updated the
consumption ledger, the host calls `Capture(router, gameplayContext, player)`
once, before `FixedUpdate`. Fixed simulation calls only `Consume(simulationTick)`
and receives a value frame; it never receives the router, collector, snapshot,
or mutable device state. `Consume` is allocation-free and uses the scheduler's
one-based tick ordinal. The host must call it once for each attempted tick in
strict tick order, and owns retry/rollback policy if a fixed-update participant
fails.

Held axes use the latest admitted capture for every catch-up tick. Press edges
are latched across presentation frames with no fixed tick, then assigned to the
next tick exactly once. A second capture of the same committed snapshot is
ignored. Losing focus or gameplay context ownership clears both held values and
pending edges so input cannot leak through a modal or resume later. `Reset()`
clears a builder at player/session ownership changes. Recording stores the
already assigned value frames; replay feeds those frames directly to simulation,
without repeating action routing or OS events.

The concrete frame also carries the move action's `down`, `pressed`, and
`released` bits. These preserve existing semantic callbacks even when opposing
movement bindings cancel the axis value; their edges follow the same once-per-
tick latch policy.

Migration from the initial `Consume(router, context, tick, player)` API: move
routing to the host's pre-fixed-update phase using `Capture`, then replace the
fixed-update call with `Consume(tick)`. No production caller of the initial API
existed when this contract was qualified; the input unit test was updated. The
editor play screen also moves its old fixed-update `ReadAction` call to a
pre-fixed `OnInputSnapshot` callback and passes the scheduler tick through
`GuiScreenHost::OnFixedUpdate`. Screen implementations overriding that callback
must accept the new tick argument.

### INP-001.5 qualification

| Acceptance criterion | Executable or audit evidence |
| --- | --- |
| Recorded replay has identical commands | `Gameplay Commands Survive Zero Tick Frames And Match Different Presentation Cadences` records and replays the full tick-stamped sequence; `Recording Replays Exactly` covers cursor exhaustion/reset. |
| Tick assignment is cadence-independent | The same test compares a zero-tick-then-catch-up schedule against one tick per presentation. `Actions And Fixed Tick Edges Resolve Once` checks held/edge behavior across two catch-up ticks. |
| Move semantic transitions survive projection | `Tick Frames Preserve Move Action Edges When Opposing Axes Cancel` checks pressed/released independently of net movement. |
| Fixed simulation does not read raw device state | `GameplayInputFrameBuilder::Consume` takes only `SimulationTick`; the router is limited to `Capture`. An audit of `src/runtime`, `src/editor`, `apps`, and `tests/fixtures/gameplay_e2e` found raw input collection in adapters/router/editor presentation only, not fixed-update implementations. The editor play screen's former fixed-update `ReadAction` call was moved to `OnInputSnapshot`. `Gameplay Capture Uses The Consumption Ledger Before Tick Production` verifies the routing boundary. |

The focused local command is `ctest --test-dir build/skeleton -R
'^(HoroInputTests|HoroGuiScreenHostLifecycleTests|HoroEditorPlaySessionControllerTests)::'
--output-on-failure --parallel 2` (35/35 on Linux). The hosted current-head
status remains the source of truth for the full platform matrix; see the PR
checks rather than preserving a stale status in this document.

This representation supports recording, replay, tests, and future networking
without replaying OS events.

## Rebinding And Persistence

Binding overrides are user configuration by default. A game project may define
its own action schema and packaged defaults.

Rebinding validates:

- reserved operating system shortcuts
- duplicate or conflicting bindings in active contexts
- unsupported device controls
- required actions left unbound
- modifier and chord ambiguity

Display labels are generated by the platform/UI adapter and are not persistent
binding identity.

## Device Lifecycle

Device connection and disconnection update the next snapshot. Gameplay assigns
devices through explicit player/device policy.

Loss of an active device produces neutral state and a typed notification. It
does not leave keys, buttons, or axes logically held.

## Gamepad Support

Gamepad support is part of the runtime input contract. The engine owns the
canonical gamepad model, snapshot integration, action-map binding support,
rebinding validation, device lifecycle, player assignment, normalization,
disconnect behavior, haptics contract, and virtual test backend.

The baseline engine distribution must provide a usable common-controller path:

- canonical `GamepadState`, `GamepadDeviceId`, `GamepadButton`, and
  `GamepadAxis` types
- basic controller mapping fallback using platform game-controller APIs or an
  SDL/XInput-style canonical layout strategy
- unknown-controller fallback that exposes supported buttons and axes safely
- action binding support for buttons, triggers, sticks, and D-pad controls
- configurable deadzone and axis normalization policies
- disconnect neutralization
- player/device assignment
- virtual gamepad injection for tests and headless runs
- simple rumble/haptics interface where the platform supports it

Optional packages may extend gamepad support with larger controller mapping
databases, glyph packs, Steam Input integration, DualSense advanced features,
Nintendo layout metadata, adaptive triggers, lightbar, touchpad, or other
vendor-specific capabilities. These packages improve correctness, presentation,
and device-specific richness; they do not define whether gamepad input exists.

### Device Identity

Gamepad identity is not a stable integer index alone. Device handles include a
session generation so stale references cannot silently point at a different
controller after disconnect/reconnect:

```cpp
struct GamepadDeviceId {
    uint32_t slot;
    uint64_t sessionGeneration;
};
```

The slot is presentation-friendly. The session generation protects ownership and
lifetime-sensitive state.

### Canonical Controls

The engine exposes physical position-oriented canonical controls:

```cpp
enum class GamepadButton {
    South,
    East,
    West,
    North,
    LeftShoulder,
    RightShoulder,
    LeftStick,
    RightStick,
    Start,
    Select,
    DPadUp,
    DPadDown,
    DPadLeft,
    DPadRight,
};

enum class GamepadAxis {
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
};
```

Gameplay and editor actions bind to canonical controls, not vendor display
labels. A project should bind `gameplay.jump` to `GamepadButton::South`; the UI
can then present Xbox `A`, PlayStation `Cross`, or Nintendo south-position text
through a glyph provider.

### Player Assignment

Player assignment is explicit and supports mixed device ownership:

```text
Player 1 -> Keyboard/Mouse
Player 2 -> Gamepad A
Player 3 -> Gamepad B
```

Future local co-op flows such as "press any button to join" are built on top of
device lifecycle and assignment primitives. They do not bypass the action-map or
snapshot model.

### Deadzone And Normalization

Deadzone policy is binding/context-specific, not a single global constant:

```text
leftStick.move      radial deadzone 0.18
rightStick.camera   radial deadzone 0.12
trigger.accelerate  threshold 0.05
menu.navigation     digital threshold 0.50
```

Normalization happens before action resolution so gameplay consumes stable
values independent of backend quirks. Raw values may be exposed only through
diagnostic or explicitly raw-input APIs.

### Glyph And Presentation Providers

The engine owns the `GlyphProvider` interface but not every icon pack:

```text
Action: gameplay.jump
Binding: GamepadButton::South
Glyph provider:
  Xbox        -> A
  PlayStation -> Cross
  Nintendo   -> south-position policy label
```

Glyph packs are packages. The absence of a glyph pack falls back to canonical
text labels and does not break input.

### Haptics Boundary

Core haptics is intentionally narrow:

```cpp
class IGamepadHaptics {
public:
    virtual Result<void> PlayRumble(GamepadDeviceId id, RumbleEffect effect) = 0;
    virtual Result<void> Stop(GamepadDeviceId id) = 0;
};
```

Advanced vendor features such as adaptive triggers, lightbar, touchpad gestures,
or HD rumble require explicit capability descriptors and may be delivered by
optional packages or platform integrations.

### Virtual Test Backend

Headless and deterministic tests use a virtual gamepad backend:

```cpp
VirtualGamepad pad;
pad.Connect();
pad.Press(GamepadButton::South);
pad.SetAxis(GamepadAxis::LeftX, 1.0f);
```

Virtual input commits through the same snapshot path as platform devices. Tests
must not mutate gameplay state by bypassing the input service.

## XR Input And Interaction Profiles

The XR backend publishes bounded pose/action snapshots and projects admitted
OpenXR actions into the same canonical Horo action router. Input does not poll
OpenXR or retain native action/path handles.

[ADR-159](../../adr/159-xr-action-tracking-and-input-projection-ownership.md)
defines the normative handoff. Application/project policy declares canonical actions
and product requirements; Input owns semantic schemas, contexts, player assignment,
overrides, routing and transition consumption; XROpenXR owns native action sets/actions/
spaces/paths, synchronization and location; XRRuntime owns the generation-scoped Horo
snapshot. A host-composed adapter is the only XR-to-Input publisher.

XR device identities include the XR session generation. Grip/aim poses,
buttons, axes, hand joints, and haptic targets become invalid when tracking,
focus, interaction profile, device, or session generation changes. The next
input frame neutralizes affected actions instead of preserving stale values.

Suggested-binding data is schema-versioned independently from ordinary user
input profiles. Resolution records:

- the runtime-selected interaction profile;
- the engine action and native component-path mapping;
- generic fallback and unsupported bindings;
- conflicts and missing required actions;
- active-profile changes during a session;
- migration of compatible user overrides after schema changes.

Native OpenXR paths are physical binding identities, not localized labels or
gameplay action IDs. Presentation uses the existing glyph/label provider model.
Frame-hot hand joints use fixed-capacity or owner-backed spans rather than one
heap allocation per hand per frame.

XR sampling has one bounded owner-thread opportunity after native runtime-event handling
and before the normal `BuildInputSnapshot` commit. Late callback/worker results enter the
next frame. The adapter cannot create actions, choose a player, bypass context priority
or consume a transition. On focus, tracking, profile, device, permission or session loss,
it supplies explicit unavailable state and Input commits neutralization rather than
reusing the last snapshot.

Gameplay fixed ticks receive an immutable tick-assigned Horo projection with exact
source sample/generation. They never query XRRuntime for a latest pose. Rendering-time
late poses cannot rewrite that projection. Catch-up ticks follow declared edge/hold
policy, and any pose interpolation/extrapolation must be an explicit simulation policy.

Gestures are produced by finite host-composed recognizers from immutable admitted data,
then routed as canonical action candidates by Input. A recognizer owns neither focus nor
gameplay meaning. Hand joints and eye gaze additionally require purpose-bound privacy
admission; derived access does not grant logging, replay, telemetry or AI retention.

XR haptics follow the same ownership principles as gamepad haptics but include
session/device generation, action/subpath target, duration, frequency where
supported, cancellation, and explicit unsupported fallback. Ordinary input or
focus loss cancels effects owned by the lost scope.

The Input/application haptic coordinator owns request scope, validation and cancellation;
XROpenXR owns native apply/stop. Accepted, submitted and physically completed are
distinct states. Device/profile/session changes, timeout and shutdown cancel old work;
no timer or callback outlives its owner generation.

[ADR-161](../../adr/161-xr-interaction-runtime-ui-locomotion-and-accessibility-ownership.md)
keeps an XR ray, direct point, proximity volume, Physics hit or gesture as evidence until
normal Input routing admits the associated semantic action. Runtime UI owns UI focus and
capture; gameplay owns use/grab/locomotion meaning; Character owns movement. The XR
adapter cannot choose a player, bypass the consumption ledger or mutate either target.

## Data Bus Relationship

High-frequency input does not travel through `EngineDataBus` or
`EditorDataBus`. The authoritative input service is queried once per frame or
tick.

Low-frequency committed changes may publish notifications:

- bindings changed
- active device changed
- input capability changed

## Implemented Module And Persistence Topology

The backend-neutral implementation is the `HoroInput` CMake target, exported as
`HoroEngine::Input`, with its public contract in `include/Horo/Runtime/Input.h`.
It depends only on Foundation plus the private JSON parser used by profile
persistence. SDL collection and rumble live in the separate `HoroInputSdl`
target; SDL types do not cross the public runtime boundary. Headless consumers
compose `RawInputCollector` and `VirtualGamepad` without SDL or a window.

Profiles use schema version 1 and are replaced atomically. The active project
layers profiles in this order:

1. action descriptor defaults
2. `.horo/input.json` project defaults
3. editor-global user profile under `.horo/input/editor.json` in the platform
   user configuration home
4. project-user override under `.horo/input/projects/<projectId>.json` in that
   same user configuration home

Parsing, schema, or binding validation failure preserves the last valid router
profile and emits a diagnostic; profiles are never partially applied. New
projects create a valid `.horo/input.json` during atomic project staging.

The editor registers real action descriptors and exposes them through the
`Input Mapping` workspace panel. Settings retains viewport sensitivity and
inversion only, and directs binding edits to this panel so there is no second
string-based shortcut authority.

## Testing

Required tests cover:

- pressed/released frame semantics
- context priority and consumption
- modal blocking and focus restoration
- pointer capture cancellation
- text input versus shortcut routing
- fixed-tick edge consumption during catch-up
- deterministic recorded input replay
- binding conflict validation
- device disconnect neutralization
- headless injection without a native window
- canonical gamepad mapping fallback without external packages
- stale gamepad handle rejection after disconnect/reconnect
- mixed keyboard/gamepad player assignment
- binding-specific gamepad deadzone policies
- glyph-provider fallback when no glyph package is installed
- virtual gamepad injection through the snapshot path
- XR interaction-profile changes and binding-schema migration
- XR focus/tracking/session loss neutralization
- bounded hand-joint snapshots and stale XR device rejection
- XR haptic cancellation and unsupported fallback

## Related Documents

- [Input Mapping Editor UI Reference](./input-mapping-editor.html): action maps, bindings, device preview, and conflict detection panel.

- [Input Layer and Modal Ownership](./input-layer-ownership.md): layer ownership table, context kind priority, `EditorInteractionScope` mapping, frame-order invariants, and per-layer testing obligations.
- [Runtime Lifecycle](./runtime-lifecycle.md)
- [Editor Modal Host](../editor/editor-modal-host.md)
- [GUI Screen Host](../editor/gui-screen-host.md)
- [Configuration System](../foundation/configuration-system.md)
- [Platform Abstraction](../foundation/platform-abstraction.md)
- [XR Architecture](./vr-ar-architecture.md)
