#pragma once

#include "Horo/Navigation/NavigationCrowdSnapshot.h"

#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <vector>

namespace Horo::Navigation::Detail {
    struct NavigationCrowdCellEntry final {
        std::int32_t x{};
        std::int32_t z{};
        std::uint32_t index{};

        [[nodiscard]] constexpr auto operator<=>(const NavigationCrowdCellEntry &) const noexcept = default;
    };

    struct NavigationCrowdSnapshotStorage final {
        NavigationAgentSceneBinding binding;
        NavigationDynamicRegistryRevision dynamicRevision;
        std::uint64_t captureTick{};
        AvoidanceExecutionMode mode{AvoidanceExecutionMode::Disabled};
        std::vector<NavigationCrowdAgentFact> agents;
        std::vector<NavigationCrowdProfileFacts> profiles;
        std::vector<NavigationAvoidanceLayerDescriptor> avoidanceLayers;
        std::uint64_t avoidanceLayerBits{};
        std::vector<std::uint32_t> neighborIndices;
        std::vector<std::uint32_t> boundaryIndices;
        std::vector<NavigationCrowdBoundarySegment> segments;
        std::vector<NavigationCrowdCell> cells;
        std::vector<std::uint32_t> cellAgentIndices;
        std::vector<NavigationCrowdCellEntry> boundaryCellEntries;
        std::vector<NavigationCrowdProfileTruncation> truncation;
    };

    [[nodiscard]] inline bool CrowdCellCoordinate(const double value, const float cellSize, std::int32_t &coordinate) noexcept {
        if (!std::isfinite(value))
            return false;
        const double cell = std::floor(value / static_cast<double>(cellSize));
        if (cell < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
            cell > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
            return false;
        coordinate = static_cast<std::int32_t>(cell);
        return true;
    }

    [[nodiscard]] Result<void> CaptureCrowdBoundaries(const NavigationDynamicRegistrySnapshot &dynamic,
                                                      const NavigationCrowdSnapshotLimits &limits, NavigationCrowdSnapshotStorage &storage);
    [[nodiscard]] Result<void> PartitionCrowdAgents(const NavigationCrowdSnapshotLimits &limits, NavigationCrowdSnapshotStorage &storage);
    [[nodiscard]] Result<void> GatherCrowdFacts(const NavigationCrowdSnapshotLimits &limits, NavigationCrowdSnapshotStorage &storage);
}  // namespace Horo::Navigation::Detail
