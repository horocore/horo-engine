# Durable slot lifecycle composition

`SaveSlotLifecycle` owns worker-side delete, copy, label rename, import, export and
soft-delete restoration for HORO-1430 #1430 [SAV-003.9]. Compose it once per typed
namespace with an approved `ProductSaveRoot`, immutable product policy and a host
authority that outlives accepted work. It retains the existing filesystem kernel
namespace lock; a second owner cannot open the same physical namespace.

Each operation owns a detached catalog snapshot and a private RAII owner lock.
Loading, optimistic consent, staging, publication and bounded recovery use that
same operation. The lock remains held until its catalog is destroyed; helpers do
not reacquire it. Opening validates the initial manifest before publishing the
owner, and releases that initial operation before moving the owner to its caller.
There is no ambient mutable catalog cache or second source of selection truth.

The host grants each operation independently. Shipping is the default profile,
all capabilities default to denied, and signatures default to Required. Server
composition requires Required signatures. Explicit unsigned local policy retains
the same bounds, containment and transactions. Requests cannot select a profile,
trust root, key, native path or namespace policy. A source-scope index selects only
one of the host's fixed import scopes; unknown sources are rejected. A client scope
must match its user/profile namespace tuple. A server scope supplies explicit
archive user/profile identities and signature verification binds the complete
server-world namespace encoding.

The host's `AcquireBinding` must pin the exact available namespace and revision
until its lease is released. Profile close/switch first closes admission, then
settles these leases. Callbacks run on storage workers and must not reenter the
owner. `ValidateSemantics` validates all required participants in detached storage;
it cannot publish a runtime candidate. The signature verifier and signer keep keys
inside host providers. The owner verifies a signer's returned header, manifest,
every stored chunk and signature policy before publishing it.

Import uses the request's `source` selector as the destination slot. Its imported
bytes are copied into operation ownership before admission. Copy/import verify
framing, integrity, host signature/scope policy, direct compatibility and participant
semantics. Migration-required input fails closed; compose a separately authorized
migration before retrying rather than widening policy. Every new publication gets
a fresh opaque generation, destination namespace/user/profile/slot identities and
a parent generation only when explicitly replacing that destination. Cross-scope
import can create an absent slot but cannot overwrite an existing or soft-deleted
destination. Every stored chunk, including unknown optional data, survives repacking.
Generic archive inspection remains strict: unsupported optional codecs fail closed.
Schema-2 authenticated Opaque records require the separately qualified
`ValidatedSaveSceneCanonicalPreservation` host path; this owner does not infer that
authority from archive metadata or bypass its required canonical-layout admission.
The old signature is never copied to changed bytes. Generation-bound thumbnail
catalog references are cleared on repack; retained optional presentation chunks do
not become a fresh-generation thumbnail by implication.

Rename changes bounded display metadata on the same slot and generation. It does
not rename an opaque slot file. Labels, including path-like or localized labels,
never participate in physical addressing. `List` and `ListDeleted` return derived
catalog indexes in one revision domain. Mutation consent binds that revision, the
exact binding revision and the target's generation. Absence consent includes
soft-deleted records; overwriting one requires an explicit restore/delete flow.

Soft deletion retains a generation and can be restored after restart. Permanent
deletion requires the host policy and retires bytes after the selection publication.
Platform recycle additionally requires an explicitly implemented host capability;
there is no permanent-delete fallback. Recycle receives complete immutable bytes
and metadata, never an internal filename. It must durably preserve and deduplicate
by namespace/slot/generation. Recycle failure reports committed logical deletion
with deferred cleanup and retains the source file for retry. The provider receipt
is persisted before file retirement. Deletion is not a secure-erasure promise.

`Execute(Export)` returns an independent immutable complete archive copy.
`ExportTo` publishes it through a separately host-admitted filesystem namespace
capability and opaque destination slot. It exposes no catalog, journal, thumbnail
temporary or operation staging file. A failed pre-replacement export leaves the
source and existing destination unchanged; a post-replacement sync failure has an
unknown external old-or-new outcome and requires host reconciliation.

## Storage authority and recovery

The lifecycle layout is a dedicated contained selection-manifest composition:

- `.lifecycle.catalog` is the authoritative atomic selection/lifecycle manifest;
  `SaveSlotIndex` and UI projections are derived from it.
- `.generation.<opaque-generation>.horosave` holds immutable unselected or selected
  generations. Generation publication uses atomic create-if-absent, never a
  replace-existing generation rename. Logical slot IDs do not name these files.
- `.lifecycle.journal` binds the exact namespace, proposed generation and complete
  byte digest before candidate creation. Recovery never deletes a colliding file
  with a different digest. Corrupt ownership evidence fails closed.
- Retired generations and pending recycle receipts remain in the bounded selection
  manifest until cleanup acknowledgement. Unrelated files are never swept.

The worker serializes all operations under the owner mutex and host binding lease.
It writes/flushes an owned sibling temporary before the atomic selection rename.
Failed validation, staging, write, flush or pre-rename operations preserve the
selected source/destination bytes and metadata. Cancellation is observed through
candidate preparation and immediately before the selection gate; after visibility
it cannot return Cancelled. A post-rename synchronization failure returns
`save.slot_commit.outcome_unknown`; do not retry as though nothing happened.

On reopen, the manifest selects complete old or new generations. Call `Reconcile`
under the same binding authority. It re-establishes selection durability **before**
retiring a journal or last-known-good generation, checks journal ownership/digest
against exact selected bytes, removes only an unpublished owned candidate, and
replays exact retired cleanup idempotently. A malformed/missing selected generation
fails closed. Acknowledged publication stays successful when cleanup is deferred.
No fallible result allocation follows the selection gate.

This composition does not automatically adopt legacy ordinary slot files or a
different store's catalog. Use explicit bounded verified import/export during a
host migration; never open a second filesystem owner or reinterpret orphan files
as committed slots. Existing `SaveStorageAdapter` and `SaveSlotCommitTransaction`
contracts remain unchanged for their qualified provider/store compositions. A host
must choose one storage authority per namespace; it cannot expose both catalogs as
live competing sources of truth.

The optional worker I/O observer is a bounded qualification seam with no path or
payload access. It injects failures immediately before write, after actual write
progress, before file sync/replace, after visibility before directory sync, and
before retired cleanup. Normal composition supplies no observer. Native Windows
and macOS durability/reparse behavior must be qualified on their actual platforms;
Linux tests alone do not provide that evidence.
