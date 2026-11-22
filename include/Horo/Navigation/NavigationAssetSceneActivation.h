#pragma once

/** @file NavigationAssetSceneActivation.h
 * @brief Host-composed atomic Scene navigation asset loading and query lifetime pins. */

#include "Horo/Navigation/NavMeshAssetLoading.h"
#include "Horo/Navigation/NavigationWorldLifecycle.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <functional>

namespace Horo::Navigation {
    /** @brief Exact Scene-authored partition supplied only during detached provider construction. */
    struct NavigationLoadedSurface final {
        Assets::AssetId definition;
        std::uint64_t surfaceGeneration{};         /**< Exact live Scene surface incarnation, independent of bake revision. */
        const LoadedNavMeshPartition *partition{}; /**< Borrow expires when the injected factory returns. */
    };

    /** @brief One exact partition from a verified, possibly selectively loaded asset. */
    struct NavigationStreamedSurface final {
        const LoadedNavMeshAsset &asset;
        std::size_t partitionIndex{};
        std::uint64_t surfaceGeneration{};
    };

    /** @brief Unique native provider plus its host-qualified allocation reservation. */
    struct NavigationPreparedAssetBackend final {
        std::unique_ptr<INavigationQueryBackend> backend;
        std::size_t reservedProviderBytes{}; /**< Factory must bound all owned provider allocations by this reservation. */
    };

    /** @brief Composition seam; copy borrowed tables, preserve all requested surface/profile semantics or fail.
     * @details The host selects a concrete backend. The factory must reject unsupported neutral features rather
     * than dropping links or areas. It receives the remaining reservation budget and returns a fully initialized
     * provider; no I/O, conversion or allocation is deferred into publication or queries. */
    using NavigationAssetBackendFactory =
        std::function<Result<NavigationPreparedAssetBackend>(const NavigationWorldActivationDescriptor &,
                                                             std::span<const NavigationLoadedSurface>, std::size_t)>;

    /** @brief Canonical asset provenance retained with the prepared provider generation. */
    struct NavigationAssetProvenance final {
        Assets::AssetId id;
        Assets::AssetRegistryRevision registryRevision;
        Sha256Digest cookedContentDigest;
        Sha256Digest sourceDigest;
        Sha256Digest cacheKeyDigest;
    };

    /** @brief Separate immutable tile cache and per-world provider reservation accounting. */
    struct NavigationAssetWorldSnapshot final {
        std::size_t liveWorlds{};            /**< Includes retired query-pinned and detached prepared worlds. */
        std::size_t reservedProviderBytes{}; /**< Never refunded while the backend remains query-pinned. */
        Assets::AssetPayloadCacheSnapshot immutableBytes;
    };

    /** @brief Finite host bounds on live provider records, independently of the AssetPipeline tile budget. */
    struct NavigationAssetSceneLimits final {
        std::size_t maximumLiveWorlds{64};
        std::size_t maximumReservedProviderBytes{256U * 1024U * 1024U};
        NavMeshAssetLimits assets;
    };

    /** @brief Exact host-selected local or packaged Scene scope and optional mandatory project policy. */
    struct NavigationAssetSceneSelection final {
        NavigationBakeScopeId scope;
        std::optional<NavigationProjectProfile> requiredProfile; /**< Requires a real world and exact cooked project policy. */
        bool streamCoverage{}; /**< Start with a verified catalog and grant coverage only through streamed cell commits. */
    };

    namespace Detail {
        struct NavigationAssetSceneState;
    }

    /**
     * @brief Aggregate Scene activation participant that prepares canonical assets and a detached navigation world.
     * @details Owner-thread Prepare/Acquire/Snapshot. Cache and factory dependencies outlive this participant and
     * returned candidates. All fallible work precedes aggregate Scene publication. Failed validation/load/provider
     * construction preserves the prior world. Publication swaps an already finalized lifecycle; old candidate
     * Shutdown revokes only its own world. Existing NavigationWorldReadLease pins its backend and immutable tile
     * allocations across cache eviction, replacement and shutdown; revoked work must obey lease cancellation.
     * An absent explicit host factory reports CapabilityUnavailable; factory failures preserve their original typed error.
     */
    class NavigationAssetSceneActivationParticipant final : public Runtime::SceneActivationParticipant {
    public:
        NavigationAssetSceneActivationParticipant(const NavigationAssetSceneActivationParticipant &) = delete;
        NavigationAssetSceneActivationParticipant &operator=(const NavigationAssetSceneActivationParticipant &) = delete;
        /** @brief Bind canonical byte owner and explicit host provider composition.
         * @param cache Owner-thread cache; must outlive the participant and candidate preparation.
         * @param target Exact runtime cook target.
         * @param factory Fully initialized provider constructor, without backend discovery.
         * @param limits Finite positive live-world/provider and asset bounds.
         * @param content Optional owned release expectations. Presence requires an exact entry for each Scene NavMesh asset;
         * empty or missing entries never permit legacy fallback. The host obtains these from verified package evidence.
         * @param selection Exact selected scope and project-policy requirement. Default retains definition-local compatibility. */
        NavigationAssetSceneActivationParticipant(Assets::AssetPayloadCache &cache, AssetCookTargetId target,
                                                  NavigationAssetBackendFactory factory, const NavigationAssetSceneLimits &limits = {},
                                                  std::optional<std::vector<NavMeshAssetContentExpectation>> content = std::nullopt,
                                                  NavigationAssetSceneSelection selection = {});
        /** @brief Revoke active work without waiting; worker leases keep storage safe. */
        ~NavigationAssetSceneActivationParticipant() override;
        /** @copydoc Runtime::SceneActivationParticipant::Prepare */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                                         const Runtime::RuntimeSceneView &scene) override;
        /** @brief Acquire current generation through the existing runtime world lease.
         * @return Current provider pin or NoNavigationData. */
        [[nodiscard]] Result<NavigationWorldReadLease> Acquire() const;
        /** @brief Pins the active lifecycle for structural registration on the same host owner thread.
         * @return Exact published lifecycle or null. The Scene participant still owns replacement and shutdown. */
        [[nodiscard]] std::shared_ptr<NavigationWorldLifecycle> ActiveWorld() const noexcept;
        /** @brief Builds and stages the complete desired resident set for the active world.
         * @param expected Exact live predecessor before cell work started.
         * @param next New unique topology generation.
         * @param surfaces Verified authored asset selections, borrowed only during preparation.
         * @return Success or a typed failure. The active world is preserved until its owner commits the candidate.
         * @details The backend retains selected tile bytes and its reservation through worker retirement. An empty
         * desired set publishes NoNavigationData under a new topology generation of the same world.
         */
        [[nodiscard]] Result<void> StageStreamedTopology(const NavigationWorldActivationDescriptor &expected, NavigationGeneration next,
                                                         std::span<const NavigationStreamedSurface> surfaces);
        /** @brief Prepares topology for publication through a Scene structural transaction.
         * @param expected Exact active predecessor. @param next New unique topology generation.
         * @param surfaces Borrowed verified desired resident set.
         * @return Owned structural candidate or original error. Its destruction discards any unpublished stage.
         */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> PrepareStreamedPublication(
            const NavigationWorldActivationDescriptor &expected, NavigationGeneration next,
            std::span<const NavigationStreamedSurface> surfaces);
        /** @brief Read exact asset provenance for the current provider generation.
         * @return Owner-thread borrow valid until publication or shutdown; no inactive fallback is returned. */
        [[nodiscard]] std::span<const NavigationAssetProvenance> ActiveAssetProvenance() const noexcept;
        /** @brief Inspect retained storage reservations and canonical immutable byte accounting.
         * @return Physical lifetime accounting, including revoked query-pinned providers. */
        [[nodiscard]] NavigationAssetWorldSnapshot Snapshot() const noexcept;
        /** @brief Permanently close preparation and revoke active provider work without waiting for leases. */
        void Shutdown() const noexcept;

    private:
        std::shared_ptr<Detail::NavigationAssetSceneState> state_;
    };
}  // namespace Horo::Navigation
