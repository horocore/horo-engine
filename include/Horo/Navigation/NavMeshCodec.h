#pragma once

/** @file NavMeshCodec.h
 * @brief Bounded little-endian wire representation of the existing NavMeshData schema. */

#include "Horo/Navigation/NavMeshData.h"

namespace Horo::Navigation {
    /** @brief Exact encoded tile byte range inside one verified artifact. */
    struct NavMeshEncodedTileRange final {
        NavMeshTileKey key;
        std::size_t offset{};
        std::size_t bytes{};
    };

    /** @brief Validated decoded data and locations of independently hashed immutable tile bytes. */
    struct DecodedNavMeshArtifact final {
        NavMeshData data;
        std::vector<NavMeshEncodedTileRange> encodedTiles;
    };

    /** @brief Encode the existing neutral tables; recompute actual wire sizes and checksums.
     * @param artifact Valid decoded schema with parser-observed digest evidence.
     * @param limits Qualified schema/storage ceilings.
     * @return Canonical little-endian uncompressed bytes or typed validation failure.
     * @note Compression is rejected; this codec never substitutes provider-private or authored records. */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeNavMeshArtifact(const NavMeshArtifactView &artifact,
                                                                       const NavMeshArtifactLimits &limits = {});

    /** @brief Preflight fixed facts before allocation, verify exact bytes, then validate every neutral table.
     * @param bytes Exact cooked neutral artifact, without the AssetCook envelope.
     * @param limits Qualified schema/storage ceilings.
     * @return Owned decoded data and bounded encoded tile ranges, or typed corrupt/unsupported/capacity failure. */
    [[nodiscard]] Result<DecodedNavMeshArtifact> DecodeNavMeshArtifact(std::span<const std::byte> bytes,
                                                                       const NavMeshArtifactLimits &limits = {});
}  // namespace Horo::Navigation
