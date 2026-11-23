#include "Horo/Runtime/Render/DynamicResolution.h"

#include <algorithm>
#include <limits>

namespace Horo::Render {
    namespace DynamicResolutionErrors {
        namespace {
            const ErrorDomainId Domain{"render.dynamic_resolution"};
        }

        const ErrorCodeDescriptor InvalidConfiguration{Domain,
                                                       ErrorCode{"render.dynamic_resolution.invalid_configuration"},
                                                       ErrorSeverity::Error,
                                                       "Dynamic resolution limits or output extent are invalid.",
                                                       {}};
        const ErrorCodeDescriptor InvalidTiming{Domain,
                                                ErrorCode{"render.dynamic_resolution.invalid_timing"},
                                                ErrorSeverity::Error,
                                                "Dynamic resolution requires positive GPU timing from a real frame.",
                                                {}};
        const ErrorCodeDescriptor StaleTiming{Domain,
                                              ErrorCode{"render.dynamic_resolution.stale_timing"},
                                              ErrorSeverity::Error,
                                              "Dynamic resolution timing belongs to a retired generation or frame.",
                                              {}};
        const ErrorCodeDescriptor StaleChange{Domain,
                                              ErrorCode{"render.dynamic_resolution.stale_change"},
                                              ErrorSeverity::Error,
                                              "The dynamic resolution proposal is no longer pending.",
                                              {}};
        const ErrorCodeDescriptor InvalidState{Domain,
                                               ErrorCode{"render.dynamic_resolution.invalid_state"},
                                               ErrorSeverity::Error,
                                               "The dynamic resolution controller is in the wrong lifecycle phase.",
                                               {}};
        const ErrorCodeDescriptor WrongThread{Domain,
                                              ErrorCode{"render.dynamic_resolution.wrong_thread"},
                                              ErrorSeverity::Error,
                                              "Dynamic resolution belongs to a different thread.",
                                              {}};
        const ErrorCodeDescriptor GenerationExhausted{Domain,
                                                      ErrorCode{"render.dynamic_resolution.generation_exhausted"},
                                                      ErrorSeverity::Error,
                                                      "Dynamic resolution generation space is exhausted.",
                                                      {}};
    }  // namespace DynamicResolutionErrors

    namespace {
        constexpr std::uint32_t FullScale = 1000;
        constexpr auto LastGeneration = std::numeric_limits<std::uint64_t>::max();

        /** @brief Computes a nonzero per-axis size with exact ceil division and no float or overflow. */
        [[nodiscard]] FramebufferExtent ScaleExtent(FramebufferExtent target, const std::uint16_t scale) noexcept {
            const auto scaleAxis = [scale](const std::uint32_t axis) {
                return static_cast<std::uint32_t>((std::uint64_t{axis} * scale + FullScale - 1) / FullScale);
            };
            return {scaleAxis(target.width), scaleAxis(target.height)};
        }

        /** @brief Preserves monotonic counters even at their representation limit. */
        void Increment(std::uint64_t &value) noexcept {
            if (value != std::numeric_limits<std::uint64_t>::max())
                ++value;
        }

        /** @brief Classifies one piece of timing evidence against the independent hysteresis thresholds. */
        [[nodiscard]] std::optional<DynamicResolutionReason> Direction(const DynamicResolutionPolicy &policy,
                                                                       const std::chrono::nanoseconds elapsed) noexcept {
            if (elapsed > policy.lowerAbove)
                return DynamicResolutionReason::AboveBudget;
            if (elapsed < policy.raiseBelow)
                return DynamicResolutionReason::BelowBudget;
            return std::nullopt;
        }
    }  // namespace

    /** @copydoc DynamicResolutionPolicy::IsValid */
    bool DynamicResolutionPolicy::IsValid() const noexcept {
        return raiseBelow.count() > 0 && lowerAbove > raiseBelow && minimumPermille > 0 && minimumPermille <= maximumPermille &&
               maximumPermille <= FullScale && stepPermille > 0 && stepPermille <= FullScale && raiseAfter > 0 && lowerAfter > 0;
    }

    /** @brief Guards all timing and publication state before use. */
    Result<void> DynamicResolutionController::RequireActive() const {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(DynamicResolutionErrors::WrongThread));
        if (phase_ != Phase::Active)
            return Result<void>::Failure(MakeError(DynamicResolutionErrors::InvalidState));
        return Result<void>::Success();
    }

    /** @copydoc DynamicResolutionController::Configure */
    Result<void> DynamicResolutionController::Configure(const RenderViewId view, const DynamicResolutionPolicy &policy,
                                                        const FramebufferExtent targetExtent, const std::uint16_t initialPermille) {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(DynamicResolutionErrors::WrongThread));
        if (phase_ != Phase::Unconfigured)
            return Result<void>::Failure(MakeError(DynamicResolutionErrors::InvalidState));
        if (!view.IsValid() || !policy.IsValid() || !targetExtent.IsValid() || initialPermille < policy.minimumPermille ||
            initialPermille > policy.maximumPermille)
            return Result<void>::Failure(MakeError(DynamicResolutionErrors::InvalidConfiguration));
        policy_ = policy;
        state_.active = {view, targetExtent, ScaleExtent(targetExtent, initialPermille), 1, initialPermille};
        phase_ = Phase::Active;
        return Result<void>::Success();
    }

    /** @brief Finds the next step that changes a pixel, skipping quantized duplicates on small outputs. */
    std::uint16_t DynamicResolutionController::NextScale(const DynamicResolutionReason reason) const noexcept {
        const auto &current = state_.active;
        const bool lowering = reason == DynamicResolutionReason::AboveBudget;
        if (const auto limit = lowering ? policy_.minimumPermille : policy_.maximumPermille;
            ScaleExtent(current.targetExtent, limit) == current.renderExtent)
            return current.scalePermille;
        auto scale = current.scalePermille;
        const int step = lowering ? -int{policy_.stepPermille} : int{policy_.stepPermille};
        do {
            scale = static_cast<std::uint16_t>(std::clamp<int>(scale + step, policy_.minimumPermille, policy_.maximumPermille));
        } while (ScaleExtent(current.targetExtent, scale) == current.renderExtent);
        return scale;
    }

    /** @brief Tracks a consecutive run on one side of the budget after the settling window. */
    std::optional<DynamicResolutionReason> DynamicResolutionController::AccumulateTiming(const std::chrono::nanoseconds elapsed) noexcept {
        if (settling_ > 0) {
            --settling_;
            return std::nullopt;
        }
        if (const auto direction = Direction(policy_, elapsed); direction != direction_) {
            direction_ = direction;
            consecutive_ = 0;
        }
        if (!direction_)
            return std::nullopt;
        const auto threshold = *direction_ == DynamicResolutionReason::AboveBudget ? policy_.lowerAfter : policy_.raiseAfter;
        if (consecutive_ < threshold)
            ++consecutive_;
        return consecutive_ == threshold ? direction_ : std::nullopt;
    }

    /** @copydoc DynamicResolutionController::Observe */
    Result<std::optional<DynamicResolutionChange>> DynamicResolutionController::Observe(const DynamicResolutionSample &sample) {
        using Outcome = Result<std::optional<DynamicResolutionChange>>;
        if (const auto active = RequireActive(); active.HasError())
            return Outcome::Failure(active.ErrorValue());
        if (!sample.view.IsValid() || !sample.frameId || !sample.generation || sample.elapsed.count() <= 0)
            return Outcome::Failure(MakeError(DynamicResolutionErrors::InvalidTiming));
        const auto &current = state_.active;
        if (sample.view != current.view || sample.generation != current.generation ||
            (state_.lastSample && sample.frameId <= state_.lastSample->frameId))
            return Outcome::Failure(MakeError(DynamicResolutionErrors::StaleTiming));
        state_.lastSample = sample;
        if (state_.pending)
            return Outcome::Success(std::nullopt);
        const auto direction = AccumulateTiming(sample.elapsed);
        if (!direction)
            return Outcome::Success(std::nullopt);

        const auto scale = NextScale(*direction);
        if (scale == current.scalePermille)
            return Outcome::Success(std::nullopt);
        if (current.generation == LastGeneration)
            return Outcome::Failure(MakeError(DynamicResolutionErrors::GenerationExhausted));
        const auto extent = ScaleExtent(current.targetExtent, scale);
        state_.pending = DynamicResolutionChange{{current.view, current.targetExtent, extent, current.generation + 1, scale},
                                                 sample.frameId,
                                                 *direction};
        return Outcome::Success(state_.pending);
    }

    /** @brief Starts a fresh timing window while leaving cumulative diagnostics intact. */
    void DynamicResolutionController::ResetEvidence() noexcept {
        direction_.reset();
        consecutive_ = 0;
        settling_ = policy_.settlingSamples;
    }

    /** @copydoc DynamicResolutionController::Resolve */
    Result<void> DynamicResolutionController::Resolve(const DynamicResolutionChange &change, const bool published) {
        if (const auto active = RequireActive(); active.HasError())
            return active;
        if (state_.pending != change)
            return Result<void>::Failure(MakeError(DynamicResolutionErrors::StaleChange));
        if (published) {
            state_.active = change.candidate;
            state_.lastSample.reset();
            Increment(state_.publishedChanges);
        } else {
            Increment(state_.rejectedChanges);
        }
        state_.pending.reset();
        ResetEvidence();
        return Result<void>::Success();
    }

    /** @copydoc DynamicResolutionController::UpdateOutputExtent */
    Result<void> DynamicResolutionController::UpdateOutputExtent(const FramebufferExtent targetExtent) {
        if (const auto active = RequireActive(); active.HasError())
            return active;
        if (!targetExtent.IsValid())
            return Result<void>::Failure(MakeError(DynamicResolutionErrors::InvalidConfiguration));
        auto &current = state_.active;
        if (current.targetExtent == targetExtent)
            return Result<void>::Success();
        if (current.generation == LastGeneration)
            return Result<void>::Failure(MakeError(DynamicResolutionErrors::GenerationExhausted));
        current = {current.view, targetExtent, ScaleExtent(targetExtent, current.scalePermille), current.generation + 1,
                   current.scalePermille};
        state_.pending.reset();
        state_.lastSample.reset();
        ResetEvidence();
        return Result<void>::Success();
    }

    /** @copydoc DynamicResolutionController::Snapshot */
    Result<DynamicResolutionSnapshot> DynamicResolutionController::Snapshot() const {
        if (const auto active = RequireActive(); active.HasError())
            return Result<DynamicResolutionSnapshot>::Failure(active.ErrorValue());
        return Result<DynamicResolutionSnapshot>::Success(state_);
    }

    /** @copydoc DynamicResolutionController::Stop */
    Result<void> DynamicResolutionController::Stop() {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(DynamicResolutionErrors::WrongThread));
        phase_ = Phase::Stopped;
        state_.pending.reset();
        state_.lastSample.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::Render
