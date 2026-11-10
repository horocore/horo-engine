# ADR-024: Perception Ownership, Sense Policy and Budget Decision

- **Status**: Proposed
- **Date**: 2026-08-28
- **Supersedes**: None
- **Scope**: AI perception subsystem, sensory stimulus emission, line-of-sight query seams, update policies, time-sliced budgets, and bounded memory decay
- **Issue**: [GAI-002.1](https://github.com/HoroCore/horo-engine/issues/1321)
- **Jira**: [HORO-1321](https://horo-engine.atlassian.net/browse/HORO-1321)
- **Companion decision**: [GAI-003.1 AI Decision Assets and Shared Gameplay Behavior Boundary](https://github.com/HoroCore/horo-engine/issues/1333) (ADR-025)
- **Normative documents**: [Navigation And AI Architecture](../architecture/runtime/navigation-and-ai-architecture.md), [AI Fixed-Tick Order, Authority and Simulation Budget](022-ai-fixed-tick-order-authority-and-simulation-budget.md), [Save Game And Persistence](../architecture/runtime/save-game-and-persistence.md)

## Context

Autonomous AI agents require sensory systems to detect, interpret, and react to their environment. In early game architectures and legacy implementations, perception was frequently coupled directly to rendering and audio output—scraping framebuffers, querying pixel depth or occlusion buffers, and inspecting audio mixer voice states. Such coupling creates severe architectural problems:

1. **Headless & Dedicated Server Failure**: Dedicated servers, continuous integration smoke runs, and headless simulation environments cannot execute perception if it depends on GPU contexts, display pipelines, or audio devices.
2. **Non-Deterministic Behavior**: Rendering resolutions, LOD popping, culling optimizations, audio volume attenuation curves, and hardware mixer voice limits introduce frame-rate-dependent and platform-dependent divergence into gameplay logic.
3. **Unbounded CPU Spikes**: Uncoordinated line-of-sight raycasts and all-to-all agent sensory evaluations cause quadratic scaling ($O(N^2)$) and major frame drops when agent counts increase.
4. **Memory Leaks and Dangling Handles**: Unbounded perception memory tracking unmanaged entity pointers or raw handles results in memory exhaustion and use-after-free crashes when perceived entities are despawned.

To achieve robust, high-performance, and deterministic AI simulation, Horo Engine requires a strict architecture decision that:

- Separates gameplay perception truth from rendering and audio presentation.
- Defines clear authority, timing ownership, and query seams for all built-in senses.
- Establishes a hybrid update model with strict per-tick time budgets and distance-based sensory LOD.
- Enforces bounded perception memory with deterministic linear decay, forgetting thresholds, and entity lifecycle safety.

## Decision

**Perception is an authoritative simulation subsystem owned by `PerceptionManager` in `SceneRuntime`. It operates exclusively on typed simulation stimuli and scene/physics spatial queries; it never scrapes renderer visibility, GPU depth buffers, or audio mixer buffers. Senses use a hybrid update model where instantaneous senses are event-driven and continuous senses are time-sliced across frames within bounded CPU raycast budgets. Perception memory is strictly bounded per agent, using linear decay, configurable forgetting thresholds, and weak entity tracking.**

### 1. Gameplay Truth vs Presentation Separation

```text
+--------------------------------------------------------------------------+
|                            SIMULATION LAYER                              |
|                                                                          |
|  Gameplay Systems / Emitters               Perception Subsystem          |
|  +-------------------------+ StimulusEvent +--------------------------+  |
|  | Weapon / Footstep /     |-------------->| PerceptionManager        |  |
|  | Health / Scene Emitter  | (Typed Data)  | - Sense Registries       |  |
|  +-------------------------+               | - Time-Sliced Schedulers |  |
|                                            +--------------------------+  |
|                                                          |               |
|                                                          v               |
|                                            +--------------------------+  |
|                                            | Agent Perception Memory  |  |
|                                            | - Bounded Tracked Targets|  |
|                                            | - Linear Decay / State   |  |
|                                            +--------------------------+  |
|                                                          | (Observer)    |
+----------------------------------------------------------|---------------+
                                                           |
                                                           v
+--------------------------------------------------------------------------+
|                           PRESENTATION LAYER                             |
|                                                                          |
|  Debug & Editor Visualizers                Audio & VFX Presentation      |
|  +-------------------------+               +--------------------------+  |
|  | Vision Cones / Hearing  |<--------------| Sensory Alert Cues       |  |
|  | Sensed Stimuli Overlay  |  (Read-Only)  | Dynamic Music Triggers   |  |
|  +-------------------------+               +--------------------------+  |
+--------------------------------------------------------------------------+
```

1. **Strict Presentation Independence**:
   - Perception logic **NEVER** inspects render viewports, depth buffers, GPU occlusion queries, or audio mixer voice output buffers.
   - All sensory inputs are emitted as strongly typed data structures (`StimulusEvent`, `SightStimulus`, `HearingStimulus`, `DamageStimulus`, `TouchStimulus`, `TeamStimulus`).
   - Headless servers, automated regression tests, and dedicated server processes execute identical perception logic with zero GPU or audio hardware dependencies.
2. **Unidirectional Observation**:
   - Presentation systems (editor debug overlays, gameplay HUD, spatial audio reaction cues) may observe perception state for rendering debug cones or triggering acoustic tension cues.
   - Presentation systems never feed data back into perception state.

### 2. Senses, Authorities, and Query Seams

Every built-in sense has an explicit authority, timing owner, and underlying query seam:

| Sense | Timing Owner / Dispatch Model | Authority & Source | Underlying Query Seam | Purpose |
|---|---|---|---|---|
| **Sight** | Periodic time-sliced (`PerceptionManager` tick) | `PerceptionManager` + `PhysicsWorld` | `PhysicsWorld::Raycast` / `Sweep` for LOS occlusion; `SceneRuntime` spatial index for candidates | Visual detection within FOV cone, peripheral angle, and sight radius |
| **Hearing** | Periodic time-sliced / queued stimulus drain | `PerceptionManager` + `AudioStimulusEmitter` | Distance attenuation + optional `PhysicsWorld` acoustic obstruction raycast | Acoustic detection of footsteps, gunshots, explosions, and environmental noise |
| **Damage** | Event-driven (immediate upon damage application) | `HealthSystem` / `CombatSystem` | Direct gameplay event carrying instigator entity, damage amount, and hit direction | Detection of inflicted harm, alerting agent to attacker identity/direction |
| **Touch / Proximity** | Event-driven (fixed physics tick) | `PhysicsWorld` collision/trigger dispatcher | Contact manifolds and trigger overlap events | Immediate awareness of physical contact, collisions, and proximity penetration |
| **Team / Affiliation** | Event-driven affiliation/distress updates plus periodic awareness broadcast | `TeamPerceptionRelay` | Squad/faction registry and communication radius or radio channel | Shared squad awareness, target spotting distribution, and distress alerts |

1. **Physics Query Seam**:
   - Line-of-sight (LOS) validation queries `PhysicsWorld` using dedicated collision query channels (`CollisionChannel::Visibility` / `SightOcclusion`).
   - Physics queries operate against read-only spatial acceleration structures (broadphase BVH) and do not mutate physics simulation state.
2. **Scene Spatial Seam**:
   - Candidate emitter gathering queries `SceneRuntime` spatial index (octree/BVH grid) to find entities within an agent's maximum sensory radius before running detailed LOS tests.
   - Eliminates $O(N^2)$ cross-comparisons between all agents.
3. **Team Dispatch Split**:
   - Membership, faction, direct distress, and explicit target-spot events are event-driven and invalidate or wake affected agents immediately.
   - Periodic broadcasts share selected, already-committed perception facts among eligible teammates at the configured relay cadence. They are bounded by relay fan-out/range budgets and cannot inspect another agent's mutable in-progress sense evaluation.

### 3. Update Policies, Time-Slicing, and CPU Budgets

Perception evaluation uses a hybrid execution model to maximize responsiveness while guaranteeing bounded CPU time:

1. **Hybrid Execution Model**:
   - **Event-Driven Inputs (`Damage`, `Touch`, team affiliation/distress)**:
     Handled immediately or collected in a bounded queue during fixed simulation
     ticks. Processing is $O(E)$ where $E$ is the number of events.
   - **Continuous Periodic Inputs (`Sight`, `Hearing`, team awareness relay)**:
     Evaluated at scheduled intervals across multiple simulation ticks and
     distributed evenly via time-slicing.
2. **Time-Sliced Scheduling & Per-Tick Budgets**:
   - The engine enforces hard limits per fixed simulation tick:
     - `maxSightRaycastsPerTick`: Maximum physics LOS raycasts allowed across all
       agents in one fixed simulation tick. It is profile-bounded (for example,
       at most 16 for `LowCpu` and 128 for `MediumCpu`).
     - `maxPerceptionExecutionTime`: Hard execution budget cap (e.g. 1.0 ms per simulation tick).
     - `maxAgentsEvaluatedPerTick`: Maximum number of agent sight sweeps per tick.
   - ADR-022's `GameplayAiProfile::maxPerceptionQueriesPerTick` is the aggregate
     admission cap for costly spatial and physics queries across all senses.
     `maxSightRaycastsPerTick` is its LOS-raycast subset and must not exceed it.
     Event receipt does not consume a query; a resulting LOS, overlap, or spatial
     lookup does.
   - Agents are scheduled with weighted fair round-robin queues. Deadline aging promotes an agent as its LOD service interval approaches, so continuously busy near-agent queues cannot starve distant queues. Overdue agents are serviced oldest-deadline-first before normal weighted slots.
   - Scene activation validates that configured budgets can satisfy the admitted population's maximum service intervals. If runtime population exceeds that envelope, the scheduler preserves the hard CPU/raycast limits, emits `PerceptionBudgetUnsatisfied`, and applies the configured deterministic admission/degradation policy rather than silently starving agents.
3. **Distance-Based Sensory LOD (Level of Detail)**:
   - Evaluation frequency dynamically scales with distance from simulation relevance anchors such as active player characters and gameplay objectives. Rendering camera/frustum state never controls authoritative LOD on network hosts or headless servers.
     - **LOD 0 (Near, $[0, 25\text{m})$)**: Full evaluation rate (e.g. 10 Hz / every 6 ticks at 60Hz).
     - **LOD 1 (Medium, $[25\text{m}, 60\text{m})$)**: Halved evaluation rate (e.g. 5 Hz / every 12 ticks).
     - **LOD 2 (Far/Active, $[60\text{m}, \text{dormancyDistance})$, default endpoint 150m)**: Throttled evaluation rate (e.g. 1-2 Hz / every 30-60 ticks). Agents beyond the endpoint remain LOD2 while gameplay-pinned, inside an active simulation/streaming relevance volume, or retaining a high-priority active stimulus.
     - **LOD 3 (Dormant)**: Entered only when the agent is outside active simulation relevance (for example, an inactive streaming cell), or beyond `dormancyDistance` with no gameplay pin or high-priority active stimulus. Continuous senses pause except for a bounded dormancy recheck (default every 120 ticks); any event-driven stimulus wakes the agent immediately and recomputes its LOD.

### 4. Memory Model, Decay, and Lifecycle Safety

```text
+-------------------------------------------------------------------------+
|                         AIPerceptionMemory                              |
|  +-------------------------------------------------------------------+  |
|  | Tracked Stimuli List (Bounded Ring Buffer / Fixed Array: max 16)  |  |
|  +-------------------------------------------------------------------+  |
|    | Stimulus 0: Target A | Sight | Age: 0.2s | Strength: 0.95 | Active |
|    | Stimulus 1: Target B | Sound | Age: 2.1s | Strength: 0.40 | Decaying|
|    | Stimulus 2: Target C | Damage| Age: 4.8s | Strength: 0.05 | Forgetting|
+-------------------------------------------------------------------------+
```

1. **Bounded Stimulus Storage**:
   - Each agent maintains an `AIPerceptionMemory` container with a compile-time hard cap (`kMaxTrackedStimuli = 32`) and a runtime cap (`maxTrackedStimuli`, default 16).
   - Prevents dynamic heap allocations during runtime perception ticks.
2. **Linear Decay and Forgetting**:
   - Every tracked stimulus possesses an `age` (seconds since last sensed) and normalized `strength` ($[0.0, 1.0]$).
   - While actively sensed, `strength` is refreshed to $1.0$ and `age` resets to $0.0$.
   - `decayRate` is measured in inverse simulation seconds ($s^{-1}$) and defaults to $0.1\,s^{-1}$.
   - When sight/sound is lost, `strength` decays linearly and is clamped:
     $$\text{strength}(t) = \max(0, 1.0 - \text{decayRate} \cdot \text{ageSeconds})$$
   - When $\text{strength} \le \text{forgetThreshold}$ (default $0.0$) or $\text{ageSeconds} \ge \text{memoryDuration}$ (default 10 simulation seconds), the stimulus is marked forgotten and evicted.
3. **Last Known Position & Extrapolation**:
   - Perception records store `lastKnownPosition` and `lastKnownVelocity`.
   - Behavior trees and blackboard queries access last known location rather than live target entity transforms, preventing AI agents from "cheating" through walls.
4. **Lifecycle and Weak Entity References**:
   - Perception memory stores weak generation-checked `EntityId` handles.
   - When an entity is destroyed or pooled, subsequent perception queries detect stale handles and discard the record immediately, preventing use-after-free and stale target locking.

### 5. Blackboard Publication And Persistence

1. **Blackboard Safe-Point Ownership**:
   - `PerceptionManager` owns sensing and `AIPerceptionMemory`; it publishes immutable staged `PerceptionDelta` records at the end of the Perception phase and never mutates `AIBlackboardView` directly.
   - `AIBlackboardSyncSystem` is the sole authority that maps committed deltas and last-known facts into schema-approved blackboard keys during `SystemPhase::BlackboardSync`. ADR-025 decision tasks and Service nodes consume the resulting read-only decision-phase view; they are not an alternate perception-to-blackboard writer.
2. **Save And Restore**:
   - The AI subsystem's `IGameplayStateProvider` stores a versioned perception chunk. `PerceptionPersistencePolicy::PreserveMemory` is the default and captures durable records (sense/tag, stable source reference when available, last-known position/velocity, strength, and simulation age). Ambient agents may explicitly select `ResetOnRestore`.
   - Raycast queues, scheduler buckets/deadlines, raw `EntityId` generations, and in-flight event buffers are transient and never serialized. Restore resolves stable source references in staging, preserves last-known facts when a source no longer exists, republishes blackboard deltas at the first `BlackboardSync`, and resumes decay from saved simulation age without applying real-world offline time.

## Ratify-or-Revise Outcomes

| Area | Current Baseline | Outcome |
|---|---|---|
| Gameplay truth vs presentation | Ad-hoc perception queries; risk of render/audio coupling | **Ratified & Enforced.** Perception operates strictly on typed simulation stimuli; zero graphics/audio presentation coupling. |
| Built-in senses & authority | Loose sense enum without explicit timing owners | **Revised.** Explicit timing owners, query seams, and dispatch policies defined for Sight, Hearing, Damage, Touch, and Team. |
| Line-of-sight query seam | Direct scene traversal | **Revised.** Bounded `PhysicsWorld` raycast seam with visibility collision channel. |
| Update policy & budget | Unbounded per-frame evaluation | **Revised.** Hybrid execution with strict per-tick raycast budgets (`maxSightRaycastsPerTick`) and time caps. |
| Sensory LOD | All agents evaluated equally regardless of distance | **Revised.** Multi-tier distance-based sensory LOD reducing update frequency for distant agents. |
| Perception memory & decay | Unbounded container, basic age float | **Revised.** Bounded fixed-capacity storage, linear decay formulas, forgetting thresholds, and weak entity tracking. |
| Blackboard publication | Perception/decision write authority unspecified | **Revised.** Perception stages deltas; only `AIBlackboardSyncSystem` commits schema-approved blackboard values. |
| Save / restore | Perception memory persistence unspecified | **Revised.** Versioned durable memory is preserved by default; transient query and scheduling state is rebuilt. |

## Verification Requirements

The committed gameplay delivery boundary admits typed sound events with a
canonical location, normalized intensity, radius, producer tags, and a lifetime
in fixed ticks. A producer may explicitly apply its own acoustic policy before
committing a delivery. The default uses the radius and existing scene visibility
policy with no mixer or Physics reads. One bounded batch accepts at most 128
deliveries, and subsequent accepted deliveries for the same listener and source
refresh one memory entry in delivery order. Only the last-known location and
weak source enter memory.

The C++ migration adds `HearingPerceptionEvent` to `GameplayPerceptionEvent` so
hearing uses the same recipient and generation checks as other authoritative
stimuli. Rebuild consumers of `PerceptionEventDelivery` and extend exhaustive
visitors with the new alternative. Existing damage, contact, and team producers
keep their initializers. Raw hearing observations now require the committed
Gameplay route, so migrate any direct sound writes to that route.

At a structural safe point, the perception owner republishes the complete
resident participant set. Omitted sources or listeners leave the active index
and the replaced publication revokes derived read leases. Memory prunes dead
source and provenance generations on its next owner-side snapshot. Sight
cancels a pending LOS batch when any captured endpoint retires, even before the
spatial publication is replaced. Reappearing slots with a new generation must
pass fresh gain samples. A streamed source with no current Scene entity has no
eligible memory or LOS result until it is registered again.

- LOD boundary tests cover 25m, 60m, `dormancyDistance`, active relevance pins,
  inactive streaming cells, and immediate LOD3 wake-up from event-driven stimuli.
- Sustained LOD0 load cannot starve LOD1/LOD2 queues while admission remains
  within the validated budget envelope; over-capacity produces the typed
  diagnostic without exceeding hard limits.
- Decay uses simulation seconds, the configured inverse-seconds rate, clamping,
  threshold eviction, and maximum-duration eviction deterministically.
- Perception cannot write a blackboard outside `BlackboardSync`; decision Service
  nodes observe only the committed snapshot.
- Save/restore preserves or resets memory according to policy, excludes transient
  scheduler/query state, revalidates stable sources, and republishes restored
  knowledge at the safe point.

## Consequences

- Dedicated server and headless simulation operate with 100% feature parity to client perception.
- AI behavior is strictly deterministic and decoupled from client rendering resolution, framerate, or audio mixer settings.
- CPU overhead is strictly bounded, preventing frame spikes during large-scale battles.
- Behavior trees and decision planners receive realistic last known target positions rather than cheating with perfect scene knowledge.
- Memory safety is preserved across entity destruction and scene streaming transitions.

## Rejected Alternatives

- **Scrape renderer depth buffer or GPU occlusion queries for sight.** Rejected: breaks headless/dedicated servers, introduces GPU-CPU readback stalls, and makes AI gameplay frame-rate-dependent.
- **Inspect audio mixer voice volume or bus output for hearing.** Rejected: mixes master volume/mute settings into gameplay truth, fails on headless servers, and violates audio thread lock-free invariants.
- **Synchronous all-to-all sight checks every frame.** Rejected: quadratic scaling ($O(N^2)$) causes massive CPU spikes with dozens of agents.
- **Unbounded heap-allocated stimulus memory per agent.** Rejected: creates memory fragmentation, heap allocation overhead during gameplay, and vulnerability to memory exhaustion.
- **Direct raw entity pointers in perception memory.** Rejected: leads to dangling pointers and crashes when entities are destroyed without notifying all observing agents.
