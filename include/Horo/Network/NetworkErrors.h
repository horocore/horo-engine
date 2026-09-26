#pragma once

/**
 * @file NetworkErrors.h
 * @brief Stable backend-neutral network error identities.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Network::NetworkErrors {
    /** @brief Endpoint text is malformed or has an ambiguous non-canonical representation. */
    extern const ErrorCodeDescriptor NetworkAddressInvalid;
    /** @brief Endpoint text uses a deliberately unsupported scheme, zone, or IDNA representation. */
    extern const ErrorCodeDescriptor NetworkAddressUnsupported;
    /** @brief Endpoint text exceeds the finite public address bound. */
    extern const ErrorCodeDescriptor NetworkAddressCapacityExceeded;
    /** @brief A transport handle is malformed, stale, or does not match the current owner generation. */
    extern const ErrorCodeDescriptor TransportHandleInvalid;
    /** @brief A reclaimed handle slot cannot advance its generation without wrapping. */
    extern const ErrorCodeDescriptor TransportGenerationExhausted;
    /** @brief Backend composition identity, descriptor, or operation is malformed. */
    extern const ErrorCodeDescriptor TransportBackendInvalid;
    /** @brief The exact requested backend is not installed in this host composition. */
    extern const ErrorCodeDescriptor TransportBackendUnavailable;
    /** @brief An installed backend is not supported on this host. */
    extern const ErrorCodeDescriptor TransportBackendUnsupported;
    /** @brief An installed backend has no admitted complete configuration. */
    extern const ErrorCodeDescriptor TransportBackendNotConfigured;
    /** @brief A backend identity was already registered or another backend selected. */
    extern const ErrorCodeDescriptor TransportBackendConflict;
    /** @brief The host's finite backend registration capacity is exhausted. */
    extern const ErrorCodeDescriptor TransportBackendCapacityExceeded;
    /** @brief A factory returned no instance or threw before activation. */
    extern const ErrorCodeDescriptor TransportBackendFactoryFailed;
    /** @brief Backend selection or activation was cancelled by the composition owner. */
    extern const ErrorCodeDescriptor TransportBackendCancelled;
    /** @brief Backend composition has entered terminal shutdown. */
    extern const ErrorCodeDescriptor TransportBackendShuttingDown;
    /** @brief A replicated authority epoch or object slot/generation is malformed. */
    extern const ErrorCodeDescriptor NetworkObjectIdentityInvalid;
    /** @brief A retired replicated-object slot cannot advance without generation wrap. */
    extern const ErrorCodeDescriptor NetworkObjectGenerationExhausted;
    /** @brief Network-object mapping construction, provenance, scene, or lifecycle input is malformed. */
    extern const ErrorCodeDescriptor NetworkObjectMappingInvalid;
    /** @brief A network-object slot or local entity is already mapped to a live occurrence. */
    extern const ErrorCodeDescriptor NetworkObjectMappingConflict;
    /** @brief A network-object identity or local entity is absent, retired, or stale. */
    extern const ErrorCodeDescriptor NetworkObjectMappingUnknown;
    /** @brief The session-owned mapping exhausted its prepared unique-slot capacity. */
    extern const ErrorCodeDescriptor NetworkObjectMappingCapacityExceeded;
    /** @brief The session-owned mapping is shutting down or invalidated with its scene. */
    extern const ErrorCodeDescriptor NetworkObjectMappingTerminal;
    /** @brief Packet pool descriptor is malformed. */
    extern const ErrorCodeDescriptor PacketBufferInvalid;
    /** @brief Packet bytes or prepared storage exceed finite capacity. */
    extern const ErrorCodeDescriptor PacketBufferCapacityExceeded;
    /** @brief Every prepared packet slot is leased. */
    extern const ErrorCodeDescriptor PacketBufferPoolExhausted;
    /** @brief Queue descriptor or packet metadata is malformed. */
    extern const ErrorCodeDescriptor PacketQueueInvalid;
    /** @brief A packet cannot fit the queue's declared byte bounds. */
    extern const ErrorCodeDescriptor PacketQueueCapacityExceeded;
    /** @brief Explicit reject/replace policy could not admit a packet. */
    extern const ErrorCodeDescriptor PacketQueueFull;
    /** @brief A stable network identity uses its reserved zero representation. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief Network project settings contain an unknown, malformed, or incoherent typed value. */
    extern const ErrorCodeDescriptor NetworkProjectSettingsInvalid;
    /** @brief Network project settings exceed an explicit finite profile or queue bound. */
    extern const ErrorCodeDescriptor NetworkProjectSettingsCapacityExceeded;
    /** @brief A project-settings replacement was based on an older immutable revision. */
    extern const ErrorCodeDescriptor NetworkProjectSettingsStale;
    /** @brief Project-settings publication is closed and rejects late commands. */
    extern const ErrorCodeDescriptor NetworkProjectSettingsShuttingDown;
    /** @brief Replication descriptor metadata is malformed or exceeds its declared bounds. */
    extern const ErrorCodeDescriptor ReplicationDescriptorInvalid;
    /** @brief Schema, field, or tombstone identities collide in one candidate snapshot. */
    extern const ErrorCodeDescriptor ReplicationDescriptorConflict;
    /** @brief A replacement reuses stable identity with incompatible semantics. */
    extern const ErrorCodeDescriptor ReplicationDescriptorIncompatible;
    /** @brief An exact replication schema identity is absent from the pinned snapshot. */
    extern const ErrorCodeDescriptor ReplicationSchemaUnknown;
    /** @brief Descriptor snapshot construction exceeded its explicit finite capacity. */
    extern const ErrorCodeDescriptor ReplicationCapacityExceeded;
    /** @brief RPC declaration metadata, routing, permissions, or bounds are malformed. */
    extern const ErrorCodeDescriptor RpcDescriptorInvalid;
    /** @brief RPC or parameter stable identities conflict within one generation. */
    extern const ErrorCodeDescriptor RpcDescriptorConflict;
    /** @brief RPC declaration evolution is not explicitly compatible. */
    extern const ErrorCodeDescriptor RpcDescriptorIncompatible;
    /** @brief No exact accepted RPC declaration exists for the requested identity. */
    extern const ErrorCodeDescriptor RpcUnknown;
    /** @brief RPC declaration construction exceeded an explicit finite bound. */
    extern const ErrorCodeDescriptor RpcCapacityExceeded;
    /** @brief An RPC parameter has no exact accepted typed codec metadata. */
    extern const ErrorCodeDescriptor RpcParameterUnsupported;
    /** @brief Serializer metadata, quantization policy, or typed value representation is malformed. */
    extern const ErrorCodeDescriptor ReplicationSerializerInvalid;
    /** @brief Multiple serializers claim the same owner, semantic type, and codec identity. */
    extern const ErrorCodeDescriptor ReplicationSerializerConflict;
    /** @brief A declared schema field has no exact serializer binding in the pinned generation. */
    extern const ErrorCodeDescriptor ReplicationSerializerUnknown;
    /** @brief Serializer input, output, or registry construction exceeded an explicit finite bound. */
    extern const ErrorCodeDescriptor ReplicationSerializerCapacityExceeded;
    /** @brief Encoded type/codec tags do not match the exact declared field contract. */
    extern const ErrorCodeDescriptor ReplicationSerializerIncompatible;
    /** @brief A runtime value or canonical byte sequence is invalid for its declared typed codec. */
    extern const ErrorCodeDescriptor ReplicationSerializerValueInvalid;
    /** @brief A pinned replication role, peer, object, schema, or record context is malformed. */
    extern const ErrorCodeDescriptor ReplicationRoleContextInvalid;
    /** @brief A client role attempted to originate authority-server canonical state. */
    extern const ErrorCodeDescriptor ReplicationAuthorityDenied;
    /** @brief A role transition was based on a stale revision or committed at the wrong safe point. */
    extern const ErrorCodeDescriptor ReplicationRoleTransitionStale;
    /** @brief A second role transition was staged before the prior transition committed. */
    extern const ErrorCodeDescriptor ReplicationRoleTransitionPending;
    /** @brief Replication role state was accessed from a thread other than its owner. */
    extern const ErrorCodeDescriptor ReplicationRoleWrongThread;
    /** @brief Replication role state rejected work after shutdown. */
    extern const ErrorCodeDescriptor ReplicationRoleShuttingDown;
    /** @brief A replication world activation or identity tuple is malformed. */
    extern const ErrorCodeDescriptor ReplicationWorldInvalid;
    /** @brief A replication world request names a replaced Scene/session generation. */
    extern const ErrorCodeDescriptor ReplicationWorldStale;
    /** @brief A replication world is not currently active for the requested operation. */
    extern const ErrorCodeDescriptor ReplicationWorldUnavailable;
    /** @brief A replication world request names a phase not declared by the active world. */
    extern const ErrorCodeDescriptor ReplicationWorldPhaseInvalid;
    /** @brief A replication world candidate or retired-world ledger exceeded its bound. */
    extern const ErrorCodeDescriptor ReplicationWorldCapacityExceeded;
    /** @brief A replication world operation was cancelled before publication or admission. */
    extern const ErrorCodeDescriptor ReplicationWorldCancelled;
    /** @brief A replication world rejected work after shutdown began. */
    extern const ErrorCodeDescriptor ReplicationWorldShuttingDown;
    /** @brief Transport capability evidence or a bounded requirement is malformed. */
    extern const ErrorCodeDescriptor TransportCapabilityDescriptorInvalid;
    /** @brief Transport capability evidence changed after the caller captured its revision. */
    extern const ErrorCodeDescriptor TransportCapabilityStale;
    /** @brief The explicit transport candidate cannot provide an exact required delivery semantic. */
    extern const ErrorCodeDescriptor TransportDeliveryUnsupported;
    /** @brief Known transport functionality is not currently available. */
    extern const ErrorCodeDescriptor TransportCapabilityUnavailable;
    /** @brief A channel, message, or deadline requirement exceeds the candidate's finite limits. */
    extern const ErrorCodeDescriptor TransportLimitExceeded;
    /** @brief Transport budget policy, traffic metadata, or tick input is malformed. */
    extern const ErrorCodeDescriptor TransportBudgetInvalid;
    /** @brief A policy or submission exceeds prepared transport budget capacity. */
    extern const ErrorCodeDescriptor TransportBudgetCapacityExceeded;
    /** @brief A policy update was based on a replaced active revision. */
    extern const ErrorCodeDescriptor TransportBudgetPolicyStale;
    /** @brief Required reliable work was rejected without being silently discarded. */
    extern const ErrorCodeDescriptor TransportReliableBackpressure;
    /** @brief A queued-work completion names a released or replaced ticket generation. */
    extern const ErrorCodeDescriptor TransportBudgetTicketStale;
    /** @brief Caller-owned cancellation rejected transport admission before queue mutation. */
    extern const ErrorCodeDescriptor TransportOperationCancelled;
    /** @brief Caller-owned shutdown state rejected transport admission before queue mutation. */
    extern const ErrorCodeDescriptor TransportShuttingDown;
    /** @brief Host-scoped network I/O service construction or call bounds are invalid. */
    extern const ErrorCodeDescriptor NetworkIoServiceInvalid;
    /** @brief Prepared network I/O service storage could not be allocated. */
    extern const ErrorCodeDescriptor NetworkIoServiceCapacityExceeded;
    /** @brief A backend completion violates the closed immutable handoff contract. */
    extern const ErrorCodeDescriptor NetworkIoCompletionInvalid;
    /** @brief The prepared completion queue or current poll budget is exhausted. */
    extern const ErrorCodeDescriptor NetworkIoCompletionQueueFull;
    /** @brief Another transport thread is already polling this host-scoped service. */
    extern const ErrorCodeDescriptor NetworkIoPollBusy;
    /** @brief A retained completion producer belongs to a poll call that already returned. */
    extern const ErrorCodeDescriptor NetworkIoPollStale;
    /** @brief Completion drain was attempted from a thread other than the declared owner. */
    extern const ErrorCodeDescriptor NetworkIoWrongThread;
    /** @brief The service cannot advance a poll or completion sequence without wrapping. */
    extern const ErrorCodeDescriptor NetworkIoSequenceExhausted;
    /** @brief Listener or connection lifecycle input, bounds, or terminal evidence is malformed. */
    extern const ErrorCodeDescriptor NetworkLifecycleInvalid;
    /** @brief Prepared listener or connection lifecycle capacity is exhausted. */
    extern const ErrorCodeDescriptor NetworkLifecycleCapacityExceeded;
    /** @brief A listener or connection phase change is not legal from its current state. */
    extern const ErrorCodeDescriptor NetworkLifecycleTransitionInvalid;
    /** @brief A completion names a retired handle or asynchronous operation generation. */
    extern const ErrorCodeDescriptor NetworkLifecycleOperationStale;
    /** @brief Protocol identity contributions or version ranges are malformed. */
    extern const ErrorCodeDescriptor ProtocolIdentityDescriptorInvalid;
    /** @brief A protocol-scoped stable identity is registered more than once. */
    extern const ErrorCodeDescriptor ProtocolIdentityConflict;
    /** @brief An exact protocol-scoped identity is absent from the immutable registry. */
    extern const ErrorCodeDescriptor ProtocolIdentityUnknown;
    /** @brief Protocol version ranges have no explicit compatible overlap. */
    extern const ErrorCodeDescriptor ProtocolVersionIncompatible;
    /** @brief Protocol identity registry construction exceeded an explicit finite bound. */
    extern const ErrorCodeDescriptor ProtocolIdentityCapacityExceeded;
    /** @brief Handshake policy or peer input is malformed or exceeds its finite bounds. */
    extern const ErrorCodeDescriptor HandshakeInvalid;
    /** @brief Explicit protocol, feature, compression, schema, or transport requirements have no compatible selection. */
    extern const ErrorCodeDescriptor HandshakeIncompatible;
    /** @brief A handshake operation is not legal from the current terminal or negotiating state. */
    extern const ErrorCodeDescriptor HandshakeStateInvalid;
    /** @brief Authentication policy, challenge, evidence, proof framing, or authority output is malformed. */
    extern const ErrorCodeDescriptor AuthenticationInvalid;
    /** @brief Authentication input conflicts with the immutable challenge or exposure security floor. */
    extern const ErrorCodeDescriptor AuthenticationIncompatible;
    /** @brief A required host-owned credential, certificate, key, or peer-trust authority is unavailable. */
    extern const ErrorCodeDescriptor AuthenticationTrustUnavailable;
    /** @brief A host-owned authority rejected authentication without disclosing private provider detail. */
    extern const ErrorCodeDescriptor AuthenticationRejected;
    /** @brief Authentication is not legal from the current terminal or authenticating state. */
    extern const ErrorCodeDescriptor AuthenticationStateInvalid;
    /** @brief Message framing or codec metadata is malformed or non-canonical. */
    extern const ErrorCodeDescriptor MessageEnvelopeInvalid;
    /** @brief Declared message framing exceeds an explicit finite bound. */
    extern const ErrorCodeDescriptor MessageEnvelopeCapacityExceeded;
    /** @brief Message codec or field metadata conflicts with another stable identity. */
    extern const ErrorCodeDescriptor MessageCodecConflict;
    /** @brief No exact inert codec metadata exists for the requested message. */
    extern const ErrorCodeDescriptor MessageCodecUnknown;
    /** @brief The payload schema identity or version is incompatible with codec metadata. */
    extern const ErrorCodeDescriptor MessageSchemaIncompatible;
    /** @brief A required envelope extension is not understood by the local registry. */
    extern const ErrorCodeDescriptor MessageEnvelopeUnknownRequiredField;
    /** @brief A 32-bit message sequence or acknowledgement counter cannot advance without wrapping. */
    extern const ErrorCodeDescriptor MessageCounterExhausted;
    /** @brief Caller-owned cancellation state rejected message codec admission. */
    extern const ErrorCodeDescriptor MessageEnvelopeCancelled;
    /** @brief Caller-owned timeout state rejected message codec admission. */
    extern const ErrorCodeDescriptor MessageEnvelopeTimedOut;
    /** @brief Caller-owned shutdown state rejected message codec admission. */
    extern const ErrorCodeDescriptor MessageEnvelopeShuttingDown;
    /** @brief A terminal failure record is malformed or uses an incompatible layer/kind/context combination. */
    extern const ErrorCodeDescriptor TerminalRecordInvalid;
    /** @brief A second terminal completion attempted to replace the immutable first result. */
    extern const ErrorCodeDescriptor TerminalAlreadyResolved;
    /** @brief A terminal completion belongs to a retired or foreign connection generation. */
    extern const ErrorCodeDescriptor TerminalGenerationStale;
    /** @brief Private backend name resolution failed before transport connection. */
    extern const ErrorCodeDescriptor NameResolutionFailed;
    /** @brief Immutable local session policy rejected admission. */
    extern const ErrorCodeDescriptor SessionPolicyRejected;
    /** @brief The remote peer explicitly and safely rejected the session. */
    extern const ErrorCodeDescriptor SessionRemoteRejected;
    /** @brief The owning caller cancelled session admission. */
    extern const ErrorCodeDescriptor SessionCancelled;
    /** @brief The bounded session admission deadline expired. */
    extern const ErrorCodeDescriptor SessionTimedOut;
    /** @brief Session work was terminated by owner-controlled shutdown. */
    extern const ErrorCodeDescriptor SessionShuttingDown;
    /** @brief Gameplay dispatch was rejected before invoking game code. */
    extern const ErrorCodeDescriptor GameplayDispatchRejected;
    /** @brief A non-recoverable network invariant failed. */
    extern const ErrorCodeDescriptor FatalFailure;
}  // namespace Horo::Network::NetworkErrors
