# Animation state-machine contract migration

ANI-002.4 extends the actual `AnimationGraphProgram`/`AnimationGraphInstance`
boundary. New Scene activation, application registration and cooked asset loading
remain ANI-004.5/#486 and the Assets owner. Descriptors stay inert.

## Source and caller migration

Current authoring is source schema 4 / graph semantic contract 1.2.0. Source
versions 1/2 use 1.0.0; the pose-composition parent owns source 3 / contract 1.1.0.
Use `MigrateAnimationGraph` for an in-memory candidate or explicitly select
`AnimationGraphSourceMigration::MigrateSupported` when decoding earlier source.
The version-1-only policy still accepts only schema 1. Unknown/mismatched versions
and state payloads disguised as older schemas fail. Stable asset, definition,
node, pin, parameter, state and transition IDs survive migration. The project
version and unrelated asset schemas do not change.

Existing Clip/Parameter/Call/Input/Output/normalized-Blend callers retain their
meaning. Recompile immutable programs at their load/control boundary and prepare
instances from exact pinned skeleton/clip snapshots. Public headers stay owned by
AnimationApi, AnimationRuntime and AnimationGraphSourceCodec respectively; no
root include path or reverse Scene dependency is added. Additive/masked pose
semantics come from the pose parent, not a second state-machine blend kernel.

## State-node behavior

`GraphStateMachineNode` owns typed states and transitions. State sources are
exact clips or asset-local definitions with no external interface inputs.
Definition-call recursion, including state-call edges and unused definitions,
is rejected by the compiler. All source counts and expanded storage/work are
bounded before allocation. The compiled plan supplies control before guarded
children and composition after children. Two lanes give a self-transition two
distinct player/subgraph incarnations; inactive players do not advance. Active
branch traversals are preflighted through the exact shared clip kernel even when
a cut retires the source before pose sampling. Retiring cursors are staged before
slot reuse. Nested active control owners stage exit/interruption records and
retain their next entry generation across retirement.

Larger transition priority wins; equal priority uses smaller stable transition
identity. Conditions form a typed conjunction. Above/Below use strict enter
thresholds and persistent release thresholds, updated transactionally even while
an exit-time gate has not opened. Exit time is exact signed-64-bit-range authored
elapsed nanoseconds since state entry, including cycles. It is a state clock,
not a second clip cursor or a normalized floating threshold.

Each entry clock retains the same exact fixed-point-rate residual as the shared
clip rate-scaling kernel. Zero rate holds state/blend time; negative rate traverses
poses and state time in reverse, while transition blend progression uses the
nonnegative magnitude of the admitted local interval. Gameplay pause supplies no
tick; presentation never advances this owner. Overflow or excessive complete loop
work fails the attempted tick instead of dropping events or advancing partly.
Direct Loop clips use their descriptor duration; PingPong state cycles span the
complete out-and-back period. Once/ClampForever have no cyclic state-loop events.
Subgraphs optionally declare a positive exact cycle, with the graph owning its
state clock and each actual clip still owning its cursor.

At most one new transition starts per attempted tick. A new destination samples
its entry boundary that tick, then advances on subsequent ticks. Duration zero
cuts complete immediately. An admitted higher-priority interruption starts from
the last committed composed pose and an independently reset destination; scalar
curve weight is supplied to the shared pose-composition seam. Linear, SmoothStep,
EaseIn and EaseOut are finite, clamped scalar curves. The current transition's
Source/Destination/Either/None policy chooses allowed interruption origins.

## Transaction and event consumers

Evaluate stages clocks, latches, active states, actual player cursors, composed
poses, parameters and state lifecycle records privately. Commit publishes them
together after its cancellation fence; Abort/failure/cancellation publishes no
new tick or event. Failed attempts preserve queued triggers for retry. No state
event invokes gameplay, Audio, VFX or a callback during evaluation.

`CopyStateEvents` reads an exact resident pose-publication handle into complete
caller-owned capacity, with written count returned only on success. Instance,
tick, compiled occurrence, ordinal, stable source/state/transition, entry generation
and directed loop ordinal identify records. Entry generations distinguish self
transitions. Loop ordinals count directed traversal occurrences, including reverse;
they are not a mutable clip-notify cursor. Event ordering follows actual compiled
control execution and source-before-destination loop observation. Clip notifies
remain their separate ticket/owner.

Both resident publications retain immutable event blocks like pose/parameter
blocks. A later third publication retires the oldest handle. Wrong-owner,
malformed/stale and insufficient-capacity copies fail before modifying output or
written count. Shutdown is idempotent, stops execution and discards private work
while resident copied queries remain readable. Failed reload preserves last-good
state and queued triggers; successful explicit next-generation replacement starts
fresh state and retires old handles. No pose or event storage borrow escapes.

The normal allocation probe, actual source/compiler/instance API regressions and
public consumer cover this contract. Native/hosted results must be recorded from
the actual final source after published-parent reconciliation; authored tests alone
are not qualification evidence.
