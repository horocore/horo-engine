#include "AnimationGraphSyncMapping.h"

#include <algorithm>

namespace Horo::Animation::Detail {
    /** @copydoc ScaleGraphSyncTime */
    std::int64_t ScaleGraphSyncTime(const std::int64_t value, const std::int64_t from, const std::int64_t to) noexcept {
        std::uint64_t quotient = 0;
        std::uint64_t remainder = 0;
        for (int bit = 63; bit >= 0; --bit) {
            remainder = 2 * remainder + (((static_cast<std::uint64_t>(value) >> bit) & 1U) * static_cast<std::uint64_t>(to));
            quotient = 2 * quotient + remainder / static_cast<std::uint64_t>(from);
            remainder %= static_cast<std::uint64_t>(from);
        }
        return static_cast<std::int64_t>(quotient);
    }

    namespace {
        /** @brief Requires the same ordered marker semantics; time positions may differ between clip revisions. */
        bool Compatible(const AnimationClipData &leader, const AnimationClipData &follower) noexcept {
            if (leader.syncMarkers.size() < 2 || leader.syncMarkers.size() != follower.syncMarkers.size())
                return false;
            for (std::size_t i = 0; i < leader.syncMarkers.size(); ++i)
                if (leader.syncMarkers[i].id != follower.syncMarkers[i].id)
                    return false;
            return true;
        }

        /** @brief Corresponding cyclic interval with one consistent unwrapped origin for old/new phases. */
        std::array<std::int64_t, 2> Segment(const AnimationClipData &clip, const std::size_t next) noexcept {
            const auto &markers = clip.syncMarkers;
            if (next == 0)
                return {markers.back().time.ticks - clip.descriptor.duration.ticks, markers.front().time.ticks};
            if (next == markers.size())
                return {markers.back().time.ticks, markers.front().time.ticks + clip.descriptor.duration.ticks};
            return {markers[next - 1].time.ticks, markers[next].time.ticks};
        }
    }  // namespace

    /** @copydoc MapGraphSyncPhase */
    std::int64_t MapGraphSyncPhase(const AnimationClipData &leader, const AnimationClipData &follower, const std::int64_t phase,
                                   const bool markers, bool &fallback) noexcept {
        fallback = markers && !Compatible(leader, follower);
        if (!markers || fallback)
            return ScaleGraphSyncTime(phase, leader.descriptor.duration.ticks, follower.descriptor.duration.ticks);
        const auto next = std::ranges::upper_bound(leader.syncMarkers, AnimationTime{phase}, {}, &AnimationSyncMarker::time);
        const auto index = static_cast<std::size_t>(next - leader.syncMarkers.begin());
        const auto source = Segment(leader, index);
        const auto destination = Segment(follower, index);
        return destination[0] + ScaleGraphSyncTime(phase - source[0], source[1] - source[0], destination[1] - destination[0]);
    }
}  // namespace Horo::Animation::Detail
