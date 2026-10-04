# Save telemetry host integration

HORO-1489 adds one Runtime-owned public header, `SaveTelemetry.h`, to the
`HoroRuntime` public ownership registry and generated consumer coverage.
The headless executable gains the explicit `horo-engine -> HoroRuntime` composition
edge and its inert host descriptor inventory adds Runtime. Application and Foundation
remain Runtime-neutral; policy regression rejects reverse edges from both. Hosts needing the Save
contribution explicitly link Runtime and create its registration after process
observability startup. Join or retire all Save jobs, adapters and transactions
before destroying the registration, then shut observability down. Registration
is serialized host composition and is not an ambient service lookup.

The editor and headless product hosts compose the capture-free
`SummarizeSaveTelemetry` function in `HostObservabilityConfiguration::summaries`.
Existing configurations default to no contributions. Providers run only for
explicit bundle export after log flush, receive retained host log paths and must
return a bounded privacy-reviewed metadata pair. No captured references or
producer state may cross this lifetime boundary.

`CaptureOperationIdentity` captures only numeric lineage with no ambient field
copying; `CaptureOperationContext` retains its previous full snapshot semantics.
`LogContextSnapshot::Isolated` is additive. Existing constructors retain merging;
existing callers need no migration. Privacy-reviewed operations may opt into an
isolated owned snapshot. Derivation, job submission and platform request retention
preserve its boundary and nested RAII restores outer contexts. Inner callers must
still classify their own fields. Save supplies only numeric operation lineage.

Do not use counters, logs or summaries to decide publication, retries or success.
Keep existing typed Results and operation snapshots as authority. Sync hooks are
contracts for the future application/profile coordinator, not proof of a live
cloud transfer implementation. No archive format or migration schema changes.

## Sonar correction compatibility

`SaveStageObservation::Complete`, `Fail` and `ObserveSaveStage` borrow scalar
`SaveTelemetryEvidence` by const reference for the duration of the synchronous
call. Existing value, designated-initializer and default-argument callers remain
source compatible; recompile consumers for the updated member signatures.
`Fail` projects its failure category into a local copy, preserving caller evidence.
The registration constructor uses a private explicit construction key so the
factory can use `std::make_unique`; hosts still create owners only through `Create`.
The Runtime header ownership and dependency graph remain unchanged.

Exceptions during optional observation are reported through the existing bounded,
allocation-free emergency logger using fixed text only. No exception text or
ambient private context is included, and the authoritative Save result is retained.
