#include "UiAnimationRuntimeInternal.h"

#include <limits>

namespace Horo::Runtime {
    UiAnimationClockController::UiAnimationClockController(std::shared_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    UiAnimationClockController::~UiAnimationClockController() = default;
    UiAnimationClockController::UiAnimationClockController(UiAnimationClockController &&) noexcept = default;
    UiAnimationClockController &UiAnimationClockController::operator=(UiAnimationClockController &&) noexcept = default;

    /** @brief Qualifies the exact application-issued incarnation before reserving one bounded local command. */
    Result<std::size_t> UiAnimationClockController::Admit(Storage &storage, const Ui::UiAnimationClockId clock) {
        if (storage.ownerThread != std::this_thread::get_id() || storage.retired.load() || storage.frameReserved)
            return Result<std::size_t>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        for (std::size_t index = 0; index < storage.domains.size(); ++index) {
            const auto &domain = storage.domains[index];
            if (!domain.available || domain.clock != clock)
                continue;
            if (storage.commands == storage.capacity)
                return Result<std::size_t>::Failure(MakeError(Ui::UiErrors::AnimationBudgetExceeded));
            if (domain.revision == std::numeric_limits<std::uint64_t>::max())
                return Result<std::size_t>::Failure(MakeError(Ui::UiErrors::ClockOverflow));
            return Result<std::size_t>::Success(index);
        }
        return Result<std::size_t>::Failure(MakeError(Ui::UiErrors::ClockSourceStale));
    }

    /** @copydoc UiAnimationClockController::Clock */
    Result<Ui::UiAnimationClockId> UiAnimationClockController::Clock(const Ui::UiTimeDomain domain) const {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id() || storage_->retired.load())
            return Result<Ui::UiAnimationClockId>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        if (domain >= Ui::UiTimeDomain::Count || !storage_->domains[static_cast<std::size_t>(domain)].available)
            return Result<Ui::UiAnimationClockId>::Failure(MakeError(Ui::UiErrors::ClockUnavailable));
        return Result<Ui::UiAnimationClockId>::Success(storage_->domains[static_cast<std::size_t>(domain)].clock);
    }

    /** @copydoc UiAnimationClockController::Step */
    Result<void> UiAnimationClockController::Step(const Ui::UiAnimationClockId clock, const Ui::UiDuration duration) const {
        if (!storage_)
            return Result<void>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        const auto admitted = Admit(*storage_, clock);
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        auto &domain = storage_->domains[admitted.Value()];
        if (duration.nanoseconds < 0)
            return Result<void>::Failure(MakeError(Ui::UiErrors::ClockInputInvalid));
        if (duration.nanoseconds > std::numeric_limits<std::int64_t>::max() - domain.pending.nanoseconds)
            return Result<void>::Failure(MakeError(Ui::UiErrors::ClockOverflow));
        domain.pending.nanoseconds += duration.nanoseconds;
        ++domain.revision;
        ++storage_->commands;
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationClockController::Seek */
    Result<Ui::UiAnimationClockId> UiAnimationClockController::Seek(const Ui::UiAnimationClockId clock,
                                                                    const Ui::UiDuration position) const {
        if (!storage_)
            return Result<Ui::UiAnimationClockId>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        const auto admitted = Admit(*storage_, clock);
        if (admitted.HasError())
            return Result<Ui::UiAnimationClockId>::Failure(admitted.ErrorValue());
        auto &domain = storage_->domains[admitted.Value()];
        if (position.nanoseconds < 0)
            return Result<Ui::UiAnimationClockId>::Failure(MakeError(Ui::UiErrors::ClockInputInvalid));
        if (domain.clock.generation == std::numeric_limits<std::uint32_t>::max())
            return Result<Ui::UiAnimationClockId>::Failure(MakeError(Ui::UiErrors::ClockOverflow));
        ++domain.clock.generation;
        ++domain.revision;
        ++storage_->commands;
        domain.pending = {};
        domain.seek = position;
        domain.remainder = {};
        return Result<Ui::UiAnimationClockId>::Success(domain.clock);
    }

    /** @copydoc UiAnimationClockController::SetPreviewPlayback */
    Result<void> UiAnimationClockController::SetPreviewPlayback(const Ui::UiAnimationClockId clock, const bool playing,
                                                                const Ui::UiPlaybackRate rate) {
        if (!storage_)
            return Result<void>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        const auto admitted = Admit(*storage_, clock);
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        if (admitted.Value() != static_cast<std::size_t>(Ui::UiTimeDomain::EditorPreview) || !rate.IsValid())
            return Result<void>::Failure(MakeError(Ui::UiErrors::ClockInputInvalid));
        auto &domain = storage_->domains[admitted.Value()];
        domain.playing = playing;
        domain.rate = rate;
        ++domain.revision;
        ++storage_->commands;
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
