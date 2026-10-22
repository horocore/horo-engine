# Network foundation and deterministic scenario qualification

This document maps the M3 networking bundle to concrete implementation and
executable coverage. Test presence is not a passing result. The PR validation
record owns the exact revision, platform, commands and current hosted results.

## Transport foundation — HORO-1088 / #1088 / [NET-001]

| Acceptance | Implementation and executable evidence |
| --- | --- |
| Backend-neutral producer/consumer contract | `NetworkTransport.h`, generation-checked `NetworkHandles.h`, staged `HoroNetworkApi` headers; `HoroNetworkApiTests` and public-header consumers. GNS native/crypto types remain private. |
| Non-blocking latency-sensitive phases and bounded overflow | `NetworkIoService`, `PacketQueue`, `TransportBudgetController`, `DeterministicTransport` and GNS queue admission; budget, I/O, packet queue and qualification tests. |
| Bounded listener/connection/send terminal outcomes | `NetworkLifecycleRegistry`, cancellation/shutdown tests, deterministic loss admission results, GNS transport/qualification tests. A simulated result is not native socket evidence. |
| Products can omit concrete transports | `HoroNetworkDisabledLinkProbe`, `HoroNetworkNullLinkProbe`, `AssertNetworkTransportLinkage.cmake`; four product launch/composition paths. |
| Fragmentation, delivery, saturation, cancellation, failure and shutdown | `DeterministicTransportQualificationTests`, shared `TransportQualificationContract`, `GnsTransportQualificationTests`, `NetworkLifecycleTests`, `NetworkIoServiceTests`. Native GNS must run in an enabled compatible build before claiming its qualification. |

## Secure protocol/session — HORO-1089 / #1089 / [NET-002]

**Integration incomplete.** The executable reference product in
`apps/horo-engine/NetworkProductLaunch.cpp` deliberately installs empty protocol
and codec registries and no credential authority. Its inbound router is
fail-closed before Active; it does not complete an authenticated session.
`AuthenticationSessionAdapter` and `PeerSessionLifecycle::Create` have no
production composition caller in this checkout. The coverage below proves
individual contracts with explicitly injected test authorities, not an integrated
production negotiation/authentication/admission path. This bundle must not close
the parent on that evidence alone. Production completion requires host-injected
credential/trust authorities, explicit bounded pre-active handshake routing,
revocation/timeout ownership and a successfully admitted typed dispatch path;
there is no safe static credential or fake-session fallback.

| Acceptance | Implementation and executable evidence |
| --- | --- |
| Only compatible authenticated active peers dispatch | `HandshakeNegotiation`, host-injected `AuthenticationSessionAdapter`, `PeerSessionLifecycle`, `InboundMessageDispatcher`; corresponding runtime tests reject pending, stale, incompatible and untrusted sessions. |
| Bounds/schema checks precede decode and owner access | `MessageEnvelope`, `MessageCodecRegistry`, bounded dispatcher staging; envelope malformed/version/fragmentation and dispatcher tests. |
| Distinct cancellation/timeout/rejection/transport terminals | `PeerSessionLifecycle`, `NetworkLifecycle`, typed `NetworkFailure`; lifecycle/failure tests and admission protections. |
| Diagnostics exclude secrets/payload | `NetworkDiagnostics`, `NetworkDebugger`, typed sanitized evidence; diagnostics, debugger and disabled-instrumentation tests. The chaos owner accepts no credential authority. |
| Fragmentation/coalescing, malformed frames, skew, auth failure, replay, saturation, shutdown | `MessageEnvelopeTests`, `HandshakeNegotiationTests`, `AuthenticationSessionAdapterTests`, `AdmissionProtectionTests`, `MessageDeliveryGateTests`, `InboundMessageDispatcherTests`, `PeerSessionLifecycleTests`. |

## Replication declarations — HORO-1090 / #1090 / [NET-003]

**Integration incomplete.** The headless product now consumes the canonical
policy against its actual loaded native or Lua generation. Native admission runs
after inert registration freezes and before service factories or module Start;
Lua admission precedes behavior activation. Both retain the accepted generation
through behavior shutdown and release it before native retirement. Actual world
roles select server/client operations, and a no-gameplay world checks the complete
inventory against no installed modules. Unknown inventory, missing required code
and incompatible descriptors fail closed. Editor inspection and cook assessment
consumers remain missing; runtime integration alone cannot close the parent.
The PR validation record must distinguish these added regressions from executed
current-source tests.

| Acceptance | Implementation and executable evidence |
| --- | --- |
| Canonical descriptors across runtime/editor/cook/compatibility | Product activation consumes project inventory and actual frozen gameplay descriptors through `GameplayReplicationAdmissionPolicy`; editor/cook assessment consumers remain missing. |
| Identity survives rename/layout changes | Explicit schema/field/RPC IDs and canonical serializers, independent `NetworkObjectIdentity`/local mappings; descriptor rename and serializer/mapping tests. |
| Explicit authority and mutation phases | `ReplicationRoles`, `ReplicationWorldLifecycle`, `RpcGameplayDispatch`, `ReplicationInboundApply` and Scene-owned safe-point commits; role, permissions, RPC and actual Scene apply tests. |
| Deterministic missing/duplicate/skew outcomes | Registry, descriptor/declaration compatibility, missing native/script metadata and Lua registration tests. |
| Registration/evolution/permissions/missing-code/native-script parity | `HoroNetworkApiTests`, `HoroNetworkRuntimeTests`, `HoroGameplayRuntimeTests`, `HoroLuaBehaviorTests`, replication codec/capture/apply targets and header consumers. Target names must be confirmed against the configured composition. |

## Host modes — HORO-1162 / #1162 / [NET-007.2]

`NetworkProductHost` admits the package/product/host capability matrix before
factories. `NetworkModeComposition` creates the selected optional participants;
`HeadlessNetworkServices` composes the ordinary Scene/Physics/Gameplay owners.
Standalone omits network factories; dedicated omits presentation and local player
services. Listen mode owns separate authority/client worlds. However, the reference
executable's `SessionOrLocalPlayerService` is an inert/no-op participant, and its
transport participant has no production authenticated session admission. This is
not evidence of completed accepted-mode session/replication integration; the
broader composition work must replace that placeholder with explicit real owners
and externally supplied trust before claiming the full ticket complete.

`NetworkModeHostIntegrationTests` exercises all four concrete compositions,
admission, travel, disconnect and shutdown. Added regression coverage queues
client travel and then disconnects or shuts down before the canonical lifecycle
safe point. The original world remains intact on stale-session rejection; stopped
hosts publish no replacement world or admitted role. With native Physics disabled,
these tests exercise explicit startup rejection only and cannot prove successful
host lifecycle behavior. `NetworkModeCompositionTests` separately tests pure
composition, rollback and role fencing.

## Chaos harness — HORO-1201 / #1201 / [NET-010.6]

`NetworkChaosScenario` adds a fixed-capacity copied timeline and topology,
consecutive relative ticks, seed-preserving impairment revisions, stalls/resume,
exact-generation disconnect/restart, replay-pinned drain capacity and expected
checkpoints to the existing seeded simulated transport. Failure of a checkpoint
is terminal. Disabled/default profiles fail before transport construction. The
owner only constructs in-memory transport and checks the selected host/session
scope for every operation; it has no production transport or global discovery
surface.

Qualification covers identical replay traces, queued-work behavior across
impairment changes, loss/duplication/reordering/jitter, saturation during stalls,
stale/foreign scope, generation exhaustion, hostile late actions, copied input
and move lifetime, insufficient event buffers, explicit shutdown, no-allocation
successful operations and disabled evidence with checkpoint enforcement.
Restart-specific coverage retains the actual old-generation disconnect across
stalls and a saturated drain budget, rejects next-generation sends before that
event is returned, fences old handles, moves pending ownership, cancels deferred
opens on explicit close/shutdown/duration and terminalizes overlapping actions.
The existing impaired RPC fixture now composes the same timeline core for real
session/envelope replay and Gameplay dispatch tests, and for the runtime scheduler
integration that applies replication to an actual Scene only at its declared safe
point. This does not replace production backend qualification.

### Outstanding external dependency

Native blocker #1177 [NET-008.6] remains open. Its built-in/project/user preset
storage, authorized live/timeline editor commands, immutable editor projection,
shared widgets/localization and editor lifecycle qualification are outside this
bundle. This change supplies the deterministic runtime/test scenario core and
headless integration; it does not implement that editor workflow, remove its
native dependency relation or claim that issue complete. Any final #1201 closure
must explicitly distinguish its harness criteria from the still-open editor
prerequisite and the actual validation available at the PR revision.

## Public contract migration

The added scenario header belongs only to `HoroNetworkTransportNull`; no
`NetworkApi` or production transport dependency exposes fault injection. Existing
static-scenario callers keep their semantics. `ReplaceScenario` is additive and
rejects fragment-storage or seed changes, stale revisions and terminal owners.
No project settings, credentials or serialized replication identities migrate.

`GameModuleHostOptions` and the non-installed headless composition inputs add an
optional owned replication admission policy. Existing non-network constructors
intentionally omit it. Rebuild host consumers of these C++ layouts; native module
ABI contexts and SDK fingerprints are unchanged. Actual network world factories
require a complete inventory, including the explicitly empty case. A factory-level
`Count` operation means unresolved until the actual world request selects server
or client; an explicit mismatching operation is rejected. Module/Lua admission
accepts only resolved server/client operations. Editor inspection and cook never
authorize runtime activation. Public header ownership remains with the existing
Gameplay targets and their declared NetworkApi dependency.
