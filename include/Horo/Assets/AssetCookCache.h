#pragma once

/**
 * @file AssetCookCache.h
 * @brief Immutable content-addressed cooked artifact cache with versioned complete input identities.
 */

#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Assets {

    /**
     * @brief The canonical pre-image that produces the CacheKeyV1 digest.
     * @details Every byte-affecting input is represented exactly once. Changing any
     *          semantic input, schema, cooker contract, target profile, or artifact
     *          envelope version must produce a different cache key.
     */
    struct AssetCookCacheKeyInputs {
        AssetId assetId;                       /**< Stable asset identity (16 bytes on wire). */
        AssetTypeId assetType;                 /**< Registered asset type (length-delimited). */
        Sha256Digest sourceDigest;             /**< SHA-256 of the exact source bytes. */
        Sha256Digest metadataDigest;           /**< SHA-256 of canonical cooker-visible metadata. */
        std::uint32_t metadataSchemaVersion;   /**< Metadata schema version. */
        Sha256Digest settingsDigest;           /**< SHA-256 of canonical effective settings. */
        std::uint32_t settingsSchemaVersion;   /**< Effective settings schema version. */
        std::string_view cookerContributionId; /**< Stable cooker contribution identity. */
        std::string_view cookerVersion;        /**< Cooker version string. */
        AssetCookTargetId target;              /**< Cook target ID (length-delimited). */
        std::uint32_t artifactFormatVersion;   /**< Artifact envelope format version. */
    };

    /**
     * @brief Immutable content-addressed key for cache entry lookup.
     */
    struct AssetCookCacheKey {
        Sha256Digest digest; /**< The complete versioned cache-key digest. */

        /** @brief Lexicographic comparison on digest bytes. */
        [[nodiscard]] auto operator<=>(const AssetCookCacheKey &) const noexcept = default;
    };

    /**
     * @brief Builds the canonical CacheKeyV1 digest from length-delimited inputs.
     * @param inputs All byte-affecting inputs for a cook invocation.
     * @return The immutable cache key.
     */
    [[nodiscard]] AssetCookCacheKey BuildAssetCookCacheKey(const AssetCookCacheKeyInputs &inputs);

    /** @brief Exact immutable dependency artifact evidence captured by the host, never an asset path. */
    struct AssetCookDependencyIdentity final {
        AssetId id;                  /**< Stable dependency identity. */
        AssetTypeId type;            /**< Type from the pinned registry. */
        Sha256Digest artifactDigest; /**< Digest of the verified complete dependency envelope. */
    };

    /**
     * @brief Builds CacheKeyV2 without changing the existing V1 encoding or entries.
     * @param inputs Complete single-source V1 inputs.
     * @param dependencies Unique host-verified dependency artifacts; input order is immaterial.
     * @param semanticInputsDigest Digest of host-canonical project, package, resolver and role-specific inputs.
     * @param maximumDependencies Positive admission bound checked before copying or sorting.
     * @return Complete V2 key, or a typed bounds/identity failure for invalid or duplicate dependencies.
     * @details The key commits to the V1 digest, semantic-input digest and ascending AssetId/type/artifact
     * digest table. This function hashes evidence; it does not verify source files or grant publication authority.
     * @throws std::bad_alloc if bounded scratch allocation fails; no cache or publication is modified.
     */
    [[nodiscard]] Result<AssetCookCacheKey> BuildAssetCookCacheKeyV2(const AssetCookCacheKeyInputs &inputs,
                                                                     std::span<const AssetCookDependencyIdentity> dependencies,
                                                                     const Sha256Digest &semanticInputsDigest,
                                                                     std::size_t maximumDependencies);

    /**
     * @brief Immutable content-addressed cooked artifact cache.
     * @details Keys map to the full encoded artifact envelope bytes. Entries are
     *          never overwritten with different bytes. Lookup includes constant-time
     *          digest comparison and full envelope verification.
     *
     *          Layout: <root>/<first-two-hex>/<remaining-hex>.cooked
     */
    class AssetCookCache final {
    public:
        /**
         * @brief Constructs a cache rooted at the given directory.
         * @param root Cache root directory. Created if it does not exist.
         * @param limits Size bounds applied to loaded and stored entries.
         */
        explicit AssetCookCache(std::filesystem::path root, const AssetCookLimits &limits = {});

        /**
         * @brief Loads and verifies a cached artifact envelope.
         * @param key Cache key to look up.
         * @param cancellation Cancellation token for cooperative abort.
         * @return Verified artifact envelope bytes, nullopt on miss, or typed error on corruption.
         */
        [[nodiscard]] Result<std::optional<std::vector<std::uint8_t>>> Load(const AssetCookCacheKey &key,
                                                                            const CancellationToken &cancellation) const;

        /**
         * @brief Stores an artifact envelope into the cache.
         * @param key Cache key for the entry.
         * @param artifact Full encoded artifact envelope bytes.
         * @param cancellation Cancellation token for cooperative abort.
         * @return Result<void> indicating success or a typed store error.
         * @details Writes to a temporary sibling file, then atomically renames.
         *          If the key path already exists, verifies existing bytes are
         *          identical before discarding the temp file.
         */
        [[nodiscard]] Result<void> Store(const AssetCookCacheKey &key, std::span<const std::uint8_t> artifact,
                                         const CancellationToken &cancellation) const;

    private:
        /** @brief Builds the filesystem path for a cache key. */
        [[nodiscard]] std::filesystem::path PathForKey(const Sha256Digest &digest) const;

        std::filesystem::path root_;
        AssetCookLimits limits_;
    };

}  // namespace Horo::Assets
