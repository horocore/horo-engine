#include "NavigationProjectBakeInputInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Application::NavigationBakeDetail {
    namespace {
        namespace Nav = Navigation;
        namespace Errors = Nav::NavigationErrors;

        struct GridRange final {
            std::int32_t first{};
            std::int32_t last{};
            std::uint64_t count{};
        };

        struct Grid final {
            Nav::NavigationBakeTileKey key;
            GridRange x;
            GridRange z;
            float size{};
            float minimumY{};
            float maximumY{};
        };

        /** @brief Computes an inclusive grid without unsigned narrowing or an extra tile at an exact upper edge. */
        Result<GridRange> Range(float minimum, float maximum, float size) {
            const double first = std::floor(static_cast<double>(minimum) / size);
            const double last = std::max(first, std::ceil(static_cast<double>(maximum) / size) - 1);
            if (!std::isfinite(first) || !std::isfinite(last) || first < INT32_MIN || last > INT32_MAX)
                return Result<GridRange>::Failure(MakeError(Errors::CapacityExceeded, "Navigation grid exceeds portable tile coordinates"));
            const auto a = static_cast<std::int32_t>(first);
            const auto b = static_cast<std::int32_t>(last);
            const float lower = static_cast<float>(a) * size;
            if (const float upper = static_cast<float>(b) * size + size;
                !std::isfinite(lower) || !std::isfinite(upper) || lower > minimum || upper < maximum)
                return Result<GridRange>::Failure(
                    MakeError(Errors::BakeInputInvalid, "Navigation grid cannot exactly cover source bounds"));
            return Result<GridRange>::Success({a, b, static_cast<std::uint64_t>(static_cast<std::int64_t>(b) - a) + 1});
        }

        /** @brief Resolves exact profile geometry and full authored/source bounds before allocating any tile rows. */
        Result<Grid> Plan(const Nav::NavigationTileBuildPartition &partition, const Nav::NavigationBakeInputSnapshot &input,
                          const Nav::NavigationDefinition &definition, const ProjectBakeSurfaces &surfaces) {
            const auto surface = surfaces.find({partition.surface, partition.scope});
            const auto profile = std::ranges::find(input.Profiles(), partition.profile, &Nav::NavigationResolvedBakeProfile::id);
            if (surface == surfaces.end() || !surface->second.bounds || profile == input.Profiles().end())
                return Result<Grid>::Failure(
                    MakeError(Errors::BakeInputReferenceMissing, "Navigation partition has no complete source bounds"));
            const auto &geometry = profile->buildGeometry;
            const float size = static_cast<float>(definition.Tiles().tileSizeCells) * geometry.cellSizeMeters;
            const double cells = static_cast<double>(size) / geometry.cellSizeMeters;
            if (const double border = std::ceil(static_cast<double>(geometry.radiusMeters) / geometry.cellSizeMeters) + 3;
                !std::isfinite(size) || size <= 0 || !std::isfinite(cells) || std::abs(cells - std::round(cells)) > 1.0e-4 ||
                !std::isfinite(border) || border >= 255)
                return Result<Grid>::Failure(
                    MakeError(Errors::BakeInputInvalid, "Navigation tile extent is not a representable integral voxel grid"));
            const auto &bounds = *surface->second.bounds;
            auto x = Range(bounds.minimum.x, bounds.maximum.x, size);
            auto z = Range(bounds.minimum.z, bounds.maximum.z, size);
            if (x.HasError())
                return Result<Grid>::Failure(x.ErrorValue());
            if (z.HasError())
                return Result<Grid>::Failure(z.ErrorValue());
            const float minimumY = bounds.minimum.y - geometry.cellHeightMeters;
            const float maximumY = bounds.maximum.y + geometry.heightMeters + geometry.stepHeightMeters + geometry.cellHeightMeters;
            if (!std::isfinite(minimumY) || !std::isfinite(maximumY) || minimumY >= bounds.minimum.y || maximumY <= bounds.maximum.y)
                return Result<Grid>::Failure(MakeError(Errors::BakeInputInvalid, "Navigation vertical clearance is not representable"));
            return Result<Grid>::Success(
                {{partition.profile, partition.surface, {}, partition.scope}, x.Value(), z.Value(), size, minimumY, maximumY});
        }

        /** @brief Preserves the exact float-grid spelling required by the canonical dependency and builder boundary. */
        Result<Nav::NavigationBakeTile> Tile(const Grid &grid, std::int32_t x, std::int32_t z) {
            auto key = grid.key;
            key.tile = {.x = x, .z = z};
            const float minimumX = static_cast<float>(x) * grid.size;
            const float minimumZ = static_cast<float>(z) * grid.size;
            const float maximumX = minimumX + grid.size;
            const float maximumZ = minimumZ + grid.size;
            const Math::Aabb bounds{{minimumX, grid.minimumY, minimumZ}, {maximumX, grid.maximumY, maximumZ}};
            const bool joinedX = x == grid.x.last || maximumX == static_cast<float>(static_cast<std::int64_t>(x) + 1) * grid.size;
            if (const bool joinedZ = z == grid.z.last || maximumZ == static_cast<float>(static_cast<std::int64_t>(z) + 1) * grid.size;
                !bounds.IsValid() || maximumX <= minimumX || maximumZ <= minimumZ || !joinedX || !joinedZ)
                return Result<Nav::NavigationBakeTile>::Failure(
                    MakeError(Errors::BakeInputInvalid, "Navigation float-grid edges collapse or leave a gap"));
            return Result<Nav::NavigationBakeTile>::Success({key, bounds, grid.size});
        }

        /** @brief Emits one reserved grid with bounded cancellation checks and exact shared-edge validation. */
        Result<void> AppendGrid(std::vector<Nav::NavigationBakeTile> &output, const Grid &grid, CancellationToken cancellation) {
            for (std::int64_t x = grid.x.first; x <= grid.x.last; ++x)
                for (std::int64_t z = grid.z.first; z <= grid.z.last; ++z) {
                    if (cancellation.IsCancellationRequested())
                        return Result<void>::Failure(MakeError(Errors::BakeInputCancelled));
                    auto tile = Tile(grid, static_cast<std::int32_t>(x), static_cast<std::int32_t>(z));
                    if (tile.HasError())
                        return Result<void>::Failure(tile.ErrorValue());
                    output.push_back(std::move(tile).Value());
                }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CaptureProjectBakeTiles */
    Result<std::vector<Nav::NavigationBakeTile>> CaptureProjectBakeTiles(const Nav::NavigationBakeInputSnapshot &input,
                                                                         const Nav::NavigationDefinition &definition,
                                                                         const ProjectBakeSurfaces &surfaces, std::size_t maximumTiles,
                                                                         CancellationToken cancellation) {
        using Return = Result<std::vector<Nav::NavigationBakeTile>>;
        maximumTiles = std::min<std::size_t>(maximumTiles, definition.Tiles().maximumTiles);
        std::vector<Grid> plans;
        plans.reserve(input.Partitions().size());
        std::size_t count{};
        for (const auto &partition : input.Partitions()) {
            if (cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(Errors::BakeInputCancelled));
            auto planned = Plan(partition, input, definition, surfaces);
            if (planned.HasError())
                return Return::Failure(planned.ErrorValue());
            const auto &grid = planned.Value();
            if (grid.x.count > maximumTiles - count || grid.z.count > (maximumTiles - count) / grid.x.count)
                return Return::Failure(MakeError(Errors::CapacityExceeded, "Complete navigation grid exceeds the selected tile budget"));
            count += static_cast<std::size_t>(grid.x.count * grid.z.count);
            plans.push_back(grid);
        }
        std::vector<Nav::NavigationBakeTile> output;
        output.reserve(count);
        for (const auto &grid : plans)
            if (auto appended = AppendGrid(output, grid, cancellation); appended.HasError())
                return Return::Failure(appended.ErrorValue());
        std::ranges::sort(output, {}, &Nav::NavigationBakeTile::key);
        return Return::Success(std::move(output));
    }
}  // namespace Horo::Application::NavigationBakeDetail
