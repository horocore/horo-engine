#pragma once

/** @file FixedAttemptEvidence.h
 * @brief Non-installed checked scheduler attempt identity shared with actual owner qualification.
 */
#include "../../lifecycle/RuntimeErrors.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <limits>

namespace Horo::Runtime::Internal {
    /** @brief Reserves one actual fixed dispatch ordinal before any callback; exhaustion preserves the last identity. */
    [[nodiscard]] inline Result<std::uint64_t> ReserveFixedAttempt(std::uint64_t &attempt) {
        if (attempt == std::numeric_limits<std::uint64_t>::max())
            return Result<std::uint64_t>::Failure(MakeError(RuntimeErrors::FixedAttemptIdentityExhausted));
        return Result<std::uint64_t>::Success(++attempt);
    }

    /** @brief Reserves a real frame ordinal before clock sampling or callbacks; failure retains the last frame. */
    [[nodiscard]] inline Result<std::uint64_t> ReservePresentationFrame(std::uint64_t &frame) {
        if (frame == std::numeric_limits<std::uint64_t>::max())
            return Result<std::uint64_t>::Failure(MakeError(RuntimeErrors::FrameIdentityExhausted));
        return Result<std::uint64_t>::Success(++frame);
    }
}  // namespace Horo::Runtime::Internal
