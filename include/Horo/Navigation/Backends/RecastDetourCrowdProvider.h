#pragma once

/**
 * @file RecastDetourCrowdProvider.h
 * @brief Optional bounded DetourCrowd avoidance provider composition.
 */

#include "Horo/Navigation/NavigationCrowdAvoidance.h"

#include <cstdint>
#include <memory>

namespace Horo::Navigation {
    /** @brief Fixed provider capacities allocated once at host composition, before concurrent queries. */
    struct RecastDetourCrowdLimits final {
        std::uint32_t maximumNeighbors{32};        /**< Fixed native circle capacity in [1, 256]. */
        std::uint32_t maximumBoundarySegments{32}; /**< Fixed native segment capacity in [1, 256]. */
    };

    /**
     * @brief Creates an optional best-effort Detour obstacle-avoidance provider without native public types.
     * @param limits Fixed query storage bounds, copied at creation.
     * @return Owned provider or typed invalid/capacity/unsupported failure; no partial publication.
     */
    [[nodiscard]] Result<std::unique_ptr<INavigationCrowdBackend>> CreateRecastDetourCrowdBackend(
        const RecastDetourCrowdLimits &limits = {});
}  // namespace Horo::Navigation
