#pragma once

/** @file NetworkModeWorldState.h @brief Stable NetworkRuntime-owned canonical mode-world publication state. */

#include "Horo/Network/InboundMessageDispatcher.h"
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Network/NetworkWorldBinding.h"
#include "NetworkWorldOccurrenceIssuer.h"

namespace Horo::Network::Detail {
    /** @brief One owner-thread identity/publication authority; all external references point to this stable allocation.
     * The composition transfers the owner on move. Shutdown revokes records before any Scene facade is
     * destroyed; borrowed transport/handler owners must detach before this allocation is released.
     */
    class NetworkModeWorldState final {
    public:
        NetworkModeWorldState(NetworkModePlan plan, NetworkWorldBindingPolicy policy, std::uint64_t schemaFingerprint);
        ~NetworkModeWorldState() noexcept;
        NetworkModeWorldState(const NetworkModeWorldState &) = delete;
        NetworkModeWorldState &operator=(const NetworkModeWorldState &) = delete;

        /** @brief Stage the selected server occurrence without publishing a role. */
        [[nodiscard]] Result<void> Prepare();
        /** @brief Prepare only host-selected local travel Scenes; old publication remains unchanged. */
        [[nodiscard]] Result<void> PrepareTravel(std::span<const NetworkModeWorld> worlds, std::uint64_t nowTick,
                                                 std::uint64_t maximumPendingTicks);

        [[nodiscard]] bool TravelPending() const noexcept {
            return !stopped_ && travelPending_;
        }

        /** @brief Whether the complete detached local aggregate may publish at the next safe point. */
        [[nodiscard]] bool TravelReady() const noexcept;
        /** @brief Borrow the server candidate identity without publishing a role or object grant. */
        [[nodiscard]] const NetworkWorldBindingRecord *PreparedServerBinding() const noexcept;
        /** @brief Cancel one pending aggregate, preserving all published worlds. */
        void CancelTravel(const ErrorCodeDescriptor *reason = nullptr) noexcept;
        /** @brief Retain the first canonical reason a pending candidate retired. */
        [[nodiscard]] Result<void> TravelStatus() const;
        /** @brief Retain the actual outbound canonical lifecycle; no world is granted here. */
        [[nodiscard]] Result<void> BindClient(std::shared_ptr<PeerSessionLifecycle> peer, ConnectionHandle connection,
                                              NetworkOperationGeneration generation, std::uint64_t nowTick);
        /** @brief Revoke the exact local client projection without changing the canonical peer's terminal reason. */
        [[nodiscard]] Result<void> UnbindClient(NetworkOperationGeneration generation);
        /** @brief Authorize received binding using the current exact peer and host-selected server capability. */
        [[nodiscard]] Result<void> Accept(const NetworkWorldBindingRecord &record, const InboundMessageContext &context);
        /** @brief Commit detached initial candidates only at the real canonical safe point. */
        [[nodiscard]] Result<void> Commit(Runtime::RuntimePhase phase);
        /** @brief Revoke a terminal client before world dispatch, retaining the server independently. */
        void Reconcile(std::uint64_t nowTick) noexcept;
        /** @brief Query canonical readiness without allocating an expected-unready error in frame dispatch. */
        [[nodiscard]] bool Ready(NetworkModeWorldKind kind, Runtime::SceneRuntimeId scene) const noexcept;
        /** @brief Query the host-selected published world for one connection direction. */
        [[nodiscard]] bool ReadyForPeer(bool inboundServer) const noexcept;
        /** @brief Project role and identities solely from a canonical published occurrence. */
        [[nodiscard]] Result<NetworkModeRoleView> Role(NetworkModeWorldKind kind, Runtime::SceneRuntimeId scene) const;
        /** @brief Acquire actual phase/tick permission from the same published lifecycle. */
        [[nodiscard]] Result<ReplicationWorldReadLease> Acquire(NetworkModeWorldKind kind, const ReplicationWorldWorkRequest &request,
                                                                ReplicationWorldCapability capability) const;
        /** @brief Borrow the committed server binding record; absent before actual publication. */
        [[nodiscard]] const NetworkWorldBindingRecord *ServerBinding() const noexcept;
        /** @brief Revoke every published and staged occurrence once before dependent teardown. */
        void Shutdown() noexcept;

    private:
        struct World final {
            World(ReplicationWorldLifecycle lifecycle, NetworkWorldBindingRecord binding)
                : lifecycle(std::move(lifecycle)), binding(std::move(binding)) {}

            ReplicationWorldLifecycle lifecycle;
            NetworkWorldBindingRecord binding;
        };

        [[nodiscard]] Result<void> CheckOwner() const;
        [[nodiscard]] std::size_t Index(NetworkModeWorldKind kind) const noexcept;
        [[nodiscard]] Result<void> AuthorizeClient(const InboundMessageContext &context) const;
        /** @brief Validate the matching prepared travel occurrence without publishing client authority. */
        [[nodiscard]] Result<void> AcceptTravel(const NetworkWorldBindingRecord &record, const InboundMessageContext &context,
                                                std::size_t index);
        /** @brief Stage only the host-selected local Scene, checking a listen server's exact occurrence first. */
        [[nodiscard]] Result<void> StageClient(const NetworkWorldBindingRecord &record, std::size_t index, std::uint64_t nowTick);
        [[nodiscard]] Result<std::unique_ptr<World>> Stage(Runtime::SceneRuntimeId scene, ReplicationExecutionRole role,
                                                           const NetworkWorldBindingRecord &record) const;

        NetworkModePlan plan_;
        const NetworkWorldBindingPolicy policy_;
        const std::uint64_t schemaFingerprint_;
        NetworkWorldOccurrenceIssuer issuer_;
        std::array<std::unique_ptr<World>, 2> current_;
        std::array<std::unique_ptr<World>, 2> candidate_;
        std::array<NetworkModeWorld, 2> travelWorlds_{};
        std::shared_ptr<PeerSessionLifecycle> client_;
        ConnectionHandle connection_;
        NetworkOperationGeneration peerGeneration_;
        std::uint64_t nowTick_{};
        const std::thread::id owner_{std::this_thread::get_id()};
        std::uint64_t travelDeadline_{};
        const ErrorCodeDescriptor *travelFailure_{};
        bool travelPending_{};
        bool remoteCommit_{};
        bool everPublished_{};
        bool stopped_{};
    };
}  // namespace Horo::Network::Detail
