#pragma once

/** @file AssetCookPreparation.h @brief Target-private owned candidate preparation shared by the joined cook and publication pipeline. */
#include "AssetCookOperationScope.h"
#include "Horo/Assets/AssetCookCache.h"
#include "Horo/Assets/AssetCookService.h"

namespace Horo::Assets::Detail {
    /** @brief Owns admitted source bytes and all subsequent cache/cook state; accepted jobs drain before this storage is released. */
    struct CookSlot final {
        AssetRecord record;
        std::vector<std::uint8_t> sourceBytes;
        Sha256Digest sourceDigest;
        AssetCookCacheKey cacheKey;
        bool cacheHit{false};
        std::vector<std::uint8_t> cacheArtifact;
        std::vector<std::uint8_t> cookedArtifact;
        std::optional<Error> cookError;
    };

    /** @brief Adds authoritative source navigation to a typed asset-scoped result.
     * @param request Joined operation's source authority. @param record Exact admitted asset.
     * @param operation Borrowed operation owner. @param result Owned result enriched before publication. */
    void PublishAssetResult(const AssetCookRequest &request, const AssetRecord &record, const CookOperationScope &operation,
                            BuildOutputRecord result);

    /** @brief Admits sources and requested V1/V2 cache identities into operation-owned slots before jobs are scheduled.
     * @param request Bounded operation input and optional immutable pin. @param catalog Selected immutable strategies.
     * @param records Exact candidate registry subset. @param operation Joined operation and diagnostic authority.
     * @param dependencies Exact resource envelope identities for a dependent candidate phase.
     * @return Owned complete slot set or typed source/identity/cooker admission failure, without publication. */
    [[nodiscard]] Result<std::vector<CookSlot>> PrepareCookSlots(const AssetCookRequest &request, const CookerCatalogSnapshot &catalog,
                                                                 std::span<const AssetRecord> records, CookOperationScope &operation,
                                                                 std::span<const AssetCookDependencyIdentity> dependencies = {});
}  // namespace Horo::Assets::Detail
