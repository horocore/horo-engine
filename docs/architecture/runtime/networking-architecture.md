# Networking Architecture

## Purpose

This document defines the optional network transport, connection lifecycle,
message serialization, asynchronous I/O, security boundaries, runtime
integration, and observability for Horo Engine.

Networking is an optional engine capability. Hosts, tools, and games that do not need it
do not construct, link, or activate concrete transport backends.

Future Character replication/rollback must consume
[ADR-092](../../adr/092-character-controller-determinism-and-state-composition.md)'s
canonical typed state/codec rather than defining another authoritative Character
field set. Network authority, interest, baselines, prediction, packet framing,
compression, acknowledgement and security remain Network-owned. Exact canonical
deltas may wrap the state; lossy presentation snapshots are distinct and cannot be
used for canonical restore/hash. Connection/session/packet state never enters the
Character checkpoint.

## Core Decisions

- **Strict Layered Architecture**: Networking is separated into target-level tiers: public contracts and types (`HoroEngine::NetworkApi`), session coordination and replication runtime (`HoroEngine::NetworkRuntime`), and private concrete transport backends (`HoroEngine::NetworkTransportNull`, `HoroEngine::NetworkTransportGNS`).
- **Production Baseline**: ADR-097 selects open-source GameNetworkingSockets (GNS) for production direct-IP real-time transport. Null remains the deterministic/offline backend; ICE/P2P, relay and provider integrations require explicit optional composition.
- **Transport vs Session Split**: `INetworkTransport` is a packet-transport abstraction. Protocol negotiation, authentication, replication, and `NetworkSession` live in `NetworkRuntime`. Transport backends do not implement application authentication.
- **Admission Before Gameplay**: Transport `Connected` creates a pre-active session only. The runtime's bounded admission controller is the sole authority that may negotiate, authenticate, activate and publish a session to gameplay.
- **Host-Owned Trust**: The composition root supplies an immutable loopback-development, LAN or remote trust-policy snapshot. Project data, peers and transports cannot choose trust roots, weaken authentication or widen bind scope.
- **Handle-Based Public Transport API**: Public transport operations use `INetworkTransport` plus generation-checked `ConnectionHandle` / `ListenerHandle` values. `ITransportConnection` and `ITransportListener` are not public types.
- **Complete Native Encapsulation**: Public headers expose zero OS socket descriptors (`SOCKET`, `int fd`), OS socket headers, TLS/OpenSSL contexts (`SSL*`), event loops, or GNS/provider transport structures.
- **Optional Link and Composition**: Single-player games, asset tools, and offline CLI utilities compile and run without linking transport backends. Products can compose `NetworkTransportNull` or omit the network runtime entirely.
- **Host-Owned Injection**: The composition root instantiates a concrete transport and transfers unique ownership into `NetworkRuntime`. `NetworkRuntime` never links a concrete backend directly.
- **Thread Isolation & Zero I/O State Mutation**: Network I/O executes on dedicated background worker threads owned by the concrete transport that needs them. I/O threads never directly mutate Scene, ECS, Entity, Component, Editor, or Gameplay state.
- **Transport-Owned Cross-Thread Queues**: The transport owns inbound event and outbound send queues. `NetworkRuntime` observes inbound traffic only by calling `PollEvents()` on the simulation thread.
- **Dedicated Frame Schedule Phases**: Inbound transport events and session state updates are processed on the simulation/main thread during `FrameScheduler::Phase::NetworkPoll`. Outbound replication snapshots and RPCs are serialized and submitted during `FrameScheduler::Phase::NetworkFlush`. These phases do not depend on render command extraction.
- **Copy-On-Send Payloads**: `Send()` copies caller payload bytes before returning. The transport does not retain the caller-provided span.
- **Bounded Queues and Backpressure**: Inbound and outbound message queues enforce fixed capacity bounds and deterministic drop/rejection policies for overloaded connections.
- **Explicit Schemas & Negotiation**: Messages use typed schemas and handshake negotiation. Raw C++ memory layout, vtables, pointers, and unstructured object graphs are never serialized.
- **Deterministic Cancellation and Shutdown**: Every connection reaches a definitive terminal state. Disconnections, timeouts, cancellations, and engine shutdown follow bounded, leak-free teardown protocols.
- **Opt-In Prediction**: NonPredicted is the baseline and constructs no history/replay machinery. LocalPrediction and RollbackResimulation require validated ADR-100 descriptors, bounded histories and owner-provided fixed-tick hooks.
- **Network-Owned Scheduling**: One NetworkRuntime scheduler applies immutable renderer-independent project profiles, per-connection bandwidth/work/queue ledgers, bounded interest and weighted-deficit fairness.
- **Single Capture Strategy**: ADR-175 captures declared canonical state once at the post-commit owner safe point into bounded immutable NetworkRuntime storage. Dirty marks are lossy scheduling hints; polling component memory and gameplay-owned replication shadows are prohibited.
- **Typed Runtime Mode Plan**: ADR-102 separates package-supported modes from the one standalone, client, listen-server or dedicated-server plan selected before world publication. Gameplay receives world/session-generation-scoped roles and capabilities; process globals, locality and headless state never grant authority.
- **Shared Typed Configuration**: ADR-103 applies ADR-009 once across editor, CLI, MCP, CI and packaged hosts. Project defaults, preview preferences, release role, host requests and credential bindings retain distinct owners; product capability/security floors constrain runtime selection.

## Target Topology and Module Ownership

The network subsystem is organized into four distinct CMake targets with strict compile-time boundaries:

```text
               +---------------------------+
               |   HoroEngine::Foundation  |
               +-------------+-------------+
                             ^
                             |
               +-------------+-------------+
               |   HoroEngine::NetworkApi  |
               +------+--------------+-----+
                      ^              ^
                      |              |
+---------------------+----+   +-----+-------------------------+
| HoroEngine::NetworkRuntime|   | Concrete Transports (Private) |
| (depends also on Runtime) |   | - NetworkTransportNull        |
+---------------------------+   | - NetworkTransportGNS         |
                                +-------------------------------+
```

### 1. `HoroEngine::NetworkApi`

- **Role**: Backend-neutral public interface and data type definitions.
- **Direct Dependencies**: `HoroEngine::Foundation` only.
- **Public Header Location**: `include/Horo/Network/`
- **Exposed Contracts**:
  - Stable value types: `NetworkAddress`, `NetworkId`, `DeliveryPolicy`, `ChannelId`, `NetworkRole`.
  - Generation-checked opaque handles: `ConnectionHandle`, `ListenerHandle`.
  - Result and error types: `Result<T, NetworkErrorCode>`, `NetworkErrorCode`, `DisconnectReason`.
  - Message abstractions: `MessageView`, `ConstMessageSpan`, `IMessageSerializer`.
  - Transport interface and events: `INetworkTransport`, `ITransportEventConsumer`, `TransportEvent`, `TransportConnectionState`, `TransportCapabilities`.
  - Replication schema contracts: `ReplicationSchemaId`, stable `FieldId`, `ReplicationSchemaVersion`, field descriptors, canonical codec bindings and `ReplicationCondition`.
  - RPC declaration contracts: stable `RpcId`/`RpcParameterId`, typed parameter
    codec bindings, immutable descriptor generations, direction, delivery,
    recipient, caller-permission, rate and payload bounds. Executable handlers
    remain outside descriptor construction and later require exact accepted-ID
    binding by NetworkRuntime and the gameplay owner.
- **Not Public**: `ITransportConnection`, `ITransportListener`, native peers, OS sockets, and backend queue types.

### 2. `HoroEngine::NetworkRuntime`

- **Role**: Session management, protocol negotiation, authentication, replication coordination, and simulation-thread dispatch.
- **Direct Dependencies**: `HoroEngine::NetworkApi`, `HoroEngine::Foundation`, `HoroEngine::Runtime`.
- **Does Not Own**: Transport I/O threads, native connection objects, or the cross-thread inbound/outbound queues. Those remain inside the injected transport.
- **Construction**: Receives `std::unique_ptr<INetworkTransport>` from the host composition root. The runtime destroys the transport only after `Shutdown()`.
- **Key Components**:
  - `NetworkRuntimeCoordinator`: Owns the injected transport and active sessions, and ticks during `NetworkPoll` / `NetworkFlush`.
  - `NetworkSession`: Application-facing session that starts after the transport reports `Connected`.
  - `SessionAdmissionController`: Owns bounded compatibility, peer trust, credential verification and activation before gameplay dispatch.
  - `SessionStateMachine`: Manages session transitions (`Created` -> `Negotiating` -> `Authenticating` -> `Activating` -> `Active` -> `Closing` -> `Closed`).
  - `ReplicationManager`: Pins the validated schema generation and manages explicit dirty hints, immutable captured snapshots, baselines, interest, wire routing and validated apply commands without owning gameplay state.
  - `MessageDispatcher`: Routes incoming RPCs and replication payloads to registered game handlers on the simulation thread.

### 3. `HoroEngine::NetworkTransportNull`

- **Role**: Deterministic in-memory loopback and null transport backend.
- **Direct Dependencies**: `HoroEngine::NetworkApi`, `HoroEngine::Foundation`.
- **Must Not Depend On**: `HoroEngine::Platform`, OS sockets, or a network I/O thread.
- **Purpose**: Unit testing, CI verification, single-player offline simulation, and synthetic latency/loss testing harnesses. `PollEvents()` and `Send()` run on the caller thread against in-memory queues.

The backend has three explicit modes: network-disabled rejection, in-memory
loopback, and a versioned seeded impairment scenario. Scenario evaluation uses a
fixed integer generator and stable draw order for latency, jitter, loss,
duplication, and reordering. Fragmentation copies into prepared fixed slices, and
all scheduled delivery, payload, connection, message, byte, and rate work remains
bounded. Exact connection and queue-ticket generations fence replacement and late
delivery. Disconnect and shutdown are explicit caller-thread lifecycle events.
Null/simulated evidence qualifies deterministic orchestration only; it never
qualifies native sockets, encryption, latency, or production throughput and is
never an implicit fallback from a failed production backend.

### 4. `HoroEngine::NetworkTransportGNS`

- **Role**: Production direct-IP reliable and unreliable message transport implemented with open-source GameNetworkingSockets.
- **Direct Dependencies**: `HoroEngine::NetworkApi`, `HoroEngine::Foundation`, `HoroEngine::Platform`.
- **Encapsulation**: Links GNS, its selected crypto provider, protobuf and platform socket libraries strictly as `PRIVATE`. No third-party headers, values or socket types leak into public includes.
- **Owns**: I/O thread, native GNS/socket state and messages, inbound event queue, and outbound send queue.
- **Baseline Features**: Direct IPv4/IPv6 only. ICE/P2P and Steam/provider-specific integration are disabled unless a product explicitly composes and qualifies them.

## Public Header Encapsulation

To maintain portability, compile speed, and memory safety, public headers under `include/Horo/` enforce complete encapsulation:

1. **No Socket Headers**: Headers such as `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<winsock2.h>`, and `<ws2tcpip.h>` are forbidden in public headers.
2. **No Native Sockets / Handles**: Sockets are encapsulated as opaque integer handles or private members within backend `.cpp` files. Public APIs operate exclusively on `ConnectionHandle` and `ListenerHandle`.
3. **No TLS / Crypto Contexts**: OpenSSL, mbedTLS, or native TLS context pointers (`SSL*`, `SSL_CTX*`) never appear in public interfaces.
4. **No Native Event Loops**: Event loop primitives (libuv `uv_loop_t`, epoll file descriptors, kqueue, IOCP handles) remain private to backend implementations.
5. **No Third-Party Types**: GNS interfaces, handles, messages, connection-info structures, configuration values, callbacks and Steam networking structures are strictly confined to concrete backend sources.

## Host Composition and Transport Ownership

The host composition root selects and instantiates the backend. `NetworkRuntime` never links `NetworkTransportNull` or `NetworkTransportGNS`.

The implemented `TransportBackendComposition` is the bounded, host-owned
registration and selection seam for this decision. The host registers inert
`TransportBackendDescriptor` values and factories only for backends its target
actually links. A status reports installed, host-supported, configured,
selected, and active independently. Exact selection fails with a typed result
when an ID is absent, unsupported, or unconfigured; it never substitutes Null.
Only activation invokes the selected factory. Cancellation closes activation
and asks an active backend to stop; shutdown releases it before the host unloads
factory code. Dynamic extension factories need an external code lease through
the approved application-capability/provider boundary before registration.

The `DISABLED` and `HEADLESS_NULL` target profiles have configure-time
transitive-link checks against production transport libraries. The Null profile
can explicitly create a bounded `DeterministicTransport` lifetime without a
production backend. The GNS profile fails configuration until the separate
GNS implementation target is installed. This composition lifetime is not yet
the future packet-level `INetworkTransport`/`NetworkRuntimeCoordinator` handoff;
adding that handoff must preserve the exact-selection and optional-link rules.

```cpp
auto transport = CreateGnsTransport(config); // or CreateNullTransport(config)
NetworkRuntime runtime({
    .transport = std::move(transport),
});
```

Ownership rules:

- The host transfers unique ownership of `INetworkTransport` into `NetworkRuntime`.
- Shared ownership is forbidden. The runtime is the sole owner after injection.
- The host must not call `PollEvents`, `Send`, `Close`, or `Shutdown` on a transport after transferring it.
- `NetworkRuntime::Shutdown()` shuts down the transport, joins any I/O thread the backend owns, then destroys the `unique_ptr`.

## Capability Interface

```cpp
namespace Horo::Network {

struct ConnectRequest {
    NetworkAddress endpoint;
    std::chrono::milliseconds timeout{5000};
    uint32_t channelCount{2};
    CancellationToken cancellation{};
};

struct ListenRequest {
    NetworkAddress bindAddress;
    uint32_t maxConnections{32};
    uint32_t channelCount{2};
};

class INetworkTransport {
public:
    virtual ~INetworkTransport() = default;

    virtual Result<void> Initialize(const TransportConfig& config) = 0;
    virtual Result<void> Shutdown() = 0;

    virtual Result<ListenerHandle> Listen(const ListenRequest& request) = 0;
    virtual Result<ConnectionHandle> Connect(const ConnectRequest& request) = 0;
    virtual Result<void> Send(ConnectionHandle handle, ChannelId channel, ConstMessageSpan payload, DeliveryPolicy policy) = 0;
    virtual Result<void> Close(ConnectionHandle handle, DisconnectReason reason) = 0;

    virtual void PollEvents(ITransportEventConsumer& consumer) = 0;
};

} // namespace Horo::Network
```

Handles are 64-bit generation-checked identifiers preventing ABA handle reuse issues.

### Connect and Listen Semantics

`Connect()` is asynchronous:

- After request validation, `Connect()` allocates a `ConnectionHandle` and returns it immediately.
- The returned connection is `Created`, or `Resolving` when `endpoint` contains a hostname.
- DNS, socket connect, and transport handshake continue in the backend. They must not block the caller.
- State changes and inbound packets are reported later through `PollEvents()`.
- A successful `Connect()` return is not a connected session and is not an authenticated `NetworkSession`.

`Listen()` likewise returns a `ListenerHandle` immediately after bind-request validation. Accept events arrive through `PollEvents()`.

### Payload Lifetime

- `Send()` copies the caller-provided `ConstMessageSpan` before returning. The transport does not retain that span.
- The caller may destroy or reuse the source buffer as soon as `Send()` returns.
- `MessageView` values delivered to `ITransportEventConsumer` are valid only for the duration of that callback. Consumers that retain payload bytes must copy them.

### Handle Lifetime

| Call | Result |
|---|---|
| `Connect()` / `Listen()` validation failure | No handle is issued; typed `NetworkErrorCode` |
| `Send` / `Close` with never-issued or generation-mismatched handle | `NetworkErrorCode::InvalidHandle` |
| `Send` on a handle whose connection is `Closing`, `Closed`, or `Failed` | `NetworkErrorCode::ConnectionClosed` |
| `Close` on an already `Closed` or `Failed` handle with matching generation | Success (idempotent) |
| `Close` on `InvalidHandle` | `NetworkErrorCode::InvalidHandle` |

A handle remains generation-valid until the transport reclaims the slot. Reuse advances the non-zero 32-bit generation without
wrapping; an exhausted slot is retired. Every operation compares slot and generation against the owner registry, so an old handle
cannot name a replacement. `PeerHandle` is transport correlation only and never authenticates a remote principal. Diagnostics may
project the opaque numeric slot and observed generation, but those values are neither wire identity nor a native handle.

## NetworkAddress

`NetworkAddress` is a first-class endpoint value. It must represent:

- IPv4 literals: `127.0.0.1:7777`
- IPv6 literals: `[::1]:7777`
- Hostnames: `game.example.com:7777`

Literal addresses skip `Resolving`. Hostname endpoints require asynchronous DNS as part of the transport connection lifecycle. Resolution failure transitions the transport connection to `Failed` with `NetworkErrorCode::NameResolutionFailed`.

Public `NetworkAddress` stores IPv4/IPv6 bytes in network byte order or an owned lowercase bounded ASCII DNS hostname plus a
non-zero numeric port. IPv6 is bracketed at the parse boundary. Empty labels, trailing-dot aliases, non-canonical dotted decimal,
URI schemes, IPv6 zones, Unicode/IDNA input, service names and oversized values fail closed before backend work. Numeric and DNS
representations compare by canonical owned data; diagnostic text is bounded and never authoritative. It never exposes `sockaddr`,
`addrinfo`, or other OS types.

## Transport and Session Lifecycles

Transport connection state and session state are distinct machines. Authentication is not a transport state.

```text
TransportConnection
  Created
    -> Resolving          // hostname only
    -> Connecting
    -> Connected
    -> Closing
    -> Closed

  Any non-terminal state -> Failed
```

```text
NetworkSession
  Created                 // allocated when transport reaches Connected
    -> Negotiating        // protocol version, channels, capabilities
    -> Authenticating     // runtime/session token validation
    -> Activating         // transcript-bound mutual activation acknowledgement
    -> Active
    -> Closing
    -> Closed

  Any non-terminal state -> Failed
```

- **Transport `Created`**: Handle allocated; request validated. `Connect()` has already returned.
- **Transport `Resolving`**: Asynchronous DNS for a hostname endpoint.
- **Transport `Connecting`**: Native connect and transport-level handshake. No application token is exchanged here.
- **Transport `Connected`**: Packets can flow. `NetworkRuntime` now creates a `NetworkSession`.
- **Session `Negotiating`**: Schema/protocol version, compression, and MTU agreement.
- **Session `Authenticating`**: `NetworkRuntime` validates host/peer trust and application credentials against the immutable listener policy. Tokens are runtime/session data, not `ConnectRequest` fields on the transport.
- **Session `Activating`**: Both peers validate transcript-bound activation acknowledgements; no gameplay dispatch is permitted yet.
- **Session `Active`**: Gameplay messages, RPCs, and replication are admitted.
- **Closing / Closed / Failed**: Each layer tears down independently. Session close requests transport `Close()`. Transport `Failed` fails the associated session.

Future provider transports replace only the transport machine. They must not absorb session authentication.

## Queue Ownership

Model A is normative:

```text
Transport I/O thread (GNS) or caller thread (Null)
    -> transport-owned inbound event queue
    -> NetworkRuntime calls transport.PollEvents(consumer)

NetworkRuntime NetworkFlush
    -> transport.Send(...)            // copies into transport-owned outbound queue
    -> transport I/O thread sends
```

The transport owns:

- I/O thread, when the backend needs one
- raw/native connection state
- inbound event queue
- outbound send queue

`NetworkRuntime` does not know the queue's mutex, lock-free, or ring-buffer implementation. The architecture requires a bounded thread-safe queue. Spinlock versus mutex is a backend implementation choice, not a public contract.

`NetworkIoService` is the reusable Horo-owned normalization boundary inside one
concrete transport. The transport still owns and schedules its I/O thread; the
service owns the injected private polling source and its prepared completion
FIFO. `PollBackend` gives the source a generation-scoped producer with an exact
per-call budget. A producer retained after that call is stale and cannot publish.
The source may contain native sockets or provider state in its private concrete
type, but normalized completions contain only Horo handles, packet leases and
typed terminal records.

The service binds owner-thread identity at construction. `DrainOwnerThread` is
the only consumer callback boundary, removes one record under synchronization,
then releases the lock before invoking the borrowed callback. Consumers are
never retained. Queue exhaustion rejects publication explicitly; it does not
grow storage, invoke application code on the I/O thread or silently drop a
record. Shutdown closes producer admission before requesting the private source
to wake, serializes against the final poll, discards unobserved records and then
releases the backend. This contract does not create a process-global I/O loop or
move connection lifecycle authority out of the concrete transport.

`NetworkLifecycleRegistry` is the owner-thread boundary immediately after that
normalized drain. It prepares finite listener and connection slots at setup and
accepts only Horo `ListenerHandle` / `ConnectionHandle` values paired with a
non-zero asynchronous operation generation. Connections advance explicitly
through created or resolving, connecting, and authentication-ready states; the
last state is a handoff boundary, not an authenticated gameplay session.

Close requests are idempotent, while terminal publication is exactly once. The
first graceful, failed, cancelled, timed-out, or shutdown result is retained as
immutable typed evidence. A reused slot must carry the exact next non-wrapping
handle generation, and every completion must also match the current operation
generation. Bounded deadline scans, cancellation and shutdown therefore cannot
let a late DNS, connect, close, or native callback revive a replacement. The
registry is owner-thread-only; private transport threads publish through
`NetworkIoService` and never mutate lifecycle state directly.

`TransportBudgetController` is the owner-thread admission boundary before payload
copy or native send-queue mutation. Host composition prepares finite connection
and queued-work storage, then installs a complete immutable versioned policy.
Reliable traffic is rejected explicitly under overload; replaceable state may
coalesce by stable key or be discarded as an explicit result. Per-connection and
global byte, message, rate, and connection limits are enforced together. Repeated
input in one tick cannot amplify saturation work, while distinct saturated ticks
may require lifecycle-owned connection close. Policy replacement is atomic and
revision-fenced, queued completions are generation-fenced, and shutdown closes
admission before discarding bounded outstanding work. The controller owns no
payload, socket, transport thread, or connection lifecycle transition.

`PollEvents()` may be called only on the simulation/main thread during `NetworkPoll`. It drains the transport-owned inbound queue into `ITransportEventConsumer` callbacks. Those callbacks must not block, allocate unboundedly, or re-enter the transport.

## Threading Model and Frame Schedule Phases

To prevent race conditions and frame stalls, network operations are strictly partitioned:

```text
[ Background I/O Worker Thread ]          // GNS only; Null has none
  OS Socket Poll -> Recv Packets -> Framing
         |
         v
  +--------------------------------------+
  | Transport-owned inbound event queue  |
  | (bounded, thread-safe)               |
  +--------------------------------------+
         |
         v  NetworkRuntime::PollEvents during NetworkPoll
[ Simulation / Main Engine Thread ]
  Frame Phase: NetworkPoll
    - transport.PollEvents(consumer)
    - advance TransportConnection states
    - create/advance NetworkSession (negotiate, authenticate, activate)
    - dispatch Active-session messages to ReplicationManager / gameplay
  Frame Phase: Simulation & Gameplay Update
  Frame Phase: NetworkFlush
    - ReplicationManager admits dirty/revision hints and bounded reconciliation
    - capture each selected owner once from the complete committed simulation state
    - publish one immutable candidate only after full identity/field validation
    - serialize outbound snapshots and RPCs
    - transport.Send(...) for Active sessions
  Frame Phase: RenderExtraction
  Frame Phase: Render
         |
         v
  +--------------------------------------+
  | Transport-owned outbound send queue  |
  | (bounded, thread-safe)               |
  +--------------------------------------+
         |
         v
[ Background I/O Worker Thread ]
  Packetize -> OS Socket Send
```

### Dedicated Frame Scheduler Phases

1. `FrameScheduler::Phase::NetworkPoll`:
   - Runs before scene simulation and physics update.
   - Drains transport events and advances session state on the simulation thread.
2. `FrameScheduler::Phase::NetworkFlush`:
   - Runs after scene simulation and before render command extraction.
   - Captures declared canonical state from the complete post-simulation commit and submits derived outbound packets.
   - Has no dependency on render extraction. Flush is ordered before extract so replication latency is not coupled to rendering work.

Allowed adjacent orders are `NetworkPoll -> Simulation -> NetworkFlush -> RenderExtraction -> Render`. `NetworkFlush` after `RenderExtraction` is forbidden as the default because it adds render-extract latency to the network path without a transport requirement.

## Message Schema, Protocol Negotiation and Session Trust

Messages consist of:

- 16-bit Protocol Identifier and 16-bit Schema Version.
- 16-bit Message Type ID.
- 32-bit Sequence / Acknowledgement Numbers.
- Bounded payload bytes with validated length headers.

Replication payloads further carry ADR-099 authority epoch, network object ID/
generation, authoritative tick, schema ID/version and canonical ascending stable
`FieldId` records. Property paths, native offsets and component memory layouts are
never protocol identity.

`NetworkRuntime` performs handshake negotiation after the transport reports
`Connected`. Until the session is `Active`, it accepts only bounded admission
messages on the reserved control channel. Early gameplay, RPC, replication, voice,
console and extension messages are rejected and never buffered for later dispatch.

`HandshakeNegotiator` is the owner-thread, backend-neutral compatibility seam. Its
construction copies a host-owned local policy into fixed-capacity storage. One peer
offer is then validated synchronously against the exact connection and admission
generation, deadline, protocol/version interval, schema fingerprint, mandatory
features, compression policy and immutable transport-capability revision. Acceptance
publishes one immutable `HandshakeSelection`; rejection, timeout, cancellation and
shutdown are terminal and late or replacement-generation calls cannot alter the
result. The seam does not parse native packets, authenticate principals, select a
transport, activate gameplay or retain borrowed offer storage.

`AuthenticationSessionAdapter` is the next owner-thread, backend-neutral seam. The
host supplies its immutable `NetworkTrustPolicySnapshot`, fresh transcript-bound
`AuthenticationChallenge`, absolute deadline and borrowed credential, certificate,
private-key and peer-verification authorities. The adapter validates bounded response
framing and normalized transport/certificate evidence before invoking any authority.
Authentication proof bytes remain a synchronous borrow into the credential authority;
they are never copied into adapter state, accepted output, errors or the redacted
`AuthenticationDiagnostics` projection. Provider failures are translated to closed
safe authentication classes without forwarding provider text.

Every authority completion carries the exact connection and admission generation plus
a non-zero authority generation. The accepted principal and private-key authority's
`SecureChannelHandoff` are committed atomically only when all completion fences match,
the principal is canonical and unexpired, and the handoff names the exact transport
channel generation. Remote admission requires confidentiality, integrity,
authenticated peer evidence and product-anchor trust; LAN requires paired or
product-anchor trust. Only an explicitly configured, proven in-memory loopback may
omit transport protection. Unavailable authorities, weaker trust, malformed evidence,
timeout, cancellation and shutdown all fail closed and cannot publish late results.

`PeerSessionLifecycle` owns the next created-through-closed gameplay-session boundary.
It pins the exact `HandshakeSelection` and `AuthenticationResult`, admits activation
only for the authenticated secure-channel generation/binding digest, and admits
gameplay only while that same connection/session generation is `Active` and within
credential expiry, inactivity and absolute-lifetime deadlines. Activity extends only
the inactivity deadline and never credential authority or the immutable lifetime
ceiling.

Protocol rejection, authentication rejection, transport failure, local cancellation,
each stage/activity timeout, graceful local/remote close and shutdown remain distinct
typed terminal reasons. Graceful close pins its reason before transport drain; a later
transport failure or shutdown completes that close without replacing the first reason.
Every other terminal path publishes once, clears gameplay admission and rejects late
messages/callbacks or replacement generations. Transport close remains externally
owned and cannot rewrite the session snapshot.

The canonical hello exchange includes:

- Product/protocol family identity and minimum/maximum wire versions.
- Gameplay schema-set fingerprint and compatibility epoch.
- Required/optional capability IDs, compression and MTU limits.
- Trust-policy ID/revision, transport protection evidence and fresh nonces.

The server selects the highest mutually supported version above both configured
compatibility/security floors. Required capabilities and schema fingerprints use
an explicit compatibility table; no string comparison, native-layout inference or
silent downgrade is permitted. Both peers bind verification and activation to a
canonical transcript digest.

Mismatched protocol versions, trust failures or invalid credentials fail the
**session** with a typed safe error and then close the transport. Transport
backends must not interpret application credentials.

GNS connection encryption and packet integrity are private transport capabilities;
they do not authenticate a Horo user, authorize a session or replace application
protocol negotiation. `NetworkRuntime` owns credential/token exchange, peer trust,
session admission and replication authority.

NAT traversal is an optional capability boundary. A concrete adapter may own an
ICE/STUN/TURN or provider-native mechanism, while `NetworkRuntime` and the host own
signaling policy, credential acquisition and provider selection. The baseline GNS
composition does not enable traversal, Steam signaling/authentication, Steam
Datagram Relay, WebRTC or console-provider SDKs.

### Exposure and Admission Policy

The host supplies an immutable `NetworkTrustPolicySnapshot` before listener/connect
creation. Effective bind scope must agree with its exposure:

| Exposure | Required channel/peer policy | Application admission |
|---|---|---|
| Loopback development | Proven loopback bind/peer; native encryption when supported; Null may remain in-memory | Explicit local principal or configured credential |
| Local network | Encryption/integrity plus product trust anchor, exact pin or explicit out-of-band pairing | Credential or explicitly configured bounded LAN guest principal |
| Remote | Encryption/integrity plus authenticated server identity | Required short-lived transcript/nonces/server-bound admission proof |

IP/private-subnet identity, successful key exchange and encrypted-but-
unauthenticated transport are not peer authentication. A remote connection cannot
become active without both server trust and application admission. Provider/native
identity is verifier evidence, not gameplay authority.

Successful verification creates an immutable Horo `SessionPrincipal` containing a
fresh random session ID, principal ID, trust level, bounded roles/capabilities,
credential provenance and expiry. Client claims never grant authority directly.
Expiry or revocation closes the M0 session; transparent reauthentication is not
part of this baseline.

Admission has finite byte/message/field, pre-active connection, verifier-work,
attempt, per-source rate, negotiation and activation limits. Cheap canonical
parsing and compatibility checks precede expensive verification. Timeout,
cancellation or shutdown invalidates the admission generation, so late transport
or verifier completion cannot activate a session.

## Delivery Semantics and Backpressure

[ADR-070](../../adr/070-capture-and-voice-io-ownership.md) keeps voice packet policy
in NET. Network voice may consume bounded timestamped PCM from Audio and return
validated remote PCM through the playback-source seam, but NET retains framing,
codec/packet negotiation, encryption, peer/session routing, jitter/reorder/loss,
bitrate, QoS, mute/block and moderation. Audio never receives sockets, packet
headers, credentials or remote peer authority.

Delivery policies:

- **UnreliableUnordered**: Fast state updates (e.g. high-frequency physics/transform telemetry); latest packet overwrites previous.
- **UnreliableSequenced**: Discards out-of-order packets; only newer packets are accepted.
- **ReliableOrdered**: Guaranteed delivery in order (e.g., game events, inventory actions, chat).
- **ReliableUnordered**: Guaranteed delivery without strict ordering constraints.

### Backpressure and Overload Policies

All transport queues have bounded capacities:

- **DropOldestSnapshot**: When the outbound queue for unreliable state exceeds threshold, older snapshots are discarded in favor of the latest state.
- **RejectSend**: Reliable message requests exceeding queue limits return `NetworkErrorCode::QueueFull`.
- **DisconnectAbusivePeer**: Inbound queues experiencing sustained flood without consumption trigger connection termination.

## Deterministic Cancellation and Shutdown

- **Cancellation**: Connect and resolve requests accept `CancellationToken`. Triggering cancellation halts DNS lookups and connection attempts, closes any native attempt and reports exactly one terminal `NetworkErrorCode::OperationCancelled` event through `PollEvents()`. Generation checks discard late native callbacks.
- **Graceful Teardown**: Session close asks the transport to `Close()`. The transport transmits a disconnect frame and starts a bounded drain timer. Upon expiry or peer ACK, native resources are reclaimed.
- **Process Shutdown Sequence**:
  1. The host calls `NetworkRuntime::Shutdown()`.
  2. Runtime stops admission, invalidates verification generations, removes every session from gameplay dispatch and cancels verifier work.
  3. Runtime calls `INetworkTransport::Shutdown()`: listeners stop accepting, pending connects are cancelled, connections send disconnect notices and perform only the bounded close drain.
  4. The backend stops and joins its I/O thread, drains or explicitly discards native and normalized messages under the shutdown policy, then releases native library state.
  5. Runtime destroys the unique transport. No callback may target it after destruction.
  6. Session pools and dispatcher tables are freed.

## Runtime Network Modes and Authority Exposure

[ADR-102](../../adr/102-runtime-network-modes-and-authority-exposure.md)
separates the immutable modes supported by a product artifact from the one mode
selected for a host generation. Runtime flags and project configuration can select
only a declared mode; they cannot add a missing target/backend or synthesize
server authority.

| Runtime mode | World composition | Network service ownership | Gameplay authority exposure |
|---|---|---|---|
| `Standalone` | One standalone world | Omitted by default; an explicit Null facade remains inert | Normal local Scene/Gameplay ownership without a network session, grant or authority epoch |
| `Client` | One client world | Outbound attempts and Active sessions owned by `NetworkRuntime`; no listener | Autonomous/simulated capabilities only through the current admitted session and object grants |
| `ListenServer` | Separate authority-server and local-client worlds | Server listener/admission plus an explicitly admitted loopback client session | Server authority appears only in the server world; locality never grants the client world authority |
| `DedicatedServer` | One authority-server world | Server listener, admission and sessions; no outbound gameplay client | Server-world authority with no local player, renderer, audio, input or window requirement |

The host validates the whole world/listener/outbound/local-player matrix before
publishing a world. Gameplay receives an immutable `GameplayNetworkRoleView`
scoped by plan, host, scene and optional session/authority generations. Privileged
operations revalidate those generations at the owner safe point. A process-global
`IsServer`, listener presence, local player, loopback address, executable name,
headless state or service-locator result is never authority.

Mode is fixed for one host generation. Travel replaces world/authority generations
without changing mode; reconnect creates a new session generation. Disconnect
unpublishes the client session view and cannot promote a client to standalone.
Shutdown unpublishes views and invalidates grants before worlds, sessions,
listeners or transports are destroyed.

Immersive Editor AI under
[ADR-172](../../adr/172-immersive-agent-ownership-authoring-mode-and-risk.md)
does not alter this authority model. Local XR presence, an agent/MCP session,
voice or physical interaction cannot mint a peer, session, object or server-world
grant. Remote participants cannot impersonate local proposal approval, and
network content remains untrusted context rather than editor-tool authority.

## Project Configuration and Product Capability

[ADR-103](../../adr/103-network-project-configuration-and-build-profile-ownership.md)
specializes the Foundation configuration resolver without creating a Network-local
precedence implementation. The Network module contributes inert `network.*`
descriptors and projects the resolved snapshot into one validated
`EffectiveNetworkConfiguration`. Editor, CLI, MCP, CI and packaged hosts call the
same application use case and receive the same typed diagnostics/provenance.

Portable project policy owns protocol/profile/trust/provider intent; local user
configuration owns preview-only preferences; the product release profile owns
supported ADR-102 modes, exact targets/providers, security floors and qualification
evidence; a host request selects only within that envelope. Renderer backend,
device/quality tier, developer CMake profile and headless state cannot select a
network mode, transport or ADR-101 scheduling profile.

Packaging emits a safe versioned `NetworkProductCapabilityManifest`. It describes
supported modes/providers, public compatibility/security evidence and permitted
override bounds, but grants no authority. Project files and manifests contain no
raw credential/private-key values or private credential bindings. They declare
stable credential requirements; private host/CI input binds an opaque provider
reference, and the consuming operation resolves a short-lived value/operation
handle. Missing mode/provider/credential/trust capability fails explicitly without
fallback.

Every preview, release job and runtime activation pins the effective configuration
revision. Migration is versioned and cannot infer network values from renderer
tiers. Runtime mode/provider/exposure changes require ADR-102 host recomposition;
existing sessions keep their negotiated generations until bounded replacement or
shutdown.

The portable `NetworkProjectSettings` codec accepts a closed, bounded JSON object.
Version 2 adds a canonical optional default endpoint and a public numeric credential
requirement ID. Version 1 documents omit those two fields; pure migration supplies
an absent endpoint and no credential requirement before complete validation. Unknown
fields, duplicate keys, malformed integers, and future versions fail. The codec
never stores a private credential binding or secret. Default construction creates
a standalone-only policy with finite scheduling limits; projects must explicitly
declare network roles and required transport capability evidence before packaging
or activation. `PreflightNetworkProjectSettings` validates the selected role and
exact transport evidence before an operation publishes state.

## Optional Composition and Product Configurations

Horo Engine products declare only the modes and network targets they can realize:

| Configuration | Composed Targets | Behavior |
|---|---|---|
| **Editor / IDE** (`HoroEditor`) | `NetworkApi`, `NetworkRuntime`, `NetworkTransportGNS`, optional Null test backend | Validates an explicit standalone/client/listen/dedicated preview plan; no editor-only authority role. |
| **Network-capable game artifact** | `NetworkApi`, `NetworkRuntime`, selected qualified transport(s) | May support client/listen/dedicated modes according to its cooked product capability declaration. |
| **Standalone artifact** | Network targets omitted by default; optional `NetworkApi`, `NetworkRuntime`, `NetworkTransportNull` test facade | Standalone role only; the optional facade creates no listener, session, replication or authority epoch. |
| **Dedicated-server artifact** | `NetworkApi`, `NetworkRuntime`, `NetworkTransportGNS` | Headless dedicated plan without renderer, audio, input, window or local-player dependencies. |
| **Tooling / Packager** (`horopak`) | *None* | Links zero network targets; compiles cleanly with minimal footprint. |

## Observability and Diagnostics

Networking integrates with Horo's diagnostic and metric infrastructure:

- `NetworkFailureKind` maps each supported terminal path to exactly one declared
  `horo.network` error descriptor. `NetworkFailureLayer` and
  `NetworkFailureDisposition` provide backend-neutral branching for transport,
  protocol, session, replication, and gameplay-dispatch outcomes; adapters never
  branch on fallback message text.
- `NetworkTerminalRecord` owns at most eight strictly ordered typed context fields.
  Protocol-scoped close reasons carry both `ProtocolId` and `CloseReasonId`;
  connection evidence is an opaque slot/generation projection, never a native
  handle or authenticated peer identity.
- Private adapters normalize native diagnostic text through a bounded inspection
  boundary. No source byte, credential, token, payload, native code, or provider
  type is retained in the public record. Disabling instrumentation removes only
  evidence flags and cannot change the canonical terminal kind, disposition, or
  lifecycle result.
- Each owner-thread connection slot accepts exactly one immutable terminal result
  for its current generation. Duplicate completion loses deterministically;
  cancellation, shutdown, and success use the same publication gate. Replacement
  requires the exact next non-wrapping handle generation, so late callbacks cannot
  terminate a new connection.

- **Counters**: `net.bytes_sent`, `net.bytes_received`, `net.packets_lost`, `net.packets_dropped`.
- **Gauges**: `net.active_connections`, `net.inbound_queue_depth`, `net.outbound_queue_depth`, `net.rtt_ms`.
- **Tracing**: Transport connection events and session handshakes log to the `LogCategory::Network` category. Payloads are scrubbed of sensitive data by default.

The host may compose `NetworkMetrics` with the selected transport, I/O service,
peer-session lifecycle, and replication-world lifecycle. A metrics owner outlives
those services and shuts them down before closing or destroying the collector.
Only the owning network thread records measurements. I/O producers retain a
separate shared admission flag, not the collector, so a late completion cannot
access a retired metric owner. Closing the collector disables producer-side
capacity-drop accounting immediately. The host calls `Publish` at a network
safe point; `Snapshot` returns the last coherent fixed-size value to editor,
headless, or other readers without touching in-flight network state. Process
composition alone registers telemetry descriptors and passes pre-bound handles
to `NetworkMetricPublisher`; no network component chooses an exporter.

Transport-category outbound counts are admitted send requests; inbound counts
are delivered normalized transport events. The deterministic transport counts
each emitted fragment as a received event and its actual emitted bytes; seeded
loss does not fabricate receive traffic.
Additional closed control/replication/RPC categories are semantic observations,
not partitions of those wire totals. Replication mapping counters count only
successful register/retire operations, not unimplemented wire spawns or updates.
`net.packets_lost` is qualified by `lossAvailable`: a backend without an actual
loss counter does not turn absence of evidence into zero loss. RTT is the mean
of owner-observed finite connection samples in one publication window and is
unavailable when that window contains none. Queue and connection gauges are
owner-safe-point values. Every series name and category is compiled from a
closed vocabulary; peer handles, addresses, protocol IDs, message IDs, payloads,
and native diagnostics never become metric dimensions. Saturation clamps
counters and marks the snapshot; instrumentation failure never affects network
admission, send, terminal state, or replication authority.
The new observer parameters on existing network factories default to null;
existing callers keep their behavior and need no migration. Hosts opting in
must enforce the documented owner and shutdown order.

## Testing and Verification Strategy

The networking subsystem requires targeted automated verification:

1. **Unit Tests (`tests/unit/runtime/networking/`)**:
   - `NetworkConfigurationTests`: legal source/precedence matrix, security-floor constraints, cross-surface parity, renderer/build-profile isolation and unavailable capability errors.
   - `NetworkArtifactPlanTests`: mode/provider target inventory, canonical manifest, migration and proof that credentials/private bindings never enter public outputs.
   - `NetworkAddressTests`: IPv4, IPv6, hostname, loopback, port parsing, serialization.
   - `TransportConnectionStateTests`: `Connect()` returns immediately; `Created` / `Resolving` / `Connecting` / `Connected`; invalid transitions; timeout.
   - `SessionStateMachineTests`: `Negotiating` / `Authenticating` / `Activating` / `Active`; only Active reaches gameplay dispatch.
   - `SessionAdmissionTests`: protocol/schema compatibility, transcript binding, exposure/bind policy, principal creation, expiry and revocation.
   - `NetworkHandleTests`: stale handle -> `InvalidHandle`; send-after-close -> `ConnectionClosed`; duplicate `Close` is idempotent.
   - `NetworkFailureTests`: complete canonical kind/layer/disposition mapping,
     bounded typed context, private-detail redaction, instrumentation-disabled
     parity, exactly-once terminal publication, cancel/shutdown ordering, stale
     completion rejection and non-wrapping generation replacement.
   - `SendPayloadLifetimeTests`: caller buffer may be overwritten after `Send()` returns.
   - `ReplicationManagerTests`: ADR-175 safe-point capture and immutable publication; lost/duplicate dirty hints, stale identity, cancellation, shutdown, delta compression and interest queries.
   - `ReplicationSerializerTests`: complete owner/type/codec binding, immutable adapter lifetime pins, canonical scalar encoding,
     explicit quantization equivalence, field bounds, incompatible tags and malformed payload rejection.
2. **Deterministic Transport Tests (`NetworkTransportNullTests`)**:
   - No `Platform` link; no OS sockets; no I/O thread.
   - Simulated latency, jitter, packet loss, duplicate packets, and out-of-order delivery.
   - `PollEvents()` drains the transport-owned in-memory queue on the caller thread.
   - Graceful disconnect and timeout handling.
3. **Integration & Lifecycle Tests**:
   - Complete standalone/client/listen/dedicated plan matrix, including unsupported package modes and incompatible world/listener/outbound/local-player capabilities.
   - World-scoped role exposure rejects authority inferred from flags, headless state, listeners, local players, loopback or same-process pointers.
   - Listen-server worlds remain isolated through admitted loopback, travel and local-client disconnect; client disconnect never promotes to standalone.
   - Travel/reconnect replace scene, authority and session generations; stale grants, role views and late work cannot mutate or republish state.
   - Full client-server connect, session authenticate, transfer, and disconnect sequences.
   - Connected peers sending early gameplay never reach gameplay/replication callbacks.
   - Loopback, LAN and remote valid flows; wrong pins/roots, unauthenticated encryption, invalid/expired/revoked credentials and downgrade/replay attempts.
   - Bounded malformed/truncated/oversized admission input, rate/flood limits and verifier exhaustion.
   - Cancellation during DNS and connect; `Connect()` already returned a handle.
   - Negotiation, verification and activation timeout/cancellation races discard late completions.
   - Unique-ptr injection: runtime shutdown destroys the transport exactly once.
   - Process shutdown with active connections and pending queued messages without hangs or memory leaks.
4. **GNS Adapter Contract Tests (`NetworkTransportGNSTests`)**:
   - Successful direct-IP IPv4/IPv6 listen, connect, reliable/unreliable transfer, close and reconnect.
   - Zero/max message, connection, listener, channel/lane and queue boundaries; unsupported capability combinations fail explicitly.
   - Malformed, truncated, oversized, duplicate, reordered and flood input is rejected without unbounded allocation or runtime mutation.
   - Cancellation racing native success/failure publishes one terminal event and cannot revive a stale handle.
   - Shutdown before initialization, after partial failure and with active native callbacks/messages is bounded, idempotent and callback-free after destruction.
   - Dependency builds and smoke tests cover every claimed platform/toolchain and crypto-provider configuration; sanitizer/fuzz evidence is retained where supported.

## Related Documents

- [ADR-020: Network Target Ownership and Dependency Boundary](../../adr/020-network-target-ownership-and-dependency-boundary.md)
- [ADR-097: Default Real-Time Transport Backend](../../adr/097-default-real-time-transport-backend.md)
- [ADR-098: Protocol, Session and Trust Policy](../../adr/098-protocol-session-and-trust-policy.md)
- [ADR-099: Replication Ownership, Authority and Compatibility](../../adr/099-replication-ownership-authority-and-compatibility.md)
- [ADR-100: Prediction Capability Tiers and Determinism Policy](../../adr/100-prediction-capability-tiers-and-determinism-policy.md)
- [ADR-101: Interest, Priority and Network Budget Model](../../adr/101-interest-priority-and-network-budget-model.md)
- [ADR-102: Runtime Network Modes and Authority Exposure](../../adr/102-runtime-network-modes-and-authority-exposure.md)
- [ADR-103: Network Project Configuration and Build-Profile Ownership](../../adr/103-network-project-configuration-and-build-profile-ownership.md)
- [System Design](../foundation/system-design.md)
- [Desired Project Trees](../desired-project-tree.md)
- [Multiplayer Replication Architecture](./multiplayer-replication-architecture.md)
- [Runtime Lifecycle](./runtime-lifecycle.md)
- [Concurrency And Job System](../foundation/concurrency-and-jobs.md)
- [Network Debugger UI Reference](./network-debugger.html)
- [Application Security Architecture](../security/application-security.md)
