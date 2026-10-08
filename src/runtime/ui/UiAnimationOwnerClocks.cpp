#include "UiAnimationOwnerInternal.h"

#include <limits>

namespace Horo::Runtime::Ui {
    /** @copydoc UiAnimationOwner::SourceBinding */
    UiAnimationHostSourceId UiAnimationOwner::SourceBinding() const noexcept {
        return storage_->binding.source;
    }

    /** @copydoc UiAnimationOwner::ClockBindings */
    UiClockSnapshot UiAnimationOwner::ClockBindings() const noexcept {
        return storage_->binding.clocks;
    }

    namespace {
        /** @brief Only explicit application-controlled domains may seek; host time can never be rewritten by a UI input. */
        [[nodiscard]] bool SeekDomain(const UiTimeDomain domain) noexcept {
            using enum UiTimeDomain;
            return domain == EditorPreview || domain == DeterministicTest || domain == Manual;
        }

        /** @brief Validates copied private-controller observations before changing any inactive clock sample. */
        [[nodiscard]] Result<void> ValidateInput(const UiClockSample &current, const UiAnimationDomainInput &input) {
            if (input.delta.nanoseconds < 0 || input.continuity > UiClockContinuity::Held ||
                (input.seek && (input.seek->nanoseconds < 0 || !SeekDomain(current.domain))))
                return Result<void>::Failure(MakeError(UiErrors::ClockInputInvalid));
            if (input.available != current.available || (!input.available && (input.delta.nanoseconds != 0 || input.seek)))
                return Result<void>::Failure(MakeError(UiErrors::ClockUnavailable));
            if ((input.continuity == UiClockContinuity::ExplicitSeek) != input.seek.has_value())
                return Result<void>::Failure(MakeError(UiErrors::ClockInputInvalid));
            if (SeekDomain(current.domain) && input.available) {
                if (input.controlledClock.ownership != current.clock.ownership || input.controlledClock.slot != current.clock.slot ||
                    input.controlledClock.generation < current.clock.generation ||
                    (!input.seek && input.controlledClock.generation != current.clock.generation) ||
                    (input.seek && input.controlledClock.generation <= current.clock.generation))
                    return Result<void>::Failure(MakeError(UiErrors::ClockSourceStale));
            } else if (input.controlledClock.IsValid()) {
                return Result<void>::Failure(MakeError(UiErrors::ClockInputInvalid));
            }
            if (input.sourceRevision < current.sourceRevision)
                return Result<void>::Failure(MakeError(UiErrors::ClockSourceStale));
            if (input.seek && (input.continuity != UiClockContinuity::ExplicitSeek || input.sourceRevision <= current.sourceRevision))
                return Result<void>::Failure(MakeError(UiErrors::ClockSourceStale));
            return Result<void>::Success();
        }

        /** @brief Checks arithmetic and actual continuity before replacing one inactive value; source cursors remain untouched. */
        [[nodiscard]] Result<void> ApplyInput(UiClockSample &sample, const UiAnimationDomainInput &input, const std::uint64_t sequence) {
            if (const auto valid = ValidateInput(sample, input); valid.HasError())
                return valid;
            const auto base = input.seek ? input.seek->nanoseconds : sample.elapsed.nanoseconds;
            if (input.delta.nanoseconds > std::numeric_limits<std::int64_t>::max() - base)
                return Result<void>::Failure(MakeError(UiErrors::ClockOverflow));
            if (const bool newIncarnation = sample.sequence != 0 && input.continuity == UiClockContinuity::BaselineReset &&
                                            input.sourceRevision != sample.sourceRevision;
                newIncarnation) {
                if (sample.clock.generation == std::numeric_limits<std::uint32_t>::max())
                    return Result<void>::Failure(MakeError(UiErrors::ClockOverflow));
                ++sample.clock.generation;
            }
            if (input.seek)
                sample.clock = input.controlledClock;
            sample.elapsed = {base + input.delta.nanoseconds};
            sample.delta = input.delta;
            sample.sequence = sequence;
            sample.sourceRevision = input.sourceRevision;
            sample.continuity = input.continuity;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiAnimationOwner::BindClocks */
    Result<void> UiAnimationOwner::BindClocks(const std::array<bool, UiTimeDomainCount> &enabled) {
        using enum UiTimeDomain;
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (storage_->stopped || storage_->draining || storage_->binding.bound || storage_->currentFrame.has_value() ||
            storage_->pendingCommands)
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (!enabled[static_cast<std::size_t>(Simulation)] || !enabled[static_cast<std::size_t>(PresentationUnscaled)] ||
            enabled[static_cast<std::size_t>(ScreenTransition)])
            return Result<void>::Failure(MakeError(UiErrors::ClockInputInvalid));
        for (std::size_t index = 0; index < UiTimeDomainCount; ++index)
            storage_->binding.clocks.domains[index].available = enabled[index];
        storage_->binding.bound = true;
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::PrepareClocks */
    Result<void> UiAnimationOwner::PrepareClocks(const Storage &storage, const UiAnimationHostRead &read, UiClockSnapshot &candidate) {
        if (!storage.binding.bound || read.Source() != storage.binding.source || read.Frame() <= storage.lastSourceFrame)
            return Result<void>::Failure(MakeError(UiErrors::ClockSourceStale));
        if (const auto &domains = read.Domains();
            domains[static_cast<std::size_t>(UiTimeDomain::Simulation)].sourceRevision != read.SimulationTick() ||
            domains[static_cast<std::size_t>(UiTimeDomain::PresentationUnscaled)].sourceRevision != read.PresentationGeneration() ||
            read.PresentationGeneration() == 0)
            return Result<void>::Failure(MakeError(UiErrors::ClockSourceStale));
        if (storage.binding.clocks.updateSequence == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(UiErrors::ClockOverflow));
        candidate = storage.binding.clocks;
        ++candidate.updateSequence;
        for (std::size_t index = 0; index < UiTimeDomainCount; ++index) {
            if (index == static_cast<std::size_t>(UiTimeDomain::ScreenTransition)) {
                if (const auto &input = read.Domains()[index];
                    input.available || input.delta.nanoseconds != 0 || input.seek || input.controlledClock.IsValid())
                    return Result<void>::Failure(MakeError(UiErrors::ClockInputInvalid));
                continue;
            }
            if (const auto applied = ApplyInput(candidate.domains[index], read.Domains()[index], candidate.updateSequence);
                applied.HasError())
                return applied;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui
