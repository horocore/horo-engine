#pragma once

/** @file RuntimeDispatchOrdinal.h
 * @brief Owner-private checked identity primitive used by the actual dispatch producer.
 */
#include <cstdint>
#include <limits>

namespace Horo::Runtime::Internal {
    /** @brief Reserves one never-wrapping dispatch identity; exhaustion leaves the prior value unchanged. */
    [[nodiscard]] inline bool AdvanceDispatchOrdinal(std::uint64_t &ordinal) noexcept {
        if (ordinal == std::numeric_limits<std::uint64_t>::max())
            return false;
        ++ordinal;
        return true;
    }
}  // namespace Horo::Runtime::Internal
