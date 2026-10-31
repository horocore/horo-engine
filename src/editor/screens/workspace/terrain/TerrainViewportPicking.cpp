#include "TerrainViewportPicking.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Editor {
    namespace {
        struct Point final {
            double x{}, y{}, z{};
        };

        Point Sub(Point a, Point b) noexcept {
            return {a.x - b.x, a.y - b.y, a.z - b.z};
        }

        Point Cross(Point a, Point b) noexcept {
            return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        }

        double Dot(Point a, Point b) noexcept {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        /** @brief Double-precision two-sided ray/triangle intersection preserves canonical interpolation. */
        std::optional<double> Triangle(Point origin, Point direction, Point a, Point b, Point c, double minimum, double maximum) noexcept {
            const auto edge1 = Sub(b, a), edge2 = Sub(c, a), p = Cross(direction, edge2);
            const double determinant = Dot(edge1, p);
            if (std::abs(determinant) < 1e-15)
                return {};
            const double inverse = 1.0 / determinant;
            const auto t = Sub(origin, a);
            const double u = Dot(t, p) * inverse;
            if (u < -1e-10 || u > 1.0 + 1e-10)
                return {};
            const auto q = Cross(t, edge1);
            const double v = Dot(direction, q) * inverse;
            if (v < -1e-10 || u + v > 1.0 + 1e-10)
                return {};
            const double distance = Dot(edge2, q) * inverse;
            if (!std::isfinite(distance) || distance < minimum || distance > maximum)
                return {};
            return distance;
        }

        /** @brief Clip the finite ray to one exact canonical grid interval. */
        bool Clip(double origin, double direction, double maximum, double &begin, double &end) noexcept {
            if (direction == 0)
                return origin >= 0 && origin <= maximum;
            double a = -origin / direction, b = (maximum - origin) / direction;
            if (a > b)
                std::swap(a, b);
            begin = std::max(begin, a);
            end = std::min(end, b);
            return begin <= end;
        }

        /** @brief The boundary belongs to the adjacent interior cell instead of an out-of-range sample. */
        std::uint32_t Cell(double sample, std::uint32_t samples) noexcept {
            return static_cast<std::uint32_t>(std::clamp(std::floor(sample), 0.0, double(samples - 2)));
        }
    }  // namespace

    /** @copydoc PickTerrainSurface */
    Result<std::optional<TerrainSurfaceHit>> PickTerrainSurface(const Terrain::TerrainCanonicalSource &source,
                                                                const Math::Transform &datasetToViewport, const Math::Ray &ray,
                                                                const std::size_t maximumVisitedCells,
                                                                const CancellationToken &cancellation) {
        using Return = Result<std::optional<TerrainSurfaceHit>>;
        if (cancellation.IsCancellationRequested())
            return Return::Failure(MakeError(TerrainEditErrors::Cancelled));
        if (source.width < 2 || source.height < 2 || source.width > Terrain::TerrainDescriptorHardLimits::SamplesPerAxis ||
            source.height > Terrain::TerrainDescriptorHardLimits::SamplesPerAxis ||
            source.heightsMeters.size() != std::uint64_t{source.width} * source.height ||
            (!source.holes.empty() && source.holes.size() != source.heightsMeters.size()) || !std::isfinite(source.coordinates.spacingX) ||
            source.coordinates.spacingX <= 0 || !std::isfinite(source.coordinates.spacingZ) || source.coordinates.spacingZ <= 0 ||
            !Math::IsFinite(ray.origin) || !Math::IsFinite(ray.direction) ||
            std::abs(Math::Dot(ray.direction, ray.direction) - 1.0F) > 1e-3F || !std::isfinite(ray.minimumDistance) ||
            !std::isfinite(ray.maximumDistance) || ray.minimumDistance < 0 || ray.maximumDistance < ray.minimumDistance ||
            maximumVisitedCells == 0)
            return Return::Failure(MakeError(TerrainEditErrors::Invalid));
        auto matrix = datasetToViewport.TryToMatrix();
        if (matrix.HasError())
            return Return::Failure(std::move(matrix).ErrorValue());
        auto inverse = Math::TryInverseAffine(matrix.Value());
        if (inverse.HasError())
            return Return::Failure(std::move(inverse).ErrorValue());
        auto localOrigin = Math::TryTransformPoint(inverse.Value(), ray.origin);
        if (localOrigin.HasError())
            return Return::Failure(std::move(localOrigin).ErrorValue());
        const auto localDirection = Math::TransformDirection(inverse.Value(), ray.direction);
        if (!Math::IsFinite(localDirection))
            return Return::Failure(MakeError(TerrainEditErrors::Invalid));
        const Point origin{localOrigin.Value().x, localOrigin.Value().y, localOrigin.Value().z};
        const Point direction{localDirection.x, localDirection.y, localDirection.z};
        const double ox = origin.x / source.coordinates.spacingX, oz = origin.z / source.coordinates.spacingZ;
        const double dx = direction.x / source.coordinates.spacingX, dz = direction.z / source.coordinates.spacingZ;
        double begin = ray.minimumDistance, end = ray.maximumDistance;
        if (!Clip(ox, dx, source.width - 1, begin, end) || !Clip(oz, dz, source.height - 1, begin, end))
            return Return::Success({});
        auto x = Cell(ox + dx * begin, source.width), z = Cell(oz + dz * begin, source.height);
        const int stepX = dx > 0 ? 1 : dx < 0 ? -1 : 0, stepZ = dz > 0 ? 1 : dz < 0 ? -1 : 0;
        const double infinity = std::numeric_limits<double>::infinity();
        double nextX = stepX == 0 ? infinity : ((stepX > 0 ? double(x + 1) : double(x)) - ox) / dx;
        double nextZ = stepZ == 0 ? infinity : ((stepZ > 0 ? double(z + 1) : double(z)) - oz) / dz;
        const double deltaX = stepX == 0 ? infinity : std::abs(1.0 / dx), deltaZ = stepZ == 0 ? infinity : std::abs(1.0 / dz);
        for (std::size_t visited = 0; visited < maximumVisitedCells; ++visited) {
            if (cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(TerrainEditErrors::Cancelled));
            const std::array<std::size_t, 4> indices{std::size_t{z} * source.width + x, std::size_t{z} * source.width + x + 1,
                                                     std::size_t{z + 1} * source.width + x, std::size_t{z + 1} * source.width + x + 1};
            bool solid = true;
            for (const auto index : indices) {
                if (!std::isfinite(source.heightsMeters[index]) || (!source.holes.empty() && source.holes[index] > 1))
                    return Return::Failure(MakeError(TerrainEditErrors::Invalid));
                solid = solid && (source.holes.empty() || source.holes[index] == 0);
            }
            const double cellEnd = std::min({nextX, nextZ, end});
            if (solid) {
                const double sx = source.coordinates.spacingX, sz = source.coordinates.spacingZ;
                const Point a{x * sx, source.heightsMeters[indices[0]], z * sz}, b{(x + 1) * sx, source.heightsMeters[indices[1]], z * sz},
                    c{x * sx, source.heightsMeters[indices[2]], (z + 1) * sz},
                    d{(x + 1) * sx, source.heightsMeters[indices[3]], (z + 1) * sz};
                // Match Terrain source cook's canonical diagonal and winding.
                auto first = Triangle(origin, direction, a, c, b, begin, cellEnd);
                auto second = Triangle(origin, direction, b, c, d, begin, cellEnd);
                const auto hit = first && second ? std::optional{std::min(*first, *second)} : first ? first : second;
                if (hit) {
                    const double px = origin.x + direction.x * *hit, py = origin.y + direction.y * *hit, pz = origin.z + direction.z * *hit;
                    return Return::Success(TerrainSurfaceHit{{px / sx, pz / sz},
                                                             ray.origin + ray.direction * static_cast<float>(*hit),
                                                             static_cast<float>(py),
                                                             *hit});
                }
            }
            if (cellEnd >= end || (stepX == 0 && stepZ == 0))
                return Return::Success({});
            const bool crossX = nextX <= nextZ, crossZ = nextZ <= nextX;
            if (crossX) {
                if ((stepX < 0 && x == 0) || (stepX > 0 && x + 1 >= source.width - 1))
                    return Return::Success({});
                x = static_cast<std::uint32_t>(static_cast<std::int64_t>(x) + stepX);
                nextX += deltaX;
            }
            if (crossZ) {
                if ((stepZ < 0 && z == 0) || (stepZ > 0 && z + 1 >= source.height - 1))
                    return Return::Success({});
                z = static_cast<std::uint32_t>(static_cast<std::int64_t>(z) + stepZ);
                nextZ += deltaZ;
            }
            begin = cellEnd;
        }
        return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
    }
}  // namespace Horo::Editor
