#pragma once

/**
 * @file AssetArchive.h
 * @brief Deterministic release asset archive and verified runtime provider.
 */

#include "Horo/Assets/AssetChunkPlan.h"
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/AssetCookTargetId.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Assets {
    /** @brief One exact encoded cooked artifact selected for release packaging. */
    struct AssetArchiveInput {
        AssetId id;
        std::vector<std::uint8_t> bytes;
    };

    /** @brief Archive size and shape ceilings checked before allocation or payload reads. */
    struct AssetArchiveLimits {
        std::size_t maximumArchiveBytes{1024U * 1024U * 1024U};
        std::size_t maximumAssetBytes{256U * 1024U * 1024U};
        std::size_t maximumAssets{100'000U};
        std::size_t maximumChunks{1024U};
    };

    /**
     * @brief Encodes one complete, canonical release archive from validated chunk membership and cooked envelopes.
     * @param plan Validated chunk graph.
     * @param target Exact cook target required in every envelope.
     * @param artifacts Complete selected cooked envelope set; extra and missing assets reject publication.
     * @param limits Finite host-owned bounds.
     * @return Complete assets.horo bytes or a typed failure; no partial archive escapes.
     */
    [[nodiscard]] Result<std::vector<std::uint8_t>> BuildAssetArchive(const AssetChunkPlan &plan, const AssetCookTargetId &target,
                                                                      std::span<const AssetArchiveInput> artifacts,
                                                                      const AssetArchiveLimits &limits = {});

    /**
     * @brief Builds an archive from a pinned, fully verified cook generation.
     * @param plan Exact chunk membership for the release.
     * @param generation Frozen generation identity and manifest digest.
     * @param limits Finite archive and artifact bounds.
     * @return Archive bytes or a typed failure before any partial output escapes.
     */
    [[nodiscard]] Result<std::vector<std::uint8_t>> BuildAssetArchive(const AssetChunkPlan &plan, const AssetCookGeneration &generation,
                                                                      const AssetArchiveLimits &limits = {});

    /** @brief Owned identity and type validated from one visible archived cooked envelope. */
    struct AssetArchiveMember final {
        AssetId id;
        AssetTypeId type;
    };

    /** @brief Immutable archive-backed runtime provider with no filesystem fallback. */
    class AssetArchiveProvider final : public IAssetProvider {
    public:
        /**
         * @brief Validates an entire archive before exposing any asset.
         * @param bytes Complete archive bytes.
         * @param expectedTarget Exact runtime cook target.
         * @param limits Allocation and format limits.
         * @return Verified provider or typed corruption/compatibility failure.
         */
        [[nodiscard]] static Result<AssetArchiveProvider> Open(std::span<const std::uint8_t> bytes, const AssetCookTargetId &expectedTarget,
                                                               const AssetArchiveLimits &limits = {});

        /**
         * @brief Opens only dependency-closed selected chunks from an archive matching an authenticated release plan.
         * @param bytes Complete externally authenticated archive bytes.
         * @param expectedTarget Exact runtime cook target.
         * @param expectedPlan Chunk definitions authenticated by the release manifest.
         * @param selected Exact installed chunk IDs, including their base and dependencies.
         * @param baseManifest Digest of the authenticated base release manifest.
         * @param limits Allocation and format bounds.
         * @return Provider exposing selected assets only, or failure before any asset is visible.
         * @note The host verifies package signature and owns provider replacement/unmount; this call does not grant package trust.
         */
        [[nodiscard]] static Result<AssetArchiveProvider> OpenSelected(
            std::span<const std::uint8_t> bytes, const AssetCookTargetId &expectedTarget, const AssetChunkPlan &expectedPlan,
            std::span<const AssetChunkId> selected, const Sha256Digest &baseManifest, const AssetArchiveLimits &limits = {});

        /**
         * @brief Inspect immutable visible members without reading or copying their payloads.
         * @return AssetId-sorted metadata for exactly the assets exposed by this provider.
         * @note The borrowed span is valid until this provider is moved, assigned or destroyed.
         * OpenSelected omits unmounted members. Metadata proves archive integrity, not package authentication.
         */
        [[nodiscard]] std::span<const AssetArchiveMember> Members() const noexcept;

        /**
         * @brief Return the target validated against every visible envelope during archive admission.
         * @return Provider-owned target; valid until this provider is moved, assigned or destroyed.
         */
        [[nodiscard]] const AssetCookTargetId &Target() const noexcept;

        [[nodiscard]] Result<bool> Exists(AssetId id, const CancellationToken &cancellation) const override;
        [[nodiscard]] Result<std::vector<std::uint8_t>> Load(AssetId id, const CancellationToken &cancellation) const override;

    private:
        struct Entry {
            AssetId id;
            std::size_t offset{};
            std::size_t size{};
        };

        AssetArchiveProvider(std::vector<std::uint8_t> bytes, std::vector<Entry> entries, std::vector<AssetChunkDefinition> chunks,
                             std::vector<AssetArchiveMember> members, AssetCookTargetId target);
        /** @brief Parses the complete archive and retains its internal graph for selected admission. */
        [[nodiscard]] static Result<AssetArchiveProvider> OpenParsed(std::span<const std::uint8_t> bytes,
                                                                     const AssetCookTargetId &expectedTarget,
                                                                     const AssetArchiveLimits &limits);

        std::vector<std::uint8_t> bytes_;
        std::vector<Entry> entries_;
        std::vector<AssetChunkDefinition> chunks_;
        std::vector<AssetArchiveMember> members_;
        AssetCookTargetId target_;
    };
}  // namespace Horo::Assets
