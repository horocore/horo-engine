#pragma once

#include "Horo/Navigation/Backends/RecastDetourProvider.h"

#include <DetourNavMesh.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace Horo::Navigation::Detail {
    [[nodiscard]] inline bool IsPositiveFinite(const float value) noexcept {
        return std::isfinite(value) && value > 0.0F;
    }

    [[nodiscard]] inline bool IsNonNegativeFinite(const float value) noexcept {
        return std::isfinite(value) && value >= 0.0F;
    }

    [[nodiscard]] inline bool CheckedAdd(std::size_t &total, const std::size_t value) noexcept {
        if (const std::size_t remaining = std::numeric_limits<std::size_t>::max() - total; value > remaining)
            return false;
        total = total + value;
        return true;
    }

    [[nodiscard]] inline bool CheckedProduct(const std::size_t first, const std::size_t second, std::size_t &result) noexcept {
        if (first == 0) {
            result = 0;
            return true;
        }
        if (second > std::numeric_limits<std::size_t>::max() / first)
            return false;
        result = first * second;
        return true;
    }

    [[nodiscard]] inline bool FitsOwnedBudget(const RecastDetourProviderCreateInfo &info) noexcept {
        std::size_t bytes{};
        if (std::size_t value{};
            !CheckedProduct(info.vertices.size(), 64U, value) || !CheckedAdd(bytes, value) ||
            !CheckedProduct(info.polygons.size(), 256U, value) || !CheckedAdd(bytes, value) ||
            !CheckedProduct(info.maximumConcurrentQueries,
                            (static_cast<std::size_t>(std::max(info.maximumQueryNodes, info.maximumResultPoints)) * sizeof(dtPolyRef)) +
                                (static_cast<std::size_t>(info.maximumResultPoints) *
                                 ((sizeof(float) * 3U) + sizeof(unsigned char) + sizeof(dtPolyRef))),
                            value) ||
            !CheckedAdd(bytes, value))
            return false;
        return bytes <= info.maximumOwnedBytes;
    }

    [[nodiscard]] inline bool HasValidIdentityAndTopologyBounds(const RecastDetourProviderCreateInfo &info) noexcept {
        return info.world.IsValid() && info.topology.IsValid() && info.vertices.size() >= 3 && !info.polygons.empty() &&
               info.vertices.size() <= RecastDetourProviderHardLimits::Vertices &&
               info.polygons.size() <= RecastDetourProviderHardLimits::Polygons &&
               info.vertices.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
               info.polygons.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max());
    }

    [[nodiscard]] inline bool HasValidAgentSettings(const RecastDetourProviderCreateInfo &info) noexcept {
        return Math::IsFinite(info.nearestPointHalfExtents) && info.nearestPointHalfExtents.x > 0.0F &&
               info.nearestPointHalfExtents.y > 0.0F && info.nearestPointHalfExtents.z > 0.0F && IsPositiveFinite(info.cellSizeMeters) &&
               IsPositiveFinite(info.cellHeightMeters) && IsPositiveFinite(info.walkableHeightMeters) &&
               IsNonNegativeFinite(info.walkableRadiusMeters) && IsNonNegativeFinite(info.walkableClimbMeters);
    }

    [[nodiscard]] inline bool HasValidQuerySettings(const RecastDetourProviderCreateInfo &info) noexcept {
        return info.maximumQueryNodes > 0 && info.maximumQueryNodes <= RecastDetourProviderHardLimits::QueryNodes &&
               info.maximumResultPoints >= 2 && info.maximumResultPoints <= RecastDetourProviderHardLimits::ResultPoints &&
               info.maximumConcurrentQueries > 0 && info.maximumConcurrentQueries <= RecastDetourProviderHardLimits::ConcurrentQueries &&
               IsPositiveFinite(info.maximumSearchDistanceMeters) && info.capabilityRevision != 0;
    }

    [[nodiscard]] inline bool HasValidMemoryBudget(const RecastDetourProviderCreateInfo &info) noexcept {
        return info.maximumOwnedBytes > 0 && info.maximumOwnedBytes <= RecastDetourProviderHardLimits::OwnedBytes && FitsOwnedBudget(info);
    }
}  // namespace Horo::Navigation::Detail
