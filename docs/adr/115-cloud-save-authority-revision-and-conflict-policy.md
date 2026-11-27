# ADR-115: Cloud Save Authority, Revision and Conflict Policy

- **Status**: Proposed
- **Date**: 2026-09-02
- **Supersedes**: None
- **Scope**: Local versus cloud authority, provider revision/write preconditions, offline/startup/upload/download/delete states, lineage classification, clock treatment, conflict preservation and UI decision boundary
- **Issue**: [SAV-006.1](https://github.com/HoroCore/horo-engine/issues/1466)
- **Jira**: [HORO-1466](https://horo-engine.atlassian.net/browse/HORO-1466)
- **Related**: [ADR-008](008-error-model-exception-boundary-and-registry.md), [ADR-112](112-save-archive-container-and-compatibility-policy.md), [ADR-113](113-local-storage-user-profile-and-slot-ownership.md), [ADR-114](114-canonical-runtime-world-persistence-boundary.md)
- **Normative documents**: [Save Game And Persistence](../architecture/runtime/save-game-and-persistence.md), [Platform Services](../architecture/runtime/platform-services-architecture.md), [Platform Abstraction](../architecture/foundation/platform-abstraction.md)

## Context

Local Runtime Save produces one validated durable archive generation and remains
usable without Platform Services. Cloud backends transport opaque blobs under an
authenticated provider account, but provider capabilities differ: timestamps may be
skewed, revisions may be opaque, and some stores cannot perform conditional writes.

Unconditional upload, download-newest or timestamp-based last-write-wins can erase an
offline device's complete generation. A generic retry queue can also replay an older
upload after a newer local save. `SlotGenerationId` identifies a logical publication,
while a provider ETag/revision identifies one remote object state; neither substitutes
for the other.

Conflict UI may collect a user's choice, but a panel cannot own durable state, retain
leases or apply a provider/local write. The sync coordinator needs an explicit state
machine, persisted intent, lineage comparison and compare-and-swap boundary that
preserves recoverable archives.

## Decision

### 1. Local storage is gameplay authority; cloud is a replica

Runtime Save reads, writes, validates, migrates and publishes only through the local
ADR-113 namespace/storage authority. A successful local `PublishedDurable` result is
gameplay-save success even when cloud is disabled, unavailable, offline, signed out,
quota-limited or failed. Cloud state is reported separately and never relabels local
success as rollback/failure.

Platform Services owns authenticated provider session and opaque blob transport. It
does not parse archives, inspect gameplay state, choose a winner, mutate the local
catalog or expose local paths. `CloudSaveCoordinator`, owned by the application/profile
session, is the single sync state authority. It consumes immutable local catalog/
archive leases and the narrow platform backend.

Cloud dispatch and request snapshots project provider failures to host-owned
error descriptors. A normalized provider failure may retain its typed category.
Provider text, diagnostics and unrecognized causal details stay inside the adapter.

Game load always uses a locally published validated archive. A remote archive must be
downloaded, boundedly verified under local scope/signature/compatibility policy and
published through the local storage transaction before Runtime Save can load it.

### 2. Local generation, archive identity and provider revision are distinct

```cpp
struct ProviderObjectRevision { BoundedOpaqueBytes value; };
struct CloudSyncRecordRevision { uint64_t value; };

struct CloudReplicaHead {
    SaveAddress address;
    SlotGenerationId generation;
    OptionalSlotGenerationId parentGeneration;
    ArchiveContentHash archive;
    CanonicalStateHash state;
    ProviderObjectRevision providerRevision;
};
```

`SlotGenerationId` and parent lineage come from the signed/verified archive and identify
one logical slot publication. `ArchiveContentHash` identifies immutable archive bytes.
`ProviderObjectRevision` is an opaque provider-scoped compare-and-swap token for the
current remote object; it is not ordered, portable, gameplay-visible or stored as
archive identity. `CloudSyncRecordRevision` orders local coordinator snapshots only.

Provider modified time, local wall clock, play duration and device clock are bounded
presentation/provenance. They may help a human recognize a generation but never prove
causality, ancestry, freshness or overwrite permission.

### 3. Conflict-safe cloud capability requires conditional mutation

A backend reports one of:

| Capability | Sync behavior |
|---|---|
| `ConditionalRevision` | Read/list returns an opaque revision; write/delete require expected revision or create-if-absent. |
| `UncoordinatedBlob` | No safe remote write concurrency primitive; background upload/delete is disabled. Local saves still work; explicit export/import may be offered. |

Automatic sync is admitted only for `ConditionalRevision` and only after backend
qualification proves the precondition is enforced atomically. A read-then-
unconditional write is not compare-and-swap. Process-local mutexes or advisory leases
do not coordinate other devices. Provider last-modified time is never an expected
revision.

Writes return the new provider revision and preserve an idempotency key for retries of
that exact expected-revision/content tuple. `PreconditionFailed` is a normal signal to
refetch and reclassify, never permission to retry unconditionally.

### 4. A namespace owns a durable sync journal

After local save publication, the coordinator durably journals an upload intent
containing exact address, generation, parent, archive hash, expected provider revision
and retry identity. The intent pins or revalidates the archive before every attempt.
A later local save creates/supersedes intent explicitly; successful upload of an older
archive cannot mark the newer generation synced.

The journal also records last verified local/remote heads, provider account/session
scope, pending download/conflict/delete work, retry/backoff state and immutable typed
outcomes. It contains no credentials or raw platform handles. Profile switch/sign-out
closes scheduling for the old namespace as ADR-113 requires; queued work cannot replay
under a new provider user.

Platform Services' generic offline queue does not persist cloud archive writes or
deletes. Their payload lease, expected revision and lineage are owned by this journal.
The frontend may retry one idempotent request while the coordinator remains alive,
but only the coordinator decides durable replay.

### 5. Startup and offline behavior are explicit

Cloud sync has typed states such as Disabled, Unavailable, SignedOut, Offline,
Reconciling, InSync, UploadPending, DownloadPending, Conflict, Failed and Closing.
These are a projection of coordinator state, not slot lifecycle or Runtime Save result.

Startup opens and validates the local catalog first. Local save/load remains admitted
according to local policy without waiting for network. When cloud becomes available,
the coordinator lists/reads remote heads, validates provider user/product/profile
scope and reconciles against the journal. A host may present a bounded “checking cloud”
state before a slot-selection UX, but it cannot make local durability depend on remote
availability or silently replace local data during that window.

Offline saves advance local lineage normally and journal upload intent. Retry uses
bounded exponential backoff/jitter and visible quota/auth/permanent failure. Network
recovery triggers reconciliation, not blind FIFO upload.

### 6. Reconciliation classifies verified complete generations

For each `CloudSaveObjectKey`, the coordinator verifies remote metadata and downloads
the blob when lineage/content evidence is needed. An unverified/malformed remote is
CorruptOrUntrusted and quarantined/reported; it is not a conflict candidate or local
overwrite source.

Given verified local and remote heads:

| Relationship | Result |
|---|---|
| Same `SlotGenerationId` and `ArchiveContentHash` | InSync; refresh provider revision only |
| Same generation, different archive hash | Identity/integrity violation; quarantine, never choose automatically |
| Remote is a known ancestor of local | Upload local with expected provider revision |
| Local is a known ancestor of remote | Stage/verify and durably apply remote to local as the same logical publication |
| Neither is a known ancestor, or retained history cannot prove ancestry | Conflict; preserve both complete archives |
| One side absent with no verified tombstone/history | Unknown/Conflict; absence alone is not deletion authority |

`CanonicalStateHash` may prove two generations contain equivalent logical state, but
does not grant overwrite permission or collapse publication lineage. It may let UI
explain that the conflict is content-equivalent.

Lineage evidence is bounded. Each catalog/journal retains a configured chain of recent
generation-parent/hash records. If ancestry has aged out, the coordinator classifies
conservatively rather than inferring from timestamp or generation bytes.

### 7. Download/apply is a local storage transaction

A downloaded archive is untrusted until framing, bounds, `ArchiveContentHash`,
signature/scope, product/profile/logical-slot identity and compatibility preflight all
pass. It remains operation-owned staging. The coordinator acquires the local slot
lease and revalidates the expected local generation before atomic durable publication
and catalog update.

Replicating the same remote logical publication preserves its embedded
`SlotGenerationId`; it does not allocate a new one. Migration or import into another
slot/namespace creates a new archive/publication through Runtime Save. Failed download,
verification, local quota or atomic replace leaves the prior local generation intact.

### 8. Divergent conflicts require explicit resolution and preservation

The baseline performs no field/record/participant semantic merge and no automatic
last-write-wins. A conflict operation pins exact local generation/hash and remote
provider revision/generation/hash. Before any destructive replacement, both verified
archive byte sets are durably retained: the current local slot plus an operation-owned
conflict recovery record/blob for the other side. Signed bytes are not rewritten merely
to make a new filename.

Host policy or UI may submit one typed command:

- `KeepLocal`: retain remote in conflict recovery, then conditionally upload local;
- `KeepRemote`: retain local in conflict recovery, then publish remote locally;
- `KeepBoth`: retain both and, when scope/signing policy permits, import one through
  Runtime Save as a newly finalized slot publication; or
- `Defer`: keep Conflict state and both copies without mutation.

The UI owns only presentation/confirmation. `CloudSaveCoordinator` revalidates all
captured revisions under local lease and provider CAS, executes the command and
publishes result. Any stale/precondition failure returns to reconciliation. Closing a
dialog cannot resolve, cancel ownership unsafely or delete a copy.

Conflict recovery retention is budgeted and visible. Capacity failure blocks a
destructive resolution; it does not authorize discarding the losing generation.
Explicit later deletion of a recovery copy follows product retention/confirmation
policy and is not part of automatic sync.

### 9. Delete is a revisioned intent, not remote absence

Local slot deletion records a durable coordinator delete intent with last known
generation/hash and expected provider revision. Remote deletion uses revision CAS. If the
remote head advanced, deletion becomes Conflict. A provider returning NotFound does
not prove another device intended deletion unless verified journal/tombstone policy
does so.

Until the product's slot-lifecycle contract defines synchronized tombstones, the
baseline does not automatically propagate remote absence into local deletion or local
absence into remote deletion. Explicit confirmed delete may remove both only after
revalidation and recovery/retention policy.

### 10. Qualification covers races, clocks and preservation

Required evidence includes:

- local save/load while cloud is disabled, missing, offline, signed out, quota-full or
  permanently failed;
- conditional create/write/delete, precondition failure and rejection of
  uncoordinated background mutation;
- device A/B offline branches, upload reordering, stale retry after newer local save,
  process crash at each journal/provider/local-publication boundary and profile switch;
- same, ancestor, divergent, unknown-history, same-generation/different-hash, corrupt/
  untrusted and absent-without-tombstone classification;
- arbitrary provider/device clock skew proving timestamps never select a winner;
- KeepLocal/KeepRemote/KeepBoth/Defer with stale revisions, failure injection, signing/
  scope restrictions and both complete archives recoverable; and
- UI close/reopen/headless policy using coordinator snapshots/commands with no path,
  credential, provider object or semantic merge authority.

## Consequences

### Positive

- Local persistence remains reliable independently of cloud support.
- Provider concurrency and logical publication identity cannot be conflated.
- Offline branches are classified without trusting clocks.
- Conflict resolution cannot silently discard both recoverable generations.

### Costs

- Providers need qualified revision CAS capability for automatic mutation.
- The coordinator owns a durable journal, ancestry evidence and recovery storage.
- Some simple blob stores support explicit export/import only.

## Rejected Alternatives

### Newest provider timestamp wins

Rejected because clocks are skewable presentation data and do not prove lineage or
write authority.

### Always prefer local or always prefer cloud

Rejected because either rule silently destroys valid offline work on the other side.

### Read then unconditionally write

Rejected because another device may mutate between calls; only provider-enforced
revision CAS closes that race.

### Merge participant records automatically

Rejected for the baseline because subsystem invariants, cross-participant transactions
and deletes cannot be reconstructed safely by a generic cloud layer.

### Let the conflict dialog perform writes

Rejected because presentation does not own durable journal, local leases, provider
preconditions or operation lifetime.
