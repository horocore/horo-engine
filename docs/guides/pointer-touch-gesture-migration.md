# Pointer, touch and gesture integration

HORO-743 adds typed bounded pointer recognition rather than platform-owned UI
state. `UiPointerInteraction.h` belongs to `HoroRuntimeUi`;
`UiPointerInput.h` belongs to `HoroRuntimeUiInput`. Public consumers receive only
these targets' staged headers. Native SDL identities remain private to InputSdl.

These implementation admission limits do not narrow the ticket's acceptance
criteria or relax the normative Runtime UI ownership, accessibility, lifecycle
and frame-hot contracts. Over-limit requests require explicit typed outcomes;
the supported bounded route must still deliver the complete interaction behavior.

## Composition and delivery

Create an adapter at an owner safe point using the real Input router/context,
the currently presented canvas owner identity, copied target policies and four
registered semantic actions. Supply the actual immutable hit-test snapshot,
resolved canvas mapping, explicit collector-space viewport, route/default handler
and shared sequence counter on each Pump. Bind `UiPointerInputSurface::owner` to
the existing canvas aggregate or `UiAnimationRuntimeParticipant`; the latter
implements `UiPointerInteractionHost` without lending its mutable owner.

Deliver committed Input at the host's input cutoff before animation prepares its
next frame, using the last successfully presented interaction generation. A
prepared canvas does not admit input, and extraction cannot grant input authority.
Do not retain the surface, dispatcher event, current tree or control references.
SDL source coordinates match window logical mouse coordinates; map them through
the explicit source viewport, not directly through an assumed drawable size.

Success is bounded to sixteen simultaneous pointers, sixty-four samples, 256
target/layout records, 64 Input contexts and sixteen current/previous gamepads.
Action maps/profiles are limited to512 descriptors/overrides and32 bindings each.
Excess capacity, malformed input, foreign owners and backwards time return typed
errors before admission where possible. Partial delivery exposes its applied
default prefix; it does not undo already admitted application effects.

## Lifecycle and migration

- Existing synchronous owner/control APIs remain. New virtual pointer admission
  adds a vtable/base to UiAnimationOwner and an interface to its runtime participant;
  rebuild all RuntimeUi/RuntimeUiInput/runtime-integration consumers. Do not rely on
  binary layout compatibility or expose a second mutable owner for compatibility.
- `RawInputSnapshot` now contains a fixed touch array and overflow evidence.
  Rebuild Input consumers; copy snapshots only at the existing collection boundary.
  Contact incarnations are physical identities, not UI handles or player IDs.
- Call Suspend while the live owner is available before removing an Input
  context or replacing policy. Rebind copies a full new presented identity;
  held sources remain disarmed. Aggregate reload/shutdown already cancels its
  real controls; adapter shutdown releases native Input capture without invoking
  a dead owner. Keep the Input router alive until its tokens have been released.
- Pointer hosts implement `PointerInputEligible` against their actual currently
  presented instance, canvas, document, tree, interaction and view. View-level
  eligibility alone cannot authorize a retained recognizer after replacement.
  The adapter checks that complete lineage before consuming actions or collecting
  physical edges, cancels only its old authority and returns `NeedsRebind` when
  replacement invalidates it. Owner dispatch retains its independent rejection.
- Adapter and participant addresses are stable. Recognizer/aggregate moves are
  permitted only at quiescence. Destruction or move during Pump violates a live
  borrow and terminates; callback Shutdown is explicitly supported.
- Preserve route default prevention. A prevented begin may retry, but cannot
  authorize update/drop, and physical slop cannot become a tap or long press.
  Callback revocation, context/frame replacement and generation loss stop further
  defaults. Normal release is not cancellation, and text focus/editing survives a
  successfully admitted tap.
- Register canonical keyboard actions via DefaultUiPointerActions, or supply four
  equivalent bounded digital actions in the exact context. Context action uses
  Shift+F10 by default; Space picks/drops, Enter activates/drops and Escape cancels.
  These are remappable semantics, not native key ownership inside RuntimeUi.

## Regression targets

HoroRuntimeUiHitTestingTests covers recognition, routed prevention/revocation,
hoverless pinch, bounds, terminal semantics and quiescent moves.
HoroRuntimeUiAnimationIntegrationTests covers committed Input through the real
RuntimeHost participant, actual control state, keyboard alternatives, reload,
generation loss, callback shutdown and allocation measurement.
HoroInputTests covers collection/incarnations/capacity and routing ownership.
HoroInputSdlTests covers exact-window native finger normalization, cancellation,
stale/overflow/malformed batches and duplicate mouse suppression.
HoroRuntimeUiPointerPublicHeaderConsumer and the existing animation-integration
public consumer cover staged ownership and concrete host compatibility.

These are authored coverage targets, not claims that a test has run. Native SDL,
headless runtime, consumers and platform configurations require their actual
configured validation; no Linux object result proves another platform's execution.
