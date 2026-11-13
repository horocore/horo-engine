#pragma once
#include "AnimationGraphInstanceInternal.h"

namespace Horo::Animation::Detail {
    /** @brief Exact portable floor(value*to/from), using bounded 64-bit long division rather than floating phase clocks. */
    [[nodiscard]] std::int64_t ScaleGraphSyncTime(std::int64_t value, std::int64_t from, std::int64_t to) noexcept;
    /** @brief Maps a loop phase into the follower's unwrapped corresponding marker segment, or normalized fallback. */
    [[nodiscard]] std::int64_t MapGraphSyncPhase(const AnimationClipData &leader, const AnimationClipData &follower, std::int64_t phase,
                                                 bool markers, bool &fallback) noexcept;
}  // namespace Horo::Animation::Detail
