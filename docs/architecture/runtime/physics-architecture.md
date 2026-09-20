# Physics Architecture

## Purpose

This document defines physics-world ownership, fixed-step simulation, scene
synchronization, collision events, queries, determinism, threading, and
debugging for Horo Engine.

## Core Decisions

- Each active runtime scene owns or explicitly references one physics world.
- Physics advances only during fixed simulation ticks.
- Physics simulation operates in local cluster coordinates relative to the active floating origin.
- Origin shifts translate spatial proxies without altering velocities, momentum, contact caches, or waking sleeping bodies.
- Runtime components store typed body or shape handles, not owning pointers.
- Transform synchronization has one declared authority per body mode.
- Structural physics changes are deferred to safe points.
- Collision events are bounded tick results, not unrestricted process-bus
  traffic.
- Physics queries use explicit snapshots or world affinity.
- Backend-independent engine contracts do not expose solver implementation
  details.
- [ADR-084](../../adr/084-canonical-physics-solver-units-and-tolerances.md)
  selects pinned Jolt v5.6.0 as the one private initial solver; it does not create
  a runtime multi-solver ABI.
- [ADR-086](../../adr/086-collision-layer-profile-and-query-channel-policy.md)
  separates stable project collision layers, reusable profiles and query channels
  from generation-scoped packed/native filter representations.

## Canonical Solver, Units And Tolerance Profile

CanonicalV1 pins Jolt `v5.6.0` at commit
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`, float precision and a serial Horo-
owned job profile. Jolt types, handles, listeners, locks, allocators, job objects
and binary state stay private to `HoroEngine::Physics`; public/scene/gameplay
contracts expose only Horo values and generation-checked handles.

Physics uses SI units: meters, seconds, kilograms, radians, newtons and derived SI
units. It shares Scene Math's right-handed, `+Y`-up, `-Z`-forward, column-vector
convention without a hidden axis/unit conversion. Default gravity is
`(0, -9.81, 0) m/s²`. Foreign content normalizes at import; Physics never guesses
centimeters or per-body scale.

The ADR-026 local float cluster has a hard half-extent of `8192 m` and a qualified
high-fidelity dynamic-contact radius of `4096 m`. Canonical ordinary dynamic
objects are `0.1..10 m`, static objects `0.1..2000 m`, and speeds `0..500 m/s`.
Out-of-profile data is diagnosed/rejected, not silently clamped.

`PhysicsToleranceProfileId::CanonicalV1` pins collision tolerance `1e-4 m`,
manifold tolerance `1e-3 m`, speculative distance and penetration slop `0.02 m`,
maximum correction `0.2 m`, warm-contact distance `0.01 m`, sleep velocity
`0.03 m/s`, sleep time `0.5 s`, and solver iterations `10/2`. Exact identities,
events, layers and serialization never use these as a generic epsilon.

Initial Horo qualification covers macOS 14+ arm64/x86_64, Windows 11 x86_64 and
Ubuntu 24.04 x86_64, including headless compositions. Android, iOS, WebAssembly,
other architectures and consoles remain unqualified/unsupported for shipped Horo
Physics until their explicit matrix exists. Upstream build support alone is not a
Horo support claim.

Each world captures one immutable `PhysicsWorldSettings` before native allocation.
The snapshot owns its descriptor values, so editing project defaults affects only a
subsequently rebuilt world. CanonicalV1 admits a `60 Hz` fixed step, one substep,
`10/2` velocity/position iterations, sleeping after `0.5 s` below `0.03 m/s`, and
discrete motion quality by default. Other well-formed solver schedules are rejected
until a qualified profile names them; native defaults never silently widen this set.
The snapshot also owns gravity, all world/resource limits, the `8192 m` local hard
half-extent, the `4096 m` dynamic-contact radius, and the selected non-finite policy.

Settings identity is SHA-256 over schema 1's fixed ordered field words encoded as
little-endian 64-bit values; float bit patterns are widened and signed zero is
canonicalized. It never hashes structure padding or native state. Diagnostics and
reference fixtures retain this identity, but checkpoint compatibility additionally
requires the exact solver build, determinism tier, world/origin/scene generations,
and relevant structure identities. Equal settings identity alone is not a replay or
checkpoint compatibility receipt.

Jolt is distributed under MIT with pinned license/notice/SBOM inputs. Solver
upgrades are dedicated compatibility changes reviewing release/API/default drift,
compile definitions, derived-cache invalidation, platform qualification,
performance and rollback. Horo never persists Jolt binary state as project/save
authority.

## Ownership

The always-built `HoroEngine::PhysicsModel` target owns the portable authored Scene
schema. A rigid body has one stable body slot and explicit motion/mass policy;
colliders contribute stable slots, exact analytic or asset shape sources, local pose,
scale, collision profile and material mappings to an explicit body reference;
constraints name two exact body/world endpoints and a typed fixed or distance policy.
Each payload carries schema version and generation, and scene persistence round-trips
only Horo identities and canonical units. Runtime handles, native solver IDs, filter
indices, leases and cooked payloads are deliberately unsupported in this durable
surface.

RuntimeScene validates aggregate producer identity and reference invariants before
publishing an immutable definition: bodies never arise implicitly, every body has a
bounded non-empty collider set, static planes bind only static bodies, and constraints
cannot discover endpoints by hierarchy or name. This remains inert admission; native
planning, resource resolution, activation, rollback and retirement stay with the
solver-bearing Physics scene participant defined by ADR-087 and PHY-006.3.

```text
SceneRuntime
  +-- PhysicsWorld
        +-- Bodies
        +-- Shapes
        +-- Constraints
        +-- Broadphase
        +-- Solver
        +-- Tick Event Buffer
```

The physics world owns bodies, shapes, constraints, broadphase proxies, and
solver state. ECS components hold generation-checked handles.

`PhysicsRuntime` is an explicit process-composition owner. `Canonical` is used by
both graphical and headless hosts that require simulation; `Null` explicitly
reports omitted Physics and unsupported features. It is never an automatic fallback.
The initial lifecycle implementation advertises canonical world creation and the
owner-thread immediate-query capability. Rigid-body, constraint, snapshot-query and
origin-rebasing behavior is not advertised merely because a native empty system
exists. Simulation filters remain closed until validated collision-profile and body
admission are implemented; immediate-query fixtures are a narrow analytic admission
path and do not publish simulation bodies or replace scene activation.

`PrepareWorld` builds an isolated unpublished candidate from one captured settings
snapshot. It owns scratch storage, serial job dispatch, filters and native system
in dependency order. `Activate` binds a valid host-issued world generation without
native startup or successful-path allocation, rejects duplicate active identities,
and transitions a prepared world exactly once. The host remains responsible for
never reusing historical world generations and for publishing the complete scene
bundle atomically; preparing a candidate does not publish identity or events.

Runtime creation, preparation, activation and teardown are serialized on the Physics
owner thread. Shutdown closes new admission immediately. Internal world leases keep
the process-owned Jolt registration alive until all world resources have retired,
even if a host releases its runtime wrapper too early. Normal composition still
destroys scene worlds before the process runtime. Repeated world/runtime shutdown
is idempotent, and no process-global static initializer starts the native lifecycle.

World reset is an owner-thread transaction that closes admission, retires the native
world, invalidates its published identity, clears queued commands and every publication
domain, and rebuilds an unpublished candidate from the same immutable settings. A
successful reset returns to `PreparedSolver` or `PreparedNull` and requires a new
host-issued generation before work resumes; repeated reset while prepared is a no-op.
Scene unload is a distinct idempotent terminal path that retires the world before
component storage disappears. Fatal solver or joined-child failure retains the exact
typed error and prior coherent publication until reset, scene unload or shutdown.

Expected stage failures return `physics.initialization.failed` after reverse-order
rollback. Horo ownership-allocation failure is contained before returning a typed
capacity error. Native Jolt allocation cannot unwind through its no-exception frames:
the explicitly installed allocation hooks terminate on heap exhaustion rather than
returning a null pointer into native code. This is not a recoverable world-creation
OOM guarantee or a replacement for process-wide memory admission. Body quarantine
is rejected at native preparation until its safe-point retirement path is implemented.
Zero body capacity remains a valid descriptor for omitted compositions, but canonical
preparation rejects it with `OperationUnsupported` before allocation: the pinned
native broad phase requires storage for its root nodes even in an empty world.

Scene unload disables physics updates, drains scene-scoped queries/jobs, removes
bindings, and destroys the world before component storage disappears.

## Handles

```cpp
using BodyHandle = Handle<PhysicsBodyTag>;
using ShapeHandle = Handle<PhysicsShapeTag>;
using ConstraintHandle = Handle<PhysicsConstraintTag>;
```

Stale handles return typed errors or empty query results according to the API
contract. They never alias newly created physics objects.

Each world owns separate bounded body, shape and constraint registries. Candidate
preparation allocates their slot tables before publication; activation binds the
host-issued `PhysicsWorldId` without allocation. A successful removal increments
the slot generation before reuse. A slot already at the generation ceiling is
retired permanently, and the registry never wraps it to an earlier identity.
Full registries distinguish temporarily occupied capacity from terminal generation
exhaustion with stable typed diagnostics. Native solver IDs, pointers and leases
remain values of the target-private mapping and never become public handle fields.

Constraint requests use a generation-checked `ConstraintHandle` only after the
owner-thread structural safe point publishes them. Their endpoints are explicit:
the first is a live body-local frame and the second is either another distinct
body-local frame or a world frame bound to the receiving world's current origin
epoch. Anchor poses are relative to body poses, not native centers of mass, and no
display name, traversal order, native ID or solver-owned pointer participates in
identity. World anchors are transient runtime intent; origin rebasing must rebind
or reject queued intent rather than treating numeric coordinates as durable identity.

`PhysicsFixedConstraint` and `PhysicsDistanceConstraint` are the initial typed
parameter vocabulary. Validation proves representation and owner consistency only;
admission separately requires exact-revision `PhysicsCapability::Constraints`
evidence before body resolution, lease retention or native creation. Unsupported,
temporarily unavailable and stale evidence remain distinct typed failures. Hinge,
slider, cone-twist, six-DOF, motor, break and spring behavior must gain typed policy
and qualification rather than being approximated by fixed or distance constraints.

Constraint solving inherits the immutable world's `PhysicsStepPolicy`. CanonicalV1
uses the qualified 10 velocity and 2 position iterations for the complete world;
individual constraints cannot override iteration counts. This keeps work budgets,
deterministic fingerprints and cross-backend behavior explicit. A future per-body
or per-constraint override requires a new typed capability/profile and qualification
matrix; passing native iteration fields through the descriptor is unsupported.

Portable rigid-body intent stores motion, mass policy and initial velocities but no
runtime handle, world pose or native state. Scene conversion combines validated
intent with an admitted shape and world-frame pose into a runtime creation
descriptor. After publication, query snapshots expose current pose, velocities and
awake/sleeping evidence separately; they are not persistence input, creation policy,
mutation authority or a lifetime-extending body lease.

Mass properties are a separate Physics-owned semantic result. `PhysicsMass` applies
one uniform derived density to an admitted finite-volume contributor set so an
explicit total mass preserves the compound's geometric distribution; the derived
density must remain within the CanonicalV1 density envelope. `PhysicsDensity`
derives total mass directly in kg/m³. Each analytic contributor contributes volume,
center, and inertia in SI units, and Physics combines translated/rotated contributors
with the parallel-axis theorem before publishing one center-of-mass inertia tensor.
The bounded compound input is a borrowed span and is never retained. It does not
subtract overlapping regions or infer a collider from a render mesh. Exact cooked
shape artifacts may provide a complete `PhysicsMassPropertiesOverride`, which is
validated as finite positive mass and symmetric positive-definite inertia without
accepting native solver state. Empty, non-volumetric, overflowing, non-finite, or
invalid-inertia input fails before body publication; no value is clamped or silently
replaced.

## Shape Authoring, Cook And Runtime Boundary

[ADR-085](../../adr/085-physics-shape-authoring-cook-and-runtime-boundary.md)
separates typed authored collider descriptors, Physics-normalized cook input,
target-keyed Horo cook artifacts and immutable runtime shape leases. Scene and
gameplay data retain stable Horo asset/material/subshape IDs; source paths, native
Jolt types, pointers and serialized solver state do not cross the boundary.

Boxes, spheres, capsules and static planes use analytic descriptors. Convex hulls,
triangle meshes, height fields and compounds use bounded canonical geometry and
stable child/material mappings. Dynamic bodies accept primitives, convex hulls and
convex compounds; triangle meshes, height fields and static planes remain static.
Scale is validated and baked before cook rather than applied to runtime shapes.

Analytic authoring resolves one owned body-local pose and typed positive finite
scale into scale-free geometry before admission. Boxes admit component-wise
non-uniform scale. Spheres and capsules require uniform scale because an affine
non-uniform result is no longer that analytic kind. Static planes preserve their
exact equation by inverse-transpose normal transformation and signed-distance
renormalization. Degenerate, overflowing or non-finite results reject without
mutating the authored request; native backends never reinterpret these rules.

Cook identity binds semantic source and dependency digests to Horo shape/cooker
schemas, the canonical tolerance profile, target platform and the exact private
solver build fingerprint. Runtime activation consumes only validated artifacts;
it never imports or cooks source and never silently substitutes a primitive.
Shape replacement builds a candidate lease first and swaps body references only
at the Physics pre-step safe point, retaining old leases until all readers and
frames that can reference them have drained.

## Scene Conversion And Activation Ownership

[ADR-087](../../adr/087-scene-to-physics-ownership-and-conversion.md) makes
authored rigid-body components explicit body producers, collider components
explicit contributors to one named body shape and constraint components explicit
links between named body slots. Colliders, render meshes, primitives and hierarchy
never create implicit bodies; authoring conveniences persist complete component
bundles.

Physics owns `PhysicsScenePlanBuilder`, which consumes immutable typed scene data
and resolves body graphs, transforms, shape/material/filter dependencies,
constraints and stable writeback mappings without editor or native types. The host
injects Physics as a scene activation participant. It builds a closed detached
world candidate; neither workers nor editor code mutate the active world or publish
handles.

`RuntimeSceneService` owns the aggregate candidate and sole activation path.
`CommitDeferredLifecycleChanges` atomically publishes ECS storage, resource leases,
the Physics world and its generation-scoped binding table only after all fallible
work and final generation/budget validation succeed. Replacement failure destroys
only the candidate and leaves the prior scene/world/query state unchanged. The
first Physics tick occurs after the complete bundle is authoritative.

[ADR-137](../../adr/137-terrain-foliage-ownership-data-tier-and-lifecycle.md)
keeps terrain/foliage collision on this ownership path. Terrain supplies immutable
cooked shape-install descriptors tagged with exact dataset/tile/content generations;
Physics prepares and owns bodies, native shapes, thread affinity and retirement.
Terrain observes typed readiness/leases only and cannot mutate an active body or claim
collision ready from visual residency. Replacement publishes through the aggregate
scene/cell barrier and retains old Physics resources until Physics acknowledges release.

[ADR-141](../../adr/141-terrain-foliage-cross-system-ownership-and-readiness.md)
defines the receipt protocol for that barrier. Terrain lends a bounded immutable neutral
collision snapshot with exact Terrain/content/mutation/cell/request/origin evidence.
Physics validates and prepares its own shapes, bodies, filters and material mappings, then
returns generation-tagged Ready/Prepared/Published/Retired receipts. Prepared publication
executes at the Physics pre-step safe point into activation-scoped routing; ordinary
queries cannot discover it until RuntimeScene/World Streaming commits the aggregate root.
A stale snapshot/receipt or required Physics failure rolls back the candidate, never the
old collision world. Physics alone acknowledges retirement after steps, queries and shape/
body readers drain.

[ADR-144](../../adr/144-destruction-ownership-authority-state-and-runtime-geometry-boundary.md)
keeps fracture semantics outside Physics. Contacts are bounded tick/generation evidence;
they cannot mutate health, chunk membership or Scene structure from a solver callback.
Destruction consumes eligible evidence after the step and may stage a later transition.
Physics prepares only the exact pre-cooked convex chunk shapes/bodies named by that
transition, publishes them privately at its safe point and returns typed readiness to
the aggregate Scene commit. Core 1.0 performs no runtime mesh cutting, convex
decomposition, collision cook or fallback-shape invention.

[ADR-145](../../adr/145-destruction-source-chunk-geometry-collision-and-cook-ownership.md)
places canonical chunk geometry/connectivity and solver-neutral convex inputs in the DFR
artifact, not in Physics. Physics consumes one exact artifact/chunk/subshape revision,
validates dynamic-convex and mass/material/filter policy, and cooks a separately keyed
solver/profile/platform-private immutable shape artifact under ADR-085. It cannot alter
chunk topology/identity or substitute a box/mesh/recomputed hull; Destruction cannot
serialize Jolt data or claim a native shape ready.

[ADR-146](../../adr/146-destruction-runtime-activation-physics-cleanup-and-rollback.md)
defines the runtime body transaction. Contact callbacks append only bounded immutable
evidence; Destruction consumes it after the step and plans a later transition. Physics
prepares exact pre-cooked chunk bodies privately, applies staged initial impulses and
installs them at the pre-step safe point into transition-ticket routing. Ordinary queries
continue to resolve the old root until RuntimeScene aggregate commit. Before that commit,
cancel/failure retires only the candidate after readers drain; after it, recovery is a
new transaction rather than restoration of old native handles. Solver sleep cannot
authorize canonical chunk cleanup.

## Character Query Boundary

[ADR-089](../../adr/089-character-controller-ownership-implementation-and-update-order.md)
keeps the Horo Character domain behind the initial `HoroEngine::Physics` target but
does not expose Jolt Character classes or add a replaceable backend ABI. One scene-
owned `CharacterWorld` implements the bounded Horo overlap/support/carry/sweep/
slide/step/ground algorithm against a read-only, tick/world-generation-affine
`CharacterPhysicsQueryContext`.

The private adapter maps Horo capsule queries, filter/material evidence and staged
impulses/presence targets to Jolt narrowphase/body commands. Native collectors do
not run gameplay or mutate bodies. A private adapter/solver replacement must
reproduce Horo golden semantics and requalify determinism/performance.

Character movement stages before the Physics step from committed bodies, admitted
kinematic targets and support point-velocity evidence. Physics then applies staged
commands and steps once. Post-Physics Character work finalizes support/attachment
and publishes the collision root with tick commit; it never performs a second move.

[ADR-090](../../adr/090-character-dynamic-body-visibility-push-and-proxy-policy.md)
separates dynamic query visibility, one-way staged impulses and optional solver
visibility. In `BidirectionalProxy`, Physics privately owns one derived kinematic
capsule proxy per admitted Character. The proxy collides only with selected dynamics,
is not a public/authored body and never becomes transform authority. Physics reduces
post-step proxy contacts into bounded stable Horo reaction evidence; Character may
consume committed evidence on the next tick through its ordinary sweep solver.

One-way impulses and proxy solver impulses are mutually exclusive for a pair. Proxy
creation, target updates, filter/schema generations, contact evidence and retirement
are part of the same scene/world/Character lifecycle and fail closed when the exact
capability or qualified determinism tier is unavailable.

[ADR-092](../../adr/092-character-controller-determinism-and-state-composition.md)
requires every Character checkpoint to bind one exact committed Physics/world
checkpoint, tick, origin, structure and determinism fingerprint. Stable support-body
bindings resolve against that detached Physics candidate during restore. Character
cannot restore independently or persist Jolt bodies, proxy state, manifolds, query
caches or native IDs. Aggregate failure leaves the active Scene/Physics/Character
bundle unchanged.

## Fixed-Step Pipeline

One physics tick:

1. apply deferred body, shape, and constraint changes
2. copy kinematic targets from scene transforms
3. apply forces, impulses, and controller commands
4. run broadphase
5. generate and update contact manifolds
6. solve constraints
7. integrate dynamic bodies
8. write dynamic results to runtime transform state
9. produce bounded collision and trigger events
10. record metrics and debug snapshot

Physics uses the fixed delta provided by
[Runtime Lifecycle](./runtime-lifecycle.md). It does not measure wall time
internally.

## Body Modes

| Mode | Transform authority |
|---|---|
| Static | Scene definition; changes rebuild or update the physics representation |
| Kinematic | Scene/gameplay target drives physics |
| Dynamic | Physics result drives runtime transform |

Conflicting writes are rejected or ordered by an explicit controller contract.
A dynamic body cannot also be silently overwritten by an arbitrary transform
system after the physics step.

## Dynamic Body Inputs

`PhysicsBodyDynamicsCommand` is the backend-neutral fixed-tick contract for dynamic
forces, impulses, torques, angular impulses, per-body gravity scale and linear or
angular velocity controls. Commands carry the exact tick, scene generation,
generation-checked body handle, stable source and source-owned sequence. A receiving
world revalidates that complete frame and the live dynamic-body generation before
bounded queue admission; producer timing, worker completion and native body IDs are
never ordering inputs.

Linear force and torque are SI rates integrated exactly once by the owning fixed
tick. Linear and angular impulses are instantaneous SI changes and are never scaled
by render delta or applied a second time during presentation. An optional force or
impulse application point is an absolute coordinate in the active world's local-origin
meter frame, never a body-relative offset; absence means center of mass. Gravity scale multiplies the world's immutable gravity
vector and does not mutate world settings. Version one admits finite non-negative
gravity scales through `100`.

Velocity controls explicitly select set or additive semantics. Their vectors must
fit the body's admitted linear/angular speed ceilings; the live owner also validates
the resulting value for an additive command before mutation. Static and kinematic
bodies reject this physical-input contract because their transform authority remains
host-owned. Every command names `WakeIfSleeping` or `PreserveSleeping`; zero-valued
commands are valid and the latter policy never creates an implicit wake transition.
Unknown modes, stale handles, wrong scene/tick affinity, non-finite inputs and values
outside the CanonicalV1 command bounds fail before solver mutation.

## Structural Changes

Creating or removing bodies, changing shapes, and modifying constraints while
the world is stepping are deferred through a physics command buffer.

Commands carry scene and generation identity. Late commands targeting an
unloaded scene or stale entity are discarded with diagnostics.

## Collision And Trigger Events

Tick events include:

- contact began
- contact persisted where requested
- contact ended
- trigger entered
- trigger exited

```cpp
struct PhysicsContactEvent {
    SimulationTick tick;
    EntityId first;
    EntityId second;
    ContactEventKind kind;
    ContactSummary contact;
};
```

Events are stored in a world-owned bounded buffer and consumed during the
declared post-physics phase. They are not individually published to the
process-wide data bus.

An immersive editor agent admitted under
[ADR-172](../../adr/172-immersive-agent-ownership-authoring-mode-and-risk.md)
may consume bounded, generation-checked query or contact summaries as context.
Physics events remain simulation facts: contact, overlap, grab and throw evidence
never grants agent intent, proposal approval or solver-mutation authority.

If overflow occurs, the world records a metric and emits one diagnostic summary.

## Queries

Supported queries include ray, shape cast, overlap, and point tests.

Queries declare:

- target world and scene generation
- one stable project query channel plus explicit typed selectors where applicable
- whether triggers are included
- maximum result count
- ordering guarantee

The typed baseline represents ray, resident-shape sweep, resident-shape overlap
and point geometry as a closed variant. Every descriptor owns its geometry and
bounded selectors; it contains no native filter, callback or borrowed result
storage. A descriptor names one exact `PhysicsWorldId` and non-zero scene
generation. Resident shape/body selectors remain non-owning generation-checked
handles and are revalidated by the receiving world.

`Closest` and `Any` admit exactly one result; `Any` uses the same deterministic
closest-first winner as `Closest` while expressing that only one admissible hit is
needed. `All` admits at most the descriptor's bounded maximum.
`ThroughFirstBlock` first orders all admitted evidence, then retains overlaps through
the closest blocking hit. The CanonicalV1 public hit ceiling is
`1024`; exceeding it fails admission instead of allocating or truncating silently.
When caller-provided storage cannot retain every contractually selected hit, the
result reports `truncated` explicitly.

Public ordering is closest distance first. Exact-distance ties order `Block`
before `Overlap`, then body slot/generation, shape slot/generation, stable
subshape identity, hit position/optional normal and exact material
asset/generation/slot evidence. Native
broadphase order, native IDs, pointers and face indexes never participate. Hits
with identical public evidence are equivalent; implementations may coalesce them
but cannot expose traversal order as a further tie-break.

Hits copy stable body, shape, layer, profile, query-channel, schema, optional
subshape and optional material evidence into caller-owned bounded storage. They
do not extend world, schema, shape or material leases. A missing geometric hit is
a successful zero-hit result; stale world/scene/filter generations and malformed
evidence are typed errors.

Immediate queries execute on the physics owner thread outside a step. Parallel
or asynchronous queries use a read-only broadphase snapshot with documented
staleness.

CanonicalV1 currently admits bounded analytic query fixtures (box, sphere, capsule
and static plane) through the active world solely to exercise this query contract
until authored scene conversion publishes resident bodies and shapes. Fixture
creation and retirement are owner-thread operations outside a fixed step. The Horo
channel/profile/layer/trigger selectors are applied by the native body filter before
collector callbacks; collectors retain at most `1024` hits and project only copied
generation-checked evidence. Callbacks cannot mutate world structure. A world records
the scene generation at the last completed fixed tick, so queries before the first
publication or against a different scene generation fail with `QuerySnapshotStale`.

## Layers And Filtering

[ADR-086](../../adr/086-collision-layer-profile-and-query-channel-policy.md)
defines separate opaque 128-bit `CollisionLayerId`, `CollisionProfileId` and
`PhysicsQueryChannelId` values. They are project-stable identities; display names,
list positions, serialized bitmasks and Jolt object-layer values are not identity.

The committed project collision schema owns one complete symmetric layer-pair
matrix. `Ignore` rejects simulation, `Overlap` detects without solver response and
`Block` admits contact solving. Every complete reusable profile selects exactly one
simulation layer and an explicit `Ignore`/`Overlap`/`Block` response for every
query channel. Simulation and query responses are distinct enum types and cannot
be substituted for one another.

Scene preparation validates exact profile/channel references and acquires an
immutable schema generation. The world deterministically compiles dense indices,
bitsets, broadphase partitions and native adapters as private state. Immediate
queries use the active generation; snapshot queries lease the captured generation.
Unknown IDs, missing/asymmetric entries and unsupported runtime changes fail with
typed diagnostics rather than using the project's authoring default.

Semantic schema changes build complete candidate tables and publish atomically at
the Physics pre-step safe point. Bodies retain typed IDs plus schema generation,
and every debug/event/query projection translates private indices back to Horo IDs.

## Determinism

[ADR-088](../../adr/088-physics-determinism-capability-and-support-tiers.md)
defines four fail-closed capability tiers: `Unspecified`, development-only
`SameMachineDiagnostic`, the 1.0 target `SameBuildSamePlatform`, and future
`CrossPlatformQualified`. The current architecture decision does not itself mark a
shipping tuple qualified; PHY-007.2/.3/.5/.8 ordering, state, replay and evidence
gates must pass first.

Compatibility is an exact versioned fingerprint over shipped participating module
bytes, pinned solver source/defines, compiler/ABI/ISA/FP/job profile, Horo Physics
algorithms/schemas, fixed delta, content/filter/shape/material/package manifests,
initial state and ordered command protocol. Tier 2 requires identical fingerprints
and a qualified platform class. Tier 3 uses separate member fingerprints plus one
explicit pairwise-evidenced compatibility group; enabling Jolt's cross-platform
define locally creates no claim.

Initial state and every mutation use stable Horo identity/order/seed and tick-indexed
command frames. Included checkpoints, body/constraint state, events, writeback,
origin and command results compare exact canonical encodings. Raw callback, native
enumeration and broadphase query order are excluded; an authoritative Horo query
must revalidate/canonicalize/sort results before it can influence simulation.

`PhysicsCommandOrderKey` is the single structural-command ordering authority. Its
version, simulation tick, Physics world generation, scene generation, semantic
target class, stable Horo target identity, command kind, stable source identity and
source-owned per-tick sequence form the canonical comparison tuple. The bounded
world queue admits future tick-indexed commands in any producer insertion order and
sorts in-place on the Physics owner thread before observation or native mutation.
Duplicate source positions, missing source predecessors, completed ticks and stale
world/scene generations reject the complete frame without changing the previously
published tick. Native IDs, pointers, container iteration, worker completion and
admission timing are not ordering inputs.

`PhysicsSeedPolicy` records the exact algorithm and version, immutable policy
revision, explicit root/session seeds and owning world generation. Randomness is
derived purely from that policy plus a named `PhysicsRandomStreamId`, stable
consumption owner and non-zero owner-controlled sequence. The policy has one fixed
little-endian encoding for later build/session fingerprint composition. Physics
does not read a process-global RNG, wall clock, process/thread identity or worker
schedule, and the derivation API owns no mutable hidden consumption state.

CanonicalV1 remains serial and uses the normal non-cross-platform build. Parallel
stepping or a future `CrossPlatformDeterministicV1` profile needs a new fingerprint,
performance/support matrix and qualification. Determinism alone does not authorize
rollback or lockstep; complete Horo snapshots, structural resimulation, history,
side-effect and Network authority contracts remain separate.

## Threading

The initial canonical model has one physics owner thread within the fixed tick.
Runtime creation, world activation, structural admission, native stepping and
publication stay on that thread; foreign mutable calls return the typed
`physics.thread_affinity.violation` error. Read-only published-tick snapshots are
the only current cross-thread world call.

Optional solver-neutral child work is admitted as an immutable per-tick batch and
dispatched through an injected Horo `JobSystem`. Workers may touch only job-owned
or privately synchronized data; they cannot mutate world structure, retain world
or adapter references, publish results, or begin the next tick. The owning
`TaskGroup` applies one finite deadline, cooperatively cancels on timeout/failure,
and drains every accepted child before `AdvanceFixedTick` returns. Publication
occurs only after a successful join. A child failure moves the world to `Failed`
once, leaves the prior publication intact and clears the simulating guard before
control returns; shutdown can then retire the world without live captures.

Internal parallelism may be expanded behind the world interface when:

- dependency order remains deterministic within the declared contract
- component storage is not accessed concurrently without snapshots
- task completion is joined before results are published
- shutdown and scene unload cancel or join all physics tasks

Gameplay code does not retain references into solver-owned temporary memory.

## Reload And Play Mode

Entering editor play creates a play-session physics world from the runtime scene
definition. Stopping play destroys it without modifying authoring transforms.

Reload rebuilds physics state by default. Preservation of velocity or sleep
state requires a typed policy keyed by stable object ID.

## Floating Origin Rebasing

The active `PhysicsWorld` executes in local rebased cluster coordinates relative to the dynamic floating origin:

- **Local Half-Extent**: Simulation is bounded by $[-R_{\text{physics}}, +R_{\text{physics}}]$ (default $8192\,\text{m}$). This is independent of the rebase trigger $R_{\text{threshold}}$ (default $1000\,\text{m}$). See [Coordinate Precision And Origin Rebasing](./coordinate-precision-and-origin-rebasing.md).
- **Two-Phase Protocol**: As a registered `IOriginRebaseParticipant`, the physics adapter validates solver lock state during `PrepareRebase` and shifts spatial data during `CommitRebase`.
- **Position Updates**: Bodies, colliders, broadphase bounding volumes, and raycast caches have $\Delta_{\text{origin}}$ subtracted:
  $$\vec{x}_{\text{new}} = \vec{x}_{\text{old}} - \Delta_{\text{origin}}$$
- **Velocity Invariance**: Because origin shifting is an instantaneous coordinate re-indexing, linear velocity $\vec{v}$, angular velocity $\vec{\omega}$, and applied forces $\vec{F}$ remain strictly unchanged ($\Delta \vec{v} = 0, \Delta \vec{\omega} = 0$).
- **Solver State Continuity**: Contact manifolds retain relative contact points and penetration normals. Sleeping rigid bodies and deactivated islands remain asleep without triggering wake-up spikes or momentum shocks.
- **Timing Safe Point**: Origin shifts are forbidden while `PhysicsWorld::Step` is executing. Shifts execute only at the declared pre-render frame synchronization safe point.

See [Coordinate Precision And Origin Rebasing](./coordinate-precision-and-origin-rebasing.md) and [ADR-026](../../adr/026-large-world-precision-and-floating-origin-strategy.md).

## Debugging And Metrics

Physics exposes:

- step, broadphase, narrowphase, and solver time
- active and sleeping body counts
- shape and constraint counts
- broadphase pair and contact counts
- command and event buffer depth
- dropped event count

Debug draw data is extracted into a bounded render snapshot. The renderer does
not access live physics storage.

## Error Handling

### Initial world resource profile

`PhysicsWorldBudgets` captures limits, not allocations or performance guarantees.
The initial profile uses the following independent reservations. Counts are
positive; reducing a limit never silently changes another field.

| Reservation | Default | Supported maximum |
|---|---:|---:|
| Resident shape identities | 65,536 | 1,048,576 |
| Contact body pairs | 65,536 | 1,048,576 |
| Contact constraints (not authored joints) | 10,240 | 1,048,576 |
| In-flight contact pairs | 16,384 | Contact-pair capacity |
| Command entries | 4,096 | 65,536 |
| Event entries | 8,192 | 65,536 |
| Query entries | 1,024 | 65,536 |
| Commands admitted per tick | 4,096 | Command capacity |
| Queries admitted per tick | 1,024 | Query capacity |
| Dedicated temporary solver storage | 64 MiB | 256 MiB |
| Resident immutable shape storage charged to a world | 256 MiB | 1 GiB |

Byte limits are positive. Body, collider-slot, authored-constraint and scene-plan
limits remain in `PhysicsWorldCapacity`; shape lease storage is not scene-plan
storage. These initial resource ceilings bound admission but do not establish
total native allocation size, a wall-clock tick deadline or qualified throughput.
World creation still accounts for all native overhead and process-wide budgets.

Shape, command and query admission rejects before exceeding capacity. Required
contact/event tick-output overflow suppresses tick publication rather than
dropping records or publishing a partial result. The pinned native temporary
allocator cannot recover from overflow: `PhysicsScratchExhaustionPolicy::FatalProcess`
is its explicit supported policy and the initial default. `FailTick` is rejected
before native allocation until a qualified recoverable allocator/solver path
exists. It is not implemented by returning a null pointer to native solver code,
throwing through no-exception native frames or using an unbounded malloc fallback.
This scratch policy is separate from non-finite body/world containment.

### Runtime diagnostics

Invalid user or scene data returns diagnostics. Internal solver invariant
violations use assertions in development and preserve safety checks in release.

Physics diagnostic evidence uses six closed categories: configuration, cook,
runtime, query, event and lifecycle. Records copy one canonical Physics error code,
severity, bounded message and at most eight typed context fields in stable key order.
The operation `Error` remains control-flow authority; a diagnostic record is inert
owned evidence with no logging, storage, event, lifetime or mutation authority.
Metrics/profiler ingestion and native solver callback translation remain separate
owning contracts.

The canonical solver adapter installs process-owned native trace/assertion callbacks
only for the admitted runtime lifetime and restores the prior hooks on retirement.
Callbacks may only attempt a non-blocking copy into one fixed per-world inbox while a
joined native step is active. The owner thread drains that inbox after the step,
normalizes validation, assertion and fatal conditions to stable `horo.physics` codes,
and retains one owned `PhysicsDiagnosticRecord`. Validation evidence is inert;
assertion and fatal evidence fail the world exactly once while preserving the prior
coherent publication. Reset, scene unload and shutdown clear the retained evidence,
and no callback may log, allocate, mutate gameplay state or retain native text.

`PhysicsMetricSnapshot` is the immutable bounded handoff for one committed tick.
Physics validates its exact world and publication revisions, finite host/adapter-
supplied durations, coherent counts and admitted world limits before invoking any
pre-bound Telemetry handle. Process composition registers the closed metric
vocabulary and selects `Off`, `Core` or `Detailed`; an unavailable required binding
rejects explicitly, while optional unavailable and policy-off bindings remain
distinct from a zero measurement. World activation creates the binding on the
Physics owner thread; moving the binding never transfers that affinity. The
world generation is validation evidence, never a metric dimension: process metrics
remain intentionally aggregate and low-cardinality. Valid dropped-event and
overflow evidence at the admitted bounds is still published; only impossible
one-over snapshots reject before invoking a provider. The fixed-step path performs
no string lookup,
dimension binding, clock read or heap growth. Telemetry contention, saturation,
shutdown and stale internal handles may lose observations but cannot influence
simulation admission, order, state or determinism. Detailed stage timings are
profiler-consumable measurements only; this slice does not create a second profiler
store, arm native capture or claim backend timing support.

NaN or non-finite body state is detected at owned boundaries, associated with
body/entity identity, and quarantined or treated as fatal according to the
configured runtime policy.

## Testing

### Physics Foundation Qualification Matrix

The Physics foundation contract is qualified in headless CI on Ubuntu 24.04
x86_64/GCC, macOS 14+ arm64 or x86_64/Clang, and Windows 11 x86_64/MSVC. Both
`Canonical` and explicit `Null` compositions build on every supported platform;
Canonical solver execution runs wherever the pinned native solver target is enabled,
while Null fixtures prove omitted-capability behavior without a graphics or window
dependency.

Qualification evidence is split by invariant rather than duplicated in one monolithic
fixture:

- `PhysicsFoundationQualificationTests` exercises two-world isolation, interleaved
  stepping and destruction, repeated reset/unload/shutdown, fatal joined-job cleanup,
  terminal diagnostics and explicit headless Null behavior.
- `PhysicsWorldTests` covers render-rate variation, fixed-phase ordering,
  deferred-command canonicalization, stale world/scene rejection, bounded command
  overflow, thread affinity and coherent publication. `FrameSchedulerTests` owns the
  shared fixed-tick catch-up limit exercised by the Physics participant.
- `PhysicsHandleRegistryTests` covers stale and cross-world handles plus terminal
  generation retirement.
- `CanonicalPhysicsRuntimeTests` injects every supported partial process/world
  initialization failure and proves reverse-order native resource rollback.

All supported platform jobs execute the public fixtures. The native lifecycle fixture
also checks process-global allocator/factory release and per-world scratch, job-system
and solver counts; no successful test may leave native ownership live at process
teardown.

Required tests cover:

- fixed-step independence from render frame rate
- body and shape handle generation
- static, kinematic, and dynamic transform authority
- deferred mutation during simulation
- deterministic ordering fixtures
- collision begin/end and trigger semantics
- bounded event overflow
- query filtering and stable ordering
- scene unload and stale command rejection
- reload preservation policy
- non-finite state detection
- core collider shape primitives resolve correctly from the primitive catalog
- origin shift position translation without velocity or momentum alterations
- sleeping island preservation across origin rebasing transactions
- terrain/foliage collision descriptor generation, aggregate activation, stale
  replacement rejection and Physics-owned retirement
- Terrain collision snapshot/receipt generations, adversarial owner-safe-point order,
  required/optional failure and no partial query visibility before aggregate commit
- destruction contacts remain immutable post-step evidence, while pre-cooked chunk
  bodies prepare/rollback/publish/retire through Physics safe points without runtime
  geometry or collision cooking
- destruction body receipts remain transition/world/binding-generation scoped; private
  publication is not query visibility, and aggregate commit is the rollback boundary

## Related Documents

- [Physics Debugger UI Reference](./physics-debugger.html): collision layers, contact pairs, rigidbody inspection, and solver diagnostics panel.

- [Coordinate Precision And Origin Rebasing](./coordinate-precision-and-origin-rebasing.md)
- [ADR-026: Large-World Precision and Floating Origin Strategy](../../adr/026-large-world-precision-and-floating-origin-strategy.md)
- [Runtime Lifecycle](./runtime-lifecycle.md)
- [Scene Runtime](./scene-runtime.md)
- [World Streaming Architecture](./world-streaming-architecture.md)
- [Input Architecture](./input-architecture.md)
- [Built-In Scene Primitives](./built-in-scene-primitives.md)
- [Ownership And Resource Lifetime](../foundation/ownership-and-resource-lifetime.md)
- [Observability Metrics And Profiling](../observability/observability-performance.md)
- [ADR-137: Terrain and Foliage Ownership, Data, Tier and Lifecycle](../../adr/137-terrain-foliage-ownership-data-tier-and-lifecycle.md)
- [ADR-141: Terrain/Foliage Cross-System Ownership and Readiness](../../adr/141-terrain-foliage-cross-system-ownership-and-readiness.md)
- [ADR-144: Destruction Ownership, Authority, State and Runtime Geometry Boundary](../../adr/144-destruction-ownership-authority-state-and-runtime-geometry-boundary.md)
- [ADR-145: Destruction Source, Chunk Geometry, Collision and Cook Ownership](../../adr/145-destruction-source-chunk-geometry-collision-and-cook-ownership.md)
- [ADR-146: Destruction Runtime Activation, Physics, Cleanup and Rollback](../../adr/146-destruction-runtime-activation-physics-cleanup-and-rollback.md)
