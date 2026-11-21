#pragma once

/** @file HeadlessPrefabReplica.h @brief Owner-lane consumer for one authenticated client Scene generation. */

#include "HeadlessPrefabIntent.h"

#include <deque>
#include <map>

namespace Horo::Application::Internal {
    class GameplayWorldComposition;

    /** @brief Admits one transaction at a time and retains only exact committed replica handles. */
    class HeadlessPrefabReplica final {
    public:
        explicit HeadlessPrefabReplica(Network::NetworkClientPrefabInbox inbox) noexcept : inbox_(std::move(inbox)) {}

        /** @brief Transfers protected records and advances asynchronous reconstruction at the Scene safe point. */
        [[nodiscard]] Result<void> Advance(GameplayWorldComposition &gameplay, const AssetCookTargetId &target,
                                           const Prefab::PrefabLimitProfile &limits, const Runtime::RuntimeSceneView &scene,
                                           const CancellationToken &cancellation);

        /** @brief Cancels unpublished work before the Scene or Gameplay owner retires. */
        void Cancel() noexcept;

    private:
        struct ActiveOperation final {
            Network::NetworkObjectId occurrence;
            Prefab::PrefabOperation receipt;
            bool creating;
        };

        [[nodiscard]] Result<void> Enqueue(std::vector<Network::NetworkPrefabLifecycleRecord> records);
        [[nodiscard]] Result<void> Observe();
        [[nodiscard]] Result<void> Schedule(GameplayWorldComposition &gameplay, const AssetCookTargetId &target,
                                            const Prefab::PrefabLimitProfile &limits, const Runtime::RuntimeSceneView &scene,
                                            const CancellationToken &cancellation);
        [[nodiscard]] Result<void> Spawn(GameplayWorldComposition &gameplay, const Network::NetworkPrefabLifecycleRecord &record,
                                         Prefab::PrefabSpawnRequest request, const CancellationToken &cancellation);
        [[nodiscard]] Result<void> Despawn(GameplayWorldComposition &gameplay, Network::NetworkObjectId occurrence,
                                           const CancellationToken &cancellation);

        Network::NetworkClientPrefabInbox inbox_;
        std::deque<Network::NetworkPrefabLifecycleRecord> waiting_;
        std::map<Network::NetworkObjectId, Prefab::PrefabInstance> instances_;
        std::optional<ActiveOperation> active_;
        std::uint32_t unresolvedFrames_{};
    };
}  // namespace Horo::Application::Internal
