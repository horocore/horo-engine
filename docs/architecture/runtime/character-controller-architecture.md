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
- Surface materials drive friction, footstep audio, VFX, and gameplay events.
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

Options:

- `preventSlidingOnWalkableSlopes` — apply counter-force along slope normal
- `slideDownSteepSlopes` — apply gravity along the steep surface
- `preserveHorizontalSpeedOnSlopes` — adjust velocity to maintain requested
  ground-plane speed

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

Surface materials describe the physical and gameplay properties of a collider.

```cpp
struct SurfaceMaterial {
    SurfaceMaterialId id;
    float staticFriction;
    float dynamicFriction;
    float restitution;
    SurfaceType surfaceType;       // e.g., Metal, Wood, Concrete, Grass
    AssetId footstepSoundSet;      // variation container reference
    AssetId impactSoundSet;
    AssetId bulletImpactEffect;    // VFX asset reference
    AssetId footstepEffect;
};
```

Surface materials are assigned to colliders, not to meshes. A single mesh may
use multiple surface materials through material IDs or collision sub-shapes.

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
| `SurfaceChanged` | Ground surface material changed. |
| `SlideStart` / `SlideEnd` | Started/stopped sliding on steep slope. |

Events carry:

- surface material ID
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

During sweeps, the controller reads the physics material from hit colliders.

```cpp
std::optional<SurfaceMaterialId> QuerySurfaceMaterial(const PhysicsHit& hit);
```

If a collider has no surface material, the controller uses its default material.
`MovementResult.groundSurface` and `SurfaceContact.material` always contain a
valid material ID; the fallback from `QuerySurfaceMaterial` to the descriptor's
`defaultMaterial` is applied inside the controller before the result is returned.

## Collider Filtering

The controller selects one stable project `PhysicsQueryChannelId` and explicit
typed selectors under ADR-086. Collider profiles answer that channel with
`Ignore`, `Overlap` or `Block`; serialized/native layer masks are not controller
identity. Trigger inclusion is explicit. Trigger enter/exit events remain owned by
the gameplay volume/Physics event contract, not controller surface events.

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
