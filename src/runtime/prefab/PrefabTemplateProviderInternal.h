#pragma once

#include "Horo/Prefab/PrefabTemplateProvider.h"

#include <optional>
#include <thread>

namespace Horo::Prefab::Detail {
    /** @brief Unforgeable provider incarnation shared only by its prepared allocations and requests. */
    struct PrefabProviderIdentity final {};

    /** @brief Complete immutable runtime artifact closure; never contains scene borrows or provider callbacks. */
    struct PrefabTemplateAllocation final {
        CookedPrefab value;
        Assets::AssetRegistrySnapshot registry;
        Assets::AssetPayloadLease root;
        std::vector<PrefabTemplateDependencyLease> dependencies;
        AssetCookTargetId target;
        std::shared_ptr<const PrefabProviderIdentity> identity;
    };

    /** @brief Owner-mutated preparation state; CancellationSource alone permits cross-thread mutation. */
    struct PrefabTemplateRequest final {
        PrefabTemplateRequest(PrefabTemplateLoadRequest input, Assets::AssetRegistrySnapshot snapshot, Runtime::SceneRuntimeId sceneId,
                              const CancellationToken &parent, std::shared_ptr<const PrefabProviderIdentity> provider)
            : root(std::move(input)), registry(std::move(snapshot)), scene(sceneId), cancellation(parent), identity(std::move(provider)) {}

        PrefabTemplateLoadRequest root;
        Assets::AssetRegistrySnapshot registry;
        Runtime::SceneRuntimeId scene;
        CancellationSource cancellation;
        std::shared_ptr<const PrefabProviderIdentity> identity;
        PrefabTemplateLoadState status{PrefabTemplateLoadState::Loading};
        std::optional<Error> error;
        std::optional<Assets::AssetLoadHandle> pending;
        std::optional<CookedPrefab> decoded;
        Assets::AssetPayloadLease rootLease;
        std::vector<PrefabTemplateDependencyLease> dependencies;
        std::shared_ptr<const PrefabTemplateAllocation> result;
        bool consumed{};
    };

    /** @brief Weak entries retain capacity identity even when all resident cache pins have been evicted. */
    struct PrefabTemplateCacheEntry final {
        std::weak_ptr<const PrefabTemplateAllocation> allocation;
        std::shared_ptr<const PrefabTemplateAllocation> resident;
    };

    /** @brief One owner-lane bounded provider; shared AssetLoadService owns all worker execution. */
    struct PrefabTemplateProviderState final {
        PrefabTemplateProviderState(Assets::AssetRegistry &assetRegistry, Assets::AssetLoadService &assetLoads,
                                    Runtime::RuntimeSceneService &sceneService, PrefabLimitProfile capturedProfile,
                                    const PrefabTemplateProviderLimits &capacities);
        [[nodiscard]] Result<void> CheckOwner() const;
        [[nodiscard]] Result<void> CheckAdmission(const Assets::AssetRegistrySnapshot &snapshot, Runtime::SceneRuntimeId scene) const;
        /** @brief Completes cancellation only when the shared-loader worker has released its slot. */
        bool CancelRequest(PrefabTemplateRequest &request);
        [[nodiscard]] Result<void> Pump();
        /** @brief Advances one admitted request and accounts for newly scheduled workers. */
        void Progress(PrefabTemplateRequest &request, std::size_t &inFlight);
        [[nodiscard]] Result<void> Consume(PrefabTemplateRequest &request);
        [[nodiscard]] Result<void> Schedule(PrefabTemplateRequest &request);
        [[nodiscard]] Result<void> Publish(PrefabTemplateRequest &request);
        void ReleasePins(PrefabTemplateRequest &request) noexcept;
        void EvictAllocation(const PrefabTemplateAllocation &allocation) noexcept;
        void DropResidency() noexcept;

        Assets::AssetRegistry &registry;
        Assets::AssetLoadService &loads;
        Runtime::RuntimeSceneService &scenes;
        PrefabLimitProfile profile;
        PrefabTemplateProviderLimits limits;
        const std::thread::id owner{std::this_thread::get_id()};
        std::shared_ptr<const PrefabProviderIdentity> identity{std::make_shared<const PrefabProviderIdentity>()};
        std::unique_ptr<Assets::AssetPayloadCache> payloads;
        std::vector<std::shared_ptr<PrefabTemplateRequest>> requests;
        std::vector<PrefabTemplateCacheEntry> cache;
        Runtime::SceneRuntimeId activeScene;
        Assets::AssetRegistryRevision activeRevision;
        CancellationSource retirement;
        bool closed{};
    };
}  // namespace Horo::Prefab::Detail
