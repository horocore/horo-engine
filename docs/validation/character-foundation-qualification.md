# Character foundation qualification

HORO-944 / #944 / [CHR-001.8] qualifies the existing public Character foundation,
not an alternative controller or native Character implementation. `HoroPhysicsTests`
executes the actual `CharacterWorld` lifecycle, retained storage, command reducer,
placement and movement publication. Existing Physics/Scene activation regressions
exercise its owning composition. No public API, backend choice or native handle is
added by this qualification.

## Coverage map

| Contract | Automated evidence |
| --- | --- |
| Invalid descriptors, foreign scene/world handles and exhausted capacities | `CharacterControllerContractsTests`, `CharacterWorldSettingsTests`, `CharacterWorldTests` |
| Slot reuse and non-wrapping retirement | Existing registry/world lifecycle cases and 128 successful structural slot reuse operations measured without allocation or deallocation |
| Bounded recovery and clear teleport admission | Existing recovery boundary/malformed-query cases and 32 composed teleports plus exact next-tick reservations |
| Canonical ordering, replacement and no replay | Existing command cases and 32 frames of four concurrent producers in both controller assignments |
| Failure and teardown | Allocation failure injected at every observed Character-world preparation allocation; each typed failure followed by a complete fresh world lifecycle; existing callback-shutdown and stale query rollback cases |
| Nonblocking admission | An actual copied-state reader holds the registry while a producer returns `RejectedBusy`; the rejected intent remains unretained and applies exactly once after owner repair |
| Coherent publication | An external reader held at the frozen-command boundary observes the old committed transform/marker; its future intent applies only in the next fixed tick |
| Physics/Scene ownership | Existing `PhysicsSceneActivationTests` in the same target, with public Character/Physics consumer builds |

All six new qualification cases augment the existing boundary, malformed, movement,
grounding, stance, diagnostic and lifecycle coverage; they do not replace it.

## Nonblocking admission correction

Admission already tried the command mutex, but then used a blocking registry
acquisition. A concurrent copied-state reader or owner mutation could therefore
block a producer despite the public nonblocking contract. Both acquisitions now
try their existing mutexes, in command-then-registry order. No mutex, atomic, spin
loop or public API is added. Failure at either acquisition returns the existing
`RejectedBusy` outcome, increments the existing atomic rejected counter once, and
retains no intent. RAII releases the command lock on every registry-busy return.
Only a caller owning both locks validates mutable controller state, appends to the
bounded command storage and updates admitted/depth counters.

Immutable request and atomic lifecycle/tick checks retain their original priority
before locking. Thus already-observed shutdown still returns `InvalidState`;
contention during a race can return `RejectedBusy`, as it could at the command
mutex already. The owner disables admission before draining commands, releases
the command lock before draining the registry, and defers teardown during a tick
or placement. Concurrent copied-state reads use the existing registry protection.
The correction changes neither owner thread authority nor these shutdown paths.

The regression uses the executable's existing one-shot allocation failure observer
during a stale-handle error constructed by the real `ControllerDescriptor` read.
That method still owns the registry when the observer releases a parked producer.
The producer must return while the reader retains the lock, with no allocation or
retained command. A five-second observer watchdog makes the old blocking behavior
fail without stranding the producer: error unwinding releases the registry, and
RAII releases test-owned waits and joins before world/probe storage retires. The
watchdog is a failure bound, not a throughput measurement. Assertions and thread
teardown occur after this observation window. If the standard library constructs
the error without allocation, this particular fault-observation seam is explicitly
reported unsupported and skipped; it is not counted as a demonstrated lock test.

## Deterministic measurement boundary

Qualification captures small, explicit settings: four controller/command slots,
64 contacts/queries, eight events/impulses/diagnostics/debug items, and 4 KiB of
scratch. Controller contact and fixed-tick work ceilings retain their validation.
The one-controller reuse/placement cases use the same bounded profile with one
controller slot. No production default is enlarged or modified.

Allocation measurements use the test executable's global ordinary/aligned C++
`new` and `delete` observer. Settings capture, world storage preparation, thread
creation, synchronization warm-up, assertions and thread teardown are outside the
steady-state windows. Success paths must observe zero additional allocations and
zero deallocations. This does not measure native allocator calls, direct `malloc`,
process RSS or operating-system synchronization storage.

Four parked producer threads each submit one unique controller/tick/sequence intent.
The main owner waits for all non-blocking attempts before advancing the tick. A
`RejectedBusy` attempt is repaired exactly once with the **same** immutable intent
after contenders have parked; accepted intents are never retried. Each frame checks
controller order and the exact resulting sequence and achieved velocity. Final
counters require 128 admitted commands, rejected count equal to the exact repair
count, queue depth four, no pending commands, and the independently calculated final
positions. Thus scheduler-selected busy counts may vary without changing admitted
intent or final state. No sleep, probabilistic busy expectation or unbounded retry
loop participates in the proof. Barriers delimit the data ownership and all threads
join before borrowed world/request storage retires.

Only standard-thread creation failures classified as resource-unavailable or
operation-not-supported skip concurrency cases, with an explicit diagnostic that
qualification did not execute. Unexpected thread errors fail. Skipped tests must be
reported separately from passed qualification; a skip is not concurrency evidence.
Functional barrier checks do not claim ThreadSanitizer or platform-wide race freedom.

## Execution scope and limits

Build `HoroPhysicsTests`, `HoroCharacterSurfacePublicHeaderConsumer`, and the owning
Physics/PhysicsSceneIntegration public consumers, then run the full anchored
`HoroPhysicsTests::` suite plus the Character public consumer. This includes the
existing recovery, capacities, ordering and malformed-input cases in the coverage map.
Record toolchain, build mode, executed case counts, skips and failure causes in the PR.
This document is a reproducible qualification recipe and does not claim that tests
have already run.

The placement probe supplies bounded clear Physics query evidence; the production
Character lifecycle and reducer consume it. Existing sweep/grounding cases cover
collision evidence reduction. This is headless contract qualification, not a private
Jolt adapter, GPU smoke, cross-platform determinism or native performance claim.
Hosted platform checks and quality gates remain independent delivery requirements.
