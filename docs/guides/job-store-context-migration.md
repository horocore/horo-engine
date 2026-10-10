# Job Store And Submission Context Migration

JOB-001.2 keeps the cancellation-only submission overloads source-compatible,
but configuration-dependent jobs should migrate to `SubmitContext` and consume
only `JobExecutionContext` inside the callback.

`SubmitResult` now accepts `const JobDescriptor &` to avoid an extra wrapper copy.
It delegates synchronously to `SubmitContext`, which owns a descriptor copy before
admission; no caller reference survives submission or is captured by queued work.
Ordinary lvalue and temporary call sites remain source-compatible. Rebuild callers;
code storing a pointer to this member function must use the new const-reference
signature. The callback is still transferred by value and retained by the scheduler.

```cpp
JobDescriptor descriptor{
    .parentCancellation = parent,
    .operationId = operationId,
    .configuration = configurationService.Snapshot(),
};

auto submitted = jobs.SubmitContext(
    std::move(descriptor),
    [](const JobExecutionContext &context) -> Result<void> {
        const ConfigurationRevision revision = context.Configuration()->Revision();
        // Process using this captured revision only.
        return context.UpdateProgress("complete", 1.0F);
    });
```

The application owns operation visibility. `operationId` is only a correlation
edge; `JobSystem` does not create an `OperationStore`, and internal jobs should
leave it empty. Structured children use `TaskGroup::SpawnContext`, which replaces
any supplied task-group identity with the group's typed ID.

Progress values are finite and normalized. Phase names are copied into a bounded
128-byte inline value, so repeated updates allocate no phase storage. They cannot
decrease within one phase; beginning a different bounded phase permits a reset.
The first terminal result is immutable. `JobHandle::Snapshot()` remains valid after bounded store
eviction, while `JobSystem::Find()` and `SnapshotIfChanged()` expose only active
and retained recent records. Dropping a handle neither cancels the callback nor
removes scheduler authority.

## Resource scheduling and host teardown (JOB-001.5)

Existing descriptors remain CPU jobs. To schedule blocking I/O, the host must
configure `JobSystemConfig::ioWorkerCount` and the caller must select
`JobDescriptor::resource = JobResource::Io`. No I/O capacity means a typed
admission rejection, rather than a CPU fallback. Recent-project inspection now
requires this explicit capacity; its host/test compositions have been updated.
`WorkerCount()` continues to report CPU capacity only. All lanes share the
existing global/priority queue bounds and terminal retention.

Set `reservedInteractiveJobs` when background saturation must leave queue room
for direct user feedback. It defaults to zero, is capped at `maxQueuedJobs`, and
does not override any priority queue limit.

Hosts call `StopAccepting()` before stopping request sources and cancelling
service scopes, then drain required owner continuations and call `Shutdown()`
while callback dependencies are alive. Do not defer the only join to scheduler
destruction after application services disappear. Shutdown also accounts for
callbacks claimed through bounded waits and releases their captures before
returning. Rebuild public-header consumers for the appended descriptor/config
fields; no installed header ownership or target dependency changes are required.

Callbacks cannot synchronously wait on another resource lane, including through
the legacy unbounded wait. Such waits and callback-owned task-group cross-lane
spawns return `job.wait_capacity_deadlock`. Existing CPU-only callers retain
same-lane structured helping. Move cross-lane pipeline joining to the external
operation owner; do not block a CPU callback waiting for I/O which may itself need
CPU capacity. Public wait contracts now state this rule explicitly, and mutual
CPU/I/O wait regressions verify both overloads and admission behavior.
