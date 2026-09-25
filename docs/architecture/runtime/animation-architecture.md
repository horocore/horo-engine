# Animation Architecture

## Purpose

This document defines Horo Engine's animation runtime. It specifies the skeletal
animation data model, animation clips, pose evaluation, animation graphs, blend
trees, inverse kinematics, root motion, retargeting, animation events, and the
integration between animation, physics, and rendering.

The goal is to provide a deterministic, data-oriented animation runtime that can
drive characters from simple loops to complex gameplay-responsive locomotion and
cinematics.

[ADR-061](../../adr/061-animation-ownership-update-order-and-clock.md) is the
single normative owner of pose ownership, animation clock domains, fixed-tick
order, root-motion timing, and physics/render handoff. This document owns the
broader animation subsystem model and summarizes that decision.

[Animation Asset Pipeline Contract](./animation-asset-pipeline-contract.md) is
the normative ANI-001.8 owner of animation authoring payloads, import settings,
typed dependencies, cooked representations, diagnostics, and the Animation/AST
boundary. The examples in this document describe semantic values only and are
not an alternate serialized format.

## Scope

Covered:

- skeleton asset format and pose representation
- animation clip asset format and compression
- animation sampling and evaluation
- pose storage, write-back, and skinning data flow
- animation graphs, blend trees, and state machines
- animation events and notifies
- inverse kinematics (IK)
- root motion
- retargeting
- GPU vs CPU skinning boundary
- integration with scene runtime, physics, and render extraction

Not covered:

- specific animation tools or DCC exporters (see [Asset Pipeline](./asset-pipeline.md))
- facial animation systems
- cloth or hair simulation
- ragdoll physics (see [Physics Architecture](./physics-architecture.md))

## Core Decisions

- Animations are data assets. The runtime samples them; it does not author them.
- Poses are the primary runtime currency: animation outputs a pose, IK modifies
  it, physics may override parts of it, and skinning consumes it.
- Authoritative animation advances once per attempted fixed simulation tick.
  Presentation interpolates committed poses and editor preview uses isolated
  state; neither advances gameplay animation.
- Animation graphs are authored assets that compile into a runtime evaluation
  tree. They are not interpreted scripts.
- Root motion is explicit: animation stages the exact fixed-tick interval delta;
  gameplay selects consumption policy and the character controller owns final
  collision-aware movement.
- Cinematic skeletal tracks are owner-admitted per-joint contributions. Animation
  continues to own/evaluate the graph and composes Override/Blend before Physics'
  final per-joint authority; presentation overlays never feed simulation.
- Retargeting is a cook/import-time process where possible. Runtime retargeting
  is supported but more expensive.
- GPU skinning is the default for skinned meshes. CPU skinning is a fallback for
  debugging, low-end platforms, or custom deformation requirements.

## Skeleton

### Identity And Component Contract

`HoroEngine::AnimationApi` owns the backend-neutral ANI-001.2 contract in
`Horo/Animation/AnimationIdentity.h` and `AnimationComponents.h`. Persistent
skeleton, clip, graph, and retarget identities wrap canonical Assets identities in
distinct C++ types. Stable component and joint identities do not depend on names,
entity slots, hierarchy positions, memory addresses, or load order.

Runtime instance and pose handles are process-local, non-owning, non-serializable,
and generation checked. An instance handle is fenced by runtime incarnation,
authored component, registry slot, and slot generation. A pose handle is also
fenced by its committed semantic generation; copying one does not retain recyclable
pose storage. Each typed registry admits at most `MaximumAnimationHandleSlots`
live or recyclable slots and rejects larger indexes before lookup. A
`PresentationPoseHandle` additionally fences the exact committed source pose to a
presentation-frame generation and frame-pool slot, so it cannot alias a replacement
pose or become later simulation input. Root-motion
requests carry the exact instance, attempted fixed tick,
and request generation, while successful tick commit separately owns durable
consumption deduplication.

The persistent authoring component declares exactly one clip or graph source, one
skeleton, an optional retarget profile, initial playback intent, and typed root-
motion policy. Its construction/validation is inert. The runtime component is an
immutable projection of the live instance, exact authored asset binding, explicit
evaluation domain/lifecycle state, and ordered previous/current committed poses.
It neither owns nor exposes mutable pose arrays, callbacks, jobs, leases, service
locators, concrete animation middleware, renderer state, or physics state.

Validation is bounded and allocation-free on success. It rejects reserved IDs,
contradictory source unions, unsupported contract versions, cross-runtime/component
handles, retired generations, asset-binding skew, and reversed committed-pose
generations before registry or pose access. Success proves value association only;
the Animation owner still proves residency and issues any immutable lease. Reload
publishes a compatible replacement at the owner safe point and retires old handles.
Scene unload and shutdown stop admission, cancel/join bounded work, revoke/drain
leases, and retire generations before pose storage, assets, and dependencies are
released. No fallback converts stale handles to a newly loaded instance.

A skeleton is an immutable validated hierarchy owned by `AnimationApi`. Candidate
assets are bounded before hierarchy work, then canonicalized with the
lexicographically least stable-identity topological order. Every parent therefore
precedes its children regardless of source container order, and disconnected roots
are retained in stable identity order. Duplicate identities, missing parents,
cycles, excessive depth/counts, malformed transforms, inconsistent inverse bind
matrices, non-reciprocal mirror pairs and sockets targeting absent joints fail with
stable typed results before publication.

Each joint has:

- stable typed identity and bounded advisory name
- optional stable parent identity (empty for a root)
- inverse bind matrix
- local-space reference transform
- typed retarget role, side and optional reciprocal mirror identity

```cpp
struct SkeletonJoint {
    JointId id;
    std::optional<JointId> parent;
    Mat4 inverseBindMatrix;
    Transform referenceLocalTransform;
    SkeletonRetargetRole retargetRole;
    SkeletonJointSide side;
    std::optional<JointId> mirror;
};

struct SkeletonAssetData {
    SkeletonId skeleton;
    std::vector<SkeletonJoint> joints;
    std::vector<SkeletonSocket> sockets;
};
```

Skeleton assets are shared. Multiple meshes and animation clips reference the
same skeleton. `SkeletonAsset::Create` is a synchronous load/cook/control-boundary
transaction and is not frame-hot. It owns only portable values and bounded dynamic
storage; it performs no I/O, service lookup, callback, job scheduling or backend
selection. Initial load and compatible reload use the same validator. A reload
candidate must preserve the persistent `SkeletonId`; cancellation and shutdown fail
closed without publishing partial state or replacing the last good snapshot.

## Pose

`Horo/Animation/PoseStorage.h` is the ANI-001.5 owner of bounded local/model
pose memory. `PoseFrameArena::Create` runs only at a runtime control boundary and
preallocates structure-of-arrays storage for its complete pose and joint policy.
After creation, frame begin/reset, pose allocation, local mutation, dirty
propagation, hierarchy evaluation, lease acquisition/release, cancellation, and
shutdown perform no dynamic allocation or blocking I/O.

The arena is bound to one `AnimationRuntimeId`, persistent `SkeletonId`, and exact
`SkeletonAssetGeneration`. Mutation is owner-thread-only. Canonical skeleton order
provides parent-before-child evaluation, while a sorted stable-ID lookup keeps joint
identity separate from dense storage. A local edit marks exactly that joint and its
descendants dirty. Full evaluation visits every dirty joint once; partial evaluation
visits only requested joints and the ancestors required to make their cached model
matrices valid. Both paths have work bounded by the captured joint limit.

Each allocation returns a `PoseHandle` carrying the runtime/instance, recyclable
slot generation, and semantic pose generation. Beginning a strictly newer frame
retires every unleased slot without wrapping its generation. Old frame handles,
cross-runtime handles, non-monotonic frames, skeleton reload skew, malformed local
transforms, and exhausted capacity fail with stable typed results rather than
aliasing replacement storage.

`PoseReadLease` is move-only and exposes immutable contiguous local transforms plus
checked model-matrix lookup. A lease may be read and destroyed on a consumer thread,
but it pins the exact slot: owner mutation, reset, cancellation, and shutdown reject
while any lease remains. Shared internal control state makes a late lease destructor
safe if the arena facade has already been destroyed; it does not restore admission or
grant mutation authority. Cancellation retires uncommitted frame storage, and
idempotent shutdown closes all later admission after leases drain.

Rules:

- Local transforms are authoritative.
- Model-space matrices are computed once per pose generation on first access,
  then cached until a new candidate pose invalidates them.
- Mutable poses are owned only by Animation Runtime. Gameplay and physics receive
  generation-checked read-only leases; physics returns typed post-step overrides
  rather than writing pose arrays.
- Working poses use bounded instance/frame storage. Previous/current committed
  poses remain valid until presentation and render leases retire.
- Render extraction receives a frame-owned immutable pose/palette projection
  tagged with scene, instance, tick, and pose-generation identity.
- Arena statistics expose fixed capacity, current/peak usage, failed allocations,
  and active leases without allocating; hosts may project these into observability.

### Cinematic Pose Authority

[ADR-118](../../adr/118-animation-character-and-gameplay-authority-during-cinematics.md)
defines cinematic pose composition. A sequence player submits immutable generation-
checked contributions through the Animation adapter; it never receives mutable pose
storage. Activation validates stable skeleton/joint-mask identity, required/optional
claims, player priority/order, evaluation seam, finite weights and handoff policy.

For every attempted fixed tick, Animation evaluates the underlying graph first. It
then applies cinematic `Override` (weight 1 on declared AnimationDriven joints) or
`Blend` contributions in canonical ADR-117 player/joint order. The graph is not
suspended merely because its output is masked, so cursor/state/event progression
remains tied to actual simulation ticks. Cut or bounded blend handoff begins from the
last committed pose and cannot restore a stale pre-cinematic snapshot.

PhysicsDriven joints reject required cinematic ownership unless Physics/Animation
first perform an explicit safe-point authority transition. Existing `Blended` Physics
authority composes over the graph-plus-cinematic Animation-side candidate after the
step. A `PresentationOverlay` modifies only the immutable presentation pose and is
excluded from root motion, events, hit shapes, colliders and later fixed-tick input.

Gameplay animation-parameter commands targeting an exclusive cinematic claim return
`SuppressedByCinematic`; unclaimed parameters remain admitted and declared blend
parameters are owner-composed. Suppressed commands are not reported as success or
queued until release. Stop, scene/actor generation loss and authority change close
contribution admission before owner-safe-point handoff/lease retirement.

## Animation Clip

A clip stores immutable sampled animation data for one exact skeleton publication.
`Horo/Animation/AnimationClip.h` is the ANI-001.6 owner of the portable clip,
directed traversal, and sampling contract. Persistent `AnimationClipId` remains
separate from the non-reusable `AnimationClipGeneration`; reload never makes an
old generation refer to replacement data.

```cpp
struct AnimationClipDescriptor {
    AnimationClipId id;
    AnimationClipGeneration generation;
    SkeletonId skeleton;
    SkeletonAssetGeneration skeletonGeneration;
    AnimationDeltaTime duration;
    AnimationSampleRate sampleRate;
    AnimationWrapMode wrapMode;
    AnimationClipKind kind;
    bool hasRootMotion;
    AnimationCompressionScheme compression;
    optional<AnimationReferencePoseBinding> referencePose;
};
```

`AnimationTime`, `AnimationTimeDelta`, and `AnimationDeltaTime` use signed 64-bit
nanosecond ticks. Clip-local sample positions are in the closed interval from zero
through duration; cursor phases are normalized by wrap mode. Sample rate is a
positive reduced `uint32/uint32` rational. Load/cook rejects non-canonical rates,
non-positive or excessive durations, overflow-prone metadata, and unsupported
typed values rather than converting through binary floating-point seconds.

Per-joint tracks use stable `JointId` and complete local-transform keys. Candidate
track and key containers are canonicalized by joint identity and exact key time at
the load/cook boundary. Duplicate tracks/times, missing joints, invalid transforms,
unknown interpolation, and bounded-count violations fail transactionally. Runtime
keys are already decoded portable values; compression metadata never exposes a
native codec or causes frame-hot I/O.

Track transforms contain:

- translation
- rotation (quaternion, shortest-path interpolation)
- scale

Compression options:

- `None` — full precision
- `Linear` — linear key reduction
- `Adaptive` — error-tolerant key reduction per joint

Additive clips are a separate clip kind, not a compression scheme. They bind one
stable `AnimationReferencePoseId` and immutable reference-pose generation. Sampling
requires that exact binding plus complete immutable reference and current base-pose
views. It computes the sampled reference-relative translation, rotation, and scale
delta and composes it onto the current base pose. Missing, stale, or size-skewed
reference input fails before any output write. A clip may be both additive and
compressed.

Cook-time compression may reduce precision based on the active cook profile.

### Compression Cook Contract

`Horo/Animation/AnimationCompression.h` owns ANI-001.7. Compression is an
Animation/Asset-Pipeline cook-boundary transaction over an already validated
`AnimationClipAsset` and its exact skeleton publication. The caller captures the
current clip and skeleton generations before admission. Cancellation or shutdown
rejects before reduction begins, and reload accepts a replacement only for the
same stable clip, skeleton, profile, scheme, and compression-contract domain. A
failure never replaces the last good immutable publication.

`Lossless`, `Balanced`, and `Aggressive` resolve to complete typed profiles with a
stable `AnimationCompressionProfileId`; no project setting, platform codec,
service locator, mutable global, or backend handle supplies an implicit fallback.
Every profile declares finite source/output-key and error-evaluation limits plus
finite translation, quaternion-angle, and scale thresholds. `Linear` performs one
canonical reduction pass. `Adaptive` deterministically subdivides each segment at
the intermediate key with the greatest normalized threshold error, retaining the
earliest key when errors tie. Step and cubic boundaries are retained because
replacing them with linear interpolation would change their semantics.
The immutable statistics report exact source/output/removed keys, maximum keys per
track, and performed error evaluations.

The compressed publication carries its exact contract version, profile, stable
clip and skeleton identities, immutable generations, and representation. Runtime
sampling must present that complete compatibility value; old generations fail as
stale and another profile or representation fails as unsupported. Before writing
caller-owned output, the wrapper proves the track and binary-search worst-case fit
the captured `AnimationDecompressionBudget`. Successful frame-hot sampling then
delegates to the immutable clip sampler and performs no allocation, blocking I/O,
callback, global lookup, or backend dispatch. Immutable publications may be read
concurrently while their owner-provided lifetime remains valid. The asset owner
closes admission, joins/cancels cook work, retires old publication leases, and only
then destroys clip and skeleton storage during unload or shutdown.

Migration: the ANI-001.6 `AnimationClipDescriptor::compression` field remains the
portable representation tag, but it is not sufficient proof that a cooked artifact
is compatible. Existing compressed artifacts must be recooked to publish the
ANI-001.7 compatibility value and statistics; runtimes must not synthesize missing
profile identity or generation evidence.

### Clip Sampling

`AnimationClipAsset::Sample` writes a complete local pose into caller-owned bounded
storage. Untracked absolute joints retain the exact skeleton reference pose captured
at clip publication. Step, linear, and zero-tangent cubic-Hermite interpolation are
selected by the left key; rotations use normalized shortest-path interpolation.
After successful `Create`, sampling performs no allocation, blocking, I/O, service
lookup, callback, or mutable global access and is safe on concurrent readers while
the immutable clip and caller views remain alive.

Behavior:

- `AnimationClipAsset::Traverse` accepts a canonical cursor and exact signed delta,
  then returns a complete candidate cursor, local sample time, direction, and
  boundary-crossing count without mutating player state
- `Once` clamps at the first reached endpoint and becomes terminal; a terminal
  cursor holds until its owner explicitly resets the player
- `Loop` keeps phase in `[0,duration)`; `PingPong` keeps phase in
  `[0,2*duration)` and reflects local sample direction at both endpoints
- `ClampForever` holds at an endpoint without becoming terminal and may later move
  away under an opposite signed delta
- reverse playback uses the same phase and crossing rules with a negative delta;
  it does not reverse simulation, physics, or irreversible side effects
- addition overflow, malformed phase, cancellation, shutdown, or a crossing count
  above the captured hard-bounded policy returns a typed failure with no partial
  cursor, event, root-motion, or pose output

Clip creation is a load/cook/control-boundary transaction. Cancellation and shutdown
are captured before work; reload requires the same persistent clip identity and
publishes only after complete validation. Sampling rechecks clip, skeleton, and
additive-reference generations. Asset owners retain the last good publication on
any validation failure and retire old immutable data only after their external
lease policy permits it.

## Animation Graph

An animation graph is a directed acyclic graph of nodes that produces a final
pose each frame.

```text
AnimationGraph
  +-- Output Pose
      +-- Blend Node
          +-- State Machine
          +-- IK Pass
          +-- Aim Offset
```

Graph assets compile to a runtime `AnimationGraphInstance`. The graph owns its
parameter block, pose working memory, player cursors, and the exact fractional
remainders produced by playback-rate multiplication and cursor accumulation.
Candidate cursors and remainders publish together only when their fixed tick
commits.

### Graph Parameters

Parameters are typed values exposed to gameplay:

```text
float speed
float direction
bool isAiming
int weaponSlot
```

Parameters are set by gameplay systems before graph evaluation. The graph does
not read raw input devices directly.

### Node Types

Core nodes:

| Node | Purpose |
|---|---|
| `Clip` | Sample a single animation clip. |
| `Blend` | Blend two poses by a float parameter. |
| `BlendTree` | Multi-directional blend by two axes (e.g., speed/direction). |
| `StateMachine` | Discrete states with transition rules and blend times. |
| `Layer` | Overlay an upper-body animation on a base pose. |
| `IK` | Modify a pose to reach a target. |
| `AimOffset` | Additive aim pose selected by pitch/yaw. |
| `Mirror` | Mirror a pose across a skeleton axis. |

Nodes produce poses; they do not allocate per evaluation. Memory is owned by the
graph instance.

### State Machine

A state machine node contains states and transitions.

- each state references a sub-graph or clip
- transitions have duration, curve, and trigger conditions
- trigger conditions should use hysteresis to avoid oscillation when a parameter
  hovers near a threshold
- transitions can be interrupted by higher-priority transitions
- state changes are deterministic given the same parameter inputs

Example states:

```text
Idle
  -> Walk (speed > 0.1)
Walk
  -> Run (speed > 3.0)
  -> Idle (speed < 0.1)
Run
  -> Walk (speed < 2.5)
```

## Blend Trees

A blend tree blends multiple clips based on up to two float parameters.

Common patterns:

- 1D blend by speed
- 2D directional blend by direction and speed
- freeform Cartesian blend

Interpolation rules:

- rotations use spherical linear interpolation (SLERP) or normalized lerp
- translations and scales use linear interpolation
- blend weights are normalized and clamped

## Inverse Kinematics

IK nodes modify a pose after animation sampling.

Supported solvers:

| Solver | Use case |
|---|---|
| Two-bone IK | Elbow/knee bending for foot/hand placement. |
| CCD | Longer chains, less stable but simple. |
| FABRIK | Longer chains, more stable. |
| Look-at | Head/aim direction. |
| Foot-lock / foot-plant | Prevent foot sliding on slopes and stairs. |

IK inputs:

- target position/rotation in model or world space
- hint position for elbow/knee polarity
- weight and blend curve

IK is applied after graph sampling and before physics read. Physics therefore
sees the IK-modified pose for ragdoll initialization, hit-detection shapes, and
any other pose query.

## Root Motion

Root motion produces a transform delta for one authoritative fixed-tick player
interval from the animation clip's root joint.

```cpp
struct RootMotionDelta {
    Vec3 translation;
    Quat rotation;
};
```

Rules:

- root motion is optional per clip
- the authoritative delta is extracted once for the exact directed fixed-tick
  player interval; presentation and preview never produce consumable root motion
- gameplay chooses whether to consume the delta for character movement, while
  the controller owns collision-aware resolution and final transform authority
- networked games must replicate root motion parameters or resulting transforms,
  not raw animation time
- requests carry scene/instance/tick/generation identity; consumption becomes
  durable only at successful tick commit, an aborted attempt's lease and staged
  marker are discarded, and a retry may consume its newly staged equivalent
- stale, duplicate-after-commit, preview, and leaked aborted-attempt requests are
  rejected
- reverse playback can emit an inverse directed delta only when the clip/node and
  owner policy admit reverse traversal; it never reverses the physics world

## Retargeting

Retargeting maps animation data from a source skeleton to a target skeleton.

### Cook-Time Retargeting

When source and target skeletons are known at cook time, retargeting bakes a
new clip for the target skeleton. This is the preferred path for performance.

### Runtime Retargeting

Runtime retargeting uses a retargeting profile that maps joints by name or
retargeting bone type. It is supported for dynamic sources (e.g., gameplay
packages, user-generated content) but costs more per frame.

Retargeting rules:

- source pose is normalized to a reference pose
- rotations are transferred using retargeting bone type semantics
- translation retargeting scales by skeleton proportions
- root motion is recomputed for the target skeleton when the source and target
  have different proportions or root joint conventions; otherwise it may be
  reused directly. The retargeting profile declares which path to use.

## Animation Events

Animation events are named markers attached to a clip at a specific time.

```cpp
struct AnimationEvent {
    EventName name;
    AnimationTime time;
    std::optional<AnimationDeltaTime> duration;
    VariantMap payload;  // typed key-value map; see foundation type glossary
};
```

Example event types:

- `Footstep.Left` / `Footstep.Right` — authoritative locomotion presentation timing
- `WeaponSwing` — enable/disable hit box
- `ReloadComplete` — gameplay notification
- `SpawnProjectile` — fire event
- `Custom` — gameplay-defined

The animation system stages event occurrences during fixed-tick evaluation and
publishes them only after that tick commits. Consumers subscribe by typed event
identity. Events do not directly spawn gameplay objects; they enter bounded owner
queues for a later permitted boundary. Reverse and large-interval traversal follow
ADR-061's eligibility and atomic preflight rules.

When an application maps an occurrence to sound, the Audio adapter preserves the
animation player generation, event ID, traversal/loop ordinal, direction, and
committed tick/time under
[ADR-068](../../adr/068-music-transport-and-cross-system-ownership.md). Audio owns
mapping that committed timestamp to its current sample epoch and deduplicating the
stable occurrence. Presentation pose sampling, editor scrub, or a failed tick never
submits Audio work, and callback completion cannot change Animation's event cursor.

[ADR-091](../../adr/091-footstep-and-locomotion-event-ownership.md) specializes
footstep ownership. Animation publishes the typed committed marker occurrence but
does not query a surface or call Audio/VFX. After tick commit, an application-owned
adapter correlates it with the exact same-tick Character surface snapshot and
deduplicates one semantic request. Character never synthesizes missing footstep
timing, and stale/missing support evidence suppresses presentation.

## Skinning

Skinning transforms mesh vertices by joint matrices.

`Horo/Animation/SkeletalMeshSkinning.h` is the backend-neutral ANI-001.4
asset boundary. A `SkeletalMeshId` identifies the persistent mesh independently
of a resident vertex buffer. Each immutable publication binds to one exact
`SkeletonId`, skeleton contract version, and non-reusable
`SkeletonAssetGeneration`; an older generation fails closed instead of silently
remapping to a reloaded hierarchy.

Vertices and section palettes use stable mesh-local `SkinningJointId` values.
The binding owns an explicit one-to-one remap to stable skeleton `JointId`
values, so neither names nor dense hierarchy positions become identity. Load or
cook validation sorts remaps, LODs, sections, palettes, and influences into one
canonical form. Influences must be finite and positive, are normalized in
double precision, and are ordered by descending normalized weight with stable
joint identity as the tie-breaker. Sections form a complete non-overlapping
vertex partition and every influence must belong to its section palette.

All LOD, vertex, section, remap, palette, and influence counts are bounded by a
captured policy and immutable hard ceilings. Validation is transactional at a
load, cook, or owner control boundary: cancellation, shutdown, version skew,
reload identity mismatch, stale skeleton generation, or malformed input
publishes no partial replacement. A successful `SkeletalMeshSkinningAsset`
owns its canonical snapshot independently of candidate and skeleton object
lifetimes. Animation runtime storage, pose palettes, renderer resources, and
backend-native handles remain outside this contract.

### GPU Skinning

Default path. The animation system uploads:

- joint matrices palette (model-space transform × inverse bind matrix)
- per-vertex joint indices and weights

The renderer consumes this in the vertex shader. Joint palette updates happen
once per skinned mesh instance per frame.

### CPU Skinning

Fallback path. The animation system computes deformed vertices on the CPU and
uploads the resulting vertex buffer. Used for:

- platforms without viable GPU compute
- debugging
- custom deformation not expressible in vertex shaders

CPU skinning must be explicitly enabled per mesh and is slower.

## Integration

### Scene Runtime

Skinned meshes and animation graph components live in the scene runtime. During
scene conversion:

- skeleton references are resolved
- clips are loaded or referenced
- graph instances are created
- skinned mesh render instances are registered

### Update Order

The authoritative non-AI path follows ADR-061 inside every attempted fixed tick.
This remains distinct from ADR-022's AI phase graph, but both paths are fixed-tick
contracts. Catch-up evaluates animation separately for every attempted tick; one
render-frame pose is never reused as the authoritative input to several physics
steps.

```text
Attempted Fixed Tick
  Gameplay/AI/Nav and owner-admitted cinematic inputs stage parameters, movement,
  desired heading and kinematic-platform targets
  Animation evaluates candidate pose, eligible IK, events, and root-motion delta
  Physics freezes Character query/support/platform-motion evidence
  Character applies platform carry and resolves desired movement plus root motion
  Physics applies staged commands, steps once and publishes body/pose overrides
  Character finalizes support and its authoritative collision-root transform
  Animation composes admitted overrides and finalizes candidate pose
  Tick commit publishes Physics, Character/transforms, player state, events, and poses

After FixedUpdate
  Presentation interpolates committed poses and applies presentation-only overlays
  Render extraction reads the immutable frame pose and joint palette
```

Simulation-affecting IK runs before controller/physics consumption. Post-physics
or presentation IK cannot feed same-tick root motion, movement, or colliders.
Interpolated cinematic overlays apply only to the render/preview snapshot after
movement authority resolves. Only owner-admitted non-interpolated fixed-tick inputs
participate in movement-producing evaluation.

The Character controller moves before Physics and performs only support/transform
finalization after Physics; it never performs a hidden second move. Platform carry,
capsule up/heading, root rotation and visual orientation ownership follow
[ADR-089](../../adr/089-character-controller-ownership-implementation-and-update-order.md)
and [Character Controller Architecture](./character-controller-architecture.md).

### Render Extraction

The renderer receives:

- transform
- mesh asset
- material
- frame-owned immutable joint palette snapshot/lease
- bounds

Bounds for skinned meshes must account for pose extents. Conservative bounds may
be used when exact pose bounds are too expensive.

### Physics

Physics reads pose for ragdoll initialization or hit-detection shapes. Physics
never writes Animation Runtime storage. Ragdoll or procedural physics publishes a
typed post-step override for the exact scene, instance, skeleton, tick, joint map,
and pose generation. Animation validates and composes it under explicit
`AnimationDriven`, `PhysicsDriven`, or `Blended` authority, then publishes the
committed pose. Animation-to-physics handoff is explicit and generation-checked.

## Asset Formats

The following JSON fragments are illustrative projections of typed Animation
values. They are not canonical files, do not assign persistent identity, and do
not define import or cooked bytes. ANI-001.8 owns those contracts in
[Animation Asset Pipeline Contract](./animation-asset-pipeline-contract.md).

### Skeleton Asset

```json
{
  "schemaVersion": 1,
  "assetType": "skeleton",
  "joints": [
    {
      "name": "Hips",
      "parent": -1,
      "inverseBindMatrix": [...],
      "defaultTranslation": [0, 1, 0]
    }
  ]
}
```

### Animation Clip Asset

```json
{
  "schemaVersion": 1,
  "assetType": "animation_clip",
  "skeleton": "skeleton_humanoid_001",
  "duration": { "numerator": 1, "denominator": 1 },
  "sampleRate": { "numerator": 30, "denominator": 1 },
  "wrapMode": "Loop",
  "hasRootMotion": true,
  "tracks": [
    {
      "joint": "Hips",
      "translationKeys": [...],
      "rotationKeys": [...],
      "scaleKeys": [...]
    }
  ],
  "events": [
    {
      "name": "Footstep.Left",
      "time": { "numerator": 1, "denominator": 4 }
    }
  ]
}
```

The rational objects above are illustrative schema values in seconds and samples
per second. ANI-001.6 owns their final field widths and normalization rules.
Durations use `AnimationDeltaTime`; cursors and event positions use
`AnimationTime`. Decoders reject zero denominators, overflow, non-canonical
fractions, and values outside the clip's declared duration instead of converting
through binary floating point.

### Animation Graph Asset

```json
{
  "schemaVersion": 1,
  "assetType": "animation_graph",
  "skeleton": "skeleton_humanoid_001",
  "parameters": [
    { "name": "speed", "type": "float", "default": 0.0 }
  ],
  "nodes": [...],
  "outputNode": "..."
}
```

## Diagnostics And Validation

- Skeleton mismatch between clip and mesh produces errors at import time.
- Graph cycles are rejected at compile time.
- Missing clips or parameters are reported with asset paths.
- IK targets outside reachable range degrade gracefully rather than exploding.
- Root motion clips used without root joint metadata produce warnings.

## Testing Requirements

- Unit tests for pose hierarchy updates and matrix multiplication.
- Sampling tests for loop/ping-pong wrap modes.
- Fixed-tick determinism tests across different render cadences and catch-up grouping.
- Pause, single-step, simulation-rate, reverse, and large-interval budget tests.
- Blend tree tests verifying normalized weights and rotation correctness.
- IK solver tests for known configurations.
- Retargeting tests comparing source and target poses.
- Visual regression tests for skinned mesh playback.
- Root-motion duplicate/stale request and physics-override authority tests.
- Cinematic per-joint Override/Blend/PresentationOverlay, continuing underlying graph,
  entry/exit handoff, PhysicsDriven conflict and typed gameplay-parameter suppression.
- Preview/play isolation, stale pose lease, reload, scene replacement, and shutdown tests.
- Performance tests for joint palette upload and GPU skinning throughput.

## Related Documents

- [Animation Editor UI Reference](../../../mock-studio/designs.md#architecture-runtime-animation-editor): clip timeline, state machine, blend tree, events, and skeleton preview panel.

- [Rendering Architecture](./rendering-architecture.md): skinned mesh render
  extraction and joint palette binding.
- [Cinematic Sequencer Architecture](./cinematic-sequencer-architecture.md): timeline,
  tracks, clock authority, and evaluation phase integration.
- [ADR-118: Animation, Character and Gameplay Authority During Cinematics](../../adr/118-animation-character-and-gameplay-authority-during-cinematics.md)
- [Physics Architecture](./physics-architecture.md): ragdoll, hit detection, and
  animation/physics handoff.
- [Asset Pipeline](./asset-pipeline.md): clip import, compression, and cook.
- [Animation Asset Pipeline Contract](./animation-asset-pipeline-contract.md):
  canonical authoring payload, import settings, dependency, cook, diagnostic,
  and migration ownership.
- [Advanced Rendering Architecture](./advanced-rendering-architecture.md):
  meshlets, GPU-driven rendering, and virtual geometry boundaries.
