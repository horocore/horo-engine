#include "StreamingSourcePlannerInternal.h"
#include "WorldStreamingInternal.h"

#include <limits>
#include <type_traits>

namespace Horo::WorldStreaming::SourcePlanning {
    namespace {
        /** @brief Preserves point offsets with checked integer translation. */
        Result<Math::WorldCoordinate64> TranslatePoint(const Math::WorldCoordinate64 &point, const std::array<std::int64_t, 3> &oldAnchor,
                                                       const std::array<std::int64_t, 3> &target) {
            auto axes = point.Millimeters();
            for (std::size_t axis = 0; axis < axes.size(); ++axis) {
                std::int64_t offset{};
                const auto previous = oldAnchor[axis];
                if ((previous > 0 && axes[axis] < std::numeric_limits<std::int64_t>::min() + previous) ||
                    (previous < 0 && axes[axis] > std::numeric_limits<std::int64_t>::max() + previous))
                    return Failure<Math::WorldCoordinate64>(WorldStreamingErrors::CoordinateOutOfRange);
                offset = axes[axis] - previous;
                if (!Internal::CheckedAdd(target[axis], offset, axes[axis]))
                    return Failure<Math::WorldCoordinate64>(WorldStreamingErrors::CoordinateOutOfRange);
            }
            return Result<Math::WorldCoordinate64>::Success(Math::WorldCoordinate64::FromMillimeters(axes[0], axes[1], axes[2]));
        }
    }  // namespace

    namespace {
        /** @brief Preserves concrete source geometry while translating its validated anchor. */
        template <typename Shape> Result<StreamingSourceShape> MoveShape(const Shape &original, const Math::WorldCoordinate64 &anchor) {
            const auto oldAnchor = [&] {
                if constexpr (std::is_same_v<Shape, StreamingSourceSphere>)
                    return original.center;
                else if constexpr (std::is_same_v<Shape, StreamingSourceBox>)
                    return original.minimum;
                else if constexpr (std::is_same_v<Shape, StreamingSourceFrustum>)
                    return original.anchor;
                else
                    return original.points[0];
            }().Millimeters();
            const auto target = anchor.Millimeters();
            auto moved = original;
            if constexpr (std::is_same_v<Shape, StreamingSourceSphere>)
                moved.center = anchor;
            else if constexpr (std::is_same_v<Shape, StreamingSourceFrustum>)
                moved.anchor = anchor;
            else if constexpr (std::is_same_v<Shape, StreamingSourceBox>) {
                const auto maximum = TranslatePoint(original.maximum, oldAnchor, target);
                if (maximum.HasError())
                    return Result<StreamingSourceShape>::Failure(maximum.ErrorValue());
                moved.minimum = anchor;
                moved.maximum = maximum.Value();
            } else {
                for (auto &point : std::span{moved.points}.first(moved.pointCount)) {
                    const auto translated = TranslatePoint(point, oldAnchor, target);
                    if (translated.HasError())
                        return Result<StreamingSourceShape>::Failure(translated.ErrorValue());
                    point = translated.Value();
                }
            }
            const StreamingSourceShape result{moved};
            if (const auto valid = ValidateStreamingSourceShape(result); valid.HasError())
                return Result<StreamingSourceShape>::Failure(valid.ErrorValue());
            return Result<StreamingSourceShape>::Success(result);
        }
    }  // namespace

    /** @copydoc Reposition */
    Result<StreamingSourceShape> Reposition(const StreamingSourceShape &shape, const Math::WorldCoordinate64 &anchor) {
        return std::visit([&anchor](const auto &original) {
            return MoveShape(original, anchor);
        }, shape);
    }
}  // namespace Horo::WorldStreaming::SourcePlanning
