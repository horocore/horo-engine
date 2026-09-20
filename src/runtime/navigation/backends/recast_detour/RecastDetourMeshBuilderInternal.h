#pragma once

#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Navigation/NavigationMeshBuilder.h"

#include <Recast.h>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace Horo::Navigation::RecastDetourMeshBuilderInternal {
    inline constexpr unsigned char FirstRecastArea = 1U;
    inline constexpr unsigned char LastRecastSemanticArea = RC_WALKABLE_AREA - 1U;
    inline constexpr double DegenerateTriangleAreaSquared = 1.0e-12;
    inline constexpr float TileBoundsEpsilon = 1.0e-3F;

    template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
        return Result<T>::Failure(MakeError(descriptor));
    }

    [[nodiscard]] inline bool TryAdd(const std::uint64_t left, const std::uint64_t right, std::uint64_t &result) noexcept {
        if (right > std::numeric_limits<std::uint64_t>::max() - left)
            return false;
        result = left + right;
        return true;
    }

    [[nodiscard]] inline bool TryMultiply(const std::uint64_t left, const std::uint64_t right, std::uint64_t &result) noexcept {
        if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
            return false;
        result = left * right;
        return true;
    }

    struct AreaCode final {
        NavigationAreaId id;
        unsigned char code{};
    };

    [[nodiscard]] const AreaCode *FindAreaCode(const std::vector<AreaCode> &areas, NavigationAreaId id) noexcept;

    struct PreparedTriangles final {
        std::vector<float> vertices;
        std::vector<int> indices;
        std::vector<unsigned char> areas;
        std::vector<const NavigationTileBuildTriangle *> sources;
        std::vector<AreaCode> areaCodes;
    };

    struct RecastConfig final {
        rcConfig value{};
        std::uint64_t workUnits{};
        std::uint64_t estimatedBytes{};
    };

    struct PolyMeshDeleter final {
        void operator()(rcPolyMesh *value) const noexcept {
            rcFreePolyMesh(value);
        }
    };

    using PolyMeshPtr = std::unique_ptr<rcPolyMesh, PolyMeshDeleter>;

    struct RecastPipelineResult final {
        PolyMeshPtr mesh;
        std::uint64_t walkableTriangleCount{};
        std::uint64_t discardedTriangleCount{};
        std::uint64_t rasterizedSpanCount{};
        std::uint32_t regionCount{};
        std::uint32_t discardedRegionSpanCount{};
        std::uint32_t contourCount{};
        std::uint32_t providerWarnings{};
        bool empty{};
    };

    [[nodiscard]] Result<void> ValidateRequest(const NavigationTileBuildRequest &request);

    [[nodiscard]] Result<PreparedTriangles> PrepareTriangles(const NavigationTileBuildRequest &request);

    [[nodiscard]] Result<RecastConfig> MakeConfig(const NavigationTileBuildRequest &request);

    [[nodiscard]] Result<RecastPipelineResult> RunRecastPipeline(const NavigationTileBuildRequest &request,
                                                                 const PreparedTriangles &prepared, const RecastConfig &config,
                                                                 const CancellationToken &cancellation);

    [[nodiscard]] Result<NavigationTileBuildResult> TranslateResult(const NavigationTileBuildRequest &request,
                                                                    const PreparedTriangles &prepared, RecastPipelineResult pipeline);
}  // namespace Horo::Navigation::RecastDetourMeshBuilderInternal
