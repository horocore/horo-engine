#pragma once
/** @file TerrainSculptProducer.h @brief Private bounded exact-patch producer for workspace terrain strokes. */
#include "Horo/Editor/TerrainAuthoringDocument.h"

namespace Horo::Editor {
    /** @brief Closed sculpt operations; no tool mutates canonical document storage. */
    enum class TerrainSculptMode : std::uint8_t {
        Raise,
        Lower,
        Smooth,
        Flatten,
        Noise
    };

    /** @brief One detached brush position in canonical sample coordinates. */
    struct TerrainSculptStamp final {
        double sampleX{}, sampleZ{};
    };

    /** @brief Transient stroke policy, captured once before pointer motion. */
    struct TerrainSculptBrush final {
        TerrainSculptMode mode{TerrainSculptMode::Raise};
        double radiusMeters{2.0};
        float amountMeters{0.25F};
        float blend{0.25F};
        float flattenMeters{};
        std::uint64_t noiseSeed{1};
    };

    /** @brief Explicit interaction work/history ceilings, admitted before patch allocation. */
    struct TerrainSculptLimits final {
        std::size_t maximumStamps{64};
        std::size_t maximumPatchSamples{16'384};
        std::size_t maximumSampleVisits{1'048'576};
    };

    /** @brief Compute one stroke as an exact region-only command over an immutable document source.
     * @param source Canonical document borrow valid for this synchronous producer call.
     * @param fence Exact source/capability/session fence. @param operation Host-issued correlation.
     * @param brush Captured transient brush policy. @param stamps Complete bounded stroke positions.
     * @param limits Explicit work and patch ceilings. @param cancellation Checked before allocation and per row/stamp.
     * @return Exact patch command or typed stale/invalid/budget/cancellation error without source mutation.
     */
    [[nodiscard]] Result<TerrainEditOperation> ProduceTerrainSculpt(const Terrain::TerrainCanonicalSource &source, TerrainEditFence fence,
                                                                    TerrainEditOperationId operation, const TerrainSculptBrush &brush,
                                                                    std::span<const TerrainSculptStamp> stamps,
                                                                    const TerrainSculptLimits &limits = {},
                                                                    const CancellationToken &cancellation = {});
}  // namespace Horo::Editor
