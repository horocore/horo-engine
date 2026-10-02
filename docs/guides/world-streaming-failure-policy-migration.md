# World Streaming Failure Policy Migration

WST-003.12 adds `StreamingFailurePolicy.h` to the public surface owned by
`HoroWorldStreaming`. Existing public callers remain source-compatible; no other
target receives repository-wide headers or backend dependencies. The generated
per-header consumer target verifies the new header independently.

StreamingAuthorityRole owns one optional `StreamingFailureRecord` per cell for the
mounted epoch. It supplies exact policy, content/provider revisions and unscaled
monotonic time. Record only canonical Failed terminal operations after cleanup,
using the original attempt publications; reject completions for replaced attempts.
Budget pressure remains pending admission, and permanently oversized preflight
failures remain distinct from transient failures.

Retain history across demand removal/reentry and cancelled work. Evaluate eligibility
without mutation, issue a fresh queued Load with a greater generation, and atomically
publish the successor together with requeue. Obtain fresh scheduler/budget admission
before starting it. A failed admission retains the same queued retry. Supply the
issued tombstone when recording its next failure, so duplicates cannot reset or
multiply retries. `ValidateSuccess` permits release only after canonical Active residency of that exact
generation. Load success alone never resets history; activation failures in the
issued generation retain its consumed allowance. Partition teardown discards tombstones after resource retirement.

Default automatic retries use 2, 4 and 8 seconds, then quarantine. Zero allowance
means no automatic retries; positive cooldowns prohibit zero-delay storms. Permanent
causes remain quarantined. Newer content/provider publications or explicit host
authorization start a fresh attempt series, preserving the last cause while requeued.
Policy replacement is stale and must be reconciled by the owning authority without
silently dropping tombstones. Cancellation and shutdown reject new transitions;
resource draining stays in the canonical operation/scheduler authorities.

Expose `Snapshot()` attempt count, consumed allowance, cause and next retry deadline
in diagnostics. This contract neither registers services nor owns admission,
residency, provider cleanup, clocks, queues or diagnostic storage.

An issued retry that is cancelled, replaced or shut down remains charged to the
retry allowance. After its exact canonical terminal proves cleanup, an active
(resumed) authority calls `ReconcileInterruption` to retain the cause/count and
begin the next cooldown or quarantine. Duplicate acknowledgements and live
retirement snapshots are rejected. A closed authority simply retains history
until teardown; it admits no new retry work.
