#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationCrowdSnapshotInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <ranges>
#include <type_traits>
#include <variant>
#include <vector>

namespace Horo::Navigation::Detail {
    namespace {
        [[nodiscard]] Result<void> InvalidBoundary() {
            return Result<void>::Failure(MakeError(NavigationErrors::DynamicRegistryInvalid));
        }

        [[nodiscard]] Result<void> CapacityExceeded() {
            return Result<void>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }

        struct RectangleFootprint final {
            Math::Vec3 center;
            float halfX{};
            float halfY{};
            float halfZ{};
        };

        /** @brief Projects supported source shapes to a conservative axis-aligned footprint. */
        [[nodiscard]] std::optional<RectangleFootprint> FootprintFor(const NavigationDynamicShape &shape) {
            return std::visit([]<typename Shape>(const Shape &value) -> std::optional<RectangleFootprint> {
                if (!value.IsValid())
                    return std::nullopt;
                if constexpr (std::is_same_v<Shape, NavigationDynamicBoxShape>)
                    return RectangleFootprint{value.center, value.halfExtents.x, value.halfExtents.y, value.halfExtents.z};
                else
                    return RectangleFootprint{value.center, value.radius, value.halfHeight, value.radius};
            }, shape);
        }

        [[nodiscard]] Result<void> IndexSegmentCells(const NavigationCrowdBoundarySegment &segment, const std::uint32_t segmentIndex,
                                                     const NavigationCrowdSnapshotLimits &limits, NavigationCrowdSnapshotStorage &storage) {
            std::int32_t firstX{};
            std::int32_t lastX{};
            std::int32_t firstZ{};
            std::int32_t lastZ{};
            if (!CrowdCellCoordinate(std::min(segment.first.x, segment.second.x), limits.cellSizeMeters, firstX) ||
                !CrowdCellCoordinate(std::max(segment.first.x, segment.second.x), limits.cellSizeMeters, lastX) ||
                !CrowdCellCoordinate(std::min(segment.first.z, segment.second.z), limits.cellSizeMeters, firstZ) ||
                !CrowdCellCoordinate(std::max(segment.first.z, segment.second.z), limits.cellSizeMeters, lastZ))
                return InvalidBoundary();
            const auto width = static_cast<std::int64_t>(lastX) - firstX + 1;
            const auto height = static_cast<std::int64_t>(lastZ) - firstZ + 1;
            if (const std::size_t remaining = limits.maximumCellEntries - storage.agents.size() - storage.boundaryCellEntries.size();
                width > static_cast<std::int64_t>(remaining) || height > static_cast<std::int64_t>(remaining) ||
                width * height > static_cast<std::int64_t>(remaining))
                return CapacityExceeded();
            for (std::int64_t x = firstX; x <= lastX; ++x) {
                for (std::int64_t z = firstZ; z <= lastZ; ++z)
                    storage.boundaryCellEntries.push_back({static_cast<std::int32_t>(x), static_cast<std::int32_t>(z), segmentIndex});
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendRectangle(const NavigationCrowdBoundarySource &source, const std::uint64_t stableId,
                                                   const NavigationDynamicLayerMask layers, const NavigationDynamicShape &shape,
                                                   const NavigationCrowdSnapshotLimits &limits, NavigationCrowdSnapshotStorage &storage) {
            const auto footprint = FootprintFor(shape);
            if (!footprint)
                return InvalidBoundary();
            const auto &[center, halfX, halfY, halfZ] = *footprint;
            if (storage.segments.size() + 4 > limits.maximumBoundarySegments)
                return CapacityExceeded();
            const float minimumY = center.y - halfY;
            const float maximumY = center.y + halfY;
            if (!std::isfinite(minimumY) || !std::isfinite(maximumY))
                return InvalidBoundary();
            const std::array<Math::Vec3, 4> corners{{{center.x - halfX, center.y, center.z - halfZ},
                                                     {center.x + halfX, center.y, center.z - halfZ},
                                                     {center.x + halfX, center.y, center.z + halfZ},
                                                     {center.x - halfX, center.y, center.z + halfZ}}};
            if (corners[0].x == corners[1].x || corners[1].z == corners[2].z)
                return InvalidBoundary();
            for (std::uint8_t edge = 0; edge < corners.size(); ++edge) {
                const NavigationCrowdBoundarySegment segment{.source = source,
                                                             .stableId = stableId,
                                                             .edge = edge,
                                                             .layers = layers,
                                                             .first = corners[edge],
                                                             .second = corners[(edge + 1U) % corners.size()],
                                                             .minimumY = minimumY,
                                                             .maximumY = maximumY};
                if (const auto indexed = IndexSegmentCells(segment, static_cast<std::uint32_t>(storage.segments.size()), limits, storage);
                    indexed.HasError())
                    return indexed;
                storage.segments.push_back(segment);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @brief Builds stable XZ cells for every captured agent without retaining the source registry. */
    Result<void> PartitionCrowdAgents(const NavigationCrowdSnapshotLimits &limits, NavigationCrowdSnapshotStorage &storage) {
        if (storage.agents.size() > limits.maximumCellEntries)
            return CapacityExceeded();
        std::vector<NavigationCrowdCellEntry> entries;
        entries.reserve(storage.agents.size());
        for (std::size_t index = 0; index < storage.agents.size(); ++index) {
            std::int32_t x{};
            std::int32_t z{};
            if (!CrowdCellCoordinate(storage.agents[index].position.x, limits.cellSizeMeters, x) ||
                !CrowdCellCoordinate(storage.agents[index].position.z, limits.cellSizeMeters, z))
                return Result<void>::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
            entries.push_back({x, z, static_cast<std::uint32_t>(index)});
        }
        std::ranges::sort(entries);
        storage.cellAgentIndices.reserve(entries.size());
        storage.cells.reserve(entries.size());
        for (const auto &entry : entries) {
            if (storage.cells.empty() || storage.cells.back().x != entry.x || storage.cells.back().z != entry.z)
                storage.cells.push_back(
                    {.x = entry.x, .z = entry.z, .firstAgent = static_cast<std::uint32_t>(storage.cellAgentIndices.size())});
            storage.cellAgentIndices.push_back(entry.index);
            ++storage.cells.back().agentCount;
        }
        return Result<void>::Success();
    }

    /** @brief Copies enabled obstacle and exclusion-modifier footprints into bounded conservative segments. */
    Result<void> CaptureCrowdBoundaries(const NavigationDynamicRegistrySnapshot &dynamic, const NavigationCrowdSnapshotLimits &limits,
                                        NavigationCrowdSnapshotStorage &storage) {
        for (const auto &obstacle : dynamic.Obstacles()) {
            if (!obstacle.enabled)
                continue;
            const auto appended = AppendRectangle(obstacle.handle, obstacle.id.Value(), obstacle.layers, obstacle.shape, limits, storage);
            if (appended.HasError())
                return appended;
        }
        for (const auto &modifier : dynamic.Modifiers()) {
            if (!modifier.enabled || modifier.operation != NavigationDynamicModifierOperation::Exclude)
                continue;
            const auto appended = AppendRectangle(modifier.handle, modifier.id.Value(), modifier.layers, modifier.shape, limits, storage);
            if (appended.HasError())
                return appended;
        }
        std::ranges::sort(storage.boundaryCellEntries);
        return Result<void>::Success();
    }
}  // namespace Horo::Navigation::Detail
