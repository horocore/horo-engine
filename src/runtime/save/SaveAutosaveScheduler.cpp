#include "Horo/Runtime/Save/SaveAutosaveScheduler.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Runtime {
    namespace {
        /** @brief Validates the closed policy domain and checks addition before using Duration arithmetic. */
        [[nodiscard]] bool ValidPolicy(const SaveAutosavePolicy &policy) noexcept {
            const auto interval = policy.interval.ToNanoseconds();
            const auto jitter = policy.jitter.ToNanoseconds();
            const auto known = [](const SaveAutosaveClockPolicy value) {
                return value == SaveAutosaveClockPolicy::Freeze || value == SaveAutosaveClockPolicy::Accumulate;
            };
            return interval > 0 && jitter >= 0 && policy.cooldown >= Duration{} &&
                   jitter <= std::numeric_limits<std::int64_t>::max() - interval &&
                   (policy.domain == SaveAutosaveTimeDomain::Gameplay || policy.domain == SaveAutosaveTimeDomain::MonotonicRealTime) &&
                   known(policy.paused) && known(policy.loading) && known(policy.inactive);
        }

        /** @brief Clocks use exact finite integers; floating-point and nonfinite inputs have no conversion seam. */
        [[nodiscard]] bool ValidSample(const SaveAutosaveClockSample &sample) noexcept {
            return sample.generation.IsValid() && sample.gameplay >= Duration{} && sample.monotonic >= Duration{} &&
                   sample.activity >= SaveAutosaveActivity::Active && sample.activity <= SaveAutosaveActivity::Inactive;
        }

        /** @brief Chooses whether the just-closed activity interval advances the selected clock. */
        [[nodiscard]] bool Accumulates(const SaveAutosavePolicy &policy, const SaveAutosaveActivity activity) noexcept {
            using enum SaveAutosaveActivity;
            using enum SaveAutosaveClockPolicy;
            switch (activity) {
                case Active:
                    return true;
                case Paused:
                    return policy.paused == Accumulate;
                case Loading:
                    return policy.loading == Accumulate;
                case Inactive:
                    return policy.inactive == Accumulate;
            }
            return false;
        }

        /** @brief Adds diagnostic counters without allowing a long-lived session to wrap. */
        [[nodiscard]] std::uint64_t SaturatingCount(const std::uint64_t value, const std::uint64_t delta) noexcept {
            return value + std::min(delta, std::numeric_limits<std::uint64_t>::max() - value);
        }

        /** @brief Adds nonnegative age diagnostics without overflowing signed Duration. */
        [[nodiscard]] Duration SaturatingAge(const Duration age, const std::int64_t delta) noexcept {
            return Duration::FromNanoseconds(age.ToNanoseconds() +
                                             std::min(delta, std::numeric_limits<std::int64_t>::max() - age.ToNanoseconds()));
        }
    }  // namespace

    /** @copydoc SaveAutosaveScheduler::Create */
    Result<std::unique_ptr<SaveAutosaveScheduler>> SaveAutosaveScheduler::Create(const SaveAutosavePolicy &policy,
                                                                                 const SaveAutosaveClockSample &initial,
                                                                                 SaveOperationArbiter &arbiter,
                                                                                 SaveCaptureBarrier &barrier) {
        if (!ValidPolicy(policy) || !ValidSample(initial))
            return Result<std::unique_ptr<SaveAutosaveScheduler>>::Failure(MakeError(SaveErrors::PolicyInvalid));
        if (const auto owner = barrier.Snapshot(); owner.HasError())
            return Result<std::unique_ptr<SaveAutosaveScheduler>>::Failure(owner.ErrorValue());
        try {
            return Result<std::unique_ptr<SaveAutosaveScheduler>>::Success(
                std::make_unique<SaveAutosaveScheduler>(policy, initial, arbiter, barrier, ConstructionKey{}));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<SaveAutosaveScheduler>>::Failure(MakeError(SaveErrors::OperationAllocationFailed));
        }
    }

    /** @copydoc SaveAutosaveScheduler::SaveAutosaveScheduler */
    SaveAutosaveScheduler::SaveAutosaveScheduler(const SaveAutosavePolicy &policy, const SaveAutosaveClockSample &initial,
                                                 SaveOperationArbiter &arbiter, SaveCaptureBarrier &barrier, ConstructionKey) noexcept
        : policy_(policy), arbiter_(&arbiter), barrier_(&barrier), owner_(std::this_thread::get_id()) {
        Reset(initial);
    }

    /** @copydoc SaveAutosaveScheduler::Reset */
    void SaveAutosaveScheduler::Reset(const SaveAutosaveClockSample &initial) noexcept {
        last_ = initial;
        const auto width = static_cast<std::uint64_t>(policy_.jitter.ToNanoseconds()) + 1;
        const auto offset = static_cast<std::int64_t>(policy_.jitterSeed % width);
        snapshot_ = {.generation = initial.generation,
                     .untilNextTrigger = Duration::FromNanoseconds(policy_.interval.ToNanoseconds() + offset)};
        operation_ = {};
        awaitingCapture_ = false;
    }

    /** @copydoc SaveAutosaveScheduler::ValidateOwner */
    Result<void> SaveAutosaveScheduler::ValidateOwner() const {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(SaveErrors::ThreadAffinityViolation));
        return Result<void>::Success();
    }

    /** @copydoc SaveAutosaveScheduler::ValidateMutation */
    Result<void> SaveAutosaveScheduler::ValidateMutation() const {
        if (const auto owner = ValidateOwner(); owner.HasError())
            return owner;
        if (executing_)
            return Result<void>::Failure(MakeError(SaveErrors::LifecycleReentrant));
        if (closed_)
            return Result<void>::Failure(MakeError(SaveErrors::LifecycleUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc SaveAutosaveScheduler::ValidateSafePoint */
    Result<void> SaveAutosaveScheduler::ValidateSafePoint(const RuntimePhase phase, const SaveRuntimeGeneration generation) const {
        if (const auto valid = ValidateMutation(); valid.HasError())
            return valid;
        if (phase != RuntimePhase::CommitDeferredLifecycleChanges)
            return Result<void>::Failure(MakeError(SaveErrors::SafePointInvalid));
        if (generation != last_.generation)
            return Result<void>::Failure(MakeError(SaveErrors::GenerationStale));
        return Result<void>::Success();
    }

    /** @copydoc SaveAutosaveScheduler::AdvanceTime */
    void SaveAutosaveScheduler::AdvanceTime(const std::int64_t delta) noexcept {
        snapshot_.cooldownRemaining =
            Duration::FromNanoseconds(std::max<std::int64_t>(0, snapshot_.cooldownRemaining.ToNanoseconds() - delta));
        if (snapshot_.pending)
            snapshot_.pendingAge = SaturatingAge(snapshot_.pendingAge, delta);
        const auto remaining = snapshot_.untilNextTrigger.ToNanoseconds();
        if (delta < remaining) {
            snapshot_.untilNextTrigger = Duration::FromNanoseconds(remaining - delta);
            return;
        }
        const auto interval = policy_.interval.ToNanoseconds();
        const auto afterFirst = delta - remaining;
        const auto due = 1 + static_cast<std::uint64_t>(afterFirst / interval);
        snapshot_.untilNextTrigger = Duration::FromNanoseconds(interval - afterFirst % interval);
        snapshot_.triggers = SaturatingCount(snapshot_.triggers, due);
        snapshot_.coalescedTriggers = SaturatingCount(snapshot_.coalescedTriggers, due - (snapshot_.pending ? 0 : 1));
        if (!snapshot_.pending)
            snapshot_.pendingAge = Duration::FromNanoseconds(afterFirst);
        snapshot_.pending = true;
        if (!operation_.IsValid() && !snapshot_.blocked)
            snapshot_.disposition = SaveAutosaveDisposition::Pending;
    }

    /** @copydoc SaveAutosaveScheduler::Sample */
    Result<void> SaveAutosaveScheduler::Sample(const SaveAutosaveClockSample &sample) {
        if (const auto valid = ValidateMutation(); valid.HasError())
            return valid;
        if (sample.generation != last_.generation)
            return Result<void>::Failure(MakeError(SaveErrors::GenerationStale));
        if (!ValidSample(sample) || sample.gameplay < last_.gameplay || sample.monotonic < last_.monotonic)
            return Result<void>::Failure(MakeError(SaveErrors::PolicyInvalid));
        const auto current = policy_.domain == SaveAutosaveTimeDomain::Gameplay ? sample.gameplay : sample.monotonic;
        const auto previous = policy_.domain == SaveAutosaveTimeDomain::Gameplay ? last_.gameplay : last_.monotonic;
        if (Accumulates(policy_, last_.activity))
            AdvanceTime(current.ToNanoseconds() - previous.ToNanoseconds());
        last_ = sample;
        return Result<void>::Success();
    }

    /** @copydoc SaveAutosaveScheduler::Snapshot */
    Result<SaveAutosaveSchedulerSnapshot> SaveAutosaveScheduler::Snapshot() const {
        if (const auto owner = ValidateOwner(); owner.HasError())
            return Result<SaveAutosaveSchedulerSnapshot>::Failure(owner.ErrorValue());
        return Result<SaveAutosaveSchedulerSnapshot>::Success(snapshot_);
    }
}  // namespace Horo::Runtime
