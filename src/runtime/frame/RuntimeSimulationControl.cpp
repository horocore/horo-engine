#include "../lifecycle/RuntimeErrors.h"
#include "Horo/Runtime/RuntimeSimulationTiming.h"
#include "internal/RuntimeSimulationTimingStorage.h"

#include <algorithm>
#include <limits>
#include <new>
#include <numeric>

namespace Horo::Runtime {
    namespace {
        constexpr auto MaximumRevision = std::numeric_limits<std::uint64_t>::max();

        /** @brief Validates actual owner-thread admission before any desired policy changes. */
        Result<void> ValidateOwner(const SimulationTimingDetail::Storage *storage) {
            if (!storage || storage->closed)
                return Result<void>::Failure(MakeError(RuntimeErrors::SimulationTimingClosed));
            if (storage->owner != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(RuntimeErrors::SimulationTimingInvalid));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc RuntimeSimulationControl::Initialize */
    Result<void> RuntimeSimulationControl::Initialize(const std::uint32_t pauseCapacity, const std::uint32_t stepCapacity) {
        if (storage_.Borrow() || pauseCapacity == 0 || stepCapacity == 0 || pauseCapacity > 4096 || stepCapacity > 4096)
            return Result<void>::Failure(MakeError(RuntimeErrors::SimulationTimingInvalid));
        try {
            storage_.PublisherPin() = std::make_shared<SimulationTimingDetail::Storage>(pauseCapacity, stepCapacity);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(RuntimeErrors::SimulationTimingCapacity));
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSimulationControl::ReadPolicy */
    Result<RuntimeSimulationPolicyRead> RuntimeSimulationControl::ReadPolicy() const {
        const auto *state = storage_.Borrow();
        if (const auto valid = ValidateOwner(state); valid.HasError())
            return Result<RuntimeSimulationPolicyRead>::Failure(valid.ErrorValue());
        return Result<RuntimeSimulationPolicyRead>::Success(RuntimeSimulationPolicyRead{storage_.ReadPin(), state->desired});
    }

    /** @copydoc RuntimeSimulationControl::Policy */
    RuntimeSimulationPolicy RuntimeSimulationControl::Policy() const noexcept {
        const auto *state = storage_.Borrow();
        return state ? state->active : RuntimeSimulationPolicy{};
    }

    /** @copydoc RuntimeSimulationControl::SetRate */
    Result<std::uint64_t> RuntimeSimulationControl::SetRate(RuntimeSimulationRate rate, const RuntimeSimulationPolicyRead &expected) {
        auto *state = storage_.Mutable();
        if (const auto valid = ValidateOwner(state); valid.HasError())
            return Result<std::uint64_t>::Failure(valid.ErrorValue());
        if (expected.owner_.get() != state || expected.policy_.commandRevision != state->desired.commandRevision)
            return Result<std::uint64_t>::Failure(MakeError(RuntimeErrors::SimulationTimingStale));
        if (rate.numerator == 0 || rate.denominator == 0)
            return Result<std::uint64_t>::Failure(MakeError(RuntimeErrors::SimulationTimingInvalid));
        const auto divisor = std::gcd(rate.numerator, rate.denominator);
        rate = {rate.numerator / divisor, rate.denominator / divisor};
        if (rate == state->desired.rate)
            return Result<std::uint64_t>::Success(state->desired.commandRevision);
        if (state->desired.commandRevision == MaximumRevision || state->desired.rateRevision == MaximumRevision)
            return Result<std::uint64_t>::Failure(MakeError(RuntimeErrors::SimulationTimingOverflow));
        state->desired.rate = rate;
        ++state->desired.rateRevision;
        return Result<std::uint64_t>::Success(++state->desired.commandRevision);
    }

    /** @copydoc RuntimeSimulationControl::AcquirePause */
    Result<RuntimeSimulationPauseLease> RuntimeSimulationControl::AcquirePause(const RuntimeSimulationPauseReason reason,
                                                                               const RuntimeSimulationPolicyRead &expected) {
        auto *state = storage_.Mutable();
        if (const auto valid = ValidateOwner(state); valid.HasError())
            return Result<RuntimeSimulationPauseLease>::Failure(valid.ErrorValue());
        if (expected.owner_.get() != state || expected.policy_.commandRevision != state->desired.commandRevision)
            return Result<RuntimeSimulationPauseLease>::Failure(MakeError(RuntimeErrors::SimulationTimingStale));
        if (reason >= RuntimeSimulationPauseReason::Count)
            return Result<RuntimeSimulationPauseLease>::Failure(MakeError(RuntimeErrors::SimulationTimingInvalid));
        if (state->desired.commandRevision == MaximumRevision || state->desired.pauseRevision == MaximumRevision)
            return Result<RuntimeSimulationPauseLease>::Failure(MakeError(RuntimeErrors::SimulationTimingOverflow));
        const auto available = std::ranges::find_if(state->pauses, [](const auto &record) {
            return !record.occupied && record.generation != MaximumRevision;
        });
        if (available == state->pauses.end())
            return Result<RuntimeSimulationPauseLease>::Failure(MakeError(RuntimeErrors::SimulationTimingCapacity));
        available->reason = reason;
        available->occupied = true;
        ++available->generation;
        available->released.store(false);
        ++state->desired.pauseCount;
        state->desired.paused = true;
        ++state->desired.pauseRevision;
        ++state->desired.commandRevision;
        const auto slot = static_cast<std::uint32_t>(available - state->pauses.begin());
        return Result<RuntimeSimulationPauseLease>::Success(
            RuntimeSimulationPauseLease{storage_.PublisherPin(), slot, available->generation});
    }

    /** @copydoc RuntimeSimulationControl::RequestStep */
    Result<RuntimeSingleStepReceipt> RuntimeSimulationControl::RequestStep(const RuntimeSimulationPolicyRead &expected) {
        auto *state = storage_.Mutable();
        if (const auto valid = ValidateOwner(state); valid.HasError())
            return Result<RuntimeSingleStepReceipt>::Failure(valid.ErrorValue());
        if (expected.owner_.get() != state || expected.policy_.commandRevision != state->desired.commandRevision)
            return Result<RuntimeSingleStepReceipt>::Failure(MakeError(RuntimeErrors::SimulationTimingStale));
        if (!state->desired.paused)
            return Result<RuntimeSingleStepReceipt>::Failure(MakeError(RuntimeErrors::SimulationTimingInvalid));
        if (state->desired.commandRevision == MaximumRevision || state->nextStepSequence == MaximumRevision)
            return Result<RuntimeSingleStepReceipt>::Failure(MakeError(RuntimeErrors::SimulationTimingOverflow));
        const auto available = std::ranges::find_if(state->steps, [](const auto &record) {
            return !record.occupied && record.generation != MaximumRevision;
        });
        if (available == state->steps.end())
            return Result<RuntimeSingleStepReceipt>::Failure(MakeError(RuntimeErrors::SimulationTimingCapacity));
        ++available->generation;
        available->pauseRevision = state->desired.pauseRevision;
        available->result = {.requestSequence = ++state->nextStepSequence};
        available->occupied = true;
        available->admitted = false;
        available->released.store(false);
        ++state->desired.pendingSteps;
        ++state->desired.commandRevision;
        const auto slot = static_cast<std::uint32_t>(available - state->steps.begin());
        return Result<RuntimeSingleStepReceipt>::Success(RuntimeSingleStepReceipt{storage_.PublisherPin(), slot, available->generation});
    }
}  // namespace Horo::Runtime
