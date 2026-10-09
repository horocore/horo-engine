#pragma once

/** @file ReplicationInboundApply.h
 * @brief Authority-admitted bounded inbound state staging and atomic owner-safe-point application.
 */

#include "Horo/Network/InboundMessageDispatcher.h"
#include "Horo/Network/ReplicationStateCodec.h"

namespace Horo::Network {
    /** @brief One complete immutable decoded update and exact local owner identity. */
    struct ReplicationApplyUpdate final {
        NetworkObjectMappingEntry mapping;
        ReplicationRoleBinding role;
        ReplicationDecodedState state;
        CancellationToken cancellation;      /**< Receipt/admission ancestry sampled again by the owner at publication. */
        CancellationToken worldCancellation; /**< Exact world retirement sampled at publication. */
    };

    /** @brief Unpublished owner candidate; destruction discards all staged mutation. */
    class IReplicationApplyCandidate {
    public:
        virtual ~IReplicationApplyCandidate() = default;
        /** @brief Commits the complete batch atomically or leaves every owner value unchanged.
         * @param cancellation Cooperative publication cancellation, checked before mutation.
         * @return Success or typed rejection with no partial visibility.
         * @details Revalidate exact Scene/entity generations immediately before publication. Do not call
         * providers, reenter Network, retain decoded borrows or publish side effects on failure.
         */
        [[nodiscard]] virtual Result<void> Commit(const CancellationToken &cancellation) = 0;
    };

    /** @brief Declaring Scene/Gameplay owner for complete transactional received-state batches. */
    class IReplicationApplyOwner {
    public:
        virtual ~IReplicationApplyOwner() = default;
        /** @brief Validates semantic values and prepares a detached candidate without mutating live storage.
         * @param updates Complete batch of exact identities and immutable network values, borrowed until return.
         * @return Sole candidate or typed rejection preserving all prior local state.
         * @details Copy required values into the candidate; no decoded borrows may survive this call.
         * @throws Exceptions are contained by the inbound owner and reject the entire batch.
         */
        [[nodiscard]] virtual Result<std::unique_ptr<IReplicationApplyCandidate>> Prepare(
            std::span<const ReplicationApplyUpdate> updates) = 0;
    };

    /** @brief Exact host-approved authority channel; packet metadata never creates this grant. */
    struct ReplicationInboundAuthority final {
        std::shared_ptr<PeerSessionLifecycle> session;
        ConnectionHandle connection;
        NetworkOperationGeneration generation;
        Runtime::SceneRuntimeId scene;
        NetworkSessionGeneration networkSession;
        ReplicationAuthorityEpoch epoch;
        NetworkPeerId localRecipient;
        ProtocolId protocol;
        MessageTypeId message;
    };

    /** @brief Finite lifetime registry, pending-record and retained-value ceilings. */
    struct ReplicationInboundLimits final {
        std::size_t maximumObjects{256};
        std::size_t maximumPending{64};
        std::size_t maximumRetainedBytes{4 * 1024 * 1024};
        std::size_t maximumWireBytes{1024 * 1024};
    };

    /** @brief Complete-batch terminal result, distinct from decoding and queue acceptance. */
    struct ReplicationApplyReport final {
        std::size_t applied{};
    };

    /** @brief One trusted authority channel's owner-thread receive and safe-point transaction boundary.
     * Each admitted message contains one complete HRS1 codec record. Receipt only decodes and stages;
     * ApplyAtSafePoint prepares and commits all pending objects as one batch. Newer records replace the
     * same object's pending projection. Applied or pending duplicate revisions are ignored; older records
     * are stale. Failed batches are discarded without advancing the committed delta roots.
     * The host must compose only an authenticated server authority, retain the borrowed lifecycle,
     * revoke mappings before entity retirement, and close this owner before session/module retirement.
     */
    class ReplicationInboundApply final : public IInboundMessageHandler, public std::enable_shared_from_this<ReplicationInboundApply> {
        struct ConstructionKey final {
        private:
            friend class ReplicationInboundApply;
            ConstructionKey() = default;
        };

    public:
        /** @internal Factory-only construction with validated identity and finite storage. */
        ReplicationInboundApply(ConstructionKey, const ReplicationInboundAuthority &authority, ReplicationWorldLifecycle &world,
                                std::weak_ptr<IReplicationApplyOwner> owner, const ReplicationInboundLimits &limits);
        ReplicationInboundApply(const ReplicationInboundApply &) = delete;
        ReplicationInboundApply &operator=(const ReplicationInboundApply &) = delete;
        /** @brief Pins one admitted authority and preallocates bounded registry storage.
         * @param authority Complete host-issued channel, world and recipient evidence.
         * @param world Borrowed receiving lifecycle, retained through this owner's destruction.
         * @param owner Transaction owner retained weakly outside each safe-point call.
         * @param now Current monotonic admission-clock tick.
         * @param limits Explicit bounded storage and work ceilings.
         * @return Prepared route or typed validation/admission/capacity failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<ReplicationInboundApply>> Create(const ReplicationInboundAuthority &authority,
                                                                                     ReplicationWorldLifecycle &world,
                                                                                     const std::shared_ptr<IReplicationApplyOwner> &owner,
                                                                                     std::uint64_t now,
                                                                                     const ReplicationInboundLimits &limits = {});
        /** @brief Binds one host-resolved exact object, live role publication and codec generation.
         * @param mapping Exact Scene/entity and schema occurrence.
         * @param role Owner-published local client role.
         * @param codec Exact negotiated object/recipient projection; code/module registry pins remain owned.
         * @return Success or stale/conflict/capacity failure; retired identities cannot be rebound.
         */
        [[nodiscard]] Result<void> RegisterObject(const NetworkObjectMappingEntry &mapping, std::shared_ptr<ReplicationRoleState> role,
                                                  std::shared_ptr<ReplicationStateCodec> codec);
        /** @brief Releases one retired object's pending values and baseline while keeping its identity tombstone.
         * @param object Exact retiring object occurrence.
         * @return Success or typed owner/reentrancy failure.
         */
        [[nodiscard]] Result<void> RevokeObject(NetworkObjectId object);
        /** @brief Rejects unadmitted receipt. @param message Untrusted message. @return Always failure. */
        [[nodiscard]] Result<void> Handle(const MessageEnvelope &message) override;
        /** @brief Decodes and stages one bounded complete record without invoking the mutation owner.
         * @param context Exact admitted channel/session evidence from InboundMessageDispatcher.
         * @param message One complete canonical codec record.
         * @return Stage acceptance, idempotent duplicate, or typed rejection preserving prior storage.
         */
        [[nodiscard]] Result<void> HandleAdmitted(const InboundMessageContext &context, const MessageEnvelope &message) override;
        /** @brief Atomically applies the entire staged batch at CommitDeferredLifecycleChanges.
         * @param request Exact world/session/phase and local positive simulation tick.
         * @param now Current host admission-clock tick, independent of authoritative record ticks.
         * @return Complete applied count or typed failure with prior local state unchanged.
         */
        [[nodiscard]] Result<ReplicationApplyReport> ApplyAtSafePoint(const ReplicationWorldWorkRequest &request, std::uint64_t now);
        /** @brief Closes admission and releases every staged value, baseline and codec pin.
         * @return Idempotent owner-thread success or typed affinity failure.
         */
        [[nodiscard]] Result<void> Shutdown();

    private:
        struct Object final {
            NetworkObjectMappingEntry mapping;
            std::shared_ptr<ReplicationRoleState> role;
            std::shared_ptr<ReplicationStateCodec> codec;
            std::optional<ReplicationDecodedState> baseline;
            std::optional<ReplicationDecodedState> pending;
            Sha256Digest baselineWire;
            Sha256Digest pendingWire;
            CancellationToken cancellation;
            bool retired{};

            [[nodiscard]] const ReplicationDecodedState *Latest() const noexcept {
                if (pending)
                    return &*pending;
                return baseline ? &*baseline : nullptr;
            }
        };

        [[nodiscard]] Result<void> CheckOwner() const;
        [[nodiscard]] Result<ReplicationWorldReadLease> Admit(Runtime::RuntimePhase phase, std::uint64_t now,
                                                              const CancellationToken &cancellation) const;
        [[nodiscard]] Result<void> ValidateObject(const Object &object, const ReplicationWorldReadLease &world) const;
        [[nodiscard]] Result<void> ValidatePending(const ReplicationWorldWorkRequest &request, std::uint64_t now) const;
        [[nodiscard]] Result<void> AdmitRetainedState(std::size_t index, const ReplicationDecodedState &state) const;
        [[nodiscard]] Result<std::size_t> ResolveRecord(std::span<const std::byte> wire, const ReplicationWorldReadLease &world) const;
        [[nodiscard]] Result<void> DecodeAndStage(std::size_t index, const InboundMessageContext &context, std::span<const std::byte> wire);
        /** @brief Copies a complete call-owned immutable batch before invoking any mutation owner. */
        [[nodiscard]] std::vector<ReplicationApplyUpdate> CollectPending(const CancellationToken &worldCancellation) const;

        /** @brief Allocation-free outcome separating callback containment from typed diagnostic construction. */
        struct ApplyAttempt final {
            enum class Fault {
                None,
                Capacity,
                Callback
            };
            std::optional<Result<ReplicationApplyReport>> result;
            Fault fault{Fault::None};
        };

        /** @brief Contains every foreign owner exception without allocating an error inside the nonthrowing boundary.
         * @param owner Pinned declaring owner for this synchronous transaction.
         * @param world Exact admitted receiving world lease.
         * @param request Owner safe-point evidence and cancellation.
         * @param now Current admission-clock tick.
         * @return Stored result or fixed capacity/callback fault; caller discards pending state before creating errors.
         */
        [[nodiscard]] ApplyAttempt InvokeApplyPending(IReplicationApplyOwner &owner, const ReplicationWorldReadLease &world,
                                                      const ReplicationWorldWorkRequest &request, std::uint64_t now) noexcept;
        /** @brief Runs the guarded owner transaction; committed roots advance only after complete publication. */
        [[nodiscard]] Result<ReplicationApplyReport> ApplyPending(IReplicationApplyOwner &owner, const ReplicationWorldReadLease &world,
                                                                  const ReplicationWorldWorkRequest &request, std::uint64_t now);
        void DiscardPending() noexcept;
        ReplicationInboundAuthority authority_;
        ReplicationWorldLifecycle &world_;
        std::weak_ptr<IReplicationApplyOwner> owner_;
        ReplicationInboundLimits limits_;
        std::thread::id thread_{std::this_thread::get_id()};
        std::vector<Object> objects_;
        std::uint64_t revision_{};
        bool busy_{};
        bool stopped_{};
    };
}  // namespace Horo::Network
