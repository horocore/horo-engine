# ADR-025: AI Decision Assets and Shared Gameplay Behavior Boundary Decision

- **Status**: Proposed
- **Date**: 2026-08-28
- **Supersedes**: None
- **Scope**: AI decision graph assets (`BehaviorTreeAsset`, `StateMachineAsset`, `UtilityAiAsset`), runtime execution plan compilation, node hierarchy, UI separation, 1.0 paradigms vs post-1.0 extensions, task execution and lifecycle alignment
- **Issue**: [GAI-003.1](https://github.com/HoroCore/horo-engine/issues/1333)
- **Jira**: [HORO-1333](https://horo-engine.atlassian.net/browse/HORO-1333)
- **Normative documents**: [Navigation And AI Architecture](../architecture/runtime/navigation-and-ai-architecture.md), [Gameplay Behavior Authoring](../architecture/extensions/gameplay-behavior-authoring.md), [Save Game And Persistence](../architecture/runtime/save-game-and-persistence.md)

## Context

Horo Engine requires rich decision-making capabilities for AI agents, NPCs, and autonomous gameplay entities. Developers need behavior trees, state machines, and utility-based decision models that integrate seamlessly with the engine's scene runtime, blackboard system, and navigation stack.

Prior to this decision, the boundary between AI decision graphs, generic visual scripting, editor UI widgets, and runtime task execution was underspecified:

- **UI Coupling Risk**: Graph authoring tools often couple persisted asset schemas with editor UI state (such as `imgui-node-editor` node positions, canvas zoom/pan, pin layout coordinates, or visual styling). This bloats runtime assets, introduces editor dependencies into runtime modules, and makes headless/server builds fragile.
- **Generic Visual Scripting vs Focused Decision Graphs**: Attempting to make AI decision graphs a universal, Turing-complete visual programming language creates unnecessary complexity, poor performance, and difficult debugging. AI decision graphs require specialized evaluation semantics (ticks, reactive aborts, decorator conditions, utility curves) rather than generic imperative dataflow.
- **Task Lifecycle Duplication**: There was a risk of creating an isolated AI task manager, thread scheduler, or redundant component lifecycle that bypasses the deterministic `SystemPhase::Gameplay` fixed-step scheduling, scene command buffers, and standard `BehaviorContext`.
- **Scope Creep (HTN, GOAP, RL)**: Advanced AI paradigms such as Hierarchical Task Networks (HTN), Goal-Oriented Action Planning (GOAP), and Reinforcement Learning / LLM agents were undefined in terms of delivery phase, creating ambiguity for 1.0 deliverables.

[GAI-003.1] establishes the authoritative architectural boundary, asset model, compiler pipeline, 1.0 core paradigms, and runtime execution contract.

## Decision

**AI decision graphs (`BehaviorTreeAsset`, `StateMachineAsset`, `UtilityAiAsset`) are immutable, typed data assets compiled into flat, cache-friendly runtime execution plans. Persisted graph identity and schemas are strictly decoupled from editor UI widgets (`imgui-node-editor`). The 1.0 AI decision runtime standardizes on Behavior Trees, Hierarchical State Machines, and Simple Utility scoring; advanced planning (HTN, GOAP) and learned/generative policies are explicitly classified as post-1.0 extensions. AI task execution is fully aligned with the engine's existing `BehaviorExecutionContext` and ECS lifecycle, creating no competing task managers or ambient threads.**

---

### 1. Graph Asset Model & UI Independence

1. **Persistent Identity**:
   - Persisted graph assets (`.horo_bt`, `.horo_sm`, `.horo_utility`) store pure semantic graph topology: stable `GraphId`, `NodeId`, `PinId`, `PropertyId`, schema versions, node type identifiers, typed properties, and blackboard key bindings.
   - Editor layout data (node position $(x, y)$, visual routing points, comments, zoom/pan, expansion state) is stored exclusively in the asset's canonical `.meta` sidecar (for example, `Guard.horo_bt.meta`) or discarded during asset cooking. `.editor_meta` is not a second supported convention.
   - Runtime modules and cooked assets have **zero dependency** on `imgui-node-editor`, ImGui, or any visual layout types.

2. **Compilation to Flat Runtime Plans**:
   - Graph assets are not directly evaluated as pointer-chasing graph object hierarchies at runtime.
   - The `DecisionGraphCompiler` validates and compiles the source asset into an immutable `CookedDecisionPlan` (e.g., `CookedBehaviorTreePlan`, `CookedStateMachinePlan`, `CookedUtilityPlan`).
   - The compiled plan is a flat, contiguous array of node descriptors with precomputed integer offsets, child ranges, decorator masks, and blackboard index bindings.
   - Cross-paradigm calls remain asset references rather than pointers or nested plan copies. A call node stores an index into the owning plan's cooked dependency table; each entry contains a stable `DecisionGraphAssetId`, expected plan kind, and required schema version. The cook validates referenced assets, rejects dependency cycles, and enforces a bounded nesting depth.
   - Evaluation is cache-coherent and allocation-free in steady-state operation.

3. **Instance State vs Static Plan**:
   - `CookedDecisionPlan` is shared and read-only across all agent instances executing that asset.
   - Per-agent execution state is encapsulated in an allocation-conscious `DecisionInstanceState`. Each active Parallel branch owns a bounded execution-frame stack of `(DecisionGraphAssetId, DecisionNodeId)` pairs, so an HSM state can invoke a Behavior Tree without merging their flat plans. Decorator memory and timers are keyed by the same pair. Hot-reload and restore never identify state by transient array index.

---

### 2. Core 1.0 AI Paradigms

Horo 1.0 implements three complementary decision paradigms:

#### A. Behavior Trees (BT)

- **Composites**:
  - `Selector` (Fallback / Priority): Evaluates children in order until one returns `Success` or `Running`.
  - `Sequence`: Evaluates children in order until one returns `Failure` or `Running`.
  - `Parallel`: Evaluates all children concurrently with configurable completion policies (`RequireOneSuccess`, `RequireAllSuccess`, `RequireAllComplete`, `StopOthersOnFailure`).
- **Decorators (Conditions & Flow Modifiers)**:
  - `Inverter`: Inverts child result (`Success` $\leftrightarrow$ `Failure`).
  - `Cooldown`: Enforces a cooldown duration between child executions.
  - `Loop`: Repeats child execution for $N$ iterations or while a condition is satisfied.
  - `BlackboardCheck`: Evaluates a blackboard key against constants, ranges, or other keys with abort options (`None`, `Self`, `LowerPriority`, `Both`).
  - `TimeLimit`: Aborts child task if execution exceeds a specified duration.
- **Tasks (Leaf Action Nodes)**:
  - `MoveTo`: Issues asynchronous pathfinding and movement requests to the `NavigationSystem`.
  - `Wait`: Pauses branch execution for a specified duration or random range.
  - `PlayAnim`: Triggers an animation montage or clip and waits for playback/notify completion.
  - `CustomTask`: User-defined native C++ or script tasks conforming to the decision task contract.
- **Services**:
  - Periodic background checks attached to composite or subtree nodes that update the blackboard or evaluate environment queries at declared intervals while the subtree is active.

#### B. Hierarchical State Machines (HSM)

- `StateMachineAsset` is the canonical persisted asset family name; it supports both flat and hierarchical state topology. “Hierarchical State Machine” names the supported execution paradigm, not a separate `HierarchicalStateMachineAsset` type.
- Hierarchical states with nested sub-state machines.
- Explicit entry actions, update actions, and exit actions.
- Event-triggered and condition-triggered transitions with guard expressions evaluated against the blackboard.
- State machines can invoke Behavior Trees as sub-state behaviors.

#### C. Simple Utility Scoring

- Evaluates a set of competing actions using consideration response curves (Linear, Polynomial, Logistic, Step).
- Normalized scoring $[0.0, 1.0]$ with priority weighting and multiplier aggregation.
- Utility selector picks the highest-scoring action or samples from a bucketed top-tier probability distribution.

#### D. Explicit Post-1.0 Deferred Paradigms

The following paradigms are explicitly designated as **Post-1.0 Extensions** and must not delay the 1.0 architecture baseline:

- **Hierarchical Task Networks (HTN)**: Domain planning with compound tasks, methods, and primitive tasks.
- **Goal-Oriented Action Planning (GOAP)**: Dynamic action graphs solved via A* regression over world-state preconditions and effects.
- **Reinforcement Learning (RL) & Learned Policies**: On-device neural network inference / ML-agents.
- **Conversational / Large Language Model (LLM) Decision Providers**: External generative AI NPC reasoning seams.

These post-1.0 capabilities will plug in via dedicated provider interfaces without modifying the core 1.0 decision plan schema or execution lifecycle.

---

### 3. Runtime Task & Lifecycle Alignment

1. **Evaluation Phase & Concurrency**:
   - AI decision plans evaluate strictly within `SystemPhase::Gameplay` during the deterministic fixed-step simulation tick.
   - `AIDecisionSystem` is the sole scheduling and evaluation authority. `AiControllerComponent` and eligible `BehaviorComponent` attachments are inert bindings that identify an entity, plan asset, and configuration; the system discovers those bindings and evaluates them. Components do not own runners or private update loops.
   - Tasks execute sequentially or in parallel batches per agent; task code does not spawn uncoordinated OS threads or ambient background jobs.

2. **Standard Execution Context**:
   - AI nodes and tasks receive `BehaviorExecutionContext` (extending `BehaviorContext`), granting controlled access to:
     - `SceneRuntimeAccess&` / `EntityId`
     - `AIBlackboardView&` (typed blackboard read/write)
     - `GameplayInputAccess&`
     - `SceneCommandBuffer&` (deferred structural ECS changes)
     - `CancellationToken` (for aborting long-running asynchronous tasks)
   - Tasks do not perform direct raw pointer mutations on foreign entities or bypass ECS synchronization points.
   - `GameplayInputAccess` exposes read-only, semantic gameplay actions for shared tasks used by possessed/player-controlled entities; it never exposes raw device state. NPC-only contexts receive a deterministic empty action snapshot, so AI decisions cannot observe another player's input implicitly.

3. **Task Return Statuses & Cooperative Abort**:
   - Standard task statuses: `Success`, `Failure`, `Running`, `Aborted`.
   - When a task returns `Running`, it receives recurring ticks until completion or until higher-priority decorators/transitions trigger an abort.
   - Long-running async actions (e.g. navigation movement or animation playback) interact with underlying systems through cooperative tokens. On abort, the task's `OnAbort()` hook immediately cancels downstream subsystem requests.

4. **Hot Reload & Plan Replacement**:
   - Asset re-compilation generates a new `CookedDecisionPlan`.
   - Running instances migrate at safe tick boundaries:
     - If the active node and execution path are structurally compatible, instance state is preserved.
     - If the plan is incompatible, the running subtree is gracefully aborted (`OnAbort()`), instance memory is reset, and evaluation restarts from the root node.
   - A malformed or failing graph edit leaves the previous valid cooked plan active, logging a typed compiler diagnostic.

5. **Save And Restore Boundary**:
   - The AI subsystem contributes an `IGameplayStateProvider` to `RuntimeSaveService`. Capture occurs at the save snapshot safe point and records only durable logical state: plan asset IDs and schema versions, bounded execution-frame stacks identified by stable `DecisionNodeId`, serializable decorator/timer state, and schema-approved blackboard values.
   - Runtime-only handles (`JobHandle`, cancellation tokens, path/query handles, pointers, and cached array indices) are never serialized. Restore resolves the cooked plans in staging, migrates compatible stable IDs, and restarts or reissues asynchronous tasks after atomic state application.
   - Missing or incompatible plans produce typed diagnostics and reset the affected agent to its plan root according to project policy; they do not partially apply an invalid execution stack or prevent unrelated gameplay providers from restoring.

---

## Ratify-or-Revise Summary

| Area | Prior Architecture State | Ratified Outcome |
|---|---|---|
| Graph Asset Schema | Undefined separation between visual node editor and asset data | **Decoupled.** Graph assets store pure semantic data. Node coordinates/visual properties are editor-only sidecars. |
| Runtime Plan Representation | Unspecified (risk of deep runtime node pointer trees) | **Compiled flat plan.** Read-only contiguous plan bytecode/records, separate per-agent execution state. |
| 1.0 AI Paradigms | Loosely listed BT, HSM, Utility, GOAP, HTN | **1.0 frozen to BT, HSM, Simple Utility.** HTN, GOAP, RL, and LLM reasoning are strictly post-1.0 extensions. |
| Task Scheduling | Risk of separate AI task runner / thread pool | **Unified in `BehaviorExecutionContext`.** Evaluates in `SystemPhase::Gameplay` fixed tick with cooperative async cancellation. |
| Blackboard Access | Generic property maps | **Typed `AIBlackboardView` with schema validation and change notifications.** |
| Hot Reload Policy | Unspecified | **Safe-point plan swap with graceful abort and fallback preservation.** |
| Save / Restore | Decision runtime state not connected to persistence | **Durable logical state captured through `IGameplayStateProvider`; transient jobs and handles are rebuilt after restore.** |

## Verification Requirements

- Cook validation rejects missing subplans, plan-kind/schema mismatches,
  dependency cycles, and nesting beyond the configured bound.
- An HSM state can invoke a Behavior Tree while Parallel branches preserve
  independent bounded execution-frame stacks.
- `AIDecisionSystem` is the only evaluator; component attachments remain inert
  bindings and cannot create private update loops.
- NPC contexts observe an empty gameplay-action snapshot, while a possessed
  entity may consume its own semantic action snapshot without raw device access.
- Save/restore round-trips compatible stable plan/node state, excludes transient
  handles, restarts asynchronous work after commit, and resets only the affected
  agent when plan migration is incompatible.

## Consequences

- **Performance**: Contiguous compiled execution plans eliminate runtime pointer chasing and per-frame heap allocations during AI evaluation.
- **Maintainability**: Editor UI changes in `imgui-node-editor` cannot break runtime decision asset schemas, binary serialization, or network replication.
- **Safety**: Strict adherence to `SystemPhase::Gameplay` and `SceneCommandBuffer` prevents race conditions, memory corruption, and non-deterministic simulation divergence.
- **Extensibility**: Custom tasks, decorators, and considerations can be authored in native C++ or scripts through standard descriptor registration macros without modifying engine core.
- **Clarity**: Future advanced planning architectures (GOAP/HTN/RL) have clear non-blocking extension seams without complicating the 1.0 release milestone.

## Utility selector implementation

`UtilitySelector` captures scalar blackboard considerations and bounded action
options. Each input is clamped to its fixed range before applying the authored
curve. The option priority multiplies weighted responses, with optional
compensation for multiple considerations. Exact score ties use ascending
stable option identity. Missing or non-finite inputs make the option ineligible.
Evaluation reserves the full consideration and option allowance against both
the agent and global budgets before reading the snapshot.

Callers can capture normalized consideration and option scores in bounded
storage. The selector returns a stable task identity. Cooked Utility graph
composition and task dispatch require the corresponding decision runtime entry
point.

## Rejected Alternatives

- **Use `imgui-node-editor` data structs directly as the asset format**: Rejected because it introduces editor UI dependencies into headless servers/runtime builds and couples visual rendering to game logic.
- **Make AI decision graphs a general-purpose visual scripting language**: Rejected because general visual scripting requires arbitrary dataflow and execution branching, whereas AI decision trees/state machines require hierarchical priority selection, reactive decorators, and periodic services.
- **Create an independent AI thread pool / task manager**: Rejected because out-of-band AI task threads violate deterministic fixed-step simulation, complicate save/load state capture, and risk race conditions with ECS components.
- **Include HTN and GOAP in the 1.0 milestone**: Rejected due to delivery risk and complexity; standard BTs, HSMs, and Utility scoring satisfy 95%+ of game AI requirements while leaving clean extension points for future planners.

## Utility graph runtime and transport migration

DecisionGraphSource now admits UtilityDecisionSource as family tag 2 alongside
the existing BT tag 0 and HSM tag 1. Existing version-one source and cooked
envelopes keep their family tags and wire fields. The Utility extension is
rejected by older readers that do not recognize its family. The normal cooker
regenerates compiled layout declarations and digests; an old cooked layout that
differs from the current measured storage is rejected by canonical admission.

Utility source declares its selector's scalar blackboard requirements and actual
provider bindings. Compilation checks every consideration against those bindings,
validates finite bounded policies and prepares one canonical task kernel per
option. The task kernels retain the shared admitted binding/schema plan and
reuse existing provider factories, continuation mailboxes, task generations,
cancellation and exactly-once cleanup. Runtime selects once per activation;
a running action keeps its selection until completion or owner cancellation.
Independent or repeated graph calls receive disjoint task slots and runners.

DecisionGraphStepInput carries borrowed agent/global Utility allowances for
closures containing Utility. Existing BT/HSM callers keep their current inputs.
Utility step admission rejects missing allowances before task callbacks or changing
trace storage. Selection reserves its full allowance before reading values.
A budget yield starts no task and can resume at a later decision tick.

DecisionStepTrace version 2 adds UtilityScore records with stable option/key
identities and optional normalized scores. The bounded caller trace retains the
existing graph digest, occurrence and blackboard revision. Raw inputs remain in
the blackboard owner. Trace capture uses the same selector and task path as an
untraced step; it cannot change action order or scheduler admission.
