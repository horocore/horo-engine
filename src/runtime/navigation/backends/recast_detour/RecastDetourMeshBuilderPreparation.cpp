#include "RecastDetourMeshBuilderInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <ranges>
#include <tuple>
#include <utility>

namespace Horo::Navigation::RecastDetourMeshBuilderInternal {
    namespace {
        [[nodiscard]] bool IsKnown(const NavigationBakeModifierMode mode) noexcept {
            return mode >= NavigationBakeModifierMode::AssignArea && mode < NavigationBakeModifierMode::Count;
        }

        [[nodiscard]] bool IsKnown(const NavigationSourceProducerKind kind) noexcept {
            return kind >= NavigationSourceProducerKind::StaticCollider && kind < NavigationSourceProducerKind::Count;
        }

        [[nodiscard]] bool NearlyEqual(const float left, const float right) noexcept {
            return std::abs(left - right) <= TileBoundsEpsilon;
        }

        [[nodiscard]] bool IsNonDegenerateTriangle(const std::array<Math::Vec3, 3> &triangle) noexcept {
            if (!Math::IsFinite(triangle[0]) || !Math::IsFinite(triangle[1]) || !Math::IsFinite(triangle[2]))
                return false;
            const Math::Vec3 first = triangle[1] - triangle[0];
            const Math::Vec3 second = triangle[2] - triangle[0];
            const double crossX = (static_cast<double>(first.y) * second.z) - (static_cast<double>(first.z) * second.y);
            const double crossY = (static_cast<double>(first.z) * second.x) - (static_cast<double>(first.x) * second.z);
            const double crossZ = (static_cast<double>(first.x) * second.y) - (static_cast<double>(first.y) * second.x);
            const double areaSquared = (crossX * crossX) + (crossY * crossY) + (crossZ * crossZ);
            return std::isfinite(areaSquared) && areaSquared > DegenerateTriangleAreaSquared;
        }

        [[nodiscard]] bool IntersectsBounds(const std::array<Math::Vec3, 3> &triangle, const Math::Aabb &bounds) noexcept {
            Math::Vec3 minimum = triangle[0];
            Math::Vec3 maximum = triangle[0];
            for (std::size_t index = 1; index < triangle.size(); ++index) {
                minimum.x = std::min(minimum.x, triangle[index].x);
                minimum.y = std::min(minimum.y, triangle[index].y);
                minimum.z = std::min(minimum.z, triangle[index].z);
                maximum.x = std::max(maximum.x, triangle[index].x);
                maximum.y = std::max(maximum.y, triangle[index].y);
                maximum.z = std::max(maximum.z, triangle[index].z);
            }
            return minimum.x <= bounds.maximum.x && maximum.x >= bounds.minimum.x && minimum.y <= bounds.maximum.y &&
                   maximum.y >= bounds.minimum.y && minimum.z <= bounds.maximum.z && maximum.z >= bounds.minimum.z;
        }

        [[nodiscard]] bool IsValidTileBounds(const NavigationTileBuildRequest &request) noexcept {
            if (!request.bounds.IsValid() || !std::isfinite(request.tileSizeMeters) || request.tileSizeMeters <= 0.0F ||
                request.bounds.maximum.x <= request.bounds.minimum.x || request.bounds.maximum.z <= request.bounds.minimum.z)
                return false;
            const float expectedMinimumX = static_cast<float>(request.key.x) * request.tileSizeMeters;
            const float expectedMinimumZ = static_cast<float>(request.key.z) * request.tileSizeMeters;
            return std::isfinite(expectedMinimumX) && std::isfinite(expectedMinimumZ) &&
                   NearlyEqual(request.bounds.minimum.x, expectedMinimumX) &&
                   NearlyEqual(request.bounds.maximum.x, expectedMinimumX + request.tileSizeMeters) &&
                   NearlyEqual(request.bounds.minimum.z, expectedMinimumZ) &&
                   NearlyEqual(request.bounds.maximum.z, expectedMinimumZ + request.tileSizeMeters);
        }

        [[nodiscard]] bool IsValidLimits(const NavigationTileBuildLimits &limits) noexcept {
            return limits.maximumVertices > 0 && limits.maximumVertices <= NavigationTileBuildLimits::MaximumVertices &&
                   limits.maximumPolygons > 0 && limits.maximumPolygons <= NavigationTileBuildLimits::MaximumPolygons &&
                   limits.maximumOffMeshLinks > 0 && limits.maximumOffMeshLinks <= NavigationTileBuildLimits::MaximumOffMeshLinks &&
                   limits.maximumVerticesPerPolygon >= 3 &&
                   limits.maximumVerticesPerPolygon <= NavigationTileBuildLimits::MaximumVerticesPerPolygon &&
                   limits.maximumOwnedBytes > 0 && limits.maximumOwnedBytes <= NavigationTileBuildLimits::MaximumOwnedBytes &&
                   limits.maximumWorkUnits > 0 && limits.maximumWorkUnits <= NavigationTileBuildLimits::MaximumWorkUnits;
        }

        [[nodiscard]] auto TriangleSortKey(const NavigationTileBuildTriangle &triangle) {
            return std::tuple{triangle.provenance,    triangle.area.Value(),  triangle.materialSlot.value, triangle.vertices[0].x,
                              triangle.vertices[0].y, triangle.vertices[0].z, triangle.vertices[1].x,      triangle.vertices[1].y,
                              triangle.vertices[1].z, triangle.vertices[2].x, triangle.vertices[2].y,      triangle.vertices[2].z};
        }

        [[nodiscard]] Result<void> ConfigureGrid(RecastConfig &config, const NavigationTileBuildRequest &request) {
            config.value.cs = request.buildGeometry.cellSizeMeters;
            config.value.ch = request.buildGeometry.cellHeightMeters;
            config.value.bmin[0] = request.bounds.minimum.x;
            config.value.bmin[1] = request.bounds.minimum.y;
            config.value.bmin[2] = request.bounds.minimum.z;
            config.value.bmax[0] = request.bounds.maximum.x;
            config.value.bmax[1] = request.bounds.maximum.y;
            config.value.bmax[2] = request.bounds.maximum.z;
            rcCalcGridSize(config.value.bmin, config.value.bmax, config.value.cs, &config.value.width, &config.value.height);
            if (config.value.width <= 0 || config.value.height <= 0)
                return Failure<void>(NavigationErrors::BakeInputInvalid);
            config.value.tileSize = std::max(config.value.width, config.value.height);
            config.value.borderSize = 0;
            config.value.walkableSlopeAngle = request.buildGeometry.maxSlopeDegrees;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ConfigureAgent(RecastConfig &config, const NavigationTileBuildRequest &request) {
            const double walkableHeight = std::ceil(static_cast<double>(request.buildGeometry.heightMeters) /
                                                    static_cast<double>(request.buildGeometry.cellHeightMeters));
            const double walkableClimb = std::ceil(static_cast<double>(request.buildGeometry.stepHeightMeters) /
                                                   static_cast<double>(request.buildGeometry.cellHeightMeters));
            const double walkableRadius = std::ceil(static_cast<double>(request.buildGeometry.radiusMeters) /
                                                    static_cast<double>(request.buildGeometry.cellSizeMeters));
            if (!std::isfinite(walkableHeight) || !std::isfinite(walkableClimb) || !std::isfinite(walkableRadius) ||
                walkableHeight > std::numeric_limits<int>::max() || walkableClimb > 255.0 || walkableRadius >= 255.0)
                return Failure<void>(NavigationErrors::CapacityExceeded);
            config.value.walkableHeight = std::max(3, static_cast<int>(walkableHeight));
            config.value.walkableClimb = static_cast<int>(walkableClimb);
            config.value.walkableRadius = static_cast<int>(walkableRadius);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ConfigureRegions(RecastConfig &config, const NavigationTileBuildRequest &request) {
            const double maxEdgeLength = std::ceil(12.0 / static_cast<double>(config.value.cs));
            const double minRegionArea = std::ceil(
                std::pow(static_cast<double>(request.buildGeometry.minimumRegionSizeMeters) / static_cast<double>(config.value.cs), 2.0));
            if (!std::isfinite(maxEdgeLength) || !std::isfinite(minRegionArea) || maxEdgeLength > std::numeric_limits<int>::max() ||
                minRegionArea > std::numeric_limits<int>::max())
                return Failure<void>(NavigationErrors::CapacityExceeded);
            config.value.maxEdgeLen = std::max(1, static_cast<int>(maxEdgeLength));
            config.value.maxSimplificationError = config.value.cs * 1.3F;
            config.value.minRegionArea = static_cast<int>(minRegionArea);
            config.value.mergeRegionArea = config.value.minRegionArea > (std::numeric_limits<int>::max() / 2)
                                               ? std::numeric_limits<int>::max()
                                               : config.value.minRegionArea * 2;
            config.value.maxVertsPerPoly = static_cast<int>(request.limits.maximumVerticesPerPolygon);
            config.value.detailSampleDist = 0.0F;
            config.value.detailSampleMaxError = 0.0F;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> EstimateBudget(RecastConfig &config, const NavigationTileBuildRequest &request) {
            std::uint64_t gridCells{};
            std::uint64_t triangleWork{};
            if (std::uint64_t modifierWork{};
                !TryMultiply(static_cast<std::uint64_t>(config.value.width), static_cast<std::uint64_t>(config.value.height), gridCells) ||
                !TryMultiply(request.triangles.size(), 64U, triangleWork) || !TryMultiply(request.modifiers.size(), 128U, modifierWork) ||
                !TryAdd(gridCells, triangleWork, config.workUnits) || !TryAdd(config.workUnits, modifierWork, config.workUnits) ||
                config.workUnits > request.limits.maximumWorkUnits)
                return Failure<void>(NavigationErrors::CapacityExceeded);

            std::uint64_t gridBytes{};
            if (std::uint64_t triangleBytes{}; !TryMultiply(gridCells, sizeof(rcCompactCell) + sizeof(rcSpan *) + 128U, gridBytes) ||
                                               !TryMultiply(request.triangles.size(), 256U, triangleBytes) ||
                                               !TryAdd(gridBytes, triangleBytes, config.estimatedBytes) ||
                                               config.estimatedBytes > request.limits.maximumOwnedBytes)
                return Failure<void>(NavigationErrors::CapacityExceeded);
            return Result<void>::Success();
        }
    }  // namespace

    /** @brief Finds the deterministic Recast code assigned to one semantic navigation area. */
    const AreaCode *FindAreaCode(const std::vector<AreaCode> &areas, const NavigationAreaId id) noexcept {
        const auto found = std::ranges::lower_bound(areas, id.Value(), {}, [](const AreaCode &area) {
            return area.id.Value();
        });
        return found == areas.end() || found->id != id ? nullptr : std::to_address(found);
    }

    /** @copydoc ValidateRequest */
    Result<void> ValidateRequest(const NavigationTileBuildRequest &request) {
        if (!IsValidTileBounds(request) || !IsValidLimits(request.limits) ||
            ValidateNavigationAgentBuildGeometry(request.buildGeometry).HasError())
            return Failure<void>(NavigationErrors::BakeInputInvalid);

        for (const NavigationTileBuildTriangle &triangle : request.triangles) {
            if (!triangle.area.IsValid() || !IsNonDegenerateTriangle(triangle.vertices) || !triangle.provenance.producer.IsValid() ||
                !triangle.provenance.contribution.IsValid() || !triangle.provenance.revision.IsValid() ||
                !IsKnown(triangle.provenance.kind))
                return Failure<void>(NavigationErrors::BakeInputInvalid);
        }
        for (const NavigationTileBuildModifier &modifier : request.modifiers) {
            const auto dimensions = modifier.canonicalBounds.maximum - modifier.canonicalBounds.minimum;
            if (!modifier.id.IsValid() || !modifier.surface.IsValid() || !modifier.profile.IsValid() || !modifier.area.IsValid() ||
                !IsKnown(modifier.mode) || !modifier.canonicalBounds.IsValid() || dimensions.x <= 0.0F || dimensions.y <= 0.0F ||
                dimensions.z <= 0.0F)
                return Failure<void>(NavigationErrors::BakeInputInvalid);
        }
        return Result<void>::Success();
    }

    /** @copydoc PrepareTriangles */
    Result<PreparedTriangles> PrepareTriangles(const NavigationTileBuildRequest &request) {
        std::vector<const NavigationTileBuildTriangle *> ordered;
        ordered.reserve(request.triangles.size());
        for (const NavigationTileBuildTriangle &triangle : request.triangles)
            ordered.push_back(&triangle);
        std::ranges::sort(ordered, [](const auto *left, const auto *right) {
            return TriangleSortKey(*left) < TriangleSortKey(*right);
        });

        std::vector<NavigationAreaId> areaIds;
        areaIds.reserve(ordered.size() + request.modifiers.size());
        for (const auto *triangle : ordered)
            areaIds.push_back(triangle->area);
        for (const NavigationTileBuildModifier &modifier : request.modifiers)
            areaIds.push_back(modifier.area);
        std::ranges::sort(areaIds);
        areaIds.erase(std::ranges::unique(areaIds).begin(), areaIds.end());
        if (areaIds.empty())
            return Result<PreparedTriangles>::Success(PreparedTriangles{});
        if (areaIds.size() > static_cast<std::size_t>(LastRecastSemanticArea - FirstRecastArea + 1U))
            return Failure<PreparedTriangles>(NavigationErrors::CapacityExceeded);

        PreparedTriangles prepared;
        prepared.areaCodes.reserve(areaIds.size());
        for (std::size_t index = 0; index < areaIds.size(); ++index)
            prepared.areaCodes.push_back({.id = areaIds[index], .code = static_cast<unsigned char>(FirstRecastArea + index)});

        prepared.vertices.reserve(ordered.size() * 9U);
        prepared.indices.reserve(ordered.size() * 3U);
        prepared.areas.reserve(ordered.size());
        prepared.sources.reserve(ordered.size());
        for (const auto *triangle : ordered) {
            if (!IntersectsBounds(triangle->vertices, request.bounds))
                continue;
            const AreaCode *area = FindAreaCode(prepared.areaCodes, triangle->area);
            if (area == nullptr)
                return Failure<PreparedTriangles>(NavigationErrors::ProviderFailed);
            const auto firstIndex = static_cast<int>(prepared.vertices.size() / 3U);
            for (const Math::Vec3 vertex : triangle->vertices)
                prepared.vertices.insert(prepared.vertices.end(), {vertex.x, vertex.y, vertex.z});
            prepared.indices.insert(prepared.indices.end(), {firstIndex, firstIndex + 1, firstIndex + 2});
            prepared.areas.push_back(area->code);
            prepared.sources.push_back(triangle);
        }
        return Result<PreparedTriangles>::Success(std::move(prepared));
    }

    /** @copydoc MakeConfig */
    Result<RecastConfig> MakeConfig(const NavigationTileBuildRequest &request) {
        RecastConfig config;
        if (const auto grid = ConfigureGrid(config, request); grid.HasError())
            return Result<RecastConfig>::Failure(grid.ErrorValue());
        if (const auto agent = ConfigureAgent(config, request); agent.HasError())
            return Result<RecastConfig>::Failure(agent.ErrorValue());
        if (const auto regions = ConfigureRegions(config, request); regions.HasError())
            return Result<RecastConfig>::Failure(regions.ErrorValue());
        if (const auto budget = EstimateBudget(config, request); budget.HasError())
            return Result<RecastConfig>::Failure(budget.ErrorValue());
        return Result<RecastConfig>::Success(std::move(config));
    }
}  // namespace Horo::Navigation::RecastDetourMeshBuilderInternal
