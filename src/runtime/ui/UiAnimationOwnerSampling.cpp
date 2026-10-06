#include "UiAnimationMarkers.h"
#include "UiAnimationOwnerInternal.h"
#include "UiAnimationTrackSampling.h"

#include <algorithm>

namespace Horo::Runtime::Ui {
    /** @copydoc UiAnimationOwner::PrepareTimeline */
    Result<void> UiAnimationOwner::PrepareTimeline(Storage &storage, const std::uint32_t index) {
        const auto &current = storage.timelines[index];
        auto &timeline = storage.candidate.timelines[index];
        timeline = current;
        if (timeline.required && storage.candidate.routeCancellation != UiAnimationCancellation::None)
            timeline.cancellation = storage.candidate.routeCancellation;
        if (!timeline.occupied || (timeline.waiting && timeline.cancellation == UiAnimationCancellation::None))
            return Result<void>::Success();
        auto &frame = *storage.frames[storage.candidate.frameSlot];
        const auto &definition = storage.definition.animations[timeline.definition];
        const auto &clock = frame.clocks.domains[static_cast<std::size_t>(definition.time.domain)];
        if (timeline.required && timeline.terminalIssued) {
            auto sample = timeline.cursor.sample;
            sample.newTerminalOutcome = false;
            sample.crossedIterations = 0;
            const UiAnimationTimelineId id{storage.source.ownership,
                                           storage.range.FirstSlot() + static_cast<std::uint32_t>(UiTimeDomainCount) + 1 + index,
                                           timeline.generation};
            frame.timelines.push_back({id, definition.id, sample});
            return Result<void>::Success();
        }
        if (!clock.available && timeline.cancellation == UiAnimationCancellation::None)
            return Result<void>::Failure(MakeError(UiErrors::ClockUnavailable));
        UiDuration delta = timeline.pendingStart ? UiDuration{} : clock.delta;
        if (timeline.pendingStart)
            timeline.origin = clock.elapsed;
        auto initial = timeline.cursor;
        const bool seek = clock.continuity == UiClockContinuity::ExplicitSeek;
        if (seek) {
            initial = {};
            delta = {std::max<std::int64_t>(0, clock.elapsed.nanoseconds - timeline.origin.nanoseconds)};
        }
        auto evaluated = timeline.cancellation != UiAnimationCancellation::None
                             ? AnimationInternal::CancelPlayback(initial, timeline.cancellation)
                         : seek ? AnimationInternal::SeekPlayback(definition.time, delta)
                                : AnimationInternal::AdvancePlayback(initial, definition.time, delta, storage.candidate.remainingCrossings);
        if (evaluated.HasError())
            return Result<void>::Failure(evaluated.ErrorValue());
        if (timeline.required && !timeline.waiting && storage.route.gate &&
            storage.candidate.routeCancellation == UiAnimationCancellation::None &&
            storage.candidate.routeElapsed.nanoseconds >= storage.route.stages[storage.route.stage].maximumWait.nanoseconds &&
            evaluated.Value().sample.outcome != UiAnimationOutcome::Completed) {
            storage.candidate.routeCancellation = UiAnimationCancellation::Deadline;
            timeline.cancellation = UiAnimationCancellation::Deadline;
            evaluated = AnimationInternal::CancelPlayback(initial, timeline.cancellation);
        }
        timeline.cursor = evaluated.Value();
        storage.candidate.remainingCrossings -= timeline.cursor.sample.crossedIterations;
        timeline.cursor.sample.newTerminalOutcome = timeline.cursor.sample.newTerminalOutcome && !timeline.terminalIssued;
        timeline.terminalIssued = timeline.terminalIssued || timeline.cursor.sample.newTerminalOutcome;
        timeline.clock = clock.clock;
        const UiAnimationTimelineId id{storage.source.ownership,
                                       storage.range.FirstSlot() + static_cast<std::uint32_t>(UiTimeDomainCount) + 1 + index,
                                       timeline.generation};
        // Explicit preview/test/manual seek samples local values but cannot replay semantic marker callbacks.
        if (!seek) {
            if (auto markers =
                    AnimationInternal::AppendMarkers(definition, {id, frame.clocks.updateSequence, storage.limits.markerCrossingsPerUpdate},
                                                     current.cursor, timeline.cursor, timeline.pendingStart, frame.markers);
                markers.HasError())
                return markers;
        }
        timeline.pendingStart = false;
        frame.timelines.push_back({id, definition.id, timeline.cursor.sample});
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::PrepareTimelines */
    Result<void> UiAnimationOwner::PrepareTimelines(Storage &storage) {
        std::ranges::fill(storage.sampleCounts, 0);
        storage.candidate.remainingCrossings = storage.limits.markerCrossingsPerUpdate;
        for (std::uint32_t index = 0; index < storage.timelines.size(); ++index) {
            if (auto prepared = PrepareTimeline(storage, index); prepared.HasError())
                return prepared;
            const auto &timeline = storage.candidate.timelines[index];
            if (!timeline.occupied || timeline.waiting || !timeline.cursor.sample.contributesValue)
                continue;
            for (const auto &track : storage.definition.animations[timeline.definition].tracks) {
                const auto target = std::ranges::find(storage.definition.elements, track.target, &UiAnimationElementDefinition::element);
                ++storage.sampleCounts[static_cast<std::size_t>(target - storage.definition.elements.begin())];
            }
        }
        storage.sampleOffsets[0] = 0;
        for (std::size_t index = 0; index < storage.sampleCounts.size(); ++index)
            storage.sampleOffsets[index + 1] = storage.sampleOffsets[index] + storage.sampleCounts[index];
        std::ranges::fill(storage.sampleCounts, 0);
        for (const auto &timeline : storage.candidate.timelines) {
            if (!timeline.occupied || timeline.waiting || !timeline.cursor.sample.contributesValue)
                continue;
            for (const auto &track : storage.definition.animations[timeline.definition].tracks) {
                const auto target = std::ranges::find(storage.definition.elements, track.target, &UiAnimationElementDefinition::element);
                const auto index = static_cast<std::size_t>(target - storage.definition.elements.begin());
                const auto value = AnimationInternal::SamplePropertyTrack(track, timeline.cursor.sample.progress);
                if (value.HasError())
                    return Result<void>::Failure(value.ErrorValue());
                storage.samples[storage.sampleOffsets[index] + storage.sampleCounts[index]++] = {track.property, value.Value()};
            }
        }
        for (std::size_t index = 0; index < storage.elementInputs.size(); ++index)
            storage.elementInputs[index].animation =
                std::span(storage.samples).subspan(storage.sampleOffsets[index], storage.sampleCounts[index]);
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui
