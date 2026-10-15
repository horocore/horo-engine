#include "Horo/WorldStreaming/StreamingSourceRange.h"

#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "WorldStreamingInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>

namespace Horo::WorldStreaming {
    namespace {
        using Coordinates = std::array<std::int64_t, 3>;
        constexpr double FrustumBoundaryEpsilon = 1.0e-5;

        struct ExactBounds final {
            Coordinates minimum{};
            Coordinates maximum{};
        };

        [[nodiscard]] Result<void> InvalidShape() {
            return Result<void>::Failure(MakeError(WorldStreamingErrors::SourceShapeInvalid));
        }

        [[nodiscard]] std::uint64_t Distance(const std::int64_t lhs, const std::int64_t rhs) noexcept {
            if ((lhs < 0) == (rhs < 0))
                return Internal::UnsignedMagnitude(lhs - rhs);
            return Internal::UnsignedMagnitude(lhs) + Internal::UnsignedMagnitude(rhs);
        }

        [[nodiscard]] bool MultiplyChecked(const std::int64_t lhs, const std::int64_t rhs, std::int64_t &product) noexcept {
            if (lhs == 0 || rhs == 0) {
                product = 0;
                return true;
            }
            const bool negative = (lhs < 0) != (rhs < 0);
            const std::uint64_t lhsMagnitude = Internal::UnsignedMagnitude(lhs);
            const std::uint64_t rhsMagnitude = Internal::UnsignedMagnitude(rhs);
            constexpr std::uint64_t MinimumMagnitude = std::uint64_t{1} << 63U;
            if (const std::uint64_t limit =
                    negative ? MinimumMagnitude : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
                lhsMagnitude > limit / rhsMagnitude)
                return false;
            if (const std::uint64_t productMagnitude = lhsMagnitude * rhsMagnitude; negative && productMagnitude == MinimumMagnitude)
                product = std::numeric_limits<std::int64_t>::min();
            else
                product = negative ? -static_cast<std::int64_t>(productMagnitude) : static_cast<std::int64_t>(productMagnitude);
            return true;
        }

        [[nodiscard]] std::int64_t AddSaturated(const std::int64_t value, const std::int64_t delta) noexcept {
            if (std::int64_t sum{}; Internal::CheckedAdd(value, delta, sum))
                return sum;
            return delta < 0 ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max();
        }

        [[nodiscard]] std::uint64_t AxisDistanceFromBounds(const std::int64_t coordinate, const std::int64_t minimum,
                                                           const std::int64_t maximum) noexcept {
            if (coordinate < minimum)
                return Distance(coordinate, minimum);
            if (coordinate > maximum)
                return Distance(coordinate, maximum);
            return 0U;
        }

        [[nodiscard]] bool ExpandChecked(const Coordinates &center, const std::int64_t extent, ExactBounds &bounds) noexcept {
            for (std::size_t axis = 0; axis < center.size(); ++axis) {
                if (!Internal::CheckedAdd(center[axis], -extent, bounds.minimum[axis]) ||
                    !Internal::CheckedAdd(center[axis], extent, bounds.maximum[axis]))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool OrderedAndBounded(const ExactBounds &bounds) noexcept {
            for (std::size_t axis = 0; axis < bounds.minimum.size(); ++axis) {
                if (bounds.minimum[axis] > bounds.maximum[axis] ||
                    Distance(bounds.minimum[axis], bounds.maximum[axis]) >
                        static_cast<std::uint64_t>(StreamingSourceRangeLimits::MaximumExtentMillimeters))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool Overlaps(const ExactBounds &lhs, const ExactBounds &rhs) noexcept {
            for (std::size_t axis = 0; axis < lhs.minimum.size(); ++axis) {
                if (lhs.maximum[axis] < rhs.minimum[axis] || rhs.maximum[axis] < lhs.minimum[axis])
                    return false;
            }
            return true;
        }

        [[nodiscard]] Result<void> ValidateCellReference(const WorldCellQuantizationPolicy &policy, const StreamingCellId cell) {
            if (!cell.IsValid())
                return Result<void>::Failure(MakeError(WorldStreamingErrors::IdentityInvalid));
            if (cell.lod >= policy.LodLevels())
                return Result<void>::Failure(MakeError(WorldStreamingErrors::LodUnsupported));
            const WorldCellBounds allowed = policy.Bounds();
            const std::array coordinates{cell.x, cell.y, cell.z};
            const std::array minimum{allowed.minimumX, allowed.minimumY, allowed.minimumZ};
            const std::array maximum{allowed.maximumX, allowed.maximumY, allowed.maximumZ};
            for (std::size_t axis = 0; axis < coordinates.size(); ++axis) {
                if (coordinates[axis] < minimum[axis] || coordinates[axis] > maximum[axis])
                    return Result<void>::Failure(MakeError(WorldStreamingErrors::CellOutOfBounds));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::array<std::int64_t, 2>> CellAxisBounds(const std::int64_t origin, const std::int64_t cellSize,
                                                                         const std::int32_t coordinate) {
            std::int64_t offset{};
            if (!MultiplyChecked(cellSize, coordinate, offset))
                return Result<std::array<std::int64_t, 2>>::Failure(MakeError(WorldStreamingErrors::CoordinateOutOfRange));
            std::int64_t minimum{};
            if (!Internal::CheckedAdd(origin, offset, minimum))
                return Result<std::array<std::int64_t, 2>>::Failure(MakeError(WorldStreamingErrors::CoordinateOutOfRange));
            std::int64_t maximum{};
            if (!Internal::CheckedAdd(minimum, cellSize - 1, maximum))
                return Result<std::array<std::int64_t, 2>>::Failure(MakeError(WorldStreamingErrors::CoordinateOutOfRange));
            return Result<std::array<std::int64_t, 2>>::Success({minimum, maximum});
        }

        [[nodiscard]] Result<ExactBounds> CellBounds(const WorldCellQuantizationPolicy &policy, const StreamingCellId cell) {
            if (const auto validCell = ValidateCellReference(policy, cell); validCell.HasError())
                return Result<ExactBounds>::Failure(validCell.ErrorValue());

            const std::int64_t cellSize = policy.BaseCellSizeMillimeters() << cell.lod;
            const Coordinates origin = policy.Origin().Millimeters();
            const std::array coordinates{cell.x, cell.y, cell.z};
            ExactBounds bounds;
            for (std::size_t axis = 0; axis < coordinates.size(); ++axis) {
                const auto axisBounds = CellAxisBounds(origin[axis], cellSize, coordinates[axis]);
                if (axisBounds.HasError())
                    return Result<ExactBounds>::Failure(axisBounds.ErrorValue());
                bounds.minimum[axis] = axisBounds.Value()[0];
                bounds.maximum[axis] = axisBounds.Value()[1];
            }
            return Result<ExactBounds>::Success(bounds);
        }

        [[nodiscard]] bool Intersects(const StreamingSourceSphere &sphere, const ExactBounds &cell) noexcept {
            const Coordinates center = sphere.center.Millimeters();
            const auto radius = static_cast<std::uint64_t>(sphere.radiusMillimeters);
            std::uint64_t squaredDistance{};
            for (std::size_t axis = 0; axis < center.size(); ++axis) {
                const std::uint64_t distance = AxisDistanceFromBounds(center[axis], cell.minimum[axis], cell.maximum[axis]);
                if (distance > radius)
                    return false;
                squaredDistance += distance * distance;
            }
            return squaredDistance <= radius * radius;
        }

        [[nodiscard]] bool Intersects(const StreamingSourceBox &box, const ExactBounds &cell) noexcept {
            return Overlaps({box.minimum.Millimeters(), box.maximum.Millimeters()}, cell);
        }

        [[nodiscard]] bool Intersects(const StreamingSourceFrustum &frustum, const ExactBounds &cell) noexcept {
            ExactBounds extentBounds;
            const Coordinates anchor = frustum.anchor.Millimeters();
            if (!ExpandChecked(anchor, frustum.maximumExtentMillimeters, extentBounds) || !Overlaps(extentBounds, cell))
                return false;

            for (const StreamingSourceFrustumPlane &plane : frustum.planes) {
                const std::array normal{plane.inwardNormal.x, plane.inwardNormal.y, plane.inwardNormal.z};
                double maximumProjection = plane.distanceMillimeters;
                for (std::size_t axis = 0; axis < normal.size(); ++axis) {
                    const std::int64_t selected = normal[axis] >= 0.0F ? std::min(cell.maximum[axis], extentBounds.maximum[axis])
                                                                       : std::max(cell.minimum[axis], extentBounds.minimum[axis]);
                    maximumProjection += static_cast<double>(normal[axis]) * static_cast<double>(selected - anchor[axis]);
                }
                if (maximumProjection < -FrustumBoundaryEpsilon)
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool SegmentIntersectsExpandedCell(const Coordinates &start, const Coordinates &end, const ExactBounds &cell,
                                                         const std::int64_t halfExtent) noexcept {
            double lower = 0.0;
            double upper = 1.0;
            for (std::size_t axis = 0; axis < start.size(); ++axis) {
                const auto direction = static_cast<double>(end[axis] - start[axis]);
                const std::int64_t segmentMinimum = std::min(start[axis], end[axis]);
                const std::int64_t segmentMaximum = std::max(start[axis], end[axis]);
                const std::int64_t expandedMinimum = AddSaturated(cell.minimum[axis], -halfExtent);
                const std::int64_t expandedMaximum = AddSaturated(cell.maximum[axis], halfExtent);
                if (expandedMaximum < segmentMinimum || segmentMaximum < expandedMinimum)
                    return false;
                const auto minimum = static_cast<double>(std::max(expandedMinimum, segmentMinimum) - start[axis]);
                const auto maximum = static_cast<double>(std::min(expandedMaximum, segmentMaximum) - start[axis]);
                if (direction == 0.0) {
                    if (0.0 < minimum || 0.0 > maximum)
                        return false;
                    continue;
                }
                double entry = minimum / direction;
                double exit = maximum / direction;
                if (entry > exit)
                    std::swap(entry, exit);
                lower = std::max(lower, entry);
                upper = std::min(upper, exit);
                if (lower > upper)
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool Intersects(const StreamingSourcePathVolume &path, const ExactBounds &cell) noexcept {
            for (std::size_t index = 1; index < path.pointCount; ++index) {
                if (SegmentIntersectsExpandedCell(path.points[index - 1].Millimeters(), path.points[index].Millimeters(), cell,
                                                  path.halfExtentMillimeters))
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsSupported(const StreamingSourceShape &shape, const StreamingSourceShapeSupport support) noexcept {
            return std::visit([support]<typename Shape>(const Shape &) {
                if constexpr (std::is_same_v<Shape, StreamingSourceSphere>)
                    return support.sphere;
                if constexpr (std::is_same_v<Shape, StreamingSourceBox>)
                    return support.box;
                if constexpr (std::is_same_v<Shape, StreamingSourceFrustum>)
                    return support.frustum;
                return support.pathVolume;
            }, shape);
        }

        [[nodiscard]] Result<void> ValidateShape(const StreamingSourceSphere &sphere) {
            ExactBounds bounds;
            if (sphere.radiusMillimeters <= 0 || sphere.radiusMillimeters > StreamingSourceRangeLimits::MaximumExtentMillimeters)
                return InvalidShape();
            if (!ExpandChecked(sphere.center.Millimeters(), sphere.radiusMillimeters, bounds))
                return InvalidShape();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateShape(const StreamingSourceBox &box) {
            if (!OrderedAndBounded({box.minimum.Millimeters(), box.maximum.Millimeters()}))
                return InvalidShape();
            return Result<void>::Success();
        }

        [[nodiscard]] bool IsValidPlane(const StreamingSourceFrustumPlane &plane, const std::int64_t maximumExtent) noexcept {
            if (const float lengthSquared = plane.inwardNormal.x * plane.inwardNormal.x + plane.inwardNormal.y * plane.inwardNormal.y +
                                            plane.inwardNormal.z * plane.inwardNormal.z;
                !std::isfinite(lengthSquared) || lengthSquared < 0.998F || lengthSquared > 1.002F)
                return false;
            return std::isfinite(plane.distanceMillimeters) && plane.distanceMillimeters >= 0.0F &&
                   plane.distanceMillimeters <= static_cast<float>(maximumExtent);
        }

        [[nodiscard]] Result<void> ValidateShape(const StreamingSourceFrustum &frustum) {
            ExactBounds bounds;
            if (frustum.maximumExtentMillimeters <= 0 ||
                frustum.maximumExtentMillimeters > StreamingSourceRangeLimits::MaximumExtentMillimeters)
                return InvalidShape();
            if (!ExpandChecked(frustum.anchor.Millimeters(), frustum.maximumExtentMillimeters, bounds))
                return InvalidShape();
            for (const StreamingSourceFrustumPlane &plane : frustum.planes) {
                if (!IsValidPlane(plane, frustum.maximumExtentMillimeters))
                    return InvalidShape();
            }
            return Result<void>::Success();
        }

        [[nodiscard]] bool IsPathPointBounded(const Coordinates &point, const Coordinates &anchor, const std::int64_t halfExtent) noexcept {
            if (ExactBounds expanded; !ExpandChecked(point, halfExtent, expanded))
                return false;
            for (std::size_t axis = 0; axis < point.size(); ++axis) {
                const std::uint64_t distance = Distance(point[axis], anchor[axis]);
                if (distance + static_cast<std::uint64_t>(halfExtent) >
                    static_cast<std::uint64_t>(StreamingSourceRangeLimits::MaximumExtentMillimeters))
                    return false;
            }
            return true;
        }

        [[nodiscard]] Result<void> ValidateShape(const StreamingSourcePathVolume &path) {
            if (path.pointCount < 2 || path.pointCount > path.points.size())
                return InvalidShape();
            if (path.halfExtentMillimeters <= 0 || path.halfExtentMillimeters > StreamingSourceRangeLimits::MaximumExtentMillimeters)
                return InvalidShape();
            const Coordinates anchor = path.points[0].Millimeters();
            for (std::size_t index = 0; index < path.pointCount; ++index) {
                if (!IsPathPointBounded(path.points[index].Millimeters(), anchor, path.halfExtentMillimeters))
                    return InvalidShape();
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateStreamingSourceShape */
    Result<void> ValidateStreamingSourceShape(const StreamingSourceShape &shape) {
        return std::visit([](const auto &value) {
            return ValidateShape(value);
        }, shape);
    }

    /** @copydoc EvaluateStreamingSourceCellRange */
    Result<bool> EvaluateStreamingSourceCellRange(const StreamingSourceDescriptor &descriptor,
                                                  const StreamingSourceAdmissionContext &admission, const StreamingSourceShape &shape,
                                                  const StreamingSourceShapeSupport support, const WorldCellQuantizationPolicy &policy,
                                                  const StreamingCellId cell) {
        if (const auto source = ValidateStreamingSourceAdmission(descriptor, admission); source.HasError())
            return Result<bool>::Failure(source.ErrorValue());
        if (const auto validShape = ValidateStreamingSourceShape(shape); validShape.HasError())
            return Result<bool>::Failure(validShape.ErrorValue());
        if (!IsSupported(shape, support))
            return Result<bool>::Failure(MakeError(WorldStreamingErrors::SourceShapeUnsupported));
        const auto bounds = CellBounds(policy, cell);
        if (bounds.HasError())
            return Result<bool>::Failure(bounds.ErrorValue());
        return Result<bool>::Success(std::visit([&bounds](const auto &value) {
            return Intersects(value, bounds.Value());
        }, shape));
    }

    namespace {
        /** @brief Applies signed inward margins to the same concrete geometry used by ordinary range evaluation. */
        [[nodiscard]] bool MarginIntersects(const StreamingSourceShape &shape, const ExactBounds &cell, const std::int64_t inward) {
            return std::visit([&]<typename Shape>(const Shape &original) {
                auto value = original;
                if constexpr (std::is_same_v<Shape, StreamingSourceSphere>) {
                    value.radiusMillimeters -= inward;
                    if (value.radiusMillimeters < 0)
                        return false;
                } else if constexpr (std::is_same_v<Shape, StreamingSourceBox>) {
                    auto minimum = value.minimum.Millimeters();
                    auto maximum = value.maximum.Millimeters();
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        minimum[axis] = AddSaturated(minimum[axis], inward);
                        maximum[axis] = AddSaturated(maximum[axis], -inward);
                        if (minimum[axis] > maximum[axis])
                            return false;
                    }
                    value.minimum = Math::WorldCoordinate64::FromMillimeters(minimum[0], minimum[1], minimum[2]);
                    value.maximum = Math::WorldCoordinate64::FromMillimeters(maximum[0], maximum[1], maximum[2]);
                } else if constexpr (std::is_same_v<Shape, StreamingSourceFrustum>) {
                    value.maximumExtentMillimeters -= inward;
                    if (value.maximumExtentMillimeters < 0)
                        return false;
                    for (auto &plane : value.planes)
                        plane.distanceMillimeters -= static_cast<float>(inward);
                } else {
                    value.halfExtentMillimeters -= inward;
                    if (value.halfExtentMillimeters < 0)
                        return false;
                }
                return Intersects(value, cell);
            }, shape);
        }
    }  // namespace

    /** @copydoc StreamingSourceCellDistance */
    Result<std::uint64_t> StreamingSourceCellDistance(const StreamingSourceShape &shape, const WorldCellQuantizationPolicy &policy,
                                                      const StreamingCellId cell) {
        if (const auto valid = ValidateStreamingSourceShape(shape); valid.HasError())
            return Result<std::uint64_t>::Failure(valid.ErrorValue());
        const auto bounds = CellBounds(policy, cell);
        if (bounds.HasError())
            return Result<std::uint64_t>::Failure(bounds.ErrorValue());
        const auto anchor = std::visit(
                                []<typename Shape>(const Shape &value) {
            if constexpr (std::is_same_v<Shape, StreamingSourceSphere>)
                return value.center;
            else if constexpr (std::is_same_v<Shape, StreamingSourceBox>)
                return value.minimum;
            else if constexpr (std::is_same_v<Shape, StreamingSourceFrustum>)
                return value.anchor;
            else
                return value.points[0];
        }, shape)
                                .Millimeters();
        std::uint64_t distance{};
        for (std::size_t axis = 0; axis < anchor.size(); ++axis)
            distance = std::max(distance, AxisDistanceFromBounds(anchor[axis], bounds.Value().minimum[axis], bounds.Value().maximum[axis]));
        return Result<std::uint64_t>::Success(distance);
    }

    /** @copydoc StreamingSourceQueryBounds */
    Result<WorldPartitionBounds> StreamingSourceQueryBounds(const StreamingSourceShape &shape, const std::int64_t expansionMillimeters) {
        if (const auto valid = ValidateStreamingSourceShape(shape); valid.HasError())
            return Result<WorldPartitionBounds>::Failure(valid.ErrorValue());
        if (expansionMillimeters < 0 || expansionMillimeters > StreamingSourceRangeLimits::MaximumExtentMillimeters)
            return Result<WorldPartitionBounds>::Failure(MakeError(WorldStreamingErrors::SourceShapeInvalid));
        const auto bounds = std::visit([]<typename Shape>(const Shape &value) -> ExactBounds {
            if constexpr (std::is_same_v<Shape, StreamingSourceBox>) {
                return {value.minimum.Millimeters(), value.maximum.Millimeters()};
            } else if constexpr (std::is_same_v<Shape, StreamingSourcePathVolume>) {
                ExactBounds result{value.points[0].Millimeters(), value.points[0].Millimeters()};
                for (std::size_t index = 1; index < value.pointCount; ++index)
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        result.minimum[axis] = std::min(result.minimum[axis], value.points[index].Millimeters()[axis]);
                        result.maximum[axis] = std::max(result.maximum[axis], value.points[index].Millimeters()[axis]);
                    }
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    result.minimum[axis] = AddSaturated(result.minimum[axis], -value.halfExtentMillimeters);
                    result.maximum[axis] = AddSaturated(result.maximum[axis], value.halfExtentMillimeters);
                }
                return result;
            } else {
                ExactBounds result;
                if constexpr (std::is_same_v<Shape, StreamingSourceSphere>)
                    static_cast<void>(ExpandChecked(value.center.Millimeters(), value.radiusMillimeters, result));
                else
                    static_cast<void>(ExpandChecked(value.anchor.Millimeters(), value.maximumExtentMillimeters, result));
                return result;
            }
        }, shape);
        auto minimum = bounds.minimum;
        auto maximum = bounds.maximum;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            minimum[axis] = AddSaturated(minimum[axis], -expansionMillimeters);
            maximum[axis] = AddSaturated(maximum[axis], expansionMillimeters);
        }
        return Result<WorldPartitionBounds>::Success({Math::WorldCoordinate64::FromMillimeters(minimum[0], minimum[1], minimum[2]),
                                                      Math::WorldCoordinate64::FromMillimeters(maximum[0], maximum[1], maximum[2])});
    }

    /** @copydoc EvaluateStreamingSourceCellBoundary */
    Result<std::int64_t> EvaluateStreamingSourceCellBoundary(const StreamingSourceShape &shape, const WorldCellQuantizationPolicy &policy,
                                                             const StreamingCellId cell, const std::int64_t enterMarginMillimeters,
                                                             const std::int64_t exitMarginMillimeters) {
        if (enterMarginMillimeters < 0 || exitMarginMillimeters < 0 ||
            enterMarginMillimeters > StreamingSourceRangeLimits::MaximumExtentMillimeters ||
            exitMarginMillimeters > StreamingSourceRangeLimits::MaximumExtentMillimeters)
            return Result<std::int64_t>::Failure(MakeError(WorldStreamingErrors::SourceShapeInvalid));
        if (const auto valid = ValidateStreamingSourceShape(shape); valid.HasError())
            return Result<std::int64_t>::Failure(valid.ErrorValue());
        const auto bounds = CellBounds(policy, cell);
        if (bounds.HasError())
            return Result<std::int64_t>::Failure(bounds.ErrorValue());
        if (MarginIntersects(shape, bounds.Value(), enterMarginMillimeters))
            return Result<std::int64_t>::Success(enterMarginMillimeters);
        if (MarginIntersects(shape, bounds.Value(), 0))
            return Result<std::int64_t>::Success(0);
        if (MarginIntersects(shape, bounds.Value(), -exitMarginMillimeters))
            return Result<std::int64_t>::Success(-exitMarginMillimeters);
        return Result<std::int64_t>::Success(-exitMarginMillimeters - 1);
    }
}  // namespace Horo::WorldStreaming
