#include "Horo/Navigation/NavigationErrors.h"
#include "runtime/navigation/backends/recast_detour/RecastDetourProviderInternal.h"

#include <algorithm>
#include <new>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        using Detail::Failure;
    }

    Result<NavigationSamplePositionResult> BuildRecastDetourSamplePositionResult(std::vector<NavigationSurfaceHit> hits,
                                                                                 const std::uint32_t maximumResultPoints) {
        try {
            NavigationSamplePositionResult result;
            result.truncated = hits.size() > maximumResultPoints;
            const auto outputCount = std::min<std::size_t>(hits.size(), maximumResultPoints);
            result.samples.reserve(outputCount);
            for (std::size_t index = 0; index < outputCount; ++index)
                result.samples.push_back(std::move(hits[index]));
            return Result<NavigationSamplePositionResult>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Failure<NavigationSamplePositionResult>(NavigationErrors::CapacityExceeded);
        }
    }

    Result<NavigationPolygonQueryResult> BuildRecastDetourPolygonQueryResult(std::vector<NavigationSurfaceHit> hits,
                                                                             const std::uint32_t maximumResultPoints) {
        try {
            NavigationPolygonQueryResult result;
            result.truncated = hits.size() > maximumResultPoints;
            const auto outputCount = std::min<std::size_t>(hits.size(), maximumResultPoints);
            result.polygons.reserve(outputCount);
            for (std::size_t index = 0; index < outputCount; ++index)
                result.polygons.push_back(std::move(hits[index]));
            return Result<NavigationPolygonQueryResult>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Failure<NavigationPolygonQueryResult>(NavigationErrors::CapacityExceeded);
        }
    }
}  // namespace Horo::Navigation
