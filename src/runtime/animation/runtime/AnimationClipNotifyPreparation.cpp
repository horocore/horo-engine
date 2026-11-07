#include "AnimationGraphInstanceInternal.h"

#include <algorithm>
#include <cmath>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief A single immutable bound supplies a conservative fixed duration-table stride. */
        std::uint32_t MarkerCount(const GraphInstanceAssets &assets) noexcept {
            std::uint32_t count = 0;
            for (const auto &clip : assets.clips)
                if (clip)
                    count = std::max(count, static_cast<std::uint32_t>(clip->Data().events.size()));
            return count;
        }

        /** @brief Schema-only authored metadata also requires explicit event-domain admission. */
        bool HasMetadata(const GraphInstanceAssets &assets) noexcept {
            return std::ranges::any_of(assets.clips, [](const auto &clip) {
                return clip && (!clip->Data().events.empty() || !clip->Data().eventSchemas.empty());
            });
        }

        /** @brief Only actual component-domain values are admitted; subscriptions require authoritative simulation. */
        bool Domain(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context) noexcept {
            if (context.evaluationDomain > AnimationEvaluationDomain::Count)
                return false;
            if (context.evaluationDomain == AnimationEvaluationDomain::Count)
                return !HasMetadata(assets) && context.clipEvents.subscriptions.empty();
            return context.clipEvents.subscriptions.empty() ||
                   context.evaluationDomain == AnimationEvaluationDomain::AuthoritativeSimulation;
        }

        /** @brief Cold subscription types must name an actual pinned schema revision. */
        bool KnownType(const GraphInstanceAssets &assets, const AnimationEventTypeBinding type) noexcept {
            for (const auto &clip : assets.clips) {
                if (!clip)
                    continue;
                for (const auto &schema : clip->Data().eventSchemas)
                    if (schema.type == type.type && schema.revision == type.schemaRevision)
                        return true;
            }
            return false;
        }

        /** @brief All queue admission precedes any payload/workspace allocation. */
        GraphInstanceFailure Subscription(const GraphInstanceAssets &assets, const AnimationEventSubscription &value) noexcept {
            if (!value.consumer.IsValid() || value.capacity == 0 || value.capacity > AnimationNotifyHardLimits::QueuedOccurrences)
                return GraphInstanceFailure::Malformed;
            if (value.types.empty() || value.types.size() > AnimationNotifyHardLimits::TypesPerConsumer)
                return GraphInstanceFailure::LimitExceeded;
            for (std::size_t i = 0; i < value.types.size(); ++i) {
                if (!KnownType(assets, value.types[i]))
                    return GraphInstanceFailure::BindingMismatch;
                for (std::size_t j = 0; j < i; ++j)
                    if (value.types[j] == value.types[i])
                        return GraphInstanceFailure::Malformed;
            }
            return GraphInstanceFailure::None;
        }

        /** @brief Checks finite filter policy and hard bounds before size arithmetic. */
        bool Settings(const GraphClipEventSettings &settings) noexcept {
            return settings.maximumOccurrencesPerTick > 0 &&
                   settings.maximumOccurrencesPerTick <= AnimationNotifyHardLimits::OccurrencesPerTick &&
                   settings.maximumOperations <= AnimationNotifyHardLimits::Operations &&
                   settings.subscriptions.size() <= AnimationNotifyHardLimits::Consumers &&
                   settings.transitions < GraphTransitionEventPolicy::Count && std::isfinite(settings.minimumStateContribution) &&
                   settings.minimumStateContribution >= 0.0F && settings.minimumStateContribution <= 1.0F;
        }

        /** @brief Non-authoritative evaluation allocates no authoritative queue or event workspace. */
        bool Enabled(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context) noexcept {
            return context.evaluationDomain == AnimationEvaluationDomain::AuthoritativeSimulation &&
                   (MarkerCount(assets) != 0 || !context.clipEvents.subscriptions.empty());
        }
    }  // namespace

    /** @copydoc AdmitClipNotifies */
    GraphInstanceFailure AdmitClipNotifies(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context) noexcept {
        if (!Domain(assets, context) || !Settings(context.clipEvents))
            return GraphInstanceFailure::Malformed;
        std::uint64_t queued = 0;
        const auto subscriptions = context.clipEvents.subscriptions;
        for (std::size_t i = 0; i < subscriptions.size(); ++i) {
            if (const auto failure = Subscription(assets, subscriptions[i]); failure != GraphInstanceFailure::None)
                return failure;
            queued += subscriptions[i].capacity;
            for (std::size_t j = 0; j < i; ++j)
                if (subscriptions[j].consumer == subscriptions[i].consumer)
                    return GraphInstanceFailure::Malformed;
        }
        if (queued > AnimationNotifyHardLimits::QueuedOccurrences ||
            ClipNotifyOperations(assets, context) > context.clipEvents.maximumOperations)
            return GraphInstanceFailure::LimitExceeded;
        return GraphInstanceFailure::None;
    }

    /** @copydoc ClipNotifyOperations */
    std::uint64_t ClipNotifyOperations(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context) noexcept {
        if (!Enabled(assets, context))
            return 0;
        const auto segments = context.clipLimits.maximumBoundaryCrossings + 2ULL;
        const auto markers = MarkerCount(assets);
        // Includes directed scans, bounded hit sorting, state policy and exact queue matching.
        return segments * markers * assets.program->Memory().clipPlayers * 32ULL + context.clipEvents.maximumOccurrencesPerTick *
                                                                                       context.clipEvents.subscriptions.size() *
                                                                                       AnimationNotifyHardLimits::TypesPerConsumer;
    }

    /** @copydoc ClipNotifyStorageBytes */
    std::uint64_t ClipNotifyStorageBytes(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context) noexcept {
        if (!Enabled(assets, context))
            return 0;
        const auto markers = MarkerCount(assets);
        const auto players = assets.program->Memory().clipPlayers;
        auto bytes = players * (2ULL * sizeof(NotifyPlayerState) + 2ULL * markers * sizeof(std::uint64_t));
        bytes += 3ULL * context.clipEvents.maximumOccurrencesPerTick * sizeof(AnimationEventOccurrence);
        bytes += (context.clipLimits.maximumBoundaryCrossings + 2ULL) * sizeof(AnimationClipTraversalSegment);
        bytes += 3ULL * markers * sizeof(NotifyHit);
        for (const auto &subscription : context.clipEvents.subscriptions)
            bytes += sizeof(NotifyConsumerQueue) + subscription.capacity * static_cast<std::uint64_t>(sizeof(AnimationEventOccurrence)) +
                     subscription.types.size() * sizeof(AnimationEventTypeBinding);
        return bytes;
    }

    /** @copydoc PrepareClipNotifies */
    GraphInstanceFailure PrepareClipNotifies(GraphInstanceState &state, const GraphInstanceCreateContext &context) {
        GraphInstanceAssets assets{.program = state.program, .clips = state.clips};
        if (!Enabled(assets, context))
            return GraphInstanceFailure::None;
        auto &work = state.notifies;
        work.settings = context.clipEvents;
        work.settings.subscriptions = {};
        work.markersPerPlayer = MarkerCount(assets);
        work.players.resize(state.players.size());
        work.candidate.resize(state.players.size());
        work.windows.resize(state.players.size() * work.markersPerPlayer);
        work.candidateWindows.resize(work.windows.size());
        work.occurrences.resize(context.clipEvents.maximumOccurrencesPerTick);
        for (auto &publication : work.published)
            publication.resize(work.occurrences.size());
        work.trace.resize(context.clipLimits.maximumBoundaryCrossings + 2ULL);
        work.hits.resize(3ULL * work.markersPerPlayer);
        work.consumers.reserve(context.clipEvents.subscriptions.size());
        for (const auto &subscription : context.clipEvents.subscriptions) {
            NotifyConsumerQueue queue{.consumer = subscription.consumer};
            queue.types.assign(subscription.types.begin(), subscription.types.end());
            queue.values.resize(subscription.capacity);
            work.consumers.push_back(std::move(queue));
        }
        return GraphInstanceFailure::None;
    }
}  // namespace Horo::Animation::Detail
