# PCG graph document sessions and source publication

HORO-2052 adds `PCGGraphDocument` and the workspace-owned
`PCGGraphDocumentService` to `HoroEditorServices`. The existing binary
`PCGGraphAsset` codec remains the sole graph serializer. `.hpcg` routes use
`DocumentKind::PCGGraph` / `pcg_graph` and bypass the UTF-8 text owner.
Consumers must rebuild for the appended kind and updated source extension policy;
persisted keys for existing kinds remain unchanged.

The host supplies the registry `AssetId`, a unique workspace incarnation, source
path, owned node-support catalog and optional explicit migrator. Reopening an
already writable asset focuses its existing document without loading over unsaved
intent. One workspace owns at most 32 sessions; panels own presentation and query
immutable graph roots. Current GUI composition supplies an empty support catalog,
preserving unknown nodes inert until a host composes supported types. It does not
discover or activate node libraries.

Each edit carries exact session, document revision, catalog revision and mutation
capability. A transaction contains at most 256 typed operations, validates a
detached complete graph and reserves exact node/edge/input/policy semantic deltas
before publishing one root/history/revision change. Shared
`SemanticDocumentHistory` also owns Mixer retention. PCG retains at most 128 entries
/ 4 MiB; Mixer preserves its 64-entry / 8 MiB policy. Allocation, validation and
cancellation failures preserve prior state. Undo/redo never evaluates graphs.
Dirty compares authored-state digests, excluding the durable graph revision.

Save captures immutable bytes, working-state identity and exact accepted source.
`acceptedGeneration` records the latest durable generation; the working root keeps
its loaded lineage until explicit reload. Save advances durable revision only in
the captured serialization. Edits during publication survive and remain dirty;
history survives save. Reload invalidates earlier save captures and clears history
after complete replacement validation. Missing types preserve opaque payloads
through the existing `PreserveInert` contract without granting cook eligibility.

`HoroAssets` owns bounded source capture and publication. The caller retains its
real shared project mutation lease and an exact native per-source exclusive lease.
Publication rechecks accepted source before staging and before replacement,
verifies staged bytes and uses tracked native durable replacement. Detected
external changes abort without replacing source. A committed replacement followed
by sync failure returns `ReconciliationRequired`; dirty state and accepted base
remain unchanged and require explicit reconciliation. Existing staging files
reject publication until explicitly reconciled.

This is a cooperating-writer contract: native POSIX/Windows replacement offers
no content compare-and-swap against a noncooperating write after the final source
recheck. The host must exclude such writers for publication authority. No
protection against that final-gap race is claimed.

`DocumentRecoveryService` owns checksum-protected envelopes beneath
`.horo/local/recovery`: at most 64 records / 128 MiB total, 8 MiB per record.
Payloads are ordinary canonical graph bytes. Autosave never writes canonical
source or marks saved. Inspection is read-only; acceptance rechecks exact
candidate, source and live command fence and creates reversible working intent.
Explicit acceptance authorizes atomic transfer of that exact recovery slot to
the new session, preserving the old candidate until replacement commits. Late,
substituted and unrelated-session writes reject; full storage never implicitly
evicts another document. Scene recovery consumes the same publication/quota owner
and preserves its existing JSON format.

Workspace autosave and leave decisions include graph sessions. Failed save or
recovery retains ownership for retry. Close/shutdown reject lifecycle replacement
during publication and recheck revisions after cleanup. Shutdown requires recovery
covering latest dirty semantics, including edits during an earlier recovery write.
Explicit discard removes only current-session or explicitly accepted recovery and
leaves canonical source unchanged. No document-owned workers or detached callbacks
exist. `HoroEditorServices` now publicly declares Assets and PCG dependencies;
all new public headers have exactly one ownership-registry target.

Schema conversion requires a host-owned `IPCGGraphSourceMigrator`. No default
legacy converter is installed because no normative schema-1.0 field mapping
exists. Tests use a synthetic current-layout fixture to exercise the existing
migration hook and invalid-output rejection. Unsupported versions fail without
replacing roots/source; migrated candidates stay dirty until separately saved.

Coverage: `HoroPCGDocumentTests`, GUI-enabled `HoroEditorWorkspaceControllerTests`,
standalone `HoroPCGDocumentPublicHeaderConsumer`, and existing Mixer/Scene
persistence suites. GPU/manual UI checks and hosted quality gates require separate
evidence.

`ObserveExternal` borrows its source identity by const reference for the duration
of the synchronous owner-thread check. Call sites remain source compatible;
consumers must rebuild with the updated declaration. No identity or lease is
retained beyond the call. Successful publication and recovery leases remain owned
by the operation scope until its last filesystem callback completes.
