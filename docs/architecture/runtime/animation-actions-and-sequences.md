# Action playback, slots and animation sequences

HORO-469 / ANI-002.8 adds gameplay animation actions to the existing graph
compiler and `AnimationGraphInstance`. Cinematic sequences retain their separate
clock/authority boundary. Clip notifies remain ANI-002.9; action completions below
are actual request lifecycle results, not manufactured clip events.

## Immutable authoring and migration

Graph schema 6 / contract 1.4 adds a graph-local action catalog and typed named
slots. Existing schemas 1..5 require explicit migration and retain their existing
pose/state/blend behavior. Older envelopes cannot contain action slot opcodes or
catalog fields. Current canonical source includes empty action arrays even when
unused. IDs remain authoritative; bounded ASCII names are tool/binding metadata.

`GraphActionSlotNode` reads a connected base pose and writes a composed result.
Slot IDs are instance-wide, including reusable definitions. Multiple occurrences
of the same slot compose the same action sample; they never advance the action
again. Distinct slots own distinct queues and actual original clip players.
Actions continue on each real graph tick even if their slot output is masked or
unvisited. The underlying graph also advances. There is no inferred pose-weight
clock suspension or runtime callback.

An action definition binds one slot, a named section sequence, a sparse bone
mask, blend-in/out durations, shared blend curve, interruption policy and explicit
reverse eligibility. Absolute clips are admitted through the same immutable
clip/skeleton generation set as ordinary graph players. Additive clips are
rejected for this action opcode; existing explicit additive pose layers remain
available around it. Missing tracks use explicit skeleton-reference or
preserve-base composition policy.

Each section names one actual clip interval `[begin,end]` and its sole ordinary
successor. Successor links are acyclic. Explicit nonoverlapping loop regions
alone authorize repetition from their last section back to their first; the last
section's ordinary successor is the sole exit authority. A region is finite or
until-release. Finite repetitions count the initial pass, including an explicit
start inside that region. Intro/loop/outro and multi-clip sequences retain exact
leftover tick time across every section/loop/queue handoff. Release-loop intent
exits at the current or next region's final boundary. It never seeks the cursor.

## One canonical traversal and transaction

`AnimationClipAsset::TraverseRegionPrepared` delegates to the same checked
traversal kernel as whole-clip `TraversePrepared`. Its cursor is region-relative;
its sample time is actual clip-local time. Region bounds are validated before
subtraction/offset arithmetic. A Loop clip can therefore supply a one-shot region
without changing its immutable descriptor or placing a whole-clip Loop cursor at
an invalid phase equal to its duration. Terminal and held samples retain the
actual region endpoint. Admission/cancellation/shutdown precedence, reverse,
PingPong, cursor validation, overflow and crossing limits use the same provider.

Each slot uses an existing `GraphPlayerState`, fixed-point rate residual and the
actual `GraphEvaluationInput` interval. It scales once per slot and splits that
exact directed interval across regional traversal and envelope work. Zero-rate
pause creates no second clock. Reverse is explicit per action; unsupported
reverse rejects the whole attempted tick. Whole-graph abort/cancelled Commit or
boundary/transition budget failure leaves all committed action/player/queue state
unchanged. No partial advance or dropped leftover time is published.

Blend-in progresses alongside consumed action travel. Natural completion holds
the real terminal sample during authored blend-out, then returns to the current
advancing base. Immediate interruption may retain the old committed overlay and
its exact mask while the incoming action blends in. It does not freeze unrelated
base joints or restore a stale whole-body snapshot. Section-boundary interruption
waits for the actual boundary and passes leftover time to the new action. All
pose arithmetic uses the existing mask-aware `ComposePose`; all envelopes use the
same curve evaluator as state transitions.

## Commands, results and lifetime

`QueueAction` reserves a bounded request record before issuing a nonwrapping
instance-generation handle. Global retained-record capacity and pending-command
processing capacity reject before issue. Per-slot FIFO capacity, IfIdle and
interruption conflicts produce exact retained Rejected results at the accepting
Commit rather than silently dropping work. Owner cancellation remains available
even for an ordinarily uninterruptible action.

Play, cancel and release-loop commands are owner-thread intents captured before
evaluation. They survive Abort and cancelled Commit, and are consumed only at a
successful graph Commit. Calls during a pending candidate fail deterministically.
Cancellation is explicit Cut or authored bounded fade-out; a captured request
cancellation token uses the same owner path. There are no completion callbacks or
reentrant gameplay calls.

`QueryAction` reads one exact registered request. Completed, Cancelled,
Interrupted and Rejected terminal records remain charged and stable until
`ReleaseActionResult`. Unknown, released and old-generation handles are stale;
absence never means completion. Retained terminal records are part of the cold
bounded request table, not an unbounded history or overflow-prone notify queue.

`CopyActionSlots` and `CopyActionCompletions` use the same exact current/previous
resident PoseHandle generation as pose/parameter/state outputs. Completion copies
contain only requests that became terminal in that actual committed tick. Copies
perform no consumption, and releasing a live owner result does not mutate an old
publication. Successful instance replacement revokes old handles and snapshots;
failed replacement keeps the last-good owner state and immutable assets.

Shutdown is an explicit owner operation: it closes commands and terminally marks
outstanding records Shutdown, without changing old immutable pose publications.
Those terminal owner facts remain queryable/releasable. It does not pretend that
a simulation tick or a clip notify occurred during shutdown.

## Bounds and evidence

Cold preparation admits the entire catalog, immutable clip keys/generations,
resolved section/loop links, dense masks/coverage, original per-slot players,
request/FIFO metadata, overlay/frozen poses and double-buffered publications.
Defaults admit 64 retained requests, 16 queued per slot and 128 transitions per
tick; independent implementation ceilings are 32 slots, 256 actions, 4096 sections,
256 loop regions, 256 retained requests and 4096 transitions. Caller policies can
lower these bounds. Both aggregate action crossings and explicit transition work
are checked atomically. Pending intent admission cannot exceed the configured
processing capacity and strand its own requests.

Commands, Evaluate, Commit, Abort, queries/copies, result release and Shutdown
perform no dynamic allocation or blocking I/O. The accounting is logical retained
payload under existing instance storage policy, not allocator/driver/RSS claims.

Focused tests use actual skeletons, Loop clip assets, graph compilation and graph
instances for one-shots, multiple slot occurrences/slots, masks, FIFO leftover
time, finite/released loops, interruption, fades, reverse, request cancellation,
abort/retry, capacity/retention, source migration, reload/shutdown and allocation
counters. Qualification is reported separately; authored tests are not an execution
receipt. This scope adds no editor authoring UI, clip-notify dispatch, root-motion
consumer, cinematic ownership or native GPU animation claim.
