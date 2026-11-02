#include "AnimationGraphInstanceInternal.h"

#include <algorithm>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Matches only cold admitted exact type/revision bindings. */
        bool Matches(const NotifyConsumerQueue &queue, const AnimationEventOccurrence &event) noexcept {
            return std::ranges::any_of(queue.types, [&](const AnimationEventTypeBinding &type) {
                return type.type == event.type && type.schemaRevision == event.schemaRevision;
            });
        }

        /** @brief Computes complete queue pressure before Commit can publish any occurrence. */
        bool Fits(const NotifyWorkspace &work, const NotifyConsumerQueue &queue) noexcept {
            std::size_t required = queue.count;
            for (const auto &event : std::span{work.occurrences}.first(work.count))
                required += Matches(queue, event) ? 1 : 0;
            return required <= queue.values.size();
        }
    }  // namespace

    /** @copydoc BeginClipNotifies */
    void BeginClipNotifies(GraphInstanceState &state, const AnimationTickId tick) noexcept {
        auto &work = state.notifies;
        std::ranges::copy(work.players, work.candidate.begin());
        std::ranges::copy(work.windows, work.candidateWindows.begin());
        work.count = 0;
        work.failure = GraphInstanceFailure::None;
        work.tick = tick;
        for (auto &player : work.candidate)
            player.visited = false;
    }

    /** @copydoc CompleteClipNotifies */
    GraphInstanceOutcome CompleteClipNotifies(GraphInstanceState &state) noexcept {
        auto &work = state.notifies;
        for (std::uint32_t player = 0; player < work.candidate.size(); ++player)
            if (!work.candidate[player].visited)
                ResetClipNotifyPlayer(state, player, AnimationNotifyReason::Inactive);
        if (work.failure != GraphInstanceFailure::None)
            return {work.failure};
        for (const auto &queue : work.consumers)
            if (!Fits(work, queue))
                return {GraphInstanceFailure::LimitExceeded};
        return {};
    }

    /** @copydoc CommitClipNotifies */
    void CommitClipNotifies(GraphInstanceState &state, const std::uint32_t publication) noexcept {
        auto &work = state.notifies;
        work.publishedCounts[publication] = work.count;
        std::copy_n(work.occurrences.begin(), work.count, work.published[publication].begin());
        for (auto &queue : work.consumers)
            for (const auto &event : std::span{work.occurrences}.first(work.count))
                if (Matches(queue, event)) {
                    queue.values[(queue.head + queue.count) % queue.values.size()] = event;
                    ++queue.count;
                }
        work.players.swap(work.candidate);
        work.windows.swap(work.candidateWindows);
    }

    /** @copydoc ClipNotifyBacklog */
    bool ClipNotifyBacklog(const GraphInstanceState &state) noexcept {
        return std::ranges::any_of(state.notifies.consumers, [](const NotifyConsumerQueue &queue) {
            return queue.count != 0;
        });
    }

    /** @copydoc DrainClipNotifyQueue */
    GraphInstanceOutcome DrainClipNotifyQueue(GraphInstanceState &state, const AnimationEventConsumerId consumer,
                                              const std::span<AnimationEventOccurrence> output, std::uint32_t &written) noexcept {
        const auto found = std::ranges::find(state.notifies.consumers, consumer, &NotifyConsumerQueue::consumer);
        if (found == state.notifies.consumers.end())
            return {GraphInstanceFailure::BindingMismatch};
        const auto count = static_cast<std::uint32_t>(std::min(output.size(), static_cast<std::size_t>(found->count)));
        for (std::uint32_t i = 0; i < count; ++i)
            output[i] = found->values[(found->head + i) % found->values.size()];
        found->head = static_cast<std::uint32_t>((found->head + count) % found->values.size());
        found->count -= count;
        written = count;
        return {};
    }
}  // namespace Horo::Animation::Detail
