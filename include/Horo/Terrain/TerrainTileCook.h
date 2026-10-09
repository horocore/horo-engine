#pragma once

/**
 * @file TerrainTileCook.h
 * @brief Deterministic, detached Terrain tile cook values and verification.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Terrain/TerrainSourceImport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Terrain {
    /** @brief Version of the canonical tile and manifest byte layouts. */
    inline constexpr std::uint32_t CurrentTerrainTileCookSchema = 1;

    namespace TerrainTileCookErrors {
        extern const ErrorCodeDescriptor InvalidSource;   /**< Canonical source is malformed or mutated. */
        extern const ErrorCodeDescriptor InvalidProfile;  /**< Profile, target, tier, or dependency is invalid. */
        extern const ErrorCodeDescriptor LimitExceeded;   /**< Finite tile, byte, or work ceiling exceeded. */
        extern const ErrorCodeDescriptor Cancelled;       /**< Operation cancelled without publication. */
        extern const ErrorCodeDescriptor CorruptPrevious; /**< Prior candidate fails integrity or provenance verification. */
    }  // namespace TerrainTileCookErrors

    /** @brief Exact dependency artifact participating in the Terrain cook key. */
    struct TerrainTileCookDependency final {
        Assets::AssetId asset{};       /**< Stable dependency identity. */
        Sha256Digest artifactDigest{}; /**< Exact verified dependency artifact digest. */
    };

    /** @brief Captured finite cook policy and generic Assets target/toolchain envelope. */
    struct TerrainTileCookProfile final {
        std::uint32_t interiorQuads{128};                        /**< Power-of-two quads per tile axis at each LOD. */
        std::uint8_t lodLevels{1};                               /**< Exact number of independently addressed LODs. */
        TerrainFeatureTier tier{TerrainFeatureTier::Baseline};   /**< Exact tier, never an implicit fallback. */
        Sha256Digest targetDigest{};                             /**< Host-supplied generic target/envelope identity. */
        Sha256Digest toolchainDigest{};                          /**< Host-supplied pinned cooker/toolchain identity. */
        std::uint32_t maximumTiles{256};                         /**< Baseline-tier tile/LOD ceiling. */
        std::uint64_t maximumPayloadBytes{128ULL * 1024 * 1024}; /**< Baseline-tier encoded byte ceiling. */
        std::uint64_t maximumWorkItems{1'048'576};               /**< Baseline-tier sample-visit ceiling. */
    };

    /** @brief Independently verifiable, neutral tile payload and semantic edge signatures. */
    struct TerrainCookedTile final {
        TerrainTileId id{};                  /**< Stable dataset and exact signed tile/LOD address. */
        std::uint32_t samplesX{};            /**< Encoded samples along X, including both edges. */
        std::uint32_t samplesZ{};            /**< Encoded samples along Z, including both edges. */
        std::array<Sha256Digest, 4> seams{}; /**< West, east, north, south signatures. */
        Sha256Digest digest{};               /**< SHA-256 over exact payload bytes. */
        std::vector<std::uint8_t> payload{}; /**< Versioned canonical bytes, independently readable. */
    };

    /** @brief Detached complete cook; Assets owns storage and atomic publication outside this type. */
    struct CookedTerrainTileSet final {
        TerrainDatasetId dataset{};             /**< Exact source dataset. */
        Assets::AssetId sourceAsset{};          /**< Exact tracked authoring asset. */
        TerrainSourceRevision sourceRevision{}; /**< Exact authoring revision. */
        std::uint32_t sourceWidth{};            /**< Canonical source columns captured for membership. */
        std::uint32_t sourceHeight{};           /**< Canonical source rows captured for membership. */
        std::uint8_t layerCount{};              /**< Exact number of normalized weight channels. */
        bool hasHoles{};                        /**< Whether a canonical hole channel was authored. */
        TerrainSourceCoordinates coordinates{}; /**< Exact source coordinate metadata. */
        TerrainTileCookProfile profile{};       /**< Exact tier, target and finite cook policy. */
        Sha256Digest sourceDigest{};            /**< Canonical source value digest. */
        Sha256Digest fingerprint{};             /**< All source, dependency, policy, target and schema inputs. */
        Sha256Digest manifestDigest{};          /**< Canonical sorted tile manifest integrity. */
        std::vector<TerrainCookedTile> tiles{}; /**< Canonical LOD, Z, X order. */
    };

    /**
     * @brief Cooks canonical height, weight, hole and coordinate metadata into bounded independent tiles.
     * @param source Detached canonical authoring source borrowed for this call.
     * @param profile Captured exact tier, target, algorithm and finite work limits.
     * @param dependencies Required artifact identities; input order is irrelevant, duplicates fail.
     * @param cancellation Cooperative cancellation checked before publication and during bounded work.
     * @param previous Optional immutable prior result; corruption fails closed, identical tiles may be reused.
     * @return Complete detached candidate or typed failure; no cache, registry or runtime state changes.
     */
    [[nodiscard]] Result<CookedTerrainTileSet> CookTerrainTiles(const TerrainCanonicalSource &source, const TerrainTileCookProfile &profile,
                                                                std::span<const TerrainTileCookDependency> dependencies,
                                                                const CancellationToken &cancellation,
                                                                const CookedTerrainTileSet *previous = nullptr);

    /**
     * @brief Verifies payload hashes, canonical order, source provenance and complete manifest integrity.
     * @param cooked Candidate or cache result to verify before host publication or reuse.
     * @param cancellation Cooperative observer checked between tiles, sample batches and hash chunks.
     * @return Success or typed corruption failure; does not repair a partial generation.
     */
    [[nodiscard]] Result<void> VerifyCookedTerrainTiles(const CookedTerrainTileSet &cooked, const CancellationToken &cancellation = {});
}  // namespace Horo::Terrain
