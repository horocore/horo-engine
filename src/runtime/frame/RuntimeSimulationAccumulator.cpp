#include "../lifecycle/RuntimeErrors.h"
#include "CheckedRationalDuration.h"
#include "Horo/Runtime/RuntimeSimulationTiming.h"
#include "internal/RuntimeSimulationTimingStorage.h"

#include <limits>

namespace Horo::Runtime {
    /** @copydoc RuntimeSimulationControl::AddAccumulatorTime */
    Result<void> RuntimeSimulationControl::AddAccumulatorTime(const Duration delta, Duration &accumulator) {
        auto *state = storage_.Mutable();
        if (!state || state->closed)
            return Result<void>::Failure(MakeError(RuntimeErrors::SimulationTimingClosed));
        if (state->owner != std::this_thread::get_id() || delta.ToNanoseconds() < 0 || accumulator.ToNanoseconds() < 0)
            return Result<void>::Failure(MakeError(RuntimeErrors::SimulationTimingInvalid));
        if (state->active.paused)
            return Result<void>::Success();
        const auto candidate =
            Foundation::TimeInternal::Scale(delta.ToNanoseconds(), state->active.rate.numerator, state->active.rate.denominator,
                                            {state->active.remainder.numerator, state->active.remainder.denominator});
        if (candidate.error != Foundation::TimeInternal::ArithmeticError::None ||
            candidate.nanoseconds > std::numeric_limits<std::int64_t>::max() - accumulator.ToNanoseconds())
            return Result<void>::Failure(MakeError(RuntimeErrors::SimulationTimingOverflow));
        accumulator = Duration::FromNanoseconds(accumulator.ToNanoseconds() + candidate.nanoseconds);
        state->active.remainder = {candidate.remainder.numerator, candidate.remainder.denominator};
        state->desired.remainder = state->active.remainder;
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
