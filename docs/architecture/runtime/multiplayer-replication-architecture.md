# Multiplayer Replication Architecture

## Purpose

This document defines Horo Engine's server-authoritative replication ownership,
schema and `FieldId` identity, runtime roles, capture/transmit/apply flow, object
lifecycle, compatibility, RPC/input boundary, prediction seam, interest management,
dedicated-server composition and transport integration.

## Core Decisions

- The authority server is the only network role that publishes canonical gameplay
  state. Clients submit typed inputs/commands; they do not write server state.
- Gameplay/component owners declare stable schemas, codecs and capture/apply
  adapters. Scene/Gameplay retain canonical values and mutation safe points.
- `NetworkRuntime` owns registry snapshots, network object identity, capture
  orchestration, baselines, interest, wire encoding, routing and validated apply
  commands—not gameplay state.
- Wire fields use stable numeric `FieldId` values scoped by stable schema IDs and
  explicit versions. Property paths, member names, offsets and native layouts are
  presentation/implementation details only.
- Standalone, authority-server, autonomous-client and simulated-client roles are
  explicit world/session capabilities. Process locality, possession and input
  devices never grant authority.
- [ADR-149](../../adr/149-destruction-persistence-replication-streaming-and-authority.md)
  specializes this split for destruction. The server captures exact content/revision/
  seed/chunk/support state through the DFR adapter, while Physics separately contributes
  paired active-chunk motion. Clients validate and stage snapshots/deltas for the DFR
  owner safe point; network threads, local contacts and predicted cosmetics never commit
  canonical fracture state. Late join publishes one compatible snapshot root before
  contiguous deltas and never replays historical presentation events.
- Only ADR-098 `Active` sessions reach replication/RPC dispatch.
- Automatic ECS-memory and generic event-bus replication are prohibited.
- ADR-102 host plans map standalone/client/listen/dedicated modes to finite world
  roles before publication. Gameplay receives only a generation-scoped role view;
  process globals and same-process locality never expose authority.

## Ownership Boundary

| Responsibility | Owner |
|---|---|
| Field meaning, schema/field IDs, codec, bounds and capture/apply adapters | Declaring gameplay/component module |
| Canonical values, entity/component lifetime and mutation safe points | Scene/Gameplay owner |
| Registry generation, network IDs, snapshots, baselines, interest, encoding, routing and receive validation | `NetworkRuntime` |
| Protected message movement and connection lifecycle | `INetworkTransport` |
| Role/trust/session composition | Host and ADR-098 admission policy |

Declarations are inert metadata until host composition validates and publishes one
immutable registry generation. Descriptors cannot register globally, inspect live
objects, find a service locator or mutate state during construction.

## Execution Roles and Authority

```cpp
enum class ReplicationExecutionRole : std::uint8_t {
    Standalone,
    AuthorityServer,
    AutonomousClient,
    SimulatedClient
};

struct ReplicationAuthorityGrant {
    RuntimeSceneId scene;
    RuntimeSceneGeneration sceneGeneration;
    ReplicationAuthorityEpoch epoch;
    ReplicationExecutionRole role;
    NetworkSessionGeneration sessionGeneration;
};
```

The host assigns a role to a specific runtime world. The authority server issues
object-level ownership/submission grants under its epoch. Every operation validates
world, epoch, object generation, session generation and role.

| Role | Canonical state | Submission | Received state |
|---|---|---|---|
| Standalone | Normal local Scene/Gameplay ownership; no network grant | None | None |
| Authority server | Captures/publishes canonical declared state at safe points | Validates typed client inputs/commands | Rejects client-authored state snapshots |
| Autonomous client | No server-state authority; may hold separately declared prediction/presentation state | Sends only commands permitted by the server grant | Applies authoritative snapshot/correction through owner adapter |
| Simulated client | No authority or local prediction grant | No object-authoritative submission | Applies authoritative replica state through owner adapter |

A listen-server process composes distinct server and client worlds/roles. The fact
that both live in one process, share a player, or use loopback transport does not
grant the client world authority. Autonomous means permitted input submission and
possibly a separately qualified prediction capability; it never means canonical
server write permission.

`ReplicationRoleBinding` pins one role decision to the exact session generation,
authority-scoped object occurrence, schema identity/version and publication revision.
An autonomous-client binding is valid only when its local peer is the granted owner;
a simulated-client binding cannot alias that owner. `ReplicationRoleState` stages at
most one complete role/ownership replacement and publishes it only at the matching
owner-thread safe point. Stale, cross-session, cross-object, cross-schema, concurrent
or post-shutdown changes fail without altering the published binding.

Field routing evaluates the descriptor's closed `ReplicationCondition` against one
validated pinned recipient value in constant work. `InitialOnly` depends on the typed
spawn/update record kind; owner-only, skip-owner and simulated-only decisions use the
validated role/owner relationship. This visibility policy never grants mutation:
`AuthorizeReplicationFieldWrite` admits canonical field origination only for an
explicit authority-server binding, even when a client owns the presentation object.

`ReplicationExecutionRole` describes one world, not the process. ADR-102 resolves
the complete host mode plan and supplies the current plan, host, scene, session and
authority generations in a read-only gameplay role view. Every capture, apply,
input/RPC submission and prediction request revalidates that view plus its
object-level grant. A process-global `IsServer`, executable/build kind, listener,
headless state, local player, possession or loopback connection cannot substitute.

## Replication Schema and Field Identity

```cpp
struct ReplicationSchemaVersion {
    std::uint16_t major;
    std::uint16_t minor;
};

struct ReplicationFieldDescriptor {
    FieldId id;
    ReplicationValueTypeId valueType;
    ReplicationCodecId codec;
    ReplicationCondition condition;
    ReplicationFieldRequirement requirement;
    ReplicationWritePolicy writePolicy;
    ReplicationFieldLimits limits;
};

struct ReplicationSchemaDescriptor {
    ReplicationSchemaId id;
    ReplicationSchemaVersion version;
    ModuleId owner;
    std::span<const ReplicationFieldDescriptor> fields;
    ReplicationCaptureBinding capture;
    ReplicationApplyBinding apply;
};
```

`ReplicationSchemaId` is a globally registered stable semantic ID. `FieldId` is a
non-zero stable 32-bit ID scoped by its schema. Both survive rename, reorder and
C++ refactor. Published IDs are never reused; removed field IDs remain tombstoned.

A field declares one semantic value type, canonical bounded codec, condition,
requiredness, write policy and limits. Editor/display names, `PropertyPath`, C++
member names, component order, reflection indices, pointer values and byte offsets
never enter the wire contract.

`ReplicationSerializerRegistry` binds each field's exact owner, semantic value
type and codec identity to one pinned typed adapter during host composition. The
registry copies adapter metadata, pins adapter and schema lifetimes, rejects
missing/duplicate/foreign bindings, and enforces both adapter and field bounds
around every call. Runtime values cross this boundary only through the closed
`ReplicationRuntimeValue` representation; serializers never receive an ECS
address, member offset, object span or native-layout byte view.

Quantization is explicit codec metadata. The Horo scalar codec supports exact
boolean/integer/floating representations and deterministic nearest-step floating
quantization. Canonical comparison encodes both typed source values and compares
the resulting type-tagged codec bytes, so values in the same quantization bucket
do not produce spurious deltas. Quantized values remain network representations:
they never replace Gameplay's authored/runtime value or become save/hash
authority. A quantization change allocates a new codec identity.

Replication conditions are descriptor policy, not authority:

- `Always`: considered for every eligible snapshot;
- `InitialOnly`: required in the authoritative spawn record;
- `OwnerOnly`: routed only to the granted autonomous client;
- `SkipOwner`: omitted for that autonomous client;
- `SimulatedOnly`: routed only to simulated-client views.
- `Custom`: routed only from one exact registered condition identity and its
  owner-computed bounded decision evidence; missing or mismatched evidence fails.

An owner may use an explicit revision counter or
`MarkReplicationDirty(NetworkObjectId, FieldId)` as a scheduling hint. The hint
does not contain the value and grants no write/capture authority.

## Registry Composition and Lifecycle

Modules contribute schema descriptors before network world/session activation.
Validation rejects:

- zero, duplicate, tombstoned or foreign-owned schema/field IDs;
- overflowed/empty schemas, duplicate/unsorted fields and invalid version graphs;
- missing value types/codecs, unbounded limits or contradictory condition/write
  policy;
- missing role-required capture/apply adapter and incompatible adapter ownership;
- ambiguous/cyclic compatibility edges or invalid translators.

Fields are canonicalized by numeric `FieldId`. The registry computes a domain-
separated schema-set fingerprint used by ADR-098 session negotiation. Failure
publishes nothing; active sessions pin the complete immutable generation.

Hot reload builds a candidate off-thread and publishes it at an owner safe point.
Existing sessions retain their pinned generation or explicitly close/re-negotiate.
Module unload waits until sessions, snapshots, baselines, queues and adapter calls
release the old generation. Shutdown cancels work, closes dispatch and drains pins
before module/runtime destruction.

## Network Object Lifecycle

The authority server allocates `NetworkObjectId` plus generation under a
`ReplicationAuthorityEpoch` and maps it to one scene-qualified `EntityRef`.
Network IDs are not ECS indices, addresses, names, hierarchy paths or authored
scene IDs.

Lifecycle records are ordered:

1. `Spawn`: object ID/generation, schema/version and required initial fields.
2. `Update`: authoritative tick, baseline identity and changed field records.
3. `Despawn`: terminal tick/reason; later updates for that generation fail.

Reuse increments object generation. Scene replacement increments the authority
epoch. Late packets can therefore never mutate a reused entity slot or replacement
world.

## Capture and Snapshot Ownership

[ADR-175](../../adr/175-replication-dirty-state-capture-and-copy-strategy.md)
selects one strategy: capture declared canonical state once after the complete owner
commit into bounded immutable NetworkRuntime-owned storage. Per-client interest,
baseline and delta work consumes that shared snapshot and never repeats source
extraction.

During the fixed network capture safe point, `NetworkRuntime` selects authoritative
relevant objects and invokes the registered schema capture adapter with:

- a validated read-only owner-thread state view;
- the pinned schema generation;
- object/authority identity;
- a preallocated bounded typed writer.

The adapter emits canonical values by `FieldId`. It cannot mutate gameplay state,
retain component views, register fields, call the transport or allocate/block
without bound. After return, NetworkRuntime owns the immutable captured snapshot
and derived baselines—not the source values.

NetworkRuntime never scans ECS/component memory, reflects arbitrary fields,
serializes `sizeof(T)`, compares padding or derives replication from editor
metadata. Explicit capture is the only state source.

Dirty marks and owner revisions are lossy, idempotent scheduling hints, never values or
publication authority. They may be coalesced, duplicated or lost; revision-based bounded
reconciliation and comparison against the compatible prior captured state preserve
correctness. Polling arbitrary component memory, mirroring generic events, or maintaining
a gameplay-owned replication shadow is prohibited. Reconciliation advances a stable
cursor under finite per-tick object/work/byte budgets; it cannot scan the complete object
population in one simulation tick.

Candidate storage remains private until the complete world/session/authority/object/
descriptor identity and every field validate. Failure, cancellation, capacity denial or
stale completion preserves the prior immutable snapshot. Shutdown closes admission,
cancels candidates and drains snapshot/owner-view pins before owner destruction.
Advancing any identity generation immediately invalidates old snapshots and baselines;
data for a destroyed or reused logical identity is never preserved as a fallback.

## Wire Records and Compatibility

Every replication state record contains:

- authority epoch, object ID/generation and authoritative simulation tick;
- schema ID and negotiated schema version;
- spawn/update/despawn kind and baseline ID where applicable;
- bounded field count with canonical ascending `FieldId` entries;
- each entry's wire type/codec tag, bounded byte length and canonical bytes.

The receiver validates the session is `Active`, server-to-client authority
direction, epoch, object lifecycle, schema generation/version, counts, ordering,
duplicate IDs, field types, lengths, codec limits and total work before decode or
allocation. Native memory layout, padding, RTTI, vtables and pointers never cross
the wire.

Compatibility rules are explicit:

- major versions are incompatible unless the declaring owner registers a bounded
  deterministic translator;
- a compatible minor version may add optional fields with canonical defaults;
- removed IDs are tombstoned;
- semantic type/meaning/requiredness changes allocate a new field ID or follow an
  explicit major-version translation;
- unknown/missing required fields, incompatible codec/type or unsupported
  translator fail;
- unknown optional length-delimited fields may be skipped only when the negotiated
  compatibility rule permits it.

ADR-098 negotiates one compatible schema-set fingerprint/projection. The sender
encodes only that projection. There is no best-effort member/path matching, implicit
downgrade or layout-derived compatibility.

## Validated Apply Path

NetworkRuntime decodes a complete record into a bounded typed apply command. It
validates session, role, authority, object lifecycle and schema before queueing the
command to the target world.

At the owner-thread safe point, Scene revalidates scene/entity generation and calls
the declaring owner's apply adapter. The adapter validates semantic ranges and
commits all accepted fields as one owned mutation, or rejects without partial
visibility. It cannot retain decoded views, bypass component invariants, perform
structural mutation outside Scene commands or write authority-server state from a
client snapshot.

Malformed records are atomic failures. Repeated hostile records follow ADR-098's
abuse/close policy.

## Input, Commands, RPCs and Events

Client input and RPCs are not replicated state writes. They use separately
registered stable command/RPC schemas with direction, reliability, rate, permission,
parameter and payload limits. The server validates the active principal and object
grant, then gameplay decides whether canonical state changes.

Gameplay may publish local events that cause owned state mutation or mark declared
fields dirty. NetworkRuntime does not subscribe to the generic event bus and mirror
arbitrary event payloads. A network event/RPC requires its own typed schema; local
event publication alone produces no network traffic.

## Prediction and Reconciliation Boundary

Prediction is opt-in under
[ADR-100](../../adr/100-prediction-capability-tiers-and-determinism-policy.md):

| Tier | Client behavior | History/guarantee |
|---|---|---|
| `NonPredicted` | Applies authoritative snapshots; presentation may interpolate separately | No input journal, checkpoint ring or replay work; this is the default |
| `LocalPrediction` | Advances an autonomous local candidate from fixed-tick input | Bounded pending inputs/current candidate; correction replaces current candidate without historical rewind |
| `RollbackResimulation` | Restores an aggregate checkpoint, applies correction and replays the ordinary fixed-tick pipeline | Bounded input/checkpoint/occurrence history and qualified deterministic provider closure |

Games select a validated `PredictionDescriptor` with input/state schemas, fixed
tick rate, hooks, provider set, determinism requirement, history/replay limits and
overflow policy. Missing hooks/providers or an unqualified fingerprint rejects the
tier. Simulated clients never gain prediction or input-submission authority.

Rollback reuses canonical owner contracts such as ADR-092 Character plus its paired
Physics/world checkpoint. NetworkRuntime cannot invent a smaller state or restore
component/native solver memory. Aggregate restore/correction/replay publishes only
after complete transactional success.

History and replay work are hard-bounded. An old correction, broken provider/delta
closure or count/byte/replay overflow causes a full authoritative resync and
temporary `NonPredicted` behavior; limits never expand. LocalPrediction overflow
similarly discards its candidate and returns to authoritative state.

Prediction never changes server authority. Distributed authority, peer lockstep
and client-majority correction are unsupported. Speculative side effects require
stable occurrence IDs and bounded confirm/cancel/deduplication; irreversible
external effects are server-authoritative only.

Platform achievement, stat and leaderboard projection follows
[ADR-133](../../adr/133-platform-progression-authority-trust-and-idempotency.md).
A client sends ordinary gameplay input/commands, never a privileged “grant progression”
conclusion. The server gameplay owner derives a committed fact after authoritative
validation and uses an injected server-only progression commit capability. Prediction
and rollback of the same stable occurrence cannot emit a second logical mutation, and
the listen-server client world gains no authority from process locality.

## Interest Management

`ReplicationScheduler` is the sole interest/priority/budget authority for an
authority world. Gameplay, Scene and registered providers contribute bounded
read-only facts; they cannot enqueue packets, mutate deficits/tokens, force-send or
change authoritative state.

```cpp
struct ConnectionSchedulingInput {
    NetworkSessionGeneration session;
    NetworkProjectProfileId profile;
    BoundedVector<ViewerFact> viewers;
    BoundedVector<SubscriptionFact> subscriptions;
    TransportBackpressure backpressure;
};

struct ObjectSchedulingInput {
    NetworkObjectId object;
    NetworkObjectGeneration generation;
    WorldBounds bounds;
    ReplicationGroupMask groups;
    ReplicationPriorityClass priorityClass;
    SimulationTick dirtySince;
    SimulationTick lastSent;
};
```

Spatial cell/distance, explicit subscriptions/groups and authority-required
lifecycle are baseline interest sources. Results are unioned/filtered by stable
object ID under candidate/relevant limits. Enter/exit thresholds and dwell ticks
provide hysteresis. Boundary-change work is capped per connection/tick; excess is
deferred in stable object-ID order and revalidated next tick.

Interest is a read-only routing input. It cannot grant authority, mutate Scene,
mark values dirty or change schema compatibility. Despawn and permission revocation
use the reserved lifecycle class rather than ordinary relevance deferral.

## Runtime Mode Projection

[ADR-102](../../adr/102-runtime-network-modes-and-authority-exposure.md) maps host
modes onto the roles above:

- standalone publishes one `Standalone` world and creates no network authority
  epoch, session or replication work;
- client publishes one client world whose autonomous/simulated capabilities exist
  only through the current Active session and server-issued object grants;
- listen server publishes distinct authority-server and local-client worlds and
  crosses the same admitted schema/command boundaries as a remote composition; and
- dedicated server publishes one authority-server world with no local-player or
  presentation capabilities.

Dedicated servers run headless with full gameplay/physics simulation, authority-
server role, client admission, replication capture and observability. They compose
no renderer/audio dependency merely to define network limits.

```bash
horo-engine server --project /path/to/MyGame --map MainLevel --port 7777
```

Listen and dedicated servers share the same authority, schema and admission
contracts. A listen server's co-located client uses a separate client role/world.
Travel replaces world/authority generations without changing the host mode.
Reconnect replaces the client session generation; disconnect cannot promote a
client world to standalone or transfer listen-server authority. Shutdown removes
all gameplay role views and invalidates grants before retiring worlds and sessions.

## Network Transport

Replication submits bounded messages through the backend-neutral network runtime;
it never depends on native transport identity:

```cpp
class IReplicationProtocol {
public:
    virtual Result<void> SendReplicationMessage(
        ClientId target,
        const ReplicationMessage& message) = 0;
    virtual Result<void> BroadcastReplicationMessage(
        const ReplicationMessage& message) = 0;
    virtual Result<void> PollReplicationMessages(
        std::span<ReplicationMessage> outMessages) = 0;
};
```

ADR-097 selects private `NetworkTransportGNS` for production direct IP and Null
for deterministic/offline tests. Optional providers remain explicit product
compositions. Transport encryption/provider identity does not grant replication
authority; ADR-098 active-session admission remains mandatory.

## Bandwidth Management

The immutable `NetworkProjectProfileV1` is renderer-independent and gives every
limit a unit/value before world activation:

- network tick rate (`Hz`), active connections and network objects (`count`);
- candidates, relevant objects and interest transitions
  (`count/connection/network-tick`);
- captured objects (`count/network-tick`) and serialized fields/messages
  (`count/connection/network-tick`);
- interest, capture and scheduling work (`microseconds/network-tick`);
- target rate (`bytes/second/connection`), burst and reliable/unreliable queue
  capacity (`bytes/connection`);
- enter/exit dwell, starvation and saturation grace (`network ticks`).

Profiles are selected by product/match mode and qualification evidence—not `es3`,
DirectX/Vulkan, `high_end`, render LOD, GPU memory, resolution or frame rate. A
profile replacement creates a new scheduling generation at a network-tick safe
point.

Each connection uses a rate-derived byte token bucket, per-tick message/field/work
ledgers and capped per-priority weighted deficits. Required lifecycle/initial state,
gameplay-critical reliable, gameplay state and cosmetic/replaceable classes have
explicit reservations/weights. Selection uses class, deficit/age and stable object
ID; rotating connection start plus accumulated deficit prevents a fixed low-ID
connection from always winning world budgets.

Overload is bounded: stop admission at capacity, preserve control/lifecycle,
coalesce newest replaceable unreliable state, defer lower classes with fairness,
reject reliable queue overflow, then degrade optional frequency/classes and close a
connection that cannot make required progress after its grace window. A slow peer
cannot spend another connection's ledger. Limits never expand and scheduling never
mutates authoritative state.

## Testing and Qualification

Required automated coverage includes:

- valid schema registration, canonical fingerprint/order and rejection of zero,
  duplicate, tombstoned, foreign-owned and overflowed declarations;
- missing codec/type, contradictory policy, unbounded limits and malformed/cyclic
  compatibility translators;
- standalone/server/autonomous/simulated capability matrices and co-located
  listen-server worlds without locality-derived authority;
- valid/invalid standalone/client/listen/dedicated host plans; unsupported package
  modes, process/headless/listener/local-player inference and mutually incompatible
  role capabilities are rejected before world publication;
- rename/reorder/layout changes preserving `FieldId`, with explicit rejection of
  `PropertyPath`, offset, pointer and raw-memory fixtures;
- exact, additive-minor and explicit-major translation plus missing/unknown/
  duplicate/reordered/wrong-type/oversized fields;
- spawn/update/despawn, stale object/scene/session/authority generations,
  disconnect, scene reload and module reload/unload pinning;
- capture/apply owner safe points, dirty hints, atomic apply rejection, queue/work
  overflow, cancellation and shutdown with queued work;
- proof that arbitrary component mutations and generic event publication do not
  replicate without a registered schema and explicit capture/dirty path.
- NonPredicted profiles allocate/schedule no prediction input/checkpoint/replay
  history; prediction descriptors reject incomplete hooks/provider closure.
- Latency/loss/reorder/duplicate inputs, local correction, rollback golden replay,
  history/replay overflow, full resync/degrade and lifecycle generation changes.
- Speculative occurrence deduplication and prohibition of irreversible client-side
  effects during replay.
- renderer-independent profile unit/bound/overflow validation and proof that
  renderer backend/tier/frame rate cannot alter scheduling;
- exact-boundary interest movement, hysteresis/dwell, transition saturation,
  permission/group changes and stable deferred order;
- token/deficit accounting, weighted fairness, rotating connections, starvation
  bounds, queue isolation, unreliable coalescing and reliable overflow;
- sustained saturation degrade/disconnect plus spawn/despawn/profile/scene/session
  generation and shutdown with queued/provider work.

Descriptor, compatibility, record framing and codec parsers receive bounded fuzz/
property coverage. Diagnostics identify stable IDs/versions and safe reasons but
exclude field payloads by default.

## Related Documents

- [ADR-097: Default Real-Time Transport Backend](../../adr/097-default-real-time-transport-backend.md)
- [ADR-098: Protocol, Session and Trust Policy](../../adr/098-protocol-session-and-trust-policy.md)
- [ADR-099: Replication Ownership, Authority and Compatibility](../../adr/099-replication-ownership-authority-and-compatibility.md)
- [ADR-100: Prediction Capability Tiers and Determinism Policy](../../adr/100-prediction-capability-tiers-and-determinism-policy.md)
- [ADR-101: Interest, Priority and Network Budget Model](../../adr/101-interest-priority-and-network-budget-model.md)
- [ADR-102: Runtime Network Modes and Authority Exposure](../../adr/102-runtime-network-modes-and-authority-exposure.md)
- [Networking Architecture](./networking-architecture.md)
- [Scene Runtime](./scene-runtime.md)
- [Gameplay Behavior Authoring](../extensions/gameplay-behavior-authoring.md)
- [Physics Architecture](./physics-architecture.md)
- [Save Game And Persistence](./save-game-and-persistence.md)
- [Application Security Architecture](../security/application-security.md)
- [Observability Performance](../observability/observability-performance.md)
