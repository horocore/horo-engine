#pragma once

#include "Horo/WorldStreaming/WorldDependencyArtifact.h"

namespace Horo::WorldStreaming::Detail {
    /** @brief Captures only the already validated owner plans and verifies their exact shared object/revision projection. */
    [[nodiscard]] Result<DecodedWorldDependencies> CaptureWorldDependencyProjection(
        const DecodedWorldIndex &index, const WorldSpatialAssignment &assignments, const WorldSpanningObjectPlan &placements,
        const WorldDependencyPlan &dependencies, const WorldDependencyArtifactLimits &limits, const CancellationToken &cancellation);

    /**
     * @brief Checks bounded canonical projection and derives the complete hard-cell closure without trusting stored edges.
     * @return Canonical cell rows, including zero-edge cells; caller compares decoded rows before admitting the sibling.
     */
    [[nodiscard]] Result<std::vector<WorldDependencyCellProjection>> DeriveWorldDependencyCells(const DecodedWorldDependencies &projection,
                                                                                                const DecodedWorldIndex &index,
                                                                                                const WorldDependencyArtifactLimits &limits,
                                                                                                const CancellationToken &cancellation);
}  // namespace Horo::WorldStreaming::Detail
