#pragma once

/** @file
 * @brief Owner-private child-clock incarnation arithmetic used by actual route admission.
 */
#include <cstdint>
#include <limits>
#include <optional>

namespace Horo::Runtime::Ui::AnimationInternal {
    /** @brief Burns one contiguous generation interval only if every child fits; failure preserves the previous high-water.
     * @param highWater Every generation issued previously, including cancelled admissions.
     * @param count Actual reserved route-stage count. @return First issued generation, or empty on exhaustion.
     */
    [[nodiscard]] inline std::optional<std::uint32_t> ReserveChildIncarnations(std::uint32_t &highWater,
                                                                               const std::uint32_t count) noexcept {
        if (count > std::numeric_limits<std::uint32_t>::max() - highWater)
            return std::nullopt;
        if (count == 0)
            return highWater;
        const auto first = highWater + 1;
        highWater += count;
        return first;
    }
}  // namespace Horo::Runtime::Ui::AnimationInternal
