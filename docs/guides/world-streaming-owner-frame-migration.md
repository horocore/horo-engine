# World Streaming owner-frame work migration

WST-003.8 makes owner-work admission mandatory at the existing activation and
direction seams. Previously a direction poll could synchronously drain and destroy
all ready participants; several prepared cells could also publish in the same frame
without sharing any owner-work accounting. The new contract bounds both paths.

## Affected callers and ownership

`StreamingCellActivationTransaction::Commit`,
`StreamingCellDirectionOwner::CommitActivation`, and
`StreamingCellDirectionOwner::PollRetirement` now require one shared
`StreamingOwnerFrameBudget` and a monotonic elapsed-service-time sample. There is no
unbudgeted overload. Existing activation/direction tests migrate in this change;
the repository currently has no concrete Scene/provider receipt implementation.
Out-of-tree host adapters must implement the new positive immutable work-cost
methods on activation receipts and retirement participants.

The authority creates exactly one move-only budget per scheduler owner frame using
an exact typed policy revision, monotonic non-zero frame identity, work-time target
and unit-count ceiling. It passes the same budget to every current and retiring
cell, including old partition incarnations. Creating a fresh budget for each cell,
resetting it during the frame or refunding pending/error polls violates this
contract. Samples measure unscaled monotonic elapsed time from the beginning of
the owner's streaming service window, immediately before each call; advancing a
policy or frame does not change already charged resource reservations.

The budget performs no allocation or native work. A unit starts only when its
complete conservative charge and sampled elapsed time fit the target and a unit
slot remains. The time target stops new work; it does not preempt a native call
or promise measured frame timing. Zero, foreign/backwards and oversized facts
return typed errors. A mandatory indivisible oversized unit requires an explicit
host loading barrier or revised work partition, rather than perpetual silent
queueing or implicit budget bypass.

## Publication and rollback

Every activation receipt declares the positive upper bound for its no-fail
publication. Preparation checks the sum without overflow. Commit admits that
complete sum as one indivisible unit at `CommitDeferredLifecycleChanges`. Frame
exhaustion returns `OwnerFrameDeferred`, retaining all prepared receipts and the
scheduler reservation. A later frame rechecks the exact operation snapshot and
cancellation/shutdown gates before publication. No subset becomes visible.

Large activation preparation remains detached, and adapters must reduce final
publication to bounded ownership transfer. `RollbackPrepared` is immediate access
revocation and transfer to the already admitted retirement owner. It must not
synchronously destroy heavy resources. Those resources remain charged and owned
by the same dependency-ordered retirement participants until acknowledged cleanup.
This preserves existing atomic Scene publication and cancellation fencing.

## Retirement continuation

Each direction poll admits at most one participant step: idempotent begin, one
non-blocking poll and, only after exact acknowledgement, bounded adapter
destruction. Heavy destruction must be performed in resumable poll steps or on the
domain's native executor; it cannot be hidden in a destructor. An empty/pending
poll or preserved domain error still consumes its frame charge. Deferred polls
invoke no adapter callbacks. A stale acknowledgement retains the participant and
reservation.

The owner retains the next dependency index and begun flag across frames and
moves. It destroys each acknowledged adapter inside that admitted step instead
of batching all destruction at finalization. Cancellation, replacement, failure,
normal eviction and shutdown use the same continuation. Final scheduler release
occurs only after the last acknowledgement. Shutdown does not mint another budget
or bypass the work ceiling; any explicit bounded teardown drain follows the host's
separate lifecycle policy.

## Verification

`HoroWorldStreamingTests` covers shared cross-cell activation/retirement accounting,
atomic publication deferral, exact-target admission, unit/time exhaustion,
overflow, malformed/foreign facts, backwards clocks, moved budgets, pending/error
poll charges, ordered destruction, retained reservations and every interruption
outcome. `HoroWorldStreamingPublicHeaderConsumer` compiles the new owned public
header and the migrated operational call signatures through declared dependencies.
