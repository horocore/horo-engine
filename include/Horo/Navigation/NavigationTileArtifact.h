#pragma once

/** @file NavigationTileArtifact.h
 * @brief Immutable validated content identities and portable tile bytes for incremental cook reuse.
 */

#include "Horo/Navigation/NavigationProjectProfiles.h"
#include "Horo/Navigation/NavigationTileDependencies.h"

#include <optional>

namespace Horo::Navigation {
    /** @brief Validated tile shared by a candidate, a last-valid generation and retained readers. */
    class NavigationCookedTile final {
        struct ConstructionKey {
        private:
            friend class NavigationCookedTile;
            ConstructionKey() = default;
        };

    public:
        /** @brief Factory-only construction; the private key prevents bypassing Create validation.
         * @param key Validated partition/grid identity. @param dependency Validated complete dependency key.
         * @param result Validated owned neutral topology. @param bytes Canonical encoded topology.
         */
        NavigationCookedTile(ConstructionKey, const NavigationBakeTileKey &key, const Sha256Digest &dependency,
                             NavigationTileBuildResult result, std::vector<std::uint8_t> bytes);
        /**
         * @brief Validates and encodes a complete builder result without publishing it.
         * @param input Exact canonical tile work descriptor.
         * @param result Owned fresh builder output, transferred only to the detached artifact.
         * @param maximumBytes Positive tile byte ceiling, at most the builder hard ceiling.
         * @return Shared immutable artifact or typed invalid/corrupt/capacity failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<const NavigationCookedTile>> Create(
            const NavigationPreparedTile &input, NavigationTileBuildResult result,
            std::size_t maximumBytes = NavigationTileBuildLimits::MaximumOwnedBytes);
        /**
         * @brief Decodes and semantically validates exact portable tile bytes before cache/runtime use.
         * @param bytes Complete encoded tile; truncation and trailing bytes are rejected.
         * @param maximumBytes Positive byte ceiling no greater than the hard ceiling.
         * @return Immutable artifact or typed corrupt/capacity failure; no partially decoded artifact escapes.
         */
        [[nodiscard]] static Result<std::shared_ptr<const NavigationCookedTile>> Decode(
            std::span<const std::uint8_t> bytes, std::size_t maximumBytes = NavigationTileBuildLimits::MaximumOwnedBytes);

        /** @brief Returns measured retained storage, including vector capacities and portable bytes.
         * @return Object and owned table/byte storage, excluding the shared-ownership control block.
         */
        [[nodiscard]] std::size_t StorageBytes() const noexcept;

        /** @brief Returns the stable partition/grid identity. @return Immutable tile address. */
        [[nodiscard]] const NavigationBakeTileKey &Key() const noexcept {
            return key_;
        }

        /** @brief Returns the exact complete dependency key. @return Versioned input digest. */
        [[nodiscard]] const Sha256Digest &DependencyKey() const noexcept {
            return dependencyKey_;
        }

        /** @brief Returns the digest of the actual portable bytes. @return Content identity independent of request generation. */
        [[nodiscard]] const Sha256Digest &ContentIdentity() const noexcept {
            return contentIdentity_;
        }

        /** @brief Returns the validated neutral topology. @return Borrowed immutable builder output. */
        [[nodiscard]] const NavigationTileBuildResult &Topology() const noexcept {
            return topology_;
        }

        /** @brief Returns the byte-identical portable cache/cook payload. @return Borrowed bytes retained by this artifact. */
        [[nodiscard]] std::span<const std::uint8_t> Bytes() const noexcept {
            return bytes_;
        }

    private:
        NavigationBakeTileKey key_;
        Sha256Digest dependencyKey_;
        Sha256Digest contentIdentity_;
        NavigationTileBuildResult topology_;
        std::vector<std::uint8_t> bytes_;
    };

    /**
     * @brief Producer-captured compatibility and optional project authority persisted in HNS2.
     * @details Digests come from the exact bake request, never reconstructed from a tile dependency key.
     * A missing project profile identifies non-release content; required release admission rejects it.
     * The codec derives and validates the surface/profile closure from the immutable tiles.
     */
    struct NavigationCookedContentProvenance final {
        NavigationTileBakeCompatibility compatibility;
        std::optional<NavigationProjectProfile> projectProfile;
    };

    /** @brief Complete definition-rooted cooked replacement closure; tiles never acquire authoring AssetIds. */
    struct NavigationCookedTileSet final {
        Sha256Digest inputFingerprint;
        std::vector<std::shared_ptr<const NavigationCookedTile>> tiles; /**< Strictly key-sorted complete closure, including empty tiles. */
        std::optional<NavigationCookedContentProvenance> provenance;    /**< Absent only for explicit HNS1 legacy content. */
    };

    /**
     * @brief Encodes a complete sorted tile closure into the core.navmesh cook payload.
     * @param set Complete immutable candidate tiles and source fingerprint; captured provenance emits HNS2, absence emits legacy HNS1.
     * @param maximumBytes Aggregate encoded byte ceiling.
     * @return Canonical portable payload or typed invalid/capacity failure.
     */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodeNavigationCookedTileSet(const NavigationCookedTileSet &set,
                                                                                  std::size_t maximumBytes);
    /**
     * @brief Validates explicit HNS1 legacy or HNS2 provenance and every tile's actual-byte content identity.
     * @details HNS1 never acquires inferred provenance. Unknown versions and incomplete HNS2 metadata fail closed.
     * @param bytes Complete versioned tile-set payload from a verified asset envelope.
     * @param maximumBytes Aggregate encoded and retained decoded storage ceiling; one bounded tile is decoded before checking its retained
     * size.
     * @return Owned immutable tile set or typed corrupt/capacity failure.
     */
    [[nodiscard]] Result<NavigationCookedTileSet> DecodeNavigationCookedTileSet(std::span<const std::uint8_t> bytes,
                                                                                std::size_t maximumBytes);
}  // namespace Horo::Navigation
