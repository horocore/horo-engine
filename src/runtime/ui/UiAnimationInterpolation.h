#pragma once

/** @file UiAnimationInterpolation.h
 * @brief Checked interpolation of the closed continuous Runtime UI style vocabulary.
 */
#include "Horo/Runtime/Ui/UiStyle.h"

#include <cmath>
#include <limits>
#include <type_traits>

namespace Horo::Runtime::Ui::AnimationInternal {
    /** @brief Normalized closed interval; exact zero/one endpoints avoid accumulated floating cursor error. */
    struct Progress final {
        std::uint32_t fraction{};
    };

    /** @brief Interpolates signed logical units with nearest rounding, ties away from the starting value. */
    [[nodiscard]] inline std::int32_t InterpolateUnits(const std::int32_t from, const std::int32_t to, const Progress progress) noexcept {
        constexpr auto Denominator = std::numeric_limits<std::uint32_t>::max();
        const auto difference = static_cast<std::int64_t>(to) - from;
        const auto magnitude = static_cast<std::uint64_t>(difference < 0 ? -difference : difference);
        // Both factors are at most UINT32_MAX; their product fits UINT64_MAX even for INT32_MIN to INT32_MAX.
        const auto product = magnitude * progress.fraction;
        auto steps = product / Denominator;
        if (product % Denominator > Denominator / 2U)
            ++steps;
        const auto signedSteps = static_cast<std::int64_t>(steps);
        return static_cast<std::int32_t>(static_cast<std::int64_t>(from) + (difference < 0 ? -signedSteps : signedSteps));
    }

    /** @brief Samples finite scalar endpoints; zero and one return the original representation exactly. */
    [[nodiscard]] inline float InterpolateScalar(const float from, const float to, const Progress progress) noexcept {
        if (progress.fraction == 0)
            return from;
        if (progress.fraction == std::numeric_limits<std::uint32_t>::max())
            return to;
        const auto ratio = static_cast<double>(progress.fraction) / std::numeric_limits<std::uint32_t>::max();
        return static_cast<float>(std::lerp(static_cast<double>(from), static_cast<double>(to), ratio));
    }

    /** @brief Keeps semantic color role immutable while sampling unpremultiplied linear components. */
    [[nodiscard]] inline Result<UiStyleValue> InterpolateColor(const UiStyleColor &from, const UiStyleColor &to, const Progress progress) {
        if (!from.IsValid() || !to.IsValid() || from.role != to.role)
            return Result<UiStyleValue>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        return Result<UiStyleValue>::Success(
            UiStyleColor{InterpolateScalar(from.red, to.red, progress), InterpolateScalar(from.green, to.green, progress),
                         InterpolateScalar(from.blue, to.blue, progress), InterpolateScalar(from.alpha, to.alpha, progress), from.role});
    }

    /** @brief Samples every logical shape component without introducing a native geometry representation. */
    [[nodiscard]] inline Result<UiStyleValue> InterpolateShape(const UiStyleShape &from, const UiStyleShape &to, const Progress progress) {
        if (!from.IsValid() || !to.IsValid())
            return Result<UiStyleValue>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        return Result<UiStyleValue>::Success(UiStyleShape{InterpolateUnits(from.radius, to.radius, progress),
                                                          InterpolateUnits(from.borderWidth, to.borderWidth, progress),
                                                          InterpolateUnits(from.outlineWidth, to.outlineWidth, progress),
                                                          InterpolateUnits(from.shadowOffsetX, to.shadowOffsetX, progress),
                                                          InterpolateUnits(from.shadowOffsetY, to.shadowOffsetY, progress),
                                                          InterpolateUnits(from.shadowBlur, to.shadowBlur, progress)});
    }

    /** @brief Rejects discrete/category-changing tracks; samples only one closed continuous category. */
    [[nodiscard]] inline Result<UiStyleValue> InterpolateValue(const UiStyleValue &from, const UiStyleValue &to, const Progress progress) {
        if (from.index() != to.index())
            return Result<UiStyleValue>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        return std::visit([&to, progress]<typename Value>(const Value &start) -> Result<UiStyleValue> {
            const auto &end = std::get<Value>(to);
            if constexpr (std::is_same_v<Value, UiStyleColor>) {
                return InterpolateColor(start, end, progress);
            } else if constexpr (std::is_same_v<Value, UiStyleDimension>) {
                return Result<UiStyleValue>::Success(UiStyleDimension{InterpolateUnits(start.value, end.value, progress)});
            } else if constexpr (std::is_same_v<Value, UiStyleShape>) {
                return InterpolateShape(start, end, progress);
            } else if constexpr (std::is_same_v<Value, UiStyleScalar>) {
                if (!start.IsValid() || !end.IsValid())
                    return Result<UiStyleValue>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
                return Result<UiStyleValue>::Success(UiStyleScalar{InterpolateScalar(start.value, end.value, progress)});
            } else {
                return Result<UiStyleValue>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            }
        }, from);
    }
}  // namespace Horo::Runtime::Ui::AnimationInternal
