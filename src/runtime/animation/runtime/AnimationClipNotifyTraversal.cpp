#include "AnimationGraphInstanceInternal.h"

#include <limits>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief One actual loop seam advances identity without adding a second playback phase. */
        bool LoopOrdinal(GraphInstanceState &state, const std::uint32_t player, const bool reverse) noexcept {
            auto &ordinal = state.notifies.candidate[player].loopOrdinal;
            if ((reverse && ordinal == std::numeric_limits<std::int64_t>::min()) ||
                (!reverse && ordinal == std::numeric_limits<std::int64_t>::max())) {
                state.notifies.failure = GraphInstanceFailure::LimitExceeded;
                return false;
            }
            ordinal += reverse ? -1 : 1;
            return true;
        }

        /** @brief A loop discontinuity closes clipped windows truthfully before entering the other edge. */
        void Seam(GraphInstanceState &state, const std::uint32_t player, const AnimationTime time,
                  const AnimationTraversalDirection direction, const bool allowed, const bool reverse) noexcept {
            CloseClipNotifyPlayer(state, player, AnimationNotifyReason::Loop);
            if (!LoopOrdinal(state, player, reverse))
                return;
            ClipNotifyOrigin(state, player, time, direction, allowed, AnimationNotifyReason::Loop, true);
        }

        /** @brief Clip/section changes retire old duration ownership before reusing one canonical player. */
        void Bind(GraphInstanceState &state, const std::uint32_t player, const std::uint32_t clip, const AnimationClipRegion region,
                  const GraphActionHandle action) noexcept {
            auto &owner = state.notifies.candidate[player];
            if (owner.initialized && (owner.clip != clip || owner.region != region || owner.action != action))
                ResetClipNotifyPlayer(state, player, AnimationNotifyReason::SectionChanged);
            owner.clip = clip;
            owner.region = region;
            owner.action = action;
            owner.visited = true;
        }

        /** @brief Records each real directed segment and exact seam; no event is inferred from alignment. */
        void Segment(GraphInstanceState &state, const std::uint32_t player, const AnimationClipTraversalSegment &segment,
                     const bool allowed) noexcept {
            auto &owner = state.notifies.candidate[player];
            if (!owner.travelled) {
                const auto origin = segment.loopSeamBefore ? owner.region.begin : segment.from;
                ClipNotifyOrigin(state, player, origin, segment.direction, allowed, owner.originReason, true);
                owner.travelled = true;
            }
            if (segment.loopSeamBefore)
                Seam(state, player, owner.region.end, segment.direction, allowed, true);
            else if (segment.cycleDeltaBefore != 0)
                LoopOrdinal(state, player, true);
            owner.sample = segment.from;
            ClipNotifySegment(state, player, segment, allowed);
            owner.sample = segment.to;
            if (segment.loopSeamAfter) {
                Seam(state, player, owner.region.begin, segment.direction, allowed, false);
                owner.sample = owner.region.begin;
            } else if (segment.cycleDeltaAfter != 0)
                LoopOrdinal(state, player, false);
        }
    }  // namespace

    namespace {
        /** @brief Even a zero-travel alignment relocation revokes ownership at its true prior sample. */
        void Relocate(GraphInstanceState &state, const std::uint32_t player, const AnimationClipTrace &trace) noexcept {
            auto &owner = state.notifies.candidate[player];
            if (!owner.initialized)
                return;
            const auto origin = trace.segments == 0 ? trace.traversal.sampleTime : state.notifies.trace[0].from;
            const bool loopSeam = trace.segments != 0 && state.notifies.trace[0].loopSeamBefore;
            if (!loopSeam && owner.sample != origin)
                CloseClipNotifyPlayer(state, player, AnimationNotifyReason::Reentered);
        }

        /** @brief Prepares truthful discontinuity closure before recording an already validated path. */
        void StageTrace(GraphInstanceState &state, const std::uint32_t player, const AnimationClipTrace &trace, const bool allowed,
                        const GraphActionHandle action) noexcept {
            auto &work = state.notifies;
            auto &owner = work.candidate[player];
            Relocate(state, player, trace);
            owner.initialized = true;
            if (trace.segments == 0) {
                if (!allowed)
                    CloseClipNotifyPlayer(state, player, AnimationNotifyReason::Suppressed);
                owner.sample = trace.traversal.sampleTime;
            }
            for (const auto &segment : std::span{work.trace}.first(trace.segments))
                Segment(state, player, segment, allowed);
            if (trace.traversal.cursor.terminal && !action.IsValid())
                CloseClipNotifyPlayer(state, player, AnimationNotifyReason::PlaybackEnd);
        }

        /** @brief Closed transition policy uses only the canonical state-machine blend outcome. */
        bool TransitionAllows(const GraphTransitionEventPolicy policy, const GraphMachineState &machine, const bool source) noexcept {
            switch (policy) {
                case GraphTransitionEventPolicy::SourceOnly:
                    return source;
                case GraphTransitionEventPolicy::DestinationOnly:
                    return !source;
                case GraphTransitionEventPolicy::HighestContribution:
                    return source == (machine.outcome.weight <= 0.5F);
                case GraphTransitionEventPolicy::SuppressWhileBlending:
                    return false;
                default:
                    return true;
            }
        }
    }  // namespace

    /** @copydoc StageClipNotifies */
    GraphInstanceOutcome StageClipNotifies(GraphInstanceState &state, const std::uint32_t player, const std::uint32_t clip,
                                           const AnimationClipCursor before, const AnimationTimeDelta delta,
                                           const AnimationClipRegion region, const bool allowed, const GraphActionHandle action) noexcept {
        auto &work = state.notifies;
        if (work.candidate.empty())
            return {};
        Bind(state, player, clip, region, action);
        const auto traced = state.clips[clip]->TraceRegionPrepared(before, delta, region, work.trace, state.clipLimits);
        if (traced.HasError())
            return {GraphInstanceFailure::ClipFailure, traced.ErrorValue()};
        StageTrace(state, player, traced.Value(), allowed, action);
        return {work.failure};
    }

    /** @copydoc ClipNotifyEligible */
    bool ClipNotifyEligible(const GraphInstanceState &state, const GraphInstructionOccurrence &occurrence) noexcept {
        if (state.notifies.candidate.empty())
            return false;
        const auto &settings = state.notifies.settings;
        double contribution = 1.0;
        for (const auto &guard : occurrence.guards) {
            const auto &machine = state.candidate.machines[guard.machine];
            if (!machine.runtime.destination)
                continue;
            const bool source = guard.lane == machine.sourceLane;
            if (!TransitionAllows(settings.transitions, machine, source))
                return false;
            contribution *= source ? 1.0 - machine.outcome.weight : machine.outcome.weight;
        }
        return contribution > settings.minimumStateContribution;
    }
}  // namespace Horo::Animation::Detail
