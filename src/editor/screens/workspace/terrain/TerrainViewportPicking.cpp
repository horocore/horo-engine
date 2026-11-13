#include "TerrainViewportPicking.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Editor {
    namespace {
        struct Point final {
            double x{};
            double y{};
            double z{};
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
            const auto edge1 = Sub(b, a);
            const auto edge2 = Sub(c, a);
            const auto p = Cross(direction, edge2);
            const double determinant = Dot(edge1, p);
            if (std::abs(determinant) < 1e-15)
                return {};
            const double inverse = 1.0 / determinant;
            const auto t = Sub(origin, a);
            const double u = Dot(t, p) * inverse;
            if (u < -1e-10 || u > 1.0 + 1e-10)
                return {};
            const auto q = Cross(t, edge1);
            if (const double v = Dot(direction, q) * inverse; v < -1e-10 || u + v > 1.0 + 1e-10)
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
            double a = -origin / direction;
            double b = (maximum - origin) / direction;
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

        using PickResult = Result<std::optional<TerrainSurfaceHit>>;

        /** @brief Admit source shape without inspecting or allocating channel payloads. */
        bool ValidGrid(const Terrain::TerrainCanonicalSource &source) {
            return source.width >= 2 && source.height >= 2 && source.width <= Terrain::TerrainDescriptorHardLimits::SamplesPerAxis &&
                   source.height <= Terrain::TerrainDescriptorHardLimits::SamplesPerAxis &&
                   source.heightsMeters.size() == std::uint64_t{source.width} * source.height &&
                   (source.holes.empty() || source.holes.size() == source.heightsMeters.size());
        }

        /** @brief Admit the physical grid scale independently of per-cell traversal. */
        bool ValidSpacing(const Terrain::TerrainSourceCoordinates &coordinates) {
            return std::isfinite(coordinates.spacingX) && coordinates.spacingX > 0 && std::isfinite(coordinates.spacingZ) &&
                   coordinates.spacingZ > 0;
        }

        /** @brief Admit a finite normalized world ray and ordered nonnegative distance interval. */
        bool ValidRay(const Math::Ray &ray) {
            return Math::IsFinite(ray.origin) && Math::IsFinite(ray.direction) &&
                   std::abs(Math::Dot(ray.direction, ray.direction) - 1.0F) <= 1e-3F && std::isfinite(ray.minimumDistance) &&
                   std::isfinite(ray.maximumDistance) && ray.minimumDistance >= 0 && ray.maximumDistance >= ray.minimumDistance;
        }

        /** @brief Validated local ray retains the original world-distance parameter under affine transformation. */
        struct LocalRay final {
            Point origin;
            Point direction;
        };

        Result<LocalRay> TransformRay(const Math::Transform &transform, const Math::Ray &ray) {
            auto matrix = transform.TryToMatrix();
            if (matrix.HasError())
                return Result<LocalRay>::Failure(matrix.ErrorValue());
            auto inverse = Math::TryInverseAffine(matrix.Value());
            if (inverse.HasError())
                return Result<LocalRay>::Failure(inverse.ErrorValue());
            auto origin = Math::TryTransformPoint(inverse.Value(), ray.origin);
            if (origin.HasError())
                return Result<LocalRay>::Failure(origin.ErrorValue());
            auto direction = Math::TransformDirection(inverse.Value(), ray.direction);
            if (!Math::IsFinite(direction))
                return Result<LocalRay>::Failure(MakeError(TerrainEditErrors::Invalid));
            return Result<LocalRay>::Success(
                {{origin.Value().x, origin.Value().y, origin.Value().z}, {direction.x, direction.y, direction.z}});
        }

        /** @brief One monotonic DDA axis owns its boundary/extent checks. */
        struct Axis final {
            std::uint32_t cell{};
            std::uint32_t samples{};
            int step{};
            double next{};
            double delta{};

            bool Advance() {
                if ((step < 0 && cell == 0) || (step > 0 && cell + 1 >= samples - 1))
                    return false;
                cell = static_cast<std::uint32_t>(static_cast<std::int64_t>(cell) + step);
                next += delta;
                return true;
            }
        };

        /** @brief Initialize exact source-sample traversal, including stationary axes. */
        Axis MakeAxis(double origin, double direction, double begin, std::uint32_t samples) {
            Axis axis;
            axis.cell = Cell(origin + direction * begin, samples);
            axis.samples = samples;
            axis.step = direction > 0 ? 1 : direction < 0 ? -1 : 0;
            axis.next = std::numeric_limits<double>::infinity();
            axis.delta = axis.next;
            if (axis.step != 0) {
                const double boundary = axis.step > 0 ? double(axis.cell + 1) : double(axis.cell);
                axis.next = (boundary - origin) / direction;
                axis.delta = std::abs(1.0 / direction);
            }
            return axis;
        }

        /** @brief Reject noncanonical sample payload while retaining explicit source-hole visibility. */
        Result<bool> SolidCell(const Terrain::TerrainCanonicalSource &source, const std::array<std::size_t, 4> &indices) {
            bool solid = true;
            for (const auto index : indices) {
                if (!std::isfinite(source.heightsMeters[index]))
                    return Result<bool>::Failure(MakeError(TerrainEditErrors::Invalid));
                if (!source.holes.empty()) {
                    if (source.holes[index] > 1)
                        return Result<bool>::Failure(MakeError(TerrainEditErrors::Invalid));
                    solid = solid && source.holes[index] == 0;
                }
            }
            return Result<bool>::Success(solid);
        }

        /** @brief Choose the nearest hit on the canonical source diagonal. */
        std::optional<double> Nearest(std::optional<double> first, std::optional<double> second) {
            if (first.has_value() && second.has_value())
                return std::min(*first, *second);
            return first.has_value() ? first : second;
        }

        /** @brief Bounded traversal scratch; no live runtime/renderer or mutable document is retained. */
        struct Walk final {
            const Terrain::TerrainCanonicalSource &source;
            const Math::Ray &world;
            LocalRay local;
            TerrainSurfacePickMode mode;
            Axis x;
            Axis z;
            double begin{};
            double end{};

            /** @brief Intersect one validated source cell, with explicit hole restoration policy. */
            PickResult Hit() const {
                const auto cx = x.cell;
                const auto cz = z.cell;
                const std::array<std::size_t, 4> indices{std::size_t(cz) * source.width + cx, std::size_t(cz) * source.width + cx + 1,
                                                         std::size_t(cz + 1) * source.width + cx,
                                                         std::size_t(cz + 1) * source.width + cx + 1};
                auto solid = SolidCell(source, indices);
                if (solid.HasError())
                    return PickResult::Failure(solid.ErrorValue());
                if (!solid.Value() && mode == TerrainSurfacePickMode::VisibleSurface)
                    return PickResult::Success({});
                const double sx = source.coordinates.spacingX;
                const double sz = source.coordinates.spacingZ;
                const Point a{cx * sx, source.heightsMeters[indices[0]], cz * sz};
                const Point b{(cx + 1) * sx, source.heightsMeters[indices[1]], cz * sz};
                const Point c{cx * sx, source.heightsMeters[indices[2]], (cz + 1) * sz};
                const Point d{(cx + 1) * sx, source.heightsMeters[indices[3]], (cz + 1) * sz};
                const double cellEnd = std::min({x.next, z.next, end});
                const auto hit = Nearest(Triangle(local.origin, local.direction, a, c, b, begin, cellEnd),
                                         Triangle(local.origin, local.direction, b, c, d, begin, cellEnd));
                if (!hit.has_value())
                    return PickResult::Success({});
                const double px = local.origin.x + local.direction.x * *hit;
                const double py = local.origin.y + local.direction.y * *hit;
                const double pz = local.origin.z + local.direction.z * *hit;
                return PickResult::Success(TerrainSurfaceHit{{px / sx, pz / sz},
                                                             world.origin + world.direction * static_cast<float>(*hit),
                                                             static_cast<float>(py),
                                                             *hit});
            }

            /** @brief Cross all simultaneous cell boundaries without walking beyond source membership. */
            bool Advance() {
                const double cellEnd = std::min({x.next, z.next, end});
                if (cellEnd >= end)
                    return false;
                const bool crossX = x.next <= z.next;
                const bool crossZ = z.next <= x.next;
                if (crossX && !x.Advance())
                    return false;
                if (crossZ && !z.Advance())
                    return false;
                begin = cellEnd;
                return true;
            }

            /** @brief Perform only admitted visits and cooperatively reject work before returning a detached hit. */
            PickResult Run(std::size_t maximum, const CancellationToken &cancellation) {
                for (std::size_t visited = 0; visited < maximum; ++visited) {
                    if (cancellation.IsCancellationRequested())
                        return PickResult::Failure(MakeError(TerrainEditErrors::Cancelled));
                    auto hit = Hit();
                    if (hit.HasError())
                        return hit;
                    if (hit.Value())
                        return hit;
                    if (!Advance())
                        return PickResult::Success({});
                }
                return PickResult::Failure(MakeError(TerrainEditErrors::LimitExceeded));
            }
        };

        /** @brief Clip and construct the actual heightfield traversal from an admitted local ray. */
        PickResult Traverse(const Terrain::TerrainCanonicalSource &source, const Math::Ray &ray, const LocalRay &local, std::size_t maximum,
                            const CancellationToken &cancel, TerrainSurfacePickMode mode) {
            const double ox = local.origin.x / source.coordinates.spacingX;
            const double oz = local.origin.z / source.coordinates.spacingZ;
            const double dx = local.direction.x / source.coordinates.spacingX;
            const double dz = local.direction.z / source.coordinates.spacingZ;
            if (!std::isfinite(ox) || !std::isfinite(oz) || !std::isfinite(dx) || !std::isfinite(dz))
                return PickResult::Failure(MakeError(TerrainEditErrors::Invalid));
            double begin = ray.minimumDistance;
            double end = ray.maximumDistance;
            if (!Clip(ox, dx, source.width - 1, begin, end) || !Clip(oz, dz, source.height - 1, begin, end))
                return PickResult::Success({});
            Walk walk{source, ray, local, mode, MakeAxis(ox, dx, begin, source.width), MakeAxis(oz, dz, begin, source.height), begin, end};
            return walk.Run(maximum, cancel);
        }
    }  // namespace

    /** @copydoc PickTerrainSurface */
    Result<std::optional<TerrainSurfaceHit>> PickTerrainSurface(const Terrain::TerrainCanonicalSource &source,
                                                                const Math::Transform &datasetToViewport, const Math::Ray &ray,
                                                                std::size_t maximumVisitedCells, const CancellationToken &cancellation,
                                                                TerrainSurfacePickMode mode) {
        if (cancellation.IsCancellationRequested())
            return PickResult::Failure(MakeError(TerrainEditErrors::Cancelled));
        if (!ValidGrid(source) || !ValidSpacing(source.coordinates) || !ValidRay(ray) || !maximumVisitedCells ||
            mode > TerrainSurfacePickMode::UnderlyingHeightfield)
            return PickResult::Failure(MakeError(TerrainEditErrors::Invalid));
        auto local = TransformRay(datasetToViewport, ray);
        if (local.HasError())
            return PickResult::Failure(local.ErrorValue());
        return Traverse(source, ray, local.Value(), maximumVisitedCells, cancellation, mode);
    }
}  // namespace Horo::Editor
