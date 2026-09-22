#include "runtime/navigation/backends/recast_detour/RecastDetourProviderQueriesInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Horo::Navigation::RecastDetourQueries {
    namespace {
        using Detail::Failure;
    }

    /** @copydoc ResolveEndpoint */
    Result<QueryEndpoint> ResolveEndpoint(const QuerySlot &slot, const Math::Vec3 point, const Math::Vec3 halfExtents,
                                          const NavigationPathRequest &request, const NavigationAreaRegistry &areaRegistry,
                                          const std::vector<GroundedNavigationPolygon> &polygons,
                                          const std::vector<dtPolyRef> &polygonReferences) {
        QueryEndpoint endpoint;
        double bestDistance = std::numeric_limits<double>::infinity();
        for (std::size_t index = 0; index < polygons.size(); ++index) {
            const auto traversal = areaRegistry.ResolveTraversal(request.filter, polygons[index].area);
            if (traversal.HasError())
                return Result<QueryEndpoint>::Failure(traversal.ErrorValue());
            if (!traversal.Value().traversable)
                continue;

            const std::array<float, 3> source{point.x, point.y, point.z};
            std::array<float, 3> projected{};
            bool pointOverPolygon{};
            if (dtStatusFailed(
                    slot.query->closestPointOnPoly(polygonReferences[index], source.data(), projected.data(), &pointOverPolygon)))
                return Failure<QueryEndpoint>(NavigationErrors::ProviderFailed);
            static_cast<void>(pointOverPolygon);
            if (!std::ranges::all_of(projected, [](const float value) {
                return std::isfinite(value);
            }))
                return Failure<QueryEndpoint>(NavigationErrors::ProviderFailed);
            if (std::abs(static_cast<double>(projected[0]) - point.x) > halfExtents.x ||
                std::abs(static_cast<double>(projected[1]) - point.y) > halfExtents.y ||
                std::abs(static_cast<double>(projected[2]) - point.z) > halfExtents.z)
                continue;
            const double distance = std::hypot(static_cast<double>(projected[0]) - point.x, static_cast<double>(projected[1]) - point.y,
                                               static_cast<double>(projected[2]) - point.z);
            if (!std::isfinite(distance) ||
                (endpoint.found && (distance > bestDistance || (distance == bestDistance && index >= endpoint.polygon))))
                continue;
            endpoint.found = true;
            endpoint.polygon = static_cast<std::uint32_t>(index);
            endpoint.projected = {projected[0], projected[1], projected[2]};
            bestDistance = distance;
        }
        return Result<QueryEndpoint>::Success(endpoint);
    }
}  // namespace Horo::Navigation::RecastDetourQueries
