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
