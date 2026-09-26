#pragma once

#include "Horo/Terrain/TerrainSourceImport.h"

namespace Horo::Terrain::ImportDetail {
    /** @brief Detached scalar pixels returned by the format-specific PNG boundary. */
    struct DecodedPngRaster final {
        TerrainRasterFormat format{TerrainRasterFormat::Count};
        std::vector<std::byte> bytes;
    };

    /** @brief Decodes a bounded non-interlaced grayscale PNG to operation-owned raw bytes. */
    [[nodiscard]] Result<DecodedPngRaster> DecodePngGray(const TerrainRasterInput &source, std::uint64_t maximumDecodedBytes);
}  // namespace Horo::Terrain::ImportDetail
