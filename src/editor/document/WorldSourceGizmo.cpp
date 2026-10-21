#include "Horo/Editor/PartitionedWorldWorkspace.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;
        using Point = Math::WorldCoordinate64;

        /** @brief Rejects unrepresentable handle coordinates before integer conversion. */
        Result<Point> Offset(const Point &origin, long double x, long double y, long double z) {
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
        std::array<long double, 3> Difference(const Point &left, const Point &right) {
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
        Result<void> MoveFrustum(WS::StreamingSourceFrustum &shape, std::size_t handle, const Point &position) {
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

        /** @brief Appends one exact positive-axis extent handle after admitted shape points. */
        Result<std::size_t> AppendExtent(std::span<Point> points, std::size_t count, const Point &origin, std::int64_t extent) {
            const auto point = Offset(origin, extent, 0, 0);
            if (point.HasError())
                return Result<std::size_t>::Failure(point.ErrorValue());
            points[count] = point.Value();
            return Result<std::size_t>::Success(count + 1);
        }

        /** @brief Emits the two authored box corners. */
        Result<std::size_t> FillPoints(const WS::StreamingSourceBox &shape, std::span<Point> points) {
            points[0] = shape.minimum;
            points[1] = shape.maximum;
            return Result<std::size_t>::Success(2);
        }

        /** @brief Emits the anchor and all six bounded signed-plane handles. */
        Result<std::size_t> FillPoints(const WS::StreamingSourceFrustum &shape, std::span<Point> points) {
            std::size_t count{};
            points[count++] = shape.anchor;
            for (const auto &plane : shape.planes) {
                const auto point =
                    Offset(shape.anchor, -plane.inwardNormal.x * plane.distanceMillimeters,
                           -plane.inwardNormal.y * plane.distanceMillimeters, -plane.inwardNormal.z * plane.distanceMillimeters);
                if (point.HasError())
                    return Result<std::size_t>::Failure(point.ErrorValue());
                points[count++] = point.Value();
            }
            return Result<std::size_t>::Success(count);
        }

        /** @brief Emits a sphere center and its radius handle. */
        Result<std::size_t> FillPoints(const WS::StreamingSourceSphere &shape, std::span<Point> points) {
            points[0] = shape.center;
            return AppendExtent(points, 1, shape.center, shape.radiusMillimeters);
        }

        /** @brief Emits the validated path prefix and one half-extent handle. */
        Result<std::size_t> FillPoints(const WS::StreamingSourcePathVolume &shape, std::span<Point> points) {
            std::copy_n(shape.points.begin(), shape.pointCount, points.begin());
            return AppendExtent(points, shape.pointCount, shape.points[0], shape.halfExtentMillimeters);
        }

        /** @brief Admits only a positive bounded X-axis extent before integer conversion. */
        Result<std::int64_t> ExtentFromHandle(const Point &position, const Point &origin) {
            const auto delta = Difference(position, origin);
            if (delta[0] < 1 || delta[0] > WS::StreamingSourceRangeLimits::MaximumExtentMillimeters)
                return Result<std::int64_t>::Failure(MakeError(WorldDocumentErrors::Invalid));
            return Result<std::int64_t>::Success(static_cast<std::int64_t>(delta[0]));
        }

        /** @brief Moves one admitted box corner; common validation follows the visitor. */
        Result<void> MoveGeometry(WS::StreamingSourceBox &shape, std::size_t handle, const Point &position) {
            (handle == 0 ? shape.minimum : shape.maximum) = position;
            return Result<void>::Success();
        }

        /** @brief Moves a frustum through its bounded plane-distance kernel. */
        Result<void> MoveGeometry(WS::StreamingSourceFrustum &shape, std::size_t handle, const Point &position) {
            return MoveFrustum(shape, handle, position);
        }

        /** @brief Moves the sphere center or admits its exact positive radius. */
        Result<void> MoveGeometry(WS::StreamingSourceSphere &shape, std::size_t handle, const Point &position) {
            if (handle == 0) {
                shape.center = position;
                return Result<void>::Success();
            }
            const auto extent = ExtentFromHandle(position, shape.center);
            if (extent.HasError())
                return Result<void>::Failure(extent.ErrorValue());
            shape.radiusMillimeters = extent.Value();
            return Result<void>::Success();
        }

        /** @brief Moves an admitted path vertex or its exact positive half-extent. */
        Result<void> MoveGeometry(WS::StreamingSourcePathVolume &shape, std::size_t handle, const Point &position) {
            if (handle < shape.pointCount) {
                shape.points[handle] = position;
                return Result<void>::Success();
            }
            const auto extent = ExtentFromHandle(position, shape.points[0]);
            if (extent.HasError())
                return Result<void>::Failure(extent.ErrorValue());
            shape.halfExtentMillimeters = extent.Value();
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc WorldSourceViewportPoint */
    Result<Math::Vec3> WorldSourceViewportPoint(const Point &point, const Point &origin) {
        const auto delta = Difference(point, origin);
        if (std::ranges::any_of(delta, [](long double value) {
            return std::abs(value) > 8'000'000;
        }))
            return Result<Math::Vec3>::Failure(MakeError(WorldDocumentErrors::Unsupported));
        return Result<Math::Vec3>::Success(
            {static_cast<float>(delta[0]) / 1000, static_cast<float>(delta[1]) / 1000, static_cast<float>(delta[2]) / 1000});
    }

    /** @copydoc WorldSourceViewportGlobal */
    Result<Math::WorldCoordinate64> WorldSourceViewportGlobal(const Point &origin, Math::Vec3 local) {
        if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || std::abs(local.x) > 8000 ||
            std::abs(local.y) > 8000 || std::abs(local.z) > 8000)
            return Result<Point>::Failure(MakeError(WorldDocumentErrors::Unsupported));
        return Offset(origin, static_cast<long double>(local.x) * 1000, static_cast<long double>(local.y) * 1000,
                      static_cast<long double>(local.z) * 1000);
    }

    /** @copydoc WorldSourceGizmoPoints */
    Result<WorldSourceGizmoPointSet> WorldSourceGizmoPoints(const PartitionedWorldSource &source) {
        if (const auto valid = WS::ValidateStreamingSourceShape(source.shape); valid.HasError())
            return Result<WorldSourceGizmoPointSet>::Failure(valid.ErrorValue());
        WorldSourceGizmoPointSet points;
        const auto count = std::visit([&]<typename Shape>(const Shape &shape) {
            return FillPoints(shape, points.points_);
        }, source.shape);
        if (count.HasError())
            return Result<WorldSourceGizmoPointSet>::Failure(count.ErrorValue());
        points.count_ = count.Value();
        return Result<WorldSourceGizmoPointSet>::Success(std::move(points));
    }

    /** @copydoc MoveWorldSourceGizmo */
    Result<PartitionedWorldSource> MoveWorldSourceGizmo(const PartitionedWorldSource &source, const std::size_t handle,
                                                        const Point &position) {
        const auto points = WorldSourceGizmoPoints(source);
        if (points.HasError())
            return Result<PartitionedWorldSource>::Failure(points.ErrorValue());
        if (handle >= points.Value().Points().size())
            return Result<PartitionedWorldSource>::Failure(MakeError(WorldDocumentErrors::Invalid));
        if (points.Value().Points()[handle] == position)
            return Result<PartitionedWorldSource>::Success(source);
        auto candidate = source;
        if (const auto changed = std::visit(
                [&]<typename Shape>(Shape &shape) {
            return MoveGeometry(shape, handle, position);
        }, candidate.shape);
            changed.HasError())
            return Result<PartitionedWorldSource>::Failure(changed.ErrorValue());
        if (const auto valid = WS::ValidateStreamingSourceShape(candidate.shape); valid.HasError())
            return Result<PartitionedWorldSource>::Failure(valid.ErrorValue());
        return Result<PartitionedWorldSource>::Success(std::move(candidate));
    }
}  // namespace Horo::Editor
