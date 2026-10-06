#pragma once

/** @file UiAnimationTimeArithmetic.h
 * @brief Checked allocation-free integer/rational UI timeline arithmetic.
 */
#include "CheckedRationalDuration.h"
#include "Horo/Runtime/Ui/UiAnimationClock.h"

#include <limits>

namespace Horo::Runtime::Ui::AnimationInternal {
    /** @brief Complete scaled candidate; inputs are unchanged on every failure. */
    struct ScaledTime final {
        UiDuration duration;
        UiTimeRemainder remainder;
    };

    /** @brief Scales a non-negative duration, preserving exact sub-nanosecond time across rate changes. */
    [[nodiscard]] inline Result<ScaledTime> ScaleDuration(const UiDuration delta, const UiPlaybackRate rate,
                                                          const UiTimeRemainder remainder) {
        if (!rate.IsValid())
            return Result<ScaledTime>::Failure(MakeError(UiErrors::ClockInputInvalid));
        const auto result = Foundation::TimeInternal::Scale(delta.nanoseconds, rate.numerator, rate.denominator,
                                                            {remainder.numerator, remainder.denominator});
        if (result.error != Foundation::TimeInternal::ArithmeticError::None)
            return Result<ScaledTime>::Failure(MakeError(result.error == Foundation::TimeInternal::ArithmeticError::Invalid
                                                             ? UiErrors::ClockInputInvalid
                                                             : UiErrors::ClockOverflow));
        return Result<ScaledTime>::Success(
            {UiDuration{result.nanoseconds}, UiTimeRemainder{result.remainder.numerator, result.remainder.denominator}});
    }

    /** @brief Adds forward clock evidence without wrapping or changing either input on failure. */
    [[nodiscard]] inline Result<UiDuration> AdvanceElapsed(const UiDuration elapsed, const UiDuration delta) {
        if (elapsed.nanoseconds < 0 || delta.nanoseconds < 0)
            return Result<UiDuration>::Failure(MakeError(UiErrors::ClockInputInvalid));
        if (delta.nanoseconds > std::numeric_limits<std::int64_t>::max() - elapsed.nanoseconds)
            return Result<UiDuration>::Failure(MakeError(UiErrors::ClockOverflow));
        return Result<UiDuration>::Success(UiDuration{elapsed.nanoseconds + delta.nanoseconds});
    }
}  // namespace Horo::Runtime::Ui::AnimationInternal
