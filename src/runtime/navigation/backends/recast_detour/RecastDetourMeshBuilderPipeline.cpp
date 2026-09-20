#include "RecastDetourMeshBuilderInternal.h"

#include <Recast.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Horo::Navigation::RecastDetourMeshBuilderInternal {
    namespace {
        struct BuildContext final : rcContext {
            BuildContext() : rcContext(true) {
                enableTimer(false);
            }

            [[nodiscard]] std::uint32_t WarningCount() const noexcept {
                return warnings_;
            }

            [[nodiscard]] std::uint32_t ErrorCount() const noexcept {
                return errors_;
            }

        protected:
            void doLog(const rcLogCategory category, const char *, const int) override {
                if (category == RC_LOG_WARNING)
                    ++warnings_;
                else if (category == RC_LOG_ERROR)
                    ++errors_;
            }

        private:
            std::atomic<std::uint32_t> warnings_{};
            std::atomic<std::uint32_t> errors_{};
        };

        struct HeightfieldDeleter final {
            void operator()(rcHeightfield *value) const noexcept {
                rcFreeHeightField(value);
            }
        };

        struct CompactHeightfieldDeleter final {
            void operator()(rcCompactHeightfield *value) const noexcept {
                rcFreeCompactHeightfield(value);
            }
        };

        struct ContourSetDeleter final {
            void operator()(rcContourSet *value) const noexcept {
                rcFreeContourSet(value);
            }
        };

        using HeightfieldPtr = std::unique_ptr<rcHeightfield, HeightfieldDeleter>;
        using CompactHeightfieldPtr = std::unique_ptr<rcCompactHeightfield, CompactHeightfieldDeleter>;
        using ContourSetPtr = std::unique_ptr<rcContourSet, ContourSetDeleter>;

        struct RasterizedHeightfield final {
            HeightfieldPtr value;
            std::uint64_t walkableTriangleCount{};
            std::uint64_t discardedTriangleCount{};
            std::uint64_t rasterizedSpanCount{};
        };

        struct CompactBuild final {
            CompactHeightfieldPtr value;
            bool empty{};
        };

        struct RegionBuild final {
            CompactHeightfieldPtr value;
            std::uint32_t regionCount{};
            std::uint32_t discardedRegionSpanCount{};
            bool empty{};
        };

        struct ContourBuild final {
            PolyMeshPtr mesh;
            std::uint32_t contourCount{};
            bool empty{};
        };

        [[nodiscard]] Result<RasterizedHeightfield> Rasterize(const PreparedTriangles &prepared, const RecastConfig &config,
                                                              BuildContext &context, const CancellationToken &cancellation) {
            HeightfieldPtr heightfield{rcAllocHeightfield()};
            if (!heightfield || cancellation.IsCancellationRequested())
                return cancellation.IsCancellationRequested() ? Failure<RasterizedHeightfield>(NavigationErrors::BakeInputCancelled)
                                                              : Failure<RasterizedHeightfield>(NavigationErrors::CapacityExceeded);
            if (!rcCreateHeightfield(&context, *heightfield, config.value.width, config.value.height, config.value.bmin, config.value.bmax,
                                     config.value.cs, config.value.ch))
                return Failure<RasterizedHeightfield>(NavigationErrors::ProviderFailed);

            std::vector<unsigned char> walkableAreas = prepared.areas;
            RasterizedHeightfield output{.value = std::move(heightfield)};
            rcMarkWalkableTriangles(&context, config.value.walkableSlopeAngle, prepared.vertices.data(),
                                    static_cast<int>(prepared.vertices.size() / 3U), prepared.indices.data(),
                                    static_cast<int>(walkableAreas.size()), walkableAreas.data());
            for (std::size_t index = 0; index < walkableAreas.size(); ++index) {
                if (walkableAreas[index] == RC_WALKABLE_AREA) {
                    walkableAreas[index] = prepared.areas[index];
                    ++output.walkableTriangleCount;
                } else {
                    walkableAreas[index] = RC_NULL_AREA;
                    ++output.discardedTriangleCount;
                }
            }
            if (!rcRasterizeTriangles(&context, prepared.vertices.data(), static_cast<int>(prepared.vertices.size() / 3U),
                                      prepared.indices.data(), walkableAreas.data(), static_cast<int>(walkableAreas.size()), *output.value))
                return Failure<RasterizedHeightfield>(NavigationErrors::ProviderFailed);
            output.rasterizedSpanCount = static_cast<std::uint64_t>(std::max(0, rcGetHeightFieldSpanCount(&context, *output.value)));
            if (cancellation.IsCancellationRequested())
                return Failure<RasterizedHeightfield>(NavigationErrors::BakeInputCancelled);
            return Result<RasterizedHeightfield>::Success(std::move(output));
        }

        [[nodiscard]] Result<CompactBuild> CompactAndFilter(const NavigationTileBuildRequest &request, const PreparedTriangles &prepared,
                                                            const RecastConfig &config, BuildContext &context, HeightfieldPtr heightfield,
                                                            const CancellationToken &cancellation) {
            rcFilterLowHangingWalkableObstacles(&context, config.value.walkableClimb, *heightfield);
            rcFilterLedgeSpans(&context, config.value.walkableHeight, config.value.walkableClimb, *heightfield);
            rcFilterWalkableLowHeightSpans(&context, config.value.walkableHeight, *heightfield);
            if (cancellation.IsCancellationRequested())
                return Failure<CompactBuild>(NavigationErrors::BakeInputCancelled);

            CompactHeightfieldPtr compact{rcAllocCompactHeightfield()};
            if (!compact)
                return Failure<CompactBuild>(NavigationErrors::CapacityExceeded);
            if (!rcBuildCompactHeightfield(&context, config.value.walkableHeight, config.value.walkableClimb, *heightfield, *compact))
                return Failure<CompactBuild>(NavigationErrors::ProviderFailed);
            CompactBuild output{.value = std::move(compact)};
            if (output.value->spanCount == 0) {
                output.empty = true;
                return Result<CompactBuild>::Success(std::move(output));
            }

            if (config.value.walkableRadius > 0 && !rcErodeWalkableArea(&context, config.value.walkableRadius, *output.value))
                return Failure<CompactBuild>(NavigationErrors::ProviderFailed);
            for (const NavigationTileBuildModifier &modifier : request.modifiers) {
                const AreaCode *area = FindAreaCode(prepared.areaCodes, modifier.area);
                if (area == nullptr)
                    return Failure<CompactBuild>(NavigationErrors::ProviderFailed);
                const std::array<float, 3> minimum{modifier.canonicalBounds.minimum.x, modifier.canonicalBounds.minimum.y,
                                                   modifier.canonicalBounds.minimum.z};
                const std::array<float, 3> maximum{modifier.canonicalBounds.maximum.x, modifier.canonicalBounds.maximum.y,
                                                   modifier.canonicalBounds.maximum.z};
                rcMarkBoxArea(&context, minimum.data(), maximum.data(),
                              modifier.mode == NavigationBakeModifierMode::Exclude ? RC_NULL_AREA : area->code, *output.value);
            }
            if (cancellation.IsCancellationRequested())
                return Failure<CompactBuild>(NavigationErrors::BakeInputCancelled);
            return Result<CompactBuild>::Success(std::move(output));
        }

        [[nodiscard]] std::uint32_t CountRegions(const rcCompactHeightfield &heightfield) {
            std::vector<bool> seen(static_cast<std::size_t>(heightfield.maxRegions) + 1U, false);
            std::uint32_t count{};
            for (int index = 0; index < heightfield.spanCount; ++index) {
                const auto region = static_cast<unsigned short>(heightfield.spans[index].reg & RC_CONTOUR_REG_MASK);
                if (region == 0 || region >= seen.size() || seen[region])
                    continue;
                seen[region] = true;
                ++count;
            }
            return count;
        }

        [[nodiscard]] Result<RegionBuild> BuildRegions(const RecastConfig &config, BuildContext &context, CompactHeightfieldPtr compact,
                                                       const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Failure<RegionBuild>(NavigationErrors::BakeInputCancelled);
            if (!rcBuildDistanceField(&context, *compact) ||
                !rcBuildRegions(&context, *compact, config.value.borderSize, config.value.minRegionArea, config.value.mergeRegionArea))
                return Failure<RegionBuild>(NavigationErrors::ProviderFailed);

            RegionBuild output{.value = std::move(compact)};
            for (int index = 0; index < output.value->spanCount; ++index) {
                if (output.value->areas[index] != RC_NULL_AREA && (output.value->spans[index].reg & RC_CONTOUR_REG_MASK) == 0)
                    ++output.discardedRegionSpanCount;
            }
            output.regionCount = CountRegions(*output.value);
            output.empty = output.regionCount == 0;
            return Result<RegionBuild>::Success(std::move(output));
        }

        [[nodiscard]] Result<ContourBuild> BuildContoursAndMesh(const RecastConfig &config, BuildContext &context,
                                                                CompactHeightfieldPtr compact) {
            ContourSetPtr contours{rcAllocContourSet()};
            if (!contours)
                return Failure<ContourBuild>(NavigationErrors::CapacityExceeded);
            if (!rcBuildContours(&context, *compact, config.value.maxSimplificationError, config.value.maxEdgeLen, *contours))
                return Failure<ContourBuild>(NavigationErrors::ProviderFailed);

            ContourBuild output{.contourCount = static_cast<std::uint32_t>(std::max(0, contours->nconts))};
            if (contours->nconts == 0) {
                output.empty = true;
                return Result<ContourBuild>::Success(std::move(output));
            }
            PolyMeshPtr mesh{rcAllocPolyMesh()};
            if (!mesh)
                return Failure<ContourBuild>(NavigationErrors::CapacityExceeded);
            if (!rcBuildPolyMesh(&context, *contours, config.value.maxVertsPerPoly, *mesh))
                return Failure<ContourBuild>(NavigationErrors::ProviderFailed);
            output.empty = mesh->npolys == 0;
            output.mesh = std::move(mesh);
            return Result<ContourBuild>::Success(std::move(output));
        }
    }  // namespace

    /** @copydoc RunRecastPipeline */
    Result<RecastPipelineResult> RunRecastPipeline(const NavigationTileBuildRequest &request, const PreparedTriangles &prepared,
                                                   const RecastConfig &config, const CancellationToken &cancellation) {
        RecastPipelineResult output;
        if (prepared.vertices.empty()) {
            output.empty = true;
            return Result<RecastPipelineResult>::Success(std::move(output));
        }

        BuildContext context;
        auto rasterized = Rasterize(prepared, config, context, cancellation);
        if (rasterized.HasError())
            return Result<RecastPipelineResult>::Failure(rasterized.ErrorValue());
        auto rasterizedValue = std::move(rasterized).Value();
        output.walkableTriangleCount = rasterizedValue.walkableTriangleCount;
        output.discardedTriangleCount = rasterizedValue.discardedTriangleCount;
        output.rasterizedSpanCount = rasterizedValue.rasterizedSpanCount;

        auto compact = CompactAndFilter(request, prepared, config, context, std::move(rasterizedValue.value), cancellation);
        if (compact.HasError())
            return Result<RecastPipelineResult>::Failure(compact.ErrorValue());
        auto compactValue = std::move(compact).Value();
        if (compactValue.empty) {
            output.empty = true;
            output.providerWarnings = context.WarningCount() + context.ErrorCount();
            return Result<RecastPipelineResult>::Success(std::move(output));
        }

        auto regions = BuildRegions(config, context, std::move(compactValue.value), cancellation);
        if (regions.HasError())
            return Result<RecastPipelineResult>::Failure(regions.ErrorValue());
        auto regionsValue = std::move(regions).Value();
        output.regionCount = regionsValue.regionCount;
        output.discardedRegionSpanCount = regionsValue.discardedRegionSpanCount;
        if (regionsValue.empty) {
            output.empty = true;
            output.providerWarnings = context.WarningCount() + context.ErrorCount();
            return Result<RecastPipelineResult>::Success(std::move(output));
        }

        auto mesh = BuildContoursAndMesh(config, context, std::move(regionsValue.value));
        if (mesh.HasError())
            return Result<RecastPipelineResult>::Failure(mesh.ErrorValue());
        auto meshValue = std::move(mesh).Value();
        output.contourCount = meshValue.contourCount;
        output.empty = meshValue.empty;
        output.mesh = std::move(meshValue.mesh);
        output.providerWarnings = context.WarningCount() + context.ErrorCount();
        return Result<RecastPipelineResult>::Success(std::move(output));
    }
}  // namespace Horo::Navigation::RecastDetourMeshBuilderInternal
