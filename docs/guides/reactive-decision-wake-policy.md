# Reactive decision wake policy

`Horo/AI/DecisionWakePolicy.h` is an owner-thread admission kernel for one compiled
decision graph. It belongs to `HoroAI` and introduces no new target dependency.
It works with the existing immutable `DecisionAssetPlan`, blackboard observer
registry and generation-fenced task vocabulary. It does not create an evaluator,
thread pool or alternate gameplay clock.

Create the policy at `BlackboardSync`, after admitting the plan and blackboard
publication. The policy retains the plan, but borrows the blackboard. Reserve an
observer-owner task handle for the full graph lifetime; do not reuse a provider
task's shorter lifetime. Distinct compiled keys receive one observer each, with
precomputed node edges. Unrelated and unchanged writes cause no wake. Callback
contexts remain stable until policy close/destruction, which must occur on the
owner outside blackboard notification publication. The blackboard must outlive
the policy, or be released only after `CloseAtBlackboardSync` succeeds.

The decision owner performs this sequence:

1. Drain bounded detached task outcomes and committed perception events on the
   simulation owner. Match the exact task execution before `NotifyTask`; bind
   each newly started execution through `BindTask`, and revoke it on retirement.
   `NotifyPerception` accepts only declared typed listener dependencies for this
   exact recipient. Listener/version/capability admission still belongs to the
   perception registry and host; this policy does not authorize event disclosure.
2. Call `BeginUpdate(tick, activeAgent)` once at the decision safe point. Empty
   requests require no graph evaluation or snapshot copy. Nonempty requests are
   stable-node-ordered causes; capture one frozen blackboard snapshot and perform
   the authorized bounded decision evaluation. The causes are not gameplay
   payloads and cannot replace the snapshot or task lifecycle terminal result.
3. Pair every nonempty admission with `FinishUpdate`, including provider-error
   paths. Do not retain its borrowed request span after finishing. Notifications
   received during evaluation remain pending for a later admission; they never
   alter the frozen batch or recursively evaluate the graph.

The default cap admits at most one graph evaluation per fixed tick. Projects may
lower/select a finite cap up to eight; reaching it preserves pending work for
the next tick. The caller must not drain the policy in an unbounded loop.
Task/perception delivery order does not change request ordering or duplicate
evaluation counts. Events coalesce by compiled node and typed cause, preserving
the fact that multiple cause families occurred without retaining event payloads.

The host lowers periodic services, tasks requiring explicit polling, and timer
deadlines into finite `DecisionWakePollingRule` records. A service rule is enabled
only while its owner subtree is active; use `SetPollingEnabled` when that state
changes. One-shot timers disable their rule after admission. Each update admits
at most `maximumPollingWakeupsPerUpdate` due rules. A cursor over stable node
order rotates across updates, so perpetually due low identities cannot starve
later rules. Missed intervals coalesce into one wake and the next deadline is
relative to the actual admitted tick; there is no catch-up loop. Deadline
overflow retires the cadence instead of wrapping simulation time.

The polling budget limits due-rule admission, not instructions executed inside a
provider. The owning decision system must enforce its provider/agent execution
budgets and honor the admitted poll set. Existing direct `BehaviorTreeInstance::Evaluate`
callers retain their explicit fixed-tick/service semantics; creating a policy
does not silently change those callers. A host opting into reactive evaluation
must declare polls for tasks that require ticks and timers that can otherwise
make progress without input changes. An event-only task remains asleep until a
bound event arrives. Periodic services remain subject to their existing active
subtree and provider contracts.

Pause stops advancement by withholding newer simulation ticks. Repeated ticks
do not replenish the cap; backward ticks fail. Cancellation or a replaced,
retired or torn-down blackboard generation rejects admission and removes owned
observers. Close is idempotent and does not mutate unrelated registrations.
Plan replacement creates a detached replacement policy and closes the old one
at the safe point before releasing its borrowed context. The host still owns
task cancellation, outcome drain/join and downstream resource retirement.

Idle admission checks the lifetime fence, tick cap, one pending flag and one
cached earliest polling deadline. It does not scan node/observer/poll arrays or
allocate. Due/event processing scans only fixed admitted storage. These are
algorithmic bounds; no wall-clock speedup is claimed without measurements.
The `HoroAITests` regression suite includes ordinary/aligned C++ allocation
measurement for idle and due admission, fairness, feedback caps, exact-generation
events, observer rollback/lifetime, and a host-driven real behavior-tree example.

`BlackboardInstance::Binding` is a narrow owner-thread read of the already
existing publication fence. It avoids allocating a snapshot just to reject a
stale or sleeping graph; it does not expose values or mutation. Both headers are
owned by `HoroAI` and covered by its generated public-header consumer. Existing
callers and dependency directions remain valid.
