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
This core owns no update loop, threads or worker completion queue. Call
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
operation handle; broader async completion remains separate from this
control-flow delivery (#1339).

Replacement is a safe-point operation. Null/failed compilations preserve the
active instance. Exact stable executable semantics, provider versions, binding
records and schema publication retain task and timer state. Other replacements
stage all storage first, then abort the old tree and restart from the new root.
Task generations survive replacement. There is no partial activation on storage
failure. Cross-plan dependencies are explicitly rejected by this compiler until
the dependency-frame runner is composed; they are never silently ignored or
flattened into competing shared declarations. Save/restore and concrete
navigation/animation task adapters remain their own later integration work.
