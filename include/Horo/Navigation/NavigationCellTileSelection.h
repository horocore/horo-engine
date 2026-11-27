#pragma once

/** @file NavigationCellTileSelection.h
 * @brief Portable membership in an existing cooked NavMesh definition.
 */
#include "Horo/Assets/AssetId.h"
#include "Horo/Navigation/NavigationTileArtifact.h"

namespace Horo::Navigation {
    /** @brief Cell membership in an existing cooked definition, independent of cell/runtime identities. */
    struct NavigationCellTileSelection final {
        static constexpr std::uint32_t WireVersion = 1;
        Assets::AssetId definition;
        Sha256Digest cookedEnvelope;              /**< Digest of the complete immutable Assets envelope. */
        std::vector<NavigationBakeTileKey> tiles; /**< Strictly sorted addresses in one coordinate scope. */
    };

    /** @brief Encodes bounded version-one membership for a required World cell NavigationMesh block.
     * @param selection Exact base envelope and canonical tile membership.
     * @param maximumBytes Maximum output byte count.
     * @return Portable bytes or a typed artifact/capacity error. No provider state is allocated.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeNavigationCellTileSelection(const NavigationCellTileSelection &selection,
                                                                                   std::size_t maximumBytes);

    /** @brief Decodes version-one membership after the caller verifies the enclosing cell integrity and TOC version.
     * @param bytes Exact provider block, including no trailing bytes. @param maximumBytes Caller byte ceiling.
     * @return Owned canonical membership or a typed artifact/capacity error.
     */
    [[nodiscard]] Result<NavigationCellTileSelection> DecodeNavigationCellTileSelection(std::span<const std::byte> bytes,
                                                                                        std::size_t maximumBytes);
}  // namespace Horo::Navigation
