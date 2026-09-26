#include "Horo/Network/NetworkErrors.h"

namespace Horo::Network::NetworkErrors {
    namespace {
        const ErrorDomainId NetworkDomain{"horo.network"};

        [[nodiscard]] ErrorCodeDescriptor BudgetError(const char *code, const ErrorSeverity severity, const std::string_view summary,
                                                      const std::string_view remediation, const bool retryable, const bool userActionable) {
            return {NetworkDomain, ErrorCode{code}, severity, summary, remediation, retryable, userActionable};
        }
    }  // namespace

    const ErrorCodeDescriptor
        NetworkAddressInvalid{NetworkDomain,
                              ErrorCode{"network.address.invalid"},
                              ErrorSeverity::Error,
                              "The network endpoint is malformed or ambiguous.",
                              "Use canonical IPv4, bracketed IPv6, or a bounded ASCII DNS hostname with a numeric port.",
                              false,
                              true};
    const ErrorCodeDescriptor TransportNativeUnavailable{NetworkDomain,
                                                         ErrorCode{"network.transport.native_unavailable"},
                                                         ErrorSeverity::Error,
                                                         "The selected native transport is unavailable.",
                                                         "Check the selected backend's host support and endpoint availability.",
                                                         true,
                                                         false};
    const ErrorCodeDescriptor TransportConnectionFailed{NetworkDomain,
                                                        ErrorCode{"network.transport.connection_failed"},
                                                        ErrorSeverity::Error,
                                                        "The native connection failed or closed unexpectedly.",
                                                        "Retry only according to the caller's bounded connection policy.",
                                                        true,
                                                        false};
    const ErrorCodeDescriptor TransportMalformedPacket{NetworkDomain,
                                                       ErrorCode{"network.transport.malformed_packet"},
                                                       ErrorSeverity::Error,
                                                       "The native packet exceeds the negotiated transport bounds.",
                                                       "Close the malformed connection and reject its payload.",
                                                       false,
                                                       false};
    const ErrorCodeDescriptor
        NetworkAddressUnsupported{NetworkDomain,
                                  ErrorCode{"network.address.unsupported"},
                                  ErrorSeverity::Error,
                                  "The network endpoint representation is unsupported.",
                                  "Remove schemes and zones; provide an ASCII DNS name already resolved by host policy.",
                                  false,
                                  true};
    const ErrorCodeDescriptor NetworkAddressCapacityExceeded{NetworkDomain,
                                                             ErrorCode{"network.address.capacity_exceeded"},
                                                             ErrorSeverity::Error,
                                                             "The network endpoint exceeds its finite text capacity.",
                                                             "Use a DNS name of at most 253 bytes and a canonical numeric port.",
                                                             false,
                                                             true};
    const ErrorCodeDescriptor TransportHandleInvalid{NetworkDomain,
                                                     ErrorCode{"network.transport.handle_invalid"},
                                                     ErrorSeverity::Error,
                                                     "The transport handle is invalid or stale.",
                                                     "Use the exact current slot generation issued by the owning transport.",
                                                     false,
                                                     false};
    const ErrorCodeDescriptor TransportGenerationExhausted{NetworkDomain,
                                                           ErrorCode{"network.transport.generation_exhausted"},
                                                           ErrorSeverity::Error,
                                                           "The transport handle generation cannot advance without wrapping.",
                                                           "Retire the exhausted slot permanently and allocate a different bounded slot.",
                                                           false,
                                                           false};
    const ErrorCodeDescriptor NetworkObjectIdentityInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.object.identity_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The replicated authority or object identity is invalid.",
        .remediationHint = "Use a non-zero authority epoch and a non-zero owner-issued object slot and generation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor NetworkObjectGenerationExhausted{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.object.generation_exhausted"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The replicated-object generation cannot advance without wrapping.",
        .remediationHint = "Retire the exhausted slot permanently and allocate a different bounded slot.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NetworkObjectMappingInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.object.mapping_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The replicated-object mapping input is malformed or belongs to another scene.",
        .remediationHint = "Use valid schema provenance and a generation-checked entity from the mapping's exact scene.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor NetworkObjectMappingConflict{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.object.mapping_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A replicated-object slot or local entity already has a live mapping.",
        .remediationHint = "Retire the exact prior occurrence before reusing its slot, and map each entity only once.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor NetworkObjectMappingUnknown{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.object.mapping_unknown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The replicated-object identity or local entity is absent, retired, or stale.",
        .remediationHint = "Use the exact active authority epoch, scene, entity generation, and object generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NetworkObjectMappingCapacityExceeded{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.object.mapping_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The session-owned replicated-object mapping exhausted its prepared slot capacity.",
        .remediationHint = "Apply bounded admission policy or create the session with an explicitly larger finite capacity.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NetworkObjectMappingTerminal{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.object.mapping_terminal"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The replicated-object mapping is shutting down or invalidated.",
        .remediationHint = "Reject late work and create a new mapping for the replacement session and scene generation.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor PacketBufferInvalid{NetworkDomain,
                                                  ErrorCode{"network.packet.buffer_invalid"},
                                                  ErrorSeverity::Error,
                                                  "Packet buffer pool bounds are invalid.",
                                                  "Use positive finite slot and byte bounds.",
                                                  false,
                                                  true};
    const ErrorCodeDescriptor PacketBufferCapacityExceeded{NetworkDomain,
                                                           ErrorCode{"network.packet.buffer_capacity_exceeded"},
                                                           ErrorSeverity::Error,
                                                           "Packet buffer capacity was exceeded.",
                                                           "Reduce the payload or prepare a larger bounded pool.",
                                                           false,
                                                           true};
    const ErrorCodeDescriptor PacketBufferPoolExhausted{NetworkDomain,
                                                        ErrorCode{"network.packet.buffer_pool_exhausted"},
                                                        ErrorSeverity::Error,
                                                        "No prepared packet buffer slot is available.",
                                                        "Release a lease or apply explicit queue backpressure.",
                                                        true,
                                                        false};
    const ErrorCodeDescriptor PacketQueueInvalid{NetworkDomain,
                                                 ErrorCode{"network.packet.queue_invalid"},
                                                 ErrorSeverity::Error,
                                                 "Packet queue input or bounds are invalid.",
                                                 "Use valid identities and positive finite bounds.",
                                                 false,
                                                 true};
    const ErrorCodeDescriptor PacketQueueCapacityExceeded{NetworkDomain,
                                                          ErrorCode{"network.packet.queue_capacity_exceeded"},
                                                          ErrorSeverity::Error,
                                                          "A packet can never fit the queue's declared byte bounds.",
                                                          "Reduce the packet or explicitly revise queue bounds.",
                                                          false,
                                                          true};
    const ErrorCodeDescriptor PacketQueueFull{NetworkDomain,
                                              ErrorCode{"network.packet.queue_full"},
                                              ErrorSeverity::Error,
                                              "The bounded packet queue rejected admission.",
                                              "Apply the queue's explicit overload policy or retry after bounded drain.",
                                              true,
                                              false};
    const ErrorCodeDescriptor TransportBudgetInvalid =
        BudgetError("network.transport.budget_invalid", ErrorSeverity::Error, "Transport budget policy or admission input is malformed.",
                    "Use a version-one higher revision, positive finite bounds, a monotonic tick, and valid traffic metadata.", false,
                    true);
    const ErrorCodeDescriptor TransportBudgetCapacityExceeded =
        BudgetError("network.transport.budget_capacity_exceeded", ErrorSeverity::Error, "Transport budget capacity was exceeded.",
                    "Reject admission or atomically install a validated policy within the prepared hard ceilings.", true, false);
    const ErrorCodeDescriptor TransportBudgetPolicyStale =
        BudgetError("network.transport.budget_policy_stale", ErrorSeverity::Warning, "The transport budget policy revision was replaced.",
                    "Rebase the complete candidate on the exact current policy revision before retrying.", true, false);
    const ErrorCodeDescriptor TransportReliableBackpressure =
        BudgetError("network.transport.reliable_backpressure", ErrorSeverity::Warning,
                    "The bounded transport budget rejected required reliable work.",
                    "Retain ownership outside the transport only under a separate bounded retry policy, or close the saturated peer.", true,
                    false);
    const ErrorCodeDescriptor TransportBudgetTicketStale =
        BudgetError("network.transport.budget_ticket_stale", ErrorSeverity::Warning,
                    "The queued transport work ticket is released or stale.",
                    "Discard the late completion and use only the exact ticket retained for currently queued work.", false, false);

    const ErrorCodeDescriptor IdentityInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.identity.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A stable network identity is invalid.",
        .remediationHint =
            "Use a non-zero identity allocated by the declaring owner; never derive identity from a name or storage address.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationDescriptorInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Replication descriptor metadata is malformed.",
        .remediationHint = "Provide known typed policies, finite limits, canonical defaults, and valid stable identities.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationDescriptorConflict{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.descriptor_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Replication descriptor identities conflict.",
        .remediationHint = "Allocate each schema and field identity once and keep removed field identities tombstoned.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationDescriptorIncompatible{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.descriptor_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Replication descriptor evolution is incompatible.",
        .remediationHint = "Preserve existing field semantics, add compatible fields as optional defaults, or declare a new major-version "
                           "translation later.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationSchemaUnknown{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.schema_unknown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested replication schema is absent.",
        .remediationHint = "Use an exact schema identity from the pinned registry snapshot; no default schema is substituted.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ReplicationCapacityExceeded{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Replication descriptor construction exceeded its finite capacity.",
        .remediationHint = "Reduce the schema, field, owner-identity, or canonical-default footprint within the admitted limits.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationRoleContextInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.role_context_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The pinned replication role context is malformed or mixes unrelated identity generations.",
        .remediationHint = "Use one valid session, object, schema version, role, peer, and owner binding from the same immutable snapshot.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationAuthorityDenied{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.authority_denied"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The current world role cannot originate canonical replicated state.",
        .remediationHint =
            "Submit an allowed typed command to the authority server; client ownership never grants canonical write authority.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ReplicationRoleTransitionStale{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.role_transition_stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The replication role transition revision is stale or does not match the pending safe point.",
        .remediationHint = "Rebuild the complete transition from the current immutable role binding.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ReplicationRoleTransitionPending{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.role_transition_pending"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "A replication role transition is already staged.",
        .remediationHint = "Commit or discard the staged transition at the owner safe point before staging another.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ReplicationRoleWrongThread{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.role_wrong_thread"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Replication role state was accessed from a non-owner thread.",
        .remediationHint = "Transfer the complete command to the runtime owner-thread safe point.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ReplicationRoleShuttingDown{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.role_shutting_down"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Replication role state is shutting down.",
        .remediationHint = "Reject late work and bind it only to a new active session/object generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ReplicationWorldInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.world_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The replication world activation or identity tuple is malformed.",
        .remediationHint = "Use one valid Scene, active session, authority epoch, role, and declared runtime phase set.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationWorldStale{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.world_stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Replication work belongs to a replaced Scene or session generation.",
        .remediationHint = "Discard the stale work and reacquire the current world capability at its owner safe point.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ReplicationWorldUnavailable{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.world_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested replication world is not active for this operation.",
        .remediationHint = "Wait for activation or use the exact active Scene/session generation.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ReplicationWorldPhaseInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.world_phase_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Replication work was submitted outside its declared runtime phase.",
        .remediationHint = "Submit the operation only through the phase admitted by the active world contract.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ReplicationWorldCapacityExceeded{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.world_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Replication world retirement or object storage reached its finite bound.",
        .remediationHint = "Drain old world capabilities or increase the explicitly qualified world limit.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationWorldCancelled{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.world_cancelled"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Replication world work was cancelled before publication.",
        .remediationHint = "Discard the candidate and retry only under the current owner generation.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ReplicationWorldShuttingDown{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.world_shutting_down"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Replication world admission is closed for shutdown.",
        .remediationHint = "Stop submitting replication work and wait for the host to compose a new generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor TransportCapabilityDescriptorInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.transport.capability_descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Transport capability evidence or delivery requirements are malformed.",
        .remediationHint = "Use version-one evidence, an exact non-zero revision, known policies, and finite positive limits.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor TransportCapabilityStale{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.transport.capability_stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Transport capability evidence changed during admission.",
        .remediationHint = "Capture the current candidate evidence and repeat complete delivery negotiation.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor TransportDeliveryUnsupported{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.transport.delivery_unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The selected transport candidate cannot provide the exact required delivery semantics.",
        .remediationHint = "Choose an explicit candidate supporting the required policy; do not weaken reliability or ordering.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor TransportCapabilityUnavailable{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.transport.capability_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A required transport capability is not currently available.",
        .remediationHint = "Restore the explicit candidate or retry after it publishes a new capability revision.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor TransportLimitExceeded{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.transport.limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A transport delivery requirement exceeds the candidate's finite limits.",
        .remediationHint = "Reduce the requested channel, message, or deadline bound or choose an explicit capable candidate.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor TransportOperationCancelled{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.transport.operation_cancelled"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "Transport admission was cancelled before queue mutation.",
        .remediationHint = "Submit again only if the owning caller still requires the operation.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor TransportShuttingDown{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.transport.shutting_down"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "Transport admission was rejected because shutdown has begun.",
        .remediationHint = "Do not enqueue new transport work after the owning runtime begins shutdown.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NetworkIoServiceInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.io.service_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The network I/O service or one of its work bounds is invalid.",
        .remediationHint = "Inject one backend and use positive finite queue, poll, and owner-drain limits.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor NetworkIoServiceCapacityExceeded{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.io.service_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The network I/O service could not reserve its declared finite storage.",
        .remediationHint = "Reduce the prepared completion capacity or release host memory before composing the service.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NetworkIoCompletionInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.io.completion_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A network I/O completion violates its typed handoff contract.",
        .remediationHint = "Publish a valid connection generation and the exact payload or terminal evidence required by its kind.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NetworkIoCompletionQueueFull{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.io.completion_queue_full"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The bounded network I/O completion handoff rejected publication.",
        .remediationHint = "Drain at the owner-thread safe point or apply explicit transport overload policy.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NetworkIoPollBusy{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.io.poll_busy"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The network I/O service already has an active backend poll.",
        .remediationHint = "Use exactly one transport-owned polling thread for each host-scoped service.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NetworkIoPollStale{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.io.poll_stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A completion producer outlived the bounded backend poll that issued it.",
        .remediationHint = "Publish normalized completions synchronously during the current Poll call only.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NetworkIoWrongThread{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.io.wrong_thread"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Network I/O completions were drained from the wrong owner thread.",
        .remediationHint = "Drain only during the declared owner-thread network poll phase.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NetworkIoSequenceExhausted{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.io.sequence_exhausted"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The network I/O service sequence cannot advance without wrapping.",
        .remediationHint = "Retire this host-scoped service and compose a fresh generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NetworkLifecycleInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.lifecycle.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The listener or connection lifecycle request is malformed.",
        .remediationHint = "Use valid exact handles, non-zero work generations, deadlines and terminal evidence.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor NetworkLifecycleCapacityExceeded{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.lifecycle.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The prepared network lifecycle registry is full.",
        .remediationHint = "Close and replace an existing generation or increase the bounded setup capacity.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor NetworkLifecycleTransitionInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.lifecycle.transition_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested network lifecycle transition is illegal.",
        .remediationHint = "Advance only through the documented listener or connection state sequence.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NetworkLifecycleOperationStale{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.lifecycle.operation_stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The network completion belongs to a stale handle or operation generation.",
        .remediationHint = "Discard the completion and retain the current owner-published generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ProtocolIdentityDescriptorInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.protocol.identity_descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Protocol identity metadata is malformed.",
        .remediationHint = "Use non-zero fixed-width identities, matching namespaces, and valid bounded versions.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ProtocolIdentityConflict{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.protocol.identity_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Protocol identity registrations conflict.",
        .remediationHint = "Allocate each protocol-scoped wire identity once and preserve it across renames.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ProtocolIdentityUnknown{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.protocol.identity_unknown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested protocol identity is not registered.",
        .remediationHint = "Use an exact identity from the pinned registry; no name or default fallback is applied.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ProtocolVersionIncompatible{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.protocol.version_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Protocol versions have no compatible overlap.",
        .remediationHint = "Advertise an explicit same-major interval permitted by both protocol peers.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ProtocolIdentityCapacityExceeded{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.protocol.identity_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Protocol identity registry construction exceeded its finite capacity.",
        .remediationHint = "Reduce protocol, message, feature, or close-reason contributions within configured limits.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor HandshakeInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.handshake.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The protocol handshake input or local policy is malformed.",
        .remediationHint =
            "Use version-one bounded declarations, valid identities and ranges, canonical feature sets, and a future deadline.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor HandshakeIncompatible{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.handshake.incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The peer and local handshake requirements have no explicit compatible selection.",
        .remediationHint = "Align protocol, schema, mandatory features, compression, and transport requirements before retrying.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor HandshakeStateInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.handshake.state_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The handshake operation is invalid for its current lifecycle state.",
        .remediationHint = "Retain the first terminal result and start a new connection and session generation for another attempt.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor AuthenticationInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.authentication.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Network authentication input is malformed.",
        .remediationHint = "Use the exact bounded challenge contract and generation-bound host authority outputs.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor AuthenticationIncompatible{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.authentication.incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Network authentication conflicts with immutable trust policy.",
        .remediationHint = "Use the exact policy revision, transcript, and exposure-specific protection requirements.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AuthenticationTrustUnavailable{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.authentication.trust_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A required host trust authority is unavailable.",
        .remediationHint = "Restore the configured credential, certificate, peer-verification, and private-key providers.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AuthenticationRejected{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.authentication.rejected"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Host trust policy rejected network authentication.",
        .remediationHint = "Present only this safe failure class; keep provider, account, proof, certificate, and key detail private.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AuthenticationStateInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.authentication.state_invalid"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Network authentication is not legal from its current state.",
        .remediationHint = "Retain the first terminal result and begin a new authentication generation when required.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor MessageEnvelopeInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.message.envelope_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Message envelope framing or codec metadata is malformed.",
        .remediationHint = "Use the canonical fixed header, ordered bounded fields, and exact declared lengths.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor MessageEnvelopeCapacityExceeded{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.message.envelope_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Message envelope exceeds an admitted finite bound.",
        .remediationHint = "Reduce frame, payload, extension count, or extension bytes within the active limits.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor MessageCodecConflict{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.message.codec_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Message codec metadata contains a duplicate stable identity.",
        .remediationHint = "Publish one codec and one extension descriptor for each protocol-scoped identity.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor MessageCodecUnknown{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.message.codec_unknown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "No exact payload codec metadata is registered for the message.",
        .remediationHint = "Use an explicitly registered protocol and message identity; no fallback codec is selected.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor MessageSchemaIncompatible{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.message.schema_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The payload schema identity or version is incompatible.",
        .remediationHint = "Use the codec's exact schema identity and an explicitly supported same-major version.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor MessageEnvelopeUnknownRequiredField{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.message.unknown_required_field"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A required message-envelope extension is unknown.",
        .remediationHint = "Negotiate support before sending required extensions; only optional unknown fields may be skipped.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor MessageCounterExhausted{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.message.counter_exhausted"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A message wire counter cannot advance without wrapping.",
        .remediationHint = "Close or replace the owning protocol/session generation before reusing a 32-bit counter value.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor MessageEnvelopeCancelled{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.message.cancelled"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Message codec admission was cancelled.",
        .remediationHint = "Do not retry unless the caller supplies a new active operation generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor MessageEnvelopeTimedOut{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.message.timed_out"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Message codec admission deadline expired.",
        .remediationHint = "Discard the late frame and continue only under a new caller-owned operation generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor MessageEnvelopeShuttingDown{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.message.shutting_down"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "Message codec admission is closed for shutdown.",
        .remediationHint = "Stop producing new frames and complete bounded owner-controlled teardown.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor TerminalRecordInvalid{NetworkDomain,
                                                    ErrorCode{"network.terminal.invalid"},
                                                    ErrorSeverity::Error,
                                                    "The canonical network terminal record is malformed.",
                                                    "Use a known layer/kind pair and ordered bounded typed context.",
                                                    false,
                                                    true};
    const ErrorCodeDescriptor TerminalAlreadyResolved{NetworkDomain,
                                                      ErrorCode{"network.terminal.already_resolved"},
                                                      ErrorSeverity::Warning,
                                                      "The connection generation already has an immutable terminal result.",
                                                      "Discard duplicate or late completion and retain the first terminal result.",
                                                      false,
                                                      false};
    const ErrorCodeDescriptor TerminalGenerationStale{NetworkDomain,
                                                      ErrorCode{"network.terminal.generation_stale"},
                                                      ErrorSeverity::Warning,
                                                      "The terminal completion belongs to a retired connection generation.",
                                                      "Discard the late completion and use the exact current owner-issued handle.",
                                                      false,
                                                      false};
    const ErrorCodeDescriptor NameResolutionFailed{NetworkDomain,
                                                   ErrorCode{"network.transport.name_resolution_failed"},
                                                   ErrorSeverity::Error,
                                                   "The endpoint name could not be resolved.",
                                                   "Retry under bounded policy or provide a canonical reachable endpoint.",
                                                   true,
                                                   true};
    const ErrorCodeDescriptor SessionPolicyRejected{NetworkDomain,
                                                    ErrorCode{"network.session.local_policy_rejected"},
                                                    ErrorSeverity::Error,
                                                    "Local immutable session policy rejected admission.",
                                                    "Use a permitted exposure, trust, capability, and protocol profile.",
                                                    false,
                                                    true};
    const ErrorCodeDescriptor SessionRemoteRejected{NetworkDomain,
                                                    ErrorCode{"network.session.remote_rejected"},
                                                    ErrorSeverity::Error,
                                                    "The remote peer rejected session admission.",
                                                    "Present the safe canonical reason without exposing remote or credential detail.",
                                                    false,
                                                    true};
    const ErrorCodeDescriptor SessionCancelled{NetworkDomain,
                                               ErrorCode{"network.session.cancelled"},
                                               ErrorSeverity::Warning,
                                               "Session admission was cancelled by its owner.",
                                               "Start a new generation only when the caller still requires a session.",
                                               false,
                                               false};
    const ErrorCodeDescriptor SessionTimedOut{NetworkDomain,
                                              ErrorCode{"network.session.timed_out"},
                                              ErrorSeverity::Warning,
                                              "The bounded session admission deadline expired.",
                                              "Retry only under explicit bounded policy with a new session generation.",
                                              true,
                                              false};
    const ErrorCodeDescriptor SessionShuttingDown{NetworkDomain,
                                                  ErrorCode{"network.session.shutting_down"},
                                                  ErrorSeverity::Info,
                                                  "Session work terminated during owner-controlled shutdown.",
                                                  "Discard late callbacks and complete bounded teardown.",
                                                  false,
                                                  false};
    const ErrorCodeDescriptor GameplayDispatchRejected{NetworkDomain,
                                                       ErrorCode{"network.gameplay.dispatch_rejected"},
                                                       ErrorSeverity::Error,
                                                       "Gameplay dispatch rejected an unadmitted network message.",
                                                       "Activate the exact trusted session and validate message authority before dispatch.",
                                                       false,
                                                       false};
    const ErrorCodeDescriptor FatalFailure{NetworkDomain,
                                           ErrorCode{"network.internal.fatal"},
                                           ErrorSeverity::Critical,
                                           "A non-recoverable network invariant failed.",
                                           "Stop the affected network owner and preserve bounded redacted evidence.",
                                           false,
                                           false};
}  // namespace Horo::Network::NetworkErrors
