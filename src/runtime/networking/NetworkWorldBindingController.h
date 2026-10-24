#pragma once
/** @file NetworkWorldBindingController.h @brief Stable canonical host world owner and admitted control handoff. */

#include "Horo/Network/NetworkAdmissionController.h"
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Network/NetworkWorldBinding.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"

namespace Horo::Network {
    /** @brief One owner-thread world/control owner, stable across permitted composition moves.
     * @details A composition owns this shared allocation; transport/handler participants borrow weakly.
     * Shutdown revokes world views and registered callbacks before Scene/native services are destroyed.
     * A single bounded outbox owns each peer's active Control lane sequence. Backpressure retains exact
     * encoded bytes/sequence until the finite deadline, without rolling back a local committed world.
     * Peer retirement revokes this projection only; canonical admission retains sole native Close ownership.
     */
    class NetworkWorldBindingController final : public IInboundMessageHandler {
        struct Impl;

        class ConstructionKey final {
            friend class NetworkWorldBindingController;
            ConstructionKey() = default;

        public:
            ConstructionKey(const ConstructionKey &) = default;
        };

    public:
        /** @brief Validate and retain one canonical world owner without activating native services. */
        [[nodiscard]] static Result<std::shared_ptr<NetworkWorldBindingController>> Create(NetworkModePlan plan,
                                                                                           NetworkWorldBindingSetup setup);
        /** @internal Factory-only construction; only Create owns the nonaggregate key and complete implementation. */
        NetworkWorldBindingController(ConstructionKey, std::unique_ptr<Impl> implementation) noexcept;
        ~NetworkWorldBindingController() override;
        NetworkWorldBindingController(const NetworkWorldBindingController &) = delete;
        NetworkWorldBindingController &operator=(const NetworkWorldBindingController &) = delete;

        /** @brief Stage the selected local server occurrence before any role publication. */
        [[nodiscard]] Result<void> Prepare();
        /** @brief Install the exact weak admitted handler before native listeners; caller detaches before transport destruction. */
        [[nodiscard]] Result<void> Attach(INetworkTransport &transport, InboundMessageDispatcher &dispatcher,
                                          const std::shared_ptr<NetworkWorldBindingController> &owner);
        /** @brief Revoke the exact handler and all pending sends before releasing native borrows. */
        void Detach(const INetworkTransport &transport) noexcept;
        /** @brief Retain the same actual active peer; trusted endpoint direction is supplied only by the admission observer. */
        [[nodiscard]] Result<void> BindPeer(AdmissionEndpoint endpoint, std::shared_ptr<PeerSessionLifecycle> peer,
                                            NetworkOperationGeneration generation, std::uint64_t nowTick);
        /** @brief Revoke only the exact local client world projection; stale generations cannot touch replacements. */
        [[nodiscard]] Result<void> UnbindClient(NetworkOperationGeneration generation);
        /** @brief Unadmitted calls always fail; no payload can establish a world independently. */
        [[nodiscard]] Result<void> Handle(const MessageEnvelope &message) override;
        /** @brief Validate exact authenticated direction/principal/feature/schema before staging public metadata. */
        [[nodiscard]] Result<void> HandleAdmitted(const InboundMessageContext &context, const MessageEnvelope &message) override;
        /** @brief Authorize non-Control dispatch against the same live peer and published local world. */
        [[nodiscard]] Result<void> AdmitWorldDispatch(ConnectionHandle connection, NetworkOperationGeneration generation,
                                                      const PeerSessionLifecycle &peer, std::uint64_t nowTick) const;
        /** @brief Expire/revoke exact peers and pending travel before any gameplay world work. */
        [[nodiscard]] Result<void> Reconcile(std::uint64_t nowTick);
        /** @brief Queue an active host-owned Control payload in the SAME bounded peer/lane sequence owner.
         * @param connection Exact active local transport generation.
         * @param generation Exact canonical peer operation generation.
         * @param message Existing registry identity/payload; sequence must be zero because this owner issues it.
         * @param phase Must be NetworkFlush; payload is synchronously copied into the fixed outbox.
         * @return Typed stale/schema/capacity error or queued status. World-binding IDs are reserved to this controller.
         */
        [[nodiscard]] Result<void> QueueControl(ConnectionHandle connection, NetworkOperationGeneration generation,
                                                const MessageEnvelopePayloadView &message, Runtime::RuntimePhase phase);
        /** @brief Flush bounded control debt only in NetworkFlush, retaining sequence on backpressure. */
        [[nodiscard]] Result<void> Flush(std::uint64_t nowTick);
        /** @brief Prepare host-selected local travel candidates; never read a local Scene/path from a packet. */
        [[nodiscard]] Result<void> PrepareTravel(std::span<const NetworkModeWorld> worlds);
        /** @brief Whether one finite local travel candidate still exists; false after expiry/cancel/teardown. */
        [[nodiscard]] bool TravelPending() const noexcept;
        /** @brief Query pending status or its original canonical retirement reason. */
        [[nodiscard]] Result<void> TravelStatus() const;
        /** @brief Whether every local detached candidate and required commit outbox reservation is ready. */
        [[nodiscard]] bool TravelReady() const noexcept;
        /** @brief Commit only at the actual canonical safe point after all dependent service preparation. */
        [[nodiscard]] Result<void> Commit(Runtime::RuntimePhase phase);
        /** @brief Discard only detached travel candidates, retaining old published worlds. */
        [[nodiscard]] Result<void> CancelTravel();
        /** @brief Query canonical readiness without allocating an expected-unready frame error. */
        [[nodiscard]] bool Ready(NetworkModeWorldKind kind, Runtime::SceneRuntimeId scene) const noexcept;
        /** @brief Acquire a view from the canonical published lifecycle, never from an independent role cache. */
        [[nodiscard]] Result<NetworkModeRoleView> Role(NetworkModeWorldKind kind, Runtime::SceneRuntimeId scene) const;
        /** @brief Acquire actual phase/tick role permission from the same canonical lifecycle. */
        [[nodiscard]] Result<ReplicationWorldReadLease> Acquire(NetworkModeWorldKind kind, const ReplicationWorldWorkRequest &request,
                                                                ReplicationWorldCapability capability) const;
        /** @brief Revoke worlds/handler/outboxes idempotently before dependent owner teardown. */
        void Shutdown() noexcept;

    private:
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Network
