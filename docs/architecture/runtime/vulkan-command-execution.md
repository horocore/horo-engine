# Canonical graph submission and native command retirement

This contract records the deliberate public API migration required by
HORO-326 / #326 / [RND-006.4]. OpenGL, Metal, Vulkan and Null remain concrete
peers. A host selects and composes one backend before native initialization.

## One execution authority

`ValidateRenderGraphExecutionRequest` validates every execution request.
`RenderFrameScope::ExecuteGraph` retains its existing single effective queue
contract. `ExecuteSubmittedGraph` supplies explicit Horo-owned submission
records to the same frontend lease, workload, resource-generation and validator
path. Backends cannot silently ignore nonempty submission records. A backend
without this mode returns a typed unsupported result before native work.

The config owns the role-to-queue assignment. An explicit execution partitions
all compiled passes into bounded, ordered, nonempty ranges. Every range uses
its compiled queue, has one globally increasing admission order and one
monotonically increasing signal for that queue. Cross-queue dependencies and
ownership acquires wait on the exact producing record's signal. Queue values
are not reset between graphs in one selected device lifetime. Native Vulkan
queues and semaphore handles remain private.

The Vulkan implementation records core Vulkan 1.3 command buffers from bounded
pools and submits with `vkQueueSubmit2`. One timeline semaphore belongs to each
admitted effective queue. Distinct graphics/transfer IDs require distinct
native queue families; a missing family is typed unavailable, never an alias.
Release and acquire barriers carry matched resource, source/destination family,
and old/new layout evidence. Accepted commands retain native backing and the
frontend's one completion lease until actual terminal completion. Unsent
abandonment does not release already accepted sibling submissions.

## Imported state and contents

Synchronization and compiled execution plans own their imported state records;
moving a plan cannot leave a borrowed view into a destroyed synthesis result.
Each native import identifies the exact frontend generation and actual resident
instance. Duplicate graph identities for one native generation are rejected.

`Preserved` imports must match successfully submitted native state and known
contents. `Discarded` imports require the exact undefined logical state and a
first write. Discard removes contents preservation only: earlier native accesses,
queue ownership, submitted completion and resident leases still apply. First
read and read-write access to discarded contents fail. A repeated same-queue
discard carries the previous native memory dependency. A cross-graph queue
change without matched prior release evidence is explicitly unsupported; the
backend does not invent an external signal or layout.

The command implementation supports whole-resource buffer copies and whole,
single-sample RGBA8 offscreen color clear/load/store. It validates native usage,
identity, checked byte ranges and complete destination initialization before
recording. Primary WSI acquisition/presentation belongs to HORO-325; shader,
light-culling and VFX realization belongs to HORO-327. Unsupported workloads
fail with typed capability information. Transient placed resource realization
is not inferred from dedicated-allocation support.

## Closing and bounded observation

The former void backend `Shutdown` could report apparent completion while
accepted GPU work or cancelled CPU encoding still owned native resources.
It is replaced by one canonical pair:

- `BeginShutdown() -> Result<void>` closes admission and cancels/abandons unsent work.
- `PollDrain() -> Result<RenderBackendDrainProgress>` reports `Pending`,
  `Drained`, or the original failure without waiting.

The host owns its finite deadline and retains the frontend, native parents,
ports, GUI/viewport and modules on pending/error. It can retry observation and
drain. No timeout becomes device loss; cancellation does not undo submission.
Only terminal GPU completion/error plus retired CPU work and retained capsules
permit final release. Destruction without explicit successful drain fails
closed. Initialization and shutdown are externally serialized owner operations;
foreign-thread regression calls join while the owner lifecycle remains stable.

OpenGL retains its native context/port until final backend destruction because
host GUI teardown issues GL deletes after renderer drain. Metal's native upload,
submitted command and recording owners likewise prove terminal retirement.
Host presentation/acquisition retention integrates with the Metal presentation
workstream using this same closing authority.

`ObserveSubmission(RenderCpuWait)` observes an actually issued queue point.
Poll uses zero timeout; bounded explicit test/readback/recovery purposes may
wait, capped at 30 seconds in the native Vulkan implementation. Blocking
observation is rejected during an active frame before any native wait. Polling
remains available while closing, until Drained. Unsupported peers report typed
unavailability rather than inventing completion.

The observation request includes `expectedDeviceIncarnation`. It must be
nonzero and match the actual selected capability snapshot before native
counter/wait access. `AcquireRenderDeviceIncarnation` is the single process-wide,
non-reused producer shared by peer/device recreation. It fails at exhaustion
rather than wrapping. Existing owner-bound readback/query wait semantics remain
unchanged; their owning port still validates its own identity.

## One native-retirement lease per memory charge

The existing budget reservation remains the sole accounting authority. The five
native creation methods accept a move-only `RenderResourceNativeLifetime` for
that same charge. Native owners move it into actual backing before work can be
accepted. The same lease survives successful publication, failed creation with
accepted native work, publication rollback and resource destruction.

Registry slots are bounded and reserved before exposure. Live leased entries
never relocate, compact or move. A backend releases the lease only after actual
native backing and accepted copy/use references have retired on the owner thread.
Release only marks evidence; registry draining later cancels the original
reservation or acknowledges the original allocation exactly once. No native
callback reenters the registry or creates a competing accounting ledger.

Failure can retain a charge without a successful resource instance. Frontend
shutdown keeps the registry and budget alive while any such lease is pending.
Direct native callers with no frontend charge may pass the default empty
lifetime; they still own native dependency and drain ordering.

## Migration and evidence

Affected callers include RenderFrontend resource/upload/transient completion,
all concrete backends and direct resource implementations, backend mocks,
public consumers and native smoke owners. Replace old Shutdown calls with
explicit Begin/Poll plus finite host retention; never destroy a pending owner.
Backend implementations move the supplied lifetime into actual native ownership
instead of releasing it at creation return. Providers must reject unsupported
explicit submission records before executing any workload.

Regression source covers exact partition/wait ordering, discarded read and
read-write rejection, move-owned imports, native pool bounds, signal generation,
finite timeout recovery, foreign-thread exclusion, pending shutdown retention,
creation failure/publication rollback and stable slot reuse. Native graph tests
capture actual Vulkan API dispatch arguments and native backing lifetime.
The opt-in Linux native smoke composes the installed driver through the public
frontend, submits real copy and dynamic-rendering commands and observes its
timeline. Its provenance records source revision, installed driver/layer packages,
ICD digest, commands and JUnit. A driver without a separate transfer family skips
only that native cross-family case explicitly. Command/timeline completion does
not claim rendered-pixel readback or cross-family hardware evidence when skipped.
