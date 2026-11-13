#include "AnimationGraphInstanceInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Duration state is indexed by this exact immutable marker table. */
        std::uint64_t &Window(GraphInstanceState &state, const std::uint32_t player, const std::uint32_t marker) noexcept {
            return state.notifies.candidateWindows[static_cast<std::size_t>(player) * state.notifies.markersPerPlayer + marker];
        }

        /** @brief One immutable event is reverse eligible only by its authored policy. */
        bool Eligible(const AnimationClipEvent &event, const AnimationTraversalDirection direction, const bool allowed) noexcept {
            return allowed && (direction != AnimationTraversalDirection::Reverse || event.fireInReverse);
        }

        /** @brief Records a hit in already admitted scratch; overflow fails the complete candidate. */
        void Hit(GraphInstanceState &state, const std::uint32_t marker, const AnimationTime time, const AnimationNotifyPhase phase,
                 const AnimationNotifyReason reason = AnimationNotifyReason::Crossed) noexcept {
            auto &work = state.notifies;
            if (work.hitCount == work.hits.size()) {
                work.failure = GraphInstanceFailure::LimitExceeded;
                return;
            }
            work.hits[work.hitCount++] = {marker, time, phase, reason};
        }

        /** @brief Duration occupancy immediately in the directed interval, with no endpoint epsilon. */
        bool Inside(const AnimationClipEvent &event, const AnimationTime time, const AnimationTraversalDirection direction) noexcept {
            const auto end = event.time.ticks + event.duration->ticks;
            if (direction == AnimationTraversalDirection::Reverse)
                return time.ticks > event.time.ticks && time.ticks <= end;
            return time.ticks >= event.time.ticks && time.ticks < end;
        }

        /** @brief Ordered half-open directed crossings exclude the already visited origin. */
        bool Crossed(const AnimationTime from, const AnimationTime to, const AnimationTime at) noexcept {
            return from < to ? from < at && at <= to : to <= at && at < from;
        }

        /** @brief Forward duration edges use the same [start,end) ownership convention. */
        void ForwardWindow(GraphInstanceState &state, const std::uint32_t marker, const AnimationClipEvent &event,
                           const AnimationClipTraversalSegment &segment) noexcept {
            if (Crossed(segment.from, segment.to, event.time))
                Hit(state, marker, event.time, AnimationNotifyPhase::Begin);
            const AnimationTime end{event.time.ticks + event.duration->ticks};
            if (Crossed(segment.from, segment.to, end))
                Hit(state, marker, end, AnimationNotifyPhase::End);
        }

        /** @brief Reverse arrival at start still owns the boundary; the following departure closes it. */
        void ReverseWindow(GraphInstanceState &state, const std::uint32_t marker, const AnimationClipEvent &event,
                           const AnimationClipTraversalSegment &segment) noexcept {
            if (const AnimationTime end{event.time.ticks + event.duration->ticks}; segment.to < end && end < segment.from)
                Hit(state, marker, end, AnimationNotifyPhase::Begin);
            if (segment.to < event.time && event.time < segment.from)
                Hit(state, marker, event.time, AnimationNotifyPhase::End);
        }

        /** @brief Activation policy affects origin points; an actual loop seam is always a crossing. */
        void OriginPoint(GraphInstanceState &state, const std::uint32_t marker, const AnimationClipEvent &event, const AnimationTime origin,
                         const AnimationNotifyReason reason, const bool points) noexcept {
            if (points && (reason == AnimationNotifyReason::Loop || event.fireOnActivation) && event.time == origin)
                Hit(state, marker, origin, AnimationNotifyPhase::Point, reason);
        }

        /** @brief Reconciles one delivered duration with exact directed occupancy or explicit policy revocation. */
        void OriginWindow(GraphInstanceState &state, const std::uint32_t player, const std::uint32_t marker, const AnimationTime origin,
                          const AnimationTraversalDirection direction, const bool allowed, const AnimationNotifyReason reason) noexcept {
            const auto &event = state.clips[state.notifies.candidate[player].clip]->Data().events[marker];
            const bool eligible = Eligible(event, direction, allowed);
            const bool inside = eligible && Inside(event, origin, direction);
            if (inside && Window(state, player, marker) == 0)
                Hit(state, marker, origin, AnimationNotifyPhase::Begin, reason);
            if (!inside && Window(state, player, marker) != 0) {
                const auto closed = allowed ? AnimationNotifyReason::ReverseIneligible : AnimationNotifyReason::Suppressed;
                Hit(state, marker, origin, AnimationNotifyPhase::End, eligible ? reason : closed);
            }
        }

        /** @brief Actual action-loop travel uses a nonwrapping directed cycle identity, not a second cursor. */
        std::int64_t NextActionNotifyLoop(GraphInstanceState &state, const std::int64_t ordinal,
                                          const AnimationTraversalDirection direction) noexcept {
            const bool reverse = direction == AnimationTraversalDirection::Reverse;
            if ((reverse && ordinal == std::numeric_limits<std::int64_t>::min()) ||
                (!reverse && ordinal == std::numeric_limits<std::int64_t>::max())) {
                state.notifies.failure = GraphInstanceFailure::LimitExceeded;
                return ordinal;
            }
            return ordinal + (reverse ? -1 : 1);
        }

        /** @brief Sorts simultaneous boundaries deterministically without changing chronological traversal order. */
        void PublishHits(GraphInstanceState &state, const std::uint32_t player, const AnimationTraversalDirection direction) noexcept {
            auto &work = state.notifies;
            const auto &events = state.clips[work.candidate[player].clip]->Data().events;
            auto hits = std::span{work.hits}.first(work.hitCount);
            std::ranges::sort(hits, [&](const NotifyHit &a, const NotifyHit &b) {
                if (a.time != b.time)
                    return direction == AnimationTraversalDirection::Reverse ? a.time > b.time : a.time < b.time;
                if (a.phase != b.phase)
                    return a.phase < b.phase;
                return events[a.marker].id < events[b.marker].id;
            });
            for (const auto &hit : hits)
                EmitClipNotify(state, player, hit, direction);
        }
    }  // namespace

    /** @copydoc EmitClipNotify */
    void EmitClipNotify(GraphInstanceState &state, const std::uint32_t player, const NotifyHit &hit,
                        const AnimationTraversalDirection direction) noexcept {
        using enum Horo::Animation::AnimationNotifyPhase;
        auto &work = state.notifies;
        auto &owner = work.candidate[player];
        auto &window = Window(state, player, hit.marker);
        if (hit.phase == End && window == 0)
            return;
        if (hit.phase == Begin && window != 0)
            return;
        if (owner.serial == std::numeric_limits<std::uint64_t>::max() || work.count == work.occurrences.size()) {
            work.failure = GraphInstanceFailure::LimitExceeded;
            return;
        }
        const auto &clip = state.clips[owner.clip]->Data();
        const auto &event = clip.events[hit.marker];
        const auto serial = ++owner.serial;
        const auto begin = hit.phase == Begin ? serial : window;
        work.occurrences[work.count++] = {.id = {state.identity, work.tick, clip.descriptor.id, clip.descriptor.generation, event.id,
                                                 player, serial, owner.loopOrdinal, hit.phase, direction},
                                          .type = event.type,
                                          .schemaRevision = event.schemaRevision,
                                          .payload = event.payload,
                                          .time = hit.time,
                                          .action = owner.action,
                                          .windowBeginSerial = begin,
                                          .reason = hit.reason};
        if (hit.phase != Point)
            window = hit.phase == Begin ? serial : 0;
    }

    /** @copydoc ClipNotifyOrigin */
    void ClipNotifyOrigin(GraphInstanceState &state, const std::uint32_t player, const AnimationTime origin,
                          const AnimationTraversalDirection direction, const bool allowed, const AnimationNotifyReason reason,
                          const bool points) noexcept {
        auto &work = state.notifies;
        const auto &events = state.clips[work.candidate[player].clip]->Data().events;
        work.hitCount = 0;
        for (std::uint32_t i = 0; i < events.size(); ++i) {
            const auto &event = events[i];
            if (event.duration)
                OriginWindow(state, player, i, origin, direction, allowed, reason);
            else if (Eligible(event, direction, allowed))
                OriginPoint(state, i, event, origin, reason, points);
        }
        PublishHits(state, player, direction);
    }

    /** @copydoc ClipNotifySegment */
    void ClipNotifySegment(GraphInstanceState &state, const std::uint32_t player, const AnimationClipTraversalSegment &segment,
                           const bool allowed) noexcept {
        ClipNotifyOrigin(state, player, segment.from, segment.direction, allowed, AnimationNotifyReason::Resumed, false);
        auto &work = state.notifies;
        const auto &events = state.clips[work.candidate[player].clip]->Data().events;
        work.hitCount = 0;
        for (std::uint32_t i = 0; i < events.size(); ++i) {
            const auto &event = events[i];
            if (!Eligible(event, segment.direction, allowed))
                continue;
            if (!event.duration) {
                if (Crossed(segment.from, segment.to, event.time))
                    Hit(state, i, event.time, AnimationNotifyPhase::Point);
            } else if (segment.direction == AnimationTraversalDirection::Reverse)
                ReverseWindow(state, i, event, segment);
            else
                ForwardWindow(state, i, event, segment);
        }
        PublishHits(state, player, segment.direction);
    }

    /** @copydoc CloseClipNotifyPlayer */
    void CloseClipNotifyPlayer(GraphInstanceState &state, const std::uint32_t player, const AnimationNotifyReason reason) noexcept {
        auto &work = state.notifies;
        if (work.candidate.empty() || !work.candidate[player].initialized)
            return;
        const auto &owner = work.candidate[player];
        const auto &events = state.clips[owner.clip]->Data().events;
        for (std::uint32_t i = 0; i < events.size(); ++i)
            EmitClipNotify(state, player, {i, owner.sample, AnimationNotifyPhase::End, reason}, AnimationTraversalDirection::Held);
    }

    /** @copydoc ResetClipNotifyPlayer */
    void ResetClipNotifyPlayer(GraphInstanceState &state, const std::uint32_t player, const AnimationNotifyReason reason,
                               const AnimationTraversalDirection direction) noexcept {
        using enum Horo::Animation::AnimationNotifyReason;
        if (state.notifies.candidate.empty())
            return;
        CloseClipNotifyPlayer(state, player, reason);
        auto &owner = state.notifies.candidate[player];
        const auto serial = owner.serial;
        auto ordinal = owner.loopOrdinal;
        if (reason == Loop)
            ordinal = NextActionNotifyLoop(state, ordinal, direction);
        else if (reason != SectionChanged)
            ordinal = 0;
        owner = {};
        owner.serial = serial;
        owner.loopOrdinal = ordinal;
        owner.originReason = reason == Reentered ? Activation : reason;
    }
}  // namespace Horo::Animation::Detail
