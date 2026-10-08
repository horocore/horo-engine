#pragma once

/** @file PresentationClockGeneration.h
 * @brief Non-installed checked presentation-baseline operations shared only with owner qualification.
 */
#include "../../lifecycle/RuntimeErrors.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <limits>

namespace Horo::Runtime::Internal {
    /** @brief Advances the actual scheduler baseline, latching exhaustion without replacing its last identity. */
    inline void AdvancePresentationBaseline(std::uint64_t &generation, bool &exhausted) noexcept {
        if (exhausted || generation == std::numeric_limits<std::uint64_t>::max()) {
            exhausted = true;
            return;
        }
        ++generation;
    }

    /** @brief Gates dispatch after actual baseline exhaustion; success never changes simulation commitment. */
    [[nodiscard]] inline Result<void> AdmitPresentationBaseline(const bool exhausted) {
        if (exhausted)
            return Result<void>::Failure(MakeError(RuntimeErrors::PresentationClockGenerationExhausted));
        return Result<void>::Success();
    }

    /** @brief Admits normalized duration before dispatch; exhaustion is latched without changing prior producer evidence. */
    [[nodiscard]] inline Result<void> AdmitPresentationDuration(Duration &total, const Duration delta, bool &exhausted) {
        const auto elapsed = total.ToNanoseconds();
        const auto increment = delta.ToNanoseconds();
        if (exhausted || increment < 0 || elapsed > std::numeric_limits<std::int64_t>::max() - increment) {
            exhausted = true;
            return Result<void>::Failure(MakeError(RuntimeErrors::PresentationDurationExhausted));
        }
        total = Duration::FromNanoseconds(elapsed + increment);
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Internal
