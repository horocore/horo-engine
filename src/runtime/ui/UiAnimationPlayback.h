#pragma once

/** @file UiAnimationPlayback.h
 * @brief Owner-private checked playback candidates; evaluation does not publish clocks, values or terminal events.
 */
#include "Horo/Runtime/Ui/UiAnimationTimeline.h"
#include "UiAnimationTimeArithmetic.h"

#include <limits>

namespace Horo::Runtime::Ui::AnimationInternal {
    /** @brief Instance-owned cursor copied into inactive bounded storage before aggregate evaluation. */
    struct PlaybackCursor final {
        UiAnimationPlaybackSample sample;
        UiTimeRemainder remainder;
    };

    /** @brief Rejects unknown enumerations rather than assigning implicit fallback behavior. */
    [[nodiscard]] inline bool ValidPlaybackEnums(const UiAnimationTimePolicy &policy) noexcept {
        return policy.domain < UiTimeDomain::Count && policy.direction <= UiPlaybackDirection::AlternatingReverse &&
               policy.loop.kind <= UiLoopKind::Infinite && policy.fill <= UiAnimationFill::Both &&
               policy.zeroRate <= UiZeroRatePolicy::Hold && policy.lifecycle <= UiAnimationLifecycle::RequiredExit;
    }

    /** @brief Checks finite total time before any cursor allocation/admission. */
    [[nodiscard]] inline Result<UiDuration> TotalPlaybackTime(const UiAnimationTimePolicy &policy) {
        constexpr auto Maximum = std::numeric_limits<std::int64_t>::max();
        if (policy.delay.nanoseconds < 0 || policy.duration.nanoseconds < 0 || policy.loop.iterations == 0)
            return Result<UiDuration>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        if (policy.duration.nanoseconds != 0 &&
            policy.loop.iterations > static_cast<std::uint64_t>((Maximum - policy.delay.nanoseconds) / policy.duration.nanoseconds))
            return Result<UiDuration>::Failure(MakeError(UiErrors::ClockOverflow));
        return Result<UiDuration>::Success({policy.delay.nanoseconds + policy.duration.nanoseconds * policy.loop.iterations});
    }

    /** @brief Validates inert authored policy; exact required-route admission is checked separately by its real owner. */
    [[nodiscard]] inline Result<void> ValidatePlaybackPolicy(const UiAnimationTimePolicy &policy) {
        if (!ValidPlaybackEnums(policy) || policy.delay.nanoseconds < 0 || policy.duration.nanoseconds < 0 || !policy.rate.IsValid() ||
            (policy.rate.numerator == 0 && policy.zeroRate != UiZeroRatePolicy::Hold))
            return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        if (policy.loop.kind == UiLoopKind::Infinite) {
            if (policy.duration.nanoseconds == 0 || policy.lifecycle != UiAnimationLifecycle::NonBlocking ||
                policy.domain == UiTimeDomain::Simulation)
                return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            return Result<void>::Success();
        }
        if (policy.loop.iterations == 0 || (policy.duration.nanoseconds == 0 && policy.loop.iterations != 1))
            return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        auto total = TotalPlaybackTime(policy);
        if (total.HasError())
            return Result<void>::Failure(total.ErrorValue());
        return Result<void>::Success();
    }

    /** @brief Computes floor(numerator*UINT32_MAX/denominator) in exactly32 bounded steps without overflow or float rounding. */
    [[nodiscard]] inline std::uint32_t ClosedProgress(const std::uint64_t numerator, const std::uint64_t denominator) noexcept {
        constexpr auto Maximum = std::numeric_limits<std::uint32_t>::max();
        if (numerator == 0)
            return 0;
        if (numerator >= denominator)
            return Maximum;
        auto remainder = numerator;
        std::uint64_t quotient = 0;
        for (std::uint32_t bit = 0; bit < 32; ++bit) {
            remainder *= 2;
            quotient *= 2;
            if (remainder >= denominator) {
                remainder -= denominator;
                ++quotient;
            }
        }
        if (remainder < numerator)
            --quotient;
        return static_cast<std::uint32_t>(quotient);
    }

    /** @brief Alternation changes local progress, never the forward domain sample. */
    [[nodiscard]] inline bool ReverseIteration(const UiPlaybackDirection direction, const std::uint64_t iteration) noexcept {
        const bool initiallyReverse = direction == UiPlaybackDirection::Reverse || direction == UiPlaybackDirection::AlternatingReverse;
        const bool alternating = direction == UiPlaybackDirection::Alternating || direction == UiPlaybackDirection::AlternatingReverse;
        return initiallyReverse != (alternating && iteration % 2 != 0);
    }

    /** @brief Applies one exact endpoint or interior sample after delay/iteration admission. */
    inline void SampleProgress(UiAnimationPlaybackSample &sample, const UiAnimationTimePolicy &policy, const bool completed) noexcept {
        const auto active = sample.elapsed.nanoseconds - policy.delay.nanoseconds;
        if (completed) {
            sample.iteration = policy.loop.iterations - 1;
            sample.progress = std::numeric_limits<std::uint32_t>::max();
        } else {
            sample.iteration = static_cast<std::uint64_t>(active / policy.duration.nanoseconds);
            sample.progress = ClosedProgress(static_cast<std::uint64_t>(active % policy.duration.nanoseconds),
                                             static_cast<std::uint64_t>(policy.duration.nanoseconds));
        }
        if (ReverseIteration(policy.direction, sample.iteration))
            sample.progress = std::numeric_limits<std::uint32_t>::max() - sample.progress;
    }

    /** @brief Counts required iteration crossings algebraically; exceeding the caller's bounded work budget rejects the candidate. */
    [[nodiscard]] inline Result<std::uint32_t> CountCrossings(const UiAnimationTimePolicy &policy, const UiDuration prior,
                                                              const UiDuration next, const std::uint32_t budget) {
        if (next.nanoseconds < policy.delay.nanoseconds)
            return Result<std::uint32_t>::Success(0);
        const auto oldActive = prior.nanoseconds <= policy.delay.nanoseconds ? 0 : prior.nanoseconds - policy.delay.nanoseconds;
        const auto active = next.nanoseconds - policy.delay.nanoseconds;
        const auto crossings =
            policy.duration.nanoseconds == 0
                ? 1
                : static_cast<std::uint64_t>(active / policy.duration.nanoseconds - oldActive / policy.duration.nanoseconds);
        if (crossings > budget)
            return Result<std::uint32_t>::Failure(MakeError(UiErrors::AnimationBudgetExceeded));
        return Result<std::uint32_t>::Success(static_cast<std::uint32_t>(crossings));
    }

    /** @brief Delay samples respect backward fill and local direction without creating a terminal outcome. */
    inline void SampleWaiting(UiAnimationPlaybackSample &sample, const UiAnimationTimePolicy &policy) noexcept {
        sample.state = UiAnimationState::Waiting;
        sample.contributesValue = policy.fill == UiAnimationFill::Backwards || policy.fill == UiAnimationFill::Both;
        sample.progress = ReverseIteration(policy.direction, 0) ? std::numeric_limits<std::uint32_t>::max() : 0;
    }

    /** @brief Final sampling emits one candidate terminal outcome; only successful aggregate commit may record its publication. */
    inline void SampleActive(UiAnimationPlaybackSample &sample, const UiAnimationTimePolicy &policy, const bool completed) noexcept {
        sample.state =
            completed ? UiAnimationState::Completed : (policy.rate.numerator == 0 ? UiAnimationState::Held : UiAnimationState::Running);
        sample.outcome = completed ? UiAnimationOutcome::Completed : UiAnimationOutcome::None;
        sample.newTerminalOutcome = completed;
        sample.contributesValue = !completed || policy.fill == UiAnimationFill::Forwards || policy.fill == UiAnimationFill::Both;
        SampleProgress(sample, policy, completed);
    }

    /** @brief Evaluates an inactive cursor without mutating current state; every arithmetic/work error retains the prior cursor. */
    [[nodiscard]] inline Result<PlaybackCursor> AdvancePlayback(const PlaybackCursor &current, const UiAnimationTimePolicy &policy,
                                                                const UiDuration domainDelta, const std::uint32_t crossingBudget) {
        auto admitted = ValidatePlaybackPolicy(policy);
        if (admitted.HasError())
            return Result<PlaybackCursor>::Failure(admitted.ErrorValue());
        auto candidate = current;
        candidate.sample.newTerminalOutcome = false;
        candidate.sample.crossedIterations = 0;
        if (current.sample.outcome != UiAnimationOutcome::None)
            return Result<PlaybackCursor>::Success(candidate);
        auto scaled = ScaleDuration(domainDelta, policy.rate, current.remainder);
        if (scaled.HasError())
            return Result<PlaybackCursor>::Failure(scaled.ErrorValue());
        auto elapsed = AdvanceElapsed(current.sample.elapsed, scaled.Value().duration);
        if (elapsed.HasError())
            return Result<PlaybackCursor>::Failure(elapsed.ErrorValue());
        candidate.remainder = scaled.Value().remainder;
        candidate.sample.elapsed = elapsed.Value();
        bool completed = false;
        if (policy.loop.kind == UiLoopKind::Finite) {
            const auto total = TotalPlaybackTime(policy).Value();
            completed = candidate.sample.elapsed.nanoseconds >= total.nanoseconds;
            if (completed)
                candidate.sample.elapsed = total;
        }
        auto crossings = CountCrossings(policy, current.sample.elapsed, candidate.sample.elapsed, crossingBudget);
        if (crossings.HasError())
            return Result<PlaybackCursor>::Failure(crossings.ErrorValue());
        candidate.sample.crossedIterations = crossings.Value();
        if (candidate.sample.elapsed.nanoseconds < policy.delay.nanoseconds)
            SampleWaiting(candidate.sample, policy);
        else
            SampleActive(candidate.sample, policy, completed);
        return Result<PlaybackCursor>::Success(candidate);
    }

    /** @brief Resamples an explicit controlled-domain seek algebraically without walking iterations or replaying semantic events. */
    [[nodiscard]] inline Result<PlaybackCursor> SeekPlayback(const UiAnimationTimePolicy &policy, const UiDuration position) {
        if (auto admitted = ValidatePlaybackPolicy(policy); admitted.HasError())
            return Result<PlaybackCursor>::Failure(admitted.ErrorValue());
        auto scaled = ScaleDuration(position, policy.rate, {});
        if (scaled.HasError())
            return Result<PlaybackCursor>::Failure(scaled.ErrorValue());
        PlaybackCursor baseline;
        baseline.sample.elapsed = scaled.Value().duration;
        baseline.remainder = scaled.Value().remainder;
        bool completed = false;
        if (policy.loop.kind == UiLoopKind::Finite) {
            const auto total = TotalPlaybackTime(policy).Value();
            completed = baseline.sample.elapsed.nanoseconds >= total.nanoseconds;
            if (completed)
                baseline.sample.elapsed = total;
        }
        if (baseline.sample.elapsed.nanoseconds < policy.delay.nanoseconds)
            SampleWaiting(baseline.sample, policy);
        else
            SampleActive(baseline.sample, policy, completed);
        baseline.sample.newTerminalOutcome = false;
        return Result<PlaybackCursor>::Success(baseline);
    }

    /** @brief Proposes one cancellation outcome without invoking a callback or changing a clock/route/target owner. */
    [[nodiscard]] inline Result<PlaybackCursor> CancelPlayback(const PlaybackCursor &current, const UiAnimationCancellation reason) {
        if (reason == UiAnimationCancellation::None || reason > UiAnimationCancellation::AccessibilityReplacement)
            return Result<PlaybackCursor>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        if (current.sample.outcome == UiAnimationOutcome::Completed ||
            (current.sample.outcome == UiAnimationOutcome::Cancelled && current.sample.cancellation != reason))
            return Result<PlaybackCursor>::Failure(MakeError(UiErrors::AnimationConflict));
        auto candidate = current;
        candidate.sample.newTerminalOutcome = current.sample.outcome == UiAnimationOutcome::None;
        candidate.sample.crossedIterations = 0;
        candidate.sample.state = UiAnimationState::Cancelled;
        candidate.sample.outcome = UiAnimationOutcome::Cancelled;
        candidate.sample.cancellation = reason;
        candidate.sample.contributesValue = false;
        return Result<PlaybackCursor>::Success(candidate);
    }
}  // namespace Horo::Runtime::Ui::AnimationInternal
