#pragma once
#include "Horo/Network/NetworkDebugger.h"

/**
 * @file RpcGameplayDispatch.h
 * @brief Bounded owner-thread RPC receipt and gameplay safe-point execution.
 */

#include "Horo/Network/InboundMessageDispatcher.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"
#include "Horo/Network/RpcDescriptorRegistry.h"

#include <memory>
#include <span>
#include <thread>
#include <variant>
#include <vector>

namespace Horo::Network {
    /** @brief Trusted role assigned by the host after session admission. */
    enum class RpcRemoteRole : std::uint8_t {
        Client,
        Authority
    };

    /** @brief Closed numeric representation; constraints never retain arbitrary strings or byte buffers. */
    using RpcScalar = std::variant<std::int64_t, std::uint64_t, double>;

    /** @brief Inclusive scalar constraint composed by the Gameplay owner for an exact typed parameter. */
    struct RpcParameterConstraint final {
        RpcParameterId parameter;
        RpcScalar minimum; /**< Signed integer, unsigned integer or finite floating value. */
        RpcScalar maximum; /**< Same representation as minimum and the parameter codec. */
    };

    /** @brief Trusted caller evidence; payload metadata never supplies a role or ownership grant. */
    struct RpcCallerContext final {
        NetworkPeerId caller;
        RpcRemoteRole remoteRole{};
        ConnectionHandle connection;
        NetworkOperationGeneration admission;
        ReplicationRoleBinding object;
    };

    /** @brief Host-approved pure, bounded custom caller policy, invoked on the dispatch owner thread. */
    class IRpcCallerPolicy {
    public:
        virtual ~IRpcCallerPolicy() = default;
        /** @brief Evaluates permission without mutating Gameplay or Scene.
         * @param context Borrowed exact admitted caller and current object publication.
         * @return Whether this caller is permitted; errors and exceptions deny permission.
         * @throws All exceptions are contained by dispatch. */
        [[nodiscard]] virtual Result<bool> Authorize(const RpcCallerContext &context) const = 0;
    };

    /** @brief Immutable host-composed enforcement data for one executable binding. */
    struct RpcGameplayPolicy final {
        std::optional<RpcPermissionId> permission;       /**< Exact custom declaration identity, if configured. */
        std::shared_ptr<const IRpcCallerPolicy> caller;  /**< Code pinned by the handler's module lease. */
        std::vector<RpcParameterConstraint> parameters;  /**< At most one constraint per declared parameter. */
        std::optional<ReplicationSchemaId> objectSchema; /**< Exact owning object/component schema when the handler is schema-specific. */
        ReplicationSchemaVersion objectSchemaVersion;    /**< Exact version of objectSchema; zero when absent. */
    };

    /** @brief Explicit finite storage and execution budgets for one active world. */
    struct RpcDispatchLimits final {
        std::size_t maximumPeers{64};
        std::size_t maximumBindings{512};
        std::size_t maximumObjects{4096};
        std::size_t maximumPending{256};
        std::size_t maximumReplayScopes{4096};
        std::size_t maximumPerDrain{64};
        std::size_t maximumInvocationBytes{64 * 1024}; /**< Wire and retained-value byte ceiling, including defaults. */
        std::size_t maximumRateScopes{4096};           /**< Caller/RPC ledgers, retained across binding and peer revocation. */
        std::size_t maximumCallerScopes{4096};         /**< Retained caller admission-work ledgers across reconnects. */
        std::uint64_t ticksPerSecond{1000};            /**< Host monotonic admission-clock frequency, independent of simulation ticks. */
        std::uint32_t maximumAttemptsPerSecond{1024};  /**< Per caller attempts charged before wire parsing. */
        std::uint32_t maximumGlobalAttemptsPerSecond{8192};
        std::uint64_t maximumBytesPerSecond{4 * 1024 * 1024}; /**< Per caller bytes charged before parsing. */
        std::uint64_t maximumGlobalBytesPerSecond{32 * 1024 * 1024};
        std::size_t maximumPendingPerPeer{32}; /**< Per admitted caller queue ceiling, also bounded by maximumPending. */
    };

    /** @brief Trusted, generation-fenced context delivered to the gameplay owner. */
    struct RpcGameplayContext final {
        NetworkPeerId sender;
        NetworkObjectId object;
        Runtime::EntityRef entity;
        Runtime::SceneRuntimeId scene;
        NetworkSessionGeneration session;
        std::uint64_t logicalSequence{};
        std::uint64_t simulationTick{};
    };

    /**
     * @brief Gameplay-owned executable binding for an exact RPC declaration.
     * @details Execute must publish Scene/Gameplay mutation transactionally. A failure cannot be retried by the
     * network owner because the handler may already have committed its transaction. The handler must resolve the
     * entity against its owning Scene before staging mutation and discard its candidate on failure or exception.
     */
    class IRpcGameplayHandler {
    public:
        virtual ~IRpcGameplayHandler() = default;
        /**
         * @brief Executes one fully validated invocation at the Gameplay fixed-step safe point.
         * @param context Trusted sender and exact world/object/entity identities, borrowed for this call.
         * @param parameters Owned typed values borrowed for this call; must not be retained.
         * @return Transaction acceptance or a typed rejection. No partial candidate may escape on failure.
         * @throws Any exception is contained by dispatch and becomes a terminal Gameplay rejection.
         */
        [[nodiscard]] virtual Result<void> Execute(const RpcGameplayContext &context,
                                                   std::span<const ReplicationRuntimeValue> parameters) = 0;
    };

    /** @brief Bounded terminal evidence for one safe-point drain. */
    struct RpcDispatchReport final {
        std::size_t consumed{};
        std::size_t invoked{};
        std::size_t rejected{};
        std::optional<Error> lastError;
    };

    /** @brief Exactly-once bounded terminal accounting, including work discarded by revocation or shutdown. */
    struct RpcTerminalTotals final {
        std::uint64_t accepted{};
        std::uint64_t succeeded{};
        std::uint64_t failed{};
        std::uint64_t cancelled{};
    };

    /**
     * @brief Host-composed RPC route above an admitted inbound message class.
     * @details The host registers this object with InboundMessageDispatcher, then binds admitted peers, live object
     * role states, exact descriptor handlers and canonical serializers. No packet field grants peer identity or
     * authority. Receipt queues owned values; Drain rechecks every mutable grant before invoking Gameplay.
     * All methods are owner-thread only. The borrowed world must outlive this object; the descriptor generation is pinned.
     * Hosts retire object mappings before Scene destruction and close dispatch before module retirement. Attempts and
     * bytes are charged before parsing; caller/RPC rate budgets precede serializers and policy callbacks. A queued
     * command pins the exact role publication and entity occurrence. Absent custom policy fails closed.
     */
    class RpcGameplayDispatch final : public IInboundMessageHandler, public std::enable_shared_from_this<RpcGameplayDispatch> {
    public:
        /**
         * @brief Validates bounds and pins the accepted descriptor generation.
         * @param descriptors Non-null immutable accepted declaration generation.
         * @param world Borrowed owner-thread lifecycle, retained by the host through dispatch destruction.
         * @param limits Finite registry, queue, payload and safe-point work ceilings.
         * @param debugger Optional owner-thread collector; must outlive this dispatch.
         * @return Prepared dispatch or typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<RpcGameplayDispatch>> Create(RpcDescriptorSnapshotPtr descriptors,
                                                                                 ReplicationWorldLifecycle &world,
                                                                                 const RpcDispatchLimits &limits = {},
                                                                                 NetworkDebugger *debugger = nullptr);
        ~RpcGameplayDispatch() override;

        /**
         * @brief Binds one admitted session to host-issued peer identity and remote role.
         * @param session Host-admitted Active session retained until revocation.
         * @param connection Exact session connection.
         * @param generation Exact admitted operation generation.
         * @param peer Host-issued remote identity; packet bytes cannot supply this grant.
         * @param role Trusted remote client or authority role.
         * @param nowTick Current host admission-clock tick.
         * @param localRecipient Exact local peer for authority-to-client routes; absent for client-to-authority.
         * @return Success or typed admission, conflict or capacity failure.
         */
        [[nodiscard]] Result<void> RegisterPeer(const std::shared_ptr<PeerSessionLifecycle> &session, ConnectionHandle connection,
                                                NetworkOperationGeneration generation, NetworkPeerId peer, RpcRemoteRole role,
                                                std::uint64_t nowTick, NetworkPeerId localRecipient = {});
        /** @brief Revokes one peer and discards its pending work before session destruction.
         * @param connection Exact revoked connection. Same-generation replay evidence remains retained. */
        void RevokePeer(ConnectionHandle connection) noexcept;
        /** @brief Binds the live role state for one exact network object occurrence.
         * @param object Exact authority/object generation.
         * @param role Owner-published role state retained through revocation.
         * @return Typed validation, conflict or capacity result. */
        [[nodiscard]] Result<void> RegisterObject(NetworkObjectId object, const std::shared_ptr<ReplicationRoleState> &role);
        /** @brief Revokes one object occurrence and discards its pending work.
         * @param object Exact revoked occurrence; replay evidence remains retained. */
        void RevokeObject(NetworkObjectId object) noexcept;
        /**
         * @brief Installs one executable handler and exact ordered canonical parameter serializers.
         * @param id Accepted declaration identity.
         * @param handler Call-owned pin; the installed binding retains the Gameplay transaction owner weakly.
         * @param serializers Exact canonical adapters in descriptor parameter order.
         * @param moduleLease Host-issued exact-generation code lease, retained until adapters and callbacks retire.
         * @param policy Exact custom permission implementation and finite typed scalar constraints, copied at registration.
         * @details Adapter metadata is copied once under a callback guard. Nested registration is rejected;
         * shutdown or revocation during metadata publication aborts installation. Inputs remain pinned through callbacks.
         * @return Typed validation or capacity result; no partial binding is installed.
         */
        [[nodiscard]] Result<void> RegisterHandler(RpcId id, std::shared_ptr<IRpcGameplayHandler> handler,
                                                   std::span<const std::shared_ptr<const IReplicationFieldSerializer>> serializers,
                                                   std::shared_ptr<const void> moduleLease, const RpcGameplayPolicy &policy = {});
        /** @brief Revokes one executable binding and discards its pending work.
         * @param id Exact revoked declaration; replay evidence remains retained. */
        void RevokeHandler(RpcId id) noexcept;

        /** @brief Rejects a message that lacks registered admission evidence.
         * @param message Unadmitted message.
         * @return Always a typed Gameplay dispatch rejection. */
        [[nodiscard]] Result<void> Handle(const MessageEnvelope &message) override;
        /** @brief Validates and stages one admitted canonical RPC payload.
         * @param context Evidence supplied by the admitted inbound route.
         * @param message Bounded canonical wire invocation borrowed through decoding.
         * @return Queue acceptance or typed denial; failure invokes no Gameplay handler. */
        [[nodiscard]] Result<void> HandleAdmitted(const InboundMessageContext &context, const MessageEnvelope &message) override;
        /** @brief Executes at most maximumPerDrain queued commands at the exact fixed-step Gameplay safe point.
         * @param request Exact active world/session and fixed-tick phase, with cooperative cancellation.
         * @return Bounded consumption and terminal evidence, or typed phase/identity/admission failure. */
        [[nodiscard]] Result<RpcDispatchReport> DrainAtGameplaySafePoint(const ReplicationWorldWorkRequest &request);
        /** @brief Idempotently revokes all peers, handlers and pending commands. */
        void Shutdown() noexcept;
        /** @brief Reads cumulative terminal evidence even after shutdown, without emitting per-packet diagnostics.
         * @return Owned totals or a typed owner-thread failure. */
        [[nodiscard]] Result<RpcTerminalTotals> TerminalTotals() const;

    private:
        class ConstructionKey final {
            friend class RpcGameplayDispatch;
            ConstructionKey() = default;

        public:
            ConstructionKey(const ConstructionKey &) = default;
        };

    public:
        /** @internal Factory-only constructor; the private key cannot be created by consumers. */
        RpcGameplayDispatch(ConstructionKey, RpcDescriptorSnapshotPtr descriptors, ReplicationWorldLifecycle &world,
                            const RpcDispatchLimits &limits, NetworkDebugger *debugger);

    private:
        struct Peer;
        struct Object;
        struct Binding;
        struct Pending;
        struct ReplayScope;
        struct RateScope;
        struct WorkScope;
        struct AdmittedInvocation;

        /** @brief Checks reliable logical replay and ledger capacity without consuming the occurrence. */
        [[nodiscard]] Result<ReplayScope *> CheckReplay(const Peer &peer, NetworkObjectId object, const RpcDescriptor &descriptor,
                                                        std::uint64_t sequence);
        [[nodiscard]] Result<void> ExecutePending(const Pending &command, const ReplicationWorldWorkRequest &request);
        /** @brief Pins an executable binding and revalidates its queued publication around custom policy. */
        [[nodiscard]] Result<void> ExecuteBinding(const Pending &command, const ReplicationWorldWorkRequest &request,
                                                  const Binding &binding, const Peer &peer, const Object &object) const;
        [[nodiscard]] Result<void> StageAdmitted(const InboundMessageContext &context, const MessageEnvelope &message, const Peer &peer,
                                                 std::uint64_t revision);
        /** @brief Validates target/session/envelope/rate evidence and pins it before any external decoder runs. */
        [[nodiscard]] Result<AdmittedInvocation> AdmitTarget(const InboundMessageContext &context, const MessageEnvelope &message,
                                                             const Peer &peer, RpcId id, NetworkObjectId object, std::uint64_t sequence,
                                                             NetworkPeerId recipient);
        /** @brief Rechecks exact admission publication after external callbacks without accepting the command. */
        [[nodiscard]] Result<void> RevalidateAdmission(const Pending &command, const AdmittedInvocation &target, std::uint64_t nowTick,
                                                       std::uint64_t revision) const;
        /** @brief Publishes a completely authorized command and its reliable replay occurrence together. */
        void QueueAccepted(Pending command, RpcDelivery delivery, ReplayScope *scope);
        /** @brief Captures exact ordered serializer metadata while guarding retirement during callbacks. */
        [[nodiscard]] Result<void> CaptureSerializerMetadata(Binding &binding, std::uint64_t revision) const;
        [[nodiscard]] Result<void> CheckOwner() const;
        /** @brief Charges finite caller and global work before parsing; clock regression never refills budgets. */
        [[nodiscard]] Result<void> ChargeWork(const Peer &peer, std::size_t bytes, std::uint64_t nowTick);
        /** @brief Charges one caller/RPC token before any parameter adapter or custom policy executes. */
        [[nodiscard]] Result<void> ChargeRate(const Peer &peer, const RpcDescriptor &descriptor, std::uint64_t nowTick);
        /** @brief Validates immutable host policy against ordered declared codec representations. */
        [[nodiscard]] static Result<void> ValidatePolicy(const Binding &binding);
        /** @brief Applies typed constraints and host caller permission to a pinned binding and live role. */
        [[nodiscard]] Result<void> AuthorizePolicy(const Binding &binding, const Peer &peer, const Object &object,
                                                   std::span<const ReplicationRuntimeValue> values) const;
        [[nodiscard]] Result<void> ValidateTarget(const Peer &peer, const RpcDescriptor &descriptor, const ReplicationRoleBinding &role,
                                                  NetworkPeerId wireRecipient) const;
        [[nodiscard]] Result<ReplicationWorldReadLease> ValidateLive(const Peer &peer, const Object &object,
                                                                     const RpcDescriptor &descriptor, NetworkPeerId wireRecipient,
                                                                     std::uint64_t nowTick, Runtime::RuntimePhase phase,
                                                                     Runtime::EntityRef &entity) const;

        RpcDescriptorSnapshotPtr descriptors_;
        ReplicationWorldLifecycle &world_;
        RpcDispatchLimits limits_;
        std::thread::id owner_;
        std::vector<Peer> peers_;
        std::vector<Object> objects_;
        std::vector<Binding> bindings_;
        std::vector<Pending> pending_;
        std::vector<ReplayScope> replay_;
        std::vector<RateScope> rates_;
        std::vector<WorkScope> work_;
        std::uint64_t lastAdmissionTick_{};
        RpcTerminalTotals terminals_;
        NetworkDebugger *debugger_{};
        NetworkDiagnosticSource diagnosticSource_{};
        std::uint64_t revocationRevision_{};
        bool receiving_{};
        bool draining_{};
        bool stopped_{};
    };
}  // namespace Horo::Network
