#pragma once

/** @file PhysicsSaturatingAdd.h
 * @brief Target-private saturating counter arithmetic shared by Physics implementation units.
 */

#include <cstdint>
#include <limits>

namespace Horo::Physics {
    /** @brief Adds bounded telemetry counters without wrapping into a misleading low value. */
    [[nodiscard]] inline std::uint64_t SaturatingAdd(const std::uint64_t left, const std::uint64_t right) noexcept {
        if (right > std::numeric_limits<std::uint64_t>::max() - left)
            return std::numeric_limits<std::uint64_t>::max();
        return left + right;
    }
}  // namespace Horo::Physics
