# Character Controller Architecture

## Purpose

This document defines Horo Engine's character controller and surface interaction
runtime. It specifies the kinematic character controller, slope and step
handling, moving platforms, physics surface events, and the contract between
animation root motion, gameplay movement, and the physics world.

The goal is to give gameplay systems a stable, deterministic way to move
characters in a physics world without exposing the full complexity of rigid-body
dynamics to every gameplay module.

[ADR-061](../../adr/061-animation-ownership-update-order-and-clock.md) owns the
fixed-tick animation/root-motion ordering and pose handoff. This document owns
movement admission, collision resolution, and the resulting transform.

## Scope

Covered:

- kinematic character controller
- capsule controller geometry and queries
- ground detection and slope handling
- step climbing and step-down behavior
- moving platform attachment and velocity transfer
- surface material detection and events
- integration with animation root motion
- integration with scene runtime and physics world

Not covered:

- AI pathfinding and navigation mesh (see future navigation document)
- vehicle physics
- ragdoll physics (see [Physics Architecture](./physics-architecture.md))
- full gameplay locomotion state machines (gameplay module concern)

## Core Decisions

- The character controller is kinematic. It queries physics and moves by
  explicit position updates, not by simulating a dynamic body.
- The controller owns a single capsule collider used for collision queries.
- Gameplay sets desired velocity; the controller resolves collisions and
  reports the final displacement.
- Slope and step handling are deterministic and configurable per controller.
- Moving platforms transfer velocity and optionally angular velocity to
  standing characters.
- Physics material assets own contact coefficients; downstream application bindings select gameplay, footstep Audio and VFX semantics.
- Root motion from animation may feed into the controller as a delta request,
  but the controller decides the final transform.
- [ADR-089](../../adr/089-character-controller-ownership-implementation-and-update-order.md)
  makes Horo's bounded query/sweep pipeline the semantic implementation. Private
  Jolt narrowphase queries supply collision evidence; no native Character class or
  replaceable backend ABI owns public behavior.
- Each active scene generation owns one `CharacterWorld` paired with its exact
  Physics world/filter/origin generations.
- Character updates exactly once per attempted fixed tick and publishes the
  collision-root transform only with successful tick commit.
- [ADR-090](../../adr/090-character-dynamic-body-visibility-push-and-proxy-policy.md)
  defines explicit `Disabled`, `ObstacleOnly`, `OneWayPush` and
  `BidirectionalProxy` dynamic interaction modes. Character remains root authority
  in every mode; unsupported capability never silently changes the effective mode.
- [ADR-118](../../adr/118-animation-character-and-gameplay-authority-during-cinematics.md)
  keeps Character as collision-root authority during cinematics. Each claimed channel
  is GameplayControlled or CinematicControlled; whole-game pause performs no move.

## Implementation And Ownership

The first `Horo::Character` implementation lives behind `HoroEngine::Physics`.
`CharacterWorld` owns controller slots/state, tick command admission, support
attachments, fixed-capacity query/contact/impulse/event scratch and immutable debug
snapshots for one scene generation. The host publishes it with the exact Physics
world in ADR-087's aggregate scene transaction.

`CharacterWorldSettings` is the single immutable per-scene authority for retained
controller, command, contact, event, query, impulse, diagnostic and debug capacity;
fixed-tick work and scratch ceilings; and optional checkpoint/resimulation budgets.
The host captures its versioned canonical content identity before Character-world
activation. Defaults are deterministic and platform-neutral; renderer or device
selection cannot enlarge them. A live world cannot mutate individual settings:
adopting another snapshot requires complete transactional world replacement.
Capacity or work exhaustion rejects/truncates only through the owning later runtime
contract, preserves the last valid controller state, and reports against these named
limits through structured counters and diagnostics. Per-controller capsule,
locomotion, slope, step and gravity policy remains in
`CharacterControllerDescriptor`.

`CharacterDiagnosticRecord` is the CHR-007.1 backend-neutral projection for
rejected commands, degraded lifecycle, query/solver failures, and exhausted
Character-owned limits. Every record owns a valid controller handle, matching
scene generation, non-zero simulation tick, stable category/code/severity, and a
bounded message. Optional metadata is a closed, ordered, fixed-capacity vocabulary
for the paired Physics world, operation sequence, requested count, and capacity.
Unknown codes, malformed identity/tick evidence, unordered metadata, and oversized
messages fail before publication. When Character wraps a declared Physics error,
the record retains that exact typed Physics code separately; consumers never parse
messages or flatten cause chains to recover the source failure.

Owners maintain one `CharacterDiagnosticRateLimiter` per
controller/category/code stream. The limiter is allocation-free and owner-thread
only, admits a finite number of records per tick, enforces a minimum tick interval,
counts suppressed and out-of-order attempts with saturating counters, and performs
no logging or callback execution. Observability, editor, and debug consumers read
immutable retained records after Character publication; they do not own rate policy
or simulation mutation. `maximumDiagnosticRecords` remains the scene-wide retained
record bound; a retaining layer that reaches it must preserve Character state and
report aggregate drop telemetry rather than allocating or evicting
nondeterministically.

`CharacterMetricCapture` is an optional caller-owned output for one synchronous
fixed-tick attempt. When present, Character counts actual sweep calls, movement
iterations and resolved contacts, records active controller occupancy and
cumulative bounded-storage overflow pressure, and times reached tick phases on
the owner thread. A failed attempt is marked without a publication revision;
invalid pre-admission input leaves an empty capture. With no capture, the tick
does no clock reads or metric publication. Host composition prebinds closed
`subsystem=character` Telemetry series and publishes the detached snapshot only
after the tick returns, with exact world, scene and host revision validation.
The binding closes before the world retires; presentation consumers never borrow
mutable controller records or query adapters.

The public lifecycle surfaces are `Horo/Physics/CharacterWorld.h` and the
`HoroPhysicsSceneIntegration` adapter owning the
`Horo/Physics/PhysicsSceneActivation.h` participant. The adapter is the explicit
composition boundary that may depend on both Physics and RuntimeScene; neither
core target reverses the architecture dependency direction. The Physics
runtime issues historically monotonic world identities, so recreating a participant
cannot reuse one. The application-owned activation authority supplies a coherent
collision-filter and local-origin generation snapshot for each candidate. Physics
revalidates that evidence immediately before publication. Character issues its own
never-reused process-local world identity while preparing the bounded slot table.
`RuntimeSceneService` owns one aggregate of the detached Scene and fully finalized
participant candidates. After all fallible preparation and evidence validation
succeeds, one no-fail aggregate ownership switch publishes it; failure retires only
the candidate and leaves the old aggregate visible. `Shutdown` is idempotent, drains
every owned controller record, and runs before the paired Physics world is retired.
Slot reuse advances a non-wrapping generation; exhausted slots are retired instead
of allowing an older handle to alias a replacement. Active controller creation and
destruction remain rejected until their structural safe-point command payloads are
defined; CHR-001.4 supplies the separate bounded movement-command pipeline.

The Horo algorithm performs bounded overlap recovery, support classification,
platform carry, capsule sweep/slide, guarded step-up/forward/down, vertical motion,
ground snap and contact canonicalization. It borrows one read-only world/tick-
affine `CharacterPhysicsQueryContext`; the private adapter maps Horo queries and
staged impulses to Jolt. It never mutates bodies from a native collector.

`JPH::Character` and `JPH::CharacterVirtual` are not state or behavior authorities.
There is no public backend selector. Replacing the private Physics query adapter
must pass the same Horo golden, determinism and performance qualification.

## Canonical State, Checkpoint And Restore

[ADR-092](../../adr/092-character-controller-determinism-and-state-composition.md)
defines `CanonicalCharacterStateV1` and `CharacterStateCodecV1` as the only state
that may resume Character simulation. Save, replay and future networking consume
that shared typed model/codec; transport/envelope policy cannot create another
authoritative field set.

The canonical world header binds the committed tick, scene/structural revision,
paired Physics checkpoint, determinism fingerprint, exact world-origin state,
collision/profile revisions and command protocol. Ordered controller records contain
only resume-required semantic state:

- stable authored controller binding, descriptor/profile revisions and state
  sequence;
- global collision root, up/heading, achieved and gravity/free-fall velocity;
- stance/geometry transition continuation and fixed-point remainders;
- grounding/support attachment, resolved surface and platform carry evidence;
- command/root-motion/teleport/stance/fact watermarks;
- pending transfer velocity and ADR-090 dynamic reaction;
- any versioned algorithm remainder or named Character-owned random stream.

Native handles/proxies/manifolds/query caches, scratch, candidate state, immutable
descriptor duplicates, published output payloads, presentation/pose/debug state and
foreign Gameplay/Animation/Network history are excluded. Derived private state is
rebuilt against a detached Physics candidate.

SHA-256 over domain-separated canonical bytes is the exact determinism authority.
Field-specific numeric tolerances are diagnostic only and cannot accept restore or
make a failed exact comparison pass. Capture takes one aggregate committed
Scene/Physics/Character cut. Restore validates and publishes that complete candidate
atomically; standalone or partial Character restore is forbidden.

Bounded full/delta history may retain canonical bytes and hashes. The resimulation
coordinator separately owns complete ordered producer command histories and replays
the ordinary fixed-tick pipeline. History exhaustion or missing input ends the
rewind horizon explicitly.

[ADR-100](../../adr/100-prediction-capability-tiers-and-determinism-policy.md)
uses this contract only for an explicitly admitted `RollbackResimulation` provider
closure. `NonPredicted` and `LocalPrediction` do not construct Character checkpoint
history merely because a client is autonomous. Network rollback must pair Character
with the exact Physics/world checkpoint and all other participating providers; it
cannot restore a standalone Character blob or a network-specific field subset.

## Character Controller Component

`Horo::Character::CharacterControllerDescriptor` is the exact inert live creation
contract. It binds typed capsule geometry, root/up/gravity, canonical collision
profile/query-channel filtering, fallback material and a fixed contact budget to one
scene, Character-world and Physics-world generation. Pure validation proves
representation and owner-generation consistency without allocating a controller or
proving registry liveness; the CHR-001.3 world manager owns slot admission and stale
generation checks.

The component attaches to a scene object. While active, `CharacterWorld` owns the
authoritative collision-root position, capsule up basis and heading. Scene Transform
is the committed projection; arbitrary systems do not write it. Gameplay owns
desired heading, Animation owns root-motion rotation/visual pose and Physics owns
platform motion evidence under ADR-089.

[ADR-161](../../adr/161-xr-interaction-runtime-ui-locomotion-and-accessibility-ownership.md)
applies this authority to XR. Gameplay converts routed XR actions and tracking-space
evidence into tick-assigned movement, turn or teleport intents. Character validates
collision/clearance and owns the committed root; Camera motion, room-scale head motion,
recenter and raw XR poses cannot write it. An optional body-follow policy produces a
future Character intent rather than a render-frame Transform update.

## Update Order

```text
Attempted Fixed Tick
  Gameplay/AI/Nav stage movement, facing, animation parameters and platform targets
  Animation evaluates and stages the exact tick's root-motion request
  Physics freezes the Character query/platform-motion snapshot
  Character applies platform carry and resolves intent/root motion with Horo sweeps
  Physics applies staged commands and steps once
  Physics publishes rigid-body/contact/platform results
  Character finalizes support and the authoritative collision-root transform
  Post-Physics animation pose override/finalization runs
  Tick commit publishes Physics, Character, transforms, poses and events atomically
```

The controller moves once after animation root motion/query evidence is staged and
before each Physics fixed step. Post-Physics Character work finalizes support and
publication only; it never performs a hidden second move. Catch-up repeats the
complete sequence and never reuses or accumulates a render-frame root delta.

## Collision And Visual Orientation

`CharacterWorld` owns a finite unit capsule up basis and collision heading twist.
Gameplay owns optional absolute desired heading. Animation owns local root-motion
rotation and visual pose. Physics owns platform angular evidence. Presentation owns
visual lean/aim overlays.

Character applies optional platform twist about its up basis, then an explicit
Gameplay desired heading, then admitted root-motion twist. Root `Override` ignores
Gameplay desired heading for that tick and applies root twist to the carried
heading. Root translation uses the pre-root heading. Platform tilt and root/visual
pitch or roll never rotate the capsule; changing the up basis is a typed safe-point
command with clearance validation.

## Cinematic Control Arbitration

Before a sequence starts, the application requests generation-scoped Character
claims for translation, heading, stance/jump or a separately admitted teleport
capability. Required conflicts fail aggregate player activation; optional tracks
disable visibly. The resulting per-tick authority snapshot selects exactly one source
for each claimed channel:

- `GameplayControlled` consumes the ordinary Gameplay/AI command and configured
  Animation root-motion policy; or
- `CinematicControlled` consumes the ADR-117-selected cinematic command and returns
  `SuppressedByCinematic` for ordinary commands targeting the same channel.

Suppressed movement, heading, jump, stance and actions are not queued for replay after
the claim releases. Unclaimed channels remain accepted. The baseline does not sum
gameplay and cinematic displacement; a future cooperative mode must be an explicit
Character-owned reducer with its own collision/timing contract.

A cinematic command carries exact scene/controller/player/tick/generation/order
identity and no caller-selected delta or native body handle. Character performs the
same platform carry, root-motion composition, bounded Horo sweeps, collision/dynamic
interaction and Physics command staging used by gameplay. Cinematic Runtime never
writes Scene Transform or bypasses the controller with a visual-root value.

Host gameplay pause produces no Character tick or movement. Collision-aware cutscene
movement therefore keeps simulation running and suppresses selected Gameplay/AI
control through owner leases. Presentation pose motion while paused cannot update the
capsule, accumulate root motion or become a resume-time catch-up displacement.

## Movement Resolution

### Input

`Horo::Character::CharacterMovementRequest` owns an exact tick and producer
sequence, generation-checked controller identity, explicit optional desired velocity
and heading, jump intent and typed stance intent. It contains no caller delta time.

`CharacterWorld::QueueMovementCommand` copies requests into storage reserved by the
immutable world settings. Producers use non-blocking admission: contention returns
`RejectedBusy`, exhaustion returns `RejectedFull`, and neither path mutates controller
state. Exact duplicate controller/tick/sequence positions, stale producer sequences,
and commands for a closed tick fail during admission with
`character.command.order_invalid`. Queued sequences for one controller must increase
across future ticks, independent of producer arrival order. Before movement, the owner
thread freezes the eligible frame and orders it by tick, stable controller handle and
sequence. A greater sequence for the same controller and tick replaces the earlier
intent; only the final replacement executes. A controller with no command performs
no movement for that tick, and an earlier intent is never replayed. The world itself
must advance through consecutive ticks, so a skipped attempted tick is an explicit
order error rather than an inferred catch-up policy.

The controller consumes `FixedStepContext::fixedDelta`; callers cannot provide a
different delta. Desired translation/facing use explicit presence and root-motion
composition modes, not zero-vector/epsilon inference.

### Output

`Horo::Character::CharacterMovementResult` owns fixed-capacity ordered contacts,
typed collision flags, achieved motion and optional grounded-surface evidence for
one committed tick. Its active contact prefix is bounded by the descriptor and the
absolute inline ceiling; truncation is explicit and no result path allocates a
contact vector. Later world publication may expose immutable borrowed generations,
but it cannot change this payload's validation semantics.

### Collision Pass

The controller resolves movement in passes:

1. **Ground detection** — cast a sphere/capsule probe downward to find standing
   surface.
2. **Step up** — if blocked by a vertical obstacle within `stepOffset`, attempt
   to step onto it.
3. **Horizontal movement** — sweep the capsule along desired horizontal velocity,
   slide against obstacles.
4. **Vertical movement** — apply gravity and jump velocity, sweep vertically.
5. **Step down** — after horizontal movement, snap to ground if within step
   offset.
6. **Surface material query** — read surface material from touched colliders.

All sweeps use the physics query API, not direct transform mutation.

## Ground Detection

Ground detection determines whether the character is standing on a surface.

Rules:

- a downward sweep within `skinWidth + smallEpsilon` must hit a surface
- the hit normal's angle from world up must be less than or equal to
  `slopeLimitDegrees`
- the hit collider must be on the ground collision layer
- dynamic bodies do not count as ground unless configured

If no valid ground is found, the character is airborne.

## Slope Handling

Slopes within `slopeLimitDegrees` are walkable. Slopes above the limit block
movement unless the character is sliding down.

The runtime descriptor owns `preserveHorizontalSpeedOnSlopes` (default true)
and typed `CharacterSteepSlopePolicy::{Stop, Slide}` (default Stop). Walkable
blocking normals lift remaining displacement along the descriptor's `up` axis,
preserving its entire horizontal vector rather than normalizing an orthogonal
projection. Golden ramp qualification allows 0.001 m/s error in each horizontal
velocity component, including the exact slope-limit boundary. The normal cosine
classification tolerance is 0.00001. Normals effectively perpendicular to `up`
never enter the division used by ramp lifting.

Non-walkable upward-facing normals remove horizontal motion into the steep face
before resolving any downward component. This cannot create ascent from horizontal
intent; contour and downhill travel remain available. Conflicting simultaneous
planes fail closed when a later projection violates an earlier blocking plane.
No speed restoration scales a wall-clipped vector back through an obstacle.

Stop adds no automatic surface drift. Slide uses the committed Character-owned
`gravityVelocityMetersPerSecond` from the locomotion snapshot, distinct from
caller-owned desired velocity and displacement-derived achieved velocity. It
projects prior gravity velocity and descriptor gravity onto the steep face,
computes `nextVelocity = priorVelocity + tangentGravity * fixedDelta`, and sweeps
`0.5 * (priorVelocity + nextVelocity) * fixedDelta`. Repeated ticks retain actual
acceleration; unobstructed planar reference cases agree across equal-duration
fixed-tick partitions within 0.00001 m and 0.00001 m/s. This continuation is
observable, finite-validated, staged with the movement result and committed only
with the whole tick, following ADR-092. It introduces no separate slide accumulator.

Slide starts only when the nearest downward blocking evidence is steep and within
skin width. Upward tangent gravity cannot produce a climb. Any blocking slide sweep resets gravity continuation, so obstructed displacement
cannot accumulate pressure independently of the contact-retention capacity. Motion
still slides against the canonical blocking normals. Continuation also resets on
walkable support, Stop, and teleport. Failed ticks preserve the prior snapshot and
continuation. Leaving steep support carries that same velocity into free flight;
changing capsule geometry alone cannot cancel airborne velocity.

Slide displacement uses the same collision sweeps and remaining movement-iteration
budget, including for sub-minimum ordinary movement distances. Zero ordinary
movement consumes no movement iteration. A required slide with no remaining
movement iterations rejects the entire candidate tick with `CapacityExceeded`,
which preserves the committed state. The shared query budget includes both support
probes. A steep surface does not publish grounded or
platform support, and the reducer never snaps through the nearest steep face to a
deeper walkable surface. Failed queries or shutdown discard all candidate motion
before publication.

## Jump And Airborne Resolution (CHR-002.5)

`jumpSpeedMetersPerSecond` is finite and nonnegative; zero disables jumping. A
jump command consumes only the previous committed grounded state. Its accepted
impulse replaces the Character continuation with `up * jumpSpeedMetersPerSecond`;
subsequent airborne commands cannot reapply it. Producers submit discrete jump
intent; a command on a later grounded tick is a new jump. Spawn first establishes
support through the ordinary bounded ground query. Desired gameplay velocity
remains separate from Character-owned gravity velocity.

Ordinary movement and support classification run first. If no walkable or steep
support remains, the remaining shared movement-iteration budget resolves
`nextVelocity = priorVelocity + gravity * fixedDelta` and
`0.5 * (priorVelocity + nextVelocity) * fixedDelta`. Gravity travel is resolved even
below the ordinary intent threshold. Upward continuation suppresses downward snap;
airborne landing probes use skin-width reach rather than step-height reach. The
initial spawn support probe retains ordinary ground acquisition reach. Ceiling
collision removes only continuation into each canonical blocking normal, retaining
tangent velocity; conflicting corner constraints fail closed. Landing resets the
continuation. Steep slide retains its established bounded obstructed-slide policy
and does not integrate a second gravity phase during the same tick.

The committed result owns `jumpApplied` and the closed `CharacterGroundTransition`
value (`None`, `LeftGround`, `Landed`). The world derives exactly one transition
from previous and candidate grounded states. Repeated support contacts produce
`None`; failed query, capacity exhaustion and shutdown publish no candidate fact.
Consumers identify facts by controller/tick/sequence and consume each committed
result once. No callback or render frame can independently emit a landing.
Consecutive fixed ticks consume their exact admitted command frames; a tick with
no command continues to preserve its prior controller snapshot as specified above.

Migration: controller descriptor and movement result grow appended fields owned by
`HoroEngine::Physics` in the existing header registry. In-process Physics consumers
must rebuild; no new header or dependency is published. Custom movement-result
providers may retain `None`; CharacterWorld owns transition derivation. Native
Physics/Scene/Gameplay consumers and the public-header consumer cover the appended
fields. Actual capsule changes now preserve airborne continuation; teleport remains
the explicit reset boundary. There is no additional velocity accumulator or state
codec authority.

## Step Handling

Step climbing:

- detect obstacle in movement direction
- test whether the top of the obstacle is within `stepOffset`
- move the capsule up by step height
- sweep horizontally
- step down onto the new surface

Step down:

- after horizontal movement, sweep down by `stepOffset`
- if a valid ground is found, snap to it
- preserve momentum if the drop is significant

Step behavior is configurable:

- `maxStepHeight`
- `minStepDepth`
- `stepSpeed` — how fast the character visually ascends

## Moving Platforms

When the character is grounded on a moving platform, the controller must track
the platform's transform and velocity.

```cpp
struct MovingPlatformAttachment {
    PhysicsBodyId platformBody;
    Transform localTransformOnPlatform;
    Vec3 platformLinearVelocity;
    Vec3 platformAngularVelocity;
};
```

Rules:

- attachment is established on ground contact with a platform body
- local offset is stored in platform space
- on the next attempted fixed tick, stage-3 evidence supplies the admitted
  kinematic target or committed dynamic point velocity
- platform carry is swept before the character's own movement
- full platform rotation transports the attachment point; only twist about the
  Character-owned up basis affects heading when explicitly enabled
- the final post-Physics platform result updates next-tick attachment evidence and
  never causes a hidden second move in the current tick
- detachment happens when the character leaves the platform, becomes airborne, or
  is teleported

[ADR-108](../../adr/108-dynamic-overlay-carving-and-tile-rebuild-policy.md)
keeps moving-platform motion and attachment under Character/Physics authority.
Navigation may expose a stable timed or conditional link for a platform transfer,
but the 1.0 baseline does not continuously move or rebuild grounded NavMesh with
the platform. When transfer conditions cannot be proven, the link remains
unavailable and the dynamic-change outcome is typed rather than inferred from
render or Physics transforms.

## Surface Materials

[ADR-181](../../adr/181-physics-material-surface-and-cross-system-identity.md)
separates physical coefficients from optional gameplay surface identity. A
collider/shape material slot binds a physical material asset and may separately
bind a project `SurfaceMaterialId`. Mesh and compound subshapes preserve their
authored slot mapping. The surface catalog carries no friction, sound or VFX
asset; application/Gameplay binding assets select optional consumer cues from
the committed semantic surface. A physical asset can serve several surfaces,
and several physical assets can share a surface.

### Implemented physical evidence contract

`CharacterSweepHit` consumes the existing Physics-owned `PhysicsQueryMaterial`
asset UUID, exact asset generation and authored material slot. It also copies the
optional authored `PhysicsShapeSubresourceId`; native child indexes never escape
Physics. Selected ground evidence and retained contacts preserve those values,
body/shape world and slot generations, and `CharacterMaterialSource`:

- `Query` means the Physics adapter supplied the physical binding.
- `DescriptorFallback` means the binding was absent and Character used the
  admitted descriptor's exact physical fallback. It does not claim that the
  collider owns that binding or that a semantic surface was resolved.

Invalid present material/child identities fail the whole tick before publication;
absence alone does not. Ground selection includes authored child identity in its
canonical ordering. Contact reduction distinguishes children of the same shape.
No support clears all ground references, point and provenance. The selected
support point is retained independently of the bounded contact prefix.

`BuildCharacterGroundSurfaceFact` validates a copied committed locomotion snapshot
against its captured descriptor, then projects selected support into one bounded
owned fact. It preserves scene/controller/world, tick, command sequence, state
revision and transform publication revision. Airborne snapshots produce no fact.
The projection accesses no live world, material registry or consumer and can run
on copied snapshots after shutdown. It does not generate transition cadence or
Animation markers; those adapters consume the fact at their owning boundaries.
The existing bounded internal event storage retains the same enriched contact
identity and rejects malformed child/provenance evidence.

Missing or deleted physical bindings in a new valid Physics query use the explicit
fallback; existing copied facts retain their original UUID/generation/slot.
An old fact never resolves implicitly to the latest material revision. Reload
publishes a new complete world/mapping at the Physics safe point, and consumers
must fence old world generations. Missing/deleted downstream bindings, failed
optional consumers, consumer reload and shutdown suppress presentation without
feeding back into Character movement or rewriting historical facts.

The proposed semantic `SurfaceMaterialId`, catalog admission and collider semantic
projection described in ADR-181 do **not** ship in this contract. Physical evidence
must not be cast, renamed or inferred into a semantic ID. Once Physics provides the
versioned typed semantic projection, Character will copy it and its mapping
provenance and admit the catalog-valid default separately. Until that producer is
implemented, semantic-dependent consumers remain explicitly unavailable; there
is no coupled `SurfaceMaterial` containing coefficients or Audio/VFX assets.

## Locomotion Facts And Footstep Correlation

The controller publishes bounded physical facts based on committed contacts and
state changes. It does not own animation cadence and never emits `Footstep`.

Events:

| Event | Trigger |
|---|---|
| `Landed` | Transition from airborne to grounded. |
| `LeftGround` | Transition from grounded to airborne. |
| `HitWall` | Horizontal movement blocked by surface. |
| `HitCeiling` | Vertical movement blocked above. |
| `SurfaceChanged` | Committed semantic ground surface changed; physical asset revision changes alone are insufficient. |
| `SlideStart` / `SlideEnd` | Started/stopped sliding on steep slope. |

Events carry:

- Physics physical asset/generation/slot, body/shape and optional authored child references; future semantic identity remains separate
- explicit query or descriptor-fallback provenance
- contact point and normal
- impact velocity
- controller reference

Facts carry stable scene/controller/tick/result identity plus bounded material,
contact and achieved/impact velocity evidence where applicable. Repeated raw
contacts do not create another state-transition fact.

[ADR-091](../../adr/091-footstep-and-locomotion-event-ownership.md) makes committed
Animation marker occurrences the authoritative timing source for animation-driven
footsteps. After atomic tick commit, an application/Gameplay-owned locomotion
presentation adapter correlates the occurrence with the exact same-tick Character
snapshot and its resolved support material/contact. It then submits immutable,
deduplicated cue intents to Audio and VFX through their own admission contracts.

Missing markers produce no inferred Character footstep. Missing/stale same-tick
ground evidence suppresses presentation without a new Physics query. Missing,
failed or shutting-down Audio/VFX consumers cannot alter Animation, Character or
Gameplay simulation results. Applications may map `Landed` or other facts to
distinct presentation cues, but cannot relabel them as the marker occurrence.

## Root Motion Integration

Animation root motion may provide a movement delta.

```cpp
enum class RootMotionPriority {
    GameplayOnly,
    Suggest,     // root motion is used only when gameplay declares no translation
    Additive,    // root and gameplay translations are summed before collision
    Override     // root motion replaces gameplay translation for this fixed tick
};

struct RootMotionRequest {
    Vec3 deltaTranslation;
    Quat deltaRotation;
    bool applyToPosition;
    bool applyToOrientation;
    RootMotionPriority priority = RootMotionPriority::Suggest;
};
```

The controller treats root motion as one tick-addressed movement request and
resolves it through the same collision passes. Animation produces it directly from
the authoritative fixed-tick interval; presentation frames never accumulate root
motion. This ensures animation-driven movement respects walls, slopes, and steps.

The request identifies its scene, animation instance, simulation tick, and root-
motion generation. Controller resolution stages a consumption marker in the tick
transaction; only successful tick commit makes that marker durable. Aborting an
attempt discards its marker and request lease, so a retry of the same simulation
tick can consume the newly staged equivalent request. After a successful commit,
another request with that identity is a duplicate and is rejected. Presentation/
editor-preview, stale, duplicate-after-commit, or leaked aborted-attempt requests
are invalid. Reverse player traversal may submit the inverse directed delta when
animation and gameplay policy admit it; the controller still performs ordinary
forward collision resolution and does not reverse physics.

Gameplay selects the admitted translation/rotation mode before command closure.
`Suggest` uses an explicit gameplay-translation presence bit, not a velocity
epsilon. Platform carry applies first. Desired gameplay heading establishes the
pre-root basis, root translation is rotated by that basis, and admitted root twist
then composes under ADR-089. Root pitch/roll remains visual and never tilts the
capsule.

## Physics Material Query

During sweeps, the controller reads copied physical material evidence and the
optional semantic surface binding of the supporting collider. For grounded
support with no semantic binding, Character uses its explicit project-valid
default `SurfaceMaterialId`. This is independent of the implemented descriptor's
physical `PhysicsQueryMaterial` fallback. No valid support means no ground surface;
no live catalog lookup occurs during a sweep. A grounded committed result has a
valid semantic surface only after the ADR-181 typed projection and default are
implemented; current C++ query/results expose physical material evidence only.

## Collider Filtering

The controller selects one stable project `PhysicsQueryChannelId` and explicit
typed selectors under ADR-086. Collider profiles answer that channel with
`Ignore`, `Overlap` or `Block`; serialized/native layer masks are not controller
identity. Trigger inclusion is explicit. Trigger enter/exit events remain owned by
the gameplay volume/Physics event contract, not controller surface events.

### Implemented bounded collision selectors

`CharacterCollisionSelectors` intersects the descriptor's stable query channel with
at most one required layer, one required profile and one excluded body identity.
Every spawn recovery, teleport clearance, shape clearance, movement sweep and
floor/snap probe receives the same owned selectors. Character physical probes
exclude triggers and query `Overlap` responses before collection and recovery;
trigger evidence from a custom sweep adapter is also ineligible for contact and
ground reduction. Sweep hits copy typed layer/profile evidence when selected;
missing or malformed evidence fails closed, while valid mismatches and excluded
body identities cannot block or become ground. Ignored channel responses are
absent from the query inventory.

`CharacterMovementRequest::filterChange` replaces the complete selector value for
its addressed tick. The final replacement command selected at command closure
supplies the filter for that tick's shape and movement probes. Only successful
atomic tick publication persists it to the controller policy. Admission, future
commands and failed ticks cannot change the committed filter. An explicit empty
value clears selectors; absence retains them. Teleports use the last committed
filter. Changes require a spawned controller and obey command capacity, sequence,
world-generation and shutdown validation.

`CharacterPhysicsQueryAdapter` borrows one explicitly selected Physics world on
its owner thread. It maps probes to inline analytic capsule queries, using fixed
world-owned native collector storage and stack capsule geometry. It admits only
blocking, non-trigger fixtures before reduction, preserves typed query errors and
fails rather than silently accepting over-capacity evidence. Recovery chooses the
first positive penetration in Physics' canonical hit ordering and then re-probes
under Character's iteration/displacement budget.

The adapter currently uses the supported canonical immediate-query fixture
inventory. Authored scene/cooked collider query projection and immutable Physics
snapshot execution retain their documented unsupported status; this change does
not infer collision layers or query responses from a scene collision profile.
Application composition may use the adapter with Scene's transform command buffer
and the existing Gameplay Physics capability; no native API enters those contracts.

## Dynamic-Body Visibility And Push

Query visibility, Character-to-body push and body-to-Character reaction are
separate policies:

| Effective mode | Query blocker | Push authority | Body sees Character | Character reaction |
|---|---|---|---|---|
| `Disabled` | No dynamic body | None | No | None |
| `ObstacleOnly` | Selected dynamics | None | No | Support/carry only |
| `OneWayPush` | Selected dynamics | Canonical staged Horo impulse | No | Support/carry only |
| `BidirectionalProxy` | Selected dynamics | Physics contact with private proxy | Yes | Bounded next-tick evidence |

The query channel and proxy collision profile are independent stable ADR-086
identities. The baseline proxy collides only with explicitly admitted dynamic-body
profiles; static, kinematic, sensor and other Character proxies ignore it. It is
not ground, a trigger producer, a public body or a constraint endpoint.

`OneWayPush` reduces query hits into fixed-capacity, stable-ID-ordered Physics
impulse commands after collection. `BidirectionalProxy` instead stages one private
kinematic capsule target after Character resolves movement. Physics owns the pair's
solver impulse, so the same body receives no query-derived push in that mode.

Post-step proxy contacts reduce into one bounded `CharacterDynamicReaction`. The
current collision root is unchanged. The next attempted tick converts the committed
reaction through the interaction profile and resolves it with ordinary sweeps. A
stale controller/body/world/proxy generation, teleport, mode/profile change or
overflow invalidates or fails according to the typed policy.

Requested and effective modes are inspectable. A descriptor may name one exact
fallback; without it, missing query/impulse/proxy/reaction capability or an
unqualified determinism tier rejects scene activation. Disabled interaction is an
explicit mode, not a runtime error fallback.

## Crouching And Size Changes

The controller supports runtime size changes:

- crouch reduces capsule height
- a resize request checks for ceiling clearance
- if clearance is insufficient, crouch is rejected or the character is forced
  into crouch until space is available
- size changes do not alter `stepOffset`; if crouched geometry requires different
  step behavior, the gameplay system must use a separate controller descriptor or
  override the movement request accordingly

The instantaneous runtime contract uses `CharacterMovementRequest::shapeChange`
for an explicit radius/cylindrical-half-height replacement, or `Stand`/`Crouch`
against the immutable descriptor's standing capsule and optional `crouchedCapsule`.
The crouch profile keeps the standing radius and reduces cylindrical height.
Explicit geometry requires `Keep` stance; a conflicting named stance, missing
crouch profile, malformed dimensions or out-of-envelope capsule publishes `Invalid`.
This replaces the former accepted-but-unused stance intent; callers that request
crouch must provide the descriptor profile and current overlap adapter.

The collision root is the capsule center and stays fixed during instantaneous
resize. Every different candidate geometry, including shrink, uses one exact-tick
overlap probe without depenetration. An overlap publishes `Blocked` and keeps the
prior geometry/stance; a clear candidate publishes `Applied`. Query errors retain
their original typed cause and fail the attempted tick. Identity, authored descriptor,
step policy and heading are preserved. The effective capsule and stance live in the
owned locomotion snapshot; later movement and teleport queries use that capsule.
Shape results cannot be supplied by an adapter. All controller candidates preflight
before any geometry or transform is committed. Failed ticks publish no candidate.
Without a movement sweep, applied resize detaches support and requires grounding
revalidation; the sweep path resolves support against the new geometry immediately.
Shape clearance and movement/ground sweeps share the immutable per-tick query
budget; exhausting it aborts the attempt before another adapter call.
Queue capacity, replacement, generation and shutdown rules are the movement rules.
No transition is retried implicitly on a later tick.

Size changes are fixed-tick commands. A gradual transition advances once per
committed tick under a typed profile; presentation delta never changes collision
height. Every intermediate capsule requires clearance or the command reports its
declared hold/reject result.

## Validation And Safety

- The controller rejects invalid descriptors (negative radius, zero height).
- If the capsule is inside geometry on spawn, it attempts depenetration within
  a budget; otherwise it reports an error.
- Teleports bypass collision but flag the controller as needing ground
  re-evaluation. A teleport also detaches the character from any moving platform.
- Out-of-profile velocity/displacement returns a typed safety failure. An explicit
  profile may define deterministic saturation and reports the applied value; no
  silent clamp is permitted.

## Diagnostics

Debug visualization:

- capsule shape
- ground probe ray
- step probe arcs
- contact points and normals
- surface material labels
- moving platform attachment line

Runtime variables:

- `cc.drawDebug`
- `cc.showSurfaceEvents`
- `cc.logTunnelingWarnings`

## Testing Requirements

- Unit tests for capsule-sweep resolution against simple primitives.
- Slope limit tests.
- Step up/down tests on known geometry.
- Moving platform attachment and detachment tests.
- Locomotion fact transition tests and footstep marker/surface correlation tests.
- Root motion collision tests.
- Root motion duplicate/stale generation and reverse-policy tests.
- Determinism tests for fixed-step playback under different render/catch-up grouping.
- Platform carry point rotation, dynamic point-velocity prediction, optional heading
  twist and no post-Physics second move.
- Capsule up, Gameplay desired heading, root twist and visual pitch/roll ownership.
- GameplayControlled/CinematicControlled claims, claimed/unclaimed command outcomes,
  multi-player priority conflicts and no suppressed-command backlog.
- Whole-game pause versus running-simulation cinematic control transfer, including no
  presentation/root-motion catch-up and ordinary collision-aware movement.
- Four-mode dynamic visibility/push/reaction truth table, including filtered,
  unsupported-capability and explicit-fallback cases.
- One-way command canonicalization, proxy pair exclusivity, no double impulse,
  bounded next-tick reaction and no post-Physics root write.
- Proxy spawn/resize/teleport/reload/removal/origin-shift generation and rollback.
- Canonical state golden bytes/hash, field completeness/exclusion, mixed-checkpoint
  rejection, aggregate restore rollback and bounded full/delta resimulation history.
- Private Physics-query adapter parity against Horo controller golden fixtures.
- Performance tests for many concurrent controllers.

## Related Documents

- [Character Setup UI Reference](../../../mock-studio/designs.md#architecture-runtime-character-setup): capsule, movement parameters, camera, and input bindings panel.

- [Physics Architecture](./physics-architecture.md): collision queries,
  materials, rigid bodies, and fixed-step world.
- [Animation Architecture](./animation-architecture.md): root motion and
  animation-driven movement.
- [ADR-118: Animation, Character and Gameplay Authority During Cinematics](../../adr/118-animation-character-and-gameplay-authority-during-cinematics.md)
- [Audio Architecture](./audio-architecture.md): audio event routing and
  variation containers.
- [VFX And Particles Architecture](./vfx-and-particles-architecture.md): impact
  effects and decals.
- [Scene Runtime](./scene-runtime.md): transform ownership and update order.
