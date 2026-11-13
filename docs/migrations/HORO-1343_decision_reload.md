# Decision reload reservation and safe-point contract

The existing standalone BT `Replace` path previously resized nodes and traversal
storage while retaining the original async mailbox. A plan with more task slots
could index beyond that mailbox. Replacement also lacked a fence against
evaluating both plans in the same simulation tick.

`BehaviorTreeInstanceBinding` now appends optional `availableTaskSlots`. Existing
aggregate initializers remain source compatible: absence captures the initial
plan's exact requirement at `Create`. A host that supports growth must reserve an
exclusive maximum range and supply that capacity at creation. Explicit zero
reserves no tasks. Capacity and first-slot arithmetic are checked before activation
and on replacement. Adjacent slots are never inferred to be free. Composed BT and
Utility action construction continues to capture each compiler-reserved local
range; it does not borrow capacity from siblings. Rebuild public consumers and
recook plans whose checked storage declarations contain the changed binding or
instance layout. These changes are part of the current unreleased SDK30 worktree.

Incompatible replacement stages a fresh mailbox together with all node, service
and scratch allocations, then cancels old tasks and installs the complete set.
Allocation failure leaves the previous execution and continuations valid. Removed
tasks receive PlanReplaced cancellation/cleanup once. Queued late results from
their retained old mailboxes remain stale. Compatible replacement keeps that
mailbox and live state.

Provider/node/key binding changes require the new two-argument Replace overload
with an executor prepared for the candidate. The one-argument API is preserved,
including its member-function-pointer signature, but now rejects incompatible
frozen bindings with DecisionAssetDescriptorIncompatible instead of dispatching
new nodes through the old adapter. Callback-role changes also require a fresh
adapter. Supplying a new executor always restarts provider state, even if topology
is unchanged. The old adapter remains alive through cancellation/cleanup, then
releases its captured providers before any new task starts. Preparation failure
preserves old execution. A staged-header-only public consumer covers both overloads
and the appended reservation field.

After any successful Replace, both fixed and event evaluation must use a tick
strictly greater than the last old-plan evaluation. Existing callers already
advance the simulation clock after reload. A caller that previously invoked
Replace and Evaluate again in the same tick must defer that evaluation; it receives
TaskTransitionInvalid without callbacks until the next tick. Failed replacement
does not install this fence and preserves ordinary same-tick event delivery.

Regression sources cover growing async plans under an explicit reservation,
growth rejection without spare ownership, removed-continuation retirement,
compatible/incompatible same-tick fencing, overflowing reservations and failure
in every staged allocation, including mailbox storage. Their current native pass
is pending the shared build slot. Full composed graph active-path migration,
provider-generation replacement and atomic blackboard-schema migration remain
outstanding; the standalone fixes do not satisfy the complete GAI-003.11 ticket.

Scene composition now reserves its physical task namespace through
`AiSceneRuntime::ReserveDecisionTaskSlotsAtSafePoint` and captures observations
through `CaptureDecisionSnapshotAtSafePoint`. Both require the actual Scene owner
lane and an active agent. A move-only reservation captures that agent's exact
blackboard/schema generation and a child cancellation scope. A successful range
is never returned or reused, including when graph creation fails; failed admission
does not advance the allocator. Zero-task ranges are valid. The count describes
logical indices and does not allocate count-sized storage.

The Scene overload of `DecisionGraphInstance::Create` consumes this reservation,
checks the actual schema before provider factories and retains the scope through
graph retirement. Use it instead of inventing indices from compiler offsets.
`AiTaskOperationContext` and `BehaviorTreeInstanceBinding` append an independent
owner cancellation token. Operation and module cancellation remain independent;
Scene composition installs its reservation as the owner ancestor. Rebuild users
and recook checked layouts in this unreleased SDK30 migration. Queued workers
check all ancestors before and after invocation. Running providers must check
both operation and Foundation cancellation between bounded steps; cancellation
does not forcibly stop native code, and image pins remain until work exits.

Graph cancellation on its owner lane retires existing tasks before attempting to
read an expired Scene snapshot. New regression sources exercise real Scene
reservation/restore/disable/shutdown, disjoint native task indices, failed
admission, move ownership, schema rejection before factories and BT/HSM child
cleanup. These additions remain uncompiled pending the shared build grant.
