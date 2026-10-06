#pragma once

/** @file NavMeshAssetLoading.h
 * @brief AssetPipeline-to-Navigation host adapter for canonical cooked NavMesh residency. */

#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetDependency.h"
#include "Horo/Assets/AssetPayloadCache.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Assets/NavMeshAssetType.h"
#include "Horo/Navigation/NavigationTileArtifact.h"
#include "Horo/Navigation/NavigationTileDescriptor.h"

namespace Horo::Navigation {
    /** @brief Immutable complete surface/profile tile closure, including encoded empty tiles. */
    struct LoadedNavMeshPartition final {
        SurfaceId surface;
        NavigationAgentProfileId profile;
        NavigationCookedTileDescriptor descriptor;
        std::vector<std::shared_ptr<const NavigationCookedTile>> tiles;
    };

    /** @brief Verified release expectation copied from immutable promoted content and its authenticated manifest.
     * @details This is host evidence, never reconstructed from live geometry or a provider revision. */
    struct NavMeshAssetContentExpectation final {
        Assets::AssetId id;
        Sha256Digest cookedContentDigest;
        NavigationTileBakeCompatibility compatibility;
        NavigationProjectProfile projectProfile;
    };

    /** @brief Exact canonical provenance and temporary decoded preparation storage with shared tile byte pins. */
    struct LoadedNavMeshAsset final {
        /** @brief Begin one move-only preparation value with exact canonical provenance.
         * @param assetId Canonical definition identity. @param revision Captured registry incarnation.
         * @param content Exact cooked envelope digest. @param source Host source evidence. @param key Host cache-key evidence. */
        LoadedNavMeshAsset(Assets::AssetId assetId, Assets::AssetRegistryRevision revision, const Sha256Digest &content,
                           const Sha256Digest &source, const Sha256Digest &key)
            : id(assetId), registryRevision(revision), cookedContentDigest(content), sourceDigest(source), cacheKeyDigest(key) {}

        LoadedNavMeshAsset(const LoadedNavMeshAsset &) = delete;
        LoadedNavMeshAsset &operator=(const LoadedNavMeshAsset &) = delete;
        LoadedNavMeshAsset(LoadedNavMeshAsset &&) noexcept = default;
        LoadedNavMeshAsset &operator=(LoadedNavMeshAsset &&) noexcept = default;
        Assets::AssetId id;
        Assets::AssetRegistryRevision registryRevision;
        Sha256Digest cookedContentDigest;
        Sha256Digest sourceDigest;
        Sha256Digest cacheKeyDigest;
        std::optional<NavigationCookedContentProvenance>
            contentProvenance; /**< Owned decoded evidence; empty only for explicit legacy content. */
        std::vector<LoadedNavMeshPartition> partitions;
        std::vector<Assets::AssetPayloadLease> tileBytes;
    };

    /** @brief Bounds for the definition tile closure in addition to canonical tile decoder limits. */
    struct NavMeshAssetLimits final {
        std::size_t maximumPartitions{64};
        Assets::AssetCookLimits cook;
        std::size_t maximumDecodedBytes{64U * 1024U * 1024U};
    };

    /** @brief Verify canonical record/envelope identity and complete canonical closure before admitting immutable tile allocations.
     * @param metadata Canonical identity/type resolved from the captured registry or prepared Scene dependency.
     * @param registryRevision Registry revision that resolved the record.
     * @param encoded Exact provider bytes containing the AssetCook envelope.
     * @param target Host-selected cook target; mismatches never fall back to another target.
     * @param cache AssetPipeline allocation owner, mutated only on its owner thread.
     * @param limits Qualified envelope/canonical tile closure ceilings.
     * @param expectation Optional exact release evidence; presence requires complete matching HNS2 before cache admission.
     * @return Fully prepared value or original envelope/typed navigation error; no world is published.
     * @note A failed capacity admission may warm the byte cache; it cannot mutate a navigation world. */
    [[nodiscard]] Result<LoadedNavMeshAsset> LoadNavMeshAsset(const Assets::AssetDependency &metadata,
                                                              Assets::AssetRegistryRevision registryRevision,
                                                              std::span<const std::uint8_t> encoded, const AssetCookTargetId &target,
                                                              Assets::AssetPayloadCache &cache, const NavMeshAssetLimits &limits = {},
                                                              const NavMeshAssetContentExpectation *expectation = nullptr);

    /** @brief Resolve via the canonical provider, using a captured registry snapshot and cooperative cancellation.
     * @param registry Captured canonical asset metadata.
     * @param provider Editor/filesystem or packaged/archive provider selected by the host.
     * @param id Exact definition AssetId.
     * @param target Expected cook target.
     * @param cache Owner-thread immutable allocation cache.
     * @param cancellation Provider I/O cancellation token.
     * @param limits Qualified decode ceilings.
     * @param expectation Optional exact release evidence, with no legacy fallback when present.
     * @return Prepared value or original provider/typed navigation failure. */
    [[nodiscard]] Result<LoadedNavMeshAsset> LoadNavMeshAsset(const Assets::AssetRegistrySnapshot &registry,
                                                              const Assets::IAssetProvider &provider, Assets::AssetId id,
                                                              const AssetCookTargetId &target, Assets::AssetPayloadCache &cache,
                                                              const CancellationToken &cancellation, const NavMeshAssetLimits &limits = {},
                                                              const NavMeshAssetContentExpectation *expectation = nullptr);
}  // namespace Horo::Navigation
