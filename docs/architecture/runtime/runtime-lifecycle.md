# Runtime Lifecycle Architecture

## Purpose

This document defines process startup, host composition, frame execution,
fixed-step simulation, play state, suspension, scene transitions, and shutdown
for editor and game runtimes.

## Core Decisions

- Hosts own runtime composition and lifecycle.
- Simulation advances with a fixed timestep; presentation uses variable frame
  time and interpolation.
- Frame phases and mutation boundaries are explicit.
- Editor mode and play mode use the same runtime services but separate
  authoritative scene state.
- Scene transitions occur through state machines, never by replacing live state
  from arbitrary callbacks.
- Shutdown is ordered, idempotent, and stops producers before dependencies.
- Headless execution follows the same lifecycle without window, input, GUI, or
  graphics capabilities unless requested.
- Network-capable hosts validate one ADR-102 standalone/client/listen/dedicated
  mode plan before world publication. Runtime mode stays fixed for the host
  generation and gameplay authority remains world/session-generation scoped.

## Runtime Owners

```text
Process Host
  +-- Platform Services
  +-- Observability Runtime
  +-- Configuration Service
  +-- Job System
  +-- Engine Data Bus
  +-- Asset Services
  +-- Scene Runtime
  +-- Physics Runtime
  +-- Audio Runtime (optional)
  +-- Network Runtime (optional)
  +-- Renderer (optional)
  +-- Runtime UI Service (optional model-only or rendered composition)
  +-- Application Services
  +-- Runtime Console Service (optional)
  +-- MCP Service (optional)
  +-- GUI / Editor Session (HoroEditor only)
```

The composition root constructs dependencies in order and shuts them down in
reverse dependency order. No subsystem starts a hidden process-global runtime.

## Process State Machine

```text
Created
  -> Initializing
  -> Ready
  -> Running <-> Suspended
  -> Stopping
  -> Stopped

Initializing -> Failed
Running      -> Failed
```

Every transition is observable and happens once. A failed initialization
unwinds only resources that were successfully initialized.

## Startup

Startup order:

1. parse launch arguments without starting subsystems
2. initialize emergency diagnostics and platform directories
3. start observability and emit build/system identity
4. resolve configuration
5. validate product capabilities and resolve one ADR-102 runtime network plan
6. construct platform, jobs, data bus, and asset services
7. initialize the requested network runtime/transport and local-player composition
8. initialize the requested omitted/null/device audio composition through ADR-062
9. initialize requested renderer and window capabilities
10. initialize the selected omitted/model-only/rendered Runtime UI composition
11. construct application services and unpublished world candidates with exact roles
12. load and atomically publish the initial scene/world composition
13. start optional runtime console, MCP, and GUI adapters
14. enter `Ready`, then `Running`

Arguments, environment, and persisted settings are resolved through
[Configuration System](../foundation/configuration-system.md). Startup failures return
typed errors and retain enough diagnostics for CLI or GUI presentation.

### Runtime network mode lifecycle

[ADR-102](../../adr/102-runtime-network-modes-and-authority-exposure.md) separates
the modes supported by an artifact from the one plan selected for a host
generation. Unsupported requests and incompatible listener/outbound/local-player/
world-role combinations fail before any world or authority view is published.
Standalone omits networking by default; client, listen-server and dedicated-server
plans compose only their declared services.

Transport `Connected` is not a gameplay lifecycle transition. A client role view
gains a session generation only after ADR-098 activation. Listen-server authority
and local-client worlds publish as distinct roles even in one process. Dedicated
mode omits presentation/local-player services without changing server authority.

Travel preserves mode while replacing scene and authority generations; listen-
server travel commits compatible server/client candidates together. Reconnect
repeats admission under a new session generation. Disconnect unpublishes the
client session view and cannot silently turn the world into standalone. Changing
runtime network mode requires bounded host recomposition rather than a scene
transition or global-flag mutation.

## Frame Phases

The canonical interactive frame is:

```text
1. BeginFrame
2. PollPlatformEvents
3. BuildInputSnapshot
4. ApplyQueuedOwnerThreadCommands
5. FixedUpdate zero or more times
6. VariableUpdate
7. RenderExtraction
8. RenderExecution
9. RenderGui
10. Present
11. CommitDeferredLifecycleChanges
12. EndFrame
```

Frame phases are profiler scopes. A subsystem cannot mutate another owner's
state merely because both execute in the same frame.

`RuntimePhase` exposes this complete ordering to both graphical and headless
hosts. Headless compositions register successful no-op participants for omitted
capabilities rather than defining a second phase contract. Presentation is
admitted only after `RenderExtraction`, `RenderExecution`, and `RenderGui` have
all completed successfully in the same frame. Failure or cancellation skips all
later normal phases, including `EndFrame`; owned frame scopes perform cleanup by
abort/RAII instead.

Main-thread job continuations are pumped with phase-specific budgets. State
required by fixed simulation is committed before fixed steps begin. Destructive
scene and renderer lifecycle changes are committed only at their documented
safe points.

[ADR-159](../../adr/159-xr-action-tracking-and-input-projection-ownership.md)
adds no runtime phase. After native XR event handling and before `BuildInputSnapshot`
commits, XRRuntime gets one bounded owner-thread action/tracking sample and the
host-composed adapter joins it to Input's transaction. Results after the cutoff wait for
the next frame. FixedUpdate consumes only its tick-assigned immutable Horo projection;
presentation-late poses used during render execution cannot rewrite simulation input.
Loss commits Input neutralization before a replacement XR generation is eligible.

[ADR-033](../../adr/033-presentation-and-display-ownership.md) specializes these
safe points for presentation: apply host/window intent and publish a revisioned
pending output candidate through owner-thread commands before layout/extraction.
Realize that candidate and commit active surface state at `RenderExecution`
entry before native frame acquisition; mismatched or unrealized candidates skip
that output. Commit final destruction through deferred lifecycle work after
retirement. Minimized/zero-pixel output suspends presentation, not the simulation
clock. No additional runtime phase is introduced.

[ADR-062](../../adr/062-audio-runtime-ownership-and-update-order.md) specializes
audio participation without adding phases. Owner-thread commands commit audio
lifecycle/focus/device/scene requests; `VariableUpdate` drains producer and
callback queues and publishes bounded callback work; deferred lifecycle commit
retires acknowledged scene-context barriers; `EndFrame` publishes bounded
control snapshots. The hardware callback advances on its independent sample
clock and never calls runtime phases.

[ADR-073](../../adr/073-runtime-ui-ownership-scope-and-update-order.md) specializes
Runtime UI without adding phases. Owner commands commit scope/document/resource
lifecycle work. FixedUpdate publishes gameplay state but never advances UI.
VariableUpdate routes input against the last successfully presented interaction
snapshot, resolves committed bindings and publishes layout/focus/hit-test state.
RenderExtraction emits immutable per-view UI snapshots and RenderExecution composes
them. Runtime game UI is not `RenderGui`; that phase remains host/editor/development
GUI composition. Presentation success adopts the next interaction revision and
deferred lifecycle commit retires old scope/render generations.

[ADR-077](../../adr/077-runtime-ui-animation-clock-and-time-domain.md) refines UI
time without adding a phase or changing host clock ownership. Before Runtime UI
VariableUpdate, the host supplies one immutable clock-evidence snapshot containing
committed simulation ticks and admitted presentation time. Runtime UI derives its
simulation, unscaled, screen-transition, preview, deterministic-test and manual
domains and evaluates every active UI timeline exactly once before style/layout
publication. Render extraction/execution never advances them.

[ADR-117](../../adr/117-playback-ownership-frame-order-and-determinism.md) refines
Cinematic Runtime without adding a phase. Before each fixed or permitted service/
presentation boundary, the session-owned service drains commands through that
boundary's cutoff and publishes one immutable player batch ordered by domain,
descending priority and stable identity. Commands or worker completions after the
cutoff cannot alter the active batch. Failed attempted ticks discard staged cinematic
cursors/values/occurrences; tick commit advances them and releases occurrences.

[ADR-119](../../adr/119-camera-authority-during-cinematics.md) adds no runtime phase.
After `VariableUpdate` and before `RenderExtraction`, each live camera-view context
drains eligible gameplay/cinematic/editor proposals and commits one immutable camera
selection. Every pass for that rendered frame consumes that selection; a cut, stop,
cancel or late completion after the cutoff is eligible only for the next commit.
Multiple fixed ticks may cross multiple cut keys, but only the final eligible
selection at this boundary is promised a rendered frame. Context teardown retires
the snapshot only after frame consumers complete.

[ADR-120](../../adr/120-cinematic-event-dispatch-and-audio-coupling-boundary.md)
also adds no phase. Cinematic EventTrack evaluation stages bounded occurrences; only
a successfully committed source tick makes authoritative gameplay occurrences
eligible. The session dispatcher drains them at each destination owner's next
permitted boundary, where a typed adapter may submit a normal command/operation.
Handlers cannot reenter the active sequence batch or imply same-tick mutation.
AudioTrack bundles follow ADR-062's existing Audio control/callback publication
boundaries and ADR-068 sample correlation; render interpolation never emits them.

[ADR-122](../../adr/122-cinematic-trigger-sources-and-capability-policy.md) adds no
phase and no direct player-construction path. Every gameplay, scene-autoplay, event or
tooling source enqueues the same typed start request. Owner commands validate the
complete principal/profile/trust/capability/approval/session/world/effect plan before
the next eligible Cinematic batch; denial publishes no player or domain side effect.
Scene autoplay becomes eligible only after aggregate scene activation, and gameplay-
event starts only after their source occurrence commits. Late approval/preparation
results join a later boundary only when every request generation remains current.

[ADR-123](../../adr/123-vfx-cpu-stage-order-determinism-and-gameplay-coupling.md)
specializes each gameplay-coupled VFX fixed step without adding a host phase. Within
the VFX scheduler node, one candidate runs Spawn, Initialize, Forces, Integrate,
Collide and Kill in order against the tick's immutable inputs/query snapshots. After
successful candidate commit, read-only Extract publishes immutable output without
adding another host phase. The
candidate particle generation and bounded gameplay occurrences publish only with
successful tick commit. RenderExtraction later reads the committed post-Kill state.
Failure/cancel discards all stages, and gameplay consumes committed output at its next
permitted owner boundary rather than reentering the source tick.

[ADR-078](../../adr/078-runtime-ui-input-context-and-player-routing.md) also adds no
phase. `BuildInputSnapshot` publishes device/action/assignment evidence; Runtime UI
VariableUpdate applies one ordered context stack against the last presented
interaction snapshot, publishes a consumption ledger and sends only filtered
gameplay/UI owner commands onward. Context/modal/focus/capture teardown commits at
the same ADR-073 lifecycle cutoffs.

[ADR-079](../../adr/079-runtime-ui-binding-provider-schema-identity-and-lifetime.md)
keeps provider ownership outside Runtime UI while preserving one coherent read
revision. Provider owners publish typed immutable snapshots at their safe points;
Runtime UI freezes a compatible set during VariableUpdate and emits expected-
revision writes as commands for provider-owner safe points. Provider revocation
closes admission and drains snapshot/command/UI leases through deferred lifecycle
retirement before player/scene/game/module storage or code disappears.

[ADR-080](../../adr/080-runtime-ui-presentation-scope-layer-and-route.md) prepares
route operations privately and commits complete stack generations at the same
ADR-073 lifecycle cutoff. VariableUpdate evaluates route-generation transitions,
then extraction publishes immutable per-view presentation plans. Removed routes
retire only after interaction, snapshot, render and transition leases close.

[ADR-081](../../adr/081-runtime-ui-and-localization-ownership-boundary.md) starts
Localization before Runtime UI consumers and publishes immutable catalog/locale
snapshots. Runtime UI freezes one compatible snapshot during VariableUpdate and
publishes text/font/layout as one generation. Locale/catalog replacement may retain
last-good UI until preparation completes; shutdown retires UI leases before
catalogs, formatter strategies, namespaces or Assets disappear.

[ADR-082](../../adr/082-runtime-ui-accessibility-capability-and-ownership.md)
publishes semantic snapshots from that same UI/interaction generation and admits
native exposure only after matching presentation. Platform dispatch is bounded and
thread-affine; native actions return as revision-checked commands for a later
VariableUpdate. Shutdown closes action/dispatch admission and retires semantic/
native leases before Platform, Localization, Input or Configuration disappear.

[ADR-083](../../adr/083-ui-template-identity-schema-and-expansion.md) keeps template
insert/rebase/detach in authoring document transactions. UI cook resolves accepted
template revisions and flattens linked instances into ordinary cooked elements;
packaged runtime activation therefore follows ADR-073 without source-template or
live-propagation phases. Preview replacement prepares a complete candidate and
retains last-good UI on expansion/dependency failure.

## Time Model

The host tracks:

- monotonic real time
- clamped presentation delta
- positive rational simulation rate and its revision
- fixed simulation delta
- accumulator
- interpolation alpha
- simulation tick
- frame number

```cpp
accumulator.AddScaled(Clamp(realDelta, RealDuration::Zero(), maxFrameDelta),
                      simulationRate, simulationScaleRemainder);

while (accumulator >= fixedDelta && steps < maxCatchUpSteps) {
    FixedUpdate(fixedDelta);
    accumulator -= fixedDelta;
}

alpha = accumulator / fixedDelta;
Render(alpha);
```

`simulationRate` changes how much clamped elapsed time enters the accumulator;
it does not change `fixedDelta`. Scaling uses checked rational/fixed-point math
and the host clock owns `simulationScaleRemainder` across frames. `AddScaled`
preflights overflow and commits the accumulator plus remainder atomically; it
does not round through floating-point seconds or lose an unrepresented fraction.
The baseline rate is `1/1`. A rate change commits through the owner-thread command
boundary before fixed-step scheduling, preserves that remainder, and records a
revision for replay evidence. Negative rates are invalid. Gameplay
pause is separate composed state and contributes no accumulator time; it is not
represented by a zero rate.

Long stalls do not create an unbounded simulation spiral. When
`maxCatchUpSteps` is reached, the host records dropped simulation time according
to the configured policy.

The baseline Phase 1 policy uses a 16,666,667 ns fixed step, at most five
catch-up ticks, and a 250 ms maximum real delta. Negative samples normalize to
zero. After catch-up saturation, whole fixed intervals are discarded while the
fractional remainder is preserved, keeping interpolation alpha in `[0, 1)`.
The first frame and first frame after resume establish a zero-delta baseline.

`FixedStepContext::simulationTick` is the one-based tick currently being
attempted. It becomes committed only after every fixed-step participant
succeeds. `FrameContext::completedSimulationTick` is the count of committed
ticks and is therefore zero before the first successful tick. Variable update
and render extraction observe that committed count together with interpolation
alpha.

Participants with staged state use the canonical fixed-attempt boundary. After
all `OnFixedUpdate` callbacks succeed, the scheduler invokes every participant's
fallible `PrepareFixedCommit`. Failure, a lifecycle state change, or cancellation
before the final fence invokes `AbortFixedUpdate` in reverse registration order
while the same dispatch evidence remains active. Abort must also tolerate owners
whose update callback was never reached.

Once all preparation succeeds, the scheduler stamps the exact attempted tick into
its opaque dispatch facts, commits its counters and any single-step receipt, then
invokes `CommitFixedUpdate` in registration order. Publication is nonthrowing and
must not allocate, perform I/O, dispatch external callbacks, change lifecycle state,
or reconsider cancellation. Cancellation after this fence cannot undo or partially
publish the committed tick. Preparation sees the prior committed tick; publication
sees the newly committed tick through the same issuer-bound dispatch evidence.
Retiring the issuer after that fence closes future admission but leaves the exact
active committed fixed proof readable until publication ends. Retirement before
the fence still rejects commitment; ending the dispatch revokes this final read.
Retirement before the fence also denies ordinary fact reads during rollback.
An owner that already admitted a candidate may authenticate that rollback by first
reading against its explicitly bound issuer on the owner thread (accepting only
`Valid` or `Retired`) and then comparing the exact retained opaque evidence with
`RefersToSameDispatch`. This identity-only comparison does not grant execution,
fact-read or commitment authority and deliberately survives expiry; it cannot
replace admission. Numeric tick/attempt copies alone never authorize rollback.
The lifecycle's publication entry point is private to the scheduler. Default
callbacks preserve existing participants; they do not make legacy owner mutations
transactional. Each staged owner remains responsible for its candidate, no-fail
publication, and abort implementation.

The Editor forwards the actual issuer and fixed context through its screen,
workspace, Play controller and application-injected Scene host. Only the Scene
host composes Animation before native Physics; it is not also registered as a
second runtime participant. A Play failure aborts its own staged candidate before
closing that Play unit and remains an Editor-local failure. Global scheduler
rollback discards candidates without consuming a queued paused step. That step
is consumed only after genuine same-attempt commitment. Stop requested during an
attempt retains the Scene/behavior owners until rollback or publication releases
the reservation; ordinary presentation performs deferred teardown. Presentation
projection and viewport extraction remain outside fixed preparation/publication.

[ADR-061](../../adr/061-animation-ownership-update-order-and-clock.md) applies
this policy to animation. Animation consumes the unchanged fixed quantum once
per attempted tick. Host simulation rate is already represented by tick cadence;
animation and physics do not multiply it into their delta again.

The scheduler retains allocation-free cumulative counters for committed ticks,
dropped time and steps, catch-up saturation, negative samples, and maximum-delta
clamps. Hosts query snapshots without locks or logging in the hot path;
observability adapters may publish them at their own bounded cadence.

### Cinematic Fixed Control Clock

ADR-014 defines an optional Runtime-owned unscaled fixed control clock for
presentation-only cutscenes that continue while gameplay is paused. It is a service
clock, not FixedUpdate: it advances no simulationTick/completedSimulationTick count
and never runs physics, AI, controller movement or gameplay callbacks during pause.
The host services its bounded quanta in VariableUpdate before presentation sampling.

Its validated configuration explicitly supplies a positive quantum and catch-up
limit. Baseline defaults reuse 16,666,667 ns, five quanta and the 250 ms elapsed-time
clamp above; excess whole quanta are dropped with counters, keeping the remainder.
The first frame/resume establishes zero delta. Host suspension freezes the clock.
Identical recorded control-quanta and setting histories reproduce cinematic
sampling; elapsed wall time or a different dropped-time history alone does not.
Gameplay pause tokens compose at a safe point before the next simulation tick;
releasing a cinematic token cannot unpause another owner's reason. See
[Cinematic Sequencer Architecture](./cinematic-sequencer-architecture.md).

## Error And Allocation Contract

Expected participant failures use `Result<void>` and stable error codes.
Exceptions are forbidden as participant control flow, but the host contains an
unexpected exception at the participant boundary, aborts owned frame state, and
translates it to a typed fatal runtime error.

The allocation-free guarantee applies to the successful steady-state scheduler
path: clock math, context construction, participant traversal, and phase
dispatch. Error construction, diagnostic formatting, exception containment,
and shutdown failure handling may allocate. GUI and concrete renderer internals
retain their own separately measured allocation contracts.

Variable-rate update is not used for deterministic physics integration.

## Simulation Tick

One fixed tick executes:

1. consume the input command state assigned to the tick
2. stage Gameplay/AI/Nav and owner-admitted Cinematic movement/facing, animation
   parameters and kinematic-platform targets under the effective authority snapshot
3. evaluate authoritative Animation pose/root motion and freeze Physics-owned
   Character query/support/platform-motion evidence
4. run Horo Character platform carry and bounded query movement, staging its root
   and Physics commands
5. apply staged commands and step Physics once
6. publish Physics results, then finalize Character support/attachment and its
   authoritative collision-root transform without a second move
7. compose typed post-Physics Animation pose overrides and candidate pose
8. commit deferred entity/component changes
9. atomically publish committed subsystem state, transforms, events, and previous/current
   state for interpolation

[ADR-089](../../adr/089-character-controller-ownership-implementation-and-update-order.md)
owns this Character-specific dependency order and the separate capsule up, desired
heading, root rotation, platform angular carry and visual orientation authorities.
Presentation frames neither accumulate root motion nor update collision state.

[ADR-084](../../adr/084-canonical-physics-solver-units-and-tolerances.md) fixes the
initial Physics step to pinned Jolt CanonicalV1 under one owner thread and serial
private job adapter. Physics consumes only the host fixed delta (default qualified
at 60 Hz), completes all native work before publishing transforms/events and never
measures render/wall time. Parallel solver jobs require a separately qualified
private profile and must join before step publication.

[ADR-088](../../adr/088-physics-determinism-capability-and-support-tiers.md)
requires a stronger-than-`Unspecified` world to close one canonical tick-indexed
input/Physics command frame before the step. Its exact tier/fingerprint/evidence are
immutable for that world generation. Worker completion, process event order,
variable update and wall time cannot select command order, streaming/origin commit
tick or authoritative query results. A first divergence invalidates the stronger
session and follows its explicit fail-closed policy.

[ADR-092](../../adr/092-character-controller-determinism-and-state-composition.md)
captures Character only from the atomically committed stage-9 state and pairs it
with the exact Scene/Physics/world tick, structure, origin and determinism
fingerprint. The lifecycle safe point lends immutable owned checkpoint data to
workers; it never exposes candidate state or blocks simulation on hashing/storage.
Restore prepares the complete aggregate and publishes at
`CommitDeferredLifecycleChanges`; no standalone or partial Character state becomes
visible.

System ordering is declared by the scene runtime and validated before execution.
The data bus is not used to establish per-tick system order.

## Variable Update

Variable update is used for:

- editor camera and presentation behavior
- interpolation of committed animation poses and isolated editor preview playback
- Runtime UI input, binding, ADR-077 domain-driven animation and layout under
  ADR-073
- job progress and query refresh
- audio presentation updates where supported
- streaming and resource coordination

Variable update must not introduce simulation behavior that changes with frame
rate. Animation presentation/preview cannot emit gameplay events, root motion,
or physics/controller writes; its authoritative order is defined by ADR-061.

## Render Snapshot

The scene runtime builds an immutable or frame-owned render snapshot after
simulation updates. The renderer does not traverse mutable editor documents or
partially updated ECS storage.

Interpolation reads previous and current simulation transforms using `alpha`.
It does not modify authoritative simulation state.

## Runtime Modes

```cpp
enum class RuntimeMode {
    Headless,
    Game,
    EditorIdle,
    EditorPlay,
    EditorPaused
};
```

Capabilities vary by mode, but lifecycle rules remain the same.

### Editor Idle

The editor document is authoritative. A preview runtime may render a converted
snapshot, but runtime mutation does not silently modify the document.

### Editor Play

Entering play mode:

1. validate the editor document
2. build a runtime scene definition
3. create a new play-session scene runtime
4. load required resources
5. activate simulation at tick zero

The play runtime owns its ECS and physics state. Stopping play destroys this
state and returns to the editor document. Applying runtime changes back to the
document requires an explicit editor command.

### Pause And Step

Pause stops simulation ticks but keeps event processing, GUI, rendering, and
required service updates alive. Single-step advances exactly one fixed tick and
returns to paused state. It uses the ordinary unchanged fixed quantum and complete
subsystem order; it does not consume accumulated wall time. Authoritative animation
therefore holds during pause and advances once during a successful step. Isolated
editor preview may advance only under its separate preview controls.

Under [ADR-118](../../adr/118-animation-character-and-gameplay-authority-during-cinematics.md),
whole-game pause also means no authoritative cinematic pose advance, root-motion
request, Character movement, Physics step or gameplay-action backlog. Permitted
unscaled/external cinematic work is presentation/service-only. A cutscene that must
move collision-aware actors leaves fixed simulation running and acquires scoped
Gameplay/Animation/Character control claims; it does not use pause as selective input
suppression. Resume cannot apply elapsed presentation motion as a catch-up tick.

Runtime UI continues its ordinary VariableUpdate/input/layout/render path while
gameplay is paused. Menus/navigation/accessibility feedback use declared unscaled
presentation time by default; `FollowGameplay` UI animation freezes and `Manual`
time advances only by owner command. A UI action requests pause through the
application pause capability and never owns scheduler tokens directly. After
single-step, UI observes the newly committed tick in one normal VariableUpdate.

### Runtime Console And Development Overlays

The runtime console and development overlays are lifecycle participants when the
selected product profile enables them. They may accept input and query logs,
metrics, profiler state, and runtime inspection services while the game is
running or paused. Mutable commands are queued to the owning runtime safe point;
the console UI never mutates scene, renderer, physics, asset, or networking state
directly.

See [Runtime Debug Console And Development Overlays](./debug-console-and-overlays.md).

## Scene Transitions

Scene load, reload, unload, and replacement use the state model in
[Scene Runtime](./scene-runtime.md). Worker preparation may occur
asynchronously; final activation happens at a runtime safe point.

[ADR-087](../../adr/087-scene-to-physics-ownership-and-conversion.md) requires
Physics to join the one aggregate scene candidate through an explicitly injected
activation participant. Core ECS storage, resource leases, the detached Physics
world and its binding table publish together at
`CommitDeferredLifecycleChanges` only after all fallible conversion/native startup
work succeeds. A failed replacement leaves the prior active scene and Physics world
unchanged; the new world's first fixed tick occurs after publication.

A stale completed load cannot replace a newer request. Transition requests
carry runtime session and generation identities.

## Suspension And Focus

Window focus loss does not automatically pause all products. The active host
policy decides whether to:

- continue normally
- throttle rendering
- pause game simulation
- suppress gameplay input

Minimized or occluded windows avoid unnecessary presentation work while still
servicing required jobs, transports, and shutdown requests.

Explicit host suspension is distinct from gameplay pause. While suspended, the
host runs only `BeginFrame`, `PollPlatformEvents`,
`ApplyQueuedOwnerThreadCommands`, and `EndFrame`. Fixed/variable simulation,
render extraction, execution, GUI rendering, and presentation are skipped.
Resume resets the clock baseline so suspended wall time never becomes catch-up
work. ADR-062 keeps the bounded audio lifecycle/control subset serviceable on
the remaining owner-thread boundaries; the callback follows only committed audio
suspend policy.

Runtime UI performs no VariableUpdate, input dispatch, extraction or render work
while suspended; interactive capture is released and resume revalidates viewport
attachments with a zero-delta baseline. Suspended wall time is not UI catch-up.

## Fatal Failure

A fatal runtime failure:

1. stops accepting new mutation requests
2. records the typed error and emergency diagnostics
3. attempts bounded cleanup that is known to be safe
4. presents or returns the failure through the active host
5. terminates rather than continuing with violated invariants

Recoverable subsystem failures, such as an asset import error, do not enter this
path.

## Shutdown

Canonical shutdown:

1. transition host to `Stopping`
2. stop external request acceptance and close modal workflows
3. unpublish ADR-102 gameplay network role/session views, invalidate grants and
   stop local-player/input producers
4. stop play simulation and unload active scenes
5. cancel and join host-scoped jobs
6. stop MCP, networking, and other transports
7. drain owner-thread continuations
8. close Runtime UI command/input admission, retire every scope/attachment, join
   UI work, and release UI render/resource leases
9. release GUI and editor sessions
10. destroy scene/project/application services after closing their renderer and
   audio producer ports
11. wait for renderer idle as required and destroy GPU resources
12. stop remaining audio producers, quiesce/detach the callback, reconcile terminal outcomes,
   and release device-owned resources under ADR-062
13. destroy remaining asset, data-bus, job, and platform services
14. flush observability and write clean-shutdown marker
15. transition to `Stopped`, unless a subsystem's documented fatal-retention path
    requires immediate process termination without normal reclamation

Shutdown may be requested more than once but executes its transitions once.

## Headless Runtime

Headless commands use the same initialization graph with omitted capabilities.
A command that requires rendering explicitly requests the null or a real
renderer. Headless execution does not create a hidden window merely to satisfy
an accidental dependency.

Runtime UI composition is independently `Omitted`, `ModelOnly`, or `Rendered`.
Model-only headless tools may validate documents, bindings, layout and Null
extraction without a viewport/input/GPU. A request requiring visible or interactive
UI fails capability preflight instead of creating a hidden window or editor GUI.

## Testing

Required tests cover:

- startup success and partial-initialization failure unwind
- fixed-step accumulator and catch-up bounds
- fixed-state determinism under 30, 60, and 144 Hz variable frame cadences
- cinematic immutable-batch cutoff/order, failed-tick discard and same-history replay
  across zero/one/multiple fixed ticks per presentation frame
- interpolation alpha at equivalent accumulator positions
- presentation gating after extraction, execution, and GUI failures
- dropped-time and clamp counter observability
- allocation-free successful steady-state scheduler dispatch
- mid-frame cancellation and exception containment
- explicit suspend pump phases and resume baseline reset
- pause and one-tick step
- editor play isolation and stop restoration
- stale asynchronous scene transition rejection
- minimized and focus-loss policies
- repeated shutdown requests
- shutdown with running jobs and renderer resources
- headless lifecycle without GUI or graphics
- Runtime UI phase order across zero/multiple fixed ticks, gameplay pause/step,
  suspension, failed presentation, scene/viewport teardown and shutdown
- standalone/client/listen/dedicated network plan startup, admission, aggregate
  listen-server travel, disconnect/reconnect generation replacement and shutdown
- rejection of incompatible role capabilities and of authority inferred from
  process flags, locality, local players, listeners, headless state or build kind

## Related Documents

- [Scene Runtime](./scene-runtime.md)
- [Rendering Architecture](./rendering-architecture.md)
- [Input Architecture](./input-architecture.md)
- [Physics Architecture](./physics-architecture.md)
- [Audio Architecture](./audio-architecture.md)
- [Networking Architecture](./networking-architecture.md)
- [ADR-117: Playback Ownership, Frame Order and Determinism](../../adr/117-playback-ownership-frame-order-and-determinism.md)
- [ADR-118: Animation, Character and Gameplay Authority During Cinematics](../../adr/118-animation-character-and-gameplay-authority-during-cinematics.md)
- [ADR-102: Runtime Network Modes and Authority Exposure](../../adr/102-runtime-network-modes-and-authority-exposure.md)
- [Asset Pipeline](./asset-pipeline.md)
- [Runtime Debug Console And Development Overlays](./debug-console-and-overlays.md)
- [Concurrency And Job System](../foundation/concurrency-and-jobs.md)
