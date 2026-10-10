# Parallel render work and command encoding (RND-009.8)

The host still owns the process `JobSystem`. `RenderFrameScope` adds bounded
asynchronous preparation without transferring frontend, registry, executor or
submission authority to a worker. Existing synchronous `Execute` and
`ExecuteGraph` callers remain valid; no automatic parallel mode or backend
fallback is introduced. Consumers rebuild for the appended backend virtual
methods and frame-scope state. `RenderParallelWork.h` belongs to `HoroRenderApi`;
the frontend and native implementations depend on that contract, not each other.

Both preparation methods borrow `const JobSystem&`, matching the scheduler's
existing const `SubmitResult` facade. This does not make submission pure: the
host-owned shared scheduler state still admits and executes callbacks, with the
same render-owner producer role, bounds and cancellation. Ordinary callers with
mutable pools remain source-compatible; consumers must rebuild for the changed
method symbols and update explicit member-function pointer signatures. No
scheduler reference escapes preparation into worker callbacks.

`PrepareParallelExecution` copies borrowed mesh, instance, material and light
arrays into a move-only owning handoff. Containers are frozen before descriptors
are rebound, including short strings. Input/output metadata and payload are
charged before allocation: at most 4,096 passes and 64 MiB. Allocation itself is
not preemptible; payload copying checks cancellation at most every 4 KiB.
Workers validate and record disjoint slots. The owner polls durable host job
records in canonical order and executes frozen commands in that order through
the existing backend and attached executor. Neither is called from a worker.

`PrepareParallelGraphExecution` is the explicit native-worker route. The owner
resolves exact resident generations, acquires the existing graph lease and
validates the entire intact compiled graph before job admission. Success freezes
all backend payload and transfers the lease to the backend. A worker borrows no
caller graph, resource table, frontend, runtime, presentation port or registry.
Each accepted callback owns a shared recording capsule and writes one native
slot. An admission failure cancels its admitted prefix. Pending, cancellation,
failure, moved-from frame and closed generation never publish ready work.

Owner-side graph resolution is shared by synchronous execution and parallel
capture. It only checks exact registry generations into a bounded value table;
it does not acquire pins or create another publication authority. The existing
frame scope still controls admission, lease transfer, acceptance and abort.

Metal records supported single-effective-queue primary output, resident color
and buffer-copy workloads into independent command buffers on host workers.
Its existing graph admission remains the single validation policy. Compute,
ownership-transfer and unmaterialized transient routes remain explicitly typed
unsupported; OpenGL and runtimes without native-worker support report zero
capability and return typed unsupported, never acquire a worker context or
silently switch to synchronous graph execution. CPU preparation remains usable
with owner-only backends. Native resources, backing heaps and presentation
drawable parents are retained, not inferred from absence of public native types.

Only the owner accepts fully recorded slots for the active frame/session and
commits them in compiled order at `Present`, before the final owner presentation
buffer. Nothing is pre-enqueued: abandoning an unsubmitted buffer cannot strand
the queue. A stored primary output opens an owner-only load/store continuation
for the existing GUI bridge; it preserves the worker result and is submitted
after the graph buffers. A discarded primary output does not invent valid GUI
contents or a continuation. CPU job success is not GPU completion. Every worker buffer is polled
for native error/completion before its graph pins retire, even if the final
marker succeeds. Native failure closes healthy submission with the original
typed error rather than retrying or declaring completion.

The private submitted-graph queue owns the bounded native retention and retires
only at owner safe points. Adapter discovery is separate from frame execution;
the resource materializer and graph encoder share one private native heap and
identity store, with no worker access to that store.

The native envelope counts a capsule until its last worker or submitted GPU
reference drains: eight live capsules, sixteen buffers each, sixty-four retained
uploads and at most four owner buffers are below the queue's 256-buffer limit.
The editor bridge only borrows the current owner buffer and cannot independently
allocate or submit buffers on this queue. Admission reports busy instead of
waiting for capacity. Cancelled CPU leases also gate drawable acquisition so
`nextDrawable` is not entered while an abandoned CPU capsule retains a parent.

The active/closed atomic protocol has one sequentially consistent order: a
worker increments active before checking closure; the owner closes before
checking idle. Both sides cannot observe the old state and permit simultaneous
pin release/encoding. After closure, owner safe points retain cancelled pins
until already-entered encoders are idle. Future entrants return cancellation
without native access. No callback releases frontend pins or owns publication.
The noncopyable/nonmovable active-record guard decrements on every exit.

Normal frames neither join jobs nor wait for native completion. Host shutdown
cancels/drains its scheduler according to its existing policy; frontend/backend
shutdown closes submission without joining CPU jobs. Closing the native domain
prevents reuse and releases owner leases before the frontend registry disappears.
Late cancelled callbacks retain their own native resources and placement heaps.
Explicit GPU teardown remains bounded; native-only completion callbacks retain
submitted placement heaps across a teardown timeout and never access owner pins.

Headless host/backend contract tests do not qualify Metal driver execution.
The opt-in `MetalParallelRecordingSmoke.mm` checks real worker encoding, ordered
copy dependencies and cancelled payload lifetime on a compatible Metal device.
Linux cannot compile Objective-C++ Metal sources. macOS compilation and GPU smoke
results must be reported separately; no runtime or GPU pass is inferred from
compiled objects or test doubles.
