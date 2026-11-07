#pragma once
/** @file TerrainAssetFormat.h @brief Private Assets importer seam for explicit height-only terrain RAW-F32 source. */
#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Terrain/TerrainSourceImport.h"

namespace Horo::Editor {
    inline constexpr std::string_view TerrainRawImporterId = "horo.terrain.raw-f32";
    inline constexpr std::string_view TerrainRawImporterVersion = "1.0.0";
    /** @brief Build explicit terrainraw importer metadata; generic raw/image extensions remain untouched. */
    [[nodiscard]] Assets::AssetImporterContribution TerrainRawImporterContribution();
    /** @brief Resolve the exact versioned Assets sidecar and decode existing RAW-F32 bytes through Terrain normalization.
     * @param metadata Existing Assets identity/provenance; contribution/type/version must match exactly.
     * @param bytes Exact current tracked source bytes. @param revision New document session's source revision.
     * @param cancellation Cooperative observer. @return Canonical height-only source or typed format/identity/budget failure.
     */
    [[nodiscard]] Result<Terrain::TerrainCanonicalSource> DecodeTerrainAsset(const Assets::AssetImportMetadata &metadata,
                                                                             std::span<const std::byte> bytes,
                                                                             Terrain::TerrainSourceRevision revision,
                                                                             const CancellationToken &cancellation = {});
    /** @brief Encode only explicitly admitted height-only source in the same existing RAW-F32 little-endian representation.
     * @param source Canonical document source. @param cancellation Cooperative observer.
     * @return Exact bytes or typed unsupported channel/invalid/budget/cancellation failure; no metadata changes.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeTerrainHeights(const Terrain::TerrainCanonicalSource &source,
                                                                      const CancellationToken &cancellation = {});
}  // namespace Horo::Editor
