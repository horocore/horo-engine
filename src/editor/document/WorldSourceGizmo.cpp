#include "Horo/Editor/PartitionedWorldWorkspace.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;
        using Point = Math::WorldCoordinate64;

        /** @brief Rejects unrepresentable handle coordinates before integer conversion. */
        Result<Point> Offset(Point origin, long double x, long double y, long double z) {
            const auto base = origin.Millimeters();
            std::array<std::int64_t, 3> result{};
            const std::array<long double, 3> offset{x, y, z};
            for (std::size_t axis = 0; axis < result.size(); ++axis) {
                const auto rounded = std::round(offset[axis]);
                if (!std::isfinite(rounded) || std::abs(rounded) > WS::StreamingSourceRangeLimits::MaximumExtentMillimeters)
                    return Result<Point>::Failure(MakeError(WorldDocumentErrors::Invalid));
                const auto delta = static_cast<std::int64_t>(rounded);
                if ((delta > 0 && base[axis] > std::numeric_limits<std::int64_t>::max() - delta) ||
                    (delta < 0 && base[axis] < std::numeric_limits<std::int64_t>::min() - delta))
                    return Result<Point>::Failure(MakeError(WorldDocumentErrors::Invalid));
                result[axis] = base[axis] + delta;
            }
            return Result<Point>::Success(Point::FromMillimeters(result[0], result[1], result[2]));
        }

        /** @brief Computes differences without signed integer overflow. */
        std::array<long double, 3> Difference(Point left, Point right) {
            const auto a = left.Millimeters();
            const auto b = right.Millimeters();
            std::array<long double, 3> delta{};
            for (std::size_t axis = 0; axis < delta.size(); ++axis) {
                const auto magnitude = a[axis] >= b[axis] ? static_cast<std::uint64_t>(a[axis]) - static_cast<std::uint64_t>(b[axis])
                                                          : static_cast<std::uint64_t>(b[axis]) - static_cast<std::uint64_t>(a[axis]);
                delta[axis] = a[axis] >= b[axis] ? static_cast<long double>(magnitude) : -static_cast<long double>(magnitude);
            }
            return delta;
        }

        /** @brief Moves a frustum anchor or its signed plane distance without admitting unbounded deltas. */
        Result<void> MoveFrustum(WS::StreamingSourceFrustum &shape, std::size_t handle, Point position) {
            if (handle == 0) {
                shape.anchor = position;
                return Result<void>::Success();
            }
            auto &plane = shape.planes[handle - 1];
            const auto delta = Difference(position, shape.anchor);
            if (std::ranges::any_of(delta, [](long double value) {
                return std::abs(value) > WS::StreamingSourceRangeLimits::MaximumExtentMillimeters;
            }))
                return Result<void>::Failure(MakeError(WorldDocumentErrors::Invalid));
            const long double distance =
                -(delta[0] * plane.inwardNormal.x + delta[1] * plane.inwardNormal.y + delta[2] * plane.inwardNormal.z);
            if (!std::isfinite(distance) || distance < 0 || distance > shape.maximumExtentMillimeters)
                return Result<void>::Failure(MakeError(WorldDocumentErrors::Invalid));
            plane.distanceMillimeters = static_cast<float>(distance);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc WorldSourceViewportPoint */
    Result<Math::Vec3> WorldSourceViewportPoint(Point point, Point origin) {
        const auto delta = Difference(point, origin);
        if (std::ranges::any_of(delta, [](long double value) {
            return std::abs(value) > 8'000'000;
        }))
            return Result<Math::Vec3>::Failure(MakeError(WorldDocumentErrors::Unsupported));
        return Result<Math::Vec3>::Success(
            {static_cast<float>(delta[0]) / 1000, static_cast<float>(delta[1]) / 1000, static_cast<float>(delta[2]) / 1000});
    }

    /** @copydoc WorldSourceViewportGlobal */
    Result<Math::WorldCoordinate64> WorldSourceViewportGlobal(Point origin, Math::Vec3 local) {
        if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || std::abs(local.x) > 8000 ||
            std::abs(local.y) > 8000 || std::abs(local.z) > 8000)
            return Result<Point>::Failure(MakeError(WorldDocumentErrors::Unsupported));
        return Offset(origin, static_cast<long double>(local.x) * 1000, static_cast<long double>(local.y) * 1000,
                      static_cast<long double>(local.z) * 1000);
    }

    /** @copydoc WorldSourceGizmoPoints */
    Result<WorldSourceGizmoPointSet> WorldSourceGizmoPoints(const PartitionedWorldSource &source) {
        using Points = WorldSourceGizmoPointSet;
        if (const auto valid = WS::ValidateStreamingSourceShape(source.shape); valid.HasError())
            return Result<Points>::Failure(valid.ErrorValue());
        return std::visit([](const auto &shape) -> Result<Points> {
            using T = std::decay_t<decltype(shape)>;
            Points points;
            if constexpr (std::is_same_v<T, WS::StreamingSourceBox>) {
                points.points_[points.count_++] = shape.minimum;
                points.points_[points.count_++] = shape.maximum;
            } else if constexpr (std::is_same_v<T, WS::StreamingSourceFrustum>) {
                points.points_[points.count_++] = shape.anchor;
                for (const auto &plane : shape.planes) {
                    const auto point =
                        Offset(shape.anchor, -plane.inwardNormal.x * plane.distanceMillimeters,
                               -plane.inwardNormal.y * plane.distanceMillimeters, -plane.inwardNormal.z * plane.distanceMillimeters);
                    if (point.HasError())
                        return Result<Points>::Failure(point.ErrorValue());
                    points.points_[points.count_++] = point.Value();
                }
            } else {
                Point origin;
                std::int64_t extent{};
                if constexpr (std::is_same_v<T, WS::StreamingSourceSphere>) {
                    origin = shape.center;
                    extent = shape.radiusMillimeters;
                    points.points_[points.count_++] = origin;
                } else {
                    origin = shape.points[0];
                    extent = shape.halfExtentMillimeters;
                    for (std::size_t index = 0; index < shape.pointCount; ++index)
                        points.points_[points.count_++] = shape.points[index];
                }
                const auto point = Offset(origin, extent, 0, 0);
                if (point.HasError())
                    return Result<Points>::Failure(point.ErrorValue());
                points.points_[points.count_++] = point.Value();
            }
            return Result<Points>::Success(std::move(points));
        }, source.shape);
    }

    /** @copydoc MoveWorldSourceGizmo */
    Result<PartitionedWorldSource> MoveWorldSourceGizmo(const PartitionedWorldSource &source, const std::size_t handle, Point position) {
        const auto points = WorldSourceGizmoPoints(source);
        if (points.HasError())
            return Result<PartitionedWorldSource>::Failure(points.ErrorValue());
        if (handle >= points.Value().Points().size())
            return Result<PartitionedWorldSource>::Failure(MakeError(WorldDocumentErrors::Invalid));
        auto candidate = source;
        const auto changed = std::visit([&](auto &shape) -> Result<void> {
            using T = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<T, WS::StreamingSourceBox>) {
                (handle == 0 ? shape.minimum : shape.maximum) = position;
            } else if constexpr (std::is_same_v<T, WS::StreamingSourceFrustum>) {
                if (const auto moved = MoveFrustum(shape, handle, position); moved.HasError())
                    return moved;
            } else {
                Point origin;
                bool extentHandle{};
                if constexpr (std::is_same_v<T, WS::StreamingSourceSphere>) {
                    if (handle == 0)
                        shape.center = position;
                    origin = shape.center;
                    extentHandle = handle == 1;
                } else {
                    if (handle < shape.pointCount)
                        shape.points[handle] = position;
                    origin = shape.points[0];
                    extentHandle = handle == shape.pointCount;
                }
                if (extentHandle) {
                    const auto delta = Difference(position, origin);
                    if (delta[0] < 1 || delta[0] > WS::StreamingSourceRangeLimits::MaximumExtentMillimeters)
                        return Result<void>::Failure(MakeError(WorldDocumentErrors::Invalid));
                    const auto extent = static_cast<std::int64_t>(delta[0]);
                    if constexpr (std::is_same_v<T, WS::StreamingSourceSphere>)
                        shape.radiusMillimeters = extent;
                    else
                        shape.halfExtentMillimeters = extent;
                }
            }
            return WS::ValidateStreamingSourceShape(candidate.shape);
        }, candidate.shape);
        if (changed.HasError())
            return Result<PartitionedWorldSource>::Failure(changed.ErrorValue());
        return Result<PartitionedWorldSource>::Success(std::move(candidate));
    }
}  // namespace Horo::Editor
