# Input Layer and Modal Ownership

## Purpose

This document ratifies the ownership, lifetime, and coupling contracts for the
three input layers — collection, snapshot, and routing — and their interaction
with the editor modal host. It is a companion to
[Input Architecture](./input-architecture.md), which defines the overall
strategy, and to [Editor Modal Host](../editor/editor-modal-host.md), which
defines the modal workflow contract. Read those first.

The normative scope of this document is:

- which layer owns mutable device state, immutable frames, and semantic resolution
- `InputContextKind` priority ordering and the host responsible for each level
- `EditorInteractionScope` ↔ `InputContextKind` mapping
- `FocusedGuiWidget` per-frame context lifecycle
- `PointerCaptureToken` cancellation catalogue
- frame-order invariants for snapshot commit, context mutations, and modal stack
- `InputService` composition helper vs. direct composition
- `NativeDialog` RAII context pattern
- normative invariants and testing obligations per layer

## Layer Ownership

Three layers form a strict one-way data flow. Each layer has one owner and one
responsibility.

```text
OS / Window Events
       |
       v
RawInputCollector          ← mutable device state, frame-edge generation
       |  Commit()
       v
RawInputSnapshot           ← immutable per-frame value; consumers read but never write
       |  BeginFrame()
       v
InputRouter + Contexts     ← semantic resolution, consumption, and capture
       |
       +-- GUI text / navigation
       +-- Editor commands and viewport controls
       +-- Gameplay InputFrame
```

### Layer 1 — `RawInputCollector`

`RawInputCollector` owns all mutable device state. It accumulates platform
callbacks within one frame and produces one immutable snapshot per `Commit()`
call.

Responsibilities:

- track held, pressed, and released state for keyboard, pointer, and gamepads
- accumulate wheel deltas, text, IME composition, modifier state, and window
  state within the frame
- neutralize all held controls via `Neutralize()` when focus or device ownership
  is lost
- assign generation-safe `GamepadDeviceId`s on connect; invalidate them on
  disconnect

Invariants:

- `BeginFrame()` must be called before any device mutations for that frame
- `Commit()` seals the accumulated state into a new `RawInputSnapshot`; device
  mutations after `Commit()` but before the next `BeginFrame()` are not defined
- the collector does not call application logic, publish data-bus events, or
  mutate scene or editor state

### Layer 2 — `RawInputSnapshot`

`RawInputSnapshot` is an immutable value. Once `Commit()` returns a reference,
that snapshot's content does not change until `Commit()` is called again on the
next frame.

Consumers:

- read the snapshot through `InputRouter::Snapshot()` or receive it directly
  from a backend
- never write to a snapshot or keep a pointer across frames (the reference is
  valid only until the next `Commit()`)

The snapshot carries:

- `frame` counter for correlation
- `keyboard` — `ButtonState` per `Key`
- `pointer` — position, deltas, wheel deltas, and `ButtonState` per
  `PointerButton`
- `gamepads` — a vector of `GamepadState`, one per connected device
- `text` — raw text input for this frame
- `composition` — active IME pre-edit text and selection
- `modifiers` — logical modifier state
- `window` — focus, pointer-inside, and pointer-device-available flags

### Layer 3 — `InputRouter` and Contexts

`InputRouter` owns semantic resolution, consumption tracking, and exclusive
pointer capture. It operates on the immutable snapshot installed by
`BeginFrame()` and exposes actions, raw key/button consumption, and capture
through a stack of RAII `InputContextToken`s.

Responsibilities:

- maintain an ordered priority stack of live contexts (`InputContextToken`s)
- route action reads and consumption to the highest-eligible context
- enforce exclusive pointer capture through `PointerCaptureToken`
- validate and apply `InputBindingProfile`s without partial application
- neutralize capture when the owning context is removed or priority changes

## `InputContextKind` Priority Table

Contexts are ordered from lowest to highest priority. A higher-priority context
is given first opportunity to consume an input transition. Held state reads
are not consumed; only transitions (`pressed`, `released`) are tracked for
consumption.

| Priority | `InputContextKind` | Pushed by | Lifetime |
|---|---|---|---|
| 0 (lowest) | `GlobalNonMutating` | Application host | Full editor session |
| 1 | `Gameplay` | Runtime play session | Active play session |
| 2 | `EditorWorkspace` | `BuildEditorInputActions()` registration | Active workspace screen |
| 3 | `EditorToolCapture` | Active viewport tool (gizmo, placement, brush) | Duration of tool gesture |
| 4 | `FocusedGuiWidget` | Per-frame by ImGui `WantTextInput` check | Single frame (re-acquired each frame) |
| 5 | `ModalRoot` | `EditorModalHost::OpenRoot()` | Until modal is removed from stack |
| 6 | `ModalChild` | `EditorModalHost::PushChild()` | Until child is removed from stack |
| 7 (highest) | `NativeDialog` | Caller wrapping a native OS dialog | Duration of native dialog |

Only one context token is active per level in the standard editor composition.
If two distinct logical contexts ever hold the same `InputContextKind`
simultaneously (for example, two viewports each pushing an `EditorToolCapture`
token), `IsContextActive()` returns `true` only for the most-recently pushed
token at that kind. In the current host, `FocusedGuiWidget` is always held by a
single member field; there is never more than one token of that kind live at a
time.

## `EditorInteractionScope` ↔ `InputContextKind` Mapping

`EditorModalHost` translates its internal modal stack state into an
`EditorInteractionScope` and simultaneously pushes matching `InputContextToken`s
into the `InputRouter`.

```cpp
enum class EditorInteractionScopeKind {
    Workspace,
    Modal,
    NativeDialog
};
```

The mapping is:

| `EditorInteractionScopeKind` | Router context(s) held | `InteractionScope()` return value |
|---|---|---|
| `Workspace` | No modal tokens; `EditorWorkspace` (and lower levels) active | `{kind = Workspace}` |
| `Modal` | `ModalRoot` token for the root modal; `ModalChild` token(s) for each child | `{kind = Modal, modalId = top-of-stack}` |
| `NativeDialog` | Caller-pushed `NativeDialog` token plus any underlying modal tokens | _not returned by `EditorModalHost::InteractionScope()`_ |

> **Important:** `EditorModalHost::InteractionScope()` reads only the modal
> stack (`m_stack`). It returns `Workspace` when the stack is empty and `Modal`
> when the stack is non-empty — regardless of whether a `NativeDialog`
> `InputContextToken` is also live in the router. The `NativeDialog` kind in
> `EditorInteractionScopeKind` is a router-level priority concept; callers that
> need to detect an open native dialog must track that state themselves (for
> example, by wrapping the dialog in a RAII struct that sets a local flag).

`EditorModalHost::InteractionScope()` reflects the modal stack state
synchronously. Before any modal is opened — including during the first frame
before `OnUpdate` runs — `m_stack` is empty and `InteractionScope()` returns
`{kind = Workspace}`. A pending accepted root-open request is reported as
`Modal` even before its first draw. This means the router enforces modal
exclusivity before the modal renders for the first time.

When `EditorModalHost::OpenRoot()` pushes a `ModalRoot` context:

1. `InputRouter::CapturePointer` is cancelled for the previously active context
   with `CaptureCancellationReason::ModalOpened`.
2. The `ModalRoot` token is pushed; it immediately outranks all workspace contexts.
3. `EditorInteractionScope` transitions to `Modal`.

When the root modal is removed, the `ModalRoot` token is destroyed and the
router reverts to whichever contexts remain below it.

## `FocusedGuiWidget` Per-Frame Context Lifecycle

The `FocusedGuiWidget` context exists for exactly one frame. The application
host re-acquires it each frame when ImGui signals that a text widget is focused.

Frame-by-frame pattern (from `HoroEditorApp`):

```cpp
// At the start of UpdatePresentation each frame:
focusedWidgetInputContext_.Reset();          // destroy previous frame's token

// After ImGui::NewFrame():
if (io.WantTextInput) {
    focusedWidgetInputContext_ =
        inputRouter.PushContext(InputContextId{"editor.focused_widget"},
                                InputContextKind::FocusedGuiWidget);
}
```

This pattern means:

- `FocusedGuiWidget` is pushed after `ImGui::NewFrame()` runs, so ImGui's own
  focus state is already settled for the frame before the context exists
- the token is destroyed at the beginning of the next frame's `UpdatePresentation`,
  which clears per-frame consumption cleanly
- if ImGui loses text focus mid-frame (for example, a modal closes during `Draw()`),
  the context is still active for the rest of that frame; it disappears at the
  next frame boundary
- only one `FocusedGuiWidget` token exists at a time because the single
  `focusedWidgetInputContext_` member holds the previous frame's token until reset

Consequence: text shortcuts in ImGui text fields are suppressed from editor
command handlers during the frame that `WantTextInput` is set, because
`FocusedGuiWidget` outranks `EditorWorkspace`.

## `PointerCaptureToken` Cancellation Catalogue

`CapturePointer()` grants exclusive pointer delivery for a specific
`PointerButton` to one context. The capture is cancelled automatically under
nine conditions:

| `CaptureCancellationReason` | Triggering condition | Error code |
|---|---|---|
| `Explicit` | Caller calls `InputRouter::CancelCapture(Explicit)` | — |
| `Released` | `EndFrame()` observes an unhandled initiating-button release after gesture handlers run | — |
| `Escape` | Escape key is pressed while capture is active | — |
| `FocusLost` | `WindowInputState::focused` becomes `false` | — |
| `ModalOpened` | A `ModalRoot`, `ModalChild`, or `NativeDialog` context is pushed while capture is active | `CaptureInactiveContext` on next attempt |
| `ContextPreempted` | A newer same-priority or higher-priority non-modal context takes the capture owner's place | `CaptureInactiveContext` on next attempt |
| `OwnerDestroyed` | The owner explicitly calls `CancelCapture(OwnerDestroyed)` during detach | — |
| `DeviceDisconnected` | `WindowInputState::pointerDeviceAvailable` becomes `false` | — |
| `ContextRemoved` | The owning `InputContextToken` is destroyed or reset | — |

`CaptureBusy` (`InputErrors::CaptureBusy`) is returned by `CapturePointer()`
when another context already holds capture. The caller must release the existing
capture before acquiring a new one.

`CaptureInactiveContext` (`InputErrors::CaptureInactiveContext`) is returned
by `CapturePointer()` when the supplied `InputContextToken` is not the
highest-eligible active context. Callers must not acquire capture for contexts
that are not currently active.

`IInputCaptureOwner::OnInputCaptureCancelled()` is called exactly once per
capture, synchronously on the thread that triggers the cancellation condition.
The owner must not call `CapturePointer()` again inside this callback.
An owner's destructor invalidates a still-live capture without invoking its
already-destructing virtual callback; the stale `PointerCaptureToken` then
reports inactive. An owner can hold at most one capture, including across
routers. Normal token release is not a cancellation callback.

The router retains a same-frame modal barrier after the last modal context
closes, so its opening/closing transition cannot reach workspace, viewport or
gameplay. A synchronous `NativeDialog` token also blocks the first committed
snapshot after the dialog returns; the preceding RAII stack then resumes.
The barrier excludes every context below `ModalRoot`, including held-state
reads, not just pressed edges. A live highest-eligible context is the sole
keyboard focus routing owner. Runtime UI focus remains per audience and
presentation scope as specified by ADR-078, not process-global.
The host calls `InputRouter::EndFrame()` after interactive presentation. Gesture
owners may commit and release normally when they see the release edge; the
router cancels only a capture still held at that boundary. Cancellation on
focus, device, modal, Escape, or context loss remains immediate.

## Frame-Order Invariant

Within one editor frame the following order is enforced:

```text
1. PollPlatformEvents()
   - process SDL events → inputBackend.ProcessEvent()
   - process native menu invocations and engine data-bus events

2. inputBackend.BeginFrame(frameNumber)
   - reset per-frame edge state in the collector

3. inputBackend.Commit() → RawInputSnapshot sealed

4. inputRouter.BeginFrame(snapshot)
   - install snapshot, clear per-frame consumption map

5. (modal host reads interaction scope; stack is stable at this point)

6. UpdatePresentation()
   - focusedWidgetInputContext_.Reset()  ← destroy previous frame token
   - ImGui::NewFrame()
   - push FocusedGuiWidget context if WantTextInput
   - modalHost.OnUpdate(dt)
       CommitPendingOpens(): promote any deferred child entries to m_stack,
                             then call OnOpen() for each newly added entry
       [modal OnUpdate() calls]
       CommitPendingCloses(): call RemoveTop() for each pending close reason
                              (destroys Entry and its InputContextToken)
   - screenHost.OnUpdate(dt)

7. ExtractFrame() / Draw()
   - screenHost.Draw()                  ← workspace draws, reads actions
   - modalHost.Draw()
       CommitPendingOpens()             ← same as above, in case Draw() queued more
       [top modal Draw(); may call RequestClose() → enqueues pending close]
       CommitPendingCloses()
   - ImGui::Render()

9. Present frame
```

**Key invariants**:

- `inputRouter.BeginFrame()` is called exactly once per frame, before any
  context pushes that frame and before any `ReadAction()` or `ConsumeKey()`
  calls
- `OpenRoot()` pushes the `ModalRoot` `InputContextToken` **immediately and
  synchronously** when called (inside `OnUpdate` or `Draw`). The router
  enforces modal exclusivity from that point within the same frame.
- Child modal tokens are also pushed immediately when `PushChild()` is called;
  however, the `Entry` is placed in `m_pendingChildOpens` and promoted to
  `m_stack` at the next `CommitPendingOpens()` call (at the start of `OnUpdate`
  or `Draw`).
- `OnOpen()` is called inside `CommitPendingOpens()`, not at the push site.
  An `OnOpen()` failure removes the entry and its token from the stack in the
  same call.
- `RequestClose()` enqueues a close reason; `RemoveTop()` — which destroys the
  `Entry` and its `InputContextToken` — runs inside `CommitPendingCloses()`.
  The token is live until `CommitPendingCloses()` runs.
- Draw code reads action state from the snapshot installed at step 4 only.
  It must not re-query the router after a stack mutation mid-draw; the
  `CommitPendingOpens` / `CommitPendingCloses` pattern at the boundaries of
  `OnUpdate` and `Draw` exists precisely to make this safe.


## `InputService` Composition Helper

`InputService` owns one `RawInputCollector` and one `InputRouter` and wires
their `BeginFrame` / `Commit` / `BeginFrame` calls together:

```cpp
class InputService {
public:
    void BeginFrame(FrameNumber frame);
    RawInputCollector& Collector() noexcept;
    InputRouter&       Router()    noexcept;
    const RawInputSnapshot& CommitFrame();
};
```

Use `InputService` when:

- the context is headless (unit tests, gameplay e2e, CLI tools) and both layers
  need to be driven together from one owner
- the host does not need a platform backend between the collector and router

Use `RawInputCollector` and `InputRouter` independently when:

- a platform backend (`SdlInputBackend`) sits between them
- the backend owns the collector and exposes `Commit()` while the application
  owns the router and calls `BeginFrame(snapshot)`

The editor application (`HoroEditorApp`) uses the independent path:
`SdlInputBackend` owns the collector; `InputRouter` is a separate member.
`InputService` is used by headless tests and gameplay e2e fixtures.

## `NativeDialog` RAII Context

The editor wraps every native OS dialog (file pickers, folder selectors,
credential prompts) in a `NativeDialog` RAII context token. This ensures:

- `InputContextKind::NativeDialog` outranks all other levels while the dialog
  is open, including an active `ModalRoot` or `ModalChild`
- destroying the RAII wrapper automatically removes the `NativeDialog` token
  and restores the preceding context stack without requiring explicit cleanup
- no ambient `NativeDialog` state is left behind if the caller throws or returns
  early

```text
NativeDialog RAII token alive
    → InputContextKind::NativeDialog active
    → Workspace, tool, modal contexts below it: not active
RAII token destroyed
    → NativeDialog context removed
    → Previous highest context becomes active again
```

If a platform dialog blocks the GUI thread (synchronous modal dialog API), the
`NativeDialog` token is still held for the duration. The input gate remains
correct when the blocking call returns and the GUI thread resumes.

A native dialog opened from inside a modal does not pop the `ModalRoot` or
`ModalChild` tokens from the router. Both the `NativeDialog` token and the
underlying modal token(s) remain live simultaneously. `EditorModalHost::InteractionScope()`
continues to return `{kind = Modal}` because `m_stack` is non-empty. When the
native dialog closes and its RAII token is destroyed, the `NativeDialog` router
context is removed and only the modal context(s) remain. Focus is then restored
inside the top modal, not to the workspace.

## Normative Invariants

The following rules are normative. Any implementation or future change must
preserve them:

1. **Single collector per backend.** One `RawInputCollector` instance owns device
   state for one platform context. A headless test runner uses `InputService`;
   the SDL backend owns its own collector.

2. **Snapshot validity.** A `RawInputSnapshot` reference returned by `Commit()`
   is valid until the next `Commit()`. No consumer stores a reference across
   frames.

3. **Context outlives owner.** An `InputContextToken` must not outlive the object
   responsible for it. Tab, modal, tool, and play-session owners must hold their
   token as a member and destroy it in their destructor or teardown path.

4. **No capture for inactive contexts.** `CapturePointer()` returns
   `CaptureInactiveContext` if the supplied token is not the currently active
   highest-priority context. Callers check `IsContextActive()` before attempting
   capture when unsure.

5. **Modal open cancels capture first.** The router cancels any active pointer
   capture with `ModalOpened` synchronously when the `ModalRoot` token is
   pushed inside `OpenRoot()`. The capture owner's `OnInputCaptureCancelled()`
   is called before the modal's first `OnOpen()` and before the modal draws for
   the first time.

6. **Dim layer is not a barrier.** The dim layer drawn by `EditorModalHost` is a
   visual affordance only. Input exclusivity is enforced by the router through
   context priority. Code must not rely on the dim layer to block actions.

7. **Frame-boundary mutations only.** Modal stack opens and closes committed
   inside `Draw()` take effect at the frame boundary (step 8 of the frame order
   above). Draw code must not read action state after stack mutations; it reads
   state only from the snapshot installed at step 4.

8. **Profile changes are atomic.** `SetActionMap()` and `SetProfile()` either
   apply the entire validated change or return an error; partial application
   never occurs. The previous valid state is preserved on failure.

9. **Neutralize on focus and device loss.** When `WindowInputState::focused`
   becomes false or `pointerDeviceAvailable` becomes false, `Neutralize()` is
   called to release all held controls. No key, button, or axis remains logically
   held after the neutralization snapshot commits.

10. **`OnInputCaptureCancelled()` called exactly once.** The owner does not
    call `CapturePointer()` re-entrantly from inside this callback.

## Testing Obligations

### Layer 1 — Collector

- `BeginFrame` clears per-frame transitions (pressed/released) while preserving
  held state
- `Commit()` seals the snapshot; mutations after commit do not affect the sealed
  value
- `Neutralize()` zeroes all held state and produces `released` transitions for
  every previously held control when called directly
- setting `WindowInputState::focused = false` in the next snapshot causes the
  router's `BeginFrame()` to neutralize capture; no key, button, or axis remains
  held after that snapshot commits (Invariant #9 auto-trigger path)
- setting `WindowInputState::pointerDeviceAvailable = false` produces the same
  neutralization for the pointer-device-unavailable path
- gamepad `ConnectGamepad` / `DisconnectGamepad` round-trips produce valid and
  invalid `GamepadDeviceId`s respectively
- stale `GamepadDeviceId` (from a previous session generation) is rejected by
  `SetGamepadButton`, `SetGamepadAxis` etc.

### Layer 2 — Snapshot

- `State(Key)` and `State(PointerButton)` return the correct `ButtonState` for
  the committed frame
- `FindGamepad(id)` returns the matching `GamepadState` or `nullptr` for
  unknown/disconnected ids

### Layer 3 — Router and Contexts

- pressed/released frame semantics: a transition fires once and is consumed
- context priority and consumption: lower-priority contexts cannot consume
  inputs consumed by higher-priority contexts
- `IsContextActive()` returns `true` only for the highest-priority,
  most-recent token at each level
- modal blocking: `EditorWorkspace` actions are not readable while a
  `ModalRoot` context is active
- `FocusedGuiWidget` outranks `EditorWorkspace` actions during text focus
- all nine `CaptureCancellationReason`s trigger correctly and call
  `OnInputCaptureCancelled()` exactly once
- `CaptureBusy` returned when capture already held
- `CaptureInactiveContext` returned by `CapturePointer()` when the supplied
  token is not the highest-eligible active context (not only after
  `ModalOpened`; any call with a non-active token produces this error)
- `NativeDialog` context outranks `ModalRoot` and `ModalChild`
- context token destruction removes context without leaving dangling references
- `InputService` headless path: `BeginFrame` → `Commit` → `ReadAction` round-trip
  produces correct values without a platform backend
- `SetProfile()` with an invalid profile returns an error and leaves the
  previously active profile unchanged (Invariant #8 — atomic profile application)
- `SetActionMap()` with conflicting or invalid descriptors returns an error and
  leaves the router in its previous valid state

## Related Documents

- [Input Architecture](./input-architecture.md): overall input strategy, layer
  model, action mapping, rebinding, gamepad, XR, and data-bus relationship
- [Editor Modal Host](../editor/editor-modal-host.md): modal workflow contract,
  interaction scope, focus lifecycle, and frame order from the modal owner's view
- [Runtime Lifecycle](./runtime-lifecycle.md): frame scheduler and the host loop
  that drives collector, snapshot, and router in the correct order
- [Platform Abstraction](../foundation/platform-abstraction.md): native dialog,
  window handle, and OS event delivery
- [GUI Screen Host](../editor/gui-screen-host.md): how top-level routes and
  screens interact with the context stack
