#include "Horo/Navigation/NavigationLinkValidation.h"

#include <algorithm>
#include <bit>
#include <tuple>
#include <utility>

namespace Horo::Navigation {
    namespace {
        /** @brief Computes finite edge midpoints without overflowing float addition. */
        [[nodiscard]] Math::Vec3 Midpoint(const Math::Vec3 left, const Math::Vec3 right) noexcept {
            return {static_cast<float>((static_cast<double>(left.x) + right.x) * 0.5),
                    static_cast<float>((static_cast<double>(left.y) + right.y) * 0.5),
                    static_cast<float>((static_cast<double>(left.z) + right.z) * 0.5)};
        }

        /** @brief Captures one tile's canonical polygon boundaries with transactional output and work ceilings. */
        [[nodiscard]] Result<void> CaptureTile(const NavMeshTileView &tile, const NavigationLinkGenerationSurfaceBinding &binding,
                                               const float radius, const std::uint32_t maximumAnchors, std::uint64_t &remainingWork,
                                               const CancellationToken &cancellation,
                                               std::vector<NavigationLinkGenerationAnchor> &anchors) {
            const auto &descriptor = *tile.descriptor;
            const std::size_t firstPolygon = binding.polygons.first - descriptor.polygons.first;
            const std::size_t endPolygon = firstPolygon + binding.polygons.count;
            for (std::size_t polygonIndex = firstPolygon; polygonIndex < endPolygon; ++polygonIndex) {
                const auto &polygon = tile.tables.polygons[polygonIndex];
                const auto firstIndex = polygon.vertexIndices.first - descriptor.polygonVertexIndices.first;
                for (std::uint32_t edge = 0; edge < polygon.vertexIndices.count; ++edge) {
                    if (cancellation.IsCancellationRequested())
                        return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
                    if (remainingWork == 0)
                        return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCapacityExceeded));
                    --remainingWork;
                    if (polygon.adjacencies.count > 0 &&
                        tile.tables.polygonAdjacencies[polygon.adjacencies.first - descriptor.polygonAdjacencies.first + edge] !=
                            NavMeshBoundaryAdjacency)
                        continue;
                    if (anchors.size() >= maximumAnchors)
                        return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCapacityExceeded));
                    const auto start = tile.tables.polygonVertexIndices[firstIndex + edge] - descriptor.vertices.first;
                    const auto end =
                        tile.tables.polygonVertexIndices[firstIndex + (edge + 1) % polygon.vertexIndices.count] - descriptor.vertices.first;
                    const std::uint64_t identity = ((descriptor.polygons.first + polygonIndex) << 8U) + edge + 1;
                    anchors.push_back({.id = NavigationLinkAnchorId::Create(identity).Value(),
                                       .endpoint = {.surface = binding.surface,
                                                    .position = Midpoint(tile.tables.vertices[start], tile.tables.vertices[end]),
                                                    .connectionRadiusMeters = radius}});
                }
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CaptureNavigationLinkBoundaryAnchors */
    Result<std::vector<NavigationLinkGenerationAnchor>> CaptureNavigationLinkBoundaryAnchors(
        const NavMeshData &mesh, const std::span<const NavigationLinkGenerationSurfaceBinding> bindings, const std::uint32_t maximumAnchors,
        const std::uint64_t maximumWorkUnits, const CancellationToken &cancellation) {
        using Output = std::vector<NavigationLinkGenerationAnchor>;
        if (maximumAnchors == 0 || maximumAnchors > NavigationLinkValidationLimits::MaximumAnchors || maximumWorkUnits == 0 ||
            maximumWorkUnits > NavigationLinkValidationLimits::MaximumWorkUnits)
            return Result<Output>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        if (cancellation.IsCancellationRequested())
            return Result<Output>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
        if (bindings.size() > maximumAnchors)
            return Result<Output>::Failure(MakeError(NavigationErrors::BakeInputCapacityExceeded));
        const auto sortingWork =
            bindings.size() * (2 + std::bit_width(bindings.size())) + maximumAnchors * (2 + std::bit_width(maximumAnchors));
        if (sortingWork > maximumWorkUnits)
            return Result<Output>::Failure(MakeError(NavigationErrors::BakeInputCapacityExceeded));
        auto remainingWork = maximumWorkUnits - sortingWork;
        std::vector<NavigationLinkGenerationSurfaceBinding> ordered{bindings.begin(), bindings.end()};
        std::ranges::sort(ordered, {}, [](const auto &binding) {
            return std::tuple{binding.tile, binding.polygons.first};
        });
        if (std::ranges::adjacent_find(ordered, [](const auto &left, const auto &right) {
            return left.tile == right.tile && static_cast<std::uint64_t>(left.polygons.first) + left.polygons.count > right.polygons.first;
        }) != ordered.end())
            return Result<Output>::Failure(MakeError(NavigationErrors::DescriptorConflict));
        Output anchors;
        anchors.reserve(maximumAnchors);
        for (const auto &binding : ordered) {
            if (!binding.surface.IsValid())
                return Result<Output>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            const auto tile = mesh.ResolveTile(binding.tile);
            if (tile.HasError())
                return Result<Output>::Failure(tile.ErrorValue());
            if (const auto range = tile.Value().descriptor->polygons;
                binding.polygons.count == 0 || binding.polygons.first < range.first ||
                static_cast<std::uint64_t>(binding.polygons.first) + binding.polygons.count >
                    static_cast<std::uint64_t>(range.first) + range.count)
                return Result<Output>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            const auto captured = CaptureTile(tile.Value(), binding, mesh.Header().profile.buildGeometry.radiusMeters, maximumAnchors,
                                              remainingWork, cancellation, anchors);
            if (captured.HasError())
                return Result<Output>::Failure(captured.ErrorValue());
        }
        std::ranges::sort(anchors, {}, &NavigationLinkGenerationAnchor::id);
        if (cancellation.IsCancellationRequested())
            return Result<Output>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
        return Result<Output>::Success(std::move(anchors));
    }
}  // namespace Horo::Navigation
