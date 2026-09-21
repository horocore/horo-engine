# ADR-180: Runtime UI Interactive Control State Machines and Default Actions

- **Status**: Proposed
- **Date**: 2026-09-21
- **Scope**: Runtime UI control state, normalized input edges, default-action ordering, repeat, and lifecycle
- **Issue**: [RUI-005.5](https://github.com/HoroCore/horo-engine/issues/739)
- **Jira**: [HORO-739](https://horo-engine.atlassian.net/browse/HORO-739)
- **Parent**: [RUI-005](https://github.com/HoroCore/horo-engine/issues/736)
- **Related**: [ADR-073](073-runtime-ui-ownership-scope-and-update-order.md), [ADR-078](078-runtime-ui-input-context-and-player-routing.md), [ADR-176](176-runtime-ui-element-and-control-taxonomy.md)
- **Normative documents**: [Game UI and HUD](../architecture/runtime/game-ui-and-hud.md)

## Context

The Runtime UI taxonomy defines controls and typed action values, but a control
still needs an explicit owner-bound state machine for focus, press, editing,
value, repeat, cancellation, and default-action ordering. If those transitions
are left to device handlers or renderer widgets, pointer and keyboard behavior
can diverge, disabled controls can retain capture, and a handler can observe or
commit a value before the event route has decided whether the default applies.

The state must also remain safe across retained-tree replacement and shutdown.
Input is therefore evidence from a presented owner generation, not an implicit
reference to a native widget or gameplay object.

## Decision

### 1. Core controls use closed typed descriptors and state projections

`Button`, `Toggle`, `Slider`, and `TextInput` are represented by a closed typed
descriptor variant and a matching typed state variant. Descriptors carry only
Horo-owned identity, bounded action payloads, availability/focus policy, and
control-specific finite values. They contain no callback, renderer/style state,
native handle, provider, editor object, or gameplay reference.

The mutable state machine is owned by one Runtime UI owner/revision generation.
Its input and output remain Horo-owned values so render, editor, platform, and
gameplay layers can project or consume the result without owning the state.

### 2. Normalized input is owner- and element-fenced

Inputs carry exact `UiActionSource` evidence, a strictly increasing owner
sequence, a normalized activation source, and bounded control-specific data.
Foreign owner/element evidence, malformed edges, stale sequences, and exhausted
repeat ticks fail closed. Raw device types do not cross this contract.

Pointer press/release and normalized submit press/release share the same
pressed transition contract. Focus gain is admitted only for enabled,
focusable controls. Disabled controls cannot acquire focus or press state.
Focus loss, cancellation, disabling, retirement, and shutdown clear transient
press, repeat, capture-equivalent, and editing state. Text cancellation restores
the value captured when the edit session began.

### 3. Default actions have a two-step route boundary

`Handle` may stage exactly one default action and return `DefaultPending`; it
does not commit toggle values, scalar values, or submitted text. The route owner
must then call `ApplyDefault` or call `SuppressDefault` after a handler prevents
the default. New input is rejected while the decision is pending, except for
cancel and focus-loss cleanup. A successful application emits one typed action
with the exact owner, element, action identity, modality, sequence, repeat flag,
and bounded payload.

This ordering makes handler prevention observable before state mutation and
ensures repeated input cannot reorder or duplicate a default action.

### 4. Repeat is bounded owner-tick work

Repeat policies use finite initial delay and interval ticks supplied by the
owner. Each admitted tick emits at most one staged default action; a delayed
caller does not cause an unbounded catch-up burst. Repeat is disabled for text
input and is cleared by release, cancellation, focus loss, availability change,
retirement, and shutdown. No frame-hot control operation performs I/O, waits, or
allocates; fixed-capacity action and text storage is reserved by the value types
and control creation path.

### 5. Lifecycle is explicit and fail-closed

An active control may enter `Retiring`, which closes new input and default
application after transient state is cleared. `Shutdown` is idempotent and
stops the machine. A replacement presentation creates a new owner/revision
generation; late input from the previous generation cannot affect it.

## Consequences

- Pointer, keyboard, accessibility, programmatic, and future gamepad adapters
  share one deterministic control transition contract.
- Event routing can inspect and prevent a default before the control commits a
  toggle, scalar, or submitted text value.
- Fixed-capacity values and owner ticks keep the frame-hot path bounded and
  allocation-free after creation.
- Invalid ownership, ordering, capacity, and lifecycle evidence produces typed
  errors instead of silently mutating a replacement control.
- Route owners must resolve each `DefaultPending` result before admitting the
  next ordinary control input.

## Rejected Alternatives

### Native or renderer widget state as the authority

Rejected because native handles and renderer widgets have different lifetimes,
threading rules, and platform semantics. They cannot own generation-fenced
Runtime UI behavior.

### Immediate mutation during event handling

Rejected because a route handler must be able to prevent the default action
before a toggle/value/text submission is committed, and because it makes
capture-loss and stale-route rollback ambiguous.

### Callback-bearing control descriptors

Rejected because callbacks retain unbounded ownership and cross the Runtime UI
boundary into gameplay, editor, provider, or platform lifetimes. Stable action
identities and bounded typed payloads keep ownership explicit.

### Timer- or thread-owned repeat scheduling

Rejected because it introduces hidden lifetime, cancellation, and ordering
authority. Owner-supplied ticks make repeat deterministic and bounded within the
existing Runtime UI update order.
