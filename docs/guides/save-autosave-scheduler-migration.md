# Runtime autosave scheduler integration (HORO-1449)

`HoroEngine::Runtime` owns the additive `SaveAutosaveScheduler.h` contract.
Existing save callers keep their signatures. The scheduler borrows the session's
existing `SaveOperationArbiter` and `SaveCaptureBarrier`; it introduces no worker,
operation store, storage authority, backend, persisted format or editor dependency.
The generated Runtime header consumers and `HoroSaveAutosavePublicHeaderConsumer`
compile the contract through its sole registered target and declared dependencies.

The arbiter additionally exposes `PollCancellation` so a waiting capture can
observe caller, parent and deadline cancellation through the existing producer,
preserving its winning reason. Existing `ObserveCancellation` callers retain their
contract. Hosts may call the new poll only while they own the active work and
before invoking canonical capture adapters.

Barrier cancellation now makes the exact matched request terminal before sampling
elapsed time. If the host clock throws, `Cancel` returns the typed timing failure
with state Cancelled and the last successful elapsed sample; consumers must still
acknowledge that terminal request. The scheduler retires its barrier and producer
on this path before returning the error, so destruction cannot orphan a pending
request under a persistently failing clock. Explicit shutdown preserves the error
and exact cancelled barrier evidence; no successful timing sample is invented and
unrelated queued/manual operations and mutation tickets remain intact.

## Host wiring

Construct one scheduler on the owner thread after composing the existing arbiter,
barrier, canonical registry and four barrier domains. Supply a valid runtime/scene/
registry generation and both absolute clock baselines. Keep those authorities
alive until the scheduler is closed/destroyed and detached operations have retired.

Sample **cumulative committed gameplay nanoseconds**, or explicitly approve the
monotonic real-time domain. Do not sum rounded per-frame floating-point deltas.
Both clocks must remain nonnegative and nondecreasing; a backward/invalid/stale
sample rejects atomically without moving either baseline. The host must handle
that error or explicitly replace the session baseline. There is no floating-point
conversion API, so NaN/infinity cannot enter cadence state.

At an activity transition, sample the boundary with the new activity. The elapsed
interval closes under the previous activity, then the new policy takes effect.
Pause, loading and inactive periods each explicitly freeze or accumulate the
selected clock; defaults freeze. Accumulation can create pending intent, but only
Active admits capture. Cooldown and pending age use the same eligible selected
clock. Hosts must publish transition boundaries instead of retrospectively
changing the activity of a whole interval.

Inside actual `CommitDeferredLifecycleChanges`, after structural writes commit,
call `CommitAtSafePoint` with current provenance/registry and a fresh application
`OperationId` descriptor plus the current typed autosave slot chosen by catalog
policy. The descriptor is consumed only at a new admission. Manual/queued work
and any busy barrier take precedence. No timer-time snapshot is retained:
capture is requested and polled through the real barrier, using the latest
coherent epoch. A handoff contains an existing operation handle and sealed
immutable canonical snapshot. Submit only that detached snapshot to the existing
worker pipeline; advance/fail/complete the same arbiter operation there, then
acknowledge terminal arbiter records under the host's retention policy.

## Cadence and bounded behavior

The first deadline is interval plus `seed % (jitterNanoseconds + 1)`. Jitter is
one deterministic phase offset within the configured inclusive bound; subsequent
deadlines are interval apart. Zero jitter disables spreading. Polling/admission
delays never reset the grid. Integer quotient/remainder coalesces missed periods
in constant work without looping or catching up saves. At most one latest-state
pending intent exists independently of the one admitted operation. Trigger counts
and age saturate; interval+jitter is checked for overflow before construction.
Long suspension does not grow state or enqueue a storm.

Cooldown starts at admission (or a rejected admission attempt). Pending intent
survives busy/manual work and future timer triggers; diagnostics expose its age,
coalesced count, cooldown, last operation and exact barrier timing/denial evidence.
Capture defer/failure and detached operation failure block further automatic
admission while retaining a visible latest-state intent and the original typed
operation cause. `Resume` is an explicit host recovery decision; it does not
authorize blind retry of an Unknown storage publication. No automatic unbounded
retry or storage failure reclassification is implemented.

`Cancel` reports Cancelled and clears intent explicitly. Session replacement
cancels old pre-commit work, clears the old intent, reports cancellation and resets
cadence for the new generation. Old detached handles remain producer/host-owned;
their terminal outcomes cannot update new-session scheduler state. Shutdown closes
scheduling without waiting or closing shared authorities. Cancellation past the
commit gate remains TooLate in the original operation, whose actual outcome must
still be published. Capture/terminal callbacks cannot reenter scheduler mutation.

## Product scope

This implements the complete time-based scheduler goal and cadence/activity/bounded
state acceptance criteria, including real arbiter/barrier immutable capture and a
headless lifecycle integration regression. `RuntimeSaveService` is still schematic
in the repository, as documented by save foundation qualification. Concrete
production storage/worker host composition, automated storage retries/backpressure
(HORO-1458), autosave catalog ring selection/rotation (HORO-1461), and product
suspend/quit orchestration (HORO-1456) remain their owning deliveries. Rotation must
advance only on confirmed durable success. The scheduler does not change those
architecture obligations or claim to enable production saves by itself.
