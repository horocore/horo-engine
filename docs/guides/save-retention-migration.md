# Durable automatic save retention

SAV-005.8 adds opt-in worker publication to `SaveSlotLifecycle`, the existing
namespace/kernel-lock, binding-lease, hidden-generation and selection-manifest
owner. It does not add a scheduler, mutable index, storage owner or cloud queue.

Hosts using `ISaveSlotCommitStore` keep that authority until deliberately migrated.
Do not publish a namespace through both authorities. To adopt this implementation,
compose one lifecycle owner, grant `PublishSave` and independently grant `Retention`,
enable `SaveSlotLifecyclePolicy::retention`, and route the existing save worker's
finalized archive through `CommitSave`. Existing callers retain their default-denied
capabilities; archive and ordinary lifecycle command formats are unchanged.

Capture the destination's exact namespace binding, catalog revision and previous
generation before encoding. The archive must already name that destination and
parent generation and match its complete catalog metadata. No repacking, generation
allocation, capture or new job occurs in `CommitSave`. Publish its result back to
the original arbiter/controller: success completes that operation; pre-publication
failure retains its original failure; `SlotCommitOutcomeUnknown` requires worker
reconciliation, never blind retry or a rewritten terminal result.

Auto and Checkpoint limits apply independently. Normal and low-space capacities
are between two and 64, age has a retained floor, and at least one retired backup
is kept. Low-space pressure is an explicit host observation, not an inference from
a disk-full error. The catalog allocates causal order from successful publication
revisions. The host supplies a nondecreasing commit clock; archive timestamps and
opaque IDs do not order retention. Deterministic retirement proceeds oldest first.
A replacement must target the oldest eligible slot, never the newest publication,
a pin, a deleted record or another category. A new slot may evict older automatic
slots within its category. Manual/Quick/Recovery/System publications and unknown
legacy order are excluded from automatic pruning. Capacity blocked by pins, legacy
protection or pending tombstone storage returns quota failure before any new write.

Pins use exact catalog/generation consent via `SetPinned`; they persist on reopen.
Age and capacity decisions, the new publication, backups and tombstones cross the
same atomic manifest gate. A failed save cannot consume rotation. An unknown gate
outcome exposes a complete old or new catalog; `Reconcile` establishes durability
before cleanup. Intentional backup and cloud holds do not block later save work.
`ReadBackup` requires Export capability and independently verifies the complete
archive, scope, signature, compatibility, semantics and matching metadata. Recovery
owners can feed this archive and its trusted sequence to existing recovery logic;
promotion remains a separate explicit publication with fresh consent/generation.

When cloud tombstones are enabled, the worker supplies an existing validated
`SaveCloudRevisionSnapshot` bound to the exact pre-publication index and namespace.
The retirement copies provider/account scope, exact slot/generation/archive hash,
opaque object key, CAS revision and confirmed mutation evidence into the selection
manifest before the index row disappears. Missing, stale or foreign evidence fails
closed. Unknown remote state remains a coordinator reconciliation task; it does not
invent an object key or authorize an unconditional remote delete.

The authenticated sync owner validates current provider/account/object CAS and
obtains durable remote deletion confirmation before `AcknowledgeCloudDelete`.
The call requires the exact persisted scope, generation and current catalog
revision plus a nonzero mutation receipt. It records Deleted/receipt and clears the
cloud hold atomically. It does not release a backup hold. Stale or foreign consent
cannot affect a replacement; worker reconciliation cleans only generations with
neither hold. This durable tombstone is coordination evidence for the existing
cloud owner, not a provider invocation or independent sync queue.

The private lifecycle catalog advances from schema 1 to schema 2. Schema 1 remains
readable; its absent causal order/time/pins become zero/zero/unpinned, and its normal
retired cleanup receipts retain their original semantics. Unknown order is protected
from automatic pruning rather than inferred from presentation timestamps or a scan.
The next successful mutation writes schema 2; older runtimes reject that catalog.
Products must gate downgrade or preserve an installation rollback snapshot. No
parallel compatibility catalog is written. Malformed versions, unsigned/type errors,
duplicate order/generation/slot evidence, contradictory times, invalid provider
metadata and oversized catalogs fail closed before cleanup.

Validation uses the existing lifecycle executable and isolated public-header
consumer, including real native I/O occurrence failures, restart, pins, saturation,
legacy migration, backup corruption, cloud object/CAS round trips and stale consent.
GPU hardware and provider-network execution are not part of this policy path.
