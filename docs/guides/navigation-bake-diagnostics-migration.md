# Navigation bake diagnostics composition

`HoroEngine::NavigationBakeService` now owns the public
`Horo/Application/NavigationBakeDiagnostics.h` contract. Existing hosts may omit
`NavigationBakeServiceConfig::diagnostics`. Hosts exposing retained diagnostics
must explicitly compose one journal for each project/definition binding and use
the same `BuildOutputStore` observed by their Build Output presentation.

The application target declares `HoroRuntime` because authored Scene destinations
use its existing `SceneDefinitionId` and `SceneObjectId`. No backend handles or
repository-wide include roots enter the public contract. Both the generated
public-header consumer and the linked diagnostics consumer cover this boundary.

## Host ownership and migration

1. Supply a persistent nonzero `NavigationDiagnosticProjectId`, the definition
   `AssetId`, an absolute existing project root and the shared output store.
   Project identity must survive restart; a runtime operation or Scene slot is
   unsuitable.
2. Create a Foundation `Diagnostics::OperationHistorySink` with a dedicated
   project/definition history location and bounded file/roll/recovery policy.
   Pass it to `NavigationBakeDiagnostics::Create`. Creation recovers valid
   matching checkpoints into the journal and shared output.
3. Register the journal as an additional process telemetry sink before producers
   run. For Logger composition use its `additionalSinks`; for direct telemetry
   composition include the journal in `Telemetry::Runtime::Initialize`.
   Journal creation and service configuration do not initialize ambient runtime
   state. The existing dispatcher performs persistence; bake jobs do no history
   filesystem I/O.
4. Bind that journal through `NavigationBakeServiceConfig::diagnostics`. The
   factory rejects a journal bound to another definition. Populate each request's
   optional `diagnosticSources` with captured source observations and their
   authored Scene object or asset destinations. Each observation must belong to
   the request's complete source capture.
5. Keep the output, journal, operations and scheduler alive beyond presentation
   closure. Close service admission, drain accepted jobs, flush/shut down the
   process dispatcher, then release sinks and stores. A panel snapshot never
   owns an accepted bake.

The production service records admission, actual scheduler progress, concrete
tile failures and terminal results. Stable `navigation.bake.*` codes, profile,
surface/tile and producer/contribution/revision context and remediation text are
projected directly into shared output. The original cause code remains available
as the tool code. No log parser is involved.

## Retention and recovery

Journal capacity is bounded (1–4096 records), as is the per-operation detail
limit (1–4096). The latter counts queued/progress/tile detail; terminal results
always bypass it. Each suppressed detail produces a warning summary with the
cumulative operation count. Snapshots expose suppression and overwrite counts;
accepted checkpoints retain cumulative counts across history recovery.

The Foundation history file/roll/recovery limits independently bound disk and
replay retention. Set `maxFileBytes` to accommodate checkpoint envelopes (the
decoder permits at most 8192 payload bytes); a smaller limit is an observable
sink failure. The journal reports diagnostic construction failures, persistence
submission rejection, process-wide dispatcher drops and its own history export
failures separately. Rejected delivery emits a
shared output warning. These counters describe incomplete retention rather than
changing the bake's terminal result.

Recovered records are historical evidence. They receive journal-local sequence
identities and do not enter `OperationStore`; their Build Output records have no
live operation ID or session ID. A process-local operation number cannot alias
newly accepted work. Unsupported, malformed, oversized and foreign-owner
checkpoints are ignored before output projection.

## Source navigation

Use the retained journal sequence with `Navigate`, passing the current project
and definition and a fresh authoritative source ownership map. The map must
reflect current Scene object/asset existence and exact producer, contribution,
revision, digest and authored destination. Missing, changed or ambiguous entries
are rejected before invoking the host adapter.

Paths are hints within the project root. Navigation rejects noncanonical relative
paths, traversal, drive/absolute paths, invalid UTF-8, missing files and symlinked
components, including project-root ancestors. The host-owned
`INavigationDiagnosticNavigator` adapter receives the stable authored target and
validated absolute file path, outside the journal lock. Its synchronous `noexcept`
method returns `Result<bool>`: expected navigation failures and their cause chains
pass through unchanged, while false denotes a declined action. Hosts convert
private UI/filesystem exceptions at that adapter's owning boundary before
returning. The interface's one virtual call is a tooling-only action enforcing
the exception-free application boundary; it is not frame work. Hosts perform
navigation on their owning thread and preserve source ownership through the
synchronous call. No callback or adapter reference is retained.

This replaces the initial boolean callback with a typed host adapter. The affected
consumers in this change are the production/recovery routing tests and public
header consumers. New presentation adapters implement
`INavigationDiagnosticNavigator` and return their existing normalized application
errors. UTF-8 path hints are converted through `char8_t` paths and compared as
UTF-8 on Linux, macOS and Windows.

The generic Build Output `source` field stays unset because its ordinary file
opener cannot validate navigation source identities. A navigation presentation
routes through the journal contract to its Scene selection or asset opener. This
change provides production bake evidence and validated routing; editor panel
composition and Problems presentation remain in their owning adapter workstreams.
