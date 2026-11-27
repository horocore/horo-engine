#pragma once

/** @file NetworkModeWorldState.h @brief Stable NetworkRuntime-owned canonical mode-world publication state. */

#include "Horo/Network/InboundMessageDispatcher.h"
#include "Horo/Network/NetworkAuthoredScenePage.h"
#include "Horo/Network/NetworkInitialStateStream.h"
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Network/NetworkPrefabLifecycle.h"
#include "Horo/Network/NetworkWorldBinding.h"
#include "NetworkWorldOccurrenceIssuer.h"

namespace Horo::Network::Detail {
    /** @brief Borrowed authority group inventory, valid until mapping refresh or world replacement. */
    struct AuthorityPrefabView final {
        std::uint64_t revision;
        std::span<const NetworkPrefabLifecycleRecord> records;
    };

    /** @brief Mapping pin and read-only capture authority for one synchronous host call. */
    struct AuthorityInitialView final {
        ReplicationWorldReadLease mapping;
        const ReplicationWorldLifecycle &lifecycle;
    };

    /** @brief One owner-thread identity/publication authority; all external references point to this stable allocation.
     * The composition transfers the owner on move. Shutdown revokes records before any Scene facade is
     * destroyed; borrowed transport/handler owners must detach before this allocation is released.
     */
    class NetworkModeWorldState final {
    public:
        NetworkModeWorldState(NetworkModePlan plan, NetworkWorldBindingPolicy policy, std::uint64_t schemaFingerprint,
                              std::uint64_t maximumPendingTicks);
        ~NetworkModeWorldState() noexcept;
        NetworkModeWorldState(const NetworkModeWorldState &) = delete;
        NetworkModeWorldState &operator=(const NetworkModeWorldState &) = delete;

        /** @brief Stage the selected server occurrence without publishing a role. */
        [[nodiscard]] Result<void> Prepare(const NetworkAuthoredSceneProjection &authored, NetworkAuthoredSceneProjection client);
        /** @brief Prepare only host-selected local travel Scenes; old publication remains unchanged. */
        [[nodiscard]] Result<void> PrepareTravel(std::span<const NetworkModeWorld> worlds, std::uint64_t nowTick,
                                                 std::uint64_t maximumPendingTicks, const NetworkAuthoredSceneProjection &authored,
                                                 NetworkAuthoredSceneProjection client);

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
        /** @brief Admit protected Prefab data into the currently published client's bounded inbox. */
        [[nodiscard]] Result<void> AcceptPrefab(std::span<const std::byte> payload, const InboundMessageContext &context);
        /** @brief Stages authored rows from an authenticated candidate only. */
        [[nodiscard]] Result<void> AcceptAuthored(std::span<const std::byte> payload, const InboundMessageContext &context);
        /** @brief Borrows the immutable inventory for the authority's exact current or provisional world. */
        [[nodiscard]] const ReplicationAuthoredSceneInventory *AuthorityAuthored(NetworkSessionGeneration session) const noexcept;
        /** @brief Transfer staged work only to the current client Scene at its structural safe point. */
        [[nodiscard]] Result<std::vector<NetworkPrefabLifecycleRecord>> TakePrefab(Runtime::SceneRuntimeId scene,
                                                                                   Runtime::RuntimePhase phase);
        /** @brief Prepares the actual client group only while its authenticated world is published. */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> PrepareClientPrefab(
            Runtime::SceneRuntimeId scene, const NetworkPrefabLifecycleRecord &spawn, std::span<const Runtime::EntityRef> members,
            bool creating, const CancellationToken &cancellation) const;
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
        /** @brief Prepares a structural mapping delta only for the currently published authority Scene. */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> PrepareAuthorityObjects(
            Runtime::SceneRuntimeId scene, std::span<const ReplicationDynamicSceneObject> added,
            std::span<const Runtime::EntityRef> retired, const CancellationToken &cancellation) const;
        /** @brief Borrow the committed server binding record; absent before actual publication. */
        [[nodiscard]] const NetworkWorldBindingRecord *ServerBinding() const noexcept;
        /** @brief Checks the authority and matches an inbound scope to the current or staged client Scene. */
        [[nodiscard]] Result<Runtime::SceneRuntimeId> AuthorizeInitial(NetworkSessionGeneration session,
                                                                       ReplicationAuthorityEpoch authority,
                                                                       const InboundMessageContext &context) const;
        /** @brief Borrows the exact published receiving lifecycle after structural receipt is drained. */
        [[nodiscard]] ReplicationWorldLifecycle *InitialReceiver(NetworkSessionGeneration session, std::size_t minimumObjects) noexcept;
        /** @brief Installs the validated owner facts after the same initial field transaction succeeds. */
        [[nodiscard]] bool PublishInitialOwnership(NetworkSessionGeneration session, std::vector<ReplicationInitialOwnership> ownership,
                                                   NetworkInitialStateTarget updates = {}) noexcept;
        /** @brief Borrows the incremental receiver only in the exact published client world. */
        [[nodiscard]] const NetworkInitialStateTarget *ClientStateUpdates(NetworkSessionGeneration session) const noexcept;
        /** @brief Looks up a published initial grant only while its exact Scene, session and object remain live. */
        [[nodiscard]] Result<ReplicationInitialOwnership> InitialOwnership(Runtime::SceneRuntimeId scene, NetworkSessionGeneration session,
                                                                           NetworkObjectId object) const;
        /** @brief Selects the exact published client binding for traffic admission. */
        [[nodiscard]] const NetworkWorldBindingRecord *ClientBinding() const noexcept;
        /** @brief Acquires a pinned authority mapping at NetworkFlush for the selected published world. */
        [[nodiscard]] Result<AuthorityInitialView> AcquireInitialAuthority(std::uint64_t tick) const;
        /** @brief Refreshes and borrows the authority group inventory at NetworkFlush. */
        [[nodiscard]] Result<AuthorityPrefabView> CaptureAuthorityPrefabs();
        /** @brief Reverse index lookup for a currently published authority. */
        [[nodiscard]] Result<NetworkObjectId> FindAuthorityObject(Runtime::EntityRef entity) const;
        /** @brief Exact client mapping lookup for a currently published session. */
        [[nodiscard]] Result<Runtime::EntityRef> ResolveClientObject(Runtime::SceneRuntimeId scene, NetworkSessionGeneration session,
                                                                     NetworkObjectId object) const;
        /** @brief Revoke every published and staged occurrence once before dependent teardown. */
        void Shutdown() noexcept;

    private:
        struct PendingPrefab final {
            NetworkPrefabLifecycleRecord record;
            CancellationToken cancellation;
        };

        struct World final {
            World(ReplicationWorldLifecycle lifecycle, NetworkWorldBindingRecord binding)
                : lifecycle(std::move(lifecycle)), binding(std::move(binding)) {}

            void BeginShutdown() noexcept {
                if (stateUpdates.receiver)
                    static_cast<void>(stateUpdates.receiver->Shutdown());
                stateUpdates = {};
                lifecycle.BeginShutdown();
            }

            ReplicationWorldLifecycle lifecycle;
            NetworkWorldBindingRecord binding;
            std::vector<PendingPrefab> prefabInbox;
            std::vector<NetworkPrefabLifecycleRecord> authorityPrefabs;
            std::uint64_t prefabRevision{};
            std::optional<ReplicationAuthoredSceneInventory> authored;
            std::optional<NetworkAuthoredSceneAssembly> authoredAssembly;
            CancellationToken authoredDelivery;
            std::uint64_t authoredDeadline{};
            std::optional<std::vector<ReplicationInitialOwnership>> initialOwnership;
            NetworkInitialStateTarget stateUpdates;
        };

        [[nodiscard]] Result<void> CheckOwner() const;
        [[nodiscard]] std::size_t Index(NetworkModeWorldKind kind) const noexcept;
        [[nodiscard]] Result<void> AuthorizeClient(const InboundMessageContext &context) const;
        /** @brief Locates the published client or exact authenticated candidate awaiting its safe point. */
        [[nodiscard]] World *PrefabReceiver(const NetworkPrefabLifecycleRecord &record);
        /** @brief Validate the matching prepared travel occurrence without publishing client authority. */
        [[nodiscard]] Result<void> AcceptTravel(const NetworkWorldBindingRecord &record, const InboundMessageContext &context,
                                                std::size_t index);
        /** @brief Stage only the host-selected local Scene, checking a listen server's exact occurrence first. */
        [[nodiscard]] Result<void> StageClient(const NetworkWorldBindingRecord &record, std::size_t index, std::uint64_t nowTick);
        [[nodiscard]] Result<std::unique_ptr<World>> PrepareClientWorld(Runtime::SceneRuntimeId scene,
                                                                        const NetworkWorldBindingRecord &record,
                                                                        std::uint64_t nowTick) const;
        /** @brief Transfers a complete assembly into a detached client mapping candidate. */
        [[nodiscard]] Result<void> AppendAuthored(World &world, std::size_t index, const NetworkAuthoredScenePage &page,
                                                  const CancellationToken &cancellation);
        /** @brief Commits a complete eligible candidate without publishing its role. */
        [[nodiscard]] Result<void> CommitCandidate(std::size_t index, Runtime::RuntimePhase phase);
        /** @brief Releases an incomplete initial authored acquisition when its control scope retires. */
        void DiscardExpiredAuthored(std::uint64_t nowTick) noexcept;
        [[nodiscard]] Result<std::unique_ptr<World>> Stage(Runtime::SceneRuntimeId scene, ReplicationExecutionRole role,
                                                           const NetworkWorldBindingRecord &record,
                                                           const NetworkAuthoredSceneProjection &authored = {}) const;

        NetworkModePlan plan_;
        const NetworkWorldBindingPolicy policy_;
        const std::uint64_t schemaFingerprint_;
        const std::uint64_t maximumPendingTicks_;
        NetworkWorldOccurrenceIssuer issuer_;
        std::array<std::unique_ptr<World>, 2> current_;
        std::array<std::unique_ptr<World>, 2> candidate_;
        std::array<NetworkModeWorld, 2> travelWorlds_{};
        NetworkAuthoredSceneProjection clientAuthored_;
        NetworkAuthoredSceneProjection travelClientAuthored_;
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
