#pragma once

#include "Horo/Runtime/Ui/UiAnimationTracks.h"
#include "UiAnimationPlayback.h"

#include <algorithm>
#include <vector>

namespace Horo::Runtime::Ui::AnimationInternal {
    /** @brief Converts an exact closed normalized location to the first integral nanosecond crossing without overflow. */
    [[nodiscard]] inline std::int64_t MarkerOffset(const std::int64_t duration, const std::uint32_t position) noexcept {
        constexpr std::uint64_t Scale = std::numeric_limits<std::uint32_t>::max();
        const auto whole = static_cast<std::uint64_t>(duration) / Scale;
        const auto fraction = (static_cast<std::uint64_t>(duration) % Scale) * position;
        return static_cast<std::int64_t>(whole * position + fraction / Scale + static_cast<std::uint64_t>(fraction % Scale != 0));
    }

    /** @brief Exact immutable publication correlation shared by bounded marker helpers. */
    struct MarkerPublication final {
        UiAnimationTimelineId timeline;
        std::uint64_t sequence{};
        std::uint32_t capacity{};
    };

    /** @brief Copies one bounded interval's crossings; exact shared endpoints retain distinct authored marker/iteration identity. */
    [[nodiscard]] inline Result<void> AppendIterationMarkers(const UiAnimationDefinition &definition, const MarkerPublication &publication,
                                                             const std::uint64_t iteration, const std::int64_t prior,
                                                             const std::int64_t next, std::vector<UiAnimationMarkerCrossing> &output) {
        const bool reverse = ReverseIteration(definition.time.direction, iteration);
        for (std::size_t offset = 0; offset < definition.markers.size(); ++offset) {
            const auto &marker = definition.markers[reverse ? definition.markers.size() - offset - 1 : offset];
            const auto position = reverse ? std::numeric_limits<std::uint32_t>::max() - marker.position : marker.position;
            if (const auto threshold = MarkerOffset(definition.time.duration.nanoseconds, position); threshold <= prior || threshold > next)
                continue;
            if (output.size() == publication.capacity)
                return Result<void>::Failure(MakeError(UiErrors::AnimationBudgetExceeded));
            output.push_back({publication.timeline, marker.id, iteration, publication.sequence, reverse});
        }
        return Result<void>::Success();
    }

    /** @brief Enumerates only admitted finite work; zero-duration tracks cross all markers once on their initial publication. */
    [[nodiscard]] inline Result<void> AppendMarkers(const UiAnimationDefinition &definition, const MarkerPublication &publication,
                                                    const PlaybackCursor &prior, const PlaybackCursor &next, const bool starting,
                                                    std::vector<UiAnimationMarkerCrossing> &output) {
        if (definition.markers.empty() || next.sample.outcome == UiAnimationOutcome::Cancelled ||
            prior.sample.outcome != UiAnimationOutcome::None || next.sample.elapsed.nanoseconds < definition.time.delay.nanoseconds)
            return Result<void>::Success();
        const auto duration = definition.time.duration.nanoseconds;
        const auto oldActive = std::max<std::int64_t>(0, prior.sample.elapsed.nanoseconds - definition.time.delay.nanoseconds);
        const auto newActive = next.sample.elapsed.nanoseconds - definition.time.delay.nanoseconds;
        if (duration == 0)
            return AppendIterationMarkers(definition, publication, 0, -1, 0, output);
        const auto first = static_cast<std::uint64_t>(oldActive / duration);
        auto last = static_cast<std::uint64_t>(newActive / duration);
        if (definition.time.loop.kind == UiLoopKind::Finite)
            last = std::min(last, static_cast<std::uint64_t>(definition.time.loop.iterations - 1));
        for (auto iteration = first; iteration <= last; ++iteration) {
            const auto base = static_cast<std::int64_t>(iteration * static_cast<std::uint64_t>(duration));
            const auto lower = (starting || prior.sample.elapsed.nanoseconds < definition.time.delay.nanoseconds || oldActive < base)
                                   ? -1
                                   : std::min(duration, oldActive - base);
            const auto upper = std::min(duration, newActive - base);
            if (auto appended = AppendIterationMarkers(definition, publication, iteration, lower, upper, output); appended.HasError())
                return appended;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui::AnimationInternal
