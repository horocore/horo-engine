# World Streaming Fair Queue Integration

The existing numerical `StreamingPriorityPolicy` and scheduler admission contracts
remain unchanged. `StreamingFairQueue` adds one public header owned by
`HoroWorldStreaming`; existing callers need no migration. The generated public-header
consumer target validates the new boundary.

A host composes the queue on `StreamingAuthorityRole`, issuing a non-reused queue ID
for one mounted partition epoch and immutable priority publication. Create reserves
all storage and validates the queue ceiling against the policy's candidate ceiling.
Moved-from queue values may only be destroyed or assigned, as with other owning
lifecycle values. No queue commands run concurrently.

For each command capture `queue.Revision()` in a context with the queue's ID and
monotonic unscaled service milliseconds. Enqueue exact pending operation handles
and source descriptors; supplied enqueue timestamps are overwritten by the owner.
Only one pending operation per cell and one stable operation ID may coexist.
`Replace` accepts a different operation ID and strictly newer generation for the
same cell and retains its wait order. Source/current demand validation stays with
the authority; source evidence must be refreshed through successor attempt replacement
or withdrawal before dispatch when current demand is invalidated.

Before selecting, evaluate every pending operation against current required-content,
source-freshness and budget constraints. Supply exactly one Admissible/Deferred row
for each exact operation in any order, fenced by the current queue revision.
Selection publishes a new revision and retains work. At the same authority safe
point, try the scheduler's atomic reservation; on success call `CommitDispatch`
with the returned proposal and new revision. On reservation failure retain queued
work; a fresh selection supersedes the proposal without spending fairness credit.
Do not admit a proposal after an intervening queue mutation. A budget/demand change
must likewise trigger a new selection (including an all-deferred snapshot) before
any commit. The queue intentionally owns neither budget samples nor reservations.

Use `Discard` for queued Cancelled, Failed or Replaced outcomes. Once dispatched,
route those outcomes to the canonical scheduler operation lifecycle instead.
`Shutdown` revokes proposals and clears unadmitted metadata only. Drain admitted
work through the scheduler's exact retirement acknowledgements. For world or
policy replacement close the old queue and compose a new non-reused owner lifetime;
old revisions, proposals and operation generations cannot route into the successor.

Every burst of at most `maximumPriorityDispatches` score-first successful commits
is followed by oldest-admissible dispatch. Stable wait order makes service bounded
for continuously admissible pending cells despite new urgent arrivals. This
cannot ensure real-time progress while budgets, pins or required content deny work.
