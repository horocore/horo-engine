# Bounded job queue admission

`HORO-1787`, GitHub #1831 (`JOB-001.4`), extends Foundation admission without
changing ownership of jobs or user-facing operations. The ticket remains M4;
its queue contract unlocks dependent M3 schedulers.

Existing `JobDescriptor` submissions default to Normal priority and Required work. Existing
`JobSystemConfig` initializers retain their global queue bound, terminal-record
bound and worker count. Normal-only dispatch remains FIFO. Newly appended fields
are source-compatible with existing designated initializers; consumers must be
rebuilt because the public C++ structures have changed layout.

`priorityQueues` is ordered Interactive, Normal, Background. Each optional class
capacity defaults to `maxQueuedJobs`; the global limit applies independently,
so a class capacity larger than the global limit cannot increase admission.
Zero capacity disables a class. Queue-count arithmetic remains representable:
under the scheduler mutex every enqueue first proves total queued count is less
than the global `size_t` bound, and adds exactly one record. Dispatch uses a fixed
4:2:1 cycle with FIFO inside each class. Concurrent submissions are linearized
by the admission mutex; identical linearized submissions and dequeue pressure
have identical policy decisions and dispatch order. Worker completion order is
not promised to be deterministic.

Reject returns `job.queue_full`. Shed returns `job.queue_shed` only for incoming
work explicitly marked `.requirement = JobRequirement::Optional`; required work
at capacity still returns `job.queue_full`. Priority and Shed configuration never
declare an entire class optional. Optionality does not bypass capacity or change
Reject/Block behavior. Shed counters count only optional shedding; both outcomes
count as rejections. Neither outcome replaces accepted work or consumes a job
identity or creates a record. Block requires a positive timeout and a
host-installed `JobProducerScope(NonCritical)`. Unknown, main/editor, render,
transport, worker and I/O roles cannot block; nested scopes cannot widen a
restriction. Scheduler callbacks and helping-wait callbacks cannot grant
themselves blocking rights. Host compositions install scopes only on dedicated
non-critical producers, with all dependencies able to progress independently.

Waiting producers have a separate global `maxWaitingProducers` bound and FIFO
admission order per class. No-worker or disabled-capacity waits return
`job.wait_capacity_deadlock`; excess waiters return `job.queue_full`. At a waiter
synchronization point the decision precedence is shutdown, cancellation,
deadline, then available capacity. A cancelled waiting submission returns
`job.cancelled` without a record; an already-cancelled immediate submission
retains the existing accepted-terminal-cancellation behavior. Timeout returns
`job.wait_timed_out` without extending its deadline.

Parent cancellation, queued-record cancellation, helping claims, worker dequeue
and shutdown notify producer waiters. Cancellation ancestry holds only scoped,
deduplicated weak scheduler notification targets; the last waiter removes each
registration. Notification runs no user callback. A 5 ms bounded predicate
recheck closes cancellation's notify-before-wait race. Shutdown wakes admission
waiters before joining workers. Ordered resource shutdown remains JOB-001.5.

`AdmissionSnapshot()` returns fixed-size class depths and rejection, shed and
timeout counters. `job.queue_overload` events carry priority, policy and typed
outcome with submitter diagnostic context, outside scheduler locks, through the
existing bounded observability runtime. Disabled or saturated telemetry cannot
change admission outcomes. Queue state and accepted records remain authoritative.

Callbacks are initialized before records become visible to store observers,
cancellation or helping execution. Publication allocation failures roll back
retention insertion without consuming an identity. The unpublished local record
outlives the scheduler lock, so its callback is released outside that lock even
when publication throws. Allocation exceptions propagate; this is not a
`noexcept` submission contract. Cancellation state is private and accessed only
by its owning source/token contracts.

Public-header ownership stays with `HoroFoundation`; no target dependencies or
ownership entries change. `HoroJobAdmissionPublicHeaderConsumer` checks external
consumption of the extended header through its staged target boundary. The
job tests cover per-class/global saturation, disabled capacities, weighted FIFO,
role enforcement, cancellation ancestry, shutdown, waiter caps and telemetry.

Test thread ownership uses `OwnedJobTestThread.h`, which selects `std::jthread`
only when the standard library advertises `__cpp_lib_jthread`. Libraries without
that feature retain explicitly joined `std::thread` ownership; neither branch
detaches work or changes production thread policy. The excluded-from-ALL
`HoroJobAdmissionThreadFallbackTests` diagnostic target forces that fallback and
runs the same admission regressions. It is explicitly built/run when validating
portability and is not a replacement for hosted macOS compiler/runtime evidence.
