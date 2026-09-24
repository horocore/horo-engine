# Platform Services Architecture

## Purpose

This document defines how Horo Engine integrates with platform-specific online
services such as achievements, leaderboards, cloud save, user presence, and
friends. These services are unlike audio, rendering, or physics: they are
asynchronous, network-bound, user-session dependent, and deeply tied to
closed platform SDKs and certification requirements.

The goal is a single backend-neutral frontend that gameplay code can use
without caring which platform the game is running on, while the actual
platform SDK implementations live in separate, optionally loaded backends.

[ADR-130](../../adr/130-platform-services-frontend-request-lifetime-timeout-null-and-error-semantics.md)
is the normative baseline for frontend-owned request records, admission, state and
result publication, deferred callback dispatch, capability truth, Null behavior,
timeout and provider-error normalization. Later service decisions specialize this
contract rather than creating another request lifecycle.

[ADR-131](../../adr/131-platform-services-closed-sdk-extension-abi-package-and-composition-boundary.md)
keeps proprietary SDKs in separately built private packages, assigns discovery/trust/
load/selection to distinct owners, constrains asynchronous provider exchange to a
versioned C ABI and freezes explicit composition for development, headless, test,
unsupported and certification profiles.

[ADR-132](../../adr/132-platform-services-project-salt-stable-id-tombstone-and-provider-mapping.md)
defines the one committed project identity ledger, byte-exact SHA-256 ID derivation,
canonical encodings, permanent tombstones, key aliases, clone/fork migration and the
private provider-mapping boundary used by all service categories.

Implementation status on 10 September 2026: PLS-003.2 publishes the common
`PlatformStableIdRegistry` contract in `HoroEngine::PlatformServices`. Detached
candidates are bounded and validated before immutable publication; schema-v1 IDs use
the exact project-salted ADR-132 derivation, typed service lookup, canonical salt/ID
text, global collision detection, per-kind primary/alias uniqueness and permanent
tombstones. Deterministic registry fingerprints fence opaque provider mapping evidence
to one complete registry and mapping revision. Provider-native values remain private:
the public validator receives only SHA-256 evidence and never treats it as Horo
identity. JSON loading, atomic project mutation, fork migration planning and concrete
provider manifest codecs remain with their Project, migration and adapter owners.

[ADR-133](../../adr/133-platform-progression-authority-trust-and-idempotency.md)
separates gameplay facts from remote projection, selects local or server authority per
definition and makes retry/replay depend on typed mutation algebra plus qualified
provider deduplication/atomicity rather than a blanket “idempotent write” assumption.

[ADR-134](../../adr/134-cloud-blob-transport-revision-precondition-and-offline-ownership.md)
ratifies Platform Services as authenticated opaque-object transport: complete
revision-consistent reads, conditional atomic mutations, finite quota/transfer limits,
whole-blob integrity and partial/ambiguous outcomes while the ADR-115 coordinator stays
the only durable cloud-intent owner.

[ADR-135](../../adr/135-platform-identity-session-generation-privacy-and-consent.md)
separates provider account, live subject capability, local profile, gameplay identity
and presentation data; every user-scoped commit is fenced by session/access generation
and every consent/restriction decision is enforced by a typed capability snapshot.

[ADR-136](../../adr/136-platform-offline-queue-ownership-replay-and-cloud-intent-boundary.md)
makes the Platform Offline Queue the single durable owner for eligible progression and
opted-in expiring presence intent. Durable acceptance precedes provider submission,
while cloud upload/delete remains exclusively in Save's coordinator journal.

## Scope

Platform services covered here:

- Achievements / Trophies
- Leaderboards and persistent stats
- Cloud save (platform-managed user save sync)
- User presence / rich presence
- Friends and social graph read access
- Platform user identity and session

Deliberately out of scope:

- Audio backends (`audio-architecture.md`)
- Input haptics such as PS5 adaptive triggers (`input-architecture.md`)
- Local save files and serialization (game save system)
- In-app purchase and DLC entitlement validation (owned by release/DRM flow)
- Platform OS abstractions such as filesystem, window, or process
  (`platform-abstraction.md`)
- XR loader/runtime/system/session/device/capability and mixed-reality permission state
  ([XR Architecture](./vr-ar-architecture.md) and
  [ADR-157](../../adr/157-xr-ownership-runtime-composition-and-capability-tier.md));
  Platform Services account identity or presence never grants XR authority or consent

[ADR-162](../../adr/162-mixed-reality-ownership-privacy-and-capability-tier.md)
also keeps passthrough, anchors, environment geometry, hit tests, light estimation and
their consent/access generations outside Platform Services. A future shared/cloud anchor
requires a separate trust, account-mapping and sharing contract; ordinary account login,
presence or cloud-save availability does not provide one.

## Core vs Modular

Platform services are **modular**, not part of the engine core.

The core engine must never link against Steamworks, PSN, Xbox Live, Nintendo
Online, or any other closed platform SDK. Those SDKs:

- are under NDA and cannot live in the public engine repository
- differ wildly in capability, size, and certification rules
- may not exist for headless, test, or prototype targets
- change on platform-holder timelines, not engine timelines

What the core provides:

- `IPlatformServicesBackend`: a narrow interface contract
- `PlatformServicesFrontend`: a backend-neutral, async API
- stable ID registries for achievements, leaderboards, and stats
- a null backend for tests and headless builds
- request queuing, offline buffering, and retry policy

Concrete backends live in separate platform packages such as
`horo-platform-steam`, `horo-platform-ps5`, `horo-platform-xbox`, and are
loaded by the runtime like any other extension package.

No public/core target includes, links, delay-loads, fetches or generates from a closed
SDK. Public CI builds the frontend, Null, Mock and ABI conformance fixtures without SDK
paths. A private provider package owns SDK initialization, native objects/callbacks,
credentials, error translation and certification policy and exposes only the Horo
`platform.services.provider` C ABI profile.

## Provider Package And Composition Boundary

Provider discovery reads only verified `.horopkg` install records and inert manifests;
it never probes PATH or loads candidates to discover capabilities. Package/Trust
services resolve integrity, signature, permissions, license and enablement. ExtensionHost
then loads the exact target binary, negotiates ABI and creates a candidate without
mutating live capability state. The application composition root validates the copied
Horo capability/limit snapshot and selects exactly one provider generation or explicit
Null before frontend admission opens.

The transaction is:

```text
verified package graph
  -> exact module/entry and ABI negotiation
  -> private SDK candidate initialization
  -> copied capability/limit/error descriptor validation
  -> product-policy selection and frontend reservation
  -> atomic provider generation publication
```

Failure before publication revokes candidate sinks, destroys partial SDK state,
unloads the candidate and releases package leases in reverse order while preserving an
old active generation. Replacement prepares the new candidate at full overlap, closes
and drains the old ADR-130 frontend generation at a safe point, then unloads old code
only after native operations and callback epochs retire. Timeout retains the module
lease rather than force-unloading code with possible callbacks.

The versioned C ABI carries fixed-width values/enums, sized structs, canonical Horo
IDs, call-borrowed byte/string spans, host-owned bounded sinks, adapter-scoped context/
operation tokens and Horo request/result/error envelopes. It carries no STL/C++ object,
exception, allocator ownership, retained borrowed span, SDK/native handle, credential,
raw account identity or unrestricted provider payload. Context/operation tokens remain
inside the private host adapter and never become frontend/gameplay identity.

Provider callbacks translate native state inside the package and copy bounded Horo
completion into a host sink tagged with provider generation. They never call gameplay
or observers, retain sink buffers or publish after sink revocation. Both sides catch
exceptions at the ABI edge; ABI status represents call validity, while valid operation
failure uses the ADR-130 typed result envelope.

Composition policy is explicit:

- development/editor may use one locally trusted exact provider or Null;
- tests use Mock/Null and public ABI fixtures;
- headless defaults to Null unless a server-capable provider is explicitly manifested;
- unsupported targets report typed optional unavailability or fail required startup,
  never load a closest variant; and
- certification builds freeze package/module hashes, ABI, SDK runtime, entitlement,
  capability and selection in the signed product manifest, with marketplace/update,
  arbitrary local providers, Mock and development fallback excluded.

Gameplay uses only narrow Horo frontend capabilities. It cannot discover/select a
provider, invoke the extension ABI or observe SDK types/tokens. Provider mapping and
SDK evolution remain private; new gameplay semantics require a versioned public Horo
contract rather than a native property escape.

## High-Level Architecture

```text
Gameplay / UI code
        |
        v
PlatformServicesFrontend
  (Achievements, Leaderboards, CloudSave, Presence, Friends)
        |
        v
IPlatformServicesBackend (loaded extension)
        |
        +-- SteamBackend
        +-- PSNBackend
        +-- XboxLiveBackend
        +-- NintendoOnlineBackend
        +-- NullBackend
        +-- MockBackend (tests)
```

Gameplay code calls the frontend. The frontend routes the call to the active
backend, which is selected at process composition time based on the target
platform and available packages.

## Frontend Contract

All platform service calls are asynchronous. The frontend never blocks the
calling thread on network I/O.

Implementation status on 10 September 2026: PLS-001.2 provides the standalone
`HoroEngine::PlatformServices` target and the typed `PlatformRequestStore` foundation.
It owns bounded active/terminal records, generation-fenced move-only handles, immutable
typed terminal snapshots, idempotent cancellation intent, and deferred at-most-once
`OnComplete` subscriptions. Provider routing, the SDK evidence queue, timeout policy,
provider cancellation, normalized provider errors, and host-wide frontend composition
remain the later PLS-001.3 through PLS-001.8 slices; callers must not treat the request
store as a provider backend or bypass those owners.

PLS-001.3 adds the generation-fenced `PlatformServicesFrontend` routing boundary.
It retains a strong backend lease plus copied capability/session snapshots, exposes
only Horo request types and public finite limits, and rejects closed, policy-denied,
unavailable/Null, stale-session, malformed-ID and over-bound requests before backend
invocation. Routing and idempotent `Close` are serialized by the composition owner;
`Close` publishes closed admission before invoking backend shutdown. The completion
queue and provider-to-frontend request-store handoff remain PLS-001.4 scope.

```cpp
template <typename T>
class PlatformRequestHandle {
public:
    PlatformRequestId Id() const;
    PlatformRequestGeneration Generation() const;
    bool IsValid() const;
};

class PlatformServicesFrontend {
public:
    Result<PlatformRequestHandle<void>> UnlockAchievement(AchievementUnlockRequest request) const;
    Result<PlatformRequestHandle<void>> SubmitScore(LeaderboardScoreRequest request) const;
    Result<PlatformRequestHandle<void>> WriteStat(StatWriteRequest request) const;
    Result<PlatformRequestHandle<CloudReadResult>> ReadCloudObject(CloudReadRequest request) const;
    Result<PlatformRequestHandle<void>> WriteCloudObject(CloudWriteRequest request) const;
    Result<PlatformRequestHandle<void>> SetPresence(PresenceUpdateRequest request) const;
    Result<PlatformRequestHandle<void>> ClearPresence(PlatformSubjectHandle subject) const;
    Result<PlatformRequestHandle<FriendsPage>> QueryFriends(FriendsQuery query) const;
    Result<PlatformRequestHandle<PlatformSessionSnapshot>> QueryCurrentSession() const;
    Result<PlatformServiceLimits> ServiceLimits(PlatformServiceKind service) const;
    Result<void> Close();
};
```

The query, subscription and cancellation surfaces remain owned by the request store
until the PLS-001.4 completion handoff connects admitted frontend handles to that
store; they are not frontend methods in this slice.

Pre-admission validation, permission, lifecycle, capability, session and bounded-
capacity failure returns `Result` with no request record or provider call. Once admitted,
the frontend owns the request record independently of every handle and subscription.
Handles contain only typed request/frontend identity; they never own provider objects.

The authoritative snapshot state is exhaustive:

```cpp
enum class PlatformRequestState : uint8_t {
    Queued,
    Running,
    Cancelling,
    Succeeded,
    Failed,
    Cancelled,
    TimedOut
};
```

| From | Legal next states |
|---|---|
| `Queued` | `Running`, `Cancelling`, `Failed`, `TimedOut` |
| `Running` | `Cancelling`, `Succeeded`, `Failed`, `TimedOut` |
| `Cancelling` | `Succeeded`, `Failed`, `Cancelled`, `TimedOut` |
| `Succeeded`, `Failed`, `Cancelled`, `TimedOut` | none |

Exactly one terminal result publishes atomically with the terminal state. Success owns
one immutable value; Failed, Cancelled and TimedOut own one typed Error matching the
state. Non-terminal snapshots contain no terminal result.

### Request Lifetime

1. The frontend validates and either returns an admission error or atomically creates
   a `Queued` record with request, provider, session, policy and deadline generations.
2. Provider submission moves the record to `Running`; failure after admission commits
   `Failed` without inventing provider progress.
3. The provider performs asynchronous work and enqueues bounded generation-tagged
   evidence. SDK threads never mutate frontend state or invoke user code.
4. The frontend owner lane validates evidence and commits at most one terminal state/
   result before scheduling observer notification.
5. The record remains queryable under finite count/time retention after completion and
   until provider/callback retirement permits reclamation.

Dropping the last handle or subscription does not cancel an admitted request, erase a
write or free provider work. Explicit `Cancel(typedHandle)` or owner shutdown policy is
required. Querying expired or stale identity returns `platform.request.expired` or
`platform.request.stale`; it does not fabricate a replacement terminal result.

### Cancellation

`Cancel()` posts cancellation intent to the frontend owner. It never invokes the
provider or an observer inline. Cancellation is best-effort:

- An accepted queued/running request moves to `Cancelling` and closes new retries.
- Provider acknowledgement with no committed effect publishes `Cancelled` and
  `platform.request.cancelled`.
- A provider success already committed or completed despite cancellation may publish
  `Succeeded`; provider failure may publish `Failed`.
- Deadline expiry may still publish `TimedOut`. A terminal request cannot be cancelled
  retroactively.

Multiple `Cancel()` calls are idempotent. Cancellation intent is recorded separately
from the one final state; `Cancelled` does not promise rollback unless a service-
specific contract provides that guarantee.

### Threading

Completion callbacks run on an engine-controlled thread, never on a platform
SDK thread. Backends must not call frontend completion handlers directly; they
notify the frontend through an internal completion queue that the frontend
drains on an appropriate engine thread.

The authoritative observation API is snapshot query plus optional subscription.
Subscription returns a move-only RAII token and each token receives at most one
terminal notification. Destroying it prevents future delivery without cancelling the
request. Callbacks never run inline from submit, subscribe, cancel, provider callback,
queue drain/state mutation or while a frontend/provider lock is held. The frontend
commits terminal state first, then posts immutable request ID/terminal snapshot to the
declared engine executor for a later dispatch turn. Subscribing after retained terminal
completion schedules the same deferred delivery rather than invoking synchronously.

Gameplay code may submit from permitted threads through bounded ingress, but the
frontend serializes request mutation on its declared owner lane. Re-entering from a
later completion callback may enqueue new work; it cannot recursively drain completion
or change the published terminal record. Callback exceptions are caught at the engine
executor boundary and do not change the platform request result.

### Timeouts, Retry, And Throttling

The frontend captures one finite monotonic deadline at admission. Valid completion
evidence observed no later than the deadline is processed before deadline evaluation.
Otherwise the frontend commits `TimedOut` with `platform.request.timed_out`, requests
best-effort provider cancellation and closes caller-visible completion. Later native
completion is retirement/reconciliation evidence only and cannot replace the result.
Provider resources remain alive until acknowledged safe; timeout is not proof of
physical cancellation.

Retry is controlled by the owning frontend/service policy, not gameplay code, and all
attempts fit within the original request deadline:

- Only ADR-133 operations whose declared algebra and effective provider/gateway
  capability prove retry safety may be retried.
- Every attempt preserves the exact progression mutation ID, payload, subject,
  authority/policy/session/provider generations and precondition. Timeout never
  allocates a replacement logical mutation.
- Reads and queries receive a bounded in-memory retry only when project policy allows;
  they never become durable progression replay intents.
- When the backend reports normalized `Offline` or `NotSignedIn`, retriable writes move
  to the PLS-007 durable owner only when their ADR-133 replay class and captured
  subject/session are eligible.

High-frequency writes are throttled and coalesced:

- maximum/minimum progress or stat intents may reduce to the strongest value only
  within one subject/policy generation;
- `SubmitBestScore` may reduce to the mathematical best under the definition's
  declared ordering, never the most recent arrival;
- exact duplicate progression mutation IDs join one logical intent; and
- conditional snapshots/replacements and `AddStatOnce` are never coalesced.

Every accepted logical mutation retains an individual receipt/outcome even when a
result-preserving aggregate uses one provider operation. Arrival or provider completion
order never becomes stat/score conflict policy.

Throttling metrics:

```text
platform.stats.coalesced_writes     -- stat writes merged by frontend
platform.leaderboards.deferred_submits -- submissions delayed by debounce
```

### Result And Errors

Submission and terminal snapshots use typed `Result`/`Error` contracts. Errors are
normalized so gameplay code does not need platform-specific error handling:

```text
platform.frontend.unavailable       -- frontend not active for admission
platform.capability.unavailable     -- selected real provider lacks the service
platform.provider.null              -- explicit Null composition; no operation accepted
platform.request.capacity_exceeded  -- bounded admission/observation capacity exhausted
platform.request.cancelled          -- acknowledged cancellation terminal outcome
platform.request.timed_out          -- frontend deadline terminal outcome
platform.request.expired            -- terminal observation retention ended
platform.request.stale              -- request/frontend/provider/session generation mismatch
platform.provider.failed            -- admitted provider operation failed
platform.progression.authority_denied -- fact lacks the definition's required authority
platform.progression.idempotency_conflict -- mutation ID reused with another envelope
platform.progression.unsafe_retry   -- algebra/provider cannot safely retry ambiguity
platform.offline.ineligible         -- operation/provider semantics prohibit durability
platform.offline.durable_unavailable -- queue storage cannot provide durable acceptance
platform.offline.capacity_exceeded  -- finite queue/partition/byte budget exhausted
platform.offline.storage_unknown    -- queue publication outcome requires recovery
platform.offline.expired            -- finite replay age ended without remote success
platform.offline.reconciliation_required -- remote outcome cannot safely auto-retry
platform.offline.abandoned          -- authorized audited stop with no false rollback
```

`platform.provider.failed` carries one normalized category: `Offline`, `NotSignedIn`,
`Forbidden`, `RateLimited`, `PreconditionFailed`, `QuotaExceeded`, `InvalidResponse`,
`TransientFailure` or `PermanentFailure`. Bounded redacted native codes may appear as
diagnostic/cause evidence but never as branching identity. Capability absence, Null,
cancellation and timeout are never remapped into provider failure.

### Ordering And Coalescing

Independent requests may complete out of order. Dependent operations are chained by
their owning coordinator. In particular, cloud mutation always carries the exact
provider revision returned by the reconciled read/list (or create-if-absent); generic
callers cannot turn a read completion into an unconditional write.

Presence updates are coalesced. If `SetPresence` is called many times before
the backend finishes the previous update, only the most recent state is sent.

## Service Categories

### Achievements

Achievements are authored once in project configuration and cooked into
platform-specific manifests. Each definition uses an ADR-132 `AchievementId`, typed
progress schema and immutable `LocalProduct` or `AuthorityServer` policy. Display text
is presentation and provider names remain mapping data.

```cpp
struct AchievementDefinition {
    AchievementId id;
    ProgressionAuthorityMode authority;
    AchievementProgressSchema progress;
    LocalizedAchievementPresentation presentation;
};

using AchievementMutation =
    std::variant<UnlockOnce, SetProgressMaximum>;
```

Implementation status on 10 September 2026: PLS-003.3 publishes the typed
`AchievementDefinitionRegistry` contract in `HoroEngine::PlatformServices`. A detached
complete candidate is bound to one ADR-132 project and registry fingerprint, validates every active
achievement exactly once, canonicalizes by unsigned stable ID and publishes an immutable
semantic fingerprint. Localization keys and progress totals are explicitly bounded and
malformed fields carry document-path diagnostics. Ordinary replacement may change
presentation but rejects authority/progress changes and definition removal until the
stable ledger retains the corresponding tombstone. Provider mappings stay in the
opaque ADR-132 mapping pipeline; SDK types and raw account identifiers do not enter the
definition snapshot.

Gameplay submits a committed typed fact through `IProgressionIntentSink`; it never
calls a provider service. The product progression router validates the definition,
value and authority. Server-mode clients send ordinary gameplay input/commands; the
server gameplay owner derives the achievement fact and its server-only progression
commit capability submits it. A client cannot send a privileged “unlock” conclusion.

`UnlockOnce` makes unlocked true and is intrinsically idempotent only after provider
qualification proves repeated unlock has the same effect. `SetProgressMaximum` is
monotonic and retryable only when the provider/gateway atomically implements maximum-
or-equal. Reset/decrement is not a runtime operation. The remote platform owns its
account projection; querying that projection does not make it trusted gameplay state.

### Leaderboards And Stats

Leaderboards are also authored once and mapped to platform backends at cook
time. Definitions fix typed numeric domain/range, score ordering, mutation algebra and
local/server authority before runtime composition.

```cpp
enum class ProgressionMutationKind : std::uint8_t {
    UnlockOnce,
    SetProgressMaximum,
    SetStatMaximum,
    SetStatMinimum,
    SetStatSnapshot,
    AddStatOnce,
    SubmitBestScore,
    ReplaceScoreAtRevision,
};

struct ProgressionMutationEnvelope {
    ProgressionMutationId mutation;
    ProgressionSubjectScope subject;
    ProgressionDefinitionId definition;
    ProgressionMutationKind kind;
    ProgressionValue value;
    ProgressionPrecondition precondition;
    ProgressionAuthorityEvidence authority;
    ProgressionPolicyRevision policy;
};
```

The correct semantic authority allocates one nonzero 128-bit
`ProgressionMutationId` when it first accepts a logical fact. The exact canonical
envelope follows that intent through retries, durable replay and reconciliation. An
exact duplicate joins the existing intent; reuse of the ID with different fields is
`platform.progression.idempotency_conflict`. A key is deduplication identity, not
authentication.

Maximum/minimum stats and best-score submissions retry only with matching atomic
provider semantics. Snapshot/score replacement requires an exact opaque revision and
atomic precondition. `AddStatOnce` requires durable mutation-ID dedupe in the provider
or trusted gateway. Horo never emulates these with read-then-unconditional-write. A
provider without the required primitive reports the operation unavailable rather than
silently weakening it.

Timeout or transport loss after submission can leave `RemoteOutcomeUnknown`. The
original ADR-130 request remains `TimedOut`; safe algebra may resubmit the same
envelope, conditional operations query/reconcile revision, and an unsafe non-deduped
operation stops automatic retry. Late provider evidence may resolve coordinator
reconciliation but never rewrites the terminal request or invokes observers twice.

Stat and leaderboard queries return bounded provider/account projections at a captured
generation/revision. Leaderboard order is provider-owned under the cooked definition.
These values support UI and local hints; clients cannot use them as authority for
shared economy, rewards, simulation or access control.

Implementation status on 10 September 2026: PLS-003.4 publishes immutable typed
`StatDefinitionRegistry` and `LeaderboardDefinitionRegistry` snapshots in
`HoroEngine::PlatformServices`. Each complete candidate is fenced to one ADR-132
project/ledger fingerprint, canonicalized by unsigned stable ID and assigned a
deterministic semantic fingerprint. Definitions fix authority, signed/unsigned integer
domain, inclusive range, stat mutation algebra and leaderboard ordering before cook.
An optional leaderboard-to-stat reference must resolve in the captured stat snapshot
with an identical numeric kind and a range covering every leaderboard score. Missing,
wrong-kind, incompatible, duplicate, tombstoned, stale, unbounded and incomplete input
fails before publication with a field-path diagnostic. Presentation may evolve during
ordinary replacement, while semantic changes or removal without an ADR-132 tombstone
require an explicit migration. Provider mappings remain opaque ADR-132 inputs rather
than definition fields.

### Cloud Save

Cloud save is authenticated transport of opaque complete objects. It is not a
replacement for the local save system and owns no local generation, archive parsing,
lineage, merge, winner, retention or conflict policy.

```cpp
struct CloudSaveObjectKey { BoundedOpaqueBytes value; };
struct CloudSaveObjectPrefix { BoundedOpaqueBytes value; };
struct CloudListCursor { BoundedOpaqueBytes value; };
struct CloudBlobOwnedBytes { BoundedArray<std::byte> value; };
struct CloudBlobSourceHandle { UInt128 value; };
struct ProviderObjectRevision { BoundedOpaqueBytes value; };
struct CloudBlobDigest { Sha256Digest value; };
struct CloudMutationId { UInt128 value; };

struct CreateIfAbsent {};
struct MatchProviderRevision { ProviderObjectRevision revision; };
struct CloudListRequest {
    CloudSaveObjectPrefix prefix;
    std::optional<CloudListCursor> cursor;
};

enum class CloudMutationAtomicity : std::uint8_t {
    ConditionalAtomicObject,
    UncoordinatedBlob,
};

struct CloudObjectHead {
    CloudSaveObjectKey key;
    ProviderObjectRevision revision;
    std::uint64_t sizeBytes;
    std::optional<CloudBlobDigest> transportDigest;
    OptionalProviderTimestamp modifiedForPresentation;
};

struct CloudObjectPage {
    BoundedArray<CloudObjectHead> objects;
    std::optional<CloudListCursor> next;
};

struct CloudBlobReadRequest {
    CloudSaveObjectKey key;
};

struct CloudBlobReadResult {
    CloudObjectHead head;
    CloudBlobOwnedBytes bytes;
};

struct CloudBlobReadSource {
    CloudBlobSourceHandle source;
};

struct CloudMutationResult {
    CloudSaveObjectKey key;
    CloudMutationId mutation;
    std::optional<CloudObjectHead> committedObject; // absent after successful delete
};

using CloudWritePrecondition =
    Variant<CreateIfAbsent, MatchProviderRevision>;

struct CloudBlobWriteRequest {
    CloudSaveObjectKey key;
    CloudBlobReadSource source;
    std::uint64_t exactSizeBytes;
    CloudBlobDigest expectedDigest;
    CloudWritePrecondition precondition;
    CloudMutationId mutation;
};

struct CloudBlobDeleteRequest {
    CloudSaveObjectKey key;
    ProviderObjectRevision expectedRevision;
    CloudMutationId mutation;
};

class ICloudBlobTransport {
public:
    virtual Result<PlatformRequestHandle<CloudObjectPage>>
    List(CloudListRequest request) = 0;
    virtual Result<PlatformRequestHandle<CloudBlobReadResult>>
    Read(CloudBlobReadRequest request) = 0;
    virtual Result<PlatformRequestHandle<CloudMutationResult>>
    Write(CloudBlobWriteRequest request) = 0;
    virtual Result<PlatformRequestHandle<CloudMutationResult>>
    Delete(CloudBlobDeleteRequest request) = 0;
};
```

Under ADR-113, gameplay and UI never create `CloudSaveObjectKey` from a string. The
coordinator derives it from the typed product/environment/profile/slot address and
private authenticated provider-user context. It is not a path, display name, raw
account ID or erased `SaveGameSlotId`. `ProviderObjectRevision` is bounded opaque bytes
scoped to one provider/session/key and compared only for exact equality; it is not
ordered, portable, a timestamp or a save generation.

The immutable cloud capability snapshot declares finite object/namespace bytes, object
count, key/revision length, list page, chunk and concurrent-transfer limits plus list/
read consistency, conditional create/replace/delete, durable mutation-ID dedupe,
digest/progress and cancellation behavior. Quota usage is advisory and can change
after observation; `QuotaExceeded` remains possible after preflight and cannot trigger
automatic deletion/truncation/splitting.

List is bounded/cursor-based. Because provider lists may not be snapshot-consistent,
the coordinator re-reads an exact key before deciding. Successful read returns one
complete immutable byte owner and `CloudObjectHead` for the same revision. Separate
metadata/byte APIs must revision-check around transfer; a change is
`platform.cloud.revision_changed`, never mixed success. Host-owned sinks enforce exact
length, checked chunk/byte bounds and SHA-256 transport digest before publication; no
partial span escapes.

Automatic mutation requires qualified `ConditionalAtomicObject`. Create requires
absence; replace/delete requires the exact current revision. Success atomically exposes
all new bytes or deletion and returns required revision/digest evidence. A stale value
is `PreconditionFailed`; the coordinator refetches/reclassifies and never retries
unconditionally. `UncoordinatedBlob` disables automatic write/delete while local
save/load and explicit policy-approved import/export remain usable.

Provider-native multipart/temp objects remain private and never become visible at the
public key. Failure/cancel/timeout exposes no partial result or success; staging cleanup
is best effort and keeps provider code leased until callbacks/parts retire. Upload
pulls bounded call-borrowed chunks from a coordinator-owned immutable archive lease;
download pushes to request-owned bounded staging. Progress is deferred observational
data and never proves commit.

`CloudSaveCoordinator` allocates and durably owns one `CloudMutationId`, exact payload
digest/size, precondition, generation/lineage, source lease, retry and reconciliation
state. Platform Services owns only an in-memory ADR-130 request and never writes cloud
upload/delete into the generic PLS-007 queue. Frontend retry preserves the exact
request inside its original deadline. Timeout/late commit remains an ambiguous durable
intent until the coordinator reads the key and recognizes matching content, retries
the still-uncommitted precondition or enters ADR-115 conflict handling.

Authentication, TLS, provider ownership and a matching transport digest do not make
archive bytes trusted. The coordinator still applies ADR-116 framing, archive hash,
signature/scope, compatibility, semantic, namespace and local lease/generation gates.
The provider cannot inspect save contents, select a trust root or reinterpret failure
as empty/not-found/older success.

### Presence

Presence is best-effort and non-critical. The frontend batches rapid presence
updates and coalesces them into the most recent state.

```cpp
struct PresenceState {
    PresenceStatusId status;
    std::optional<BoundedUtf8> detail;
    std::optional<PlatformPresenceArtId> largeImage;
    std::optional<PlatformPresenceArtId> smallImage;
};

class IPresenceService {
public:
    virtual Result<PlatformRequestHandle<void>>
    SetPresence(PlatformSubjectHandle subject, const PresenceState& state) = 0;

    virtual Result<PlatformRequestHandle<void>>
    ClearPresence(PlatformSubjectHandle subject) = 0;
};
```

`PresenceStatusId` and art IDs are project-owned typed identities resolved through the
ADR-132 registry/mapping pipeline. Optional detail is bounded untrusted presentation
data. Publication requires a current subject plus `PresencePublish` access, and the
captured session/access generations are revalidated before provider submission and
observable completion.

Implementation status for PLS-006.5: `PlatformPresenceCoordinator` resolves every set
status through the immutable presence-definition registry, enforces the registered
detail policy and valid UTF-8 bounds, and captures the exact subject/session/access
authority. It retains one latest-wins pending desired state, never replaces an
in-flight provider operation, applies a bounded publication interval, and invalidates
pending/in-flight state on sign-out, session/access replacement or close. Provider
adapters receive only a Horo-owned publication token and normalized completion result;
provider strings, native values and implicit status fallback are not representable.

PLS-003.4 also publishes the immutable `PresenceDefinitionRegistry`. Every active
presence-status identity has exactly one definition fixing whether free detail is
forbidden or optional and, when optional, its finite UTF-8 byte ceiling. Localization
and visibility remain presentation metadata. Deterministic registry construction,
stale-ledger rejection, tombstone-gated removal and transactional replacement mirror
the stat/leaderboard lifecycle; runtime session handles, raw account identifiers,
provider art/native values and implicit fallback are deliberately absent.

### Friends

Friends access is read-only in core. Invites and friend management UI are
platform-owned.

```cpp
struct PlatformSocialSubjectHandle {
    UInt128 nonce;
    ProviderGeneration providerGeneration;
    PlatformSessionGeneration sessionGeneration;
    PlatformAccessPolicyRevision accessRevision;
};

struct PlatformUserPresentation {
    PlatformSocialSubjectHandle subject;
    BoundedUtf8 displayName;
    std::optional<ProviderApprovedAvatarToken> avatar;
    PlatformPresentationSource source;
    PlatformPresentationFreshness freshness;
};

class IFriendsService {
public:
    virtual Result<PlatformRequestHandle<BoundedArray<PlatformUserPresentation>>>
    GetFriends(PlatformSubjectHandle subject) = 0;
};
```

Friends access requires explicit user consent and platform policy support.
Absence of the capability is typed, not a silent empty list. Social handles are
ephemeral, scoped to the requesting session/access revision and distinct from the
local subject capability. Display/avatar fields are bounded consent-scoped
presentation, never account, gameplay or durable storage identity.

## Stable ID Registries

Runtime and gameplay never pass strings for achievement, leaderboard, stat or
presence-status identity. All authoring keys resolve before cook publication to
strong typed 64-bit Horo IDs. The kinds are not interchangeable with each other,
runtime handles, provider identifiers or raw integers; `0` is invalid.

### Project identity authority

The Project domain owns one committed `.horo/platform_services.ids.json` ledger. It
contains all active and tombstoned primary entries and their authoring aliases. A
service-specific definition document owns display/localization and behavior fields and
references an ID from this ledger. Provider mapping documents and generated manifests
consume it; neither can allocate or rewrite a Horo ID.

The ledger header binds schema `1`, the exact owning `projectId` and algorithm
`horo.platform-services.stable-id.v1`. `.horo/project.json` is the sole authority for
the 128-bit `platformServicesIdSalt`, serialized as `psid1:` plus 32 lowercase hex
digits. The ledger does not duplicate the salt. It stores each derived numeric value
as `sid1:` plus exactly 16 lowercase hex digits, including leading zeroes. JSON numbers
and alternative spellings are rejected so 64-bit precision and canonical comparison
do not depend on a parser.

The salt is generated from the platform cryptographic random source together with a
new project. Zero, short reads and random-source failure fail project creation. The
salt is portable non-secret metadata committed with the project; environment, user
preferences, provider packages and build profiles cannot override it. A project that
predates the field obtains it once through a transactional migration, never as a
read-only-open or cook side effect.

### Canonical keys and deterministic derivation

Each primary definition has one immutable 1–96 byte ASCII `canonicalKey` matching
`^[a-z][a-z0-9_.-]{0,95}$`. The exact bytes are machine identity. They are not trimmed,
case folded, localized or derived from a display name. Presentation renames therefore
preserve identity.

Version 1 hashes exactly:

```text
ASCII("horo.platform-services.stable-id.v1")
0x00
project salt                                      (16 raw bytes)
kind                                              (uint8: achievement=1,
                                                   leaderboard=2, stat=3,
                                                   presence-status=4)
canonical-key byte count                          (uint16, big-endian)
canonical-key                                     (exact ASCII bytes)
```

Horo computes SHA-256, takes digest bytes 0–7 and interprets them as an unsigned
big-endian `uint64`. Cooked binary and the platform provider ABI also encode those
eight bytes big-endian. Implementations use the shared golden-vector codec; they never
use `std::hash`, host byte order or a provider hash.

The ledger retains the derived value beside its key. Validation always recomputes it,
rejects zero and rejects a stored/derived mismatch. Primary entries are canonically
ordered by kind tag and then unsigned ID; aliases use exact ASCII byte order. A
domain-separated SHA-256 of the canonical complete ledger is its snapshot fingerprint.

### Active, tombstoned and aliased identity

A primary entry is exactly `Active` or `Tombstoned`. Removing a definition preserves
its kind, canonical key, ID, aliases and bounded removal provenance as a tombstone.
Tombstones participate in key and numeric uniqueness forever under that project
namespace. Ordinary authoring, cook and cleanup cannot prune them or allocate their
identity to a new meaning.

Re-adding a tombstoned key fails. A separately reviewed restore operation may reactivate
the exact same semantics and ID only after provider mappings and product policy accept
it; it is never implicit. New semantics require a new canonical key.

An alias is another valid canonical-key spelling bound directly to exactly one primary
entry of the same kind. It has no independently hashed value. Aliases support explicit
authoring/import migrations while runtime/cooked references retain only the numeric
ID. Primary keys and aliases are unique per kind across active and tombstoned entries.
Chains, cycles, wildcard/case-insensitive lookup, cross-kind aliases and provider-native
values used as Horo aliases are forbidden. Tombstoning retains all aliases.

### Collision and candidate validation

Candidate validation builds one numeric index across every active and tombstoned
primary entry. Different primaries producing the same 64-bit value fail with
`platform.identity.hash_collision`, even across kinds. No entry wins. Diagnostics
name the algorithm, ID, both kind/key pairs and safe source locations.

A new unpublished entry resolves a collision by selecting and reviewing a different
canonical key. An existing published entry never changes. Incrementing the value,
rehashing with a counter, insertion-order probing or silently widening one ID is
forbidden because each would make results depend on authoring/merge order. Conflicting
published histories require a declared project migration and cannot merge by guess.

The application project service parses a bounded detached candidate and validates
project/salt ownership, schema/algorithm, canonical encoding/order, key/alias
uniqueness, stored derivations, global numeric uniqueness, states and all durable
references before atomically publishing one immutable snapshot. Failure preserves the
last valid snapshot and never partially mutates a provider registry.

### Clone, fork and regeneration

Filesystem copies, VCS clones/branches/checkouts, path moves and backups preserve
`projectId`, salt, ledger and IDs because they are working copies of the same product.
A Duplicate Project workflow must explicitly choose:

- `PreserveIdentity` for a branch/port of the same product; or
- `ForkIdentity` for an independent product, generating a new `projectId` and
  cryptographic salt and executing a complete namespace migration.

There is no path heuristic or automatic regeneration when a copy is detected. Salt
regeneration is not a normal setting. Fork/regeneration first produces a dry-run with
all active and tombstoned old-to-new IDs, aliases, durable references, provider
mapping dispositions, offline operations and derived artifacts. It validates the full
candidate before atomically updating project metadata, ledger and every owned durable
reference, then invalidates affected manifests/caches/offline entries. Unknown or
opaque references, incomplete mappings and collisions block with the old namespace
intact.

After an ID has shipped, entered remote state, saves/network protocols or another
package, regeneration is prohibited by default. It requires a separately approved
product migration with complete external transition evidence or an explicit
incompatible new-product reset. Text aliases cannot bridge two numeric namespaces.

### Provider mapping and cook/runtime projection

A trusted provider adapter owns its mapping keyed by `(ProviderId, kind, Horo ID)`.
Values such as Steam API names, trophy numbers and console manifest indexes remain
bounded provider-private data. Required active entries have exactly one mapping for
the selected target; optional absence is explicit product capability policy. Unknown,
tombstoned, duplicate, wrong-kind or stale-ledger mapping rows fail provider manifest
generation. Tombstoned remote values cannot be reassigned to new Horo definitions.

Cook captures the project, algorithm, ledger fingerprint, service definitions,
provider mapping revision and product/target profile as one generation. It resolves
keys/aliases once and emits typed IDs into provider-neutral artifacts; provider
manifests are deterministic derived outputs from that same snapshot. Any revision
change invalidates the candidate.

Runtime loads bounded sorted tables with the expected fingerprint and performs only
typed numeric lookup. It never hashes strings, reads editor aliases or asks a provider
to allocate identity. Registry generations remain pinned by admitted requests and
durable offline entries until retirement/migration. Provider callback reverse lookup
is private and immediately converts to the captured Horo ID. Gameplay, saves, scripts,
offline queues and public telemetry never persist provider IDs or raw display strings
as identity.

## Backend Interface

The backend is a capability bundle. A platform may implement all, some, or none
of the services.

```cpp
enum class PlatformServiceAvailability : uint8_t {
    Available,
    Unavailable
};

enum class PlatformServiceUnavailableReason : uint8_t {
    NoProviderSelected,
    NullProviderSelected,
    ServiceUnsupported,
    HostPolicyDenied,
    ProviderInitializationFailed
};

struct PlatformServiceCapability {
    PlatformServiceKind service;
    PlatformServiceAvailability availability;
    PlatformServiceLimits limits;
    std::optional<PlatformServiceBindingId> binding;
    std::optional<PlatformServiceUnavailableReason> unavailableReason;
};

struct PlatformServiceCapabilitySnapshot {
    ProviderGeneration providerGeneration;
    BoundedArray<PlatformServiceCapability> services;
};

class IPlatformServicesBackend {
public:
    virtual ~IPlatformServicesBackend() = default;

    virtual Result<PlatformServiceCapabilitySnapshot>
    Initialize(const PlatformServicesConfig& config) = 0;
    virtual Result<ProviderOperationId>
    Submit(const PlatformProviderRequest& request,
           PlatformProviderCompletionSink& completions) = 0;
    virtual Result<void> RequestCancel(ProviderOperationId operation) = 0;
    virtual Result<void> Shutdown() = 0;
};
```

The validated capability snapshot is the sole availability truth. `Available` has
exactly one compatible private binding; `Unavailable` has no binding and exactly one
reason. Missing, duplicate or mismatched bindings fail composition. Public callers do
not inspect provider pointers, backend names or SDK flags.

Implementation status on 10 September 2026: PLS-002.2 publishes the version-1
backend-neutral capability bundle and typed achievement, leaderboard/stat, cloud,
presence, friends and session interfaces in `HoroEngine::PlatformServices`. Candidate
inspection is inert; `ActivatePlatformServicesBackend` validates interface version,
provider/generation identity, exact service coverage, finite limits, binding/reason
coherence and required-service policy before invoking activation. Unavailable services
remain real interface methods that return typed failure, never nullable bindings.
Private SDK objects and ABI tokens remain outside this C++ surface under ADR-131.

Installed/provider capability and effective subject access are distinct immutable
snapshots; admission intersects them instead of collapsing them into a boolean.
Connectivity and rate-limit state remain dynamic request preconditions. Provider
reload, session replacement or access-policy change increments the applicable
generation/revision; stale completion evidence cannot publish into the replacement.

## Offline And Degraded Behavior

The ADR-136 `PlatformOfflineQueue` is the only durable owner for replay-eligible
progression and explicitly opted-in presence desired state. It stores canonical Horo
logical intent, not ADR-130 requests or provider calls. `DurableUntilTerminal` commits
the bounded record through qualified atomic storage before the first provider submit;
only then may the producer observe durable acceptance. `VolatileAttempt` is explicitly
process-lossy and never presented as queued.

Eligibility remains exhaustive:

- ADR-133 intrinsic/atomic monotonic/best operations may persist only when the current
  provider/gateway capability proves their exact semantics;
- conditional stat/score replacement retains and reconciles its exact revision;
- `AddStatOnce` or another non-intrinsic effect requires durable provider/gateway
  mutation-ID deduplication;
- presence may persist only as one consented, bounded and short-expiry latest desired
  `Set`/`Clear` state per subject/purpose lane; and
- reads, queries and cloud mutation are never admitted.

The queue preserves the original progression mutation or presence intent identity,
canonical payload, protected ADR-135 subject-binding partition, registry/policy/access
requirements, expiry and attempt policy. It never allocates identity on replay,
contains a live subject handle/raw provider identity or retargets the current account.
Exact duplicates join one row; different payload reuse is a conflict.

Durable states distinguish Pending, Dispatching, Reconciling, Suspended, Succeeded,
PermanentlyFailed, Expired, Superseded and Abandoned. Terminal disposition is committed
before observer success and later atomic compaction. A crash after remote commit but
before local completion therefore retains the original intent in Reconciling rather
than creating a fresh unsafe attempt. Corrupt/publication-unknown storage is
quarantined; startup never silently creates an empty queue.

Ordering is per `(subject partition, semantic definition/purpose lane)`, not global
FIFO. Conditional/non-commutative work serializes; ADR-133 monotonic/best operations
coalesce only when every receipt/result is preserved. Presence keeps the latest desired
state, but cannot erase an already ambiguous provider outcome. Unrelated lanes use
bounded fair concurrency.

Startup validates storage without provider calls. Replay resumes only after proving
the same subject binding and revalidating current session/access, authority/profile,
registry/mapping and provider capability. Sign-out/account switch suspends the old
partition. `AuthorityServer` records require a current server authority path; a client
cannot manufacture or upgrade them.

Attempt count, jittered backoff, record/byte/partition/in-flight capacity and expiry
are finite product policy within engine hard maxima. Expiry is a durable explicit
failure and never resets on retry/restart. Full capacity rejects admission instead of
dropping oldest or ambiguous work. Provider retry hints are bounded and clock anomalies
cannot extend retention indefinitely or produce a retry storm.

Expiry/attempt exhaustion applies only when no unresolved remote effect exists.
Reconciling ambiguity remains retained until semantic resolution or an explicitly
permissioned, audited Abandoned disposition; neither TTL nor cleanup can erase it.

Cloud archive writes/deletes are deliberately absent. Their immutable payload lease,
slot lineage, expected provider revision, retry identity and profile binding remain
durably owned only by ADR-115/134 `CloudSaveCoordinator`. Both owners may use stateless
deadline/backoff/error/atomic-storage helpers, but share no row, scheduler, outcome or
FIFO. Reconnect may wake both coordinators independently.

Shutdown closes admission/scheduling, durably checkpoints state, requests bounded
best-effort cancellation and retains provider/module/credential dependencies until
callbacks retire. It never deletes unresolved rows. Cancellation may remove a Pending
intent only before provider commit is possible; ambiguity remains for reconciliation.

The Null backend is a valid explicit headless/test/product composition. It reports all
remote services unavailable with `NullProviderSelected`. Every read and write fails
admission with `platform.provider.null`; it creates no request, offline queue entry,
idempotency record or provider mutation. In particular, it never accepts or silently
discards achievements, stats, scores, cloud writes or presence. Optional
application intent may be explicitly suppressed before submission, but that is not a
successful Null operation. Tests needing success use the capability-accurate mock.

## Authentication And Session Boundary

Platform account locator, live platform subject, ADR-113 local storage/profile,
gameplay/network player and user presentation are distinct identity domains. Equal
display text never links them. Provider account/native identity stays inside the
private provider/identity adapter; gameplay and broad services receive only a typed
live capability:

```cpp
struct PlatformSubjectHandle {
    UInt128 nonce;
    ProviderGeneration providerGeneration;
    PlatformSessionGeneration sessionGeneration;
};

enum class PlatformSessionPhase : std::uint8_t {
    NoSubject,
    Authenticating,
    Active,
    Closing,
    Failed,
};

struct PlatformSessionSnapshot {
    PlatformSessionPhase phase;
    PlatformSessionGeneration generation;
    std::optional<PlatformSubjectHandle> subject;
    PlatformAccessPolicyRevision accessRevision;
    PlatformSessionCapabilities capabilities;
    NormalizedSessionReason reason;
};
```

The subject nonce is nonzero/non-reused 128-bit cryptographic randomness allocated by
the frontend identity broker after authenticated binding. It is equality-only,
non-guessable and valid only for its exact provider/session generations. It is not a
provider ID, persistent account key, authorization secret or display value and is never
serialized/logged. Gameplay cannot construct or enumerate handles.

The `[PLS-006.2]` public model implements this boundary in
`Horo/PlatformServices/PlatformUserSession.h`. Only the detached identity-broker
candidate carries the 128 random nonce bytes; a published `PlatformSubjectHandle`
keeps them private and exposes only provider/session generations needed for fencing.
The handle deliberately has equality but no ordering, text conversion, serialization,
hash, native-account accessor or display projection. Complete snapshot validation is
inert and allocation-free after error construction: it does not authenticate, call a
provider, publish an observer or mutate ambient session state.

`BuildPlatformSessionSnapshotReplacement` validates the explicit phase transition and
monotonic generations before returning a detached replacement. An Active-to-Active
refresh may retain the exact subject/session generation only while advancing the access
revision; provider or subject changes require the close/rebind path. Every consuming
commit calls `ValidatePlatformSessionAccess` with its captured handle and access
revision. The validator returns typed phase, stale-session, stale-policy, consent,
denial, restriction, revocation or unavailable errors and never converts them to empty
success. Session observer dispatch and the orchestration that cancels/quarantines
service work during account switching remain owned by `[PLS-006.3]` and `[PLS-006.4]`.

`Active` has exactly one subject and immutable effective access snapshot. Other phases
have none for admission; there is no second `signedIn` bool or nullable provider object.
A restricted account may be Active while individual operations are unavailable. The
session generation increases on bind, sign-out, account switch/invalidation and
provider replacement. Subject-preserving access changes increment the access revision;
identity uncertainty closes/rebinds a new generation.

The private identity/profile service maps a provider stable authenticated subject to a
pseudonymous product/provider-scoped binding, ADR-113 `LocalUserStorageId` and explicit
`GameProfileId`. Raw account ID, gamertag, email, native handle or credentials do not
enter portable project/save/cloud/progression/offline state. If no stable provider
subject exists, the provider advertises installation-local/single-user capability and
cloud/linking remains unavailable; mutable personal text is never hashed as fallback.

Every user-scoped admission captures subject, session, provider/frontend and access-
policy generations plus its semantic profile/authority generation. Every result,
cache, event, observer, journal/offline or synced-state commit revalidates the captured
session and applicable access revision. Mismatch is `platform.session.stale`; old
provider work may retire privately but cannot publish into a new account.

Durable intent stores a protected pseudonymous subject-binding partition and policy
requirements, never a live handle. Replay first verifies the same binding under a new
active session and reauthorizes the operation. “Current account” cannot retarget it.

### Sign-in, sign-out and account switch

Sign-in prepares provider authentication, stable-subject/private profile mapping,
restrictions/consent, capability limits and a fresh handle as a detached candidate,
then atomically publishes Active. Authentication UI completion is evidence, not
publication authority. Failure destroys candidate handles/credentials with no partial
capability.

Sign-out/account switch closes old admission and invalidates its generation before
cancelling/retiring native work, stopping cache/observer publication and suspending old
durable partitions. Credentials/handles release only after callback safety. A switch
then prepares a wholly new binding; it never mutates a global user pointer in place or
publishes old profile state with a new account. Failure leaves explicit NoSubject/
Failed; reopening old state is a fresh transaction.

Single/current/multiple-user support remains explicit. Multiple users have separate
sessions, handles and request partitions. Listen-server locality, UI selection or
provider login never changes gameplay/server authority.

### Consent and restricted access

Each operation declares a finite Horo purpose/data-class set such as cloud sync,
achievement projection, leaderboard read, friends read or presence publish. Effective
access is the intersection of provider capability/session, platform/parental/child
restriction, region/legal/product policy, OS/provider permission and versioned product
consent where required:

```cpp
enum class PlatformAccessState : std::uint8_t {
    Granted,
    ConsentRequired,
    Denied,
    Restricted,
    Revoked,
    Unavailable,
};

struct PlatformServiceAccess {
    PlatformServiceOperation operation;
    PlatformAccessState state;
    PlatformAccessReason reason;
    PlatformPurposeId purpose;
    PlatformDataClassMask dataClasses;
    ConsentPolicyVersion consentVersion;
};
```

Only Granted admits work; every other state has one typed reason and no usable binding
for that operation. UI/overlay presents localized disclosure and submits an expected-
revision decision to the consent authority. UI cannot grant itself; closing a prompt
is not consent, provider permission does not replace required product consent and a
checkbox cannot override parental/restricted policy.

Consent is purpose/data-class/version specific, revocable and finitely retained. A
policy/restriction change closes affected admission, advances access revision, blocks
stale in-flight publication and triggers bounded service cache/observer cleanup.
Unrelated operations continue only when their captured decision/data classes remain
valid. Revocation does not claim already committed provider effects were rolled back.

### Personal and display data

`PlatformUserPresentation` and social-subject handles are bounded, untrusted,
consent-scoped projections separate from identity. Display names, avatars, presence
and friend data never authorize, route, identify storage or compare accounts. Text/
images are validated/escaped before rendering. By default they remain only in bounded
memory until request/session/consent retention expires.

Durable cache, analytics/export or user-generated presence requires a separate
declared purpose, schema, retention/deletion policy and consent. General logs, metrics,
crash bundles, project/save files, journals, scripts and ordinary CLI/MCP results
contain no raw account/native IDs, live/social handle bytes, personal display data,
friends graph, presence free text, credentials or private binding locators. Safe
observability uses aggregate counts, typed state/reason/generations and ephemeral
non-account-derived correlation IDs.

## Shutdown And Late Completion

Shutdown closes frontend admission and increments generation before requesting
policy-permitted cancellation. It then drains accepted owner-lane evidence, commits at
most one terminal result per admitted request, stops subscriptions/executor delivery
and waits within the declared bound for provider operation and callback epochs. Safe
retirement, not terminal publication or dropped handles, permits provider/package and
borrowed-buffer release.

If native work does not retire within the bound, shutdown reports typed failure and
retains the required dependencies rather than unloading provider code early. Late
callbacks carry the closed provider/session/frontend generation, may complete private
retirement bookkeeping and cannot publish a new terminal result or invoke user code.
Shutdown is idempotent after partial initialization and repeated calls return the
recorded outcome without duplicating cancellation or native teardown.

## Security And Trust

- Closed platform SDK implementations live in private repositories. The public
  engine repository contains only interfaces, registries, the null backend, and
  the mock backend.
- ADR-133 progression definitions select local or server authority; competitive
  clients never submit privileged conclusions and idempotency does not provide
  authentication/anti-cheat.
- Cloud save data is opaque untrusted input to the platform service layer. Transport
  authentication does not establish archive trust. Integrity, signature/scope,
  compatibility and optional confidentiality policy belong to Runtime Save and the
  host-selected save security profile under ADR-116.
- Provider tokens and account credentials remain inside the backend's short-lived
  credential lease. They never enter archive bytes, coordinator journals, tool
  results, logs or conflict metadata.
- ADR-135 live subject handles are non-persistent generation capabilities; personal/
  social presentation is separate bounded data and every operation passes typed
  purpose, consent and restriction policy before admission/commit.

## Privacy And Compliance

- Friends, presence and any other personal-data operation declare purpose/data class,
  product retention and required provider/OS/product consent explicitly.
- Child, parental, region and platform restriction is an operation capability state,
  not UI-only visibility or a generic successful empty result.
- Presence free text and display/social data are untrusted user/provider content;
  retention/export/analytics requires separate consent and policy.
- Cloud provider retention/backup is external evidence; Horo's local journal/recovery
  retention and deletion intent remain product/coordinator policy.
- Consent denial/revocation is typed, closes affected publication and cannot be
  bypassed by debug tooling, configuration or development builds.

## Observability

Platform services emit bounded metrics:

```text
platform.request.pending_count           -- in-flight requests
platform.request.completed_count         -- completed by service and backend
platform.request.failed_count            -- by error category
platform.request.latency_ms              -- end-to-end latency
platform.offline.pending                 -- aggregate pending by service/state
platform.offline.reconciling             -- aggregate remote-ambiguity count
platform.offline.storage_bytes           -- bounded queue storage utilization
platform.session.signed_in               -- 0/1 gauge
platform.capability.available            -- gauge per service per backend
```

No platform SDK logging or network callbacks run on the audio or render
threads.

## Editor And Runtime UI Surfaces

Platform services need authoring surfaces for project-level configuration and
small runtime/debugging panels for observing session and request state. Each
surface below includes an ASCII layout before implementation begins.

### Project Settings > Platform Services

Access: `Edit > Project Settings > Platform Services`

```text
+--------------------------------------------------------------+
| Project Settings                                      [Save] |
+--------------------------------------------------------------+
| General | Audio | Rendering | Physics | Platform Services |   |
+--------------------------------------------------------------+
| Active Backend                                               |
|   [ Steamworks                                     v ]       |
|                                                              |
| Achievements                                                 |
|   Canonical Key             Display Name      Hidden  Steps |
|   -------------             ------------      ------  ----- |
|   campaign.first_blood      First Blood       [ ]     1     |
|   combat.kill_streak_10     Ten in a Row      [x]     10    |
|   [+ Add]                                                    |
|                                                              |
| Leaderboards                                                 |
|   Canonical Key             Sort        Friends Only        |
|   score.high                Descending  [x]                  |
|   [+ Add]                                                    |
|                                                              |
| Cloud Save                                                   |
|   Conflict policy: [ Defer to Save Coordinator      v ]      |
|                                                              |
| Offline Delivery                                             |
|   [x] Eligible progression durable delivery                  |
|   [ ] Persist consented presence desired state               |
|   Progression expiry: [ 7 days ]  Presence: [ 15 minutes ]  |
|                                                              |
| Timeouts And Retry                                           |
|   Default timeout:  [ 30 ] seconds                           |
|   Retry attempts:   [ 3  ]                                   |
+--------------------------------------------------------------+
```

### Platform Diagnostics Panel

Access: `Window > Platform Diagnostics`

```text
+--------------------------------------------------------------+
| Platform Diagnostics                                   [x]   |
+--------------------------------------------------------------+
| Session: Active          Generation: 17                      |
| Subject: Bound           Presentation: Consent granted       |
| Backend: SteamBackend                                        |
+--------------------------------------------------------------+
| Service        | Available | Pending | Errors | Avg Latency |
|----------------|-----------|---------|------|-------------|
| Achievements   |    [x]    |    0    |   0  |    45 ms    |
| Leaderboards   |    [x]    |    1    |   0  |   120 ms    |
| Cloud Save     |    [x]    |    0    |   0  |   210 ms    |
| Presence       |    [ ]    |    0    |   0  |      -      |
| Friends        |    [x]    |    0    |   0  |    30 ms    |
+--------------------------------------------------------------+
| Platform Queue: 2 pending | Reconciling: 0 | Oldest: 4m 12s |
| Save Cloud Journal: 1 pending | Conflict: 0                  |
+--------------------------------------------------------------+
```

### Social / Friends Panel (Optional)

Most friend management is platform-owned. Horo may expose a read-only runtime
overlay or editor debug panel.

```text
+--------------------------------------------------------------+
| Friends                                                [x]   |
+--------------------------------------------------------------+
| [ Online ] [ All ]                                           |
+--------------------------------------------------------------+
| [o] Alice        In Main Menu                                |
| [o] Bob          Playing Level 3                             |
| [ ] Carol        Offline                                     |
+--------------------------------------------------------------+
```

### Required Surface Checklist

| Surface | UI Placement | Access Pattern |
|---|---|---|
| Project Settings > Platform Services | Persistent settings panel | Menu: Edit → Project Settings → Platform Services |
| Achievements configuration | Persistent settings sub-panel | Inside Project Settings > Platform Services |
| Leaderboards configuration | Persistent settings sub-panel | Inside Project Settings > Platform Services |
| Cloud Save settings | Persistent settings sub-panel | Inside Project Settings > Platform Services |
| Presence status mapping | Persistent settings sub-panel | Inside Project Settings > Platform Services |
| Platform Diagnostics | Persistent panel | Menu: Window → Platform Diagnostics |
| Social/Friends panel | Runtime overlay or editor debug panel | Optional; most friend management is platform-owned |

Configuration authority lives in Project Settings:

- active platform backend selection
- achievement, leaderboard, stat, and presence stable ID tables
- cloud save capability, timeout and retry policy; conflict resolution authority is
  owned by the save/cloud coordinator
- per-service timeout and retry policy
- offline queue persistence settings

Implementation status on 10 September 2026: PLS-003.5 publishes one typed immutable
`PlatformProjectConfiguration` contract for GUI, CLI, headless and cook composition.
The versioned project candidate fixes the exact host profile, explicit Null or exact
provider selection, and disabled/optional/required policy for every service. Trusted
provider packages contribute only bounded inert Horo metadata (module/provider
identity, interface version, profile eligibility and capability claims); validation
never discovers or activates code. Contributions are canonicalized before the policy
fingerprint is computed, exact selection never falls back, required capability/profile
conflicts fail before backend activation, and replacements are fenced to the prior
immutable fingerprint. Provider SDK types, native handles, credentials and account
identities remain outside the contract. Schema versions other than version 1 require
an explicit project migration and fail closed in ordinary open/cook paths.

The Platform Diagnostics panel shows runtime state:

- session phase/generations and consent-safe presentation availability
- backend capability availability (per service)
- pending request count and recent errors by category
- offline queue depth and oldest pending operation
- per-service latency and success/failure rates

Most platform-owned flows (friend invites, achievement toast notifications,
rich presence detail rendering) are handled by the platform overlay or OS, not
by Horo UI. Horo only authors the configuration and observes the state.

## Testing

Required tests cover:

- admission failure creates no request/provider call/callback, while dropping every
  handle/subscription leaves an admitted operation owned until terminal retirement
- every legal and illegal request-state transition, with terminal state/result atomic
  and exactly once under completion/cancel/timeout/shutdown races
- callbacks are never inline or under frontend/provider locks; late subscription is
  deferred, token destruction suppresses delivery and re-entry cannot recursively
  drain completion
- timeout uses one monotonic admission deadline across retries, wins only when no
  eligible pre-deadline completion exists and remains terminal after late provider work
- Null rejects every read and write with `platform.provider.null` and creates no
  request, offline entry or idempotency record
- mock backend replays scripted responses
- frontend routes calls to the correct backend service
- local/server authority routing exposes no server commit capability to clients or the
  listen-server client world; prediction replay emits one logical mutation
- every achievement/stat/leaderboard operation exercises its exact algebra and
  capability requirement, including provider variants without safe primitives
- exact mutation duplicates join, conflicting payload reuse fails and every retry/
  durable replay preserves envelope identity and scope
- timeout/late completion races preserve ADR-130 terminal state while progression
  ambiguity reconciles without a second remote effect
- max/min/best coalescing is result-preserving under every input order and retains an
  outcome for each logical receipt; conditional/additive operations never coalesce
- durable mode commits one queue row before first provider submit; volatile mode is
  never reported as queued, and duplicate admission joins only an exact envelope
- every ADR-133 replay class is accepted/rejected against qualified provider semantics;
  cloud writes/deletes and all reads/queries can never enter the platform queue
- crash/fault injection at queue admission, provider submit/commit, terminal commit and
  compaction preserves the original intent/ambiguity without false success
- per-subject/semantic-lane ordering, fair cross-lane scheduling and legal coalescing
  retain every logical receipt; capacity exhaustion never evicts oldest/ambiguous work
- backoff, rate limit, expiry, clock skew, attempt and storage limits are bounded;
  expiry is a durable terminal failure and never resets across restart
- presence persistence is opt-in/consented/short-lived, coalesces Set/Clear desired
  state only within one exact lane and never becomes history
- request cancellation does not leak state
- stable ID registries reject unregistered names
- achievement definition candidates reject malformed localization/progress fields,
  duplicate or incomplete active IDs, stale/cross-project ledger bindings and ordinary
  authority/progress replacement; presentation-only replacement and tombstoned removal
  preserve the prior immutable snapshot
- stable ID golden vectors agree across hosts; malformed salts/encodings, stored/
  derived mismatches, collisions, duplicate aliases and zero IDs fail closed
- display rename/reorder and identity-preserving clones retain IDs; tombstones cannot
  be silently reused and namespace-fork failure rolls back every reference
- provider mapping generation rejects unknown, tombstoned, duplicate, wrong-kind and
  stale-fingerprint rows; provider-native values never become gameplay identity
- typed cloud-object addressing, provider revision propagation and no timestamp-based
  automatic winner selection
- conditional write/delete and precondition failure; uncoordinated providers never
  receive background mutations
- list/read revision consistency, exact length/digest and malformed/partial/oversized
  transfer rejection with no partial bytes published
- atomic create/replace/delete races and native multipart failure/cancel/timeout with
  no staging object visible as the public key
- object/count/namespace/staging/concurrency quota boundaries and no destructive
  transport-owned cleanup
- cloud mutation-ID replay/conflict plus committed/uncommitted/conflicting ambiguity
  reconciliation; exactly one ADR-115 journal and no generic queue copy
- cloud archive writes/deletes never enter the generic offline queue
- authenticated-provider downloads remain untrusted until local bounded admission;
  malformed/oversized results cannot publish or replace a local generation
- cloud credential/provider metadata is redacted from save diagnostics and tools
- sign-in candidate failure, sign-out/account-switch/provider-reload races and stale
  callbacks never publish across subject/session generations
- same-binding reauthorization may resume an eligible durable partition; a newly
  current account never retargets old progression or cloud intent
- every consent/restriction state and access-revision change is enforced at admission
  and commit, including child/parental/region policy and UI/debug bypass attempts
- social handles and bounded presentation expire with session/access scope; malformed
  personal data and raw-ID/PII leakage into logs, metrics, crashes or tools fail closed
- typed capability snapshot and private binding agree; missing/duplicate/mismatched
  bindings fail composition and unavailable services return
  `platform.capability.unavailable`
- Null, capability absence, cancellation, timeout and provider failure have distinct
  stable codes; provider categories/native evidence never replace frontend identity
- presence updates coalesce to the most recent state

### Mock Backend

The mock backend is a core test helper that exposes the same
`IPlatformServicesBackend` interface as real platform backends. It accepts a
scripted response table instead of calling an SDK:

```cpp
struct MockResponse {
    std::optional<Error> error;
    std::variant</* response types */> payload;
    std::chrono::milliseconds delay{0};
};

class MockPlatformServicesBackend : public IPlatformServicesBackend {
public:
    void SetResponse(ServiceType service,
                     OperationType operation,
                     MockResponse response);

    void ExpectSequence(std::vector<ExpectedCall> calls);
    bool AllExpectationsMet() const;
};
```

Tests use the mock backend to verify:

- correct request routing and error translation
- timeout handling when `delay` exceeds the policy
- offline eligibility when the backend returns normalized `Offline`
- progression mutation ID/envelope stability across retry, replay and reconciliation
- stat max/min and leaderboard best-score coalescing under reordered inputs
- conditional revision, durable dedupe, unsupported operation and ambiguous-outcome
  provider capability variants
- cloud list/read consistency, partial transfer, atomic revision CAS, quota and
  committed-after-timeout scripts with exact caller-owned reconciliation evidence
- authentication-stage failure, account-switch/stale-callback and consent-revocation
  scripts with no cross-subject cache, observer or durable-replay publication
- queue storage disk-full, permission, truncated/corrupt/version-skewed and publication-
  unknown scripts plus repeated/partial shutdown and late callback retirement
- concurrent Platform Offline Queue and Save cloud-journal recovery proves there is no
  shared durable row, scheduler, outcome or cross-owner FIFO

Platform-specific backend tests live in the private platform repositories.

## Related Documents

- [ADR-130](../../adr/130-platform-services-frontend-request-lifetime-timeout-null-and-error-semantics.md):
  frontend request ownership, lifecycle, callback, capability, Null and error baseline.
- [ADR-131](../../adr/131-platform-services-closed-sdk-extension-abi-package-and-composition-boundary.md):
  closed SDK packaging, extension ABI, provider lifecycle and product composition.
- [ADR-132](../../adr/132-platform-services-project-salt-stable-id-tombstone-and-provider-mapping.md):
  deterministic project IDs, canonical encoding, tombstones, aliases and provider
  mapping ownership.
- [ADR-133](../../adr/133-platform-progression-authority-trust-and-idempotency.md):
  achievement/stat/leaderboard authority, trust, typed mutation algebra, retry and
  ambiguity policy.
- [ADR-134](../../adr/134-cloud-blob-transport-revision-precondition-and-offline-ownership.md):
  authenticated opaque blobs, revision CAS, quota/integrity/partial transfer and
  caller-owned durable cloud intent.
- [ADR-135](../../adr/135-platform-identity-session-generation-privacy-and-consent.md):
  platform identity-domain separation, generation-fenced subject capabilities,
  consent/restriction authority and personal-data handling.
- [ADR-136](../../adr/136-platform-offline-queue-ownership-replay-and-cloud-intent-boundary.md):
  single-owner offline durability, admission/replay/expiry/shutdown semantics and the
  Save-owned cloud intent boundary.
- [Platform Services Config UI Reference](./platform-services-config.html): achievements, leaderboards, cloud saves, presence, and platform adapters panel.

- [Audio Architecture](./audio-architecture.md)
- [Input Architecture](./input-architecture.md)
- [Platform Abstraction Architecture](../foundation/platform-abstraction.md)
- [ADR-113](../../adr/113-local-storage-user-profile-and-slot-ownership.md): product,
  user/profile and logical-slot addressing across local and cloud boundaries.
- [ADR-115](../../adr/115-cloud-save-authority-revision-and-conflict-policy.md): local
  authority, provider preconditions, offline journal and conflict preservation.
- [ADR-116](../../adr/116-save-data-threat-model-and-trust-policy.md): untrusted cloud
  bytes, bounded local admission, credential isolation and save tool policy.
- [Extension System](../extensions/plugin-system.md)
- [Release Security](../release/release-security.md)
