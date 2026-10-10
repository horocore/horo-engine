# Save Game And Persistence Architecture

## Purpose And Core Decisions

This document defines runtime save authority, coherent snapshot capture, durable
archives, transactional restore, migration, host isolation and security for
HORO-1391 #1391 [SAV-001.1]. These are normative implementation requirements; the
complete save service/archive protocol is not claimed as implemented by this change.
ADR-112 freezes the portable archive, canonical state, identity and compatibility
policy for HORO-1410 #1410 [SAV-002.1].
ADR-113 freezes product/environment/user/profile namespaces, logical slot addressing
and storage mapping for HORO-1425 #1425 [SAV-003.1].
ADR-114 freezes authoring/runtime state composition and subsystem-owned canonical
adapters for HORO-1438 #1438 [SAV-004.1].
[ADR-140](../../adr/140-foliage-placement-baked-dynamic-state-and-eviction-ownership.md)
applies that single-authority model to baked, ephemeral and durable foliage mutations:
Runtime Save/Persistent World owns canonical capture and dormant deltas, while Terrain
defines and applies the semantic projection without a second persistence store.
[ADR-149](../../adr/149-destruction-persistence-replication-streaming-and-authority.md)
applies it to destruction: DFR owns semantic revision/seed/chunk/support sets, Physics
owns paired active-chunk motion, and Persistent World owns dormant storage without
becoming a semantic or residency authority.
ADR-115 freezes local/cloud authority, provider revisions and conflict preservation
for HORO-1466 #1466 [SAV-006.1].
ADR-116 freezes save-source trust, bounded admission, tool capabilities, credentials
and development/shipping security profiles for HORO-1495 #1495 [SAV-008.1].

- RuntimeSaveService coordinates one runtime save/restore authority under the
  application/session lifetime. It never retains an unleased reference to a scene
  that restore can replace.
- Runtime state is never serialized through SceneDocument or the authoring AST.
- Capture produces owned immutable data at a Scene lifecycle safe point; workers
  serialize, hash, sign and perform all durable file operations.
- A save is one final immutable file. Its `ArchiveContentHash` excludes the
  integrity trailer; signing finishes before the one durable publication, never
  afterward.
- Restore prepares every required participant before a no-fail owner-boundary
  publication. A failed preparation leaves the active runtime unchanged.
- Signature acceptance is trusted host policy, never a flag chosen by the archive.
- Persistent world deltas include inactive cells; account settings/achievements are
  independent of slot-specific player state.

## Persistence Domain Boundaries

| Domain | Authority | Stored content | Isolation |
|---|---|---|---|
| Runtime save (.horosave) | RuntimeSaveService plus subsystem canonical adapters | Stable entity structure and subsystem-owned canonical gameplay/world participants | User/server save namespace, never authored .horo files or runtime memory images |
| Scene document (.horo) | SceneDocumentPersistence | Authored hierarchy/defaults/assets and editor semantics | Project asset tree; no runtime mutation on save/load |
| Editor recovery (.horo_recovery) | ProjectSceneRecoveryRecord | Dirty document snapshot, revisions and recovery context | Bounded recovery namespace; cannot replace canonical data implicitly |
| Project/workspace metadata | ProjectSession / Workspace | Project configuration and editor layout/state | Separate JSON schemas, authority and transactions |
| Account/profile persistence | Account/profile service | Settings, accessibility, achievements, account-wide statistics | Independent user/account store; loading an old slot cannot roll these back |

The four original document/runtime/workspace categories stay separate. Account
persistence is explicitly a fifth independent domain rather than a chunk implicitly
restored from a runtime slot. SlotPlayerState contains only the host-declared
inventory, health, transform, local progression and similar slot-scoped state.
Each field belongs to one persistence owner; gameplay modules and slot player data
must not store two authoritative copies of the same field.

[ADR-068](../../adr/068-music-transport-and-cross-system-ownership.md) applies the
same rule to Audio. Gameplay/narrative/adaptive-music owners may persist semantic
music state, definition/section identity, transition variables, and an admitted
stable musical/content position. Audio may contribute that owned checkpoint at the
save barrier, but live voice/bus/stream/provider handles, callback/device epochs,
raw output sample indices, queues, decoder/DSP state, ring buffers, and native
objects never enter a save. Restore resolves current localized/cooked content and
prepares a new Audio transport generation before publication.

[ADR-092](../../adr/092-character-controller-determinism-and-state-composition.md)
applies the same single-authority rule to Character. Runtime Save contributes the
required `horo.character.state.v1` provider chunk through
`CharacterStateCodecV1`; it does not serialize only a transform, live
`CharacterWorld`, Jolt/proxy state or a save-specific Character schema. The chunk
belongs to the same capture epoch as its paired Physics/world checkpoint. Restore
decodes/migrates a complete canonical Character candidate, resolves stable support
bindings against the detached Physics candidate and publishes only with the
aggregate no-fail commit.

```text
SceneDocument -> one-way RuntimeSceneDefinition conversion
    -> application/session runtime ownership
        -> active SceneRuntime + gameplay state + persistent world deltas
        -> RuntimeSaveService -> immutable RuntimeSaveSnapshot
            -> worker serialization/integrity/signing -> single .horosave file
Account/profile service -> separate account store (not restored by slot load)
Editor document/recovery/workspace writers -> separate namespaces and schemas
```

## Runtime State Classification And Composition

Persistence is opt-in by semantic owner, not inferred from reflection, component
membership, trivial copyability or object reachability:

| State category | Owner | Runtime-save treatment |
|---|---|---|
| Authoring definition/default | SceneDocument, source asset and cook pipeline | Immutable compatible base reference; never rewritten by slot save/load |
| Runtime canonical state | Scene or owning gameplay subsystem | One registered canonical adapter may capture it |
| Derived/rebuildable | Owning runtime subsystem/cache | Excluded and rebuilt from base assets plus restored canonical state |
| Transient execution | Scheduler/operation/subsystem runtime | Excluded unless the owner promotes a semantic value through a versioned schema |
| Presentation | Renderer, Audio device/voice, Runtime UI | Excluded; gameplay/domain owner may persist semantic intent separately |
| Asset/cooked content | Asset Registry and cook/package authorities | Stable identity/revision dependency only, not copied into runtime-state chunks |
| Account/profile | Profile/account service | Separate store/transaction; slot restore cannot rewind it |
| Platform/network external state | Platform/network authority | Excluded and revalidated/reconnected after restore |

Restore resolves the compatible cooked Scene/world base, then applies saved state by
stable identity. Authored entities without saved overrides use compatible defaults;
declared overrides replace only owned fields; deletion is an explicit tombstone.
Durable runtime spawns carry `PersistentEntityId`, declared archetype/prefab/definition
identity and canonical owned values. A changed base requires declared migration or a
typed incompatibility. Runtime state never writes back to SceneDocument/cooked assets.

`SavedSceneBootstrapDescriptor` is the typed admission record for that first step. It
binds logical world/base-scene identity, persisted cooked asset type evidence, exact scene
definition/revision/content digest, an optional stable authored spawn anchor, and
slot-generation/source-world transition provenance. Content reconciliation converts
the saved base-scene UUID bytes directly to `AssetId` and admits its actual
provider-loaded cooked envelope from an immutable verified mounted selection.
`AssetRegistrySnapshot` authoring presence is not installed cooked availability.
Preparation requires privately issued `ReconciledSaveContent` and an explicit host
decoder receiving the admitted owned envelope. The host supplies the authoritative
scene asset type separately; changing a save's expected type cannot approve another
cooked type. Missing required content, type/revision/digest changes, unsupported
decoders and missing spawn anchors reject before world preparation. Admission is
revalidated before the existing Scene queue and aggregate transfer. Successful
preparation owns authored defaults; publication remains at the normal lifecycle
commit boundary. Shipping cooked-scene decoder and aggregate application save
composition are not supplied by this backend contract.

Content-aware queueing issues a private Scene-owned retained publication receipt.
Only the actual aggregate transfer supplies its SceneRuntimeId and dataset projection;
ordinary same-definition scenes cannot bind that receipt. Native installation evidence
is privately issued by the actual loaded module and pins its immutable declaration,
adapter and revocable native generation. Owner-thread admission precedes callbacks;
accepted safe-point captures instead seal immutable source/composition facts so
worker serialization never reads live Scene or installation controls.

The Horo Scene adapter owns persistent entity existence, authored/spawn identity,
tombstones, hierarchy/ownership, stable reference remaps and explicitly assigned core
component fields. It does not walk arbitrary component memory or own gameplay,
Physics, Character, AI or dormant world state. Each such subsystem owns its canonical
participant; duplicate field/semantic ownership rejects composition.

`PersistentEntityIdentityMap` is the Scene-owned detached identity candidate for that
boundary. Its persisted records contain only `PersistentEntityId`, a non-zero durable
incarnation, explicit authored or spawn-definition provenance, and live/tombstone state;
`EntityRef` appears only in the separate restore-time binding input. Construction sorts by
persistent identity, rejects duplicate IDs and authored origins, requires exactly one
same-Scene binding for every live record, and rejects every binding for a tombstone. This
makes decoder/ECS iteration order irrelevant, prevents stale generations from resolving,
and preserves destroyed authored entities as deletions rather than allowing defaults to
silently recreate them. Prefab-backed spawns carry the existing path-free save asset and
prefab-instance identities instead of a prefab object pointer or runtime occurrence handle.

`SaveableComponentAdapterRegistry` is the host-composed bridge between that stable
Scene identity map and gameplay component authority. The host declares required or
optional component types before activation and binds at most one versioned semantic
adapter per type against the frozen gameplay component registry. Required missing
adapters reject composition. Capture can return only canonical codec output; restore
first resolves the exact persistent entity generation and verifies current runtime
component ownership, then performs explicit migration and validation before producing
an inactive adapter-owned candidate. Defaults use the same validated entity/type route,
and publication is a separate no-fail lifecycle transfer. The adapter owns its runtime
save schema independently of the component descriptor's authoring schema. Authoring
serializers and native component memory are not inputs to this contract.

Always excluded unless a semantic owner defines a different canonical value are
pointers/native objects, runtime handles/indices, container capacity, jobs/futures/
cancellation/callbacks, pending queues/events, GPU/render extraction/fences, Audio
voice/device/decoder/DSP state, native Physics solver/manifold/proxy/query state,
Navigation queries/rebuild scratch, network connections/keys/packet queues, UI widget/
focus/animation state and wall-clock progression.

## Authority, Lifetime And Public Operation Contract

RuntimeSaveService is application-owned. SaveGameAuthority is its internal
transaction state, not a competing coordinator. The application/session outlives
both this service's in-flight work and every active/replacing SceneRuntime.
IRuntimeSessionHost is a typed injected capture/restore seam implemented by host
composition; runtime codecs do not depend on editor types or application internals.
A scene borrow is valid only inside its owner callback. Workers receive immutable
snapshots, candidates, asset/provider leases and generation-scoped completion data,
never SceneRuntime references or callbacks capturing raw scene pointers.

Canonical participant descriptors are inert, stable-type-ID keyed metadata.
Composition binds one `ICanonicalStateAdapter` per participant, validates unique
semantic ownership, schemas, scopes, capture/restore roles, required/optional
behavior, cost bounds and dependency DAG, then seals a registry revision. An operation
pins that revision and module leases. Registry changes require an explicit host
quiescent rebind; they cannot unload codecs under a worker or register through ambient
service discovery.

Each dependency edge declares required or optional absence policy and the exact
capture, restore or combined phase it orders. Snapshot publication builds separate
stable topological capture and restore plans, selecting the lowest stable participant
ID whenever multiple nodes are ready. Registration order, addresses and unordered
container iteration never break ties. Missing required dependencies, present
dependencies without the declared phase capability and cycles fail snapshot
publication with diagnostics naming the involved stable IDs; optional absence alone
does not fail. Separate capture and restore edges may name the same provider and carry
different absence policies; overlapping phase coverage for one provider is rejected.
Cycle diagnostics name one actual deterministic cycle rather than downstream blocked
participants. Allocation failure returns a typed registry error, and fallible insertion
completes before generation publication so membership and generation remain atomic.
The identity-sorted binding view remains the canonical manifest/query
projection and is not used as an execution plan.

`Horo/Runtime/Save/SaveParticipation.h` is the installed gameplay/module-facing
contract. A host-owned `SaveParticipationHost` issues weak clients for one exact
non-zero module generation, advertises the exact API version and capture, restore,
save-request and load-request capabilities, and binds accepted participant adapters
into the application-owned canonical registry. Closing or replacing the host first
revokes client admission and unregisters that generation's live bindings; immutable
registry snapshots continue to pin their exact adapter leases until detached work
retires. Retaining a client across reload therefore grants neither stale callbacks
nor registry authority.

`Horo/Gameplay/PersistenceSource.h` is the project SDK callback contract;
`Horo/Gameplay/SaveGameplayPersistence.h` supplies the explicit runtime-only
behavior-instance, module-global, service and session binding. Each owner declares
one stable canonical participant/record, schema, module version and bounded payload.
The binding captures owned bytes at the safe point and creates an inactive staged
restore receipt. Exact compatibility is checked before publication; the receipt
pins its module generation through retirement. See the
[gameplay persistence migration guide](../../guides/gameplay-persistence-migration.md).
Native module registration exposes this opt-in declaration through
`GameRegistrationContext::persistence`; freeze resolves generated behavior and
service identities before module startup. Explicit host composition acquires an
adapter from `LoadedGameModule::AcquirePersistence` and registers it through the
participation client. Adapter acquisition closes with native reload admission;
retained registry snapshots and staged receipts prevent code unload. The exported
SDK's canonical identity/error/participant declarations belong to Foundation-only
`SaveApi`; archive and restore execution remain in Runtime, with gameplay adapters
in GameplayRuntime.

Gameplay save/load requests contain only an opaque `SaveGameSlotId` and typed policy
mode. The injected application operation host resolves the active namespace, project
policy, operation identity, safe point and storage adapter. The public request cannot
name a path, namespace, storage provider or native handle. Native C++ participants and
planned scripting adapters map to the same inert descriptor, bounded capture sink,
staged restore and path-free request semantics rather than parallel persistence models.

The complete application service shape below remains schematic; it is not an
installed `RuntimeSaveService` header:

```cpp
struct ProductStorageId { Uuid value; };
struct EnvironmentStorageId { Uuid value; };
struct LocalUserStorageId { Uuid value; };
struct GameProfileId { Uuid value; };
struct SaveGameSlotId { Uuid value; }; // nonzero opaque identity, never a path
struct SaveNamespaceHandle { GenerationCheckedHandle value; };
struct SaveAddress {
    SaveNamespaceHandle namespaceHandle;
    SaveGameSlotId slot;
};
struct SaveOperationHandle { OperationId value; };
struct RestoreOperationHandle { OperationId value; };
struct CanonicalStateHash { Sha256 value; };
struct ArchiveContentHash { Sha256 value; };
struct SlotGenerationId { Opaque128 value; }; // nonzero, never time/hash-derived

enum class SaveOperationPhase : uint8_t {
    Queued, CapturingSnapshot, Serializing, FinalizingArchive, WritingTemporary,
    CommitStarted, VerifyingArchive, Migrating, PreparingRestore, ReadyToCommit,
    ApplyingState, Completed, Failed, Cancelled
};
enum class SaveCommitOutcome : uint8_t {
    NotCommitted, Committed, Unknown
};
struct SaveOperationSnapshot {
    OperationId operation;
    SaveAddress address;
    SaveOperationPhase phase;
    SaveCommitOutcome commitOutcome;
    float progress;
    OptionalError terminalError;
    OptionalSlotGenerationId slotGeneration;
    OptionalCanonicalStateHash canonicalState;
    OptionalArchiveContentHash archiveContent;
};
struct SaveRequest {
    BoundedUtf8 displayName;       // catalog metadata only, never archive/path identity
    SaveRequestKind kind;         // manual, quicksave, autosave
    ThumbnailPolicy thumbnail;   // optional/omitted or explicitly required
};

class RuntimeSaveService final {
public:
    RuntimeSaveService(IRuntimeSessionHost&, CanonicalStateParticipantRegistry&,
                       SaveStorageAdapter&, SaveNamespaceBinding&,
                       PlatformServices&, JobSystem&, OperationStore&);
    Result<SaveOperationHandle> SaveSlotAsync(
        SaveAddress, SaveRequest, CancellationToken);
    Result<RestoreOperationHandle> LoadSlotAsync(
        SaveAddress, RestoreRequest, CancellationToken);
    Result<OperationId> RefreshCatalogAsync(CancellationToken);
    Result<OperationId> DeleteSlotAsync(SaveAddress, CancellationToken);
    SaveCatalogSnapshot GetCatalogSnapshot() const; // cached immutable data; no I/O
    Result<SaveOperationSnapshot> GetOperationSnapshot(OperationId) const;
    void PumpOwnerThread();              // bounded record drain; never filesystem I/O
    void OnLifecycleCommitSafePoint();   // explicit lifecycle participant
    Result<ShutdownTicket> BeginShutdown();
};
```

Initial `Result<SaveOperationHandle>` / `Result<RestoreOperationHandle>` reports only
admission. Disk-full, signing, migration or participant-adapter failure later becomes Failed
with the original typed cause in that operation's snapshot. It cannot retroactively
change the return value of SaveSlotAsync. Handles use the application-owned ADR-010
OperationStore/OperationId; no independent save job scheduler/store is created.
GetOperationSnapshot returns the pending/terminal value without blocking; an unknown
or expired handle is a typed lookup error, never apparent success.

Terminal snapshots remain immutable per operation until acknowledged/evicted under
the store's bounded retention policy; a new request does not erase the previous
caller's result. Completed has progress 1 and Committed for a mutation. Failed may
carry Unknown publication outcome; Cancelled always means cancellation won before
its commit gate. Completion/event delivery capacity is reserved at admission.

### Concurrency, Autosave And Safe Points

At most one save/restore/delete mutation is active per session. Per-slot storage
leases additionally serialize cloud/import/other-process writers. Conflicting manual
requests return save.operation.in_progress with the current operation ID. Enumeration
uses a separately bounded worker and publishes a catalog snapshot; it does not read
files synchronously from a browser or owner-thread getter.

The host autosave scheduler retains one coalesced pending autosave intent when busy,
marked visibly Pending rather than silently discarded. It captures the latest state
when admitted, not the stale state when its timer fired. It retries at the next
eligible idle safe point, with bounded cooldown/age diagnostics; persistent I/O/quota
failure uses bounded retries and an explicit exhausted/error indication. Manual work
can take priority but cannot silently clear pending autosave. Stop/session replacement
invalidates the old intent; the host reports cancellation or explicitly schedules a
new-session checkpoint. Autosave ring rotation advances only after durable success;
a failed save never consumes the last good ring entry.

`SaveEventTriggers` is the additive owner-thread event adapter for SAV-005.6.
A host copies an immutable allowlist of at most 64 product-issued trigger IDs,
typed payload requirements, Auto/Checkpoint modes, safe logical targets and
Continue/Block transition-failure policy. Publishers supply only an ID, monotonic
sequence, generation and a bounded gameplay/milestone/project/transition payload;
paths and target selection are absent from that contract. Registrations cannot
publish Manual/Quick slots or bypass namespace, catalog capacity, generation CAS,
product eligibility, cooldown or confirmation policy. A fixed registered target
is chosen by the host's catalog policy; ring selection/rotation remains a separate
host responsibility and advances only on durable success. Recompose registrations
at a quiescent session boundary to change allowed targets.

One pending intent survives competing arbiter work. Equivalent rapid events
coalesce only when cooked policy permits it and return the original effective
correlation; distinct busy intents are explicitly rejected rather than silently
lost or queued without bound. Receipts retain one effective event per registration;
older monotonic sequences cannot replay work. Callers keep the returned handle for
terminal observation after the bounded receipt is replaced. Cooldown is shared by
all triggers for a mode, so a second publisher cannot bypass it.

BeforeTransition payloads capture the exact source scene/registry incarnation;
the transition owner waits asynchronously for durable completion before applying
the scene change. AfterTransition payloads capture the activated destination and
hold transition finalization; Block denotes an explicit failure state and never
an automatic rollback. Either failure or cancellation applies the registered
Continue/Block policy and preserves its original typed cause. No timer-time state
or live scene pointer is retained. A transition to the wrong incarnation before
capture fails visibly. Session replacement closes and recreates the dispatcher.

Migration for event publishers: replace direct storage calls with Submit, retain
the effective correlation, and poll Receipt/DecideSaveTransition. Host composition
calls CommitAtSafePoint only inside CommitDeferredLifecycleChanges and forwards
its immutable handoff with the existing handle. The adapter reserves the existing
safe-point coordinator's exact-generation capture fence. The host capture executor
polls cancellation, calls Revalidate under its mutation lease, uses the existing
capture barrier, and advances the arbiter to Encoding only after a coherent cut.
Workers receive the same correlation and expected generation; the host enforces
storage CAS, publication, terminal acknowledgement and lifecycle-fence retirement.
Close admission on the owner before destroying dependencies; admitted workers
remain host-owned. There is no independent event bus, operation store or worker
pipeline, and timer/manual producer contracts are unchanged.

The host registers capture and restore publication **inside**
CommitDeferredLifecycleChanges, after pending structural changes are resolved and
before the next simulation step. PumpOwnerThread runs on the owner and drains ready
worker records with a phase budget. A pump after that lifecycle phase queues actions
for the next valid boundary; it cannot claim that a past safe point is still active.
No worker invokes the pump, commits Scene state, calls UI, or waits on nested jobs.
SaveLimits validates finite positive limits for archive/decoded bytes, chunk/string
counts, snapshot/COW and candidate memory, queue/completion slots, per-pump work,
retry counts and stage/readback/signing deadlines. Aggregate admission counts old
and new snapshots/runtimes/retired resources together; declared local participant
limits are not extra capacity. Exceeding a bound fails or defers explicitly, never
silently unbounds a worker or guarantees a frame-time target.
Parser profile overrides may lower or raise individual bounds only within compiled
secure ceilings. Archive admission reserves cumulative validation work before hashing
and shares the remaining allowance with repeated chunk selections. Canonical child
readers share decoded-memory and byte-inspection counters even when reopened; migration
charges source staging and each step's input, declared work, and output against one
operation budget. A bounded failure reports a stable category and trusted structural
location, never untrusted text or payload bytes.
Serialization, compression, hashing, signing, quota queries, directory scans, flush,
AtomicReplace, deletion and cleanup run on admitted worker/storage roles. No normal
owner/render/transport frame waits for them. ADR-010 governs allowed teardown drains.

## Coherent Immutable Capture

RuntimeSaveSnapshot owns a StableTypeId-sorted set of `OwnedCanonicalSnapshot` values,
the core Scene identity snapshot, SlotPlayerState and PersistentWorld participants,
base asset/dataset revisions and a coherent CanonicalCaptureEpoch. It contains stable
persisted identities and owned/copy-on-write payload leases, never EntityRef addresses,
live component spans, renderer handles or editor objects.

At the owner safe point the session validates scene incarnation, participant registry
revision and capture budget, then copies bounded state or pins a versioned immutable/
COW root for **one logical tick**. Every required participant/world-delta root belongs
to that same capture epoch. Independent native owners prepare immutable semantic
capture versions asynchronously before that boundary; unavailable coherent versions
defer or fail capture instead of mixing ticks.

Capture never leaves live ECS pools, gameplay adapters or streaming state locked or
frozen after the safe point. Workers serialize only the detached snapshot. Large
captures require admitted COW/pages or versioned chunks with bounded owner work;
naive partial copies across subsequent live ticks are not coherent snapshots. COW
write headroom and old/new pages are reserved before pinning; if the budget cannot
support a consistent cut, report CaptureBudgetExceeded or defer. Do not hold the
simulation locked until a background serializer finishes.

Scene replacement after capture does not invalidate an already accepted detached
save: its source scene/revision remains explicit and its storage leases keep it safe.
PIE stop/session shutdown can still request cancellation before the durable commit
gate. A capture not yet completed against its expected incarnation fails stale.

`RuntimeSaveCaptureBuilder` is the public owner-safe-point coordinator for this cut. It
invokes each core `ICanonicalStateAdapter` with an exact value-only
`CanonicalCaptureContext` and a call-scoped host sink. The context exposes participant
and operation bytes, record/segment capacity and the eager-copy ceiling before the
adapter produces state. The sink may copy a small borrowed record into core-owned
storage or admit an already immutable, concurrently readable segmented/COW payload
lease; large state is not forced through one eager contiguous copy. Every retained
payload is destroyed while its adapter/registry lease still pins module code.

`Seal` requires every record of each required or participating optional capture owner,
establishes canonical participant/record order, and returns `RuntimeSaveSnapshot` with
an immutable manifest-facing participant projection. Descriptor-approved optional
omission is explicit in that projection rather than inferred later by an encoder. The
sealed snapshot exposes no participant adapter, so background encoding, cancellation
cleanup and shutdown can observe only immutable payload segments and stable capture
provenance. No borrowed span, mutable runtime pointer or module-owned container
allocator crosses the safe-point boundary.

### Save-safe quiescence authority

`SaveCaptureBarrier` supplies the bounded SAV-005.3 owner-thread authority before
`RuntimeSaveCaptureBuilder`. The session registers stable owners for all four
required domains: fixed simulation, jobs that mutate canonical state, deferred
scene structural mutation, and subsystem semantic roots. Every capture binding
in the pinned registry must have a corresponding registered barrier owner.
Even an empty job/structural domain publishes an explicit ready epoch; absence is
not readiness. Host-only domain owners do not become serialized records.

A producer acquires an exact authority/index/serial mutation ticket before writing
canonical state. Starting a mutation invalidates its prior ready epoch. The host
finishes the ticket only after its work commits, publishing the exact semantic
capture epoch; independently prepared immutable versions use `PublishReadiness`.
Worker jobs never call the barrier: their owner-thread completion handoff publishes
readiness after completion, without a nested join or wait. Adapters remain responsible
for the semantic correctness and immutability of their supplied roots.

`Request` exposes a pending operation/generation fence. A pending request permits
normal simulation and mutation between frames. At `CommitDeferredLifecycleChanges`,
after structural publication and before the next simulation step, the host invokes
`CaptureAtSafePoint` with current provenance and its pinned registry. A busy or
wrong-epoch participant returns typed pending evidence without invoking any adapter;
an explicit denial or quiesce deadline returns Deferred or Failed according to the
admitted policy. The host acknowledges terminal evidence and may schedule a new
request with current state; the barrier does not retry, replace operations, or reuse
a stale tick automatically. Scene/session replacement cancels the old request and
creates a new exact generation request. Incorrect-generation capture is rejected.

The readiness check and closing of mutation admission are serial on the same owner
thread. During the synchronous builder capture, reentrant mutation/readiness/lifecycle
calls reject explicitly. On success, typed failure, budget rejection or unexpected
exception, admission reopens before return. The result hands off only the builder's
sealed immutable snapshot. Background serialization, signing and storage never extend
the barrier. Shutdown closes new admission and cancels pending requests; already
issued mutation tickets may finish so their owners can drain safely.

The polling projection measures elapsed request time and synchronous capture duration
with an injected monotonic clock. Finite positive quiesce and capture budgets have
compiled ceilings. A capture exceeding its synchronous budget is discarded and
reported with `CaptureBudgetExceeded`; the clock measures actual completion rather
than promising preemption of arbitrary adapter code. Adapters must honor their bounded
owner-work contracts. Backward samples cannot reduce already observed duration.

Migration: existing `SaveSafePointCoordinator` users retain its operation/worker
lifecycle fencing. Capture executors route their concrete immutable cut through
`SaveCaptureBarrier`, register all canonical mutation owners, and publish readiness
at each current committed epoch. Phase membership alone no longer proves participant
quiescence. Restore publication keeps its existing prepared-candidate transaction.

### Thumbnail Capture

Thumbnail acquisition is a separate renderer-owned asynchronous readback request
with scene/view/frame generation and bounded staging/fence lifetime. No save capture
safe point performs a synchronous GPU readback or waits on a render target. Prefer
an already completed compatible thumbnail; otherwise request a later completion
and record its source frame so it is not falsely advertised as the exact capture tick.
Optional thumbnails are omitted on timeout, stale scene or headless execution.
An explicitly required thumbnail may fail the save after its configured finite
deadline, without blocking the owner. Late readbacks only retire their own resources.

## Canonical Logical State

Every required participant produces a canonical logical byte stream before entry
packing, compression, signing or publication metadata. The whole-save canonical
stream contains the stable project/world/base-scene and base-dataset identities,
`SaveSchemaVersion`, then participant ID, `ParticipantSchemaVersion` and canonical
payload tuples sorted by `StableTypeId` bytes. Every variable-length identity and
payload is prefixed by its canonical unsigned byte length before its bytes, so tuple
concatenation is unambiguous. Optional participant presence is part
of the logical stream when its state participates in restore.

Canonical participant codecs use stable numeric field IDs and schema-defined
presence/default rules. Integers have explicit widths and signed representation;
booleans are 0 or 1; enums use declared stable values. Declared IEEE 754 floats are
encoded explicitly, normalize negative zero to positive zero and reject non-finite
values unless that schema defines one exact canonical representation. Strings are
length-delimited validated UTF-8 scalar sequences with no locale or implicit
normalization. Maps and sets sort by canonical encoded key bytes and reject duplicate
canonical keys; sequences retain schema-defined semantic order.

Compiler padding, pointer/native-handle values, native enum widths, unordered
iteration, wall/capture timestamps, play duration, display name, slot/generation,
entry layout, codec/compression and signature metadata never enter canonical state.
Changing a canonical rule requires a `SaveSchemaVersion` or owning
`ParticipantSchemaVersion` change and migration. The identity is:

```text
CanonicalStateHash = SHA256(
    ASCII("HoroSave.CanonicalState.v1") || 0x00 || canonicalSaveState)
```

`CanonicalStateHash` establishes logical equivalence under the same schemas and
semantic dependencies. It is not a file-integrity value, conflict generation or
authentication proof. Readers validate it from decoded canonical participant state;
they do not reconstruct it by reserializing metadata JSON with a local library.

## Single-File Archive And Integrity Envelope

A durable .horosave is **one regular file**, never a live directory bundle. Payload
entries may be compressed independently. Directory layouts are tooling exports only
and must be finalized into this container before publication. Platforms without the
required same-storage atomic transaction semantics must provide a qualified save
container adapter or return UnsupportedAtomicStorage; no copy-over-live fallback.

The following v1 envelope fixes the byte range covered by integrity. All integers
are unsigned little-endian; fields are encoded explicitly, not native struct dumps.

| Preamble field | Width / rule |
|---|---|
| magic | 8 bytes, ASCII HOROSAVE |
| archiveFormatVersion | uint32, v1 = 1 |
| flags | uint32, v1 = 0; unknown bits rejected |
| payloadByteLength | uint64, bounded before allocation/read |
| trailerByteLength | uint32, exactly 52 unsigned or 116 Ed25519-signed |
| reserved | uint32, must be zero |

Preamble size is 32 bytes. The payload follows immediately. The integrity trailer
begins at offset `32 + payloadByteLength`; the file ends exactly after the trailer.
Checked arithmetic rejects overflow, truncation, trailing bytes and contradictory
lengths. Archive size is derived from this framing/filesystem result, not a mutable
header field later patched during hashing.

Payload is the versioned container's bounded entry table plus exact stored entry
bytes. Entries use stable typed IDs, deterministic order, explicit codec IDs and
stored/decoded lengths, never filesystem extraction paths. The v1 container codec
must reject duplicate IDs, overlap, gaps/unreferenced bytes, unsupported codecs and
unbounded decompression. Metadata encoding is deterministic UTF-8 with stable key
order and duplicate-key rejection. Verification hashes the **stored bytes**, not
JSON reserialized by the reader.

The headless `SaveArchiveReader` admits the v1 payload with one additional fixed
wire boundary: a 32-byte `HSCTNR1` container header declares a v1 container, zero
flags, an entry count, and a fixed 188-byte entry-record size. Each record carries
an entry kind, raw storage codec, stable record bytes, a bounded participant owner,
relative offset, stored/decoded lengths, alignment and decoded SHA-256. Header and
manifest records are first, followed by manifest-owned chunk records in stable
record order; their data ranges must be contiguous from the first data byte through
the exact payload end. v1 rejects extension records and codecs other than raw before
any decompression or participant decode. Archive/container v2 retains that exact
framing and adds codec ID 1 (`Deflate`, zlib-wrapped DEFLATE) for chunk records only;
header and manifest stay raw. Both version fields must agree. Readers of v1 still
reject Deflate rather than silently changing old-format interpretation. The built-in
codec inventory declares supported levels 1–9 and no dictionary capability. Writer
policy retains metadata and small/ineffective chunks raw, with explicit required
codec failure rather than silent fallback. Per-chunk stored and decoded lengths,
expansion ratio and total decode work are checked before output allocation. Unknown
required codec IDs fail as a typed compatibility error. Only container v2 with
semantic save schema 2 may retain an unsupported codec on a chunk whose actual
manifest owner is optional. Metadata, v1, schema 1 and required owners still reject
unsupported codecs. The public directory validator remains supported-codec-only;
this exception is issued by the bounded reader after parsing the actual manifest.
Unsupported selection always fails without decoding. Content admission additionally
requires the authenticated canonical layout to classify that chunk as opaque and
verify the exact stored bytes; structural retention alone grants no activation or
semantic compatibility authority. The reader verifies the finalized envelope
hash first, then uses the existing metadata and chunk-directory validators and
returns only an immutable detached view. Selected chunks now return owned decoded
bytes (including v1 raw selections) rather than a borrowed archive span; callers
must consume or move that owned value, and no selected bytes outlive their own
result accidentally. The reader owns no filesystem, module callback or
gameplay activation authority.

| Logical payload entry | Content |
|---|---|
| header.json | Logical slot ID, `SlotGenerationId`, optional parent generation, producing `ProductSaveCompatibilityVersion`, project/world/account scope, baseSceneAsset and bounded provenance timestamps |
| manifest.json | `SaveSchemaVersion`, `CanonicalStateHash`, StableTypeId-sorted participant/chunk IDs, each `ParticipantSchemaVersion`, required flags, lengths, codecs and per-chunk SHA-256 |
| core Scene participant | Stable authored/spawn entity identities, owned core fields, tombstones, hierarchy and reference remaps |
| subsystem participant entries | One canonical payload per registered owner/StableTypeId, including gameplay services/components |
| slot-player participant | Slot-scoped player state only; no global settings/achievements or duplicated subsystem fields |
| Persistent World participant/cell deltas | Persistent state of active and inactive cells with base dataset revision |
| thumbnail.png | Optional bounded image; source capture/view metadata |

These names describe entry roles, not directories to extract on disk. Header metadata
has no checksum field; manifest has no archiveSignature field. Manifest per-chunk
hashes cover raw decoded **data** entries only, not header/manifest themselves.
`ArchiveContentHash` authenticates all stored metadata and compressed bytes. After
outer verification, bounded decode checks each data entry's raw digest and length.
Every required entry has exactly one manifest record; unknown required data fails.

The trailer is encoded in this exact order:

```text
archiveContentHash  32 bytes
signatureAlgorithm   2 bytes (uint16: 0 None, 1 Ed25519)
signerKeyId         16 bytes (opaque key ID; all zero only for None)
signatureByteLength  2 bytes (uint16: 0 or 64)
signature            signatureByteLength bytes
```

None requires a zero key ID, length 0 and a 52-byte trailer. Ed25519 requires a
nonzero key ID, length 64 and a 116-byte trailer. Unknown/mismatched combinations
fail validation. Ed25519's signature format and verification follow
[RFC 8032](https://www.rfc-editor.org/rfc/rfc8032#section-5.1); use a vetted crypto
implementation, not a custom signing algorithm.

Hash/signature inputs are normative (`||` concatenates bytes; each ASCII tag
includes exactly one terminating zero byte):

```text
ArchiveContentHash = SHA256(
    ASCII("HoroSave.ArchiveContent.v1") || 0x00 || preamble || payload)

signatureMessage =
    ASCII("HoroSave.Signature.v1") || 0x00 ||
    uint16LE(signatureAlgorithm) || signerKeyId ||
    uint16LE(signatureByteLength) || ArchiveContentHash

signature = Ed25519.Sign(trustedHostPrivateKey, signatureMessage)
```

This is Ed25519 over the framed message, not Ed25519ph. `ArchiveContentHash` is the
typed identity of the final immutable preamble/payload bytes; the self-referential
integrity trailer that carries it is excluded. Neither that hash nor the signature
is part of its own input. Choose the signature scheme/length before constructing the
preamble, hash the final preamble/payload once, then append the complete trailer. No
header/manifest/archive bytes are patched after durable publication. Changing slot
generation, timestamps, metadata, chunks or compression requires a new hash/signature
and a new transaction, even when `CanonicalStateHash` is unchanged.

### Trusted Signature Policy

SaveSignaturePolicy is a trusted host/project/namespace configuration with Disabled,
Optional and Required modes. The file cannot choose or weaken it.

- Disabled is explicit local/PIE unsigned-only policy: reject signed input unless
  the host explicitly changes to a verifying policy. It never silently ignores a
  present invalid signature.
- Optional permits unsigned files; every present signature must verify against a
  trusted, scope-authorized key. It offers corruption detection for unsigned files,
  **not** tamper protection or prevention of signature stripping.
- Required rejects missing, stripped, malformed, untrusted or invalid signatures.
  Secure namespaces cannot fall back to Optional based on file contents or error.

The host binds trusted key IDs to project, account/server/world scope and permitted
algorithm. Public keys/trust roots are not supplied by the archive. Signed headers
bind these identities, the logical slot and `SlotGenerationId`; load compares them
to the requested namespace. Required signing failure or unavailable signer fails the
save before publication; ordinary clients hold no server private key and cannot
invent a local unsigned substitute. A trusted asynchronous signer may be injected
with bounded timeout, but no signing network call blocks the simulation thread.

No mode stores credentials/private keys in saves. Digests are not authentication;
a valid signature also does not prevent replay of an old valid save. Titles needing
anti-rollback require separately trusted generation/anti-replay state, not a
timestamp inside the attacker-controlled file. The archive is not encrypted by this
protocol.

### Optional Authenticated-Encryption Provider Boundary

`SaveArchiveProtection.h` defines a host-composed seam outside the v1 archive
format. `UnencryptedLocal` is an explicit local policy; `RequireAuthenticatedEncryption`
rejects plaintext without a fallback. The host selects an opaque provider ID and
non-secret key reference, supplies bounded associated data binding the namespace,
slot, generation and protection format, and composes a provider that mints a fresh
nonce and authenticates the complete sealed bytes before returning plaintext.
The protected admission function checks finite lengths, provider identity and
capabilities before invoking that provider; only its successful authenticated
plaintext reaches the bounded v1 reader. It does not treat `ArchiveContentHash`
as authentication. Provider error text and key material cannot enter the returned
diagnostic; stable unavailable, rotated, revoked, unsupported and authentication
failures remain distinguishable.

`SaveArchiveAuthenticity.h` adds the separate v1 signature-verifier seam.
Disabled rejects signed input, Optional verifies every present signature, and
Required rejects missing signatures. A bounded trailer preflight constructs the
exact ADR-112 Ed25519 signature message and calls a host-selected verifier with
host-selected scope before the archive reader can decode metadata. The reader
then checks the signed `ArchiveContentHash` against exact bytes before decode.
The verifier owns trusted roots, key rotation/revocation and cryptographic work;
Runtime Save exposes no public key source supplied by the archive. This does not
add a signing backend or make a signed archive fresh or semantically valid.

This seam is not a `.horosave` encryption format, key store, production crypto
backend or encrypted storage integration. A separately reviewed envelope and
vetted platform/credential provider are required before shipping encrypted saves;
the existing v1 writer and reader remain unencrypted and unchanged.

### Untrusted Input And Threat Policy

Every archive begins as untrusted bytes, including a file already present in the
canonical save directory, a provider-authenticated cloud download, a migrated output,
a modded save and a server-signed save. Path containment, catalog membership,
transport authentication, a filename suffix, a content hash or a signature is only
the evidence it specifically claims; none skips later compatibility, semantic,
namespace or freshness checks.

[ADR-116](../../adr/116-save-data-threat-model-and-trust-policy.md) defines the common
admission pipeline. The host resolves the operation profile, capability, namespace,
budgets and cancellation policy before acquiring bytes. Framing and total length are
checked before payload allocation; outer integrity precedes trust in payload control
fields; signature/scope policy precedes decode; compatibility and migrations operate
on bounded detached candidates; and all owners prepare before one no-fail restore or
atomic publication. Failure leaves the active runtime, source and last-known-good
local generation unchanged.

Import, export, inspection, migration, load, delete and conflict resolution are
separate capabilities. UI, CLI and MCP invoke application-owned operations with typed
addresses or admitted external handles; they never call codecs, choose trust roots,
hold leases, access signing/cloud credentials or publish a live slot directly.
Inspection returns bounded allowlisted metadata and safe diagnostics, not raw
participant state by default.

Development profiles may admit isolated unsigned/modded namespaces, fixture tools and
explicit raw developer export. They retain the same framing, parser/decompression
limits, path containment, transactional publication and credential isolation as
shipping. Shipping/server policy is fail-closed and cannot be weakened by archive
fields, debug flags, directory copies or a remote tool request.

## Save Pipeline, Publication And Cancellation

```text
Admit operation and namespace/slot mutation lease
    -> capture coherent immutable snapshot at owner lifecycle safe point
    -> worker serialize/compress, per-chunk hashes and final payload
    -> allocate SlotGenerationId and finalize publication metadata
    -> compute ArchiveContentHash, required/optional signing, append final trailer
    -> write unique sibling temporary file and flush final bytes
    -> enter non-cancellable commit gate
    -> worker AtomicReplace plus required directory/container durability
    -> publish owner catalog/result and cloud-sync intent for exact slot generation
    -> Completed (local durable success)
```

SaveRequest carries user metadata only. The service fills format versions, IDs,
scope, capture revision, chunk hashes, lengths and signature metadata internally.
The storage adapter resolves paths, performs quota checks and reserves peak space
for old file, temporary file and any required recovery copy. Quota estimates do not
replace handling a later disk-full/write error.

The local archive file primitive maps only typed namespace and slot identities below
the resolved product root. It holds directory handles across reads and publication,
rejects redirected directory entries, reparse/symlink targets, hard-linked archives,
and case aliases where the platform folds names. It writes a complete temporary
archive through the held slot directory before replacing a generation. Callers must
still hold the namespace and per-slot lease and reconcile a reported error after
the atomic rename, because a post-publication durability failure cannot restore
the previous generation by assumption.

Cancellation is cooperative until the worker atomically enters CommitStarted after
its final cancellation check and before replacement. That gate is the practical point
of no return: cancellation arriving afterward returns TooLate and cannot label a
possibly published archive Cancelled. Before the gate, stop supported work, retire
I/O/signing/readback leases and remove only this operation's temporary file once it
is no longer in use. Cleanup failure is reported and swept safely later; it is not
an assumption of immediate removal or physical I/O interruption.

Worker-side AtomicReplace is the final **content publication**, followed only by
required durability synchronization and result delivery. The file is never reopened
to add a signature or thumbnail. The slot lease stays held through publication/outcome
reconciliation. Owner Pump updates an immutable catalog and emits completion records;
it does no rename, fsync or directory I/O.

### Publication Outcomes And Crash Recovery

Distinguish atomic visibility from acknowledged durability. SaveStorageAdapter must
report NotPublished, PublishedDurable or OutcomeUnknown. The current Foundation
DurableFileSystem::AtomicReplace returns `Result<void>` and can report a directory
sync failure after rename; generic failure therefore does **not** prove the old file
is unchanged. The save adapter must conservatively classify that case as unknown
and reconcile under the slot lease; this documentation does not claim the primitive
already exposes richer outcomes.

- Failure proven before publication leaves the previous archive intact; operation
  fails with its cause and eventually cleans its temporary file.
- PublishedDurable yields Committed/Completed. Late cancellation is ignored/reported
  TooLate; cloud upload failure cannot undo the local save.
- OutcomeUnknown yields Failed with commitOutcome Unknown, quarantines further slot
  mutation/sync, and retains a recovery diagnostic/transaction record. Reopen/validate
  the destination under the lease, compare its `SlotGenerationId` and
  `ArchiveContentHash` with old/new expectations and retry durability where supported.
  Do not delete a possibly published archive or blindly retry over it. Report
  old/new/unresolved truthfully.
  Reconciliation has a separate operation/catalog revision; it does not rewrite the
  original immutable Failed/Unknown result into a retroactive success.

A filesystem/process crash before rename leaves the old archive and perhaps a temp;
after rename it may expose old or new according to the qualified durability contract,
never a partly rewritten destination. Startup validates published files and worker-side
sweeps only operation-owned stale temporaries under their namespace/lease rules.
Temporary or migration files are never catalogued as save slots. Cleanup is not run
from a synchronous EnumerateSaveSlots call. No portable crash guarantee is assumed
for an unqualified filesystem/platform container.

### Bounded Last-Known-Good Slot Recovery

Last-known-good recovery is a detached planning stage under the exact namespace and
slot lease; it is not an alternate publication authority. Storage supplies the current
artifact, recovery backups and already quarantined evidence together with a non-zero,
trusted retention sequence. The planner bounds the total observations and orders them
by that sequence only. Archive timestamps, slot-generation bytes, filenames and scan
order never establish retention causality.

`SaveArchiveRecoveryValidator` admits each backup with the bounded archive reader,
matches its immutable archive evidence to trusted catalog metadata, and evaluates the
active compatibility policy before the artifact can be offered for recovery. The
result remains typed: `Valid` is eligible for promotion, malformed/truncated,
integrity-mismatched or contradictory data is `Corrupt`, and unsupported reader or
compatibility policy is `Incompatible`. Corrupt and incompatible evidence is retained
as different quarantine diagnostics; neither can become a promotion candidate.

A catalog/archive pair that is internally consistent but addresses a different
requested logical slot is also `Incompatible`: it is wrong-scope input, not damaged
bytes, and must not be silently offered as a recovery source. A mismatch between the
archive and the trusted catalog for the same requested slot is `Corrupt`. This keeps
wrong-scope current data on the explicit-confirmation path while preserving automatic
corrupt-current recovery for genuinely damaged evidence.

The default recovery policy retains three valid backups and eight quarantine artifacts
within a bounded observation budget. Retention is deterministic across enumeration
orders: newest valid backups are kept first, invalid evidence is ordered by retention
sequence, and the invalid current artifact is protected before other quarantine
cleanup. If the policy cannot preserve that current evidence, planning fails closed
without cleanup. A valid current artifact suppresses recovery and produces no cleanup
plan when no recovery publication is needed.

Promotion is represented as immutable input to `SaveSlotCommitTransaction`. The
transaction journals the candidate, prepares a complete hidden generation, and keeps
the current publication selected until the candidate is durably published. Therefore
validation failure, user cancellation, quota failure and an unknown publication
outcome cannot overwrite the only valid generation; an unknown outcome is reconciled
under the same lease. Planned backup/quarantine cleanup is applied only after durable
publication succeeds, never before. Recovery copies preserve the embedded
`SlotGenerationId` as replicas of that logical publication, consistent with
[ADR-112](../../adr/112-save-archive-container-and-compatibility-policy.md).

Automatic promotion is limited to the explicit policy: corrupt-current recovery is
enabled by default, while interrupted-publication recovery requires opt-in. An
incompatible current artifact always requires an explicit host/UI confirmation. The
planner returns typed decision reasons; presentation adapters own localized
explanation and confirmation, and cannot publish or delete recovery evidence.

Cloud registration occurs only for the final validated local slot generation after
PublishedDurable. The coordinator durably journals the exact address, generation,
parent, archive hash, expected provider revision and retry identity, then pins or
revalidates that archive before transfer. A concurrent later save explicitly
supersedes intent; an older upload cannot be reported as the newer generation.
Startup reconciles the journal rather than blindly replaying a generic FIFO. Completed
means local durability; cloud state is separate and cannot turn local success into
false rollback.

## Transactional Restore

### Prepare Without Mutating The Active Runtime

1. Pin a read lease/version of the source archive. Bound/check the framing and outer
   `ArchiveContentHash`, enforce trusted signature policy, then decode metadata/chunks
   with all length/hash/schema/identity checks. Verify project/account/world scope and
   base AssetId/dataset/package dependencies. Browsing unverified headers shows
   untrusted metadata, not an authenticated playable slot.
2. Migrate only detached staging data if needed. Workers decode the core Scene and
   subsystem canonical participants, SlotPlayerState and persistent world deltas into
   a private PreparedRuntimeBundle. Pin participant registry and AssetRegistry
   revisions/leases.
   Persisted stable IDs are remapped to new runtime identities; old EntityRef and
   streaming epochs are never restored from disk.
3. The owner queues RuntimeSceneService::QueuePreparation through the existing
   scene activation admission path; heavy preparation still runs in background. Each
   required adapter's PrepareRestore stages independent candidate state on its
   declared worker/native role and returns Pending/Prepared or typed failure. Native
   resources obey ADR-012/011 admission and retirement barriers.
   No worker creates a published RuntimeScene identity or mutates the live scene.
4. Prepare the entire commit ticket: candidate Scene storage, gameplay/participant
   roots, slot player state, persistent world ledger, reference remaps and admitted
   lifecycle events/retirement capacity. Required initial cells must reach their
   Ready/Prepared barrier; remaining dormant deltas need not load every world cell.

Each adapter exposes fallible PrepareRestore and a bounded no-fail PublishPrepared
(or equivalent noexcept ownership transfer), plus asynchronous candidate retirement.
An adapter that can only mutate live state through a fallible commit cannot join an
atomic required restore; reject the composition before accepting a load. Optional
participants need an explicit validated absence/substitute policy, not a swallowed
required failure. Account/profile services and unrelated external side effects do
not participate in the slot transaction.

### Commit At One Observable Boundary

The session adapter coordinates a PreparedRuntimeBundle ticket with the existing
Scene lifecycle service; it does not introduce a bypass public activation API.
This is a required extension of the commit integration, not a claim that SCN-001
already supports a composite gameplay restore. The current one-pending-scene-operation
rule remains enforced. The scene candidate must remain behind the shared bundle
commit gate: its own readiness cannot trigger early automatic scene activation while
a required gameplay participant is still Pending. A host lacking that integration
rejects composite restore rather than applying the scene first.

`SceneRestoreBundle` supplies the Scene-owned implementation through
`QueuePreparationWithRestore`. It uses the original staged restore transaction's
scoped aggregate publication gate. Archive/session composition supplies validated
source receipts and live generation authority; reference-bearing prepared owners
consume the resolved context after all identity allocation and state application.
The [reference-fixup migration](../../migrations/HORO-1437_restore_reference_fixups.md)
records the owning targets, compatibility behavior, bounds and required regressions.

At CommitDeferredLifecycleChanges, under the exclusive owner mutation boundary,
revalidate the operation cancellation gate, expected session/scene incarnation,
source/archive and participant/asset registry revisions, and all Prepared acknowledgements.
A mismatch rejects the candidate before publication. Then publish Scene and all
candidate participant/world/player roots as one unobservable-to-readers transaction.
Publication performs only prevalidated ownership transfers: no allocation, I/O,
blocking wait, arbitrary callback or new recoverable failure. Runtime identity is
created/published by the Scene service at this boundary, not by a worker candidate.

No simulation tick, renderer extraction or observer can see Scene applied but
inventory/quests unapplied. Observers receive SceneRestoredEvent only after the
complete bundle is published. The old bundle is queued for dependency-ordered
retirement; its destructors/jobs/GPU fences do not run as unbounded commit work.
Logical success may precede physical old-resource release, with all leases charged.

Cancellation wins only before the final commit gate. After publication, report
Committed/Completed and TooLate for late cancel. Recoverable preparation failure
retires the candidate and leaves the active bundle unchanged. An unexpected adapter
violation after publication is a host/session fault, not a fictional safe rollback;
contain/fail the session under host policy and retain unsafe-to-free dependencies.
The normal atomicity guarantee relies on validating the no-fail publish contract.

### Host Failure Presentation

RestoreRequest captures a host failure policy, not an engine hardcoded Main Menu.
With a usable active runtime, default failure preserves it and returns diagnostics.
At initial load with no usable runtime, remain in host Loading/NoActiveRuntime and
report failure. A host may explicitly request a named safe-scene transition or UI
recovery flow **after** the failed transaction; that is a separate transition, not
partial restore commit or implicit destruction of a running scene.

## World Streaming And Inactive Cell Persistence

ADR-012 StreamingPartitionAuthority remains the sole residency authority. The session
owns PersistentWorldState, a revisioned persistence ledger, not a second cell loader.
Its keys combine stable world/dataset identity and the full cell tuple
(x,y,z,lod,layerId). Entries store stable entity IDs, spawned records, mutations and
tombstones relative to the cooked base revision. Runtime PartitionEpoch, per-attempt
StreamingGeneration, slots, pointers and GPU/resource handles are not durable IDs.

Before eviction removes the last live copy of dirty cell state, the owning gameplay/
Scene providers publish its delta into that ledger at the registered owner safe
point. This is an explicit edge in ADR-012's retirement DAG while needed Scene handles
are still valid. Failed capture/admission holds retirement and keeps resources
charged; it must not discard unsaved state merely because a cell becomes Unloaded.
Eviction need not durably save a user slot on every cell change, but it must retain
an owned recoverable-in-session representation of the delta.

Foliage follows this protocol under ADR-140. The immutable cooked cluster is the base and
is not repeated in a save. Product-authorized durable spawns, baked-instance tombstones
and canonical updates enter the Persistent World ledger by stable world/dataset/base/
cell/foliage identity. Cell/session/owner-bound ephemeral overlays, decoded instances,
render buffers, Physics bodies and Navigation tiles are excluded. Terrain owns semantic
validation/encoding and active application; it does not own another archive, spill store
or dormant ledger. A dirty foliage handoff failure blocks eviction rather than dropping
the delta or forcing an implicit slot save.

Destruction follows the same no-loss protocol under ADR-149. Its canonical snapshot
binds exact fracture artifact content/chunk-table identity, semantic revision,
deterministic seed, health/phase and broken/active/supported/dormant sets plus required
support data. Runtime Save captures that DFR-owned value once. Gameplay-authoritative
active-chunk motion is a paired Physics-owned Horo checkpoint at the same world/tick;
native solver bodies/shapes/manifolds are excluded.

Before the last resident copy retires, Persistent World accepts the exact DFR revision
and required motion-to-dormancy policy result. A stale, over-budget or failed handoff
blocks eviction. Later cell activation resolves compatible artifacts and applies the
dormant state through the normal aggregate Scene/DFR/Physics/Render barrier. A content
change requires registered stable-chunk migration; index/name matching, partial masks
and intact fallback are forbidden.

Active-cell changes, inactive ledger entries and tombstones join the same coherent
SaveCaptureEpoch. An entity/field has one authoritative record at that cut, not two
copies from both live ECS and the ledger. Resident-but-unpublished provider candidates
are not live gameplay state and are not captured as spawned entities. Loading/Failed/
Evicting cells use the last committed delta plus any owner-committed live changes,
never half-decoded candidates or discarded tombstones. Unloaded cells retain their
last state even when no ECS entity is present.

Ledger memory and spill space are bounded. Large dormant deltas may live in immutable
spill chunks written on workers; ledger roots pin exact chunk revisions through the
save. Eviction cannot release its source until the replacement representation is
owned and available. A missing/corrupt referenced spill is a save failure, not an
empty cell. Save archives are self-contained for their persisted deltas and do not
depend on ephemeral spill paths after completion.

Restore stages a new ledger and base dataset revision with the candidate runtime.
Required initial cells use the normal streaming admission/readiness barriers and
apply their saved deltas before activation. Other deltas remain dormant until normal
residency requests load those cells, then apply exactly once per new incarnation.
Fresh partition/cell generations fence old asynchronous work. Cross-cell entities
have explicit persistent or cell ownership and stable relocation records; saving or
evicting a cell cannot duplicate/delete a network/persistent actor based on position.
A base dataset change needs an explicit delta migration or typed incompatibility.

Full-world saves/restore in multiplayer are server-authorized operations. Clients
cannot restore authoritative server state from local slots. Host network admission,
peer quiescence/relevance and resynchronization policy are explicit prerequisites for
a server restore; old replication messages are fenced by the new runtime/session
incarnation. Local cosmetic/client state must not overwrite server gameplay state.

## Archive, Schema And Product Compatibility

The four version axes are independent:

| Version | Authority / use |
|---|---|
| `ArchiveFormatVersion` | Horo-owned envelope, entry table and trailer codec; framing preflight and archive migration |
| `SaveSchemaVersion` | Whole-save canonical root, required roles and cross-participant composition |
| `ParticipantSchemaVersion` | StableTypeId-keyed participant payload codec and independent migration chain, including ECS and slot player state |
| `ProductSaveCompatibilityVersion` | Product release's declared compatibility matrix/policy epoch; never a codec selector |

Base asset/dataset revisions are semantic dependency identities, not schemas.
`engineVersion`, `projectBuildId` and product semantic version are bounded diagnostic
provenance; none determines readability. A newer engine build is not rejected solely
by its build string.

SaveGameManifest contains `SaveSchemaVersion`, `CanonicalStateHash` and a
StableTypeId-sorted list of participant entries with explicit
`ParticipantSchemaVersion`, required flag and chunk IDs. It is not a string-keyed map
of engine-version aliases. Header metadata contains stable project, base-scene,
logical-slot and `SlotGenerationId` values. Display names and timestamps never select
codecs, resolve assets, establish causality or participate in logical-state identity.
The header also records the producing `ProductSaveCompatibilityVersion` as policy
provenance, never as a participant codec selector.

### Content-aware semantic schema 2

The content reconciliation writer emits semantic `SaveSchemaVersion` 2 while
preserving container versions 1/2 and independent participant versions. Its canonical
root keeps domain `HoroSave.CanonicalState.v1` and encodes explicit UInt32 version 2,
stable project/world/base-scene UUIDs, a dataset sequence qualified by real prepared
composition, and sorted owner/schema/record tuples. The implemented no-dataset path
requires privately sealed `Absent` projections and rejects unqualified or unmapped
PersistentWorld owners before publication; unrelated Gameplay/SlotPlayer owners
remain supported. Schema 1 is admitted only by explicit trusted legacy policy and
its hash is never reused after a changed capture.

A compatible cooked substitution changes physical installation resolution only.
The original logical base-scene/asset identity and expected source digest remain
in the required declaration and accepted capture header. Every later admission
reapplies the explicit typed project mapping against the actual provider envelope;
capture does not silently migrate persistent identities. Declared schema-1 inputs
also enforce this policy rather than bypassing it through legacy baseline lookup.

Required owner `horo.save.scene.canonical.v2`, participant schema 1, reserved record
`2d576609-77ae-4fb2-bf4a-9e7b63cbc002` stores complete version-1 canonical layout.
Every directory record appears once, including the layout itself as Known. Tag 1
hashes exact supported canonical payload bytes. Tag 2 hashes a closed tuple of codec,
stored/decoded lengths, alignment, decoded digest and exact stored bytes without
asserting decoded semantics. Required owners cannot be opaque. The authenticated
layout persists across releases recognizing an optional owner or codec; that
recognition alone cannot reinterpret its stored representation. Required content
requirements remain separately decoded and reconciled, so opaque tags cannot bypass
module/content admission. Missing/duplicate/extra/conflicting/version-mismatched
layout or a source canonical hash mismatch rejects before host callbacks.
The exact bounded wire tuples, ordering, reserved declaration identity and caller
migration are defined in [the content reconciliation contract](../../migrations/HORO-1439_content_reconciliation.md).

The inert SaveMigrationRegistry has three distinct step kinds:

```cpp
struct ArchiveMigrationStep {
    ArchiveFormatVersion from;
    ArchiveFormatVersion to;
    ArchiveMigrationFn migrate;
};
struct SaveSchemaMigrationStep {
    SaveSchemaVersion from;
    SaveSchemaVersion to;
    SaveSchemaMigrationFn migrate;
};
struct ParticipantMigrationStep {
    StableTypeId participant;
    ParticipantSchemaVersion from;
    ParticipantSchemaVersion to;
    ParticipantMigrationFn migrate;
};
```

Composition validates unique edges, complete deterministic paths, bounded resource
costs and declared cross-participant dependencies, then seals a migration-catalog
identity. Registry functions operate only on staging readers/writers, never on live
ECS, account profiles or source files. Archive migration changes container structure;
save-schema migration changes root composition; participant migration advances only
that participant (for example quests 7 -> 8 independently of inventory 2 -> 5). No
implicit backward schema downgrade is permitted.

The runtime contract is exposed by `Horo/Runtime/Save/SaveMigration.h`. Its
`SaveMigrationRegistry` is mutable only during composition and produces an
immutable generation-pinned snapshot for planning. The snapshot canonicalizes
definition order and identity, validates one forward edge per typed range,
selects one sequential route or an explicitly declared checkpoint, and returns
typed diagnostics for gaps, ambiguity, unsupported source/newer versions, and
non-equivalent checkpoints. `SaveMigrationExecutor` copies the validated source
into bounded detached staging and returns a new candidate; callbacks cannot
receive a mutable source or live runtime reference, and the source is checked
again before success.
The step context carries remaining operation work and candidate byte ceilings so
callbacks can reject expansion before allocating. The executor charges source
staging, each step's input and declared work, and each bounded output against one
operation budget.

Participant steps may also register a record callback. A detached participant's
records are sorted by `SaveRecordId`; each carries its own schema version and the
verified source participant, record ID, and schema provenance. The executor passes
only borrowed bytes for one owned record to that callback, checks its output byte
limit, advances its schema, and retains its identity and provenance. Record callbacks
may return a field-specific error through `SaveMigrationRecordContext::Fail`, which
adds step, source/target schema, participant, record, and field context while
preserving the typed cause. A participant step cannot change another owner's
payload or records unless its registered definition names that owner in a sorted,
unique `crossParticipantTransforms` contract bound to the target schema. The contract permits data transforms
only; target identity, schema, requiredness, record identities and provenance stay
fixed. An absent optional target is harmless when the callback skips it; a missing
required target is rejected by compatibility planning. Save-schema steps remain the whole-save composition boundary. Existing
participant callbacks without record callbacks must advance any populated record
schemas themselves. Existing payload-only callers need no migration. Failure at any
record or later validation stage discards the detached candidate and leaves the
verified source unchanged.
`RetainVerifiedSaveRecords` stages recognized manifest chunks through the bounded
archive reader and supplies their initial provenance; it commits to the detached
source only after every selected chunk succeeds. Callers still use
`RetainUnknownSaveData` for unsupported optional owners and must finalize and
verify any durable replacement before the slot commit transaction.
The catalog and route identity domains advance to v2 because participant step
identity now includes record-transform presence and exact cross-owner grants. Release
composition must regenerate its migration-catalog identity; archive wire versions
and existing source files do not change. Existing payload-only definitions may be
registered unchanged, while record-aware definitions supply the new callback and
cross-owner descriptors explicitly.

Compatibility preflight proceeds through framing/limits, archive version, outer
integrity/signature, save schema, required participant set/schema, then semantic
dependency identities and decoded hashes. Direct load is allowed only when every
required axis is in the release manifest's inclusive direct-readable range. Migration
is allowed only when every required axis is in a declared migration-source range and
the sealed registry has one complete path to current directly readable writer
versions. Unknown optional participants may be skipped only when no required
participant depends on them. Missing/unknown required participants reject.

An unknown participant marked required in the manifest blocks restore before any
participant callback; the diagnostic names its stable owner so the host can direct
the user to missing content, DLC, or a module. Unknown required feature bits also
block restore with the unsupported bit mask. An unknown optional participant is
preservable by default. A sealed release policy may list stable optional owner IDs
that are explicitly droppable; the list is sorted, unique, and disjoint from installed
participants. An absent entry is never permission to discard it. Before migration or
save-copy publication, the reader verifies every retained chunk and copies its exact
stored bytes plus codec, lengths, alignment, and decoded digest into bounded detached
staging. A changed archive may relocate an entry, but must retain those bytes and
integrity fields. A known optional owner with an unsupported newer participant schema
is treated as opaque under the same preservation rule. If a required participant
declares a dependency on either unknown owner or unsupported schema, preflight blocks
load and names the dependency. The migration executor rejects candidate output that changes a
preservable unknown participant, and a repack/copy must compare source and candidate
opaque records before commit. The source remains available on any failure. This
policy does not grant permission to interpret unknown payload schemas or bypass the
archive's integrity/signature and size checks.

For callers migrating from the earlier API, empty droppable and dependency lists
retain the conservative behavior. Code that constructs `SaveMigrationSource` from a
validated archive must call `RetainUnknownSaveData` before planning; planning now
rejects an unknown preservable owner without verified chunk evidence. Repack/copy
callers must carry those records into the destination and use
`VerifyUnknownDataRoundTrip` before the commit gate. The new policy fields are host
declarations, not archive-provided permissions, so existing v1/v2 files need no wire
rewrite.

Any newer archive format, save schema, required participant schema or product
compatibility version outside the declared range fails with a typed unsupported-newer
result before live mutation. Readers do not best-effort interpret forward input.

Verify the original archive under its trusted signature policy **before** migration.
Migrate into operation-owned staging and validate the output. For restore, an in-memory
candidate may use a trusted deterministic migration of an authenticated source; that
does not make newly serialized bytes carry the old signature. A durable upgraded
archive always gets a new `SlotGenerationId`, `ArchiveContentHash`, signature and the
current writer versions, then traverses the same finalize/flush/commit pipeline.
`CanonicalStateHash` remains unchanged only if logical state and semantic dependency
identities remain equivalent. Required signing without an authorized signer forbids
durable replacement; it may still permit host-approved in-memory restore migration.
Never copy the old signature onto changed payload or weaken Required for an upgrade.

Original slots remain unchanged unless explicit user confirmation or captured host
auto-upgrade policy authorizes replacement. Consent binds the source
`SlotGenerationId`, `ArchiveContentHash`, migration plan and destination namespace;
revalidate them under the slot lease before commit so a new save/cloud update is not
overwritten by stale consent. Migration failure/cancellation only retires its staging.
Unique sibling migration temporaries are not save slots and follow the same
lease-aware cleanup/crash reconciliation.

Before product 1.0, each shipped preview declares exact supported ranges and retains
fixtures for every claimed version; compatibility is not inferred across previews.
From product 1.0, every stable release migrates saves from at least the previous two
stable minor release lines and for at least 12 months after each source line's last
release, whichever is longer. Patch releases cannot narrow ranges or remove migration
paths. Later removal requires deprecation in two preceding stable minor release lines,
release notes and a final bridge release. A security-blocked vulnerable decoder may
shorten this only through an explicit release-manifest exception and typed security
diagnostic, never a false corrupt-save result.

Legacy proposed `checksumSha256`/`archiveSignature`, generic archive-revision,
single-`saveFormatVersion` and `player_profile.bin` layouts have no mechanically valid
implicit v1 interpretation. A supported older implementation needs an explicit
versioned reader/migration with bounded verification and declared release policy;
otherwise reject `UnsupportedArchiveVersion`. Never guess whether self-referential
hashing or signature stripping was intended. Account-global data in an old slot is
not restored into the account store; any explicit import is a separate
user-authorized account migration with conflict/merge policy.

## Product, User, Profile And Slot Storage

### Typed namespace hierarchy

`ProductStorageId` is stable product configuration copied into release metadata; it
is never derived from display name, executable/install path, semantic version, branch
or update channel. `EnvironmentStorageId` explicitly separates production,
development, tests and each unique PIE session. A product fork allocates a new product
ID and imports old saves deliberately. Missing/zero identities disable save admission
rather than falling back to a current directory or name.

The namespace owner is a typed variant. A client `UserProfileOwner` combines one
opaque `LocalUserStorageId` with one product-owned `GameProfileId`. A dedicated/headless
`ServerWorldOwner` combines a `ServerStorageOwnerId` with stable world/tenant scope.
The resulting `SaveNamespaceId { product, environment, owner }` is opened by the
application/profile owner and exposed to Runtime Save as a generation-checked
`SaveNamespaceHandle`. Callers cannot construct a handle from raw IDs.

Platform Services owns live platform authentication handles. The game identity/profile
service privately maps a provider-scoped handle to `LocalUserStorageId`; gamertag,
email, display name and raw provider tokens are never directory components. It owns
the per-user profile catalog, active `GameProfileId`, account association and profile
display metadata. Runtime Save owns slot operations, not user sign-in, profile
creation/linking or profile-store state.

[ADR-135](../../adr/135-platform-identity-session-generation-privacy-and-consent.md)
requires that live handle to be a non-guessable, non-persistent capability scoped to
one provider/session generation, distinct from the private durable provider binding.
Every cloud/profile user-scoped commit revalidates the captured session and applicable
access-policy revision. Account switch/sign-out invalidates old admission before native
teardown; stale callbacks and journal work cannot publish/replay into the next user.
Raw provider identity and personal presentation never enter save catalogs, archives,
paths or broad diagnostics.

When multi-device/cloud persistence is advertised, the backend must provide a stable
opaque authenticated-user scope so the same platform account maps consistently on
each device. If it cannot, cloud persistence is unavailable and local fallback uses
an installation-local partition; mutable user text is never hashed as a substitute.

Platform user capability is explicit:

| Capability | Behavior |
|---|---|
| `SingleLocalUser` | Bind one installation-local user partition. Platform-user selection/switching returns `NotSupported`; multiple product profiles may still be allowed. |
| `CurrentPlatformUser` | Bind only the authenticated current user; sign-out closes the binding and no new user inherits it. |
| `MultiplePlatformUsers` | Select an explicit signed-in handle; each maps to a distinct local partition. |

Guest/offline storage is its own installation-local user partition. Later sign-in does
not merge or re-own it automatically. Cross-user/product profile movement is an
explicit verified import/copy transaction that preserves source data until destination
publication succeeds.

### Slot, category and catalog identity

`SaveSlotLifecycle` supplies one explicit worker-side contained filesystem
composition for delete, copy, label rename, bounded untrusted import/export and
soft-delete restoration. Its atomic lifecycle selection manifest is the storage
authority; immutable `SaveSlotIndex` projections remain derived. Namespace binding
leases pin host/profile policy through publication, while hidden generation files
and digest-bound recovery evidence preserve old-or-new selection on failure.
Independent capabilities default to denied, unsupported recycle cannot fall back
to permanent delete, and cross-scope import can create only an absent destination.
See the [lifecycle composition and migration guide](../../guides/save-slot-lifecycle-migration.md)
for generation/ownership repacking, signature/semantic admission, cancellation,
post-rename outcome reconciliation and deliberate migration from other qualified
storage compositions. Hosts choose one storage authority per namespace.

`SaveGameSlotId` is an opaque nonzero UUID scoped to its namespace and remains stable
across overwrites. ADR-112's `SlotGenerationId` instead identifies one durable
publication to that slot. `SaveAddress { namespaceHandle, slot }` is the only runtime,
UI and cloud-coordinator address; it contains no path or label.

Manual, Quick, Auto, Checkpoint and registered product `SaveCategoryId` values are
catalog policy controlling capacity, rotation, overwrite and presentation. Quicksave
aliases and autosave rings map to slot IDs. Category, display name, list index and
timestamp never identify a slot or form a filename. Reclassification changes catalog
metadata transactionally without changing slot identity or archive bytes.

The storage authority owns one immutable revisioned catalog projection per open
namespace. Records include address, category, bounded presentation metadata, current
generation/content/state identities and lifecycle state. UI retains addresses and
expected catalog revisions; it never loads/deletes by label, timestamp or row index.

`SaveManagerProjection` is the read-only presentation boundary over a published
`SaveSlotIndex`, the active namespace binding, host-provided opaque profile summaries,
generation-specific compatibility/integrity assessments, operation progress, and
typed diagnostic categories. The producer deep-copies bounded values into a shared
const publication before an editor, runtime UI, CLI, or test adapter can retain it.
It never retains archive bytes, storage providers, account handles, live operation
handles, mutable index objects, paths, or raw terminal error text. Profile display
metadata and account authority remain with the profile owner; this projection exposes
only typed namespace IDs and availability. A host increments the publication revision
for any changed row, assessment, operation or diagnostic, even when the catalog itself
does not change.

Queries use stable slot-identity order, bounded exact-byte filters and pages. A
continuation cursor binds the exact namespace/binding/catalog/publication revisions
and filter; a changed publication requires a fresh query. Load/delete intents carry
those same revisions plus the selected slot's exact generation. The latest view
revalidates them before dispatch, and the owning service **also** revalidates under
its mutation lease; a view check alone is never commit authority. The public header
is owned by `HoroRuntime` in the header-ownership registry. Existing catalog/storage
callers need no migration; presentation adapters should replace retained catalog or
storage objects with this immutable view and command preconditions.

### Manual, quick-save and load command admission

`SaveCommands` is the owner-thread gameplay/script command adapter over the existing
session `SaveOperationArbiter`. The host supplies trusted namespace binding, runtime
revision/activity/authority, cooked product policy, a bounded catalog and exact-generation
compatibility/integrity assessments. Requests cannot supply those authority facts.
Manual save and slot load name opaque slots and capture their exact generation;
all requests carry namespace/binding, catalog and runtime revisions. Rejected admission
leaves the arbiter unchanged. Loading requires a Direct or MigrationAvailable assessment
and Verified or VerificationRequired integrity; archive verification and migration are
still mandatory in the existing load pipeline.

Cooked Quick policy already requires ReplaceSingle with one retained logical slot.
Both quick commands resolve the host's reserved `quickSlot`, independent of timestamps,
display names or catalog ordering. A missing quick generation permits quick save and
rejects quick load. Manual save cannot write the reserved quick slot or reclassify
another slot kind, and new manual slots must fit the cooked category capacity.

Confirmation returns a typed exact target and no operation handle. UI adapters own
prompt wording and interaction. A confirmed retry preserves the original revisions
and expected target generation; it cannot follow an overwrite or namespace/session
change. At most one nonterminal user command is retained: identical repeated input
returns its original handle and cancellation/deadline, while other user requests or
existing arbiter work return `save.operation.in_progress`. Product cooldown is measured
from admission using the host's monotonic clock and never accumulates delayed requests.

The host calls `Revalidate` under its mutation lease before dispatch/capture/load and
publication, then carries the exact expected generation into the existing slot CAS.
Changed authority, eligibility, binding, revision, kind or assessment rejects dispatch;
the host publishes the failure/cancellation through the arbiter. This adapter does no
I/O, capture, callbacks, worker scheduling or blocking. The host still owns barrier,
archive, storage, restore, shutdown and terminal acknowledgement. Close the command
adapter before settling the shared arbiter. Borrowed host facts/arbiter outlive the
adapter; recreate it for a replacement runtime session.

The public header is assigned to `HoroRuntime` with a dedicated public-header consumer.
This additive command contract changes no archive format or existing storage/capture API;
new adapters should submit typed commands rather than admitting arbitrary user intents
directly to the arbiter. Existing host producers retain their explicit composition path.

### Physical mapping and safety

Platform Abstraction resolves a product state root for the validated
`ProductStorageId`. SaveStorageAdapter alone maps
`SaveNamespaceId + SaveGameSlotId` to storage. The conceptual filesystem shape is:

```text
<product-state-root>/
  <environment-id>/
    <owner-id>/
      catalog
      slots/<save-slot-id>.horosave
      staging/<owned-operation-id>.temporary
```

Every component uses a fixed lossless filesystem-safe encoding of its opaque identity.
Free-form product/user/profile/category/slot labels are never path components. Secure
console containers may map the same typed keys to records without exposing paths.

The adapter enforces containment using safe handle-relative/no-follow platform
operations, not a string prefix test. Reject symlink/reparse redirection, traversal,
alternate-stream/device/path separators and unexpected file types. Never follow entry
names or embedded paths from the archive into the filesystem. Authorized imports copy
untrusted input through bounded verification into the destination namespace.

Mutations acquire a namespace/slot lease covering final source/destination checks and
publication; other processes/cloud coordination use the same locking/generation
protocol. Credentials, scope authority and lock paths come from trusted composition,
not archive metadata. Cleanup only targets owned temporary records under a valid
lease; a wildcard sweep must not unlink another process's active work. Catalog,
delete and import apply the same typed-ID, containment and generation checks.

Foundation's path-based DurableFileSystem primitives are building blocks, not proof
that no-follow/containment/outcome guarantees exist. SaveStorageAdapter must implement
and qualify them before production saves are enabled. Profile/account storage, cloud
retry journals, editor recovery, authored projects and PIE sandboxes remain distinct
sibling/virtual namespaces with separate schemas and mutation leases.

### Local operation ordering and process ownership

The local filesystem foundation supports one live storage owner per physical namespace.
`SaveFilesystemStorage::Open` acquires a nonblocking exclusive kernel lock on the
handle-relative `.namespace.lock` file in the slot directory (`flock` on POSIX,
`LockFileEx` on Windows). A competing owner, including another open in the same
process, returns `save.operation.in_progress`. Separate profiles/environments remain
independent. Unsupported lock facilities fail closed; no unlocked fallback is allowed.
The namespace lock is retained across moves until close. The file is persistent and
never removed, and PID/timestamp contents never establish ownership. Process termination
releases the kernel lock, so stale unlocked files need no destructive recovery sweep.
Inherited POSIX handles retain ownership until all holders close; close-on-exec prevents
ordinary child executables from extending the lease. All participants must cooperate
with this protocol; arbitrary external edits are not qualified concurrent access.

Within one owner, the filesystem byte primitive serializes worker read selection and
replacements across the namespace. A read pins the immutable file before releasing the
operation mutex and reading bytes, so later replacements cannot change that selected
inode/handle. It returns one complete old or new archive; a returned byte vector remains
immutable to later publication. The target-private pinned-file mechanism owns the selected native handle
independently of directory replacement/deletion or storage-owner shutdown and closes
it through RAII after byte I/O. Its deterministic regression uses a deliberate,
non-installed internal test interface; the public storage contract remains `Read`.
Windows publication uses `FileRenameInformationEx` with replace-existing and POSIX
semantics so open readers continue using the previous immutable file while new opens
select the replacement. This requires Windows 10 version 1709+ and filesystem support;
unsupported publication fails closed rather than switching to destructive replacement.
No lock is held by
a polling UI getter.
`Replace` itself is a low-level byte operation, not generation compare-and-swap.

`SaveSlotCommitTransaction` acquires a non-null exclusive store operation lease before
journal inspection, base-generation comparison, preparation or replay, and retains it
through publication/outcome reporting and journal retirement. The store must share one
lock domain across all instances addressing the same namespace/slot. Coarse namespace
serialization is supported; finer per-slot concurrency must still serialize catalog
publication and source/destination operations in a stable typed-address order. Read/list
providers take snapshots under that same domain: each result sees a complete old or new
catalog, and separate calls need not select the same revision. Callers must pin the exact
generation when relating metadata and archive reads. Recovery obtains the lease before
inspecting or removing evidence and never steals a live owner's work.

Generation preconditions are checked under the lease. An absent previous entry means
create-if-absent; an existing entry must match the selected publication exactly. Two
writes from the same base cannot both publish: a busy lease returns
`save.operation.in_progress`, and a later stale base returns `save.slot_commit.generation_stale`
before journal creation. Admission ordering follows lock acquisition, not wall clocks
or thread arrival; callers may retry a busy operation through their bounded scheduler.
The store's namespace binding is revalidated under its lease. Shutdown closes admission
and settles accepted work before releasing storage or lease authority.

Migration: implementations of `ISaveSlotCommitStore` must now implement `AcquireLease`
and return an owned non-null RAII guard. The existing fake store is the only current
implementation; no production catalog store is claimed. This deliberate source contract
change removes reliance on undocumented caller locking. Existing public headers remain
owned by `HoroRuntime`; affected consumer and transaction coverage must compile with the
new seam. Filesystem byte callers must share one opened namespace capability instead of
opening competing owners for each request.

### Profile-switch transaction

The application/session owner closes old save/load/delete/import/cloud-apply admission,
cancels pre-commit work, boundedly settles or retains ownership for post-commit work,
stops old cloud scheduling, releases catalog/slot leases and invalidates the old
namespace handle generation. It then binds the selected user/profile, validates the
product/environment, opens its catalog, publishes a new snapshot and reopens admission.

Operations never change namespace in flight. Completion captures the old handle
generation and can report/finalize only against that authority, never update the new
profile's catalog/UI. A local save already past commit may finish in the old namespace
but cannot upload under the new platform user. Sign-out uses the same close path.
Failure to open the new catalog yields explicit `NoActiveProfile`; rollback/rebind is
a fresh transaction, not a half-old/half-new binding.

## Host Environments, Account State And Cloud

| Host | Required behavior |
|---|---|
| PIE | Unique `EnvironmentStorageId` sandbox or bounded virtual store; never writes production slots, account profile or canonical .horo documents |
| Development/test | Explicit non-production product/environment partition; sharing requires reviewed product policy, never path/name coincidence |
| Packaged standalone | Validated product environment plus installation/platform-user and active game-profile namespace; manual/quicksave/rotating autosave catalog |
| Dedicated/headless | Server world namespace and authoritative gameplay state; no viewport, client prediction, HUD or client account preferences |

The host decides signature policy from its trust needs; being a server alone is not
an algorithm for choosing keys. Secure signed namespaces require Required. PIE
unsigned policy is isolated and cannot be carried into a production namespace by
renaming/importing files. Headless saves omit thumbnails and graphics dependencies.
Stopping PIE cancels before commit where possible and retires sandbox I/O; cleanup
waits for ownership release, never erases production storage.

Slot load does not restore audio volume, accessibility settings, account achievements
or account-wide stats. Account changes triggered by gameplay are independent typed
profile operations. A successful save/load is not a transaction over external platform
achievement/cloud-account APIs. Account policy may merge monotonic progression, but
that is never an automatic rewind from slot_player_state.bin.

Catalog UI consumes immutable records keyed by `SaveAddress`; no filesystem path or
raw platform handle crosses the surface. The save/cloud coordinator derives a bounded
`CloudSaveObjectKey` from the typed address and authenticated provider context.
Platform Services treats that key and finalized archive bytes as opaque and cannot
edit local storage. The application/profile-owned `CloudSaveCoordinator` is the one
sync state authority; Platform Services and UI are transport/presentation adapters.

The coordinator's versioned `SaveCloudRevisionMetadata` sidecar is scoped by the
complete local `SaveNamespaceId` plus distinct provider and account identities. Each
record names one current slot generation and exact `ArchiveContentHash`, with bounded
opaque provider object key, optional CAS revision, typed sync state and last confirmed
mutation. Provider revisions are not ordered and are never substituted for generation
or archive identity. The sidecar is not an archive field or a credential store.
The older `SaveSlotPublicationMetadata.cloudState` is only a non-causal catalog
listing hint; it cannot override this generation-bound coordinator state or grant a
remote mutation. Presentation adapters derive current sync status from a validated
coordinator snapshot instead of treating that hint as a second authority.

The local storage authority persists the sidecar and matching `SaveSlotIndex` as one
atomic catalog publication under the namespace/slot lease. Readers expose only a
validated immutable pair; a mismatched index revision, generation, hash, scope,
schema or limit is not a usable cloud state. A missing or stale sidecar is rebuilt
from the authoritative local index: exact same-scope generation/hash rows may retain
confirmed evidence even when another slot advanced the index. Changed rows become
`Unknown`; sidecars naming a newer index than the local input are rejected rather than
rolled back. In-flight upload/download
states return to `Unknown` for journal reconciliation after restart. Missing rows
never imply deletion; `Deleted` explicitly describes a confirmed remote state for a
still-named local generation, with durable deletion intent owned by the separate sync
journal. Existing catalogs need no archive migration: first open creates an all-unknown
sidecar before cloud scheduling. No cloud failure changes local-save success.

Automatic remote mutation requires provider-enforced conditional revision. A read
followed by unconditional write, a process-local mutex, advisory lease or provider
timestamp is insufficient. An uncoordinated blob backend disables background
upload/delete while local save/load remains functional.
The provider's opaque revision is only a compare-and-swap token; it is not
`SlotGenerationId`, ordered gameplay state or portable archive identity.

[ADR-134](../../adr/134-cloud-blob-transport-revision-precondition-and-offline-ownership.md)
defines that narrow transport: list pages are bounded, reads return complete bytes and
metadata from one exact revision, and automatic create/replace/delete is atomically
guarded by absence or exact provider revision. Whole-blob SHA-256 transport digest and
exact length detect partial/substituted transfer but do not replace archive trust.
Provider multipart staging never becomes visible as a Horo object; cancellation,
timeout and short transfer publish no partial bytes.

The coordinator allocates the durable `CloudMutationId` and retains exact payload
lease/digest, precondition and reconciliation state in its existing journal. Platform
Services holds only an in-memory request and the Platform Offline Queue holds no cloud
upload/delete copy. Quota observations are advisory: transport cannot delete,
truncate, split or select another local generation when provider capacity changes.

[ADR-136](../../adr/136-platform-offline-queue-ownership-replay-and-cloud-intent-boundary.md)
makes that exclusion structural: no API converts cloud upload/delete into a Platform
Offline Queue record, and no “persist offline queue” setting controls Save durability.
The queue and `CloudSaveCoordinator` may share stateless retry/deadline/error and
caller-owned atomic-storage helpers, but never a row, scheduler, outcome or cross-owner
FIFO. Reconnect wakes the owners independently.

The Save journal is durable before the coordinator submits its first automatic remote
mutation. Confirmed remote completion is journalled before cleanup; a crash after
provider commit but before local completion therefore re-enters exact-key/revision/hash
reconciliation with the original `CloudMutationId`. It never becomes a fresh generic
upload. Shutdown closes scheduling, checkpoints journal state and retains archive/
provider leases through request retirement without deleting unresolved intent.

On startup/reconnect the coordinator opens local state first, then reconciles verified
local and remote heads. Same generation/hash is InSync. A known remote ancestor of
local permits conditional upload; a known local ancestor of remote permits verified
download and local durable application preserving that remote generation. Same
generation/different hash is an integrity violation. Unknown ancestry, absent data
without verified deletion evidence, or two non-ancestor complete heads is Conflict.
`CanonicalStateHash` may report logical equivalence but cannot authorize overwrite.
Provider/device timestamps and play duration remain presentation/provenance only.

Downloaded data is untrusted until bounds, content hash, signature/scope and
compatibility checks pass. It enters local storage only through a slot lease, expected
generation recheck and atomic durable transaction; Platform Services never edits a
local file. Failed verification/publication leaves the previous local generation.

Divergent complete generations are never field/record merged and never resolved by
automatic last-write-wins. Before KeepLocal or KeepRemote can replace anything, both
verified byte sets must be durable: the active local slot plus a conflict recovery
record/blob carrying exact archive identity and provenance. KeepBoth may import one as
a newly finalized slot publication when scope/signing policy permits. Quota/retention
failure blocks destructive resolution instead of discarding the losing archive.

Conflict UI or headless host policy sees immutable coordinator snapshots and submits
a typed KeepLocal, KeepRemote, KeepBoth or Defer command capturing local generation/
hash and remote provider revision/generation/hash. The coordinator revalidates local
lease and provider CAS before applying it; stale input returns to reconciliation.
Closing UI changes no state. Signed slot identity is never rewritten merely to fit a
conflict filename.

Offline saves advance local lineage and retain bounded visible retry state. Profile
switch/sign-out closes old scheduling so intent never replays under a new provider
user. Required-signature policy also applies to downloads and migration; cloud
availability cannot authorize unsigned downgrade. Remote absence is not automatic
delete authority until a synchronized tombstone contract exists.

## Failure, Cancellation And Shutdown Summary

Slot lifecycle qualification observers (`ISaveSlotLifecycleIoObserver::Before`)
and recycle providers (`ISaveSlotLifecycleHost::Recycle`) use non-throwing typed
`Result<void>` boundaries. Providers translate their failures before returning,
including allocation failures using previously prepared error evidence. Escaping
exceptions violate the callback contract and are not recoverable lifecycle outcomes.
After selection visibility, a typed synchronization failure is outcome-unknown;
after acknowledged publication, typed recycle/cleanup failure preserves retired
bytes and reports committed/deferred cleanup. Engine allocation failures at these
stages preserve the same publication knowledge. External provider migrations are
documented in the [slot lifecycle guide](../../guides/save-slot-lifecycle-migration.md).

All failures follow ADR-008 Result/Error, retaining operation, `SaveAddress`, source
`SlotGenerationId`/`ArchiveContentHash` when available and nested asset/provider/
filesystem cause. Admission errors are distinct from later operation outcomes.

`SaveDiagnosticRecord` is the bounded backend-neutral projection of that failure
evidence. It maps the existing `horo.save` descriptors to a closed typed category,
retains an explicit owner disposition, operation stage, admission-or-terminal result,
commit knowledge, canonical typed correlation and required/optional partial-data
facts. Admitted terminal records require `OperationId`, namespace and logical-slot
correlation; immediate admission rejection has no operation identity. Generation
evidence can be checked against the current registry, namespace, slot-publication and
archive observations before use.

The projection never becomes an operation store, scheduler, result channel, logging
sink or retry policy. It retains only allowlisted Horo identities and scalars. Cause
chains retain bounded typed Runtime Save identities, while operation messages and
provider, filesystem, path, archive-content or credential text are discarded in
favor of the canonical descriptor summary. Optional private evidence contributes
only bounded observed/truncated/malformed flags. Required and optional data outcomes
are recorded explicitly; consumers never infer partial success from a missing row.

| Failure | Observable result |
|---|---|
| Busy / invalid slot / invalid scope | Immediate typed rejection; no admitted mutation |
| Capture budget/coherence failure | Defer under bounded policy or Failed; no live ECS freeze |
| Write/full/signing failure before commit | Failed/NotCommitted; previous archive intact; owned temp eventually retired |
| AtomicReplace or durability outcome uncertain | Failed/Unknown; reconcile/quarantine slot, no cloud publication or blind retry |
| Corrupt archive-content hash/chunk / missing or invalid required signature | Failed before restore staging/publication |
| Missing asset / incompatible schema / required participant failure | Retire candidate; active runtime preserved |
| Stale scene/registry/archive before commit | Reject candidate; never publish into a replacement incarnation |
| Cancellation before commit gate | Cancelled/NotCommitted after owned work reaches safe terminal cleanup |
| Cancellation after commit gate | TooLate; report actual commit success/failure/unknown outcome |
| Shutdown deadline exceeded | ShutdownIncomplete; retain outstanding dependency ownership safely |

BeginShutdown closes admission, invalidates pending capture/restore tickets and
requests cooperative cancellation. Worker I/O/signers and renderer readbacks retain
operation storage/module/asset leases until completion. Only ADR-010-permitted bounded
teardown drains may wait. Timeout returns ShutdownIncomplete with outstanding owners
and retained bytes; a retirement owner keeps all referenced dependencies alive. No
raw callback reaches a destroyed session and no worker is detached from freed state.
A commit already past its gate finishes or reports Unknown and leaves recovery data;
shutdown must not relabel it Cancelled or discard a potentially committed archive.

## Save Observability Contribution

Hosts explicitly create `SaveTelemetryRegistration` after process observability
startup and retire every Save producer before destroying that owner. Descriptors
remain inert; feature code neither discovers services nor installs sinks.
`SaveStageObservation` uses the existing Foundation dispatcher, bounded metric
series and privacy-safe structured logs. Disabled/unregistered scopes return
before clocks, context capture, formatting or allocation. Evidence collection
that scans bounded owner state is gated by `IsActive` as well.

The closed stage taxonomy is capture, encode, migrate, commit, restore, sync,
queue, recovery and participant. Stage outcomes, duration, byte count, retries,
dropped work and queue depth are observational; authoritative typed Results,
commit knowledge and completion callbacks retain their existing behavior.
Failed stages use common logging WARN delivery, including its bounded emergency
reporting when the normal queue rejects a record. Save never adds producer retries,
flushes or drain waits. Low-severity logs, spans and metrics retain the Foundation
best-effort/drop accounting contract. Operation terminal records are attempted
once after the lifecycle lock is released, including cancellation, abandonment
and shutdown. Optional observation exceptions use the common bounded emergency
logger with fixed text and no private context. They do not invent a duration
for work that never ran. Metrics use only stage/outcome dimensions. Correlation
uses safe numeric operation and parent IDs in an isolated diagnostic snapshot;
paths, display names, accounts, payloads and raw provider/error text are excluded.
JobSystem and retained PlatformRequest contexts preserve this snapshot boundary.

Current production hooks cover participant capture, archive finalization,
migration execution, slot commit/recovery, staged restore preparation, arbiter
admission and storage jobs. The sync event contract is available for explicit
application/profile coordinator work and is tested across platform request
completion. This tree has cloud revision/request contracts but no live
CloudSaveCoordinator transfer/retry implementation; live cloud transfer timing,
retry and completion instrumentation remains unproven until that owner exists.
Local storage work is never labelled successful cloud synchronization.

Application host composition accepts at most eight capture-free diagnostic summary
providers. During user-requested export, after flushing the existing log dispatcher,
the Save provider derives `save.summary` from the host's retained canonical JSONL
inputs. It creates no second event store. Input work is bounded to 32 sources,
2,048 rows, 4,096 bytes per row and 8 MiB total; malformed, partial, oversized or
missing input is represented explicitly. Only closed stage/outcome names and
validated unsigned aggregates enter the summary; raw rows and source paths are
never copied. The summary describes retained observations, not lifetime totals;
queue/dispatcher drops and retention can limit evidence.

## Qualification And Implementation Status

The following are required implementation tests, not test suites delivered by this
documentation change:

- Authoring/recovery/workspace/account isolation, PIE namespace separation and
  headless exclusion of UI/client profile data.
- Product/environment/user/profile collision tests, all three platform-user
  capabilities, guest import, sign-out and restart with no display name/path identity.
- Profile-switch races before/after commit, stale completion fencing, catalog-open
  failure, production/development/PIE/server separation and UI/cloud path opacity.
- Duplicate/renamed display names, category reclassification, quicksave/autosave
  aliases and stable slot identity across overwrite.
- Capture one tick across Scene/subsystem adapters/world deltas; resume live mutation immediately
  after the safe point; race scene replacement, COW exhaustion and stale readbacks.
- Byte fixtures for the 32-byte preamble, unsigned/signed trailer lengths, exact hash
  ranges and signature message. Tamper header, lengths, compressed bytes, raw chunks,
  trailer, algorithm/key ID and trailing data; verify no self-reference or post-commit edit.
- Cross-platform/compiler golden canonical-state vectors, map ordering, negative zero,
  non-finite policy and proof that timestamps/display/compression/generation do not
  change `CanonicalStateHash` while covered archive-byte changes do change
  `ArchiveContentHash`.
- Required signature stripping, wrong project/account/world, untrusted key, signer
  timeout, malformed inputs, decompression bombs and unsupported schema versions.
- Every local/imported/cloud/migrated/modded/server-signed source enters as untrusted;
  catalog location, provider authentication and inspection success never skip
  integrity, scope, compatibility or semantic validation.
- Per-operation byte/count/string/nesting/decompression/work/disk budgets; hostile
  archives cannot trigger unbounded retry, diagnostic output or automatic budget
  escalation.
- Inspect/import/export/migrate/load/delete/conflict capability separation across
  UI, CLI and MCP, including shipping remote denial and no raw state/path/credential
  leakage.
- Snapshot admission versus eventual worker failure, per-operation sticky outcomes,
  cancellation races on both commit gates and no UI-thread filesystem calls.
- Disk-full/flush/rename/directory-sync fault injection, especially rename-success plus
  sync-failure; process-crash recovery and no false old-file-preserved guarantee.
- Restore all required participants from isolated candidates; fail each prepare stage;
  prove no observer sees partial Scene/inventory/quest publication. Validate no-fail
  commit and asynchronous retirement under native fence delay.
- Save/restore unloaded/resident/evicting cells, dropped-item/tombstone/cross-cell cases,
  bounded spill chunks, stale generations and changed base dataset revisions.
- Save/restore baked-relative durable foliage spawn/update/tombstone deltas with one-copy
  active/dormant capture, ephemeral/native exclusion and no-loss eviction failure.
- Independent archive/save/participant migration paths, minimum support horizon,
  current/oldest/one-newer fixtures, stale consent and signed migration without
  signing authority; unchanged source on pre-publication failure.
- Repeated equivalent-state publications with distinct `SlotGenerationId` values,
  replica preservation, parent-generation compare-and-swap and timestamp skew.
- Slot ID/path attack cases, symlink/reparse races, active-temp cleanup exclusion,
  other-process mutation, named-slot import and signed conflict-copy identity mapping.
- Busy autosave coalescing, bounded retry exhaustion, rotation only after success,
  cloud conditional upload/download conflicts, offline lineage branches, clock skew,
  stale retries and crashes at journal/provider/local-publication boundaries.
- Same/ancestor/divergent/unknown/same-generation-different-hash classification;
  KeepLocal/KeepRemote/KeepBoth/Defer preserve both verified conflict archives and do
  not perform semantic record merge.
- Shutdown while a save is past commit, while candidates hold modules/assets, and
  while optional GPU thumbnails are pending; no early free or silent incomplete unload.

The v1 container's entry-table codec, participant/session commit integration, secure
storage adapter and platform-specific durability qualification are implementation
work under these contracts. They must be completed before enabling runtime saves;
this specification does not claim that current Foundation/Scene APIs alone implement
all guarantees. Any public API/format implementation must include its own migration
and regression coverage.

## Related Documents

- [Scene Runtime](./scene-runtime.md): Candidate preparation, runtime identities and lifecycle commit.
- [Runtime Lifecycle](./runtime-lifecycle.md): Host frame phases and owner boundaries.
- [World Streaming](./world-streaming-architecture.md): Residency, generation fences and provider retirement.
- [VFX And Particles](./vfx-and-particles-architecture.md): Deferred renderer resource lifetime.
- [Editor Document Model](../editor/editor-document-model.md): Authoring and recovery authority.
- [Project Model](../editor/project-model.md): Project/workspace persistence.
- [Platform Services](./platform-services-architecture.md): User namespaces and cloud adapters.
- [Gameplay Module Boundary](../extensions/gameplay-module-boundary.md): Versioned provider contracts.
- [Concurrency And Jobs](../foundation/concurrency-and-jobs.md): Worker/owner completion ownership.
- [ADR-010](../../adr/010-job-waiting-and-operation-store-ownership.md): OperationStore and non-blocking work.
- [ADR-012](../../adr/012-world-streaming-partition-authority-and-subsystem-boundaries.md): Cell authority and barriers.
- [ADR-136](../../adr/136-platform-offline-queue-ownership-replay-and-cloud-intent-boundary.md): Platform offline queue ownership, replay guarantees and Save-owned cloud intent.
- [ADR-140](../../adr/140-foliage-placement-baked-dynamic-state-and-eviction-ownership.md): Foliage baked/ephemeral/durable classification, canonical deltas and eviction handoff.
- [ADR-008](../../adr/008-error-model-exception-boundary-and-registry.md): Typed error propagation.
- [ADR-112](../../adr/112-save-archive-container-and-compatibility-policy.md): Portable container, canonical state, version axes, identities and compatibility horizon.
- [ADR-113](../../adr/113-local-storage-user-profile-and-slot-ownership.md): Product/user/profile namespaces, logical slot addressing and physical storage ownership.
- [ADR-114](../../adr/114-canonical-runtime-world-persistence-boundary.md): Authoring/runtime composition, state classification and subsystem-owned canonical adapters.
- [ADR-115](../../adr/115-cloud-save-authority-revision-and-conflict-policy.md): Local/cloud authority, provider CAS revisions, offline lineage and conflict preservation.
- [ADR-116](../../adr/116-save-data-threat-model-and-trust-policy.md): Save-source trust classification, bounded admission, tool capabilities, credentials and profile policy.
- [Application Security](../security/application-security.md): Trust and untrusted-input boundaries.

Schema-2 opaque preservation uses `ValidatedSaveSceneCanonicalPreservation`, an
owned, non-fabricable Save authority minted only after authenticating the entire
required canonical layout and state hash. Borrowed-reader storage is rejected;
accepted proof values pin immutable archive backing. Raw hash/copy work is charged
on the shared reader ledger before materialization. Owner recognition in a later
release never grants a decoder for an authenticated Opaque record. Required and
Known records keep actual supported decode/hash verification; the generic reader
inspection and round-trip APIs remain strict. Content reconciliation and re-save
use the same qualified source/candidate path and compare exact stored bytes and
directory metadata. This path never permits dropping retained optional records.

## Gameplay Checkpoint Capture And Restart API

`GameplayCheckpoint` is an immutable retry source owned by the host. `Capture`
invokes the same bounded `RuntimeSaveCaptureBuilder` and participant adapters as
ordinary saves at the host's exclusive capture safe point. A transient checkpoint
retains immutable canonical payloads and exact registry, runtime session and Scene
incarnation evidence. It is a process-local rollback point and never claims a
durable slot. Capture requires an inert project payload descriptor identifying a
mandatory capture/restore participant, exact schema and owned record.

Checkpoint identity, spawn anchor, restart context, project, world, base scene,
product compatibility and baseline digest are stable typed facts. Display labels
are advisory and may be localized or duplicated; invalid presentation is omitted.
The project participant encodes checkpoint metadata and spawn/restart semantics in
its canonical checkpoint record so that durable reload can recover those facts.
It applies those semantics to its inactive candidate during the normal restore
phases, rather than calling gameplay after live publication.

`CommitDurable` accepts the trusted publication returned by the ordinary slot
commit of that exact snapshot. It requires Checkpoint slot kind, matching checkpoint
identity, base scene and product compatibility. `OpenDurable` reopens metadata
already decoded from a verified committed archive. Durable sources retain exact
slot/generation metadata and do not retain a process-local snapshot. Host composition
owns namespace binding and proves the captured-state/publication relationship;
these APIs perform no storage I/O or ambient backend discovery.

`GameplayCheckpointController::Activate` selects a retry source without mutating
live gameplay. Incompatible replacement leaves the previous selection intact.
`Restart` revalidates baseline, project payload descriptor and transient lifetime,
then asks the trusted ordinary slot-load composition to stage its source. Durable
staging must pin the exact namespace/slot generation, verify archive integrity and
compatibility, and compare canonical checkpoint metadata before returning receipts.
A stale publication must fail; it may not silently load the newest slot generation.
The returned receipts are adopted and prepared by `StagedRestoreTransaction` with
its deterministic validation, dependency plan, rollback and cancellation behavior.
The host publishes only by calling that transaction's `Activate` with fresh runtime
and registry evidence under the exclusive lifecycle commit boundary. No checkpoint
path bypasses the ordinary restore transaction or performs post-publication spawn
callbacks. Controller access is confined to its constructing thread.

Checkpoint API input migration: `Capture` borrows its read-only provenance and
limits for the synchronous builder call; the builder owns those values before
participant callbacks. `Restart` borrows context only to copy it into owned
operation evidence before any staging callback. Existing call expressions remain
valid; callers retaining function/member pointers must update these parameter
types and rebuild. Runtime checkpoint regressions and the staged public-header
consumer qualify these signatures. Archive formats and metadata are unchanged.
