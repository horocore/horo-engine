#pragma once
/** @file FoliagePaintProducer.h @brief Private bounded paint/erase/selection intent over canonical authored placements. */
#include "FoliagePaletteModel.h"
#include "TerrainSculptProducer.h"

namespace Horo::Editor {
    enum class FoliagePaintMode : std::uint8_t {
        Paint,
        Erase
    };

    /** @brief Explicit gesture parameters. The host issues a fresh persistent-key seed once per new paint gesture. */
    struct FoliagePaintBrush final {
        FoliagePaintMode mode{FoliagePaintMode::Paint};
        double radiusMeters{2};
        std::uint16_t densityPermille{1'000};
        float scale{1};
        float offsetMeters{};
        float yawRadians{};
        bool allTypes{};                        /**< Erase/select all types instead of the explicitly active palette type. */
        std::array<std::byte, 16> gestureKey{}; /**< Identity provenance, independent of source revision/rebakes. */
    };

    /** @brief Explicit whole-stroke ceilings, separate from document transaction/history limits. */
    struct FoliageToolLimits final {
        TerrainSculptLimits stroke;
        std::size_t maximumPlacementEdits{4'096};
        std::size_t maximumExistingPlacements{65'536};
    };

    /** @brief Produce exact stable-ID placement edits without changing source, history or selection.
     * @param document Actual sole source owner. @param fence Captured exact document publication.
     * @param operation Host-issued correlation. @param active Explicit actual type asset, required unless erasing all types.
     * @param brush Captured gesture policy. @param stamps Bounded canonical sample positions. @param tileCells Actual host tile layout.
     * @param limits Whole-stroke finite work/output limits. @param cancellation Cooperative stop.
     * @return Complete TerrainEditOperation to submit through Execute, or typed failure with no mutation. */
    [[nodiscard]] Result<TerrainEditOperation> ProduceFoliagePaint(const TerrainAuthoringDocument &document, const TerrainEditFence &fence,
                                                                   TerrainEditOperationId operation, const FoliagePaletteItem *active,
                                                                   const FoliagePaintBrush &brush,
                                                                   std::span<const TerrainSculptStamp> stamps, std::uint32_t tileCells,
                                                                   const FoliageToolLimits &limits = {},
                                                                   const CancellationToken &cancellation = {});
    /** @brief Query existing authored IDs inside the brush; never return runtime handles or submit edits.
     * @param document Actual sole placement owner. @param fence Exact publication. @param active Selected type or all-types mode.
     * @param brush Radius/type filter only. @param stamps Bounded canonical positions. @param limits Work/output policy.
     * @param cancellation Cooperative stop. @return Sorted stable IDs or typed rejection. */
    [[nodiscard]] Result<std::vector<Terrain::FoliageInstanceId>> SelectFoliageInBrush(
        const TerrainAuthoringDocument &document, const TerrainEditFence &fence, const FoliagePaletteItem *active,
        const FoliagePaintBrush &brush, std::span<const TerrainSculptStamp> stamps, const FoliageToolLimits &limits = {},
        const CancellationToken &cancellation = {});
}  // namespace Horo::Editor
