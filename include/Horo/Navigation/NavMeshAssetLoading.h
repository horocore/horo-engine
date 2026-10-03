#pragma once

/** @file NavMeshAssetLoading.h
 * @brief AssetPipeline-to-Navigation host adapter for canonical cooked NavMesh residency. */

#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetDependency.h"
#include "Horo/Assets/AssetPayloadCache.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Assets/NavMeshAssetType.h"
#include "Horo/Navigation/NavMeshCodec.h"

namespace Horo::Navigation {
    /** @brief Exact dependency identity/type and digest of its complete verified cooked envelope. */
    struct NavMeshAssetDependency final {
        Assets::AssetDependency asset;
        Sha256Digest cookedContentDigest;
    };

    /** @brief One generated surface/profile partition inside the definition's single canonical AssetId. */
    struct NavMeshAssetPartitionInput final {
        SurfaceId surface;
        std::uint64_t surfaceGeneration{};
        NavMeshArtifactView artifact;
    };

    /** @brief Immutable decoded preparation input; provider construction must copy every borrowed row it retains. */
    struct LoadedNavMeshPartition final {
        SurfaceId surface;
        std::uint64_t surfaceGeneration{};
        NavMeshData data;
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
        std::vector<NavMeshAssetDependency> dependencies;
        std::vector<LoadedNavMeshPartition> partitions;
        std::vector<Assets::AssetPayloadLease> tileBytes;
    };

    /** @brief Bounds for the definition bundle in addition to existing per-partition NavMeshData limits. */
    struct NavMeshAssetLimits final {
        std::size_t maximumPartitions{64};
        std::size_t maximumDependencies{1024};
        Assets::AssetCookLimits cook;
        NavMeshArtifactLimits mesh;
    };

    /** @brief Encode canonical sorted partitions and exact dependency evidence for the AssetCook payload.
     * @param partitions Surface/profile partitions, with no generated AssetIds.
     * @param dependencies Unique sorted canonical asset requirements with exact cooked content evidence.
     * @param limits Positive qualified limits.
     * @return Bounded producer wire bytes or typed invalid/capacity error.
     * @note The future baker uses this encoder inside the host-owned AssetCook envelope; CacheKeyV1 is unchanged. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodeNavMeshAssetPayload(std::span<const NavMeshAssetPartitionInput> partitions,
                                                                              std::span<const NavMeshAssetDependency> dependencies,
                                                                              const NavMeshAssetLimits &limits = {});

    /** @brief Verify canonical record/envelope identity and all tables before admitting immutable tile allocations.
     * @param metadata Canonical identity/type resolved from the captured registry or prepared Scene dependency.
     * @param registryRevision Registry revision that resolved the record.
     * @param encoded Exact provider bytes containing the AssetCook envelope.
     * @param target Host-selected cook target; mismatches never fall back to another target.
     * @param cache AssetPipeline allocation owner, mutated only on its owner thread.
     * @param limits Qualified envelope/bundle/table ceilings.
     * @return Fully prepared value or original envelope/typed navigation error; no world is published.
     * @note A failed capacity admission may warm the byte cache; it cannot mutate a navigation world. */
    [[nodiscard]] Result<LoadedNavMeshAsset> LoadNavMeshAsset(const Assets::AssetDependency &metadata,
                                                              Assets::AssetRegistryRevision registryRevision,
                                                              std::span<const std::uint8_t> encoded, const AssetCookTargetId &target,
                                                              Assets::AssetPayloadCache &cache, const NavMeshAssetLimits &limits = {});

    /** @brief Resolve via the canonical provider, using a captured registry snapshot and cooperative cancellation.
     * @param registry Captured canonical asset metadata.
     * @param provider Editor/filesystem or packaged/archive provider selected by the host.
     * @param id Exact definition AssetId.
     * @param target Expected cook target.
     * @param cache Owner-thread immutable allocation cache.
     * @param cancellation Provider I/O cancellation token.
     * @param limits Qualified decode ceilings.
     * @return Prepared value or original provider/typed navigation failure. */
    [[nodiscard]] Result<LoadedNavMeshAsset> LoadNavMeshAsset(const Assets::AssetRegistrySnapshot &registry,
                                                              const Assets::IAssetProvider &provider, Assets::AssetId id,
                                                              const AssetCookTargetId &target, Assets::AssetPayloadCache &cache,
                                                              const CancellationToken &cancellation, const NavMeshAssetLimits &limits = {});
}  // namespace Horo::Navigation
