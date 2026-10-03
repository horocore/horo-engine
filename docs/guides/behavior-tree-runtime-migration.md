# Behavior-tree control-flow runtime

`HoroEngine::AI` owns `BehaviorTreeRuntime.h`. Existing `BehaviorTreeAsset` and
`DecisionAssetValidation.h` declarations remain unchanged. Consumers first use
`DecisionAssetCompiler` for descriptor/schema admission, then compile typed
`BehaviorTreeExecutionNode` and service attachments against that immutable plan.
Every admitted node must be accounted for exactly once. Child-vector order is
semantic priority; source-array order and display labels never select execution.
The generated public-header consumer covers the new header through its narrow
HoroAI owner; no target dependency or repository-wide include root is added.

The scene owner creates `BehaviorTreeInstance` and transfers an executor into
it. The owner reserves `HardNodes` consecutive task slots exclusively until that
instance is destroyed, and supplies an exact agent generation, scene token and
blackboard instance/schema publication binding. Snapshot identity/version alone
cannot admit a different blackboard generation. Schema-publication changes
require staged blackboard/instance recreation; Replace rejects them while
preserving the active tree.
Each start uses a fresh nonwrapping task generation, including after loops,
restart and replacement. Provider adapters receive detached `AiTaskOperationContext`
and a borrowed frozen blackboard snapshot with resolved bindings. They must not
retain the borrowed context or reenter the instance. A scene/gameplay adapter may
project its already authorized behavior capabilities into those provider calls;
the core never discovers Scene, jobs, native backends or editor services.

`AIDecisionSystem` remains the scheduling authority in `AiDecisionEvaluate`.
The core owns bounded task mailboxes, but no update loop or worker threads. Call
`Evaluate` with a positive nondecreasing fixed tick; event resumes may use the
same tick. Successful evaluation allocates no core storage. Node state, service
state and depth-limited scratch frames are contiguous arrays allocated before
activation. Traversal is iterative and takes at most twice the admitted control
node count in traversal steps. Pure priority checks and attachments are also
bounded by the admitted node count. Each Loop completes at most one iteration
per evaluation, so even the maximum finite repeat count cannot monopolize a tick.
Structural depth and total control/service count are capped at 1024 and may be
lowered by the project.

Sequence and Selector remember the running child. Parallel steps every branch
in declared order before applying its explicit policy. `RequireOneSuccess`
succeeds after any success and fails after all branches fail. `RequireAllSuccess`
waits for all branches and fails if any failed. `RequireAllComplete` succeeds
after all branches complete, even with failures. `StopOthersOnFailure` fails
after any failure and otherwise succeeds when all succeed. Any cancelled child
propagates cancellation. Terminal parallel outcomes abort remaining running
descendants. Root terminal results remain observable until explicit restart.

Inverter flips success/failure and preserves running/cancelled. Cooldown starts
at child completion and survives branch resets; attempts during the cooldown
fail without starting the child. TimeLimit fails and cancels its descendants
when elapsed fixed ticks reach the duration. Loop repeats successful children,
yielding between iterations, and propagates failure/cancellation. BlackboardCheck
uses the host's pure typed predicate over frozen bindings. `None` latches its
entry result; `Self` checks active descendants on every evaluation;
`LowerPriority` observes a failed higher-priority branch while a later Selector
child runs; `Both` combines these. Lower-priority observers must be direct
Selector children, enforced by compilation. Conditions are evaluated at most
once per evaluation. Priority changes abort the old branch before starting the
new branch.

Services run on activation and while their owner is visited. Periodic service
intervals use fixed ticks, without catch-up bursts; reactive services wake on
committed blackboard revision changes. Services stage writes for the next
BlackboardSync, and cannot alter the snapshot used by this evaluation. The
service hook does not own long-lived work; asynchronous operations belong to
task adapters. Dormant/terminal subtrees do not run services.

Every started task uses the existing `AiTaskLifecycle` terminal and cleanup
authority. A provider error becomes Failed with the original typed cause.
Aborting a subtree publishes cancellation for every running owned task, calls
the executor's downstream Abort once, then Cleanup once. Cleanup observes the
immutable terminal result. Completed tasks are never cancelled again. Provider
condition/service errors abort remaining running work and return the cause.
Malformed evaluation inputs preserve state and return typed failures. Agent
retirement and scene cancellation cancel before any later provider call and
close the instance. Explicit Shutdown and destruction cancel remaining work
with OwnerShutdown. Adapters must fence detached worker outcomes with their
operation handle. Instance-issued continuations now perform that fencing at the
actual task slot; providers must use this path instead of retaining an instance
pointer or directly publishing a second terminal store.

Replacement is a safe-point operation. Null/failed compilations preserve the
active instance. Exact stable executable semantics, provider versions, binding
records and schema publication retain task and timer state. Other replacements
stage all storage first, then abort the old tree and restart from the new root.
Task generations survive replacement. There is no partial activation on storage
failure. Cross-plan dependencies are explicitly rejected by this compiler until
the dependency-frame runner is composed; they are never silently ignored or
flattened into competing shared declarations. Save/restore and concrete
navigation/animation task adapters remain their own later integration work.

## Async task delivery and migration

`AITaskContinuation.h` and `AITaskJobService.h` belong to `HoroEngine::AI`, which
still depends only on Foundation. `BehaviorTreeEvaluationContext` now supplies
a copyable `continuation` for Start/Resume, and `IBehaviorTreeExecutor` adds an
optional `Pump()` hook. Rebuild all executor implementations and their consumers:
this changes the C++ context layout and executor vtable. Existing synchronous
implementations retain their Start/Resume return behavior and inherit an empty
Pump. This is a host C++ integration contract, not a gameplay SDK or extension
ABI capability. Conditions and services receive an empty continuation.

Detached callbacks may retain only the continuation and their owned inputs,
never the borrowed evaluation context, blackboard snapshot, executor or mutable
Scene. `PublishTerminal` validates the canonical success/failure/cancellation
shape and moves one candidate only when accepted. It preserves original typed
failure information. `NotifyEvent` coalesces wakes. Both use one preallocated
terminal record and event bit per task slot, with nonblocking try-lock admission:
Contended requires a later retry; AlreadyPublished and Stale are final rejection
dispositions. A candidate does not complete a lifecycle or advance the tree.

The owner calls Evaluate in `AiDecisionEvaluate`. After validating the scene,
agent, frozen blackboard and clock, it invokes Pump, observes reactive priority
and deadlines, and consumes candidates in declared traversal order. Only that
phase can publish the immutable lifecycle terminal and advance Sequence,
Selector, Parallel or decorators. Event wakes select Event for the next provider
resume; recurring evaluations retain FixedTick. A queued completion loses to
an owner abort or expired deadline before consumption. Timeout uses TimedOut;
restart/priority replacement uses Superseded; incompatible reload uses
PlanReplaced. Existing cancellation enum values retain their numeric identity.
Completion invalidates its continuation before Cleanup. Abort invalidates it
before downstream Abort and Cleanup, each once. Later task starts, loops and
replacement reuse slots with fresh nonwrapping generations. Compatible reload
retains the active continuation. Tokens surviving destruction retain inert
mailbox storage and reject publication; they retain no executable callback.

The explicit `AiTaskJobService` connects the same path to the process
Foundation JobSystem. The composition root injects the scheduler; a task adapter
admits owned `IAiTaskJob` work with an exact `AiTaskJobLease`, calls Pump from its
executor hook, and forwards Abort to Cancel. Start returns typed scheduler
admission failures. Pump maps immutable job terminals into canonical candidates
without waiting and retains candidates on mailbox contention. Service capacity
is finite and project-lowerable (maximum 1024); cancelled running work consumes
capacity until its worker reaches terminal. Admission-time scheduler allocation
is separate from the allocation-free tree evaluation path for direct events and
completions. Async availability timing is not promised to be replay deterministic.

Workers capture detached operation identity, cooperative cancellation ancestry,
immutable work and the required code-image/callback pin. The pin must cover work
destruction and all activation-scoped dependencies; work is destroyed first.
For module callbacks, retain the admitted Foundation ModuleCallbackLease in that
pin. Service Shutdown closes admission and requests cancellation without waiting;
workers may outlive both service and decision instance. The host keeps the
JobSystem alive and drains callback/image leases at teardown before releasing
dependencies or unloading code. Results never authorize worker Scene writes;
navigation, animation and script adapters must stage their own authorized owner
commands. Their concrete adapters, aggregate scheduling and save/restore remain
separate deliveries.
