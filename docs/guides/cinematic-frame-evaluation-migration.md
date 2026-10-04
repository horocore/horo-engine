# Cinematic Per-Frame Evaluation Migration

Runtime hosts should build one `SequenceFrameEvaluationPlan` during player
preparation and retain its borrowed callback contexts until the player is retired.
The plan owns only bounded backend-neutral descriptors; concrete scene, camera and
event owners remain injected adapters.

At a fixed or admitted service boundary:

1. Snapshot eligible players and call `OrderSequenceFramePlayers` using caller-owned
   storage.
2. Create each `SequenceFrameCursor` from the current `SequencePlayerSnapshot`.
   Use `EmitCurrentBoundary` for initial play and `SuppressCurrentBoundary` after a
   seek or external discontinuity.
3. Supply fixed-capacity value, occurrence and camera-cut spans to `Evaluate`.
4. Reserve the entire event span with `SequenceFrameHooks::eventStage`. This
   replaces the per-occurrence `eventHook`; update existing callback signatures
   to return `Result<void>` and accept `const BorrowedCallbackContext&` plus a span.
   Explicitly construct `BorrowedCallbackContext{&owner}` from the concrete borrowed pointer and recover it using
   `Get<T>()`; a null or mismatched type returns null without an unchecked cast. Reservation failure publishes no
   cursor or values. This hook stages data and never invokes gameplay.

For attempted simulation ticks, use `CinematicEventSession` over the session's
`CinematicRuntimeService` and `CinematicEventDispatcher`. Its `Activate` admits
both owners transactionally. Call `BeginTick`, `Evaluate` once for each eligible
player with distinct retained scratch, then `CommitTick` only after the host's
source tick succeeds. Call `AbortTick` on every failed attempt. Preparation does
not publish live cursor/value/camera/completion state; commit publishes all
prepared frames. Drain the dispatcher at the destination gameplay owner's later
safe point. The existing service `Evaluate`/`EvaluateClock` APIs remain for already
committed source boundaries; they cannot be used to speculatively mutate a tick.
The fixed-tick composition admits committed-simulation players and charges retained
cooked key/payload storage to their runtime activation budget. Other clock owners
use their explicitly committed source boundaries and injected destination adapters.

Apply, camera, and completion hooks must not mutate or reenter the runtime during
publication. Scratch and hook contexts remain borrowed through tick retirement.
Run commands and lifecycle changes outside the transaction. Use the session's
`Stop` to preserve admitted events, `Cancel` to discard undispatched work, and
`Close` before scene or module teardown. Stop completion, restore, and release
remain owned by the runtime service after committed work retires.

Cook event names, script-export versions, payload types, time, and reverse policy
with `CookScriptEvents`. Only that validated adapter can construct a
`CookedEventPlan`; arbitrary bytes cannot bypass schema validation. Activation
compares the complete cooked/evaluation key semantics and admitted context.
Sampling/dispatch never resolves function names or constructs payload maps.

`ClearResults` acknowledges diagnostic records without erasing delivery identity.
Deduplication retains one forward/reverse traversal watermark per cooked key and
player generation, with storage reserved at activation. Long-lived players can
deliver arbitrarily many successive traversals while acknowledged results recycle.
Old traversal submissions remain terminal; cancellation retires the generation's
watermarks. Cursor resets after seek, resume, or clock discontinuity create a fresh
traversal ordinal so later legitimate crossings do not alias prior delivery.

Native modules register inert callbacks through `GameRegistrationContext::events`.
Host composition calls `BindGameplayEvent` on the loaded exact module generation,
then registers the returned leased adapter with the dispatcher. Its lease pins
callback context, registries, and code image. Reload retirement revokes admission
and refuses unloading while a lease remains. Revoke registrations before native
teardown; editor preview rejects native gameplay adapters.

The combined event and persistence registration context advances
`GameplaySdkBoundaryVersion` to 8 and the gameplay build fingerprint to
`gameplay-sdk8`. Boundary-7 modules built against either independently developed
registration layout are incompatible with this combined context. Rebuild native modules and
their SDK consumers; old artifacts fail compatibility validation before any
factory or callback executes. `GameplayApi` owns `GameEventRegistry.h`,
`CinematicModel` owns cooked values/errors, `CinematicRuntime` owns dispatcher and
tick transaction headers, and the host-side `CinematicScriptBridge` owns cook and
native adapters. `Foundation` owns the allocation-free `BorrowedCallbackContext.h` used by
these new callbacks. Its identity is local to the code image, never serialized;
construct and recover each context in the same retained code image.
Public-header consumers are staged per owning target.

Recreate a cursor whenever the player control revision changes. A stale cursor is a
typed error and must never be patched in place. Pause, stop, cancellation, scene
replacement and shutdown close admission at the owning service boundary. Frame-hot
code must not resize scratch buffers, discover services, resolve strings, block, or
retain backend-native handles.
